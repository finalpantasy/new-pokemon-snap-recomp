# New Pokémon Snap native PC project

This is the durable project plan and working agreement. Read it before changing the project. Update it when a decision or verified project state changes.

## Goals and constraints

- Personal side project for the user's own dumps. Game files, keys, extracted assets, generated game code, and proprietary binaries stay local and are never distributed.
- Build a real native x86-64 PC version through ahead-of-time static recompilation, not a runtime emulator or CPU JIT.
- First stabilize correct gameplay and the original frame-rate behavior, then target native 4K, DLAA/DLSS, and real 120/144/240 FPS.
- Make modding a first-class feature: texture/model/UI replacement, new Pokémon, new courses, additional content, and versioned native/content mod APIs.
- Include game-themed settings and mod-management menus with strong controller, mouse, keyboard, and accessibility support.
- Preserve photography correctness: scoring, saved photos, editing, and depth-based evaluation must not be corrupted by upscaling or temporal rendering.
- Prefer MIT/Apache-2.0 code for the implementation. Use GPL/AGPL projects as references unless the project license is deliberately changed. Do not copy proprietary SDK code, leaked material, or NVIDIA binaries into the repository.

## Current verified state

- Source files are under `<dumps-dir>`: one base XCI, four NSP updates, and the user's `prod.keys`.
- **Target revision locked (2026-11-11): v262144 (2.0.1).** Its `main` is byte-identical to v196608 (2.0.0), so one binary covers the full 2.0 content. Keep v0 (1.0.0) as the diff reference. Patch title ID `0100F4300BF2C800`; base program ID `0100F4300BF2C000`.
- Revision audit (`scripts/audit_revisions.py` -> `analysis/revisions/summary.{md,json}`; all five program NCAs match their CNMT hashes):

  | Rev | Ver | main build ID | .text | FDEs | def/imp dynsyms |
  |---|---|---|---|---|---|
  | v0 | 1.0.0 | 7B30946B... | 0xa11f80 | 24,347 | 58/724 |
  | v65536 | 1.1.0 | B8301424... | 0xa35cb0 | 25,314 | 56/720 |
  | v131072 | 1.2.0 | 3147ADD3... | 0xa2dab0 | 24,561 | 56/721 |
  | v196608 | 2.0.0 | DDB900FF... | 0xa38e40 | 24,601 | 57/731 |
  | v262144 | 2.0.1 | DDB900FF... | 0xa38e40 | 24,601 | 57/731 |

  - **Shared modules and settings:** `sdk` and `rtld` are identical in all five revisions. NPDM main-thread stack drops from 0x100000 to 0x4000 at v196608.
  - **Symbols and RTTI:** every revision is stripped to dynsyms. There are 419 RTTI type-name strings, nearly all `nn::`/NEX, boost or libc++, so the game code is built without RTTI.
  - **Compiler signals:** no toolchain change. There are no compiler version strings, prologue statistics are the same in every revision, and about 89% of function bodies match when addresses are masked. v65536 is an outlier with +967 FDEs and a `/perforce/lentil` build path instead of the Jenkins build path; the reason is unknown.
  - **Content:** v196608 adds 157 RomFS files (`nx/Sound` 86, `nx/Pack` 44, `common/UI` 27); v262144 has the same file names, and changes inside files are unknown. Patched RomFS listings are name-level only.
- Validated import path (`analysis/import/validation.md`, 9/9 checks pass):
  - **Route 1, Ghidra:** Ghidra 12.1.2 with Adubbz SwitchLoader 1.6.1 and Temurin JDK 21.0.9, installed in `tools/` because the PATH Java is a JRE that Ghidra rejects. NSO import base is 0x7100000000.
  - **Route 2, ELF:** `scripts/nso2elf.py` produces a deterministic ELF64. Its segments, 789 dynsyms and 27,516 relocations match NSTool and the Ghidra NSO route.
  - **Primary route and seeding:** use the Ghidra NSO route as primary. Offset 0 is MOD0, not code, so seed analysis from DT_INIT, DT_FINI and init_array.
  - **Wrappers:** `scripts/ghidra_import.ps1` imports and `scripts/ghidra_analyze.ps1` runs full analysis plus export.
- Deterministic maps in `analysis/maps/` (README there). Two fresh full analyses (about 12-13 min each) gave byte-identical map files.
  - **Functions:** 26,756 Ghidra functions vs 24,601 `.eh_frame` FDEs, 24,593 in both. There are 816 Ghidra-only functions, 8 FDE starts with no function and 4,252 size mismatches (`functions_disagreements.tsv`). 14,032 functions are address-taken.
  - **Imports:** 731 named. nn 413, libc++ 172, libc 133, nvn 1.
  - **Relocations:** RELATIVE 25,240, ABS64 1,570, JUMP_SLOT 649, GLOB_DAT 57.
  - **RTTI:** 44 objects, all std/runtime types. There are 2,006 vtable candidates (`vtables_candidate.tsv`), which is the main route to game-class recovery.
  - **NVN bootstrap:** `0x910270` calls `nvnBootstrapLoader`, then two straight-line loaders (`0x254c40` and `0x90a0c0`) each make 516 individual `getProcAddress` calls. Loader 0x90a0c0 stores into `.bss` from `0x1488db0`. Map: `nvn_procs.tsv`. These are module offsets.
  - **Render passes:** 182 strings in 157 functions. The main setup candidates are `0x5bd940` (DOF, SSAO, DynamicShadow, PhotoEditingFrame), `0x5807d0` (photo-edit effect factory), `0x59ab20` (tonemap and FXAA), `0x5a0e70` (SSAO), `0x4cb480` (shadow) and `0x7fdaf0` (Lumen). The `nu::RenderPassBase` string is only the boost topological-sort error path (`0x988ac0`). The graph builder has no strings and must be found through its vtables.
