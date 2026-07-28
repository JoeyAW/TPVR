// src/dusk/vr/vr_main.cpp
//
// VR frame-loop integration for Dusklight.
//
// This file is the single seam between the VR subsystem (the five .hpp
// files) and the existing game loop in m_Do/m_Do_main.cpp.
//
// HOW TO WIRE INTO m_Do_main.cpp:
//
//   Two call sites need to change inside main01():
//
//   1. After mDoGph_Create() at the top of main01(), add:
//        dusk::vr::initialize();
//
//   2. Replace the existing frame block:
//
//        // BEFORE (original):
//        if (!aurora_begin_frame()) { continue; }
//        VIWaitForRetrace();
//        ...
//        fapGm_Execute();       // or the interp path
//        ...
//        aurora_end_frame();
//
//        // AFTER (VR):
//        if (!dusk::vr::beginFrame()) { continue; }
//        VIWaitForRetrace();
//        ...
//        fapGm_Execute();       // unchanged — game logic runs once as normal
//        ...
//        dusk::vr::renderAndSubmit(pacing);
//        // aurora_end_frame() is now called inside renderAndSubmit()
//
//   3. In the `exit:` label block at the bottom of main01(), add:
//        dusk::vr::shutdown();
//
// The existing non-VR build is unaffected: every call in this file is
// guarded by #ifdef DUSK_VR_ENABLED, which is only defined when the
// CMakeLists_vr_fragment.cmake is active.

#ifdef DUSK_VR_ENABLED

#include "vr_xr_bootstrap.hpp"
#include "vr_xr_submit.hpp"
#include "vr_stereo_render.hpp"
#include "vr_link_visibility.hpp"
#include "vr_swing_detector.hpp"

// Game headers
#include "m_Do/m_Do_controller_pad.h"   // mDoCPd_c, PAD_1
#include "dusk/frame_interpolation.h"   // dusk::game_clock pacing type
#include "dusk/game_clock.h"

#include <dolphin/pad.h>                // PAD_BUTTON_A

#include <openxr/openxr.h>
#include <aurora/aurora.h>              // aurora_begin_frame, aurora_end_frame

#include <memory>

