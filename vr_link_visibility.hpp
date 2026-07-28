// vr_link_visibility.hpp
//
// Per-frame VR visibility management for Link's model parts, plus
// controller-to-game-world hand matrix mapping.
//
// Call vr_link::updateFrame() once per rendered eye, after xrLocateSpaces
// has been called and before aurora::gfx::create_pass opens the eye pass.
//
// What this does each frame:
//   1. Hides mpLinkFaceModel and mpLinkHatModel (J3DModel::hide())
//      These are separate J3DModel* objects on daAlink_c, so hiding them
//      is a single flag write with no side effects on the body mesh.
//   2. Leaves mpLinkModel (body) visible. Arms are part of this mesh.
//      See note below on whether you need to hide them.
//   3. Maps mpLinkHandModel's root joint matrix to the controller's
//      6DOF pose in game-world space, so the hand and whatever weapon
//      Link is holding tracks the physical controller.
//
// --- Coordinate space mapping ---
//
// OpenXR tracking space: origin = HMD start position, metres, right-handed
//                        X=right, Y=up, Z=back
// TP game world:         arbitrary world origin, game units (~100 per metre)
//                        X=right, Y=up, Z=forward (left-handed, like D3D)
//
// Bridging them uses the HMD pose (from xrLocateViews) as the "anchor":
//   - In tracking space, HMD is at hmdPos (metres)
//   - In game space,    HMD maps to view->lookat.eye (game units)
//   - So the controller's offset from HMD in tracking space, scaled
//     and axis-flipped, gives its offset from Link's eye in game space.
//
// Axis flip: OpenXR Z-back → game Z-forward means negate Z.
// Scale:     VR_SCALE_FACTOR below (~100.0f). Tune this if hands feel
//            too close or too far from the body — it's the only fudge factor.
//
// --- Arm visibility note ---
//
// Arms are part of mpLinkModel (same mesh as the body) not a separate
// model, so hiding them requires per-shape indexing via J3DShapeTable.
// RECOMMENDED: test first with only the head/hat hidden. In first-person
// at eye height the arms may fall below the frustum entirely during
// normal play and never be visible. Only do shape-level hiding if you
// actually see arms clipping into view during playtesting.
// If you do need it, the shape indices can be found by opening
// Link's body .bmd in j3dview/bmdview and cross-referencing
// mpLinkModel->getModelData()->getShapeNodePointer(i)->... with the
// name table: mpLinkModel->getModelData()->mShapeTable.mShapeName

#pragma once

#include <openxr/openxr.h>

#include "d/actor/d_a_alink.h"        // daAlink_c, mpLinkHandModel, etc.
#include "d/d_com_inf_game.h"          // dComIfGp_getLinkPlayer(), dComIfGd_getView()
#include "f_op/f_op_view.h"            // view_class, lookat_class
#include "JSystem/J3DGraphAnimator/J3DModel.h"  // J3DModel::hide(), setAnmMtx()

#include <cmath>
#include <cstring>

