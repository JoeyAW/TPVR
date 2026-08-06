// vr_main.cpp
// Game loop integration for the Dusklight VR mod.
//
// Built against the real APIs in vr_swing_detector.hpp, vr_link_visibility.hpp,
// and vr_stereo_render.hpp (vr_combat::, vr_link::, vr_render:: namespaces) --
// not guessed names. Items marked TODO depend on vr_xr_bootstrap.hpp internals
// (grip/view XrSpace handles) or on the m_Do_main.cpp integration points
// (aurora_begin_frame/aurora_end_frame call sites, confirmed swapchain format)
// that haven't been shown yet.
//
// Also requires edits to src/m_Do/m_Do_main.cpp to call dusk::vr::startup()
// once at init and dusk::vr::tick() from the main loop each frame -- see
// VR_MOD_HANDOFF_2.md. Neither call site exists yet.

#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <dolphin/pad.h>  // PADStatus, PADSetVirtualStatus/PADClearVirtualStatus -- gameplay controller input

#include "SSystem/SComponent/c_API_graphic.h"  // cAPIGph_Painter
#include "m_Do/m_Do_graphic.h"                  // mDoGph_gInf_c::captureHudBillboard
#include "f_pc/f_pc_manager.h"                  // fpcM_DrawIterater, fpcM_Draw
#include "dusk/game_clock.h"                    // dusk::game_clock::MainLoopPacer

#include "dusk/vr/vr_xr_bootstrap.hpp"
#include "dusk/vr/vr_stereo_render.hpp"         // vr_render::
#include "dusk/vr/vr_swing_detector.hpp"        // vr_combat::
#include "dusk/vr/vr_link_visibility.hpp"       // vr_link::
#include "dusk/vr/vr_xr_submit.hpp"             // dusk::vr::Session
#include "dusk/vr/vr_main.hpp"

// TEMP DIAGNOSTIC (VR black-screen-after-save investigation): plain,
// unmangled, non-namespaced global mirroring g_renderedToHeadsetThisFrame
// below, purely so it can be used as a Visual Studio conditional-breakpoint
// expression from OTHER translation units (extern/aurora's GXFrameBuffer.cpp)
// without fighting anonymous-namespace name mangling in the debugger's
// expression evaluator.
extern "C" bool g_duskVRRenderingToHeadset = false;
// TEMP DIAGNOSTIC (VR heat-wave "floating portal" investigation): unlike
// g_duskVRRenderingToHeadset above (only true during the narrow per-eye
// draw window inside tick(), which runs AFTER game-logic update in the
// frame), this stays true for the whole VR session lifetime (mirrors
// isActive()/g_session != nullptr). Particle spawn calls happen during
// actor update, not draw, so gating a spawn-time log on the per-eye flag
// risks a several-frame lag or missing the window entirely depending on
// update/draw ordering -- this is the reliable one to gate spawn-time
// diagnostics on. extern "C" for the same reason as above: usable from
// other translation units without namespace/mangling issues.
extern "C" bool g_duskVRSessionActive = false;
// TEMP DIAGNOSTIC (VR water-black investigation): which eye (0=left,
// 1=right) is currently being drawn, set right before beginEye() each
// iteration of the per-eye loop below. Same extern "C" pattern as
// g_duskVRRenderingToHeadset, for the same reason (usable from
// extern/aurora's gx.cpp without namespace/mangling issues).
extern "C" uint32_t g_duskVRCurrentEyeIndex = 0;
// NEW this session (minimap black-screen investigation): unlike
// g_duskVRRenderingToHeadset above -- which, despite its old comment
// claiming otherwise, is actually true for the ENTIRE tick() call once a
// gameplay view is ready, not just within an open eye pass -- this is true
// ONLY between a given beginEye() and its matching endEye(). Needed because
// some render-to-texture systems (the minimap/map-screen's own
// GXCreateFrameBuffer pass, d_map_path.cpp's dRenderingMap_c::renderingMap())
// are safe to run in the window AFTER isViewReady() but BEFORE the per-eye
// loop opens an eye's protected offscreen pass (same safe window
// captureHudBillboard() already uses), but NOT safe while nested inside an
// actual open eye pass -- g_duskVRRenderingToHeadset can't distinguish those
// two cases, only this can.
extern "C" bool g_duskVREyePassOpen = false;

