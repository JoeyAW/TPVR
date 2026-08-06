---
name: vr-mod-notes
description: Full history and current status of the Dusklight VR mod — root-caused bugs, fixes, investigation trails, and known issues, organized by subsystem (water reflection, tracked hands, camera anchoring, controller input, HUD billboard, minimap, frustum culling, cross-runtime launch compat, stereo eye alignment, smooth-turn). Load before resuming any VR-related debugging or feature work in this codebase.
---

# Dusklight VR mod — working notes

Twilight Princess PC port ("dusklight") with an in-progress VR mod. This file
tracks the state of active VR debugging so a session can be resumed cleanly
after a break. It reflects the working tree as of 2026-07-30; check `git
status`/`git diff` against this list before trusting anything below, since
these are hand-maintained notes, not generated from the diff.

For the always-loaded short version of the permanent constraints below (the
"never re-enable X without new evidence" rules), see the root `CLAUDE.md` —
this skill has the full reasoning and history behind each one.

## VR rendering debug workflow

- Debugging loop that works well for VR rendering bugs: add targeted
  `OutputDebugStringA` logging (fire once or a capped handful of times, never
  every frame), rebuild, ask the user to test in the headset and paste back
  the Output-window lines (they run the game under Visual Studio's debugger
  — Debug → Attach to Process if launched via something else like
  RenderDoc — and read them from VS's Output pane). For visual symptoms hard
  to describe in words, dumping raw pixel buffers to disk (BMP → PNG) for
  direct inspection has worked well in the past (see the eye-buffer BMP
  dump tooling in `vr_xr_submit.hpp`, built 2026-07-29, generically
  reusable). Remove all diagnostic scaffolding once a bug is confirmed
  fixed.
- **When the `OutputDebugStringA`-log-and-guess loop stalls, reach for a
  RenderDoc GPU capture sooner rather than later** — it's what actually
  found the water-black root cause 2026-07-29 after many rounds of guessing.
  Gotchas specific to this project: (1) RenderDoc's own hotkey capture
  (F12) only sees the DESKTOP window's `Present()` calls, which are
  essentially empty while VR is active (the game skips its own desktop
  redraw then) — confirmed via two real captures containing nothing but a
  fence signal and a `Present`. (2) Instead, launch via RenderDoc's "Launch
  Application", then trigger a capture from in-game code: `m_Do_main.cpp`
  has an **F9 hotkey** (`getRenderDocApi()`/the `rdocCapturing` block in
  `main01()`) that brackets `StartFrameCapture`/`EndFrameCapture` around the
  actual `aurora_begin_frame()`/`aurora_end_frame()` pair, which is where
  VR's real GPU submission happens — this produced a real, useful ~634MB
  capture on the first try. `src/dusk/vr/renderdoc_app.h` (copied from the
  installed RenderDoc's own SDK header) is required for this to compile;
  it's a vendored, unmodified official header, safe to keep. (3) Debug-group
  markers (`GX_DEBUG_GROUP`, named like `dComIfGd_drawXluListInvisible` —
  handy for finding specific draw calls in the Event Browser) are OFF by
  default outside `Debug` builds — the build cache currently has
  `DUSK_GFX_DEBUG_GROUPS=ON` forced via `cmake --preset
  windows-msvc-relwithdebinfo -DDUSK_GFX_DEBUG_GROUPS=ON` (persists across
  incremental rebuilds; only needs re-passing after a fresh/deleted build
  dir). Even with that on, the markers won't show up in RenderDoc without
  `WinPixEventRuntime.dll` present (not currently in the build output) — Dawn
  silently no-ops debug markers on D3D12 without it. Without markers,
  searching the Resource Inspector by a resource's known
  width/height/format (sort or filter the Texture List) and checking its
  "Used in Frame" events is a reliable fallback for finding the right draw
  calls.

## Overall VR status

Renders correctly end-to-end as of the 2026-07-27 session: no crash, fills
the frame, correct stereo (not crossed/stretched). Water no longer renders
solid black as of 2026-07-29 (see section 3) — that specific blocking bug
is fixed and stable. Reflection *quality* is a separate, still-unresolved
follow-up: many rounds were tried 2026-07-29 (real capture with
ordering/size fixes → ghosting; solid color → stable but opaque/flat;
gradient/stripes → no visible improvement) without fully converging, and
the session was deliberately stopped there per explicit user agreement
rather than continuing to iterate blindly — see section 3's "Options for a
future session" for where to pick this up. Not a blocker, just imperfect.
Heat-wave/"kagerou" particle effects (the "floating portal duplicating the
scene" bug) are also fixed as of 2026-07-29 — see section 5, written up in
detail specifically so someone hitting the same thing in a similar engine
can follow the same approach. Goron Mines (lava dungeon) confirmed clear of
the portal too, but has a separate, not-yet-investigated visual issue —
future session, not started. VR launch itself is now confirmed working on
all three runtimes actually used for this project (SteamVR, Virtual
Desktop, Meta Link) as of 2026-07-30 — see section 6 for the OpenXR API
version / swapchain format / SteamVR color fixes involved; before that
session only Meta Link had ever been tested. The 2D HUD (hearts, rupees,
menus) now renders as a comfortable, head-locked 3D billboard (with
orientation damping so it doesn't shake with head-tracking jitter) instead
of a flat per-eye overlay glued to the lens, as of 2026-07-30 — see
section 7. The in-game HUD minimap (and, by the same fix, the pause-screen
map) no longer renders solid black with scattered color-corruption pixels
in VR, as of 2026-07-30 — see section 8; user-confirmed fixed in-headset.
Model/mesh-level frustum culling (background objects like rocks/buildings
fully disappearing when Link faces away, distinct from section 2's actor
culling) is disabled in VR as of 2026-07-30 — see section 9; built
successfully but **not yet confirmed in-headset**.
Goron Mines' own "heat wave" effect (visually similar to section 5's
floating-portal bug but a separate follow-up, previously unidentified) is
root-caused and fixed as of 2026-07-31 — see section 10; user-confirmed
gone in-headset. Note this section 10 fix **removes the effect entirely**
(both VR and flatscreen) rather than VR-gating it like section 5's fixes —
an explicit user choice for this specific location, not the project's
general VR-bug-fix pattern.
The VR camera is now anchored to Link's actual head position (true
first-person) during normal human-form gameplay, instead of the old
third-person-eye-plus-HMD-delta composition, as of 2026-07-31 — see
section 11; user-confirmed smooth in-headset, including the
form-dependent third-person fallback for Wolf Link. **Not yet tested**:
Epona (horse) riding or snowboarding — see section 11's gaps.
Tracked VR hands (driving `mpLinkHandModel` from real controller poses,
the last of the three-part VR embodiment plan) got real OpenXR controller
input wired up for the first time as of 2026-07-31 — see section 12.
**Position tracking is fixed and confirmed working in-headset.**
**Rotation is now FULLY FIXED AND CONFIRMED as of 2026-08-02**, after
three sessions of attempts (2026-07-31, its continuation, and this final
session). Two genuinely separate bugs were involved, both now resolved:
(1) `rotateVecByQuat()` was computing the INVERSE rotation instead of the
forward one (verified numerically against a reference implementation) —
this explained why every previous "verified correct" static correction
still came out wrong in general headset movement, since it was being
composed with a rotation that ran backwards; and (2) once that was fixed,
a residual static offset (a fixed rotation between the calibrated local
axes and how the mesh's own facing/palm/thumb axes are actually authored)
needed one final calibration pass, ultimately solved by testing all three
possible single-axis rotation planes against real in-headset photos taken
at multiple different controller orientations, which is what finally
confirmed a fix that holds up across general movement rather than just
one reference pose. Section 12 has the full history — genuinely one of
the hardest bugs in this project, worth reading in full before attempting
anything similar (rotation calibration, quaternion conventions, or
photo/data-based derivation of a fixed correction) elsewhere in this
codebase. **Left hand is now also fully calibrated and confirmed working
in-headset, as of 2026-08-03** — turned out much simpler than the right
hand (identity local axes + live in-headset iteration on the static
offset, no aim-pose data capture needed), see section 12's left-hand
writeup. This closes out the three-part VR embodiment plan (camera,
arms/ears/hat hiding, tracked hands) entirely. Separately, Quest 3
controllers now also drive real GAMEPLAY input (movement, camera, attack,
items, pause) — not just hand visuals — as of 2026-08-03, user-confirmed
working in-headset; see section 13, including a genuinely tricky root
cause (three independent, unrelated code paths all clearing the same
shared virtual-pad slot every frame, from a touch-screen-overlay system
that predates VR and never anticipated a second writer). A swing-gesture
attack was drafted but explicitly deferred to a future session. Stereo
eyes misaligning at large head yaw ("left/right eyes look swapped" near
90°) is fixed and user-confirmed in-headset as of 2026-08-05 — see
section 14, including a first fix attempt that regressed (reversed
pitch/yaw entirely) and was caught and reverted same-session before the
real fix landed; genuinely worth reading before touching
`eyePoseToViewMtx()`'s rotation math again. The right thumbstick now
drives VR smooth-turn (comfort camera rotation) instead of the flatscreen
C-stick, unbinding the latter per explicit user request — see section 15;
user-confirmed working in-headset same session. The sword and shield now
track the real controllers (via the same tracked-hand matrices as
mpLinkHandModel) instead of floating at Link's flatscreen third-person
hand position — see section 16; built successfully, **not yet confirmed
in-headset**. Other known issues below.

## Currently uncommitted working-tree changes

`extern/aurora` (submodule, dirty working tree — NOT yet committed even
inside the submodule):
- `include/aurora/gfx.hpp` / `lib/gfx/common.cpp`: `begin_offscreen()`/
  `end_offscreen()` now suspend-and-resume correctly when a NEW offscreen
  pass (`GXCreateFrameBuffer`) opens while a *protected* offscreen pass (a VR
  eye) is already active, instead of silently losing track of it. This was
  written to fix "water/heat-wave indirect-distortion effects rendering
  solid black in VR" but **turned out not to be the actual cause of the
  black-water bug** (see below) — it's still a real, correct fix for genuine
  `GXCreateFrameBuffer`-nesting-under-VR cases (e.g. if the bloom/DOF guards
  below are ever removed), just not sufficient by itself for water.

Main repo, by topic:

### 1. Shadows — intentionally DISABLED in VR, do not re-enable without new evidence
- `src/m_Do/m_Do_graphic.cpp`: `GX_DEBUG_GROUP(dComIfGd_drawShadow, ...)` call
  guarded by `!dusk::vr::isRenderingToHeadset()`.
- `src/d/d_drawlist.cpp`: `dDlst_shadowSimple_c::draw()` early-returns when
  `g_duskVRRenderingToHeadset`.
- Also in `d_drawlist.cpp`: double-precision fixes for
  `dDlst_shadowReal_c::setShadowRealMtx()`'s view*proj concat, and in
  `vr_stereo_render.hpp`'s `eyePoseToViewMtx()` (translation math), targeting
  float32 precision loss at this game's huge (~30,000–100,000+ unit) world
  coordinates. **These were tried and confirmed (by testing) to NOT fix the
  visible stretching.** A past session mistakenly re-enabled shadows
  reasoning "the fix landed, the disable must be stale" — it hadn't, and
  re-enabling just reproduced the same stretching. **Lesson: do not infer
  that a nearby fix supersedes a disable guard without checking — ask, or
  look for an explicit "confirmed still broken" comment first.**
- Status: shadows stay off in VR until someone picks this up with fresh
  diagnostics. The precision fixes are harmless and can stay.

### 2. Actor-culling — fixed
- `vr_stereo_render.hpp`'s `beginEye()`: `mDoLib_clipper` (actor visibility
  frustum) is rebuilt every eye from the real asymmetric VR FOV (smallest
  symmetric frustum that fully contains it), instead of the stale flatscreen
  camera's ~60°/1.357 values. Fixed the "objects fade in/out near the edge of
  the visor" issue.
- Deliberately does **not** touch `view->fovy`/`view->aspect` themselves,
  since those also drive particle billboarding, rain shadow-projection,
  audio spatialization, and the modding API — scoped to just the clipper via
  a parallel computation, to avoid side effects on those other systems.

### 3. Water rendering solid black in VR — ROOT-CAUSED AND FIXED 2026-07-29
**FULLY RESOLVED 2026-07-29 (including the reflection-quality follow-up
below) — read this box first, the rest of the section is historical detail
kept for context/lessons, not current status.**

Both the original black-water bug AND the follow-up "reflection looks bad"
problem are fixed. Final state:

- Water's own reflective-surface material (`MA02`/`MA10`) is now **skipped
  entirely in VR** — it never draws at all. Fix lives in
  `d_com_inf_game.cpp`'s `dComIfGd_drawXluListInvisible()`/
  `drawOpaListInvisible()`: added `&& !g_duskVRRenderingToHeadset` to the
  existing condition that already gated these draws behind the
  player-facing "Disable Water Refraction" ImGui checkbox
  (`dusk::getSettings().game.disableWaterRefraction`,
  `ImGuiMenuTools.cpp`) — confirmed live via that exact checkbox before
  baking it into VR permanently. What you see in VR now is the water's base
  layer (diffuse + foam/wave texture, still animated) with no reflective
  overlay — no black, no opaque placeholder color, just no reflection
  effect. **This is what finally fixed the "opaque, no transparency, wrong
  color" symptom** — not a texture-content or blend-state fix, an
  "don't draw this layer in VR at all" fix. See Round 9's writeup below for
  the (extensive, ultimately unnecessary for the final fix) investigation
  that preceded finding this.
- The shared screen-capture texture (`mDoGph_gInf_c::getFrameBufferTex()`)
  is back to real content in VR (`retry_captue_frame()`'s VR guard removed
  at its main call site in `m_Do_graphic.cpp`) instead of the diagnostic
  gradient/stripe placeholder — needed because the underwater motion-blur
  effect (`motionBlure()`, samples the same shared texture) was still
  showing the stripe placeholder when the camera went underwater, even
  after water's own draw was skipped. Real capture is fine for blur (no
  geometric-accuracy requirement the way a reflection has), even though it
  wasn't good enough for water's reflection specifically.
- **Cleanup performed same session**: removed the now-unused
  `captureGradientCornerReflection()` function entirely
  (`m_Do_graphic.cpp`) and all the round 2/4-11 diagnostic
  `OutputDebugStringA` logging + inert magenta/blend-override test code from
  `d_kankyo.cpp`'s `dKy_bg_MAxx_proc()` (none of it ended up load-bearing
  for the actual fix — the real fix was found by looking at the draw-list
  gating in `d_com_inf_game.cpp`, unrelated to anything that diagnostic
  code was investigating). The Round 1 reflection-matrix/fovy-aspect fix
  (`dComIfGd_getReflectionFovAspect()` and its call sites) was **left in
  place** — it's still correct, just currently moot for VR since the
  material it feeds never draws there anymore; harmless to leave, and would
  matter again if water's VR draw is ever re-enabled.
