// src/dusk/vr/vr_link_visibility.hpp
//
// Per-frame VR visibility management for Link's model parts, plus
// controller-to-game-world hand matrix mapping.
//
// hide()/show() live on J3DModelData::getShapeTable() (a J3DShapeTable),
// not on J3DModel directly. Calling J3DShapeTable::hide()/show() toggles
// J3DShpFlag_Visible on every shape in the table at once, which is exactly
// what we want for the face/hat models (hide the whole thing).

#pragma once

#include <openxr/openxr.h>

#include "dusk/vr/vr_smooth_turn.hpp"  // dusk::vr::rotateYawXr/rotateYawQuat
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dusk/frame_interpolation.h"
#include "f_op/f_op_view.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphAnimator/J3DShapeTable.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace vr_link {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Game units per metre of physical controller movement.
// TP's Link is ~170 game units tall (~1.7 m real), giving ~100 u/m.
// CONFIRMED this session (temporarily bumped to 2000 as a diagnostic,
// reverted back to the correct 1:1 value here): translation DOES fully
// respond to real controller movement -- ruled out any downstream
// discard/dilution (e.g. smooth-skin blending with the world_root joint).
// The original "position not mapped to my controller" report was
// investigated at this exact 100 value; not yet re-confirmed as actually
// feeling correct at normal scale -- see the call site notes for the next
// test to run before considering this fully resolved.
inline constexpr float VR_SCALE_FACTOR = 100.0f;

// Wrist joint indices in mpLinkHandModel -- confirmed via a one-time
// joint-name dump (OutputDebugStringA, since J3DModelData exposes joint
// names via getJointName() the same way it exposes material names --
// removed after use): mpLinkHandModel has exactly 3 joints, 0 = world_root
// (the shared parent both hands hang off of -- NOT either hand), 1 =
// al_handsL, 2 = al_handsR. HAND_ROOT_JOINT used to be defined as 0 and
// applied to the RIGHT hand -- both wrong: index 0 is the shared root, not
// a per-hand joint, and writing a single hand's pose into it would drag
// BOTH hands rigidly together instead of moving one hand independently.
inline constexpr int LEFT_HAND_JOINT = 1;   // al_handsL
inline constexpr int RIGHT_HAND_JOINT = 2;  // al_handsR

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Hide all shapes in a J3DModel's shape table.
// hide()/show() are on J3DShapeTable, reached via
//   model->getModelData()->getShapeTable()->hide()
inline void hideModel(J3DModel* model) {
    if (!model) return;
    model->getModelData()->getShapeTable()->hide();
}

inline void showModel(J3DModel* model) {
    if (!model) return;
    model->getModelData()->getShapeTable()->show();
}

// Build a Nintendo Mtx (3x4, row-major) in game-world space from a
// controller pose in OpenXR tracking space.
//
//   hmdPosXR        : HMD position in tracking space (metres)
//   controllerPoseXR: controller grip pose in tracking space
//   linkEyeGame     : view->lookat.eye (game units)
//   scale           : metres → game units
//   isLeftHand      : true for the left controller/al_handsL -- see
//                     right_hand_cal's comment for why only the right
//                     hand's rotation is corrected here
struct Vec3f { float x, y, z; };

// FIXED 2026-08-02: this previously implemented R(q)^T (the INVERSE
// rotation, R(q)^-1 = R(conjugate(q))) instead of the forward rotation
// R(q) -- a real, longstanding sign bug, not a calibration issue. Found
// after the user reported every rotation axis (roll, yaw, AND pitch)
// backwards simultaneously while translation stayed correct -- translation
// is computed via a completely separate code path (plain vector addition,
// no quaternion involved), so this uniform, all-axes-at-once inversion
// pointed straight at this function rather than the calibration matrices.
// Verified numerically against a reference implementation: the old
// off-diagonal signs (xy+wz / xz-wy / xy-wz / yz+wx / xz+wy / yz-wx)
// exactly matched R(q)^T, not R(q). This matches the convention this
// project's own Python calibration scripts use (quat_to_R, see CLAUDE.md
// section 12's Session C writeup) -- so the M matrix in
// applyStaticCorrection and the motion-derived kLocalRight/kLocalUp/
// kLocalForward constants are still valid (they only ever depended on
// LOCAL-frame relationships, never on this function's convention) --
// but every empirically-tuned constant that was hand-tuned by looking at
// the RESULT of calling this (buggy) function -- kMeshRollCorrectionDeg
// and kWristRollCorrectionDeg -- was implicitly tuned to compensate for
// R(q)^T at one specific neutral pose, and will very likely need
// re-tuning from scratch now that this returns the correct R(q): R(q) and
// R(q)^T generally disagree at any non-identity q, which the controller's
// real neutral grip orientation is.
inline Vec3f rotateVecByQuat(float qx, float qy, float qz, float qw, const Vec3f& v) {
    const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    const float wx = qw*qx, wy = qw*qy, wz = qw*qz;
    return Vec3f{
        (1.f - 2.f*(yy+zz))*v.x + 2.f*(xy-wz)*v.y        + 2.f*(xz+wy)*v.z,
        2.f*(xy+wz)*v.x        + (1.f - 2.f*(xx+zz))*v.y + 2.f*(yz-wx)*v.z,
        2.f*(xz-wy)*v.x        + 2.f*(yz+wx)*v.y          + (1.f - 2.f*(xx+yy))*v.z,
    };
}

// RIGHT-hand rotation calibration -- derived from real isolated-axis data
// this session (2026-08-01), NOT hand-composed corrective quaternions like
// the earlier, reverted attempts (see git history and CLAUDE.md section 12
// for the full account of what was tried and failed before this).
//
// Methodology: two SEPARATE, slow, isolated single-axis motions were
// captured via [dusk::vr::handrot] logging -- a roll (twisting the
// controller like a doorknob) and a yaw (turning it like shaking your
// head "no"). For each, the WORLD-frame rotation axis between a
// before/after sample pair was computed (q_after * conj(q_before) ->
// axis-angle) via a verification script, not by hand, then un-rotated by
// that test's own "before" orientation to get the axis in the
// CONTROLLER's own local/body frame. That's what kLocalForward (from the
// roll motion) and kLocalUp (from the yaw motion) are: the controller's
// own forward and up axes, in its intrinsic local coordinate system,
// independent of which way it happened to be held during either test.
//
// The previous (reverted) attempt's fatal flaw: it derived "local up" from
// an ASSUMED static target (world (0,1,0) at whatever arbitrary
// orientation a reference pose happened to be in) rather than from real
// motion -- cross-validating that version against held-out data was off
// by 100+ degrees. This version's kLocalForward/kLocalUp are BOTH
// motion-derived, and cross-validate against each other (forward vs. the
// independently-measured yaw rotation axis, which must be perpendicular
// to forward for any single rigid body) to within ~10 degrees of the
// expected 90 -- consistent with ordinary hand-motion imprecision, not a
// methodology error. Gram-Schmidt orthogonalized in the derivation script
// (splitting that ~10 degrees onto "up") to get an exact orthonormal
// frame, since a rotation matrix can't represent non-perpendicular basis
// vectors at all.
//
// Scope: RIGHT hand only -- this data all came from the right controller.
// Left hand is presumed to need its own separate calibration (the earlier
// "left hand upside down, right hand fine" report is real evidence the
// meshes are mirrored, not identical) and is NOT touched here; it still
// uses the raw/unflipped quaternion below.
namespace right_hand_cal {
inline constexpr Vec3f kLocalUp{+0.032410f, +0.774430f, -0.631829f};
inline constexpr Vec3f kLocalForward{+0.210963f, +0.612618f, +0.761704f};
inline constexpr Vec3f kLocalRight{+0.976957f, -0.157979f, -0.143520f};
}  // namespace right_hand_cal

// LEFT hand calibration, started 2026-08-02 (right hand's rotation now
// fully confirmed fixed -- see applyStaticCorrection's history). Unlike
// the right hand's kLocalRight/Up/Forward (derived from real isolated-axis
// motion capture, see right_hand_cal's comment above), this starts from a
// plain IDENTITY local basis rather than repeating that motion-capture
// step. Reasoning: the right hand's aim-pose-based calibration
// (applyStaticCorrection's derivation) is a Kabsch/direct-solve fit that
// maps WHATEVER starting local basis is used to the correct target
// orientation -- the choice of starting basis doesn't matter
// mathematically as long as it's used consistently between deriving the
// correction and applying it, since the correction fully re-maps it
// either way. Using identity avoids re-doing the isolated-axis capture
// (roll/yaw/pitch) that took real effort to get right for the right hand,
// and lets the left hand go straight to the aim-pose calibration + (if
// needed) the same three-rotation-plane live-test approach that ultimately
// nailed the right hand's static offset.
namespace left_hand_cal {
inline constexpr Vec3f kLocalRight{1.f, 0.f, 0.f};
inline constexpr Vec3f kLocalUp{0.f, 1.f, 0.f};
inline constexpr Vec3f kLocalForward{0.f, 0.f, 1.f};
}  // namespace left_hand_cal

// CONFIRMED CORRECT IN-HEADSET 2026-08-03. Dynamic mapping needed no
// per-axis remapping at all -- plain identity local axes
// (left_hand_cal::kLocalRight/Up/Forward = X/Y/Z) plus the already-fixed
// rotateVecByQuat() tracked correctly from the start. Only a static resting
// offset was needed, arrived at via live in-headset iteration (full
// back-and-forth trail in git history, not reproduced here): a Z-axis
// rotation of -90 degrees (theta=270, holding forward/Z fixed), then a
// Y-axis rotation of +110 degrees (holding up/Y fixed), composed in that
// order on the identity basis. The X-axis step tried during iteration
// converged back to identity (0 degrees) -- not needed in the final
// result, unlike the right hand's calibration.
inline Vec3f applyLeftStaticCorrection(const Vec3f& v) {
    const Vec3f afterZ{v.y, -v.x, v.z};

    const Vec3f afterX{
        afterZ.x,
        afterZ.y,
        afterZ.z,
    };

    const float yRad = 110.f * 3.14159265358979323846f / 180.f;
    const float yc = std::cos(yRad);
    const float ys = std::sin(yRad);
    return Vec3f{
        yc*afterX.x + ys*afterX.z,
        afterX.y,
        -ys*afterX.x + yc*afterX.z,
    };
}

