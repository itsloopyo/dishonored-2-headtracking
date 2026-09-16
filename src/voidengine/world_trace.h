// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <cstdint>

#include "voidengine/void_math.h"

namespace D2HT::voidengine {

// What one line check against the level found.
//
// `queried` and `blocked` are separate because a check that could not run is not
// a clear path. A clamp that has quietly stopped clamping looks exactly like one
// that never engaged, so the caller has to be able to tell them apart and say so.
struct TraceResult {
    bool queried = false;
    bool blocked = false;
    // From the start along the direction, in engine units, which are metres.
    // Only meaningful when blocked.
    float distance = 0.0f;
};

// The engine's own line check against the level, as the game itself uses it.
//
// Void Engine's physics is Havok, and every collision query in the image goes
// through one dispatcher whose profiler zone is named "TtWorldCastRay". The mod
// does not build a Havok query itself: the engine already wraps that dispatcher
// in a helper that takes two world points, a filter word and an out distance,
// and that helper is what is called here. It resolves the physics world from a
// global of its own, so nothing has to be found or held on to.
//
// The helper's own first argument is dead - its prologue overwrites the register
// before reading it - so there is no game object to supply and no lifetime to
// respect. That is worth stating because it is what makes this callable from a
// render-view hook at all.
//
// The filter word is a REQUIRED-flags mask, not a layer index. The engine's
// filter predicate accepts a body when its layer bit is in the filter's fixed
// layer mask AND every bit of this word is present in the body's own flags, so
// zero means "anything the layer mask lets through". The value the game passes
// for its own camera-versus-world line check is what the mod's collision default
// is set to.
class WorldTrace {
public:
    // @p rva is the helper's offset in the running image; zero leaves this
    // unbound and every Line call reports queried = false.
    void Bind(std::uintptr_t base, std::uint32_t rva);
    bool IsBound() const { return m_fn != nullptr; }

    // @p direction must be unit length. @p max_distance is in engine units.
    TraceResult Line(const Vec3& start, const Vec3& direction, float max_distance,
                     std::uint32_t channel) const;

private:
    // (unused, const idVec3* start, const idVec3* end, uint32_t requiredFlags,
    //  float* outDistance) -> true when the path is CLEAR.
    using TraceFn = bool(__fastcall*)(void*, const Vec3*, const Vec3*, std::uint32_t, float*);
    TraceFn m_fn = nullptr;
};

}  // namespace D2HT::voidengine
