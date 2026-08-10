// vr_xr_bootstrap.hpp
//
// Minimal OpenXR instance/system/graphics-requirements bootstrap for the
// D3D12 path, plus XR-side D3D12 device + session creation.
//
// Architecture decision (confirmed this session, see VR_MOD_HANDOFF_2.md):
// this is "outcome 2" -- Aurora's Dawn device is independent of the XR
// runtime's required adapter, so we create a SEPARATE ID3D12Device here
// for the XR session, and share textures across devices via
// wgpu::SharedTextureMemory + fence sync (see vr_xr_submit.hpp).

#pragma once

// Windows and D3D12 headers MUST come before openxr_platform.h.
// openxr_platform.h uses ID3D12Device*, LUID, IUnknown etc. without
// including them itself when XR_USE_GRAPHICS_API_D3D12 is defined.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>  // XrGraphicsRequirementsD3D12KHR, XrGraphicsBindingD3D12KHR
#include <cstring>
#include <stdexcept>
#include <string>

namespace vr_xr {

struct Bootstrap {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrGraphicsRequirementsD3D12KHR d3d12Requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};

    // KHR extension functions aren't statically exported by the loader -
    // they're loaded manually via xrGetInstanceProcAddr below.
    PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsKHR_ = nullptr;
};

inline void checkResult(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        throw std::runtime_error(std::string("OpenXR call failed: ") + what);
    }
}

// Creates the XrInstance with the D3D12 extension enabled, resolves the
// HMD system, and queries the adapter LUID / minimum feature level that
// the active runtime requires us to use.
inline Bootstrap initialize() {
    Bootstrap boot;

    const char* requiredExtensions[] = {
        XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
    };

    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = requiredExtensions;
    std::strncpy(instanceInfo.applicationInfo.applicationName, "Dusklight VR",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    // XR_CURRENT_API_VERSION (1.1.x in the vendored headers) is rejected
    // with XR_ERROR_API_VERSION_UNSUPPORTED by both SteamVR's and Virtual
    // Desktop's OpenXR runtimes (confirmed 2026-07-30) -- neither has
    // caught up past the 1.0.x instance API. Request 1.0 explicitly; this
    // bootstrap only uses core 1.0 functionality plus the D3D12 KHR
    // extension, so there's no feature reason to ask for 1.1.
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    checkResult(xrCreateInstance(&instanceInfo, &boot.instance), "xrCreateInstance");

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    checkResult(xrGetSystem(boot.instance, &systemInfo, &boot.systemId), "xrGetSystem");

    checkResult(
        xrGetInstanceProcAddr(boot.instance, "xrGetD3D12GraphicsRequirementsKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(
                                   &boot.xrGetD3D12GraphicsRequirementsKHR_)),
        "xrGetInstanceProcAddr(xrGetD3D12GraphicsRequirementsKHR)");

    checkResult(
        boot.xrGetD3D12GraphicsRequirementsKHR_(boot.instance, boot.systemId,
                                                  &boot.d3d12Requirements),
        "xrGetD3D12GraphicsRequirementsKHR");

    // boot.d3d12Requirements.adapterLuid - exactly which GPU the active
    //   runtime wants you rendering on (matters on multi-GPU laptops).
    // boot.d3d12Requirements.minFeatureLevel - minimum D3D_FEATURE_LEVEL
    //   your device must support.
    return boot;
}

// --- XR-side D3D12 device + session creation (outcome 2: separate device
// from Aurora's Dawn device, shared into the XR swapchain via
// SharedTextureMemory + fence sync -- see vr_xr_submit.hpp) ---

struct XrGraphicsDevice {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;
};

