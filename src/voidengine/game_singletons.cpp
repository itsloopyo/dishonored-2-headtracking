// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "voidengine/game_singletons.h"

#include "core/logging.h"

#include <cameraunlock/memory/safe_memory.h>

#include <atomic>
#include <memory>

namespace D2HT::voidengine {

namespace {

// The largest region the sweep reads in one go. The game has allocations of
// several hundred megabytes and reading one whole is a large transient commit,
// so regions are walked in slices.
constexpr SIZE_T kSliceBytes = 4u << 20;

// Where Dishonored2.exe is loaded, which every RVA below is measured from.
// Resolved once: it is fixed for the life of the process, and the live-object
// check runs on the render thread on every rendered frame.
std::uintptr_t ExeBase() {
    static const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}

// How many rejected candidates the sweep reports before it goes quiet. Enough
// passes to show which read refused when an offset has moved, and short of the
// per-session flood an uncapped census becomes at one sweep every three seconds.
constexpr int kMaxRejectionReports = 40;

bool ReadState(std::uintptr_t object, const GameLocalShape& shape, std::int32_t& gamestate) {
    if (!cameraunlock::memory::SafeRead(object + shape.gamestate_offset, gamestate)) return false;
    return gamestate >= kGameStateUninitialized && gamestate <= kGameStateShutdown;
}

// Whether the object's embedded arkGameLogicManager holds at least one state.
//
// A state points back at the manager it belongs to, so a pointer inside the
// manager whose target points back at the manager is one of its states and
// recycled heap is not. One match is enough to tell a live object from a stale
// copy; GameState does the same walk in full to identify which states they are.
bool HasGameLogicStates(std::uintptr_t object, const GameLocalShape& shape) {
    const std::uintptr_t manager = object + shape.logic_offset;
    for (std::uint32_t off = 0; off + sizeof(void*) <= shape.logic_bytes; off += sizeof(void*)) {
        std::uintptr_t candidate = 0;
        if (!cameraunlock::memory::SafeRead(manager + off, candidate) || candidate == 0) continue;
        // The word just read is whatever the game happened to have there, so the
        // page it points at is checked before it is touched.
        if (!SafeToDereference(candidate + shape.state_backref_offset)) continue;
        std::uintptr_t backref = 0;
        if (!cameraunlock::memory::SafeRead(candidate + shape.state_backref_offset, backref)) {
            continue;
        }
        if (backref == manager) return true;
    }
    return false;
}

// The live object is the one running a level. The vtable is already a strong key
// - the only other places it turns up are register spills on a thread stack -
// and those read GAMESTATE_UNINITIALIZED. An ACTIVE gameState_t narrows it
// further, and the game-logic states settle it: a stale copy can keep both the
// vtable and a word that still reads ACTIVE, and accepting one of those latches
// a dead object for the whole session with no way back.
bool IsLiveCandidate(std::uintptr_t object, const GameLocalShape& shape) {
    std::int32_t gamestate = 0;
    if (!ReadState(object, shape, gamestate)) return false;
    const bool live = gamestate == kGameStateActive && HasGameLogicStates(object, shape);

    // Candidates are reported because the alternative when an offset moves is a
    // mod that says "no level loaded" for the whole session with nothing to say
    // which of the reads refused. Capped rather than left running, because the
    // sweep repeats every three seconds for as long as nothing is published: a
    // player sitting in the shell rewrites the same handful of rejections for as
    // long as they sit there. An acceptance is never dropped - it happens once,
    // and it is the line the sweep exists to reach. Sweep thread only, like its
    // caller.
    static int s_rejections = 0;
    if (live) {
        Log::Line("[state] idGameLocal candidate @ %p gamestate=%d with game-logic states "
                  "-> accepted", reinterpret_cast<void*>(object), gamestate);
    } else if (s_rejections < kMaxRejectionReports) {
        ++s_rejections;
        Log::Line("[state] idGameLocal candidate @ %p gamestate=%d -> rejected (not in a level, "
                  "or a stale copy with no game-logic states)%s",
                  reinterpret_cast<void*>(object), gamestate,
                  s_rejections == kMaxRejectionReports
                      ? "; no further rejected-candidate lines this session"
                      : "");
    }
    return live;
}

// The sweep reads into a heap buffer that is itself committed, readable memory,
// so a later pass would walk it and find a verbatim copy of whatever it held,
// vtable pointer included. An object address computed inside that copy is a
// phantom that evaporates the moment the buffer is refilled. Skipping the buffer
// itself is what keeps a sweep out of its own scratch space.
bool OverlapsSlice(std::uintptr_t sliceData, SIZE_T sliceSize, std::uintptr_t base, SIZE_T size) {
    return base < sliceData + sliceSize && sliceData < base + size;
}

// One pass over committed memory for objects carrying `vtable_rva`, accepting the
// first that `accept` recognises.
template <typename Accept>
void* SweepForVtable(std::uint32_t vtable_rva, const std::atomic<bool>& cancelled, Accept accept) {
    const std::uintptr_t vtable = ExeBase() + vtable_rva;

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto address = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto limit = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);

