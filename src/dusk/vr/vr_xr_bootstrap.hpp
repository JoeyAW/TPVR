// vr_xr_bootstrap.hpp
//
// Minimal OpenXR instance/system/graphics-requirements bootstrap, plus
// XR-side graphics device + session creation. Two graphics backends live
// side by side here, selected by DUSK_VR_XR_GRAPHICS_VULKAN: D3D12 (PC,
// SteamVR/Virtual Desktop/Meta Link) and Vulkan (Android/Quest). Function
// and type names are identical across both branches so vr_main.cpp doesn't
// need to know which one it's linked against.
//
// Architecture decision for the D3D12 path (confirmed this session, see
// VR_MOD_HANDOFF_2.md): this is "outcome 2" -- Aurora's Dawn device is
// independent of the XR runtime's required adapter, so we create a
// SEPARATE ID3D12Device here for the XR session, and share textures across
// devices via wgpu::SharedTextureMemory + fence sync (see vr_xr_submit.hpp).
//
// The Vulkan branch below is an UNVERIFIED PROTOTYPE (2026-09-16), written
// against the OpenXR 1.0 spec text for XR_KHR_vulkan_enable2,
// XR_KHR_android_create_instance, and XR_KHR_loader_init_android, and NOT
// compiled -- there's no Android NDK on this machine, so this file has
// never gone through a compiler for the Vulkan branch. Treat struct/field
// names as best-effort from spec knowledge, not confirmed against the
// vendored headers the way the D3D12 path's own bottom comment already
// flags for itself. If/when this actually gets configured against the
// Android toolchain, paste the first compile error rather than have
// anything here re-guessed blind.
//
// Still unwritten after this prototype: vr_xr_submit.hpp's readbackEyeCopy()
// (the D3D12 CopyTextureRegion/upload-heap path) has no Vulkan equivalent
// yet, and vr_stereo_render.hpp still assumes XrSwapchainImageD3D12KHR.
// Neither is touched by this change. This file alone getting a Vulkan
// XrSession doesn't make vr_main.cpp buildable for Android yet.
//
// Also NOT done yet (CMakeLists.txt's Dusklight VR fragment still hard
// `return()`s on `if (NOT WIN32)`, deliberately untouched this pass --
// see the comment there): defining XR_USE_GRAPHICS_API_VULKAN=1 and
// XR_USE_PLATFORM_ANDROID=1 before this header is included (openxr_platform.h
// gates every Vulkan/Android struct used above behind those two macros --
// without them this file won't even declare the types it uses), and
// sourcing an OpenXR loader .so for Android (nothing in this repo fetches
// one today; the existing find_package(OpenXR)/vcpkg path is Windows-only).

#pragma once

#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
#define DUSK_VR_XR_GRAPHICS_VULKAN 1
#else
#define DUSK_VR_XR_GRAPHICS_VULKAN 0
#endif

#if DUSK_VR_XR_GRAPHICS_VULKAN

// Vulkan + Android headers MUST come before openxr_platform.h, same
// ordering requirement as the D3D12 branch below (openxr_platform.h uses
// VkInstance/VkPhysicalDevice/VkDevice etc. without including them itself
// when XR_USE_GRAPHICS_API_VULKAN is defined). jni.h/SDL_system.h are for
// xrInitializeLoaderKHR and XrInstanceCreateInfoAndroidKHR below, which
// both need the JavaVM/Activity the SDL Android shell already owns --
// same SDL_GetAndroidJNIEnv()/SDL_GetAndroidActivity() pattern already
// used in dusk/android_frame_rate.cpp and dusk/http/android.cpp.
// Needed for the AHardwareBuffer GPU-direct swapchain-copy path's Android-
// specific Vulkan extension names/structs (VK_ANDROID_external_memory_
// android_hardware_buffer, declared in <vulkan/vulkan_android.h>, only
// pulled in by vulkan.h/vulkan_core.h when this platform macro is defined
// -- must come before vulkan.h is first included, same ordering
// requirement as XR_USE_PLATFORM_ANDROID below).
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#include <jni.h>
#include <SDL3/SDL_system.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>  // XrGraphicsRequirementsVulkanKHR, XrGraphicsBindingVulkanKHR
#include <cstdint>
#include <cstring>
#include <iterator>  // std::size(requiredExtensions) in initialize() below
#include <stdexcept>
#include <string>
#include <vector>

