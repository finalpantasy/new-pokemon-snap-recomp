# Lighting, shadow, AO and ray tracing plan

Status: plan only, nothing built. Based on static reading; no capture was made for this plan. Items marked "unverified" need the cheap probe named next to them.

Ground rules for every package:

- Opt-in. The default stays the original look.
- One switch per feature: a `SNAP_*` env var plus a native options row (F1 table in `runtime/ui/ui_options.cpp`, env map in `runtime/hle/settings.cpp` `kEnvKeys[]`, in-game row in `scripts/ui/make_native_options.py`). A hand-set env var always wins.
- Never write the Evaluation depth/stencil target (tag EVDP, created at main+0x5c3d3c), the evaluation SSBO, guest memory, or scene `DEPTH32F`. Effects write HDR scene color or their own scratch images only.
- Game data stays local. Nothing here prints or commits game files, keys or generated code.

## 1. What the game does today

Shadows
- One dynamic 1024x1024 orthographic depth map (format id 0x144, created by `0x5b8b10` at `0x5bdd34`), refit every frame around the subject (half-extent about 64 units in the lab yard, about 1.4 near a Pokemon). Built inline in `0x5bd940`. No cascades.
- Static shadow: 3 x 1024x1024 maps built by `0x4d0f50`. The pass enable byte (+0xf0) was 0 in the one pass dump, so it may not render.
- Receivers are the 856 material pixel shaders (CharacterShader, StageShader). Each does 1 or 2 single hardware depth-compare taps (`OpImageSampleDrefExplicitLod`; 177 shaders have 1 tap, 679 have 2). No software PCF, no bias, no texel-size constant. The deferred compute shader has no shadow sampling.
- Caster matrix: 64-byte VS uniform at slot 9 of the ShadowCast draws. Receiver matrix: floats 16..31 of a 132-byte uniform at slot 9. Both are already edited by `ShadowDistanceOnBind` (`runtime/nvn/nvn_draw.cpp:1487-1517`, called from `Op::BindUbo` at `:1558`).
- Fade is separate from coverage: `PerViewCBuffer` (c3) holds `dynamicShadowFarClip` at +0x2e0 and `dynamicShadowFadeLength` at +0x2e4. `SNAP_SHADOW_DISTANCE` never touches them, which likely explains why 2x distance looked unchanged.
- Most course shadows are baked (4,578 Shadow textures, 9,738 lightmaps).
- Resolution path: `runtime/nvn/nvn_entry.cpp:629-671` (`SNAP_SHADOW_RES`, `SNAP_LIGHTING=high` gives 2048). Row at `runtime/ui/ui_options.cpp:203-205`. 4096 gives cleaner Pokemon ground-shadow edges, no acne seen. Frame cost at 4096/8192 not measured.
- A compare binding on a color texture (the game's 8x8 "no shadow map" stand-in) is replaced by a 1x1 D32F=1.0 view (`nvn_draw.cpp:584-589, 900-902`). Any shader patch must keep this "always lit" path.

Ambient occlusion
- The game's SSAO (nodes SSAOCreate `0x5bed60`, BlurX, BlurY, Composite `0x5bf81c`) is depth-only, 1920x1080, HBAO 8 dirs x 32 samples live. It runs only near photo select (about present 20000). In the course it is off; contact darkening there is baked.
- `SNAP_SSAO_QUALITY` (hooks in `host/mods/gamemods.cpp:61-72`) therefore has no visible effect in the course.
- The pass toggle hook (`0x989048`, `rt_gm_pass`) can only turn passes off; SSAO names are refused unless `SNAP_PASS_FORCE=1`.

Lighting
- Stage and character materials are forward shaded into one RGBA16F target (1,222 pixel programs write one target). Indirect light is baked SH, lightmap and cubemap. The G-buffer program `nuFeGBufferPS` and the tiled deferred compute `nuFeTiledDeferredShadingCS` exist; whether they run in the course is unverified (one `NVN_DRAW_LOG` capture settles it). Cache key prefix of the deferred CS is `718ce2eb78a2` (by shape).
- Water reflection and refraction are cubemap lookups. No SSR, planar reflection or ray tracing code exists anywhere in `runtime/`.

Host facts the plan builds on
- Replay: `DoDraw` (`nvn_draw.cpp:926`), `DoDispatch` (`:1174`), `ReplayDrawOp` (`:1519`). Render targets in `cx.rt[0..8]`.
- Scene targets the upscaler already finds: scene depth 1920x1080 DEPTH32F (reversed, infinite far), scene color 1920x1080 RGBA16F linear, camera from c3 (viewProjection +0x100, projection +0x80, camera position +0x240) in `runtime/nvn/upscale/upscale.cpp:6-12, 218-281`. Camera state is in an anonymous namespace and not exported.
- Same-frame A/B precedent: `SNAP_AA_SPLIT`, `SNAP_AA_AB`, `SNAP_AA_DUMP` (`nvn_aa.cpp:50, 81, 299, 357, 403`).
- Post-pass precedent: `AaAfterDraw` (`nvn_aa.cpp`, called at `nvn_draw.cpp:1148`).
- Device creation: `runtime/nvn/nvn_vk.cpp:271-353` (Vulkan 1.3). No acceleration structure, ray query, buffer device address or transform feedback yet.
- Photo scoring reads only game-side data plus GPU depth/stencil visibility counters from the Evaluation passes. A color-only effect cannot change those counters. It does appear in saved photos and thumbnails (Capture3D, 360x240 targets carry lit color).

## 2. Ranked work packages

Ranking is by value per effort. Quick wins first (hours to 2 days), then medium work, then the RT prototype (section 4).

Common acceptance (applies to every package, "std checks"):
1. A/B screenshots: same-frame split or dump pair (`SNAP_*_SPLIT` or `NVN_FRAME_DUMP`), original vs feature, saved under `analysis/mods/compare/`.
2. Score: route `florio_eval`, feature off then on, one run at a time at BelowNormal. Compare `NVN_EVAL_DEPTH_DUMP` sheets for matching frames and the distribution of visible fractions from `NVN_XFER_LOG=1`. The route is not deterministic, so compare distributions (median and range), never single photos. Pass condition: no shift beyond run-to-run noise.
3. Cost: frame GPU busy time at 4K (render scale 2) on the RTX 4090, feature off vs on, median over a fixed route segment. Report the delta in ms.
4. Vulkan validation clean: run with the validation layer on, zero new messages.
5. Default-off check: with no `SNAP_*` set, the output is bit-identical to before (frame dump hash).

### Quick wins (hours to 2 days)

**P1. Probes (hours).**
- Goal: settle the three cheap unknowns that gate later packages.
- Hooks and files: log the game's compare-sampler filter and the Dref sampler formats at `nvn_gfx.cpp:286` (`SamplerInitialize`); log render-target formats and the deferred CS dispatch count with the existing `NVN_DRAW_LOG`; confirm the polygon-offset enable on ShadowCast via `NVN_SHADOW_LOG` (`nvn_draw.cpp:1082`); log SSAO pass allocation when forced. Files: `runtime/nvn/nvn_gfx.cpp`, `runtime/nvn/nvn_draw.cpp` (log lines only).
- Switch: none (env-gated logs, `NVN_*`).
- Acceptance: one muted run, a text note in `analysis/` with the answers. Default-off check only.
- Effort: 0.5 day plus one run slot. Dependencies: none.

**P2. Lighting preset row goes live plus an "Ultra" step (0.5 day).**
- Goal: turn the disabled `gfx_lighting` "coming soon" row into a working Original / High / Ultra control.
- Hooks and files: `runtime/ui/ui_options.cpp:227` (change `kSoon` to `kRestart`), `runtime/hle/settings.cpp` (`gfx_lighting` to `SNAP_LIGHTING` already mapped), `scripts/ui/make_native_options.py` (Textures tab row), `host/mods/gamemods.cpp:13` (parse `ultra` = 4096 shadow maps, 4x distance, SSAO ultra). Run `tests\hle\run_options_test.ps1`.
- Switch: `SNAP_LIGHTING=high|ultra`. Row label: "Lighting".
- Acceptance: options test passes (saved values and exported env), F1 screenshot via `tests\hle\opt_shots.py`, std checks 1-5 at ultra.
- Effort: 0.5 day. Dependencies: none (best done after P3 so the preset includes the fade fix).

**P3. Shadow host knobs: fade distance and bias (1.5 days).**
- Goal: make `SNAP_SHADOW_DISTANCE` really extend where shadows fade out, and add a bias scale for large maps.
- Hooks and files: extend `ShadowDistanceOnBind` (`nvn_draw.cpp:1487`) with a sibling for the c3 `PerViewCBuffer` bank that multiplies `dynamicShadowFarClip` (+0x2e0) and `dynamicShadowFadeLength` (+0x2e4) by the same k, idempotent like the existing "done" map. Find the buffer by float layout using `NVN_SHADOW_LOG`. Add `SNAP_SHADOW_BIAS` that scales biasFactor and biasUnits in `Op::PolygonOffset` (`:1580`, applied at `:1116`) only when the bound target is the 1024-class shadow map (the `rt[8]` test at `:1489`). Rows in `ui_options.cpp`.
- Switch: `SNAP_SHADOW_DISTANCE` (existing, now effective), `SNAP_SHADOW_BIAS=<scale>`. Row labels: "Shadow distance", "Shadow bias".
- Acceptance: std checks; A/B in an open area (Florio ride) at distance 1x vs 2x showing a visible change; no acne at 4096.
- Effort: 1.5 days. Dependencies: P1 (offset-enable answer, c3 layout confirmation).

**P4. Force the game's SSAO on in the course (2 days).**
- Goal: get existing HBAO into the course render and test whether the composite works when the game has it off.
- Hooks and files: add an "on" list to `rt_gm_pass` (`host/mods/gamemods.cpp`, hook `0x989048`) that sets +0xf0=1 on SSAOCreate/BlurX/BlurY/Composite after the game's own setter. Verify depth is complete before SSAOCreate and the blend is multiply with `SNAP_PASS_DUMP=1` and `SNAP_GM_TRACE`. Tune with existing `SNAP_SSAO_QUALITY`, `SNAP_SSAO_HBAO`, `SNAP_SSAO_RADIUS`, `SNAP_SSAO_STRENGTH`.
- Switch: `SNAP_SSAO_FORCE=1`. Row label: "Ambient occlusion in course".
- Acceptance: std checks; no double darkening against baked AO in a lightmapped scene; game still behaves when it later disables the passes.
- Effort: 2 days. Dependencies: P1. Risk: the passes may be invalid when game-disabled; this package decides it.

### Medium work (3 to 7 days)

**P5. Wider shadow PCF via a SPIR-V patch of the Dref taps (4 days).**
- Goal: replace each single hardware tap in the 856 receiver shaders with an 8 or 16 tap rotated Poisson or grid kernel for soft, smooth edges at 2048-4096. Best look per cost.
- Hooks and files: new translator patch next to `scripts/xlate_patches/0010` (bump `SNAP_XLATE_REV`, new cache folder) or a post-pass in the fragment-variant path (`nvn_shader.cpp:245, 326, 375`, `fragMods` in `nvn_internal.h:311, 473`, variant keys in `nvn_pipeline.cpp`). Texel size from `OpImageQuerySizeLod` on the depth image. Kernel size via specialization constant (0 = original). Compare sampler must be linear (check at `nvn_gfx.cpp:286`, answer from P1). Keep the dummy-shadow path (`nvn_draw.cpp:584-589`) working. This is the shared patch point that RT shadows (P10) later reuse.
- Switch: `SNAP_SHADOW_FILTER=0|8|16`. Row label: "Shadow softness" (Off, Medium, High).
- Acceptance: std checks with `spirv-val` clean on all 856 patched programs; A/B at 2048 and 4096 on a Pokemon ground shadow; cost reported at render scale 2 for 8 and 16 taps.
- Effort: 4 days. Dependencies: P1 (sampler), P3 (bias knob so wider kernels do not show acne).

**P6. Shared effect plumbing, "Fx" module (3 days).**
- Goal: do the plumbing once so P7, P8, P9 are small. Provides: camera accessor exported from the upscaler, scene depth/color finders, a post-pass hook slot, a photo-frame and Evaluation-target guard, and same-frame A/B tools.
- Hooks and files: new `runtime/nvn/nvn_fx.cpp/.h` with `FxCamera()`, `FxSceneDepth()`, `FxSceneColor()`, `FxIsPhotoFrame()` (reuse the upscaler's photo-frame look-ahead, `upscale.cpp ~660-690`), an assertion that no written image is the EVDP target or a depth format. A small accessor added to `runtime/nvn/upscale/upscale.cpp`. Pre-place the one-line hook calls in `nvn_draw.cpp`: after the tonemap draw (like `AaAfterDraw` at `:1148`), at the upscaler's pre-tonemap point, and at the end of `DoDispatch` (`:1203-1210`, deferred CS key `718ce2eb78a2`). Add `SNAP_FX_SPLIT=1` and `SNAP_FX_AB=<P>` copied from `nvn_aa.cpp`.
- Switch: `SNAP_FX=1` master gate (individual effects have their own switches). No row of its own.
- Acceptance: with the module on and no effect enabled, output is bit-identical; guard test: a deliberate bad write to the EVDP target trips the assertion.
- Effort: 3 days. Dependencies: none. Landing it first lets later packages avoid editing `nvn_draw.cpp`.

**P7. Screen-space contact shadows (4 days).**
- Goal: add small-scale contact darkening under Pokemon, props and foliage that 1024-4096 maps miss.
- Hooks and files: new `runtime/nvn/nvn_contact.cpp` plus a host SPIR-V header (build like `nvn_aa_spv.h`, `make_spv.py`). Compute pass at the Fx pre-tonemap hook (the game is forward shaded, so the deferred CS point may not exist; P1 decides). Reads scene depth, derives normals from depth, marches 16-24 steps toward the light direction taken from the caster matrix rows 0-2 of the slot-9 buffer (already visible in `ShadowDistanceOnBind`), multiplies HDR color. Skips photo frames and the Evaluation target.
- Switch: `SNAP_CONTACT_SHADOWS=0|1`. Row label: "Contact shadows".
- Acceptance: std checks; no darkening where baked shadows already exist (lerp by distance and strength); cost under 0.5 ms at 4K.
- Effort: 4 days. Dependencies: P6.

**P8. GTAO replacement of the HBAO shader (5 days).**
- Goal: much better AO quality than HBAO (cosine-weighted horizons, thickness heuristic, multi-bounce) using the game's own blur and composite.
- Hooks and files: GLSL compiled offline to SPIR-V, substituted for `SSAOHorizonBasedAOPS` by content hash in `runtime/nvn/nvn_shader.cpp` (same descriptor layout; HBAO cbuffer offsets are known from the nushdb reflection). Quality map: `SNAP_SSAO_QUALITY` to slice and step counts. Output AO in the channel the composite reads (.x); do not pack bent normals in v1 because the game's bilateral blur would damage them.
- Switch: `SNAP_SSAO_GTAO=0|1` (on top of `SNAP_SSAO_FORCE`). Row label: "AO quality: GTAO".
- Acceptance: std checks; compare to HBAO at the same radius; no halos at depth edges; under 1 ms at render scale 2.
- Effort: 5 days. Dependencies: P4.

**P9. Water screen-space reflections (7 days).**
- Goal: reflect Pokemon, rocks and trees on water instead of a static cubemap, falling back to the cubemap when the ray leaves the screen.
- Hooks and files: identify water draws (Sea pass `0x5c09f0`; programs such as the `*_Sea_*` and `*_Water_*` stage shaders, 56 programs carry `RefractionRatio_BgCube`/`FrontReflection`) with one `NVN_DRAW_LOG` capture. Copy scene color at the first Sea draw, run a hierarchical depth ray march, blend by Fresnel and an edge fade, either by patching the water pixel shaders' cubemap fetch (xlate patch series) or by a post pass over a water mask. Files: new `runtime/nvn/nvn_ssr.cpp`, patch under `scripts/xlate_patches/`, Fx hooks.
- Switch: `SNAP_WATER_SSR=0|1`. Row label: "Water reflections".
- Acceptance: std checks; A/B on a beach or river course; photos of Pokemon in water change color but must not move the score distribution.
- Effort: 7 days. Dependencies: P6, P1 (draw capture).

### Later (not in the first wave)

- **P11. Host GTAO compute with temporal accumulation and bent normals** (10 days): `nvn_fx` pass independent of the game's SSAO objects; needs stable pass tagging because the layer does not know pass names today. Only worth it if P8 shows the game's blur is the limit.
- **P12. SSIL / one-bounce screen-space GI** (12 days): risk of double lighting scenes that already have lightmap bounce; no albedo buffer in forward shading. Defer until RT results show the value.
- **P13. Contact-hardening PCSS, second far shadow map** (8 and 10 days): both need a descriptor-layout or guest-recording change across 856 programs and give less than P5 plus P3. Defer; do the far map only if P3 shows the open courses lack far shadows.
- **Remix-style path tracing of everything:** not recommended (about 40 days, fights the baked lightmaps and art direction, high scoring risk).

## 3. Parallel work and file ownership

Rule: `runtime/nvn/nvn_draw.cpp` and `nvn_shader.cpp` are hot files. P6 pre-places the hook call sites in `nvn_draw.cpp` so later packages mostly edit their own new files. The one exception is P3 (shadow region of `nvn_draw.cpp`, lines near 1480-1590) which owns that region until it lands. Options rows touch shared tables, so each package adds its rows in a separate commit and rebases; merge conflicts there are mechanical.

| Package | Owns (may edit) | Must not touch |
|---|---|---|
| P1 probes | log lines in `nvn_gfx.cpp`, `nvn_draw.cpp` (isolated lines), new `analysis/` note | anything else |
| P2 preset row | `ui_options.cpp` (gfx_lighting row), `settings.cpp`, `make_native_options.py`, `gamemods.cpp` (SNAP_LIGHTING parse only) | `nvn_draw.cpp`, `nvn_shader.cpp` |
| P3 shadow knobs | `nvn_draw.cpp` shadow region (1480-1590), `ui_options.cpp` (shadow rows) | `nvn_shader.cpp`, `gamemods.cpp` |
| P4 force SSAO | `host/mods/gamemods.cpp` (`rt_gm_pass`, SSAO hooks), SSAO row | `nvn_draw.cpp`, `nvn_shader.cpp` |
| P5 PCF patch | `nvn_shader.cpp`, `nvn_pipeline.cpp`, `scripts/xlate_patches/`, shadow filter row | `nvn_draw.cpp` |
| P6 Fx module | new `nvn_fx.cpp/.h`, `upscale.cpp` accessor, hook lines in `nvn_draw.cpp` | shadow region of `nvn_draw.cpp` |
| P7 contact shadows | new `nvn_contact.cpp` and `nvn_contact_spv.h`, its row | everything else |
| P8 GTAO | `nvn_shader.cpp` (hash substitution block, separate function), new GLSL and SPIR-V header, its row | `gamemods.cpp` |
| P9 water SSR | new `nvn_ssr.cpp`, new patch under `scripts/xlate_patches/` | `nvn_shader.cpp` hash block of P8 |

Safe concurrent groups:
- Wave A (all at once): P1, P2, P3 (after P1 gives it its answers, otherwise just start the probes), P4, P5-design, P6. P3 and P6 both touch `nvn_draw.cpp` but in different regions; P6 should land its hook lines first.
- Wave B (after P6 and P1): P7 and P9 in parallel (new files only); P8 after P4; P5 after P3.
- Serial pairs: P3 then P5 (bias before wide kernels); P4 then P8; P5 then RT shadows (P10); P6 then everything that uses Fx.
- `nvn_shader.cpp` is touched by P5, P8 and later P10. Run them one at a time or give each a separate function and a separate hook line.

Run-slot rule for all packages: one heavy process at a time, BelowNormal, under 10 minutes, kill by PID only. `florio_eval` runs are the scarce resource, so batch score checks: one off run (baseline) shared by all packages, then one on run per package.

## 4. Ray tracing prototype plan

Why RT is worth only a staged approach: draw volume is small (98 draws per frame, max 274), and the GPU is a 4090, so GPU time is fine. The limits are (a) most lighting is baked, so there is little for RT to replace, (b) characters are skinned in a GPU compute pass so their final vertices are not in guest memory, (c) the host has no scene registry. The honest value is crisp, resolution-independent Pokemon and prop shadows, plus off-screen-correct AO and water reflections. Do P3, P5 and P7 first; they show how much RT can add.

Switches: `SNAP_RT=0|1` master (device features), then `SNAP_RT_SHADOWS=0|1|soft`, `SNAP_RT_AO=0|1`. Rows: "Ray tracing" (Off/On), "RT shadows", "RT ambient occlusion". All off by default; if the device lacks the extensions the rows stay off and a log line says why.

**M0. Capability and plumbing (1 day).**
- Enable `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_buffer_device_address`, `VK_EXT_transform_feedback` (and deferred host operations if required) in a new has() block next to `nvn_vk.cpp:316-345`, behind `SNAP_RT`. Pattern: `UpscaleDeviceExtensions` at `nvn_vk.cpp:347`.
- Acceptance: device creates with RT on, validation clean, default path unchanged. Files: `nvn_vk.cpp`.

**M1. Geometry capture spike (4 days).**
- Capture world-space triangles of the shadow-cast draws and main-pass opaque draws into host-owned triangle-soup buffers using transform feedback, so game pool buffers need no device-address flags (pools are created without them at `nvn_vk.cpp:664-684`).
- Add `SpvAddXfbPosition` (Xfb decoration on the Position builtin) next to `SpvAddStorageImageCaps` (`nvn_shader.cpp:245, 326, 375`) for vertex stages when RT is on. Wrap `vkCmdDraw*` (`nvn_draw.cpp:1135-1146`) with bind and begin/end transform feedback for cast draws (test at `:1493`) and scene opaque draws. Convert clip to world with the inverse c3 viewProj for scene draws and the inverse slot-9 cast matrix for cast draws. Calibrate the Y flip and z range once against c3.
- Log triangle counts per frame (no note has them), draws per pass, and capture cost.
- New files: `runtime/nvn/nvn_rt.cpp/.h`. Resolve the "3 compute programs in census vs about 20 skinning dispatches" mismatch here.
- Acceptance: triangle log, capture cost under 1 ms GPU at 4K, validation clean, no change to game output.

**M2. BLAS/TLAS and a debug view (2 days).**
- Build a BLAS per capture (rebuild each frame at first; fast-build flag) and one TLAS after the shadow-cast pass. Fullscreen debug overlay with a ray query showing hit distance or a sun-shadow mask on screen. Overlay is a separate switch `SNAP_RT_DEBUG=1`.
- Acceptance: overlay matches the on-screen geometry (screenshots), TLAS build under 1 ms at 4K, with the numbers measured.

**Status of M0-M2 (2026-11-14, rt worker; code in `runtime/nvn/nvn_rt.{h,cpp}`, switches in `host/README.md`).** Built and run on the RTX 4090 with `SNAP_RT=1 SNAP_RT_DEBUG=1`, route `core_florio_a`.
- M0 done. Device extensions, the `bufferDeviceAddress` feature and the ray-query properties are set up behind `SNAP_RT`; with it unset nothing changes. Only the layer's own buffers carry device-address flags, so game pools are untouched.
- M1 done in a different shape than written above. Every vertex shader gets a SPIR-V patch that decorates `gl_Position` for transform feedback (harmless unless a capture is active). A scene opaque draw (1920x1080 DEPTH32F target, depth write on, no blend, not a photo frame) is captured by the same draw call that renders it: post-VS clip positions, expanded to a triangle list, then a compute pass maps them to world space with the inverse c3 viewProj. Per-frame submitted scene draws are only 30-110 and 90k-320k triangles (14 MB of capture at most). The shadow-cast pass is not captured yet (M3).
- M2 done. One BLAS per draw, kept in a persistent cache keyed by draw identity (vertex shader, buffer addresses, draw range, per-object constants), plus one TLAS per frame built at the tonemap draw. Overlay sanity (ray hit vs scene depth, 1/16 of pixels): 99.2-99.9% match in steady gameplay at 1080p and 1440p; the rest is rays hitting slightly in front of the depth (distant backdrop, depth precision at the far plane). Measured GPU cost: convert + BLAS + TLAS 1.2-2.0 ms at typical frames and 3.2-3.6 ms at the busiest (110 draws, 316k triangles); this does not depend on resolution. The debug overlay trace (one ray per pixel, never shipped) costs about 10 ms at 1440p and 12 ms at 4K. CPU: capture 0.1-0.5 ms plus build recording 0.5-2.6 ms per frame. Memory: arena and BLAS store peak at about 112 MB and 130 MB.
- Misses against the M2 targets: the TLAS and BLAS build is above the "under 1 ms" target (1.2-3.6 ms) because every visible draw is rebuilt each frame.

**What geometry the capture can and cannot give (honest limits).**
- It only sees draws the game submitted this frame, that is, after the game's own frustum culling. Off-screen geometry the game culled this frame is not available. Static world geometry accumulates in the cache as the camera moves along the rail and stays until its time-to-live ends (`SNAP_RT_TTL`, 900 frames), so a caster just behind the camera can still shadow the view, but something the player never had in view is absent.
- Moving objects freeze at their last captured position until that time-to-live ends; skinned characters are captured after the skinning pass (the vertex shader sees the skinned positions), so they are correct while visible.
- Draws whose buffers are ring-allocated per frame produce a new key every frame and live only a couple of frames (this is intended, they are dynamic).
- Alpha-tested geometry is treated as opaque. Primitive-restart tails can leave stale triangles.
- Shadow casters the game draws only in its shadow-cast pass (dynamic casters) are not in the cache yet; static shadows are baked.

**M3 design notes (not started).**
- The receiver position comes from the Dref tap and lies on a surface already in the TLAS, with no normal. Self-intersection handling is core: use a small tmin, offset along the sun direction, and an instance mask or a per-draw receiver id so the receiver's own BLAS is skipped where possible.
- The shadow-cast pass must be captured too (cast matrix in slot 9): a second capture mode that maps clip positions with the inverse cast matrix instead of the camera's.
- Ray queries in the 856+ fragment programs raise compile time and register pressure; prefer patching only the dynamic-map taps (P5's patch point) and measure pipeline compile time before widening.

**Revised effort (after M0-M2).** M3 is 8-10 days as written, 4 days of it spent on cast-pass capture and self-intersection; the AS plumbing is done. M4 (RTAO) drops by about 2 days (capture and TLAS exist) but still needs a normal source (depth derivative or the game's normal target) and a denoiser: 7-9 days, 4-10 ms at 4K still to be measured. The decision gate after M1 passes: triangle counts and capture cost are low. The remaining gate is whether the world-space TLAS built from culled geometry covers enough of the shadow-relevant world.

**M3. RT sun shadows (10 days, MVP).**
- Reuse P5's Dref patch point. A new `SpvPatchShadowDref` turns each dynamic-map Dref tap into a ray query (TerminateOnFirstHit) from the receiver position, rebuilt from the sample's (u, v, depthRef) with the inverse cast matrix and the sun direction from the cast matrix Z axis. Pass the TLAS address and a 3x4 matrix via push constants (not yet used by game pipelines; add a range at `nvn_shader.cpp:490-492`) or a set 4. Out-of-box returns 1 (lit), as now. Keep the shadow-dummy path.
- Steps: (a) hard shadows for opaque casters; (b) soft shadows with a jittered sun cone and blue noise, `SNAP_RT_SHADOWS=soft`; (c) alpha-tested casters (ShadowAlphaTexture shaders) later via a candidate loop; v1 treats them as opaque.
- Files: `nvn_shader.cpp`, `nvn_pipeline.cpp`, `nvn_draw.cpp` (push constants and xfb hook lines), `nvn_rt.cpp`, rows in `ui_options.cpp`, `host/mods/nativeopt.cpp` and `make_native_options.py`.
- Acceptance: std checks; RT off equals original; RT on compared with P5 at 4096 (decision point: stop here if RT adds nothing visible); under 3 ms at 4K worst case; RT disabled on Evaluation and photo frames.

**M4. RTAO in the SSAO slot (9 days).**
- Needs M1 (scene-opaque capture), M2, and P4 (SSAO force proven). At the SSAOCreate draw, replace the program with a host fullscreen pipeline that reads the bound depth and normal textures, casts 2-4 short cosine rays (0.5-2 m), accumulates temporally with a bilateral denoise into the same 1920x1080 target; keep the game's blur and composite. Add a distance cutoff and strength so baked AO is not doubled on the static world.
- Acceptance: std checks; 4-10 ms at 4K measured (decision point on cost); A/B in a creature close-up.

**M5. Optional: water reflections via ray query (after M1-M2).**
- Reflection rays from water pixels into the same TLAS with the cubemap as the miss fallback. Compare to P9; build only if P9 shows screen-space limits matter. Roughly 7 more days after M2.

Decision gates:
- After M1: if triangle counts or capture cost are too high, stop and fall back to screen-space (P7, P9).
- After M3: if RT shadows are not visibly better than P5 at 4096, skip M4 and keep P5 as the answer.
- Scoring gate before any RT feature is advertised: two `florio_eval` runs (off, on), distribution compared per the std check 2.

## Order summary

1. Today: P1 (probes), P2 (preset row), P6 (Fx plumbing) in parallel, then P3.
2. This week: P4, P5.
3. Next: P7 and P8, then P9.
4. RT: M0, M1, M2 start once P6 lands (they use separate files); M3 after P5; M4 after P4 and M2.

## Open risks

- Whether the deferred CS runs in the course (P1 answers; it decides where P7's hook goes).
- Compare-sampler filter and which map each of the 1-2 Dref taps reads (P1 answers; gates P5 and M3).
- Forcing SSAO on in the course may fail (P4 answers; gates P8 and M4).
- Every effect that alters lit color shows up in saved photos and thumbnails. That is expected, but scoring needs the `florio_eval` checks above because no lighting mod has been scored yet.
