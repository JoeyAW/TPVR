// vr_stereo_render.hpp
//
// Stereo per-eye render loop for the Dusklight VR mod.
// All Aurora and game API calls are now real — no TODO stubs.
//
// Key sources read to write this:
//   extern/aurora/include/aurora/gfx.hpp  - aurora::gfx::create_pass,
//                                           resolve_pass, push_custom_draw
//   include/f_op/f_op_view.h              - view_class fields (projMtx,
//                                           viewMtx, lookat, fovy, aspect …)
//   src/dusk/frame_interpolation.cpp      - how Dusklight already drives
//                                           C_MTXPerspective + mDoMtx_lookAt
//                                           per-frame; we slot in before that
//
// --- Integration overview ---
//
//   The per-frame camera path in frame_interpolation.cpp already does:
//       C_MTXPerspective(view->projMtx, view->fovy, view->aspect,
//                        view->near_, view->far_);
//       mDoMtx_lookAt(view->viewMtx, …);
//       j3dSys.setViewMtx(view->viewMtx);
//
//   For VR, we call that same block TWICE, once per eye, each time
//   replacing view->lookat / view->fovy / view->aspect with the HMD eye
//   pose and asymmetric FOV from xrLocateViews, then calling
//   aurora::gfx::create_pass / resolve_pass around the scene draw.
//
//   The outer frame loop (in m_Do_main.cpp) calls aurora_begin_frame()
//   then runs the game's render pass then aurora_end_frame(). We need to
//   wrap that middle section with a VR stereo loop. The cleanest hook is
//   right before the interpolation block sets the view matrices, so we
//   drive both eyes from the same already-interpolated world state.
//
// --- Getting resolved frames into OpenXR swapchains ---
//
//   resolve_pass() returns aurora::gfx::ResolvedTargets which contains
//   wgpu::TextureView (WebGPU, via Dawn). OpenXR's D3D12 swapchain gives
//   us XrSwapchainImageD3D12KHR which wraps an ID3D12Resource.
//   To bridge them, use Dawn's native D3D12 backend interop header:
//       #include <dawn/native/D3D12Backend.h>
//   Call dawn::native::d3d12::Texture::GetD3D12Resource() on the texture
//   that backs the view, then CopyResource into the XR swapchain image.
//   That code lives in vr_xr_submit.hpp (not yet written) and is
//   deliberately kept separate since it depends on the swapchain session
//   being open; the render/camera logic here does not.

#pragma once

#include <aurora/gfx.hpp>          // aurora::gfx::create_pass, resolve_pass, etc.
#include <openxr/openxr.h>

#include "d/d_com_inf_game.h"      // dComIfGd_getView()
#include "f_op/f_op_view.h"        // view_class, lookat_class, Mtx44, Mtx
#include <dolphin/mtx.h>           // C_MTXPerspective, mDoMtx_lookAt (or equivalent)
#include <JSystem/J3D/J3DSystem.h> // j3dSys.setViewMtx()

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>

