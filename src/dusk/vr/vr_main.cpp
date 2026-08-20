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
#include "SSystem/SComponent/c_math.h"          // cM_atan2s -- getHeadMoveAngleS()
#include "m_Do/m_Do_controller_pad.h"            // mDoCPd_c::getSubStickX -- real physical gamepad C-stick
#include "m_Do/m_Do_graphic.h"                  // mDoGph_gInf_c::captureHudBillboard
#include "f_pc/f_pc_manager.h"                  // fpcM_DrawIterater, fpcM_Draw
#include "dusk/game_clock.h"                    // dusk::game_clock::MainLoopPacer
#include "dusk/settings.h"                      // dusk::getSettings().game.vrDesktopMirror
#include "dusk/logging.h"                       // DuskLog-style aurora::Module, see VrLog below
#include "dusk/ui/ui.hpp"                       // dusk::ui::any_document_visible() -- VR menu billboard gating

#include "dusk/vr/vr_xr_bootstrap.hpp"
#include "dusk/vr/vr_stereo_render.hpp"         // vr_render::
#include "dusk/vr/vr_swing_detector.hpp"        // vr_combat::
#include "dusk/vr/vr_link_visibility.hpp"       // vr_link::
#include "dusk/vr/vr_smooth_turn.hpp"           // dusk::vr::updateSmoothTurn/getSmoothTurnYawRad
#include "dusk/vr/vr_xr_submit.hpp"             // dusk::vr::Session
#include "dusk/vr/vr_menu_gamepad.hpp"          // dusk::vr::ensureVrMenuGamepadAttached, etc.
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
// Real, persistent logging (goes to the AppData log file every user gets
// for free, unlike OutputDebugStringA -- which only exists at all with a
// debugger attached, and is what the rest of this file's TEMP DIAGNOSTIC
// comments use instead). Scoped to genuinely rare, event-driven call sites
// only (startup outcome, session-state transitions) -- NEVER called from
// the per-eye render path. See dusk/logging.cpp's WriteLogLine(): every
// call does a synchronous fflush() to disk under a mutex, which is cheap
// at "once per startup" / "once per state change" rates but would be a
// real, measurable frame-time cost (and a stutter/stall risk from disk
// I/O) if it were ever called every VR frame.
static aurora::Module VrLog("dusk::vr");

Session* g_session = nullptr;
std::unique_ptr<Session> g_ownedSession;  // vr_main.cpp owns the Session; g_session just points to it
// RIGHT hand -> R (shield bash), added 2026-08-13 per explicit user request
// ("if you thrust the right controller it should press R basically, as R
// is shield bash"). This is the same infra originally drafted 2026-08-03
// as a right-hand SWORD-swing attempt (deferred, never wired -- see the
// left-hand swing gesture's own comment below for that history) --
// repurposed here for its actually-requested use rather than left dead.
// Started from g_leftSwing's own FINAL, six-round-tuned values (below)
// rather than SwingDetector's untested class defaults -- a thrust and a
// swing are physically the same character of gesture (a deliberate fast
// hand motion) on the same hardware/runtime, so reusing already-proven-good
// numbers is a much better starting point than guessing blind a second
// time. Untested for THIS gesture specifically though -- a stab may turn
// out to want a shorter minSwingDistance (less travel than a full sword
// swing) or a different triggerSpeed; retune with real
// [dusk::vr::swingdiag]-style data if the user reports it's off, same
// workflow as the sword gesture's own six rounds.
vr_combat::SwingDetector g_rightThrust = [] {
    vr_combat::SwingDetector d;
    d.triggerSpeed = 2.2f;
    d.resetSpeed = 0.4f;
    d.minSwingDistance = 0.12f;
    d.cooldownSec = 0.0;
    return d;
}();

// LEFT hand -> B (attack), wired below -- sword is Link's LEFT hand
// (section 16 of vr-mod-notes), so a swing of the hand actually holding the
// sword is the physically-intuitive gesture for this, unlike the deferred
// right-hand attempt above.
//
// TUNED 2026-08-05, ROUND 2 (real [dusk::vr::swingdiag] capture analyzed --
// see git history/vr-mod-notes for the full log breakdown, not repeated
// here). Round 1 (triggerSpeed=1.4/resetSpeed=0.4/minSwingDistance=0.08,
// user feedback "technically worked but very unresponsive" on the
// untouched 2.5/0.8/0.15 defaults) turned out to be tuned TOO far down:
// the captured log showed one frame of ordinary "hold the controller
// neutrally and turn around" movement hitting 1.44 m/s -- just over
// round 1's 1.4 m/s trigger -- firing a spurious attack (confirms the
// "swings when I move normally" report). The dt source itself (predDt vs.
// pacing.presentation_dt_seconds, logged side by side specifically to
// check this) tracked each other almost exactly throughout that phase, so
// this was a plain threshold problem, not a timing/jitter bug. The SAME
// capture's real-swing phase logged 13 separate triggers (speeds ~1.5 up
// to ~17 m/s) against only ~5 visible in-game sword swings -- likely
// Link's own attack-animation lock absorbing extra virtual B-presses
// (same as mashing the real button), not under-detection, so round 2 only
// raises thresholds (to comfortably clear the observed 1.44 m/s false-
// positive peak with real margin) rather than trying to make it MORE
// sensitive. resetSpeed/cooldownSec also nudged up to reduce the chance of
// one continuous swing motion dipping-and-re-arming into a double count.
// Tuned here rather than in the shared header so g_rightThrust (or any
// future user of SwingDetector) isn't silently affected by tuning specific
// to this one gesture.
//
// ROUND 4 (2026-08-13) -- user report "missed swings (real swings don't
// register)" after round 3 (see history above). A fresh
// [dusk::vr::swingdiag] capture (227 samples) showed this WASN'T a
// triggerSpeed problem -- the vast majority of samples with speed well
// above 2.2 m/s (many in the 3-7+ m/s range, dozens of them) simply never
// triggered. Cross-checked against SwingDetector::update()'s actual logic
// (vr_swing_detector.hpp): canFire_ only resets once speed drops to AT OR
// BELOW resetSpeed -- a hard one-shot re-arm gate, not a decaying window.
// During a real multi-swing test, hand speed only briefly dips (if at all)
// between individual swings, so round 3's resetSpeed=0.7 m/s left the
// detector "stuck" not-armed through most of a continuous flurry --
// directly confirmed in the capture (e.g. one stretch: a real trigger at
// speed 2.48 m/s, then SIX consecutive high-speed samples up to 6.4 m/s
// all logged TRIGGERED=0, before it finally re-armed ~650ms later). This
// is the opposite failure mode from round 2's original "one continuous
// swing double-counts" concern that motivated raising resetSpeed in the
// first place -- but cooldownSec (a hard TIME lockout, unchanged at 0.15s)
// already covers that same concern independently, so lowering resetSpeed
// back down doesn't require also touching cooldownSec. triggerSpeed/
// minSwingDistance are untouched -- no evidence in this capture that
// either is currently a problem (neutral-movement speeds stayed
// comfortably under ~1.1 m/s against the 2.2 m/s trigger).
vr_combat::SwingDetector g_leftSwing = [] {
    vr_combat::SwingDetector d;
    d.triggerSpeed = 2.2f;        // round 1: 1.4 (too low -- false-fired at 1.44
                                    // during ordinary movement); default: 2.5
    d.resetSpeed = 0.4f;          // round 4: back down from round 3's 0.7,
                                    // which was confirmed (by capture) to leave
                                    // the detector stuck not-armed through most
                                    // of a real multi-swing flurry; round 1: 0.4
                                    // (same value, now re-derived from evidence
                                    // rather than the original guess); default: 0.8
    d.minSwingDistance = 0.12f;   // round 1: 0.08; default: 0.15
    d.cooldownSec = 0.0;          // round 5 (2026-08-13): disabled per explicit
                                    // user request, to test whether it's capping
                                    // how fast real swings can chain vs. only
                                    // ever blocking one-swing double-counts.
                                    // resetSpeed's own hysteresis (must drop to
                                    // 0.4 m/s before re-arming) still guards
                                    // against a single continuous swing firing
                                    // twice, so this isn't fully unguarded --
                                    // just removes the additional flat time
                                    // floor on top of that. round 1/default: 0.12;
                                    // round 2-4: 0.15
    return d;
}();

// NEW this session: needed so tick()'s event pump (below) can call
// xrPollEvent without needing Session to expose its private instance_.
// Set once in startup() alongside g_ownedSession.
XrInstance g_xrInstance = XR_NULL_HANDLE;

// FIXED this session ("Link's movement isn't relative to the headset, like
// a flatscreen camera is still steering it" -- user report, confirmed
// accurate): daAlink_c's mMoveAngle (d_a_alink.cpp) used to be built from
// dCam_getControledAngleY() -- the flatscreen third-person camera's own
// angle, driven by the base game's normal auto-follow camera logic, with
// zero relationship to which way the player's actual head is turned --
// plus only the VR smooth-turn stick's yaw contribution added on top
// (section 15 of vr-mod-notes). Physically turning your head without
// touching the stick therefore never changed which way "forward" on the
// movement stick walked Link. This is the real, undamped game-world yaw
// the player's head is currently facing (including the smooth-turn
// offset -- see computeHeadWorldForward()'s own comment), computed once
// per frame in tick() below and read by d_a_alink.cpp via
// getHeadMoveAngleS(), replacing dCam_getControledAngleY() entirely as
// the VR movement-direction basis rather than only patching stick-turn on
// top of it. Deliberately undamped (unlike the HUD's own
// g_hudSmoothedWorldForward) -- movement direction should track head
// rotation immediately, not lag.
s16 g_headMoveAngleS = 0;

