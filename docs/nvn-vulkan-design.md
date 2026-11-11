# NVN to Vulkan layer: design (milestones 1 and 2)

Status: milestone 1 implemented (`runtime/nvn/`, test in `tests/nvn/`). Phase 0 of `analysis/runtime/nvn_usage.md` (present a cleared frame) works with Vulkan validation clean. Phases 1 and 2 are designed here and not built.

## Scope and ABI

- The lifted game gets a function table from `nvnBootstrapLoader` (`nvnHost*` extras are host-only). Entry points use the exact NVN names, keyed by name in `getProcAddress`. 83 are registered: all 78 of the phase-0 list plus `DeviceGetProcAddress`, `DeviceRegisterFastClearDepth`, `CommandBufferClearDepthStencil`, `CommandBufferSetDebugLabel` and `TextureGetDepth`.
- Guest NVN objects are opaque blobs the game allocates (Device 0x3000, Queue 0x2000, CommandBuffer 0xa0, MemoryPool 0x100, Sync 0x40, Window 0x180, Texture 0x80; sizes confirmed from allocation sites in `nn::gfx` at 0x90d590 and 0x985790). We write only `{magic, impl*}` into the first 16 bytes and keep real state in host objects. This makes us independent of the real struct layouts. Builders live on the guest stack and are written in place; they must fit 0x40 bytes (0x80 for the texture builder).
- Provenance of every enum value and signature is tagged in `runtime/nvn/nvn_types.h` and at each entry point: `[CALL addr]` from argument setup in the game, `[TOTK-NVN]` from `tools/ref/totk/runtime/nvn.h` (MIT community reconstruction), `[LAS-NVN]` from `tools/ref/las/runtime/nvn/formats.c` (MIT). No Nintendo header was used.
- Calling convention is the host C ABI. Bridging the lifted code's convention to these functions (argument registers, `float` in `s0`, 8-bit `bool` return) is the lifter/runtime's job, not this layer's.

## Object mapping

| NVN | Host object | Vulkan |
|---|---|---|
| Device | `Host` (single instance) | `VkInstance`, `VkDevice`, one graphics+compute queue family with Win32 presentation, `VkPipelineCache`, validation messenger |
| Queue | `QueueImpl` | one `VkCommandPool`, one timeline `VkSemaphore`; all NVN queues share the single `VkQueue` under `Host::queueMutex` |
| CommandBuffer | `CommandBufferImpl` | none while recording (see below) |
| MemoryPool | `PoolImpl` | lazy `VkBuffer` + `VkDeviceMemory` mirror |
| Texture | `TextureImpl` | `VkImage` created on first use, own dedicated allocation (phase 1: placed in pool memory) |
| Sync | `SyncImpl` | `{timeline semaphore, value}` |
| Window | `WindowImpl` | `VkSurfaceKHR`, `VkSwapchainKHR`, semaphore rings |

### Device

`DeviceInitialize` creates Vulkan 1.3 with synchronization2, dynamicRendering and timelineSemaphore (all required), plus multiViewport when available. Validation layers come from `nvnHostSetValidation(1)`, the builder's DEBUG_ENABLE flag, or env `NVN_VK_VALIDATION=1`; synchronization validation is on, best-practices off. `DeviceGetInteger` answers all 0..0x61 infos from a table in `nvn_entry.cpp` (the game reads 0..0x5f). Window origin mode and depth mode are stored on the host and applied at replay (negative-height viewport for upper-left origin, see Fixes) and later at pipeline creation (clip-space depth remap for `NEAR_IS_MINUS_W`; the game sets near-is-zero, which matches Vulkan, so no remap is needed for it). `RegisterFastClearColor/Depth` only record the hint.

### CommandBuffer: recording model

NVN command buffers write GPU method words into guest "command memory" and bookkeeping into "control memory", and call a memory callback when either runs out. We do not reproduce any of that:

