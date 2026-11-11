# CPU backend evaluation: Kirikae vs LAS/TotK-style AArch64-to-C

Status: read-only source study, 2026-11-11. No project was built or run (reason in section 6). No game data in this file.
All numbers labeled "claim" are the projects' own statements, not measurements of ours.

Sources (shallow clones under `tools\ref\`, ignored by VCS):

| backend | repo | newest commit read | license | commits (GitHub API) |
|---|---|---|---|---|
| Kirikae | `Cor3Duo/kirikae` (`tools\ref\kirikae`) | 2026-11-07 (repo created 2026-10-23; v0.1.0-v0.1.2 released 2026-10-23..24) | MIT or Apache-2.0 | 133 |
| TotK Recomp | `valentderah/totk-pc-recomp` (`tools\ref\totk`) | 2026-11-10 | MIT | 1006 |
| LAS Recomp (canonical) | `peiikai/LASRecomp` (`tools\ref\las`), linked from `totk\README.md:74` and `totk\THIRD_PARTY_NOTICES.md:8` (pinned at `da35b83`) | 2026-11-06 (579fdad) | MIT | 950 |

TotK's CPU half is a derivative of LAS (lifter, NEON lowering, CPU headers, differential tests), rewritten for scale
(`totk\docs\architecture.md:17,33-46`). Below, "LAS/TotK" means the TotK generator unless stated; LAS-only facts are marked.
LAS and TotK are Python generators emitting C; Kirikae is a Rust workspace (`crates\a64` 11.2K lines, `recomp` 3.0K,
`rt` 31.6K, `loader` 2.1K).

## 1. Inputs and ingestion

**Kirikae**
- Takes a whole dump: NSP/XCI/ExeFS folder/NSO/NRO (`crates\loader\src\package.rs:225-275`, `nso.rs:55`). Modules are
  laid out at `LOAD_BASE = 0x0800_0000` (`loader\src\lib.rs:17`, `package.rs:43`), not at the Switch base.
- Function discovery is its own and runs at build time: seeds = each module entry + `dynamic::code_pointers`
  (RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT to defined targets, plus defined FUNC dynsyms; `loader\src\dynamic.rs:94-150`,
  `recomp\src\project.rs:152-159`), then a worklist over BL targets, ADR, and ADRP+ADD (`recomp\src\discover.rs:224-262`).
  No `.eh_frame` reader (grep for `eh_frame` in `loader` and `recomp`: no hits). Only RELA tables are read (no DT_RELR; Snap
  uses RELA per our `relocations.tsv`, so this is not a blocker).
- There is no way to feed an external function list. Extra entries come only from a profile file of addresses the
  interpreter had to run (`project.rs:191-264,313-320`, header `# kirikae:`, tagged to the dump via `.guest`).
- Adapter for us: (a) a small patch to `discover::find_entries` seeds (`project.rs:313`) to also read `functions_final.tsv`
  (FDE starts, Ghidra starts, address_taken) or, with no code change, write our list as a `.profile` file (format at
  `project.rs:191-218`; needs the `.guest` hash tag); (b) feed the loader the NSO directly (it LZ4-decompresses; our
  `main.uncompressed.nso` is probably accepted, to be verified at first build); (c) map our module-relative offsets by
  adding `LOAD_BASE`.

**LAS/TotK**
- Python generator over raw NSO files: `recomp\nso.py` (LZ4, MOD0, DT_RELA/JMPREL/RELR, `.eh_frame_hdr` bounds),
  `recomp\module.py:1-112`. Function starts = `.eh_frame_hdr` FDEs + exported FUNC symbols + RELATIVE targets + every BL
  target + ADR/ADRP+ADD materialized code addresses, then a fixpoint that promotes out-of-function branch targets to new
  starts (`recomp\gen.py:18-24,83-170`). LAS does the same (`las\tools\gen.py:20,74,550`, uses `eh_frame_functions`).
  This is almost exactly our own map method (FDE seeds, address_taken), so our TSV is a cross-check, not new information.
- Adapter for us: small. Replace `Module.seeds()` (`module.py`) with a loader of `functions_final.tsv`, and diff the
  generator's `manifest.json` function list against it; or leave it alone and treat the generator's list as the
  independent cross-check AGENTS.md asks for. Guest base is configurable (`gen.py <name> <nso> <base> <outdir>` in LAS,
  `--version`/layout in TotK `recomp\layout.py`).
- Our relocations are consumed at runtime by the host loader (`totk\runtime\loader.c`, 737 lines) rather than by the lifter,
  except RELATIVE targets as seeds.

## 2. Instruction coverage

**Kirikae** (`crates\a64`)
- One IR per instruction (`ir.rs:40-75`, `lift.rs` 861 lines, `lift_fp.rs` 976, `simd\*` ~3.4K incl. `neon.rs`,
  `crypto.rs`, `fp.rs`). The same IR drives the interpreter and Cranelift; `ops.rs` is the single semantic source.
- Integer/branch/load-store: native Cranelift lowering. FP scalar partly lowered; NEON/other FP are `Stmt::Simd(word)` helper
  calls into Rust semantics (`clif.rs:1252-1259`, `simd.rs`): correct by construction but slower than inlined vector code.
- System registers: `lift.rs:186-196` lists TPIDR_EL0, TPIDRRO_EL0, CNTVCT_EL0, CNTPCT_EL0, CNTFRQ_EL0, FPCR, FPSR,
  DCZID_EL0, CTR_EL0 (MRS), writes via `SetSysReg` (`lift.rs:285-298`).
- Exclusives: LDXR/LDAXR/STXR/STLXR/LDXP/STXP lowered to `Monitor`/`StoreExclusive` (`lift.rs:380-411`, state in
  `rt\src\cpu.rs:22-24,103-132` using `compare_exchange` SeqCst). No ARMv8.1 LSE (CAS/SWP/LDADD): grep finds none; harmless
  for a Switch A57 title but unverified for Snap until a coverage scan.
- Unimplemented: anything not lifted becomes `Stmt::Undefined(word)` (`lift.rs:6`) which the generated code reports through
  `kirikae_undefined` (`clif.rs:1275-1283`, `rt\src\abi.rs:49`). `kirikae info <dump>` prints decoder coverage (README).

**LAS/TotK** (`recomp\lifter.py` 805 lines, `simd_c.py` 1157, `runtime\cpu.h` 361, `simd.h`, `simd_fp.h`)
- Integer, branches, load/store, pairs, bitfields, CRC32, FP/NEON lowered to C (clang auto-vectorizes; AVX2/FMA build
  default, x86-64-v2 fallback with software fma, `totk\docs\building.md:95-101`). LAS can alternatively emit AArch64 inline asm on
  ARM hosts (`las\tools\gen.py:1-6`).
- Sysregs: MRS of TPIDRRO_EL0, TPIDR_EL0, CNTFRQ (hard-coded 19.2 MHz), CNTVCT/CNTPCT (`rt_ticks()`), NZCV, FPCR, FPSR, DCZID, CTR;
  MSR of TPIDR_EL0, NZCV, FPCR, FPSR (`lifter.py:318-362`). Anything else raises `Unsupported`.
- Atomics: exclusive monitor emulated by compare-and-swap (`cpu.h:86-125`, incl. 128-bit LDXP/STXP via cmpxchg16b, added in TotK),
  plus ARMv8.1 atomics tested in `tests\diff\atomic.py` (`building.md:233`). DMB/DSB/ISB/CLREX handled (`lifter.py:324-329`).
- Unimplemented: `Unsupported` becomes `rt_udf(c, pc)` (noreturn trap) in generated code (`module.py:73-80`, `runtime\main.c:169`).
  `tests\diff\lift.py --coverage` lists every rejected word (`building.md:233`). TotK reports covering all 68,968 OS-touching sites of 1.4.3 `main`
  (`architecture.md:36`); that is a claim about TotK, the Snap NSO still needs its own scan.

## 3. Control flow

| topic | Kirikae | LAS/TotK |
|---|---|---|
| Direct branches | per-function blocks; branch out of function becomes a call/tail via entries (`discover.rs:271-333`) | `goto` inside the function; out-of-function target is `musttail` call to its function, or via `rt_dispatch` if not a known start (`gen.py:381-388`) |
| Indirect `br` / jump tables | own symbolic `jump_table()` (`discover.rs:136-176`), compact; unresolved falls to dispatcher | `recomp\jumptables.py` (1,345 lines): symbolic walk with a fixpoint, rejects what it cannot prove; checked against a Unicorn oracle with "0 disagreements" required (`tests\jumptables_check.py`, `building.md:236`); unresolved `br` re-enters via `fb_<entry>` switch on `c->pc` then `rt_dispatch` (`gen.py:390-403`) |
| Tail calls | same IR `Jump` to another entry; the entry set controls it (`discover.rs:271-290`) | `musttail` C tail calls (`gen.py:55,386`), so no stack growth |
| Noreturn | no explicit model; `Undefined` and `Return` end blocks; SVC halts via `cpu.halted` (`clif.rs:1260-1273`) | `rt_udf`/break are `noreturn` (`cpu.h:39`); no noreturn inference |
| `blr` through vtables | `call_indirect` -> `run_until` lookup (`process.rs:414-460`) | `rt_dispatch`: O(1) flat table, 4 bytes per guest instruction, sealed read-only (`dispatch.c:1-13,163-196`) |
| Unresolved target | falls back to the **interpreter** (`process.rs:431-446`); records the address (`coverage.rs`, `interp.rs:73`) in a `.profile` consumed by the next build | **fatal**: `dispatch_fail` prints pc, lr and reason, exit 70 (`dispatch.c:176-186`); no interpreter |

For Snap, TotK's fatal-unknown matches AGENTS.md "unresolved targets remain explicit"; Kirikae's interpreter-plus-profile loop
matches "record interpreted/unresolved targets and feed them into later AOT builds" directly (AGENTS.md Validation strategy). Both
satisfy it in different ways. Note AGENTS.md says "Do not link an emulator": Kirikae's interpreter is its own IR evaluator (`interp.rs`,
109 lines), not a third-party emulator, but it is a second execution engine in the product; a TotK-style build has none.

