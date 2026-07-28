# Dusklight VR Mod — Handoff v8

## Project
Full first-person 6DOF VR mod for **Dusklight** (open-source PC port of Zelda: Twilight
Princess, `zeldaret/tp` decomp). Repo: https://github.com/TwilitRealm/dusklight
User: joeyw, Windows, builds from x64 Native Tools Command Prompt for VS 2022.

## Design decisions (unchanged since v1)
- Full first-person 6DOF, gesture-triggered attacks (no IK)
- Hide Link's face/hat models, keep+reposition hand model each frame
- Physical controller swing → synthetic PAD_BUTTON_A (no new combat logic)
- SteamVR / Quest Link / Virtual Desktop via OpenXR, D3D12, Aurora (WebGPU/Dawn)
- Two separate `ID3D12Device` instances (Aurora's Dawn device + a dedicated
  XR-side device), shared via `wgpu::SharedTextureMemory` + fence sync.
- **New this session: one-swapchain-vs-per-eye is now DECIDED** — single
  double-wide swapchain (`arraySize=1`, width = 2× per-eye recommended
  width), not two separate `XrSwapchain` handles. See rationale below.

## STATUS: `createSwapchain()` is now called from `startup()`. **Written this
session, NOT rebuilt or compiler-checked — no compiler available in this
session (sandboxed, no Windows/MSVC toolchain, no network access to the
repo).** Per the project's own methodology (see v7's "working methodology"
section), treat everything below as **written-but-unverified** until you
run the real build and paste the output back.

Previously (v7): `isActive()` stayed false even with a headset connected,
because `startup()` constructed a `Session` but nothing called
`createSwapchain()`. That call site now exists — see below — but until you
compile it for real, there's no confirmation it's correct.

## This session's changes (all in `vr_xr_submit.hpp` and `vr_main.cpp`)

### 1. New: `toDxgiSwapchainFormat()` in `vr_xr_submit.hpp`
`Session::createSwapchain(width, height, int64_t format)` wants a raw
`DXGI_FORMAT` value, but Aurora's `aurora::gfx::color_format()` returns a
`wgpu::TextureFormat` — the two enums don't share numeric values, so this
can't be a `reinterpret_cast`/assumption. Added:

```cpp
inline int64_t toDxgiSwapchainFormat(wgpu::TextureFormat format);
```

covering `RGBA8Unorm`, `RGBA8UnormSrgb`, `BGRA8Unorm`, `BGRA8UnormSrgb`,
`RGBA16Float`. **Throws `std::runtime_error` on anything else** rather than
silently falling back to an assumed format — a wrong-but-compiling format
here would show up as a corrupted/black headset image with no clear link
back to this call site, so it's designed to fail loudly at `startup()`
instead. If the real build throws this, that tells you which format case is
missing — add it rather than widen the fallback.

**Unverified:** the exact `wgpu::TextureFormat` value Aurora's
`color_format()` actually returns on joeyw's build was never read from
`gfx.hpp` this session (v7 only confirmed the function *exists* at
`gfx.hpp:54`, not its current return value). If it returns something not in
the switch above, `startup()` will throw and return `false` — check the
exception message / a debugger break there first before assuming something
else is wrong.

