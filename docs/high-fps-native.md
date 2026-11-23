# Native high frame rates: any rate, unlocked, match monitor (2026-11-23, worker hifps2)

The game simulates every frame natively at the chosen rate: no interpolation and no frame generation. The default stays the
game's own 60 fps, and nothing in this document changes a default run. Earlier work: `docs/high-fps-plan.md`,
`analysis/perf/high-fps.md`, `analysis/perf/fps_audit.md` (local, not in git).

## 1. What the player can pick

| Where | Values |
|---|---|
| F1 menu, Graphics / Frame rate, "Frame rate target" (`fps_target`) | 60 (default), 72, 90, 100, 120, 144, 165, 180, 200, 240, 300, 360, 480, Match monitor, Unlocked; any other saved value shows as "N fps (custom)" |
| Native Settings, Frame rate row (`scripts/ui/make_native_options.py`, 10 entries at most) | default, 90, 120, 144, 165, 180, 240, 360, Match monitor, Unlocked |
| Launcher (`host/launcher_ui.cpp`) | the F1 list |
| Command line | `--fps <n\|unlocked\|monitor\|default>` (`host/cli_flags.h`; n from 61 to 1000, decimals allowed, so 128, 143.988 or 175 work) |
| Environment / settings.cfg | `SNAP_FPS=<same values>`, `fps_target=<same values>` |
| Frame rate limit (`frame_cap`, `SNAP_WINDOW_FRAME_CAP`) | Off, 30, 45, 60, 75, 90, 120, 144, 165, 180, 200, 240, 360 |

Parsing is one function, `highfps::parse` in `host/mods/highfps_core.h`. Accepted aliases: unlocked, uncapped, unlimited and max;
monitor, refresh, display and match. 59.5 to 60.5 counts as the default. Anything else (0, 30, 1001, "144fps") is rejected at
the command line and ignored with a log line from settings.

How the target and the limit combine (`highfps::pace_hz`, applied on every present by `host/mods/highfps.cpp`):

- Fixed rate N: the game runs at N. When a frame rate limit is set below N, the game runs at the limit. Before this change only
  the present limiter used the limit while the game's vsync clock stayed at N; perf pass 21 found this with 144 under a saved
  120 limit. A limit of 60 or less keeps the game's own 60 clock.
- Match monitor: the panel's exact refresh (143.988 Hz here, so the pacer locks to vblank). It falls back to 60 until the
  refresh is known, and stays at 60 on a 60 Hz panel.
- Unlocked: no limiter, and the game's vsync clock does not wait. With a frame rate limit set, Unlocked runs at the limit
  (before this change the limit was ignored when unlocked).
- The change takes effect live, both from F1 and with `SNAP_FPS_AFTER_LOAD` (below).

## 2. Fixes in this change

| Issue | Fix | Kill switch |
|---|---|---|
| Unlocked: menus, dialogue and hint panels ran about 3x fast. The interface pacing read the pacer period, which is 0 when unlocked, so it stepped every frame. | `host/mods/uipace.cpp` reads `nvnHostEffectivePaceHz()`: the pace rate, or the measured present rate when unlocked (runtime/nvn/nvn_vk.cpp `UpdateEffHz`, a 16-present average of frame time) | `SNAP_UI_PACE=0` |
| Uneven frames (3, 3, 3, 40 ms, an unlocked 81 fps) gave 41 to 49 interface steps a second instead of 60. A long frame dropped the time beyond one step. | `uipace_core.h` keeps one extra step of backlog (`kBacklogNs`); a hitch still never banks more than that | none (test `tests/mods/highfps_test.cpp`) |
| The measured rate averaged rates, not frame times. On a loaded PC (bursts of short frames between long ones) it read 165 fps while 30 frames a second were shown. | `UpdateEffHz` averages frame time | none |
| Interface smoothing (`nvn_uismooth.h`) was gated on the pace rate and counted presents at that rate, so it was off when unlocked and wrong when the PC could not hold the rate | gated on the effective rate; the step fraction comes from measured time since the step | `SNAP_UI_SMOOTH=0` |
| A frame rate limit below the target left the game's vsync clock at the target (two clocks, uneven pacing); the limit was ignored when unlocked | `pace_hz` sets one rate for the limiter and the game's clock | none |
| Only 60, 72, 90, 120, 144, 165 and 240 could be chosen | any rate from 61 to 1000, Unlocked, Match monitor; `--fps` | none |

Test aids (off by default): `SNAP_FRAME_SIG=<file>` records a 32x18 luma grid of every presented frame, and
`scripts/hifps/frame_sig.py` finds frames that differ from both neighbours (a one- or two-frame flash).
`SNAP_FPS_AFTER_LOAD=<target>` switches the target after the first save-state load, for the same-photo score check.

## 3. How it was checked

- Unit tests: `tests/mods/highfps_test.cpp` covers parsing, target and limit, and 60 ± 0.6 interface steps a second at 61 to 1000
  fps, steady or with jitter, plus the uneven unlocked case. `tests/mods/uismooth_test.cpp` covers the time-based smoothing,
  including unlocked. `tests/hle/cli_flags_test.cpp` covers `--fps`.
- Route `host/routes/hifps_sweep.txt` drives one Florio ride from the title to the end of the Evaluation. Every input is on wall
  seconds, so the same script presses the same buttons at the same times at every rate. Run it with
  `scripts/hifps/sweep_run.ps1 -Tag <t> -Fps <rate> [-Cap n] [-Vsync on|off]` and read the result with
  `scripts/hifps/sweep_report.py --md`.
- Route `host/routes/hifps_score_ck.txt` checks that the same photos get the same score: the ride at rate A, a save state when
  the film is used up, photo select and the Evaluation, then a load, the switch to rate B (`SNAP_FPS_AFTER_LOAD`), and the same
  picks and Evaluation again. Compare the "shows photo" lines and the Score screenshots of the two passes.
- `scripts/hifps/frame_counter_scan.py` is a static pass for frame-counted logic (counters stepped by one per call and compared
  to a limit). It found 81 counters in 59 functions. The 1/60 literal pass is `analysis/perf/fps_audit_scan.py`.

RESULTS_PLACEHOLDER
