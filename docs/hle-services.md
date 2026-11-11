# Horizon service HLE (runtime/hle)

Host overrides for every non-os, non-audio, non-NVN `nn::` service that New Pokemon Snap reaches. Each override is a
plain `void(Cpu*)` function keyed by its mangled symbol (TotK `HOST_OVERRIDE` convention): arguments come from the
AAPCS64 registers in `Cpu`, guest pointers are host pointers. Nothing here depends on the lifted game, so `tests/hle`
runs without it.

Layout (all under `runtime/hle/`):

| File | Content |
| --- | --- |
| `hle.h`, `overrides.cpp` | `Entry` registry, `HLE_IMPL` / `HLE_NOOP`, `bind_all`, logging, `unimplemented()` backstop, `fatal()` |
| `hle_config.*`, `json_lite.h` | Config file (`hle.json`), minimal JSON reader/writer with `//` comments |
| `fs_core.*`, `fs.cpp` | Host file system (rom, save, host mounts) and the `nn::fs` shims |
| `input_core.*`, `input_backends.cpp`, `hid.cpp` | Mapping table, Win32 and SDL3 backends, `nn::hid` shims |
| `services_sys.cpp` | oe, pctl, settings, time, account, err, diag, album, ngc, swkbd, vi, crypto, friends, codec |
| `services_net.cpp` | nifm, nsd, prepo, socket, ssl (offline) |
| `reg_sys.inc`, `reg_net.inc` | One registration per line, generated from `analysis/runtime/imports_usage.tsv` |
| `gen_hle_stubs.py` | Writes `analysis/runtime/hle_coverage.tsv` and `generated/hle_unimplemented.cpp` |
| `PROPOSED_SHARED_CHANGES.md` | What the loader, `runtime/os` and `runtime/nvn` need to do |

## Coverage

`analysis/runtime/hle_coverage.tsv` has one row per import of `main` (731) plus 14 rows for overrides of symbols that are
not `main` imports. Totals for the 731:

| Status | Count | Meaning |
| --- | --- | --- |
| implemented | 153 | Real behavior in `runtime/hle` |
| stub-noop | 67 | Registered, does nothing on purpose (prepo, nv, init/finalize calls) |
| lifted-pure-software | 408 | Runs as lifted guest code: libc, libc++, libc++abi, nn::util, fontll, image (JPEG), mem, crypto AES/MD5/SHA1, swkbd arg builders, nn::result, weak data symbols |
| todo | 103 | os 86 (`runtime/os`), audio 16 (`runtime/audio`), nvn 1 (`runtime/nvn`). Zero are owned by `runtime/hle` |

`nn::` imports owned here that have no override log their name, caller (x30), sp and x0-x7 (plus readable C-string
arguments) and abort: `hle::unimplemented()`. `gen_hle_stubs.py` creates one such stub per leftover import
(`generated/hle_unimplemented.cpp`); today that list is empty. `--all` also stubs os, audio and nvn imports.

Regenerate after any registration change: `python runtime/hle/gen_hle_stubs.py`.

## nn::fs

### Mounts and boot path (evidence in `main`, module offsets)

- `nnMain` builds the strings "rom", "save", "sdmc" (0x4e3f70..0x4e4138). Only `MountRom("rom", cache, size)` runs
  (0x4e4138), after `QueryMountRomCacheSize` (0x4e410c). Paths are built as `rom:/...` (prefix at 0x4e4150-0x4e41cc).
  `rom:` maps to `extracted/romfs-v262144/` (config `romfs_root`), read-only.
- Save init is one function (0x4d5300-0x4d9cf4): `account::Initialize` (0x4d7a0c), `GetUserCount` (0x4d7a14),
  `TryOpenPreselectedUser` (0x4d7a30), `GetUserId` (0x4d7a44), `GetNickname` (0x4d7a60),
  `fs::GetSaveDataSize(&size, &journal, uid)` (0x4d7b00), `MountSaveData("savedata", uid)` (0x4d7b10, string at 0xb0aad6).
- Save file names use the format `savedata:/%08x%08x` (0xaf02b0). `Unmount("savedata")` at 0x4e146c.
  `Commit("savedata")` at 0x6072f4, 0x60744c, 0x607564, 0x607678, 0x607790, 0x6078ac, 0x609710.