// SESSION C, 2026-08-02 (rotation calibration, take 3): the previous
// version of this function (git history) applied its correction matrix to
// WORLD-space vectors AFTER rotateVecByQuat -- proven this session to be
// mathematically incapable of being correct at any orientation other than
// the single reference pose it was derived from (a rotation matrix applied
// post-rotation does not commute with the runtime's actual quaternion q,
// so "correct at the calibration pose" does not imply "correct in
// general"). This is the likely reason every previous "verified correct"
// correction still drifted wrong in general headset movement.
//
// This version fixes that shape: the correction is applied to
// kLocalRight/kLocalUp/kLocalForward THEMSELVES, in the controller's own
// local frame, BEFORE rotateVecByQuat -- see buildHandMtx() below. Since
// this only ever touches the fixed local axes and never anything that
// depends on q, it is mathematically capable of being correct for every
// orientation at once, not just one.
//
// It also replaces the previous version's camera-forward proxy (an
// unverified assumption that the camera was looking where the controller
// pointed during calibration -- flagged as the leading suspect for the
// old correction's failure) with OpenXR's own "aim pose" action, a
// runtime-computed reference for "the direction this controller is really
// pointing" that needs no assumption about where the player was looking.
// A ~160-sample capture (right hand, waved through a variety of
// orientations, see CLAUDE.md section 12's "Session C" writeup) showed
// R_grip^-1 * R_aim is a genuinely constant matrix EXPRESSED IN THE
// CONTROLLER'S LOCAL FRAME (deviation ~0.001 degrees across all samples,
// a fixed ~60 degree rotation) -- unlike an earlier, discarded capture on
// this same runtime where the analogous quantity was constant in WORLD
// space instead (physically impossible for a real aim pose; ruled out as
// unusable, see CLAUDE.md). This capture's local-frame constancy is
// exactly what a genuine fixed body-frame offset between grip and aim
// should look like, so it was used as ground truth here.
//
// Derivation (script, not by hand): averaged R_grip^-1 * R_aim across all
// samples, re-orthonormalized via SVD to denoise. Aim pose's own
// right/up/forward axes in the grip's local frame are then
// Rrel @ (1,0,0) / Rrel @ (0,1,0) / Rrel @ (0,0,+1) respectively -- note
// +Z, not OpenXR's spec "-Z is the aim direction": this codebase's
// existing kLocalRight/kLocalUp/kLocalForward satisfy
// cross(right,up)=+forward (verified directly), the opposite handedness
// convention from OpenXR's own right=+X/up=+Y/forward=-Z, so +Z is the
// self-consistent choice here -- using -Z produces a matrix with
// det=-1 (a mirror), a clear tell of the handedness mismatch. Solved
// M = target_frame * current_frame^T (current_frame's columns are
// orthonormal, so its inverse is its transpose), then SVD-cleaned to
// guarantee a proper rotation. Verified before writing this: det(M) =
// +1.0 exactly, and M @ each of kLocalRight/kLocalUp/kLocalForward
// reproduces its corresponding aim-pose target axis to within numerical
// noise (0.0000 degrees).
//
// NOT yet confirmed in-headset -- next step for whoever picks this up:
// rebuild, test normal gameplay movement (not just the calibration pose),
// and specifically check whether it now holds up across general head/hand
// motion rather than drifting wrong away from one reference orientation,
// which is the failure mode this new local-space shape is meant to fix.
// UPDATE 2026-08-02 (post-rotateVecByQuat-fix retuning): fixing
// rotateVecByQuat's inverse-rotation bug (see that function's comment)
// invalidated the two empirical corrections that used to live here
// (kMeshRollCorrectionDeg, kWristRollCorrectionDeg -- git history has the
// full trial-and-error account, including the hard-won discovery that
// dest column 0 ("right") is the mesh's true facing/heading axis, not
// column 2 as the original "right/up/forward" naming assumed).
//
// UPDATE, same day: the first attempt at solving this directly (from a
// verbal description alone) was still wrong. Redone from two actual
// screenshots instead, which gave real observed directions rather than an
// assumption for one of the three axes:
//   - Photo 1 (neutral grip, CURRENT/wrong result of the previous matrix):
//     thumb toward the viewer, palm down.
//   - Photo 2 (controller rotated by the user to some other pose): thumb
//     up, palm left -- this happens to be the DESIRED look, just reached
//     by rotating away from neutral rather than by the calibration's
//     resting orientation. Confirms the dynamic mapping is still fine;
//     only the neutral-pose offset is wrong.
// Method (script, not by hand): computed the actual current local vectors
// (this matrix's previous coefficients applied to kLocalRight/Up/
// Forward), used photo 1's two DIRECTLY OBSERVED axes (palm, thumb) plus
// the cross-product relationship (facing = palm x thumb, since
// cross(right,up)=forward here means cross(up,fwd)=right cyclically) to
// derive the third (facing) rather than assuming it, then solved for Rq0
// (the actual fixed neutral-pose rotation) from that full correspondence.
// Used the same Rq0 to back out the local vectors needed for photo 2's
// directions (also fully derived, same cross-product check). Folded
// directly into this matrix, replacing the aim-pose-only M entirely (this
// IS the composed result, not a separate step) -- verified: proper
// rotation (det=+1), reproduces all three target vectors to 0.0000
// degrees.
//
// UPDATE, same day: STILL wrong in-headset -- the photo-1/photo-2
// derived matrix above produced a THIRD distinct wrong result (pointing
// straight down, palm toward viewer), confirmed via a fresh screenshot
// (photo 3). Re-solved the exact same way, this time using photo 3 as the
// new "current" observation (facing=down, palm=toward viewer, thumb=
// derived via cross product) against the SAME target as before (facing=
// forward, palm=left, thumb=up, from photo 2 -- still the goal, unchanged).
// Verified: proper rotation (det=+1), reproduces the target to 0.0000
// degrees, same as every previous attempt's verification -- worth noting
// explicitly, since this is now the SECOND time a matrix that verified
// perfectly in Python still came out wrong in-headset. This does not mean
// the math is wrong; it means at least one of the world-direction
// OBSERVATIONS feeding the math (facing/palm/thumb as read off a
// screenshot) has been mismeasured or mislabeled somewhere -- e.g. the
// assumed correspondence between dest columns (right/up/forward) and the
// mesh's actual (facing/palm/thumb) anatomy, or the world-axis convention
// used to encode each screenshot as vectors, could be subtly off in a way
// that isn't caught by internal consistency checks (the math is
// self-consistent by construction regardless of whether the INPUT
// observations are correct). NOT yet confirmed in-headset -- if this is
// ALSO wrong, the next step should be re-examining that mapping assumption
// directly rather than deriving a fourth matrix the same way.
//
// UPDATE, same day: STILL wrong -- photo 3's matrix produced a FOURTH
// distinct result (photo 4: facing right, palm down), confirmed via
// another screenshot. Re-solved the same way again (photo 4 as current,
// same photo-2 target as always). Notable pattern: this new matrix is
// almost exactly the photo-1/photo-2-derived matrix from two updates ago,
// with its "up" and "forward" rows both negated (i.e. a clean 180-degree
// flip in that (up,forward) block specifically, facing row unchanged) --
// consecutive derivations are oscillating by 180 degrees in the same
// block rather than converging closer to correct. This is suspicious: it
// could mean the user isn't holding the exact same real controller
// orientation between test photos (each screenshot's implied Rq0 assumes
// the SAME fixed neutral pose as every other screenshot, which may not
// hold in practice), or that the up=palm/forward=thumb anatomical mapping
// assumption (never independently verified, only inferred from a
// handedness self-consistency check early on) is swapped.
//
// ROOT CAUSE FOUND, same day: user confirmed they held the controller the
// SAME way every test ("buttons straight up") -- but that constrains only
// ROLL (rotation about the controller's own pointing axis), not which way
// the arm was actually aimed (pitch/yaw). So each of the 4 test photos was
// at a genuinely DIFFERENT real aim direction, which is exactly why
// "facing" cycled through left/down/right/up across tests -- comparing
// world-ABSOLUTE directions across photos taken at different aim
// directions was the actual bug, not the linear algebra (every individual
// derivation's 0.0000-degree self-verification was real; the WRONG THING
// was being verified). Confirmed by computing, for each of the 4 photos,
// the roll angle between its OWN observed (facing,palm,thumb) frame and a
// facing-aligned version of the target frame (i.e. "how far is this
// photo's roll from correct, RELATIVE TO its own facing direction" rather
// than vs. a fixed world direction) -- all four gave EXACTLY -90.00
// degrees, independent of which way each photo's facing pointed. This is
// the real, aim-direction-invariant signal: a pure -90 degree roll error
// around whatever the current facing/pointing axis is, not a full 3D
// misorientation at all. (Second bug found while verifying the fix: the
// roll-correction rotation formula's sign convention -- newUp = c*up -
// s*fwd, newFwd = s*up + c*fwd, same convention as the old, now-removed
// applyMeshRollCorrection/applyWristRollCorrection functions -- represents
// the OPPOSITE angle sign from the textbook axis-angle rotation formula;
// initially plugging in the naive +90 fix for the measured -90 error gave
// nonsense until this was caught by checking a full angle sweep instead of
// assuming the sign.) Verified via the full sweep: theta=-90 (in this
// codebase's own rotation-formula convention) gives EXACTLY 0.0 degrees
// residual roll error simultaneously for all 4 independent real test
// photos -- not just one -- which is a much stronger confirmation than
// any single derivation's internal 0.0000-degree self-check has been so
// far this session, since it holds across genuinely different real aim
// directions instead of just being self-consistent by construction.
// Composed by applying this -90 degree roll (in the (up,forward) plane,
// holding right/facing fixed, matching applyMeshRollCorrection/
// applyWristRollCorrection's original intent) directly on top of the
// photo-4-derived matrix, then folded down into this single matrix.
// NOT yet confirmed in-headset -- the theory is now well-validated by
// multiple independent real data points agreeing exactly, which the
// previous four attempts never had, but this is still a NEW matrix that
// hasn't itself been tested yet.
//
// CONFIRMED, same day: user tested the above matrix across FIVE poses --
// neutral, roll left, roll right, pitch up (all consistent with each
// other, confirming the dynamic mapping and the roll fix both hold up
// across real orientation changes, not just one pose) -- and pitch DOWN
// 90 degrees from neutral, which looked CORRECT. This means the dynamic
// mapping and roll calibration are validated, but there's a residual
// static PITCH offset -- neutral should look like what "pitch down 90"
// currently shows. Fixed by applying one more -90 degree rotation in the
// SAME (up, forward) plane (holding right/facing fixed) on top of the
// above matrix, per the user's direct instruction ("pitch it down 90
// degrees"), folded down into this single matrix the same way as every
// previous update in this section.
//
// UPDATE, same day: WRONG PLANE -- user reported the above rotated the
// hand 90 degrees around world Y (vertical) instead of world X (lateral).
// This makes sense: the (up,forward) plane rotates around 'right'
// (facing) -- so at this test's specific neutral pose, 'right' apparently
// points close to world Y (vertical) currently, not a lateral direction,
// meaning a rotation around it reads as a Y-axis/vertical spin rather
// than the intended pitch-like adjustment. Reverted to the previous
// (dynamic-mapping-validated) matrix and instead rotated -90 degrees in
// the (right, forward) plane, holding 'up' fixed -- the other available
// local rotation plane, per the local axis mapping established back in
// section 12 (kLocalRight=pitch axis, kLocalUp=yaw axis,
// kLocalForward=roll axis; this plane rotates around 'up', i.e. the yaw
// axis).
//
// UPDATE, same day: ALSO wrong -- user asked to go back to the Y-axis
// rotation instead (i.e. revert to the (up,forward)/'right'-axis version
// from two updates ago). Worth flagging explicitly: this back-and-forth
// (Y wrong -> try X -> X also wrong -> back to Y) is consistent with the
// same "world axis identity depends on which way you're currently aiming"
// ambiguity CLAUDE.md's photo1-5 saga already ran into (facing direction
// cycled through 4 different world directions across tests that were
// supposed to be "the same neutral pose", because holding a fixed ROLL
// doesn't fix aim direction) -- comparing "which world axis did this spin
// around" between separate test sessions may not be reliable the same way
// the earlier photo-based approach wasn't. Reverted to the (up,forward)
// plane / 'right'-axis rotation (matching CLAUDE.md's "pitch it down 90
// degrees" matrix).
//
// UPDATE, same day: user asked for Z axis instead. This is the third and
// last of the three available single-axis rotation planes (the other two,
// (up,forward)/'right' and (right,forward)/'up', were both already tried
// and reported wrong) -- (right,up) plane, holding 'forward' fixed.
// Applied -90 degrees here (same sign convention as every other attempt
// this session) on top of the dynamic-mapping-validated matrix. NOT yet
// confirmed in-headset. If this is ALSO wrong, all three single-axis
// planes at -90 degrees will have been tried without success -- worth
// stepping back to the more rigorous aim-invariant methodology (multiple
// real test photos at different orientations, solving directly like the
// roll fix earlier in this section) rather than continuing to guess
// individual planes/signs one at a time.
//
// UPDATE, same day: user confirmed this is the correct plane/axis (Z),
// just the wrong direction -- flipped from -90 to +90 (180 degrees from
// the previous value), same (right,up) plane, holding 'forward' fixed.
// NOT yet confirmed in-headset.
inline Vec3f applyStaticCorrection(const Vec3f& v) {
    return Vec3f{
        +0.210964f*v.x + +0.612618f*v.y + +0.761705f*v.z,
        -0.829864f*v.x + +0.524030f*v.y + -0.191621f*v.z,
        -0.516547f*v.x + -0.591686f*v.y + +0.618941f*v.z,
    };
}

