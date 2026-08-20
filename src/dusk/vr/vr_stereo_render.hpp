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

// Lib-internal aurora header, same pattern vr_xr_submit.hpp already uses
// for extern/aurora/lib/webgpu/gpu.hpp -- aurora::rmlui::get_render_target()
// (RmlUi's own render target, wired up in step 4). Small/self-contained,
// safe to include directly (unlike gx.hpp below, deliberately NOT
// included wholesale -- see that comment).
#include "../../../extern/aurora/lib/rmlui.hpp"

// aurora::gx::ensure_external_copy_texture() lives in the lib-internal
// extern/aurora/lib/gx/gx.hpp -- deliberately NOT included wholesale here
// (that header pulls in GXState, the entire internal GX-emulation state
// struct, and is only ever included today from within extern/aurora's own
// GX backend translation units -- untested and unnecessarily risky to pull
// into this already-heavy game-side header). Forward-declared instead,
// matching its real signature exactly (gx.cpp/gx.hpp are the source of
// truth if this ever needs re-checking).
namespace aurora::gx {
wgpu::Texture ensure_external_copy_texture(const void* dest, uint32_t width, uint32_t height,
                                            GXTexFmt format) noexcept;
}

#include "dusk/vr/vr_smooth_turn.hpp"  // dusk::vr::rotateYawXr/rotateYawQuat
#include "d/d_com_inf_game.h"      // dComIfGd_getView()
#include "f_op/f_op_view.h"        // view_class, lookat_class, Mtx44, Mtx
#include "m_Do/m_Do_lib.h"         // mDoLib_clipper::setup()
#include "m_Do/m_Do_mtx.h"         // mDoMtx_multVec() -- drawAimCrosshair()
#include <dolphin/mtx.h>           // C_MTXPerspective, mDoMtx_lookAt (or equivalent)
#include <dolphin/gx.h>            // GXBegin/GXEnd/etc. -- drawHudBillboard()
#include <JSystem/J3DGraphBase/J3DSys.h> // j3dSys.setViewMtx()

#include <algorithm>
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

// Game units per metre of physical HMD/controller movement. Must match
// vr_link_visibility.hpp's vr_link::VR_SCALE_FACTOR (TP's Link is ~170 game
// units tall / ~1.7 m real) -- kept as a separate constant here rather than
// including that actor-visibility header (heavier dependency, unrelated
// concern) into this rendering-only header.
inline constexpr float kEyePosScale = 100.0f;

