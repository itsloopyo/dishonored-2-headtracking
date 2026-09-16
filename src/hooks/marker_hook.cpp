// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#include "pch.h"
#include "hooks/marker_hook.h"

#include "core/logging.h"
#include "core/mod.h"
#include "hooks/camera_hook.h"

#include <cameraunlock/hooks/hook_manager.h>

namespace D2HT {

namespace {

std::atomic<void(__fastcall*)(void*)> g_origUpdate{nullptr};
std::atomic<void(__fastcall*)(void*, const float*, float*)> g_origWorldToScreen{nullptr};
std::atomic<void(__fastcall*)(void*, const float*, float*)> g_origScreenToWorld{nullptr};
std::atomic<void(__fastcall*)(void*, float*, float*)> g_origOriginAxis{nullptr};

std::atomic<MarkerHook*> g_hook{nullptr};

// Set for the length of one marker update, on the thread running it. The
// accessor and the projections are ordinary engine functions with callers all
// over the game, and this is what keeps the tracked view out of every one of
// them but the marker's. No initialisers: see ViewDelta.
struct Scope {
    bool active;
    voidengine::ViewDelta delta;
};
thread_local Scope t_scope;

}  // namespace

bool MarkerHook::Install(const BuildProfile& profile, Mod& mod) {
    m_mod = &mod;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto& hooks = cameraunlock::hooks::HookManager::Instance();
    using cameraunlock::hooks::HookStatus;

    struct Target {
        const char* name;
        std::uint32_t rva;
        void* detour;
        void* original = nullptr;
    };
    Target targets[] = {
        {"view origin/axis accessor", profile.view_origin_axis_rva,
         reinterpret_cast<void*>(&MarkerHook::DetourOriginAxis)},
        {"world-to-screen", profile.view_world_to_screen_rva,
         reinterpret_cast<void*>(&MarkerHook::DetourWorldToScreen)},
        {"screen-to-world", profile.view_screen_to_world_rva,
         reinterpret_cast<void*>(&MarkerHook::DetourScreenToWorld)},
        // Last, so nothing turns the scope on until every function it redirects
        // is already hooked. A marker placed with the accessor tracked and the
        // projection clean is clamped against one view and drawn in another.
        {"marker update", profile.marker_update_rva,
         reinterpret_cast<void*>(&MarkerHook::DetourUpdate)},
    };

    // All or nothing, for the same reason.
    int created = 0;
    for (Target& t : targets) {
        const HookStatus status = hooks.CreateHook(reinterpret_cast<void*>(base + t.rva), t.detour,
                                                   &t.original);
        if (status != HookStatus::Ok) {
            Log::Line("[markers] could not hook the %s at +0x%X (%s); HUD markers stay where the "
                      "game puts them and will not follow a turned or leaned view",
                      t.name, t.rva, cameraunlock::hooks::HookStatusToString(status));
            for (int i = 0; i < created; ++i) {
                hooks.RemoveHook(reinterpret_cast<void*>(base + targets[i].rva));
            }
            return false;
        }
        ++created;
    }

    // Published before any hook is enabled: each detour's first act is to call
    // through its original.
    g_origOriginAxis.store(reinterpret_cast<void(__fastcall*)(void*, float*, float*)>(
                               targets[0].original),
                           std::memory_order_release);
    g_origWorldToScreen.store(
        reinterpret_cast<void(__fastcall*)(void*, const float*, float*)>(targets[1].original),
        std::memory_order_release);
    g_origScreenToWorld.store(
        reinterpret_cast<void(__fastcall*)(void*, const float*, float*)>(targets[2].original),
        std::memory_order_release);
    g_origUpdate.store(reinterpret_cast<void(__fastcall*)(void*)>(targets[3].original),
                       std::memory_order_release);
    g_hook.store(this, std::memory_order_release);

    for (int i = 0; i < created; ++i) {
        const HookStatus status = hooks.EnableHook(reinterpret_cast<void*>(base + targets[i].rva));
        if (status != HookStatus::Ok) {
            // The update is the last to enable, so a failure here leaves it
            // disabled and every redirect behind it inert.
            Log::Line("[markers] could not enable the %s hook at +0x%X (%s); HUD markers stay "
                      "where the game puts them",
                      targets[i].name, targets[i].rva,
                      cameraunlock::hooks::HookStatusToString(status));
            for (int j = 0; j < i; ++j) {
                hooks.DisableHook(reinterpret_cast<void*>(base + targets[j].rva));
            }
            return false;
        }
    }

    Log::Line("[markers] hooked the marker update at +0x%X; markers are placed through the "
              "head-tracked view", profile.marker_update_rva);
    return true;
}

void __fastcall MarkerHook::DetourUpdate(void* marker) {
    MarkerHook* const hook = g_hook.load(std::memory_order_acquire);
    const Scope saved = t_scope;
    t_scope.active = hook != nullptr && hook->CaptureDelta(t_scope.delta);
    g_origUpdate.load(std::memory_order_acquire)(marker);
    t_scope = saved;
}

bool MarkerHook::CaptureDelta(voidengine::ViewDelta& out) {
    const CameraHook* camera = m_mod->Camera();
    CameraHook::Frame frame;
    if (camera == nullptr || !camera->LastInjectedFrame(frame)) return false;
    out = voidengine::DeltaBetween(frame.clean_origin, frame.clean_axis, frame.render_origin,
                                   frame.render_axis);
    if (!m_activeLogged.exchange(true)) {
        Log::Line("[markers] first marker update with a tracked view");
    }
    return true;
}

bool MarkerHook::BuildViews(const float cleanOrigin[3], const float cleanAxis[9],
                            const voidengine::ViewDelta& delta, Views& out) {
    out.clean_origin = {cleanOrigin[0], cleanOrigin[1], cleanOrigin[2]};
    for (int i = 0; i < 9; ++i) out.clean_axis.m[i] = cleanAxis[i];
    if (!voidengine::IsOrthonormal(out.clean_axis) || !voidengine::IsFinite(out.clean_origin)) {
        if (!m_refusalLogged.exchange(true)) {
            Log::Line("[markers] the view accessor returned org=(%g %g %g) fwd=(%g %g %g), which is "
                      "not a camera; markers are left where the game puts them while this holds",
                      out.clean_origin.x, out.clean_origin.y, out.clean_origin.z,
                      out.clean_axis.m[0], out.clean_axis.m[1], out.clean_axis.m[2]);
        }
        return false;
    }
    voidengine::ApplyDelta(delta, out.clean_origin, out.clean_axis, out.tracked_origin,
                           out.tracked_axis);
    return true;
}

bool MarkerHook::ReadViews(void* view, const voidengine::ViewDelta& delta, Views& out) {
    float origin[3];
    float axis[9];
    g_origOriginAxis.load(std::memory_order_acquire)(view, origin, axis);
    return BuildViews(origin, axis, delta, out);
}

void __fastcall MarkerHook::DetourOriginAxis(void* view, float* origin, float* axis) {
    g_origOriginAxis.load(std::memory_order_acquire)(view, origin, axis);
    if (!t_scope.active) return;
    Views v;
    if (!g_hook.load(std::memory_order_acquire)->BuildViews(origin, axis, t_scope.delta, v)) return;
    origin[0] = v.tracked_origin.x;
    origin[1] = v.tracked_origin.y;
    origin[2] = v.tracked_origin.z;
    for (int i = 0; i < 9; ++i) axis[i] = v.tracked_axis.m[i];
}

void __fastcall MarkerHook::DetourWorldToScreen(void* view, const float* in, float* out) {
    const auto original = g_origWorldToScreen.load(std::memory_order_acquire);
    if (!t_scope.active) {
        original(view, in, out);
        return;
    }
    MarkerHook* const hook = g_hook.load(std::memory_order_acquire);
    Views v;
    if (!hook->ReadViews(view, t_scope.delta, v)) {
        original(view, in, out);
        return;
    }
    // Where the point sits in the tracked view, expressed as the point that
    // sits there in the clean one - which is the view the engine's matrices
    // were built for.
    const voidengine::Vec3 rebased = voidengine::RebaseWorldPoint(
        {in[0], in[1], in[2]}, v.tracked_origin, v.tracked_axis, v.clean_origin, v.clean_axis);
    const float point[3] = {rebased.x, rebased.y, rebased.z};
    original(view, point, out);

    // The pixel the game would have used next to the one it now gets. With the
    // head centred the two agree; turned or leaned, they differ by what the
    // world moved on screen.
    if (!hook->PlacementLogDue()) return;
    float clean[3];
    original(view, in, clean);
    Log::Line("[markers] point=(%.2f %.2f %.2f) clean px=(%.1f %.1f) -> tracked px=(%.1f %.1f) "
              "depth=%.4f",
              in[0], in[1], in[2], clean[0], clean[1], out[0], out[1], out[2]);
}

void __fastcall MarkerHook::DetourScreenToWorld(void* view, const float* in, float* out) {
    g_origScreenToWorld.load(std::memory_order_acquire)(view, in, out);
    if (!t_scope.active) return;
    Views v;
    if (!g_hook.load(std::memory_order_acquire)->ReadViews(view, t_scope.delta, v)) return;
    // The engine unprojected through the clean view; the screen position it
    // was given is a tracked one, so the point it found is carried back to
    // where it sits in the world as the tracked view sees it.
    const voidengine::Vec3 world = voidengine::RebaseWorldPoint(
        {out[0], out[1], out[2]}, v.clean_origin, v.clean_axis, v.tracked_origin, v.tracked_axis);
    out[0] = world.x;
    out[1] = world.y;
    out[2] = world.z;
}

bool MarkerHook::PlacementLogDue() {
    if (m_placementLog.ShouldLog()) return true;
    const long interval = m_mod->GetConfig().camera_trace_ms;
    return interval > 0 &&
           m_traceGate.Due(GetTickCount64(), static_cast<std::uint64_t>(interval));
}

}  // namespace D2HT
