#pragma once

// vr_main.hpp
// Declarations for the VR mod's game-loop integration points, implemented
// in vr_main.cpp. m_Do_main.cpp calls startup() once during init and
// isActive()/tick() from the main loop each frame, plus submitFrame()
// right after its own aurora_end_frame() -- see tick()/submitFrame()'s
// own comments below for why the split exists.

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
void tick();

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
