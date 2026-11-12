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
  - **Characters and validation fixed (2026-11-11; `docs/nvn-vulkan-design.md`, "Fixes: validation cleanup and invisible characters"):**
    - **Invisible characters:** skinning is a compute shader whose real workgroup size (256x1x1) is in the nushdb control section at `+0x748`. The translator always emitted 1x1x1, so the size is now patched into the SPIR-V. SSBO-written ranges are marked GPU-written, so CPU copies don't overwrite them.
    - **Render targets:** attachments now use per-subresource views and layouts.
    - **Other fixes:** typed view cache, `hostQueryReset`, and dual-source blend output patching.
    - **Result:** a 125 s boot shows 0 validation errors and 0 skipped draws (`analysis/boot/characters_visible.png`).
    - **Test scripts:** the Python texture-compare scripts need `python -E -P` (user-site numpy).
  - **Live play (2026-11-11):** `host\play.ps1` runs a copy of the exe from `build\play\` under `build\play.lock`.
    - **Switches:** `-SkipIntro` (scripted input to the camera tutorial, about 4 min) and `-ListPads`.
    - **Test runs:** `run_boot.ps1` refuses to start while `play.lock` is live; runs use `build\runs\slot<N>\save`, an exe copy and a shader-cache copy per slot.
    - **Lock protocol (owner exception, 2026-11-12, `host\locks.ps1`, details in `host\README.md`):** builds hold `build\heavy.lock`; `run_boot.ps1` takes one of 4 slots `build\slots\run0..3.lock` (owner 2026-11-12: raised from 3 to 6, then cut to 4 because 6 runs held the CPU at 100%) itself (do not hold heavy.lock for runs); `-Bench`/`-Exclusive`/perf take all 4.
    - **Locks are released only by their holder** (2026-11-12): three workers deleted another worker's `heavy.lock` (`rm`, cleanup commands). Release through `host\locks.ps1` with your own owner tag; never delete a lock file you did not create, even if it looks stale; report it instead.
    - **Muted test runs (owner, 2026-11-12):** hidden runs default to the silent null audio backend (`SNAP_AUDIO_BACKEND=null`); capture still works. Use `wasapi` only when a test needs the device, with `SNAP_VOL_MASTER=0` where possible.
    - **Parallel worker builds (2026-11-12):** a relink into your own folder needs no lock: `tools\venv-recomp\Scripts\python -I host\build_snap.py --out build\w\<owner>`, then `run_boot.ps1 -BuildDir build\w\<owner>` (or `$env:SNAP_BUILD_DIR`). `build\heavy.lock` is only for writing shared outputs: `build\snap`, `recomp\snap_build.py` (the main/sdk libs, needed after runtime header or lifter edits) and regenerating main.
    - **Editing shared sources (2026-11-12):** edit C/C++ with the Edit tool, never shell heredocs or inline Python: they turned `\n` inside string literals into real newlines and broke every build four times (`mods.cpp`, `nvn_vk.cpp`, `nvn_hdr.cpp`, `nvn_gfx.cpp`). Relink your own build right after each edit. Do not use `run_boot.ps1 -NoLock`: it skips the run-slot limit, and the PC sat at 100% CPU with 6 runs. Each run's save is now copied to `build\runs\saves\<run id>` automatically (only the current generation, newest 20 runs kept: the full copies filled F: with 721 GB on 2026-11-12). **Keep the drive clean (owner 2026-11-12):** `run_boot.ps1` also deletes `analysis\boot` screenshots older than 24 h unless a doc names them, and `*_raw` dump folders older than 24 h; copy a shot you need into `analysis\mods\compare\` or cite it. Delete your own `build\w\<owner>` build and scratch files when your task is done. Test runs now load no mods by default (`SNAP_MODS_DIR` = an empty per-slot folder): the owner's HD packs in `%APPDATA%\SnapRecomp\mods` made hidden-run loading about 15x slower (perf worker, loader threads on one lock). To test a pack, set `SNAP_MODS_DIR` to its folder.
    - **Priority and routes:** while `build\heavy.priority` exists slot 0 is for `$env:SNAP_LOCK_OWNER` = its content. Route scripts accept `@f<frame>` times (present count) so load does not shift them.
    - **Input:** the game reads npad 0 in Pro controller style only, never six-axis (`SNAP_HLE_TRACE=hid`, 3 course runs, 0 six-axis calls). Every SDL3 gamepad plus XInput slots 0-3 are merged, with a rescan every 1 s.
    - **Rumble and gyro (2026-11-12, input worker; `runtime/hle/hid.cpp`, `input_core.cpp`, `input_backends.cpp`, `docs/hle-services.md`, switches in `host/README.md`):**
      - **Rumble:** the game imports `GetVibrationDeviceHandles`, `InitializeVibrationDevice`, `SendVibrationValue` (all three overridden; no VibrationPlayer or Node imports). Handles are asked once at about 16 s (main+0x159074); `SendVibrationValue` is called from the vibration class (vtable `0xc6c820`; ctor `0x158f50`, init `0x158fd0`, stop-all `0x159310`, per-frame update `0x1594f0` -> play `0x159860`). Values are amplitudes from clip data, mapped to the low and high motors, then `SDL_RumbleGamepad` on every rumble pad, XInput as fallback. `SNAP_RUMBLE=0..1.5` (0 = off, beats the profile) and `SNAP_RUMBLE_LOG=1` (`rumble:` lines with present count and held buttons). Verified in run `5-20261112_113016` (`host\routes\rumble_probe.txt`, 29,190 presents): host chain works (a scripted `rumble=1,0.5` token gives `SDL_RumbleGamepad lo=65535 hi=32767 accepted=1`, then `0,1`, then stop). **The game itself sent only all-zero stop values** (main+0x159354, at the end of the run and at menu changes), never a non-zero value, through shutter (A, 72 shots), X, Y, B, ZR, R, L, ZL and a right-stick sweep in Florio. So which game action vibrates is NOT established (possible causes: a vibration option in the save, or events I did not trigger such as scan or fluffruit hits; the static trace did not show the gate). Hidden runs rumble the owner's real pads if non-zero values come; amplitudes are 0 today.
      - **Gyro:** the game never reads six-axis, so gyro is a host feature: SDL3 gyro (pad with `gyro=yes`; the owner's pads report no gyro) feeds the right stick (`gyro_to_stick`, 90 deg/s = full, 1.5 deg/s dead zone), also added to the six-axis state if the game ever asks (UNTESTED against the game, never called). `SNAP_GYRO=0|1` (beats the profile), `SNAP_GYRO_SENS=<n>`, `SNAP_GYRO_FAKE=<yaw>,<pitch>` (pretend gyro; yaw positive = left, pitch positive = up) and the script token `gyro=<yaw>,<pitch>` (needs `SNAP_GYRO=1`). Verified (route `host\routes\gyro_probe.txt`, runs `2-20261112_123938` SNAP_GYRO=1 and control `2-20261112_124306` SNAP_GYRO=0, dumps at presents 4495/4560/4650/4780, opened): with gyro on the trace shows `rx=-32767` and the view pans between 4495 and 4560 (frame changes 54 vs 26 mean abs levels; the Florio ride moves on its own rail, so the control also changes, and the two runs are not the same position). The real SDL gyro path is untested (no gyro pad).
      - **Mouse:** mouse look, sensitivity, invert and smoothing already existed (Controls > Mouse, `mouse.*` in `input.json`); nothing added.
      - **Test routes:** `host\routes\rumble_probe.txt`, `host\routes\gyro_probe.txt`; script tokens `rumble=<lo>,<hi>` and `gyro=<yaw>,<pitch>` in `input_core.cpp`.
    - **Boot crashes fixed:** the host supplies the `ProfanityFilter` vtable, and the software keyboard returns "Ash" (`SNAP_PLAYER_NAME`).
    - **Save files (2026-11-11):** `scripts\snap_save.py` can dump and patch saves (`dump`, `unlock`, `patch`). Each save file is a 0x30 header plus entries. Each entry is AES-128-CBC with an xorshift IV, plus adler32 and crc32 checksums. The key is read at run time from a local clone of NewSnap (ISC) in `tools\ref\NewSnap` and is never committed. Re-encoding an unedited file gives byte-identical output.
      - **Unlock fields (static RE, details in `analysis\save\fields.md`):**
        - Course status is file 11, byte `route-1`: 0 locked, 1 unlocked, 2 entered. High confidence on the read side; the code that sets it was not found.
        - Course level comes from research points: the file 9 route record at `route*0x1f8`, entries at +8 (12 bytes each, four u16 scores). The level 1-4 thresholds come from `ResearchPoint.mdrp`. High confidence.
        - Entry ids and the +4 field are low confidence. The points entry uses id 1, which may break the research log UI.
      - **Fixtures:**
        - `build\fixtures\saves\unlock_all`: all courses unlocked, level 4. Checksums verified.
        - `unlock_routes`: unlocks only, no points. Use it if the research log crashes on `unlock_all`.
        - Rebuilt 2026-11-12 from `post_tutorial` (stage 2); the old new-game versions are `unlock_all_v1_newgame` / `unlock_routes_v1_newgame`. Research entry id 1 is still a guess and the fixtures are NOT yet verified in game (`host/routes/unlock_all_check.txt`; the PC was too loaded to reach the map in 400-590 s). The Florio scoring route produced no research entries yet; see the 2026-11-12 diff in `analysis/save/fields.md`.
        - `post_tutorial` (2026-11-12, verified in game): a game-written save past the intro, yard camera tutorial, evaluation tutorial and scan tutorial; title, then A, then the lab hub and the Lental Map with Florio (`host\routes\post_tutorial_check.txt` shows the map by 62 s; with no input the run stops at the title). The key is story stage byte `9:0:0x3b50` (1 new game, 3 after yard, 4 after first evaluation, 2 normal); details in `analysis\save\fields.md`. The old fresh save is now fixture `new_game`; `post_yard` is the save right after the yard tutorial, and `host\routes\tut_full.txt` passes the yard tutorial from `new_game` (2 of 2 runs).
      - **Next step:** diff a save from before and after a scored course run, once photo capture works.
      - **Photos (2026-11-11):** photos live in the save, not a system album: file 7 = 1200 JPEG entries, file 10 = thumbnails, file 6 = 0x148 metadata (limit 1200, `SNAP_ALBUM_LIMIT` not implemented, plan in `analysis\save\album.md`); `tools-src\gallery` extracts and serves them.
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
  - **Volume (2026-11-11, done):** per-category volume goes through the game's own options path.
    - The setter `main+0x67c610(mgr, cat, level 0..10)` posts the `Set_volume_{bgm,se,voice}_NN` events. The host API `snap_audio_set_volume` (master, bgm, se, voice) calls it, and 1.0 posts nothing.
    - Settings persist in `audio.json`, and `SNAP_VOL_*` env vars override them. See `docs\audio.md`.
    - Verified: setting all three to 0 gives silence. The persistence round-trip is untested.
  - **Surround and spatial (2026-11-12, `docs/audio.md` "Surround and spatial output"):** `SNAP_AUDIO_OUTPUT=stereo|speakers|spatial` (also `audio.json` `output`, `channels`, `speakers`) selects 5.1 or 7.1 WASAPI or a Windows spatial 6-object bed (falls back to speakers, then stereo). The game hard-codes stereo (`AkOutputSettings` channel config `0x3102`, built in `main+0x67ffa0`); `host/abi_audio.cpp` rewrites it to 5.1 (`0x60f106`) at the `ListAudioDeviceName` call and again at the sink Open (`main+0x124780`), and the sink then opens 6 channels. Wwise's mixer bus in `Init.bnk` (id 508372457) is fixed stereo; `scripts/audio_make_surround_pack.py` writes a local `surround-bus` mod that frees it, and a host `Upmixer` fills FC, LFE and surrounds from the stereo content (lab device mix RMS FC 0.014, LFE 0.009, BL/BR 0.008, vs 0.0000 before). Course check (Florio ride, `build/audio_caps/florio/`): the game's own mix does carry surround ambience in the course (BL/BR RMS 0.003 to 0.010) and the upmixer now gates center/LFE and surrounds separately (`audio_mix.h`; before, any BL/BR energy closed the whole fill and FC/LFE were 0.0000 in the ride). Florio capture after the fix (`build/audio_caps/florio2/`): ride FC 0.009-0.019, LFE 0.003-0.008, BL/BR 0.002-0.009 (unit check in `tests/audio/mix_test.cpp` passes). `spatial` opens (6 static bed objects, no fallback). Details in `docs/audio.md`.
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
- **Photo evaluation path (2026-11-12, photo worker; details in `docs/nvn-vulkan-design.md`, "Photo capture"):**
  - **Mechanism (static, from the shipped GLSL):** the score's visibility input is `EvaluationAggregatePS`, a fragment shader with `early_fragment_tests` that does `atomicAdd` into an SSBO per subject pixel that survives the depth/stencil test. The CPU reads that buffer.
  - **Three layer bugs fixed:**
    - SSBOs were bound by translator ordinal instead of NVN index, so the aggregate shader (binding 1) wrote into the dummy buffer. Now mapped from the c0 descriptor offset (`SsboIndex`).
    - The early-fragment-tests flag (control+0x718 bit 0) was dropped, so hidden pixels would have counted too. Now patched into the SPIR-V.
    - Fragment-shader SSBO writes into CPU pools never reached guest memory. Now copied back at the sync/fence (readback kind 3; `NVN_NO_SHADER_WB=1` disables it).
  - **When it runs:** at the start of photo select, not at the shutter (about 13 frames for 72 photos). Each subject gets a 0x40-byte SSBO: 4 counters each for stencil 1 (subject hidden), 2 (subject visible), 3 (sphere hidden), 4 (sphere visible). The CPU reads it in guest `main+0x418350` about 3 frames later and needs visible-subject pixels (stencil 2) above a threshold.
  - **Two more fixes (2026-11-12, verified in runs `2-20261112_082323` and `1-20261112_084106`, Florio):**
    - Black thumbnails: the game's GPU BC1 encoder renders into a BC1 texture, which Vulkan cannot render to. It now draws into an uncompressed stand-in (one texel per block) that is copied into the BC1 image (`RedirectCompressedTargets`; `NVN_NO_BC_RT=1` disables).
    - Every photo "not clearly visible": the startup pipeline precompile could load the Aggregate shader before the game rebuilt its `.efz` marker (test runs mirror `build\snap\nvn_cache`, which has none), so the stencil test ran after the atomics and every counter read 518400. BuildStage now tells the pipeline service directly; a template loaded too early is patched and its pipelines rebuilt (`nvn_pipeline.cpp` `PatchTmplEft`; `NVN_NO_EFT_FIX=1` disables).
  - **Result:** photo select sorts photos by subject (Wurmple, Vivillon, Dodrio, Bidoof, Emolga), thumbnails show, a Vivillon photo can be picked, and the professor scores it (Total 1,780; route `host\routes\florio_eval.txt`). Wurmple photos stay unselectable, and the shots show no Wurmple in them. Debug: `NVN_EVAL_LOG=1` (evaluation draws and clears), `NVN_XFER_LOG=1` (`wb values` lines), `NVN_WB_WATCH=1` (guard-page watch of the CPU reader).
  - **Render scale (2026-11-12):** at `SNAP_RENDER_SCALE=2` visibility dropped (median visible fraction 0.27 vs 0.65-0.94) because CopyDepth sampled the scaled scene depth 1/4 pixel off. A 2x2-minimum CopyDepth variant fixes it (`NVN_NO_EVAL_DEPTH_FIX=1` disables): verified at scale 2 in runs `4-20261112_113015` (median 0.99) and `2-20261112_120107` (median 0.57, Vivillon scored Total 1,798). Quarter-photo preview at scale 2 fixed: the 360x240 thumbnail and 1080x720 Capture3D targets are exempt from scaling (`NVN_NO_THUMB_EXEMPT=1` disables); verified at the AA default (ssaa2, 1920x1080 window = scale 2) in run `5-20261112_122638`: preview, thumbnails and pop-up whole, Dodrio scored 2,359, exported PNG 1080x720 full picture. Table in `docs/nvn-vulkan-design.md` "Photo capture".
- **Shader path (2026-11-11; `analysis/shaders/`, `scripts/nushdb_parse.py`, `scripts/shader_harness/`):**
  - **Format:** nushdb = SSBH SHDR v1.2 (matches `ssbh_lib`, MIT). The GLSLC inner binary decodes fully (sections, SPH, SASS, reflection); some control fields and reflection tables 5-6 remain undecoded.
  - **Census:** 6,227 programs, 1,639 unique (VS 291, PS 1,343, GS 2 with transform feedback, CS 3). Only 263 `resource.drp` programs carry GLSL or NV_gpu_program5 references.
  - **Maxwell checks and translator r2 (2026-11-12, Astral Chain recomp review, MIT, notice in THIRD_PARTY_NOTICES.md):** runtime now enables provoking-vertex LAST, pins 32-lane subgroups for the 2 programs that use subgroup ops, and uses VK_EXT_custom_border_color (switches `SNAP_VK_PROVOKING/BORDER/SGPIN=0`). FMZ census: 0 of ~357k instructions, no fix. Translator patches 0005+0006+0007 are `SNAP_XLATE_REV=2` (separate caches `nvn_cache_r2`/`shadercache_r2`, 294 of 1,639 SPIR-V changed, 1,639/1,639 spirv-val); 0001-0004, 0008, 0009 skipped with reasons. Details and rebuild steps: `docs/nvn-vulkan-design.md` section Maxwell checks. Default (unset) behavior is unchanged.
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
  - **Function tests:** 390 of 434 functions tested, 0 failures, at both -O1 and -O2 (2026-11-12).
    - **-O2 FP compare fix:** at -O2, LLVM folded an FP compare against 0.0 into an integer test that ignores DAZ, so 0x5adcc0 failed. `runtime/simd_fp.h` `FN(cmp)`/`FN(vcmp)` now pass their operands through an opaque `__asm__` copy.
    - **Harness default:** `tests/snap_func.py` defaults to -O1, which hid this bug. Test with `--cflags "-march=x86-64-v3 -O2"`, the optimization level the real build uses.
    - **Rebuild after editing runtime headers:** run `recomp/snap_build.py` first; `host/build_snap.py` only relinks.
  - **Host spike:** lifted `0x910270` runs natively in `host/spike.cpp`. It makes 1,033 NVN proc requests (517 names), exactly matching the static map.
- **Language decision (owner, 2026-11-11):**
  - **Lifted game code:** generated C compiled with clang.
  - **Core host runtime:** C++, reusing TotK/LAS runtime patterns. Horizon HLE, NVN->Vulkan and DLSS/Streamline belong here.
  - **Mod loader and untrusted asset/mod-package parsers:** Rust, behind a C ABI.
  - **Priority rule:** a fully playable, correct game comes first. If Rust would slow any step on the path to playable, use C++ there and revisit later.
- **Rust work paused (owner, 2026-11-12):** "stop the rust work until we have the rest of the game and mods and stuff working". No Rust sessions or workers until the game, photo scoring and the mods work; C++ stays the path for the mod loader meanwhile. Handoff: `docs/rust-paused.md`.
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
- **Upscalers implemented (2026-11-12, `runtime/nvn/upscale/`, small `[upscale]` edits in `nvn_vk.cpp`, `nvn_draw.cpp`, `nvn_entry.cpp`, `nvn_gfx.cpp`, `nvn_internal.h`; Astral Chain adaptation noted in `THIRD_PARTY_NOTICES.md`):**
  - **Switches:** `SNAP_UPSCALER=off|dlss|dlaa|fsr|xess`, `SNAP_UPSCALE_QUALITY=native|quality|balanced|performance` (ratio 1, 1.5, 1.724, 2). Output = window (`SNAP_WINDOW_W/H`), scene = output / ratio; mip bias auto (`-log2(output/scene)`); the game's FXAA path and the default `ssaa2+smaa` are off while an upscaler runs (unless `SNAP_AA` is set). Fallback: the chosen backend, then DLSS, FSR, XeSS, then plain render scale, each with an `[upscale]` log line; a backend that fails mid-run hands over to the next.
  - **Backends:** DLSS/DLAA through the driver's NGX core (`_nvngx.dll` from the DriverStore, `nvngx_dlss.dll` 310.9.16 from `tools\nvidia\`, no Streamline); FSR 3.1.4 through the FidelityFX API (`amd_fidelityfx_vk.dll`, headers from `tools\amd\` at build time; its backend calls some device functions without a null check, so `upscale_fsr.cpp` maps KHR aliases and debug labels); XeSS 2.0.2 (`libxess.dll`, DP4a on the RTX 4090). `host/build_snap.py` copies the three DLLs next to the exe when present.
  - **Inputs:** scene colour (RGBA16F, before the tonemap and UI), scene depth (DEPTH32F), camera-only motion vectors (`upscale_mv.comp`: previous viewProj x inverse current viewProj from `PerViewCBuffer` c3 +0x100, jitter removed), Halton(2,3) jitter through the viewport of scene-depth draws only, reset on camera cuts (eye > 5 units or > 30 deg) and after > 4 presents without an upscale. The projection is reversed infinite Z (depth 1 = near 0.01), so all three get their inverted/infinite flags. The output (output-size RGBA16F) replaces the scene colour only where the game's tonemap draws into the display texture, so tonemap and UI run at output size.
  - **Photo safety:** a submit that draws into the Evaluation depth target or a 360x240 photo target renders that frame unjittered and not upscaled (hold 2 presents). The photo pass often runs at the start of a present and downscales the PREVIOUS present, so a press of A, ZR or R also holds the next 6 presents (Florio: tap at f6000+24k, photo pass at p6003+24k). Log line `photo target bound ... (display holds the previous present, which was held: native, unjittered)`: 72 of 72 photo-pass binds were native in each run after the fix (before it, 36 of 72 read an upscaled present).
  - **Verified on the RTX 4090 at 3840x2160 (Florio course, `florio_photos`):** DLAA (run `4-20261112_120428`), DLSS Quality (`1-20261112_121603`), FSR Quality (`0-20261112_122917`), XeSS Quality (`4-20261112_123948`) all upscale through the course with no crash. Same-view crops: `analysis/mods/compare/up_native.png` and `up_dlaa.png` (consecutive presents 7528/7527 with `SNAP_UPSCALE_AB=8`, run `2-20261112_132434`: native shows stair-stepped Taillow tails, DLAA smooth edges); `up_dlss_q.png`, `up_fsr_q.png`, `up_xess_q.png` (consecutive presents with `SNAP_UPSCALE_COMPARE=1`, run `3-20261112_125027`; in one frame DLSS has the smoothest edges, FSR is slightly sharper with harder edges, XeSS shows some stair steps on thin leaves). Ghosting: none seen on a moving Comfey and on a Vivillon wing leaving the frame at DLAA (`up_dlaa_ghost_*_p8508_p8509.png`, two consecutive presents) or on Taillow. Scoring with DLSS Quality on (`florio_eval`, run `3-20261112_133447`, `NVN_EVAL_LOG`/`NVN_XFER_LOG`): the professor scores a Dodrio photo, Total 2,874 (`analysis/boot/route_florio_eval_3-20261112_133447_37.png`); Evaluation visibility over 187 subjects has median visible fraction 0.56 (3 fully visible, 33 not visible), and an earlier DLSS-Q eval run had 504 subjects at median 0.85, inside the no-upscaler range of 0.57 to 0.99 (runs differ in which shots they take, so this is a distribution check, not a same-photo check). Validation (`NVN_VK_VALIDATION=1`, `VK_LAYER_PATH=tools\vulkan_sdk\Bin`): DLSS-Q eval run has only the known startup BC1 pipeline error; with all three backends per frame (`SNAP_UPSCALE_COMPARE=1`) FSR gave `VUID-vkCmdDispatch-imageLayout-00344` (it samples depth as SHADER_READ_ONLY), fixed by a layout switch around its dispatch in `upscale_fsr.cpp` (rerun `0-20261112_135714`, all three backends every frame through the course: only the startup BC1 error remains); two `VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933` come from the window code, not the upscaler. CPU cost of the upscale point 0.23 to 0.55 ms per frame; presents per second in the course 37 to 58 with 6 parallel test runs on the PC (not a benchmark; GPU time not measured).
  - **Test aids:** `SNAP_UPSCALE_LOG=1|2`, `SNAP_UPSCALE_JITTER=0`, `SNAP_UPSCALE_MV=0`, `SNAP_UPSCALE_PROBE=<present>` (dumps c3/c4), `SNAP_UPSCALE_NOEVAL=1`, `SNAP_UPSCALE_AB=<P>` (odd blocks of P presents native: same-view native vs upscaled pairs at N = 2kP + P - 1, N + 1), `SNAP_UPSCALE_COMPARE=1` (every backend upscales every frame, the display shows backend `present % n`: DLSS, FSR, XeSS).
  - **Not done:** per-object motion vectors (characters use camera-only vectors; the doc's skinning tier is next), reactive/transparency masks, a settings-menu row (the options page still says "coming soon"), Balanced/Performance modes not run, GPU timings.

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
  - **SSAO quality, pass toggles, color looks (2026-11-11; `host/mods/gamemods.cpp`, `host/mods/look.cpp`, rows 7/16/19/29 of `docs/mod-ideas.md`):** `SNAP_SSAO_QUALITY=high|ultra`, `SNAP_SSAO_SAMPLES`, `SNAP_SSAO_HBAO=dirs,samples` patch the pass objects from `MAIN_INLINE_HOOKS` (`0x5a6c94`, `0x5a7bac`); the sample count is a cbuffer value (SimpleAO `+0x17c`, HBAO `+0x174/+0x178`), kernel buffer holds 512. `SNAP_PASS_DUMP=1` / `SNAP_PASS_OFF=` use the pass loop hook `0x989048`; `SNAP_LOOK=warm:0.6` needs the present hook in `analysis/mods/nvn_hook_requests.md`. Photo score overlay (#19) is documented, not implemented. **Sharper fonts (#29) are implemented and ON by default at 1440p and above (2026-11-12):** `SNAP_UI_FONT_SCALE` unset/`auto` = 2 when the game window's client height is >= 1440 (checked once at the first atlas creation; borderless fullscreen counts as the monitor height), `1`/`off` = off, `2..4` = forced; F1 Display tab > Text > "Menu text sharpness" (`gfx_ui_font`, restart needed). Hooks `0x6410bc/0x641288` double the glyph atlas dims, `0x4358c0/0x435eb0` re-rasterize each glyph at k x scale (SetScale/GetGlyph via `call_guest_nested`, composite buffer carries 1x bearings/advance), `0x435cf0` keeps entry layout w/h at 1x so text size and position are unchanged; no NVN change. Checked with the mod on: title, lab hub, dialog, pause menu, course HUD (4K and 1080p), photo select, professor's evaluation and score, album (`analysis/mods/compare/font_{default,sharp}.png`): same layout, no garbled glyphs, no host crash; present rate over equal frame ranges within noise of default (about 48 fps in the course on this PC either way); atlas 1 MB -> 4 MB (k=2). Not checked: Vulkan validation layer (not installed here: `NVN_VK_VALIDATION=1` logs "layer not found"), the outline/second-GetGlyph path (`0x435eb0`, never hit in those scenes), process memory.
  - **Native 4K:** every fixed-size target must be scaled together.
  - **Scoring safety:** keep the Evaluation depth target (`0x5c3d38`, width 1920) and the photo passes at their original size.
- **LOD, distant animation and FOV tracing (2026-11-12, `host/mods/lodanim.cpp`, off unless env set):** `SNAP_TRACE_LOD=1` (even/odd-frame function-entry profile for the 30 Hz animation signature; `_FN`, `_CALLERS`, `_MEM`, `_FROM`, `_FRAMES`, `_OUT`), `SNAP_TRACE_FOV=1` and `SNAP_FOV` (multiplier below 10, degrees from 10; scales only the `tanf` call sites in `SNAP_FOV_SITES`, none confirmed yet). Hooks: `snap_fn_observe` call in `host/loader.cpp` `fnprof_hit`, `snap_lodanim_install` in `host/hooks.cpp`. Camera perspective builders found statically (fov at `obj+0x128`, aspect `+0x12c`): `0x291d70 0x295170 0x2971c0 0x297540 0x38b710 0x44f480 0x454270 0x56ab70`; Dynamic results (2026-11-12, runs `1-20261112_034423`, `2-20261112_044540`, `2-20261112_050158`/`1-20261112_050216`): **gameplay camera (lab yard, tutorial) perspective site = `main+0x38cdbc`** (builder `0x38b710`; vertical FOV 33 deg, 1 call per frame from the yard onward); other 33-35 deg callers in the yard are `0x296400 0x297cd4 0x4cf904 0x4cf920 0x352a54 0x337c68` and per-object `0x337a70` (~124 calls per frame), cutscenes use other sites (`0x10e458` 83-149 deg). `SNAP_FOV=1.8 SNAP_FOV_SITES=0x38cdbc` is applied (trace line `fov: SNAP_FOV applied ... 16.50 -> 29.70 deg`, 1000+ hits); default stays off; the other 33 deg sites (culling or shadow frustums?) are not scaled, so a wide FOV may cull at the edges; **FOV works through `main+0x296400` (2026-11-12, Florio ride): `SNAP_FOV=1.3 SNAP_FOV_SITES=0x296400` gives a visibly wider view** (builder `0x295170`, the 18 deg half-angle camera the ride is rendered with). Evidence: in-run A/B with the new test aid `SNAP_FOV_AB=<P>` (`lodanim.cpp`, applies the FOV change only while (present count / P) is odd; the pair is frames N and N+1, so same pose; use `SNAP_FOV_AB=1` with `NVN_FRAME_DUMP_AT=N,N+1`; in the dumps the even-labelled N is the wide one): objects shrink to about 0.75 (blue plants 270 -> 190 px, rock tower 145 -> 100 px, tree crown 170 -> 130 px at 1920 wide), edge objects stretch, a cliff appears at the left edge; shots `analysis/mods/compare/fov_florio_{default,wide}.png` (opened and checked, p5201 default and p5200 wide). `0x38cdbc`, `0x2973b0`, `0x337a70/0x337c68/0x352a54` and `0x4cf904/0x4cf920`+`0x10d5d4/0x10eee8/0x10efe4` alone change nothing visible; `0x29249c` and `0x297cd4` are cutscene-side builders (with `0x296400` they bend the portal intro into a black wedge) and are untested alone in the ride. Edge culling: none seen in 3 wide frames (terrain, cliffs and bushes drawn to all four edges, no pop-in or black corners), but the `0x38cdbc` site (gameplay camera object used for visibility/Evaluation) stays at the original angle, so the scoring frustum is narrower than the widened picture: photo framing and Evaluation visibility under `SNAP_FOV` are NOT verified, keep it off by default. Earlier site sets (`0x38cdbc`, `0x10e458`) changed nothing in the course. **Scoring under wide FOV (2026-11-12, `florio_eval`, build `build\w\fov-ab`, 3 runs at once on slots 2/4/5, runs `2-20261112_110750` default, `5-20261112_110758` `0x296400`, `4-20261112_110749` `0x296400,0x38cdbc`, `SNAP_FOV=1.3`):** all three runs reach photo select, pick a Dodrio photo, and get the professor's scoring with no crash. Default: Dodrio, Pose 500, Size 142, Direction 851, expedition points +474 (different shot: the route's photos are not frame-identical across runs). `0x296400`: Dodrio, Pose 500, Size 83, Direction 857, Placement 893, Other Pokemon 48, Total 2,381, +2,381 points. Both sites: Dodrio, Pose 500, Size 80, +2,375 points. Subject recognition, thumbnails and the evaluation draw work under both wide sets and the two wide totals agree within 6 points, so the scoring frustum does not break; the default total is not comparable (different photo). **FOV PAUSED (owner 2026-11-12): "turn off fov, hide the option, stop working on it for now" (it changes photo scores: Size 142 -> ~80). The F1 row and the settings key `gfx_fov` are removed; `SNAP_FOV` stays a developer env switch, off by default. No FOV work until the owner reopens it.** Recipe (still off by default): `SNAP_FOV=1.3 SNAP_FOV_SITES=0x296400,0x38cdbc` (the render camera plus the gameplay/visibility camera, so every consumer sees the same angle); `0x296400` alone also scores sanely. Not verified: a same-photo before/after score, and edge-of-frame subjects. Test caveat: runs under load reach the ride at very different present counts (the A-mash route shifts), so cross-run pairing failed and the within-run toggle was needed. `SNAP_TRACE_LOD_SEQ` added. Course run (Florio, `florio_photos`, runs `0-20261112_074044`, `0-20261112_080238`, `analysis/mods/fn_parity_course*.tsv`): the gameplay camera is still `0x38cdbc` in the course (36 deg, 600 hits per 600 frames; `0x2973b0` also 600). Per-frame entry counts of the hottest imbalanced functions (`0x2c1130 0x5aec90 0x31e100 0x902b90`, skinning/matrix helpers) are CONSTANT every frame (e.g. 52,52,52,...) with spikes only on shutter frames, so the even/odd imbalance in the parity tables is shutter spikes, not a 30 Hz update: no function-entry level half-rate signature exists; the distant-animation reduction, if any, is a data/branch inside the per-frame update, not a call skipped every other frame. LOD and distant animation FOUND by static RE (2026-11-12, details in the `host/mods/lodanim.cpp` comment): the model instance class (vtable `0xc7c5e8`, ctor `0x32dc90`, LOD meshes loaded by `0x342700`, 0x80 bytes per LOD) picks its LOD in its update `0x334db0` (`0x337c2c` main path, `0x337a3c` sub-model loop; the `0x337a70` tanf caller). It computes the screen share of the bounding sphere, `pi*(r*scale)^2 / ((2*d*tan(fov/2))^2 * aspect)`, with the camera at `0xcfb480` (fov `0xcfb4c8`). It compares it with a 3-float table per LOD group (seen: 0.9/0.6/0.5 and 0.8/0.5/0.2): LOD0, 1, 2, or 3 = not drawn. The result is stored at instance `+0x4174`. The distant-animation limit is in the pre-draw `0x338170` (`0x33a744..0x33a770`): the render job `+0x158` (`0x971170`, queued by `0x98d3b0`) is rebuilt every frame at LOD0, every 2nd frame at LOD1 and every 4th at LOD2. `SNAP_DRAW_DISTANCE=<m>` multiplies the ratio by m^2 (hooks `0x337ca8`, `0x337ab8`), so every LOD switch and the cull distance move out by m. `SNAP_FULL_RATE_ANIM=1` (hook `0x33a744`) rebuilds the job every frame. Both are off by default. Course trace (`SNAP_TRACE_LODSEL=1`, Florio): default picks were LOD0 37%, LOD3 (culled) 17% of sub-model picks; at `SNAP_DRAW_DISTANCE=4` they were 82% and 3%. About 60% of the per-frame job checks would have skipped a rebuild by default (5,410 of 8,970). `SNAP_DRAW_DISTANCE_AB=<P>` is a test aid that applies the multiplier only on odd blocks of P presents. Clean A/B (2026-11-12, route `lod_yard`: tutorial A taps only, camera still in the lab yard; `SNAP_DRAW_DISTANCE=4 SNAP_DRAW_DISTANCE_AB=8`, dumps at presents 16k+7 (x1) and 16k+15 (x4), run `0-20261112_110822`): `analysis/mods/compare/drawdist_yard_x1_left_x4_right_crop.png` shows the mid-distance flower beds drawn denser and more leaf shadow on the near tree at x4; 27,332 px differ in all three pairs while staying stable within each phase (`drawdist_yard_changed_px_mask.png`). Scoring check (florio_eval, default run `5-20261112_095022` vs `SNAP_DRAW_DISTANCE=2` run `1-20261112_104759`): NEITHER run reached a professor score. Default: Wurmple, 5 one-star photos, all refused as "subject not clearly visible". DD=2: 7 photos, 4 subjects (Bidoof, Grookey, Comfey, Taillow), "You have no photos to show to the professor, skipping evaluation". The runs took different photos (load-dependent timing), so scoring is NOT compared; defaults stay OFF until a route that reaches a score is run both ways. `SNAP_FULL_RATE_ANIM` cost is not measured (the perf bench needs exclusive slots) and its visible smoothness is not checked. Risk: a wider FOV or longer draw distance can change photo framing and Evaluation visibility, so keep them off by default and re-check scoring.
- **Graphics mods (2026-11-12, `runtime/nvn/nvn_entry.cpp` `ApplyHostEnvPolicy`, `nvn_host.h`):** env `SNAP_ANISO=1..16` (mipmapped linear non-compare samplers only), `SNAP_LOD_BIAS=<f>|auto` (auto = -log2 scale when scale is not 1), `SNAP_RENDER_SCALE`, `SNAP_SHADOW_RES=2048|4096` (1024 depth maps only); output size is `SNAP_WINDOW_W/H`; `NVN_FRAME_DUMP_AT=<presents>` and `SNAP_SCALE_LOG=1` for tests. Verified 2026-11-12 (shots in `analysis/mods/compare/`, rows 1/3/4/9 of `docs/mod-ideas.md`): aniso 16x is now the DEFAULT (`SNAP_ANISO=0` = 1x); render scale 2 at 3840x2160 works with 0 validation errors (device loss came from the 1920-wide depth exemption catching the main depth; the Evaluation depth is now tagged from the guest call chain, `nvnHostSetGuestFrame` from the generated bridge, and viewports/scissors re-scale on render-target scale changes); `SNAP_LOD_BIAS` auto is the upscaler bias (0 at native scale); `SNAP_SHADOW_RES=2048` sharpens shadows without offset; `SNAP_LOOK` and `SNAP_WINDOW_FRAME_CAP=30` (33.3 ms median) work. Comparison tools: `NVN_FRAME_DUMP_FULL=1` (full-size dumps), `NVN_FRAME_DUMP_POST=1` (swapchain after look/UI). Present-count dumps are not scene-exact across runs (load times shift the A-mash route); dump many frames and pair them by image difference.
- **Lighting preset (2026-11-12, `SNAP_LIGHTING=high`, `host/mods/gamemods.cpp`, `runtime/nvn/nvn_draw.cpp` `ShadowDistanceOnBind`, `runtime/nvn/nvn_entry.cpp`):** `SNAP_SHADOW_RES` (dynamic + static maps), `SNAP_SHADOW_DISTANCE=<1..16>` (new, original 1), `SNAP_SSAO_QUALITY`, `SNAP_SSAO_RADIUS`, `SNAP_SSAO_STRENGTH` (new, SimpleAO only); `SNAP_LIGHTING=high` = 2048 maps + 2x distance + SSAO high, every explicit switch wins; default is the original lighting. Findings: (1) the dynamic shadow is ONE 1024 ortho map refit every frame around the subject (ortho half-extent about 64 units in the lab yard, 1.4 units near a Pokemon); the cast light view-projection is a 64-byte VS uniform buffer at slot 9 of the ShadowCast draws (rows 0-2 w=0, row 3 w=1, row-vector convention), the receivers read the same matrix remapped to texture space (0.5x, 0.5 offset) at floats 16..31 of a 132-byte buffer at slot 9. `SNAP_SHADOW_DISTANCE` scales the x/y columns by 1/k in guest memory in the `BindUbo` replay op before the GPU mirror sync (idempotent); no guest code was changed and the guest function that fits the ortho box was NOT found (`DynamicShadowFarClip` consumer unknown). The static shadow is 3 further 1024 maps whose receiver matrix has about 290-unit extent; `StaticShadow` pass was off (`+0xf0`=0) in the pass dump, so static resolution only matters if it renders (not scaled separately; `SNAP_SHADOW_RES` covers it). (2) The course render has NO SSAO: the live SSAO objects are HBAO 8x32 and executed only near present 20000 (photo select); contact darkening in the course is baked, so SSAO quality switches do not change it. (3) A/B (`analysis/mods/compare/light_*.png`): 4096 gives cleaner Pokemon ground-shadow edges; the gain is small; 2x distance showed no visible change in ride frames (same-view pairs differ only in UI/butterflies). Frame-time cost could not be measured (6 runs in parallel, SNAP_PERF needs exclusive; 25-30 ms per present noise). Debug: `NVN_SHADOW_LOG=<present>` prints caster/receiver uniform buffers. Hooks need an exe linked against a main lib newer than `snap_gen.py` (the pre-10:00 `build\snap` exe had no `rt_gm_ao_*` hooks).
- **Anti-aliasing (2026-11-12, `runtime/nvn/nvn_aa.cpp`, `nvn_aa_spv.h`, shaders in `host/mods/shaders/`; SMAA and CAS are MIT, notices in `THIRD_PARTY_NOTICES.md`):** default is `ssaa2+smaa` with CAS 0.3. `SNAP_AA=off|original|fxaa` is the original path, `none` removes the game's FXAA and adds nothing, tokens combine with `+` (`ssaa2+smaa`). `ssaa2` sets render scale to 2x the window height over 1080 (an explicit `SNAP_RENDER_SCALE` beats the default ssaa2). `SNAP_SHARPEN=0..1` is CAS. `SNAP_AA_AB=<P>` (SMAA/CAS only while present/P is odd) and `SNAP_AA_DUMP=<dir> SNAP_AA_DUMP_AT=<presents>` (writes `aa_p<N>_{before,smaaonly,after}.bmp` of the SAME frame, no UI; 4K pairs in `analysis/mods/compare/4k/`) are test aids; `SNAP_AA_LOG=1` logs each hooked draw, `SNAP_AA_SPLIT=1` shows the original on the left half and the result on the right of one frame. Finding: the game's runtime tonemap is `TonemappingLutPS` (key 5729ba114d4b, no FXAA), so the original has effectively no AA. The four FXAA tonemap variants are patched in SPIR-V (edge test forced false); SMAA and CAS run in place on the tonemap output before the UI pass. The downsample at the present blit is exact 2x2 box only when scale/window is exactly 2; other ratios are plain bilinear. The saved photo JPEG also gets the AA, but the score reads Evaluation data (1920 depth target is untouched). Verified: SMAA and ssaa2+smaa+CAS 0.5 run on `florio_eval` without pipeline errors, crops in `analysis/mods/compare/aa_*.png`. The default run (`4-20261112_114559`, 3840x2160 render) reached photo scoring (Dodrio +2,413; scores vary per run, so equality to the original is not proven). Unverified: frame times, LOD bias/aniso with ssaa2, tex_combo crops, a same-scene original-vs-new pair (the route is not deterministic).
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
- **Entry-level DRP mods (2026-11-11; `docs/mods.md`, `runtime/mods/`, `scripts/mods/make_override.py`):** packs under `%APPDATA%\SnapRecomp\mods\<pack>\<archive>.drp\<entry>.<ext>` replace single DRP entries without repacking, any size. The hook is `MAIN_INLINE_HOOKS` in `recomp/snap_gen.py` (before `main+0x5e3a58`, in the entry worker `0x5e38b0`); changing it needs `main` regenerated.
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
- **Owner request (2026-11-11): the F1 menu, profile picker and shader screens must look like they belong in the game.**
  - **Approach:** restyle the ImGui overlay from a measured style reference (`analysis\ui\style_ref.md`). The game's font and UI textures are loaded at run time from `extracted\`, with a plain fallback if they are missing.
  - **Audio:** an Audio tab drives `snap_audio_set_volume`.
  - **Later:** adding an entry to the game's own pause or options menu is only a feasibility check for now.
- **Option pages (2026-11-12):** tabs Graphics, Display, Photos, Mods and the Audio output mode are table-driven in `runtime\ui\ui_options.cpp`; settings keys map to env names in `kEnvKeys` (`runtime\hle\settings.cpp`), env stays the override. Upscaler, upscaler quality and lighting preset are disabled "coming soon" rows (flip `kSoon` when code lands). "Apply and restart" uses `restart_now`. Tests: `tests\hle\run_options_test.ps1` (details in `host\README.md`).
- **Quality presets (2026-11-12, `runtime/ui/ui_options.cpp` `kPresets`):** F1 Graphics "Quality preset" row (Very Low, Low, Normal, High, Ultra, Maximum, Custom). The preset is derived from nine rows (render scale, AA, sharpen, aniso, LOD bias, shadow res, SSAO quality, `gfx_ssao_samples`, `gfx_ssao_hbao`), never stored; Normal = all defaults. Sub-native render scale (0.5, 0.75) LOST the GPU device after the photo score screen in 4 of 4 `florio_eval` runs (scale 1 never), so no preset uses it and the slider starts at 1.0; scale 6 and 8 crash at boot, 4 is the max tested. `SNAP_SHADOW_RES=512` added. `SNAP_PASS_OFF` is a stub (names are never matched), so no preset turns passes off. Details and numbers in `host/README.md`.
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
- **Perf pass 1 (2026-11-11; `analysis\perf\{baseline,results,proposals,shader_precompile_api}.md`):** the lab intro went from 32.1 to 57.5 fps average.
  - **Pacing:** `Pace()` added a full extra period to every slow frame. Fixed.
  - **Buffer sync:** bound buffers were synced per draw, about 128 MB per frame. They are now synced once per submit.
  - **Pipelines:** an async pipeline service with a persistent `VkPipelineCache` and key list in `%APPDATA%\SnapRecomp\shadercache`. Startup precompile goes through `runtime\nvn\shaderprecompile.h`.
  - **Open risks:**
    - A cold cache skips about 1.4 draws per new pipeline. A photo taken in that window could miss objects, so photo frames must never skip draws.
    - Texture-upload bursts (320-440 ms at scene changes) set the worst lows.
    - The first course was not measured.
  - **Pass 2 design** (`analysis\perf\pass2_design.md`): implement it after the photo worker releases `runtime\nvn`.
    - **Order:** instrumentation, then dirty-page tracking with `GetWriteWatch` (poolsync copies 44-52 MB per frame, about 3 ms), then off-thread texture uploads, then a fast pipeline link with GPL (the RTX 4090 supports it). `present_wait` pacing comes after that.
    - **Finding:** most frames over 50 ms are guest-side stalls (replay under 3 ms). The 1% lows need a CPU-side investigation (game thread, `nn::os`, file I/O), not more renderer work.
- **Perf pass 3 (2026-11-12, CPU side, Florio course; `analysis\perf\results.md` "Scene 2 ... pass 3", `stall_report.py`):** no proven gain.
  - **Numbers:** exclusive benches, course presents 6000-11400. Core release off: 50.9 fps, 1% low 16.8, 134 frames over 33 ms, 28 over 50 ms. Core release on, two runs: 49.3/11.3/211/70 and 56.0/24.1/45/8. The PC was shared with another project's game and tests, so same-config runs differ more than the configs do.
  - **Cause found:** on a quiet run, `nn::os` waits are small in slow course frames (core waits 1-4 ms, file reads and sleeps 0). The frame is a job graph; the presenting `JobWorker`'s NVN replay grows (10-15 ms, up to 45). On a saturated PC, pinned loader threads (`ResourceUncompress` p31) starve for virtual cores held by JobWorkers during replay or GPU waits.
  - **Switches (default off):** `SNAP_NVN_RELEASE_CORE=1` (bridge releases the virtual core in submit/present/finish/SyncWait/acquire; `host\gen_nvn_bridge.py`), `SNAP_OS_CORES`, `SNAP_OS_OVERCOMMIT_US`, `SNAP_OS_RR_US`, `SNAP_PERF_WAIT_MS`. New `SNAP_PERF` output: per-cause blocked time (`gt_*`/`mt_*`/`oth_*` columns, `<base>_waits.csv`, long-wait events naming the waker or the core holders).
  - **HD mods:** the owner's `%APPDATA%\SnapRecomp\mods` slowed hidden-run loading about 15x (present #300 at 131 s vs #4,950 at 145 s with `SNAP_MODS_DIR` empty). Loader threads wait on one lock. Not fixed (`runtime\mods`). Benches use an empty `SNAP_MODS_DIR`.
  - **Next:** the requested NVN changes are in `pass2_design.md` "Pass 3 note" (dirty pages first, then replay sub-scopes).
- **HD assets (owner decision, 2026-11-11; `docs/hd-assets.md`):** the owner allows third-party model files for personal, local mods.
  - **Storage:** they go only in `build/hd/external/`, with provenance in `SOURCES.md` there. They are never committed or distributed.
  - **File types:** model and texture formats only, never executables.
  - **Unchanged:** the rule against downloading game dumps, keys, firmware or Nintendo SDKs still stands.
  - **First comparison (2026-11-11):** only Scorbunny's model is clearly more detailed than Snap's (46.7k vs 9.0k tris). Pikachu, Pichu, Bidoof, Vivillon and Bouffalant are worse, and Grookey is about equal. They come from Pokemon-3D-api/assets, which repackages Sketchfab fan models. They are listed in `build\hd\cache\alt_models.json`, and the comparison images are in `analysis\hd\compare_<species>.png`.
  - **AI re-texture (2026-11-12):** `scripts/hd/retexture_sd.py` (SD1.5 + tile ControlNet in `tools/sd/`, venv `tools/venv-sd`) writes `<tex>.x4_sd.png`; groundB, bark and flower look faithful and visibly richer, rockD/rockK drift (invented content); details in `docs/hd-assets.md`. `x4_blend` (50/50 Real-ESRGAN + SD, `scripts/hd/blend_variants.py`) is the owner pick; x4_scan rejected.
  - **In-game HD textures (2026-11-12, `docs/hd-assets.md` "Phase 2 result"):** `scripts/hd/detail_boost.py` (own-detail synthesis + bump for tiling env textures, `x2_detail`) and `scripts/hd/make_hd_pack.py` (cache PNG to nutexb pack; prefers x4_blend, then x4_sd for bark and flower, then x2_detail) build `build\mods_hd_florio\hd-florio` (28 Florio textures, BC1/BC3/BC5 same format as the original, up to 4096). Loaded in game: 28 of 28 `[mods] hit` lines, no crash, ground and path crops in `analysis\mods\compare\hd_{ground,path}_{orig,hd}.png` show finer grain and crisper pebbles (modest, not dramatic). The 2x magenta wash was NOT a size bug: the same wash appears with 1x textures; it is the full-screen 960x540 overlay `hud-00024/25` replaced by magenta during a scene fade (`analysis\mods\compare\magenta_wash_1x_hud2425.png`). The loader now also rejects a malformed nutexb replacement with a `[mods] skipped` log line (`nutexb_problem` in `runtime/mods/mods.cpp`; compiled-checked only, not linked into a run).
  - **HD follow-up (2026-11-12):** `hd-florio` ran at 4K with 28 of 28 hits and is installed in the owner mods folder. Pikachu texture packs above the original 1024 size may crash the hub load (QA B1); `make_pikachu_packs.py` caps at 1024. The loader now caches replacement files. Details in `docs/hd-assets.md`.
  - **Owner direction (2026-11-11):** models are parked; do texture upscales only.
    - **Detail:** add modest detail (crisp eyes, markings and edges, fine surface grain) and keep each Pokémon faithful to its official look. Check against reference images in `build\hd\refs\` (local only).
    - **Comparisons:** always textured, with the same camera and scale for both sides.
- **NVIDIA SDKs (owner decision, 2026-11-11):** the owner approved downloading the DLSS (NGX/Streamline) and NVIDIA frame-generation SDKs from NVIDIA's developer site, for local use.
  - **Storage:** they go only in `tools\nvidia\`, which is git-ignored, and are never committed or distributed.
  - **Public builds:** if DLSS is missing, the code falls back to FSR or native rendering.
- **Upscalers (owner request, 2026-11-12):** DLSS/DLAA, AMD FSR 3.1.4 and Intel XeSS, selectable. Local SDK downloads approved: NVIDIA in `tools\nvidia\`, AMD FidelityFX in `tools\amd\`, Intel XeSS in `tools\intel\`, all git-ignored and never committed. Photos and scoring never use jittered or upscaled output.
7. Save states (owner request, 2026-11-11): save and reload state buttons, so a player can retry one photo.
   - **When:** after photo capture works, a full course scores and 60 FPS is stable.
   - **First step:** a read-only feasibility study.
   - **Likely route:** session-only states. Quiesce all guest threads at a frame boundary, snapshot guest memory plus the host runtime state, and track the NVN/Vulkan objects created or destroyed since the snapshot.
   - **Risks:** host stacks, GPU resources, audio and file handles.

## Repository hygiene

- **Git:** the repo is public on GitHub at `finalpantasy/new-pokemon-snap-recomp` (created 2026-11-11, made public by the owner the same day), with code and two README screenshots only.
  - The local git identity is `finalpantasy` with its noreply email. Never use the global identity.
  - Only the coordinator commits and pushes. Before every push, check that `git grep -i` finds no owner name or email and that `git ls-files` contains no game formats or images.
  - Push using the finalpantasy token (`gh auth token -u finalpantasy`) through a per-command credential helper. The active gh account is a different one.

- `work/`, `extracted/`, `analysis/`, and `tools/` are generated/local and ignored.
- Never commit `.nsp`, `.xci`, `.nca`, `.nso`, `.keys`, extracted RomFS, generated C from the game, NVIDIA binaries, or proprietary SDK files.
- Keep research/source code separate from game-derived artifacts and record tool versions and input hashes for reproducibility.
- **Window options (2026-11-12, code written, NOT yet linked or run):** host/window_opts.cpp (F11/Alt+Enter borderless, monitor, remembered rect, pause + mute on focus loss via host tick freeze and present-hook block because the game imports no focus query, window.json, C API; docs in host/README.md), os_time_pause in runtime/os/os_kernel.cpp, hle::oe_post_message in runtime/hle/services_sys.cpp; frame-cap line for nvn Pace() is request #3 in analysis/mods/nvn_hook_requests.md (applied). Linked and run 2026-11-12: the focus test (`SNAP_WINDOW_TEST=blur@70,focus@90`) shows presents and guest time frozen for the blur, audio output 0, all resumed on focus; F11, monitor moves and a real alt-tab are untested.
- **HDR10 output (2026-11-12, `runtime/nvn/nvn_hdr.{h,cpp}`, `runtime/nvn/shaders/hdr.{vert,frag}`, `nvn_hdr_spv.h` made by `shaders/make_spv.py`; small edits in `nvn_vk.cpp`: extension lists, format pick in `WindowImpl::Recreate`, a branch in `PresentWindowOnce`; `WindowImpl::hdr`):** off by default. `SNAP_HDR=1` enables `VK_EXT_swapchain_colorspace` and `VK_EXT_hdr_metadata`, creates an A2B10G10R10 + HDR10_ST2084 swapchain, sets the static metadata, and replaces the present blit with a PQ pass (sRGB decode, inverse tone map that lifts only the top quarter of the range, Rec.709 to Rec.2020, PQ). `SNAP_HDR_PEAK` (nits, default 1000), `SNAP_HDR_PAPER` (default 200). If the display or driver lacks the format the log says `hdr: A2B10G10R10 + HDR10_ST2084 not offered ... falling back to SDR` and the SDR path runs; a failed pass setup also falls back. Debug: with `NVN_FRAME_DUMP`, `NVN_FRAME_DUMP_POST=1` and `SNAP_HDR=1`, each dump is the raw PQ swapchain image written as `<name>.bmp` (SDR preview with a soft shoulder above paper white) plus `<name>_nits.bmp` (grey to paper white, then blue to red up to peak) and a `hdr: dump ... mean/max nits` log line. Photo capture, thumbnails and scoring read the game's own targets and never see this pass.
  - **Verified:** the hidden desktop DOES create the HDR10 swapchain (`hdr: swapchain 1280x720 fmt=64 colorspace=1000104008 HDR10 ST2084 active`, runs `4-20261112_094357` and `4-20261112_100111`). Opened previews: loading screen, course frames, professor dialogue, photo select; course frames average 100 to 130 nits with a 400 nit maximum (itm only lifts near-white). Validation run (florio_eval, `4-20261112_100111`): 1 error, a `vkCreateGraphicsPipelines` with a BC1 colour attachment at startup, logged before the first present (existing pipeline service, not the HDR pass); the HDR pass gave none. The ImGui overlay and `SNAP_LOOK` problem is fixed (see the HDR overlay entry below).
  - **Scene-tap mode (option a) NOT built, findings:** the scene HDR target is RGBA16F 1920x1080 (NVN format 0x29). The tonemap plus FXAA draw (prog 411 in boot runs, fragment key `724587eedba6`, one sampler) writes straight into the RGBA8 SRGB window texture (0x38, 3 rotating slots), and a later draw (key `0b6e3cad047c`) also writes the window texture. But the loading and menu UI is drawn into the HDR scene target BEFORE the tonemap (many draws with rt0 = the 0x29 target plus a depth buffer), so UI cannot go on top at paper white without replacing the game's tonemap (bloom blend, LUT, exposure, FXAA). That needs its own pass fed with the game's constants; not attempted.
  - **scRGB mode (2026-11-12, owner request):** `SNAP_HDR=off|hdr10|scrgb` (`1` = hdr10). `scrgb` = R16G16B16A16_SFLOAT + EXTENDED_SRGB_LINEAR swapchain, linear Rec.709, 1.0 = 80 nits; fallback chain scrgb -> hdr10 -> SDR with a log line at each step. Both modes: `SNAP_HDR_PEAK_NITS` (default the DXGI_OUTPUT_DESC1 MaxLuminance of the window's output, else 1000; 775 on this PC, also works on the hidden desktop; the old `SNAP_HDR_PEAK`/`SNAP_HDR_PAPER` still work), `SNAP_HDR_PAPER_NITS` (200), `SNAP_HDR_EXPAND` (0..1, default 0.3, how far near-white is lifted: a plain SDR frame cannot separate UI white from a highlight, so the UI is at paper white only for expand 0) and an exponential roll-off above 75% of peak so highlights approach the peak instead of clipping. Verified (runs `1-20261112_105753` scrgb with validation, `1-20261112_114517` hdr10; opened a scRGB title frame): both swapchains active at 775 nits; scRGB run had only the same startup BC1 pipeline validation error as above and no HDR error. Banding probe (`hdr: dump ... sky strip` line: distinct green codes and one code step in the top quarter of the frame, title screen): hdr10 one code = 0.96 to 1.7% of luminance, scRGB one code = 0.05 to 0.08% (fp16). The overlay and `SNAP_LOOK` are handled in both modes (next entry). Photo scoring was re-checked only in hdr10 mode (florio_eval run `3-20261112_101335`: the score screen shows 2,376 for a Vivillon photo); scRGB uses the same untouched game targets. One hdr10 run crashed in a guest thread right after the swapchain was created (`1-20261112_111536`, host rip in the `std::unordered_map<int, TexSlot>` emplace of the texture pool code, other workers were editing `runtime/nvn`); the rerun was fine; triaged 2026-11-12: a null-object dereference in a host NVN entry reached from guest `main+0x91044c` before the first present, not the swapchain path, never reproduced (noted in `analysis/qa/bugs.md`).
  - **Overlay and SNAP_LOOK under HDR (2026-11-12):** with HDR active `PresentWindowOnce` blits the scene into an RGBA8 sRGB scratch image (`snap_hdr::PrepScratch`), runs `snap_look_present` on that scratch, and runs `snap_ui_present` on a second RGBA8 UNORM layer cleared to transparent (the overlay code is unchanged; it only sees a different image and format). `hdr.frag` then composes in linear light: scene gets the inverse tone map and roll-off, the layer (premultiplied, sRGB encoded) is decoded and blended at `SNAP_HDR_PAPER_NITS` after the roll-off, so UI is never lifted; output is PQ (hdr10) or nits/80 (scRGB). Verified with `SNAP_UI_TEST` (F1 at 56 s) and `NVN_FRAME_DUMP_POST`: the open F1 menu is a clean white panel at paper white (hdr10 mean 144 nits, max 199.2; scRGB max 200.0; previews `analysis/hdr/f1_hdr10_menu.png`, `f1_scrgb_menu.png`, opened); validation showed only the old startup BC1 pipeline error. `SNAP_LOOK` was not run under HDR (code path shared with the SDR scratch, untested). Test notes: hidden runs take about 45 s of wall time before the scene appears (black frames before that, not a bug), and the ui-test `key F1 0` posts a second toggle that closes the menu. `SNAP_HDR_DBG=1` clears the scratch red and skips the scene blit.
- **Fast boot (2026-11-12):** `SNAP_FAST_BOOT=1` drops the frame limiter for the first 3200 presents (`SNAP_FAST_BOOT_FRAMES`). Present #3030 (title logo up) at 58.8 s average over 3 exclusive runs before, 42.4 s after. The first ~2070 frames are a black, frame-count-bound load phase that A presses do not skip.
- **Photo export, gallery, album limit (2026-11-12, album worker; `host/mods/photoexport.cpp`, `tools-src/gallery/`, rows 22 and "Full-res photo export" of `docs/mod-ideas.md`):**
  - **Export (done, verified):** every photo the game JPEG-encodes for the album is also written as a lossless PNG plus a `.json` sidecar (size, JPEG byte size, CRC) to `%APPDATA%\SnapRecomp\photos\`. Two host overrides wrap the lifted `nn::image::JpegEncoder::SetPixelData` and `Encode` (the original still runs, so the saved JPEG is unchanged); a worker thread writes the PNG through WIC. The game passes pixel format 14 (RGBA bytes) at **1080x720**; thumbnails are 240x200 and 204x136 and are skipped (`SNAP_PHOTO_MIN_W`, default 640). 1080x720 is the game's own capture size (its Capture3D target), so this is the full resolution of a photo; it does not follow `SNAP_RENDER_SCALE`. `SNAP_PHOTO_EXPORT=0` disables, `SNAP_PHOTO_DIR` moves the folder, `SNAP_PHOTO_LOG=1` logs each encode. Test runs (`SNAP_SAVE_DIR` set, no `SNAP_PHOTO_DIR`) export nothing, so they never fill the player's folder. Each distinct JPEG is written once per session (the game encodes some photos twice). Checked: a florio_eval run (build\album_test\photos) wrote 15 PNGs; opened `photo_20261112_112222_001.png` (a real Dodrio and Vivillon scene); `analysis/mods/compare/gallery_page.png` shows the gallery with real photos.
  - **Encoder detail:** the first `Encode` calls on a photo return 0xffffffbf (buffer too small) and the game retries with a bigger buffer; only a result of 0 is exported.
  - **Gallery (done for export, partly for album):** `tools-src/gallery` lists the PNG exports (with date from the file name) plus the save's album, Source filter, thumbnails with Pillow if present. An export is joined to its album entry by equal JPEG byte size, confirmed by a 24x16 grey-thumbnail comparison (the CRC cannot be used: the game edits the JPEG after the encoder). Course, species and score are NOT shown: `META_LAYOUT` (file 6, 0x148 bytes) is still undecoded, because no run has left more than one album record in a save (the route never ends a course, and the album is written then). So the album-to-export join was not seen to succeed on real data.
  - **Unlimited photos (#3): decision, PNG export is the unlimited store.** The 1200 limit is the entry count in the headers of save files 6, 7 and 10; the code that sets it was not found (`analysis/save/album.md`), the game makes those files itself with fixed sizes, and 1200 x 0x70840 bytes is already 82 MB. Raising it would mean patching lifted code plus migrating saves, with no way to check the album UI, so `SNAP_ALBUM_LIMIT` is NOT implemented. The export folder has no limit.
  - **Not done:** #4 photo-score overlay; settings-menu toggle for the export (env only); decoding file 6.
