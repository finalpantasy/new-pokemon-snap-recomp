# Command line

Every option the launcher offers for starting the game also has a flag, so another launcher, a shortcut or a script can start Snap Recomp without our window. `SnapRecomp.exe --help` prints the same list.

```
SnapRecomp.exe                      open the launcher
SnapRecomp.exe --play [options]     start the game without the launcher window
```

## Options

| Flag | What it does |
| --- | --- |
| `--play` | Skips the launcher window. The game-folder check still runs first. The process ends when the game closes, so the caller can wait on it. No window of ours opens: a start that cannot go on prints the reason and ends with exit code 1 (the game folder failed its check) or 2 (the converted files are missing or damaged). The first-start upscaler offer waits for a start without `--play`, and after a start that failed the safe-mode question becomes a printed line; add `--safe-mode` to use it. |
| `--safe-mode` | One start with mods, HD packs, enhanced effects and the shader cache off ([safe-mode.md](safe-mode.md)). |
| `--resume` | After a session that ended without closing (a crash or a kill), skips the title screen and menus back to the lab menu or the course map, whichever was open last. Does nothing after a clean exit. With `--play` the question is never asked: this flag is the answer ([crash-resume.md](crash-resume.md)). |
| `--crash-prompt on\|off` | The "The game closed unexpectedly" question in the launcher, and the marker that makes it possible. This start only; config.json `crash_prompt` is the stored setting. |
| `--fullscreen`, `--borderless` | Borderless full-screen window. Both names do the same thing. |
| `--windowed` | Normal window. |
| `--steam-deck` | The Steam Deck preset: borderless 1280 x 800, a lighter 3D picture, a 150% HUD and Xbox button icons ([steam-deck.md](steam-deck.md)). Flags after it win. |
| `--monitor <n>` | Monitor number. 0 is the first. |
| `--width <px>`, `--height <px>` | Window size. |
| `--pause-in-background on\|off` | Pause the game while its window is in the background (the F1 and launcher Window row). This start only. |
| `--mute-in-background on\|off` | Mute the game while its window is in the background. This start only. |
| `--shiny <off\|on\|n>` | Rare shiny Pokemon (needs the shiny pack): `on` is 1 in 512, a number is 1 in that many, `1` makes every Pokemon shiny (test). Colors only; scores, spawns and saves do not change. This start only. |
| `--mods-readahead on\|off` | Read the HD pack files of the course that is loading into the file cache ahead of the game (key `mods_readahead`). This start only. |
| `--shader-workers <n\|auto>` | Shader translator processes when shaders are missing from the translated-shader cache (the first launch): the game starts at once and that many processes translate in the background; a draw that needs a shader not done yet waits for it, so nothing is drawn wrong. `auto` (default) is half the logical processors, at most 8; `1` is the old way, one shader at a time while the game loads (minutes). Variable `SNAP_XLATE_WORKERS`. [shader-prebuild.md](shader-prebuild.md). This start only. |
| `--fps <n>`, `--fps unlocked`, `--fps monitor` | Frame rate target (settings key `fps_target`, `SNAP_FPS`): any rate from 61 to 1000 (120, 128, 144, 165, 180, 240, 360 ...), `unlocked` (as fast as the PC allows), `monitor` (the monitor's refresh rate) or `default` (the game's own 60). Real frames: the game keeps its speed. This start only. |
| `--frame-cap <fps>` | Frame rate limit. 0 is no extra limit. This start only. |
| `--vsync on\|adaptive\|fast\|off` | Present mode: On is FIFO, Adaptive is FIFO_RELAXED, Fast is MAILBOX, Off is IMMEDIATE. A mode the driver lacks falls back to FIFO (Off tries MAILBOX first). This start only. |
| `--gpl auto\|on\|off` | Vulkan graphics pipeline libraries. A pipeline the game needs before its compile finished is fast-linked from pre-built parts and replaced by the full compile in the background. Auto is off on every GPU for now (list and evidence in `runtime/nvn/nvn_quirks.cpp`): measured on NVIDIA it gave no fewer hitches and doubled the compile work. On is for testing. Settings key `gfx_gpl`, variable `SNAP_GPL`. This start only. |
| `--distant-scenery off\|high\|ultra` | Farther model detail and draw distance, display only (Original is off). High is 3x detail distance and 2x draw distance, Ultra 8x and 4x. Photo scoring keeps the game's own picks. This start only. See [draw-distance.md](draw-distance.md). |
| `--aniso off\|2\|4\|8\|16` | Anisotropic texture filtering. Off is the game's own 1x. This start only. |
| `--hdr-photos on\|off` | Also write `<name>.hdr.png` (16-bit PNG, PQ, BT.2020) beside each HD photo copy and screenshot. Needs HDR output (`--set gfx_hdr=hdr10` or `scrgb`); with it off nothing is written. Highlights are reconstructed from the normal picture, not captured from the scene. Settings key `hdr_photos`, variable `SNAP_HDR_PHOTOS`. docs/hdr-photos.md. This start only. |
| `--update-check on\|off` | Check for updates when the launcher starts. This start only; config.json is not changed. |
| `--update-channel stable\|testing` | Which update channel the check follows. This start only. |
| `--backup on\|off` | Copy the profile to the backup folder when the game closes (and for `--backups now`). This start only; the launcher's Backups page keeps the saved choice. |
| `--backup-folder <folder>` | The folder the backup zips go in, for example a OneDrive or Dropbox folder. |
| `--backup-keep <n>` | Zips kept per profile, 1 to 100. The default is 3. |
| `--backup-include <list>` | What goes in: any of `saves`, `settings`, `photos`. The default is all three. HD packs and shader caches are never included. |
| `--profile <name>` | The profile to play, or the one `--import-save` and `--export-save` use. |
| `--set <key>=<value>` | One setting for this start only. Repeat it for more settings. The settings file is not changed. |
| `--data-dir <folder>` | Keeps saves, settings, mods, caches and logs in this folder. The folder is created when it is missing. |
| `--no-prompt` | Never opens a question window. A game folder the exe cannot write to ends the start with exit code 3 and a message on stderr unless `--data-dir` is given ([readonly-install.md](readonly-install.md)). `--play`, the launcher actions and the other commands already behave this way. |
| `--help`, `-h`, `/?` | Prints the flag list and exits with code 0. |
| `--version` | Prints the version and exits with code 0. |

