// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <cmath>

// Void Engine is an id Tech derivative, and its world keeps id Tech's
// conventions: right-handed, +X forward, +Y left, +Z up. An idMat3 is nine
// floats, row-major, and renderView_t::viewaxis holds its rows in the order
// forward / left / up.
//
// That row order is not assumed. The engine's own world-to-view builder (RVA
// 0x109CC0, called from idRenderView::Setup) writes its first row as the
// NEGATED second viewaxis row, its second row as the third, and its third as
// the negated first - so view x is right, view y is up, view z is backward,
// and the three viewaxis rows are forward, left, up in that order.
namespace D2HT::voidengine {

// Written out once here rather than at each call site. Every angle in this file
// arrives from the tracker in degrees and reaches the engine in radians, and a
// second copy of the literal is a second place for a digit to go missing.
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDegToRad = kPi / 180.0f;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    float* Row(int i) { return m + i * 3; }
    const float* Row(int i) const { return m + i * 3; }
};

inline float Dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline bool IsFinite3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// The same check for a Vec3, by member rather than by walking off &v.x: the
// three floats are separate members, so indexing off the first is only a valid
// array walk by convention. IsFinite3 above stays for Mat3::Row, which really
// does hand back an array.
inline bool IsFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// How far from unit length, and from perpendicular, a basis may be and still be
// something the engine will not choke on. Loose enough to pass a float basis the
// engine built itself, tight enough to refuse a garbage read.
inline constexpr float kOrthonormalTolerance = 0.01f;

// Three finite, unit-length, mutually perpendicular rows. Checked on the way IN
// as well as on the way out, so a frame where the engine handed the hook
// something unexpected is passed through untouched rather than turned into a
// camera pointing at nothing.
inline bool IsOrthonormal(const Mat3& axis) {
    for (int i = 0; i < 3; ++i) {
        if (!IsFinite3(axis.Row(i))) return false;
        if (std::fabs(Dot(axis.Row(i), axis.Row(i)) - 1.0f) > kOrthonormalTolerance) return false;
    }
    return std::fabs(Dot(axis.Row(0), axis.Row(1))) < kOrthonormalTolerance &&
           std::fabs(Dot(axis.Row(0), axis.Row(2))) < kOrthonormalTolerance &&
           std::fabs(Dot(axis.Row(1), axis.Row(2))) < kOrthonormalTolerance;
}

// tan of half the VERTICAL field of view the engine derives from a setting of
// @p setting_degrees, given the build's own conversion constant.
//
// idView::SetFovXandY writes fov_y = 2 * atan(tan(setting / 2) * scale) and then
// widens fov_x out of fov_y for the viewport's aspect, so this is the one axis
// where the setting and the rendered frame can be compared without the display's
// shape coming into it. The setting is the ark_fieldOfView cvar, in degrees, and
// the engine calls it horizontal - which it is, at 16:9, the scale being 9/16.
//
// Both arguments come out of game memory, so the caller checks them.
inline float TanHalfVerticalFovFromSetting(float setting_degrees, float scale) {
    constexpr float kHalfDegToRad = kPi / 360.0f;
    return scale * std::tan(setting_degrees * kHalfDegToRad);
}

// One metre in Void Engine units.
//
// Measured, not assumed, and NOT inherited from the id Tech games this engine
// descends from: those measure in inches, and Void Engine does not. Crouching in
// game drops renderView_t::vieworg.z by 0.7, which is a crouch in metres and
// nothing at all in inches, so the world is metric and the scale is 1.
//
// It was 39.3701 for one build, carried over from this project's own mod for
// the id Tech 5 sibling named below. That is the kind of mistake this comment
// exists to stop being made twice: a lean of 0.30 came out as 11.8 world units
// and threw the camera through the wall behind the player.
inline constexpr float kUnitsPerMetre = 1.0f;

// The tracker pose as the engine wants it: radians, and engine units along the
// basis rows forward / left / up.
struct EnginePose {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float forward = 0.0f;
    float left = 0.0f;
    float up = 0.0f;
};

