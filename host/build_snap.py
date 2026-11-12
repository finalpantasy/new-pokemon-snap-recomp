"""One-command build of the full boot executable build/snap/snap_boot.exe (project tooling, ours).

  tools/venv-recomp/Scripts/python -I host/build_snap.py [--jobs 8] [--rebuild] [--out build/snap]

Compiles (BelowNormal, incremental by mtime, at most --jobs compiles at once): host/{loader,hooks,boot}.cpp, the nn::os layer
(runtime/os), HLE (runtime/hle), audio (runtime/audio), the NVN-to-Vulkan layer (runtime/nvn), the generated NVN ABI bridge
(generated/boot/nvn_bridge.cpp), the generated "not implemented" stubs (generated/unimplemented.cpp) and runtime/fma.c; then
links them with the lifted main and sdk libraries (build/eh/*/lib, from recomp/snap_build.py) using lld.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LLVM = ROOT / 'tools' / 'llvm' / 'bin'
VK = ROOT / 'tools' / 'vulkan_sdk'
BELOW_NORMAL = 0x4000
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'


def vc_env():
    env = dict(os.environ)
    out = subprocess.run(f'cmd /c ""{VCVARS}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--rebuild', action='store_true')
    ap.add_argument('--out', default=str(ROOT / 'build' / 'snap'))
    ap.add_argument('--main-lib', default=str(ROOT / 'build' / 'eh' / 'main' / 'lib' / 'snap_main.lib'))
    ap.add_argument('--sdk-lib', default=str(ROOT / 'build' / 'eh' / 'sdk' / 'lib' / 'snap_sdk.lib'))
    ap.add_argument('--name', default='snap_boot')
    args = ap.parse_args()
    jobs = min(args.jobs, 8)
    out = Path(args.out)
    obj = out / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    py = sys.executable
    # generated inputs
    subprocess.check_call([py, '-I', str(ROOT / 'host' / 'gen_nvn_bridge.py')])
    cov = ROOT / 'analysis' / 'runtime' / 'hle_coverage.tsv'
    bak = cov.read_bytes() if cov.exists() else None
    subprocess.check_call([py, '-I', str(ROOT / 'runtime' / 'hle' / 'gen_hle_stubs.py'), '--all'], stdout=subprocess.DEVNULL)
    if bak is not None:
        cov.write_bytes(bak)   # the coverage table is the HLE owner's; only the stub file is ours
    shutil.copyfile(ROOT / 'generated' / 'hle_unimplemented.cpp', ROOT / 'generated' / 'unimplemented.cpp')

    srcs = [(ROOT / 'host' / f, 17) for f in ('loader.cpp', 'hooks.cpp', 'boot.cpp', 'abi_audio.cpp', 'window_opts.cpp')]
    srcs += [(p, 20) for p in sorted((ROOT / 'runtime' / 'os').glob('*.cpp'))]
    srcs += [(p, 20) for p in sorted((ROOT / 'host' / 'mods').glob('*.cpp'))]   # draw-distance mods 26 and 27 (host/mods/lodanim.cpp)
    for d in ('hle', 'audio', 'nvn', 'mods'):
        srcs += [(p, 20) for p in sorted((ROOT / 'runtime' / d).glob('*.cpp'))]
    # in-game controls overlay (runtime/ui): our files plus the vendored Dear ImGui core and its Vulkan / Win32 backends
    srcs += [(p, 20) for p in sorted((ROOT / 'runtime' / 'ui').glob('*.cpp'))]
    imgui = ROOT / 'runtime' / 'ui' / 'third_party' / 'imgui'
    srcs += [(imgui / f, 17) for f in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'imgui_impl_vulkan.cpp', 'imgui_impl_win32.cpp')]
    # audio: one backend file is the null sink fallback, the others real; all are part of the runtime target
    srcs += [(ROOT / 'generated' / 'boot' / 'nvn_bridge.cpp', 20), (ROOT / 'generated' / 'unimplemented.cpp', 20)]
    cflags = ['-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-g',
              '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'host'), '-I', str(ROOT / 'runtime' / 'nvn'), '-I', str(VK / 'Include'),
              '-I', str(ROOT / 'runtime' / 'ui'), '-I', str(ROOT / 'runtime' / 'ui' / 'third_party' / 'imgui'),
              '-DNVN_HARNESS_DLL="' + (ROOT / 'tools' / 'shader_harness' / 'bin' / 'Release' / 'net10.0' / 'ShaderHarness.dll').as_posix() + '"']
    # Gamepads through SDL3 when the SDK is unpacked under tools/dl_sdl3 (the DLL is copied next to the exe); XInput otherwise.
    sdl = ROOT / 'tools' / 'dl_sdl3' / 'x' / 'SDL3-3.4.20'
    sdl_ok = (sdl / 'include' / 'SDL3' / 'SDL.h').exists() and (sdl / 'lib' / 'x64' / 'SDL3.lib').exists()
    if sdl_ok:
        cflags += ['-DHLE_WITH_SDL3', '-I', str(sdl / 'include')]
    env = vc_env()

    def compile_one(item):
        src, std = item
        o = obj / (src.parent.name + '_' + src.stem + '.obj')
        deps = [src] + list((ROOT / 'host').glob('*.h')) + list((ROOT / 'runtime').glob('*.h')) + list((ROOT / 'runtime' / src.parent.name).glob('*.h'))
        if not args.rebuild and o.exists() and o.stat().st_mtime > max(d.stat().st_mtime for d in deps):
            return o, 0, ''
        r = subprocess.run([str(LLVM / 'clang++.exe'), f'-std=c++{std}'] + cflags + ['-c', str(src), '-o', str(o)], env=env,
                           creationflags=BELOW_NORMAL, capture_output=True, text=True, timeout=1800)
        return o, r.returncode, r.stderr
    objs, fail = [], 0
    with ThreadPoolExecutor(jobs) as ex:
        for o, rc, err in ex.map(compile_one, srcs):
            objs.append(o)
            if rc:
                fail += 1
                print('FAILED', o.name, '\n', err[-3000:], file=sys.stderr)
    r = subprocess.run([str(LLVM / 'clang.exe')] + [c for c in cflags if not c.startswith('-DNVN')] + ['-c', str(ROOT / 'runtime' / 'fma.c'), '-o', str(obj / 'fma.obj')], env=env, creationflags=BELOW_NORMAL)
    if r.returncode:
        fail += 1
    objs.append(obj / 'fma.obj')
    if fail:
        print(f'{fail} compile failures')
        return 1
    exe = out / f'{args.name}.exe'
    # exe icon (resource ID 1): game icon from local extracted data, else the original fallback; generated, never committed
    res = []
    if subprocess.run([sys.executable, '-I', str(ROOT / 'host' / 'make_icon.py'), '--out', str(ROOT / 'build')]).returncode == 0:
        if subprocess.run([str(LLVM / 'llvm-rc.exe'), '/FO', str(ROOT / 'build' / 'snap_icon.res'), str(ROOT / 'build' / 'snap_icon.rc')], cwd=str(ROOT / 'build')).returncode == 0:
            res = [str(ROOT / 'build' / 'snap_icon.res')]
    libs = [args.main_lib, args.sdk_lib, str(VK / 'Lib' / 'vulkan-1.lib'), '-luser32', '-lgdi32', '-lbcrypt', '-lole32', '-lavrt', '-lshell32', '-ladvapi32', '-lwinmm', '-loleaut32', '-ldbghelp', '-ldwmapi']
    if sdl_ok:
        libs.append(str(sdl / 'lib' / 'x64' / 'SDL3.lib'))
        shutil.copyfile(sdl / 'lib' / 'x64' / 'SDL3.dll', out / 'SDL3.dll')
    r = subprocess.run([str(LLVM / 'clang++.exe'), '-fuse-ld=lld', '-g', '-Wl,/map:' + str(out / (args.name + '.map')), '-Wl,/debug', '-o', str(exe)] + [str(o) for o in objs] + res + libs,
                       env=env, creationflags=BELOW_NORMAL)
    if r.returncode:
        return 1
    print(f'built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.1f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