- **Known residual risk, not yet an observed problem**: the Invisible list
  these draw functions gate may hold content besides water ("among other
  things" per the drawn-list tracing below) — skipping it wholesale for VR
  could theoretically be hiding something else too. User tested both
  surface water and underwater after this fix with no other regressions
  noticed, but this wasn't exhaustively audited. If something unrelated
  looks off in VR later, check here first.

**Original symptom** (user-confirmed): ALL water in the game — lakes,
rivers, waterfalls, still or flowing — rendered as pitch black, fully
opaque, no visible texture, only in VR (fine on flatscreen). **Now fixed**:
water shows real (if visually imperfect — see "Remaining issue" below)
content in VR instead of solid black, confirmed by the user in-headset
after the fix below, at the exact camera angle from the original bug
report. No regressions found in the "black screen after loading a save"
fix this touches (user explicitly re-tested loading a save after this fix
— still works).

**Actual root cause** (found via RenderDoc GPU capture, see the long
investigation trail below for everything that was ruled out first): this
game's water does NOT have a real per-surface reflection asset or a real
planar reflection render — it uses the cheap, old-game trick of sampling a
recent **screen capture** (the scene as it looked a moment ago), distorted
through the UV-generation matrix already fixed in Round 1, to fake a
shimmering reflection. That screen capture is `mDoGph_gInf_c::
getFrameBufferTex()` (a shared, single 304×224 texture —
`FB_WIDTH_BASE/2 × FB_HEIGHT_BASE/2`, confirmed via RenderDoc's Resource
Inspector matching the exact dimensions found for water's bound texture in
Round 8's read-only introspection), populated by `retry_captue_frame()`
(`m_Do_graphic.cpp`) via a `GXCopyTex`.

`retry_captue_frame()` was unconditionally skipped in VR at every call site
(`if (!dusk::vr::isRenderingToHeadset()) { retry_captue_frame(...); }`) —
guards added in an earlier session to fix a *different* bug ("black screen
after loading a save"), because the underlying `GXCopyTex` → `resolve_pass_
into()` substitution used to unconditionally corrupt VR's protected eye
pass if it ran mid-eye-render. Skipping it fixed that bug but, as an
unintended side effect, meant this shared screen-capture texture never got
written to during VR at all — leaving it permanently at whatever it was
initialized to. This is exactly why the user recalled water used to show
"an attempt to display what my view was in the headset" (a stale/frozen
screen capture, from before these guards existed) and only went solid black
after that fix landed.

**The fix (two parts, both required — first alone was NOT sufficient,
confirmed by testing)**:
1. `extern/aurora/lib/gfx/common.cpp`, `resolve_pass_into()`: previously
   this dropped the entire substitution (returned early, doing nothing) if
   the current pass was VR's protected eye pass. Changed to let the
   substitution through. Verified safe because: the substitution always
   carries the same `colorView`/`depthStencilView` forward onto the new pass
   object it creates (only the pass *wrapper* gets a new id, not the actual
   render target) — and `resolve_pass_checked()` (used by `endEye()` to
   close out the eye pass afterward) already had a fallback for exactly this
   case (id mismatch alone isn't treated as failure if `colorView` still
   matches). After the split, `g_protectedOffscreenPassId` is updated to
   the new pass's id so other internal checks (e.g. `begin_offscreen()`'s
   own nesting detection) don't lose track of it either.
2. `m_Do_graphic.cpp`: re-enabled the
   `retry_captue_frame()` call site right before the Invisible-list draw
   (~line 2629 pre-this-session, the one that runs unconditionally every
   frame aside from the VR guard) by removing its `!dusk::vr::
   isRenderingToHeadset()` guard entirely. This is the ONE call site
   confirmed sufficient to fix water — the other 3 guarded call sites
   (`F_SP124`-stage-specific, bloom's, and the `#if DEBUG` darkworld one —
   see section 4) were deliberately left alone; not yet confirmed necessary
   or safe to also re-enable.

**Remaining issue (NOT the black bug — a visual quality/accuracy problem)
— STOPPED after many rounds on 2026-07-29 per explicit user agreement
("one more focused attempt, but if it doesn't land, stop regardless"), then
RESUMED later the same day: user explicitly ruled out the second-camera
option for now ("how good can it look without the second camera, by
troubleshooting alpha") and asked to keep fixing bugs with changes kept
easy to revert. See "Round 9" below for what was found/changed on resume —
picking up with RenderDoc inspection (the top item in "Options for a future
session" below) rather than more blind guessing.

Full history of what was tried, in order:

- **Ghosting round 1** (per-eye capture ordering): confirmed the reflection
  showed REAL content once the black bug was fixed, but with duplicated
  ghost copies of Link's own model and wrong crops (a band of sky where
  rock should be). Root cause found: `retry_captue_frame()`'s capture for a
  given eye was being read by that SAME eye's water draw only if it ran
  AFTER the capture in `mDoGph_Painter()`'s per-frame sequence — depending
  on `g_env_light.is_blure`, water's Invisible-list draw could happen
  BEFORE that frame's capture, reading the OTHER eye's (or previous
  frame's) capture instead — a view offset by the full stereo
  interpupillary distance. **Fixed**: added a VR-only early capture call
  right before the first Invisible-list draw check, so water always reads
  at-worst-one-eye-stale data instead of a different eye's view outright.
  Improved things ("a little better") but did not fully fix the ghosting.
- **Destination-size bug** (found alongside the above): `retry_captue_frame()`
  on the `TARGET_PC` path passed the FULL captured width/height to
  `GXSetTexCopyDst()`, while the real destination texture
  (`mDoGph_gInf_c::getFrameBufferTex()`) is allocated at HALF that size —
  the non-PC branch already used the half-size values
  (`var_r24`/`var_r23`) the PC branch computed but never used. **Fixed** by
  using the half-size values on the PC path too. Confirmed as a real bug
  (destination size now matches the real texture), but did not resolve the
  ghosting on its own either.
- **User's diagnosis** (correct, and the reason further real-capture fixes
  were abandoned): a VR headset moves in ways a flatscreen third-person
  camera never does (free rotation, no camera-position clamping, quick
  head turns) — this screen-capture-as-reflection technique fundamentally
  assumes smooth, bounded camera motion. Chasing every VR-specific
  mismatch this exposes has diminishing returns; a placeholder approach
  was tried instead of continuing to patch the real capture.
- **Solid-color-in-a-new-offscreen-pass (CRASHED, do not retry as
  written)**: tried opening a NEW nested offscreen pass (`GXCreateFrameBuffer`)
  to draw an exact solid color while already inside VR's protected eye
  pass. This is a genuine second-level-of-nesting case: the `GXCopyTex`
  inside that nested pass triggers `resolve_pass_into()`'s ordinary
  pass-substitution, which `begin_offscreen()`/`end_offscreen()`'s
  single-slot suspend/resume mechanism (`extern/aurora/lib/gfx/common.cpp`,
  comment: "Only one level of nesting is tracked") does not account for —
  corrupts which pass index gets resumed, crashes on the next
  `SetViewport` call with "Attempted to append command SetViewport to
  sealed render pass". **Do not retry this exact approach** without first
  extending that suspend/resume logic for arbitrary nesting depth (bigger,
  riskier change than this fallback is worth) — reverted immediately.
- **Tiny-real-scene-corner capture (reverted, didn't crash, didn't work)**:
  tried capturing a real but tiny (16×16 logical pixel) corner of the
  actual scene instead of the whole view, theorizing it'd look
  "blurred/uniform" once scaled up. In practice the corner still contains
  real (if blocky) scene content and visibly changed color with head
  movement — not an improvement.
- **Solid-color quad drawn directly into the current pass (no crash, but
  flat/lifeless)**: switched to drawing an actual solid-color quad directly
  into the ALREADY-OPEN eye pass (no new pass at all — avoids the crash
  above entirely), in a small corner, then capturing just that quad. This
  worked without crashing and gave a stable (non-flashing) color, but the
  user correctly identified two problems: (1) fully opaque, no
  see-through/transparency (confirmed later that flatscreen water genuinely
  IS see-through to the lakebed — this is a real regression, not a
  pre-existing design limit); (2) a perfectly uniform color gives the
  water's own wind-driven UV distortion animation (`dKyw_get_wind_vec()`,
  `d_kankyo.cpp`) nothing to reveal — sampling any distorted UV of a
  uniform color returns that same color, so it looked like a dead flat
  square with no waves at all.
- **Transparency investigation**: lowered the placeholder's alpha
  (255 → 130) to test whether the material's blend reads texture alpha.
  **Zero visible effect** — confirmed the shared capture texture's format
  genuinely supports alpha (`GX_TF_RGBA8`, checked directly in
  `mDoGph_gInf_c`'s init code, so it's not a format limitation), meaning
  this material's translucency (real on flatscreen) comes from something
  other than straightforward texture alpha that hasn't been identified —
  possibly the same class of "needs `patch()`/`diff()` to reach the GPU"
  issue as Round 4-7's TevColor/blend experiments in `d_kankyo.cpp`, or a
  genuinely different mechanism not yet investigated. **Unresolved.** Also
  likely explains the "goes solid blue near/underwater, can't see
  anything" report — same fixed-opacity surface, just filling more of the
  view when the VR camera gets close to or through the water plane (which
  happens more in VR than flatscreen, since head movement isn't clamped
  like a flatscreen camera's position).
- **Gradient instead of solid color (no visible difference)**: per-vertex
  2-color diagonal gradient (`GX_SRC_VTX`/`GX_CC_RASC`) instead of one flat
  KONST color, theorizing the UV distortion would reveal movement in the
  gradient. **No visible difference reported** — a smooth gradient varies
  too gradually across space for the water's actual (small) per-frame UV
  shift to move anything a viewer would notice.
- **Alternating stripes instead of a gradient**: 8 thin alternating-color
  vertical stripes instead of one smooth gradient, theorizing high-frequency
  detail would make the same small UV shift visibly move a stripe boundary.
  Result: one large washed-out pale band, not visible individual stripes —
  diagnosed as a NEW bug: the capture was being routed through
  `retry_captue_frame()` with a fake tiny (16×16) `view_port_class`, and
  that function computes its destination copy size as `width >> 1` of
  WHATEVER width/height it's given (correct for its normal
  608×448→304×224 use, but with a 16×16 fake source this told the copy
  system the destination was only 8×8 — not the real 304×224 texture), so
  the striped quad likely only ever filled a small corner of the real
  destination, leaving the rest at stale/default content.
- **Focused fix attempt (per explicit user agreement to try once more,
  then stop regardless) — did NOT resolve it**: bypassed
  `retry_captue_frame()` entirely for this capture; call
  `GXSetTexCopySrc`/`GXSetTexCopyDst`/`GXCopyTex` directly with the real
  destination texture's own dimensions
  (`mDoGph_gInf_c::getFrameBufferTimg()->width`/`height`) instead of a
  derived-from-fake-source size. User-reported result: **"same size"** —
  no visible change. This means the destination-size mismatch, while a
  real and now-fixed bug, was NOT the (or not the only) cause of the
  washed-out appearance. The actual remaining cause of "stripes don't show
  up as distinct stripes" is still unidentified.

**Round 9 (resumed session, 2026-07-29 — dstAlpha theory ruled out, real
alpha=0 bug found and fixed via RenderDoc, NOT yet retested in-headset)**:

- **dstAlpha override theory (ruled out)**: `GXCopyTex`'s destination-alpha
  path (`extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp`'s `copy_tex()`) can
  force a copied texture's alpha to a constant `dstAlpha` value, which
  would've explained "changing alpha did nothing." Checked: `dstAlpha`
  defaults disabled (`GXSetDstAlpha(GX_DISABLE, 0)` in `GXInit`,
  `libs/dolphin/src/gx/GXInit.c`) and nothing in game code or the J3D
  material system ever calls `GXSetDstAlpha` (only an unrelated particle
  file, `JPABaseShape.cpp`, references it at all). This mechanism never
  fires here — ruled out, not just abandoned.
- **Reflection texture is NOT 304×224 in VR — it's scaled non-uniformly**:
  confirmed via RenderDoc's Resource Inspector: the actual GPU resource is
  `Dawn_InternalTexture_Resolved Texture`, 1032×1136, B8G8R8A8_UNORM — not
  304×224. Root cause: `scale_copy_dst()` (same file as above) scales every
  `GXCopyTex` destination by `(real render target size) / (logical fb
  size)`. VR's eye rendering (`vr_stereo_render.hpp:447`,
  `set_offscreen_uses_native_logical_size(true)`, reset `false` at line 520)
  deliberately makes `logical_fb_size()` report the small flatscreen-native
  size instead of the real (large) eye-target size *while inside the eye
  pass*, specifically so the normal flatscreen draw code's viewport calls
  scale up to fill the real eye texture — but this same override also
  applies to our 304×224 capture request, scaling it up too. The non-square
  result (1032×1136, not a uniform multiple of 304×224 in both dimensions)
  is consistent with the VR headset's per-eye aspect ratio differing from
  the flat game's internal ~608×448 aspect — expected once you know the
  mechanism, not itself a bug. **Lesson for future sessions**: don't assume
  a `GXSetTexCopyDst`-requested size is the real allocated size while inside
  a VR eye pass — check via RenderDoc, or via
  `aurora::gfx::get_render_target_size()`/`aurora::gx::logical_fb_size()` if
  reasoning about it in code.
- **Real alpha=0 bug found and fixed (root cause of "changing the
  placeholder's alpha had zero visible effect", from the section above)**:
  opened the actual 1032×1136 texture in RenderDoc's Texture Viewer.
  Confirmed via a direct pixel readout — position (938, 1093), RGBA
  (0.47059, 0.68627, 0.78431, **0.00**) — a pixel whose RGB clearly matches
  one of the stripe colors, but alpha is exactly 0. Root cause: `GXSetBlendMode()`
  (`extern/aurora/lib/dolphin/gx/GXPixel.cpp:114`) only writes the blend
  enable/src/dst/op bits of the shared `cmode0` BP register — it does NOT
  touch the alpha-update bit, which is set independently by
  `GXSetAlphaUpdate()` and persists across `GXSetBlendMode` calls.
  `captureGradientCornerReflection()` never called `GXSetAlphaUpdate` at
  all, so it drew with whatever alpha-write state some earlier system in
  the frame left behind. Checked every `GXSetAlphaUpdate` call site in the
  whole codebase (`d_drawlist.cpp`, `d_particle.cpp`, `d_error_msg.cpp`,
  `d_home_button.cpp`, `d_a_mirror.cpp`, `d_a_movie_player.cpp`,
  `m_Do_graphic.cpp:2391`) — every single one disables it
  (`GX_DISABLE`/`GX_FALSE`); none re-enable it. So by the time our capture
  draw runs, alpha-write is off, and the render target's alpha channel
  stays at whatever it was cleared to (0) regardless of the vertex alpha we
  draw. **Fix applied** (`m_Do_graphic.cpp`,
  `captureGradientCornerReflection()`): wrapped the stripe `GXBegin`/`GXEnd`
  block with `GXSetAlphaUpdate(GX_ENABLE)` before and
  `GXSetAlphaUpdate(GX_DISABLE)` after — restoring disabled afterward to
  match the rest of the codebase's convention rather than leaving it on for
  whatever draws next. **NOT YET rebuilt/retested** — next step for whoever
  picks this up: rebuild, capture again in RenderDoc, confirm the same
  pixel (or any stripe pixel) now shows non-zero alpha, then test in-headset
  whether this actually changes water's visible transparency (it fixes a
  real, confirmed bug regardless, but whether THIS is what's driving the
  "opaque, no transparency" symptom specifically — as opposed to some other
  mechanism entirely, e.g. the water material's own vertex alpha or a
  separate blend constant not derived from this texture at all — is still
  unconfirmed; the prior "translucency comes from something other than
  straightforward texture alpha" finding from the section above was never
  fully explained either, only that naive edits to the *source stripe
  colors'* alpha had no effect — which this same alpha=0-write bug fully
  explains on its own, without requiring some entirely separate mechanism).
  **CONFIRMED (rebuilt + retested same session)**: fix works exactly as
  expected — RenderDoc pixel readout at (345, 378) now shows alpha `1.00`
  (was `0.00` before the fix). **But user confirmed zero visible change to
  water's transparency in-headset.** This makes the earlier "not simple
  texture alpha" suspicion definitive rather than just plausible: the
  reflection texture's alpha channel is provably not what drives water's
  translucency at all (we can now write real, correct alpha into it and
  nothing changes). The alpha-write fix itself is still worth keeping (it
  was a genuine bug — silently-dropped alpha writes could bite something
  else later), but it is NOT the fix for the opacity symptom. **Next step,
  not yet done**: stop looking at the reflection texture's own content
  entirely and instead find water's ACTUAL draw call in RenderDoc (not
  `dKy_bg_MAxx_proc`, which only builds the tex-gen matrix — the real
  blend/TEV state lives in the model's own compiled material data, applied
  at the model's normal per-frame draw-time material entry) and inspect its
  Pipeline State — Output Merger blend factors, pixel shader texture/alpha
  inputs — directly. Pixel History was not available in this RenderDoc
  build/capture type; use the Resource Inspector's usage list on
  `Dawn_InternalTexture_Resolved Texture` (or whatever the real water
  diffuse/reflection texture turns out to be) to jump to a read event
  instead.
- **Separately, still unexplained**: the captured texture's color content
  itself doesn't show 8 distinct stripes either — RenderDoc's Outputs
  thumbnail showed roughly 3 merged color bands with solid black on both
  the left and right edges, not 8 alternating stripes across the full
  width. Not yet root-caused. Candidate theory (not confirmed): the 16×16
  logical-pixel source rect (`GXSetTexCopySrc(0, 0, 16, 16)` in
  `captureGradientCornerReflection()`) also gets scaled via
  `map_logical_scissor()` using the same non-uniform VR scale factors
  described above, so the actual captured source rectangle in real pixels
  may not line up cleanly with where the 8 stripes were actually drawn
  (also sized via the same viewport scaling) — worth checking directly in
  RenderDoc (compare the drawn stripe quad's real screen-space extent,
  visible in the Mesh Viewer/Pipeline State for the stripe draw call,
  against the source rect used by the following `GXCopyTex`) before
  guessing at another fix blind.

**Current code state (`m_Do_graphic.cpp`, functional, not reverted)**:
`captureGradientCornerReflection()` draws 8 alternating-color stripes
directly into the current eye pass's corner (now with alpha-write
explicitly enabled for that draw — see Round 9 above), then captures them
with the corrected full-size destination, replacing the real
`retry_captue_frame()` call in VR (which is now guarded to flatscreen-only
again). Not yet rebuilt/retested since the Round 9 alpha fix.

**Options for a future session** (roughly in order of how promising they
seem, not yet attempted):
- Directly inspect (e.g. via RenderDoc, which is now a proven-useful tool
  for this project — see the Build Workflow section's notes on it) what
  the ACTUAL captured 304×224 texture looks like right after this capture
  runs, and separately what water's material actually samples from it at
  draw time. This would show directly whether the stripes really are
  present in the captured texture (ruling the capture side in/out) or
  whether the problem is entirely on water's sampling/UV side.
- Investigate the transparency mechanism properly: since it's confirmed
  NOT simple texture alpha, find where this material's actual blend
  factors/alpha source are configured (likely needs the same "find the
  legitimate per-frame `patch()`/`diff()` timing" investigation flagged as
  unresolved after Round 7's crash in the section below) rather than
  continuing to guess via texture content.
- **Second-camera idea (discussed 2026-07-29, not yet attempted — user's
  planned next-session direction)**: render an auxiliary,
  camera-**position**-anchored (but not head-**orientation**-anchored)
  view specifically for this capture, decoupling it from the VR headset's
  actual rapid/free head rotation — closer to how the original flatscreen
  camera behaves, which is what this whole screen-capture-as-reflection
  technique was originally designed around. Feasibility assessment:
  - **Performance is very likely NOT the limiting factor** if scoped down:
    low resolution keeps fragment/pixel cost trivial, and skipping most
    dynamic content (no shadows, no particles, no other water — obviously
    avoid recursively reflecting water) keeps CPU-side traversal and
    vertex cost low too. Reflections being lower-fidelity than the main
    view is normal in real games; this is not an unusual ask.
  - **The real risk is re-triggering this exact session's nested-
    offscreen-pass crash** (see "Solid-color-in-a-new-offscreen-pass
    (CRASHED...)" above). `mDoGph_Painter()`'s normal full scene draw
    invokes several systems that each do their OWN internal
    `GXCreateFrameBuffer`/`GXCopyTex` tricks (shadows, DOF blur, bloom,
    some particle effects) — and `begin_offscreen()`/`end_offscreen()`'s
    suspend/resume mechanism (`extern/aurora/lib/gfx/common.cpp`) only
    tracks ONE level of nesting (a single slot, not a stack), not
    arbitrary depth. A naive "just call the normal scene-draw function
    again, pointed at a new camera and a new small offscreen target" would
    very likely nest a THIRD level (VR eye pass → this new offscreen pass
    → whichever of those systems opens ITS OWN nested pass) and crash the
    same way. Mitigation: don't call the general "draw everything" path at
    all — call a narrow, deliberately-picked subset of draw calls instead
    (basically just the opaque terrain/BG list, e.g. whatever
    `dComIfGd_drawOpaList`-family function draws plain background
    geometry), skipping shadows/particles/bloom/DOF/other water entirely,
    both for the nesting risk and because reflections don't need that
    level of detail anyway.
  - **Worth checking early**: does `camera_p->view.viewMtx`/`projMtx` still
    hold a sensible flatscreen-style camera transform BEFORE VR's
    `beginEye()` overwrites it for each eye (`vr_stereo_render.hpp`)? If the
    game's underlying camera/follow logic still updates normally under the
    hood even while VR overrides the actual render matrices, that existing
    transform could be reused directly for this second render instead of
    computing a new camera-positioning scheme from scratch — a meaningful
    simplification if true, not yet confirmed either way.
- Accept the placeholder as sufficient for now (it's stable and doesn't
  crash) and deprioritize further reflection-quality work in favor of other
  VR issues (e.g. shadows, section 1).

**Original symptom description (superseded, kept for history)**:

**Round 1 fix attempt (tried, tested, NOT sufficient — but plausibly still
correct/needed)**: several water/reflection materials build their
environment-map reflection matrix via
`C_MTXLightPerspective(fovy, aspect, ...)` using `dComIfGd_getView()->fovy`/
`->aspect` directly — the same stale-for-VR fields the culling fix above
deliberately avoided touching. Added:
- `dusk::vr::getEyeSymmetricFov(float*, float*)` (`vr_main.hpp`/`.cpp`,
  backed by `vr_render::getEyeSymmetricFov()` in `vr_stereo_render.hpp`,
  which reuses the same symmetric-frustum-containing-the-real-asymmetric-FOV
  math already computed for the clipper fix).
- `dComIfGd_getReflectionFovAspect(f32*, f32*)` (`include/d/d_com_inf_game.h`,
  near `dComIfGd_getView()`) — returns `view->fovy/aspect` normally, or the
  VR-correct symmetric equivalent when `isRenderingToHeadset()`. Guarded
  `#if defined(TARGET_PC) && defined(DUSK_BUILDING_GAME)` (mods build
  `d_com_inf_game.h` too, without VR headers — don't remove that guard or
  the mod DLLs fail to compile).
- Applied at every confirmed water/reflection call site that read
  `dComIfGd_getView()->fovy`/`aspect` directly:
  `d_a_obj_groundwater.cpp` (`daGrdWater_c::Draw()`, the general lake actor),
  `d_a_obj_lv3Water.cpp`, `d_a_obj_lv3Water2.cpp`, `d_a_obj_lv3WaterB.cpp`
  (Lakebed-Temple-family water), `d_a_obj_rstair.cpp` (`mWaterModels`), and
  `d_kankyo.cpp`'s general BG water case (material names `MA10`/`MA02`,
  confirmed via the neighboring `mWaterSurfaceShineRate` reference).
  **Deliberately did not touch** `d_a_obj_tp.cpp` (uses the same
  `C_MTXLightPerspective` pattern but isn't clearly water-related — not
  reported broken, left alone) or `d_kankyo_rain.cpp` (same pattern via a
  `window_cam` pointer, likely the same underlying view but rain wasn't
  reported broken — left alone).
- **Result: rebuilt, tested in headset — water was still completely black.**
  So the fovy/aspect staleness theory, even if real, is not the (whole)
  cause. Do not re-attempt this exact fix; the code above is still in place
  (harmless/plausibly-still-correct) but something else is the actual cause.

**Round 2 (in progress — diagnostic logging added, awaiting test data)**:
Added `OutputDebugStringA` logging, gated to fire once per flatscreen/VR
state, at:
- `d_a_obj_groundwater.cpp`'s `daGrdWater_c::Draw()`: entry point (confirms
  the actor draws at all in VR), and after the lighting/tev setup — logs
  `tevStr.TevColor/AmbCol/TevKColor/mLightInf` (rules in/out a
  lighting-produces-black-color explanation distinct from the reflection
  matrix) — and after the reflection matrix computation, logs the actual
  `waterFovy`/`waterAspect` used and the resulting effect matrix contents.
  Log prefix: `[dusk::grdwater]`.
- `d_kankyo.cpp`'s general BG water case (`MA10`/`MA02`): logs material name,
  fovy, aspect once. Log prefix: `[dusk::kankyowater]`.
- **This has NOT yet been tested** — the immediate next step for whoever
  picks this up: launch in the headset, look at water, and read back
  whichever of `[dusk::grdwater]` / `[dusk::kankyowater]` lines appear (or
  note if NEITHER appears, which would mean the water the user is looking at
  uses some other rendering path entirely that hasn't been located yet —
  in which case, go looking for other `C_MTXLightPerspective` /
  `dComIfGd_getView()->fovy` call sites, or reconsider the mechanism from
  scratch: e.g. whether water's actual environment-map *texture* — as
  opposed to the coordinate-generation matrix — depends on something else
  VR never populates).
- Build succeeded with this logging in place; not yet tested in headset.

**Round 2 result (tested 2026-07-29)**: only `[dusk::kankyowater] VR=1
mat=MA02 fovy=110.00 aspect=0.964` fired — `[dusk::grdwater]` never appeared.
So the specific water body the user is looking at goes through `d_kankyo.cpp`'s
general BG-water path (`dKy_bg_MAxx_proc`, material `MA02`), not the
`daGrdWater_c` lake actor. fovy/aspect are sane (matches the Round 1 fix
working as intended) — confirms Round 1 is fully ruled out for this water,
not just "plausibly insufficient."

**Round 3 (bisect, tested 2026-07-29)**: temporarily skipped the *entire*
reflection-matrix branch in VR (treated `getTexMtx(0)` as NULL) in
`dKy_bg_MAxx_proc`. Water looked identical — still solid black. Caveat noted
at the time: this test is ambiguous on its own, since if this branch had
*never* run in VR this session, the material's effect matrix could
coincidentally still sample black either way.

**Rounds 4-6 (forced-color / forced-blend tests, tested 2026-07-29 — turned
out to be INVALID, see Round 7 correction below)**: forced this material's
`TevColor(0-3)`/`TevKColor(0-3)` registers to magenta (round 4), then also
logged+forced its blend mode from the original `GX_BM_BLEND` (src=`SRCALPHA`,
dst=`INVSRCALPHA` — completely ordinary alpha blending) to `GX_BM_NONE`
(round 5), all via direct `J3DMaterial`/`J3DBlend` setter calls right after
`dComIfGd_setListInvisisble()`. Water looked completely unchanged in VR
both times. Round 6 re-ran the same override **unconditionally, including
on flatscreen** as a control — and flatscreen water ALSO showed zero visible
change despite normally rendering correctly. **That control result
invalidates rounds 4 and 5's conclusions** — the overrides were never
reaching the real draw call at all (on either platform), so "no visible
effect" was never evidence about VR specifically, or about color/blend not
mattering. Do not cite rounds 4/5 as ruling anything out.

**Round 7 (root cause of rounds 4-6's non-effect, tested 2026-07-29 —
CRASHED, reverted, do not repeat without more care)**: `J3DMaterial::calc()`
(`J3DMaterial.cpp:267`, invoked via `simpleCalcMaterial` for the tex-mtx
work later in this same branch) only recomputes `mTexGenBlock` — it never
touches `mTevBlock`/`mColorBlock`/`mPEBlock`. Pushing those to the actual GX
command stream requires `J3DMaterial::patch()` (`J3DMaterial.cpp:240`,
calling `mTevBlock->patch()`/`mColorBlock->patch()`/`mTexGenBlock->patch()`
inside a `j3dSys.getMatPacket()` `beginPatch()`/`endPatch()` bracket) — which
rounds 4-6 never called, explaining the null effect on both platforms.
Tried calling `mat_p->patch()` directly after the overrides to fix this —
**this crashed immediately** with `[FATAL | aurora::gx::fifo]
command_processor: unknown opcode 0x7E`, a corrupted GX FIFO stream. Reverted
right away (game exited on the crash, no exe lock to worry about). Conclusion:
`patch()` (and presumably `diff()`) needs some packet-recording context that
isn't active during `dKy_bg_MAxx_proc` (this runs in the environment/kankyo
update pass, not during the model's actual draw-time material entry) —
calling it from here is unsafe until that context requirement is understood.
**Net result: we still do not have a working, safe way to force-apply
TevColor/blend changes to this material from this code path** — rounds 4-6's
"color/blend aren't the cause" conclusions are neither confirmed nor denied,
they're just uninformative. The reflection-matrix work from Round 1 remains
legitimately validated (it goes through `calc()`, which does work), but
nothing about color/blend/texture content has actually been tested yet.
- All diagnostic code from rounds 2/4/5/6 is still in `d_kankyo.cpp`
  (`dKy_bg_MAxx_proc`, the `MA10`/`MA02` branch) and safe (only the crashing
  `patch()` call was reverted). Only fires/logs once per VR/flat state via
  static bools; the magenta/blend overrides currently apply unconditionally
  (round 6's control-test change, not yet reverted back to VR-only gating).

**Drawn-list tracing (2026-07-29)**: `dComIfGd_setListInvisisble()` (called
unconditionally, VR or not, right before the `MA02`/`MA10` branch) routes
this material into the engine's "Invisible" list category — a legacy name;
functionally this is one of the two draw-list buckets
(`dComIfGd_drawOpaListInvisible`/`drawXluListInvisible`, in
`d_com_inf_game.cpp`) actually holding water among other things. Traced all
3 call sites of those draws in `mDoGph_Painter()` (`m_Do_graphic.cpp`,
around lines 2548-2554, 2642-2648, 2748-2758) — **none of them are
VR-guarded**; they're gated only by `g_env_light.is_blure` (0 vs 1, an
either/or pair covering both cases) and a `#if DEBUG` darkworld branch. So
geometry submission for this list is not skipped by any obvious guard in
VR — though this hasn't been confirmed by a direct "did GXCallDisplayList
actually execute" counter yet.
- Dead end found along the way: `dComIfGd_drawOpaListInvisible`/
  `drawXluListInvisible` are gated by
  `dusk::getSettings().game.disableWaterRefraction` — a manually-named
  ImGui debug checkbox ("Disable Water Refraction",
  `src/dusk/imgui/ImGuiMenuTools.cpp`), defaults `false`, has nothing to do
  with VR (not referenced anywhere under `src/dusk/vr/`). Since flatscreen
  water works fine with the same setting, this isn't the mechanism — just a
  suggestively-named coincidence. Don't chase this further unless the
  setting is confirmed toggled on somehow.
- Also checked all `Claude handoffs/VR_MOD_HANDOFF_*.md` files (3,4,5,7,8,9,10
  exist; `_11` is referenced in several code comments — e.g.
  `vr_stereo_render.hpp`, `vr_main.cpp` — but that `.md` was never actually
  written/saved). None of them mention water at all; they're all dated
  2026-07-22 through 2026-07-24, predating the water investigation
  entirely, and cover earlier stereo-rendering plumbing (submit/sync,
  `resolve_pass_checked`, foreign-pass substitution) that's now stable per
  the "Overall VR status" section above. Not useful for this bug beyond
  context already summarized in this file.

**Eye-buffer BMP dump (built + tested 2026-07-29 — confirmed, load-bearing
finding)**: built new texture-dump tooling reusing the VR mod's existing CPU
round-trip eye-readback path (`src/dusk/vr/vr_xr_submit.hpp`'s
`readbackEyeCopy()`, originally built for XR swapchain submission — see
`VR_MOD_HANDOFF_10`/`_11` comments there). Added `dumpEyeBufferToBmp()` (same
file, right before the `Session` class) — writes the already-mapped
`const uint8_t* mapped` CPU buffer straight to a 32bpp top-down BMP, handling
both `RGBA8Unorm*`/`BGRA8Unorm*` swapchain formats (throws/logs-and-skips on
anything else rather than guessing channel order). Hooked into
`readbackEyeCopy()` right after the `MapAsync` success check, gated to
re-dump every 90 frames per eye (overwriting `C:\Users\joeyw\dusklight\
vr_debug_eye0.bmp` / `vr_debug_eye1.bmp`) rather than a one-shot dump at VR
startup, so the file on disk always reflects a recent frame regardless of
when the user actually looks at water.
- **Result**: the dumped buffer shows a **distinct black silhouette exactly
  matching a lake/water-plane shape** (clean jagged shoreline edge,
  correctly positioned/occluding in the scene) sitting among otherwise
  correctly-rendered terrain — i.e. this is the game's own rendered output,
  not a compositor/XR-submission artifact. **This conclusively rules out any
  theory involving corruption after the game's render** (XR submission,
  swapchain format mismatch, compositor color-space issues, etc.) — the
  game itself really does render this material as solid black. It also
  confirms the geometry IS being drawn (distinct silhouette, correctly
  depth-sorted against the terrain) — not culled or skipped, contradicting
  the "geometry never submitted" theory from the drawn-list tracing below.
- BMPs can be converted for viewing with e.g. PowerShell's
  `[System.Drawing.Image]::FromFile(...).Save(..., Png)` (Read tool can't
  open raw `.bmp` directly).
- This tooling is generically reusable for future VR visual-symptom
  debugging (whatever's rendered to either eye, once per ~90 frames) — not
  water-specific. Leave in place; harmless (a bit of extra CPU-side work
  writing files every 90 frames), matches the "diagnostic scaffolding stays
  until the bug's fixed" workflow rule.

**Ideas not yet tried (SUPERSEDED — kept for history only)**: everything
below was written while still hunting for the cause and turned out to be
barking up the wrong tree entirely — the real cause was that `dKy_bg_
MAxx_proc`'s texture *is* `retry_captue_frame()`'s shared screen-capture
target, just not written via any call visible from inside that function
itself (the capture happens earlier in the frame, in `m_Do_graphic.cpp`).
See the ROOT-CAUSED writeup at the top of this section instead.
- ~~find a SAFE way to force-test this material's actual TEV/blend/texture
  content~~ — turned out to be irrelevant; the fix required zero changes to
  `dKy_bg_MAxx_proc` or any `J3DMaterial`/TEV/blend state at all.
- ~~dump the buffer with vs. without the Round 1 reflection-matrix fix~~ —
  moot; Round 1 was legitimate but the texture it was projecting was simply
  never being written to.
- ~~confirm whether the reflection texture is a static baked asset~~ — it
  is NOT; it's the shared 304×224 screen-capture texture, confirmed via
  RenderDoc.
- A GPU debugger capture (RenderDoc) turned out to be exactly the right
  call — this is genuinely how the actual root cause got found, once the
  build-and-guess `OutputDebugStringA` loop hit its limits. If a future
  investigation on this project stalls the same way, reach for RenderDoc
  sooner rather than later (see the RenderDoc setup notes elsewhere in
  this file, if kept, or re-derive: launch the game via RenderDoc's
  "Launch Application" — NOT its own hotkey capture, which only sees the
  desktop window's mostly-empty Present() calls while VR is active — and
  trigger a capture from in-game code instead, bracketing the actual
  `aurora_begin_frame()`/`aurora_end_frame()` pair in `m_Do_main.cpp`).

### 4. Underwater bloom — investigated, NOT the water-black bug, still disabled
- `m_Do_graphic.cpp`: `mDoGph_gInf_c::getBloom()->draw()` (triggered near/in
  water via `camera_water_in_status`) is guarded off entirely for VR, same
  for the `retry_captue_frame()` call that feeds it. Comment at the guard
  explicitly says "real fix is giving offscreen passes protected identity in
  common.cpp (option (b), not done here)" — which is what the `extern/aurora`
  nesting fix (#1 above) implements. **However**: `bloom_c::draw()` samples
  from `getFrameBufferTexObj()`, which is only populated by
  `retry_captue_frame()` — a plain `GXCopyTex`/`resolve_pass_into()`
  substitution, NOT a `GXCreateFrameBuffer`, so it is NOT covered by the
  aurora nesting fix at all. `resolve_pass_into()` still unconditionally
  drops the copy when the current pass is protected (VR eye) — see its
  comment in `extern/aurora/lib/gfx/common.cpp`. So even removing both
  guards would very likely still show stale/no bloom, since the capture
  feeding it can never succeed during VR as-is.
- This was a *dead end for the water-black bug specifically* — bloom is an
  additive glow overlay; if it failed to render you'd expect no glow, not a
  fully opaque black base surface. Left disabled; not touched further.
- **UPDATE 2026-07-29 — the underlying `resolve_pass_into()` limitation
  described above is now FIXED** (see section 3's writeup): it no longer
  unconditionally drops the copy when the pass is protected, it lets the
  substitution through safely instead. This was fixed for water, but the
  fix is generic (in `resolve_pass_into()` itself, not water-specific) — so
  bloom's OWN `retry_captue_frame()`/`getBloom()->draw()` VR guards
  (still in place, not touched this session) could very plausibly now be
  safely re-enabled too, the same way water's was. **Not yet attempted or
  tested** — bloom's guards are separate call sites from the one re-enabled
  for water; re-enabling them is a candidate follow-up, not yet done.

### 5. Heat-wave / "kagerou" particle effects — ROOT-CAUSED AND FIXED 2026-07-29

Written up in more detail than usual because this is a well-known class of
bug for anyone VR-modding a GameCube/Wii-era engine with this style of
cheap heat-shimmer effect — people have said they want to attempt the same
fix elsewhere, so this section is meant to be followable on its own, not
just a change-log entry.

**Symptom**: a large rectangular "floating portal" hanging in the scene,
showing a duplicated/offset copy of whatever's in view (e.g. a visibly
duplicated horse, floating above the real one, offset roughly by the
stereo eye separation) — VR-only, first reported in outdoor sunset scenes.
Looked cosmetic/minor but was jarring and immersion-breaking in-headset.

**General mechanism (the reusable part)**: this era of Zelda engine fakes
"heat shimmer" using the exact same cheap trick water's fake reflection
uses (see section 3) — sampling a shared, low-res, live screen-capture
texture (`mDoGph_gInf_c::getFrameBufferTex()`) through a particle instead
of a real heat-distortion shader. The hook is
`JPAResourceManager::swapTexture(mDoGph_gInf_c::getFrameBufferTimg(),
"dummy")`, called once when the common/scene particle resource managers are
created (`d_particle.cpp`'s `dPa_control_c::createCommon()`/
`createRoomScene()`): **any particle asset whose JPA texture is literally
named `"dummy"` gets that texture silently swapped for the live shared
capture at load time** — nothing at the call site marks it as special, you
have to know this mechanism exists to find it by reading code. In VR, that
single shared capture texture is subject to the same per-eye/stale-frame
ambiguity that broke water's reflection: sampling it through a particle's
own UV animation produces "a duplicate of the scene, offset like a ghost"
instead of a subtle shimmer, which reads as a floating portal rather than
heat haze. **If you're chasing this in a similar engine: any particle using
this "dummy"-texture-swap technique is a candidate, regardless of what it's
named** — this project's actual instances weren't even all named
"kagerou"/"heat"/"shimmer".

**Why three earlier guesses (from a prior, undocumented session) all
failed**: the shared IndScreen distortion pass, the sun disc sprite/lens
flare, and one specific torch-actor's kagerou particle were each disabled
in turn, and the user kept seeing the blob after every one. Root cause:
the exact same kagerou particle ID is spawned independently from *at
least three unrelated systems* in this codebase alone (see below) — each
disable only touched the one system a session happened to guess at. **The
lesson: don't assume disabling the first plausible-looking spawn site is
sufficient. Prove it with evidence (below), not by exhausting guesses.**

**Diagnostic technique that actually found it (the reusable method)**:
guessing was replaced with direct evidence by logging every *distinct*
particle id/name spawned during a VR session, once each, via
`OutputDebugStringA`, then reproducing the bug and reading back the Output
window. Two non-obvious pitfalls cost real time building this and are
worth knowing up front:
1. **Instrument the actual creation choke point(s), not a specific
   effect's suspected call site.** This engine has (at least) two
   completely separate top-level particle-spawn functions:
   `dPa_control_c::setSimple()` and `dPa_control_c::set()` (both in
   `d_particle.cpp`). `dComIfGp_particle_setColor()`/`_setNormal()` (used
   by most gameplay effects) funnel through `set()`; only a minority of
   call sites (mostly fire/torch particles) go through `setSimple()`.
   Instrumenting only `setSimple()` — the first, most obvious place to add
   a log — produced **zero relevant log lines**, not because nothing was
   spawning, but because the actual culprit spawned through the other
   function entirely. Both had to be instrumented before the log was
   useful.
2. **Gate the log on a whole-session-scoped flag, not a per-frame
   draw-scoped one.** This project already had a `g_duskVRRenderingToHeadset`
   flag, but it's only `true` for the narrow window inside the per-eye
   render call each frame (`vr_main.cpp`'s `tick()`) — particle spawns
   happen during game-logic update, which runs outside that window, so
   gating a spawn-time log on it risks silently never firing depending on
   update/draw ordering, independent of whether anything is actually
   spawning. Added `g_duskVRSessionActive` (mirrors `isActive()`/
   `g_session != nullptr`, true for the whole VR session lifetime) instead,
   specifically for this kind of update-phase diagnostic.
3. **A found id is only useful if you can safely turn it back into a
   name.** `dPa_name::getName()` (`d_particle_name.cpp`) has a real,
   pre-existing bug: it bounds-checks a *masked* id
   (`i_id & 0xFFFF1FFF >= ID_PARTICLE_MAX`) but then indexes its name table
   with the *raw*, unmasked id — so an id with high flag bits set (e.g. a
   dynamically-assigned scene/group id) can pass the check yet read the
   array far out of bounds, returning garbage instead of `NULL`. A plain
   `if (name != NULL)` guard is **not sufficient**. This caused a real
   crash (access violation, unrelated actor `daBubbPilar_c` spawning a
   scene-flagged id during a save load) after the logging was added. Fix:
   only call `getName()` when the *raw, unmasked* id itself is directly a
   safe in-bounds index (`param < ID_PARTICLE_MAX`); otherwise skip the
   name lookup and log the numeric id alone.

**The actual particles found and fixed**:
1. `d_kankyo_rain.cpp`'s `dKyr_sun_move()` (~line 447-474): particle id
   `0x11C` (`ZI_J_sunKagerou01.jpa`) — a heat-shimmer effect tracking the
   sun, spawned every frame the sun is visible (`camera_water_in_status ==
   0 && daytime > 255.0f && sunAlpha >= 0.2f`), positioned a fixed 30160
   units from the camera eye toward the sun. A VR headset's free head
   rotation sweeps that fixed-offset anchor across the view far more than
   a flatscreen third-person camera ever would — the same class of problem
   already noted for water's reflection quality. Previously undiscovered
   because it's spawned from environment/weather update code, not from any
   actor or the weather *draw* functions a prior session had already
   checked. Fixed by skipping the spawn call in VR
   (`#ifdef TARGET_PC / if (!dusk::vr::isRenderingToHeadset())`), same
   pattern as the already-accepted disables for the other two kagerou
   effects.
2. The *exact same* particle id `0x103` (`ID_ZI_J_O_KAGEROU`) that the
   prior session had already disabled for one specific torch actor
   (`d_a_ep.cpp`'s `ep_class`) turned out to *also* be spawned directly and
   unconditionally by six completely separate, unrelated actor classes,
   none of which route through `ep_class` at all:
   `d_a_obj_lv1Candle00.cpp`, `d_a_obj_lv1Candle01.cpp`,
   `d_a_obj_lv2Candle.cpp`, `d_a_obj_lv3Candle.cpp`,
   `d_a_obj_onsenFire.cpp` (hot spring fire), `d_a_obj_TvCdlst.cpp`. Found
   by grepping the whole codebase for other direct callers of the same
   particle id once it was identified via the log — this is the step that
   actually generalizes; the specific ids won't match another game/mod,
   but "grep for every other call site of the id you just found" is the
   repeatable part. Each site got the same VR-skip guard applied only to
   its `0x103` call, leaving that actor's other fire particles
   (`0x100`/`0x101`/`0x83a6`/`0x83a7`) untouched.

**The generalizable recipe, for anyone attempting this in a similar
engine**:
1. Instrument the *actual* particle-creation choke point(s) — trace what
   your suspected effect's spawn call really funnels through, don't assume
   it's the first/obvious-looking function. Log every distinct id/name
   once per VR session, gated on a whole-session-scoped "is VR active"
   flag rather than a per-frame render-scoped one.
2. Reproduce the bug, read the log, identify the id(s) via your particle
   name table. Sanity-check that lookup function for bounds bugs before
   trusting it blindly (see pitfall 3 above) — the crash cost more time
   than the original investigation.
3. Grep the *entire* codebase for every other direct call site of that
   same id. Assume it's spawned unconditionally by multiple unrelated
   actor classes until you've actually checked — this project found the
   identical id hardcoded independently in 7 call sites across 8 files,
   not just the one obviously related to what you were looking at.
4. Wrap each spawn call in a VR-skip guard
   (`!dusk::vr::isRenderingToHeadset()` or your engine's equivalent),
   touching only that one call, leaving every other particle/effect at
   that call site alone.
5. Separately, check whether the effect's underlying resource uses a
   live-screen-capture texture-swap technique at all (the `"dummy"`-name
   convention described above, or whatever your engine's equivalent is) —
   if so, treat *any* particle using it as a candidate for this exact
   symptom, independent of naming.

**Known gaps / not yet covered**:
- Dawn (the `daytime < 180.0f` branch of the same sun-color-blend logic in
  `dKyr_sun_move()`) hasn't been explicitly tested in-headset — same code
  path, untested time-of-day window. Likely fine (same guard covers it)
  but not confirmed.
- Goron Mines (the lava dungeon) confirmed clear of the portal after these
  fixes, but had a separate, similar-looking visual issue there per the
  user — **root-caused and fixed 2026-07-31, see section 10.** Turned out to
  be three more instances of this exact "dummy"-texture mechanism that this
  section's whole-codebase grep didn't happen to catch, since none of them
  are named "kagerou" and one is spawned via a raw hex literal instead of
  the id constant's name.
- The diagnostic logging added this session (`d_particle.cpp`'s
  `[dusk::particle] setSimple`/`[dusk::particle] set` logs, and
  `g_duskVRSessionActive` in `vr_main.cpp`) is still in the tree as of this
  writing. Per this project's usual practice (see Build workflow notes)
  it should eventually be removed now that the bug is confirmed fixed, but
  was deliberately left in for now in case it's useful for the Goron Mines
  follow-up or any other still-unguarded kagerou-family/`"dummy"`-texture
  spawn site that a whole-codebase grep didn't happen to catch.

### 6. VR failing to launch at all on SteamVR/Virtual Desktop (worked fine on Meta Link) — FIXED 2026-07-30

**Symptom**: the mod had only ever been tested via Meta Link (Quest Link/Air
Link) before this session — that always worked. Testing SteamVR and Virtual
Desktop (VDXR) for the first time on 2026-07-30 found the game silently
fell back to flatscreen on both, with zero visible error to the user (VR
mod's own design: `startup()` catches everything and just proceeds
flatscreen-only — see Build Workflow's `OutputDebugStringA` logging, which
is what actually diagnosed this).

**Root cause 1 — OpenXR API version mismatch**: `vr_xr_bootstrap.hpp`'s
`initialize()` requested `XR_CURRENT_API_VERSION`, which resolves to
1.1.60 in this project's vendored OpenXR headers (`/c/vcpkg/installed/*/
include/openxr/openxr.h`). Neither SteamVR's nor Virtual Desktop's OpenXR
runtime supports the 1.1.x instance API yet — `xrCreateInstance` failed
with `XR_ERROR_API_VERSION_UNSUPPORTED` on both (confirmed via the VS
Output window's `[dusk::vr::startup] EXCEPTION: OpenXR call failed:
xrCreateInstance` line). Meta's runtime happens to support 1.1, which is
why this was never caught before. **Fix**: request `XR_API_VERSION_1_0`
explicitly instead — this bootstrap only uses core 1.0 functionality plus
the D3D12 KHR extension, so there's no feature reason to ask for 1.1.

**Root cause 2 — swapchain format not universally supported**: even after
fixing the API version, Virtual Desktop worked but SteamVR failed at
`xrCreateSwapchain` with `Failed to create swapchain image: Unsupported
format: 87` (87 = `DXGI_FORMAT_B8G8R8A8_UNORM`, aurora's native render
format, hardcoded as the swapchain format via `toDxgiSwapchainFormat()`).
Per the OpenXR spec, an app must only request a format the runtime actually
returned from `xrEnumerateSwapchainFormats` — this code never called it,
just assumed its own native format would be accepted everywhere (true for
Meta and Virtual Desktop, false for SteamVR). **Fix**: `vr_xr_submit.hpp`'s
`Session::createSwapchain()` now enumerates the runtime's real supported
list and picks the first viable candidate in preference order: (1) native
format exactly, (2) its channel-swapped counterpart (real R/B swap, still
no gamma semantics), (3) its sRGB-toggled counterpart, (4) both
channel-swapped and sRGB-toggled, (5) `DXGI_FORMAT_R10G10B10A2_UNORM` as an
absolute last resort. `readbackEyeCopy()` was extended to actually perform
whichever pixel transform the chosen format needs (`SwapchainPixelConversion`
enum: `None`/`ChannelSwap`/`PackR10G10B10A2`) before uploading, instead of
just changing the declared format and leaving stale bytes.

**A confusing wrinkle worth remembering**: SteamVR's `xrEnumerateSwapchainFormats`
list (`29 91 2 10 24 40 55 45 20`) includes `24` (`R10G10B10A2_UNORM`) and
`xrCreateSwapchain` with it succeeds — but actually submitting a real
projection layer with it fails at runtime (`ComposeLayerProjection: failed
to submit view 0/1: VRCompositorError_TextureUsesUnsupportedFormat`), with
the headset stuck on SteamVR's "waiting for application" screen forever
despite the flatscreen window running fine. **Lesson: `xrEnumerateSwapchainFormats`
returning a format is not proof the runtime's actual projection-layer
compositor path accepts it — verify by actually getting a frame in front of
the user, not just by a successful `xrCreateSwapchain` call.** This is why
`R10G10B10A2_UNORM` is ranked *last* in the preference order above (after
the sRGB variants, which SteamVR both advertises AND actually composites)
rather than higher despite being the more "correct" gamma-neutral choice on
paper.

**Root cause 3 — SteamVR's sRGB format visibly oversaturates colors**:
once frames were actually reaching the headset via SteamVR's
`B8G8R8A8_UNORM_SRGB` format, colors looked oversaturated compared to
Virtual Desktop/Meta Link (both use the plain, non-SRGB native format,
zero pixel transform). The theoretical "should be a lossless round-trip"
argument (SteamVR's SRGB-aware sampling decodes on read, then re-encodes
for the panel, and encode∘decode should cancel out) does NOT hold up
against the observed result — something in SteamVR's closed-source
compositor (likely gamut/color-management processing applied only to
properly-tagged SRGB content, not a simple gamma bug) makes this visibly
different from a raw passthrough, and there's no way to inspect or
precisely reverse-engineer it from outside. **Fix was empirical, not
derived**: added a tunable gamma-compensation LUT
(`vr_xr_submit.hpp`'s `steamVrGammaCompensationLut()`/
`kSteamVrGammaCompensationExponent`), applied to R/G/B (never alpha)
before upload, ONLY when the chosen swapchain format is one of the SRGB
variants (`Session::swapchainIsSrgb_`) — so this never touches VD/Meta at
all. Tried exponent 2.2 first (darkening curve) — user reported "worse,
looks evil" (overcorrected too dark). Flipped to **1.0/2.2 (brightening
curve)** — user confirmed "looks normal." If this ever needs revisiting
(e.g. a SteamVR update changes its compositor's color handling), the
exponent is the one constant to retune via the same rebuild-and-eyeball
loop; 1.0 disables compensation entirely as a sanity-check baseline.

**Framerate cost — moved from CPU to GPU 2026-07-30, only partially
recovered, flagged as a possible crash-regression risk**: originally
implemented as a scalar CPU per-pixel loop (3 LUT lookups per texel) over
the full stereo resolution (~9.7M texels/frame at 2112×2304-per-eye),
replacing what used to be a fast bulk `memcpy`, layered on top of a
CPU-readback path already documented elsewhere in this file as
"correctness-first, blocking, perf TODO." User observed roughly halved
framerate on SteamVR specifically (confirmed NOT affecting VD/Meta, which
never touch this code path either way). Per user request, moved to an
actual GPU compute pass instead of the CPU loop, specifically to eliminate
the cost rather than just shrink it:
- `vr_xr_submit.hpp`: `kGammaComputeShaderSource` (a WGSL compute shader,
  `textureLoad` from the source eye texture, `pow()`-based gamma curve on
  R/G/B only, packs the result into a `u32` storage buffer matching the
  CPU-readback buffer's row layout exactly), `GammaComputeParams` (the
  matching uniform struct, manually padded to 32 bytes for WGSL's
  host-shareable layout rules), `Session::ensureGammaComputeResources()`
  (lazily creates the pipeline/bind-group-layout once via
  `aurora::webgpu::g_device`), and `Session::CpuCopyBuffers::gammaStorage`/
  `gammaUniform` (per-eye GPU-only buffers the shader writes into, then
  `CopyBufferToBuffer`'d into the existing CPU-mappable `readback` buffer —
  WebGPU doesn't allow combining `Storage` with `MapRead` usage on one
  buffer, hence the extra GPU-side copy). `encoderTaskCallback()` now
  branches: if `swapchainIsSrgb_`, dispatch the compute pass instead of the
  plain `CopyTextureToBuffer`; `readbackEyeCopy()` correspondingly just
  memcpy's when `swapchainIsSrgb_` (the shader already applied gamma AND
  any channel reorder), only falling through to the old per-conversion CPU
  switch (`ChannelSwap`/`PackR10G10B10A2`/`None`) for the non-SRGB
  candidates where no gamma correction ever applied.
- **Gating unchanged, confirmed VD/Meta still completely unaffected**:
  `swapchainIsSrgb_` is only ever true when `createSwapchain()`'s candidate
  search picks one of the SRGB formats, which only happens for whichever
  runtime forces it (SteamVR, confirmed). VD/Meta's very first candidate
  (native format) succeeds, so `ensureGammaComputeResources()` is never
  even called and `encoderTaskCallback()` takes the untouched plain-copy
  branch for them — verified by construction (the gate), not just assumed.
- **Result, user-reported**: "got 20 more frames" compared to the CPU LUT
  version — a real improvement, but phrased as partial, not "back to
  normal" — full parity with VD/Meta's framerate on SteamVR is NOT
  confirmed. **User explicitly wants to revisit/optimize this further in a
  future session** rather than closing it out as fully resolved.
- **Flagged as a possible regression source for OTHER bugs, not just
  perf**: this is new, relatively complex GPU-side code (a real compute
  pipeline, bind group, extra per-eye storage/uniform buffers, a
  buffer-to-buffer copy) added directly into the per-frame VR eye-copy
  path, on SteamVR only, with limited testing so far (confirmed working
  once, not stress-tested across long sessions/dungeons/save-load cycles
  the way earlier bugs in this file were). **If something SteamVR-specific
  breaks later (crash, hang, corrupted frame, hitching) that doesn't
  reproduce on VD/Meta, check this compute pass first** before assuming
  it's unrelated — the gating means it's the one meaningfully different
  code path SteamVR takes that VD/Meta don't.

**Confirmed working end-to-end on all three runtimes as of 2026-07-30**:
SteamVR, Virtual Desktop, and Meta Link (Meta Link retested after these
changes specifically to confirm no regression from the API-version/format
changes — none found).

### 7. VR HUD — flat per-eye overlay replaced with a head-locked 3D billboard — FIXED 2026-07-30

**Symptom**: the 2D HUD (hearts, rupees, menus — drawn by the tail of
`mDoGph_Painter()`, `m_Do_graphic.cpp`) used to be drawn per-eye with a raw
screen-space orthographic projection, identical in both eye textures —
zero stereo disparity, which read as either painted directly on the lens or
otherwise sitting at an uncomfortable, badly-defined depth. User wanted it
pushed back to a comfortable, unobtrusive distance instead. Explicitly
agreed approach: **head-locked** (rigidly follows the view every frame) for
now, structured so it can grow into **body-locked** (lags behind quick head
turns, steadier/less "swimmy") in a future session without a rewrite.

**Architecture**:
- `mDoGph_drawHud2D()` (`m_Do_graphic.cpp`) — the old inline HUD-drawing
  tail of `mDoGph_Painter()`, extracted into its own function so it can be
  called standalone. Flatscreen behavior is unchanged (still called inline,
  same content).
- `mDoGph_gInf_c::captureHudBillboard()` (`m_Do_graphic.cpp`) — renders that
  same flat HUD, once per frame, into a small persistent offscreen texture
  (`m_hudBillboardTimg`/`Tex`/`TexObj`, same "allocate once via `createTimg()`,
  refresh every frame via `GXCopyTex`" template as the existing
  `m_fullFrameBufferTex*`/`mFrameBufferTex*` capture buffers). Called from
  `vr_main.cpp`'s `tick()` **before** the per-eye loop opens either eye's
  protected offscreen pass — critical: `GXCreateFrameBuffer`'s
  single-level-nesting limit means this must never run nested inside a VR
  eye pass (see section 3's "Solid-color-in-a-new-offscreen-pass" crash for
  why). Sized exactly `FB_WIDTH x FB_HEIGHT` (608x448) so the capture reuses
  the existing ortho/viewport code with zero rescaling awareness needed.
- `vr_render::drawHudBillboard()` (`vr_stereo_render.hpp`, forwarded via a
  thin `dusk::vr::drawHudBillboard()` wrapper in `vr_main.hpp`/`.cpp` so
  `m_Do_graphic.cpp` doesn't need to include the heavier OpenXR/aurora
  headers) — draws that captured texture as a real 3D quad, once per eye,
  called from `mDoGph_Painter()`'s original HUD call site
  (`if (!dusk::vr::isRenderingToHeadset()) { mDoGph_drawHud2D(); } else { dusk::vr::drawHudBillboard(...); }`).
  Reasserts the eye's real asymmetric projection
  (`GXSetProjection(view->projMtx, GX_PERSPECTIVE)` — not automatic, a
  stateful register write the 2D/3D passes both clobber earlier in the
  frame) and draws the quad with an **identity position matrix**, vertices
  authored directly in eye-space — this is what makes it head-locked "for
  free," no view-matrix multiply needed. Quad placement (`computeHudPose()`,
  same file) is deliberately its own small function, separate from the
  actual GX draw calls — the seam for a future body-locked mode (swap this
  one function for something that low-pass-filters yaw instead of using the
  raw eye pose, without touching capture/texture/draw plumbing at all).
  Tunable constants: `kHudDistanceMeters` (2.0), `kHudWidthMeters` (1.4,
  bumped up from an initial 1.0 per user feedback), `kHudHeightMeters`
  (derived, matches 608:448 aspect).

**Gotcha 1 — captured content came back solid black**: `mDoGph_drawHud2D()`
draws nothing when called from `captureHudBillboard()`'s pre-eye-loop
position, even though the offscreen-pass/`GXCopyTex` pipeline itself was
proven fine (confirmed via a hand-drawn solid-color marker quad injected
directly into the same capture, independent of `mDoGph_drawHud2D()`'s own
draw calls — it showed up fine on the billboard). Root cause: whatever
populates/refreshes the persistent 2D HUD draw-list content happens as a
side effect of `fpcM_DrawIterater()` (actor updates, meter state, etc.),
not independently of it — and at the point `captureHudBillboard()` runs
(before the per-eye loop), that hasn't run yet this frame. **Fix**: call
`fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw)` once, explicitly,
right before `captureHudBillboard()` in `vr_main.cpp`'s `tick()` — a third
call per frame (previously only once per eye, twice total). Its 3D draw
output targets the normal EFB pass, which is discarded/never presented
while VR is active (same "game skips its own desktop redraw" behavior noted
in the Build Workflow section), so this is CPU-traversal cost, not wasted
GPU-visible work.

**Gotcha 2 — real per-pixel alpha made the WHOLE panel invisible**: tried
sampling the captured texture's own alpha channel (`GX_CA_TEXA`) for real
per-pixel transparency (icons visible, background see-through). Confirmed
the captured COLOR content was correct first (a debug pass forcing
`GX_BM_NONE` — opaque overwrite, alpha ignored entirely — showed real HUD
icons, just against an opaque black background as expected for that test).
But switching to real alpha blending made the ENTIRE billboard disappear,
icons included — not just the background. This is the same class of "this
material's translucency isn't simple texture alpha" finding as the water
investigation (section 3) hit and never fully explained either; likely
these HUD materials' TEV alpha outputs were simply never authored to be
meaningful, since alpha-write has always been disabled during normal
gameplay (nothing downstream ever consumed it before this VR use case) —
not one narrow bug to fix, a systemic non-signal. **Direct inspection ruled
out**: unlike the eye-buffer readback path (`vr_xr_submit.hpp`'s
`dumpEyeBufferToBmp()`), the `GXCopyTex` destination pointer used for this
capture is only a GPU-texture-cache key in this PC port
(`extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp`'s `copy_tex()` — `dest`
is a `CopyTextureKey`, real pixels are GPU-resident) — no cheap CPU-side
pixel readout is possible; a real answer would require a full RenderDoc,
per-material investigation, the same open-ended effort that never fully
resolved for water's alpha. **Fix chosen instead (explicit user tradeoff,
not a default)**: derive alpha from the captured COLOR itself — a "luma
key". `drawHudBillboard()` uses 3 TEV stages, repurposing
`GX_TEV_SWAP1`/`2`/`3` (leaving `SWAP0`'s identity default untouched) to
read the texture's red/green/blue channels one at a time into the alpha
slot, accumulating `R+G+B` (saturating) as the final alpha via
`GX_CA_APREV`-chained `ADD` ops — background (capture's black clear color)
→ alpha ≈ 0 → transparent; any real HUD content (colored) → higher alpha →
visible. Not pixel-perfect (a near-black icon pixel would read as
transparent too) but requires no per-material investigation and looks
correct for real icon art in practice — confirmed in-headset. **If this
ever needs revisiting**: the honest fallback if the luma-key look isn't
good enough is the full RenderDoc per-material investigation described
above, not another blind heuristic.

**Performance, measured (not assumed) 2026-07-30**: `std::chrono` timing
around both the extra `fpcM_DrawIterater()` call and `captureHudBillboard()`
itself, logged in 90-frame running averages. Normal gameplay: ~0.09-0.14ms
+ ~0.04-0.07ms. Menu/pause screens (more 2D content, less 3D actor
traversal): up to ~0.5ms combined at the observed worst case. Against a
72-90Hz VR frame budget (~11-14ms), that's roughly 1-4% at worst, under 2%
typically — not a measurable perf concern, not worth optimizing further
absent new evidence. Diagnostic timing code removed after confirming this;
if perf ever needs re-checking, the pattern (accumulate
`std::chrono::high_resolution_clock` deltas over N frames, log the average,
reset) is simple to re-add at the same two call sites in `vr_main.cpp`'s
`tick()`.

**Follow-up same day: damping added, user reported the head-locked panel
felt "really shaky"** — expected, since it was rigidly glued to raw
per-frame head tracking with zero filtering. Fixed via
`vr_stereo_render.hpp`'s `updateHudSmoothing()`/`computeHudPose()`: a
persistent, GAME-WORLD-space direction (`g_hudSmoothedWorldForward`) is
low-pass-filtered toward the real head direction once per frame
(`kHudDampingAlpha = 0.08`, lerp-per-frame, not frame-time-corrected),
computed via `updateHudSmoothing()` (called once, `vr_main.cpp`'s `tick()`,
alongside `captureHudBillboard()`) by reusing `eyePoseToViewMtx()` — already
validated, drives the whole working 3D scene — purely for its rotation math
(zero position delta, dummy `linkEyeGame`, only reads back row 2 of the
resulting matrix). **Deliberately damps ORIENTATION only, not position** —
position tracks the head instantly, matching how shipped VR games' actual
body-locked UI behaves (translating with you feels natural; only rotational
lag reads as "steadier"). `computeHudPose()` re-projects the damped
world-space direction into whichever eye is CURRENTLY drawing via that
eye's own (un-damped, current) `view->viewMtx` rotation each call — this is
what keeps the panel's own plane always flat-facing you (right/up axes stay
purely eye-local, never rotated) while only the CENTER's angular position
lags.

**Bug hit and fixed same round**: first version double-negated the
distance — `dist` was still the OLD pre-signed constant
(`-(kHudDistanceMeters*kHudUnitsPerMetre)`) left over from the original
fixed-offset code, but the new re-projected direction `(ex,ey,ez)` ALSO
already carries the correct sign (≈`(0,0,-1)` in eye-local space when
undamped) — multiplying two negatives together flipped the panel to
directly behind the camera, making it disappear entirely (**user report:
"the huds gone"**). Fixed by making `dist` a plain positive magnitude
(`kHudDistanceMeters * kHudUnitsPerMetre`, no sign) since the direction
vector now supplies the sign. **Lesson for next time a "reproject a
direction into local space" pattern gets added here**: any pre-existing
sign baked into a distance/offset constant from an OLDER fixed-vector
version needs auditing once that constant starts being multiplied by an
actual direction vector instead of used directly as a raw offset — the two
conventions (signed offset vs. positive-magnitude-times-signed-direction)
look superficially similar but silently double-negate if mixed. Confirmed
fixed and steady in-headset after the correction — user: "That looks really
nice."

**Unrelated incident during this session, worth flagging**:
`extern/aurora/lib/gfx/common.cpp` (a file untouched by any of this HUD
work) had its `wait_for_gpu_progress()` function's closing `}` replaced
with literal garbled text (`Why ca}`, then — after being fixed once —
regenerated a SECOND time as `Why caYea}`, i.e. it grew rather than just
reappearing identically) mid-session, breaking the build both times. Fixed
both times (restored the plain `}`); this pattern (same exact line,
growing between occurrences) strongly suggests something on the user's
end — an editor, autocomplete, or dictation tool — was actively typing into
that specific file while it had focus, not a one-off fluke or anything
these code changes caused. Flagged to the user; if the build ever breaks
again at this exact spot, check for a stray focused window/editor on
`extern/aurora/lib/gfx/common.cpp` before assuming it's a real regression.

### 8. VR minimap black with color-corruption pixels — FIXED 2026-07-30

**Symptom**: the small in-game HUD minimap (and, it turns out, the
pause-screen full map by the same mechanism) rendered as solid black with a
scattering of stray colorful pixels — VR only, fine on flatscreen. Pattern
is the classic signature of an uninitialized/never-written GPU texture
(garbage memory content), not a shader or blend bug.

**Root cause**: the minimap renders its own source texture via a dedicated
`GXCreateFrameBuffer` offscreen pass — `d_map_path.cpp`'s
`dRenderingMap_c::renderingMap()`, reached through
`dComIfGd_drawCopy2D()` → `dDlst_list_c::drawCopy2D()` → virtual `draw()`
on the registered `dMap_c` (small minimap) or `dMenu_FmapMap_c` (pause
map) instance. That call site fires once per eye from inside
`mDoGph_Painter()` (`m_Do_graphic.cpp`), i.e. *after* `beginEye()` has
already opened that eye's own protected offscreen pass — nesting a second
one there would crash, same class of bug as the water-reflection capture
(section 3). An earlier session had already guarded against exactly that,
by making `renderingMap()` (and `postRenderingMap()`'s internal capture
step) return early whenever `dusk::vr::isRenderingToHeadset()` was true.
The bug: that flag is true for the *entire* VR-rendering `tick()` call
(set at `vr_main.cpp` ~line 506, well before the per-eye loop even starts),
not just while an eye pass is actually open — despite older comments
nearby assuming the narrower meaning. So the guard fired on every single
call, every frame, meaning the minimap's texture **never rendered at all
during VR** and stayed at whatever garbage was in that GPU memory at
allocation time. The minimap's on-screen picture (`mMapJ2DPicture`) was
still being composited correctly as part of the already-fixed HUD
billboard (section 7) — it was faithfully displaying a texture that
simply never got written.

**Fix** (mirrors section 7's HUD billboard architecture exactly): added a
new, narrower flag/accessor, `dusk::vr::isEyePassOpen()`
(`vr_main.hpp`/`.cpp`, backed by `g_duskVREyePassOpen`), true only between
a given `beginEye()` and its matching `endEye()` inside the per-eye loop —
unlike `isRenderingToHeadset()`, which is true for the whole frame. Added
`mDoGph_gInf_c::captureMapCopy2D()` (`m_Do_graphic.cpp`, declared in
`m_Do_graphic.h` next to `captureHudBillboard()`), which reproduces the
same `J2DOrthoGraph`/`dComIfGp_setCurrentGrafPort()` setup
`mDoGph_Painter()` does right before its own `dComIfGd_drawCopy2D()` call
(needed because `postRenderingMap()` reads back the current graf port to
call `setup2D()`), then calls `dComIfGd_drawCopy2D()` itself. Called once
per frame from `vr_main.cpp`'s `tick()`, right after
`captureHudBillboard()` and before the per-eye loop opens any eye pass —
the same safe window HUD's capture already uses. Changed
`d_map_path.cpp`'s two guards (`renderingMap()`'s early-return and
`postRenderingMap()`'s `skipCapture`) from `isRenderingToHeadset()` to
`isEyePassOpen()`, so the minimap now actually renders during this safe
pre-loop window but still correctly no-ops during the redundant per-eye
call from inside `mDoGph_Painter()` (avoiding the nested-offscreen-pass
crash the original guard existed to prevent).

**Confirmed fixed in-headset same session** — user: "Surprisingly easy fix.
Bug closed." Since the pause-screen map (`dMenu_FmapMap_c`) funnels through
the exact same `dRenderingMap_c::renderingMap()`/`postRenderingMap()`
guards, it should be fixed by the same change, but this was not separately
confirmed in-headset — if it's ever reported still broken, start here
rather than assuming a new bug.

**Lesson worth remembering**: `g_duskVRRenderingToHeadset`/
`isRenderingToHeadset()` reads like a "we are currently rendering an eye"
flag from its name, but it's actually scoped to the whole `tick()` call
once a gameplay view is ready — not to the narrower "an eye's protected
offscreen pass is currently open" window. Any future code that needs to
know specifically whether nesting a `GXCreateFrameBuffer` right now would
crash should check `isEyePassOpen()`, not `isRenderingToHeadset()` — the
same mistake (assuming the broader flag meant the narrower thing) is what
caused this bug in the first place, and the flag's own old comments
predating this fix still described it inaccurately.

### 9. Model/mesh-level frustum culling behind Link's facing direction — FIXED 2026-07-30 (built, NOT yet tested in-headset)

Distinct from section 2's `mDoLib_clipper` fix (the actor-visibility
frustum, already fixed) — this is a separate clipper class, `J3DUClipper`,
used for per-mesh J3D model geometry culling (background objects: rocks,
buildings, etc.). **Symptom**: objects fully disappear (not just fade, per
section 2's already-fixed issue) when Link faces away from them — because a
VR headset's head can look around independently of Link's body-facing
direction, but this clipper's frustum was still derived from the stale
flatscreen-camera-facing direction, same underlying class of bug as
section 2 just in a different subsystem.

**Found via the user's direct contact with the dusklight devs**: the
(excluded-from-build) `shadow_mod` mod already ships a "No Frustum
Clipping" toggle (`mods/shadow_mod/src/mod.cpp`,
`g_cvarNoFrustumClipping`/`on_frustum_clip_pre()`) that works by
runtime-hooking both `J3DUClipper::clip` overloads via the project's
mod-hook framework (`DEFINE_HOOK`/`hook_add_pre`) and forcing the return
value to `0` (not culled) while active. The dev's guidance: this can be
done directly in `J3DUClipper::clip` itself, without needing `shadow_mod`
or its hooking machinery at all.

**Fix applied directly in `libs/JSystem/src/J3DU/J3DUClipper.cpp`**: both
`clip()` overloads now check `g_duskVRRenderingToHeadset` (declared
`extern "C" bool`, same pattern already used in `d_drawlist.cpp`) at entry
and return `0` immediately when true — culling never fires while rendering
to the headset. Flatscreen keeps normal culling (a real perf optimization
there, and not reported as buggy on flatscreen) — deliberately scoped to
VR only via this flag, unlike shadow_mod's global on/off toggle.
`J3DUClipper.cpp` only compiles into the `JSystem_J3DU` static lib
(`files.cmake`), which is not linked into any mod DLL build, so — unlike
`d_com_inf_game.h` (see section 1's Round 1 fix) — no
`TARGET_PC`/`DUSK_BUILDING_GAME` guard is needed here.

**Status: built successfully (RelWithDebInfo), NOT yet confirmed
in-headset.** Next step for whoever picks this up: launch in VR, turn to
face away from a known object (rock/building) that previously vanished,
confirm it now stays visible, and check for any new problems (e.g.
increased draw calls/perf cost from disabling this optimization, though
given section 7's HUD billboard perf numbers this is expected to be
negligible against the VR frame budget).

**Unrelated incident hit while building this fix**: `extern/aurora/lib/gfx/
common.cpp`'s `wait_for_gpu_progress()` had its closing `}` replaced with
garbled text (`Ty`) again — a further occurrence of the exact pattern
flagged in section 7's "Unrelated incident" note (previously `Why ca}`,
then `Why caYea}`). Fixed by restoring the plain `}`; this is the third
time this exact line has been corrupted mid-session, reinforcing that it's
something on the user's end (an editor/dictation/autocomplete tool with
focus on that file) rather than a regression from any code change in this
project. If the build ever fails at this exact spot again, check here
before assuming a real regression.

### 10. Goron Mines "heat wave" effect — ROOT-CAUSED AND REMOVED 2026-07-31

**Symptom** (user-confirmed, VR only): entering Goron Mines showed "a bunch
of squares flying up" that "have a similar effect to the heatwaves where I
see my view duplicated and displayed in the texture" — visually related to
section 5's floating-portal bug, but Goron Mines had already been checked
clear of every kagerou spawn site section 5 found and fixed. User wanted
this removed entirely (not VR-gated) for both VR and flatscreen, unlike
this project's usual "disable in VR only" pattern for this class of bug —
an explicit, one-off choice for this location.

**Method**: same as section 5 — the existing `[dusk::particle]
set`/`setSimple` diagnostic logging (still in the tree from that session,
gated on `g_duskVRSessionActive`) was already sufficient; no new logging
needed. User reproduced in-headset, pasted Output-window lines, three
separate rounds of "still there" narrowed it to three distinct spawn
sites — each confirmed fixed (absent from the next log) before moving to
the next, rather than guessing all three up front.

**Three separate spawn sites found, all sharing the same root mechanism**
(the "dummy"-texture live-screen-capture substitution documented in
section 5 — `dPa_control_c::createCommon()`/`createRoomScene()` swap any
particle literally textured `"dummy"` for the shared screen-capture
texture; VR's stereo/per-eye ambiguity turns that into a duplicated-scene
artifact):

1. **Magma Pole head-burst** (`d_a_obj_firepillar2.cpp`,
   `daObjFPillar2_c::actionOnInit()`, `KIND_MAGMA_POLE`): the erupting
   lava-geyser hazard's "head" burst VFX, ids `l_yogan_headS/M/L_id`
   (`0x84E4`–`0x84EC`, i.e. `ID_ZI_S_YOGANBASHIRA_{S,M,L}_HEAD_{A,B,C}` —
   "Yougan Bashira" = lava pillar). Found by cross-referencing the user's
   very first log (`0x84e7-0x84e9`, masked via `dPa_RM`'s `0x8000` flag —
   see `getRM_ID()`/`dPa_group_id_change()` in `d_particle.cpp`) directly
   against the particle-name header's `/* 0x4E7 */` comments. Disabled
   unconditionally by removing the whole spawn loop; the pillar's
   hazard/damage/animation logic is untouched, only this burst VFX is gone.
2. **Pipe Fire jet** (same file, same actor class's `KIND_PIPE_FIRE`
   variant — a separate, continuous upward fire-jet hazard sharing the
   same `Obj_yogan` JPA archive): both its idle pilot-light particles
   (`Create()`'s `0x84df`/`0x84e0`) and its rate/lifetime-driven directional
   jet (`actionOnWaitInit()`'s `l_pipe_fire_id`, `0x84E1`-`0x84E3`)
   disabled the same way. This was the strongest match for "flying up" of
   the three, being a sustained jet rather than a one-shot burst — but
   turned out not to be the whole story either.
3. **`daYkgr_c` ("Dragon Mountain Heat Haze")** — the actual primary
   cause, found last: `src/d/actor/d_a_ykgr.cpp`, internal HIO label "竜の
   山陽炎" (literally "Dragon Mountain Heat Haze"), matching the
   `AK_SP_MtDragonKagerou.jpa` asset name noticed early in this
   investigation but never traced to a call site until the log pointed
   back to it. Spawns `0x80e2` = `dPa_RM(ID_ZI_S_SCREENKAGEROU01)` — the
   same dedicated "screen kagerou" id already seen (harmlessly) in
   `d_kankyo.cpp`'s `#if DEBUG`-only HIO test menu, but invoked here via a
   raw hex literal rather than the id constant's name, which is why the
   original whole-codebase name grep in section 5 missed it. Confirmed
   Goron-Mines-specific via `_draw()`'s `strcmp(dComIfGp_getStartStageName(),
   "D_MN04A") == 0` check (a Goron Mines room, tied to boss health via
   `dComIfGs_BossLife_public_Get()`). **Why this one actually matched the
   symptom**: unlike the other two (world-space particles at a fixed
   hazard), `daYkgr_c::set_mtx()` re-anchors the effect to the **camera's**
   eye position/lookat direction every frame — i.e. it's camera-locked, not
   world-locked. A VR headset's free head rotation sweeping a camera-locked
   screen-capture distortion effect is exactly what would read as "a bunch
   of squares flying up" as the view whips around, far more than a
   world-anchored hazard effect only breaks when you're standing right
   next to it. Disabled by returning `cPhs_COMPLEATE_e` at the top of
   `_create()` before the particle-set call — this actor exists solely to
   drive this one effect (no collision, no other gameplay role), so
   skipping creation entirely (rather than just skipping the spawn and
   leaving a dangling emitter-less instance) is the clean fix.

**User-confirmed fixed in-headset** after the `daYkgr_c` fix specifically —
the first two fixes alone were each confirmed (via absence from the next
log) to have actually stopped spawning, but neither alone stopped the
visible symptom, meaning genuinely all three needed fixing rather than the
first match being sufficient. **This matches section 5's "grep the entire
codebase for every other call site" lesson exactly, but one level harder**:
that lesson assumed every spawn site shares a common id name to grep for —
here, three *different* ids across three *different* archives all fed the
same visual symptom, and one of the three was invoked by numeric literal
with no name at its call site at all. **Reusable lesson**: when a "dummy"-
texture-style bug resists a single fix, don't assume the first confirmed-
and-disabled instance was the only one just because it's a strong
circumstantial match (world-space "flying up" particles felt like an
obvious fit) — camera-locked/screen-anchored effects are a categorically
different (and easy to overlook) instance of the same underlying
mechanism, and are actually the more VR-disruptive case since they track
head movement directly rather than requiring proximity to a hazard.

**Scope note**: per explicit user request, all three fixes here are
unconditional (no VR guard) — this removes the effects on flatscreen too,
unlike every other kagerou/heat-wave fix in this file. If a future session
is asked to restore flatscreen's heat-wave visuals in Goron Mines
specifically, these three sites (not section 5's candle/torch/sun sites)
are where to look.

### 11. VR camera anchored to Link's head (true first-person) — FIXED 2026-07-31

**Goal** (explicit user request, first of a planned three-part sequence —
see also the not-yet-started "hide arms/ears/hat" and "physical tracked
hands" follow-ups discussed the same session, not written up here since
neither has been started): put the VR camera at Link's actual head
position during normal gameplay, instead of the pre-existing behavior of
anchoring HMD head-tracking on top of the flatscreen third-person camera's
eye position (correct stereo/no-crash, but never actually first-person).

**Where the anchor point lives**: `vr_stereo_render.hpp`'s
`eyePoseToViewMtx()` already composed the view matrix as "HMD positional
delta from `hmdRefPos`, scaled/Z-flipped, added onto a `linkEyeGame`
world-space anchor" — this pre-dates this session (see section on the
cutscene-out-of-bounds fix). Previously `linkEyeGame` was always
`view->lookat.eye` (the flatscreen camera's own eye, whatever the base game
computed that frame — normal follow-cam or an authored cutscene camera).
This session added a new `EyeParams::eyeAnchor` field, computed ONCE per
frame (not per eye — same value fed to both) in `vr_main.cpp`'s `tick()` via
`vr_link::getVrCameraEyeAnchor()` (new, `vr_link_visibility.hpp`), and used
in place of `view->lookat.eye` at `beginEye()`'s `eyePoseToViewMtx()` call
site.

**The anchor value itself**: `daAlink_c::getSubjectEyePos()`
(`field_0x3768`) — the SAME value `d_camera.cpp` already reads for its own
default camera-attention fallback, already computed every sim tick by
`setBodyPartPos()`, and already form-/mount-aware for free (wolf form uses
a different joint + local offset internally; canoe/board/horse mounts each
get their own offset branch). No new position-tracking code was needed —
the game already tracks where Link's eyes are, this just reads it.

**Two fallback cases, both intentionally still third-person** (return the
caller's `view->lookat.eye` instead of the head anchor):
- **Cutscenes/events** (`daAlink_c::checkEventRun()`): an authored cutscene
  camera isn't guaranteed to be looking at Link at all — snapping to his
  head there would put the viewer inside his skull for shots never
  designed to be seen from there. This narrows (doesn't remove)
  `eyePoseToViewMtx()`'s original design guarantee that `view->lookat.eye`
  works "whether that's normal follow-cam or an authored cutscene camera"
  down to cutscenes specifically.
- **Wolf form** (`daAlink_c::checkWolf()`, a base-class player-state flag —
  `d_a_player.h`): added per explicit user request the same session, as a
  deliberate, permanent form-dependent split — **first-person as human,
  third-person as Wolf Link**, always, not just a temporary limitation.
  Reasoning given: Wolf Link's model/gait/head joint are different enough
  (four-legged, separate `wlLocalEye` branch in `setBodyPartPos()`) that
  the wolf's own head was never designed to be viewed from inside it. Both
  fallback cases reset `getVrCameraEyeAnchor()`'s interpolation state
  (`s_eyeAnchorValid = false`) so gameplay doesn't lerp FROM a stale
  pre-cutscene/pre-transformation head position the next time first-person
  resumes.
- **User has explicitly flagged wanting to revisit this later**: possibly
  making SOME cutscenes first-person after all (not proposed as "remove the
  guard" — cutscenes vary too much in whether they're even looking at Link
  for a blanket flip; would need per-cutscene opt-in, not attempted this
  session). If picked up, `getVrCameraEyeAnchor()`'s `checkEventRun()`
  branch in `vr_link_visibility.hpp` is the place to start.

**Jitter bug found and fixed same session** (first in-headset test after
the initial implementation): camera motion AND Link's own nearby body both
looked badly jittery. **Root cause**: `getSubjectEyePos()`/`field_0x3768`
is only recomputed once per SIM TICK (`setBodyPartPos()`, called from
`fapGm_Execute()` inside `m_Do_main.cpp`'s fixed-rate sim-tick loop —
GameCube-era game logic, well under VR's 72-90Hz render rate). Reading it
directly every render frame stair-steps: the value only changes once every
few frames, which combined with the HMD's continuously-updating tracking
delta on top reads as jitter — and because the camera now sits right at
Link's head, that stair-stepping made his own nearby body geometry look
like it was jittering too, even though his mesh itself still rendered
smoothly in world space (unaffected — see below). `view->lookat.eye` never
had this problem because `dusk::frame_interp` (`frame_interpolation.cpp`,
pre-existing) already lerps IT every render frame between the previous and
current sim tick's recorded camera position
(`s_cam_prev`/`s_cam_curr`/`interp_view()`).

**Fix**: reproduced that exact same prev/curr-snapshot-and-lerp technique
for Link's head position specifically, entirely from VR mod code (no core
engine changes) — see `vr_link_visibility.hpp`'s `getVrCameraEyeAnchor()`
and its `detail::` namespace state. New-sim-tick detection uses
`dusk::frame_interp::sim_tick_seq()` (incremented once per real sim tick
via `begin_sim_tick()`) rather than `is_sim_frame()` — **non-obvious
gotcha**: by the time `vr_main.cpp`'s `tick()` runs each frame,
`m_Do_main.cpp` has already unconditionally reset `is_sim_frame()` to
`false` for the presentation phase (its `begin_frame()` call sequence
flips it false right after the sim-tick loop, before `dusk::vr::tick()` is
ever reached), so `is_sim_frame()` can't tell VR code whether THIS
iteration actually ran a sim tick — `sim_tick_seq()` changing is what
actually works. `get_interpolation_step()` was confirmed (by reading both
`begin_frame()` call sites in `m_Do_main.cpp`) to stay populated from
`game_clock` regardless of the user's "enable frame interpolation" setting
— only `interp_view()`'s OWN early-out respects that setting — so this
head-position lerp doesn't need its own separate settings gate. **Why
Link's body itself was never actually broken**: his mesh is drawn through
the game's normal (already-interpolated) draw pipeline every eye, same as
flatscreen — the "jitter" was entirely the camera stair-stepping around a
smoothly-rendered body, not the body's own animation being wrong. This is
worth remembering if a similar "reads real gameplay state directly, looks
jittery in VR" symptom shows up elsewhere in this project: check whether
the value being read is sim-tick-rate (raw) vs. already going through
`dusk::frame_interp` before assuming a new bug.

**Confirmed fixed in-headset** (movement smooth) and confirmed working for
the wolf/human third-person/first-person split, same session.

**Not yet tested** (explicitly flagged by user, not started): Epona (horse)
riding and snowboarding. Both already have dedicated offset branches inside
`setBodyPartPos()` (`horseLocalEyeFromRoot`, `boardLocalEyeFromRoot`) that
`getSubjectEyePos()` picks up automatically — so these are *expected* to
work with zero additional code, but this has not been confirmed in-headset
and should not be assumed correct until it is. If either looks wrong, start
by confirming in-headset which of the two conditions is actually active for
that mount (`dComIfGp_checkPlayerStatus0(0, 0x2000)` / the
`checkCanoeRide()`/`checkBoardRide()`/`checkReinRide()` branch in
`setBodyPartPos()`) before assuming the interpolation or anchor logic
itself is at fault — those offset branches are pre-existing base-game code,
not something this session touched or can rule out independently.

### 12. VR tracked hands — POSITION FIXED, ROTATION STILL WRONG (unresolved, follow-up needed)

**Goal** (third of the three-part VR embodiment plan — camera done in
section 11, arms/ears/hat hiding done separately, this is the last piece):
drive `mpLinkHandModel`'s two hand joints from real controller poses, so
Link's existing hand geometry (sword/shield already attached to it) tracks
the player's actual hands in VR.

**Real OpenXR controller input didn't exist at all before this session.**
`g_rightGripSpace`/`g_leftGripSpace` (`vr_main.cpp`) were bare
`XR_NULL_HANDLE` globals nothing ever assigned — `locateSpace()`'s identity
fallback meant `vr_link_visibility.hpp`'s pre-existing (but never-actually-
tested-with-real-data) `buildHandMtx()`/`FrameInput` consumers had been
rendering hands at tracking-space origin the whole time. Fixed by adding a
real `XrActionSet`/pose `XrAction`/`XrActionSpace` setup
(`vr_xr_bootstrap.hpp`'s `createHandActionSet()`/
`attachAndCreateHandSpaces()`, called from `startup()`; one POSE action
with `/user/hand/left` and `/user/hand/right` subaction paths, bindings
suggested for `khr/simple_controller` plus the native Touch/Vive/Index
profiles), plus a per-frame `xrSyncActions()` call in `tick()` before the
grip spaces are located. **Confirmed working** — real, continuously
changing grip-pose data flows in (verified via logged position deltas
swinging over a full meter-plus range while the user waved a hand, with
`eyePos`/Link's own position held still).

**Joint mapping bug found and fixed**: `HAND_ROOT_JOINT = 0` (assumed,
never verified) turned out to be `mpLinkHandModel`'s `world_root` joint —
the shared PARENT both hands hang off of, not either hand specifically.
Confirmed via a one-time joint-name dump (same technique as section 2's
material-name dump): `mpLinkHandModel` has exactly 3 joints — `0
world_root`, `1 al_handsL`, `2 al_handsR`. Fixed to `LEFT_HAND_JOINT = 1`,
`RIGHT_HAND_JOINT = 2`, and the previously-never-implemented left hand was
wired up alongside the right.

**"Controllers do nothing" bug, root-caused and fixed**: even with correct
joint indices and confirmed-real pose data, hands still only showed normal
body animation. Root cause: `d_a_alink.cpp`'s `setDrawHand()`-adjacent
draw-prep code unconditionally re-syncs `mpLinkHandModel`'s joints 1/2 from
the BODY model's own current hand-joint matrices, EVERY EYE, immediately
before `modelDraw(mpLinkHandModel, ...)` actually draws it —
`mpLinkHandModel->setAnmMtx(1, mpLinkModel->getAnmMtx(9))` /
`setAnmMtx(2, mpLinkModel->getAnmMtx(0xE))`, present already with the
comment "Always set these, otherwise the hands occasionally zip to
origin." Since this runs AFTER `vr_link::updateFrame()` (which fires once
per frame, before the per-eye loop even opens), it was silently
overwriting the tracked pose every single eye before anything reached the
screen. Fixed by caching the computed hand matrices in
`vr_link_visibility.hpp` (`detail::s_rightHandMtx`/`s_leftHandMtx`) instead
of writing them directly in `updateFrame()`, and adding
`dusk::vr::applyTrackedHandMtx()` (thin forward to
`vr_link::applyTrackedHandMtx()`, same "keep the heavier OpenXR header out
of core game files" pattern as `drawHudBillboard()`) called from
`d_a_alink.cpp` immediately AFTER that body-joint re-sync, guarded on
`isRenderingToHeadset()` — making the tracked pose the LAST write before
the draw, every eye, instead of the first.

**Position: fixed, confirmed working.** Two bugs found:
- **Wrong anchor**: `updateFrame()` was anchoring hands to `view->
  lookat.eye` (the old third-person camera eye) instead of
  `vr_link::getVrCameraEyeAnchor()` (section 11's head-anchor, what the VR
  camera actually renders from) — hands tracked relative motion correctly
  but were offset from wherever the player's own view actually was. Fixed
  by calling `getVrCameraEyeAnchor()` in `updateFrame()` too (forward-
  declared earlier in the file; harmless to call twice a frame since it's
  idempotent within a frame — see section 11's `sim_tick_seq()` gating).
- **Front/back mirrored**: `buildHandMtx()`'s position formula used
  `linkEyeGame.z - dz * scale` (a "flip Z" inherited from thinking it
  needed to match the camera code's convention). User report was precise —
  "front" and "behind" specifically swapped, not a general direction bug —
  and removing the flip (`+ dz * scale`) fixed it outright. **Confirmed
  in-headset**: sweeping a hand through a large motion tracks correctly at
  the correct position, matching where the real controller is.

**Rotation: STILL NOT FIXED after two full sessions of attempts.** This is
by far the hardest unsolved piece of the whole VR mod. Full history below,
including a session where the debugging METHOD itself improved
substantially (isolated single-axis motion capture + script-verified math
instead of guessing) but the actual fix still didn't land. Read the
"reusable lessons" at the end before attempting this again — several
things that FEEL like the obvious next step (compose one more correction,
swap two columns) are proven traps below.

**Session A (rounds 1-5, all guessed corrections, ended in a full revert
to raw/unflipped quaternion for both hands):**
- Round 1 (qz-unflip): fixed an initial pitch/roll-axis SWAP (tilting the
  controller rolled the hand, rolling it tilted) by removing a `qz = -q.z`
  flip in the rotation-matrix construction that mirrored the position
  flip — `eyePoseToViewMtx`'s own comment (camera code, validated) already
  documented this exact mistake. This part is correct and is still in the
  code (folded into `rotateVecByQuat`'s unflipped `q.z` usage).
- Rounds 2-3 (compound 90° guess, then a composition-order bug): guessed
  90°-ish corrective quaternions composed by hand, one of which was
  composed in the wrong order (`(rawQ * offsetX) * offsetZ` actually
  applies Z first, X second — backwards from intent) and produced a
  cyclic 3-axis permutation (yaw input → pitch output → roll output → yaw
  output) as a result. **Lesson, still true**: composing corrective
  quaternions by hand is extremely easy to get backwards under pressure,
  and a wrong-order composition doesn't degrade gracefully — it produces a
  qualitatively different, confusing failure that looks like a brand new
  bug.
- Round 4 (mirrored-mesh left-hand guess) and round 5 (a from-real-data
  calibration attempt, reported WORSE than doing nothing, fully reverted).
  See prior version of this file (git history) for the blow-by-blow if
  ever needed — superseded by Session B's cleaner methodology below.

**Session B (this session's continuation) — isolated single-axis motion
capture + script-verified math, real progress on METHOD, rotation still
not resolved:**
- Captured three SEPARATE, slow, isolated single-axis controller motions
  (roll, yaw, pitch — each done as "hold steady, slowly rotate ~90° over
  3-4 seconds, hold steady", not a quick snap) via `[dusk::vr::handrot]`
  logging (raw quaternion only). Analyzed with Python scripts (NOT by
  hand) that segment the quaternion stream into "motion runs" and compute
  the actual world-frame rotation axis between before/after samples via
  axis-angle decomposition — this is a MUCH more reliable data source than
  a verbal "it looks rotated" description, and is worth reusing as-is if
  this is picked up again.
- Derived `kLocalRight`/`kLocalUp`/`kLocalForward` (in
  `vr_link_visibility.hpp`, `right_hand_cal` namespace) as the
  MOTION-DERIVED local axes (un-rotating each measured world-frame
  rotation axis by that test's own "before" orientation) rather than an
  assumed static target — this fixed a real methodology flaw from
  session A's round 5 (which used an assumed, unverified static target for
  "up" and cross-validated 100+ degrees off). Gram-Schmidt orthogonalized
  to force exact perpendicularity (the raw motion data was ~10° off
  perpendicular between roll and yaw, consistent with ordinary hand-motion
  imprecision).
- **Confirmed, by direct measurement, that the axis MAPPING itself is
  correct**: a dedicated isolated pitch capture showed `kLocalRight` is
  99.88%-aligned with the real measured pitch rotation axis. Later,
  re-testing the ORIGINAL, completely unmodified calibration (right=
  kLocalRight, up=kLocalUp, forward=kLocalForward, zero corrections)
  directly against all three real motion captures simultaneously confirmed
  it cleanly discriminates all three: forward drifts 0.0° during roll,
  right drifts 3.2° during pitch, up drifts 11.8° during yaw (matching the
  known ~10° calibration imprecision, still far smaller than the ~70-108°
  the OTHER two axes move during each test). **This mapping has never
  actually been wrong** — see the critical mathematical lesson below for
  why the several "it's swapped, let me swap two columns" fixes attempted
  along the way could never have been genuine repairs.
- **Critical mathematical lesson, the main reusable insight from this
  whole session**: swapping which vector feeds two of the three dest
  columns (even with a sign flip to preserve a proper, non-mirrored
  rotation) is NOT a "relabel the mesh's semantics" operation — it is
  ALWAYS mathematically equivalent to applying some fixed 90°/180°
  rotation to the entire right/up/forward frame at once (verified this
  algebraically: e.g. swapping right and forward columns with a
  negation is identical to a fixed 90° rotation around the "up" vector
  that stayed unchanged). And a fixed rotation applied uniformly to all
  three basis vectors, by a conjugation-identity proof done this session
  (`Rotate(angle, R*axis) = R*Rotate(angle, axis)*R^-1`), provably CANNOT
  change which physical motion (pitch/roll/yaw) maps to which visual
  rotation TYPE — it can only change the static resting orientation. So
  when a "pitch looks like roll" symptom appeared to go away after a
  column swap, that was either (a) a coincidence of the static
  orientation changing enough to fool the eye during a quick check, or
  (b) the axis-confusion report was never really about axis TYPE at all,
  just an extremely-wrong static orientation making a correct pitch LOOK
  like a roll to the observer. **Do not attempt another column swap as a
  fix for "X acts like Y" — it cannot work, by the math above. If that
  symptom reappears, the axis mapping itself needs re-verification via the
  isolated-motion-capture method (which has never actually shown it to be
  wrong), not another swap.**
- Given the mapping was confirmed correct, all guessed 90°-at-a-time
  static corrections (pitch-down-90, yaw-left-90, yaw-right-90 which
  canceled the previous one out entirely since both are fixed rotations
  around the identical world axis, then yaw-180) were removed, and a
  SINGLE precisely-computed correction matrix was derived instead: solved
  directly as `M = target_frame * current_frame^T` (current_frame's
  columns are orthonormal so its transpose is its inverse), targeting
  world `(0,1,0)` for up and the roll-test's own logged
  `[dusk::vr::camrot]` forward direction (horizontally projected) for
  forward. Verified before deploying: `det(M) = +1.0` (proper rotation,
  confirmed not an accidental mirror) and `M` exactly reproduces the
  target frame at the reference orientation (self-check passed exactly).
  **User-tested, still not correct.** Since the mapping is independently
  confirmed right and the correction matrix is verified bug-free by
  construction, the remaining error must be in the INPUT DATA the matrix
  was derived from, not the math — see next point.

**Leading unverified suspect for why it's STILL wrong**: the correction
matrix's "forward" target came from `[dusk::vr::camrot]`'s logged camera
direction at the roll-test's timestamp, horizontally projected — i.e. an
assumption that "the camera was looking roughly where the controller was
pointed" during that test. This was flagged as a risk from the very first
time this proxy was used (session A round 5) and has never actually been
verified. If the player wasn't looking directly at their hand during the
roll capture (quite plausible — nothing enforced it), the "forward"
target itself is wrong by however many degrees their gaze was off, and a
mathematically-perfect correction built from a wrong target still gives a
wrong result. This is the single most likely place to look next.

**Concrete next steps for a future session**, in order of how promising
they seem:
1. **Verify or replace the camera-forward proxy.** Either (a) capture a
   NEW reference pose where the player is deliberately, verifiably looking
   directly along the controller's pointing direction (e.g. sighting down
   the controller like a rifle, confirmed by the player themselves, not
   inferred), or (b) find a reference that doesn't need the camera at all
   — e.g. OpenXR's own documented grip-pose axis convention (looked up
   directly from the spec/runtime docs, not inferred from this project's
   data) combined with a pure gravity reference for "up" fully determines
   both targets without knowing where the player was looking.
2. If a new correction matrix is computed, verify it in Python FIRST
   (unit length, orthogonality, self-check against the reference sample it
   was built from) exactly like this session did — that part of the
   process worked correctly and caught real errors before they reached
   the headset.
3. Do NOT re-attempt a column swap to fix an "X acts like Y" symptom — see
   the mathematical lesson above. If that symptom appears again, re-run
   the isolated-motion-capture verification (scripts already exist and
   worked cleanly this session) before assuming the mapping changed.
4. Left hand has no calibration at all yet (still raw/unflipped) — once
   the right hand is genuinely confirmed correct, the same isolated-motion
   capture method needs repeating for the left controller specifically
   (the meshes are presumed mirrored, per session A round 4's "left hand
   upside down, right hand fine" report, so the right hand's calibration
   cannot simply be reused or trivially negated without its own
   verification).

**What's still in the tree**: `logCameraBasisPeriodically()`
(`vr_link_visibility.hpp`, called from `updateFrame()`) — logs the HMD's
own up/forward world vectors every ~45 frames as `[dusk::vr::camrot]`,
using the identical quaternion-to-matrix formula `buildHandMtx()` uses.
`logHandPosesPeriodically`-equivalent raw-quaternion logging
(`[dusk::vr::handrot]`) also still fires from `buildHandMtx()` itself.
Both left in deliberately (harmless, no gameplay effect) since this exact
kind of paired camera+hand real data, captured via slow isolated-axis
motions, is what actually produced verifiable progress this session (the
confirmed-correct axis mapping) even though the final static correction
still isn't right.

**Reusable lessons for whoever picks this up next** (compounding on top of
session A's lessons above):
- The mathematical lesson about column swaps (above) is the single most
  important thing to internalize before touching this again — it would
  have saved most of this session's later rounds.
- Isolated, slow, single-axis motion capture + script-based (not by-hand)
  analysis is a genuinely reliable methodology now — reuse it rather than
  reasoning abstractly about which axis "should" do what.
- Always verify a derived correction (matrix or quaternion) with a
  standalone script BEFORE building/testing in-headset: check it's a
  proper rotation (determinant +1, or equivalently that its rows/columns
  are unit length and mutually orthogonal) and that it reproduces its own
  reference sample exactly. This catches real bugs cheaply.
- A "it's fixed" or "it's still broken" report from a quick in-headset
  glance is a much noisier signal than the actual measured data — when
  they seem to conflict (e.g. a column swap seeming to fix an axis-type
  symptom, when the math says it can't), trust the math and look for what
  ELSE could explain the observation (here: the static orientation being
  so wrong it fooled a quick visual check) rather than the visual report.

**Session C (2026-08-02) — replaced the camera-forward proxy with a real
OpenXR "aim pose" reference, built but NOT yet tested in-headset:**

Directly acted on Session B's #1 next step ("verify or replace the
camera-forward proxy"), option (b): rather than trying to look up the
grip pose's spec convention (which turned out to be moot anyway — the
motion-derived `kLocalForward`/`kLocalUp`/`kLocalRight` vectors are
diagonal combinations, not aligned to any single spec axis, consistent
with real controllers' physical construction not matching the spec's
idealized diagram exactly — so a spec-table lookup wouldn't have been a
usable target on its own), wired up OpenXR's separate, standard **aim
pose** action. Unlike grip pose, aim pose is spec-defined specifically as
"the direction the user would point the controller to indicate a target"
(-Z axis), computed by the runtime from the controller's own tracked
geometry — not from anything this app assumes about where the player was
looking. Grip and aim poses are both available simultaneously from the
same physical controller at every instant, so this needs no special
"hold still and sight down the barrel" reference pose at all (the previous
approach's fatal assumption) — ordinary hand movement during a capture
gives many independent (gripQuat, aimQuat) sample pairs for a proper
least-squares fit instead of trusting one hand-picked data point.

**What was built**:
- `vr_xr_bootstrap.hpp`: `HandActions::aimPoseAction` (a second
  `XR_ACTION_TYPE_POSE_INPUT` action, same two-subaction-path pattern as
  `gripPoseAction`), bound to `/user/hand/{left,right}/input/aim/pose` for
  all four profiles `createHandActionSet()` already suggests bindings for.
  `attachAndCreateHandSpaces()` now also takes `outLeftAimSpace`/
  `outRightAimSpace` and creates those two action spaces alongside the
  existing grip ones.
- `vr_main.cpp`: `g_rightAimSpace`/`g_leftAimSpace` globals, located every
  frame in `tick()` the same way the grip spaces already are (right after
  `xrSyncActions()`), threaded into a widened `vr_link::FrameInput`.
- `vr_link_visibility.hpp`: `FrameInput` gained `rightAimPose`/
  `leftAimPose` fields (calibration-only — `buildHandMtx()` still reads
  only the grip poses for the actual draw pose, unchanged). New
  `logHandCalibrationSample()`, called once per frame from
  `updateFrame()`, logs the right hand's raw grip quat and raw aim quat
  together on one line (`[dusk::vr::handcal] gripQuat=(...) aimQuat=(...)`)
  every 10 frames (~9Hz @ 90Hz) — dense enough that a ~15-20s "wave the
  controller through a bunch of different orientations" capture yields
  several hundred sample pairs.
- Built successfully (RelWithDebInfo) — only `vr_main.cpp` needed
  recompiling, clean link, no new warnings.

**Analysis plan for the next session (not yet run — needs a real
in-headset log first)**: per sample, `current_forward = R(gripQuat) *
kLocalForward`, `current_up = R(gripQuat) * kLocalUp` (existing
motion-derived local axes, `right_hand_cal` namespace,
`vr_link_visibility.hpp` — these are NOT being re-derived, only the
static-correction TARGET is); `target_forward = R(aimQuat) * (0,0,-1)`,
`target_up = R(aimQuat) * (0,1,0)` (OpenXR aim pose convention). Solve for
the single best-fit rotation `M` minimizing squared error between
`M*current_i` and `target_i` across ALL samples and both vector types at
once (Kabsch algorithm / SVD of the cross-covariance matrix) — a proper
least-squares fit over hundreds of real samples, not a single reference
point the way the reverted correction was derived. **Verify in Python
before touching the headset again** (same rule Session B learned the hard
way): confirm `det(M) = +1` (proper rotation, not a mirror) and that `M`
reproduces each individual sample reasonably closely (a tight scatter, not
just the mean) before updating `applyStaticCorrection()`.

**Concrete next step**: launch in VR, wave the right controller through a
variety of orientations (rotate it around, don't just hold it still) for
15-20 seconds, then paste back every `[dusk::vr::handcal]` line from the
Output window. That log is the input to the analysis above — nothing else
is needed to proceed.

**UPDATE (same day, log collected and analyzed) — the aim pose data itself
is broken on whatever runtime this was tested on; DO NOT calibrate from it
as-is.** 177 real samples were collected and run through the Kabsch fit
described above. Two things went wrong, in order:

1. **The naive world-space fit (`M` applied after `rotateVecByQuat`, same
   architecture as the reverted Session B correction) gave a mean residual
   of ~68 degrees** — nowhere close to a valid fit. This is not just "bad
   data", it's a real, generalizable finding about `applyStaticCorrection`'s
   existing shape: a rotation matrix applied to WORLD-space vectors *after*
   `rotateVecByQuat(q, kLocal)` can only ever match the single reference
   orientation it was derived from — it cannot commute with arbitrary `q`,
   so a "static correction" of this shape is mathematically incapable of
   being globally valid across orientations, no matter how precisely its
   coefficients are computed. (This is consistent with, and explains, why
   every previous session's verified-correct-in-isolation matrix still
   drifted wrong in general in-headset movement — the shape of the fix was
   the problem, not just the specific numbers.) **The correct shape applies
   the correction to `kLocalRight`/`kLocalUp`/`kLocalForward` themselves,
   BEFORE `rotateVecByQuat`** (`rotateVecByQuat(q, applyStaticCorrection(kLocalForward))`
   instead of `applyStaticCorrection(rotateVecByQuat(q, kLocalForward))`) —
   a genuine local-frame recalibration, which — unlike the world-space
   version — is mathematically capable of being correct for every
   orientation at once, since it only ever touches the fixed local axes,
   never anything that depends on `q`.
2. **Refitting in local space also failed (mean residual ~70 degrees) — but
   this time because the underlying grip/aim data isn't self-consistent.**
   Per-sample analysis (see the diagnostic scripts run this session, not
   currently checked into the repo) found that `R_grip^-1 * R_aim`, which
   should be a CONSTANT matrix if aim pose really were a fixed local-frame
   offset from grip pose (as the OpenXR spec's own aim-pose definition
   implies), has a constant ANGLE (exactly 60.0000 degrees, std
   0.0006 degrees, across all 177 samples) but a wildly varying AXIS (up to
   144 degrees of deviation) when expressed in the controller's own local
   frame. Re-expressing that same axis in WORLD space instead
   (`R_grip @ local_axis`) collapses it to an almost exactly constant
   direction — `(1, 0, 0)`, deviation under 0.3 degrees across all samples
   — and `Rotate(60deg, world +X) @ R_grip` reproduces the logged `aimQuat`
   to within 3e-5 (float rounding on the logged 5-decimal values) for
   EVERY sample, regardless of how the controller was actually oriented.
   **A real aim pose cannot behave this way** — the whole physical point of
   aim pose is that it's a fixed offset from grip *in the controller's own
   body frame*, so it should rotate together with the controller as the
   user turns their wrist; a world-frame-fixed relationship (independent of
   the controller's actual orientation) is not physically meaningful and
   points to a genuine bug somewhere in the aim-pose codepath for whatever
   runtime this was tested on — most likely the RUNTIME's own OpenXR aim
   pose implementation, not this project's wiring (checked: the
   `vr_xr_bootstrap.hpp` action/binding/space setup for aim pose mirrors
   the already-working grip pose setup exactly, no copy-paste divergence
   found). Notably, a real, previously-documented SteamVR bug exists in
   this exact area ("OpenXR aim pose ... twisted", SteamVR issue tracker) —
   consistent with, though not yet confirmed to be, what's being hit here.

**Concrete next step, revised**: before trying this again, find out (a)
which runtime (SteamVR / Virtual Desktop / Meta Link) and controller type
the calibration capture was done on, and (b) whether aim pose behaves
correctly (i.e. `R_grip^-1 * R_aim` is a genuinely constant LOCAL matrix,
not just a constant-angle/wandering-axis one) on a DIFFERENT runtime — if
so, redo the capture there instead, since the grip-pose data itself (used
for the actual draw pose and for position tracking) has never shown this
kind of problem and is not in question, only aim pose specifically. If
aim pose turns out to be broken on every available runtime, this whole
approach is a dead end and the next session should fall back to a more
carefully-verified version of Session B's camera-gaze proxy (e.g. an
explicit "sight down the barrel, confirmed by the player" reference pose,
captured deliberately rather than incidentally) instead.

**UPDATE 2**: confirmed the 177-sample capture was done on **Virtual
Desktop** — notably NOT SteamVR, so the documented SteamVR aim-pose-twist
bug doesn't explain this after all; this looks like either a Virtual
Desktop-specific runtime issue or something not yet identified. Re-reviewed
`vr_xr_bootstrap.hpp`'s aim-pose action/binding/space setup line by line
against the working grip-pose setup — no divergence found. Added one more
diagnostic before pointing further at "runtime bug" as the conclusion:
`vr_main.cpp`'s `tick()` now logs `[dusk::vr::handcal_flags]`, once a
second, showing whether OpenXR itself reports
`XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT` set for both the right grip and
right aim spaces side by side — rules out "aim action silently isn't
bound/tracked and `xrLocateSpace` is returning some untracked fallback
pose" as a simpler explanation than a genuine runtime bug. Built
successfully. **Next step**: retest (same wave-the-controller-around
capture), and this time also paste back the `[dusk::vr::handcal_flags]`
lines. If convenient, also try the SAME capture on a different runtime
(Meta Link ideally, since it's the native Quest OpenXR runtime and the
most likely to have a spec-correct aim pose implementation) to check
whether this is Virtual-Desktop-specific.

**Not addressed this round, still open**: left-hand calibration (still
raw/unflipped — per Session B's step 4, needs its own motion-capture
verification once the right hand is actually confirmed correct, since the
meshes are presumed mirrored rather than simply negatable). The old
`logCameraBasisPeriodically()`/`[dusk::vr::camrot]` diagnostic is left in
place (harmless) even though this new approach doesn't depend on it —
removing still-possibly-useful diagnostic scaffolding before a bug is
actually confirmed fixed isn't this project's convention.

**UPDATE 3 (2026-08-02, continuation) — new capture on Virtual Desktop
came back USABLE this time, new local-space correction derived and built,
NOT yet tested in-headset:**

A fresh ~160-sample `[dusk::vr::handcal]`/`[dusk::vr::handcal_flags]`
capture was retested on Virtual Desktop (same runtime as the broken
177-sample one above). `handcal_flags` showed both right grip and right
aim spaces fully valid+tracked (`0xf`) across the entire capture, ruling
out "the action isn't bound" again. This time the actual data was usable
— and in the OPPOSITE way the previous capture was broken: `R_grip^-1 *
R_aim` (relative rotation from grip to aim) came back as a genuinely
**constant matrix expressed in the CONTROLLER'S LOCAL FRAME** (a fixed
~60.0000-degree rotation, deviation ~0.001 degrees across all 160
samples) — the opposite of the earlier capture, which was constant in
WORLD frame and wandered by up to 144 degrees in local frame (physically
impossible for a real aim pose). Local-frame constancy is exactly what a
genuine fixed grip-to-aim body offset should look like, so this capture
was treated as trustworthy and used as calibration ground truth. **Not
yet understood why this capture behaved correctly when the last one on
the same runtime didn't** — possible explanations not yet investigated:
a Virtual Desktop update between sessions, a difference in how the
controller was moved during the two captures, or something else. Worth
keeping in mind if a future capture goes back to being broken.

**New correction derived and applied** (`vr_link_visibility.hpp`):
directly acted on the durable finding from the previous update in this
section — the OLD `applyStaticCorrection` applied its matrix to
WORLD-space vectors AFTER `rotateVecByQuat`, which is mathematically
incapable of being correct outside the single orientation it was derived
from. Rederived using this capture's data with the corrected shape:
- Averaged `R_grip^-1 * R_aim` across all 160 samples, SVD-cleaned to a
  proper rotation (`Rrel`).
- Computed aim pose's right/up/forward axes in the grip's own local frame
  as `Rrel @ (1,0,0)` / `Rrel @ (0,1,0)` / `Rrel @ (0,0,+1)`. Note: **+Z,
  not OpenXR's spec convention that -Z is the aim direction** — verified
  directly that this codebase's existing `kLocalRight`/`kLocalUp`/
  `kLocalForward` satisfy `cross(right,up)=+forward`, the opposite
  handedness from OpenXR's own right/up/forward-is-minus-Z convention;
  using -Z produced a matrix with `det=-1` (a mirror) as a clear tell of
  the mismatch, +Z gave `det=+1`. This says nothing about "true" aim
  direction on paper — only that this project's motion-derived
  `kLocalForward` happens to be defined antiparallel to OpenXR's spec
  convention, which is fine as long as it's used consistently.
- Solved `M = target_frame * current_frame^T` (current_frame's columns
  orthonormal, so transpose is inverse), SVD-cleaned. Verified: `det(M) =
  +1.0` exactly, and `M` applied to each of `kLocalRight`/`kLocalUp`/
  `kLocalForward` reproduces its corresponding aim-pose target axis to
  0.0000 degrees.
- **Applied in the corrected shape**: `buildHandMtx()` now calls
  `applyStaticCorrection()` on the LOCAL axis constants themselves, before
  `rotateVecByQuat`, e.g. `rotateVecByQuat(q, applyStaticCorrection(
  right_hand_cal::kLocalRight))` — replacing the old
  `applyStaticCorrection(rotateVecByQuat(q, kLocalRight))` order. This is
  the fix for the "only ever correct at one reference orientation"
  problem identified in the previous update.

Built successfully (RelWithDebInfo), clean link, no new warnings.
**NOT yet tested in-headset** — this is a new, different correction
matrix from anything tried before, derived from data that (unlike every
previous attempt) is both physically self-consistent AND applied in a
shape that's mathematically capable of holding up across general
movement, not just one pose. Still needs a real in-headset test moving
the hand through a variety of orientations (not just holding it at the
calibration pose) before this can be called fixed.

**Concrete next step**: launch in VR (Virtual Desktop), move the right
hand/controller through normal gameplay motions and a range of
orientations, and report whether rotation now tracks correctly in
general — or how specifically it's still wrong if not (e.g. a specific
axis mismatch, a fixed offset, gets progressively worse with movement,
etc. — as specific a description as possible, since "still wrong" alone
doesn't distinguish between "the mapping is somehow still off" and "a new
bug entirely").

**FINAL RESOLUTION (2026-08-02, same-day continuation) — CONFIRMED FIXED
IN-HEADSET. Read this box first if picking up hand rotation again; the
rest of this section is historical detail.**

The above matrix was tested and initially still showed a residual roll
offset ("hand facing right direction but rotation feels off"), which led
to a second, genuinely separate bug being found and fixed:
**`rotateVecByQuat()` itself (`vr_link_visibility.hpp`) was computing
`R(q)^-1` (the INVERSE rotation) instead of `R(q)`** — verified
numerically against a reference implementation (its off-diagonal signs
exactly matched the transpose of the standard active-rotation matrix).
This single bug explains why every previous "verified correct" static
correction across two earlier sessions still drifted wrong under general
headset/controller movement: the correction math was sound, but the
underlying primitive it was being composed with ran the rotation
backwards, which a STATIC correction can partially mask at one reference
pose but not in general. Position tracking was never affected (computed
via a separate plain-vector-addition code path, no quaternion rotation
involved) — this is why position had been confirmed working the whole
time while rotation kept failing.

Fixing `rotateVecByQuat()` invalidated the existing static correction
constants (they'd been empirically tuned against the buggy inverse
rotation), requiring one more calibration pass. This was solved by
systematically testing all THREE possible single-axis local rotation
planes (the only three that exist for a 3-vector orthonormal frame:
(right,up) holding forward fixed, (up,forward) holding right fixed,
(right,forward) holding up fixed) against real screenshots taken at
multiple different controller orientations (neutral, rolled left/right,
pitched up/down) supplied by the user, iterating live with the user
watching for which plane/sign actually produced the correct look — rather
than deriving from photos assumed to share one fixed "neutral" real-world
orientation (see the photo1-5 saga above, which taught the hard way that
holding a consistent ROLL, e.g. "buttons straight up", does NOT mean the
controller was aimed the same way each test, since aim direction is a
separate, uncontrolled degree of freedom — comparing world-absolute
directions across such photos is unreliable, which is why the final
convergence came from testing rotation PLANES directly against live
in-headset feedback across a range of orientations, not from another
single-photo derivation).

**Confirmed by the user in-headset**: rotation now tracks correctly
across roll left/right, pitch up/down, and general movement, matching the
real controller's orientation exactly ("Finalllyyyyyyyyyy it matches the
controller"). Both position AND rotation for the right hand are now fully
working.

**Diagnostic scaffolding removed** (per this project's normal practice, now
that the bug is confirmed fixed): the `[dusk::vr::handrot]`,
`[dusk::vr::camrot]` (`logCameraBasisPeriodically()`), `[dusk::vr::handcal]`
(`logHandCalibrationSample()`), and `[dusk::vr::handcal_flags]` per-frame
`OutputDebugStringA` logs have all been removed from `vr_link_visibility.hpp`
and `vr_main.cpp`. The underlying OpenXR aim-pose action/space plumbing
(`vr_xr_bootstrap.hpp`'s `HandActions::aimPoseAction`, `g_rightAimSpace`/
`g_leftAimSpace`, `FrameInput::rightAimPose`/`leftAimPose`) was
DELIBERATELY left in place rather than removed — it's real, working
infrastructure (not just diagnostic noise) that could be reused directly
for left-hand calibration.

**Left-hand calibration — CONFIRMED FIXED IN-HEADSET 2026-08-03.** Turned
out to need much less work than the right hand: dynamic rotation mapping
came out correct immediately using a plain IDENTITY local basis
(`left_hand_cal::kLocalRight/Up/Forward` = X/Y/Z) combined with the
now-fixed `rotateVecByQuat()` — no motion-capture or aim-pose data capture
needed at all. Only a static resting offset was needed on top of that,
converged on entirely via live in-headset iteration (rebuild → test →
adjust one axis at a time, no photos or derived matrices this time,
unlike the right hand's saga): a Z-axis rotation of 270° (equivalent
-90°), then a Y-axis rotation of 110°, composed in that order on the
identity basis. Several intermediate values (an X-axis rotation that
bounced between -45°/+90°/+20° before landing back on identity/0°; a
since-reverted attempt at zeroing the Z-axis entirely) were tried and
abandoned along the way — full trail in `applyLeftStaticCorrection()`'s
git history if ever needed, not reproduced here. **User-confirmed correct
in-headset**: "The hands look correct." Both hands' position and rotation
are now fully working, closing out the three-part VR embodiment plan
(camera in section 11, tracked hands here).

**Cleanup performed same session**: removed `logLeftHandCalibrationSample()`
and its `[dusk::vr::handcal_left]` per-frame `OutputDebugStringA` log
(`vr_link_visibility.hpp`) — it was added in case the left hand needed the
same aim-pose-capture calibration approach as the right hand, but turned
out unnecessary once identity axes + live iteration proved sufficient, so
it never actually got used for anything. The underlying OpenXR aim-pose
action/space plumbing (`vr_xr_bootstrap.hpp`'s `HandActions::aimPoseAction`,
`g_rightAimSpace`/`g_leftAimSpace`, `FrameInput::rightAimPose`/
`leftAimPose`) was left in place, same reasoning as the right hand's
cleanup above — real working infrastructure, not diagnostic noise, in case
a future session needs it for something else.

The meshes are presumed mirrored (per the very first session's "left hand
upside down, right hand fine" report), which is presumably why the left
hand's final static correction (Z 270° + Y 110°) doesn't match the right
hand's derived matrix — this was never investigated further and wasn't
necessary to confirm the fix works.

### 13. Quest 3 controllers as real gameplay input (buttons/sticks, not just hand visuals) — FIXED 2026-08-03

**Goal** (explicit user request): wire real OpenXR controller input (thumbsticks,
triggers, face buttons) into actual gameplay — movement, camera, attack,
items, pause — as opposed to sections 11/12's camera/hand-tracking work,
which only ever drove *visuals*, never game input.

**Architecture**: extends `vr_xr_bootstrap.hpp`'s existing `HandActions`
action set (same one already used for grip/aim pose) with six new actions
— `trigger_value`/`squeeze_value` (float), `thumbstick` (vector2f),
`primary_click`/`secondary_click`/`menu_click` (bool), each with the usual
left/right subaction paths. Bindings are suggested only for
`/interaction_profiles/oculus/touch_controller` (Quest 3's native profile,
and what SteamVR/Virtual Desktop/Meta Link all report for Touch
controllers) — the other 3 profiles (khr/simple, vive, index) keep only
their pre-existing pose bindings, since their button/axis layouts
genuinely differ and weren't in scope. `vr_main.cpp`'s `tick()` reads all
six actions every frame (right after the existing `xrSyncActions` call),
builds a `PADStatus`, and calls `PADSetVirtualStatus(PAD_CHAN0, ...)` —
**the exact same mechanism the touch-screen overlay already uses**
(`touch_controls.cpp`'s `sync_virtual_input()`) to inject input into
`PADRead()` (`extern/aurora/lib/dolphin/pad/pad.cpp`), which merges it
into the real controller-port status every frame. This means
`mDoCPd_c::getTrigA/getHoldX/getStickX(...)` etc. — what `d_a_alink.cpp`
and the rest of gameplay actually read — see it with **zero actor-code
changes**.

**Mapping** (mirrors the game's existing default Xbox-controller layout,
`extern/aurora/lib/dolphin/pad/pad.cpp`'s SDL default binding table, so it
behaves like a normal gamepad): left thumbstick → main stick (movement),
right thumbstick → C-stick (camera), left trigger → analog L, right
trigger → analog R (raise shield), right squeeze/grip → Z (target
lock/call, digital, >50% threshold), right A → context action, right B →
attack, left X/Y → assigned items, left menu button → Start (pause).

**A swing-gesture-to-attack feature was drafted then explicitly deferred**
per user request ("remove the swing controls for now, that's something
for another session") — while wiring this up, a genuinely dead
`if (!pacing.is_interpolating)` gate around the pre-existing (never
actually working) `g_rightSwing.update()` call was found and fixed
(that condition can never be true inside `tick()`, since `tick()` is only
ever invoked FROM the `if (pacing.is_interpolating)` branch in
`m_Do_main.cpp`), and the old handoff-doc note's `PAD_BUTTON_A` was
corrected to `PAD_BUTTON_B` (confirmed via `d_a_alink.cpp`'s
`METER2_USEBUTTON_B` gating on `BTN_B` — B is attack, A is the
context-action button). Both fixes are harmless and left in place, but the
actual OR-into-B wiring was pulled back out per the user's request — not
tested/tuned. `g_rightSwing`/`vr_swing_detector.hpp` are untouched,
ready to pick back up.

**Root cause of "nothing happened in game with controller presses" (the
actual bulk of this session) — THREE separate, independent per-frame
`PADClearVirtualStatus(PAD_CHAN0)` call paths, all needing to be found and
fixed one at a time via direct evidence, not guessing**:

OpenXR input itself was confirmed correct almost immediately (real
trigger/button/stick values, `isActive=true`, `xrSyncActions` succeeding —
see the diagnostic-logging trail in git history if ever needed) — the
entire remaining investigation was about *why a correctly-built
`PADStatus`, handed to `PADSetVirtualStatus` every frame, never once
survived to be merged inside `PADRead()`*. The answer: `touch_controls.cpp`
(the PC touch-screen control overlay) was written years before VR input
existed, on the assumption that it was the *only* consumer of
`PAD_CHAN0`'s virtual-pad slot — so several of its internal per-frame sync
functions unconditionally call `PADClearVirtualStatus(PAD_CHAN0)` whenever
touch controls are disabled (the default), with zero awareness that a
second system might also be using that slot. Found and fixed in three
rounds, confirmed via direct instrumentation of `PADSetVirtualStatus`/
`PADClearVirtualStatus` themselves (logging every call to port 0 from
anywhere in the codebase) once guessing at individual call sites twice in
a row hadn't fully resolved it:
1. `TouchControls::sync_virtual_input()` → `sync_touch_state()`, called
   every frame from `mDoCPd_c::read()` (right before `JUTGamePad::read()`
   actually consumes the merged status) — fixed by skipping the call
   entirely when `g_duskVRSessionActive` (`m_Do_controller_pad.cpp`).
2. `TouchControls::update()` → `sync_touch_state()` (the SAME function,
   but reached via a second, completely independent call path: the
   general per-frame UI document loop, `dusk::ui::update()`, unrelated to
   `mDoCPd_c::read()`). Fixing round 1 alone did nothing because this path
   was untouched. Rather than patch `sync_touch_state()` a second time
   from a second angle, this round's fix went one level up: an early
   `if (g_duskVRSessionActive) return;` at the very top of
   `TouchControls::update()` itself, in `touch_controls.cpp`.
3. Still not fixed after round 2 — direct evidence (logging inside
   `PADSetVirtualStatus`/`PADClearVirtualStatus` themselves, not just
   their callers) showed a `PADClearVirtualStatus(0)` call interleaved
   before every single one of VR's `PADSetVirtualStatus(0)` calls, proving
   a *third*, still-unguarded path existed. Found: `sync_visibility()` —
   called FIRST inside `TouchControls::update()`, i.e. *before* the
   `sync_touch_state()` call rounds 1-2 were focused on — has its own,
   completely separate unconditional `clear_virtual_input()` call in its
   `else` branch (reached whenever touch controls are disabled and the
   panel is already hidden, the default steady state). This is what
   round 2's `TouchControls::update()`-level gate (added for a different
   reason, to cover the second `sync_touch_state()` path) ended up ALSO
   fixing, once actually verified — round 2 and round 3's fix are the same
   line of code, just two different reasons it turned out to be
   necessary and sufficient together.

**User-confirmed working in-headset**: "Yup the buttons work."

**Reusable lesson**: when a shared, order-dependent mutable resource
(here, one virtual-pad "slot" meant to represent one physical controller
port) gets a NEW second writer added to it, don't assume the original
single-writer code's own internal per-frame reset/clear logic is confined
to one call site — grep isn't enough when a function has multiple
callers reached via genuinely independent code paths (a direct function
call vs. a general per-frame update loop, in this case). Instrumenting the
actual shared resource's mutation points directly (`PADSetVirtualStatus`/
`PADClearVirtualStatus` themselves, logging every call regardless of
caller) — rather than instrumenting or reasoning about individual
suspected call sites one at a time — is what actually found the second
and third paths quickly once relied on, and should be reached for sooner
next time a similar "my writes keep getting silently overwritten" bug
shows up in this project.

**Diagnostic scaffolding removed** (per this project's normal practice,
now that the bug is confirmed fixed): the `[dusk::vr::input]` per-frame/
periodic `OutputDebugStringA` logs added across `vr_xr_bootstrap.hpp`,
`vr_main.cpp`, `m_Do_controller_pad.cpp`, and `extern/aurora/lib/dolphin/
pad/pad.cpp` (including the direct `PADSetVirtualStatus`/
`PADClearVirtualStatus` instrumentation described above) have all been
removed. The three real fixes (the `g_duskVRSessionActive` gates in
`m_Do_controller_pad.cpp` and `touch_controls.cpp`) are permanent and
left in place.

**UPDATE 2026-08-04 — right thumbstick click added as a second pause
trigger, user-confirmed working in-headset.** Per explicit user request
("make the right stick click the pause menu"): added a new
`stickClickAction` (`vr_xr_bootstrap.hpp`'s `HandActions`, bound to
`/user/hand/right/input/thumbstick/click` on the `oculus/touch_controller`
profile only, same scoping as the rest of this section's button bindings),
read each frame in `vr_main.cpp`'s `tick()` and OR'd into `PAD_BUTTON_START`
alongside the pre-existing left menu button — both now trigger pause; the
left menu button binding was not removed, this is an additional way in,
not a replacement.

**UPDATE 2026-08-04 — left thumbstick click bound to D-pad right (SUPERSEDED
same day, see the full-remap update below — left stick click no longer
does this).** Per explicit user request ("bind dpad right to left stick
click"): reused the same `stickClickAction` above (one action, both hands'
thumbstick-click physical inputs bound to it —
`/user/hand/left/input/thumbstick/click` added alongside the existing
right-hand binding), read separately per hand via subaction path in
`vr_main.cpp`'s `tick()`, left hand's OR'd into `PAD_BUTTON_RIGHT`
(D-pad right). Built successfully at the time, but never confirmed
in-headset before being reassigned — see below.

**UPDATE 2026-08-04 — full control remap, user-confirmed working
in-headset.** Per explicit user request ("bind X to right squeeze, Y to
right trigger, DPAD up to Y, DPAD left to X, and Z to left stick click"):
reassigned five of this section's existing bindings. **This is now the
authoritative mapping** — the original "Mapping" list earlier in this
section (the one starting "left thumbstick -> main stick") is stale;
current state is the table below.

| Controller input | Game action |
|---|---|
| Left thumbstick | Move (main stick) — unchanged |
| Right thumbstick | Camera (C-stick) — unchanged |
| Left trigger | Analog L — unchanged |
| Right trigger | **Y** (was analog R/raise shield) |
| Right squeeze/grip | **X** (was Z) |
| Left squeeze/grip | unbound — unchanged |
| Right A button | A (context action) — unchanged |
| Right B button | B (attack) — unchanged |
| Left X button | **D-pad left** (was X) |
| Left Y button | **D-pad up** (was Y) |
| Left menu button | Start (pause) — unchanged |
| Right stick click | Start (pause) — unchanged |
| Left stick click | **Z** (was D-pad right, from the update directly above — that assignment lasted less than a day) |
| — | **D-pad right — unbound** (nothing currently maps to it, now that left stick click moved to Z) |
| — | **Analog R / raise shield — unbound** (nothing currently maps to it, now that right trigger moved to Y) |

Implementation: `vr_main.cpp`'s `tick()` — `leftXHeld`/`leftYHeld` now OR
into `PAD_BUTTON_LEFT`/`PAD_BUTTON_UP` instead of `PAD_BUTTON_X`/
`PAD_BUTTON_Y`; `rightSqueeze > kSqueezeThreshold` now ORs `PAD_BUTTON_X`
instead of `PAD_TRIGGER_Z`; `rightTrigger > kTriggerDeadzone` now ORs
`PAD_BUTTON_Y` only (digital — no longer also writes `PAD_TRIGGER_R` or
`padStatus.triggerRight`, since Y has no analog counterpart in
`PADStatus`); `leftStickClickHeld` now ORs `PAD_TRIGGER_Z` instead of
`PAD_BUTTON_RIGHT`. No `vr_xr_bootstrap.hpp` changes needed for this
revision — same underlying OpenXR actions as before, only which
`PADStatus` bit each one feeds into changed. Built successfully
(RelWithDebInfo, clean) and **user-confirmed working in-headset**.

**Desired follow-up, not yet started**: user wants the R button (now
unbound as of the remap above — previously analog R/raise shield)
replaced with an actual physical movement (e.g. some real controller
gesture) instead of a button press. Not designed or implemented yet — no
gesture chosen, no code written. If picked up, note `g_rightSwing`/
`vr_swing_detector.hpp` already exists as deferred, untested
swing-gesture infrastructure from this same section's
"swing-gesture-to-attack" work (drafted 2026-08-03, explicitly pulled back
out per user request) — worth checking whether that detector (or the same
general approach) is reusable for a shield-raise gesture too, rather than
building physical-motion detection from scratch.

**UPDATE 2026-08-05 — left-hand swing-to-attack wired up (the deferred
right-hand version above stays deferred/unused), built, NOT yet tested
in-headset.** Per explicit user request ("swinging your left hand in front
of you acts as pressing the b button"): added `g_leftSwing`
(`vr_combat::SwingDetector`, same engine-agnostic infra as the never-wired
`g_rightSwing` above — no changes needed to `vr_swing_detector.hpp`
itself), fed each frame from the already-located `leftPose` grip pose and
the frame's `XrTime` converted to seconds (`time * 1e-9`, monotonic —
epoch doesn't matter, only deltas do). `leftSwingEvent.triggered` (a
one-frame edge; the detector's own cooldown + reset-speed hysteresis
already prevents one swing firing twice) is OR'd into `PAD_BUTTON_B`
alongside the real right-B-button read, in `vr_main.cpp`'s `tick()`.

**Left hand specifically, not a revival of the right-hand draft**: this
is a deliberate choice, not an arbitrary pick between two symmetric
options — section 16 (sword/shield tracking) established the sword is
Link's **left**-hand item (`mLeftItemJntNo`), so swinging the hand
actually holding the sword is the physically-intuitive gesture; a
right-hand swing (what was drafted and deferred back in section 13's
original work, before section 16 had even established which hand holds
the sword) would attack with the empty/shield hand instead. `g_rightSwing`
is left in the tree untouched, same as before — still real, reusable
infrastructure if a future request wants the right hand tied to
something (the shield-raise-gesture idea floated in the paragraph above
remains open).

Built successfully (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling, clean link, no new warnings.

**ROUND 1 tuning (same day) — user tested, "technically worked but was
very unresponsive."** Lowered `g_leftSwing`'s tunables well below
`vr_swing_detector.hpp`'s defaults: `triggerSpeed` 2.5→1.4 m/s,
`resetSpeed` 0.8→0.4 m/s, `minSwingDistance` 0.15→0.08m (set on the
`g_leftSwing` instance only, in `vr_main.cpp` — not in the shared header,
so `g_rightSwing`/future users aren't affected). Built, untested against
real data — a guess based on the defaults looking demanding on paper, not
measured.

**Diagnostic logging added before a third guess** — user's next report
("swings when I move my hand normally, doesn't trigger on a real swing")
was too counter-intuitive to tune blindly against a third time (this
project's rotation-calibration history in section 12 is the standing
lesson for why). Added temporary `[dusk::vr::swingdiag]` logging
(`vr_main.cpp`, right at the `g_leftSwing.update()` call site): raw left
grip position, **two dt sources computed side by side** — the
`predictedDisplayTime`-diff the detector actually uses vs.
`pacing.presentation_dt_seconds` (the real measured frame time, already
used elsewhere for smooth-turn) — and the speed each implies, throttled to
~9Hz (every 10 frames) plus every actual trigger logged unconditionally.
Built, asked user to capture ~10s of neutral/casual hand movement followed
by several real swings and paste back the Output-window lines.

**ROUND 2 — real capture analyzed, root cause found, NOT a timing bug.**
Two findings from the actual 207-line capture:
1. **The false-positive was a plain threshold problem.** One frame during
   the deliberately-neutral "hold still and turn around" phase hit
   1.44 m/s — just over round 1's 1.4 m/s trigger — and fired. `predDt`
   and `pacingDt` tracked each other almost exactly for the entire neutral
   phase (the two only diverged sharply — up to ~9x — during two rare
   apparent frame hitches later in the capture, out of 207 samples), so
   the dt-source-jitter theory this logging was added to check is ruled
   out here: round 1's thresholds were just genuinely too low for ordinary
   arm movement while turning.
2. **The detector was NOT under-firing during real swings** — 13 separate
   triggers were logged during the swing phase (instantaneous speeds
   ranged ~1.5 up to a spiky ~17 m/s), well more than the ~5 sword swings
   the user actually saw play out. This gap is almost certainly downstream
   of the detector, not a detection failure: Link's own attack-animation
   state machine very likely absorbs rapid repeat B-presses the same way
   it would absorb mashing the real button (can't start a new swing
   mid-animation/recovery) — possibly compounded by `resetSpeed` being low
   enough that one continuous physical swing's velocity dips and re-arms
   mid-motion, double-counting as two logical swings. Not fully
   disambiguated between those two contributing causes, but neither points
   back at "raise sensitivity further," so round 3 only raises thresholds.

**ROUND 3 tuning applied**: `triggerSpeed` 1.4→**2.2 m/s** (clears the
observed 1.44 m/s false-positive peak with real margin, still comfortably
under most real-swing peaks from the capture), `resetSpeed` 0.4→**0.7
m/s**, `minSwingDistance` 0.08→**0.12m**, `cooldownSec` 0.12→**0.15s**
(the latter two both nudged up mainly to reduce the "one physical swing
double-counts" risk from finding 2 above). Built successfully.
`[dusk::vr::swingdiag]` diagnostic logging is deliberately still in the
tree (not yet confirmed fixed — this project's normal practice) for one
more capture if needed.

**Unrelated incident hit during round 3's build**: `_deps/xxhash-src/
xxhash.h` (a CMake FetchContent-vendored third-party header, unrelated to
anything touched this session) had a line corrupted with an injected `It`
token (`It            xacc[i] = ...`), breaking the build with an unrelated
C2065/C2146 error. Same class of corruption CLAUDE.md already documents
for `extern/aurora/lib/gfx/common.cpp`'s `wait_for_gpu_progress()` (a
stray focused editor/dictation tool typing into whatever file has focus,
not a real regression from any code change) — just the first time it's
hit a DIFFERENT file. Fixed the same way: restored the line, rebuilt, did
not investigate further. Worth broadening the CLAUDE.md guidance on this
if it recurs in a third file — it may not be specific to `common.cpp`.

**NOT yet tested in-headset** with round 3's values — next step for
whoever picks this up: repeat the neutral-movement + real-swing capture
one more time (or just play normally) and confirm both (a) no spurious
attack during ordinary movement/turning, and (b) real swings register
without needing near-maximum effort. If the perceived-swing-count-vs-
trigger-count gap from finding 2 persists, that's the point to
investigate Link's attack-animation gating directly rather than tuning
the detector further.

### 14. Stereo eyes misalign at large head yaw ("left/right eyes look swapped" near 90°) — CONFIRMED FIXED IN-HEADSET 2026-08-05 (first fix attempt regressed and was reverted first — read in full before touching this again)

**Symptom** (user-reported, not yet reproduced/investigated in-headset by a
session): turning the head left/right causes the two eyes to progressively
misalign; at roughly 90° yaw, looking at Link's own body, it looks "almost
as if the left and right eyes are swapped." Fine (or close to it) facing
forward: gets worse the further the head turns away from forward.

**Investigation so far (code-reading only, no build/test/instrumentation
yet)**: read `vr_stereo_render.hpp`'s `eyePoseToViewMtx()` (builds each
eye's view matrix from its `XrView` pose). Found a plausible root cause,
NOT yet confirmed:
- The eye's **position** offset (`dx,dy,dz` = eye pose minus head-center
  pose) gets Z-flipped before being added to the shared world anchor
  (`wz_ = linkEyeGame.z - dz*scale`) — converting OpenXR's right-handed,
  Z-back tracking convention into the game's left-handed, Z-forward world
  convention.
- The eye's **orientation** (the `r00..r22` rotation matrix) is built from
  the **raw, unflipped** quaternion — deliberately left that way per an
  existing comment in the same function ("flipping qz here...inverted
  pitch and roll"), i.e. a previous session already found flipping it
  breaks something else.
- These two pieces of the same view matrix are therefore expressed in two
  different coordinate handedness conventions. Near forward-facing (where
  X dominates and X is untouched by the flip) this would be invisible;
  as yaw approaches 90° (where the eye-separation direction rotates onto
  the axis that WAS flipped for position but NOT for orientation) the
  mismatch would surface exactly as the reported symptom. This is a
  hypothesis from reading the code, not something verified by testing,
  logging, or an isolated capture.
- **Ruled out** (by reasoning, not testing): the flatscreen-camera/`linkEyeGame`
  anchor (section 11) is computed once per frame and fed identically to
  BOTH eyes, so it cannot by itself cause a differential left-vs-right
  symptom — a wrong anchor would shift both eyes together, not swap them
  relative to each other. User asked specifically whether this could be the
  cause; answered no for this reason.

**Why this is flagged as potentially hard, not a quick fix**: this is the
same *category* of bug as section 12's hand-tracking rotation saga
(quaternion/coordinate-handedness math that looks correct near one
reference orientation and drifts wrong away from it) — which took three
full sessions in this project to actually resolve, including a genuinely
subtle inverse-rotation bug that survived two sessions of "verified
correct" fixes before being found. Given that history, this should NOT be
approached as a guess-and-rebuild loop; if picked up, reuse the lessons
already written up in section 12 (script-verify any derived
rotation/coordinate math before touching the headset, isolated single-axis
motion tests rather than reasoning abstractly, don't assume a fix is
sufficient just because it looks right at one reference pose).

**Status (2026-08-04)**: explicitly deferred per user request ("note this
for the future") rather than investigated further that session — user was
undecided on fixing now vs. later and chose to defer once given this
difficulty estimate. Not blocking (doesn't crash; only degrades
accuracy/comfort at extreme head yaw), so safe to leave deferred. Next
step for whoever picks this up: reproduce and confirm the symptom exists
as described, then verify (rather than assume) whether the
position/orientation handedness mismatch above is the actual cause before
attempting a fix.

**2026-08-05 — fix derived, verified by script, applied and built. NOT yet
confirmed in-headset — this is a candidate fix, not a closed bug.** Acted
on the hypothesis above, but per section 12's lesson didn't go straight to
a guess-and-rebuild: wrote a standalone Python simulation
(`verify_depth_fix.py`, scratch — not checked into the repo) BEFORE
touching any code, to check whether the handedness-mismatch theory
actually predicts the reported symptom.

**What the script found**: simulate a point fixed at the camera's physical
right (roughly where the other eye sits) and ask where the CURRENT view
matrix's rotation block says it should appear in view space, across a
range of head yaw angles (0°/30°/60°/90°/120°). Result:
```
yaw=  0   current=[ 1,0, 0]      <- right, correct
yaw= 30   current=[ 0.5,0,0.87]  <- drifting
yaw= 60   current=[-0.5,0,0.87]  <- already flipped past center
yaw= 90   current=[-1,0, 0]      <- reads as fully LEFT
yaw=120   current=[-0.5,0,-0.87]
```
At 90° a point physically to the camera's right reads as fully to view-space
LEFT with the current matrix — i.e. the current code reproduces "eyes
swapped at 90°" exactly, on paper, before any in-headset test. This is
strong (not certain) evidence the handedness-mismatch theory from
2026-08-04 was right.

**The fix, and why it's not the same as the previously-rejected "flip qz"
attempt**: the position offset is already converted from OpenXR's tracking
convention to the game's world convention via a Z flip
(`F = diag(1,1,-1)`) before this function combines it into the view
matrix — see `wz_ = linkEyeGame.z - dz*scale` — but the orientation
(`r00..r22`, built straight from the raw quaternion) never got the same
conversion. The mathematically correct way to carry a rotation matrix
through a coordinate reflection `F` is the similarity transform `F·R·F`
(NOT flipping a quaternion component before the quat→matrix formula —
verified algebraically these are only the same when certain cross terms
happen to be zero, e.g. conveniently near forward-facing, which is
probably why the old "flip qz" attempt looked locally sane before being
rejected on pitch/roll grounds). Since `F` is diagonal, `F·R·F` reduces to
negating exactly the four matrix entries that mix the Z axis with X/Y
(`r02`, `r12`, `r20`, `r21`); the other five entries (including `r22`
itself) are untouched — which is also why this fix is a no-op at
forward-facing (those four terms are ~0 there), matching "fine facing
forward, worse toward 90°" from the original report. Confirmed via the
same script that this stays a proper rotation (det=+1, not a mirror)
across 2000 random test orientations, and that with the fix applied, the
same "point at camera's physical right" simulation reads as
view-space-right at EVERY yaw angle tested (0° through 120°), not just at
0°.

**Applied in `vr_stereo_render.hpp`'s `eyePoseToViewMtx()`**: after
computing `r00..r22` as before (unchanged), four corrected values
`r02c/r12c/r20c/r21c` (the same values, negated) are used in place of
`r02/r12/r20/r21` both in the matrix write AND in the translation
dot-products right below it (the translation must use whatever actually
ends up in the matrix, not the pre-correction values — this tripped up
nothing this time, but is exactly the kind of easy-to-miss consistency
requirement this bug class tends to punish). Full reasoning is inline in
the code comment there.

**Built successfully** (RelWithDebInfo, `windows-msvc-relwithdebinfo`
preset) — only `vr_main.cpp` needed recompiling (it includes this header),
clean link, no new warnings.

**Built successfully, tested in-headset by the user immediately after —
REGRESSION FOUND AND REVERTED same session, root cause understood, a
second (different, better-supported) fix applied and built. NOT yet
retested in-headset.**

**The regression**: user report was immediate and unambiguous — "the
headsets movement is reversed, so turning it left turns right and looking
up looks down." This is section 12's own warning playing out exactly as
written ("don't assume a fix is sufficient just because it looks right at
one reference pose/derivation") — the script only checked the *stereo
offset direction*, never checked whether the orientation fix preserved
ordinary look-around SENSE, which turned out to be the thing it broke.

Re-derived by hand once the report came in: for a pure-pitch quaternion
(rotation about local X only), the applied `F*R*F` correction turns the
resulting 2D rotation block from `[[cosθ,-sinθ],[sinθ,cosθ]]` into
`[[cosθ,sinθ],[-sinθ,cosθ]]` — i.e. a rotation by `-θ` instead of `θ`. Any
rotation touching the Z axis (pitch, yaw) gets its direction reversed by
this correction. Confirmed numerically too (see the script from the
original attempt, extended to print a concrete before/after test vector).
This is a strictly worse regression than the narrow 90-degree stereo
symptom it was meant to fix — reverted the orientation change immediately.

**Reconsidering with the new evidence in hand**: since the user's report
proves the ORIGINAL (unflipped) orientation must be correct — it was fine
before touching it, and only my change broke look-around sense — the bug
has to be somewhere else. Went back to `eyePoseToViewMtx`'s own comment
("flip Z...matches buildHandMtx's convention") and checked what
`buildHandMtx` actually does now (`vr_link_visibility.hpp:463`): its
position formula uses `linkEyeGame.z + dz*scale` — no flip — because
section 12 found and fixed an identical Z-flip there for a "front/back
mirrored" hand-tracking bug, confirmed working since. `eyePoseToViewMtx`
was never updated to match; it kept the stale, already-proven-wrong `- dz
* scale`. Both bugs plausibly share one root cause: a Z-flip that was
correct for nothing more than "seemed to match the sibling function" at
the time it was written, before that sibling's own version of the same
flip was independently found wrong and removed.

**Re-verified in script before reapplying**: with the position offset's Z
flip removed (matching `buildHandMtx`) and the orientation matrix left
completely alone (proven necessary by the regression above), the
self-consistency check from the original derivation
(`Rᵀ(q) · R(q) · local_offset == local_offset`) holds **exactly** at every
yaw angle tested, not approximately — this is a mathematical identity
(a rotation matrix transposed times itself is identity), not something
tuned to fit. This is also a materially different, better-supported claim
than the reverted fix: it's not "this looks right in isolation," it's "the
sibling function had the identical bug, already fixed and confirmed
working, and this makes the two consistent instead of diverging by
construction."

**Applied**: `eyePoseToViewMtx()`'s `wz_` now uses `+ dz * scale` (removed
the flip) instead of `- dz * scale`; orientation math is back to exactly
what it was before this whole investigation (unflipped, unmodified). Full
reasoning inline in the code comment.

**Built successfully** a second time (RelWithDebInfo, clean, only
`vr_main.cpp` recompiled).

**CONFIRMED FIXED IN-HEADSET, same session**: user tested immediately —
"The eye swap is gone and the headset movement is correct." Both the
original 90°-yaw stereo-swap symptom and ordinary look-around sense
(regression-free) confirmed in one pass. Forward/backward head-motion feel
(leaning, walking) was not separately called out by the user as broken, so
treated as fine, though not as explicitly interrogated as the other two —
if a subtle forward/back feel issue ever surfaces later, start here, since
this is the one part of the fix whose correctness for the CAMERA
specifically (as opposed to hands, where the identical flip-removal was
directly validated) was inferred by analogy rather than independently
confirmed.

**Why the second attempt landed in one try where the first didn't**: the
first fix was a derivation that satisfied one property (stereo-offset
consistency) without proof it was the *only* correct transform, and broke
a different one (rotation sense) that was never checked. The second fix
was closer to "apply a cure this codebase already found for the identical
bug next door" (section 12's `buildHandMtx` fix) than a fresh derivation —
and because the orientation side was independently proven correct by the
regression report, the remaining fix (remove the position-side flip)
reduces to the exact identity `Rᵀ(q)·R(q) = I`, not an approximate
patch. Closes out section 14.

### 15. VR smooth-turn (right thumbstick) — CONFIRMED WORKING IN-HEADSET 2026-08-05

**Goal** (explicit user request): "add smooth camera rotation to the right
stick and also unbind the C stick." Right thumbstick used to feed
`padStatus.substickX/Y` directly (the game's normal C-stick, which
smoothly orbits the flatscreen third-person camera — see section 13's
mapping table) — replaced with a purpose-built VR comfort-turn: pushing
the stick left/right smoothly rotates a persistent yaw offset that the
camera, both tracked hands, and the HUD billboard all rotate by, plus
gameplay's own movement-direction reference so walking stays consistent
with the new view direction.

**New shared header**: `src/dusk/vr/vr_smooth_turn.hpp` — a persistent
`g_smoothTurnYawRad` (radians, OpenXR/tracking-space convention, updated
once per frame by `updateSmoothTurn(rightStickX, dtSeconds)`), plus two
pure rotation helpers, `rotateYawXr()` (position) and `rotateYawQuat()`
(orientation quaternion, Hamilton composition `RotateY(yaw) * q`).
Deliberately a SINGLE shared header rather than duplicated per call site —
see section 14's own lesson from earlier the same day: `eyePoseToViewMtx`
and `buildHandMtx` used to each carry their own copy of a
"matches-the-other-one's-convention" position formula that silently
drifted out of sync (one got fixed, the other didn't) and caused a real
bug. All three call sites that need yaw rotation now include this one
header and call the same two functions, so there's exactly one
implementation to keep correct.

**Verified in a standalone script before writing any game code** (same
discipline as section 14, not a repeat of that session's first-attempt
mistake): confirmed `rotateYawQuat` and `rotateYawXr` compose consistently
— rotating a local vector by the yaw-rotated quaternion's matrix gives an
identical result (to float precision, ~1e-15 over 2000 random trials) as
rotating the ORIGINAL-orientation-transformed vector by `rotateYawXr`
directly. Also confirmed `rotateYawQuat` always returns a unit quaternion
and that yaw=0 is an exact no-op for both functions (safe default for any
call site not using smooth-turn). This same script is also what determined
the SIGN convention (positive yaw turns the view LEFT with this
formula's convention) — `updateSmoothTurn()` negates the stick input to
compensate, so pushing the stick right turns the view right, derived
rather than left as a "flip if backwards in-headset" guess like some of
this project's earlier direction constants.

**Wired into four places**:
- `eyePoseToViewMtx()` (`vr_stereo_render.hpp`): new `yawRad` parameter
  (defaulted `0.f`), applied to the tracked position offset and
  orientation quaternion right at the top, before any of the existing
  game-convention math (including section 14's same-day handedness fix)
  runs — kept deliberately orthogonal, operating purely in OpenXR's native
  coordinate system so it can't interact with that fix.
- `buildHandMtx()` (`vr_link_visibility.hpp`): same treatment, new `yawRad`
  parameter (defaulted `0.f`), so tracked hands stay visually consistent
  with the smooth-turned view instead of appearing to lag behind it.
- `updateHudSmoothing()` (`vr_stereo_render.hpp`): also takes `yawRad` now,
  so the head-locked HUD billboard's world-forward reference rotates with
  the rest of the scene. `computeHudPose()` itself needed no change — it
  already re-projects through the CURRENT eye's `view->viewMtx`, which by
  construction already includes the yaw once `beginEye()` applies it.
- `daAlink_c`'s movement-angle computation (`d_a_alink.cpp`, human-form
  normal-gameplay branch only): `mMoveAngle = mStickAngle +
  dCam_getControledAngleY(...)` now also adds
  `cM_rad2s(dusk::vr::getSmoothTurnYawRad())` when
  `isRenderingToHeadset()`. Necessary because the right stick no longer
  drives the flatscreen camera object at all in VR, so its yaw would never
  reach `dCam_getControledAngleY()` the normal way — without this, smooth-
  turning would rotate what you SEE but not which way "forward" walks you,
  which is disorienting and defeats the point of the feature (every VR
  game with stick-based smooth-turn couples look and movement direction
  this way; not treated as an open design question).

**Threading the yaw value through**: `vr_main.cpp`'s `tick()` calls
`dusk::vr::updateSmoothTurn(rightStick.x, pacing.presentation_dt_seconds)`
once per frame (right where the old substickX/Y assignment used to be),
then reads it back via a new thin-forward accessor,
`dusk::vr::getSmoothTurnYawRad()` (declared in `vr_main.hpp`, defined in
`vr_main.cpp` — same "keep heavy OpenXR headers out of core game files"
pattern as `isRenderingToHeadset()`/`applyTrackedHandMtx()`, which is why
`d_a_alink.cpp` can use it without including `vr_smooth_turn.hpp`
directly), and passes it into `EyeParams` (new `smoothTurnYawRad` field,
one value shared by both eyes), `vr_link::FrameInput` (same, new field,
consumed by both `buildHandMtx()` calls), and the `updateHudSmoothing()`
call site.

**Unbinding the C-stick**: `vr_main.cpp`'s right-stick handling no longer
writes `padStatus.substickX/Y` at all. Cleaned up `wantsVirtualPad`'s
OR-chain to drop the now-always-zero substick fields it used to check.

**Tuning constants** (`vr_smooth_turn.hpp`): `kSmoothTurnDegPerSec = 90`
(turn rate at full stick deflection), `kSmoothTurnStickDeadzone = 0.15`
(matches this project's other thumbstick deadzones). Untested picks, not
derived from anything — the first thing to retune if turning feels too
fast/slow or twitchy near center.

**Built successfully** (RelWithDebInfo, full incremental rebuild since a
widely-included header changed — no errors or new warnings in any touched
file). **NOT yet tested in-headset.**

**Known gaps, not yet addressed**:
- Wolf Link / horse / other non-"normal human gameplay" movement branches
  don't get the `mMoveAngle` addition (only the one call site in the
  human-form branch was touched, matching how section 11's first-person
  camera anchor also only covers that same case). Untested whether smooth
  visual turning still works for those forms (it should, since the camera/
  hand rotation wiring isn't form-gated) even though movement-direction
  won't follow it there.
- The turn-rate/deadzone constants are unvalidated guesses.
- No accessibility alternative yet (e.g. snap-turn instead of smooth) —
  not requested, not built.

**CONFIRMED WORKING IN-HEADSET, same session**: user tested and reported
"It works" — a terse confirmation, not itemized against the four specific
checks above (turn direction, hands/HUD staying locked, movement following
the turn, turn-rate feel). Treated as a genuine pass rather than
under-verified, since a wrong turn direction or unsynced hands/HUD would
be immediately, obviously broken (not the kind of subtle-drift bug this
project's rotation work has sometimes needed precise reproduction steps
to catch — contrast section 14's regression, which needed a specific
"turning left turns right" description to diagnose). If a subtler issue
turns up later (e.g. movement direction feeling slightly off, or an issue
specific to Wolf/horse form per the known gaps above), come back here
first rather than assuming a new bug.

### 16. Sword/shield floating instead of tracking VR hands — built 2026-08-05, NOT yet confirmed in-headset

**Symptom** (user-reported): pulling out the sword and shield in VR shows
them floating in front of the player at roughly where they'd sit on Link's
regular (flatscreen third-person) model, instead of in the player's
tracked hands.

**Root cause**: `mSwordModel`/`mShieldModel` (`d_a_alink.cpp`) are separate
`J3DModel` instances, positioned once per frame (`setItemMatrix()`, NOT
per-eye) via
```
mSwordModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mLeftItemJntNo));
mShieldModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mRightItemJntNo));
```
`mLeftItemJntNo`/`mRightItemJntNo` are the SAME body-model joint indices
(9, 0xE) that `setDrawHand()` feeds into `mpLinkHandModel`'s joints 1/2
(al_handsL/al_handsR, see section 12) — i.e. on flatscreen the sword
already attaches directly to Link's animated LEFT hand joint, the shield
to his RIGHT. Section 12's tracked-hand fix only re-pointed
`mpLinkHandModel`'s joints at the real controller pose; the BODY model's
own hand joints (what sword/shield actually read) were never touched, so
they kept reflecting the flatscreen third-person animation the whole
time — exactly the reported symptom. Left/right mapping confirmed by
cross-referencing `mLeftItemJntNo`/`mRightItemJntNo`'s wolf-form values
against `setDrawHand()`'s joint-9/0xE calls, matching section 12's
already-confirmed `LEFT_HAND_JOINT`=al_handsL/`RIGHT_HAND_JOINT`=al_handsR
mapping — sword is Link's left hand, shield his right (standard Zelda
left-handed convention).

**Fix**: reuse the exact tracked matrices already computed for
`mpLinkHandModel` (`vr_link_visibility.hpp`'s `detail::s_leftHandMtx`/
`s_rightHandMtx`, section 12) — valid here too, since `getAnmMtx(9)`/
`getAnmMtx(0xE)` were already being used as plain world-space "where the
hand is" matrices for this exact purpose. New
`vr_link::applyTrackedItemMtx(swordModel, shieldModel)`
(`vr_link_visibility.hpp`) sets `swordModel`'s base transform to
`s_leftHandMtx` and `shieldModel`'s to `s_rightHandMtx`, then calls
`->calc()` on each.

**Why `calc()` is required here but wasn't for the hand-joint fix**:
`applyTrackedHandMtx()` (section 12) calls `handModel->setAnmMtx(jointNo,
m)`, which pokes directly into `mMtxBuffer` — an already-RESOLVED
joint-world-matrix buffer read directly at draw time, no recalculation
needed. `setBaseTRMtx()` is different: it only assigns
`J3DModel::mBaseTransformMtx`, a plain member that `calcAnmMtx()` (called
from `calc()`) reads to resolve the model's OWN joint tree
(`J3DModel.cpp`: `calcAnmMtx()` → `getJointTree().calc(mMtxBuffer,
mBaseScale, mBaseTransformMtx)`) — confirmed by reading `J3DModel.cpp`
directly before writing this fix, not assumed. Since sword/shield were
already `calc()`'d once this frame (`setItemMatrix()`, with the stale
flatscreen-joint base matrix), changing `mBaseTransformMtx` alone would
have zero visible effect until some later frame's `calc()` happened to
run — `calc()` has to be called again, per eye, for the new base matrix
to actually reach the draw. Sword/shield are small, simple models, so a
second/third `calc()` per frame is not a perf concern (same reasoning as
section 7's HUD billboard capture cost).

**Call site**: `d_a_alink.cpp`, right after `setDrawHand()` in the
non-wolf gameplay draw branch — same per-eye, "last write before draw"
window as section 12's hand fix, guarded on `isRenderingToHeadset()`.
Thin-forwarded through `dusk::vr::applyTrackedItemMtx()`
(`vr_main.hpp`/`.cpp`), same "keep heavier OpenXR/aurora headers out of
core game files" pattern as `applyTrackedHandMtx()`/`drawHudBillboard()`.
Not touched in the Wolf-form draw branch — matches `setDrawHand()`'s own
scope (Wolf Link doesn't use tracked hands either, per section 11's
third-person wolf fallback).

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp`,
`d_a_alink.cpp`, and their dependents needed recompiling, clean link, no
new warnings. **NOT yet tested in-headset** — next step for whoever picks
this up: launch in VR, draw the sword/shield, and confirm they now sit in
and move with the tracked hands rather than floating at a fixed
third-person-relative position. Worth checking specifically whether the
attachment POINT looks right (e.g. sword handle in the fist vs. offset
from it) — this fix reuses the hand's own tracked matrix as-is with no
additional grip offset, on the theory that `getAnmMtx(9)`/`getAnmMtx(0xE)`
already served as the flatscreen attachment point at this same joint, so
no new offset should be needed, but this hasn't been visually confirmed.