inline void buildHandMtx(
    Mtx               dest,
    const XrVector3f& hmdPosXR,
    const XrPosef&    controllerPoseXR,
    const cXyz&       linkEyeGame,
    float             scale,
    bool              isLeftHand,
    float             yawRad = 0.f)
{
    // Offset of controller from HMD in tracking space (metres)
    float dx = controllerPoseXR.position.x - hmdPosXR.x;
    float dy = controllerPoseXR.position.y - hmdPosXR.y;
    float dz = controllerPoseXR.position.z - hmdPosXR.z;

    // VR smooth-turn (vr_smooth_turn.hpp): rotate the hand's offset from
    // the head, and its orientation, by the same persistent yaw offset
    // eyePoseToViewMtx applies to the camera -- keeps tracked hands
    // visually consistent with the smooth-turned view instead of staying
    // pinned to their pre-turn position/orientation relative to it.
    const XrVector3f rotatedOffset = dusk::vr::rotateYawXr(XrVector3f{dx, dy, dz}, yawRad);
    dx = rotatedOffset.x; dy = rotatedOffset.y; dz = rotatedOffset.z;

    // CONFIRMED this session: front/back was specifically swapped (hand
    // appeared behind the player instead of in front) -- fixed by removing
    // the earlier "flip Z" (was `linkEyeGame.z - dz * scale`); position
    // tracking is validated working since.
    const float gx = linkEyeGame.x + dx * scale;
    const float gy = linkEyeGame.y + dy * scale;
    const float gz = linkEyeGame.z + dz * scale;

    const XrQuaternionf q = dusk::vr::rotateYawQuat(controllerPoseXR.orientation, yawRad);

    if (!isLeftHand) {
        // Calibration-derived rotation, mapped straightforwardly (right=
        // kLocalRight, up=kLocalUp, forward=kLocalForward -- confirmed
        // correct against all three real motion captures, see
        // applyStaticCorrection's comment above). The static correction is
        // applied to the LOCAL axes BEFORE rotateVecByQuat now, not to the
        // world-space result after -- see applyStaticCorrection's comment
        // for why the previous after-rotation shape could never be
        // globally correct.
        Vec3f localRight = applyStaticCorrection(right_hand_cal::kLocalRight);
        Vec3f localUp = applyStaticCorrection(right_hand_cal::kLocalUp);
        Vec3f localFwd = applyStaticCorrection(right_hand_cal::kLocalForward);

        Vec3f right = rotateVecByQuat(q.x, q.y, q.z, q.w, localRight);
        Vec3f up = rotateVecByQuat(q.x, q.y, q.z, q.w, localUp);
        Vec3f fwd = rotateVecByQuat(q.x, q.y, q.z, q.w, localFwd);

        dest[0][0] = right.x; dest[0][1] = up.x; dest[0][2] = fwd.x; dest[0][3] = gx;
        dest[1][0] = right.y; dest[1][1] = up.y; dest[1][2] = fwd.y; dest[1][3] = gy;
        dest[2][0] = right.z; dest[2][1] = up.z; dest[2][2] = fwd.z; dest[2][3] = gz;
    } else {
        // Left hand: calibration started 2026-08-02 -- see left_hand_cal's
        // comment. Previously this used its own inline raw/unflipped
        // quaternion formula (matching eyePoseToViewMtx's convention) --
        // that formula turns out to be the SAME matrix form rotateVecByQuat
        // had before its inverse-rotation bug was fixed (see that
        // function's comment), meaning left hand's dynamic rotation was
        // very likely backwards too, the same bug class as the right hand,
        // just never calibrated far enough to notice. Switched to the same
        // rotateVecByQuat() + local-correction-before-rotation structure as
        // the right hand, starting from identity local axes and a no-op
        // static correction (applyLeftStaticCorrection) until real
        // calibration data replaces it.
        Vec3f localRight = applyLeftStaticCorrection(left_hand_cal::kLocalRight);
        Vec3f localUp = applyLeftStaticCorrection(left_hand_cal::kLocalUp);
        Vec3f localFwd = applyLeftStaticCorrection(left_hand_cal::kLocalForward);

        Vec3f right = rotateVecByQuat(q.x, q.y, q.z, q.w, localRight);
        Vec3f up = rotateVecByQuat(q.x, q.y, q.z, q.w, localUp);
        Vec3f fwd = rotateVecByQuat(q.x, q.y, q.z, q.w, localFwd);

        dest[0][0] = right.x; dest[0][1] = up.x; dest[0][2] = fwd.x; dest[0][3] = gx;
        dest[1][0] = right.y; dest[1][1] = up.y; dest[1][2] = fwd.y; dest[1][3] = gy;
        dest[2][0] = right.z; dest[2][1] = up.z; dest[2][2] = fwd.z; dest[2][3] = gz;
    }

}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct FrameInput {
    XrPosef hmdPose;
    XrPosef rightControllerPose;
    XrPosef leftControllerPose;
    // NEW (rotation-calibration follow-up): the OpenXR "aim" pose sampled at
    // the same instant as the grip poses above -- see
    // vr_xr_bootstrap.hpp's HandActions::aimPoseAction comment. Not used to
    // drive the actual hand draw pose (buildHandMtx() below still only reads
    // *ControllerPose, the grip poses) -- was a calibration reference for the
    // now-confirmed-correct right-hand rotation fix (see CLAUDE.md section
    // 12); kept populated in case left-hand calibration wants the same
    // approach, though the final right-hand fix ended up derived from real
    // photos instead (see section 12's later updates).
    XrPosef rightAimPose;
    XrPosef leftAimPose;
    // VR smooth-turn yaw offset (vr_smooth_turn.hpp), read once per frame by
    // vr_main.cpp via dusk::vr::getSmoothTurnYawRad() -- see buildHandMtx's
    // yawRad parameter.
    float smoothTurnYawRad = 0.f;
};

// Arm- and ear-related material indices on mpLinkModel (Link's body),
// identified from a one-time material-name dump (removed after use --
// J3DShapeTable itself doesn't expose shape names, but
// J3DModelData::getMaterialName() exposes MATERIAL names, and every
// existing base-game body-part-hide call site
// (d_a_alink.cpp/d_a_alink_wolf.inc) hides by material index via
// getMaterialNodePointer(idx)->getShape()->hide() -- this follows that same
// established pattern rather than inventing a new one). Confirmed via
// OutputDebugStringA dump, in-headset session 2026-07-31:
//   0 al_armL_m   1 al_armR_m   2 al_arm_m   3 al_bag_m     4 al_beltS_m
//   5 al_beltW_m  6 al_boots_m  7 al_ear_m   8 al_earring_m 9 al_eri_m
//   10 al_gauntletL_m  11 al_handLA_m  12 al_handRA_m  13 al_inner_m
//   14 al_lowbody_m    15 al_pants_m   16 al_skirt_m   17 al_upbody_m
//
// al_gauntletL_m and al_earring_m are included alongside the arm/ear
// materials proper because they're worn ON the arm/ear -- leaving them
// visible would show a gauntlet or earring floating disconnected in space
// once the limb/ear underneath it is hidden.
//
// al_handLA_m/al_handRA_m (11/12) are deliberately NOT included. Per the
// VR embodiment plan discussed with the user, Link's existing hand
// geometry (mpLinkHandModel -- a separate J3DModel, see buildHandMtx()
// below) is meant to stay visible and eventually be repurposed as the
// tracked VR hand model (keeps the sword/shield already attached to it).
// These two body-model hand materials are a different, separate pair of
// shapes on mpLinkModel itself, whose exact role (idle/bare-hand fallback
// vs. otherwise-unused geometry) hasn't been confirmed and isn't needed
// for this task -- left untouched.
inline constexpr int kArmEarMaterialIndices[] = {0, 1, 2, 7, 8, 10};

// Hidden every VR frame (not once) because the base game's own per-frame
// outfit-branch logic (daAlink_c's draw-prep, ~d_a_alink_wolf.inc:420-500)
// unconditionally show()s/hide()s a different subset of body-model shapes
// depending on current outfit/state -- a one-shot hide would get silently
// reversed the next time that logic runs for an unrelated reason (e.g.
// switching between casual wear / Zora armor / magic armor).
//
// Skipped entirely in Wolf form (daAlink_c::checkWolf()): mpLinkModel is
// the SAME J3DModel object reused for Wolf Link's body (setBodyPartPos()
// reads a wolf-specific joint (index 4) off this same pointer -- see the
// eye-anchor section below), which means its underlying resource/material
// table gets swapped to wolf's own set when transformed. The human-form
// indices above would silently hide whatever unrelated wolf materials
// happen to occupy those slots instead. Not a practical concern for the
// VR camera itself (section 11's third-person wolf fallback means nobody's
// eye is ever near this geometry in wolf form anyway), but worth guarding
// explicitly rather than relying on that coincidence.
// Ordon Clothes (internally "casual wear", daAlink_c::checkCasualWearFlg(),
// dItemNo_WEAR_CASUAL_e) use a GENUINELY DIFFERENT, smaller material table
// than Hero's Clothes -- not just a texture swap on the same one. Confirmed
// directly in the base game's own code, not assumed: d_a_alink.cpp's Master
// Sword glow-tint branch has a separate `else if (checkCasualWearFlg())`
// case reading different material indices, guarded by `getMaterialNum() >=
// 8` -- vs. the default (Hero's Clothes) branch's `>= 18`. kArmEarMaterialIndices
// above was dumped specifically while wearing Hero's Clothes (see its own
// comment) and is therefore meaningless while wearing Ordon Clothes: indices
// 8/10 are simply out of range (already skipped by the bounds check below)
// and 0/1/2/7 point at whatever unrelated materials happen to occupy those
// slots in the casual-wear table instead. This is the reported "neck and
// arms still visible in Ordon Clothes" bug -- same underlying mechanism as
// the already-documented Wolf-form material-table swap above, just for a
// different outfit nobody had captured real data for yet.
//
// Real capture came back (2026-08-10, [dusk::vr::ordonmats]), 8 materials,
// "bl_" prefix (vs. Hero's Clothes' "al_"):
//   0 bl_beltS_m   1 bl_ear_m     2 bl_earring_m  3 bl_handLA_m
//   4 bl_handRA_m  5 bl_lowbody_m 6 bl_skin_m     7 bl_upbody_m
//
// No separate arm material exists at all here, unlike Hero's Clothes'
// three (al_armL_m/al_armR_m/al_arm_m) -- bl_skin_m is the only candidate
// for exposed arm/neck skin: face is a separate model
// (mpLinkFaceModel, already hidden independently in updateFrame()), hands
// are their own pair (3/4, excluded below for the same reason Hero's
// Clothes' hand materials are -- keep visible for the tracked VR hand
// model), and everything else here is clearly clothing (belt/lowbody/
// upbody) -- bl_skin_m is what's left, covering exactly what Ordon
// Clothes' short sleeves/open collar actually expose. kArmEarMaterialIndicesCasual
// below is 1 (bl_ear_m), 2 (bl_earring_m, worn on the ear, same "don't
// leave it floating" reasoning as Hero's Clothes' al_earring_m), and 6
// (bl_skin_m).
inline constexpr int kArmEarMaterialIndicesCasual[] = {1, 2, 6};

inline void hideArmsAndEars(daAlink_c* link) {
    if (!link || !link->mpLinkModel || link->checkWolf()) return;
    J3DModelData* modelData = link->mpLinkModel->getModelData();
    if (!modelData) return;

    const u16 matNum = modelData->getMaterialNum();
    if (link->checkCasualWearFlg()) {
        for (int idx : kArmEarMaterialIndicesCasual) {
            if (static_cast<u16>(idx) >= matNum) continue;
            J3DShape* shape = modelData->getMaterialNodePointer(idx)->getShape();
            if (shape) shape->hide();
        }
        return;
    }

    for (int idx : kArmEarMaterialIndices) {
        if (static_cast<u16>(idx) >= matNum) continue;
        J3DShape* shape = modelData->getMaterialNodePointer(idx)->getShape();
        if (shape) shape->hide();
    }
}

// Mirror of hideArmsAndEars() above, for the third-person fallback case
// (see updateFrame()'s firstPerson branch below) -- same per-frame
// re-application reasoning (the base game's own outfit-branch logic can
// re-hide/re-show an overlapping subset of these same shapes on any given
// frame for unrelated reasons, so a one-shot show() would get silently
// reversed) and the same checkWolf() guard: mpLinkModel's material table
// is swapped to Wolf Link's own set in wolf form, so these indices don't
// mean "arm/ear" there at all -- touching them would show()/hide() random
// wolf materials by coincidence of index. Safe to call unconditionally
// from the non-first-person branch since it already no-ops correctly
// during wolf form on its own.
inline void showArmsAndEars(daAlink_c* link) {
    if (!link || !link->mpLinkModel || link->checkWolf()) return;
    J3DModelData* modelData = link->mpLinkModel->getModelData();
    if (!modelData) return;

    const u16 matNum = modelData->getMaterialNum();
    if (link->checkCasualWearFlg()) {
        for (int idx : kArmEarMaterialIndicesCasual) {
            if (static_cast<u16>(idx) >= matNum) continue;
            J3DShape* shape = modelData->getMaterialNodePointer(idx)->getShape();
            if (shape) shape->show();
        }
        return;
    }

    for (int idx : kArmEarMaterialIndices) {
        if (static_cast<u16>(idx) >= matNum) continue;
        J3DShape* shape = modelData->getMaterialNodePointer(idx)->getShape();
        if (shape) shape->show();
    }
}

// ROOT-CAUSED this session ("hands still just follow normal animations,
// controllers do nothing" -- confirmed real, changing grip-pose data via
// logHandPosesPeriodically() above, so the XR action wiring itself was
// fine): d_a_alink.cpp's setDrawHand()-adjacent draw-prep code
// unconditionally re-syncs mpLinkHandModel's joints 1/2 from the BODY
// model's own current hand-joint matrices EVERY EYE, right before
// `modelDraw(mpLinkHandModel, ...)` actually draws it --
//   mpLinkHandModel->setAnmMtx(1, mpLinkModel->getAnmMtx(9));   // al_handsL
//   mpLinkHandModel->setAnmMtx(2, mpLinkModel->getAnmMtx(0xE)); // al_handsR
// (comment there: "Always set these, otherwise the hands occasionally zip
// to origin"). Since that runs AFTER this updateFrame() call (which fires
// once per frame, before the per-eye loop even opens), whatever
// setAnmMtx() wrote here was being silently overwritten moments later,
// every single eye, before any of it reached the screen -- looked exactly
// like "controllers do nothing" from the headset.
//
// Fix: don't write the joint matrices directly here anymore. Cache the
// computed matrices instead, and apply them from a NEW call site added
// directly after that body-joint re-sync in d_a_alink.cpp (guarded on
// isRenderingToHeadset(), via dusk::vr::applyTrackedHandMtx() --
// vr_main.hpp/.cpp -- so d_a_alink.cpp doesn't need to include this
// heavier OpenXR-dependent header directly, same reasoning as
// isRenderingToHeadset()/drawHudBillboard()'s existing forwarding
// wrappers). That ordering guarantees our tracked pose is the LAST thing
// written before the draw, every eye, instead of the first.
namespace detail {
inline Mtx s_rightHandMtx;
inline Mtx s_leftHandMtx;
inline bool s_handMtxValid = false;

// Sword/shield RESTING-POSE (not-hand-attached) smoothing state: the
// belt/back-relative pose setItemMatrix() computes is only written once per
// SIM TICK. Marking that matrix "live" (to bypass frame_interp's stale
// once-per-tick interpolation) fixes lag but exposes choppiness if left
// raw/un-smoothed. Same prev/curr-snapshot-and-lerp technique already
// proven for Link's own head anchor (getVrCameraEyeAnchor()) is used
// instead, via refreshRestingPoseSmoothed() below.
inline Mtx s_swordRestingPrevMtx;
inline Mtx s_swordRestingCurrMtx;
inline uint64_t s_swordRestingTick = ~0ull;
inline bool s_swordRestingValid = false;
inline Mtx s_shieldRestingPrevMtx;
inline Mtx s_shieldRestingCurrMtx;
inline uint64_t s_shieldRestingTick = ~0ull;
inline bool s_shieldRestingValid = false;
}  // namespace detail