namespace dusk::vr {

namespace {
Session* g_session = nullptr;
std::unique_ptr<Session> g_ownedSession;  // vr_main.cpp owns the Session; g_session just points to it
vr_combat::SwingDetector g_rightSwing;

// NEW this session: needed so tick()'s event pump (below) can call
// xrPollEvent without needing Session to expose its private instance_.
// Set once in startup() alongside g_ownedSession.
XrInstance g_xrInstance = XR_NULL_HANDLE;

// ROOT-CAUSED this session ("VR stops updating after creating a save file"):
// true once xrBeginSession() has actually succeeded and false once the
// runtime has told us to stop (XR_SESSION_STATE_STOPPING) -- distinct from
// g_session being null, which now only happens on genuine teardown
// (EXITING/LOSS_PENDING). Lets tick() keep polling events and resume
// rendering when READY comes back around, instead of the old behaviour of
// nulling g_session on STOPPING and never touching the session again.
bool g_sessionRunning = false;

// TEMP DIAGNOSTIC (VR black-screen-after-save investigation continued --
// pacing.is_interpolating stayed true throughout, ruling that theory out).
// Logs the reason tick() took each frame ONLY when it changes from the
// previous frame, so a single test run pinpoints exactly which exit path
// starts firing persistently once the headset goes black after a save,
// without spamming every frame in the common/expected case.
const char* g_lastTickReason = "";
void logTickReasonOnChange(const char* reason) {
    if (std::strcmp(g_lastTickReason, reason) == 0) {
        return;
    }
    g_lastTickReason = reason;
    char msg[128];
    _snprintf_s(msg, _TRUNCATE, "[dusk::vr::tick] reason -> %s\n", reason);
    OutputDebugStringA(msg);
}

// NEW this session: backs isRenderingToHeadset(). tick() has several early-
// return paths (no session, session just went STOPPING/EXITING, xrWaitFrame/
// xrBeginFrame failure, shouldRender==false, no ready gameplay view) where it
// submits an empty XR frame or does nothing at all -- none of those actually
// draw stereo eyes. Reset to false at the top of every tick() call and only
// set true right before the per-eye draw loop runs, so the flag always
// reflects what actually happened THIS frame, not a stale previous value.
bool g_renderedToHeadsetThisFrame = false;

// NEW this session: carries per-frame state from tick() across the gap to
// submitFrame() (called separately, after m_Do_main.cpp's aurora_end_frame()
// -- see submitFrame()'s own comment for why the split exists). Only
// meaningful when g_hasPendingFrameSubmit is true; tick() sets both, and
// submitFrame() clears the flag once it's consumed them.
bool g_hasPendingFrameSubmit = false;
struct PendingEyeReadback {
    // NEW this session (VR_MOD_HANDOFF_10 follow-up, option (c)): false
    // when endEye() returned an empty ResolvedTargets (foreign pass
    // substitution detected -- see vr_stereo_render.hpp's endEye()
    // comment). submitFrame() skips readbackEyeCopy() for any eye where
    // this is false, since encodeEyeCopy() was never called for it this
    // frame either -- there's nothing new in its readback buffer to
    // upload, and the swapchain image simply keeps last frame's content
    // for that eye rather than showing corrupt/wrong-sized data.
    bool valid = false;
    uint32_t eyeIndex = 0;
    uint32_t swapchainIndex = 0;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    uint32_t dstXOffset = 0;
};
struct PendingFrameSubmit {
    std::vector<PendingEyeReadback> eyes;
    std::vector<XrCompositionLayerProjectionView> projViews;
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrSpace base = XR_NULL_HANDLE;
    uint32_t viewCount = 0;
};
PendingFrameSubmit g_pendingSubmit;

// FIXED this session: these now come from real xrCreateActionSpace calls
// (vr_xr_bootstrap.hpp's createHandActionSet()/attachAndCreateHandSpaces(),
// called from startup() below) instead of staying XR_NULL_HANDLE forever --
// same fix pattern already applied to g_viewSpace. g_handActionSet must be
// synced via xrSyncActions() once per frame (tick(), right before these
// spaces are located) for their poses to update at all.
XrSpace g_rightGripSpace = XR_NULL_HANDLE;
XrSpace g_leftGripSpace = XR_NULL_HANDLE;
// NEW (rotation-calibration follow-up): aim-pose action spaces, alongside
// the grip spaces above -- see vr_xr_bootstrap.hpp's HandActions::
// aimPoseAction comment for why. Located every frame the same way the grip
// spaces are, purely for calibration logging right now (not fed into
// buildHandMtx()/the actual draw pose).
XrSpace g_rightAimSpace = XR_NULL_HANDLE;
XrSpace g_leftAimSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
XrActionSet g_handActionSet = XR_NULL_HANDLE;

// NEW (gameplay controller input, 2026-08-03): the button/axis actions
// created alongside the pose actions above (see vr_xr_bootstrap.hpp's
// HandActions), plus the left/right subaction paths needed to query each
// hand's state out of them. Synced by the same xrSyncActions() call as the
// pose actions (same action set) -- see tick()'s per-frame sync below.
XrAction g_triggerValueAction = XR_NULL_HANDLE;
XrAction g_squeezeValueAction = XR_NULL_HANDLE;
XrAction g_thumbstickAction = XR_NULL_HANDLE;
XrAction g_primaryClickAction = XR_NULL_HANDLE;
XrAction g_secondaryClickAction = XR_NULL_HANDLE;
XrAction g_menuClickAction = XR_NULL_HANDLE;
// NEW (2026-08-04, per explicit user request "make the right stick click
// the pause menu"): right thumbstick click, additionally OR'd into
// PAD_BUTTON_START in tick() below -- see vr_xr_bootstrap.hpp's
// HandActions::stickClickAction.
XrAction g_stickClickAction = XR_NULL_HANDLE;
XrPath g_leftHandPath = XR_NULL_PATH;
XrPath g_rightHandPath = XR_NULL_PATH;

vr_render::HandDrawState g_handDrawState;

// NEW this session. Root cause of the startup() hang: xrCreateSession()
// alone does not start a session per the OpenXR spec -- the runtime must
// deliver an XrEventDataSessionStateChanged event with state READY via
// xrPollEvent before xrBeginSession() is legal to call. No event pump
// existed anywhere in this codebase before now, so nothing was ever
// polling for that event; downstream calls (createSwapchain succeeding is
// plausible, but xrWaitFrame in tick() would then block forever waiting on
// a session that was never begun) had no way to make progress.
//
// Bounded (kMaxAttempts * ~16ms sleep, so a few seconds) rather than a true
// infinite wait: a runtime that genuinely never sends READY should surface
// as a diagnosable startup() failure, not hang the process again under a
// different disguise.
bool waitForSessionReadyAndBegin(XrInstance instance, XrSession session) {
    constexpr int kMaxAttempts = 300;  // ~5s at the 16ms sleep below
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(instance, &event);

        if (result == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& stateEvent =
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);

                char msg[128];
                _snprintf_s(msg, _TRUNCATE,
                            "[dusk::vr::startup] session state -> %d\n",
                            static_cast<int>(stateEvent.state));
                OutputDebugStringA(msg);

                if (stateEvent.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    return XR_SUCCEEDED(xrBeginSession(session, &beginInfo));
                }
                if (stateEvent.state == XR_SESSION_STATE_LOSS_PENDING ||
                    stateEvent.state == XR_SESSION_STATE_EXITING) {
                    // Runtime is telling us to give up before we ever got
                    // going -- don't keep waiting for a READY that won't come.
                    return false;
                }
            }
            // Other event types (reference space change pending, instance
            // loss pending, etc.) intentionally ignored here -- this is just
            // the startup gate. tick()'s own poll loop (below) is the place
            // for ongoing handling once the session is running.
            continue;
        }

        if (result == XR_EVENT_UNAVAILABLE) {
            // No event pending yet -- give the runtime a moment and retry.
            Sleep(16);
            continue;
        }

        // Any other XrResult from xrPollEvent is itself a hard failure.
        return false;
    }
    return false;  // gave up after kMaxAttempts without reaching READY
}
}  // namespace

void initSession(Session* session) {
    g_session = session;
    g_sessionRunning = true;
    g_duskVRSessionActive = true;
    // TODO: call once, after an aurora::gfx device exists:
    // g_handDrawState.typeId = aurora::gfx::register_draw_type(vr_render::handDrawDescriptor());
}

bool isActive() {
    return g_session != nullptr;
}

bool isRenderingToHeadset() {
    return g_renderedToHeadsetThisFrame;
}

bool isEyePassOpen() {
    return g_duskVREyePassOpen;
}

void getEyeSymmetricFov(float* fovyDeg, float* aspect) {
    vr_render::getEyeSymmetricFov(fovyDeg, aspect);
}

void drawHudBillboard(TGXTexObj* hudTex) {
    vr_render::drawHudBillboard(hudTex);
}

void applyTrackedHandMtx(J3DModel* handModel) {
    vr_link::applyTrackedHandMtx(handModel);
}

