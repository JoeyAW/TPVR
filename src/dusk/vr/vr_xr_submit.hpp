#pragma once

// vr_xr_submit.hpp
// XR session/swapchain management + graphics-API interop for submitting
// rendered eye textures to the OpenXR runtime. Two backends, selected by
// vr_xr_bootstrap.hpp's DUSK_VR_XR_GRAPHICS_VULKAN: D3D12 (PC) and Vulkan
// (Android/Quest, UNVERIFIED PROTOTYPE, 2026-09-16 -- see this comment's
// end).
//
// FENCE SYNC (D3D12 branch only): the fence-sync types are the base
// WebGPU-spec wgpu::SharedFence family, confirmed present in
// dawn/webgpu_cpp.h -- NOT dawn::native::d3d12::SharedFence* (that does not
// exist in this Dawn build; D3D12Backend.h has zero fence-related exports).
// Actual fence *creation* is plain D3D12 (ID3D12Device::CreateFence +
// CreateSharedHandle), done on the XR-side device from
// vr_xr_bootstrap.hpp's createXrGraphicsDevice(). Dawn only *imports* the
// resulting HANDLE via device.ImportSharedFence(...). After EndAccess, the
// XR-side queue is signaled to the value Dawn reports signaling, so the
// NEXT frame's BeginAccess wait is against real completed work rather than
// a stale/zero value. See VR_MOD_HANDOFF_4.md for the full investigation
// trail.
//
// VULKAN BRANCH SCOPE (2026-09-16, unverified/uncompiled -- no Android NDK
// on the machine that wrote it): ensureFenceSync()/importSwapchainImage()/
// probeSwapchainImageShareable() are the abandoned cross-device
// shared-texture-memory approach -- confirmed dead code even on the D3D12
// branch (grepped vr_main.cpp: none of the three are called; the CPU
// round-trip copy path below, encodeEyeCopy()/readbackEyeCopy(), is what's
// actually wired up and confirmed working in-headset on PC). The Vulkan
// branch only ports the CPU round-trip path and omits those three
// entirely, rather than inventing a new Vulkan shared-memory scheme nobody
// asked for and nothing here has tested. getD3D12DeviceAndQueue() and
// adapterMatchesXrRequirement() (bottom of file) are similarly D3D12/DXGI-
// specific and unused outside this file -- also D3D12-only below.

#include "../../../extern/aurora/lib/webgpu/gpu_prof.hpp"  // aurora::webgpu::gpu_prof::Zone (GPU timing of the hand-off)
#include <openxr/openxr.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "dusk/vr/vr_xr_bootstrap.hpp"  // DUSK_VR_XR_GRAPHICS_VULKAN, vr_xr::XrGraphicsDevice
#include "dusk/vr/vr_debug_log.hpp"     // dusk::vr::duskVrLog/duskVrSnprintf

#if DUSK_VR_XR_GRAPHICS_VULKAN
// Already pulled in transitively via vr_xr_bootstrap.hpp above (it needs
// vulkan.h before openxr_platform.h internally), but re-included here
// explicitly -- same as the D3D12 branch's own re-inclusion of d3d12.h
// below -- since this file uses raw Vulkan types/calls directly (VkBuffer,
// vkCreateBuffer, command pools, ...), not just the XR-side structs
// bootstrap.hpp itself needs.
#include <vulkan/vulkan.h>
#include <android/log.h>
// dup()/close() -- Dawn's exported fence fd stays Dawn-owned, Vulkan's
// import takes ownership of what it's given; a dup is the only way to hand
// Vulkan its own copy (see finishSharedImageGpuCopy()).
#include <unistd.h>
// g_queue/g_device/g_vulkanSharedImageExportSupported -- same
// role this header plays for the D3D12 branch below (g_sharedFenceDxgiSupported/
// g_sharedTextureMemoryD3D12Supported), just the Android/Vulkan feature-flag
// counterpart. Same relative path as the D3D12 branch's own include.
#include "../../../extern/aurora/lib/webgpu/gpu.hpp"
#else
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <dawn/native/D3D12Backend.h>

// For aurora::webgpu::g_sharedFenceDxgiSupported -- ensureFenceSync() below
// must check this before calling ImportSharedFence(), since Aurora's
// uncaptured-error callback treats a post-init device error (including
// requesting/using an unsupported feature) as fatal, not a recoverable one.
// Path is relative from src/dusk/vr/vr_xr_submit.hpp to
// extern/aurora/lib/webgpu/gpu.hpp (both confirmed paths, not guessed).
// D3D12-only: only ensureFenceSync() (dead code, see this file's top
// comment) reads this flag.
#include "../../../extern/aurora/lib/webgpu/gpu.hpp"
#endif

namespace dusk::vr {

// duskVrLog/duskVrSnprintf (portable stand-ins for OutputDebugStringA/
// _snprintf_s(buf, _TRUNCATE, fmt, ...), used throughout this file's
// diagnostic logging) now live in vr_debug_log.hpp, included above --
// pulled out into their own header in the same pass that ported
// vr_link_visibility.hpp/vr_menu_gamepad.hpp's logging, both of which
// vr_main.cpp includes alongside this file; leaving a second definition
// here would redefine the same dusk::vr:: symbols in that translation
// unit.

// Maps Aurora's wgpu::TextureFormat (from aurora::gfx::color_format(),
// gfx.hpp:54) to the DXGI_FORMAT that Session::createSwapchain() needs for
// XrSwapchainCreateInfo::format. OpenXR/D3D12 wants the raw DXGI enum value,
// not a WebGPU one, so this can't be a reinterpret_cast -- the two enums
// don't share numeric values.
//
// Only covers the formats Aurora could plausibly report for a swapchain
// color target. Throws on anything else rather than silently guessing a
// fallback -- a wrong-but-compiling format here would manifest as a
// corrupted/black headset image that's hard to trace back to this call,
// so fail loud at startup() instead.
//
// Kept named "Dxgi" on the Vulkan branch too (it returns a VkFormat, not a
// DXGI one there) purely so vr_main.cpp's existing call site
// (toDxgiSwapchainFormat(aurora::gfx::color_format())) doesn't need to
// change -- same "identical names across branches" convention
// vr_xr_bootstrap.hpp already uses throughout.
#if DUSK_VR_XR_GRAPHICS_VULKAN
inline int64_t toDxgiSwapchainFormat(wgpu::TextureFormat format) {
    switch (format) {
        case wgpu::TextureFormat::RGBA8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case wgpu::TextureFormat::RGBA8UnormSrgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case wgpu::TextureFormat::BGRA8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case wgpu::TextureFormat::BGRA8UnormSrgb:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case wgpu::TextureFormat::RGBA16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            throw std::runtime_error(
                "toDxgiSwapchainFormat: unhandled wgpu::TextureFormat from "
                "aurora::gfx::color_format() -- add a case rather than assume "
                "RGBA8Unorm (see VR_MOD_HANDOFF_7.md TODO list).");
    }
}
#else
inline int64_t toDxgiSwapchainFormat(wgpu::TextureFormat format) {
    switch (format) {
        case wgpu::TextureFormat::RGBA8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case wgpu::TextureFormat::RGBA8UnormSrgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case wgpu::TextureFormat::BGRA8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case wgpu::TextureFormat::BGRA8UnormSrgb:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case wgpu::TextureFormat::RGBA16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            throw std::runtime_error(
                "toDxgiSwapchainFormat: unhandled wgpu::TextureFormat from "
                "aurora::gfx::color_format() -- add a case rather than assume "
                "RGBA8Unorm (see VR_MOD_HANDOFF_7.md TODO list).");
    }
}

// Reverse of toDxgiSwapchainFormat() -- needed by Session::
// ensureSwapchainTexture() (GPU-direct swapchain copy path, see
// sameDeviceAsAurora_'s comment): Dawn's TextureDescriptor::format passed
// to SharedTextureMemory::CreateTexture() must match the REAL format the
// underlying ID3D12Resource was created with (swapchainDxgiFormat_ --
// which may differ from aurora::gfx::color_format() whenever
// createSwapchain() had to fall back to a channel-swapped/sRGB-toggled
// candidate), not aurora's own native color format. Only covers the 8bpc
// RGBA/BGRA UNORM/SRGB candidates createSwapchain() can actually choose
// for the gamma-compute-path formats (the only ones
// usesGpuDirectSwapchainCopy() ever applies to -- see that flag's own
// comment) -- the rare kPackedFallbackFormat (10bpc packed) case never
// reaches this function, since that path stays on the old CPU copy
// unconditionally.
inline wgpu::TextureFormat fromDxgiSwapchainFormat(int64_t dxgiFormat) {
    switch (dxgiFormat) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return wgpu::TextureFormat::RGBA8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return wgpu::TextureFormat::RGBA8UnormSrgb;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return wgpu::TextureFormat::BGRA8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return wgpu::TextureFormat::BGRA8UnormSrgb;
        default:
            throw std::runtime_error(
                "fromDxgiSwapchainFormat: unhandled DXGI format -- "
                "usesGpuDirectSwapchainCopy() should never be true for "
                "whatever format reached here (see its own comment).");
    }
}
#endif

// Root cause of "works in Virtual Desktop, xrCreateSwapchain fails on
// SteamVR" (confirmed 2026-07-30): different OpenXR runtimes support
// different, non-overlapping sets of D3D12 swapchain formats -- Virtual
// Desktop's runtime accepts DXGI_FORMAT_B8G8R8A8_UNORM (87, aurora's native
// color format), SteamVR's compositor rejects it ("Unsupported format: 87")
// entirely -- confirmed via logging its real xrEnumerateSwapchainFormats
// list (29 91 2 10 24 40 55 45 20): SteamVR only offers the sRGB-encoded
// 8bpc variants (91 = B8G8R8A8_UNORM_SRGB, 29 = R8G8B8A8_UNORM_SRGB), not
// plain UNORM, alongside HDR float/10-bit/depth formats we can't use here.
// Per the OpenXR spec, an app may only request a format returned by
// xrEnumerateSwapchainFormats -- it must not assume its own native format
// is accepted.
//
// Two independent axes a runtime might differ on, and what each requires
// from the CPU-copy path below:
//  - channel order (B8G8R8A8 vs R8G8B8A8) -- genuinely different byte
//    layout, requires an actual R/B swap per pixel before upload.
//  - sRGB-ness (UNORM vs UNORM_SRGB) -- a gamma-interpretation flag that
//    only affects the raw CopyTextureRegion we do (no shader involved,
//    so the bytes really do pass through unchanged) -- BUT the runtime's
//    own compositor later SAMPLES this swapchain image through its own
//    shaders (lens/distortion correction, reprojection) using an SRV that
//    respects the SRGB tag, decoding our stored bytes as sRGB-encoded
//    before doing its own internal linear-space processing. That's
//    actually the mathematically correct thing to do PROVIDED the stored
//    bytes really are ordinary gamma-encoded color (true here -- aurora's
//    whole rendering chain is confirmed non-linear-workflow, plain 8-bit
//    "display-ready" output) -- per the OpenXR spec's own documented
//    ambiguity (github.com/KhronosGroup/OpenXR-SDK-Source issue #467),
//    different runtimes handle this differently: SteamVR apparently
//    doesn't do a clean decode-then-recompose round trip (needs its own
//    residual per-runtime correction, still not fully root-caused -- see
//    the live "VR Gamma Compensation (SteamVR)" slider), while a
//    spec-faithful runtime (the issue specifically describes this as
//    Meta's behavior) should round-trip correctly with little or no
//    further correction needed.
//
//    REVISED 2026-08-20 (see this file's gamma-exponent comments for the
//    full trail): using the SRGB variant on SteamVR alone, UNCOMPENSATED,
//    does look oversaturated -- confirmed by testing 2026-07-30 -- but
//    that's a statement about needing a per-runtime exponent ON TOP of the
//    SRGB format, not evidence the format choice itself is wrong. A prior
//    version of this comment concluded the opposite ("prefer a format with
//    NO sRGB semantics") from that same data point -- superseded now that
//    real testing across all three runtimes shows Virtual Desktop/Meta
//    Link (submitted via the plain non-SRGB nativeFormat candidate, zero
//    gamma semantics, no compositor-side decode at all) were ALSO reported
//    too bright/washed out -- i.e. the non-SRGB candidates were never
//    actually correct either; they'd just never been paired with a
//    live-adjustable exponent the way SteamVR's SRGB path had, so the
//    error went unnoticed for longer. SRGB submission (paired with the
//    existing per-runtime live gamma-exponent slider, at 1.0/no-op or a
//    small correction, whichever real testing confirms) is now PREFERRED
//    over the plain native format for every runtime that supports it, not
//    just a SteamVR-forced fallback -- see the reordered candidate list
//    below.
// Returns 0 if `format` has no such counterpart (e.g. RGBA16Float, which
// has neither) -- 0 signals "no fallback on this axis" to the caller
// rather than silently guessing.
#if DUSK_VR_XR_GRAPHICS_VULKAN
inline int64_t channelSwapCounterpart(int64_t format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        default:
            return 0;
    }
}

inline int64_t srgbToggleCounterpart(int64_t format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return 0;
    }
}

// Vulkan counterpart of DXGI_FORMAT_R10G10B10A2_UNORM used below as the
// last-resort packed-10bpc swapchain format candidate. Bit-compatible with
// DXGI's layout (confirmed against the Vulkan spec's packed-format naming
// rule -- components are listed MSB-first, so A2B10G10R10 places R in bits
// 0-9, G 10-19, B 20-29, A 30-31, exactly matching DXGI_FORMAT_R10G10B10A2
// -- so packR10G10B10A2Unorm()'s existing bit-packing below is reused
// as-is, no separate Vulkan version needed).
inline constexpr int64_t kPackedFallbackFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
#else
inline int64_t channelSwapCounterpart(int64_t format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default:
            return 0;
    }
}

inline int64_t srgbToggleCounterpart(int64_t format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            return 0;
    }
}

inline constexpr int64_t kPackedFallbackFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
#endif

// Preferred fallback over any SRGB variant (see the big comment above):
// DXGI_FORMAT_R10G10B10A2_UNORM has no automatic gamma-decode/encode
// semantics attached at all (unlike the *_SRGB formats), so there's no
// double-gamma risk -- confirmed present in SteamVR's actual
// xrEnumerateSwapchainFormats list (format 24) alongside the SRGB-only
// 8bpc formats. Costs a real per-pixel repack (8-bit-per-channel source ->
// 10-bit R/G/B + 2-bit A, bit-replicated rather than naively truncated so
// e.g. 8-bit 255 maps to the true 10-bit max 1023, not 1020) instead of
// a plain memcpy or channel swap, but avoids the color-space ambiguity
// entirely. Bit layout per the DXGI spec (R in bits 0-9, G in 10-19, B in
// 20-29, A in 30-31): confirmed against d3d/dxgiformat docs, not guessed.
inline uint32_t packR10G10B10A2Unorm(uint8_t r8, uint8_t g8, uint8_t b8, uint8_t a8) {
    auto to10 = [](uint8_t v8) -> uint32_t {
        return (static_cast<uint32_t>(v8) << 2) | (static_cast<uint32_t>(v8) >> 6);
    };
    const uint32_t r10 = to10(r8);
    const uint32_t g10 = to10(g8);
    const uint32_t b10 = to10(b8);
    const uint32_t a2 = static_cast<uint32_t>(a8) >> 6;
    return r10 | (g10 << 10) | (b10 << 20) | (a2 << 30);
}

// What readbackEyeCopy() must do to the CPU-side bytes before uploading,
// decided once by createSwapchain() based on which candidate format the
// runtime actually supports.
enum class SwapchainPixelConversion {
    None,             // native format accepted as-is, plain memcpy
    ChannelSwap,      // runtime wants the channel-swapped (still 8bpc, non-SRGB) counterpart
    PackR10G10B10A2,  // runtime only offered SRGB 8bpc variants -- use the gamma-neutral 10bpc format instead
};

// EMPIRICAL gamma pre-compensation for the SRGB-swapchain-format case
// (confirmed 2026-07-30: SteamVR only composites projection layers
// successfully with an SRGB-tagged 8bpc format, and using one as-is makes
// the in-headset image visibly oversaturated compared to VD/Meta Link,
// which both accept our plain non-SRGB native format and show it
// untouched). The exact transform SteamVR's closed-source compositor
// applies to SRGB-tagged content isn't something we can inspect directly
// -- this constant is a starting guess, tuned against the user's
// in-headset visual comparison against VD/Meta Link's correct appearance:
// 2.2 (darken) was tried first and looked worse ("evil", overcorrected too
// dark); 1.0/2.2 (brighten) was confirmed to look normal. Adjust and
// rebuild if this ever needs revisiting (e.g. a SteamVR update changes its
// compositor's color handling) -- 1.0 disables compensation entirely.
//
// STILL JUST A PER-RUNTIME BASELINE, NOT THE WHOLE STORY (2026-08-16):
// external tester feedback ("the mod is too bright" -- washed out in the
// headset specifically, desktop mirror looks correct) reproduced on ALL
// THREE runtimes, including VD/Meta Link, which never touch this constant
// at all (they always pick the plain non-SRGB native format -- see
// createSwapchain()'s candidate order -- and previously got a byte-for-byte
// passthrough with zero gamma modification). That rules this constant out
// as the sole cause. Fix: generalized the GPU compute pass (below) to run
// for every runtime, gave VD/Meta Link a real, live-adjustable exponent of
// their own (Session::gammaCompensationMultiplier_/effectiveGammaExponent()),
// exposed via the "VR Gamma Compensation" ImGui slider (ImGuiMenuTools.cpp,
// dusk::getSettings().game.vrGammaCompensation) so it can be dialed in
// per-headset without a rebuild each guess -- CONFIRMED 2.0 looks correct
// on Virtual Desktop. **Deliberately does NOT touch THIS constant or
// SteamVR's exponent at all** -- effectiveGammaExponent() keeps SteamVR on
// its own already-tuned, independently-confirmed baseline, decoupled from
// the new slider, specifically because 2.0 was only ever tested on VD;
// blindly multiplying it onto SteamVR's already-correct ~0.4545 (giving
// ~0.909, nearly cancelling the original fix) would have been pure
// speculation. If SteamVR is ever tested against the new slider and found
// to need adjustment too, that's a deliberate separate step, not a given.
//
// CORRECTED, SAME DAY: SteamVR WAS tested, and this constant's "already-
// correct" status above turned out to be wrong -- user report:
// "steamvr is undersaturated now." In hindsight, this constant was never
// independently verified as objectively correct back in section 6 -- it
// was tuned by visual comparison AGAINST VD/Meta Link's own appearance,
// and this entire 2026-08-16 investigation exists because VD/Meta Link
// were ALSO too bright the whole time. So SteamVR was tuned to match a
// reference that was itself wrong. CONFIRMED FIX: 1.0 (no compensation at
// all) is correct, not this constant's ~0.4545 brightening curve. This
// constant is now ONLY the seed value for `Session::steamVrGammaExponent_`,
// immediately overwritten every real frame from `dusk::getSettings()
// .game.vrGammaCompensationSteamVr` -- kept only as a comment/history
// anchor and as the field's harmless pre-first-frame initializer, not as
// the actual runtime value.
//
// REVERSED AGAIN, 2026-08-20: the "1.0 is correct" conclusion immediately
// above was ITSELF wrong. After the createSwapchain() SRGB-preference
// change (see its own comment) and reading up on the underlying OpenXR
// spec ambiguity (github.com/KhronosGroup/OpenXR-SDK-Source issue #467,
// via an outside modder's tip -- see this file's other 2026-08-20 comments),
// the user reconsidered and confirmed the ORIGINAL ~0.4545 brightening
// curve (this constant's own value) was reading SteamVR's colors correctly
// all along -- the "undersaturated" judgment that led to the 1.0 change
// was a mistake, not a real regression. The actual problem the whole time,
// per this same reconsideration, was that Virtual Desktop/Meta Link were
// too bright (see gammaCompensationMultiplier_'s comment) -- not that
// SteamVR needed correcting away from this constant.
// `dusk::getSettings().game.vrGammaCompensationSteamVr`'s compiled default
// (settings.cpp) briefly matched this constant's value, THEN FLIPPED A
// THIRD TIME the same day: right after the createSwapchain() SRGB-
// preference reorder confirmed Virtual Desktop correct at 1.0/100% with
// zero compensation, explicit follow-up -- "It should be 1.0 or 100%, not
// 45%." The real compiled default (settings.cpp) is 1.0 again, NOT this
// constant's ~0.4545 -- this constant is purely a pre-first-frame seed, as
// its own field comment already says; do not trust it as a proxy for the
// current real default without checking settings.cpp directly. Three
// flips in one project on this exact value is a strong argument for real
// skepticism on any future report about it -- get a direct side-by-side
// against the desktop mirror (known-correct) rather than a memory-based
// impression before changing it a fourth time.
//
// PERFORMANCE (confirmed 2026-07-30): originally applied via a per-pixel
// CPU LUT lookup in readbackEyeCopy() -- this measurably halved SteamVR's
// framerate (a scalar loop over ~9.7M texels/frame, on top of the
// already-CPU-bound blocking readback path documented elsewhere in this
// file). Moved to a GPU compute pass instead (see kGammaComputeShaderSource/
// ensureGammaComputeResources() below) specifically to eliminate that cost
// -- GPUs are built for exactly this kind of massively parallel per-pixel
// work, and this way it runs as part of the existing render pipeline
// rather than adding a new CPU stage. Now created/dispatched whenever
// Session::useGammaComputePath_ is true -- i.e. for every runtime/format
// EXCEPT the rare DXGI_FORMAT_R10G10B10A2_UNORM last-resort fallback (the
// compute shader only packs 8bpc output; that one rare candidate still
// takes the old plain CPU-repack path with no gamma applied).
constexpr float kSteamVrGammaCompensationExponent = 1.0f / 2.2f;

// Uniform buffer layout consumed by kGammaComputeShaderSource's `Params`
// struct -- field order/sizes/padding must match exactly (WGSL host-shareable
// uniform structs round up to 16-byte multiples; this is manually padded to
// 32 bytes to match). swapRB selects output byte order: 0 = native channel
// order (e.g. BGRA, matching SwapchainPixelConversion::None), 1 = swapped
// (matching SwapchainPixelConversion::ChannelSwap) -- lets the same compute
// pass handle gamma correction AND any channel reorder the chosen SRGB
// candidate format also needed, in one GPU pass instead of two.
struct GammaComputeParams {
    uint32_t width;
    uint32_t height;
    uint32_t rowWords;    // res.bytesPerRow / 4 -- destination row stride in u32 texels
    uint32_t swapRB;
    float gammaExponent;
    uint32_t _pad0 = 0;
    uint32_t _pad1 = 0;
    uint32_t _pad2 = 0;
};
static_assert(sizeof(GammaComputeParams) == 32,
              "GammaComputeParams must match kGammaComputeShaderSource's Params uniform layout");

// Reads the source eye texture directly (textureLoad -- no sampler, no
// filtering, exact texel values) and writes gamma-corrected, correctly
// byte-ordered pixels into a packed u32 storage buffer laid out exactly
// like the CPU-readback buffer CopyTextureToBuffer would otherwise
// produce (row stride = rowWords u32s, matching res.bytesPerRow). Alpha is
// passed through unchanged -- only R/G/B get the compensation curve.
inline constexpr const char* kGammaComputeShaderSource = R"WGSL(
struct Params {
    width: u32,
    height: u32,
    rowWords: u32,
    swapRB: u32,
    gammaExponent: f32,
};

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> dst: array<u32>;
@group(0) @binding(2) var<uniform> params: Params;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.width || gid.y >= params.height) {
        return;
    }

    let texel = textureLoad(srcTex, vec2<i32>(i32(gid.x), i32(gid.y)), 0);
    let r = pow(clamp(texel.r, 0.0, 1.0), params.gammaExponent);
    let g = pow(clamp(texel.g, 0.0, 1.0), params.gammaExponent);
    let b = pow(clamp(texel.b, 0.0, 1.0), params.gammaExponent);
    let a = clamp(texel.a, 0.0, 1.0);

    let r8 = u32(r * 255.0 + 0.5);
    let g8 = u32(g * 255.0 + 0.5);
    let b8 = u32(b * 255.0 + 0.5);
    let a8 = u32(a * 255.0 + 0.5);

    var packed: u32;
    if (params.swapRB != 0u) {
        packed = (a8 << 24u) | (b8 << 16u) | (g8 << 8u) | r8;
    } else {
        packed = (a8 << 24u) | (r8 << 16u) | (g8 << 8u) | b8;
    }
    dst[gid.y * params.rowWords + gid.x] = packed;
}
)WGSL";

// Per-frame constants of the space-warp motion-vector pass (see Session's
// SPACE WARP section). Layout matches kSpaceWarpShaderSource's SwParams
// exactly (WGSL uniform rules: mat4x4f = 64 bytes, vec4 = 16).
struct SpaceWarpUniforms {
    // Per eye: P_prev * V_prev * inv(V_cur), column-major (WGSL mat4x4f).
    float prevFromCurView[2][16];
    // Per eye: this frame's projection terms {p0, p1, p2, p3} =
    // {m[0][0], m[0][2], m[1][1], m[1][2]} of eyeFovToProjMtx()'s output.
    float invProj[2][4];
    float nearFar[4];   // {near, far, debugFlags (as float; 1=negate 2=flipY 4=zeroMv 8=flatDepth), 0}
    uint32_t sizes[4];  // {mv per-eye width, mv height, depth per-eye width, depth height}
};
static_assert(sizeof(SpaceWarpUniforms) == 192, "SpaceWarpUniforms must match SwParams' WGSL layout");

