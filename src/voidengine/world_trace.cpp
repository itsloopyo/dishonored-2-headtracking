// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "voidengine/world_trace.h"

#include <cmath>

namespace D2HT::voidengine {

void WorldTrace::Bind(std::uintptr_t base, std::uint32_t rva) {
    m_fn = rva == 0 ? nullptr : reinterpret_cast<TraceFn>(base + rva);
}

TraceResult WorldTrace::Line(const Vec3& start, const Vec3& direction, float max_distance,
                             std::uint32_t channel) const {
    TraceResult out;
    if (m_fn == nullptr) return out;
    if (!(max_distance > 0.0f) || !std::isfinite(max_distance)) return out;
    if (!IsFinite(start) || !IsFinite(direction)) return out;

    const Vec3 end{start.x + direction.x * max_distance, start.y + direction.y * max_distance,
                   start.z + direction.z * max_distance};

    // The helper leaves the out parameter untouched when nothing is hit, so it
    // is seeded rather than read back cold.
    float distance = 0.0f;
    const bool clear = m_fn(nullptr, &start, &end, channel, &distance);

    out.queried = true;
    if (clear) return out;

    // A hit at a non-finite or negative distance is a result nothing sensible
    // can be done with, and passing it on would put the eye behind the player.
    // Reported as a definite no-hit rather than as a failed query: the query DID
    // run, so saying otherwise would send a reader looking for a missing hook.
    if (!std::isfinite(distance) || distance < 0.0f) return out;

    out.blocked = true;
    out.distance = distance;
    return out;
}

}  // namespace D2HT::voidengine
