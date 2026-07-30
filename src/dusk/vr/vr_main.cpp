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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "SSystem/SComponent/c_API_graphic.h"  // cAPIGph_Painter
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
// TEMP DIAGNOSTIC (VR water-black investigation): which eye (0=left,
// 1=right) is currently being drawn, set right before beginEye() each
// iteration of the per-eye loop below. Same extern "C" pattern as
// g_duskVRRenderingToHeadset, for the same reason (usable from
// extern/aurora's gx.cpp without namespace/mangling issues).
extern "C" uint32_t g_duskVRCurrentEyeIndex = 0;

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

// TODO: the grip spaces still need real xrCreateActionSpace calls (needs
// action set setup -- NOT YET WRITTEN ANYWHERE). Until fixed, hands render
// at tracking-space origin (rightPose/leftPose in tick() below). g_viewSpace
// no longer has this problem -- startup() now assigns it a real
// xrCreateReferenceSpace(VIEW) handle (see vr_xr_bootstrap.hpp), so hmdPose
// in tick() reflects genuine head tracking.
XrSpace g_rightGripSpace = XR_NULL_HANDLE;
XrSpace g_leftGripSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;

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
    // TODO: call once, after an aurora::gfx device exists:
    // g_handDrawState.typeId = aurora::gfx::register_draw_type(vr_render::handDrawDescriptor());
}

bool isActive() {
    return g_session != nullptr;
}

bool isRenderingToHeadset() {
    return g_renderedToHeadsetThisFrame;
}

void getEyeSymmetricFov(float* fovyDeg, float* aspect) {
    vr_render::getEyeSymmetricFov(fovyDeg, aspect);
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

static XrPosef locateSpace(XrSpace space, XrSpace base, XrTime time) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (space == XR_NULL_HANDLE) {
        return XrPosef{{0, 0, 0, 1}, {0, 0, 0}};  // identity fallback
    }
    xrLocateSpace(space, base, time, &loc);
    return loc.pose;
}

void tick(const dusk::game_clock::MainLoopPacer& pacing) {
    // Reset up front: every early-return below (no session, session just
    // stopped, XR wait/begin failure, shouldRender==false, no ready
    // gameplay view) means no stereo draw happened this frame. Only the
    // per-eye loop further down flips this true.
    g_renderedToHeadsetThisFrame = false;
    g_duskVRRenderingToHeadset = false;

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

    const XrPosef hmdPose = locateSpace(g_viewSpace, base, time);
    const XrPosef rightPose = locateSpace(g_rightGripSpace, base, time);
    const XrPosef leftPose = locateSpace(g_leftGripSpace, base, time);

    // --- swing gesture -> attack trigger (design decision: no new combat logic) ---
    if (!pacing.is_interpolating) {
        vr_combat::Pose swingPose{
            {rightPose.position.x, rightPose.position.y, rightPose.position.z},
            static_cast<double>(time) / 1e9};  // XrTime is nanoseconds
        vr_combat::SwingEvent event = g_rightSwing.update(swingPose);
        if (event.triggered) {
            // Per handoff doc:
            // mDoCPd_c::getCpadInfo(PAD_1).mPressedButtonFlags |= PAD_BUTTON_A (0x0100)
            // TODO: confirm the exact call site/include for mDoCPd_c and wire this in.
        }
    }

    // --- Link head hide + hand matrix mapping ---
    vr_link::FrameInput frameInput{hmdPose, rightPose, leftPose};
    vr_link::updateFrame(frameInput);

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

    for (uint32_t eye = 0; eye < viewCount; ++eye) {
        vr_render::EyeParams eyeParams{
            views[eye].pose,
            views[eye].fov,
            configViews[eye].recommendedImageRectWidth,
            configViews[eye].recommendedImageRectHeight,
            hmdPose.position,
        };

        // Safe to call unconditionally here: the isViewReady() check earlier
        // in this function already returned before reaching this loop if
        // there's no active gameplay view, so beginEye()'s own internal
        // dComIfGd_getView() (see vr_stereo_render.hpp) is guaranteed
        // non-null at this point.
        g_duskVRCurrentEyeIndex = eye;
        vr_render::beginEye(eyeParams);

        fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
        cAPIGph_Painter();

        // TODO: inject hand mesh before resolving the pass:
        // vr_render::HandPayload payload{ eye == 0 ? leftPose : rightPose };
        // aurora::gfx::push_custom_draw(g_handDrawState.typeId, &payload, sizeof(payload));

        aurora::gfx::ResolvedTargets targets = vr_render::endEye();

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
