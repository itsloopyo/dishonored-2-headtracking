// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "hooks/reticle_hook.h"

#include "core/logging.h"
#include "core/mod.h"
#include "hooks/camera_hook.h"
#include "voidengine/void_math.h"

#include <cameraunlock/hooks/hook_manager.h>
#include <cameraunlock/math/angle_utils.h>
#include <cameraunlock/memory/safe_memory.h>

namespace D2HT {

namespace {

using UpdateFn = void(__fastcall*)(void*);

std::atomic<UpdateFn> g_origUpdate{nullptr};

// The release store in Install is what publishes the hook's own resolved engine
// entry points to the render thread along with the object itself.
std::atomic<ReticleHook*> g_hook{nullptr};

// Below this the point is level with the eye or behind it, and the perspective
// divide runs away to infinity. Refusing there is what keeps a coordinate of
// 1e30 out of the Scaleform object.
constexpr float kMinForward = 1e-3f;

// How far outside the frame the reticle may be pushed before it is pinned. The
// engine draws it wherever it is told, so a point behind the player has to be
// stopped somewhere, and the edge of the stage is where it stops.
constexpr float kMaxNdc = 1.0f;

constexpr unsigned kBurstLines = 4;
constexpr unsigned kEarlyLines = 16;
constexpr unsigned kEarlyIntervalFrames = 600;
constexpr unsigned kSteadyIntervalFrames = 4000;

// Normalised device coordinates of a direction seen through a basis whose rows
// are forward / left / up. The engine's own world-to-view builder makes view x
// the NEGATED left row, so screen right is -left.
//
// A direction rather than a point, deliberately: the caller decides what the
// direction is measured FROM, and that choice is what separates a reticle that
// follows the shot from one that follows the view. From the RENDER eye to the
// traced impact it is the shot; from the CLEAN eye it is the aim direction, which
// carries no range and so no parallax.
bool ProjectDirection(const voidengine::Mat3& axis, const float d[3], float tanX, float tanY,
                      float& ndcX, float& ndcY) {
    const float forward = voidengine::Dot(axis.Row(0), d);
    if (!(forward > kMinForward) || tanX <= 0.0f || tanY <= 0.0f) return false;
    const float left = voidengine::Dot(axis.Row(1), d);
    const float up = voidengine::Dot(axis.Row(2), d);
    ndcX = (-left / forward) / tanX;
    ndcY = (up / forward) / tanY;
    return true;
}

}  // namespace

ReticleHook::ReticleHook()
    : m_placementLog(kBurstLines, kEarlyLines, kEarlyIntervalFrames, kSteadyIntervalFrames) {}

bool ReticleHook::Install(const BuildProfile& profile, Mod& mod) {
    m_profile = &profile;
    m_mod = &mod;
    m_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    m_iggySetFloat = reinterpret_cast<IggySetFloatFn>(m_base + profile.iggy_set_member_rva);
    m_resolveCpnt = reinterpret_cast<ResolveCpntFn>(m_base + profile.checked_cpnt_resolve_rva);
    m_centredValue = *reinterpret_cast<const float*>(m_base + profile.cpnt_centred_value_rva);

    auto& hooks = cameraunlock::hooks::HookManager::Instance();
    void* target = reinterpret_cast<void*>(m_base + profile.crosshair_update_rva);
    UpdateFn original = nullptr;
    const auto createStatus =
        hooks.CreateHook(target, reinterpret_cast<void*>(&ReticleHook::DetourUpdate),
                         reinterpret_cast<void**>(&original));
    // Published before the hook is enabled, never after. The detour is live from
    // the moment EnableHook returns and its first act is to call through this
    // pointer, so a crosshair update landing in the gap would call null.
    g_origUpdate.store(original, std::memory_order_release);
    if (createStatus != cameraunlock::hooks::HookStatus::Ok ||
        hooks.EnableHook(target) != cameraunlock::hooks::HookStatus::Ok) {
        Log::Line("[reticle] could not hook the crosshair update at +0x%X; the reticle stays where "
                  "the game puts it and will not follow a turned or leaned view",
                  profile.crosshair_update_rva);
        return false;
    }
    g_hook.store(this, std::memory_order_release);
    Log::Line("[reticle] hooked the crosshair update at +0x%X, centred when the component's flag "
              "reads %g", profile.crosshair_update_rva, m_centredValue);
    return true;
}

void __fastcall ReticleHook::DetourUpdate(void* element) {
    // The engine's own update runs first and keeps its bookkeeping intact - the
    // crosshair style, the alpha, and the cached flag that says whether it thinks
    // the reticle is centred. Only the final position is then rewritten.
    g_origUpdate.load(std::memory_order_acquire)(element);
    ReticleHook* const hook = g_hook.load(std::memory_order_acquire);
    if (hook == nullptr || element == nullptr) return;
    hook->Place(reinterpret_cast<std::uintptr_t>(element));
}

void ReticleHook::Place(std::uintptr_t element) {
    // The bound Scaleform movie, checked exactly as the engine checks it. Null
    // between the widget being torn down and bound again, and the teardown
    // RELEASES the AS3 object this hook would otherwise write through - the
    // engine's own update returns here rather than touching it, and so must
    // this, or a HUD rebuild at a level transition is a crash in the mod. That
    // is a "touch nothing at all" gate rather than a frame with nothing to
    // correct, so the reticle is not handed back through it either.
    const std::uintptr_t movieField = element + m_profile->crosshair_movie_offset;
    std::uintptr_t movie = 0;
    if (!cameraunlock::memory::SafeRead(movieField, movie)) return;
    if (movie == 0) return;

    if (!TryPlace(element)) Release(element);
}

bool ReticleHook::TryPlace(std::uintptr_t element) {
    const BuildProfile& p = *m_profile;

    // Only while a pose actually reached the camera. On every other frame the
    // engine's own placement is what the reticle needs.
    const CameraHook* camera = m_mod->Camera();
    CameraHook::Frame frame;
    if (camera == nullptr || !camera->LastInjectedFrame(frame)) return false;

    const std::uintptr_t cpnt =
        m_resolveCpnt(reinterpret_cast<void*>(element + p.crosshair_cpnt_offset), 2);
    if (cpnt == 0) return false;

    float centred = 0.0f;
    float point[3] = {0.0f, 0.0f, 0.0f};
    if (!cameraunlock::memory::SafeRead(cpnt + p.cpnt_centred_offset, centred) ||
        !cameraunlock::memory::SafeRead(cpnt + p.cpnt_world_point_offset, point)) {
        return false;
    }

    // The reticle marks where the shot lands, seen from the eye the frame was
    // drawn with.
    //
    // With an impact point the direction is measured from the RENDER origin, so
    // the lean is in it: move the head 0.30 m to the side and the point a few
    // metres away swings across the frame by exactly as much as the picture did,
    // and the reticle stays on it. That is the whole positional correction, and
    // it is why the vector below starts at render_origin and not at the eye the
    // shot leaves from.
    //
    // Without one - a shot into open sky, or the aim trace off - there is no
    // range to correct against and the best available answer is the clean aim
    // DIRECTION seen through the tracked basis. That is exact under rotation and
    // ignores the lean, which is right for a point at infinity and increasingly
    // wrong as the target gets closer. The log names which of the two was used.
    const bool isCentred = centred == m_centredValue;
    const float* clean = frame.clean_axis.Row(0);
    const float toPoint[3] = {point[0] - frame.clean_origin.x, point[1] - frame.clean_origin.y,
                              point[2] - frame.clean_origin.z};

    // The impact, from the clean eye along clean aim, which is the line the game
    // fires along.
    const float impact[3] = {frame.clean_origin.x + clean[0] * frame.aim_distance,
                             frame.clean_origin.y + clean[1] * frame.aim_distance,
                             frame.clean_origin.z + clean[2] * frame.aim_distance};
    const float fromRender[3] = {impact[0] - frame.render_origin.x,
                                 impact[1] - frame.render_origin.y,
                                 impact[2] - frame.render_origin.z};

    const char* source = nullptr;
    const float* direction = nullptr;
    if (frame.aim_hit) {
        source = "impact";
        direction = fromRender;
    } else if (isCentred) {
        source = "aim direction";
        direction = clean;
    } else {
        source = "marked point";
        direction = toPoint;
    }

    float ndcX = 0.0f, ndcY = 0.0f;
    if (!ProjectDirection(frame.render_axis, direction, frame.tan_half_fov_x, frame.tan_half_fov_y,
                          ndcX, ndcY)) {
        return false;
    }
    Write(element, ndcX, ndcY);
    m_placed = true;

    const float lean[3] = {frame.render_origin.x - frame.clean_origin.x,
                           frame.render_origin.y - frame.clean_origin.y,
                           frame.render_origin.z - frame.clean_origin.z};
    LogPlacement(source, ndcX, ndcY,
                 frame.aim_hit ? frame.aim_distance : voidengine::Dot(clean, toPoint), lean,
                 frame.aim_hit ? impact : point);
    return true;
}

// Hands the reticle back to the engine on the first frame this hook stops
// correcting it.
//
// The engine places its centred reticle ONCE, on the transition into centred
// mode, and latches that on a byte of its own; it then leaves the Scaleform
// object alone for as long as the reticle stays centred. So simply going quiet
// leaves whatever this hook last wrote sitting on screen - turn your head left,
// press End, and the crosshair stays parked off-centre for the rest of the
// mission. Clearing the engine's latch makes it re-place the reticle itself on
// the very next frame, which is the position it would have had all along.
void ReticleHook::Release(std::uintptr_t element) {
    if (!m_placed) return;
    m_placed = false;
    *reinterpret_cast<unsigned char*>(element + m_profile->crosshair_centred_latch_offset) = 0;
}

void ReticleHook::LogPlacement(const char* source, float ndcX, float ndcY, float distance,
                               const float lean[3], const float point[3]) {
    // Two schedules, either of which can let the line through: the always-on
    // trickle that a shipped log is read from, and the dense interval a user
    // turns on to check the reticle against the shot.
    bool due = m_placementLog.ShouldLog();
    const long interval = m_mod->GetConfig().camera_trace_ms;
    if (!due && interval > 0) {
        due = m_traceGate.Due(GetTickCount64(), static_cast<std::uint64_t>(interval));
    }
    if (!due) return;
    // The source is the first thing to read: "impact" is the corrected reticle,
    // and anything else means the lean is not being compensated on this frame
    // and the reticle will drift off a near target as the head moves.
    Log::Line("[reticle] %s ndc=(%.4f %.4f) dist=%.2f lean=(%.3f %.3f %.3f) "
              "point=(%.1f %.1f %.1f)",
              source, ndcX, ndcY, distance, lean[0], lean[1], lean[2], point[0], point[1],
              point[2]);
}

void ReticleHook::Write(std::uintptr_t element, float ndcX, float ndcY) {
    const BuildProfile& p = *m_profile;
    const float stageX = *reinterpret_cast<const float*>(m_base + p.stage_scale_x_rva);
    const float stageY = *reinterpret_cast<const float*>(m_base + p.stage_scale_y_rva);
    if (!std::isfinite(stageX) || !std::isfinite(stageY)) return;

    // The engine's own scaling, which is what keeps this write in the same units
    // as the one it replaces: x = 0.5 * ndc_x * stage_x, and y negated because
    // the stage runs downwards.
    float x = 0.5f * cameraunlock::math::Clamp(ndcX, -kMaxNdc, kMaxNdc) * stageX;
    float y = -0.5f * cameraunlock::math::Clamp(ndcY, -kMaxNdc, kMaxNdc) * stageY;
    if (!std::isfinite(x) || !std::isfinite(y)) return;

    const std::uintptr_t object = element + p.crosshair_object_offset;
    m_iggySetFloat(object, &x, reinterpret_cast<void*>(element + p.crosshair_x_member_offset));
    m_iggySetFloat(object, &y, reinterpret_cast<void*>(element + p.crosshair_y_member_offset));
}

}  // namespace D2HT
