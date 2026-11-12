# Reference review: Astral Chain PC recomp

Source: https://github.com/captainj-sparrow/astral-chain-pc-recomp (shallow clone read at `tools/ref/astral-chain/`, git-ignored; nothing built or run). Reviewed 2026-11-12. The repo has one commit in the clone, so "version" is whatever HEAD was that day; pin a commit hash in `THIRD_PARTY_NOTICES.md` before copying anything.

## 1. License verdict

**MIT** (`LICENSE`, "Copyright (c) 2026 Valiantsin Dzerakh"; README says the same). Code can be adapted with notices, which fits `AGENTS.md` "Licensing and reference boundaries".

Conditions and traps:

- It is itself a derivative. `THIRD_PARTY_NOTICES.md` (one long table) lists what it took from TotK Recomp, LAS Recomp, MacNativeGames, nxnative, d2r-mobile, SavageWindRecomp, Ryujinx, deko3d (zlib), Dear ImGui and RenoDX, all MIT/zlib/BSD/Apache. When copying a file, keep the header it carries and add rows for both Astral Chain and the original author (the file headers name the original commit).
- One GPL item: `tools/ultrawide_layout.py` uses UI pane lists from the "TOTK-Ultrawide" community mod (GPL, version not recorded), data only. Do not copy that file.
- Not distributable by us: `libxess.dll` (Intel Simplified Software License), `nvngx_dlss.dll` (NVIDIA). Astral Chain loads both at run time from the user's folder and compiles in no vendor header (types declared by hand from the documented ABI). FSR 3 passes are AMD MIT shaders fetched by `tools/ffx_sdk.py` at build time (pinned commit and SHA-256 per file), not stored in the repo.
- The repo holds about 150 MB of game-UI-looking binaries (`controller-ui/*/romfs/ui/*.dtt`, `ui_core.dat`), which look like modified Astral Chain UI assets (not verified). Never copy `controller-ui/`; Snap's rule against distributing extracted assets applies. (Its `check_content.py` blocks `.dtt` yet they are present, so check whether the guard is bypassed.)

## 2. What it is

Static recompilation of Astral Chain 1.0.0/1.0.1 to a Windows x86-64 exe, built on TotK Recomp's pipeline: ARM64 lifted to C (LAS lifter, rewritten for scale), host runtime in C with `nn::` replaced by symbol, NVN front end from MacNativeGames recorded then replayed into Vulkan 1.3, Ryujinx translator as an AOT-built `ac_xlate.exe`, SDL3, Dear ImGui menu, installer that lifts and builds on the player's PC. About 730 files, runtime about 100 C files. Status per README: playable start to credits at 60 fps, DLSS/XeSS/FSR3, HDR10, ultrawide, mods and cheats.

Same foundation as Snap (TotK/LAS/MacNativeGames, Ryujinx translator, nn:: by symbol, 3 virtual cores, guest = host pointers), so most of it transfers. Snap is ahead on CPU backend evaluation, `sdk` lifting, render-graph knowledge (NU pass graph), photo-scoring safety and DRP mod loader.

## 3. Ranked borrowable items

Effort: S under a day, M a few days, L a week or more. "Code" = MIT, adapt with notices; "Idea" = copy the design only.

### 1. Maxwell/NVN behaviour checklist and the fixes behind it (S for the list, S-M per fix)
- What: 21 numbered Maxwell-vs-Vulkan differences with the fix each needed. Snap greps show no handling for: provoking vertex LAST (`VK_EXT_provoking_vertex`, `vk_pipeline.c` `create_graphics`), 32-lane subgroup pinning (`vk_caps.c`, `vk_pipeline.c` `pin_subgroup32`), custom border colors, FMZ semantics, unwritten VS outputs (`spirv_add_outputs`), D16 shadow compare rounding. Snap does have occlusion queries, color-mask clears and dual-source blend (check each against the notes).
- Where: `docs/maxwell.md` sections 6 and 7.
- Snap mapping: `runtime/nvn/nvn_pipeline.cpp`, `nvn_draw.cpp`, `nvn_vk.cpp`, `nvn_tex_vk.cpp`; add a "checks" section to `docs/nvn-vulkan-design.md`.
- License: docs are MIT (read freely); code adaptable.

