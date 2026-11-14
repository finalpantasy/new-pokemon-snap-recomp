# host/: loader, boot driver and the one-command build

Private personal-use project code; nothing here is game data.

## Build (one command)

    tools/venv-recomp/Scripts/python -I host/build_snap.py [--jobs 8] [--rebuild]

Output: `build/snap/snap_boot.exe` (64 MB, with a linker map). The script:

1. generates `generated/boot/nvn_bridge.cpp` (`host/gen_nvn_bridge.py`: AArch64-to-x86-64 ABI wrappers for every NVN proc, read from the
   C++ definitions in `runtime/nvn`),
2. runs `runtime/hle/gen_hle_stubs.py --all` and copies its stub file to `generated/unimplemented.cpp` (the coverage table is restored; it
   belongs to the HLE owner),
3. compiles (BelowNormal, at most 8 jobs, incremental by mtime) `host/*.cpp`, `runtime/os`, `runtime/hle`, `runtime/audio`, `runtime/nvn`
   and the generated files, using the VS Build Tools environment for the Windows SDK,
4. links with the lifted main and sdk libraries from `build/eh/{main,sdk}/lib` (made by `recomp/snap_build.py`) and `vulkan-1.lib`.

A cold build of the host side takes about 90 s; relinking after a host-only change takes 5 to 10 s. Regenerating and recompiling the lifted
modules is separate (`recomp/snap_gen.py`, `recomp/snap_build.py`).

Build notes (2026-11-14): the incremental check depends on every `host/**/*.h` and `runtime/**/*.h`, and each object has a `.flags` stamp next to it (hash of its compile command), so changing a header in any directory or a code-generation switch (`--pgo-*`, `--lto`, `--cflags-extra`) rebuilds what it affects. `run_boot.ps1` exits with the game's exit code (124 = killed on timeout, route TIMEOUT = 3); `bench.ps1` prints BENCH RUN FAILED for a nonzero code.

## PGO and ThinLTO (2026-11-13, opt worker)

**PGO is the default when `build\pgo\snap.profdata` exists** (local, never committed: it is derived from generated game code).
`recomp\snap_build.py --pgo auto` (default) adds `-fprofile-use` for main and sdk, and a new profile changes the flag stamp, so
the next build recompiles every TU. `host\build_snap.py --pgo-use auto` (default) does the same for the host side; the profile is a
dependency of every host object. Switch back: `snap_build.py --pgo off` and `build_snap.py --pgo-use off --rebuild` (host objects
only notice a new profile, not its removal, so pass `--rebuild` once). A/B result and numbers: `analysis\perf\results.md`, "Build optimization: PGO".

Re-train after main is regenerated (functions whose code changed lose PGO until then; clang skips mismatched ones silently):

    tools\venv-recomp\Scripts\python -I recomp\snap_build.py --gen generated\v262144\sdk --out build\w\<you>\gen\sdk --lib snap_sdk --cflags "-march=x86-64-v3 -fprofile-generate"
    tools\venv-recomp\Scripts\python -I recomp\snap_build.py --gen generated\v262144\main --out build\w\<you>\gen\main --cflags "-march=x86-64-v3 -fprofile-generate"
    tools\venv-recomp\Scripts\python -I host\build_snap.py --out build\w\<you>\gen\exe --pgo-gen --main-lib build\w\<you>\gen\main\lib\snap_main.lib --sdk-lib build\w\<you>\gen\sdk\lib\snap_sdk.lib
    $env:LLVM_PROFILE_FILE = '<abs dir>\snap-%p.profraw'; powershell -File host\run_boot.ps1 -Route florio_photos -BuildDir build\w\<you>\gen\exe -Timeout 590
    tools\llvm\bin\llvm-profdata merge -o build\pgo\snap.profdata <abs dir>\*.profraw      (take build\heavy.lock for this copy)

The instrumented exe runs the whole route at about 50 fps. `host\pgo.cpp` (only in `--pgo-gen` builds) writes the counters every
`SNAP_PGO_DUMP_S` seconds (default 15) because killed runs skip the exit-time write; on Windows each write appends to the file, so it
resets the counters after each write and the merge sums disjoint slices. Check the merge: `llvm-profdata show --function=PresentWindowOnce
--counts` should show about one count per present.

**ThinLTO is opt-in and not recommended**: `snap_build.py --cflags "-march=x86-64-v3 -flto=thin"` (bitcode libs) plus
`build_snap.py --lto thin`. PGO + ThinLTO was no faster than the plain build in two A/B runs, and every relink takes about 140 s
instead of 15 to 60 s. `--cflags-extra` passes extra host compile flags.

## Run

    powershell -File host/run_boot.ps1 [-Timeout 300] [-Log analysis\boot\boot_trace.log] run

runs `snap_boot.exe run` at BelowNormal with a hard timeout (killed by PID). Other commands, all from the project root: `info`, `shared`
(symbols defined in both modules), `findptr <addr>` (slots holding a pointer), `init [--probe N]` (old initializer probe), `eh`, `lua`,
`bench`, `all`. `run` options: `--trace FILE`, `--timeout S`, `--stop-after-init`, `--probe N`, `--stub <mangled>`.
Debug: `SNAP_FNTRACE=0xaddr[,..]` logs every entry of those lifted functions; `SNAP_OS_TRACE=1` logs os-layer thread events;
`NVN_VK_VALIDATION=1` and `NVN_LOG=1` for the NVN layer.

## Play (live input)

    powershell -File host\play.ps1              play
    powershell -File host\play.ps1 -SkipIntro   scripted input through loading, name entry and the lab cutscene (about 3.5 minutes), then it stops and hands control back before the tutorial panels; real input stays active
    powershell -File host\play.ps1 -ListPads    list the controllers the game sees and show live pad input for 15 s