Without `--play` the launcher window opens with the flags already applied, and they hold for the game it starts.

When two window flags disagree, the last one on the line wins. A flag wins over an environment variable, and an environment variable wins over the settings file.

A mistyped flag stops the start: the exe prints `SnapRecomp: unknown option --name (see --help)` and exits with code 2. A flag with a missing or bad value does the same.

## Examples

```
SnapRecomp.exe --play --fullscreen --monitor 1
SnapRecomp.exe --play --steam-deck
SnapRecomp.exe --play --windowed --width 1280 --height 720 --profile Ash
SnapRecomp.exe --play --set fps_target=120 --set fps_overlay=fps
SnapRecomp.exe --play --data-dir D:\SnapData
```

## Keys for `--set`

`--set` takes the same keys the settings file (`settings.cfg`) uses, with the same values. A name that starts with `SNAP_` sets that environment variable for the start instead, for the options that exist only as a variable.

| Group | Keys |
| --- | --- |
| Frame rate | `fps_target`, `vsync`, `frame_generation`, `gfx_full_rate_anim`, `fps_overlay`, `fps_overlay_corner` |
| Resolution and upscaling | `gfx_render_scale`, `gfx_upscaler`, `gfx_upscale_quality`, `gfx_aa`, `gfx_smaa_quality`, `gfx_sharpen`, `gfx_ultrawide` |
| Detail | `gfx_aniso`, `gfx_lod_bias`, `gfx_draw_dist`, `gfx_lod_dist`, `gfx_lod_force0`, `gfx_pom`, `foliage_aa`, `leaf_translucency`, `soft_particles`, `specular_aa` |
| Lighting and shadows | `gfx_lighting`, `gfx_bounce`, `gfx_shadow_res`, `gfx_shadow_dist`, `contact_shadows`, `ssao_quality`, `gfx_ssao_samples`, `gfx_ssao_hbao` |
| Ray tracing | `rt_reflect`, `rt_gi`, `rt_gi_rays` |
| HDR | `gfx_hdr`, `gfx_hdr_peak`, `gfx_hdr_paper`, `gfx_hdr_expand` |
| Water | `water_ssr`, `water_caustics`, `water_foam`, `water_ocean` |
| Atmosphere and lens | `atmos_shafts`, `atmos_fog`, `atmos_exposure`, `atmos_bloom`, `atmos_clouds`, `atmos_sky`, `gfx_bokeh_dof`, `gfx_motion_blur`, `film` |
| Interface | `hud_scale`, `gfx_ui_font`, `ui_smooth`, `game_language`, `button_icons`, `swap_ab`, `win_size` |
| Aim feel (`docs/aim-feel.md`) | `stick_curve`, `stick_deadzone`, `gyro_on`, `gyro_sens`, `gyro_smooth`, `gyro_dead`, `gyro_avg`, `raw_mouse`, `mouse_window_ms` |
| Photos | `photo_export`, `photo_dir`, `hd_photos` |
| Play | `fast_boot`, `skip_cutscenes`, `skip_course_intro`, `neo_speed`, `freecam`, `freecam_freeze`, `spawn_rand`, `spawn_seed`, `return_to_launcher` |
| Preset | `gfx_profile` (`original`, `balanced`, `enhanced` or `max`: writes the preset's rows for this start, with no upscaler) |
| Window and sound | `win_mode` (`windowed`, `borderless`), `win_monitor`, `win_pause`, `win_mute`, `frame_cap`, `audio_output` (`stereo`, `speakers`, `spatial`), `master_volume`, `music_volume`, `effects_volume`, `voice_volume` (0 to 1) |
| Controller and mouse | `face_layout` (`position`, `nintendo`), `swap_sticks`, `left_invert_y`, `right_invert_y`, `toggle_zl`, `toggle_zr`, `mouse_look`, `mouse_sensitivity`, `mouse_invert_x`, `mouse_invert_y`, `mouse_touch`, `lock_cursor` (1 or 0) |
| Accessibility | `access_cvd`, `access_cvd_strength`, `access_flash`, `access_ui_size`, `access_contrast`, `access_mono`, `access_toggle` |

The window, sound and controller keys are held in `window.json`, `audio.json` and `input.json`, not in `settings.cfg`. `--set` turns each into a variable for this start (`SNAP_WINDOW_MODE`, `SNAP_VOL_MASTER`, `SNAP_CTL_<KEY>` and so on) and writes none of the three files. [option-map.md](option-map.md) lists every option with its key, its launcher page, its F1 tab and its tab in the game's own Settings screen.

Other keys from `settings.cfg` work too. Safe mode still ignores the settings it always ignores, even when `--set` names one.

## Data folder

A release build keeps everything the game writes in `portable_data` next to the built exe: `Roaming\SnapRecomp` holds saves, settings, profiles and mods, and `Local\SnapRecomp` holds caches and logs. The `portable.txt` file next to the built exe switches this on. [install-folder.md](install-folder.md) shows the whole folder.

`--data-dir <folder>` uses that folder in place of `portable_data`, with the same two subfolders. Nothing is copied into it, so a new folder starts with a fresh save. Import one with `--import-save` and the same `--data-dir`.

When the exe's own folder cannot be written (Program Files, a read-only share, Controlled Folder Access), a plain start asks whether to move the game or to keep its data in another folder; `--data-dir` skips the question. See [readonly-install.md](readonly-install.md).

## Launcher actions

Each of these does one launcher job without the launcher window. The game does not start. Use one at a time, without `--play`.

| Flag | What it does |
| --- | --- |
| `--update` | Installs the newest version. The update runs in its own window. Exit code 0 means it started, 1 means the builder is missing. |
| `--check-install` | Checks the game files and prints the result. Exit code 0 when everything is in order, 1 when something is wrong. |
| `--report` | Makes the problem report zip and prints its path. Nothing is uploaded. |
| `--restore-backup` | Prints the save backups, newest first. `--restore-backup latest` or `--restore-backup <number>` puts that one back; the current save is backed up first. Exit code 0 done, 1 not restored, 2 wrong argument. |
| `--backups` | The profile backups to the backup folder ([sync-backups.md](sync-backups.md)). No argument or `list`: the backups, newest first. `now`: make one. `verify [<number>]`: re-read one against its manifest. `restore <number>` or `restore latest`: put one into a new profile. `restore ... --into-current --yes`: replace the current profile's save instead. Exit code 0 done, 1 failed or skipped (the save is in use, mid-write or waiting for recovery), 2 wrong argument. |
| `--upscalers` | Prints DLSS, FSR and XeSS, whether each is installed, and where its license is. `--upscalers <name> --accept-license <name>` downloads that one from the vendor. Without `--accept-license` nothing is downloaded: the license is yours to accept. |
| `--hdtex` | Prints the HD texture state. With options it runs the HD texture tool with them, for example `--hdtex --level hd --dry-run`; `builder\HD-TEXTURES.md` lists the options. HD shadows: `--hdtex --cats shadow --pack-id hd-shadows --from-romfs <romfs folder> --level hd --shadow-source upscaled` (the game's maps enlarged) or `--shadow-source rebaked` (re-traced on the graphics card, upscaled where a re-bake does not match; needs ray tracing), and `--hdtex --level off --pack-id hd-shadows` turns both off. The exit code is the tool's. |
| `--mods` | Mod pack order and conflicts, no window: `list`, `conflicts [--files] [<id>]`, `order <id> ...`, `move <id> up\|down\|top\|bottom\|<n>`, `enable <id> ...`, `disable <id> ...`. Without arguments it lists. It reads and writes `mods.json` in the mods folder (see `docs/mods.md`). Exit 0 done, 1 write failed, 2 bad argument. |

None of these opens a window. `--restore-backup`, `--backups`, `--upscalers`, `--hdtex` and `--mods` take everything after them as their own arguments, so put `--data-dir` and the other player flags before them.

## Setup flags

The `SnapRecomp.exe` you download answers these itself. Every other flag goes to the built game.

| Flag | What it does |
| --- | --- |
| `--game <folder>` | The folder with your game files. Setup opens with it chosen. |
| `--build-dir <folder>` | Where the build goes and where the exe looks for it. The default is the exe's own folder. |
| `--yes` | Builds without questions: accepts Microsoft's C runtime license, uses the defaults and closes when done. Exit code 0 means built, 1 failed, 2 no `--game` given. |
| `--setup` | Opens setup even when the game is built. |
| `--repair` | Rebuilds from the game files chosen before. |
| `--uninstall` | Removes the build. With `--yes` it does not ask, and it keeps saves and build tools. |
| `--desktop-shortcut`, `--start-menu-shortcut` | With `--yes`: also make that shortcut. Neither is made by default. |

A launcher that manages the game itself can build and start it with two lines:

```
SnapRecomp.exe --game D:\Dumps\Snap --build-dir D:\Games\Snap --yes
SnapRecomp.exe --build-dir D:\Games\Snap --play --fullscreen
```

Before a build exists, `--help` and `--version` print the setup's own text.

## Other commands

These do one job and exit. None opens the launcher.

| Command | What it does |
| --- | --- |
| `--import-save <folder or zip>` | Copies a save into the profile. Exit code 0 for success, 1 for a failure, 2 when the game is running. |
| `--export-save <folder or zip>` | Writes the profile's save out, with the same exit codes. |
| `--benchmark` | Plays the benchmark ride and writes a report. `--benchmark-capped` and `--benchmark-uncapped` pick the frame-rate mode, and `--benchmark-view <file>` shows an earlier report. |
| `--prepare-shaders` | Builds the shader cache ahead of play. Takes `--force`, `--visible`, `--list`, `--stop` and `--courses <list>`. |

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | The game or the command ended normally |
| 1 | A command failed |
| 2 | A bad command line, a data folder that could not be created, or converted game files that are missing or damaged |
| 3 | The exe's own folder cannot be written and no `--data-dir` was given, in a start where no window may open ([readonly-install.md](readonly-install.md)) |
