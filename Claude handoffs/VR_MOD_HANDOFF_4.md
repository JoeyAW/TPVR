# Dusklight VR Mod — Handoff v4

## Project
Full first-person 6DOF VR mod for **Dusklight** (open-source PC port of Zelda: Twilight
Princess, `zeldaret/tp` decomp). Repo: https://github.com/TwilitRealm/dusklight
User: joeyw, Windows, builds from x64 Native Tools Command Prompt for VS 2022.

## Design decisions (unchanged since v1)
- Full first-person 6DOF, gesture-triggered attacks (no IK)
- Hide Link's face/hat models, keep+reposition hand model each frame
- Physical controller swing → synthetic PAD_BUTTON_A (no new combat logic)
- SteamVR / Quest Link / Virtual Desktop via OpenXR, D3D12, Aurora (WebGPU/Dawn)
- Confirmed architecture (v2): two separate `ID3D12Device` instances (Aurora's
  Dawn device + a dedicated XR-side device), shared via
  `wgpu::SharedTextureMemory` + fence sync.

## STATUS: as of v3, `dusklight` built and linked clean with a real
`startup()`/`tick()` frame loop. **This session's fence-sync changes to
`vr_xr_submit.hpp` have NOT been rebuilt/confirmed yet — see "What's
actually on disk right now" below before assuming this compiles.**

## ⚠️ What's actually on disk right now (read this first)
This session made fence-sync changes to `vr_xr_submit.hpp` in **two
separate passes that were NOT merged into one file**:

1. **Pass 1 (delivered as a full file, `vr_xr_submit.hpp` in the outputs
   panel)** — added `ensureFenceSync()`, real `BeginAccess`/`EndAccess`
   wiring, `FreeMembers()` call. `endAccessAll()` in this version takes an
   unused `ID3D12CommandQueue* xrQueue = nullptr` parameter with a TODO
   comment — the actual `Signal()` call was explicitly NOT implemented in
   this pass.
2. **Pass 2 (given as a snippet only, NOT as a full regenerated file)** —
   describes changing `endAccessAll`'s signature to
   `Microsoft::WRL::ComPtr<ID3D12CommandQueue> xrQueue = nullptr`, adding
   the real `xrQueue->Signal(d3dFence_.Get(), fenceValue_)` call, and
   suggested (but did not write out) storing the queue as a constructor-set
   member (`xrQueue_`) instead of a per-call parameter, plus updating
   `Session`'s constructor to accept `gfx.commandQueue` alongside
   `gfx.device`, and updating `vr_main.cpp`'s `startup()` call site to pass
   both.

**If the user pasted pass 1 into their real file, the queue-signal
completion from pass 2 is NOT in it.** Pass 2 was described in chat as a
diff, and the user asked to update the handoff instead of requesting the
full regenerated file. **The next session should regenerate the complete,
final `vr_xr_submit.hpp` (merging both passes) before anything else** —
do not assume pass 2 was hand-applied correctly; ask/confirm what's
actually in the file first, per the working methodology below.

## Files (all in `src/dusk/vr/` unless noted)
| File | Status |
|---|---|
| `vr_xr_bootstrap.hpp` | From v3, unchanged this session. Compiles (confirmed via full rebuild in v3). Has `createXrGraphicsDevice()`/`createXrSession()`. |
| `vr_stereo_render.hpp` | Unchanged since v1. `handDrawCallback` body still a TODO stub. |
| `vr_swing_detector.hpp` | Unchanged since v1. |
| `vr_link_visibility.hpp` | Unchanged since v1. |
| `vr_xr_submit.hpp` | **Mid-edit — see warning above.** Pass-1 content compiles-clean-by-inspection only (NOT rebuilt this session). Pass-2 queue-signal completion described but not written into a file. |
| `vr_main.cpp` | From v3 (confirmed building clean). **Needs a further edit** once `vr_xr_submit.hpp` is finalized: `Session` construction must pass `gfx.commandQueue` in addition to `gfx.device` (see pass 2 above) — not yet applied. |
| `CMakeLists_vr_fragment.cmake` | Stale/unused duplicate, unchanged since v3 — can be ignored or deleted. |

