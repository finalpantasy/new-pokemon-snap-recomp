#!/usr/bin/env python3
"""Deterministic, re-runnable audit of the five New Pokemon Snap revisions.

Run:  python -I scripts/audit_revisions.py [--keys <prod.keys>]

Inputs (read-only): the XCI and four update NSPs plus the keyset, all passed to
NSTool by path (key contents are never read or printed here).
Outputs (ignored dirs only):
  extracted/<rev>/...            tik, cnmt NCA, program/control NCA, exefs, *.uncompressed.nso
  analysis/revisions/summary.json, summary.md, romfs-<rev>.txt, symbols-<rev>.json, rtti-<rev>.txt
No game data is embedded in this script.  Pure stdlib (works under python -I).
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(os.environ.get("SNAP_DUMPS_DIR", str(ROOT / "dumps")))  # folder with the XCI/NSP files and prod.keys
NSTOOL = ROOT / "tools" / "nstool" / "nstool.exe"
OUT = ROOT / "analysis" / "revisions"
EXT = ROOT / "extracted"
BELOW_NORMAL = 0x00004000 if os.name == "nt" else 0

REVS = [
    ("v0", "n-pokemonsnap.xci"),
    ("v65536", "n-pokemonsnap_v65536.nsp"),
    ("v131072", "n_pokemon_snap_v131072.nsp"),
    ("v196608", "n-pokemon_snap_v196608.nsp"),
    ("v262144", "n-new_pokemon_snap_v262144.nsp"),
]


def load_pfs0():
    spec = importlib.util.spec_from_file_location("pfs0", ROOT / "scripts" / "pfs0.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pfs0 = load_pfs0()


def run(args, **kw):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", creationflags=BELOW_NORMAL, **kw)
    return r.returncode, r.stdout + r.stderr


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(8 * 1024 * 1024)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


# --------------------------------------------------------------------------- acquisition

def parse_cnmt_text(text):
    info = {"contents": []}
    m = re.search(r"Version:\s+\S+ \(v(\d+)\)", text)
    info["version"] = int(m.group(1)) if m else None
    m = re.search(r"TitleId:\s+(0x[0-9a-f]+)", text)
    info["title_id"] = m.group(1) if m else None
    m = re.search(r"Type:\s+(\w+) \(\d+\)\n\s+Attributes", text)
    info["meta_type"] = m.group(1) if m else None
    for m in re.finditer(r"Type:\s+(\w+) \(\d+\)\s+Id:\s+([0-9a-f]{32})\s+Size:\s+(0x[0-9a-f]+)\s+Hash:\s+([0-9a-f]{64})", text):
        info["contents"].append({"type": m.group(1), "id": m.group(2),
                                 "size": int(m.group(3), 16), "sha256": m.group(4)})
    return info


def acquire(rev, srcname, keys):
    d = EXT / rev
    d.mkdir(parents=True, exist_ok=True)
    src = SRC / srcname
    meta = {"rev": rev, "source": srcname, "source_size": src.stat().st_size}
    entries = {}
    if srcname.endswith(".xci"):
        sec = d / "secure"
        if not (sec.exists() and any(sec.glob("*.cnmt.nca"))):
            sec.mkdir(exist_ok=True)
            rc, out = run([NSTOOL, "-k", keys, "--secure", sec, src])
            if rc:
                raise RuntimeError("xci secure extract failed: " + out[-500:])
        cnmt_nca = next(sec.glob("*.cnmt.nca"))
        store = sec
        tik = None
    else:
        entries = {e["name"]: e for e in pfs0.inventory(src)}
        names = list(entries)
        cnmt_name = next(n for n in names if n.endswith(".cnmt.nca"))
        tik_name = next(n for n in names if n.endswith(".tik"))
        for n in (cnmt_name, tik_name):
            t = d / n
            if not t.exists():
                pfs0.extract(src, entries[n], t, False)
        cnmt_nca = d / cnmt_name
        tik = d / tik_name
        store = d
    tmp = Path(tempfile.mkdtemp(prefix="cnmt_"))
    try:
        run([NSTOOL, "-k", keys, "--part0", tmp, cnmt_nca])
        cnmt_file = next(tmp.glob("*.cnmt"))
        rc, text = run([NSTOOL, "-k", keys, "-t", "cnmt", cnmt_file])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    (d / "cnmt.txt").write_text(text, encoding="utf-8")
    cnmt = parse_cnmt_text(text)
    meta["cnmt"] = cnmt
    for c in cnmt["contents"]:
        if c["type"] in ("Program", "Control"):
            nca = store / (c["id"] + ".nca")
            if not nca.exists() and entries:
                pfs0.extract(src, entries[c["id"] + ".nca"], nca, False)
            c["path"] = str(nca)
            c["hash_matches_cnmt"] = nca.stat().st_size == c["size"] and sha256_file(nca) == c["sha256"]
    prog_path = next(c["path"] for c in cnmt["contents"] if c["type"] == "Program")
    rc, hdrtxt = run([NSTOOL, "-k", keys] + (["--tik", tik] if tik else []) + [prog_path])
    for key, label in (("sdk_addon_version", r"SdkAddon Ver\.:\s+(.*)"), ("nca_dist_type", r"Dist\. Type:\s+(.*)"),
                       ("nca_key_generation", r"Key Generation:\s+(.*)")):
        mm = re.search(label, hdrtxt)
        meta[key] = mm.group(1).strip() if mm else None
    meta["tik"] = str(tik) if tik else None
    meta["cnmt_nca"] = str(cnmt_nca)
    if entries:
        meta["nsp_members"] = [{"name": n, "size": entries[n]["size"]} for n in entries]
        meta["nsp_content_nca_sizes_total"] = sum(e["size"] for n, e in entries.items() if n.endswith(".nca"))
    return meta


def nca_args(keys, meta):
    a = ["-k", keys]
    if meta["tik"]:
        a += ["--tik", meta["tik"]]
    return a


def content(meta, typ):
    return next(c for c in meta["cnmt"]["contents"] if c["type"] == typ)


def extract_exefs(rev, meta, keys):
    d = EXT / rev / "exefs"
    prog = content(meta, "Program")["path"]
    if not (d / "main").exists():
        d.mkdir(parents=True, exist_ok=True)
        rc, out = run([NSTOOL] + nca_args(keys, meta) + ["--part0", d, prog])
        if not (d / "main").exists():
            raise RuntimeError("exefs extract failed: " + out[-500:])
    return d


def extract_nacp(rev, meta, keys):
    d = EXT / rev / "control"
    d.mkdir(parents=True, exist_ok=True)
    nacp = d / "control.nacp"
    if not nacp.exists():
        ctl = content(meta, "Control")["path"]
        run([NSTOOL] + nca_args(keys, meta) + ["-x", "/control.nacp", nacp, ctl])
        if not nacp.exists():
            tmp = d / "part0"
            run([NSTOOL] + nca_args(keys, meta) + ["--part0", tmp, ctl])
            if (tmp / "control.nacp").exists():
                (tmp / "control.nacp").replace(nacp)
    data = nacp.read_bytes()
    ver = data[0x3060:0x3070].split(b"\0")[0].decode("ascii", "replace")
    name = data[0:0x200].split(b"\0")[0].decode("utf-8", "replace")
    return {"display_version": ver, "title_en": name, "nacp_sha256": hashlib.sha256(data).hexdigest()}


# --------------------------------------------------------------------------- RomFS listings

def romfs_listing(meta, keys, base_nca):
    """Names only; sizes are not exposed by `nstool --fstree`. Returns paths relative to the
    NCA root, e.g. '1/common/...' (partition 1 = RomFS)."""
    prog = content(meta, "Program")["path"]
    args = [NSTOOL] + nca_args(keys, meta)
    if meta["rev"] != "v0":
        args += ["--basenca", base_nca]
    rc, out = run(args + ["--fstree", prog])
    lines = out.splitlines()
    try:
        i = next(k for k, l in enumerate(lines) if "ContentArchive/Tree" in l)
    except StopIteration:
        return None, out[-300:]
    files, stack = [], []
    for l in lines[i + 1:]:
        if not l.strip():
            continue
        depth = len(l) - len(l.lstrip(" "))
        name = l.strip()
        stack = stack[:depth - 1]
        if name.endswith("/"):
            stack.append(name.rstrip("/"))
        else:
            files.append("/".join(stack[1:] + [name]))
    return files, None


def by_dir(paths):
    d = {}
    for f in paths:
        p = f.split("/")
        k = "/".join(p[:3]) if len(p) > 3 else "/".join(p[:-1])
        d[k] = d.get(k, 0) + 1
    return dict(sorted(d.items()))


# --------------------------------------------------------------------------- NSO handling

def lz4_block(src, size):
    """Pure-Python LZ4 block decompressor (no third-party deps, works under python -I)."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]
        i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[i]
                i += 1
                ll += b
                if b != 255:
                    break
        out += src[i:i + ll]
        i += ll
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            chunk = bytes(out[start:])
            out += (chunk * (ml // off + 1))[:ml]
    if len(out) != size:
        raise ValueError("lz4 size mismatch")
    return bytes(out)


def read_nso(path):
    b = Path(path).read_bytes()
    assert b[:4] == b"NSO0", "not an NSO"
    flags = struct.unpack_from("<I", b, 0xC)[0]
    seg = []
    for i in range(3):
        fo, mo, ds = struct.unpack_from("<III", b, 0x10 + 0x10 * i)
        seg.append({"file_off": fo, "mem_off": mo, "decomp": ds})
    bss = struct.unpack_from("<I", b, 0x3C)[0]
    mid = b[0x40:0x60]
    csz = struct.unpack_from("<III", b, 0x60)
    hashes = [b[0xA0 + 0x20 * i:0xC0 + 0x20 * i] for i in range(3)]
    api = struct.unpack_from("<II", b, 0x88)
    dynstr = struct.unpack_from("<II", b, 0x90)
    dynsym = struct.unpack_from("<II", b, 0x98)
    data = []
    for i in range(3):
        raw = b[seg[i]["file_off"]:seg[i]["file_off"] + csz[i]]
        if flags & (1 << i):
            raw = lz4_block(raw, seg[i]["decomp"])
        assert len(raw) == seg[i]["decomp"]
        if flags & (1 << (3 + i)):
            assert hashlib.sha256(raw).digest() == hashes[i], "segment hash mismatch"
        data.append(raw)
    return {"hdr": b[:0x100], "flags": flags, "seg": seg, "bss": bss, "module_id": mid,
            "csz": csz, "data": data, "api": api, "dynstr": dynstr, "dynsym": dynsym,
            "file_size": len(b)}


def write_uncompressed(nso, out):
    """Header + memory image (segments at 0x100 + mem_offset, gaps zero-filled), compression flags cleared."""
    hdr = bytearray(nso["hdr"])
    struct.pack_into("<I", hdr, 0xC, nso["flags"] & ~7)
    struct.pack_into("<I", hdr, 0x2C, 0)  # module-name size zeroed; matches the reference v262144 file hash
    body = bytearray(max(s["mem_off"] + s["decomp"] for s in nso["seg"]))
    for i, (s, raw) in enumerate(zip(nso["seg"], nso["data"])):
        struct.pack_into("<I", hdr, 0x10 + 0x10 * i, 0x100 + s["mem_off"])
        struct.pack_into("<I", hdr, 0x60 + 4 * i, len(raw))
        body[s["mem_off"]:s["mem_off"] + len(raw)] = raw
    out.write_bytes(bytes(hdr) + bytes(body))


def image_of(nso):
    end = max(s["mem_off"] + s["decomp"] for s in nso["seg"])
    img = bytearray(end)
    for s, raw in zip(nso["seg"], nso["data"]):
        img[s["mem_off"]:s["mem_off"] + len(raw)] = raw
    return img


def dec_ptr(img, off, enc, base_hdr):
    fmt = {0x03: ("<I", 4), 0x0B: ("<i", 4), 0x04: ("<Q", 8), 0x0C: ("<q", 8)}[enc & 0x0F]
    v = struct.unpack_from(fmt[0], img, off)[0]
    app = enc & 0x70
    if app == 0x10:
        v += off
    elif app == 0x30:
        v += base_hdr
    return v, off + fmt[1]


def parse_fdes(img):
    res = {"mod0": False}
    try:
        mo = struct.unpack_from("<I", img, 4)[0]
        if img[mo:mo + 4] != b"MOD0":
            return res, []
        res["mod0"] = True
        dyn, bss_s, bss_e, eh_s, eh_e, rt = struct.unpack_from("<iiiiii", img, mo + 4)
        res["mod0_offset"] = mo
        res["bss_range"] = [mo + bss_s, mo + bss_e]
        ehh = mo + eh_s
        res["eh_frame_hdr"] = [ehh, mo + eh_e]
        ver, ptr_enc, cnt_enc, tab_enc = img[ehh:ehh + 4]
        off = ehh + 4
        _, off = dec_ptr(img, off, ptr_enc, ehh)
        cnt, off = dec_ptr(img, off, cnt_enc, ehh)
        res["fde_count"] = cnt
        fdes = []
        for i in range(cnt):
            a, off = dec_ptr(img, off, tab_enc, ehh)
            f, off = dec_ptr(img, off, tab_enc, ehh)
            length = struct.unpack_from("<I", img, f)[0]
            rng = struct.unpack_from("<I", img, f + 12)[0] if length else 0
            fdes.append((a, rng))
        return res, fdes
    except Exception as e:  # noqa
        res["fde_error"] = str(e)
        return res, []


def parse_dynsym(nso):
    ro = nso["data"][1]
    so, ss = nso["dynsym"]
    to, ts = nso["dynstr"]
    strtab = ro[to:to + ts]
    syms = []
    for i in range(ss // 24):
        nm, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", ro, so + 24 * i)
        name = strtab[nm:strtab.find(b"\0", nm)].decode("latin1") if nm else ""
        syms.append({"name": name, "type": info & 15, "bind": info >> 4, "defined": shndx != 0,
                     "value": val, "size": size})
    return syms


STR_RE = re.compile(rb"[\x20-\x7e]{4,}")
BUILTIN = set("bcahstijlmxyfdvwe")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*")


def _body(s, i):
    """Validate Itanium nested/template body up to the matching 'E'. Returns (index_of_E, names)."""
    named = 0
    while i < len(s):
        c = s[i]
        if c.isdigit():
            j = i
            while j < len(s) and s[j].isdigit():
                j += 1
            n = int(s[i:j])
            if n == 0 or s[i] == '0' or j + n > len(s) or not IDENT.fullmatch(s[j:j + n]):
                return -1, 0
            i = j + n
            named += 1
        elif c == "E":
            return i, named
        elif c in "NI":
            e, nn = _body(s, i + 1)
            if e < 0:
                return -1, 0
            i = e + 1
            named += nn
        elif c == "S":
            if s[i:i + 2] in ("St", "Sa", "Ss", "Sb", "Si", "So", "Sd"):
                i += 2
            else:
                m = re.match(r"S[0-9A-Z]*_", s[i:])
                if not m:
                    return -1, 0
                i += m.end()
        elif c in BUILTIN or c in "PRK":
            i += 1
        else:
            return -1, 0
    return -1, 0


def is_rtti_name(s):
    """Itanium typeinfo-name grammar: '<len><ident>' or 'N...E' with length-validated identifiers."""
    if len(s) < 3 or len(s) > 400:
        return False
    if s[0].isdigit():
        m = re.match(r"(\d+)", s)
        n = int(m.group(1))
        body = s[m.end():]
        return n > 1 and m.group(1)[0] != '0' and len(body) == n and IDENT.fullmatch(body) is not None
    if s[0] == "N" and s[-1] == "E":
        e, named = _body(s, 1)
        return e == len(s) - 1 and named >= 1
    return False


def ns_counts(names):
    """Count RTTI names by first identifier (e.g. nn / St / game-ish), top 12."""
    c = {}
    for n in names:
        m = re.match(r"N?(?:St)?(\d+)", n)
        k = "?"
        if m:
            ln = int(m.group(1))
            k = n[m.end():m.end() + ln]
            if n.startswith("St") or n.startswith("NSt"):
                k = "std:" + k
        c[k] = c.get(k, 0) + 1
    return dict(sorted(c.items(), key=lambda kv: -kv[1])[:12])


def analyze_strings(ro, data):
    strs = [m.group().decode("ascii") for m in STR_RE.finditer(ro)]
    allstrs = strs + [m.group().decode("ascii") for m in STR_RE.finditer(data)]
    rtti = sorted({s for s in strs if is_rtti_name(s)})
    src_re = re.compile(r"[\w./\\\-]+\.(?:cpp|cc|cxx|c|h|hpp|inl)(?![\w])")
    src = sorted({m.group() for s in strs for m in [src_re.search(s)] if m})
    cpp = [s for s in src if s.lower().endswith((".cpp", ".cc", ".cxx", ".c"))]
    hdr = [s for s in src if s.lower().endswith((".h", ".hpp", ".inl"))]
    low = [s.lower() for s in strs]
    kw = {k: sum(1 for s in low if k in s) for k in
          ("assert", "error", "warning", "fatal", "panic", "abort", "failed", "debug", "trace")}
    sdk = sorted({m.group() for s in allstrs for m in
                  re.finditer(r"NintendoSdk_nnSdk-[\w.\-]+|NintendoSDK_\w+-[\w.\-]+", s)})
    tool = sorted({s for s in allstrs if re.search(
        r"clang version|LLVM [0-9]|Nintendo [Cc]lang|GCC: |Apple LLVM|armclang", s)})[:10]
    return {
        "ro_string_count": len(strs),
        "rtti_name_count": len(rtti),
        "rtti_namespace_counts": ns_counts(rtti),
        "rtti_ZTS_literal_count": sum(1 for s in strs if s.startswith("_ZTS")),
        "source_path_unique": len(src),
        "source_cpp_unique": len(cpp),
        "source_header_unique": len(hdr),
        "keyword_counts": kw,
        "sdk_version_strings": sdk[:20],
        "toolchain_strings": tool,
        "nvn_api_names": sum(1 for s in strs if re.fullmatch(r"nvn[A-Z]\w+", s)),
        "_rtti_set": rtti,
        "_src_set": src,
    }


def scan_text(text):
    """Cheap AArch64 pattern counts over 4-byte aligned words."""
    n = len(text) // 4
    words = struct.unpack("<%dI" % n, text[:n * 4])
    imm_mask = ~(0x7F << 15) & 0xFFFFFFFF
    c = dict.fromkeys(("stp_x29_x30_pre", "stp_x29_x30_off", "ldp_x29_x30_post", "ldp_x29_x30_off",
                       "ret", "bl", "blr", "br", "paciasp", "autiasp", "bti", "nop", "adrp", "zero_words"), 0)
    regs = (30 << 10) | (31 << 5) | 29
    pre, off = 0xA9800000 | regs, 0xA9000000 | regs
    post, ldo = 0xA8C00000 | regs, 0xA9400000 | regs
    for w in words:
        m = w & imm_mask
        if m == pre:
            c["stp_x29_x30_pre"] += 1
        elif m == off:
            c["stp_x29_x30_off"] += 1
        elif m == post:
            c["ldp_x29_x30_post"] += 1
        elif m == ldo:
            c["ldp_x29_x30_off"] += 1
        elif w == 0xD65F03C0:
            c["ret"] += 1
        elif (w >> 26) == 0x25:
            c["bl"] += 1
        elif (w & 0xFFFFFC1F) == 0xD63F0000:
            c["blr"] += 1
        elif (w & 0xFFFFFC1F) == 0xD61F0000:
            c["br"] += 1
        elif w == 0xD503233F:
            c["paciasp"] += 1
        elif w == 0xD50323BF:
            c["autiasp"] += 1
        elif (w & 0xFFFFFF3F) == 0xD503241F:
            c["bti"] += 1
        elif w == 0xD503201F:
            c["nop"] += 1
        elif (w & 0x9F000000) == 0x90000000:
            c["adrp"] += 1
        elif w == 0:
            c["zero_words"] += 1
    return c


def analyze_nso(path, name, collect=False):
    nso = read_nso(path)
    unc = Path(str(path) + ".uncompressed.nso")
    if not unc.exists():
        write_uncompressed(nso, unc)
    r = {
        "name": name,
        "build_id": nso["module_id"][:20].hex().upper(),
        "module_id_full": nso["module_id"].hex(),
        "compressed_file_size": nso["file_size"],
        "flags": nso["flags"],
        "segments": [{"name": n, "mem_offset": s["mem_off"], "size": s["decomp"], "compressed": c}
                     for n, s, c in zip((".text", ".ro", ".data"), nso["seg"], nso["csz"])],
        "bss_size": nso["bss"],
        "uncompressed_size": 0x100 + sum(s["decomp"] for s in nso["seg"]),
        "uncompressed_file_size": unc.stat().st_size,
        "uncompressed_sha256": sha256_file(unc),
        "compressed_sha256": sha256_file(path),
    }
    img = image_of(nso)
    eh, fdes = parse_fdes(img)
    r["mod0"] = eh["mod0"]
    for k in ("mod0_offset", "bss_range", "fde_count", "fde_error"):
        if k in eh:
            r[k] = eh[k]
    if "eh_frame_hdr" in eh:
        r["eh_frame_hdr_range"] = eh["eh_frame_hdr"]
    sizes = [s for _, s in fdes if s]
    if sizes:
        r["fde_func_bytes_total"] = sum(sizes)
        r["fde_func_size_median"] = int(statistics.median(sizes))
        r["fde_func_size_mean"] = round(sum(sizes) / len(sizes), 1)
        r["fde_func_size_gt_1k"] = sum(1 for s in sizes if s > 1024)
        r["fde_text_coverage_pct"] = round(100.0 * sum(sizes) / nso["seg"][0]["decomp"], 2)
    syms = parse_dynsym(nso)
    defined = [s for s in syms if s["defined"] and s["name"]]
    imported = [s for s in syms if not s["defined"] and s["name"]]
    r["dynsym_total"] = len(syms)
    r["dynsym_defined"] = len(defined)
    r["dynsym_imported"] = len(imported)
    r["dynsym_defined_func"] = sum(1 for s in defined if s["type"] == 2)
    r["dynsym_defined_object"] = sum(1 for s in defined if s["type"] == 1)
    a_off, a_sz = nso["api"]
    r["sdk_api_list"] = sorted({m.group().decode() for m in STR_RE.finditer(nso["data"][1][a_off:a_off + a_sz])})
    r["_defined_names"] = sorted(s["name"] for s in defined)
    r["_imported_names"] = sorted(s["name"] for s in imported)
    st = analyze_strings(nso["data"][1], nso["data"][2])
    r["strings"] = {k: v for k, v in st.items() if not k.startswith("_")}
    r["_rtti_set"] = st["_rtti_set"]
    r["_src_set"] = st["_src_set"]
    r["text_scan"] = scan_text(nso["data"][0])
    if collect:
        r["_text"] = nso["data"][0]
        r["_fdes"] = fdes
    return r


def normalize_words(text, lenient):
    n = len(text) // 4
    w = list(struct.unpack("<%dI" % n, text[:n * 4]))
    for i, x in enumerate(w):
        if (x >> 26) == 0x25 or (x >> 26) == 0x05:
            w[i] = x & 0xFC000000          # BL / B imm26
        elif (x & 0x1F000000) == 0x10000000:
            w[i] = x & 0x9F00001F          # ADR / ADRP
        elif (x & 0x3B000000) == 0x18000000:
            w[i] = x & 0xFF00001F          # LDR literal
        elif lenient:
            if (x & 0x7F800000) == 0x11000000:
                w[i] = x & ~(0xFFF << 10) & 0xFFFFFFFF   # ADD imm
            elif (x & 0x3B000000) == 0x39000000:
                w[i] = x & ~(0xFFF << 10) & 0xFFFFFFFF   # LDR/STR unsigned imm
    return struct.pack("<%dI" % n, *w)


def norm_hashes(x, lenient):
    t = normalize_words(x["_text"], lenient)
    return {hashlib.sha1(t[s:s + n]).digest() for s, n in x["_fdes"] if n and s + n <= len(t)}


def func_hash_overlap(a, b):
    def hs(x):
        t = x["_text"]
        return {hashlib.sha1(t[s:s + n]).digest() for s, n in x["_fdes"] if n and s + n <= len(t)}
    ha, hb = hs(a), hs(b)
    na, nb = norm_hashes(a, False), norm_hashes(b, False)
    la, lb = norm_hashes(a, True), norm_hashes(b, True)
    return {"a_unique_bodies": len(ha), "b_unique_bodies": len(hb), "byte_identical": len(ha & hb),
            "identical_masking_branch_adrp_literal": len(na & nb), "unique_a_masked": len(na), "unique_b_masked": len(nb),
            "identical_lenient_also_mask_add_ldr_imm12": len(la & lb), "unique_a_lenient": len(la), "unique_b_lenient": len(lb)}


def text_page_equal(a, b):
    ta, tb = a["_text"], b["_text"]
    n = min(len(ta), len(tb)) // 4096
    eq = sum(1 for i in range(n) if ta[i * 4096:(i + 1) * 4096] == tb[i * 4096:(i + 1) * 4096])
    return {"pages_compared": n, "identical_pages_same_offset": eq}


def npdm_info(path, keys):
    rc, out = run([NSTOOL, "-k", keys, "-t", "meta", path])
    return {"sha256": sha256_file(path), "size": Path(path).stat().st_size,
            "nstool_text": [l for l in out.splitlines() if l.strip() and "WARNING" not in l]}


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=str(SRC / "prod.keys"))
    keys = ap.parse_args().keys
    OUT.mkdir(parents=True, exist_ok=True)
    rc, nsv = run([NSTOOL, "--help"])
    versions = {"nstool": nsv.splitlines()[0] if nsv else "?", "python": sys.version.split()[0]}

    order = [r for r, _ in REVS]
    metas, nsos = {}, {}
    for rev, srcname in REVS:
        print("acquire", rev, flush=True)
        metas[rev] = acquire(rev, srcname, keys)
    base_nca = content(metas["v0"], "Program")["path"]
    for rev in order:
        m = metas[rev]
        print("analyze", rev, flush=True)
        exe = extract_exefs(rev, m, keys)
        m["nacp"] = extract_nacp(rev, m, keys)
        mods = {}
        for p in sorted(exe.iterdir()):
            if p.is_file() and not p.name.endswith((".npdm", ".nso", ".part")):
                mods[p.name] = analyze_nso(p, p.name, collect=(p.name == "main"))
        m["exefs_files"] = sorted(p.name for p in exe.iterdir() if p.is_file() and not p.name.endswith(".nso"))
        m["npdm"] = npdm_info(exe / "main.npdm", keys)
        m["modules"] = mods
        nsos[rev] = mods["main"]
        (OUT / f"symbols-{rev}.json").write_text(json.dumps(
            {"defined": nsos[rev]["_defined_names"], "imported": nsos[rev]["_imported_names"]}), encoding="utf-8")
        (OUT / f"rtti-{rev}.txt").write_text("\n".join(nsos[rev]["_rtti_set"]), encoding="utf-8")
        (OUT / f"sourcepaths-{rev}.txt").write_text("\n".join(nsos[rev]["_src_set"]), encoding="utf-8")
    cmp = {}
    for a, b in zip(order, order[1:]):
        x, y = nsos[a], nsos[b]
        ra, rb = set(x["_rtti_set"]), set(y["_rtti_set"])
        da, db = set(x["_defined_names"]), set(y["_defined_names"])
        ia, ib = set(x["_imported_names"]), set(y["_imported_names"])
        sa, sb = set(x["_src_set"]), set(y["_src_set"])
        cmp[f"{a}->{b}"] = {
            "main_build_id_same": x["build_id"] == y["build_id"],
            "text_size_delta": y["segments"][0]["size"] - x["segments"][0]["size"],
            "fde_count_delta": y.get("fde_count", 0) - x.get("fde_count", 0),
            "rtti_added": len(rb - ra), "rtti_removed": len(ra - rb),
            "defined_sym_added": len(db - da), "defined_sym_removed": len(da - db),
            "imported_added_count": len(ib - ia), "imported_removed_count": len(ia - ib),
            "imported_sym_added": sorted(ib - ia)[:40], "imported_sym_removed": sorted(ia - ib)[:40],
            "source_paths_added": len(sb - sa), "source_paths_removed": len(sa - sb),
            "function_bodies": func_hash_overlap(x, y),
            "text_pages": text_page_equal(x, y),
        }
    listings = {}
    for rev in order:
        print("romfs", rev, flush=True)
        files, err = romfs_listing(metas[rev], keys, base_nca)
        listings[rev] = files
        if files is None:
            metas[rev]["romfs_listing_error"] = err
            continue
        (OUT / f"romfs-{rev}.txt").write_text("\n".join(sorted(files)), encoding="utf-8")
        metas[rev]["romfs_file_count"] = sum(1 for f in files if f.startswith("1/"))
    romfs_diff = {}
    for a, b in zip(order, order[1:]):
        if listings[a] is None or listings[b] is None:
            continue
        sa, sb = set(listings[a]), set(listings[b])
        romfs_diff[f"{a}->{b}"] = {
            "added": len(sb - sa), "removed": len(sa - sb),
            "added_by_dir": by_dir(sb - sa), "removed_by_dir": by_dir(sa - sb),
            "note": "name-level only; per-file size/hash of the patched RomFS is not exposed by nstool"}
    for rev in order:
        for r in metas[rev]["modules"].values():
            for k in [k for k in r if k.startswith("_")]:
                del r[k]
    npdm_diffs = {}
    for a, b in zip(order, order[1:]):
        import difflib
        la, lb = metas[a]["npdm"]["nstool_text"], metas[b]["npdm"]["nstool_text"]
        npdm_diffs[f"{a}->{b}"] = {"file_sha256_differs": metas[a]["npdm"]["sha256"] != metas[b]["npdm"]["sha256"],
                                   "text_diff": [l for l in difflib.unified_diff(la, lb, lineterm="", n=0)
                                                 if not l.startswith(("---", "+++", "@@"))]}
    cmp["npdm_diffs"] = npdm_diffs
    summary = {"tool_versions": versions, "revisions": metas, "comparisons": cmp, "romfs_diff": romfs_diff}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=1, sort_keys=True), encoding="utf-8")
    write_md(summary, order)
    print("done")


