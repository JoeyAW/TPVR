// vr_xr_submit.hpp
//
// Bridges Aurora's resolved WebGPU eye textures into OpenXR's D3D12
// swapchain images, and drives the xrWaitFrame / xrBeginFrame / xrEndFrame
// loop that the XR runtime requires every displayed frame.
//
// --- Dependency note ---
// This file uses Dawn's D3D12 native backend API to import XR swapchain
// images as wgpu::Textures so that Aurora's encoder task can copy into them.
// The key header is:
//     dawn/native/D3D12Backend.h
// which lives inside aurora's extern/dawn submodule. The API used here
// (ExternalImageDXGI, BeginAccess/EndAccess) has been stable since ~2022
// but CHECK the actual Dawn commit that aurora vendors before building —
// if function signatures differ, the comments below say what to look for.
//
// --- Frame loop shape ---
//
// Your outer VR frame loop, replacing the existing aurora_begin_frame /
// aurora_end_frame block in m_Do_main.cpp, looks like this:
//
//     vr_xr::Session xr(boot);  // created once after aurora_initialize()
//
//     while (running) {
//         xr.waitFrame();        // blocks until runtime wants a frame
//         xr.beginFrame();
//         aurora_begin_frame();
//
//         for (int eye = 0; eye < 2; ++eye) {
//             vr_render::EyeParams eyeParams = xr.getEyeParams(eye);
//             vr_link::updateFrame(xr.getFrameInput());   // hide head, map hands
//             vr_render::beginEye(eyeParams);             // create_pass + camera
//             // ... normal scene render here ...
//             vr_render::HandPayload hp{ xr.getControllerPose(eye) };
//             aurora::gfx::push_custom_draw(handTypeId, &hp, sizeof(hp));
//             auto targets = vr_render::endEye();         // resolve_pass
//             xr.submitEye(eye, targets);                 // <-- this file
//         }
//
//         aurora_end_frame();    // presents desktop mirror window
//         xr.endFrame();        // submits to headset
//     }

#pragma once

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>    // XrSwapchainImageD3D12KHR

#include <aurora/gfx.hpp>              // aurora::gfx::push_encoder_task,
                                        // ResolvedTargets, EncoderTaskId

// Dawn native D3D12 backend — lives in aurora's extern/dawn submodule.
// Include path is typically: <dawn/native/D3D12Backend.h>
// If the build can't find it, add ${aurora_SOURCE_DIR}/extern/dawn/include
// (or wherever Dawn's include root is) to your CMakeLists target_include_directories.
#include <dawn/native/D3D12Backend.h>

#include <d3d12.h>                     // ID3D12Resource, ID3D12Fence
#include <wrl/client.h>                // Microsoft::WRL::ComPtr

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

// From vr_xr_bootstrap.hpp
#include "vr_xr_bootstrap.hpp"
// From vr_stereo_render.hpp (for EyeParams)
#include "vr_stereo_render.hpp"
// From vr_link_visibility.hpp (for FrameInput)
#include "vr_link_visibility.hpp"

namespace vr_xr {

// ---------------------------------------------------------------------------
// Internal: encoder task that copies one eye's resolved WebGPU texture
// into the acquired OpenXR D3D12 swapchain image.
//
// Registered once at session creation, reused every frame.
// Runs on Aurora's render worker thread inside push_encoder_task().
// ---------------------------------------------------------------------------

struct EyeCopyPayload {
    // The wgpu::Texture that resolve_pass snapshotted into.
    // We store the raw WGPUTexture handle (C API) to keep this POD
    // and safely copyable by Aurora's inline payload mechanism.
    WGPUTexture srcTexture = nullptr;

    // The wgpu::Texture wrapping the acquired XR swapchain image.
    // Created via Dawn's ExternalImageDXGI::BeginAccess each frame.
    WGPUTexture dstTexture = nullptr;

