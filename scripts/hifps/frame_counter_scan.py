"""Static pass for frame-counted logic in main (docs/high-fps-native.md; project code, ours). Extends analysis/perf/fps_audit_scan.py
(1/60 and 60.0 loads) to integer counters a function keeps in memory and steps by one each call:

  ldr  wA, [xB, #off]      (or ldrh/ldrb)
  add  wC, wA, #1          (or sub / subs #1: a countdown)
  str  wC, [xB, #off]      same base register and offset, within 8 instructions

and then, within 12 instructions, compares the stepped value (cmp / cmn / ccmp against an immediate, or cbz / cbnz / b.eq on the flags
of a subs) or branches on it. Such a counter counts calls; if the function runs once per game frame it counts frames, so a limit on it is
a frame-count timeout that runs 2.4 times as fast at 144 fps. Each hit is listed with its function and the limit; --fns lists the
functions only (for SNAP_FNTRACE, to see which run once per frame in play). Reads the local ELF (never committed):

  tools/venv-recomp/Scripts/python -E -P scripts/hifps/frame_counter_scan.py [--fns] [--min 8] [--within <fn list file>]
"""
import argparse
import bisect
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / 'analysis' / 'import' / 'v262144-main.elf'
BASE = 0   # the local ELF is linked at 0 (main offsets)

ap = argparse.ArgumentParser()
ap.add_argument('--fns', action='store_true', help='print the distinct functions only')
ap.add_argument('--min', type=int, default=8, help='smallest compare limit kept (default 8; 1 to 7 are mostly flags and indices)')
ap.add_argument('--within', help='file of main offsets (hex, one per line): keep only hits in these functions')
a = ap.parse_args()

