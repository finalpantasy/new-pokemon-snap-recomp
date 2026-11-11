"""Light AArch64 xref finder for the main ELF (numpy only, no disassembler needed for the scan).

  python -I scripts/elf_xref.py find <bytes-literal>        # rodata hits, then code xrefs (adrp+add)
  python -I scripts/elf_xref.py func <addr> [maxinstr]      # disassemble with capstone
  python -I scripts/elf_xref.py vt <addr> [maxinstr]        # FlatBuffers vtable-offset pattern scan in a function
"""
import sys, os, struct
import numpy as np

ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'analysis', 'import', 'v262144-main.elf')
TEXT_END = 0xa365b0


def load():
    return open(ELF, 'rb').read()


def find_literals(d, lit, lo=0xa36000, hi=0xc61000):
    out = []
    i = d.find(lit, lo)
    while i != -1 and i < hi:
        out.append(i)
        i = d.find(lit, i + 1)
    return out


def xrefs(d, targets):
    """Code addresses of adrp(+add) pairs that materialise any of the target addresses."""
    code = np.frombuffer(d[:TEXT_END], dtype='<u4')
    isadrp = (code & 0x9f000000) == 0x90000000
    idx = np.nonzero(isadrp)[0]
    ins = code[idx].astype(np.int64)
    immlo = (ins >> 29) & 3
    immhi = (ins >> 5) & 0x7ffff
    imm = (immhi << 2) | immlo
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = ((idx.astype(np.int64) * 4) & ~0xfff) + (imm << 12)
    rd = ins & 31
    res = {t: [] for t in targets}
    tset = {}
    for t in targets:
        tset.setdefault(t & ~0xfff, []).append(t)
    for k in range(len(idx)):
        pg = int(page[k])
        if pg not in tset:
            continue
        for j in range(1, 6):
            if idx[k] + j >= len(code):
                break
            nx = int(code[idx[k] + j])
            if (nx & 0xff800000) == 0x91000000 and ((nx >> 5) & 31) == int(rd[k]):
                off = (nx >> 10) & 0xfff
                for t in tset[pg]:
                    if pg + off == t:
                        res[t].append(int(idx[k]) * 4)
    return res


def func_start(d, addr):
    """Walk back to a plausible function entry (stp x29,x30 prologue or sub sp) - heuristic."""
    code = np.frombuffer(d[:TEXT_END], dtype='<u4')
    i = addr // 4
    for j in range(i, max(i - 4000, 0), -1):
        w = int(code[j])
        if (w & 0xffc003e0) == 0xa98003e0 or (w & 0xff0003ff) == 0xd10003ff:  # stp pre-index / sub sp,sp,#
            # include preceding stp sequence
            return j * 4
    return addr


def disasm(d, addr, n):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for ins in md.disasm(d[addr: addr + 4 * n], addr):
        print('%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))


def main():
    d = load()
    cmd = sys.argv[1]
    if cmd == 'find':
        lit = eval(sys.argv[2]) if sys.argv[2].startswith("b'") else sys.argv[2].encode()
        hits = find_literals(d, lit)
        print('rodata hits', [hex(h) for h in hits[:20]])
        r = xrefs(d, hits[:20])
        for t, l in r.items():
            print(hex(t), [hex(x) for x in l])
    elif cmd == 'func':
        disasm(d, int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 120)
    elif cmd == 'start':
        print(hex(func_start(d, int(sys.argv[2], 16))))


if __name__ == '__main__':
    main()