// Real right-controller-pointing aim yaw/pitch -- see getControllerAimAngles()'s
// own declaration comment (vr_main.hpp). Computed once per frame in tick(),
// same pattern/reasoning as g_headMoveAngleS above.
s16 g_controllerAimYawS = 0;
s16 g_controllerAimPitchS = 0;

// HMD-based aim pitch -- see getHeadAimAngles()'s own declaration comment
// (vr_main.hpp). Yaw reuses g_headMoveAngleS directly (identical formula,
// same headForward vector -- no reason to keep a second copy), so only
// pitch needs its own storage here.
s16 g_headAimPitchS = 0;

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
                // Event-driven (only fires when the runtime actually sends
                // a state change, at most a handful of times during
                // startup) -- cheap enough for real DuskLog, unlike a
                // per-frame call would be.
                VrLog.info("startup: session state -> {}", static_cast<int>(stateEvent.state));

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

// Forward declaration -- real definition (and the null-space identity
// fallback it provides) is further down in this file; needed here so
// applyTrackedHandMtx() below can call it for late-latching.
static XrPosef locateSpace(XrSpace space, XrSpace base, XrTime time,
                            XrSpaceLocationFlags* outFlags = nullptr);

// LATE-LATCHING: re-locates the HMD + both controller grip spaces again
// right before writing the tracked-hand joints, on the theory that a
// slightly later real-time sample lets the runtime's xrLocateSpace
// extrapolation use fresher IMU data (standard technique other VR engines
// use to shave sample-to-display latency for hand-tracked geometry).
// Confirmed real but NOT the actual fix for hand lag -- this whole call
// site (reached only via daAlink_c::draw()'s legacy once-per-sim-tick
// path) turned out to never run during a real VR eye pass at all; the
// actual fix is refreshTrackedHandDrawMtxLive() below, called once per real
// frame directly from tick(). Left in place -- harmless (cheap XR calls),
// and still correctly keeps mpLinkHandModel's AnmMtx in sync for whatever
// legacy/flatscreen-adjacent code reads it via that dead path.
void applyTrackedHandMtx(J3DModel* handModel) {
    if (g_session) {
        const XrTime time = g_session->predictedDisplayTime();
        const XrSpace base = g_session->localSpace();
        const XrPosef hmdPose = locateSpace(g_viewSpace, base, time);
        const XrPosef rightPose = locateSpace(g_rightGripSpace, base, time);
        const XrPosef leftPose = locateSpace(g_leftGripSpace, base, time);

        view_class* view = dComIfGd_getView();
        if (view) {
            const cXyz eyeAnchor = vr_link::getVrCameraEyeAnchor(view->lookat.eye);
            vr_link::computeTrackedHandMatrices(hmdPose.position, rightPose, leftPose,
                                                 eyeAnchor, getSmoothTurnYawRad());
        }
    }
    vr_link::applyTrackedHandMtx(handModel);
}

void refreshTrackedHandDrawMtxLive(J3DModel* handModel) {
    vr_link::refreshTrackedHandDrawMtxLive(handModel);
}

void applyTrackedItemMtx(J3DModel* swordModel, J3DModel* shieldModel,
                          float (*leftItemJointMtx)[4], float (*leftHandJointMtx)[4],
                          float (*rightItemJointMtx)[4], float (*rightHandJointMtx)[4]) {
    vr_link::applyTrackedItemMtx(swordModel, shieldModel,
                                  leftItemJointMtx, leftHandJointMtx,
                                  rightItemJointMtx, rightHandJointMtx);
}

void refreshTrackedItemMtxLive() {
    vr_link::refreshTrackedItemMtxLive();
}

bool isVrFirstPerson(daAlink_c* link) {
    return vr_link::isFirstPerson(link);
}

bool shouldTrackHookshotToHand(daAlink_c* link) {
    return vr_link::shouldTrackHookshotToHand(link);
}

void refreshTrackedHeldItemMtxLive() {
    vr_link::refreshTrackedHeldItemMtxLive();
}

bool getTrackedHandWorldPos(bool isLeftHand, float& outX, float& outY, float& outZ) {
    return vr_link::getTrackedHandWorldPos(isLeftHand, outX, outY, outZ);
}

void refreshTrackedItemJointMtxLive() {
    vr_link::refreshTrackedItemJointMtxLive();
}

bool getTrackedItemJointMtx(bool isLeft, float (*outMtx)[4]) {
    return vr_link::getTrackedItemJointMtx(isLeft, outMtx);
}

void refreshTrackedBoomerangMtxLive() {
    vr_link::refreshTrackedBoomerangMtxLive();
}

void refreshTrackedFishingRodMtxLive() {
    vr_link::refreshTrackedFishingRodMtxLive();
}

bool isFishingHookInWater() {
    return vr_link::isFishingHookInWater();
}

bool isFishingRodActive() {
    return vr_link::isFishingRodActive();
}

bool isRealCutsceneRunning() {
    return vr_link::isRealCutsceneRunning();
}

void refreshTrackedHookshotMtxLive() {
    vr_link::refreshTrackedHookshotMtxLive();
}

void applyVrBodyPositionOffset(J3DModel* bodyModel) {
    vr_link::applyVrBodyPositionOffset(bodyModel);
}

float getSmoothTurnYawRad() {
    return dusk::vr::g_smoothTurnYawRad;
}

s16 getHeadMoveAngleS() {
    return g_headMoveAngleS;
}

void getControllerAimAngles(s16* outYawS, s16* outPitchS) {
    *outYawS = g_controllerAimYawS;
    *outPitchS = g_controllerAimPitchS;
}

void getHeadAimAngles(s16* outYawS, s16* outPitchS) {
    *outYawS = g_headMoveAngleS;
    *outPitchS = g_headAimPitchS;
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
    // Declared here, outside the try block, so the catch block below can
    // still reference it (with an empty systemName, from the aggregate
    // init's zero-fill) even if the exception happened before
    // xrGetSystemProperties() ever ran -- e.g. vr_xr::initialize() itself
    // throwing. Queried once inside the try block below (cheap -- a single
    // OpenXR call, not per-frame) so every later log line in this function
    // can say which runtime it's talking about. Genuinely useful for real
    // user bug reports: most of this project's VR history (see
    // vr-mod-notes section 6, stereo eye alignment, aim-pose calibration,
    // etc.) turned out to be runtime-specific (SteamVR vs. Virtual Desktop
    // vs. Meta Link), and today none of that is visible outside a
    // debugger session.
    XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
    try {
        vr_xr::Bootstrap boot = vr_xr::initialize();
        xrGetSystemProperties(boot.instance, boot.systemId, &sysProps);

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
        // Real "is this SteamVR" signal for Session::effectiveGammaExponent()
        // -- see isSteamVr_'s own comment (vr_xr_submit.hpp) for why this can
        // no longer be inferred from which swapchain format ended up chosen.
        // Substring check, not exact match: sysProps.systemName is a
        // free-form runtime-supplied string ("SteamVR/OpenXR",
        // "SteamVR/OpenXR : oculus", etc. across driver versions), not a
        // stable enum.
        g_ownedSession->setIsSteamVr(std::strstr(sysProps.systemName, "SteamVR") != nullptr);
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
            VrLog.error("startup failed: enumerateViewConfigurationViews returned 0 views (runtime: {})",
                        sysProps.systemName);
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
            VrLog.error("startup failed: createSwapchain({}, {}, dxgiFormat={}) returned false (runtime: {})",
                        eyeWidth * 2, eyeHeight, dxgiFormat, sysProps.systemName);
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
            VrLog.error("startup failed: session never reached READY / xrBeginSession failed (runtime: {})",
                        sysProps.systemName);
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
        VrLog.info("startup succeeded: runtime={} swapchain={}x{} dxgiFormat={}",
                   sysProps.systemName, eyeWidth * 2, eyeHeight, dxgiFormat);

        // Phase 1 of the VR-controller-drives-the-Dusklight-menu feature
        // (see vr_menu_gamepad.hpp's own header comment) -- attach once
        // here, now that the session is confirmed up. Not fatal if this
        // fails (logs internally); VR still works without it, just without
        // menu access.
        ensureVrMenuGamepadAttached();

        return true;
    } catch (const std::exception& e) {
        // Catches everything upstream, including toDxgiSwapchainFormat()
        // throwing on an aurora::gfx::color_format() value not yet in its
        // switch, or any OpenXR/D3D12 setup call (xrCreateInstance,
        // D3D12CreateDevice, xrCreateSession, etc.) failing via
        // vr_xr::checkResult(). NOW routed to real DuskLog too (closes the
        // old TODO here) -- this is the single most valuable line for a
        // real user's "VR doesn't work" report: previously this whole
        // function silently fell back to flatscreen with zero trace
        // outside a debugger session (see vr-mod-notes section 6's own
        // note on this). sysProps.systemName may be empty here if the
        // throw happened before it was queried (e.g. vr_xr::initialize()
        // itself failing) -- that's fine, an empty runtime name is itself
        // informative (means it failed before even reaching the runtime).
        char msg[512];
        _snprintf_s(msg, _TRUNCATE, "[dusk::vr::startup] EXCEPTION: %s\n", e.what());
        OutputDebugStringA(msg);
        VrLog.error("startup failed: exception: {} (runtime: {})", e.what(), sysProps.systemName);
        return false;
    }
}