// ROOT-CAUSED this session (cutscene out-of-bounds / "no skybox, only edges
// render" culling investigation): this used to build the view matrix
// directly from `pose` -- the raw OpenXR LOCAL-space pose -- treating its
// `position` as an ABSOLUTE game-world position. LOCAL space's origin is an
// arbitrary point (wherever the headset was when the XR session started,
// see vr_xr_bootstrap.hpp's poseInReferenceSpace = identity), in metres,
// with zero relationship to where Link/the game camera actually is. During
// normal gameplay this could coincidentally look plausible if a level's
// local coordinate origin happened to sit near the player; during a
// cutscene with an author-placed camera potentially far from world origin,
// the eye was rendering from close to empty space -- exactly matching the
// reported "no skybox, scene only visible at the edges" symptom (mDoLib_clipper
// culling everything near that near-origin point correctly; the actual
// bug was the camera's fed-in position, not the culling math).
//
// Fixed the same way vr_link_visibility.hpp's buildHandMtx() already
// anchors controllers to game-world space: take the DELTA of this eye's
// pose from the head-center pose (hmdRefPos -- both in the same XR tracking
// space, so the arbitrary origin cancels out), scale metres -> game units,
// flip Z (OpenXR Z-back -> TP game Z-forward, same comment as buildHandMtx),
// and add that onto `linkEyeGame` (view->lookat.eye -- the game's own
// current camera eye position for this frame, correct whether that's normal
// follow-cam or an authored cutscene camera). Orientation still comes
// directly from the eye's absolute pose (Z-flipped to match), so full head
// look-around still works -- only position is anchored/scaled instead of
// used as a raw absolute coordinate.
// Result goes into view->viewMtx.
inline void eyePoseToViewMtx(
    Mtx               dest,
    const XrPosef&    pose,
    const XrVector3f& hmdRefPos,
    const cXyz&       linkEyeGame,
    float             scale = kEyePosScale,
    float             yawRad = 0.f)
{
    const auto& p = pose.position;

    // Offset of this eye from the head-center reference pose, in metres.
    float dx = p.x - hmdRefPos.x;
    float dy = p.y - hmdRefPos.y;
    float dz = p.z - hmdRefPos.z;

    // VR smooth-turn (vr_smooth_turn.hpp): rotate both the tracked offset
    // and the orientation quaternion by the same persistent yaw offset, in
    // OpenXR's own native tracking-space convention, BEFORE any of the
    // existing game-convention math below runs. Applying this first keeps
    // it fully orthogonal to the position/orientation handedness fix a few
    // lines down -- this operates purely within OpenXR's own coordinate
    // system and never interacts with that conversion.
    const XrVector3f rotatedOffset = dusk::vr::rotateYawXr(XrVector3f{dx, dy, dz}, yawRad);
    dx = rotatedOffset.x; dy = rotatedOffset.y; dz = rotatedOffset.z;
    const XrQuaternionf q = dusk::vr::rotateYawQuat(pose.orientation, yawRad);

    // Scale to game units and anchor to Link's actual game-world eye
    // position for this frame.
    //
    // ROOT-CAUSED 2026-08-05 (section 14, "eyes swap near 90 degree yaw"):
    // this used to flip Z here (`linkEyeGame.z - dz*scale`), with a
    // comment claiming that "matches buildHandMtx's convention" -- true
    // when this code was written, but buildHandMtx's OWN position formula
    // had that identical Z flip removed later the same day (see CLAUDE.md
    // section 12, "front/back mirrored... fixed by removing the earlier
    // 'flip Z'") after being confirmed wrong there. Nobody came back to
    // update this copy to match. Root cause of THIS bug is that stale,
    // already-proven-wrong flip, not the orientation math below (which a
    // same-day attempt at "fixing" turned out to reverse pitch/yaw
    // entirely -- see the removed F*R*F attempt in git history / the
    // dated note below; reverted once that regression was reported).
    //
    // Verified in a standalone script before reapplying here: with the Z
    // flip removed on THIS side, and the orientation matrix below left
    // completely alone (unflipped, as it always was and is now confirmed
    // it must stay), the identity Rt(q) * R(q) * local_offset ==
    // local_offset holds EXACTLY regardless of head yaw -- i.e. a point
    // fixed to the camera's physical right reads as view-space right at
    // every yaw angle tested, not approximately (as the rejected
    // orientation-side fix only managed) but exactly, since it reduces to
    // multiplying by a rotation matrix and its own transpose. This also
    // matches hand tracking's own confirmed-working fix in shape, not just
    // in outcome.
    const double wx_ = static_cast<double>(linkEyeGame.x) + static_cast<double>(dx) * scale;
    const double wy_ = static_cast<double>(linkEyeGame.y) + static_cast<double>(dy) * scale;
    const double wz_ = static_cast<double>(linkEyeGame.z) + static_cast<double>(dz) * scale;

    // Orientation: raw quaternion, UNFLIPPED. CONFIRMED (not just assumed)
    // this must stay this way: a 2026-08-05 attempt to also apply a
    // coordinate-handedness correction here (mirroring the Z flip that
    // used to be on the position above via F*R*F) was tried, built, and
    // reported by the user as reversing pitch/yaw entirely ("turning left
    // turns right, looking up looks down") -- confirmed by re-deriving the
    // matrix by hand afterward: F*R*F reverses the sign of the rotation
    // angle for any rotation that touches the Z axis (yaw, pitch), which
    // is a much worse regression than the narrow 90-degree-yaw stereo
    // symptom it was meant to fix. Reverted the same session. Do not
    // reattempt an orientation-side fix for this bug without a much
    // stronger reason than "matches the position side's flip" -- see the
    // fix actually applied above instead (position-side, not this).
    const float qx = q.x, qy = q.y, qz = q.z, qw = q.w;

    // Quaternion → rotation (row-major 3x3)
    const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
    const float xy = qx*qy, xz = qx*qz, yz = qy*qz;
    const float wxq = qw*qx, wyq = qw*qy, wzq = qw*qz;

    // Rows are the camera's local X, Y, Z axes in world space.
    // View matrix = transpose(R) | -transpose(R)*t
    const float r00 = 1.f - 2.f*(yy+zz), r01 = 2.f*(xy+wzq), r02 = 2.f*(xz-wyq);
    const float r10 = 2.f*(xy-wzq),        r11 = 1.f - 2.f*(xx+zz), r12 = 2.f*(yz+wxq);
    const float r20 = 2.f*(xz+wyq),        r21 = 2.f*(yz-wxq),       r22 = 1.f - 2.f*(xx+yy);

    // dest is Mtx = float[3][4]. wx_/wy_/wz_ are double (see comment
    // above); r00.. are float but promote to double in these dot products,
    // so the sum-then-negate happens in double precision and only rounds
    // to float on the final assignment below.
    dest[0][0] = r00; dest[0][1] = r01; dest[0][2] = r02;
    dest[0][3] = static_cast<float>(-(wx_*r00 + wy_*r01 + wz_*r02));

    dest[1][0] = r10; dest[1][1] = r11; dest[1][2] = r12;
    dest[1][3] = static_cast<float>(-(wx_*r10 + wy_*r11 + wz_*r12));

    dest[2][0] = r20; dest[2][1] = r21; dest[2][2] = r22;
    dest[2][3] = static_cast<float>(-(wx_*r20 + wy_*r21 + wz_*r22));
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
    // ROOT-CAUSED this session (crossed-stereo investigation, round 5 --
    // the actual fix, rounds 1-4 were all dead ends on this specific bug):
    // GXTransform.cpp's real GXSetProjection() -- the actual GX call that
    // submits this matrix to the GPU -- reads the off-center terms from
    // mtx[0][2] (x) and mtx[1][2] (y):
    //   projVec = {type, mtx[0][0], mtx[0][2], mtx[1][1], mtx[1][2], mtx[2][2], mtx[2][3]}
    // matching the real GameCube hardware's fixed 6-parameter projection
    // format (equivalent to the classic glFrustum-style matrix
    // [[A,0,C,0],[0,B,D,0],[0,0,E,F],[0,0,-1,0]] read as v' = M*v). This
    // function was instead writing the off-center terms to dest[2][0] and
    // dest[2][1] -- a row-vector-convention position that GXSetProjection
    // never reads. Nothing ever wrote dest[0][2]/dest[1][2], so the GPU
    // always received zero there: every eye rendered with a perfectly
    // centered (symmetric) frustum despite the correct asymmetric FOV
    // being used for the scale terms and separately declared to the XR
    // runtime for its own lens/distortion correction -- that mismatch
    // between "declared asymmetric FOV" and "actually-rendered symmetric
    // frustum" is what produced both the crossed convergence (confirmed via
    // photos through each lens) and the stretching. Re-derived against the
    // standard glFrustum formula (C = (r+l)/(r-l) with l=n*tl, r=n*tr,
    // which reduces to exactly (tr+tl)*rw -- the ORIGINAL sign, not negated;
    // the round-4 sign flip was based on a wrong row-vector assumption and
    // is reverted here alongside the position fix) to get the correct
    // values into the positions GXSetProjection actually reads.
    dest[0][2] = (tr + tl) * rw;  // horizontal center offset
    dest[1][2] = (tu + td) * rh;  // vertical center offset
    // ROOT-CAUSED this session ("pillars/geometry render through terrain
    // that should occlude them" -- screenshot showed a near grassy cliff
    // failing to hide bridge pillars that are geometrically behind it):
    // these three cells used to be dest[2][2]=farZ/(farZ-nearZ) (wrong sign
    // AND wrong term), dest[2][3]=1.f (a bare constant with NO dependence
    // on near/far at all), and dest[3][2]=-(farZ*nearZ)/(farZ-nearZ) (a
    // cell GXSetProjection never even reads -- row 3 is hardware-fixed to
    // [0,0,-1,0], confirmed by the real SDK below). None of that was
    // derived from anything -- compare against extern/aurora/lib/dolphin/mtx/
    // mtx44.c's real C_MTXPerspective(), the exact function every other
    // camera in the game (frame_interpolation.cpp included) calls for this:
    //   tmp = 1 / (f - n);
    //   m[2][2] = (-n * tmp);
    //   m[2][3] = (tmp * -(f * n));
    //   m[3][2] = -1;
    // Depth terms don't depend on the frustum being symmetric or not (only
    // the X/Y off-center terms above do), so this is just that formula
    // directly. The previous values produced a depth mapping unrelated to
    // the real near/far range -- non-monotonic/degenerate enough to explain
    // both near geometry failing to occlude far geometry (this bug) and
    // plausibly some of the earlier culling symptoms, since GX's hardware
    // near/far clip also relies on this same clip.z being sane.
    const float depthTmp = 1.f / (farZ - nearZ);
    dest[2][2] = -nearZ * depthTmp;
    dest[2][3] = depthTmp * -(farZ * nearZ);
    dest[3][2] = -1.f;
}

// ---------------------------------------------------------------------------
// Per-eye render parameters (filled from xrLocateViews each frame)
// ---------------------------------------------------------------------------

