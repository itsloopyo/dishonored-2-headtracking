// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <cstdint>

#include "core/log_throttle.h"
#include "hooks/build_profiles.h"

namespace D2HT {

class Mod;

// Puts the game's own reticle where the shot lands.
//
// Head tracking moves the eye off the gun: the frame is drawn from the tracked
// view while the game aims, traces and fires from the clean one, so the reticle
// the HUD draws for the clean view no longer marks where rounds go. Nothing is
// drawn here and no second reticle appears - arkHUDElementCrosshair already
// places its reticle by writing `x` and `y` on a Scaleform object, and this hook
// writes the corrected position through the same setter it uses.
//
// The hook is on the element's per-frame update rather than on either of the two
// placement helpers it calls. The helper that parks the reticle at the centre
// runs once, on the transition into centred mode, and the element then leaves
// the Scaleform object alone for as long as the reticle stays centred - so a
// hook on the helpers corrects the reticle for a single frame and then goes
// quiet, which is exactly the shape of a reticle that does not follow.
//
// What is projected is the POINT the shot lands on, seen from the eye the frame
// was drawn with.
//
// That distinction is the whole of the positional half. Under rotation alone the
// two eyes coincide and marking the aim DIRECTION is exact; the moment a lean
// moves the render eye off the shot eye the two disagree by the lean divided by
// the range, so a reticle placed on the direction agrees with the bullet at
// exactly one distance and splays either side of it. The camera hook casts along
// clean aim each frame and publishes how far it reached, and the impact point is
// what gets projected here.
//
// The engine's own point, at the crosshair component's world-point offset, is
// NOT that impact: it is the eye plus roughly one unit of aim direction, so its
// depth is a metre rather than the range to the target. It is used only as a
// direction, and only when there is no traced impact to use instead - a shot
// into open sky, or the aim trace switched off.
class ReticleHook {
public:
    ReticleHook();

    bool Install(const BuildProfile& profile, Mod& mod);

private:
    // arkIggyAS3Object::SetAS3Value<float>(object, value, member).
    using IggySetFloatFn = std::uint64_t(__fastcall*)(std::uintptr_t, const float*, void*);
    // arkComponent checked-pointer resolve: the component, or null once the
    // generation stamp no longer matches.
    using ResolveCpntFn = std::uintptr_t(__fastcall*)(void*, int);

    static void __fastcall DetourUpdate(void* element);

    // Corrects the reticle, or hands it back to the engine when this frame has
    // nothing to correct with.
    void Place(std::uintptr_t element);
    // True when the reticle was actually written this frame. Every way of
    // failing means the same thing to the caller - there is no corrected
    // position for this frame - so the handing back lives in Place alone.
    bool TryPlace(std::uintptr_t element);

    // Clears the engine's centred latch so it re-places the reticle itself, on
    // the first frame this hook stops correcting one it had moved.
    void Release(std::uintptr_t element);

    // Writes normalised device coordinates through the engine's own AS3 float
    // setter, scaled by the stage half-extents exactly as the engine's own
    // placement does.
    void Write(std::uintptr_t element, float ndcX, float ndcY);

    void LogPlacement(const char* source, float ndcX, float ndcY, float distance,
                      const float lean[3], const float point[3]);

    const BuildProfile* m_profile = nullptr;
    Mod* m_mod = nullptr;
    std::uintptr_t m_base = 0;

    // Resolved in Install and published to the detour thread by the same release
    // store that publishes this object.
    IggySetFloatFn m_iggySetFloat = nullptr;
    ResolveCpntFn m_resolveCpnt = nullptr;

    // The float the element compares the component's flag against to decide
    // between its two placements. Read out of the image rather than assumed to
    // be 1.0, because taking the opposite branch to the engine on every frame
    // has no symptom the log would show.
    float m_centredValue = 0.0f;

    // Whether the reticle currently sits where this hook put it, so the frame
    // that stops correcting can hand it back. Render-thread only, like the
    // placement itself.
    bool m_placed = false;

    LogThrottle m_placementLog;

    // The same interval [Diagnostics] CameraTraceMs drives on the camera side, so
    // one setting turns on both halves of what a reticle check needs: where the
    // camera put the eye, and where the reticle went as a result. Zero leaves
    // only the always-on trickle above.
    IntervalGate m_traceGate;
};

}  // namespace D2HT
