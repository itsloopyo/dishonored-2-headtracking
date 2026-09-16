// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
//
// Every Steam build of Dishonored2.exe this mod knows about. One file per store,
// appended to and never edited in place: a user who has not taken a patch keeps
// matching the profile they already matched, and the PE fingerprint is what
// routes them there.
#include "pch.h"
#include "hooks/build_profiles.h"

namespace D2HT {

// ---------------------------------------------------------------------------
// Steam build, Dishonored2.exe TimeDateStamp 2025-02-06.
//
// Dishonored 2 runs on Arkane's Void Engine, an id Tech derivative, and its
// render view is id Tech's: an idRenderView holding a game copy of renderView_t
// at offset 0 and a render copy at +0x39D0.
//
// render_view_setup_rva is idRenderView::Setup, the only function referencing
// "idRenderView: MVP Matrix Invert failed!". It opens by copying the game copy
// over the render copy, then builds the projection from the render copy
// (+0x39D0+0x9C/+0xA0) and the world-to-view matrix from the render copy's
// vieworg and viewaxis (+0x39D0+0xA8, +0x39D0+0xCC) - so the game copy is what
// every matrix in the frame descends from.
//
// The renderView_t member offsets are the engine's own: it registers its fields
// for its type dump as fov_x_RADIANS +0x9C, fov_y_RADIANS +0xA0, vieworg +0xA8,
// viewaxis +0xCC and useExplicitProjectionMatrix +0x140. The view-matrix builder
// at RVA 0x109CC0 corroborates the last two by reading exactly +0xA8 and +0xCC,
// and it is also where the row order comes from: it writes the view's first row
// as the negated second viewaxis row, so the rows are forward / left / up.
//
// idGameLocal is allocated (0x341600 bytes) and the engine keeps its pointer to
// it at RVA 0x39A5290. Two of its own paths to the same member settle what that
// global holds: the crosshair's world-point placement (0x672E20) reaches the
// object through the HUD element and takes the view list at +0x1F52B8 off it,
// while 0x639120 takes the view list at the same +0x1F52B8 off the global. The
// level line check (0x638570) reads the physics world at +0x60410 off it, and
// the HUD reads the screen size at +0x340EA8 - all inside the object's 0x341600
// bytes. Its member offsets are from the engine's reflection tables: m_gamestate
// +0x341058 and an EMBEDDED arkGameLogicManager m_gameLogic +0x1F6EC8, 0x8B8
// bytes long.
//
// The manager keeps no reflected current-state field, but every arkGameLogicState
// does carry three: a back-pointer to its manager at +0x08, its own
// arkGameLogicStateIds_t at +0x18 and bool m_isStateActive at +0x40. The states
// are found by scanning the manager for pointers whose target points back at the
// manager, which is a shape nothing else in that block satisfies.
//
// The field of view the frame is drawn with is renderView_t's own fov_y_RADIANS,
// and the un-zoomed reference it is measured against is the cvar registered at
// RVA 0x1392B10 as "ark_fieldOfView", which its registration describes as a
// horizontal field of view in degrees that the engine then adjusts to fit the
// aspect ratio. The game's settings screen binds a widget straight to that cvar
// object, so it is the slider's own value; the float sits at +0x2C inside it.
//
// idView::SetFovXandY (RVA 0x890B50) is where the two meet, and it is where the
// 0.5625 comes from: it writes fov_y = 2 * atan(tan(setting / 2) * 0.5625) and
// then widens fov_x from fov_y for the viewport's actual aspect. The engine
// keeps the same ratio itself, at RVA 0x88E0C0, as ark_fieldOfView divided by
// whichever fov is live.
//
// The reticle side is arkHUDElementCrosshair. Its per-frame update (RVA 0x672F70,
// the element's third virtual) returns immediately unless the Scaleform movie
// bound at +0x98 is live, then resolves the player's crosshair component and
// either projects the world point at that component's +0x38 or parks the reticle
// at the centre of the stage, latching the centred case on the byte at +0x94.
// Either way it writes `x` and `y` on the Scaleform object cached at +0x3A8
// through the member handles at +0x660 and +0x670, using the AS3 float setter at
// RVA 0x2A2C0, and it scales normalised coordinates by the two stage extents.
//
// The line check at RVA 0x638570 is the engine's own, taken from its third-person
// camera collision at 0x638AE0, which calls it as
// (unused, start, end, 0x800, &distance) against the level and a sibling shape
// cast with 0x20000 against characters. It builds a Havok ray and hands it to the
// dispatcher whose profiler zone is the string "TtWorldCastRay" (0xC89B00),
// resolving the physics world itself from the global at 0x39A5290 + 0x60410, and
// its own first argument is dead: the prologue loads that world into RCX before
// reading the incoming one. The filter predicate (0x996D0) accepts a body when
// its layer bit is in the filter's fixed 0x2F layer mask AND every bit of the
// caller's word is present in the body's own flags, so 0x800 is a required-flags
// mask rather than a layer index and 0 would widen the query to the whole mask.
//
// r_znear registers at RVA 0x1376EE0 with a range of 0.001 to 3.125, into the
// cvar object at 0x39373E0. Its float sits at +0x2C, the same place
// ark_fieldOfView's does.
//
// HUD markers are arkEntityMarker objects owned by arkEntityMarkerManager, whose
// update (RVA 0x53AA20, the one that sorts with
// arkEntityMarkerManager::sortMarkersByDist) calls the marker update at 0x677B10
// for every live marker. That update reads the player's idView through the
// origin / axis accessor at 0x88E140 (+0xA8 and +0xCC, the renderView_t
// members), projects with 0x6DDA50 - the view's MVP, then
// x = (ndc_x + 1) * width / 2, y = (1 - ndc_y) * height / 2 - clamps to the
// screen rectangle or ellipse, unprojects with 0x6DDA00 through the inverted MVP,
// and writes the screen position to the movie at 0x53A2F0.
// ---------------------------------------------------------------------------
const BuildProfile kSteamProfile_20250206 = {
    .name = "steam-win64-20250206",
    .fingerprint =
        {
            0x67A50CDE,  // TimeDateStamp
            0x041CB000,  // SizeOfImage
            0x027436C9,  // CheckSum
        },

    .render_view_setup_rva = 0x374E40,  // idRenderView::Setup

    .rv_vieworg_offset = 0xA8,
    .rv_viewaxis_offset = 0xCC,
    .rv_fov_x_offset = 0x9C,
    .rv_fov_y_offset = 0xA0,
    .rv_explicit_projection_offset = 0x140,

    .fov_setting_rva = 0x39A40CC,          // ark_fieldOfView, what the FOV slider writes
    .fov_vertical_tan_scale_rva = 0x1ACA860,  // tan(fov_y/2) = 0.5625 * tan(setting/2)

    .game_local_pointer_rva = 0x39A5290,
    .game_local_vtable_rva = 0x1C940A8,
    .game_gamestate_offset = 0x341058,
    .game_logic_offset = 0x1F6EC8,  // embedded arkGameLogicManager
    .logic_manager_bytes = 0x8B8,

    .state_manager_backref_offset = 0x08,
    .state_id_offset = 0x18,
    .state_active_offset = 0x40,

    .crosshair_update_rva = 0x672F70,  // arkHUDElementCrosshair::Update
    .iggy_set_member_rva = 0x2A2C0,    // arkIggyAS3Object::SetAS3Value<float>

    .crosshair_object_offset = 0x3A8,
    .crosshair_x_member_offset = 0x660,
    .crosshair_y_member_offset = 0x670,
    .crosshair_cpnt_offset = 0x68,
    .crosshair_movie_offset = 0x98,
    .crosshair_centred_latch_offset = 0x94,

    .checked_cpnt_resolve_rva = 0x3DA910,

    .cpnt_centred_offset = 0x20,
    .cpnt_world_point_offset = 0x38,
    .cpnt_centred_value_rva = 0x1F697B0,

    .world_line_trace_rva = 0x638570,
    .znear_setting_rva = 0x393740C,  // r_znear cvar object 0x39373E0, float at +0x2C

    .stage_scale_x_rva = 0x3989EAC,
    .stage_scale_y_rva = 0x3989F2C,

    .marker_update_rva = 0x677B10,           // arkEntityMarker per-frame update
    .view_world_to_screen_rva = 0x6DDA50,
    .view_screen_to_world_rva = 0x6DDA00,
    .view_origin_axis_rva = 0x88E140,
};

}  // namespace D2HT