// Creates a dedicated ID3D12Device + ID3D12CommandQueue on the exact
// adapter the XR runtime requires (boot.d3d12Requirements.adapterLuid),
// independent of whatever adapter Aurora's Dawn device landed on.
inline XrGraphicsDevice createXrGraphicsDevice(const Bootstrap& boot) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    checkResult(
        SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ? XR_SUCCESS
                                                                  : XR_ERROR_RUNTIME_FAILURE,
        "CreateDXGIFactory2");

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    checkResult(
        SUCCEEDED(factory->EnumAdapterByLuid(boot.d3d12Requirements.adapterLuid,
                                              IID_PPV_ARGS(&adapter)))
            ? XR_SUCCESS
            : XR_ERROR_RUNTIME_FAILURE,
        "EnumAdapterByLuid");

    // REVERTED (Meta XR Simulator "solid flashing color" investigation,
    // 2026-08-08): tried enabling the D3D12 debug layer here to catch a
    // possible resource-state mismatch in readbackEyeCopy()'s raw D3D12
    // copy. Instead it made D3D12CreateDevice itself fail on this
    // adapter/interop path (VR fell back to flatscreen entirely) and was
    // followed by an unrelated DXGI_ERROR_DEVICE_RESET crash of Aurora's
    // own Dawn device a few seconds later -- zero validation messages were
    // ever printed, since the debug-layer device never got created. Not a
    // safe diagnostic on this adapter; do not re-add without a different
    // approach (e.g. GPU-based validation off, or a build-time-only debug
    // layer rather than runtime-conditional).
    XrGraphicsDevice gfx;
    checkResult(
        SUCCEEDED(D3D12CreateDevice(adapter.Get(), boot.d3d12Requirements.minFeatureLevel,
                                     IID_PPV_ARGS(&gfx.device)))
            ? XR_SUCCESS
            : XR_ERROR_RUNTIME_FAILURE,
        "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    checkResult(
        SUCCEEDED(gfx.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gfx.commandQueue)))
            ? XR_SUCCESS
            : XR_ERROR_RUNTIME_FAILURE,
        "ID3D12Device::CreateCommandQueue");

    return gfx;
}

// Creates the XrSession bound to the XR-side D3D12 device above, plus the
// LOCAL reference space used for tracking and a VIEW reference space used
// as the head-center reference for per-eye stereo offsets (see
// vr_stereo_render.hpp's eyePoseToViewMtx). dusk::vr::Session
// (vr_xr_submit.hpp) is constructed from the outputs of this call.
//
// ROOT-CAUSED this session (torn/wrong-distance geometry after the
// camera-anchor fix): outViewSpace used to not exist at all -- vr_main.cpp's
// g_viewSpace was a bare global that NOTHING ever assigned, so it stayed
// XR_NULL_HANDLE for the whole session and locateSpace() silently fell back
// to an identity pose {0,0,0} every single frame (see the pre-existing TODO
// comment above its declaration: "Until fixed, hands/head render at
// tracking-space origin"). Once the camera's position started being
// computed as a delta from that fake always-zero "head reference" instead
// of used as an absolute position directly, the per-eye offset became the
// eye's full raw LOCAL-space position (not a true small head-relative
// stereo/IPD offset) -- accumulating however far the player's real head had
// drifted from the tracking origin, independently per moment, producing the
// reported shearing/wrong-distance artifacts. Actually creating and using a
// real, continuously-tracked VIEW space here fixes that at the source.
inline XrSession createXrSession(const Bootstrap& boot, const XrGraphicsDevice& gfx,
                                  XrSpace* outLocalSpace, XrSpace* outViewSpace) {
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = gfx.device.Get();
    binding.queue = gfx.commandQueue.Get();

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = boot.systemId;

    XrSession session = XR_NULL_HANDLE;
    checkResult(xrCreateSession(boot.instance, &sessionInfo, &session), "xrCreateSession");

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
    checkResult(xrCreateReferenceSpace(session, &spaceInfo, outLocalSpace),
                "xrCreateReferenceSpace(LOCAL)");

    XrReferenceSpaceCreateInfo viewSpaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
    checkResult(xrCreateReferenceSpace(session, &viewSpaceInfo, outViewSpace),
                "xrCreateReferenceSpace(VIEW)");

    return session;
}