    uint32_t width  = 0;
    uint32_t height = 0;
};
static_assert(sizeof(EyeCopyPayload) <= aurora::gfx::InlineDrawPayloadSize,
              "EyeCopyPayload too large for inline encoder task payload");

inline void eyeCopyCallback(
    const aurora::gfx::EncoderTaskContext& /*ctx*/,
    const wgpu::CommandEncoder& encoder,
    const void* payload,
    size_t      /*payloadSize*/,
    void*       /*userdata*/)
{
    const auto& p = *static_cast<const EyeCopyPayload*>(payload);
    if (!p.srcTexture || !p.dstTexture) return;

    wgpu::ImageCopyTexture src{};
    src.texture  = wgpu::Texture::Acquire(p.srcTexture);
    src.mipLevel = 0;
    src.origin   = {};
    src.aspect   = wgpu::TextureAspect::ColorOnly;

    wgpu::ImageCopyTexture dst{};
    dst.texture  = wgpu::Texture::Acquire(p.dstTexture);
    dst.mipLevel = 0;
    dst.origin   = {};
    dst.aspect   = wgpu::TextureAspect::ColorOnly;

    wgpu::Extent3D extent{ p.width, p.height, 1 };

    encoder.CopyTextureToTexture(&src, &dst, &extent);

    // Release the acquired references — Acquire() above incremented refcount.
    // The underlying WGPUTexture objects are owned by EyeSwapchain below.
    src.texture = nullptr;
    dst.texture = nullptr;
}

// ---------------------------------------------------------------------------
// Per-eye swapchain: wraps the XrSwapchain and the Dawn external image
// imports for each of its backing ID3D12Resources.
// ---------------------------------------------------------------------------

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t    width  = 0;
    uint32_t    height = 0;

    // One external image import per swapchain image.
    // Dawn's ExternalImageDXGI lets us wrap an ID3D12Resource that we don't
    // own (the XR runtime owns it) as a wgpu::Texture for the duration of
    // each frame's copy.
    struct SlotImage {
        ID3D12Resource* d3d12Resource = nullptr;   // non-owning, XR runtime owns it
        // ExternalImageDXGI wraps one external resource and vends
        // wgpu::Texture handles via BeginAccess / EndAccess.
        // NOTE: if your Dawn version uses a different type name here
        // (e.g. dawn::native::d3d12::ExternalImageDXGIImpl), check
        // D3D12Backend.h in your vendored Dawn for the current spelling.
        std::unique_ptr<dawn::native::d3d12::ExternalImageDXGI> externalImage;
    };
    std::vector<SlotImage> images;

    // Index of the currently-acquired swapchain image this frame.
    uint32_t acquiredIndex = 0;

    // The wgpu::Texture vended by BeginAccess for the current frame.
    // Stored as raw handle so EyeCopyPayload stays POD.
    WGPUTexture acquiredTexture = nullptr;

    void create(XrSession session, const XrViewConfigurationView& vcv,
                int64_t colorFormat) {
        width  = vcv.recommendedImageRectWidth;
        height = vcv.recommendedImageRectHeight;

        XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                        | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format      = colorFormat;
        sci.sampleCount = 1;
        sci.width       = width;
        sci.height      = height;
        sci.faceCount   = 1;
        sci.arraySize   = 1;
        sci.mipCount    = 1;

        if (XR_FAILED(xrCreateSwapchain(session, &sci, &handle))) {
            throw std::runtime_error("xrCreateSwapchain failed");
        }

        // Enumerate the D3D12 resources backing the swapchain images.
        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(handle, 0, &imgCount, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> xrImages(
            imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        xrEnumerateSwapchainImages(
            handle, imgCount, &imgCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data()));

        wgpu::Device wgpuDevice = aurora::gfx::device();

        images.resize(imgCount);
        for (uint32_t i = 0; i < imgCount; ++i) {
            images[i].d3d12Resource = xrImages[i].texture;

            // Create a DXGI shared handle for this ID3D12Resource so Dawn
            // can import it. The XR runtime must have created the resource
            // with the D3D12_HEAP_FLAG_SHARED flag; OpenXR runtimes targeting
            // D3D12 are required to do this for interop.
            HANDLE sharedHandle = nullptr;
            Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
            xrImages[i].texture->GetDevice(IID_PPV_ARGS(&d3d12Device));
            HRESULT hr = d3d12Device->CreateSharedHandle(
                xrImages[i].texture, nullptr, GENERIC_ALL, nullptr, &sharedHandle);
            if (FAILED(hr)) {
                throw std::runtime_error("CreateSharedHandle for XR swapchain image failed");
            }

            // Import into Dawn.
            // ExternalImageDescriptorDXGISharedHandle describes what Dawn
            // needs to know about the resource to wrap it safely.
            // NOTE: field names here match Dawn ~2023+; older commits may
            // use `sharedHandle` spelled differently or omit `isInitialized`.
            dawn::native::d3d12::ExternalImageDescriptorDXGISharedHandle desc{};
            desc.sharedHandle    = sharedHandle;
            desc.cTextureDescriptor = new WGPUTextureDescriptor{
                .nextInChain     = nullptr,
                .label           = "xr_swapchain_image",
                .usage           = WGPUTextureUsage_CopyDst
                                 | WGPUTextureUsage_RenderAttachment,
                .dimension       = WGPUTextureDimension_2D,
                .size            = { width, height, 1 },
                .format          = WGPUTextureFormat_BGRA8Unorm,
                .mipLevelCount   = 1,
                .sampleCount     = 1,
                .viewFormatCount = 0,
                .viewFormats     = nullptr,
            };
            desc.isInitialized = false;

            images[i].externalImage =
                dawn::native::d3d12::ExternalImageDXGI::Create(
                    wgpuDevice.Get(), &desc);

            // The shared handle can be closed after ExternalImageDXGI::Create;
            // Dawn has duplicated it internally.
            ::CloseHandle(sharedHandle);

            if (!images[i].externalImage) {
                throw std::runtime_error("Dawn ExternalImageDXGI::Create failed");
            }
        }
    }

    void destroy() {
        images.clear();
        if (handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(handle);
            handle = XR_NULL_HANDLE;
        }
    }

    // Call before scheduling the encoder task.
    void acquireImage() {
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        xrAcquireSwapchainImage(handle, &ai, &acquiredIndex);

        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(handle, &wi);

        // Wrap the acquired D3D12 resource as a wgpu::Texture for this frame.
        dawn::native::d3d12::ExternalImageDXGIBeginAccessDescriptor bad{};
        bad.isInitialized = true;
        bad.usage         = WGPUTextureUsage_CopyDst;
        acquiredTexture   =
            images[acquiredIndex].externalImage->BeginAccess(&bad);
    }

    // Call after the encoder task has run (i.e. after aurora_end_frame
    // flushes Aurora's command encoder).
    void releaseImage() {
        if (acquiredTexture) {
            // EndAccess signals the fence that tells the XR runtime the
            // copy is complete and the image is ready to composite.
            dawn::native::d3d12::ExternalImageDXGIFenceInfo fenceInfo{};
            images[acquiredIndex].externalImage->EndAccess(
                acquiredTexture, &fenceInfo);
            acquiredTexture = nullptr;
        }

        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(handle, &ri);
    }
};

// ---------------------------------------------------------------------------
// Session: owns the XrSession, both eye swapchains, and the encoder task.
// ---------------------------------------------------------------------------

class Session {
public:
    explicit Session(const Bootstrap& boot) {
        // Register the eye-copy encoder task once.
        aurora::gfx::EncoderTaskDescriptor etd{
            .label    = "vr_eye_copy",
            .callback = eyeCopyCallback,
            .userdata = nullptr,
        };
        copyTaskId_ = aurora::gfx::register_encoder_task_type(etd);

        createSession(boot);
        createSwapchains();
    }

