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

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_view.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphAnimator/J3DShapeTable.h"

#include <cmath>
#include <cstring>

namespace vr_link {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Game units per metre of physical controller movement.
// TP's Link is ~170 game units tall (~1.7 m real), giving ~100 u/m.
inline constexpr float VR_SCALE_FACTOR = 100.0f;

// Root/wrist joint index in mpLinkHandModel.
inline constexpr int HAND_ROOT_JOINT = 0;

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
inline void buildHandMtx(
    Mtx               dest,
    const XrVector3f& hmdPosXR,
    const XrPosef&    controllerPoseXR,
    const cXyz&       linkEyeGame,
    float             scale)
{
    // Offset of controller from HMD in tracking space (metres)
    const float dx = controllerPoseXR.position.x - hmdPosXR.x;
    const float dy = controllerPoseXR.position.y - hmdPosXR.y;
    const float dz = controllerPoseXR.position.z - hmdPosXR.z;

    // Scale and flip Z (OpenXR Z-back → TP game Z-forward)
    const float gx = linkEyeGame.x + dx * scale;
    const float gy = linkEyeGame.y + dy * scale;
    const float gz = linkEyeGame.z - dz * scale;

    // Orientation: flip Z component of quaternion to match axis flip
    const auto& q = controllerPoseXR.orientation;
    const float qx = q.x, qy = q.y, qz = -q.z, qw = q.w;

    const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    const float wx = qw*qx, wy = qw*qy, wz = qw*qz;

    dest[0][0] = 1.f - 2.f*(yy+zz); dest[0][1] = 2.f*(xy+wz);        dest[0][2] = 2.f*(xz-wy);        dest[0][3] = gx;
    dest[1][0] = 2.f*(xy-wz);        dest[1][1] = 1.f - 2.f*(xx+zz); dest[1][2] = 2.f*(yz+wx);        dest[1][3] = gy;
    dest[2][0] = 2.f*(xz+wy);        dest[2][1] = 2.f*(yz-wx);        dest[2][2] = 1.f - 2.f*(xx+yy); dest[2][3] = gz;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct FrameInput {
    XrPosef hmdPose;
    XrPosef rightControllerPose;
    XrPosef leftControllerPose;
};

inline void updateFrame(const FrameInput& input) {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;

    // Hide face and hat — these are separate J3DModel objects so hiding
    // their shape tables has no effect on the body or hand models.
    hideModel(link->mpLinkFaceModel);
    hideModel(link->mpLinkHatModel);

    // Map right hand to controller
    view_class* view = dComIfGd_getView();
    if (!view) return;

    const cXyz& eyePos  = view->lookat.eye;
    const XrVector3f& hmdPos = input.hmdPose.position;

    if (link->mpLinkHandModel) {
        Mtx handMtx;
        buildHandMtx(handMtx, hmdPos, input.rightControllerPose,
                     eyePos, VR_SCALE_FACTOR);
        link->mpLinkHandModel->setAnmMtx(HAND_ROOT_JOINT, handMtx);
    }

    // Left hand / shield bone — TODO: identify joint index from body .bmd
    // Mtx leftMtx;
    // buildHandMtx(leftMtx, hmdPos, input.leftControllerPose,
    //              eyePos, VR_SCALE_FACTOR);
    // link->mpLinkModel->setAnmMtx(LEFT_HAND_JOINT_IDX, leftMtx);
}

inline void restoreVisibility() {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer());
    if (!link) return;
    showModel(link->mpLinkFaceModel);
    showModel(link->mpLinkHatModel);
}

} // namespace vr_link