## 4. Memory model

- **Kirikae: translated.** Reserves the 39-bit guest space once and emits `host = base + (addr & ADDR_MASK)` for every guest
  access (`rt\src\mem.rs:7-11,88`, `clif.rs:631-634`). Guest pointer != host pointer. Safe (bad pointer page-faults inside the
  reservation) but costs an AND+ADD per access and conflicts with AGENTS.md "preserve guest pointers as host pointers where
  practical" (NVN/Vulkan imports, host-visible buffers, mirrored pools would need translation at the boundary).
  Stack and TLS are allocated inside it (`process.rs:18-19,189`). Exclusive monitor lives in the per-thread `Cpu` (`cpu.rs:22-24`).
- **LAS/TotK: identity.** Guest address equals host address, all below 2^39 (`totk\docs\architecture.md:37`; the `Cpu` carries
  `tpidr`/`tpidrro`/`excl_addr`/`excl_val`/`excl_val_hi`, `cpu.h:13-20`). Heap/stack/alias are reserved with `MEM_WRITE_WATCH`
  so the graphics layer can ask for CPU-written pages. This is the AGENTS.md preference, and it simplifies NVN memory-pool
  imports. Cost: the host must be able to reserve the same virtual addresses on Windows (TotK does, `runtime\memory.c`).
