// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/log_throttle.h"
#include "hooks/build_profiles.h"
#include "voidengine/void_math.h"
#include "voidengine/world_trace.h"

#include <cameraunlock/camera/lean_clamp.h>

namespace D2HT {

class Mod;

// Hooks idRenderView::Setup and injects the head pose into the render view the
// frame is built from.
//
// The injection is a sandwich around the original call: the game's own view is
// read out of the game copy `g` at offset 0, the tracked view is written in its
// place, Setup runs - copying `g` into the render copy `r` and deriving the
// projection, view and MVP matrices from it - and the game's view is put
// straight back. Nothing that runs later can observe the tracked pose, so aim,
// projectiles, traces and AI vision are identical with tracking on and off; and
// because every matrix is derived AFTER the write, the culling frustum is turned
// with the view and there is nothing to cull at the edges of a turned frame.

// A zero-extent ray that stops where the lean stops cannot see the surface the
// lean is about to come to rest against, so the eye travels the whole lean,
// arrives against the wall and only pops back out once the head pushes far
// enough for the ray itself to cross it. The overreach a surface needs is the
// standoff divided by the cosine of the approach angle, and nothing here knows
// the surface normal, so this is that divisor floored: it covers every approach
// out to seventy degrees off the normal, and looking further than needed costs
// nothing because the clamp only ever takes the room it asked for.
inline constexpr float kApproachOverreach = 3.0f;

// How far the lean trace actually reaches: what core asked for, plus the
// overreach above.
//
// In the header, and used by CameraHook::LeanQuery rather than written out
// there, so the unit test can call the SAME function the hook calls instead of
// restating the arithmetic. Restated, the test pins only the constant and a
// change to the formula ships with every gate green.
inline float ReachForLean(float max_distance, float skin) {
    return max_distance + skin * kApproachOverreach;
}

class CameraHook {
public:
    CameraHook();

    bool Install(const BuildProfile& profile, Mod& mod);

    // Makes the detour start injecting. Separate from Install so the caller can
    // finish wiring up everything the first frame touches before a frame can
    // arrive. Until this is called the detour is a pass-through.
    void Arm();

    // The last frame's views and half-field tangents, for the reticle. False
    // when the last frame injected nothing, in which case the reticle has
    // nothing to correct.
    //
    // Published under a seqlock rather than a single flag. Which thread runs the
    // HUD element's update has not been established, and a plain struct copy of
    // twenty-four floats read while the render thread is rewriting them yields a
    // basis half from one frame and half from the next: not a wrong reticle
    // position, a non-orthonormal one, drawn wherever the arithmetic lands.
    struct Frame {
        voidengine::Vec3 clean_origin;
        voidengine::Mat3 clean_axis;
        voidengine::Vec3 render_origin;
        voidengine::Mat3 render_axis;
        float tan_half_fov_x = 0.0f;
        float tan_half_fov_y = 0.0f;

        // Where the shot lands: how far along CLEAN aim, from the clean eye, the
        // engine's line check stopped. False when the aim line reached nothing,
        // in which case there is no impact point and the reticle marks the aim
        // direction instead - which is the right answer for a shot into open
        // sky, and the only available one when the trace is switched off.
        //
        // Traced here rather than in the reticle hook so the engine is called
        // once a frame, from one thread, whichever thread that turns out to be.
        bool aim_hit = false;
        float aim_distance = 0.0f;
    };
    bool LastInjectedFrame(Frame& out) const;

private:
    static void __fastcall Detour(void* renderView, int* windowRect, int* renderRect);
    // Whether this entry owns the pre/post sandwich. Cannot fault, so it is safe
    // to call before the guarded region; see the definition.
    bool TryClaim();
    void PreSetup(void* renderView);
    // The frame's decision, split out of PreSetup so the two diagnostic lines
    // are written once for every path rather than once per path.
    void InjectFrame(void* renderView, bool active);
    void PostSetup(void* renderView, bool claimed);
    // Puts the game's own view back, byte for byte. Only called with the claim
    // still held; see PostSetup.
    void RestoreCleanView(void* renderView);
    void ReadFieldOfView(std::uintptr_t view);
    void UpdateZoomFactor();
    void LogZoomBasis();
    void PublishFrame(const voidengine::Vec3& origin, const voidengine::Mat3& axis, bool valid);
    bool BuildTrackedView(voidengine::Vec3& origin, voidengine::Mat3& axis);
    void UpdateAimTrace();
    voidengine::Vec3 ClampLean(const voidengine::Vec3& offset);

