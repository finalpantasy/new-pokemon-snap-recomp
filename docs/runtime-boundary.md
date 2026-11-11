# Runtime boundary and C++ exceptions

Design decision for where lifted guest code ends and the host runtime begins, and how C++ exceptions and
`setjmp`/`longjmp` work through lifted code. Static analysis only (2026-11-11). No game bytes are in this file; the
data is in `analysis/runtime/boundary/` and is reproduced by
`tools/venv-recomp/Scripts/python.exe -I scripts/sdk_boundary.py` (about 1 minute, read-only).

## Recommendation

Lift `sdk` together with `main`. Run the SDK's own musl libc, libc++, libc++abi and LLVM libunwind as guest code.
Replace `nn::` APIs by symbol in both modules' GOTs (the TotK `HOST_OVERRIDE` pattern), and override pure libc leaf
functions (`mem*`, `str*`) by symbol for speed. Keep an SVC layer only as a logging backstop for paths that still
reach the kernel. Do not lift `rtld`; the host loader replaces it.

This changes the AGENTS.md "CPU and runtime" rule "Do not initially lift `sdk` or `rtld`". Proposed replacement text:

> Lift `main` and `sdk` together; `sdk`'s libc, libc++, libc++abi and libunwind run as guest code. Replace `nn::`
> APIs by symbol across both modules (host overrides win over `sdk` definitions), override libc leaf functions by
> symbol, and keep SVCs as a logging backstop. Do not lift `rtld`. C++ exceptions and `longjmp` use the guest
> unwinder plus return-propagated resume (docs/runtime-boundary.md).

Top three reasons:

1. **The guest-layout problem disappears.** All 731 imports except 6 resolve to `sdk` exports
   (`main_imports_in_sdk.tsv`). The 167 libc++ and 133 libc imports operate on libc++ and musl objects whose layout is
   already compiled inline into `main`, and 17 of the imports are data (`stdin/stdout/stderr`, libc++ vtables and
   `typeinfo` vtables used by exception matching). Lifted `sdk` code gets `std::string`, `ios_base`, `FILE`, `tm`,
   32-bit `wchar_t`, AAPCS64 `va_list` and `%ld` right automatically, because it is the same code that built those objects.
2. **Exceptions run the real unwinder.** `sdk` contains LLVM libunwind and libc++abi (`_Unwind_RaiseException`
   0x512fa0, `__gxx_personality_v0` 0x50ec70, `__cxa_throw` 0x50e5c0, `unw_*` API). The lifted unwinder walks the
   guest stack that lifted code already maintains, reads the real `.eh_frame`/LSDA data, and does type matching
   against guest `typeinfo`. It needs two host hooks: the register-restore leaf and the FDE lookup.
3. **Both precedents do this and the boundary stays small.** TotK lifts `main`, `sdk` and `subsdk0`, drops `rtld`,
   and overrides about 163 `nn::` functions by symbol (`tools/ref/totk/docs/architecture.md:13,32,51`,
   `runtime/host.h:110`). LAS lifts `main` and `sdk` and host-replaces libc leaves for speed
   (`tools/ref/las/docs/architecture.md:12,32`, `runtime/hle/libc.c:1-5`). For Snap, the libc/libc++ side of `sdk`
   reaches only 43 `nn::` entry points by direct calls, and 31 of them are imported by `main` anyway
   (`runtime_frontier_nn.tsv`). The current lifter handles every `sdk` instruction: 0 unsupported of 1,373,866 words.

## 1. What `sdk` contains

| item | value |
|---|---|
| `.text` | 5,666,272 bytes (5.40 MiB; `main` is 10.47 MiB), `.plt` 135,792 bytes (8,487 stubs) |
| `.data` / `.bss` | 324,656 / 683,832 bytes |
| exports | 26,488 defined dynsyms, 22,788 functions; 13 undefined |
| functions | 28,193 FDEs (551 with an LSDA, all `__gxx_personality_v0`); 28,278 starts incl. BL targets and exports |
| undefined | `malloc calloc realloc free aligned_alloc` (defined in `main`), `nnMain`, `nninitStartup`, `__rel_*` x4, `__nnDetailNintendoSdkRuntimeObjectFile`, `nnosGetDefaultUserExceptionStackForApplication` |
| instruction coverage | 0 of 1,373,866 words unsupported by `recomp/lifter.py` (FDE ranges plus the SVC stub table) |
| system registers | `mrs tpidrro_el0` 60, `tpidr_el0` 2 (+3 `msr`), `cntpct_el0` 33, `ctr_el0` 4, FPCR/FPSR 11, NZCV 2, `dc`/`ic` 5 |