// Call once at startup, after an aurora::gfx device exists (per the
// existing TODO in initSession() -- same timing requirement applies here).
// Returns false on any XR/D3D12 setup failure; caller should proceed
// without VR rather than crash. NOT YET CALLED from anywhere -- still
// needs a call site in m_Do_main.cpp's init path.
bool startup() {
    // TEMP DIAGNOSTIC (v8, remove once confirmed working): confirms startup()
    // is actually being called from m_Do_main.cpp at all -- I've only seen the
    // v7 handoff's description of that call site, never the file itself, so
    // this closes that gap rather than assuming it's wired correctly.
    OutputDebugStringA("[dusk::vr::startup] called\n");
    try {
        vr_xr::Bootstrap boot = vr_xr::initialize();
        vr_xr::XrGraphicsDevice gfx = vr_xr::createXrGraphicsDevice(boot);

        XrSpace localSpace = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;
        XrSession session = vr_xr::createXrSession(boot, gfx, &localSpace, &viewSpace);
        // FIXED this session: g_viewSpace used to stay XR_NULL_HANDLE for the
        // whole session (nothing ever assigned it) -- see createXrSession's
        // updated comment. Real head tracking now flows into hmdPose in
        // tick() below.
        g_viewSpace = viewSpace;

        // FIXED this session: g_rightGripSpace/g_leftGripSpace had the exact
        // same problem as g_viewSpace did before it (see above) -- nothing
        // ever created an action set, let alone attached it or created
        // action spaces from it, so both stayed XR_NULL_HANDLE and
        // vr_link::buildHandMtx() rendered hands at tracking-space origin
        // (see the TODO that used to sit above their declaration).
        vr_xr::HandActions handActions = vr_xr::createHandActionSet(boot.instance);
        vr_xr::attachAndCreateHandSpaces(session, handActions, &g_leftGripSpace, &g_rightGripSpace,
                                          &g_leftAimSpace, &g_rightAimSpace);
        g_handActionSet = handActions.actionSet;

        // NEW (gameplay controller input): the new button/axis actions live
        // in the same action set attached above, so no separate attach call
        // is needed -- just carry the handles forward for tick() to query.
        g_triggerValueAction = handActions.triggerValueAction;
        g_squeezeValueAction = handActions.squeezeValueAction;
        g_thumbstickAction = handActions.thumbstickAction;
        g_primaryClickAction = handActions.primaryClickAction;
        g_secondaryClickAction = handActions.secondaryClickAction;
        g_menuClickAction = handActions.menuClickAction;
        g_stickClickAction = handActions.stickClickAction;
        g_leftHandPath = handActions.leftHandPath;
        g_rightHandPath = handActions.rightHandPath;

        g_ownedSession = std::make_unique<Session>(boot.instance, boot.systemId, session, localSpace, gfx.device, gfx.commandQueue);
        // Registers the encoder task type backing encodeEyeCopy()'s Dawn-side
        // copy (see vr_xr_submit.hpp's Session::registerCpuCopyEncoderTask).
        // Must happen before the first endEye()/encodeEyeCopy() call, which
        // this satisfies since tick() can't run until g_session is set below.
        g_ownedSession->registerCpuCopyEncoderTask();
        // NOTE: initSession() (which sets g_session, and therefore isActive())
        // is deliberately NOT called here yet -- see below. It used to be
        // called immediately after construction, which meant isActive() could
        // report true even if the session never actually began (e.g. this
        // session's waitForSessionReadyAndBegin() timing out stuck at IDLE).
        // tick() would then run against a session xrBeginSession was never
        // called on -- an illegal call sequence per the OpenXR spec, and the
        // likely cause of the crash. g_session now only gets set once we've
        // confirmed xrBeginSession() actually succeeded, below.

        // One-swapchain-vs-per-eye decision: ONE double-wide swapchain shared by
        // both eyes (arraySize=1, width = 2x per-eye recommended width), rather
        // than two separate XrSwapchain handles. This matches what tick() already
        // assumed (single g_session->swapchain() handle, imageArrayIndex=0 for
        // both eyes) -- per-eye swapchains would need that call site reworked too.
        // tick()'s per-eye imageRect offset is updated below to match (right eye
        // occupies the second half of the double-wide image).
        std::vector<XrViewConfigurationView> configViews =
            g_ownedSession->enumerateViewConfigurationViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
        if (configViews.empty()) {
            // TEMP DIAGNOSTIC (v8, remove once startup() is confirmed working
            // in-headset): view configuration enumeration returned no eyes at
            // all -- suggests the runtime/system wasn't ready, not a format or
            // swapchain problem specifically. Visible via DebugView (Sysinternals)
            // or a debugger's Output window; not routed to real DuskLog since
            // that call site still isn't confirmed (see catch block below).
            OutputDebugStringA("[dusk::vr::startup] FAILED: enumerateViewConfigurationViews returned 0 views\n");
            return false;
        }
        // Per-eye recommended size is assumed identical across both eyes here
        // (true for every real HMD OpenXR runtime reports today) -- using
        // configViews[0] for both.
        const uint32_t eyeWidth = configViews[0].recommendedImageRectWidth;
        const uint32_t eyeHeight = configViews[0].recommendedImageRectHeight;

        // Real pixel format, cross-checked against Aurora's actual color target
        // instead of the previously-assumed RGBA8Unorm.
        const int64_t dxgiFormat = toDxgiSwapchainFormat(aurora::gfx::color_format());

        if (!g_ownedSession->createSwapchain(eyeWidth * 2, eyeHeight, dxgiFormat)) {
            // TEMP DIAGNOSTIC (v8, remove once confirmed working): xrCreateSwapchain
            // itself failed (wrong format, wrong size, or runtime rejected it) --
            // distinct from the "0 views" case above and from the catch block's
            // "threw before getting this far" case.
            char msg[256];
            _snprintf_s(msg, _TRUNCATE,
                        "[dusk::vr::startup] FAILED: createSwapchain(%u, %u, dxgiFormat=%lld) returned false\n",
                        eyeWidth * 2, eyeHeight, static_cast<long long>(dxgiFormat));
            OutputDebugStringA(msg);
            return false;
        }

        // NEW this session -- the actual fix for the hang: xrCreateSession()
        // alone doesn't start a session. Block (boundedly) here until the
        // runtime reports READY and xrBeginSession() succeeds, so that by
        // the time startup() returns true, the session is genuinely running
        // and tick()'s xrWaitFrame() has something to wait on.
        if (!waitForSessionReadyAndBegin(boot.instance, g_ownedSession->session())) {
            OutputDebugStringA(
                "[dusk::vr::startup] FAILED: session never reached READY / "
                "xrBeginSession failed (see session-state log lines above)\n");
            return false;
        }

        // Only now -- session confirmed actually begun -- do we let
        // isActive() start reporting true and expose the instance handle
        // tick()'s event pump needs.
        initSession(g_ownedSession.get());
        g_xrInstance = boot.instance;

        // TEMP DIAGNOSTIC (v8, remove once confirmed working): success case,
        // to distinguish "startup() ran and succeeded" from "startup() was
        // never called at all" -- both currently look identical from the
        // outside (grey screen, no failure log). Prints the actual swapchain
        // dimensions/format used so we can also sanity-check those.
        {
            char msg[256];
            _snprintf_s(msg, _TRUNCATE,
                        "[dusk::vr::startup] SUCCEEDED: swapchain %ux%u dxgiFormat=%lld\n",
                        eyeWidth * 2, eyeHeight, static_cast<long long>(dxgiFormat));
            OutputDebugStringA(msg);
        }

        return true;
    } catch (const std::exception& e) {
        // TEMP DIAGNOSTIC (v8, remove once startup() is confirmed working
        // in-headset): catches everything upstream, including
        // toDxgiSwapchainFormat() throwing on an aurora::gfx::color_format()
        // value not yet in its switch, or any OpenXR/D3D12 setup call
        // (xrCreateInstance, D3D12CreateDevice, xrCreateSession, etc.)
        // failing via vr_xr::checkResult(). Not routed to real DuskLog --
        // that call site still isn't confirmed, per the original TODO here.
        char msg[512];
        _snprintf_s(msg, _TRUNCATE, "[dusk::vr::startup] EXCEPTION: %s\n", e.what());
        OutputDebugStringA(msg);
        return false;
    }
}