`play.ps1` takes `build\heavy.lock` only while it copies `build\snap\snap_boot.exe` (plus `SDL3.dll`, the PDB and `nvn_cache`) to `build\play\`,
releases it, creates `build\play.lock` (workers must not start boot runs while it exists), runs the copy at normal priority with no timeout and
validation off, and deletes `play.lock` when the game exits (window closed, crash or Ctrl+C). Builds can continue during play. Logs:
`analysis\boot\play_stderr.log`, `play_stdout.log` (device list), `play_trace.log`. Click the game window once so it has focus: keyboard and
gamepad input are ignored while another window is in front.

Saves: the player's save is `%APPDATA%\SnapRecomp\save` (`SNAP_SAVE_DIR` overrides it). `host\run_boot.ps1` and all test runs use
`build\runs\slot<N>\save`, wiped per run (`-KeepSave` keeps it), so tests never touch the player's save. The game writes the profile (language, voice,
appearance, name) when "Are you ready to start?" is confirmed; a later launch skips those screens and goes straight to the opening cutscene. No
later write was seen up to the first tutorial step, so the opening cutscene and tutorial replay until the game saves its progress.

## Window options (`host/window_opts.cpp`, API in `host/window_opts.h`)

F11 or Alt+Enter toggles borderless fullscreen (a `WS_POPUP` window covering the chosen monitor; the NVN swapchain follows through its normal out-of-date path). The windowed
size and position are remembered, and a position on a monitor that is gone is moved to the chosen (or primary) monitor. Settings live in `%APPDATA%\SnapRecomp\window.json`
(`SNAP_CONFIG_DIR` moves the folder), written atomically on every change, on drag end and on close. Test runs (`SNAP_SAVE_DIR` set) do not write it unless `SNAP_WINDOW_SAVE=1`.

| Key | Env override | Default |
|---|---|---|
| `mode` `"windowed"` or `"borderless"` | `SNAP_WINDOW_MODE` | windowed |
| `monitor` (index, -1 = where the window is) | `SNAP_WINDOW_MONITOR` | -1 |
| `width`, `height` (windowed client size), `x`, `y` (outer position), `maximized` | `SNAP_WINDOW_W/H/X/Y` | 1280x720 |
| `pause_on_focus_loss` | `SNAP_WINDOW_PAUSE` | true |
| `mute_in_background` | `SNAP_WINDOW_MUTE_BG` | true |
| `game_notify` (also post nn::oe FocusStateChanged 15 and Resume 16 to the game) | `SNAP_WINDOW_NOTIFY` | false |
| `frame_cap` (fps, 0 = none; only lowers the game's own pacing; needs the nvn `Pace()` line in `analysis\mods
vn_hook_requests.md` #3) | `SNAP_WINDOW_FRAME_CAP` | 0 |

Pause: the game imports no focus-state query (`GetCurrentFocusState`, `SetFocusHandlingMode` are absent; only `TryPopNotificationMessage`, whose one caller at main+0x5dc3f0
turns message 16 into a resume event, 30 into an operation-mode event and anything else into a generic event). So the host pauses: guest ticks freeze (`os_time_pause`), and
the present hook blocks the game thread until focus returns (no frame-delta jump). Mute sets the master volume to 0 and restores it; `window.muted` next to `window.json` restores
it after a crash. With pause on and mute off, the audio thread keeps running on its buffered state.
C API (callable from any thread; applied on the window thread, saved): `snap_window_get_mode/set_mode/toggle_fullscreen`, `snap_window_monitor_count/monitor_name(i)/get_monitor/set_monitor(i)`,
`snap_window_get/set_pause_on_focus_loss`, `snap_window_get/set_mute_in_background`, `snap_window_frame_cap/set_frame_cap`, `snap_window_focused/paused`.
Test helper: `SNAP_WINDOW_TEST="blur@60,focus@75,f11@90,f11@95"` sends WM_ACTIVATE (`blur`, `focus`), WM_KILLFOCUS (`kill`), WM_SETFOCUS (`setfocus`), F11 and Alt+Enter (`altenter`) to the window
at those seconds and logs `[window] test:` lines (paused, master volume, client size) to `boot_stderr.log`. `SNAP_AUDIO_LEVELS=<file>` logs the real output level per second.

Focus test (2026-11-12, `SNAP_WINDOW_TEST="blur@70,focus@90"`, run `analysis\boot\runs\0-20261112_034536`, audio levels in `analysis\boot\audio_levels_win.txt`): blur at 70 s logged `paused=1 master=0.00`; the trace has
no `present` lines between present #3780 (69.9 s) and #3810 (90.7 s), so the guest advanced 0.5 s while 20 s of wall time passed (guest ticks frozen, game thread blocked in the present hook);
the output level was exactly 0 from t=70 to 88 s and about 0.02 rms again after focus; `focus` logged `paused=0 master=1.00` and the cutscene went on. Frame dumps cannot be taken while paused (they run
on the blocked present), so the freeze is shown by the present counter, not by two equal PNGs. The 8 s stall watchdog prints a `STALL` dump during a long pause (harmless, log only). Not tested: F11 and
monitor moves, a real alt-tab, `game_notify`.

Fast boot: `SNAP_FAST_BOOT=1` (optionally `SNAP_FAST_BOOT_FRAMES=<n>`, default 3200) turns the frame limiter off (`nvnHostSetFramePacing(0)`) for the first n presents and back to 60 Hz after. Measured (post_tutorial
save, exclusive runs, trace time of present #3030, when the title logo is up, 3 runs each): without 61.3, 59.1, 56.1 s (average 58.8); with 41.9, 44.2, 41.2 s (average 42.4), about 16 s (28 %) faster.
The first ~2070 presents are black frames (a frame-count-bound load phase with almost no CPU use; holding A from the start did not shorten it): 38.9 s average at 60 Hz, 18.6 s with the limiter off (about 105 fps, bounded by
frame work and FIFO). After that the game is work-bound (about 57 fps even unlimited). More gain needs a present-interval-0 path or cheaper frames in `runtime\nvn` (not touched). No scripted A presses are used: the title waits for the
player (a hands-off run can use `SNAP_INPUT_SCRIPT`).

## Save profiles

Several independent saves live under `%APPDATA%\SnapRecomp\profiles\<id>\save\<uid>\` (same generation layout as before); `profiles.json` holds the
names, last played times and the last used profile.

- First run with the old `%APPDATA%\SnapRecomp\save` folder: it is copied into a profile named "Profile 1", verified, and the old folder is renamed
  `save.migrated-<date>`. Nothing is deleted.
- Startup: with more than one profile a picker window shows name, last played and size, plus "New profile" (mouse, keyboard Up/Down/Enter, controller
  D-pad and A; Esc or B continues with the last played). With one profile there is no picker; hold Shift while starting to see it anyway.
- Direct selection: `host\play.ps1 -Profile "Photo run"` or env `SNAP_PROFILE=<name>` (created when new); env `SNAP_PROFILE_PICKER=0` never shows the picker.
- F1 menu, Saves tab: Create, Rename, Duplicate (copies one consistent generation), Clear save, Delete profile, Open folder, Switch (restarts the
  game), "Import save from folder", "Import test save" (folders in `build\fixtures\saves\`), and "Restore from trash". Clear and Delete move the data to
  `profiles\_trash\<name>-<timestamp>\` (newest 10 kept) after a hold-to-confirm. For the profile that is running, Clear and Delete are queued and applied at the
  next start, before the save mounts. "Restart now" and "Switch profile" relaunch the way `host\play.ps1` starts the game: same exe and arguments, working directory of the first start,
  same environment minus the one-shot variables (`SNAP_INPUT_SCRIPT`, so a `-SkipIntro` script does not replay; the profile picker is not shown again), `SNAP_PROFILE` set
  to the chosen profile. `play.ps1` sets `SNAP_PLAY_ROOT`; the game then writes the new pid to `build\play.restart` and `play.ps1` follows it, keeping `play.lock`.
- `SNAP_SAVE_DIR` still overrides everything (tests); `SNAP_PROFILES_DIR` moves the profiles folder (and keeps profiles on even with `SNAP_SAVE_DIR`).

## Shader precompile screens

Uses the C API in `runtime/nvn/shaderprecompile.h`. First launch (mode not chosen): a prompt offers "Compile shaders now" (with the estimated time; unavailable
until some shaders have been recorded) or "Compile while playing". Mode NOW shows a progress screen (percent, count, time left) with "Continue in background" and
"Cancel", usable by controller. F1 menu, Graphics tab: mode, status, "Compile all known shaders now", and "Rebuild shader cache" (deletes the driver cache,
sets mode NOW and restarts the game). Test runs (`SNAP_SAVE_DIR`, `SNAP_UI_TEST`, `SNAP_SHADER_PRECOMPILE` set) never show the prompt; `SNAP_SHADER_PROMPT=1` forces it.

## Controls menu (F1)

F1, or holding Back/View + Start on a controller for 0.5 s, opens the in-game controls menu (Dear ImGui overlay, native resolution); the same
keys close it (Esc closes too). While it is open the game sees no input. Keyboard and mouse drive it, and every controller the game accepts drives
it through ImGui gamepad navigation (D-pad or left stick moves, south button activates).

- **Bindings tab:** every Switch input (A B X Y L R ZL ZR + - D-pad, stick clicks, the four directions of each stick) with two slots each for
  Keyboard, Mouse (buttons, X1/X2, wheel up/down) and Controller (buttons, triggers, stick directions). Click a slot, then press the input; Esc cancels,
  Delete or right-click clears. Conflicts are marked with `!` and allowed. Per-device "Reset to defaults" buttons.
- **Mouse tab:** mouse look (right stick) with sensitivity, invert X/Y and smoothing (off by default), touch screen on the left button, cursor lock
  (the cursor is confined to the window while playing; released while the menu is open and when the window loses focus).
- **Controller tab:** face layout (by position, default, or Nintendo labels), swap sticks, inner and outer dead zone and invert Y per stick, ZL/ZR
  thresholds, live stick plots.
- **Controller tab, Gyro aim and Rumble (bottom):** gyro aim is off by default; with a pad that reports a gyroscope (SDL3 sensor API) the angular rate is added
  to the right (camera) stick, with sensitivity (1 = full deflection at 90 deg/s, dead zone 1.5 deg/s) and invert X/Y. Rumble on/off and strength (0 to 1.5) scale what the game sends through
  `SendVibrationValue`; rumble goes through `SDL_RumbleGamepad` (DualSense, Xbox, Switch Pro) with XInput as the fallback. Both apply at once.
- **Settings menu (F1) option pages** (`runtime\ui\ui_options.cpp`, table-driven): Graphics (render scale, anti-aliasing, sharpening, anisotropic filtering, texture sharpness, shadow resolution, ambient occlusion, color look and strength, HDR + peak and paper-white nits, frame cap, UI font scale, field of view 1.0-1.4 with `SNAP_FOV_SITES=0x296400,0x38cdbc`, detail distance 1-8, always full detail, draw distance 1-8 and full-rate distant animation, the last four marked "may change photo scores, being tested"), Display (window mode, monitor, windowed size, pause when unfocused, mute in background, fast boot), Photos (PNG export on/off, folder, open folder, gallery), Mods (packs with enable box and load order, written to `mods.json`) and the output mode on the Audio tab. Every row shows "applies now" or "restart needed" (yellow "changed: restart to apply" once changed); a help panel on the right gives plain-language help, the default, the effective render size and the env override name. A bar under the title counts pending restart changes and "Apply and restart" calls `restart_now`.
- **Upscaler rows (live, restart):** Upscaler (`SNAP_UPSCALER`: off, DLAA, DLSS, FSR, XeSS) and Upscaler quality (`SNAP_UPSCALE_QUALITY`: native, quality, balanced, performance; the help text lists the render size of each for a 4K window). They are not part of the quality presets. **Coming soon (disabled row, env wired in `settings.cpp`):** Lighting preset (`SNAP_LIGHTING`). Flip `kSoon` in the option table when the feature code lands.
- **Where values live:** graphics, photo and fast-boot keys in the per-profile `settings.cfg` (exported as env vars at start, a hand-set env wins; "Default" erases the key); window mode, monitor and frame cap in `window.json`; audio output in `audio.json`; mods in `mods.json`.
- **Photos tab:** PNG export options plus the gallery: starts `tools-src\gallery\serve.py --no-open` for the active profile with `--port-file`; the server tries 8777 then any free port (8765 is taken by other software on some PCs) and the tab opens the port it reports.
- **Options test:** `tests\hle\run_options_test.ps1 -BuildDir <dir>` runs stage A (sets options through `SNAP_UI_TEST`, toggles and reorders mods, Apply and restart) then stage B (a fresh run on the same profile: checks saved values, exported env vars and mods order) and writes `analysis\ui\options_test.log`; `tests\hle\opt_shots.py` makes `analysis\ui\f1_*.png`. Verified: stage A 23 PASS, stage B 22 checks 0 failed; the game log shows `aa: mode smaa, sharpen 0.30` and `gamemods: UI font scale 2` after the restart. Settings load only when `SNAP_PROFILES_DIR` is set (tests also set `SNAP_PROFILE` and `SNAP_PROFILE_PICKER=0`); in hidden runs the relaunched child dies with the run, hence two runs.
- **Per-profile settings** live in `%APPDATA%\SnapRecomp\profiles\<id>\settings.cfg` (flat `key=value`; `runtime\hle\settings.cpp`): `look`, `look_intensity`, `ssao_quality`,
  `gyro_on`, `gyro_sens`, `gyro_invert_x/y`, `rumble_on`, `rumble_strength`. Loaded in `hle::activate_profiles()`; test runs with `SNAP_SAVE_DIR` do not load them.
- **Accessibility tab:** toggle-or-hold for ZL and ZR, UI scale.  **Test input tab:** live readout of what npad 0 would report.
- Settings apply live and are saved atomically to `%APPDATA%\SnapRecomp\input.json` (`SNAP_CONFIG_DIR` overrides the folder). An invalid file is kept as
  `input.json.invalid`, defaults are used and a notice shows on screen. The old `config\input_map.json` is migrated once.
- Test: `tests\hle
un_tests.ps1` (binding resolution, JSON round trip, migration, capture) and `tests\hle
un_ui_test.ps1` (boots the game, posts F1,
  rebinds a key, checks the npad; `SNAP_UI_TEST=<script>`).

Default controls (before any edit). Buttons use the Switch layout by position, so on an Xbox pad Switch A is the right face button (Xbox B) and
Switch B is the bottom one (Xbox A); the Controller tab can switch to Nintendo labels.

| Switch | Gamepad (XInput or SDL3: Xbox, PlayStation, Switch Pro) | Keyboard |
|---|---|---|
| A / B / X / Y | east / south / north / west face button | L / K / I / J |
| Left stick (move) | left stick | W A S D |
| Right stick (aim camera) | right stick | arrow keys |
| L / R | left / right shoulder | Q / E |
| ZL / ZR | left / right trigger | Left Shift / Space |
| Plus / Minus | Start / Back | Enter / Backspace |
| D-pad | D-pad | T / F / H / G (up, left, right, down) |
| Touch screen | | left mouse button |

Input switches (environment, override the profile): `SNAP_RUMBLE=0..1.5` (strength, 0 = off), `SNAP_RUMBLE_LOG=1` (log each game vibration request and
host rumble call), `SNAP_GYRO=0|1` (gyro aim on the camera stick; needs a pad with a gyro; untested on real hardware), `SNAP_GYRO_SENS=<n>` (1 = full
stick at 90 deg/s), `SNAP_GYRO_FAKE=<yaw_dps>,<pitch_dps>` (pretend gyro for tests; yaw positive turns left, pitch positive tips up; implies `SNAP_GYRO=1`).
Test routes: `host\routes\rumble_probe.txt`, `host\routes\gyro_probe.txt`; input-script tokens `rumble=<lo>,<hi>` and `gyro=<yaw>,<pitch>` (pretend gyro, needs `SNAP_GYRO=1`).
Mouse aim (Controls, Mouse tab) already has mouse-look, sensitivity, invert and smoothing (`mouse.sensitivity`, `mouse.smoothing` in `input.json`).

The game polls only the Full-key (Pro controller) npad 0 state, the touch screen and gestures; it never asks for six-axis (motion) data.
Pads: every gamepad SDL3 reports is opened and merged with every XInput slot into npad 0 (buttons OR-ed, larger stick value wins), the
joystick list is rescanned every second (hot-plug, wake-up) and each device is printed (`pad:` lines in `play_stdout.log`). SDL3 is used when
`tools\dl_sdl3\x\SDL3-3.4.20` exists (the build copies `SDL3.dll` next to the exe); XInput is always read as well.
The player name prompt is answered automatically (no on-screen keyboard): the name is the setting `player_name` in `hle.json` (`%APPDATA%\SnapRecomp\config\hle.json`), default "Ash"; the env var `SNAP_PLAYER_NAME` overrides it. UTF-8 is accepted; the game's own length limit applies.
Scripted input for unattended runs: `SNAP_INPUT_SCRIPT=<file>` (lines `<start> <hold> <buttons or lx= ly= rx= ry=>`, `repeat <start> <end> <period>
<hold> ...` and `anchor <event>`; seconds since the first poll or since the event), OR-ed onto the real input. `host\skip_intro.txt` is the intro script.

### Frame-based script times (load-independent)

A script line whose start is written `@f<frame>` counts guest presents (the NVN present count) since the segment began instead of seconds:
`@f600 30 A` holds A from present 600 for 30 presents; `repeat @f0 @f3000 90 15 A` taps A for 15 presents every 90. End, period and hold take
an optional `@f` prefix (frames either way on such a line). Time lines and frame lines can be mixed in one file. At 60 fps 60 presents is
1 s; under load the game just takes longer in wall time and the route stays in step. Implemented in `runtime\hle\input_core.cpp`
(the script player lives there, not in `hid.cpp`).

### Waiting on game state (`wait`, `end`; 2026-11-13)

Present counts still drift by thousands between runs (load times, ride phase), so routes wait on the game instead:

    wait <condition> [timeout=<s>] [for=<n>]
    end [<start>]

`wait` starts a new segment (like `anchor`) once the condition holds for `n` presents in a row (default 1); the lines after it count
from that moment (`@f0` = the present it was met). Until then the lines of the segment before keep running. `timeout` (default 300 s,
`0` = none) counts wall seconds from the start of the segment before; when it runs out the log gets `input script: TIMEOUT after <s> s
waiting for condition '<c>'` plus one line per known UI scene, rail, film and dialog state, and the run exits with code 3 (set
`SNAP_ROUTE_TIMEOUT=continue` to log and keep going). `anchor` lines take `timeout=` too. `end` exits the run with code 0 at that time
of its segment (a finished route need not sit out `-Timeout`). Every met wait logs `input script: wait '<c>' met after <s> s at present
<n>`. A `!` in front negates a condition.

| condition | true when | read from |
|---|---|---|
| `ui=<scene>` | that Lumen UI scene is open (between its "opened" and "closed" events). Names are the `.drp` base names: `title`, `main_menu` (lab hub menu), `map_select`, `photo_submit` (photo select), `evaluation`, `pause_menu`, `option`, `result_reward`, `myphoto`, ... | inline hook `rt_gm_route_ui_event` at `main+0x6c1d90` (`host\mods\routes.cpp`): the scene base's event handler; scene name from the Lumen player (`+0x10`) |
| `ui=<scene>:<state>` | open and in that layout state (`State*` = prefix). Seen: `title:PressStart`, `main_menu:MainMenu`, `map_select:Start` / `StageSelect` / `CourseSelect` / `Exit`, `photo_submit:FolderSelect` (subject folders) / `PhotoSubmit` (one folder's photos; A on a photo picks it and goes back to the folders), `evaluation:Start`, then per photo `EvaluationNew` / `Comment` / `Score` / `Announce` / `PhotoRegister`, then `End` | same hook (state string of each event) and `rt_gm_route_ui_setstate` at `main+0x79a6d0` (Player::SetState) |
| `dialog` | the dialogue / skit text window waits for A (seen for the professor before photo select; not every window uses it) | existing hook `rt_gm_skip_wait` at `main+0x676630` (`host\mods\skips.cpp`) |
| `ride` | the NEO-ONE rail moves (progress advanced in the last 20 presents) | existing hook `rt_gm_neo_speed` at `main+0x4c1df4` (`neofilm.cpp`): rail object `+0x70` |
| `ride>=<x>` | moving and the rail progress is at least x (Florio: 0 at the start, about 116 at the end of the first rail; about 0.9 per second) | same |
| `ride_end` | a ride was seen and the rail has not moved for 90 presents | same |
| `film<=<n>` | the course HUD film counter (72 at the start) is at most n (`film<=0`: the film is used up, which ends the expedition) | existing hook `rt_gm_film_c` at `main+0x393d9c` (`neofilm.cpp`): HUD `+0x2b8` |

Game-time rides (2026-11-13): present counts swing about 7 times with PC load, while the rail and camera turns follow game time. Chain `wait rail>=<x>` (alias of `ride>=<x>`) for rail points and use seconds lines (`<start> <hold> tokens`, no `@f`) inside each segment; both stay put under load. Keep `@f` only on `shot=` lines (a `shot=` on a seconds line dumped no frames in run `0-20261113_235715`).

Gotcha: when a `wait` (or `anchor`) is met, the lines of the segment before it stop for good. A line whose start time (for example `@f240`) lies after the moment the wait was met never fires. The runner now logs `input script: WARNING segment <k> ended before <n> of its lines started (first at @f<t>); they never fire` at that point (2026-11-13). Put such lines after the wait, timed from it, or keep the whole sequence before the wait.

Debug: `SNAP_ROUTE_LOG=1` logs every scene, state change, non-frame UI event (1 built, 2 opened, 3 closed, 5/6 state entered/ready,
7 input on, 16 focus, 18+10 decide, 21+12 decide done), rail start/move/restart, film count and dialog window (`route:` lines in
`boot_stderr.log`); read it to find the state names for a new route. All of it is off (each hook returns at once) unless a script has a
`wait` line or `SNAP_ROUTE_LOG` is set. Routes using it: `florio_eval`, `florio_photos`, `post_tutorial_check`, `unlock_all_check`,
template `core_template.txt` for the course sweep.

Fixture `story_complete` (2026-11-13, secrets worker): `unlock_all` plus every scenario bit (file 9 entry 0 offset 0x3b58 = 0xFFFFFFFFFFF, events 0..43) and all Boss flag bytes (0x3b88..0x3b9b = 1), built with `scripts\snap_save.py patch` (flags per `analysis\core\story.md`). Use it for content gated on story completion (Florio Secret Side Path, Mew). `analysis\core\secrets\run_s.sh` takes it through `SNAP_FIX=story_complete`. Route token `shot=<n>` (in a `@f<start> <hold> shot=<n>` line) dumps a 4-frame burst every n presents.

### Parallel runs (owner exception to the one-heavy-process rule)

Locks (`host\locks.ps1`; a lock file holds `owner pid=<pid> <time> <what>`, stale when the pid is dead or it is older than 20 min):

- **Build lock** `build\heavy.lock`: one build (`host\build_snap.py`, `snap_build`) at a time. Unchanged.
- **Run slots** `build\slots\run0..3.lock`: `host\run_boot.ps1` takes one (up to 4 hidden runs at once; owner 2026-11-12, cut from 6 because 6 held the CPU at 100%), waits and prints the owners when full,
  releases in a finally block. While `build\heavy.priority` exists slot 0 is only for the run with `$env:SNAP_LOCK_OWNER` equal to its content.
- **Muted**: hidden runs (`run_boot.ps1`, `run_boot_mods.ps1` without `-Visible`) default to `SNAP_AUDIO_BACKEND=null`: no sound reaches the speakers, while audio timing and `SNAP_AUDIO_CAPTURE` work as before. Set `SNAP_AUDIO_BACKEND=wasapi` only for a test that needs the real device (spatial or WASAPI channel checks), and then mute the game with `SNAP_VOL_MASTER=0` unless the sound itself is under test.
- **Own builds**: `host\build_snap.py --out build\w\<owner>` relinks into a private folder without `build\heavy.lock`; run it with `run_boot.ps1 -BuildDir build\w\<owner>` (or `$env:SNAP_BUILD_DIR`). A build dir without its own `nvn_cache` gets the warm one from `build\snap`. Take `heavy.lock` only to write `build\snap`, rebuild the main/sdk libs (`recomp\snap_build.py`) or regenerate main.
- **Exclusive**: `-Bench`, `-Exclusive` or `SNAP_PERF` set: takes `build\slots\exclusive.lock`, then all 4 slots; new normal runs wait behind it.
- **Shared perf runs** (2026-11-13): `SNAP_PERF_SHARED=<prefix>` writes the same `SNAP_PERF` CSVs but `run_boot.ps1` does not key on it, so the run takes one normal slot. For A/B comparisons on a shared PC only (alternate A and B in the same time window); absolute numbers are not comparable with exclusive benches.

Each run copies `build\snap\snap_boot.exe`, DLLs and `nvn_cache` to `build\runs\slot<N>\` (a concurrent build may relink), uses
`build\runs\slot<N>\save` and its own copy of the shared `%APPDATA%\SnapRecomp\shadercache` (via `SNAP_SHADERCACHE_DIR`; warm, no concurrent
writes; new pipelines found by a run are not merged back), and runs at BelowNormal. Output has a run id `<slot>-<yyyymmdd_hhmmss>`:
`analysis\boot\runs\<id>\boot_trace.log` (+ `boot_stderr.log`, `boot_stdout.log`), `route_<name>_<id>_NN.png` or `run_<id>_NN.png`
(frame dumps; `-ShotAt` without `-Route` also dumps). Old names stay as copies of the latest finished run: `analysis\boot\boot_trace.log`,
`boot_stderr.log`, `boot_stdout.log`, `route_<name>_NN.png`. An explicit `-Log` is honored as-is (stderr/stdout beside it). `build\testsave`
is no longer used. For other scripts: `. host\locks.ps1; Take-BuildLock 'what'; ...; Release-Lock build\heavy.lock`.

### Scripted routes (`-Route`)

    powershell -File host/run_boot.ps1 -Route florio_photos [-Timeout 295] [-ShotAt 40] [-ShotEvery 15] [-ShotMax 17] [-SaveFixture name] [-NoLock]

Feeds `host\routes\<name>.txt` as `SNAP_INPUT_SCRIPT` (format: see Scripted input below), seeds the run's save dir from the fixture named
on the file's first line (`# fixture: post_tutorial`; `-SaveFixture` overrides), takes a run slot (see Parallel runs; `-NoLock` if the
caller holds one) and writes checkpoint screenshots `analysis\boot\route_<name>_<runid>_NN.png` (latest also copied to `route_<name>_NN.png`) (half size, from `NVN_FRAME_DUMP`; the desktop capture
`SNAP_SHOT` is black on the hidden desktop). Use `-ShotAt` 40 or lower: with a later start no dumps were produced in some runs. Script time is
wall time from the first input poll, so a loaded PC shifts the game but not the script (use frame times, below); presses during loading are lost.
`florio_photos` (A taps plus ZR) reaches the lab intro and the camera tutorial by ~190 s; later steps are unverified (see
`analysis\recomp\integration_issues.md`). `host\routes\unlock_all.txt` is the same A-tap probe on the `unlock_all` fixture.