// True when the player should currently be viewing (and the VR camera
// should render from) Link's own first-person head position; false
// whenever a third-person fallback is appropriate instead. Shared by
// updateFrame() (face/hat/arms/ears visibility, below) and
// getVrCameraEyeAnchor() (the actual camera anchor point, further down
// this file) -- both need to agree, or the camera could render
// first-person while the limbs that view is supposed to hide are still
// showing, or vice versa. (Originally duplicated inline at each call site
// since getVrCameraEyeAnchor() used to be a strictly simpler condition;
// factored out once the TALK-mode carve-out below made keeping two copies
// in sync too easy to get wrong -- see CLAUDE.md section 14/vr-mod-notes'
// standing lesson about duplicated "matches-the-other-call-site"
// formulas silently drifting apart.)
//
// Wolf Link is always third-person (see the block comment on
// getVrCameraEyeAnchor() itself for why -- unchanged by this function).
//
// FIXED 2026-08-07/08 (user request: "I want the game to stay in first
// person while talking to npcs"): third-person fallback used to trigger
// on `checkEventRun()` alone, which cannot distinguish an actual
// cutscene/transition from ordinary NPC dialogue -- it's ONE shared flag
// (dEvt_control_c::mEventStatus, `d_event.cpp`) that every event type
// flips identically: cutscenes (dEvt_type_OTHER_e/COMPULSORY_e), door/
// transition events (dEvt_type_DOOR_e/TREASURE_e -- confirmed via
// doorCheck() in d_event.cpp, which sets `mMode = dEvt_mode_DEMO_e`, the
// SAME mode a scripted cutscene uses), AND plain conversations
// (dEvt_type_TALK_e) all set it. The one thing that DOES distinguish
// plain dialogue is dEvt_control_c's own mode: talkCheck()/talkXyCheck()
// set it to dEvt_mode_TALK_e specifically, while every cutscene/door/
// treasure/compulsory path sets dEvt_mode_DEMO_e or
// dEvt_mode_COMPULSORY_e instead -- so checking getMode() ==
// dEvt_mode_TALK_e is what actually separates "just talking" (stay
// first-person) from "an authored camera/scripted demo has taken over
// Link's body" (still needs the third-person fallback, same reasoning as
// cutscenes below).
//
// The first version of this ALSO required !checkPlayerDemoMode(), on the
// theory that story-important conversations are sometimes staged as full
// demos with dialogue baked in rather than a plain TALK event, so that
// flag should distinguish "just talking" from "a demo with dialogue."
// User-reported regression ("dialogue is still third person") plus a real
// [dusk::vr::fpdiag] capture proved that theory wrong: checkPlayerDemoMode()
// reads TRUE for the ENTIRE DURATION of an ordinary, plain conversation
// (mode=1/TALK the whole time, playerDemoMode=1 the whole time) -- Link
// apparently runs through some local demo-driven "stop and face the NPC"
// state just to hold a normal conversation at all, not only for scripted
// cutscenes-with-dialogue. That guard was therefore excluding essentially
// ALL dialogue, not just the cutscene case it was meant to carve out.
// Removed -- dEvt_control_c's own mode is the only signal actually needed;
// the logged mode=2/DEMO blocks for real cutscenes/other events in the
// same capture confirm nothing gets confused by dropping it.
//
// FIXED 2026-08-08 (user request: "make every cutscene that has link
// loaded in first person"): the DEMO/COMPULSORY branch below used to be a
// blanket third-person fallback for every cutscene/door-transition event,
// on the theory that an authored cutscene camera isn't guaranteed to be
// looking at Link at all. That's still true for shots that don't actually
// feature Link's own body -- many cutscenes swap in a separate stand-in
// demo actor and hide the real Link actor entirely rather than animating
// him directly (or just point the camera elsewhere while he's parked
// off-camera) -- so this doesn't blanket-flip to first-person; it checks
// whether Link's OWN body is actually the thing being drawn this frame via
// checkPlayerNoDraw() (d_a_alink_link.inc -- the same flag
// draw() itself already gates mpLinkModel's draw call on, driven either by
// a camera-attention "hide player" bit or FLG0_PLAYER_NO_DRAW, both of
// which are only ever set from the demo/cutscene code in
// d_a_alink_demo.inc, never during ordinary gameplay). When Link is
// genuinely loaded/visible in the shot, stay first-person same as
// dialogue; when he's been swapped out or explicitly hidden for a shot
// that isn't about him, keep the third-person fallback -- there's no real
// head position worth anchoring the camera to in that case anyway.
inline bool isFirstPerson(daAlink_c* link) {
    if (!link || link->checkWolf()) return false;
    if (!link->checkEventRun()) return true;  // no event at all -- ordinary gameplay

    dEvt_control_c* event = dComIfGp_getEvent();
    if (!dComIfGp_event_runCheck() || !event) return false;
    if (event->getMode() == dEvt_mode_TALK_e) return true;  // plain dialogue

    // FIXED 2026-08-08 (user report: forcing cutscenes first-person put the
    // camera "phasing through Epona's head" during a horseback cutscene).
    // getVrCameraEyeAnchor()'s mount-relative eye offset
    // (horseLocalEyeFromRoot/canoeLocalEyeFromRoot/boardLocalEyeFromRoot,
    // setBodyPartPos() in d_a_alink.cpp) only activates under a specific
    // set of active-gameplay player-status flags
    // (dComIfGp_checkPlayerStatus0/1(...)) -- a scripted cutscene demo
    // doesn't appear to set those the same way normal interactive riding
    // does, so during a mounted cutscene the anchor falls through to
    // field_0x3768 = eyePos, i.e. Link's own bare head-joint position with
    // no mount-relative offset applied at all -- and section 11 already
    // flagged this exact mount-eye-anchor path as "expected to work but
    // never actually confirmed in-headset." Until that's separately
    // root-caused, treat mounted cutscenes as third-person, the same
    // carve-out reasoning as the Wolf-form check at the top of this
    // function (a mount's own rig was never designed to be viewed from
    // inside Link's un-adjusted head position). Does NOT affect ordinary
    // mounted GAMEPLAY (the `!link->checkEventRun()` branch above returns
    // true before this is ever reached) -- only mounted cutscenes.
    if (link->checkReinRide() || link->checkCanoeRide() || link->checkBoardRide()) {
        return false;
    }

    // Cutscene / door-transition event: first-person too, but only while
    // Link's real body is actually loaded/drawn in this shot.
    return !link->checkPlayerNoDraw();
}

// Crawling has no single MODE_FLG bit the way swimming has MODE_SWIMMING
// -- it's a sequence of dedicated daAlink_PROC states instead
// (PROC_CRAWL_START/_MOVE/_AUTO_MOVE/_END, d_a_alink.h) that mProcID walks
// through while crawling through a tunnel/gap (confirmed via their use in
// d_a_alink.cpp, e.g. the PROC_CRAWL_END exclusion at line ~11556). Grouped
// into one shared helper (used by both the camera-anchor fallback and the
// tracked-hand disable below) rather than duplicating the four-way
// comparison at each call site -- same "one shared definition" reasoning
// as isFirstPerson() itself.
inline bool isCrawling(daAlink_c* link) {
    if (!link) return false;
    switch (link->mProcID) {
        case daAlink_c::PROC_CRAWL_START:
        case daAlink_c::PROC_CRAWL_MOVE:
        case daAlink_c::PROC_CRAWL_AUTO_MOVE:
        case daAlink_c::PROC_CRAWL_END:
            return true;
        default:
            return false;
    }
}

// Forward-declared here, defined further down (see its own comment) --
// needed by updateFrame() below so hands anchor to the SAME point the VR
// camera actually renders from.
inline cXyz getVrCameraEyeAnchor(const cXyz& fallbackEye);

// Computes both hands' tracked matrices from a given (hmdPos, controller
// poses, eye anchor, yaw) sample and caches them into detail::s_rightHandMtx/
// s_leftHandMtx -- the actual write applyTrackedHandMtx() (below) later
// reads from. Factored out of updateFrame() (2026-08-08, section 20
// continuation: VR hands lag behind during fast movement) so a SECOND,
// LATER call site -- vr_main.cpp's applyTrackedHandMtx() forward, invoked
// once per eye right before the hand joints are actually drawn -- can
// recompute this from a freshly re-located sample instead of the one
// updateFrame() took near the top of the frame. One implementation, two
// call sites, same "don't let a duplicated formula silently drift out of
// sync" reasoning as vr_smooth_turn.hpp's own header comment.
inline void computeTrackedHandMatrices(const XrVector3f& hmdPos,
                                        const XrPosef& rightControllerPose,
                                        const XrPosef& leftControllerPose,
                                        const cXyz& eyeAnchor,
                                        float yawRad) {
    buildHandMtx(detail::s_rightHandMtx, hmdPos, rightControllerPose, eyeAnchor, VR_SCALE_FACTOR, false, yawRad);
    buildHandMtx(detail::s_leftHandMtx, hmdPos, leftControllerPose, eyeAnchor, VR_SCALE_FACTOR, true, yawRad);
    detail::s_handMtxValid = true;
}

inline void updateFrame(const FrameInput& input) {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;

    // Only hide face/hat/arms/ears while actually viewing through Link's
    // own eyes -- otherwise you'd be looking at his third-person body with
    // holes where his face/hat/limbs should be. See isFirstPerson()'s own
    // comment for exactly what counts (ordinary gameplay AND plain NPC
    // dialogue both count as first-person; cutscenes, door/transition
    // events, and Wolf form don't).
    //
    // FIXED 2026-08-07 (user request: "if not in gameplay [i.e. not first
    // person], show all of Link's limbs"): previously hideModel()'d face
    // and hat completely unconditionally, and hideArmsAndEars() only
    // skipped itself for Wolf form, not cutscenes -- so a cutscene's
    // third-person fallback camera, and (for face/hat specifically) even
    // Wolf form's, still showed Link with his face/hat missing. Run every
    // frame either way (not just on the first-person/third-person
    // transition), matching hideArmsAndEars()'s own existing reasoning: the
    // base game's per-frame outfit-branch logic can re-hide/re-show an
    // overlapping subset of these shapes on any given frame for unrelated
    // reasons, so a one-shot toggle would get silently reversed.
    // Also show everything while the pause/status menu is open (user
    // request 2026-08-10, "see all of link's limbs in the pause menu").
    // d_a_alink_swindow.inc's statusWindowDraw() ("Pause Menu Player
    // Display") draws mpLinkModel/mpLinkHandModel/mpLinkHatModel/
    // mpLinkFaceModel -- the SAME shared model instances this function
    // hides shapes on for first-person viewing. Shape-hide is a property
    // of the shared J3DModelData/J3DModel resource itself, not scoped to
    // any one camera/view, so hiding for gameplay bleeds straight into
    // this third-person character preview too -- isFirstPerson() has no
    // idea the pause menu is even open (checkEventRun()/checkWolf() are
    // both unrelated to pause state; you can pause during ordinary
    // exploration, where checkEventRun() is already false-not-relevant
    // either way). dComIfGp_isPauseFlag() (d_com_inf_game.h) is the real,
    // already-used pause-menu-open flag -- set/cleared directly in
    // d_menu_window.cpp's dMw_c, confirmed by reading it, not guessed.
    // Deliberately only affects THIS visibility decision, not
    // isFirstPerson() itself -- the VR camera-anchor logic
    // (getVrCameraEyeAnchor()) doesn't need to change for this: the live
    // 3D view isn't what's on screen while paused anyway (the pause
    // menu's own backdrop is a frozen capture -- see dDlst_MENU_CAPTURE_c
    // in d_menu_window.cpp), so touching the shared isFirstPerson()
    // would risk an untested side effect on the camera for no benefit.
    const bool firstPerson = isFirstPerson(link) && !dComIfGp_isPauseFlag();
    if (firstPerson) {
        // Hide face and hat — these are separate J3DModel objects so hiding
        // their shape tables has no effect on the body or hand models.
        hideModel(link->mpLinkFaceModel);
        hideModel(link->mpLinkHatModel);
        hideArmsAndEars(link);
    } else {
        showModel(link->mpLinkFaceModel);
        showModel(link->mpLinkHatModel);
        showArmsAndEars(link);
    }

    // Compute (but do not yet apply -- see applyTrackedHandMtx() below)
    // both hands' tracked matrices.
    view_class* view = dComIfGd_getView();
    if (!view) return;

    // ROOT-CAUSED this session ("hand tracked my sweep, but its position
    // wasn't where my controller actually was"): this used to anchor hands
    // to view->lookat.eye directly -- the flatscreen third-person camera's
    // eye, a DIFFERENT point in space from where the VR camera itself is
    // actually anchored (getVrCameraEyeAnchor(), Link's real head position
    // during normal gameplay -- see that function's own comment). Movement
    // was correctly scaled/tracked relative to THAT anchor, but since it
    // wasn't the same point the player's view renders from, every hand
    // position came out systematically offset by the distance between the
    // two anchors -- looked exactly like "tracks my movement, but isn't
    // where my controller is". Using the same anchor as the camera fixes
    // both to agree.
    // NOTE (2026-08-08, section 20 continuation; CORRECTED 2026-08-09): this
    // early-in-tick() sample is what drives face/hat/arm visibility above
    // and is what actually reaches the render. The old version of this
    // comment claimed vr_main.cpp's per-eye applyTrackedHandMtx() call site
    // (inside daAlink_c::draw(), via setDrawHand()) overwrites this again,
    // freshly re-located, right before each eye's real draw -- that was
    // never actually true: a full-session [dusk::vr::eyepasscheck] log
    // capture proved daAlink_c::draw() never runs during a real VR eye pass
    // at all (only from the legacy once-per-sim-tick fapGm_Execute() path),
    // so that call site silently never fired during real rendering. The
    // matrices computed HERE are instead applied via
    // refreshTrackedHandDrawMtxLive() (below), called once per real frame
    // from vr_main.cpp's tick() right after this function returns -- see
    // its own comment for the full root-cause writeup and why a plain
    // setAnmMtx() write alone isn't sufficient either. Position tracking
    // itself has been confirmed working since 2026-08-02 (section 12).
    const cXyz eyePos = getVrCameraEyeAnchor(view->lookat.eye);
    computeTrackedHandMatrices(input.hmdPose.position, input.rightControllerPose,
                                input.leftControllerPose, eyePos, input.smoothTurnYawRad);
}

