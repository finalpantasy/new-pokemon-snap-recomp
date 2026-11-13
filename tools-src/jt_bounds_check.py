"""Static check that no recovered jump table can miss a case (project tooling, ours).

For every `br xN` of main and sdk that has the shape clang gives a dense switch (table load, add to an anchor, br),
this re-derives the table independently of recomp/jumptables.py: it finds the bounds check on the index register
(`cmp wI, #K` with `b.hi`/`b.hs` to the default, or `b.ls`/`b.lo` into the dispatch block), reads K (+1) entries from
the ELF and computes their targets. Then it checks that the generated C has a `switch` whose cases cover them all.
A table whose bound it cannot find is listed as `unchecked` with the reason, for a manual look.

Usage: tools/venv-recomp/Scripts/python -I tools-src/jt_bounds_check.py [--gen generated/v262144] [--module main|sdk|both]
Exit status 1 when a table misses a case.
"""
import argparse
import re
import sys
from pathlib import Path

import capstone
from capstone import arm64_const as A
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
ELFS = {'main': (ROOT / 'analysis' / 'import' / 'v262144-main.elf', 0x7100000000),
        'sdk': (ROOT / 'analysis' / 'import' / 'v262144-sdk.elf', 0x7110000000)}
LOOKBACK = 48   # instructions searched above the br for the load, anchor and bound


class Image:
    def __init__(self, path):
        self.segs = []
        with open(path, 'rb') as f:
            e = ELFFile(f)
            for s in e.iter_segments():
                if s['p_type'] == 'PT_LOAD':
                    self.segs.append((s['p_vaddr'], s.data().ljust(s['p_memsz'], b'\0')))
            self.text = next((s['sh_addr'], s['sh_size']) for s in e.iter_sections() if s.name == '.text')

    def read(self, a, n):
        for va, d in self.segs:
            if va <= a and a + n <= va + len(d):
                return d[a - va:a - va + n]
        raise KeyError(hex(a))


def reg_id(cs, r):
    """Register number 0..30 of an x/w operand, or None."""
    n = cs.reg_name(r)
    if n and n[0] in 'xw' and n[1:].isdigit():
        return int(n[1:])
    return None


def writes(cs, ins, r):
    """True if ins writes general register r (x or w view)."""
    if ins.mnemonic in ('cmp', 'cmn', 'tst', 'ccmp', 'ccmn', 'fcmp', 'fccmp'):
        return False   # capstone 5 lists the first operand of these aliases as written
    try:
        _, w = ins.regs_access()
    except capstone.CsError:
        return True
    return any(reg_id(cs, x) == r for x in w)


WINDOW = 2048   # instructions each side of a br searched for branches into its dispatch block
_window = {}


def window(cs, img, br):
    """(instructions around br by address, {target: [branch instructions]}) for the predecessor search."""
    key = br >> 14
    if key not in _window:
        t0, tn = img.text
        lo = max(t0, ((key << 14) - 4 * WINDOW)), min(t0 + tn, (key << 14) + (1 << 14) + 4 * WINDOW)
        lo, hi = lo
        by, into = {}, {}
        for i in cs.disasm(img.read(lo, hi - lo), lo):
            by[i.address] = i
            if i.mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or i.mnemonic.startswith('b.'):
                op = i.operands[-1]
                if op.type == A.ARM64_OP_IMM:
                    into.setdefault(op.imm, []).append(i)
        _window.clear()
        _window[key] = (by, into)
    return _window[key]


def cond_count(c, K, into):
    """Entries a `cmp I, #K; b.c` bound allows; into: the branch goes to the dispatch (its condition holds in range)."""
    if c == 'hi' and not into or c == 'ls' and into:
        return K + 1
    if c in ('hs', 'cs') and not into or c in ('lo', 'cc') and into:
        return K
    return None