class Session {
public:
#if DUSK_VR_XR_GRAPHICS_VULKAN
    // xrGfx is the XR-bound Vulkan instance/physical device/device/queue
    // from vr_xr::createXrGraphicsDevice -- taken as the whole struct
    // (unlike the D3D12 branch's two separate ComPtr args below) because
    // the Vulkan copy path also needs the physical device (memory-type
    // queries in ensureCpuCopyBuffers()) and queue family index (the
    // command pool in ensureCpuCopyCmdList()), not just a device/queue
    // pair. vr_main.cpp's construction call site is branched accordingly.
    Session(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace localSpace,
            const vr_xr::XrGraphicsDevice& xrGfx)
        : instance_(instance),
          systemId_(systemId),
          session_(session),
          localSpace_(localSpace),
          xrPhysicalDevice_(xrGfx.physicalDevice),
          xrDevice_(xrGfx.device),
          xrQueue_(xrGfx.commandQueue),
          xrQueueFamilyIndex_(xrGfx.queueFamilyIndex) {}
#else
    // xrDevice/xrQueue are the XR-side ID3D12Device/ID3D12CommandQueue
    // (from vr_xr::createXrGraphicsDevice) -- needed here to create and
    // signal the shared fence used for cross-device sync with Dawn's device.
    Session(XrInstance instance, XrSystemId systemId, XrSession session, XrSpace localSpace,
            Microsoft::WRL::ComPtr<ID3D12Device> xrDevice,
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> xrQueue)
        : instance_(instance),
          systemId_(systemId),
          session_(session),
          localSpace_(localSpace),
          xrDevice_(std::move(xrDevice)),
          xrQueue_(std::move(xrQueue)) {}
#endif

    XrSession session() const { return session_; }
    XrSpace localSpace() const { return localSpace_; }
    XrTime predictedDisplayTime() const { return frameState_.predictedDisplayTime; }

    // Called once per frame after xrWaitFrame.
    void setFrameState(const XrFrameState& state) { frameState_ = state; }

    std::vector<XrViewConfigurationView> enumerateViewConfigurationViews(
        XrViewConfigurationType viewConfigType) {
        uint32_t count = 0;
        xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfigType, 0, &count, nullptr);

        std::vector<XrViewConfigurationView> views(
            count, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(instance_, systemId_, viewConfigType, count, &count,
                                           views.data());
        return views;
    }

    std::vector<int64_t> enumerateSwapchainFormats() {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
        return formats;
    }

    // `nativeFormat` is aurora's actual render color format, converted to
    // DXGI via toDxgiSwapchainFormat() -- what the CPU-readback path's
    // staging buffer bytes are genuinely laid out as. Per the OpenXR spec
    // we can only request a format the runtime actually returned from
    // xrEnumerateSwapchainFormats -- but confirmed 2026-07-30 that this
    // alone isn't sufficient either: SteamVR's xrEnumerateSwapchainFormats
    // lists DXGI_FORMAT_R10G10B10A2_UNORM (24), xrCreateSwapchain with it
    // succeeds, but actually submitting a projection layer with it fails at
    // runtime with "ComposeLayerProjection: ... VRCompositorError_
    // TextureUsesUnsupportedFormat" -- i.e. SteamVR over-advertises formats
    // its projection-layer compositor path doesn't really accept, so R10G10B10A2
    // is demoted to last resort here despite being gamma-neutral (see its
    // own comment).
    //
    // REORDERED 2026-08-20 to prefer SRGB universally (see the big comment
    // above this function for the full reasoning): SRGB submission is no
    // longer treated as a SteamVR-only fallback -- real testing found
    // Virtual Desktop/Meta Link's previous top choice (plain nativeFormat,
    // zero gamma semantics) was ALSO producing a too-bright/washed-out
    // image, just never diagnosed as such because nothing was correcting
    // for it there the way SteamVR's forced SRGB path already was. New
    // preference order:
    //   (1) nativeFormat's sRGB-toggled counterpart -- same channel order,
    //       correct gamma semantics. What SteamVR was already forced onto
    //       (confirmed working end-to-end 2026-07-30, format 91/
    //       B8G8R8A8_UNORM_SRGB); now the first choice for every runtime
    //       that advertises it, not just SteamVR.
    //   (2) both channel-swapped AND sRGB-toggled -- same gamma correctness,
    //       different channel order.
    //   (3) nativeFormat exactly -- fallback if the runtime doesn't
    //       advertise an SRGB variant at all; no gamma semantics attached
    //       (the pre-2026-08-20 behavior for every runtime that reached
    //       this point, VD/Meta Link included).
    //   (4) its channel-swapped counterpart -- same as (3), different
    //       channel order.
    //   (5) DXGI_FORMAT_R10G10B10A2_UNORM -- LAST RESORT: gamma-neutral in
    //       theory, but confirmed to fail at actual frame submission on
    //       SteamVR despite being enumerated as supported. Kept as a final
    //       fallback in case some OTHER runtime advertises it AND actually
    //       honors it, rather than removing it outright.
    // Picks the first the runtime actually supports and records what
    // readbackEyeCopy() needs to do to the pixel data for it. Fails loudly
    // (returns false) rather than silently guessing a 6th format if none
    // of these match.
    bool createSwapchain(uint32_t width, uint32_t height, int64_t nativeFormat) {
        const std::vector<int64_t> supported = enumerateSwapchainFormats();

        const int64_t channelSwapped = channelSwapCounterpart(nativeFormat);
        const int64_t srgbToggled = srgbToggleCounterpart(nativeFormat);
        const int64_t bothSwapped = channelSwapped != 0 ? srgbToggleCounterpart(channelSwapped) : 0;

        struct Candidate { int64_t format; SwapchainPixelConversion conversion; };
        const Candidate candidates[] = {
            {srgbToggled, SwapchainPixelConversion::None},
            {bothSwapped, SwapchainPixelConversion::ChannelSwap},
            {nativeFormat, SwapchainPixelConversion::None},
            {channelSwapped, SwapchainPixelConversion::ChannelSwap},
            {kPackedFallbackFormat, SwapchainPixelConversion::PackR10G10B10A2},
        };

        int64_t chosenFormat = 0;
        SwapchainPixelConversion conversion = SwapchainPixelConversion::None;
        for (const Candidate& c : candidates) {
            if (c.format != 0 && std::find(supported.begin(), supported.end(), c.format) != supported.end()) {
                chosenFormat = c.format;
                conversion = c.conversion;
                break;
            }
        }

        if (chosenFormat == 0) {
            char msg[512];
            int off = duskVrSnprintf(msg, sizeof(msg),
                                  "[dusk::vr] createSwapchain: none of native format %lld or its "
                                  "sRGB/channel-swap/10bpc variants supported; runtime offers:",
                                  static_cast<long long>(nativeFormat));
            for (size_t i = 0; i < supported.size() && off > 0 && off < 480; ++i) {
                int written = duskVrSnprintf(msg + off, sizeof(msg) - off, " %lld",
                                           static_cast<long long>(supported[i]));
                if (written < 0) break;
                off += written;
            }
            duskVrLog(msg);
            duskVrLog("\n");
            return false;
        }
        if (chosenFormat != nativeFormat) {
            char msg[200];
            duskVrSnprintf(msg, sizeof(msg),
                        "[dusk::vr] createSwapchain: native format %lld not supported by this "
                        "runtime, using %lld instead (conversion=%d)\n",
                        static_cast<long long>(nativeFormat), static_cast<long long>(chosenFormat),
                        static_cast<int>(conversion));
            duskVrLog(msg);
        }

        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        // TRANSFER_DST added 2026-09-19: every submit path this file has
        // (CPU readback upload, D3D12 intermediate copy, Vulkan AHB copy)
        // writes into the swapchain image with a transfer/copy command, and
        // the OpenXR spec requires the app to declare that usage up front --
        // on Vulkan, copying into an image created without
        // VK_IMAGE_USAGE_TRANSFER_DST_BIT is undefined behavior (it only
        // ever worked because runtimes tend to over-provision usage).
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = chosenFormat;
        ci.width = width;
        ci.height = height;
        ci.sampleCount = 1;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;

        if (XR_FAILED(xrCreateSwapchain(session_, &ci, &swapchain_))) {
            return false;
        }

        swapchainDxgiFormat_ = chosenFormat;
        swapchainPixelConversion_ = conversion;
#if DUSK_VR_XR_GRAPHICS_VULKAN
        swapchainIsSrgb_ = chosenFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                           chosenFormat == VK_FORMAT_R8G8B8A8_SRGB;
        // FOUND 2026-09-16 (first real Quest 3 hardware test, everything
        // rendering with a blue tint despite correct geometry/lighting --
        // real bug, not a gamma/brightness issue): kGammaComputeShaderSource's
        // swapRB flag was being derived from SwapchainPixelConversion (None
        // vs ChannelSwap), which only ever encodes "does the chosen format
        // differ from aurora's assumed native format" -- a concept baked in
        // when this file was Windows/D3D12-only, where the native format
        // really is BGRA, so swapRB=0 (the shader's non-swapped case)
        // correctly meant "pack as BGRA". On Android/Vulkan the chosen
        // format is R8G8B8A8 (confirmed via a real device log: native
        // format 37 = VK_FORMAT_R8G8B8A8_UNORM, chosen 43 =
        // VK_FORMAT_R8G8B8A8_SRGB -- SAME channel order, hence
        // conversion=None), so swapRB stayed 0 and the shader kept packing
        // BGRA bytes into a buffer the swapchain expects as RGBA -- R and B
        // swapped on every pixel. Fix: compute the shader's byte-order flag
        // from what the CHOSEN format's real channel layout actually is,
        // not from the None/ChannelSwap distinction (which stays exactly
        // what it was for the CPU-side conversion switch above -- that one
        // is about whether a swap is needed RELATIVE to nativeFormat, a
        // different question this new flag doesn't replace).
        swapchainIsBgra_ = chosenFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                           chosenFormat == VK_FORMAT_B8G8R8A8_SRGB;
#else
        swapchainIsSrgb_ = chosenFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                           chosenFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        // See the Vulkan branch's comment above for why this exists. On the
        // D3D12 path every real candidate this project has ever chosen has
        // been BGRA-family (native format itself, per this whole file's
        // "native format is BGRA" history) -- computed properly anyway,
        // the same way as Vulkan, rather than hardcoding true, in case a
        // future candidate ever picks an RGBA-family DXGI format here.
        swapchainIsBgra_ = chosenFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
                           chosenFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
#endif
        // Generalized 2026-08-16 (see kSteamVrGammaCompensationExponent's
        // comment): the GPU gamma-compensation compute pass now runs for
        // every candidate EXCEPT PackR10G10B10A2 -- the shader only ever
        // packs 8bpc output, so that one rare last-resort format keeps the
        // old plain CPU repack path with no gamma applied at all.
        useGammaComputePath_ = conversion != SwapchainPixelConversion::PackR10G10B10A2;

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchain_, 0, &imageCount, nullptr);
#if DUSK_VR_XR_GRAPHICS_VULKAN
        swapchainImages_.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
#else
        swapchainImages_.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
#endif
        xrEnumerateSwapchainImages(
            swapchain_, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages_.data()));
#if !DUSK_VR_XR_GRAPHICS_VULKAN
        // FOUND 2026-09-17 (first real GPU-direct-path test, Virtual
        // Desktop/AMD RX 5700 XT): xrCreateSwapchain was asked for a TYPED
        // format (chosenFormat, e.g. DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), but
        // the runtime is free to allocate the underlying ID3D12Resource
        // with the TYPELESS counterpart instead (so it can expose both an
        // SRGB and non-SRGB view of the same resource) -- confirmed via a
        // real crash: ensureSwapchainTexture()'s ImportSharedTextureMemory
        // reads the resource's OWN GetDesc().Format directly, found
        // DXGI_FORMAT_B8G8R8A8_TYPELESS (90/0x5a), and Dawn's D3D12
        // SharedTextureMemory backend doesn't accept a typeless resource at
        // all -- fatal ("Unsupported DXGI format 5a"). Detect this HERE,
        // before ever calling ImportSharedTextureMemory, and disable the
        // GPU-direct path for the rest of the session if the real resource
        // format doesn't match what we requested -- usesGpuDirectSwapchainCopy()
        // then correctly falls back to the already-proven CPU-readback path
        // instead of crashing. Checking swapchainImages_[0] alone is
        // sufficient -- every image in one swapchain shares the same format
        // by construction.
        if (!swapchainImages_.empty()) {
            D3D12_RESOURCE_DESC realDesc = swapchainImages_[0].texture->GetDesc();
            if (static_cast<int64_t>(realDesc.Format) != chosenFormat) {
                char msg[256];
                // 2026-09-19: no longer disables the GPU-direct path -- the
                // intermediate-texture variant (ensureIntermediateTexture())
                // handles a typeless runtime resource; this flag now only
                // records that DIRECT import would be impossible.
                duskVrSnprintf(msg, sizeof(msg),
                            "[dusk::vr] createSwapchain: runtime's real swapchain resource "
                            "format (%lld) doesn't match the requested format (%lld) -- "
                            "allocated typeless; direct Dawn import impossible, GPU-direct "
                            "copy will go through the typed intermediate texture instead\n",
                            static_cast<long long>(realDesc.Format), static_cast<long long>(chosenFormat));
                duskVrLog(msg);
                swapchainResourceFormatUsable_ = false;
            }
        }
#endif
        return true;
    }

    XrSwapchain swapchain() const { return swapchain_; }

#if !DUSK_VR_XR_GRAPHICS_VULKAN
    // Lazily creates the D3D12 fence (on the XR-side device) and imports it
    // into Dawn as a wgpu::SharedFence. Called once, on first use, from
    // importSwapchainImage(). Not done at construction time because Dawn's
    // wgpu::Device isn't available until the first frame call site (Session
    // is constructed before Aurora's gfx device exists, per initSession()'s
    // existing TODO in vr_main.cpp).
    void ensureFenceSync(const wgpu::Device& dawnDevice) {
        if (fenceInitialized_) {
            return;
        }

        // NEW: aurora::webgpu::g_sharedFenceDxgiSupported (gpu.cpp) reflects
        // whether the adapter/device actually support DXGI shared-fence
        // import -- checked and requested at device-creation time, since
        // Dawn can't enable it after the fact. This MUST be checked before
        // calling ImportSharedFence() below: Aurora's uncaptured-error
        // callback treats any post-init device error as fatal (process
        // abort), so calling in unsupported doesn't fail gracefully the way
        // the HRESULT checks above do -- it crashes the whole game. Confirmed
        // this is exactly what happened before the feature was requested in
        // gpu.cpp: "FeatureName::SharedFenceDXGISharedHandle is not enabled."
        if (!aurora::webgpu::g_sharedFenceDxgiSupported) {
            return;
        }

        HRESULT hr = xrDevice_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3dFence_));
        if (FAILED(hr)) {
            // TODO: route to real logging once the call site is confirmed --
            // for now, fenceInitialized_ stays false and callers fall back
            // to unsynchronized access (same behavior as before fence sync
            // existed at all).
            return;
        }

        HANDLE fenceHandle = nullptr;
        hr = xrDevice_->CreateSharedHandle(d3dFence_.Get(), nullptr, GENERIC_ALL, nullptr,
                                            &fenceHandle);
        if (FAILED(hr)) {
            return;
        }

        wgpu::SharedFenceDXGISharedHandleDescriptor dxgiDesc{};
        dxgiDesc.handle = fenceHandle;

        wgpu::SharedFenceDescriptor fenceDesc{};
        fenceDesc.nextInChain = &dxgiDesc;

        dawnFence_ = dawnDevice.ImportSharedFence(&fenceDesc);

        // Dawn duplicates the handle internally on import; close our copy
        // per the standard DXGI shared-handle ownership pattern.
        CloseHandle(fenceHandle);

        fenceInitialized_ = true;
    }

    // Wraps swapchain image `index` as an importable wgpu::Texture for this
    // frame. If fence sync was successfully initialized, waits on the
    // fence's last-signaled value before Dawn touches the texture --
    // otherwise falls back to unsynchronized access rather than failing the
    // frame outright.
    // DIAGNOSTIC (temporary, remove once answered): tests whether XR
    // swapchain image resources can actually be shared cross-device at all,
    // before committing to rewriting importSwapchainImage() around
    // SharedTextureMemoryDXGISharedHandle. CreateSharedHandle() only
    // succeeds on a resource allocated with D3D12_HEAP_FLAG_SHARED -- the
    // OpenXR runtime allocates swapchain images, not us, so it's a real
    // open question whether it did that. This calls the exact same D3D12
    // entry point the real import path would need, on a real swapchain
    // image, and just logs the HRESULT -- it doesn't touch Dawn at all, so
    // a failure here can't be confused with a Dawn/feature problem.
    void probeSwapchainImageShareable() {
        static bool probed = false;
        if (probed || swapchainImages_.empty()) {
            return;
        }
        probed = true;

        Microsoft::WRL::ComPtr<ID3D12Resource> resource = swapchainImages_[0].texture;
        HANDLE handle = nullptr;
        HRESULT hr = xrDevice_->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle);

        char msg[256];
        if (SUCCEEDED(hr)) {
            _snprintf_s(msg, _TRUNCATE,
                        "[dusk::vr] PROBE: swapchain image IS shareable "
                        "(CreateSharedHandle succeeded, hr=0x%08lX) -- "
                        "SharedTextureMemoryDXGISharedHandle path is viable\n",
                        static_cast<long>(hr));
            CloseHandle(handle);
        } else {
            _snprintf_s(msg, _TRUNCATE,
                        "[dusk::vr] PROBE: swapchain image is NOT shareable "
                        "(CreateSharedHandle FAILED, hr=0x%08lX) -- runtime "
                        "did not allocate this with D3D12_HEAP_FLAG_SHARED, "
                        "need a different strategy (e.g. explicit copy)\n",
                        static_cast<long>(hr));
        }
        OutputDebugStringA(msg);
    }

    wgpu::Texture importSwapchainImage(const wgpu::Device& device, uint32_t index, uint32_t width,
                                        uint32_t height, wgpu::TextureFormat format) {
        probeSwapchainImageShareable();

        ensureFenceSync(device);

        dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor d3dDesc;
        d3dDesc.resource = swapchainImages_[index].texture;

        wgpu::SharedTextureMemoryDescriptor stmDesc{};
        stmDesc.nextInChain = &d3dDesc;

        wgpu::SharedTextureMemory stm = device.ImportSharedTextureMemory(&stmDesc);

        wgpu::TextureDescriptor texDesc{};
        texDesc.format = format;
        texDesc.size = {width, height, 1};
        texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;

        wgpu::Texture sharedTex = stm.CreateTexture(&texDesc);

        wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
        beginDesc.initialized = true;

        if (fenceInitialized_) {
            beginDesc.fenceCount = 1;
            beginDesc.fences = &dawnFence_;
            beginDesc.signaledValueCount = 1;
            beginDesc.signaledValues = &fenceValue_;
        }

        stm.BeginAccess(sharedTex, &beginDesc);

        pendingMemory_.push_back(stm);
        pendingTextures_.push_back(sharedTex);
        return sharedTex;
    }
#endif  // !DUSK_VR_XR_GRAPHICS_VULKAN

    // ------------------------------------------------------------------
    // CPU ROUND-TRIP COPY PATH (VR_MOD_HANDOFF_10 #3/#9, revised)
    // ------------------------------------------------------------------
    // Cross-device D3D12 resource/fence sharing is confirmed non-viable on
    // this adapter/driver (SharedTextureMemoryD3D12Resource unsupported;
    // ExternalImageDXGI doesn't exist in this Dawn build; swapchain images
    // aren't allocated D3D12_HEAP_FLAG_SHARED per probeSwapchainImageShareable()
    // above). This copies eye pixels Dawn-device -> CPU -> xrDevice_ instead
    // of sharing the resource. Slower, but the only path that doesn't crash.
    //
    // WHY THIS IS SPLIT IN TWO (root-caused this session, not guessed):
    // Aurora records an ENTIRE frame -- every render pass, including
    // resolve_pass's snapshot copy into colorTexture -- onto one shared
    // CommandEncoder (common.cpp's packet.encoder), and only calls
    // Finish()+Submit() once, inside aurora_end_frame()'s callback
    // (aurora.cpp:355). Nothing is actually on the GPU queue before that
    // point -- it's all just recorded, unsubmitted commands. The original
    // single-function submitEyeCpuCopy created its OWN encoder and called
    // Submit() synchronously from tick(), BEFORE aurora_end_frame() even
    // runs for that frame -- meaning its CopyTextureToBuffer read
    // colorTexture before the command that writes it had been submitted
    // at all. That's a read of an uninitialized resource, which
    // g_device's skip_validation toggle (RelWithDebInfo/NDEBUG, set in
    // gpu.cpp) lets through to raw D3D12 instead of catching cleanly --
    // matches the "E_INVALIDARG closing pending command list" / device-lost
    // crash seen this session.
    //
    // FIX: encodeEyeCopy() below records the Dawn-side copy via
    // push_encoder_task, so it lands on the SAME shared encoder, in-order,
    // right after resolve_pass's own snapshot copy -- both get submitted
    // together at end_frame(). Call it once per eye, right after endEye(),
    // same as the old submitEyeCpuCopy call site. The CPU-side readback
    // (readbackEyeCopy(), the MapAsync/D3D12-upload/XR-submit part) MUST
    // move to run AFTER aurora_end_frame() returns -- call it once per eye
    // there instead, using the same swapchainIndex/width/height/dstXOffset
    // args. Calling readbackEyeCopy() before aurora_end_frame() reintroduces
    // the exact bug this split fixes.
    //
    // dstXOffset EXISTS BECAUSE the swapchain is a single double-wide image
    // (startup()'s design, confirmed via vr_main.cpp's tick(): swapchain
    // width == 2 * eyeWidth, left eye at x=0, right eye at
    // x=eyeWidth -- see projViews[eye].subImage.imageRect in vr_main.cpp).
    // srcTexture is the per-eye-sized Dawn texture from endEye(); this
    // writes it into the correct half of the wider destination resource
    // rather than assuming a 1:1 same-size copy.
    //
    // CONFIRMED this session against the real dawn/webgpu_cpp.h: MapAsync's
    // signature, the callback shape (status, StringView), and
    // wgpu::MapAsyncStatus::Success are all exactly as used below -- no
    // longer a guess.

    // Registers the encoder task type backing encodeEyeCopy(). Call ONCE at
    // VR session startup (wherever Session is constructed), before the
    // first endEye()/encodeEyeCopy() of the first frame.
    void registerCpuCopyEncoderTask() {
        aurora::gfx::EncoderTaskDescriptor desc{
            .label = "vr_eye_cpu_copy",
            .callback = &Session::encoderTaskCallback,
            .userdata = this,
        };
        cpuCopyTaskId_ = aurora::gfx::register_encoder_task_type(desc);
    }

    // Call once per eye, immediately after endEye(), while still inside the
    // active render pass (push_encoder_task requires that -- see gfx.hpp).
    // Ensures the destination staging buffer exists, stashes srcTexture in a
    // small per-eye side-table (payload must stay POD/trivially-copyable per
    // gfx.hpp's InlineDrawPayloadSize contract -- wgpu::Texture itself is
    // ref-counted and NOT safe to memcpy through that payload), then pushes
    // the actual CopyTextureToBuffer to run on the frame's real encoder.
    //
    // FIXED this session: eyeIndex (0/1) added and used to key
    // cpuCopyBuffers_/pendingCopySrc_ -- NOT swapchainIndex. This is a
    // single shared double-wide swapchain image (one swapchainIndex per
    // FRAME, the same value for both eyes -- see startup()'s design
    // comment), so keying the per-eye staging buffer by swapchainIndex made
    // both eyes read/write the exact same wgpu::Buffer object every frame:
    // eye 1's Dawn-side copy silently overwrote eye 0's data in it before
    // either got read back, and the two eyes' sequential MapAsync/Unmap
    // cycles in readbackEyeCopy() colliding on that one shared buffer is
    // what produced this session's "Concurrent buffer operations are not
    // allowed" MapAsync crash. swapchainIndex is still used (unchanged)
    // for the actual XR swapchain image destination in readbackEyeCopy()
    // below -- that one genuinely is shared between eyes (same image,
    // different halves via dstXOffset), just not the CPU staging buffer.
    void encodeEyeCopy(const wgpu::Texture& srcTexture, uint32_t eyeIndex, uint32_t swapchainIndex,
                        uint32_t eyeWidth, uint32_t eyeHeight, uint32_t dstXOffset, wgpu::TextureFormat format) {
        ensureCpuCopyBuffers(eyeIndex, eyeWidth, eyeHeight, format);

        if (pendingCopySrc_.size() <= eyeIndex) {
            pendingCopySrc_.resize(eyeIndex + 1);
        }
        pendingCopySrc_[eyeIndex] = srcTexture; // keeps it alive until the task runs

        const CpuCopyTaskPayload payload{
            .eyeIndex = eyeIndex,
            .eyeWidth = eyeWidth,
            .eyeHeight = eyeHeight,
        };
        static_assert(sizeof(CpuCopyTaskPayload) <= aurora::gfx::InlineDrawPayloadSize,
                      "CpuCopyTaskPayload too large for inline encoder task payload");
        aurora::gfx::push_encoder_task(cpuCopyTaskId_, &payload, sizeof(payload));
    }

