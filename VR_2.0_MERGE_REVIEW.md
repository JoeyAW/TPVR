# Dusklight 2.0 merge — VR review checklist

Branch: `test/upstream-2.0-merge` (not merged into `main` yet).
Merge commit: `4aa60df281`, plus follow-up fix commits `ef9e060c6a` and
`de4825a077` (+ `0210209` inside `extern/aurora`). Builds clean
(`windows-msvc-relwithdebinfo`, confirmed end to end including mod
packaging).

**Second in-headset issue found and fixed**: "view is in the top left
corner of the screen" (VR content confined to a small corner, rest black)
— this is a real fix *I* introduced during the merge, not upstream's
fault. While resolving a large textual conflict in `extern/aurora`'s
`lib/gx/gx.cpp`, I deleted a block of functions on the theory they were
plain duplicates of content that had moved to `texture.cpp` unchanged
(true for most of them). `logical_fb_size()` got swept up in that same
deletion, but it wasn't a plain duplicate — our fork's version had an
extra condition (`&& !gfx::offscreen_uses_native_logical_size()`)
upstream's never had. Without it, VR's eye-pass viewport scaling
collapses to 1:1, producing exactly this symptom. Restored in
`de4825a077`. **This one is a good reason to be extra alert for any other
"only worked in a small corner" / "1:1 instead of scaled" style symptom**
— it's the kind of mistake (assuming duplicate-by-name without diffing
content) that could recur elsewhere in a merge this size.

**First in-headset run crashed** on Virtual Desktop/AMD RX 5700 XT —
fatal `[aurora::gpu] WebGPU error 2: Unsupported DXGI format 5a` inside
`Session::ensureSwapchainTexture` → `ImportSharedTextureMemory`. Root
cause and fix already existed in a pre-2.0-merge WIP stash
("vr submit sync fix") that got set aside at the start of this merge and
never reapplied — see `ef9e060c6a` for the reapplied fix. **This has not
yet been re-tested in-headset after the fix** — please retry the exact
same session (Virtual Desktop + this GPU) first.

Upstream 2.0 is a 403-commit release headlined by a decoupled render/simulation
architecture (fixed 30Hz sim tick, presentation can run much faster — the
"4x framerate" claim) and a rewritten per-subsystem interpolation system
(`dusk::interp::camera/material/particle/vertex/samples`, replacing the old
`dusk::frame_interp`). It also rewrote aurora's render-pass recording
internals, added a `borealis`-based application/build framework (new
submodule), and did a broad Wii/mod-UI overhaul. This required re-porting
several VR-specific fixes onto the new architecture rather than a mechanical
merge. Below is what to check in-headset before trusting this branch.

## Needs in-headset re-verification (logic ported, not yet tested live)

- **GPU-direct swapchain-copy crash (see above).** Reapplied from the
  stashed WIP: `createSwapchain()` now checks the runtime's *actual*
  D3D12 swapchain resource format (via `GetDesc()`) against what was
  requested right after `xrEnumerateSwapchainImages`, and disables the
  GPU-direct path for the session (clean fallback to CPU readback) if
  they don't match — this is permanently the case on Virtual
  Desktop/AMD RX 5700 XT (runtime allocates it typeless). Also fixed a
  second latent crash right behind it: one branch in
  `encoderTaskCallback()` checked the raw `sameDeviceAsAurora_` flag
  instead of the combined `usesGpuDirectSwapchainCopy()`. **Retest this
  exact session first** — if it doesn't crash and you see the
  `"disabling GPU-direct swapchain copy, falling back to CPU readback"`
  log line, that's expected/correct on this rig, not a new bug.
- **Water / reflection materials no longer render solid black in VR.**
  Upstream added a shared `dusk::interp::material::set_view_projection()`
  path that several water/reflection actors now route through
  (groundwater, lv3Water, lv3Water2, lv3WaterB, rstair). It built the
  reflection env-map matrix from the raw camera FOV/aspect, which would
  have reintroduced the original "water renders solid black in VR" bug.
  Fixed by routing it through `dComIfGd_getReflectionFovAspect()` (the
  existing VR-aware helper) instead. **This is the single highest-risk
  item in this merge — please check every water surface and the
  Lakebed/Zora's Domain reflections specifically.**
- **VR camera interpolation.** Upstream rewrote camera presentation
  interpolation (`dusk::interp::camera.cpp`) — game camera position/FOV
  now gets snapshotted and lerped every presentation frame, applied via
  `camera_apply_presentation()`/`camera_restore_presentation()`. Our
  camera-anchoring logic (root/core vs head-joint) sits downstream of
  this and wasn't touched, but the interpolation timing it now runs
  against changed. Check for camera judder/pop, especially at
  sim-tick boundaries.
- **Hand/item tracking freshness ("mark_live_this_frame").** Ported the
  mechanism that stops the interpolation system from overwriting VR's
  freshly-written per-frame hand/sword/shield matrices with a stale
  once-per-tick cached value. Re-implemented against the new
  `dusk::interp` core; logic is unchanged but never tested live in this
  form. Watch for the hand-lag bug returning.
- **Fresh-install VR blank splash fix.** `game_clock.cpp`'s `advance()`
  was rewritten upstream; re-applied the "force interpolation on whenever
  a VR session is active" override on top of it. Test a genuinely fresh
  `config.json` (delete it) and confirm the headset doesn't sit on a
  blank splash.
