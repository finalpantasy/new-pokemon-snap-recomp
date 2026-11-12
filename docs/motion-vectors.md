# Motion vectors for DLAA / DLSS (design study)

Read-only study, 2026-11-12. Static analysis plus reads of the shipped shader reflection. No code was changed and the game was not run. Addresses are module offsets of `main` (build `DDB900FF...`) unless noted.
Confidence: H = several independent facts agree, M = one solid fact plus inference, L = guess that needs a run.

Overlap note: another worker is writing the upscaler front end in `runtime/nvn/upscale/` (`upscaler.h`, `upscale_nvn.h`, `upscale_dlss.cpp`, `shaders/upscale_mv.comp`) while this study was done. It already does tier 0 below (camera-only vectors from depth, jitter through viewports). Section 6 says what this study adds and what to check against that code.

## 1. Findings

1. The game has no velocity target, no TAA and no history buffer. FXAA is spatial. (H)
2. The camera matrices are in one constant buffer, `PerViewCBuffer`, with the current frame only. There is no previous-frame matrix anywhere in the buffers, so the host must keep last frame's copy. (H)
3. Every moving object is a `CharacterShader` draw. Its vertices come out of GPU skinning compute, so previous positions exist only if the host keeps last frame's skinned buffer. (H for the shader split, M for the buffer reuse)
4. Cheapest correct route: camera-only reprojection from depth (tier 0), then a velocity write for `CharacterShader` draws only (tier 1). Skip a full velocity pass for the world. (M)

## 2. Does the game already have a velocity target? No.

Evidence:

- `work/v262144-main.strings.txt` has no hit for velocity, motion vector, TAA, temporal, jitter, reproject, prev view or last frame. The only matches are `Wwise_Motion`, `ScenarioEmotion_Lines` and NEX packet history. (H)
- The 278 shader base names in `analysis/shaders/census.tsv` have no velocity, motion, temporal or history entry. The only anti-aliasing shaders are `FXAAOnlyPS`, `TonemappingFXAAPS`, `TonemappingLutFXAAPS`, `TonemappingLutFXAADistortionPS`. (H)
- Reflection of the three per-draw constant buffers (section 3) lists no previous-frame member. The 37 pass names (`render_graph.md` section 4) include ZPrepass, Opaque, Effect, BlendBuffer, Bloom, DOF, Final, UI, Fade and none for velocity. (H)
- The only render targets with a fixed size are the 1920x1080 scene targets, the 960x540 chain and the 192x108 bloom target (`analysis/enhancements/knobs.md`). No RG16F target was found by name. Formats were not decoded from the allocation sites, so a hidden RG16F target is excluded only by the three points above. (M)

## 3. Matrices and constant buffers

The shader reflection (`scripts/nushdb_parse.py`, 497 of the Resident programs sampled, all consistent) gives these layouts. NVN slot = reflection binding + 3 (`docs/nvn-vulkan-design.md`, "Binding model"). The layer maps them to Vulkan set 0 (design doc, "Binding model").

| Buffer | Reflection binding | NVN slot (`c[n]`) | Size | Stage | Confidence |
|---|---|---|---|---|---|
| `PerViewCBuffer` | 0 | c3 | 752 (0x2f0) | VS and PS | H |
| `PerWorldCBuffer` | 1 | c4 | 352 (0x160) | VS | H |
| `UpdatePerObject` | 3 | c6 | 128 (0x80) | VS and PS | H |

`PerViewCBuffer` members (offsets in bytes, each matrix is four vec4 = 64 bytes):