### 2. `startup()` in `vr_main.cpp` — `createSwapchain()` now called
```cpp
std::vector<XrViewConfigurationView> configViews =
    g_ownedSession->enumerateViewConfigurationViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
if (configViews.empty()) return false;

const uint32_t eyeWidth = configViews[0].recommendedImageRectWidth;
const uint32_t eyeHeight = configViews[0].recommendedImageRectHeight;
const int64_t dxgiFormat = toDxgiSwapchainFormat(aurora::gfx::color_format());

if (!g_ownedSession->createSwapchain(eyeWidth * 2, eyeHeight, dxgiFormat)) {
    return false;
}
```
Assumes both eyes report the same recommended size (true for every current
real-world OpenXR HMD runtime, but not something this session verified
against joeyw's specific runtime).

### 3. One-swapchain-vs-per-eye: decided as single double-wide
Went with a single double-wide swapchain because `Session::createSwapchain()`
(as written in v6/v7, unchanged this session) already only creates one
`XrSwapchain` with `arraySize=1`, and `tick()`'s composition-layer code
already assumed a single `g_session->swapchain()` handle shared by both
eyes. Making it double-wide (rather than leaving both eyes pointing at the
*same* image region, which was v7's actual state) was the smaller change
consistent with that existing shape. Per-eye swapchains remain a valid
alternative not taken — would require `Session` to hold two `XrSwapchain`
handles and `createSwapchain()`/`tick()` reworked accordingly.

### 4. `tick()` in `vr_main.cpp` — per-eye `imageRect` now offsets correctly
v7's code set `imageRect.offset = {0, 0}` for **both** eyes — a placeholder
bug once a real (non-double-wide-aware) swapchain existed, since both eyes
would sample the same image region. Now:
```cpp
projViews[eye].subImage.imageRect.offset = {
    static_cast<int32_t>(eye * eyeParams.width), 0};
```
left eye (index 0) → left half, right eye (index 1) → right half. Relies on
OpenXR's standard-mandated view ordering (view 0 = left, view 1 = right for
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`) — not independently re-verified
against this specific runtime this session, just the spec's stated order.

### 5. New TODO surfaced (not fixed this session): `importSwapchainImage()` call in `tick()`
Flagged, not fixed — the copy-to-swapchain path (`push_encoder_task`) is
still unimplemented dead code (`(void)targets.colorTexture; (void)swapTex;`,
unchanged from v7). But now that the swapchain is double-wide,
`importSwapchainImage(device, swapchainIndex, eyeParams.width,
eyeParams.height, ...)` describes a per-eye-sized `wgpu::Texture` view over
a resource that's actually **twice** that width. Whoever wires the real copy
needs to pass the full swapchain width plus this eye's x-offset
(`eye * eyeParams.width`) so the destination copy origin lands in the
correct half. Left as a comment at the call site rather than guessed at,
since the actual `push_encoder_task` payload shape is still an open design
question (see TODO list below, unchanged from v7).

Also changed `importSwapchainImage()`'s format argument from the previously
hardcoded `wgpu::TextureFormat::RGBA8Unorm` to `aurora::gfx::color_format()`,
so it now matches what was actually passed to `createSwapchain()` via
`toDxgiSwapchainFormat()` above — these two now can't drift apart.

## Files (all in `src/dusk/vr/` unless noted)
| File | Status |
|---|---|
| `vr_xr_bootstrap.hpp` | Unchanged since v3. Last confirmed compiling in v3's rebuild. |
| `vr_stereo_render.hpp` | Unchanged since v1. `handDrawCallback` body still a stub. |
| `vr_swing_detector.hpp` | Unchanged since v1. |
| `vr_link_visibility.hpp` | Unchanged since v1. |
| `vr_xr_submit.hpp` | **This session: added `toDxgiSwapchainFormat()`.** Fence sync still last confirmed compiling in v6. New code NOT yet compiled. |
| `vr_main.cpp` | **This session: `createSwapchain()` call wired into `startup()`; per-eye `imageRect` offset fixed for double-wide.** `isActive()`/main-loop wiring unchanged from v7 (still last confirmed compiling then). This session's new code NOT yet compiled. |
| `vr_main.hpp` | Unchanged since v7 (last confirmed compiling then). |
| `m_Do_main.cpp` (actual path unconfirmed) | Unchanged this session — VR wiring into the game loop was v7's work, last confirmed compiling then. |
| `CMakeLists_vr_fragment.cmake` | Stale/unused duplicate — unchanged, can be ignored or deleted. |

## Key architectural facts (carried over, still accurate)
- Real fence-sync types: `wgpu::SharedFence`/`SharedFenceDXGISharedHandleDescriptor`,
  confirmed in `dawn/webgpu_cpp.h` — NOT `dawn::native::d3d12::SharedFence*`
  (doesn't exist in this Dawn build).
- Fence *creation* is plain D3D12 (`ID3D12Device::CreateFence` +
  `CreateSharedHandle`) on the XR-side device; Dawn only *imports* the
  resulting `HANDLE` via `ImportSharedFence`.
- `SharedTextureMemoryEndAccessState` owns heap memory, released via its
  destructor (NOT a manual `.FreeMembers()` call — that method is private
  on this struct).
- `wgpu::TextureFormat` and `DXGI_FORMAT` are numerically unrelated enums —
  **new this session** — don't assume a cast works; use
  `toDxgiSwapchainFormat()`.
- `RequestAdapterOptionsLUID` exists, confirmed unused anywhere in Aurora —
  still an open question, unchanged since v2.

## What to actually do first, this session (v9)
1. **Rebuild and paste the full compiler output.** This is the priority —
   nothing below this session (`toDxgiSwapchainFormat`, the `createSwapchain`
   call, the `imageRect` fix) has touched a compiler yet. Likely first
   failure points if something's wrong: `aurora::gfx::color_format()`'s
   actual return type/value (see TODO #1 in `toDxgiSwapchainFormat`'s
   comment above), or an `XrViewConfigurationView` field name mismatch in
   `enumerateViewConfigurationViews()` (unchanged code, but now exercised
   from a new call site).
2. If it compiles: runtime-test with a headset connected. `isActive()`
   should now have a real chance of going true — confirm `createSwapchain()`
   actually succeeds (not just compiles) before trusting it.
3. Wire `push_encoder_task` for the swapchain copy (still not done — see
   TODO #5 above for the double-wide-aware offset it now needs).
4. Grip/view `XrSpace`s still `XR_NULL_HANDLE` (unchanged from v7).
5. Non-interpolating render branch (`fapGm_Execute()`'s internal path)
   still needs VR wired in — unchanged from v7, still pending reading
   what's inside `fapGm_Execute()`.

## `vr_main.cpp` — current TODOs
1. Grip/view `XrSpace`s still `XR_NULL_HANDLE`. *(unchanged)*
2. `startup()` still not called from anywhere in `m_Do_main.cpp`. *(unchanged since v6 — main-loop call sites were wired in v7, but confirm this specific call still exists when you rebuild)*
3. Swapchain copy still not scheduled via `push_encoder_task` — **now also
   needs the double-wide x-offset**, see TODO #5 above. *(updated this session)*
4. ~~Swapchain pixel format assumed `RGBA8Unorm`~~ — **DONE this session,
   NOT yet compiler-verified.**
5. ~~`Session::createSwapchain()` never called~~ — **DONE this session, NOT
   yet compiler-verified.**
6. ~~One-swapchain-vs-per-eye question undecided~~ — **DECIDED this session
   (double-wide single swapchain), NOT yet compiler-verified.**
7. Attack trigger still a no-op. *(unchanged)*
8. Hand mesh draw still a stub. *(unchanged)*

## `vr_xr_submit.hpp` — current TODOs
1. ~~Fence sync~~ — DONE, confirmed compiling as of v6. Runtime behavior still unverified.
2. `adapterMatchesXrRequirement()` — still unused, still an open question vs. `RequestAdapterOptionsLUID`. *(unchanged)*
3. ~~`Session::createSwapchain` format param never cross-checked~~ — **DONE
   this session via `toDxgiSwapchainFormat()`, NOT yet compiler-verified.**

## Working methodology (unchanged, still applies)
- Verify empty `findstr`/`type` results against a `dir` on the searched
  folder before trusting them.
- Chain multiple `type`/`findstr` calls per message rather than one per
  round-trip.
- Prefer a full regenerated file over a multi-location manual patch for
  anything nontrivial.
- When a session ends mid-change, the handoff must say explicitly what's
  actually been rebuilt/confirmed vs. what's written-but-unverified. **This
  entire session is the written-but-unverified case — no compiler was
  available, so nothing here has been rebuilt. Rebuild first, before
  trusting any of it.**
- When proposing a small brace-sensitive edit, recount braces in the full
  before/after block rather than trusting a local diff.

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```

## Immediate next steps, in order
1. **Rebuild. Paste full compiler output before anything else.**
2. If clean: connect a headset, runtime-test `startup()` — confirm
   `createSwapchain()` returns `true` and `isActive()` goes true.
3. Wire `push_encoder_task` for the swapchain copy, double-wide-aware.
4. Grip-pose action set + `xrCreateActionSpace`, VIEW reference space.
5. Hand mesh draw callback body.
6. Attack-trigger call site.
7. Non-interpolating render branch in `fapGm_Execute()`.
8. `adapterMatchesXrRequirement()`/`RequestAdapterOptionsLUID` decision.
