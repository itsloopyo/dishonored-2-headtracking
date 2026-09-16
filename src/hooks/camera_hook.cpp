// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "hooks/camera_hook.h"

#include "core/logging.h"
#include "core/mod.h"

#include <cameraunlock/camera/zoom_compensation.h>
#include <cameraunlock/hooks/hook_manager.h>
#include <cameraunlock/math/angle_utils.h>

#include <cstdio>

namespace D2HT {

namespace {

using SetupFn = void(__fastcall*)(void*, int*, int*);

// Atomic, and read on the detour's null branch as well as its live one. MinHook
// writes it inside CreateHook and its EnableHook barrier does order that write
// ahead of the first detour call, but a relaxed load costs nothing on x64 and
// the null branch stays reachable for the whole of the bootstrap.
std::atomic<SetupFn> g_original{nullptr};

// Atomic because the detour is live from the moment EnableHook returns, so the
// game's render thread reads this while the bootstrap thread is still writing
// it. The release store in Arm() is also what publishes m_profile and m_mod.
std::atomic<CameraHook*> g_hook{nullptr};

// Dense at first, because that is where a wrong offset shows, then a thin
// trickle for the rest of the session, which is what a late report is read from.
constexpr unsigned kBurstLines = 4;
constexpr unsigned kEarlyLines = 20;
constexpr unsigned kEarlyIntervalFrames = 600;
constexpr unsigned kSteadyIntervalFrames = 2000;

constexpr float kRadToDeg = static_cast<float>(cameraunlock::math::kRadToDeg);

// A full field of view is a straight angle at most, so this is the open upper
// bound on one in radians.
constexpr float kMaxFovRadians = 3.1415926f;

// How far past the requested lean the line check looks, as a multiple of the
// standoff.
//
// What a field of view read out of the game has to look like to be believed.
// Both ends are outside anything idView::SetFovXandY will accept - it refuses
// below 1 degree and above 165 - so this only ever catches a garbage read.
bool IsUsableFovDegrees(float degrees) {
    return std::isfinite(degrees) && degrees > 0.0f && degrees < 180.0f;
}

bool IsUsableFovRadians(float radians) {
    return std::isfinite(radians) && radians > 0.0f && radians < kMaxFovRadians;
}

}  // namespace

CameraHook::CameraHook()
    : m_frameLog(kBurstLines, kEarlyLines, kEarlyIntervalFrames, kSteadyIntervalFrames) {}

bool CameraHook::Install(const BuildProfile& profile, Mod& mod) {
    m_profile = &profile;
    m_mod = &mod;

    auto& hooks = cameraunlock::hooks::HookManager::Instance();
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    m_base = base;
    // .rdata, so it is read once. The cvar it converts is read every frame
    // instead, because the player can move the slider without leaving the game.
    m_fovVerticalTanScale = *reinterpret_cast<const float*>(base + profile.fov_vertical_tan_scale_rva);
    void* target = reinterpret_cast<void*>(base + profile.render_view_setup_rva);

    SetupFn original = nullptr;
    const auto createStatus =
        hooks.CreateHook(target, reinterpret_cast<void*>(&CameraHook::Detour),
                         reinterpret_cast<void**>(&original));
    g_original.store(original, std::memory_order_release);
    if (createStatus != cameraunlock::hooks::HookStatus::Ok) {
        Log::Line("[camera] could not hook idRenderView::Setup at +0x%X (%s); the mod is dormant "
                  "and the game is unmodified", profile.render_view_setup_rva,
                  cameraunlock::hooks::HookStatusToString(createStatus));
        return false;
    }

    const auto enableStatus = hooks.EnableHook(target);
    if (enableStatus != cameraunlock::hooks::HookStatus::Ok) {
        Log::Line("[camera] could not enable the idRenderView::Setup hook (%s); the mod is dormant "
                  "and the game is unmodified",
                  cameraunlock::hooks::HookStatusToString(enableStatus));
        return false;
    }

    m_trace.Bind(base, profile.world_line_trace_rva);

    const Config& cfg = mod.GetConfig();
    cameraunlock::camera::LeanClampSettings lean;
    lean.skin = cfg.collision_margin;
    lean.release_smoothing = cfg.collision_release_smoothing;
    m_leanClamp.SetSettings(lean);

    // The standoff has to exceed the near clip or the clamp does not deliver
    // what it promises: geometry closer to the eye than the near plane is
    // culled, so a wall held two centimetres away with a ten centimetre near
    // plane is still not drawn and the player still sees the room behind it.
    // Reported rather than corrected - the margin is the user's setting, and
    // silently raising it would hide a near plane that had moved.
    const float znear = *reinterpret_cast<const float*>(base + profile.znear_setting_rva);
    if (cfg.collision_enabled && std::isfinite(znear) && znear > 0.0f &&
        cfg.collision_margin <= znear) {
        Log::Line("[camera] WARN: CollisionMargin=%.3f m is not larger than the camera's near clip "
                  "of %.3f m (r_znear at +0x%X), so a surface held at that distance is culled "
                  "before it is drawn and the lean will still look like it goes through the wall. "
                  "Raise CollisionMargin above %.3f.",
                  cfg.collision_margin, znear, profile.znear_setting_rva, znear);
    }

    Log::Line("[camera] hooked idRenderView::Setup at +0x%X; near clip %.3f m, aim trace %s "
              "(channel 0x%X, %.0f m), collision %s (channel 0x%X, margin %.3f m, release %.2f)",
              profile.render_view_setup_rva, znear,
              cfg.aim_trace_enabled ? "on" : "off", cfg.aim_trace_channel, cfg.aim_trace_range,
              cfg.collision_enabled ? "on" : "off", cfg.collision_channel, cfg.collision_margin,
              cfg.collision_release_smoothing);
    if (!m_trace.IsBound()) {
        Log::Line("[camera] the engine's line check could not be resolved, so the reticle marks "
                  "the aim direction and the lean is not clamped against the level");
    }
    return true;
}

void CameraHook::Arm() {
    // Deliberately not the tail of Install. Every failure in Install leaves the
    // caller free to destroy this object, and a global still pointing at a
    // destroyed hook is one enabled detour away from a use-after-free. A frame
    // landing before this store takes the null branch below, which is the plain
    // pass-through to the game's own Setup.
    g_hook.store(this, std::memory_order_release);
}

void __fastcall CameraHook::Detour(void* renderView, int* windowRect, int* renderRect) {
    const SetupFn original = g_original.load(std::memory_order_acquire);
    CameraHook* const hook = g_hook.load(std::memory_order_acquire);
    if (hook == nullptr || renderView == nullptr) {
        original(renderView, windowRect, renderRect);
        return;
    }
    // Claimed before the guarded region and released inside it, so the claim
    // cannot outlive the call that took it. TryClaim only touches an atomic
    // counter and cannot fault; PreSetup reads the game's own memory and can, so
    // it goes INSIDE the __try - otherwise a fault there would skip PostSetup and
    // leave the claim standing for the rest of the session.
    const bool claimed = hook->TryClaim();
    // __finally rather than a call placed after the trampoline: if the game's own
    // Setup unwinds, the tracked view is left sitting in the render view, and the
    // next frame's PreSetup snapshots THAT as its clean view and composes on top
    // of it, so the pose compounds frame after frame until the view is unusable.
    // The restore has to run on every exit, and a C++ destructor does not run for
    // an SEH unwind.
    __try {
        if (claimed) hook->PreSetup(renderView);
        original(renderView, windowRect, renderRect);
    } __finally {
        hook->PostSetup(renderView, claimed);
    }
}

// Whether this entry owns the pre/post sandwich.
//
// The clean snapshot, m_wrote and the lean allowance are one set of members
// shared by every entry into the detour. A second entry arriving before the
// first has returned - a nested Setup, or a second thread - would snapshot the
// TRACKED view as its clean view and its PostSetup would clear m_wrote, so the
// first call's restore never ran and the game's own view was lost for good.
//
// A counter rather than the view pointer: two entries can carry the SAME
// renderView, and keying on the pointer would let the inner one release the
// outer one's claim. fetch_add returns the previous value, so exactly one entry
// ever sees zero, which makes this correct for concurrent entries as well as
// nested ones. Every claim is released in the __finally above, taken or not.
bool CameraHook::TryClaim() {
    return m_activeDepth.fetch_add(1, std::memory_order_acq_rel) == 0;
}

// Read, never written. The projection this frame is built from comes out of
// these two angles - unless the engine is supplying an explicit projection
// matrix instead, in which case they are not what the frame is drawn with, the
// log has to say so rather than print them, and the zoom compensation has
// nothing to scale against.
void CameraHook::ReadFieldOfView(std::uintptr_t view) {
    const BuildProfile& p = *m_profile;
    m_fovX = *reinterpret_cast<const float*>(view + p.rv_fov_x_offset);
    m_fovY = *reinterpret_cast<const float*>(view + p.rv_fov_y_offset);
    m_explicitProjection =
        *reinterpret_cast<const unsigned char*>(view + p.rv_explicit_projection_offset) != 0;

    const bool usable =
        !m_explicitProjection && IsUsableFovRadians(m_fovX) && IsUsableFovRadians(m_fovY);
    m_tanHalfFovX = usable ? std::tan(m_fovX * 0.5f) : 0.0f;
    m_tanHalfFovY = usable ? std::tan(m_fovY * 0.5f) : 0.0f;
    UpdateZoomFactor();
}

// How much of the pose survives the field of view the frame is actually being
// drawn with.
//
// A narrower view magnifies everything in the frame, head tracking included: the
// head turns ten degrees, the camera turns ten degrees, and the picture moves
// further, by the ratio between the two fields of view. Dishonored 2 moves its
// field of view - the cinematic camera writes keyframed values through
// idView::SetFov, and the spyglass carries its own zoom levels - and left alone
// that reads as the mod's sensitivity jumping the moment the game zooms.
//
// The comparison is in the VERTICAL axis, and that is the whole of the care
// needed here. fov_y descends from the ark_fieldOfView setting and nothing else,
// so the two numbers agree exactly when the game is not zoomed and the factor
// reads 1.0. fov_x is widened out of fov_y for the viewport's real aspect, so
// pairing it with the setting would put every display that is not 16:9 at a
// fixed fraction of the pose, through all of ordinary play, with no symptom
// beyond head tracking feeling weak.
void CameraHook::UpdateZoomFactor() {
    m_fovSettingDegrees = *reinterpret_cast<const float*>(m_base + m_profile->fov_setting_rva);
    m_tanHalfFovYBase =
        voidengine::TanHalfVerticalFovFromSetting(m_fovSettingDegrees, m_fovVerticalTanScale);

    const bool available = m_tanHalfFovY > 0.0f && IsUsableFovDegrees(m_fovSettingDegrees) &&
                           m_tanHalfFovYBase > 0.0f && std::isfinite(m_tanHalfFovYBase);
    if (!m_zoomStateKnown || available != m_zoomAvailable) {
        m_zoomStateKnown = true;
        m_zoomAvailable = available;
        if (available) {
            Log::Line("[camera] zoom compensation available from this frame");
        } else {
            // No compensation rather than a guessed one. Worth a line, because a
            // factor stuck at 1.0 looks exactly like a game that never zooms.
            Log::Line("[camera] no zoom compensation from this frame: fov_y=%.4f rad, "
                      "ark_fieldOfView=%.2f deg at +0x%X, scale=%.4f at +0x%X. Head tracking still "
                      "applies, at 1:1, and the check is repeated every frame.",
                      m_fovY, m_fovSettingDegrees, m_profile->fov_setting_rva,
                      m_fovVerticalTanScale, m_profile->fov_vertical_tan_scale_rva);
        }
    }
    if (!available) {
        m_zoomFactor = 1.0f;
        return;
    }

    m_zoomFactor = cameraunlock::camera::FovZoomFactor(m_tanHalfFovY, m_tanHalfFovYBase);
    LogZoomBasis();
}

// Every term of the factor, once, off the camera rather than off the pose - so
// it is in the log without a tracker connected. The line has to read 1.0000 in
// ordinary gameplay; anything else means the two fields of view are not in the
// same axis or the same units.
void CameraHook::LogZoomBasis() {
    // The gate is what this line reads in ORDINARY play, so an un-zoomed frame
    // is worth reporting even after a zoomed one has been. Taken off the first
    // frame alone, a cinematic or a raised spyglass at startup prints a factor
    // that is legitimately not 1.0 and the check silently loses its meaning;
    // held back until 1.0 alone, a session that opens zoomed says nothing at all
    // about whether the terms can even be read.
    const bool atUnity = std::fabs(m_zoomFactor - 1.0f) <= 1e-3f;
    if (m_zoomBasisLogged && (m_zoomBasisAtUnity || !atUnity)) return;
    m_zoomBasisLogged = true;
    m_zoomBasisAtUnity = atUnity;
    Log::Line("[camera] zoom compensation basis: frame fov_y=%.2f deg (vertical) tan half=%.4f, "
              "ark_fieldOfView=%.2f deg (horizontal at 16:9) x scale %.4f -> tan half=%.4f, "
              "factor=%.4f. fov_x=%.2f deg, which the aspect widens and nothing here uses.",
              m_fovY * kRadToDeg, m_tanHalfFovY, m_fovSettingDegrees, m_fovVerticalTanScale,
              m_tanHalfFovYBase, m_zoomFactor, m_fovX * kRadToDeg);
}

// An angle scaled for the frame's field of view, or the angle untouched when it
// is outside the domain the scaling is defined on.
//
// ScaleAngleForZoom is atan(tan(a) * factor). tan flips sign across the pole, so
// at or past +/-90 the round trip returns the angle on the OPPOSITE side - an
// unguarded +100 comes back as -80 and the view snaps behind the player. Nothing
// upstream bounds the pose: it is consumed at 1:1 and the processor clamps
// position only, so the bound belongs here.
//
// Past the pole the angle is passed through UNSCALED rather than clamped to the
// pole. Clamping would make this a hard limit on how far the head may turn, and
// a limit that only exists while the game happens to be zoomed is worse than no
// compensation at all out there - a tangent projection has no screen
// displacement to hold at 90 degrees anyway.
float CameraHook::ScaleAngleForZoomInDomain(float degrees, float factor) {
    constexpr float kPole = 89.0f;
    if (degrees >= kPole || degrees <= -kPole) return degrees;
    return cameraunlock::camera::ScaleAngleForZoom(degrees, factor);
}

// The pose, scaled for the frame's field of view and applied to the clean view.
//
// Yaw, pitch and the lean all move the picture ACROSS the frame, so all three
// take the zoom factor. Roll turns it about the view axis instead: ten degrees
// of head roll rolls the picture ten degrees at every field of view there is,
// and scaling it would flatten a tilt the player is holding for nothing.
//
// This runs before the write, so everything downstream - the culling frustum,
// the reticle projection built from the basis that was written - agrees with it
// without knowing the factor exists.
bool CameraHook::BuildTrackedView(voidengine::Vec3& origin, voidengine::Mat3& axis) {
    bool modified = false;

    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    if (m_mod->GetRotationRadians(yaw, pitch, roll)) {
        // Through the tangents rather than by multiplying: the screen
        // displacement of an angle goes as tan(angle), so holding that ratio is
        // what keeps the picture moving by the same amount.
        //
        // Unconditional, because gating on the factor would only skip work that
        // is already the identity: atan(tan(a) * 1.0f) returns a to the last bit
        // across the whole domain. When compensation is unavailable the factor
        // is pinned to exactly 1.0f, and when it is available it is a ratio of
        // two independently computed tangents that sits near 1.0 in ordinary
        // play without being bit equal to it. Either way there is nothing to
        // save. The domain guard inside is what makes running it every frame
        // safe.
        yaw = ScaleAngleForZoomInDomain(yaw * kRadToDeg, m_zoomFactor) * voidengine::kDegToRad;
        pitch = ScaleAngleForZoomInDomain(pitch * kRadToDeg, m_zoomFactor) * voidengine::kDegToRad;
        axis = m_mod->IsWorldSpaceYaw()
                   ? voidengine::RotateBasisWorldYaw(m_cleanAxis, yaw, pitch, roll)
                   : voidengine::RotateBasisLocal(m_cleanAxis, yaw, pitch, roll);
        modified = true;
    }

    ClearLeanResult();

    float forward = 0.0f, left = 0.0f, up = 0.0f;
    if (m_mod->GetPositionOffset(forward, left, up)) {
        // Linear: a head offset d seen at depth D lands at
        // d / (2 * D * tan(fov / 2)) of the frame. The whole offset takes the
        // same factor, so a lean keeps its direction and only its size changes.
        const voidengine::Vec3 leaned = voidengine::TranslateAlongBasis(
            m_cleanOrigin, m_cleanAxis, forward * m_zoomFactor, left * m_zoomFactor,
            up * m_zoomFactor);
        m_leanRequested = voidengine::Vec3{leaned.x - m_cleanOrigin.x, leaned.y - m_cleanOrigin.y,
                                           leaned.z - m_cleanOrigin.z};
        m_hadLean = true;
        // Clamped BEFORE it is applied. Clamping afterwards would mean reading
        // back a position that is already inside the wall.
        m_leanApplied = ClampLean(m_leanRequested);
        origin = voidengine::Vec3{m_cleanOrigin.x + m_leanApplied.x,
                                  m_cleanOrigin.y + m_leanApplied.y,
                                  m_cleanOrigin.z + m_leanApplied.z};
        modified = true;
    } else {
        // No lean at all this frame, so the allowance is dropped rather than
        // carried: left standing it would ration the next lean through its
        // release ease from whatever wall the player has already walked away
        // from. Core's clamp does this itself for a lean that shrinks to zero,
        // and this covers the frames it never sees - a closed gameplay gate, a
        // tracker that stopped sending, rotation-only mode.
        m_leanClamp.Reset();
    }
    return modified;
}

// Cuts the lean to whatever the room leaves, and says so on any change.
//
// The policy - how far off a surface to hold the eye, how fast the allowance may
// reopen, what a failed query means - is core's. The query is the engine's own
// line check, and it is the whole of what this file adds.
voidengine::Vec3 CameraHook::ClampLean(const voidengine::Vec3& offset) {
    const Config& cfg = m_mod->GetConfig();
    if (!cfg.collision_enabled || !m_trace.IsBound()) return offset;

    const cameraunlock::math::Vec3 eye{m_cleanOrigin.x, m_cleanOrigin.y, m_cleanOrigin.z};
    const cameraunlock::math::Vec3 desired{offset.x, offset.y, offset.z};
    const cameraunlock::math::Vec3 allowed =
        m_leanClamp.Apply(eye, desired, m_mod->LastFrameSeconds(), &CameraHook::LeanQuery, this);

    const bool contact = m_leanClamp.InContact();
    const bool failed = m_leanClamp.LastQueryFailed();
    if (!m_contactKnown || contact != m_lastContact || failed != m_lastQueryFailed) {
        m_contactKnown = true;
        m_lastContact = contact;
        m_lastQueryFailed = failed;
        // Contact and a failed query are reported together and separately,
        // because "the check runs and the room is open" and "the check is not
        // running" both read as a lean that never gets clamped.
        Log::Line("[camera] lean clamp: %s, query %s (asked %.3f m, got %.3f m)",
                  contact ? "holding the eye off a surface" : "not restricting the lean",
                  failed ? "COULD NOT RUN" : "ran",
                  static_cast<double>(desired.Magnitude()),
                  static_cast<double>(allowed.Magnitude()));
    }
    return voidengine::Vec3{allowed.x, allowed.y, allowed.z};
}

cameraunlock::camera::LeanObstruction CameraHook::LeanQuery(
    void* context, const cameraunlock::math::Vec3& start,
    const cameraunlock::math::Vec3& direction, float max_distance) {
    auto* self = static_cast<CameraHook*>(context);
    cameraunlock::camera::LeanObstruction out;

    // The ray is deliberately longer than the lean plus the standoff core asked
    // for; see kApproachOverreach. Reporting the true hit distance is what keeps
    // that honest - the clamp takes only the room it needs from it.
    const float reach = ReachForLean(max_distance, self->m_leanClamp.Settings().skin);
    const voidengine::TraceResult hit = self->m_trace.Line(
        voidengine::Vec3{start.x, start.y, start.z},
        voidengine::Vec3{direction.x, direction.y, direction.z}, reach,
        self->m_mod->GetConfig().collision_channel);

    out.queried = hit.queried;
    out.blocked = hit.blocked;
    out.distance = hit.distance;
    return out;
}

// How far along CLEAN aim the shot reaches, so the reticle can be drawn on the
// point it lands on rather than along the direction it leaves in.
//
// The two differ by the lean divided by the range: with the eye 0.30 m to one
// side of the gun, a target at 3 m sits a tenth of a radian off the aim
// direction, which is a tenth of the frame. Marking the direction agrees with
// the shot at exactly one range and splays apart either side of it.
void CameraHook::UpdateAimTrace() {
    ClearAimResult();

    const Config& cfg = m_mod->GetConfig();
    if (!cfg.aim_trace_enabled || !m_trace.IsBound()) return;

    const float* fwd = m_cleanAxis.Row(0);
    const voidengine::TraceResult hit =
        m_trace.Line(m_cleanOrigin, voidengine::Vec3{fwd[0], fwd[1], fwd[2]}, cfg.aim_trace_range,
                     cfg.aim_trace_channel);
    m_aimQueried = hit.queried;
    if (!hit.blocked) return;
    m_aimHit = true;
    m_aimDistance = hit.distance;
}

void CameraHook::ClearAimResult() {
    m_aimQueried = false;
    m_aimHit = false;
    m_aimDistance = 0.0f;
}

void CameraHook::ClearLeanResult() {
    m_hadLean = false;
    m_leanRequested = voidengine::Vec3{};
    m_leanApplied = voidengine::Vec3{};
}

// Only ever reached by the entry that claimed the sandwich; see TryClaim.
void CameraHook::PreSetup(void* renderView) {
    const BuildProfile& p = *m_profile;
    const auto view = reinterpret_cast<std::uintptr_t>(renderView);

    ReadFieldOfView(view);

    // Setup derives every matrix from the game copy at offset 0, so that is what
    // is snapshotted here, what gets written, and what PostSetup puts back.
    m_cleanOrigin = *reinterpret_cast<const voidengine::Vec3*>(view + p.rv_vieworg_offset);
    m_cleanAxis = *reinterpret_cast<const voidengine::Mat3*>(view + p.rv_viewaxis_offset);
    m_renderOrigin = m_cleanOrigin;
    m_renderAxis = m_cleanAxis;
    m_wrote = false;

    const bool active = m_mod->UpdateForFrame();
    InjectFrame(renderView, active);
    // Written once, for every path the frame took, so a path that decides to
    // inject nothing is as visible in the log as one that injects.
    LogFrame(active, renderView);
    LogTrace(active);
}

// The frame's decision: whether there is a usable clean view to compose on, what
// the pose makes of it, and which of the two the render view is left holding.
// The clean snapshot is already in place; nothing here logs.
void CameraHook::InjectFrame(void* renderView, bool active) {
    const BuildProfile& p = *m_profile;
    const auto view = reinterpret_cast<std::uintptr_t>(renderView);

    const bool cleanUsable =
        voidengine::IsOrthonormal(m_cleanAxis) && voidengine::IsFinite(m_cleanOrigin);
    if (active && !cleanUsable) {
        ReportRefusal(RefusalKind::CleanView,
                      "the view read out of the frame is not a usable camera");
    }
    if (!active || !cleanUsable) {
        // Nothing is being injected, so the reticle has nothing to correct and
        // has to be told: the engine writes its centred position once and then
        // leaves the object alone, so a correction left standing stays on screen.
        ClearAimResult();
        ClearLeanResult();
        // The room the previous frame found does not describe this one. A cut,
        // a load or a closed gate all land here, and an allowance carried
        // across one of them rations the first lean in the new place against
        // the old place's wall.
        m_leanClamp.Reset();
        PublishFrame(m_cleanOrigin, m_cleanAxis, false);
        return;
    }

    // Along the CLEAN aim, from the CLEAN eye, before anything is written: this
    // is the line the game will fire, trace and raycast along, and it must not
    // depend on where the head has moved the view to.
    UpdateAimTrace();

    voidengine::Vec3 renderOrigin = m_cleanOrigin;
    voidengine::Mat3 renderAxis = m_cleanAxis;
    const bool built = BuildTrackedView(renderOrigin, renderAxis);
    const bool trackedUsable =
        built && voidengine::IsOrthonormal(renderAxis) && voidengine::IsFinite(renderOrigin);
    if (!trackedUsable) {
        // BuildTrackedView has already run the clamp and left an allowance
        // standing, and this frame applies no lean at all - so it goes, for the
        // same reason the gate-closed path above drops it.
        m_leanClamp.Reset();
        PublishFrame(m_cleanOrigin, m_cleanAxis, false);
        // Only when there WAS a tracked view to malform. A frame with no pose at
        // all is the ordinary idle case, not a refusal.
        if (built) ReportRefusal(RefusalKind::TrackedView, "the tracked view came out malformed");
        return;
    }

    *reinterpret_cast<voidengine::Vec3*>(view + p.rv_vieworg_offset) = renderOrigin;
    *reinterpret_cast<voidengine::Mat3*>(view + p.rv_viewaxis_offset) = renderAxis;
    m_renderOrigin = renderOrigin;
    m_renderAxis = renderAxis;
    m_wrote = true;
    PublishFrame(renderOrigin, renderAxis, true);
}

// Said once, from the entry that was turned away, so a game that really does
// render a second view says so in the log rather than silently losing tracking
// on it.
void CameraHook::ReportNestedEntry() {
    if (m_nestedLogged) return;
    m_nestedLogged = true;
    Log::Line("[camera] idRenderView::Setup was entered again before the previous call returned; "
              "that view is being left untracked. Head tracking still applies to the view that "
              "claimed the frame.");
}

void CameraHook::ReportRefusal(RefusalKind kind, const char* what) {
    // A latch per reason, not one for both. They are different faults with
    // different next steps, and a single latch means whichever happens first
    // permanently silences the other.
    bool& logged = m_refusalLogged[static_cast<int>(kind)];
    if (logged) return;
    logged = true;
    const BuildProfile& p = *m_profile;
    Log::Line("[camera] refusing to inject: %s. org=(%g %g %g) fwd=(%g %g %g) read at +0x%X/+0x%X. "
              "Head tracking will not appear while this holds.",
              what, m_cleanOrigin.x, m_cleanOrigin.y, m_cleanOrigin.z, m_cleanAxis.m[0],
              m_cleanAxis.m[1], m_cleanAxis.m[2], p.rv_vieworg_offset, p.rv_viewaxis_offset);
}

void CameraHook::PostSetup(void* renderView, bool claimed) {
    // An entry that did not claim the sandwich wrote nothing and must not touch
    // the claiming call's state. It still releases its own reference.
    if (!claimed) {
        m_activeDepth.fetch_sub(1, std::memory_order_acq_rel);
        ReportNestedEntry();
        return;
    }

    // The claim is released LAST, after the restore, not first. Releasing it up
    // here would leave m_wrote and the clean snapshot unguarded for the whole of
    // the restore: another entry could claim in that window, overwrite the
    // snapshot from its own view and clear m_wrote, and this call would then
    // either skip its restore entirely - leaving the tracked view in place for
    // the next frame to compose on top of, which is the compounding this whole
    // protocol exists to prevent - or write the other view's camera into this
    // one.
    if (m_wrote) {
        RestoreCleanView(renderView);
    }
    m_activeDepth.fetch_sub(1, std::memory_order_release);
}

void CameraHook::RestoreCleanView(void* renderView) {
    const BuildProfile& p = *m_profile;
    const auto view = reinterpret_cast<std::uintptr_t>(renderView);

    // The game's own view goes straight back. Nothing in game logic reads this
    // copy, so this is not what decouples aim - the write only ever existed
    // between here and the Setup call, and every matrix the frame is drawn and
    // culled with was derived from it inside that window. What the restore buys
    // is that a frame the engine renders WITHOUT refilling the view from the game
    // starts from the game's view again instead of compounding the head rotation.
    *reinterpret_cast<voidengine::Vec3*>(view + p.rv_vieworg_offset) = m_cleanOrigin;
    *reinterpret_cast<voidengine::Mat3*>(view + p.rv_viewaxis_offset) = m_cleanAxis;
    m_wrote = false;
}

// The sequence goes odd for the duration of the stores and even again after
// them, so a reader can tell that it read a torn copy.
void CameraHook::PublishFrame(const voidengine::Vec3& origin, const voidengine::Mat3& axis,
                              bool valid) {
    m_publishedSeq.fetch_add(1, std::memory_order_acq_rel);
    m_publishedValid = valid;
    m_published.clean_origin = m_cleanOrigin;
    m_published.clean_axis = m_cleanAxis;
    m_published.render_origin = origin;
    m_published.render_axis = axis;
    m_published.tan_half_fov_x = m_tanHalfFovX;
    m_published.tan_half_fov_y = m_tanHalfFovY;
    m_published.aim_hit = m_aimHit;
    m_published.aim_distance = m_aimDistance;
    m_publishedSeq.fetch_add(1, std::memory_order_acq_rel);
}

// A reader that sees the same EVEN sequence either side of its copy has a frame
// nothing rewrote underneath it. An odd or a changed value means the render
// thread was mid-publish, and the answer then is to leave the reticle where it
// is for this frame rather than place it from half of one basis and half of
// another.
bool CameraHook::LastInjectedFrame(Frame& out) const {
    const std::uint32_t before = m_publishedSeq.load(std::memory_order_acquire);
    if (before % 2 != 0) return false;
    const bool valid = m_publishedValid;
    out = m_published;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (m_publishedSeq.load(std::memory_order_acquire) != before) return false;
    return valid;
}

// The dense trace a user turns on to check an axis, the aim line or the lean
// clamp, and turns off again. Off by default.
//
// Everything the frame decided is on one line, in one place: the view the game
// asked for, the view that was drawn, how far the aim line reached, and what the
// clamp did with the lean. An axis that is mirrored, an aim line that never
// reaches anything and a clamp that is not running all look identical from
// inside the game and are all obvious here.
void CameraHook::LogTrace(bool active) {
    const long interval = m_mod->GetConfig().camera_trace_ms;
    if (interval <= 0) return;
    if (!m_traceGate.Due(GetTickCount64(), static_cast<std::uint64_t>(interval))) return;

    char aim[64];
    DescribeAim(active, aim, sizeof(aim));
    char lean[128];
    DescribeLean(active, lean, sizeof(lean));

    Log::Line("[trace] %s org=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f) left=(%.3f %.3f %.3f) "
              "up=(%.3f %.3f %.3f) -> org=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f) aim=%s lean=%s",
              active ? "active" : "idle",
              m_cleanOrigin.x, m_cleanOrigin.y, m_cleanOrigin.z, m_cleanAxis.m[0], m_cleanAxis.m[1],
              m_cleanAxis.m[2], m_cleanAxis.m[3], m_cleanAxis.m[4], m_cleanAxis.m[5],
              m_cleanAxis.m[6], m_cleanAxis.m[7], m_cleanAxis.m[8], m_renderOrigin.x,
              m_renderOrigin.y, m_renderOrigin.z, m_renderAxis.m[0], m_renderAxis.m[1],
              m_renderAxis.m[2], aim, lean);
}

// Every "nothing happened" here has to name WHICH nothing. A suspended gate, an
// aim line that reached open sky and an aim line that never ran all leave the
// reticle marking the aim direction, and they need three different fixes: play
// on, look at something, check the trace is bound.
void CameraHook::DescribeAim(bool active, char* out, std::size_t size) const {
    if (!active) {
        std::snprintf(out, size, "not run (suspended)");
    } else if (!m_aimQueried) {
        std::snprintf(out, size, "not run (off or unresolved)");
    } else if (!m_aimHit) {
        std::snprintf(out, size, "clear to %.0f m", m_mod->GetConfig().aim_trace_range);
    } else {
        std::snprintf(out, size, "%.2f m", m_aimDistance);
    }
}

void CameraHook::DescribeLean(bool active, char* out, std::size_t size) const {
    if (!active) {
        std::snprintf(out, size, "not applied (suspended)");
        return;
    }
    if (!m_hadLean) {
        std::snprintf(out, size, "none (rotation only, or no pose yet)");
        return;
    }
    // "clear" has to mean the check ran and found room, never that no check ran
    // at all, or a collision setting left switched off reads as a level with
    // nothing in it.
    const char* verdict = "not checked";
    if (m_mod->GetConfig().collision_enabled && m_trace.IsBound()) {
        verdict = m_leanClamp.LastQueryFailed()
                      ? "CLAMP QUERY FAILED"
                      : (m_leanClamp.InContact() ? "clamped" : "clear");
    }
    std::snprintf(out, size, "(%.3f %.3f %.3f) -> (%.3f %.3f %.3f) %s", m_leanRequested.x,
                  m_leanRequested.y, m_leanRequested.z, m_leanApplied.x, m_leanApplied.y,
                  m_leanApplied.z, verdict);
}

void CameraHook::LogFrame(bool active, const void* renderView) {
    if (!m_frameLog.ShouldLog()) return;
    char fov[80];
    if (m_explicitProjection) {
        std::snprintf(fov, sizeof(fov), "explicit zoom=%.4f", m_zoomFactor);
    } else {
        std::snprintf(fov, sizeof(fov), "%.2fx%.2f deg tan=%.4f/%.4f base=%.2f deg zoom=%.4f",
                      m_fovX * kRadToDeg, m_fovY * kRadToDeg, m_tanHalfFovX, m_tanHalfFovY,
                      m_fovSettingDegrees, m_zoomFactor);
    }
    Log::Line("[camera] %s view=%p org=(%.1f %.1f %.1f) fwd=(%.3f %.3f %.3f) -> org=(%.1f %.1f %.1f) "
              "fwd=(%.3f %.3f %.3f) fov=%s",
              active ? "active" : "idle", renderView,
              m_cleanOrigin.x, m_cleanOrigin.y, m_cleanOrigin.z,
              m_cleanAxis.m[0], m_cleanAxis.m[1], m_cleanAxis.m[2],
              m_renderOrigin.x, m_renderOrigin.y, m_renderOrigin.z,
              m_renderAxis.m[0], m_renderAxis.m[1], m_renderAxis.m[2], fov);
}

}  // namespace D2HT
