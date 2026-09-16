// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <cstdint>

#include "voidengine/resolver_thread.h"

namespace D2HT::voidengine {

// Whether @p address sits on a committed, readable, non-guard page.
//
// SafeRead's filter handles EXCEPTION_ACCESS_VIOLATION and nothing else, so a
// read that lands on a PAGE_GUARD page raises STATUS_GUARD_PAGE_VIOLATION,
// declines the filter and unwinds out of whichever thread ran it - having
// already stripped the guard bit off the stack it hit, so that thread blows its
// stack later somewhere with no connection to the mod. Widening the filter in
// core would be the wrong repair for the same reason. Call this before
// dereferencing any address that came out of the game's own memory rather than
// one the mod chose.
bool SafeToDereference(std::uintptr_t address);

// gameState_t, in the engine's own declaration order. Only ACTIVE means a level
// is running; it is also the upper bound a candidate idGameLocal's gamestate has
// to sit inside to be believed.
enum GameStateValue {
    kGameStateUninitialized = 0,
    kGameStateNoMap = 1,
    kGameStateStartup = 2,
    kGameStateActive = 3,
    kGameStateShutdown = 4,
};

// arkGameLogicStateIds, the engine's own values. INGAME is the only one where
// the player is in control of the view: PAUSE is the pause menu, MAINMENU the
// shell, CINEMATIC and CONVERSATION the scripted scenes, JOURNAL / POWERWHEEL /
// READNOTE / BLACKMARKET the screens opened over a live level, MAP_TRANSITION a
// level load, and RESULTS / GAMEOVER the end-of-mission and death screens.
enum GameLogicState {
    kLogicStateNone = 0,
    kLogicStateInGame = 1,
    kLogicStatePause = 2,
    kLogicStateNotification = 3,
    kLogicStateReadNote = 4,
    kLogicStateCinematic = 5,
    kLogicStateMainMenu = 6,
    kLogicStateJournal = 7,
    kLogicStatePowerwheel = 8,
    kLogicStateConversation = 9,
    kLogicStateBlackMarket = 10,
    kLogicStateResults = 11,
    kLogicStateTutorial = 12,
    kLogicStateGameOver = 13,
    kLogicStateCharacterChoice = 14,
    kLogicStateMapTransition = 15,
    kLogicStateDemo = 16,
    kLogicStateMax = 17,
};

const char* GameLogicStateName(int state);

// What a candidate idGameLocal has to look like to be accepted.
//
// The vtable and an ACTIVE gameState_t are not enough on their own. The engine
// leaves stale copies of the object behind whose memory still carries the
// vtable, and a freed one whose gamestate word happens to still read ACTIVE
// passes both - after which the mod reads a dead object for the rest of the
// session. So a candidate also has to CONTAIN what a live one contains: at
// least one pointer inside its embedded arkGameLogicManager whose target points
// back at that manager, which is the same shape GameState identifies its states
// by and which recycled heap does not satisfy.
struct GameLocalShape {
    // The engine's own pointer to the live object. Zero on a build where it has
    // not been established, which leaves the sweep below as the only source.
    std::uint32_t pointer_rva;

    std::uint32_t vtable_rva;
    std::uint32_t gamestate_offset;
    std::uint32_t logic_offset;         // embedded arkGameLogicManager
    std::uint32_t logic_bytes;          // how much of it to scan
    std::uint32_t state_backref_offset; // arkGameLogicState::m_gameLogic
};

// Resolves the one live idGameLocal and keeps it resolved across map changes.
//
// Two sources, in order. The engine keeps a pointer to the live object in a
// global of its own, and that is read first: it costs two guarded reads, and it
// names the NEW object on the first frame after a level reload replaces it.
//
// The sweep behind it is the older source and the slower one by four orders of
// magnitude. The object is heap allocated - 0x341600 bytes from the engine's own
// allocator - so a scan of committed memory for its vtable finds every object of
// the class; several turn up, because the engine leaves stale copies whose memory
// still carries the vtable, and they are told apart by state rather than by
// address. A whole scan takes seconds, which is what the global is here to stop
// the player paying every time they reload a checkpoint.
//
// Nothing is written and nothing is called; the object is only read.
class GameLocalResolver {
public:
    // Starts sweeping immediately. The shape is copied into the worker, so
    // nothing the worker runs reaches back into this object.
    explicit GameLocalResolver(const GameLocalShape& shape);

    // The live object, or null while no level is loaded. Cheap: one atomic read
    // plus the revalidation, and it asks the worker to sweep again when the
    // object it had has gone. @p gamestate receives the gameState_t the
    // revalidation already read, so the caller does not pay for a second guarded
    // read of the same field on every rendered frame.
    void* Get(std::int32_t& gamestate);

private:
    // Says once which source the object came from, and - when the sweep had
    // already found one - whether the two agree. The global is new and the
    // sweep is the source every measurement so far was taken through, so the
    // log has to be able to answer "is the fast path the same object".
    void ReportSource(void* fromGlobal, void* swept);

    GameLocalShape m_shape;
    ResolverThread m_worker;
    bool m_sourceLogged = false;
};

}  // namespace D2HT::voidengine