- **Function list for the lifter** (`analysis/maps/functions_final.{tsv,md}`, `scripts/reconcile_functions.py`, `ghidra_analyze.ps1 -Stage Reconcile`):
  - **Counts:** 25,500 functions (24,601 FDE + 899 non-FDE) and 649 PLT stubs. 4 functions remain `unresolved`.
  - **Coverage:** `.text` is fully classified (98.41% functions, the rest padding and MOD0) with no overlaps.
  - **Gaps:** the region 0x21d45c-0x254c40 has no unwind data.
  - **Lifter risks:** 730 recovered jump tables and 2 unrecovered, 1,594 indirect tail calls, 35,856 `blr` sites, 452 noreturn functions (found by heuristic) and 23 interior-entry targets (`functions_interior_entries.tsv`).
  - **Determinism:** two runs gave identical outputs.
- **Classes and render graph** (`analysis/maps/{classes.tsv,class_functions.tsv,render_graph.md,render_passes_final.tsv}`; scripts `scan_vtable_writers.py`, `classify_vtables.py`, `trace_render_graph.py`; deterministic, re-checked by the coordinator):
  - **Classes:** 1,950 classes from 2,006 vtable candidates.
  - **Pass graph:**
    - The builder is `0x5bd940`, called from `0x634070`. It creates 37 pass nodes through `0x5cd800` and the factory `0x97a680`, then adds them with `add_vertex 0x9863c0` and `add_edge 0x987d20`.
    - Sort: `0x988060`, which calls the DFS `0x988ac0`; called from `0x5ca458`.
    - Per-frame execute loop: `0x988fe0` calls vtable slot 5 (`0x982210`) on each enabled pass. Its caller chain is `0x5d6860` <- `0x4df460`.
  - **Base class:** the RenderPassBase vtable is probably `0xcc3fe8` (medium confidence), with all passes using the concrete `0xcc4058`. Per-pass behavior lives in attached plug/task objects, not vtable overrides.
  - **Feature setups:** the earlier candidates `0x5807d0`, `0x59ab20`, `0x5a0e70`, `0x4cb480` and `0x7fdaf0` build feature objects, not graph nodes.
  - **Static only:** all of this is static analysis and needs dynamic confirmation.
- **CPU backend decision** (`docs/cpu-backend-eval.md`; source reading only, no builds):
  - **Choice:** prototype the LAS/TotK-style AArch64-to-C generator first. Its guest-equals-host pointers, symbol-level SDK replacement, FDE-seeded discovery and Unicorn differential tests fit this plan.
  - **Bake-off:** Kirikae, which masks and offsets every memory access, is the bake-off candidate on a fixed function subset.
  - **Prerequisite:** clang must be installed before the prototype.
  - **References:** clones are in `tools/ref/{totk,las,kirikae}`.
- **Patched RomFS** (`extracted/romfs-v262144/`; census in `analysis/romfs/`, `scripts/romfs_census.py`):
  - **Extraction:** NSTool wrote base + v262144 patch: 3,310 files, 6.81 GiB, matching the 2.0.1 listing.
  - **Contents:** 1,125 DRP archives and 2,185 WEM files. All DRP headers parse. 537 DRPs are encrypted with their own seed table, so no external key is involved.
  - **Oodle:** 71,868 of 82,127 DRP entries (17.35 GB decompressed) are Oodle-compressed, including models, textures, shaders, `.luo`, text, UI and soundbanks.
  - **Shaders:** game shaders are 3,138 `nushdb` entries in `Resident.drp`/`resource.drp`. The 8 plaintext `!!NV*5.0` programs in `main` are NVN driver helpers.
  - **Registry candidates:** `analysis/romfs/registries.md`.
- **Runtime scope (2026-11-11; `analysis/runtime/`):**
  - **NVN:** 205 of 516 procs are called (652 sites), mostly through the static `nn::gfx` NVN backend at `0x8f4670-0x924b00` plus about 26 inlined callers. The call-site order is CommandBuffer 286, state objects 110, Buffer 60, Texture 54, MemoryPool 49, then Device, Queue, Sampler, Window and Sync.
  - **Imports:** 731 in total, including libc++ 167, libc 133, `nn::os` 86 and socket 49. 84 are on the direct boot path, and 514 with virtual and address-taken edges.
  - **Threads:** 10 thread-creation sites, 5 of them Wwise. The wrapper `0x8dea70` maps a priority enum to (31, 23, 16, 8, 1), and the core mask is always 7. One fiber job scheduler sits at `0x548000-0x54f000`.
  - **Boot blockers:**
    - C++ exceptions through lifted frames (504 `__cxa_throw`, 4,274 `_Unwind_Resume`).
    - Guest-layout libc++ objects.
    - libc ABI: 32-bit `wchar_t`, AAPCS64 `va_list`, musl structs, and `setjmp`/`longjmp` in Lua.
    - Horizon thread semantics.
    - `nn::audio` and `nn::fontll` are hard.
