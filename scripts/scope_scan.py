#!/usr/bin/env python3
"""scope_scan.py - read-only static scan of the v262144 main ELF for the NVN, import and thread census.

Input : analysis/import/v262144-main.elf and the analysis/maps tables (read only).
Output: a pickle (path given by --out) with per-function events, consumed by scope_report.py.
Run with:  python -I scripts/scope_scan.py --out <pkl>

Method: linear scan of every function body with a tiny register tracker (adrp/add/ldr/mov, reset at
branch targets and after calls) that resolves
  * bl/b to PLT stubs                -> import call
  * adrp+ldr of a GOT slot           -> import / address load (and "then blr")
  * loads of the NVN proc variables  -> NVN call site when followed by blr
  * function and vtable addresses    -> address-taken edges for the tier-2 call graph
Not a decompiler: control flow is not followed, spills to the stack lose the value, and a loaded pointer
that is moved through memory before the blr is recorded as a load only.
"""
import argparse, bisect, ctypes, os, pickle, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, 'analysis', 'import', 'v262144-main.elf')
MAPS = os.path.join(ROOT, 'analysis', 'maps')
FOFF = {0: 0x1000, 0xa39000: 0xa3a000, 0xc61000: 0xc62000}  # vaddr base -> file offset (3 PT_LOAD)
SEGS = [(0x0, 0xa38e40, 0x1000), (0xa39000, 0xc60bc4, 0xa3a000), (0xc61000, 0xc61000 + 0x6a908, 0xc62000)]
BSS = (0xccc000, 0xccc000 + 0x1bc56f8)
GOT = (0xcc77c8, 0xcc77c8 + 0x3b10)
GOTPLT = (0xcc6368, 0xcc6368 + 0x1460)


def set_lowprio():
    try:
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    except Exception:
        pass


class Image:
    def __init__(self):
        self.d = open(ELF, 'rb').read()

    def rd(self, a, n):
        for lo, hi, fo in SEGS:
            if lo <= a < hi:
                return self.d[a + (fo - lo):a + (fo - lo) + n]
        return b'\0' * n

    def u64(self, a):
        return int.from_bytes(self.rd(a, 8), 'little')

    def cstr(self, a):
        for lo, hi, fo in SEGS:
            if lo <= a < hi:
                o = a + (fo - lo)
                e = self.d.index(b'\0', o)
                return self.d[o:e].decode('latin1')
        return None


def tsv(name):
    rows = []
    hdr = None
    for ln in open(os.path.join(MAPS, name), encoding='utf-8'):
        ln = ln.rstrip('\n')
        if ln.startswith('#') or not ln:
            continue
        f = ln.split('\t')
        if hdr is None:
            hdr = f
            continue
        rows.append(dict(zip(hdr, f)))
    return rows


def hx(s):
    return int(s, 16) if s else None


def load_funcs():
    funcs = []  # (start,end,kind)
    for r in tsv('functions_final.tsv'):
        if r['kind'] in ('function', 'thunk', 'unresolved'):
            funcs.append((hx(r['start']), hx(r['end']), r['kind']))
    funcs.sort()
    plt = {}
    for r in tsv('functions_final.tsv'):
        if r['kind'] == 'plt_stub':
            plt[hx(r['start'])] = hx(r['end'])
    return funcs, plt


def load_relocs():
    rel = {}
    for r in tsv('relocations.tsv'):
        rel[hx(r['offset'])] = (r['type'], r['symbol'], hx(r['addend']) if r['addend'] else 0)
    return rel


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) else v


class Tracker:
    """register tracker. state values: ('a',addr) address constant; ('g',name) import symbol via GOT;
    ('l',ea) value loaded from memory at ea; ('p', addr, got_slot) address obtained from a GOT slot."""

    def __init__(self):
        self.r = [None] * 32

    def reset(self):
        self.r = [None] * 32


