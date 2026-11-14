# High frame rate plan: 120 and 144 fps

Status (2026-11-14): corrected after a critique; M0 (measure) and the first half of WP1 (opt-in SNAP_FPS spike,
`host\mods\highfps.cpp`) are done. Numbers and the go/no-go are in `analysis\perf\high-fps.md`; section 6 below
summarizes them. Default behavior stays 60 fps and byte-identical (the default game is the original base game, natively
ported): every new code path is inert unless SNAP_FPS=120|144 (or a diagnostic switch) is set.
Hard constraints: opt-in (SNAP_* switch plus a native options row), ride timing (rail speed,
spawn timing, scripted events) the same in seconds as at 60 fps, photo scoring equivalent within a stated tolerance (section 3,
test 3; byte-identical photos are not achievable at a different rate), photos always captured from a real simulated frame,
never an interpolated or generated one.
Target hardware: RTX 4090, monitors at 60, 144 (primary) and 120 Hz.

Corrections folded in (critique, 2026-11-14): (1) the pace target already has a public setter,
`nvnHostSetFramePacing(hz)` (runtime\nvn\nvn_entry.cpp, nvn_host.h); no change to nvn_pace.* or nvn_vk.cpp is needed.
(2) The 14 ms sleep floor is clamped at runtime from the OS layer (caller LR 0x71004e1010), no main regenerate.
(3) M0 reuses SNAP_PERF (no new probe). (4) Timing at 120/144 is a hypothesis until measured in seconds. (5) The integer
ms channels are not "fixed" until their readers are known. (6) Photo acceptance is tolerance based. (7) Hidden runs cannot
show display cadence.

## 1. How the game keeps time today

- The game's main loop (main+0x4df460, body main+0x4e0f64..0x4e1018) reads the clock once per
  iteration, calls the active state function, bumps a frame counter, and sleeps if the iteration
  took under 14 ms. That 14 ms sleep floor alone would cap the loop near 71 fps.
