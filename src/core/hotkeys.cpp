// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "core/hotkeys.h"
#include "core/mod.h"
#include "core/logging.h"

#include <cameraunlock/input/chord_hotkeys.h>

namespace D2HT {

void Hotkeys::Start(const Config& cfg) {
    using cameraunlock::input::ChordGuarded;
    using cameraunlock::input::NavGuarded;

    // Primary nav-cluster bindings. NavGuarded holds them off while Ctrl+Shift
    // is down so a layout that reaches a nav key through the chord letter fires
    // the action once, from the chord path, rather than through both.
    m_poller.AddHotkey(cfg.hotkey_toggle, NavGuarded([] { Mod::Instance().Toggle(); }));
    m_poller.AddHotkey(cfg.hotkey_tracking_mode, NavGuarded([] { Mod::Instance().CycleTrackingMode(); }));
    m_poller.AddHotkey(cfg.hotkey_yaw_mode, NavGuarded([] { Mod::Instance().ToggleYawMode(); }));

    // Chord aliases: Ctrl+Shift+Y/G/H
    m_poller.AddHotkey('Y', ChordGuarded([] { Mod::Instance().Toggle(); }));
    m_poller.AddHotkey('G', ChordGuarded([] { Mod::Instance().CycleTrackingMode(); }));
    m_poller.AddHotkey('H', ChordGuarded([] { Mod::Instance().ToggleYawMode(); }));

    m_poller.Start();

    Log::Line("Hotkeys ready: toggle=0x%02X mode=0x%02X yawmode=0x%02X "
              "+ Ctrl+Shift+Y/G/H chords",
              cfg.hotkey_toggle, cfg.hotkey_tracking_mode, cfg.hotkey_yaw_mode);
}

} // namespace D2HT