struct EyeParams {
    XrPosef   pose;         // from XrView.pose
    XrFovf    fov;          // from XrView.fov
    uint32_t  width;        // from XrViewConfigurationView.recommendedImageRectWidth
    uint32_t  height;
    // Head-center pose (from g_viewSpace, same XR tracking space as pose)
    // for this frame -- see eyePoseToViewMtx's comment on why the camera is
    // anchored to Link's game position via this delta rather than using
    // `pose` as an absolute world position.
    XrVector3f hmdRefPos;
    // World-space position this eye is anchored to (the `linkEyeGame`
    // argument to eyePoseToViewMtx). During normal gameplay this is Link's
    // actual head/eye position (daAlink_c::getSubjectEyePos() -- form- and
    // mount-aware); during cutscenes/events it falls back to the authored
    // camera's own view->lookat.eye instead, since an authored shot may not
    // even be looking at Link -- snapping to his head there would put the
    // viewer inside his skull for shots never designed to be seen from
    // there. Computed once per frame (not per eye) by
    // vr_link::getVrCameraEyeAnchor() in vr_main.cpp's tick(), which is
    // where daAlink_c is actually available -- kept out of this header for
    // the same "heavier dependency, unrelated concern" reason kEyePosScale's
    // comment above gives for not including vr_link_visibility.hpp here.
    cXyz eyeAnchor;
    // VR smooth-turn yaw offset (vr_smooth_turn.hpp), read once per frame
    // via dusk::vr::getSmoothTurnYawRad() and copied in here for both eyes
    // -- see eyePoseToViewMtx's yawRad parameter.
    float smoothTurnYawRad = 0.f;
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

// NEW this session. dComIfGd_getView() can legitimately return nullptr
// when called outside active gameplay (title screen, loading screen, etc.
// -- exactly the scenario beginEye()'s existing assert() was meant to
// catch). That assert() does nothing in this build: RelWithDebInfo defines
// NDEBUG by default, which compiles assert() out entirely, so the null
// dereference inside beginEye() (view->viewMtx, then a write through it in
// the inlined eyePoseToViewMtx) was never actually being caught -- it was
// crashing instead. Callers (tick()) should check this BEFORE calling
// beginEye() and skip VR rendering for that frame if it's false, rather
// than relying on the assert.
inline bool isViewReady() {
    return dComIfGd_getView() != nullptr;
}

// NEW this session (VR_MOD_HANDOFF_10 follow-up, option (c)): carries the
// pass id captured in beginEye() across to endEye(). Fine as a plain global
// since beginEye()/endEye() are always called strictly sequentially for one
// eye at a time (never interleaved/reentrant) -- see tick()'s per-eye loop.
inline uint64_t g_currentEyePassId = 0;

// NEW this session: captured alongside g_currentEyePassId. Ordinary
// GXCopyTex substitutions (shadows, bloom, depth-of-field -- confirmed via
// debugger this session to fire on every eye, every frame, unconditionally)
// always carry the same colorView forward onto whatever new pass object
// they substitute in, so the eye's real render target usually survives even
// when g_currentEyePassId no longer matches. endEye() passes this to
// resolve_pass_checked() so it can tell "target survived, just re-wrapped"
// apart from "target genuinely gone" instead of refusing on every id
// mismatch -- which, empirically, was every single eye/frame.
inline wgpu::TextureView g_currentEyeColorView;

// ROOT-CAUSED this session (VR water rendering solid black): several game
// systems read view->fovy/view->aspect directly instead of view->projMtx --
// most notably daGrdWater_c::Draw() (d_a_obj_groundwater.cpp), which builds
// its reflection env-map matrix via C_MTXLightPerspective(fovy, aspect, ...).
// The culling fix above deliberately left view->fovy/aspect untouched (see
// its comment) since those same two fields also drive particle billboarding,
// rain shadow-projection, audio spatialization, and the modding API --
// changing them wholesale risked side effects on all of those, untested.
// Instead, expose the SAME symmetric-frustum-containing-the-real-asymmetric-
// FOV values already computed for the clipper below, via getEyeSymmetricFov()
// (dusk::vr::getEyeSymmetricFov() forwards to this from vr_main.cpp), so
// individual call sites can opt in to VR-correct values without touching the
// shared view_class fields everything else still relies on.
inline float g_eyeSymmetricFovyDeg = 60.0f;
inline float g_eyeSymmetricAspect = 1.3571428f;

inline void getEyeSymmetricFov(float* fovyDeg, float* aspect) noexcept {
    *fovyDeg = g_eyeSymmetricFovyDeg;
    *aspect = g_eyeSymmetricAspect;
}

inline aurora::gfx::ResolvedTargets beginEye(const EyeParams& eye) {
    // 1. Get the shared game view and patch it with this eye's transform.
    //    This is the same view_class pointer that frame_interpolation.cpp
    //    already modifies each frame; we're just overriding its matrices
    //    for the duration of one offscreen draw.
    view_class* view = dComIfGd_getView();
    assert(view != nullptr && "VR: no active view_class — called outside gameplay?");

    // Build and apply the view matrix, anchored to eye.eyeAnchor (Link's
    // real head position during gameplay, or the authored camera's eye
    // during cutscenes -- see EyeParams::eyeAnchor's comment) rather than
    // treating the raw XR pose as an absolute world position -- see
    // eyePoseToViewMtx's comment.
    eyePoseToViewMtx(view->viewMtx, eye.pose, eye.hmdRefPos, eye.eyeAnchor,
                      kEyePosScale, eye.smoothTurnYawRad);
    j3dSys.setViewMtx(view->viewMtx);

    // Inverse view for anything that needs world-from-view (shadow maps, etc.)
    // MTXInverse is the GC SDK equivalent of cMtx_inverse used in the original.
    MTXInverse(view->viewMtx, view->invViewMtx);

    // Build and apply the asymmetric projection matrix from the eye's FOV.
    // This replaces the C_MTXPerspective call in frame_interpolation.cpp
    // for this eye only; view->near_ and view->far_ come from the game's
    // normal camera setup and are intentionally reused as-is.
    eyeFovToProjMtx(view->projMtx, eye.fov, view->near_, view->far_);

    // ROOT-CAUSED this session (aggressive VR-edge culling investigation):
    // mDoLib_clipper is the actor-culling frustum tester used throughout
    // f_op_actor_mng.cpp's per-actor visibility checks. It's rebuilt every
    // frame from the CURRENT camera's fovy/aspect by the normal (non-VR)
    // camera code (frame_interpolation.cpp/d_camera.cpp) -- but nothing
    // ever rebuilds it for VR's eyes, so it kept testing objects against
    // the flatscreen camera's narrow ~60-degree SYMMETRIC frustum while
    // actually rendering VR's much wider, ASYMMETRIC one: objects visible
    // near the edge of the real VR FOV were being wrongly culled.
    //
    // J3DUClipper (what setup() configures) only supports a symmetric
    // fovy+aspect frustum, not the true asymmetric XR one, so this computes
    // the smallest symmetric frustum that still fully CONTAINS the real
    // asymmetric FOV (using the larger of |tan(angleLeft)|/|tan(angleRight)|
    // for the horizontal half-extent, same for vertical) -- guaranteed not
    // to wrongly cull anything actually visible, at the cost of being very
    // slightly more permissive than optimal on each eye's narrower
    // (nasal/inner) side. Deliberately does NOT touch view->fovy/aspect
    // themselves -- those feed particle billboarding, mirror/water/rain
    // shadow-projection matrices, audio spatialization, and the modding
    // API, none of which this is meant to change.
    {
        const float tl = std::fabs(std::tan(eye.fov.angleLeft));
        const float tr = std::fabs(std::tan(eye.fov.angleRight));
        const float td = std::fabs(std::tan(eye.fov.angleDown));
        const float tu = std::fabs(std::tan(eye.fov.angleUp));
        // Round 2's 2x safety margin was a red herring -- the real cause
        // (mFovyRate going negative for fovy > 90 degrees, see
        // m_Do_lib.cpp's mDoLib_clipper::setup()) is fixed at the source
        // now, so this is back to the minimal symmetric bound that fully
        // contains the real asymmetric FOV.
        const float halfH = std::max(tl, tr);
        const float halfV = std::max(td, tu);
        constexpr float kRadToDeg = 57.29577951308232f;
        const float clipperFovyDeg = 2.f * std::atan(halfV) * kRadToDeg;
        const float clipperAspect = halfH / halfV;
        mDoLib_clipper::setup(clipperFovyDeg, clipperAspect, view->near_, view->far_);
        // Shared with getEyeSymmetricFov() -- see its comment above.
        g_eyeSymmetricFovyDeg = clipperFovyDeg;
        g_eyeSymmetricAspect = clipperAspect;

    }

    // 2. Open the offscreen pass for this eye at its native HMD resolution.
    //    aurora::gfx::create_pass clears color+depth and sets a full-target
    //    viewport/scissor automatically.
    // NOTE (v8): this assert(), like the view!=nullptr one above, is
    // compiled out under RelWithDebInfo/NDEBUG -- if create_pass() fails,
    // this currently falls through silently rather than catching it. Not
    // fixed this session (no evidence yet that it's actually failing,
    // unlike the view-null case) -- flagged so a future crash here isn't
    // mistaken for something new.
    // ROOT-CAUSED this session: the scene draw between beginEye()/endEye()
    // (fpcM_DrawIterater/cAPIGph_Painter) is the SAME code the flatscreen
    // path uses, and it sets its own viewport/scissor via GXSetViewport
    // using the native configured resolution (FB_WIDTH/FB_HEIGHT-ish
    // values), same as it always does -- it has no idea it's currently
    // targeting a VR eye texture of a completely different size. Without
    // this flag, aurora::gx::logical_fb_size() reports THIS offscreen
    // pass's own size as "logical" while offscreen, which collapses the
    // viewport scale factor to 1:1 and applies that native-resolution call
    // as literal pixels on the eye texture -- correct-looking content
    // confined to a small corner, the rest left black. Enabling it here
    // makes the native-resolution viewport call scale up to fill the
    // actual eye target instead, matching what already happens correctly
    // for normal flatscreen rendering. See gfx.hpp's doc comment.
    aurora::gfx::set_offscreen_uses_native_logical_size(true);
    const bool ok = aurora::gfx::create_pass(eye.width, eye.height);
    assert(ok && "VR: create_pass failed — is another offscreen pass already open?");

    // ROOT-CAUSED this session: the scene draw that runs between
    // beginEye()/endEye() (fpcM_DrawIterater/cAPIGph_Painter) is ordinary
    // gameplay code that can queue GXCopyTex calls (shadows, HUD, menu
    // overlays -- constant, normal usage, not an edge case). Draining one
    // of those mid-scene-draw silently substitutes a different pass in via
    // resolve_pass_into(), which doesn't check is_offscreen() at all --
    // g_inOffscreen stays true, but the pass object underneath changes.
    // Capture this pass's real identity now so endEye() can verify it
    // survived, instead of trusting create_pass()'s pass to still be
    // current later. See gfx.hpp's current_pass_id()/resolve_pass_checked()
    // comments for the full mechanism.
    g_currentEyePassId = aurora::gfx::current_pass_id();
    g_currentEyeColorView = aurora::gfx::current_pass_color_view();

    // ROOT-CAUSED this session (architectural fix superseding the many
    // individual per-call-site GXCopyTex guards elsewhere): protect this
    // eye's pass centrally so resolve_pass_into() (extern/aurora/lib/gfx/
    // common.cpp) refuses to substitute over it no matter which system's
    // GXCopyTex call is responsible -- see set_protected_offscreen_pass()'s
    // doc comment in gfx.hpp. Must stay set until AFTER endEye()'s
    // resolve_pass_checked() call below, since that call drains the GX FIFO
    // itself and any queued copy from this eye's own scene draw gets
    // processed during that drain, not before.
    aurora::gfx::set_protected_offscreen_pass(g_currentEyePassId);

    // Return an empty ResolvedTargets as the "pass is now open" signal;
    // the real targets come back from endEye().
    return {};
}

inline aurora::gfx::ResolvedTargets endEye() {
    // Close the offscreen pass and snapshot the color buffer.
    // resolve_pass on an offscreen pass (created by create_pass) ends it
    // and restores the suspended EFB pass — exactly what we want between eyes.
    aurora::gfx::ResolvedTargets targets;
    // CHANGED this session (VR_MOD_HANDOFF_10/11 follow-up): resolve_pass_checked()
    // instead of resolve_pass() -- verifies the pass beginEye() opened is
    // still current, and now also checks g_currentEyeColorView on a
    // mismatch: since ordinary GXCopyTex substitutions (shadows, bloom,
    // depth-of-field -- confirmed this session to fire on every single eye,
    // every frame) always carry the real render target forward onto the new
    // pass object, this usually resolves normally even after a
    // substitution. Only a genuine loss of the render target (both id AND
    // colorView mismatch) returns false and logs a warning -- targets stays
    // {} (colorTexture null) rather than silently handing back a
    // desktop-sized texture like the crash this was root-caused from.
    // Callers (vr_main.cpp's tick()) must still check targets.colorTexture
    // before using it -- a null/invalid texture here means skip this eye's
    // copy for this frame, not a bug to assert on.
    // CORRECTED this session: must NOT clear the override before this call.
    // resolve_pass_checked() drains the GX command FIFO as its own first
    // action (see its call to gx::fifo::drain(), and HANDOFF_12's finding on
    // this exact behavior) -- and the game's GXSetViewport(0,0,608,448) call
    // from the scene draw above is only QUEUED at record time, not actually
    // processed (i.e. map_logical_viewport() isn't called) until THAT drain
    // runs. Clearing the override here before resolve_pass_checked meant the
    // native-resolution viewport was still being mapped with the override
    // already back off -- confirmed via diagnostic logging showing
    // native_override=false at the point map_logical_viewport actually ran.
    // The override must stay on through this call and only come off after.
    const bool ok = aurora::gfx::resolve_pass_checked({.color = true, .depth = false}, targets, g_currentEyePassId,
                                                       g_currentEyeColorView);

    // Scope ends here regardless of outcome -- must not leak into whatever
    // (non-VR) offscreen pass runs next this frame (shadows/bloom/DOF all
    // size their own viewport to their own target and rely on the default
    // off behavior). Harmless once is_offscreen() is false (logical_fb_size()
    // only consults this flag in the offscreen branch), but cleared
    // unconditionally here for clarity.
    aurora::gfx::set_offscreen_uses_native_logical_size(false);
    // Matches the set_protected_offscreen_pass() call in beginEye() -- clear
    // it now that this eye's pass is fully resolved (or has failed to), so
    // whatever (non-VR) offscreen pass runs next this frame isn't
    // accidentally protected too.
    aurora::gfx::clear_protected_offscreen_pass();

    if (!ok) {
        // Deliberately NOT an assert: unlike create_pass()/resolve_pass()
        // failing outside an active pass (a real logic bug), a foreign
        // substitution is a known, already-logged condition (see
        // resolve_pass_checked's own warning) that ordinary gameplay
        // rendering can trigger. Asserting here would just reintroduce a
        // crash under a different name.
        return {};
    }
    return targets;
    // targets.color is now a wgpu::TextureView of this eye's rendered frame.
    // Pass it to vr_xr_submit::submitEye() once that file is written.
}

// ---------------------------------------------------------------------------
// Head-locked HUD billboard
//
// The flat 2D HUD (hearts, rupees, message boxes -- see m_Do_graphic.cpp's
// mDoGph_drawHud2D()) used to be drawn per-eye with a raw screen-space
// orthographic projection, identical in both eye textures -- zero stereo
// disparity, which reads as either "painted on the lens" or an otherwise
// uncomfortable, badly-defined depth. Instead, mDoGph_gInf_c::
// captureHudBillboard() (m_Do_graphic.cpp) renders that same flat HUD once
// per frame, before either eye's pass opens, into a small shared texture --
// getHudBillboardTexObj() -- and this draws that texture as a real 3D quad
// using the eye's actual asymmetric projection, so it gets correct per-eye
// convergence like a real object sitting a fixed distance away instead of a
// flat image glued to each eye's raster grid.
//
// The panel derives its alpha from the captured COLOR itself (a "luma key")
// rather than the captured texture's own per-pixel alpha channel:
// individual HUD element materials don't reliably produce a usable alpha
// value (confirmed in-headset -- real per-pixel alpha blending made the
// whole panel invisible even though the captured color content was
// verified correct via a forced-opaque debug pass; same class of "this
// material's translucency isn't simple texture alpha" finding as
// CLAUDE.md's water-reflection investigation, which was never fully
// root-caused there either; and unlike the eye-buffer readback path,
// aurora's GXCopyTex destination pointer here is only a GPU-texture-cache
// key, not real CPU-readable pixel data, so directly inspecting captured
// alpha values from code isn't an option either). The capture's clear
// color is black, so summing R+G+B (see drawHudBillboard()) gives ~0 alpha
// for empty background and higher alpha for any real HUD content --
// not pixel-perfect (a near-black icon pixel would read as transparent
// too) but avoids an open-ended per-material investigation.
// ---------------------------------------------------------------------------

// Game units per metre -- same constant/value as kEyePosScale above, kept as
// its own name here since this isn't converting an HMD pose, just sizing a
// panel in the same unit system.
inline constexpr float kHudUnitsPerMetre = kEyePosScale;

// Tunable placement/size, first-pass values -- retune empirically in-headset
// (this project's usual workflow, see CLAUDE.md's Build Workflow section)
// rather than deriving analytically; "comfortable, unobtrusive" is a
// subjective target.
inline constexpr float kHudDistanceMeters = 2.0f;
inline constexpr float kHudWidthMeters = 1.4f; // bumped up from 1.0f per user feedback
inline constexpr float kHudHeightMeters = kHudWidthMeters * (448.0f / 608.0f); // matches FB_HEIGHT/FB_WIDTH

// Damping: the panel is still drawn flat-facing-you every frame (its own
// local right/up axes always match the CURRENT eye, so it never looks
// tilted/warped -- see computeHudPose()), but the direction its CENTER sits
// in is a low-pass-filtered ("damped") version of head orientation instead
// of the raw per-frame pose, so small tracking jitter doesn't translate
// directly into visible shake. Only orientation is damped, not position --
// position tracks the head instantly, matching how shipped VR games'
// body-locked UI usually behaves (translating with you feels natural;
// lagging your rotation is what actually reads as "steadier", not laggy).
//
// g_hudSmoothedWorldForward is maintained in GAME-WORLD space (not
// eye-local) specifically so it's meaningful across frames regardless of
// which eye is currently rendering -- updateHudSmoothing() computes it once
// per frame (vr_main.cpp's tick(), before the eye loop, alongside
// mDoGph_gInf_c::captureHudBillboard()); computeHudPose() re-projects it
// into whichever eye is currently drawing.
inline cXyz g_hudSmoothedWorldForward{0.f, 0.f, -1.f};
inline bool g_hudSmoothingInitialized = false;

// Per-frame lerp factor toward the raw head direction -- smaller = more lag
// (steadier but slower to catch up when you turn on purpose), larger = less
// lag (closer to the original shaky head-locked behavior). Not
// frame-time-corrected (VR here runs at a fairly stable fixed refresh, and
// this project already accepts similar simplifications elsewhere) --
// retune empirically in-headset like the other constants in this section.
inline constexpr float kHudDampingAlpha = 0.08f;

inline void normalizeInPlace(cXyz& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 0.0001f) {
        v.x /= len; v.y /= len; v.z /= len;
    }
}