- Delta time is measured, not fixed. The clock object (updated by 0x6abea0) converts
  GetSystemTick to microseconds and takes the difference since the last update. If the gap is
  under 0.5 s, dt is that gap in seconds; otherwise dt falls back to exactly 1/60 s (so a
  big hitch plays as slow motion). The clock has three identical channels (float dt at +0x1c/+0x34/+0x4c plus an
  integer millisecond value at +0x10/+0x28/+0x40, each floor(delta_us/1000)). A fixed-step mode exists in the clock
  structure but nothing selects it; a runtime dump of 400 boot updates showed mode 0 and dt between 7 and 43 ms.
  That looks like a contradiction with the 14 ms floor; it is explained in `analysis\perf\high-fps.md` ("Clock callers
  and the dt 7-43 ms dump"): the dump was boot/load frames, and the floor only sleeps when the iteration was shorter than
  14 ms. Do not read the floor as a guarantee of dt >= 14 ms.
- Rail speed, camera tween, camera velocity, event timers and VFX stepping all consume dt.
  Photo scoring reads positions and poses, not time. Earlier florio_eval runs took the same
  47 s for leg 1 at 39, 47 and 51 presents per second, and a hidden run with guest vsync at
  120 Hz did not change ride length in seconds. So ride and spawn timing in seconds do not
  depend on frame rate. The game already supports a higher rate in principle.
- What actually pins the game at 60: the host limiter (g_paceHz = 60 in nvn_vk.cpp, because
  the game asks for present interval 1) and the 14 ms sleep floor. Guest vsync is not what
  drives the sim; it comes from the same nvn_pace clock and the game keeps a counter that
  nothing reads.
- nvn_pace: the limiter and the guest vsync event share one clock. It locks to real vblanks
  when the display refresh is within 0.6% of a whole multiple of the target rate, otherwise it
  uses a steady grid. Today at 120 Hz the target is 60, so k=2 locked (guest sees 60 Hz vsync
  every second vblank). 144 Hz runs a steady grid.
- Things driven by frame count, not time: distant animation (host\mods\lodanim.cpp rebuilds LOD1
  every 2nd and LOD2 every 4th frame, so 30/15 Hz at 60 fps). At a higher rate these run faster
  in real time, which is fine visually.
- Things independent of the sim: audio (own thread, own tick measurement).
- Unknowns that need measurement, not guesses: CPU cost per frame uncapped (the guest thread
  takes about 17 ms of CPU per frame today, so 144 needs 6.9 ms), which readers use the
  integer millisecond channels (floor(delta_us/1000) runs 4% slow at 120 and 13.6% slow at
  144 versus 4% at 60), and whether any system accumulates frame counts as time.

## 2. Ranked approaches

Rank is for meeting the goal (real 120+ fps) under the hard constraints.

### 1. True simulation rate (recommended). Effort about 3 days to first result, up to 7 with CPU work

Raise the present cap and the pace target to 120 or 144, remove or shrink the 14 ms sleep floor,
fix the integer-millisecond rounding, leave dt measured. This is AGENTS.md's stated goal.
- Quality: best. Input, camera and animation really run at the rate, latency drops.
- Why timing survives: dt is measured, so ride length in seconds is unchanged.
- Risks: (a) CPU may not reach 144, unmeasured; (b) integer ms timers run slow at 144;
  (c) the 1/60 hitch fallback becomes 8.3% slow motion at 120 (and 13.6% at 144 if it stays 1/60,
  so make it 1/target); (d) frame-count logic anywhere else in the 600+ lifted dt consumers;
  (e) scoring is pose based, so it should match, but poses at shutter time differ slightly
  between rates, so "identical" must be defined as a tolerance, see section 3;
  (f) the sleep floor is clamped at runtime (OS layer, caller-LR check), so no main regenerate is needed;
  (g) routes and the upscaler count presents (route `@f` times, shutter hold 6 and 2 presents), so presents-based
  timing runs 2x faster in seconds at 120.

### 2. Render interpolation (simulate at 60, present 120/144). Effort 5 days camera-only; 3 to 6 weeks full

Keep the sim and guest vsync at 60, re-run the real frame's command lists with an interpolated
camera (c3 buffer) and, in later tiers, interpolated per-object matrices (c4) and a skinned vertex
lerp. Photos and scoring are identical by construction.
- Quality: camera-only makes the world, sky and terrain smooth; Pokemon and particles stay at
  60 Hz steps. Full tier looks very good but needs draw matching across frames.
- Costs: about 4 ms of extra CPU replay per output frame on the guest thread (CPU bound), about
  1 sim frame added latency, input stays at 60 Hz.
- Risks: unverified draw identity keys, present decoupling in nvn_pace, side effects that must not
  repeat (counters, readbacks, photo passes), temporal upscaler history.
- Role: the fallback if approach 1 cannot hold 144 on the CPU, and the only way to deliver 144
  smoothness if the game thread cannot go faster than about 100 fps.

### 3. Frame generation (separate opt-in, never counted as real fps). Effort 7 to 12 days

FSR 3.1 frame generation via the MIT FFX API with our own presenter (best first backend), DLSS-G
through Streamline later (proprietary, user-supplied, local only), or our own reprojection shader
as a fallback. 2x only on this GPU: 60 becomes 120, not 144.
- Quality: good on the static world and camera moves, weaker on Pokemon and particles without
  object motion vectors. About 1 base frame of added latency.
- Photo safety: generated frames exist only in swapchain images. The game reads its own window
  texture for the shutter, and saved photos come from the guest's JPEG encoder buffer. Also reuse
  the shutter hold in the upscaler so no generated frame flashes at the shutter.
- Risks: needs a HUD-less color capture at the first UI draw (new detector, about 2 to 3 days,
  shared by all backends), a presenter thread decoupled from the game thread, hitches get doubled.
- Label it as generated, report real simulation fps separately.

### 4. Driver-level smoothing (NVIDIA Smooth Motion, AFMF). Effort 0.5 day, docs only

Zero integration. Lowest quality (interpolates HUD and fades), not controllable. Mention in docs.

### Not recommended: decoupled 60 Hz sim plus per-object pose interpolation in the game code

About 15 days and unmapped hook points; approach 2 gives the same visible result from the
render side with fewer unknowns.

## 3. Recommended path and acceptance tests

Order: measure first, then real rate, then interpolation only if the numbers demand it, then
frame generation as an independent add-on.

### Milestone 0: measure (1 to 2 days)

- No new probe. SNAP_PERF (or SNAP_PERF_SHARED on a shared PC) already writes per-frame CSVs under analysis\perf
  (dt_ms, pace_ms, cpu_mcycles, replay_ms, poolsync_ms, gpu_busy_ms). Run with the sleep clamp on and pacing off
  (SNAP_FPS=120 SNAP_FPS_UNCAPPED=1) at 1440p and 4K and read the real uncapped fps and CPU ms per frame. Replay alone is
  3.6 to 6.8 ms mean and 144 fps has a 6.9 ms budget, so the report must say whether real 120/144 simulation is reachable or
  whether replay and pool sync must first move off the game thread.
- Clock diagnostics only (SNAP_FPS_TRACE, SNAP_FPS_MSWATCH in highfps.cpp, off by default): log callers of the clock update
  f_71006abea0, the mode, dt and ms channels, and enumerate readers of clock+0x10, +0x28, +0x40 with hardware watchpoints.
- Baseline variance: run florio_eval at 60 several times and record the spread of leg-1 seconds,
  score and the Dodrio photo, so "identical" has a measured tolerance.
- Exit: a table of uncapped fps and per-frame cost. Decision: if uncapped rate at 1440p is at or
  above 150, go for approach 1 fully. If between 100 and 150, approach 1 for 120 and approach 2 for
  144. Below 100, approach 2 becomes the main path.

### Milestone 1: real 120/144 simulation (3 to 5 days)

- SNAP_FPS=60|120|144 (default 60; 60 is inert). `host\mods\highfps.cpp` calls the existing
  `nvnHostSetFramePacing(hz)` from the present hook, and re-applies it right after the SNAP_FAST_BOOT window (window_opts.cpp
  fast_boot_step hard-codes 60 there; window_opts.cpp is not edited). Do not edit nvn_pace.* or nvn_vk.cpp (pacing worker).
- Sleep floor: clamp at runtime, no main regenerate. The loop's sleep call (main_0060.c ~19439, x30 = 0x71004e1010, through PLT
  stub f_7100a36ad0 into ov_SleepThread, runtime\os\os_thread.cpp) goes through `snap_highfps_sleep_ns(lr, ns)`, which shortens it
  only for that caller and only when SNAP_FPS is set (default floor 0, SNAP_FPS_FLOOR_MS to change). OS-layer change: 3 lines.
