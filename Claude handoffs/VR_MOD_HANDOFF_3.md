# Dusklight VR Mod — Handoff v3

## Project
Full first-person 6DOF VR mod for **Dusklight** (open-source PC port of Zelda: Twilight
Princess, `zeldaret/tp` decomp). Repo: https://github.com/TwilitRealm/dusklight
User: joeyw, Windows, builds from x64 Native Tools Command Prompt for VS 2022.

## Design decisions (unchanged since v1)
- Full first-person 6DOF, gesture-triggered attacks (no IK)
- Hide Link's face/hat models, keep+reposition hand model each frame
- Physical controller swing → synthetic PAD_BUTTON_A (no new combat logic)
- SteamVR / Quest Link / Virtual Desktop via OpenXR, D3D12, Aurora (WebGPU/Dawn)
- Confirmed architecture (v2): Aurora's Dawn device is independent of the XR
  runtime's required D3D12 adapter ("outcome 2") — two separate `ID3D12Device`
  instances, shared via `wgpu::SharedTextureMemory` + fence sync, not
  zero-copy same-device rendering.

## STATUS: `dusklight` (main executable, VR compiled in directly — not a
separate `.dusk` mod package) builds and links clean as of this handoff.
`cmake --build --preset windows-msvc-relwithdebinfo` completes with no
`FAILED:` lines. This does NOT mean VR rendering works at runtime yet — see
"Known-incomplete at runtime" below.

**Important build-model note (confirmed this session):** the VR code is
wired via `target_sources(dusklight PRIVATE src/dusk/vr/vr_main.cpp)` in
root `CMakeLists.txt` (lines ~804–1032) — it compiles straight into the main
`dusklight` executable, unlike `ao_mod`/`shadow_mod` which package as
separate `.dusk` files. So a successful VR build shows **no distinct output
line** in the ninja build log — just an ordinary `Linking CXX executable`
step for `dusklight` itself. Don't go looking for a `vr_mod.dusk` package;
it will never exist under the current wiring.
`CMakeLists_vr_fragment.cmake` (a separate file at repo root) is a stale,
byte-identical duplicate of the same block already in `CMakeLists.txt` —
it is NOT included/referenced anywhere and can be ignored or deleted.

## Files (all in `src/dusk/vr/` unless noted)
| File | Status |
|---|---|
| `vr_xr_bootstrap.hpp` | **Extended this session.** Original instance/system/graphics-requirements query unchanged. Added: `createXrGraphicsDevice(Bootstrap)` (real `ID3D12Device`+`ID3D12CommandQueue` on `d3d12Requirements.adapterLuid` via `EnumAdapterByLuid`+`D3D12CreateDevice`) and `createXrSession(Bootstrap, XrGraphicsDevice, XrSpace*)` (real `xrCreateSession` with `XrGraphicsBindingD3D12KHR`, plus LOCAL reference space creation). Compiles clean. NOT independently verified against the real `openxr_platform.h` field names (`XrGraphicsBindingD3D12KHR::device`/`::queue`) — standard KHR_D3D12_enable symbols, should be fine, but if a future build fails here, check that first. |
| `vr_stereo_render.hpp` | Unchanged since v1. Compiles. `vr_render::beginEye(EyeParams)`/`endEye()`, hand mesh scaffold — `handDrawCallback` body is still a TODO stub. |
| `vr_swing_detector.hpp` | Unchanged since v1. Compiles. Engine-agnostic. |
| `vr_link_visibility.hpp` | Unchanged since v1. Compiles. |
| `vr_xr_submit.hpp` | Unchanged since v2 (fence sync fields still zero-initialized — see below). |
| `vr_main.cpp` | **Rewritten this session.** Now has a real `startup()` (calls the new bootstrap functions, constructs `dusk::vr::Session`, calls `initSession()`) and a real `tick()` with `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` + composition-layer submission (previously `tick()` never called any of these — frame loop literally did not exist). Compiles clean. **Two real bugs were hit and fixed while writing this — see "Mistakes made this session" below, worth reading before trusting future multi-part patches at face value.** |
| `CMakeLists_vr_fragment.cmake` | Stale/unused duplicate — see build-model note above. |