def write_md(s, order):
    L = ["# Revision audit (generated by scripts/audit_revisions.py)", "",
         "Tools: " + ", ".join(f"{k} {v}" for k, v in s["tool_versions"].items()), ""]
    rows = []

    def row(label, fn):
        rows.append([label] + [str(fn(s["revisions"][r])) for r in order])
    M = lambda m: m["modules"]["main"]
    row("source file size (bytes)", lambda m: m["source_size"])
    row("NACP display version", lambda m: m["nacp"]["display_version"])
    row("cnmt version", lambda m: m["cnmt"]["version"])
    row("program NCA id", lambda m: content(m, "Program")["id"])
    row("program NCA size", lambda m: content(m, "Program")["size"])
    row("program NCA SdkAddon version", lambda m: m["sdk_addon_version"])
    row("program NCA SHA-256 == CNMT", lambda m: content(m, "Program")["hash_matches_cnmt"])
    row("control NCA SHA-256 == CNMT", lambda m: content(m, "Control")["hash_matches_cnmt"])
    row("ExeFS files", lambda m: " ".join(m["exefs_files"]))
    row("main build ID", lambda m: M(m)["build_id"])
    row("main compressed size", lambda m: M(m)["compressed_file_size"])
    row("main uncompressed size", lambda m: M(m)["uncompressed_file_size"])
    row("main uncompressed SHA-256", lambda m: M(m)["uncompressed_sha256"])
    row("main .text size", lambda m: hex(M(m)["segments"][0]["size"]))
    row("main .ro size @ addr", lambda m: f'{hex(M(m)["segments"][1]["size"])} @ {hex(M(m)["segments"][1]["mem_offset"])}')
    row("main .data size @ addr", lambda m: f'{hex(M(m)["segments"][2]["size"])} @ {hex(M(m)["segments"][2]["mem_offset"])}')
    row("main .bss size", lambda m: hex(M(m)["bss_size"]))
    row("MOD0", lambda m: M(m)["mod0"])
    row("FDE count", lambda m: M(m).get("fde_count"))
    row("FDE median func size", lambda m: M(m).get("fde_func_size_median"))
    row("FDE text coverage %", lambda m: M(m).get("fde_text_coverage_pct"))
    row("dynsym defined / imported", lambda m: f'{M(m)["dynsym_defined"]} / {M(m)["dynsym_imported"]}')
    row("RTTI-style type names", lambda m: M(m)["strings"]["rtti_name_count"])
    row("source path strings .cpp / .h", lambda m: f'{M(m)["strings"]["source_cpp_unique"]} / {M(m)["strings"]["source_header_unique"]}')
    row("assert / error strings", lambda m: f'{M(m)["strings"]["keyword_counts"]["assert"]} / {M(m)["strings"]["keyword_counts"]["error"]}')
    row("SDK version strings", lambda m: "; ".join(M(m)["strings"]["sdk_version_strings"][:2]))
    row("RTTI names by namespace (top 4)", lambda m: ", ".join(f"{k}:{v}" for k, v in list(M(m)["strings"]["rtti_namespace_counts"].items())[:4]))
    row("toolchain strings", lambda m: len(M(m)["strings"]["toolchain_strings"]))
    for k in ("stp_x29_x30_pre", "stp_x29_x30_off", "ret", "bl", "paciasp", "bti"):
        row("scan " + k, lambda m, k=k: M(m)["text_scan"][k])
    for mod in ("sdk", "rtld"):
        row(mod + " build ID", lambda m, mod=mod: m["modules"][mod]["build_id"])
        row(mod + " uncompressed SHA-256", lambda m, mod=mod: m["modules"][mod]["uncompressed_sha256"])
    row("main.npdm SHA-256", lambda m: m["npdm"]["sha256"])
    row("RomFS file count (name listing)", lambda m: m.get("romfs_file_count", "?"))
    L.append("| field | " + " | ".join(order) + " |")
    L.append("|" + "---|" * (len(order) + 1))
    for r in rows:
        L.append("| " + " | ".join(r) + " |")
    L += ["", "## Consecutive comparisons", "```json", json.dumps(s["comparisons"], indent=1), "```", "",
          "## RomFS name diffs", "```json", json.dumps(s["romfs_diff"], indent=1), "```", ""]
    (OUT / "summary.md").write_text("\n".join(L), encoding="utf-8")


if __name__ == "__main__":
    main()