    // This frame's aim and lean results, cleared. Each is called both from the
    // path that is about to recompute it and from the frame that will not have
    // one at all, so the two cannot drift apart.
    void ClearAimResult();
    void ClearLeanResult();

    void LogFrame(bool active, const void* renderView);
    void LogTrace(bool active);
    // What the trace line says about this frame's aim line and lean, written
    // into @p out. Split out because each has three "nothing happened" cases
    // that have to be told apart, and inline they buried the line itself.
    void DescribeAim(bool active, char* out, std::size_t size) const;
    void DescribeLean(bool active, char* out, std::size_t size) const;

    // The engine query, in the shape core's LeanClamp takes it. Static because
    // the clamp holds a plain function pointer; the hook arrives as the context.
    static cameraunlock::camera::LeanObstruction LeanQuery(
        void* context, const cameraunlock::math::Vec3& start,
        const cameraunlock::math::Vec3& direction, float max_distance);

    // Scales an angle for the frame's field of view, passing it through
    // untouched outside the domain the tangent round trip is defined on.
    static float ScaleAngleForZoomInDomain(float degrees, float factor);

    // Says once that a second entry into Setup was turned away.
    void ReportNestedEntry();

    // Which sanity guard refused, and so which latch the report uses.
    enum class RefusalKind { CleanView = 0, TrackedView = 1, Count = 2 };

    // Says once per kind that a guard refused this frame. Refusing silently is
    // the exact symptom a wrong vieworg/viewaxis offset on a new build produces:
    // every frame is dropped, the log still reports the hook as active, and the
    // player reports "no head tracking" with nothing to triage.
    void ReportRefusal(RefusalKind kind, const char* what);

    const BuildProfile* m_profile = nullptr;
    Mod* m_mod = nullptr;
    std::uintptr_t m_base = 0;

    // Carried across the original call so PostSetup can put the game's view back
    // exactly as it was, byte for byte, rather than recomputing an inverse.
    voidengine::Vec3 m_cleanOrigin{};
    voidengine::Mat3 m_cleanAxis{};
    voidengine::Vec3 m_renderOrigin{};
    voidengine::Mat3 m_renderAxis{};
    // Always false on entry to PreSetup: the only writers are InjectFrame, which
    // sets it after a successful write, and PostSetup, which clears it after the
    // matching restore. That invariant is what makes a fault inside PreSetup
    // safe - PostSetup then restores nothing rather than writing a previous
    // frame's snapshot into this frame's view.
    bool m_wrote = false;

    // How many entries into the detour are in flight. Exactly one of them sees
    // zero and owns the pre/post sandwich; see TryClaim.
    std::atomic<int> m_activeDepth{0};
    bool m_nestedLogged = false;
    bool m_refusalLogged[static_cast<int>(RefusalKind::Count)] = {};

    // The frame's field of view. fov_x_RADIANS / fov_y_RADIANS are full angles;
    // the projection builder feeds fov_y through tan(fov_y * 0.5) and refuses the
    // frame unless both are positive. Read, never written: the game has its own
    // FOV slider, and the zoom compensation scales the pose rather than the view.
    float m_fovX = 0.0f;
    float m_fovY = 0.0f;
    float m_tanHalfFovX = 0.0f;
    float m_tanHalfFovY = 0.0f;
    bool m_explicitProjection = false;

