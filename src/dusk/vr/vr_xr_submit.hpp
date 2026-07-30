#pragma once

// vr_xr_submit.hpp
// XR session/swapchain management + Dawn D3D12 interop for submitting
// rendered eye textures to the OpenXR runtime.
//
// FENCE SYNC: the fence-sync types are the base WebGPU-spec wgpu::SharedFence
// family, confirmed present in dawn/webgpu_cpp.h -- NOT
// dawn::native::d3d12::SharedFence* (that does not exist in this Dawn build;
// D3D12Backend.h has zero fence-related exports). Actual fence *creation* is
// plain D3D12 (ID3D12Device::CreateFence + CreateSharedHandle), done on the
// XR-side device from vr_xr_bootstrap.hpp's createXrGraphicsDevice(). Dawn
// only *imports* the resulting HANDLE via device.ImportSharedFence(...).
// After EndAccess, the XR-side queue is signaled to the value Dawn reports
// signaling, so the NEXT frame's BeginAccess wait is against real completed
// work rather than a stale/zero value. See VR_MOD_HANDOFF_4.md for the full
// investigation trail.

#include <openxr/openxr.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dawn/native/D3D12Backend.h>
#include <webgpu/webgpu_cpp.h>

// For aurora::webgpu::g_sharedFenceDxgiSupported -- ensureFenceSync() below
// must check this before calling ImportSharedFence(), since Aurora's
// uncaptured-error callback treats a post-init device error (including
// requesting/using an unsupported feature) as fatal, not a recoverable one.
// Path is relative from src/dusk/vr/vr_xr_submit.hpp to
// extern/aurora/lib/webgpu/gpu.hpp (both confirmed paths, not guessed).
#include "../../../extern/aurora/lib/webgpu/gpu.hpp"

#include "dusk/vr/vr_xr_bootstrap.hpp"

namespace dusk::vr {

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

// TEMP DIAGNOSTIC (VR water-black investigation): dumps an eye's actual
// rendered color buffer (post game-rendering, pre-XR-submission) to a BMP
// on disk, once per eyeIndex. Lets us distinguish "the game rendered water
// wrong" (buffer already shows black/whatever the user sees) from "the game
// rendered it correctly but something after this readback point -- XR
// submission, compositor, swapchain format -- corrupts it before the
// headset displays it" (buffer shows the correct/expected color). Only
// handles the 8-bit-per-channel formats actually in use for the VR
// swapchain; silently skips (with a log) for anything else rather than
// guessing a wrong channel order.
inline void dumpEyeBufferToBmp(const uint8_t* mapped, uint32_t width, uint32_t height,
                                uint32_t bytesPerRow, wgpu::TextureFormat format, uint32_t eyeIndex) {
    const bool isBgra = format == wgpu::TextureFormat::BGRA8Unorm ||
                         format == wgpu::TextureFormat::BGRA8UnormSrgb;
    const bool isRgba = format == wgpu::TextureFormat::RGBA8Unorm ||
                         format == wgpu::TextureFormat::RGBA8UnormSrgb;
    if (!isBgra && !isRgba) {
        OutputDebugStringA("[dusk::vr] dumpEyeBufferToBmp: unsupported format, skipping dump\n");
        return;
    }

    const uint32_t rowBytes = width * 4;
    const uint32_t imageSize = rowBytes * height;
    const uint32_t fileSize = 14 + 40 + imageSize;

    std::vector<uint8_t> fileBuf(fileSize);
    uint8_t* p = fileBuf.data();

    p[0] = 'B';
    p[1] = 'M';
    *reinterpret_cast<uint32_t*>(p + 2) = fileSize;
    *reinterpret_cast<uint32_t*>(p + 6) = 0;
    *reinterpret_cast<uint32_t*>(p + 10) = 54;

    uint8_t* h = p + 14;
    *reinterpret_cast<int32_t*>(h + 0) = 40;
    *reinterpret_cast<int32_t*>(h + 4) = static_cast<int32_t>(width);
    *reinterpret_cast<int32_t*>(h + 8) = -static_cast<int32_t>(height); // negative = top-down rows
    *reinterpret_cast<int16_t*>(h + 12) = 1;
    *reinterpret_cast<int16_t*>(h + 14) = 32;
    *reinterpret_cast<int32_t*>(h + 16) = 0;
    *reinterpret_cast<int32_t*>(h + 20) = static_cast<int32_t>(imageSize);
    *reinterpret_cast<int32_t*>(h + 24) = 0;
    *reinterpret_cast<int32_t*>(h + 28) = 0;
    *reinterpret_cast<int32_t*>(h + 32) = 0;
    *reinterpret_cast<int32_t*>(h + 36) = 0;

    uint8_t* pixels = p + 54;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = mapped + static_cast<size_t>(y) * bytesPerRow;
        uint8_t* dstRow = pixels + static_cast<size_t>(y) * rowBytes;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r, g, b, a;
            if (isBgra) {
                b = srcRow[x * 4 + 0];
                g = srcRow[x * 4 + 1];
                r = srcRow[x * 4 + 2];
                a = srcRow[x * 4 + 3];
            } else {
                r = srcRow[x * 4 + 0];
                g = srcRow[x * 4 + 1];
                b = srcRow[x * 4 + 2];
                a = srcRow[x * 4 + 3];
            }
            dstRow[x * 4 + 0] = b;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = r;
            dstRow[x * 4 + 3] = a;
        }
    }

    char path[260];
    _snprintf_s(path, _TRUNCATE, "C:\\Users\\joeyw\\dusklight\\vr_debug_eye%u.bmp", eyeIndex);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(fileBuf.data(), 1, fileBuf.size(), f);
        fclose(f);
        char msg[300];
        _snprintf_s(msg, _TRUNCATE, "[dusk::vr] dumped eye %u buffer (%ux%u) to %s\n", eyeIndex, width,
                    height, path);
        OutputDebugStringA(msg);
    } else {
        OutputDebugStringA("[dusk::vr] dumpEyeBufferToBmp: failed to open output file\n");
    }
}

