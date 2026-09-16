// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include "hooks/build_profiles.h"
#include "voidengine/game_singletons.h"

#include <cstdint>
#include <vector>

namespace D2HT {

// Whether the player is in control of the view right now.
//
// Head tracking is suppressed everywhere else - the shell, a level load, the
// pause menu, a cutscene, a conversation, the journal, the power wheel, a note,
// the black market, the mission results and the death screen - so a screen the
// player is reading does not swim about while they look at the keyboard.
class GameState {
public:
    explicit GameState(const BuildProfile& profile);

    // A handful of guarded loads off a pointer another thread resolved, so this
    // is cheap enough for the per-frame call the render hook makes.
    bool IsGameplay();

    // The last state that was actually observed, in the log line's words.
    const char* LastReason() const { return m_reason; }

private:
    struct State {
        std::uintptr_t address;
        int id;
    };

    void FindStates(std::uintptr_t gameLocal);

    // What the engine's own state objects report right now: whether the in-game
    // state is active, the first OTHER state that is, or kLogicStateNone, and
    // how many of the cached states still looked like states at all.
    struct ActiveStates {
        bool in_game = false;
        int other = voidengine::kLogicStateNone;
        int checked = 0;
    };
    ActiveStates ReadActiveStates() const;

    const BuildProfile& m_profile;
    voidengine::GameLocalResolver m_resolver;
    const char* m_reason = "";

    // The manager the states below were found in, so a level change is noticed.
    std::uintptr_t m_manager = 0;
    std::vector<State> m_states;

    // Frames still to wait before another FindStates walk, while it keeps
    // coming up empty. Zero means the next call walks.
    static constexpr int kFindRetryIntervalFrames = 30;
    int m_findRetryFrames = 0;

    // Whether the "no states identified" line has already been written since
    // the last time states were found.
    bool m_noStatesLogged = false;

    // The manager the "states identified" line was last written for. Cleared
    // when the level goes away, so re-entering one says it again.
    std::uintptr_t m_loggedManager = 0;
};

}  // namespace D2HT
