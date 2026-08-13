#pragma once

// vr_main.hpp
// Declarations for the VR mod's game-loop integration points, implemented
// in vr_main.cpp. m_Do_main.cpp calls startup() once during init and
// isActive()/tick() from the main loop each frame, plus submitFrame()
// right after its own aurora_end_frame() -- see tick()/submitFrame()'s
// own comments below for why the split exists.

#include <dolphin/types.h>     // s16 -- getHeadMoveAngleS()
#include "dusk/game_clock.h"  // dusk::game_clock::MainLoopPacer
#include "helpers/gx_helper.h"  // TGXTexObj

class J3DModel;

namespace dusk::vr {

// Call once, after an aurora::gfx device exists (see startup()'s existing
// comment in vr_main.cpp for why timing matters). Returns false on any
// XR/D3D12 setup failure -- caller should proceed without VR, not crash.
bool startup();

// True once startup() has succeeded and a session exists. This does NOT
// mean tick() is currently drawing anything -- a session can be active with
// no gameplay view yet (title/loading screens), in which case tick() pumps
// the XR frame loop but renders nothing. Use isActive() to decide whether
// to call tick() at all; use isRenderingToHeadset() (after calling tick())
// to decide whether the normal flatscreen draw should ALSO run this frame.
bool isActive();

// True if tick() actually rendered real stereo eyes into the headset during
// its most recent call this frame (i.e. isActive() was true AND a valid
// gameplay view existed). False whenever tick() only pumped the XR frame
// loop and submitted an empty frame -- title/loading screens, or isActive()
// being false. Callers should call tick() first, then check this to decide
// whether fpcM_DrawIterater/cAPIGph_Painter should ALSO run this frame:
// tick() alone does not draw anything to the flatscreen swapchain, so
// skipping that fallback whenever this is false blanks menus/video, not
// just 3D gameplay.
bool isRenderingToHeadset();

// True ONLY while a VR eye's own protected offscreen pass is actually open
// (between a given beginEye() and its matching endEye() inside tick()'s
// per-eye loop) -- unlike isRenderingToHeadset() above, which is true for
// tick()'s entire duration once a gameplay view is ready, including the
// window before the per-eye loop even starts. Render-to-texture systems
// that open their OWN GXCreateFrameBuffer pass (e.g. the minimap/map-screen,
// d_map_path.cpp's dRenderingMap_c::renderingMap()) must check this, not
// isRenderingToHeadset(), to tell "unsafe to nest a second offscreen pass
// right now" apart from "VR is active this frame but no eye pass is open
// yet" -- the latter is exactly the safe window captureHudBillboard() and
// captureMapCopy2D() (m_Do_graphic.cpp) already render into.
bool isEyePassOpen();

// Only meaningful while isRenderingToHeadset() is true (returns the last
// computed values otherwise, harmlessly stale). The smallest symmetric
// fovy/aspect frustum that fully contains the current eye's real asymmetric
// VR FOV -- the same values the actor-culling frustum (mDoLib_clipper) uses.
// For call sites that build their own fovy/aspect-based projection matrix
// (e.g. daGrdWater_c::Draw()'s reflection env-map matrix) and need a VR-
// correct substitute for view->fovy/view->aspect WITHOUT those shared
// view_class fields themselves being changed for VR -- see
// vr_stereo_render.hpp's getEyeSymmetricFov() comment for why those fields
// are deliberately left alone.
void getEyeSymmetricFov(float* fovyDeg, float* aspect);

// Draws the head-locked HUD billboard into the CURRENTLY OPEN eye pass --
// call from mDoGph_Painter()'s per-eye HUD call site (m_Do_graphic.cpp),
// after the 3D world draw, in place of the flat mDoGph_drawHud2D() call
// used on flatscreen. `hudTex` must already be populated for this frame by
// mDoGph_gInf_c::captureHudBillboard() (called once, before tick()'s per-eye
// loop -- see that function's own comment for why the ordering matters).
// Thin forward to vr_render::drawHudBillboard() (vr_stereo_render.hpp) --
// kept out of this header so callers like m_Do_graphic.cpp don't need to
// include the heavier OpenXR/aurora headers vr_stereo_render.hpp pulls in,
// same reasoning as isRenderingToHeadset()/getEyeSymmetricFov() above.
void drawHudBillboard(TGXTexObj* hudTex);

// Overwrites mpLinkHandModel's two hand joints (indices 1/2 -- al_handsL/
// al_handsR) with this frame's tracked controller poses. Call site:
// d_a_alink.cpp's setDrawHand()-adjacent draw-prep code, IMMEDIATELY AFTER
// its own existing `mpLinkHandModel->setAnmMtx(1/2, mpLinkModel->
// getAnmMtx(9/0xE))` body-joint re-sync -- that re-sync runs every eye,
// right before the model actually draws, and unconditionally overwrites
// whatever anyone wrote earlier in the frame (discovered when tracked
// hands were first wired up: writing the tracked pose from
// vr_link::updateFrame(), which runs once before the per-eye loop, had
// zero visible effect because of exactly this). Guard the call site on
// isRenderingToHeadset() -- flatscreen must keep the base game's own
// body-joint sync untouched. Thin forward to
// vr_link::applyTrackedHandMtx() (vr_link_visibility.hpp), kept out of
// this header for the same "heavier OpenXR/aurora dependency" reason
// drawHudBillboard() above is.
void applyTrackedHandMtx(J3DModel* handModel);

// ACTUAL FIX for the persistent hand-lag bug (2026-08-09) -- applyTrackedHandMtx()
// above was proven, via a full-session [dusk::vr::eyepasscheck] log capture,
// to NEVER actually run during a real VR eye pass at all (its only call
// site, inside daAlink_c::draw(), is only ever reached from the legacy
// once-per-sim-tick fapGm_Execute() path). Call this once per real frame
// instead, from tick() itself (BEFORE the per-eye loop opens -- both eyes
// share the same non-double-buffered draw-matrix slot, confirmed via
// J3DMtxBuffer's mCurrentViewNo, so one call per frame is sufficient), with
// the real daAlink_c's hand model (dComIfGp_getLinkPlayer()-> getHandModel()).
// Thin forward to vr_link::refreshTrackedHandDrawMtxLive() (vr_link_visibility.hpp)
// -- see its own comment for the full root-cause writeup and why this needs
// to also bypass frame_interp's cached interpolation, not just write a
// fresh matrix. No-op outside VR / before the first updateFrame() call.
void refreshTrackedHandDrawMtxLive(J3DModel* handModel);

// Re-points mSwordModel/mShieldModel's base transform so they track the
// real tracked hands, preserving the body rig's own relative offset
// between the HAND joint (9/0xE, what drives mpLinkHandModel) and the
// separate ITEM joint (10/0xF -- confirmed a DIFFERENT joint, not the same
// one; see vr_link::applyTrackedItemMtx()'s comment for how that was
// found and why an earlier version of this fix wrongly assumed they were
// the same), then recalculates. Call site: d_a_alink.cpp, right after
// setDrawHand() (see applyTrackedHandMtx() above for why "right after" --
// same per-eye, last-write-before-draw ordering). Either model pointer may
// be NULL (e.g. sword/shield not currently equipped) -- a no-op for that
// one.
//
// leftItemJointMtx/leftHandJointMtx (and the right-hand equivalents): the
// caller's own mpLinkModel->getAnmMtx(mLeftItemJntNo)/
// getAnmMtx(mLeftHandJntNo) (and mRightItemJntNo/mRightHandJntNo),
// evaluated THIS frame. itemJointMtx doubles as the gate -- detects
// whether setItemMatrix() actually attached this model to the hand joint
// this frame, vs. its separate belt/back-relative resting pose (an
// earlier version without this gate made sheathed/stowed sword+shield
// float at the tracked hand instead of staying put) -- and as the
// numerator of the hand-to-item relative-offset preservation described
// above; handJointMtx is the offset's denominator.
//
// Guard the call site on isRenderingToHeadset(). Thin forward to
// vr_link::applyTrackedItemMtx() (vr_link_visibility.hpp), same "keep the
// heavier OpenXR/aurora header out of core game files" reasoning as
// drawHudBillboard()/applyTrackedHandMtx() above. Takes plain
// `float (*)[4]` rather than the MtxP typedef -- vr_main.hpp deliberately
// only forward-declares J3DModel and doesn't pull in the (dolphin
// mtx.h-dependent) J3DModel.h just for this one typedef; the two types are
// identical (MtxP is `f32 (*)[4]`, f32 is `float`).
void applyTrackedItemMtx(J3DModel* swordModel, J3DModel* shieldModel,
                          float (*leftItemJointMtx)[4], float (*leftHandJointMtx)[4],
                          float (*rightItemJointMtx)[4], float (*rightHandJointMtx)[4]);

// ACTUAL FIX for sword/shield lag (2026-08-09) -- same root cause and fix
// shape as refreshTrackedHandDrawMtxLive() above: applyTrackedItemMtx()'s
// only call site (d_a_alink.cpp, inside daAlink_c::draw()) never runs
// during a real VR eye pass. Call once per real frame from tick(), before
// the per-eye loop opens. No arguments -- fetches the player and every
// matrix it needs internally (vr_link::refreshTrackedItemMtxLive(),
// vr_link_visibility.hpp), since this is a new VR-internal-only call site.
void refreshTrackedItemMtxLive();

// Extends refreshTrackedItemMtxLive() above to mHeldItemModel (bow,
// bottles, oil bottle, copy rod, boomerang, etc.) and the separate
// lantern/kantera model -- same "call once per real frame, before the
// per-eye loop opens" reasoning. Thin forward to
// vr_link::refreshTrackedHeldItemMtxLive() (vr_link_visibility.hpp) --
// see its own comment for which items this covers and which are
// deliberately excluded (hookshot, iron ball, the head-attached 0x106
// item).
void refreshTrackedHeldItemMtxLive();

// Returns the real tracked controller's world-space position (same game
// coordinate convention/units as e.g. daAlink_c::mLeftHandPos/
// mRightHandPos) via outX/outY/outZ, for VR-tracking a carried/grabbed
// actor's position source -- see setBodyPartPos()'s call site
// (d_a_alink.cpp), used there to fix a held bomb's position source.
// Returns false (leaving outX/outY/outZ untouched) if hand-tracking data
// isn't valid yet this session, so the caller can fall back to the
// flatscreen-animated joint position. Plain floats rather than cXyz,
// deliberately -- vr_main.hpp avoids pulling in core-game math headers
// just for this one type, same reasoning as applyTrackedItemMtx()'s plain
// float(*)[4] parameters above.
bool getTrackedHandWorldPos(bool isLeftHand, float& outX, float& outY, float& outZ);

// Extends held-item tracking to daAlink_c::getLeftItemMatrix()/
// getRightItemMatrix() (d_a_alink_link.inc) themselves -- fixes every
// downstream consumer of those two (virtual) accessors at once through
// ordinary virtual dispatch (nocked arrows, boomerang, fishing rod,
// several enemy-interaction actors, canoe paddle -- see
// vr_link::refreshTrackedItemJointMtxLive()'s own comment,
// vr_link_visibility.hpp, for the full list and reasoning). Call once per
// real frame from tick(), before the per-eye loop opens, same as the
// other refresh*Live() functions above.
void refreshTrackedItemJointMtxLive();

// Copies the tracked-hand-relative version of mLeftItemJntNo's/
// mRightItemJntNo's raw world matrix into outMtx (3x4, same layout as
// applyTrackedItemMtx()'s float(*)[4] params above); returns false
// (leaving outMtx untouched) if not available this frame -- caller
// (daAlink_c::getLeftItemMatrix()/getRightItemMatrix()) falls back to the
// raw joint matrix in that case.
bool getTrackedItemJointMtx(bool isLeft, float (*outMtx)[4]);

// VR fix (2026-08-12): re-runs daBoomerang_c::applyTrackedKeepTransforms()
// (the visual-transform half of setKeepMatrix(), split out for exactly
// this purpose) once per real frame -- fixes the held boomerang's own
// visible stair-stepping, on top of getLeftItemMatrix()'s own fix above
// (which fixed fishing-rod/nocked-arrow/canoe consumers, but not this one,
// since setKeepMatrix() only ever samples that matrix once per sim tick).
// See vr_link::refreshTrackedBoomerangMtxLive()'s own comment
// (vr_link_visibility.hpp) for the full gating reasoning. Call once per
// real frame from tick(), same as the other refresh*Live() functions.
void refreshTrackedBoomerangMtxLive();

// VR fix (2026-08-12): same shape as refreshTrackedBoomerangMtxLive()
// above, applied to the fishing rod -- see
// vr_link::refreshTrackedFishingRodMtxLive()'s own comment
// (vr_link_visibility.hpp) for the full reasoning. Call once per real
// frame from tick(), same as the other refresh*Live() functions.
void refreshTrackedFishingRodMtxLive();

// VR fix (2026-08-12): the clawshot's hand-grip tracking (deliberately
// scoped to the two grip models only, not the chain itself -- see
// vr_link::refreshTrackedHookshotMtxLive()'s own comment,
// vr_link_visibility.hpp, for the full reasoning and scoping). Call once
// per real frame from tick(), same as the other refresh*Live() functions.
void refreshTrackedHookshotMtxLive();

// FIXED 2026-08-08 (section 20 continuation -- "link's entire body lags
// behind... for all direction[s]" when moving): mpLinkModel's own base
// transform is only ever set once per 30Hz sim tick (daAlink_c::
// setMatrix(), called from execute()), with zero render-time smoothing --
// unlike the camera/hands, which read the smoothed+extrapolated
// getVrCameraEyeAnchor() every render frame. Nudges bodyModel's base
// transform by the exact same world-space delta the eye anchor's
// smoothing/extrapolation already applied this frame (zero outside
// first-person, where the camera doesn't get that treatment either -- see
// vr_link::getVrBodyPositionOffset()'s own comment) and recalculates, so
// the whole body stays visually rigid with the camera and tracked hands
// instead of stair-stepping behind them. Call site: d_a_alink.cpp, right
// before modelDraw(mpLinkModel, ...) in the human-form draw branch (not
// Wolf form -- getVrBodyPositionOffset() is always zero there anyway,
// since isFirstPerson() excludes Wolf form). Guard the call site on
// isRenderingToHeadset(). Thin forward to
// vr_link::applyVrBodyPositionOffset() (vr_link_visibility.hpp), same
// "keep the heavier OpenXR/aurora header out of core game files"
// reasoning as the functions above.
void applyVrBodyPositionOffset(J3DModel* bodyModel);

// The current VR smooth-turn yaw offset (vr_smooth_turn.hpp), in radians --
// 0 outside VR or before the right thumbstick has been used to turn.
// Exposed here (a plain float, no OpenXR types in the signature) so
// gameplay code like d_a_alink.cpp can fold it into movement-direction
// math without including vr_smooth_turn.hpp/vr_stereo_render.hpp directly
// -- same "thin forward, keep heavier VR headers out of core game files"
// reasoning as isRenderingToHeadset()/applyTrackedHandMtx() above. Callers
// should gate on isRenderingToHeadset() themselves if they only want this
// while actually rendering to the headset -- this always returns whatever
// is currently accumulated, VR-active or not.
float getSmoothTurnYawRad();

// The real, undamped in-game yaw (same s16 binary-angle unit as
// daAlink_c::mMoveAngle/shape_angle.y) the player's HMD is currently facing,
// including the VR smooth-turn offset above. Computed once per frame in
// tick() (0 before the first frame / outside VR). See its backing global
// g_headMoveAngleS's declaration comment in vr_main.cpp for the bug this
// exists to fix: movement direction used to be based on the flatscreen
// third-person camera's own angle (dCam_getControledAngleY()), which has no
// relationship to where the player's head is actually turned in VR.
s16 getHeadMoveAngleS();

// Real right-controller-pointing aim yaw/pitch (same s16 BAMS unit as
// daAlink_c::shape_angle.y / mBodyAngle.x), for first-person item aiming
// (bow/slingshot/hookshot/boomerang, all funneling through
// daAlink_c::setBodyAngleToCamera() -- see that function's own VR branch,
// d_a_alink_link.inc) to follow the real controller's pointing direction
// instead of stick/gyro/mouse deltas. Computed once per frame in tick(),
// same pattern as getHeadMoveAngleS() above (0/0 before the first frame or
// outside VR -- callers already gate on isRenderingToHeadset() plus the
// game's own aim-context check, so a stale zero here is harmless when
// unused). See vr_link::computeControllerAimForward()'s own comment
// (vr_link_visibility.hpp) for why the RIGHT hand is used unconditionally
// and why this sources from OpenXR's own aim pose rather than the tracked
// hand mesh's grip-based calibration.
void getControllerAimAngles(s16* outYawS, s16* outPitchS);

// Runs the first half of one VR frame: xrWaitFrame/xrBeginFrame, per-eye
// render (including the fpcM_DrawIterater/cAPIGph_Painter draw call), and
// encodes (but does not yet submit) the eye-texture copy for each eye.
// Caller must NOT also call fpcM_DrawIterater/cAPIGph_Painter for this
// frame when calling tick() -- tick() does that once per eye internally.
// Caller's normal per-frame input read (mDoCPd_c::read(), etc.),
// fapGm_Execute(), and mDoAud_Execute() should still run exactly once per
// frame as usual; only the final draw call is replaced by tick().
//
// CHANGED this session: tick() no longer does the swapchain submit or
// xrEndFrame itself -- see submitFrame() below for why and where that
// moved. Every tick() call (that renders real eyes; see isActive()/
// isRenderingToHeadset() above) must be followed by a submitFrame() call
// later the same frame, after the caller's own aurora_end_frame().
//
// FIXED this session: takes the caller's already-computed MainLoopPacer
// instead of calling dusk::game_clock::advance_main_loop() a second time.
// That function mutates shared clock state on every call (unconditionally
// stamps s_previous_sample = now, among other things) -- calling it again
// here corrupted the frame-pacing bookkeeping for every VR frame, since the
// second call always saw a near-zero elapsed time versus the first call
// m_Do_main.cpp already made a few instructions earlier. Pass the same
// pacing through instead of re-deriving (and re-mutating) it.
void tick(const dusk::game_clock::MainLoopPacer& pacing);

// Call once per frame, right after the caller's own aurora_end_frame() --
// NOT inside the aurora_begin_frame()/aurora_end_frame() pair tick() runs
// in. Finishes what tick() started: reads back each eye's copied pixels,
// uploads them into the XR swapchain image, releases the swapchain image,
// and calls xrEndFrame(). Required because tick() returns before
// aurora_end_frame() actually submits the frame's GPU work, so the copy
// tick() encodes isn't safe to read back until after that Submit() has
// run. Safe to call unconditionally every frame -- a no-op if tick()
// didn't actually render stereo eyes this frame.
void submitFrame();

}  // namespace dusk::vr
