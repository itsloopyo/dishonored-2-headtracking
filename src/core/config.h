// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include "core/constants.h"

#include <cameraunlock/data/position_settings.h>
#include <cameraunlock/math/smoothing_utils.h>

#include <cstdint>
#include <string>

namespace D2HT {

struct Config {
    // Network
    uint16_t udp_port = 4242;

    // General
    bool enable_on_startup = true;

    // Rotation sensitivities (defaults all 1.0)
    float yaw_sensitivity = 1.0f;
    float pitch_sensitivity = 1.0f;
    float roll_sensitivity = 1.0f;
    bool invert_yaw = false;
    bool invert_pitch = false;
    bool invert_roll = false;

    // Smoothing 0..1, picked per connection from the packet source address and
    // covering both rotation and position. A tracker on this machine (loopback)
    // uses local_smoothing; a remote network device uses remote_smoothing.
    float local_smoothing = static_cast<float>(cameraunlock::math::kDefaultLocalSmoothing);
    float remote_smoothing = static_cast<float>(cameraunlock::math::kDefaultRemoteSmoothing);

    // Position (6DOF)
    bool position_enabled = true;
    float pos_sens_x = 1.0f;
    float pos_sens_y = 1.0f;
    float pos_sens_z = 1.0f;
    float pos_limit_x = cameraunlock::PositionSettings{}.limit_x;
    float pos_limit_y = cameraunlock::PositionSettings{}.limit_y;
    float pos_limit_z = cameraunlock::PositionSettings{}.limit_z;
    float pos_limit_z_back = cameraunlock::PositionSettings{}.limit_z_back;
    bool invert_pos_x = false;
    bool invert_pos_y = false;
    bool invert_pos_z = false;

    // Camera collision. The position limits stop the eye leaving the player's
    // body; they say nothing about the room it is standing in, and a 0.30m lean
    // into a doorframe puts the rendered eye inside the wood.
    //
    // Ships ON, which it only earns by having been watched doing it: with the
    // standoff temporarily widened to a metre, the log showed the clamp cut a
    // 0.30m lean to zero against a wall the aim line independently put at 0.78m,
    // and leave the same lean untouched at 1.40m, 1.95m and 3.77m. That is the
    // filter word collided with real level geometry at the real distance rather
    // than blocking on nothing or on everything.
    bool collision_enabled = true;
    // How far off a surface the eye is held, in metres. Has to EXCEED the
    // camera's near clip distance or the wall is culled and the player sees
    // through it anyway, which is the same complaint with extra steps. Checked
    // against r_znear at startup.
    float collision_margin = 0.15f;
    // The required-flags word the engine's filter predicate tests a body against.
    // 0x800 is what the game's own third-person camera collision uses for its
    // line check against the level.
    unsigned collision_channel = 0x800;
    // How quickly the allowance reopens once an obstruction clears. Tightening
    // is never smoothed; see cameraunlock::camera::LeanClamp.
    float collision_release_smoothing = 0.9f;

    // The reticle's aim trace: how far along clean aim the world stops the shot,
    // so the reticle can be drawn on that point rather than along the aim
    // direction alone. Without it the reticle is exact under rotation and drifts
    // under a lean, by the lean divided by the range to the target.
    bool aim_trace_enabled = true;
    // A required-flags mask, not a layer index. 0x800 is the flag the game's own
    // third-person camera collision line-checks against level geometry; it
    // shape-casts 0x20000 separately for characters, so whether a character body
    // also answers this line check is not established. Unconfirmed in game, and
    // the reason it is a setting rather than a constant.
    unsigned aim_trace_channel = 0x800;
    // How far along clean aim to look, in metres. Past this the reticle falls
    // back to projecting the aim direction, which is what a shot into open sky
    // wants anyway.
    float aim_trace_range = 200.0f;

    // Diagnostics: how often, in milliseconds, to write the camera line that
    // reports the clean and tracked view together with this frame's aim and lean.
    // 0 is off and is the shipped default; the line is what a wrong axis or a
    // clamp that has stopped clamping is read from.
    long camera_trace_ms = 0;

    // Yaw mode
    bool world_locked_yaw = false;

    // Hotkey VK codes
    int hotkey_toggle = DEFAULT_TOGGLE_KEY;
    int hotkey_tracking_mode = DEFAULT_TRACKING_MODE_KEY;
    int hotkey_yaw_mode = DEFAULT_YAW_MODE_KEY;

    // Reads the file, PARSING each value; a value whose text is not wholly a
    // number keeps the field's default and says so in the log. Range checking
    // is Sanitize's job, below.
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    // Range-checks the struct at the file-system trust boundary, whatever set
    // it. "nan"/"inf" parse as numbers and would poison every frame of
    // view-matrix math with NaN; a limit of 4 is a misplaced decimal point that
    // puts the render eye four metres in front of the player, one engine unit
    // being one metre here; a negative limit inverts the bounds of its own clamp
    // and pins the eye at a fixed offset; a port outside 1..65535 or a VK the OS
    // cannot poll silently stops the thing it configures from ever working.
    // Each correction names its key in the log.
    // Returns true if anything was corrected.
    bool Sanitize();
};

} // namespace D2HT
