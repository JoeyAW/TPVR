# Dusklight VR Mod — Handoff v10

## Project
Full first-person 6DOF VR mod for **Dusklight** (open-source PC port of Zelda: Twilight
Princess, `zeldaret/tp` decomp). Repo: https://github.com/TwilitRealm/dusklight
User: joeyw, Windows, builds from x64 Native Tools Command Prompt for VS 2022.
Headset: Quest 3 via Quest Link. GPU: AMD (inferred from `amdxx64.dll` worker
thread in crash stacks this session — not independently confirmed via
`dxdiag`/adapter enumeration, worth nailing down for real if it matters later).

## READ THIS FIRST — process note for whoever picks this up
This session went through a long chain of "try X → crash → find real cause →
try Y" over many turns, entirely driven by the user uploading files on
request (never assumed, never fetched blind). **Continue that pattern.** If
you need to see a file — a header, a `.cpp`, a build log, anything — ask the
user to upload or paste it directly rather than guessing its contents,
assuming a path, or reasoning from a similarly-named file you found via web
search. This session hit two real dead ends specifically *because* an
assumption looked reasonable until the real file proved it wrong (see #3 and
#4 below) — both were caught only because the actual header got uploaded
before code was written against it, not after.

## STATUS: root-caused a whole chain of runtime crashes back to a real
architectural wall: **this Dawn build cannot share GPU resources (textures
or fences) between two separate `ID3D12Device` instances on this
GPU/driver**, confirmed multiple independent ways (see below). The
"two separate devices + shared memory" design (decided v2, unchanged through
v9) is now confirmed non-viable as built. Flatscreen blanking (v9's blocker)
is FIXED and confirmed working. Nothing is visible in the headset yet — grey
flashing screen, same as v9 — but for a now fully-understood reason instead
of an unknown one.

## This session's timeline (all confirmed via DebugView + VS Output window + VS debugger, in order)

### 1. Flatscreen blanking (v9's #4) — ROOT CAUSE FOUND AND FIXED, confirmed working
**Root cause, confirmed by finally reading `m_Do_main.cpp` for the first
time this session:** the interpolating-frame branch did
`isActive() ? tick() : (normal flatscreen draw)`. `isActive()` only means a
session exists — it says nothing about whether `tick()` actually had a
gameplay view to draw. So the entire time VR was active, every frame routed
into `tick()` instead of the flatscreen draw, including menus/video/loading
screens, and `tick()` only draws anything when `isViewReady()` (v9) is true.

**Fix:** added `dusk::vr::isRenderingToHeadset()` (`vr_main.hpp`/`.cpp`) — a
flag reset to `false` at the top of every `tick()` call, set `true` only
right before the real per-eye draw loop runs (i.e. after `isViewReady()`
already passed). `m_Do_main.cpp`'s call site now always calls `tick()` while
`isActive()` (to keep pumping the XR frame loop), and separately runs the
normal `fpcM_DrawIterater`/`cAPIGph_Painter` flatscreen draw whenever
`!isActive() || !isRenderingToHeadset()`. Menus/video draw normally again;
only real per-eye headset rendering skips the flatscreen draw.

**Confirmed working** — rebuilt clean, user confirmed menus/gameplay draw
again on the flatscreen with VR active.

### 2. First real crash: `ImportSharedFence` fatal — ROOT CAUSE FOUND AND FIXED
Getting past #1 meant `isViewReady()` finally went true for the first time
ever in a headset, which reached `ensureFenceSync()` →
`wgpu::Device::ImportSharedFence()` for the first time — which crashed the
whole process. **Root cause, confirmed via VS debugger call stack + the
actual Dawn error text (not guessed):**
```
[FATAL | aurora::gpu] WebGPU error 2: FeatureName::SharedFenceDXGISharedHandle is not enabled.
```
Aurora's Dawn device (`gpu.cpp`) never requested this feature — nothing had
a reason to before VR's fence-sync path existed, and it had never actually
run before this session. Also surfaced a separate, more dangerous fact:
Aurora's `SetUncapturedErrorCallback` calls `FATAL()` (process abort) for
**any** post-init device error — so any unsupported/misused WebGPU feature
crashes the whole game, not just the VR path. Worth remembering generally.

**Fix (`gpu.cpp`/`gpu.hpp`):** request `wgpu::FeatureName::SharedFenceDXGISharedHandle`
at device creation, gated on `g_adapter.HasFeature(...)` (never assume
support — log a warning and skip if absent). Added exported
`bool g_sharedFenceDxgiSupported` so VR code can check before calling in,
since Aurora's fatal-callback policy means there's no graceful in-callback
recovery from calling an unsupported feature. `vr_xr_submit.hpp`'s
`ensureFenceSync()` now checks this flag before calling
`ImportSharedFence()` and returns early (falls back to unsynchronized
access, matching the function's original documented intent) if unsupported.

### 3. Second crash, same shape: `ImportSharedTextureMemory` fatal — root cause found, NOT fixable as first attempted
Past #2, `tick()` reached `importSwapchainImage()` →
`wgpu::Device::ImportSharedTextureMemory()` for the first time — same crash
shape:
```
[FATAL | aurora::gpu] WebGPU error 2: FeatureName::SharedTextureMemoryD3D12Resource is not enabled.
```
**First attempted fix (same pattern as #2):** requested
`SharedTextureMemoryD3D12Resource` in `gpu.cpp`, added
`g_sharedTextureMemoryD3D12Supported`. **Did not work** —
`g_adapter.HasFeature()` returned `false` for this one; the warning fired
("Adapter does not support SharedTextureMemoryD3D12Resource"), and the
crash reproduced identically once we (correctly) let it call in anyway to
confirm.

**Ruled out, in order, each with real evidence (do not re-attempt these
without new information):**
- **Feature-level gating.** Adapter is requested at
  `wgpu::FeatureLevel::Compatibility` (`gpu.cpp`). Hypothesis: Compatibility
  mode hides this feature regardless of real hardware support. **Tested**
  by temporarily switching to `Core` and rebuilding — **identical warning,
  identical crash.** Ruled out. (This edit was reverted back to
  `Compatibility` before this handoff was written — confirm the line in
  `gpu.cpp` still reads `Compatibility` before doing anything else, in case
  the wrong copy of the file gets picked up.)
- **Outdated Dawn prebuilt.** Checked Dawn's own git history:
  `SharedTextureMemory`/`SharedFence` D3D12 support landed roughly two years
  ago upstream — a mature, long-shipped feature, not something a newer
  prebuilt would freshly add. Not independently confirmed against this
  project's actual `dawn_prebuilt` version pin in `CMakeLists.txt` (that
  pin's exact value was never located/read this session) — if a next
  session wants to fully close this off, find and paste that pin.
- **`dawn::native::d3d12::ExternalImageDXGI`** (an older, non-feature-gated
  D3D12 shared-resource import API, found via Dawn git history, looked very
  promising — takes a DXGI shared handle, same pattern as the fence fix).
  **Confirmed absent from this build's actual `D3D12Backend.h`** (user
  uploaded the real header — only `GetD3D12Device`, `GetOrCreateD3D11On12Device`,
  `GetD3D12CommandQueue`, `SetExternalMemoryReservation`, and the two
  `SharedXxxMemoryD3D12ResourceDescriptor` structs are exported). Not
  shipped in this prebuilt package. Dead end.
- **Single-device architecture merge** (bind XR session to Dawn's own
  device via the already-present-but-unused `getD3D12DeviceAndQueue()`,
  instead of the separate XR-side device from `vr_xr_bootstrap.hpp`).
  **Also ruled out** — the real, uploaded `D3D12Backend.h`'s doc comment on
  `SharedTextureMemoryD3D12ResourceDescriptor` states the wrapped
  `ID3D12Resource` "must be created from the same `ID3D12Device` used in the
  `WGPUDevice`" — so it only ever wraps resources Dawn's own device already
  owns; it was never the right call for cross-device import regardless of
  the feature-support question. And the feature itself still isn't
  supported by this adapter/Dawn combo either way (confirmed same at both
  feature levels above), so merging devices wouldn't have unlocked anything.

**Conclusion, held with real confidence (not the first two crashes' "maybe
try this"):** this Dawn build cannot do zero-copy D3D12 resource/fence
sharing between separate devices on this GPU/driver, and the one
same-device-only import path that exists (`SharedTextureMemoryD3D12Resource`)
is *also* unsupported by this adapter regardless of device topology. Confirmed
three independent ways, not assumed.

## Next planned approach (NOT YET STARTED — this is where v11 should pick up)
Abandon GPU-side resource sharing entirely for the swapchain image. Do an
explicit CPU round-trip instead:
1. Dawn renders each eye into its own **normal, non-shared** offscreen
   texture (`ResolvedTargets`, already effectively how `endEye()` works) —
   ordinary WebGPU, no experimental features needed.
2. Read that texture back to CPU via a plain `MapAsync`/staging-buffer copy
   — also ordinary WebGPU.
3. Upload those pixels into the XR swapchain image via plain D3D12
   (`WriteToSubresource` or an upload heap) on the XR-side device.

This costs a real GPU→CPU→GPU round-trip per eye per frame and is not the
long-term architecture, but doesn't touch anything confirmed unavailable
above. **Not written yet — no code exists for this as of v10.** The fence
sync code (`ensureFenceSync`, now gated safely per #2 above) becomes dead
weight under this approach and should probably be removed once the copy
path is working, not before (keep it as a documented fallback in case a
smarter sharing approach becomes viable later).

## Current file state (what's actually different from v9, and confirmed how)

| File | State |
|---|---|
| `vr_main.hpp` | **This session:** added `isRenderingToHeadset()` declaration. Confirmed compiling + working (flatscreen fix, see #1). |
| `vr_main.cpp` | **This session:** added `g_renderedToHeadsetThisFrame`, reset at top of `tick()`, set `true` before the per-eye loop. Confirmed compiling + working. |
| `m_Do_main.cpp` | **This session, first time actually read (path: confirmed real, was previously never opened).** Interpolating-branch call site rewritten per #1 above. Confirmed compiling + working. Non-interpolating branch (its own pre-existing TODO) still untouched. |
| `gpu.hpp` / `gpu.cpp` (`extern/aurora/lib/webgpu/`) | **This session:** added `g_sharedFenceDxgiSupported` (working, see #2) and `g_sharedTextureMemoryD3D12Supported` (correctly reports `false` on this adapter, see #3 — the flag itself works, the underlying feature just isn't available). Both gated by real `HasFeature()` checks, both logged. The temporary `FeatureLevel::Core` diagnostic swap has been reverted back to `Compatibility` — **verify this is actually true in the live tree**, don't assume the revert made it back into the real file. |
| `vr_xr_submit.hpp` | **This session:** `#include`s `gpu.hpp` (real relative path confirmed:  `../../../extern/aurora/lib/webgpu/gpu.hpp` from `src/dusk/vr/`). `ensureFenceSync()` now checks `g_sharedFenceDxgiSupported` before calling in (working, see #2). **Still contains a temporary diagnostic:** `probeSwapchainImageShareable()`, added to test whether XR swapchain images were allocated with `D3D12_HEAP_FLAG_SHARED` (they are NOT — confirmed via `CreateSharedHandle` failing on a real swapchain image). This probe's finding is now folded into this handoff; **the probe function itself should be deleted** next session — it was diagnostic-only and its question has been answered. `importSwapchainImage()` itself is still unfixed — still calls the broken/unsupported `ImportSharedTextureMemory` path and will still crash if reached; **do not consider this file done.** |
| `vr_xr_bootstrap.hpp` | Unchanged this session. Its "outcome 2" two-device design (module header comment) is the thing now confirmed non-viable as built — worth a comment update next session once the CPU-copy approach actually lands, so the header doesn't keep describing a plan that's been superseded. |
| `vr_stereo_render.hpp` | Unchanged this session. |
| `CMakeLists.txt` | Read this session (user uploaded) — confirmed Dawn is vendored via `FetchContent` as `dawn_prebuilt`, headers land at `_deps/dawn_prebuilt-src/include/`. Exact version/tag of that prebuilt was not located — if pinned via a URL or tag elsewhere in this file, worth grepping for specifically next session. |

## Key architectural facts (carried over + new this session)
- (v9 facts about `xrCreateSession`/`XR_SESSION_STATE_READY`, `assert()` being
  inert under this `RelWithDebInfo` preset, etc. — all still true, not
  re-verified this session, not repeated in full here — see v9 for detail.)
- **NEW: this Dawn build + this adapter/driver cannot do cross-device D3D12
  resource/fence sharing at all**, confirmed three ways (see #3 above). Any
  future idea resembling "share a GPU resource between Aurora's Dawn device
  and the XR-side device" should be checked against this finding before
  being attempted again.
- **NEW: Aurora's `SetUncapturedErrorCallback` treats ANY post-init Dawn
  device error as a fatal process abort**, not a recoverable one. This
  means any code calling into Dawn with an unsupported/misused feature
  crashes the whole game, not just the calling subsystem. Always check
  `HasFeature()`-style flags before calling in; never rely on catching the
  error.
- **NEW: `dawn::native::d3d12::ExternalImageDXGI` does not exist in this
  project's vendored Dawn build.** Don't reach for it again without
  re-checking a freshly uploaded `D3D12Backend.h` first.
- **NEW: `SharedTextureMemoryD3D12ResourceDescriptor` (this build's actual
  header, confirmed via upload) only ever wraps a resource already owned by
  Dawn's own device** — it was never viable for cross-device import
  regardless of feature support. This is documented directly in the header,
  not inferred.

## Working methodology (unchanged, reinforced hard this session)
- All v7–v9 methodology notes still apply (see those handoffs).
- **NEW, the big one this session: always get the real file uploaded before
  writing code against a remembered/searched-up API surface — even when
  the API looks extremely promising and well-evidenced from search results
  (see `ExternalImageDXGI`, which looked like the answer and turned out not
  to exist in this build).** Two dead ends this session were caught cheaply
  specifically because the real header was requested and read before code
  was written, not after a wasted rewrite.
- **NEW: when ruling out a hypothesis, prefer a cheap, reversible,
  isolated experiment over a full rewrite** (the `Core` vs `Compatibility`
  feature-level test, and the `probeSwapchainImageShareable()` diagnostic,
  both answered a real question in a few lines without committing to a
  large change that might not have even been the right one).
- **NEW: mark every temporary/diagnostic change explicitly in the code
  itself** (`TEMP DIAGNOSTIC`, with what to do about it and when), and
  track them in this doc too — this session had one (the `Core` swap) that
  needed an explicit revert step and one (`probeSwapchainImageShareable()`)
  that's still sitting in the tree needing cleanup. Don't let these linger
  silently.

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```

Debugging tools that worked well this session (unchanged from v9):
```bat
devenv /debugexe C:\Users\joeyw\dusklight\build\windows-msvc-relwithdebinfo\dusklight.exe
procdump -ma -w dusklight.exe C:\Users\joeyw\dusklight\build\windows-msvc-relwithdebinfo\dusklight.exe
```
**NEW finding on logging tools:** Aurora's own logger (`[INFO | aurora::...]`,
`[FATAL | aurora::...]`, etc.) does **not** reliably show up in DebugView —
confirmed by searching DebugView for plain `"aurora"` and getting nothing,
even for a message that turned out to exist. **Use the Visual Studio Output
window (Debug → Windows → Output, "Show output from: Debug") for anything
tagged `[LEVEL | aurora::...]`.** DebugView remains correct for this
project's own `OutputDebugStringA` diagnostics (`[dusk::vr::...]` prefixed).
Also: Output-window scrollback can span multiple runs — when checking for a
specific line "from this run," scroll from the bottom/crash point, don't
trust a match found by scrolling from the top, since it may be stale from
an earlier session (this cost real time and one confusing back-and-forth
this session).

## Immediate next steps, in order
1. **Ask the user to directly upload/paste any file needed before writing
   code against it — this is the top-line instruction for this whole
   session, restated here as step 1 on purpose.**
2. Delete `probeSwapchainImageShareable()` from `vr_xr_submit.hpp` — its
   question has been answered and folded into this doc (see file-state
   table above).
3. Write the CPU round-trip copy path (Dawn texture → `MapAsync`/staging
   buffer → CPU → D3D12 upload into the XR swapchain resource on the
   XR-side device) — this is genuinely new code, nothing to adapt from an
   existing broken attempt. Ask for whatever files are needed to do this
   correctly (likely: wherever `endEye()`/`ResolvedTargets` actually lives,
   if it's not already in context — check the listing/upload state at the
   start of that session rather than assuming v9/v10's context carries
   forward).
4. Once pixels are actually reaching the XR swapchain, that's the real
   first "something visible in the headset" milestone for this whole
   project — confirm it visually before assuming correctness (wrong
   eye/orientation/format bugs are very plausible on a first real image and
   should be expected, not treated as a new mystery).
5. Grip-pose action set + `xrCreateActionSpace`, VIEW reference space
   *(carried over from v9, unchanged, still not started)*.
6. Hand mesh draw callback body *(carried over, unchanged)*.
7. Attack-trigger call site *(carried over, unchanged)*.
8. Clean up `g_ownedSession`/`xrDestroySession()` after `STOPPING`
   *(carried over from v9, unchanged, still minor/non-urgent)*.
9. Once the CPU-copy path is confirmed working, come back and update
   `vr_xr_bootstrap.hpp`'s "outcome 2" header comment — it still describes
   the two-device shared-memory plan as current design, which this session
   confirmed isn't viable as originally conceived.