## Key architectural facts confirmed this session (hard-won, from real headers)
- **Fence sync types exist and are real**, but NOT where earlier assumed.
  `dawn::native::d3d12::SharedFence*` types do **not** exist anywhere —
  `D3D12Backend.h` only exports `GetD3D12Device`, `GetOrCreateD3D11On12Device`,
  `GetD3D12CommandQueue`, `SetExternalMemoryReservation`, and the two
  `SharedBufferMemoryD3D12ResourceDescriptor`/`SharedTextureMemoryD3D12ResourceDescriptor`
  structs we already use. The real fence API is the base WebGPU-spec
  `wgpu::SharedFence` family, confirmed present in
  `_deps/dawn_prebuilt-src/include/dawn/webgpu_cpp.h` (NOT the small stub
  header at `include/webgpu/webgpu_cpp.h` — that one just forwards to the
  real generated headers under `include/dawn/`). On D3D12 specifically:
  `wgpu::SharedFenceDXGISharedHandleDescriptor` (has a `.handle` field, a
  Windows `HANDLE`) is what gets chained into a `SharedFenceDescriptor`
  passed to `device.ImportSharedFence(...)`.
  `SharedTextureMemoryBeginAccessDescriptor` takes `fenceCount`/`fences`/
  `signaledValueCount`/`signaledValues` (fences to wait on before Dawn
  touches the texture); `SharedTextureMemoryEndAccessState` is
  **output-only** (const fields), populated *by* `EndAccess()` with the
  fence(s) Dawn signals on completion, and **owns heap memory that must be
  released via `.FreeMembers()`** (a generated
  `wgpuSharedTextureMemoryEndAccessStateFreeMembers` exists for this) — our
  current `endAccessAll()` never calls this; that's a real leak/bug once
  fence sync is wired for real, not yet fixed.
- **Fence creation is NOT a Dawn API at all.** `D3D12Backend.h` has zero
  fence-related exports. Creating the actual D3D12 fence
  (`ID3D12Device::CreateFence` + `CreateSharedHandle` to get a `HANDLE`) is
  plain D3D12 API, done on whichever `ID3D12Device` you already have — Dawn
  only *consumes* the resulting `HANDLE` via `ImportSharedFence`.
- **This means fence-sync wiring was blocked on a real prerequisite, not
  just a missing header**: you need a second real `ID3D12Device` to create
  the fence and sync against Dawn's device. That device did not exist
  anywhere in the codebase before this session (`vr_xr_bootstrap.hpp`
  stopped at querying `d3d12Requirements`, per v2). **This is now
  unblocked** — `createXrGraphicsDevice()` (added this session) creates
  exactly that device. Fence sync wiring in `vr_xr_submit.hpp` is the
  natural next step (see "Immediate next steps").
- `RequestAdapterOptionsLUID` (in `D3DBackend.h`, allows chaining a target
  LUID into `RequestAdapter`) exists but is confirmed **unused anywhere in
  Aurora** (`extern/aurora/include`, `extern/aurora/lib` searched
  recursively, zero matches). Adapter-pinning via this struct would be new
  plumbing, not a hookup to something already there — still an open
  question whether it's worth doing instead of/alongside fence sync.
- All facts from v2 (ResolvedTargets::colorTexture fix, xrEnumerateViewConfigurationViews
  taking instance+systemId not session, GetD3D12Device/GetD3D12CommandQueue
  signatures, m_Do_main.cpp frame loop call sites) remain accurate and
  unchanged.

## Mistakes made this session (read before trusting future multi-part edits)
Being explicit about this because the working methodology depends on
catching errors rather than assuming a large patch is correct:
1. When first asked to check whether the VR mod was building, jumped to
   "no `vr_mod` target exists, something's broken" from a `--target help`
   listing, without first re-checking how `vr_main.cpp` is actually wired
   into the build (directly into `dusklight`, not a separate mod target).
   Wasted a round-trip. **Lesson embedded in this doc now** — see
   build-model note above — so this shouldn't recur.