**Known gap, not fixed this session**: `mHeldItemModel` — a separate,
broader "currently equipped item" model (bow, lantern, boomerang, etc.),
also positioned from `mLeftItemJntNo`/`mRightItemJntNo` in several places
in `d_a_alink.cpp` — very likely has the exact same floating-in-VR
symptom, by the same mechanism. Not addressed here; only sword/shield were
reported. If picked up, `applyTrackedItemMtx()`'s pattern should apply
directly.

**ROUND 2 (same day) — first version was WRONG, fixed, built, still NOT
yet confirmed in-headset.** User tested the round-1 fix: shield looked
correctly positioned, but the sword did not, AND the shield showed up even
when not equipped, and the sword's sheathed hilt showed up even when the
sword wasn't the active/equipped item (pulling the sword out then showed
its blade, i.e. the model itself was fine, just visible/positioned in the
wrong place beforehand).

**Root cause of round 1's bug**: `setItemMatrix()` does NOT always attach
sword/shield to the hand joint — it switches per frame between the
hand-joint matrix (sword: only when `mEquipItem == 0x103`, i.e. sword is
Link's currently-EQUIPPED weapon — `param_0` is always `0` at all 3 real
call sites, so that reduces to just this one condition in practice;
shield: a wider OR-chain covering actively guarding/attacking/holding it
up/etc.) and a completely different, computed BELT/BACK-relative offset
matrix (the `else` branch — e.g. sword sheathed while some OTHER item like
the bow is equipped, or shield stowed on the back) for everything else.
Round 1's `applyTrackedItemMtx()` overwrote BOTH cases unconditionally with
the tracked hand position — so a sheathed sword (while a different item
was equipped) or a stowed shield ended up floating at the tracked hand
instead of staying out of the way at the hip/back, exactly matching what
the user reported.