Exported functions by class (`sdk_exports_by_class.tsv`, top): `nn::sf` (IPC framework) 4,305; libc 2,506;
libc++ 1,457; `nn::audio` 1,410; `nn` C API 1,345; `nn::fs` 1,137; `nn::hid` 845; `nn::mii` 833; Android-derived
code 829; `nn::settings` 570; `nn::codec` 495; `nn::os` 432; libc++abi 44; libunwind 18; `nvn` 4 (the NVN driver itself
is internal, reached only through `nvnBootstrapLoader`).

Because `sdk` has default-visibility exports and calls even its own exports through its PLT (8,487 stubs for 13
undefined symbols), a by-name override replaces every caller in both modules, not only `main`'s.

**Import resolution.** 725 of 731 `main` imports resolve to `sdk`. The 6 that do not: `__rel_dyn_start/end`,
`__rel_plt_start/end` (rtld data), the weak `thread_local` init hook for `vdk_compute::CCppCompute::s_randomSeed`, and
the weak `PageHeapFreeHook`.

**Where `nn::` services live.** Most services are client stubs over IPC: `sdk` holds `nn::sf` CMIF/HIPC proxies and
the names of 52 services (`sm:`, `fsp-srv`, `hid`, `appletOE`, `audren:u`, `nvdrv`, `vi:u`, `time:u`, `acc:u0`,
`set`, `bsd:u`, `ssl`, `prepo:u`, `nifm:u`, `caps:su`, ...; `sdk_census.json`). Some are user-mode software
whose direct-call closure reaches no IPC SVC: `nn::fontll` (324 functions), `nn::image` JPEG (162), `nn::util`,
`nn::mem`, `nn::diag`, `nn::err` UI args, `nn::swkbd` arg building (`per_service_closure.tsv`). Under this design
fontll and JPEG, listed as "hard" in AGENTS.md, are lifted instead of rewritten.

**SVCs.** `sdk` has 61 distinct SVC immediates, each at exactly one site, all in one unexported stub table at
`sdk+0x399c18..0x399fa8` (`sdk_svcs.tsv`):
`0x01-0x1D, 0x1F, 0x21-0x23, 0x25-0x2A, 0x2C, 0x2D, 0x2F, 0x32-0x36, 0x40, 0x41, 0x43-0x45, 0x48, 0x49, 0x4B,
0x50-0x53, 0x70, 0x72`. Boundary size by design:

| boundary | SVCs | other surface |
|---|---|---|
| pure SVC/IPC HLE (LAS-style) | 61 | IPC servers for up to 52 services, including `nvdrv` ioctls (would run the guest NVN driver, which AGENTS.md forbids) |
| direct-call closure of all 731 imports | 43 | as above |
| libc/libc++/abi/unwind imports only | 19 | 181 `nn::`/`nv` functions in the closure |
| **recommended: `nn::` overrides at the runtime frontier** | **0 required** | 43 frontier functions (31 already imported by `main`, 12 new: `nn::ro::detail::GetExceptionInfo`, `nn::os::SdkAllocateTlsSlot`, `nn::fs::ReadDirectory`, `nn::fs::GetDirectoryEntryCount`, `nn::socket::Write`, 7 `nn`/`nnos` C entry points) |

So the HLE list is `main`'s 413 `nn` imports plus 12, minus the ones left lifted because they are pure software.
The SVC table stays as a backstop that logs the SVC and caller and stops. Expect it to grow a short list like TotK's
38 SVCs, of which TotK marks 21 fatal (`tools/ref/totk/runtime/svc.c:1-5,42-62`). All counts are direct calls only
(lower bounds): virtual calls, `FILE` callbacks and `std::function` are not followed.

