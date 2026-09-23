// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
// Unit tests for the Dishonored 2 Head Tracking mod's own code (src/core).
//
// These cover the file-system trust boundary: config values come from a
// user-editable HeadTracking.ini. strtod("nan") and a zero UDP port are the
// failure modes pinned here - both would otherwise propagate silently (NaN
// into the view matrix, an ephemeral bind the tracker can never reach). The
// round-trip tests pin the Save/Load key names against each other so a rename
// on one side cannot silently fall back to defaults on the other.

#include "core/config.h"
#include "core/log_throttle.h"
#include "hooks/camera_hook.h"
#include "voidengine/resolver_thread.h"
#include "voidengine/void_math.h"
#include "voidengine/world_trace.h"

#include <cameraunlock/camera/lean_clamp.h>
#include <cameraunlock/math/angle_utils.h>

#include <cameraunlock/camera/zoom_compensation.h>

// Before <windows.h>: this pulls in <WinSock2.h>, which must precede it.
#include <cameraunlock/protocol/udp_receiver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

int g_failures = 0;

// True when the log's bind-failure line carries the OS's own message text after
// the error code, rather than only a bracket somewhere in the file. Testing for
// "(" and ")" anywhere passes on any other line that happens to have brackets,
// so it would still pass with the FormatMessage text stripped out entirely.
bool HasSystemMessageAfterErrorCode(const std::string& log, const std::string& code) {
    const std::string anchor = "bind failed with error " + code + " (";
    const size_t open = log.find(anchor);
    if (open == std::string::npos) return false;
    const size_t textStart = open + anchor.size();
    const size_t close = log.find(')', textStart);
    if (close == std::string::npos) return false;
    return close - textStart >= 12;
}

void Check(bool cond, const char* name) {
    if (cond) {
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_failures;
    }
}

void SanitizeTests() {
    using D2HT::Config;
    std::cout << "Config sanitize tests\n";

    // A clean default config needs no correction.
    {
        Config c;
        Check(!c.Sanitize(), "default config is already valid");
    }

    // NaN / Inf in a float field is replaced with the default.
    {
        Config c;
        c.yaw_sensitivity = std::nanf("");
        c.pos_limit_z = std::numeric_limits<float>::infinity();
        const bool changed = c.Sanitize();
        Check(changed, "non-finite fields trigger correction");
        Check(std::isfinite(c.yaw_sensitivity) && c.yaw_sensitivity == 1.0f,
              "NaN sensitivity reset to default");
        Check(std::isfinite(c.pos_limit_z) && c.pos_limit_z == 0.40f,
              "Inf limit reset to default");
    }

    // Smoothing is a [0,1] math domain; out-of-range is clamped.
    {
        Config c;
        c.local_smoothing = 5.0f;
        c.remote_smoothing = -2.0f;
        Check(c.Sanitize(), "out-of-range smoothing triggers correction");
        Check(c.local_smoothing == 1.0f, "local smoothing clamped high");
        Check(c.remote_smoothing == 0.0f, "remote smoothing clamped low");
    }

    // A zero port binds an ephemeral one the tracker can never reach -> reset.
    {
        Config c;
        c.udp_port = 0;
        Check(c.Sanitize(), "a zero port triggers correction");
        Check(c.udp_port == 4242, "zero port reset to default");
    }

    // A negative travel limit inverts the bounds of the clamp it forms, and
    // core's Clamp then returns its lower bound for every input - the eye sits
    // at a fixed offset for the whole session instead of following the head.
    // What has to hold afterwards is that no limit is negative; the guard
    // brings each one to the nearest legal value and says so per key.
    {
        Config c;
        c.pos_limit_x = -0.30f;
        c.pos_limit_y = -0.20f;
        c.pos_limit_z = -0.40f;
        c.pos_limit_z_back = -0.10f;
        Check(c.Sanitize(), "negative travel limits trigger correction");
        Check(c.pos_limit_x >= 0.0f && c.pos_limit_y >= 0.0f && c.pos_limit_z >= 0.0f &&
                  c.pos_limit_z_back >= 0.0f,
              "no travel limit is left negative, so no clamp can invert its bounds");
    }

    // Zero is not the same fault: it is how a user disables travel on one axis,
    // and it leaves the clamp's bounds in order.
    {
        Config c;
        c.pos_limit_x = 0.0f;
        c.pos_limit_z_back = 0.0f;
        Check(!c.Sanitize(), "a zero travel limit is left alone");
        Check(c.pos_limit_x == 0.0f && c.pos_limit_z_back == 0.0f, "zero limits unchanged");
    }

    // A VK the OS cannot poll can never fire -> reset to default.
    {
        Config c;
        c.hotkey_toggle = 0;
        c.hotkey_tracking_mode = 0x100;
        c.hotkey_yaw_mode = -1;
        Check(c.Sanitize(), "out-of-range VK codes trigger correction");
        Check(c.hotkey_toggle == 0x23, "zero VK reset to default");
        Check(c.hotkey_tracking_mode == 0x21, "VK above 0xFF reset to default");
        Check(c.hotkey_yaw_mode == 0x22, "negative VK reset to default");
    }

    // A modifier is in range and still unusable: Ctrl and Shift are what the
    // chord guard tests, so a nav binding on one is suppressed exactly when it
    // would fire and an unguarded one fires on every chord. Binding tracking to
    // Shift would toggle it on every sprint.
    {
        Config c;
        c.hotkey_toggle = 0x10;         // VK_SHIFT
        c.hotkey_tracking_mode = 0x11;  // VK_CONTROL
        c.hotkey_yaw_mode = 0xFF;       // in 0x01..0xFF, but not a key
        Check(c.Sanitize(), "modifier and unpollable VK codes trigger correction");
        Check(c.hotkey_toggle == 0x23 && c.hotkey_tracking_mode == 0x21 &&
                  c.hotkey_yaw_mode == 0x22,
              "modifier and unpollable VK codes reset to their defaults");
    }

    // A limit past the top of the range is the same fault as a negative one and
    // is caught the same way. One engine unit is one metre, so LimitZ=4 - a
    // misplaced decimal point for 0.4 - puts the rendered eye four metres in
    // front of the player, through whatever is there.
    {
        Config c;
        c.pos_limit_z = 400.0f;
        c.pos_sens_x = 1e6f;
        Check(c.Sanitize(), "an absurd travel limit triggers correction");
        Check(c.pos_limit_z <= 10.0f, "the travel limit is brought back into range");
        Check(c.pos_sens_x <= 100.0f, "the sensitivity is brought back into range");
    }

    // A legitimate non-default value is left untouched (no behaviour change).
    {
        Config c;
        c.yaw_sensitivity = 2.5f;
        c.udp_port = 5005;
        c.hotkey_toggle = 0x70;
        Check(!c.Sanitize(), "legitimate values are preserved");
        Check(c.yaw_sensitivity == 2.5f && c.udp_port == 5005 && c.hotkey_toggle == 0x70,
              "values unchanged");
    }
}

std::string TempIniPath() {
    char dir[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, dir);
    if (len == 0 || len >= MAX_PATH) return "d2ht_test_config.ini";
    return std::string(dir, len) + "d2ht_test_config.ini";
}