#if !DUSK_VR_XR_GRAPHICS_VULKAN
    // GPU-DIRECT SWAPCHAIN-COPY PATH (sameDeviceAsAurora_ / see that field's
    // comment for the full "why this exists" writeup). Only meaningful when
    // sameDeviceAsAurora_ is true -- vr_main.cpp only ever calls these two
    // functions (beginSwapchainAccessForFrame/encodeSwapchainCopy) when
    // usesGpuDirectSwapchainCopy() said yes.

    // Lazily wraps swapchainImages_[index].texture (an ID3D12Resource
    // created on Aurora's OWN device -- see sameDeviceAsAurora_'s comment
    // for why that's what makes this safe/documented, unlike
    // importSwapchainImage() above) as a Dawn wgpu::Texture via
    // SharedTextureMemory. No fence needed (unlike ensureFenceSync()'s dead
    // cross-device code) -- same device AND same command queue (both handed
    // to xrCreateSession via getD3D12DeviceAndQueue()), so Dawn's own
    // internal command-submission ordering already serializes our copy
    // against anything the XR runtime itself does with this queue.
    void ensureSwapchainTexture(uint32_t index, uint32_t width, uint32_t height) {
        if (swapchainTextures_.size() <= index) {
            swapchainMemory_.resize(index + 1);
            swapchainTextures_.resize(index + 1);
        }
        if (swapchainTextures_[index]) {
            return; // already imported -- same underlying image every time this index is acquired
        }

        dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor d3dDesc;
        d3dDesc.resource = swapchainImages_[index].texture;

        wgpu::SharedTextureMemoryDescriptor stmDesc{};
        stmDesc.nextInChain = &d3dDesc;
        swapchainMemory_[index] = aurora::webgpu::g_device.ImportSharedTextureMemory(&stmDesc);

        wgpu::TextureDescriptor texDesc{};
        // MUST be the swapchain's REAL format (swapchainDxgiFormat_), not
        // aurora's own native color format -- see fromDxgiSwapchainFormat()'s
        // comment for why those can differ.
        texDesc.format = fromDxgiSwapchainFormat(swapchainDxgiFormat_);
        texDesc.size = {width, height, 1};
        // RenderAttachment mirrors the swapchain's own declared XR usage
        // (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, createSwapchain()) --
        // CopyDst is what CopyBufferToTexture below actually needs. Same
        // combination importSwapchainImage()'s already-compiling dead code
        // above used, not a new guess.
        texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
        swapchainTextures_[index] = swapchainMemory_[index].CreateTexture(&texDesc);
    }

    // Call ONCE per frame, right after xrAcquireSwapchainImage/
    // xrWaitSwapchainImage succeed -- NOT per eye (both eyes write into the
    // same double-wide swapchain image/index this frame; BeginAccess must
    // only be opened once per actual use of the resource, not once per
    // logical eye). endAccessAll() (already called once per frame from
    // submitFrame(), unchanged) closes this out via the existing
    // pendingMemory_/pendingTextures_ bookkeeping -- reused as-is, not
    // duplicated.
    // INTERMEDIATE-TEXTURE VARIANT (2026-09-19) -- the path that actually
    // runs in practice (see kDirectSwapchainImport). Root cause it works
    // around: Virtual Desktop (and, per the OpenXR spec, any runtime is
    // free to) allocates the swapchain's real ID3D12Resource as the
    // TYPELESS member of the requested format family so it can expose
    // both SRGB and non-SRGB views of one resource -- and Dawn's D3D12
    // SharedTextureMemory import only accepts typed formats (confirmed
    // against Dawn's own UtilsD3D.cpp format table: every _SRGB/_UNORM
    // variant is listed, no _TYPELESS ones). So instead of importing the
    // RUNTIME's resource, create OUR OWN typed one on the same device
    // (xrDevice_ == Aurora's own ID3D12Device whenever sameDeviceAsAurora_,
    // which is the only time this is ever reached), import THAT into Dawn
    // (typed, so the import is valid), let the gamma-compute pass write
    // into it exactly as it would have written into the swapchain texture,
    // and once Dawn's EndAccess has run, copy it into the real swapchain
    // image with one raw same-queue ID3D12 CopyTextureRegion
    // (finishIntermediateSwapchainCopy()). D3D12 permits CopyTextureRegion
    // between a typed format and its own typeless family member, so this
    // works whether the runtime allocated typed OR typeless. Zero CPU
    // touch, no separate device, no MapAsync -- the one extra GPU-side
    // 39MB-ish copy per frame is negligible next to the multi-millisecond
    // CPU stall it replaces.
    //
    // Dawn's D3D12 import requirements, read from its own
    // SharedTextureMemoryD3D12.cpp rather than assumed: Texture2D, 1 mip,
    // 1 array slice, 1 sample, and the resource MUST carry
    // D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS (a hard validation
    // error otherwise). That flag is also what makes the raw copy below
    // barrier-free on the source side: simultaneous-access resources
    // decay to COMMON at every ExecuteCommandLists boundary and get
    // implicitly promoted to COPY_SOURCE for the read.
    //
    // Sync: same device AND same queue as Dawn (both handed to
    // xrCreateSession), and the raw copy is only ever recorded/submitted
    // from submitFrame() AFTER aurora::gfx::synchronize() has confirmed
    // the render worker already called Submit() for this frame -- D3D12
    // executes command lists on one queue in submission order, so our copy
    // is ordered after Dawn's write with no fence handshake needed, and
    // next frame's Dawn write (submitted later still) is ordered after our
    // read of the same intermediate. One intermediate resource for the
    // whole session is therefore sufficient.
    void ensureIntermediateTexture(uint32_t width, uint32_t height) {
        if (intermediateTexture_ || intermediateCreateFailed_) {
            return;
        }

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        // The swapchain's REAL requested format (typed), e.g.
        // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB -- same value the gamma-compute
        // shader already packs bytes for, and the same family the runtime's
        // (possibly typeless) resource belongs to, which is what makes the
        // final CopyTextureRegion legal.
        desc.Format = static_cast<DXGI_FORMAT>(swapchainDxgiFormat_);
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        HRESULT hr = xrDevice_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&intermediateResource_));
        if (FAILED(hr) || !intermediateResource_) {
            char msg[192];
            duskVrSnprintf(msg, sizeof(msg),
                           "[dusk::vr] ensureIntermediateTexture: CreateCommittedResource failed "
                           "(hr=0x%08lx) -- disabling GPU-direct swapchain copy, falling back to "
                           "CPU readback\n",
                           static_cast<unsigned long>(hr));
            duskVrLog(msg);
            intermediateCreateFailed_ = true;
            return;
        }

        // Aurora's uncaptured-error callback FATALs on any post-init Dawn
        // validation error -- bracket the two Dawn calls below in an error
        // scope so a rejected import degrades to the CPU path with a log
        // line instead of aborting the whole game (the exact failure mode
        // the typeless-format crash had before it was detected up front).
        aurora::webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);

        dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor d3dDesc;
        d3dDesc.resource = intermediateResource_;

        wgpu::SharedTextureMemoryDescriptor stmDesc{};
        stmDesc.nextInChain = &d3dDesc;
        intermediateMemory_ = aurora::webgpu::g_device.ImportSharedTextureMemory(&stmDesc);

        wgpu::TextureDescriptor texDesc{};
        texDesc.format = fromDxgiSwapchainFormat(swapchainDxgiFormat_);
        texDesc.size = {width, height, 1};
        // CopyDst is all encoderTaskCallback()'s CopyBufferToTexture needs.
        // (RenderAttachment would additionally require ALLOW_RENDER_TARGET
        // on the resource -- not asked for, not needed.)
        texDesc.usage = wgpu::TextureUsage::CopyDst;
        intermediateTexture_ = intermediateMemory_.CreateTexture(&texDesc);

        bool scopeDone = false;
        bool scopeFailed = false;
        std::string scopeMessage;
        const auto future = aurora::webgpu::g_device.PopErrorScope(
            wgpu::CallbackMode::WaitAnyOnly,
            [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
                scopeDone = true;
                if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
                    scopeFailed = true;
                    scopeMessage = std::string{std::string_view{message}};
                }
            });
        aurora::webgpu::g_instance.WaitAny(future, 5000000000);

        if (!scopeDone || scopeFailed || !intermediateTexture_) {
            char msg[512];
            duskVrSnprintf(msg, sizeof(msg),
                           "[dusk::vr] ensureIntermediateTexture: Dawn rejected the intermediate "
                           "texture import (%s) -- disabling GPU-direct swapchain copy, falling "
                           "back to CPU readback\n",
                           scopeDone ? scopeMessage.c_str() : "PopErrorScope timed out");
            duskVrLog(msg);
            intermediateTexture_ = nullptr;
            intermediateMemory_ = nullptr;
            intermediateResource_.Reset();
            intermediateCreateFailed_ = true;
            return;
        }

        intermediateWidth_ = width;
        intermediateHeight_ = height;
        char msg[256];
        duskVrSnprintf(msg, sizeof(msg),
                       "[dusk::vr] ensureIntermediateTexture: %ux%u dxgiFormat=%lld imported into "
                       "Dawn -- GPU-direct swapchain copy via intermediate texture is ACTIVE "
                       "(no CPU readback)\n",
                       width, height, static_cast<long long>(swapchainDxgiFormat_));
        duskVrLog(msg);
    }

    void beginSwapchainAccessForFrame(uint32_t index, uint32_t width, uint32_t height) {
        if (usesIntermediateSwapchainCopy()) {
            ensureIntermediateTexture(width, height);
            if (!intermediateTexture_) {
                // Creation failed -- intermediateCreateFailed_ is now set,
                // so usesGpuDirectSwapchainCopy() reads false for the rest
                // of the session and tick()'s per-eye branch below this
                // call falls through to encodeEyeCopy()/readbackEyeCopy().
                return;
            }
            wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
            beginDesc.initialized = true;
            intermediateMemory_.BeginAccess(intermediateTexture_, &beginDesc);
            pendingMemory_.push_back(intermediateMemory_);
            pendingTextures_.push_back(intermediateTexture_);
            return;
        }

        ensureSwapchainTexture(index, width, height);

        wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
        // Content doesn't need to be preserved -- both eyes fully overwrite
        // their half of the image every frame -- but `initialized` also
        // gates whether Dawn treats the resource as safe to touch at all
        // (an uninitialized-content resource can still be a validly
        // usable one); true matches what the swapchain's own
        // xrWaitSwapchainImage contract already guarantees (a real,
        // previously-released image, not raw uninitialized memory).
        beginDesc.initialized = true;
        swapchainMemory_[index].BeginAccess(swapchainTextures_[index], &beginDesc);

        pendingMemory_.push_back(swapchainMemory_[index]);
        pendingTextures_.push_back(swapchainTextures_[index]);
    }

    // The GPU-direct equivalent of encodeEyeCopy() -- call once per eye,
    // same call site/timing (right after endEye(), inside tick()). Pushes
    // the SAME encoder task type encodeEyeCopy() uses (cpuCopyTaskId_) --
    // encoderTaskCallback() itself branches on sameDeviceAsAurora_ to decide
    // whether its tail writes into res.readback (CPU path) or straight into
    // swapchainTextures_[swapchainIndex] (this path) -- so no second task
    // type/registration is needed.
    void encodeSwapchainCopy(const wgpu::Texture& srcTexture, uint32_t eyeIndex, uint32_t swapchainIndex,
                              uint32_t eyeWidth, uint32_t eyeHeight, uint32_t dstXOffset,
                              wgpu::TextureFormat format) {
        // Still needed: the gamma compute pass's OWN buffers (gammaStorage/
        // gammaUniform) -- everything about that pass is unchanged, only
        // its final destination (this function's caller's
        // encoderTaskCallback branch) differs. res.readback/uploadHeap
        // (the CPU-path-only members this also allocates) simply go
        // unused on this path -- harmless, not worth special-casing out.
        ensureCpuCopyBuffers(eyeIndex, eyeWidth, eyeHeight, format);

        if (pendingCopySrc_.size() <= eyeIndex) {
            pendingCopySrc_.resize(eyeIndex + 1);
        }
        pendingCopySrc_[eyeIndex] = srcTexture;

        const CpuCopyTaskPayload payload{
            .eyeIndex = eyeIndex,
            .eyeWidth = eyeWidth,
            .eyeHeight = eyeHeight,
            .swapchainIndex = swapchainIndex,
            .dstXOffset = dstXOffset,
        };
        static_assert(sizeof(CpuCopyTaskPayload) <= aurora::gfx::InlineDrawPayloadSize,
                      "CpuCopyTaskPayload too large for inline encoder task payload");
        aurora::gfx::push_encoder_task(cpuCopyTaskId_, &payload, sizeof(payload));
    }

    // Second half of the intermediate-texture path (see
    // ensureIntermediateTexture()'s comment): one raw same-queue
    // CopyTextureRegion from the (already EndAccess'd -- endAccessAll()
    // must run BEFORE this, so Dawn has formally handed the resource back)
    // typed intermediate into the real swapchain image. Call ONCE per frame
    // from submitFrame(), after aurora::gfx::synchronize() and
    // endAccessAll(), with the full double-wide dimensions. Non-blocking:
    // command allocators are ring-buffered and only waited on if the GPU
    // is somehow still executing that slot's copy from two frames ago,
    // same shape as readbackEyeCopy()'s own slot logic.
    void finishIntermediateSwapchainCopy(uint32_t swapchainIndex, uint32_t width, uint32_t height) {
        if (!intermediateTexture_) {
            return; // creation failed earlier this session -- already logged
        }
        ensureCpuCopyCmdList(); // creates copyFence_ if the CPU path never did

        const uint32_t slot = intermediateSlot_;
        if (!intermediateCmdList_[slot]) {
            xrDevice_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&intermediateCmdAlloc_[slot]));
            xrDevice_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, intermediateCmdAlloc_[slot].Get(),
                                          nullptr, IID_PPV_ARGS(&intermediateCmdList_[slot]));
            intermediateCmdList_[slot]->Close();
        }
        if (copyFence_->GetCompletedValue() < intermediateSlotFenceValue_[slot]) {
            HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            copyFence_->SetEventOnCompletion(intermediateSlotFenceValue_[slot], event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }

        ID3D12CommandAllocator* alloc = intermediateCmdAlloc_[slot].Get();
        ID3D12GraphicsCommandList* list = intermediateCmdList_[slot].Get();
        alloc->Reset();
        list->Reset(alloc, nullptr);

        ID3D12Resource* dstResource = swapchainImages_[swapchainIndex].texture;

        // Destination: same COMMON -> COPY_DEST -> COMMON bracket the CPU
        // path already uses on this exact resource. Source: no barrier --
        // ALLOW_SIMULTANEOUS_ACCESS resources are implicitly promoted from
        // COMMON to COPY_SOURCE and decay back after execution.
        D3D12_RESOURCE_BARRIER toDest{};
        toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDest.Transition.pResource = dstResource;
        toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &toDest);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstResource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = intermediateResource_.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_BOX srcBox{0, 0, 0, width, height, 1};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);

        D3D12_RESOURCE_BARRIER toCommon = toDest;
        std::swap(toCommon.Transition.StateBefore, toCommon.Transition.StateAfter);
        list->ResourceBarrier(1, &toCommon);

        list->Close();
        ID3D12CommandList* lists[] = {list};
        xrQueue_->ExecuteCommandLists(1, lists);

        ++copyFenceValue_;
        xrQueue_->Signal(copyFence_.Get(), copyFenceValue_);
        intermediateSlotFenceValue_[slot] = copyFenceValue_;
        intermediateSlot_ = (slot + 1) % kIntermediateSlotCount;
    }
#endif  // !DUSK_VR_XR_GRAPHICS_VULKAN

