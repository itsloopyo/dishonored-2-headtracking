// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "core/game_window.h"
#include "core/logging.h"

#include <cameraunlock/os/game_window.h>

namespace D2HT {

namespace {

constexpr DWORD kPollMillis = 250;

// The window comes up behind the startup logos, so the wait is generous.
// Giving up leaves the window exactly where the game put it.
constexpr std::uint64_t kWaitMillis = 30000;

// How many sightings of the same window at the same size count as settled. Four
// sightings is three intervals, so 750 ms of held size; two would have been a
// single 250 ms interval, which is not long enough to be worth calling settled.
// The engine creates its window and then resizes it when the swap chain comes
// up, and it can sit at the first size for longer than one poll interval while
// the startup logos play.
constexpr int kSettledPolls = 4;

// The window counts once kSettledPolls sightings agree on the window and its
// size. Centring the size it had before the swap chain came up leaves it
// visibly off-centre with nothing in the log to say why.
//
// A poll where the window cannot be found or measured is skipped rather than
// counted: it neither advances the tally nor resets it, so a window that
// blinks out for one poll and returns unchanged is still treated as settled.
bool WaitForSettledWindow(HWND& outHwnd, RECT& outRect) {
    const std::uint64_t started = GetTickCount64();
    HWND hwnd = nullptr;
    RECT rect{};
    int agreed = 0;
    for (;;) {
        HWND found = cameraunlock::os::FindGameWindow();
        RECT r{};
        if (found != nullptr && GetWindowRect(found, &r)) {
            if (found == hwnd && (r.right - r.left) == (rect.right - rect.left) &&
                (r.bottom - r.top) == (rect.bottom - rect.top)) {
                if (++agreed >= kSettledPolls) {
                    outHwnd = found;
                    outRect = r;
                    return true;
                }
            } else {
                // A different window or a new size restarts the count rather
                // than extending it, so a window still being resized never
                // accumulates agreement across the resize.
                agreed = 1;
            }
            hwnd = found;
            rect = r;
        }
        if (GetTickCount64() - started >= kWaitMillis) return false;
        Sleep(kPollMillis);
    }
}

}  // namespace

void CenterGameWindowOnce() {
    HWND hwnd = nullptr;
    RECT rect{};
    if (!WaitForSettledWindow(hwnd, rect)) {
        Log::Line("WARN: window: no settled game window after %llu ms - leaving it where it is",
                  kWaitMillis);
        return;
    }

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == 0) {
        Log::Line("window: borderless or exclusive fullscreen, leaving it where it is");
        return;
    }
    if ((style & (WS_MINIMIZE | WS_MAXIMIZE)) != 0) {
        Log::Line("window: minimised or maximised, leaving it where it is");
        return;
    }

    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;

    // NEAREST, not PRIMARY: on a multi-monitor desk the window belongs on the
    // display it is already on rather than jumping to the primary one.
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        Log::Line("WARN: window: GetMonitorInfoW failed (error %lu)", GetLastError());
        return;
    }
    const RECT& work = info.rcWork;

    LONG x = work.left + ((work.right - work.left) - width) / 2;
    LONG y = work.top + ((work.bottom - work.top) - height) / 2;
    // A window wider or taller than the work area centres to a negative offset,
    // which puts its title bar off-screen where it cannot be dragged back.
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;

    if (rect.left == x && rect.top == y) {
        Log::Line("window: already centred at %ld,%ld (size %ldx%ld)", x, y, width, height);
        return;
    }

    if (!SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        Log::Line("WARN: window: SetWindowPos failed (error %lu)", GetLastError());
        return;
    }
    Log::Line("window: centred windowed mode at %ld,%ld (size %ldx%ld)", x, y, width, height);
}

} // namespace D2HT