## Key architectural facts (carried over from v3, still accurate)
- Real fence-sync types are `wgpu::SharedFence`/`SharedFenceDXGISharedHandleDescriptor`,
  confirmed in `dawn/webgpu_cpp.h` — NOT `dawn::native::d3d12::SharedFence*`
  (doesn't exist in this Dawn build).
- Fence *creation* is plain D3D12 (`ID3D12Device::CreateFence` +
  `CreateSharedHandle`), done on the XR-side device; Dawn only *imports*
  the resulting `HANDLE` via `ImportSharedFence`.
- `SharedTextureMemoryEndAccessState` is output-only and owns heap memory
  that must be released via `.FreeMembers()` — this is now called in
  pass-1's `endAccessAll()` (previously missing/leaking, per v3).
- `RequestAdapterOptionsLUID` exists but is confirmed unused anywhere in
  Aurora — still an open question, unchanged since v3.

## Fence-sync design (as described in pass 2 — NOT yet written to disk)
For the next session to apply directly:
- `Session` gains a second constructor parameter:
  `Microsoft::WRL::ComPtr<ID3D12CommandQueue> xrQueue`, stored as a member
  `xrQueue_` alongside the existing `xrDevice_`.
- `endAccessAll()` becomes a no-arg method again (queue comes from the
  stored member, not a per-call parameter — cleaner than threading it
  through `tick()`'s call site every frame).
- After the existing `EndAccess`/`FreeMembers()` loop, if `fenceInitialized_`
  is true: `++fenceValue_; xrQueue_->Signal(d3dFence_.Get(), fenceValue_);`
  — this is what makes the *next* frame's `BeginAccess` wait meaningful,
  since it guarantees the XR-side queue actually reaches that fence value
  before Dawn's next wait is satisfied.
- `vr_main.cpp`'s `startup()` call site becomes:
  `std::make_unique<Session>(boot.instance, boot.systemId, session, localSpace, gfx.device, gfx.commandQueue);`

## `vr_main.cpp` — current TODOs (unchanged from v3 except as noted)
1. Grip/view `XrSpace`s still `XR_NULL_HANDLE` — unchanged.
2. `startup()` still not called from anywhere in `m_Do_main.cpp` — unchanged.
3. `tick()`'s real frame loop still not hooked into `m_Do_main.cpp`'s
   per-frame loop — unchanged.
4. `aurora_begin_frame()`/`aurora_end_frame()` still not called — unchanged.
5. Swapchain copy still not scheduled via `push_encoder_task` — unchanged.
6. Swapchain pixel format still assumed `RGBA8Unorm` — unchanged.
7. `Session::createSwapchain()` still never called — unchanged.
8. One-swapchain-vs-per-eye question still undecided — unchanged.
9. Attack trigger still a no-op — unchanged.
10. Hand mesh draw still a stub — unchanged.
11. **NEW**: `Session` construction call site needs updating once
    `vr_xr_submit.hpp` pass 2 is applied (see fence-sync design above).

## `vr_xr_submit.hpp` — current TODOs
1. **Finish applying pass 2** (queue member, constructor param, real
   `Signal()` call) — see design above. This is the actual next step,
   ahead of everything else, since it's mid-flight.
2. `adapterMatchesXrRequirement()` — unchanged since v2/v3, still unused,
   still an open question vs. `RequestAdapterOptionsLUID`.
3. `Session::createSwapchain` format param — still never cross-checked.
4. Device/session creation — resolved in v3, unchanged.

## Working methodology (reinforced again this session)
**New lesson this session, added to the existing list**: when a change
spans a description-only diff (chat snippet) rather than a fully
regenerated file, say so explicitly and track it as "not yet applied" —
don't let a partial/described change get silently treated as done in the
next summary or handoff. This handoff is itself an example of doing that
correctly: pass 2 is written up in full design detail above specifically
*because* it wasn't committed to a file, so the next session (or the next
context window) doesn't mistake "described in chat" for "shipped."

Everything else from v3's methodology section stands unchanged: verify
empty `findstr`/`type` results against a `dir` on the searched folder
before trusting them; chain multiple `type`/`findstr` calls per message;
prefer a full regenerated file over a multi-location manual patch for
anything nontrivial (this session broke that rule under time pressure for
pass 2 — don't repeat that).

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```

## Immediate next steps, in order
1. **Regenerate the complete, final `vr_xr_submit.hpp`**, merging pass 1
   (already delivered as a file) with pass 2's design (above) into one
   consistent file — do not assume the user hand-merged these correctly.
2. Update `vr_main.cpp`'s `Session` construction call site to match the
   new constructor signature.
3. Rebuild and confirm clean compile — this has NOT been rebuilt since
   before pass 1, so treat it as unverified until a real build log is
   pasted back.
4. Once compiling: decide the one-swapchain-vs-per-eye question, write
   the real `Session::createSwapchain()` call site with a real pixel
   format.
5. Wire `push_encoder_task` for the swapchain copy.
6. Call `dusk::vr::startup()`/`dusk::vr::tick()` from `m_Do_main.cpp`.
7. Grip-pose action set + `xrCreateActionSpace`, VIEW reference space.
8. Hand mesh draw callback body.
9. Attack-trigger call site.
10. `adapterMatchesXrRequirement()`/`RequestAdapterOptionsLUID` decision.

Always rebuild after each change and paste the full compiler output back
before proceeding to the next step.
