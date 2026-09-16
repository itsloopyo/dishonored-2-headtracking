// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "core/mod.h"
#include "core/logging.h"
#include "core/path_utils.h"
#include "core/constants.h"

#include <exception>
#include <process.h>

static unsigned __stdcall InitThread(void* /*lp*/) {
    // Quick guard: only run if we're inside Dishonored2.exe. ASI Loader can be
    // dragged into adjacent processes (launchers, helpers). Fail fast otherwise.
    const std::string exeName = D2HT::PathUtils::GetExeFileName();
    if (_stricmp(exeName.c_str(), D2HT::D2_GAME_EXE) != 0) {
        return 0;
    }

    // Open() truncates, after filing the outgoing session away as
    // HeadTracking.prev.log, so the log never grows across sessions.
    D2HT::Log::Open(D2HT::PathUtils::GetExePathW(D2HT::LOG_FILENAME));
    D2HT::Log::Line("Dishonored 2 Head Tracking v%s attached to %s",
                    D2HT::D2HT_VERSION, exeName.c_str());

    // Brief delay so the engine has set up its initial state before Initialize
    // fingerprints the EXE and installs the hooks.
    Sleep(500);

    // The bootstrap allocates, binds a socket and starts two threads, and every
    // one of those throws rather than returning a code: a std::thread that
    // cannot be created rethrows out of HotkeyPoller::Start, and so does the
    // receiver's supervisor. An exception leaving this function is the thread
    // entry point's, which is std::terminate - the game dies outright with
    // nothing in the log. Catching it leaves the player able to play.
    //
    // What the line must NOT claim is that the game is unmodified. The camera
    // detour is installed and armed partway through Initialize, and the throwing
    // calls that come after it - the hotkey thread, the window centring - leave
    // a live hook behind. Telling the player nothing was touched would send them
    // hunting a stutter or a crash somewhere it is not.
    try {
        D2HT::Mod::Instance().Initialize();
    } catch (const std::exception& e) {
        D2HT::Log::Line("ERROR: Mod initialization threw (%s). Anything it had already installed "
                        "is still in place; the rest is not. Send this log with a report.",
                        e.what());
        return 1;
    } catch (...) {
        D2HT::Log::Line("ERROR: Mod initialization threw. Anything it had already installed is "
                        "still in place; the rest is not. Send this log with a report.");
        return 1;
    }

    D2HT::Log::Line("Initialization complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // Pinned, so the reference count can never reach zero and this module is
        // never unmapped before the process ends. That is what makes the absent
        // DLL_PROCESS_DETACH handling correct rather than merely convenient:
        // the bootstrap thread can sit inside CenterGameWindowOnce for up to
        // thirty seconds, the singleton resolver's worker runs a sweep lasting
        // seconds, and an installed detour is entered from the render thread on
        // every frame. A FreeLibrary would unmap the code all three are
        // executing, and there is no wait short enough for the loader lock and
        // long enough to see them out.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

        const HANDLE thread = (HANDLE)_beginthreadex(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            // The one failure that can happen before the log file exists, and
            // its symptom - no head tracking and no HeadTracking.log at all - is
            // identical to the ASI loader never having loaded us, which is the
            // first thing any "mod not working" report has to rule out.
            // OutputDebugStringA is used because the log file does not exist yet
            // and this is still inside DllMain.
            OutputDebugStringA("Dishonored2HeadTracking: could not start its init "
                               "thread; the mod is not running.\n");
        }
    }
    // Nothing on DLL_PROCESS_DETACH. With the module pinned the only detach left
    // is process exit, where every other thread has already been forcibly
    // killed, possibly mid-lock, and joining one under the loader lock is the
    // classic DllMain deadlock. The OS reclaims all of it.
    return TRUE;
}
