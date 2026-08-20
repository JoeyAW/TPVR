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
#include "dusk/settings.h"  // dusk::getSettings().game.vrThirdPerson
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_boomerang.h"
#include "d/actor/d_a_mg_rod.h"
#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_ext.h"  // mDoExt_McaMorf::getModel(), for the fishing rod's live-refresh
#include "dusk/frame_interpolation.h"
#include "f_op/f_op_view.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphAnimator/J3DShapeTable.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>  // TEMP DIAGNOSTIC: OutputDebugStringA for [dusk::vr::coreanchor] logging below

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
// Controller-pointing aim direction ("point where you want it to go")
//
// Goal (explicit user request): first-person item aiming (bow, slingshot,
// hookshot, boomerang -- everything that funnels through
// daAlink_c::setBodyAngleToCamera(), d_a_alink_link.inc, the same function
// gyro/mouse/touch aim already hook into) should follow the real RIGHT
// controller's pointing direction, not stick/gyro/mouse deltas -- point the
// controller where you want the shot to go, the way a real bow/slingshot/
// hookshot/boomerang throw would work. Right hand chosen unconditionally
// (explicit user choice) regardless of which hand the 3D item model happens
// to render in for a given item -- simpler and more predictable than
// tracking each item's own (sometimes switchable, e.g. checkBowGrabLeftHand())
// hand assignment.
//
// FIRST VERSION (reverted) reused buildHandMtx()'s right-hand mesh-forward
// calibration (applyStaticCorrection(right_hand_cal::kLocalForward), rotated
// by the grip quaternion). Real in-headset test found it off by a FIXED
// 90 degrees in yaw AND, more tellingly, physically yawing the controller
// made pitch/roll appear to swap -- direct evidence the underlying vector
// isn't correctly axis-aligned with the physical pointing direction at all
// (right_hand_cal's "forward" was calibrated for how the MESH should
// visually look holding a sword, section 12, never verified as "the
// direction a player naturally points this controller to aim"). A follow-up
// fixed-angle-space correction (subtract 90 degrees from yaw, negate pitch)
// was tried and also rejected without even reaching the headset: per this
// project's own hard-won lesson (CLAUDE.md, "don't attempt a column swap to
// fix an axis-confusion symptom -- provably cannot work", full algebraic
// proof in CLAUDE.md section 12) a UNIFORM correction -- whether a matrix
// column swap or a constant angle offset -- cannot change which physical
// rotation axis (yaw/pitch/roll) feeds which computed output; it can only
// shift the resting orientation. The "pitch/roll swapped" symptom is
// exactly what that lesson predicts for a wrong SOURCE vector, so patching
// the angle output further was the wrong class of fix.
//
// CURRENT VERSION: uses OpenXR's own AIM POSE directly instead of deriving
// from the grip/mesh calibration at all -- aim pose is spec-defined
// specifically as "the direction the user would point the controller to
// indicate a target" (local -Z axis), computed by the runtime from the
// controller's own tracked geometry, not from anything this app assumes
// about mesh orientation. This sidesteps the whole "which mesh-calibrated
// local axis is really the pointing direction" question by not depending on
// right_hand_cal/applyStaticCorrection at all. Local -Z convention and the
// rotateVecByQuat()-direct approach both mirror
// vr_stereo_render.hpp's computeHeadWorldForward() (the HMD's own already-
// proven-correct forward direction, confirmed via working VR movement
// direction) exactly, rather than inventing a new convention.
//
// Known risk, accepted for now: aim pose was found to be runtime-
// dependently unreliable in THIS project once before (CLAUDE.md section 12,
// "aim pose data itself is broken on whatever runtime this was tested on")
// -- but that finding was about the RELATIVE offset between grip and aim
// pose staying constant across many samples in one specific capture; a
// LATER capture on the SAME runtime showed aim pose behaving as a proper,
// physically meaningful fixed local-frame offset from grip. Used directly
// here (not derived FROM grip at all), so that specific failure mode
// doesn't apply the same way -- but this hasn't been proven reliable for
// this exact use yet either; worth a real in-headset test before trusting
// it as the final answer.
//
// Returns a world-space (game-world-convention, matching buildHandMtx()'s
// position math and computeHeadWorldForward()'s own convention -- no axis
// flip needed) direction vector, NOT an angle -- callers convert to this
// engine's s16 BAMS yaw/pitch themselves via cM_atan2s(), same reasoning as
// computeHeadWorldForward() (keeps this coordinate-math file free of an
// engine-specific angle convention).
inline Vec3f computeControllerAimForward(const XrPosef& rightAimPoseXR, float yawRad) {
    const XrQuaternionf q = dusk::vr::rotateYawQuat(rightAimPoseXR.orientation, yawRad);
    // OpenXR spec: aim pose's own local -Z axis is "the direction the user
    // would point the controller to indicate a target" -- same local
    // forward convention computeHeadWorldForward() already uses for the HMD.
    constexpr Vec3f kAimPoseForwardLocal{0.f, 0.f, -1.f};
    return rotateVecByQuat(q.x, q.y, q.z, q.w, kAimPoseForwardLocal);
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

// Held-item (mHeldItemModel/mpKanteraModel) tracking state -- see
// RawBasisCache's own comment for why this is needed (several branches
// layer an extra local offset on top of the item joint, unlike sword/
// shield's direct attach). Kantera also gets sword/shield's exact
// resting-pose-smoothing treatment for its belt/back pose.
inline Mtx s_kanteraRestingPrevMtx;
inline Mtx s_kanteraRestingCurrMtx;
inline uint64_t s_kanteraRestingTick = ~0ull;
inline bool s_kanteraRestingValid = false;
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
// Forward declarations -- isHookshotAirborneOrHanging()/isHookshotAiming()
// (below, next to isCrawling()) are needed by isFirstPerson()'s own Third
// Person carve-out right below, but are themselves defined further down
// this same file.
inline bool isHookshotAirborneOrHanging(daAlink_c* link);
inline bool isHookshotAiming(daAlink_c* link);

inline bool isFirstPerson(daAlink_c* link) {
    if (!link || link->checkWolf()) return false;

    // "Third Person" VR setting (added 2026-08-18, explicit user request:
    // "add a third person option that shows link's body and puts the
    // entire game in third person"). Forcing this single choke point to
    // false, unconditionally, before any of the event/mount checks below,
    // reuses every third-person fallback this file already has proven
    // in-headset (getVrCameraEyeAnchor() falls back to the flatscreen
    // third-person eye same as Wolf form/cutscenes; updateFrame() shows
    // face/hat/arms/ears the same as it already does for those same
    // fallback cases; tracked-hand/item overrides -- all gated on
    // isFirstPerson() -- correctly stop overriding pose so the normal
    // third-person animation shows instead) rather than inventing a
    // parallel camera-anchor/visibility path from scratch. See its
    // matching hideBodyForVr override (d_a_alink.cpp) for the "shows
    // link's body" half -- this function alone only stops FIRST-person
    // from ever being requested, it doesn't independently un-hide
    // anything the separate "Hide Body" setting hid.
    //
    // CARVE-OUT (2026-08-19, user report: "instead of going into first
    // person [while using the clawshot with Third Person on], the camera
    // goes under the ground behind link"): the flatscreen third-person
    // camera this falls back to (getVrCameraEyeAnchor()'s fallbackEye,
    // i.e. view->lookat.eye) was never confirmed to behave sanely during
    // hookshot flight/hanging -- unlike Wolf form and cutscenes, which
    // this fallback IS already proven correct for. Hookshot's own camera
    // fix (isHookshotAirborneOrHanging(), section "Hookshot/clawshot
    // flight + hanging" in vr-mod-notes) exists specifically because the
    // ordinary camera anchors don't hold up while Link is mid-air on a
    // chain or hanging off a wall/ceiling -- the third-person eye is
    // exactly as untested for this state as the anchors that fix already
    // had to route around. Per explicit user request ("try not to fix
    // first person"), rather than attempt a new third-person-camera-
    // during-hookshot fix from scratch, this specific state is exempted
    // from the Third Person override entirely -- stays first-person
    // (using the already-confirmed-correct hookshot anchor) even while
    // Third Person is on, everything else unaffected.
    //
    // FOLLOW-UP (same day, first attempt above insufficient): user
    // confirmed the actual drift happens specifically while AIMING the
    // clawshot (PROC_HOOKSHOT_SUBJECT -- NOT covered by
    // isHookshotAirborneOrHanging(), which deliberately excludes it), not
    // during flight/hanging. See isHookshotAiming()'s own comment for the
    // full mechanism (the base game's mode-4 "subject" aim camera,
    // untested under VR's absolute controller-pointing aim). Confirmed
    // via direct user follow-up that this is clawshot-specific, not a
    // general aim-camera issue (bow/slingshot/boomerang aiming was
    // explicitly checked and does NOT show this symptom) -- so scoped
    // narrowly to hookshot's own aiming state, not every aim-capable item.
    if (dusk::getSettings().game.vrThirdPerson.getValue() &&
        !isHookshotAirborneOrHanging(link) && !isHookshotAiming(link)) return false;

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

// Distinguishes a genuine scripted CUTSCENE (dEvt_type_OTHER_e/
// COMPULSORY_e) from plain dialogue (dEvt_type_TALK_e -- already handled
// separately via getMode()==dEvt_mode_TALK_e above) and from door/
// treasure-chest transitions (dEvt_type_DOOR_e/TREASURE_e). All three set
// the exact same dEvt_mode_DEMO_e once running -- see this function's own
// citations of demoCheck()/doorCheck() above -- so getMode() alone can't
// tell them apart; only the ORIGINAL event TYPE recorded on the accepted
// dEvt_order_c still carries that distinction once the event is actually
// running. dComIfGp_getEvent()'s mOrder[8]/mOrderIdx are both public
// members (d_event.h) -- mOrderIdx is set once, in entry(), when an order
// is accepted and starts running, and nothing in Step() (the per-frame
// event pump) touches it again until the event ends, so it stays valid
// for the event's whole duration, not just at the instant it started.
//
// Added 2026-08-18 for the "Hide Body" VR setting's own cutscene carve-
// out (see dusk::vr::isRealCutsceneRunning(), vr_main.hpp/.cpp, and its
// call site in d_a_alink.cpp) -- explicit user request, after confirming
// the base "hide body at all times" behavior worked: "show his body when
// in a cutscene? Not dialogue, not transition or doors, just cutscenes."
inline bool isRealCutsceneRunning() {
    if (!dComIfGp_event_runCheck()) return false;
    dEvt_control_c* event = dComIfGp_getEvent();
    if (!event) return false;
    const s8 idx = event->mOrderIdx;
    if (idx < 0 || idx >= 8) return false;
    const u16 type = event->mOrder[idx].mEventType;
    return type == dEvt_type_OTHER_e || type == dEvt_type_COMPULSORY_e;
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

// Same reasoning/shape as isCrawling() -- hookshot flight/hanging has no
// single MODE_FLG bit either, it's a sequence of dedicated daAlink_PROC
// states (d_a_alink.h). PROC_HOOKSHOT_FLY is "being pulled through the
// air" (user's own wording); PROC_HOOKSHOT_ROOF_WAIT/_WALL_WAIT is
// "hanging on to a clawshot target" once attached, and their _SHOOT/_BOOTS
// siblings (firing the second clawshot at another target, or standing in
// iron boots on the ceiling) are still attached to the original point the
// whole time, so grouped in here too rather than dropping back out of
// first-person for those sub-actions. PROC_HOOKSHOT_SUBJECT (aiming,
// before firing) and PROC_HOOKSHOT_MOVE (dragging a grabbed OBJECT, not
// Link's own body, toward himself) are deliberately excluded -- neither
// involves Link's own body leaving its normal standing pose.
inline bool isHookshotAirborneOrHanging(daAlink_c* link) {
    if (!link) return false;
    switch (link->mProcID) {
        case daAlink_c::PROC_HOOKSHOT_FLY:
        case daAlink_c::PROC_HOOKSHOT_ROOF_WAIT:
        case daAlink_c::PROC_HOOKSHOT_ROOF_SHOOT:
        case daAlink_c::PROC_HOOKSHOT_ROOF_BOOTS:
        case daAlink_c::PROC_HOOKSHOT_WALL_WAIT:
        case daAlink_c::PROC_HOOKSHOT_WALL_SHOOT:
            return true;
        default:
            return false;
    }
}

// SEPARATE from isHookshotAirborneOrHanging() above on purpose -- used
// ONLY by isFirstPerson()'s Third Person carve-out (2026-08-19 follow-up),
// NOT added to isHookshotAirborneOrHanging() itself or
// computeRawEyeAnchor()'s fallback list, since that would also change
// plain FIRST-PERSON camera behavior while aiming (untested, and the user
// explicitly asked not to touch first person -- "try not to fix first
// person"). PROC_HOOKSHOT_SUBJECT (standing, aiming before firing --
// "readying"/pointing the clawshot at a target) engages the base game's
// own flatscreen "subject"/aim camera mode (dCam_getBody()->ChangeModeOK(4),
// setSubjectMode(), d_a_alink_hook.inc's procHookshotRoofWait()/
// procHookshotRoofShoot()) -- a camera mode never exercised by this
// project's VR work before Third Person existed (first-person VR never
// reads the flatscreen camera object at all while aiming, so this mode's
// own behavior was untested here). Real symptom (user report, 2026-08-19):
// with Third Person on, aiming the clawshot makes the camera drift
// backward/downward continuously, worse aiming up, slower aiming down,
// not stopping until aim/Z-target releases -- consistent with this
// aim-camera mode running an unbounded position integration that's fine
// on flatscreen (bounded by normal analog-stick-driven aim rates) but
// runs away under VR's controller-pointing aim, which can swing the aim
// angle far more abruptly (setBodyAngleToCamera()'s VR branch assigns the
// ABSOLUTE controller angle every frame, not an incremental delta -- see
// that function's own comment, d_a_alink_link.inc). Rather than debug
// dCam's mode-4 subject-camera internals from scratch, simplest fix
// (matching isHookshotAirborneOrHanging()'s own precedent) is to just
// never let Third Person force this camera mode to engage in the first
// place -- stay first-person during hookshot aiming too, same as flight/
// hanging already does. PROC_SWIM_HOOKSHOT_SUBJECT is the underwater
// equivalent (d_a_alink.h) -- included for the same reason, not
// separately confirmed broken but presumptively the same code path.
inline bool isHookshotAiming(daAlink_c* link) {
    if (!link) return false;
    switch (link->mProcID) {
        case daAlink_c::PROC_HOOKSHOT_SUBJECT:
        case daAlink_c::PROC_SWIM_HOOKSHOT_SUBJECT:
            return true;
        default:
            return false;
    }
}

// DECOUPLES "camera should be first-person right now" from "the hookshot
// model should track the real controller" -- added 2026-08-19, same-day
// follow-up. isFirstPerson()'s Third Person carve-out (above) forces
// first-person during hookshot aim/fly/hang SPECIFICALLY so the camera
// anchor stays head-tracked instead of falling back to the broken
// third-person "subject" aim camera (the underground-drift bug) -- but
// this had an unintended side effect: since the tracked-hand override
// (refreshTrackedHookshotMtxLive() below) and getLeftItemMatrix()/
// getRightItemMatrix()'s own tracked-matrix substitution (d_a_alink_link.inc)
// are BOTH gated on the same isFirstPerson() value, forcing it true for
// the camera's sake also turned ON hand-tracking for the grip models --
// which the user explicitly did NOT want while Third Person is on
// ("Instead of returning to link's player model they are on my hands").
// Confirmed via direct question: user wants the CAMERA to stay
// first-person here (no drift), but the MODEL to keep following Link's
// normal animated hand pose instead of the tracked controller.
//
// This function is that narrower "should track" decision, used ONLY by
// the two hookshot-specific call sites below -- deliberately NOT folded
// into isFirstPerson() itself (that would also affect Wolf/cutscene/
// dialogue first-person, which is unrelated) and deliberately NOT
// changing getLeftItemMatrix()/getRightItemMatrix()'s own general gating
// (still isFirstPerson()-based, since ~10 OTHER unrelated consumers --
// arrows, boomerang, fishing rod, canoe paddle, enemy actors -- read
// those accessors too, and none of them are reachable while hookshot
// aim/fly/hang is active in practice, so touching their shared gate isn't
// warranted for a hookshot-only preference).
inline bool shouldTrackHookshotToHand(daAlink_c* link) {
    if (!isFirstPerson(link)) return false;
    if (dusk::getSettings().game.vrThirdPerson.getValue() &&
        (isHookshotAirborneOrHanging(link) || isHookshotAiming(link))) {
        return false;
    }
    return true;
}

// ADDED 2026-08-15 (user request: "make it so iron boots are using the
// first person anchor that swimming, climbing, cutscenes, etc use" --
// clarified the actual symptom is the camera feeling wrong while stuck to
// a wall/ceiling via Iron Boots' magnetism, not ordinary walking with them
// equipped -- isFirstPerson() already permits first-person for plain
// Iron-Boots gameplay same as everything else with no event running).
// Same underlying gap as every other entry in this list: the core anchor
// (section 23) assumes Link is standing upright on flat ground with
// world-up as up, and adds its calibrated height offset straight onto
// current.pos.y -- but magnetized Iron Boots can have his whole body
// pitched sideways on a wall or upside-down on a ceiling
// (setMagneBootsMtx(), d_a_alink_hvyboots.inc, rotates shape_angle/
// current.angle directly to match the magnetic surface's normal), so a
// fixed vertical-only offset from a standing calibration means nothing
// there. checkMagneBootsOn() (d_a_player.h, already public) covers
// "actually stuck," and PROC_MAGNE_BOOTS_FLY (d_a_alink_hvyboots.inc's
// procMagneBootsFlyInit()/procMagneBootsFly()) covers the brief
// being-pulled-toward-the-surface flight beforehand -- both leave Link's
// body away from a normal standing pose the same way
// isHookshotAirborneOrHanging()'s states do, so grouped the same way.
inline bool isMagnetized(daAlink_c* link) {
    if (!link) return false;
    return link->checkMagneBootsOn() || link->mProcID == daAlink_c::PROC_MAGNE_BOOTS_FLY;
}

// SAME DAY follow-up (user: "It's also an underwater issue" -- i.e. use
// the same raw-anchor treatment for this too). Iron Boots' other
// well-known use is sinking to walk along a submerged lake/river bed
// instead of floating. That state is NOT covered by MODE_SWIMMING: once
// Link actually lands on the underwater floor he transitions into an
// ordinary walking daAlink_PROC (confirmed via checkSwimUpAction(),
// d_a_alink_swim.inc -- calls the same procLandInit() used for normal
// dry-land landings), and mModeFlg gets fully REPLACED from that new
// proc's own entry in m_procInitTable (commonProcInit(), d_a_alink.cpp)
// rather than incrementally OR'd, so MODE_SWIMMING doesn't survive the
// transition even though he's still fully submerged -- confirmed
// separately from isMagnetized() above, not the same mechanism. The
// engine's own signal for "underwater and using Iron Boots to stay down,"
// independent of which proc state that lands him in, is
// FLG0_WATER_IN_MOVE (checkWaterInMove(), d_a_player.h -- already public,
// set in checkSwimUpAction()'s heavy-boots landing branch and cleared in
// swimOutAfter()). Note this is a DIFFERENT justification than
// isMagnetized()'s: setBodyPartPos()'s own eye branch (d_a_alink.cpp,
// line ~5581) actually takes the SAME root+local-offset path here as
// ordinary standing (its gate is MODE_SWIMMING-based, which is off by
// this point) -- so unlike the magnetized case, this isn't fixing an
// orientation mismatch the engine already solved elsewhere. It's the same
// "core anchor's standing-calibrated height doesn't necessarily transfer"
// risk as every other entry in this list (different sink/walk posture
// underwater is plausible, not confirmed) -- folded in per direct user
// request rather than a proven-first root cause.

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
    // FIXED 2026-08-14 (crash report: access violation reading 0xFFFF...FFFF
    // inside J3DShape::offFlag(), from showModel(mpLinkHatModel) here, while
    // changing armor/clothes on the menu screen). mClothesChangeWaitTimer
    // (d_a_alink.h, public getClothesChangeWaitTimer()) is the base game's
    // OWN guard for exactly this class of hazard -- a real handful of
    // existing show()/hide() call sites on these same body-related shape
    // tables in d_a_alink.cpp are already wrapped in
    // "if (mClothesChangeWaitTimer == 0) { ... }", because the underlying
    // J3DModelData/shape tables are apparently in a transient, unsafe-to-
    // touch state while a clothes change is in flight (the swap between
    // Hero's Clothes/Ordon Clothes model resources takes a few frames).
    // This VR code was calling showModel()/hideModel() on the SAME shapes
    // every real frame with no such guard, and could land mid-swap and
    // dereference a stale/freed shape-table pointer. Skipping entirely
    // during the wait window is safe and matches this function's own
    // existing "runs every frame either way, so a skipped frame just picks
    // back up correctly on the next one" reasoning above -- no state is
    // lost, the hide/show decision just resumes once the timer clears.
    // Deliberately scoped to just this block (not an early-return out of
    // the whole function) -- the tracked-hand-matrix computation below is
    // unrelated to the clothes-change hazard and shouldn't stall too, even
    // though in practice the player is looking at a menu during this brief
    // window either way.
    if (link->getClothesChangeWaitTimer() == 0) {
        const bool firstPerson = isFirstPerson(link) && !dComIfGp_isPauseFlag();
        if (firstPerson) {
            // Hide face and hat — these are separate J3DModel objects so
            // hiding their shape tables has no effect on the body or hand
            // models.
            hideModel(link->mpLinkFaceModel);
            hideModel(link->mpLinkHatModel);
            hideArmsAndEars(link);
        } else {
            showModel(link->mpLinkFaceModel);
            showModel(link->mpLinkHatModel);
            showArmsAndEars(link);
        }
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
// "Third Person" VR setting (2026-08-19 follow-up, user report: turning
// Third Person on left the sword/shield/hands still tracking the real
// controllers instead of showing normal third-person animation): this
// function -- unlike refreshTrackedItemJointMtxLive()/
// refreshTrackedBoomerangMtxLive()/refreshTrackedFishingRodMtxLive()/
// refreshTrackedHookshotMtxLive() below, which all already gate on
// isFirstPerson() -- never actually checked it, only swimming/crawling.
// isFirstPerson() itself already forces third-person whenever the VR
// setting is on (see its own comment), so gating here makes this function
// stop overriding the hand pose the same way it already does for Wolf
// form/cutscenes. Checked BEFORE dereferencing link for the swim/crawl
// checks below, not after -- a null link should never reach those either.
inline void refreshTrackedHandDrawMtxLive(J3DModel* handModel) {
    if (!handModel || !detail::s_handMtxValid) return;

    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link || !isFirstPerson(link)) return;
    if (link->checkModeFlg(daAlink_c::MODE_SWIMMING) || isCrawling(link)) return;

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
// "Third Person" VR setting (2026-08-19 follow-up): same gap as
// refreshTrackedHandDrawMtxLive() above -- never checked isFirstPerson(),
// so sword/shield kept tracking the real controllers even with Third
// Person forcing third-person camera/visibility. Returning early here
// leaves mSwordModel/mShieldModel's base transform (and frame_interp's
// once-per-tick recording of it) at whatever setItemMatrix()'s own
// once-per-tick write already set -- exactly the normal third-person
// fallback every other case (Wolf form, cutscenes) already relies on, no
// separate smoothing needed since mDoExt_modelUpdateDL() (d_a_alink.cpp)
// already resubmits geometry at real eye-pass rate regardless of
// first/third person.
inline void refreshTrackedItemMtxLive() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;
    if (!isFirstPerson(link)) return;

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
// Held-item (mHeldItemModel / mpKanteraModel) tracked-hand attachment
// ---------------------------------------------------------------------------
//
// Extends the sword/shield fix above to daAlink_c::setItemMatrix()'s
// broader "currently equipped item" model (bow, bottles, oil bottle, copy
// rod, boomerang, etc.) and the separate lantern/kantera model. Explicitly
// OUT OF SCOPE, left completely untouched: the 0x106 branch (attaches to
// the HEAD/face joint, not a hand at all), hookshot (setHookshotPos()
// drives a real fire/retract state machine across both item joints), and
// iron ball (setIronBallPos() reads a hardcoded joint 15 plus a real
// chain-link simulation and AtSph collision) -- none of these are a simple
// joint attach.
//
// Key difference from sword/shield: those always attach directly to their
// item joint with ZERO extra offset, so the raw body-model item-joint
// matrix could be used directly as computeTrackedItemMtx()'s basis.
// Several held-item branches (bottle, oil-bottle, bow-left-hand, kantera)
// layer an ADDITIONAL mDoMtx_stack_c translate+rotate on top of the item
// joint -- duplicating those literal offset constants into VR code would
// be a second place to keep in sync if setItemMatrix() ever changes them.
// Instead, RawBasisCache captures the model's own getBaseTRMtx() -- i.e.
// whatever setItemMatrix() actually computed this tick, offset included,
// whichever branch it took -- ONCE PER REAL SIM TICK, BEFORE this file's
// own tracked-pose override overwrites it. This must be cached rather than
// read live every real frame: a later-same-tick read would see our OWN
// prior overwrite instead of the real game pose -- the exact
// self-corruption trap applyTrackedItemMtxIfAttached()'s own comment
// documents two failed heuristics for (sword/shield's gating) -- avoided
// here structurally by capturing pre-override instead of comparing
// post-override.
struct RawBasisCache {
    Mtx mtx;
    uint64_t tick = ~0ull;
    bool valid = false;
};

// Safe to call unconditionally every real frame -- only does real work on
// the first call after a new sim tick (sim_tick_seq() changed). Must be
// called before any override write to `model` this frame.
inline void captureRawBasisOnce(J3DModel* model, RawBasisCache& cache) {
    if (!model) return;
    const uint64_t tick = dusk::frame_interp::sim_tick_seq();
    if (cache.valid && cache.tick == tick) return;
    cache.tick = tick;
    MTXCopy(model->getBaseTRMtx(), cache.mtx);
    cache.valid = true;
}

namespace detail {
inline RawBasisCache s_heldItemRawBasis;
inline RawBasisCache s_kanteraRawBasis;
}  // namespace detail

enum class HeldItemAttach { None, Left, Right };

// Mirrors daAlink_c::setItemMatrix()'s mHeldItemModel branch dispatch
// (d_a_alink.cpp), in the same order, using the real predicate helpers
// setItemMatrix() itself calls -- same "mirror the real dispatch
// condition structurally" precedent as daAlink_c::checkShieldHandAttached()
// (duplicates setItemMatrix()'s shield OR-chain for identical reasons).
// One more place that would silently drift out of sync if setItemMatrix()
// ever changes, but there's no cheaper way to ask "which branch did it
// take" without a real flag from that function itself.
inline HeldItemAttach computeHeldItemAttach(daAlink_c* link) {
    const u16 equip = link->getEquipItem();

    // Out of scope -- see this section's header comment.
    if (equip == 0x106) return HeldItemAttach::None;
    if (link->checkHookshotItem(equip)) return HeldItemAttach::None;
    if (link->checkIronBallEquip()) return HeldItemAttach::None;

    if (link->checkOilBottleItemNotGet(equip)) return HeldItemAttach::Right;
    if (link->checkBottleItem(equip)) return HeldItemAttach::Left;
    if (link->checkBowAndSlingItem(equip)) {
        return link->checkBowGrabLeftHand() ? HeldItemAttach::Left : HeldItemAttach::Right;
    }
    // Default/generic branch (copy rod, boomerang, etc.) -- matches
    // setItemMatrix()'s final `else`: direct mLeftItemJntNo attach, no
    // extra offset.
    return HeldItemAttach::Left;
}

// Mirrors the outer gate setItemMatrix() uses to decide whether
// mpKanteraModel is drawn/positioned at all this tick.
inline bool computeKanteraActive(daAlink_c* link) {
    return link->checkNoResetFlg2(daAlink_c::FLG2_UNK_1) ||
           link->checkEndResetFlg1(daAlink_c::ERFLG1_UNK_4);
}

// Mirrors the kantera branch's own hand-attach condition (only meaningful
// when computeKanteraActive() is true) -- Left when kantera is the
// equipped item or an oil bottle not yet obtained is, else None (resting
// at the fixed belt/back joint 0x10, still drawn but not hand-attached).
inline HeldItemAttach computeKanteraAttach(daAlink_c* link) {
    const u16 equip = link->getEquipItem();
    if (equip == dItemNo_KANTERA_e || link->checkOilBottleItemNotGet(equip)) {
        return HeldItemAttach::Left;
    }
    return HeldItemAttach::None;
}

inline void detail_pickHeldItemHandRefs(daAlink_c* link, J3DModel* bodyModel, bool isLeft,
                                         MtxP& outHandJointMtx, MtxP& outTrackedHandMtx) {
    outHandJointMtx = bodyModel->getAnmMtx(isLeft ? link->getLeftHandJntNo() : link->getRightHandJntNo());
    outTrackedHandMtx = isLeft ? detail::s_leftHandMtx : detail::s_rightHandMtx;
}

// Called once per real frame from vr_main.cpp's tick(), alongside (and
// after) refreshTrackedItemMtxLive() -- same "before the per-eye loop
// opens" ordering, same reasons.
// "Third Person" VR setting (2026-08-19 follow-up): same gap as
// refreshTrackedItemMtxLive()/refreshTrackedHandDrawMtxLive() above --
// never checked isFirstPerson(), so held items (bow, bottles, lantern,
// copy rod, etc.) kept tracking the real controllers in third person too.
inline void refreshTrackedHeldItemMtxLive() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;
    if (!isFirstPerson(link)) return;

    J3DModel* bodyModel = link->getBodyModel();
    if (!bodyModel) return;

    J3DModel* heldItemModel = link->getHeldItemModel();
    if (heldItemModel) {
        captureRawBasisOnce(heldItemModel, detail::s_heldItemRawBasis);

        const HeldItemAttach attach = computeHeldItemAttach(link);
        MtxP handJointMtx = nullptr;
        MtxP trackedHandMtx = nullptr;
        detail_pickHeldItemHandRefs(link, bodyModel, attach != HeldItemAttach::Right,
                                     handJointMtx, trackedHandMtx);

        const bool attached = (attach != HeldItemAttach::None) && detail::s_heldItemRawBasis.valid;
        if (applyTrackedItemMtxIfAttached(heldItemModel, attached, detail::s_heldItemRawBasis.mtx,
                                           handJointMtx, trackedHandMtx)) {
            markModelJointsLive(heldItemModel);
        }
        // attach == None (head-item / hookshot / iron ball): leave
        // completely untouched, including frame_interp's normal
        // once-per-tick smoothing -- correct for everything this fix
        // doesn't cover.
    }

    J3DModel* kanteraModel = link->getKanteraModel();
    if (kanteraModel && computeKanteraActive(link)) {
        captureRawBasisOnce(kanteraModel, detail::s_kanteraRawBasis);

        const HeldItemAttach attach = computeKanteraAttach(link);
        MtxP handJointMtx = bodyModel->getAnmMtx(link->getLeftHandJntNo());
        const bool attached = (attach == HeldItemAttach::Left) && detail::s_kanteraRawBasis.valid;

        const bool updated = applyTrackedItemMtxIfAttached(
            kanteraModel, attached, detail::s_kanteraRawBasis.mtx, handJointMtx, detail::s_leftHandMtx);
        if (!updated) {
            refreshRestingPoseSmoothed(kanteraModel, detail::s_kanteraRestingTick, detail::s_kanteraRestingValid,
                                        detail::s_kanteraRestingPrevMtx, detail::s_kanteraRestingCurrMtx);
        }
        markModelJointsLive(kanteraModel);

        // The flame glow sprite (mpKanteraGlowModel) is a SEPARATE model
        // from kanteraModel (the lantern body) above -- its own base
        // transform is only ever set from mKandelaarFlamePos inside
        // setItemMatrix()'s once-per-sim-tick block (d_a_alink.cpp), which
        // is what left it visibly stuck at "the original position" once
        // the lantern body above started tracking at real VR framerate
        // (explicit user report, after this session's lantern-swing-
        // physics VR disable -- d_a_alink_kandelaar.inc's
        // kandelaarModelCallBack()). kanteraModel->calc() just above
        // (inside applyTrackedItemMtxIfAttached()/refreshRestingPoseSmoothed())
        // already re-triggers kandelaarModelCallBack() -- a joint callback
        // J3DModel::calc() invokes automatically during its joint
        // traversal -- which recomputes mKandelaarFlamePos fresh every
        // real frame (now swing-free per that guard), so by this point it
        // already reflects the tracked position; this just copies it into
        // the glow model's own transform, the same shape setItemMatrix()'s
        // own code uses (a plain translation, no rotation).
        J3DModel* glowModel = link->getKanteraGlowModel();
        if (glowModel) {
            const cXyz& flamePos = link->getKandelaarFlamePosRaw();
            Mtx glowMtx;
            MTXTrans(glowMtx, flamePos.x, flamePos.y, flamePos.z);
            glowModel->setBaseTRMtx(glowMtx);
            glowModel->calc();
            markModelJointsLive(glowModel);

            // The actual flame VFX players see -- a JPA particle effect
            // (ID_ZI_J_KANTERA_FIRE/_SWINGFIRE), NOT glowModel above
            // (a supplementary soft light halo -- fixing only that one
            // is what the previous round's "it's lit up, but the flame
            // itself is still where it would be on Link's model" report
            // was describing). This particle is spawned/kept alive by
            // daAlink_c::setLight() (d_a_alink.cpp), called from the
            // actor's normal per-tick update -- same legacy sim-tick-only
            // call pattern as setItemMatrix() above, and NOT re-anchored
            // anywhere in d_a_alink_effect.inc's own per-tick particle
            // position-refresh file (checked directly -- no
            // setGlobalRTMatrix() call there references this particle's
            // id at all), so its emitter's own global position only ever
            // reflects wherever mKandelaarFlamePos happened to be at the
            // last sim tick setLight() ran, same underlying bug class as
            // the glow model just above. Re-anchored here every real VR
            // frame via setGlobalTranslation() (a plain position setter,
            // not a full re-spawn -- doesn't disturb the particle's own
            // ongoing simulation/flicker/lifetime).
            JPABaseEmitter* flameEmitter = dComIfGp_particle_getEmitter(link->getKandelaarParticleId());
            if (flameEmitter) {
                flameEmitter->setGlobalTranslation(flamePos.x, flamePos.y, flamePos.z);

                // CONFIRMED FIXED IN-HEADSET (2026-08-13). Full investigation
                // trail (why setGlobalTranslation() alone wasn't enough, the
                // status-0x20/mark_live_this_frame() layer below it, and the
                // real root cause -- this whole particle system's calc_p()
                // only running via the legacy, sim-tick-rate-only
                // dScnPly_Draw() path, same bug class as vr-mod-notes section
                // 20's daAlink_c::draw() saga) is in vr-mod-notes, not
                // reproduced here.
                //
                // setGlobalTranslation() above keeps mGlobalTrs/mGlobalPos
                // correct for newly-spawned particles and for whenever the
                // legacy ~30Hz calc_p() tick does land. status 0x20 (below)
                // is what makes THAT eventual calc_p() re-anchor
                // mOffsetPosition from mGlobalPos, so it can never drift out
                // of sync with what this loop writes directly. But since that
                // tick only lands ~1/3 as often as VR actually renders, the
                // loop below also writes mOffsetPosition/mPosition directly,
                // every real frame -- preserving each particle's own local
                // wobble (mPosition - mOffsetPosition, e.g. flicker/gravity
                // drift) but re-rooting it at the fresh tracked position --
                // so the visible result is correct every real frame
                // regardless of the underlying sim-tick cadence.
                // mark_live_this_frame() stops JPADrawRotBillboard()'s own
                // dusk::frame_interp::lookup_replacement(ptcl, ...) check
                // from substituting a stale interpolated snapshot over what
                // this loop just wrote -- uses the particle pointer itself as
                // the key, the same identity that lookup keys off.
                for (JPANode<JPABaseParticle>* node = flameEmitter->mAlivePtclBase.getFirst();
                     node != flameEmitter->mAlivePtclBase.getEnd(); node = node->getNext()) {
                    JPABaseParticle* p = node->getObject();
                    const f32 localX = p->mPosition.x - p->mOffsetPosition.x;
                    const f32 localY = p->mPosition.y - p->mOffsetPosition.y;
                    const f32 localZ = p->mPosition.z - p->mOffsetPosition.z;
                    p->mOffsetPosition.set(flamePos.x, flamePos.y, flamePos.z);
                    p->mPosition.set(flamePos.x + localX, flamePos.y + localY, flamePos.z + localZ);
                    p->setStatus(0x20);
                    dusk::frame_interp::mark_live_this_frame(p);
                }
                for (JPANode<JPABaseParticle>* node = flameEmitter->mAlivePtclChld.getFirst();
                     node != flameEmitter->mAlivePtclChld.getEnd(); node = node->getNext()) {
                    JPABaseParticle* p = node->getObject();
                    const f32 localX = p->mPosition.x - p->mOffsetPosition.x;
                    const f32 localY = p->mPosition.y - p->mOffsetPosition.y;
                    const f32 localZ = p->mPosition.z - p->mOffsetPosition.z;
                    p->mOffsetPosition.set(flamePos.x, flamePos.y, flamePos.z);
                    p->mPosition.set(flamePos.x + localX, flamePos.y + localY, flamePos.z + localZ);
                    p->setStatus(0x20);
                    dusk::frame_interp::mark_live_this_frame(p);
                }
            }
        }
    }
}

// See dusk::vr::getTrackedHandWorldPos() (vr_main.hpp) for the
// caller-facing contract (used to VR-track a carried/grabbed actor's
// position source, e.g. a held bomb -- see setBodyPartPos()'s call site,
// d_a_alink.cpp). Reads the translation column directly out of
// detail::s_leftHandMtx/s_rightHandMtx -- the same fully-calibrated,
// confirmed-correct tracked-hand matrices buildHandMtx() already produces
// for mpLinkHandModel/sword/shield/held items above.
inline bool getTrackedHandWorldPos(bool isLeftHand, float& outX, float& outY, float& outZ) {
    if (!detail::s_handMtxValid) return false;
    MtxP m = isLeftHand ? detail::s_leftHandMtx : detail::s_rightHandMtx;
    outX = m[0][3];
    outY = m[1][3];
    outZ = m[2][3];
    return true;
}

// ---------------------------------------------------------------------------
// getLeftItemMatrix()/getRightItemMatrix() VR-awareness
// ---------------------------------------------------------------------------
//
// daAlink_c::getLeftItemMatrix()/getRightItemMatrix() (mpLinkModel->
// getAnmMtx(mLeftItemJntNo/mRightItemJntNo), d_a_alink_link.inc) are
// virtual and read directly by ~10 OTHER actor files for effects anchored
// to whatever's in Link's hand: nocked arrows (d_a_alink_bow.inc,
// d_a_arrow.cpp), boomerang throw/trail (d_a_boomerang.cpp), fishing rod
// (d_a_mg_rod.cpp, d_a_mg_fish.cpp), several enemy-interaction actors
// (d_a_e_bug.cpp/d_a_e_fm.cpp/d_a_e_gob.cpp/d_a_e_sm2.cpp), an NPC
// interaction (d_a_npc_tk.cpp), an object interaction (d_a_obj_lp.cpp),
// and the canoe paddle (d_a_alink_canoe.inc). None of those files were
// made VR-aware by refreshTrackedHeldItemMtxLive() above (which only
// overrides mHeldItemModel's/mpKanteraModel's OWN draw matrix, not the
// raw body-joint matrix these other actors read directly) -- reported by
// the user as "fishing rod and boomerang do not track."
//
// Fix lives at the SOURCE instead of touching every caller: since
// getLeftItemMatrix()/getRightItemMatrix() are virtual and daAlink_c
// already overrides them, changing the two function bodies (d_a_alink_link.inc)
// to return a tracked-hand-relative matrix while rendering to the headset
// fixes every one of those ~10 consumers for free, through ordinary
// virtual dispatch -- no changes needed to any of those files. This ALSO
// means canoe paddling and the enemy-interaction cases above now get
// tracked-hand-relative positions too, as a natural side effect of fixing
// the general mechanism, not something individually verified in-headset.
//
// Unlike the held-item RawBasisCache above, no once-per-tick caching is
// needed here: mpLinkModel->getAnmMtx(mLeftItemJntNo/mRightItemJntNo) is
// the BODY model's own joint matrix, which nothing in this VR code ever
// writes to (only mSwordModel/mShieldModel/mHeldItemModel/mpKanteraModel's
// own base transforms are overridden elsewhere in this file) -- safe to
// read fresh every real frame with no self-corruption risk.
//
// Gated on isFirstPerson(link), not just isRenderingToHeadset() (unlike
// sword/shield/held-item's own overrides above, which have no such gate --
// safe there only because those items simply aren't drawn/equipped during
// Wolf form or third-person cutscenes in practice). getLeftItemMatrix()/
// getRightItemMatrix() are called unconditionally by files with no
// awareness of Link's current form (e.g. enemy-interaction actors could
// run in Wolf form too) -- during Wolf form mLeftItemJntNo/mLeftHandJntNo
// collapse to the SAME joint (19), which would make the computed relative
// offset an identity and return the tracked hand's raw matrix directly,
// wrong for Wolf's different rig/third-person camera. Gating on
// isFirstPerson() (already covers Wolf/mounted-cutscene/hidden-cutscene
// correctly, see its own definition below) avoids that risk instead of
// discovering it via a report later.
namespace detail {
inline Mtx s_trackedLeftItemJointMtx;
inline Mtx s_trackedRightItemJointMtx;
inline bool s_trackedItemJointMtxValid = false;
}  // namespace detail

// Called once per real frame from vr_main.cpp's tick(), before the
// per-eye loop opens -- same ordering as every other refresh*Live()
// function in this file.
inline void refreshTrackedItemJointMtxLive() {
    detail::s_trackedItemJointMtxValid = false;

    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link || !detail::s_handMtxValid || !isFirstPerson(link)) return;

    J3DModel* bodyModel = link->getBodyModel();
    if (!bodyModel) return;

    MtxP leftItemJointMtx = bodyModel->getAnmMtx(link->getLeftItemJntNo());
    MtxP leftHandJointMtx = bodyModel->getAnmMtx(link->getLeftHandJntNo());
    MtxP rightItemJointMtx = bodyModel->getAnmMtx(link->getRightItemJntNo());
    MtxP rightHandJointMtx = bodyModel->getAnmMtx(link->getRightHandJntNo());

    const bool leftOk = computeTrackedItemMtx(
        detail::s_trackedLeftItemJointMtx, leftItemJointMtx, leftHandJointMtx, detail::s_leftHandMtx);
    const bool rightOk = computeTrackedItemMtx(
        detail::s_trackedRightItemJointMtx, rightItemJointMtx, rightHandJointMtx, detail::s_rightHandMtx);
    detail::s_trackedItemJointMtxValid = leftOk && rightOk;
}

// Copies the tracked-hand-relative item-joint matrix into outMtx; returns
// false (leaving outMtx untouched) if not available this frame -- caller
// (daAlink_c::getLeftItemMatrix()/getRightItemMatrix(), d_a_alink_link.inc)
// falls back to the raw joint matrix in that case.
inline bool getTrackedItemJointMtx(bool isLeft, Mtx outMtx) {
    if (!detail::s_trackedItemJointMtxValid) return false;
    MTXCopy(isLeft ? detail::s_trackedLeftItemJointMtx : detail::s_trackedRightItemJointMtx, outMtx);
    return true;
}

// VR fix (2026-08-12): boomerang lag, same root cause/fix shape as the
// hand/sword/shield lag section 20 root-caused -- getLeftItemMatrix() is
// fresh every real frame (via refreshTrackedItemJointMtxLive() above), but
// daBoomerang_c::setKeepMatrix() (its own base-transform setter) is only
// ever called once per sim tick, from procWait() -- so the boomerang's
// visible position stair-stepped at 30Hz even though the matrix it reads
// was already fresh. Fixed the same way as every other item in this file:
// re-run just the transform-setting half
// (daBoomerang_c::applyTrackedKeepTransforms(), split out of
// setKeepMatrix() for exactly this purpose) once per real frame here, on
// top of setKeepMatrix()'s own once-per-tick call (which still separately
// owns current.pos and the wait-animation restart -- both intentionally
// excluded from the live-refreshed half, see applyTrackedKeepTransforms()'s
// own comment in d_a_boomerang.h).
//
// Gated to the KEPT (held, not yet thrown) boomerang only --
// getThrowBoomerangAcKeep()'s id becomes valid at the exact moment the
// same actor transitions from procWait (held, hand-attached) to procMove
// (thrown flight, positioned entirely by setMoveMatrix()'s own physics,
// unrelated to the tracked hand) -- checked directly against the real
// state transition rather than assumed.
//
// ROUND 2 FIX (user-tested round 1, still laggy): setBaseTRMtx() alone
// was never enough -- missed the other half of the ALREADY-established
// pattern (applyTrackedItemMtxIfAttached()/markModelJointsLive() above):
// without an explicit calc() AND mark_live_this_frame() call THIS real
// frame, frame_interp's own draw-time substitution overrides the fresh
// transform with a stale once-per-tick-interpolated snapshot regardless
// of how often setBaseTRMtx() itself gets called -- the exact mechanism
// section 20 root-caused for hands/sword/shield. Explicit calc() here
// (not left to daBoomerang_c::draw()'s own mDoExt_modelUpdateDL() call)
// matches applyTrackedItemMtxIfAttached()'s own shape exactly, rather
// than assuming draw() runs at a point in the frame that makes it moot.
inline void refreshTrackedBoomerangMtxLive() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link || !isFirstPerson(link)) return;

    if (link->getThrowBoomerangAcKeep()->getID() != fpcM_ERROR_PROCESS_ID_e) return;

    fopAc_ac_c* boomActor = link->getBoomerangActor();
    if (!boomActor) return;

    daBoomerang_c* boomerang = static_cast<daBoomerang_c*>(boomActor);
    boomerang->applyTrackedKeepTransforms();

    if (J3DModel* m = boomerang->getBoomModel()) {
        m->calc();
        markModelJointsLive(m);
    }
    if (J3DModel* m = boomerang->getShippuModel()) {
        m->calc();
        markModelJointsLive(m);
    }
    if (J3DModel* m = boomerang->getSetboomEfModel()) {
        m->calc();
        markModelJointsLive(m);
    }
}

