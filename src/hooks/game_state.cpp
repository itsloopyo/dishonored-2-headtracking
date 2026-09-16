// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "hooks/game_state.h"

#include "core/logging.h"

#include <cameraunlock/memory/safe_memory.h>

namespace D2HT {

GameState::GameState(const BuildProfile& profile)
    : m_profile(profile),
      m_resolver({profile.game_local_pointer_rva, profile.game_local_vtable_rva,
                  profile.game_gamestate_offset,
                  profile.game_logic_offset, profile.logic_manager_bytes,
                  profile.state_manager_backref_offset}) {}

// The manager keeps its states as pointers, and each state points back at the
// manager it belongs to. That back-pointer is the whole identification: a
// pointer inside the manager whose target points back at the manager is one of
// its arkGameLogicState objects, and a pointer to anything else is not. The
// state's own id then says which one it is, so nothing has to be assumed about
// the order they are stored in.
void GameState::FindStates(std::uintptr_t gameLocal) {
    const std::uintptr_t manager = gameLocal + m_profile.game_logic_offset;
    m_manager = manager;
    m_states.clear();

    for (std::uint32_t off = 0; off + sizeof(void*) <= m_profile.logic_manager_bytes;
         off += sizeof(void*)) {
        std::uintptr_t candidate = 0;
        if (!cameraunlock::memory::SafeRead(manager + off, candidate)) continue;
        if (candidate == 0) continue;

        // The word just read is whatever the game had at that slot, so the page
        // it points at is checked before it is touched: SafeRead's filter does
        // not cover a guard page, and this walk runs on the render thread.
        if (!voidengine::SafeToDereference(candidate + m_profile.state_manager_backref_offset)) {
            continue;
        }
        std::uintptr_t backref = 0;
        if (!cameraunlock::memory::SafeRead(candidate + m_profile.state_manager_backref_offset,
                                            backref)) {
            continue;
        }
        if (backref != manager) continue;

        std::int32_t id = 0;
        if (!cameraunlock::memory::SafeRead(candidate + m_profile.state_id_offset, id)) continue;
        if (id <= voidengine::kLogicStateNone || id >= voidengine::kLogicStateMax) continue;

        m_states.push_back(State{candidate, id});
    }

    if (m_states.empty()) {
        // Latched, because IsGameplay re-runs this search on every rendered
        // frame for as long as it comes up empty: unlatched, the one line that
        // says which of the two halves failed is written sixty to a hundred and
        // forty times a second and buries the rest of the log within a minute.
        // Cleared again below, so a later level that fails says so.
        if (!m_noStatesLogged) {
            m_noStatesLogged = true;
            Log::Line("[state] idGameLocal found but none of its game-logic states could be "
                      "identified; tracking stays suspended");
        }
        return;
    }
    m_noStatesLogged = false;

    // Latched against the manager it was written for, because this search re-runs
    // on every rendered frame that the cached states come back stale: unlatched,
    // a run of stale frames writes this same line sixty to a hundred and forty
    // times a second and buries everything else, which is the fault the empty
    // branch above is already latched against. A different manager is a
    // different idGameLocal, and worth saying again.
    if (m_loggedManager == manager) return;
    m_loggedManager = manager;
    Log::Line("[state] %zu game-logic states identified", m_states.size());
}

bool GameState::IsGameplay() {
    // The resolver revalidates the pointer it publishes and hands back the
    // gameState_t it read doing so, so this frame's verdict costs one guarded
    // read of that field rather than two.
    std::int32_t gamestate = voidengine::kGameStateUninitialized;
    void* gameLocal = m_resolver.Get(gamestate);
    if (gameLocal == nullptr) {
        m_reason = "no level loaded";
        m_states.clear();
        m_manager = 0;
        m_loggedManager = 0;
        return false;
    }
    if (gamestate != voidengine::kGameStateActive) {
        // Cleared here as well as on the null path. The manager is EMBEDDED in
        // idGameLocal, so its address does not move when a level does, and the
        // re-find gate below would keep the previous level's cached state
        // pointers. Those objects are allocated with the level: after a map
        // change their storage is recycled, the guarded read still succeeds
        // because the page is still committed, and a non-zero byte where
        // m_isStateActive used to sit reports some unrelated state as active
        // for the rest of the session.
        m_reason = "not in a level";
        m_states.clear();
        m_loggedManager = 0;
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(gameLocal);

    // Re-found whenever the level changes, because the states are allocated with
    // the level and the manager they hang off is inside the idGameLocal the
    // resolver just handed over.
    if (m_states.empty() || m_manager != base + m_profile.game_logic_offset) {
        // Rate-limited when it keeps coming up empty, which is what a moved
        // offset looks like: the walk queries and reads 279 manager slots, and
        // retrying that on every rendered frame for the rest of the session
        // costs the render thread real time to reach the same answer.
        //
        // A level change is never rate-limited, and it is the !m_states.empty()
        // test below that guarantees it rather than the manager comparison
        // above: the cached states are still in hand when the manager address
        // changes, so the walk runs immediately. The counter only ever throttles
        // the case where the walk itself keeps failing.
        if (!m_states.empty() || m_findRetryFrames == 0) {
            FindStates(base);
            m_findRetryFrames = m_states.empty() ? kFindRetryIntervalFrames : 0;
        } else {
            --m_findRetryFrames;
        }
        if (m_states.empty()) {
            m_reason = "the game-logic states could not be read";
            return false;
        }
    }

    const ActiveStates active = ReadActiveStates();
    if (active.checked == 0) {
        // Every cached state failed its back-pointer check, so all of them are
        // recycled heap. Dropping them is what makes the gate above re-find them
        // next frame: the manager is EMBEDDED in idGameLocal, so its address
        // never moves and nothing else here can notice. Left in place, the
        // verdict would be "no game-logic state is active" for the rest of the
        // session, naming nothing the player did.
        m_states.clear();
        m_reason = "the game-logic states went stale";
        return false;
    }

    // A screen opened over a live level leaves the in-game state active
    // underneath it, so anything else being active outranks it. That is what
    // makes the power wheel and the journal suppress tracking rather than only
    // the pause menu, which is the one state that takes in-game down with it.
    if (active.other != voidengine::kLogicStateNone) {
        m_reason = voidengine::GameLogicStateName(active.other);
        return false;
    }
    if (!active.in_game) {
        m_reason = "no game-logic state is active";
        return false;
    }

    m_reason = "gameplay";
    return true;
}

// Which state is active is the engine's own answer to what the player is doing.
// Only ARK_GAME_LOGIC_STATE_INGAME is the player in control of the view: the
// pause menu, the journal, the power wheel, a note, the black market, a
// conversation, a cutscene, a level load and the results and death screens are
// all states of their own, and each one suppresses tracking.
GameState::ActiveStates GameState::ReadActiveStates() const {
    ActiveStates states;
    for (const State& state : m_states) {
        // The back-pointer is re-checked, not just read once when the state was
        // found. It is the only thing that tells a live state object from
        // recycled heap that happens to still be readable, and reading a
        // recycled byte as m_isStateActive is what pins tracking off with the
        // log naming a screen the player never opened.
        //
        // Deliberately NOT behind SafeToDereference, unlike the walk in
        // FindStates. That walk dereferences arbitrary words read out of the
        // manager; these addresses already passed the back-pointer and id tests
        // when they were recorded, so they are known heap objects rather than
        // guesses, and this runs on the render thread on every gameplay frame
        // where a VirtualQuery per state would be paid forever.
        std::uintptr_t backref = 0;
        if (!cameraunlock::memory::SafeRead(state.address + m_profile.state_manager_backref_offset,
                                            backref) ||
            backref != m_manager) {
            continue;
        }
        std::uint8_t active = 0;
        if (!cameraunlock::memory::SafeRead(state.address + m_profile.state_active_offset,
                                            active)) {
            continue;
        }
        ++states.checked;
        if (active == 0) continue;
        if (state.id == voidengine::kLogicStateInGame) {
            states.in_game = true;
        } else if (states.other == voidengine::kLogicStateNone) {
            states.other = state.id;
        }
    }
    return states;
}

}  // namespace D2HT
