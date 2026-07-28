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
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

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
