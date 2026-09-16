// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "core/config.h"

#include "core/logging.h"

#include <cameraunlock/config/ini_reader.h>
#include <cameraunlock/config/value_guards.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace D2HT {

namespace {

// printf-style sink for core's guards, so their diagnostics land in our log
// under the mod's own prefix. Formatted here rather than forwarded, because the
// log exposes no va_list entry point.
void GuardLog(const char* fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    Log::Line("WARN: %s", message);
}

// How many keys this load actually got text for.
//
// IniReader::Open only asks whether the file EXISTS; every read after it goes
// through GetPrivateProfileString, which returns the empty default on any
// failure. So a file that is present but unreadable - denied by an ACL, held
// open by another process without FILE_SHARE_READ, a bad sector - is
// indistinguishable from a file with no keys in it, and every setting silently
// falls back to its default while the caller logs "Loaded config from ...".
// Counting the keys that produced text is what separates the two.
int g_keysRead = 0;

std::string RawValue(const cameraunlock::IniReader& ini, const char* section, const char* key) {
    std::string raw = cameraunlock::config::ReadRawValue(ini, section, key);
    if (!raw.empty()) ++g_keysRead;
    return raw;
}

// Every numeric key is parsed WHOLE. IniReader's own readers are strtod and
// strtol, which take a prefix and report nothing, and the prefix a mistyped key
// parses to is always a plausible number: "0,15" for a European decimal comma
// reads as 0.0 and passes every range check there is, so a phone-over-WiFi user
// gets no smoothing with nothing in the log, and "LimitZ=0,4" kills the forward
// lean the same way. A refusal keeps the field's default and names the text.
bool ReadFloatWhole(const cameraunlock::IniReader& ini, const char* section, const char* key,
                    float& value) {
    const std::string raw = RawValue(ini, section, key);
    if (raw.empty()) return true;
    float parsed = 0.0f;
    if (!cameraunlock::config::ParseFloatStrict(raw, parsed)) {
        Log::Line("WARN: config: [%s] %s=%s is not a number, so the default %g is kept. Use a dot "
                  "for the decimal point.", section, key, raw.c_str(), static_cast<double>(value));
        return false;
    }
    value = parsed;
    return true;
}

// Same rule for a decimal integer.
bool ReadIntWhole(const cameraunlock::IniReader& ini, const char* section, const char* key,
                  long& value) {
    const std::string raw = RawValue(ini, section, key);
    if (raw.empty()) return true;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    // ERANGE as well as the two parse failures. A number too wide for the field
    // does not fail: strtol clamps it to LONG_MIN or LONG_MAX and still consumes
    // the whole string, so it arrives looking exactly like a value the user
    // meant. CameraTraceMs=99999999999999999999 would be taken as an interval of
    // LONG_MAX milliseconds - the diagnostic trace switched off by arithmetic
    // rather than by choice, with nothing in the log to say so.
    if (end == raw.c_str() || *end != '\0' || errno == ERANGE) {
        Log::Line("WARN: config: [%s] %s=%s is not a whole number this setting can hold, so the "
                  "default %ld is kept.", section, key, raw.c_str(), value);
        return false;
    }
    value = parsed;
    return true;
}

// The digits of a hex value, whole. Shared by the two keys written in hex - a
// virtual-key code and a collision filter word - because the parse is the same
// and only what a wrong one costs the user differs, which is what the two
// callers' own messages say. False leaves @p parsed untouched.
//
// The prefix parse this replaces is what let a key NAME through: this file's own
// comment says "End=Toggle", so "Toggle=End" is the natural thing to write back,
// and strtol reads it as 0x0E - an unassigned VK that passes every range check
// and simply never fires, with the log reporting a healthy "toggle=0x0E".
// "Delete" lands on 0xDE, the apostrophe key, and binds the action to that.
//
// A sign and a value too wide for the field are refused alongside the two parse
// failures, because strtoul reports neither. It APPLIES a leading minus to the
// unsigned result and consumes the whole string doing it, so "-1" comes back as
// 0xFFFFFFFF with every check passed; an overflow clamps to the maximum the same
// way, with only errno to say so. Neither is caught downstream: a virtual key is
// range-checked by Sanitize, but a filter word is a bit pattern and there is no
// range to check it against. CollisionChannel=-1 would leave the lean clamp
// asking the engine for a body carrying every flag there is, which nothing
// carries - so it queries every frame, never blocks, and the log reports the
// collision as on and the lean as unrestricted.
bool ParseHexWhole(const std::string& raw, std::uint32_t& parsed) {
    const char* digits = raw.c_str();
    if (*digits == '-' || *digits == '+') return false;
    if (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) digits += 2;
    if (*digits == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(digits, &end, 16);
    if (end == digits || *end != '\0') return false;
    if (errno == ERANGE || value > 0xFFFFFFFFull) return false;
    parsed = static_cast<std::uint32_t>(value);
    return true;
}

// A virtual-key code.
bool ReadHexWhole(const cameraunlock::IniReader& ini, const char* section, const char* key,
                  int& value) {
    const std::string raw = RawValue(ini, section, key);
    if (raw.empty()) return true;
    std::uint32_t parsed = 0;
    if (!ParseHexWhole(raw, parsed)) {
        Log::Line("WARN: config: [%s] %s=%s is not a hex virtual-key code, so the default 0x%02X "
                  "is kept. Write the code itself, as in 0x23, never the name of the key.",
                  section, key, raw.c_str(), value);
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

// And for a true/false key, which IniReader matches against a fixed token list
// and silently defaults on anything else. That is the same fault as the prefix
// parse above and it reads the same way to a user: "InvertPitch=true ; my
// webcam is upside down" keeps the trailing comment, matches nothing, and the
// setting stays off with nothing in the log.
bool ReadBoolWhole(const cameraunlock::IniReader& ini, const char* section, const char* key,
                   bool& value) {
    const std::string raw = RawValue(ini, section, key);
    if (raw.empty()) return true;
    std::string lowered;
    lowered.reserve(raw.size());
    for (char c : raw) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        value = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        value = false;
        return true;
    }
    Log::Line("WARN: config: [%s] %s=%s is not a yes/no value, so the default %d is kept. Write 1 "
              "or 0.", section, key, raw.c_str(), value ? 1 : 0);
    return false;
}

// A collision filter word, written in hex like a virtual key but not one: it is
// a bit mask the engine tests a body's own flags against, so a decimal reading
// of "0x800" as 0 would silently widen the query to everything the layer mask
// lets through rather than failing.
bool ReadHexMaskWhole(const cameraunlock::IniReader& ini, const char* section, const char* key,
                      unsigned& value) {
    const std::string raw = RawValue(ini, section, key);
    if (raw.empty()) return true;
    std::uint32_t parsed = 0;
    if (!ParseHexWhole(raw, parsed)) {
        Log::Line("WARN: config: [%s] %s=%s is not a hex collision filter word, so the default "
                  "0x%X is kept.", section, key, raw.c_str(), value);
        return false;
    }
    value = static_cast<unsigned>(parsed);
    return true;
}

// Past this an aim line is longer than any Dishonored 2 level, so a value beyond
// it is a typo rather than a preference, and a huge one costs a longer query
// every frame for a reticle that would land in the skybox anyway.
constexpr float kMaxAimTraceRangeMetres = 10000.0f;

}  // namespace

bool Config::Load(const std::string& path) {
    cameraunlock::IniReader ini;
    if (!ini.Open(path)) return false;
    g_keysRead = 0;

    // Struct member defaults double as the read defaults, so missing keys keep
    // their documented values without a second constant table.
    //
    // The port is read as a long and narrowed only once it is known to fit.
    // Narrowing first wraps a mistyped port into a different valid one -
    // UdpPort=70000 binds 4464, and the tracker never arrives with nothing in
    // the log to say why. Out of range lands as 0, which Sanitize reports.
    long port = udp_port;
    ReadIntWhole(ini, "Network", "UdpPort", port);
    udp_port = (port > 0 && port <= 0xFFFF) ? static_cast<uint16_t>(port) : 0;

    ReadBoolWhole(ini, "General", "EnableOnStartup", enable_on_startup);

    ReadFloatWhole(ini, "Rotation", "YawSensitivity", yaw_sensitivity);
    ReadFloatWhole(ini, "Rotation", "PitchSensitivity", pitch_sensitivity);
    ReadFloatWhole(ini, "Rotation", "RollSensitivity", roll_sensitivity);
    ReadBoolWhole(ini, "Rotation", "InvertYaw", invert_yaw);
    ReadBoolWhole(ini, "Rotation", "InvertPitch", invert_pitch);
    ReadBoolWhole(ini, "Rotation", "InvertRoll", invert_roll);
    ReadFloatWhole(ini, "Rotation", "LocalSmoothing", local_smoothing);
    ReadFloatWhole(ini, "Rotation", "RemoteSmoothing", remote_smoothing);
    ReadBoolWhole(ini, "Rotation", "WorldLockedYaw", world_locked_yaw);
    cameraunlock::config::WarnRetiredSmoothingKey(ini, "Rotation", "Smoothing", &GuardLog);

    ReadBoolWhole(ini, "Position", "Enabled", position_enabled);
    ReadFloatWhole(ini, "Position", "SensitivityX", pos_sens_x);
    ReadFloatWhole(ini, "Position", "SensitivityY", pos_sens_y);
    ReadFloatWhole(ini, "Position", "SensitivityZ", pos_sens_z);
    ReadFloatWhole(ini, "Position", "LimitX", pos_limit_x);
    ReadFloatWhole(ini, "Position", "LimitY", pos_limit_y);
    ReadFloatWhole(ini, "Position", "LimitZ", pos_limit_z);
    ReadFloatWhole(ini, "Position", "LimitZBack", pos_limit_z_back);
    ReadBoolWhole(ini, "Position", "InvertX", invert_pos_x);
    ReadBoolWhole(ini, "Position", "InvertY", invert_pos_y);
    ReadBoolWhole(ini, "Position", "InvertZ", invert_pos_z);
    ReadBoolWhole(ini, "Position", "CollisionEnabled", collision_enabled);
    ReadFloatWhole(ini, "Position", "CollisionMargin", collision_margin);
    ReadHexMaskWhole(ini, "Position", "CollisionChannel", collision_channel);
    ReadFloatWhole(ini, "Position", "CollisionReleaseSmoothing", collision_release_smoothing);
    cameraunlock::config::WarnRetiredSmoothingKey(ini, "Position", "Smoothing", &GuardLog);

    ReadBoolWhole(ini, "Reticle", "AimTraceEnabled", aim_trace_enabled);
    ReadHexMaskWhole(ini, "Reticle", "AimTraceChannel", aim_trace_channel);
    ReadFloatWhole(ini, "Reticle", "AimTraceRange", aim_trace_range);

    long trace_ms = camera_trace_ms;
    ReadIntWhole(ini, "Diagnostics", "CameraTraceMs", trace_ms);
    camera_trace_ms = trace_ms;

    ReadHexWhole(ini, "Hotkeys", "Toggle", hotkey_toggle);
    ReadHexWhole(ini, "Hotkeys", "TrackingMode", hotkey_tracking_mode);
    ReadHexWhole(ini, "Hotkeys", "YawMode", hotkey_yaw_mode);

    // The file is there and not one key came back with text. An empty config is
    // legal - every setting has a default - but a file the player has edited
    // that reads as empty is a file we could not read, and saying "Loaded
    // config" over the top of that is the one thing that stops them finding it.
    // WarnRetiredSmoothingKey reads through IniReader directly rather than
    // through RawValue, so a file carrying ONLY a retired key would look
    // unreadable to the counter when it plainly was read. Counting the retired
    // keys here closes that: the values are discarded, only the fact that the
    // file yielded text matters.
    RawValue(ini, "Rotation", "Smoothing");
    RawValue(ini, "Position", "Smoothing");

    if (g_keysRead == 0) {
        Log::Line("WARN: %s exists but no settings could be read from it, so every one is running "
                  "on its built-in default. If you have edited that file, check it is not empty "
                  "and that the game can read it.", path.c_str());
    }

    return true;
}

bool Config::Save(const std::string& path) const {
    cameraunlock::IniWriter w;
    if (!w.Open(path)) return false;

    w.WriteComment("Dishonored 2 Head Tracking config.");
    w.WriteComment("Read once, at startup: restart the game after editing it. Delete this file");
    w.WriteComment("and a fresh default copy is written on the next launch.");
    w.WriteComment("A trailing comment is fine on any line: everything from the first ; or #");
    w.WriteComment("onwards is stripped before the value is read.");
    w.WriteComment("A number has to be written whole, with a dot for the decimal point, and a");
    w.WriteComment("hotkey as a code rather than a key name. Anything else keeps the default");
    w.WriteComment("and says so in HeadTracking.log, naming the key and what was written.");
    w.WriteBlankLine();

    w.WriteSection("Network");
    w.WriteComment("The UDP port the tracker sends to. OpenTrack's default is 4242.");
    w.WriteInt("UdpPort", udp_port);
    w.WriteBlankLine();

    w.WriteSection("General");
    w.WriteComment("Start with tracking switched on. End (or Ctrl+Shift+Y) toggles it in game.");
    w.WriteBool("EnableOnStartup", enable_on_startup);
    w.WriteBlankLine();

    w.WriteSection("Rotation");
    w.WriteComment("Per-axis multipliers on the pose the tracker sends. 1.0 is 1:1.");
    w.WriteDouble("YawSensitivity", yaw_sensitivity);
    w.WriteDouble("PitchSensitivity", pitch_sensitivity);
    w.WriteDouble("RollSensitivity", roll_sensitivity);
    w.WriteComment("Flip an axis that tracks backwards.");
    w.WriteBool("InvertYaw", invert_yaw);
    w.WriteBool("InvertPitch", invert_pitch);
    w.WriteBool("InvertRoll", invert_roll);
    w.WriteComment("Smoothing is chosen per connection and covers rotation and position. Which");
    w.WriteComment("of the two applies is decided by the address the packets arrive from, so a");
    w.WriteComment("tracker on this PC sending to a LAN address instead of 127.0.0.1 counts as");
    w.WriteComment("remote. 0 = none, 1 = heavy.");
    w.WriteComment("LocalSmoothing: packets from 127.0.0.1 on this machine.");
    w.WriteDouble("LocalSmoothing", local_smoothing);
    w.WriteComment("RemoteSmoothing: packets from any other address.");
    w.WriteDouble("RemoteSmoothing", remote_smoothing);
    w.WriteComment("1 = yaw turns about the world's up axis, 0 = about the camera's own.");
    w.WriteComment("Page Down (or Ctrl+Shift+H) switches it in game.");
    w.WriteBool("WorldLockedYaw", world_locked_yaw);
    w.WriteBlankLine();

    w.WriteSection("Position");
    w.WriteComment("Which mode the mod STARTS in: 1 is rotation and position, 0 is rotation");
    w.WriteComment("only. Page Up (or Ctrl+Shift+G) cycles all three modes whatever this says.");
    w.WriteBool("Enabled", position_enabled);
    w.WriteComment("Per-axis multipliers on the head position the tracker sends. 1.0 is 1:1.");
    w.WriteDouble("SensitivityX", pos_sens_x);
    w.WriteDouble("SensitivityY", pos_sens_y);
    w.WriteDouble("SensitivityZ", pos_sens_z);
    w.WriteComment("How far the eye may travel, in metres. LimitX and LimitY are symmetric.");
    w.WriteDouble("LimitX", pos_limit_x);
    w.WriteDouble("LimitY", pos_limit_y);
    w.WriteComment("LimitZ is the forward lean and LimitZBack the backward one. They differ so");
    w.WriteComment("that leaning back does not pull the eye into the player's own body.");
    w.WriteDouble("LimitZ", pos_limit_z);
    w.WriteDouble("LimitZBack", pos_limit_z_back);
    w.WriteComment("Flip an axis that leans the wrong way.");
    w.WriteBool("InvertX", invert_pos_x);
    w.WriteBool("InvertY", invert_pos_y);
    w.WriteComment("InvertZ is for a tracker that sends depth backwards, not for a lean that");
    w.WriteComment("feels reversed. It is applied before the LimitZ / LimitZBack clamp, so");
    w.WriteComment("turning it on also swaps the travel budgets to 0.10m forward and 0.40m back.");
    w.WriteBool("InvertZ", invert_pos_z);
    w.WriteComment("Camera collision: stop a lean putting the view inside a wall. It runs the");
    w.WriteComment("engine's own line check from the un-leaned eye toward where the head wants");
    w.WriteComment("to go and cuts the lean to whatever the room leaves.");
    w.WriteBool("CollisionEnabled", collision_enabled);
    w.WriteComment("How far off a surface the eye is held, in metres. This has to be larger");
    w.WriteComment("than the camera's near clip distance or the wall is not drawn anyway.");
    w.WriteDouble("CollisionMargin", collision_margin);
    w.WriteComment("Which bodies the check collides with, as the hex flag word the engine's");
    w.WriteComment("own camera collision uses. 0 collides with everything the query allows.");
    w.WriteHex("CollisionChannel", static_cast<int>(collision_channel));
    w.WriteComment("How quickly the lean reopens once the obstruction clears. Tightening is");
    w.WriteComment("never smoothed. 0 = instant, 1 = very slow.");
    w.WriteDouble("CollisionReleaseSmoothing", collision_release_smoothing);
    w.WriteBlankLine();

    w.WriteSection("Reticle");
    w.WriteComment("Look along the aim line for what the shot will hit, and draw the reticle");
    w.WriteComment("on that point. Turn this off and the reticle marks the aim DIRECTION,");
    w.WriteComment("which is exact when you turn your head and drifts when you lean.");
    w.WriteBool("AimTraceEnabled", aim_trace_enabled);
    w.WriteComment("Which bodies the aim line stops on, as a hex flag word.");
    w.WriteHex("AimTraceChannel", static_cast<int>(aim_trace_channel));
    w.WriteComment("How far along the aim line to look, in metres.");
    w.WriteDouble("AimTraceRange", aim_trace_range);
    w.WriteBlankLine();

    w.WriteSection("Diagnostics");
    w.WriteComment("Write a camera line to HeadTracking.log this often, in milliseconds, saying");
    w.WriteComment("what the view was before and after the head pose, how far the aim line");
    w.WriteComment("reached and what the lean clamp did. 0 turns it off. Use 250 while");
    w.WriteComment("checking an axis or the camera collision, then set it back to 0.");
    w.WriteInt("CameraTraceMs", static_cast<int>(camera_trace_ms));
    w.WriteBlankLine();

    w.WriteSection("Hotkeys");
    w.WriteComment("Virtual-key codes in hex, as in 0x23: the code itself, never the name of");
    w.WriteComment("the key. Defaults are End=Toggle, PgUp=TrackingMode, PgDn=YawMode. The");
    w.WriteComment("Ctrl+Shift+Y/G/H chord alternatives are always active whatever these say.");
    w.WriteHex("Toggle", hotkey_toggle);
    w.WriteHex("TrackingMode", hotkey_tracking_mode);
    w.WriteHex("YawMode", hotkey_yaw_mode);

    w.Close();
    return true;
}

bool Config::Sanitize() {
    const Config def;
    bool changed = false;

    using Guard = float (*)(const char*, float, float, cameraunlock::config::LogSink);
    auto guard = [&](const char* key, float& v, float fallback, Guard fn) {
        const float corrected = fn(key, v, fallback, &GuardLog);
        if (!(corrected == v)) { v = corrected; changed = true; }
    };
    auto sensitivity = [&](const char* key, float& v, float fallback) {
        guard(key, v, fallback, &cameraunlock::config::SanitizeSensitivity);
    };
    // Validation only (NaN/Inf reject, [0,1] range). Not a floor: an in-range
    // value passes through exactly as the user set it.
    auto smoothing = [&](const char* key, float& v, float fallback) {
        guard(key, v, fallback, &cameraunlock::config::SanitizeSmoothing);
    };
    // Clamped at both ends, not only below zero. One engine unit is one metre
    // here, so LimitZ=4 - a misplaced decimal point for 0.4, and the likeliest
    // edit anyone makes to this file - is finite, positive, and puts the
    // rendered eye four metres in front of the player, through whatever is
    // there. A negative limit inverts the bounds of its own clamp and pins the
    // eye at a fixed offset instead.
    auto limit = [&](const char* key, float& v, float fallback) {
        guard(key, v, fallback, &cameraunlock::config::SanitizePositionLimit);
    };
    // A VK the OS cannot poll, or a modifier the chord guard is already
    // watching, is a binding that can never fire or one that fires on every
    // chord. Either way the key silently does nothing.
    auto vk = [&](const char* key, int& v, int fallback) {
        if (cameraunlock::config::IsBindableVirtualKey(v)) return;
        Log::Line("WARN: config: [Hotkeys] %s=0x%02X is not a key that can be bound, so the "
                  "default 0x%02X is used instead.", key, v, fallback);
        v = fallback;
        changed = true;
    };

    // Port 0 binds an ephemeral port, which the tracker can never reach.
    if (udp_port == 0) {
        Log::Line("WARN: config: [Network] UdpPort is outside 1-65535, so the default %u is used "
                  "instead.", def.udp_port);
        udp_port = def.udp_port;
        changed = true;
    }

    sensitivity("[Rotation] YawSensitivity", yaw_sensitivity, def.yaw_sensitivity);
    sensitivity("[Rotation] PitchSensitivity", pitch_sensitivity, def.pitch_sensitivity);
    sensitivity("[Rotation] RollSensitivity", roll_sensitivity, def.roll_sensitivity);

    smoothing("[Rotation] LocalSmoothing", local_smoothing, def.local_smoothing);
    smoothing("[Rotation] RemoteSmoothing", remote_smoothing, def.remote_smoothing);

    sensitivity("[Position] SensitivityX", pos_sens_x, def.pos_sens_x);
    sensitivity("[Position] SensitivityY", pos_sens_y, def.pos_sens_y);
    sensitivity("[Position] SensitivityZ", pos_sens_z, def.pos_sens_z);

    limit("[Position] LimitX", pos_limit_x, def.pos_limit_x);
    limit("[Position] LimitY", pos_limit_y, def.pos_limit_y);
    limit("[Position] LimitZ", pos_limit_z, def.pos_limit_z);
    limit("[Position] LimitZBack", pos_limit_z_back, def.pos_limit_z_back);
    // The standoff is a distance in the same metres the limits are in, so the
    // same guard catches the same mistakes: a NaN, a negative that would hold
    // the eye INSIDE the surface, and a misplaced decimal point that would hold
    // it a room and a half away from every wall.
    limit("[Position] CollisionMargin", collision_margin, def.collision_margin);
    smoothing("[Position] CollisionReleaseSmoothing", collision_release_smoothing,
              def.collision_release_smoothing);
    // NOT the position-limit guard: that one caps at the half-metre a head can
    // lean, and this is a distance across a level.
    if (!(aim_trace_range > 0.0f) || !std::isfinite(aim_trace_range) ||
        aim_trace_range > kMaxAimTraceRangeMetres) {
        Log::Line("WARN: config: [Reticle] AimTraceRange=%g is outside 0 to %g metres, so the "
                  "default %g is used instead.", static_cast<double>(aim_trace_range),
                  static_cast<double>(kMaxAimTraceRangeMetres),
                  static_cast<double>(def.aim_trace_range));
        aim_trace_range = def.aim_trace_range;
        changed = true;
    }

    // Negative is a nonsense interval and would be read as "off" by accident
    // rather than by choice, which is a different thing to say in the log.
    if (camera_trace_ms < 0) {
        Log::Line("WARN: config: [Diagnostics] CameraTraceMs=%ld is negative, so the default %ld "
                  "is used instead.", camera_trace_ms, def.camera_trace_ms);
        camera_trace_ms = def.camera_trace_ms;
        changed = true;
    }

    vk("Toggle", hotkey_toggle, def.hotkey_toggle);
    vk("TrackingMode", hotkey_tracking_mode, def.hotkey_tracking_mode);
    vk("YawMode", hotkey_yaw_mode, def.hotkey_yaw_mode);

    return changed;
}

} // namespace D2HT