| Offset | Member |
|---|---|
| 0x000 / 0x040 | `viewMatrix` / `viewInverseMatrix` |
| 0x080 / 0x0c0 | `projectionMatrix` / `projectionInverseMatrix` |
| 0x100 / 0x140 | `viewProjectionMatrix` / `viewProjectionInverseMatrix` |
| 0x240 / 0x250 | `cameraPosition` / `cameraDirection` |
| 0x2a0 | `screenSize` (vec2) |
| 0x2a8, 0x2ac, 0x2b0 | game timer, absolute timer, cut-scene timer (seconds) |
| 0x2b4, 0x2c4, 0x2c8 | `exposureReciprocal`, `currentEV`, `currentExposure` |
| 0x200 | `preComputedWindDirection` (foliage wind) |
| 0x2e0, 0x2e4 | `dynamicShadowFarClip`, `dynamicShadowFadeLength` |

`PerWorldCBuffer` members: `worldMatrix` +0x00, `worldInverseTranspose` +0x40, `worldViewProjectionMatrix` +0x80, `billboardWorldMatrix` +0xc0, then padding.

Facts that matter for vectors:

- Matrices are stored column-major as four column vec4s. The shipped GLSL of `NormalTangentTexcoordPositionWorldSpaceVS` does `gl_Position = x*m[0] + y*m[1] + z*m[2] + m[3]`. (H)
- Objects are drawn with the baked `worldViewProjectionMatrix`, not with `viewProjection * world`. So changing `PerViewCBuffer` does not move objects. This is why jitter must go through the viewport (section 5). (H)
- There is no previous-frame member in any of the three buffers. (H)
- Depth is near-is-zero 0..1 (host layer, design doc). The projection matrix gives near, far and field of view at run time: `P[2][2]`, `P[3][2]`, `P[1][1]`. (M)
- Scene views other than the main camera also bind c3: shadow maps (1024x1024), evaluation spheres, photo passes. Tell the main view apart by its depth target (the other worker does this in `UpscaleOnSceneDraw`). (H)

