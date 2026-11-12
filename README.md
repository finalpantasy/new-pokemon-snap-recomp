# New Pokémon Snap PC Recomp

A native Windows static recompilation of New Pokémon Snap (Switch): the game's AArch64 code is lifted to C, built with clang, and run against a hand-written Horizon OS and NVN-to-Vulkan runtime.

**This repository contains source code only. It includes no game files, keys, extracted assets or generated game code. You must supply your own legally dumped copy of v2.0.1 (version 262144, title update 2.0.1) of the game.**

## Status

Early, but playable. The game boots through the intro and tutorials into the first course, and Pokémon and characters render. Photo select works: photos sort by subject, thumbnails show, and the professor scores them. Keyboard, mouse and controllers work, and F1 opens a menu for remapping them and for the options below.

Known problems:

- Stable 60 FPS is not reached yet, and shader compiles can stutter.
- Draw distance and full-rate distant animation are experimental. They may change photo scores, so they are still being tested.
- Sub-native render scales (below 1.0) lost the GPU device after the photo score screen in testing, so no preset uses them.

## Features

All of these are in the F1 menu (Graphics, Display, Photos and Mods tabs); each row says whether it applies now or after a restart.

- **Quality presets:** Very Low, Low, Normal, High, Ultra, Maximum and Custom. They set render scale, anti-aliasing, sharpening, anisotropic filtering, texture sharpness, shadow resolution and ambient occlusion together. Normal matches the defaults.
- **Anti-aliasing:** 2x supersampling plus SMAA with CAS sharpening by default. The original FXAA path and other combinations are options.
- **Upscalers:** NVIDIA DLSS and DLAA, AMD FSR 3.1 and Intel XeSS, each with quality modes. The vendor SDK files are not in this repo; you download them yourself and place them under `tools/` (git-ignored). Photos and scoring never use jittered or upscaled output.
- **HDR output:** HDR10 and scRGB swapchains, with a fallback to standard output.
- **Sharper UI fonts:** automatic 2x font rendering at 1440p and above, with manual 1x to 4x choices.
- **Draw distance and distant animation:** options for longer draw distance and full-rate animation of distant Pokémon (experimental, see above).
- **Photo export:** every album photo is also saved as a lossless PNG with a JSON sidecar.
- **Input:** rumble, and gyro aim on controllers that report a gyro (it drives the right stick, since the game never reads motion).
- **Window and boot:** borderless fullscreen, monitor choice, a remembered window rectangle, pause and mute when unfocused, and an optional fast-boot mode that skips the frame limiter during loading.
- **Mods:** a loader for loose-file packs that replace single game archive entries, including HD texture packs. Packs live in `%APPDATA%\SnapRecomp\mods`, or the folder named by `SNAP_MODS_DIR`, and are enabled and ordered from the F1 Mods tab. See `docs/mods.md`. The repo holds only the loader, never pack contents made from game data.

![Professor Mirror and Rita at the lab, rendered by the PC port](docs/screenshots/lab.webp)

![Photographing Vivillon in the first course. The black box at bottom left is the old broken photo thumbnail, fixed since.](docs/screenshots/course.webp)

Both are older captures of the native build running on Windows, scaled to 1280x720.

## Architecture

- `recomp/`: Python lifter and C emitter. It reads the game's `main` and `sdk` modules as ELF files and writes C, one function per guest function. Generated C is never committed.
- `runtime/`: the guest CPU model and SIMD helpers, a Horizon OS layer (`os/`, threads and sync), HLE services (`hle/`), audio (`audio/`, WASAPI), and the NVN-to-Vulkan graphics layer (`nvn/`).
- `host/`: loader, boot driver and the one-command build of `snap_boot.exe`.
- `scripts/`: extraction, unpacking and analysis tools (NSO to ELF, PFS0, DRP archives, shader and format parsers, Ghidra scripts, an offline Maxwell-to-SPIR-V harness).
- `tests/`: instruction-level differential tests, HLE, audio, NVN and OS tests. Test runs use up to 4 parallel slots (see `host/README.md`).
- `docs/` and `AGENTS.md`: design notes and the project's decisions and state.

Game shaders are translated offline (Ryujinx shader translator to SPIR-V) and loaded at run time from the user's own extracted files. Nothing derived from the game is stored in the repo.

## Prerequisites

Windows 10 or 11 x64. Local tools go under `tools/`, which is git-ignored, and the scripts look for them at these paths.

- LLVM/clang/lld 23 (`tools/llvm`) and Visual Studio 2022 Build Tools (Windows SDK, `vcvars64.bat`).
- Vulkan SDK (`tools/vulkan_sdk`), including `vulkan-1.lib` and `glslangValidator`. SDL3 under `tools/dl_sdl3` is optional (gamepads; XInput is the fallback).
- Python 3.12 or newer. `tools/venv-recomp` needs `capstone`, `pyelftools`, `numpy`, `lz4` and `unicorn`. `tools/venv-oodle` is the DRP unpacking environment.
- Rust, only to build the small XOR helper in `scripts/drp_xor/`.
- NSTool for NCA/RomFS extraction (`tools/nstool`). Ghidra 12.1.2 with the Switch loader extension and JDK 21 for function discovery (`tools/ghidra_*`, `tools/jdk-*`).
- .NET SDK, for `scripts/shader_harness`. It references a local checkout of the Ryubing Ryujinx shader translator at `tools/ref/ryubing-ryujinx`.

Set `SNAP_DUMPS_DIR` to the folder holding your XCI/NSP files and `prod.keys` for `scripts/audit_revisions.py`.

## Pipeline

Run from the repo root.

1. Extract: use NSTool on the base XCI plus the 2.0.1 update NSP to get the program NCA and the patched RomFS (into `extracted/`). `scripts/pfs0.py` lists and extracts PFS0/NSP members.
2. Convert modules: `python -I scripts/nso2elf.py <main.nso> <main.elf>` (same for `sdk`).
3. Unpack archives: `tools\venv-oodle\Scripts\python -I scripts\drp_unpack.py --set priority`.
4. Discover functions: `scripts/ghidra_import.ps1` and `scripts/ghidra_analyze.ps1`, then the export scripts in `scripts/ghidra/`.
5. Lift: `tools/venv-recomp/Scripts/python -I recomp/snap_gen.py --module main`, same for `sdk`, then `recomp/snap_build.py` to compile the C into libraries.
6. Build: `tools/venv-recomp/Scripts/python -I host/build_snap.py` produces `build/snap/snap_boot.exe`.
7. Run: `powershell -File host\run_boot.ps1 -Timeout 100 run` (timed run with a trace log), or `powershell -File host\play.ps1` for an untimed session.

`AGENTS.md` and `host/README.md` hold the detailed flags and the current state of each step.

## License and credits

MIT, see `LICENSE`. The lifter and CPU runtime adapt TotK Recomp and LAS Recomp (both MIT); the shader harness builds on the Ryujinx shader translator; the SSBH layout follows ssbh_lib. Full notices are in `THIRD_PARTY_NOTICES.md`.

New Pokémon Snap is a trademark of Nintendo and The Pokémon Company. This project is unaffiliated with them.