// Real-time (undamped) game-world-space direction the player's head is
// currently facing, given a raw OpenXR head pose and the current VR
// smooth-turn yaw offset (vr_smooth_turn.hpp) -- already baked in here
// (passed straight through to eyePoseToViewMtx) rather than something a
// caller needs to add on separately. Factored out of updateHudSmoothing()
// below so it has exactly one implementation: updateHudSmoothing() low-pass
// filters this for the HUD billboard (steadier, deliberately laggy), while
// dusk::vr::getHeadMoveAngleS() (vr_main.cpp, VR movement-direction basis)
// uses it directly, undamped, since movement should track head rotation
// immediately rather than lag the way the HUD intentionally does.
inline cXyz computeHeadWorldForward(const XrPosef& headPose, float yawRad) {
    // Reuses eyePoseToViewMtx (already validated -- it drives the entire
    // working 3D VR scene) purely for its rotation math: passing the same
    // position as both the pose and the reference gives a zero position
    // delta, and a dummy zero linkEyeGame, since only the ROTATION rows are
    // used below -- this function's translation output is discarded.
    Mtx headViewMtx;
    eyePoseToViewMtx(headViewMtx, headPose, headPose.position, cXyz(0.f, 0.f, 0.f),
                      kEyePosScale, yawRad);

    // headViewMtx's rotation submatrix is R^T (world-to-head), per
    // eyePoseToViewMtx's own comment ("View matrix = transpose(R) | ...").
    // World-space forward = R * localForward, localForward = (0,0,-1)
    // (OpenXR's local-space forward convention, matching the sign already
    // validated for computeHudPose()'s un-damped distance below) = R's
    // third column negated = R^T's third ROW negated = -(row 2 of
    // headViewMtx). This is a derivation, not a guess -- if it's ever wrong
    // it will self-evidently invert (panel/movement points away instead of
    // toward you) rather than subtly misbehave.
    cXyz forward(-headViewMtx[2][0], -headViewMtx[2][1], -headViewMtx[2][2]);
    normalizeInPlace(forward);
    return forward;
}