// The protocol-to-engine conversion, done once, here, at the boundary where the
// tracker's convention meets Void Engine's - never as a user setting.
//
// Inherited verbatim from this project's own mod for Wolfenstein: The New
// Order, an id Tech 5 sibling whose camera boundary is the same renderView_t
// with the same forward / left / up rows, and whose signs were settled in a
// running game. Only the metres-per-unit scale differs, and that difference is
// the whole of kUnitsPerMetre above.
//
//   yaw:      the tracker calls a head turned RIGHT positive, RotateBasisLocal
//             calls a view turned LEFT positive, so it is negated.
//   pitch,
//   roll:     pass through. Roll passing through is the measured answer, not an
//             omission: the fleet default negates yaw, roll and x, and on this
//             engine family it was watched in a running game (Wolfenstein: The
//             New Order, 2026-09-06) and roll and x came out MIRRORED under that
//             default, so both negations were dropped and the boundary negates
//             yaw and z only. RotateBasisLocal's positive roll already tilts the
//             up row toward LEFT, which is where the second negation went.
//   forward:  the processor reports NEGATIVE z for a head leaning in, and the
//             basis's first row is forward, so forward is -z.
//   left:     the processor's +x is a head moved right and the basis's second
//             row is LEFT, so x passes through as left - which is the same
//             negation every mod in the fleet applies against a +x-is-right
//             engine, arrived at by the row order rather than by a minus sign.
//   up:       the processor's +y is up and so is the third row.
//
// @p x, @p y and @p z arrive in metres, already clamped by the processor's
// asymmetric forward/back limits, and are scaled to engine units here.
inline EnginePose PoseToEngine(float yawDegrees, float pitchDegrees, float rollDegrees, float x,
                               float y, float z) {
    EnginePose out;
    out.yaw = -yawDegrees * kDegToRad;
    out.pitch = pitchDegrees * kDegToRad;
    out.roll = rollDegrees * kDegToRad;
    out.forward = -z * kUnitsPerMetre;
    out.left = x * kUnitsPerMetre;
    out.up = y * kUnitsPerMetre;
    return out;
}

namespace detail {

// out = a * b, both row-major. The composition below chains two of these, and
// the second is the first with different operands - which is exactly the shape a
// transcription error hides in.
inline void Multiply3x3(const float a[3][3], const float b[3][3], float out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

}  // namespace detail

// Rotates an orthonormal basis whose rows are forward / left / up, by angles
// expressed in that same basis. Radians.
//
//   yaw   > 0 turns the view LEFT   (right-hand rule about the up row)
//   pitch > 0 raises the view       (about the negated left row, because a
//                                    right-handed turn about +left lowers it)
//   roll  > 0 tilts the up row toward the LEFT row, which is what a head
//             tilted LEFT does
//
// Composition is yaw outermost then pitch then roll, matching the shared
// yaw * pitch * roll order every mod in the fleet applies, so a reticle
// projection derived from it can reuse the same decomposition.
//
// The rotation is built in the basis's own coordinates and then mapped back
// through the basis, which is what makes it camera-local: the resulting rows
// are combinations of the incoming rows, so an axis that was already banked
// stays banked.
inline Mat3 RotateBasisLocal(const Mat3& axis, float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll),  sr = std::sin(roll);

    // L[i][j] is how much of the old row j the new row i is made of.
    // Yaw about up (row 2):     f -> c f + s l,  l -> -s f + c l
    // Pitch about left (row 1): f -> c f + s u,  u -> -s f + c u   (positive = up)
    // Roll about forward (0):   l -> c l - s u,  u ->  s l + c u   (up tilts toward +left)
    const float Ly[3][3] = {{cy, sy, 0}, {-sy, cy, 0}, {0, 0, 1}};
    const float Lp[3][3] = {{cp, 0, sp}, {0, 1, 0}, {-sp, 0, cp}};
    const float Lr[3][3] = {{1, 0, 0}, {0, cr, -sr}, {0, sr, cr}};

    // L maps OLD-basis coordinates to NEW-basis coordinates, so it is the
    // INVERSE of the frame rotation - which reverses the order the three
    // factors go in. For the frame to turn by yaw * pitch * roll with roll
    // innermost, L has to be Lr * Lp * Ly. Written the other way round every
    // single-axis case still looks right and only a combined pose is wrong,
    // with roll swinging where the camera points.
    float Lrp[3][3];
    detail::Multiply3x3(Lr, Lp, Lrp);
    float L[3][3];
    detail::Multiply3x3(Lrp, Ly, L);

    Mat3 out;
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c)
            out.m[i * 3 + c] = L[i][0] * axis.m[0 * 3 + c] + L[i][1] * axis.m[1 * 3 + c] +
                               L[i][2] * axis.m[2 * 3 + c];
    return out;
}