namespace vr_link {

// ---------------------------------------------------------------------------
// Tunable constants
// ---------------------------------------------------------------------------

// Game units per metre of physical controller movement.
// TP's Link is ~170 game units tall (~1.7m real), giving ~100 u/m.
// Adjust if controller movement feels too large or too small in-game.
inline constexpr float VR_SCALE_FACTOR = 100.0f;

// Joint index for the root/wrist joint in mpLinkHandModel.
// Index 0 is almost always the root joint in J3D models; confirm by
// inspecting the hand .bmd if the hand appears in the wrong pose.
inline constexpr int HAND_ROOT_JOINT = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Convert an XrQuaternionf + XrVector3f controller pose into a Nintendo
// Mtx (3 rows × 4 cols, row-major) in game-world space, anchored so that
// the controller's offset from the HMD maps to an offset from Link's eye.
//
//   hmdPosXR       : HMD position in tracking space (metres)
//   controllerPoseXR: controller grip pose in tracking space
//   linkEyeGame    : view->lookat.eye (game units)
//   scale          : tracking metres → game units
inline void buildHandMtx(
    Mtx                dest,
    const XrVector3f&  hmdPosXR,
    const XrPosef&     controllerPoseXR,
    const cXyz&        linkEyeGame,
    float              scale)
{
    // --- Position ---
    // Offset of controller from HMD in tracking space (metres)
    const float dx = controllerPoseXR.position.x - hmdPosXR.x;
    const float dy = controllerPoseXR.position.y - hmdPosXR.y;
    const float dz = controllerPoseXR.position.z - hmdPosXR.z;

    // Apply scale and flip Z: OpenXR Z-back → TP game Z-forward
    const float gx = linkEyeGame.x + dx * scale;
    const float gy = linkEyeGame.y + dy * scale;
    const float gz = linkEyeGame.z - dz * scale;  // negated Z

    // --- Orientation ---
    // Controller orientation quaternion, then flip handedness on Z component
    // to match the axis flip above. This keeps the rotation consistent with
    // the translated position.
    const auto& q = controllerPoseXR.orientation;
    const float qx = q.x, qy = q.y, qz = -q.z, qw = q.w;

    const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    const float wx = qw*qx, wy = qw*qy, wz = qw*qz;

    // Row 0: local X axis
    dest[0][0] = 1.f - 2.f*(yy+zz);
    dest[0][1] = 2.f*(xy+wz);
    dest[0][2] = 2.f*(xz-wy);
    dest[0][3] = gx;

    // Row 1: local Y axis
    dest[1][0] = 2.f*(xy-wz);
    dest[1][1] = 1.f - 2.f*(xx+zz);
    dest[1][2] = 2.f*(yz+wx);
    dest[1][3] = gy;

    // Row 2: local Z axis
    dest[2][0] = 2.f*(xz+wy);
    dest[2][1] = 2.f*(yz-wx);
    dest[2][2] = 1.f - 2.f*(xx+yy);
    dest[2][3] = gz;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct FrameInput {
    // From xrLocateViews — the centre/combined HMD pose for this frame.
    // Use the view pose for whichever eye you're currently rendering; the
    // difference between left and right eye HMD positions is negligible
    // for hand anchoring (it's ~3cm of IPD offset). Using one consistent
    // value per frame avoids the hands shifting slightly between eyes.
    XrPosef hmdPose;

    // From xrLocateSpace on the right-hand and left-hand grip action spaces.
    // If a controller isn't tracked (locationFlags missing
    // XR_SPACE_LOCATION_POSITION_VALID_BIT), pass the last known valid pose.
    XrPosef rightControllerPose;
    XrPosef leftControllerPose;
};

// Call once per frame (not per eye — hand positions don't change between
// the left and right eye renders, and setting them twice is harmless but
// wasteful). Best called right after xrLocateSpaces, before create_pass.
inline void updateFrame(const FrameInput& input) {
    // --- Get Link's actor ---
    // dComIfGp_getLinkPlayer returns daPy_py_c*; daAlink_c inherits from it.
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (link == nullptr) {
        return; // Not in gameplay (title screen, loading, etc.)
    }

    // --- Hide head + hat ---
    // These are completely separate J3DModel objects, so hide() is a single
    // flag write with no effect on the body or hand models.
    if (link->mpLinkFaceModel != nullptr) {
        link->mpLinkFaceModel->hide();
    }
    if (link->mpLinkHatModel != nullptr) {
        link->mpLinkHatModel->hide();
    }
    // Note: mpLinkModel (body + arms) stays visible.
    // See the arm visibility note at the top of this file.

    // --- Map hands to controllers ---
    view_class* view = dComIfGd_getView();
    if (view == nullptr) {
        return;
    }

    const cXyz& eyePos = view->lookat.eye;
    const XrVector3f& hmdPos = input.hmdPose.position;

    // Right hand (primary sword hand in TP)
    if (link->mpLinkHandModel != nullptr) {
        Mtx handMtx;
        buildHandMtx(handMtx,
                     hmdPos, input.rightControllerPose,
                     eyePos, VR_SCALE_FACTOR);
        link->mpLinkHandModel->setAnmMtx(HAND_ROOT_JOINT, handMtx);
    }

    // Left hand (shield / off-hand)
    // TP doesn't have a separate left-hand model in the same way the right
    // hand does — the shield attaches to a bone on mpLinkModel.
    // TODO: once you've identified the shield attach joint index on
    // mpLinkModel (open the body .bmd and look for a joint named something
    // like "LeftHandEnd" or "ArmL_end"), override it the same way:
    //
    //   Mtx leftMtx;
    //   buildHandMtx(leftMtx,
    //                hmdPos, input.leftControllerPose,
    //                eyePos, VR_SCALE_FACTOR);
    //   link->mpLinkModel->setAnmMtx(LEFT_HAND_JOINT_IDX, leftMtx);
}

// Call this once when the player exits gameplay back to title/loading,
// to restore visibility so the models look correct if they're reused
// for cutscenes or the death screen.
inline void restoreVisibility() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (link == nullptr) {
        return;
    }
    if (link->mpLinkFaceModel != nullptr) {
        link->mpLinkFaceModel->show();
    }
    if (link->mpLinkHatModel != nullptr) {
        link->mpLinkHatModel->show();
    }
}

} // namespace vr_link