static XrPosef locateSpace(XrSpace space, XrSpace base, XrTime time,
                            XrSpaceLocationFlags* outFlags = nullptr) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (space == XR_NULL_HANDLE) {
        if (outFlags) *outFlags = 0;
        return XrPosef{{0, 0, 0, 1}, {0, 0, 0}};  // identity fallback
    }
    xrLocateSpace(space, base, time, &loc);
    if (outFlags) *outFlags = loc.locationFlags;
    return loc.pose;
}

void tick(const dusk::game_clock::MainLoopPacer& pacing) {
    // Reset up front: every early-return below (no session, session just
    // stopped, XR wait/begin failure, shouldRender==false, no ready
    // gameplay view) means no stereo draw happened this frame. Only the
    // per-eye loop further down flips this true.
    g_renderedToHeadsetThisFrame = false;
    g_duskVRRenderingToHeadset = false;
    g_duskVREyePassOpen = false;

    if (!g_session) {
        logTickReasonOnChange("no-session");
        return;
    }

    // Pump XR events every frame, not just once at startup. A session can
    // transition state mid-session (headset taken off / system overlay
    // taking focus -> STOPPING, resumable; runtime shutting down -> EXITING
    // / LOSS_PENDING, not resumable). STOPPING calls xrEndSession() per spec
    // and marks the session not-running but keeps it alive so a later READY
    // can resume it (see the STOPPING/READY cases below); only genuine
    // teardown (EXITING/LOSS_PENDING) clears g_session so isActive() goes
    // false and the caller falls back permanently to the flatscreen path.
    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(g_xrInstance, &event) != XR_SUCCESS) {
            break;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& stateEvent =
                *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (stateEvent.state == XR_SESSION_STATE_STOPPING) {
                // FIXED this session: STOPPING is not permanent per the
                // OpenXR spec -- it just means "stop submitting frames and
                // call xrEndSession() for now", e.g. a system
                // overlay/dashboard taking focus (plausibly triggered by the
                // save-file UI's hitch). Used to null g_session here, which
                // made the `if (!g_session) return;` above stop this whole
                // loop from ever running again -- so a later READY was never
                // even seen and VR died silently while the flatscreen path
                // kept going untouched. Now we just mark it stopped and keep
                // the session/swapchain alive so READY (below) can resume it.
                xrEndSession(g_session->session());
                g_sessionRunning = false;
            } else if (stateEvent.state == XR_SESSION_STATE_READY) {
                // Mirror of waitForSessionReadyAndBegin()'s startup case --
                // the runtime wants us to (re)begin rendering.
                if (!g_sessionRunning) {
                    XrSessionBeginInfo resumeInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    resumeInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    g_sessionRunning =
                        XR_SUCCEEDED(xrBeginSession(g_session->session(), &resumeInfo));
                }
            } else if (stateEvent.state == XR_SESSION_STATE_EXITING ||
                       stateEvent.state == XR_SESSION_STATE_LOSS_PENDING) {
                // Genuine teardown -- the runtime is not coming back for
                // this session.
                g_session = nullptr;
                g_sessionRunning = false;
                g_duskVRSessionActive = false;
                return;
            }
        }
    }

    if (!g_session || !g_sessionRunning) {
        // Session exists but is currently stopped (between STOPPING and the
        // next READY) -- nothing to render this frame, but keep coming back
        // so the event pump above keeps running and can see READY.
        logTickReasonOnChange("session-not-running");
        return;
    }

    // --- wait for the runtime to tell us the predicted display time for this frame ---
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session->session(), &waitInfo, &frameState))) {
        logTickReasonOnChange("xrWaitFrame-failed");
        OutputDebugStringA("[dusk::vr::tick] FAILED: xrWaitFrame\n");
        return;
    }
    g_session->setFrameState(frameState);

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session->session(), &beginInfo))) {
        logTickReasonOnChange("xrBeginFrame-failed");
        OutputDebugStringA("[dusk::vr::tick] FAILED: xrBeginFrame\n");
        return;
    }

    // shouldRender: runtime may ask us to skip rendering (e.g. headset not worn)
    // but we must still call xrEndFrame with layerCount=0 to keep the frame loop alive.
    if (!frameState.shouldRender) {
        logTickReasonOnChange("shouldRender-false");
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(g_session->session(), &endInfo);
        return;
    }

    // NEW this session -- the actual crash fix: dComIfGd_getView() can
    // legitimately return nullptr when tick() runs before real gameplay has
    // started (title/loading screen). beginEye() used to guard this with
    // assert(), which is compiled out under RelWithDebInfo's NDEBUG, so it
    // was crashing on a null write instead of catching it. Same
    // empty-frame pattern as the shouldRender==false case above -- keep
    // the XR frame loop alive, just don't render anything yet.
    if (!vr_render::isViewReady()) {
        logTickReasonOnChange("view-not-ready");
        // TEMP DIAGNOSTIC (v8, remove once confirmed working): log once so
        // we can confirm this is really what's happening rather than
        // inferring it from the crash site alone.
        static bool loggedOnce = false;
        if (!loggedOnce) {
            OutputDebugStringA("[dusk::vr::tick] view not ready yet (dComIfGd_getView() == nullptr) -- skipping VR render this frame\n");
            loggedOnce = true;
        }
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(g_session->session(), &endInfo);
        return;
    }

    // Past this point isViewReady() already returned true (see the check
    // above), so this frame really is drawing stereo eyes into the headset.
    logTickReasonOnChange("rendering-normally");
    g_renderedToHeadsetThisFrame = true;
    g_duskVRRenderingToHeadset = true;

    const XrTime time = g_session->predictedDisplayTime();
    const XrSpace base = g_session->localSpace();

    // Must run once per frame before locating the grip spaces below -- an
    // action's/action space's pose is only as fresh as the last
    // xrSyncActions call. Failure (e.g. XR_SESSION_NOT_FOCUSED, briefly
    // possible right after startup) isn't fatal here: locateSpace() already
    // has no special handling for a stale/untracked pose, same as it's
    // always had for g_viewSpace.
    if (g_handActionSet != XR_NULL_HANDLE) {
        XrActiveActionSet activeSet{g_handActionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        xrSyncActions(g_session->session(), &syncInfo);
    }

    const XrPosef hmdPose = locateSpace(g_viewSpace, base, time);
    const XrPosef rightPose = locateSpace(g_rightGripSpace, base, time);
    const XrPosef leftPose = locateSpace(g_leftGripSpace, base, time);
    const XrPosef rightAimPose = locateSpace(g_rightAimSpace, base, time);
    const XrPosef leftAimPose = locateSpace(g_leftAimSpace, base, time);

    // --- gameplay controller input (buttons/axes -> PADStatus) ---
    // NEW (2026-08-03, per explicit user request "set up the quest 3
    // controllers as an actual controller"): reads the button/axis actions
    // synced above and merges them into the game's normal pad pipeline via
    // PADSetVirtualStatus() -- the same mechanism the existing touch-screen
    // overlay already uses (see touch_controls.cpp's TouchControls::
    // sync_virtual_input()). PADRead() (extern/aurora/lib/dolphin/pad/
    // pad.cpp) merges whatever's set here into the real controller-port
    // status every frame, so mDoCPd_c::getTrigA/getHoldX/getStickX(...) --
    // what d_a_alink.cpp and the rest of gameplay actually read -- see it
    // with zero actor-code changes.
    //
    // KNOWN LATENCY: mDoCPd_c::read() runs earlier in the frame, inside the
    // sim-tick loop in m_Do_main.cpp, BEFORE dusk::vr::tick() (this
    // function). Whatever's set here isn't consumed until next frame's sim
    // tick(s) -- one frame (~11-16ms at typical VR refresh rates) of extra
    // input latency, not perceptible for buttons/movement. Not addressed
    // here; would need restructuring where OpenXR input is read relative to
    // the main loop's phases, bigger than this pass's scope.
    //
    // KNOWN OVERLAP: touch_controls.cpp's virtual pad also targets
    // PAD_CHAN0 (same port PAD_1/gameplay reads) -- if the touch overlay AND
    // VR were both actively injecting input the same frame, whichever runs
    // later in the frame wins rather than merging. Not a real-world
    // scenario (can't touch a screen overlay while wearing a headset) so
    // not guarded against here.
    //
    // Mapping -- REVISED 2026-08-04 per explicit user request ("bind X to
    // right squeeze, Y to right trigger, DPAD up to Y, DPAD left to X, and
    // Z to left stick click"), superseding this comment block's previous
    // version (still visible in git history). No longer mirrors the
    // default Xbox-controller layout 1:1 -- see the individual entries
    // below for what moved where and why:
    //   left thumbstick  -> main stick (movement) -- unchanged
    //   right thumbstick -> C-stick (camera) -- unchanged
    //   left trigger     -> analog L -- unchanged
    //   right A click    -> A (context action) -- unchanged
    //   right B click    -> B (attack) -- unchanged -- ALSO triggered by
    //                        the swing-gesture detector below, so swinging
    //                        the controller like a sword is an additional
    //                        way to attack, not instead of the button
    //   left menu click  -> Start (pause) -- unchanged
    //   right stick click -> Start (pause) -- unchanged (added 2026-08-04,
    //                        earlier same day as this revision)
    //   right squeeze    -> X (was Z)
    //   right trigger    -> Y, digital only -- no analog value written
    //                        anymore, unlike the old analog-R mapping this
    //                        replaced (was analog R/raise shield; R is now
    //                        UNBOUND -- user's stated plan is to eventually
    //                        replace it with a physical movement gesture
    //                        instead of a button, not yet implemented)
    //   left X click     -> D-pad right (was D-pad left as of the revision
    //                        above, was X before that -- changed again
    //                        same day per explicit user request "change x
    //                        dpad left to dpad right")
    //   left Y click     -> D-pad up (was Y)
    //   left stick click -> Z (was D-pad right, added earlier the same day
    //                        as this revision -- D-pad right is no longer
    //                        UNBOUND as of the change above, it's now fed
    //                        by left X click instead)
    auto getFloatAction = [&](XrAction action, XrPath subactionPath) -> float {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subactionPath;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(g_session->session(), &info, &state);
        return state.isActive ? state.currentState : 0.f;
    };
    auto getBoolAction = [&](XrAction action, XrPath subactionPath) -> bool {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subactionPath;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(g_session->session(), &info, &state);
        return state.isActive && state.currentState;
    };
    auto getVec2Action = [&](XrAction action, XrPath subactionPath) -> XrVector2f {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subactionPath;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(g_session->session(), &info, &state);
        return state.isActive ? state.currentState : XrVector2f{0.f, 0.f};
    };

    const float leftTrigger = getFloatAction(g_triggerValueAction, g_leftHandPath);
    const float rightTrigger = getFloatAction(g_triggerValueAction, g_rightHandPath);
    const float rightSqueeze = getFloatAction(g_squeezeValueAction, g_rightHandPath);
    const XrVector2f leftStick = getVec2Action(g_thumbstickAction, g_leftHandPath);
    const XrVector2f rightStick = getVec2Action(g_thumbstickAction, g_rightHandPath);
    const bool rightAHeld = getBoolAction(g_primaryClickAction, g_rightHandPath);
    const bool leftXHeld = getBoolAction(g_primaryClickAction, g_leftHandPath);
    const bool rightBHeld = getBoolAction(g_secondaryClickAction, g_rightHandPath);
    const bool leftYHeld = getBoolAction(g_secondaryClickAction, g_leftHandPath);
    const bool leftMenuHeld = getBoolAction(g_menuClickAction, g_leftHandPath);
    const bool rightStickClickHeld = getBoolAction(g_stickClickAction, g_rightHandPath);
    const bool leftStickClickHeld = getBoolAction(g_stickClickAction, g_leftHandPath);

    // Swing-gesture -> attack: DEFERRED per explicit user request 2026-08-03
    // ("remove the swing controls for now, that's something for another
    // session") -- was wired here (see git history: fixed a dead
    // `!pacing.is_interpolating` gate that meant g_rightSwing.update() had
    // never actually run, then OR'd swingEvent.triggered into PAD_BUTTON_B
    // alongside the real B button), but pulled back out without having been
    // tested/tuned yet. g_rightSwing (vr_combat::SwingDetector, declared
    // above) and vr_swing_detector.hpp are left in place, just not called
    // from here -- real infrastructure to pick back up later, not dead code
    // to re-derive from scratch.

    PADStatus padStatus{};
    padStatus.err = PAD_ERR_NONE;
    if (rightAHeld) padStatus.button |= PAD_BUTTON_A;
    if (rightBHeld) padStatus.button |= PAD_BUTTON_B;
    if (leftXHeld) padStatus.button |= PAD_BUTTON_RIGHT;  // D-pad right (was D-pad left, was X)
    if (leftYHeld) padStatus.button |= PAD_BUTTON_UP;     // D-pad up (was Y)
    if (leftMenuHeld || rightStickClickHeld) padStatus.button |= PAD_BUTTON_START;
    if (leftStickClickHeld) padStatus.button |= PAD_TRIGGER_Z;  // was D-pad right (superseded above, now Z)

    // Deadzones/thresholds matched to typical thumbstick/trigger/squeeze
    // noise floors -- avoids a resting controller reporting a tiny nonzero
    // value that would make wantsVirtualPad below think input is present
    // every frame.
    constexpr float kTriggerDeadzone = 0.1f;
    constexpr float kSqueezeThreshold = 0.5f;
    constexpr float kStickDeadzone = 0.15f;
    if (rightSqueeze > kSqueezeThreshold) padStatus.button |= PAD_BUTTON_X;  // was Z
    if (rightTrigger > kTriggerDeadzone) padStatus.button |= PAD_BUTTON_Y;   // was analog R; R is now unbound
    if (leftTrigger > kTriggerDeadzone) {
        padStatus.button |= PAD_TRIGGER_L;
        padStatus.triggerLeft = static_cast<u8>(std::clamp(leftTrigger, 0.f, 1.f) * 255.f);
    }
    if (std::abs(leftStick.x) > kStickDeadzone || std::abs(leftStick.y) > kStickDeadzone) {
        padStatus.stickX = static_cast<s8>(std::clamp(leftStick.x, -1.f, 1.f) * 127.f);
        padStatus.stickY = static_cast<s8>(std::clamp(leftStick.y, -1.f, 1.f) * 127.f);
    }
    if (std::abs(rightStick.x) > kStickDeadzone || std::abs(rightStick.y) > kStickDeadzone) {
        padStatus.substickX = static_cast<s8>(std::clamp(rightStick.x, -1.f, 1.f) * 127.f);
        padStatus.substickY = static_cast<s8>(std::clamp(rightStick.y, -1.f, 1.f) * 127.f);
    }

    constexpr u32 kVrPadPort = PAD_CHAN0;
    const bool wantsVirtualPad = padStatus.button != 0 || padStatus.stickX != 0 ||
                                  padStatus.stickY != 0 || padStatus.substickX != 0 ||
                                  padStatus.substickY != 0 || padStatus.triggerLeft != 0 ||
                                  padStatus.triggerRight != 0;
    if (wantsVirtualPad) {
        PADSetVirtualStatus(kVrPadPort, &padStatus);
    } else {
        PADClearVirtualStatus(kVrPadPort);
    }

    // --- Link head hide + hand matrix mapping ---
    vr_link::FrameInput frameInput{hmdPose, rightPose, leftPose, rightAimPose, leftAimPose};
    vr_link::updateFrame(frameInput);

    // World-space point both eyes anchor their view matrix to this frame --
    // see vr_link::getVrCameraEyeAnchor()'s comment. Computed once (not per
    // eye) since it doesn't depend on which eye is rendering, only on
    // Link's/the camera's state this frame. dComIfGd_getView() is guaranteed
    // non-null here: isViewReady() already returned before reaching this
    // point (see the check above).
    view_class* currentView = dComIfGd_getView();
    const cXyz vrCameraEyeAnchor = vr_link::getVrCameraEyeAnchor(currentView->lookat.eye);

    // --- locate both eyes for this frame ---
    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = time;
    locateInfo.space = base;

    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    xrLocateViews(g_session->session(), &locateInfo, &viewState, 0, &viewCount, nullptr);

    std::vector<XrView> views(viewCount, XrView{XR_TYPE_VIEW});
    xrLocateViews(g_session->session(), &locateInfo, &viewState, viewCount, &viewCount,
                  views.data());

    std::vector<XrViewConfigurationView> configViews =
        g_session->enumerateViewConfigurationViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

    // --- acquire + wait on the swapchain image for this frame ---
    uint32_t swapchainIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    // TEMP DIAGNOSTIC (this session): neither of these two calls, nor
    // xrReleaseSwapchainImage/xrEndFrame in submitFrame() below, previously
    // checked their XrResult at all -- a silent failure here would produce
    // exactly "compositor sees the app, gets nothing" with zero evidence
    // anywhere. Added purely to see whether that's what's happening; not a
    // behavior change if these are succeeding.
    const XrResult acquireResult = xrAcquireSwapchainImage(g_session->swapchain(), &acquireInfo, &swapchainIndex);
    if (XR_FAILED(acquireResult)) {
        char msg[128];
        _snprintf_s(msg, _TRUNCATE,
                    "[dusk::vr::tick] FAILED: xrAcquireSwapchainImage, XrResult=%d\n",
                    static_cast<int>(acquireResult));
        OutputDebugStringA(msg);
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(g_session->session(), &endInfo);
        return;
    }

    XrSwapchainImageWaitInfo waitImgInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitImgInfo.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g_session->swapchain(), &waitImgInfo))) {
        OutputDebugStringA("[dusk::vr::tick] FAILED: xrWaitSwapchainImage\n");
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_session->swapchain(), &releaseInfo);
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(g_session->session(), &endInfo);
        return;
    }

    // CONFIRMED this session (m_Do_main.cpp): tick() is called from INSIDE
    // that file's own aurora_begin_frame()/aurora_end_frame() pair (around
    // its line 335), not the other way around -- there's no
    // aurora_begin_frame() call for tick() to make here. See submitFrame()
    // below (called separately, after m_Do_main.cpp's aurora_end_frame())
    // for the other half of what used to be this function.

    std::vector<XrCompositionLayerProjectionView> projViews(viewCount);
    std::vector<PendingEyeReadback> pendingEyes(viewCount);

    // Advance the HUD billboard's damped reference direction once per frame
    // (not per eye) -- see vr_stereo_render.hpp's updateHudSmoothing()/
    // computeHudPose() comments. Added per user feedback that the
    // head-locked panel felt "really shaky" with raw per-frame tracking.
    vr_render::updateHudSmoothing(hmdPose);

    // CONFIRMED this session (first HUD-billboard in-headset test came back
    // solid black): mDoGph_drawHud2D() draws nothing until fpcM_DrawIterater()
    // has run at least once this frame -- whatever populates/refreshes the
    // persistent 2D HUD draw-list content (actor updates, meter state, etc.)
    // happens as a side effect of that call, not independently of it. A
    // hand-drawn marker quad in the same capture pass proved the
    // offscreen-pass/GXCopyTex pipeline itself was fine even while this was
    // broken, isolating the problem to content timing specifically. Calling
    // it here, before captureHudBillboard(), guarantees real content is
    // ready for the capture; eye 0/eye 1's own later calls to this same
    // function (inside the loop below) are unaffected -- confirmed via
    // real HUD content (icons, not black) rendering correctly afterward.
    //
    // MEASURED this session: ~0.09-0.14ms/frame during normal gameplay,
    // rising to ~0.5ms combined with captureHudBillboard() below when a
    // menu/pause screen is open (more 2D content to draw, less 3D actor
    // traversal to do) -- under 2-4% of a single frame's budget even at the
    // worst observed moment against a 72-90Hz VR target. Not a measurable
    // perf concern; not worth optimizing further absent new evidence.
    fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);

    // Capture the flat 2D HUD into a shared offscreen texture ONCE, before
    // either eye's protected offscreen pass opens (see captureHudBillboard()'s
    // own comment for why this ordering matters -- GXCreateFrameBuffer only
    // supports one level of nesting, so this must never run between
    // beginEye()/endEye()). Both eyes then draw it as the same stereo
    // billboard (vr_render::drawHudBillboard(), invoked from
    // mDoGph_Painter()'s per-eye HUD call site) instead of each eye
    // redrawing the flat HUD independently at zero disparity.
    //
    // MEASURED this session: ~0.04-0.07ms/frame during normal gameplay,
    // rising to ~0.15-0.49ms when a menu/pause screen is open (more 2D
    // content to draw) -- see fpcM_DrawIterater()'s comment above for the
    // combined-cost assessment against a VR frame budget.
    mDoGph_gInf_c::captureHudBillboard();

    // ROOT-CAUSED this session: the minimap (and pause-screen map) render
    // their own source texture via a SEPARATE GXCreateFrameBuffer offscreen
    // pass (d_map_path.cpp's dRenderingMap_c::renderingMap(), reached via
    // dComIfGd_drawCopy2D() -> dDlst_list_c::drawCopy2D()), normally
    // triggered once per eye from inside mDoGph_Painter() -- i.e. AFTER
    // beginEye() has already opened that eye's own protected offscreen pass,
    // where nesting a second one would crash (same class of bug as water's
    // reflection capture). renderingMap() has always unconditionally skipped
    // itself there via isRenderingToHeadset(), which -- because that flag is
    // true for this whole function, not just inside an eye pass -- also
    // meant the minimap's texture NEVER got rendered at all during VR,
    // leaving it at whatever uninitialized GPU memory it started with (the
    // reported "black with scattered colorful corruption pixels"). The
    // minimap's actual on-screen picture (mMapJ2DPicture, drawn as part of
    // the flat 2D HUD) was already being captured/displayed correctly by
    // captureHudBillboard() above -- only the texture IT SAMPLES was stale.
    // Fix: render it here instead, once per frame, in this same safe window
    // captureHudBillboard() already uses (no eye pass open yet). renderingMap()
    // and postRenderingMap()'s capture guard were changed from
    // isRenderingToHeadset() to the new, narrower isEyePassOpen() (false
    // here, true during mDoGph_Painter()'s later per-eye call) so it actually
    // runs now instead of skipping again.
    mDoGph_gInf_c::captureMapCopy2D();

    for (uint32_t eye = 0; eye < viewCount; ++eye) {
        vr_render::EyeParams eyeParams{
            views[eye].pose,
            views[eye].fov,
            configViews[eye].recommendedImageRectWidth,
            configViews[eye].recommendedImageRectHeight,
            hmdPose.position,
            vrCameraEyeAnchor,
        };

        // Safe to call unconditionally here: the isViewReady() check earlier
        // in this function already returned before reaching this loop if
        // there's no active gameplay view, so beginEye()'s own internal
        // dComIfGd_getView() (see vr_stereo_render.hpp) is guaranteed
        // non-null at this point.
        g_duskVRCurrentEyeIndex = eye;
        vr_render::beginEye(eyeParams);
        g_duskVREyePassOpen = true;

        fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
        cAPIGph_Painter();

        // TODO: inject hand mesh before resolving the pass:
        // vr_render::HandPayload payload{ eye == 0 ? leftPose : rightPose };
        // aurora::gfx::push_custom_draw(g_handDrawState.typeId, &payload, sizeof(payload));

        aurora::gfx::ResolvedTargets targets = vr_render::endEye();
        g_duskVREyePassOpen = false;

        // ROOT-CAUSED this session (VR_MOD_HANDOFF_10 follow-up): endEye()
        // now uses resolve_pass_checked() internally and returns an empty
        // ResolvedTargets (colorTexture null) when the offscreen pass this
        // eye opened got silently substituted by an ordinary in-game
        // GXCopyTex drain during the scene draw (resolve_pass_into doesn't
        // check is_offscreen() -- see gfx.hpp's comments). Skip this eye's
        // copy entirely this frame rather than encoding a copy from a null/
        // wrong-sized texture -- that was the actual crash. The swapchain
        // image simply keeps last frame's content for this eye.
        if (!targets.colorTexture) {
            pendingEyes[eye] = PendingEyeReadback{}; // valid = false (default)
            projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projViews[eye].pose = views[eye].pose;
            projViews[eye].fov = views[eye].fov;
            projViews[eye].subImage.swapchain = g_session->swapchain();
            projViews[eye].subImage.imageArrayIndex = 0;
            projViews[eye].subImage.imageRect.offset = {
                static_cast<int32_t>(eye * eyeParams.width), 0};
            projViews[eye].subImage.imageRect.extent = {
                static_cast<int32_t>(eyeParams.width), static_cast<int32_t>(eyeParams.height)};
            continue;
        }

        // CPU round-trip copy, part 1 of 2 (VR_MOD_HANDOFF_10 #3, revised
        // this session): records the Dawn-side copy via push_encoder_task
        // so it lands on the SAME shared frame encoder as everything else,
        // in-order, right after resolve_pass's own snapshot copy -- see
        // vr_xr_submit.hpp's Session::encodeEyeCopy comment for the full
        // root-cause chain (the previous single-call version submitted its
        // own copy BEFORE aurora_end_frame's Submit() had run at all, i.e.
        // before colorTexture's write was even on the GPU queue -- that's
        // the actual crash cause, not just a missing usage flag).
        //
        // dstXOffset matches projViews[eye].subImage.imageRect.offset below --
        // same double-wide-swapchain math (eye * eyeParams.width), kept in
        // sync deliberately since both describe where this eye lands in the
        // one physical swapchain image.
        //
        // Part 2 (readbackEyeCopy -- the MapAsync/D3D12-upload/XR-submit
        // half) runs later, in submitFrame() (bottom of this file), called
        // by m_Do_main.cpp right after ITS aurora_end_frame() -- confirmed
        // this session that tick() runs INSIDE m_Do_main.cpp's own
        // aurora_begin_frame()/aurora_end_frame() pair and returns before
        // aurora_end_frame() executes its Submit(). This tick()/submitFrame()
        // split, and moving xrReleaseSwapchainImage()/xrEndFrame() into
        // submitFrame() too, exists because of that.
        g_session->encodeEyeCopy(
            targets.colorTexture, eye, swapchainIndex, eyeParams.width, eyeParams.height,
            eye * eyeParams.width, aurora::gfx::color_format());

        pendingEyes[eye] = PendingEyeReadback{
            true, eye, swapchainIndex, eyeParams.width, eyeParams.height, eye * eyeParams.width};

        projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projViews[eye].pose = views[eye].pose;
        projViews[eye].fov = views[eye].fov;
        projViews[eye].subImage.swapchain = g_session->swapchain();
        projViews[eye].subImage.imageArrayIndex = 0;
        // Double-wide single swapchain (decided in startup(), see its comment):
        // eye 0 (left) occupies the left half, eye 1 (right) the right half.
        // Relies on OpenXR's view ordering convention (view 0 = left, view 1 =
        // right for XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) -- confirmed
        // correct this session via the FOV asymmetry (each view's angleLeft/
        // angleRight is wider on its own outward side), and via a from-scratch
        // math derivation of eyeFovToProjMtx's off-center term that fixed the
        // actual crossed-eyes bug (see vr_stereo_render.hpp).
        projViews[eye].subImage.imageRect.offset = {
            static_cast<int32_t>(eye * eyeParams.width), 0};
        projViews[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(eyeParams.width), static_cast<int32_t>(eyeParams.height)};
    }

    // NEW this session (split from what used to be tick()'s tail): we can't
    // call readbackEyeCopy(), xrReleaseSwapchainImage(), or xrEndFrame() yet
    // -- readbackEyeCopy() needs this frame's aurora_end_frame() (called by
    // m_Do_main.cpp AFTER tick() returns, per its own loop structure -- see
    // submitFrame()'s comment) to have actually run its Submit() first.
    // Releasing the swapchain image or calling xrEndFrame before that would
    // hand the image back to the runtime (or let it composite) before pixels
    // are actually written into it. Stash what submitFrame() needs instead.
    g_pendingSubmit.eyes = std::move(pendingEyes);
    g_pendingSubmit.projViews = std::move(projViews);
    g_pendingSubmit.frameState = frameState;
    g_pendingSubmit.base = base;
    g_pendingSubmit.viewCount = viewCount;
    g_hasPendingFrameSubmit = true;
}

