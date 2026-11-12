#!/usr/bin/env python3
"""Extract every unique shader program of the game as the NVN layer sees it (data section, stage, constOff, codeOff).

Output (under build/, never committed): OUT/bins/<sha256(data)>.bin and OUT/list.tsv (bin<TAB>stage<TAB>constOff<TAB>codeOff<TAB>name),
the input of `ShaderHarness --nvn-batch`. Stage numbering is the harness's (0 VS, 1 PS, 2 GS, 3 TCS, 4 TES, 5 CS).

  python -I scripts/xlate_corpus.py ROOT OUT      (ROOT = extracted/drp-v262144)
"""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I removes the script dir; add it back for the sibling parser
import nushdb_parse as P

SPH_TO_STAGE = {1: 0, 2: 3, 3: 4, 4: 2, 5: 1}

def main(root, out):
    os.makedirs(os.path.join(out, "bins"), exist_ok=True)
    seen, rows = {}, []
    for dp, dn, fn in os.walk(root):
        for f in sorted(fn):
            if not f.endswith(".nushdb"):
                continue
            for pr in P.parse_file(os.path.join(dp, f)):
                b, bl = pr["binary"], pr["blob"]
                ctrl, d = bl["ctrl_off"], bl["data_off"]
                data = b[d:d + bl["data_size"]]
                const_off = struct.unpack_from("<I", b, ctrl + 0x700)[0]
                hdr_size = struct.unpack_from("<I", b, ctrl + 0x708)[0]
                compute = struct.unpack_from("<I", data, 0)[0] != 0x12345678
                if compute:
                    stage, code_off = 5, 0
                else:
                    code_off = hdr_size
                    stage = SPH_TO_STAGE[(struct.unpack_from("<I", data, code_off)[0] >> 10) & 15]
                h = hashlib.sha256(data).hexdigest()
                k = (h, stage, code_off)
                if k in seen:
                    continue
                seen[k] = 1
                with open(os.path.join(out, "bins", h + ".bin"), "wb") as fh:
                    fh.write(data)
                rows.append((h, stage, const_off, code_off, pr["name"]))
    with open(os.path.join(out, "list.tsv"), "w") as fh:
        for h, s, c, o, n in rows:
            fh.write("%s\t%d\t%d\t%d\t%s\n" % (h, s, c, o, n))
    print("unique programs", len(rows))

main(sys.argv[1], sys.argv[2])
