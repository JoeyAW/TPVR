#pragma once

// vr_main.hpp
// Declarations for the VR mod's game-loop integration points, implemented
// in vr_main.cpp. m_Do_main.cpp calls startup() once during init and
// isActive()/tick() from the main loop each frame, plus submitFrame()
// right after its own aurora_end_frame() -- see tick()/submitFrame()'s
// own comments below for why the split exists.

#include "dusk/game_clock.h"  // dusk::game_clock::MainLoopPacer
#include "helpers/gx_helper.h"  // TGXTexObj

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