#if DUSK_VR_XR_GRAPHICS_VULKAN
    // SHARED-IMAGE GPU-DIRECT SWAPCHAIN-COPY PATH, Vulkan/Android
    // (usesSharedImageGpuDirect_ -- see that field's comment for the full
    // "why this exists" writeup). Dawn's Vulkan backend exposes NO way to
    // get its own VkDevice/VkQueue (only GetInstance()/GetInstanceProcAddr()
    // in the vendored VulkanBackend.h, unlike the D3D12 backend's
    // GetD3D12Device()/GetD3D12CommandQueue() the branch above relies on),
    // so the D3D12 same-device trick does not port -- there is nothing to
    // hand xrCreateSession. Instead: allocate a normal VkImage on the
    // XR-side device with EXPORTABLE memory (VK_KHR_external_memory_fd,
    // opaque fd), import that same memory into Dawn as a wgpu::Texture
    // (ImportSharedTextureMemory + SharedTextureMemoryOpaqueFDDescriptor),
    // let the gamma-compute pass write into it exactly like the D3D12
    // intermediate texture, and once Dawn signals completion, one GPU-side
    // blit on the XR device moves it into the real swapchain image -- zero
    // CPU-visible pixel data at any point.
    //
    // HISTORY (2026-09-18/19): the first version of this path shared an
    // AHardwareBuffer instead of an exported VkImage. Real measurements on a
    // Quest 3 killed that design: vkCmdCopyImage from the gralloc-backed
    // AHB image cost ~7ms of CPU time just to RECORD (every other command
    // in the buffer was microseconds; neither a linear AHB layout nor a
    // matching-format import changed it), and the fast alternative,
    // vkCmdBlitImage (~11us), format-CONVERTS -- with the AHB's mandatory
    // UNORM format and the runtime's sRGB swapchain it sRGB-encoded bytes
    // the gamma shader had already encoded (confirmed washed-out colors in
    // the headset). An AHB cannot legally be imported as its sRGB sibling
    // (VUID-VkMemoryAllocateInfo-pNext-02387: format must be exactly what
    // vkGetAndroidHardwareBufferPropertiesANDROID reports). An exported
    // VkImage has no such restriction: it is created with the swapchain's
    // OWN format, so the blit's source and destination formats are
    // identical and the blit is an exact byte-preserving identity, while
    // keeping the cheap record cost.
    //
    // Sync design (unchanged from the AHB version): kSharedSlotCount
    // frames in flight, each with its own image + command buffer + fence +
    // semaphore. Dawn's EndAccess exports a SharedFence (SyncFD on Android
    // -- Dawn prefers it whenever enabled; OpaqueFD handled too) that is
    // dup()ed (Dawn keeps ownership of the fd it hands out and closes it)
    // and imported as a VkSemaphore the blit submit waits on -- a GPU-side
    // handoff, no CPU stall. A slot's fence is only waited on when that
    // slot comes around again kSharedSlotCount frames later.

    static constexpr uint32_t kSharedSlotCount = 2;
    struct SharedImageSlot {
        wgpu::SharedTextureMemory memory;
        wgpu::Texture texture;
        // View of `texture` in aurora's scene color format (RGBA8Unorm on an
        // R8G8B8A8_SRGB image, i.e. a raw-bytes reinterpretation) so the eye
        // pass can render straight into the shared image with the same
        // pipelines -- see sharedImageRenderTarget(). Null if the view
        // couldn't be made; callers then fall back to the copy hand-off.
        wgpu::TextureView renderView;
        VkImage xrImage = VK_NULL_HANDLE;
        VkDeviceMemory xrMemory = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        VkFence copyFence = VK_NULL_HANDLE;
        bool copyPending = false;          // copyFence has an unwaited submit outstanding
        VkSemaphore dawnDoneSemaphore = VK_NULL_HANDLE;
        // Layout Dawn reported leaving the image in at its last EndAccess
        // (SharedTextureMemoryVkImageLayoutEndState::newLayout) -- used as
        // the XR-side acquire barrier's oldLayout so Dawn's writes are
        // preserved (an UNDEFINED transition may legally discard them).
        VkImageLayout dawnEndLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    // Vulkan counterpart of the D3D12 branch's fromDxgiSwapchainFormat():
    // the wgpu format Dawn must be told for a texture wrapping memory laid
    // out as the swapchain's VkFormat. Only the 8bpc RGBA/BGRA candidates
    // createSwapchain() can pick for the gamma-compute path.
    static bool fromVkSwapchainFormat(int64_t vkFormat, wgpu::TextureFormat* out) {
        switch (vkFormat) {
            case VK_FORMAT_R8G8B8A8_UNORM: *out = wgpu::TextureFormat::RGBA8Unorm; return true;
            case VK_FORMAT_R8G8B8A8_SRGB: *out = wgpu::TextureFormat::RGBA8UnormSrgb; return true;
            case VK_FORMAT_B8G8R8A8_UNORM: *out = wgpu::TextureFormat::BGRA8Unorm; return true;
            case VK_FORMAT_B8G8R8A8_SRGB: *out = wgpu::TextureFormat::BGRA8UnormSrgb; return true;
            default: return false;
        }
    }

    // One VkImage created on the XR-side device with exportable (opaque-fd)
    // memory and imported into Dawn as a wgpu::Texture over the SAME memory
    // -- the building block of every GPU-direct hand-off on this branch:
    // the color swapchain staging image (SharedImageSlot) and, since
    // 2026-09-20, space warp's motion-vector and depth images
    // (SpaceWarpSlot). Factored out of ensureSharedImageResources() verbatim
    // when space warp needed the same thing twice more.
    struct ExportedImage {
        wgpu::SharedTextureMemory memory;
        wgpu::Texture texture;
        // View in ExportedImageDesc::dawnViewFormat (the texture's own format
        // when that's Undefined).
        wgpu::TextureView view;
        VkImage xrImage = VK_NULL_HANDLE;
        VkDeviceMemory xrMemory = VK_NULL_HANDLE;
        VkFormat vkFormat = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        // Layout Dawn reported leaving the image in at its last EndAccess
        // (SharedTextureMemoryVkImageLayoutEndState::newLayout) -- used as
        // the XR-side acquire barrier's oldLayout so Dawn's writes are
        // preserved (an UNDEFINED transition may legally discard them).
        VkImageLayout dawnEndLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Imported (TEMPORARY) from Dawn's EndAccess fence each frame the
        // image is handed over -- see importDawnEndFence().
        VkSemaphore dawnDoneSemaphore = VK_NULL_HANDLE;
    };
    struct ExportedImageDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        VkFormat vkFormat = VK_FORMAT_UNDEFINED;
        // MUTABLE_FORMAT + a VkImageFormatListCreateInfo naming {vkFormat,
        // vkViewFormat} so a second-format view is legal AND the driver
        // keeps UBWC compression (see the note inside createExportedImage()).
        // VK_FORMAT_UNDEFINED: no format list, no MUTABLE flag.
        VkFormat vkViewFormat = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags vkUsage = 0;
        wgpu::TextureFormat dawnFormat = wgpu::TextureFormat::Undefined;
        wgpu::TextureFormat dawnViewFormat = wgpu::TextureFormat::Undefined;  // Undefined = dawnFormat
        wgpu::TextureUsage dawnUsage = wgpu::TextureUsage::None;
    };

    // Returns false with a reason in errBuf. On failure `out` may hold a
    // partially-created XR-side image/memory; callers treat the whole
    // feature as failed for the session (same as every other one-way
    // failure flag in this file), so nothing is torn down.
    bool createExportedImage(const ExportedImageDesc& d, ExportedImage& out, char* errBuf, size_t errLen) {
        auto fail = [&](const char* what) {
            duskVrSnprintf(errBuf, errLen, "%s", what);
            return false;
        };
        out.vkFormat = d.vkFormat;
        out.width = d.width;
        out.height = d.height;

        // --- XR-side: a normal optimally-tiled image whose memory can be
        // exported. The SAME VkImageCreateInfo (pNext chain and all) is
        // handed to Dawn, which creates its own VkImage from it -- the
        // opaque-fd sharing contract requires identical creation
        // parameters on both sides, and Dawn requires the chain to hold
        // exactly one VkExternalMemoryImageCreateInfo with OPAQUE_FD.
        // MUTABLE_FORMAT alone makes Adreno drop UBWC framebuffer compression
        // (measured: the eye pass rendering into this image cost ~12ms vs
        // ~10ms into a pooled texture). Declaring the exact view-format set
        // via VkImageFormatListCreateInfo (VK_KHR_image_format_list, core in
        // 1.2) is what lets the driver keep compression for a mutable image.
        const bool mutableFormat = d.vkViewFormat != VK_FORMAT_UNDEFINED;
        const VkFormat viewFormats[2] = {d.vkFormat, d.vkViewFormat};
        VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        formatList.viewFormatCount = d.vkViewFormat != d.vkFormat ? 2u : 1u;
        formatList.pViewFormats = viewFormats;

        VkExternalMemoryImageCreateInfo extMemImageInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extMemImageInfo.pNext = mutableFormat ? &formatList : nullptr;
        extMemImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageCI.pNext = &extMemImageInfo;
        imageCI.imageType = VK_IMAGE_TYPE_2D;
        imageCI.format = d.vkFormat;
        imageCI.extent = {d.width, d.height, 1};
        imageCI.mipLevels = 1;
        imageCI.arrayLayers = 1;
        imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Dawn creates its own VkImage from this same create info for the
        // imported memory, so every usage bit either side needs must be here.
        imageCI.flags = mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        imageCI.usage = d.vkUsage;
        imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(xrDevice_, &imageCI, nullptr, &out.xrImage) != VK_SUCCESS) {
            return fail("vkCreateImage (exportable) failed");
        }

        VkMemoryDedicatedRequirements dedicatedReqs{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 memReqs2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicatedReqs};
        VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = out.xrImage;
        vkGetImageMemoryRequirements2(xrDevice_, &reqInfo, &memReqs2);
        const VkMemoryRequirements& memReqs = memReqs2.memoryRequirements;

        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(xrPhysicalDevice_, &memProps);
        uint32_t memTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReqs.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memTypeIndex = i;
                break;
            }
        }
        if (memTypeIndex == UINT32_MAX) {
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if (memReqs.memoryTypeBits & (1u << i)) {
                    memTypeIndex = i;
                    break;
                }
            }
        }
        if (memTypeIndex == UINT32_MAX) {
            return fail("no usable memory type for the exportable image");
        }

        // Always dedicated: required by some drivers for exportable images,
        // always permitted, and it keeps the two sides' allocations
        // symmetric (Dawn is told dedicatedAllocation=true below).
        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = out.xrImage;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicatedInfo};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = memTypeIndex;
        if (vkAllocateMemory(xrDevice_, &allocInfo, nullptr, &out.xrMemory) != VK_SUCCESS) {
            return fail("vkAllocateMemory (exportable) failed");
        }
        if (vkBindImageMemory(xrDevice_, out.xrImage, out.xrMemory, 0) != VK_SUCCESS) {
            return fail("vkBindImageMemory failed");
        }

        // vkGetMemoryFdKHR isn't directly linkable through Android's Vulkan
        // loader (same as vkImportSemaphoreFdKHR in importDawnEndFence())
        // -- resolve via vkGetDeviceProcAddr.
        static PFN_vkGetMemoryFdKHR pfnGetMemoryFdKHR = nullptr;
        if (!pfnGetMemoryFdKHR) {
            pfnGetMemoryFdKHR =
                reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(xrDevice_, "vkGetMemoryFdKHR"));
        }
        if (!pfnGetMemoryFdKHR) {
            return fail("vkGetMemoryFdKHR unavailable on the XR device");
        }
        VkMemoryGetFdInfoKHR getFdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        getFdInfo.memory = out.xrMemory;
        getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int memoryFd = -1;
        if (pfnGetMemoryFdKHR(xrDevice_, &getFdInfo, &memoryFd) != VK_SUCCESS || memoryFd < 0) {
            return fail("vkGetMemoryFdKHR failed");
        }

        // --- Dawn side. Bracketed in an error scope: Aurora's uncaptured-
        // error callback FATALs on any post-init Dawn validation error, so a
        // rejected import must degrade to a fallback path with a log line.
        aurora::webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
        {
            wgpu::SharedTextureMemoryOpaqueFDDescriptor fdDesc{};
            fdDesc.vkImageCreateInfo = &imageCI;
            fdDesc.memoryFD = memoryFd;
            fdDesc.memoryTypeIndex = memTypeIndex;
            fdDesc.allocationSize = memReqs.size;
            fdDesc.dedicatedAllocation = true;
            wgpu::SharedTextureMemoryDescriptor stmDesc{};
            stmDesc.nextInChain = &fdDesc;
            out.memory = aurora::webgpu::g_device.ImportSharedTextureMemory(&stmDesc);

            const wgpu::TextureFormat viewFormat =
                d.dawnViewFormat != wgpu::TextureFormat::Undefined ? d.dawnViewFormat : d.dawnFormat;
            const bool needsViewFormat = viewFormat != d.dawnFormat;
            wgpu::TextureDescriptor texDesc{};
            texDesc.format = d.dawnFormat;
            texDesc.size = {d.width, d.height, 1};
            texDesc.usage = d.dawnUsage;
            texDesc.viewFormatCount = needsViewFormat ? 1 : 0;
            texDesc.viewFormats = needsViewFormat ? &viewFormat : nullptr;
            out.texture = out.memory.CreateTexture(&texDesc);
            if (out.texture) {
                wgpu::TextureViewDescriptor viewDesc{};
                viewDesc.format = viewFormat;
                viewDesc.dimension = wgpu::TextureViewDimension::e2D;
                viewDesc.mipLevelCount = 1;
                viewDesc.arrayLayerCount = 1;
                out.view = out.texture.CreateView(&viewDesc);
            }
        }
        // Dawn dup()s the fd on import (verified in its SharedTextureMemoryVk.cpp)
        // -- ours is still ours to close, success or failure.
        close(memoryFd);
        {
            bool done = false;
            bool failed = false;
            std::string message;
            const auto future = aurora::webgpu::g_device.PopErrorScope(
                wgpu::CallbackMode::WaitAnyOnly,
                [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView msg) {
                    done = true;
                    if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
                        failed = true;
                        message = std::string{std::string_view{msg}};
                    }
                });
            aurora::webgpu::g_instance.WaitAny(future, 5000000000);
            if (!done || failed || !out.texture) {
                duskVrSnprintf(errBuf, errLen, "Dawn rejected the opaque-fd image import: %s",
                               done ? message.c_str() : "PopErrorScope timed out");
                return false;
            }
        }
        VkSemaphoreCreateInfo semCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(xrDevice_, &semCI, nullptr, &out.dawnDoneSemaphore);
        return true;
    }

    // Lazily creates one slot's exported VkImage on the XR-side device and
    // imports its memory into Dawn (memory/texture). width/height are the
    // FULL double-wide swapchain dimensions (both eyes share one image,
    // offset via dstXOffset in encodeSwapchainCopy()). Any failure sets
    // sharedImageCreateFailed_, after which usesSharedImageGpuDirect() reads
    // false for the rest of the session and the caller falls back to the
    // CPU-readback path for this and every later frame.
    void ensureSharedImageResources(uint32_t slotIndex, uint32_t width, uint32_t height) {
        SharedImageSlot& s = sharedSlots_[slotIndex];
        if (s.texture || sharedImageCreateFailed_) {
            return;
        }
        auto fail = [&](const char* what) {
            char buf[256];
            duskVrSnprintf(buf, sizeof(buf),
                           "[dusk::vr] ensureSharedImageResources(slot %u): %s -- disabling shared-image "
                           "GPU-direct path, falling back to CPU readback\n",
                           slotIndex, what);
            duskVrLog(buf);
            s.texture = nullptr;
            s.memory = nullptr;
            sharedImageCreateFailed_ = true;
        };

        wgpu::TextureFormat dawnFormat;
        if (!fromVkSwapchainFormat(swapchainDxgiFormat_, &dawnFormat)) {
            fail("swapchain VkFormat has no wgpu counterpart");
            return;
        }

        // The swapchain's own format: the final vkCmdBlitImage is then
        // format-identical, i.e. an exact byte copy (see the HISTORY note).
        // Second view format (the UNORM sibling of an sRGB swapchain format):
        // the direct-render path (sharedImageRenderTarget()) draws the eye
        // pass into an RGBA8Unorm view of this image. TRANSFER_DST: Dawn's
        // CopyBufferToTexture writes (Dawn validates this bit is present).
        // TRANSFER_SRC: the XR-side blit reads. COLOR_ATTACHMENT + SAMPLED:
        // the direct render, and in-pass GXCopyTex captures sampling it.
        const VkFormat swapchainVkFormat = static_cast<VkFormat>(swapchainDxgiFormat_);
        const VkFormat unormVkFormat = swapchainVkFormat == VK_FORMAT_R8G8B8A8_SRGB   ? VK_FORMAT_R8G8B8A8_UNORM
                                       : swapchainVkFormat == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM
                                                                                      : swapchainVkFormat;
        ExportedImageDesc d{};
        d.width = width;
        d.height = height;
        d.vkFormat = swapchainVkFormat;
        d.vkViewFormat = unormVkFormat;  // == vkFormat for a UNORM swapchain: 1-entry list, still MUTABLE (as before)
        d.vkUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        d.dawnFormat = dawnFormat;
        d.dawnViewFormat = aurora::gfx::color_format();  // renderView: aurora's scene format
        d.dawnUsage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc |
                      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        ExportedImage img;
        char err[512];
        if (!createExportedImage(d, img, err, sizeof(err))) {
            fail(err);
            return;
        }
        s.memory = img.memory;
        s.texture = img.texture;
        s.renderView = img.view;
        s.xrImage = img.xrImage;
        s.xrMemory = img.xrMemory;
        s.dawnDoneSemaphore = img.dawnDoneSemaphore;

        // --- Per-slot XR-side sync objects. ---
        ensureSharedCopyCmdPool();
        VkCommandBufferAllocateInfo cbAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAllocInfo.commandPool = sharedCopyCmdPool_;
        cbAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAllocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(xrDevice_, &cbAllocInfo, &s.cmdBuf);
        VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(xrDevice_, &fenceCI, nullptr, &s.copyFence);

        char buf[224];
        duskVrSnprintf(buf, sizeof(buf),
                       "[dusk::vr] ensureSharedImageResources(slot %u): %ux%u vkFormat=%lld exported image "
                       "imported into Dawn -- GPU-direct swapchain copy ACTIVE (no CPU readback)\n",
                       slotIndex, width, height, static_cast<long long>(swapchainDxgiFormat_));
        duskVrLog(buf);
    }

    void ensureSharedCopyCmdPool() {
        if (sharedCopyCmdPool_ != VK_NULL_HANDLE) {
            return;
        }
        VkCommandPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCI.queueFamilyIndex = xrQueueFamilyIndex_;
        vkCreateCommandPool(xrDevice_, &poolCI, nullptr, &sharedCopyCmdPool_);
    }

    // Call ONCE per frame, right after xrAcquireSwapchainImage/
    // xrWaitSwapchainImage succeed -- same timing/role as the D3D12
    // branch's beginSwapchainAccessForFrame() (name reused deliberately so
    // vr_main.cpp's call site is branch-agnostic). NOT per eye -- both eyes
    // write into the same slot's texture this frame, different halves.
    void beginSwapchainAccessForFrame(uint32_t /*index*/, uint32_t width, uint32_t height) {
        const uint32_t slotIndex = sharedNextSlot_;
        sharedWidth_ = width;
        sharedHeight_ = height;
        ensureSharedImageResources(slotIndex, width, height);
        SharedImageSlot& s = sharedSlots_[slotIndex];
        if (!s.texture) {
            return; // failed -- sharedImageCreateFailed_ set, caller falls back to the CPU path
        }

        // This slot was last used kSharedSlotCount frames ago; its XR-side copy
        // (which reads the AHB) must be finished before Dawn writes into the
        // same buffer again. Normally long complete by now -- a genuine
        // wait here only happens if the GPU has fallen more than
        // kSharedSlotCount frames behind, i.e. real backpressure, not the
        // guaranteed every-frame stall the old single-buffered code had.
        if (s.copyPending) {
            vkWaitForFences(xrDevice_, 1, &s.copyFence, VK_TRUE, UINT64_MAX);
            vkResetFences(xrDevice_, 1, &s.copyFence);
            s.copyPending = false;
            spaceWarpDebugReadbackLog(slotIndex);  // TEMP diagnostic, no-op unless pending
        }

        // Dawn's Vulkan backend REQUIRES this chained struct (found via a
        // real error-scope capture: "Expected chain root to match ...
        // [ SType::SharedTextureMemoryVkImageLayoutBeginState ]"). Both
        // UNDEFINED is correct here: Dawn fully overwrites the texture every
        // frame, so letting it transition from UNDEFINED (discarding whatever
        // the previous frame left) is exactly right, and it sidesteps having
        // to describe the XR side's own last transition to Dawn.
        wgpu::SharedTextureMemoryVkImageLayoutBeginState vkLayoutBegin{};
        vkLayoutBegin.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkLayoutBegin.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
        beginDesc.nextInChain = &vkLayoutBegin;
        beginDesc.initialized = true;

        s.memory.BeginAccess(s.texture, &beginDesc);
        sharedFrameSlot_ = slotIndex;
        sharedFrameSlotValid_ = true;
    }

    // The GPU-direct equivalent of encodeEyeCopy() for this branch -- call
    // once per eye, same call site/timing as the D3D12 branch's
    // encodeSwapchainCopy(). Pushes the SAME encoder task type
    // encodeEyeCopy() uses; encoderTaskCallback() branches on
    // usesSharedImageGpuDirect() and writes into sharedSlots_[payload.sharedSlot].texture.
    // swapchainIndex is unused here (kept for signature parity) -- the real
    // swapchain image isn't touched until finishSharedImageGpuCopy(), once per
    // FRAME, not per eye.
    void encodeSwapchainCopy(const wgpu::Texture& srcTexture, uint32_t eyeIndex, uint32_t swapchainIndex,
                              uint32_t eyeWidth, uint32_t eyeHeight, uint32_t dstXOffset,
                              wgpu::TextureFormat format) {
        ensureCpuCopyBuffers(eyeIndex, eyeWidth, eyeHeight, format);

        if (pendingCopySrc_.size() <= eyeIndex) {
            pendingCopySrc_.resize(eyeIndex + 1);
        }
        pendingCopySrc_[eyeIndex] = srcTexture;

        const CpuCopyTaskPayload payload{
            .eyeIndex = eyeIndex,
            .eyeWidth = eyeWidth,
            .eyeHeight = eyeHeight,
            .swapchainIndex = swapchainIndex,
            .dstXOffset = dstXOffset,
            .sharedSlot = sharedFrameSlot_,
        };
        static_assert(sizeof(CpuCopyTaskPayload) <= aurora::gfx::InlineDrawPayloadSize,
                      "CpuCopyTaskPayload too large for inline encoder task payload");
        aurora::gfx::push_encoder_task(cpuCopyTaskId_, &payload, sizeof(payload));
    }

    // Imports the completion fence Dawn's EndAccess handed back (endState)
    // into `semaphore` (TEMPORARY import -- the payload is consumed by one
    // queue wait and the semaphore reverts to its own unsignaled state,
    // ready for this slot's next import kSharedSlotCount frames later, by
    // which point that wait has provably executed since
    // beginSwapchainAccessForFrame() waited on the slot's copy fence).
    // Returns true when `semaphore` now carries a payload the copy submit
    // must wait on. *knownComplete is set when Dawn reports the work is
    // already finished (a SyncFD of -1, Dawn's kSemaphoreFdAlreadySignaledFd
    // convention) -- nothing to wait on at all, GPU- or CPU-side. Both
    // false: no usable fence, caller must fall back to a CPU wait.
    // Type is read from the fence Dawn returned: SyncFD (Dawn's preference
    // on Android when enabled) or OpaqueFD.
    bool importDawnEndFence(const wgpu::SharedTextureMemoryEndAccessState& endState, VkSemaphore semaphore,
                            bool* knownComplete) {
        *knownComplete = false;
        if (!supportsExternalSemaphoreFd_ || endState.fenceCount == 0 || endState.fences == nullptr) {
            return false;
        }
        wgpu::SharedFenceExportInfo typeQuery{};
        endState.fences[0].ExportInfo(&typeQuery);

        int dawnFd = -1;
        VkExternalSemaphoreHandleTypeFlagBits vkHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        if (typeQuery.type == wgpu::SharedFenceType::SyncFD) {
            wgpu::SharedFenceSyncFDExportInfo info{};
            wgpu::SharedFenceExportInfo exportInfo{};
            exportInfo.nextInChain = &info;
            endState.fences[0].ExportInfo(&exportInfo);
            dawnFd = info.handle;
            vkHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            if (dawnFd < 0) {
                *knownComplete = true;
                return false;
            }
        } else if (typeQuery.type == wgpu::SharedFenceType::VkSemaphoreOpaqueFD) {
            wgpu::SharedFenceVkSemaphoreOpaqueFDExportInfo info{};
            wgpu::SharedFenceExportInfo exportInfo{};
            exportInfo.nextInChain = &info;
            endState.fences[0].ExportInfo(&exportInfo);
            dawnFd = info.handle;
            vkHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        } else {
            static bool s_loggedUnexpectedType = false;
            if (!s_loggedUnexpectedType) {
                s_loggedUnexpectedType = true;
                char buf[160];
                duskVrSnprintf(buf, sizeof(buf),
                               "[dusk::vr] importDawnEndFence: unexpected SharedFenceType=%d -- using the "
                               "CPU-wait fallback every frame\n",
                               static_cast<int>(typeQuery.type));
                duskVrLog(buf);
            }
            return false;
        }
        if (dawnFd < 0) {
            return false;
        }

        // dup(): Dawn keeps ownership of (and will close) the fd it handed
        // us; Vulkan takes ownership of the fd it imports. Giving Vulkan its
        // own copy is the only way both are right.
        const int importFd = dup(dawnFd);
        if (importFd < 0) {
            return false;
        }
        static PFN_vkImportSemaphoreFdKHR pfnImportSemaphoreFdKHR = nullptr;
        if (!pfnImportSemaphoreFdKHR) {
            // Not directly linkable through Android's Vulkan loader
            // (confirmed: "undefined symbol" at link time) -- resolve via
            // vkGetDeviceProcAddr like any non-core extension entry point.
            pfnImportSemaphoreFdKHR = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
                vkGetDeviceProcAddr(xrDevice_, "vkImportSemaphoreFdKHR"));
        }
        VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        importInfo.semaphore = semaphore;
        importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;  // mandatory for SyncFD, fine for OpaqueFD
        importInfo.handleType = vkHandleType;
        importInfo.fd = importFd;
        const bool ok = pfnImportSemaphoreFdKHR != nullptr &&
                        pfnImportSemaphoreFdKHR(xrDevice_, &importInfo) == VK_SUCCESS;
        if (!ok) {
            close(importFd);  // import failed: the dup is still ours
            static bool s_loggedImportFail = false;
            if (!s_loggedImportFail) {
                s_loggedImportFail = true;
                duskVrLog("[dusk::vr] importDawnEndFence: vkImportSemaphoreFdKHR unavailable/failed -- "
                          "using the CPU-wait fallback\n");
            }
        }
        return ok;
    }

    // Hands an ExportedImage back from Dawn for this frame and, if Dawn's
    // completion fence could be imported, appends the image's semaphore to
    // `waits`. Sets *needCpuWait when neither a semaphore nor a
    // known-complete answer was available.
    void endDawnAccess(ExportedImage& img, VkSemaphore* waits, uint32_t* waitCount, bool* needCpuWait) {
        wgpu::SharedTextureMemoryVkImageLayoutEndState vkLayoutEnd{};
        wgpu::SharedTextureMemoryEndAccessState endState{};
        endState.nextInChain = &vkLayoutEnd;
        img.memory.EndAccess(img.texture, &endState);
        img.dawnEndLayout = static_cast<VkImageLayout>(vkLayoutEnd.newLayout);
        bool knownComplete = false;
        if (importDawnEndFence(endState, img.dawnDoneSemaphore, &knownComplete)) {
            waits[(*waitCount)++] = img.dawnDoneSemaphore;
        } else if (!knownComplete) {
            *needCpuWait = true;
        }
    }

    // Call ONCE per frame from submitFrame() (after aurora::gfx::synchronize()
    // has confirmed the render worker submitted this frame's Dawn work),
    // in place of the per-eye readbackEyeCopy() loop, when usesSharedImageGpuDirect()
    // is true. width/height are the full double-wide dimensions, same as
    // beginSwapchainAccessForFrame() got this frame. Non-blocking in the
    // normal case -- see the block comment at the top of this section.
    // Space warp (see the SPACE WARP section below): when this frame also
    // produced motion vectors + depth, their images ride the SAME command
    // buffer/submit/fence as the color image -- one slot, one fence, one
    // set of semaphore waits.
    void finishSharedImageGpuCopy(uint32_t swapchainIndex, uint32_t width, uint32_t height) {
        if (!sharedFrameSlotValid_) {
            return; // beginSwapchainAccessForFrame() didn't open a slot this frame
        }
        sharedFrameSlotValid_ = false;
        SharedImageSlot& s = sharedSlots_[sharedFrameSlot_];
        if (!s.texture) {
            spaceWarpAbandonFrame();
            return;
        }

        // --- Hand the textures back from Dawn. The chained EndState tells
        // us which layout Dawn actually left each image in (fed into the
        // XR-side acquire barriers below), and endState.fences carries the
        // GPU-side signal for "Dawn's writes are complete" -- the real async
        // handoff, imported as VkSemaphores the copy submit waits on.
        VkSemaphore waits[3];
        uint32_t waitCount = 0;
        bool needCpuWait = false;
        {
            // The color slot predates ExportedImage; adapt its fields in place.
            ExportedImage color;
            color.memory = s.memory;
            color.texture = s.texture;
            color.dawnDoneSemaphore = s.dawnDoneSemaphore;
            endDawnAccess(color, waits, &waitCount, &needCpuWait);
            s.dawnEndLayout = color.dawnEndLayout;
        }
        const bool spaceWarpThisFrame = swAccessOpen_ && swEncoded_;
        if (swAccessOpen_) {
            SpaceWarpSlot& sw = swSlots_[sharedFrameSlot_];
            // EndAccess is owed whether or not the frame encoded anything
            // into them (BeginAccess without EndAccess would leak the slot).
            endDawnAccess(sw.mv, waits, &waitCount, &needCpuWait);
            endDawnAccess(sw.depth, waits, &waitCount, &needCpuWait);
            swAccessOpen_ = false;
        }
        if (needCpuWait) {
            // Fallback only: some image had no usable fence this frame.
            // Block until Dawn's queue is idle so the copies below can't
            // read a half-written image. Correct, but this IS a full
            // CPU-on-GPU stall -- if the perf log shows it happening every
            // frame, the log lines from importDawnEndFence() say why the
            // fast path wasn't taken.
            bool workDone = false;
            const auto future = aurora::webgpu::g_queue.OnSubmittedWorkDone(
                wgpu::CallbackMode::WaitAnyOnly,
                [&workDone](wgpu::QueueWorkDoneStatus /*status*/, wgpu::StringView /*message*/) {
                    workDone = true;
                });
            aurora::webgpu::g_instance.WaitAny(future, 5000000000);
            if (!workDone) {
                duskVrLog("[dusk::vr] finishSharedImageGpuCopy: OnSubmittedWorkDone timed out -- skipping this frame's copy\n");
                spaceWarpAbandonFrame();
                return;
            }
        }


        // --- Record and submit the XR-side copy. cmdBuf is free: this slot's
        // previous submit was waited on in beginSwapchainAccessForFrame().
        vkResetCommandBuffer(s.cmdBuf, 0);
        VkCommandBufferBeginInfo cbBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cmdBuf, &cbBegin);

        VkImage dstImage = swapchainImages_[swapchainIndex].image;

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.levelCount = 1;
        colorRange.layerCount = 1;

        // Source acquire: from whatever layout Dawn reported leaving it in
        // (preserving its writes -- an UNDEFINED oldLayout would let the
        // driver discard them) to TRANSFER_SRC. Destination: the runtime's
        // swapchain image, UNDEFINED -> TRANSFER_DST (we overwrite all of
        // it), then -> COLOR_ATTACHMENT_OPTIMAL for the compositor, same
        // final layout the CPU-readback path has always used here.
        VkImageMemoryBarrier srcToTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        srcToTransfer.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        srcToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcToTransfer.oldLayout = s.dawnEndLayout;
        srcToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcToTransfer.image = s.xrImage;
        srcToTransfer.subresourceRange = colorRange;

        VkImageMemoryBarrier dstToTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        dstToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstToTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dstToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstToTransfer.image = dstImage;
        dstToTransfer.subresourceRange = colorRange;

        VkImageMemoryBarrier toTransferBarriers[] = {srcToTransfer, dstToTransfer};
        vkCmdPipelineBarrier(s.cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, toTransferBarriers);

        // vkCmdBlitImage, NOT vkCmdCopyImage. Measured on a Quest 3 (Adreno,
        // 2026-09-19) with per-call timing: vkCmdCopyImage into the runtime's
        // gralloc-backed swapchain image cost ~7ms of CPU time just to RECORD
        // (independent of source layout or format), while a same-extent
        // nearest blit records in ~11us. A blit format-converts, which is
        // exactly why the source image is created with the swapchain's OWN
        // format (see ensureSharedImageResources()): identical formats make
        // the blit a byte-exact identity, including for sRGB (decode on read,
        // encode on write cancel).
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        vkCmdBlitImage(s.cmdBuf, s.xrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

        VkImageMemoryBarrier dstToPresent = dstToTransfer;
        dstToPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstToPresent.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dstToPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstToPresent.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(s.cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &dstToPresent);

        if (spaceWarpThisFrame) {
            recordSpaceWarpCopies(s.cmdBuf, swSlots_[sharedFrameSlot_]);
            swSubmitted_ = true;
        }

        vkEndCommandBuffer(s.cmdBuf);

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &s.cmdBuf;
        const VkPipelineStageFlags waitStages[3] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                    VK_PIPELINE_STAGE_TRANSFER_BIT};
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores = waits;
        submitInfo.pWaitDstStageMask = waitStages;
        // No wait here (see the section comment): the fence is checked only
        // when this slot comes around again. xrReleaseSwapchainImage() right
        // after this is fine with in-flight work -- the OpenXR Vulkan
        // binding requires the app to have SUBMITTED its work to the queue
        // before release, and makes the runtime responsible for GPU-side
        // synchronization from there (same contract the D3D12 branch's
        // non-blocking copies already rely on).
        vkQueueSubmit(xrQueue_, 1, &submitInfo, s.copyFence);
        s.copyPending = true;

        sharedNextSlot_ = (sharedFrameSlot_ + 1) % kSharedSlotCount;
        swEncoded_ = false;
    }

    // -----------------------------------------------------------------------
    // SPACE WARP (XR_FB_space_warp / "AppSW", 2026-09-20) -- Quest/Vulkan only.
    //
    // The app hands the runtime, per eye, a motion-vector image and a depth
    // image alongside the color image (chained onto each projection view via
    // XrCompositionLayerSpaceWarpInfoFB); the runtime synthesizes every other
    // display frame from them and paces the app at half rate. Per the spec
    // and Meta's own XrSpaceWarp sample, the app keeps submitting every
    // frame it renders and just chains the info -- the runtime does the
    // halving -- so this is live-toggleable and needs no frame skipping here.
    //
    // Motion vectors are CAMERA-ONLY: the GX stream has no stable per-draw
    // identity to carry a previous-frame transform through, so each pixel is
    // reprojected from this frame's depth through P_prev * V_prev * V_cur^-1
    // (per eye) and the vector is CurrNDC - PrevNDC as the spec defines it.
    // World geometry, HUD/menu billboards and anything else that only moves
    // because the camera moved is exact; self-moving things (Link, NPCs,
    // enemies, projectiles) get the camera's motion only and can ghost on
    // the synthesized frames -- the known, documented artifact class.
    //
    // Data flow per frame:
    //   tick():        spaceWarpAcquireFrame()   -- acquire MV + depth swapchain images
    //                  spaceWarpBeginAccess()    -- BeginAccess this slot's MV/depth ExportedImages
    //                  (eye pass, resolved with .depth = true)
    //                  spaceWarpEncode()         -- push the MV pass (encoder task, render worker)
    //   submitFrame(): finishSharedImageGpuCopy() -- EndAccess + XR-side copies, same submit as color
    //                  spaceWarpReleaseFrame()   -- release the two images
    //                  spaceWarpLayerInfo()      -- fill the per-eye chained structs
    //
    // Depth goes through a buffer hop on the XR device (R32 color image ->
    // staging buffer -> depth-format swapchain image): Vulkan forbids
    // image-to-image copies/blits between a color and a depth format, but
    // buffer<->image copies of the depth aspect are byte-identical to the
    // matching color format (D32_SFLOAT <-> R32_SFLOAT, D24S8's depth aspect
    // <-> a u32 with D24 in the low bits, D16 <-> R16). The MV pass writes
    // FORWARD depth (1 - reversedZ) so the runtime gets the standard
    // near/far convention and nearZ/farZ can be passed unswapped.
    // -----------------------------------------------------------------------

    struct SpaceWarpSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkanKHR> images;
        int64_t format = 0;  // VkFormat
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t acquiredIndex = 0;
        bool acquired = false;
    };
    struct SpaceWarpSlot {
        ExportedImage mv;     // RGBA16F, double-wide at the MV resolution
        ExportedImage depth;  // R32Float / R32Uint / R16Uint (see depthColorFormat_), same size
        VkBuffer depthStaging = VK_NULL_HANDLE;
        VkDeviceMemory depthStagingMemory = VK_NULL_HANDLE;
        wgpu::Buffer uniform;
        // Side table for the encoder task (wgpu handles can't ride the POD
        // payload): this frame's depth snapshot + uniform contents.
        wgpu::TextureView depthSnapshot;
        SpaceWarpUniforms uniforms{};
        bool ready = false;
    };
    struct SpaceWarpTaskPayload {
        uint32_t slot;
    };

    void setSpaceWarpSupport(bool supported, uint32_t recommendedMvWidth, uint32_t recommendedMvHeight) {
        swSupported_ = supported;
        swMvWidth_ = recommendedMvWidth;
        swMvHeight_ = recommendedMvHeight;
    }
    // True when the extension is enabled AND the shared-image hand-off this
    // rides on is active AND nothing has failed for the session.
    bool spaceWarpAvailable() const {
        return swSupported_ && swMvWidth_ > 0 && swMvHeight_ > 0 && usesSharedImageGpuDirect() && !swFailed_;
    }
    uint32_t spaceWarpMvWidth() const { return swMvWidth_; }
    uint32_t spaceWarpMvHeight() const { return swMvHeight_; }

    // Lazily creates the two swapchains (first call), then acquires + waits
    // on this frame's image of each. Call right after the color swapchain's
    // own acquire/wait in tick(). Returns false (and disables space warp for
    // the session) on any failure.
    bool spaceWarpAcquireFrame() {
        if (!spaceWarpAvailable()) {
            return false;
        }
        if (!ensureSpaceWarpSwapchains()) {
            return false;
        }
        for (SpaceWarpSwapchain* sc : {&swMvSwapchain_, &swDepthSwapchain_}) {
            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(xrAcquireSwapchainImage(sc->handle, &acquireInfo, &sc->acquiredIndex))) {
                spaceWarpFail("xrAcquireSwapchainImage failed");
                spaceWarpReleaseFrame();
                return false;
            }
            sc->acquired = true;
            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(xrWaitSwapchainImage(sc->handle, &waitInfo))) {
                spaceWarpFail("xrWaitSwapchainImage failed");
                spaceWarpReleaseFrame();
                return false;
            }
        }
        return true;
    }

    // Call right after beginSwapchainAccessForFrame() (needs the frame's
    // slot). Creates the slot's MV/depth images on first use and opens Dawn
    // access to them for this frame.
    void spaceWarpBeginAccess() {
        if (!swMvSwapchain_.acquired || !sharedFrameSlotValid_) {
            return;
        }
        SpaceWarpSlot& sw = swSlots_[sharedFrameSlot_];
        if (!sw.ready && !ensureSpaceWarpSlot(sw)) {
            spaceWarpReleaseFrame();
            return;
        }
        wgpu::SharedTextureMemoryVkImageLayoutBeginState vkLayoutBegin{};
        vkLayoutBegin.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkLayoutBegin.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        wgpu::SharedTextureMemoryBeginAccessDescriptor beginDesc{};
        beginDesc.nextInChain = &vkLayoutBegin;
        beginDesc.initialized = false;  // fully overwritten by the MV pass
        sw.mv.memory.BeginAccess(sw.mv.texture, &beginDesc);
        sw.depth.memory.BeginAccess(sw.depth.texture, &beginDesc);
        swAccessOpen_ = true;
    }

    // Call once per frame after the eye pass resolved (with .depth = true),
    // while the EFB pass is active (push_encoder_task's requirement).
    // depthSnapshot: ResolvedTargets::depth (R32Float, double-wide, valid
    // this frame). Returns false if space warp isn't producing data this
    // frame (no chained info will be submitted for it).
    bool spaceWarpEncode(const wgpu::TextureView& depthSnapshot, const SpaceWarpUniforms& uniforms) {
        if (!swAccessOpen_ || !depthSnapshot) {
            return false;
        }
        SpaceWarpSlot& sw = swSlots_[sharedFrameSlot_];
        sw.depthSnapshot = depthSnapshot;
        sw.uniforms = uniforms;
        const SpaceWarpTaskPayload payload{sharedFrameSlot_};
        static_assert(sizeof(SpaceWarpTaskPayload) <= aurora::gfx::InlineDrawPayloadSize,
                      "SpaceWarpTaskPayload too large for inline encoder task payload");
        if (!aurora::gfx::push_encoder_task(swTaskId_, &payload, sizeof(payload))) {
            return false;
        }
        swEncoded_ = true;
        return true;
    }

    // Releases this frame's MV/depth swapchain images (after
    // finishSharedImageGpuCopy() submitted the copies into them). Safe to
    // call every frame; no-op when nothing was acquired.
    void spaceWarpReleaseFrame() {
        for (SpaceWarpSwapchain* sc : {&swMvSwapchain_, &swDepthSwapchain_}) {
            if (sc->acquired) {
                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(sc->handle, &releaseInfo);
                sc->acquired = false;
            }
        }
    }

    // True when this frame's submit carried real MV + depth for the layer;
    // fills the struct to chain onto projection view `eye`. Only the
    // sub-image fields are set here -- the caller owns nearZ/farZ/
    // appSpaceDeltaPose (game-side quantities).
    bool spaceWarpLayerInfo(uint32_t eye, XrCompositionLayerSpaceWarpInfoFB* out) const {
        if (!swSubmitted_) {
            return false;
        }
        *out = XrCompositionLayerSpaceWarpInfoFB{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
        out->layerFlags = 0;
        out->motionVectorSubImage.swapchain = swMvSwapchain_.handle;
        out->motionVectorSubImage.imageArrayIndex = 0;
        out->motionVectorSubImage.imageRect.offset = {static_cast<int32_t>(eye * swMvWidth_), 0};
        out->motionVectorSubImage.imageRect.extent = {static_cast<int32_t>(swMvWidth_),
                                                      static_cast<int32_t>(swMvHeight_)};
        out->depthSubImage = out->motionVectorSubImage;
        out->depthSubImage.swapchain = swDepthSwapchain_.handle;
        out->minDepth = 0.f;
        out->maxDepth = 1.f;
        return true;
    }
    // Call at the top of every tick(): the layer info is only valid for
    // the frame that produced it, and a frame that never reached
    // finishSharedImageGpuCopy() (no valid eye) still owes Dawn its
    // EndAccess before the slot can be reopened.
    void spaceWarpNewFrame() {
        swSubmitted_ = false;
        if (swAccessOpen_) {
            spaceWarpAbandonFrame();
        }
        spaceWarpReleaseFrame();
    }

private:
    // Used when a frame can't complete: drops Dawn access without copies
    // (the acquired images get released by the caller's spaceWarpReleaseFrame()).
    void spaceWarpAbandonFrame() {
        if (swAccessOpen_) {
            SpaceWarpSlot& sw = swSlots_[sharedFrameSlot_];
            wgpu::SharedTextureMemoryEndAccessState endState{};
            sw.mv.memory.EndAccess(sw.mv.texture, &endState);
            wgpu::SharedTextureMemoryEndAccessState endState2{};
            sw.depth.memory.EndAccess(sw.depth.texture, &endState2);
            swAccessOpen_ = false;
        }
        swEncoded_ = false;
    }

    void spaceWarpFail(const char* what) {
        char buf[320];
        duskVrSnprintf(buf, sizeof(buf), "[dusk::vr] space warp: %s -- disabling space warp for this session\n", what);
        duskVrLog(buf);
        swFailed_ = true;
    }

    // Depth swapchain format -> the color format the MV pass renders depth
    // as (byte-identical to the depth aspect for buffer copies) and the
    // WGSL that produces it.
    struct DepthFormatChoice {
        VkFormat depthFormat;
        VkFormat colorFormat;
        wgpu::TextureFormat dawnFormat;
        uint32_t bytesPerTexel;
        const char* wgslOutType;
        const char* wgslOutExpr;
        VkImageAspectFlags aspects;  // for the swapchain image's layout transitions
    };
    static bool depthFormatChoice(int64_t vkFormat, DepthFormatChoice* out) {
        switch (vkFormat) {
            case VK_FORMAT_D32_SFLOAT:
                *out = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_R32_SFLOAT, wgpu::TextureFormat::R32Float, 4, "vec4f",
                        "vec4f(dOut, 0.0, 0.0, 1.0)", VK_IMAGE_ASPECT_DEPTH_BIT};
                return true;
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                *out = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R32_SFLOAT, wgpu::TextureFormat::R32Float, 4, "vec4f",
                        "vec4f(dOut, 0.0, 0.0, 1.0)", VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
                return true;
            case VK_FORMAT_D24_UNORM_S8_UINT:
                *out = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_R32_UINT, wgpu::TextureFormat::R32Uint, 4, "vec4u",
                        "vec4u(u32(dOut * 16777215.0 + 0.5), 0u, 0u, 1u)",
                        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
                return true;
            case VK_FORMAT_D16_UNORM:
                *out = {VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UINT, wgpu::TextureFormat::R16Uint, 2, "vec4u",
                        "vec4u(u32(dOut * 65535.0 + 0.5), 0u, 0u, 1u)", VK_IMAGE_ASPECT_DEPTH_BIT};
                return true;
            default:
                return false;
        }
    }

    bool ensureSpaceWarpSwapchains() {
        if (swMvSwapchain_.handle != XR_NULL_HANDLE) {
            return true;
        }
        const std::vector<int64_t> supported = enumerateSwapchainFormats();
        auto has = [&](int64_t f) { return std::find(supported.begin(), supported.end(), f) != supported.end(); };
        if (!has(VK_FORMAT_R16G16B16A16_SFLOAT)) {
            spaceWarpFail("runtime offers no R16G16B16A16_SFLOAT swapchain format for motion vectors");
            return false;
        }
        const int64_t depthPrefs[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM,
                                      VK_FORMAT_D32_SFLOAT_S8_UINT};
        int64_t depthFormat = 0;
        for (int64_t f : depthPrefs) {
            if (has(f)) {
                depthFormat = f;
                break;
            }
        }
        if (depthFormat == 0 || !depthFormatChoice(depthFormat, &swDepthChoice_)) {
            spaceWarpFail("runtime offers no usable depth swapchain format");
            return false;
        }

        const uint32_t width = swMvWidth_ * 2;  // double-wide, same layout as the color swapchain
        const uint32_t height = swMvHeight_;
        auto create = [&](SpaceWarpSwapchain& sc, int64_t format, XrSwapchainUsageFlags usage) {
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = usage;
            ci.format = format;
            ci.width = width;
            ci.height = height;
            ci.sampleCount = 1;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            if (XR_FAILED(xrCreateSwapchain(session_, &ci, &sc.handle))) {
                sc.handle = XR_NULL_HANDLE;
                return false;
            }
            uint32_t imageCount = 0;
            xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr);
            sc.images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(sc.handle, imageCount, &imageCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
            sc.format = format;
            sc.width = width;
            sc.height = height;
            return true;
        };
        // TRANSFER_DST on both: they're filled with transfer commands, and
        // on Vulkan a copy into an image created without that usage is
        // undefined (same reasoning as the color swapchain's flag).
        if (!create(swMvSwapchain_, VK_FORMAT_R16G16B16A16_SFLOAT,
                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
            spaceWarpFail("xrCreateSwapchain (motion vectors) failed");
            return false;
        }
        if (!create(swDepthSwapchain_, depthFormat,
                    XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
            spaceWarpFail("xrCreateSwapchain (depth) failed");
            return false;
        }
        char buf[200];
        duskVrSnprintf(buf, sizeof(buf),
                       "[dusk::vr] space warp: swapchains created, %ux%u per eye, mv=R16G16B16A16_SFLOAT depth=vkFormat %lld\n",
                       swMvWidth_, swMvHeight_, static_cast<long long>(depthFormat));
        duskVrLog(buf);
        return true;
    }

    bool ensureSpaceWarpSlot(SpaceWarpSlot& sw) {
        if (!ensureSpaceWarpPipeline()) {
            return false;
        }
        const uint32_t width = swMvWidth_ * 2;
        const uint32_t height = swMvHeight_;
        char err[512];
        {
            ExportedImageDesc d{};
            d.width = width;
            d.height = height;
            d.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
            // TRANSFER_DST: Dawn refuses to import an opaque-fd image without
            // it ("vkImageCreateInfo.usage did not have
            // VK_IMAGE_USAGE_TRANSFER_DST_BIT", real Quest 3 log 2026-09-20)
            // even though nothing here copies into these images.
            d.vkUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            d.dawnFormat = wgpu::TextureFormat::RGBA16Float;
            d.dawnUsage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            if (!createExportedImage(d, sw.mv, err, sizeof(err))) {
                spaceWarpFail(err);
                return false;
            }
        }
        {
            ExportedImageDesc d{};
            d.width = width;
            d.height = height;
            d.vkFormat = swDepthChoice_.colorFormat;
            // TRANSFER_DST: Dawn refuses to import an opaque-fd image without
            // it ("vkImageCreateInfo.usage did not have
            // VK_IMAGE_USAGE_TRANSFER_DST_BIT", real Quest 3 log 2026-09-20)
            // even though nothing here copies into these images.
            d.vkUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            d.dawnFormat = swDepthChoice_.dawnFormat;
            d.dawnUsage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
            if (!createExportedImage(d, sw.depth, err, sizeof(err))) {
                spaceWarpFail(err);
                return false;
            }
        }
        // Device-local staging buffer for the color->depth hop.
        {
            VkBufferCreateInfo bufCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufCI.size = static_cast<VkDeviceSize>(width) * height * swDepthChoice_.bytesPerTexel;
            bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(xrDevice_, &bufCI, nullptr, &sw.depthStaging) != VK_SUCCESS) {
                spaceWarpFail("vkCreateBuffer (depth staging) failed");
                return false;
            }
            VkMemoryRequirements memReq{};
            vkGetBufferMemoryRequirements(xrDevice_, sw.depthStaging, &memReq);
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(xrPhysicalDevice_, &memProps);
            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if ((memReq.memoryTypeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }
            if (memTypeIndex == UINT32_MAX) {
                for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                    if (memReq.memoryTypeBits & (1u << i)) {
                        memTypeIndex = i;
                        break;
                    }
                }
            }
            if (memTypeIndex == UINT32_MAX) {
                spaceWarpFail("no memory type for the depth staging buffer");
                return false;
            }
            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex = memTypeIndex;
            if (vkAllocateMemory(xrDevice_, &allocInfo, nullptr, &sw.depthStagingMemory) != VK_SUCCESS ||
                vkBindBufferMemory(xrDevice_, sw.depthStaging, sw.depthStagingMemory, 0) != VK_SUCCESS) {
                spaceWarpFail("vkAllocateMemory/vkBindBufferMemory (depth staging) failed");
                return false;
            }
        }
        wgpu::BufferDescriptor uniformDesc{};
        uniformDesc.size = sizeof(SpaceWarpUniforms);
        uniformDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        sw.uniform = aurora::webgpu::g_device.CreateBuffer(&uniformDesc);
        sw.ready = true;
        duskVrLog("[dusk::vr] space warp: slot resources created (MV + depth exported images imported into Dawn)\n");
        return true;
    }

    // Full-screen reprojection pass: one fragment per MV texel, both eyes
    // (double-wide, eye picked by x). See the section comment for the math.
    static constexpr const char* kSpaceWarpShaderSource = R"WGSL(
struct SwParams {
    prevFromCurView: array<mat4x4f, 2>,
    invProj: array<vec4f, 2>,
    nearFar: vec4f,
    sizes: vec4u,
};

@group(0) @binding(0) var depthTex: texture_2d<f32>;
@group(0) @binding(1) var<uniform> params: SwParams;

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
    var positions = array<vec2f, 3>(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
    return vec4f(positions[vi], 0.0, 1.0);
}

struct FsOut {
    @location(0) mv: vec4f,
    @location(1) depth: DEPTH_OUT_TYPE,
};

@fragment fn fs_main(@builtin(position) pos: vec4f) -> FsOut {
    let px = u32(pos.x);
    let py = u32(pos.y);
    let mvW = params.sizes.x;
    let mvH = params.sizes.y;
    let dW = params.sizes.z;
    let dH = params.sizes.w;
    let eye = select(0u, 1u, px >= mvW);
    let lx = px - eye * mvW;
    let flags = u32(params.nearFar.z + 0.5);
    let fx = (f32(lx) + 0.5) / f32(mvW);
    var fy = (f32(py) + 0.5) / f32(mvH);
    if ((flags & 16u) != 0u) { fy = 1.0 - fy; }
    // Point-sample the (full-resolution) depth snapshot at this texel's center.
    let sx = min(u32(fx * f32(dW)), dW - 1u) + eye * dW;
    let sy = min(u32(fy * f32(dH)), dH - 1u);
    let dRev = clamp(textureLoad(depthTex, vec2i(i32(sx), i32(sy)), 0).r, 0.0, 1.0);
    let dFwd = 1.0 - dRev;

    // Reversed-Z perspective depth -> positive eye-space distance
    // (dRev = n(f-e) / (e(f-n)), see vr_stereo_render.hpp's projection).
    let n = params.nearFar.x;
    let f = params.nearFar.y;
    let e = n * f / (dRev * (f - n) + n);

    // NDC (x right, y up) of this texel in its own eye, then back to view
    // space through the eye's asymmetric projection:
    //   ndc.x = p0 * xv / e - p1,  ndc.y = p2 * yv / e - p3
    let ndcX = fx * 2.0 - 1.0;
    let ndcY = 1.0 - fy * 2.0;
    let ip = params.invProj[eye];
    let xv = (ndcX + ip.y) * e / ip.x;
    let yv = (ndcY + ip.w) * e / ip.z;
    let prevClip = params.prevFromCurView[eye] * vec4f(xv, yv, -e, 1.0);

    var out: FsOut;
    var mv = vec4f(0.0, 0.0, 0.0, 0.0);
    if (prevClip.w > 1e-5) {
        let prevNdc = prevClip.xyz / prevClip.w;
        // z as GL-style NDC ([-1,1], forward), the convention Meta's sample uses.
        let zCur = 2.0 * dFwd - 1.0;
        let zPrev = 2.0 * (1.0 - clamp(prevNdc.z, 0.0, 1.0)) - 1.0;
        mv = vec4f(ndcX - prevNdc.x, ndcY - prevNdc.y, zCur - zPrev, 0.0);
    }
    // TEMPORARY convention A/B flags (settings.h's vrSpaceWarpDebug*).
    if ((flags & 1u) != 0u) { mv = -mv; }
    if ((flags & 2u) != 0u) { mv.y = -mv.y; }
    if ((flags & 4u) != 0u) { mv = vec4f(0.0, 0.0, 0.0, 0.0); }
    var dOut = dFwd;
    if ((flags & 32u) != 0u) { dOut = dRev; }
    if ((flags & 64u) != 0u) {
        // TEMP raw diagnostic: what the shader actually sees.
        let dims = textureDimensions(depthTex);
        mv = vec4f(textureLoad(depthTex, vec2i(i32(sx), i32(sy)), 0).r, f32(dims.x), f32(dims.y), f32(sx));
    }
    if ((flags & 8u) != 0u) { dOut = 1.0; }
    out.mv = mv;
    out.depth = DEPTH_OUT_EXPR;
    return out;
}
)WGSL";

    bool ensureSpaceWarpPipeline() {
        if (swPipeline_) {
            return true;
        }
        std::string src = kSpaceWarpShaderSource;
        auto replaceAll = [&](const char* from, const char* to) {
            for (size_t pos = src.find(from); pos != std::string::npos; pos = src.find(from, pos + std::strlen(to))) {
                src.replace(pos, std::strlen(from), to);
            }
        };
        replaceAll("DEPTH_OUT_TYPE", swDepthChoice_.wgslOutType);
        replaceAll("DEPTH_OUT_EXPR", swDepthChoice_.wgslOutExpr);

        aurora::webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
        {
            wgpu::ShaderSourceWGSL wgslDesc{};
            wgslDesc.code = src.c_str();
            wgpu::ShaderModuleDescriptor moduleDesc{};
            moduleDesc.nextInChain = &wgslDesc;
            moduleDesc.label = "vr_space_warp";
            wgpu::ShaderModule module = aurora::webgpu::g_device.CreateShaderModule(&moduleDesc);

            wgpu::BindGroupLayoutEntry bglEntries[2] = {};
            bglEntries[0].binding = 0;
            bglEntries[0].visibility = wgpu::ShaderStage::Fragment;
            bglEntries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;  // R32Float
            bglEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            bglEntries[1].binding = 1;
            bglEntries[1].visibility = wgpu::ShaderStage::Fragment;
            bglEntries[1].buffer.type = wgpu::BufferBindingType::Uniform;
            bglEntries[1].buffer.minBindingSize = sizeof(SpaceWarpUniforms);
            wgpu::BindGroupLayoutDescriptor bglDesc{};
            bglDesc.entryCount = 2;
            bglDesc.entries = bglEntries;
            swBindGroupLayout_ = aurora::webgpu::g_device.CreateBindGroupLayout(&bglDesc);

            wgpu::PipelineLayoutDescriptor plDesc{};
            plDesc.bindGroupLayoutCount = 1;
            plDesc.bindGroupLayouts = &swBindGroupLayout_;
            wgpu::PipelineLayout pipelineLayout = aurora::webgpu::g_device.CreatePipelineLayout(&plDesc);

            wgpu::ColorTargetState targets[2] = {};
            targets[0].format = wgpu::TextureFormat::RGBA16Float;
            targets[0].writeMask = wgpu::ColorWriteMask::All;
            targets[1].format = swDepthChoice_.dawnFormat;
            targets[1].writeMask = wgpu::ColorWriteMask::All;
            wgpu::FragmentState fragment{};
            fragment.module = module;
            fragment.entryPoint = "fs_main";
            fragment.targetCount = 2;
            fragment.targets = targets;

            wgpu::RenderPipelineDescriptor pipelineDesc{};
            pipelineDesc.label = "vr_space_warp";
            pipelineDesc.layout = pipelineLayout;
            pipelineDesc.vertex.module = module;
            pipelineDesc.vertex.entryPoint = "vs_main";
            pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
            pipelineDesc.multisample.count = 1;
            pipelineDesc.fragment = &fragment;
            swPipeline_ = aurora::webgpu::g_device.CreateRenderPipeline(&pipelineDesc);
        }
        bool done = false;
        bool failed = false;
        std::string message;
        const auto future = aurora::webgpu::g_device.PopErrorScope(
            wgpu::CallbackMode::WaitAnyOnly,
            [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView msg) {
                done = true;
                if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
                    failed = true;
                    message = std::string{std::string_view{msg}};
                }
            });
        aurora::webgpu::g_instance.WaitAny(future, 5000000000);
        if (!done || failed || !swPipeline_) {
            char buf[512];
            duskVrSnprintf(buf, sizeof(buf), "Dawn rejected the motion-vector pipeline: %s",
                           done ? message.c_str() : "PopErrorScope timed out");
            swPipeline_ = nullptr;
            spaceWarpFail(buf);
            return false;
        }
        aurora::gfx::EncoderTaskDescriptor desc{
            .label = "vr_space_warp_mv",
            .callback = &Session::spaceWarpTaskCallback,
            .userdata = this,
        };
        swTaskId_ = aurora::gfx::register_encoder_task_type(desc);
        return true;
    }

    // Render worker: the MV pass itself, between two scene passes on the
    // frame's encoder (same contract as encoderTaskCallback()).
    static void spaceWarpTaskCallback(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
                                      const void* payload, size_t /*payloadSize*/, void* userdata) {
        auto* self = static_cast<Session*>(userdata);
        const auto& p = *static_cast<const SpaceWarpTaskPayload*>(payload);
        SpaceWarpSlot& sw = self->swSlots_[p.slot];
        if (!sw.ready || !sw.depthSnapshot) {
            return;
        }
        wgpu::CommandEncoder mutableCmd = cmd;
        const aurora::webgpu::gpu_prof::Zone gpuZone{cmd, "VR space warp MV"};

        ctx.queue.WriteBuffer(sw.uniform, 0, &sw.uniforms, sizeof(sw.uniforms));

        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].textureView = sw.depthSnapshot;
        entries[1].binding = 1;
        entries[1].buffer = sw.uniform;
        entries[1].size = sizeof(SpaceWarpUniforms);
        wgpu::BindGroupDescriptor bgDesc{};
        bgDesc.layout = self->swBindGroupLayout_;
        bgDesc.entryCount = 2;
        bgDesc.entries = entries;
        wgpu::BindGroup bindGroup = aurora::webgpu::g_device.CreateBindGroup(&bgDesc);

        wgpu::RenderPassColorAttachment colorAttachments[2] = {};
        colorAttachments[0].view = sw.mv.view;
        colorAttachments[0].loadOp = wgpu::LoadOp::Clear;
        colorAttachments[0].storeOp = wgpu::StoreOp::Store;
        colorAttachments[0].clearValue = {0.0, 0.0, 0.0, 0.0};
        colorAttachments[1].view = sw.depth.view;
        colorAttachments[1].loadOp = wgpu::LoadOp::Clear;
        colorAttachments[1].storeOp = wgpu::StoreOp::Store;
        colorAttachments[1].clearValue = {0.0, 0.0, 0.0, 0.0};
        wgpu::RenderPassDescriptor passDesc{};
        passDesc.label = "VR space warp MV";
        passDesc.colorAttachmentCount = 2;
        passDesc.colorAttachments = colorAttachments;
        wgpu::RenderPassEncoder pass = mutableCmd.BeginRenderPass(&passDesc);
        pass.SetPipeline(self->swPipeline_);
        pass.SetBindGroup(0, bindGroup);
        pass.Draw(3);
        pass.End();
        sw.depthSnapshot = nullptr;  // pooled per-frame view; don't keep it alive past the frame
    }

    // XR-side: MV blit + depth buffer hop into this frame's swapchain
    // images, appended to the color copy's command buffer.
    void recordSpaceWarpCopies(VkCommandBuffer cmdBuf, SpaceWarpSlot& sw) {
        const uint32_t width = swMvWidth_ * 2;
        const uint32_t height = swMvHeight_;
        VkImage mvDst = swMvSwapchain_.images[swMvSwapchain_.acquiredIndex].image;
        VkImage depthDst = swDepthSwapchain_.images[swDepthSwapchain_.acquiredIndex].image;

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.levelCount = 1;
        colorRange.layerCount = 1;
        VkImageSubresourceRange depthRange = colorRange;
        depthRange.aspectMask = swDepthChoice_.aspects;

        auto barrier = [](VkImage image, VkImageSubresourceRange range, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = range;
            return b;
        };
        const VkImageMemoryBarrier toTransfer[4] = {
            barrier(sw.mv.xrImage, colorRange, sw.mv.dawnEndLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            barrier(sw.depth.xrImage, colorRange, sw.depth.dawnEndLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            barrier(mvDst, colorRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT),
            barrier(depthDst, depthRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 4, toTransfer);

        // Motion vectors: identical formats -> exact blit (see the color copy).
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        vkCmdBlitImage(cmdBuf, sw.mv.xrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mvDst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

        // Depth: color image -> staging buffer -> depth aspect of the swapchain image.
        VkBufferImageCopy toBuf{};
        toBuf.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toBuf.imageSubresource.layerCount = 1;
        toBuf.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmdBuf, sw.depth.xrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sw.depthStaging, 1,
                               &toBuf);
        VkBufferMemoryBarrier bufBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bufBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bufBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBarrier.buffer = sw.depthStaging;
        bufBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &bufBarrier, 0, nullptr);
        VkBufferImageCopy toImg{};
        toImg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        toImg.imageSubresource.layerCount = 1;
        toImg.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmdBuf, sw.depthStaging, depthDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &toImg);

        // Final layouts the runtime expects at release (XR_KHR_vulkan_enable2):
        // COLOR_ATTACHMENT_OPTIMAL for color, DEPTH_STENCIL_ATTACHMENT_OPTIMAL for depth.
        const VkImageMemoryBarrier toFinal[2] = {
            barrier(mvDst, colorRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
            barrier(depthDst, depthRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
        };
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 2, toFinal);

        // TEMP DIAGNOSTIC (2026-09-20 convention hunt): copy this frame's
        // MV image + depth bytes to a host-visible buffer; logged once the
        // slot's fence is waited on (spaceWarpDebugReadbackLog()).
        spaceWarpDebugRecordReadback(cmdBuf, sw, slotIndexOf(sw));
    }

    // ---- TEMP DIAGNOSTIC: readback of a few MV/depth texels ----
public:
    void spaceWarpDebugRearmReadback() { swDebugFramesLogged_ = 0; }
private:
    uint32_t slotIndexOf(const SpaceWarpSlot& sw) const {
        return static_cast<uint32_t>(&sw - &swSlots_[0]);
    }
    static float halfToFloat(uint16_t h) {
        const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
        float v;
        if (exp == 0) {
            v = std::ldexp(static_cast<float>(man), -24);
        } else if (exp == 31) {
            v = man ? NAN : INFINITY;
        } else {
            v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
        }
        return sign ? -v : v;
    }
    void spaceWarpDebugRecordReadback(VkCommandBuffer cmdBuf, SpaceWarpSlot& sw, uint32_t slot) {
        if (swDebugFramesLogged_ >= 6 || swDebugPendingSlot_ != UINT32_MAX) {
            return;
        }
        const uint32_t width = swMvWidth_ * 2;
        const uint32_t height = swMvHeight_;
        const VkDeviceSize mvBytes = static_cast<VkDeviceSize>(width) * height * 8;
        const VkDeviceSize depthBytes = static_cast<VkDeviceSize>(width) * height * swDepthChoice_.bytesPerTexel;
        if (swDebugReadback_ == VK_NULL_HANDLE) {
            VkBufferCreateInfo bufCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufCI.size = mvBytes + depthBytes;
            bufCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(xrDevice_, &bufCI, nullptr, &swDebugReadback_) != VK_SUCCESS) {
                swDebugFramesLogged_ = 99;
                return;
            }
            VkMemoryRequirements memReq{};
            vkGetBufferMemoryRequirements(xrDevice_, swDebugReadback_, &memReq);
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(xrPhysicalDevice_, &memProps);
            uint32_t memTypeIndex = UINT32_MAX;
            const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if ((memReq.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & required) == required) {
                    memTypeIndex = i;
                    break;
                }
            }
            if (memTypeIndex == UINT32_MAX) {
                swDebugFramesLogged_ = 99;
                return;
            }
            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex = memTypeIndex;
            vkAllocateMemory(xrDevice_, &allocInfo, nullptr, &swDebugReadbackMemory_);
            vkBindBufferMemory(xrDevice_, swDebugReadback_, swDebugReadbackMemory_, 0);
        }
        // MV image is in TRANSFER_SRC_OPTIMAL here (barrier above); depth
        // staging buffer already holds this frame's depth bytes.
        VkBufferImageCopy toBuf{};
        toBuf.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toBuf.imageSubresource.layerCount = 1;
        toBuf.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmdBuf, sw.mv.xrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swDebugReadback_, 1, &toBuf);
        VkBufferCopy region{};
        region.dstOffset = mvBytes;
        region.size = depthBytes;
        vkCmdCopyBuffer(cmdBuf, sw.depthStaging, swDebugReadback_, 1, &region);
        VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer = swDebugReadback_;
        hostBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &hostBarrier, 0, nullptr);
        swDebugPendingSlot_ = slot;
    }
    void spaceWarpDebugReadbackLog(uint32_t slot) {
        if (swDebugPendingSlot_ != slot) {
            return;
        }
        swDebugPendingSlot_ = UINT32_MAX;
        ++swDebugFramesLogged_;
        const uint32_t width = swMvWidth_ * 2;
        const uint32_t height = swMvHeight_;
        const size_t mvBytes = static_cast<size_t>(width) * height * 8;
        void* mapped = nullptr;
        if (vkMapMemory(xrDevice_, swDebugReadbackMemory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            return;
        }
        const auto* mv = static_cast<const uint16_t*>(mapped);
        const auto* depthBytes = static_cast<const uint8_t*>(mapped) + mvBytes;
        auto depthAt = [&](uint32_t x, uint32_t y) -> float {
            const size_t i = static_cast<size_t>(y) * width + x;
            switch (swDepthChoice_.bytesPerTexel) {
                case 4:
                    if (swDepthChoice_.dawnFormat == wgpu::TextureFormat::R32Float) {
                        float f;
                        std::memcpy(&f, depthBytes + i * 4, 4);
                        return f;
                    } else {
                        uint32_t u;
                        std::memcpy(&u, depthBytes + i * 4, 4);
                        return static_cast<float>(u & 0xFFFFFFu) / 16777215.f;
                    }
                default: {
                    uint16_t u;
                    std::memcpy(&u, depthBytes + i * 2, 2);
                    return static_cast<float>(u) / 65535.f;
                }
            }
        };
        const uint32_t w = swMvWidth_, h = swMvHeight_;
        const struct { const char* name; uint32_t x, y; } pts[] = {
            {"L-center", w / 2, h / 2},   {"L-top", w / 2, h / 10},      {"L-bottom", w / 2, h - h / 10},
            {"L-left", w / 10, h / 2},    {"L-right", w - w / 10, h / 2}, {"R-center", w + w / 2, h / 2},
        };
        char msg[900];
        int off = duskVrSnprintf(msg, sizeof(msg), "[dusk::vr] space warp READBACK #%u:", swDebugFramesLogged_);
        for (const auto& pt : pts) {
            const size_t i = (static_cast<size_t>(pt.y) * width + pt.x) * 4;
            off += duskVrSnprintf(msg + off, sizeof(msg) - off, " %s mv=(%.4f,%.4f,%.4f,%.1f) d=%.5f;", pt.name,
                                  halfToFloat(mv[i]), halfToFloat(mv[i + 1]), halfToFloat(mv[i + 2]),
                                  halfToFloat(mv[i + 3]), depthAt(pt.x, pt.y));
        }
        duskVrSnprintf(msg + off, sizeof(msg) - off, "\n");
        duskVrLog(msg);
        vkUnmapMemory(xrDevice_, swDebugReadbackMemory_);
    }

public:
#endif  // DUSK_VR_XR_GRAPHICS_VULKAN

    // Call once per eye AFTER aurora_end_frame() has returned for this
    // frame -- see the WHY THIS IS SPLIT IN TWO note above. Same
    // eyeIndex/swapchainIndex/eyeWidth/eyeHeight/dstXOffset/format as the
    // matching encodeEyeCopy() call for this eye this frame.
    //
    // FIXED this session: eyeIndex (0/1) now selects cpuCopyBuffers_ (the
    // per-eye CPU staging buffer -- see encodeEyeCopy's comment above for
    // why swapchainIndex was wrong for that). swapchainIndex is still used
    // below, unchanged, to select the actual XR swapchain image -- that
    // part IS correctly shared between both eyes (single double-wide
    // image), only offset differently via dstXOffset.
    void readbackEyeCopy(uint32_t eyeIndex, uint32_t swapchainIndex, uint32_t eyeWidth, uint32_t eyeHeight,
                          uint32_t dstXOffset, wgpu::TextureFormat format) {
        // Defensive, belt-and-suspenders (vr_main.cpp's submitFrame() is
        // also expected to skip calling this entirely when
        // usesGpuDirectSwapchainCopy() -- see that flag's comment): if
        // called anyway, res.readback was never written this frame (the
        // GPU-direct path replaced its CopyBufferToBuffer with a direct
        // CopyBufferToTexture into the swapchain image already -- see
        // encoderTaskCallback()), so mapping and re-uploading it here
        // would overwrite the swapchain image with stale/empty data,
        // corrupting the very frame the fast path just correctly wrote.
        if (usesGpuDirectSwapchainCopy()) {
            return;
        }
#if DUSK_VR_XR_GRAPHICS_VULKAN
        // Same reasoning, Vulkan shared-image GPU-direct path (see
        // usesSharedImageGpuDirect()'s comment) -- vr_main.cpp's submitFrame()
        // calls finishSharedImageGpuCopy() ONCE per frame instead of this
        // function's normal per-eye loop when this is true.
        if (usesSharedImageGpuDirect()) {
            return;
        }
#endif
        ensureCpuCopyCmdList();
        auto& res = cpuCopyBuffers_[eyeIndex];

#if !DUSK_VR_XR_GRAPHICS_VULKAN
        // DOUBLE-BUFFERED (2026-09-16 perf follow-up): pick this call's
        // upload-heap/command-list slot, lazily create it on first use,
        // then wait ONLY if this specific slot's own last-submitted copy
        // hasn't completed yet -- on first use slotFenceValue is 0 and
        // GetCompletedValue() starts at 0 too, so this is always a no-op
        // then; on reuse (2 calls later, i.e. next frame for this same
        // eye) a whole frame has elapsed, so it's almost always still a
        // no-op rather than the guaranteed stall the old single-buffered
        // version had at the END of every single call.
        const uint32_t slot = res.slotIndex;
        if (!res.slotCmdList[slot]) {
            xrDevice_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&res.slotCmdAlloc[slot]));
            xrDevice_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, res.slotCmdAlloc[slot].Get(),
                                          nullptr, IID_PPV_ARGS(&res.slotCmdList[slot]));
            res.slotCmdList[slot]->Close(); // Reset() below expects a closed list first time
        }
        if (copyFence_->GetCompletedValue() < res.slotFenceValue[slot]) {
            HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            copyFence_->SetEventOnCompletion(res.slotFenceValue[slot], event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
#endif

        // --- Map the staging buffer (written by encodeEyeCopy's task,
        // already submitted to the GPU as part of this frame's single
        // Submit() in aurora_end_frame -- safe to wait on now) and block
        // until it's ready. ---
        // Blocking per-eye stall -- correctness-first pass, matches handoff
        // step 3's scope ("nothing to adapt from"). Double-buffering this
        // is a real perf TODO once pixels are confirmed reaching the
        // headset (handoff step 4) -- do not treat the stall as done/final.
        bool mapDone = false;
        bool mapOk = false;
        const auto future = res.readback.MapAsync(
            wgpu::MapMode::Read, 0, static_cast<size_t>(res.bytesPerRow) * eyeHeight,
            wgpu::CallbackMode::WaitAnyOnly,
            [&mapDone, &mapOk](wgpu::MapAsyncStatus status, wgpu::StringView /*message*/) {
                mapDone = true;
                mapOk = (status == wgpu::MapAsyncStatus::Success);
            });
        aurora::webgpu::g_instance.WaitAny(future, 5000000000);
        if (!mapDone || !mapOk) {
            // TEMP diagnostic (this session): this path was silently
            // returning with no log at all -- if this is what's firing
            // every frame, that alone would fully explain "compositor sees
            // the app, submits nothing" with zero other evidence anywhere.
            char msg[256];
            duskVrSnprintf(msg, sizeof(msg),
                        "[dusk::vr] readbackEyeCopy: MapAsync failed/timed "
                        "out (mapDone=%d, mapOk=%d) -- skipping this eye's "
                        "swapchain copy\n",
                        mapDone, mapOk);
            duskVrLog(msg);
            return;
        }

        const uint8_t* mapped = static_cast<const uint8_t*>(
            res.readback.GetConstMappedRange(0, static_cast<size_t>(res.bytesPerRow) * eyeHeight));


        // --- Copy row-by-row into the D3D12 upload heap. ---
        // Two independent row-pitch alignments (Dawn's and D3D12's are both
        // 256 today, but don't assume they stay equal -- copy the real
        // per-row byte count (eyeWidth * 4) and respect each side's own
        // pitch, tracked separately in CpuCopyBuffers).
        const uint32_t rowBytes = eyeWidth * 4; // RGBA8/BGRA8 -- 4 bytes/texel
#if DUSK_VR_XR_GRAPHICS_VULKAN
        // Persistently mapped in ensureCpuCopyBuffers() (HOST_COHERENT --
        // no explicit Map()/flush needed, unlike the D3D12 upload heap's
        // per-call Map()/Unmap() below).
        void* uploadMapped = res.uploadMapped;
#else
        void* uploadMapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        res.uploadHeap[slot]->Map(0, &noRead, &uploadMapped);
#endif
        // swapchainPixelConversion_ (see createSwapchain()'s comment): what
        // the runtime's actually-accepted format requires we do to the
        // bytes before upload -- otherwise this would silently corrupt
        // colors (channel swap) or fail validation (wrong format) in the
        // headset. NOT consulted when useGammaComputePath_ is true: in that
        // case encoderTaskCallback()'s GPU compute pass already applied
        // gamma compensation AND any needed channel reorder before this
        // buffer was populated (see kGammaComputeShaderSource's comment) --
        // `mapped` here is already the final byte layout, so just memcpy.
        // Generalized 2026-08-16 -- this used to be gated on swapchainIsSrgb_
        // (SteamVR only); now covers every format the compute path handles
        // (see useGammaComputePath_'s own comment), which is everything
        // except the rare PackR10G10B10A2 last-resort fallback below.
        const bool srcIsBgra = format == wgpu::TextureFormat::BGRA8Unorm ||
                                format == wgpu::TextureFormat::BGRA8UnormSrgb;
        if (useGammaComputePath_) {
            for (uint32_t y = 0; y < eyeHeight; ++y) {
                std::memcpy(static_cast<uint8_t*>(uploadMapped) + y * res.uploadRowPitch,
                            mapped + y * res.bytesPerRow, rowBytes);
            }
        } else {
            switch (swapchainPixelConversion_) {
                case SwapchainPixelConversion::ChannelSwap:
                    for (uint32_t y = 0; y < eyeHeight; ++y) {
                        const uint8_t* srcRow = mapped + y * res.bytesPerRow;
                        uint8_t* dstRow = static_cast<uint8_t*>(uploadMapped) + y * res.uploadRowPitch;
                        for (uint32_t x = 0; x < eyeWidth; ++x) {
                            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
                        }
                    }
                    break;
                case SwapchainPixelConversion::PackR10G10B10A2:
                    // Repack each 8-bit-per-channel source pixel into a 10/10/10/2
                    // word -- see packR10G10B10A2Unorm's comment for why this
                    // format was chosen over an SRGB variant. Extract r/g/b/a
                    // respecting the SOURCE's real channel order (srcIsBgra),
                    // packR10G10B10A2Unorm always takes them as (r,g,b,a) and
                    // places them per the DXGI-defined bit layout regardless.
                    for (uint32_t y = 0; y < eyeHeight; ++y) {
                        const uint8_t* srcRow = mapped + y * res.bytesPerRow;
                        uint32_t* dstRow = reinterpret_cast<uint32_t*>(
                            static_cast<uint8_t*>(uploadMapped) + y * res.uploadRowPitch);
                        for (uint32_t x = 0; x < eyeWidth; ++x) {
                            const uint8_t c0 = srcRow[x * 4 + 0];
                            const uint8_t c1 = srcRow[x * 4 + 1];
                            const uint8_t c2 = srcRow[x * 4 + 2];
                            const uint8_t a = srcRow[x * 4 + 3];
                            const uint8_t r = srcIsBgra ? c2 : c0;
                            const uint8_t g = c1;
                            const uint8_t b = srcIsBgra ? c0 : c2;
                            dstRow[x] = packR10G10B10A2Unorm(r, g, b, a);
                        }
                    }
                    break;
                case SwapchainPixelConversion::None:
                default:
                    for (uint32_t y = 0; y < eyeHeight; ++y) {
                        std::memcpy(static_cast<uint8_t*>(uploadMapped) + y * res.uploadRowPitch,
                                    mapped + y * res.bytesPerRow, rowBytes);
                    }
                    break;
            }
        }
#if DUSK_VR_XR_GRAPHICS_VULKAN
        // Nothing to unmap -- HOST_COHERENT persistent mapping, see above.
#else
        D3D12_RANGE written{0, static_cast<SIZE_T>(res.uploadRowPitch) * eyeHeight};
        res.uploadHeap[slot]->Unmap(0, &written);
#endif

        res.readback.Unmap();


        // --- Record + execute the upload buffer -> swapchain image copy on
        // the XR-side device/queue, offset into the correct eye's half of
        // the double-wide destination. ---
#if DUSK_VR_XR_GRAPHICS_VULKAN
        vkResetCommandPool(xrDevice_, copyCmdPool_, 0);
        VkCommandBufferBeginInfo cbBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(copyCmdBuf_, &cbBegin);

        VkImage dstImage = swapchainImages_[swapchainIndex].image;

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.baseMipLevel = 0;
        colorRange.levelCount = 1;
        colorRange.baseArrayLayer = 0;
        colorRange.layerCount = 1;

        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // UNDEFINED as oldLayout is deliberate, not "don't know the real
        // layout" laziness -- we're about to overwrite the whole image via
        // copy anyway, so discarding whatever it held is correct and is
        // the standard Vulkan idiom for a copy-destination transition (same
        // role D3D12_RESOURCE_STATE_COMMON plays as the D3D12 branch's
        // resting state below).
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = dstImage;
        toDst.subresourceRange = colorRange;
        vkCmdPipelineBarrier(copyCmdBuf_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        // bufferRowLength/bufferImageHeight are in TEXELS, not bytes; 0
        // would mean "tightly packed" (== imageExtent.width), but
        // res.uploadRowPitch is 256-byte-aligned like the D3D12 upload
        // heap, so it must be stated explicitly whenever real padding was
        // added. Assumes 4 bytes/texel -- true for every
        // SwapchainPixelConversion candidate this file produces (8bpc
        // RGBA/BGRA and the 10/10/10/2 pack are both 4-byte words).
        region.bufferRowLength = res.uploadRowPitch / 4;
        region.bufferImageHeight = eyeHeight;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        // dstXOffset lands this eye's copy in the correct half of the
        // double-wide resource; y/z stay 0 (full height, single layer).
        region.imageOffset = {static_cast<int32_t>(dstXOffset), 0, 0};
        region.imageExtent = {eyeWidth, eyeHeight, 1};
        vkCmdCopyBufferToImage(copyCmdBuf_, res.uploadBuffer, dstImage,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // UNVERIFIED which layout the runtime actually expects a Vulkan
        // color swapchain image left in at xrReleaseSwapchainImage/
        // xrEndFrame time -- OpenXR's spec doesn't pin this down as
        // tightly as D3D12's resource-state model does. COLOR_ATTACHMENT_
        // OPTIMAL matches this swapchain's own
        // XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT usage flag (set in
        // createSwapchain(), shared with the D3D12 branch) and what
        // Vulkan OpenXR samples typically leave a color swapchain image
        // in after rendering into it -- if the compositor rejects or
        // corrupts the submitted layer, try VK_IMAGE_LAYOUT_GENERAL or
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL here first, in that
        // order.
        VkImageMemoryBarrier toPresent = toDst;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(copyCmdBuf_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                              1, &toPresent);

        vkEndCommandBuffer(copyCmdBuf_);

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &copyCmdBuf_;
        vkResetFences(xrDevice_, 1, &copyFence_);
        vkQueueSubmit(xrQueue_, 1, &submitInfo, copyFence_);

        // Block until the XR-side GPU has finished the copy before this
        // function returns -- the upload buffer gets reused/remapped next
        // call, so it must not still be in flight. Same "correctness
        // first, perf TODO" note as the D3D12 branch and the MapAsync wait
        // above.
        vkWaitForFences(xrDevice_, 1, &copyFence_, VK_TRUE, UINT64_MAX);
#else
        ID3D12CommandAllocator* slotAlloc = res.slotCmdAlloc[slot].Get();
        ID3D12GraphicsCommandList* slotList = res.slotCmdList[slot].Get();
        slotAlloc->Reset();
        slotList->Reset(slotAlloc, nullptr);

        Microsoft::WRL::ComPtr<ID3D12Resource> dstResource = swapchainImages_[swapchainIndex].texture;

        D3D12_RESOURCE_BARRIER toDest{};
        toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDest.Transition.pResource = dstResource.Get();
        toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        slotList->ResourceBarrier(1, &toDest);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstResource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res.uploadHeap[slot].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        // Must match the swapchain's ACTUAL format (swapchainDxgiFormat_),
        // not necessarily aurora's native format -- see createSwapchain()'s
        // comment. After the channel-swap step above, the uploaded bytes
        // really are laid out as swapchainDxgiFormat_ when a swap happened.
        src.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(swapchainDxgiFormat_);
        src.PlacedFootprint.Footprint.Width = eyeWidth;
        src.PlacedFootprint.Footprint.Height = eyeHeight;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = res.uploadRowPitch;

        // dstXOffset lands this eye's copy in the correct half of the
        // double-wide resource; dstY/dstZ stay 0 (full height, single layer).
        slotList->CopyTextureRegion(&dst, dstXOffset, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER toCommon = toDest;
        std::swap(toCommon.Transition.StateBefore, toCommon.Transition.StateAfter);
        slotList->ResourceBarrier(1, &toCommon);

        slotList->Close();
        ID3D12CommandList* lists[] = {slotList};
        xrQueue_->ExecuteCommandLists(1, lists);

        // NOT blocking anymore (2026-09-16 perf follow-up -- this used to
        // be a guaranteed WaitForSingleObject(..., INFINITE) here, every
        // single call, confirmed via real in-headset timing to be a large
        // chunk of the whole CPU-readback round trip's cost). Record which
        // fence value this slot's copy will reach, then return. The NEXT
        // time this same slot comes up (2 calls from now -- this same eye,
        // next frame), the wait at the TOP of this function checks this
        // value instead -- by then a whole frame has elapsed, so that
        // check is a no-op in the common case rather than a guaranteed
        // stall every single call. Safe without any wait here because (a)
        // this slot's own buffer/command-list won't be touched again until
        // that later check passes, and (b) the swapchain image's own
        // resource-state transitions are correctly ordered by the GPU
        // itself since both eyes submit to the same queue in sequence --
        // D3D12 command lists on one queue execute in submission order
        // without needing CPU-side waits between them.
        ++copyFenceValue_;
        xrQueue_->Signal(copyFence_.Get(), copyFenceValue_);
        res.slotFenceValue[slot] = copyFenceValue_;
        res.slotIndex = (slot + 1) % CpuCopyBuffers::kUploadSlotCount;
#endif

    }

private:
    struct CpuCopyTaskPayload {
        uint32_t eyeIndex;
        uint32_t eyeWidth;
        uint32_t eyeHeight;
        // Only consulted when sameDeviceAsAurora_/usesGpuDirectSwapchainCopy()
        // -- unused (left zero-initialized by encodeEyeCopy()'s payload) on
        // the plain CPU-copy path, harmless either way.
        uint32_t swapchainIndex = 0;
        uint32_t dstXOffset = 0;
        // Vulkan shared-image path only: which double-buffer slot this
        // frame's copies target (see SharedImageSlot) -- captured at push time so
        // the worker-thread callback can't observe a later frame's slot.
        uint32_t sharedSlot = 0;
    };

    // Runs on the render worker thread, positioned between render passes on
    // the frame's real encoder -- see gfx.hpp's EncoderTaskCallback contract
    // (must not Finish() the encoder; may record copies). This is the Dawn
    // side of the copy only: eye texture -> CPU-readable staging buffer.
    static void encoderTaskCallback(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
                                     const void* payload, size_t /*payloadSize*/, void* userdata) {
        auto* self = static_cast<Session*>(userdata);
        const auto& p = *static_cast<const CpuCopyTaskPayload*>(payload);
        auto& res = self->cpuCopyBuffers_[p.eyeIndex];
        const wgpu::Texture& srcTexture = self->pendingCopySrc_[p.eyeIndex];

        wgpu::CommandEncoder mutableCmd = cmd; // several calls below are non-const on CommandEncoder
        // GPU profiler zone (aurora gpu_prof, log mode on Android): the whole
        // hand-off -- gamma compute + buffer->texture copy -- as one zone.
        const aurora::webgpu::gpu_prof::Zone gpuZone{cmd, "VR swapchain handoff"};

        // TRIED AND REVERTED (Quest perf, 2026-09-20): replacing the identity
        // gamma compute + buffer->texture copy below with one
        // CopyTextureToTexture(eye snapshot -> shared swapchain image) --
        // RGBA8Unorm/RGBA8UnormSrgb are copy-compatible, so it's the same
        // bytes. Measured WORSE on the CPU: worker Queue::Submit 3.3 -> 6.0ms,
        // the main thread's synchronize() wait 5 -> 13ms (Dawn evidently does
        // more than a plain vkCmdCopyImage for a copy into an imported
        // SharedTextureMemory texture). GPU effect unmeasurable under the
        // thermal throttling seen in the same run. Don't retry as-is.

        if (self->useGammaComputePath_) {
            // GPU gamma-compensation path (see kGammaComputeShaderSource's
            // comment) -- replaces the plain CopyTextureToBuffer below with
            // a compute dispatch that applies the correction curve directly,
            // then a GPU-side buffer copy into the same CPU-mappable
            // `readback` buffer the non-gamma path would have written via
            // CopyTextureToBuffer. ctx.queue is the right handle for the
            // WriteBuffer call here -- borrowed/valid for this call only,
            // but that's exactly the per-frame-work use case gfx.hpp
            // documents it for.
            // swapRB=0 makes the shader pack BGRA bytes (its "native" case,
            // see kGammaComputeShaderSource's comment) -- correct only when
            // the CHOSEN swapchain format really is BGRA-ordered. Was
            // wrongly derived from SwapchainPixelConversion::ChannelSwap
            // (a different question -- see swapchainIsBgra_'s own comment
            // at its assignment in createSwapchain() for the real Quest 3
            // blue-tint bug this fixed) -- now tied directly to the actual
            // chosen format's real channel layout instead.
            const GammaComputeParams params{
                p.eyeWidth,
                p.eyeHeight,
                res.bytesPerRow / 4,
                self->swapchainIsBgra_ ? 0u : 1u,
                self->effectiveGammaExponent(),
            };
            ctx.queue.WriteBuffer(res.gammaUniform, 0, &params, sizeof(params));

            wgpu::TextureView srcView = srcTexture.CreateView();

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].textureView = srcView;
            entries[1].binding = 1;
            entries[1].buffer = res.gammaStorage;
            entries[1].size = static_cast<uint64_t>(res.bytesPerRow) * p.eyeHeight;
            entries[2].binding = 2;
            entries[2].buffer = res.gammaUniform;
            entries[2].size = sizeof(GammaComputeParams);

            wgpu::BindGroupDescriptor bgDesc{};
            bgDesc.layout = self->gammaBindGroupLayout_;
            bgDesc.entryCount = 3;
            bgDesc.entries = entries;
            wgpu::BindGroup bindGroup = aurora::webgpu::g_device.CreateBindGroup(&bgDesc);

            wgpu::ComputePassEncoder pass = mutableCmd.BeginComputePass();
            pass.SetPipeline(self->gammaPipeline_);
            pass.SetBindGroup(0, bindGroup);
            pass.DispatchWorkgroups((p.eyeWidth + 7) / 8, (p.eyeHeight + 7) / 8, 1);
            pass.End();

#if !DUSK_VR_XR_GRAPHICS_VULKAN
            // GPU-DIRECT PATH (2026-09-16, see sameDeviceAsAurora_'s comment):
            // the compute pass above already wrote final, correctly-ordered,
            // gamma-corrected bytes into res.gammaStorage -- exactly the byte
            // layout the XR swapchain image expects. Copy straight into the
            // (already BeginAccess'd -- see beginSwapchainAccessForFrame(),
            // called once per frame from tick() right after
            // xrAcquireSwapchainImage/xrWaitSwapchainImage succeed)
            // Dawn-wrapped swapchain texture, GPU-side, same device/queue as
            // everything else in this frame -- no CPU touch, no separate
            // device, no manual submit/wait. Replaces the old
            // CopyBufferToBuffer-into-res.readback + readbackEyeCopy()'s
            // whole MapAsync/upload-heap/CopyTextureRegion dance for this
            // path entirely.
            //
            // FOUND 2026-09-17 (crash right after fixing the typeless-
            // swapchain-resource crash above): this branch checked the raw
            // sameDeviceAsAurora_ flag directly instead of the combined
            // usesGpuDirectSwapchainCopy() every other call site (including
            // the two in vr_main.cpp that decide whether this task type
            // even runs at all) already uses. sameDeviceAsAurora_ can stay
            // true even when the GPU-direct path is disabled for the
            // session (device reuse itself succeeded -- that's independent
            // of the swapchain resource format being unusable, see
            // swapchainResourceFormatUsable_'s comment), so this branch
            // still tried to index swapchainTextures_ -- a vector that's
            // ONLY ever populated by ensureSwapchainTexture(), itself only
            // ever reached when usesGpuDirectSwapchainCopy() was true.
            // Access violation on a default-constructed wgpu::Texture.
            if (self->usesGpuDirectSwapchainCopy()) {
                wgpu::TexelCopyBufferInfo srcBuf{};
                srcBuf.buffer = res.gammaStorage;
                srcBuf.layout.offset = 0;
                srcBuf.layout.bytesPerRow = res.bytesPerRow;
                srcBuf.layout.rowsPerImage = p.eyeHeight;

                wgpu::TexelCopyTextureInfo dstTex{};
                // Intermediate-texture variant (the path that runs in
                // practice -- see ensureIntermediateTexture()'s comment):
                // same copy, just into our own typed texture; the real
                // swapchain image gets it via finishIntermediateSwapchainCopy().
                dstTex.texture = self->usesIntermediateSwapchainCopy()
                                     ? self->intermediateTexture_
                                     : self->swapchainTextures_[p.swapchainIndex];
                dstTex.mipLevel = 0;
                dstTex.origin = {p.dstXOffset, 0, 0};
                dstTex.aspect = wgpu::TextureAspect::All;

                wgpu::Extent3D extent{p.eyeWidth, p.eyeHeight, 1};
                mutableCmd.CopyBufferToTexture(&srcBuf, &dstTex, &extent);
                return;
            }
#else
            // SHARED-IMAGE GPU-DIRECT PATH (Vulkan/Android -- see
            // ensureSharedImageResources()'s comment): same shape as the
            // D3D12 branch above, targeting this frame's slot's Dawn texture
            // (an exported VkImage's memory) instead of the swapchain
            // texture, which Dawn can't wrap on this platform. The real
            // swapchain image is written once per frame by
            // finishSharedImageGpuCopy()'s blit on the XR-side device.
            if (self->usesSharedImageGpuDirect()) {
                wgpu::TexelCopyBufferInfo srcBuf{};
                srcBuf.buffer = res.gammaStorage;
                srcBuf.layout.offset = 0;
                srcBuf.layout.bytesPerRow = res.bytesPerRow;
                srcBuf.layout.rowsPerImage = p.eyeHeight;

                wgpu::TexelCopyTextureInfo dstTex{};
                dstTex.texture = self->sharedSlots_[p.sharedSlot].texture;
                dstTex.mipLevel = 0;
                dstTex.origin = {p.dstXOffset, 0, 0};
                dstTex.aspect = wgpu::TextureAspect::All;

                wgpu::Extent3D extent{p.eyeWidth, p.eyeHeight, 1};
                mutableCmd.CopyBufferToTexture(&srcBuf, &dstTex, &extent);
                return;
            }
#endif

            mutableCmd.CopyBufferToBuffer(res.gammaStorage, 0, res.readback, 0,
                                           static_cast<uint64_t>(res.bytesPerRow) * p.eyeHeight);
            return;
        }

        wgpu::TexelCopyTextureInfo srcCopy{};
        srcCopy.texture = srcTexture;
        srcCopy.aspect = wgpu::TextureAspect::All;

        wgpu::TexelCopyBufferInfo dstCopy{};
        dstCopy.buffer = res.readback;
        dstCopy.layout.offset = 0;
        dstCopy.layout.bytesPerRow = res.bytesPerRow;
        dstCopy.layout.rowsPerImage = p.eyeHeight;

        wgpu::Extent3D extent{p.eyeWidth, p.eyeHeight, 1};
        mutableCmd.CopyTextureToBuffer(&srcCopy, &dstCopy, &extent);
    }

    aurora::gfx::EncoderTaskId cpuCopyTaskId_ = aurora::gfx::InvalidEncoderTask;
    // FIXED this session: indexed by eyeIndex (0/1), NOT swapchainIndex --
    // see encodeEyeCopy's comment. Holds each eye's source texture between
    // encodeEyeCopy() (push time) and encoderTaskCallback (actual
    // execution, later, on the worker thread).
    std::vector<wgpu::Texture> pendingCopySrc_;

public:

    // Call after the copy/draw work for this frame has been encoded and
    // submitted. Ends access on all pending shared textures, then -- if
    // fence sync is active -- signals the XR-side queue to the next fence
    // value so the FOLLOWING frame's BeginAccess wait is against real
    // completed work rather than a stale/zero value.
    void endAccessAll() {
        // On both branches, pendingMemory_/pendingTextures_ only ever get
        // populated by importSwapchainImage() -- D3D12-only, dead code even
        // there (see this file's top comment) -- so this loop body is
        // unreached in practice either way; kept exactly as before so it's
        // still correct if that path is ever revived.
        for (size_t i = 0; i < pendingMemory_.size(); ++i) {
            wgpu::SharedTextureMemoryEndAccessState endState{};
            pendingMemory_[i].EndAccess(pendingTextures_[i], &endState);

#if !DUSK_VR_XR_GRAPHICS_VULKAN
            if (fenceInitialized_ && endState.signaledValueCount > 0) {
                fenceValue_ = endState.signaledValues[0];
            }
#endif
            // NOTE: no manual FreeMembers() call here -- it's private on
            // this struct (unlike SharedBufferMemoryEndAccessState, whose
            // FreeMembers is public). SharedTextureMemoryEndAccessState is
            // RAII-managed: its destructor calls FreeMembers() internally
            // when endState goes out of scope at the end of this loop
            // iteration. Confirmed against dawn/webgpu_cpp.h ~line
            // 4208-4224 (private: FreeMembers()/Reset(); destructor ~8575).
        }
        pendingMemory_.clear();
        pendingTextures_.clear();

#if !DUSK_VR_XR_GRAPHICS_VULKAN
        if (fenceInitialized_ && xrQueue_) {
            ++fenceValue_;
            xrQueue_->Signal(d3dFence_.Get(), fenceValue_);
        }
#endif
    }

private:
    XrInstance instance_;
    XrSystemId systemId_;
    XrSession session_;
    XrSpace localSpace_;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrFrameState frameState_{XR_TYPE_FRAME_STATE};

    // Set by createSwapchain() -- see its comment. swapchainDxgiFormat_ is
    // what the swapchain images were ACTUALLY created with (may differ from
    // aurora's native color format if the runtime didn't support that
    // format); swapchainPixelConversion_ tells readbackEyeCopy() what it
    // must do to the pixel data per-pixel while uploading to match.
    int64_t swapchainDxgiFormat_ = 0;
    SwapchainPixelConversion swapchainPixelConversion_ = SwapchainPixelConversion::None;
    // True when chosenFormat is B8G8R8A8_UNORM_SRGB/R8G8B8A8_UNORM_SRGB.
    // Informational only as of 2026-08-20 -- previously doubled as the
    // signal for "is this SteamVR" in effectiveGammaExponent(), which broke
    // once createSwapchain() started preferring SRGB for every runtime, not
    // just SteamVR (see isSteamVr_ below for the real replacement).
    bool swapchainIsSrgb_ = false;
    // True when chosenFormat's real channel layout is B8G8R8A8 (as opposed
    // to R8G8B8A8) -- drives kGammaComputeShaderSource's swapRB flag. See
    // its assignment in createSwapchain() for the real Quest 3 blue-tint
    // bug (R/B swapped) this exists to fix -- SwapchainPixelConversion
    // alone wasn't enough to answer "should the shader pack BGRA or RGBA
    // bytes", only "does this differ from aurora's assumed native format".
    bool swapchainIsBgra_ = false;

    // Set once at startup (vr_main.cpp's startup(), right after
    // xrGetSystemProperties()) from a substring check on the real runtime
    // name -- the actual "is this SteamVR" signal effectiveGammaExponent()
    // needs. Added 2026-08-20: swapchainIsSrgb_ used to double for this,
    // which was correct back when SteamVR was the ONLY runtime that ever
    // chose an SRGB format (forced, since its compositor rejects the plain
    // native format for real submission) -- but createSwapchain() now
    // prefers SRGB universally (see its own comment), so Virtual Desktop/
    // Meta Link can also end up with swapchainIsSrgb_==true, and routing
    // them through steamVrGammaExponent_ in that case would be wrong --
    // they need their own independent gammaCompensationMultiplier_, same as
    // before, regardless of which format ended up chosen.
    bool isSteamVr_ = false;

    // True for every format the GPU gamma-compensation compute pass can
    // handle -- i.e. everything except the rare PackR10G10B10A2 last-resort
    // fallback (see createSwapchain()'s comment). Tells encoderTaskCallback()
    // to run the compute pass INSTEAD of a plain CopyTextureToBuffer, and
    // tells readbackEyeCopy() the resulting bytes are already final (plain
    // memcpy, no further CPU-side conversion -- the compute shader already
    // handles both gamma AND any channel reorder swapchainPixelConversion_
    // would otherwise call for). Generalized 2026-08-16 from a
    // swapchainIsSrgb_-only gate (SteamVR only) -- see
    // kSteamVrGammaCompensationExponent's comment for why.
    bool useGammaComputePath_ = false;

    // Set once at startup (vr_main.cpp's startup(), D3D12 branch only) when
    // adapterMatchesXrRequirement() confirmed the XR runtime's required
    // adapter is the SAME one Aurora's own Dawn device already landed on --
    // in which case startup() reused Aurora's own ID3D12Device/CommandQueue
    // (via getD3D12DeviceAndQueue()) as the XR graphics-binding device
    // instead of creating createXrGraphicsDevice()'s separate one. This is
    // the real fix for the CPU-readback round trip flagged throughout this
    // file's history (see readbackEyeCopy()'s own "perf TODO" comments and
    // the 2026-09-16 investigation that finally measured it: mapWait alone
    // ran 10-24ms/frame on Quest, submitFrameInternal 6.5-10.8ms/frame on a
    // strong PC rig) -- when true, encodeSwapchainCopy()/
    // usesGpuDirectSwapchainCopy() replace encodeEyeCopy()/readbackEyeCopy()
    // for the gamma-compute-path formats (effectively always -- see
    // useGammaComputePath_ above): the rendered eye texture is copied
    // straight into the XR swapchain image via a same-device, same-queue
    // Dawn CopyBufferToTexture, with zero CPU touch and zero cross-device
    // hop, because SharedTextureMemoryD3D12ResourceDescriptor's own
    // documented contract ("this ID3D12Resource must be created from the
    // same ID3D12Device used in the WGPUDevice") is now genuinely satisfied
    // -- unlike importSwapchainImage()/ensureFenceSync() above, which tried
    // to import a resource from a DIFFERENT device and needed (and never
    // got working) cross-device shared-handle + fence sync. Stays false
    // (falls back to the old CPU round-trip, unchanged) whenever the
    // adapters don't match (rare -- multi-GPU laptops) or on the Vulkan/
    // Android branch (not yet ported this pass -- see vr_main.cpp's
    // startup() for the D3D12-only detection).
    bool sameDeviceAsAurora_ = false;

    // See createSwapchain()'s comment (the "runtime's real swapchain
    // resource format" check). Starts true; set false, one-way, for the
    // rest of the session if the runtime's actual swapchain resource turns
    // out to be typeless (confirmed on Virtual Desktop/AMD RX 5700 XT --
    // Dawn's D3D12 SharedTextureMemory backend can't import a typeless
    // resource at all). ANDed into usesGpuDirectSwapchainCopy() below so
    // this is the ONLY place that needs to know about the distinction --
    // every other call site just checks usesGpuDirectSwapchainCopy().
    bool swapchainResourceFormatUsable_ = true;

public:
    void setSameDeviceAsAurora(bool v) { sameDeviceAsAurora_ = v; }
    bool sameDeviceAsAurora() const { return sameDeviceAsAurora_; }

    // True exactly when encodeSwapchainCopy() should be used in place of
    // encodeEyeCopy(), and readbackEyeCopy() should be skipped entirely --
    // see sameDeviceAsAurora_'s comment. False for the rare PackR10G10B10A2
    // fallback format even when sameDeviceAsAurora_ is true: that path
    // never runs the gamma compute pass (see useGammaComputePath_), and
    // this first pass doesn't build a second GPU-direct route for it --
    // low-value (rarely chosen) and higher-risk to get right blind, so it
    // keeps using the proven CPU path instead. Also false once
    // swapchainResourceFormatUsable_ has been detected false -- see its
    // own comment.
    bool usesGpuDirectSwapchainCopy() const {
#if DUSK_VR_XR_GRAPHICS_VULKAN
        return sameDeviceAsAurora_ && useGammaComputePath_ && swapchainResourceFormatUsable_;
#else
        if (!sameDeviceAsAurora_ || !useGammaComputePath_ || intermediateCreateFailed_) {
            return false;
        }
        // Direct import of the runtime's own resource is only usable when
        // it turned out typed AND that path is opted into; otherwise the
        // intermediate-texture variant handles it (typed or typeless
        // alike) -- see ensureIntermediateTexture()'s comment.
        return true;
#endif
    }

#if !DUSK_VR_XR_GRAPHICS_VULKAN
    // D3D12: no shared-image render target (the intermediate/swapchain
    // textures aren't set up as render attachments); tick() keeps the copy.
    bool sharedImageRenderTarget(aurora::gfx::ExternalPassTarget* /*out*/) const { return false; }
#endif
#if !DUSK_VR_XR_GRAPHICS_VULKAN
    // 2026-09-19: the intermediate-texture variant (ensureIntermediateTexture()/
    // finishIntermediateSwapchainCopy()) is used for EVERY same-device
    // session, not just the typeless-swapchain case that forced it into
    // existence -- one code path that works regardless of how a given
    // runtime allocates its swapchain resource, exercised on every rig,
    // beats two paths of which one (direct import of the runtime's
    // resource) has never actually been seen working on real hardware.
    // Flip this to true to prefer the direct import whenever the runtime
    // did allocate a typed resource; the intermediate path still catches
    // the typeless case either way.
    static constexpr bool kDirectSwapchainImport = false;

    bool usesIntermediateSwapchainCopy() const {
        return usesGpuDirectSwapchainCopy() && !(kDirectSwapchainImport && swapchainResourceFormatUsable_);
    }
#endif

#if DUSK_VR_XR_GRAPHICS_VULKAN
    // Vulkan/Android counterpart of sameDeviceAsAurora_/usesGpuDirectSwapchainCopy()
    // above -- see the AHARDWAREBUFFER GPU-DIRECT SWAPCHAIN-COPY PATH
    // comment (near ensureSharedImageResources()) for the full "why this exists
    // and why it's a different mechanism than the D3D12 branch's" writeup.
    // Set once in vr_main.cpp's startup() when BOTH sides independently
    // support the opaque-fd image interop: Dawn's own adapter
    // (aurora::webgpu::g_vulkanSharedImageExportSupported) and
    // the XR-side device's actually-enabled Vulkan extension
    // (vr_xr::XrGraphicsDevice::supportsAndroidHardwareBuffer, from
    // createXrGraphicsDevice()) -- same "both sides must independently
    // support it" shape as the D3D12 path's adaptersMatch+
    // g_sharedTextureMemoryD3D12Supported pair. Also requires
    // useGammaComputePath_, same reasoning as usesGpuDirectSwapchainCopy()
    // above -- the rare PackR10G10B10A2 fallback format keeps the old CPU
    // path unconditionally on this branch too.
    void setUsesSharedImageGpuDirect(bool v) { usesSharedImageGpuDirect_ = v; }
    // Also false once sharedImageCreateFailed_ is set (a slot's allocation/import
    // failed mid-session) -- every call site then falls back to the CPU
    // path for the rest of the session, same shape as the D3D12 branch's
    // intermediateCreateFailed_.
    bool usesSharedImageGpuDirect() const { return usesSharedImageGpuDirect_ && useGammaComputePath_ && !sharedImageCreateFailed_; }

    // Direct-render path (Quest perf, 2026-09-20): this frame's shared
    // swapchain image as a render target for aurora::gfx::create_pass_external().
    // Valid after beginSwapchainAccessForFrame() has BeginAccess'd the
    // slot, only when the swapchain needs no gamma/channel work (gamma
    // exponent 1.0, RGBA-ordered format) -- the render writes the eye's
    // bytes straight into the image, exactly what the identity hand-off
    // (gamma compute + copy, measured at ~4ms of GPU per frame) produced.
    // A frame rendered this way needs NO encodeSwapchainCopy(); the XR-side
    // blit in finishSharedImageGpuCopy() runs as usual.
    bool sharedImageRenderTarget(aurora::gfx::ExternalPassTarget* out) const {
        if (!usesSharedImageGpuDirect() || !sharedFrameSlotValid_ || swapchainIsBgra_ ||
            effectiveGammaExponent() != 1.0f) {
            return false;
        }
        const SharedImageSlot& s = sharedSlots_[sharedFrameSlot_];
        if (!s.texture || !s.renderView) {
            return false;
        }
        out->texture = s.texture;
        out->view = s.renderView;
        out->format = aurora::gfx::color_format();
        out->width = sharedWidth_;
        out->height = sharedHeight_;
        return true;
    }

    // Set once at startup (vr_main.cpp's startup(), from
    // vr_xr::XrGraphicsDevice::supportsExternalSemaphoreFd) -- whether the
    // XR-side device actually got VK_KHR_external_semaphore_fd enabled.
    // Read by finishSharedImageGpuCopy() to decide whether the real async
    // semaphore handoff (Dawn's exported SharedFence imported as a
    // VkSemaphore the copy's own vkQueueSubmit waits on) is available, or
    // whether to fall back to the original CPU-blocking
    // OnSubmittedWorkDone() poll -- same "detect, don't assume, fail safe
    // to the known-correct path" discipline as every other GPU-direct
    // feature-gate in this file.
    void setSupportsExternalSemaphoreFd(bool v) { supportsExternalSemaphoreFd_ = v; }
#endif

private:

    // Live-adjustable gamma exponent for every NON-SteamVR runtime (see
    // effectiveGammaExponent()'s comment for why SteamVR is decoupled from
    // this as of 2026-08-16 -- despite the name, this is no longer a
    // multiplier on top of a hidden baseline, it directly IS the exponent
    // for those runtimes now). Set once per frame from vr_main.cpp's
    // tick() via setGammaCompensationMultiplier(), sourced from
    // dusk::getSettings().game.vrGammaCompensation (the "VR Gamma
    // Compensation" ImGui slider, ImGuiMenuTools.cpp). Read on the render
    // worker thread inside encoderTaskCallback() via effectiveGammaExponent()
    // -- a plain float member, not atomic, but only ever written once per
    // frame from the main thread before that frame's encoder task runs, so
    // there's no genuine concurrent-write hazard, just the same informal
    // single-word-read/write pattern this codebase already relies on
    // elsewhere for per-frame settings reads.
    //
    // COMPILED DEFAULT RESET TO 1.0, 2026-08-20 (settings.cpp): the
    // previously-confirmed `2.0` was tuned specifically for the OLD
    // scenario where Virtual Desktop/Meta Link submitted via the plain
    // non-SRGB nativeFormat (a raw, untouched passthrough that the runtime
    // then apparently treated as needing its OWN gamma-encode for display
    // -- see createSwapchain()'s big comment for the OpenXR-issue-#467
    // reasoning). Now that those runtimes prefer an SRGB-tagged swapchain
    // instead (correctly signaling "this content is already gamma-encoded"
    // to a spec-faithful compositor), `2.0` is very likely no longer the
    // right correction -- possibly close to 1.0/no-op if the runtime's own
    // decode is now accurate, per the OpenXR issue's description of Meta's
    // behavior specifically. Reset to 1.0 as the new starting point;
    // NOT yet re-tested in-headset on either runtime with the new SRGB
    // submission -- retune via the live slider from real feedback, same as
    // every other constant in this file, rather than assuming 1.0 is
    // already right.
    float gammaCompensationMultiplier_ = 1.0f;

    // Live-adjustable exponent for SteamVR specifically -- added 2026-08-16
    // after the user reported SteamVR looked UNDERSATURATED with the old
    // compiled kSteamVrGammaCompensationExponent baseline, which was itself
    // tuned back in section 6 by comparing SteamVR's own appearance against
    // VD/Meta Link's. That "UNDERSATURATED" report led to a same-day
    // "CORRECTED" pass setting the default to 1.0 (no compensation) --
    // **REVERSED 2026-08-20**, then **REVERSED AGAIN THE SAME DAY**: after
    // the createSwapchain() SRGB-preference change (see its own comment)
    // and the OpenXR-issue-#467 investigation that motivated it, the user
    // first reconsidered and said the ORIGINAL ~0.4545 brightening curve
    // was correct for SteamVR all along -- but then, right after that same
    // SRGB-preference reorder also confirmed Virtual Desktop correct at
    // 1.0/100% with zero compensation, explicit follow-up: "It should be
    // 1.0 or 100%, not 45%." The compiled default here
    // (kSteamVrGammaCompensationExponent) is only a harmless pre-first-
    // frame seed regardless of its own value -- the REAL value comes from
    // `dusk::getSettings().game.vrGammaCompensationSteamVr`, whose compiled
    // default (settings.cpp) is 1.0 again, THIRD flip in one project on
    // this exact value. Do not change it a fourth time without a real
    // side-by-side against the known-correct desktop mirror, not another
    // memory-based impression -- this project has already hit "don't trust
    // a previously-confirmed tuning indefinitely" more than once in this
    // exact file.
    float steamVrGammaExponent_ = kSteamVrGammaCompensationExponent;

public:
    void setGammaCompensationMultiplier(float multiplier) {
        gammaCompensationMultiplier_ = multiplier;
    }

    void setSteamVrGammaCompensationExponent(float exponent) {
        steamVrGammaExponent_ = exponent;
    }

    // Called once at startup (vr_main.cpp) from a substring check on
    // XrSystemProperties::systemName -- see isSteamVr_'s own comment for
    // why this can no longer be inferred from the chosen swapchain format.
    void setIsSteamVr(bool isSteamVr) { isSteamVr_ = isSteamVr; }

private:
    // The actual gammaExponent fed to kGammaComputeShaderSource's Params
    // each dispatch.
    //
    // DECOUPLED 2026-08-16 (was a single `baseline * multiplier` formula
    // applied uniformly to every runtime -- see the comment above this
    // class for the original design): once real testing came back
    // (`2.0` confirmed good on Virtual Desktop, NOT yet tested on SteamVR
    // or Meta Link), multiplying that same `2.0` onto SteamVR's own
    // ALREADY-TUNED `kSteamVrGammaCompensationExponent` (~0.4545) would
    // have pushed it to ~0.909 -- nearly cancelling a correction that was
    // independently confirmed correct back in section 6, on pure
    // untested speculation that the same multiplier applies there too.
    // So SteamVR was decoupled from this slider entirely, getting its own
    // INDEPENDENT live-adjustable exponent instead of the original
    // hardcoded constant. The two sliders are deliberately still fully
    // decoupled from each other -- adjusting one must never move the
    // other.
    //
    // CHANGED 2026-08-20: branches on isSteamVr_ (the real runtime
    // identity) instead of swapchainIsSrgb_ (which format got chosen).
    // Those two used to be equivalent -- SteamVR was the only runtime that
    // ever ended up with an SRGB swapchain -- but createSwapchain() now
    // prefers SRGB for every runtime that supports it (see its own
    // comment), so Virtual Desktop/Meta Link can also have
    // swapchainIsSrgb_==true. Branching on the stale signal would have
    // silently routed them through SteamVR's own exponent instead of their
    // own.
    float effectiveGammaExponent() const {
        if (isSteamVr_) {
            return steamVrGammaExponent_;
        }
        return gammaCompensationMultiplier_;
    }

    // GPU gamma-compensation compute pipeline -- created lazily by
    // ensureGammaComputeResources(), only when useGammaComputePath_ is true
    // (effectively always now -- see its own comment for the one rare
    // exception).
    wgpu::ComputePipeline gammaPipeline_;
    wgpu::BindGroupLayout gammaBindGroupLayout_;

#if DUSK_VR_XR_GRAPHICS_VULKAN
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages_;
#else
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages_;
#endif

    std::vector<wgpu::SharedTextureMemory> pendingMemory_;
    std::vector<wgpu::Texture> pendingTextures_;

#if !DUSK_VR_XR_GRAPHICS_VULKAN
    // GPU-direct swapchain-copy path (sameDeviceAsAurora_) -- one
    // SharedTextureMemory/Texture pair per swapchain image, lazily created
    // by ensureSwapchainTexture() and reused every frame (BeginAccess/
    // EndAccess bracket each frame's actual USE of the already-created
    // Texture object; the object itself doesn't need recreating). Indexed
    // by swapchainIndex, same as swapchainImages_ itself.
    std::vector<wgpu::SharedTextureMemory> swapchainMemory_;
    std::vector<wgpu::Texture> swapchainTextures_;

    // Intermediate-texture variant of the GPU-direct path -- see
    // ensureIntermediateTexture()'s comment. One typed, ALLOW_SIMULTANEOUS_
    // ACCESS resource on Aurora's own device for the whole session,
    // imported into Dawn once; finishIntermediateSwapchainCopy() copies it
    // into whichever real swapchain image was acquired each frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> intermediateResource_;
    wgpu::SharedTextureMemory intermediateMemory_;
    wgpu::Texture intermediateTexture_;
    uint32_t intermediateWidth_ = 0;
    uint32_t intermediateHeight_ = 0;
    // One-way: set if the resource/import could not be created, after
    // which usesGpuDirectSwapchainCopy() reads false for the session.
    bool intermediateCreateFailed_ = false;
    // Ring of command allocators/lists for the raw copy (same reasoning as
    // CpuCopyBuffers::slotCmdAlloc -- an allocator can't be Reset() while
    // the GPU may still be executing its last list; 2 slots + the shared
    // copyFence_ let each frame's copy submit without blocking).
    static constexpr uint32_t kIntermediateSlotCount = 2;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> intermediateCmdAlloc_[kIntermediateSlotCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> intermediateCmdList_[kIntermediateSlotCount];
    uint64_t intermediateSlotFenceValue_[kIntermediateSlotCount] = {0, 0};
    uint32_t intermediateSlot_ = 0;
#endif

#if DUSK_VR_XR_GRAPHICS_VULKAN
    // No fence-sync state on this branch -- see this file's top comment:
    // ensureFenceSync()/importSwapchainImage() (the only things that would
    // populate it) are dead code, D3D12-only, not ported.
    VkPhysicalDevice xrPhysicalDevice_ = VK_NULL_HANDLE;
    VkDevice xrDevice_ = VK_NULL_HANDLE;
    VkQueue xrQueue_ = VK_NULL_HANDLE;
    uint32_t xrQueueFamilyIndex_ = 0;

    // --- Shared-image GPU-direct swapchain-copy path state -- see
    // ensureSharedImageResources()'s comment for the full mechanism. ---
    bool usesSharedImageGpuDirect_ = false;
    bool supportsExternalSemaphoreFd_ = false;
    // Double-buffered slots -- see the "REWRITTEN 2026-09-19" block comment
    // above ensureSharedImageResources() for the design. sharedNextSlot_ is the slot
    // the NEXT beginSwapchainAccessForFrame() will open; sharedFrameSlot_ is
    // the one currently open for this frame (valid while sharedFrameSlotValid_).
    SharedImageSlot sharedSlots_[kSharedSlotCount];
    uint32_t sharedNextSlot_ = 0;
    uint32_t sharedFrameSlot_ = 0;
    bool sharedFrameSlotValid_ = false;
    uint32_t sharedWidth_ = 0;  // full double-wide size the shared images were created with
    uint32_t sharedHeight_ = 0;
    bool sharedImageCreateFailed_ = false;
    VkCommandPool sharedCopyCmdPool_ = VK_NULL_HANDLE;

    // --- Space warp state -- see the SPACE WARP section. ---
    bool swSupported_ = false;   // XR_FB_space_warp enabled on the instance
    bool swFailed_ = false;      // one-way: any failure disables it for the session
    uint32_t swMvWidth_ = 0;     // runtime-recommended motion-vector size, per eye
    uint32_t swMvHeight_ = 0;
    SpaceWarpSwapchain swMvSwapchain_;
    SpaceWarpSwapchain swDepthSwapchain_;
    DepthFormatChoice swDepthChoice_{};
    SpaceWarpSlot swSlots_[kSharedSlotCount];  // same slot index as sharedSlots_ each frame
    wgpu::RenderPipeline swPipeline_;
    wgpu::BindGroupLayout swBindGroupLayout_;
    aurora::gfx::EncoderTaskId swTaskId_ = aurora::gfx::InvalidEncoderTask;
    bool swAccessOpen_ = false;  // this frame's slot has BeginAccess'd its MV/depth images
    bool swEncoded_ = false;     // the MV pass was pushed for this frame
    bool swSubmitted_ = false;   // XR-side copies were recorded -> spaceWarpLayerInfo() valid
    // TEMP diagnostic readback (see spaceWarpDebugRecordReadback()).
    VkBuffer swDebugReadback_ = VK_NULL_HANDLE;
    VkDeviceMemory swDebugReadbackMemory_ = VK_NULL_HANDLE;
    uint32_t swDebugPendingSlot_ = UINT32_MAX;
    uint32_t swDebugFramesLogged_ = 0;
#else
    // --- fence sync state ---
    Microsoft::WRL::ComPtr<ID3D12Device> xrDevice_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> xrQueue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3dFence_;
    wgpu::SharedFence dawnFence_;
    uint64_t fenceValue_ = 0;
    bool fenceInitialized_ = false;
#endif

    // --- CPU round-trip copy path state (see submitEyeCpuCopy above) ---
    struct CpuCopyBuffers {
        wgpu::Buffer readback;                              // Dawn side, MapRead|CopyDst
        uint32_t bytesPerRow = 0;                            // Dawn-side row pitch (256-aligned)
        uint32_t width = 0, height = 0;
#if DUSK_VR_XR_GRAPHICS_VULKAN
        VkBuffer uploadBuffer = VK_NULL_HANDLE;              // XR side, HOST_VISIBLE|HOST_COHERENT
        VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
        void* uploadMapped = nullptr;                        // persistently mapped, see ensureCpuCopyBuffers()
#else
        // DOUBLE-BUFFERED (2026-09-16, "remove the second blocking wait"
        // perf follow-up -- see readbackEyeCopy()'s own comment for the
        // full reasoning): two upload heaps + command allocators/lists per
        // eye, alternated via slotIndex, so readbackEyeCopy() can skip
        // blocking on the XR-side D3D12 copy's completion before
        // returning every single call. A slot only gets reused/reset 2
        // calls later (this same eye, next frame) -- by then the GPU has
        // almost always already finished with it, so the fence check
        // before reuse is a no-op in the common case instead of a
        // guaranteed stall.
        static constexpr uint32_t kUploadSlotCount = 2;
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadHeap[kUploadSlotCount];      // XR side, D3D12_HEAP_TYPE_UPLOAD
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> slotCmdAlloc[kUploadSlotCount];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> slotCmdList[kUploadSlotCount];
        // The copyFence_/copyFenceValue_ value that will be reached once
        // this slot's last-submitted copy completes -- checked (and
        // waited on only if not yet reached) before the slot's buffer/
        // command list are reused.
        uint64_t slotFenceValue[kUploadSlotCount] = {0, 0};
        uint32_t slotIndex = 0;
#endif
        uint32_t uploadRowPitch = 0;                         // XR-side row pitch (256-aligned, both APIs)
        // Only created/used when useGammaComputePath_ is true (see
        // ensureGammaComputeResources()). gammaStorage is written by the
        // compute pass, then CopyBufferToBuffer'd into `readback` (the same
        // CPU-mappable buffer used by the non-gamma path) -- WebGPU doesn't
        // allow combining Storage with MapRead usage on one buffer, hence
        // the extra GPU-side copy.
        wgpu::Buffer gammaStorage;   // Storage|CopySrc
        wgpu::Buffer gammaUniform;   // Uniform|CopyDst -- holds GammaComputeParams
    };
    // FIXED this session: indexed by eyeIndex (0/1), NOT swapchainIndex --
    // the previous swapchainIndex keying made both eyes share the exact
    // same staging buffer object every frame, since this is a single
    // shared double-wide swapchain image (same swapchainIndex for both
    // eyes, every frame) -- see encodeEyeCopy's comment for the full
    // mechanism and the crash this caused.
    std::vector<CpuCopyBuffers> cpuCopyBuffers_;

#if DUSK_VR_XR_GRAPHICS_VULKAN
    VkCommandPool copyCmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer copyCmdBuf_ = VK_NULL_HANDLE;
    // Binary fence, reset+waited every call -- unlike the D3D12 branch's
    // monotonic copyFenceValue_ counter, Vulkan's vkWaitForFences on a
    // freshly-reset VkFence needs no counter to track, same "block fully
    // every frame, correctness first" behavior either way.
    VkFence copyFence_ = VK_NULL_HANDLE;
#else
    // copyCmdAlloc_/copyCmdList_ used to live here as a single pair shared
    // between both eyes and every frame -- moved into CpuCopyBuffers::
    // slotCmdAlloc/slotCmdList (double-buffered per eye) 2026-09-16, see
    // ensureCpuCopyCmdList()'s and readbackEyeCopy()'s comments. A single
    // shared allocator/list can't safely be Reset() between eye0's and
    // eye1's submissions within the same frame without blocking in
    // between, which is exactly the stall this change removes. The fence
    // itself (a single monotonic counter) is still safe to share -- D3D12
    // executes command lists submitted to the same queue in submission
    // order, so GetCompletedValue() reaching a given signaled value still
    // means everything submitted before it has completed too, regardless
    // of which slot/eye signaled it.
    Microsoft::WRL::ComPtr<ID3D12Fence> copyFence_;
    uint64_t copyFenceValue_ = 0;
#endif
    bool copyCmdListReady_ = false;

    static uint32_t align256(uint32_t v) { return (v + 255u) & ~255u; }

#if DUSK_VR_XR_GRAPHICS_VULKAN
    void ensureCpuCopyCmdList() {
        if (copyCmdListReady_) {
            return;
        }
        VkCommandPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCI.queueFamilyIndex = xrQueueFamilyIndex_;
        vkCreateCommandPool(xrDevice_, &poolCI, nullptr, &copyCmdPool_);

        VkCommandBufferAllocateInfo cbAllocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAllocInfo.commandPool = copyCmdPool_;
        cbAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAllocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(xrDevice_, &cbAllocInfo, &copyCmdBuf_);

        VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(xrDevice_, &fenceCI, nullptr, &copyFence_);

        copyCmdListReady_ = true;
    }
#else
    // DOUBLE-BUFFERED (2026-09-16): per-slot command allocators/lists now
    // live inside each eye's own CpuCopyBuffers (lazily created in
    // readbackEyeCopy() on first use of a given slot) instead of one
    // shared pair here -- see CpuCopyBuffers::slotCmdAlloc/slotCmdList's
    // comment. Only the fence itself still lives here, shared.
    void ensureCpuCopyCmdList() {
        if (copyCmdListReady_) {
            return;
        }
        xrDevice_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_));
        copyCmdListReady_ = true;
    }
#endif

    // width/height here are the PER-EYE dimensions (eyeWidth/eyeHeight from
    // encodeEyeCopy) -- these staging buffers hold one eye's pixels, not
    // the full double-wide swapchain image. Keyed by eyeIndex (0/1), NOT
    // swapchainIndex -- see encodeEyeCopy's comment for why.
    void ensureCpuCopyBuffers(uint32_t eyeIndex, uint32_t width, uint32_t height,
                               wgpu::TextureFormat /*format*/) {
        if (cpuCopyBuffers_.size() <= eyeIndex) {
            cpuCopyBuffers_.resize(eyeIndex + 1);
        }
        auto& res = cpuCopyBuffers_[eyeIndex];
        if (res.readback && res.width == width && res.height == height) {
            return; // already sized correctly for this eye
        }

        const uint32_t rowBytes = width * 4;
        res.bytesPerRow = align256(rowBytes);
        res.uploadRowPitch = align256(rowBytes); // same rule today, kept separate on purpose -- see submitEyeCpuCopy's note
        res.width = width;
        res.height = height;

        wgpu::BufferDescriptor bufDesc{};
        bufDesc.size = static_cast<uint64_t>(res.bytesPerRow) * height;
        bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        res.readback = aurora::webgpu::g_device.CreateBuffer(&bufDesc);

#if DUSK_VR_XR_GRAPHICS_VULKAN
        const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(res.uploadRowPitch) * height;

        if (res.uploadMapped) {
            vkUnmapMemory(xrDevice_, res.uploadMemory);
            res.uploadMapped = nullptr;
        }
        if (res.uploadBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(xrDevice_, res.uploadBuffer, nullptr);
            res.uploadBuffer = VK_NULL_HANDLE;
        }
        if (res.uploadMemory != VK_NULL_HANDLE) {
            vkFreeMemory(xrDevice_, res.uploadMemory, nullptr);
            res.uploadMemory = VK_NULL_HANDLE;
        }

        VkBufferCreateInfo bufCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufCI.size = uploadSize;
        bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(xrDevice_, &bufCI, nullptr, &res.uploadBuffer);

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(xrDevice_, res.uploadBuffer, &memReq);

        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(xrPhysicalDevice_, &memProps);
        const VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t memTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & required) == required) {
                memTypeIndex = i;
                break;
            }
        }
        if (memTypeIndex == UINT32_MAX) {
            throw std::runtime_error(
                "ensureCpuCopyBuffers: no HOST_VISIBLE|HOST_COHERENT Vulkan memory type "
                "for the XR-side upload buffer");
        }

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = memTypeIndex;
        vkAllocateMemory(xrDevice_, &allocInfo, nullptr, &res.uploadMemory);
        vkBindBufferMemory(xrDevice_, res.uploadBuffer, res.uploadMemory, 0);

        // Persistently mapped for the buffer's lifetime -- HOST_COHERENT
        // means no explicit flush is needed after CPU writes, matching the
        // D3D12 upload heap's per-call Map()/Unmap() dance in effect
        // (always immediately re-mappable) without needing to repeat it
        // every readbackEyeCopy() call.
        vkMapMemory(xrDevice_, res.uploadMemory, 0, uploadSize, 0, &res.uploadMapped);
