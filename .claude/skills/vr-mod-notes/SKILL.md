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
  calls. (4) `WinPixEventRuntime.dll` isn't vendored in this repo or
  present anywhere on this dev machine by default — it's the official
  Microsoft NuGet package (`https://www.nuget.org/api/v2/package/WinPixEventRuntime`,
  a `.nupkg`, which is just a renamed `.zip` — extract it and copy
  `bin\x64\WinPixEventRuntime.dll` next to `dusklight.exe`, not the
  `_UAP` variant or the ARM64 folder). (5) **A single F9 press can
  produce a near-empty capture (a handful of state-reset calls, "No
  Resource" bound everywhere, no real draw calls) even though the
  headset shows a normal image at that instant** — confirmed 2026-08-09.
  Root cause: this codebase has a known, previously-diagnosed condition
  (see the `pacing.is_interpolating` diagnostic already in `m_Do_main.cpp`,
  added for an earlier black-screen investigation) where
  `dusk::vr::tick()` — the function that does ALL real per-eye VR
  rendering — is skipped entirely on some frames. If F9 happens to land
  on one of those frames, the capture bracket still opens/closes
  correctly but catches almost nothing; what you see in the headset at
  that instant is just leftover content from a prior frame still sitting
  in the shared eye texture (RenderDoc shows whatever bytes are
  currently in a resource regardless of whether the capture itself wrote
  them). **Not a setup problem — just try F9 again.** A real capture is
  unmistakably different: hundreds/thousands of events, real bound
  textures, and a noticeably longer gap between the
  `[dusk::renderdoc] StartFrameCapture`/`EndFrameCapture`
  `OutputDebugStringA` lines (already present in `m_Do_main.cpp`). (6)
  **Found and fixed a related bug while investigating (5)**:
  `m_Do_main.cpp`'s `if (!aurora_begin_frame()) { ...; continue; }`
  early-return skipped the `EndFrameCapture` call entirely (that check
  lives further down, unreached by this `continue`) — meaning if
  `aurora_begin_frame()` ever fails a few times in a row while a capture
  is open (window resize/minimize, etc.), the capture would stay open
  across ALL of those failed iterations instead of closing after one
  frame, silently accumulating far more GPU work than intended — a
  plausible cause of RenderDoc/driver instability on a later, oversized
  `EndFrameCapture`. Fixed by closing the capture on that early-return
  path too, matching the "F9 always captures exactly one frame" contract.
  Built, not yet confirmed whether this was the actual cause of the
  reported "pressing F9 twice crashes" symptom — worth retesting
  specifically whether that crash still happens now.

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
hand position — see section 16; user-confirmed fixed in-headset as of
2026-08-05 (took three rounds — sheathe/stow gating, then a genuinely
separate item-joint-vs-hand-joint offset — see section 16 for both).
Movement direction now actually follows the player's real head tracking
instead of the flatscreen third-person camera's own angle (which had no
relationship to where the HMD was looking) — see section 17; user-
confirmed fixed in-headset 2026-08-07. Link's face/hat/arms/ears — hidden
during first-person gameplay so the player doesn't see them from the
inside — now correctly show again during the two third-person fallback
cases (cutscenes, Wolf Link) instead of staying hidden there too — see
section 18; user-confirmed fixed in-headset same day. VR now stays
first-person during ordinary NPC dialogue instead of falling back to
third-person the way it still correctly does for actual cutscenes and
door/transition events — see section 19; user-confirmed fixed in-headset
2026-08-08, after a first attempt that turned out to exclude basically
all dialogue due to an untested assumption about `checkPlayerDemoMode()`,
caught via a real log capture rather than a second guess. Separately,
VR hands/body measurably lag behind during fast in-game locomotion — a
long investigation (extrapolation, late-latching, a body-position-offset
fix, a legacy-draw-pass corruption fix) each turned out real but
insufficient, until two real debugger call stacks (2026-08-09) found the
actual, final root cause: `mDoExt_modelEntryDL()` (`m_Do_ext.cpp`, called
from `daAlink_c::modelDraw()` — the shared entry point for Link's body,
hands, sword, shield, and held item) skips the real matrix/geometry
resubmission entirely unless `dusk::frame_interp::is_sim_frame()` is
true — which is only ~30 times/sec (physics-tick rate), not the ~60-90Hz
VR actually renders at. Correct and intentional for flatscreen (a lower-
level interpolation system substitutes smoothed matrices instead), but
VR's tracked-hand/sword/body overrides don't go through that system at
all — they were computing genuinely correct data every eye this whole
investigation, it just almost never reached the actual rendered frame.
Fixed by calling the always-fully-updates sibling function,
`mDoExt_modelUpdateDL()`, instead, while a real VR eye pass is open. See
section 20's update — **built, not yet tested in-headset**. Cutscenes
now also stay first-person, but only when
Link's own body is actually loaded/drawn for that shot (checked via
`checkPlayerNoDraw()`, third-person fallback stays for shots where he's
hidden/swapped for a stand-in) — see section 21; first in-headset test
found mounted (Epona) cutscenes anchored the camera inside the horse's
head, fixed with a carve-out (mounted cutscenes stay third-person,
ordinary mounted gameplay unaffected) — user-confirmed fixed in-headset
2026-08-08. Night-sky stars, which looked wrong in VR (same camera-locked-
effect class as the sun/heat-wave kagerou bugs — billboards oriented off
the stale flatscreen camera matrix), are now disabled in VR only — see
section 22; user-confirmed fixed in-headset 2026-08-08. The VR camera is
now anchored to Link's root/core position (physics-driven, not animated)
plus a calibrated fixed height offset, instead of his animated head joint
directly, as a deliberate motion-sickness-comfort tradeoff (no longer
tilts/bobs with head/torso animation) — see section 23, user-confirmed
fixed in-headset 2026-08-09, with a genuine bonus: **this also fixed
section 20's body-lag symptom** ("body doesn't lag behind" — the leading
theory now is that it was really an oscillating-extrapolation-vs-static-
mesh mismatch, not the geometry-resubmission-frequency bug section 20 had
pinned it on; see section 23 for the full reasoning). **Hands were
still laggy as of the 2026-08-09 session above, but this is now FULLY
RESOLVED (a separate, later 2026-08-09 session)** — see section 20's
"ACTUALLY FINALLY RESOLVED" update for the real root cause
(`daAlink_c::draw()`'s dead call site, plus a stale once-per-tick
`frame_interp` interpolation silently overriding any per-eye VR write
regardless of correctness) and fix. Sword/shield, reported laggy in that
same later session, are fixed too (same root cause, same fix shape). A
follow-up nudge (to clear Link's hunched-forward neck/back from view
while running) was added and tuned the same session — settled at 3in up/
6in forward, CONFIRMED FIXED IN-HEADSET ("Yup thats the right spot"). Two
new gaps surfaced, not yet investigated: **swimming needs a fix** (user's
words) and **crawling needs testing** — both because section 23's height
calibration was only reasoned through for standing gameplay; see section
23 for what's already known (the base game has its own swim-specific
root-relative eye math this doesn't yet account for). A further, still
later 2026-08-09 session split the camera anchor itself: gameplay keeps
the core anchor, but cutscenes/NPC dialogue now use the ORIGINAL animated
head-joint anchor instead (section 23's "gameplay vs. cutscene anchor
split" update), confirmed in-headset. Yet another, still later 2026-08-09
session addressed the swimming gap flagged above: the camera now falls
back to the same original animated head-joint anchor while
`MODE_SWIMMING` is set (same reasoning as the cutscene/dialogue split —
the core anchor's height calibration doesn't hold up in water), and
tracked-hand controller override is skipped while swimming too, so the
normal swim-stroke arm animation shows instead of the hands fighting with
controller tracking (see section 23's "Swimming camera fix" update) —
**camera half CONFIRMED WORKING in-headset; hand half did NOT work,
not yet root-caused, user explicitly deferred further investigation.**
The identical pair of fixes was then extended to crawling (a
`daAlink_PROC`-state check via a new `isCrawling()` helper, since crawling
has no single `MODE_FLG` bit the way swimming does) — **the hand half
failed here too ("locked in an animation," user deferred further work on
both swimming's and crawling's hand fix)**, camera half not separately
confirmed (see section 23's "Crawling" update). The swim-ripple trail's
screen-space-reflection half is now disabled in VR — **CONFIRMED WORKING
in-headset** ("Water particles are gone"). Cloud shadows (camera-locked,
moving opposite head-turn, Ordon Village) took much longer: the billboard
system (`drawCloudShadow()`) originally disabled turned out to be the
wrong mechanism, and four more guesses after it (ground-decal texture
scroll, a real-but-unrelated object-shadow bug, "Moya" ground haze, a
reverted flatscreen-camera-sync experiment) all turned out wrong or
insufficient too — full trail below if ever needed, but **the bug is now
FULLY FIXED, CONFIRMED IN-HEADSET 2026-08-10** (see the dedicated
"ACTUALLY ROOT-CAUSED AND FIXED" box near the end of this section): the
real cause was a TEV `KColor(1)` wash-out on terrain materials `MA00`/
`MA01`/`MA16` in `dKy_bg_MAxx_proc`, found via a completely separate mods
repo (`dusklight-mods`, a different author's `effect_remover` mod) rather
than by continuing to bisect this repo's own code. `MA04` (the Faron
forest-floor/tree-shadow material) was deliberately left untouched and
confirmed still working. All `[dusk::cloudshadow]` diagnostic logging
(`drawCloudShadow()`, `dKy_cloudshadow_scroll()`) has since been removed,
per this project's normal practice, now that the bug is confirmed fixed
(including the `#include <windows.h>` in `d_kankyo_rain.cpp` that only
existed for it). A broader idea was then tried and REVERTED same session: syncing the
shared flatscreen camera object's rotation to the headset every frame
(would have fixed the whole camera-anchored-effect bug class at once, at
the cost of also affecting audio panning and any other gameplay system
reading that camera) — built, tested, no noticed benefit, and a real
regression (cutscene camera hijacking) was found and fixed, but a
residual unaudited risk remained, so the user chose to drop it entirely
("Yeah maybe remove it") rather than keep it. Fully removed, not just
disabled. Cloud shadows were then disabled two more ways
(billboard packet, ground-decal texture scroll) — user tested with a real
log capture and confirmed NEITHER mechanism ever fired, ruling both out.
A real, separate bug (`dDlst_shadowReal_c` object shadows drawing
unconditionally in VR, never actually covered by the "shadows disabled in
VR" guard) was found and fixed along the way — genuinely worth keeping,
but user-tested and confirmed NOT the actual cause of the reported
symptom either. A modder-suggested lead ("Moya" ground haze —
`dKyr_mud_draw()`/`dKyr_evil_draw()`) was tried and RULED OUT by the user
("Moya isnt it"), then fully reverted (Moya renders normally in VR again,
as requested). User's sharper description of the actual symptom:
**"slightly darkened spots on the ground"** — small discrete dark
patches, not a broad haze and not full shadow blobs. Both still-active
disables (`drawCloudShadow()`, `dKy_cloudshadow_scroll()`) remain in
place (inert either way — never confirmed entered) and the
`dDlst_shadowReal_c` object-shadow fix remains in place (a real,
independently-valid fix, just not this symptom). **Next step, agreed
with the user: RenderDoc capture, not another named guess** — see the
"REVERTED" update after section 23's Moya writeup. Other known issues
below.

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
| Left squeeze/grip | **Analog R / raise shield** (2026-08-13, see UPDATE below — was unbound) |
| Right A button | A (context action) — unchanged |
| Right B button | B (attack) — unchanged |
| Left X button | **D-pad left** (was X) |
| Left Y button | **D-pad up** (was Y) |
| Left menu button | Start (pause) — unchanged |
| Right stick click | Start (pause) — unchanged |
| Left stick click | **Z** (was D-pad right, from the update directly above — that assignment lasted less than a day) |
| — | **D-pad right — unbound** (nothing currently maps to it, now that left stick click moved to Z) |

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

**UPDATE 2026-08-13 — R bound to the left controller's squeeze/grip
instead, superseding the gesture idea above (simpler request, no gesture
detector needed).** Per explicit user request ("bind R to the squeeze
button on the left controller"): `g_squeezeValueAction` already had both
hands' subaction paths bound in `vr_xr_bootstrap.hpp` (declared "both
hands" from the start, only the right side was ever actually read) — no
bootstrap changes needed, just a second `getFloatAction(g_squeezeValueAction,
g_leftHandPath)` read in `tick()`. Mirrors left trigger's existing
analog-L pattern (continuous 0-255 value written to `padStatus.triggerRight`
alongside the `PAD_TRIGGER_R` bit, gated on the same low `kTriggerDeadzone`)
rather than X's binary squeeze-threshold gate — raising the shield seemed
like the kind of thing that plausibly wants an analog feel the way L's
aiming does, not just on/off. `wantsVirtualPad`'s OR-chain already checked
`padStatus.triggerRight != 0` (a leftover from before the 2026-08-04 remap
above unbound R) so no change was needed there. Built successfully
(RelWithDebInfo, clean, only `vr_main.cpp` recompiled). **CONFIRMED WORKING
IN-HEADSET** — user tested and reported "It works." Closes out this
binding; the earlier "replace R with a gesture" idea stays superseded/
unbuilt (`g_rightSwing`/`vr_swing_detector.hpp` still sit ready if a future
request wants a gesture again).

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

**ROUND 4 (2026-08-13) — round 3 tested, user reported "missed swings (real
swings don't register)". Real capture analyzed (227 samples), root cause
found, built, NOT yet retested in-headset.**

The 227-sample capture ruled out `triggerSpeed` immediately: the vast
majority of samples with speed well above `2.2` m/s — dozens of them, many
in the 3-7+ m/s range — simply never triggered. Cross-checked against
`SwingDetector::update()`'s actual logic (`vr_swing_detector.hpp`) rather
than guessing: `canFire_` only resets once speed drops to AT OR BELOW
`resetSpeed` — a hard one-shot re-arm gate, not a decaying window. During
real continuous swinging, hand speed rarely dips all the way down between
individual swings, so round 3's `resetSpeed=0.7` left the detector stuck
not-armed through most of a multi-swing flurry — directly confirmed in the
capture (one stretch: a real trigger at 2.48 m/s, then six consecutive
high-speed samples up to 6.4 m/s all logged `TRIGGERED=0`, before it
finally re-armed roughly 650ms later). This is the opposite failure mode
from round 2's "one continuous swing double-counts" concern that motivated
raising `resetSpeed` in the first place — but `cooldownSec` (a hard TIME
lockout, independent of velocity) already covers that same concern on its
own, so lowering `resetSpeed` back down doesn't reopen the double-count
problem it was raised to fix.

**Fix**: `resetSpeed` 0.7 → **0.4** (matching round 1's original value,
now re-derived from evidence rather than the original blind guess).
`triggerSpeed`/`minSwingDistance`/`cooldownSec` left untouched — no
evidence in this capture that any of them are currently a problem
(neutral-movement speed stayed comfortably under ~1.1 m/s against the 2.2
m/s trigger).

**Bonus fix from the same capture**: its very last sample logged a
one-frame `31.6` m/s spike (a tracking-glitch teleport — position jumped
0.6m in ~19ms — roughly 4x any genuine swing peak seen across every
capture so far) that fired a trigger under the old logic. Added
`SwingDetector::maxPlausibleSpeed` (default `15.0f` m/s, generous headroom
above any real swing observed) — `aboveTrigger` now also requires
`speed <= maxPlausibleSpeed`, so an implausible teleport-speed frame can't
fire a false attack. Generic, in the shared header, so it also protects
`g_rightSwing` if that's ever revived.

Built successfully (RelWithDebInfo) — `vr_main.cpp` and
`vr_swing_detector.hpp` recompiled, clean link, no new warnings.

**ROUND 4 RESULT**: user tested and reported "responds to the first
swing sometimes, but if I swing it fast left and right it doesn't
react" — a fresh 375-line `[dusk::vr::swingdiag]` capture confirmed
round 4's fix worked as intended: triggers now fire steadily and
frequently throughout continuous swinging (no more multi-hundred-ms
stuck gaps), and the new `maxPlausibleSpeed` ceiling correctly rejected
a real 21-26 m/s tracking-glitch spike. So the DETECTOR was no longer
the bottleneck — the gap was downstream of it.

**First theory (asked, REJECTED by the user with a decisive
counter-argument)**: guessed Link's own attack-animation lock (can't
start a new swing mid-recovery, same as mashing the real B button). User
correctly pointed out mashing the REAL controller button DOES attack
repeatedly, so if the animation lock were the cause, real button-mashing
would show the same symptom — it doesn't, so this wasn't it. **Lesson**:
when a plausible-sounding theory gets a specific, falsifiable
counter-example from the user, that's real evidence, not just pushback —
re-derive from the mechanics instead of defending the guess.

**ACTUAL ROOT CAUSE, found by re-reading this file's own already-written
"KNOWN LATENCY" comment (near the `wantsVirtualPad` block) with fresh
eyes**: `mDoCPd_c::read()` — the real game-logic button read — runs on
the ~30Hz SIM-TICK loop, BEFORE `dusk::vr::tick()` (this function) even
runs that frame. A REAL held button stays "on" across MANY real frames
(the physical trigger/click is genuinely held down), so some sim tick is
guaranteed to see it during that hold. `leftSwingEvent.triggered`,
however, is a genuine ONE-FRAME pulse by design (the detector fires it
for exactly one `update()` call) — and was being OR'd into
`PAD_BUTTON_B` for exactly that one real frame (~15-20ms at this
project's typical VR framerate), which is SHORTER than the ~33ms gap
between sim-tick reads. A one-frame pulse has a real, independent chance
of landing entirely in the dead zone between two sim-tick samples and
never being read at all — explaining both "sometimes the first one
lands" (luck of frame alignment) and "repeated fast swings don't" (each
swing's pulse separately rolls the same bad odds, and bad luck compounds
across several in a row).

**Fix** (`vr_main.cpp`): a `s_leftSwingButtonHoldRemaining` timer (real
wall-clock seconds, decremented by `pacing.presentation_dt_seconds` each
frame) latches `PAD_BUTTON_B` "held" for `kSwingButtonHoldSec = 0.1`
(100ms) after `leftSwingEvent.triggered`, instead of OR'ing the raw
one-frame edge directly. 100ms is ~3x a 30Hz sim-tick period — comfortable
margin for at least one (usually several) sim-tick reads to catch it,
while still reading as instantaneous to the player, the same way a real
quick button tap already produces a many-real-frame-long physical "held"
signal rather than a true single-frame one. The `[dusk::vr::swingdiag]`
log still reports the raw `leftSwingEvent.triggered` edge (unchanged) —
only the actual `PAD_BUTTON_B` feed changed.

Built successfully (RelWithDebInfo) — only `vr_main.cpp` recompiled,
clean link, no new warnings.

**Round 5 tested — user report: "Seems maybe a bit better but still, if I
spam swing it left and right it doesn't react. Like I'll swing it back
and forth really fast and link will just stand there after one swing."**
A hard stall after the first swing (not a probabilistic miss) — a
different, more specific symptom than round 4's, worth its own diagnosis
rather than another guess.

**User asked specifically about the cooldown** (explained what
`cooldownSec` does — a flat time floor between triggers, separate from
`resetSpeed`'s speed-based hysteresis) and asked to try disabling it
(`cooldownSec` 0.15 → 0.0). Built, tested — only marginal improvement,
confirming `cooldownSec` was never the dominant blocker for this specific
symptom.

**Round 6 — actual root cause found by re-reading `SwingDetector::update()`
with the "stand there after one swing" framing in mind**: `resetSpeed`'s
re-arm condition (`canFire_` only resets `true` once scalar `speed` drops
to or below `resetSpeed`) implicitly assumes a real pause/deceleration
between swings. But `speed` is `dist/dt` — a scalar MAGNITUDE, not a
directional quantity. A fast, tight, CONTINUOUS back-and-forth flick can
keep that magnitude elevated the entire time even though the DIRECTION
reverses at each end — the hand never actually slows down, it just
changes which way it's going. For that motion, `canFire_` can get stuck
`false` indefinitely after the first trigger, exactly matching "just
stands there after one swing" (a hard stall, not bad luck) — and this is
a structurally different, worse case than round 4's fix addressed
(round 4's `resetSpeed=0.4` still assumed speed dips SOME amount between
swings; a true rapid-spam motion may never dip at all).

**Fix** (`vr_swing_detector.hpp`): added a SECOND, independent re-arm
signal based on DIRECTION reversal rather than speed magnitude. Each
trigger now records its own motion vector (`lastFireDelta_`, unnormalized
— sign of a dot product doesn't need normalization to detect "pointing
substantially the opposite way"). On every subsequent frame, if the
current frame's motion delta has a NEGATIVE dot product against
`lastFireDelta_` (moving in a substantially different/opposite direction
than the motion that produced the last trigger), `canFire_` re-arms
immediately — regardless of whether scalar speed ever dropped.
`resetSpeed`'s existing speed-based re-arm is left completely intact
(untouched code path) — the two conditions now OR together, so either a
real speed dip (a normal, unhurried swing) OR a direction reversal (a
fast continuous flick) can re-arm the detector. The actual trigger's own
speed/distance/cooldown checks are unchanged and still gate whether a
re-arm actually produces a new event, so this doesn't loosen anything
about what counts as "fast enough" to be a swing — it only fixes when the
detector is ALLOWED to consider firing again.

Built successfully (RelWithDebInfo) — `vr_main.cpp` and
`vr_swing_detector.hpp` recompiled, clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Sooooooo much
more responsive, sword works now." Closes out the whole swing-detector
saga (rounds 1-6): the detector's own tuning (round 4's `resetSpeed`
fix), the button-delivery timing (the 100ms `PAD_BUTTON_B` latch, same
session), and the direction-reversal re-arm (round 6) were three genuinely
separate, real bugs, not three guesses at the same one — each was found
by taking a specific user-reported symptom seriously and re-deriving from
the actual code/data rather than retuning a number blindly, including one
case (the animation-lock theory) where the user's own counter-argument
correctly overturned a plausible-sounding guess before it was ever coded.

`cooldownSec` is currently `0.0` (round 5, disabled to rule it out as the
round-5 blocker). Left at 0.0 for now since the user hasn't reported
attacks firing uncomfortably often — if rapid spam ever starts feeling
too spammy, a small nonzero value (e.g. 0.05-0.1) can be reintroduced
without risk of reintroducing the round-6 stall, since the two fixes are
independent (direction-reversal re-arms `canFire_`; `cooldownSec` is a
separate, independent time gate checked alongside it).

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

### 17. Movement direction not relative to the headset — CONFIRMED FIXED IN-HEADSET 2026-08-07

**Symptom** (user-reported): "Link's movement is not relative to the
headset. It almost seems as if there is a flatscreen camera still
affecting the direction he moves in." Precisely accurate, per root cause
below.

**Root cause**: `daAlink_c`'s movement-direction calc (`d_a_alink.cpp`,
human-form normal-gameplay branch) has always built
`mMoveAngle = mStickAngle + dCam_getControledAngleY(...)` —
`dCam_getControledAngleY()` reads the flatscreen third-person chase
camera's own angle, driven by the base game's normal auto-follow camera
logic. That angle has zero relationship to which way the player's head is
actually turned in VR — the *render* camera is anchored to Link's head
(section 11), but this GAMEPLAY angle is a completely separate value that
was never touched. Section 15's smooth-turn work (2026-08-05) only ever
added the right-STICK's own turn offset on top of this camera angle
(`mMoveAngle += cM_rad2s(dusk::vr::getSmoothTurnYawRad())`) — so turning
your physical head without touching the stick never changed which way
"forward" on the movement stick walked Link. Section 15's fix made
*look* direction and *stick-driven turn* stay in sync; it never made
movement direction follow actual head tracking.

**Fix**: replaced the whole basis rather than patching another term onto
it.
- `vr_stereo_render.hpp`: factored the raw (undamped) head-forward-
  direction math already used internally by `updateHudSmoothing()` out
  into its own reusable `computeHeadWorldForward(headPose, yawRad)` —
  same "one source of truth" reasoning as `vr_smooth_turn.hpp`'s own
  header comment (section 14's lesson about duplicated formulas silently
  drifting out of sync). `updateHudSmoothing()` now just calls it and
  low-pass-filters the result for the HUD; a second consumer (below) uses
  it directly, undamped.
- `vr_main.hpp`/`.cpp`: new `dusk::vr::getHeadMoveAngleS()` — the real,
  undamped in-game yaw (same s16 binary-angle unit as `mMoveAngle`/
  `shape_angle.y`) the player's HMD is currently facing, ALREADY
  including the VR smooth-turn offset (passed straight into
  `computeHeadWorldForward` as `yawRad`, matching how `eyePoseToViewMtx`
  itself bakes it in). Computed once per frame in `tick()`, right after
  `updateSmoothTurn()` (needs that frame's yaw fresh) and using the
  already-located `hmdPose`, via `cM_atan2s(headForward.x, headForward.z)`
  — the same `atan2s(x, z)` convention this engine uses everywhere else
  for a direction vector to world-yaw conversion (`d_a_b_ob.cpp`,
  `d_bg_w.cpp`, etc.), not a new one invented for this fix. Deliberately
  undamped, unlike the HUD's `g_hudSmoothedWorldForward` — movement
  direction should track head rotation immediately, not lag.
- `d_a_alink.cpp`: `mMoveAngle = mStickAngle + dusk::vr::getHeadMoveAngleS()`
  when `isRenderingToHeadset()`, replacing
  `mStickAngle + dCam_getControledAngleY(...)` entirely for that branch
  (flatscreen keeps the original camera-angle basis unchanged). The old
  separate `+= cM_rad2s(getSmoothTurnYawRad())` line from section 15 was
  removed — redundant now that the yaw is already folded into
  `getHeadMoveAngleS()` itself.

**Scope note, not yet addressed**: `d_a_alink_demo.inc` has an identical
`mStickAngle + dCam_getControledAngleY(...)` line (a narrow
demo/cutscene-transition case, `isDemoTypeStart` + `PROC_MOVE`/
`PROC_WOLF_MOVE`) that was deliberately left untouched — out of scope for
the reported symptom (normal gameplay), unconfirmed whether it's even
reachable in a way a player would notice. If VR movement ever feels wrong
specifically during a demo/cutscene transition, this is the other call
site to check.

**Confirmed fixed in-headset** — user tested and reported "Fixed."

### 18. Face/hat/arms/ears stayed hidden outside first-person (cutscenes, Wolf Link) — CONFIRMED FIXED IN-HEADSET 2026-08-07

**Symptom** (user request, not a bug report this time — "if not in
gameplay (or first person, whatever you have the function as) then show
all of link's limbs"): `vr_link::updateFrame()` (`vr_link_visibility.hpp`)
hides Link's face, hat, arms, and ears every VR frame so the first-person
view doesn't show his own head/limbs from the inside (necessary and
correct while actually looking through his eyes) — but it was doing this
**unconditionally** for face/hat (`hideModel()`, no gating at all) and
almost-unconditionally for arms/ears (`hideArmsAndEars()` only skipped
itself for Wolf form, never for cutscenes). So the two existing
third-person fallback cases — cutscenes/events (`checkEventRun()`) and
Wolf Link (`checkWolf()`), see section 11 — showed Link's third-person
body with his face and hat missing (both cases) and, during cutscenes
specifically, his arms and ears missing too.

**Fix**: `updateFrame()` now computes
`firstPerson = !link->checkEventRun() && !link->checkWolf()` — the exact
same condition `getVrCameraEyeAnchor()` (a few hundred lines further down
the same file) already uses to decide first-person-head-anchor vs.
third-person-fallback. Mirrored inline rather than factored into a shared
helper (two call sites doesn't justify a third piece of indirection).
When `firstPerson` is true, hides face/hat/arms/ears exactly as before.
When false, now calls `showModel()` on face/hat and a new
`showArmsAndEars()` (a straight mirror of the existing
`hideArmsAndEars()`, same material-index list, same `checkWolf()` guard —
required because Wolf form reuses `mpLinkModel` with a swapped material
table, so those indices don't mean "arm/ear" there; touching them in wolf
form would show/hide random wolf materials by coincidence of index) to
restore everything. Runs every frame in both directions, not just on the
first-person/third-person transition — matches `hideArmsAndEars()`'s own
pre-existing reasoning (the base game's per-frame outfit-branch logic can
re-hide/re-show an overlapping subset of these same shapes on any given
frame for unrelated reasons, so a one-shot toggle would get silently
reversed by that unrelated logic later).

**Known pre-existing dead code, unrelated to this fix, not touched**:
`vr_link::restoreVisibility()` (same file) already existed to
`showModel()` face/hat, but nothing anywhere in the codebase actually
calls it — it's dead code, and even if it were wired up it doesn't restore
arms/ears either. Not a regression from this session and out of scope for
the user's request (which was about third-person fallback DURING an
active VR session, not about what happens after the headset disconnects),
but worth knowing about if Link's limbs are ever reported stuck hidden
after a VR session ends.

**Confirmed fixed in-headset** — user tested and reported "Fixed."

### 19. VR stays first-person during NPC dialogue instead of falling back to third-person — CONFIRMED FIXED IN-HEADSET 2026-08-08

**Goal** (explicit user request, and a genuine follow-up investigation
first — see below): "I want the game to stay in first person while
talking to npcs." Before this, `isFirstPerson()`/`checkEventRun()`
(section 18) treated ANY running event — cutscene, door/transition, or
plain dialogue — identically, falling back to third-person for all of
them.

**Investigation first** (user asked to "find out" whether cutscenes,
dialogue, and transitions/doors are three separate functions before
requesting a fix): they are NOT — confirmed by reading `d_event.h`/
`d_event.cpp`. All three are dispatched as different `dEvt_type_e` values
(`TALK_e`, `OTHER_e`/`COMPULSORY_e`, `DOOR_e`/`TREASURE_e`) through ONE
shared `dEvt_control_c` object's `entry()`/`Step()` state machine, and
every type flips the exact same `mEventStatus` bit that
`dComIfGp_event_runCheck()` (and thus `daAlink_c::checkEventRun()`)
reads — confirmed door events specifically also set `mMode =
dEvt_mode_DEMO_e`, the SAME mode a scripted cutscene uses (`doorCheck()`
in `d_event.cpp`), and even call `sceneChange()` →
`dStage_changeScene4Event()` to actually load the new area once the door
animation finishes. The ONE thing that DOES distinguish plain dialogue is
`dEvt_control_c`'s own `mMode`: `talkCheck()`/`talkXyCheck()` set it to
`dEvt_mode_TALK_e` specifically. Separately, message/text-box display has
its own genuinely independent tracker, `dMsgObject_c`/
`dMsgObject_isTalkNowCheck()`, NOT consulted by `checkEventRun()` at all —
worth knowing about if a future request wants VR behavior keyed
specifically on "a textbox is on screen" rather than "an event is
running."

**Fix, round 1** (`vr_link_visibility.hpp`): refactored the previously-
duplicated first-person condition (section 18's inline `!checkEventRun()
&& !checkWolf()`, copy-pasted at two call sites) into one shared
`isFirstPerson(daAlink_c*)`, called by both `getVrCameraEyeAnchor()` and
`updateFrame()` so they can't drift out of sync (same standing lesson as
`vr_smooth_turn.hpp`'s own header comment). First version: stay
first-person if no event is running, OR if an event IS running but its
mode is `dEvt_mode_TALK_e` AND `!link->checkPlayerDemoMode()` (the latter
guard reasoned from an untested theory: some story-important
conversations are staged as full demos with dialogue baked in rather than
a plain TALK event, so this should exclude those). Built clean.

**User report: "Still third person"** (dialogue didn't stay first-person
at all). Rather than guess a second time, added temporary
`[dusk::vr::fpdiag]` logging (`isFirstPerson()`) printing
`runCheck`/`mode`/`playerDemoMode`/`result` on every state change plus
every 60 frames while an event is active. **Real capture proved the
`checkPlayerDemoMode()` theory wrong**: during an entire real, ordinary
conversation (`mode=1`/TALK for its whole duration), `playerDemoMode`
read `true` for the ENTIRE conversation too — Link apparently runs
through some local demo-driven "stop and face the NPC" state just to
hold a normal conversation at all, not only for staged cutscenes-with-
dialogue. That guard was therefore excluding essentially ALL dialogue,
not just the narrow cutscene case it was meant to carve out. **Fix, round
2**: removed the `!checkPlayerDemoMode()` condition entirely —
`isFirstPerson()` now stays first-person for ANY event whose
`dEvt_control_c` mode is `TALK`, full stop. The logged `mode=2`/DEMO
blocks for real cutscenes/other events in the same capture confirmed
nothing gets confused by dropping it. Diagnostic logging removed once
this was root-caused (per this project's normal practice).

**Confirmed fixed in-headset** — user tested the round-2 build.

### 20. VR hands lag behind during fast in-game movement — INVESTIGATED, ONE FIX LANDED (measurable but not the cause), ROOT CAUSE STILL UNCONFIRMED, PAUSED 2026-08-08

**Symptom** (user-reported): "When I am moving fast they [the hands] lag
behind." Confirmed via follow-up questions to be specifically about
in-game locomotion speed (walking/running via the movement stick), not
swinging the physical controller while standing still, and specifically
just the hands — the world/camera stays visually smooth (rules out a
frame-rate/stutter explanation), and it's just as bad in a straight line
as while turning (rules out a curved-motion-specific explanation).

**Theory 1 (real, measurable, but proven NOT the cause of this
symptom)**: `getVrCameraEyeAnchor()` (section 11) lerps Link's
sim-tick-rate `getSubjectEyePos()` between the last two committed sim
ticks — worked through the math against `dusk::game_clock`'s actual
`sim_pace() = 1/30s` and its `render_time = now - kSimPeriodDuration`
design (`game_clock.cpp`): this interpolation scheme renders a CONSTANT
~33ms (one full sim tick) behind real time, always, not just occasional
jitter. **Fix applied** (`vr_link_visibility.hpp`, `detail::
kEyeAnchorExtrapolationGain`): switched the anchor from interpolating
between the two past samples to extrapolating past the most recent one —
reusing the existing `lerpXyz(a, b, t)` helper with `t = step + gain`
(gain=1.0 default; `lerpXyz` for `t>1` already extrapolates past `b`, no
separate function needed) — which the math shows should cancel almost
all of that constant lag for smooth/near-constant-velocity motion.
Deliberately scoped to only this VR-local anchor, not the shared
`dusk::frame_interp` module the flatscreen camera uses.

**Verified via a real capture that the fix is genuinely active**: added
temporary `[dusk::vr::anchordiag]` logging comparing the old
(plain-interpolated) vs new (extrapolated) anchor value every 20 frames.
During real running, this showed substantial, real corrections (mostly
20-95 game units per sample, i.e. roughly 0.2-1 metre at this engine's
~100-units-per-metre scale), dropping to near-zero the instant the player
stopped moving — proof the mechanism is doing real, non-trivial work, not
a no-op.

**User-tested anyway: "Hands still lag behind," "feels exactly the same
as before," "just as bad in a straight line."** This combination is
actually a mathematical proof the anchor's own timing was NEVER visible
in the first place, not just that the fix didn't help enough: hands and
the camera share the IDENTICAL `eyePos` anchor value every frame (same
`getVrCameraEyeAnchor()` call, cached once per frame in
`vr_link_visibility.hpp`'s `updateFrame()`), so `hand_world - camera_world`
algebraically cancels `eyePos` out ENTIRELY regardless of how
accurate/laggy it is — re-derived precisely: with `hmdRefPos` confirmed
(by reading the actual `EyeParams` construction in `vr_main.cpp`) to be
`hmdPose.position`, i.e. the SAME live per-frame HMD position used by
both the camera's offset-from-head math (`eyePoseToViewMtx`) and the
hand's offset-from-head math (`buildHandMtx`), `hand_world - camera_world
= (controllerPose - eyePose) * scale` — completely independent of both
`eyePos` AND `hmdPos`, both already fresh/live every frame. **The
extrapolation fix is left in place** (real, harmless, measurably reduces
a genuine — if apparently imperceptible in practice — timing error), but
it is CONFIRMED not the answer to this symptom.

**Re-read `buildHandMtx()` (`vr_link_visibility.hpp`) end to end to
double-check for any other smoothing**: none found — `dx,dy,dz =
controllerPose - hmdPos` (both this-frame, live, no filtering), rotated
by the current smooth-turn yaw, added onto the anchor. The position math
is clean.

**Leading remaining theory, NOT YET CONFIRMED**: VR compositor
reprojection. Most OpenXR runtimes correct the rendered frame for
last-moment HEAD rotation right before actually displaying it (this is
why the world/camera stays smooth even under real frame-timing variance)
— but that correction is head-orientation-based and doesn't know about
or adjust arbitrary rendered geometry like tracked-hand meshes. If there
is ANY gap between when this app samples the controller pose
(`g_session->predictedDisplayTime()`, already the OpenXR-recommended best
practice) and when the frame is actually displayed, the world gets
silently corrected for it by the runtime and the hands do not — which
would produce exactly "world smooth, hands specifically lag," and would
plausibly get worse under any timing variance correlated with movement
(more to render/stream while running fast). This exactly matches every
constraint gathered from the user's answers. **Not proven** — this is
the best remaining hypothesis after ruling out the anchor, turning-
specific error, and frame-rate stutter, not something confirmed via a
capture the way the anchor theory was.

**Proposed next step, NOT STARTED, paused per explicit user request
("Pause here")**: late-latching — re-locate/re-sample the controller grip
poses as close as possible to actual frame submission (near
`xrEndFrame`) instead of once near the top of `tick()`, and re-apply just
the hand transform update at that later point, narrowing the
sample-to-display gap specifically for hands. This is a real
restructuring of the frame loop (hand pose sampling currently happens
once, early, alongside everything else in `tick()`), not a quick patch —
scope it properly before attempting, and re-verify with a fresh
diagnostic capture (raw controller pose vs. final applied matrix
translation, frame-by-frame) rather than assuming this theory is correct
without evidence, the same discipline that ruled out the previous three
theories here.

**RESUMED 2026-08-08 — late-latching implemented as scoped above, built,
NOT yet tested in-headset.** Per user request to resume ("I need to fix
link's hands lagging behind"), implemented exactly the late-latching
approach this section already scoped, rather than a new theory.

**What changed**:
- `vr_link_visibility.hpp`: factored the two `buildHandMtx()` calls
  `updateFrame()` used to inline directly into a new shared
  `computeTrackedHandMatrices(hmdPos, rightControllerPose,
  leftControllerPose, eyeAnchor, yawRad)` — one implementation, callable
  from both `updateFrame()` (still runs once early, mainly so
  face/hat/arm visibility and the hand matrices are never left
  uninitialized before the per-eye loop starts) and the new late-latch
  call site below. Also removed a stale `[dusk::vr::handoffset]` TEMP
  DIAGNOSTIC log that had been sitting in this exact block since section
  12's position-tracking investigation — labeled "remove once confirmed
  fixed" and position was confirmed fixed back on 2026-08-02; left alone
  until now only because nothing had needed to touch this block since.
- `vr_main.cpp`'s `applyTrackedHandMtx()` — already the proven "last
  write before the draw" call site (section 12), invoked once per eye
  from `d_a_alink.cpp` right before `modelDraw(mpLinkHandModel, ...)` —
  now re-locates the HMD + both controller grip `XrSpace`s AGAIN right
  there (same `predictedDisplayTime`, but called later in real wall-clock
  time than `tick()`'s single early sample) and calls
  `computeTrackedHandMatrices()` fresh with that re-located data,
  immediately before `vr_link::applyTrackedHandMtx()` writes the result
  into the joints. This runs twice per frame (once per eye) — each
  `xrLocateSpace` call is cheap (no GPU sync), so no measurable perf
  concern expected (not separately measured this session).
- Needed a `static XrPosef locateSpace(...)` forward declaration added
  near the top of the file — the real definition sits later in
  `vr_main.cpp`, after `applyTrackedHandMtx()`'s existing position in the
  file. Hit (and fixed) a real C++ rule while doing this: a default
  argument can only be specified ONCE across a declaration+definition
  pair in the same scope — had it on both initially, which is a hard
  compile error ("redefinition of default argument"); moved
  `XrSpaceLocationFlags* outFlags = nullptr`'s default onto the new
  forward declaration only, dropped from the later definition.

**Why re-locating with the SAME `predictedDisplayTime` can still help**
(the reasoning this rests on, written inline in the code too): there's no
legal way to get a genuinely later predicted timestamp mid-frame (a
second `xrWaitFrame` isn't valid between `xrBeginFrame`/`xrEndFrame`) —
but `xrLocateSpace`'s prediction for a given target time is computed from
whatever real IMU/tracking samples the runtime has AT CALL TIME,
extrapolated forward to that timestamp. Calling it again later in real
wall-clock time — after `tick()`'s HUD/minimap capture,
`xrAcquireSwapchainImage` (can block on the GPU), `xrLocateViews`, and
this eye's own full `fpcM_DrawIterater()`+`cAPIGph_Painter()` scene
traversal have all already run — lets the runtime use fresher real data
for that same extrapolation. This is the standard "late-latching"
technique other VR engines use for exactly this kind of hand-tracked-
geometry latency, without needing to restructure around a second
frame-wait. **This is still the leading theory from this section, not a
newly-proven one** — the fix targets the SAME hypothesized cause
(sample-to-display gap) the pause left off on, not a re-derivation.

**Diagnostic logging added, deliberately left in place for the first
test** (per this project's "verify a fix is materially active before
trusting a visual report" discipline — same one section 20's own
extrapolation fix used, and the same one that caught that fix DOING real
work while still not being the actual answer): `[dusk::vr::latelatch]`,
throttled to ~9Hz, logs how far the late-latched sample moved the right
hand's position relative to the frame's original early sample (still
cached in `detail::s_rightHandMtx` at the moment this reads it, just
before being overwritten) — a `correction` distance near zero at rest,
growing during real movement, would confirm the mechanism is doing
real, non-trivial work; nonzero-but-still-laggy in-headset would (like
section 20's extrapolation fix) mean this genuinely isn't the answer
either, not that it's broken.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling, clean link, no new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up:
launch in VR, move at normal/fast locomotion speed, and report (a)
whether hand lag is actually reduced or gone, and (b) paste back a few
`[dusk::vr::latelatch]` lines from during that movement so the
`correction` magnitude is on record either way. If lag persists despite
real, nonzero corrections being logged, that's strong evidence this
late-latching theory — like eye-anchor extrapolation before it — isn't
the actual cause, and the next candidate to investigate would be
something downstream of pose sampling entirely (e.g. whether the
runtime's reprojection is positional/depth-aware at all for this
headset/runtime, which would explain why static world content stays
smooth but moving hand geometry doesn't benefit the same way — not yet
looked into). Remove the `[dusk::vr::latelatch]` log once a real
in-headset verdict is in, per this project's normal practice.

**FIRST IN-HEADSET TEST (same day): "It is fixed in the intro but they lag
behind in gameplay."** Important nuance, likely NOT the clean win it
sounds like at first read: this section's ORIGINAL symptom scoping (top
of section 20) already established standing-still hand movement was
NEVER broken — the reported lag was specifically about in-game locomotion
(walking/running via the stick), confirmed via explicit follow-up at the
time. An "intro" sequence is very likely stationary (cutscene or
standing at a fixed spot before gaining movement control) — meaning
"fixed in the intro" may just be re-confirming the SAME pre-existing
working baseline (stationary hand movement was always fine), not
evidence the late-latch fix changed anything. "Lag behind in gameplay"
is the actual, still-unsolved original symptom, unchanged. **Do not treat
this as "mostly fixed, one edge case left"** — treat it as "possibly a
no-op for the real symptom" until proven otherwise by real data.

**Next step, not yet done**: get the user to capture
`[dusk::vr::latelatch]` lines specifically WHILE running/moving (not
standing still) and report back. Two things that log settles either way:
(a) whether `correction` is meaningfully nonzero during real locomotion
at all (if it's tiny/near-zero even while moving, that's evidence
`xrLocateSpace`'s prediction genuinely isn't changing between the early
and late sample points for this runtime, meaning late-latching was never
going to help the locomotion case regardless of theory) — if so, the
underlying "compositor reprojection doesn't handle moving hand geometry
right" theory may need revisiting entirely (e.g. is this runtime's
reprojection actually motion-smoothing/ASW-style, using PER-FRAME motion
vectors, which would only kick in under real GPU/frame-time pressure —
i.e. present during heavier gameplay scenes but not a light intro/menu
scene — a materially different mechanism than plain last-instant head
reprojection, and one late-latching the CPU-side pose sample can't
address at all, since it's a compositor-side temporal effect between
rendered FRAMES, not a per-app-frame pose-staleness issue). Don't guess
further without this capture in hand — same discipline that's held for
every round of this investigation so far.

**ROOT-CAUSED FOR REAL (same day), before that capture was needed — user's
next report changed the picture entirely: "I noticed link's entire body
lags behind, including the hands. If I move the headset [i.e. as Link
moves] goes forward and link's body lags a bit behind, and thats for all
direction[s]."** This wasn't a hands-specific bug at all — it's Link's
WHOLE BODY visibly separating from the camera, direction-independent,
only while actually moving. That framing pointed straight at something
neither the reprojection theory nor late-latching ever touched, and a
direct code read (not another guess) confirmed it:

- `daAlink_c::setMatrix()` (`d_a_alink.cpp`) builds `mpLinkModel`'s own
  base transform directly from raw `current.pos`/`shape_angle` — **zero
  interpolation** — and is only ever called from `execute()`, i.e. once
  per 30Hz sim tick, same as the rest of this fixed-timestep engine's game
  logic. Confirmed by tracing every call site of `setMatrix()`
  (`d_a_alink.cpp:5064/18225/18554`) back to `daAlink_c::execute()`.
- Meanwhile the CAMERA (`eyePoseToViewMtx`'s `linkEyeGame` argument) and,
  sharing the identical value, the tracked HANDS (`buildHandMtx`'s
  `linkEyeGame`) both read `getVrCameraEyeAnchor()` — which SMOOTHS *and*
  EXTRAPOLATES every render frame (72-90Hz in VR), per this same section's
  earlier `kEyeAnchorExtrapolationGain` fix.
- Net effect: whenever Link is actually moving, the camera/hands glide
  smoothly AHEAD each render frame (literally extrapolated past the
  latest confirmed sim-tick sample), while his own BODY MESH's world
  position stays frozen at whatever `execute()` last set it to — visibly
  stair-stepping 30 times a second BEHIND them. Direction-independent
  (extrapolation applies the same regardless of which way Link moves) and
  invisible at rest (zero velocity → zero extrapolation → nothing to
  diverge) — matching literally every piece of evidence gathered so far,
  including the "fixed in the intro" report from the previous round,
  which in hindsight was never evidence late-latching helped — it was
  just the pre-existing, always-fine, stationary case showing through
  again. **This was very likely the TRUE original cause of "hands lag
  behind" all along** (section 20's opening symptom), more so than the
  compositor-reprojection theory ever was — late-latching probably wasn't
  wrong to try, just solving a real but much smaller effect layered on
  top of this larger one.

**Fix** (`vr_link_visibility.hpp`): rather than building a SECOND,
independent prev/curr+extrapolation tracker for `current.pos`/
`shape_angle` (real risk of the two drifting out of sync under future
retuning — this file's own standing lesson), reuse the eye anchor's
smoothing directly:
- `getVrBodyPositionOffset(daAlink_c*)` — returns
  `getVrCameraEyeAnchor(freshEye) - freshEye`, i.e. exactly how far this
  frame's smoothing/extrapolation already pushed the eye anchor away from
  the raw, this-sim-tick `getSubjectEyePos()` value. Zero whenever
  `isFirstPerson()` is false (cutscenes, Wolf form, mounted cutscenes) —
  the camera doesn't get smoothing there either (falls back to the plain
  flatscreen eye), so there's nothing to compensate for.
- `applyVrBodyPositionOffset(J3DModel* bodyModel)` — adds that offset as a
  pure world-space translation onto the model's `getBaseTRMtx()` (a
  mutable `Mtx&`, no separate setter needed) and calls `calc()`.
  Mathematically exact, not an approximation: translating a PARENT affine
  frame by a fixed delta (rotation untouched) translates every descendant
  joint's resolved world matrix by that exact same delta regardless of
  hierarchy depth — confirmed algebraically before writing this (the
  `worldMtx[i][3] = baseTRMtx[i][0..2]·localMtx[·][3] + baseTRMtx[i][3]`
  composition rule), not assumed. So rigidly shifting just the root moves
  the WHOLE animated body — every joint, everything that reads its
  matrices — in lockstep, not only the root joint itself. Skips the
  `calc()` call entirely when the offset is exactly zero (the common
  case: standing still, or not in first-person), avoiding paying for a
  full body-skeleton recalculation when there's nothing to correct.
- Call site: `d_a_alink.cpp`, human-form draw branch only (not Wolf —
  `getVrBodyPositionOffset()` is always zero there anyway), right after
  `applyTrackedItemMtx()` and right before `modelDraw(mpLinkModel, ...)` —
  same per-eye, last-write-before-draw window as every other VR draw-time
  override in this file. Ordering relative to `applyTrackedItemMtx()`
  doesn't matter for correctness: a pure whole-body translation cancels
  out of that function's relative hand-to-item offset math (translating
  the whole rigid body doesn't change the offset BETWEEN two of its own
  joints).
- `vr_main.hpp`/`.cpp`: new `dusk::vr::applyVrBodyPositionOffset()` thin
  forward, same "keep heavier OpenXR/aurora headers out of core game
  files" pattern as every other function in this file.

**Built successfully** (RelWithDebInfo) — this one triggered a fuller
rebuild than usual (`vr_main.hpp` changed, which more files transitively
include), completed cleanly, no errors.

**NOT yet tested in-headset.** Next step for whoever picks this up:
launch in VR, move around at normal/fast speed, and confirm the body no
longer visibly separates from the camera/hands in any direction. If this
lands, it likely also fully explains (and fixes) the ORIGINAL "hands lag"
report from the top of this section — worth explicitly re-testing hand
lag specifically too, not just the whole-body symptom, before considering
section 20 closed. The `[dusk::vr::latelatch]` diagnostic from the
previous round is still in the tree (harmless, and may yet prove useful
if this fix turns out to be necessary-but-not-sufficient) — remove once
BOTH symptoms are confirmed fixed, not just the new one.

**"Still not fixed" (user report) — real capture found the fix is a
total no-op, root cause of THAT narrowed down further, one more capture
pending.** `[dusk::vr::bodyoffset]`/`[dusk::vr::bodyoffsetdiag]` logging
(added to `getVrBodyPositionOffset()`/`applyVrBodyPositionOffset()`)
proved the call site IS reached every frame, but the computed offset is
**exactly (0,0,0) on literally every sampled frame** across two separate
real captures — not numerical noise (checked via exact float equality
before logging "ZERO"). Traced to: `step` (`dusk::frame_interp::
get_interpolation_step()`) reads exactly `0.0000` every single time,
and since `t = step + kEyeAnchorExtrapolationGain(1.0) = 1.0` exactly,
`lerpXyz(prev, curr, 1.0)` always returns `curr` exactly — and `curr`
always equals `freshEye` by construction (both ultimately read
`getSubjectEyePos()`'s tick-rate-only-updated value) — so the "smoothed"
eye anchor and the raw one are ALWAYS numerically identical at this call
site, making the whole body-offset fix (and, by the same reasoning,
raises real doubt about whether the eye-anchor extrapolation was ever
doing anything for the CAMERA either, at least in this environment).

**First theory (severe framerate, ~15fps) — TESTED AND RULED OUT.**
Miscounted from the sim-tick delta between throttled log samples,
inferring `sim_ticks_to_run` was pinned at the hard cap (2) every single
frame. Added a DIRECT measurement instead
(`[dusk::vr::fpsdiag]`, `pacing.presentation_dt_seconds` +
`pacing.sim_ticks_to_run`, both computed from a real `std::chrono`
timestamp in `game_clock.cpp`, not inferred) — real capture confirmed a
healthy ~55-70fps with `sim_ticks_to_run` normally alternating 0/1 per
frame, matching the user's own runtime-reported 50-70fps. The indirect
inference was simply wrong; this is not a framerate problem. **Lesson
reinforced**: an indirect inference from unrelated counters (even a
seemingly rock-solid, zero-variance one) is not a substitute for a direct
measurement of the actual quantity in question — this project has hit
this exact trap before (section 20's own earlier late-latching detour)
and will again if this isn't internalized.

**Next diagnostic added, not yet captured**: `get_interpolation_step()`'s
return value logged directly alongside `[dusk::vr::fpsdiag]`, at the very
top of `vr_main.cpp`'s `tick()` -- close to where `m_Do_main.cpp` just set
it via `dusk::frame_interp::begin_frame(mode, false,
dusk::game_clock::sample_interpolation_step())`, right before calling
`tick()`. This isolates whether `step` is ALREADY zero at the top of the
frame (pointing upstream, at `sample_interpolation_step()`'s own pacing
math in `game_clock.cpp` -- worth rederiving by hand or script rather
than trusting intuition, since a first attempt at hand-deriving expected
behavior this session gave inconsistent/confusing results and was
abandoned in favor of just measuring) or whether it starts nonzero and
gets reset to exactly 0 somewhere between there and where
`getVrBodyPositionOffset()` reads it deep inside the per-eye draw path
(in which case the culprit is somewhere in VR code specifically --
though a grep for every `begin_frame`/`commit_sim_tick` call site in the
whole codebase found none inside `src/dusk/vr/`, so this would have to be
something less direct, not yet identified).

**Built successfully** (RelWithDebInfo) -- only `vr_main.cpp` needed
recompiling.

**Concrete next step**: one more move-around-and-capture round, this time
searching for `[dusk::vr::fpsdiag]` specifically for the new
`get_interpolation_step()=` field, comparing it against `sim_ticks_to_run`
on the SAME logged frames -- if it's ALSO always 0.0000, the bug is
upstream in `game_clock.cpp`'s pacing model itself (not VR-specific at
all, and would affect the flatscreen camera's own smoothness the same
way, unconfirmed whether anyone would have noticed there since flatscreen
was never specifically retested for this); if it's sometimes nonzero
there but still reads 0 by the time `getVrBodyPositionOffset()` sees it,
the bug is somewhere in between, inside VR code, not yet found.

**Result: `get_interpolation_step()` confirmed HEALTHY at the top of
`tick()`** (varying 0.13-0.99 across real samples, never stuck) -- ruling
out `game_clock.cpp`'s pacing model itself. So the corruption happens
somewhere between the top of `tick()` and where `getVrBodyPositionOffset()`
reads it. Bisected further with two more checkpoints (right before the
per-eye scene draw starts, and right after `fpcM_DrawIterater()` but
before `cAPIGph_Painter()`) -- BOTH still healthy. One more checkpoint at
the very top of `daAlink_c::draw()` (called from inside `cAPIGph_Painter()`'s
traversal) -- **already exactly 0 there**. So the corruption happens
somewhere inside `cAPIGph_Painter()`, before reaching Link specifically.

**ROOT-CAUSED**: rather than keep bisecting by position through a huge,
unfamiliar scene-draw call tree, instrumented the actual mutation point
directly -- `dusk::frame_interp::begin_frame()` itself (the ONLY function
that writes `g_step`, confirmed via an exhaustive codebase-wide grep with
no other call site anywhere, including `extern/aurora`). Logging every
call, unconditionally, caught it directly in a real capture: `begin_frame`
fires MULTIPLE TIMES IN RAPID SUCCESSION mid-frame (5 calls within a
handful of log lines, interleaved with water's GXCopyTex/resolve_pass
texture-bind processing for a single eye -- correlated tightly in every
capture that showed the bug, though not independently proven to be the
*trigger* specifically, just where it was observed happening) -- with a
`step_in=0.0` argument landing right in the middle of an otherwise-healthy
frame. Since there is only ONE real caller of `begin_frame()` in the whole
codebase (`m_Do_main.cpp`'s three call sites, all part of one straight-line
sequence immediately before `dusk::vr::tick()` is invoked), the only way
to reproduce this pattern is if **`dusk::vr::tick()` is being called
RE-ENTRANTLY** -- a nested call starting while an outer call is still
mid-draw. Confirmed structurally: `tick()` unconditionally resets
`g_duskVREyePassOpen = false` at its own very top on every call (part of
its "reset up front" logic) -- a nested call would silently clobber the
OUTER, still-in-progress call's `true` state, which is exactly consistent
with every "eyePassOpen=0" reading logged even while clearly mid-scene-draw
(per interleaved `[dusk::gxtex304]` eye=0 tags in the same window).

**Fix applied** (`vr_main.cpp`): a `TickReentrancyGuard` RAII struct wraps
`tick()`'s entire body -- a static `bool s_tickInProgress` flag, set true
on entry and reset false on exit via the guard's destructor (RAII rather
than a plain flag + manual reset at every return point, since `tick()` has
many early-return paths -- no session, XR call failures,
`shouldRender==false`, view not ready, etc. -- and a plain flag would be
easy to leave "stuck" true if one of those paths were missed). If a call
arrives while `s_tickInProgress` is already true, it logs
`[dusk::vr::tick] RE-ENTRANT CALL #N DETECTED -- skipping` and returns
immediately, touching nothing else -- protecting the outer call's
in-progress state instead of corrupting it. **This fixes the SYMPTOM
(shared frame-pacing state getting clobbered mid-draw) regardless of what
triggers the nested call** -- the underlying nested-Windows-message-pump
mechanism itself (if that's really what it is) is NOT identified or fixed
here; if the reentrancy count logged turns out to be high enough to matter
for other reasons (e.g. perf, or other shared state this project hasn't
noticed being corrupted the same way), that's a separate follow-up.

**Built successfully** (RelWithDebInfo) -- only `vr_main.cpp` needed
recompiling.

**NOT yet tested in-headset.** Next step for whoever picks this up: launch
in VR, move around, and check (a) whether hand/body lag is actually gone
now, and (b) grep the Output window for `[dusk::vr::tick] RE-ENTRANT CALL`
to confirm the theory directly -- if it fires at all, that's confirmation
re-entrancy is real and was happening; the FREQUENCY (rare vs. constant)
would also help gauge how big a deal the underlying nested-call trigger
is beyond just this one symptom. If lag is STILL present despite
confirmed re-entrancy blocking, that's evidence this genuinely was (one
of) the root cause(s) but something else also contributes -- don't assume
it's fully explained without checking. All the diagnostic scaffolding from
this investigation (`[dusk::vr::fpsdiag]`, `[dusk::vr::stepbisect]`/`2`/`3`,
`[dusk::vr::bodyoffsetdiag]`, `[dusk::vr::bodyoffset]`,
`[dusk::frameinterp::beginframe]`) is still in the tree -- remove once
this is confirmed fixed, per this project's normal practice.

**ACTUALLY ROOT-CAUSED 2026-08-09 — via a real debugger call stack, after
log-based bisection hit a wall the reentrancy theory couldn't explain.**
The reentrancy guard added the previous round showed ZERO violations in a
real capture, yet the corruption still happened -- direct proof that
theory was wrong, not just unconfirmed. Rather than propose a fourth
theory from log inference alone, walked the user through Visual Studio:
first a hit-count breakpoint in `begin_frame()` (came back with a
completely ordinary, single-level call stack from `main01()` -- ruling
out an "extra caller" of `begin_frame()` itself too), then a much more
precisely targeted one -- a conditional breakpoint inside
`daAlink_c::draw()` itself, breaking exactly when
`get_interpolation_step() == 0.0f` is observed (needed a small code
change first: VS's expression evaluator refuses to call functions with
side effects in breakpoint conditions, so the value was hoisted into a
plain local `drawTopStep` first so the condition could reference that
instead).

**The real call stack, captured on the actual moment of corruption**:
```
daAlink_c::draw()
fopAc_Draw()
fpcLf_Draw() / fpcDw_Execute() / dScnPly_Draw() / fpcNd_Draw() / ...
fpcM_Management()
fapGm_Execute()   <-- the sim-tick / game-logic update function
main01()
```

**Root cause**: `daAlink_c::draw()` (and presumably every other actor's
`draw()`) is called from TWO separate places, not one:
1. The real, decoupled, render-rate-independent draw path this whole PC
   port (and VR specifically) relies on --
   `cAPIGph_Painter()`/`fpcM_DrawIterater()`, called explicitly from
   `vr_main.cpp`'s `tick()`, once per eye, inside a real `beginEye()`/
   `endEye()` bracket.
2. `fapGm_Execute()` -- the SIM-TICK function, called once per committed
   physics tick (30Hz) from `main01()`'s sim-tick loop. This is a
   GameCube-era leftover: on original hardware, "execute" and "draw" were
   never decoupled (30fps logic == 30fps rendering, no reason to
   separate them), so the base game's own actor-process framework
   (`fpcM_Management`/`fpcDw_*`) has ALWAYS combined an update pass and a
   draw-method-dispatch pass into what's misleadingly just called
   "Execute." This PC port's separate, VR-enabling draw path was added
   ALONGSIDE this legacy behavior, not as a replacement for it -- both
   still run, every frame.

This second, legacy call happens BEFORE the frame's real interpolation
`step` has even been computed for that iteration (mid-sim-tick, with
`step` legitimately, correctly at 0 -- not a bug in `frame_interp` at all,
fully vindicating `game_clock.cpp`'s pacing model, which was investigated
and cleared multiple times this session). The actual bug was in THIS
session's own new code: `applyVrBodyPositionOffset()`'s call site was
guarded on `isRenderingToHeadset()` -- which, per section 8's own
already-documented lesson about this EXACT flag ("reads like a
we-are-currently-rendering-an-eye flag but is actually scoped to the
whole VR frame"), is `true` during this legacy call too, since it's set
once per `tick()` call and this legacy call happens to run while a VR
session is active. Since the fix ADDS to `mpLinkModel`'s base transform
translation (`+=`) rather than setting it outright, every legacy-pass
call permanently, additively corrupted the shared model state before the
REAL per-eye draws for that same logical frame ever ran -- compounding
once per sim tick, forever, from the moment this fix was first added.
**This is the exact same bug CLASS already root-caused once before in
this project (section 8, the minimap black-screen bug)** -- broad
`isRenderingToHeadset()` vs. narrow `isEyePassOpen()` -- just hitting a
different call site. Worth remembering as a standing lesson: ANY new
per-eye VR draw-time override added to this codebase should default to
`isEyePassOpen()`, not `isRenderingToHeadset()`, unless there's a
specific reason it also needs to fire outside a real eye pass.

**Fix** (`d_a_alink.cpp`): split `applyVrBodyPositionOffset()`'s call out
from `applyTrackedItemMtx()`'s existing `isRenderingToHeadset()`-guarded
block into its own, separately guarded on `isEyePassOpen()` instead.
`applyTrackedItemMtx()` (and, by extension, the hand-tracking/late-latch
code sharing the same `isRenderingToHeadset()` pattern elsewhere) was
deliberately left untouched -- it's idempotent (re-writes the same cached
matrix values each call, so the legacy pass invoking it too is wasteful
but harmless) and already confirmed working in-headset; touching it
without a demonstrated bug isn't warranted, per this project's own
standing "don't infer a nearby fix supersedes something without
evidence" lesson.

**Built successfully** (RelWithDebInfo) -- only `d_a_alink.cpp` needed
recompiling, clean link, no new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up:
launch in VR (no debugger/breakpoints needed this time), move around
normally, and confirm both the whole-body lag AND the original hand-lag
report are actually gone. If confirmed, remove all the diagnostic
scaffolding listed above (per this project's normal practice) --
`[dusk::vr::latelatch]` from the earlier late-latching round too, since
that turned out not to be the answer either. If NOT fully fixed, the
`isEyePassOpen()` fix is still correct and worth keeping regardless (it's
a real, demonstrated bug fix on its own merits), but there may be a
SEPARATE remaining contributor -- don't assume this one fix explains
100% of the original symptom without re-testing specifically.

**"Nope they still alge behjind" -- previous fix confirmed to be doing
NOTHING at all, traced to a real bug in ITSELF this time, which led
straight to the actual, final root cause.** `[dusk::vr::bodyoffset]`
(including its unconditional "reached" line) never appeared ONCE in a
fresh capture after a confirmed clean rebuild+relaunch -- meaning
`isEyePassOpen()` was FALSE at the exact call site the previous fix had
just been moved to, contradicting the (until-now-untested) assumption
that Link's real per-eye VR draw reaches that code region at all.

**Verification, via two more targeted debugger captures (same
conditional-breakpoint technique as before)**: broke once on
`drawTopStep == 0.0f` (the corrupted case) and once on `drawTopStep !=
0.0f` (the "healthy" case) -- **both captures showed the IDENTICAL call
stack**, `daAlink_c::draw() <- fopAc_Draw <- ... <- dScnPly_Draw <- ...
<- fapGm_Execute() <- main01()`. Neither ever showed a call originating
from `cAPIGph_Painter()`/`beginEye()` (the real per-eye VR draw path).
This was the moment the whole investigation actually turned: `isFirstPerson`,
the interpolation math, the reentrancy theory, and even the earlier
"body-position lag" theory had all been built on an UNVERIFIED assumption
-- that `daAlink_c::draw()` gets called again from the real per-eye VR
path at all. It doesn't, in the way anything downstream of it needs.

**ACTUAL ROOT CAUSE**: `m_Do_ext.cpp`'s `mDoExt_modelEntryDL()` -- called
from `daAlink_c::modelDraw()`, which is what actually submits
`mpLinkModel`/`mpLinkHandModel`/`mSwordModel`/`mShieldModel`/
`mHeldItemModel`/`mpWlChainModels`'s geometry -- has an early-return:
```cpp
if (!dusk::frame_interp::is_sim_frame()) {
    i_model->diff();  // lightweight material-only update
    return;            // SKIPS mDoExt_modelDiff() -- the real
                        // matrix/geometry resubmission
}
```
`is_sim_frame()` is only true on the rare render frame that happens to
coincide with an actual committed physics tick (~30Hz) -- false on every
other VR render frame (60-90Hz). **This is a deliberate, CORRECT
optimization on flatscreen**: the frame-interpolation system substitutes
already-interpolated matrices at a lower level
(`dusk::frame_interp::resolve_replacement()`, inside the J3D draw
pipeline itself) for non-sim-tick presentation frames, so skipping the
heavier full resubmission there is intentional (the function's own
comment: "fixes issue #355 where some lights would flicker"). **It is
NOT correct for VR's tracked-hand/sword/body-position overrides**, which
write fresh matrices directly into these models every eye via
`setAnmMtx()`/`setBaseTRMtx()`+`calc()` -- none of which go through
`dusk::frame_interp`'s replacement system at all. Those writes were
computed correctly, every eye, this whole time -- they just almost never
reached the actual rendered output, because the GPU-facing geometry
resubmission that would have picked them up was being skipped on all but
~30 of the ~60-90 VR render frames each second. **This is the real,
complete explanation for every symptom observed across this entire
investigation**: the world (drawn via the ordinary actor-list traversal,
not this special player-draw path) stays smooth at full VR rate, while
Link's own body and tracked hands specifically stair-step at physics-tick
rate -- worse the faster he's actually moving, since that's exactly when
a 33ms-stale pose is most visibly wrong. It also means every earlier fix
in this investigation (extrapolation, late-latching, the body-position
offset) was computing genuinely correct data that mostly never reached
the screen -- not wasted, but not sufficient on its own either.

**Fix** (`d_a_alink.cpp`'s `daAlink_c::modelDraw()`): while
`isEyePassOpen()` is true (a real VR eye pass, not the legacy
`fapGm_Execute()` pass -- same established distinction as every other
fix in this section), call `mDoExt_modelUpdateDL()` instead of
`mDoExt_modelEntryDL()`. `mDoExt_modelUpdateDL()` (same file,
`m_Do_ext.cpp`, defined immediately above `mDoExt_modelEntryDL()`) has no
`is_sim_frame()` gate at all -- unconditionally calls `i_model->calc()` +
`mDoExt_modelDiff()` every time. Scoped to `modelDraw()`'s `param_1==0`
(actually-visible) branch only -- the `isPlayerNoDraw` branch already
skips real geometry submission entirely, nothing to fix there. Since
`modelDraw()` is the single shared entry point for ALL of the models
listed above, this fix covers hands/sword/shield/held-item too, for
free -- not just the body.

**Built successfully** (RelWithDebInfo) -- only `d_a_alink.cpp` needed
recompiling, clean link, no new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up:
launch in VR (no debugger needed this time) and confirm the lag is
actually gone during real movement -- for the body AND hands. If this is
finally it, remove ALL the diagnostic scaffolding this investigation
accumulated across every round (`[dusk::vr::fpsdiag]`,
`[dusk::vr::stepbisect]`/`2`/`3`, `[dusk::vr::bodyoffsetdiag]`,
`[dusk::vr::bodyoffset]`, `[dusk::frameinterp::beginframe]`,
`[dusk::vr::latelatch]`, the `TickReentrancyGuard`'s own diagnostic log --
NOT the guard itself, which is a real, independently-worth-keeping fix
even though it wasn't the answer here) -- per this project's normal
practice. If this ISN'T fully it either, the `mDoExt_modelUpdateDL()`
fix is still correct and worth keeping (a real, demonstrated, and now
directly call-stack-verified bug), but this section has now accumulated
enough false starts that a SPECIFIC, precise description of what still
looks wrong (not just "still laggy") is needed before guessing again --
this project's own standing lesson, learned the hard way, repeatedly, in
this exact section.

**Reusable lesson for this whole saga, worth internalizing before
touching this file's draw-time VR overrides again**: every earlier
theory in this section (extrapolation, late-latching, reentrancy,
body-position offset) was individually well-reasoned AND partially
correct, but NONE of them were verified against a REAL debugger call
stack until very late -- each was built on an assumption about WHERE
in the frame a symptom was occurring, inferred from log timing/ordering
alone. Log-based inference got this investigation 90% of the way there
across many rounds, but the LAST, decisive 10% -- confirming which
literal code path was actually executing -- only came from two
conditional breakpoints and a Call Stack window. If a future VR bug in
this codebase resists log-based bisection for more than 2-3 rounds, reach
for a real debugger call stack sooner rather than continuing to infer.

**ACTUALLY FINALLY RESOLVED 2026-08-09 (separate session from all of the
above) — CONFIRMED FIXED IN-HEADSET, both hands AND sword/shield.** Section
23 (below) reports hands still lagging even after the core-anchor comfort
change and every fix in this section — that remained true until this
session, which found the REAL final root cause via a fresh
`[dusk::vr::eyepasscheck]` full-session log capture (zero "true" hits,
confirming — again, independently — that `daAlink_c::draw()` never runs
during a real VR eye pass) combined with direct code-reading of
`J3DModel.cpp`/`J3DShapeMtx.cpp`/`frame_interpolation.cpp`:

- **The real mechanism**: this PC port computes a model's pose ONCE per sim
  tick (~30Hz, via the legacy `daAlink_c::draw()` path) and REPLAYS it every
  real render frame. `J3DModel::setAnmMtx()`/`calc()` automatically record
  each joint's matrix into `dusk::frame_interp`'s once-per-tick snapshot
  system; the actual GX matrix load at real per-eye draw time
  (`J3DShapeMtx.cpp`'s `J3DFrameInterpConcat` → `resolve_replacement()`)
  always prefers a value LERP'd between the last two once-per-tick snapshots
  over whatever's in the raw buffer. Every previous "fix" in this section
  (extrapolation, late-latching, reentrancy guard, body-position offset,
  `mDoExt_modelUpdateDL()`) computed genuinely correct data but through a
  call site (`daAlink_c::draw()`/`modelDraw()`) that never runs during real
  rendering — so none of it could ever reach the screen at real framerate.
- **The fix**: `dusk::frame_interp::mark_live_this_frame(key)`
  (`frame_interpolation.h`/`.cpp`) — a small opt-out registry checked at the
  top of `resolve_replacement()`/`lookup_replacement()` — lets specific
  matrix addresses skip the stale-interpolation substitution for the current
  frame. `vr_link_visibility.hpp`'s `refreshTrackedHandDrawMtxLive()`,
  called once per real frame from `vr_main.cpp`'s `tick()` (a genuinely
  real per-eye-relevant call site, unlike the dead `draw()`-based ones),
  writes the tracked hand pose via the existing `applyTrackedHandMtx()` and
  marks `getAnmMtx(RIGHT_HAND_JOINT)`/`getAnmMtx(LEFT_HAND_JOINT)` live.
- **Non-obvious wrinkle, cost a full round to find**: the FIRST version of
  this fix marked `getDrawMtxPtr()` (and even called `viewCalc()` to try to
  force-populate it) — completely wrong buffer. A `[dusk::vr::liverefresh]`
  capture showed `getDrawMtxPtr()` returning the SAME static address
  (`J3DMtxBuffer::sNoUseDrawMtx`, a shared placeholder) for every different
  hand-model instance, frozen at `(0,0,0)`. Root cause: `mpLinkHandModel`'s
  shapes use the "ConcatView" load type
  (`J3DMdlDataFlag_ConcatView`/`J3DMtxBuffer::create()` routes this type to
  `setNoUseDrawMtx()` instead of allocating a real per-model draw-matrix
  array), and `J3DShapeMtxConcatView::load()` for this type reads the matrix
  straight from `getUserAnmMtx()` (aliases `getAnmMtx()`/`mpAnmMtx`
  directly) via an `sMtxPtrTbl[]`/`getDrawMtxFlag()`/`getDrawMtxIndex()`
  redirection — `getDrawMtxPtr()`/`calcDrawMtx()` are never consulted for
  this model's shapes at all. **Lesson for next time a similar fix is
  attempted on a different model**: don't assume `getDrawMtxPtr()` is the
  right buffer without checking the model's actual `J3DMdlDataFlag_*` load
  type first — a quick sentinel-address check (log the pointer + content
  across several different model instances; a frozen, identical address is
  the tell) settles it directly.
- **Sword/shield follow-up, same session, same underlying bug**:
  `applyTrackedItemMtx()` (section 16) had the identical dead-call-site
  problem. Fixed the same way (`refreshTrackedItemMtxLive()`,
  `markModelJointsLive()` — extended to also mark `getWeightAnmMtx()`, since
  sword/shield's shapes turned out to use the WEIGHT-ENVELOPE variant of the
  same `sMtxPtrTbl` redirection, not the plain `getAnmMtx()` one hands use —
  found via the identical sentinel-capture technique), but getting the
  "is this actually drawn vs. sheathed" GATE right took several more wrong
  turns worth recording since they're easy to re-attempt by accident:
  1. **Comparing the model's own base transform against a freshly-re-read
     item-joint matrix** (`mtxNearlyEqual`) — correct once per tick (its
     original, still-in-tree-but-dead call site), but self-defeating once
     called every real frame: after the first correct match-and-overwrite,
     every subsequent real frame within that tick compared the function's
     OWN prior tracked-matrix write against the item-joint matrix, which
     essentially never matches → gate falsely reads false almost always.
  2. **Caching the gate result once per real sim tick** (`sim_tick_seq()`)
     fixed the self-corruption above, but a real `[dusk::vr::itemgate]`
     capture — taken during a CONFIRMED `mEquipItem==0x103` window (verified
     via direct instrumentation of `daAlink_c::setItemMatrix()` itself) —
     showed the comparison still failing, with rotation components differing
     by up to ~1.0 and changing rapidly tick to tick. Root cause: the
     underlying item-joint VALUE genuinely moves between when
     `setItemMatrix()` captures it (during game-logic execute) and when this
     later, real-frame call site re-reads it — a fast swing animation
     visibly progresses in that gap. No amount of caching fixes a
     comparison against a value that's stale by construction.
  3. **"Always track the live hand," dropping the gate entirely** — fixed
     responsiveness but regressed the original section-16 bug: a sheathed
     sword/shield snapped to the tracked hand too.
  4. **Item-joint-to-hand-joint DISTANCE heuristic** — the item joint turned
     out to be a FIXED rig joint sitting ~10 units from the hand joint
     UNCONDITIONALLY, sheathed or not — not a signal of anything.
  5. **What actually worked**: use the game's own REAL, authoritative
     hand-attach flags directly instead of re-deriving them from position
     data. `daAlink_c::checkItemSwordEquip()` (`mEquipItem==0x103`, already
     public) for the sword; a new `daAlink_c::checkShieldHandAttached()`
     (`d_a_alink.cpp`/`.h`) mirroring `setItemMatrix()`'s exact shield
     OR-chain condition for the shield. Fed directly into
     `applyTrackedItemMtxIfAttached()` — no matrix comparison, no caching,
     no heuristic. When not attached, `refreshRestingPoseSmoothed()` (a
     prev/curr-snapshot-and-lerp technique, same shape as
     `getVrCameraEyeAnchor()`) keeps the sheathed/stowed pose smooth instead
     of choppy raw 30Hz steps.
- **Diagnostic scaffolding from this whole investigation — including
  everything this section already listed as still-in-tree
  (`[dusk::vr::fpsdiag]`, `[dusk::vr::stepbisect]`/`2`/`3`,
  `[dusk::vr::bodyoffsetdiag]`, `[dusk::vr::bodyoffset]`,
  `[dusk::frameinterp::beginframe]`, `[dusk::vr::latelatch]`,
  `[dusk::vr::eyepasscheck]`) plus everything added this session
  (`[dusk::vr::liverefresh]`, `[dusk::vr::itemsentinel]`,
  `[dusk::vr::itemgate]`, `[dusk::vr::itemdist]`, `[dusk::vr::itemrefresh]`,
  `[dusk::vr::setitemmtx]`, `[dusk::frameinterp::resolvediag]`) — has been
  removed now that both symptoms are confirmed fixed**, per this project's
  normal practice. The `TickReentrancyGuard` itself (not its diagnostic
  log) and the late-latching re-locate logic in `applyTrackedHandMtx()`
  were left in place — both harmless, and the guard remains a real
  protective mechanism independent of whether it was ever the answer here.
- **User confirmation**: hands — "the hands are no longer lagging. You
  finally fixed it." Sword/shield — after several more rounds on the
  gate specifically (documented above) — "It works now."

This closes out section 20 for real. The reusable lesson from THIS
session, on top of the one already written above: when a "fix" changes
data that's computed correctly but never verified to actually reach the
screen, verify the CONSUMING code path too (what actually reads this
matrix at draw time, and is that read point even reachable from where the
fix runs) — not just that the write itself is correct. Two separate
"final root cause, confirmed" writeups in this same section (the
`isEyePassOpen()` fix, then the reentrancy guard, then the body-position
offset) all turned out to be real, correct, and INSUFFICIENT because
none of them checked whether their own call site actually executes during
real rendering — the same category of gap this final round closed.

### 21. Cutscenes now first-person too, when Link's own body is actually loaded/drawn — built 2026-08-08, NOT yet confirmed in-headset

**Goal** (explicit user request: "I want to make every cutscene that has
link loaded in first person"). Before this, `isFirstPerson()` (section 19)
stayed first-person for ordinary gameplay and plain NPC dialogue, but any
other running event (cutscene, door/transition) still fell back to
third-person unconditionally — reasoning that an authored cutscene camera
isn't guaranteed to be looking at Link at all. Asked the user to clarify
what "has Link loaded" should mean in code terms: either (a) flip to
first-person for literally every event as long as the `daAlink_c` actor
exists, or (b) add a real per-frame check of whether Link's own body is
actually being drawn in that specific shot. **User chose (b)** — the
concern that some cutscenes swap in a stand-in demo actor or park the real
Link off-camera entirely is real, and forcing the VR camera to his head in
those shots would be meaningless/wrong.

**The check**: `daAlink_c::checkPlayerNoDraw()` (`d_a_alink_link.inc`) —
already existed, already used to gate `mpLinkModel`'s own `modelDraw()`
call in `daAlink_c::draw()` (`d_a_alink.cpp`, the `isPlayerNoDraw` local).
Returns true when either a camera-attention "hide player" bit
(`dComIfGp_checkCameraAttentionStatus(field_0x317c, 2)`) or
`FLG0_PLAYER_NO_DRAW` is set — confirmed via grep that
`FLG0_PLAYER_NO_DRAW` is only ever touched from `d_a_alink_demo.inc`
(`onPlayerNoDraw()`/`offPlayerNoDraw()`), never from any ordinary-gameplay
code path — i.e. this really is a demo/cutscene-specific "is the real
Link actor currently the thing being rendered" signal, not something that
could spuriously fire during normal play. This is exactly the "has Link
loaded" check the user asked for, already built into the base game rather
than something new to invent.

**Fix** (`vr_link_visibility.hpp`'s `isFirstPerson()`): the previous
unconditional `return dComIfGp_event_runCheck() && event &&
event->getMode() == dEvt_mode_TALK_e;` for the event-running case is now
staged: still first-person immediately for `TALK` mode (unchanged from
section 19), but for every OTHER event mode (cutscenes, door/transition),
now returns `!link->checkPlayerNoDraw()` instead of an unconditional
`false` — first-person whenever Link's body is actually loaded/drawn for
that shot, third-person fallback only when the base game has explicitly
hidden him (stand-in actor, or a shot not about him). `getVrCameraEyeAnchor()`
needed no changes — it already just calls `isFirstPerson()` and mirrors
whatever it returns.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (it transitively includes this header), clean link, no new
warnings.

**NOT yet tested in-headset** — next step for whoever picks this up:
trigger a few different cutscenes (ideally at least one plain
Link-performing-an-action cutscene, and one where the camera focuses on
an NPC/boss/object instead) and confirm (a) the Link-centric ones are now
genuinely first-person from his own head, and (b) the ones where he's
hidden/off-camera still correctly fall back to third-person rather than
anchoring the camera to a stale or meaningless position. Also worth
re-confirming dialogue (section 19) and Wolf form (section 11) still
behave as before, since this is the same shared `isFirstPerson()` function
both of those already depend on.

**UPDATE 2026-08-08 (same day) — first in-headset test found a real bug:
Epona-riding cutscene camera anchored inside Epona's head, not Link's
neck. Carve-out fix applied and built, NOT yet retested.** User report:
"It kinda worked but ... the camera isnt on links neck but rather phasing
thrugh epona's head."

**Root cause (read from code, not yet confirmed via a real capture/log —
see caveat below)**: `getVrCameraEyeAnchor()`'s anchor comes from
`daAlink_c::getSubjectEyePos()` → `setBodyPartPos()`
(`d_a_alink.cpp`). That function's mount-relative eye offsets
(`horseLocalEyeFromRoot`/`canoeLocalEyeFromRoot`/`boardLocalEyeFromRoot`)
only activate inside one big gated condition built from several
`dComIfGp_checkPlayerStatus0/1(...)` bits — flags that read like
"actively player-controlled riding gameplay is happening right now." A
scripted cutscene demo almost certainly doesn't set those the same way
interactive riding gameplay does (demos drive the actor directly, not
through normal input/status state), so during a horseback CUTSCENE this
condition is plausibly false and `setBodyPartPos()` falls through to its
`else` branch: `field_0x3768 = eyePos` — Link's own bare head-joint
position (`mpLinkModel->getAnmMtx(field_0x30b4=4)` + a small local
offset), with NO mount-relative adjustment applied at all. Section 11
already flagged this exact mount-eye-anchor code path as "expected to
work with zero additional code" but explicitly **never confirmed in
gameplay, let alone cutscenes** — this is that untested gap actually
manifesting, exposed for the first time now that section 21 makes
cutscenes reach `isFirstPerson()`'s true branch at all. Whether Link's
own head joint genuinely resolves to a position inside Epona's head
during this specific demo animation (e.g. if the demo's authored rider
pose parents/positions him differently than gameplay's `current.pos`-based
transform) or something else entirely is going on has NOT been verified
by an actual in-game capture — this is a plausible read of the code, not
a proven root cause, matching this project's own repeated lesson about
not trusting a single code-reading pass without evidence for anything in
this bug class.

**Fix applied (a carve-out, not a fix to the underlying mount-eye-anchor
math itself)**: `isFirstPerson()` now also returns third-person for
cutscenes/door-events specifically when Link is mounted —
`link->checkReinRide() || link->checkCanoeRide() ||
link->checkBoardRide()` — checked right after the `TALK` early-return and
before the `checkPlayerNoDraw()` check, mirroring the existing Wolf-form
carve-out at the top of the same function (same reasoning: a mount's own
rig/eye-anchor math was never confirmed correct, so don't force the
camera into it for a shot that was never designed to be seen from there).
**Deliberately scoped to cutscenes only** — the `!link->checkEventRun()`
branch above this returns `true` (first-person) before this check is ever
reached, so ordinary mounted GAMEPLAY (actually riding Epona around the
world, unchanged since section 11) is completely unaffected; only mounted
CUTSCENES fall back to third-person now.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling, clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Looks good."
Mounted (Epona) cutscenes correctly fall back to third-person now instead
of anchoring inside her head; ordinary mounted gameplay and non-mounted
cutscenes untouched. Closes out this round of section 21 — the underlying
mount-eye-anchor gap (why the plain head-joint fallback lands inside the
mount's own geometry during a demo) is still not root-caused with real
evidence, just carved around, same as Wolf form. Worth fixing properly
(so mounted cutscenes CAN be first-person too) as a separate future
follow-up if it matters — start with `field_0x3768`,
`dComIfGp_checkPlayerStatus0/1(...)`'s actual gate value, and
`checkReinRide()` logged during a real mounted cutscene, not another
code-reading guess.

### 22. Night-sky stars — DISABLED IN VR — built 2026-08-08, NOT yet confirmed in-headset

**Symptom** (user-reported, VR-only per follow-up confirmation): stars
"look wrong" in the headset — asked to disable them.

**Root cause (read from code, same class as sections 5/10's camera-locked
kagerou effects, not yet confirmed via an in-headset capture)**:
`dKyr_drawStar()` (`d_kankyo_rain.cpp`) draws the night-sky star-field
billboards oriented via `MTXInverse(dComIfGd_getView()->viewMtxNoTrans,
camMtx)` — the FLATSCREEN camera's view matrix, not either eye's real
per-eye VR view — and positions the moon/star anchor off
`camera->view.lookat.eye`, the old third-person eye. On flatscreen this is
fine (that camera IS what's rendering). In VR, this is the same
camera-anchored-effect mismatch already root-caused for the sun/heat-wave
kagerou effects: a headset's free head rotation and per-eye stereo
separation aren't represented in that stale flatscreen matrix at all,
which would plausibly read as stars sitting wrong and/or ghosting/
duplicating between eyes — matches "look wrong" reasonably well but
wasn't independently isolated via a real capture the way sections 5/10
were before their fixes landed.

**Fix**: added an `isRenderingToHeadset()` early-return at the top of
`dKyr_drawStar()`, right after the existing `hide_vrbox` guard already in
that function — VR-only, matching this project's usual gate pattern
(flatscreen keeps stars). `d_kankyo_rain.cpp` already includes
`vr_main.hpp` and already uses this exact guard shape one function up
(`dKyr_sun_move()`'s sun-kagerou skip, section 5), so no new plumbing was
needed. The separate shooting-star system (`dKankyo_shstar_Packet`/
`dKyr_shstar_init()`/`dKyr_shstar_move()`) was checked and confirmed to be
dead/unimplemented in this game version (empty function bodies, no
`draw()` override) — not touched, nothing to disable there.

**Built successfully** (RelWithDebInfo) — only `d_kankyo_rain.cpp` needed
recompiling, clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Yup theyre
gone." No regressions to the rest of the sky/moon draw reported. Closes
out this section; the underlying camera-anchored-billboard mismatch was
never independently root-caused with a real capture (see above), only
inferred by analogy to sections 5/10 — worth keeping in mind if this ever
needs revisiting to make stars VR-correct instead of just disabled (e.g.
re-deriving the billboard orientation from each eye's real per-eye view
matrix instead of skipping the draw).

### 23. VR camera anchored to Link's root/core position (+ fixed height offset) instead of his animated head joint — COMFORT change, built 2026-08-09, NOT yet confirmed in-headset

**Not a fix for section 20's lag bug** — a separate, deliberate comfort
change, user-requested: "instead of anchoring it to link's head, [anchor]
the camera to link's core or his position, then rais[e] the camera up to
where his head is[,] eliminat[ing]... motion sickness caused by bobbing,
rolling, getting knocked over, etc." Explicitly flagged to the user before
implementing that this should NOT be expected to resolve section 20 (the
hands/body lag): the hand-relative-to-camera math (`buildHandMtx()`) was
proven earlier in that investigation to be independent of the eye anchor's
own accuracy, and section 20's actual root cause (`mDoExt_modelEntryDL()`'s
`is_sim_frame()` geometry-resubmission gate) is about mesh submission
frequency, unrelated to what world-space point the camera anchors to.

**Implementation** (`vr_link_visibility.hpp`): `getVrCameraEyeAnchor()`
previously fed its prev/curr-snapshot-plus-extrapolation smoothing (see
section 20's extrapolation writeup) from
`*link->getSubjectEyePos()`/`field_0x3768` directly — the animated
head-joint position, which bobs/rolls/lurches with idle sway, footstep
impact, and knockback. Now feeds that same smoothing from a new
`detail::computeRawCoreAnchoredEye(link)`: `current.pos.x`,
`current.pos.z`, and `current.pos.y + s_coreAnchorHeightOffset` — i.e.
Link's root/core position (the same physics-driven value `setMatrix()`
uses to place `mpLinkModel` itself, confirmed public via `f_op_actor.h`'s
`fopAc_ac_c`), raised by a fixed vertical offset. The offset is
**calibrated, not hardcoded**: captured once per first-person activation
as `realEye.y - current.pos.y` (using the real animated eye value only to
derive a sensible height for whatever stance is active — standing,
crouched, swimming — not as the anchor itself), then held fixed until the
next activation, so it doesn't chase animation frame-to-frame. Falls back
to `kCoreAnchorHeightOffsetDefault = 55.75f` (borrowed from
`setBodyPartPos()`'s own `localEyeFromRoot.y`, an existing precedent for
"root+fixed-offset" eye placement used for a different, unrelated
condition in that function) for the handful of frames before the first
real calibration ever runs.

`getVrBodyPositionOffset()` (the section-20 body-lag-compensation function,
itself still gated behind `isEyePassOpen()` and of unconfirmed effect —
see below) was updated to match: it used to diff `getVrCameraEyeAnchor()`'s
smoothed output against a *raw* `getSubjectEyePos()` call to get "how far
the camera has been pushed this frame" — now uses the SAME
`computeRawCoreAnchoredEye()` raw basis instead, factored out specifically
so the two functions can't drift onto two different definitions of "raw
anchor" the way a second hand-copied version would (this file's own
standing lesson — see `vr_smooth_turn.hpp`'s header comment, cited
in-code).

**Deliberate tradeoff, not a bug**: the camera no longer tilts/leans with
Link's own head/torso animation at all — comfort-motivated, exactly as
requested. `isFirstPerson()`-gating, wolf-form/mount/cutscene fallback
behavior, and the extrapolation-for-lag-hiding logic are all unchanged
from section 20/11 — this only changes WHAT world-space point gets fed
into that existing smoothing pipeline, not the pipeline itself.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (pulls in the header), clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET, and a genuine surprise: this also fixed
section 20's body-lag symptom** — user tested and reported "that fixed the
body anchoring problem. The body doesn't lag behind." **Section 20's
`mDoExt_modelUpdateDL()` fix was very likely NOT the actual fix for the
body** (still unconfirmed either way whether it's reachable at all — the
`[dusk::vr::eyepasscheck]` question below is still open) — the far more
likely explanation, given this ordering: the OLD eye anchor extrapolated
`getSubjectEyePos()`, an ANIMATED head-joint value whose per-tick delta
includes running-gait head bob/sway on top of root translation. Bob is
oscillatory — extrapolating (curr−prev) THROUGH a direction reversal
overshoots wildly, every single stride. The body mesh, meanwhile, is built
from plain `current.pos` (root translation only, no bob) once per tick with
zero extrapolation. Comparing an oscillating, over-shot PREDICTION against
a smooth, un-predicted, one-tick-stale root position is a much bigger and
weirder-looking mismatch than comparing two things both ultimately driven
by root translation alone — which is exactly what section 23 changed the
eye anchor to. The residual one-tick positional lag between the
(extrapolated) camera and the (un-extrapolated) body mesh presumably still
exists mathematically, but is apparently small/smooth enough now not to
read as "lag" anymore. Worth remembering if this ever needs revisiting.

**Hands still lag** ("hands still do [lag]" — user's exact words), even
though tracked-hand position is computed fresh every render frame straight
from OpenXR controller poses (`buildHandMtx()`, unaffected by any of the
eye-anchor changes above) added ON TOP of this same eye anchor. Since the
eye anchor itself is now confirmed visually rigid with the body, and
`buildHandMtx()`'s own controller-delta math was proven earlier in this
investigation to be independent of the eye anchor's accuracy, hand lag
looks like a genuinely separate remaining bug — plausibly still
`mDoExt_modelEntryDL()`'s `is_sim_frame()` geometry-resubmission gate
(covers `mpLinkHandModel` too, per that fix's own scope), now isolated as
the clearer next thing to verify. The `[dusk::vr::eyepasscheck]`
diagnostic (top of `daAlink_c::draw()`, logs whether `isEyePassOpen()` is
ever observed true in that call tree) still has not had its log captured —
two prior debugger captures showed `draw()` only ever entered via the
legacy `fapGm_Execute()` path, which would make the `modelDraw()`
`mDoExt_modelUpdateDL()` fix unreachable and therefore a no-op for hands
too. That log capture (or a fresh debugger call stack, this time
specifically watching hand draw/matrix-write timing) is the next concrete
step for the hands specifically, now that the body is cleanly ruled out.

**Follow-up nudge, same session, still user-facing complaint after the
core-anchor fix landed**: "Link hunches forward when hes running and you
can see your neck and back in the way." Added a further fixed offset ON
TOP of the calibrated core anchor — up `kCoreAnchorExtraUpUnits` and
forward `kCoreAnchorExtraForwardUnits` (both 15.24 game units = 6 real
inches, via the established 100-units-per-metre conversion). Forward
direction uses `current.angle.y` (Link's actual BODY-facing yaw — same
field/BAMS convention `d_a_alink.cpp` already uses elsewhere for
forward-offset placement), not the HMD/smooth-turn yaw, since the geometry
being cleared is fixed relative to his body, not to where the player is
looking. Built successfully (`vr_main.cpp` + `d_a_alink.cpp` recompiled,
clean link).

**Tuned same day**: 6in up was too much ("6 was too much my bad") —
brought down to 3in up (`kCoreAnchorExtraUpUnits` = 7.62 units). Forward
left at 6in (`kCoreAnchorExtraForwardUnits` = 15.24 units), no contrary
feedback on that one. **CONFIRMED FIXED IN-HEADSET at these tuned values**
— user tested and reported "Yup thats the right spot." 3in up / 6in
forward is the settled value for this offset; don't re-tune without new
feedback.

User also reconsidered the earlier "hands still lag" report from section
23 above after seeing the forward offset in action: "I think the head
moved forwards so it looked like lag" — i.e. floating a hypothesis that
what read as hand lag may have actually been the head/eye position
sitting forward of the hands' own tracked position, not genuine temporal
lag. **Retested after the 3in/6in tuning above and RULED OUT**: "the hands
still lag" — plain, unambiguous, independent of the forward-offset amount.
Section 20 (hand lag) is NOT closed by any of the camera-anchor work in
this section; it's a real, separate, still-open bug. Next concrete step
whenever this is picked back up: the `[dusk::vr::eyepasscheck]` log
capture (or a fresh debugger call stack targeting hand-matrix-write
timing specifically) — see section 20's own writeup. **Explicitly deferred
by the user to a later session** ("I'm gonna do the hands tomorrow"), not
abandoned.

**Two new, not-yet-investigated gaps surfaced by the same user message**,
both stemming from section 23's core-anchor calibration only having been
reasoned through for standing gameplay:
- **Swimming needs a fix** (user's words: "I do need to fix swimming") —
  likely the calibrated height offset (or the core anchor concept itself)
  doesn't hold up in water. `setBodyPartPos()`'s own conditional branch
  (the one `localEyeFromRoot`/`horseLocalEyeFromRoot`/etc. live in) is
  explicitly gated in part on `!checkNoResetFlg0(FLG0_SWIM_UP)` and a
  `0x08000000` status bit — i.e. the base game ALREADY special-cases
  swimming for its own root-relative eye math, which section 23's
  `computeRawCoreAnchoredEye()` does not currently account for at all (it
  unconditionally uses one calibrated-on-activation offset regardless of
  stance). Nothing implemented yet — needs an in-headset look first to see
  exactly what's wrong (wrong height? wrong forward clearance? something
  else entirely) before guessing a fix.
- **Crawling not yet tested** (user's words: "test crawling") — no known
  issue yet, just unverified. Worth checking whether the core-anchor
  height/forward offsets look right prone/crawling, same open question as
  mounted modes already flagged in section 23.

### Gameplay vs. cutscene anchor split — CONFIRMED FIXED IN-HEADSET 2026-08-09 (further session)

**Goal** (explicit user request): "in gameplay link's head is on his core
and in cutscenes link's head is anchored to the original head anchor."
The core/root-anchor comfort change above (this section) had been applied
unconditionally to every `isFirstPerson()`-true case — ordinary gameplay
AND cutscenes/NPC dialogue alike (section 21 already extended
`isFirstPerson()` to cover most cutscenes where Link's body is drawn, and
section 19 already covers plain dialogue). But the original
motion-sickness complaint that motivated the core anchor was specifically
about running/movement (head bob/roll/knockback during locomotion) — not
cutscenes or conversations, where the ORIGINAL animated head-joint anchor
(`getSubjectEyePos()`, section 11, pre-dating this section) is arguably
more desirable: it actually follows the authored eyeline/animation (a
nod, a look-down, a lean) that a cutscene or conversation is often built
around, instead of a rigid comfort-anchored point that never moves with
it.

**Fix** (`vr_link_visibility.hpp`): added `detail::computeRawEyeAnchor(link)`,
which dispatches on `link->checkEventRun()` — the exact same condition
`isFirstPerson()`'s own first branch (`if (!link->checkEventRun()) return
true;`) already uses to distinguish plain gameplay from everything else —
rather than inventing a second condition:
- No event running (ordinary gameplay): returns
  `computeRawCoreAnchoredEye(link)` — unchanged from the section above.
- An event IS running (cutscene or NPC dialogue, since `isFirstPerson()`
  is already known true by the caller's precondition): returns
  `*link->getSubjectEyePos()` directly — the original, pre-section-23
  animated head-joint position. No core-anchor height calibration or
  hunch-clearance nudge applied in this branch — both exist specifically
  to compensate for the core anchor and for running/movement, neither of
  which is relevant here.

Both `getVrCameraEyeAnchor()` and `getVrBodyPositionOffset()` (which must
agree on the same "raw anchor" definition to compute a correct lag-
compensation delta — see that function's own comment) were updated to
call `computeRawEyeAnchor()` instead of calling
`computeRawCoreAnchoredEye()` directly, so the two can't silently diverge
onto two different definitions of "raw anchor" the same way this file's
other shared-formula lessons (`vr_smooth_turn.hpp`'s header comment,
`isFirstPerson()`'s own factoring-out) already warn against. The existing
prev/curr-snapshot smoothing and `kEyeAnchorExtrapolationGain`
extrapolation pipeline (section 20) is completely unchanged — it's simply
fed a different raw source per branch each frame; extrapolating the raw
head-joint position for cutscenes is not new behavior invented this
session, it's exactly what this same pipeline already did before section
23 introduced the core anchor (section 20's extrapolation fix predates
section 23), so no additional risk was introduced by restoring it for
this branch specifically.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Yup looks
good." Closes out this follow-up; the swimming/crawling gaps immediately
above are still open and unrelated to this change (they're about the
core-anchor branch specifically, which this fix left untouched in its
own logic).

### Swimming camera fix + tracked hands disabled while swimming — built 2026-08-09, NOT yet confirmed in-headset

**Goal** (explicit user request): "fix the camera when swimming, or set
the camera to first person, and disable the hand animation while
swimming."

**Camera fix** (`vr_link_visibility.hpp`'s `computeRawEyeAnchor()`):
acted on the theory already recorded in section 23's swimming gap —
`computeRawCoreAnchoredEye()`'s vertical offset is calibrated ONCE per
first-person activation (`s_coreAnchorCalibrated`) and held fixed until
first-person is left entirely (cutscene/Wolf/etc.); entering or leaving
the water does NOT reset that calibration, so a player who calibrated
standing on land and then goes for a swim keeps a stale, standing-height
offset added on top of `current.pos` — which does not mean the same thing
while swimming. `setBodyPartPos()` (`d_a_alink.cpp`) already special-cases
swimming for its own eye math (its `FLG0_SWIM_UP`/`MODE_SWIMMING`-gated
branch), confirming this really is a case the plain root+fixed-offset
model was never designed to cover. Rather than reverse-engineer and
duplicate that swim-specific math, `computeRawEyeAnchor()` now checks
`link->checkModeFlg(daAlink_c::MODE_SWIMMING)` first and, when true,
returns `*link->getSubjectEyePos()` directly — the same original,
animation-driven head-joint anchor already used for cutscenes/dialogue
(and the pre-section-23 anchor for all gameplay) — instead of the
core-anchored branch. This is deliberately the "set the camera to first
person" half of the user's either/or: falls back to the already-proven
first-person head anchor instead of the newer, swim-unverified comfort
anchor, rather than attempting to derive a swim-specific core-anchor
calibration from scratch. `getVrBodyPositionOffset()` did not need a
separate change — it already calls `computeRawEyeAnchor()` (not
`computeRawCoreAnchoredEye()` directly, per the "one shared raw-anchor
definition" reasoning above), so it automatically stays consistent with
whatever `getVrCameraEyeAnchor()` used this frame.

**Hands disabled while swimming** (`vr_link_visibility.hpp`'s
`refreshTrackedHandDrawMtxLive()`): added an early return when
`link->checkModeFlg(daAlink_c::MODE_SWIMMING)` is true, skipping BOTH the
tracked-pose override (`applyTrackedHandMtx()`) and the
`mark_live_this_frame()` calls together — marking the joints live without
also overriding their pose would just make the swim-stroke animation
render raw/un-interpolated instead of smoothly blended, the opposite of
what's wanted. With the override skipped, `mpLinkHandModel`'s hand joints
are left holding whatever the body's own once-per-sim-tick swim-stroke
sync last wrote (the pre-existing "Always set these" resync in
`d_a_alink.cpp`), and since nothing marks them live this frame,
`frame_interp`'s normal once-per-tick interpolation applies to them —
i.e. the swim-stroke animation plays through smoothly, exactly as it
already does on flatscreen, instead of the player's tracked controller
pose fighting with it.

**Both changes are scoped to `MODE_SWIMMING` specifically** — checked via
the same public `checkModeFlg()` accessor `d_a_alink.cpp` itself uses
internally (confirmed public: `daAlink_c`'s `MODE_FLG` enum and
`checkModeFlg()` both sit in one continuous `public:` block spanning
lines 232–1414 of `d_a_alink.h`, which already covers other methods this
file calls directly on `daAlink_c*`, e.g. `checkWolf()`/`checkEventRun()`).
Sword/shield tracking (`refreshTrackedItemMtxLive()`) and crawling were
NOT touched — out of scope for this request; crawling remains an open,
untested gap per section 23.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**IN-HEADSET RESULT: camera fix CONFIRMED WORKING. Hand-disable fix did
NOT work.** User: "The camera is fixed but not the hands," then
explicitly deferred further investigation ("Honestly its ok ill do it
another time" — paused, not abandoned). Not yet root-caused why
`refreshTrackedHandDrawMtxLive()`'s `MODE_SWIMMING` early-return had no
visible effect on the hands specifically — two untested hypotheses, ranked
by how easy they are to rule in/out first: (a) `MODE_SWIMMING` may not
actually be the flag set for however the user was swimming — the base
game's own `setBodyPartPos()` gates its OWN swim-eye branch on
`FLG0_SWIM_UP` + `checkModeFlg(0x40000)` together, not `MODE_SWIMMING`
alone, so the two may not coincide the way this fix assumed (this would
also cast a little doubt on why the CAMERA half worked, unless
`MODE_SWIMMING` and the base game's own swim-eye gate happen to overlap
for the common case but diverge at the edges — worth checking directly
with a log rather than assuming); (b) something else may still be
writing/marking the hand joints live this frame despite the early return
(e.g. `applyTrackedItemMtx`'s still-present, nominally-dead
`d_a_alink.cpp` call site, or an untraced second tracked-hand write path
not audited this session). Needs a real in-headset log capture (confirm
which mode/flag is actually true while the symptom is visible, and
whether `refreshTrackedHandDrawMtxLive()`'s early return is actually being
hit) before guessing again — same discipline section 20's saga eventually
had to fall back on after enough blind attempts.

### Crawling — same camera + hand-tracking fix applied 2026-08-09, NOT yet confirmed in-headset

**Goal** (explicit user follow-up: "I need you to do the same fix with
crawling"). Crawling has no single `MODE_FLG` bit the way swimming has
`MODE_SWIMMING` — it's a sequence of dedicated `daAlink_PROC` states
instead (`PROC_CRAWL_START`/`_MOVE`/`_AUTO_MOVE`/`_END`, `d_a_alink.h`)
that `mProcID` walks through while crawling through a tunnel/gap
(confirmed via their existing use in `d_a_alink.cpp`, e.g. the
`PROC_CRAWL_END` exclusion around line 11556). Added a shared
`vr_link::isCrawling(daAlink_c*)` helper (a `switch` over `mProcID`
against all four crawl states) rather than duplicating a four-way OR at
each call site — same "one shared definition" reasoning as
`isFirstPerson()`. `mProcID` and the `PROC_CRAWL_*` enum values are public
(confirmed: `daAlink_c`'s body has no top-level `private:`/`protected:`
specifier between its opening `public:` and well past both declarations).

Both of swimming's fixes were extended with `|| isCrawling(link)`:
- `computeRawEyeAnchor()` (`vr_link_visibility.hpp`, `detail` namespace):
  now falls back to the raw head-joint anchor (`getSubjectEyePos()`)
  while crawling too, same reasoning as swimming — the core anchor's
  height calibration is captured once while standing and doesn't get
  invalidated by dropping into a crawl.
- `refreshTrackedHandDrawMtxLive()`: now also skips the tracked-hand
  override (and its `mark_live_this_frame()` calls) while crawling, same
  reasoning as swimming — let the body's own once-per-sim-tick crawl-pose
  hand animation play through normal interpolation instead of fighting
  with controller tracking.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling, clean link, no new warnings.

**IN-HEADSET RESULT: hand-disable fix did NOT work here either** — user:
"Hands are still locked in an animation but its fine for now" (explicitly
deferred, not asking for another attempt right now). Camera half not
separately called out this time — not confirmed either way for crawling,
unlike swimming's camera half which WAS separately confirmed working.

**New data point, not yet acted on**: the phrasing this time — "locked in
an animation" — is more specific than swimming's plain "still lag[ging]"
report and may describe a different symptom: not "hands still track the
controller" (the swimming report's apparent meaning) but hands frozen on
a single animation frame/pose, not moving at all. Both fixes (swimming
and crawling) share the exact same code shape (an early return in
`refreshTrackedHandDrawMtxLive()` before the override write), so this is
a genuine, useful data point in favor of hypothesis (b) from the swimming
writeup above — something other than this early-return is determining
what the hands actually show, since two independent state checks
(`MODE_SWIMMING` vs. `isCrawling()`'s `mProcID` switch) are both failing
to produce the intended "normal body animation plays instead" result, and
now with a more specific "frozen" symptom rather than "still tracked."
Possible next angle if revisited: check whether skipping the override
actually leaves the joints at whatever the base game's OWN per-frame
hand-joint resync last wrote (the assumption both fixes relied on), or
whether something about NOT calling `mark_live_this_frame()` while the
override is also skipped causes `frame_interp` to freeze on a stale
once-per-tick snapshot instead of interpolating normally — this second
possibility would directly explain "locked in an animation" (frozen) as
opposed to "still tracking the controller." Not investigated — user
explicitly deferred.

### Vine climbing — camera falls back to first-person (head-joint anchor) — built 2026-08-10, NOT yet confirmed in-headset

**Goal** (explicit user request: "make climbing vines specifically first
person"). Same underlying gap as swimming/crawling — the core/root
comfort anchor's height calibration (section 23) is only reasoned through
for standing gameplay and doesn't hold up in a stance this different.
Deliberately scoped narrower than "all climbing": Twilight Princess has a
real, dedicated `MODE_VINE_CLIMB = 0x10000` mode bit (`d_a_alink.h`,
comment "used for vine climbing"), genuinely distinct from the general
`MODE_CLIMB` bit used for ladders and weak-wall climbing — confirmed by
grepping `d_a_alink.cpp`'s real usage (e.g. line ~10323's
`checkModeFlg(MODE_SWIMMING | MODE_ROPE_WALK | MODE_VINE_CLIMB |
MODE_UNK_800 | MODE_RIDING | MODE_NO_COLLISION | MODE_CLIMB | MODE_JUMP)`
lists them as separate bits in the same mask, not aliases). So this fix
intentionally does NOT touch ladder/weak-wall climbing — only vines.

**Fix** (`vr_link_visibility.hpp`'s `computeRawEyeAnchor()`): extended the
existing `MODE_SWIMMING || isCrawling(link)` fallback condition to also
include `MODE_VINE_CLIMB` (via `checkModeFlg(MODE_SWIMMING |
MODE_VINE_CLIMB)`, matching the existing OR-mask calling convention
already used elsewhere in `d_a_alink.cpp`) — falls back to the original,
animation-driven head-joint anchor (`getSubjectEyePos()`) instead of the
core-anchored branch while vine-climbing, same reasoning as swimming/
crawling. **Camera-only, deliberately** — did NOT extend the
tracked-hand-disable half (`refreshTrackedHandDrawMtxLive()`) the way
swimming/crawling both got, since that fix is CONFIRMED FAILED for both
of the other two modes (swimming: "still lag[ging]"; crawling: hands
"locked in an animation") and the user only asked for the camera this
time — no reason to carry over a proven-ineffective fix speculatively.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**NOT yet tested in-headset.**

### Cloud shadows + swim-ripple screen-space "reflection" disabled in VR — built 2026-08-09, NOT yet confirmed in-headset

**Goal** (explicit user request): "disable 2 effects: the cloud shadows on
the ground and the screen space reflections from the trailer that link
leaves behind in water." Asked the user to confirm scope first (VR-only,
matching this project's usual pattern, vs. removing both effects
everywhere like the Goron Mines heat-wave one-off) — **user chose VR-only**.

**Cloud shadows** (`d_kankyo_rain.cpp`'s `drawCloudShadow()`, called from
`d_kankyo_wether.cpp`'s `dKankyo_cloud_Packet::draw()`): same class of bug
as the already-fixed night-sky stars (section 22) and sun/heat-wave
kagerou effects (sections 5/10) — orients its shadow quads via
`MTXInverse(dComIfGd_getView()->viewMtxNoTrans, camMtx)`, the FLATSCREEN
camera's view matrix, not either eye's real per-eye VR view. Fixed the
same way as stars: an `isRenderingToHeadset()` early-return at the top of
the function (this file already includes `vr_main.hpp` and uses this
exact pattern for `dKyr_drawStar()`/`dKyr_sun_move()`), skipping the whole
draw in VR. Flatscreen unchanged.

**Swim-ripple screen-space reflection** (`d_particle.cpp`'s
`dPa_control_c::setWaterRipple()`, called from `d_a_alink.cpp` while
swimming, `d_a_canoe.cpp` for paddle/hull ripples, and `d_a_demo00.cpp`/
`d_particle_copoly.cpp`): this function spawns TWO particles per ripple —
`ID_ZI_J_HAMON_A` and `ID_ZI_J_HAMON_IND` (0x1B2/0x1B3,
`d_particle_name.h`). The `_IND`/`_A` naming pattern repeats consistently
across dozens of other "hamon" (ripple) effects throughout this particle
table (e.g. `ID_ZI_S_FM_HAMON_A`/`_IND`, `ID_ZI_S_BQ_APPHAMON_A`/`_IND`) —
`_IND` is the GX indirect-texture-mapped half that actually samples/
distorts the screen (a real screen-space reflection technique, distinct
from this codebase's separate "dummy"-texture-swap mechanism from section
5, but the same underlying VR problem class: a screen-relative sample
that reads wrong per-eye/per-frame data in a headset), while `_A` is the
plain ripple-ring shape/alpha with no screen sampling. Fixed by skipping
just the `_IND` spawn call while `g_duskVRRenderingToHeadset` is true
(this file already forward-declares that extern for its existing
diagnostic-logging code), leaving `*param_0`'s tracked emitter id at 0
(this codebase's existing "no emitter" convention, used the same way by
every other `dComIfGp_particle_setPolyColor` caller) rather than ever
writing a real id into it — the ordinary `_A` ripple-ring keeps spawning
normally. Kept the whole `_A`/`_IND` PAIR loop structure intact rather
than special-casing the array — only that one iteration is skipped.

**Built successfully** (RelWithDebInfo) — `d_kankyo_rain.cpp` and
`d_particle.cpp` recompiled, clean link, no new warnings.

**IN-HEADSET RESULT: swim-ripple SSR fix CONFIRMED WORKING ("Water
particles are gone"). Cloud shadows are NOT fixed** — user: "cloud
shadows are still there. I see them in ordon village, and when I turn my
head left they move right and vice versa. They rotate with the games
flatscreen camera." User wants these actually FIXED (kept, oriented
correctly), not disabled — reversing the earlier "disable" framing now
that it's clear `drawCloudShadow()` wasn't the effect actually being
seen.

**Investigation (2026-08-09, same session) — `drawCloudShadow()`
(billboard `CLOUD_EFF` packet) is very likely NOT what the user is
seeing.** Its early-return is correctly placed (verified by re-reading the
built code) and should reliably fire whenever `isRenderingToHeadset()` is
true, so if the user still sees camera-locked shadows, this specific
billboard system is probably not the actual mechanism drawing them — a
second, real ground-DECAL system was found instead:
`d_kankyo.cpp`'s `dKy_cloudshadow_scroll()`, called from
`dScnKy_env_light_c::setLightTevColorType_MAJI()` whenever a material's
`tevstr_p->Type & 0x20` bit is set. It scrolls a `TexMtx(1)` SRT
translation on ground materials named `MA00`/`MA01`/`MA16` (real terrain
material naming, unlike `CLOUD_EFF`'s standalone particle-style packet) —
a much better match for "shadows on the ground, widespread in Ordon
Village."

**Traced the underlying camera dependency as far as static reading
allows, but couldn't conclusively confirm the root cause**: `dKy_cloudshadow_scroll()`
itself only touches a translation offset — no camera math at all. The
actual rotation-with-camera behavior, if this is really the right
function, would have to come from this material's `TexMtx(1)`
**mapping mode** (`J3DTexMtxMode_Envmap`/`Projmap`/`ViewProjmap`, baked
into the level's `.bmd` data, not visible from this C++ source) — traced
into `J3DMatBlock.cpp`, which shows ALL of those modes correctly read
`j3dSys.getViewMtx()`, the SAME per-eye view matrix normal (correctly
stereo) geometry already uses. That's a real finding, but it means this
mapping SHOULD already be VR-correct with zero extra code by the same
reasoning that normal terrain draws correctly — which contradicts the
observed bug, so either (a) this material uses a different, non-camera
mapping mode after all and something else entirely is responsible, or (b)
`j3dSys.getViewMtx()` genuinely isn't what's active at the moment this
particular material's `calc()` runs (a timing/call-path issue, the same
bug CLASS as several other VR bugs in this file — e.g. section 20's
`daAlink_c::draw()` never running during a real eye pass — just not yet
confirmed for this specific call site). A parallel hypothesis (a
GameCube-era "already computed this frame" cache flag, `j3dSys.checkFlag(4)`/
`checkFlag(8)`, silently reusing eye 0's matrix for eye 1) was
investigated and RULED OUT by reading `J3DModel.cpp` — those flags
actually gate CPU-skinning model-matrix concatenation, unrelated to
per-eye caching.

**Diagnostic logging added instead of guessing a fix blind** (this
project's own established practice — see the Build Workflow notes on the
`OutputDebugStringA`-log-and-test loop): `d_kankyo_rain.cpp`'s
`drawCloudShadow()` now logs once (`[dusk::cloudshadow] drawCloudShadow
entered in VR, mCount=...`) right before its existing early-return, so we
can confirm/rule out whether it's ever reached at all while the user sees
the bug. `d_kankyo.cpp`'s `dKy_cloudshadow_scroll()` now logs once when
entered in VR (`matNum=...`), and once per matched ground material
(`[dusk::cloudshadow] matched material name=... mProjection=... mInfo=...`)
— `mProjection` directly reveals which `J3DTexMtxMode` this material
actually uses (0 = Basic/no camera dependency at all, contradicting the
whole theory; a higher value = one of the Envmap/Projmap family, meaning
the bug is a timing/call-path issue rather than a wrong-formula issue).
Needed a `#include <windows.h>` added to `d_kankyo_rain.cpp` (not
previously needed there) for `OutputDebugStringA` to resolve — caught by
the build failing immediately, fixed same round.

**Built successfully** (RelWithDebInfo) — `d_kankyo.cpp` and
`d_kankyo_rain.cpp` recompiled, clean link, no new warnings.

**NOT tested in-headset before the direction changed — see below.** User
proposed a different, much broader idea instead of chasing this one
capture at a time: sync the shared flatscreen camera's own ROTATION to
the headset every frame, since every one of these camera-anchored bugs
(stars, sun/heat-wave kagerou, now cloud shadows) all read the SAME
`dComIfGd_getView()` object for orientation — fixing that one shared
object fixes the whole bug class at once, including undiscovered
instances, instead of patching each read site individually. Explicit
tradeoff flagged and accepted by the user: this same object's rotation
also feeds the audio camera and potentially other gameplay systems
(aiming/targeting) that read it for legitimate reasons, not just visual
effects — those would now also respond to head rotation, previously
inert in VR. User's response: "I think it is a good side effect, as many
times I'd be playing and think I wish the camera was matching my
headset."

### Flatscreen camera synced to headset direction — built 2026-08-09, NOT yet confirmed in-headset (supersedes the cloud-shadow-specific investigation above)

**What changed**: `d_camera.cpp`'s `camera_draw()` (the function that
computes `view_class`'s `viewMtx`/`viewMtxNoTrans`/`projViewMtx` from
`lookat.eye/center/up` every frame, feeds `j3dSys.setViewMtx()` for
env-mapped materials, AND sets up `Z2GetAudience()`'s audio camera — all
downstream of the exact same `mDoMtx_lookAt()` call) now overrides
`process->view.lookat.center`/`.up` while `isRenderingToHeadset()`, right
before that computation runs, using a new `dusk::vr::getHeadWorldForward()`
accessor (`vr_main.hpp`/`.cpp` — a `cXyz`, the same real-time undamped
head direction `getHeadMoveAngleS()` already flattens to an s16 for
movement, now also exposed as a raw 3D vector including pitch, cached
once per frame in `tick()` alongside it — one `computeHeadWorldForward()`
call, two consumers). `lookat.eye` (the camera's POSITION) is
deliberately left completely alone — the base game's own third-person
follow-camera AI (distance, height, collision) keeps working exactly as
before; only which direction it's looking changes. `lookat.up` is pinned
to world-up (not head roll) — matches how the real per-eye VR render
already handles roll correctly on its own, and avoids introducing camera
roll into whatever else reads this shared object.

**Why this should fix cloud shadows (and the stars/sun-kagerou bugs
retroactively, if ever re-enabled)**: all of them read this same object's
rotation (`viewMtxNoTrans` directly, or indirectly via `j3dSys.getViewMtx()`
for env-mapped materials like the cloud-shadow ground decal). Once that
object's rotation actually matches the headset every frame, any code
reading it gets a head-correct answer for free, with no further per-effect
patching needed.

**Everything from the previous round left in place, not removed** (per
explicit user instruction, "don't remove the logging until the bug is
fixed"): the `[dusk::cloudshadow]` diagnostic logging in
`drawCloudShadow()`/`dKy_cloudshadow_scroll()` is still in the tree — if
this broader fix doesn't fully resolve cloud shadows, that logging is
still the next lead to pull (e.g. it would help confirm whether
`dKy_cloudshadow_scroll()`'s material actually uses one of the
camera-dependent `J3DTexMtxMode`s at all, still unconfirmed). The earlier
`drawCloudShadow()` VR-disable (the billboard `CLOUD_EFF` packet) is also
still in place, untouched — unrelated to this fix, no reason to revert it
without evidence it was ever the right target.

**Built successfully** (RelWithDebInfo) — this changed `vr_main.hpp` (a
widely-included header), so it triggered a full rebuild (1185 objects);
completed cleanly, no errors, no new warnings.

**IN-HEADSET RESULT: user didn't notice a clear benefit either way, but
wants to keep the change regardless — followed by a direct "could this
regress anything?" question, answered with a real regression review
(below) rather than just reassurance.**

**Regression review (2026-08-09, same session) — one real, concrete risk
identified and mitigated; one residual, unaudited risk named honestly,
not fixed.**

1. **Cutscene/dialogue authored-camera hijacking (real, mitigated)**:
   `camera_draw()` runs EVERY VR frame unconditionally — including
   cutscenes and door/transition events, where the flatscreen camera is
   doing something AUTHORED (a scripted pan, deliberate framing) that
   audio panning and env-mapped VFX orientation are presumably meant to
   track. The original version of this fix had no gate at all, meaning a
   cutscene's audio mix / lighting-effect orientation would silently
   follow wherever the player's head happened to be pointed instead of
   the actual shot — desyncing those systems from what's on screen, even
   though the real per-eye VR RENDER itself was never affected (cutscenes
   don't read this object's rotation for the actual image — see
   `getVrCameraEyeAnchor()`'s own fallback handling). **Fixed same
   session**: gated the whole override on a new `dusk::vr::
   isFirstPersonView()` (`vr_main.hpp`/`.cpp`, thin forward to
   `vr_link::isFirstPerson()`) — now only applies during ordinary
   first-person gameplay and plain NPC dialogue, the same scope every
   other first-person-only VR fix in this project already uses. Cutscenes
   keep their authored camera direction for audio/VFX; normal gameplay
   still gets the requested head-sync effect.
2. **Unaudited gameplay logic reading `lookat.center`/`up` as literal
   values during ordinary FIRST-PERSON gameplay (residual, NOT fixed,
   named honestly rather than hidden)**: even after the gate above, any
   system that reads this shared camera's `center` as an actual WORLD
   POSITION (not just a direction) — e.g. a reverb/audio-focus-distance
   calculation, a "what is the camera looking at" query used by aiming or
   auto-target-candidate selection — would now see a point ~1m in front
   of the player's eye instead of whatever the base game normally puts
   there (often much farther away, e.g. roughly toward Link/a target).
   Similarly, `up` is now always pinned to world-up, discarding any
   INTENTIONAL camera roll during special camera modes (if any exist)
   whose downstream consumers expected real roll data. **Not fixed —
   this needs either a full audit of every `dComIfGd_getView()` reader in
   the codebase (large, not attempted) or empirical in-headset testing of
   specific systems (aiming, lock-on, reverb-heavy areas) to catch
   anything that actually breaks.** If something camera-relative feels
   wrong during normal gameplay specifically (not cutscenes — those are
   now excluded), START HERE.

**Built successfully** (RelWithDebInfo) — `d_camera.cpp`, `vr_main.hpp`,
`vr_main.cpp` recompiled, clean link, no new warnings.

**REVERTED 2026-08-09 (same session), per explicit user request ("Yeah
maybe remove it") after the regression review above.** No benefit had
been noticed in-headset, and given the residual, unaudited risk (#2
above) couldn't be fully ruled out, the user chose to drop the feature
entirely rather than keep carrying that uncertainty. Fully removed, not
just disabled behind a flag: the `camera_draw()` override block
(`d_camera.cpp`, including its now-unneeded `#include "dusk/vr/vr_main.hpp"`),
`dusk::vr::getHeadWorldForward()`/`isFirstPersonView()` and their
declarations (`vr_main.hpp`), and `g_headWorldForward`/its per-frame
computation (`vr_main.cpp`) — all deleted outright rather than left
commented out or gated off, since this project's own convention is clean
removal once something's confirmed not worth keeping, not dead scaffolding.
`getHeadMoveAngleS()`/`g_headMoveAngleS` (movement-direction fix, section
17, unrelated and still working) were NOT touched — only the parts added
specifically for this camera-sync experiment came out.

**Built successfully** (RelWithDebInfo) — `d_camera.cpp`, `vr_main.hpp`,
`vr_main.cpp` recompiled, clean link, no new warnings. Confirms a clean
revert (no leftover dangling references).

**State this leaves things in**: back to the one-effect-at-a-time approach
if this is ever revisited. Water-ripple SSR disable and the billboard
cloud-shadow disable (both confirmed/left in from earlier this session)
are untouched by this revert.

### Ground-decal cloud shadows disabled in VR — built 2026-08-09, NOT yet confirmed in-headset (closes out this whole investigation)

**Goal** (explicit user request, after the camera-sync revert: "I need to
actually disable the cloud shadows now"): stop trying to fix the
orientation properly (needs a full TEV/blend understanding this
investigation never reached) — just remove them from VR, matching the
original request pattern before the "fix them properly" detour.

**Fix**: `d_kankyo.cpp`'s `dScnKy_env_light_c::setLightTevColorType_MAJI()`
dispatches per-material lighting to one of two branches based on
`tevstr_p->Type & 0x20` — the cloud-shadow branch
(`dKy_cloudshadow_scroll()`, the ground-decal mechanism identified in the
earlier investigation) or the ordinary per-material lighting loop
(`setLightTevColorType_MAJI_sub()`, called for every other material in
the game). Added `&& !isRenderingToHeadset()` to the condition that
selects the cloud-shadow branch — in VR, these materials now always fall
through to the SAME ordinary lighting path every other material uses
(which `dKy_cloudshadow_scroll()` itself also calls internally, so normal
ambient lighting for this terrain is unaffected — only the cloud-shadow-
specific TevKColor tweak and TexMtx(1) scroll are skipped). Chosen over
trying to hand-tune this material's TEV/alpha state directly (would
require reverse-engineering a blend setup this investigation never fully
understood) — same "disable the camera-relative half, don't guess at the
TEV math" reasoning already used for the billboard cloud-shadow packet and
the swim-ripple screen-space reflection earlier this session. Flatscreen
completely unchanged.

**Diagnostic logging left in place, not removed**: the `[dusk::cloudshadow]`
`OutputDebugStringA` logging inside `dKy_cloudshadow_scroll()` (both the
entry log and the per-material `mProjection`/`mInfo` dump) is untouched —
it simply never fires anymore in VR now that the function isn't called
there. Per the user's standing "don't remove the logging until the bug is
fixed" instruction — this is a disable/workaround, not a root-cause fix,
so the logging may still be useful if the orientation is ever properly
investigated later.

**Built successfully** (RelWithDebInfo) — only `d_kankyo.cpp` recompiled,
clean link, no new warnings.

**IN-HEADSET RESULT: still not fixed.** User captured a real log
(`C:\Users\joeyw\Downloads\log.txt`) and confirmed: zero
`[dusk::cloudshadow]` lines anywhere in the whole session, despite the
capture clearly showing a real, actively-rendering VR session
(`[dusk::vr::tick] reason -> rendering-normally`, eye buffer dumps,
etc.). **This rules out BOTH candidate mechanisms investigated so far**
(`drawCloudShadow()`'s billboard packet AND `dKy_cloudshadow_scroll()`'s
ground-material texture scroll) — neither was ever entered during this
session. Whatever the user is seeing is a third, still-unidentified thing
— the whole "cloud shadow" framing may have been wrong from the start.

### ACTUAL ROOT CAUSE FOUND: not clouds at all — `dDlst_shadowReal_c`, a real object-shadow system, was never actually disabled in VR — FIXED 2026-08-09

**The real finding**: searching the same log for anything shadow-related
(not just cloud-specific) turned up pre-existing, previously-undiscussed
diagnostic logging already in the tree — `[dusk::shadow]`/
`[dusk::realshadow]` (`d_drawlist.cpp`) — firing repeatedly with `VR=1`
throughout the whole session, dumping real per-object shadow projection
matrices (`mVolumeMtx`, `mMtx`, `finalRecvProj`) for many different world
positions. This is `dDlst_shadowReal_c` — a SEPARATE shadow subsystem
from `dDlst_shadowSimple_c` (the stencil-volume shadow CLAUDE.md's
"shadows disabled in VR" permanent constraint already covers) — a
projected-texture "blob" shadow cast by buildings/NPCs/props onto the
ground, using `GXLoadTexMtxImm`/`mShadowRealPoly.draw()` to blend a soft
shadow decal.

**Confirmed via direct code reading, not just the log**:
`dDlst_shadowReal_c::draw()` (`d_drawlist.cpp`) had NO
`g_duskVRRenderingToHeadset` early-return at all — only defensive
frame-interp guards further down (`have_view_mtx`/`have_recv_proj_mtx`,
forced false in VR, falling back to `mReceiverProjMtx`/`mViewMtx`
instead) — meaning `mShadowRealPoly.draw()`, the actual GPU blend of the
shadow decal onto the ground, ran completely UNCONDITIONALLY in VR the
whole time. This directly contradicts CLAUDE.md's "shadows are
intentionally disabled in VR" note — that note only ever covered
`dDlst_shadowSimple_c`; `dDlst_shadowReal_c` was apparently missed
entirely when that disable was originally applied, and nobody had
independently verified whether IT looked correct in VR since — every
prior session's "shadow stretching" investigation (section 1) was about
`dDlst_shadowSimple_c`/`dDlst_shadowReal_c::setShadowRealMtx()`'s
PRECISION (matrix math correctness at large world coordinates), not about
whether the Real variant's actual draw call was gated for VR at all.

This fully explains the user's symptom: soft shadow blobs cast by
Ordon Village's buildings/NPCs/fences onto the ground, rendered using a
camera/view-relative projection matrix that was never corrected for VR's
real per-eye view — same underlying bug CLASS as every other
camera-anchored effect this session chased (stars, sun-kagerou,
now-ruled-out cloud effects), just in a completely different subsystem
nobody thought to check because "shadows" were already believed to be
fully off.

**Fix**: added the exact same VR early-return `dDlst_shadowSimple_c::draw()`
already has, to `dDlst_shadowReal_c::draw()` too — `if
(g_duskVRRenderingToHeadset) return;` at the top, before any GX state
changes. This COMPLETES the existing, already-documented "shadows
disabled in VR" intent rather than reversing or second-guessing it — CLAUDE.md's
own standing lesson ("don't infer a fix supersedes a disable guard without
evidence") doesn't apply in reverse here; this is disabling something
that was actively drawing, matching the documented intent, not
re-enabling something that was off.

**Built successfully** (RelWithDebInfo) — only `d_drawlist.cpp`
recompiled, clean link, no new warnings.

**CLAUDE.md updated** to correct the permanent-constraints note — it
previously implied ALL shadows were disabled in VR; now clarifies both
subsystems are covered as of this fix, and explains the gap that existed
until now.

**Everything from the cloud-shadow investigation (both dead-end
mechanisms) left in place, untouched**: the `[dusk::cloudshadow]`
logging in `drawCloudShadow()`/`dKy_cloudshadow_scroll()`, and both VR
disables applied to them earlier this session (the billboard packet and
the ground-material texture scroll) — all harmless, none of them were
ever the actual cause, but none were wrong to have tried either given the
evidence available at each point. Not worth reverting; they just never
mattered for this specific symptom.

**IN-HEADSET RESULT: still not fixed.** Real object shadows (`dDlst_shadowReal_c`)
were a genuine, separate bug worth fixing on their own merits, but not
the one the user was actually describing.

### ACTUAL ACTUAL root cause, from an outside source: "Moya" ground haze — FIXED 2026-08-09

**The real identification, from another modder** (not found via this
session's own investigation): "It's a particle effect, its proper name is
'Moya', projected particle ground shading... not necessarily a typical
EFB or scrolling texture, tho it does scroll." "Moya" (もや, "haze/mist")
is a real, pre-existing name already in this codebase —
`g_env_light.mMoyaMode`/`mMoyaCount` (state driven by `d_a_kytag06.cpp`,
an environment-mist trigger actor) and, more directly, a shared texture
resource `mpMoyaRes` used by exactly two draw functions in
`d_kankyo_rain.cpp`: `dKyr_mud_draw()` (`dKankyo_mud_Packet`/`EF_MUD_EFF`
— despite the internal "mud" name, this is general ground haze, not a
mud-dungeon-specific effect) and `dKyr_evil_draw()`
(`dKankyo_evil_Packet`/`EF_EVIL_EFF` — the Twilight/evil-realm variant,
also calls a `dKyr_evil_draw2()` helper internally). Both use the exact
same camera-anchored billboard technique as every other effect in this
bug class: `MTXInverse(dComIfGd_getView()->viewMtxNoTrans, camMtx)`,
confirmed by direct code read — the FLATSCREEN camera's matrix, not
either eye's real per-eye VR view.

Not a JPA particle system in the strict engine sense (the modder's
"particle" description doesn't map to this codebase's actual
`dPa_control_c`/JPA particle infrastructure used elsewhere in this
file) — it's the same billboard-quad-with-a-haze-texture technique as
`drawCloudShadow()`/`dKyr_drawStar()`, just under a different internal
name ("mud"/"evil") that never surfaced in any of this session's earlier
greps for "cloud"/"kumo"/"shadow". The "Moya" name itself is what
finally connected it — a name this session's own code reading never
independently arrived at.

**Fix**: added the same `isRenderingToHeadset()` early-return to both
`dKyr_mud_draw()` and `dKyr_evil_draw()` (the latter's internal
`dKyr_evil_draw2()` helper is only ever called from within
`dKyr_evil_draw()`, so it's covered transitively, no separate guard
needed). Flatscreen unchanged.

**Built successfully** (RelWithDebInfo) — only `d_kankyo_rain.cpp`
recompiled, clean link, no new warnings.

**Everything from every earlier wrong guess this session left in place,
untouched**: the billboard `drawCloudShadow()` disable, the ground-decal
`dKy_cloudshadow_scroll()` disable, the `[dusk::cloudshadow]` diagnostic
logging, and the `dDlst_shadowReal_c` object-shadow disable — none of them
were the actual cause of THIS symptom, but the object-shadow fix in
particular is a real, independently-valid bug fix worth keeping regardless
(shadows genuinely were drawing unconditionally in VR, camera-locked, even
if that specific system wasn't what "cloud shadows" turned out to be).

**IN-HEADSET RESULT: NOT the cause.** User: "Moya isnt it but make sure
the original moya particles aren't harmed by this change." Also gave a
sharper visual description this time, worth recording precisely for
whoever picks this up next: **"These cloud shadows are like slightly
darkened spots on the ground"** — small discrete dark patches, not a
broad hazy/foggy overlay (which is what Moya actually looks like) and not
full black shadow blobs either (already ruled out via the
`dDlst_shadowReal_c` fix, which is real and worth keeping but also didn't
fix this).

**REVERTED same session**: both `dKyr_mud_draw()` and `dKyr_evil_draw()`
guards removed entirely, back to their original unguarded state — Moya
renders normally in VR again, exactly as before this whole detour. Built
clean (`d_kankyo_rain.cpp` only), confirmed via a fresh build after the
user closed the running game (needed — `dusklight.exe` was still running
and would have locked the linker).

**State after four wrong guesses across two sessions**: `drawCloudShadow()`
(billboard `CLOUD_EFF`) and `dKy_cloudshadow_scroll()` (ground-material
texture scroll) are STILL disabled in VR (never reverted — no evidence
either was ever entered per the `[dusk::cloudshadow]` log capture, so
disabling them is inert either way, not actively wrong to leave as-is).
`dDlst_shadowReal_c::draw()` is STILL disabled in VR (a real, independently
-valid fix — genuine object shadows WERE drawing unconditionally,
camera-locked — just not this specific symptom). Moya is the only one
reverted, since the user explicitly asked for it back.

**Next step, per the plan already agreed with the user**: stop guessing
candidate functions by name — reach for a RenderDoc capture instead (this
project's own established fallback, Build Workflow notes: "When the
`OutputDebugStringA`-log-and-guess loop stalls, reach for a RenderDoc GPU
capture sooner rather than later"). With "slightly darkened spots on the
ground" as a precise visual target, the Resource Inspector/Texture Viewer
approach (search by known width/height/format, or use debug-group markers
if `WinPixEventRuntime.dll` is available) should identify the actual draw
call directly, rather than continuing to guess based on function/effect
names that keep turning out to be red herrings.

### "Darkened spots on the ground" investigation — PAUSED for the night 2026-08-09, resume here

**User explicitly paused** ("Honestly its getting late. Id like to
document this and continue it tomorrow") mid-RenderDoc investigation.
This box is the resume point — read it before touching any of this again,
rather than re-deriving from the play-by-play above.

**Bug still NOT root-caused, after FIVE wrong guesses across two
sessions**: slightly darkened spots on the ground, camera-locked (move
opposite head-turn), confirmed in Ordon Village. Ruled out, in order:
`drawCloudShadow()` (billboard `CLOUD_EFF` packet — never entered per a
real log capture), `dKy_cloudshadow_scroll()` (ground-material texture
scroll — also never entered), `dDlst_shadowReal_c` (real object
"blob" shadows — a genuine separate bug, fixed, KEPT, but confirmed not
this symptom), "Moya" ground haze (`dKyr_mud_draw()`/`dKyr_evil_draw()`
— a specific tip from another modder, tested and ruled out, REVERTED
back to original unguarded state per user request).

**Current tool state, mid-RenderDoc-capture-debugging**:
- `WinPixEventRuntime.dll` is in place next to `dusklight.exe`
  (`build\windows-msvc-relwithdebinfo\`) and `DUSK_GFX_DEBUG_GROUPS=ON`
  is set in the CMake cache — debug-group markers should show up in
  RenderDoc's Event Browser. Confirmed NOT the cause of the capture
  truncation below (tested with the DLL removed too, same result) — no
  need to touch this again unless a NEW reason comes up.
- **Captures are truncating far short of a real frame** — first attempt
  stopped at event 8, most recent at event 41, both times with no bound
  render target (empty Outputs tab) past event 4. Reproducible, not
  random — ruling out an earlier "bad luck landed on a skipped-tick
  frame" theory. NOT yet explained.
- A real, valid bug WAS found and fixed along the way (keep this
  regardless of whether it explains the truncation): `m_Do_main.cpp`'s
  `if (!aurora_begin_frame()) { ...; continue; }` used to skip
  `EndFrameCapture()` entirely, capable of leaving a capture open across
  many more frames than intended. This measurably changed the truncation
  point (8 → 41 event count, more consistent since), but didn't fix the
  underlying truncation.
- **Last diagnostic added, NOT yet captured**: `m_Do_main.cpp` now logs
  `[dusk::renderdoc] pre-EndFrameCapture stats: drawCallCount=...
  mergedDrawCallCount=...` (from `aurora_get_stats()`) right before every
  `EndFrameCapture()` call. **This is the immediate next step tomorrow**:
  take one more F9 capture, then find that exact log line in the VS
  Output window and report the numbers back. High `drawCallCount` despite
  a tiny RenderDoc event count would confirm a genuine capture/
  synchronization gap (the frame's real GPU work is deferred to Aurora's
  render worker thread — see `submitFrame()`'s `aurora::gfx::synchronize()`
  comment in `vr_main.cpp` for the existing, possibly-incomplete fix for
  this exact class of cross-thread timing issue) worth digging into
  further; a low `drawCallCount` would mean the game itself is doing
  almost nothing that frame, which is a different and probably easier
  problem to chase (e.g. capturing during a frame where
  `dusk::vr::tick()` doesn't run at all).

**Files currently modified from this session, for a quick `git diff`
orientation tomorrow** (see each item's own writeup above for full
reasoning):
- `d_particle.cpp` — swim-ripple SSR disabled in VR. **Confirmed working,
  keep.**
- `d_kankyo_rain.cpp` — `drawCloudShadow()` (billboard) VR-disabled +
  `[dusk::cloudshadow]` diagnostic logging. Inert (never entered), not
  reverted (harmless).
- `d_kankyo.cpp` — `dKy_cloudshadow_scroll()`'s dispatch guarded off in
  VR (`setLightTevColorType_MAJI`) + its own `[dusk::cloudshadow]`
  logging. Also inert, also not reverted.
- `d_drawlist.cpp` — `dDlst_shadowReal_c::draw()` VR-disabled. **Real,
  independently-valid fix — confirmed genuine object shadows were
  drawing unconditionally in VR. Keep regardless of the main
  investigation's outcome.**
- `m_Do_main.cpp` — RenderDoc capture-leak fix (keep, real bug) + the
  `drawCallCount` diagnostic (temporary, remove once this is
  root-caused).
- `d_camera.cpp`, `vr_main.hpp`/`.cpp` — the flatscreen-camera-headset-sync
  experiment. **Fully reverted, zero trace left.**

**Cross-check against `mods/shadow_mod` (2026-08-10, next-day session) —
confirms coverage is complete, no new lead.** User pointed at
`mods/shadow_mod` (the pre-existing "[Demo] Dynamic Shadows" mod) on a
tip from the dusklight devs that it "shows how to remove cloud shadows."
It hooks exactly three of the game's own shadow functions to avoid
double-shadowing its own dynamic system: `dDlst_shadowControl_c::
imageDraw()`/`::draw()` (the umbrella class — `draw()` itself loops both
`mSimple[]` and the `dDlst_shadowReal_c` linked-list, i.e. covers BOTH
shadow variants already discussed in section 1) and `drawCloudShadow()`
(confirmed, again, to be "the weather cloud shadows" function — no new
name surfaced). Checked whether either umbrella entry point has an
uncovered call site the way `dDlst_shadowReal_c::draw()` itself once did:
`dComIfGd_drawShadow()`/`dComIfGd_imageDrawShadow()` each have exactly
ONE call site in the whole codebase, both already gated on
`!dusk::vr::isRenderingToHeadset()` (`m_Do_graphic.cpp:2623`/`:2485`), on
top of the per-class early-returns already in `dDlst_shadowSimple_c::
draw()`/`dDlst_shadowReal_c::draw()` themselves. **Net result: no new
fix, no new lead — just independent confirmation that the game's entire
built-in shadow system (both variants + cloud billboard) is fully gated
off in VR with no remaining gaps of the kind this investigation already
had to fix once.** Doesn't change the "darkened spots" bug's status or
the RenderDoc-capture next step above at all — noted here mainly so a
future session doesn't re-spend time re-deriving this same dead end from
`shadow_mod` again.

### "Darkened spots on the ground" — ACTUALLY ROOT-CAUSED AND FIXED 2026-08-10, via a second separate mods repo (`dusklight-mods`), CONFIRMED IN-HEADSET

**The real find, from a completely different source than `shadow_mod`**:
user pointed at `C:\Users\joeyw\dusklight-mods` (a SEPARATE repo/author —
"automata-rtx" — not `mods/shadow_mod` inside this repo) and its
`effect_remover` mod. Its "Terrain Shadow Removal" sub-feature post-hooks
`dKy_bg_MAxx_proc` and pins `TevKColor(1)`'s red channel to 255
(full wash-out) on terrain materials coded `MA00`/`MA01`/`MA16`/`MA04` —
a mechanism this project's own investigation had never looked at.
`effect_remover`'s own comments independently identify `MA00` as "the
usual swaying forest-floor / field-floor shade" (driven by the cloud
packet) and, critically, **`MA04` specifically as "the Faron/forest-floor
ground shadow overlay, confirmed in-game as the slowly swaying floor
shade there"** — i.e. tree/canopy floor shading, a DIFFERENT visual from
the reported Ordon Village bug (which the user confirmed is NOT tree-
shaped — "cloud shaped shadows on the ground that move with the
camera").

**Cross-checked against this repo's own code before touching anything**:
`d_kankyo.cpp`'s `dKy_bg_MAxx_proc` has the identical `MA00`/`MA01`/
`MA04`/`MA16` → `setTevKColor(1, ...)` block (line ~11565), confirming the
mechanism is real here too. Also noticed `dKy_cloudshadow_scroll()` (a
few hundred lines earlier in the same file, already VR-disabled per this
section's own earlier rounds) matches only `MA00`/`MA01`/`MA16` in its own
per-material loop — **it never matches `MA04` either** — independent,
pre-existing precedent in this exact file that `MA04` is NOT part of the
same "generic ground cloud shadow" family as the other three codes.

**Fix applied** (`d_kankyo.cpp`'s `dKy_bg_MAxx_proc`): while
`dusk::vr::isRenderingToHeadset()`, force `sp5C.r = 255` (matching
`effect_remover`'s wash-out technique) right before the existing
`setTevKColor(1, ...)` call — but **only** for `MA00`/`MA01`/`MA16`,
explicitly excluding `MA04` via a `memcmp` check, deliberately mirroring
`dKy_cloudshadow_scroll()`'s own pre-existing 3-code scope one function up
rather than inventing a new one. Flatscreen completely unchanged (the
whole thing is inside `#ifdef TARGET_PC` + the `isRenderingToHeadset()`
check). Built clean, `d_kankyo.cpp` only.

**CONFIRMED FIXED IN-HEADSET, first try**: user tested Ordon Village
(cloud shadows gone) AND Faron Woods (tree/canopy floor shading still
present, `MA04` untouched as intended) — exactly the outcome designed
for. **This closes the "darkened spots on the ground" investigation** —
five wrong guesses across two sessions (`drawCloudShadow()` billboard,
`dKy_cloudshadow_scroll()` scroll, `dDlst_shadowReal_c` object shadows
[kept, real bug, just not this], Moya haze, the reverted camera-sync
experiment) before this. The RenderDoc-capture-truncation investigation
from the PAUSED box above is now MOOT — no need to resume it for this
bug. **Cleanup performed same session**: removed all `[dusk::cloudshadow]`
diagnostic logging (`drawCloudShadow()`'s entry log, `dKy_cloudshadow_
scroll()`'s entry log and its per-material `mProjection`/`mInfo` dump) and
the now-unused `#include <windows.h>` it required in `d_kankyo_rain.cpp`
— per this project's normal practice, now that the bug is confirmed
fixed. Both functions' real VR-disable early-returns are untouched and
still in place (harmless, real disables of real camera-relative effects,
just not the actual cause of this specific symptom). Rebuilt clean
(`d_kankyo.cpp` + `d_kankyo_rain.cpp`), confirmed no leftover references.

**Reusable lesson**: the actual fix came from a second, independently-
maintained mods repo the user happened to check, not from continuing to
bisect inside this repo's own code — worth remembering that an external
mod targeting the same base game can reveal a mechanism (here, a TEV
KColor wash-out on a specific material-code family) that a pure
internal-code investigation had never gone looking for, even after two
full sessions and a RenderDoc capture in progress.

### Hide neck/arms while wearing Ordon Clothes — CONFIRMED WORKING IN-HEADSET 2026-08-10

**Goal** (explicit user request: "hide link's neck and arms when hes in his
ordon clothes"). Section 18's `hideArmsAndEars()`/`showArmsAndEars()`
already hide Link's arm/ear materials during first-person VR gameplay —
but only correctly for Hero's Clothes. Ordon Clothes (internally "casual
wear", `daAlink_c::checkCasualWearFlg()`, `dItemNo_WEAR_CASUAL_e` — the
outfit Link wears for the game's opening prologue, before Wolf Link) use a
**genuinely different, smaller material table**, not just a texture swap
on the same one.

**Confirmed directly in the base game's own code, not assumed**:
`d_a_alink.cpp`'s Master Sword glow-tint logic has a separate
`else if (checkCasualWearFlg())` branch reading different material
indices, guarded by `getMaterialNum() >= 8` — vs. the default (Hero's
Clothes) branch's `>= 18`. `kArmEarMaterialIndices` (section 18) was
dumped specifically while wearing Hero's Clothes (see its own inline
comment, a 2026-07-31 `OutputDebugStringA` capture) and is therefore
meaningless for Ordon Clothes: indices 8/10 are simply out of range
(silently skipped by the existing bounds check) and indices 0/1/2/7 hit
whatever unrelated materials happen to occupy those slots in the
casual-wear table instead. This fully explains the reported symptom —
arms/neck staying visible in first-person specifically while wearing
Ordon Clothes.

**What was added** (`vr_link_visibility.hpp`): both `hideArmsAndEars()`
and `showArmsAndEars()` now check `link->checkCasualWearFlg()` first and
early-return (no-op, symmetric between the two — neither touches
anything) rather than applying the wrong (Hero's-Clothes-derived)
indices. `logCasualWearMaterialsOnce()` — same "one-time material-name
dump" technique this file already used once for Hero's Clothes, not a
new invention — fires the first time `checkCasualWearFlg()` is observed
true, dumping `mpLinkModel`'s real material count and every material name
via `[dusk::vr::ordonmats]` `OutputDebugStringA` lines. Deliberately did
NOT guess plausible-looking indices from the Hero's-Clothes naming
convention (`al_armL_m` etc.) — this project's own standing lesson
(section 12 and others) is that guessing indices/mappings without real
data wastes rounds; a real dump settles it in one pass.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**UPDATE (same day) — real capture came back, real fix applied.** User
pasted a full log capture (`C:\Users\joeyw\Downloads\log.txt`), also
noting "his ears became visible again" — expected and correct: that's
exactly what the interim no-op fallback was designed to do (leave casual
wear completely untouched until real data came in), not a regression.
`[dusk::vr::ordonmats]` showed the real table — 8 materials, `bl_` prefix
(vs. Hero's Clothes' `al_`), notably with **no separate arm material at
all**, unlike Hero's Clothes' three dedicated ones:
```
0 bl_beltS_m   1 bl_ear_m     2 bl_earring_m  3 bl_handLA_m
4 bl_handRA_m  5 bl_lowbody_m 6 bl_skin_m     7 bl_upbody_m
```
`bl_skin_m` (6) is the only candidate for exposed arm/neck skin: face is
its own separate model (already hidden independently), hands are their
own pair (3/4, deliberately excluded — same reasoning as Hero's Clothes,
kept visible for the tracked VR hand model), and everything else is
obviously clothing (belt/lowbody/upbody). `kArmEarMaterialIndicesCasual =
{1, 2, 6}` — `bl_ear_m`, `bl_earring_m` (worn on the ear, same "don't
leave it floating" inclusion reasoning as Hero's Clothes' `al_earring_m`),
and `bl_skin_m`. Wired into both `hideArmsAndEars()`/`showArmsAndEars()`,
branching on `checkCasualWearFlg()` the same shape as the existing
Hero's-Clothes branch. `logCasualWearMaterialsOnce()` left in place
(harmless, fires once) until the mapping is confirmed correct in-headset.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling, clean link, no new warnings.

**CONFIRMED WORKING IN-HEADSET** — user tested and reported "Yup looks
good." Arms, neck (via `bl_skin_m`), and ear/earring all correctly hidden
in first-person while wearing Ordon Clothes, with no reported side
effects elsewhere on the body (i.e. `bl_skin_m` was NOT shared with any
unintended region). Closes out this feature — Link's first-person arm/
ear/neck hiding now works correctly across both outfits (Hero's Clothes,
section 18; Ordon Clothes, here).

**UPDATE (same day) — `logCasualWearMaterialsOnce()` removed.** User gave
standing permission to remove confirmed-fixed VR diagnostics without
asking first going forward (this "leave it in until asked" pattern was a
one-time exception for the earlier cloud-shadow investigation, not a
general policy — see [[feedback_remove_diagnostics_freely]]). Removed the
function and its call site from `hideArmsAndEars()`; `kArmEarMaterialIndicesCasual`
and the hide/show logic that uses it are untouched. Built clean.

### Show all of Link's limbs in the pause/status menu — CONFIRMED FIXED IN-HEADSET 2026-08-10

**Goal** (explicit user request: "make it so you see all of link's limbs
in the pause menu"). Root cause, found by reading code rather than
guessing: `d_a_alink_swindow.inc` ("Pause Menu Player Display",
`daAlink_c::statusWindowDraw()`) draws `mpLinkModel`/`mpLinkHandModel`/
`mpLinkHatModel`/`mpLinkFaceModel` — the SAME shared model instances
`hideArmsAndEars()`/`hideModel()` hide shapes on for first-person VR
gameplay. Shape-hide (`J3DShapeTable::hide()`/`J3DShape::hide()`) is a
property of the shared `J3DModelData`/`J3DModel` RESOURCE itself, not
scoped to any one camera or view — so hiding for first-person gameplay
bleeds directly into this separate third-person character-preview draw
too. `isFirstPerson()` has no idea the pause/status menu is even open:
`checkEventRun()`/`checkWolf()` are both unrelated to pause state (you
can pause during completely ordinary exploration).

**Fix** (`vr_link_visibility.hpp`'s `updateFrame()`): the local
`firstPerson` bool that gates hiding is now `isFirstPerson(link) &&
!dComIfGp_isPauseFlag()` — `dComIfGp_isPauseFlag()` (`d_com_inf_game.h`)
is the real, already-used pause-menu-open flag, confirmed by reading
where it's actually set/cleared (`d_menu_window.cpp`'s `dMw_c`), not
guessed from its name. **Deliberately scoped to just this local, not
`isFirstPerson()` itself** — `getVrCameraEyeAnchor()`/
`getVrBodyPositionOffset()` also call `isFirstPerson()` for camera-anchor
purposes, and changing that shared function would risk an untested side
effect on camera behavior while paused for zero benefit: the pause
menu's own backdrop is a frozen screen capture (`dDlst_MENU_CAPTURE_c`,
same file), not a live re-render, so the live VR camera isn't what's
actually on screen while paused anyway.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Fixed." Link's
face/hat/arms/ears/neck all show correctly in the pause/status menu's
character preview now, and the existing per-frame re-application logic
(same mechanism already covering the cutscene/Wolf-form third-person
cases) correctly re-hides everything again once the menu closes, with no
separate handling needed for that direction. Closes out this feature.

### Persistent VR status logging via real DuskLog (not just OutputDebugStringA) — built 2026-08-10, NOT yet confirmed in-headset

**Goal** (user question "would it hurt performance to log VR related
issues", followed by "sure" to the offer of adding a low-overhead status
logger). This project's whole `OutputDebugStringA`-based VR debug
workflow (top of this skill file) only ever produces output visible with
a debugger attached — meaningless for a real end user's bug report, since
they won't have Visual Studio attached to their own game. Meanwhile
`startup()` already had an explicit, long-standing TODO on its exception
handler: "Not routed to real DuskLog -- that call site still isn't
confirmed."

**Why not just log everything, always**: checked `dusk/logging.cpp`'s
actual write path before deciding — `WriteLogLine()` does
`fprintf`+`fwrite`+`fflush()` on EVERY call (a real synchronous disk
flush, not buffered), under a mutex, to BOTH a console stream and the
file. Genuinely expensive per call, and unpredictable (disk I/O can stall
for real time) — calling this every VR frame would be a measurable,
possibly stutter-inducing cost against an ~11-14ms frame budget. Same
underlying reasoning this project's own `OutputDebugStringA` diagnostics
already follow ("fire once or a capped handful of times, never every
frame") — just with a stricter version of it, since `DuskLog` is
provably more expensive per call than `OutputDebugStringA` (which is
near-free with no debugger attached).

**What was added** (`vr_main.cpp`): a new `static aurora::Module
VrLog("dusk::vr");`, following this codebase's existing per-subsystem
`aurora::Module` convention (`DuskConfigLog("dusk::config")`,
`Log("dusk::mods::manifest")`, etc.) — writes to the same real,
persistent AppData log file every user gets automatically, no dev
tooling required. Scoped ONLY to genuinely rare, event-driven call
sites, never the per-eye render path:
- `startup()`'s outcome — success (with runtime name + swapchain
  dims/format) and all four existing failure points (0 views, swapchain
  creation failure, session-never-READY, and the exception catch-all —
  this closes the old TODO). A one-time `xrGetSystemProperties()` call
  right after `vr_xr::initialize()` succeeds captures the runtime name
  (`sysProps.systemName`, e.g. distinguishing SteamVR/Virtual
  Desktop/Meta Link) so every log line in the function can say which
  runtime it's talking about — genuinely high-value given how much of
  this project's history (section 6, stereo eye alignment, aim-pose
  calibration) turned out to be runtime-specific. `sysProps` is
  deliberately declared OUTSIDE the `try` block (not inside, where the
  existing `boot`/`gfx` locals live) so the `catch` block can still
  reference it — with an empty `systemName` (from the aggregate init's
  zero-fill) if the exception fired before the query ever ran, e.g.
  `vr_xr::initialize()` itself throwing — which is itself informative
  (means it failed before even reaching the runtime).
- Session-state-changed transitions, both during startup
  (`waitForSessionReadyAndBegin()`) and mid-session (`tick()`'s ongoing
  event pump) — STOPPING/READY-resume/EXITING/LOSS_PENDING. The
  mid-session pump previously had ZERO logging on any channel at all
  (not even `OutputDebugStringA`) despite handling real scenarios like
  system-dashboard focus stealing or headset removal — exactly the kind
  of thing a "VR randomly stopped working mid-play" user report would
  otherwise leave no trace of.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling, clean link, no new warnings.

**NOT yet tested in-headset** — next step: launch in VR (success case)
and check the AppData log for a `[INFO | dusk::vr] startup succeeded:
runtime=... swapchain=...` line; separately worth deliberately
reproducing a startup failure or a mid-session STOPPING event (e.g.
opening the SteamVR dashboard) to confirm those paths log correctly too.
No perf concern expected given the scoping above, but not separately
measured this session.

### Desktop mirror (show a VR eye's view on the desktop window) — CONFIRMED WORKING IN-HEADSET 2026-08-10

**Goal** (explicit user request: "show the game view in the window without
hurting performance too bad"). Today the desktop window shows essentially
nothing/stale content while VR renders — `m_Do_main.cpp` deliberately skips
the normal flatscreen draw whenever `isRenderingToHeadset()` is true (its
own comment: "the game skips its own desktop redraw"). A prior session had
already tried the obvious fix and reverted it — see the same file's
"REMOVED this session: a temporary desktop-mirror hack" comment: drawing
the whole scene a SECOND time (reusing the last VR eye's camera) for the
desktop corrupted state several systems assume runs exactly once per frame
(solid-white dialogue-vignette corruption, broken water reflections). That
history ruled out "just draw twice" as an option before this session even
started.

**The actual mechanism used instead — reuse, don't duplicate**: `aurora`
(the `extern/aurora` submodule) already runs a "present-resample" pass
EVERY frame regardless of VR state — `end_frame()` always samples
`webgpu::present_source()` (normally aurora's own internal flatscreen
framebuffer) and scales/letterboxes it into the real OS window. Since VR's
per-eye rendering already produces a fully-resolved, GPU-resident color
texture every eye (`aurora::gfx::ResolvedTargets::color`/`.colorTexture`,
returned by `vr_render::endEye()` — already used for the real XR
swapchain submission, see section 20's `[dusk::vr::eyepasscheck]`
investigation for how solid that pipeline is), pointing the EXISTING
resample pass at eye 0's texture instead of the (empty, since flatscreen
draw is skipped) internal framebuffer gets a mirror with **zero extra
render passes, zero CPU readback** — the resample pass runs unconditionally
either way; only WHICH texture it samples changes.

**What was added**:
- `extern/aurora/lib/webgpu/gpu.hpp`/`.cpp`: `g_presentSourceOverride`
  (a `TextureWithSampler`, empty by default) + `set_present_source_override()`/
  `clear_present_source_override()`. `present_source()` now checks the
  override first before falling back to the normal internal framebuffer.
  Plain field writes (ref-counted wgpu handle copies), no direct GPU/queue
  calls — safe to call from the same thread as
  `aurora_begin_frame()`/`aurora_end_frame()`, unlike e.g.
  `resample_present_source()`'s `g_queue.WriteBuffer`, which has its own
  `ASSERT(gfx::render_worker::is_worker_thread(), ...)` for exactly that
  reason — this new code deliberately avoids needing that.
- `extern/aurora/include/aurora/gfx.hpp`/`lib/gfx/common.cpp` (the PUBLIC-
  facing layer VR code already includes, unlike the internal `webgpu::`
  headers): `aurora::gfx::set_present_source_mirror(const ResolvedTargets&)`/
  `clear_present_source_mirror()` — wraps the webgpu-layer functions,
  building a `TextureWithSampler` from the `ResolvedTargets` fields plus a
  lazily-created, cached linear/clamp sampler (same descriptor shape
  `create_render_texture()` already uses for its own render targets — one
  sampler instance, reused every call, no per-frame allocation). No-ops
  safely if `source.color`/`.colorTexture` are null (a skipped/foreign-
  substituted eye this frame).
- `vr_main.cpp`'s `tick()`: `clear_present_source_mirror()` added to the
  existing "reset up front" block at the very top (alongside
  `g_duskVREyePassOpen = false` etc.) — matches this file's established
  convention (see the `TickReentrancyGuard` writeup in section 20 for why
  that block exists) so any of `tick()`'s many early-return paths (no
  session, XR failures, no ready gameplay view) leave the desktop on its
  normal flatscreen fallback rather than stuck showing a stale VR eye. Eye
  0's `targets` are captured into a `mirrorEyeTargets` local right after
  `endEye()` inside the per-eye loop (only when `targets.colorTexture` is
  non-null, i.e. this eye actually resolved this frame — same
  foreign-pass-substitution caveat already documented at that call site
  for the real XR eye-copy path); `set_present_source_mirror()` is called
  once, after the loop, gated on a new settings toggle.
- `dusk/settings.h`/`.cpp`: new `ConfigVar<bool> vrDesktopMirror`
  (`"game.vrDesktopMirror"`, default `true`) under Graphics, registered the
  same way as every other settings bool in this file (`disableWaterRefraction`,
  `shadowResolutionMultiplier`, etc.).

**UPDATE 2026-08-10 (same day) — in-game Video-settings toggle added,
built.** User asked how hard adding a toggle in the video settings would
be — turned out trivial, since `dusk/ui/settings.cpp`'s "Video" tab
already has the exact `config_bool_select(leftPane, rightPane, <ConfigVar<bool>>,
{.key, .helpText})` pattern used for every other simple graphics toggle
(`enableMapBackground`, `disableCutscenePillarboxing`, right next to
where this was added). One more call in that same shape, right after
`disableCutscenePillarboxing`'s — no new plumbing needed, since
`vrDesktopMirror` was already a registered `ConfigVar<bool>` from the
initial implementation above. **Not the same setting as `enableMirrorMode`**
(labeled "Mirror Mode" in the General tab) — that's an unrelated, pre-
existing gameplay feature (a Mirror-World-style horizontal flip), already
noted above as a naming trap to avoid confusing with this. Built clean,
only `settings.cpp` recompiled.

**Why "eye 0 (left), fixed choice" rather than something fancier**: no
stereo depth is needed for a 2D desktop preview, and eye 0 is already the
convention used elsewhere in this codebase for single-eye readback (e.g.
the existing `vr_debug_eye0.bmp`/`vr_debug_eye1.bmp` dump tooling in
`vr_xr_submit.hpp`, which dumps both but treats eye0 as primary). No
per-frame flip-flopping between eyes was implemented — simpler, and
avoids a visible left/right jump if aspect/FOV differ slightly per eye.

**Built successfully** (RelWithDebInfo, full rebuild since this touched
`extern/aurora`'s public headers — 1250/1250 objects, no errors, no new
warnings). `extern/aurora` is a submodule with its own dirty working tree
from this change, same as the project's other in-place aurora edits
documented elsewhere in this file (e.g. section 3's `resolve_pass_into()`
fix) — not committed inside the submodule, matching existing practice.

**CONFIRMED WORKING IN-HEADSET** — user tested and reported "It works,"
covering both the mirror itself (desktop window shows the left eye's
view instead of stale/blank) and the Video-settings toggle added the same
day (below) — turning it off/on correctly stops/resumes the mirror. No
aspect-ratio stretching or performance complaint reported. Closes out this
feature — the near-zero-cost mechanism (reusing aurora's existing
present-resample pass rather than adding any new render work) held up in
practice, not just in theory.

### Known issue, NOT YET INVESTIGATED — pervasive "offscreen pass" warnings throughout real VR sessions, found via the new persistent VR logging

**Found incidentally** (2026-08-10) while checking the first real AppData
log produced by the new `VrLog` status logging above — not something
anyone was actively looking for. In
`dusklight-20260810-154547.log`, this pair of warnings:
```
[WARNING | aurora::gx] aurora::gx::copy_tex: draining a queued GXCopyTex WHILE an offscreen pass is open
[WARNING | aurora::gfx] resolve_pass_into: substituting the current pass while an offscreen pass is open (pass id=...) -- this seals/replaces whatever pass is current without checking g_inOffscreen. If something else opened that offscreen pass and expects it to still be current later, it will be silently orphaned unless it verifies via current_pass_id()/resolve_pass_checked().
```
fires **9,336 times** across the session — starting at line 657 (shortly
after the VR session reaches `FOCUSED`, well before any shutdown/menu
activity) and continuing essentially continuously through to the very end
of the ~31,000-line log, right up to the final mod-shutdown sequence. Not
a one-off burst tied to quitting or opening a menu — this looks like an
ongoing, steady-state condition throughout normal VR gameplay.

**Why this is worth a look eventually, not urgent right now**: this is
directly the "foreign/nested offscreen pass substitution" bug class this
whole project has hit multiple times before as a REAL root cause —
`resolve_pass_checked()`/`current_pass_id()`/the "protected offscreen
pass" mechanism (section 3's water-black bug, section 8's minimap
black-screen bug) exist specifically because this exact substitution
silently corrupts whatever pass it steps on. The user has NOT reported
any specific visible symptom tied to this — no crash, no visual
corruption currently known — so this may be an already-handled/harmless
case (e.g. something already covered by the protected-pass guard, just
still emitting a warning on the way), or it may be an early symptom of
something not yet noticed. **Explicitly deferred by the user** ("just
write that down... we can go back to it if theres a problem") — not
investigated further this session.

**If picked up later**: grep the AppData log for `pass id=` numbers
around a few of these warnings and cross-reference against
`current_pass_id()`/`resolve_pass_checked()` call sites to see whether
VR's own protected eye pass is ever among the ones getting silently
substituted (vs. some other, already-tolerated offscreen pass elsewhere
in the frame) — that would distinguish "background noise, ignore" from
"this might explain some other pending report." The `pass id`s logged in
this specific capture ranged from the low 40000s up into the mid 45000s
by the end of the session, for reference.

### VR tracked-hand attachment extended past sword/shield to held items + bombs — built 2026-08-12, NOT yet confirmed in-headset

**Goal** (explicit user request: "put all of the items that you can equip
and hold in your hand" [VR-tracked], calling out bombs specifically).
Extends section 16's sword/shield tracked-hand fix to `mHeldItemModel`
(bow, bottles, oil bottle, copy rod, boomerang, etc.), the separate
`mpKanteraModel` (lantern), and bombs — the three pieces section 16's own
writeup had explicitly scoped out/deferred.

**`mHeldItemModel`/`mpKanteraModel`** (`vr_link_visibility.hpp`'s new
`refreshTrackedHeldItemMtxLive()`, called from `vr_main.cpp`'s `tick()`
right after the existing `refreshTrackedItemMtxLive()`): reuses the exact
proven architecture (`computeTrackedItemMtx()`, `applyTrackedItemMtxIfAttached()`,
`markModelJointsLive()`, `refreshRestingPoseSmoothed()`) with one real
generalization. Sword/shield always attach directly to their item joint
with zero extra offset, so the raw body-model item-joint matrix could be
used directly as the "current relationship to the hand" basis. Several
held-item branches (bottle, oil-bottle, bow-left-hand, kantera) layer an
*additional* `mDoMtx_stack_c` translate+rotate on top of the item joint in
`setItemMatrix()` — rather than duplicate those literal offset constants
into VR code (a second place to keep in sync), a new `RawBasisCache`
captures the model's own `getBaseTRMtx()` — i.e. whatever `setItemMatrix()`
actually computed that tick, offset included, whichever branch it took —
**once per real sim tick, before this file's own override overwrites it**.
This sidesteps, by construction, the exact self-corruption trap
`applyTrackedItemMtxIfAttached()`'s own comment documents two failed
heuristics for on sword/shield's gating (a live re-read after the first
frame would see our own prior overwrite instead of the real game pose).

`computeHeldItemAttach()`/`computeKanteraAttach()` mirror `setItemMatrix()`'s
real branch dispatch (same precedent as `checkShieldHandAttached()` already
established: duplicating the real dispatch condition structurally, since
there's no cheaper way to ask "which branch did it take"). **Deliberately
excluded, left completely untouched**: the `0x106` branch (attaches to the
HEAD/face joint, not a hand), hookshot (`setHookshotPos()` drives a real
fire/retract state machine across both item joints, swapping which
physical model gets which joint), and iron ball (`setIronBallPos()` reads
a hardcoded joint 15 plus a real chain-link simulation and `AtSph`
collision) — none of these are a simple joint attach, matching section
16's original scoping boundary. Kantera additionally reuses
`refreshRestingPoseSmoothed()` for its belt/back resting pose (it has one,
unlike `mHeldItemModel`, which is always hand-attached to *something*
whenever the model exists at all).

**Known, accepted gap, not fixed this round**: bottles get a *second*,
independent position write via a J3D animation callback
(`bottleModelCallBack()`, `d_a_alink_bottle.inc`) that writes
`mHeldItemModel`'s own joint 1 directly from `mRightItemJntNo` — bypasses
`setBaseTRMtx()` entirely, so this fix's base-transform override doesn't
reach it. Left as a residual visual imperfection (bottle's sub-joint,
maybe a cork/stopper piece, won't track as tightly as the rest of the
model) rather than chasing it this round.

**Bombs** — architecturally separate (confirmed via exploration): once
pulled out, a bomb is a real actor (`dBomb_c`), positioned every sim tick
by `daAlink_c::setGrabItemPos()` (`d_a_alink_grab.inc`) at
`(mLeftHandPos + mRightHandPos) * 0.5` plus a small local offset.
`mLeftHandPos`/`mRightHandPos` themselves are computed once per sim tick in
`setBodyPartPos()` from the flatscreen-animated hand joints — same bug
class as sword/shield's original "floating" symptom, just one level
removed (an averaged midpoint feeding a carried actor's real physics
transform, not a model's own draw matrix). **Fix**: while
`isRenderingToHeadset()`, `setBodyPartPos()`'s human-form branch now
overrides `mLeftHandPos`/`mRightHandPos` with the real tracked controller
positions (new `dusk::vr::getTrackedHandWorldPos()`, reading the
translation column of `vr_link::detail::s_leftHandMtx`/`s_rightHandMtx` —
falls back to the original flatscreen-joint computation if hand-tracking
data isn't valid yet). Verified safe against every other read of
`mLeftHandPos`/`mRightHandPos` in `d_a_alink.cpp`: the wolf-form branch
(a separate, untouched code path) feeds `setWolfCollisionPos()`; the two
remaining human-form reads (fishing-rod-lure spawn position, bomb spawn
position) both benefit from the more-accurate tracked position, no adverse
consumer found.

**Explicitly a PARTIAL fix for bombs, not the same depth as sword/shield**:
`setBodyPartPos()` runs at sim-tick rate (~30Hz) regardless of this
change, and `setGrabItemPos()` — what actually consumes this for a carried
actor — runs at the same rate, so some residual stair-step lag is expected
even after this fix, matching the class of bug section 20 had to fix more
deeply (`mark_live_this_frame()`/`mDoExt_modelUpdateDL()`) for Link's own
body/hands. A full fix would need extending that live-mark/override
technique to the carried actor's own `current.pos`, which is real
physics/collision state (read by `dBomb_c`'s own logic), not just a draw
matrix — a materially riskier change than anything else in this section,
deliberately not attempted, not yet explored.

**Built successfully** (RelWithDebInfo, `windows-msvc-relwithdebinfo`
preset) — `vr_link_visibility.hpp`, `vr_main.hpp`/`.cpp`, `d_a_alink.h`,
`d_a_alink.cpp` all recompiled, clean link, no new warnings, verified via
a second no-op incremental rebuild.

**NOT yet tested in-headset.** Next step for whoever picks this up: equip
and draw the bow (both grip stances if reachable), a bottle, an oil bottle
(if reachable pre-item-get), the lantern/kantera, copy rod, and boomerang
— confirm each tracks the real hand instead of floating at the old
flatscreen position. Confirm hookshot and iron ball are UNCHANGED (sanity
check, not a regression target). Pull out and hold a bomb — confirm it now
sits at/moves with the real tracked hand (not necessarily perfectly
smooth, per the documented partial-fix caveat above).

### `getLeftItemMatrix()`/`getRightItemMatrix()` made VR-aware — fixes fishing rod, boomerang, nocked arrows, and ~7 other downstream consumers at once — built 2026-08-12, NOT yet confirmed in-headset

**Follow-up to the section above**, same day. User tested and reported
"Double clawshot, fishing rod and boomerang do not track on my hands."
Split into two different situations:
- **Double clawshot (hookshot)**: deliberately excluded by the section
  above (physics/chain-driven state machine, not a simple attach) — asked
  the user whether to attempt it now or defer; **user chose to defer**
  ("Leave it out for now").
- **Fishing rod and boomerang**: a real, unaddressed gap. While just
  *held* (not actively used), both are positioned by `mHeldItemModel`'s
  generic branch, already covered by the fix above. But an actively-cast
  fishing rod (`d_a_mg_rod.cpp`, the fishing minigame) and a thrown/flying
  boomerang (`d_a_boomerang.cpp`) are their own separate actors that read
  `daAlink_c::getLeftItemMatrix()`/`getRightItemMatrix()` DIRECTLY — the
  raw, untracked body-joint matrix (`mpLinkModel->getAnmMtx(mLeftItemJntNo/
  mRightItemJntNo)`) — never touched by the previous fix (which only
  overrides `mHeldItemModel`'s/`mpKanteraModel`'s OWN draw matrix, not the
  raw joint other actors read independently). This is exactly the "ripple
  effect" gap section 16's original writeup flagged as the harder,
  unaddressed part of extending VR tracking past sword/shield: `getLeftItemMatrix()`/
  `getRightItemMatrix()` are read directly by ~10 actor files besides
  `mHeldItemModel` itself — nocked arrows (`d_a_alink_bow.inc`,
  `d_a_arrow.cpp`), boomerang throw/trail (`d_a_boomerang.cpp`, 3 call
  sites), fishing rod (`d_a_mg_rod.cpp`, 4 call sites; `d_a_mg_fish.cpp`),
  several enemy-interaction actors (`d_a_e_bug.cpp`/`d_a_e_fm.cpp`/
  `d_a_e_gob.cpp`/`d_a_e_sm2.cpp`), an NPC interaction (`d_a_npc_tk.cpp`),
  an object interaction (`d_a_obj_lp.cpp`), and the canoe paddle
  (`d_a_alink_canoe.inc`).

**Fix, at the source rather than touching every caller**: `getLeftItemMatrix()`/
`getRightItemMatrix()` (`d_a_alink_link.inc`) are `virtual`, and `daAlink_c`
already overrides them — so changing the two function BODIES fixes every
one of those ~10 downstream consumers at once, through ordinary virtual
dispatch, with zero changes needed to any of those files. Each now checks
`isRenderingToHeadset()` first and, if a fresh tracked-hand-relative
matrix is available this frame, returns a `static Mtx` local holding it
instead of the raw joint matrix — same "stable `MtxP` into persistent
storage" contract `getAnmMtx()` itself already provides, so no caller
needed to change how it uses the return value.

**New plumbing** (`vr_link_visibility.hpp`): unlike the held-item
`RawBasisCache`, no once-per-tick caching is needed here — `mpLinkModel->
getAnmMtx(mLeftItemJntNo/mRightItemJntNo)` is the BODY model's own joint
matrix, which nothing in this VR code ever writes to (only
`mSwordModel`/`mShieldModel`/`mHeldItemModel`/`mpKanteraModel`'s own base
transforms are overridden elsewhere), so it's safe to read fresh every
real frame with no self-corruption risk. `refreshTrackedItemJointMtxLive()`
(called once per real frame from `vr_main.cpp`'s `tick()`, alongside the
other `refresh*Live()` calls) computes both sides via the same
`computeTrackedItemMtx()` used for held items, caching into
`detail::s_trackedLeftItemJointMtx`/`s_trackedRightItemJointMtx`;
`getTrackedItemJointMtx(bool isLeft, Mtx outMtx)` copies the cached result
out, returning false if unavailable.

**Deliberately gated on `isFirstPerson(link)`, not just
`isRenderingToHeadset()`** — a difference from sword/shield/held-item's
own overrides, which have no such gate (safe there only because those
models simply aren't drawn/equipped during Wolf form or third-person
cutscenes in practice). `getLeftItemMatrix()`/`getRightItemMatrix()` are
called unconditionally by files with zero awareness of Link's current
form (an enemy-interaction actor could run during Wolf form too). During
Wolf form, `mLeftItemJntNo`/`mLeftHandJntNo` collapse to the SAME joint
(19) — the computed relative offset would reduce to identity, returning
the tracked hand's raw matrix directly, which is wrong for Wolf's
different rig and third-person camera. Gating on `isFirstPerson()` (already
correctly covers Wolf/mounted-cutscene/hidden-cutscene, see its own
definition) avoids that risk by construction instead of discovering it via
a report later.

**Known, accepted side effect, not individually verified**: this also
changes canoe-paddle positioning and the several enemy-interaction actors'
hand-reference point during ordinary first-person gameplay (they now get
the same tracked-hand-relative treatment for free) — a natural consequence
of fixing the general mechanism at its source rather than per-caller, not
something individually tested in-headset. If canoe paddling or an
enemy-grab animation looks off after this, start here.

**Built successfully** (RelWithDebInfo) — `vr_link_visibility.hpp`,
`vr_main.hpp`/`.cpp`, `d_a_alink_link.inc` (via `d_a_alink.cpp`)
recompiled, clean link, no new warnings, verified via a second no-op
incremental rebuild.

**NOT yet tested in-headset.** Next step for whoever picks this up: cast
the fishing rod and confirm it tracks the real hand during the minigame;
throw a boomerang and confirm its trail/flight tracks correctly; as a
bonus check, nock an arrow with the bow and paddle a canoe to see whether
either looks better or worse than before (neither was reported broken,
but both are now affected by this same mechanism). Double clawshot remains
a known, deliberately-deferred gap — not attempted this round.

### Boomerang + fishing rod still laggy after the above — same root cause as the original hand/sword/shield bug, one level downstream — FIXED 2026-08-12, built, NOT yet confirmed in-headset

**User report, testing the two sections above**: "the boomerang and
fishing rod are in the hand, but they're lagging behind just like the
hands, sword, and shield originally did. the other items do not lag. the
clawshot is still not tracked" (clawshot expected — deliberately deferred
above).

**Root cause**: `getLeftItemMatrix()`/`getRightItemMatrix()` themselves
are fresh every real VR frame (previous section) — but their two
`d_a_mg_rod.cpp`/`d_a_boomerang.cpp` consumers only ever *sample* that
fresh value once per 30Hz sim tick, then hold the resulting model
transform fixed until the next tick — the exact same bug class section
20 spent a whole investigation on for Link's own body/hands, just one
level removed (the accessor is fixed; the caller only reads it at tick
rate). Confirmed directly by call-stack tracing, not inference:
- `daBoomerang_c::setKeepMatrix()` (sets `mp_boomModel`/`mp_shippuModel`/
  `mp_setboomEfModel`'s base transforms from `getLeftItemMatrix()`) is only
  ever called from `procWait()` — the boomerang's own `m_procFn`, run once
  per sim tick — and once from `create()`.
- `rod_control()` (the fishing rod's attach-point/IK-chain derivation,
  reading `getLeftItemMatrix()`/`getRightItemMatrix()`) is only ever
  called from `rod_main()`, itself only called from `dmg_rod_Execute()` —
  the rod's sim-tick execute function.

**Fix shape, boomerang (straightforward)**: `setKeepMatrix()` split into
`daBoomerang_c::applyTrackedKeepTransforms()` (the pure visual half —
`getLeftItemMatrix()` read + `mDoMtx_stack_c` offset/rotate +
`setBaseTRMtx()` × 3) and the original `setKeepMatrix()` (now just calls
the new function, then keeps `current.pos` and `simpleAnmPlay()` for
itself — both deliberately excluded from the live-refreshed half:
`current.pos` is real collision/logic state that belongs at tick rate,
and re-triggering `simpleAnmPlay()` every real frame would restart the
wait animation instead of just refreshing a transform).
`vr_link::refreshTrackedBoomerangMtxLive()` (`vr_link_visibility.hpp`),
called once per real frame from `vr_main.cpp`'s `tick()`, re-invokes
`applyTrackedKeepTransforms()` — gated to the KEPT (held, not thrown)
boomerang only, checked via `getThrowBoomerangAcKeep()->getID() ==
fpcM_ERROR_PROCESS_ID_e` (a real state-transition check, not a guess —
that id becomes valid at the exact moment the same actor's `m_procFn`
flips from `procWait` to `procMove`/thrown flight, which is positioned by
`setMoveMatrix()`'s own unrelated physics).

**Fix shape, fishing rod (needed a real audit first — initial "just
re-call it" assumption was wrong)**: `rod_control()` is a ~250-line
procedural IK chain (16 rod segments derived from the attach matrix +
persistent "spring" scalars). First assumption — that it's pure/stateless
enough to safely re-invoke at real frame rate — turned out to be **almost
but not quite right**: enumerating every single `i_this->` field write
inside the function (not sampled — every one) found exactly two things
written: `rod_angle_y` (a harmless per-call output) and a
previous/current tip-position delta-tracking pair
(`field_0x6b8`/`rod_tip_pos`, `field_0x6d4`/`field_0x6c8` — presumably
feeding rod-tip-velocity physics read elsewhere, e.g. cast/fish-bite
detection). Naively re-calling `rod_control()` extra times per real frame
would have corrupted that pair (each extra call's "previous" value would
be a fraction of a tick old instead of one full tick old), risking a
subtle, hard-to-notice break in fishing-minigame feel — exactly the class
of risk this project's own notes already flag for bombs' physics state
("materially riskier... not attempted"). **Fix**: `dmg_rod_
refreshTrackedPositionLive()` (new, `d_a_mg_rod.h`/`.cpp`) snapshots that
one delta-tracking pair, calls `rod_control()`, then restores it — so an
extra real-frame call refreshes every visual output (the 16-segment
position array, every rod/lure model's base transform) while leaving the
*next real sim tick's* `rod_control()` call to see exactly the state it
would have if the extra call had never happened. No duplication of the
IK math itself — the real function runs unmodified, just bracketed.
`vr_link::refreshTrackedFishingRodMtxLive()` (`vr_link_visibility.hpp`),
called once per real frame from `tick()`, finds the live rod actor via
`fopAcM_SearchByName(fpcNm_MG_ROD_e)` (`dmg_rod_class` has no dedicated
accessor on `daAlink_c` the way the boomerang does — this is the
established fallback for actors without one) and calls the new wrapper.

**Both gated on `isFirstPerson(link)`**, matching every other item-tracking
live-refresh in this section.

**Built successfully** (RelWithDebInfo, two separate incremental builds —
boomerang fix built and verified clean first, fishing rod fix added and
rebuilt clean second) — `d_a_boomerang.h`/`.cpp`, `d_a_mg_rod.h`/`.cpp`,
`vr_link_visibility.hpp`, `vr_main.hpp`/`.cpp` all recompiled across the
two rounds, no errors, no new warnings either time.

**ROUND 2 (same day) — user tested round 1, "They both still lag." Real
missing piece found and fixed, built, NOT yet confirmed in-headset.**

Round 1 only did HALF of the already-established fix pattern. Comparing
directly against `applyTrackedItemMtxIfAttached()`/`markModelJointsLive()`
(the proven, confirmed-working mechanism for sword/shield/held items)
found the gap: calling `setBaseTRMtx()` at real frame rate is necessary
but NOT sufficient. Without an explicit `model->calc()` **and**
`dusk::frame_interp::mark_live_this_frame()` call on that model's joints
THIS SAME real frame, `frame_interp`'s own draw-time substitution
(`resolve_replacement()`, inside `J3DShapeMtx.cpp`) overrides the fresh
transform with a stale once-per-tick-interpolated snapshot regardless of
how often `setBaseTRMtx()`/`calc()` themselves run — this is the exact
mechanism section 20 spent a full investigation root-causing for Link's
own hands/sword/shield, and round 1 of this fix rediscovered the same
gap by skipping the step rather than by a new bug.

**Fix, boomerang**: `refreshTrackedBoomerangMtxLive()` now explicitly
calls `->calc()` and `markModelJointsLive()` on `mp_boomModel`,
`mp_shippuModel`, and `mp_setboomEfModel` after
`applyTrackedKeepTransforms()`. Those three fields are private on
`daBoomerang_c`, so three small public getters
(`getBoomModel()`/`getShippuModel()`/`getSetboomEfModel()`) were added
rather than widening the class's public data surface further.

**Fix, fishing rod**: `refreshTrackedFishingRodMtxLive()` now marks EVERY
model the rod could plausibly touch — `rod_uki_model[15]`,
`unk_ring_model[6]`, `lure_model[5]`, `hook_model[2]`, `esa_model[2]`,
`ring_model`, `uki_model`, `uki_saki_model`, and
`rod_modelMorf->getModel()` — deliberately over-inclusive rather than
tracing exactly which subset `rod_control()` itself sets per kind/action
this frame, matching `markModelJointsLive()`'s own established "small
models, negligible extra cost, guessing an index wrong silently
reintroduces the bug for that index" reasoning. `dmg_rod_class`'s fields
are all `public` already (no `private:`/`protected:` anywhere in
`d_a_mg_rod.h`, confirmed by grep), so no new accessors were needed there
— just a new `#include "m_Do/m_Do_ext.h"` in `vr_link_visibility.hpp` for
`mDoExt_McaMorf`'s full type (needed to call `rod_modelMorf->getModel()`,
previously only forward-declared through this file's other includes).

**Built successfully** (RelWithDebInfo) — `d_a_boomerang.h`,
`vr_link_visibility.hpp`, `vr_main.cpp` recompiled, clean link, no new
warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up: hold
the boomerang (unthrown) and confirm it no longer stair-steps; cast/reel
the fishing rod and confirm the same, AND — since the rod fix's safety
rests on the snapshot/restore around `rod_tip_pos`/`field_0x6c8` being
sufficient (round 1's own audit) — pay particular attention to anything
tip-velocity-related (does casting still feel right, do fish still
bite/hook normally) in case some other, not-yet-found consumer of that
pair was missed. If EITHER still lags after this round, the next thing to
verify directly (not guess) is whether `daBoomerang_c::draw()`/
`dmg_rod_Draw()` are actually reached during a real VR eye pass at all —
section 20's saga eventually found `daAlink_c::draw()` was NOT (only ever
called via the legacy `fapGm_Execute()` path) — that was never
independently re-checked for these two actors specifically, just assumed
safe by analogy; a real debugger call stack (same conditional-breakpoint
technique section 20 used) would settle it directly. Double clawshot
remains deliberately deferred (unchanged from the section above).

**ROUND 3 (same day) — user confirmed both fixed ("theyre fixed now"),
but a NEW symptom surfaced: "the actual hook on the fishing rod is
jittery when im moving." Root-caused and fixed, built, NOT yet confirmed
in-headset.**

**Root cause**: `dmg_rod_Draw()` has a pre-existing, flatscreen-only
smoothing mechanism (`mLineInterpPrev`/`mLineInterpCurr`,
`dmg_rod_interp_callback()`) that snapshots the fishing LINE's rendered
vertex positions (`linemat.getPos(0)`) once per `dmg_rod_Draw()` call,
shifts prev←curr, and later blends prev/curr using
`dusk::frame_interp::get_interpolation_step()` — the fraction of progress
through the CURRENT SIM TICK. This was harmless before round 1/2's fix:
since the line's underlying position only changed once per 30Hz tick
either way, `dmg_rod_Draw()` calling this every real frame just captured
the SAME position repeatedly — prev and curr stayed nearly identical, so
the blend was a no-op. **Round 1/2's live-refresh broke this assumption**:
the line's position now changes every real frame (as intended), so this
same capture now snapshots ADJACENT REAL FRAMES into prev/curr instead of
tick boundaries — but the blend is still driven by `get_interpolation_step()`,
a value on the SIM-TICK clock, completely unrelated to the real-frame
cadence prev/curr are now actually sampled at. That clock mismatch is what
produced the jitter: the interpolated result jumps between two
already-live, nearly-adjacent samples using a stale-cadence alpha,
visibly worse the more the underlying position actually changes per
frame (i.e. worse while moving) — exactly the reported symptom.

**Fix**: gated both `if (dusk::frame_interp::is_enabled())` call sites in
`dmg_rod_Draw()` (LURE and UKI branches) with `&& !dusk::vr::
isRenderingToHeadset()` — skips the whole prev/curr-capture-and-
interpolation-callback-registration block in VR entirely. No smoothing is
needed there anymore: the position is already fresh every real frame
thanks to round 1/2's fix, so the un-interpolated `linemat.update()` call
immediately above this block (already using live data) stands as-is.
`dmg_rod_interp_callback()` itself needed no separate guard — it only
ever runs if registered via `add_interpolation_callback()`, which this
fix stops doing during VR. Needed a new `#include "dusk/vr/vr_main.hpp"`
under this file's existing `#if TARGET_PC` include block.

**Built successfully** (RelWithDebInfo) — `d_a_mg_rod.cpp` recompiled,
clean link, no new warnings.

**IN-HEADSET RESULT: round 3 did NOT fix it** — user: "It still jitters."
So the line-interpolation-cadence-mismatch theory, while a real and
correctly-fixed bug on its own terms, is not the (or not the whole) cause
of the hook jitter specifically. **User explicitly deprioritized this**
("That's ok, we need to fix the clawshots") rather than asking for a
4th round immediately — picked back up whenever fishing-rod work resumes.
Live leads for a future round, not yet tried: (a) the "rod model itself,
not just the line" possibility flagged above was never actually ruled
out — worth checking directly rather than assuming the line fix was
sufficient; (b) `hook_model[]`/`esa_model[]` (the actual hook/bait,
closest to what the user would be looking at as "the hook") are NOT
positioned inside `rod_control()` at all (confirmed via that function's
own field-write audit) — they're set later in `dmg_rod_Draw()` itself
from data `rod_control()` produces, a code path not yet read/traced this
session; since round 2's `markModelJointsLive()` pass marks them
over-inclusively but doesn't control WHEN/HOW their base transform is
actually computed, if THAT computation has its own tick-vs-real-frame
mismatch (same bug class, different location), marking them live wouldn't
fix it — this is the more likely remaining culprit than the already-fixed
line interpolation, and matches "the actual hook" being the user's precise
wording rather than "the line" or "the rod."

### Double clawshot / hookshot grip tracking — deliberately excluded by section 16, tackled 2026-08-12 (grip only, NOT the chain) — built, NOT yet confirmed in-headset

**User request**: "That's ok, we need to fix the clawshots" (deprioritizing
the still-unresolved fishing-rod hook jitter above). Double clawshot was
the one item section 16 explicitly scoped out as "not a simple joint
attach" — a real fire/retract state machine across both item joints,
swapping which physical model represents which hand.

**Investigation confirmed section 16's scoping was right, but found the
scope splits cleanly into a safe part and a genuinely separate, riskier
part**: `daAlink_c::setHookshotPos()` (`d_a_alink_hook.inc`) is a ~380-line
function (916-1294, much bigger than an initial partial read suggested —
cost a build error to discover, see below) covering BOTH:
1. **Hand-grip positioning** (top of the function): sets
   `mHeldItemModel`/`field_0x0710` (the two physical grip models — WHICH
   one represents left vs right hand swaps every call via `field_0x3020`/
   `getHookshotLeft()`, since double clawshot alternates hands) directly
   from `mpLinkModel->getAnmMtx(mLeftItemJntNo/mRightItemJntNo)` —
   **bypassing `getLeftItemMatrix()`/`getRightItemMatrix()` entirely**,
   unlike the fishing rod/boomerang/nocked-arrow consumers fixed earlier —
   this is why those two accessors' own VR-awareness fix never reached the
   clawshot at all. Also computes `mHeldItemRootPos`/`field_0x3810` (the
   chain's ROOT/near-hand anchor points, read by `getHsChainRootPos()`/
   `getHsSubChainRootPos()`) directly from those same grip models' base
   transforms.
2. **Real per-tick physics** (the rest of the function, not touched): shoot/
   fly/return state transitions, animation-frame counters, sound triggers,
   and the actual chain-length/target-collision simulation that positions
   `mHookshotTopPos` (the chain's FAR/grapple-side end) — genuine
   integrator state, same category this project has repeatedly treated as
   too risky to duplicate-call (bombs' `current.pos`, the fishing rod's
   tip-velocity pair).

**Fix, scoped to (1) only**: split the grip-positioning logic out into a
new `daAlink_c::applyTrackedHookshotGripTransforms()`, using
`getLeftItemMatrix()`/`getRightItemMatrix()` instead of the direct
`getAnmMtx()` reads — reusing the SAME already-VR-aware basis every other
held item already benefits from, rather than architecting new tracking
logic. `setHookshotPos()` now just calls this new function first, then
continues into its own real per-tick logic unchanged.
`vr_link::refreshTrackedHookshotMtxLive()` (`vr_link_visibility.hpp`),
called once per real frame from `tick()`, re-invokes
`applyTrackedHookshotGripTransforms()` and then explicitly `calc()`s +
`markModelJointsLive()`s both grip models — same proven shape as the
boomerang/rod fixes. Gated on `daPy_py_c::checkHookshotItem(getEquipItem())`
— the exact condition `setItemMatrix()` itself already uses to decide
whether to call `setHookshotPos()` at all.

**Deliberately NOT attempted this round: the chain itself**
(`hsChainShape_c::draw()`) — a genuinely separate, raw-immediate-mode GX
system (procedural chain-link segments computed and submitted via
`GXLoadPosMtxImm`/`simpleDrawCache()` every draw call, not through
J3DModel/`frame_interp`'s joint-matrix substitution mechanism at all) with
its OWN pre-existing flatscreen interpolation (`mHsChainInterp*` fields,
captured inside `daAlink_c::draw()` — itself of uncertain real-per-eye
reachability, same open question section 20 left for `daBoomerang_c::draw()`/
`dmg_rod_Draw()`). Given the fishing-rod hook jitter (three rounds, still
unresolved) suggests a real gap in this project's current understanding of
this exact bug class's timing, extending the same guesswork to a MORE
novel subsystem without new verification was judged too risky for this
round — matches section 16's own original reasoning for excluding
hookshot, just now narrowed to specifically the chain rather than the
whole item. The chain's near/root end should still visually follow the
grip to some degree even untouched (it reads the now-tracked
`mHeldItemRootPos`/`field_0x3810` whenever `setHookshotPos()` itself
runs), just not confirmed to update at real frame rate the way the grip
models now do.

**Build note**: first attempt failed to compile —
`static Vec const hookRoot = {0.0f, 0.0f, 23.5f};` had been declared as a
function-local inside what was assumed to be the END of `setHookshotPos()`
(around where the grip logic stopped), but the function actually continues
~380 lines further and references that same local further down (lines
~1090/~1292 in the current file) — a direct consequence of not having read
the function's real extent before splitting it. Fixed by hoisting the
constant to file scope (both functions can see it now) rather than
duplicating the declaration in two places. **Lesson reinforced**: before
splitting a function found via a partial read, grep for the function's own
closing boundary (next sibling function signature) to confirm the read
covered the WHOLE thing — this cost a wasted build cycle here, cheap to
avoid next time.

**Built successfully** (RelWithDebInfo, second attempt) —
`d_a_alink.h`, `d_a_alink_hook.inc`, `vr_link_visibility.hpp`,
`vr_main.hpp`/`.cpp` recompiled, clean link, no new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up: equip
the (double) clawshot and confirm the hand-grip models now track the real
controllers instead of floating at the old flatscreen position; fire it
and confirm sheathing/firing/retracting still behaves normally (none of
that state-machine logic was touched, but worth a sanity check given how
large and previously-unread the function turned out to be); separately
note whether the CHAIN visually looks acceptable as a secondary
observation, not a pass/fail target for this round — if it's reported
laggy/jittery, that's the deliberately-deferred piece, not a regression.

**IN-HEADSET RESULT: grips confirmed tracked ("They're tracked now"),
follow-up report: "the tips that actually fire lag behind."** Traced
directly (not guessed): each hookshot has TWO tip models —
`mpHookTipModel` (the PRIMARY/actively-aimed-or-flying tip) and
`field_0x0714` (the SECONDARY tip, resting on whichever grip isn't
currently active) — swapped via `changeHookshotDrawModel()`, same
swap-which-physical-model-represents-which-hand pattern as the grips.
Both remained entirely inside the still-tick-rate-only tail of
`setHookshotPos()` (past where the grip fix's extraction stopped), so
neither benefited from the live-refresh at all — matching the report
exactly.

**Split findings by risk, fixed only the safe half**:
- **`field_0x0714` (secondary, resting tip) — FIXED**: when
  `field_0x3024 == 0` (not mid a `cLib_chasePos()` "settle to new resting
  spot" — real per-tick integrator state, left untouched), its transform
  is a pure derivation: `field_0x0710`'s (now-tracked) base transform plus
  the same fixed `hookRoot` offset used everywhere else in this file — no
  side effects, directly analogous to the grip fix itself. New
  `daAlink_c::applyTrackedHookshotTipRestingTransform()`, called from
  `refreshTrackedHookshotMtxLive()` alongside the grip refresh, same
  `calc()`/`markModelJointsLive()` treatment.
- **`mpHookTipModel` (primary, actively-aimed tip) — NOT attempted,
  genuinely riskier**: traced its READY-mode (aiming-before-firing)
  positioning to lines ~1044-1093 of `setHookshotPos()` and found it is
  NOT a simple grip-offset derivation — its ORIENTATION comes from Link's
  aim direction/body angle (`mBodyAngle`, `mProcID`, target-lock via
  `mTargetedActor`/`getBodyAngleXAtnActor()`), and the exact branch taken
  also triggers a real one-shot sound effect
  (`seStartOnlyReverb(Z2SE_LK_HS_SHOOT)`) and writes real state
  (`field_0x3028`, `field_0x3828`) that other branches of the SAME
  function (the iron-ball-adjacent RETURN-mode chase-detection heuristic)
  read later. Duplicate-calling this at real frame rate risks either
  sound-spamming every real frame or corrupting that shared state,
  neither of which could be verified without in-headset testing this
  session didn't have time for. Deliberately left untouched — this is the
  literal continuation of the chain-rendering gap flagged above, not a
  new discovery: `mpHookTipModel`'s in-flight physics were always
  correctly out of scope, but its RESTING/READY position turned out to be
  entangled with gameplay/aim/sound logic in a way `field_0x0714`'s
  simpler resting case wasn't.

**Built successfully** (RelWithDebInfo) — `d_a_alink.h`,
`d_a_alink_hook.inc`, `vr_link_visibility.hpp` recompiled, clean link, no
new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up: check
whether the secondary (resting) tip now tracks correctly; if the PRIMARY
aimed tip is still the dominant complaint (most likely, since that's the
one visibly in front of you while aiming to fire), that's the deliberately
-deferred piece above — picking it up would mean either (a) carefully
replicating just the READY-mode angle/position derivation (lines
~1044-1093) into a new side-effect-free function the same way this round
did for the secondary tip, being careful to exclude the sound trigger and
either duplicate or reason out whether the `field_0x3028`/`field_0x3828`
writes are safe to duplicate, or (b) asking the user whether a genuinely
different design (the aim tip tracks the controller's own aim/pointing
direction directly, rather than deriving from Link's body-relative aim
angle at all) is preferable for VR specifically -- not yet discussed with
the user.

**IN-HEADSET RESULT: "The tip in the right hand is fixed, but not tip on
the left hand."** Not a new/separate bug — this is a report of the
already-known gap above (the primary tip, `mpHookTipModel`), just
identifying WHICH hand it currently happened to be on (the primary/
secondary grip assignment swaps via `field_0x3020`, so "left" here isn't
a stable identity — it's whichever hand `mHeldItemModel` currently maps
to). Prompted a closer re-read of `setHookshotPos()`'s exact brace
structure (previous round's risk assessment had been reasoning from a
partial read) — **found the primary tip's resting/READY-mode transform is
actually the SAME safe shape as the secondary tip's, not the riskier
one**: the earlier assumption that it derives from
`mDoMtx_stack_c::transS(mHookshotTopPos)`/`ZXYrotM(...)` (the
angle/physics rebuild) was wrong — that rebuild only happens in
`setHookshotPos()`'s SEPARATE "else" branch (actively flying, gated by
the SAME top-level `if (checkHookshotWait() || mItemMode == 2) {...}
else {...}` the wait-branch is also inside) — while
`checkHookshotWait()` is true (`mItemMode` is NONE or READY, which
by construction excludes both `mItemMode==2`, the one-tick "just fired"
transition with its sound effect, and all of SHOOT/FLY/RETURN), the tip's
final transform is simply `mHeldItemModel`'s (already tracked) base
transform plus the same fixed `hookRoot` offset — no angle math, no sound,
no shared-state writes needed at all.

**Fixed**: `daAlink_c::applyTrackedHookshotPrimaryTipRestingTransform()`
(`d_a_alink_hook.inc`) — gated on `checkHookshotWait()` alone (already
public, already exactly the right condition), directly mirrors
`applyTrackedHookshotTipRestingTransform()`'s shape one-for-one, just
using `mHeldItemModel` instead of `field_0x0710`. Wired into
`refreshTrackedHookshotMtxLive()` the same way, with its own
`calc()`/`markModelJointsLive()` pair. **Both tips are now covered** —
the risk assessment from the previous round undersold this one; it was
never actually as entangled as it looked from a partial function read.

**Built successfully** (RelWithDebInfo) — `d_a_alink.h`,
`d_a_alink_hook.inc`, `vr_link_visibility.hpp` recompiled, clean link, no
new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "That's fixed."
Both grips and both tips (primary and secondary, whichever hand each
currently maps to via `field_0x3020`) now track the real controllers.
**This closes out the clawshot/double-clawshot hand-tracking work** —
the only deliberately-untouched piece is the chain itself (both the
procedural rope rendering, `hsChainShape_c::draw()`, and the actual
in-flight grapple physics/`mHookshotTopPos` integration while SHOOT/FLY/
RETURN), unchanged from every round above. If the chain is ever reported
looking wrong, that's expected/known, not a regression — start with this
section's own scoping notes rather than re-investigating from scratch.

### Item-tracking status summary (as of 2026-08-12, end of session)

Quick reference for what's confirmed vs. still open across everything
extended past sword/shield/basic held items this session:
- **Confirmed working in-headset**: held items generally (bow, bottles,
  lantern, copy rod — from the original round), `getLeftItemMatrix()`/
  `getRightItemMatrix()` consumers generally, boomerang (held, unthrown),
  fishing rod's own base/grip tracking, clawshot/double-clawshot grips
  AND both tips.
- **Confirmed NOT fixed, deferred by user choice**: the fishing rod's
  hook/line jitter (3 rounds attempted, root cause still not fully
  understood — see that section's own "live leads for a future round").
- **Deliberately out of scope, not attempted**: bombs' full physics-rate
  tracking (partial fix only, documented gap), double clawshot's CHAIN
  rendering/flight physics (this section), fishing rod's actively-cast/
  reeled physics beyond the grip.

### World-space aim-point marker ("physical crosshair") — CONFIRMED WORKING IN-HEADSET 2026-08-12 (scope finalized)

**Goal** (explicit user request): "add a physical crosshair in the game
world to show where you are aiming all items." Scoped first via two
questions rather than guessed: visual style (**simple glowing dot/sphere**,
chosen over a surface-oriented ring decal or a billboarded cross symbol)
and item scope (**bow, slingshot, hookshot, boomerang** — everything
`checkSightLine()` already supports natively — chosen over a narrower
bow/slingshot-only scope matching the existing flatscreen reticle, or a
broader scope including bombs' arc-thrown trajectory, which would need
new prediction math).

**Reused the existing aim-point computation instead of writing new
raycasting logic**: `daAlink_c::checkSightLine()` (`d_a_alink.cpp`) is an
already-existing, general-purpose function that computes a world-space
sight/aim point via `dBgS_LinChk` line-collision checks, already
special-cased per item (hookshot/slingshot start from `mHeldItemRootPos`;
bow starts from the arrow actor; boomerang uses its own `mBoomerangLinChk`).
It's already the thing driving the existing flatscreen 2D reticle
(`daAlink_c::mSight`, a `daAlink_sight_c`/`daPy_sightPacket_c`-derived
sprite packet) — confirmed by reading each item's own call site:
`setBowSight()` (bow/slingshot), `setHookshotSight()` (hookshot, also
handles a locked-target case via `mHookTargetAcKeep`), and
`daBoomerang_c`'s own throw-aim code (`d_a_alink_boom.inc`) — ALL three
call `mSight.setPos(...)`/`mSight.onDrawFlg()`/`offDrawFlg()` on the SAME
shared `daAlink_c::mSight` object. This means one single flag+position
pair (`mSight.getDrawFlg()`, `mSight.getPosP()` — the latter already had
a public accessor, `getLineTopPosP()`) already covers all four scoped
items with zero per-item special-casing needed in new code. Added one
new public accessor, `daAlink_c::getAimSightVisible()`, mirroring
`getLineTopPosP()`'s existing shape (non-`const`, matching — `getDrawFlg()`
itself is non-`const`, caught by a build error on the first attempt).

**Rendering**: new `vr_render::drawAimCrosshair(const cXyz& worldPos)`
(`vr_stereo_render.hpp`), called once per eye from `vr_main.cpp`'s
`tick()` right after `cAPIGph_Painter()` (so the world's own geometry has
already been drawn this eye and Z-testing against it is meaningful) and
before `endEye()` — a real, proven-reachable per-eye call site this
session already used repeatedly, not a leap of faith. Deliberately NOT
head-locked/eye-space-fixed the way `drawHudBillboard()` is (that
function's own comment explains why identity-position-matrix vertices
make a panel head-locked "for free") — this needs to look like a real
object AT the aim point, so the world position is transformed into the
CURRENT eye's view space via `mDoMtx_multVec(view->viewMtx, ...)` first,
then drawn with an identity position matrix relative to THAT (same
"vertices already in eye-space" idiom, different anchor). A 16-segment
camera-facing triangle fan, no texture (raw vertex-color TEV pass-through,
`GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR)` — same minimal pattern used by
`d_home_button.cpp`'s own plain-colored-quad draw, adapted to add an
RGBA/alpha channel), warm white/yellow color, center vertex near-opaque
fading to fully transparent at the rim (a soft glow via per-vertex alpha,
no texture asset needed). **Z-test ENABLED, Z-write disabled** — the
"physical, not a HUD overlay" requirement means it must be occluded by
real geometry (a wall between you and the aim point should hide it);
write is disabled per the standard translucent-geometry convention (avoid
punching a depth-buffer hole other translucent draws would incorrectly
sort against). Fixed 8-unit world-space radius (~8cm at this project's
~100 units/metre scale) — deliberately NOT distance-compensated to a
constant screen size, since that would read as more HUD-like and less
"physical," the opposite of what was asked for; will naturally shrink
with distance like a real small object would.

**Deliberately excluded**: bombs (arc-thrown, `mSight` isn't driven for
them at all — would need new trajectory-prediction math, out of scope per
the scoping question above) and the fishing rod's cast (same reason).
Double clawshot's hookshot IS covered (per the scoping choice) but note
it's driven by `setHookshotSight()`'s OWN aim computation, independent of
this session's earlier hookshot hand-tracking fixes — no interaction
between the two features expected, but not something to assume without
seeing it in-headset.

**Build note**: first attempt failed — `getAimSightVisible()` was
initially declared `const`, but `daPy_sightPacket_c::getDrawFlg()` itself
is non-`const`; fixed by dropping `const` to match `getLineTopPosP()`'s
existing (non-`const`) convention on the same class.

**Built successfully** (RelWithDebInfo, second attempt) — `d_a_alink.h`,
`vr_stereo_render.hpp`, `vr_main.cpp` recompiled, clean link, no new
warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up: draw
the bow/slingshot/boomerang (aiming/charging, not just holding) and ready
the hookshot, and confirm a small glowing dot appears at the actual aim
point, correctly occluded by walls/terrain between the player and that
point, with correct stereo depth (should visually sit AT the surface it's
aiming at, not float in front of or behind it — the most likely thing to
be subtly wrong on a first pass, worth checking carefully). Also worth
sanity-checking the hookshot's locked-target case (`mHookTargetAcKeep`
branch in `setHookshotSight()`) shows the dot on the actual target actor,
not just terrain. Tune `kRadiusUnits`/`kGlowColor`
(`vr_stereo_render.hpp`) if the size/brightness feels off — untested
guesses, not derived from anything.

**IN-HEADSET RESULT, round 2: "Clawshot and boomerang render the dot but
not the bow, slingshot, or ball and chain. Can you make the dot red
too."** Two different situations, handled separately:

- **Ball and chain (iron ball)**: correct as-is, NOT a bug — it's a swung
  melee weapon with no ranged sight line at all. `checkSightLine()` is
  never invoked for it anywhere in the codebase (no `setIronBallSight()`
  or equivalent exists), so `mSight` is never driven for this item on
  flatscreen either. No fix made; explained to the user rather than
  guessing at a "swing indicator" feature nobody asked for.
- **Bow/slingshot — real bug, found by reading `setBowSight()`
  (`d_a_alink_bow.inc`) directly, unrelated to anything touched this
  session**: it calls `mSight.offDrawFlg()` UNCONDITIONALLY in BOTH its
  aiming and non-aiming branches — meaning the "Aiming Reticle" settings
  toggle (the base-game feature this whole `mSight` mechanism traces back
  to, section "World-space aim-point marker" above) currently does
  NOTHING even on flatscreen; the draw flag never turns on regardless of
  the setting. `mSight`'s POSITION is still updated correctly whenever
  aiming (`mSight.setPos(&sight_pos)` runs fine, only the flag is
  affected), so the fix doesn't touch `setBowSight()`/flatscreen code at
  all (deliberately, per this project's general caution around unrelated
  fixes) — instead, `getAimSightVisible()` (`d_a_alink.h`) now falls back
  to independently re-deriving the same "is aiming" condition
  (`checkBowAndSlingItem(mEquipItem) && checkBowChargeWaitAnime() &&
  !dComIfGp_checkPlayerStatus0(0, 0x200000)` — the exact condition
  `setBowSight()`'s own `if` already gates on) whenever `mSight`'s own
  flag reads false, so the VR crosshair works regardless of that
  flatscreen bug (or the setting's state) without depending on either.
- **Color**: `kGlowColor` (`vr_stereo_render.hpp`) changed from warm
  white/yellow to red (`{235, 30, 30, 220}`), same near-opaque-center/
  transparent-edge falloff shape, unchanged otherwise.

**Built successfully** (RelWithDebInfo) — `d_a_alink.h`,
`vr_stereo_render.hpp` recompiled, clean link, no new warnings.

User's response to the `setBowSight()` flatscreen bug report: "I will fix
it later" — explicitly deferred, own code to fix in their own time, not
something for a future VR-mod session to pick up. Not blocking: the VR
crosshair fix above works around it independently (re-derives the aiming
condition itself rather than relying on `mSight`'s flag), so it functions
correctly in-headset regardless of whether/when that flatscreen fix lands.

**CONFIRMED WORKING IN-HEADSET (round 3)** — user tested and reported bow
and slingshot now show the red dot correctly (the `getAimSightVisible()`
fallback fixed it). Two follow-up items raised ("not the ball and the
rod") turned out to be scope questions, not bugs, and were resolved by
asking rather than guessing:
- **Ball and chain (iron ball)**: reconfirmed correct as-is — it's a
  swung melee weapon with no ranged sight line at all, on flatscreen
  either (`checkSightLine()`/`mSight` is never invoked for it anywhere in
  the codebase). Nothing to add.
- **Fishing rod's cast**: still deliberately out of scope. Asked the user
  directly whether to build new arc-trajectory prediction for it (its
  cast isn't a straight sight line the way the other four items are, so
  it can't just reuse `mSight` the way this feature does for everything
  else) — **user chose to leave it out**, confirming the original scoping
  decision rather than expanding it.

**This closes out the aim-point marker feature** — final scope is bow,
slingshot, hookshot, and boomerang, all confirmed working in-headset with
correct stereo depth/occlusion (no issues reported on either), red glow
color as requested. Ball-and-chain has no applicable concept of an aim
point; fishing rod's cast remains a known, explicitly-declined gap, not
an oversight, should anyone ask about it again later.

**UPDATE 2026-08-13 — Dominion Rod ("Copy Rod" internally,
`d_a_alink_copyrod.inc`/`dItemNo_COPY_ROD_e`) CONFIRMED WORKING IN-HEADSET
with zero new code.** User
asked to "add a crosshair for the rod and ball" — clarified via question:
"rod" meant the Dominion Rod specifically (not the fishing rod, which
stays explicitly out of scope per the paragraph above), and "ball"
(ball-and-chain) was confirmed to have no aim concept and was dropped
from scope, same conclusion as the paragraph above already reached
independently.

Traced the Dominion Rod's own aim code before writing anything:
`daAlink_c::procCopyRodSubject()` (`d_a_alink_copyrod.inc:281`) calls
`setBodyAngleToCamera()` — the SAME shared aim-input function this
session's controller-pointing-aim feature already made VR-aware, not
item-specific — and if that returns true, calls `setCopyRodSight()`
(line 252), which runs `checkSightLine(getCopyRodBallDisMax(), &sight_pos)`
and writes into `mSight` via `mSight.setPos()`/`mSight.onDrawFlg()` —
**unconditionally**, unlike bow's known flatscreen draw-flag bug that
needed a fallback in `getAimSightVisible()`. Since the VR crosshair draw
call (`vr_main.cpp`'s per-eye loop) is item-agnostic — it only checks
`link->getAimSightVisible()`/`getLineTopPosP()`, never which item is
equipped — the Dominion Rod should already show the crosshair the same
way bow/slingshot/hookshot/boomerang do, with no code changes at all.

No code was written for this — the existing item-agnostic crosshair
mechanism just worked once `mSight` was being driven correctly, exactly
as traced. User tested and confirmed "It works." Closes out this
follow-up; the aim-point marker feature's scope is now bow, slingshot,
hookshot, boomerang, and Dominion Rod — all five sharing the same
`mSight`/`checkSightLine()` mechanism, all confirmed in-headset.

### Controller-pointing item aim ("aim with the controllers and where you point them") — CONFIRMED WORKING IN-HEADSET 2026-08-12

**Goal** (explicit user request): first-person item aiming (bow, slingshot,
hookshot, boomerang) should follow the real controller's physical pointing
direction instead of stick/gyro/mouse/touch input.

**Found the right insertion point by reading code, not guessing**:
`daAlink_c::setBodyAngleToCamera()` (`d_a_alink_link.inc`) is the ONE
shared function every aim-capable item already funnels through — bow,
hookshot, boomerang, copy rod, ball-and-chain, plus the general
horse/wolf/swim "subjective" look modes, all call it (confirmed via a
grep of every call site). It already computes `shape_angle.y` (aim yaw)
and a local `sp8` (feeds `mBodyAngle.x`, aim pitch) from whichever input
method is active — stick (`checkInputOnR()`), gyro, mouse, or touch —
gated by the same pre-existing `checkAimInputContext()`/camera-attention
check for all of them. This is the same function the world-space aim-dot
feature (previous section) already reads the RESULT of via `mSight`/
`checkSightLine()` — so pointing the controller here changes what the dot
marks too, for free.

**Which hand**: right hand, unconditionally — asked the user directly
(varies-per-item was the alternative, since bow/hookshot/boomerang can
each attach to either hand depending on state) rather than guessing;
**user chose right-always** for simplicity/predictability.

**Direction source**: reuses `buildHandMtx()`'s own already-fully-
confirmed-working right-hand calibration (`applyStaticCorrection(
right_hand_cal::kLocalForward)` rotated by the live, smooth-turn-adjusted
grip quaternion, section 12) rather than the separate OpenXR aim-pose
action — deliberately, since aim pose was already found to be
runtime-dependently unreliable in this exact codebase (section 12's "aim
pose data itself is broken on whatever runtime this was tested on"),
while grip pose has never shown that problem. This is also the exact
direction the player already sees their tracked hand mesh pointing, so
aiming should feel visually consistent with the hand rather than aiming
from some invisible, separately-calibrated reference.

**New code**:
- `vr_link::computeControllerAimForward(rightControllerPoseXR, yawRad)`
  (`vr_link_visibility.hpp`) — returns a world-space (game-convention, no
  axis flip needed) direction vector. Deliberately returns a raw vector,
  not an angle, mirroring `computeHeadWorldForward()`'s own shape (the
  cM_atan2s conversion happens in vr_main.cpp, keeping this
  coordinate-math file free of engine-angle-convention specifics).
- `vr_main.cpp`'s `tick()`: computed once per frame right next to the
  existing `g_headMoveAngleS` block (same `rightPose`/
  `getSmoothTurnYawRad()` inputs already available there). Yaw:
  `cM_atan2s(fwd.x, fwd.z)`, matching `g_headMoveAngleS`'s own established
  convention. Pitch: `cM_atan2s(fwd.y, horizontalLength)` — matches an
  existing precedent found elsewhere in this codebase for deriving
  `mBodyAngle.x` from a direction vector
  (`d_a_alink_guard.inc`'s `cM_atan2s(dmg_vec->y, dmg_vec->absXZ())`)
  rather than inventing a new sign convention from scratch. **Not
  independently verified against a real in-headset test** — same
  "flip the sign if it reads backwards" caveat every other rotation-sign
  guess in this project has needed at least once before landing.
  Exposed via `dusk::vr::getControllerAimAngles(s16*, s16*)`.
- `d_a_alink_link.inc`'s `setBodyAngleToCamera()`: new
  `isRenderingToHeadset()` branch, checked first, ABSOLUTE-assigns
  `shape_angle.y`/`sp8` from the controller angles instead of applying an
  incremental delta — deliberately replaces stick/gyro/mouse/touch input
  entirely for this function while in VR (guarded the three existing
  input blocks with `!isRenderingToHeadset()`) rather than layering a
  delta on top of an absolute pointing direction, which wouldn't compose
  sensibly. Sits inside the same pre-existing outer
  `dComIfGp_checkCameraAttentionStatus(field_0x317c, 0x10)` gate every
  other input method already uses, so scoping to "actually in an
  aim/subjective context" needed no new condition.

**Known, accepted side effect, not separately verified**: since
`setBodyAngleToCamera()` is shared by the general horse/wolf/swim
"subjective look" states too (not just weapon aim), those also now follow
the controller's pointing direction — in practice this was very likely
already the ONLY functional input for them in VR anyway (the right stick
was unbound from aiming for VR smooth-turn back in section 15, and
gyro/mouse aim are unlikely to be enabled by someone playing in a
headset), so this is expected to read as "now it works" rather than a
behavior change, but hasn't been tested specifically for the non-weapon
subjective-look cases.

**Built successfully** (RelWithDebInfo) — `vr_link_visibility.hpp`,
`vr_main.hpp`/`.cpp`, `d_a_alink_link.inc` (via `d_a_alink.cpp`)
recompiled, clean link, no new warnings.

**ROUND 1 in-headset result: real axis-confusion symptom found, first fix
attempt rejected without testing, source swapped entirely — built, NOT
yet re-tested.** User report: "the yaw is rotated 90 degrees to the
right" and, critically, "rotating on the yaw axis made the pitch and roll
axis switch places." The first finding alone (fixed 90° yaw offset) would
have been a simple constant to subtract — but the second finding is the
signature of a genuinely wrong SOURCE vector, not a wrong constant: this
project already has a hard-won, explicit standing lesson for exactly this
symptom (CLAUDE.md's permanent constraints, and section 12's full
algebraic proof) — **a uniform correction (matrix column swap OR a
constant angle offset) cannot change which physical rotation axis feeds
which computed output, only the resting orientation.** A first attempt at
exactly that kind of fix (subtract 90° from yaw, negate pitch) was
written and built, then **discarded without even sending it back for
testing** once the second symptom made clear it was the wrong class of
fix — no point spending the user's headset time confirming something the
math already rules out.

**Real fix**: switched `computeControllerAimForward()` from the grip pose
+ `right_hand_cal` mesh calibration (tuned for how the tracked hand MESH
should visually look, never verified as "the direction a player naturally
points this controller") to OpenXR's own **aim pose** directly — spec-
defined specifically as "the direction the user would point the
controller to indicate a target" (local -Z axis), independent of any
mesh calibration. `rightAimPose` was already located every frame
(existing infrastructure from the section 12 hand-rotation-calibration
saga, kept in the tree specifically because it "could be reused directly"
later). Local -Z + `rotateVecByQuat()`-direct mirrors
`computeHeadWorldForward()`'s own already-proven-correct convention for
the HMD exactly, rather than inventing a new one.

**Known risk carried forward, not newly introduced**: aim pose was found
runtime-dependently unreliable in this exact project once before (section
12, one Virtual Desktop capture showed a physically-impossible
world-frame-fixed grip/aim relationship; a LATER capture on the same
runtime was fine). That finding was about the RELATIVE grip/aim
relationship across many samples, not aim pose's own absolute quality in
general — and this feature uses aim pose directly, not derived from grip
— but it's not proven reliable for this exact use yet either. If yaw/pitch
come back scrambled or erratic (as opposed to a clean, describable
offset) after this change, aim-pose unreliability on the current runtime
is the next thing to suspect, not another correction attempt.

**Built successfully** (RelWithDebInfo) — `vr_link_visibility.hpp`,
`vr_main.hpp`/`.cpp` recompiled, clean link, no new warnings.

**ROUND 2 in-headset result: axis confusion fully gone, plain inverted
pitch left — fixed, built, NOT yet re-tested.** User confirmed: "The axis
are right but pitch is inverted." The aim-pose switch fully resolved the
axis-mixing symptom (yaw/pitch each now respond only to their own
physical motion) — leaving just a clean sign flip, unlike round 1's
rejected pitch negation (which was discarded specifically because it was
riding on top of a confirmed-wrong source vector, not because negation
itself was the wrong idea). Negated `g_controllerAimPitchS` in
`vr_main.cpp`. This is exactly the class of fix a plain sign flip IS
capable of — the "don't fix axis-mixing with a uniform correction" lesson
was never an objection to sign flips in general, only to using one to
paper over a wrong source.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` recompiled,
clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "pitch is fixed
now, aim dot tracks too." Yaw, pitch, and the world-space aim dot (earlier
section, reads the same `mSight`/`checkSightLine()` result this feature
now drives) all confirmed correct together. **This closes out the
controller-pointing aim feature.**

Worth remembering as a reusable lesson alongside section 12's original
one: the axis-confusion symptom ("yawing swapped pitch/roll") was
correctly diagnosed from a single in-headset report, without needing to
build and test a doomed fix first — the fix that was written and
discarded before ever reaching the headset (a uniform angle-space
correction) would have failed for the same provable reason section 12's
"column swap" lesson already covers. Recognizing the SAME underlying
class of symptom is what made this a same-day fix instead of another
multi-round saga like section 12's original hand-rotation calibration.
The actual fix (switching source from grip pose + mesh calibration to
OpenXR's own aim pose) took one round once framed correctly; the final
pitch sign flip took one more, and was safe specifically because it was
applied on top of an already axis-clean source, not a substitute for
fixing the source.

### Lantern (Kantera) VR physics/tracking — swing physics, glow halo, AND flame particle position ALL CONFIRMED FIXED (flame particle resolved 2026-08-13, see the box after part 3 below)

**User request, three parts, tackled in order**: (1) "lantern physics are
spazzing out when I hold it" — swing-simulation instability; (2) once
fixed, "the flame is where the original position is" — a supplementary
glow-halo mesh not tracking; (3) still open — the ACTUAL flame particle
VFX still doesn't track the tracked/held lantern at all. **User paused
here for the night ("finish it tomorrow") mid-round-2 of part 3** — read
this box before touching kantera/lantern code again.

**Part 1 — CONFIRMED FIXED.** `daAlink_c::kandelaarModelCallBack()`
(`d_a_alink_kandelaar.inc`) is a spring/lag flame-SWING simulation
(`field_0x3618`'s `*0.9` decay term), tuned for this game's original
~30Hz call rate. This session's earlier held-item live-refresh work
(`refreshTrackedHeldItemMtxLive()`) made `mpKanteraModel`'s `calc()` (and
therefore this joint callback) run at real VR framerate (72-90Hz)
instead, fed by real, high-frequency/-amplitude controller motion instead
of smooth capped animation — destabilizing the simulation. **Fix**: while
`isRenderingToHeadset()`, force the computed swing angles (`var_r28`/
`var_r27`) to zero — position/yaw tracking (`sp44`, `var_r29`) untouched,
so the lantern still tracks correctly, it just never tilts/springs.
User-confirmed stable in-headset.

**Part 2 — CONFIRMED FIXED.** `mpKanteraGlowModel` (a supplementary soft
light-halo mesh, separate from the lantern body) had its own base
transform set from `mKandelaarFlamePos` ONLY inside `setItemMatrix()`'s
once-per-sim-tick legacy block (`d_a_alink.cpp`) — never touched by the
VR live-refresh, so it stayed visually stuck once the lantern body itself
started tracking at real framerate. **Fix**: `refreshTrackedHeldItemMtxLive()`'s
kantera block (`vr_link_visibility.hpp`) now also copies the (already
correctly live-tracked, thanks to part 1's calc()-triggered callback)
`mKandelaarFlamePos` into `glowModel`'s own base transform every real
frame, `calc()`s it, and `markModelJointsLive()`s it — same shape as
every other held-item live-refresh this session. New public accessors
added for this: `daAlink_c::getKanteraGlowModel()`,
`getKandelaarFlamePosRaw()` (`d_a_alink.h`). User-confirmed: "it's lit
up."

**Part 3 — NOT FIXED, two rounds in, real progress but not resolved.**
The user's precise follow-up report clarified the glow-halo fix (part 2)
was NOT what they meant by "the flame" — the actual flame VFX (what a
player would call "the flame") is a SEPARATE JPA particle effect
(`ID_ZI_J_KANTERA_FIRE`/`_SWINGFIRE`), spawned/kept alive by
`daAlink_c::setLight()` (`d_a_alink.cpp`, called from the same
once-per-sim-tick legacy update block as `setItemMatrix()`), tracked
independently via a persistent emitter id (`field_0x31c4`, new accessor
`getKandelaarParticleId()`).

**Round 1 fix (built, tested, ZERO visible change)**: added a
`JPABaseEmitter::setGlobalTranslation()` call to the same VR live-refresh
block, using the (confirmed correct) live `mKandelaarFlamePos`, once per
real frame. No effect reported at all — not "still laggy," genuinely
stuck exactly where it was before the fix.

**Diagnostic round (real capture obtained, real findings, ruled things
IN not just out)**: added throttled `OutputDebugStringA` logging at two
points — inside `kandelaarModelCallBack()` itself
(`[dusk::vr::kandelaar]`) and in the VR refresh block
(`[dusk::vr::kanteradiag]`). A real in-headset capture
(`C:\Users\joeyw\Downloads\log.txt`) conclusively showed:
- `attach=1 attached=1` throughout — the tracking branch is engaged.
- `mKandelaarFlamePos` genuinely tracks `kanteraPos` (the tracked lantern
  body) in lockstep across the whole capture, including large jumps —
  **the position DATA is correct**, not the bug.
- The particle id stabilizes early (`34`→`35`, then constant) with a
  valid, non-null emitter pointer throughout.
- So: correct position, correct live emitter, `setGlobalTranslation()`
  called every real frame with that correct position — and the user
  STILL sees it stuck. The bug is downstream of position data, in how
  (or whether) the particle system's actual rendering picks up
  `setGlobalTranslation()`'s effect.

**Investigated `dPa_control_c::set()` (`d_particle.cpp:1824`, what the
LEGACY `setLight()` path calls) directly, looking for what it does that
our fix doesn't**: found it calls `pJVar4->playCalcEmitter()` and
`pJVar4->playCreateParticle()` on every legacy invocation — LOOKED like
a real "advance simulation" step our fix might be missing, and was the
leading theory heading into round 2. **Checked the actual implementation
in `JPAEmitter.h` before acting on this theory (correctly, this time) --
both are trivial status-flag clears** (`playCalcEmitter()` just clears
`JPAEmtrStts_StopCalc`; `playCreateParticle()` clears `JPAEmtrStts_StopEmit`)
-- NOT a synchronous simulation step. **This theory was NOT acted on** --
recognized as likely a red herring before writing another blind fix,
given particle rendering is independently proven to work correctly at
real VR framerate elsewhere in this project (sections 5/10's whole
kagerou/heat-wave investigation depends on it).

**Round 2 diagnostic added (built, NOT yet captured — this is where to
resume)**: `[dusk::vr::flamediag]`, logging the emitter's own status
flags (`StopCalc`/`StopEmit`/`StopDraw`), live `getParticleNumber()`, and
a `getGlobalTranslation()` READBACK immediately after our
`setGlobalTranslation()` call -- to settle, with direct evidence rather
than more JPA-internals guessing: (a) is this emitter actually
active/unpaused, (b) does it have any live particles to reposition at
all (if `particleNum=0` consistently, "the flame" the user sees isn't
this emitter, and the search needs to widen to some OTHER effect/model
entirely), (c) does our write to translation actually stick moment-to-moment
(rules out something else clobbering it same-frame).

**Concrete next step, tomorrow**: launch under the debugger, hold the
lit lantern, move your hand around, capture a fresh log, and grep for
`[dusk::vr::flamediag]` (the `[dusk::vr::kandelaar]`/`[dusk::vr::kanteradiag]`
logs from round 1 are still in the tree too, harmless, still useful for
cross-referencing position if needed). Branch on what it shows:
- `particleNum` consistently 0 → wrong emitter entirely; "the flame"
  visual must be coming from somewhere else (worth re-checking the
  `checkKandelaarSwingAnime()` swing-variant particle, or reconsidering
  whether there's a THIRD position source not yet found -- e.g. search
  for any other `mKandelaarFlamePos` reader, or any other
  `dComIfGp_particle_set`/`dComIfGp_particle_getEmitter` call anywhere
  near kantera code that wasn't caught by the earlier
  `field_0x31c4|Kantera|KANDELAAR|mKandelaarFlamePos` grep of
  `d_a_alink_effect.inc`).
- Any `stop*` flag set unexpectedly → found a real pause mechanism to
  investigate (what sets it, and whether it needs clearing from VR code
  too).
- Readback doesn't match what we just wrote → something else really is
  clobbering it same-frame; check write ordering against `setLight()`'s
  own legacy call more carefully (may need to confirm which runs LAST
  each real frame that also happens to coincide with a sim tick).
- Everything looks correct (unpaused, particles present, readback
  matches) despite the user still seeing it stuck → the bug is almost
  certainly in the actual GPU DRAW submission path for this specific
  particle system not reading live emitter state the way it should, at
  which point this needs the same tool section 20's saga eventually
  reached for (a real debugger call stack on the particle draw call, or
  a RenderDoc capture), not another log round.

**Part 3 — FULLY RESOLVED 2026-08-13 (later session, resumed from the pause
above). Read this box if picking up flame/particle VR-tracking work again;
the rounds above are historical trail, not current status.**

Picked up exactly where the pause left off: captured a fresh
`[dusk::vr::flamediag]` log per the "concrete next step" above. Result:
`stopCalc=0 stopEmit=0 stopDraw=0`, `particleNum` in the 3-14 range, and
`readback` exactly matching what `setGlobalTranslation()` had just written
— i.e. the LAST branch in the pause box's own checklist ("everything looks
correct... the bug is almost certainly in the actual GPU draw submission
path"). Rather than reach for a debugger/RenderDoc immediately, cross-
referenced this capture's `[dusk::vr::jpacalc]` lines (added in an earlier,
already-in-tree round 3 diagnostic that was never actually analyzed
carefully before) against the `[dusk::vr::kandelaar]` per-real-frame call
counter, and found the real root cause directly from that comparison: only
~10-11 `calcWorkData_c()` calls occur per 30 real VR frames — a clean,
consistent ~3:1 ratio across the whole capture, i.e. this emitter's ENTIRE
particle simulation (`JPAResource::calc()`, including the `calc_p()` call
that applies our tracked position via status `0x20`) runs at ~30Hz sim-tick
rate, not real VR framerate as an earlier round's diagnostic comment had
(wrongly) concluded.

**Traced why**: `dComIfGp_particle_calc3D()` (`d_s_play.cpp`, the function
that runs the ENTIRE particle simulation for every 3D emitter in the game)
is called from `dScnPly_Draw()` — which, despite its name, is the exact
same legacy call site section 20's whole hands/body/sword saga already
root-caused for `daAlink_c::draw()`: reachable only via `fapGm_Execute()`'s
once-per-sim-tick path, never from the real per-eye VR draw. So the
status-`0x20` fix from an earlier round could only ever take effect once
every ~3 real frames — and `mark_live_this_frame()` (also already in
place) correctly stopped the draw from substituting a stale *interpolated*
snapshot, but did nothing about the underlying `mPosition` itself only
being recomputed at 1/3 the real rate. Net effect read as fully "stuck,"
not just choppy, since short observation windows rarely happened to land
on one of the ~30Hz refresh moments.

**Fix**: stopped waiting for `calc_p()` to run at all. The VR code now
writes each alive/child particle's `mOffsetPosition`/`mPosition` directly,
every real frame — preserving whatever local wobble offset the last real
`calc_p()` tick gave that particle (`mPosition - mOffsetPosition`, e.g.
flicker/gravity/velocity drift) but re-rooting it at the fresh tracked
position. This can't drift out of sync with the eventual real `calc_p()`
tick either, since that tick computes the identical `mOffsetPosition` from
`mGlobalTrs` via the same status-`0x20` branch. `setGlobalTranslation()`
and status `0x20`/`mark_live_this_frame()` were all kept (harmless, still
correct for whenever a real sim tick does land, and for newly-spawned
particles).

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "Lantern is
fixed," after already having confirmed parts 1 (swing physics) and 2 (glow
halo) in the earlier session. All three parts of this investigation are
now closed.

**Diagnostic scaffolding removed** (per this project's normal practice, now
that all three parts are confirmed fixed): `[dusk::vr::kandelaar]`
(`d_a_alink_kandelaar.inc`, including its now-unneeded `#include <cstdio>`),
`[dusk::vr::kanteradiag]`/`[dusk::vr::flamediag]` (`vr_link_visibility.hpp`),
and `[dusk::vr::jpacalc]`/`[dusk::vr::jpadrawfunc]` plus the
`g_duskVRKanteraFlameEmitterPtr` cross-translation-unit identifier and the
`jpaDrawFuncName()` helper it fed (`libs/JSystem/src/JParticle/JPAResource.cpp`,
`vr_main.cpp`, `vr_link_visibility.hpp` — including that file's now-unneeded
`<cstdio>`/`<windows.h>` includes) have all been removed. Rebuilt clean
(only `vr_main.cpp` needed recompiling after the JPAResource.cpp/
d_a_alink_kandelaar.inc changes had already been picked up by an earlier
build in the same session) — the real fixes (swing-angle zeroing, the glow
model's per-frame copy, and the direct particle-position rewrite) are all
still in place.

**Reusable lesson**: an earlier round's diagnostic comment had concluded
"`calcWorkData_c()` runs at real VR framerate" from a short observation
window — that conclusion was wrong, and nobody re-checked it against a
longer, cleaner capture until this session. When a diagnostic log's own
inline comment asserts a conclusion, treat it as a hypothesis to re-verify
against fresh data before building further fixes on top of it, the same
way this project already treats any other unverified assumption — a stale
"confirmed" comment is just as capable of misleading a future session as
no comment at all.

### Hookshot/clawshot flight + hanging — camera falls back to first-person (head-joint anchor) — CONFIRMED FIXED IN-HEADSET 2026-08-13

**Goal** (explicit user request: "make the camera first person when you
are in the air being pulled by the clawshot, and when you are hanging on
to a clawshot target"). Same underlying gap as swimming/crawling/vine-
climbing (section 23 and its follow-ups): `isFirstPerson()` already
permits first-person here on its own — hookshot flight/hanging is not a
`dEvt_control_c` event (confirmed by reading `d_a_alink_hook.inc` end to
end: no `event_regist`/camera-mode/demo-actor call anywhere in it), so it
falls straight through `isFirstPerson()`'s "no event — ordinary gameplay"
branch. This was never a first/third-person GATING problem — it's the
same comfort-anchor-CALIBRATION gap the swim/crawl/vine fixes already
addressed: the core anchor's standing-height offset (section 23) doesn't
mean anything while Link is horizontal mid-air on the end of a chain, or
hanging off a wall/ceiling at an odd angle.

**The states covered**: hookshot has no single `MODE_FLG` bit either (same
situation as crawling) — a sequence of dedicated `daAlink_PROC` states
instead (`d_a_alink.h`). New `isHookshotAirborneOrHanging()` helper
(`vr_link_visibility.hpp`, same shape as `isCrawling()`) covers
`PROC_HOOKSHOT_FLY` ("being pulled through the air," the user's own
wording) and `PROC_HOOKSHOT_ROOF_WAIT`/`_WALL_WAIT` ("hanging on to a
clawshot target" once attached), plus their `_SHOOT`/`_BOOTS` siblings
(firing the second clawshot at another point, or standing in iron boots
on the ceiling — still attached to the original point the whole time, so
grouped in rather than dropping back to the core anchor mid-action).
Deliberately EXCLUDED: `PROC_HOOKSHOT_SUBJECT` (aiming, before firing —
Link is still standing normally) and `PROC_HOOKSHOT_MOVE` (traced via its
call sites — this is for dragging a grabbed OBJECT toward Link, not Link
himself flying, so his own pose never leaves the normal standing case
either).

**Fix**: `computeRawEyeAnchor()`'s existing swim/crawl/vine fallback
condition (`vr_link_visibility.hpp`) now also falls back to the raw,
animation-driven head-joint anchor (`getSubjectEyePos()`) — the base
game's own eye position, correct by construction for whatever pose the
hookshot animation puts Link in, no separate calibration needed — while
`isHookshotAirborneOrHanging()` is true. Exactly the same fix shape as
every other entry in this fallback list; no new mechanism invented.

Built successfully (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "camera feels
right." Not separately broken out by sub-case (roof vs. wall, double
clawshot, iron boots on the ceiling) — if any specific one is ever
reported still off, `isHookshotAirborneOrHanging()` is where to check
first, but the general mechanism is confirmed working.

### Right-hand thrust gesture → shield bash (R) — CONFIRMED WORKING IN-HEADSET 2026-08-13

**Goal** (explicit user request: "if you thrust the right controller it
should press R basically, as R is shield bash"). `g_rightSwing`
(`vr_main.cpp`) — dormant infrastructure from a 2026-08-03 right-hand
SWORD-swing draft that was deferred and left in the tree specifically so
it wouldn't need re-deriving later — repurposed here for its actually-
requested use, renamed `g_rightThrust`. Seeded with `g_leftSwing`'s own
FINAL, six-round-tuned values (`triggerSpeed=2.2`, `resetSpeed=0.4`,
`minSwingDistance=0.12`, `cooldownSec=0.0`) rather than the class's
untested defaults — a thrust and a sword swing are the same character of
gesture (a deliberate fast hand motion) on the same hardware, so reusing
already-proven-good numbers is a much better starting point than guessing
blind a second time. Explicitly untested for THIS gesture though — a stab
may want different tuning (shorter `minSwingDistance`, different
`triggerSpeed`) than a full swing; retune with real data if reported off,
same workflow the sword gesture's six rounds already established.

**Traced the actual game mechanic before writing anything** (not assumed):
shield bash fires via `daAlink_c::spActionTrigger()` →
`itemTriggerCheck(BTN_R)` → `mItemTrigger & BTN_R`, and that bit is only
ever set by `mDoCPd_c::getTrigLockR(PAD_1)` — a genuine RISING-EDGE
detector (0→1 transition only, not a hold check). This is why the fix
shape had to be different from the sword's B-latch fix, not just a copy of
it: raising the shield (left squeeze, added earlier this session) already
holds `PAD_TRIGGER_R` CONTINUOUSLY the whole time it's up — and the
natural way to actually want a shield bash is WHILE already blocking. If R
is already sitting at 1 from the squeeze hold, a thrust asserting R again
produces no transition at all from the game's point of view, so
`spActionTrigger()` would never see an edge no matter how the trigger
event itself is latched.

**Fix** (`vr_main.cpp`): on a thrust trigger, force a brief RELEASE window
first (`kThrustForceReleaseSec = 50ms`, comfortably longer than one ~33ms
30Hz sim-tick period — guarantees a real 0 sample reaches at least one sim
tick even if squeeze is currently holding R up), THEN force a brief HOLD
window (`kThrustHoldSec = 100ms`, mirrors the sword fix's own latch
directly) — producing a genuine, detectable 0→1 pulse regardless of the
left hand's current squeeze state, which then reverts to whatever squeeze
naturally wants. `padStatus.triggerRight`'s analog value during the forced
hold is set to full (255) rather than reflecting `leftSqueeze` (which could
be 0 if the player thrusts without squeezing at all).

Built successfully (RelWithDebInfo) — only `vr_main.cpp` recompiled,
clean link, no new warnings.

**CONFIRMED WORKING IN-HEADSET** — user tested and reported "works good."
The rising-edge pulse (force-release then force-hold) correctly produces
a real shield bash both while already blocking and from a cold thrust,
first attempt, no retuning needed. No directional/forward-only filtering
was added — fires on any sufficiently fast right-hand motion, same "start
simple" shape as the sword gesture's own first version — if it's ever
reported firing on unrelated fast right-hand motion (general gestures,
aiming), a forward-direction dot-product check against the controller's
own aim-forward vector (`vr_link::computeControllerAimForward()`, already
used for controller-pointing aim) is the natural next refinement, not
needed yet.

### Camera anchor going above/below Link after loads/cutscenes (Epona mount/dismount, shop exit) — CONFIRMED FIXED IN-HEADSET 2026-08-13

**Symptom** (user report, "last fix before releasing the mod"): "the camera
going above or below Link after certain loads or cutscenes. Sometimes I'll
get off epona and the camera will be on the ground. Sometimes I'll exit a
shop and it will be underground. Sometimes I'll get on epona and it will
be too high."

**Investigation was code-reading only** (no diagnostic build/log round this
time — the reasoning below is strong enough on its own merits, given how
directly it's supported by reading `setBodyPartPos()` and
`isFirstPerson()`'s own already-documented comments, but per this
project's own standing practice, treat it as unconfirmed until tested).

**Two separate root causes found, both inside section 23's core-anchor
comfort system** (`vr_link_visibility.hpp`'s `computeRawCoreAnchoredEye()`/
`computeRawEyeAnchor()`):

1. **Mounted gameplay (horse/canoe/board) was never added to the core-
   anchor's fallback list**, unlike swimming/crawling/vine-climbing/
   hookshot, which all got this exact same fix earlier. Section 11's own
   notes explicitly flagged this as "expected to work with zero additional
   code" but **never actually confirmed in-headset** — and it doesn't hold
   up: `setBodyPartPos()` (`d_a_alink.cpp`) computes the animated eye
   position for horse/canoe/board via dedicated
   `horseLocalEyeFromRoot`/`canoeLocalEyeFromRoot`/`boardLocalEyeFromRoot`
   offsets applied through a matrix stack rooted at `field_0x3834`
   (`getRootPosP()`, the model's own joint-0 world position) — a
   structurally different derivation than plain standing, not just a
   different constant. The core anchor's calibrated
   `s_coreAnchorHeightOffset` is captured once (almost always while
   standing, since that's the far more common state to first enter
   first-person in) and held fixed forever — applying it on top of
   `current.pos.y` while mounted produces a height that means something
   different than what it was calibrated for, explaining "too high" while
   riding directly. It also explains dismounting going wrong WITHOUT
   needing any recalibration mid-ride: mounting/dismounting during
   ordinary gameplay never runs a real `dEvt_control_c` event
   (`checkEventRun()` stays false the whole time — confirmed directly from
   `isFirstPerson()`'s own comment: "Does NOT affect ordinary mounted
   GAMEPLAY... only mounted cutscenes"), so nothing was ever
   resetting/recalibrating the offset around a mount/dismount either. If a
   player's FIRST first-person activation in a session happened to land
   while already mounted (loading a save that starts on horseback, or a
   cutscene ending mid-ride), the one-shot calibration would capture the
   MOUNTED relationship between `current.pos.y` and eye height instead of
   the standing one — and that wrong-for-standing offset would then
   persist after dismounting too, until the next false→true
   `isFirstPerson()` transition. **Fix**: added
   `link->checkReinRide() || link->checkCanoeRide() || link->checkBoardRide()`
   to `computeRawEyeAnchor()`'s existing fallback condition (same one
   swim/crawl/vine/hookshot already use), falling back to the already
   mount-aware `getSubjectEyePos()` instead of the core anchor — removes
   the whole failure mode by construction rather than patching around one
   symptom of it.
2. **The calibration itself trusted the very first frame it ever saw**,
   with no defense against that frame coinciding with a scene-load/door
   transition (shops) where `current.pos`/`getSubjectEyePos()` might not
   yet both reflect the new area. Once locked in, this offset is held
   fixed until the next `isFirstPerson()` false→true transition — a bad
   sample here reads as a *persistent* wrong height exactly matching "exit
   a shop and it will be underground," not a one-frame glitch. **Fix**, in
   the same "reject an implausible sample" spirit as
   `vr_swing_detector.hpp`'s `maxPlausibleSpeed` (added for the analogous
   reason — see section 13): (a) require at least one MORE real sim tick
   to run after first-person reactivates before attempting calibration at
   all (`s_coreAnchorActivationTickKnown`/`s_coreAnchorActivationSimTick`),
   giving one tick for both position sources to settle post-transition;
   (b) reject a candidate offset outside a generous plausible range
   (20-100 units, `kCoreAnchorHeightOffsetDefault` = 55.75 is the expected
   common case) and keep retrying on subsequent ticks instead of locking
   it in, up to a bounded attempt count (`kCoreAnchorCalibrationMaxAttempts`
   = 30, ~1s) so a genuinely-out-of-range case doesn't retry forever stuck
   on the placeholder default.

Both fixes live in `vr_link_visibility.hpp` only. `getVrBodyPositionOffset()`
needed no separate change — it already calls the same shared
`computeRawEyeAnchor()` (not `computeRawCoreAnchoredEye()` directly, per
this file's own "one shared raw-anchor definition" pattern), so it
automatically stays consistent with whatever `getVrCameraEyeAnchor()`
decided this frame.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported "It's fixed," a
terse but unambiguous pass (both halves — Epona mount/dismount and shop
exit — were called out together in the original report, and nothing came
back as a partial/still-broken case). Notable since this whole
investigation was code-reading only, with no diagnostic-log round or
in-headset iteration before landing — the two root causes (mounted
gameplay missing from the core-anchor fallback list, unguarded
calibration timing) were derived directly from reading `setBodyPartPos()`
and `isFirstPerson()`'s own already-written comments, which turned out to
be sufficient evidence on their own this time. This was flagged by the
user as "last fix before releasing the mod" — worth remembering if this
area is revisited, since it may mean release prep is now unblocked on the
VR side. If a subtler variant of this ever resurfaces (e.g. a different
scene-load transition still going wrong, or a mount type not covered
here), the two mechanisms above — the mount-fallback list in
`computeRawEyeAnchor()` and the settle-window/plausibility-range gate in
`computeRawCoreAnchoredEye()` — are exactly where to look first; widening
the settle window (require N consecutive plausible-range ticks instead of
just one) is the natural next lever if a slower transition than a shop
door ever exposes the same class of bug again, per exactly
how long the settle window needs to be.

### Camera-anchor calibration — settle window widened for slower transitions (Ordon Village entry) — CONFIRMED FIXED IN-HEADSET 2026-08-14

**Follow-up report, same bug class**: "the camera still sometimes goes on
the ground in ordon village." Asked the user to narrow it down rather
than guessing again: trigger was **walking into Ordon Village** (a
room-transition, not a shop door, mount/dismount, or dialogue), and the
camera **stayed stuck** rather than self-correcting — the same
persistent-bad-calibration shape as the 2026-08-13 fix, just from a
transition that fix's single-sample settle window didn't anticipate. This
is exactly the "next lever" that fix's own writeup already named in
advance (see immediately above): a bigger/slower transition than a shop
doorway can still land the required ONE post-reactivation sample on a
still-mid-load moment that happens to fall inside the 20-100 plausible
band by coincidence — current.pos already at the new area's spawn point
while the animated eye joint is still a tick or two behind (or vice
versa) — locking in a "plausible but wrong" gap that then never gets
re-evaluated (calibration only re-runs on the next `isFirstPerson()`
false→true transition).

**Fix** (`vr_link_visibility.hpp`'s `computeRawCoreAnchoredEye()`):
requires `kCoreAnchorRequiredConsecutivePlausible` (3) CONSECUTIVE
plausible samples, each within `kCoreAnchorConsecutiveTolerance` (5
units, ~2in) of the previous plausible one, before locking in — not just
one. A sample outside the plausible band resets the run to 0; a plausible
sample that's meaningfully different from the last plausible one (still
settling, just happens to stay inside the band) restarts the run at 1
rather than continuing to accumulate, so a transition that keeps landing
"plausible" but keeps moving can't falsely satisfy the requirement. The
existing `kCoreAnchorCalibrationMaxAttempts` (30, ~1s) fallback is
unchanged — past that many attempts, whatever the latest sample is gets
accepted regardless of the consecutive-run state, same as before, so a
genuinely slow/never-settling case still can't stall calibration forever.
Reset alongside the other calibration state (`s_coreAnchorConsecutivePlausible`
added to `getVrCameraEyeAnchor()`'s existing `!isFirstPerson()` reset
block) so each new activation gets its own fresh run count.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested (walking into Ordon Village,
the original repro) and reported "Fixed." Terse but unambiguous, matching
the direct, specific repro that was confirmed before the fix. Requiring 3
consecutive plausible-and-consistent samples instead of 1 was sufficient
for this transition. If a still-slower transition ever exposes the same
class of bug again, `kCoreAnchorRequiredConsecutivePlausible`/
`kCoreAnchorConsecutiveTolerance` (`vr_link_visibility.hpp`) are the
constants to retune next — but get a specific repro first, same as this
round, rather than retuning blind.

### Epona dash speed-blur effect — CONFIRMED WORKING IN-HEADSET 2026-08-14

**Goal** (explicit user request: "disable the epona speed effect"). User
also pointed at `C:\Users\joeyw\dusklight-mods`'s `effect_remover` mod on
the chance it already covered this — checked directly: that mod only
covers three unrelated "fake shading" removers (projected/moya cloud
shade, terrain shadow wash-out — the same TevKColor mechanism already used
for this project's own cloud-shadow fix — and unbaked vertex lighting).
Nothing in that repo (any of its 6 mods) mentions Epona, horses, dash, or
speed effects at all — a clean grep across the whole repo came back empty.
No reusable fix there; root-caused directly in this codebase instead.

**Root cause**: `dCamera_c::onHorseDush()` (`d_camera.cpp`), called from
two sites in `d_camera.cpp` whenever Epona's lash-dash starts
(`horse->getLashDashStart()`), calls `StartBlure(55, mpPlayerActor, 0.75f,
1.0f)`. `StartBlure()` sets `mBlure` state that drives `motionBlure()`
(`m_Do_graphic.cpp`) — a screen-space radial zoom-blur post-process:
samples the shared captured-framebuffer texture
(`mDoGph_gInf_c::getFrameBufferTexObj()`) through a texture matrix that
pulls the screen toward the target actor's on-screen position, blended
over several frames for a streak look. **This is the exact same shared
screen-capture texture already documented at length in CLAUDE.md section 3
for water's fake reflection and section 5's heat-wave kagerou particles**
— every one of those needed VR-specific handling because a headset's free
head rotation and per-eye stereo separation break the "smooth, bounded
flatscreen camera motion" assumption this whole screen-capture-as-effect
technique depends on. Same mechanism, same assumption, same class of bug,
just triggered by Epona's dash instead of water/heat-shimmer — a strong,
direct match for "speed effect" without needing a diagnostic-log round.

**Fix**: gated the single `StartBlure()` call inside `onHorseDush()` on
`!dusk::vr::isRenderingToHeadset()` — needed a new
`#include "dusk/vr/vr_main.hpp"` in `d_camera.cpp` (removed during the
2026-08-09 flatscreen-camera-sync revert, per that section's own writeup —
re-added here for this unrelated purpose). **Deliberately scoped to only
this one call site**, not `StartBlure()`/`motionBlure()` globally —
`StartBlure()` is a general-purpose effect also used for several unrelated
things (underwater motion blur, enemy charge attacks in
`d_a_e_po.cpp`/`d_a_e_pz.cpp`/`d_a_e_zh.cpp`, cutscene possession shots in
`d_ev_camera.cpp`/`d_event_data.cpp`, Golden Wolf in
`d_a_npc_gwolf.cpp`), none of which were reported and none of which are
touched here — matching this project's established "disable only the
reported call site" pattern (section 5's kagerou fixes: "touching only
that one call, leaving every other particle/effect at that call site
alone"). Since `onHorseDush()` is a single shared function with exactly
two call sites (both just call it, no per-site logic), gating inside the
function itself covers both without needing to touch either call site.

**Built successfully** (RelWithDebInfo) — only `d_camera.cpp` needed
recompiling, clean link, no new warnings.

**CONFIRMED WORKING IN-HEADSET** — user tested and reported "The effect is
gone." No regressions to flatscreen or the other `StartBlure()` triggers
reported.

### Epona camera pulled back ~1ft + raised ~6in, was phasing through her head — CONFIRMED FIXED IN-HEADSET 2026-08-14

**Goal** (explicit user follow-up, same horse-riding session as the
speed-blur fix above: "can you move the camera backwards by about a foot
when on epona? The camera phases through her head and it's way too far
ahead of link's body"). Direct continuation of the 2026-08-13 mounted-
gameplay fix (`computeRawEyeAnchor()`'s horse/canoe/board fallback to
`getSubjectEyePos()`) — that fix corrected the VERTICAL height (was "too
high"), and this fix addresses a separate, FORWARD/BACK problem in the
same underlying value: `setBodyPartPos()`'s `horseLocalEyeFromRoot`
offset ({1.75, 55.0, 25.5}, `d_a_alink.cpp`) was authored for the
flatscreen third-person camera's own needs (that camera sits well behind
Link even at its closest, so a forward-biased reference point is
invisible to it) — a first-person VR camera sitting directly at that same
point is far enough forward to clip through Epona's own head/neck
geometry, matching the report exactly.

**Fix** (`vr_link_visibility.hpp`'s `computeRawEyeAnchor()`): split the
horse case (`checkReinRide()`) out of the shared swim/crawl/vine/
hookshot/canoe/board fallback branch into its own branch, and pull the
raw head-joint anchor BACK along Link's body-facing direction
(`current.angle.y`, the same field/convention
`computeRawCoreAnchoredEye()` already uses for its own forward-offset
nudge — see section 23 — just negated here) by a new
`kHorseCameraBackUnits = 30.48f` (1 real foot, same 100-units/metre
conversion this file already establishes). **Scoped to horse riding
only** — canoe/board weren't reported and keep their own separate offsets
(`canoeLocalEyeFromRoot`/`boardLocalEyeFromRoot`) untouched; don't assume
they have the same problem without separate confirmation, same reasoning
this project always applies before widening a fix's scope.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**ROUND 1 RESULT**: user tested the backward pull alone and confirmed
"It moved back" (no complaint about the amount), then immediately
requested a second, independent nudge: "can you also move it up by about
6 inches" — same underlying `horseLocalEyeFromRoot` reference point being
too low for a first-person VR seat, not a correction to the backward
amount.

**ROUND 2 fix** (same function, same day): added `eye.y +=
kHorseCameraUpUnits` (`15.24f`, 6 real inches, same 100-units/metre
conversion) right alongside the existing backward pull in the
`checkReinRide()` branch — a plain vertical addition, no yaw/direction
math needed since up is up regardless of facing. Rewrote the block
comment above the branch to cover both axes together (was
"BACKWARD NUDGE," now "BACKWARD/UP NUDGE") rather than stacking a second,
separately-dated comment block on the same few lines.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` needed
recompiling (transitively includes the header), clean link, no new
warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested the up-nudge and reported
"Fixed," a terse but clear pass with no follow-up amount requested. Both
axes (1ft back, 6in up — `kHorseCameraBackUnits`/`kHorseCameraUpUnits`)
are done at these values; retune only on new feedback, per this file's
usual convention for tuned-and-confirmed constants (e.g. section 23's own
3in-up/6in-forward standing-camera nudge).

### Fishing hookset — right-hand yank gesture, reusing the shield-bash thrust detector — built 2026-08-14, NOT yet confirmed in-headset

**Symptom** (user report): "I can move the hook with the rod (it's attached
to the line) however the fish bite and when I pull they just let go. Is
this fixable or do I need to bind the C stick to use the original fishing
controls?"

**Traced the real minigame code before answering** (`d_a_mg_rod.cpp`):
hook-setting checks `rod_stick_y < -0.5f` — the MAIN/left stick pulled
sharply back, NOT the C-stick (which only drives casting power/direction,
already superseded by the controller-pointing-aim feature). VR's left
thumbstick already correctly feeds `rod_stick_y` (`padStatus.stickY`,
unchanged since section 13) — the mechanic was never actually broken, and
binding the C-stick would not have helped (it isn't read for this). The
real problem is UX: the game wants a thumbstick flick at the exact moment
a fish bites, which isn't the natural VR motion a player reaches for
(physically yanking the rod-holding hand, mimicking a real hookset).

**Fix, per user's explicit choice (asked first)**: right hand, reusing
`g_rightThrust` — the SAME `vr_combat::SwingDetector` instance already
driving the shield-bash thrust (a dedicated second tuned detector was
deliberately NOT added; overloading is harmless, since forcing R while
fishing does nothing with no shield equipped, and this fix's stick pulse
does nothing unless the game's own `mRemainingHookTime` bite window is
open). New `vr_link::isFishingHookInWater()` (`vr_link_visibility.hpp`)
gates the pulse to only apply while `dmg_rod_class::is_hook_in_water` is
true (a public field already, no new accessor needed on that class) —
scoped narrower than "rod equipped" so the forced pulse can't nudge
movement during ordinary casting/idle rod-holding. Thin-forwarded via
`dusk::vr::isFishingHookInWater()` (`vr_main.hpp`/`.cpp`), same pattern
as every other function in this file.

`vr_main.cpp`'s `tick()`: on `rightThrustEvent.triggered &&
isFishingHookInWater()`, latches `padStatus.stickY = -127` for
`kRodYankStickHoldSec = 0.15s` (a plain level-hold, not the R-button
fix's release-then-assert pattern — `rod_stick_y` is read as a continuous
value every sim tick, not an edge, so holding it low for a few sim-tick
periods is sufficient on its own). Applied as an override right after the
normal left-stick write, so a real stick deflection is only overridden
during the brief pulse window.

**Built successfully** (RelWithDebInfo, full rebuild since `vr_main.hpp`
changed) — `vr_link_visibility.hpp`, `vr_main.hpp`/`.cpp` all recompiled,
clean link, no new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up: cast
the rod, wait for a bite, and yank the right hand back sharply during the
bite window — confirm the fish gets hooked instead of letting go. If it's
unresponsive, check whether `g_rightThrust`'s existing tuning (from the
shield-bash gesture) is well-suited to a "pull back" motion specifically
(it doesn't distinguish direction, so any fast right-hand motion should
still trigger it) before adding direction filtering. If it over-triggers
(fires from casting motions, not just the intended yank), the
`isFishingHookInWater()` gate should already prevent any effect outside
an active cast — if that's not holding, `is_hook_in_water`'s real
lifecycle (when exactly it flips) is the next thing to verify with a log,
not assumed from a partial reading of `d_a_mg_rod.cpp`.

### Fishing hookset, round 2 — C-stick rebound to the right thumbstick while fishing (yank gesture didn't work) — CONFIRMED FIXED IN-HEADSET 2026-08-14

**User report on the yank-gesture fix (previous section)**: "That didn't
seem to do it. Moving the rod still unhooks the fish." Explicit follow-up
request, asked directly rather than guessing why the gesture failed:
"Can you bind C stick to the right stick, but only while you are
fishing?"

**Implemented literally as asked, rather than debugging the gesture
further** — the user's request is itself a complete, well-scoped
alternative fix, and section 15's own original unbinding of the C-stick
(replaced by VR smooth-turn) is exactly what stood between the fishing
minigame and its real analog controls; reverting that specifically while
fishing restores the ORIGINAL flatscreen mechanic rather than trying to
paper over the yank gesture with more tuning.

**New gate, broader than the yank fix's `isFishingHookInWater()`**:
`vr_link::isFishingRodActive()` (`vr_link_visibility.hpp`) — true
whenever the fishing-rod actor merely exists (casting, waiting, hook in
water, reeling), not just while a fish is on the line. Needed to be
broader because the REAL C-stick-driven mechanics this restores —
`rod_substick_x/y` in `d_a_mg_rod.cpp` — cover cast pull-back/power
(~line 1243-1272) and rod-tip/lure steering (~line 3773-3780) too, both
usable before any bite happens, not just the hookset moment the yank fix
targeted. Same `fopAcM_SearchByName(fpcNm_MG_ROD_e)` existence check
already used by `isFishingHookInWater()`/`refreshTrackedFishingRodMtxLive()`
— no new lookup mechanism.

**`vr_main.cpp`'s `tick()`**: the right-thumbstick block (previously
unconditional `updateSmoothTurn(rightStick.x, ...)`, since section 15)
now branches on `isFishingRodActive()`:
- **Fishing**: writes `padStatus.substickX/Y` from the right stick (same
  clamp-to-127 pattern the left stick's write already uses), restoring
  real C-stick input to the minigame. `updateSmoothTurn()` is
  deliberately SKIPPED for these frames (not fed zero) — the stick is
  doing fishing input instead, matching how the original flatscreen
  controls never used the C-stick for camera turn while fishing either.
- **Not fishing**: unchanged — smooth-turn as before, no substick write.

The yank-gesture fix from the previous round (`g_rightThrust` +
`rodYankForceStickDown` forcing `padStatus.stickY`) is left in place, not
reverted — an additional/alternative control, not a replacement; harmless
to keep since it only ever fires while `isFishingHookInWater()` is true
and does nothing if the real C-stick already sets the hook first.

**`wantsVirtualPad`'s OR-chain updated** — section 15 had dropped the
`substickX`/`substickY` checks entirely, reasoning they were "always
zero" once the C-stick was fully unbound. That's no longer true now that
fishing can write real nonzero values there again; added both back so a
frame with ONLY C-stick input (e.g. casting while the rest of the
controller is idle) still reaches `PADSetVirtualStatus()` instead of
being silently dropped.

**Built successfully** (RelWithDebInfo, full rebuild since `vr_main.hpp`
changed) — `vr_link_visibility.hpp`, `vr_main.hpp`/`.cpp` all recompiled,
clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested and reported: "Fixed, I have
to hold the right stick down to hook the fish then I can physically wave
the rod left and right to get the fis[h]." Real working control scheme,
for reference: hold the right thumbstick (C-stick) down to set the hook,
then physically move the tracked hand (controller-pointing steers the rod
tip, per the earlier controller-aim feature) to reel/fight the fish —
matches the original flatscreen game's own C-stick-driven hookset/fight
controls, now genuinely restored.

**Worth flagging, not re-investigated**: this contradicts the previous
section's code trace, which found hookset gated on `rod_stick_y < -0.5f`
— the MAIN/left stick, not the C-stick/substick — and concluded this
round's C-stick rebind "does NOT change how hookset itself is triggered."
The user's actual result says otherwise: holding the RIGHT stick down is
what works. Two explanations, neither confirmed: (a) `d_a_mg_rod.cpp` has
another, unread hookset-adjacent check on `rod_substick_y` somewhere this
session's partial reading missed (the file is large, ~6000 lines, and
was never read end-to-end — only the specific regions this investigation
happened to grep into), or (b) holding the C-stick down while reeling
satisfies some other real game-logic gate (e.g. `checkFishingRodUseAccept()`'s
own `rod_substick_y < -0.9f` condition, glimpsed but not traced fully in
the previous section at `d_a_mg_rod.cpp:3664`) that's a prerequisite for
the hookset check to even run. Since the fix works and the user has a
clear, repeatable control scheme now, this discrepancy isn't worth
chasing further unless a future report contradicts the working behavior
above — if it does, start by actually reading `rod_substick_y`'s full
set of consumers in `d_a_mg_rod.cpp` rather than re-trusting this
session's partial trace.

**This closes out the fishing minigame VR-control investigation** — cast/
steer via the right stick (this round), hookset via holding the right
stick down (same rebind, mechanism not fully explained but confirmed
working), reel/fight via physically moving the tracked hand. The
earlier-suspected left-stick/yank-gesture path for hookset was not what
actually ended up mattering in practice.

### Eye-buffer BMP/PNG debug dump tooling removed — 2026-08-14

**User request**: "remove the debug bmp and png screenshots and their
function." The eye-buffer BMP dump tooling (`dumpEyeBufferToBmp()`,
`vr_xr_submit.hpp`, built 2026-07-29 — see section 3's water-black
investigation, where it was the load-bearing tool that found the real
root cause) was hooked into `readbackEyeCopy()` to re-dump each eye's
rendered buffer to `vr_debug_eye0.bmp`/`vr_debug_eye1.bmp` every 90
frames. Per this project's normal practice, diagnostic scaffolding is
removed once no longer needed — this tool hasn't been reached for since
the water investigation closed out, and the four dump files
(`vr_debug_eye{0,1}.bmp`/`.png`, the PNGs being one-off manual
conversions from an earlier session) had been sitting **checked into git**
this whole time, at several MB each.

**Removed**: `dumpEyeBufferToBmp()` itself and its call site (the
`frameCounter[eyeIndex]++ % 90` gate) in `readbackEyeCopy()`
(`vr_xr_submit.hpp`), plus `git rm`'d all four tracked files
(`vr_debug_eye0.bmp`, `vr_debug_eye0.png`, `vr_debug_eye1.bmp`,
`vr_debug_eye1.png`) from the repo.

**Built successfully** (RelWithDebInfo) — only `vr_main.cpp` (which
transitively includes `vr_xr_submit.hpp`) needed recompiling, clean link,
no new warnings, no other call sites found by grep. If a future VR
rendering investigation needs this kind of raw-pixel-dump capability
again, section 3's writeup above still has the full technique/reasoning
to rebuild it from — it's a genuinely reusable pattern, just not worth
keeping resident in the tree indefinitely.

### Flat 2D aiming reticle disabled in VR; smooth-turn rate increased 1.5x — both CONFIRMED WORKING IN-HEADSET 2026-08-14

**Reticle**: `daAlink_sight_c::draw()` (`d_a_alink_effect.inc`) — the flat
2D crosshair sprite shown while aiming bow/slingshot/hookshot/boomerang
(both the normal `daPy_sightPacket_c::draw()` case and the lock-on
`mLockCursor` case) — now early-returns while
`dusk::vr::isRenderingToHeadset()`. Per explicit user request, scoped to
VR only (flatscreen unaffected). Redundant now that the world-space
physical aim-point marker exists (`drawAimCrosshair()`, see the section
above) — the 2D reticle would otherwise still bake into the head-locked
HUD billboard, fixed center-screen regardless of where the player is
actually looking. `d_a_alink_effect.inc` is textually included into
`d_a_alink.cpp` (which already includes `vr_main.hpp`), so no new include
was needed. Built clean, confirmed fixed.

**Smooth-turn rate**: `kSmoothTurnDegPerSec` (`vr_smooth_turn.hpp`)
raised from 90°/s to 135°/s (1.5x, exact user-requested multiplier) at
full right-stick deflection. Deadzone and everything else about the
feature (section 15) unchanged. Built clean, confirmed working.

### Crash on changing armor/clothes in the menu — FIXED, CONFIRMED IN-HEADSET 2026-08-14

**Symptom** (user-reported crash, with a real call stack): access
violation reading `0xFFFFFFFFFFFFFFFF` inside `J3DShape::offFlag()`,
called from `J3DShapeTable::show()`, called from
`vr_link::showModel(mpLinkHatModel)` inside `vr_link::updateFrame()` —
i.e. a crash in this project's own VR code, not base-game code, triggered
specifically by changing armor/clothes on the menu screen.

**Root cause**: `updateFrame()` calls `showModel()`/`hideModel()` on
`mpLinkFaceModel`/`mpLinkHatModel` (plus `hideArmsAndEars()`/
`showArmsAndEars()`, which touch `mpLinkModel`'s own shape table)
**unconditionally every real VR frame**, per section 18's "runs every
frame either way" reasoning. But `d_a_alink.cpp` already has its own,
pre-existing precedent for exactly this hazard: several base-game
show()/hide() calls on these SAME body-related shape tables are wrapped
in `if (mClothesChangeWaitTimer == 0) { ... }` — meaning the underlying
`J3DModelData`/shape tables are apparently in a transient, unsafe state
for a few frames while a clothes change (Hero's Clothes ↔ Ordon Clothes,
or Magic Armor) is actually swapping model resources. This VR code never
checked that flag, so it could land mid-swap and dereference a stale/
freed shape-table pointer — matching the `-1`-pattern access violation
exactly.

**Fix** (`vr_link_visibility.hpp`'s `updateFrame()`): wrapped the whole
face/hat/arms/ears show/hide block in
`if (link->getClothesChangeWaitTimer() == 0) { ... }` — a public accessor
that already existed (`d_a_alink.h`), same guard shape the base game
already uses at its own call sites on these shapes. Deliberately scoped
to just this block, not an early-return out of the whole function — the
tracked-hand-matrix computation later in `updateFrame()` is unrelated to
this hazard and shouldn't stall too, even though in practice the player
is looking at a menu during this brief (~4-tick) window either way. Since
this function already runs every real frame regardless, skipping a
handful of frames during the wait window costs nothing — the hide/show
decision just resumes correctly once the timer clears, same reasoning
section 18 already established for why this block is safe to run
unconditionally in the first place.

Built successfully (RelWithDebInfo) — only `vr_main.cpp` (transitively
includes the header) recompiled, clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested (changing armor on the menu
screen, the exact repro from the crash report) and reported "Fixed."
**Reusable lesson**: `mClothesChangeWaitTimer`/`getClothesChangeWaitTimer()`
is the general-purpose signal for "don't touch Link's face/hat/arm/body
shape tables right now, a clothes/armor swap is mid-flight" — worth
checking for at any FUTURE VR call site that shows/hides shapes on
`mpLinkModel`/`mpLinkFaceModel`/`mpLinkHatModel`/`mpLinkHandModel`,
not just the ones this fix touched.

### Camera-ground bug on load — ACTUAL root cause found and fixed 2026-08-15 (two earlier attempts on the same report were real but insufficient)

**User report, precise repro data**: "the camera in the ground bug is still
there sometimes when loading" — later narrowed via follow-up to: ordinary
loading zones, fast travel (Owl Statue/overworld), and loading a save all
trigger it; behavior is "sometimes it stays stuck, especially if you move
too much or trigger first person as soon as it loads, but other times it
corrects itself if you stand completely still."

**Two earlier fixes this same round, both real but NOT the actual cause**
(kept, harmless, just insufficient on their own):
1. Teleport-jump detection (`vr_link_visibility.hpp`'s
   `computeRawCoreAnchoredEye()`) — any single-tick `current.pos` jump
   bigger than real movement can produce (300 units) now force-resets
   calibration, catching loads/warps that never trip `isFirstPerson()`
   false→true. Real, addresses a genuine gap, but wasn't what was
   observed firing in the actual capture below (the false→true edge was
   already covering this particular repro).
2. Widened `kCoreAnchorCalibrationMaxAttempts` from 30 (~1s) to 150
   (~5s) — user called this out directly ("did you actually attempt to
   fix it or did you just increase how long until the check?") and was
   right to: a real `[dusk::vr::coreanchor]` capture (below) proved this
   just delayed hitting the same guaranteed-to-fail path, not fixed it.

**ACTUAL root cause, found via real diagnostic capture** (temporary
`[dusk::vr::coreanchor]` `OutputDebugStringA` logging added to log every
calibration attempt's candidate value/plausibility/consecutive-count,
plus the final commit reason): the calibration's plausibility band
(`kCoreAnchorHeightOffsetPlausibleMin/Max`) was `20–100`, copied from
`kCoreAnchorHeightOffsetDefault = 55.75` — itself copied verbatim from
`setBodyPartPos()`'s `localEyeFromRoot = {0, 55.75, 15}` (`d_a_alink.cpp`).
That constant is a LOCAL offset run through `mDoMtx_stack_c::multVec()`
against the model's own joint-matrix stack — not a plain world-space Y
delta against `current.pos.y` the way this calibration's own
`realEye.y - current.pos.y` is. The two were never actually the same
quantity. The real capture showed the genuine, rock-stable candidate
value was **~157–158** (drifting under 1 unit across a full 150-attempt/
5-second window — clearly settled, real data) — comfortably OUTSIDE the
old 20–100 band, so `plausible=0` fired on literally every single attempt,
every time, guaranteeing it could never reach `settled` and always fell
through to the `MAX_ATTEMPTS_FALLBACK` — explaining why the bug was
consistent/every-time rather than occasional, and why round 2's timeout
widening just moved the wait from ~1s to ~5s without fixing anything.

**Fix**: raised the plausible band to `80–220` (comfortably bracketing the
real ~158 while still excluding real garbage samples seen in the same
capture — 299 and 18687.98, both harmless transient pre-settle-window
frames) and updated `kCoreAnchorHeightOffsetDefault` from 55.75 → 158 to
match the real measured value instead of the mismatched borrowed one.
With the band no longer rejecting the correct value outright, calibration
should now converge in ~3 ticks (~100ms) via the real `settled` path
instead of every time hitting the 5-second fallback.

Built successfully (RelWithDebInfo) — only `vr_main.cpp` (transitively
includes the header) recompiled, clean link, no new warnings.

**Diagnostic logging (`[dusk::vr::coreanchor]`, `computeRawCoreAnchoredEye()`
and the teleport-detection block) is still in the tree** — this specific
fix (the plausibility band correction) has not been independently
re-confirmed in-headset yet (the user's next message moved on to a
separate Iron Boots request before circling back). Per this project's
normal practice, remove the logging once a fresh capture/report confirms
it now commits via `SETTLED` quickly instead of `MAX_ATTEMPTS_FALLBACK`.

**Reusable lesson**: when a user directly challenges whether a change was
a real fix or just a bigger timeout, take that literally and check — in
this case they were exactly right, and the honest answer led straight to
finding the actual bug via a real capture instead of shipping a third
guess.

### Iron Boots — magnetized (wall/ceiling) AND submerged (underwater floor-walking) camera anchor — CONFIRMED FIXED IN-HEADSET 2026-08-15

**Goal** (user request: "make the camera first person for when the iron
boots are equipped", clarified via follow-up to specifically mean "the
camera feels wrong while stuck to a wall/ceiling" — not ordinary walking
with them equipped, which was already first-person under the existing
rules since `isFirstPerson()` never gated on Iron Boots at all).

**Two separate mechanisms, same fix shape, added to `computeRawEyeAnchor()`'s
existing raw-head-joint-anchor fallback list** (the same list swimming,
crawling, vine climbing, hookshot, and mounted gameplay already use — see
section 23's various follow-ups above):

1. **Magnetized** (`isMagnetized()`, new helper, same shape as
   `isCrawling()`/`isHookshotAirborneOrHanging()`): true when
   `checkMagneBootsOn()` (actually stuck to a magnetic surface) or
   `mProcID == PROC_MAGNE_BOOTS_FLY` (mid-flight being pulled toward one,
   `d_a_alink_hvyboots.inc`). The core anchor (section 23) assumes
   standing upright on flat ground with world-up as up and adds its
   calibrated height offset straight onto `current.pos.y` — but
   `setMagneBootsMtx()` (`d_a_alink_hvyboots.inc`) rotates
   `shape_angle`/`current.angle` directly to match the magnetic surface's
   normal, so Link's whole body can be pitched sideways on a wall or
   upside-down on a ceiling, where a fixed vertical-only offset means
   nothing. Confirmed the fallback (`getSubjectEyePos()`) actually
   handles this correctly by reading `setBodyPartPos()` itself
   (`d_a_alink.cpp` ~line 5581): its root+local-offset eye branch calls
   `concatMagneBootMtx()` before applying the local offset, i.e. the
   engine's own animated eye position already accounts for magnetic
   surface rotation — this fix just routes VR to use that existing,
   correct computation instead of the orientation-blind core anchor.
2. **Submerged/underwater floor-walking** (added after user follow-up:
   "It's also an underwater issue" — same requested treatment, not a
   separately root-caused bug): `link->checkWaterInMove()`
   (`FLG0_WATER_IN_MOVE`, `d_a_player.h` — already public). A genuinely
   different mechanism than the magnetized case, not just the same bug in
   two places: `MODE_SWIMMING` — which the raw-anchor fallback list
   already checked — does NOT stay set while Link walks along a
   submerged lake/river bed with Iron Boots on. Once he actually lands on
   the underwater floor (`checkSwimUpAction()`, `d_a_alink_swim.inc`,
   calls the same `procLandInit()` used for ordinary dry-land landings),
   the new proc state's `mModeFlg` gets fully REPLACED from
   `m_procInitTable` (not incrementally OR'd), so `MODE_SWIMMING` doesn't
   survive the transition even though he's still fully submerged — this
   is why the pre-existing `MODE_SWIMMING` check in the fallback list
   wasn't catching this case at all. Worth noting for anyone revisiting
   this: unlike the magnetized case, `setBodyPartPos()`'s own eye branch
   does NOT have bespoke math for this state (its gate is
   `MODE_SWIMMING`-based, which is off by this point, so it takes the
   same root+offset path as ordinary standing) — this fix isn't routing
   around a proven orientation bug the way the magnetized fix is, it's
   applying the same "core anchor's standing-calibrated height may not
   transfer to a different posture" precaution as every other entry in
   this list, per direct user request rather than an independently
   confirmed root cause.

Built successfully (RelWithDebInfo) — only `vr_main.cpp` (transitively
includes the header) recompiled, clean link, no new warnings.

**CONFIRMED FIXED IN-HEADSET** — user tested both (magnetized wall/ceiling
and submerged floor-walking) and reported "Fixed."

### VR "too bright" (external tester feedback) — universal live-tunable gamma compensation added 2026-08-16, NOT yet confirmed in-headset

**Goal** (external mod feedback relayed by the user, not a first-hand
in-headset report this time: "the number one complaint... it is too
bright"). Narrowed via direct questions before touching anything (per this
file's own standing "verify before guessing" discipline): VR-only (the
flatscreen/desktop-mirror window looks correct), described as overall
exposure too high, and — critically — reproduced across **all three
runtimes** (SteamVR, Virtual Desktop, Meta Link).

**Why that last point changes the diagnosis**: the ONLY existing gamma
modification anywhere in the VR submission path was
`kSteamVrGammaCompensationExponent` (`vr_xr_submit.hpp`, section 6) — a
GPU compute pass applying `pow(color, 1/2.2)` (a brightening curve),
gated entirely on `Session::swapchainIsSrgb_`. Per createSwapchain()'s own
candidate-preference order, VD and Meta Link always pick the plain native
(non-sRGB) format as their first successful candidate — `swapchainIsSrgb_`
is only ever true for SteamVR (the one runtime that requires an sRGB
format to actually composite a projection layer at all, confirmed back in
section 6). So VD/Meta Link were getting a **byte-for-byte passthrough
with zero gamma modification** the whole time. Since all three runtimes
show the identical "correct on the desktop mirror, washed out in the
headset" symptom, the existing SteamVR-only constant cannot be the (whole)
explanation — whatever's making the image too bright has to be something
that hits every runtime's compositor path uniformly, which this codebase
had never actually applied any correction for on the non-SteamVR runtimes
at all. (This also means the desktop-mirror feature from
2026-08-10 — which grabs eye 0's resolved texture *before* any of this
submission-path processing — was itself the tool that made this
comparison possible in the first place: "fine in the window, wrong in the
headset" is only diagnostic because the mirror bypasses the XR submission
path entirely.)

**Fix, asked-and-confirmed approach (user chose a live ImGui slider over
the usual hardcode-and-rebuild loop, specifically to make iterating on the
correct value faster than a rebuild+headset round-trip per guess)**:
- `Session::useGammaComputePath_` (`vr_xr_submit.hpp`) — new flag,
  generalizing what used to be a `swapchainIsSrgb_`-only gate. True for
  every swapchain format candidate the GPU compute shader can actually
  produce output for (i.e. every real candidate EXCEPT the rare
  `DXGI_FORMAT_R10G10B10A2_UNORM` last-resort fallback, which the shader
  was never written to pack into 10bpc — that one keeps the old plain CPU
  path with zero compensation, unchanged). Now gates both
  `ensureCpuCopyBuffers()`'s resource creation and `encoderTaskCallback()`'s
  compute-vs-plain-copy branch — previously both were gated on
  `swapchainIsSrgb_` directly, meaning VD/Meta Link never even allocated
  the compute pipeline.
- `Session::effectiveGammaExponent()` — the real per-dispatch exponent is
  now `baseline * gammaCompensationMultiplier_`, where `baseline` is
  exactly what was hardcoded before (`kSteamVrGammaCompensationExponent`
  for SteamVR's sRGB case, `1.0` — no-op — for everyone else), and
  `gammaCompensationMultiplier_` is the new live-adjustable piece.
  `pow(pow(x,a),b) == pow(x,a*b)`, so multiplying the two exponents
  together is mathematically exactly equivalent to applying both curves
  in sequence — no second compute dispatch needed, and SteamVR's already-
  tuned baseline is preserved exactly when the new multiplier is left at
  1.0.
- `dusk::getSettings().game.vrGammaCompensation` (`settings.h`/`.cpp`) —
  new `ConfigVar<float>`, default `1.0` (no additional change vs. what
  already existed), registered the same way as every other settings float
  in this file.
- **"VR Gamma Compensation" slider** (`ImGuiMenuTools.cpp`, Debug >
  Graphics Settings, right next to the existing "Disable Water
  Refraction"/"Enable LOD Bias" toggles), range `0.3`–`3.0`, using the
  existing `dusk::config::ImGuiSliderFloat` helper (same one-line
  set-and-persist pattern already used for other float settings in this
  codebase). `>1.0` darkens further, `<1.0` brightens further.
- `vr_main.cpp`'s `tick()`: `g_session->setGammaCompensationMultiplier(
  dusk::getSettings().game.vrGammaCompensation.getValue())` called once
  per frame, read on the main thread, right after `setFrameState()` —
  well before that frame's `encodeEyeCopy()`/encoder-task dispatch.
  `effectiveGammaExponent()` itself is consulted later, on the render
  worker thread, from a plain (non-atomic) float member — deliberately not
  synchronized, since it's only ever written once per frame from the main
  thread before that frame's task runs; same informal single-word-read
  pattern this codebase already relies on for other per-frame settings
  reads, not a new risk class.

**Built successfully** (RelWithDebInfo) — `settings.h`/`.cpp`,
`ImGuiMenuTools.cpp`, `vr_xr_submit.hpp`, `vr_main.cpp` all recompiled,
clean link, no new warnings.

**NOT yet tested in-headset.** Next step for whoever picks this up: launch
in VR (any runtime — the slider now does something on all three, not just
SteamVR), open Debug > Graphics Settings, and slide "VR Gamma
Compensation" up from 1.0 until the headset's brightness matches the
desktop mirror's. Report back the value that looks right, and on which
runtime(s) it was tested — since testers are apparently spread across all
three, it's plausible (not yet confirmed either way) that different
runtimes need different corrected values, in which case a single global
multiplier isn't quite the final shape of this fix and it may need
splitting per-runtime the way `kSteamVrGammaCompensationExponent` already
implicitly is. If the slider turns out to do nothing at all on VD/Meta
Link specifically, `useGammaComputePath_` not actually being true for
their chosen candidate format would be the first thing to check (add a
temporary `OutputDebugStringA` log of `useGammaComputePath_`/
`swapchainDxgiFormat_` right after `createSwapchain()` returns, per this
file's usual diagnostic-logging workflow) rather than assuming the shader
math itself is wrong.

**UPDATE (same day) — `2.0` CONFIRMED correct on Virtual Desktop; SteamVR
and Meta Link not yet tested. Decoupled SteamVR's baseline from the new
slider before setting a default, built, NOT yet re-tested in-headset.**
User: "Setting the slider to 2.000 looks best" — asked which runtime(s)
this was tested on before doing anything with the number, since the
original design multiplied the slider onto EVERY runtime's baseline
uniformly, including SteamVR's own already-tuned
`kSteamVrGammaCompensationExponent` (~0.4545, confirmed correct back in
section 6). **Answer: Virtual Desktop only.** Blindly setting the
compiled default to `2.0` would have pushed SteamVR's effective exponent
to `~0.4545 * 2.0 ≈ 0.909` — nearly cancelling a correction that was
independently confirmed correct four sessions ago, on pure untested
speculation that the same multiplier applies there too. Exactly the kind
of "don't infer a nearby fix supersedes something without evidence"
mistake this file's own closing lesson warns about, just in a new spot.

**Fix**: `Session::effectiveGammaExponent()` (`vr_xr_submit.hpp`) no
longer multiplies the slider onto a per-runtime baseline uniformly —
SteamVR now returns its own `kSteamVrGammaCompensationExponent` completely
untouched by the slider; every other runtime (VD, Meta Link, anything else
that lands on a non-sRGB candidate format) uses the slider value directly
as its exponent (mathematically identical to "multiplied onto a baseline
of 1.0", just no longer coupled to SteamVR's separate baseline by
construction). With that decoupling in place, `vrGammaCompensation`'s
compiled default (`settings.cpp`) was updated `1.0` → `2.0` — now safe,
since it can no longer touch SteamVR at all. Comments updated throughout
(`vr_xr_submit.hpp`, `settings.h`, `ImGuiMenuTools.cpp`) to record that
`2.0` is confirmed for VD specifically, not yet tested on SteamVR/Meta
Link, and that the slider is deliberately scoped to exclude SteamVR going
forward unless SteamVR itself is separately tested and found to need
adjustment too.

Built successfully (RelWithDebInfo) — `vr_xr_submit.hpp`, `settings.h`/
`.cpp`, `ImGuiMenuTools.cpp` recompiled, clean link, no new warnings.

**NOT yet independently re-tested in-headset** — the user's own running
session already had `2.0` live-set and persisted via `config::save()`
before this change, so their own experience shouldn't change at all (VD
was never touched by the SteamVR-decoupling edit). What's unverified is
whether `2.0` is *also* right for Meta Link (same zero-baseline
architecture as VD, so plausible but unconfirmed) and whether SteamVR
needs its own adjustment on top of its existing baseline now that a real
"too bright" report exists for it too. Next step: test on Meta Link and/or
SteamVR if convenient, and report back per-runtime findings the same way
VD's was — don't assume one number covers all three without checking.

**UPDATE (same day) — user tested SteamVR: "steamvr is undersaturated
now." Gave SteamVR its OWN independent live-adjustable slider instead of
guessing a new constant, built, NOT yet re-tested in-headset.** This is
consistent with, and arguably confirms, a risk flagged internally at the
very start of this whole investigation but never written into the user
-facing response: `kSteamVrGammaCompensationExponent` (~0.4545) was
originally tuned back in section 6 by comparing SteamVR's own appearance
against VD/Meta Link's — i.e. tuned to visually MATCH them, not
independently verified as objectively correct. This session's entire
"too bright" investigation started because VD/Meta Link were ALSO
reported too bright (same original 3-runtime answer) — meaning the
reference SteamVR was tuned to match may itself have been wrong the whole
time. Since VD/Meta needed DARKENING (the `2.0` exponent) while SteamVR's
existing fix is a BRIGHTENING curve, SteamVR plausibly needs to move in
the opposite direction (toward `1.0`, i.e. less brightening) — consistent
with "undersaturated" (over-brightening flattens contrast/vividness).

**Fix**: new `Session::steamVrGammaExponent_` (`vr_xr_submit.hpp`,
default = the original `kSteamVrGammaCompensationExponent` so nothing
changes until retested), its own setter
(`setSteamVrGammaCompensationExponent()`), its own `ConfigVar<float>
vrGammaCompensationSteamVr` (`settings.h`/`.cpp`, same default), and its
own **independent** "VR Gamma Compensation (SteamVR)" ImGui slider
(`ImGuiMenuTools.cpp`, range `0.3`–`2.2` — covers both directions already
tested historically: the original `~0.4545` brighten fix, `2.2`'s
previously-found-too-dark darken extreme, and everything up toward `1.0`/
no-correction-at-all in between). `effectiveGammaExponent()` now returns
`steamVrGammaExponent_` for the SteamVR branch instead of the hardcoded
constant. **Deliberately kept fully decoupled from the VD/Meta slider
added earlier the same day** — the whole point of today's second fix was
exactly this kind of cross-runtime coupling accident; adding a second
independent slider avoids repeating it a second time.

Built successfully (RelWithDebInfo) — `vr_xr_submit.hpp`, `settings.h`/
`.cpp`, `ImGuiMenuTools.cpp`, `vr_main.cpp` recompiled, clean link, no new
warnings.

**NOT yet tested in-headset.** Next step: launch on SteamVR, open Debug >
Graphics Settings, and slide "VR Gamma Compensation (SteamVR)" toward 1.0
(less brightening) from its current default (~0.4545) until saturation
looks right, independent of whatever the VD/Meta Link slider is set to.
Report the value that looks correct the same way VD's `2.0` was reported.

**UPDATE (same day) — CONFIRMED: SteamVR should be 1.0 (no compensation
at all), default updated, built.** User: "Steamvr should be set back to
1.0." `Session::steamVrGammaExponent_`'s compiled seed
(`kSteamVrGammaCompensationExponent`, ~0.4545) is now purely historical —
`ConfigVar<float> vrGammaCompensationSteamVr`'s compiled default
(`settings.cpp`) changed `1.0f/2.2f` → `1.0f`. Updated every comment that
called `kSteamVrGammaCompensationExponent` "already-correct"/"already-
tuned" (the top-of-file block, the field comment, the ImGui slider
comment) with a "CORRECTED, SAME DAY" note recording that it was never
actually independently verified — only tuned to visually match VD/Meta
Link, which turned out to be wrong too. **This closes out the whole
2026-08-16 "too bright" investigation for now**: VD/Meta Link confirmed
at `2.0`, SteamVR confirmed at `1.0` (i.e. the GPU gamma-compensation
compute pass still runs for SteamVR — still needed for its sRGB
format's channel handling — but with an exponent of 1.0 it's now a
color-value no-op, equivalent to the plain passthrough VD/Meta Link
always had). Built successfully (RelWithDebInfo) — `settings.cpp`,
`vr_xr_submit.hpp`, `ImGuiMenuTools.cpp` recompiled, clean link, no new
warnings. Both sliders remain live-adjustable in Debug > Graphics
Settings for either value ever needing revisiting (e.g. a future runtime
update changing compositor color handling, same "adjust and rebuild if
this ever needs revisiting" caveat the original constant's comment always
carried).

**Reusable lesson for this whole thread**: a "confirmed correct" tuning
from an earlier session (SteamVR's original ~0.4545, confirmed back in
section 6) turned out to have been confirmed against the wrong reference
(VD/Meta Link's own, also-wrong brightness) rather than an objective
standard — worth remembering before trusting ANY empirically-tuned
constant in this file as permanently settled, especially ones tuned by
comparison to another runtime rather than to a known-correct baseline
(the desktop mirror, once it existed, was what finally exposed this).

**CONFIRMED FIXED IN-HEADSET, all three runtimes — closes out the whole
2026-08-16 "too bright" investigation.** User: "yup they all look right"
— covering the previously-untested Meta Link (sharing the VD/Meta Link
`2.0` default, now confirmed for both, not just VD) alongside the already
-confirmed VD (`2.0`) and SteamVR (`1.0`). No further code changes needed
-- both compiled defaults (`settings.cpp`) already match what's confirmed,
and both remain live-adjustable via their own independent Debug > Graphics
Settings sliders if a future compositor update on any runtime ever changes
its color handling again (the original constant's own comment always
carried this exact caveat). Started from an external tester's #1
complaint about the released mod; root-caused via a runtime-agnostic
"desktop mirror vs. headset" comparison none of this project's prior
gamma work had made before (the mirror feature itself, built 2026-08-10
for an unrelated reason, is what made this diagnosis possible at all);
ended up finding and correcting a second, independent bug (SteamVR's own
multi-session-old "confirmed" baseline was actually tuned against a wrong
reference) along the way that nobody had reason to suspect until real
user data forced a second look. This section is done; no known remaining
gaps.

## Key lesson learned this session

Don't infer that an uncommitted fix supersedes a nearby disable guard just
because they're both present in the diff — ask, or look for explicit
"confirmed/tested" language, before re-enabling. A conversation that resumes
mid-investigation (e.g. after a closed/reopened prompt) cannot tell "fix
landed, disable is stale" apart from "fix was tried and rejected, decision
already made" from the code alone — both look identical in a diff. This bit
us once already on the shadow-stretching guard (see #1).