- Integer ms channels (clock+0x10, +0x28, +0x40, floor(us/1000), already about 4% slow at 60): do NOT carry the sub-ms remainder
  until the readers are known (rail, spawn and event timers may or may not read them). The reader enumeration and the decision
  are recorded in `analysis\perf\high-fps.md`. The shared 1/60 rodata constant at 0x7100ae7e4c must not be patched (many users).
  The hitch fallback (gap over 0.5 s plays as exactly 1/60 s) stays unchanged for now; at 120 it is a slow-motion hitch, not a drift.
- Default path stays byte-identical: with SNAP_FPS unset no hook changes behavior (the OS-layer hook returns its argument).
- Acceptance tests:
  0. Timing at 120/144 is a hypothesis: measure ride length in seconds and spawn timing at SNAP_FPS=120 vs 60 first. Routes
     count presents (`@f`), so they run 2x faster in seconds at 120: use the rail/game-time route timing (host\mods\routes,
     input_core.cpp, secrets worker) when it lands, otherwise time checks in wall seconds and say so.
  1. Ride length: florio_eval leg 1 and full-ride seconds at 120 and 144 within the measured 60 fps
     spread (plan for 0.3 s or 1%, whichever is larger) of 60 fps; rail restart progress values equal.
  2. Spawn timing: the logged spawn and scripted-event timestamps (in seconds from ride start)
     within the same tolerance of the 60 fps run, event order identical.
  3. Photos and score (cannot be byte-identical at another rate): over N photos (N at least 5, same shutter script or same
     rail points) require the same subjects, the same pose bucket and the same score tier, with per-field deltas (score
     points, pose fields, size and position share) under a stated bound (start: score within 5%, other fields within one
     bucket; tighten after the 60 fps repeat spread is measured). Any outside the bound is investigated, not tolerated.
     Also report the upscaler's `lateHold` and `shutterHolds` counters (upscale.cpp counts presents: hold 6 on shutter, 2
     after the photo pass) at 120 vs 60; do not edit upscale.cpp.
  4. Photos from real frames: each saved photo's source frame number appears in the sim frame log
     and is a real sim frame; with SNAP_FPS_MODE=sim there are no generated frames at all.
  5. (Needs a visible run: hidden runs never block on acquire, so display cadence, vsync lock and judder cannot be seen.)
     1% lows at 1440p and 4K on the 4090: at least 115 fps (120 target) and at least 138 fps
     (144 target) for the 1% low, measured with SNAP_SAMPLE over the whole ride, on the 144 Hz
     primary (k=1 locked) and the 120 Hz monitor. 120 on the 144 Hz monitor without VRR judders
     (2/3 vblank); document, do not hide.
  6. Vulkan validation clean (no new errors or warnings versus 60 fps run).
  7. Default unchanged: SNAP_FPS unset gives a run identical to today (present count and timing
     within noise, same log hashes for deterministic outputs).