namespace vr_xr {

struct Bootstrap {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrGraphicsRequirementsVulkanKHR vulkanRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};

    // KHR extension functions aren't statically exported by the loader -
    // they're loaded manually via xrGetInstanceProcAddr below.
    PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanGraphicsRequirements2KHR_ = nullptr;
    PFN_xrCreateVulkanInstanceKHR xrCreateVulkanInstanceKHR_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR xrGetVulkanGraphicsDevice2KHR_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR xrCreateVulkanDeviceKHR_ = nullptr;
};

#else

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

#endif  // DUSK_VR_XR_GRAPHICS_VULKAN

inline void checkResult(XrResult result, const char* what) {
    if (XR_FAILED(result)) {
        throw std::runtime_error(std::string("OpenXR call failed: ") + what);
    }
}

#if DUSK_VR_XR_GRAPHICS_VULKAN

// Creates the XrInstance with the Vulkan + Android-create-instance
// extensions enabled, resolves the HMD system, and queries the Vulkan API
// version range the active runtime requires.
//
// Android has no equivalent of desktop's well-known loader search paths
// (registry keys / /usr paths), so xrInitializeLoaderKHR() MUST run before
// xrCreateInstance() or the loader has no way to find the installed
// runtime at all -- this is the Android-specific step the D3D12/desktop
// branch doesn't need. It's resolved via xrGetInstanceProcAddr(XR_NULL_HANDLE,
// ...) since, by definition, no XrInstance exists yet to resolve it against.
inline Bootstrap initialize() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
        throw std::runtime_error("SDL_GetAndroidJNIEnv() returned null");
    }
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity == nullptr) {
        throw std::runtime_error("SDL_GetAndroidActivity() returned null");
    }

    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR_ = nullptr;
    checkResult(
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR_)),
        "xrGetInstanceProcAddr(xrInitializeLoaderKHR)");

    XrLoaderInitInfoAndroidKHR loaderInitInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfo.applicationVM = vm;
    loaderInitInfo.applicationContext = activity;
    checkResult(
        xrInitializeLoaderKHR_(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfo)),
        "xrInitializeLoaderKHR");

    Bootstrap boot;

    const char* requiredExtensions[] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = vm;
    androidInfo.applicationActivity = activity;

    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.next = &androidInfo;
    instanceInfo.enabledExtensionCount =
        static_cast<uint32_t>(std::size(requiredExtensions));
    instanceInfo.enabledExtensionNames = requiredExtensions;
    std::strncpy(instanceInfo.applicationInfo.applicationName, "Dusklight VR",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    // Same reasoning as the D3D12 branch's identical line: request 1.0
    // explicitly rather than XR_CURRENT_API_VERSION. UNVERIFIED for Meta's
    // Android runtime specifically -- the desktop runtimes' 1.0-only
    // ceiling was confirmed in-headset (2026-07-30), this one hasn't been.
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    checkResult(xrCreateInstance(&instanceInfo, &boot.instance), "xrCreateInstance");

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    checkResult(xrGetSystem(boot.instance, &systemInfo, &boot.systemId), "xrGetSystem");

    checkResult(
        xrGetInstanceProcAddr(boot.instance, "xrGetVulkanGraphicsRequirements2KHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(
                                   &boot.xrGetVulkanGraphicsRequirements2KHR_)),
        "xrGetInstanceProcAddr(xrGetVulkanGraphicsRequirements2KHR)");
    checkResult(
        xrGetInstanceProcAddr(boot.instance, "xrCreateVulkanInstanceKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(
                                   &boot.xrCreateVulkanInstanceKHR_)),
        "xrGetInstanceProcAddr(xrCreateVulkanInstanceKHR)");
    checkResult(
        xrGetInstanceProcAddr(boot.instance, "xrGetVulkanGraphicsDevice2KHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(
                                   &boot.xrGetVulkanGraphicsDevice2KHR_)),
        "xrGetInstanceProcAddr(xrGetVulkanGraphicsDevice2KHR)");
    checkResult(
        xrGetInstanceProcAddr(boot.instance, "xrCreateVulkanDeviceKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(
                                   &boot.xrCreateVulkanDeviceKHR_)),
        "xrGetInstanceProcAddr(xrCreateVulkanDeviceKHR)");

    checkResult(
        boot.xrGetVulkanGraphicsRequirements2KHR_(boot.instance, boot.systemId,
                                                    &boot.vulkanRequirements),
        "xrGetVulkanGraphicsRequirements2KHR");

    // boot.vulkanRequirements.minApiVersionSupported/maxApiVersionSupported -
    //   the Vulkan API version range createXrGraphicsDevice() below must
    //   request, same role as d3d12Requirements.minFeatureLevel on desktop.
    return boot;
}

