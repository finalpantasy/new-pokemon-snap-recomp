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
  game), "Import save from folder", "Import test save" (folders in `buildixtures\saves\`), and "Restore from trash". Clear and Delete move the data to
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
- **Settings menu (F1) option pages** (`runtime\ui\ui_options.cpp`, table-driven): Graphics (render scale, anti-aliasing, sharpening, anisotropic filtering, texture sharpness, shadow resolution, ambient occlusion, color look and strength, HDR + peak and paper-white nits, frame cap, UI font scale, field of view 1.0-1.4 with `SNAP_FOV_SITES=0x296400,0x38cdbc`, draw distance 1-4 and full-rate distant animation, the last three marked "may change photo scores, being tested"), Display (window mode, monitor, windowed size, pause when unfocused, mute in background, fast boot), Photos (PNG export on/off, folder, open folder, gallery), Mods (packs with enable box and load order, written to `mods.json`) and the output mode on the Audio tab. Every row shows "applies now" or "restart needed" (yellow "changed: restart to apply" once changed); a help panel on the right gives plain-language help, the default, the effective render size and the env override name. A bar under the title counts pending restart changes and "Apply and restart" calls `restart_now`.
- **Coming soon (disabled rows, env already wired in `settings.cpp`):** Upscaler (`SNAP_UPSCALER`), Upscaler quality (`SNAP_UPSCALE_QUALITY`), Lighting preset (`SNAP_LIGHTING`). Flip `kSoon` in the option table when the feature code lands.
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
The player name is fixed to "Ash" (no on-screen keyboard yet; `SNAP_PLAYER_NAME` overrides it).
Scripted input for unattended runs: `SNAP_INPUT_SCRIPT=<file>` (lines `<start> <hold> <buttons or lx= ly= rx= ry=>`, `repeat <start> <end> <period>
<hold> ...` and `anchor <event>`; seconds since the first poll or since the event), OR-ed onto the real input. `host\skip_intro.txt` is the intro script.

### Frame-based script times (load-independent)

A script line whose start is written `@f<frame>` counts guest presents (the NVN present count) since the segment began instead of seconds:
`@f600 30 A` holds A from present 600 for 30 presents; `repeat @f0 @f3000 90 15 A` taps A for 15 presents every 90. End, period and hold take
an optional `@f` prefix (frames either way on such a line). Time lines and frame lines can be mixed in one file. At 60 fps 60 presents is
1 s; under load the game just takes longer in wall time and the route stays in step. Implemented in `runtime\hle\input_core.cpp`
(the script player lives there, not in `hid.cpp`).

### Parallel runs (owner exception to the one-heavy-process rule)

Locks (`host\locks.ps1`; a lock file holds `owner pid=<pid> <time> <what>`, stale when the pid is dead or it is older than 20 min):

- **Build lock** `build\heavy.lock`: one build (`host\build_snap.py`, `snap_build`) at a time. Unchanged.
- **Run slots** `build\slots\run0..3.lock`: `host\run_boot.ps1` takes one (up to 4 hidden runs at once; owner 2026-11-12, cut from 6 because 6 held the CPU at 100%), waits and prints the owners when full,
  releases in a finally block. While `build\heavy.priority` exists slot 0 is only for the run with `$env:SNAP_LOCK_OWNER` equal to its content.
- **Muted**: hidden runs (`run_boot.ps1`, `run_boot_mods.ps1` without `-Visible`) default to `SNAP_AUDIO_BACKEND=null`: no sound reaches the speakers, while audio timing and `SNAP_AUDIO_CAPTURE` work as before. Set `SNAP_AUDIO_BACKEND=wasapi` only for a test that needs the real device (spatial or WASAPI channel checks), and then mute the game with `SNAP_VOL_MASTER=0` unless the sound itself is under test.
- **Own builds**: `host\build_snap.py --out build\w\<owner>` relinks into a private folder without `build\heavy.lock`; run it with `run_boot.ps1 -BuildDir build\w\<owner>` (or `$env:SNAP_BUILD_DIR`). A build dir without its own `nvn_cache` gets the warm one from `build\snap`. Take `heavy.lock` only to write `build\snap`, rebuild the main/sdk libs (`recomp\snap_build.py`) or regenerate main.
- **Exclusive**: `-Bench`, `-Exclusive` or `SNAP_PERF` set: takes `build\slots\exclusive.lock`, then all 4 slots; new normal runs wait behind it.

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