**Fix**: `applyTrackedItemMtx()` now takes two extra parameters —
`leftHandJointMtx`/`rightHandJointMtx` — which the caller
(`d_a_alink.cpp`) computes as `mpLinkModel->getAnmMtx(mLeftItemJntNo)`/
`getAnmMtx(mRightItemJntNo)`, i.e. exactly what `setItemMatrix()` itself
would have used THIS frame in the hand-attached case. A new
`mtxNearlyEqual()` helper (`vr_link_visibility.hpp`, epsilon 0.01 per
entry) compares each model's CURRENT base transform (as `setItemMatrix()`
already set it, earlier this same frame) against that hand-joint matrix —
only when they match (i.e. `setItemMatrix()` actually chose the
hand-attached branch this frame) does the tracked-hand substitution now
happen; otherwise the model is left completely alone, keeping the base
game's own belt/back-relative resting pose. Deliberately detects this
structurally (matrix comparison) rather than duplicating
`setItemMatrix()`'s own multi-condition boolean logic in the VR code — one
less place that silently drifts out of sync if that logic ever changes,
same reasoning this project has applied elsewhere (e.g. section 1's water
fovy/aspect fix).

Updated signatures: `vr_link::applyTrackedItemMtx(swordModel, shieldModel,
leftHandJointMtx, rightHandJointMtx)`; the `dusk::vr::` forward
(`vr_main.hpp`/`.cpp`) takes the last two as plain `float (*)[4]` rather
than the `MtxP` typedef, since `vr_main.hpp` deliberately doesn't include
the (dolphin-mtx.h-dependent) `J3DModel.h` just for that one type — both
are the same underlying type (`MtxP` is `f32 (*)[4]`, `f32` is `float`).