#else

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

#endif  // DUSK_VR_XR_GRAPHICS_VULKAN

#if DUSK_VR_XR_GRAPHICS_VULKAN

// --- XR-side Vulkan instance + device + session creation ---
//
// Whether this should end up being "outcome 2" the same way the D3D12 path
// is (a separate VkInstance/VkDevice from whatever aurora's wgpu/Dawn
// Vulkan backend already opened, sharing textures across devices) or
// whether aurora's device can be reused/imported directly is an OPEN
// QUESTION -- not resolved here, flagged in chat. This function always
// creates a fresh XR-bound VkInstance/VkDevice via xrCreateVulkanInstanceKHR/
// xrCreateVulkanDeviceKHR (mirrors "outcome 2" by construction, since those
// two calls always mint new Vulkan objects), which at minimum unblocks
// getting an XrSession. The cross-device texture-sharing equivalent of
// vr_xr_submit.hpp's SharedTextureMemory + fence sync is unwritten.

struct XrGraphicsDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    uint32_t queueIndex = 0;
    // Fetched via vkGetDeviceQueue at the end of createXrGraphicsDevice()
    // below -- named commandQueue (not `queue`) to match the D3D12 branch's
    // XrGraphicsDevice::commandQueue field, since vr_main.cpp's Session
    // construction call site (`gfx.device, gfx.commandQueue`) is shared,
    // unbranched code.
    VkQueue commandQueue = VK_NULL_HANDLE;
    // True when VK_ANDROID_external_memory_android_hardware_buffer (plus
    // its real dependencies) was actually enabled on `device` below. No
    // longer gates anything as of 2026-09-19 (the AHardwareBuffer-based
    // GPU-direct path was replaced by the opaque-fd one below -- see
    // vr_xr_submit.hpp's ensureSharedImageResources() HISTORY note); kept
    // as information only.
    bool supportsAndroidHardwareBuffer = false;
    // True when VK_KHR_external_memory_fd was actually enabled on `device`
    // -- the XR-session-side precondition for dusk::vr::Session's
    // shared-image GPU-direct swapchain-copy path (vr_xr_submit.hpp's
    // ensureSharedImageResources(): a VkImage with exportable memory that
    // Dawn imports as an opaque fd). The OTHER precondition,
    // aurora::webgpu::g_vulkanSharedImageExportSupported, is Dawn's own
    // adapter-side support -- vr_main.cpp's startup() requires BOTH before
    // enabling the path, same "both sides must independently support it"
    // pattern as the D3D12 GPU-direct path's adaptersMatch+
    // g_sharedTextureMemoryD3D12Supported check.
    bool supportsExternalMemoryFd = false;
    // True when VK_KHR_external_semaphore_fd was actually enabled on
    // `device` -- the XR-session-side precondition for the async
    // semaphore-gated handoff in dusk::vr::Session's shared-image path
    // (vr_xr_submit.hpp's finishSharedImageGpuCopy()). Same "detect, don't
    // assume" pattern as supportsExternalMemoryFd above.
    bool supportsExternalSemaphoreFd = false;
};

