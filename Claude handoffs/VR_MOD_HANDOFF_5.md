# Dusklight VR Mod — Handoff v5

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

## STATUS: **fence sync is now fully merged into one consistent
`vr_xr_submit.hpp`** (v4 left this split across two undelivered passes —
that's resolved now). `vr_main.cpp`'s `Session` construction call site was
updated to match. **NEITHER FILE HAS BEEN REBUILT YET THIS SESSION** — the
last confirmed clean build was v3's frame-loop work. Treat this as
unverified until a real compiler log is pasted back. This is the very next
step, ahead of everything else.

## Files (all in `src/dusk/vr/` unless noted)
| File | Status |
|---|---|
| `vr_xr_bootstrap.hpp` | Unchanged since v3. Last confirmed compiling in v3's rebuild. |
| `vr_stereo_render.hpp` | Unchanged since v1. `handDrawCallback` body still a stub. |
| `vr_swing_detector.hpp` | Unchanged since v1. |
| `vr_link_visibility.hpp` | Unchanged since v1. |
| `vr_xr_submit.hpp` | **Rewritten this session — fence sync complete in one file.** `Session` constructor now takes both `xrDevice` and `xrQueue`. `ensureFenceSync()`, real `BeginAccess`/`EndAccess` wiring, `FreeMembers()`, and the `xrQueue_->Signal(...)` call are all present together (v4's pass-1/pass-2 split is resolved). **NOT YET REBUILT.** |
| `vr_main.cpp` | `Session` construction line updated to pass `gfx.device, gfx.commandQueue` (previously only `gfx.device` was ever passed — that itself would have been a compile error against the new constructor signature had it not been caught here). **NOT YET REBUILT with this change.** |
| `CMakeLists_vr_fragment.cmake` | Stale/unused duplicate — unchanged, can be ignored or deleted. |

## Key architectural facts (carried over, still accurate)
- Real fence-sync types: `wgpu::SharedFence`/`SharedFenceDXGISharedHandleDescriptor`,
  confirmed in `dawn/webgpu_cpp.h` — NOT `dawn::native::d3d12::SharedFence*`
  (doesn't exist in this Dawn build).
- Fence *creation* is plain D3D12 (`ID3D12Device::CreateFence` +
  `CreateSharedHandle`) on the XR-side device; Dawn only *imports* the
  resulting `HANDLE` via `ImportSharedFence`.
- `SharedTextureMemoryEndAccessState` owns heap memory, released via
  `.FreeMembers()` — now called every time in `endAccessAll()`.
- After `EndAccess`, the XR-side queue (`xrQueue_`) is signaled to the next
  fence value via `Signal()`, so the following frame's `BeginAccess` wait is
  against real completed work — this is the piece that was missing/TODO
  through v3 and split across two undelivered passes in v4. It is now
  written into the single `vr_xr_submit.hpp` file, unverified by a build.
- `RequestAdapterOptionsLUID` exists, confirmed unused anywhere in Aurora —
  still an open question, unchanged since v2.

## What to actually do first, this session
1. **Rebuild.** `cmake --build --preset windows-msvc-relwithdebinfo` from
   the VS2022 x64 Native Tools prompt. Paste the FULL output, success or
   failure — do not summarize or trim it. Given this is a real constructor
   signature change plus new D3D12/Dawn API surface
   (`CreateFence`/`CreateSharedHandle`/`ImportSharedFence`/fence field names
   on `BeginAccessDescriptor`), there's a real chance of a typo or field
   name mismatch that wasn't caught by inspection alone — that's expected
   and fine, paste whatever comes back rather than assuming success.
2. If it fails: fix from the real error text, not by re-guessing the API
   surface — everything used here traces back to confirmed `findstr`/`type`
   output from earlier sessions (see v3's investigation trail), so a
   failure here is more likely a typo/ordering issue (as with v3's two
   real bugs) than a wrong API assumption. Check that first.
3. If it succeeds: this closes out the single biggest correctness gap
   flagged since v2 (`vr_xr_submit.hpp` TODO #1, "no real GPU sync between
   Dawn's device and the D3D12 device backing the XR swapchain"). Update
   this doc to confirm it, then move to the next item below.

## `vr_main.cpp` — current TODOs (unchanged from v3/v4)
1. Grip/view `XrSpace`s still `XR_NULL_HANDLE`.
2. `startup()` still not called from anywhere in `m_Do_main.cpp`.
3. `tick()`'s frame loop still not hooked into `m_Do_main.cpp`'s per-frame
   loop.
4. `aurora_begin_frame()`/`aurora_end_frame()` still not called.
5. Swapchain copy still not scheduled via `push_encoder_task`.
6. Swapchain pixel format still assumed `RGBA8Unorm`.
7. `Session::createSwapchain()` still never called.
8. One-swapchain-vs-per-eye question still undecided.
9. Attack trigger still a no-op.
10. Hand mesh draw still a stub.

## `vr_xr_submit.hpp` — current TODOs
1. ~~Fence sync (queue signal completion)~~ — **DONE this session**,
   pending rebuild confirmation (see above).
2. `adapterMatchesXrRequirement()` — still unused, still an open question
   vs. `RequestAdapterOptionsLUID`.
3. `Session::createSwapchain` format param — still never cross-checked
   against Aurora's actual color format.

## Working methodology (unchanged, still applies)
- Verify empty `findstr`/`type` results against a `dir` on the searched
  folder before trusting them.
- Chain multiple `type`/`findstr` calls per message rather than one per
  round-trip.
- **Prefer a full regenerated file over a multi-location manual patch for
  anything nontrivial.** v4 broke this rule under time pressure (fence-sync
  queue completion was described as a diff instead of written into a file)
  and it created real risk of the change silently not landing — this
  session's file is the corrected, fully-merged version. Don't let a
  described-but-undelivered change get treated as done again.
- When a session ends mid-change, the handoff must say explicitly what's
  actually been rebuilt/confirmed vs. what's written-but-unverified — this
  doc's "STATUS" line and step 1 above are written that way on purpose.

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```

## Immediate next steps, in order
1. **Rebuild and confirm clean compile** of this session's
   `vr_xr_submit.hpp`/`vr_main.cpp` changes — unverified, see above.
2. Decide the one-swapchain-vs-per-eye question, write the real
   `Session::createSwapchain()` call site in `startup()` with a real
   (not assumed) pixel format.
3. Wire `push_encoder_task` for the swapchain copy.
4. Call `dusk::vr::startup()`/`dusk::vr::tick()` from `m_Do_main.cpp`.
5. Grip-pose action set + `xrCreateActionSpace`, VIEW reference space.
6. Hand mesh draw callback body.
7. Attack-trigger call site.
8. `adapterMatchesXrRequirement()`/`RequestAdapterOptionsLUID` decision.

Always rebuild after each change and paste the full compiler output back
before proceeding to the next step.