- **NVN-to-Vulkan milestone 1 (2026-11-11; `docs/nvn-vulkan-design.md`, `runtime/nvn/`, `tests/nvn/`):** the cleared-frame test presents 300 frames with 0 validation errors and 0 warnings, under synchronization validation.
  - **Entry points:** 83 of the game's 205 called procs are implemented, in a name-keyed table handed out through `nvnBootstrapLoader`.
  - **Object model:** guest NVN objects are opaque and hold only a host pointer; their sizes come from the game's allocation sites.
  - **Command buffers and syncs:** command buffers record into host memory and replay into Vulkan at submit. Syncs are timeline semaphores.
  - **Present:** the window's textures are offscreen images blitted to the swapchain. That blit is the future 4K/DLSS hook.
  - **Signature provenance:** signatures match TotK's MIT community-reconstructed `nvn.h` and were cross-checked against Snap call sites.
  - **Open items:**
    - `DeviceGetInteger` GLSLC version values.
    - Real Maxwell texture layout sizes.
    - Texture-handle packing above 20 bits.
    - A 60 FPS cadence on a 144 Hz display judders, so present pacing needs VRR or frame-pacing work later.
- **First real boot (2026-11-11; `analysis/recomp/boot_progress.md`, `host/README.md`):**
  - **Build:** one `snap_boot.exe` links lifted main and sdk with the loader, HLE, audio, NVN (through a generated AArch64-to-x86-64 bridge) and the real `nn::os` layer. Build with `tools/venv-recomp/Scripts/python -I host/build_snap.py`; run with `powershell -File host\run_boot.ps1 -Timeout 100 run`.
  - **Symbol precedence:** fixed to host override, then main, then sdk (49 symbols are defined in both).
  - **Init order:** DT_INIT already runs init_array, so it must not be run twice.
  - **How far it gets:** all 306 initializers run. `nninitStartup` and `nnMain` run through fs mount, fibers, the game thread, account and save mount, a real Vulkan NVN device, window init, one present, RomFS texture loads and about 142 distinct NVN calls.
  - **Blockers:**
    1. A guest null dereference in `main+0x9897c0` on the game thread.
    2. Fixed 2026-11-11: `svcGetInfo` (0x29) is in `os_svc`, along with OutputDebugString and Break. The call sites and values are in `runtime/os/svc_getinfo.md`; the os tests pass 227 of 227; the `host/boot.cpp` stopgap is dead code.
    3. 305 requested-but-unregistered NVN names, none called yet.
    4. Spin-loop gating is not done.
  - **Indirect-call misses:** 0 so far.
  - **First UI (2026-11-11):** the game reaches the autosave notice and then the voice-language select screen. Text is legible and the image is upright (`analysis/boot/fixed_screen.png`).
    - **Upside-down image:** the game selects the UPPER_LEFT window origin, which now gets a negative-height viewport.
    - **Garbled text:** the glyph atlas is an R8 512x2048 pitch-linear texture that the CPU keeps writing. It is now read with its stride and re-uploaded when its hash changes.
    - **Debug switches:** `NVN_TEX_DUMP`, `NVN_DRAW_LOG`.
    - **Open NVN defects:**
      - about 15,000 render-target-also-sampled skips per minute (mip-level render targets);
      - `vkResetQueryPool` used without hostQueryReset;
      - sampled-view type mismatches;
      - dual-source blend used without the feature.
- **nn::os layer (2026-11-11; `docs/horizon-os.md`, `runtime/os/`, `tests/os/`):** 197 overrides covering threads, TLS, fibers, time, every sync type, MultiWait, VAMM (`VirtualAlloc`) and the heap. SVC 0x1d is served on the same wait queues.
  - **Layouts:** guest ThreadType is 0x1c0 bytes and the TLR 0x200, with the thread pointer at +0x1f0 and the ThreadType at +0x1f8.
  - **Scheduler:** 3 virtual cores with ideal-core pinning, priority/FIFO wakeups and priority inheritance. Preemption happens at cooperative check-ins (sync calls, `GetSystemTick`, spin back-edges); a thread that skips check-ins gets an overflow core after 2 ms.
  - **Tests:** 145 checks pass, stable over 7 runs. The stress test found and fixed a condvar lost-wakeup bug.
  - **Open items:** the loader must pass the musl thread-pointer init and destroy hooks. Spin loops need lifter check-ins. About 10 rarely used `nn::os` functions (suspend, shared memory, interrupt events) are still lifted.
- **sdk lift and two-module loader (2026-11-11; `analysis/recomp/sdk_and_boot.md`, `host/`):**
  - **sdk lift:** 36,831 functions, 102 MB of C, compiled in 66 s. The differential test passes 263 of 263.
  - **Loader:** guest pointer equals host pointer, and all relocations resolve. Table-based indirect calls cost 2.0 ns each.
  - **Exceptions:** C++ throw and catch across lifted frames passes, as does Lua `lua_pcall` through `setjmp`/`longjmp`. The resume checks add 11.9% object size for main and 4.9% for sdk.
  - **Init order:** the 111 sdk initializers run when three `nn::os` calls are stubbed. Boot blockers in order: `ThreadManager` TLS singleton, SVC 0x1d `SignalProcessWideKey`, then VAMM `AllocateAddressRegion`/`AllocateMemoryPages`, which need a real implementation. Main's 195 init_array entries have not run yet.
  - **Bug to fix:** symbol precedence for the 57 symbols defined in both modules must be host override, then `main`, then `sdk` (rtld load order: main wins). The loader currently does sdk-first, which was a coordinator brief error.