// VR fix (2026-08-12): same root cause/fix shape as
// refreshTrackedBoomerangMtxLive() above, applied to the fishing rod.
// rod_control() (d_a_mg_rod.cpp) only ever runs once per sim tick, inside
// the fishing minigame's own execute() -- even though getLeftItemMatrix()/
// getRightItemMatrix() are now fresh every real frame (via
// refreshTrackedItemJointMtxLive() above), the rod's own attach-point
// derivation only ever sampled that fresh value at 30Hz, so the rod
// visibly stair-stepped the same way the boomerang did. Re-runs it here at
// real frame rate via dmg_rod_refreshTrackedPositionLive() (d_a_mg_rod.h),
// which snapshots/restores the one piece of real per-tick state
// rod_control() writes (a tip-position delta-tracking pair, presumably
// feeding rod-tip-velocity physics elsewhere) so this extra call can't
// corrupt whatever downstream logic reads it -- see that function's own
// comment for the full field-write audit this rests on.
// ROUND 2 FIX (user-tested round 1, still laggy) -- same missing piece as
// the boomerang's own round 2 above: dmg_rod_refreshTrackedPositionLive()
// re-runs rod_control() (which calls setBaseTRMtx() on several models),
// but without an explicit calc() + mark_live_this_frame() call THIS real
// frame, frame_interp's draw-time substitution still overrides the fresh
// transform with a stale once-per-tick snapshot regardless of how often
// setBaseTRMtx() itself runs. Marks every model rod_control() (or the
// rod's own draw()) could plausibly set a transform on -- deliberately
// over-inclusive rather than tracing exactly which ones rod_control()
// itself touches per kind/action, matching markModelJointsLive()'s own
// established "small models, negligible extra cost" reasoning.
inline void refreshTrackedFishingRodMtxLive() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link || !isFirstPerson(link)) return;

    fopAc_ac_c* rodActor = fopAcM_SearchByName(fpcNm_MG_ROD_e);
    if (!rodActor) return;

    dmg_rod_class* rod = reinterpret_cast<dmg_rod_class*>(rodActor);
    dmg_rod_refreshTrackedPositionLive(rod);

    auto markLive = [](J3DModel* m) {
        if (!m) return;
        m->calc();
        markModelJointsLive(m);
    };

    for (int i = 0; i < 15; ++i) markLive(rod->rod_uki_model[i]);
    for (int i = 0; i < 6; ++i) markLive(rod->unk_ring_model[i]);
    for (int i = 0; i < 5; ++i) markLive(rod->lure_model[i]);
    for (int i = 0; i < 2; ++i) markLive(rod->hook_model[i]);
    for (int i = 0; i < 2; ++i) markLive(rod->esa_model[i]);
    markLive(rod->ring_model);
    markLive(rod->uki_model);
    markLive(rod->uki_saki_model);
    if (rod->rod_modelMorf) markLive(rod->rod_modelMorf->getModel());
}