void RoundTripTests() {
    using D2HT::Config;
    std::cout << "Config save/load round-trip tests\n";

    const std::string path = TempIniPath();

    Config saved;
    saved.udp_port = 5005;
    saved.enable_on_startup = false;
    saved.yaw_sensitivity = 2.5f;
    saved.invert_pitch = true;
    saved.local_smoothing = 0.25f;
    saved.remote_smoothing = 0.40f;
    saved.world_locked_yaw = false;
    saved.position_enabled = false;
    saved.pos_sens_z = 1.5f;
    saved.pos_limit_z_back = 0.05f;
    saved.invert_pos_y = true;
    saved.hotkey_toggle = 0x70;
    Check(saved.Save(path), "save writes the ini");

    Config loaded;
    Check(loaded.Load(path), "load reads the ini back");

    Check(loaded.udp_port == 5005, "port round-trips");
    Check(!loaded.enable_on_startup, "enable-on-startup round-trips");
    Check(loaded.yaw_sensitivity == 2.5f, "yaw sensitivity round-trips");
    Check(loaded.invert_pitch, "invert pitch round-trips");
    Check(loaded.local_smoothing == 0.25f, "local smoothing round-trips");
    Check(loaded.remote_smoothing == 0.40f, "remote smoothing round-trips");
    Check(!loaded.world_locked_yaw, "camera-local yaw round-trips");
    Check(!loaded.position_enabled, "position enabled round-trips");
    Check(loaded.pos_sens_z == 1.5f, "position sensitivity round-trips");
    Check(loaded.pos_limit_z_back == 0.05f, "position limit round-trips");
    Check(loaded.invert_pos_y, "position inversion round-trips");
    Check(loaded.hotkey_toggle == 0x70, "hex hotkey round-trips");

    std::remove(path.c_str());
}

// The shipped HeadTracking.ini has to be the file Config::Save writes.
//
// It is not just documentation: package-release.ps1 bakes it into the launcher
// manifest's loader.seed, so a launcher install delivers exactly these bytes,
// while install.cmd copies the same file and the mod regenerates it from Save
// when it is absent. Three deliveries of one file, and nothing else compares
// them. A key renamed in Save and not here ships a seed the code cannot read.
void ShippedIniTests() {
    using D2HT::Config;
    std::cout << "Shipped HeadTracking.ini tests\n";

    const std::string generatedPath = TempIniPath() + ".shipped";
    Config defaults;
    Check(defaults.Save(generatedPath), "a default config can be written");

    const auto slurp = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };
    const std::string generated = slurp(generatedPath);
    const std::string shipped = slurp(std::string(D2HT_REPO_ROOT) + "/HeadTracking.ini");
    std::remove(generatedPath.c_str());

    const bool same = !generated.empty() && generated == shipped;
    Check(same, "the shipped HeadTracking.ini is byte-for-byte what Config::Save writes");
    if (!same) {
        const std::string expected = std::string(D2HT_REPO_ROOT) + "/HeadTracking.ini.expected";
        std::ofstream out(expected, std::ios::binary | std::ios::trunc);
        out << generated;
        std::cout << "        wrote the expected file to " << expected << "\n";
    }
}

void MissingFileTests() {
    using D2HT::Config;
    std::cout << "Missing config file tests\n";

    Config c;
    Check(!c.Load(TempIniPath() + ".does-not-exist"), "missing file returns false");
    Check(c.udp_port == 4242 && c.yaw_sensitivity == 1.0f,
          "defaults preserved after failed load");
}

// The port a user typed, read through the boundary rather than narrowed at it.
// A value wider than a port used to be cast straight to uint16_t, so 70000
// bound 4464 - a valid port, a silent bind, and a tracker that never arrives.
void PortRangeTests() {
    using D2HT::Config;
    std::cout << "Config port range tests\n";

    const std::string path = TempIniPath() + ".port";
    const auto loadPort = [&](const char* value) {
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        out << "[Network]\nUdpPort=" << value << "\n";
        out.close();
        Config c;
        c.Load(path);
        const bool corrected = c.Sanitize();
        std::remove(path.c_str());
        return std::pair<uint16_t, bool>(c.udp_port, corrected);
    };

    const auto inRange = loadPort("5005");
    Check(inRange.first == 5005 && !inRange.second, "a port inside the range is taken as written");

    const auto tooLarge = loadPort("70000");
    Check(tooLarge.second && tooLarge.first == 4242,
          "a port above 65535 is refused rather than wrapped into another port");

    const auto negative = loadPort("-1");
    Check(negative.second && negative.first == 4242, "a negative port is refused");

    // Text that is not a number at all is refused one step earlier, by the
    // parse, which keeps the default and names the offending text. There is
    // then nothing left for the range check to correct, which is why this one
    // asserts the value rather than the correction flag.
    const auto garbage = loadPort("not-a-port");
    Check(garbage.first == 4242, "unparseable port text keeps the default");
}