    // Typed as uintptr_t rather than bytes so the word-at-a-time scan below reads
    // the storage as the type it was written as, and lands aligned by
    // construction rather than by luck. Left UNINITIALISED on purpose: every word
    // the scan reads was written by the ReadProcessMemory below, and a container
    // that value-initialises would zero four megabytes on every sweep - once
    // every three seconds for as long as the player sits in the shell.
    const std::unique_ptr<std::uintptr_t[]> slice(
        new std::uintptr_t[kSliceBytes / sizeof(std::uintptr_t)]);
    const auto sliceData = reinterpret_cast<std::uintptr_t>(slice.get());

    while (address < limit) {
        // Checked per region, and again per slice below. A whole scan is seconds
        // long and a single region can be hundreds of megabytes, and teardown
        // asking the worker to stop has to be observed inside one rather than
        // after it: until the sweep returns, this module's code is still
        // executing.
        if (cancelled.load()) return nullptr;
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == 0) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::uintptr_t next = regionBase + region.RegionSize;

        // Masked rather than compared whole, so a readable page is still
        // recognised when the allocator has combined the access with
        // PAGE_NOCACHE or PAGE_WRITECOMBINE. PAGE_GUARD survives the mask on
        // purpose: touching a guard page raises. The target is a heap object, so
        // the copy-on-write and no-access protections are left out; the
        // executable and read-only ones are admitted anyway because a heap
        // allocator can hand back either after a reprotect, and the vtable
        // comparison rejects whatever they turn up at a cost of scan time only.
        const DWORD access = region.Protect & ~static_cast<DWORD>(PAGE_NOCACHE | PAGE_WRITECOMBINE);
        const bool readable =
            region.State == MEM_COMMIT &&
            (access == PAGE_READWRITE || access == PAGE_EXECUTE_READWRITE ||
             access == PAGE_READONLY);
        if (readable) {
            for (SIZE_T done = 0; done < region.RegionSize; done += kSliceBytes) {
                if (cancelled.load()) return nullptr;
                const SIZE_T remaining = region.RegionSize - done;
                const SIZE_T want = remaining < kSliceBytes ? remaining : kSliceBytes;
                const std::uintptr_t sliceBase = regionBase + done;
                if (OverlapsSlice(sliceData, kSliceBytes, sliceBase, want)) continue;
                // Zeroed here, and that initialiser is what makes the scan below
                // safe: the kernel writes the copied count on success and on a
                // partial copy, and does not touch it if it never got that far.
                SIZE_T got = 0;
                // The return value is deliberately not tested. A region can shrink
                // or be reprotected between the VirtualQuery above and this read,
                // which fails the call with ERROR_PARTIAL_COPY after it has
                // already filled `got` bytes. Those bytes are valid and the live
                // object may be among them.
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(sliceBase),
                                  slice.get(), want, &got);
                const SIZE_T count = got / sizeof(std::uintptr_t);
                for (SIZE_T i = 0; i < count; ++i) {
                    if (slice[i] != vtable) continue;
                    const std::uintptr_t object = sliceBase + i * sizeof(std::uintptr_t);
                    if (accept(object)) return reinterpret_cast<void*>(object);
                }
            }
        }
        if (next <= address) break;
        address = next;
    }
    return nullptr;
}

// Whether @p object still looks like the live idGameLocal: right vtable and a
// gameState_t in range. The state it read is handed back, because the caller
// wants it on every frame and reading it twice buys nothing.
bool IsLiveGameLocal(void* object, const GameLocalShape& shape, std::int32_t& gamestate) {
    if (object == nullptr) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(object);

    std::uintptr_t vptr = 0;
    if (!cameraunlock::memory::SafeRead(address, vptr)) return false;
    if (vptr != ExeBase() + shape.vtable_rva) return false;

    // Deliberately NOT the stricter live-candidate test: once the object is
    // known, leaving a level takes gamestate away from ACTIVE without freeing
    // the object, and re-sweeping on every menu screen would cost a gigabyte of
    // reads for a pointer that has not moved.
    return ReadState(address, shape, gamestate);
}

}  // namespace

namespace {

bool PageIsReadable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == 0) return false;
    if (region.State != MEM_COMMIT) return false;
    if ((region.Protect & PAGE_GUARD) != 0) return false;
    const DWORD access = region.Protect & ~static_cast<DWORD>(PAGE_NOCACHE | PAGE_WRITECOMBINE);
    return access == PAGE_READWRITE || access == PAGE_EXECUTE_READWRITE || access == PAGE_READONLY;
}

}  // namespace