// Yaw applied about WORLD up (+Z) instead of about the basis's own up row, with
// pitch and roll still camera-local. Keeps a glance left level while the player
// is looking steeply up or down, which is the whole point of the world-yaw mode:
// with a camera-local yaw, looking down a stairwell and turning the head spins
// the world about the view axis instead of sweeping it sideways.
inline Mat3 RotateBasisWorldYaw(const Mat3& axis, float yaw, float pitch, float roll) {
    const Mat3 local = RotateBasisLocal(axis, 0.0f, pitch, roll);

    const float c = std::cos(yaw), s = std::sin(yaw);
    // Right-hand rule about world +Z: x -> c x - s y, y -> s x + c y. That turns
    // +X (forward) toward +Y (left), so positive yaw looks left here too.
    Mat3 out;
    for (int i = 0; i < 3; ++i) {
        const float* r = local.Row(i);
        out.m[i * 3 + 0] = c * r[0] - s * r[1];
        out.m[i * 3 + 1] = s * r[0] + c * r[1];
        out.m[i * 3 + 2] = r[2];
    }
    return out;
}

// Moves an eye along the rows of a basis. `forward`, `left` and `up` are in
// engine units and are measured in `axis`, so passing the CLEAN basis is what
// makes a lean follow where the body faces rather than where the head is
// looking: turn your head and lean in, and you move along your shoulders'
// forward, not along the new line of sight.
inline Vec3 TranslateAlongBasis(const Vec3& origin, const Mat3& axis, float forward, float left,
                                float up) {
    // Written out per component rather than looped through `&out.x`: the three
    // floats are separate members, so indexing off the first of them is only a
    // valid array walk by convention, and the loop bought nothing over the three
    // lines it hid.
    Vec3 out;
    out.x = origin.x + axis.m[0] * forward + axis.m[3] * left + axis.m[6] * up;
    out.y = origin.y + axis.m[1] * forward + axis.m[4] * left + axis.m[7] * up;
    out.z = origin.z + axis.m[2] * forward + axis.m[5] * left + axis.m[8] * up;
    return out;
}

// What the head did to one frame's view, held relative to that frame's clean
// view so it can be put onto a different one.
//
// Anything that reads the view a frame after the camera hook published it sees
// a clean view that has already moved on with the mouse. Carrying the tracked
// view across as absolute numbers would drag it a frame behind the player's
// turn; carrying the head's rotation and lean across and re-applying them to the
// view that is live now does not.
//
// Trivially constructible on purpose: it is held in a thread_local, and a DLL
// loaded into a running process cannot count on dynamic initialisation of one
// on threads that already exist.
struct ViewDelta {
    // Rows of the tracked basis in coordinates of the clean one.
    float rotation[3][3];
    // The lean, forward / left / up along the clean basis.
    float forward, left, up;
};

inline ViewDelta DeltaBetween(const Vec3& cleanOrigin, const Mat3& cleanAxis,
                              const Vec3& trackedOrigin, const Mat3& trackedAxis) {
    ViewDelta d{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) d.rotation[i][j] = Dot(trackedAxis.Row(i), cleanAxis.Row(j));
    const float lean[3] = {trackedOrigin.x - cleanOrigin.x, trackedOrigin.y - cleanOrigin.y,
                           trackedOrigin.z - cleanOrigin.z};
    d.forward = Dot(cleanAxis.Row(0), lean);
    d.left = Dot(cleanAxis.Row(1), lean);
    d.up = Dot(cleanAxis.Row(2), lean);
    return d;
}

inline void ApplyDelta(const ViewDelta& d, const Vec3& cleanOrigin, const Mat3& cleanAxis,
                       Vec3& trackedOrigin, Mat3& trackedAxis) {
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c)
            trackedAxis.m[i * 3 + c] = d.rotation[i][0] * cleanAxis.m[c] +
                                       d.rotation[i][1] * cleanAxis.m[3 + c] +
                                       d.rotation[i][2] * cleanAxis.m[6 + c];
    trackedOrigin = TranslateAlongBasis(cleanOrigin, cleanAxis, d.forward, d.left, d.up);
}

// The world point that sits in view @p to exactly where @p p sits in view
// @p from.
//
// This is how a projection built for one view is made to answer for another
// without rebuilding it: the engine's world-to-screen for the clean view, fed
// RebaseWorldPoint(p, tracked, clean), gives the pixel and depth p has in the
// tracked view, in the engine's own screen and depth conventions. Swapping the
// two views takes a point the engine unprojected through the clean view back to
// where it is in the world as seen from the tracked one.
inline Vec3 RebaseWorldPoint(const Vec3& p, const Vec3& fromOrigin, const Mat3& fromAxis,
                             const Vec3& toOrigin, const Mat3& toAxis) {
    const float rel[3] = {p.x - fromOrigin.x, p.y - fromOrigin.y, p.z - fromOrigin.z};
    return TranslateAlongBasis(toOrigin, toAxis, Dot(fromAxis.Row(0), rel),
                               Dot(fromAxis.Row(1), rel), Dot(fromAxis.Row(2), rel));
}

}  // namespace D2HT::voidengine