// The sweep worker outlives the resolver that started it: a sweep reads a
// gigabyte of committed memory, and the destructor runs while the game is
// tearing down, so it detaches rather than waiting. Everything the worker
// reads therefore has to be owned by the sweep itself.
//
// The owner here is heap-allocated and deleted mid-sweep, with the shape it
// carries poisoned before the storage is released, so a sweep that reads
// through the owner reads the poison. A sweep capturing its owner's `this` -
// which is what the resolver used to hand over, and what this exists to stop
// coming back - fails on the value, not on a crash it might get away with.
void ResolverThreadTests() {
    std::cout << "Resolver thread lifetime tests\n";

    struct Owner {
        std::uint32_t shape = 0xD2000001u;
        std::atomic<bool>* in_sweep;
        std::atomic<bool>* release;
        std::atomic<std::uint32_t>* seen;
        std::unique_ptr<D2HT::voidengine::ResolverThread> worker;
    };

    // The four flags the sweep touches are heap-owned and captured BY VALUE, so
    // they outlive this function whatever happens. Captured by reference they
    // are locals of this frame, and the worker is detached: on the failure path
    // below - a Check that fails after a timeout - this function returns while
    // the worker is still polling them, and the next test reuses the stack. A
    // real assertion failure would then present as a crashed or corrupted test
    // binary instead of a clean FAIL line, which is the one time the output
    // matters most. This is the same discipline the sweep itself is being
    // tested for.
    struct Flags {
        std::atomic<bool> in_sweep{false};
        std::atomic<bool> release{false};
        std::atomic<std::uint32_t> seen{0};
        std::atomic<int> observed_cancel{0};
    };
    auto flags = std::make_shared<Flags>();

    auto* owner = new Owner{0xD2000001u, &flags->in_sweep, &flags->release, &flags->seen, nullptr};

    // The shape is copied into the sweep, exactly as GameLocalResolver copies it
    // into the worker. Nothing below reaches through `owner`.
    const std::uint32_t shape = owner->shape;
    owner->worker = std::make_unique<D2HT::voidengine::ResolverThread>(
        [shape, flags](const std::atomic<bool>& cancelled) -> void* {
            flags->in_sweep.store(true);
            while (!flags->release.load() && !cancelled.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (cancelled.load()) flags->observed_cancel.fetch_add(1);
            flags->seen.store(shape);
            return nullptr;
        });

    for (int i = 0; i < 5000 && !flags->in_sweep.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(flags->in_sweep.load(), "the worker is sweeping");

    // Destroyed with the sweep still in flight, which is the teardown shape.
    // The poison goes in before the storage is freed so a read through the
    // owner is a wrong VALUE rather than a crash the allocator might absorb.
    owner->shape = 0xDEADBEEFu;
    delete owner;

    for (int i = 0; i < 5000 && flags->seen.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(flags->seen.load() == 0xD2000001u,
          "a sweep in flight when its owner is destroyed reads the shape it was given, "
          "not the owner's");

    // And the sweep is asked to stop rather than left to run: the destructor
    // sets the flag the sweep polls, which is the only thing that bounds how
    // long the worker stays inside this module after teardown.
    Check(flags->observed_cancel.load() == 1,
          "the destructor's stop flag reaches the running sweep");

    flags->release.store(true);
}


// ---------------------------------------------------------------------------
// Port recovery.
//
// The scenario is a player who launches Dishonored 2 with the previous game
// still running: that process holds UDP 4242, our bind fails, and when they
// alt-tab out and close it the mod has to pick the port up on its own. These
// measure how long that takes end to end, from the port freeing to the first
// tracker pose reaching the game thread, with a tracker streaming throughout.
// ---------------------------------------------------------------------------

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Binds a port the way another game's receiver would: INADDR_ANY, no
/// SO_REUSEADDR. Returns INVALID_SOCKET if the port is not free.
SOCKET OccupyPort(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/// Streams 48-byte OpenTrack packets at 60 Hz until told to stop, with a yaw
/// that moves every packet so nothing is taken for a tracker that has stalled.
void StreamTracker(uint16_t port, std::atomic<bool>* stop) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    double yaw = 0.0;
    while (!stop->load(std::memory_order_relaxed)) {
        double values[6] = {0.0, 0.0, 0.0, yaw, 0.0, 0.0};
        uint8_t packet[48];
        std::memcpy(packet, values, sizeof(packet));
        sendto(s, reinterpret_cast<const char*>(packet), sizeof(packet), 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        yaw += 0.25;
        if (yaw > 4.0) yaw = 0.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    closesocket(s);
}

/// One full cycle: another process holds `port`, the mod's receiver fails to
/// bind and retries, the holder exits after `holdMs`, and the receiver takes
/// the port over. Fills in the two intervals a player actually waits through.
/// `log` receives everything the receiver reported during the cycle.
bool MeasureReclaim(uint16_t port, int holdMs, int64_t& bindMs, int64_t& packetMs,
                    std::string& log) {
    using cameraunlock::UdpReceiver;

    SOCKET occupier = OccupyPort(port);
    if (occupier == INVALID_SOCKET) return false;

    // The tracker is already running and sending, as it would be while the
    // other game holds the port.
    std::atomic<bool> stopSender(false);
    std::thread sender(StreamTracker, port, &stopSender);

    UdpReceiver receiver;
    receiver.SetLog([&log](const std::string& message) { log += message + "\n"; });

    const bool startedBound = receiver.Start(port);
    const bool retrying = receiver.IsRetrying() && receiver.IsFailed() && !receiver.IsRunning();

    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    const bool stillRetrying = receiver.IsRetrying() && !receiver.IsRunning();

    const int64_t releasedAt = NowMs();
    closesocket(occupier);

    int64_t boundAt = 0;
    int64_t firstPacketAt = 0;
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    while (NowMs() - releasedAt < 5000) {
        if (boundAt == 0 && receiver.IsRunning()) boundAt = NowMs();
        if (boundAt != 0 && receiver.GetRotation(yaw, pitch, roll)) {
            firstPacketAt = NowMs();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const bool cleared = !receiver.IsRetrying() && !receiver.IsFailed();

    receiver.Stop();
    stopSender.store(true);
    sender.join();

    bindMs = boundAt == 0 ? -1 : boundAt - releasedAt;
    packetMs = firstPacketAt == 0 ? -1 : firstPacketAt - releasedAt;
    return !startedBound && retrying && stillRetrying && cleared && boundAt != 0 &&
           firstPacketAt != 0;
}

void PortRecoveryTests() {
    using cameraunlock::UdpReceiver;
    std::cout << "UDP port recovery tests\n";

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Check(false, "WSAStartup for the test's own sockets");
        return;
    }

    // A port this run can contest without disturbing a real tracker.
    uint16_t port = 0;
    for (uint16_t candidate = 14561; candidate < 14561 + 32; ++candidate) {
        SOCKET probe = OccupyPort(candidate);
        if (probe != INVALID_SOCKET) {
            closesocket(probe);
            port = candidate;
            break;
        }
    }
    if (port == 0) {
        Check(false, "a free loopback port to contest");
        WSACleanup();
        return;
    }

    // The hold spans a full retry interval so the release lands at a different
    // phase of the retry cycle each time. The worst of these is the real
    // worst case a player meets; a single hold only ever samples one phase.
    const int holds[] = {700, 800, 900, 1000, 1100, 1200};
    int64_t worstBind = -1;
    int64_t worstPacket = -1;
    bool allRecovered = true;
    std::string log;

    for (int hold : holds) {
        int64_t bindMs = -1;
        int64_t packetMs = -1;
        std::string cycleLog;
        const bool ok = MeasureReclaim(port, hold, bindMs, packetMs, cycleLog);
        allRecovered = allRecovered && ok;
        if (log.empty()) log = cycleLog;
        if (bindMs > worstBind) worstBind = bindMs;
        if (packetMs > worstPacket) worstPacket = packetMs;
        std::cout << "        held " << hold << " ms -> bound " << bindMs
                  << " ms after the port freed, first tracker pose " << packetMs
                  << " ms after\n";
    }

    Check(allRecovered,
          "every cycle: bind fails, the supervisor retries, and it reclaims the port");

    // Measured and printed, never asserted. The bound this used to check was
    // one retry interval plus a couple of hundred milliseconds of slack, which
    // a scheduler stall on a loaded CI runner clears with nothing wrong in the
    // code - a failure that says nothing about the thing under test. What has
    // to hold is the functional chain above; how fast it ran is for a human
    // reading the output.
    std::cout << "        worst reclaim " << worstBind << " ms, worst first pose " << worstPacket
              << " ms, against a retry interval of " << UdpReceiver::kRetryIntervalMs << " ms\n";

    // The log must carry what the OS said, not a guess at it. WSAEADDRINUSE is
    // the code Windows gives for this one; "another program is holding it" is
    // only ever true when the error itself says so.
    const std::string expectedCode = std::to_string(WSAEADDRINUSE);
    Check(log.find("bind failed with error " + expectedCode) != std::string::npos,
          "bind failure log names the OS error code the socket returned");
    Check(HasSystemMessageAfterErrorCode(log, expectedCode),
          "bind failure log carries the system message text for that code");
    Check(log.find("Bound UDP port " + std::to_string(port)) != std::string::npos,
          "the reclaim is reported to the log");
    // Printed whether or not it passed: the point of the line is that a human
    // reading a user's log can act on it, and that only shows in the wording.
    std::cout << "        logged: " << log;

    WSACleanup();
}

// The units gate for the zoom compensation.
//
// The engine turns the ark_fieldOfView setting into the frame's fov_y exactly as
// idView::SetFovXandY does below, so an un-zoomed frame has to come back at a
// factor of 1.0 whatever the setting is. Getting the axis or the units wrong -
// pairing the vertical fov_y with a horizontal reference, say - does not fail
// anywhere visible: it just parks the whole of normal play at a constant
// fraction of the pose, and head tracking feels weak everywhere.
void ZoomCompensationTests() {
    using D2HT::voidengine::TanHalfVerticalFovFromSetting;
    using cameraunlock::camera::FovZoomFactor;
    using cameraunlock::camera::ScaleAngleForZoom;
    std::cout << "Zoom compensation tests\n";

    // idView::SetFovXandY, RVA 0x890B50: fov_y = 2 * atan(tan(setting / 2) * scale).
    const float kScale = 0.5625f;  // the build's constant, at RVA 0x1ACA860
    const auto engineFovY = [&](float settingDeg) {
        const float half = settingDeg * 3.14159265358979323846f / 360.0f;
        return 2.0f * std::atan(std::tan(half) * kScale);
    };

    for (float setting : {60.0f, 75.0f, 90.0f, 110.0f}) {
        const float tanHalfFovY = std::tan(engineFovY(setting) * 0.5f);
        const float factor = FovZoomFactor(tanHalfFovY,
                                           TanHalfVerticalFovFromSetting(setting, kScale));
        Check(std::fabs(factor - 1.0f) < 1e-4f, "un-zoomed frame gives a factor of 1.0");
    }

    // The two ways of pairing the frame against the reference, on a display that
    // is not 16:9, where they differ. Neither mistake fails anywhere visible:
    // each parks the whole of normal play at a constant fraction of the pose and
    // head tracking just feels weak everywhere. The window in the in-game log
    // that settled this was 1904x993.
    {
        const float setting = 80.0f;
        const float aspect = 1904.0f / 993.0f;
        const float fovY = engineFovY(setting);
        const float fovX = 2.0f * std::atan(std::tan(fovY * 0.5f) * aspect);
        const float verticalReference = TanHalfVerticalFovFromSetting(setting, kScale);
        const float settingHalfTan = std::tan(setting * 3.14159265358979323846f / 360.0f);

        // Pairing the frame's HORIZONTAL fov with the setting read as horizontal
        // leaves the aspect over 16:9, and nothing else. That is the subtle one -
        // eight per cent, small enough to read as "the tracker feels weak" rather
        // than as a fault - and it is the mistake Deus Ex: Human Revolution
        // shipped for an afternoon.
        Check(std::fabs(FovZoomFactor(std::tan(fovX * 0.5f), settingHalfTan) - 1.0785f) < 1e-3f,
              "pairing the horizontal frame fov with the raw setting is off by the aspect");

        // Pairing the frame's horizontal fov with the VERTICAL reference is the
        // same axis error plus the missing 0.5625, and nearly a factor of two.
        Check(std::fabs(FovZoomFactor(std::tan(fovX * 0.5f), verticalReference) - 1.9174f) < 1e-3f,
              "pairing the horizontal frame fov with the vertical reference is off by 1.92");

        // What the mod does. Both terms are vertical, so the aspect cancels and
        // the factor is 1.0 whatever shape the window is.
        Check(std::fabs(FovZoomFactor(std::tan(fovY * 0.5f), verticalReference) - 1.0f) < 1e-4f,
              "the vertical pairing the mod uses reads 1.0 un-zoomed at any aspect");
    }

    // A spyglass-shaped zoom: the frame is drawn at half the setting, so the
    // pose has to move the picture by half as much as it otherwise would.
    {
        const float setting = 90.0f;
        const float tanHalfFovY = std::tan(engineFovY(45.0f) * 0.5f);
        const float factor = FovZoomFactor(tanHalfFovY,
                                           TanHalfVerticalFovFromSetting(setting, kScale));
        Check(factor > 0.4f && factor < 0.45f, "a 90 -> 45 degree zoom scales the pose down");

        // The scaled angle displaces the picture by what the raw angle would
        // have displaced it by at the setting's own field of view.
        const float raw = 12.0f;
        const float scaled = ScaleAngleForZoom(raw, factor);
        const float zoomed = std::tan(scaled * 3.14159265358979323846f / 180.0f) / tanHalfFovY;
        const float base = std::tan(raw * 3.14159265358979323846f / 180.0f) /
                           TanHalfVerticalFovFromSetting(setting, kScale);
        Check(std::fabs(zoomed - base) < 1e-4f, "a scaled angle moves the picture as far as before");
    }

    // Roll is not scaled anywhere, and a factor of 1.0 has to leave an angle
    // exactly where it was.
    Check(std::fabs(ScaleAngleForZoom(17.5f, 1.0f) - 17.5f) < 1e-4f,
          "a factor of 1.0 leaves the angle alone");

    // The domain the tangent round trip is defined on, and why the camera hook
    // bounds the pose before it calls this.
    //
    // ScaleAngleForZoom is atan(tan(a) * factor), which is the identity at
    // factor 1.0 only for |a| < 90. tan flips sign across the pole, so an
    // unbounded angle comes back on the OPPOSITE side: the view snaps 180
    // degrees the moment a tracker that keeps tracking past a shoulder check
    // crosses 90. Nothing upstream bounds the pose - it is consumed at 1:1 and
    // the processor clamps position only - so the guard is at the call site,
    // and this is the arithmetic that makes it necessary.
    {
        const float past = 100.0f;
        const float wrapped = ScaleAngleForZoom(past, 1.0f);
        Check(wrapped < 0.0f,
              "past 90 degrees the raw tangent round trip flips the angle's sign");

        // Inside the domain it is the identity at factor 1.0, which is what makes
        // passing an out-of-domain angle through UNSCALED the safe answer:
        // CameraHook::ScaleAngleForZoomInDomain returns such an angle untouched
        // rather than clamping it, so a large head turn is never capped. The
        // hook is not linked into this binary, so what is pinned here is the
        // core property the hook relies on.
        Check(std::fabs(ScaleAngleForZoom(88.0f, 1.0f) - 88.0f) < 1e-3f,
              "just inside the domain the round trip is still the identity");
    }
}

// The basis maths the camera hook composes every frame, pinned so a
// restructure cannot quietly change a sign, an axis or the composition order.
// The row order is forward / left / up throughout, radians throughout.
void VoidMathTests() {
    using namespace D2HT::voidengine;
    std::cout << "Void Engine basis maths tests\n";

    const Mat3 identity;  // forward +X, left +Y, up +Z
    Check(IsOrthonormal(identity), "the identity basis is orthonormal");

    {
        Mat3 skewed;
        skewed.m[0] = 2.0f;
        Check(!IsOrthonormal(skewed), "a row off unit length is refused");
        Mat3 nonFinite;
        nonFinite.m[4] = std::numeric_limits<float>::quiet_NaN();
        Check(!IsOrthonormal(nonFinite), "a non-finite row is refused");
    }

    {
        const Mat3 same = RotateBasisLocal(identity, 0.0f, 0.0f, 0.0f);
        bool unchanged = true;
        for (int i = 0; i < 9; ++i) unchanged = unchanged && same.m[i] == identity.m[i];
        Check(unchanged, "a zero pose leaves the basis exactly as it was");
    }

    const float a = 0.4f;
    {
        // Positive yaw turns the view LEFT, i.e. forward swings toward +Y.
        const Mat3 yawed = RotateBasisLocal(identity, a, 0.0f, 0.0f);
        Check(std::fabs(yawed.Row(0)[0] - std::cos(a)) < 1e-5f &&
                  std::fabs(yawed.Row(0)[1] - std::sin(a)) < 1e-5f &&
                  std::fabs(yawed.Row(0)[2]) < 1e-5f,
              "positive yaw turns forward toward +Y (left)");
        Check(std::fabs(yawed.Row(2)[2] - 1.0f) < 1e-5f, "yaw leaves the up row on +Z");
        Check(IsOrthonormal(yawed), "a yawed basis stays orthonormal");
    }

    {
        // Positive pitch raises the view: forward gains +Z.
        const Mat3 pitched = RotateBasisLocal(identity, 0.0f, a, 0.0f);
        Check(std::fabs(pitched.Row(0)[0] - std::cos(a)) < 1e-5f &&
                  std::fabs(pitched.Row(0)[2] - std::sin(a)) < 1e-5f,
              "positive pitch raises forward toward +Z");
        Check(std::fabs(pitched.Row(1)[1] - 1.0f) < 1e-5f, "pitch leaves the left row on +Y");
    }

    {
        // Positive roll tilts the up row toward the LEFT row - a head tilted left.
        const Mat3 rolled = RotateBasisLocal(identity, 0.0f, 0.0f, a);
        Check(std::fabs(rolled.Row(0)[0] - 1.0f) < 1e-5f, "roll leaves forward alone");
        Check(std::fabs(rolled.Row(2)[1] - std::sin(a)) < 1e-5f &&
                  std::fabs(rolled.Row(2)[2] - std::cos(a)) < 1e-5f,
              "positive roll tilts up toward +Y (left)");
    }

    {
        // A combined pose: pinned element by element, because the composition
        // order is the one thing a single-axis test cannot see.
        const Mat3 combined = RotateBasisLocal(identity, 0.3f, -0.2f, 0.1f);
        Check(IsOrthonormal(combined), "a combined pose stays orthonormal");
        const float expected[9] = {0.936293f,  0.289629f, -0.198669f,
                                   -0.312992f, 0.944702f, -0.097843f,
                                   0.159345f,  0.153792f,  0.975170f};
        bool matches = true;
        for (int i = 0; i < 9; ++i) matches = matches && std::fabs(combined.m[i] - expected[i]) < 1e-4f;
        Check(matches, "a combined yaw/pitch/roll composes as it always has");
    }

    {
        // World yaw turns about world +Z. On a level basis that is the same
        // rotation camera-local yaw makes; on a pitched one it is not.
        const Mat3 world = RotateBasisWorldYaw(identity, a, 0.0f, 0.0f);
        const Mat3 local = RotateBasisLocal(identity, a, 0.0f, 0.0f);
        bool same = true;
        for (int i = 0; i < 9; ++i) same = same && std::fabs(world.m[i] - local.m[i]) < 1e-5f;
        Check(same, "on a level basis world yaw and local yaw agree");

        const Mat3 pitchedBase = RotateBasisLocal(identity, 0.0f, 1.0f, 0.0f);
        const Mat3 worldOnPitched = RotateBasisWorldYaw(pitchedBase, a, 0.0f, 0.0f);
        Check(IsOrthonormal(worldOnPitched), "world yaw on a pitched basis stays orthonormal");
        Check(std::fabs(worldOnPitched.Row(0)[2] - pitchedBase.Row(0)[2]) < 1e-5f,
              "world yaw leaves the forward row's height alone");
    }

    {
        // The world-Z arithmetic and its signs, on the only shape the camera hook
        // ever calls this in: all three angles non-zero on a basis that is
        // already pitched and rolled. Every other world-yaw case above passes
        // pitch = roll = 0, which makes the inner RotateBasisLocal the identity
        // and leaves the yaw block exercised only against an unrotated basis.
        //
        // What this does NOT pin, despite appearances, is the ORDER of the two
        // steps. RotateBasisLocal LEFT-multiplies (L * axis) and the world-yaw
        // step RIGHT-multiplies (local * W), so by associativity
        // L * (axis * W) and (L * axis) * W are the same matrix: yawing first
        // and yawing last are not distinguishable here, and no assertion can
        // separate them. Do not add one claiming otherwise. What a wrong sign or
        // a transposed term in the yaw block does change is caught below.
        const Mat3 base = RotateBasisLocal(identity, 0.4f, -0.25f, 0.15f);
        const float yaw = 0.3f, pitch = -0.2f, roll = 0.1f;

        const Mat3 inner = RotateBasisLocal(base, 0.0f, pitch, roll);
        const float c = std::cos(yaw), sn = std::sin(yaw);
        Mat3 expected;
        for (int i = 0; i < 3; ++i) {
            const float* r = inner.Row(i);
            expected.m[i * 3 + 0] = c * r[0] - sn * r[1];
            expected.m[i * 3 + 1] = sn * r[0] + c * r[1];
            expected.m[i * 3 + 2] = r[2];
        }

        const Mat3 got = RotateBasisWorldYaw(base, yaw, pitch, roll);
        bool matches = true;
        for (int i = 0; i < 9; ++i) matches = matches && std::fabs(got.m[i] - expected.m[i]) < 1e-5f;
        Check(matches, "world yaw turns the pitched and rolled basis about world up");
        Check(IsOrthonormal(got), "a combined world-yaw pose stays orthonormal");

        // And it is genuinely a different rotation from the camera-local one on
        // a pitched basis, so the assertion above is not passing by coincidence.
        const Mat3 localVariant = RotateBasisLocal(base, yaw, pitch, roll);
        bool differs = false;
        for (int i = 0; i < 9; ++i) differs = differs || std::fabs(got.m[i] - localVariant.m[i]) > 1e-3f;
        Check(differs, "on a pitched basis world yaw is not the same as camera-local yaw");
    }

    {
        const Vec3 origin{10.0f, 20.0f, 30.0f};
        const Vec3 moved = TranslateAlongBasis(origin, identity, 1.0f, 2.0f, 3.0f);
        Check(std::fabs(moved.x - 11.0f) < 1e-5f && std::fabs(moved.y - 22.0f) < 1e-5f &&
                  std::fabs(moved.z - 33.0f) < 1e-5f,
              "a lean moves the eye along the basis rows");

        const Mat3 yawed = RotateBasisLocal(identity, 1.5707963f, 0.0f, 0.0f);
        const Vec3 leaned = TranslateAlongBasis(origin, yawed, 1.0f, 0.0f, 0.0f);
        Check(std::fabs(leaned.x - 10.0f) < 1e-4f && std::fabs(leaned.y - 21.0f) < 1e-4f,
              "a forward lean follows the basis it is measured in");
    }

    Check(D2HT::voidengine::kUnitsPerMetre == 1.0f, "the world is metric: one unit per metre");

    // The camera hook scales its zoom-compensated yaw and pitch back into
    // radians with this constant, and it used to carry a second copy of its own
    // derived from core's double. The two are the same float and the duplicate
    // is gone; if a compiler or a core edit ever separated them, every scaled
    // angle would move by an ulp with nothing in the log to say so.
    Check(D2HT::voidengine::kDegToRad == static_cast<float>(cameraunlock::math::kDegToRad),
          "the engine boundary's degrees-to-radians constant is core's, to the bit");
}

// The protocol-to-engine boundary: the single highest-risk conversion in the
// mod, and the one a port re-derives from geometry and gets wrong. Every sign
// here was settled in a running game on the id Tech 5 sibling; a change to any
// of them is a change a human has to make deliberately.
void PoseBoundaryTests() {
    using D2HT::voidengine::EnginePose;
    using D2HT::voidengine::PoseToEngine;
    std::cout << "Tracker-to-engine boundary tests\n";

    const float kDeg = 3.14159265358979323846f / 180.0f;

    {
        const EnginePose p = PoseToEngine(0, 0, 0, 0, 0, 0);
        Check(p.yaw == 0.0f && p.pitch == 0.0f && p.roll == 0.0f && p.forward == 0.0f &&
                  p.left == 0.0f && p.up == 0.0f,
              "a centred pose converts to nothing at all");
    }

    // The tracker calls a head turned RIGHT positive yaw; RotateBasisLocal calls
    // a view turned LEFT positive. Negated exactly once, here.
    {
        const EnginePose p = PoseToEngine(10.0f, 0, 0, 0, 0, 0);
        Check(std::fabs(p.yaw + 10.0f * kDeg) < 1e-6f, "a head turned right yaws the view right");
    }

    // Pitch and roll pass through, in sign and in scale.
    {
        const EnginePose p = PoseToEngine(0, 7.0f, -3.0f, 0, 0, 0);
        Check(std::fabs(p.pitch - 7.0f * kDeg) < 1e-6f, "pitch passes through");
        Check(std::fabs(p.roll + 3.0f * kDeg) < 1e-6f, "roll passes through");
    }

    // The processor reports NEGATIVE z for a head leaning IN, and the basis's
    // first row is forward. Getting this backwards puts the generous 0.40 m
    // budget on the backward lean and leaves 0.10 m for leaning in.
    {
        const EnginePose p = PoseToEngine(0, 0, 0, 0, 0, -0.40f);
        Check(p.forward > 0.0f && std::fabs(p.forward - 0.40f) < 1e-6f,
              "the processor's negative z is a lean FORWARD");
    }
    {
        const EnginePose p = PoseToEngine(0, 0, 0, 0, 0, 0.10f);
        Check(p.forward < 0.0f, "the processor's positive z is a lean back");
    }

    // x is a head moved right and the second row is LEFT, so the conversion is
    // in the row order rather than in a minus sign; y and up agree.
    {
        const EnginePose p = PoseToEngine(0, 0, 0, 0.30f, 0.20f, 0);
        Check(std::fabs(p.left - 0.30f) < 1e-6f, "x maps to the basis's left row");
        Check(std::fabs(p.up - 0.20f) < 1e-6f, "y maps to the basis's up row");
    }

    // Metres, not inches. A build once inherited 39.3701 units per metre from
    // the id Tech sibling and a 0.30 m lean came out as 11.8 world units, which
    // threw the camera through the wall behind the player.
    {
        const EnginePose p = PoseToEngine(0, 0, 0, 1.0f, 0, 0);
        Check(std::fabs(p.left - 1.0f) < 1e-6f, "one metre of lean is one engine unit");
    }
}

// The parse half of the config boundary. Sanitize checks ranges; this checks
// that the number reaching it is the number the user wrote. Every one of these
// used to read as a plausible value and pass every range check there is.
void ConfigParseTests() {
    using D2HT::Config;
    std::cout << "Config parse tests\n";

    const std::string path = TempIniPath() + ".parse";
    const auto load = [&](const char* body) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << body;
        out.close();
        Config c;
        c.Load(path);
        return c;
    };

    // A decimal comma is the natural spelling wherever the keyboard has one,
    // and IniReader pins the C locale. Parsed as a prefix it reads 0.0, which is
    // inside every valid range, so the phone-over-WiFi user this key exists for
    // silently gets no smoothing at all.
    Check(load("[Rotation]\nRemoteSmoothing=0,15\n").remote_smoothing == 0.15f,
          "a decimal comma keeps the default rather than reading as zero");
    Check(load("[Position]\nLimitZ=0,4\n").pos_limit_z == 0.40f,
          "a decimal comma in a travel limit keeps the default");
    Check(load("[Rotation]\nRemoteSmoothing=0.4\n").remote_smoothing == 0.4f,
          "a well-formed value is still read");

    // The hotkey comment in the file says "End=Toggle", so writing the key's
    // NAME is the obvious mistake. As a hex prefix "End" reads 0x0E, an
    // unassigned VK that passes every range check and simply never fires.
    Check(load("[Hotkeys]\nToggle=End\n").hotkey_toggle == 0x23,
          "a key name in a hotkey keeps the default rather than reading as hex");
    Check(load("[Hotkeys]\nTrackingMode=Delete\n").hotkey_tracking_mode == 0x21,
          "a key name that parses whole as hex is still refused");
    Check(load("[Hotkeys]\nToggle=0x70\n").hotkey_toggle == 0x70,
          "a well-formed hex code is still read");

    // strtoul APPLIES a leading minus to the unsigned result and consumes the
    // whole string doing it, so "-1" used to arrive as 0xFFFFFFFF having passed
    // every check. A virtual key that wide is caught by Sanitize; a filter word
    // is a bit pattern with no range to check, so nothing downstream would ever
    // have noticed. CollisionChannel=-1 asks the engine for a body carrying
    // every flag there is, which nothing carries: the lean clamp queries every
    // frame, never blocks, and the log calls the collision on and the lean
    // unrestricted.
    Check(load("[Position]\nCollisionChannel=-1\n").collision_channel == 0x800u,
          "a negative collision filter word keeps the default rather than becoming an "
          "all-bits mask nothing can satisfy");
    Check(load("[Reticle]\nAimTraceChannel=-0x800\n").aim_trace_channel == 0x800u,
          "and the same for the aim line's filter word");
    // An overflow clamps to the type's maximum and consumes the whole string
    // too, so it is refused on the same footing rather than silently truncated.
    Check(load("[Position]\nCollisionChannel=0x1FFFFFFFFF\n").collision_channel == 0x800u,
          "a filter word too wide for the field keeps the default");
    Check(load("[Hotkeys]\nToggle=-23\n").hotkey_toggle == 0x23,
          "a signed hotkey code keeps the default");
    Check(load("[Position]\nCollisionChannel=0x20000\n").collision_channel == 0x20000u,
          "a well-formed filter word is still read");

    // strtol clamps an over-wide decimal the same way. CameraTraceMs is the one
    // that has nothing downstream to catch it: LONG_MAX milliseconds is the
    // diagnostic trace switched off by arithmetic rather than by choice.
    Check(load("[Diagnostics]\nCameraTraceMs=99999999999999999999\n").camera_trace_ms == 0,
          "a decimal too wide for the field keeps the default");
    Check(load("[Diagnostics]\nCameraTraceMs=250\n").camera_trace_ms == 250,
          "a well-formed interval is still read");

    // A true/false key used to default silently on anything outside a fixed
    // token list, which a trailing comment put it outside. Every key now goes
    // through the same trimming reader, so the comment comes off first.
    Check(load("[Rotation]\nInvertPitch=true ; webcam is upside down\n").invert_pitch,
          "a trailing comment on a yes/no line no longer swallows the setting");
    Check(load("[Rotation]\nInvertPitch=ture\n").invert_pitch == false,
          "a misspelled yes/no value keeps the default");
    Check(load("[Rotation]\nInvertPitch=true\n").invert_pitch,
          "a well-formed yes/no value is still read");
    Check(load("[Rotation]\nInvertPitch=ON\n").invert_pitch,
          "the yes/no token list is case-insensitive");

    // An inline comment survives, because the raw reader trims at ';' first.
    Check(load("[Rotation]\nLocalSmoothing=0.25 ; steady\n").local_smoothing == 0.25f,
          "a trailing comment does not stop a number being read");

    std::remove(path.c_str());
}

// The diagnostic log schedule: a burst, then a dense interval while the run is
// young, then a thin one. Frames are counted on every call and lines only when
// one is emitted, and mixing the two up is what turns a thin trickle silent.
// The engine's line check is one call through a function pointer, and every
// caller depends on how its answer is read: TRUE means the path was CLEAR, and
// the out parameter is only meaningful when it comes back false. Getting that
// backwards would clamp every lean to zero and put the reticle on the eye.
//
// The stand-in below is bound the same way the real one is - a base address plus
// an offset - so the binding arithmetic is under test too.
namespace worldtrace {

// What the fake engine should answer, and what it was asked.
bool g_clear = true;
float g_distance = 0.0f;
D2HT::voidengine::Vec3 g_start{};
D2HT::voidengine::Vec3 g_end{};
unsigned g_channel = 0;
int g_calls = 0;

bool __fastcall Fake(void* unused, const D2HT::voidengine::Vec3* start,
                     const D2HT::voidengine::Vec3* end, std::uint32_t channel, float* distance) {
    (void)unused;
    ++g_calls;
    g_start = *start;
    g_end = *end;
    g_channel = channel;
    if (!g_clear) *distance = g_distance;
    return g_clear;
}

// Offset 0 is how the mod says "this build has no such function", so the base
// is backed off by a real offset here and the arithmetic has to put it back.
constexpr std::uint32_t kFakeRva = 0x40;

D2HT::voidengine::WorldTrace Bound() {
    D2HT::voidengine::WorldTrace t;
    t.Bind(reinterpret_cast<std::uintptr_t>(&Fake) - kFakeRva, kFakeRva);
    return t;
}

}  // namespace worldtrace

void WorldTraceTests() {
    using D2HT::voidengine::Vec3;
    std::cout << "World trace tests\n";

    const Vec3 start{1.0f, 2.0f, 3.0f};
    const Vec3 dir{0.0f, 1.0f, 0.0f};

    D2HT::voidengine::WorldTrace unbound;
    unbound.Bind(reinterpret_cast<std::uintptr_t>(&worldtrace::Fake), 0);
    Check(!unbound.IsBound(), "an offset of zero means the build has no such function");
    Check(!unbound.Line(start, dir, 10.0f, 0x800).queried,
          "an unbound trace reports that it could not run, not a clear path");

    auto trace = worldtrace::Bound();
    Check(trace.IsBound(), "binding to a base plus offset resolves the entry point");

    worldtrace::g_calls = 0;
    worldtrace::g_clear = true;
    D2HT::voidengine::TraceResult r = trace.Line(start, dir, 10.0f, 0x800);
    Check(worldtrace::g_calls == 1, "one call per Line");
    Check(r.queried && !r.blocked, "a CLEAR answer is a definite no-hit, not a failed query");
    Check(worldtrace::g_channel == 0x800, "the filter word is passed through unchanged");
    Check(worldtrace::g_start.x == 1.0f && worldtrace::g_start.y == 2.0f &&
              worldtrace::g_start.z == 3.0f,
          "the start point is passed through unchanged");
    Check(worldtrace::g_end.x == 1.0f && worldtrace::g_end.y == 12.0f &&
              worldtrace::g_end.z == 3.0f,
          "the end point is the start plus the direction times the distance");

    worldtrace::g_clear = false;
    worldtrace::g_distance = 4.5f;
    r = trace.Line(start, dir, 10.0f, 0);
    Check(r.queried && r.blocked && r.distance == 4.5f,
          "a blocked answer carries the distance the engine wrote");

    // A hit the arithmetic cannot use is reported as a no-hit that DID run: the
    // query is not what failed, so saying otherwise would send a reader hunting
    // a hook that is working.
    worldtrace::g_distance = std::numeric_limits<float>::quiet_NaN();
    r = trace.Line(start, dir, 10.0f, 0);
    Check(r.queried && !r.blocked, "a non-finite hit distance is a no-hit, not a failed query");
    worldtrace::g_distance = -1.0f;
    r = trace.Line(start, dir, 10.0f, 0);
    Check(r.queried && !r.blocked, "a negative hit distance is a no-hit, not a failed query");

    // Refused before the engine is called at all, so a NaN pose cannot reach
    // Havok.
    worldtrace::g_calls = 0;
    const Vec3 bad{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    Check(!trace.Line(bad, dir, 10.0f, 0).queried, "a non-finite start is refused");
    Check(!trace.Line(start, bad, 10.0f, 0).queried, "a non-finite direction is refused");
    Check(!trace.Line(start, dir, 0.0f, 0).queried, "a zero-length trace is refused");
    Check(!trace.Line(start, dir, -1.0f, 0).queried, "a negative-length trace is refused");
    Check(worldtrace::g_calls == 0, "none of those reached the engine");
}

// The half of the camera collision this mod owns is the query; the policy is
// core's. What is pinned here is CORE'S POLICY as the camera hook relies on it -
// the standoff comes off the hit distance, a clear room passes the lean through,
// and a query that cannot run says so rather than silently behaving like a clear
// path. CameraHook::LeanQuery itself is not reached - camera_hook.cpp is not
// linked into this binary - but the arithmetic it uses to size the ray is, via
// D2HT::ReachForLean, which the assertion at the end calls directly.
void LeanClampWiringTests() {
    using cameraunlock::camera::LeanClamp;
    using cameraunlock::camera::LeanClampSettings;
    using cameraunlock::camera::LeanObstruction;
    using cameraunlock::math::Vec3;
    std::cout << "Lean clamp wiring tests\n";

    struct Answer {
        static LeanObstruction Clear(void*, const Vec3&, const Vec3&, float) {
            LeanObstruction o;
            o.queried = true;
            return o;
        }
        static LeanObstruction Wall(void*, const Vec3&, const Vec3&, float) {
            LeanObstruction o;
            o.queried = true;
            o.blocked = true;
            o.distance = 0.25f;  // a wall a quarter of a metre away
            return o;
        }
        static LeanObstruction Failed(void*, const Vec3&, const Vec3&, float) {
            return LeanObstruction{};
        }
    };

    LeanClampSettings settings;
    settings.skin = 0.15f;
    LeanClamp clamp;
    clamp.SetSettings(settings);

    const Vec3 eye{0.0f, 0.0f, 0.0f};
    const Vec3 want{0.30f, 0.0f, 0.0f};

    Vec3 got = clamp.Apply(eye, want, 0.016f, &Answer::Clear, nullptr);
    Check(std::fabs(got.Magnitude() - 0.30f) < 1e-4f && !clamp.InContact(),
          "an open room passes the whole lean through");

    clamp.Reset();
    got = clamp.Apply(eye, want, 0.016f, &Answer::Wall, nullptr);
    Check(std::fabs(got.Magnitude() - 0.10f) < 1e-4f,
          "a wall at 0.25m with a 0.15m standoff leaves 0.10m of lean");
    Check(clamp.InContact(), "and the clamp says it is holding the eye off a surface");

    clamp.Reset();
    got = clamp.Apply(eye, want, 0.016f, &Answer::Failed, nullptr);
    Check(std::fabs(got.Magnitude() - 0.30f) < 1e-4f,
          "a query that could not run passes the lean through unclamped");
    Check(clamp.LastQueryFailed(),
          "and reports it, because a clamp that stopped clamping looks like one that never "
          "engaged");

    clamp.Reset();
    got = clamp.Apply(eye, want, 0.016f, nullptr, nullptr);
    Check(std::fabs(got.Magnitude() - 0.30f) < 1e-4f && !clamp.LastQueryFailed(),
          "no query at all is the feature switched off, which is not a failure");
    // How far the camera hook's ray reaches, pinned by calling the hook's OWN
    // function rather than restating its arithmetic.
    //
    // Core adds its skin before calling the query - it asks for
    // `desired + skin` - and the hook then sizes the ray with
    // D2HT::ReachForLean, which is the function CameraHook::LeanQuery itself
    // calls. The overreach it adds is not a double-count to be tidied away: a
    // zero-extent ray stopping where the lean stops cannot see the surface the
    // lean is about to rest against, so dropping it puts the eye flush against
    // walls again. Calling the shared function is what makes this a real pin -
    // change either the constant or the formula and this fails.
    {
        const float skin = 0.15f;
        const float desired = 0.30f;
        const float coreAsksFor = desired + skin;
        const float hookTracesTo = D2HT::ReachForLean(coreAsksFor, skin);
        Check(std::fabs(hookTracesTo - (coreAsksFor + skin * 3.0f)) < 1e-6f,
              "the lean ray reaches core's request plus three times the standoff");
        Check(hookTracesTo > coreAsksFor + skin,
              "the ray passes the surface the lean would come to rest against rather "
              "than stopping on it");
    }

}

void LogThrottleTests() {
    std::cout << "Log throttle tests\n";

    D2HT::LogThrottle throttle(2, 4, 3, 5);
    std::string emitted;
    for (int frame = 1; frame <= 30; ++frame) {
        if (throttle.ShouldLog()) emitted += std::to_string(frame) + " ";
    }
    Check(emitted == "1 2 3 6 10 15 20 25 30 ", "the burst, early and steady tiers fire as before");

    // A throttle that has emitted nothing yet always writes its first line.
    D2HT::LogThrottle fresh(1, 1, 1000, 1000);
    Check(fresh.ShouldLog(), "the first call always emits");
    Check(!fresh.ShouldLog(), "the second waits for the interval");

    // The wall-clock gate the pose line and the two halves of the diagnostic
    // trace share. The first call always passes, so a line that exists to prove
    // a path runs at all is never withheld for an interval first, and the
    // interval is measured from the last line rather than from the last call.
    D2HT::IntervalGate gate;
    Check(gate.Due(5000, 1000), "the first call always passes");
    Check(!gate.Due(5999, 1000), "a call inside the interval does not");
    Check(gate.Due(6000, 1000), "a call on the interval does");
    Check(!gate.Due(6500, 1000), "and the interval restarts from the line that was written");
    // Each caller picks the interval per call: the pose line thins with the age
    // of the session and the trace interval is a config value that can change.
    Check(gate.Due(6600, 100), "a shortened interval takes effect immediately");
}

}  // namespace

// The rebasing the marker hook puts between the HUD and the engine's
// world-to-screen. Everything it does rests on one identity - a point rebased
// from the tracked view into the clean one has the same view-space coordinates
// in the clean view that the original point has in the tracked view - so that is
// what is checked, on a pose with every axis and a lean non-zero.
void ViewRebaseTests() {
    using namespace D2HT::voidengine;
    std::cout << "View rebase tests\n";

    const auto near3 = [](const Vec3& a, const Vec3& b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f &&
               std::fabs(a.z - b.z) < 1e-4f;
    };
    const auto viewCoords = [](const Vec3& p, const Vec3& origin, const Mat3& axis) {
        const float rel[3] = {p.x - origin.x, p.y - origin.y, p.z - origin.z};
        return Vec3{Dot(axis.Row(0), rel), Dot(axis.Row(1), rel), Dot(axis.Row(2), rel)};
    };

    const Vec3 cleanOrigin{12.0f, -4.0f, 1.7f};
    const Mat3 cleanAxis = RotateBasisLocal(Mat3{}, 1.1f, -0.3f, 0.05f);
    const Mat3 trackedAxis = RotateBasisLocal(cleanAxis, 0.5f, 0.2f, -0.15f);
    const Vec3 trackedOrigin = TranslateAlongBasis(cleanOrigin, cleanAxis, 0.3f, -0.2f, 0.1f);

    {
        const ViewDelta d = DeltaBetween(cleanOrigin, cleanAxis, trackedOrigin, trackedAxis);
        Vec3 origin;
        Mat3 axis;
        ApplyDelta(d, cleanOrigin, cleanAxis, origin, axis);
        bool same = near3(origin, trackedOrigin);
        for (int i = 0; i < 9; ++i) same = same && std::fabs(axis.m[i] - trackedAxis.m[i]) < 1e-4f;
        Check(same, "a delta put back on its own clean view gives the tracked view");
    }

    {
        // The same head pose on a clean view the mouse has since turned: the
        // tracked view has to turn with it, not stay where the old one was.
        const ViewDelta d = DeltaBetween(cleanOrigin, cleanAxis, trackedOrigin, trackedAxis);
        const Vec3 laterOrigin{13.0f, -4.5f, 1.7f};
        const Mat3 laterAxis = RotateBasisLocal(cleanAxis, -0.7f, 0.1f, 0.0f);
        Vec3 origin;
        Mat3 axis;
        ApplyDelta(d, laterOrigin, laterAxis, origin, axis);
        Check(IsOrthonormal(axis), "a delta moved onto another view stays orthonormal");
        const ViewDelta again = DeltaBetween(laterOrigin, laterAxis, origin, axis);
        bool same = std::fabs(again.forward - d.forward) < 1e-4f &&
                    std::fabs(again.left - d.left) < 1e-4f && std::fabs(again.up - d.up) < 1e-4f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                same = same && std::fabs(again.rotation[i][j] - d.rotation[i][j]) < 1e-4f;
        Check(same, "the head pose is the same relative to whichever clean view carries it");
    }

    const Vec3 marker{20.0f, 3.0f, 2.5f};
    {
        const Vec3 rebased =
            RebaseWorldPoint(marker, trackedOrigin, trackedAxis, cleanOrigin, cleanAxis);
        Check(near3(viewCoords(rebased, cleanOrigin, cleanAxis),
                    viewCoords(marker, trackedOrigin, trackedAxis)),
              "a rebased point sits in the clean view where the point sits in the tracked one");
        Check(!near3(viewCoords(marker, cleanOrigin, cleanAxis),
                     viewCoords(marker, trackedOrigin, trackedAxis)),
              "the pose used here really does move the point on screen");
    }

    {
        const Vec3 there = RebaseWorldPoint(marker, trackedOrigin, trackedAxis, cleanOrigin, cleanAxis);
        const Vec3 back = RebaseWorldPoint(there, cleanOrigin, cleanAxis, trackedOrigin, trackedAxis);
        Check(near3(back, marker), "rebasing there and back returns the point");
    }
}

// Publishing an object the caller resolved elsewhere. The worker sweeps only
// while nothing is published, so this is both how the engine's own pointer is
// handed to it and how a gigabyte of reads is kept off the machine while that
// pointer keeps answering.
void ResolverPublishTests() {
    std::cout << "Resolver publish tests\n";

    // Heap-owned and captured by value, like the test above: the worker is
    // detached, so anything it touches has to outlive this frame.
    auto sweeps = std::make_shared<std::atomic<int>>(0);
    D2HT::voidengine::ResolverThread worker([sweeps](const std::atomic<bool>&) -> void* {
        sweeps->fetch_add(1);
        return nullptr;
    });

    int object = 0;
    worker.Publish(&object);
    Check(worker.Published() == &object, "a published object is what the resolver reads back");

    worker.Clear();
    Check(worker.Published() == nullptr, "clearing the published object asks for another sweep");
}

int main() {
    std::cout << "Dishonored 2 Head Tracking Tests\n";
    std::cout << "================================\n";

    SanitizeTests();
    ConfigParseTests();
    ShippedIniTests();
    RoundTripTests();
    MissingFileTests();
    PortRangeTests();
    ResolverThreadTests();
    ResolverPublishTests();
    PortRecoveryTests();
    ZoomCompensationTests();
    VoidMathTests();
    ViewRebaseTests();
    PoseBoundaryTests();
    WorldTraceTests();
    LeanClampWiringTests();
    LogThrottleTests();

    if (g_failures == 0) {
        std::cout << "All tests passed!\n";
        return 0;
    }
    std::cout << g_failures << " test(s) FAILED\n";
    return 1;
}