static XrPosef locateSpace(XrSpace space, XrSpace base, XrTime time,
                            XrSpaceLocationFlags* outFlags) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (space == XR_NULL_HANDLE) {
        if (outFlags) *outFlags = 0;
        return XrPosef{{0, 0, 0, 1}, {0, 0, 0}};  // identity fallback
    }
    xrLocateSpace(space, base, time, &loc);
    if (outFlags) *outFlags = loc.locationFlags;
    return loc.pose;
}

namespace {
// ROOT-CAUSED 2026-08-08 (section 20 continuation -- the "hands/body lag
// behind" investigation): a real capture caught dusk::frame_interp::
// begin_frame() firing multiple times in rapid succession mid-scene-draw
// (see [dusk::frameinterp::beginframe] in the diagnostic added to that
// function), with a step=0.0 argument landing right in the middle of an
// otherwise-healthy frame -- and tick() unconditionally resets
// g_duskVREyePassOpen = false at its own top on EVERY call. An exhaustive
// codebase-wide search found dusk::frame_interp::begin_frame() has only
// ONE real caller (m_Do_main.cpp's three call sites, all part of one
// straight-line sequence immediately before dusk::vr::tick() is invoked)
// -- there is no code path that legitimately calls it again mid-tick().
// The only way to reproduce the observed pattern is if tick() itself is
// being called RE-ENTRANTLY -- a nested call starting while an outer
// tick() call is still mid-draw, most likely triggered by a nested
// Windows message pump somewhere deep in the scene draw (the corruption
// correlated tightly with water's GXCopyTex/resolve_pass substitution in
// the captures that showed it, though that specific trigger is not
// independently confirmed) -- clobbering the outer call's in-progress
// state (g_duskVREyePassOpen, and indirectly g_step via the nested call's
// own begin_frame-adjacent work) out from under it.
//
// Fix: guard tick() against re-entrancy directly, rather than continuing
// to hunt for the exact nested-pump trigger -- this closes the symptom
// regardless of what causes the nested call, and any real fix for the
// nested-pump cause itself (if one is ever found) can layer on top
// without conflicting with this guard. RAII rather than a plain flag +
// manual reset at every return point: tick() has many early-return paths
// (no session, XR call failures, shouldRender==false, view not ready,
// etc.) and a plain flag would need updating at every single one --
// easy to miss one and leave the guard permanently "stuck" true.
struct TickReentrancyGuard {
    bool& flag;
    bool weAcquired;
    explicit TickReentrancyGuard(bool& f) : flag(f), weAcquired(!f) {
        if (weAcquired) flag = true;
    }
    ~TickReentrancyGuard() {
        if (weAcquired) flag = false;
    }
    bool alreadyRunning() const { return !weAcquired; }
};
}  // namespace