namespace dusk::vr {

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

namespace {

vr_xr::Bootstrap                    g_boot{};
std::unique_ptr<vr_xr::Session>     g_session;

// Registered once; used every frame to draw the hand mesh
aurora::gfx::DrawTypeId             g_handDrawTypeId = aurora::gfx::InvalidDrawType;

// One swing detector per hand
vr_combat::SwingDetector            g_rightSwing;
vr_combat::SwingDetector            g_leftSwing;

// Last known controller poses (updated each frame from xrLocateSpace).
// Kept as module state so they survive frames where locate fails.
XrPosef g_rightCtrlPose{ {0,0,0,1}, {0,0,0} };
XrPosef g_leftCtrlPose { {0,0,0,1}, {0,0,0} };

// Action spaces for the two grip poses.
// Created once in initialize(), used every frame in locateControllers().
XrSpace g_rightGripSpace = XR_NULL_HANDLE;
XrSpace g_leftGripSpace  = XR_NULL_HANDLE;
XrActionSet g_actionSet  = XR_NULL_HANDLE;
XrAction g_rightGripAction = XR_NULL_HANDLE;
XrAction g_leftGripAction  = XR_NULL_HANDLE;

// Whether initialization succeeded. Checked before every VR call so that
// a headset-not-present scenario degrades gracefully to flat-screen play.
bool g_initialized = false;

} // namespace

// ---------------------------------------------------------------------------
// OpenXR action setup (grip poses for both controllers)
// ---------------------------------------------------------------------------

static void createActionSpaces() {
    // Action set
    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(asci.actionSetName,          "dusklight_vr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(asci.localizedActionSetName, "Dusklight VR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    asci.priority = 0;
    xrCreateActionSet(g_boot.instance, &asci, &g_actionSet);

    // Right grip action
    XrActionCreateInfo raci{ XR_TYPE_ACTION_CREATE_INFO };
    raci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy(raci.actionName,          "right_grip_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(raci.localizedActionName, "Right Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    xrCreateAction(g_actionSet, &raci, &g_rightGripAction);

    // Left grip action
    XrActionCreateInfo laci{ XR_TYPE_ACTION_CREATE_INFO };
    laci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy(laci.actionName,          "left_grip_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(laci.localizedActionName, "Left Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    xrCreateAction(g_actionSet, &laci, &g_leftGripAction);

    // Suggest bindings for common interaction profiles.
    // SteamVR and most runtimes accept the simple_controller profile as a
    // fallback; add more profiles (valve_index, oculus_touch, etc.) here
    // as you expand controller support.
    const XrPath interactionProfile = [&] {
        XrPath p{};
        xrStringToPath(g_boot.instance,
            "/interaction_profiles/khr/simple_controller", &p);
        return p;
    }();

    XrPath rightGripPath{}, leftGripPath{};
    xrStringToPath(g_boot.instance,
        "/user/hand/right/input/grip/pose", &rightGripPath);
    xrStringToPath(g_boot.instance,
        "/user/hand/left/input/grip/pose",  &leftGripPath);

    XrActionSuggestedBinding bindings[] = {
        { g_rightGripAction, rightGripPath },
        { g_leftGripAction,  leftGripPath  },
    };
    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile     = interactionProfile;
    suggested.suggestedBindings      = bindings;
    suggested.countSuggestedBindings = 2;
    xrSuggestInteractionProfileBindings(g_boot.instance, &suggested);
}

static void attachActionsToSession(XrSession session) {
    XrSessionActionSetsAttachInfo attachInfo{
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.actionSets      = &g_actionSet;
    attachInfo.countActionSets = 1;
    xrAttachSessionActionSets(session, &attachInfo);

    // Create action spaces for the grip poses
    XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    sci.poseInActionSpace = { {0,0,0,1}, {0,0,0} };

    sci.action = g_rightGripAction;
    xrCreateActionSpace(session, &sci, &g_rightGripSpace);

    sci.action = g_leftGripAction;
    xrCreateActionSpace(session, &sci, &g_leftGripSpace);
}

// ---------------------------------------------------------------------------
// Locate controllers and feed swing detector
// ---------------------------------------------------------------------------

static void locateControllers(XrTime predictedDisplayTime, XrSpace localSpace) {
    // Sync actions first
    XrActiveActionSet activeSet{ g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.activeActionSets      = &activeSet;
    syncInfo.countActiveActionSets = 1;
    xrSyncActions(g_session->session(), &syncInfo);

    auto locateGrip = [&](XrSpace gripSpace, XrPosef& outPose) {
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(gripSpace, localSpace, predictedDisplayTime, &loc);
        const bool valid =
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (valid) {
            outPose = loc.pose;
        }
        // If not valid, outPose retains its last known good value.
    };

    locateGrip(g_rightGripSpace, g_rightCtrlPose);
    locateGrip(g_leftGripSpace,  g_leftCtrlPose);

    // Feed right controller position into swing detector.
    // predictedDisplayTime is in nanoseconds; convert to seconds.
    const double timeSec =
        static_cast<double>(predictedDisplayTime) * 1e-9;

    vr_combat::Pose rightPose{
        { g_rightCtrlPose.position.x,
          g_rightCtrlPose.position.y,
          g_rightCtrlPose.position.z },
        timeSec
    };
    const auto swingEvent = g_rightSwing.update(rightPose);

    if (swingEvent.triggered) {
        // Synthesize a one-frame A-button press on PAD_1.
        // mPressedButtonFlags = "newly pressed this frame" (edge trigger),
        // mButtonFlags        = "held this frame" (level).
        // The game's attack logic reads getTrigA() which checks
        // mPressedButtonFlags, so setting both is correct for one frame.
        auto& pad = mDoCPd_c::getCpadInfo(PAD_1);
        pad.mPressedButtonFlags |= PAD_BUTTON_A;
        pad.mButtonFlags        |= PAD_BUTTON_A;
        // These flags will be overwritten by the next mDoCPd_c::read() call,
        // so they naturally last exactly one tick — exactly what a button
        // press lasts normally. No cleanup needed.
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void initialize() {
    try {
        g_boot    = vr_xr::initialize();
        g_session = std::make_unique<vr_xr::Session>(g_boot);

        createActionSpaces();
        attachActionsToSession(g_session->session());

        g_handDrawTypeId =
            aurora::gfx::register_draw_type(vr_render::handDrawDescriptor());

        g_initialized = true;
    } catch (const std::exception& e) {
        // No headset present, runtime not installed, or D3D12 not available.
        // Log and fall through to flat-screen mode — the game loop branches
        // on g_initialized so nothing VR-specific runs.
        OSReport("[VR] Failed to initialize: %s\n", e.what());
        OSReport("[VR] Running in flat-screen mode.\n");
    }
}

// Called in place of aurora_begin_frame().
// Returns false if the frame should be skipped (same semantics as
// aurora_begin_frame returning false).
bool beginFrame() {
    if (!g_initialized) {
        return aurora_begin_frame();
    }

    g_session->waitFrame();
    g_session->beginFrame();

    return aurora_begin_frame();
}

// Called in place of aurora_end_frame(), after game logic has run.
// Renders the scene twice (once per eye) into XR swapchain images,
// then calls aurora_end_frame() for the desktop mirror window.
//
// pacing is the same value already computed at the top of the frame;
// pass it through so renderAndSubmit can call the same draw functions
// the original loop would have called.
void renderAndSubmit(const dusk::game_clock::Pacing& pacing) {
    if (!g_initialized) {
        aurora_end_frame();
        return;
    }

    // Locate controllers and synthesize attack input if a swing was detected.
    // This runs before the eye loop so the injected button press is visible
    // to game logic that already ran (fapGm_Execute) for this tick.
    // The flag lasts until the next mDoCPd_c::read() at the start of the
    // next frame — correct one-frame edge trigger behaviour.
    locateControllers(
        g_session->predictedDisplayTime(),
        g_session->localSpace());

    // Update Link's visibility and hand positions (once, not per-eye).
    const XrPosef hmdPose = g_session->getEyeParams(0).pose; // left eye ≈ HMD
    vr_link::updateFrame({ hmdPose, g_rightCtrlPose, g_leftCtrlPose });

    // Render both eyes.
    for (int eye = 0; eye < 2; ++eye) {
        const auto eyeParams = g_session->getEyeParams(eye);

        // Open the offscreen pass and override the camera.
        vr_render::beginEye(eyeParams);

        // Draw the scene into this eye. This is a direct call to the same
        // render functions the original flat-screen loop calls, just with
        // the camera matrix already overridden by beginEye().
        // The interpolation path and the non-interpolation path produce the
        // same rendered output for a given world state; either works here.
        if (pacing.is_interpolating) {
            fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
            cAPIGph_Painter();
        } else {
            // Non-interp: fapGm_Execute already ran the draw inside it.
            // For VR we need to re-run just the draw portion for the second
            // eye. TODO: separate fapGm_Execute's draw from its simulate
            // step so the second eye doesn't re-simulate.
            // For now, call the draw iterator directly as above — it re-draws
            // the world state that fapGm_Execute already advanced.
            fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
            cAPIGph_Painter();
        }

        // Inject the hand mesh at the controller's tracked position.
        if (g_handDrawTypeId != aurora::gfx::InvalidDrawType) {
            vr_render::HandPayload hp{};
            hp.controllerPose = (eye == 0) ? g_rightCtrlPose : g_leftCtrlPose;
            aurora::gfx::push_custom_draw(
                g_handDrawTypeId, &hp, sizeof(hp));
        }

        // Close the offscreen pass and schedule the copy to the XR swapchain.
        const auto targets = vr_render::endEye();
        g_session->submitEye(eye, targets);
    }

    // Present the desktop mirror window (shows whatever was in the EFB pass,
    // i.e. the last eye rendered — fine for a mirror window).
    aurora_end_frame();

    // Submit the completed frame to the XR runtime.
    // Must happen after aurora_end_frame() has flushed Aurora's encoder
    // so the encoder tasks (eye copies) have actually run.
    g_session->endFrame();
}

void shutdown() {
    if (!g_initialized) return;

    vr_link::restoreVisibility();

    if (g_handDrawTypeId != aurora::gfx::InvalidDrawType) {
        aurora::gfx::unregister_draw_type(g_handDrawTypeId);
        g_handDrawTypeId = aurora::gfx::InvalidDrawType;
    }

    if (g_rightGripSpace != XR_NULL_HANDLE) {
        xrDestroySpace(g_rightGripSpace);
        g_rightGripSpace = XR_NULL_HANDLE;
    }
    if (g_leftGripSpace != XR_NULL_HANDLE) {
        xrDestroySpace(g_leftGripSpace);
        g_leftGripSpace = XR_NULL_HANDLE;
    }
    if (g_rightGripAction != XR_NULL_HANDLE) {
        xrDestroyAction(g_rightGripAction);
        g_rightGripAction = XR_NULL_HANDLE;
    }
    if (g_leftGripAction != XR_NULL_HANDLE) {
        xrDestroyAction(g_leftGripAction);
        g_leftGripAction = XR_NULL_HANDLE;
    }
    if (g_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(g_actionSet);
        g_actionSet = XR_NULL_HANDLE;
    }

    g_session.reset();
    g_initialized = false;
}

} // namespace dusk::vr

#endif // DUSK_VR_ENABLED