// Called once per frame (not per eye) to advance the damped reference
// direction toward the current real head pose. `yawRad` is the VR
// smooth-turn offset (vr_smooth_turn.hpp) -- passed through so the HUD's
// world-forward reference rotates along with the rest of the scene
// instead of staying pinned to the un-rotated raw head direction, which
// would otherwise make the HUD appear to drift out of sync with the
// smooth-turned view.
inline void updateHudSmoothing(const XrPosef& headPose, float yawRad) {
    const cXyz rawForward = computeHeadWorldForward(headPose, yawRad);

    if (!g_hudSmoothingInitialized) {
        // Snap on the very first frame -- avoids a pop-in lerp from the
        // arbitrary (0,0,-1) default toward wherever the player actually
        // starts looking.
        g_hudSmoothedWorldForward = rawForward;
        g_hudSmoothingInitialized = true;
        return;
    }

    g_hudSmoothedWorldForward.x += (rawForward.x - g_hudSmoothedWorldForward.x) * kHudDampingAlpha;
    g_hudSmoothedWorldForward.y += (rawForward.y - g_hudSmoothedWorldForward.y) * kHudDampingAlpha;
    g_hudSmoothedWorldForward.z += (rawForward.z - g_hudSmoothedWorldForward.z) * kHudDampingAlpha;
    // Lerping two unit vectors shrinks the result -- renormalize.
    normalizeInPlace(g_hudSmoothedWorldForward);
}

// One quad's worth of eye-space corners, in draw order (top-left, top-right,
// bottom-right, bottom-left -- matches mDoGph_drawFilterQuad's texcoord
// convention elsewhere in this codebase). Deliberately its own function,
// separate from the actual GX draw call below: this is the ONE place that
// decides where the panel sits.
struct HudQuadCorners {
    float x[4], y[4], z[4];
};

