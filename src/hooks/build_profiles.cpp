// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "hooks/build_profiles.h"
#include "core/logging.h"

namespace D2HT {

using cameraunlock::memory::FingerprintMismatch;
using cameraunlock::memory::PeFingerprint;

// Newest first: index 0 is the diagnostic primary, the build an unrecognised EXE
// is compared against to say whether the user is ahead of this mod or behind it.
// The profiles themselves live in their per-store file and are appended to
// there, never edited.
const BuildProfile kKnownProfiles[] = {
    kSteamProfile_20250206,
};
const size_t kKnownProfileCount = sizeof(kKnownProfiles) / sizeof(kKnownProfiles[0]);

const BuildProfile* SelectBuildProfile() {
    PeFingerprint running{};
    if (!cameraunlock::memory::ReadPeFingerprint(GetModuleHandleA(nullptr), running)) {
        Log::Line("ERROR: Could not read PE fingerprint of running EXE");
        return nullptr;
    }

    for (size_t i = 0; i < kKnownProfileCount; ++i) {
        const BuildProfile& p = kKnownProfiles[i];
        if (running.Matches(p.fingerprint)) {
            Log::Line("Build profile matched: %s", p.name);
            return &p;
        }
    }

    const BuildProfile& primary = kKnownProfiles[0];
    switch (cameraunlock::memory::ClassifyMismatch(running, primary.fingerprint)) {
        case FingerprintMismatch::Newer:
            Log::Line(
                "WARN: Unrecognised Dishonored 2 build (TimeDateStamp 0x%08X) is NEWER than this "
                "mod knows (newest known %s, 0x%08X). Check for a mod update. Mod dormant.",
                running.TimeDateStamp, primary.name, primary.fingerprint.TimeDateStamp);
            break;
        case FingerprintMismatch::Older:
            Log::Line(
                "WARN: Unrecognised Dishonored 2 build (TimeDateStamp 0x%08X) is OLDER than the newest "
                "known build (%s). Let the store finish updating. Mod dormant.",
                running.TimeDateStamp, primary.name);
            break;
        case FingerprintMismatch::Differs:
            Log::Line(
                "WARN: Dishonored 2 EXE has a known TimeDateStamp but a different SizeOfImage/CheckSum "
                "(tampered or repacked). Mod will not engage on a modified binary.");
            break;
    }
    return nullptr;
}

} // namespace D2HT