def bound(cs, img, a, idx, br, depth, st=None):
    """(entries, why): the bound on register idx where the instruction at a runs (first the table load), from the cmp
    that guards it. Walks up the block; follows `mov` copies; `and #2^n-1` bounds by itself; where other branches
    enter (a block start or a join below the cmp) every entering path must carry a bound, and the largest wins."""
    by, into = window(cs, img, br)
    if depth > 4:
        return None, 'too deep'
    mask, cands, pend = (st[0], dict(st[1]), dict(st[2])) if st else (None, {}, {})
    q = a   # q: the instruction the walk has reached (not yet looked at the one above it)
    while True:
        enter = [b for b in into.get(q, []) if not (q <= b.address < a)]   # branches from outside the walked span
        p = q - 4
        x = by.get(p)
        if x is None:
            return (mask, 'and') if mask else (None, 'left window')
        if enter or not fallthrough(x):
            st = (mask, cands, pend)
            sub = [branch_bound(cs, img, b, idx, br, depth, st) for b in enter]
            if fallthrough(x):
                sub.append(bound(cs, img, q, idx, br, depth + 1, st) if q != a else walk_on(cs, img, q, idx, br, depth, st))
            if not sub:
                return (mask, 'and') if mask else (None, f'no branch into {q:#x} in window')
            bad = [s for s in sub if s[0] is None]
            if bad:
                return (mask, 'and') if mask else (None, bad[0][1])
            n = max(s[0] for s in sub)
            return (min(n, mask) if mask else n), 'paths'
        m = x.mnemonic
        if m == 'cmp' and x.operands[1].type == A.ARM64_OP_IMM:
            c = next((by[r] for r in range(p + 4, a, 4) if by[r].mnemonic.startswith('b.')), None)
            n = cond_count(c.mnemonic[2:], x.operands[1].imm, p < c.operands[0].imm <= br) if c else None
            if reg_id(cs, x.operands[0].reg) == idx:
                if c is None:
                    return None, 'cmp without branch'
                if n is None:
                    return None, f'bound cond {c.mnemonic}'
                return (min(n, mask) if mask else n), 'cmp'
            if n is not None:   # `and wR, wJ, #m; cmp wR, #K` next to `and xI, xJ, #m`: the cmp bounds the same value
                cands.setdefault(reg_id(cs, x.operands[0].reg), n)
        r = next((r for r in cands if writes(cs, x, r)), None)
        if r is not None:
            if m == 'and' and x.operands[2].type == A.ARM64_OP_IMM:
                key = (reg_id(cs, x.operands[1].reg), x.operands[2].imm + 1)
                if mask and key == (idx, mask):
                    return min(cands[r], mask), 'and+cmp'
                pend[key] = cands[r]
            del cands[r]
        if writes(cs, x, idx):
            if m == 'mov' and x.operands[1].type == A.ARM64_OP_REG:
                idx = reg_id(cs, x.operands[1].reg)
            elif m == 'mov' and x.operands[1].type == A.ARM64_OP_IMM:   # a constant index on this path
                return x.operands[1].imm + 1, 'const'
            elif m == 'and' and x.operands[2].type == A.ARM64_OP_IMM and (x.operands[2].imm + 1) & x.operands[2].imm == 0:
                mask = min(mask or 1 << 62, x.operands[2].imm + 1)
                idx = reg_id(cs, x.operands[1].reg)
                if (idx, mask) in pend:
                    return min(pend[(idx, mask)], mask), 'and+cmp'
            else:
                return (mask, 'and') if mask else (None, f'index written by {m} before a bound')
        q = p
        a_seen = True


def walk_on(cs, img, q, idx, br, depth, st):
    """The fallthrough path into q when q is where the walk started: continue above it ignoring q's own branches."""
    by, into = window(cs, img, br)
    saved = into.pop(q, None)
    try:
        return bound(cs, img, q, idx, br, depth + 1, st)
    finally:
        if saved is not None:
            into[q] = saved


def fallthrough(x):
    return x.mnemonic not in ('b', 'br', 'ret', 'brk')


