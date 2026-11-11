#!/usr/bin/env python3
"""Parser for nushdb (SSBH SHDR v1.2 + NVN GLSLC program binary) files.

Layout sources:
  - SSBH SHDR v1.2 outer container: ssbh_lib (MIT, Smash Ultimate Research Group),
    ssbh_lib/src/formats/shdr.rs  (name, stage, unk=2, binary offset/size, binary_size, 16 pad).
  - The inner binary (GLSLC output) is decoded here empirically from the game's own files.
  - Maxwell shader program header (SPH) bit layout follows Ryujinx.Graphics.Shader
    Translation/ShaderHeader.cs (MIT; Ryubing fork).

Run game-data readers with `python -I`.

Usage:
  python -I nushdb_parse.py FILE [FILE ...]            # pretty-print programs
  python -I nushdb_parse.py --census ROOT OUT.tsv      # census over every *.nushdb under ROOT
"""
import hashlib
import os
import struct
import sys

SSBH_STAGE = {0: "VS", 3: "GS", 4: "PS", 5: "CS"}
NVN_STAGE = {0: "VS", 1: "PS", 2: "GS", 3: "TCS", 4: "TES", 5: "CS"}
SPH_STAGE = {0: "CS", 1: "VS", 2: "TCS", 3: "TES", 4: "GS", 5: "PS"}
SECTION_TYPE = {0: "gpucode", 1: "source", 2: "info108", 3: "reflection", 4: "debug"}
BLOB_MAGIC = 0x19866891
CTRL_MAGIC = 0x198761234
DATA_MAGIC = 0x12345678
REFL_MAGIC = 0x19292919
NOP = 0x50B0000000070F00
SPH_SIZE = 0x50
DATA_HDR = 0x30  # marker/header words before the SPH inside the data section
CODE_OFF = DATA_HDR + SPH_SIZE  # 0x80: first scheduling word of SASS inside the data section

# Reflection table order (count, offset) pairs, 7 of them, then string table size/offset.
REFL_TABLES = ["ubo", "uniform", "input", "output", "ssbo", "ssbo_member", "unknown7"]
REFL_STRIDE = {"ubo": 0x6C, "uniform": 0xA4, "input": 0x5C, "output": 0x5C, "ssbo": 0x6C, "ssbo_member": 0x70}


class ParseError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def s32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def parse_ssbh(data):
    """Outer SSBH/SHDR container -> list of dicts (name, stage, binary bytes)."""
    if data[:4] != b"HBSS" or data[0x10:0x14] != b"RDHS":
        raise ParseError("not an SSBH SHDR file")
    major, minor = struct.unpack_from("<HH", data, 0x14)
    if (major, minor) != (1, 2):
        raise ParseError("SHDR version %d.%d" % (major, minor))
    # SsbhArray at 0x18: relative offset (from its own position) then count.
    rel, count = struct.unpack_from("<QQ", data, 0x18)
    base = 0x18 + rel
    out = []
    for i in range(count):
        e = base + i * 0x38
        name_rel, stage, unk, bin_rel, bin_size, bin_size2 = struct.unpack_from("<QIIQQQ", data, e)
        n0 = e + name_rel
        name = data[n0:data.index(b"\0", n0)].decode("ascii", "replace")
        b0 = e + 0x10 + bin_rel
        if b0 + bin_size > len(data):
            raise ParseError("binary out of range")
        out.append(dict(name=name, ssbh_stage=stage, unk=unk, binary=data[b0:b0 + bin_size],
                        binary_size=bin_size2, bin_off=b0))
    return out