// ---------------------------------------------------------------------------
// Hand tracking: grip-pose action set
// ---------------------------------------------------------------------------
//
// Prior to this, g_rightGripSpace/g_leftGripSpace (vr_main.cpp) were bare
// XR_NULL_HANDLE globals with nothing to ever assign them -- see the TODO
// that used to sit above their declaration. locateSpace() silently fell
// back to an identity pose for a null space (same fallback g_viewSpace hit
// before it got a real xrCreateReferenceSpace(VIEW) call -- see
// createXrSession's comment), so vr_link::buildHandMtx() has been
// rendering hands at tracking-space origin ever since it was written. This
// is the actual xrCreateActionSet/xrCreateAction/xrCreateActionSpace setup
// that was missing.
//
// One POSE action ("grip_pose") with two subaction paths (/user/hand/left,
// /user/hand/right) rather than two separate actions -- the idiomatic
// OpenXR pattern, and it lets both hands share one binding suggestion per
// profile below instead of two.
struct HandActions {
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction gripPoseAction = XR_NULL_HANDLE;
    // NEW (rotation-calibration follow-up): the OpenXR "aim" pose is a
    // SEPARATE standard pose from grip -- spec-defined with -Z as "the
    // direction the user would point the controller to indicate a target",
    // computed by the runtime from real controller geometry/calibration, not
    // derived from anything this app assumes. Grip and aim poses are always
    // available simultaneously from the same physical controller at the same
    // instant. This exists specifically to replace the previous rotation
    // correction's reliance on an unverified "the camera was looking where
    // the controller pointed" proxy (see vr_link_visibility.hpp's
    // applyStaticCorrection comment / CLAUDE.md section 12) with a
    // self-consistent, runtime-provided reference that needs no assumption
    // about the player's gaze at all.
    XrAction aimPoseAction = XR_NULL_HANDLE;

    // NEW (gameplay controller input, 2026-08-03): real button/axis actions
    // driving actual game input (movement, attack, items), as opposed to
    // the two pose actions above which only ever fed hand-tracking visuals.
    // Same one-action-two-subaction-paths idiom as gripPoseAction/
    // aimPoseAction -- see createHandActionSet()'s bindings for exactly
    // which physical input each subaction path is bound to.
    XrAction triggerValueAction = XR_NULL_HANDLE;   // FLOAT: index trigger, both hands
    XrAction squeezeValueAction = XR_NULL_HANDLE;   // FLOAT: grip squeeze, both hands
    XrAction thumbstickAction = XR_NULL_HANDLE;     // VECTOR2F: thumbstick, both hands
    XrAction primaryClickAction = XR_NULL_HANDLE;   // BOOL: A (right) / X (left)
    XrAction secondaryClickAction = XR_NULL_HANDLE; // BOOL: B (right) / Y (left)
    XrAction menuClickAction = XR_NULL_HANDLE;      // BOOL: menu button, left only
    // NEW (2026-08-04, per explicit user request "make the right stick
    // click the pause menu"): right thumbstick click, OR'd into PAD_BUTTON_START
    // alongside the pre-existing left menu button below -- both trigger
    // pause, neither was removed. One action, both subaction paths bound
    // (see createHandActionSet()'s touch-profile bindings below) -- right
    // click -> pause, left click -> D-pad right (added same day, see
    // vr_main.cpp's tick()).
    XrAction stickClickAction = XR_NULL_HANDLE;     // BOOL: thumbstick click, both hands

    XrPath leftHandPath = XR_NULL_PATH;
    XrPath rightHandPath = XR_NULL_PATH;
};