#else
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width = static_cast<UINT64>(res.uploadRowPitch) * height;
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = DXGI_FORMAT_UNKNOWN;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (uint32_t slot = 0; slot < CpuCopyBuffers::kUploadSlotCount; ++slot) {
            res.uploadHeap[slot].Reset();
            xrDevice_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&res.uploadHeap[slot]));
        }
        res.slotIndex = 0;
#endif

        if (useGammaComputePath_) {
            ensureGammaComputeResources();

            wgpu::BufferDescriptor storageDesc{};
            storageDesc.size = bufDesc.size; // same size/layout as `readback`
            storageDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
            res.gammaStorage = aurora::webgpu::g_device.CreateBuffer(&storageDesc);

            wgpu::BufferDescriptor uniformDesc{};
            uniformDesc.size = sizeof(GammaComputeParams);
            uniformDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            res.gammaUniform = aurora::webgpu::g_device.CreateBuffer(&uniformDesc);
        }
    }

    // Lazily creates the gamma-compensation compute pipeline (once per
    // session, the first time useGammaComputePath_ is true -- effectively
    // every session now, see that flag's own comment). See
    // kGammaComputeShaderSource's comment for why this exists.
    void ensureGammaComputeResources() {
        if (gammaPipeline_) {
            return;
        }

        wgpu::ShaderSourceWGSL wgslDesc{};
        wgslDesc.code = kGammaComputeShaderSource;
        wgpu::ShaderModuleDescriptor moduleDesc{};
        moduleDesc.nextInChain = &wgslDesc;
        wgpu::ShaderModule module = aurora::webgpu::g_device.CreateShaderModule(&moduleDesc);

        wgpu::ComputePipelineDescriptor pipelineDesc{};
        pipelineDesc.compute.module = module;
        pipelineDesc.compute.entryPoint = "main";
        gammaPipeline_ = aurora::webgpu::g_device.CreateComputePipeline(&pipelineDesc);
        gammaBindGroupLayout_ = gammaPipeline_.GetBindGroupLayout(0);
    }
};