bool SafeToDereference(std::uintptr_t address) {
    if (!PageIsReadable(address)) return false;
    // The caller reads a whole pointer, not a byte. An address in the last seven
    // bytes of a committed region has its read spill onto the next page, so the
    // last byte is checked too - otherwise the guard is merely shifted by a page
    // rather than closed. Same page in the common case, and VirtualQuery is not
    // called twice for it.
    const std::uintptr_t last = address + sizeof(void*) - 1;
    constexpr std::uintptr_t kPageMask = ~static_cast<std::uintptr_t>(0xFFF);
    if ((last & kPageMask) == (address & kPageMask)) return true;
    return PageIsReadable(last);
}

const char* GameLogicStateName(int state) {
    switch (state) {
        case kLogicStateNone:            return "no game logic state";
        case kLogicStateInGame:          return "gameplay";
        case kLogicStatePause:           return "the pause menu";
        case kLogicStateNotification:    return "a notification";
        case kLogicStateReadNote:        return "a note being read";
        case kLogicStateCinematic:       return "a cutscene";
        case kLogicStateMainMenu:        return "the main menu";
        case kLogicStateJournal:         return "the journal";
        case kLogicStatePowerwheel:      return "the power wheel";
        case kLogicStateConversation:    return "a conversation";
        case kLogicStateBlackMarket:     return "the black market";
        case kLogicStateResults:         return "the mission results screen";
        case kLogicStateTutorial:        return "a tutorial screen";
        case kLogicStateGameOver:        return "the death screen";
        case kLogicStateCharacterChoice: return "the character choice screen";
        case kLogicStateMapTransition:   return "a level load";
        case kLogicStateDemo:            return "the demo attract mode";
        default:                         return "an unrecognised game logic state";
    }
}

// The shape is captured BY VALUE, twice. Captured by reference, or through
// `this`, the worker reads a member of this object on every sweep - and the
// worker is detached, so a teardown landing mid-sweep reads it out of freed
// storage.
GameLocalResolver::GameLocalResolver(const GameLocalShape& shape)
    : m_shape(shape),
      m_worker([shape](const std::atomic<bool>& cancelled) {
          return SweepForVtable(shape.vtable_rva, cancelled, [shape](std::uintptr_t object) {
              return IsLiveCandidate(object, shape);
          });
      }) {}


void GameLocalResolver::ReportSource(void* fromGlobal, void* swept) {
    if (m_sourceLogged) return;
    m_sourceLogged = true;
    if (swept == nullptr || swept == fromGlobal) {
        Log::Line("[state] game object %p taken from the engine's pointer at +0x%X%s",
                  fromGlobal, m_shape.pointer_rva,
                  swept == nullptr ? "" : ", which is the one the sweep had found");
        return;
    }
    // Worth a line of its own rather than a footnote: the two sources disagreeing
    // means one of them is reading a stale copy, and which one decides whether
    // the game-state gate is trustworthy at all.
    Log::Line("[state] the engine's pointer at +0x%X names %p but the sweep found %p; the "
              "pointer is being used", m_shape.pointer_rva, fromGlobal, swept);
}

void* GameLocalResolver::Get(std::int32_t& gamestate) {
    gamestate = kGameStateUninitialized;
    void* swept = m_worker.Published();

    // The engine's own handle first. A level reload frees the object and builds
    // a new one somewhere else, and this names the new one on the very next
    // frame - where the sweep takes seconds to find it, during which the player
    // is walking around with no head tracking.
    std::uintptr_t fromGlobal = 0;
    if (m_shape.pointer_rva != 0 &&
        cameraunlock::memory::SafeRead(ExeBase() + m_shape.pointer_rva, fromGlobal) &&
        fromGlobal != 0 &&
        IsLiveGameLocal(reinterpret_cast<void*>(fromGlobal), m_shape, gamestate)) {
        ReportSource(reinterpret_cast<void*>(fromGlobal), swept);
        // The worker sweeps whenever nothing is published, so handing it the
        // object is what keeps a gigabyte of reads off the machine for as long
        // as the pointer keeps answering.
        m_worker.Publish(reinterpret_cast<void*>(fromGlobal));
        return reinterpret_cast<void*>(fromGlobal);
    }

    if (swept == nullptr) return nullptr;
    if (!IsLiveGameLocal(swept, m_shape, gamestate)) {
        // Freed under us by a map change. Clearing it is what asks the worker for
        // another sweep.
        m_worker.Clear();
        return nullptr;
    }
    return swept;
}

}  // namespace D2HT::voidengine