### Milestone 2: render interpolation, camera-only (5 to 7 days). Only if Milestone 0 says so

- SNAP_FPS_MODE=interp with SNAP_FPS=120|144: sim at 60, interpolated frames re-run the real
  frame with an interpolated camera into a spare window-sized image, presented between real
  frames. Alpha comes from the display clock (alternating 2 and 3 outputs per sim frame at 144).
- Acceptance: tests 1 to 3 pass trivially (the sim is identical to 60 fps; verify the sim fps log
  reads 60); test 4: photo submits and the frame after are never interpolated and the saved
  photo hash equals the 60 fps hash for the same shutter point; 1% lows measured on the
  presented rate and the sim rate separately; camera cuts show no blend (check cut frames in a
  logged trace); validation clean; guest thread stays at or under 16.6 ms with the extra replay.
- Later tiers (objects, skinned lerp, GPU re-submit) are a separate decision after M2 numbers.

### Milestone 3: frame generation opt-in (10 to 14 days). Independent, can run in parallel

- SNAP_FRAMEGEN=off|fsr (default off), 2x over a 60 Hz base. HUD-less capture detector,
  presenter thread, FFX dispatch, UI composite on top, shutter hold, history reset on cuts.
- Acceptance: photos byte-identical in the shutter test with FG on versus off; no generated
  frame visible within 6 presents after the shutter; overlay shows "sim 60 / output 120 (generated)";
  1% low of output at 1440p and 4K at 4090 is at least 115; validation clean; FG off path
  unchanged; 144 not claimed.

## 4. Work packages (non-overlapping file sets)

File names marked "(new)" do not exist yet. Shared files are listed once with a single owner.
Existing workers' files: nvn_pace (pace worker), nvn_draw, nvn_entry (nvn layer worker),
services_sys vsync (HLE worker). Anyone touching those must coordinate through the owner.