## 2. C++ exceptions

### What the references do

| project | throws through lifted code? (evidence) |
|---|---|
| TotK | No support. `ret` is a C `return` (`tools/ref/totk/recomp/gen.py:429-430,481-482`); a branch to a non-entry address is fatal (`runtime/dispatch.c:179-188`). Only ExitThread leaves lifted frames, via a custom non-unwinding jump (`runtime/kernel.c:1502-1505`, `runtime/platform/platform.h:66-80`), because SEH unwinding through lifted frames and `__builtin_setjmp` on Win64 are both unsafe there. |
| LAS | No support. Same `ret` lowering (`tools/ref/las/tools/gen.py:351-352,473-474`); fatal dispatch miss (`runtime/dispatch.c:158`). `setjmp`/`longjmp` only for reboot and ExitThread (`runtime/kernel.c:177-197,691`). |
| Kirikae | Yes, generically. `Return` sets `pc = x30` and leaves (`crates/recomp/src/clif.rs:1284-1289`). After every call, `after_call` compares `pc` with the expected return address and, if different, re-enters `run_until(ret, sp)` (`clif.rs:1183-1198`, `crates/rt/src/process.rs:414-457`), which runs native blocks or the interpreter until the frame is back. The guest unwinder's jump to a landing pad is just a "wrong return". |

Neither TotK nor LAS has any mechanism for a throw through lifted frames. Kirikae's works but needs an interpreter fallback and
runs landing pads on a deeper host stack.

### Snap's numbers

