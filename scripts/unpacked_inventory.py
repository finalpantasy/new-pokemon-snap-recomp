#!/usr/bin/env python3
"""Inventory of unpacked DRP entries (reads extracted/drp-v262144, prints stats; no game bytes embedded).
Run: python -I scripts/unpacked_inventory.py [nushdb|luo|registry|dc4a|magic]
"""
import collections, re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "extracted" / "drp-v262144"
REG_EXT = ["mdfb", "genderb", "pdcd", "silb", "ecbd", "mdcd", "navb", "rtd", "path", "paramb", "pcnb", "misd",
           "achd", "mdrp", "nvhb", "ldtb", "picd", "pstb", "pfrb", "pflb"]


def files(ext, sub=""):
    return sorted((OUT / sub).rglob("*." + ext)) if (OUT / sub).exists() else []


def nushdb():
    fs = files("nushdb")
    print("nushdb files", len(fs))
    hdr = collections.Counter()
    cnt = collections.Counter()
    pats = collections.Counter()
    stages = collections.Counter()
    sizes = []
    nop = b"\x00\x0f\x07\x00\x00\x00\xb0\x50"        # Maxwell NOP
    for p in fs:
        d = p.read_bytes()
        sizes.append(len(d))
        hdr[(d[:4], struct.unpack_from("<I", d, 4)[0], d[0x10:0x14], d[0x14:0x18].hex())] += 1
        n = struct.unpack_from("<Q", d, 0x20)[0]
        cnt[n] += 1
        for nm, pat in (("!!NV", b"!!NV"), ("#version", b"#version"), ("spirv", b"\x03\x02\x23\x07"),
                        ("GLSLC", b"GLSL"), ("NVN", b"NVN"), ("maxwell_nop", nop)):
            if pat in d:
                pats[nm] += 1
        for m in re.finditer(rb"[A-Za-z0-9_]{4,}(VS|PS|GS|HS|DS|CS)\x00", d):
            stages[m.group(1).decode()] += 1
    print("header (magic,u32@4,type,ver):", dict(hdr))
    print("u64@0x20 (program count?) histogram:", dict(cnt))
    print("pattern presence:", dict(pats))
    print("stage-named programs:", dict(stages))
    print("size min/max/sum", min(sizes), max(sizes), sum(sizes))


def luo():
    fs = files("luo")
    print("luo files", len(fs))
    hdr = collections.Counter()
    srcs = collections.Counter()
    for p in fs:
        d = p.read_bytes()
        h = d[:12]
        # sig, version, format, endian, sizeof int/size_t/instr/number, integral
        # then main function: source name (size_t len + bytes), linedefined, lastlinedefined, nups, nparams, vararg, maxstack
        hdr[h.hex()] += 1
        slen = struct.unpack_from("<Q", d, 12)[0]
        srcs[slen if slen < 5 else "n>4"] += 1
    print("12-byte header variants:", dict(hdr))
    print("main-chunk source name length:", dict(srcs))


def magic4():
    c = collections.Counter()
    for p in OUT.rglob("*"):
        if p.is_file() and p.suffix:
            with open(p, "rb") as f:
                c[(p.suffix.lstrip("."), f.read(4).hex())] += 1
    for (e, m), n in sorted(c.items(), key=lambda kv: (kv[0][0], -kv[1])):
        print(e, m, n)


def fb_root(d):
    """Minimal FlatBuffers table probe: returns (identifier or '', root_pos, [(field_idx, raw u32 or None)])."""
    if len(d) < 8:
        return None
    root = struct.unpack_from("<I", d, 0)[0]
    ident = d[4:8].decode("ascii") if re.fullmatch(rb"[A-Z]{4}", d[4:8]) else ""
    if root + 4 > len(d):
        return None
    so = struct.unpack_from("<i", d, root)[0]
    vt = root - so
    if vt < 0 or vt + 4 > len(d):
        return None
    vsz, tsz = struct.unpack_from("<HH", d, vt)
    nf = (vsz - 4) // 2
    fields = []
    for i in range(nf):
        off = struct.unpack_from("<H", d, vt + 4 + 2 * i)[0]
        fields.append((i, off))
    return ident, root, vsz, tsz, fields


def registry():
    for ext in REG_EXT:
        for p in files(ext):
            d = p.read_bytes()
            r = fb_root(d)
            strs = len(re.findall(rb"[ -~]{6,}", d))
            if r:
                ident, root, vsz, tsz, fields = r
                present = [i for i, o in fields if o]
                desc = "fb ident=%r root=0x%x vtable_fields=%d present=%d tablesz=%d" % (ident, root, len(fields), len(present), tsz)
                # vector fields: uoffset -> len
                vecs = []
                for i, o in fields:
                    if o and root + o + 4 <= len(d):
                        tgt = root + o + struct.unpack_from("<I", d, root + o)[0]
                        if 0 < tgt < len(d) - 4:
                            n = struct.unpack_from("<I", d, tgt)[0]
                            if 0 < n < 100000 and tgt + 4 + n <= len(d):
                                vecs.append("f%d:%d" % (i, n))
                desc += " vec/str=" + ",".join(vecs[:8])
            else:
                desc = "not-fb"
            print("%s %d B head=%s ascii_runs=%d | %s" % (p.name, len(d), d[:16].hex(), strs, desc))


def dc4a():
    fs = [p for p in OUT.rglob("*") if p.is_file() and p.suffix.upper() == ".DC4A8177"]
    print("DC4A8177 files", len(fs))
    c = collections.Counter(p.read_bytes()[:8].hex() for p in fs)
    print("first8 variants:", c.most_common(6))


if __name__ == "__main__":
    for a in (sys.argv[1:] or ["nushdb", "luo", "registry", "dc4a"]):
        print("==", a)
        globals()[a]()
