# Dusklight VR mod — working notes

Twilight Princess PC port ("dusklight") with an in-progress VR mod.

## Build workflow

- `cmake --build --preset windows-msvc-relwithdebinfo` needs the MSVC dev
  environment loaded first (plain shells fail with missing standard headers).
  Use a batch file: `call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"`
  then `cd /d C:\Users\joeyw\dusklight && cmake --build --preset windows-msvc-relwithdebinfo`,
  invoked via `cmd //c path\to\build.bat`. Don't pass a `cmd //c "... && ..."`
  string directly through git-bash — write it to a `.bat` file and invoke that.
- **Always check `tasklist //FI "IMAGENAME eq dusklight.exe"` before building**
  — if the game is running it locks the exe and the link step fails with
  `LNK1104`. Ask the user to close it first.

- **Native Linux** (added 2026-10-03, CONFIRMED in-headset on WiVRn +
  Quest 3, RX 5700 XT/RADV, GPU-direct path active): `cmake --preset linux-default-relwithdebinfo && cmake --build
  --preset linux-default-relwithdebinfo`. Needs Vulkan; the OpenXR loader is
  built from source and linked statically (no system `openxr` needed).
  Distributable x86_64 AppImage: `ci/build-linux-portable.sh` (Ubuntu 24.04
  container via podman; a host build on CachyOS needs glibc 2.44 and won't
  run elsewhere). arm64/Steam Frame AppImage (added 2026-10-19, built but
  NOT yet tested on hardware): `ci/build-linux-portable.sh aarch64` — an
  emulated arm64 container via qemu-user binfmt, about 30–60 min, output in
  `build/portable-aarch64/`. It forces `AURORA_NOD_PROVIDER=package`
  (on aarch64 "auto" would build nod from source, and Ubuntu's rustc is
  too old). Linux uses the same Vulkan XR path as Quest
  (`DUSK_VR_XR_GRAPHICS_VULKAN`); Android-only pieces are gated by
  `DUSK_VR_PLATFORM_ANDROID`. VR logs (`duskVrLog`) go to stderr on Linux.
  `.gitmodules` points `extern/aurora` at JoeyAW/aurora-vr (branch
  `aurora-vr`, fixed 2026-11-17; it used to point at encounter/aurora, which
  doesn't have the VR commits), so a plain `git submodule update --init`
  works. New aurora commits must be pushed to that branch before the
  superproject pins them, or fresh clones can't check them out.

For the VR-rendering debug loop (targeted `OutputDebugStringA` logging,
RenderDoc GPU captures) and the full VR mod status/history — root causes,
fixes, investigation trails, known open issues — see the `vr-mod-notes`
skill. Load it before resuming any VR-related debugging or feature work.

## Permanent constraints — do not revert without new evidence

Distilled from the full history in `vr-mod-notes`; read that skill for the
reasoning before touching any of this again.

- **UPDATE 2026-10-14**: object shadows are back in VR behind the
  `game.vrShadows` setting (VR tab > Appearance > "Shadows", default ON
  as of 2026-10-18 at user request,
  single-pass stereo only). CONFIRMED WORKING in-headset 2026-10-16 (empty shadow list was the last bug). New evidence for the old stretching: in VR the
  shadow maps were never rendered (`imageDrawShadow` skipped) and shadows
  were registered several times per frame. Fix and details: `vr-mod-notes`,
  "Object shadows re-enabled". The rule below still applies with the
  setting off.
- **Shadows are intentionally DISABLED in VR** (`m_Do_graphic.cpp`,
  `d_drawlist.cpp`) — tested precision fixes did NOT resolve the underlying
  stretching. Do not re-enable without new diagnostic evidence.
  **CORRECTED 2026-08-09**: this note previously covered only
  `dDlst_shadowSimple_c::draw()` (the stencil-volume shadow) — the SEPARATE
  `dDlst_shadowReal_c::draw()` (a projected-texture "blob" shadow cast by
  buildings/NPCs/props) was never actually gated, just given defensive
  frame-interp fixes; it kept drawing unconditionally in VR the whole time.
  Root-caused via a user report of "cloud shadows on the ground, camera-
  locked, moving opposite head turn" that turned out to be this, not
  weather — see `vr-mod-notes` section 23. Now disabled the same way as
  Simple shadows. Both shadow subsystems are disabled in VR as of this
  date; the "do not re-enable without new evidence" rule applies to both.
- **Don't retry the "solid-color-in-a-new-nested-offscreen-pass" approach**
  for water's reflection placeholder as originally written — it crashes
  (`begin_offscreen()`/`end_offscreen()` only track one level of pass
  nesting). Needs arbitrary-nesting-depth support first.
- **Section 10's Goron Mines heat-wave fix (in `vr-mod-notes`)**: the
  `daYkgr_c` camera-locked haze removal is still UNCONDITIONAL (affects
  flatscreen too) — an explicit one-off user choice, not a default to copy
  elsewhere. The fire-pillar (`d_a_obj_firepillar2.cpp`) particle removal
  that rode along with it was REVERSED 2026-09-21 (it made the Goron Mines
  fire spouts invisible): those spawns are back, with only the
  `"dummy"`-textured (screen-capture) sprites skipped, VR-only, via
  `dPa_control_c::checkResUsesTexture(id, "dummy")` — reuse that helper
  for any other "dummy"-texture particle instead of removing whole spawn
  sites.
- **`rotateVecByQuat()` (`vr_link_visibility.hpp`) previously computed the
  INVERSE rotation** — now fixed and confirmed. If touching hand-rotation
  math again: don't reintroduce an inverse rotation, and don't attempt a
  "column swap" to fix an axis-confusion symptom — provably cannot work
  (see `vr-mod-notes` section 12 for the algebraic proof).
- **Stray keystrokes landing in source files mid-session**: `extern/aurora/lib/gfx/common.cpp`'s
  `wait_for_gpu_progress()` has had its closing `}` corrupted multiple
  times mid-session, and on 2026-08-05 the same thing hit an unrelated
  vendored file (`_deps/xxhash-src/xxhash.h`, a literal `It` spliced into
  a line). Confirmed root cause (not a real regression, not file-specific):
  the user sometimes means to type into the terminal/chat but has a Visual
  Studio window focused instead, so the keystrokes land in whatever source
  file VS has open. If a build ever fails on a nonsensical syntax error in
  a file nobody intentionally touched, check for injected garbled text
  first and just restore the line rather than treating it as a real bug.
- **Key lesson**: don't infer that an uncommitted fix supersedes a nearby
  disable guard just because they're both present in a diff — ask, or look
  for explicit "confirmed/tested" language, before re-enabling anything.
  This bit the project once already on the shadow-stretching guard above.