def branch_bound(cs, img, b, idx, br, depth, st):
    """Bound carried by the branch b into a block of the dispatch path."""
    by, _ = window(cs, img, br)
    if b.mnemonic.startswith('b.'):
        q = b.address - 4   # cmp idx, #K; b.c here: the condition holds on this edge
        while q in by and b.address - q <= 16:
            x = by[q]
            r = reg_id(cs, x.operands[0].reg) if x.mnemonic == 'cmp' else None
            d = by.get(q - 4)
            alias = (r is not None and r != idx and st[0] and d is not None and d.mnemonic == 'and' and writes(cs, d, r)
                     and reg_id(cs, d.operands[1].reg) == idx and d.operands[2].type == A.ARM64_OP_IMM
                     and d.operands[2].imm + 1 == st[0])   # `and wR, wI, #m; cmp wR, #K; b.c` while the load masks I by m
            if x.mnemonic == 'cmp' and (r == idx or alias) and x.operands[1].type == A.ARM64_OP_IMM:
                n = cond_count(b.mnemonic[2:], x.operands[1].imm, True)
                if n is not None and st[0]:
                    n = min(n, st[0])
                return (n, 'pred cmp') if n is not None else (None, f'pred cond {b.mnemonic}')
            if writes(cs, x, idx) or x.mnemonic in ('cmp', 'cmn', 'tst', 'subs', 'adds', 'ands', 'ccmp', 'fcmp'):
                break
            q -= 4
        # no cmp on this edge: the bound may hold already where the b.c runs
        return bound(cs, img, b.address, idx, br, depth + 1, st)
    if b.mnemonic == 'b':
        return bound(cs, img, b.address, idx, br, depth + 1, st)   # whatever bounds the path reaching the b
    return None, f'pred {b.mnemonic} at {b.address:#x}'


def analyse(cs, img, br):
    """(targets, why) for the table of the br at br; targets None when the shape or bound is not recognised."""
    lo = max(br - 4 * LOOKBACK, img.text[0])
    insns = list(cs.disasm(img.read(lo, br + 4 - lo), lo))
    if not insns or insns[-1].address != br or insns[-1].mnemonic != 'br':
        return None, 'disasm'
    t = reg_id(cs, insns[-1].operands[0].reg)
    k = len(insns) - 2
    # add xT, xA, wO, sxt? #2 | add xT, xA, xO, lsl #2  (or br of the loaded value itself: absolute 8-byte table)
    anchor = shift = None
    load_dst = t
    while k >= 0 and not writes(cs, insns[k], t):
        k -= 1
    if k < 0:
        return None, 'no target def'
    ins = insns[k]
    if ins.mnemonic == 'add' and len(ins.operands) == 3 and ins.operands[2].type == A.ARM64_OP_REG:
        a_reg = reg_id(cs, ins.operands[1].reg)
        load_dst = reg_id(cs, ins.operands[2].reg)
        op2 = ins.operands[2]
        shift = op2.shift.value if op2.shift.type else 0
        j = k - 1
        while j >= 0 and not writes(cs, insns[j], a_reg):
            j -= 1
        if j >= 0 and insns[j].mnemonic.startswith('ldr') and not shift and not op2.ext:
            a_reg, load_dst = load_dst, a_reg   # `add xT, xO, xA`: the loaded offset first
        if op2.ext and op2.ext not in (A.ARM64_EXT_SXTB, A.ARM64_EXT_SXTH, A.ARM64_EXT_SXTW, A.ARM64_EXT_UXTW):
            return None, f'ext {op2.ext}'
        # anchor: adr a_reg, L  (search upward)
        j = k - 1
        while j >= 0 and not writes(cs, insns[j], a_reg):
            j -= 1
        if j < 0:
            return None, 'no anchor'
        if insns[j].mnemonic == 'adr':
            anchor = insns[j].operands[1].imm
        elif insns[j].mnemonic == 'add' and insns[j].operands[2].type == A.ARM64_OP_IMM:
            base_reg = reg_id(cs, insns[j].operands[1].reg)
            i2 = j - 1
            while i2 >= 0 and not writes(cs, insns[i2], base_reg):
                i2 -= 1
            if i2 < 0 or insns[i2].mnemonic != 'adrp':
                return None, 'anchor shape'
            anchor = insns[i2].operands[1].imm + insns[j].operands[2].imm
        else:
            return None, f'anchor {insns[j].mnemonic}'
        k -= 1
        while k >= 0 and not writes(cs, insns[k], load_dst):
            k -= 1
        if k < 0:
            return None, 'no load'
        ins = insns[k]
    m = ins.mnemonic
    if m not in ('ldrb', 'ldrh', 'ldrsw', 'ldrsb', 'ldrsh', 'ldr'):
        return None, f'load {m}'
    size = {'ldrb': 1, 'ldrsb': 1, 'ldrh': 2, 'ldrsh': 2, 'ldrsw': 4, 'ldr': 8 if cs.reg_name(ins.operands[0].reg)[0] == 'x' else 4}[m]
    signed = m in ('ldrsw', 'ldrsb', 'ldrsh')
    mem = ins.operands[1]
    if mem.type != A.ARM64_OP_MEM or not mem.mem.index:
        return None, 'load addressing'
    tab_reg, idx = reg_id(cs, mem.mem.base), reg_id(cs, mem.mem.index)
    j = k - 1
    while j >= 0 and not writes(cs, insns[j], tab_reg):
        j -= 1
    if j < 0:
        return None, 'no table base'
    if insns[j].mnemonic == 'add' and insns[j].operands[2].type == A.ARM64_OP_IMM:
        r2 = reg_id(cs, insns[j].operands[1].reg)
        i2 = j - 1
        while i2 >= 0 and not writes(cs, insns[i2], r2):
            i2 -= 1
        if i2 < 0 or insns[i2].mnemonic != 'adrp':
            return None, 'table base shape'
        table = insns[i2].operands[1].imm + insns[j].operands[2].imm
    elif insns[j].mnemonic == 'adr':
        table = insns[j].operands[1].imm
    else:
        return None, f'table base {insns[j].mnemonic}'
    count, why = bound(cs, img, ins.address, idx, br, 0)
    if count is None:
        return None, why
    if count <= 0 or count > 4096:
        return None, f'count {count}'
    raw = img.read(table, count * size)
    out = []
    for n in range(count):
        v = int.from_bytes(raw[n * size:(n + 1) * size], 'little', signed=signed)
        out.append(v if anchor is None else anchor + (v << shift))
    return out, f'count {count}'