// Shared eye-space billboard corner math -- factored out of computeHudPose()
// 2026-08-16 when the new menu billboard needed the exact same math with
// different distance/size constants and (optionally) a different smoothed-
// direction source. This project has been bitten more than once by two call
// sites each carrying their own copy of a "should match the other one"
// formula that silently drifts out of sync (vr_smooth_turn.hpp's own header
// comment cites the eyePoseToViewMtx/buildHandMtx case) -- one
// implementation here instead of a second hand-copied one.
//
// distanceUnits/halfWidthUnits/halfHeightUnits are MAGNITUDES, not
// pre-signed -- see computeHudPose()'s own historical note (still true
// here): (ex,ey,ez) below already carries the correct sign, so a caller
// passing an already-negated distance would double-negate and flip the
// panel behind the camera.
inline HudQuadCorners computeBillboardPose(const cXyz& smoothedWorldForward, float distanceUnits,
                                            float halfWidthUnits, float halfHeightUnits) {
    view_class* view = dComIfGd_getView();
    assert(view != nullptr && "VR: computeBillboardPose() called outside gameplay?");

    // Re-project the damped WORLD-space forward direction into THIS eye's
    // local space via the eye's own (already-current, un-damped) view
    // matrix rotation -- this is what keeps the panel's own plane always
    // flat-facing the current eye (no tilt/warp) while only its center
    // position lags. When smoothedWorldForward is fully caught up (matches
    // the current eye's actual forward), (ex,ey,ez) round-trips to exactly
    // (0,0,-1) -- i.e. identical to undamped behavior -- confirming the
    // math, not just asserting it.
    const Mtx& m = view->viewMtx;
    const cXyz& f = smoothedWorldForward;
    const float ex = m[0][0] * f.x + m[0][1] * f.y + m[0][2] * f.z;
    const float ey = m[1][0] * f.x + m[1][1] * f.y + m[1][2] * f.z;
    const float ez = m[2][0] * f.x + m[2][1] * f.y + m[2][2] * f.z;

    const float cx = ex * distanceUnits, cy = ey * distanceUnits, cz = ez * distanceUnits;

    HudQuadCorners c;
    c.x[0] = cx - halfWidthUnits; c.y[0] = cy + halfHeightUnits; c.z[0] = cz;  // top-left
    c.x[1] = cx + halfWidthUnits; c.y[1] = cy + halfHeightUnits; c.z[1] = cz;  // top-right
    c.x[2] = cx + halfWidthUnits; c.y[2] = cy - halfHeightUnits; c.z[2] = cz;  // bottom-right
    c.x[3] = cx - halfWidthUnits; c.y[3] = cy - halfHeightUnits; c.z[3] = cz;  // bottom-left
    return c;
}

inline HudQuadCorners computeHudPose() {
    const float halfW = kHudWidthMeters * 0.5f * kHudUnitsPerMetre;
    const float halfH = kHudHeightMeters * 0.5f * kHudUnitsPerMetre;
    const float dist = kHudDistanceMeters * kHudUnitsPerMetre;
    return computeBillboardPose(g_hudSmoothedWorldForward, dist, halfW, halfH);
}

// Call once per eye, from mDoGph_Painter()'s per-eye HUD call site (replaces
// the flat mDoGph_drawHud2D() call there while in VR). Must run after the
// eye's real scene draw (so it draws on top) and needs `hudTex` already
// populated for this frame by mDoGph_gInf_c::captureHudBillboard().
inline void drawHudBillboard(TGXTexObj* hudTex) {
    view_class* view = dComIfGd_getView();
    assert(view != nullptr && "VR: drawHudBillboard() called outside gameplay?");

    // GXSetProjection is a stateful register write, not automatic -- the 3D
    // world draw and the (skipped-in-VR, but still state-touching) HUD path
    // both clobber it earlier this frame. Reassert this eye's real
    // asymmetric projection immediately before drawing, same idiom
    // d_menu_collect.cpp's dMenu_Collect3D_c::setupItem3D() already uses for
    // "one real-perspective 3D draw sandwiched inside 2D UI code".
    GXSetProjection(view->projMtx, GX_PERSPECTIVE);
    // Identity position matrix: quad vertices below are authored directly in
    // eye/camera-local space, so no view-matrix multiply is needed. This is
    // what makes the panel head-locked "for free" -- it's rigidly glued to
    // wherever this eye is looking, every frame, with zero extra tracking
    // logic.
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(0);

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3C);

    // Color = real captured texture color (stage 0, default identity swap).
    // Alpha = derived from that SAME color (R+G+B, saturating), NOT the
    // texture's own alpha channel -- see this section's header comment for
    // why the real per-material alpha turned out unusable. Background is
    // black (the capture's clear color) -> alpha ~0 -> transparent; any real
    // HUD content is colored -> higher alpha -> visible. A "luma key"
    // (classic technique, not something new to this codebase's GX usage --
    // channel-swap tricks like this already appear in the bloom code around
    // this same file). Not pixel-perfect (a near-black icon pixel would read
    // as transparent too) but needs no per-material investigation.
    // GX_TEV_SWAP1/2/3 are repurposed here as "alpha reads channel X"
    // tables (SWAP0 is left as the untouched identity default for color).
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_GREEN);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);

    GXSetNumTevStages(3);

    // Stage 0: real color; alpha = texture's red channel (via SWAP1).
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    // Stage 1: color unchanged; alpha += texture's green channel (SWAP2).
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP2);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    // Stage 2: color unchanged; alpha += texture's blue channel (SWAP3).
    GXSetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevSwapMode(GX_TEVSTAGE2, GX_TEV_SWAP0, GX_TEV_SWAP3);
    GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    // Disabled Z-test/write: the panel always draws on top of the 3D world,
    // same as the flat ortho HUD it replaces (never occluded by scene
    // geometry -- getting close to a wall shouldn't make hearts disappear).
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);

    GXLoadTexObj(hudTex, GX_TEXMAP0);

    const HudQuadCorners c = computeHudPose();
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(c.x[0], c.y[0], c.z[0]); GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(c.x[1], c.y[1], c.z[1]); GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(c.x[2], c.y[2], c.z[2]); GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(c.x[3], c.y[3], c.z[3]); GXTexCoord2f32(0.0f, 1.0f);
    GXEnd();
}

// ---------------------------------------------------------------------------
// VR menu billboard (Dusklight/RmlUi menu, made visible in the headset)
// ---------------------------------------------------------------------------
//
// Goal (explicit user request, 2026-08-16, Phase 2 of the VR-menu feature --
// Phase 1, controller open/navigate, is already confirmed working, see
// vr_menu_gamepad.hpp): the Dusklight menu previously only rendered to the
// desktop window's own surface -- opening it in VR did nothing visible in
// the headset. Full research trail + verified facts in the approved plan,
// C:\Users\joeyw\.claude\plans\fizzy-finding-minsky.md, and the vr-mod-notes
// skill's "Phase 2" section -- summary here, not re-derived:
//
// RmlUi already renders into a real, separate, copyable wgpu::Texture
// (extern/aurora/lib/rmlui.cpp's s_renderTarget, exposed read-only via the
// new aurora::rmlui::get_render_target()) -- NOT tied to the desktop
// window's swapchain view. Critically, the only thing that writes into it
// (record_frame()) is only ever called from aurora::end_frame(), which --
// per m_Do_main.cpp's real call order -- always runs AFTER dusk::vr::tick()
// has already finished that same frame's own per-eye rendering. So
// s_renderTarget is always a fully-finished PREVIOUS frame throughout the
// whole VR eye loop -- no torn reads, no double-buffering needed, just one
// frame (~11-14ms) of inherent latency, imperceptible for a UI overlay.
// Deliberately NOT attempting to make record_frame() run earlier -- it
// calls g_context->Update() (advances RmlUi's animation/input timers), so
// calling it twice a frame would double-advance UI state.
//
// The bridge (GX texture object <- raw WebGPU texture): a GXTexObj doesn't
// own a texture itself, its `data` field is just an opaque cache-key
// pointer -- aurora::gx::ensure_external_copy_texture() (new, mirrors
// GXCopyTex's own GXState::copyTextureCache/copyTextures[dest] population,
// see gx.cpp) lets us populate that same cache from content that ISN'T the
// output of a real GX render, so the texture object below resolves to a
// texture we can freely CopyTextureToTexture into ourselves.
//
// RmlUi's alpha is real and PREMULTIPLIED (confirmed via aurora.cpp's own
// compositing pipeline, g_CopyPremultipliedAlphaPipeline) -- unlike HUD's
// alpha (never meaningfully authored, hence HUD's luma-key TEV workaround
// above), so this billboard samples GX_CA_TEXA directly and uses
// GX_BL_ONE/GX_BL_INVSRCALPHA, NOT HUD's GX_BL_SRCALPHA/GX_BL_INVSRCALPHA.
// ---------------------------------------------------------------------------