From `main`'s `.eh_frame` (`main_landing_pads.tsv`): 24,601 FDEs, 3,740 with an LSDA, all `__gxx_personality_v0`.
Those 3,740 functions have 21,719 call sites with a landing pad and 20,191 distinct pads; 1,355 functions have a
catch or filter action (this includes clang's noexcept terminate handlers). Imports: 504 `__cxa_throw` and 4,274
`_Unwind_Resume` call sites. `main` has 136,282 `bl` and 35,856 `blr`.

### Choice: guest unwinder plus return-propagated resume

The lifted libunwind does phase 1 (search) and phase 2 (cleanup) in guest code over guest memory. Only the final
"install context and jump" changes. That is the jump from `_Unwind_RaiseException`, `_Unwind_Resume`, `unw_resume`
and `longjmp`. Instead of jumping, it records the target and returns; every lifted frame returns until the target
frame catches it.

1. **The guest stack is real.** Lifted prologues store x19-x30 and FP registers to guest stack memory, and every
   lifted `bl`/`blr` sets `c->x[30]` to the guest return address (`recomp/gen.py:410,412,469`). `unw_getcontext`
   (`sdk+0x513824`) is lifted, so the context it captures is the true guest context.
2. **Host hook A: register restore.** The libunwind `Registers_arm64::jumpto` leaf is unexported, at
   `sdk+0x513794..0x513824`. It loads x2-x29 from `[x0+0x10..]`, sp from `+0xf8`, pc into x30 from `+0x100`,
   d0-d31 from `+0x110..`, then x0/x1 and `ret`. Replace it by address with a host function that copies that block into
   `Cpu` and sets `c->resuming = 1; c->pc = pc;`, then returns. `sdk` is identical in all five revisions, so the
   address is stable.
3. **Host hook B: FDE lookup.** The unwinder finds unwind sections through `nn::ro::detail::GetExceptionInfo`, which
   normally asks rtld. The host loader answers with the `.eh_frame_hdr`/`.eh_frame` ranges of `main` or `sdk` for the
   given pc. The `ExceptionInfo` layout must be read from its callers; this is still open.
4. **The generator adds a check after every call site.**
   - Functions without pads: `if (__builtin_expect(c->resuming, 0)) return;`
   - Call sites covered by an LSDA pad P (from the call-site table; LPStart defaults to the function start):
     ```c
     { uint64_t sp0 = c->sp; c->x[30] = RET; f_callee(c);
       if (__builtin_expect(c->resuming, 0)) {
           if (c->pc == P && c->sp == sp0) { c->resuming = 0; RELOAD_ALL_REGS; goto L_P; }
           return;
       } }
     ```
     The restored sp equals the caller's sp at the call (the callee's CFA), so `sp0` tells recursive instances of the
     same function apart. `RELOAD_ALL_REGS` reloads every cached register, including x19-x29, from `Cpu`, because the
     unwinder restored callee-saved registers there and set x0 (exception object) and x1 (selector).
   - Pads are block leaders seeded from the LSDA (20,191 in `main`, plus `sdk`'s 551 LSDA functions), because no
     normal edge reaches them.
   - Tail calls (`musttail`, `br`) need no check: they do not return into the current function.
5. **The cleanup chain works without special cases.** Pad code calls `_Unwind_Resume`, the unwinder continues phase
   2 from that frame, hook A sets `resuming` again, and the pad's own post-call check returns outward. A catch pad
   calls `__cxa_begin_catch`/`__cxa_end_catch` (lifted) and then falls through into normal code in the same C
   function.
6. **Boundaries.** The host entry points that call guest code (thread entry, fiber entry, host callbacks) check
   `resuming` after the call. If it is set, they abort with the target pc and sp: the guest unwinder only jumps to
   frames it found, so a miss there is a lifter bug. A throw through a host HLE frame finds no FDE for the
   host-sentinel return address and ends in `std::terminate`, as a throw through non-unwindable code does on
   hardware. Functions without FDEs (899, and the region `0x21d45c-0x254c40`) behave the same way.

Rejected alternatives:

- **Host SEH translation** (`__try` around call sites, filter matches pc/sp). It costs nothing until a throw, but it
  is Windows-only. It conflicts with `musttail` and register-cached locals, and TotK avoids SEH through lifted
  frames for this reason.
- **A setjmp per function with pads.** That is 3,740 functions, including hot `shared_ptr` cleanups, each paying a
  context save on every entry. TotK also documents that `__builtin_setjmp` is broken on Win64 with an offset frame
  register.
- **Host-reimplemented unwinder or personality (option A).** It would rewrite what `sdk` already contains and still
  need guest `typeinfo` matching.
- **Kirikae-style `pc != ret` check with an interpreter fallback.** It costs about the same per call, but needs an
  interpreter and runs pads on a growing host stack.

Cost: one load, test and not-taken branch after each of about 172k call sites in `main`, and similarly in `sdk`.
Estimate 1-2%, not measured. Prune it with a static may-unwind analysis: drop the check when the callee is a host
override leaf (`memcpy` and friends) or provably reaches no `_Unwind_RaiseException`, `_Unwind_Resume`,
`__cxa_rethrow`, `longjmp` or indirect call.

## 3. `setjmp` / `longjmp`

`main` calls `setjmp` once (`0x57be8` in `0x57bb0`, Lua `luaD_rawrunprotected`) and `longjmp` once (`0x57abc` in
the noreturn `0x57a30`, Lua `luaD_throw`). In `sdk`, `setjmp`/`_setjmp`/`__setjmp` are at `0x51d5c4` and
`longjmp`/`_longjmp` at `0x51d584`, as assembly that ends in `ret x30`, which the C `return` lowering cannot express.

- Host-override `setjmp`: store x19-x30, sp and d8-d15 (22 x 8 bytes, the musl AArch64 `jmp_buf` layout) into the
  guest buffer, return 0.
- Host-override `longjmp`: load them into `Cpu`, set `x0 = val ? val : 1`, `c->pc =` saved x30 (the return address
  of the `bl setjmp`), `c->resuming = 1`, return.
- The generator treats the instruction after each `bl setjmp` as a resume point, the same as a landing pad (pc and sp
  match, reload all registers, `goto`). Frames in between return without running guest code, which matches
  `longjmp` semantics because no cleanups run.
- The libpng `setjmp`/`longjmp` inside `sdk` (`nndetailImagePng_*`) is covered by the same rule if `nn::image` PNG
  is ever reached.

## 4. Costs, work and risk to first boot

Hot paths: under any option, libc leaves can be overridden by symbol. LAS measured the lifted SDK byte loops at about
3% of CPU (`tools/ref/las/runtime/hle/libc.c:1-3`). Override `memcpy memmove memset memcmp bcmp memchr strlen strcmp
strncmp strchr ...` with exact-result host versions (comparisons return the byte difference, as the SDK's do). Keep
math functions lifted, or build musl's math sources (MIT) for the host, so results stay bit-identical; do not
forward to the MSVC CRT. `malloc` and friends are defined in `main` (`0x4e2f60..0x4e3e90`) over `nn::mem::StandardAllocator`
in `sdk`. Lift both first and override them only if profiling says so.

| | A: host libc + libc++ | **B: lift `sdk` runtime, `nn::` HLE** | C: lift libc++/abi/unwind, host libc |
|---|---|---|---|
| guest layouts | reimplement libc++ v1 LP64 layouts on an LLP64 host (`std::string`, iostreams with exported vtables/VTTs, locale ids, `shared_ptr`, `exception_ptr`) plus musl `FILE`/`tm`/`va_list`/printf | free | libc++ free; musl `FILE`, `tm`, `va_list`, printf `%ld`, 32-bit `wchar_t` still hand-written |
| exceptions | write a guest-frame DWARF unwinder, personality and guest-`typeinfo` matcher | lifted; 2 host hooks + resume checks | same as B |
| `setjmp` | host, same resume checks | host, same resume checks | host, same resume checks |
| generated code | `main` only | `main` + 5.40 MiB `sdk` (about +52% C and build time; `sdk` is identical across revisions, so build it once and cache it) | `main` + about 0.2 MiB |
| work to first boot (CPU/runtime side, estimate) | 3-4 months | 3-5 weeks: multi-module load and bind, rtld-style init order, resume checks and pad seeding, `nn::os` with guest `ThreadType`/TLS, `nn::fs`, 2 unwinder hooks | 6-8 weeks |
| risk to first boot | high: boot ranks 12-43 already hit `std::string`, `mutex`, `ios_base::init`, `__cxa_guard`, `__cxa_throw` | medium (below) | medium-high: the libc ABI surface remains and libc++ calls into it |

Risks for B, and their mitigations:

- **Build size and time:** about +52% code. Build and cache `sdk` separately; it never changes.
- **Partial HLE:** an override at an export does not catch `sdk`-internal direct `bl` to unexported code. Override
  at the exports `main` and the runtime frontier call. Let any remaining kernel or IPC path hit the logging SVC
  backstop, which names the service and caller, then add an override (TotK's loop).
- **`nn::os` object layouts in guest memory:** musl pthread and libc++ mutexes wrap `nn::os::MutexType`/`ThreadType`,
  and `sdk` reads TLS through `tpidrro_el0` (60 sites). The host thread layer must keep guest-layout `ThreadType` and a
  guest TLS block, as TotK does. Option A needs this too.
- **Static init order:** `sdk` has 111 `init_array` entries. The host loader must run `sdk` and `main` initializers
  in rtld's order.
- **Exception edge cases:** `GetExceptionInfo` layout unconfirmed; a throw across host frames terminates; the
  per-call check overhead is not measured yet. First test: a throw and catch inside `main` and a Lua error
  (`longjmp`) under the differential harness.

## Data files (`analysis/runtime/boundary/`)

- `sdk_census.json`: every number above.
- `main_imports_in_sdk.tsv`: each `main` import with its `sdk` offset, size and class.
- `sdk_exports_by_class.tsv`: export counts by namespace.
- `sdk_svcs.tsv`: 61 SVCs and their reachability.
- `per_service_closure.tsv`: closure size, SVCs and IPC use per imported service.
- `runtime_frontier_nn.tsv`: the 43 `nn::` override points under the runtime lift.
- `runtime_closure_nn_calls.tsv`: all 181 `nn::`/`nv` functions in that closure.
- `main_landing_pads.tsv`: the 3,740 functions with pads.