### 2. Translator patches for the Ryujinx shader translator (M)
- What: 11 source patches to Ryujinx 1.3.2 that fix real miscompiles: call arguments kept in variables (0001-0003), no else-if flattening (0004), storage image write-without-format (0005), initialise unlinked vertex outputs (0006), position always invariant (0007), point size from support buffer (0008), D16 compare-reference rounding for shadow arrays (0009). 0010-0011 (PCSS, shadow filter) are TotK-specific looks; skip. 0012 is item 4.
- Where: `tools/xlate/patches/0001..0009-*.patch`, `tools/xlate/Program.cs`, `tools/build_xlate.py`.
- Snap mapping: `scripts/shader_harness/` (Snap uses the Ryujinx/Ryubing translator unpatched, per `THIRD_PARTY_NOTICES.md` and `analysis/shaders/translator.md`). Apply as a patch series with a CI check that the translator census (`analysis/shaders/census.*`) still passes `spirv-val`. Hunks may need rebasing onto the Ryubing tree Snap vendors.
- License: code (Ryujinx MIT via MacNativeGames, patches by Astral Chain/TotK authors MIT).

### 3. Temporal upscaler stack: DLSS (NGX direct), XeSS, FSR 3, jitter, history handling (L)
- What: one upscaler front end with three backends behind one setting (`off|auto|dlss|xess|fsr3`, quality `native`..`ultra_performance`). DLSS via the driver's NGX core (`_nvngx.dll`) loaded at run time with the C++ parameter vtable called by slot and checked with a set/get round trip, DLSS model "K" (transformer), exposure from frame time. History reset on camera cuts (30 degrees, 5 m, 2 stops of zoom), after 6 missed presents, on size change. Jitter is a Halton(2,3) viewport offset, not a projection edit, so camera blocks stay unjittered; phase count 8 x (output/scene)^2. LOD bias log2(scene/output) in the sampler key. HUD stays at output resolution, drawn after the resolve. Releases features after 600 presents without use.
- Where: `runtime/vk_dlaa.c` (1305 lines; sections "motion vectors", "jitter", "the upscaler, in two halves"), `runtime/vk_dlaa.h`, `runtime/vk_xess.c`, `runtime/vk_fsr3.c`, `tools/ffx_sdk.py`, `tools/xess_sdk.py`, `runtime/shaders/dlaa_mv.comp`, `runtime/shaders/ffx/`, `runtime/vk_fsr.c` (FSR 1 EASU/RCAS at present).
- Snap mapping: new `runtime/nvn/nvn_upscale.cpp` plus per-backend files; hook at `nvn_draw.cpp` pass begin/end (the design doc's planned `PassHook`) and the present step in `nvn_gfx.cpp`; scale and bias already exist (`SNAP_RENDER_SCALE`, `SNAP_LOD_BIAS` in `nvn_entry.cpp`). Settings in `runtime/hle/settings.cpp` and the ImGui overlay.
- Differences: Astral Chain's scene pass is found by pass signature (depth prepass then R11G11B10F target at `vk_dlaa_pass`). Snap knows its NU pass graph (`analysis/maps/render_graph.md`), so detection should use that instead. This is the biggest single item and also covers the `AGENTS.md` 4K/DLAA/DLSS section.
- License: code for the host plumbing; Intel and NVIDIA binaries are never shipped. Streamline (named in `AGENTS.md`) is not needed on this route, which avoids NVIDIA redistributables; confirm with the owner that hand-declared NGX ABI is acceptable.

### 4. Per-object motion vectors via last-frame vertex variants (L)
- What: camera motion from one CPU double-precision reprojection matrix (zero motion exactly when the camera is still), then moving objects redrawn into the motion image with a "previous position" vertex shader variant generated by a translator patch and a tiny motion fragment shader. Last frame's per-object uniform blocks are copied into an arena and matched next frame by draw identity (program, index buffer, first vertex, counts) plus nearest anchor translation, because the game's allocator hands out new addresses every frame.
- Where: `runtime/vk_mvprev.c`, "per-object motion" in `runtime/vk_exec_draw.c`, `tools/xlate/patches/0012-motion-previous-position.patch`, `tools/xlate/Program.cs` (mvprev), `tools/make_mvobj_fs.py`, `runtime/shaders/dlaa_mv.comp`.
- Snap mapping: `runtime/nvn/nvn_draw.cpp`, `nvn_shader.cpp`, `scripts/shader_harness`. This is the "interim/reference option" in `AGENTS.md` ("Long-term motion source: NU-level object/camera/skinning history"). Use it as the interim route; skip the draw-identity matching if NU history becomes available. Needs the Snap shader path to accept the translator patch.
- License: code (MIT, derived from TotK Recomp commit `0ae47a5`; keep that notice).

### 5. GPL fast-link pipelines plus packed shader cache and install-time prepare (M)
- What: `VK_EXT_graphics_pipeline_library`: stage libraries compiled ahead, a draw with a new state fast-links at once, an optimized pipeline replaces it from a background queue; pipeline keys seen in play are saved and rebuilt at boot; a draw whose pipeline is not ready is skipped when its pass allows. Fallback flags per GPU (`AC_GPL`). `shader-cache.pack` folds 117,000 loose translation files into one file. `--prepare-shaders` plus a shipped list of seen variants (`tools/data/shader-answers.bin`) warms the driver cache at install.
- Where: `runtime/vk_pipeline.c` (around lines 1471-2310 and 3397 stats), `runtime/vk_caps.c`, `tools/pack_shader_cache.py`, `tools/prepare_shaders.py`, `tools/shader_census.py`, `gfx_shader.c` "cache pack".
- Snap mapping: `runtime/nvn/nvn_pipeline.cpp` already has async build and a saved `VkPipelineCache` (`pipeline_keys.bin`, `pipeline_cache.bin`), so only the GPL link layer and the pack file are new; `nvn_shader.cpp` stores translated shaders keyed by SHA-256 on disk (same key idea). The shipped variant list must be generated from the user's own run, not distributed.
- License: code.

### 6. Uniform snapshot at submit and a recorder thread (M each, only if profiling asks)
- What: copy every bound uniform range of CPU-written pools at submit so late draws do not read another object's matrix when replay runs after the game moved on (`vk_usnap.c`); a ring-buffer thread that makes the `vkCmd*` calls so the replay thread only packs arguments (`vk_rec.c`); replay on its own thread at most one frame behind (`AC_REPLAY_ASYNC`).
- Where: `runtime/vk_usnap.c`, `runtime/vk_rec.c`, `runtime/vk_rec.h`, `runtime/vk_backend.c`.
- Snap mapping: `nvn_draw.cpp` replay, `nvn_internal.h`. Snap's constant-buffer read happens "at replay" (`docs/nvn-vulkan-design.md`); if replay moves off the submit thread, snapshot is required for correctness, the recorder only for speed. Check Snap's 60 FPS budget numbers first.
- License: code (design from MacNativeGames and LAS/SavageWind, all MIT).

### 7. Diagnostics: GPU pass profiler, breadcrumbs, flight recorder, perf overlay, frame dump, RenderDoc hook (S-M each)
- What: `AC_GPU_PASSES=<n>` per-pass GPU timestamps summed into a table (`vk_gpuprof.c`); `VK_AMD_buffer_marker` breadcrumbs to localize device loss (`vk_crumbs.c`); a 600-frame flight recorder with thumbnails dumped on F8 or a frame spike (`flight.c`, `tools/flight_view.py`); NVML/ADL performance overlay with a one-second summary ring (`perfmon.c`, adapted from SavageWindRecomp); per-pass PNG dump with SPIR-V hashes (`vk_dump.c`, `AC_DUMP`); optional RenderDoc in-app capture (`rdoc.c`).
- Snap mapping: `runtime/nvn/nvn_perf.cpp`, `runtime/os/perf.cpp`, `runtime/ui/ui_overlay.cpp`; Snap already has a frame dump (`NVN_FRAME_DUMP*`) and a per-pass dump hook (`SNAP_PASS_DUMP`), so the per-pass GPU table and breadcrumbs are the real gaps. They serve the `AGENTS.md` requirement to measure render, GPU and present times and per-pass costs separately.
- License: code.

### 8. Spin-wait search and core gate (M)
- What: `tools/spin_waits.py` (capstone) finds loops that only another thread can end and prints patch-table lines; the generator routes their back-edges through `rt_spin_gate`, which pauses the virtual core and hands it to a waiter after 50 us. Plus asynchronous preemption of a thread holding a virtual core in lifted code, priority inheritance through guest mutexes, SDK preemption-disable protocol (TLR+0x100/+0x102, SVC 0x36). Tests: `tests/spin_waits_test.py`, `tests/excl_mt.py`.
- Where: `tools/spin_waits.py`, `runtime/kernel.c` (`kern_spin_gate`, async preempt), `runtime/svc.c`, `tests/spin_waits_test.py`.
- Snap mapping: `runtime/os/os_thread.cpp` (`poll_preempt` exists, cores modelled), `recomp/snap_gen.py` for the back-edge hook, `runtime/cpu.h`. `AGENTS.md` already plans "backward-branch/yield polling"; this is a worked, tested version. Needed only if Snap shows the same job-queue stalls; run the finder first and see how many loops it reports.
- License: code (nxnative MIT, `THIRD_PARTY_NOTICES.md` row).

### 9. Cheap repo and build safety: content guard, hooks, object hashing, selftest (S)
- What: `tools/check_content.py` refuses game-derived suffixes, keys, generated-code markers and files over 1 MB, runs in CI and as a pre-commit hook (`.githooks/`, `.github/workflows/content-guard.yml`); every generated file carries a `GENERATED_MARKER`; objects are hashed before link (a flipped-bit RAM module once corrupted a build); `--selftest` runs without game files; a worktree helper (`tools/worktree.py`, `tests/worktree_test.py`).
- Snap mapping: `scripts/` plus a pre-commit hook; fits "Repository hygiene" and the never-distribute rule in the workspace `AGENTS.md`. Add Snap-specific suffixes (`.drp`, `.nutexb`, `.nushdb`, `.nso`).
- License: code.

### 10. Settings, input and mod conveniences (S-M each, lower priority)
- What: `cfg.c` one table of known keys with `.ini` file plus `AC_*` env override, live `cfg_set` and `cfg_save` that rewrites only changed keys; two-pane option rows menu with description pane and hold-to-repeat (`ui/option_rows.cpp`, from SavageWindRecomp); rebinding page and `AC_INPUT_SCRIPT` / `AC_INPUT_RECORD` frame-exact input replay (`input_map.c`, `input.c`, docs in `AGENTS.md`); mod folders in emulator layout, ordered with disable list (`mods.c`); Atmosphere cheat VM (`cheats.c`); color grade pass (`vk_color.c`, `color_grade.comp`); camera speed by editing a RomFS pack copy (`camera_speed.c`); installer with resume file (`installer/`, `docs/installer.md`).
- Snap mapping: Snap already has `runtime/hle/settings.cpp`, `runtime/ui/ui_controls.cpp` and `ui_test.cpp` (scripted), `runtime/mods/` (DRP entry replacement). Most useful: input record/replay for regression runs (Snap's A-mash route shifts with load time, per `AGENTS.md`), and the ini-writes-only-changed-keys behavior. The cheat VM and emulator-layout mods do not match Snap's DRP loader; skip. The installer is an eventual need (Snap needs one for release) and is a good template, but it is a large GUI project.
- License: code.

## 4. Also worth knowing

- HDR (`runtime/vk_hdr.c`, `hdr_tonemap.frag`, `hdr_encode.comp`, `hdr_ui.frag`): Snap already has an HDR10 present (`runtime/nvn/nvn_hdr.h`, SNAP_HDR). Astral Chain's extra ideas: recognize the tonemap shader by constants and resources (not hash), restore highlights by an extended-curve luminance ratio (RenoDX approach), separate paper-white and HUD-white, and classify each window pixel as scene or HUD by comparing it to the clamped scene. Snap's present-only PQ pass cannot do the HUD split without this. Worth reading when Snap's HDR gets a second pass.
- VRAM handling: `vk_headroom.c` (pre-allocate the next VMA block on a background thread; 8-19 ms stalls otherwise), ASTC to BC1/BC3 re-encode on small cards (`vk_astc.c`, `bc1_encode.comp`, `bc3_encode.comp`), and a VRAM-pressure fallback chain. Snap has ASTC and BC7 files but no VMA; use only if texture stutter shows up.
- Frame pacing: present slot = previous slot plus one period, hold the host present until then (`nvn_api_obj.c` `present_period`, from MacNativeGames `nvn_pace.h`), plus a 1 ms timer-resolution option. Relevant to the 60 FPS on 144 Hz judder noted in `AGENTS.md`.
- Wide screens: patches that hand the output aspect to camera sites and move HUD layout parts (`resolution.c`, `res_wide_*`). Snap's game and layout data differ; the method of finding projection sites is the transferable part, and `AGENTS.md` already notes the FOV writer is unfound.
- Linux host split behind `runtime/platform/platform.h`. Snap is Windows-first; not needed now.

## 5. Conflicts with Snap's decisions

1. **Scene detection by pass signature** (`vk_dlaa_pass`): conflicts with the plan to hook the known NU render-pass graph. Use the graph.
2. **In-place resolve on the light target before bloom, replacing the game's SMAA**: Snap must prove photo scoring and thumbnails never read temporally resolved or jittered output (`AGENTS.md` scoring safety). Astral Chain has no equivalent constraint, so none of its checks cover it.
3. **Viewport-offset jitter**: moves what the scene rasterizes, not the camera block. It is cheap, but Snap's Evaluation depth target and visibility tests (1920-wide depth exemption) must be excluded explicitly.
4. **Patching the game's resolution ladder** (six `csel` sites) to make the game render a smaller scene: Snap scales at the NVN layer (`SNAP_RENDER_SCALE`) and plans to scale every fixed target together. Different mechanism; keep Snap's.
5. **Dropping `rtld` and linking in the host** (MacNativeGames style) with `sdk` handled as host overrides: Snap decided on 2026-11-11 to lift `sdk` and keep guest libc/libc++/unwinder. Do not take their libc/exception approach.
6. **Language and layout**: C runtime with `AC_*` env names, Windows-only calls going through `platform.h`. Snap is C++ with `SNAP_*`; adapt names, do not mirror files.
7. **Installer builds the game on the player's PC with bundled llvm-mingw**: matches Snap's "no distributed generated code" rule, but the repo itself appears to ship game UI assets (`controller-ui/`). Never copy those.
8. **DLSS without Streamline**: `AGENTS.md` names Streamline; this repo uses the driver's NGX core. Decide which route (owner call; the NGX route ships no NVIDIA binary).
9. **Shipped shader variant list** (`tools/data/shader-answers.bin`): derived from game shaders' hashes. Snap should generate its list locally and not commit it.
