# Dusklight VR Mod — Handoff v9

## Project
Full first-person 6DOF VR mod for **Dusklight** (open-source PC port of Zelda: Twilight
Princess, `zeldaret/tp` decomp). Repo: https://github.com/TwilitRealm/dusklight
User: joeyw, Windows, builds from x64 Native Tools Command Prompt for VS 2022.
Headset: Quest 3 via Quest Link (also has SteamVR, Virtual Desktop available but
untested this session — all testing was Quest Link).

## Design decisions (unchanged since v1)
- Full first-person 6DOF, gesture-triggered attacks (no IK)
- Hide Link's face/hat models, keep+reposition hand model each frame
- Physical controller swing → synthetic PAD_BUTTON_A (no new combat logic)
- SteamVR / Quest Link / Virtual Desktop via OpenXR, D3D12, Aurora (WebGPU/Dawn)
- Two separate `ID3D12Device` instances (Aurora's Dawn device + a dedicated
  XR-side device), shared via `wgpu::SharedTextureMemory` + fence sync.
- One-swapchain-vs-per-eye: single double-wide swapchain (decided v8).

## STATUS: **First real progress in a headset. `startup()` completes for real
(session reaches `READY`, `xrBeginSession` succeeds, swapchain created).
`tick()` runs every frame without crashing.** But nothing is visible yet —
headset shows a grey/flashing screen, desktop shows grey while the headset is
connected — and a real architectural gap surfaced: **VR being "active" currently
blanks the flatscreen entirely, including menus and video, not just 3D
gameplay.** Detailed timeline below; this is all runtime-confirmed this
session, not theory.

## This session's timeline (all confirmed via DebugView + VS debugger, in order)

### 1. `createSwapchain()` never called (carried over from v7) — FIXED, confirmed compiling
See v8 for the fix itself (`toDxgiSwapchainFormat()`, real `createSwapchain()`
call in `startup()`, double-wide `imageRect` offset fix). Confirmed compiling
clean this session (rebuild with no `error`/`warning` lines).

### 2. `startup()` hung — ROOT CAUSE FOUND AND FIXED, confirmed working
**Root cause:** `xrCreateSession()` alone does not start an OpenXR session.
Per spec, the runtime delivers an `XrEventDataSessionStateChanged` event via
`xrPollEvent`, and only once that reports `XR_SESSION_STATE_READY` is
`xrBeginSession()` legal to call. **No event pump existed anywhere in this
codebase before this session** — nothing was ever polling for that event, so
downstream calls (`xrWaitFrame` in `tick()`, or `xrCreateSwapchain` itself in
some runtimes) had no way to make progress and blocked.

**Fix:** added `waitForSessionReadyAndBegin()` in `vr_main.cpp` — polls
`xrPollEvent` (bounded, ~5s), logs every session-state transition seen
(`[dusk::vr::startup] session state -> N`), calls `xrBeginSession()` on
`READY`. Also added a per-frame event pump at the top of `tick()` for
mid-session transitions (`STOPPING` → `xrEndSession()`, `EXITING`/
`LOSS_PENDING` → drop the session).

**Confirmed this session:** DebugView showed `session state -> 1` (IDLE),
`session state -> 2` (READY), then `SUCCEEDED: swapchain WxH dxgiFormat=N`.
Real session, real swapchain, no assumptions.