// Called from d_a_alink.cpp, once per eye, immediately after the base
// game's own body-joint re-sync of mpLinkHandModel's joints 1/2 (see the
// root-cause comment above) -- overwrites them again with this frame's
// tracked controller pose so VR's version is what actually reaches the
// draw. No-op before the first updateFrame() call this session (e.g. the
// very first VR frame) rather than writing uninitialized matrices.
inline void applyTrackedHandMtx(J3DModel* handModel) {
    // TEMP DIAGNOSTIC (tracked-hands still not visibly moving, remove once
    // confirmed fixed): confirms this call site is actually being reached
    // at all (proves isRenderingToHeadset() was true and setDrawHand() ran
    // for this eye), and prints the actual translation this frame's
    // buildHandMtx() computed, once every 90 calls (~45 real frames, since
    // this runs twice per frame -- one per eye). If this never appears in
    // the Output window, the call site itself isn't being reached; if it
    // appears with sane, moving values but hands still look static
    // in-headset, the bug is downstream of here (e.g. calc()/draw() using
    // a different matrix source than setAnmMtx(), or a scale/parenting
    // issue in how joints 1/2 compose under mpLinkHandModel's base
    // transform).
    static int callCounter = 0;
    if ((callCounter++ % 90) == 0) {
        char line[256];
        _snprintf_s(line, _TRUNCATE,
                    "[dusk::vr::applyhand] handModel=%p valid=%d right=(%.1f,%.1f,%.1f) left=(%.1f,%.1f,%.1f)\n",
                    static_cast<void*>(handModel), detail::s_handMtxValid,
                    detail::s_rightHandMtx[0][3], detail::s_rightHandMtx[1][3], detail::s_rightHandMtx[2][3],
                    detail::s_leftHandMtx[0][3], detail::s_leftHandMtx[1][3], detail::s_leftHandMtx[2][3]);
        OutputDebugStringA(line);
    }

    if (!handModel || !detail::s_handMtxValid) return;
    handModel->setAnmMtx(RIGHT_HAND_JOINT, detail::s_rightHandMtx);
    handModel->setAnmMtx(LEFT_HAND_JOINT, detail::s_leftHandMtx);
}

// ACTUAL FIX for section 20's persistent hand lag (2026-08-09). Same
// underlying "compute once per sim tick, REPLAY every real render frame"
// architecture writeup as before (see git history for the superseded
// getDrawMtxPtr()/viewCalc() version of this function and why it had ZERO
// observable effect) -- but the buffer this needs to intervene on was
// wrong. A [dusk::vr::liverefresh] capture showed getDrawMtxPtr() returning
// the SAME static address for every different hand-model instance, frozen
// at (0,0,0) -- that's J3DMtxBuffer::sNoUseDrawMtx, a shared placeholder
// (J3DMtxBuffer.cpp's setNoUseDrawMtx()), not a real per-model buffer at
// all. Traced why directly in J3DShapeMtx.cpp: mpLinkHandModel's shapes use
// the "ConcatView" load type (J3DMtxBuffer::create() explicitly routes
// ConcatView models to setNoUseDrawMtx() instead of allocating a real
// DrawMtx array), and J3DShapeMtxConcatView::load() for THIS load type
// reads the matrix straight from J3DModel::getAnmMtx() (via
// getUserAnmMtx(), which J3DMtxBuffer::createAnmMtx() aliases directly onto
// mpAnmMtx -- the exact same buffer setAnmMtx() writes and getAnmMtx()
// reads) through an sMtxPtrTbl[]/getDrawMtxFlag()/getDrawMtxIndex()
// redirection -- getDrawMtxPtr()/calcDrawMtx() are never consulted for this
// model's shapes at all. So the fix is simpler than the superseded version:
// mark getAnmMtx() itself live -- no separate viewCalc() recompute needed,
// since applyTrackedHandMtx()'s existing setAnmMtx() call already writes
// AND records (via J3DModel::setAnmMtx()'s own record_final_mtx() call,
// J3DModel.cpp) the exact matrix that matters. mark_live_this_frame() just
// needs to tell resolve_replacement() not to substitute a stale
// once-per-tick-interpolated value for it. Called once per real frame from
// vr_main.cpp's tick(), before the per-eye loop opens (both eyes share the
// same matrix, no per-eye duplication needed).
// SWIMMING (user request 2026-08-09): don't force the tracked controller
// pose onto Link's hands while swimming -- skip entirely so the body's own
// once-per-sim-tick swim-stroke hand animation (already synced into these
// same two joints by d_a_alink.cpp's "Always set these" body-joint resync)
// plays through frame_interp's normal once-per-tick interpolation instead,
// exactly as it already does on flatscreen. Checked INSIDE this function
// (rather than skipping the call at the vr_main.cpp call site) so the
// override write and the mark_live_this_frame() call are skipped together
// -- marking the joints live without also overriding their pose would just
// make the swim animation render raw/un-interpolated instead of smoothly
// blended, the opposite of what's wanted here.
//
// CRAWLING (user request 2026-08-09, same fix requested for crawling):
// identical reasoning -- see isCrawling()'s own comment for why this is a
// mProcID check rather than a MODE_FLG bit like swimming.
inline void refreshTrackedHandDrawMtxLive(J3DModel* handModel) {
    if (!handModel || !detail::s_handMtxValid) return;

    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (link && (link->checkModeFlg(daAlink_c::MODE_SWIMMING) || isCrawling(link))) return;

    applyTrackedHandMtx(handModel);

    dusk::frame_interp::mark_live_this_frame(handModel->getAnmMtx(RIGHT_HAND_JOINT));
    dusk::frame_interp::mark_live_this_frame(handModel->getAnmMtx(LEFT_HAND_JOINT));
}

// Sword/shield tracked attachment -- same underlying bug class as
// mpLinkHandModel above ("floats where it'd be on Link's regular model"),
// different mechanism/fix shape.
//
// On flatscreen, mSwordModel/mShieldModel are entirely separate J3DModel
// instances positioned once per frame (daAlink_c::setItemMatrix(), NOT
// per-eye) via
//   mSwordModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mLeftItemJntNo));
//   mShieldModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mRightItemJntNo));
//
// CORRECTED (round 3, same session): mLeftItemJntNo/mRightItemJntNo are
// NOT the same joints as the hand joints setDrawHand() feeds into
// mpLinkHandModel's joints 1/2. Confirmed via d_a_alink_wolf.inc's
// "revert from wolf to human" reset block, which sets ALL FOUR fields at
// once: mLeftHandJntNo=9, mRightHandJntNo=0xE, mLeftItemJntNo=10,
// mRightItemJntNo=0xF -- i.e. in human form the ITEM (sword/shield) joint
// is a DIFFERENT joint (10/0xF) from the HAND joint (9/0xE), presumably a
// child/sibling joint in the rig encoding the fixed grip offset between
// "where the wrist is" and "where a held object sits/is oriented in that
// grip." An earlier version of this fix wrongly assumed they were the
// same joint (they share a NUMBER-LOOKING resemblance -- 9/0xE for hands,
// 10/0xF for items, off by exactly one -- easy to misread quickly) and
// substituted the tracked HAND matrix directly for the item's position,
// which looked fine for the shield (a roughly flat/symmetric shape,
// forgiving of a moderate rotational offset) but visibly wrong for the
// sword (a long blade, where the same offset is obvious at the tip) --
// user-reported as "sheaths and unsheathes [correctly now] but is still
// facing the wrong way."
//
// Fix: don't substitute the tracked hand matrix directly. Instead,
// preserve whatever relative offset the body rig defines between the hand
// joint and the item joint, recomputed fresh every frame from mpLinkModel's
// own CURRENT animated matrices (works regardless of whether that offset
// is a rig constant or itself varies per-animation):
//   relativeOffset = inverse(handJointWorldMtx) * itemJointWorldMtx
//   trackedItemMtx = trackedHandMtx * relativeOffset
// i.e. "re-express the item's real current local relationship to the hand
// joint, but rooted at the TRACKED hand pose instead of the body's
// animated one." This is the same "insight" applyStaticCorrection's
// history (section 12) already contains in a different guise: composing a
// correction onto a LOCAL relationship (here, hand-to-item) rather than
// applying it after the fact to something already in world space is what
// makes it correct in general, not just at one reference pose.
//
// Unlike applyTrackedHandMtx() above, this can't just poke mMtxBuffer
// directly: setAnmMtx() there overwrites an already-resolved joint-world
// matrix inside mpLinkHandModel's OWN joint hierarchy (mMtxBuffer), read
// directly at draw time with no further recalculation needed. Sword/shield
// have no such per-joint override -- setBaseTRMtx() only assigns
// J3DModel::mBaseTransformMtx, a plain member that calcAnmMtx() (called
// from calc()) reads to resolve the model's OWN joint tree the NEXT time
// calc() runs (J3DModel.cpp: calcAnmMtx() -> getJointTree().calc(mMtxBuffer,
// mBaseScale, mBaseTransformMtx)). Since these models were already calc()'d
// once this frame (setItemMatrix(), with the stale flatscreen-joint base
// matrix), changing mBaseTransformMtx alone here would have zero visible
// effect until some later frame's calc() happened to pick it up -- calc()
// must be called again right here, per eye, for the new base matrix to
// actually reach the draw. Sword/shield are small, simple models, so a
// second (or third, across both eyes) calc() per frame is not a
// perf concern (same reasoning as section 7's HUD billboard capture cost).
//
// FIRST VERSION OF THIS FIX (unconditional override) WAS WRONG -- reverted
// same session after in-headset testing. setItemMatrix() does NOT always
// attach sword/shield to the hand joint: it switches per frame between the
// hand-joint matrix above (sword: only when `mEquipItem == 0x103`, i.e.
// sword is Link's currently-EQUIPPED weapon -- `param_0` is always 0 at
// every real call site, so that's the whole condition in practice; shield:
// a wider OR-chain covering actively guarding/attacking/etc.) and a
// completely different, computed BELT/BACK-relative offset matrix (the
// `else` branch in setItemMatrix() -- e.g. sword sheathed while a DIFFERENT
// item like the bow is equipped, or shield stowed on the back) for
// everything else. The unconditional version overwrote BOTH cases with the
// tracked hand position, so a sheathed sword (while some other item was
// equipped) or a stowed shield ended up floating at the player's tracked
// hand instead of properly out of the way at the hip/back -- reported
// in-headset as "I see the shield when it's not equipped" and "I see the
// sword hilt when it's not equipped."
//
// Fix: only substitute the tracked matrix when setItemMatrix() actually
// chose the hand-joint branch THIS frame for THIS model -- detected
// structurally (comparing the model's current base transform against
// mpLinkModel->getAnmMtx(mLeftItemJntNo/mRightItemJntNo), evaluated by the
// caller right before this call, same frame) rather than by duplicating
// setItemMatrix()'s own multi-condition boolean logic here, which would be
// one more place that silently drifts out of sync if that logic ever
// changes (same "don't infer, verify structurally" spirit as this
// project's other VR-vs-flatscreen guards). When the model is currently in
// its belt/back-relative pose, this is a no-op and the base game's own
// computed offset is left completely alone -- correct for VR too, since
// that pose was never meant to track a hand in the first place.
//
// Called from d_a_alink.cpp right after setDrawHand(), unconditionally
// (matches setDrawHand()'s own "always set these" reasoning for hands) --
// guarded on isRenderingToHeadset() at the call site, not here.
//
// Known gap: mHeldItemModel (bow, lantern, boomerang, etc. -- a separate,
// broader "currently equipped item" model, also positioned from
// mLeftItemJntNo/mRightItemJntNo in several places in d_a_alink.cpp) very
// likely has the exact same floating-in-VR symptom, by the same mechanism.
// Not fixed here -- only sword/shield were reported/requested.
inline bool mtxNearlyEqual(MtxP a, MtxP b) {
    if (!a || !b) return false;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(a[r][c] - b[r][c]) > 0.01f) return false;
        }
    }
    return true;
}