2. When delivering the rewritten `tick()` with the real frame loop, the
   instruction to "add this above `tick()`" was ambiguous about *where
   exactly*, and the user's paste left `startup()` positioned before
   `initSession()`'s definition (used inside `startup()`) — real compile
   error (`C3861`). Separately, the old `tick()`'s dangling tail
   (`endAccessAll(); // TODO...; }`) wasn't explicitly called out for
   deletion, so it stayed in the file and produced a real second batch of
   parser errors (`C3927`/`C2146`/`C2059` etc.) at the following `}`.
   Fixed by generating the full corrected file directly instead of
   describing another manual patch. **Takeaway for future edits to
   `vr_main.cpp`/other files in this codebase: prefer generating the whole
   corrected file over describing multi-location manual edits**, since the
   user is pasting by hand and ordering/deletion instructions are easy to
   get wrong on both ends.

## `vr_main.cpp` — current TODOs (all compile as no-ops, none are errors)
1. `g_rightGripSpace`/`g_leftGripSpace`/`g_viewSpace` still `XR_NULL_HANDLE`
   — needs real `xrCreateActionSpace` (grip poses, action set setup NOT YET
   WRITTEN) and `xrCreateReferenceSpace(VIEW)`. Hands/head render at
   tracking-space origin until fixed.
2. `startup()` exists (real code, compiles) but **is not called from
   anywhere** — no call site in `m_Do_main.cpp`'s init path yet.
3. `tick()` now has a real frame loop (`xrWaitFrame`/`xrBeginFrame`/
   `xrEndFrame` + composition layer submission — NEW this session) but is
   **still not called from `m_Do_main.cpp`'s per-frame loop** — same
   integration gap as v2, just now the function itself is real instead of
   a stub.
4. `aurora_begin_frame()`/`aurora_end_frame()` still not called anywhere
   inside `tick()` — same integration note as v2 (they should stay in
   `m_Do_main.cpp`, called around wherever `dusk::vr::tick()` gets hooked
   in — not yet decided/written).
5. Swapchain copy: `Session::importSwapchainImage()` +
   `copyTextureToTexture()` exist but aren't scheduled — needs
   `aurora::gfx::register_encoder_task_type` + `push_encoder_task` wiring.
6. Swapchain pixel format still assumed `wgpu::TextureFormat::RGBA8Unorm`
   — not cross-checked against `Session::createSwapchain`'s actual format
   (which itself is never called yet — see next item).
7. **`Session::createSwapchain()` is never called anywhere** — `startup()`
   has a TODO comment marking exactly where it should go, blocked on
   knowing the real target width/height/format.
8. **NEW open architectural question**: `tick()`'s composition layer
   submission currently assumes ONE swapchain shared by both eyes (an
   array texture or double-wide layout, `imageArrayIndex = 0` for both
   `XrCompositionLayerProjectionView`s referencing the same
   `g_session->swapchain()`). If the real design ends up being one
   swapchain **per eye** instead, this is wrong and needs two swapchain
   handles threaded through. Not decided — decide this before/while
   writing `createSwapchain()`'s real call site (item 7).
9. Attack trigger: `SwingEvent::triggered` branch still empty — same as
   v2, `mDoCPd_c::getCpadInfo(PAD_1)` call site still unconfirmed this
   session.
10. Hand mesh draw: `vr_render::handDrawCallback` still a stub, same as v2.