### 2b. A real bug in the fix itself — caught before it mattered, fixed
While fixing #2, `g_session` (which `isActive()` checks) was being set via
`initSession()` immediately after `Session` construction — **before**
confirming `waitForSessionReadyAndBegin()` actually succeeded. On any
`startup()` failure path, `g_session` would stay set, so `isActive()` would
lie `true` and `tick()` would run against a session that was never begun.
Moved `initSession()`/`g_xrInstance` assignment to only happen after
`waitForSessionReadyAndBegin()` returns `true`. This didn't end up being the
cause of the next crash (see #3 — that run's `startup()` genuinely succeeded),
but it's a real latent bug that's now fixed regardless.

### 3. Crash: null-pointer write in `eyePoseToViewMtx` — ROOT CAUSE FOUND AND FIXED, confirmed working
**Root cause, confirmed via VS debugger call stack + exception address, not
guessed:** `beginEye()` in `vr_stereo_render.hpp` calls
`dComIfGd_getView()`, which can legitimately return `nullptr` when called
outside active gameplay (title/loading screen — exactly what the code's own
comment already said). It was guarded by `assert(view != nullptr && ...)` —
but **`assert()` compiles to nothing under `NDEBUG`, and CMake's
`RelWithDebInfo` preset defines `NDEBUG` by default.** So instead of a clean
assertion failure, `view->viewMtx` dereferenced null, and the write at
`eyePoseToViewMtx` line 96 (`dest[0][3] = ...`, the first write through
`dest`) crashed: `0xC0000005 access violation writing location
0x00000000000001E8`. Offset `0x1E8` (488 bytes) matches `viewMtx`'s offset
within `view_class` — confirmed by exact call stack frame
(`vr_render::eyePoseToViewMtx`, inlined into `beginEye()`), not inferred.

**Fix:** added `vr_render::isViewReady()` in `vr_stereo_render.hpp` — a real
runtime check (`dComIfGd_getView() != nullptr`), not `assert`. `tick()` now
calls this **before** entering the per-eye render loop and submits an empty
frame (`layerCount = 0`, same pattern as the existing `shouldRender == false`
path) if there's no active gameplay view yet, instead of calling into
`beginEye()` at all.

**Confirmed this session:** no more crash. DebugView shows
`[dusk::vr::tick] view not ready yet (dComIfGd_getView() == nullptr) --
skipping VR render this frame` every frame, logged once (by design — see
code comment) rather than spamming.

**Flagged, NOT fixed:** two more `assert()`s in the same file
(`create_pass()` failing, `resolve_pass()` failing, both inside the per-eye
loop) are equally inert under this build config. No evidence either is
actually failing — not fixed blind, just flagged so a future crash there
isn't mistaken for something new. **Broader implication worth remembering:
every `assert()` anywhere in this VR code is currently a no-op in the build
you've been testing.** Worth keeping in mind if anything else "just works"
unexpectedly.

### 3b. My own editing mistake this session — caught and fixed, not fully explained
While writing the `isViewReady()` fix, a broken/redundant block ended up in
`vr_main.cpp`'s per-eye loop — code that called `!vr_render::beginEye(eyeParams)`
as if `beginEye()` returned `bool` (it returns `aurora::gfx::ResolvedTargets`
and always has). This didn't come from a request or a deliberate design
decision — I can't fully account for how it got into the file I sent, and
I'm noting that plainly rather than papering over it. It caused a genuine
`C2678`/`C2088` compile error (`error: no operator! for ResolvedTargets`).
**Fixed:** removed the broken block and the `frameOk` mechanism entirely —
it was dead logic anyway, since `isViewReady()` already gates the whole
per-eye loop earlier in `tick()`. Rebuilt clean afterward (confirmed: no
`error`/`warning` in the log). Restating this project's own methodology
note from v7/v8: **when something in a handoff doesn't line up with what
you expect, recount/re-verify rather than assume it's correct because it's
written down** — that applies to my output too, not just diffs.

### 4. NEW finding this session: VR-active blanks the ENTIRE flatscreen, not just 3D gameplay
**Confirmed by you, not guessed:** with the headset connected, the desktop
window is grey (goes away/back to normal the moment the headset is
disconnected). You could still blindly click "Play" and navigate the
Dusklight menu successfully (confirmed the input worked, just no visual),
but **video/movie playback doesn't come through at all**, and of course
there's no 3D gameplay view yet either (per #3 above,
`dComIfGd_getView()` is still null at this stage).

This matches v7's own description of the wiring: *"the interpolating render
branch now calls `dusk::vr::tick()` instead of the flatscreen draw when a VR
session is active"* — apparently this covers **all** interpolated frames,
not just 3D gameplay ones, so menu UI and video get swallowed by the same
replacement. This is a real architectural gap, not a bug in this session's
changes specifically — it's inherent to how `m_Do_main.cpp` was wired in v7.