// itemJointMtx/handJointMtx: this frame's mpLinkModel->getAnmMtx(mXxxItemJntNo)
// / getAnmMtx(mXxxHandJntNo) for one side, evaluated by the caller.
// itemJointMtx doubles as the gate (compare against the model's current
// base transform -- only proceed if setItemMatrix() actually chose the
// hand-attached branch this frame) and as the numerator of the relative
// offset; handJointMtx is the offset's denominator. trackedHandMtx is
// s_leftHandMtx/s_rightHandMtx. Returns false (caller should leave the
// model untouched) if the model isn't currently hand-attached, or if
// MTXInverse fails (should not happen for a well-formed rigid joint
// matrix, but MTXInverse's own return value is trusted over assuming
// success -- leaving the base game's existing transform in place is a
// safe fallback either way).
// Pure math -- no gating. Computes the tracked-hand-relative matrix for an
// item, given the body rig's current item/hand joint matrices and the
// tracked hand pose. Returns false only if MTXInverse fails (should not
// happen for a well-formed rigid joint matrix, but its return value is
// trusted over assuming success).
inline bool computeTrackedItemMtx(
    Mtx dest, MtxP itemJointMtx, MtxP handJointMtx, MtxP trackedHandMtx)
{
    Mtx handInv;
    if (!MTXInverse(handJointMtx, handInv)) return false;

    Mtx relativeOffset;
    MTXConcat(handInv, itemJointMtx, relativeOffset);
    MTXConcat(trackedHandMtx, relativeOffset, dest);
    return true;
}

// Returns whether `model` was actually updated (setItemMatrix() had chosen
// the hand-attached branch this frame for it) -- exposed as a bool (rather
// than folded straight into applyTrackedItemMtx() below) so
// refreshTrackedItemMtxLive() knows which models actually need their
// matrices marked live for frame_interp, without duplicating the gating
// logic. Gate: compare the model's CURRENT base transform against
// itemJointMtx (mtxNearlyEqual) -- only proceed if setItemMatrix() actually
// chose the hand-attached branch this frame. Unchanged from before this
// session's live-refresh work; still correct for this function's own
// (dead, but harmless) once-per-tick call site -- see
// applyTrackedItemMtxOneLive() below for why the LIVE per-real-frame call
// site needs a different (cached) version of this same gate.
inline bool applyTrackedItemMtxOne(
    J3DModel* model, MtxP itemJointMtx, MtxP handJointMtx, MtxP trackedHandMtx)
{
    if (!model || !detail::s_handMtxValid) return false;
    if (!mtxNearlyEqual(model->getBaseTRMtx(), itemJointMtx)) return false;

    Mtx trackedMtx;
    if (!computeTrackedItemMtx(trackedMtx, itemJointMtx, handJointMtx, trackedHandMtx))
        return false;

    model->setBaseTRMtx(trackedMtx);
    model->calc();
    return true;
}

// LIVE variant for refreshTrackedItemMtxLive() (2026-08-09, multiple rounds
// of history -- see git log for the full trail). FINAL fix: `attached` is
// now the REAL, AUTHORITATIVE game-state flag (daAlink_c::checkItemSwordEquip()/
// checkShieldHandAttached(), computed by the caller and passed in), not a
// position-comparison heuristic. Two different heuristics were tried and
// both failed for real reasons, not implementation bugs: (1) comparing the
// model's own base transform against a freshly-re-read item-joint matrix
// was self-defeating once called every real frame instead of once per tick
// (this function's own overwrite corrupted the next comparison); caching
// the comparison per-tick (a first attempt at fixing that) still failed
// because (2) the underlying item-joint VALUE ITSELF keeps changing between
// when setItemMatrix() captures it (during game-logic execute) and when
// this later, real-frame call site re-reads it (confirmed via a real
// [dusk::vr::itemgate] capture during a confirmed mEquipItem==0x103 window:
// base and item differed by up to ~1.0 in rotation components, changing
// rapidly tick to tick -- a fast swing animation genuinely moves the joint
// between those two read points, not a bug in the comparison). No amount of
// caching fixes a comparison against a value that's legitimately stale by
// construction -- the real flag sidesteps the whole problem.
inline bool applyTrackedItemMtxIfAttached(
    J3DModel* model, bool attached, MtxP itemJointMtx, MtxP handJointMtx, MtxP trackedHandMtx)
{
    if (!model || !attached || !detail::s_handMtxValid) return false;

    Mtx trackedMtx;
    if (!computeTrackedItemMtx(trackedMtx, itemJointMtx, handJointMtx, trackedHandMtx))
        return false;

    model->setBaseTRMtx(trackedMtx);
    model->calc();
    return true;
}

inline void lerpMtxElementwise(Mtx out, MtxP a, MtxP b, float t) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = a[r][c] + (b[r][c] - a[r][c]) * t;
        }
    }
}

// Smooths sword/shield's RESTING-POSE (not-hand-attached) base transform
// across real frames -- see detail::s_swordRestingPrevMtx's comment for the
// full history of why this is needed (frame_interp's own once-per-tick
// interpolation only sees data from the dead per-tick call path, and
// marking the matrix "live" to bypass it -- correct for the tracked-hand
// case -- makes a 30Hz-only-updated value render as raw, choppy steps at
// real framerate instead). On each NEW sim tick (sim_tick_seq() changed),
// shifts curr->prev and snapshots the model's CURRENT base transform
// (whatever setItemMatrix() just set THIS tick, correct/fresh at this exact
// point since nothing has overwritten it yet this tick -- same ordering
// guarantee applyTrackedItemMtxOneLive()'s gate cache relies on) into curr.
// Every real frame, blends prev/curr with the real interpolation fraction
// for THIS frame (dusk::frame_interp::get_interpolation_step()) -- same
// prev/curr-snapshot-and-lerp shape already proven for Link's own head
// anchor (getVrCameraEyeAnchor()), just applied to a different value.
// Per-element matrix lerp (not a proper slerp) matches frame_interp's own
// internal lerp_matrix() -- an established, already-shipping approximation
// in this codebase, reasonable given how little a joint matrix typically
// rotates between two consecutive ~30Hz ticks.
inline void refreshRestingPoseSmoothed(
    J3DModel* model, uint64_t& cachedTick, bool& valid, Mtx prevMtx, Mtx currMtx)
{
    if (!model) return;

    const uint64_t tick = dusk::frame_interp::sim_tick_seq();
    if (cachedTick != tick) {
        cachedTick = tick;
        if (valid) {
            MTXCopy(currMtx, prevMtx);
        } else {
            MTXCopy(model->getBaseTRMtx(), prevMtx);
            valid = true;
        }
        MTXCopy(model->getBaseTRMtx(), currMtx);
    }

    Mtx blended;
    lerpMtxElementwise(blended, prevMtx, currMtx, dusk::frame_interp::get_interpolation_step());
    model->setBaseTRMtx(blended);
    model->calc();
}

inline void applyTrackedItemMtx(
    J3DModel* swordModel, J3DModel* shieldModel,
    MtxP leftItemJointMtx, MtxP leftHandJointMtx,
    MtxP rightItemJointMtx, MtxP rightHandJointMtx)
{
    applyTrackedItemMtxOne(swordModel, leftItemJointMtx, leftHandJointMtx, detail::s_leftHandMtx);
    applyTrackedItemMtxOne(shieldModel, rightItemJointMtx, rightHandJointMtx, detail::s_rightHandMtx);
}

// Marks every joint of `model` live for frame_interp. FOUND AND FIXED
// 2026-08-09 (4th round on sword/shield lag): the first version of this
// function only marked getAnmMtx() -- a [dusk::vr::itemsentinel] capture
// confirmed sword/shield ARE the same ConcatView type as the hand model
// (getDrawMtxPtr() reads the shared sNoUseDrawMtx sentinel, frozen at
// (0,0,0)) and that getAnmMtx(0) WAS updating correctly frame to frame --
// so the buffer being refreshed was right, but the user still reported
// visible lag. Root cause: J3DShapeMtxConcatView::load() (J3DShapeMtx.cpp)
// picks EITHER getUserAnmMtx() (== getAnmMtx(), what markModelJointsLive()
// marked) OR getWeightAnmMtx() (a completely separate weight-envelope
// buffer, for shapes bound via envelope/skin weights rather than a plain
// rigid joint bind) via getDrawMtxFlag(mUseMtxIndex) -- which one a given
// shape actually uses isn't something this code can assume either way
// without checking per-model, and sword/shield's shapes evidently use the
// weight-envelope one. J3DModel::calc() (J3DModel.cpp) already computes AND
// records BOTH getAnmMtx() and getWeightAnmMtx() every call (calcAnmMtx()
// then calcWeightEnvelopeMtx(), each followed by a record_final_mtx() loop)
// -- so the refresh side was already correct, only the live-marking was
// incomplete. Marking every joint/envelope-matrix (not just index 0) is
// deliberately over-inclusive rather than assuming a specific index -- these
// are small models (a handful of joints/weights at most), so the extra
// iterations are negligible, and guessing a specific index wrong would
// silently reintroduce this exact bug for that one index.
inline void markModelJointsLive(J3DModel* model) {
    if (!model) return;
    const u16 jointNum = model->getModelData()->getJointNum();
    for (u16 i = 0; i < jointNum; ++i) {
        dusk::frame_interp::mark_live_this_frame(model->getAnmMtx(i));
    }
    const u16 wEvlpNum = model->getModelData()->getWEvlpMtxNum();
    for (u16 i = 0; i < wEvlpNum; ++i) {
        dusk::frame_interp::mark_live_this_frame(model->getWeightAnmMtx(i));
    }
}

// ACTUAL FIX for sword/shield lag (2026-08-09), same root cause and same
// fix shape as refreshTrackedHandDrawMtxLive() above: applyTrackedItemMtx()
// above is only ever called from d_a_alink.cpp, inside daAlink_c::draw() --
// proven (via the same [dusk::vr::eyepasscheck] capture that root-caused
// the hand-lag bug) to never run during a real VR eye pass. Called once per
// real frame from vr_main.cpp's tick(), before the per-eye loop opens.
// Takes no arguments (unlike applyTrackedItemMtx(), which is still called
// from its old dead-but-harmless d_a_alink.cpp call site) -- fetches the
// player and every matrix it needs itself, since this is a new,
// VR-internal-only call site with no existing per-eye plumbing to thread
// through. leftItemJointMtx/etc. are read directly off mpLinkModel's
// CURRENT joint matrices -- unlike the tracked hand pose, Link's own body
// animation genuinely only updates once per sim tick (real skeletal
// animation, not free-moving controller input), so reading it fresh from
// this later call site (instead of at the moment draw() used to run) is
// still just "this tick's already-correct value", no separate staleness
// problem to solve here.
// 7th round tried an item-joint-to-hand-joint DISTANCE heuristic here
// instead of the mEquipItem gate (removed) -- turned out useless: the item
// joint is a fixed rig joint that sits ~10 units from the hand joint
// UNCONDITIONALLY, sheathed or not (confirmed via direct capture), not a
// signal of anything gameplay-state-related. See refreshTrackedItemMtxLive()
// below for what actually settled this (direct instrumentation of
// daAlink_c::setItemMatrix() itself).
inline void refreshTrackedItemMtxLive() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;

    J3DModel* bodyModel = link->getBodyModel();
    if (!bodyModel) return;

    MtxP leftItemJointMtx = bodyModel->getAnmMtx(link->getLeftItemJntNo());
    MtxP leftHandJointMtx = bodyModel->getAnmMtx(link->getLeftHandJntNo());
    MtxP rightItemJointMtx = bodyModel->getAnmMtx(link->getRightItemJntNo());
    MtxP rightHandJointMtx = bodyModel->getAnmMtx(link->getRightHandJntNo());

    J3DModel* swordModel = link->getSwordModel();
    J3DModel* shieldModel = link->getShieldModel();

    // When NOT hand-attached, setItemMatrix()'s belt/back-relative resting
    // pose is still computed once per SIM TICK -- SHEATHED sword/shield are
    // just as subject to frame_interp's once-per-tick-interpolated
    // staleness as the tracked-hand case originally was, even though their
    // position doesn't need a tracked-pose override, just a "refresh + mark
    // live every real frame instead of raw un-smoothed steps" treatment
    // (refreshRestingPoseSmoothed()).
    //
    // FINAL FIX (2026-08-09, multiple rounds -- see
    // applyTrackedItemMtxIfAttached()'s own comment for the full history of
    // why two different position-based heuristics both failed for real
    // reasons): use the game's own REAL, authoritative hand-attach flags
    // (checkItemSwordEquip()/checkShieldHandAttached(), mirroring
    // setItemMatrix()'s exact conditions) instead of re-deriving "is this
    // attached" from position data that's legitimately stale by
    // construction during fast animation.
    const bool swordAttached = link->checkItemSwordEquip();
    const bool shieldAttached = link->checkShieldHandAttached();

    const bool swordUpdated = applyTrackedItemMtxIfAttached(
        swordModel, swordAttached, leftItemJointMtx, leftHandJointMtx, detail::s_leftHandMtx);
    if (swordModel) {
        if (!swordUpdated) {
            refreshRestingPoseSmoothed(swordModel, detail::s_swordRestingTick, detail::s_swordRestingValid,
                                        detail::s_swordRestingPrevMtx, detail::s_swordRestingCurrMtx);
        }
        markModelJointsLive(swordModel);
    }
    const bool shieldUpdated = applyTrackedItemMtxIfAttached(
        shieldModel, shieldAttached, rightItemJointMtx, rightHandJointMtx, detail::s_rightHandMtx);
    if (shieldModel) {
        if (!shieldUpdated) {
            refreshRestingPoseSmoothed(shieldModel, detail::s_shieldRestingTick, detail::s_shieldRestingValid,
                                        detail::s_shieldRestingPrevMtx, detail::s_shieldRestingCurrMtx);
        }
        markModelJointsLive(shieldModel);
    }
}

