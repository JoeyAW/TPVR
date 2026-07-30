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
#include "m_Do/m_Do_lib.h"         // mDoLib_clipper::setup()
#include <dolphin/mtx.h>           // C_MTXPerspective, mDoMtx_lookAt (or equivalent)
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
    float             scale = kEyePosScale)
{
    const auto& q = pose.orientation;
    const auto& p = pose.position;

    // Offset of this eye from the head-center reference pose, in metres.
    const float dx = p.x - hmdRefPos.x;
    const float dy = p.y - hmdRefPos.y;
    const float dz = p.z - hmdRefPos.z;

    // Scale to game units and flip Z, then anchor to Link's actual game-world
    // eye position for this frame (matches buildHandMtx's convention).
    // ROOT-CAUSED this session (VR shadow-stretching investigation): kept
    // in double precision here and through the translation dot-products
    // below. Diagnostic logging showed this specific area's world
    // coordinates run to ~100,000+ units (stage-specific origin offset far
    // from the visible geometry) -- at that magnitude float32 only has
    // ~0.01-0.02 units of precision left, and objects with small local
    // scale (e.g. a shadow blob with radius ~20) concatenated against a
    // huge-magnitude view matrix translation are exactly where that
    // rounding error becomes visually obvious (stretching/distortion).
    // This construction is entirely new this session (eyePoseToViewMtx
    // didn't anchor to game position at all before), so it never got the
    // numerical conditioning the original mDoMtx_lookAt-based flatscreen
    // camera path presumably has; computing in double here and truncating
    // to f32 only at the final Mtx write reduces that error without
    // changing the Mtx storage format GX expects.
    const double wx_ = static_cast<double>(linkEyeGame.x) + static_cast<double>(dx) * scale;
    const double wy_ = static_cast<double>(linkEyeGame.y) + static_cast<double>(dy) * scale;
    const double wz_ = static_cast<double>(linkEyeGame.z) - static_cast<double>(dz) * scale;

    // Orientation: use the eye's raw quaternion UNFLIPPED. Confirmed this
    // session -- flipping qz here (mirroring buildHandMtx's convention,
    // which is right for hand POSITION/mesh-space, not this) inverted pitch
    // and roll (looking down looked up, tilting left looked right). The
    // pre-existing (pre-anchor-fix) code already used the raw quaternion
    // directly and head look-around was already confirmed correct back
    // then -- only the POSITION needed the anchor+flip treatment above, the
    // rotation matrix build below was never broken.
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

    // Build and apply the view matrix, anchored to Link's actual game-world
    // eye position (view->lookat.eye) rather than treating the raw XR pose
    // as an absolute world position -- see eyePoseToViewMtx's comment.
    eyePoseToViewMtx(view->viewMtx, eye.pose, eye.hmdRefPos, view->lookat.eye);
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

        // TEMP DIAGNOSTIC (culling investigation): confirm the actual
        // computed values once, to rule out a units/formula bug producing
        // something degenerate instead of the intended wide-but-sane frustum.
        static bool loggedClipper = false;
        if (!loggedClipper) {
            loggedClipper = true;
            char msg[256];
            _snprintf_s(msg, _TRUNCATE,
                        "[dusk::vr::beginEye] clipper fov L%f R%f U%f D%f -> fovyDeg=%f aspect=%f near=%f far=%f\n",
                        eye.fov.angleLeft, eye.fov.angleRight, eye.fov.angleUp, eye.fov.angleDown, clipperFovyDeg,
                        clipperAspect, view->near_, view->far_);
            OutputDebugStringA(msg);
        }
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