def decode_range(img, lo, hi, rel, on_event, branch_targets=None):
    """walk words in [lo,hi). Calls on_event(kind, pc, info). kinds:
    bl(target), b(target), blr(reg state), br(reg state), addr(value), ldgot(slot,reloc), ldmem(ea, reg state),
    straddr(ea)"""
    data = img.rd(lo, hi - lo)
    n = (hi - lo) // 4
    import struct
    words = struct.unpack('<%dI' % n, data[:n * 4])
    tg = set()
    for i, w in enumerate(words):
        pc = lo + 4 * i
        if (w & 0xfc000000) in (0x14000000, 0x94000000):
            tg.add(pc + sx(w & 0x3ffffff, 26) * 4)
        elif (w & 0xff000010) == 0x54000000 or (w & 0x7e000000) == 0x34000000:
            tg.add(pc + sx((w >> 5) & 0x7ffff, 19) * 4)
        elif (w & 0x7e000000) == 0x36000000:
            tg.add(pc + sx((w >> 5) & 0x3fff, 14) * 4)
    R = [None] * 32
    for i, w in enumerate(words):
        pc = lo + 4 * i
        if pc in tg:
            R = [None] * 32
        rd = w & 31
        rn = (w >> 5) & 31
        if (w & 0x9f000000) == 0x90000000:  # adrp
            imm = sx((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21) << 12
            R[rd] = ('a', (pc & ~0xfff) + imm, 'page')
            continue
        if (w & 0xff800000) == 0x91000000:  # add x, x, #imm
            imm = ((w >> 10) & 0xfff) << (12 if (w >> 22) & 1 else 0)
            s = R[rn] if rn != 31 else None
            if s and s[0] == 'a':
                v = s[1] + imm
                R[rd] = ('a', v, 'full')
                on_event('addr', pc, v)
            else:
                R[rd] = None
            continue
        if (w & 0xffc00000) == 0xf9400000:  # ldr x, [xn, #imm]
            imm = ((w >> 10) & 0xfff) * 8
            s = R[rn] if rn != 31 else None
            if s and s[0] in ('a', 'p'):
                ea = s[1] + imm
                if GOT[0] <= ea < GOT[1] or GOTPLT[0] <= ea < GOTPLT[1]:
                    rl = rel.get(ea)
                    if rl:
                        if rl[0] == 'RELATIVE':
                            R[rd] = ('p', rl[2], ea)
                            on_event('ldgot', pc, (ea, rl))
                            on_event('addr', pc, rl[2])
                        else:
                            R[rd] = ('g', rl[1], ea)
                            on_event('ldgot', pc, (ea, rl))
                    else:
                        R[rd] = ('l', ea)
                else:
                    R[rd] = ('l', ea)
                    on_event('ldmem', pc, (ea, rd))
            else:
                R[rd] = None
            continue
        if (w & 0xffe0ffe0) == 0xaa0003e0:  # mov x, x
            R[rd] = R[(w >> 16) & 31]
            continue
        if (w & 0xfc000000) == 0x94000000:  # bl
            tgt = pc + sx(w & 0x3ffffff, 26) * 4
            on_event('bl', pc, tgt)
            for k in range(19):
                R[k] = None
            R[30] = None
            continue
        if (w & 0xfc000000) == 0x14000000:  # b
            on_event('b', pc, pc + sx(w & 0x3ffffff, 26) * 4)
            R = [None] * 32
            continue
        if (w & 0xfffffc1f) == 0xd63f0000:  # blr
            on_event('blr', pc, R[rn] if rn != 31 else None)
            for k in range(19):
                R[k] = None
            R[30] = None
            continue
        if (w & 0xfffffc1f) == 0xd61f0000:  # br
            on_event('br', pc, R[rn] if rn != 31 else None)
            R = [None] * 32
            continue
        if w == 0xd65f03c0:
            R = [None] * 32
            continue
        # store of x0 to known address (loader parsing)
        if (w & 0xffc00000) == 0xf9000000:  # str x, [xn,#imm]
            s = R[rn] if rn != 31 else None
            if s and s[0] in ('a', 'p'):
                on_event('str', pc, (s[1] + ((w >> 10) & 0xfff) * 8, rd, s[0]))
            continue
        # generic: invalidate destination(s)
        cls = w & 0x0a000000
        R[rd] = None
        if cls == 0x08000000:  # load/store class
            if (w & 0x3a000000) == 0x28000000:  # pair
                R[(w >> 10) & 31] = None
                if ((w >> 23) & 3) in (1, 3) and rn != 31:
                    R[rn] = None
            elif not (w >> 24) & 1 and not (w >> 21) & 1 and ((w >> 10) & 3) in (1, 3) and rn != 31:
                R[rn] = None


def parse_loaders(img, rel):
    """returns {proc_name: {'A': got_slot_target_var, 'B': bss_var}} from loaders 0x254c40 and 0x90a0c0"""
    out = {}
    for tag, fa, fe in (('A', 0x254c40, 0x2584cc), ('B', 0x90a0c0, 0x90d13c)):
        pend = {}
        state = {'name': None}
        last_str = {}

        def ev(kind, pc, info, tag=tag, state=state):
            if kind == 'addr':
                s = img.cstr(info)
                if s and s.startswith('nvn'):
                    state['name'] = s
            elif kind == 'str' and state['name']:
                ea, reg, src = info
                if reg == 0:
                    out.setdefault(state['name'], {})[tag] = ea
                    state['name'] = None
        decode_range(img, fa, fe, rel, ev)
    return out


def main():
    set_lowprio()
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    img = Image()
    rel = load_relocs()
    funcs, plt = load_funcs()
    starts = [f[0] for f in funcs]

    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and funcs[i][0] <= addr < funcs[i][1]:
            return funcs[i][0]
        return None

    # imports: GOT slot / plt stub -> name
    imp_by_got = {}
    for r in tsv('imports.tsv'):
        for g in (r['got_slots'] or '').split(';'):
            if g:
                imp_by_got[hx(g)] = r['name']
    # plt stub -> got slot: decode adrp/ldr/br in the stub
    import struct
    plt_name = {}
    for st in plt:
        w = struct.unpack('<3I', img.rd(st, 12))
        imm = sx((((w[0] >> 5) & 0x7ffff) << 2) | ((w[0] >> 29) & 3), 21) << 12
        ea = (st & ~0xfff) + imm + ((w[1] >> 10) & 0xfff) * 8
        if ea in rel and rel[ea][0] != 'RELATIVE':
            plt_name[st] = rel[ea][1]
    print('plt stubs resolved', len(plt_name), 'of', len(plt), file=sys.stderr)

    procs = parse_loaders(img, rel)
    print('loader procs', len(procs), sum('A' in v for v in procs.values()), sum('B' in v for v in procs.values()), file=sys.stderr)
    varA = {v['A']: n for n, v in procs.items() if 'A' in v}
    varB = {v['B']: n for n, v in procs.items() if 'B' in v}

    # vtable runs (for address-taken edges)
    vt_slots = {}
    for r in tsv('vtables_candidate.tsv'):
        s0 = hx(r['start_offset'])
        sl = [hx(x) for x in r['slots'].split(';') if x and not x.startswith('@')]
        vt_slots[s0] = sl
    vt_sorted = sorted(vt_slots)
    fset = set(starts)

    ev_by_func = defaultdict(lambda: {'calls': [], 'plt': [], 'imp_ld': [], 'nvn': [], 'nvn_ld': [], 'taken': {}, 'vt': {}, 'blr': 0})
    allvars = {}
    for v, n in varA.items():
        allvars[v] = ('A', n)
    for v, n in varB.items():
        allvars[v] = ('B', n)

    for fi, (fs, fe, kind) in enumerate(funcs):
        E = ev_by_func[fs]
        got_pending = {}

        def ev(k, pc, info):
            if k in ('bl', 'b'):
                t = info
                if t in plt_name:
                    E['plt'].append((pc, plt_name[t], k))
                elif fs <= t < fe and k == 'b':
                    pass
                else:
                    tf = func_of(t)
                    if tf is not None:
                        E['calls'].append((pc, tf, k, t))
                    else:
                        E['calls'].append((pc, None, k, t))
            elif k == 'blr':
                E['blr'] += 1
                if info:
                    if info[0] == 'l' and info[1] in allvars:
                        E['nvn'].append((pc, info[1]))
                    elif info[0] == 'g':
                        E['imp_ld'].append((pc, info[1], True))
                        E['imp_called'] = E.get('imp_called', 0) + 1
            elif k == 'br':
                if info and info[0] == 'g':
                    E['imp_ld'].append((pc, info[1], True))
                elif info and info[0] == 'l' and info[1] in allvars:
                    E['nvn'].append((pc, info[1]))
            elif k == 'ldgot':
                ea, rl = info
                if rl[0] != 'RELATIVE':
                    E['imp_ld'].append((pc, rl[1], False))
            elif k == 'ldmem':
                ea, rd = info
                if ea in allvars:
                    E['nvn_ld'].append((pc, ea))
            elif k == 'addr':
                v = info
                if v in fset and v != fs:
                    E['taken'].setdefault(v, pc)
                elif v in vt_slots or (v - 16) in vt_slots:
                    E['vt'].setdefault(v if v in vt_slots else v - 16, pc)
        decode_range(img, fs, fe, rel, ev)

    # cleanup: blr-through-import flagged in imp_ld with True are calls. Drop defaultdict
    res = {
        'procs': procs, 'funcs': funcs, 'plt': plt, 'plt_name': plt_name, 'vt_slots': vt_slots,
        'events': {k: {kk: vv for kk, vv in v.items()} for k, v in ev_by_func.items()},
        'allvars': allvars,
    }
    pickle.dump(res, open(a.out, 'wb'))
    print('done', len(ev_by_func), file=sys.stderr)


if __name__ == '__main__':
    main()