def parse_blob(b):
    """GLSLC program binary. Returns dict."""
    if u32(b, 0) != BLOB_MAGIC:
        raise ParseError("bad blob magic")
    r = dict(size=len(b))
    r["glslc_ver"] = struct.unpack_from("<HH", b, 8)  # (0x1d/0x1f, 0x22)
    r["hdr_words"] = struct.unpack_from("<7I", b, 0x0C)  # 4, 0x11, 0x15, 1, 0x10, 0x51 ...
    r["total"], r["hdr_size"], r["nsec"] = struct.unpack_from("<III", b, 0x44)
    if r["total"] != len(b):
        raise ParseError("blob total size mismatch")
    secs = []
    for k in range(r["nsec"]):
        o = 0x90 * (k + 1)
        size, off, ty = struct.unpack_from("<III", b, o)
        if off + size > len(b):
            raise ParseError("section out of range")
        secs.append(dict(hdr=o, size=size, off=off, type=ty))
    r["sections"] = secs
    by = {}
    for s in secs:
        by.setdefault(s["type"], s)
    # gpu code section: control then data
    g = by.get(0)
    if g is None:
        raise ParseError("no gpucode section")
    o = g["hdr"]
    nvn_stage = u32(b, o + 0x2C)
    ctrl_size, data_size, data_off = struct.unpack_from("<III", b, o + 0x34)
    r.update(nvn_stage=nvn_stage, ctrl_off=g["off"], ctrl_size=ctrl_size, data_off=g["off"] + data_off,
             data_size=data_size)
    if ctrl_size + data_size != g["size"]:
        raise ParseError("control+data != section size")
    c = g["off"]
    r["ctrl_magic"] = struct.unpack_from("<Q", b, c)[0]
    # control section tail words (found at ctrl+0x6F0 in all shaders checked)
    r["prog_size"] = u32(b, c + 0x6F8)    # SPH-relative end of code within the data section
    r["ctrl_unk6fc"] = u32(b, c + 0x6FC)  # 0x14 (VS) / varies (PS); unknown
    r["const_off"] = u32(b, c + 0x700)    # constant data (immediates) start inside data section
    d = r["data_off"]
    r["data_marker"] = u32(b, d)
    is_cs = nvn_stage == 5  # compute programs have no SPH and no data header: code starts at data+0
    code_rel = 0 if is_cs else CODE_OFF
    # SPH
    w = struct.unpack_from("<20I", b, d + DATA_HDR) if not is_cs else (0,) * 20
    r["sph_words"] = w
    r["sph_type"] = w[0] & 31
    r["sph_ver"] = (w[0] >> 5) & 31
    r["sph_stage"] = SPH_STAGE.get((w[0] >> 10) & 15, "?")
    r["mrt"] = (w[0] >> 14) & 1
    r["kills"] = (w[0] >> 15) & 1
    r["global_store"] = (w[0] >> 16) & 1
    r["sass_ver"] = (w[0] >> 17) & 15
    r["gp_passthrough"] = (w[0] >> 24) & 1
    r["load_store"] = (w[0] >> 26) & 1
    r["fp64"] = (w[0] >> 27) & 1
    r["local_mem_lo"] = w[1] & 0xFFFFFF
    r["local_mem_hi"] = w[2] & 0xFFFFFF
    r["local_mem_crs"] = w[3] & 0xFFFFFF
    r["omap_targets"] = w[18]
    r["omap_depth"] = (w[19] >> 1) & 1
    # SASS
    code0 = d + code_rel
    code_end = d + r["prog_size"]
    if not (code0 <= code_end <= d + data_size):
        raise ParseError("prog_size out of range")
    code = b[code0:code_end]
    slots = len(code) // 8
    words = struct.unpack_from("<%dQ" % slots, code)
    sched = (slots + 3) // 4
    nops = sum(1 for i, x in enumerate(words) if i % 4 != 0 and x == NOP)
    r.update(code_off=code0, code_len=len(code), slots=slots, insn=slots - sched, nops=nops,
             code_sha=hashlib.sha256(b[d + (0 if is_cs else DATA_HDR):code_end]).hexdigest(),
             consts_len=data_size - r["const_off"])
    # reflection
    rf = by.get(3)
    r["has_source"] = 1 in by
    r["has_debug"] = 4 in by
    r["refl"] = parse_reflection(b, rf) if rf else None
    r["has_glsl"] = b.find(b"#version") >= 0
    r["has_gp5"] = b.find(b"!!NV") >= 0
    return r


