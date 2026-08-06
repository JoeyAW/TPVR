#pragma once

// vr_smooth_turn.hpp
//
// VR "smooth turn" comfort locomotion: the right thumbstick's horizontal
// axis smoothly rotates a persistent yaw offset, and every VR-tracked pose
// (camera eyes, tracked hands, the HUD billboard's world-forward reference)
// gets rotated by that same offset before use -- equivalent to rotating the
// whole physical play space under the player, since the HMD's own tracked
// orientation can't be faked directly. Added 2026-08-05 per explicit user
// request ("add smooth camera rotation to the right stick, unbind the
// C-stick"), replacing the right stick's previous raw C-stick/substick PAD
// binding (see CLAUDE.md section 13's mapping table and section 15).
//
// Deliberately a single shared header, not duplicated per call site: see
// CLAUDE.md section 14's lesson -- eyePoseToViewMtx and buildHandMtx used to
// each carry their OWN copy of a "matches the other one's convention"
// position formula, which silently drifted out of sync (one got fixed,
// the other didn't) and caused a real bug the same day this file was
// written. All three call sites that need yaw rotation (eyePoseToViewMtx/
// updateHudSmoothing in vr_stereo_render.hpp, buildHandMtx in
// vr_link_visibility.hpp) include this header and call the same two
// functions below, so there is exactly one implementation to keep correct.
//
// Math verified in a standalone script before use (not just derived on
// paper -- see CLAUDE.md section 15): rotating a local vector by
// rotateYawQuat(q, yaw) and then by R(q) gives the identical result (to
// float precision) as rotating R(q)*v directly by rotateYawXr(_, yaw), for
// 2000 random (q, yaw, v) trials -- confirms position and orientation
// rotate together consistently, the same property that mattered for the
// stereo-eyes-swap bug this same session fixed elsewhere. Also confirmed:
// rotateYawQuat always returns a unit quaternion, and yaw=0 is an exact
// no-op for both functions (safe default for any call site that isn't
// using smooth-turn).

#include <openxr/openxr.h>
#include <cmath>

namespace dusk::vr {

// Persistent yaw offset, radians, OpenXR/tracking-space convention
// (rotation around the vertical +Y axis, right-handed). Updated once per
// frame (not per eye/hand) by updateSmoothTurn(), called from
// vr_main.cpp's tick() right after reading the right thumbstick. Persists
// for the whole VR session (no auto-recenter/reset), matching how
// snap/smooth-turn works in essentially every other VR game.
inline float g_smoothTurnYawRad = 0.f;

// Turn rate at full stick deflection, and a deadzone to avoid drift from
// controller noise while the stick is resting near center.
inline constexpr float kSmoothTurnDegPerSec = 90.f;
inline constexpr float kSmoothTurnStickDeadzone = 0.15f;

// Advances g_smoothTurnYawRad from the right stick's raw X axis (-1..1)
// and this frame's real elapsed time (pacing.presentation_dt_seconds).
// Sign: NEGATED here so pushing the stick right (positive rightStickX)
// turns the view right -- derived from rotateYawQuat's own convention
// (verified in script: a positive yaw rotates a forward-facing camera's
// view toward -X, i.e. turns it LEFT), not guessed, so this shouldn't
// need an in-headset sign-flip pass the way some of this project's other
// direction constants have.
inline void updateSmoothTurn(float rightStickX, float dtSeconds) {
    if (std::abs(rightStickX) < kSmoothTurnStickDeadzone) return;
    constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
    g_smoothTurnYawRad -= kSmoothTurnDegPerSec * kDegToRad * rightStickX * dtSeconds;
}

// Rotates an OpenXR-tracking-space vector around the vertical (+Y) axis by
// yawRad. Explicit parameter rather than reading the global above directly
// -- keeps this pure/testable and matches eyePoseToViewMtx's existing style
// of taking `scale` as an explicit parameter instead of a hidden global.
inline XrVector3f rotateYawXr(const XrVector3f& v, float yawRad) {
    const float s = std::sin(yawRad);
    const float c = std::cos(yawRad);
    return XrVector3f{ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

// Composes a yaw rotation onto an orientation quaternion in world/tracking
// space: result = RotateY(yawRad) (Hamilton product) q -- rotates the
// resulting world-facing direction by yawRad, the SAME physical rotation
// rotateYawXr above applies to a position, so a rigidly-tracked point
// (e.g. a hand's offset from the head) stays self-consistent under the
// applied yaw. RotateY(yawRad) = (0, sin(yawRad/2), 0, cos(yawRad/2)).
inline XrQuaternionf rotateYawQuat(const XrQuaternionf& q, float yawRad) {
    const float hs = std::sin(yawRad * 0.5f);
    const float hc = std::cos(yawRad * 0.5f);
    return XrQuaternionf{
        hc * q.x + hs * q.z,
        hc * q.y + hs * q.w,
        hc * q.z - hs * q.x,
        hc * q.w - hs * q.y,
    };
}

}  // namespace dusk::vr