- **HLE services (2026-11-11; `docs/hle-services.md`, `runtime/hle/`, `tests/hle/`, `analysis/runtime/hle_coverage.tsv`):**
  - **Coverage:** of 731 imports, 153 are implemented, 67 are stub-noop and 408 stay lifted pure software. The 103 still todo are owned elsewhere: `nn::os` 86, audio 16 (now done) and nvn 1.
  - **fs:** `rom` mounts onto `extracted/romfs-v262144` with a mod overlay list, and `savedata` uses files `savedata:/%08x%08x`. Result codes are taken from the game's compares.
  - **Saves:** written to `%APPDATA%\SnapRecomp\save\<uid>\` with generation directories, an atomic `CURRENT` swap and 5 backups. Every crash stage tested leaves the old or the new save, never a torn one.
  - **Input:** XInput and SDL3, plus keyboard and mouse, with a JSON remap table. The host reports docked and in focus.
  - **Integration:** the loader must call `hle::bind_all`.
  - **Tests:** 746 host checks, 0 failed.
- **Audio (2026-11-11; `docs/audio.md`, `runtime/audio/`, `tests/audio/`):**
  - **Surface:** the game uses AudioOut only, with no AudioRenderer. Wwise mixes everything itself and outputs 48 kHz s16 stereo (or 5.1), 1024-frame buffers, 4 queued. The sink is at `main+0x124640`.
  - **Implementation:** the 16 `nn::audio` imports are host overrides on a WASAPI shared event-driven backend, with a null fallback. The test passes with 0 underruns; the latency is about 22 ms device plus 85 ms guest queue.
  - **Volume:** BGM/SE/VOICE volume needs the Wwise bus names from the soundbanks. Only master volume is applied today.
- **Data schemas (2026-11-11; `analysis/registries/`, `scripts/fb_*.py`):**
  - **Coverage:** all 37 FlatBuffers data types have inferred `.fbs` schemas with per-field confidence. Round trips are byte-identical for every walked file, except 2 of 100 `cutb` files (known gap).
  - **ID model:** species are identified by P-number, with one row per form; `form_index` and `gender` select the form. Course and entry keys are the CRC32 of the base name.
  - **Mod requirements:** the README lists exactly what a new Pokémon or a new course must add.
  - **Unnamed fields:** PokemonData slots 7-15 and most `levelb` and photo-scoring fields are still unnamed.
- **NVN milestone 2 (2026-11-11):** a textured, depth-tested mesh is drawn through NVN entry points with the real game shader pair `vdkEfxSystemConstantVS/PS`, translated by the Ryujinx harness and cached by SHA-256. 300 frames, 0 validation errors.
  - **Coverage:** 196 of 205 called procs are implemented. Missing: indirect draws, compute dispatch, texture copies, the timestamp query, patch size, alpha ref and ReportCounter.
  - **Binding model:** cbuf set 0, binding `1+stage*16+slot` (NVN slot = reflection b-3); SSBO set 1; samplers set 2; images set 3. Texture handle = `texId | samplerId<<20`, and the BindTexture index = (handleIndex-8)/2.
  - **Unknowns:** see `docs/nvn-vulkan-design.md` (packaged-texture deswizzle, QUADS, u8 indices, alpha test, front-face on real geometry).
- **NVN phase 3 (2026-11-11):** all 205 of 205 called procs are registered.
  - **Textures:** the Maxwell block-linear deswizzle matches an independent decode byte for byte on 10 real NUTEXB files (BC1/4/5/6H/7, RGBA16F/32F, cube, 3D, mips).
  - **ASTC:** decoded on the CPU and transcoded to BC7, then cached.
  - **State gaps closed:** QUADS, QUAD_STRIP, LINE_LOOP, POLYGON, u8 indices, alpha test and partial clears.
  - **Hooks (off by default):** `nvnHostSetRenderScale` with target exemptions, and `nvnHostSetAnisotropyOverride`.
  - **Not tested yet:** alpha test, indirect draws, compute, counters and the hooks.
  - **Open question:** whether the game reads GPU-written memory without a sync.
- **Shader path (2026-11-11; `analysis/shaders/`, `scripts/nushdb_parse.py`, `scripts/shader_harness/`):**
  - **Format:** nushdb = SSBH SHDR v1.2 (matches `ssbh_lib`, MIT). The GLSLC inner binary decodes fully (sections, SPH, SASS, reflection); some control fields and reflection tables 5-6 remain undecoded.
  - **Census:** 6,227 programs, 1,639 unique (VS 291, PS 1,343, GS 2 with transform feedback, CS 3). Only 263 `resource.drp` programs carry GLSL or NV_gpu_program5 references.
  - **Translator decision:** use the Ryubing fork of Ryujinx `Ryujinx.Graphics.Shader` (MIT, commit 4d5a19d3) as an offline SASS-to-SPIR-V build tool.
    - It translates all 1,639 unique programs in 11.6 s, and 1,638 pass `spirv-val` (Vulkan SDK in `tools/vulkan_sdk`).
    - The one failure needs the `StorageImageWriteWithoutFormat` capability.
    - Integration requirements: keep the NOP padding after the declared code size, and set the fragment origin to upper-left.
  - **Unknowns:** numerical correctness vs hardware (not executed; the `resource.drp` GLSL is the first reference), and which constant-buffer slot holds the NVN texture handles.
- **CPU lifter prototype (2026-11-11; `recomp/`, `runtime/`, `tests/snap_func.py`, reports in `analysis/recomp/`):**
  - **Base:** the TotK generator (MIT, notices in `THIRD_PARTY_NOTICES.md`), with LLVM/clang 23.1.5 in `tools/llvm` and the venv `tools/venv-recomp`.
  - **Function discovery:** the generator's own discovery finds all 26,149 of our starts. Of 732 jump-table sites, 695 agree. At 31 the generator's target set is a strict subset of ours, which would abort at run time, so generated code uses our targets.
  - **Coverage:** 0 of 2.63M instruction words are unsupported. The binary has no LSE atomics, 39,150 LL/SC exclusive words in 3,099 functions, and one system-register read (`tpidr_el0`).
  - **Differential test:** 434 functions were lifted and compiled in 3.5 s. Against Unicorn, 313 of 313 runnable functions pass (2,234 cases, deterministic). 121 are untested because random inputs fault in Unicorn.
  - **Full build (2026-11-11, `analysis/recomp/full_build.md`):** generating all 25,500 functions plus 649 PLT stubs takes 24.8 s at 3.5 GB peak and produces 178.8 MB of C in 134 TUs.
    - Cold clang -O2 x86-64-v3 compile: 192 s at 12.9 GB peak, 0 failures, giving `snap_main.lib` (45 MB).
    - Incremental by TU: a one-chunk change rebuilds in 17 s.
  - **Jump tables:** all 37 disagreements are resolved (generator right at 34, our map at 3), and the two now agree at 729 sites.
  - **Per-instruction tests:** 438,738 distinct words, 0 mismatches.
  - **Function tests:** 390 of 434 functions tested, 0 failures.
  - **Host spike:** lifted `0x910270` runs natively in `host/spike.cpp`. It makes 1,033 NVN proc requests (517 names), exactly matching the static map.
- **Language decision (owner, 2026-11-11):**
  - **Lifted game code:** generated C compiled with clang.
  - **Core host runtime:** C++, reusing TotK/LAS runtime patterns. Horizon HLE, NVN->Vulkan and DLSS/Streamline belong here.
  - **Mod loader and untrusted asset/mod-package parsers:** Rust, behind a C ABI.
  - **Priority rule:** a fully playable, correct game comes first. If Rust would slow any step on the path to playable, use C++ there and revisit later.
- **Oodle decision (owner, 2026-11-11):** unpack DRP entries with the game's own statically linked Oodle decoder from `main`, run under Unicorn as an offline tool only. Never source the Oodle DLL. Output goes to `extracted/drp-v262144/`.
- **DRP unpack done 2026-11-11.** All 82,127 entries (16.16 GiB) are unpacked with status ok, each checked against the loader's CRC32. The manifest is `analysis/romfs/drp_unpacked.tsv`.
  - **Decoder:** `OodleLZ_Decompress` is at `main+0x7240`, called only by the DRP loader `0x5e3b84`. Each payload is a 4-byte CRC32 of the decompressed data followed by the Oodle stream.
  - **Harness:** `scripts/oodle_unicorn.py` and `drp_unpack.py`, at about 11 MB/s, plus the XOR helper `scripts/drp_xor/drp_xor.rs`.
  - **Shaders:** 3,138 `nushdb` files, which are SSBH containers of NVN GLSLC output (control section, header, raw Maxwell SASS, reflection), mostly one VS+PS pair each. The 156 in `resource.drp` also carry NV_gpu_program5 assembly and GLSL 440 source. There is no SPIR-V. This means a Maxwell-to-SPIR-V translator is required for most shaders, while the `resource.drp` subset can use the shipped GLSL.
  - **Scripts:** all 482 `.luo` files are stock stripped Lua 5.1 bytecode (per-character behavior).
  - **Registries:** FlatBuffers. PokemonData has 509 records x 23 fields. `DC4A8177` is FlatBuffers, likely collision data.
  - **Unknowns:** see `analysis/romfs/unpacked_formats.md` (SSBH SHDR layout, registry schemas, font obfuscation, nutexb header).
- The v262144 program NCA was selectively extracted and its SHA-256 exactly matches the CNMT manifest: `22665eeb79569efd45bc2fe7a31b1ed0f02a15150a9cd34129afc6bf29541cad`.
- ExeFS contains `main`, `main.npdm`, `rtld`, and `sdk`. All three NSO modules were decompressed successfully.
- Latest `main` build ID: `DDB900FF04A912ED42430508389853CFAF1ACCCE`.
- Latest uncompressed `main` SHA-256: `b22eafbce786fac0e6a787459abc8d7488a6a529d2b976bfb7b6e23673ee3777`.
- `main` has about 10.47 MiB of text, 732 imported dynamic symbols, 57 defined dynamic symbols, and 518 NVN API-name strings. The NVN string count is an upper bound, not a confirmed count of used functions.
- `scripts/pfs0.py` lists and selectively extracts uncompressed PFS0/NSP members with SHA-256 verification.
- hactool 1.4.0 rejects one entry in the user's key file. NSTool 1.9.2 parses the same keyset successfully; use NSTool plus the update ticket for this project. Do not print, edit, or copy the keys.
- The update RomFS requires the base NCA for update-partition reads. A complete updated RomFS has not yet been extracted.
- REA rejected the raw NSO as an unsupported format. The validated ELF (`analysis/import/v262144-main.elf`) has not yet been tried in REA.
- Research is complete. No playable port has been implemented yet.

## Engine and rendering facts

- This is not an Unreal Engine target. Do not design around Unreal tools or an Unreal PC renderer.
- The shipped binary identifies Bandai Namco's NU4 drawing framework, Lumen, and project code under `lentil`.
- Evidence is in `work/v262144-main.strings.txt`: NU4 at line 8978, Lumen at line 10108, and a Boost-topologically-sorted `nu::RenderPassBase` graph at line 9004.
- Static middleware includes Lua 5.1, Wwise, Oodle, Boost, NEX, libcurl, and Nintendo SDK components. These can generally be lifted with `main` rather than rewritten.
- The renderer has named forward, deferred-worker, shadow, SSAO, bloom, tonemapping, FXAA, photo-editing, and photo-evaluation passes.
- Contemporary reports describe the Switch game as roughly 60 FPS with dips and some lower-rate distant animations. First stabilize 60; do not assume a 30-to-60 conversion is needed.
- Usable temporal motion vectors have not yet been established. FXAA strings are present, but that does not prove a temporal motion buffer exists.
- The exact RomFS shader payloads and UI container formats are still unknown and must be inventoried.

## Chosen architecture

```text
User NSP/XCI
  -> verified extraction and version pinning
  -> main NSO function discovery and static AArch64 lifting
  -> generated native code compiled with Clang or Cranelift
  -> custom high-level Horizon/SDK runtime
  -> NVN -> Vulkan 1.3
  -> NU/Lumen render-pass hooks
  -> native 4K / DLAA / DLSS and native mod APIs