- `AddCommandMemory`/`AddControlMemory`/`SetMemoryCallback` are accepted and counted, never written or called. Our command list grows in host memory, so there is no out-of-memory event.
- Each NVN call appends a small `Cmd` to a `std::vector` (no Vulkan calls, no locks). Recording is therefore free of Vulkan thread rules and can happen on any thread; the game records on worker threads.
- `EndRecording` moves the vector into a heap `CmdList` and returns its pointer as the `CommandHandle` (a 64-bit opaque value to the game). `QueueSubmitCommands` consumes it.
- At submit the list is replayed into a real `VkCommandBuffer` (allocated from the queue's pool, recycled after the timeline value completes). Replay owns layout tracking: every `TextureImpl` carries its current `VkImageLayout`, and replay inserts sync2 barriers as needed. Clears use `vkCmdClearColorImage` / `vkCmdClearDepthStencilImage` in `TRANSFER_DST`.
- Phase 1 replaces the fat `Cmd` struct with a compact byte stream and groups consecutive draws that share render targets into one `vkCmdBeginRendering` scope (a clear that directly follows `SetRenderTargets` folds into `loadOp = CLEAR`).
- Implemented: `SetRenderTargets`, `ClearColor` (float and integer formats; partial channel masks are reported and not yet supported), `ClearDepthStencil`, `SetViewports`, `SetScissors`, `Barrier` (every bit maps to a conservative full memory barrier for now).

### MemoryPool

The guest passes a host-addressable pointer (`MemoryPoolBuilderSetStorage(ptr, size)`); with guest pointer equal to host pointer, `MemoryPoolMap` simply returns it. Vulkan backing is mirrored and lazy:

- CPU-accessible pools: `HOST_VISIBLE|COHERENT` memory, preferring `DEVICE_LOCAL` (resizable BAR), persistently mapped. `MemoryPoolFlushMappedRange` memcpy's the range from the guest pointer into the mirror. Phase 1 adds dirty-range tracking (page granularity) and flush at submit for pools the game writes without flushing.
- GPU-only pools: `DEVICE_LOCAL`, no CPU mirror.
- Backing is created on the first flush or first `Buffer` bind, never at `Initialize`, because the game creates many pools that only hold texture storage.
- Alternative kept open: import the guest pointer with `VK_EXT_external_memory_host` (needs 4 KiB alignment, which the game's pools have). We prefer the mirror on discrete GPUs, per the AGENTS.md graphics decision (no PCIe reads of imported memory).
- Textures will alias pool memory at `poolOffset` in phase 1 (pool `VkDeviceMemory` allocated with the union of buffer and image memory-type bits). Today each texture owns a dedicated allocation, which is fine for a handful of render targets and wrong for the game's thousands of textures.

### Buffer, texture and sampler pools (phase 1 design)

- `BufferInitialize` = sub-range `{PoolImpl*, offset, size}`; `BufferGetAddress` returns a synthetic 64-bit GPU address `poolBase + offset` from a per-device address allocator, and a host table maps address ranges back to `{VkBuffer, offset}`. Uniform/storage/vertex binds take addresses, so the translation is a lookup in that table at replay time.
- `TexturePool`/`SamplerPool` are descriptor-table mirrors: `PoolRegisterTexture(id, tex, view)` stores `{VkImageView}` in slot `id`, `RegisterSampler(id, sampler)` stores a `VkSampler`. `DeviceGetTextureHandle(texId, samplerId)` returns `texId | samplerId << 20` (implemented).
- Per `analysis/shaders/translator.md`, the translated shaders address textures by reflection binding `b` (TEX immediate index `8+2b`), not by the NVN handle value. The NVN handle only travels through the game's bind calls, so replay resolves `handle -> (texture id, sampler id) -> {VkImageView, VkSampler}` from the pool mirrors and writes a combined image sampler at set 2, binding `b`. The NVN handle is never forwarded to the shader.
- Handle fields beyond 20 bits are unverified.

### Programs and pipeline cache

NVN has no pipeline object: state is set on the command buffer and a draw combines program + 8 state objects + vertex attrib/stream state + render targets. Vulkan needs a pipeline for the same combination.

- `Program` holds the compiled stages (SPIR-V per stage, produced offline, see below) as `VkShaderModule`s.
- A draw computes a `PipelineKey`: program id; blend/color/channel-mask/multisample/polygon/depth-stencil state object contents (the objects are POD blobs in guest memory, so hash the bytes); vertex attrib and stream states; render target formats and sample count; primitive topology. Everything that Vulkan makes dynamic (viewport, scissor, stencil ref/mask, depth bias values, blend constants, cull/front-face, depth test enables) stays out of the key to keep the pipeline count small.
- Cache: `unordered_map<PipelineKey, VkPipeline>` behind a reader-writer lock, backed by the device `VkPipelineCache`, serialized to disk next to a key manifest so a warm start can precompile every known (program, state) pair. A miss on the draw path compiles synchronously the first time; a background thread pool precompiles keys from the manifest at startup and from frame N-1's draw list when a newly loaded program appears. Pipelines use `VK_KHR_dynamic_rendering` formats (no render pass objects).

### Sync objects

`QueueFenceSync(queue, sync)` submits an empty batch signaling the queue's next timeline value and stores `{semaphore, value}` in the `Sync`. `SyncWait(sync, ns)` maps to `vkGetSemaphoreCounterValue` (ALREADY_SIGNALED) then `vkWaitSemaphores` (CONDITION_SATISFIED / TIMEOUT_EXPIRED / FAILED). A `Sync` never fenced reports ALREADY_SIGNALED. `QueueFinish` waits the queue's last value; `QueueFlush` is a no-op because submissions are issued immediately.

### Window and present

Window textures are ordinary offscreen images at scene resolution (`TextureInitialize` stores them lazily; `WindowInitialize` creates the images). The swapchain stays internal to the layer:

- `WindowAcquireTexture` hands out slots round-robin and arms the supplied `Sync` with the timeline value of the last present that read that slot, so `SyncWait(sync, -1)` is exactly "GPU finished with this slot".
- `QueuePresentTexture(slot)`: `vkAcquireNextImageKHR` (blocks on vsync with FIFO) using a semaphore ring gated on the timeline, then one submit that transitions, clears black, blits the slot (honoring `WindowSetCrop`, aspect-fit letterbox, linear filter when scaled) into the swapchain image, signals a per-image render-done semaphore and the timeline, then `vkQueuePresentKHR`.
- Present interval 0 selects IMMEDIATE (then MAILBOX, then FIFO); any other interval uses FIFO. A frame limiter (`nvnHostSetFramePacing`, default 60 Hz for interval 1, interval n paces to n/60 s) enforces the original 60 FPS cadence on displays faster than 60 Hz using an absolute schedule, a high-resolution waitable timer and a short spin. On a 144 Hz display a 60 FPS cadence cannot land on whole vblanks, so frames judder; the intended settings are display-paced FIFO (`hz = 0`) at the display rate once the game runs above 60, or a 60/120/240 Hz display.
- Because scene size and output size are decoupled, native 4K, DLAA and DLSS only change the offscreen texture size and the present step; the NVN-visible texture size stays whatever the game asked for until a pass replacement says otherwise.
- Resize/out-of-date recreates the swapchain; the offscreen textures are untouched.

## Threading

- Recording touches only host memory, so any number of threads can record into distinct `CommandBufferImpl`s concurrently.
- All Vulkan calls that need external synchronization on the `VkQueue` (submit, fence-sync, present) take `Host::queueMutex`. `Reap`/`AcquireCb`/the queue's pool are touched only under that lock. Present holds the lock while blocked in `vkAcquireNextImageKHR` (a known simplification; phase 2 moves present to its own thread with a small frame queue so submits from other threads are never blocked by vsync).
- `SyncWait` and `QueueFinish` use `vkWaitSemaphores` outside the queue lock (except the reap step).
- The NVN contract already requires external synchronization per object, so no per-object locks are added.
- Pipeline compilation and (later) shader translation run on a worker pool; the draw path only blocks on a miss.

## Interception hooks (4K / DLAA / DLSS)

The layer exposes two places for the future render-pass replacement system (switchable original path, screenshot comparison):

1. Replay boundary. Replay already walks a decoded command list, so the planned `PassHook` interface sits between `SetRenderTargets` and the following draws: `on_pass_begin(targets, formats, size)` can substitute larger render targets (native 4K) or attach jitter and motion-vector outputs; `on_pass_end` can run a replacement pass (DLSS/DLAA temporal resolve with color, depth, exposure, jitter, mip bias from the hook). Hooks identify a pass by render-target set and the NU pass id, which the runtime learns from the pass graph (`0x982210` plug objects) and registers on the layer before submit. Today only `on_submit` is wired; it reports command counts per list.
2. Present. `NvnHostHooks::on_present(slot, w, h)` runs before the output blit. The present step is the composition point for UI at native output resolution, upscaler output and the photo reference render. It is already a separate blit, so replacing it does not disturb the game's view of its textures.

Every replacement keeps the original path behind a switch; scoring-related targets (the evaluation depth target) are never touched by hooks.

## Shader path and pipeline creation

Per `analysis/shaders/translator.md`, nushdb SASS is translated offline by the Ryujinx `Ryujinx.Graphics.Shader` SPIR-V output. The binding layout from that document: uniform buffers in set 0 (binding = reflection UBO index), storage buffers in set 1, combined image samplers in set 2 (binding = reflection sampler index), driver constant block `c[0]` as a small UBO or push constants; the fragment origin is set to upper-left. The layer feeds this as follows:

- `ProgramSetShaders(stages)` receives the nushdb blob; the runtime looks up its precompiled SPIR-V plus a binding manifest by content hash (translation never runs in the game process).
- One `VkDescriptorSetLayout` per set, derived from the manifest, plus a `VkPipelineLayout` with push constants for the driver-constant block the translator expects.
- NVN binding calls (`BindUniformBuffers(stage, index, ...)`, `BindStorageBuffers`, texture handle binds, `BindImages`) become per-draw descriptor writes (uniform set 0, storage set 1, textures set 2, using push descriptors or a per-frame descriptor pool). The texture slot is resolved: `b = (handleIdx - 8) / 2` (see Binding model).
- The pipeline key includes the SPIR-V module hash, so a changed translation invalidates cached pipelines.

## Milestone 2: what changed

Milestone 2 added the graphics path: 196 of the game's 205 called procs are implemented (204 registered in total; the 9 missing are listed under phase 3). New files in `runtime/nvn/`: `nvn_shader.cpp` (programs, harness, shader cache), `nvn_state.h` (state encodings), `nvn_gfx.cpp` (entry points), `nvn_draw.cpp` (replay of draws, pipelines, descriptors). `nvn_vk.cpp`, `nvn_entry.cpp` and `nvn_internal.h` were extended.

- **Programs.** `ProgramInitialize` and `ProgramSetShaders` parse the nushdb inner binary: control section (+0x6F8 program size, +0x700 constant offset, +0x704 data size, +0x708 header size 0x30), SPH at data+0x30, stage from SPH word 0 bits 10-13 (1 VS, 5 FS). The runtime runs the offline Ryujinx harness (`scripts/shader_harness`, `--nvn data.bin out.spv out.json stage constOff codeOff`, BelowNormal, 10 minute cap) and caches the SPIR-V and a JSON binding manifest by SHA-256 of the data section plus a salt. Harness path: baked-in `NVN_HARNESS_DLL` or env `NVN_SHADER_HARNESS`. Translation only runs on a cache miss; shaders and SPIR-V never go into source, only under `build/` or `generated/`.
- **Clip/cull fix.** The translator declares ClipDistance[8] plus CullDistance[1], which exceeds the Vulkan limit of 8 combined. The harness rewrites the gl_PerVertex array to 7 entries (`FixClipCull`) unless clip distance 7 is written.
- **Cache directory.** `NVN_CACHE_DIR`, else `<exe dir>/nvn_cache`. Holds `<sha256>.spv/.json` and `pipeline_cache.bin` (persisted `VkPipelineCache`).
- **Memory and addresses.** Each memory pool gets a synthetic GPU VA starting at 0x100000000. `BufferGetAddress` is poolVA plus offset; `ResolveAddress` maps an address back. At replay, the bound ranges are copied from guest memory into the mapped Vulkan mirror. Hazard: if the guest rewrites a bound range while a submit is in flight, the game must use fenced rings (the game's own ring discipline should cover this; unverified).
- **Draws.** Dynamic rendering (LOAD/STORE) begins lazily on the first draw, textures are transitioned before it, pipelines are keyed by the full state, and viewport, scissor, stencil masks and reference, depth bias and blend constants are dynamic. Descriptor sets come from per-command-buffer pools that are recycled after the fence is reaped. Y: NVN keeps NDC +y up in both origin modes, so upper-left origin uses a negative-height Vulkan viewport (`MakeNvnViewport`; see Fixes). NVN CCW maps directly to Vulkan CCW under that flip.
- **State.** `nvn_state.h` holds the state objects: DepthStencil is a u64 bitfield; VertexAttrib is fmt:9 | stream:4<<9 | offset:19<<13. Compare and stencil ops are the NVN value minus 1, cull values equal the Vulkan bits, blend functions use an explicit switch.
- **Device features.** All core GPU features are enabled, plus shaderDrawParameters and samplerMirrorClampToEdge. Images are created MUTABLE_FORMAT so texture views can reinterpret formats.

## Binding model

Derived from the VS/PS pair `vdkEfxSystemConstantVS/PS` and the game's bind loop at 0x915e80; replaces the earlier sketch under "Shader path".

| Vulkan set | Binding | NVN side |
|---|---|---|
| 0 | 0 | Ryujinx support buffer (1408 B, render_scale entries 1.0f); not an NVN binding |
| 0 | 1 + stage*16 + s | constant buffer c[s] of that stage |
| 1 | stage*16 + n | storage buffer n (NVN index assumed equal to the ordinal) |
| 2 | stage*32 + n | combined image sampler n |
| 3 | stage*8 + n | storage image n |

- **Constant buffers.** SASS reads reflection UBO binding b as c[b+3], so `BindUniformBuffers(stage, index)` index = slot - 3. c0 (driver constants) and c2 are bound to a zero buffer; c1 is the program's own constant-data blob (from the control section).
- **Textures.** The texture handle is `texId | samplerId << 20`; ids index the host tables of the currently bound texture and sampler pools. `BindTexture` index b = (handleIndex - 8) / 2, matching the loop at 0x915e80 (slots 0..0x1f, GetTextureHandle then BindTexture). Verified in the test with handleIndex 8 for sampler binding 0. `DeviceGetImageHandle(id)` returns id.
- **Missing resources** read dummies created in the first command buffer (zero UBO, support buffer, 1x1 sampled and storage images, sampler).

## Test and result

`tests/nvn/mesh_test.cpp` (`tests/nvn/build.bat mesh_test`) loads the real `nuSystemConstantVS/PS.nushdb` pair from `extracted/` at run time (VS: attrib 0 position, attrib 2 uv, UBO binding 1 for the WVP matrix, UBO binding 6 for the UV transform; PS: UBO binding 6 for the diffuse color, sampler 0). It draws two procedural cubes (the second with baseVertex 24) with a procedural 256x256 9-mip texture uploaded per level with `CopyBufferToTexture`, depth testing and backface culling, for 300 frames. Result: `validation: errors=0 warnings=0`, coverage 18.0%, screenshot `analysis/nvn/mesh_test.png` shows upright textured cubes that intersect correctly through the depth buffer. The projection used is Vulkan style with P(1,1) = -f; this is a test choice, not a finding about the game's own matrices.

## Milestone 3: textures, missing procs, state gaps, hooks

All 205 called procs are registered now (the table reports 212 entry points in total, including a few extras). Files added under `runtime/nvn/`: `nvn_tex.h/.cpp` (Maxwell layout, no Vulkan), `nvn_tex_vk.cpp` (upload), `nvn_astc.cpp` (ASTC LDR decoder), `nvn_bc7.cpp` (BC7 mode-6 encoder), `nvn_alpha.cpp` (alpha-test SPIR-V patch), `nvn_xfer.cpp` (texture copies and counters), `nvn_clear_spv.h` (generated).

### The NUTEXB footer and texture formats (from 28,369 game files)

The last 0xb0 bytes of a `.nutexb` are: `u32 mip_sizes[16]`, `char name[0x40]`, then at +0x80 `u32 width, height, depth, format, dim (2 = 2D, 3 = 3D, 4 = cube), mips, tile (0x1000), layers, data_size`, the magic `" XET"` and a version. The `format` field is the nn::gfx image format. The game's own conversion (function at 0x924d60) maps it to NVN formats. The set in the game's data:

| nutexb format | NVN format | Content |
|---|---|---|
| 0x100 | R8 (0x01) | single channel |
| 0x180 / 0x280 | BC4 (0x49) / BC5 (0x4b) | |
| 0x400 / 0x405 | RGBA8 (0x25) / RGBA8 sRGB (0x38) | |
| 0x434 / 0x435 | RGBA32F (0x2e) / RGBA16F (0x29) | 3D colour LUTs (33^3, 65^3) |
| 0x450 / 0x455 | BGRA8 (0x9a) / BGRA8 sRGB (0x9c) | |
| 0x480 / 0x485 | BC1 (0x42) / BC1 sRGB (0x46) | |
| 0x490 / 0x495 | BC2 (0x43) / BC2 sRGB (0x47) | |
| 0x4a0 / 0x4a5 | BC3 (0x44) / BC3 sRGB (0x48) | |
| 0x4d7 / 0x4d8 | BC6H unsigned (0x50) / signed (0x4f) | |
| 0x4e0 / 0x4e5 | BC7 (0x4d) / BC7 sRGB (0x4e) | |

No file uses ASTC. The NVN codes for ASTC (0x79 to 0x94, 14 block sizes, unorm then sRGB) are an assumption; they come from no call site.

### Block-linear layout (`nvn_tex.cpp`)

A GOB is 64 bytes by 8 rows. Blocks stack GOBs in y (block height 1 to 16 GOBs) and z (block depth). The rules, checked on real data:

- Block height log2 comes from the tegra_swizzle rule: `h = heightInBlocks + heightInBlocks / 2`, then 128/64/32/16 map to 4/3/2/1, else 0. It reproduced the footer sizes of 239 distinct geometries. On six textures where it differs from "next power of two", only this rule gave smooth decoded images.
- Per level the block height shrinks while the level is shorter than half a block.
- Arrays and cubes: the layer size is aligned to `512 << blockHeightLog2`. Verified on the 108 cubemaps.
- 3D: block depth log2 = ceil(log2(depth)) capped at 4, block height log2 = min(rule, 4 - depthLog2). The 33^3 and 65^3 RGBA16F LUTs decode smoothly only with this. The 1024x1024x8 BC1 volume contains noise, so it checks the byte layout (the GPU readback equals the independent deswizzle) but not smoothness.
- `TextureBuilderGetStorageSize` now returns the exact layout size rounded to 4 KB. The old over-estimate is gone.

Static textures are uploaded lazily, at the first replay command that reads them (sampling, copy source, image binding). The data is the packaged pointer, or the pool storage at the texture's offset for textures that are not render targets. A texture counts as defined once it has been uploaded, copied to, cleared or rendered to. Upload deswizzles each level and layer into a host-visible staging buffer owned by the command buffer and issues one `vkCmdCopyBufferToImage` per subresource. Cube textures are 2D images with 6 layers and the cube-compatible flag. Cube arrays use depth as the cube count when depth is a multiple of 6 and as cubes otherwise (unused by the game, unverified).

### ASTC strategy

This GPU has BC1 to BC7 and no ASTC LDR. The game needs no ASTC, but the layer handles it:

1. If the device supports ASTC, the texture is created in the native format and the data is uploaded as is.
2. Otherwise the blocks are decoded on the CPU (`nvn_astc.cpp`, adapted from the MIT-licensed LAS decoder, attribution in the file header) and transcoded to BC7 with `nvn_bc7.cpp`. The encoder writes mode 6 only: principal-axis endpoints, 7-bit values with p-bits, 4-bit indices, two least-squares refinement passes. It was written for this project, with no third-party encoder code.
3. `NVN_ASTC_FALLBACK=rgba8` (or a device without BC7) skips the transcode and samples the decoded RGBA8.

The result of each transcode is cached under `<cacheDir>\astc\`, keyed by a 128-bit hash of the guest data plus format and size. 3D ASTC and ASTC HDR are not decoded. `CopyBufferToTexture` into an ASTC texture is not decoded either.

### The nine procs

- `DeviceGetTimestampInNanoseconds` returns the timestamp field of the counter data (already written in nanoseconds).
- `SetPatchSize` goes into the pipeline key and `patchControlPoints`.
- `DrawArraysIndirect` and `DrawElementsIndirect` read the argument block from guest memory when the command replays and turn it into a normal draw. A buffer written by the GPU is not supported.
- `DispatchCompute` builds a compute pipeline per program (key `CS<id>`), shares the descriptor writer with draws (`PrepareTextures` and `WriteDescriptors` in `nvn_draw.cpp`) and ends with a global memory barrier.
- `SetAlphaRef`: see the alpha test below.
- `CopyTextureToBuffer` and `CopyTextureToTexture`: plain copies when size and format match, otherwise `vkCmdBlitImage` (render-scale mismatch, mirror flags, format change; this is also a GPU decode path for block-compressed sources). The buffer copy lands in a scratch buffer and is written into guest memory by `FlushReadbacks` after the submit's timeline value completes. `FlushReadbacks` runs from `SyncWait`, `QueueFinish`, `BufferInvalidateMappedRange` and shutdown. Guest code that reads the destination without any of these calls sees stale data.
- `ReportCounter`: TIMESTAMP and TIMESTAMP_TOP use `vkCmdWriteTimestamp2`, converted with `timestampPeriod`. SAMPLES_PASSED wraps each rendering scope in an occlusion query from the first recorded report on and sums the queries since the previous report into a running 64-bit counter. The counter starts at the first report, not at device creation. Other counter types write 0.

### State gaps

- QUADS, QUAD_STRIP, POLYGON, LINE_LOOP and u8 indices: the draw reads the index data from guest memory, rewrites it into a u32 scratch buffer (QUADS to two triangles, QUAD_STRIP to quads `(2k, 2k+1, 2k+3, 2k+2)` as triangles, LINE_LOOP to a line strip closed with the first vertex, POLYGON to a fan, u8 promoted to u32) and draws indexed. No primitive restart.
- Alpha test: `nvn_alpha.cpp` wraps the fragment SPIR-V. A new entry point calls the original, reads output location 0 `.w`, compares it with specialization constant 0 and discards on failure. The compare function selects one of seven patched modules, cached per program. The reference value is in the pipeline key. A module with no float vec4 at location 0 is left alone and a message is logged.
- Partial clear-channel masks: `ClearColor` with a mask other than 0xF draws a fullscreen triangle with a color write mask. The SPIR-V (float, uint and int variants) is embedded in `nvn_clear_spv.h` and was built with glslang from `build/nvn/clr`. A mask of 0 clears nothing. The clear still ignores the scissor, as before.

### Transform feedback

`VK_EXT_transform_feedback` is present on the development GPU but is not enabled in `Host::Init`. The call census contains no transform-feedback procs. The two geometry programs `efxDrawFaceGS` and `efxDrawFaceLightGS` carry stream-out masks in their shader headers, and nothing in the lifted code configures a buffer for them. So it is not implemented. If a call site appears: enable the extension and the `transformFeedback` feature, add `XfbBuffer` and `XfbOffset` decorations in the translator, then bind the buffers and begin and end transform feedback around the draws.

### Render-target scaling hook (off by default)

One policy object (`ScalePolicy`, `g_scale` in `nvn_entry.cpp`) is consulted in `TextureInitialize`. With `nvnHostSetRenderScale(1.0)`, the default, every texture matches the original. With another scale, a texture is scaled when all of these hold: the COMPRESSIBLE flag is set (the game's render-target flag), none of DISPLAY, LINEAR or LINEAR_RENDER_TARGET is set, the target is 2D or 2D array, and it is single-sample. The image is created at `ceil(size * scale)`. Every NVN getter still reports the guest size; `nvnHostGetTextureHostSize` reports the real one.

Exemptions: by signature (`nvnHostAddScaleExemption(width, height, nvnFormat, depthOnly)`, -1 matches anything), and by tag (`nvnHostSetNextTextureTag(tag)` before the allocation plus `nvnHostAddScaleExemptionTag(tag)`). A built-in signature exemption covers depth-format targets of width 1920, which is the Evaluation depth target (width 1920 at 0x5c3d38). A signature cannot be exact for it: a docked-mode main depth target could also be 1920 wide. So the tag is the reliable route, and the recomp wrapper at that call site should use it. The photo scoring reads that target back, so scaling it would break the score.

Interaction with other state:

- Viewport and scissor are in guest pixels. Replay multiplies them by the scale of the first bound render target (floor for the origin, ceil for the size). The default render area and the default viewport use the host size.
- Window textures (DISPLAY flag) are never scaled. The present blit is the single place that maps scene size to window size, so 4K output and DLAA hook in there.
- Copies between textures of different scale become blits (`CopyTextureToTexture`). `CopyBufferToTexture` into a scaled texture is not scaled and would be wrong; the game does not do that to render targets.
- Shaders that use fixed texel-size constants (SASS compiled with `1/width` as a constant) keep the same UV spacing, so their taps stay one guest texel apart and the filter looks tighter at a higher scale. That is a visual difference, not an error. Shaders that use `texelFetch` or fragment coordinates in guest pixels read wrong positions at any scale other than 1. The support buffer already carries the `render_scale` entries (1.0 now); filling them with the scale and patching those shaders is future work. Effects that depend on pixel counts (bloom kernels, blur radius) change look with the scale.

### Anisotropic filtering override

`nvnHostSetAnisotropyOverride(maxAniso)` (0 = off, the default). At `SamplerInitialize`, a sampler with a mipmap filter and no compare mode gets `max(game value, override)`, clamped to the device limit. The game uses 1x. It must be set before the game creates its samplers.

## Test and result (milestone 3)

- `tests/nvn/texture_test.cpp` plus `tests/nvn/texture_compare.py`: the compare script picks one real NUTEXB per format from `extracted\` at run time (no game data is stored in the repository), runs the test, and compares against `tests/nvn/nutexb_ref.py` (an independent numpy deswizzle) and the `texture2ddecoder` package. For every mip level and layer, the raw block bytes read back from the GPU must equal the numpy deswizzle byte for byte. Level 0 is also decoded on the GPU (a `CopyTextureToTexture` blit into RGBA8 or RGBA16F) and compared per pixel. Results go to `analysis/nvn/texture_results.tsv`, decoded images to `analysis/nvn/*.png`. The decode check does not draw a quad: the game's shaders cannot output a texel unmodified, so the GPU decode goes through the blit path.
- `tests/nvn/astc_test.cpp` plus `tests/nvn/astc_compare.py`: random ASTC blocks decoded by `nvn_astc.cpp` against `texture2ddecoder`, and the BC7 transcode measured by PSNR.
- Both run with validation layers on; the texture test fails on any validation error.

Results (this GPU, validation layers on):

- Texture test: 10 of 10 real NUTEXB files pass, with 0 validation errors. Every level and layer matches the independent deswizzle byte for byte (BC1, BC4, BC5, BC6H, BC7, RGBA16F, RGBA32F, cube, 3D, and an 11-mip chain). Level 0 decoded on the GPU matches a software decoder within 2 levels (BC4 and BC5: 8; BC6H is compared loosely because the two decoders disagree on range). The masked-clear check reads 255,127,255,255 against an expected 255,128,255,255 (rounding).
- Not covered: the set had no 2D RGBA8 sRGB file, and BC2 has no sample.
- ASTC: on blocks the reference decoder accepts as LDR-valid, `nvn_astc.cpp` is within 1 level on 100% of pixels. The BC7 transcode scores 33 to 39 dB PSNR on random noise, the worst case.
- Regression: `mesh_test` passes with 0 validation errors.
- All 205 called procs are registered.

## Fixes: first-screen rendering bugs (2026-11-11)

Both found on the loading screen (`analysis/boot/first_screen.png`); fixed capture `analysis/boot/fixed_screen.png`.

**Upside-down image: the window origin was applied as a no-op.**
- *Trace:* the game calls `nvnDeviceSetWindowOriginMode(1)` (UPPER_LEFT) and `SetDepthMode(1)` once at device setup (`0x90d6f4`, `0x90d720`). The layer applied a Y flip only for LOWER_LEFT and none for UPPER_LEFT, and the translated shaders carry no flip (the harness runs with `originUpperLeft` and no `YNegate`).
- *Root cause:* NVN follows OpenGL clip control, so NDC +y is up in both modes and the origin mode only chooses which window row is row 0. Vulkan's NDC +y points down, so a direct mapping renders every pass mirrored. Draws go to the three window textures (and the offscreen scene target), so the whole frame came out flipped; the A glyph and the emblem ring text were the visible tells.
- *Fix:* `MakeNvnViewport` (`nvn_internal.h`) gives UPPER_LEFT a negative-height viewport `{x, y + h, w, -h}`, so NDC +1 lands on the first row of the rectangle. LOWER_LEFT passes the rectangle through (row 0 is the image bottom, never presented flipped). Scissors are plain window rectangles in both modes, so the old LOWER_LEFT scissor/viewport flip against the first render target was removed. The default viewport (no `SetViewports`) uses the same helper, and the render-target scale is applied inside it.
- *Why not the present blit:* every pass, including render-to-texture passes sampled later, follows one convention (rows from the window origin, NDC +y up), exactly as on Maxwell, so intermediate targets stay consistent. Flipping only the final blit would leave the intermediate targets and `gl_FragCoord` users mirrored against each other.
- *Consequences:* with the flip, NVN CCW maps 1:1 to Vulkan CCW (a mirrored-NDC viewport is the standard GL-to-Vulkan mapping). `gl_FragCoord.y` is the row from the top in UPPER_LEFT, which is what the game's shaders expect, so no `YNegate` is needed. LOWER_LEFT with `gl_FragCoord` users is untested (the game never selects it).

**Garbled text: the glyph atlas is a pitch-linear, CPU-rewritten texture, and was read as block-linear once.**
- *Trace:* with `NVN_DRAW_LOG`, the text draws (program 6244, 94 quads per frame) sample one texture: R8, 512 x 2048, flags `0x10` (`TF_LINEAR`), in a pool. `TextureBuilderSetStride` is only called for such textures (`0x8f8700`).
- *Root cause (two defects):* (1) `UploadStaticTexture` deswizzled every pool texture as Maxwell block-linear and ignored the stride, so the rows were shuffled into blocky noise; (2) it uploaded once, but the game rasterizes glyphs into that memory with the CPU as text appears (at the first draw only rows 1 to 25 of 2048 are filled), so later glyphs never reached the GPU.
- *Fix:* `TF_LINEAR` textures take a row-by-row path with `max(stride, rowBytes)` (`nvn_tex_vk.cpp`), `TextureBuilderGetStorageSize` returns `stride * rows` for them, and `RefreshDynamicTexture` hashes the guest bytes of such textures at most once per replayed command list and re-uploads on change (64-bit word hash, about 0.1 ms per MB). It runs from `PrepareTextures` and the copy-from-texture paths. Other CPU-rewritten textures are not tracked; none were seen.
- *Debug aids (all env-gated, no cost when unset):* `NVN_TEX_DUMP=<dir>` writes the first upload of every static texture (guest data after deswizzle), every `CopyBufferToTexture` source and every linear upload as `<seq>_<kind>_t<ptr>_f<nvn format>_<WxH>_<wxh>.raw` (files over 8 MB skipped); `NVN_DRAW_LOG=<file>` with optional `NVN_DRAW_LOG_FRAMES=a-b` (present counts) logs viewports, render-target switches and every draw with its program, target and sampled textures.
- *Regression test:* `tests/nvn/linear_test.cpp` (`tests
vnuild.bat linear_test`) checks the viewport mapping numerically (both origins, scale, default viewport) and creates a linear R8 texture with stride greater than the width, reads it back through `CopyTextureToBuffer`, rewrites the pool memory and reads it again.

## Remaining work

- The cube-array layer count rule, the ASTC format codes and BC2 are not covered by any game file or test.
- GPU-written indirect argument buffers; transform feedback; primitive restart.
- `viewport_inverse` in the support buffer is unfilled; depth mode NEAR_IS_MINUS_W is not remapped.
- Texture feedback loops are rejected; very large pools make `EnsureBacking` expensive.
- Instance divisors above 1, separate textures and samplers, multi-viewport.
- `CopyBufferToTexture` into block-compressed or ASTC textures copies raw data without ASTC-aware sizing.

## Phase plan

| Phase | Contents | Status |
|---|---|---|
| 0 | Device, Queue, MemoryPool, Texture (2D), CommandBuffer record+replay of clears/viewport/scissor/barrier, Window, present, Sync | done |
| 1 | Buffer, Program, state objects, vertex input, uniform/storage/vertex binds, draws, texture/sampler pools, CopyBufferToTexture, depth clear on real targets | done (milestone 2) |
| 2 | Indirect draws, compute, texture copies, counters, alpha ref, timestamps, packaged textures, ASTC fallback, scaling and AF hooks | done (milestone 3) |

## Known unknowns

- DeviceGetInteger answers are placeholders except where the value affects allocation alignment (`DI_COMMAND_BUFFER_*`, `DI_MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT`). The GLSLC GPU-code version window (infos 0x2e..0x31) is returned as 0 until the nushdb control section values are used.
- Pool-resident textures without a packaged pointer are block-linear unless created with `TF_LINEAR` (then pitch-linear with the builder stride, see Fixes); other CPU-rewritten textures are not tracked.
- Whether the game polls GPU-written memory (counters, copied depth) without a fence wait, `QueueFinish` or `BufferInvalidateMappedRange`. Those are the only points where guest memory is refreshed.
- Hardware BC4 decoding differs from two software decoders by up to 7 levels on rare pixels, and BC1 sRGB corner blocks by up to 5. The layer does not decode these itself, so this is only a test tolerance.
- Unverified binding guesses: storage buffer NVN index equals the ordinal; storage image handleIndex-to-index mapping; what the game binds to c2; whether NVN CCW front-face is right against real game geometry (it follows from the viewport flip below, but no culled 3D draw has been checked yet).
