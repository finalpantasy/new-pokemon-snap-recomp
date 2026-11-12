# Third-party notices

This project adapts MIT-licensed code. Each adapted file keeps a header naming its source and copyright holder; this file
lists them with the exact commits and the license texts. No GPL or AGPL code is copied. Nothing here is distributed;
generated game C and build outputs stay under `generated/` and `build/` (both git-ignored).

## Adapted code

| Component | Source and commit | License | Files |
|---|---|---|---|
| AArch64-to-C lifter, FP/SIMD lowering, jump-table resolver, function discovery and C emitter, register/flag planning | TotK Recomp (https://github.com/valentderah/totk-pc-recomp) @ `7a37fc3c346cd6f2b3c80637effb27b880ce817b`, Copyright (c) 2026 Valiantsin Dzerakh; itself adapted from LAS Recomp (https://github.com/peiikai/LASRecomp) @ `da35b83a9611d046a9a681aae668d7085954d37f`, Copyright (c) 2026 Peiikai | MIT (both) | `recomp/lifter.py`, `recomp/simd_c.py`, `recomp/jumptables.py`, `recomp/flags.py`, `recomp/regs.py`, `recomp/module.py`, `recomp/patches.py`, `recomp/gen.py` (trimmed to discovery, Emitter, chunk_text), `recomp/elfimg.py` (rewrite of `nso.py` for ELF input) |
| Guest CPU header, SIMD/FP helpers, software fma | same as above (`runtime/cpu.h`, `simd.h`, `simd_fp.h`, `fma.c`) | MIT | `runtime/cpu.h`, `runtime/simd.h`, `runtime/simd_fp.h`, `runtime/fma.c` |
| Differential test harness and operand generators | same as above (`tests/diff/common.py`, `tests/diff/harness.c`, `tests/diff/func.py`) | MIT | `tests/diff_common.py`, `tests/snap_harness.c`, `tests/snap_func.py` |
| Software fma and AES S-box constants | musl libc `src/math/fma.c`, `fmaf.c`, Copyright (c) 2005-2020 Rich Felker and contributors; the AES S-box in `runtime/simd.h` is the public FIPS-197 table | MIT / public | `runtime/fma.c`, `runtime/simd.h` |
| ASTC LDR decoder | LAS Recomp `runtime/nvn/astc.c` (see above), written from the Khronos Data Format spec | MIT | `runtime/nvn/nvn_astc.cpp` |
| NVN ABI values (cross-checked only) | TotK Recomp `runtime/nvn.h` and LAS Recomp `runtime/nvn/formats.c`; values are compared against, no header is vendored | MIT | `runtime/nvn/nvn_types.h` |
| Offline Maxwell-to-SPIR-V harness | Ryujinx / Ryubing shader translator `Ryujinx.Graphics.Shader` and `Ryujinx.ShaderTools`, Copyright (c) Ryujinx Team and Contributors; `Directory.Packages.props` is a copy of the Ryujinx central package file. The translator is referenced from a local checkout under `tools/ref/`, not vendored | MIT | `scripts/shader_harness/` |
| SSBH SHDR container layout | ssbh_lib, Copyright (c) 2020 Smash Ultimate Research Group; format facts only, no code copied | MIT | `scripts/nushdb_parse.py` |

Files written for this project (no third-party code): `recomp/snap_common.py`, `recomp/snap_diff.py`,
`recomp/snap_coverage.py`, `runtime/snap_rt_abort.c`.

Reference only, not copied: Kirikae (https://github.com/Cor3Duo/kirikae) @ `304a1093114f9ddbe268897e2ddb5576676d9b11`
(MIT/Apache-2.0), kept in `tools/ref/kirikae` for the bake-off.

## Tools used (not redistributed)

- LLVM/clang/lld 23.1.5, official Windows release, Apache-2.0 with LLVM exceptions.
- Unicorn 2.1.4 (GPL-2.0), test-only reference emulator in a private venv; never linked into or shipped with the generator or runtime.
- Capstone (BSD-3-Clause), pyelftools (public domain), NumPy (BSD-3-Clause).

## License texts

### TotK Recomp (MIT)

MIT License

Copyright (c) 2026 Valiantsin Dzerakh

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### LAS Recomp (MIT)

MIT License

Copyright (c) 2026 Peiikai

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

This license covers LAS Recomp's own code. It does not cover The Legend of Zelda: Link's Awakening or any other
Nintendo property, nor the third-party components in third_party/, which keep their own licenses.

### Ryujinx / Ryubing (MIT)

MIT License

Copyright (c) Ryujinx Team and Contributors

### ssbh_lib (MIT)

MIT License

Copyright (c) 2020 Smash Ultimate Research Group

### musl libc (MIT)

Copyright (c) 2005-2020 Rich Felker, et al.

The full MIT permission and warranty text, as reproduced above for TotK Recomp, applies to the three entries in this
section.

## Dear ImGui (in-game controls overlay)

Dear ImGui v1.93.0 (https://github.com/ocornut/imgui), Copyright (c) 2014-2026 Omar Cornut, MIT. Vendored without changes except `imconfig.h`
(gamepad polling of the Win32 backend and the demo/debug windows are disabled; `imgui_demo.cpp` is not included) in
`runtime/ui/third_party/imgui/` (core, `imgui_impl_vulkan`, `imgui_impl_win32`). The license text is `runtime/ui/third_party/imgui/LICENSE.txt`:

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