def parse_reflection(b, s):
    o = s["hdr"]
    base = s["off"]
    w = struct.unpack_from("<17I", b, o + 0x2C)
    tabs = {}
    for i, name in enumerate(REFL_TABLES):
        tabs[name] = (w[2 * i], w[2 * i + 1])
    str_size, str_off = w[14], w[15]
    if u32(b, base) != REFL_MAGIC:
        raise ParseError("bad reflection magic")
    sb = base + str_off

    def name(off, ln):
        p = sb + off
        return b[p:b.index(b"\0", p)].decode("ascii", "replace") if ln else ""

    out = dict(tables=tabs, str_size=str_size)
    for t in ("ubo", "uniform", "input", "output", "ssbo", "ssbo_member"):
        cnt, off = tabs[t]
        st = REFL_STRIDE[t]
        recs = []
        for i in range(cnt):
            p = base + off + i * st
            if p + st > len(b):
                raise ParseError("reflection record out of range")
            r = struct.unpack_from("<%di" % (st // 4), b, p)
            recs.append(dict(name=name(r[0], r[1]), raw=r))
        out[t] = recs
    # derived views
    ubos = []
    for x in out["ubo"]:
        r = x["raw"]
        ubos.append(dict(name=x["name"], size=r[10], members=r[11], stage_mask=r[12],
                         binding_per_stage=r[13:19]))
    out["ubos"] = ubos
    uni, smp = [], []
    for x in out["uniform"]:
        r = x["raw"]
        e = dict(name=x["name"], type=r[10], block=r[11], offset=r[12], count=r[13], stage_mask=r[17],
                 binding_per_stage=r[18:24], is_opaque=r[24])
        (smp if r[24] == 1 else uni).append(e)
    out["uniforms"], out["samplers"] = uni, smp
    out["inputs"] = [dict(name=x["name"], type=x["raw"][10], location=x["raw"][12]) for x in out["input"]]
    out["outputs"] = [dict(name=x["name"], type=x["raw"][10], location=x["raw"][12]) for x in out["output"]]
    return out


def parse_file(path):
    with open(path, "rb") as f:
        data = f.read()
    progs = parse_ssbh(data)
    for p in progs:
        p["blob"] = parse_blob(p["binary"])
        p["blob_sha"] = hashlib.sha256(p["binary"]).hexdigest()
    return progs


def show(path):
    for p in parse_file(path):
        bl = p["blob"]
        print("%s  ssbh_stage=%s sph_stage=%s blob=0x%x" % (p["name"], SSBH_STAGE.get(p["ssbh_stage"], p["ssbh_stage"]),
                                                          bl["sph_stage"], bl["size"]))
        print("  sections:", [(SECTION_TYPE.get(s["type"], s["type"]), hex(s["off"]), hex(s["size"])) for s in bl["sections"]])
        print("  ctrl=0x%x data=0x%x prog_size=0x%x consts=0x%x slots=%d insn=%d nops=%d" % (
            bl["ctrl_size"], bl["data_size"], bl["prog_size"], bl["consts_len"], bl["slots"], bl["insn"], bl["nops"]))
        rf = bl["refl"]
        if rf:
            print("  ubo:", [(u["name"], u["size"], u["binding_per_stage"][bl["nvn_stage"]]) for u in rf["ubos"]])
            print("  samplers:", [(s["name"], s["binding_per_stage"][bl["nvn_stage"]]) for s in rf["samplers"]])
            print("  in:", [x["name"] for x in rf["inputs"]], "out:", [x["name"] for x in rf["outputs"]])


COLS = ["file", "idx", "name", "stage", "blob_bytes", "ctrl", "data", "sass_bytes", "slots", "insn", "nops", "consts",
        "sph_type", "sph_ver", "kills", "gstore", "local_lo", "ubo", "uniform", "sampler", "input", "output", "ssbo",
        "glsl", "gp5", "code_sha", "blob_sha"]


def census(root, out):
    rows, errs = [], []
    for dp, dn, fn in os.walk(root):
        for f in sorted(fn):
            if not f.endswith(".nushdb"):
                continue
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root).replace("\\", "/")
            try:
                progs = parse_file(p)
            except Exception as e:  # noqa
                errs.append((rel, repr(e)))
                continue
            for i, pr in enumerate(progs):
                b = pr["blob"]
                rf = b["refl"] or {}
                rows.append([rel, i, pr["name"], b["sph_stage"], b["size"], b["ctrl_size"], b["data_size"],
                             b["code_len"], b["slots"], b["insn"], b["nops"], b["consts_len"], b["sph_type"],
                             b["sph_ver"], b["kills"], b["global_store"], b["local_mem_lo"],
                             len(rf.get("ubos", [])), len(rf.get("uniforms", [])), len(rf.get("samplers", [])),
                             len(rf.get("inputs", [])), len(rf.get("outputs", [])), len(rf.get("ssbo", [])),
                             int(b["has_glsl"]), int(b["has_gp5"]), b["code_sha"], pr["blob_sha"]])
    with open(out, "w", newline="") as fh:
        fh.write("\t".join(COLS) + "\n")
        for r in rows:
            fh.write("\t".join(str(x) for x in r) + "\n")
    print("programs", len(rows), "parse errors", len(errs))
    for e in errs[:20]:
        print("ERR", e)


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "--census":
        census(a[1], a[2])
    else:
        for f in a:
            show(f)