- Register state: both keep a `Cpu` struct; TotK keeps NZCV and six hot registers in C locals (`flags.py`, `regs.py`); Kirikae has a
  `fast_regs` cache planner (`clif.rs:401-475`, test `recomp\tests\fast_regs.rs`).

## 5. Runtime boundary (SVC / SDK imports / HLE)

- **Kirikae**: emulates the console: Horizon kernel SVC table (`rt\src\horizon.rs`), IPC/CMIF sessions and services (`services\*`),
  an `nvdrv` GPU command processor with a Maxwell translator and Vulkan renderer (31.6K lines in `rt`). `hle.rs:1-40` places host
  functions at synthetic guest addresses (`BASE 0x7f_ff00_0000`) that the dispatcher recognizes; `Table::replace(addr, f)` can
  replace a guest function by address, which is the SDK-import HLE hook we need. NVN is wrapped, not replaced: `KIRIKAE_NVN=trace|shadow|exec`
  (`ARCHITECTURE.md`, NVN section), with shadow-vs-command-processor comparison already present (AGENTS.md wants this concept).
  Per AGENTS.md we do not want the guest NVN driver or `sdk`/`rtld` lifted in production; Kirikae recompiles every module it is given,
  so for Snap we would feed only `main` and implement `nn::*` through `hle.replace` by address from our maps. The IPC/service half
  would then be dead weight we would either drop or reuse.