// Creates a VkInstance + VkDevice via XR_KHR_vulkan_enable2's
// xrCreateVulkanInstanceKHR/xrCreateVulkanDeviceKHR, which validate and
// mint the exact instance/device the active runtime requires (same role as
// createXrGraphicsDevice()'s D3D12CreateDevice call against
// boot.d3d12Requirements.adapterLuid below) and hand back both the XR
// result and the wrapped VkResult, so a runtime-side rejection is
// distinguishable from an XR-side one.
inline XrGraphicsDevice createXrGraphicsDevice(const Bootstrap& boot) {
    XrGraphicsDevice gfx;

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Dusklight VR";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "Dusklight";
    appInfo.engineVersion = 1;
    // Clamp to what the runtime told us it supports rather than whatever
    // the NDK's Vulkan headers define -- same pattern as the D3D12 path
    // using boot.d3d12Requirements.minFeatureLevel instead of a hardcoded
    // feature level.
    appInfo.apiVersion = boot.vulkanRequirements.minApiVersionSupported;

    VkInstanceCreateInfo vkInstanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vkInstanceInfo.pApplicationInfo = &appInfo;

    XrVulkanInstanceCreateInfoKHR xrInstanceCreateInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceCreateInfo.systemId = boot.systemId;
    xrInstanceCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInstanceCreateInfo.vulkanCreateInfo = &vkInstanceInfo;
    xrInstanceCreateInfo.vulkanAllocator = nullptr;

    VkResult vkInstanceResult = VK_SUCCESS;
    checkResult(boot.xrCreateVulkanInstanceKHR_(boot.instance, &xrInstanceCreateInfo,
                                                 &gfx.instance, &vkInstanceResult),
                "xrCreateVulkanInstanceKHR");
    if (vkInstanceResult != VK_SUCCESS) {
        throw std::runtime_error(
            "xrCreateVulkanInstanceKHR: underlying vkCreateInstance failed");
    }

    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = boot.systemId;
    deviceGetInfo.vulkanInstance = gfx.instance;
    checkResult(boot.xrGetVulkanGraphicsDevice2KHR_(boot.instance, &deviceGetInfo,
                                                      &gfx.physicalDevice),
                "xrGetVulkanGraphicsDevice2KHR");

    // Find a graphics-capable queue family -- readbackEyeCopy()'s Vulkan
    // equivalent (unwritten, see this section's top comment) will need a
    // command buffer submitted on it.
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gfx.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gfx.physicalDevice, &queueFamilyCount,
                                              queueFamilies.data());
    gfx.queueFamilyIndex = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            gfx.queueFamilyIndex = i;
            break;
        }
    }
    if (gfx.queueFamilyIndex == UINT32_MAX) {
        throw std::runtime_error("No Vulkan queue family with VK_QUEUE_GRAPHICS_BIT found");
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCreateInfo.queueFamilyIndex = gfx.queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    // AHardwareBuffer GPU-direct swapchain-copy path (dusk::vr::Session,
    // vr_xr_submit.hpp): enumerate the physical device's real supported
    // extension list and only enable VK_ANDROID_external_memory_
    // android_hardware_buffer (+ its real dependencies) if actually
    // present -- same "detect, don't assume" discipline as this project's
    // D3D12 GPU-direct path (aurora::webgpu::g_sharedTextureMemoryD3D12Supported).
    // gfx.supportsAndroidHardwareBuffer records whether this actually
    // succeeded; vr_main.cpp's startup() ALSO requires Dawn's own adapter
    // to report aurora::webgpu::g_vulkanSharedImageExportSupported
    // before enabling the path -- both sides independently need it, same
    // shape as the D3D12 path's adaptersMatch+g_sharedTextureMemoryD3D12Supported
    // pair.
    uint32_t availableExtCount = 0;
    vkEnumerateDeviceExtensionProperties(gfx.physicalDevice, nullptr, &availableExtCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(availableExtCount);
    vkEnumerateDeviceExtensionProperties(gfx.physicalDevice, nullptr, &availableExtCount,
                                          availableExts.data());
    auto hasDeviceExtension = [&availableExts](const char* name) {
        for (const auto& ext : availableExts) {
            if (std::strcmp(ext.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    std::vector<const char*> enabledDeviceExtensions;
    // VK_KHR_sampler_ycbcr_conversion, VK_KHR_external_memory, and
    // VK_KHR_dedicated_allocation are real spec-listed dependencies of the
    // AHardwareBuffer extension on Vulkan 1.0 -- almost certainly already
    // promoted to core on Quest 3's actual API version
    // (boot.vulkanRequirements.minApiVersionSupported, requested as
    // appInfo.apiVersion above), so hasDeviceExtension() is expected to
    // return false for them (a core-promoted feature isn't re-listed as
    // its own extension) -- only added to enabledDeviceExtensions when the
    // driver genuinely still exposes them as separate extension strings,
    // never assumed unconditionally.
    const char* const kAhbDependencies[] = {
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };
    for (const char* name : kAhbDependencies) {
        if (hasDeviceExtension(name)) {
            enabledDeviceExtensions.push_back(name);
        }
    }
    if (hasDeviceExtension(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        enabledDeviceExtensions.push_back(
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        gfx.supportsAndroidHardwareBuffer = true;
    }
    // Opaque-fd memory export (2026-09-19, the shared-image GPU-direct path
    // -- see XrGraphicsDevice::supportsExternalMemoryFd). Its base
    // VK_KHR_external_memory is already in kAhbDependencies above.
    if (hasDeviceExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        gfx.supportsExternalMemoryFd = true;
    }

    // Async semaphore-gated handoff (2026-09-18, replaces the AHB path's
    // original CPU-blocking OnSubmittedWorkDone() poll -- see
    // dusk::vr::Session::finishSharedImageGpuCopy()'s own comment): needs
    // VK_KHR_external_semaphore_fd (+ its VK_KHR_external_semaphore base,
    // almost certainly core-promoted on Quest 3's API version, same
    // "don't assume" reasoning as kAhbDependencies above) so the XR-side
    // device can import a VkSemaphore from the opaque FD Dawn exports via
    // wgpu::SharedFenceVkSemaphoreOpaqueFDExportInfo.
    if (hasDeviceExtension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)) {
        enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
    }
    if (hasDeviceExtension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
        enabledDeviceExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        gfx.supportsExternalSemaphoreFd = true;
    }

    VkDeviceCreateInfo vkDeviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    vkDeviceInfo.queueCreateInfoCount = 1;
    vkDeviceInfo.pQueueCreateInfos = &queueCreateInfo;
    vkDeviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
    vkDeviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();

    XrVulkanDeviceCreateInfoKHR xrDeviceCreateInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceCreateInfo.systemId = boot.systemId;
    xrDeviceCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrDeviceCreateInfo.vulkanPhysicalDevice = gfx.physicalDevice;
    xrDeviceCreateInfo.vulkanCreateInfo = &vkDeviceInfo;
    xrDeviceCreateInfo.vulkanAllocator = nullptr;

    VkResult vkDeviceResult = VK_SUCCESS;
    checkResult(boot.xrCreateVulkanDeviceKHR_(boot.instance, &xrDeviceCreateInfo, &gfx.device,
                                               &vkDeviceResult),
                "xrCreateVulkanDeviceKHR");
    if (vkDeviceResult != VK_SUCCESS) {
        throw std::runtime_error("xrCreateVulkanDeviceKHR: underlying vkCreateDevice failed");
    }

    gfx.queueIndex = 0;
    vkGetDeviceQueue(gfx.device, gfx.queueFamilyIndex, gfx.queueIndex, &gfx.commandQueue);
    return gfx;
}

#else

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

#endif  // DUSK_VR_XR_GRAPHICS_VULKAN

// Creates the XrSession bound to the XR-side graphics device above (D3D12
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
#if DUSK_VR_XR_GRAPHICS_VULKAN
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = gfx.instance;
    binding.physicalDevice = gfx.physicalDevice;
    binding.device = gfx.device;
    binding.queueFamilyIndex = gfx.queueFamilyIndex;
    binding.queueIndex = gfx.queueIndex;
#else
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = gfx.device.Get();
    binding.queue = gfx.commandQueue.Get();
#endif

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