- `GetFreeSpaceSize` at 0x609a30 requires 100 MiB or more (`>> 0x16 >= 0x19`) and at 0x6107c8; on shortage the game calls
  `ExtendSaveData(uid, size + need, journal)` (0x610800). The host reports at least 4 GiB free so that path never runs.
  `GetSaveDataSize` reports `save_data_size` (128 MiB) and `save_journal_size` (32 MiB): the NACP values were not found,
  these are assumptions.
- The SDK libc inside `sdk` reaches `CreateFile`, `GetFileSize`, `SetFileSize`, `GetEntryType`, `OpenDirectory`,
  `ReadDirectory`, `CloseDirectory`, `GetDirectoryEntryCount`, `OpenFile`, `CloseFile`, `WriteFile`: all overridden.

### Result codes

`nn::Result` = module in bits 0-8, description in bits 9-21. The game classifies fs failures at 0x602c90: module must be
2 (`w & 0x1ff`), description is `w & 0x3ffe00`.

| Result | Value | Evidence |
| --- | --- | --- |
| PathNotFound | 2-1 (0x202) | description 1 (0x200) maps to game error 0xd |
| PathAlreadyExists | 2-2 (0x402) | `CreateFile` compares `& 0x3fffff == 0x402` at 0x602f0c; maps to game error 0xf |
| TargetLocked | 2-7 (0xe02) | description 7 (0xe00) maps to game error 0x11 (0x60362c) |
| UsableSpaceNotEnough | 2-31 | `ubfx w9,w0,#9,#0xd; sub #0x1e; cmp #0x10` at 0x603614: descriptions 30..45 map to game error 0x10 |
| DirectoryNotEmpty | 2-8 | SDK value, not seen in the game |
| OutOfRange | 2-3005 | SDK value for a short strict read |
| InvalidArgument, NotMounted, WriteNotPermitted | 2-6001, 2-6002, 2-6003 | host-chosen; the game treats any other nonzero as a generic failure |

Other facts the shims follow: `GetEntryType` gives 0 = directory, 1 = file (0x8bd348); `OpenDirectory` mode 1 is used
(0x8bd38c); the 4-argument `ReadFile` is strict (the generic reader at 0x8d9840 treats nonzero as failure), so a read past
the end returns OutOfRange; `WriteFile` honors the WriteOption flush bit (bit 0).

### Layered overlay (mod-loader hook)

