// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

namespace D2HT {

inline constexpr const char* D2HT_VERSION = "0.0.0";
inline constexpr const char* D2_GAME_EXE = "Dishonored2.exe";

// Truncated on every launch by Log::Open, which first files the outgoing
// session away as HeadTracking.prev.log - the crash handler writes its report
// into the live log and the player's next action after a crash is to relaunch,
// so one generation back is what makes that report survivable.
inline constexpr const char* LOG_FILENAME = "HeadTracking.log";
inline constexpr const char* CONFIG_FILENAME = "HeadTracking.ini";

// Nav-cluster default hotkey VKs (see AGENTS.md "Controls")
inline constexpr int DEFAULT_TOGGLE_KEY = 0x23;         // VK_END
inline constexpr int DEFAULT_TRACKING_MODE_KEY = 0x21;  // VK_PRIOR
inline constexpr int DEFAULT_YAW_MODE_KEY = 0x22;       // VK_NEXT

} // namespace D2HT
