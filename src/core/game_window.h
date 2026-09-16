// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

namespace D2HT {

// Centre the game's window on the monitor it is already on, once, if the game
// is running in true windowed mode. Exclusive fullscreen and borderless already
// fill the screen and are left alone, as is a window that is already centred.
//
// Blocks until the window exists and has stopped resizing, so it is called
// after the engine hooks are armed rather than before. That wait is why this is
// not cameraunlock::os::CenterGameWindowOnce: core's centres whatever window it
// finds on the spot, and the window Void Engine creates before its swap chain
// comes up is not the size the game ends up at.
void CenterGameWindowOnce();

} // namespace D2HT