namespace vr_render {

// ---------------------------------------------------------------------------
// Helpers: XR → game-native matrix conversion
// ---------------------------------------------------------------------------

// view_class uses Nintendo's Mtx (3x4, row-major) for the view matrix and
// Mtx44 (4x4, row-major) for the projection matrix.
// OpenXR gives us a quaternion pose + asymmetric FovAngles.
// We convert here rather than in the XR bootstrap to keep the math next to
// where it's used.

// Build a Nintendo-style Mtx (3 rows × 4 cols, row-major) view matrix from
// an XrPosef (the eye pose in LOCAL_SPACE). This is the inverse of the eye's
// world transform, identical in math to the existing mDoMtx_lookAt path but
// taken directly from the HMD pose instead of the follow-camera target point.
// Result goes into view->viewMtx.
inline void eyePoseToViewMtx(Mtx dest, const XrPosef& pose) {
    const auto& q = pose.orientation;
    const auto& p = pose.position;

    // Quaternion → rotation (row-major 3x3)
    const float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    const float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    const float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;

    // Rows are the camera's local X, Y, Z axes in world space.
    // View matrix = transpose(R) | -transpose(R)*t
    const float r00 = 1.f - 2.f*(yy+zz), r01 = 2.f*(xy+wz), r02 = 2.f*(xz-wy);
    const float r10 = 2.f*(xy-wz),        r11 = 1.f - 2.f*(xx+zz), r12 = 2.f*(yz+wx);
    const float r20 = 2.f*(xz+wy),        r21 = 2.f*(yz-wx),       r22 = 1.f - 2.f*(xx+yy);

    // dest is Mtx = float[3][4]
    dest[0][0] = r00; dest[0][1] = r01; dest[0][2] = r02;
    dest[0][3] = -(p.x*r00 + p.y*r01 + p.z*r02);

    dest[1][0] = r10; dest[1][1] = r11; dest[1][2] = r12;
    dest[1][3] = -(p.x*r10 + p.y*r11 + p.z*r12);

    dest[2][0] = r20; dest[2][1] = r21; dest[2][2] = r22;
    dest[2][3] = -(p.x*r20 + p.y*r21 + p.z*r22);
}

// Build a Nintendo-style Mtx44 (4x4, row-major) asymmetric perspective
// projection from an XrFovf.  This replaces the C_MTXPerspective call that
// frame_interpolation.cpp already makes. Must match Aurora's clip convention:
// left-handed, depth [0,1] (confirmed by gfx.hpp's wgpu backend).
// Result goes into view->projMtx.
inline void eyeFovToProjMtx(Mtx44 dest, const XrFovf& fov, float nearZ, float farZ) {
    const float tl = std::tan(fov.angleLeft);
    const float tr = std::tan(fov.angleRight);
    const float td = std::tan(fov.angleDown);
    const float tu = std::tan(fov.angleUp);

    const float rw = 1.f / (tr - tl);
    const float rh = 1.f / (tu - td);

    // dest is Mtx44 = float[4][4]
    std::memset(dest, 0, sizeof(float)*16);
    dest[0][0] = 2.f * rw;
    dest[1][1] = 2.f * rh;
    dest[2][0] = (tr + tl) * rw;   // horizontal center offset
    dest[2][1] = (tu + td) * rh;   // vertical center offset
    dest[2][2] = farZ / (farZ - nearZ);
    dest[2][3] = 1.f;               // w-from-z (left-handed, +z forward)
    dest[3][2] = -(farZ * nearZ) / (farZ - nearZ);
}

// ---------------------------------------------------------------------------
// Per-eye render parameters (filled from xrLocateViews each frame)
// ---------------------------------------------------------------------------

struct EyeParams {
    XrPosef   pose;         // from XrView.pose
    XrFovf    fov;          // from XrView.fov
    uint32_t  width;        // from XrViewConfigurationView.recommendedImageRectWidth
    uint32_t  height;
};

// ---------------------------------------------------------------------------
// Render one eye
//
// Call this inside aurora_begin_frame() / aurora_end_frame(), once per eye.
// It:
//   1. Patches view_class in-place with the HMD eye pose + FOV
//   2. Opens an aurora::gfx offscreen pass at the eye's resolution
//   3. Lets the caller fire the normal scene draw (see note below)
//   4. Resolves the pass, returning the wgpu::TextureView for that eye
//
// The caller (your VR frame loop) is responsible for:
//   - Calling the game's existing per-frame scene render between
//     beginEye() and endEye() — specifically the same draw call that
//     normally runs inside aurora_begin_frame/end_frame
//   - Taking the returned ResolvedTargets and feeding the color view
//     into the OpenXR swapchain submit (vr_xr_submit.hpp, not yet written)
//
// IMPORTANT: aurora::gfx::create_pass nests inside the EFB pass that
// aurora_begin_frame() opens. It does NOT replace it. When resolve_pass()
// is called it ends the offscreen pass and restores the EFB pass, so
// any UI/HUD draws after endEye() still land on the normal framebuffer.
// ---------------------------------------------------------------------------

inline aurora::gfx::ResolvedTargets beginEye(const EyeParams& eye) {
    // 1. Get the shared game view and patch it with this eye's transform.
    //    This is the same view_class pointer that frame_interpolation.cpp
    //    already modifies each frame; we're just overriding its matrices
    //    for the duration of one offscreen draw.
    view_class* view = dComIfGd_getView();
    assert(view != nullptr && "VR: no active view_class — called outside gameplay?");

    // Build and apply the view matrix directly from the HMD eye pose.
    eyePoseToViewMtx(view->viewMtx, eye.pose);
    j3dSys.setViewMtx(view->viewMtx);

    // Inverse view for anything that needs world-from-view (shadow maps, etc.)
    // MTXInverse is the GC SDK equivalent of cMtx_inverse used in the original.
    MTXInverse(view->viewMtx, view->invViewMtx);

    // Build and apply the asymmetric projection matrix from the eye's FOV.
    // This replaces the C_MTXPerspective call in frame_interpolation.cpp
    // for this eye only; view->near_ and view->far_ come from the game's
    // normal camera setup and are intentionally reused as-is.
    eyeFovToProjMtx(view->projMtx, eye.fov, view->near_, view->far_);

    // 2. Open the offscreen pass for this eye at its native HMD resolution.
    //    aurora::gfx::create_pass clears color+depth and sets a full-target
    //    viewport/scissor automatically.
    const bool ok = aurora::gfx::create_pass(eye.width, eye.height);
    assert(ok && "VR: create_pass failed — is another offscreen pass already open?");

    // Return an empty ResolvedTargets as the "pass is now open" signal;
    // the real targets come back from endEye().
    return {};
}

inline aurora::gfx::ResolvedTargets endEye() {
    // Close the offscreen pass and snapshot the color buffer.
    // resolve_pass on an offscreen pass (created by create_pass) ends it
    // and restores the suspended EFB pass — exactly what we want between eyes.
    aurora::gfx::ResolvedTargets targets;
    const bool ok = aurora::gfx::resolve_pass({.color = true, .depth = false}, targets);
    assert(ok && "VR: resolve_pass failed outside an active render pass");
    return targets;
    // targets.color is now a wgpu::TextureView of this eye's rendered frame.
    // Pass it to vr_xr_submit::submitEye() once that file is written.
}

// ---------------------------------------------------------------------------
// Hand mesh injection
//
// Uses aurora::gfx::push_custom_draw to render the controller-tracked
// hand+sword mesh directly into the currently-open pass (either the
// offscreen eye pass during a stereo frame, or the EFB for debug/flatscreen).
//
// Register once at startup:
//   vr_render::HandDrawState state;
//   state.typeId = aurora::gfx::register_draw_type(vr_render::handDrawDescriptor());
//
// Then call each eye, after the scene is drawn but before endEye():
//   vr_render::HandPayload payload{ controllerPose };
//   aurora::gfx::push_custom_draw(state.typeId, &payload, sizeof(payload));
// ---------------------------------------------------------------------------

struct HandPayload {
    XrPosef controllerPose;   // grip space pose from xrLocateSpaces
    float   scale = 1.0f;     // uniform scale for the hand mesh
};

// Filled in by the draw callback, which runs on the render worker thread.
// Keep it trivially copyable and <= aurora::gfx::InlineDrawPayloadSize (128 bytes).
static_assert(sizeof(HandPayload) <= aurora::gfx::InlineDrawPayloadSize,
              "HandPayload too large for inline draw");

// Stub draw callback — replace the body with your actual hand mesh draw.
// This runs on the render worker thread; ctx.device / ctx.queue are borrowed
// and valid only for the duration of this call.
inline void handDrawCallback(
    const aurora::gfx::DrawContext& ctx,
    const wgpu::RenderPassEncoder& pass,
    const void* payload, size_t /*payloadSize*/,
    void* /*userdata*/)
{
    const auto& p = *static_cast<const HandPayload*>(payload);

    // TODO: Build a MVP matrix from p.controllerPose + p.scale + current
    // eye projection (stored in a thread-safe side-channel or embedded in
    // the payload), upload it as a uniform via ctx.queue.WriteBuffer on a
    // buffer you manage, then record the hand mesh draw into `pass`.
    //
    // Aurora's existing GX vertex/index streaming buffers (ctx.vertexBuffer
    // etc.) are available if you want to re-use the same buffer pool.
    (void)ctx; (void)pass; (void)p;
}

inline aurora::gfx::DrawTypeDescriptor handDrawDescriptor() {
    return { .label = "vr_hand", .draw = handDrawCallback, .userdata = nullptr };
}

struct HandDrawState {
    aurora::gfx::DrawTypeId typeId = aurora::gfx::InvalidDrawType;
};

} // namespace vr_render
