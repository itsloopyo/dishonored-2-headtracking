// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <cameraunlock/memory/pe_fingerprint.h>

#include <cstdint>
#include <cstddef>

namespace D2HT {

// One shipped Dishonored2.exe build. Routing is by PE fingerprint, never by RVA,
// so a user on an un-patched build keeps matching their old profile when a new one
// is appended. See AGENTS.md "Maintain compatibility across new patches".
struct BuildProfile {
    const char* name;          // store-platform-YYYYMMDD, surfaces in the log line

    // PE fingerprint of Dishonored2.exe (authoritative routing key)
    cameraunlock::memory::PeFingerprint fingerprint;

    // idRenderView::Setup. Copies the game-supplied renderView_t (`g`, at offset 0
    // of idRenderView) into the render copy (`r`, at +0x39D0), then derives the
    // projection, view and MVP matrices from `r`. Writing `g` in a pre-hook is
    // therefore the whole render-phase injection: every matrix the frame is drawn
    // and culled with comes out of it.
    uint32_t render_view_setup_rva;

    // Byte offsets inside renderView_t, i.e. inside the game copy at offset 0.
    // Taken from the engine's own field-reflection registrar.
    uint32_t rv_vieworg_offset;   // idVec3, world position of the eye
    uint32_t rv_viewaxis_offset;  // idMat3, rows are forward / left / up

    // fov_x_RADIANS / fov_y_RADIANS. Full angles, in radians: the projection
    // builder refuses the frame unless both are positive, prints them multiplied
    // by 180/pi, and feeds fov_y through tan(fov_y * 0.5).
    uint32_t rv_fov_x_offset;
    uint32_t rv_fov_y_offset;

    // bool useExplicitProjectionMatrix: when set, the projection builder ignores
    // the two angles and copies a 4x4 out of renderView_t instead. Read so the
    // log cannot report a field of view the frame was not drawn with, and so the
    // zoom compensation stands down rather than scaling against a stale angle.
    uint32_t rv_explicit_projection_offset;

    // The float inside the ark_fieldOfView cvar object, which the game's own
    // field-of-view slider writes. The engine registers it as a horizontal field
    // of view in degrees that it then adjusts to fit the aspect ratio, and it is
    // the un-zoomed reference the pose is scaled against. Read every frame, so
    // moving the slider mid-session lands without a restart.
    uint32_t fov_setting_rva;

    // The constant idView::SetFovXandY multiplies tan(setting / 2) by to get
    // tan(fov_y / 2): 0.5625, which is 9/16, the setting being horizontal at
    // 16:9. Reading it rather than writing 0.5625 here keeps the number and the
    // build it came from together.
    //
    // It is why the compensation works in the VERTICAL axis and not the
    // horizontal one. fov_y descends from the setting and the aspect alone;
    // fov_x is then widened for the actual viewport, so on anything but a 16:9
    // display fov_x no longer equals the setting, and a factor built from those
    // two would sit at a constant fraction of 1.0 through all of normal play.
    uint32_t fov_vertical_tan_scale_rva;

    // The engine's own pointer to the live idGameLocal, in .data. Read first,
    // and the reason a level reload costs no head tracking: the object is freed
    // and rebuilt elsewhere on every reload, and this names the new one on the
    // next frame.
    uint32_t game_local_pointer_rva;

    // idGameLocal's vtable, in .rdata. What the pointer above is validated
    // against, and what the runtime sweep behind it keys on: the object is heap
    // allocated (0x341600 bytes) and this is the only thing in its first word
    // that identifies the class.
    uint32_t game_local_vtable_rva;

    // Byte offsets inside idGameLocal, from the engine's reflection tables.
    uint32_t game_gamestate_offset;   // gameState_t, GAMESTATE_ACTIVE while playing
    uint32_t game_logic_offset;       // embedded arkGameLogicManager m_gameLogic

    // How many bytes of arkGameLogicManager to scan for pointers to its states.
    uint32_t logic_manager_bytes;

    // Byte offsets inside arkGameLogicState, the base every concrete state
    // derives from, taken from the engine's reflection tables. The back-pointer
    // is what identifies a state object: a pointer inside the manager whose
    // target points back at the manager is one of its states and nothing else is.
    uint32_t state_manager_backref_offset;  // arkGameLogicManager* m_gameLogic
    uint32_t state_id_offset;               // arkGameLogicStateIds_t, this state's own id
    uint32_t state_active_offset;           // bool m_isStateActive

    // arkHUDElementCrosshair's per-frame update. Its two placement helpers are
    // NOT hooked: the one that parks the reticle centre runs once on the
    // transition into centred mode and then never again, so a hook on it would
    // correct the reticle for one frame in a session. The update runs every
    // frame, which is what a reticle that has to follow the head needs.
    uint32_t crosshair_update_rva;      // (element)

    // arkIggyAS3Object::SetAS3Value<float>(object, const float* value, member).
    uint32_t iggy_set_member_rva;

