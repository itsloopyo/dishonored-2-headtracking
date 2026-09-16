// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <cstdint>

namespace D2HT {

// The per-frame log schedule the render-view diagnostic runs on: a burst of
// unconditional opening lines, then a dense interval while the run is still
// young, then a thin one for the rest of the session.
//
// The opening burst is where an install-time fault shows (a wrong offset, a
// profile that does not fit). The thin steady trickle is what a late report is
// read from - "it drifted after an hour", "it stopped when I loaded a save" -
// which a burst that goes silent cannot see. Thinning is also what keeps the
// render thread out of the log mutex, and what bounds the file.
//
// Counted in two currencies on purpose: `m_frame` advances every call so the
// intervals are frames, while `m_lines` advances only when a line is actually
// emitted, so the tiers are lines. Render-thread only, like its caller.
class LogThrottle {
public:
    constexpr LogThrottle(unsigned burst, unsigned earlyLines, unsigned earlyIntervalFrames,
                          unsigned steadyIntervalFrames)
        : m_burst(burst),
          m_earlyLines(earlyLines),
          m_earlyIntervalFrames(earlyIntervalFrames),
          m_steadyIntervalFrames(steadyIntervalFrames) {}

    // Call once per frame from the path being logged. True on the frames whose
    // line should be written.
    bool ShouldLog() {
        ++m_frame;
        if (m_lines < m_burst) {
            ++m_lines;
            return true;
        }
        const unsigned interval =
            m_lines < m_earlyLines ? m_earlyIntervalFrames : m_steadyIntervalFrames;
        if (m_frame % interval != 0) return false;
        ++m_lines;
        return true;
    }

private:
    const unsigned m_burst;
    const unsigned m_earlyLines;
    const unsigned m_earlyIntervalFrames;
    const unsigned m_steadyIntervalFrames;

    unsigned m_frame = 0;
    unsigned m_lines = 0;
};

// The same schedule counted in wall-clock milliseconds instead of frames, for
// the lines whose spacing has to mean something to a reader rather than to the
// renderer: the pose line and the two halves of the [Diagnostics] trace.
//
// The interval is passed per call rather than held, because the pose line
// thins with the age of the session and the trace interval is a config value a
// caller reads anyway. The first call always passes, so a line that exists to
// prove a path runs at all is never withheld for an interval first.
class IntervalGate {
public:
    bool Due(std::uint64_t now_ms, std::uint64_t interval_ms) {
        if (m_last != 0 && now_ms - m_last < interval_ms) return false;
        m_last = now_ms;
        return true;
    }

private:
    std::uint64_t m_last = 0;
};

}  // namespace D2HT
