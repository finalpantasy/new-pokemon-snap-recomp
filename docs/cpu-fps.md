# CPU cost of the lifted code at high frame rates (branch cpufps, 2026-11-23)

Goal: more uncapped simulation fps (pass 19 in `analysis/perf/results.md`: 192 fps, 1% low 94, CPU-bound, about 72% of
the critical path in lifted guest code) without changing what the game computes. Defaults stay 60 fps. Game timing and
UI pacing code are untouched.

## What changed

SIMD&FP registers now live in C locals. Before, every V register access in lifted code went through the `Cpu` struct
(`c->v[n]`). The guest-memory macros use may_alias pointers, so after every guest store the compiler had to reload each
V register from `Cpu`. Matrix, vector and audio code is mostly V register traffic, so it paid this on nearly every
instruction.

- `recomp/regs.py`: `vregs(w)` decodes, from the instruction word, which V registers an instruction uses, writes, reads
  and fully overwrites. `vliveness()` is a backward liveness pass over the function. The register plan gives each chosen
  function a local `u128 vl_[32]`. The function loads only the V registers live at entry, stores them back at every sync
  point (call, SVC, return, tail exit, trap, hook), and after a call reloads only the ones still live.
- `recomp/gen.py`: picks the functions. A function qualifies when it has at least 4 SIMD&FP instructions, at least 2 of
  them per call it makes, and names at most 20 V registers. With more than 20, the 16 host vector registers spill to the
  stack and lose to the `Cpu` copy. Functions with patched sites that touch V registers are skipped. `check_vsets()`
  stops generation if the emitted C names a V register that the decoder did not report, so a decoder gap cannot turn
  into a silent miscompile.
- `runtime/simd.h`: every `c->v[` became `VB_[`, a macro that is `(c->v)` by default and `vl_` inside a function with V
  locals. The 21 helpers that take register numbers (`fp_op_s`, `sv_tbl`, `sl_ldm` and the rest) gained a `u128 *VB_`
  parameter behind a same-name wrapper macro, so lifted call sites are unchanged.
- GPR plan: a wider plan (keep callee-saved registers, all registers in leaf functions) is in `regs.py` behind
  `SNAP_GEN_REGS=<extra_kept>,<leaf_all>,<min>,<max>`. It measured neutral and slowed the hierarchy loop about 2%, so it
  is off by default (`0,0,2,0` = the old plan).
- Generator knobs, for experiments only: `SNAP_GEN_VREGS=<on>,<min>,<per call>,<max>` (default `1,4,2,20`, `0` = old
  output).
- `tests/snap_func.py`: the subset link no longer fails on tail exits into functions outside the subset (12 undefined
  symbols on origin/main).

Result: 1,095 main functions and 811 SDK functions use V locals. Rejected: fusing atomics (small gain), changes to
`rt_dispatch` (it already uses a direct table).

## Correctness

The output of every changed function must match the old output bit for bit, including NaN payloads and FPCR modes.

- Hot-function differential test (`tests/hotfn/run_hotfn_test.py`): lifted against native reference over 6 FPCR modes,
  0 mismatches.
- `tests/snap_func.py` (lifted code against the Unicorn AArch64 emulator, 435 picked functions, 3,791 cases): 394 pass,
  41 untested, 0 failing cases. This matches the run on unchanged code. 88 of the picked functions use V locals.
- Photo score harness (`tests/photoscore/run_snapshot_diff.py` on capture `run2.psc`): PHOTOSCORE_RESULT
- HLE tests 4,034 checks, 0 failed; CLI flag test passes; mods tests pass.

## Speed

Hot-function bench, time of the lifted function divided by time of the native reference (lower is better):

| Function | Before | After |
|---|---|---|
| MatrixDecompose | 2.55 | 2.03 |
| ScaledMatrixDecompose | 3.27 | 2.32 |
| ReverbFdn4 | 2.94 | 1.42 |
| HierarchyTransform | 2.05 | 2.08 |
| Bone loop 1 | 2.98 | 2.52 |
| Bone loop 2 | 2.66 | 2.52 |

In-game fps: TIMING_RESULT

## Shipping it

The change only takes effect in regenerated code. To ship it:

1. Regenerate main and the SDK.
2. Bump `SNAP_RUNTIME_ABI`. `Cpu` and the helper semantics are unchanged, but the new generated code needs the new
   `simd.h`.
3. Retrain PGO. Profiles from the old code do not match the new code's IR hashes.
