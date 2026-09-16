#pragma once

// vr_debug_log.hpp
//
// Portable stand-ins for OutputDebugStringA/_snprintf_s(buf, _TRUNCATE,
// fmt, ...), MSVC-only calls this VR mod's diagnostic logging uses
// throughout vr_main.cpp, vr_link_visibility.hpp, and vr_menu_gamepad.hpp.
// Android has neither.
//
// A near-identical pair (duskVrLog/duskVrSnprintf) already lives inside
// vr_xr_submit.hpp, added in the same pass that ported that file's Vulkan
// readback path -- NOT reused from here, deliberately, to avoid touching
// that file's already-verified-working PC build for a pure refactor.
// Pulled out into its own header here (rather than copy-pasted a third
// time) since vr_link_visibility.hpp in particular has no reason to
// depend on vr_xr_submit.hpp's much heavier Vulkan/D3D12/webgpu include
// chain just to log a string.
//
// UNVERIFIED on Android, same as the rest of this session's Vulkan work --
// no NDK on the machine that wrote it. duskVrSnprintf's logic is identical
// to vr_xr_submit.hpp's own copy, which at least compiled clean on the PC
// build (2026-09-16).

#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
#include <android/log.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdarg>
#include <cstdio>

namespace dusk::vr {

#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
inline void duskVrLog(const char* msg) {
    __android_log_print(ANDROID_LOG_INFO, "dusklight_vr", "%s", msg);
}
#else
inline void duskVrLog(const char* msg) { OutputDebugStringA(msg); }
#endif

// Matches _snprintf_s(buf, cap, _TRUNCATE, fmt, ...)'s own contract: returns
// the number of characters written (not counting the terminator) on
// success, or -1 if the output didn't fit -- some call sites rely on that
// -1 to know when to stop appending to a shared buffer.
inline int duskVrSnprintf(char* buf, size_t cap, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, cap, fmt, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        return -1;
    }
    return n;
}

}  // namespace dusk::vr