## PC tabs in the game's own Settings screen (2026-11-13, `host/mods/nativeopt.cpp`)

The game's Settings screen (lab hub gear) is data driven: `option.lmb` in `common/UI/master/Scenes/option.drp` lists states
(tabs), elements and focus groups (format and dumper: `scripts/ui/lmb.py dump <option.lmb> [state]`). The PC settings are eleven
extra tabs in sets of five after the game's "Other" tab (R past Other; L on Game goes to the last one): Quality, Textures,
Effects, Output, View | Display, Controller, Mouse, Audio, Photos | Shadows & AO. They use the game's own row widgets (two-choice rows,
pulldowns with their popup), tab bar, help line, sounds and Restore Default Settings button.

- Pack: `python -I scripts/ui/make_native_options.py [--src <option.lmb>] [--out <mods root>] [--if-stale]` writes the local
  mod pack `native-options` (patched `option.lmb`, `rows.txt`, `pack.json`; default out `%APPDATA%\SnapRecomp\mods`) from
  the user's own extracted `option.lmb`. It is game-derived: never commit it. `host\play.ps1` runs it with `--if-stale`
  before every start (rebuilds when the pack is missing or the script changed; `-NoNativeOptions` skips it; a failure only
  means no PC tabs). Rows are the `TABS` table in the script; `rows.txt` carries them to the host, so there is no second list.
