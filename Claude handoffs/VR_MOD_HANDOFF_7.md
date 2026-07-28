# Dusklight VR Mod — Handoff v7

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

## STATUS: **VR is now wired into the game's main loop and confirmed compiling.**
`dusk::vr::startup()` is called lazily on the first successful
`aurora_begin_frame()`; the interpolating render branch now calls
`dusk::vr::tick()` instead of the flatscreen draw when a VR session is
active. Full clean build confirmed
(`cmake --build --preset windows-msvc-relwithdebinfo` → success, mods
packaged). **Still not runtime-tested in a headset.** Also still open:
`createSwapchain()` is never called (TODO below), so even with a headset
connected, `isActive()` will be false until that's wired up -- `startup()`
constructs a `Session` but never creates its swapchain.

## This session's build fixes (both in `vr_xr_submit.hpp`)
1. **`SharedTextureMemoryEndAccessState::FreeMembers()` is private.**
   Confirmed against `dawn/webgpu_cpp.h` (~line 4208-4224): unlike
   `SharedBufferMemoryEndAccessState`, whose `FreeMembers` is public, this
   struct's `FreeMembers()`/`Reset()` are both private and are called
   internally by the destructor (~line 8575). Fix: **do not call
   `FreeMembers()` manually** — let `endState` fall out of scope each loop
   iteration in `endAccessAll()` and the destructor handles cleanup via
   RAII. (v5's file had called it manually, which doesn't compile.)
2. **Editing mistake, self-inflicted this session:** the fix for #1 was
   applied via a diff that left a stray extra `}` immediately after the
   `for` loop in `endAccessAll()`, which closed the function early and
   caused a large cascade of unrelated-looking errors (`pendingMemory_.clear()`
   parsed as a member declaration, `C2065 undeclared identifier` for every
   member variable, etc.). Lesson: when a small brace-count edit produces a
   big scattered error cascade far from the edit site, suspect the edit's
   brace balance first, not a language/API issue. Fixed by removing the
   duplicate `}`.

## Files (all in `src/dusk/vr/` unless noted)
| File | Status |
|---|---|
| `vr_xr_bootstrap.hpp` | Unchanged since v3. Last confirmed compiling in v3's rebuild. |
| `vr_stereo_render.hpp` | Unchanged since v1. `handDrawCallback` body still a stub. |
| `vr_swing_detector.hpp` | Unchanged since v1. |
| `vr_link_visibility.hpp` | Unchanged since v1. |
| `vr_xr_submit.hpp` | Fence sync confirmed compiling (v6). Not yet runtime-tested. |
| `vr_main.cpp` | `Session` construction confirmed compiling (v6). **This session: added `isActive()`, included new `vr_main.hpp`.** |
| `vr_main.hpp` | **New this session.** Declares `startup()`/`isActive()`/`tick()` so `m_Do_main.cpp` can call them -- didn't exist before, was a real gap. |
| `m_Do_main.cpp` (actual path unconfirmed -- found via upload, not `dir`) | **This session: VR wired in.** `startup()` called lazily on first successful `aurora_begin_frame()`. Interpolating render branch calls `vr::tick()` instead of flatscreen draw when `isActive()`. Non-interpolating branch deliberately left alone -- `fapGm_Execute()`'s internal render path unread, flagged with TODO rather than guessed. |
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
  on this struct; see this session's fix #1 above).
- After `EndAccess`, the XR-side queue (`xrQueue_`) is signaled to the next
  fence value via `Signal()`, so the following frame's `BeginAccess` wait is
  against real completed work. This code path now compiles clean.
- `RequestAdapterOptionsLUID` exists, confirmed unused anywhere in Aurora —
  still an open question, unchanged since v2.

## What to actually do first, this session
1. **Runtime-test fence sync if possible**, or move straight to the next
   architectural gap (swapchain creation, below) if a headset isn't
   available this session — a clean compile de-risks but doesn't confirm
   runtime correctness.
2. Decide the one-swapchain-vs-per-eye question, write the real
   `Session::createSwapchain()` call site in `startup()` with a real
   (not assumed) pixel format — `vr_xr_submit.hpp`'s `createSwapchain()`
   exists and compiles, but nothing calls it yet.
3. Wire `push_encoder_task` for the swapchain copy.
4. Call `dusk::vr::startup()`/`dusk::vr::tick()` from `m_Do_main.cpp` — VR
   is still fully disconnected from the game's main loop at this point.

## `vr_main.cpp` — current TODOs (unchanged from v3/v4/v5)
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
1. ~~Fence sync (queue signal completion)~~ — **DONE and confirmed
   compiling this session.** Runtime behavior still unverified.
2. `adapterMatchesXrRequirement()` — still unused, still an open question
   vs. `RequestAdapterOptionsLUID`.
3. `Session::createSwapchain` format param — still never cross-checked
   against Aurora's actual color format.

## Working methodology (unchanged, still applies — reinforced this session)
- Verify empty `findstr`/`type` results against a `dir` on the searched
  folder before trusting them.
- Chain multiple `type`/`findstr` calls per message rather than one per
  round-trip.
- Prefer a full regenerated file over a multi-location manual patch for
  anything nontrivial.
- When a session ends mid-change, the handoff must say explicitly what's
  actually been rebuilt/confirmed vs. what's written-but-unverified.
- **New this session: when proposing a small brace-sensitive edit
  (removing/adding a line inside nested braces), recount braces in the
  full before/after block rather than trusting a local diff — a
  single-brace mistake produces error cascades far from the real bug and
  wastes a round-trip.** This session's own editing mistake (stray `}`
  after the `endAccessAll()` for-loop) is the concrete example.

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```

## Immediate next steps, in order
1. **`createSwapchain()` still never called** — this is now the actual
   blocker. `startup()` constructs a `Session` but nothing calls
   `g_ownedSession->createSwapchain(width, height, format)`. Until this is
   wired up, `isActive()` stays false even with a headset connected, so
   this session's main-loop wiring has no effect yet.
2. Real pixel format for that call: `aurora::gfx::color_format()` is
   confirmed to exist (`gfx.hpp:54`, returns `wgpu::TextureFormat`) — but
   `Session::createSwapchain()` wants an `int64_t` DXGI format for
   OpenXR/D3D12, not a `wgpu::TextureFormat`. Needs a small
   `wgpu::TextureFormat` → `DXGI_FORMAT` mapping helper (not yet written)
   before this call site can be filled in for real, rather than assumed
   as `RGBA8Unorm` like the current TODO does.
3. One-swapchain-vs-per-eye question still undecided — affects the shape
   of the `createSwapchain()` call above.
4. Wire `push_encoder_task` for the swapchain copy.
5. **Non-interpolating render branch** (`fapGm_Execute()`'s internal path)
   still needs VR wired in — deliberately skipped this session pending
   reading what's inside `fapGm_Execute()`. Until this is done, VR only
   renders during interpolated frames.
6. Grip-pose action set + `xrCreateActionSpace`, VIEW reference space.
7. Hand mesh draw callback body.
8. Attack-trigger call site.
9. `adapterMatchesXrRequirement()`/`RequestAdapterOptionsLUID` decision.

Always rebuild after each change and paste the full compiler output back
before proceeding to the next step.