// ADDED 2026-08-14 (user report: "the fish bite and when I pull they just
// let go" -- traced the real minigame code, not guessed: hook-setting reads
// dmg_rod_class::rod_stick_y < -0.5f (d_a_mg_rod.cpp), i.e. the MAIN/left
// stick pulled sharply back -- not the C-stick, which is only used for
// casting power/direction. VR's left thumbstick already correctly feeds
// rod_stick_y (unchanged since section 13's original mapping), so the
// mechanic was never actually broken -- but "pull the left thumbstick down
// with your thumb at the exact moment a fish bites" isn't an intuitive VR
// gesture, unlike physically yanking the rod-holding hand back. This gate
// lets vr_main.cpp's tick() reuse the existing g_rightThrust gesture
// detector (any fast right-hand motion, already wired to shield-bash) to
// ALSO force a stick-down pulse while fishing -- reusing rather than adding
// a second tuned detector, since overloading is harmless: forcing R while
// fishing does nothing (no shield equipped), and forcing a stick-down pulse
// outside an active fish bite does nothing either (the game's own
// mRemainingHookTime check, not duplicated here, gates whether the pulse
// has any effect). Scoped to "hook actually in the water" (not just "rod
// equipped") so the forced pulse can't nudge movement during ordinary
// casting/idle rod-holding -- dmg_rod_class::is_hook_in_water is already a
// public field (d_a_mg_rod.h), no new accessor needed on that class.
inline bool isFishingHookInWater() {
    fopAc_ac_c* rodActor = fopAcM_SearchByName(fpcNm_MG_ROD_e);
    if (!rodActor) return false;
    dmg_rod_class* rod = reinterpret_cast<dmg_rod_class*>(rodActor);
    return rod->is_hook_in_water != 0;
}

