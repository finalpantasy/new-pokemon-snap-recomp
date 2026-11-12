#!/usr/bin/env python3
"""Count FFMA/FMUL/FMUL32I/FFMA32I instructions whose FMZ field is set (Ryujinx ignores it) over build/xlate/corpus.
  python -I scripts/xlate_fmz_census.py build/xlate/corpus"""
import os, struct, sys
def pat(s): 
    m = sum(1 << (63 - i) for i, c in enumerate(s) if c in "01"); v = sum(1 << (63 - i) for i, c in enumerate(s) if c == "1"); return m, v
P = {  # name: (pattern, fmz shift)
 "FFMA_R": ("010110011" + "x" * 55, 53), "FFMA_I": ("0011001x1" + "x" * 55, 53), "FFMA_C": ("010010011" + "x" * 55, 53),
 "FFMA_RC": ("010100011" + "x" * 55, 53), "FFMA32I": ("000011" + "x" * 58, 53),
 "FMUL_R": ("0101110001101" + "x" * 51, 44), "FMUL_I": ("0011100x01101" + "x" * 51, 44), "FMUL_C": ("0100110001101" + "x" * 51, 44),
 "FMUL32I": ("00011110" + "x" * 56, 53)}
P = {k: (pat(v[0]), v[1]) for k, v in P.items()}
root = sys.argv[1]
tot = {k: [0, 0, 0] for k in P}   # instr, fmz=2, ftz=1 (programs counted separately)
progs_fmz = set(); n = 0
for line in open(os.path.join(root, "list.tsv")):
    h, stage, const_off, code_off, name = line.rstrip("\n").split("\t")
    d = open(os.path.join(root, "bins", h + ".bin"), "rb").read()
    start = int(code_off) + (0 if stage == "5" else 0x50); end = int(const_off) & ~7
    n += 1
    for i, o in enumerate(range(start, end, 8)):
        if i % 4 == 0: continue            # scheduling word
        w = struct.unpack_from("<Q", d, o)[0]
        for k, ((m, v), sh) in P.items():
            if w & m == v:
                tot[k][0] += 1; f = (w >> sh) & 3
                if f == 2: tot[k][1] += 1; progs_fmz.add(h)
                if f == 1: tot[k][2] += 1
                break
print("programs", n)
for k, t in tot.items(): print("%-8s instr %7d  FMZ %6d  FTZ %6d" % (k, *t))
print("programs with FMZ:", len(progs_fmz))