- **Fishing rod line smoothing (`d_a_mg_rod.cpp`).** The 3-way merge
  silently dropped both the interpolation callback function and its
  backing struct fields (`mLineInterpPrev/Curr/*Valid`) without flagging
  a conflict — caught only because the build failed. Restored both.
  Test fishing (both lure and bobber/uki modes) for line jitter.
- **Protected offscreen pass (VR eye-pass corruption guard).** Aurora's
  render-pass recording was substantially rewritten (single global pass →
  multi-attachment `RenderPass`). Re-implemented the mechanism that stops
  ordinary gameplay texture copies (HUD captures, minimap, shadows, etc.)
  from corrupting VR's in-progress eye render pass, against the new
  `FrameRecorder`/pass-id shape. This is what several other "black/
  corrupted in VR" bugs were traced to historically — worth broad
  in-headset testing, not just water.
- **Desktop mirror.** Re-ported `set_present_source_mirror()`/
  `clear_present_source_mirror()` onto aurora's new API. Check the
  desktop-mirror video setting still shows a live eye view.
- **`dusk::vr::tick()`'s pacing type.** Changed from the old
  `MainLoopPacer` to the new `FrameTiming` (field rename:
  `presentation_dt_seconds` → `dt`). This feeds smooth-turn rate, the
  swing detector, rod-yank timing, and Z-target-tracking elapsed time.
  Logic is a straight field-rename port, but worth confirming smooth-turn
  and weapon-swing detection still feel right.

## Confirmed safe / no action needed

- **VR shadow disables** (both `dDlst_shadowSimple_c` and
  `dDlst_shadowReal_c`) — top-level `if (g_duskVRRenderingToHeadset) return;`
  guards are untouched and still gate before any of the rewritten matrix
  code runs. Verified upstream's own refactor of the underlying stencil-
  shadow matrix helper (`get_simple_shadow_mtx()`) still has the exact
  view-matrix-mismatch bug ours was disabled for — **do not re-enable**,
  consistent with the existing CLAUDE.md guidance.
- **Water reflection nested-offscreen-pass limitation** — confirmed still
  present in upstream's rewritten aurora (`create_pass()` still explicitly
  refuses to nest: `"an offscreen pass is already active (nesting is
  unsupported)"`). The "don't retry the solid-color nested pass" guidance
  in CLAUDE.md still stands; no new evidence to revisit it.
- **"Cloud shadows" / kankyo TevKColor fix** (`d_kankyo.cpp`,
  MA00/MA01/MA16) — this file wasn't touched by upstream's interpolation
  rewrite at all; fix carried forward unmodified.

## Needs a real Quest test

- **Android/Quest OpenXR-loader JNI staging — re-ported (commit `94260fd634`).**
  Upstream deleted `platforms/android/scripts/stage-jni-libs.sh` entirely
  and moved native APK packaging into a `borealis`-provided Gradle plugin,
  which had no concept of a VR mod's extra native library. Added our own
  small CMake-generated properties file + a second Gradle staging task
  that runs alongside borealis's own. **Verified mechanically**: `cmake
  --build --preset android-arm64` then `gradlew assembleDebug` both
  succeed, and the resulting APK contains all three expected libraries
  (`libmain.so`, `libc++_shared.so`, `libopenxr_loader.so`, confirmed via
  `unzip -l` on the APK). **Not yet installed/run on an actual Quest** —
  do that before trusting this is fully fixed.

## Minor / cosmetic

- Removed the "Enable LOD Bias" debug checkbox (Debug → Graphics
  Settings) — the underlying `aurora::gx::enableLodBias` toggle no longer
  exists upstream (removed, not renamed).
- VR-motivated menu darkening (window background, tab-bar, buttons) was
  re-homed from ad-hoc literal colors into the new theme-variable system
  (`--button-background` etc. in `theme.rcss`) rather than duplicated
  per-component. Same visual result, cleaner mechanism — should look
  identical, but worth a glance at Settings/Debug menus in VR.

## Also new in 2.0 (not VR-related, FYI)

- Wii disc support (previously GameCube-only).
- A large mod-browser / settings UI overhaul (`popover.rcss`,
  `mod_browser.rcss`, `command_console.rcss`, etc.) and a `borealis`-based
  cross-platform application shell.
- Independent upstream fixes bundled in: `IndexBufferSize` was already
  bumped to match our own earlier VR-motivated value; `VertexBufferSize`
  still needed our doubling on top (VR's stereo per-frame submission is
  ~2x a flatscreen frame's data) and was reapplied.

## Still in the stash, not reapplied

The pre-merge WIP stash (`stash@{0}`, "WIP vr submit sync fix before 2.0
merge test") contained more than just the crash fixes above — it also has
a large (~370-line), separate, **Android/Vulkan-only** feature: an
`AHardwareBuffer`-based GPU-direct swapchain-copy path for Quest (Dawn's
Vulkan backend can't reuse a device the way the D3D12 path does, so it
needs its own mechanism). That's unrelated to this PC crash, untested
against 2.0's Vulkan/aurora changes, and out of scope for this merge — it
still needs its own re-port/review pass before landing. It's still sitting
safely in the stash (only the two D3D12 fixes were extracted and
reapplied); check `git stash show -p stash@{0} -- src/dusk/vr/vr_xr_submit.hpp`
when you're ready to pick that up.

## How to get this onto `main`

This branch (`test/upstream-2.0-merge`) is a throwaway merge-test branch
built off `main`, currently 1 commit ahead with everything squashed into
the single merge commit above. Once you're satisfied with in-headset
testing, say so and I'll fast-forward/merge it into `main` properly (or
rebase if you'd rather keep it as discrete commits).