// ADDED 2026-08-14 (follow-up user report: "Moving the rod still unhooks
// the fish" -- the yank gesture above didn't fix it; user explicitly asked
// to "bind C stick to the right stick, but only while you are fishing"
// instead). Broader than isFishingHookInWater() above -- true whenever the
// fishing-rod actor exists at all (casting, waiting, hook in water,
// reeling), since the ORIGINAL flatscreen C-stick controls this is meant
// to restore aren't limited to the hook-in-water window: rod_substick_x/y
// (d_a_mg_rod.cpp) also drive cast pull-back/power (lines ~1243-1272) and
// lure/rod-tip steering (lines ~3773-3780), both usable before a fish ever
// bites. Simple existence check, same fopAcM_SearchByName(fpcNm_MG_ROD_e)
// pattern already used by isFishingHookInWater()/
// refreshTrackedFishingRodMtxLive() -- no new plumbing needed.
inline bool isFishingRodActive() {
    return fopAcM_SearchByName(fpcNm_MG_ROD_e) != nullptr;
}

// VR fix (2026-08-12): the clawshot's hand-grip tracking -- deliberately
// scoped to JUST the two grip models (mHeldItemModel/getHookshotSecondaryModel()),
// same "simple joint attach" category already fixed for every other held
// item. The CHAIN itself (the procedural rope-of-links visual,
// hsChainShape_c::draw()) is a genuinely separate, raw-immediate-mode GX
// system with its own pre-existing flatscreen interpolation
// (mHsChainInterp*) that doesn't go through J3DModel/frame_interp at all --
// NOT attempted this round, matching section 16's original scoping
// decision to exclude hookshot as "not a simple joint attach." The grip
// models alone are what makes it LOOK like the player is holding the
// clawshot; the chain's own root end already reads mHeldItemRootPos/
// field_0x3810 (now tracked, since applyTrackedHookshotGripTransforms()
// computes them from the tracked grip matrices) at whatever rate
// setHookshotPos() itself runs -- so the chain's near end should visually
// follow the grip even without touching its rendering directly, just not
// at real frame rate yet (same "one level downstream" gap the boomerang/
// rod fixes each ran into with their OWN models).
//
// Gated on daPy_py_c::checkHookshotItem(getEquipItem()) -- the exact same
// condition setItemMatrix() itself already uses to decide whether to call
// setHookshotPos() at all (d_a_alink.cpp), checked directly rather than
// assumed.
//
// Gated on shouldTrackHookshotToHand(), NOT plain isFirstPerson() --
// 2026-08-19 follow-up, see that function's own comment for why the two
// need to be decoupled specifically for hookshot while Third Person is on.
inline void refreshTrackedHookshotMtxLive() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link || !shouldTrackHookshotToHand(link)) return;
    if (!daPy_py_c::checkHookshotItem(link->getEquipItem())) return;

    link->applyTrackedHookshotGripTransforms();

    if (J3DModel* m = link->getHeldItemModel()) {
        m->calc();
        markModelJointsLive(m);
    }
    if (J3DModel* m = link->getHookshotSecondaryModel()) {
        m->calc();
        markModelJointsLive(m);
    }

    // Follow-up (2026-08-12): the secondary tip resting on the (now
    // tracked) secondary grip -- see
    // applyTrackedHookshotTipRestingTransform()'s own comment for why
    // only this one (of the two tips) is covered here.
    link->applyTrackedHookshotTipRestingTransform();
    if (J3DModel* m = link->getHookshotSecondaryTipModel()) {
        m->calc();
        markModelJointsLive(m);
    }

    // 2nd follow-up (2026-08-12): the primary tip's resting/READY
    // transform turned out to be the same safe shape as the secondary
    // tip's above -- see applyTrackedHookshotPrimaryTipRestingTransform()'s
    // own comment.
    link->applyTrackedHookshotPrimaryTipRestingTransform();
    if (J3DModel* m = link->getHookshotPrimaryTipModel()) {
        m->calc();
        markModelJointsLive(m);
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
// has executed even once), immediately overwritten by the real calibration
// on the very first activation.
//
// CORRECTED 2026-08-15 (see kCoreAnchorHeightOffsetPlausibleMin/Max's own
// comment below for the full story): this used to be 55.75, copied
// verbatim from setBodyPartPos()'s `localEyeFromRoot = {0, 55.75, 15}` --
// but that constant is a LOCAL offset run through
// `mDoMtx_stack_c::multVec()` against the model's own joint-matrix stack
// (rotation/scale included, and very possibly a different root reference
// than current.pos), not a plain world-space Y delta against current.pos.y
// the way this file's `realEye.y - current.pos.y` is. The two were never
// actually the same quantity -- a real in-headset capture (see below)
// showed the genuine, stable value here is ~157-158, not ~56. Updated to
// match the real measured value instead of the mismatched borrowed one.
inline constexpr float kCoreAnchorHeightOffsetDefault = 158.0f;
inline float s_coreAnchorHeightOffset = kCoreAnchorHeightOffsetDefault;
inline bool s_coreAnchorCalibrated = false;

// ADDED 2026-08-13 (user report: "camera going above or below Link after
// certain loads or cutscenes" -- dismounting Epona puts the camera on the
// ground, exiting a shop puts it underground, mounting Epona puts it too
// high). Root-caused to this calibrate-once-and-hold-forever design being
// vulnerable to capturing a bad sample on the very frame first-person
// reactivates -- exactly the frame most likely to coincide with a scene
// load (door/shop transition: current.pos and getSubjectEyePos() may not
// both reflect the new area yet) or, per computeRawEyeAnchor()'s own mount
// carve-out below, a state where current.pos's relationship to eye height
// is genuinely different than the standing case this offset assumes.
//
// Two independent defenses, both cheap and matching this project's own
// established "reject an implausible sample rather than trust it blindly"
// pattern (see vr_swing_detector.hpp's maxPlausibleSpeed, added for the
// analogous reason -- a single implausible sample that would otherwise get
// baked in and held for the rest of a session/swing):
// (1) require at least one MORE real sim tick to run after first-person
//     reactivates before trusting a sample (s_coreAnchorActivationTickKnown/
//     s_coreAnchorActivationSimTick below) -- gives current.pos/
//     getSubjectEyePos() a chance to both settle to the same, new area
//     instead of calibrating off whatever the exact reactivation frame
//     happened to catch mid-transition;
// (2) reject a candidate offset outside a generous plausible range for a
//     standing/crouching human-form eye height (kCoreAnchorHeightOffsetDefault
//     = 55.75 is the expected common case) and keep retrying on subsequent
//     ticks instead, up to a bounded attempt count so a genuinely
//     out-of-range case (unexpected rig/scale) doesn't retry forever stuck
//     on the placeholder default -- past that many attempts, accept
//     whatever the latest sample is rather than never calibrating at all.
//
// WIDENED 2026-08-14 (user report: "the camera still sometimes goes on the
// ground in ordon village", specifically walking INTO the village -- a
// room-transition event, not a shop door or a mount/dismount, and the
// camera stayed stuck rather than self-correcting, matching this same
// calibrate-once-and-hold-forever failure shape). A single plausible
// sample one tick after reactivation was enough for a shop doorway
// (small, fast transition) but is not a strong enough defense for a
// bigger/slower one like entering a village -- current.pos and
// getSubjectEyePos() can each individually land inside the 20-100
// plausible band while still disagreeing with each other, e.g. mid-load
// with current.pos already at the new area's spawn point but the animated
// eye joint still one or two ticks behind (or vice versa), by coincidence
// producing a "plausible" but wrong gap that then gets held for the rest
// of the session -- exactly the "next lever" this fix's own 2026-08-13
// comment already flagged: require several CONSECUTIVE plausible samples,
// not just one, before trusting the transition has actually settled.
inline bool s_coreAnchorActivationTickKnown = false;
inline uint64_t s_coreAnchorActivationSimTick = 0;
inline uint64_t s_coreAnchorLastAttemptSimTick = 0;
inline int s_coreAnchorCalibrationAttempts = 0;
inline int s_coreAnchorConsecutivePlausible = 0;
inline float s_coreAnchorLastPlausibleCandidate = 0.0f;
// CORRECTED 2026-08-15 (user report: ground bug "took about 5 seconds to
// fix", every time -- not intermittent). A real [dusk::vr::coreanchor]
// capture found the actual bug: this 20-100 band was based on
// kCoreAnchorHeightOffsetDefault's OLD, mismatched-reference 55.75 value
// (see that constant's own corrected comment) -- the real, legitimate,
// rock-stable candidate in the capture was ~157-158 the entire time
// (drifting under 1 unit across 150 consecutive attempts, i.e. genuinely
// settled data, not noise), which this old band rejected on literally
// EVERY attempt (`plausible=0` for all 150), so it could never reach
// `settled` and always fell through to the MAX_ATTEMPTS_FALLBACK -- not
// because settling was slow, but because the correct value was never
// even eligible to be accepted. This is why the "5 second" wait was
// consistent/every-time rather than occasional: it wasn't really a
// timing race being sometimes won and sometimes lost, it was hitting the
// same guaranteed-to-fail check every single time and just waiting out
// the clock. Raised the ceiling well above the real measured value
// (158) while keeping real garbage samples seen in the same capture
// (299, 18687.98 -- both from transient pre-settle frames, already
// harmless since they're rejected either way) safely excluded.
inline constexpr float kCoreAnchorHeightOffsetPlausibleMin = 80.0f;
inline constexpr float kCoreAnchorHeightOffsetPlausibleMax = 220.0f;
// How much a new plausible sample is allowed to differ from the previous
// plausible one and still count as part of the same consecutive run --
// distinguishes "still settling, jumping around within the plausible
// band" from "genuinely steady." 5 units (~2in) is comfortably tighter
// than ordinary per-tick head/root jitter while standing still, but loose
// enough not to reject legitimate small posture drift between ticks.
inline constexpr float kCoreAnchorConsecutiveTolerance = 5.0f;
inline constexpr int kCoreAnchorRequiredConsecutivePlausible = 3;
// WIDENED 2026-08-15 (user report: ground bug "stays stuck, especially if
// you move too much or trigger first person as soon as it loads, but
// other times it corrects itself if you stand completely still"). At the
// old cap (30, ~1s), moving right after a load kept breaking the
// consecutive-plausible streak until the cap forced a commit to whatever
// the LAST sample happened to be -- possibly taken mid-stride/mid-slope,
// i.e. exactly the wrong kind of sample to lock in for the rest of the
// session. Standing still let it converge before hitting the cap, which
// is the "self-corrects" case. While NOT yet calibrated,
// s_coreAnchorHeightOffset keeps whatever its previous value was (the
// last known-good calibration, or the plausible default on a session's
// very first activation) -- so waiting longer for a real 3-consistent-
// sample settle costs nothing but time; there's no reason to rush a
// forced accept. 150 (~5s) gives a load that's still settling, or a
// player who doesn't stand still immediately, real room to converge
// properly before the old escape hatch would have kicked in.
inline constexpr int kCoreAnchorCalibrationMaxAttempts = 150;  // ~5s at 30Hz sim tick

// ADDED 2026-08-15 (same report, other half: "trigger first person as
// soon as it loads"). The isFirstPerson()-false reset in
// getVrCameraEyeAnchor() can only re-arm calibration around a
// cutscene/Wolf-form transition -- it has no way to notice a load/warp
// that never dips isFirstPerson() false at all (e.g. loading a save
// while already standing in first-person gameplay, or an Owl Statue/
// overworld warp with no event wrapper this file's isFirstPerson()
// recognizes) -- so the pre-load offset just gets reused verbatim at the
// new location with zero re-validation. Rather than hunt for and hook
// every possible load/warp call site individually, detect the ONE thing
// they all have in common from here: current.pos jumping by an amount no
// real per-tick movement (walking, running, even galloping on Epona) can
// produce. kCoreAnchorTeleportDistanceUnits is picked well above the
// fastest real in-game per-tick displacement (Epona at full gallop is
// still well under 100 units/tick at this project's ~100-units/metre
// scale) so it can't misfire on ordinary fast movement.
inline cXyz s_coreAnchorLastTickPos{0.0f, 0.0f, 0.0f};
inline bool s_coreAnchorLastTickPosValid = false;
inline uint64_t s_coreAnchorLastTickPosSimTick = 0;
inline constexpr float kCoreAnchorTeleportDistanceUnits = 300.0f;  // ~3m/tick

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

    // Teleport/warp detection -- see kCoreAnchorTeleportDistanceUnits' own
    // comment above for why this exists. Gated on a real new sim tick
    // (current.pos only actually changes once per tick) so this can't
    // misfire by comparing two reads of the same tick's position.
    {
        const uint64_t posSimTick = dusk::frame_interp::sim_tick_seq();
        if (!s_coreAnchorLastTickPosValid) {
            s_coreAnchorLastTickPos = link->current.pos;
            s_coreAnchorLastTickPosValid = true;
            s_coreAnchorLastTickPosSimTick = posSimTick;
        } else if (posSimTick != s_coreAnchorLastTickPosSimTick) {
            const float dx = link->current.pos.x - s_coreAnchorLastTickPos.x;
            const float dy = link->current.pos.y - s_coreAnchorLastTickPos.y;
            const float dz = link->current.pos.z - s_coreAnchorLastTickPos.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > kCoreAnchorTeleportDistanceUnits * kCoreAnchorTeleportDistanceUnits) {
                // A jump this big in one tick can't be real movement --
                // force a fresh calibration, same reset shape as
                // getVrCameraEyeAnchor()'s own isFirstPerson()-false case.
                s_coreAnchorCalibrated = false;
                s_coreAnchorActivationTickKnown = false;
                s_coreAnchorCalibrationAttempts = 0;
                s_coreAnchorConsecutivePlausible = 0;
                // TEMP DIAGNOSTIC 2026-08-15 -- remove once the "5s to
                // self-correct" report is root-caused. See kCoreAnchor*
                // constants' own comments for context.
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "[dusk::vr::coreanchor] TELEPORT DETECTED dist=%.1f "
                              "(threshold=%.1f) -- forcing recalibration\n",
                              std::sqrt(distSq), kCoreAnchorTeleportDistanceUnits);
                OutputDebugStringA(buf);
            }
            s_coreAnchorLastTickPos = link->current.pos;
            s_coreAnchorLastTickPosSimTick = posSimTick;
        }
    }

    if (!s_coreAnchorCalibrated) {
        const uint64_t simTick = dusk::frame_interp::sim_tick_seq();
        if (!s_coreAnchorActivationTickKnown) {
            // First frame this activation has been observed -- don't
            // calibrate off it directly, just remember which tick it was.
            // See this block's own header comment (above
            // kCoreAnchorHeightOffsetDefault) for why.
            s_coreAnchorActivationSimTick = simTick;
            s_coreAnchorActivationTickKnown = true;
        } else if (simTick != s_coreAnchorActivationSimTick &&
                   simTick != s_coreAnchorLastAttemptSimTick) {
            // A real sim tick has run since activation (and since our last
            // attempt, if any) -- current.pos/getSubjectEyePos() have had a
            // chance to settle post-transition. Try calibrating from it.
            s_coreAnchorLastAttemptSimTick = simTick;
            ++s_coreAnchorCalibrationAttempts;
            const float candidate = realEye.y - link->current.pos.y;
            const bool plausible = candidate >= kCoreAnchorHeightOffsetPlausibleMin &&
                                    candidate <= kCoreAnchorHeightOffsetPlausibleMax;
            const bool consistentWithLastPlausible =
                s_coreAnchorConsecutivePlausible == 0 ||
                std::fabs(candidate - s_coreAnchorLastPlausibleCandidate) <=
                    kCoreAnchorConsecutiveTolerance;
            if (plausible && consistentWithLastPlausible) {
                ++s_coreAnchorConsecutivePlausible;
                s_coreAnchorLastPlausibleCandidate = candidate;
            } else {
                // Either outside the plausible band, or plausible but a
                // meaningfully different value than the last plausible
                // sample (still settling) -- restart the consecutive-run
                // count from this sample rather than from zero, so a
                // transition that's still moving but always lands
                // "plausible" doesn't get stuck never accumulating a run.
                s_coreAnchorConsecutivePlausible = plausible ? 1 : 0;
                s_coreAnchorLastPlausibleCandidate = candidate;
            }
            const bool settled =
                s_coreAnchorConsecutivePlausible >= kCoreAnchorRequiredConsecutivePlausible;
            const bool forcedByMaxAttempts =
                !settled && s_coreAnchorCalibrationAttempts >= kCoreAnchorCalibrationMaxAttempts;

            // TEMP DIAGNOSTIC 2026-08-15 -- remove once the "5s to
            // self-correct" report is root-caused. Logs every attempt (not
            // throttled) since a single activation caps at
            // kCoreAnchorCalibrationMaxAttempts (150) attempts -- fine for
            // a short-lived capture.
            {
                char buf[256];
                std::snprintf(
                    buf, sizeof(buf),
                    "[dusk::vr::coreanchor] attempt=%d candidate=%.2f plausible=%d "
                    "consecutivePlausible=%d realEye.y=%.2f current.pos.y=%.2f\n",
                    s_coreAnchorCalibrationAttempts, candidate, plausible ? 1 : 0,
                    s_coreAnchorConsecutivePlausible, realEye.y, link->current.pos.y);
                OutputDebugStringA(buf);
            }

            if (settled || forcedByMaxAttempts) {
                s_coreAnchorHeightOffset = candidate;
                s_coreAnchorCalibrated = true;
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "[dusk::vr::coreanchor] COMMITTED offset=%.2f via=%s "
                              "afterAttempts=%d\n",
                              candidate, settled ? "SETTLED" : "MAX_ATTEMPTS_FALLBACK",
                              s_coreAnchorCalibrationAttempts);
                OutputDebugStringA(buf);
            }
        }
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
//
// HOOKSHOT/CLAWSHOT (user request 2026-08-13, "make the camera first
// person when you are in the air being pulled by the clawshot, and when
// you are hanging on to a clawshot target"): same reasoning again --
// isFirstPerson() already permits first-person here (hookshot flight/
// hanging isn't a dEvt_control_c event, so it falls straight through
// isFirstPerson()'s "no event -- ordinary gameplay" branch), so this was
// never actually a first/third-person GATING problem. It's the same
// comfort-anchor-calibration gap as swimming/crawling/vine-climbing:
// the core anchor's standing-height offset doesn't mean anything while
// Link is horizontal mid-air on the end of a chain, or hanging off a
// wall/ceiling at an odd angle. See isHookshotAirborneOrHanging()'s own
// comment for exactly which PROC states this covers.
//
// MOUNTED (horse/canoe/board) GAMEPLAY (fix 2026-08-13, user report:
// "the camera going above or below Link after certain loads or cutscenes
// ... get on epona and it will be too high"): same underlying gap as
// swimming/crawling/vine-climbing/hookshot above, just never previously
// added to this list -- section 11/23's own notes explicitly flagged
// mounted gameplay as "expected to work with zero additional code" but
// NEVER actually confirmed in-headset. It doesn't hold up: setBodyPartPos()
// (d_a_alink.cpp) computes the animated eye position for these three modes
// via dedicated horseLocalEyeFromRoot/canoeLocalEyeFromRoot/
// boardLocalEyeFromRoot offsets applied through a matrix stack rooted at
// field_0x3834 (getRootPosP(), the model's own joint-0 world position) --
// a structurally different derivation than plain standing, not just a
// different constant. The core anchor's calibrated s_coreAnchorHeightOffset
// is captured once (almost always while standing, since that's the far
// more common state to first enter first-person in) and held fixed
// forever, so applying it on top of current.pos.y while mounted produces a
// height that means something different than what it was calibrated
// for -- explaining "too high" while riding. Falling back to the same
// already-mount-aware getSubjectEyePos() used for cutscenes/dialogue above
// (which is what this whole codepath used before section 23 introduced the
// core anchor) fixes this by construction, the same way it already did for
// swim/crawl/vine/hookshot. This ALSO explains "get off epona and the
// camera will be on the ground" even without touching the core-anchor
// domain: since mounting/dismounting during ordinary gameplay never runs a
// dEvt_control_c event (checkEventRun() stays false the whole time, per
// isFirstPerson()'s own comment on this exact point), nothing was ever
// resetting/recalibrating s_coreAnchorHeightOffset around a mount/dismount
// either -- if a player's FIRST first-person activation in a session
// happened to land while already mounted (e.g. loading a save that starts
// on horseback, or a cutscene ending mid-ride), the one-shot calibration
// would capture the MOUNTED relationship between current.pos.y and eye
// height instead of the standing one, and that wrong-for-standing offset
// would then persist after dismounting too, until the next
// isFirstPerson() false->true transition (a later cutscene/dialogue).
// Excluding mounted gameplay from this domain entirely removes that whole
// failure mode, not just the in-the-moment "too high while riding" case.
//
// HORSE (Epona) BACKWARD/UP NUDGE (fix 2026-08-14, user report, after
// confirming the height fix above: "the camera phases through her head
// and it's way too far ahead of link's body"). getSubjectEyePos()'s
// horseLocalEyeFromRoot offset ({1.75, 55.0, 25.5}, setBodyPartPos(),
// d_a_alink.cpp) was authored for the flatscreen third-person camera's own
// needs -- a first-person VR headset sitting directly at that same point
// sits far enough forward (and, per a same-day follow-up report, too low)
// to clip through Epona's own head/neck geometry. Pulls the anchor BACK
// along Link's body-facing direction (current.angle.y, the same field/
// convention computeRawCoreAnchoredEye() already uses for its own
// forward-offset nudge, just negated here) and UP, both by fixed
// real-world distances. Scoped to horse riding only (checkReinRide()) --
// canoe/board weren't reported and use their own separate offsets
// (canoeLocalEyeFromRoot/boardLocalEyeFromRoot); don't assume they have
// the same problem without separate confirmation.
inline constexpr float kHorseCameraBackUnits = 30.48f;  // 1 real foot (100 units/metre, see kCoreAnchorExtraForwardUnits's own comment)
inline constexpr float kHorseCameraUpUnits = 15.24f;    // 6 real inches, same conversion

