// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/log_throttle.h"
#include "core/published_triple.h"

#include <cameraunlock/protocol/udp_receiver.h>
#include <cameraunlock/time/frame_clock.h>
#include <cameraunlock/tracking/head_tracking_session.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace D2HT {

struct BuildProfile;
class CameraHook;
class GameState;
class MarkerHook;
class ReticleHook;

// Why the frame's head pose is or is not applied. There is no "dormant" reason:
// an unmatched build profile installs no hook, so no frame ever reaches the
// verdict at all.
enum class GateReason {
    Gameplay,     // nothing suppresses tracking
    NotGameplay,  // the shell, a load, the pause menu, a cutscene, a screen
    Disabled,     // the player switched tracking off
};

class Mod {
public:
    static Mod& Instance();

    // Everything that can go wrong inside is reported and left dormant, so
    // there is no failure for a caller to act on.
    void Initialize();

    void SetEnabled(bool v);
    void Toggle();

    void CycleTrackingMode();
    void ToggleYawMode();
    bool IsWorldSpaceYaw() const { return m_worldLockedYaw.load(); }

    // Called once per rendered frame from the render-view hook, before the pose
    // is read. Runs the tracking-state verdict, feeds the pipeline and publishes
    // this frame's pose. Returns whether the pose reaches the camera at all.
    bool UpdateForFrame();

    // This frame's pose in the units the camera hook wants: radians, and engine
    // units along the clean basis rows forward / left / up.
    bool GetRotationRadians(float& yaw, float& pitch, float& roll) const;
    bool GetPositionOffset(float& forward, float& left, float& up) const;

    // The render-view hook, for the reticle: it owns the clean and tracked bases
    // the reticle projection is built from. Null until the hook installs.
    const CameraHook* Camera() const { return m_cameraHook.get(); }

    // Read once at startup and never written again, so the hooks can read it
    // without a copy of their own. Valid from Initialize onwards, which is
    // before any hook is installed.
    const Config& GetConfig() const { return m_config; }

    // How long the last rendered frame took, in seconds, already clamped. The
    // lean clamp's release ease needs it and the camera hook is inside the same
    // frame the clock was ticked for.
    float LastFrameSeconds() const { return m_lastFrameSeconds; }

    Mod(const Mod&) = delete;
    Mod& operator=(const Mod&) = delete;

private:
    Mod() = default;
    // Never destroyed - see Instance().
    ~Mod() = delete;

    bool LoadConfig();
    // Config -> session settings. Pure translation: no hooks, no sockets, no
    // threads, so it is the half of startup that touches nothing the player
    // can notice.
    void ApplyConfigToSession();
    void InstallEngineHooks(const BuildProfile& profile);
    void LogVerdictChange(GateReason reason);
    void LogTrackerConnection(bool receiving);
    void LogSmoothingSelection();
    void LogPose(float yawDeg, float pitchDeg, float rollDeg, float x, float y, float z,
                 bool havePosition);
    void PublishPose();

    // Frame dt is clamped to this ceiling so a stall (alt-tab, load hitch) cannot
    // feed a huge dt into the smoothing/extrapolation math.
    static constexpr float kMaxFrameDtSeconds = 0.25f;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_worldLockedYaw{false};

    Config m_config;
    cameraunlock::UdpReceiver m_udpReceiver;
    cameraunlock::HeadTrackingSession<cameraunlock::UdpReceiver> m_session{m_udpReceiver};
    cameraunlock::time::FrameClock m_frameClock{kMaxFrameDtSeconds};

    Hotkeys m_hotkeys;

    std::unique_ptr<CameraHook> m_cameraHook;
    std::unique_ptr<GameState> m_gameState;
    std::unique_ptr<ReticleHook> m_reticleHook;
    std::unique_ptr<MarkerHook> m_markerHook;

    // The mode the player has asked for, written by the hotkey thread, and the
    // one actually pushed into the session, touched only by the render thread.
    // UpdateForFrame reconciles them: core's SetMode also resets the position
    // processor's smoothing and the position interpolator, which are plain floats
    // the render thread may be inside at that moment.
    std::atomic<cameraunlock::TrackingMode> m_requestedMode{
        cameraunlock::TrackingMode::RotationAndPosition};
    cameraunlock::TrackingMode m_appliedMode = cameraunlock::TrackingMode::RotationAndPosition;

    // Written by UpdateForFrame and read by the camera hook later in the same
    // frame, on the same thread.
    float m_lastFrameSeconds = 0.0f;

    GateReason m_lastReason = GateReason::Gameplay;
    bool m_stateKnown = false;
    bool m_wasConnected = false;
    bool m_remoteConnectionKnown = false;
    bool m_isRemoteConnection = false;
    IntervalGate m_poseLogGate;
    unsigned m_poseLogLines = 0;

    // Radians, engine sign convention.
    PublishedTriple m_rotation;
    // Engine units, camera-local forward / left / up.
    PublishedTriple m_position;
};

} // namespace D2HT