#if !DUSK_VR_XR_GRAPHICS_VULKAN
inline void getD3D12DeviceAndQueue(const wgpu::Device& wgpuDevice,
                                    Microsoft::WRL::ComPtr<ID3D12Device>& outDevice,
                                    Microsoft::WRL::ComPtr<ID3D12CommandQueue>& outQueue) {
    outDevice = dawn::native::d3d12::GetD3D12Device(wgpuDevice.Get());
    outQueue = dawn::native::d3d12::GetD3D12CommandQueue(wgpuDevice.Get());
}

// Aurora's Dawn device is created via a plain RequestAdapter with
// PowerPreference::HighPerformance -- there is no LUID/adapter-adoption hook
// (confirmed by reading gpu.cpp). On a multi-GPU machine this can pick a
// *different* physical GPU than the one the XR runtime requires
// (Bootstrap::d3d12Requirements.adapterLuid). wgpu::AdapterInfo's
// vendorID/deviceID are base WebGPU spec fields (not Dawn-specific), so we
// cross-reference them against DXGI to recover Dawn's chosen adapter's LUID
// and compare it against what the XR runtime demands, rather than silently
// rendering on the wrong GPU.
//
// NOTE: RequestAdapterOptionsLUID (D3DBackend.h) exists and could pin Dawn's
// adapter choice directly instead of verifying after the fact -- confirmed
// unused anywhere in Aurora. Not yet decided whether to switch to that
// instead of/alongside this check.
inline bool adapterMatchesXrRequirement(const wgpu::AdapterInfo& adapterInfo,
                                         LUID requiredLuid, LUID* outActualLuid = nullptr) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return false;  // can't verify -- caller should treat this as "unknown", not "match"
    }

    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (desc.VendorId == adapterInfo.vendorID && desc.DeviceId == adapterInfo.deviceID) {
            if (outActualLuid) {
                *outActualLuid = desc.AdapterLuid;
            }
            return desc.AdapterLuid.LowPart == requiredLuid.LowPart &&
                   desc.AdapterLuid.HighPart == requiredLuid.HighPart;
        }
    }
    return false;  // no DXGI adapter matched Dawn's vendor/device ID at all
}
#endif  // !DUSK_VR_XR_GRAPHICS_VULKAN

// NOTE: this Dawn build has no way to recover a wgpu::Texture from a
// wgpu::TextureView (no GetTexture() method, no wgpuTextureViewGetTexture C
// function). Aurora's ResolvedTargets was extended with a `colorTexture`
// field (see gfx.hpp / common.cpp resolve_pass) so we get the real texture
// directly instead. Call this with resolved.colorTexture, not resolved.color.
inline void copyTextureToTexture(wgpu::CommandEncoder& encoder, const wgpu::Texture& src,
                                  const wgpu::Texture& dst, uint32_t width, uint32_t height) {
    wgpu::TexelCopyTextureInfo srcCopy{};
    srcCopy.texture = src;
    srcCopy.aspect = wgpu::TextureAspect::All;

    wgpu::TexelCopyTextureInfo dstCopy{};
    dstCopy.texture = dst;
    dstCopy.aspect = wgpu::TextureAspect::All;

    wgpu::Extent3D extent{width, height, 1};
    encoder.CopyTextureToTexture(&srcCopy, &dstCopy, &extent);
}

}  // namespace dusk::vr