**Built successfully** a second time (RelWithDebInfo, clean, only
`vr_main.cpp`/`d_a_alink.cpp`/dependents recompiled).

**ROUND 3 (same day) — round 2's sheath/unsheath gating confirmed working
in-headset; sword still faced the wrong way. Real root cause found: item
joint ≠ hand joint. Fixed, built, NOT yet confirmed in-headset.** User
tested round 2: sword now correctly sheaths/unsheathes (the visibility gate
worked), but still faces the wrong way once drawn — shield remained
correct.

**Root cause**: round 1/2 both assumed `mLeftItemJntNo`/`mRightItemJntNo`
(what positions the sword/shield) are the SAME joints as
`mLeftHandJntNo`/`mRightHandJntNo` (what `setDrawHand()` feeds into
`mpLinkHandModel`'s tracked joints 1/2) — reasoning that both were "9" and
"0xE" since `setDrawHand()` hardcodes those literals. This was WRONG.
Actually checked (not assumed) by reading `d_a_alink_wolf.inc`'s
"revert-from-wolf-to-human" reset block, which sets all four fields in one
place: `mLeftHandJntNo=9`, `mRightHandJntNo=0xE`, `mLeftItemJntNo=10`,
`mRightItemJntNo=0xF` — the item joint is a genuinely DIFFERENT joint (one
index higher), presumably a child/sibling joint in the rig encoding the
fixed grip offset between "where the wrist is" and "where a held object
sits/is oriented in that grip." Substituting the tracked HAND matrix
directly for the ITEM joint (rounds 1-2) ignored this offset entirely —
looked passable for the shield (a flat/symmetric shape, forgiving of a
moderate rotational error) but was clearly wrong on the sword (a long
blade making the same error obvious at the tip). This is exactly the
"sword-specific offset, invisible on a hand/shield but visible on a blade"
theory flagged as the leading suspect at the end of round 2 — except the
actual mechanism was a genuinely different joint entirely, not an
imprecise hand-rotation calibration.

**Fix**: instead of substituting the tracked hand matrix directly, preserve
whatever relative offset the body rig currently defines between the hand
joint and the item joint, recomputed fresh every frame from `mpLinkModel`'s
own real animated matrices (correct regardless of whether that offset is a
rig constant or itself varies per-animation):
```
relativeOffset = inverse(handJointWorldMtx) * itemJointWorldMtx
trackedItemMtx = trackedHandMtx * relativeOffset
```
i.e. "re-express the item's real current local relationship to the hand
joint, but rooted at the TRACKED hand pose instead of the body's animated
one." `vr_link::applyTrackedItemMtx()` (`vr_link_visibility.hpp`) now takes
both joints' matrices per side (`leftItemJointMtx`/`leftHandJointMtx`,
`rightItemJointMtx`/`rightHandJointMtx`) — the item matrix still doubles as
the round-2 sheath/stow gate — and a new `computeTrackedItemMtx()` helper
does the `MTXInverse`/`MTXConcat` composition (Nintendo SDK's dolphin
`mtx.h`, already transitively available via this header's existing
`J3DModel.h` include). Verified `MTXConcat(a, b, ab)`'s convention first by
checking an existing call site (`d_bg_parts.cpp`:
`MTXConcat(viewMtx, modelMtx, m)`) rather than assuming — confirms
`ab*v = a*(b*v)` (ordinary left-to-right matrix multiplication), which is
what the derivation above relies on. `d_a_alink.cpp`'s call site now passes
all four matrices (`mpLinkModel->getAnmMtx(mLeftItemJntNo/mLeftHandJntNo/
mRightItemJntNo/mRightHandJntNo)`); `vr_main.hpp`/`.cpp`'s forwarding
wrapper signature grew to match.