// Suggests bindings for every controller profile actually relevant to this
// project (see Build workflow notes on which runtimes are tested):
// khr/simple_controller is the universal fallback every conformant OpenXR
// runtime must accept remapping through, and the other three are the
// native profiles for the controllers actually in use (Touch for Meta
// Link; Index/Vive covering the common SteamVR/Virtual Desktop hardware).
// A profile that isn't present on the active runtime just silently fails
// its own xrSuggestInteractionProfileBindings call -- not fatal, so this
// doesn't use checkResult() the way the rest of this file does.
inline HandActions createHandActionSet(XrInstance instance) {
    HandActions actions;

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "dusklight_hands", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "Dusklight Hands",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    setInfo.priority = 0;
    checkResult(xrCreateActionSet(instance, &setInfo, &actions.actionSet), "xrCreateActionSet");

    checkResult(xrStringToPath(instance, "/user/hand/left", &actions.leftHandPath),
                "xrStringToPath(/user/hand/left)");
    checkResult(xrStringToPath(instance, "/user/hand/right", &actions.rightHandPath),
                "xrStringToPath(/user/hand/right)");
    XrPath subactionPaths[] = {actions.leftHandPath, actions.rightHandPath};

    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(actionInfo.actionName, "grip_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(actionInfo.localizedActionName, "Grip Pose",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = subactionPaths;
    checkResult(xrCreateAction(actions.actionSet, &actionInfo, &actions.gripPoseAction),
                "xrCreateAction(grip_pose)");

    // NEW (rotation-calibration follow-up, see HandActions::aimPoseAction's
    // comment): identical setup to grip_pose above, just a different
    // standard pose action/binding path.
    XrActionCreateInfo aimActionInfo{XR_TYPE_ACTION_CREATE_INFO};
    aimActionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(aimActionInfo.actionName, "aim_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aimActionInfo.localizedActionName, "Aim Pose",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    aimActionInfo.countSubactionPaths = 2;
    aimActionInfo.subactionPaths = subactionPaths;
    checkResult(xrCreateAction(actions.actionSet, &aimActionInfo, &actions.aimPoseAction),
                "xrCreateAction(aim_pose)");

    // NEW (gameplay controller input): trigger/squeeze/thumbstick/click
    // actions, same two-subaction-path idiom as the pose actions above.
    auto createAction = [&](XrActionType type, const char* name, const char* localizedName,
                             XrAction* out) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = 2;
        info.subactionPaths = subactionPaths;
        checkResult(xrCreateAction(actions.actionSet, &info, out), name);
    };
    createAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_value", "Trigger", &actions.triggerValueAction);
    createAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_value", "Squeeze", &actions.squeezeValueAction);
    createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", &actions.thumbstickAction);
    createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "primary_click", "Primary Button", &actions.primaryClickAction);
    createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary_click", "Secondary Button", &actions.secondaryClickAction);
    createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_click", "Menu", &actions.menuClickAction);
    createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick Click", &actions.stickClickAction);

    XrPath leftBindingPath = XR_NULL_PATH;
    XrPath rightBindingPath = XR_NULL_PATH;
    XrPath leftAimBindingPath = XR_NULL_PATH;
    XrPath rightAimBindingPath = XR_NULL_PATH;
    xrStringToPath(instance, "/user/hand/left/input/grip/pose", &leftBindingPath);
    xrStringToPath(instance, "/user/hand/right/input/grip/pose", &rightBindingPath);
    xrStringToPath(instance, "/user/hand/left/input/aim/pose", &leftAimBindingPath);
    xrStringToPath(instance, "/user/hand/right/input/aim/pose", &rightAimBindingPath);
    XrActionSuggestedBinding bindings[] = {
        {actions.gripPoseAction, leftBindingPath},
        {actions.gripPoseAction, rightBindingPath},
        {actions.aimPoseAction, leftAimBindingPath},
        {actions.aimPoseAction, rightAimBindingPath},
    };

    auto suggestForProfile = [&](const char* profilePath, const XrActionSuggestedBinding* bindingsArr,
                                  uint32_t count) -> XrResult {
        XrPath profile = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance, profilePath, &profile))) return XR_ERROR_PATH_INVALID;

        XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggest.interactionProfile = profile;
        suggest.countSuggestedBindings = count;
        suggest.suggestedBindings = bindingsArr;
        return xrSuggestInteractionProfileBindings(instance, &suggest);
    };

    // Pose-only bindings for every profile this project has ever tested
    // against (see Build workflow notes) -- unchanged from before this
    // session, still needed for hand-tracking visuals on all of them.
    suggestForProfile("/interaction_profiles/khr/simple_controller", bindings, 4);
    suggestForProfile("/interaction_profiles/htc/vive_controller", bindings, 4);
    suggestForProfile("/interaction_profiles/valve/index_controller", bindings, 4);

    // Gameplay button/axis bindings, scoped to oculus/touch_controller only
    // (Quest 3's native profile, and what SteamVR/Virtual Desktop/Meta Link
    // all report for Touch controllers regardless of runtime) per explicit
    // user request ("set up the quest 3 controllers"). NOT extended to the
    // other 3 profiles above -- their button/axis layouts genuinely differ
    // (vive has a trackpad not a thumbstick, khr/simple has neither trigger
    // nor thumbstick at all) and would need their own verified binding
    // paths, not attempted here since no non-Quest hardware is in scope for
    // this pass. xrSuggestInteractionProfileBindings REPLACES all bindings
    // for a profile on each call, so the pose bindings must be repeated
    // here too, not just the new ones, or hand-tracking would silently stop
    // working on Quest specifically.
    XrPath leftTriggerPath = XR_NULL_PATH, rightTriggerPath = XR_NULL_PATH;
    XrPath leftSqueezePath = XR_NULL_PATH, rightSqueezePath = XR_NULL_PATH;
    XrPath leftStickPath = XR_NULL_PATH, rightStickPath = XR_NULL_PATH;
    XrPath leftXClickPath = XR_NULL_PATH, rightAClickPath = XR_NULL_PATH;
    XrPath leftYClickPath = XR_NULL_PATH, rightBClickPath = XR_NULL_PATH;
    XrPath leftMenuClickPath = XR_NULL_PATH;
    XrPath rightStickClickPath = XR_NULL_PATH;
    XrPath leftStickClickPath = XR_NULL_PATH;
    xrStringToPath(instance, "/user/hand/left/input/trigger/value", &leftTriggerPath);
    xrStringToPath(instance, "/user/hand/right/input/trigger/value", &rightTriggerPath);
    xrStringToPath(instance, "/user/hand/left/input/squeeze/value", &leftSqueezePath);
    xrStringToPath(instance, "/user/hand/right/input/squeeze/value", &rightSqueezePath);
    xrStringToPath(instance, "/user/hand/left/input/thumbstick", &leftStickPath);
    xrStringToPath(instance, "/user/hand/right/input/thumbstick", &rightStickPath);
    xrStringToPath(instance, "/user/hand/left/input/x/click", &leftXClickPath);
    xrStringToPath(instance, "/user/hand/right/input/a/click", &rightAClickPath);
    xrStringToPath(instance, "/user/hand/left/input/y/click", &leftYClickPath);
    xrStringToPath(instance, "/user/hand/right/input/b/click", &rightBClickPath);
    xrStringToPath(instance, "/user/hand/left/input/menu/click", &leftMenuClickPath);
    xrStringToPath(instance, "/user/hand/right/input/thumbstick/click", &rightStickClickPath);
    xrStringToPath(instance, "/user/hand/left/input/thumbstick/click", &leftStickClickPath);

    XrActionSuggestedBinding touchBindings[] = {
        {actions.gripPoseAction, leftBindingPath},
        {actions.gripPoseAction, rightBindingPath},
        {actions.aimPoseAction, leftAimBindingPath},
        {actions.aimPoseAction, rightAimBindingPath},
        {actions.triggerValueAction, leftTriggerPath},
        {actions.triggerValueAction, rightTriggerPath},
        {actions.squeezeValueAction, leftSqueezePath},
        {actions.squeezeValueAction, rightSqueezePath},
        {actions.thumbstickAction, leftStickPath},
        {actions.thumbstickAction, rightStickPath},
        {actions.primaryClickAction, leftXClickPath},
        {actions.primaryClickAction, rightAClickPath},
        {actions.secondaryClickAction, leftYClickPath},
        {actions.secondaryClickAction, rightBClickPath},
        {actions.menuClickAction, leftMenuClickPath},
        {actions.stickClickAction, rightStickClickPath},
        {actions.stickClickAction, leftStickClickPath},
    };
    suggestForProfile("/interaction_profiles/oculus/touch_controller", touchBindings, 17);

    return actions;
}

