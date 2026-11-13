"""Form-driven differential fuzzer: the game's own instruction words, lifted C (production flags) vs Unicorn (A57).

Forms and their risk weight come from analysis/recomp/insn_census.tsv (tests/insn/census.py). For every form up to
--words distinct words of main and sdk are taken; each word gets cases in proportion to its risk weight. Operands are
typed from the form: FP lanes get NaN payloads (quiet and signalling), denormals, +-0, infinities, rounding ties and
overflow edges for the element size; integer lanes and X/W registers get INT_MIN/INT_MAX/-1/0, shift amounts around
the width, and random values. FPCR is 0 (the game's value: main never writes FPCR) in 70% of cases, otherwise random
DN/FZ/RMode/AHP.

Two builds are compared against the same Unicorn results:
  run    inputs come from the case record (the harness), compiled with the production flags (-O2 -march=x86-64-v3)
  const  every input register is a C constant inside the test body, so clang can constant-fold the lifted code;
         this finds undefined behaviour in the lowering (float->int casts, shifts >= width, signed overflow) that a
         runtime-input test never sees. FP/SIMD and integer data processing words only.

Needs no game files beyond the two ELFs the lifter reads. Output: failing words with disassembly, inputs, diffs;
--regress FILE writes them (word, module offset, inputs) for tests/insn/regress.py.

Usage: tools/venv-recomp/Scripts/python -I tests/insn/fuzz.py [--words N] [--scale S] [--seed S] [--modes run,const]
       [--classes fpcvt,fparith,...] [--forms REGEX] [--jobs J] [--opt -O2]
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import argparse
import os
import random
import re
import struct
import sys
import time
import zlib
from collections import Counter, defaultdict

import capstone
import numpy as np
import unicorn as uc

from common import BUF, BUF_SIZE, F16, F32, F64, ROOT, Emu, build_harness, diff_state, dis, rand_lane, rand_v, \
    rand_x, run_harness
sys.path.insert(0, str(ROOT / 'recomp'))
from census import risk, shape  # noqa: E402
from lift import classify, to_test_body  # noqa: E402
from lifter import Lifter, Unsupported, bits  # noqa: E402
from elfimg import ElfImage  # noqa: E402
from snap_common import BASE, ELF, SDK_BASE, SDK_ELF, functions, tsv  # noqa: E402

BUF_MID = BUF + 0x10000
WORK = ROOT / 'build' / 'w' / 'fuzz'
ALIAS_CODE = 0x7220000000
CENSUS = ROOT / 'analysis' / 'recomp' / 'insn_census.tsv'
CONST_CLASSES = {'fpcvt', 'fpest', 'fparith', 'fpcmp', 'fpmove', 'neon_sat', 'neon_narrow', 'neon_widen', 'neon_shift',
                 'neon_lane', 'neon_int', 'simd_other', 'flags', 'intdiv', 'bitfield', 'csel', 'int', 'crc'}

# Extra FP edges on top of common.F32/F64: rounding ties for every RMode, int-conversion boundaries, near-denormals.
F32X = [0x3f000000, 0x3fc00000, 0x40200000, 0x40600000, 0xbf000000, 0xbfc00000, 0x4b000000, 0x4b000001, 0x4affffff,
        0x4b7fffff, 0xcb000001, 0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001, 0x4f7fffff, 0x4f800000, 0x5effffff,
        0x5f000000, 0xdf000000, 0x5f800000, 0x00000002, 0x80000002, 0x007ffffe, 0x7f7fffff, 0x7fffffff, 0xffffffff,
        0x7f800001, 0xff800001, 0x7fbfffff, 0x3f7fffff, 0x3f800001, 0x34000000, 0x33ffffff, 0xbf7fffff]
F64X = [0x3fe0000000000000, 0x3ff8000000000000, 0x4004000000000000, 0xbfe0000000000000, 0x4330000000000000,
        0x4330000000000001, 0x432fffffffffffff, 0x41dfffffffc00000, 0x41dfffffffe00000, 0x41e0000000000000,
        0xc1e0000000000000, 0xc1e0000000200000, 0x41efffffffe00000, 0x41f0000000000000, 0x43dfffffffffffff,
        0x43e0000000000000, 0xc3e0000000000000, 0x43f0000000000000, 0x7fffffffffffffff, 0x7ff0000000000001,
        0x7ff7ffffffffffff, 0x0000000000000002, 0x47efffffe0000000, 0x47efffffefffffff, 0x47effffff0000000,
        0x36a0000000000000, 0x36a0000000000001, 0x369fffffffffffff, 0x3810000000000000, 0x380fffffffffffff]
XEDGE = [0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128, 255, 256, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000,
         0x80000001, 0xFFFFFFFE, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0x8000000000000001,
         0xFFFFFFFF80000000, 0x00000000FFFF8000, 0x7FFF, 0x8000, 0xFFFF, 0x7F, 0x80, 0xFF, 0x100000000,
         0xFFFFFFFF00000000, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA]


def rf32(r):
    k = r.random()
    if k < 0.45:
        return r.choice(F32 + F32X)
    if k < 0.6:  # denormal or near-denormal
        return (r.getrandbits(1) << 31) | (r.choice([0, 0, 1, 2]) << 23) | r.getrandbits(23)
    if k < 0.7:  # NaN with random payload
        return (r.getrandbits(1) << 31) | (0xff << 23) | (r.getrandbits(22) | (r.getrandbits(1) << 22) or 1)
    if k < 0.85:
        e = r.choice([r.randint(118, 160), r.randint(0, 255), 127, 126, 150, 151, 157, 158, 189, 190])
        return (r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(23) if r.random() < 0.6 else r.getrandbits(3) << 20)
    return struct.unpack('<I', struct.pack('<f', r.choice([r.uniform(-4, 4), r.randint(-300, 300) + 0.5,
                                                           r.uniform(-1e6, 1e6), r.uniform(-1e-3, 1e-3)])))[0]


def rf64(r):
    k = r.random()
    if k < 0.45:
        return r.choice(F64 + F64X)
    if k < 0.6:
        return (r.getrandbits(1) << 63) | (r.choice([0, 0, 1, 2]) << 52) | r.getrandbits(52)
    if k < 0.7:
        return (r.getrandbits(1) << 63) | (0x7ff << 52) | (r.getrandbits(51) | (r.getrandbits(1) << 51) or 1)
    if k < 0.85:
        e = r.choice([r.randint(1010, 1090), r.randint(0, 2047), 1023, 1022, 1075, 1076, 1054, 1055, 1086, 1087,
                      r.randint(870, 910), r.randint(1150, 1160)])  # 870..910 and 1150..: float denormal/overflow
        return (r.getrandbits(1) << 63) | (e << 52) | (r.getrandbits(52) if r.random() < 0.6 else r.getrandbits(4) << 48)
    return struct.unpack('<Q', struct.pack('<d', r.choice([r.uniform(-4, 4), r.randint(-300, 300) + 0.5,
                                                           r.uniform(-1e12, 1e12), r.uniform(-1e-3, 1e-3)])))[0]


def rf16(r):
    if r.random() < 0.5:
        return r.choice(F16)
    return r.getrandbits(16)


def rx(r, wide):
    k = r.random()
    if k < 0.35:
        v = r.choice(XEDGE)
        return v if wide or r.random() < 0.3 else (v & 0xFFFFFFFF) | (r.getrandbits(32) << 32 if r.random() < 0.5 else 0)
    if k < 0.5:
        return rf32(r) | (r.getrandbits(32) << 32 if r.random() < 0.3 else 0)
    if k < 0.6:
        return rf64(r)
    return rand_x(r)


# element types present in a form: 'f16', 'f32', 'f64', or ('i', size)
_VEC = re.compile(r'v\.(\d*)([bhsdq])')
_SCAL = re.compile(r'(?:^|[ ,{\[])([bhsdq])(?=$|[ ,}\]])')
_ESZ = {'b': 0, 'h': 1, 's': 2, 'd': 3, 'q': 4}


def elem_types(form):
    mn, ops = (form.split(' ', 1) + [''])[:2]
    fp = mn[0] == 'f' or mn in ('scvtf', 'ucvtf') or mn.startswith(('frecp', 'frsqrt'))
    out = set()
    for m in _VEC.finditer(ops):
        out.add(m.group(2))
    for m in _SCAL.finditer(ops):
        out.add(m.group(1))
    types = set()
    for t in out:
        if fp and t in 'hsd':
            types.add({'h': 'f16', 's': 'f32', 'd': 'f64'}[t])
        if not fp or mn in ('fmov', 'fcvtzs', 'fcvtzu', 'scvtf', 'ucvtf') or mn.startswith(('fcvt', 'fcm')):
            types.add(('i', min(_ESZ[t], 3)))
    return types


def typed_v(r, types):
    if not types or r.random() < 0.15:
        return rand_v(r)
    t = r.choice(sorted(types, key=str))
    out = 0
    if t == 'f32':
        for i in range(4):
            out |= rf32(r) << (32 * i)
    elif t == 'f64':
        for i in range(2):
            out |= rf64(r) << (64 * i)
    elif t == 'f16':
        for i in range(8):
            out |= rf16(r) << (16 * i)
    else:
        sz = t[1]
        for i in range(16 >> sz):
            out |= rand_lane(r, sz) << ((8 << sz) * i)
    if r.random() < 0.1:
        out &= (1 << 64) - 1
    return out


def fpcr_of(r):
    if r.random() < 0.7:
        return 0
    v = r.getrandbits(1) << 25 | r.getrandbits(1) << 24 | r.randrange(4) << 22
    if r.random() < 0.1:
        v |= 1 << 26
    return v


def is_mem(w):
    return bits(w, 25, 1) == 0 and bits(w, 27, 1) == 1


def is_store(w):
    if not is_mem(w) or (w & 0x3B000000) == 0x18000000:
        return False
    return not (w >> 22) & 1


def make_case(r, w, form):
    types = elem_types(form)
    wide = ' x' in form or form.split(' ')[1:2] == ['x,']
    if is_mem(w):
        x = [BUF_MID + r.randrange(0, 0x100, 8) for _ in range(31)]
        if (bits(w, 21, 1) and bits(w, 10, 2) == 2) or (w & 0xBFA00000) == 0x0C800000:
            rm = bits(w, 16, 5)
            if rm != bits(w, 5, 5) and rm < 31:
                x[rm] = r.randrange(0, 0x100)
    else:
        x = [rx(r, wide) for _ in range(31)]
    sp = BUF_MID + r.randrange(0, 0x100, 16)
    nzcv = r.getrandbits(4) << 28
    v = [typed_v(r, types) for _ in range(32)]
    return fpcr_of(r), x, sp, nzcv, v


GRID_CLASSES = {'fpcvt', 'fpest', 'fparith', 'fpcmp', 'fpmove', 'neon_sat', 'neon_narrow', 'neon_widen', 'neon_shift',
                'neon_int', 'simd_other', 'intdiv', 'bitfield', 'flags', 'csel'}
GRID_FPCR = [0, 0, 0, 0x400000, 0x800000, 0xc00000, 0x2000000, 0x1000000, 0x3c00000]
XCONV = [(1 << 24) + 1, (1 << 24) + 3, (1 << 25) + 3, (1 << 53) + 1, (1 << 53) + 3, (1 << 54) + 6, 0xFFFFFFFFFFFFFC00,
         0xFFFFFFFFFFFFFBFF, 0x7FFFFFFFFFFFFE00, 0x7FFFFFFFFFFFFDFF, 0xFFFFFF80, 0xFFFFFF7F, 0x7FFFFFC0, 0x7FFFFFBF,
         0x80000040, 0xFFFFFFFFFFFFFFFE, 0x20000001, 0x1000001, 0xFFFFFF, 0x1FFFFFF]


def int_edges(sz):
    b = 8 << sz
    m = (1 << b) - 1
    return sorted({0, 1, 2, 3, m, m - 1, 1 << (b - 1), (1 << (b - 1)) - 1, (1 << (b - 1)) + 1, b - 1, b, b + 1, 2 * b,
                   (m - b + 1) & m, (-(b + 1)) & m, 0x55 * (m // 0xFF), 0xAA * (m // 0xFF), 1 << (b // 2),
                   (1 << (b // 2)) - 1, 0x80 & m, 0x7F, 0x81 & m, 0xFE & m} | ({0xFFFF, 0x8000, 0x7FFF} if b > 16 else set()))


def grid_cases(r, w, form):
    """Deterministic cross product of edge values over the source operands (Rn x Rm, lanes packed), then FPCR."""
    types = elem_types(form)
    fpt = [t for t in types if isinstance(t, str)]
    t = fpt[0] if fpt else (sorted(types, key=str)[0] if types else None)
    if t == 'f32':
        L, eb = sorted(set(F32 + F32X)), 32
    elif t == 'f64':
        L, eb = sorted(set(F64 + F64X)), 64
    elif t == 'f16':
        L, eb = sorted(set(F16)), 16
    elif t is not None:
        L, eb = int_edges(t[1]), 8 << t[1]
    else:
        L, eb = None, 0
    rn, rm, ra = bits(w, 5, 5), bits(w, 16, 5), bits(w, 10, 5)
    m = re.search(r'v\.(\d+)[bhsd]', form)
    lanes = int(m.group(1)) if m else 1
    lanes = max(1, min(lanes, 128 // eb)) if eb else 1
    gpr_src = re.search(r'^(scvtf|ucvtf|fmov|dup|ins|mov) [^,]+, [wx]', form) or (L is None)
    out = []
    if L is None:  # integer registers only: Rn x Rm cross product (Ra random edge)
        for xa in XEDGE:
            for xb in XEDGE:
                fpcr, x, sp, nzcv, v = make_case(r, w, form)
                if rn < 31:
                    x[rn] = xa
                if rm < 31 and rm != rn:
                    x[rm] = xb
                if ra < 31 and ra not in (rn, rm) and r.random() < 0.5:
                    x[ra] = r.choice(XEDGE)
                out.append((fpcr, x, sp, nzcv, v))
        return out
    if gpr_src:
        xs = XEDGE + XCONV
        for i, xv in enumerate(xs):
            for f in (GRID_FPCR if form[0] in 'suf' else [0]):
                fpcr, x, sp, nzcv, v = make_case(r, w, form)
                if rn < 31:
                    x[rn] = xv
                if rm < 31 and rm != rn:
                    x[rm] = xs[(i * 7 + 3) % len(xs)]
                out.append((f, x, sp, nzcv, v))
        return out
    two = rm != rn and ' v.' in form[form.index(' '):] and form.count(',') >= 2 or re.search(r'^f\w+ [sdh], [sdh], [sdh]', form)
    pairs = [(a, b) for a in L for b in L] if two else [(a, a) for a in L]
    msk = (1 << eb) - 1
    for i in range(0, len(pairs), lanes):
        grp = pairs[i:i + lanes]
        for f in ([0] if two else GRID_FPCR):
            fpcr, x, sp, nzcv, v = make_case(r, w, form)
            an = bn = 0
            for j, (a, b) in enumerate(grp):
                an |= (a & msk) << (eb * j)
                bn |= (b & msk) << (eb * j)
            v[rn] = an | (v[rn] & ~((1 << (eb * len(grp))) - 1))
            if two:
                v[rm] = bn | (v[rm] & ~((1 << (eb * len(grp))) - 1))
                if ra not in (rn, rm) and form.startswith(('fmadd', 'fmsub', 'fnmadd', 'fnmsub')):
                    v[ra] = r.choice(L) | (v[ra] & ~msk)
            if two and r.random() < 0.1:
                fpcr = r.choice(GRID_FPCR)
            else:
                fpcr = f
            out.append((fpcr, x, sp, nzcv, v))
    return out


def const_prefix(w, fpcr, x, sp, nzcv, v):
    """The case's inputs as C constants for the registers the word can name (Rd, Rn, Rm, Ra/Rt2) plus FPCR and NZCV;
    the others come from the record and are not constant."""
    regs = sorted({bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5), bits(w, 10, 5), (bits(w, 0, 5) + 1) % 32})
    s = [f'c->x[{i}] = 0x{x[i]:x}ull;' for i in regs if i < 31]
    s += [f'c->sp = 0x{sp:x}ull; c->nzcv = 0x{nzcv:x}ull; c->fpcr = 0x{fpcr:x}ull;']
    s += [f'c->v[{i}] = ((__uint128_t)0x{v[i] >> 64:x}ull << 64) | 0x{v[i] & ((1 << 64) - 1):x}ull;' for i in regs]
    return ' '.join(s)


def collect(args, r):
    """[(module, word, pc, form, class, weight)] picked per form, highest score first."""
    rows = tsv(CENSUS)
    want = {row['form']: (row['class'], risk(row['form'])[1]) for row in rows}
    classes = set(args.classes.split(',')) if args.classes else None
    fre = re.compile(args.forms) if args.forms else None
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    byform = defaultdict(list)
    images = []
    for name, path, base in (('main', ELF, BASE), ('sdk', SDK_ELF, SDK_BASE)):
        img = ElfImage(str(path))
        images.append((name, base, img))
        if name == 'main':
            ranges = [(s, e) for s, e, k, _ in functions() if k in ('function', 'thunk')]
        else:
            lo, n = img.text
            ranges = [(lo, lo + n)]
        first = {}
        for s, e in ranges:
            ws = np.frombuffer(bytes(img.image[s:e]), dtype='<u4')
            u, i = np.unique(ws, return_index=True)
            for w, j in zip(u.tolist(), i.tolist()):
                first.setdefault(w, s + 4 * j)
        for w, off in first.items():
            byform[w].append((name, base + off))
    forms = defaultdict(list)
    for w, locs in byform.items():
        d = next(md.disasm(struct.pack('<I', w), 0), None)
        if d is None:
            continue
        f = (d.mnemonic + ' ' + shape(d.op_str)).strip()
        if f not in want:
            continue
        c, wt = want[f]
        if classes and c not in classes or fre and not fre.search(f) or classify(w) == 'skip':
            continue
        forms[f].append((w, locs[0]))
    picks = []
    for f, lst in forms.items():
        c, wt = want[f]
        r.shuffle(lst)
        n = args.words * (4 if wt >= 8 else 2 if wt >= 5 else 1)
        for w, (name, pc) in lst[:n]:
            picks.append((name, w, pc, f, c, wt))
            if args.alias and c not in ('ldst', 'branch', 'excl', 'sysreg') and not is_mem(w):
                # register-aliasing variants (Rd = Rn, Rd = Rm, Rn = Rm, all equal): in-place lane updates
                d, nn, m = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
                base = w & ~((31 << 0) | (31 << 5) | (31 << 16))
                for vd, vn, vm in ((nn, nn, m), (m, nn, m), (d, nn, nn), (nn, nn, nn)):
                    if 31 in (vd, vn, vm):
                        continue
                    v2 = base | vd | (vn << 5) | (vm << 16)
                    if v2 != w and not f.startswith('adr'):
                        picks.append(('alias', v2, None, f, c, wt))
    return picks, images


def main():
    from snap_common import lower_priority
    lower_priority()
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--words', type=int, default=6, help='distinct words per form (x4 for weight >= 8, x2 for >= 5)')
    ap.add_argument('--scale', type=float, default=1.0, help='cases per word = scale x weight x 2')
    ap.add_argument('--const-cases', type=int, default=2, help='const-mode cases per word')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--modes', default='run,const')
    ap.add_argument('--classes', help='only these census classes (comma list)')
    ap.add_argument('--forms', help='only forms matching this regex')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument('--opt', default='-O2')
    ap.add_argument('--cflags', default='-march=x86-64-v3 -Wno-deprecated-declarations')
    ap.add_argument('--show', type=int, default=60)
    ap.add_argument('--regress', help='append failing cases to this file')
    ap.add_argument('--alias', action='store_true', help='add register-aliasing variants of each word')
    ap.add_argument('--grid', action='store_true', help='edge-value cross product on one word per form (GRID_CLASSES)')
    args = ap.parse_args()
    r = random.Random(args.seed)
    t0 = time.time()
    picks, images = collect(args, r)
    print(f'{len(picks)} words from {len({p[3] for p in picks})} forms ({time.time() - t0:.0f}s)')

    WORK.mkdir(parents=True, exist_ok=True)
    img_paths = []
    for name, base, img in images:
        p = WORK / f'{name}.img'
        if not p.exists() or p.stat().st_size != len(img.image):
            p.write_bytes(bytes(img.image))
        img_paths.append((base, p))
    # aliasing variants are not in the images: they run from a code page of their own (no PC-relative forms)
    alias = [i for i, p in enumerate(picks) if p[0] == 'alias']
    for j, i in enumerate(alias):
        picks[i] = ('alias', picks[i][1], ALIAS_CODE + 4 * j) + picks[i][3:]
    alias_code = struct.pack(f'<{len(alias)}I', *[picks[i][1] for i in alias]) + bytes(64)
    emu = Emu([(base, bytes(img.image)) for _, base, img in images] + [(ALIAS_CODE, alias_code)])
    mu = emu.mu
    buf0 = bytes(r.getrandbits(8) for _ in range(4096)) * (BUF_SIZE // 4096)
    mu.mem_write(BUF, buf0)
    lifter = {name: Lifter(base, 'c') for name, base, _ in images}
    lifter['alias'] = Lifter(ALIAS_CODE, 'c')

    # Lift once; cases from Unicorn.
    bodies, cases, expect, meta = [], [], [], []
    const_bodies, const_cases, const_expect = [], [], []
    modes = set(args.modes.split(','))
    unsup = Counter()
    grid_done = set()
    ucfail, accepted = Counter(), Counter()
    t1 = time.time()
    for name, w, pc, form, cls, wt in picks:
        try:
            body = to_test_body(lifter[name].lift(w, pc), pc)
        except Unsupported:
            unsup[form] += 1
            continue
        k = len(bodies)
        bodies.append(body)
        meta.append((name, w, pc, form, cls))
        mem = is_mem(w)
        st = is_store(w)
        n = max(2, int(args.scale * wt * 2))
        nconst = args.const_cases if 'const' in modes and cls in CONST_CLASSES and not mem else 0
        gen = None
        if args.grid and cls in GRID_CLASSES and form not in grid_done and not mem:
            grid_done.add(form)
            gen = grid_cases(r, w, form)
            n = len(gen)
        for i in range(n):
            fpcr, x, sp, nzcv, v = gen[i] if gen else make_case(r, w, form)
            try:
                ox, osp, onz, opc, ov = emu.run(pc, fpcr, x, sp, nzcv, v)
            except uc.UcError:
                if mem:
                    mu.mem_write(BUF, buf0)
                ucfail[form] += 1
                continue
            accepted[form] += 1
            mh = 0
            if st:
                mh = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
            if mem:
                mu.mem_write(BUF, buf0)
            want = (ox, osp, onz, opc, ov, mh)
            if 'run' in modes:
                cases.append((k, fpcr, x, sp, nzcv, v, st))
                expect.append(want)
            if i < nconst and not fpcr & (1 << 24):
                # FPCR.FZ is modelled by MXCSR FTZ/DAZ, which clang's constant folding cannot see; the game never sets
                # FZ (main has no MSR FPCR; sdk only restores a saved FPCR), so const mode tests FZ = 0 only.
                kc = len(const_bodies)
                const_bodies.append(const_prefix(w, fpcr, x, sp, nzcv, v) + ' ' + body)
                const_cases.append((kc, fpcr, x, sp, nzcv, v, False, k))
                const_expect.append(want)
    print(f'unicorn: {len(cases)} run cases, {len(const_cases)} const cases ({time.time() - t1:.0f}s); '
          f'lifter rejected {sum(unsup.values())} words')

    untested = sorted(f for f in ucfail if not accepted[f])
    if untested:
        print(f'forms Unicorn faulted on in every case (untested): {len(untested)}: ' + '; '.join(untested[:40]))
    cflags = args.cflags + ' ' + args.opt
    results = []
    if 'run' in modes and cases:
        work = WORK / 'run'
        work.mkdir(exist_ok=True)
        t2 = time.time()
        exe = build_harness(work, bodies, args.jobs, cflags)
        recs, err = run_harness(exe, work, buf0, cases, img_paths)
        print(f'run: compiled and ran {len(recs)} of {len(cases)} ({time.time() - t2:.0f}s)' + (f' ERROR {err}' if err else ''))
        results.append(('run', cases, expect, recs, [c[0] for c in cases]))
    if 'const' in modes and const_cases:
        work = WORK / 'const'
        work.mkdir(exist_ok=True)
        t2 = time.time()
        exe = build_harness(work, const_bodies, args.jobs, cflags)
        recs, err = run_harness(exe, work, buf0, [c[:7] for c in const_cases], img_paths)
        print(f'const: compiled and ran {len(recs)} of {len(const_cases)} ({time.time() - t2:.0f}s)' + (f' ERROR {err}' if err else ''))
        results.append(('const', const_cases, const_expect, recs, [c[7] for c in const_cases]))

    fails = {}
    nfail = Counter()
    reg = []
    for mode, cs, ex, recs, ks in results:
        for case, want, got, k in zip(cs, ex, recs, ks):
            d = diff_state(got, want, check_pc=got[3] != 0, check_mem=case[6])
            if d:
                nfail[mode] += 1
                name, w, pc, form, cls = meta[k]
                key = (mode, w)
                if key not in fails:
                    fails[key] = (meta[k], case, d[:4])
                reg.append((mode, meta[k], case))
    per_form = Counter()
    for (mode, w), (m, case, d) in fails.items():
        per_form[(mode, m[3])] += 1
    print(f'{len(bodies)} words, {len(cases)} run + {len(const_cases)} const cases: failing cases {dict(nfail)}, '
          f'failing words {len(fails)} ({time.time() - t0:.0f}s total)')
    for (mode, f), n in per_form.most_common(80):
        print(f'  {mode:5} {n:4} words  {f}')
    for (mode, w), (m, case, d) in list(fails.items())[:args.show]:
        name, _, pc, form, cls = m
        fpcr, x, v = case[1], case[2], case[5]
        regs = sorted({bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5), bits(w, 10, 5)})
        ins = ' '.join(f'x{j}={x[j]:x}' for j in regs if j < 31) + ' | ' + ' '.join(f'v{j}={v[j]:032x}' for j in regs)
        print(f'  [{mode}] {name}+{pc - {"main": BASE, "sdk": SDK_BASE, "alias": ALIAS_CODE}[name]:x} {w:08x} {dis(w, pc):40} fpcr={fpcr:x}')
        print(f'      in: {ins}')
        print(f'      {"; ".join(d)}')
    if args.regress and reg:
        with open(args.regress, 'a') as fh:
            for mode, (name, w, pc, form, cls), case in reg:
                fpcr, x, sp, nzcv, v = case[1:6]
                fh.write(f'{w:08x}\t{name}\t{pc:x}\t{fpcr:x}\t{nzcv:x}\t{sp:x}\t' + ','.join(f'{q:x}' for q in x[:31]) +
                         '\t' + ','.join(f'{q:x}' for q in v) + '\n')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