    ~Session() {
        eyes_[0].destroy();
        eyes_[1].destroy();
        if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
        aurora::gfx::unregister_encoder_task_type(copyTaskId_);
    }

    // --- Per-frame API ---

    // Blocks until the XR runtime is ready for a new frame.
    // Call BEFORE aurora_begin_frame().
    void waitFrame() {
        frameState_ = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
        xrWaitFrame(session_, &fwi, &frameState_);
    }

    // Call after waitFrame(), before aurora_begin_frame().
    void beginFrame() {
        XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
        xrBeginFrame(session_, &fbi);

        // Locate the eye views for this frame's predicted display time.
        XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime           = frameState_.predictedDisplayTime;
        vli.space                 = localSpace_;

        XrViewState vs{ XR_TYPE_VIEW_STATE };
        uint32_t viewCount = 2;
        xrLocateViews(session_, &vli, &vs, 2, &viewCount, views_.data());

        // Acquire both swapchain images now so they're ready for the
        // encoder tasks that run during/after aurora_end_frame().
        eyes_[0].acquireImage();
        eyes_[1].acquireImage();
    }

    // Eye parameters for vr_render::beginEye().
    vr_render::EyeParams getEyeParams(int eye) const {
        return {
            .pose   = views_[eye].pose,
            .fov    = views_[eye].fov,
            .width  = eyes_[eye].width,
            .height = eyes_[eye].height,
        };
    }

    // FrameInput for vr_link::updateFrame() — HMD + controller poses.
    // controllerPoses must be filled in from your action-space locates
    // (xrLocateSpace on grip action spaces) before calling this.
    vr_link::FrameInput getFrameInput(const XrPosef& rightCtrl,
                                      const XrPosef& leftCtrl) const {
        // Use left eye pose as the HMD anchor (close enough; ~3cm IPD error).
        return { views_[0].pose, rightCtrl, leftCtrl };
    }

