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

For the VR-rendering debug loop (targeted `OutputDebugStringA` logging,
RenderDoc GPU captures) and the full VR mod status/history — root causes,
fixes, investigation trails, known open issues — see the `vr-mod-notes`
skill. Load it before resuming any VR-related debugging or feature work.

## Permanent constraints — do not revert without new evidence

Distilled from the full history in `vr-mod-notes`; read that skill for the
reasoning before touching any of this again.

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
- **Section 10's Goron Mines heat-wave particle removal (in `vr-mod-notes`)
  is UNCONDITIONAL** (affects flatscreen too), unlike this project's usual
  VR-only-gate pattern for this bug class — an explicit one-off user choice,
  not a default to copy elsewhere.
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