`Fs::set_rom_layers({dirA, dirB, ...})` or config `rom_layers`: an ordered list, first match wins, `romfs_root` is
always last. File lookup, entry types and directory listings merge all layers (a name present in two layers is listed
once, the first layer's file wins). Layers are read-only; `extracted/` is never written.

### Save data design

Goal: the save on disk is always one complete committed state, even if the process dies at any instruction.

```
<save_root>\<uid>\            default %APPDATA%\SnapRecomp\save\<32 hex digits of the uid>\
  CURRENT                     "SNAPSAVE1\n<n>\n<check>\n"  pointer to the live generation
  gens\<n>\                   complete state of generation n, immutable once committed
  gens\<n>.ok                 marker, written after generation n is complete
  working\                    copy-on-write scratch for the mounted session (wiped at mount)
```

- Writes after `MountSaveData` go to `working/` (files are copied on first write). Nothing is visible to the next mount
  until `Commit`.
- `Commit` builds generation n+1: unchanged files are hard-linked (copied if linking fails), dirty files are copied, then
  the marker is written, then `CURRENT.tmp` is written and moved over `CURRENT` with
  `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)` (rename on POSIX). The pointer replace is the single commit point.
- Recovery at mount: read `CURRENT`; if it is missing or invalid use a valid `CURRENT.tmp`; otherwise scan for the newest
  `.ok`. Generations newer than the pointer (a crashed commit) are deleted.
- Automatic backups: the newest `save_backups` (default 5) older generations are kept next to the current one, older ones
  are pruned. `Fs::list_generations` and `Fs::restore_generation` expose them; restore commits a copy of the chosen
  generation as a new one, so it is atomic too.
- Test fault hook stages: `commit:begin`, `commit:carried`, `commit:file`, `commit:files-done`, `commit:marker`,
  `commit:pointer-tmp`, `commit:pointer-done`. Crash at any stage before `pointer-done` leaves the old save; at
  `pointer-done` the new save (tested).
- `Unmount` without `Commit` discards the working copy, like the console.

## nn::hid

Pipeline: backend (Win32 XInput + keyboard + mouse, or SDL3) fills `RawInput` -> mapping table -> `PadSample` /
`TouchSample` -> the shims fill `NpadState` (0x28 bytes: `{s64 seq; u64 buttons; s32 lx, ly, rx, ry; u32 attrs}`).

- Npad: `controller_style` 0 (default) = Pro Controller style (FullKey, id No1); 1 = Handheld (id 0x20, attributes 0x3f).
  The game lists No1 and 0x20 (0x9258f4, 0x925944) and sets styles 3 = FullKey | Handheld (0x925948), so both work.
  All four `GetNpadState` variants exist; other ids and styles read as empty. `ShowControllerSupport` reports one player.
- Touch: the left mouse button is one finger on the 1280x720 panel (`TouchScreenState<6>`, 0x100 bytes, 0x28-byte touches).
  Gestures report none.
- Six-axis: one handle per npad (a 4-byte word; 0x7fffffff means none, 0x4c1514-0x4c152c), at rest, identity matrix
  (`GetSixAxisSensorStates` stride 0x60, angular velocity at +0x1c, 0x4c15ec). No PC gyro source yet.
- Vibration: the two actuators forward to XInput / SDL rumble.
- Focus: when the window is not in the foreground all input reads as released (env `SNAP_INPUT_BACKGROUND=1` disables).
- Default mapping (Nintendo layout, A = east button). First run writes it to `%APPDATA%\SnapRecomp\config\input_map.json`.

| Switch | Gamepad | Keyboard / mouse |
| --- | --- | --- |
| A / B / X / Y | east / south / north / west | L / K / I / J |
| L / R | left / right shoulder | Q / E |
| ZL / ZR | left / right trigger (threshold 0.5) | Left Shift / Space |
| Plus / Minus | start / back | Enter / Backspace |
| D-pad | d-pad | T / F / G / H (up / left / down / right) |
| Left stick | left stick, radial dead zone 0.15 | W A S D |
| Right stick | right stick | arrow keys |
| Touch | | left mouse button |

Schema (JSON, `//` comments allowed, every key optional, missing keys keep the default; an invalid file is rejected as
a whole with a message and the old table stays):

```json
{ "version": 1,
  "trigger_threshold": 0.5,
  "mouse": { "touch": true, "look_sensitivity": 0 },
  "buttons": { "A": ["pad:east", "key:L"], "ZR": ["pad:rtrigger", "key:Space"] },
  "left_stick":  { "analog": "left",  "deadzone": 0.15, "up": ["key:W"], "down": ["key:S"], "left": ["key:A"], "right": ["key:D"] },
  "right_stick": { "analog": "right", "deadzone": 0.15, "up": ["key:Up"], "down": ["key:Down"], "left": ["key:Left"], "right": ["key:Right"] } }
```

Sources: `pad:<south|east|west|north|back|guide|start|lstick|rstick|lshoulder|rshoulder|dpad_up|dpad_down|dpad_left|dpad_right|ltrigger|rtrigger>`,
`key:<name>` (A-Z, 0-9, F1-F12, Up/Down/Left/Right, Space, Enter, Escape, Tab, Backspace, LShift ..., Numpad0-9),
`mouse:<left|right|middle>`. Button names: A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down. Positional
pad names follow SDL3, so one table serves XInput and SDL (south = Xbox A). `look_sensitivity` above 0 lets mouse
movement drive the right stick.

## nn::oe, nn::applet

`GetOperationMode` returns Console (docked) unless config `docked` is false. Focus state stays in-focus:
`TryPopNotificationMessage` never returns a message. `SetPerformanceConfiguration(mode, config)` (called at
`main 0x638a84` with `0, 0x20004`) is accepted and logged under `SNAP_HLE_TRACE=svc`. `GetDesiredLanguage` returns the
language code for config `language`. `GetProgramTotalActiveTime` returns process uptime. The imports do not include a
display-version or performance-mode query, so `display_version` is stored but nothing reads it.

## Other services

| Service | Behavior |
| --- | --- |
| time | UTC only. `GetCurrentTime` / `now()` from the host clock; `ToCalendarTime`, `ToPosixTimeFromUtc` real; location "UTC" |
| settings | `LanguageCode::Make`, `operator==` with the 18-entry Language table (1 = en-US default); `region` is stored, not read by any import |
| account | One local user, name from `user_name` ("Default"), uid from config `uid` (default `534e415052454331` `0000000000000001`). Network service account calls fail with the offline result |
| err | `ApplicationErrorArg` constructor fills the real layout (error at 0, language at 8, dialog at 16, fullscreen at 2064); `ShowApplicationError` and `ShowError` log and show a message box unless `headless` |
| diag | `AbortImpl` and `DiagAbortFormat` log expr, function, file, line and abort |
| album | `SaveScreenshot` writes an RGBA8 PNG to `screenshot_dir` (default `%APPDATA%\SnapRecomp\screenshots`) and returns success |
| swkbd | `ShowKeyboard` returns a canceled result (no on-screen keyboard) |
| ngc | Profanity filter initializes; `CountNumbers` counts digits |
| vi | Only what NVN needs: tokens for display, layer and native window; vsync event at 60 Hz via the os hooks |
| nifm, nsd, prepo | Offline: `IsNetworkAvailable` false, requests fail, `nsd::ResolveEx` echoes the name, prepo calls succeed and store nothing |
| socket | Every call fails with -1 and errno ENETDOWN (50, FreeBSD numbering), `Close` with EBADF; byte-order and inet helpers are real; `GetAddrInfo` returns EAI_NONAME |
| ssl | Contexts and connections fail with an offline result; destroy calls succeed |
| friends, pctl, codec, nv | No-ops (`pctl::TryBeginFreeCommunication` and `IsFreeCommunicationAvailable` return true) |
| crypto | `GenerateCryptographicallyRandomBytes` uses a host RNG; the AES/MD5/SHA1 code is lifted |
| mem, util, fontll, image | Pure software, lifted, no override |

Host-chosen result codes (the game only tests success versus failure, 25 call sites): nifm 110-300, ssl 123-1,
account 124-5, swkbd 101-1.

## Config

`%APPDATA%\SnapRecomp\config\hle.json` (env `SNAP_HLE_CONFIG` overrides the path). All keys optional.

| Key | Default |
| --- | --- |
| `romfs_root` | `extracted/romfs-v262144` (env `SNAP_ROMFS`) |
| `rom_layers` | `[]` (mod directories, first match wins) |
| `data_root` | `%APPDATA%\SnapRecomp` |
| `save_root` | `<data_root>\save` (env `SNAP_SAVE_ROOT`) |
| `save_backups` | 5 |
| `save_data_size`, `save_journal_size` | 128 MiB, 32 MiB (assumed) |
| `user_name`, `uid` | "Default", 32 hex digits |
| `language`, `region` | 1 (en-US), "US". Language numbers: 0 ja, 1 en-US, 2 fr, 3 de, 4 it, 5 es, 6 zh-CN, 7 ko, 8 nl, 9 pt, 10 ru, 11 zh-TW, 12 en-GB, 13 fr-CA, 14 es-419, 15 zh-Hans-CN, 16 zh-Hant-TW, 17 pt-BR |
| `docked` | true |
| `controller_style` | 0 (Pro Controller); `"handheld"` or 1 for Handheld |
| `input_map` | `<data_root>\config\input_map.json` |
| `screenshot_dir` | `<data_root>\screenshots` |
| `headless` | false (env `SNAP_HLE_HEADLESS=1`): error dialogs only log |

Trace: `SNAP_HLE_TRACE=fs,hid,svc` or `all`.

## Not verified (assumptions)

- `save_data_size` / `save_journal_size` values (NACP not seen); only that `GetSaveDataSize` succeeds matters.
- `album::SaveScreenshot` `ImageSize` call site was not found; 0 = 1280x720, 1 = 1920x1080, anything else is derived from
  the data size.
- Result values other than the four in the table above are SDK recollection or host-chosen.
- `vi` vsync and `account` async events need the os hooks (PROPOSED_SHARED_CHANGES.md); until `runtime/os` provides them
  the events never fire.
- Windows only in practice: the Win32 backend and `MoveFileEx` commit are tested; the POSIX branches compile paths exist
  but were not run.

## Build and test

```powershell
powershell -File tests\hle\run_tests.ps1        # builds tests\hle\out\hle_tests.exe with clang++ and runs it at BelowNormal
python runtime\hle\gen_hle_stubs.py             # coverage TSV and generated stubs
```

The tests (`tests/hle/hle_tests.cpp`, about 750 checks) call the overrides through the registry with a hand-built `Cpu`:
SHA-256 of 42 RomFS files against `analysis/romfs/files.tsv`, directory listing, overlay precedence, result codes; save
write / commit / reload through the game's own shim calls, crash mid-write, crash at each of the seven commit stages,
backups and restore; fake gamepad, keyboard and mouse through the default and a custom JSON mapping into Npad and touch
states; account, oe, time, album PNG, offline sockets and the unimplemented backstop. They only read `extracted/`.
