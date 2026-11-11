"""Scan .text for vtable-pointer stores; shared by classify_vtables.py. Deterministic, no capstone.
Pure word-level decode of adrp/add/ldr/str/stp/bl/b with linear register tracking."""
import struct, sys, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS = os.path.join(ROOT, 'analysis', 'maps')
ELF = os.path.join(ROOT, 'analysis', 'import', 'v262144-main.elf')

def load_image():
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'pylib'))
    from elftools.elf.elffile import ELFFile
    f = ELFFile(open(ELF, 'rb'))
    img = bytearray(0xcc7000 + 0x100000)
    for s in f.iter_segments():
        if s['p_type'] == 'PT_LOAD':
            d = s.data()
            img[s['p_vaddr']:s['p_vaddr'] + len(d)] = d
    return img

def load_tsv(name):
    rows = []; hdr = None
    for l in open(os.path.join(MAPS, name), encoding='utf-8'):
        l = l.rstrip('\n')
        if l.startswith('#') or not l: continue
        p = l.split('\t')
        if hdr is None: hdr = p; continue
        rows.append(dict(zip(hdr, p)))
    return rows

def load_funcs():
    out = []
    for r in load_tsv('functions_final.tsv'):
        if r['kind'] in ('function', 'thunk'):
            out.append((int(r['start'], 16), int(r['end'], 16), r['kind']))
    out.sort()
    return out

def load_vtables():
    vts = []
    for r in load_tsv('vtables_candidate.tsv'):
        s = int(r['start_offset'], 16)
        slots = r['slots'].split(';') if r['slots'] else []
        pw = r['prefix_words'].split(',')
        vts.append(dict(start=s, slots=slots, prefix=pw, nent=int(r['entries'])))
    vts.sort(key=lambda v: v['start'])
    return vts

def load_relocs():
    rel = {}
    for r in load_tsv('relocations.tsv'):
        if r['type'] == 'RELATIVE':
            rel[int(r['offset'], 16)] = int(r['target'], 16)
    return rel

ALLOCATORS = (0x990e70, 0x3f0)
OUT_ALLOCATORS = (0x991c30,)  # aligned allocator, result returned through [x0] out-pointer

def scan(img, funcs, vtmap, rel):
    """Per function: writes[(vt, objreg, off, bl_before, insn_idx, abs_off)], calls[(idx, target, kind, x0_off)],
    strrefs[(idx, rodata addr)].  abs_off = offset from 'this' (x0 at entry, or the fresh object returned by an
    allocator call in functions <= 0x400 bytes) when the base register is this-derived, else None.
    x0_off = same for the first argument of a call."""
    res = {}
    for (s, e, kind) in funcs:
        writes = []; calls = []; strrefs = []
        regs = {}; th = {0: 0}
        small = (e - s) <= 0x400
        sp0 = None; pending = None
        n = (e - s) // 4
        words = struct.unpack_from('<%dI' % n, img, s)
        nbl = 0
        for i, w in enumerate(words):
            a = s + 4 * i
            top = w & 0xff000000
            rd = w & 31
            if (w & 0x9f000000) == 0x90000000:  # adrp
                imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
                if imm & 0x100000: imm -= 0x200000
                regs[rd] = (a & ~0xfff) + (imm << 12); th.pop(rd, None)
            elif top == 0x91000000:  # add xd, xn, #imm
                rn = (w >> 5) & 31
                imm = (w >> 10) & 0xfff
                if (w >> 22) & 1: imm <<= 12
                if rn in regs and rn != 31:
                    v = regs[rn] + imm
                    regs[rd] = v
                    if 0xa39000 <= v < 0xc61000: strrefs.append((i, v))
                else:
                    regs.pop(rd, None)
                if rn in th and rn != 31: th[rd] = th[rn] + imm
                else: th.pop(rd, None)
                if rn == 31 and rd == 0: sp0 = imm
            elif (w & 0xffe0ffe0) == 0xaa0003e0:  # mov xd, xm
                rm = (w >> 16) & 31
                if rm in th: th[rd] = th[rm]
                else: th.pop(rd, None)
                if rm in regs: regs[rd] = regs[rm]
                else: regs.pop(rd, None)
            elif (w & 0xffc00000) == 0xf9400000:  # ldr x unsigned
                rn = (w >> 5) & 31
                th.pop(rd, None)
                if small and rn == 31 and pending is not None and ((w >> 10) & 0xfff) * 8 == pending:
                    th[rd] = 0; pending = None
                if rn in regs:
                    t = rel.get(regs[rn] + ((w >> 10) & 0xfff) * 8)
                    if t is not None: regs[rd] = t
                    else: regs.pop(rd, None)
                else: regs.pop(rd, None)
            elif (w & 0xffc00000) == 0xf9000000:  # str x
                v = regs.get(rd)
                if v is not None and v in vtmap:
                    base = (w >> 5) & 31; off = ((w >> 10) & 0xfff) * 8
                    writes.append((vtmap[v], base, off, nbl, i, (th[base] + off) if base in th else None))
            elif (w & 0xffc00000) == 0xa9000000:  # stp x signed offset
                imm = (w >> 15) & 0x7f
                if imm & 0x40: imm -= 0x80
                base = (w >> 5) & 31
                for k, r in ((0, rd), (1, (w >> 10) & 31)):
                    v = regs.get(r)
                    if v is not None and v in vtmap:
                        off = imm * 8 + 8 * k
                        writes.append((vtmap[v], base, off, nbl, i, (th[base] + off) if base in th else None))
            elif (w >> 26) == 0x25:  # bl
                imm = w & 0x3ffffff
                if imm & 0x2000000: imm -= 0x4000000
                t = a + imm * 4
                calls.append((i, t, 'bl', th.get(0))); nbl += 1
                for r in range(19): regs.pop(r, None); th.pop(r, None)
                if small and t in ALLOCATORS: th[0] = 0
                if small and t in OUT_ALLOCATORS: pending = sp0
            elif (w >> 26) == 0x5:  # b
                imm = w & 0x3ffffff
                if imm & 0x2000000: imm -= 0x4000000
                t = a + imm * 4
                if not (s <= t < e): calls.append((i, t, 'b', th.get(0)))
            elif (w & 0xfffffc1f) == 0xd63f0000:  # blr
                for r in range(19): regs.pop(r, None); th.pop(r, None)
            elif top in (0x54000000, 0xb4000000, 0xb5000000, 0x34000000, 0x35000000, 0x36000000, 0x37000000, 0xb6000000, 0xb7000000, 0x14000000):
                pass
            elif ((w >> 25) & 5) == 4:  # load/store class
                if (w >> 22) & 1:
                    regs.pop(rd, None); th.pop(rd, None)
                    if (w >> 27) & 7 == 5:
                        r2 = (w >> 10) & 31; regs.pop(r2, None); th.pop(r2, None)
            else:
                regs.pop(rd, None); th.pop(rd, None)
        res[s] = dict(end=e, writes=writes, calls=calls, strrefs=strrefs)
    return res