```

### CPU and runtime

- **Decision changed 2026-11-11** (evidence in `docs/runtime-boundary.md`): lift `sdk` together with `main`, and leave `rtld` unlifted.
  - **Guest libraries:** sdk's musl libc, libc++, libc++abi and LLVM libunwind run as lifted guest code. That keeps guest object layouts, `wchar_t`, `va_list` and the exception tables exactly right. 725 of 731 `main` imports resolve into `sdk`.
  - **HLE boundary:** `nn::` APIs are replaced by symbol across both modules (TotK `HOST_OVERRIDE` pattern), plus host overrides for hot libc leaf functions.
  - **SVCs:** sdk's 61 SVCs (one stub table at `sdk+0x399c18`) become a logging backstop that stops on any call.
  - **No IPC servers:** no IPC servers and no guest NVN driver.
  - **Exceptions:** the guest unwinder does the search and cleanup. Its register-restore jump (`sdk+0x513794`) becomes a host hook that sets a resume flag. Lifted call sites check the flag and `goto` landing pads seeded from the LSDA (20,191 pads in `main`). `setjmp`/`longjmp` reuse the same mechanism.
  - **Initializers:** sdk's 111 initializers must run in rtld's order.
  - **Cost:** about 52% more generated C, so cache the sdk build (sdk is identical in every revision).
- Generated code from `sdk` follows the same rule as `main`: it stays in ignored `generated/` and `build/` and is never distributed.
- Do not link an emulator. An emulator or low-level recompiler may be used only as an external correctness/performance reference.
- Conservative CPU-backend baseline: LAS/TotK-style AArch64-to-C generator.
- Evaluate Kirikae's permissively licensed Rust/Cranelift backend against that baseline during the initial function-map prototype. Choose from measured integration cost, correctness, build time, and runtime performance; do not maintain two production lifters.
- Preserve guest pointers as host pointers where practical, avoiding per-access address translation.
- Recover function boundaries from `.eh_frame`/unwind data, sized symbols, relocations, direct calls, and address-taken targets, cross-checked with Ghidra. Unresolved indirect targets remain explicit.
- Implement Horizon thread priorities, three virtual application cores, affinity masks, synchronization, and fibers early. Free host scheduling is unsafe for code that assumes per-core state.
- Add backward-branch/yield polling for spin loops and priority inheritance where required.
- Implement libc/C++ ABI, files, saves, input, audio, JPEG, and offline services incrementally. Network features come after the offline game is correct.

### Graphics and performance

- Intercept NVN directly and translate it to Vulkan; do not run the guest NVN driver in the production renderer.
- Use Kirikae's NVN shadow/check concept to compare host draw state against reference Maxwell command streams during bring-up.
- Inventory RomFS shaders. Prefer shipped `NV_gpu_program5` assembly if present; otherwise use a validated Maxwell-to-SPIR-V translator.
- Precompile known shaders and pipelines, persist caches, and compile unknown variants in the background.
- Cache geometry and textures, avoid unnecessary readbacks, and use asynchronous submission/replay only where correctness is proven.
- On discrete GPUs, prefer mirrored device-local pools with dirty-page tracking over repeatedly reading imported guest memory across PCIe.
- Intercept the NU render-pass graph and replace expensive passes incrementally. Every replacement needs a switchable original path and screenshot comparison.
- Use AVX2/FMA for the default x86-64-v3 build, with a documented lower-ISA fallback if needed. Apply PGO only after correctness; TotK's reported 6-12% improvement is a reference, not a promise for Snap.
- Measure simulation, render, GPU, and present times separately, including 1% lows and per-pass costs. The budgets are 16.67 ms at 60 FPS and 8.33 ms at 120 FPS.

### 4K, DLAA, and DLSS

- Native 4K: scene and output are both 3840x2160.
- DLAA: native 3840x2160 scene with temporal anti-aliasing; this is the default quality mode.
- DLSS Quality: 2560x1440 scene reconstructed to 3840x2160.
- DLSS Balanced: approximately 2227x1253 scene reconstructed to 3840x2160.
- DLSS Performance: 1920x1080 scene reconstructed to 3840x2160.
- Compose UI at native output resolution after temporal processing. Do not upscale UI with the 3D scene.
- Supply color, depth, exposure, jitter, mip bias, and correct per-pixel motion vectors. Track previous/current camera, object, and skeletal transforms.
- Handle reactive/transparency content, particles, Illumina effects, foliage, and newly appearing objects explicitly.
- Ensure every full-size intermediate target and depth buffer uses the correct resolution; avoid accidental downscale-then-upscale paths.
- Long-term motion source: NU-level object/camera/skinning history.
- Interim/reference option: independently implement transform-feedback capture and replay of current/previous post-transform geometry against scene depth, as demonstrated by ScarletNX. Do not copy its GPL implementation and do not assume geometry replay is the fastest final route.
- Provide a native-resolution, non-temporal reference render for saved photos when needed. Verify that photo scoring never accidentally consumes DLSS/DLAA output.
- Frame generation is optional later and never counts toward the real 60/120+ FPS requirement.

### Visual enhancements (owner, 2026-11-11; after correct 60 FPS)

- **Draw distance:** further draw distance through LOD distance and bias, culling and far-plane settings, and the distant-animation rate. Expose each as a setting with the original value as default.
- **Lighting:** shadow-map resolution and cascade distance, SSAO quality, and light/probe precision. Every change needs a switchable original path and photo-scoring checks.
- **Textures:** anisotropic filtering, mip bias for native 4K, and texture-pack replacement through the mod loader (higher-resolution textures).
- **Knob findings (2026-11-11, static; `analysis/enhancements/knobs.md`):**
  - **Sampler choke point:** `0x9216d0` (cache `0x3206d0`). Anisotropy is 1 today and LOD bias about 0, so anisotropic filtering is the cheapest visual win.
  - **Render targets:** all fixed-size: scene and SSAO 1920x1080 in `0x5bd940`, the 960x540 mip chain, bloom 192x108 in `0x5c23a0`. There is no dynamic resolution.
  - **Docked and handheld:** both modes render the scene at 1080p. Handheld presents at 720p, and `[ctx+0xcec]`=1 selects handheld pass variants at about 50 sites. The host reports docked.
  - **Shadows:** the dynamic shadow is one 1024² map (`0x5bdd20`), and the static shadow is 3x1024² (`0x4d12a8`).
  - **Draw distance:** per-course fog and atmosphere distances in the ENV levelb data are the best pop-in mask. There are 2,794 meshes with LOD1 and LOD2 variants.
  - **Not found statically:** the camera far plane, LOD switch distances, distant-animation rate and SSAO samples. Each needs a dynamic trace.
  - **Native 4K:** every fixed-size target must be scaled together.
  - **Scoring safety:** keep the Evaluation depth target (`0x5c3d38`, width 1920) and the photo passes at their original size.
- **Photo scoring:** scoring must not change. Draw-distance and LOD changes need verification that photo evaluation, visibility tests and spawns behave as on the original path.

### High frame rates

- Stabilize original 60 FPS behavior first, then test true 120/144/240 simulation and rendering.
- If a subsystem must remain at 60 Hz, use explicit render interpolation while reporting its simulation rate separately.
- Input sampling and camera control should run at presentation rate.
- Mod scripts receive delta time in seconds and must not depend on frame counts.
- Lower-rate distant Pokémon animation is a separate optional LOD improvement, not something FPS unlocking fixes automatically.

## Modding plan

### Content loader and packages

- Use layered overlays with per-pack enable state, priority, dependencies, target-version declarations, per-pack settings, and conflict reporting.
- Support existing LayeredFS-style replacement layouts where practical.
- Keep original game data out of mod packages. Validate assets and fail with actionable messages.
- Support hot reload for textures, materials, text, and shaders where safe. Models/courses may initially require a course reload.
- Version modded saves, record pack versions, preserve unknown IDs, and provide profiles and automatic backups.

### Asset tooling

- `kwsch/NewSnap` is ISC-licensed and useful for DRP archives, Oodle entries, saves, and MSBT-related handling. Its changer replaces existing entries; it is not a complete new-content system. Do not source the proprietary Oodle DLL from unofficial downloads.
- Existing NUSRCMDLB MaxScript is an importer, not a writer. Build a model exporter with skeleton, material, mesh-group, skin-weight, and LOD validation.
- Build NUTEXB import/export with mip generation, block-linear layout, ASTC/BC formats, and GPU-readback verification.
- Extend DRP handling to add new entries and validate indexes/checksums.
- Add Wwise event/bank mapping and UI format editing only after the exact RomFS structures are confirmed.
- Every writer needs decode -> edit -> encode -> in-game reload validation.

### New Pokémon and courses

- Start by cloning an existing species/course into a new ID and prove the complete path before attempting fully original registries.
- A species pack may require species/form IDs, names, models, animations, sounds, spawns, behaviors, photo scoring, research entries, thumbnails, and save identity.
- A course pack may require geometry, lighting, collision, camera-rail data, spawns, scripts, registration, unlock rules, localization, map/loading assets, scoring, and save handling.
- Decompile the corresponding registries and subsystems rather than assuming filenames alone define content.

### Mod API

- Expose build-ID-guarded semantic hooks, not public raw-address APIs.
- Provide versioned native C ABI events for course load, spawn, photo capture, scoring, save, and UI creation.
- Use a separate host Lua VM for data-driven mod scripts; never execute untrusted mod scripts in the game's own Lua state.
- Native DLL mods require explicit user opt-in because they have full process privileges.

## Settings and accessibility UX

- Use a game-themed layout inspired by the reviewed tabbed settings screenshot: top categories, options on the left, contextual explanations on the right, version/FPS at the edges, and input hints at the bottom.
- Derive typography, icons, shapes, and palette from Snap's own lab/NEO-ONE interface assets loaded locally. Do not copy Hookshot's theme or ship game assets.
- Default categories: Graphics, Audio, Controls, Gameplay, Mods, Accessibility, System. Keep Debug behind an advanced toggle.
- Graphics options include output/render resolution, upscaler, DLSS model, frame cap, pacing, Reflex, HDR, and renderer diagnostics where appropriate.
- Mods UI includes enabled packs, content browser, load order, profiles, dependencies, conflicts, pack settings, hot-reload/restart status, and safe removal.
- Explain every option in plain language and mark immediate versus restart-required changes. Unsupported options must explain themselves rather than silently fail.
- Distinguish simulation FPS, rendered FPS, and generated FPS.
- Include independent UI/text scaling, high contrast, redundant non-color cues, full remapping, toggle/hold options, reduced motion, independent audio categories, subtitle customization, and clear error/apply feedback.

## Validation strategy

- Differential-test AArch64 instructions, SIMD/FP, atomics, jump tables, and whole functions against Unicorn, QEMU, or real ARM64 as appropriate.
- Record interpreted/unresolved targets and feed them into later AOT builds.
- Track import/SVC/IPC/ioctl/shader coverage and fail loudly on unknown behavior during development.
- Build a complete shader census with translation and SPIR-V validation.
- Provide SwitchRECOMP-style frame traces, per-draw inputs/results, watchdogs, and offline draw replay.
- Compare screenshots and render targets against the original path or an external reference.
- Create scripted course replays covering movement, Pokémon behavior, photo capture, scoring, album/editing, saving, and UI.
- Benchmark held-out scenes after correctness, including shader-cold and shader-warm runs. Do not present other projects' performance claims as Snap benchmarks.

## Licensing and reference boundaries

- Preferred implementation sources: LAS Recomp (MIT), TotK Recomp (MIT), YW4 Recomp (MIT), Kirikae (MIT/Apache-2.0), MacNativeGames/nxnative (MIT), NewSnap (ISC), and the older Apache-2.0 SwitchRecomp code preserved in Metroid Dread Recomp.
- Reference only unless licensing changes: SMO Recomp and OpenSmash (GPL), ScarletNX (GPL), current SwitchRECOMP (AGPL with startup branding), and its GPL Vulkan renderer.
- `bgyss/SwitchRecomp` is a draft spec/Rust scaffold, not a working alternative engine. GitHub reports no license; do not reuse its code without clarification.
- Useful architecture references:
  - TotK: https://github.com/valentderah/totk-pc-recomp/blob/main/docs/architecture.md
  - YW4 renderer: https://github.com/ILFforever/YW4Recomp/blob/master/docs/decisions/0023-custom-renderer-plan.md
  - YW4 DLSS: https://github.com/ILFforever/YW4Recomp/blob/master/docs/decisions/0040-dlss-upscaling.md
  - SMO validation: https://github.com/Coz-0/smo-recomp/blob/main/docs/architecture.md
  - Kirikae: https://github.com/Cor3Duo/kirikae
  - ScarletNX: https://github.com/scarletnx/ScarletNX/blob/main/docs/dev/architecture.md
  - SwitchRECOMP diagnostics: https://github.com/SwitchRECOMP-Dev/SwitchRECOMP/blob/main/DEVELOPER_GUIDE.md
  - NewSnap: https://github.com/kwsch/NewSnap

## Next steps

1. Done 2026-11-11: revision audit, target lock, validated import path, deterministic maps (see Current verified state).
2. Done 2026-11-11: authoritative function list, classes, static render-graph trace, CPU backend evaluation, and the patched RomFS census.
3. DRP unpack and first format pass done. Next: decode the SSBH shader layout into a full shader census (stages, resource bindings, SASS sizes). Recover the FlatBuffers registry schemas from their reader functions in `main`.
4. Try the validated ELF in REA, and compare its function set against `analysis/maps/functions_final.tsv`.
5. Prototype the selected CPU backend and differential tests before building game-dependent runtime code.
6. Follow the playable milestone order: startup -> title -> complete course with photo scoring -> stable 60 -> native 4K/DLAA -> DLSS -> real 120+ FPS and advanced content mods.

## Repository hygiene

- **Git:** the repo is private on GitHub at `finalpantasy/new-pokemon-snap-recomp`, created 2026-11-11 with code only.
  - The local git identity is `finalpantasy` with its noreply email. Never use the global identity.
  - Only the coordinator commits and pushes. Before every push, check that `git grep -i` finds no owner name or email and that `git ls-files` contains no game formats or images.
  - Push using the finalpantasy token (`gh auth token -u finalpantasy`) through a per-command credential helper. The active gh account is a different one.
  - Making the repo public is the owner's decision.

- `work/`, `extracted/`, `analysis/`, and `tools/` are generated/local and ignored.
- Never commit `.nsp`, `.xci`, `.nca`, `.nso`, `.keys`, extracted RomFS, generated C from the game, NVIDIA binaries, or proprietary SDK files.
- Keep research/source code separate from game-derived artifacts and record tool versions and input hashes for reproducibility.
