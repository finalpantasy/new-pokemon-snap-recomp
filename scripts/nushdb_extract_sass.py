#!/usr/bin/env python3
"""Extract SPH+SASS (what Ryujinx's translator consumes) for sample programs from census.tsv.
Run: python -I nushdb_extract_sass.py CENSUS.tsv DRP_ROOT OUT_DIR N
Writes OUT_DIR/<i>_<stage>_<name>.bin and OUT_DIR/index.tsv. Picks a spread of stages/sizes/features."""
import csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nushdb_parse as n

def main(census, root, out, count):
    R = list(csv.DictReader(open(census), delimiter="\t"))
    seen, pick = set(), []
    def add(r):
        k = (r["stage"], r["code_sha"])
        if k in seen or len(pick) >= count: return
        seen.add(k); pick.append(r)
    for st in ("CS", "GS"):
        for r in R:
            if r["stage"] == st: add(r)
    feats = [lambda r: r["kills"] == "1", lambda r: int(r["ssbo"]) > 0 and r["stage"] == "PS",
             lambda r: int(r["sampler"]) >= 4, lambda r: int(r["insn"]) > 1500, lambda r: int(r["insn"]) < 30,
             lambda r: r["gp5"] == "1" and r["stage"] == "PS", lambda r: r["gp5"] == "1" and r["stage"] == "VS"]
    for f in feats:
        for st in ("VS", "PS"):
            c = [r for r in R if r["stage"] == st and f(r)]
            for r in c[:: max(1, len(c) // 2)][:2]: add(r)
    i = 0
    while len(pick) < count and i < len(R):
        add(R[i]); i += 97
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "index.tsv"), "w") as ix:
        for j, r in enumerate(pick):
            progs = n.parse_file(os.path.join(root, r["file"]))
            p = progs[int(r["idx"])]; b = p["blob"]["binary"] if False else p["binary"]; bl = p["blob"]
            d = bl["data_off"]; s = 0 if bl["nvn_stage"] == 5 else n.DATA_HDR
            raw = b[d + s: d + bl["const_off"]]  # include NOP padding to the constant-data start (decoder reads ahead)
            fn = "%02d_%s_%s.bin" % (j, r["stage"], r["name"][-40:])
            open(os.path.join(out, fn), "wb").write(raw)
            ix.write("%s\t%s\t%s\t%s\t%s\n" % (fn, r["stage"], r["insn"], r["sampler"], r["file"]))
    print(len(pick), "samples")
main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]))