## `vr_xr_submit.hpp` — current TODOs
1. **Fence sync — now unblocked, should probably be next.** Real types are
   confirmed (see architectural facts above). Needs: (a) `ID3D12Device::
   CreateFence` + `CreateSharedHandle` on the XR-side device (now available
   via `createXrGraphicsDevice()`'s `gfx.device`) to get a `HANDLE`, (b)
   wrap it in `wgpu::SharedFenceDXGISharedHandleDescriptor`/
   `SharedFenceDescriptor`, (c) `device.ImportSharedFence(...)` on Dawn's
   device to get a `wgpu::SharedFence`, (d) populate real
   `BeginAccessDescriptor`/`EndAccessState` fields in
   `importSwapchainImage()`/`endAccessAll()` instead of the current
   zero-initialized defaults, (e) call `.FreeMembers()` on the
   `EndAccessState` after use (currently missing entirely — will leak).
2. `adapterMatchesXrRequirement()` — unchanged since v2, written but never
   compiled/tested, no caller exists. `RequestAdapterOptionsLUID` confirmed
   unused in Aurora (see above) — open question whether adapter-pinning via
   that struct replaces the need for this function's runtime check, or
   whether both are wanted (pin AND verify).
3. `Session::createSwapchain` format param — still never cross-checked
   against Aurora's actual color format, same as v2.
4. ~~`Session` construction / `xrCreateSession` not written anywhere~~ —
   **RESOLVED this session** via `vr_xr_bootstrap.hpp`'s
   `createXrGraphicsDevice()`/`createXrSession()`.

## Working methodology (unchanged, reinforced this session)
Every fact used in code must come from something the user actually pasted
(source file contents, compiler errors, `findstr`/`type`/PowerShell output)
— guessing plausible-sounding API names/signatures wastes real cycles. This
session's `SharedFence` investigation is a good example of the pattern
working correctly: initial searches came back empty, which could have been
misread as "doesn't exist" — instead each empty result was re-verified with
a `dir` on the searched folder before being trusted, which is what caught
that `include/webgpu/` was a small stub and the real generated headers were
under `include/dawn/`. Keep doing that: an empty `findstr`/`type` result is
not evidence of absence until the searched path/folder itself is confirmed
non-empty.

Command efficiency: chain multiple `type`/`findstr` calls in one command
with `&`/`echo` separators so the user can paste one block instead of
running/pasting several — do this by default whenever more than one file
needs checking, without waiting to be asked.

For multi-location or ordering-sensitive edits to existing files
(see "Mistakes made this session" #2), prefer generating the complete
corrected file over describing a manual multi-step patch — the user pastes
by hand and ambiguous placement instructions cause real, wasted-cycle
compile errors.

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```
(Only re-run the full `cmake --preset ... -DCMAKE_TOOLCHAIN_FILE=...`
configure step if `CMakeLists.txt` itself changes or files are
added/removed — confirmed this session that header-content-only edits
don't need a reconfigure.)

## Immediate next steps, in order
1. **Wire real fence sync in `vr_xr_submit.hpp`** (now unblocked — see
   TODO #1 above). This is the single biggest correctness gap and was
   explicitly the next step before this session's device/session work took
   priority.
2. Decide the one-swapchain-vs-per-eye question (TODO #8 above), then
   write the real `Session::createSwapchain()` call site inside
   `startup()`, with a real (not assumed) pixel format.
3. Wire `push_encoder_task` for the swapchain copy (`vr_xr_submit.hpp`
   TODO #3 from v2, unchanged).
4. Call `dusk::vr::startup()` once from `m_Do_main.cpp`'s init path, and
   `dusk::vr::tick()` from its per-frame loop, replacing/branching around
   `dusk::ui::update()` per the frame-loop call-site facts confirmed in v2
   (unchanged, still not done).
5. Write grip-pose action set + `xrCreateActionSpace` calls, and confirm
   the VIEW reference space creation, to replace the `XR_NULL_HANDLE`
   stubs (TODO #1 above).
6. Implement the hand mesh draw callback body in `vr_stereo_render.hpp`.
7. Wire the attack-trigger call (TODO #9 above).
8. Decide + wire `adapterMatchesXrRequirement()`/`RequestAdapterOptionsLUID`
   (TODO #2 above) — lower priority than the above, since a wrong-GPU
   render is a correctness/perf problem, not a "nothing renders at all"
   blocker like items 1–5.

Always rebuild after each change and paste the full compiler output back
before proceeding to the next step.