- **TotK/LAS**: SDK boundary is by symbol/function: `HOST_OVERRIDE` / `HOST_OBSERVER` linker-section tables (`runtime\hostfn.c:1-40`),
  ~163 `nn::*` functions replaced by name in TotK (`architecture.md:13`), 38 SVCs (`runtime\svc.c`, 21 listed as fatal-unimplemented,
  `svc.c:29-36`), only `audren:u` and `sm:` as real IPC (`architecture.md:41`). `rtld` is dropped and linking is done by the host
  loader. This is the AGENTS.md "replace SDK imports at a high-level host boundary" design.
  Snap fit: our `imports.tsv` (731 undefined symbols, 413 `nn`) maps one-to-one to `hostfn` overrides; our PLT/GOT stubs
  need the host loader to bind them (TotK `loader.c`).

## 6. Build and runtime performance

No build was run. `cargo` is installed here, but Kirikae's workspace pulls a large dependency tree (Vulkan, egui, audio) over the
network, no C toolchain (`clang`) is on PATH for the TotK path (it targets MSYS2 UCRT64 clang/lld 23.1.3 and ninja,
`building.md:17`), and other workers hold heavy jobs. Both would exceed the "light work only" bar. Evidence below is documentary.

| item | Kirikae | LAS/TotK |
|---|---|---|
| Output form | Cranelift `.obj`, 1024 functions per object, parallel (`README.md` "How it works", `clif.rs`) plus a generated Cargo project (`project.rs`); needs rustc+MSVC at the user's machine | chunked C (LAS `--chunk` default 1500 functions per file, `gen.py:521`; TotK size-estimated chunks, `gen.py:189`) compiled by clang |
| Build time claim | "whole game in well under a minute on a desktop CPU" (README, claim); the final `cargo build` link is separate | TotK 1.4.3: lifting about 15 s, clean compile about 200 s on 24 threads (`building.md:103-105`, claim); sccache supported; later builds recompile only changed chunks |
| Scale evidence | commercial games run at 30 fps "on a modest GPU" (README "Status", claim); no function counts published in the repo | TotK is far larger than Snap (68,968 OS sites in `main` alone); Snap's ~24.6K functions are well within what it handles |
| Runtime numbers | none published as a benchmark; `KIRIKAE_STATS=1` reports fps and native-vs-interpreted counts | PGO: trained fight 12% faster per frame, untrained about 6%, on 1.4.3 (`docs\pgo.md:1-6`, claim); AGENTS.md already notes this is a reference, not a promise. `docs\agents` / pr-review kit show perf work is ongoing |
| Codegen quality note | Cranelift optimizes less than LLVM (no loop vectorization, weaker inlining); NEON through helper calls | clang -O1/-O2 with PGO and `-march=x86-64-v3`; NEON lowered to vector C |
| Compile-time risk | none known | a pathological function once cost "tens of seconds" in clang loop passes (`gen.py:391-393` comment) |

## 7. Differential testing

- **Kirikae**: Cranelift backend vs the IR semantics (`ops.rs`) on random inputs (`recomp\tests\clif_ops.rs`, `fast_regs.rs`, 244 lines);
  decode/SIMD unit tests; optional Unicorn oracle for SIMD against a Cortex-A57 model (`tools\simd_oracle.py`, `a64\src\simd\oracle.rs`);
  testkit end-to-end synthetic NROs (`crates\testkit`); CI runs `cargo test --release --workspace`. Whole-function and whole-binary differential
  testing is not provided. Runtime tracing: `KIRIKAE_STATS`, coverage JSON of used SVC/IPC/ioctl/shader, per-event PNG and backtrace.