f = ELFFile(open(ELF, 'rb'))
text = next(s for s in f.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_flags'] & 1)
fns = []
for line in open(ROOT / 'analysis' / 'maps' / 'functions_final.tsv', encoding='utf-8'):
    p = line.split('\t')
    try:
        fns.append(int(p[0], 16))
    except ValueError:
        pass
fns.sort()


def fn_of(x):
    i = bisect.bisect_right(fns, x) - 1
    return fns[i] if i >= 0 else 0


keep = None
if a.within:
    keep = {int(l.split()[0], 16) for l in open(a.within) if l.strip() and not l.startswith('#')}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
md.skipdata = True
ins_list = list(md.disasm(text.data(), text['p_vaddr']))

LOADS = {'ldr', 'ldrh', 'ldrb', 'ldur', 'ldurh', 'ldurb'}
STORES = {'str', 'strh', 'strb', 'stur', 'sturh', 'sturb'}


def mem(op):
    return (op.mem.base, op.mem.disp) if op.type == ARM64_OP_MEM and not op.mem.index else None


def wreg(ins, r):   # w and x names of one register compare equal
    n = ins.reg_name(r)
    return n[1:] if n and n[0] in 'wx' else n


addr_index = {ins.address: i for i, ins in enumerate(ins_list)}


def armed(fn, off):
    """The largest constant >= --min the function stores at +off (mov wN, #imm; str wN, [x, #off]), or None."""
    i = addr_index.get(fn)
    if i is None:
        return None
    end = fns[bisect.bisect_right(fns, fn)] if bisect.bisect_right(fns, fn) < len(fns) else fn + 0x4000
    consts, best = {}, None
    while i < len(ins_list) and ins_list[i].address < end:
        ins = ins_list[i]
        i += 1
        if not ins.id:
            continue
        o = ins.operands
        if ins.mnemonic in ('mov', 'movz') and len(o) == 2 and o[0].type == ARM64_OP_REG and o[1].type == ARM64_OP_IMM:
            consts[wreg(ins, o[0].reg)] = o[1].imm
        elif ins.mnemonic in STORES and len(o) == 2 and o[0].type == ARM64_OP_REG and o[1].type == ARM64_OP_MEM and o[1].mem.disp == off:
            v = consts.get(wreg(ins, o[0].reg))
            if v is not None and a.min <= v < 1 << 20:
                best = max(best or 0, v)
        elif o and o[0].type == ARM64_OP_REG:
            consts.pop(wreg(ins, o[0].reg), None)
    return best


hits = []
n = len(ins_list)
for i, ld in enumerate(ins_list):
    if not ld.id or ld.mnemonic not in LOADS:
        continue
    ops = ld.operands
    if len(ops) != 2 or ops[0].type != ARM64_OP_REG or not mem(ops[1]):
        continue
    if ld.reg_name(ops[0].reg)[0] not in 'wx':
        continue
    loc = mem(ops[1])
    ra = wreg(ld, ops[0].reg)
    for j in range(i + 1, min(n, i + 4)):
        st = ins_list[j]
        if not st.id:
            break
        o = st.operands
        if st.mnemonic in ('add', 'adds', 'sub', 'subs') and len(o) == 3 and o[2].type == ARM64_OP_IMM and o[2].imm == 1 and o[1].type == ARM64_OP_REG and wreg(st, o[1].reg) == ra:
            rc = wreg(st, o[0].reg)
            kind = 'up' if st.mnemonic.startswith('add') else 'down'
            stored = None
            for k in range(j + 1, min(n, j + 8)):
                s2 = ins_list[k]
                if not s2.id:
                    break
                o2 = s2.operands
                if s2.mnemonic in STORES and len(o2) == 2 and o2[0].type == ARM64_OP_REG and wreg(s2, o2[0].reg) == rc and mem(o2[1]) == loc:
                    stored = k
                    break
                if s2.mnemonic in ('bl', 'blr', 'ret', 'b', 'br'):
                    break
            if stored is None:
                break
            limit = None
            how = None
            for k in range(j + 1, min(n, j + 13)):
                c = ins_list[k]
                if not c.id:
                    break
                o3 = c.operands
                if c.mnemonic in ('cmp', 'cmn', 'ccmp') and len(o3) >= 2 and o3[0].type == ARM64_OP_REG and wreg(c, o3[0].reg) in (rc, ra) and o3[1].type == ARM64_OP_IMM:
                    limit, how = o3[1].imm, c.mnemonic
                    break
                if c.mnemonic in ('cbz', 'cbnz') and o3[0].type == ARM64_OP_REG and wreg(c, o3[0].reg) == rc and kind == 'down':
                    limit, how = 0, c.mnemonic
                    break
                if c.mnemonic.startswith('b.') and st.mnemonic == 'subs':
                    limit, how = 0, 'subs+' + c.mnemonic
                    break
                if c.mnemonic in ('bl', 'blr', 'ret', 'br'):
                    break
            if how is None:
                break
            if kind == 'up' and limit is not None and limit < a.min:
                break
            fn = fn_of(ld.address)
            if keep is not None and fn - BASE not in keep:
                break
            if kind == 'down':
                limit = armed(fn, loc[1])
                if limit is None:   # a countdown nobody arms with a constant here: mostly reference counts
                    break
            hits.append((ld.address, fn, kind, loc[1], limit, how))
            break

if a.fns:
    seen = {}
    for x, fn, kind, off, lim, how in hits:
        seen.setdefault(fn, []).append(lim)
    print('# %d functions with a stepped in-memory counter that is compared or tested (min limit %d)' % (len(seen), a.min))
    for fn, lims in sorted(seen.items()):
        print('%#x\t%s' % (fn - BASE, ','.join(str(l) for l in lims)))
else:
    up = sum(1 for h in hits if h[2] == 'up')
    print('# %d counters: %d count up and compare to a limit >= %d, %d count down to zero from a constant >= %d the same function stores' % (len(hits), up, a.min, len(hits) - up, a.min))
    for x, fn, kind, off, lim, how in hits:
        print('%#x\tfn %#x\t%s\t+%#x\tlimit %s\t%s' % (x - BASE, fn - BASE, kind, off, lim, how))
