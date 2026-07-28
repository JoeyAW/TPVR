# ============================================================
# Dusklight VR Mod — CMake fragment
#
# HOW TO USE:
#   Append this file's contents to the bottom of the root
#   CMakeLists.txt, just before the final aurora_install_runtime_dlls
#   call. It adds an INTERFACE library (dusklight_vr) that carries
#   all VR-specific include paths and link dependencies, then wires
#   it into the existing `dusklight` target with a single line.
#
# PREREQUISITES (Windows, before running cmake):
#
#   1. OpenXR loader
#      vcpkg install openxr-loader:x64-windows
#      -- or --
#      Download the OpenXR SDK from https://github.com/KhronosGroup/OpenXR-SDK
#      and set OPENXR_SDK_DIR in your cmake configure or CMakePresets.json:
#        "OPENXR_SDK_DIR": "C:/path/to/openxr-sdk"
#
#   2. Aurora submodule (required anyway to build Dusklight)
#      git submodule update --init --recursive
#      This also pulls extern/dawn, which provides dawn/native/D3D12Backend.h
#      and the wgpu C++ bindings that gfx.hpp includes.
#
#   3. VR source files
#      Copy the following headers into src/dusk/vr/ :
#        vr_xr_bootstrap.hpp
#        vr_stereo_render.hpp
#        vr_swing_detector.hpp
#        vr_link_visibility.hpp
#        vr_xr_submit.hpp
#      And create src/dusk/vr/vr_main.cpp (the frame-loop integration,
#      not yet written — see the comment at the bottom of this file).
# ============================================================

# --- Guard: Windows-only for now ---
# The D3D12 / DXGI shared-handle path in vr_xr_submit.hpp is Windows-specific.
# Vulkan + Linux support can be added later by adding a second submit backend
# (vr_xr_submit_vulkan.hpp) and selecting via cmake option.
if (NOT WIN32)
    message(STATUS "Dusklight VR: skipping — Windows only for now")
    return()
endif ()

# --- Option to disable ---
option(DUSK_VR "Build Dusklight VR mod" ON)
if (NOT DUSK_VR)
    message(STATUS "Dusklight VR: disabled (DUSK_VR=OFF)")
    return()
endif ()

message(STATUS "Dusklight VR: configuring")

# ============================================================
# 1. Find OpenXR
# ============================================================

# Try vcpkg / system find_package first, then fall back to a manual SDK path.
find_package(OpenXR CONFIG QUIET)

if (NOT OpenXR_FOUND)
    # Manual SDK path: set OPENXR_SDK_DIR in cmake configure or environment.
    if (DEFINED ENV{OPENXR_SDK_DIR})
        set(OPENXR_SDK_DIR "$ENV{OPENXR_SDK_DIR}")
    endif ()

    if (OPENXR_SDK_DIR)
        message(STATUS "Dusklight VR: using OpenXR SDK at ${OPENXR_SDK_DIR}")
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_IMPLIB   "${OPENXR_SDK_DIR}/lib/openxr_loader.lib"
            IMPORTED_LOCATION "${OPENXR_SDK_DIR}/bin/openxr_loader.dll"
        )
        target_include_directories(OpenXR::openxr_loader INTERFACE
            "${OPENXR_SDK_DIR}/include"
        )
    else ()
        message(WARNING
            "Dusklight VR: OpenXR SDK not found. "
            "Install via vcpkg (vcpkg install openxr-loader:x64-windows) "
            "or set OPENXR_SDK_DIR to the SDK root. "
            "Building without VR support.")
        return()
    endif ()
endif ()

# ============================================================
# 2. Locate Dawn's D3D12 backend header
#    (lives inside the aurora submodule's vendored Dawn)
# ============================================================

# Aurora vendors Dawn at extern/aurora/extern/dawn (populated by submodule).
# Aurora fetches Dawn as a prebuilt package via FetchContent.
# The headers land in the build directory under _deps/dawn_prebuilt-src/include/
# after the first configure completes. We search several candidate paths so
# this works regardless of whether it's a prebuilt or source build of Dawn.
set(DAWN_NATIVE_INCLUDE_CANDIDATES
    # Aurora prebuilt Dawn package (confirmed from build output)
    "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/include"
    # Fallback if Aurora switches to a source build
    "${CMAKE_BINARY_DIR}/_deps/dawn-src/include"
    # Fallback submodule layout
    "${CMAKE_SOURCE_DIR}/extern/aurora/extern/dawn/include"
)

set(DAWN_NATIVE_INCLUDE_DIR "")
foreach(candidate IN LISTS DAWN_NATIVE_INCLUDE_CANDIDATES)
    if (EXISTS "${candidate}/dawn/native/D3D12Backend.h")
        set(DAWN_NATIVE_INCLUDE_DIR "${candidate}")
        break()
    endif ()