// ---------------------------------------------------------------------------
// Interpolated head/eye anchor
// ---------------------------------------------------------------------------
//
// daAlink_c::getSubjectEyePos() (field_0x3768) is only recomputed once per
// SIM TICK -- setBodyPartPos() runs from fapGm_Execute() inside
// m_Do_main.cpp's fixed-rate sim-tick loop, typically 20-30Hz on this
// GameCube-era game logic -- while VR needs a fresh camera position every
// RENDER frame (72-90Hz). Reading it directly stair-steps: the value only
// changes once every few render frames, which combined with the HMD's
// continuously-updating tracking delta on top reads as jitter -- and
// because the camera now sits right at Link's head, that stair-stepping
// makes his own nearby body geometry look like it's jittering too, even
// though his mesh itself still renders smoothly in world space.
//
// view->lookat.eye never had this problem because dusk::frame_interp
// (frame_interpolation.cpp) already lerps IT every render frame between the
// previous and current sim tick's recorded camera position
// (s_cam_prev/s_cam_curr, interp_view()). This reproduces that exact same
// prev/curr-snapshot-and-lerp technique for Link's head position instead,
// entirely from VR mod code -- no core engine changes needed.
//
// New-sim-tick detection uses frame_interp::sim_tick_seq() (incremented
// once per real sim tick via begin_sim_tick()) rather than is_sim_frame():
// by the time vr_main.cpp's tick() runs each frame, m_Do_main.cpp has
// already unconditionally reset is_sim_frame() to false for the
// presentation phase (see its begin_frame() call sequence around the
// sim-tick loop), so is_sim_frame() can't tell us whether THIS iteration
// actually ran a sim tick -- sim_tick_seq() changing is a reliable proxy,
// and get_interpolation_step() is populated from game_clock regardless of
// the user's "enable frame interpolation" setting (only interp_view()'s
// OWN early-out respects that setting -- confirmed by reading both
// call sites in m_Do_main.cpp), so this stays correct either way.
namespace detail {
inline cXyz s_eyeAnchorPrev{};
inline cXyz s_eyeAnchorCurr{};
inline bool s_eyeAnchorValid = false;
inline uint64_t s_lastSeenSimTick = 0;

inline cXyz lerpXyz(const cXyz& a, const cXyz& b, float t) {
    cXyz out;
    out.x = a.x + (b.x - a.x) * t;
    out.y = a.y + (b.y - a.y) * t;
    out.z = a.z + (b.z - a.z) * t;
    return out;
}

// ADDED 2026-08-09 (user request: anchor the VR camera to Link's ROOT/CORE
// position -- current.pos, the same physics-driven position setMatrix()
// uses to place mpLinkModel itself -- plus a fixed vertical offset, instead
// of his animated head-joint position (getSubjectEyePos()/field_0x3768)
// directly. Motivation is comfort, not the still-open hands/body-lag bug
// (section 20): current.pos doesn't bob, roll, or lurch with idle sway,
// footstep impact, or getting knocked around the way the head joint does,
// so a camera anchored to it should feel much calmer during exactly those
// moments, at the deliberate cost of no longer tilting/bobbing with Link's
// own head lean -- a tradeoff, not a bug.
//
// The vertical offset is CALIBRATED rather than hand-tuned: captured once
// per first-person activation from the real, current getSubjectEyePos()
// value (`realEye.y - current.pos.y`) so it automatically lands at
// roughly the right height for whatever stance is active (standing,
// crouched, swimming, etc.) without needing a whole family of
// hand-authored per-mode constants the way setBodyPartPos()'s own
// localEyeFromRoot/horseLocalEyeFromRoot/boardLocalEyeFromRoot/
// canoeLocalEyeFromRoot/wlLocalEyeFromRoot already does for a different
// purpose (see that function, d_a_alink.cpp) -- then held FIXED until the
// next activation. Deliberately NOT re-sampled every frame/tick: doing so
// would just reintroduce the exact bob/roll this exists to remove, since
// getSubjectEyePos() is the animated value. kCoreAnchorHeightOffsetDefault
// is only a placeholder used for the handful of frames before the first
// real calibration ever runs (isFirstPerson() true before setBodyPartPos()
// has executed even once) -- taken from localEyeFromRoot's own vertical
// component above as a reasonable standing-height guess, immediately
// overwritten by the real calibration on the very first activation.
inline constexpr float kCoreAnchorHeightOffsetDefault = 55.75f;
inline float s_coreAnchorHeightOffset = kCoreAnchorHeightOffsetDefault;
inline bool s_coreAnchorCalibrated = false;

// ADDED 2026-08-09 (user report, after testing the core-anchor change
// in-headset: "Link hunches forward when hes running and you can see your
// neck and back in the way"). A fixed nudge applied ON TOP of the
// calibrated core anchor above -- up and forward -- to clear the camera of
// his own hunched neck/shoulder/back geometry during fast movement. 100
// game units = 1 real metre (VR_SCALE_FACTOR above / vr_stereo_render.hpp's
// kEyePosScale, same established conversion used for hand tracking) -> 1in
// = 2.54 units.
//
// TUNED same day: original 6in-up guess was too much ("6 was too much my
// bad") -- brought down to 3in up (7.62 units). Forward left at 6in per no
// contrary feedback.
inline constexpr float kCoreAnchorExtraUpUnits = 7.62f;      // 3 real inches
inline constexpr float kCoreAnchorExtraForwardUnits = 15.24f; // 6 real inches

// Shared by getVrCameraEyeAnchor() and getVrBodyPositionOffset() so both
// agree on exactly the same definition of "the raw, this-instant,
// unsmoothed anchor point" -- factored out specifically to avoid the two
// drifting apart the way a second independent copy would (see
// getVrBodyPositionOffset()'s own header comment, the same standing lesson
// cited there). Calibrates/updates detail::s_coreAnchorHeightOffset as a
// side effect, same as before this was factored out. Caller must already
// know isFirstPerson(link) is true.
inline cXyz computeRawCoreAnchoredEye(daAlink_c* link) {
    const cXyz realEye = *link->getSubjectEyePos();
    if (!s_coreAnchorCalibrated) {
        s_coreAnchorHeightOffset = realEye.y - link->current.pos.y;
        s_coreAnchorCalibrated = true;
    }

    // Forward offset direction comes from Link's actual BODY-facing yaw
    // (current.angle.y -- the same field/convention d_a_alink.cpp itself
    // already uses for forward-offset placement, e.g. `current.pos.x +
    // N*cM_ssin(current.angle.y)` / `current.pos.z + N*cM_scos(current.angle.y)`
    // elsewhere in that file), NOT the HMD/smooth-turn yaw -- deliberately,
    // since the geometry being cleared (his hunched neck/back) is fixed
    // relative to his BODY, not to wherever the player happens to be
    // looking. current.angle.y is a signed 16-bit BAMS angle (SSystem's
    // csXyz/SVec convention, full circle = 65536) with 0 facing +Z and
    // positive rotating toward +X, matching that same existing call site;
    // converted to radians here (rather than pulling in this codebase's
    // separate cM_ssin/cM_scos fixed-angle trig helpers, unused elsewhere
    // in this file) since std::sin/std::cos are already this file's own
    // established convention for yaw math (see rotateYawXr() above).
    const float yawRad = static_cast<float>(link->current.angle.y) * (3.14159265f / 32768.0f);

    cXyz eye{link->current.pos.x, link->current.pos.y + s_coreAnchorHeightOffset,
             link->current.pos.z};
    eye.y += kCoreAnchorExtraUpUnits;
    eye.x += kCoreAnchorExtraForwardUnits * std::sin(yawRad);
    eye.z += kCoreAnchorExtraForwardUnits * std::cos(yawRad);
    return eye;
}

// FIXED 2026-08-09 (user request: "in gameplay link's head is on his core
// and in cutscenes link's head is anchored to the original head anchor").
// Section 23's core-anchor comfort tradeoff (no head bob/tilt/lean) was
// applied unconditionally to every first-person case above, including
// cutscenes/dialogue -- but those weren't the source of the original
// motion-sickness complaint (that was specifically about running/movement,
// per computeRawCoreAnchoredEye()'s own comment), and during a cutscene or
// conversation the ORIGINAL head-joint anchor (section 11's
// getSubjectEyePos(), pre-section-23) is more useful: it's what actually
// follows the authored animation/eyeline (a nod, a look-down, a lean) that
// a cutscene or conversation is often built around, rather than a rigid
// comfort-anchored point that never moves with it.
//
// Dispatches on the exact same "is an event currently running" condition
// isFirstPerson()'s own first branch already uses to distinguish ordinary
// gameplay from everything else (dialogue, cutscenes, door/transition
// events) -- reusing that condition here rather than inventing a second
// one, same "one shared definition" reasoning as isFirstPerson() itself.
// Caller must already know isFirstPerson(link) is true (same precondition
// as computeRawCoreAnchoredEye() above).
// SWIMMING (user request 2026-08-09, "fix the camera when swimming"):
// computeRawCoreAnchoredEye()'s height offset is calibrated ONCE per
// first-person activation (see s_coreAnchorCalibrated) and held fixed
// until first-person is left entirely (cutscene/Wolf/etc.) -- entering or
// leaving the water does NOT reset that calibration, so a player who
// calibrated standing on land and then goes for a swim keeps a stale,
// standing-height offset added on top of current.pos, which does not mean
// the same thing while swimming. The base game's own setBodyPartPos()
// explicitly special-cases swimming for its OWN eye-position math (see its
// FLG0_SWIM_UP/MODE_SWIMMING-gated branch, d_a_alink.cpp) -- confirming
// this really is a case the plain root+fixed-offset model was never
// designed to cover, not just an untested gap. Rather than reverse-engineer
// and duplicate that swim-specific math here, fall back to the same
// original, animation-driven head-joint anchor (getSubjectEyePos()) already
// used for cutscenes/dialogue below -- the base game's OWN swim-aware eye
// position, correct by construction for every stance (surface, diving,
// etc.) with no separate calibration needed. Effectively "use the
// already-proven first-person head anchor instead of the newer, swim-
// unverified comfort anchor" -- the same tradeoff this was requested as an
// explicit either/or ("fix the camera when swimming, or set the camera to
// first person").
//
// CRAWLING (user request 2026-08-09, "do the same fix with crawling"):
// identical reasoning -- the core anchor's calibrated height offset is
// captured once while standing and doesn't get invalidated by dropping
// into a crawl, so it's just as stale here as it was for swimming. Same
// fix: fall back to the raw head-joint anchor instead. See isCrawling()'s
// own comment for why this is an mProcID check, not a MODE_FLG bit.
//
// VINE CLIMBING (user request 2026-08-10, "make climbing vines
// specifically first person"): scoped to MODE_VINE_CLIMB specifically
// (d_a_alink.h, "used for vine climbing") -- a real, dedicated mode bit
// distinct from general MODE_CLIMB (ladders, weak walls), so ladder/
// weak-wall climbing is deliberately NOT affected by this, only vines.
// Same fallback as swimming/crawling: the core anchor's standing-height
// calibration doesn't hold up in a stance this different, and vine
// climbing already has its own animated eye/body pose the base game
// drives directly, same as swimming/crawling do.
inline cXyz computeRawEyeAnchor(daAlink_c* link) {
    if (link->checkModeFlg(daAlink_c::MODE_SWIMMING | daAlink_c::MODE_VINE_CLIMB) ||
        isCrawling(link))
    {
        return *link->getSubjectEyePos();
    }
    if (!link->checkEventRun()) {
        // Ordinary gameplay -- root/core-anchored, section 23.
        return computeRawCoreAnchoredEye(link);
    }
    // Cutscene or dialogue -- the original, pre-section-23 head-joint
    // anchor. No core-anchor height calibration or hunch-clearance nudge
    // here: both exist specifically to compensate for the core anchor and
    // for running/movement, neither of which applies to this branch.
    return *link->getSubjectEyePos();
}

// TUNED 2026-08-08 (user report: "when I am moving fast [hands] lag
// behind" -- root-caused to THIS anchor, since buildHandMtx()'s own
// controller-offset math has zero smoothing; hands = this anchor + that
// offset, so any lag here is inherited by both hands and, less
// noticeably (same lag, but you don't have proprioception for where the
// CAMERA "should" be the way you do for your own hand), the view itself).
//
// dusk::game_clock's sim tick runs at a fixed sim_pace() = 1/30s (~33ms,
// game_clock.cpp), and advance_main_loop() deliberately targets
// `render_time = now - kSimPeriodDuration` -- i.e. this whole engine's
// interpolation scheme (both the shared dusk::frame_interp used by the
// flatscreen camera, and this VR-local copy of the same technique) is
// designed to always render exactly one full sim tick BEHIND real time,
// on purpose, so it only ever blends between two already-known past
// samples and never has to guess. That's a real, constant ~33ms of
// latency, not just jitter -- worked through with pen and paper: at
// step=0 (right after prev/curr were captured) render shows `prev`,
// which is exactly one tick older than `curr`; at step→1 (just before
// the next tick) render shows ≈`curr`, which is itself already ~33ms
// old by then. The delay is constant across the whole cycle, not
// "averages out" -- which is why it read as a flat, constant-feeling lag
// rather than jitter once jitter itself was already fixed.
//
// Fix: EXTRAPOLATE instead of interpolate, i.e. estimate today's
// position by continuing the (curr - prev) velocity forward from `curr`
// by `step` more, instead of only ever blending between two samples that
// are both already in the past. Implemented by reusing lerpXyz() itself
// with `t = step + kEyeAnchorExtrapolationGain` instead of plain `step`
// -- lerpXyz(a, b, t) for t>1 is already exactly
// `b + (b-a)*(t-1)`, i.e. "keep going past b at the same rate," so no
// separate function is needed. At gain=1.0 (full compensation), the
// render target becomes `curr + (curr-prev)*step`, which at step=0 shows
// `curr` itself (freshly computed THIS tick, zero added lag) and at
// step→1 shows the predicted position for the tick boundary that's
// about to happen -- i.e. close to true real-time throughout, not
// constantly ~33ms behind.
//
// Known, accepted tradeoff: unlike pure interpolation (which can never
// overshoot, since it only ever blends between two confirmed real
// samples), extrapolation predicts based on the ASSUMPTION that velocity
// stays roughly constant for one more tick -- if Link suddenly stops
// (hits a wall, releases the stick) mid-cycle, this can overshoot past
// the real position for up to one tick (~33ms) before the next real
// sample corrects it, producing a brief, smoothly-self-correcting
// (not a hard pop/teleport) settle rather than a hard stop. Judged
// preferable to a CONSTANT, ever-present lag during all normal fast
// movement, which is what was actually reported as bothersome. If
// overshoot on sudden stops/direction changes turns out to be its own
// problem once tested in-headset, dial `kEyeAnchorExtrapolationGain` down
// from 1.0 toward 0.0 (0.0 = back to the original pure-interpolation,
// always-~33ms-behind behavior) rather than reverting this wholesale --
// same "ship a named, empirically-tunable constant" pattern already used
// elsewhere in this file (e.g. the swing detector's thresholds,
// vr_stereo_render.hpp's kHudDampingAlpha).
//
// Deliberately scoped to ONLY this VR-local eye-anchor lerp, not the
// shared dusk::frame_interp module the flatscreen camera and other
// interpolated matrices use -- this needed to be provably isolated to
// the one thing the user actually reported (VR hands/view lag), not a
// blanket engine-wide behavior change with much wider (and untested)
// blast radius.
inline constexpr float kEyeAnchorExtrapolationGain = 1.0f;
}  // namespace detail

