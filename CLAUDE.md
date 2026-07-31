# Dusklight VR mod — working notes

Twilight Princess PC port ("dusklight") with an in-progress VR mod. This file
tracks the state of active VR debugging so a session can be resumed cleanly
after a break. It reflects the working tree as of 2026-07-30; check `git
status`/`git diff` against this list before trusting anything below, since
these are hand-maintained notes, not generated from the diff.

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
Other known issues below.

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

## Key lesson learned this session

Don't infer that an uncommitted fix supersedes a nearby disable guard just
because they're both present in the diff — ask, or look for explicit
"confirmed/tested" language, before re-enabling. A conversation that resumes
mid-investigation (e.g. after a closed/reopened prompt) cannot tell "fix
landed, disable is stale" apart from "fix was tried and rejected, decision
already made" from the code alone — both look identical in a diff. This bit
us once already on the shadow-stretching guard (see #1).