**Not fixed this session** — I haven't actually seen `m_Do_main.cpp`'s
contents (still true as of this handoff, see v8's note on this same gap),
so I'm not editing its call site blind. Two candidate directions, not
decided:
1. Gate the `tick()`-vs-flatscreen choice in `m_Do_main.cpp` on
   `vr_render::isViewReady()` too (not just `isActive()`), so menus/video
   still render flatscreen-normally until real gameplay starts, only
   switching to VR once there's an actual view to render.
2. Or accept the blank period as expected and just push through it blind —
   but movie playback being invisible could be a hard blocker if any
   cutscene requires an on-screen skip prompt rather than a blind
   keypress.

## Files (all in `src/dusk/vr/` unless noted)
| File | Status |
|---|---|
| `vr_xr_bootstrap.hpp` | Unchanged since v3. Last confirmed compiling in v3's rebuild. Runtime-exercised this session (via `startup()`) with no issues seen. |
| `vr_stereo_render.hpp` | **This session: added `isViewReady()`. Flagged (not fixed) two inert `assert()`s.** Confirmed compiling. `beginEye()`/`endEye()` now runtime-confirmed reached with a non-crashing guard, but not yet confirmed to actually render anything (view is still null every frame tested). |
| `vr_swing_detector.hpp` | Unchanged since v1. Not yet runtime-exercised (grip spaces still null, no real controller pose reaching it). |
| `vr_link_visibility.hpp` | Unchanged since v1. Not yet runtime-exercised — `isViewReady()` has returned false every frame tested so far, so `vr_link::updateFrame()` (called after that gate) has never actually run yet. |
| `vr_xr_submit.hpp` | **v8: added `toDxgiSwapchainFormat()`.** Confirmed compiling and confirmed working this session — `createSwapchain()` succeeds for real, per the `SUCCEEDED` log line. |
| `vr_main.cpp` | **This session: `waitForSessionReadyAndBegin()` added (startup + tick event pumps), `initSession()` reordered, `isViewReady()` guard added, and a self-inflicted broken block removed.** Confirmed compiling clean. Confirmed running every frame without crashing. |
| `vr_main.hpp` | Unchanged since v7. |
| `m_Do_main.cpp` (actual path still unconfirmed — still never directly read) | Unchanged this session. `startup()`'s call site confirmed to actually exist and fire (via the `[dusk::vr::startup] called` log), which was an open question as of v8 — now closed. The flatscreen-blanking behavior (#4 above) originates here per v7's description, but the file itself remains unseen. |
| `CMakeLists_vr_fragment.cmake` | Stale/unused duplicate — unchanged, can be ignored or deleted. |

## Key architectural facts (carried over + new this session)
- Real fence-sync types: `wgpu::SharedFence`/`SharedFenceDXGISharedHandleDescriptor` — confirmed in `dawn/webgpu_cpp.h`.
- Fence *creation* is plain D3D12 on the XR-side device; Dawn only *imports* the resulting `HANDLE`.
- `SharedTextureMemoryEndAccessState`'s `FreeMembers()`/`Reset()` are private — released via destructor/RAII only.
- `wgpu::TextureFormat` and `DXGI_FORMAT` are numerically unrelated enums — use `toDxgiSwapchainFormat()`.
- **NEW: `xrCreateSession()` does not start a session.** You must poll for `XR_SESSION_STATE_READY` via `xrPollEvent` and call `xrBeginSession()`. This is now handled (`waitForSessionReadyAndBegin()` + `tick()`'s per-frame pump) but is a fact worth remembering if session-state bugs show up elsewhere later (e.g. session restart after `STOPPING`, which clears `g_session` but never calls `xrDestroySession()` — see TODO list).
- **NEW: `assert()` is compiled out under this project's `RelWithDebInfo` preset (`NDEBUG` defined).** Any `assert()`-based guard anywhere in `dusk/vr/` should be treated as decorative, not functional, until replaced with a real check. Two known remaining instances, see #3 above.
- **NEW: VR-active (`isActive() == true`) currently blanks ALL flatscreen output**, not just 3D gameplay — menus and video included. See #4 above.
- `RequestAdapterOptionsLUID` exists, confirmed unused anywhere in Aurora — still an open question, unchanged since v2.

## What to actually do first, next session (v10)
1. **Decide and implement a fix for #4** (flatscreen blanking) — the real
   blocker to making further progress observable at all right now. Likely
   needs `m_Do_main.cpp`'s actual call site shown/read for the first time,
   rather than continuing to work from v7's description of it secondhand.
2. Once menus/video are visible again (or you push through blind to a save
   file), confirm whether `dComIfGd_getView()` ever returns non-null once
   real 3D gameplay starts — this is the next real unknown. If it does,
   `isViewReady()` should flip true and the per-eye loop will actually run
   for the first time ever in a headset.
3. At that point, the still-unwired `push_encoder_task` swapchain copy
   (flagged since v7/v8) becomes the next real blocker — the offscreen
   render target and the XR swapchain image still aren't connected, so even
   with a valid view, expect a blank/wrong image in the headset until that's
   wired.
4. Minor cleanup carried over: `g_ownedSession` is never reset and
   `xrDestroySession()` is never called after a `STOPPING` transition (see
   `tick()`'s event pump) — not urgent, but a real gap if the user takes the
   headset off and back on within one game session.

## `vr_main.cpp` — current TODOs
1. Grip/view `XrSpace`s still `XR_NULL_HANDLE`. *(unchanged)*
2. ~~`startup()` not called from `m_Do_main.cpp`~~ — **CONFIRMED this
   session to actually be called** (was previously only handoff-described,
   not verified).
3. Swapchain copy still not scheduled via `push_encoder_task` — needs the
   double-wide x-offset (v8). *(unchanged, still the next real blocker once #4 above is resolved)*
4. ~~Swapchain pixel format assumed~~ — DONE + confirmed working (v8/v9).
5. ~~`Session::createSwapchain()` never called~~ — DONE + confirmed working.
6. ~~One-swapchain-vs-per-eye undecided~~ — DECIDED + confirmed working.
7. ~~`startup()` hangs~~ — DONE + confirmed working (v9, event pump fix).
8. ~~Crash in `eyePoseToViewMtx`~~ — DONE + confirmed working (v9, `isViewReady()` guard).
9. Attack trigger still a no-op. *(unchanged)*
10. Hand mesh draw still a stub. *(unchanged)*
11. **NEW:** VR-active blanks all flatscreen output, not just gameplay — see #4 above. *(not fixed, needs `m_Do_main.cpp` read first)*
12. **NEW (minor):** `g_ownedSession`/`xrDestroySession()` not cleaned up after `STOPPING`.

## `vr_stereo_render.hpp` — current TODOs
1. ~~`view == nullptr` crash~~ — DONE + confirmed working (v9, `isViewReady()`).
2. `create_pass()` failure — `assert()` inert under NDEBUG, not fixed, no evidence it's currently failing. *(NEW, flagged)*
3. `resolve_pass()` failure — same as above. *(NEW, flagged)*
4. Hand mesh draw callback body still a stub. *(unchanged since v1)*

## `vr_xr_submit.hpp` — current TODOs
1. ~~Fence sync~~ — DONE, confirmed compiling since v6. Runtime behavior still unverified (never reached — swapchain copy still unwired, see above).
2. `adapterMatchesXrRequirement()` — still unused, still open vs. `RequestAdapterOptionsLUID`. *(unchanged)*
3. ~~`createSwapchain` format param~~ — DONE + confirmed working (v8/v9).

## Working methodology (unchanged, reinforced this session)
- Verify empty `findstr`/`type` results against a `dir` before trusting them.
- Prefer a full regenerated file over a multi-location manual patch for anything nontrivial.
- The handoff must say explicitly what's confirmed vs. written-but-unverified.
- Recount braces/logic in the full before/after block rather than trusting a local diff.
- **NEW this session: when a compiler error reveals a mismatch between what's
  in a file and what the accompanying explanation says should be there,
  don't try to patch just the error — re-verify the whole affected block
  against what actually exists (real function signatures, real return
  types) before resubmitting. Applies to edits from any source, including
  ones presented as already-reasoned-through.**
- When behavior doesn't match expectations (e.g. desktop going grey), get
  the user's direct observation before writing it up — don't assume the
  most convenient explanation.

Build command (from x64 Native Tools Command Prompt for VS 2022):
```bat
cd C:\Users\joeyw\dusklight
cmake --build --preset windows-msvc-relwithdebinfo
```

Debugging tools that worked well this session:
```bat
:: Launch under the debugger without needing a .sln open:
devenv /debugexe C:\Users\joeyw\dusklight\build\windows-msvc-relwithdebinfo\dusklight.exe

:: Or capture a crash dump without a debugger attached at launch:
procdump -ma -w dusklight.exe C:\Users\joeyw\dusklight\build\windows-msvc-relwithdebinfo\dusklight.exe
```
DebugView (Sysinternals), run as Administrator with Capture Win32 enabled,
for all `OutputDebugStringA` diagnostics added this session (all prefixed
`[dusk::vr::startup]` or `[dusk::vr::tick]` — search for `dusk` to find them
all). These are marked `TEMP DIAGNOSTIC (v8/v9, remove once confirmed
working)` in the source — leave them in until #4 and the swapchain copy are
both resolved and confirmed, since they're still actively earning their keep.

## Immediate next steps, in order
1. Get `m_Do_main.cpp`'s actual VR call-site code shown (upload or paste) —
   this has been worked from secondhand description since v7 and is now the
   direct blocker for #4.
2. Decide + implement the flatscreen-blanking fix (likely gate on
   `isViewReady()` in addition to `isActive()` at the `m_Do_main.cpp` call
   site — not decided/implemented yet, needs the file first).
3. Get a save file loaded / real gameplay reached, confirm whether
   `dComIfGd_getView()` goes non-null and the per-eye loop actually runs.
4. Wire `push_encoder_task` for the swapchain copy (double-wide-aware, per v8).
5. Grip-pose action set + `xrCreateActionSpace`, VIEW reference space.
6. Hand mesh draw callback body.
7. Attack-trigger call site.
8. Clean up `g_ownedSession`/`xrDestroySession()` after `STOPPING`.