Who writes `PerViewCBuffer`: not found statically. The layout string `ConstantBufferLayoutPerView` is referenced only from the pass-graph builder `0x5bd940` (at `0x5bde90`, `0x5be4e0`), which creates the layout; the fill code is behind a plug object (`Ccc4208` family, `0x5b8340` ctor). Camera globals found by the LOD work (`host/mods/lodanim.cpp`): camera object `main+0xcfb480`, vertical fov `main+0xcfb4c8`, aspect `main+0xcfb4cc`; perspective builder `0x38b710` (call site `0x38cdbc`, gameplay camera, fov 33 deg in the yard). (M, from that worker's static and dynamic notes)
Cheapest way to find the writer: guard-page the bound c3 range (the layer already has `NVN_WB_WATCH` machinery) and log the writer's stack. Not needed for tiers 0 and 1, because the layer reads the buffer at draw time.

## 4. The skinning compute (character vertex output)

The skinning programs are not in the nushdb files. They are plaintext NV_gpu_program5 compute programs inside GLSLC binaries in `main` rodata, each with `GROUP_SIZE 256`. Reflection names: `ForGPUSkinning`, `SkinningParam`, `PoseMatrix[0]`, `sourceStruct`, `influenceStruct`, `nearestBonesStruct`, `destinationStruct`, `dataPerBone[].dualQuaternion`, `enableBulgeCorrection`.

| Binary (module offset, size) | What it is | SSBOs | Confidence |
|---|---|---|---|
| `0xca2780` (0x5600) | matrix-palette skinning, `PoseMatrix` cbuffer | 0 source, 1 influence, 2 destination | H |
| `0xca7d80` (0x54c8) | same, no source struct | 1 influence, 2 source and destination | M |
| `0xcad248` (0x7168) | dual-quaternion with bulge correction | 0 source, 1 influence, 2 destination, 3 nearest bones | M |
| `0xcb43b0` (0x7038) | same, no source struct | 1 influence, 2 in and out, 3 nearest bones | M |
| `0xcbb3e8` (0x7af8) | morph or blend-shape: base, deltas, weights | 0 base, 8 deltas, 2 destination; cbuffers 2 and 3 | M |

How they are used:

- The five programs are created in `0x95f210` (stored at object offsets +0x40, +0x60, +0x80, +0xa0, +0xc0). It sits in vtable slot `0xcc2f10`. (H)
- There are exactly two `DispatchCompute` call sites in the game: `0x95be00` and `0x960a30` (`analysis/runtime/nvn_usage.tsv`). Both group count = `(numberOfVertices >> 8) + 1`. `0x960a30` copies 0x200 bytes of pose matrices and 0x20 bytes of `SkinningParam`; `0x95be00` copies 0x50 bytes of params into a per-frame constant ring (256-byte aligned, ring object at data `0x71012febc8`). (H for the sites, M for which program each one drives)
- The call goes through the loader-filled pointer `main+0x1489a98` (`nvnCommandBufferDispatchCompute`). (H)
- About 20 dispatches per frame in the lab and yard (design doc, "Characters and Pokemon invisible"). The destination SSBO is the same pool range later bound as the character vertex buffer. The layer marks it GPU-written (`MarkGpuWritten`, `nvn_draw.cpp` line 770) and never copies it back. (H)
- Character vertex shaders (`CharacterShader_*VS`) take only `in_POSITION0`, `in_NORMAL0`, `in_TANGENT0`, `in_TEXCOORD0`. No bone indices. So skinning is finished before the draw and the vertex positions are final model-space positions. (H)
- The in-place variants (`0xca7d80`, `0xcb43b0`) read and write the same SSBO. If the game relies on the CPU re-uploading bind-pose data each frame, the GPU-owned rule would skip that upload. Character screenshots look right, so these variants are either unused or use ranges the CPU never rewrites. Unverified; check with `NVN_DRAW_LOG` before building tier 1. (L)

Shader families (counts from `census.tsv`):

- `CharacterShader`: 1,369 programs per stage, 96 unique VS and 714 unique PS code bodies. All Pokemon and characters. (H)
- `StageShader`: 1,010 programs per stage, 134 unique VS and 463 unique PS. All static world meshes. (H)
- `SkyShader` (44), `Constant` (7), `vdkEfx*` (particles and effects), `nuPreZ`.
- This split (moving versus static) is the cheapest handle for deciding which draws need object motion.

## 5. Hooks DLSS needs

Jitter:

- Use the viewport. Add a sub-pixel offset to every viewport emitted for scene targets (x right, y down). It moves every draw with no shader change and needs no edit of `PerViewCBuffer`. Scissor is left alone. The other worker's `UpscaleViewportJitter` already does this. (H)
- Never jitter: the 1024x1024 shadow maps, SSAO and bloom targets, the Evaluation depth target (width 1920, `0x5c3d38`), photo capture and thumbnail passes. Photo frames must be rendered unjittered and not upscaled. (H, matches `AGENTS.md` scoring rule)
- Sequence: 8 or 16 phase Halton. Send the same jitter to the vector pass so vectors are jitter-free, as DLSS expects.

Mip bias:

- Sampler choke point `0x9216d0` (cache `0x3206d0`); LOD bias is read at `0x921858` (desc+0x30). The host override already exists (`SNAP_LOD_BIAS=<f>|auto`, `runtime/nvn/nvn_entry.cpp`). (H)
- NVIDIA's guidance is `log2(renderWidth / displayWidth) - 1` for DLSS. The existing `auto` is `-log2(scale)`, where scale is relative to the game's 1080p targets, which is a different quantity. Reconcile before shipping. For DLAA the first value to try is the native-4K one (`-1`), to match the original texture sharpness at 1080p. (M)

Depth and exposure:

- ~~Depth is 0..1, near-is-zero, not inverted.~~ Corrected 2026-11-12 by a runtime probe of `PerViewCBuffer` (upscaler worker, `SNAP_UPSCALE_PROBE`): the projection is reversed infinite Z. P row 2 = (0, 0, 0, 0.01), row 3 = (0, 0, -1, 0), so depth 1 = near plane (0.01) and 0 = infinity. Pass `DepthInverted` (DLSS), `DEPTH_INVERTED | DEPTH_INFINITE` (FSR) and `INVERTED_DEPTH` (XeSS); `runtime/nvn/upscale/upscale.cpp` `TakeCamera` reads this from the matrix every frame. (H)
- Exposure: `currentExposure` / `exposureReciprocal` at `PerViewCBuffer+0x2c8` / `+0x2b4` can feed `pInExposureTexture` or `InPreExposure`. Scene color format was not decoded; if it is already tonemapped 0..1, run DLSS as LDR (`hdr = false` in `Eval`). (L)

Where DLSS goes and where UI stays:

- The UI is composited in the pass named `UI`, then `Fade`, into the window (DISPLAY) textures. The layer already never scales window textures and treats the present blit as the single scene-to-window mapping (design doc, "Window and present"). So UI naturally stays at output size, after upscaling. (H)
- Upscale the scene color before the game's post chain if the post targets can read the output-size image: SSAO, bloom (192x108) and DOF (960x540 chain) are small fixed targets and only need a bigger source. `Final` (tonemap and FXAA) must then draw at output size. If that is too invasive, upscale the tonemapped image instead and treat it as LDR. (M; the pass order edges were never decoded, `render_graph.md` section 8)
- Turn the game's FXAA off while DLSS or DLAA runs (`AaPatchFragment`, `SNAP_AA=off` in `runtime/nvn/nvn_aa.cpp`). Two anti-aliasing steps would blur the result. (H)
- Photo capture (`Capture3D`, `CaptureCompress`, `Evaluation`, `PhotoEditing`) must read pre-upscale, pre-UI images. `AGENTS.md` and the design doc already require this.

## 6. Design: cheapest correct route

Tier 0, camera-only reprojection from depth (small, mostly done):

1. At the first scene draw of a frame, copy `PerViewCBuffer` (c3, 752 bytes) from the bound range. Keep two frames.
2. Compute `prevViewProj * viewProjectionInverse` in double precision, run one compute pass over the depth target. Output RG16F pixel-space vectors (previous minus current), render size, jitter removed. This is `upscale_mv.comp`.
3. Wrong for: moving Pokemon and the player vehicle, foliage wind, water, particles. Right for the whole static world and for camera cuts handled by `reset`.

Tier 1, object and skinned motion (characters only):

1. Keep last frame's per-object world-view-projection. The per-draw cbuffer lives in a per-frame ring (the same ring pattern as the skin params), so its address cannot be the key. Key by vertex buffer address of attribute 0 plus first index; for characters the skinned destination range is the stable identity. Validate in a run that the destination ranges do not move between frames. (L)
2. Keep last frame's skinned positions. In the compute dispatch hook (`nvn_draw.cpp` near line 770), before the dispatch, copy the destination SSBO range into a shadow buffer of the same pool offset (about 20 characters per frame, a few MB, one `vkCmdCopyBuffer` and a barrier). Bind that shadow as an extra vertex stream when drawing the character. (M)
3. Patch only the `CharacterShader` family at translation time (96 VS, 714 PS bodies, reusing the `PatchFragmentOutputs` approach): the VS adds an output `prevClip = prevWVP * prevPosition`; the PS writes `prevWindow - (gl_FragCoord.xy - jitter)` to a second color attachment. Add that attachment to the scene pass only (the pipeline variant key already carries attachment count).
4. Make the second attachment RGBA16F, cleared to alpha 0. Character draws write alpha 1. The tier 0 compute pass keeps pixels with alpha 1 and fills the rest from depth, then writes the final RG16F image for NGX. This avoids a second geometry pass and keeps depth testing correct, because the vector comes from the same fragment that won the depth test.
5. Dither-faded and alpha-tested character pixels (`DitherRatio`, `AlphaTestRef` in `UpdatePerObject`) discard in the same shader, so they drop their vector too.
6. Skip alpha-blended character draws, particles (`vdkEfx*`), sea and the sky. Give them zero vectors and mark them in the reactive mask (`pInBiasCurrentColorMask`).

Not worth doing first:

- A separate velocity pass for all `StageShader` draws (1,010 programs, 134 unique VS). The static world is already right in tier 0.
- Foliage wind vectors. They need the previous time in the VS (`preComputedWindDirection`, timers in `PerViewCBuffer`). Revisit if ghosting on grass is visible.
- Transform-feedback geometry replay (the ScarletNX method). It costs more than tier 1 here because the skinning output is already a buffer.

Edge cases:

- Camera cuts and scene loads: call `reset` when the camera position jumps, a fade is above a threshold, or the scene targets are recreated.
- Newly spawned objects have no history: use their own current world matrix for the previous one (camera-only motion).
- The Evaluation passes and photo frames bypass all of this.

## 7. NVIDIA SDK status and integration points

- `tools/nvidia/` exists (created 2026-11-12). It holds `dlss_v310.9.16/` with the NGX headers (`nvsdk_ngx*.h`, including `nvsdk_ngx_helpers_vk.h` and the frame-generation headers), `lib/nvsdk_ngx_{d,s}.lib`, `bin/nvngx_dlss.dll`, `LICENSE.txt`, and `SOURCES.md` (tag v310.9.16, local use only, git-ignored). Contents of `SOURCES.md` agree with the folder. (H)
- Not present: the Streamline SDK (`sl.interposer.dll`, `sl.*.dll`) and a separate frame-generation runtime. So Reflex and DLSS-G through Streamline are not available yet. DLSS-G headers are in the NGX include folder only.
- `runtime/nvn/upscale/upscale_dlss.cpp` does not compile SDK code. It declares the NGX ABI itself and loads the driver's `_nvngx.dll`, with `nvngx_dlss.dll` copied beside the exe. That keeps the public build free of NVIDIA binaries.
- NGX call sites in our layer:
  - Instance and device creation: `NVSDK_NGX_VULKAN_RequiredExtensions` result must be added to `Host::Init` in `runtime/nvn/nvn_vk.cpp` (`snap_upscale::AddInstanceExtensions` / `AddDeviceExtensions`).
  - After device creation: `NVSDK_NGX_VULKAN_Init`, then `GetCapabilityParameters`, `CreateFeature(DLSS)` on a command buffer.
  - Per frame: `EvaluateFeature` recorded into the replay command buffer when the scene pass ends (`UpscaleOnSetRenderTargets` in `upscale_nvn.h`). Inputs: color, depth, vectors (RG16F), exposure, jitter, `InMVScaleX/Y = 1`, `InReset`, `InFrameTimeDeltaInMsec`, optional reactive mask. Output image needs storage usage.
  - Teardown: `ReleaseFeature`, `NVSDK_NGX_VULKAN_Shutdown1` in `Shutdown()`.
- Public builds: no DLSS binaries. Fall back to FSR, XeSS or plain render scale (already the order in `upscaler.h`).
- Streamline, if ever wanted for Reflex or frame generation: hook the swapchain and present in `PresentWindowOnce`. It requires the Streamline interposer to own the Vulkan loader, a larger change. Frame generation never counts toward real 60 or 120 FPS (`AGENTS.md`).

## 8. Order of work and effort

1. Verify tier 0 against the front end already written (matrix source, sign, jitter): 1 to 2 days of checking, not new code.
2. Dynamic check (needs a run, not done here): dump c3 and c4 of one character draw over three frames. Confirm the matrices, ring behavior, and that skinned ranges are stable. 1 day.
3. Tier 1 skinned shadow buffer and `CharacterShader` patch: about 1 to 2 weeks including photo-safety checks.
4. Reactive mask for particles and water, then wind: later.

## 9. Open questions

- Format and exact content (HDR or tonemapped) of the scene color target. Needs the `0x5bc660` helper or a run.
- The pass edge order (which pass reads which target). Needs the 37 `add_edge` endpoints decoded.
- Whether the gameplay camera's c3 matrices are rebuilt once per frame or per view (cut-scene cameras use other perspective sites, `0x10e458`).
- Which dispatch site drives which of the five skinning programs.