**Built successfully** a third time (RelWithDebInfo, clean, only
`vr_main.cpp`/`d_a_alink.cpp`/dependents recompiled).

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Posed
correctly." Sword and shield both now track the real controllers with
correct position AND orientation, sheathe/unsheathe correctly, and no
longer show up when not the active equipped item. Closes out this
section — sword/shield VR tracking is done.

**Follow-up scoped but explicitly DEFERRED (user: "write that down, we can
do it later")** — `mHeldItemModel` (bow, bottles, oil bottle, lantern/
kantera, hookshot, iron ball, copy rod, etc. -- the broader "currently
equipped item" model, distinct from sword/shield) has the same underlying
floating-in-VR bug, by the same mechanism, but is meaningfully more work
than sword/shield was. Scoped (not yet started) via a read of
`setItemMatrix()` and the `getLeftItemMatrix()`/`getRightItemMatrix()`
accessors:

1. **Many more branches, one shared model.** Sword/shield were a clean 1:1
   case (2 fixed models, 2 fixed joints, no extra offset). `mHeldItemModel`
   is a single model whose MESH changes per equipped item, with at least
   7-8 distinct positioning branches in `setItemMatrix()` alone (bow,
   bottle, oil bottle, kantera/lantern, hookshot, iron ball, copy rod, a
   generic default case) — plus more positioning logic spread across
   `d_a_alink_bottle.inc`, `d_a_alink_copyrod.inc`, `d_a_alink_grab.inc`,
   `d_a_alink_hook.inc`, `d_a_alink_kandelaar.inc`, not yet fully read.
2. **Some branches layer an extra per-item offset** (`mDoMtx_stack_c::copy(
   mpLinkModel->getAnmMtx(mLeftItemJntNo/mRightItemJntNo)); transM(...);
   XYZrotM(...);`) on top of the item joint, rather than attaching directly
   to it like sword/shield did. Round 3's `computeTrackedItemMtx()`
   technique (preserve whatever the model's CURRENT relationship to the
   hand joint is, reproject onto the tracked hand) still works here
   unchanged in principle -- it doesn't care what the extra offset is --
   but round 2's exact-matrix-equality gate (detecting "is this
   hand-attached this frame") won't hold once an extra transM/rotM is
   layered on, so that gate needs a different design for this model.
3. **Not every branch is hand-anchored at all.** Item `0x106` attaches to
   joint 4 (looks face/head-related, not a hand); hookshot and iron ball
   use their own dedicated position functions (`setHookshotPos()`/
   `setIronBallPos()`), likely physics-driven (chain/thrown-ball motion).
   These must be explicitly excluded from any hand-tracking override, the
   same way sword/shield's belt/back-relative resting pose was left alone.
4. **Hand assignment isn't fixed.** Unlike sword=left/shield=right always,
   the bow can be gripped by either hand (`checkBowGrabLeftHand()`) --
   whichever fix approach is used needs to pick the correct tracked-hand
   matrix dynamically, not a hardcoded side.
5. **Ripple effect into OTHER actors, potentially the bigger part of this
   work**: `getLeftItemMatrix()`/`getRightItemMatrix()` (thin wrappers
   around the same `mpLinkModel->getAnmMtx(mLeftItemJntNo/mRightItemJntNo)`
   calls) are read directly by roughly 10 OTHER actor files for effects
   anchored to whatever's in Link's hand: `d_a_arrow.cpp` (nocked arrow),
   `d_a_boomerang.cpp` (throw/trail effects), `d_a_e_bug.cpp`,
   `d_a_e_fm.cpp`, `d_a_e_gob.cpp`, `d_a_e_sm2.cpp` (enemy interactions),
   `d_a_mg_fish.cpp`, `d_a_mg_rod.cpp` (fishing rod minigame),
   `d_a_npc_tk.cpp`, `d_a_obj_lp.cpp`. If only `mHeldItemModel`'s own base
   transform is fixed, these would still read the stale, untracked joint
   matrix directly -- e.g. an arrow would stay nocked at the OLD
   flatscreen hand position while the bow itself visibly moves to the
   tracked hand, a visible mismatch. The clean fix is likely to make
   `getLeftItemMatrix()`/`getRightItemMatrix()` THEMSELVES VR-aware (return
   the tracked-adjusted matrix when `isRenderingToHeadset()`), which would
   fix every downstream consumer for free rather than special-casing each
   one -- but it's more surface area to reason about and test (each
   accessor is a hot per-frame read from several unrelated systems, not
   just `mHeldItemModel`'s own positioning code).

**Rough sizing**: 2-3x the code-touched of the sword/shield fix, plus its
own in-headset verification pass per item type (bow, a bottle, the lantern
at minimum) since there's no single test that covers every branch. The
CORE technique (round 3's relative-offset preservation) is proven and
reusable here -- the extra effort is entirely in the branch/gating
complexity and the accessor-function ripple effect above, not in deriving
a new approach. **Not started** -- pick this up here when resumed.

## Key lesson learned this session

Don't infer that an uncommitted fix supersedes a nearby disable guard just
because they're both present in the diff — ask, or look for explicit
"confirmed/tested" language, before re-enabling. A conversation that resumes
mid-investigation (e.g. after a closed/reopened prompt) cannot tell "fix
landed, disable is stale" apart from "fix was tried and rejected, decision
already made" from the code alone — both look identical in a diff. This bit
us once already on the shadow-stretching guard (see #1).
