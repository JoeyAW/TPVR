// vr_xr_bootstrap.hpp
//
// Minimal OpenXR instance/system/graphics-requirements bootstrap for the
// D3D12 path. This part is safe to write regardless of how we end up
// getting frames into the XR swapchain - it just gets us to the point of
// knowing which GPU adapter and D3D12 feature level the active runtime
// (SteamVR, Meta's PCVR runtime, Virtual Desktop, etc.) requires.
//
// Deliberately NOT included yet: device creation, session creation,
// swapchains. Those depend on resolving how Aurora/Dawn's D3D12 device
// gets created - see the comment block at the bottom before writing that
// part, so it isn't built on a wrong assumption.

#pragma once

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

}  // namespace vr_xr

// --- Open question to resolve before writing device/session/swapchain code ---
//
// OpenXR's D3D12 binding does NOT create a device for you - the app creates
// its own ID3D12Device + ID3D12CommandQueue on the adapter LUID above, and
// hands them to OpenXR via XrGraphicsBindingD3D12KHR. That's good news IF
// Aurora's Dawn-based GX backend can be told to use a device we already
// created, rather than letting Dawn create its own.
//
// Two possible outcomes once you check Aurora/Dawn's backend init code
// (look for D3D12Backend.h / AdapterDiscoveryOptions in Dawn's source, and
// wherever Aurora's CMake wires up its "extern/dawn" submodule):
//
//   1. Dawn supports adopting an externally-created ID3D12Device.
//      -> Create the device yourself first, on the required LUID, and hand
//         it to both Dawn's init and to XrGraphicsBindingD3D12KHR.
//         Everything lives on one device - render Aurora's offscreen
//         target directly into the OpenXR swapchain's ID3D12Resource,
//         no copies needed.
//
//   2. Dawn only supports picking an adapter, and creates its own device.
//      -> You'll end up with two ID3D12Device instances on the same
//         physical GPU. Render to Aurora's offscreen target as normal,
//         then share it into the OpenXR swapchain image each frame via a
//         D3D12 shared resource (D3D12_HEAP_FLAG_SHARED +
//         CreateSharedHandle / OpenSharedHandle) with a shared fence for
//         cross-device sync. More plumbing, but well-trodden - it's the
//         same pattern browser compositors use for cross-device GPU
//         texture sharing.
//
// Worth spending 30 minutes confirming which one you've got before writing
// the session/swapchain code - it changes the shape of that code.