- State switches are the Lumen player's own: an element whose f2 names a state (f6 = 1) goes there on decide (tabs, pulldown
  rows), f6 = 4 returns to the previous state (popup items), and L/R open the state named at +0x30 of the page's Prev/Next
  element entries (BuildPage fills in the neighbor game tabs). The player then sends event 5 with the new state; for a PC
  state `rt_ui_opt_event` (`0x728e10`) builds the page with the game's BuildPage (`0x737360`), renames it and its five tab
  entries, points Prev/Next at the neighbor PC tabs (Other before PC tab 1, Game after the last), adds the PC variables and
  row element entries (the cursor only reaches listed elements) and calls SetState. Calling SetState at any other time does
  not switch states. After SetState the hook redraws each pulldown row's value text (the player keeps the old text). A
  two-choice element's focus action leaves its box at `lb_select` (no unfocus action; the game's handler repaints), so the
  hook repaints a two-choice row from its value when the cursor leaves it; without that both boxes stayed yellow. A popup
  is the game's own list element (`PulldownMenu`, type 6, f4 = last item): it moves the cursor itself and reports
  `PulldownMenuItemNN` (separate item elements linked by focus groups never moved the cursor). As the game's list helper
  does (`0x776c80`, `0x776bb0`), on decide on the row (before the player switches) the hook registers the items with
  `player->vtable[0x148](player, &popup state, &"PulldownMenuItemNN", true)`, focuses the current value
  (`player->vtable[0x1f0]`), and starts the list itself (goto + play `lb_start_listNN`, NN = items + 1, on the popup's
  `pulldown_NN_mc` through `vtable[0x90]`/`[0x98]`, as `0x776980` does), then focuses the current value again; the popup
  states have no `lb_start_list` enter action (the game's do not either). So a popup opens with the cursor (the game's
  yellow highlight, its only marking of the current value) on the current value (verified 2026-11-14: Render scale opens
  on 1x, Shadow distance on 1x). A popup grows downward from its row, so a long list on a low row opens from a higher
  row's popup (`pull_slot` in the script, column 9 of `rows.txt`; row 1 holds 10 items). Limit: Sharpening and Master
  volume are pulldowns, not the game's 0..10 slider: its knob is moved by the game's own tab handler, which a PC page
  bypasses.
- Tab sets: the screen has 2- to 5-tab bars (`window_mc` labels `lb_tab_02..05`, chosen by `{TabNum}`; the game plays it
  only in Init). Every game and PC tab state replays `lb_tab_0{TabNum}`, and for a short last set (Shadows & AO alone)
  the hook overwrites `TabNum` in the descriptor's variable map with max(2, tabs in the set); a spare slot is blank.
  The generator asserts at most 6 rows per tab (no scrolling), 10 items per pulldown, 112-character help.
