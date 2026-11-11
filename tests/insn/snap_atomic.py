"""Snap: differential test of the LL/SC exclusive sequences that `main` actually contains.

Scans .text for every load-exclusive (ldxr/ldaxr/ldxrb/h, ldxp/ldaxp) and the store-exclusive that follows it on the same
base register within 6 instructions, with only straight-line register ALU words between (no branch, memory, system or
FP/SIMD). Each distinct word tuple is lifted, run from random states against Unicorn (Cortex-MAX) and compared (X0-X30,
SP, V0-V31, memory). Also tests every distinct lone exclusive word (load alone; store alone, which must fail).
Reuses atomic.py's case generator and harness. Adapted from TotK Recomp tests/diff/atomic.py (MIT).

Usage: snap_atomic.py [--cases N] [--seed S] [--max-seqs M]
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import argparse
import os
import platform
import random
import struct
import sys
import time
import zlib
from collections import Counter

import numpy as np
import unicorn as uc
from unicorn import arm64_const as A

from common import BUF, BUF_SIZE, ROOT, Emu, build_harness, diff_state, dis, run_harness
import atomic
from atomic import CLREX, CODE, SLOT, make_case
from elfimg import ElfImage
from lifter import Lifter
from snap_common import ELF, lower_priority


def is_ldx(w):
    return (w & 0x3FC00000) == 0x08400000 and (w & 0x00208000) in (0, 0x8000) and not (w >> 21) & 1 and (w >> 22) & 1 \
        and (w & 0x3F600000) in (0x08400000,) or (w & 0xBFE08000) == 0x887F0000 and False


def kind_of(w):
    """'ldx' / 'stx' / None for exclusive pair and single-register forms (LDXR/LDAXR/LDXP/LDAXP/STXR/STLXR/STXP/STLXP)."""
    if (w & 0x3F000000) != 0x08000000:
        return None
    o2, L, o1 = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1
    if o2:
        return None  # LDAR/STLR/CAS family
    return 'ldx' if L else 'stx'


def rn_of(w):
    return (w >> 5) & 31


def plain_alu(w):
    """Straight-line integer register op that cannot fault or branch (data-processing immediate or register)."""
    top = (w >> 25) & 15
    return top in (0b1000, 0b1001, 0b0101, 0b1101) and (w & 0xFC000000) not in (0x14000000, 0x94000000)


def main():
    lower_priority()
    ap = argparse.ArgumentParser()
    ap.add_argument('--cases', type=int, default=24)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--max-seqs', type=int, default=6000)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--cflags', default='-march=x86-64-v3 -Wno-deprecated-declarations')
    args = ap.parse_args()
    r = random.Random(args.seed)
    t0 = time.time()
    img = ElfImage(ELF)
    lo, size = img.text
    W = np.frombuffer(bytes(img.image[lo:lo + size]), dtype='<u4').tolist()
    ldx = [i for i, w in enumerate(W) if kind_of(w) == 'ldx']
    stx_words = {w for w in W if kind_of(w) == 'stx'}
    ldx_words = {W[i] for i in ldx}
    seqs, shapes = set(), Counter()
    for i in ldx:
        for j in range(i + 1, min(i + 8, len(W))):
            w = W[j]
            if kind_of(w) == 'stx':
                if rn_of(w) == rn_of(W[i]) and all(plain_alu(W[k]) for k in range(i + 1, j)):
                    seqs.add(tuple(W[i:j + 1]))
                break
            if not plain_alu(w):
                break
    seqs = sorted(seqs)
    print(f'{len(ldx)} load-exclusives ({len(ldx_words)} distinct words), {len(stx_words)} distinct store-exclusive words, '
          f'{len(seqs)} distinct sequences')
    if len(seqs) > args.max_seqs:
        seqs = sorted(r.sample(seqs, args.max_seqs))
    # every distinct exclusive word alone, and every distinct (ldx, stx) mnemonic pair seen at least once is already in seqs
    tests = [([w], rn_of(w), 'excl') for w in sorted(ldx_words | stx_words)] + [(list(s), rn_of(s[0]), 'excl') for s in seqs]
    forms = Counter(dis(t[0][0]).split(' ')[0] + ('+' + dis(t[0][-1]).split(' ')[0] if len(t[0]) > 1 else '') for t in tests)
    print(f'{len(tests)} tests; forms:', dict(forms.most_common(30)))

    code = [CLREX, 0, 0, 0]
    bodies = []
    lifter = Lifter(0, 'c')
    for i, (words, _, _) in enumerate(tests):
        pc = CODE + SLOT + SLOT * 2 * i
        pad = [0] * (SLOT * 2 // 4 - len(words))
        code += words + pad
        bodies.append(' '.join(lifter.lift(w, pc + 4 * j).code for j, w in enumerate(words)))
    code_bytes = struct.pack(f'<{len(code)}I', *code)
    work = ROOT / 'build' / 'test' / 'snap_atomic'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'code.bin').write_bytes(code_bytes)
    exe = build_harness(work, bodies, args.jobs, args.cflags)
    emu = Emu([(CODE, code_bytes)], A.UC_CPU_ARM64_MAX)
    mu = emu.mu
    buf0 = bytes(r.getrandbits(8) for _ in range(4096)) * (BUF_SIZE // 4096)
    mu.mem_write(BUF, buf0)
    cases, expect, skipped = [], [], 0
    for k, (words, rn, kind) in enumerate(tests):
        pc = CODE + SLOT + SLOT * 2 * k
        for _ in range(args.cases):
            fpcr, x, sp, nzcv, v = make_case(r, words, rn, kind, buf0)
            mu.emu_start(CODE, CODE + 4, count=1)
            try:
                ox, osp, onz, opc, ov = emu.run(pc, fpcr, x, sp, nzcv, v, count=len(words))
            except uc.UcError as e:
                if skipped < 3: print('fault', e, [dis(w) for w in words], hex(pc))
                skipped += 1
                mu.mem_write(BUF, buf0)
                continue
            mh = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
            mu.mem_write(BUF, buf0)
            cases.append((k, fpcr, x, sp, nzcv, v, True))
            expect.append((ox, osp, onz, opc, ov, mh))
    print(f'unicorn: {len(cases)} cases ({skipped} faulted and skipped) ({time.time() - t0:.0f}s)')
    recs, err = run_harness(exe, work, buf0, cases, [(CODE, work / 'code.bin')])
    if err:
        print('harness failed:', err)
    fails, nfail = {}, 0
    for case, want, got in zip(cases, expect, recs):
        d = diff_state(got, want, check_pc=False, check_mem=True)
        if d:
            nfail += 1
            fails.setdefault(case[0], d[:3])
    print(f'{len(tests)} tests, {len(cases)} cases: {nfail} failing cases, {len(fails)} failing tests ({time.time() - t0:.0f}s)')
    for k, d in list(fails.items())[:40]:
        print('  ', ' ; '.join(dis(w) for w in tests[k][0]), '|', '; '.join(d))
    return 1 if fails or err else 0


if __name__ == '__main__':
    sys.exit(main())
