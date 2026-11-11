"""Builds the two-module boot/test driver (project tooling, ours).

Usage: tools/venv-recomp/Scripts/python -I host/build_boot.py [--main-lib L] [--sdk-lib L] [--out DIR] [--name snap_boot]
Compiles host/loader.cpp, hooks.cpp, boot.cpp and runtime/fma.c (BelowNormal), links them against the lifted main and sdk
libraries (recomp/snap_build.py output) with lld into <out>/<name>.exe.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from snap_common import lower_priority  # noqa: E402

LLVM = ROOT / 'tools' / 'llvm' / 'bin'
BELOW_NORMAL = 0x4000


def run(cmd):
    return subprocess.run([str(c) for c in cmd], creationflags=BELOW_NORMAL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main-lib', default=str(ROOT / 'build' / 'eh' / 'main' / 'lib' / 'snap_main.lib'))
    ap.add_argument('--sdk-lib', default=str(ROOT / 'build' / 'eh' / 'sdk' / 'lib' / 'snap_sdk.lib'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'boot'))
    ap.add_argument('--name', default='snap_boot')
    args = ap.parse_args()
    lower_priority()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    common = ['-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-DNOMINMAX', '-I', ROOT / 'runtime', '-I', ROOT / 'host']
    objs = []
    for src, cc, extra in (('host/loader.cpp', 'clang++.exe', ['-std=c++17']), ('host/hooks.cpp', 'clang++.exe', ['-std=c++17']),
                           ('host/boot.cpp', 'clang++.exe', ['-std=c++17']), ('runtime/fma.c', 'clang.exe', [])):
        o = out / (Path(src).stem + '.obj')
        r = run([LLVM / cc] + extra + common + ['-c', ROOT / src, '-o', o])
        if r.returncode:
            return 1
        objs.append(o)
    exe = out / f'{args.name}.exe'
    r = run([LLVM / 'clang++.exe', '-fuse-ld=lld', '-Wl,/map:' + str(out / (args.name + '.map')), '-o', exe] + objs + [args.main_lib, args.sdk_lib])
    if r.returncode:
        return 1
    print(f'built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.1f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