| Id | Title | Files (owner) | Existing workers' files touched | Days | Depends on |
|----|-------|---------------|----------------------------------|------|------------|
| WP0 | Probe and baseline harness | host\run_boot.ps1 routes option (no change to default), tools\hifps_check.py (new), analysis\perf\high-fps.md (new, results) | none | 2 | none |
| WP1 | Pace target and sleep floor | host\mods\highfps.cpp (done: SNAP_FPS, setter call, sleep clamp, diagnostics), one-line hooks in host\boot.cpp (present), host\loader.cpp (fn observe), runtime\os\os_thread.cpp (ov_SleepThread). No change to nvn_pace.* / nvn_vk.cpp / window_opts.cpp. Later half: ms remainder (only after readers known), hitch fallback | os_thread.cpp (OS worker, 3 lines) | 3 | WP0 |
| WP2 | Options rows and settings | host\window_opts.cpp, native options UI source (the file that holds the current rows), settings key table | none | 1 | WP1 (key names) |
| WP3 | Frame-count consumers | host\mods\lodanim.cpp (SNAP_FULL_RATE_ANIM tied to rate, optional time-based rebuild), audit list of dt and frame-count readers in analysis\perf\high-fps.md | none | 2 | WP0 |
| WP4 | Camera-only interpolation | runtime\nvn\nvn_interp.cpp and .h (new), BindUbo hook in runtime\nvn\nvn_draw.cpp, spare-image retarget in runtime\nvn\nvn_entry.cpp, present decoupling API in nvn_pace (small, coordinated with WP1) | nvn_draw, nvn_entry, nvn_pace | 7 | WP1, WP2 |
| WP5 | Presenter thread | runtime\nvn\nvn_presenter.cpp and .h (new), PresentWindowOnce in runtime\nvn\nvn_vk.cpp, guest vsync wiring in runtime\hle\services_sys.cpp (vsync source only) | nvn_pace, services_sys vsync, nvn_vk.cpp present | 5 | WP1 |
| WP6 | Frame generation opt-in | runtime\nvn\framegen\ (new directory: ffx backend, HUD-less capture, composite), shutter hold hook in runtime\nvn\upscale\upscale.cpp, headers in tools\amd (local only) | upscale (not in the worker list above), nvn_vk.cpp present (via WP5 API) | 12 | WP5 |
| WP7 | Acceptance and docs | docs\high-fps-plan.md updates, analysis\perf\high-fps.md, tools\hifps_check.py cases | none | 2 | all |

Notes on overlap: WP1, WP4 and WP5 all touch nvn_pace and nvn_vk.cpp, so they run in sequence
(WP1, then WP5, then WP4 and WP6 in parallel) with one owner per file at a time. WP2 and WP3 are
independent of everything else except key names and can run beside WP1.

## 5. Options rows for the native menu

Default for every row is the current behavior. Each row maps to a SNAP_* switch and a saved
setting; the environment variable wins when set.

1. Frame rate target: 60 (default), 120, 144, Match display. Key SNAP_FPS. Match display
   picks the display refresh rounded to a rate the pace model can lock (or 60 if none).
   The options UI already has "Frame rate limit" (`frame_cap`, SNAP_WINDOW_FRAME_CAP; runtime\ui\ui_options.cpp, owned by the
   options worker). The new row sits beside it: effective rate = min(target, cap) (nvn_vk.cpp already takes max(period,
   1/cap), so a lower cap wins without extra code). Row spec: id `frame_target`, label "Frame rate target", values 60/120/144,
   default 60, env SNAP_FPS wins, saved key `fps_target`, restart not needed once the pace setter is called live (the sleep
   clamp reads the value at start, so the first version needs a restart), help "Higher rates run the game itself faster;
   needs a fast CPU. The frame rate limit below still applies."
2. Frame rate mode: Real simulation (default when target is above 60), Interpolated (60 sim,
   smooth display; after Milestone 2). Key SNAP_FPS_MODE=sim|interp. Greyed when target is 60.
3. Frame generation: Off (default), FSR 2x (generated, 60 to 120). Key SNAP_FRAMEGEN. Help text:
   "Generated frames are not real frames. Photos always use real frames."
4. Smooth distant animation: Off (default), On. Key SNAP_FULL_RATE_ANIM. Help text mentions GPU cost.
5. Show rates overlay: Off (default), On. Shows simulation fps and output fps separately, labels
   generated output. Key SNAP_FPS_OVERLAY.

Menu notes: show a warning under the target row when the display refresh is not a multiple of the
target (for example 120 on the 144 Hz monitor without VRR), and remember the last target per
monitor.

## 6. Spike results (2026-11-14)

See `analysis\perf\high-fps.md` for numbers, run ids and the go/no-go. Hidden-run limits: acquire never blocks, so display
cadence, k-lock, and judder need a visible run (none were done); presents/s hidden is the limiter or the uncapped CPU rate.

## Hidden-run note

The research behind this plan used one hidden muted florio_eval run (guest vsync at 120 Hz, limiter at
60, exit code 0). It showed ride seconds do not depend on guest vsync. No further runs were made.
