# Rust fork: paused (2026-11-12)

The owner paused the Rust work until the rest of the game and the mods work. This note is where it stands, so it
can resume later. The full record is the "Rust fork" section of `AGENTS.md` on branch `rust-fork` and `docs/rust-fork.md`.

## What exists

- Branch `rust-fork`, worktree `F:\AI\Recomps\new-pokemon-snap-rust`. It is not pushed and the tree is clean;
  the last commit is 079eb74.
- The whole recompilation in Rust:
  - `snap-gen` is the lifter and crate writer. Its output is byte-identical to `recomp/rs_gen.py`.
  - `snap-cpu` is the guest CPU runtime, with an SIMD/FP slow path and FZ-safe compares.
  - Runtime crates: `snap-os`, `snap-hle`, `snap-audio`, `snap-nvn` (ash) and `snap-boot` (loader, exceptions,
    setjmp).
- Function differential tests: `tests/func_inputs.json` holds hand-written inputs for about 160 functions, main and
  sdk. `tests/snap_func.py` loads it with `--inputs`. Mainline can use the same file through
  `rust-fork-func-inputs.mainline.patch` in this folder.

## What builds and runs

- **Toolchain:** nightly-2026-04-01.
- **Cold build:** about 7 minutes in total at 12 jobs (main 257 s, sdk 113 s, boot 61 s).
- **Boot:** `host/run_rs_boot.ps1` reaches the camera tutorial with 0 NVN errors. That is the same point mainline's
  `-SkipIntro` reaches.
- **Speed:** Rust runs at C speed (geomean 0.985 of C's cycles over 394 functions).
- **Function tests:** every seed gives identical results for C and Rust at O2, with 0 failing cases and no mainline
  bug. Recent results:

  | Seed | Pass | Cases |
  |---|---|---|
  | main 7 | 423/434 | 4,306 |
  | sdk 5 | 362/384 | 3,711 (up from 359) |

## Next, when resumed

1. Record the sdk seed-5 result in AGENTS.md (`build/sd5f/`). It was not recorded because of the pause.
2. Main seed 8 baseline: `build/heavy.ps1 ... build/batch_seed.ps1 -Opt 2 -Logs build/s8 -Seed 8`. It was started
   and stopped before any result.
3. Port mainline's later features, which the fork doesn't have yet:
   - mod inline hooks `MAIN_INLINE_HOOKS`;
   - window options, icons and profiles;
   - `snap_fn_observe`;
   - lodanim;
   - the ImGui controls menu.
4. Port the C runtime's pending FCMP-under-FZ fix and the `vfix` vector slow path to mainline, or check that it has
   them.