class Session {
public:
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

    bool createSwapchain(uint32_t width, uint32_t height, int64_t format) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.width = width;
        ci.height = height;
        ci.sampleCount = 1;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;

        if (XR_FAILED(xrCreateSwapchain(session_, &ci, &swapchain_))) {
            return false;
        }

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchain_, 0, &imageCount, nullptr);
        swapchainImages_.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(
            swapchain_, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages_.data()));
        return true;
    }

    XrSwapchain swapchain() const { return swapchain_; }

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
        ensureCpuCopyCmdList();
        auto& res = cpuCopyBuffers_[eyeIndex];

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
            _snprintf_s(msg, _TRUNCATE,
                        "[dusk::vr] readbackEyeCopy: MapAsync failed/timed "
                        "out (mapDone=%d, mapOk=%d) -- skipping this eye's "
                        "swapchain copy\n",
                        mapDone, mapOk);
            OutputDebugStringA(msg);
            return;
        }

        const uint8_t* mapped = static_cast<const uint8_t*>(
            res.readback.GetConstMappedRange(0, static_cast<size_t>(res.bytesPerRow) * eyeHeight));

        // TEMP DIAGNOSTIC (VR water-black investigation): periodically
        // re-dump each eye's actual rendered buffer (overwriting the same
        // file), rather than once at VR startup -- a one-shot dump would
        // almost certainly fire before the user has navigated to water.
        // Instead the file on disk always reflects a recent frame; look at
        // water for a couple seconds in the headset, then read whatever's
        // currently there. See dumpEyeBufferToBmp()'s comment above.
        {
            static uint32_t frameCounter[2] = {0, 0};
            if (eyeIndex < 2 && (frameCounter[eyeIndex]++ % 90) == 0) {
                dumpEyeBufferToBmp(mapped, eyeWidth, eyeHeight, res.bytesPerRow, format, eyeIndex);
            }
        }

        // --- Copy row-by-row into the D3D12 upload heap. ---
        // Two independent row-pitch alignments (Dawn's and D3D12's are both
        // 256 today, but don't assume they stay equal -- copy the real
        // per-row byte count (eyeWidth * 4) and respect each side's own
        // pitch, tracked separately in CpuCopyBuffers).
        const uint32_t rowBytes = eyeWidth * 4; // RGBA8/BGRA8 -- 4 bytes/texel
        void* uploadMapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        res.uploadHeap->Map(0, &noRead, &uploadMapped);
        for (uint32_t y = 0; y < eyeHeight; ++y) {
            std::memcpy(static_cast<uint8_t*>(uploadMapped) + y * res.uploadRowPitch,
                        mapped + y * res.bytesPerRow, rowBytes);
        }
        D3D12_RANGE written{0, static_cast<SIZE_T>(res.uploadRowPitch) * eyeHeight};
        res.uploadHeap->Unmap(0, &written);

        res.readback.Unmap();

        // --- Record + execute the upload heap -> swapchain image copy on
        // the XR-side device/queue, offset into the correct eye's half of
        // the double-wide destination. ---
        copyCmdAlloc_->Reset();
        copyCmdList_->Reset(copyCmdAlloc_.Get(), nullptr);

        Microsoft::WRL::ComPtr<ID3D12Resource> dstResource = swapchainImages_[swapchainIndex].texture;

        D3D12_RESOURCE_BARRIER toDest{};
        toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDest.Transition.pResource = dstResource.Get();
        toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyCmdList_->ResourceBarrier(1, &toDest);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstResource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res.uploadHeap.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(toDxgiSwapchainFormat(format));
        src.PlacedFootprint.Footprint.Width = eyeWidth;
        src.PlacedFootprint.Footprint.Height = eyeHeight;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = res.uploadRowPitch;

        // dstXOffset lands this eye's copy in the correct half of the
        // double-wide resource; dstY/dstZ stay 0 (full height, single layer).
        copyCmdList_->CopyTextureRegion(&dst, dstXOffset, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER toCommon = toDest;
        std::swap(toCommon.Transition.StateBefore, toCommon.Transition.StateAfter);
        copyCmdList_->ResourceBarrier(1, &toCommon);

        copyCmdList_->Close();
        ID3D12CommandList* lists[] = {copyCmdList_.Get()};
        xrQueue_->ExecuteCommandLists(1, lists);

        // Block until the XR-side GPU has finished the copy before this
        // function returns -- the upload heap gets reused/remapped next
        // call, so it must not still be in flight. Same "correctness
        // first, perf TODO" note as the MapAsync wait above.
        ++copyFenceValue_;
        xrQueue_->Signal(copyFence_.Get(), copyFenceValue_);
        if (copyFence_->GetCompletedValue() < copyFenceValue_) {
            HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            copyFence_->SetEventOnCompletion(copyFenceValue_, event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
    }

private:
    struct CpuCopyTaskPayload {
        uint32_t eyeIndex;
        uint32_t eyeWidth;
        uint32_t eyeHeight;
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

        wgpu::TexelCopyTextureInfo srcCopy{};
        srcCopy.texture = srcTexture;
        srcCopy.aspect = wgpu::TextureAspect::All;

        wgpu::TexelCopyBufferInfo dstCopy{};
        dstCopy.buffer = res.readback;
        dstCopy.layout.offset = 0;
        dstCopy.layout.bytesPerRow = res.bytesPerRow;
        dstCopy.layout.rowsPerImage = p.eyeHeight;

        wgpu::Extent3D extent{p.eyeWidth, p.eyeHeight, 1};
        // ctx.device/ctx.queue are borrowed and valid only for this call, per
        // gfx.hpp -- but we only need `cmd` here (already positioned on the
        // frame's real encoder), not a separate device/queue reference.
        (void)ctx;
        wgpu::CommandEncoder mutableCmd = cmd; // CopyTextureToBuffer is non-const on CommandEncoder
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
        for (size_t i = 0; i < pendingMemory_.size(); ++i) {
            wgpu::SharedTextureMemoryEndAccessState endState{};
            pendingMemory_[i].EndAccess(pendingTextures_[i], &endState);

            if (fenceInitialized_ && endState.signaledValueCount > 0) {
                fenceValue_ = endState.signaledValues[0];
            }
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

        if (fenceInitialized_ && xrQueue_) {
            ++fenceValue_;
            xrQueue_->Signal(d3dFence_.Get(), fenceValue_);
        }
    }

private:
    XrInstance instance_;
    XrSystemId systemId_;
    XrSession session_;
    XrSpace localSpace_;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrFrameState frameState_{XR_TYPE_FRAME_STATE};

    std::vector<XrSwapchainImageD3D12KHR> swapchainImages_;

    std::vector<wgpu::SharedTextureMemory> pendingMemory_;
    std::vector<wgpu::Texture> pendingTextures_;

    // --- fence sync state ---
    Microsoft::WRL::ComPtr<ID3D12Device> xrDevice_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> xrQueue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3dFence_;
    wgpu::SharedFence dawnFence_;
    uint64_t fenceValue_ = 0;
    bool fenceInitialized_ = false;

    // --- CPU round-trip copy path state (see submitEyeCpuCopy above) ---
    struct CpuCopyBuffers {
        wgpu::Buffer readback;                              // Dawn side, MapRead|CopyDst
        uint32_t bytesPerRow = 0;                            // Dawn-side row pitch (256-aligned)
        uint32_t width = 0, height = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadHeap;   // XR side, D3D12_HEAP_TYPE_UPLOAD
        uint32_t uploadRowPitch = 0;                         // D3D12-side row pitch (256-aligned)
    };
    // FIXED this session: indexed by eyeIndex (0/1), NOT swapchainIndex --
    // the previous swapchainIndex keying made both eyes share the exact
    // same staging buffer object every frame, since this is a single
    // shared double-wide swapchain image (same swapchainIndex for both
    // eyes, every frame) -- see encodeEyeCopy's comment for the full
    // mechanism and the crash this caused.
    std::vector<CpuCopyBuffers> cpuCopyBuffers_;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyCmdAlloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyCmdList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> copyFence_;
    uint64_t copyFenceValue_ = 0;
    bool copyCmdListReady_ = false;

    static uint32_t align256(uint32_t v) { return (v + 255u) & ~255u; }

    void ensureCpuCopyCmdList() {
        if (copyCmdListReady_) {
            return;
        }
        xrDevice_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyCmdAlloc_));
        xrDevice_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyCmdAlloc_.Get(), nullptr,
                                      IID_PPV_ARGS(&copyCmdList_));
        copyCmdList_->Close(); // Reset() below expects a closed list first time
        xrDevice_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_));
        copyCmdListReady_ = true;
    }

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

        res.uploadHeap.Reset();
        xrDevice_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&res.uploadHeap));
    }
};

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
