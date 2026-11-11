# New Pokémon Snap PC Recomp

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20this%20project-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/finalpantasy)

A native Windows static recompilation of New Pokémon Snap (Switch): the game's AArch64 code is lifted to C, built with clang, and run against a hand-written Horizon OS and NVN-to-Vulkan runtime.

**This repository contains source code only. It includes no game files, keys, extracted assets or generated game code. You must supply your own legally dumped copy of v2.0.1 (version 262144, title update 2.0.1) of the game.**

## Status

Boots to the lab intro cutscene. Characters are not rendered yet. Not playable.

<!-- screenshots: pending owner decision -->

## Architecture

- `recomp/`: Python lifter and C emitter. It reads the game's `main` and `sdk` modules as ELF files and writes C, one function per guest function. Generated C is never committed.
- `runtime/`: the guest CPU model and SIMD helpers, a Horizon OS layer (`os/`, threads and sync), HLE services (`hle/`), audio (`audio/`, WASAPI), and the NVN-to-Vulkan graphics layer (`nvn/`).
- `host/`: loader, boot driver and the one-command build of `snap_boot.exe`.
- `scripts/`: extraction, unpacking and analysis tools (NSO to ELF, PFS0, DRP archives, shader and format parsers, Ghidra scripts, an offline Maxwell-to-SPIR-V harness).
- `tests/`: instruction-level differential tests, HLE, audio, NVN and OS tests.
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