SWITCH = re.compile(r'switch \([^)]*\) \{((?: case 0x[0-9a-f]+ull: goto L_[0-9a-f]+;)+) default: break; \}')
CASE = re.compile(r'case (0x[0-9a-f]+)ull')


def switch_sets(gen):
    sets = []
    for p in sorted(gen.glob('*.c')):
        for m in SWITCH.finditer(p.read_text(errors='replace')):
            s = frozenset(int(x, 16) for x in CASE.findall(m.group(1)))
            if len(s) < 0x8000:   # fb_<entry> all-label fallbacks are huge; tables are small
                sets.append(s)
    return sets


def check(name, gen):
    path, base = ELFS[name]
    img = Image(path)
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    cs.detail = True
    cs.skipdata = True   # data words inside .text must not end a disassembly window
    sets = switch_sets(gen / name)
    by_target = {}
    for s in sets:
        for t in s:
            by_target.setdefault(t, []).append(s)
    lo, n = img.text
    code = img.read(lo, n)
    brs = [lo + o for o in range(0, n, 4) if (int.from_bytes(code[o:o + 4], 'little') & 0xFFFFFC1F) == 0xD61F0000]
    stats = {'br': len(brs), 'table_shape': 0, 'covered': 0, 'missing': 0, 'unchecked': 0}
    bad, unchecked = [], []
    for b in brs:
        tg, why = analyse(cs, img, b)
        if tg is None:
            if why not in ('no target def', 'disasm') and not why.startswith('load ') and not why.startswith('anchor ldr') \
                    and why != 'load addressing':
                unchecked.append((b, why))
            continue
        stats['table_shape'] += 1
        want = {base + t for t in tg}
        cands = by_target.get(next(iter(want)), [])
        if any(want <= s for s in cands):
            stats['covered'] += 1
        else:
            stats['missing'] += 1
            best = max(cands, key=lambda s: len(want & s), default=frozenset())
            bad.append((b, len(want), sorted(hex(t - base) for t in want - best)[:8]))
    stats['unchecked'] = len(unchecked)
    print(f'{name}: {stats}')
    for b, nw, miss in bad:
        print(f'  MISSING {name}+{b:#x}: {nw} targets, not in any switch: {miss}')
    for b, why in unchecked:
        print(f'  unchecked {name}+{b:#x}: {why}')
    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, default=ROOT / 'generated' / 'v262144')
    ap.add_argument('--module', choices=('main', 'sdk', 'both'), default='both')
    a = ap.parse_args()
    ok = True
    for m in (('main', 'sdk') if a.module == 'both' else (a.module,)):
        ok &= check(m, a.gen)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