// Attaches the action set to the session (must happen exactly once, before
// the first xrSyncActions call -- see tick()'s per-frame sync in
// vr_main.cpp) and creates the per-hand action spaces used to locate grip
// (and, as of the rotation-calibration follow-up, aim) poses each frame the
// same way g_viewSpace already locates the head.
inline void attachAndCreateHandSpaces(XrSession session, const HandActions& actions,
                                       XrSpace* outLeftGripSpace, XrSpace* outRightGripSpace,
                                       XrSpace* outLeftAimSpace, XrSpace* outRightAimSpace) {
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actions.actionSet;
    checkResult(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets");

    const XrPosef identityPose{{0, 0, 0, 1}, {0, 0, 0}};

    XrActionSpaceCreateInfo leftSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    leftSpaceInfo.action = actions.gripPoseAction;
    leftSpaceInfo.subactionPath = actions.leftHandPath;
    leftSpaceInfo.poseInActionSpace = identityPose;
    checkResult(xrCreateActionSpace(session, &leftSpaceInfo, outLeftGripSpace),
                "xrCreateActionSpace(left grip)");

    XrActionSpaceCreateInfo rightSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    rightSpaceInfo.action = actions.gripPoseAction;
    rightSpaceInfo.subactionPath = actions.rightHandPath;
    rightSpaceInfo.poseInActionSpace = identityPose;
    checkResult(xrCreateActionSpace(session, &rightSpaceInfo, outRightGripSpace),
                "xrCreateActionSpace(right grip)");

    XrActionSpaceCreateInfo leftAimSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    leftAimSpaceInfo.action = actions.aimPoseAction;
    leftAimSpaceInfo.subactionPath = actions.leftHandPath;
    leftAimSpaceInfo.poseInActionSpace = identityPose;
    checkResult(xrCreateActionSpace(session, &leftAimSpaceInfo, outLeftAimSpace),
                "xrCreateActionSpace(left aim)");

    XrActionSpaceCreateInfo rightAimSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    rightAimSpaceInfo.action = actions.aimPoseAction;
    rightAimSpaceInfo.subactionPath = actions.rightHandPath;
    rightAimSpaceInfo.poseInActionSpace = identityPose;
    checkResult(xrCreateActionSpace(session, &rightAimSpaceInfo, outRightAimSpace),
                "xrCreateActionSpace(right aim)");
}

}  // namespace vr_xr

// --- Next wiring step (not yet done anywhere) ---
//
// Some call site needs to string these together and construct
// dusk::vr::Session, e.g.:
//
//   vr_xr::Bootstrap boot = vr_xr::initialize();
//   vr_xr::XrGraphicsDevice gfx = vr_xr::createXrGraphicsDevice(boot);
//   XrSpace localSpace = XR_NULL_HANDLE;
//   XrSession session = vr_xr::createXrSession(boot, gfx, &localSpace);
//   dusk::vr::Session vrSession(boot.instance, boot.systemId, session, localSpace);
//   dusk::vr::initSession(&vrSession);
//
// Where this call site lives (a new function here vs. inline in
// m_Do_main.cpp near dusk::vr::initSession()) is still an open question --
// not decided yet.
//
// NOT independently verified against your actual openxr_platform.h:
// XR_TYPE_GRAPHICS_BINDING_D3D12_KHR / XrGraphicsBindingD3D12KHR field names
// (binding.device / binding.queue) and EnumAdapterByLuid's exact signature
// on IDXGIFactory4. These are standard KHR_D3D12_enable / DXGI symbols, but
// if the build fails on this file, paste the compiler error rather than
// re-guessing.