    // The un-zoomed reference: the ark_fieldOfView setting as the player left it,
    // and the half-angle tangent it stands for in the vertical axis the frame's
    // fov_y is measured in.
    float m_fovSettingDegrees = 0.0f;
    float m_tanHalfFovYBase = 0.0f;

    // How much narrower the frame is than the setting, as a ratio of half-angle
    // tangents, and so how much further the same head movement would sweep the
    // picture if it were left alone. 1.0 through ordinary play; below 1 whenever
    // the game narrows the view for the spyglass, a sprint, or a cinematic.
    float m_zoomFactor = 1.0f;

    // Read once from the image: the constant idView::SetFovXandY turns the
    // setting into fov_y with.
    float m_fovVerticalTanScale = 0.0f;

    // Whether the factor could be computed, reported on every CHANGE rather than
    // once. A one-shot latch burns on the first frame of the session, which is
    // exactly the frame most likely to carry an uninitialised field of view or an
    // explicit projection matrix for the intro - after which an ark_fieldOfView
    // that genuinely cannot be read for the rest of the session says nothing at
    // all, and the mod runs uncompensated in silence.
    bool m_zoomAvailable = false;
    bool m_zoomStateKnown = false;

    // The basis line is written twice at most: once on the first frame there is
    // a factor to report at all, and once more on the first UN-ZOOMED frame if
    // that first one was not un-zoomed. Only the second is the gate - a factor
    // wrong by a constant looks exactly like a factor that is right, and 1.0000
    // in ordinary play is the only thing that separates them - but a session
    // that opens on a cinematic would otherwise have no basis line at all until
    // it happened to reach 1.0, and the first line is what says the terms are
    // being read at all.
    bool m_zoomBasisLogged = false;
    bool m_zoomBasisAtUnity = false;

    // The last frame a pose actually reached the camera, for the reticle.
    // m_publishedSeq is odd while a publish is in progress and even between
    // them, so a reader that sees the same even value either side of its copy
    // has a frame that was never rewritten underneath it. It is never taken
    // back down for the duration of the original Setup call: a reader on
    // another thread would then find nothing to correct for most of the frame,
    // and the previous frame's basis is a far better answer than none.
    Frame m_published{};
    std::atomic<std::uint32_t> m_publishedSeq{0};
    bool m_publishedValid = false;

    // Render-thread only, like LogFrame itself.
    LogThrottle m_frameLog;

    // The engine's line check, and the two things built on it.
    voidengine::WorldTrace m_trace;
    cameraunlock::camera::LeanClamp m_leanClamp;

    // This frame's aim result and the lean actually applied, for the diagnostic
    // line and for the reticle. Touched only on the hook's own thread.
    //
    // Queried and hit are separate for the reason core's LeanObstruction keeps
    // them separate: an aim line that never ran and one that ran and reached
    // nothing are different faults with different fixes, and a log that writes
    // the same words for both sends a reader looking for a missing hook when
    // the answer is that the player was in a menu.
    bool m_aimQueried = false;
    bool m_aimHit = false;
    float m_aimDistance = 0.0f;
    voidengine::Vec3 m_leanRequested{};
    voidengine::Vec3 m_leanApplied{};
    bool m_hadLean = false;

    // Whether the clamp reported contact or a failed query on the last frame it
    // ran, so a CHANGE can be reported rather than every frame. A clamp that has
    // quietly stopped clamping looks exactly like one that never engaged, so the
    // two are logged separately.
    bool m_contactKnown = false;
    bool m_lastContact = false;
    bool m_lastQueryFailed = false;

    // Interval-driven, and separate from m_frameLog: the frame log is the
    // always-on trickle and this is the dense trace a user turns on to check an
    // axis or the collision clamp.
    IntervalGate m_traceGate;
};

}  // namespace D2HT
