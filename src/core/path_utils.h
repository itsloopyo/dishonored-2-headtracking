// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <string>

namespace D2HT::PathUtils {

// Directory containing the mod .asi, no trailing slash. EMPTY when it cannot be
// resolved or has no ANSI form, which a caller must treat as "there is no config
// to read" rather than substituting a relative path: core's contract is explicit
// that the alternative is a best-fit name pointing at somebody else's folder.
std::string GetModDirectory();

// File name of the running EXE, no directory. Empty when it cannot be resolved.
std::string GetExeFileName();

// Absolute wide path of a file next to the game EXE (Log::Open takes wide
// paths). The ASI loader can pick the .asi up from a scripts/ or plugins/
// subfolder, so this is not always the mod directory, and the log has to land
// where the player is told to look. Empty when the directory cannot be resolved.
std::wstring GetExePathW(const char* filename);

} // namespace D2HT::PathUtils
