# VR single-pass stereo — scoping + implementation plan (handoff, 2026-09-20)

Written to be executed in a fresh session. Read `vr-mod-notes` (the skill)
section "Quest dips ROOT-CAUSED with real phase timing (2026-09-20)" first
for the measurements this plan is built on; this file is the plan, not the
history.

## 0. Why this, and what "done" means

Measured on Quest 3 in a normal outdoor area (`[dusk::vr::perf]`
instrumentation, still in `src/dusk/vr/vr_main.cpp`):

| main thread, per frame          | ms      |
|---------------------------------|---------|
| pre-eye-loop HUD+minimap capture| 1.7     |
| `beginEye()` x2                 | 2.3-2.8 |
| `fpcM_DrawIterater()` x2        | 0.5     |
| `cAPIGph_Painter()` x2          | 3.5-5.0 |
| `endEye()` x2                   | 2.0-3.4 |
| `aurora::gfx::synchronize()`    | 4-6     |
| gap/submit                      | ~0.5    |
| **total**                       | **~17** |

Budget at 72Hz is 13.9ms. The FIFO-processor + render-worker chain also
takes ~16ms per frame (that's what `synchronize` waits on). Being over
budget every frame makes Meta's runtime pace us inside `xrBeginFrame`
(13-18ms blocks, mostly on frames that also ran a sim tick) — those are
the "nasty dips". Frames alternate ~17ms / ~30ms.

Everything marked "x2" above is the same work done once per eye. Doing it
once for both eyes is the only change big enough to get BOTH the main
thread and the FIFO/render chain under ~12ms. Items #1 (mesh culling) and
#3 (perf-settings / thread hints) from the saved ideas list are in and
harmless but don't move this; foveated rendering is not possible on Dawn
(no FDM/VRS).

**Done** = steady 72Hz in that same area with `[dusk::vr::perf]` showing
`renderEnc` ≲ 6ms and `sync` ≲ 3ms, no `xrBeginFrame` blocks, and both
eyes' images correct (stereo depth right, nothing bleeding between eyes,
HUD/aim-dot/hands where they were).

## 1. The constraint that shapes the design

GX has no separate view matrix. `GXLoadPosMtxImm` (`extern/aurora/lib/
dolphin/gx/GXTransform.cpp`) ships **view × model** already multiplied by
the game's CPU code, and the vertex shader does
`out.pos = (in_pos * postex_mtx[pnmtxidx]) * proj` (`extern/aurora/lib/gx/
shader.cpp` ~line 1028). So "replay eye 0's commands with eye 1's view
matrix" cannot be done literally — the view is baked into every matrix
load.

What CAN be done: render the whole stream once from the **head-center
view** `V_c`, and let the shader apply a per-eye correction before
projection:

```
mv_pos_eye = T_eye * mv_pos           // T_eye = V_eye * V_c^-1, a 3x4
clip       = mv_pos_eye * P_eye       // each eye's own asymmetric proj
```

On the Quest 3 the two `XrView` poses share orientation and differ by a
translation, so `T_eye` is ≈ a pure translation of ±IPD/2 along view-space
X; keep it a general 3x4 anyway (some HMDs cant the displays). CPU-side
view-dependent math (env-map texgen, particle billboards, aim dot, HUD
pose, culling) uses `V_c`; the error is IPD/2 ≈ 32mm at scene scale —
imperceptible, and standard practice for single-pass stereo.

The GX projection is the 6-parameter XF form (`regs.cpp` ~line 905:
`m0[0], m0[2], m1[1], m1[2], m2[2], m2[3]`). VR already renders with
asymmetric eye projections through it today, so both eyes' projections fit
this form — no new projection representation needed.

## 2. Recommended design: instanced side-by-side into one double-wide target

Dawn (vendored `v20260807.225922`) has **no multiview**. It does expose
`FeatureName::ClipDistances` (checked in the vendored `webgpu_cpp.h`).
Aurora already uses instancing for line/point expansion
(`command_processor.cpp` ~line 452: `instanceCount = vtxCount` etc.) and
already has a per-draw 64-byte immediate block (`DrawImmediateData`,
`gx.hpp:80`, all 64 bytes used).

Per frame in VR:

1. One offscreen pass, color+depth **double-wide** (`eyeW*2 × eyeH`) —
   exactly the layout the XR swapchain image already has, so the final
   copy becomes ONE blit instead of two per-eye copies.
2. Every GX draw is issued with `instanceCount *= 2`. In the vertex
   shader `eye = instance_index & 1u` (line/point expansions keep using
   `instance_index >> 1u` for what they use `instance_index` for today).