void tick(const dusk::game_clock::MainLoopPacer& pacing) {
    static bool s_tickInProgress = false;
    TickReentrancyGuard reentrancyGuard(s_tickInProgress);
    if (reentrancyGuard.alreadyRunning()) {
        // TEMP DIAGNOSTIC (remove once confirmed fixed): confirms this is
        // really the mechanism, and how often it's actually happening.
        static int s_reentrantCount = 0;
        char msg[96];
        _snprintf_s(msg, _TRUNCATE,
            "[dusk::vr::tick] RE-ENTRANT CALL #%d DETECTED -- skipping\n",
            ++s_reentrantCount);
        OutputDebugStringA(msg);
        return;
    }

    // Reset up front: every early-return below (no session, session just
    // stopped, XR wait/begin failure, shouldRender==false, no ready
    // gameplay view) means no stereo draw happened this frame. Only the
    // per-eye loop further down flips this true.
    g_renderedToHeadsetThisFrame = false;
    g_duskVRRenderingToHeadset = false;
    g_duskVREyePassOpen = false;
    // VR-menu-gamepad (see vr_menu_gamepad.hpp): FIXED 2026-08-18 -- this
    // used to call neutralizeVrMenuGamepadState() unconditionally right
    // here, every single frame, which turned out to be a real, systemic
    // bug (see MenuGamepadFrameGuard's own comment for the full story) --
    // it generated a genuine press/release cycle on every real VR frame
    // for anything actually held, reaching RmlUi as constant spam ("if I
    // hold down A, it spams the entry on and off"). Replaced with an RAII
    // guard: neutralizes automatically in its destructor, but ONLY if the
    // real per-frame update (further down, where markRealUpdateRan() is
    // called) never ran this frame -- covers every one of tick()'s early-
    // return paths below (no session, session state transitions/teardown,
    // xrWaitFrame/xrBeginFrame failure, shouldRender==false, view-not-
    // ready) automatically, without a manual reset call at each one (see
    // TickReentrancyGuard just above for the identical reasoning, already
    // established in this file for tick()'s own reentrancy flag). It still
    // deliberately does NOT touch the chord gate's hold-to-open/cooldown
    // timer or the stick pulse gate's own phase timer (computeMenuChordGate()/
    // advanceMenuStickPulse() are never called from the neutral path) --
    // THAT state genuinely cannot tolerate an unconditional every-frame
    // call (see the TAKE 2 comment in vr_menu_gamepad.hpp for why: it broke
    // the hold-to-open feature outright the first time this was tried).
    MenuGamepadFrameGuard menuGamepadFrameGuard;
    // Desktop mirror (see the per-eye loop below, where this gets re-set for
    // real): cleared up front like everything else in this block, so any
    // early return between here and the real eye-rendering section (no
    // session, XR failures, no ready gameplay view, etc.) leaves the desktop
    // window on its normal flatscreen fallback instead of stuck showing a
    // stale VR eye from a prior frame/session.
    aurora::gfx::clear_present_source_mirror();
    // Paired with set_force_no_backdrop(true) below, right where the mirror
    // is actually (re-)set for real -- see that call site's comment and
    // aurora::rmlui::set_force_no_backdrop()'s own comment for the real
    // feedback-loop bug this exists to prevent. Reset here alongside the
    // mirror clear for the identical reason: an early return below must not
    // leave this stuck true from a prior frame that DID have the mirror
    // active.
    aurora::rmlui::set_force_no_backdrop(false);

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
            // Genuinely rare mid-session event (dashboard/system overlay
            // taking focus, headset removed, runtime shutting down) -- not
            // logged at all before now, on any channel. Real DuskLog is
            // exactly right here: this is precisely the kind of thing a
            // real user's "VR randomly stopped working mid-play" report
            // would otherwise leave zero trace of.
            VrLog.info("session state -> {}", static_cast<int>(stateEvent.state));
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
                // this session. Detach the VR-menu-gamepad here too (not on
                // STOPPING, the resumable case -- see vr_menu_gamepad.hpp's
                // detachVrMenuGamepad() comment).
                detachVrMenuGamepad();
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
    // Live-adjustable universal VR gamma compensation (see vr_xr_submit.hpp's
    // kSteamVrGammaCompensationExponent comment, 2026-08-16) -- read here
    // once per frame on the main thread and cached on Session, since
    // encoderTaskCallback() consults it later from the render worker thread.
    g_session->setGammaCompensationMultiplier(dusk::getSettings().game.vrGammaCompensation.getValue());
    // Independent SteamVR-only exponent (2026-08-16 follow-up, see
    // vr_xr_submit.hpp's steamVrGammaExponent_ comment) -- deliberately a
    // separate call/separate ConfigVar from the one above, never coupled.
    g_session->setSteamVrGammaCompensationExponent(
        dusk::getSettings().game.vrGammaCompensationSteamVr.getValue());

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
    // Mapping -- REVISED 2026-08-05 per explicit user request ("add smooth
    // camera rotation to the right stick and also unbind the C stick"),
    // superseding this comment block's previous version (still visible in
    // git history) for the right-thumbstick entry only. No longer mirrors
    // the default Xbox-controller layout 1:1 -- see the individual entries
    // below for what moved where and why:
    //   left thumbstick  -> main stick (movement) -- unchanged
    //   right thumbstick -> VR smooth-turn (see vr_smooth_turn.hpp), NOT
    //                        the C-stick/substick anymore. Right thumbstick
    //                        used to feed padStatus.substickX/Y directly
    //                        (the game's normal C-stick, which smoothly
    //                        orbits the flatscreen third-person camera) --
    //                        removed per the user's explicit "unbind the C
    //                        stick" request. The smooth-turn yaw offset
    //                        this reads instead is also added into
    //                        daAlink_c's mMoveAngle (d_a_alink.cpp, VR-
    //                        gated) so movement direction stays consistent
    //                        with the rotated view -- see
    //                        dusk::vr::getSmoothTurnYawRad()'s call site
    //                        there.
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
    //                        replaced
    //   left squeeze     -> analog R / raise shield (2026-08-13 per explicit
    //                        user request "bind R to the squeeze button on
    //                        the left controller") -- was UNBOUND; the
    //                        user's earlier stated plan to eventually
    //                        replace R with a physical movement gesture
    //                        instead was superseded by this simpler request.
    //                        Mirrors left trigger's own analog-L pattern
    //                        below (continuous 0-255 value + the digital
    //                        bit, gated on the same low deadzone) rather
    //                        than X's binary squeeze-threshold gate, since
    //                        raising the shield is the kind of thing that
    //                        plausibly wants an analog feel like L's aiming
    //                        does, not just an on/off press.
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
    const float leftSqueeze = getFloatAction(g_squeezeValueAction, g_leftHandPath);
    const XrVector2f leftStick = getVec2Action(g_thumbstickAction, g_leftHandPath);
    const XrVector2f rightStick = getVec2Action(g_thumbstickAction, g_rightHandPath);
    const bool rightAHeld = getBoolAction(g_primaryClickAction, g_rightHandPath);
    const bool leftXHeld = getBoolAction(g_primaryClickAction, g_leftHandPath);
    const bool rightBHeld = getBoolAction(g_secondaryClickAction, g_rightHandPath);
    const bool leftYHeld = getBoolAction(g_secondaryClickAction, g_leftHandPath);
    const bool leftMenuHeld = getBoolAction(g_menuClickAction, g_leftHandPath);
    const bool rightStickClickHeld = getBoolAction(g_stickClickAction, g_rightHandPath);
    const bool leftStickClickHeld = getBoolAction(g_stickClickAction, g_leftHandPath);

    // VR-menu-gamepad, plan step 3 (see vr_menu_gamepad.hpp) -- now wires
    // navigation too, on top of step 2's menu-open chord: left thumbstick
    // moves the selection (up/down/left/right), right A confirms, right B
    // backs out/cancels. All already computed above for gameplay --
    // reused as-is, no new OpenXR action reads. dusk::ui::input.cpp's own
    // PADBlockInput(any_document_visible()) already zeroes GAMEPLAY's
    // reading of these same inputs whenever a document is visible, so
    // there's no conflict on that side.
    //
    // leftStick.x/y are fed into updateVrMenuGamepadState() UNCONDITIONALLY
    // below (v7 design, see vr_menu_gamepad.hpp's "Left-stick handling"
    // history comment) -- the stuck-right bug this used to be gated
    // against is instead fixed here, directly, via a one-shot reset on the
    // real menu-closed->open transition. Deliberately a plain function-
    // local static, touched ONLY from this real per-frame call site --
    // never from neutralizeVrMenuGamepadState(), so it can't suffer the
    // same "corrupted by the unconditional every-frame neutralize call"
    // bug class that hit the hold-to-open timer and (per the user's own
    // A/B report) very likely the stick smoothing too.
    const bool menuVisible = dusk::ui::any_document_visible();
    {
        static bool s_menuWasVisibleLastRealFrame = false;
        if (menuVisible && !s_menuWasVisibleLastRealFrame) {
            resetMenuStickPulseState();
        }
        s_menuWasVisibleLastRealFrame = menuVisible;
    }
    updateVrMenuGamepadPlayerIndex();
    // RE-ENABLED 2026-08-18 to test vr_menu_gamepad.hpp's v8 navigation
    // redesign (dominant-axis selection + a pulse gate fully decoupled
    // from neutralizeVrMenuGamepadState() -- see that header's "Left-stick
    // handling" history comment for the full v1-v8 trail). Previously
    // disabled 2026-08-17 after seven failed rounds; re-enable/disable by
    // flipping kMenuChordDisabled below if this round also needs reverting.
    constexpr bool kMenuChordDisabled = false;
    const bool menuChordHeldForGate = kMenuChordDisabled ? false : (leftMenuHeld || rightStickClickHeld);
    const float menuChordTriggerForGate = kMenuChordDisabled ? 0.f : rightTrigger;
    updateVrMenuGamepadState(leftStick.x, leftStick.y, rightAHeld, rightBHeld,
                              menuChordHeldForGate, menuChordTriggerForGate,
                              pacing.presentation_dt_seconds);
    // Tells menuGamepadFrameGuard (top of tick()) a real update ran this
    // frame, so its destructor must NOT also neutralize -- see that
    // guard's own comment for the systemic press/release-spam bug this
    // prevents.
    menuGamepadFrameGuard.markRealUpdateRan();

    // Swing-gesture -> attack, LEFT hand: added 2026-08-05 per explicit user
    // request ("swinging your left hand in front of you acts as pressing the
    // b button"). A right-hand SWORD-swing version of this was drafted
    // 2026-08-03 then explicitly deferred (see git history: fixed a dead
    // `!pacing.is_interpolating` gate that meant the detector's own
    // `.update()` had never actually run, then pulled the wiring back out
    // without having tested it) -- the detector/vr_swing_detector.hpp were
    // left in place specifically so this didn't need to be re-derived from
    // scratch, and were later repurposed as g_rightThrust (below) for the
    // actually-requested right-hand gesture (shield bash), not sword
    // attacks. Using the LEFT hand for the SWORD gesture specifically is
    // deliberate, not arbitrary: section 16 of vr-mod-notes established the
    // sword is Link's LEFT-hand item (mLeftItemJntNo), so swinging the hand
    // that's actually holding the sword is the physically-intuitive gesture,
    // where a right-hand swing never would have been.
    //
    // vr_combat::SwingDetector is engine-agnostic (see vr_swing_detector.hpp)
    // and only needs a position + monotonic timestamp per frame -- feed it
    // the left grip pose already located above and the frame's predicted
    // display time (XrTime is int64 nanoseconds; converting to seconds here
    // is just for the detector's dt math, no epoch meaning is assumed
    // anywhere). `triggered` is a one-frame edge (the detector itself
    // enforces a cooldown + must-drop-below-resetSpeed-to-rearm hysteresis so
    // one swing can't repeat-fire).
    const vr_combat::Pose leftSwingPose{
        {leftPose.position.x, leftPose.position.y, leftPose.position.z},
        static_cast<double>(time) * 1e-9};
    const vr_combat::SwingEvent leftSwingEvent = g_leftSwing.update(leftSwingPose);

    // FIX (2026-08-13) -- user report: "swinging fast left and right
    // repeatedly is the equivalent of pressing B repeatedly on a
    // controller, and it did not attack repeatedly" -- ruled out (by the
    // user, correctly) that this is Link's own attack-animation lock, since
    // mashing the REAL button does attack repeatedly. A fresh
    // [dusk::vr::swingdiag] capture (round 4's resetSpeed fix already
    // landed) showed the detector itself firing frequently and reliably
    // throughout continuous swinging, so the gap is downstream of
    // detection. Root cause: this file's own "KNOWN LATENCY" comment
    // (above, at the wantsVirtualPad block) already documents that
    // mDoCPd_c::read() -- the actual game-logic button read -- runs on the
    // ~30Hz SIM-TICK loop, BEFORE dusk::vr::tick() (this function) even
    // runs that frame. A REAL held button stays "on" across many real
    // frames (the physical trigger/click is genuinely held), so SOME sim
    // tick is guaranteed to see it -- but leftSwingEvent.triggered was
    // previously OR'd into PAD_BUTTON_B for exactly ONE real frame
    // (~15-20ms at this project's typical VR framerate), which is SHORTER
    // than the ~33ms gap between sim-tick reads. A one-frame pulse has a
    // real chance of landing entirely in the dead zone between two sim
    // ticks and never being read at all -- independently, per swing,
    // explaining both "sometimes the first one lands" (luck of frame
    // alignment) and "repeated fast swings don't" (each one separately
    // rolls the same bad odds). Fix: latch the swing-triggered B press
    // for a short WALL-CLOCK duration instead of a single frame, long
    // enough to comfortably span at least one (with margin, several)
    // sim-tick reads -- same effect a real held button already gets for
    // free. 100ms is ~3x a 30Hz sim-tick period; short enough to still
    // read as instantaneous to the player, same as how a real quick
    // button tap already produces a many-real-frame-long "held" pulse
    // rather than a true single-frame one.
    constexpr double kSwingButtonHoldSec = 0.1;
    static double s_leftSwingButtonHoldRemaining = 0.0;
    if (leftSwingEvent.triggered) {
        s_leftSwingButtonHoldRemaining = kSwingButtonHoldSec;
    } else {
        s_leftSwingButtonHoldRemaining =
            std::max(0.0, s_leftSwingButtonHoldRemaining - static_cast<double>(pacing.presentation_dt_seconds));
    }
    const bool leftSwingButtonHeld = s_leftSwingButtonHoldRemaining > 0.0;

    // Thrust-gesture -> shield bash, RIGHT hand: added 2026-08-13 per
    // explicit user request ("if you thrust the right controller it should
    // press R basically, as R is shield bash"). Same vr_combat::SwingDetector
    // machinery as the left-hand sword gesture above, fed rightPose instead
    // (already located earlier in tick(), same as leftPose).
    const vr_combat::Pose rightThrustPose{
        {rightPose.position.x, rightPose.position.y, rightPose.position.z},
        static_cast<double>(time) * 1e-9};
    const vr_combat::SwingEvent rightThrustEvent = g_rightThrust.update(rightThrustPose);

    // Getting a shield bash to actually register is a genuinely different
    // problem than the sword's B-latch fix above, not just a copy-paste of
    // it. Traced the real game logic first rather than assuming (d_a_alink.cpp):
    // shield bash fires via daAlink_c::spActionTrigger() -> itemTriggerCheck(
    // BTN_R) -> mItemTrigger & BTN_R, and mItemTrigger's BTN_R bit is only
    // ever set by mDoCPd_c::getTrigLockR(PAD_1) -- a genuine RISING-EDGE
    // detector (0->1 transition only), not a hold check. Raising the shield
    // (below, driven by left squeeze) already holds PAD_TRIGGER_R
    // CONTINUOUSLY the whole time the shield is up -- so if the player is
    // already blocking (very likely; that's the normal way you'd want to
    // then bash) and just thrusts the right controller, R never actually
    // goes 0->1 from the game's point of view (it was already 1), so a bash
    // would never fire no matter how the trigger event itself is latched --
    // this is a fundamentally different problem from the sword case, where B
    // starts from a real 0 every time. Fix: on a thrust trigger, force a
    // brief RELEASE window first (guaranteeing a real 0 sample reaches at
    // least one sim tick even if squeeze is held), THEN force a brief HOLD
    // window (guaranteeing a real 1 sample reaches at least one MORE sim
    // tick right after) -- a genuine, detectable 0->1->(back to whatever
    // squeeze wants) pulse, regardless of the left hand's current squeeze
    // state. kThrustForceReleaseSec (50ms) is comfortably longer than one
    // ~33ms 30Hz sim-tick period on its own, same reasoning as the sword
    // fix's 100ms hold; kThrustHoldSec (100ms) mirrors the sword fix
    // directly once the release window has done its job.
    constexpr double kThrustForceReleaseSec = 0.05;
    constexpr double kThrustHoldSec = 0.1;
    static double s_rightThrustForceReleaseRemaining = 0.0;
    static double s_rightThrustHoldRemaining = 0.0;
    if (rightThrustEvent.triggered) {
        s_rightThrustForceReleaseRemaining = kThrustForceReleaseSec;
        s_rightThrustHoldRemaining = 0.0;  // restart the release phase even if a
                                            // previous pulse's hold was still running
    } else {
        const double dtSec = static_cast<double>(pacing.presentation_dt_seconds);
        if (s_rightThrustForceReleaseRemaining > 0.0) {
            s_rightThrustForceReleaseRemaining = std::max(0.0, s_rightThrustForceReleaseRemaining - dtSec);
            if (s_rightThrustForceReleaseRemaining <= 0.0) {
                s_rightThrustHoldRemaining = kThrustHoldSec;  // release window just
                                                                // elapsed -- start the assert window
            }
        } else if (s_rightThrustHoldRemaining > 0.0) {
            s_rightThrustHoldRemaining = std::max(0.0, s_rightThrustHoldRemaining - dtSec);
        }
    }
    const bool rightThrustForceRelease = s_rightThrustForceReleaseRemaining > 0.0;
    const bool rightThrustForceHold = s_rightThrustHoldRemaining > 0.0;

    // FISHING HOOKSET (2026-08-14) -- user report: "the fish bite and when
    // I pull they just let go." Traced the real minigame code first (see
    // vr_link::isFishingHookInWater()'s own comment, vr_link_visibility.hpp,
    // for the full reasoning): hook-setting reads dmg_rod_class's
    // rod_stick_y < -0.5f -- the MAIN/left stick pulled sharply back, not
    // the C-stick -- and VR's left thumbstick already correctly feeds that
    // value, so nothing was actually broken. The fix is a UX one: reuse the
    // SAME right-hand fast-motion detector already driving the shield-bash
    // thrust (g_rightThrust, above) to ALSO force a stick-down pulse while
    // the rod's hook is in the water, so a physical yank of the rod hand
    // sets the hook instead of requiring a thumbstick flick. Deliberately
    // NOT a second tuned SwingDetector instance -- overloading g_rightThrust
    // is harmless: forcing R while fishing does nothing (no shield
    // equipped), and this stick pulse does nothing unless the game's own
    // (untouched) mRemainingHookTime bite window is currently open.
    // A plain level-hold (no release-then-assert phase, unlike the R-button
    // fix above) is sufficient here -- rod_stick_y is read as a continuous
    // value every sim tick, not an edge, so just holding it low for a few
    // sim-tick periods guarantees at least one real read catches it.
    constexpr double kRodYankStickHoldSec = 0.15;
    static double s_rodYankStickHoldRemaining = 0.0;
    if (rightThrustEvent.triggered && dusk::vr::isFishingHookInWater()) {
        s_rodYankStickHoldRemaining = kRodYankStickHoldSec;
    } else {
        s_rodYankStickHoldRemaining =
            std::max(0.0, s_rodYankStickHoldRemaining - static_cast<double>(pacing.presentation_dt_seconds));
    }
    const bool rodYankForceStickDown = s_rodYankStickHoldRemaining > 0.0;

    // DIAGNOSTIC (temporary -- added 2026-08-05 to investigate "swings when
    // I move my hand normally, doesn't trigger on a real swing"). Two
    // things to check with real data instead of guessing again: (1) is the
    // detector's dt source (differencing predictedDisplayTime, an XrTime
    // meant for pose PREDICTION, not guaranteed to be a clean wall-clock
    // delta between calls) glitching to something tiny/unstable and
    // inflating ordinary jitter into a false "swing" -- logged side by side
    // against pacing.presentation_dt_seconds (the real measured frame time,
    // already used for updateSmoothTurn()) so the two can be compared
    // directly; and (2) what does the instantaneous speed actually look
    // like during a real intended swing vs. normal movement -- the
    // triggerSpeed/resetSpeed tuning pass earlier this session was a guess
    // without this data. Throttled to ~9Hz (every 10 frames, matching
    // section 12's proven capture cadence) so a ~15-20s capture (do a few
    // seconds of normal hand movement, then a few real swings) stays
    // readable; every actual trigger is logged unconditionally regardless
    // of the throttle since triggers are already rate-limited by the
    // detector's own cooldown. Remove once the detector's actual behavior
    // is understood and confirmed fixed -- this project's normal practice.
    {
        static XrVector3f s_prevPos{};
        static double s_prevTimeSec = 0.0;
        static bool s_hasPrev = false;
        static int s_frameCounter = 0;
        const double nowSec = static_cast<double>(time) * 1e-9;
        if (s_hasPrev) {
            ++s_frameCounter;
            const float dx = leftPose.position.x - s_prevPos.x;
            const float dy = leftPose.position.y - s_prevPos.y;
            const float dz = leftPose.position.z - s_prevPos.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double predDt = nowSec - s_prevTimeSec;
            const double pacingDt = static_cast<double>(pacing.presentation_dt_seconds);
            const float speedPredDt = predDt > 0.0 ? static_cast<float>(dist / predDt) : -1.f;
            const float speedPacingDt = pacingDt > 0.0 ? static_cast<float>(dist / pacingDt) : -1.f;
            if (leftSwingEvent.triggered || (s_frameCounter % 10) == 0) {
                char msg[256];
                _snprintf_s(msg, _TRUNCATE,
                    "[dusk::vr::swingdiag] pos=(%.4f,%.4f,%.4f) predDt=%.5f pacingDt=%.5f "
                    "dist=%.4f speedPredDt=%.3f speedPacingDt=%.3f TRIGGERED=%d\n",
                    leftPose.position.x, leftPose.position.y, leftPose.position.z,
                    predDt, pacingDt, dist, speedPredDt, speedPacingDt,
                    leftSwingEvent.triggered ? 1 : 0);
                OutputDebugStringA(msg);
            }
        }
        s_prevPos = leftPose.position;
        s_prevTimeSec = nowSec;
        s_hasPrev = true;
    }

    PADStatus padStatus{};
    padStatus.err = PAD_ERR_NONE;
    if (rightAHeld) padStatus.button |= PAD_BUTTON_A;
    if (rightBHeld || leftSwingButtonHeld) padStatus.button |= PAD_BUTTON_B;
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
    if (rightTrigger > kTriggerDeadzone) padStatus.button |= PAD_BUTTON_Y;   // was analog R
    if (leftTrigger > kTriggerDeadzone) {
        padStatus.button |= PAD_TRIGGER_L;
        padStatus.triggerLeft = static_cast<u8>(std::clamp(leftTrigger, 0.f, 1.f) * 255.f);
    }
    // R = raise shield (held, driven by left squeeze) OR a shield-bash
    // thrust pulse (see rightThrustForceRelease/Hold's own comment above for
    // why a plain OR isn't enough by itself -- the force-release phase can
    // override an existing squeeze-hold to guarantee spActionTrigger() sees
    // a real 0->1 edge).
    bool rWantsHeld = leftSqueeze > kTriggerDeadzone;
    if (rightThrustForceRelease) {
        rWantsHeld = false;
    } else if (rightThrustForceHold) {
        rWantsHeld = true;
    }
    if (rWantsHeld) {
        padStatus.button |= PAD_TRIGGER_R;  // analog R / raise shield -- was unbound
        const float rAnalog = rightThrustForceHold ? 1.0f : std::clamp(leftSqueeze, 0.f, 1.f);
        padStatus.triggerRight = static_cast<u8>(rAnalog * 255.f);
    }
    if (std::abs(leftStick.x) > kStickDeadzone || std::abs(leftStick.y) > kStickDeadzone) {
        padStatus.stickX = static_cast<s8>(std::clamp(leftStick.x, -1.f, 1.f) * 127.f);
        padStatus.stickY = static_cast<s8>(std::clamp(leftStick.y, -1.f, 1.f) * 127.f);
    }
    // Fishing hookset yank -- see rodYankForceStickDown's own comment above.
    // Overrides whatever the real left stick reported this frame (harmless:
    // only active for a brief pulse, only while the rod's hook is actually
    // in the water).
    if (rodYankForceStickDown) {
        padStatus.stickY = -127;
    }
    // Right thumbstick: UNBOUND from the C-stick/substick as of 2026-08-05
    // (see the mapping comment above) -- deliberately does NOT write
    // padStatus.substickX/Y anymore. Drives VR smooth-turn instead; see
    // updateSmoothTurn() below.
    //
    // EXCEPTION (2026-08-14) -- user report: the right-hand-yank hookset
    // gesture (rodYankForceStickDown, above) didn't fix "moving the rod
    // still unhooks the fish"; explicit follow-up request: "bind C stick
    // to the right stick, but only while you are fishing." While
    // isFishingRodActive() (vr_link_visibility.hpp) is true, the right
    // stick reverts to its ORIGINAL flatscreen role -- writes
    // padStatus.substickX/Y (same clamp/scale as the left stick's write
    // above) instead of driving smooth-turn, restoring real C-stick input
    // to d_a_mg_rod.cpp's cast-power/direction and rod-tip-steering logic
    // (rod_substick_x/y). Smooth-turn is deliberately SKIPPED (not just
    // fed zero) for these frames -- the physical stick is doing fishing
    // input instead, and the original flatscreen controls never used the
    // C-stick for camera turn while fishing either. The yank-gesture fix
    // from earlier this session is left in place, not reverted -- this is
    // an additional/alternative control, not a replacement.
    if (dusk::vr::isFishingRodActive()) {
        if (std::abs(rightStick.x) > kStickDeadzone || std::abs(rightStick.y) > kStickDeadzone) {
            padStatus.substickX = static_cast<s8>(std::clamp(rightStick.x, -1.f, 1.f) * 127.f);
            padStatus.substickY = static_cast<s8>(std::clamp(rightStick.y, -1.f, 1.f) * 127.f);
        }
    } else {
        dusk::vr::updateSmoothTurn(rightStick.x, pacing.presentation_dt_seconds);

        // Real physical gamepad's C-stick (2026-08-19, explicit user
        // request: "make the c stick function, c left and c right, rotate
        // the camera left and right in the same way that moving the right
        // stick rotates the hmd direction"). A player using a real gamepad
        // alongside VR motion controllers can already push its C-stick
        // left/right to orbit the flatscreen third-person camera --
        // mDoCPd_c::getSubStickX(PAD_1) below reads the exact same value
        // d_camera.cpp's own free-camera-control code reads, untouched by
        // this change -- but that orbit never reached the HEADSET's own
        // view rotation, only smooth-turn does: eyePoseToViewMtx's yawRad
        // parameter (vr_stereo_render.hpp) shows the VR eye's rotation
        // always comes from the real HMD pose plus smooth-turn's yaw
        // offset, never from d_camera.cpp's own camera orientation.
        // Feeding the real C-stick's X axis into the SAME
        // updateSmoothTurn() the VR right thumbstick already drives closes
        // that gap -- both inputs advance the same g_smoothTurnYawRad
        // accumulator, so they add rather than fight. Skipped while
        // fishing for the same reason the VR right stick is above: the
        // C-stick is already doing fishing input on those frames (real
        // hardware feeds d_a_mg_rod.cpp's rod_substick_x/y from this same
        // getSubStickX() call), so it shouldn't also spin the camera.
        const float realCStickX = mDoCPd_c::getSubStickX(PAD_1);
        dusk::vr::updateSmoothTurn(realCStickX, pacing.presentation_dt_seconds);
    }

    // Scripted-camera facing assist (2026-08-19 request, Third Person VR
    // setting only). Two independent mechanisms, split same day per an
    // explicit follow-up request to treat Z-targeting differently from
    // cutscenes -- see each block's own comment for why. Both deliberately
    // scoped to Third Person mode only (plain first-person VR already
    // anchors the camera to Link's own head/core, where this wouldn't make
    // sense) and computed BEFORE the g_headMoveAngleS block right below, so
    // movement direction stays in sync with whatever yaw either one
    // converges to this same frame rather than reading a one-frame-stale
    // value.

    // --- Cutscenes: jump-cut detection only ---
    //
    // REDESIGNED 2026-08-19 after two rejected continuous-pull attempts --
    // see snapScriptedCameraYaw()'s own comment (vr_smooth_turn.hpp) for
    // that history. Detects a JUMP CUT -- the flatscreen camera's own
    // facing direction changing by a large amount within a single frame
    // (a cutscene shot change) -- and instantly snaps g_smoothTurnYawRad to
    // match only then; the rest of the time (including during a real
    // cutscene between cuts), free-look is completely untouched.
    //
    // Z-targeting used to share this same jump-cut path (its initial
    // engage snap is itself a jump), but was split out into its own block
    // below per explicit follow-up request: unlike a cutscene, the Z-target
    // camera's swing INTO its resting position behind Link is a smooth,
    // continuous transition (never a single-frame jump), so jump-cut
    // detection alone never tracked that swing -- only the very first
    // instant of it.
    {
        // Persists across frames/activations -- deliberately NOT reset on
        // a false->true transition, since the "just activated" branch below
        // never reads it (only writes it fresh for the next frame's
        // comparison), and the "still active" branch's very first read
        // after a fresh activation is guarded by that same branch split.
        static s16 s_cutsceneJumpCutLastTargetYawS = 0;
        static bool s_cutsceneJumpCutWasActive = false;

        const bool cutsceneActive =
            dusk::getSettings().game.vrThirdPerson.getValue() &&
            dusk::vr::isRealCutsceneRunning();

        if (cutsceneActive) {
            if (view_class* view = dComIfGd_getView()) {
                const float dx = view->lookat.center.x - view->lookat.eye.x;
                const float dz = view->lookat.center.z - view->lookat.eye.z;
                // Skip a degenerate/zero-length lookat direction (e.g. a
                // stray frame where eye and center coincide) rather than
                // feeding cM_atan2s(0, 0) an undefined angle.
                if (std::abs(dx) > 0.0001f || std::abs(dz) > 0.0001f) {
                    const s16 targetYawS = cM_atan2s(dx, dz);

                    // s16 subtraction wraps to the shortest signed angular
                    // gap automatically (standard BAMS convention, same
                    // trick this engine's own angle-delta code relies on
                    // elsewhere).
                    const bool isCut = !s_cutsceneJumpCutWasActive ||
                        std::abs(cM_s2rad(static_cast<s16>(
                            targetYawS - s_cutsceneJumpCutLastTargetYawS))) >=
                            cM_s2rad(cM_deg2s(dusk::vr::kScriptedCameraJumpCutThresholdDeg));

                    if (isCut) {
                        const cXyz currentHeadForward = vr_render::computeHeadWorldForward(
                            hmdPose, dusk::vr::getSmoothTurnYawRad());
                        const s16 currentYawS =
                            cM_atan2s(currentHeadForward.x, currentHeadForward.z);
                        const s16 gapS = static_cast<s16>(targetYawS - currentYawS);
                        dusk::vr::snapScriptedCameraYaw(cM_s2rad(gapS));
                    }

                    s_cutsceneJumpCutLastTargetYawS = targetYawS;
                }
            }
        }

        s_cutsceneJumpCutWasActive = cutsceneActive;
    }

    // --- Z-targeting: track the swing-in, then release to free-look ---
    //
    // Explicit follow-up request: "z targeting always makes the camera face
    // the right way, until it is centered behind link. Once it is behind
    // Link you should be able to look around, even while targeting. But
    // the initial camera movement should face him." Unlike the cutscene
    // block above, this fully snaps g_smoothTurnYawRad to match the
    // flatscreen Z-target camera EVERY FRAME while that camera is still
    // visibly swinging into position -- not just once. This is safe/
    // correct specifically because the source being copied (the flatscreen
    // camera's own already-smooth swing-in animation) is itself smooth, so
    // mirroring it frame-by-frame reads as smooth tracking rather than the
    // earlier rejected continuous-pull design's jitter/fight (that design's
    // problem was fighting the PLAYER's own head input every frame, not
    // tracking a smooth source -- this only ever runs during the brief,
    // one-time swing-in, not for the whole duration of the hold).
    //
    // "Settled" (swing-in finished, hand control back to the player) is
    // detected via TWO independent signals, same "real settle + a bounded
    // fallback" shape this codebase already uses for the core-anchor
    // calibration (vr_link_visibility.hpp's computeRawCoreAnchoredEye()):
    // several consecutive frames where the camera's own per-frame movement
    // has dropped below a small threshold (it's actually stopped moving),
    // OR a generous max duration elapses regardless (so ongoing camera
    // micro-adjustments from the player continuing to move while locked on
    // -- a normal, expected thing, not part of the initial swing -- can't
    // indefinitely withhold free-look). Once settled, this state machine
    // does nothing for the REST of that Z-target hold, even if the
    // flatscreen camera keeps adjusting afterward (e.g. circling the
    // target) -- exactly the "once it is behind Link... even while
    // targeting" free-look guarantee that was asked for. Releasing and
    // re-engaging Z-target resets back to the tracking phase.
    {
        enum class ZTargetTrack : u8 { Idle, Tracking, Settled };
        static ZTargetTrack s_zTargetTrackState = ZTargetTrack::Idle;
        static s16 s_zTargetLastTargetYawS = 0;
        static int s_zTargetSettleStreak = 0;
        static float s_zTargetTrackElapsedSec = 0.f;

        bool zTargetActive = false;
        if (dusk::getSettings().game.vrThirdPerson.getValue()) {
            if (auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer())) {
                zTargetActive = link->checkAttentionLock();
            }
        }

        if (!zTargetActive) {
            s_zTargetTrackState = ZTargetTrack::Idle;
            s_zTargetSettleStreak = 0;
            s_zTargetTrackElapsedSec = 0.f;
        } else if (view_class* view = dComIfGd_getView()) {
            const float dx = view->lookat.center.x - view->lookat.eye.x;
            const float dz = view->lookat.center.z - view->lookat.eye.z;
            if (std::abs(dx) > 0.0001f || std::abs(dz) > 0.0001f) {
                const s16 targetYawS = cM_atan2s(dx, dz);

                if (s_zTargetTrackState == ZTargetTrack::Idle) {
                    // Just engaged this frame -- start tracking and snap
                    // immediately (no valid "last frame" delta to compare
                    // against yet, and the player's view was very likely
                    // facing nothing like the target direction the moment
                    // before this).
                    s_zTargetTrackState = ZTargetTrack::Tracking;
                    s_zTargetSettleStreak = 0;
                    s_zTargetTrackElapsedSec = 0.f;
                    const cXyz currentHeadForward = vr_render::computeHeadWorldForward(
                        hmdPose, dusk::vr::getSmoothTurnYawRad());
                    const s16 currentYawS =
                        cM_atan2s(currentHeadForward.x, currentHeadForward.z);
                    dusk::vr::snapScriptedCameraYaw(
                        cM_s2rad(static_cast<s16>(targetYawS - currentYawS)));
                } else if (s_zTargetTrackState == ZTargetTrack::Tracking) {
                    s_zTargetTrackElapsedSec += pacing.presentation_dt_seconds;

                    const float deltaDeg = std::abs(cM_s2rad(static_cast<s16>(
                        targetYawS - s_zTargetLastTargetYawS))) * (180.f / 3.14159265358979323846f);
                    if (deltaDeg < dusk::vr::kZTargetCameraSettleThresholdDeg) {
                        ++s_zTargetSettleStreak;
                    } else {
                        s_zTargetSettleStreak = 0;
                    }

                    const bool settled =
                        s_zTargetSettleStreak >= dusk::vr::kZTargetCameraSettleRequiredConsecutiveFrames ||
                        s_zTargetTrackElapsedSec >= dusk::vr::kZTargetCameraTrackMaxDurationSec;

                    if (settled) {
                        s_zTargetTrackState = ZTargetTrack::Settled;
                    } else {
                        const cXyz currentHeadForward = vr_render::computeHeadWorldForward(
                            hmdPose, dusk::vr::getSmoothTurnYawRad());
                        const s16 currentYawS =
                            cM_atan2s(currentHeadForward.x, currentHeadForward.z);
                        dusk::vr::snapScriptedCameraYaw(
                            cM_s2rad(static_cast<s16>(targetYawS - currentYawS)));
                    }
                }
                // ZTargetTrack::Settled -- do nothing; full free-look for
                // the rest of this Z-target hold.

                s_zTargetLastTargetYawS = targetYawS;
            }
        }
    }

    // See g_headMoveAngleS's declaration comment for the bug this fixes.
    // Computed here (once per frame, not per eye) rather than lazily in
    // getHeadMoveAngleS() itself, matching this file's existing pattern for
    // per-frame-cached values (g_smoothTurnYawRad above) -- hmdPose is
    // already available (located earlier in tick()) and the smooth-turn
    // yaw offset was just updated on the line above, so both inputs this
    // needs are fresh for this frame.
    {
        const cXyz headForward =
            vr_render::computeHeadWorldForward(hmdPose, dusk::vr::getSmoothTurnYawRad());
        g_headMoveAngleS = cM_atan2s(headForward.x, headForward.z);

        // HMD-based aim pitch (see getHeadAimAngles()'s own comment,
        // vr_main.hpp) -- same horizontal-length/atan2s(y, horiz) shape and
        // same negation as g_controllerAimPitchS below, on the theory that
        // the sign requirement comes from mBodyAngle.x's own convention
        // (what both pitches ultimately feed, d_a_alink_link.inc), not from
        // which vector produced the y-component. Untested assumption --
        // flip the sign here first if head-based aim pitch reads inverted
        // in-headset, same "guess, test, flip if backwards" caveat every
        // other rotation-sign guess in this project has needed at least
        // once before landing.
        const float horiz = std::sqrt(headForward.x * headForward.x + headForward.z * headForward.z);
        g_headAimPitchS = static_cast<s16>(-cM_atan2s(headForward.y, horiz));
    }

    // Right-controller-pointing aim direction -- see g_controllerAimYawS/
    // g_controllerAimPitchS's own declaration comment. yaw/pitch formulas
    // match g_headMoveAngleS's own cM_atan2s(x, z) / cM_atan2s(y,
    // horizontalLength) conventions just above (already proven correct
    // there for the HMD's own forward direction).
    //
    // REJECTED, first attempt: sourced this from buildHandMtx()'s grip-based
    // mesh-forward calibration. Real in-headset test found yaw off by a
    // FIXED 90 degrees AND -- the more important finding -- physically
    // yawing the controller made pitch/roll appear to swap, direct evidence
    // of a wrong SOURCE vector, not just a wrong constant. A follow-up
    // angle-space patch (subtract 90 degrees from yaw, negate pitch) was
    // reverted without even testing it -- per this project's own hard-won
    // "don't attempt a column swap to fix an axis-confusion symptom --
    // provably cannot work" lesson (CLAUDE.md section 12's algebraic proof),
    // a uniform correction (matrix column swap OR constant angle offset)
    // cannot change which physical rotation axis feeds which computed
    // output, only the resting orientation -- so patching the angle output
    // further was never going to fix an axis-confusion symptom. See
    // vr_link::computeControllerAimForward()'s own comment
    // (vr_link_visibility.hpp) for the full writeup and the current
    // approach (OpenXR's own aim pose, rightAimPose, instead of the grip
    // pose + mesh calibration).
    //
    // CONFIRMED in-headset (round 2): the aim-pose switch fixed the
    // axis-confusion symptom entirely -- yaw and pitch now respond only to
    // their own physical motion, no bleed-through -- leaving just a plain
    // inverted pitch. Unlike round 1's rejected pitch negation (which was
    // discarded alongside a wrong SOURCE vector, so it was never actually
    // safe to trust in isolation), this negation is now applied on top of
    // a confirmed-correctly-separated source, which is exactly the class
    // of fix a plain sign flip IS capable of (see the "column swap" lesson
    // above -- it only ever objected to using a uniform correction to fix
    // axis MIXING, not to a sign flip on an already axis-clean value).
    {
        const vr_link::Vec3f aimForward =
            vr_link::computeControllerAimForward(rightAimPose, dusk::vr::getSmoothTurnYawRad());
        const float horiz = std::sqrt(aimForward.x * aimForward.x + aimForward.z * aimForward.z);
        g_controllerAimYawS = cM_atan2s(aimForward.x, aimForward.z);
        g_controllerAimPitchS = static_cast<s16>(-cM_atan2s(aimForward.y, horiz));
    }

    constexpr u32 kVrPadPort = PAD_CHAN0;
    // substickX/Y checks re-added 2026-08-14 -- section 15 dropped them as
    // "always zero" when the C-stick was fully unbound from padStatus; the
    // fishing-only C-stick rebind above (isFishingRodActive()) can now
    // write real nonzero values here again, and those need to reach
    // PADSetVirtualStatus() even when every other field is zero (e.g. pure
    // C-stick casting input with the rest of the controller idle).
    const bool wantsVirtualPad = padStatus.button != 0 || padStatus.stickX != 0 ||
                                  padStatus.stickY != 0 || padStatus.triggerLeft != 0 ||
                                  padStatus.triggerRight != 0 || padStatus.substickX != 0 ||
                                  padStatus.substickY != 0;
    if (wantsVirtualPad) {
        PADSetVirtualStatus(kVrPadPort, &padStatus);
    } else {
        PADClearVirtualStatus(kVrPadPort);
    }

    // --- Link head hide + hand matrix mapping ---
    vr_link::FrameInput frameInput{hmdPose, rightPose, leftPose, rightAimPose, leftAimPose,
                                    dusk::vr::getSmoothTurnYawRad()};
    vr_link::updateFrame(frameInput);

    // ACTUAL FIX for section 20's persistent hand-lag bug (2026-08-09) --
    // must run AFTER updateFrame() above (computes this frame's tracked
    // matrices into vr_link::detail::s_rightHandMtx/s_leftHandMtx) and
    // BEFORE the per-eye loop below opens either eye's real draw. See
    // refreshTrackedHandDrawMtxLive()'s own comment (vr_link_visibility.hpp)
    // for the full root-cause writeup: applyTrackedHandMtx()'s existing
    // per-eye call site (d_a_alink.cpp, inside daAlink_c::draw()) was
    // proven, via a full-session [dusk::vr::eyepasscheck] log capture, to
    // never actually run during a real VR eye pass -- this call site does.
    if (auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer())) {
        dusk::vr::refreshTrackedHandDrawMtxLive(link->getHandModel());
    }

    // Same fix, same reason, extended to sword/shield (2026-08-09 follow-up
    // -- user-confirmed hands fixed, sword/shield still laggy, same root
    // cause). See refreshTrackedItemMtxLive()'s own comment.
    dusk::vr::refreshTrackedItemMtxLive();

    // Same fix, extended further to mHeldItemModel/mpKanteraModel (bow,
    // bottles, lantern, etc.). See refreshTrackedHeldItemMtxLive()'s own
    // comment (vr_link_visibility.hpp).
    dusk::vr::refreshTrackedHeldItemMtxLive();

    // Extends tracking to getLeftItemMatrix()/getRightItemMatrix()
    // themselves (fixes fishing rod/boomerang/nocked arrows/etc. -- see
    // refreshTrackedItemJointMtxLive()'s own comment, vr_link_visibility.hpp).
    dusk::vr::refreshTrackedItemJointMtxLive();

    // The held (not-yet-thrown) boomerang's OWN base-transform setter only
    // samples getLeftItemMatrix() once per sim tick (see
    // refreshTrackedBoomerangMtxLive()'s own comment) -- re-run it here at
    // real frame rate too, same shape as every other live-refresh above.
    dusk::vr::refreshTrackedBoomerangMtxLive();

    // Same fix, same reason, extended to the fishing rod -- see
    // refreshTrackedFishingRodMtxLive()'s own comment.
    dusk::vr::refreshTrackedFishingRodMtxLive();

    // Same fix, extended to the clawshot's hand-grip models (chain itself
    // deliberately not touched this round -- see
    // refreshTrackedHookshotMtxLive()'s own comment).
    dusk::vr::refreshTrackedHookshotMtxLive();

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
    vr_render::updateHudSmoothing(hmdPose, dusk::vr::getSmoothTurnYawRad());

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

    // VR menu billboard, Phase 2 plan step 4 (see vr_stereo_render.hpp's
    // own "VR menu billboard" section comment for the full mechanism):
    // step 3's solid-color test CONFIRMED the draw mechanism works
    // in-headset, so this now copies the REAL RmlUi content each frame.
    // Gated on menuVisible (computed earlier this frame, above, for the
    // menu-gamepad call) so this costs nothing when no document is open.
    if (menuVisible) {
        vr_render::ensureAndCopyMenuBillboardTexture();
    }

    // Desktop mirror: captured from eye 0 (left) inside the loop below,
    // applied once after it. See aurora::gfx::set_present_source_mirror()'s
    // own comment for the mechanism; this is just where VR code decides
    // WHICH texture and WHEN.
    aurora::gfx::ResolvedTargets mirrorEyeTargets;

    for (uint32_t eye = 0; eye < viewCount; ++eye) {
        vr_render::EyeParams eyeParams{
            views[eye].pose,
            views[eye].fov,
            configViews[eye].recommendedImageRectWidth,
            configViews[eye].recommendedImageRectHeight,
            hmdPose.position,
            vrCameraEyeAnchor,
            dusk::vr::getSmoothTurnYawRad(),
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

        // World-space aim-point marker ("physical crosshair") -- drawn
        // after the world/HUD (cAPIGph_Painter() above) so it's properly
        // depth-tested against real scene geometry, but still inside this
        // eye's open pass. Reuses daAlink_c::mSight, the same shared aim
        // point already driving the flatscreen bow/slingshot/hookshot/
        // boomerang 2D reticle -- see drawAimCrosshair()'s own comment
        // (vr_stereo_render.hpp) for the full "all items" scoping.
        if (auto* link = static_cast<daAlink_c*>(dComIfGp_getLinkPlayer())) {
            if (link->getAimSightVisible()) {
                vr_render::drawAimCrosshair(*link->getLineTopPosP());
            }
        }

        // VR menu billboard, Phase 2 plan step 4 -- real RmlUi content
        // (see the per-frame copy above and vr_stereo_render.hpp's own
        // "VR menu billboard" section comment). aspectHeightOverWidth is
        // now the real RmlUi render-target aspect (updated per frame by
        // ensureAndCopyMenuBillboardTexture()), not a fixed test value.
        // drawMenuBillboardBackdrop() first -- a guaranteed-opaque solid
        // quad at the same position/size, drawn behind the real content
        // (see its own comment: 2026-08-20 fix for VR reading bright/
        // transparent vs. the flatscreen desktop window's correctly dark/
        // opaque appearance for the exact same menu document).
        //
        // EXCEPT the prelaunch/title screen ("the main menu that says
        // play, settings, quit... and has the logo"), per explicit
        // follow-up request: that screen has its own full-bleed background
        // image (prelaunch.rcss's .background/.gradient, unrelated to
        // window.rcss's shared panel styling other RmlUi windows use) and
        // is meant to stay see-through in VR, unlike every other document
        // -- dusk::ui::is_prelaunch_open() (ui.cpp, already existed) is the
        // real "is THIS the title screen, not some other document"
        // signal. Only the solid backdrop is skipped -- the real menu
        // content (text/logo/buttons) still draws normally via
        // drawMenuBillboard() below, just without the opaque quad behind
        // it, so it reads through to the real VR surroundings.
        if (menuVisible) {
            if (!dusk::ui::is_prelaunch_open()) {
                vr_render::drawMenuBillboardBackdrop(vr_render::g_menuBillboardAspectHeightOverWidth);
            }
            vr_render::drawMenuBillboard(&vr_render::g_menuBillboardTexObj,
                                          vr_render::g_menuBillboardAspectHeightOverWidth);
        }

        // TODO: inject hand mesh before resolving the pass:
        // vr_render::HandPayload payload{ eye == 0 ? leftPose : rightPose };
        // aurora::gfx::push_custom_draw(g_handDrawState.typeId, &payload, sizeof(payload));

        aurora::gfx::ResolvedTargets targets = vr_render::endEye();
        g_duskVREyePassOpen = false;

        // Desktop mirror: eye 0 only, and only when this eye actually
        // resolved this frame (targets.colorTexture null means a foreign-
        // pass substitution ate it -- see the comment right below this one
        // for that whole mechanism). A stale prior-frame mirror image is a
        // much smaller problem than trying to mirror a null texture.
        if (eye == 0 && targets.colorTexture) {
            mirrorEyeTargets = targets;
        }

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

    // Desktop mirror (user request 2026-08-10): show eye 0's just-rendered
    // frame on the desktop window instead of leaving it stale/blank, which
    // is what happens today since m_Do_main.cpp skips the normal flatscreen
    // draw whenever tick() actually renders stereo eyes (see its own
    // "REMOVED this session: a temporary desktop-mirror hack" comment --
    // drawing the whole scene a SECOND time for the desktop was tried and
    // reverted there, confirmed to corrupt state several systems assume
    // runs exactly once per frame). This is a different mechanism entirely:
    // no second draw, just pointing aurora's own present-resample pass
    // (already runs every frame regardless) at a texture that's already
    // been fully rendered for the VR submission. Safe to call unconditionally
    // -- set_present_source_mirror() itself no-ops if mirrorEyeTargets never
    // got populated this frame (eye 0 never resolved).
    if (dusk::getSettings().game.vrDesktopMirror) {
        aurora::gfx::set_present_source_mirror(mirrorEyeTargets);
        // Paired with the reset in tick()'s "reset up front" block -- see
        // aurora::rmlui::set_force_no_backdrop()'s own comment for the real
        // feedback-loop bug this exists to prevent: RmlUi's backdrop-blur
        // content would otherwise sample THIS SAME frame's VR eye texture
        // (present_source(), now mirror-overridden), which itself already
        // contains the menu billboard drawn from LAST frame's RmlUi output
        // -- an unbounded circular dependency, not a cosmetic bug (found
        // 2026-08-16 via a real in-headset report: "my view is looping in
        // the window"). Gated on the mirror having actually been set THIS
        // frame (colorTexture non-null), matching
        // set_present_source_mirror()'s own no-op condition -- if the
        // mirror didn't take (eye 0 never resolved), present_source() falls
        // back to the normal internal framebuffer, which isn't part of this
        // loop at all, so there's nothing to guard against.
        if (mirrorEyeTargets.colorTexture) {
            aurora::rmlui::set_force_no_backdrop(true);
        }
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
