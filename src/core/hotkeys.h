// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include "core/config.h"

#include <cameraunlock/input/hotkey_poller.h>

namespace D2HT {

// Nav-cluster hotkeys and their Ctrl+Shift chord aliases (AGENTS.md "Chord
// Alternatives"), polled on one core HotkeyPoller thread (~60Hz).
class Hotkeys {
public:
    // Registers the bindings and starts the poll thread. HotkeyPoller::Start
    // returns true or throws, so there is no failure to report here.
    void Start(const Config& cfg);

private:
    cameraunlock::input::HotkeyPoller m_poller;
};

} // namespace D2HT
