#!/usr/bin/env python3
"""Compare two translator output dirs (scripts/xlate_run.ps1) program by program and run spirv-val on both.
  python -I scripts/xlate_compare.py BASE_DIR BASE_SALT NEW_DIR NEW_SALT [spirv-val.exe]
Prints how many SPIR-V outputs are byte-identical/changed, per-stage change counts, validation counts, and writes changed.tsv next to NEW_DIR."""
import hashlib, os, subprocess, sys, collections
corp = os.path.join("build", "xlate", "corpus")
bd, bs, nd, ns = sys.argv[1:5]
val = sys.argv[5] if len(sys.argv) > 5 else None
def key(data, salt, st, co): return hashlib.sha256(data + salt.encode() + b"|" + str(st).encode() + b"|" + str(co).encode()).hexdigest()
same = chg = 0; bystage = collections.Counter(); tot = collections.Counter(); rows = []; vb = vn = 0; vfail = []
stn = {0: "VS", 1: "PS", 2: "GS", 5: "CS"}
for line in open(os.path.join(corp, "list.tsv")):
    h, st, co, cd, name = line.rstrip("\n").split("\t"); st = int(st)
    data = open(os.path.join(corp, "bins", h + ".bin"), "rb").read()
    kb, kn = key(data, bs, st, cd), key(data, ns, st, cd)
    a = open(os.path.join(bd, kb + ".spv"), "rb").read(); b = open(os.path.join(nd, kn + ".spv"), "rb").read()
    tot[stn[st]] += 1
    jb = open(os.path.join(bd, kb + ".json"), "rb").read(); jn = open(os.path.join(nd, kn + ".json"), "rb").read()
    if a == b and jb == jn: same += 1
    else: chg += 1; bystage[stn[st]] += 1; rows.append((name, stn[st], kb, kn, len(a), len(b), jb != jn))
    if val:
        for d, k, c in ((bd, kb, "b"), (nd, kn, "n")):
            r = subprocess.run([val, "--target-env", "vulkan1.3", os.path.join(d, k + ".spv")], capture_output=True, text=True)
            if r.returncode == 0:
                if c == "b": vb += 1
                else: vn += 1
            elif c == "n": vfail.append((name, r.stderr.strip()[:200]))
print("programs", same + chg, "identical", same, "changed", chg, "by stage", dict(bystage), "of", dict(tot))
if val: print("spirv-val ok: base", vb, "new", vn, "new failures", vfail[:5])
with open(os.path.join(os.path.dirname(os.path.normpath(nd)), "changed.tsv"), "w") as f:
    for r in rows: f.write("\t".join(map(str, r)) + "\n")