endforeach()

if (NOT DAWN_NATIVE_INCLUDE_DIR)
    message(STATUS
        "Dusklight VR: dawn/native/D3D12Backend.h not found yet. "
        "Re-run cmake configure after the first build completes.")
    # Don't hard-fail — let the build proceed so Dawn gets downloaded,
    # then reconfigure. The missing include will cause a compile error
    # on vr_xr_submit.hpp which is acceptable as a "reconfigure needed" signal.
endif ()

message(STATUS "Dusklight VR: Dawn native headers at ${DAWN_NATIVE_INCLUDE_DIR}")

# ============================================================
# 3. INTERFACE library: dusklight_vr
#    Carries all VR headers + link deps. No compiled sources of
#    its own — the actual .cpp lives in dusklight's source list.
# ============================================================

add_library(dusklight_vr INTERFACE)

target_include_directories(dusklight_vr INTERFACE
    # VR headers we wrote — lives next to the rest of dusk's platform code
    "${CMAKE_SOURCE_DIR}/src/dusk/vr"
    # Dawn native D3D12 backend (for ExternalImageDXGI, GetD3D12Device, etc.)
    "${DAWN_NATIVE_INCLUDE_DIR}"
)

target_link_libraries(dusklight_vr INTERFACE
    OpenXR::openxr_loader
    # D3D12 and DXGI are already available via the Windows SDK; list them
    # explicitly so the linker finds ID3D12Device, CreateSharedHandle, etc.
    d3d12
    dxgi
)

target_compile_definitions(dusklight_vr INTERFACE
    DUSK_VR_ENABLED=1
    # Tell OpenXR platform headers to enable the D3D12 extension structs.
    XR_USE_GRAPHICS_API_D3D12=1
    XR_USE_PLATFORM_WIN32=1
)

# Require C++20 for designated initialisers ({.field = value} syntax used
# in the VR headers). Dusklight's existing code already targets C++20 per
# the aurora dependency, but state it explicitly on this target so the
# requirement is visible.
target_compile_features(dusklight_vr INTERFACE cxx_std_20)

# ============================================================
# 4. Wire into dusklight
# ============================================================

# The VR frame-loop integration source file. Create this at
# src/dusk/vr/vr_main.cpp — it #includes the VR headers and
# contains the hooks into m_Do_main.cpp's game loop.
# (Not yet written; see note at the bottom of this file.)
target_sources(dusklight PRIVATE
    src/dusk/vr/vr_main.cpp
)

target_link_libraries(dusklight PRIVATE
    dusklight_vr
)

# Copy the OpenXR loader DLL next to the dusklight executable at build time
# so you can run from the build directory without installing.
if (TARGET OpenXR::openxr_loader)
    get_target_property(_xr_dll OpenXR::openxr_loader IMPORTED_LOCATION)
    if (_xr_dll)
        add_custom_command(TARGET dusklight POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_xr_dll}"
                "$<TARGET_FILE_DIR:dusklight>"
            COMMENT "Copying openxr_loader.dll"
        )
    endif ()
endif ()

message(STATUS "Dusklight VR: configured OK")

# ============================================================
# NEXT STEP: src/dusk/vr/vr_main.cpp
#
# This file doesn't exist yet. It's the glue between the VR
# headers and m_Do_main.cpp's game loop. It needs to:
#
#   1. At startup (after aurora_initialize):
#        auto boot   = vr_xr::initialize();
#        auto session = std::make_unique<vr_xr::Session>(boot);
#        handTypeId  = aurora::gfx::register_draw_type(
#                          vr_render::handDrawDescriptor());
#
#   2. Replace the per-frame aurora_begin_frame/end_frame block:
#        session->waitFrame();
#        session->beginFrame();
#        aurora_begin_frame();
#        for (int eye = 0; eye < 2; ++eye) {
#            auto eyeParams = session->getEyeParams(eye);
#            vr_link::updateFrame(session->getFrameInput(rCtrl, lCtrl));
#            vr_render::beginEye(eyeParams);
#            // ... existing scene render call ...
#            aurora::gfx::push_custom_draw(handTypeId, &hp, sizeof(hp));
#            auto targets = vr_render::endEye();
#            session->submitEye(eye, targets);
#        }
#        aurora_end_frame();
#        session->endFrame();
#
#   3. Feed controller poses into swing detection:
#        auto event = swingDetector.update(rightControllerPose, dt);
#        if (event.triggered) { /* synthesize attack input */ }
#
#   4. On shutdown:
#        vr_link::restoreVisibility();
#        session.reset();
#        aurora::gfx::unregister_draw_type(handTypeId);
#
# When you're ready, ask for vr_main.cpp to be written.
# ============================================================