// Tunable placement/size -- untested starting guesses, to be tuned live
// in-headset like every other tuned distance in this project. Deliberately
// NOT reusing HUD's constants: HUD is a glanced-at status overlay
// (deliberately far/small); a settings menu is read/interacted-with for
// extended periods and wants to be closer/larger, more like a typical VR
// desktop-panel placement.
inline constexpr float kMenuBillboardDistanceMeters = 1.2f;
inline constexpr float kMenuBillboardWidthMeters = 1.0f;

// Height is NOT a compile-time constant like HUD's fixed 608:448 aspect --
// RmlUi's canvas is OS-window-sized (any aspect, can change on resize), so
// this is computed at draw time from the real render-target dimensions
// (see ensureAndCopyMenuBillboardTexture()'s cached width/height below).
inline HudQuadCorners computeMenuBillboardPose(float aspectHeightOverWidth) {
    const float halfW = kMenuBillboardWidthMeters * 0.5f * kHudUnitsPerMetre;
    const float halfH = halfW * aspectHeightOverWidth;
    const float dist = kMenuBillboardDistanceMeters * kHudUnitsPerMetre;
    // Deliberately reuses g_hudSmoothedWorldForward (the SAME damped
    // direction HUD uses) rather than standing up a second independent
    // smoothing system -- both are head-locked-with-damping panels driven
    // by the same "where is the player looking" signal. If in-headset
    // testing shows the menu wants different damping feel (it's read/
    // interacted-with rather than glanced-at), splitting it into its own
    // g_menuSmoothedWorldForward is a trivial, low-risk follow-up -- not
    // built speculatively without evidence it's needed.
    return computeBillboardPose(g_hudSmoothedWorldForward, dist, halfW, halfH);
}

// Persistent GX texture-object identity for the menu billboard. The key is
// pointer identity only (never read/written) -- same convention
// GXState::copyTextures/copyTextureCache use elsewhere (see gx.cpp). No
// ResTIMG/mDoLib_setResTimgObj() indirection needed here (unlike HUD's
// m_hudBillboardTexObj) -- GXInitTexObj() is called directly since there's
// no real GX-side texture resource being copied from.
inline u8 g_menuBillboardTexKey[4]{};
inline TGXTexObj g_menuBillboardTexObj{};
inline uint32_t g_menuBillboardTexWidth = 0;
inline uint32_t g_menuBillboardTexHeight = 0;

// Call once per eye (like drawHudBillboard()), from vr_main.cpp's tick(),
// gated on dusk::ui::any_document_visible() -- needs
// g_menuBillboardTexObj already populated this frame by
// ensureAndCopyMenuBillboardTexture() (vr_main.cpp, pre-eye-loop window).
inline void drawMenuBillboard(TGXTexObj* menuTex, float aspectHeightOverWidth) {
    view_class* view = dComIfGd_getView();
    assert(view != nullptr && "VR: drawMenuBillboard() called outside gameplay?");

    GXSetProjection(view->projMtx, GX_PERSPECTIVE);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(0);

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3C);

    // Real per-pixel alpha -- no luma-key needed, see this section's header
    // comment for why (RmlUi's alpha is real/meaningful/premultiplied,
    // unlike HUD's).
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    // Premultiplied blend -- matches RmlUi's own compositing convention
    // (aurora.cpp's g_CopyPremultipliedAlphaPipeline). NOT HUD's
    // GX_BL_SRCALPHA/GX_BL_INVSRCALPHA -- that pairing is correct for HUD's
    // own non-premultiplied luma-key alpha only; using it here would
    // double-darken real premultiplied content.
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_SET);
    // Disabled Z-test/write, same reasoning as HUD -- a settings menu
    // shouldn't be hideable by standing near a wall.
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);

    GXLoadTexObj(menuTex, GX_TEXMAP0);

    const HudQuadCorners c = computeMenuBillboardPose(aspectHeightOverWidth);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(c.x[0], c.y[0], c.z[0]); GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(c.x[1], c.y[1], c.z[1]); GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(c.x[2], c.y[2], c.z[2]); GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(c.x[3], c.y[3], c.z[3]); GXTexCoord2f32(0.0f, 1.0f);
    GXEnd();
}

// Real render-target aspect ratio (height/width), updated once per frame by
// ensureAndCopyMenuBillboardTexture() below -- RmlUi's canvas is OS-window-
// sized (any aspect, can change on resize), unlike HUD's fixed 608:448, so
// this can't be a compile-time constant. vr_main.cpp reads this when
// calling drawMenuBillboard() per eye.
inline float g_menuBillboardAspectHeightOverWidth = 1.0f;

namespace menu_billboard_detail {
// Side-table slots for the encoder-task callback below, same pattern
// vr_xr_submit.hpp's Session::pendingCopySrc_ already uses -- a
// wgpu::Texture is NOT safe to carry directly through push_encoder_task's
// raw memcpy'd payload buffer (it's a ref-counted handle, not POD), so it's
// kept alive here instead and the payload carries nothing. Only one menu
// billboard exists (not per-eye like the real eye-copy case), so a single
// global slot pair is enough.
inline wgpu::Texture s_pendingCopySrc;
inline wgpu::Texture s_pendingCopyDst;
inline uint32_t s_pendingCopyWidth = 0;
inline uint32_t s_pendingCopyHeight = 0;
inline aurora::gfx::EncoderTaskId s_copyTaskId = aurora::gfx::InvalidEncoderTask;

// Same 6-line shape as vr_xr_submit.hpp's dusk::vr::copyTextureToTexture()
// -- duplicated rather than cross-included to avoid pulling that much
// heavier (OpenXR/D3D12-session-specific) header into this one just for
// this, per the plan's own "whichever avoids include-order friction" note.
inline void copyEncoderTaskCallback(const aurora::gfx::EncoderTaskContext& /*ctx*/,
                                     const wgpu::CommandEncoder& cmd, const void* /*payload*/,
                                     size_t /*payloadSize*/, void* /*userdata*/) {
    if (!s_pendingCopySrc || !s_pendingCopyDst) {
        return;
    }
    wgpu::CommandEncoder mutableCmd = cmd; // CopyTextureToTexture is non-const

    wgpu::TexelCopyTextureInfo srcCopy{};
    srcCopy.texture = s_pendingCopySrc;
    srcCopy.aspect = wgpu::TextureAspect::All;

    wgpu::TexelCopyTextureInfo dstCopy{};
    dstCopy.texture = s_pendingCopyDst;
    dstCopy.aspect = wgpu::TextureAspect::All;

    wgpu::Extent3D extent{s_pendingCopyWidth, s_pendingCopyHeight, 1};
    mutableCmd.CopyTextureToTexture(&srcCopy, &dstCopy, &extent);
}
} // namespace menu_billboard_detail