// NEW this session: the other half of what used to be tick()'s tail end,
// split out because of a confirmed ordering bug (see
// vr_xr_submit.hpp's Session::encodeEyeCopy/readbackEyeCopy comment for the
// root cause). tick() is called from INSIDE m_Do_main.cpp's
// aurora_begin_frame()/aurora_end_frame() pair and returns well before
// aurora_end_frame() runs -- so anything that depends on this frame's Dawn
// copy having actually been submitted to the GPU (readbackEyeCopy) can't
// happen inside tick() itself. Call this once, right after
// m_Do_main.cpp's aurora_end_frame() -- NOT inside the begin/end pair.
//
// Safe to call unconditionally every frame: if tick() didn't actually
// render stereo eyes this frame (no session, shouldRender==false, no ready
// gameplay view -- all of which already called their own complete
// xrEndFrame with an empty layer list and returned early), g_hasPendingFrameSubmit
// stays false and this is a no-op.
void submitFrame() {
    if (!g_hasPendingFrameSubmit) {
        return;
    }
    g_hasPendingFrameSubmit = false;

    // ROOT-CAUSED this session: aurora_end_frame() only ENQUEUES this
    // frame's work onto Aurora's render worker thread (render_worker::
    // enqueue_end_frame in common.cpp) and returns immediately -- it does
    // NOT wait for that thread to actually run it. The per-eye encoder
    // task pushed in tick() (Session::encodeEyeCopy, via push_encoder_task)
    // is likewise just queued when called and replayed later on that same
    // worker thread (common.cpp's enqueue_pass/execute_encoder_task), which
    // is where the actual CopyTextureToBuffer into each eye's
    // cpuCopyBuffers_[eyeIndex] happens. Without waiting for that, this
    // loop's MapAsync below could run concurrently with the worker thread
    // still recording/submitting that same CopyTextureToBuffer against the
    // identical wgpu::Buffer -- a genuine cross-thread race, not just the
    // eyeIndex/swapchainIndex aliasing fixed earlier -- which is exactly
    // what "WebGPU error 2: Concurrent buffer operations are not allowed"
    // on MapAsync reported. aurora::gfx::synchronize() (gfx.hpp) blocks
    // until the render worker has fully drained its queue, so by the time
    // it returns here, this frame's copy is guaranteed to have actually
    // executed and been submitted -- safe to MapAsync after that.
    aurora::gfx::synchronize();

    for (const auto& eye : g_pendingSubmit.eyes) {
        // NEW this session (VR_MOD_HANDOFF_10 follow-up, option (c)): skip
        // an eye tick() marked invalid -- encodeEyeCopy() was never called
        // for it this frame (endEye() detected a foreign pass substitution
        // and returned early), so there's no new Dawn-side copy to read
        // back. Calling readbackEyeCopy() anyway would just re-upload
        // whatever stale data is already in this index's readback buffer
        // from a previous frame.
        if (!eye.valid) {
            continue;
        }
        g_session->readbackEyeCopy(eye.eyeIndex, eye.swapchainIndex, eye.eyeWidth, eye.eyeHeight,
                                    eye.dstXOffset, aurora::gfx::color_format());
    }

    g_session->endAccessAll();

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    // TEMP DIAGNOSTIC (this session): see tick()'s matching comment above.
    if (XR_FAILED(xrReleaseSwapchainImage(g_session->swapchain(), &releaseInfo))) {
        OutputDebugStringA("[dusk::vr::submitFrame] FAILED: xrReleaseSwapchainImage\n");
    }

    XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projLayer.space = g_pendingSubmit.base;
    projLayer.viewCount = g_pendingSubmit.viewCount;
    projLayer.views = g_pendingSubmit.projViews.data();

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer)};

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_pendingSubmit.frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    if (XR_FAILED(xrEndFrame(g_session->session(), &endInfo))) {
        OutputDebugStringA("[dusk::vr::submitFrame] FAILED: xrEndFrame\n");
    }
}

}  // namespace dusk::vr