3. Vertex shader (perspective draws while stereo is active):
   ```
   let t   = select(ubuf.stereo_t_l, ubuf.stereo_t_r, eye == 1u);  // mat3x4f
   let p   = select(ubuf.stereo_proj_l, ubuf.stereo_proj_r, eye == 1u);
   let mvp = vec4f(vec4f(mv_pos, 1.0) * t, 1.0) * p;
   // side-by-side remap: left eye -> x in [-1,0], right -> [0,1]
   let xs  = select(-0.5, 0.5, eye == 1u);
   out.pos = vec4f(mvp.x * 0.5 + xs * mvp.w, mvp.yzw);
   // keep each eye's geometry out of the other half:
   out.clip_distances[0] = select(-out.pos.x, out.pos.x, eye == 1u);
   ```
   `clip_distances` needs the `ClipDistances` feature requested at device
   creation (`extern/aurora/lib/webgpu/gpu.cpp`, next to the other
   `HasFeature` checks) and `enable clip_distances;` in the WGSL. Fallback
   if an adapter lacks it: fragment `discard` on the wrong half (works,
   but costs early-Z on Adreno) — implement clip distances first, keep
   discard as the guarded fallback.
4. Orthographic draws (`projType == GX_ORTHOGRAPHIC`) inside a stereo
   pass: same duplication and remap, but `t = identity` and `p = ubuf.proj`
   (the stream's own ortho matrix) — 2D overlays land identically in both
   halves. In practice VR draws no ortho inside the eye pass today (HUD is
   a 3D billboard; the 2D captures run pre-loop in their own passes), but
   don't rely on that.
5. Viewport/scissor: the game's `GXSetViewport`/scissor calls are scaled to
   the pass size by `offscreen_uses_native_logical_size` today. With the
   double-wide pass, keep scaling to a **single eye's** size and let the
   shader do the half-remap — i.e. the viewport set on the Dawn pass must
   be the full double-wide extent while `logical_fb_size()`-based scaling
   still thinks one eye. Check `map_logical_scissor()`/`scale_copy_dst()`
   in `extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp` and the
   `render_viewport_size`/`logical_viewport_size` uniforms (used by
   line/point expansion) — line width should scale by one eye's size.
6. Stereo parameters reach the FIFO thread **through the GX stream**, not
   through a global: add a small aurora extension opcode (same shape as
   the XF register writes in `GXTransform.cpp`) carrying `{enabled,
   proj_l[6], proj_r[6], t_l[12], t_r[12]}`. `command_processor.cpp`
   decodes it into new `g_gxState` fields; `shader_info.cpp`'s
   `build_uniform()` appends them after `proj` (≈160 B/draw extra — fine).
   Because it's in-stream, no `AuroraGXSync()` drain is needed to set it —
   and the `offscreen_uses_native_logical_size` flag (the reason for the
   two drains added 2026-09-20 in `beginEye()`) should ride the same
   opcode, which removes those drains entirely.
7. Shader variants: `stereo` becomes a bit in `ShaderConfig` (pipeline
   cache key) so mono/flat rendering compiles exactly the shader it does
   today — zero effect on flatscreen.

Cost model after the change: main thread does one traversal + one painter
+ one begin/end (~5-6ms); FIFO interprets one stream; render worker
records the same number of draws as mono; GPU does 2x vertex work (cheap
here) and the same fragment work as now.

### Alternative considered (keep as plan B)
Dual-emit at the FIFO level: interpret once, push each `DrawCommand` twice
(two uniform ranges, two passes / two viewports). Simpler shader, but the
render worker still records 2x draws, and the pass-split/resolve logic
(`resolve_pass_into`, protected pass ids) has to be mirrored for two
passes — that machinery has a long bug history here. Only fall back to
this if clip distances + instancing turn out unworkable.

## 3. Dusklight-side changes (`src/dusk/vr/`)

- `vr_stereo_render.hpp`: `beginEye()`/`endEye()` become
  `beginStereoPass()`/`endStereoPass()` — one pass, double-wide. Compute
  `V_c` from `hmdPose` (same math as `eyePoseToViewMtx()` with a zero eye
  offset), set `view->viewMtx`/`j3dSys` to it, set `view->projMtx` to
  either eye's (only the shader's per-eye proj matters). Compute
  `T_l/T_r = V_eye * V_c^-1` and both 6-param projections
  (`eyeFovToProjMtx()` per eye), emit the stereo opcode. Cull frustum =
  symmetric frustum containing BOTH eyes' FOVs (section 2's math, take the
  max over both views) — culling stays on (item #1).
- `vr_main.cpp` `tick()`: replace the `for (eye...)` loop with one pass.
  Per-eye things inside the loop today: `drawAimCrosshair(view->viewMtx)`
  → uses `V_c` (the shader adds the eye offset); `drawMenuBillboard`,
  `drawHudBillboard` → same; `g_duskVRCurrentEyeIndex` consumers (grep) →
  audit; the desktop-mirror `mirrorEyeTargets` → point at the left half
  (or the whole image); the `[dusk::vr::perf]` per-eye laps → collapse.
- `vr_xr_submit.hpp`: `encodeSwapchainCopy()` / the Vulkan shared-image
  blit / D3D12 intermediate copy currently run per eye with `dstXOffset`.
  With a double-wide source they run once for the whole image. Gamma
  compute shader: same, one dispatch over the full width.
- `getEyeSymmetricFov()` (feeds water/reflection fov, section 3 round 1):
  keep returning something sane (union frustum).
- Keep the old two-pass path behind a settings bool
  (`game.vrSinglePassStereo`, default off until confirmed) so A/B testing
  in the same build is one toggle — the notes' standing lesson is to keep
  the proven path until the new one is confirmed in-headset.

## 4. Known interactions to handle (don't discover these in-headset)

- **`GXCopyTex` screen captures inside the eye pass**
  (`retry_captue_frame()`, `m_Do_graphic.cpp` ~2851): the copy source rect
  is scaled from logical fb size to the pass size (`scale_copy_dst()`),
  which with a double-wide pass captures BOTH eyes into the shared 304×224
  capture texture. The only VR consumer left is the underwater motion
  blur (water's reflective draw and the kagerou particles are already
  disabled in VR). Either gate the capture to `camera_water_in_status`
  (saved ideas item #4 — also removes per-frame pass splits on the tiled
  GPU) and accept a wrong-looking blur underwater, or disable
  `motionBlure()` in VR. Decide before testing, not after.
- **Protected offscreen pass** (`set_protected_offscreen_pass`, the
  `resolve_pass_into` substitution history in `vr-mod-notes` section 3/8):
  still one pass, so the existing protection works unchanged — but the
  pass is now the ONLY pass, so any mid-pass split affects both eyes.
- **Depth-based effects**: `depth_peek.cpp` / anything sampling depth with
  screen-space UVs will see the double-wide buffer. Grep `depth_peek`,
  `snapshot_depth` usage from VR paths (there shouldn't be any).
- **`drawHudBillboard()`'s "identity position matrix, eye-space vertices"**
  trick (section 7): with the eye offset applied in-shader the quad gets
  real disparity at 2m — correct, but the luma-key/blend path is
  untouched. `computeHudPose()` re-projects through `view->viewMtx` = `V_c`
  — fine.
- **Line/point instancing** (`command_processor.cpp:452-458`,
  `shader.cpp` lineMode 3 and lines branches): they already use
  `instance_index`; the eye bit must be split off (`& 1u` / `>> 1u`) in
  exactly those three code paths.
- **`ShaderConfig` version bump** (`GXPipelineConfigVersion`,
  `pipeline.hpp`) — new config bit invalidates the on-disk pipeline cache;
  expected, not a bug.
- **Menu billboard heap-corruption fix** (`AuroraGXSync()` before
  `ensure_external_copy_texture()`, 2026-09-19) is unrelated to this and
  stays.

## 5. Verification order (cheapest first)

1. Standalone script: verify the clip-space remap + clip-distance sign
   with a few hand points (x on each side of the center, w>0), and that
   `T_eye = V_eye * V_c^-1` reproduces each eye's real view matrix from
   the center one (use two logged `XrView` poses from a real session).
2. Build with stereo opcode emitted but `T_l = T_r = identity`,
   `P_l = P_r` = one eye's projection: both halves must be pixel-identical
   to each other and to today's left-eye image. This validates
   instancing, remap, clip distances, viewport scaling, the single blit.
3. Enable the real per-eye `T`/`P`: check stereo depth on a near object
   (hands, sword) and a far one; check nothing from one eye appears at the
   other's inner edge (that's the clip distance); check HUD, aim dot,
   minimap, menu billboard.
4. Only then look at `[dusk::vr::perf]` and confirm the numbers in §0.
5. Then remove the perf instrumentation (or keep gated) per the project's
   normal practice.

## 6. Effort / risk

Aurora: new opcode + `g_gxState` fields + uniform layout + shader changes in
3 vertex paths + `ClipDistances` feature request + pipeline config bit —
roughly a session. Dusklight: the eye loop rewrite, submit path, and the
interactions in §4 — another session. Verification: one more. Risk is
concentrated in the shader remap and the viewport/scissor/copy scaling
under a double-wide target; both are script-verifiable before headset
time, which this project's rotation-math history says to do.

## 7. Where the instrumentation lives (to read the results)

`src/dusk/vr/vr_main.cpp`: `PerfClock`/`g_perf*`/`perfLap()` around
`tick()`/`submitFrame()`; log line `[dusk::vr::perf] DIP|base total=…
setup=…(waitFrame= swapWait= beginFrame= syncActions= hmd= ctrl=
mid=…) renderEnc=…(preLoop= begin= iter= painter= end=) gap= sync=
submit= sim= cull=rejected/tested`. Threshold `kPerfDipThresholdMs=22`,
baseline every `kPerfBaselineInterval=600` frames. Cull counters are
`g_duskVRCullTested/Rejected` in `libs/JSystem/src/J3DU/J3DUClipper.cpp`.
Capture with `adb logcat | grep dusk::vr::perf` (one-in-N sampling via
awk keeps it readable).