// World-space position the VR camera should be anchored to for this frame.
//
// During normal gameplay, smoothly interpolates Link's root/core position
// raised to head height (section 23's comfort anchor -- see
// computeRawCoreAnchoredEye()'s comment). During a cutscene or NPC
// dialogue, smoothly interpolates his actual animated head/eye position
// instead (section 11's original anchor, restored 2026-08-09 per user
// request -- see detail::computeRawEyeAnchor()'s comment for why) --
// form-aware (wolf uses a different joint + local offset) and mount-aware
// (canoe/board/horse each get their own offset) for free in that branch,
// since it's built on the same getSubjectEyePos() the flatscreen camera
// already relies on.
//
// During an actual cutscene/door-transition event OR while transformed
// into Wolf Link -- see isFirstPerson()'s own comment above for exactly
// what counts as which (ordinary NPC dialogue does NOT trigger this
// fallback, per 2026-08-07 user request) -- returns `fallbackEye` (the
// caller's view->lookat.eye, the normal third-person follow camera)
// instead, and marks the interpolation state invalid so gameplay doesn't
// lerp FROM a stale pre-cutscene/pre-transformation position the next
// time first-person resumes. Wolf form is explicitly kept third-person
// per user request -- Wolf Link's model/animations (four-legged gait,
// different head joint entirely -- see setBodyPartPos()'s separate
// wlLocalEye branch) were never designed to be viewed from inside the
// wolf's own head. An authored cutscene camera isn't guaranteed to be
// looking at Link at all either -- snapping to his head there would put
// the viewer inside his skull for shots never designed to be seen from
// there. This mirrors eyePoseToViewMtx's existing comment about
// view->lookat.eye already being "correct whether that's normal
// follow-cam or an authored cutscene camera" -- we're narrowing that
// guarantee to human-form, non-cutscene gameplay (which now includes
// plain dialogue), not removing it.
inline cXyz getVrCameraEyeAnchor(const cXyz& fallbackEye) {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!isFirstPerson(link)) {
        detail::s_eyeAnchorValid = false;
        // Recalibrate on the NEXT activation rather than reusing whatever
        // stance's height happened to be calibrated before this fallback
        // (e.g. don't keep a crouching offset after a cutscene ends and
        // gameplay resumes standing) -- see detail::s_coreAnchorCalibrated's
        // own comment above.
        detail::s_coreAnchorCalibrated = false;
        return fallbackEye;
    }

    const uint64_t simTick = dusk::frame_interp::sim_tick_seq();

    // During ordinary gameplay: Link's root/core position -- physics-driven
    // (same value setMatrix() uses to place mpLinkModel itself), not
    // animation-driven, so it doesn't bob/roll/lurch the way the head joint
    // does. Raised to (approximately) head height via a calibrated fixed
    // offset -- see detail::computeRawCoreAnchoredEye()'s and
    // detail::s_coreAnchorHeightOffset's own comments above. During a
    // cutscene/dialogue: the original, pre-section-23 animated head-joint
    // anchor instead -- see detail::computeRawEyeAnchor()'s own comment.
    const cXyz freshEye = detail::computeRawEyeAnchor(link);

    if (!detail::s_eyeAnchorValid) {
        detail::s_eyeAnchorPrev = freshEye;
        detail::s_eyeAnchorCurr = freshEye;
        detail::s_eyeAnchorValid = true;
        detail::s_lastSeenSimTick = simTick;
    } else if (simTick != detail::s_lastSeenSimTick) {
        detail::s_eyeAnchorPrev = detail::s_eyeAnchorCurr;
        detail::s_eyeAnchorCurr = freshEye;
        detail::s_lastSeenSimTick = simTick;
    }

    const float step = dusk::frame_interp::get_interpolation_step();
    // See detail::kEyeAnchorExtrapolationGain's own comment (above,
    // next to lerpXyz()) for why this adds the gain to `step` instead of
    // passing `step` straight through -- extrapolates ahead to roughly
    // cancel this engine's usual constant ~1-sim-tick render lag, instead
    // of just smoothing between two already-stale samples.
    const cXyz extrapolated = detail::lerpXyz(detail::s_eyeAnchorPrev, detail::s_eyeAnchorCurr,
                                               step + detail::kEyeAnchorExtrapolationGain);

    // TEMP DIAGNOSTIC (2026-08-08 -- user report: "hands still lag behind"
    // even with the extrapolation fix above applied, specifically while
    // walking/running in-game). Confirmed via a direct follow-up question
    // this is genuinely about in-game locomotion speed, not just swinging
    // the controller while standing still -- so this anchor IS the right
    // place to keep looking, not a wrong-target theory. Logs the plain
    // interpolated value (what this function used to always return)
    // alongside the new extrapolated one and the distance between them,
    // every 20 frames, so we can see directly: (a) whether extrapolation
    // is actually moving the anchor by a meaningful amount during real
    // running (rules an outright no-op/bug in the fix above in or out),
    // and (b) the raw magnitude of that correction, to judge whether
    // residual lag from something else entirely (getSubjectEyePos()'s own
    // upstream animation-driven value, OpenXR frame timing, etc.) is the
    // actually-dominant remaining factor. Remove once the real cause is
    // confirmed and fixed.
    static int s_diagFrame = 0;
    if ((s_diagFrame++ % 20) == 0) {
        const cXyz plainInterp =
            detail::lerpXyz(detail::s_eyeAnchorPrev, detail::s_eyeAnchorCurr, step);
        const float ddx = extrapolated.x - plainInterp.x;
        const float ddy = extrapolated.y - plainInterp.y;
        const float ddz = extrapolated.z - plainInterp.z;
        const float correctionDist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        char msg[256];
        _snprintf_s(msg, _TRUNCATE,
            "[dusk::vr::anchordiag] step=%.3f plainInterp=(%.1f,%.1f,%.1f) "
            "extrapolated=(%.1f,%.1f,%.1f) correction=%.2f\n",
            step, plainInterp.x, plainInterp.y, plainInterp.z,
            extrapolated.x, extrapolated.y, extrapolated.z, correctionDist);
        OutputDebugStringA(msg);
    }

    return extrapolated;
}

// FIXED 2026-08-08 (section 20 continuation -- user report, after
// late-latching turned out to be a red herring: "link's entire body lags
// behind, including the hands. If I move the headset [i.e. as Link moves]
// ... and thats for all direction[s]"). Root cause, found by reading code
// rather than guessing a fourth time: `daAlink_c::setMatrix()`
// (`d_a_alink.cpp`) builds `mpLinkModel`'s own base transform directly
// from raw `current.pos`/`shape_angle` with ZERO interpolation, and is
// only ever called from `execute()` -- i.e. once per 30Hz SIM TICK, same
// as the rest of this engine's fixed-timestep game logic. Meanwhile the
// CAMERA (and, sharing the same anchor, the tracked HANDS) read
// `getVrCameraEyeAnchor()` above, which smooths AND extrapolates every
// render frame (72-90Hz in VR). The result: whenever Link is actually
// moving, the camera/hands glide smoothly ahead each render frame (per
// `kEyeAnchorExtrapolationGain`, literally extrapolated PAST the latest
// confirmed sim-tick sample) while his own BODY MESH's world position
// stays frozen at whatever `execute()` last set it to, visibly stair-
// stepping 30 times a second behind them -- exactly "body lags behind...
// for all directions" (direction-independent because extrapolation
// applies the same way regardless of which way Link is moving) and
// exactly why standing still ("the intro") looked fine (zero velocity ==
// zero extrapolation == nothing to diverge). This was very likely the
// TRUE cause of the original "hands lag" report too, more so than the
// compositor-reprojection theory section 20 was chasing -- the previous
// late-latching fix (re-sampling controller pose closer to draw time)
// was solving a real but apparently much smaller problem than this one.
//
// Fix: rather than building a SECOND, independent prev/curr+extrapolation
// tracker for current.pos/shape_angle (real risk of the two drifting out
// of sync under future tuning -- this file's own standing lesson, see
// vr_smooth_turn.hpp's header comment), reuse the eye anchor's smoothing
// directly. `getVrCameraEyeAnchor()` already computes exactly how far
// this frame's smoothed/extrapolated eye position has been pushed away
// from the raw, this-sim-tick `getSubjectEyePos()` value -- that same
// world-space delta is what the whole body needs applied to it too, to
// stay visually rigid with the camera/hands. Returns zero whenever
// `isFirstPerson()` is false (cutscenes, Wolf form, mounted cutscenes --
// see that function's own comment): the camera doesn't get any smoothing
// in those cases either (falls back to the plain flatscreen eye), so
// there's nothing to compensate for and the body should stay exactly
// where the base game already puts it.
inline cXyz getVrBodyPositionOffset(daAlink_c* link) {
    if (!link) return cXyz(0.f, 0.f, 0.f);
    if (!isFirstPerson(link)) return cXyz(0.f, 0.f, 0.f);

    // Must use the SAME raw-anchor basis getVrCameraEyeAnchor() itself
    // smooths from -- core-anchored during gameplay, head-joint-anchored
    // during a cutscene/dialogue (see detail::computeRawEyeAnchor()'s
    // comment above). Using a different/fixed basis here would compare two
    // different quantities and produce a delta that's actually the
    // core-vs-head-joint offset, not the lag-compensation delta this
    // function exists to compute.
    const cXyz freshEye = detail::computeRawEyeAnchor(link);
    const cXyz smoothedEye = getVrCameraEyeAnchor(freshEye);
    return cXyz(smoothedEye.x - freshEye.x, smoothedEye.y - freshEye.y, smoothedEye.z - freshEye.z);
}

// Applies getVrBodyPositionOffset() to a model's own base transform as a
// pure world-space translation (rotation left completely alone -- only
// POSITION lag was reported) and recalculates it so the shift actually
// reaches the draw this eye. Mathematically exact, not an approximation:
// for an affine transform, translating a PARENT frame by a fixed delta
// (leaving its rotation untouched) translates every descendant joint's
// resolved world matrix by that exact same delta, regardless of how deep
// the hierarchy is -- so rigidly shifting mpLinkModel's root here moves
// the entire animated body (every joint, every attached part that reads
// its matrices) in lockstep, not just the root joint itself. Called once
// per eye, immediately before `modelDraw(mpLinkModel, ...)` -- same
// "recalc, once per eye, right before the draw actually reads it" pattern
// already established and working for sword/shield (section 16).
inline void applyVrBodyPositionOffset(J3DModel* bodyModel) {
    if (!bodyModel) return;
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    const cXyz offset = getVrBodyPositionOffset(link);

    if (offset.x == 0.f && offset.y == 0.f && offset.z == 0.f) {
        return;
    }
    Mtx& base = bodyModel->getBaseTRMtx();
    base[0][3] += offset.x;
    base[1][3] += offset.y;
    base[2][3] += offset.z;
    bodyModel->calc();
}

inline void restoreVisibility() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;
    showModel(link->mpLinkFaceModel);
    showModel(link->mpLinkHatModel);
}

} // namespace vr_link
