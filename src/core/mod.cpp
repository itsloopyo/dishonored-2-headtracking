// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "core/mod.h"
#include "core/game_window.h"
#include "core/logging.h"
#include "core/path_utils.h"
#include "hooks/build_profiles.h"
#include "hooks/camera_hook.h"
#include "hooks/game_state.h"
#include "hooks/marker_hook.h"
#include "hooks/reticle_hook.h"
#include "voidengine/void_math.h"

#include <cameraunlock/diagnostics/crash_handler.h>
#include <cameraunlock/hooks/hook_manager.h>
#include <cameraunlock/math/angle_utils.h>
#include <cameraunlock/math/smoothing_utils.h>

namespace D2HT {

namespace {

// The pose line's schedule: a dense opening run, where a wrong sign or an axis
// that never moves shows, then a thin trickle, which is what a late report -
// "it drifted after an hour" - is read from. See Mod::LogPose.
constexpr std::uint64_t kPoseEarlyIntervalMs = 1000;
constexpr std::uint64_t kPoseSteadyIntervalMs = 30000;
constexpr unsigned kPoseEarlyLines = 20;

// The three-state tracking-mode cycle, in TrackingMode order. The table's own
// length is the cycle length, so the names and the wrap cannot drift apart.
constexpr const char* kTrackingModeNames[] = {
    "6DOF (rotation + position)",
    "rotation only",
    "position only",
};
constexpr int kTrackingModeCount =
    static_cast<int>(sizeof(kTrackingModeNames) / sizeof(kTrackingModeNames[0]));

}  // namespace

// Leaked on purpose, and the destructor is deleted so it cannot be undone. A
// function-local static registers itself on the CRT's onexit table, which runs
// on DLL_PROCESS_DETACH whether or not the process is terminating - so ~Mod
// would join the hotkey and receiver threads under the loader lock, moments
// after the OS has forcibly killed them, possibly mid-allocation. The symptom
// is Dishonored2.exe never leaving Task Manager after the player quits. Nothing
// here owns a resource the OS does not reclaim at process exit.
Mod& Mod::Instance() {
    static Mod* const inst = new Mod();
    return *inst;
}

bool Mod::LoadConfig() {
    const std::string directory = PathUtils::GetModDirectory();
    if (directory.empty()) {
        // The mod directory has no ANSI form, and IniReader is an ANSI-only
        // API. Guessing a relative path here would read, and create, the ini in
        // whatever the launcher left as the working directory while the log
        // claimed it sat next to the game.
        Log::Line("WARN: the mod's install path cannot be expressed in this system's ANSI code "
                  "page, so %s cannot be read or written; running on built-in defaults",
                  CONFIG_FILENAME);
        return false;
    }
    const std::string path = directory + "\\" + CONFIG_FILENAME;
    if (!m_config.Load(path)) {
        // Reported on its own result, not on the attempt: a game installed
        // under Program Files without the mod folder being writable leaves the
        // player editing a file that was never written, with a log line saying
        // it was.
        if (m_config.Save(path)) {
            Log::Line("Wrote default config to %s", path.c_str());
        } else {
            Log::Line("WARN: no config at %s and it could not be written there; running on "
                      "built-in defaults, and any edit to that file will not be read",
                      path.c_str());
        }
        return false;
    }
    // Every correction has already named its own key and value on its own line,
    // so this only says how to make them stop.
    if (m_config.Sanitize()) {
        Log::Line("WARN: %s had values that could not be used; the lines above name each one, "
                  "and each is running on its default until the file is corrected",
                  CONFIG_FILENAME);
    }
    Log::Line("Loaded config from %s", path.c_str());
    return true;
}

void Mod::ApplyConfigToSession() {
    cameraunlock::TrackingProcessor& processor = m_session.GetProcessor();

    cameraunlock::SensitivitySettings sens;
    sens.yaw = m_config.yaw_sensitivity;
    sens.pitch = m_config.pitch_sensitivity;
    sens.roll = m_config.roll_sensitivity;
    sens.invert_yaw = m_config.invert_yaw;
    sens.invert_pitch = m_config.invert_pitch;
    sens.invert_roll = m_config.invert_roll;
    processor.SetSensitivity(sens);

    cameraunlock::PositionSettings posSettings = cameraunlock::PositionSettings::Default();
    posSettings.sensitivity_x = m_config.pos_sens_x;
    posSettings.sensitivity_y = m_config.pos_sens_y;
    posSettings.sensitivity_z = m_config.pos_sens_z;
    posSettings.limit_x = m_config.pos_limit_x;
    // The clamp is [-limit_y_down, +limit_y] and limit_y_down carries its own
    // default, so mirror the one configured vertical limit the way
    // PositionSettings::Symmetric does. Left unset, raising LimitY widened the
    // upward budget only and downward travel stayed pinned at 0.20m.
    posSettings.limit_y = m_config.pos_limit_y;
    posSettings.limit_y_down = m_config.pos_limit_y;
    posSettings.limit_z = m_config.pos_limit_z;
    posSettings.limit_z_back = m_config.pos_limit_z_back;
    posSettings.invert_x = m_config.invert_pos_x;
    posSettings.invert_y = m_config.invert_pos_y;
    posSettings.invert_z = m_config.invert_pos_z;

    // The session owns the two smoothing values and recomposes them onto the
    // struct, so this and the two SetSmoothing calls compose in either order.
    // Without IsRemoteConnection() on the receiver the selection silently pins to
    // local, so assert the trait.
    static_assert(decltype(m_session)::kHasRemoteConnection,
                  "receiver must expose IsRemoteConnection()");
    m_session.SetLocalSmoothing(m_config.local_smoothing);
    m_session.SetRemoteSmoothing(m_config.remote_smoothing);
    m_session.SetPositionSettings(posSettings);
    // Our trackers report head position directly, so core's synthetic pivot term -
    // which exists to cancel a webcam pivot - would only inject rotation-coupled
    // movement that is not there.
    m_session.GetPositionProcessor().SetTrackerPivotForward(0.0f);

    const auto startMode = m_config.position_enabled
                               ? cameraunlock::TrackingMode::RotationAndPosition
                               : cameraunlock::TrackingMode::RotationOnly;
    m_session.SetMode(startMode);
    m_requestedMode.store(startMode);
    m_appliedMode = startMode;

    m_worldLockedYaw.store(m_config.world_locked_yaw);
}

void Mod::Initialize() {
    LoadConfig();
    ApplyConfigToSession();

    m_udpReceiver.SetLog([](const std::string& msg) {
        Log::Line("UDP: %s", msg.c_str());
    });

    // Everything below this line modifies the running process in a way a player
    // can notice: a bound UDP port, a key poller, a hooked engine function. An
    // unrecognised build gets none of it - the build-profile doctrine is that a
    // mismatch leaves the game running vanilla.
    const BuildProfile* profile = SelectBuildProfile();
    if (profile == nullptr) return;

    // Below the gate, not above it. SetUnhandledExceptionFilter replaces a
    // process-wide handler, so installing it before the fingerprint matched
    // would leave an unrecognised build with the mod owning the game's
    // last-chance crash path - process modification on exactly the build the
    // dormancy rule promises to leave alone, and a crash report with our name
    // on it for a fault that is not ours.
    cameraunlock::diagnostics::InstallCrashHandler();

    // A false return means the bind failed; the receiver keeps a background
    // supervisor thread alive and binds once the port frees up. Start has
    // already logged what the OS said about the failure through the sink above,
    // so this line must not name a cause of its own: a port conflict is the
    // usual reason but not the only one, and a WSAEACCES from a reserved port
    // range sends a user hunting an app that is not running.
    if (m_udpReceiver.Start(m_config.udp_port)) {
        Log::Line("UDP receiver bound on port %u", m_config.udp_port);
    } else {
        Log::Line("WARN: UDP bind on %u deferred - see the reason above; retrying in "
                  "the background until it succeeds", m_config.udp_port);
    }

    InstallEngineHooks(*profile);

    m_hotkeys.Start(m_config);

    SetEnabled(m_config.enable_on_startup);

    // Last, and on this thread: centring waits for the window to appear and
    // stop resizing, and everything the player is waiting on is already armed.
    CenterGameWindowOnce();
}

void Mod::InstallEngineHooks(const BuildProfile& profile) {
    using cameraunlock::hooks::HookManager;
    using cameraunlock::hooks::HookStatus;
    using cameraunlock::hooks::HookStatusToString;

    const HookStatus status = HookManager::Instance().Initialize();
    if (status != HookStatus::Ok) {
        Log::Line("ERROR: MinHook initialization failed: %s; the mod is dormant and the game is "
                  "unmodified", HookStatusToString(status));
        return;
    }

    m_cameraHook = std::make_unique<CameraHook>();
    if (!m_cameraHook->Install(profile, *this)) {
        m_cameraHook.reset();
        return;
    }

    // Built only once the camera hook has installed, because building it starts a
    // thread that reads every committed region of the process every three seconds
    // until it finds its singleton. At the shell there is no live idGameLocal to
    // find, so it sweeps for as long as the player sits there - the most
    // expensive thing the mod does, and with a failed hook it would be running
    // immediately after the line above said the mod was dormant.
    m_gameState = std::make_unique<GameState>(profile);

    m_reticleHook = std::make_unique<ReticleHook>();
    if (!m_reticleHook->Install(profile, *this)) {
        m_reticleHook.reset();
    }

    m_markerHook = std::make_unique<MarkerHook>();
    if (!m_markerHook->Install(profile, *this)) {
        m_markerHook.reset();
    }

    // And the detour is armed last, because it reads m_gameState on its very
    // first frame. Arming inside Install would race the assignment above.
    m_cameraHook->Arm();
}

void Mod::SetEnabled(bool v) {
    bool was = m_enabled.exchange(v);
    if (was != v) {
        Log::Line("Head tracking %s", v ? "ENABLED" : "DISABLED");
    }
}

void Mod::Toggle() { SetEnabled(!m_enabled.load()); }

void Mod::CycleTrackingMode() {
    // Core's HeadTrackingSession::CycleMode owns this rule and cannot be called
    // from here: it goes through SetMode, which resets non-atomic processor state
    // under the render thread. The hotkey thread only arms a request; the render
    // thread applies it at the top of UpdateForFrame.
    static_assert(static_cast<int>(cameraunlock::TrackingMode::RotationAndPosition) == 0,
                  "core reordered TrackingMode; the restated cycle rule is now wrong");
    static_assert(static_cast<int>(cameraunlock::TrackingMode::RotationOnly) == 1,
                  "core reordered TrackingMode; the restated cycle rule is now wrong");
    static_assert(static_cast<int>(cameraunlock::TrackingMode::PositionOnly) == 2,
                  "core reordered TrackingMode; the restated cycle rule is now wrong");
    const auto next = static_cast<cameraunlock::TrackingMode>(
        (static_cast<int>(m_requestedMode.load()) + 1) % kTrackingModeCount);
    m_requestedMode.store(next);
    Log::Line("Tracking mode: %s", kTrackingModeNames[static_cast<int>(next)]);
}

void Mod::ToggleYawMode() {
    bool v = !m_worldLockedYaw.load();
    m_worldLockedYaw.store(v);
    Log::Line("Yaw mode: %s", v ? "world-locked" : "camera-local");
}

void Mod::LogVerdictChange(GateReason reason) {
    // The first evaluation is reported as well as every change. Reporting only
    // changes leaves a session that never reaches gameplay with nothing in the
    // log to say why - which is the one case a "no head tracking" report needs.
    if (m_stateKnown && reason == m_lastReason) return;
    m_stateKnown = true;
    m_lastReason = reason;

    // The gate's own words for everything it decided itself, and the engine's for
    // the one answer it did not: "not in gameplay" is true of the shell, a level
    // load, a cutscene and the pause menu alike, and which of those it is is the
    // whole content of a "no head tracking" report. NotGameplay is only reached
    // with a game state to ask, because that verdict comes from asking it.
    switch (reason) {
        case GateReason::Gameplay:
            Log::Line("[state] gameplay");
            return;
        case GateReason::NotGameplay:
            Log::Line("[state] tracking suspended (%s)", m_gameState->LastReason());
            return;
        case GateReason::Disabled:
            Log::Line("[state] tracking suspended (tracking switched off)");
            return;
    }
}

bool Mod::UpdateForFrame() {
    // The hotkey thread only ever stores an enum. Applying it is the render
    // thread's job, because SetMode also resets the position processor's
    // smoothing and the position interpolator.
    const auto requested = m_requestedMode.load();
    if (requested != m_appliedMode) {
        m_session.SetMode(requested);
        m_appliedMode = requested;
        // The half the new mode stops publishing has to go with it. Both triples
        // are only ever rewritten from PublishPose, which a tracker gap skips
        // entirely so the last pose is held - correct on its own, and wrong
        // here: a player who loses the tracker mid-lean and presses Page Up to
        // get rid of it would otherwise keep the lean until packets resume.
        if (requested == cameraunlock::TrackingMode::RotationOnly) m_position.Invalidate();
        if (requested == cameraunlock::TrackingMode::PositionOnly) m_rotation.Invalidate();
    }

    GateReason reason = GateReason::Gameplay;
    if (!m_gameState->IsGameplay()) {
        reason = GateReason::NotGameplay;
    } else if (!m_enabled.load()) {
        reason = GateReason::Disabled;
    }
    LogVerdictChange(reason);

    // Connection state is tracked whether or not tracking is active, so a player
    // who opens the pause menu with a tracker running still gets the connect and
    // disconnect lines.
    const bool receiving = m_udpReceiver.IsReceiving();
    LogTrackerConnection(receiving);

    // Ticked every frame, before any early return. FrameClock reports the time
    // since the LAST tick and clamps it, so skipping it through a gap hands the
    // first resumed frame a dt orders of magnitude too large - and at that dt the
    // smoothing factor converges the whole gap's worth of head movement in a
    // single frame.
    const float dt = m_frameClock.Tick();
    m_lastFrameSeconds = dt;

    if (reason != GateReason::Gameplay) {
        // A closed gate is a deliberate suppression - a menu, a cutscene, the
        // master toggle - so the pose goes away and the frame renders from the
        // game's own view.
        m_rotation.Invalidate();
        m_position.Invalidate();
        return false;
    }

    // A tracker gap is NOT a suppression. Dropping the pose because the last
    // packet is older than the freshness window snaps the view to the untracked
    // orientation and whips it back when packets resume, twice per dropout, and a
    // webcam tracker losing the face for half a second is the commonest thing
    // that happens to one. Doctrine is to hold the last known pose, so the
    // published pose is simply left standing.
    if (!receiving) return true;

    if (!m_session.Update(dt)) return true;

    LogSmoothingSelection();
    PublishPose();
    return true;
}

void Mod::LogTrackerConnection(bool receiving) {
    if (receiving == m_wasConnected) return;
    m_wasConnected = receiving;
    Log::Line(receiving ? "[tracker] source connected on UDP %u"
                        : "[tracker] source disconnected (no packets within the freshness "
                          "window) on UDP %u",
              m_config.udp_port);
}

// Which of the two smoothing values the packet source picked, on the first
// frame it is known and on every change after: a user who moves a tracker
// between this machine and the network gets the other value without a restart,
// and this is the only line that says which one is in force.
void Mod::LogSmoothingSelection() {
    const bool isRemote = m_session.IsRemoteConnection();
    if (m_remoteConnectionKnown && isRemote == m_isRemoteConnection) return;
    m_isRemoteConnection = isRemote;
    m_remoteConnectionKnown = true;
    Log::Line("[tracker] source is %s, smoothing=%.3f", isRemote ? "remote" : "local",
              cameraunlock::math::GetEffectiveSmoothing(m_session.GetLocalSmoothing(),
                                                        m_session.GetRemoteSmoothing(),
                                                        isRemote));
}

// The pose as the tracker sent it: the only line that separates "the tracker is
// not sending" from "the tracker is sending and the camera is not moving", which
// are the two halves of every "head tracking does nothing" report. Dense while a
// mirrored axis or a dead one would show, then thin for the rest of the session -
// a whole session at one line a second is a megabyte of pose and buries every
// line that says what went wrong.
void Mod::LogPose(float yawDeg, float pitchDeg, float rollDeg, float x, float y, float z,
                  bool havePosition) {
    const std::uint64_t interval =
        m_poseLogLines < kPoseEarlyLines ? kPoseEarlyIntervalMs : kPoseSteadyIntervalMs;
    if (!m_poseLogGate.Due(GetTickCount64(), interval)) return;
    ++m_poseLogLines;
    Log::Line("[pose] yaw=%.2f pitch=%.2f roll=%.2f x=%.3f y=%.3f z=%.3f%s", yawDeg, pitchDeg,
              rollDeg, x, y, z, havePosition ? "" : " (rotation only)");
}

void Mod::PublishPose() {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    m_session.GetRotation(yawDeg, pitchDeg, rollDeg);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    const bool havePosition = m_session.GetPositionOffset(x, y, z);

    LogPose(yawDeg, pitchDeg, rollDeg, x, y, z, havePosition);

    // The one place the tracker's convention becomes the engine's. The
    // conversion itself lives in voidengine::PoseToEngine, where every sign and
    // the unit scale are stated together and can be tested.
    const voidengine::EnginePose pose =
        voidengine::PoseToEngine(yawDeg, pitchDeg, rollDeg, x, y, z);
    m_rotation.Publish(pose.yaw, pose.pitch, pose.roll, true);

    if (!havePosition) {
        m_position.Invalidate();
        return;
    }
    m_position.Publish(pose.forward, pose.left, pose.up, true);
}

bool Mod::GetRotationRadians(float& yaw, float& pitch, float& roll) const {
    return m_rotation.Read(yaw, pitch, roll);
}

bool Mod::GetPositionOffset(float& forward, float& left, float& up) const {
    return m_position.Read(forward, left, up);
}

} // namespace D2HT