- **LAS/TotK**: the most complete harness of the two: `tests\diff\lift.py` (instructions sampled from the real modules, Unicorn),
  `simd.py` (whole FP/SIMD encoding space), `atomic.py` (exclusives plus LSE), `func.py` (whole generated functions incl. loops, jump tables,
  calls checked at the stub), `jumptables_check.py` vs Unicorn, `excl_mt.py` (monitor under contention), `flags_check.py`
  (`building.md:229-239`). LAS uses Unicorn at generation time for jump tables (`las\tools\gen.py:17`); TotK removed that to keep GPL out of
  the shipped generator (`architecture.md:34`). Unicorn is test-only in TotK, consistent with AGENTS.md.
  Runtime probes: `--probe-kernel`, `--probe-vkexec`, `--probe-nvn-tex`, `--probe-shader`; TOTK_TRACE, flight recorder, perf overlay.
- The `tests\diff` harness needs the Snap modules only for `lift.py`/`func.py`; the others are game-independent.

## 8. Maturity

| | Kirikae | LAS Recomp | TotK Recomp |
|---|---|---|---|
| Age / activity | 16 days, 133 commits, 96 stars, v0.1.2 | about 5 weeks, 950 commits, 2.3K stars | 13 days at read time, 1006 commits, 6.6K stars |
| Games shown | "complete commercial 3D games" (names not given in the README); "tested on a handful" | Link's Awakening (beta, Win/macOS/Linux) | Tears of the Kingdom (playable, 3 game versions) |
| Self-described state | "Early preview", expect crashes and missing services | Beta | "Early development", crashes expected |
| Notable gaps | no updates/DLC; NVN-on-host in progress; interpreter fallback needs profile loop | AI-authored by its own disclosure (README header); LAS-specific host | Python 3.14 pipeline, 60 GB working space, MSYS2 clang; TotK specifics throughout runtime |
| Fit to Snap | recompiles whole dumps incl. `sdk`/`rtld`; host != guest pointers | per-game host layer | closest precedent: large Switch engine game, SDK replaced, NVN to Vulkan |

Caveat: all three projects are weeks old and largely AI-assisted (LAS says so; the other two show it in `AGENTS.md`/`CLAUDE.md`). Treat
every "works" claim as unverified for Snap until our own differential tests pass.

## Recommendation

**Prototype the LAS/TotK-style C generator first, taking TotK's `recomp\` and `runtime\cpu.h` as the base, and use Kirikae as a
differential reference and a bake-off candidate on a fixed function subset.** Do not maintain both as production lifters (AGENTS.md).

Top 3 reasons:
1. Memory and boundary match AGENTS.md: identity guest==host pointers and a symbol-level `HOST_OVERRIDE` SDK boundary with `main` only; Kirikae
   translates every access (`mem.rs`, `clif.rs:631`) and ships a whole-console runtime we would mostly not use.
2. Same discovery inputs as our maps (`.eh_frame_hdr` FDEs, dynsym, RELATIVE, BL, ADRP+ADD, fixpoint) and the strongest test kit
   (whole-function and whole-encoding-space Unicorn diffs, jump-table oracle); the adapter is a seed-list swap.
3. Scale and speed evidence is for a bigger title (about 200 s clean compile, PGO gains claimed); Kirikae's build and run numbers are only claims
   with no data in the repo, and NEON goes through helper calls.

Key risks:
- LAS/TotK: Python generator at 24.6K functions needs memory (TotK pool restarts with fewer workers on OOM, `building.md:103`); unresolved indirect
  target is fatal (needs our trace-and-feed loop, which we build); clang toolchain and size of generated C; per-function code size inflates with `Cpu`
  spills; Windows reservation of identical guest addresses; heavy cooperative-scheduling hooks (`rt_spin_gate`, preemption in `dispatch.c`).
- Kirikae: 2 weeks old, single maintainer (not verified beyond the commit log); Rust/Cranelift plus MSVC toolchain at runtime; per-access masking; Cranelift code quality and
  NEON helper calls; second execution engine (interpreter) in the product; discovery has no `.eh_frame` and no external function list (patch or profile
  trick); would need a fork or plugin hooks to drop `sdk`/`rtld` and replace NVN.

