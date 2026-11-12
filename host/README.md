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
    powershell -File host\play.ps1 -SkipIntro   scripted input through loading, name entry and the lab intro (about 3.5 minutes), real input stays active
    powershell -File host\play.ps1 -ListPads    list the controllers the game sees and show live pad input for 15 s

`play.ps1` takes `build\heavy.lock` only while it copies `build\snap\snap_boot.exe` (plus `SDL3.dll`, the PDB and `nvn_cache`) to `build\play\`,
releases it, creates `build\play.lock` (workers must not start boot runs while it exists), runs the copy at normal priority with no timeout and
validation off, and deletes `play.lock` when the game exits (window closed, crash or Ctrl+C). Builds can continue during play. Logs:
`analysis\boot\play_stderr.log`, `play_stdout.log` (device list), `play_trace.log`. Click the game window once so it has focus: keyboard and
gamepad input are ignored while another window is in front.

Saves: the player's save is `%APPDATA%\SnapRecomp\save` (`SNAP_SAVE_DIR` overrides it). `host\run_boot.ps1` and all test runs use
`build\testsave`, wiped per run (`-KeepSave` keeps it), so tests never touch the player's save. The game writes the profile (language, voice,
appearance, name) when "Are you ready to start?" is confirmed; a later launch skips those screens and goes straight to the opening cutscene. No
later write was seen up to the first tutorial step, so the opening cutscene and tutorial replay until the game saves its progress.

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
- **Accessibility tab:** toggle-or-hold for ZL and ZR, UI scale.  **Test input tab:** live readout of what npad 0 would report.
- Settings apply live and are saved atomically to `%APPDATA%\SnapRecomp\input.json` (`SNAP_CONFIG_DIR` overrides the folder). An invalid file is kept as
  `input.json.invalid`, defaults are used and a notice shows on screen. The old `config\input_map.json` is migrated once.
- Test: `tests\hleun_tests.ps1` (binding resolution, JSON round trip, migration, capture) and `tests\hleun_ui_test.ps1` (boots the game, posts F1,
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

The game polls only the Full-key (Pro controller) npad 0 state, the touch screen and gestures; it never asks for six-axis (motion) data.
Pads: every gamepad SDL3 reports is opened and merged with every XInput slot into npad 0 (buttons OR-ed, larger stick value wins), the
joystick list is rescanned every second (hot-plug, wake-up) and each device is printed (`pad:` lines in `play_stdout.log`). SDL3 is used when
`tools\dl_sdl3\x\SDL3-3.4.20` exists (the build copies `SDL3.dll` next to the exe); XInput is always read as well.
The player name is fixed to "Ash" (no on-screen keyboard yet; `SNAP_PLAYER_NAME` overrides it).
Scripted input for unattended runs: `SNAP_INPUT_SCRIPT=<file>` (lines `<start> <hold> <buttons or lx= ly= rx= ry=>`, `repeat <start> <end> <period>
<hold> ...` and `anchor <event>`; seconds since the first poll or since the event), OR-ed onto the real input. `host\skip_intro.txt` is the intro script.

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
