// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "core/path_utils.h"

#include <cameraunlock/os/module_paths.h>

namespace D2HT::PathUtils {

namespace {

// The filenames widened here are the mod's own ASCII constants, so the only
// failure this can see is an empty name.
std::wstring Widen(const char* narrow) {
    const int length = MultiByteToWideChar(CP_ACP, 0, narrow, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, narrow, -1, &wide[0], length);
    wide.pop_back();
    return wide;
}

}  // namespace

std::string GetModDirectory() {
    // cameraunlock is a static library, so this resolves the .asi rather than
    // the EXE, and it grows its buffer instead of truncating at MAX_PATH - a
    // deep install path used to leave the config somewhere the player could not
    // find. Empty also means "no ANSI form" - an install path the ANSI code page
    // cannot spell - and is passed straight through: a "." fallback reads and
    // WRITES the ini in the process working directory, which is wherever the
    // launcher happened to leave it, while the log claims the file is next to
    // the game.
    return cameraunlock::os::SelfModuleDirectoryNarrow();
}

std::string GetExeFileName() {
    // The file name is split off the WIDE path and only then narrowed. Narrowing
    // the whole path first would refuse an install directory the ANSI code page
    // cannot spell - a Cyrillic user name on a Western code page - and this
    // string decides whether the mod runs at all.
    const std::wstring path = cameraunlock::os::ModuleFilePath(nullptr);
    const size_t separator = path.find_last_of(L"\\/");
    const std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);

    std::string narrow;
    if (!cameraunlock::os::NarrowToAnsi(name, narrow)) return {};
    return narrow;
}

std::wstring GetExePathW(const char* filename) {
    const std::wstring directory = cameraunlock::os::HostExeDirectory();
    if (directory.empty()) return {};
    const std::wstring name = Widen(filename);
    if (name.empty()) return {};
    return directory + L"\\" + name;
}

} // namespace D2HT::PathUtils