- "(env)" after a pulldown's value: the row's `SNAP_*` variable was set by hand before start, so it wins over the saved
  value (`settings::env_wins`; the F1 menu shows the same warning).
- Visual extras (owner 2026-11-13 and 2026-11-14: the original base game by default, every enhancement row defaults to
  Original): Anti-aliasing (stores the F1 table's values, so the quality preset keeps its name: original look = Original
  `default` / SMAA `smaa` / SMAA + supersampling `ssaa2,smaa`; under `SNAP_VISUALS=enhanced` Original is `off`, the
  game's FXAA, and `default` is the supersampling entry), Sharpening (Off, 0.1 to 0.7, 0.85, 1.0), Texture filtering
  (Original / 2x / 4x / 8x / 16x), Sharper menu text (Off = `off` / On = `auto`, 2x at 1440p and above) and Save PNG
  copies. Their defaults (and so Restore Default Settings) are read from `runtime\hle\visual_defaults.h` when the pack is
  built (the pack rebuilds when that file changes); they apply after a restart (the F1 table marks them `kRestart`).
- Advanced rows (2026-11-14, `docs/advanced-graphics-options.md`; all default to the original look): Render scale adds
  1.75x (10 steps, 0.5x to 4x), Texture sharpness -2 to +2 (10 steps), Frame rate limit adds 75, 165 and 240; Output: HDR
  output Off / HDR10 (`1`, what the old on/off row stored) / scRGB (`scrgb`), HDR peak Auto (the key erased: the
  monitor's reported peak, else 1000) or 400 to 4000 nits, HDR highlights (`gfx_hdr_expand` -> `SNAP_HDR_EXPAND`, 0 to 1,
  default 0.3); tab Shadows & AO: Shadow distance (`gfx_shadow_dist` -> `SNAP_SHADOW_DISTANCE`, 1x to 16x, not in the
  quality presets). Reserved, not drawn yet: AO radius and strength on Shadows & AO (`SNAP_SSAO_RADIUS` /
  `SNAP_SSAO_STRENGTH` are parsed in `host/mods/gamemods.cpp` but never written to the pass), the lighting stream's rows
  after them. Vsync (Output row 6 and F1 Graphics "Frame rate"): On / Adaptive / Fast / Off, live through
  `snap_vsync_set`, remembered as settings key `vsync` (exported as `SNAP_VSYNC` at the next start; On erases it).
- `rt_ui_opt_state` (the 8 SetState `blr`s) keeps a template element entry and points the game's Other page's Next at PC tab
  1 and the Game page's Prev at the last PC tab. Choices and popup picks are stored where the
  F1 overlay stores them (`snap::ui::options_test_set`: same keys, defaults, custom stores and presets; the controls mapping;
  the master volume). "Applies after restart." is in the help line of rows that need it.
- Restore Default Settings (Minus) on a PC tab resets that tab's rows to their defaults and redraws them; on a game tab it
  resets only that game tab (the game's own behavior, which never touches PC settings). Each PC tab's help line says so.
- Not in the PC tabs (F1 menu only): PNG folder path, lighting preset, stick dead zones, mouse smoothing, F1 menu scale,
  bindings, mods, saves, test input.
- `SNAP_NATIVE_OPT_TRACE=1` logs events, states and element lists. `SNAP_NATIVE_OPT_SETTINGS=<file>` is a test aid: runs
  with `SNAP_SAVE_DIR` skip `settings::load()`, so the hook reads and writes this settings file instead. Set
  `SNAP_CONFIG_DIR` to a private folder in test runs too, or Controller/Mouse/Display changes write the user's
  `%APPDATA%` input/window/audio files.
- Route `host\routes\native_options_tabs.txt` (`post_tutorial`): opens Settings, R to PC tab 1, then visits every PC tab with
  one change each (pulldown picks with Down or Up from the current value inside the popup, choices), Minus on Photos, B
  (leaves the Reset button), R to Shadows & AO and a pick there, R (wraps to Game), L (back to Shadows & AO), B closes. Run it with
  `SNAP_MODS_DIR=build\w\<owner>_mods` (pack built with `--out` there) plus the three switches above.

## Visual defaults and SNAP_VISUALS (2026-11-13, `runtime/hle/visual_defaults.h`)

The settings whose defaults can differ from the original game live in one table. The owner chose the original look as the default on 2026-11-13. Each extra still works on its own through its own switch (an F1 menu choice, which `settings.cpp` exports as env, or a hand-set env var), and that always wins over the profile. `SNAP_VISUALS=enhanced` turns every extra on at once; `SNAP_VISUALS=original` forces the original look if the default is ever flipped back. To change the default for everyone, flip `snapdef::kDefaultEnhanced` in that header (one line); the F1 menu labels, the Normal preset and the code paths all read the table.

| Setting (env) | Original (default) | Enhanced (`SNAP_VISUALS=enhanced`) | Read by |
|---|---|---|---|
| Anti-aliasing (`SNAP_AA`) | `original`: the game's own FXAA | `ssaa2+smaa`: 2x window height, capped (below), plus SMAA | `nvn_aa.cpp` (`AaParseEnv`) |
| Sharpening (`SNAP_SHARPEN`) | 0 (off) | 0.30 (CAS) | `nvn_aa.cpp`; off with an upscaler unless `SNAP_SHARPEN` is set |
| Anisotropic filtering (`SNAP_ANISO`) | 0: the game's own filtering | 16x | `nvn_entry.cpp` (`ApplyHostEnvPolicy`) |
| UI font supersampling (`SNAP_UI_FONT_SCALE`) | `off` | `auto`: 2x at a 1440p+ window | `gamemods.cpp` (`decide_font_scale`) |
| PNG photo export (`SNAP_PHOTO_EXPORT`) | 0 (off) | 1 (on) | `photoexport.cpp` |

`SNAP_LOD_BIAS` is not a row: its automatic mode only follows a render scale above 1 and does nothing at 1x, so both profiles behave the same. The ssaa2 scale is 2x the window height over 1080, capped at 3.0 (5760 x 3240) and 8192 px per side, so a 2160p window renders at 3.0, not 4.0; the log line `aa: ssaa2 scale capped ...` shows it. An explicit `SNAP_RENDER_SCALE` is never capped. The window height used for this (and by the upscaler) is `SNAP_WINDOW_H`, else the primary monitor height when the window is borderless or maximized (`SNAP_WINDOW_MODE` or `window.json`; read once at start, so a later F11 does not change it), else the `window.json` windowed height, else 720. With no `mods.json`, no mod pack is enabled except `native-options` (the PC tabs in the game's Settings screen); `mods.json` can still disable it.

## Graphics quality presets (F1 > Graphics > Quality preset)

The row picks Very Low, Low, Normal, High, Ultra or Maximum and sets nine rows at once; it shows Custom when the rows match no preset (the preset itself is not stored). All rows need a restart. Blank = the row's default.

| Row (env) | Very Low | Low | Normal | High | Ultra | Maximum |
|---|---|---|---|---|---|---|
| Render scale (`SNAP_RENDER_SCALE`) | 0.50 | 1 | default | default | 2.00 | 4.00 |
| Anti-aliasing (`SNAP_AA`) | none | off (game FXAA) | default (the game's own FXAA; ssaa2+smaa with `SNAP_VISUALS=enhanced`) | ssaa2,smaa | smaa | smaa |
| Sharpening (`SNAP_SHARPEN`) | 0 | 0 | default | 0.30 | 0.30 | 0.50 |
| Aniso (`SNAP_ANISO`) | off | 4 | default (off; 16 with `SNAP_VISUALS=enhanced`) | 16 | 16 | 16 |
| Texture sharpness (`SNAP_LOD_BIAS`) | +1 | auto | auto | auto | auto | auto |
| Shadow map (`SNAP_SHADOW_RES`) | 512 | 1024 | 1024 | 2048 | 4096 | 8192 |
| AO quality (`SNAP_SSAO_QUALITY`) | default | default | default | high | ultra | ultra |
| AO samples (`SNAP_SSAO_SAMPLES`, new row) | 4 | 8 | default | default | default | 512 |
| HBAO dirs x steps (`SNAP_SSAO_HBAO`, new row) | default | default | default | default | default | 32 x 128 |

Left out on purpose: detail distance, always full detail, draw distance, full-rate animation (photo scores unverified), FOV (paused), lighting preset (lighting worker), HDR, color look, frame cap. Upscaler and upscaler quality rows are now live (restart) but in no preset. The upscaler replaces the render scale and the ssaa2 default (the game's FXAA is off unless `SNAP_AA` is set); its scene scale is below 1 in a window under 1080p (works since the fix below; DLSS Quality at 1280x720 scored in run `0-20261112_174810`).

Findings (2026-11-12, florio_eval / florio_photos, hidden 1280x720 window, RTX 4090, 2 to 4 runs at once with other workers, so absolute times are noisy):
- Sub-native render scale lost the GPU device (`vkQueueSubmit2 failed: -4`) 20 to 30 s after the score screen in 4 of 4 runs at 0.5/0.75. Cause: the album thumbnail readback copied a 240x200 guest region out of the 120x100 scaled image. Fixed 2026-11-12 in `runtime\nvn\nvn_xfer.cpp` (render-scale-aware copies, `NVN_NO_XFER_SCALE=1` restores the old ones; runs in `AGENTS.md`, "Sub-native scale device loss fixed"). Very Low now draws at 0.5 (960 x 540) and the slider starts at 0.5. 0.5 and 0.75 each scored in 2 runs and kept running 160 to 200 s past the score screen with no device loss.
- Scale 6 and 8 crash the guest before the title (run `0-20261112_144057`); 4 works.
- `SNAP_SHADOW_RES=512` is new (the depth maps scale 0.5). `SNAP_PASS_OFF` works since 2026-11-12 (see "Render pass toggles" below).
- Presents are paced to 60 Hz, so ms/frame below is mostly the limiter plus CPU load, not GPU cost: Normal 18.9 (`3-20261112_131027`), Very Low 17.7 (`0-20261112_141432`, florio_eval, photo scored, 2,375 points shown), Low 17.6 (`1-20261112_141432`), High 18.1 (`0-20261112_142450`), Ultra 18.4 (`1-20261112_142450`), Maximum 19.5 (`0-20261112_143210`, 7680x4320 scene), Maximum with a 3840x2160 window 19.7 (`0-20261112_144203`). Maximum does not lag a 4090 under this pacing; an exclusive `SNAP_PERF` bench is needed for real GPU cost.
- AO samples and shadow size do not change the course picture (lighting worker: no SSAO in the course, shadows mostly baked).
- Not done: the one-time preset suggestion from the GPU name; scoring on High, Ultra, Maximum (only Very Low and Low ran `florio_eval`; Normal at 1x/ssaa default scored in earlier runs); the F1 test `tests\hle\run_options_test.ps1` passes the new preset checks (25 checks, FOV expectations removed).

## Fx: host screen-space effects (2026-11-14, `runtime/nvn/nvn_fx.{h,cpp}`, effects in their own files)

All off by default; developer switches only (no options rows yet; rows wanted are in the Fx worker report in `AGENTS.md`).

| Switch | Meaning |
|---|---|
| `SNAP_FX=0` | Forces every Fx effect off. Unset or 1: an effect runs only when its own switch is on. |
| `SNAP_FX_AB=<P>` | Effects run only while (present count / P) is odd: same-view A/B in one run (dump frames N and N+1 at a block edge). |
| `SNAP_FX_SPLIT=1` | The left half of the frame keeps the game's own picture, the right half gets the effects (one-frame A/B). |
| `SNAP_FX_LOG=1` | Logs frame phases, camera, shadow caster capture, photo holds and effect runs (`fx:` lines on stderr). |
| `SNAP_FX_TIME=1` | GPU time of the whole pre-tonemap Fx block (timestamps; avg/max logged every 300 blocks). |
| `SNAP_CONTACT_SHADOWS=1` | Screen-space contact shadows (`nvn_contact.cpp`, `shaders/contact.frag`). Tuning (test aids): `SNAP_CONTACT_STRENGTH` (0.6), `SNAP_CONTACT_LEN` (0.6 world units), `SNAP_CONTACT_THICK` (0.25), `SNAP_CONTACT_FADE` (45 view units), `SNAP_CONTACT_LSIGN` (+1/-1 forces the light direction sign), `SNAP_CONTACT_DEBUG` (2 tints, 3 view distance, 4 normal, 5 sun facing, 6 depth, 7 the factor itself, 100 cycles). |

Regenerate the host SPIR-V headers (`nvn_fx_spv.h`, `nvn_contact_spv.h`) after editing a shader: `tools\venv-recomp\Scripts\python -I runtime\nvn\shaders\make_fx_spv.py`.

## Layers and registration order (`bind_host_layers` in `boot.cpp`)

1. `hle::bind_all`: real HLE overrides are strong; the generated not-implemented stubs are weak (bound only when no module defines the symbol).
2. `snap_bind_audio()` (`abi_audio.cpp`): the 16 `nn::audio` overrides, host C ABI to guest ABI.
3. `snap_bind_nvn()`: the `nvnBootstrapLoader` override; `nvnDeviceGetProcAddress` and the table hand out thunks.
4. `os_override_table()`: the whole os layer, one loop. To swap the os layer, change only that loop and the `os_*` calls in `run_thread`
   and `cmd_run`.

Symbol precedence: host override, rtld special symbols, `main`, `sdk`, weak backstops. 49 symbols are defined in both modules and `main` wins
(operator new/delete, std typeinfo, `end`, `nn::os::Mutex::~Mutex`; list with `snap_boot.exe shared`).

## Boot sequence in `run`

Load both modules, `os_init` (with the musl thread-pointer variables), main thread adoption, `DT_INIT` of sdk then main (each `_init` walks its
own `init_array`; calling the array again would run every static initializer twice), `nninitStartup`, `nnMain`. Trace lines are timestamped:
first call of every host function, every SVC (first 3 per number), guest exceptions, and every initializer index. A stall watchdog prints the
main thread's guest frame after 8 s without output. Unresolved indirect targets append to `analysis/recomp/indirect_misses.tsv`.

**LOD and draw distance (2026-11-12, `host/mods/lodanim.cpp`, all off by default):** `SNAP_LOD_DIST=<1..8>` moves the LOD0/LOD1/LOD2 switch distances out by that factor, `SNAP_LOD_FORCE0=1` makes every drawn model LOD0, `SNAP_DRAW_DIST=<1..8>` moves the not-drawn (cull) distance out (never inside the LOD switches); the old `SNAP_DRAW_DISTANCE=<m>` still scales both together. `SNAP_FULL_RATE_ANIM=1` rebuilds far models' render job every frame (original: every 2nd frame at LOD1, every 4th at LOD2). F1 Graphics > View rows: Detail distance (`gfx_lod_dist`), Always full detail (`gfx_lod_force0`), Draw distance (`gfx_draw_dist`, `SNAP_DRAW_DIST`; an old saved `gfx_draw_distance` is migrated to `gfx_lod_dist` + `gfx_draw_dist` at load), Full-rate distant animation; all restart-required, in no quality preset. Test aids: `SNAP_DRAW_DISTANCE_AB=<P>` (the mod applies only on odd blocks of P presents; with the `lod_yard` route and `NVN_FRAME_DUMP_AT` at 16k+7/16k+15 the pair is the same camera pose), `SNAP_TRACE_LODSEL=1`.

## Render pass toggles (2026-11-12, `host/mods/gamemods.cpp`, hook `0x989048`)

- `SNAP_PASS_DUMP=1` (with `SNAP_GM_TRACE=1`) logs every pass once: `pass <ptr> name "<Name>" order=<n> on=<0|1> ... class protected|toggle|unverified`. A pass object keeps its name as a libc++ `std::string` at `+0x40` and its enable byte at `+0xf0`. 42 passes exist: DynamicShadow, StaticShadow, ZPrepass, Backmost, Opaque, OpaqueGround, HighResolutionTranslucent, SSAOCreate/BlurX/BlurY/Composite, DownsizeTarget, Effect, Sea, BlendBuffer, CompositeBlendBuffer, MipmapLevel0..5, BloomCreate, BloomDraw, Final, Evaluation, GaussianBlurX/Y, DOFBlurX/Y, DOF, ChromaticAberration, FilmGrain, Monotone, Capture3D, CaptureCompress, PhotoEditing, PhotoEditingFrame, Report, ResizeTexture, UI, Fade.
- `SNAP_PASS_OFF=BloomCreate,BloomDraw,DOFBlurX,DOFBlurY,DOF,FilmGrain,ChromaticAberration` (case-insensitive) zeroes the enable byte before the pass loop reads it, so the pass is never recorded. Only those seven names are accepted by default. Photo and evaluation passes (Evaluation, PhotoEditing, PhotoEditingFrame, Capture3D, CaptureCompress, Report, ResizeTexture, Final, UI, Fade, thumbnail names, DownsizeTarget, CompositeBlendBuffer) are always refused with a log line; any other name is refused as unverified unless `SNAP_PASS_FORCE=1` (test aid).
- `SNAP_PASS_AB=<P>`: the listed passes are off only in even blocks of P presents (P=1: alternate presents).
- F1 Graphics rows Bloom, Depth of field, Film grain, Chromatic aberration (`gfx_pass_bloom`, `gfx_pass_dof`, `gfx_pass_grain`, `gfx_pass_aberration`, each `default`/`off`, restart needed) are joined into `SNAP_PASS_OFF` by `runtime/hle/settings.cpp`.
- What the game does itself: the course view has Bloom on for the whole run; DOF, FilmGrain and ChromaticAberration are on only in a few scenes (the course has them off), so those three toggles change nothing in normal play.

## Performance switches (perf pass 4, 2026-11-13; `analysis\perf\results.md` "pass 4")

All on by default; each env var restores the old path.

- `SNAP_SYNC_FULL=1` (or `SNAP_WRITE_WATCH=0`): full-range copies of bound pool ranges into the Vulkan mirrors, as before. Default: guest reservations carry `MEM_WRITE_WATCH` (`runtime\os\os_memory.cpp`) and `SyncPoolRangeOnce` (`runtime\nvn\nvn_vk.cpp`) copies only the pages `GetWriteWatch` reports written since the last query. Pools that are not page aligned or not in a watched reservation, and pools with fragment-shader write-backs (photo evaluation), keep full copies. `SNAP_SYNC_FULL=1` also drops the reserve flag.
- `SNAP_MODS_CHUNK_CACHE=0`: build the replacement DRP chunk (copy + crc32 of the whole file) on every hit, as before. Default: built once per override and shared read-only (`runtime\mods\mods.cpp`).
- `SNAP_SCRATCH_POOL_MB=0`: exact-size scratch buffers freed on retire, as before. Default: retired scratch buffers are recycled by power-of-two size class (64 KB to 32 MB), at most 128 MB kept (`SNAP_SCRATCH_POOL_MB=<n>`).
- `SNAP_UPLOAD_PREFETCH_MB=0`: static textures are deswizzled on the replay thread at first sample, as before. Default: `TextureInitialize` queues the deswizzle on 1 to 4 below-normal workers; the upload checks the guest bytes are unchanged (hash before and after the job and at upload) and copies the result; anything else takes the old path. At most 1536 MB staged (`SNAP_UPLOAD_PREFETCH_MB=<n>`, 0 to 8192, oldest dropped first; pass 6 raised it from 384, which dropped every texture the scene-change bursts then missed). Not for ASTC, linear, multisample or textures over 48 MB.
- `SNAP_UPLOAD_PREFETCH_IMAGE=1` (pass 5, OFF by default): the prefetch worker also creates the VkImage and its memory after a successful deswizzle (`CreateTexImage`, `runtime\nvn\nvn_vk.cpp`). `EnsureImage` adopts them when the builder and host size are unchanged (`upload_pref_img` column); otherwise the worker's image is destroyed. Two A/B pairs: `ensureimg` per burst went from 41-49 ms to 15-29 ms, but hub-burst upload time doubled and scratch allocation slowed, so it stays off.
- `SNAP_UPLOAD_PREFETCH_HELP=1` (pass 5, OFF by default): when the replay reaches a texture whose prefetch job has not started, the replay thread runs up to 8 queued jobs from the front of the queue, then its own, while the workers keep going. Default, as before: the job is cancelled and the texture is deswizzled on the replay thread. No measured gain; burst misses are rarely "not started" (`pref_miss_*` columns: `none` never queued or dropped, `fail` bytes changed during the job, `hash` bytes changed since, `late` not started; pass 6 adds `pref_miss_drop` evicted by the budget, and per `TexPrefetch` call `pref_skip_kind`, `pref_skip_nosrc`, `pref_skip_budget`).
- `SNAP_TEXMEM_BLOCK_MB=0` / `SNAP_SCRATCH_BLOCK_MB=0` (pass 7): one `vkAllocateMemory` per texture image / per scratch buffer, as before. Default: allocations up to a quarter block come from shared 64 MB blocks (`runtime\nvn\nvn_submem.cpp`; first fit, coalescing free list; images need no dedicated allocation). `SNAP_SUBMEM_SPARE=<n>` (default 2, 0 to 8): a helper thread keeps n empty blocks of each pool ready, so a burst does not allocate blocks on the replay thread; 0 = allocate on demand. `SNAP_SUBMEM_RESERVE=<n>` (pass 9, default 8, 0 to 32): from the first image allocation at boot, the helper thread keeps the image pool at n blocks or more (empty target = max(spare, n - blocks in use)), so the first hub entry finds its blocks ready; 0 = spare blocks only.
- `SNAP_POOL_BACK_AHEAD=0` (pass 7): a memory pool's Vulkan mirror (allocation plus whole-pool copy) is created at the first draw that binds it, as before. Default: `BufferInitialize` queues it on a prefetch worker (needs `SNAP_UPLOAD_PREFETCH_MB` > 0; pools over 256 MB stay lazy). New column `backing_ms`: mirror creation time on any thread.
- `SNAP_UPLOAD_PREFETCH_STAGING=0` (pass 8): prefetch workers deswizzle into a heap buffer and the upload copies it into a new staging buffer on the replay thread, as before. Default: the worker writes straight into its own mapped staging buffer (a scratch-block range, `nvn_submem.cpp`), and the upload records the GPU copy from it with no CPU copy. Textures over 16 MB, or with `SNAP_SCRATCH_BLOCK_MB=0`, use the old path. Staged bytes still count against `SNAP_UPLOAD_PREFETCH_MB`, but they now sit in pinned host-visible memory.
- `SNAP_DUMP_EVERY=<s>` (pass 5, debug, `host\boot.cpp`): guest thread dump (wait kind, wait key, guest call chain per thread) into the trace every s seconds. For hangs that keep presenting, which the 8 s silence watchdog never sees.
- New `SNAP_PERF` columns: `ensureimg_ms`, `view_ms`, `rendbegin_ms` (replay sub-scopes), `upload_pref` (uploads served by the prefetch); `upload_bytes` is now filled; `poolsync_bytes` counts bytes actually copied.
- `SNAP_NVN_CMD_RECYCLE=0` (CPU sampler pass, `analysis\perf\cpu_profile.md`): every recording grows a fresh `vector<Cmd>`, as before. Default: released lists' vectors are pooled and a new recording reserves the largest recording seen.
- `SNAP_REPLAY_SCRATCH=0`: `PrepareTextures`, `WriteDescriptors`, `DoDraw` and `DoDispatch` allocate their per-draw vectors and deques, as before. Default: thread-local scratch vectors.
- `SNAP_POOLSYNC_SNAPSHOT=1` (OFF by default): one whole-pool `GetWriteWatch` per replay epoch lets clean ranges skip the per-range syscall; the gain was too small to keep.
- `SNAP_VIEW_LAST=0` (CPU round 2, 2026-11-14): `TextureImpl::View` builds the string key and probes the view map on every call, as before. Default: a repeat of the texture's last view returns its cached handle.
- `SNAP_VA_CACHE=0`: `Host::ResolveAddress` takes `vaMutex` and walks the pool map on every call, as before. Default: a per-thread 4-entry cache, invalidated by `Host::vaGen` when a pool is registered or unregistered.
- `SNAP_SAMPLE=1` or `=<path prefix>` (profiling, off by default; `runtime\os\sampler.cpp`): host sampling profiler. Knobs: `SNAP_SAMPLE_US` (period, default 1000), `SNAP_SAMPLE_WRITE_S` (rewrite interval, default 20), `SNAP_SAMPLE_MIN_KCYC` (idle gate, default 100). Read with `python -I analysis\perf\cpu_profile.py <prof> --map <that build's snap_boot.map>`.

## Shiny Pokemon (2026-11-14, `runtime/nvn/nvn_shiny.cpp`, off by default, cosmetic only)

Build the pack once from your own extracted textures: `python -E -P scripts\hd\make_shiny_pack.py` (output `%APPDATA%\SnapRecomp\mods\shiny-pokemon\`, outside git; palettes in `scripts\hd\shiny\palettes.json`).

- `SNAP_SHINY=1`: enable (default 0). Menu row "Shiny Pokémon" (native Photos tab row 5, F1 Photos; 2026-11-14): Off (default, key `shiny` erased) / Rare 1/4096 / Common 1/512; `settings.cpp` turns the key into `SNAP_SHINY=1` + `SNAP_SHINY_ODDS=<n>` at start, so it applies after a restart (`nvn_shiny.cpp` reads the switches once). No forced/test choice in the menu.
- `SNAP_SHINY_ODDS=<n>`: 1 in n per spawned instance (default 4096; 1 = every instance of a pack species).
- `SNAP_SHINY_SEED=<n>`: fixed seed for repeatable rolls (default: random profile seed in the pack folder, a new roll each launch).
- `SNAP_SHINY_FORCE=<hex key>`: that body texture is always shiny. `SNAP_SHINY_TRACE=<hex key|all>`: log draw state or all texture keys. `SNAP_SHINY_DIR=<dir>`: pack folder.

## Scheduler switches (nn::os review fixes, 2026-11-13; `analysis\core\review_os.md`, `runtime\os\os_kernel.cpp`)

All fixes are on by default. `SNAP_SCHED_LEGACY=1` (or `all`) restores every old path below; a comma list of names restores only those (for example `SNAP_SCHED_LEGACY=migrate,yield`). The run log prints `SNAP_SCHED_LEGACY: old scheduler paths 0x..` when any is set.

- `migrate` (finding 1): no core migration. Default: a core waiter that cannot use a free core asks a holder that can use it to move there, the way Horizon's idle core pulls a thread. The holder moves at its next poll point without giving up its run; if it does not poll within `SNAP_OS_OVERCOMMIT_US` (2 ms), the waiter gets an overflow core. This is the loader (p31, pinned to core 2) behind a JobWorker on core 2 while core 0 is idle.
- `yield` (finding 2): yields only to equal or better priority. Default: `svcSleepThread(-2)` and a spin gate that has spun 4,096 passes in a row hand the core to its best waiter whatever its priority (YieldToAnyThread); the yielder then does not preempt that thread for `SNAP_OS_YIELD_ANY_US` (1000).
- `unboost` (finding 3): a core holder whose priority gets worse (end of a priority-inheritance boost, ChangeThreadPriority) notifies nobody, so better core waiters wait up to the 20 ms safety-net recheck. Default: they are woken and the holder is asked to yield.
- `deadline` (finding 10): the 2 ms overflow deadline is armed once per core wait. Default: it restarts when the holder asked to yield changes, and is cleared when nobody is.
- `hostblock` (finding 9): file reads (`runtime\hle\fs_core.cpp` `Fs::read_file`) and VAMM retry sleeps keep the virtual core. Default: they release it like a Horizon IPC wait (`os_io_begin`/`os_io_end`).
- `events` (finding 11): manual-clear TimerEvent/LightEvent waiters re-check only the flag, so Signal followed at once by Clear can leave them asleep. Default: a waiter woken by a signal returns signalled.
- `evspin`: Event and SystemEvent `Wait`/`TimedWait`/`TryWait` calls that return without blocking are not check-ins. Default: they poll for preemption; a wait on a manual-clear Event that stays signalled, and a TryWait that fails or leaves a manual-clear Event signalled, count as a spin pass (`os_spin_gate`). The `LevelManager_LoadResource` loop at main+0x47d3e0 spins this way for minutes and used to hold its virtual core, so pinned threads ran only on 2 ms overflow grants.

Diagnostics: `SNAP_OS_STARVE_MS` (50) and `SNAP_OS_STARVE_LOG=1`. A core wait longer than that while another core is free counts in `OsStats::core_starved` (shown in the watchdog dump line `dump: ... starved N, migrations M/R, yield_any Y`), and with the log switch prints `[os] starved: thread ... holders: ...`. It should stay near 0; a pinned waiter behind a better pinned holder also counts. Each core waiter in the dump now shows how long it has waited (`id/prio/mask/ms`).
Not switchable (plain bug fixes): MultiWait signal fence, guest ticks read inside the pause lock, `InternalConditionVariable::TimedWait` far targets, thread start/destroy and join/destroy use-after-free, Link during WaitAny wakes the waiter, watchdog dump takes the scheduler lock (retries up to 500 ms; if still held it prints only the counters and skips the thread walk), `SNAP_OS_CORES=0` means unlimited.

## Load-time switches (load pass, 2026-11-13; `analysis\perf\results.md` "Load times")

On by default unless noted; each env var restores the old path.

- `SNAP_FNPROF_KEEP=1`: keep the per-function `FN_ENTER` hook on for the whole run, as before. Default: `run_initializers` (`host\loader.cpp`) turns it off after the init log unless a tracer (`SNAP_FNTRACE`, `SNAP_TRACE_LOD`, `SNAP_SKIPS_REC`, `SNAP_NEO_CAMFN`) had it on. It cost a hash lookup and three observer calls on every lifted function entry, about a fifth of the guest CPU samples in a boot profile.
- `SNAP_PACE_RELEASE_CORE=0`: the presenting JobWorker keeps its virtual core while the frame limiter sleeps, as before. Default: `Pace` (`runtime\nvn\nvn_vk.cpp`) runs inside `os_host_block_begin/end`, like vsync on the Switch, so pinned loader threads (`ResourceUncompress` p31) run during the sleep. `SNAP_NVN_RELEASE_CORE=1` (pass 3, still opt-in) releases the core for the whole submit/present/wait; it added nothing measurable on top.
- `SNAP_SHADER_FASTIO=0`: shader cache files (`.spv`, `.json`, `.efz`) read with `std::ifstream` byte iterators and one BCrypt provider open per hash, as before. Default: one sized Win32 read and one shared provider (`runtime\nvn\nvn_shader.cpp`). Boot creates about 6,200 programs.
- `SNAP_OS_SPARE_CORES=<n>` (OFF by default): n extra virtual cores any thread may take whatever its mask; guests still see cores 0..2 (`GetCurrentCoreNumber`, core masks). One run: boot to title 20 s, but the lab hub then ran at about 9 fps with JobWorkers blocked on locks (the job system at `0x54c2ec` reads the core number, and two threads now "share" a core). Do not use for play.
- Not switchable: `NVN_DRAW_SIG`, `NVN_RT_DUMP` and `NVN_TEX_DUMP` are read once instead of by `getenv` per draw (`runtime\nvn\nvn_draw.cpp`; `getenv` was 4% of the exe samples).
- Tools: `SNAP_HLE_TRACE=open` logs only fs opens (load-phase markers: `title.drp` = title screen, `LC_101_A.drp` = lab, `LC_001_A.drp` = Florio). `SNAP_HOST_PROF=<file>` (`runtime\os\hostprof.cpp`, debug) samples the RIP of every thread that ran in the last 2 ms between `SNAP_HOST_PROF_FROM` and `_TO` seconds and writes `<exe offset> <count>` lines; map them with `snap_boot.map` (most samples land in ntdll waits; compare exe functions with each other).

## Frame pacing and vsync (2026-11-13/14; `analysis\perf\pacing.md`, `runtime\nvn\nvn_pace.{h,cpp}`)

- `SNAP_PACE=legacy`: the old frame limiter (steady 60 Hz schedule, 1.5 ms spin), the old free-running 16667 us guest vsync thread and the queue lock held across the acquire. Default: one frame clock for the limiter and the guest `nn::vi` vsync, locked to the monitor's vblanks when its refresh is a whole multiple of 60 (60, 59.94, 120 Hz), a steady-clock grid otherwise (144 Hz).
- `SNAP_PACE_LOG=<file.csv>`: one row per present (release interval, guest work, limiter wait, present wait, acquire, `vkQueuePresentKHR`, vsync ticks and phase, mode). Test overrides: `SNAP_PACE_MONITOR=<n>` (follow `\\.\DISPLAY<n>`), `SNAP_PACE_REFRESH=<hz>` (a value below the real refresh is flagged within 3 vblanks and paced on the steady clock at the target rate), `SNAP_PACE_WAIT=0` (no `VK_KHR_present_wait` queue cap), `SNAP_PACE_VBLANK=0` (no vblank thread), `SNAP_VSYNC_HZ=<hz>` (legacy vsync loop period).
- `SNAP_VSYNC=on|adaptive|fast|off` (default `on`): swapchain present mode only. `on` = FIFO (no tearing; today's behavior). `adaptive` = FIFO_RELAXED (tears only when a frame misses its vblank). `fast` = MAILBOX (no tearing, lowest latency; the GPU may render frames that are never shown). `off` = IMMEDIATE (tearing; when locked to the display the frame is released 0.5 ms after the vblank, so the tear line sits near the top). A mode the driver does not offer falls back (`off` to MAILBOX, then FIFO; the others to FIFO); the choice is logged once as `[vsync] SNAP_VSYNC=<v> -> present mode <M>`. Every mode keeps the frame limiter (60 fps or the frame cap) and the guest's 60 Hz vsync, so ride and game timing do not change; with a VRR (G-Sync/FreeSync) display use `on` or `adaptive`. Aliases: `relaxed`, `mailbox`, `immediate`/`0`. Live switch: `extern "C" void snap_vsync_set(const char* mode)` (same values) recreates the swapchain at the next present, no restart; `snap_vsync_get()` returns the active value. Menu: the Vsync row (native Output tab, F1 Graphics) calls it and stores settings key `vsync` (2026-11-14). A game asking for present interval 0 still gets IMMEDIATE/MAILBOX as before.

## Ray tracing track (2026-11-14, rt worker; `runtime\nvn\nvn_rt.{h,cpp}`, `docs\lighting-rt-plan.md` section 4)

Everything is off unless `SNAP_RT=1`: with it unset the device, shader modules, pipelines and buffers are what they were. The device needs `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, `VK_EXT_transform_feedback` and the Vulkan 1.2 `bufferDeviceAddress` feature; if any is missing the log says `[rt] off: ...` and the game runs as before. Memory pools the game creates are not touched (only the layer's own buffers carry device-address flags).

- `SNAP_RT=1`: master switch. `SNAP_RT_CAPTURE=0`: device features only (M0), no vertex patch, no capture.
- `SNAP_RT_DEBUG=1|2|3`: ray-query overlay after the tonemap draw. 1 = sanity colors (green: ray hit matches the scene depth, red: hit in front of the depth, blue: hit behind it, magenta: depth but no hit, orange: hit on a sky pixel), 2 = hit-distance heat map, 3 = statistics only. `SNAP_RT_FLIPY=0|1` overrides the clip-y to screen-y mapping (default follows the window origin mode).
- `SNAP_RT_LOG=<n>`: stats every n presents (default 120 with `SNAP_RT_DEBUG`, off otherwise): triangles and draws captured, BLAS builds, live cache entries and memory, CPU record cost, GPU ms of convert+BLAS+TLAS (timestamps) and of the overlay, and the overlay's per-pixel class percentages.
- `SNAP_RT_ARENA_MB` (384), `SNAP_RT_BLAS_MB` (512), `SNAP_RT_SCRATCH_MB` (256): capture arena, BLAS storage and build scratch. A draw that does not fit is skipped and counted.
- `SNAP_RT_TTL=<frames>` (900): how long a draw seen in 3 or more frames stays in the TLAS after it last drew; draws seen fewer than 3 times drop after 2 frames.
- `SNAP_RT_KEYCB=0|1` (1): the draw identity for the BLAS cache includes a hash of the per-object vertex constants (tells apart draws that share one mesh); 0 keys by mesh identity only.
- Photo and Evaluation frames get no capture and no build (`FxIsPhotoFrame`, `FxWritable`); scene depth is only sampled, never written. Validation: `NVN_VK_VALIDATION=1`.
