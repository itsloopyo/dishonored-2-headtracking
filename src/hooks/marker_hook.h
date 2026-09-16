// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <atomic>
#include <cstdint>

#include "core/log_throttle.h"
#include "hooks/build_profiles.h"
#include "voidengine/void_math.h"

namespace D2HT {

class Mod;

// Keeps HUD markers - objectives, runes, charms and the rest - on the things
// they mark while the head moves the view.
//
// Every marker is placed each frame by arkEntityMarker's update, which projects
// the marker's world point through the player's idView. That is the CLEAN view:
// the camera hook only puts the head pose into the render view for the duration
// of idRenderView::Setup. So the markers land where their targets would be with
// the head centred, and stay fixed on screen while the world turns under them.
//
// The fix is to let that one update see the tracked view and nothing else see
// it. Inside it, and only on the thread running it, the view's origin / axis
// accessor answers with the tracked view, and the two projections it uses are
// fed points rebased between the clean and the tracked view so the engine's own
// matrices, pixel scaling and depth convention do the work. The edge clamp, the
// behind-the-player test and the distance the marker is scaled by then all
// agree with the frame that is actually on screen. Game logic calling the same
// accessor from anywhere else still gets the clean view.
//
// The head pose is carried as a delta from the camera hook's last frame and put
// onto the view that is live when the marker update runs, so a marker does not
// trail the mouse by the frame between the two.
class MarkerHook {
public:
    bool Install(const BuildProfile& profile, Mod& mod);

private:
    static void __fastcall DetourUpdate(void* marker);
    static void __fastcall DetourWorldToScreen(void* view, const float* in, float* out);
    static void __fastcall DetourScreenToWorld(void* view, const float* in, float* out);
    static void __fastcall DetourOriginAxis(void* view, float* origin, float* axis);

    // This frame's head pose as a delta, or false when the camera drew no
    // tracked view and the markers need nothing.
    bool CaptureDelta(voidengine::ViewDelta& out);

    // The clean view an engine accessor handed back, and the tracked view built
    // on it. False, and reported once, when the clean basis is not one.
    struct Views {
        voidengine::Vec3 clean_origin;
        voidengine::Mat3 clean_axis;
        voidengine::Vec3 tracked_origin;
        voidengine::Mat3 tracked_axis;
    };
    bool BuildViews(const float cleanOrigin[3], const float cleanAxis[9],
                    const voidengine::ViewDelta& delta, Views& out);
    bool ReadViews(void* view, const voidengine::ViewDelta& delta, Views& out);

    // Whether this call's placement line is due. The clean pixel it prints costs
    // a second projection, so it is only computed on the calls that log.
    bool PlacementLogDue();

    Mod* m_mod = nullptr;

    std::atomic<bool> m_activeLogged{false};
    std::atomic<bool> m_refusalLogged{false};

    // Written only from the marker update's thread.
    LogThrottle m_placementLog{4, 16, 600, 4000};
    IntervalGate m_traceGate;
};

}  // namespace D2HT
