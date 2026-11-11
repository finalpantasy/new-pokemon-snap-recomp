"""Builds and runs the host-boundary spike (project tooling, ours).

Usage: tools/venv-recomp/Scripts/python -I host/build_spike.py [--run] [--no-build]
Compiles host/spike.cpp (C++17) and runtime/fma.c, links them against build/v262144/lib/snap_main.lib into
build/v262144/spike/snap_spike.exe, and with --run runs it on analysis/import/v262144-main.elf (output build/v262144/spike/spike_out.tsv).
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from snap_common import ELF, lower_priority  # noqa: E402

LLVM = ROOT / 'tools' / 'llvm' / 'bin'
OUT = ROOT / 'build' / 'v262144' / 'spike'
BELOW_NORMAL = 0x4000


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], creationflags=BELOW_NORMAL, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--lib', default=str(ROOT / 'build' / 'v262144' / 'lib' / 'snap_main.lib'))
    args = ap.parse_args()
    lower_priority()
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / 'snap_spike.exe'
    if not args.no_build:
        t0 = time.time()
        common = ['-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-I', ROOT / 'runtime']
        r = run([LLVM / 'clang++.exe', '-std=c++17'] + common + ['-c', ROOT / 'host' / 'spike.cpp', '-o', OUT / 'spike.obj'])
        r2 = run([LLVM / 'clang.exe'] + common + ['-c', ROOT / 'runtime' / 'fma.c', '-o', OUT / 'fma.obj'])
        if r.returncode or r2.returncode:
            return 1
        r = run([LLVM / 'clang++.exe', '-fuse-ld=lld', '-o', exe, OUT / 'spike.obj', OUT / 'fma.obj', args.lib])
        if r.returncode:
            return 1
        print(f'built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.1f}s')
    if args.run:
        t0 = time.time()
        p = run([exe, ELF, OUT / 'spike_out.tsv'], capture_output=True, text=True)
        print(p.stdout[-2000:], p.stderr[-2000:], f'exit {p.returncode:#x} in {time.time() - t0:.2f}s')
        return p.returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())