    // Call after vr_render::endEye() for each eye.
    // Schedules the WebGPU→XR swapchain copy onto Aurora's frame encoder.
    // The copy actually executes when Aurora flushes its encoder, which
    // happens inside aurora_end_frame() — so call this before that.
    void submitEye(int eye, const aurora::gfx::ResolvedTargets& targets) {
        assert(eye == 0 || eye == 1);
        if (!targets.color) return;

        // Get the raw WGPUTexture from the wgpu::TextureView.
        // TextureView::GetTexture() is available on wgpu::TextureView in
        // Dawn's C++ bindings. If your Dawn version doesn't have it,
        // use wgpuTextureViewGetTexture(targets.color.Get()) from the C API.
        wgpu::Texture srcTex = targets.color.GetTexture();

        EyeCopyPayload payload{
            .srcTexture = srcTex.MoveToCHandle(),   // transfer ref to payload
            .dstTexture = eyes_[eye].acquiredTexture,
            .width      = eyes_[eye].width,
            .height     = eyes_[eye].height,
        };

        aurora::gfx::push_encoder_task(copyTaskId_, &payload, sizeof(payload));
    }

    // Call after aurora_end_frame() — by then Aurora has flushed its encoder
    // and the copy tasks have run, so it's safe to EndAccess and release.
    void endFrame() {
        eyes_[0].releaseImage();
        eyes_[1].releaseImage();

        // Build projection layer for both eyes.
        std::array<XrCompositionLayerProjectionView, 2> projViews{};
        for (int i = 0; i < 2; ++i) {
            projViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            projViews[i].pose    = views_[i].pose;
            projViews[i].fov     = views_[i].fov;
            projViews[i].subImage.swapchain               = eyes_[i].handle;
            projViews[i].subImage.imageRect.offset        = { 0, 0 };
            projViews[i].subImage.imageRect.extent.width  = eyes_[i].width;
            projViews[i].subImage.imageRect.extent.height = eyes_[i].height;
        }

        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space     = localSpace_;
        layer.viewCount = 2;
        layer.views     = projViews.data();

        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
        };

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime          = frameState_.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount           = 1;
        fei.layers               = layers;
        xrEndFrame(session_, &fei);
    }

private:
    void createSession(const Bootstrap& boot) {
        // Bind Aurora's WebGPU/Dawn D3D12 device to the XR session.
        // dawn::native::d3d12::GetD3D12Device() extracts the raw ID3D12Device
        // and ID3D12CommandQueue from the wgpu::Device.
        // NOTE: exact function name may vary by Dawn version; look for
        // GetD3D12Device / GetD3D12CommandQueue in D3D12Backend.h.
        wgpu::Device wgpuDevice = aurora::gfx::device();
        wgpu::Queue  wgpuQueue  = aurora::gfx::queue();

        ID3D12Device*       d3dDevice  =
            dawn::native::d3d12::GetD3D12Device(wgpuDevice.Get());
        ID3D12CommandQueue* d3dQueue   =
            dawn::native::d3d12::GetD3D12CommandQueue(wgpuQueue.Get());

        XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        binding.device       = d3dDevice;
        binding.queue        = d3dQueue;

        XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
        sci.next     = &binding;
        sci.systemId = boot.systemId;

        if (XR_FAILED(xrCreateSession(boot.instance, &sci, &session_))) {
            throw std::runtime_error("xrCreateSession failed");
        }

        // Create a LOCAL reference space (seated/standing play).
        // Switch to STAGE if you want room-scale with a floor origin.
        XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rsci.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };
        xrCreateReferenceSpace(session_, &rsci, &localSpace_);
    }

    void createSwapchains() {
        // Query recommended eye resolutions.
        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(
            session_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &viewCount, nullptr);
        std::vector<XrViewConfigurationView> vcvs(viewCount,
            { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        xrEnumerateViewConfigurationViews(
            session_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            viewCount, &viewCount, vcvs.data());

        // Use BGRA8 SRGB — universally supported D3D12 XR swapchain format.
        // If the runtime rejects it, enumerate xrEnumerateSwapchainFormats
        // and pick the first BGRA or RGBA 8-bit format it reports.
        const int64_t colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

        for (int i = 0; i < 2; ++i) {
            eyes_[i].create(session_, vcvs[i], colorFormat);
        }

        views_.fill({ XR_TYPE_VIEW });
    }

    XrSession    session_    = XR_NULL_HANDLE;
    XrSpace      localSpace_ = XR_NULL_HANDLE;
    XrFrameState frameState_ = { XR_TYPE_FRAME_STATE };

    std::array<EyeSwapchain, 2> eyes_{};
    std::array<XrView, 2>       views_{};

    aurora::gfx::EncoderTaskId copyTaskId_ = aurora::gfx::InvalidEncoderTask;
};

} // namespace vr_xr