inline cXyz computeRawEyeAnchor(daAlink_c* link) {
    if (link->checkReinRide()) {
        cXyz eye = *link->getSubjectEyePos();
        const float yawRad = static_cast<float>(link->current.angle.y) * (3.14159265f / 32768.0f);
        eye.x -= kHorseCameraBackUnits * std::sin(yawRad);
        eye.z -= kHorseCameraBackUnits * std::cos(yawRad);
        eye.y += kHorseCameraUpUnits;
        return eye;
    }
    if (link->checkModeFlg(daAlink_c::MODE_SWIMMING | daAlink_c::MODE_VINE_CLIMB) ||
        isCrawling(link) || isHookshotAirborneOrHanging(link) || isMagnetized(link) ||
        link->checkWaterInMove() || link->checkCanoeRide() || link->checkBoardRide())
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
        // own comment above. Also reset the delayed/plausibility-gated
        // calibration state (2026-08-13 fix, same comment) so the NEXT
        // activation gets its own fresh one-tick settle window and attempt
        // budget instead of inheriting whatever this one left behind.
        detail::s_coreAnchorCalibrated = false;
        detail::s_coreAnchorActivationTickKnown = false;
        detail::s_coreAnchorCalibrationAttempts = 0;
        detail::s_coreAnchorConsecutivePlausible = 0;
        detail::s_coreAnchorLastTickPosValid = false;
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