// Plan step 4 (real content, replacing step 3's solid-color test -- that
// test CONFIRMED WORKING in-headset, so the test scaffolding is removed
// here per this project's standing "remove diagnostics freely once
// confirmed" practice, not left in place). Call once per frame (not per
// eye) from the same pre-eye-loop window captureHudBillboard()/
// captureMapCopy2D() already use -- confirmed safe for
// aurora::gfx::push_encoder_task() by step 2's probe.
inline void ensureAndCopyMenuBillboardTexture() {
    const auto& rt = aurora::rmlui::get_render_target();
    if (!rt.texture || rt.size.width == 0 || rt.size.height == 0) {
        // RmlUi hasn't rendered a frame yet this session (or the very
        // first frame after a document just opened) -- nothing to copy
        // yet, try again next frame.
        return;
    }

    const uint32_t width = rt.size.width;
    const uint32_t height = rt.size.height;
    if (g_menuBillboardTexWidth != width || g_menuBillboardTexHeight != height) {
        // Handles both first-use AND the desktop window being resized
        // mid-VR-session (RmlUi's canvas is OS-window-sized).
        GXInitTexObj(&g_menuBillboardTexObj, g_menuBillboardTexKey, width, height, GX_TF_RGBA8,
                     GX_CLAMP, GX_CLAMP, GX_FALSE);
        g_menuBillboardTexWidth = width;
        g_menuBillboardTexHeight = height;
    }
    g_menuBillboardAspectHeightOverWidth = static_cast<float>(height) / static_cast<float>(width);

    wgpu::Texture dst =
        aurora::gx::ensure_external_copy_texture(g_menuBillboardTexKey, width, height, GX_TF_RGBA8);

    menu_billboard_detail::s_pendingCopySrc = rt.texture;
    menu_billboard_detail::s_pendingCopyDst = dst;
    menu_billboard_detail::s_pendingCopyWidth = width;
    menu_billboard_detail::s_pendingCopyHeight = height;

    if (menu_billboard_detail::s_copyTaskId == aurora::gfx::InvalidEncoderTask) {
        aurora::gfx::EncoderTaskDescriptor desc{
            .label = "vr_menu_billboard_copy",
            .callback = &menu_billboard_detail::copyEncoderTaskCallback,
            .userdata = nullptr,
        };
        menu_billboard_detail::s_copyTaskId = aurora::gfx::register_encoder_task_type(desc);
    }
    aurora::gfx::push_encoder_task(menu_billboard_detail::s_copyTaskId, nullptr, 0);
}

// ---------------------------------------------------------------------------
// World-space aim-point marker ("physical crosshair")
// ---------------------------------------------------------------------------
//
// Goal (explicit user request, 2026-08-12): "add a physical crosshair in
// the game world to show where you are aiming all items." Draws a small,
// soft-edged glowing dot at a real WORLD-SPACE point -- deliberately NOT
// head-locked/eye-space-fixed like drawHudBillboard() above, since this is
// meant to look like a real object sitting at the aim point in the game
// world (correct per-eye stereo parallax, normal depth-testing/occlusion
// against scene geometry), not a screen overlay.
//
// "All items" scope: reuses daAlink_c::mSight -- the SAME shared sight/
// aim-point object already driven by setBowSight() (bow/slingshot),
// setHookshotSight(), AND daBoomerang_c's own throw-aim code (confirmed via
// each one's own mSight.setPos()/onDrawFlg() calls) -- one flag+position
// pair already covers all four items with zero per-item special-casing
// needed here. Deliberately does NOT cover bombs (arc-thrown, not a
// straight sight line -- mSight isn't driven for them at all) or the
// fishing rod's cast (same reason) -- out of scope for this round, per
// explicit user choice when this was scoped.
//
// Call once per eye, from vr_main.cpp's tick() right after
// cAPIGph_Painter() (so the world's own geometry has already been drawn
// this eye and Z-testing against it is meaningful) and before endEye().
// Caller supplies the world-space aim point
// (daAlink_c::getLineTopPosP()/getAimSightVisible()) -- this file stays
// free of the heavy d_a_alink.h include, matching drawHudBillboard()'s own
// "caller supplies pre-computed data" pattern above.
inline void drawAimCrosshair(const cXyz& worldPos) {
    view_class* view = dComIfGd_getView();
    assert(view != nullptr && "VR: drawAimCrosshair() called outside gameplay?");

    // Transform the world-space aim point into THIS eye's view/eye space --
    // vertices below are authored as eye-space offsets from it, so an
    // identity position matrix (same idiom as drawHudBillboard()) is all
    // that's needed to place them correctly; real per-eye stereo parallax
    // comes for free from view->viewMtx already differing per eye.
    cXyz eyeSpacePos;
    mDoMtx_multVec(view->viewMtx, &worldPos, &eyeSpacePos);

    // Behind the eye -- nothing sensible to draw. Shouldn't normally happen
    // for a real aim point, but a max-range miss right at the edge of view
    // could plausibly land here; skip rather than draw garbage.
    if (eyeSpacePos.z >= 0.0f) return;

    // GXSetProjection is a stateful register write clobbered earlier this
    // frame (2D UI, etc.) -- reassert this eye's real asymmetric projection
    // immediately before drawing, same idiom drawHudBillboard() uses.
    GXSetProjection(view->projMtx, GX_PERSPECTIVE);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(0);

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    // No lighting, no texture -- per-vertex color, passed straight through.
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    // Alpha blend (not additive) -- a bright warm color with a per-vertex
    // alpha falloff (near-opaque center, fully transparent edge) reads as a
    // soft glow without washing out against bright daytime scenes the way
    // additive blending would.
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    // Depth-TEST enabled (occluded by real geometry -- this is what makes
    // it read as a physical object rather than a HUD overlay) but
    // depth-WRITE disabled -- standard for translucent geometry, avoids
    // punching a hole in the depth buffer that other translucent draws
    // (particles, etc.) would incorrectly sort against.
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);

    constexpr int kSegments = 16;
    constexpr float kMinRadiusUnits = 8.0f;  // ~8cm at this project's ~100 units/metre scale -- the original fixed size, already confirmed correct at typical/close aim range; now a FLOOR, not the constant size
    constexpr GXColor kGlowColor = {235, 30, 30, 220};  // red, near-opaque center

    // Distance-scaled radius (2026-08-19 follow-up request: "make the red
    // aiming reticle... get bigger as it gets further from the camera, that
    // way it stays visible"). A fixed world-space radius subtends a
    // shrinking angle on screen as range increases -- fine up close, but
    // this game's ranged items (bow/slingshot/hookshot/boomerang) can aim
    // at targets hundreds of units out, where a flat 8-unit dot becomes
    // imperceptibly small. Scaling the radius proportionally to distance
    // instead holds a roughly CONSTANT apparent angular size beyond
    // kMinRadiusUnits's own close-range floor -- eyeSpacePos.z is already
    // the (negative, camera-forward-is--Z) distance from this eye, no extra
    // transform needed. kAngularSizeRatio is an untested guess (radius =
    // distance * ratio once that exceeds the floor; ratio 0.02 -> roughly
    // atan(0.02) ~= 1.15 degrees half-angle, ~2.3 degrees full width,
    // comparable to an ordinary crosshair/reticle's visual size) -- retune
    // if it still reads as too small far away (raise) or grows distractingly
    // large at long range (lower).
    constexpr float kAngularSizeRatio = 0.02f;
    const float distance = -eyeSpacePos.z;
    const float radius = std::max(kMinRadiusUnits, distance * kAngularSizeRatio);

    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, kSegments + 2);
    GXPosition3f32(eyeSpacePos.x, eyeSpacePos.y, eyeSpacePos.z);
    GXColor4u8(kGlowColor.r, kGlowColor.g, kGlowColor.b, kGlowColor.a);
    for (int i = 0; i <= kSegments; ++i) {
        const float t = (2.0f * static_cast<float>(M_PI) * static_cast<float>(i)) / static_cast<float>(kSegments);
        const float dx = radius * std::cos(t);
        const float dy = radius * std::sin(t);
        GXPosition3f32(eyeSpacePos.x + dx, eyeSpacePos.y + dy, eyeSpacePos.z);
        GXColor4u8(kGlowColor.r, kGlowColor.g, kGlowColor.b, 0);  // transparent edge
    }
    GXEnd();
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
