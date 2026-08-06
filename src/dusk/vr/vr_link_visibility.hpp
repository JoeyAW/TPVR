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
inline void hideArmsAndEars(daAlink_c* link) {
    if (!link || !link->mpLinkModel || link->checkWolf()) return;
    J3DModelData* modelData = link->mpLinkModel->getModelData();
    if (!modelData) return;

    const u16 matNum = modelData->getMaterialNum();
    for (int idx : kArmEarMaterialIndices) {
        if (static_cast<u16>(idx) >= matNum) continue;
        J3DShape* shape = modelData->getMaterialNodePointer(idx)->getShape();
        if (shape) shape->hide();
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
}  // namespace detail

// Forward-declared here, defined further down (see its own comment) --
// needed by updateFrame() below so hands anchor to the SAME point the VR
// camera actually renders from.
inline cXyz getVrCameraEyeAnchor(const cXyz& fallbackEye);

inline void updateFrame(const FrameInput& input) {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;

    // Hide face and hat — these are separate J3DModel objects so hiding
    // their shape tables has no effect on the body or hand models.
    hideModel(link->mpLinkFaceModel);
    hideModel(link->mpLinkHatModel);
    hideArmsAndEars(link);

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
    const cXyz eyePos = getVrCameraEyeAnchor(view->lookat.eye);
    const XrVector3f& hmdPos = input.hmdPose.position;

    buildHandMtx(detail::s_rightHandMtx, hmdPos, input.rightControllerPose, eyePos, VR_SCALE_FACTOR, false, input.smoothTurnYawRad);
    buildHandMtx(detail::s_leftHandMtx, hmdPos, input.leftControllerPose, eyePos, VR_SCALE_FACTOR, true, input.smoothTurnYawRad);
    detail::s_handMtxValid = true;

    // TEMP DIAGNOSTIC (position-not-tracking investigation, remove once
    // confirmed fixed): the previous round's [dusk::vr::handpose] log
    // proved the raw controller poses are real/changing, and
    // [dusk::vr::applyhand] proved this override reaches the draw
    // (rotation visibly responds in-headset) -- but that log's world-space
    // numbers are dominated by linkEyeGame's own tens-of-thousands-of-units
    // magnitude, making a real but modest hand-tracking contribution
    // impossible to eyeball. This isolates the actual head-relative offset
    // (dx/dy/dz, metres) separately, fired every 15 frames (~6x/sec at
    // 90Hz) so even a few seconds of "hold still, wave one hand" testing
    // captures plenty of samples.
    static int frameCounter = 0;
    if ((frameCounter++ % 15) == 0) {
        const float rdx = input.rightControllerPose.position.x - hmdPos.x;
        const float rdy = input.rightControllerPose.position.y - hmdPos.y;
        const float rdz = input.rightControllerPose.position.z - hmdPos.z;
        char line[256];
        _snprintf_s(line, _TRUNCATE,
                    "[dusk::vr::handoffset] right dx,dy,dz=(%.4f,%.4f,%.4f) eyePos=(%.1f,%.1f,%.1f)\n",
                    rdx, rdy, rdz, eyePos.x, eyePos.y, eyePos.z);
        OutputDebugStringA(line);
    }
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
inline bool computeTrackedItemMtx(
    Mtx dest, MtxP currentBaseMtx, MtxP itemJointMtx, MtxP handJointMtx,
    MtxP trackedHandMtx)
{
    if (!mtxNearlyEqual(currentBaseMtx, itemJointMtx)) return false;

    Mtx handInv;
    if (!MTXInverse(handJointMtx, handInv)) return false;

    Mtx relativeOffset;
    MTXConcat(handInv, itemJointMtx, relativeOffset);
    MTXConcat(trackedHandMtx, relativeOffset, dest);
    return true;
}

inline void applyTrackedItemMtx(
    J3DModel* swordModel, J3DModel* shieldModel,
    MtxP leftItemJointMtx, MtxP leftHandJointMtx,
    MtxP rightItemJointMtx, MtxP rightHandJointMtx)
{
    if (!detail::s_handMtxValid) return;

    Mtx trackedMtx;
    if (swordModel &&
        computeTrackedItemMtx(trackedMtx, swordModel->getBaseTRMtx(),
                               leftItemJointMtx, leftHandJointMtx, detail::s_leftHandMtx))
    {
        swordModel->setBaseTRMtx(trackedMtx);
        swordModel->calc();
    }
    if (shieldModel &&
        computeTrackedItemMtx(trackedMtx, shieldModel->getBaseTRMtx(),
                               rightItemJointMtx, rightHandJointMtx, detail::s_rightHandMtx))
    {
        shieldModel->setBaseTRMtx(trackedMtx);
        shieldModel->calc();
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
}  // namespace detail

// World-space position the VR camera should be anchored to for this frame.
//
// During normal gameplay, smoothly interpolates Link's actual head/eye
// position (see the block comment above) -- form-aware (wolf uses a
// different joint + local offset) and mount-aware (canoe/board/horse each
// get their own offset) for free, since it's built on the same
// getSubjectEyePos() the flatscreen camera already relies on.
//
// During cutscenes/events (daAlink_c::checkEventRun()) OR while transformed
// into Wolf Link (daAlink_c::checkWolf(), a base-class player-state flag --
// see d_a_player.h), returns `fallbackEye` (the caller's view->lookat.eye,
// the normal third-person follow camera) instead, and marks the
// interpolation state invalid so gameplay doesn't lerp FROM a stale
// pre-cutscene/pre-transformation position the next time first-person
// resumes. Wolf form is explicitly kept third-person per user request --
// Wolf Link's model/animations (four-legged gait, different head joint
// entirely -- see setBodyPartPos()'s separate wlLocalEye branch) were never
// designed to be viewed from inside the wolf's own head. An authored
// cutscene camera isn't guaranteed to be looking at Link at all either --
// snapping to his head there would put the viewer inside his skull for
// shots never designed to be seen from there. This mirrors
// eyePoseToViewMtx's existing comment about view->lookat.eye already being
// "correct whether that's normal follow-cam or an authored cutscene
// camera" -- we're narrowing that guarantee to human-form gameplay only,
// not removing it.
inline cXyz getVrCameraEyeAnchor(const cXyz& fallbackEye) {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link || link->checkEventRun() || link->checkWolf()) {
        detail::s_eyeAnchorValid = false;
        return fallbackEye;
    }

    const uint64_t simTick = dusk::frame_interp::sim_tick_seq();
    const cXyz freshEye = *link->getSubjectEyePos();

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
    return detail::lerpXyz(detail::s_eyeAnchorPrev, detail::s_eyeAnchorCurr, step);
}

inline void restoreVisibility() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;
    showModel(link->mpLinkFaceModel);
    showModel(link->mpLinkHatModel);
}

} // namespace vr_link