    // Byte offsets inside arkHUDElementCrosshair.
    uint32_t crosshair_object_offset;    // m_cachedCrosshairObject, _crosshair._container_mc
    uint32_t crosshair_x_member_offset;  // cached handle for that object's `x`
    uint32_t crosshair_y_member_offset;  // cached handle for that object's `y`
    uint32_t crosshair_cpnt_offset;      // m_cachedPlayerCrosshair, a checked component pointer

    // The bound Scaleform movie. Null between the widget being torn down and
    // bound again, and the element's own update and both of its placement
    // helpers refuse to touch the AS3 object while it is - the teardown path
    // releases the object at crosshair_object_offset as it clears this. Writing
    // through the released object is a crash, so it is checked here for the same
    // reason the engine checks it.
    uint32_t crosshair_movie_offset;

    // The engine's own latch for "the reticle is already parked at centre". It
    // places the reticle centre ONCE, on the transition into centred mode, sets
    // this, and then leaves the object alone until something moves it off
    // centre again. Clearing it is how a correction is handed back: without
    // that, the last position this mod wrote stays on screen for the rest of
    // the mission the moment it stops writing.
    uint32_t crosshair_centred_latch_offset;

    // arkComponent checked-pointer resolve: takes the checked pointer and returns
    // the component, or null when the generation stamp no longer matches.
    uint32_t checked_cpnt_resolve_rva;

    // Byte offsets inside arkCpntPlayerCrosshair.
    uint32_t cpnt_centred_offset;      // float, matches the constant below when centred
    uint32_t cpnt_world_point_offset;  // idVec3, the point the reticle marks

    // The float the element compares cpnt_centred_offset against to decide which
    // of its two placements to run. Read rather than assumed: taking the wrong
    // branch to the engine on every frame is silent, and this is the one value
    // in the reticle path that cannot be checked any other way.
    uint32_t cpnt_centred_value_rva;

    // The engine's own line check against the level, the one its third-person
    // camera collision uses. Signature is
    //   bool (unused, const idVec3* start, const idVec3* end, uint32_t requiredFlags,
    //         float* outDistance)
    // returning TRUE when the path is CLEAR and writing the distance from start
    // to the hit otherwise. It resolves the physics world from a global of its
    // own and its first argument is dead - the prologue overwrites that register
    // before reading it - so there is nothing to find and nothing to hold.
    //
    // It builds a Havok ray query and hands it to the dispatcher whose profiler
    // zone is named "TtWorldCastRay", with a filter whose predicate accepts a
    // body when its layer bit is in a fixed layer mask AND every bit of
    // requiredFlags is present in the body's own flags.
    uint32_t world_line_trace_rva;

    // The float inside the r_znear cvar object, the camera's near clip distance
    // in metres. Read so the collision standoff can be checked against it rather
    // than assumed: geometry closer to the eye than the near plane is culled, so
    // a wall held closer than this is still not drawn and the player still sees
    // through it.
    uint32_t znear_setting_rva;

    // The stage extents the HUD scales normalised device coordinates by:
    // x = 0.5 * ndc_x * stage_x, y = -0.5 * ndc_y * stage_y, which is the
    // engine's own arithmetic in the world-point placement helper. Read live
    // rather than assumed, because they follow the player's resolution.
    uint32_t stage_scale_x_rva;
    uint32_t stage_scale_y_rva;

    // arkEntityMarker's per-frame update, (marker), called by
    // arkEntityMarkerManager for every live marker. It projects the marker's
    // world point, clamps it to the screen edge, unprojects the clamped point
    // back into the world and hands the screen position to the HUD movie.
    uint32_t marker_update_rva;

    // The two projections that update uses, both (view, const idVec3* in,
    // idVec3* out). World-to-screen writes pixels and the engine's own depth;
    // screen-to-world takes the same three and writes a world point. Nothing
    // outside the marker code calls the first; the second has one other caller,
    // which is why both are only redirected inside the update above.
    uint32_t view_world_to_screen_rva;
    uint32_t view_screen_to_world_rva;

    // idView's (view, idVec3* origin, idMat3* axis) accessor. The marker update
    // reads it to decide whether a marker is behind the player, which way a
    // metre offset on the marker points and how far away the marker is.
    uint32_t view_origin_axis_rva;
};

// One profile per shipped build, appended, never edited in place: a user on an
// un-patched build has to keep matching the profile they already match. Declared
// here and defined in the per-store file (steam_offsets.cpp).
extern const BuildProfile kSteamProfile_20250206;

// Append-only registry. Newest build first (diagnostic primary).
extern const BuildProfile kKnownProfiles[];
extern const size_t kKnownProfileCount;

// Fingerprint the running EXE and return the matching profile, or nullptr if the
// build is unknown (mod stays dormant - never hook against stale RVAs).
const BuildProfile* SelectBuildProfile();

} // namespace D2HT