## Bake-off criteria (same inputs, same function set, same machine, BelowNormal priority)

1. **Correctness**: pass rate on a differential set vs Unicorn (an external, test-only dependency): (a) 20K instruction words sampled from `main`
   .text by class (integer, FP, NEON, exclusives, sysregs), (b) 300 whole functions (loops, jump tables, vtable calls, leaf FP) with identical
   input registers and memory, comparing all X/V/NZCV/FPSR and memory writes, (c) any word either backend rejects (coverage list). Target: 0 mismatches,
   rejected words listed with counts.
2. **Compile time for the full `main`**: wall time and peak RSS of lift/codegen, then compile/link of all ~24.6K functions; cold and warm cache; output
   size on disk. Target set by measurement: any build under 10 minutes fits our rules.
3. **Hot-function runtime**: 20 hot functions chosen from the maps (render-pass setup `0x5bd940`, tonemap `0x59ab20`, vtable-heavy, a NEON skinning loop once identified),
   microbenchmarked with fixed inputs, median and p99 per call, both backends against each other. Report ratios, not absolute claims.
4. **Integration cost**: engineer-hours and lines changed to (a) ingest `functions_final.tsv`, (b) bind our PLT/GOT imports, (c) override 10 `nn::`/libc
   functions, (d) keep guest==host pointers for NVN pools, (e) record and feed back unresolved targets. Also: build-toolchain footprint on the user's PC.
5. **Diagnosability**: cost to get a guest backtrace and unresolved-target log on a crash.

## 1-2 week prototype plan (against our maps)

Day 0: record the pinned commits (`da35b83` LAS; Kirikae and TotK commits from `tools\ref`); stage one NSO copy in a scratch folder (read only, no changes to `extracted\`).

1. **Days 1-2, function-list cross-check (no execution of game code).** Run TotK's `discover()` (`gen.py:83`) on `main.uncompressed.nso` with Snap's base and diff its
   starts against `functions_final.tsv` (FDE, ghidra, address_taken tags). Do the same with Kirikae `kirikae info` / `find_entries`. Write the two disagreement
   lists. This validates our maps and measures how many starts each backend finds without Ghidra.
2. **Days 2-4, instruction coverage scan.** `tests\diff\lift.py --coverage` equivalent for Snap's `main` and Kirikae's decode coverage. Output: rejected-word counts per class (NEON, LSE, sysreg, crypto, exclusives), a ranked list.
3. **Days 3-6, differential set.** Build the 20K-word and 300-function sets from `main` (indexed by `functions_final.tsv` so selection is reproducible), run Unicorn once, store golden
   register/memory results as data files (not code), then run each backend's lifted output against them. Needs a one-time Unicorn install in a venv; Unicorn is test-only.
4. **Days 5-8, full-binary build.** Generate the whole `main` with the TotK generator (cold, then warm) and with Kirikae, record time/RSS/size. If a toolchain is missing, install it in
   a scratch location, never touching `extracted\` or `analysis\ghidra\`.
5. **Days 7-10, hot function microbench** using the twenty functions above in a stub harness with fixed memory; no game boot.
6. **Days 9-12, integration spike.** Bind five imports from `imports.tsv` (`nvnBootstrapLoader`, one libc, one libc++, two `nn`) through `HOST_OVERRIDE` (TotK) and `hle.replace` (Kirikae) and call `FUN_7100910270` (NVN bootstrap, `analysis\maps\README.md`) with a mocked `getProcAddress`; record how each backend handles the unresolved indirect call (fatal vs interpreter).
7. **Day 12-14**: score with the criteria above, write a decision note, delete the losing backend's scratch build. If Kirikae loses on pointer model or toolchain but wins on speed, record a follow-up: lift only its `a64` crate and Cranelift emit as a library, not the runtime.

Assumptions and open questions for the first day: (1) Kirikae's loader accepts the uncompressed NSO as-is; (2) Snap `main` uses no LSE atomics (coverage scan answers it); (3) Snap's relocations are plain RELA (our `relocations.tsv` shows RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT only);
(4) the Windows host can reserve the guest range at identical addresses without clashing with the process, which TotK's `runtime\memory.c` shows is workable.
