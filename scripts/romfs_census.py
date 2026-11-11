#!/usr/bin/env python3
"""Deterministic census of the extracted v262144 RomFS (no game data embedded).

Run:  python -I scripts/romfs_census.py [--root extracted/romfs-v262144] [--no-hash] [--shaders]

Reads  : extracted/romfs-v262144/** (read-only)
Writes : analysis/romfs/files.tsv         path, size, sha256, ext, magic4
         analysis/romfs/formats.tsv       kind/key/count/bytes tallies (ext, magic, group, drp_*)
         analysis/romfs/drp_entries.tsv   one row per DRP entry (header metadata only)
         analysis/romfs/drp_archives.tsv  one row per DRP archive
         analysis/romfs/shader_scan.tsv   (--shaders) pattern hits per file / DRP entry
DRP layout follows the public ISC-licensed kwsch/NewSnap reader (header, seed table, per-entry
xorshift crypto, CRC32 extension ids).  Only headers/names are decrypted; Oodle payloads are
never decompressed (Oodle is not available here).  Pure stdlib.
"""
import argparse, collections, hashlib, os, re, struct, sys, zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "analysis" / "romfs"
M32 = 0xFFFFFFFF

DRPF_MAGIC = 0x7F0E5359      # crc32("DRPF") as seen on disk
FHDR = 0xC65753E8            # crc32("fhdr"): unencrypted marker
RESD = 0xE0A331B4            # crc32("resd"): file block
OODL = 0xE42D98BA            # crc32("Oodl"): entry payload is Oodle-compressed

# CRC32-of-extension ids (from public kwsch/NewSnap DrpFileEntry.cs)
DRP_EXT = {
0x1C375F45:"txt",0xBE1C9ACB:"msbt",0xB9715ED2:"msbp",0xBB922BB1:"lm",0x5C156DBC:"nutexb",0x950C38A5:"bnk",
0xCD63FEC8:"lmb",0x53076B6B:"lme",0x31C91A2C:"luo",0xA9B87C9F:"rtd",0xA76EEEC0:"shb",0x8C43BD03:"skb",
0x0FB47EED:"achd",0x89E759E1:"allb",0xE42AB2FE:"cbsb",0xE165A47B:"cesb",0xB20291CC:"cutb",0x7A390130:"ecbd",
0x9B70EEC4:"facb",0xF78548DD:"ldtb",0xBAF0E4F7:"lprb",0xA3D90E1B:"mdcd",0x37CD5F6B:"mdfb",0xEADAF976:"mdrp",
0xE1C38F19:"misd",0x6971203F:"navb",0xA459AA15:"nvhb",0x0B548B0F:"path",0x58284CA4:"pcnb",0x01AA8159:"pdcd",
0x6CD5ECCD:"pflb",0xB894D312:"pfrb",0x0972120A:"picd",0xF423150F:"pstb",0xBE2D954D:"ptsb",0x753C041E:"silb",
0x97948F6D:"bfotf",0x87E7C3FC:"bfttf",0x2E9EDB6D:"efxbn",0x509961FC:"characterb",0x623D9D06:"matinstb",
0xDAB89279:"numatb",0x8E6610FB:"modelb",0x0032C3E4:"nuanmb",0x81D2341C:"nuhlpb",0x236DB83A:"numshb",
0x67E93703:"nusktb",0x3F86D270:"nusrcmdlb",0xC3157ABE:"courseb",0x9C5515DE:"nufxlb",0x2F6D9B0B:"nushdb",
0x7E309203:"reflectb",0xE57E2E01:"paramb",0x2E641827:"stfrolb",0xE9AB884F:"genderb",0xF59A44EB:"levelb",
0x62BC395A:"navmshb",
}


def sar(v, n):
    if v & 0x80000000:
        v -= 1 << 32
    return (v >> n) & M32


class XorShift:
    __slots__ = "x", "y", "z", "w"

    def __init__(self, seed):
        self.w = (0x41C64E6D * seed + 12345) & M32
        self.z = (0x41C64E6D * self.w + 12345) & M32
        self.y = (0x41C64E6D * self.z + 12345) & M32
        self.x = (0x41C64E6D * self.y + 12345) & M32

    def nxt(self):
        t = (self.x ^ (self.x << 11)) & M32
        self.x, self.y, self.z = self.y, self.z, self.w
        self.w = (self.w ^ sar(self.w, 19) ^ (t ^ sar(t, 8))) & M32
        return self.w & 0x7FFFFFFF

    def word(self):  # GetNext(0xFFFFFFFF)
        a = self.nxt()
        b = self.nxt() >> 30
        return a | (b << 31)


def rng_for(seed, table):
    key = 0
    for i in range(4):
        key |= table[(seed >> (i * 8)) & 0x7F] << (i * 8)
    return XorShift(key)


def decrypt_chunk_prefix(buf, table, nbytes):
    """Decrypt a file-block chunk's first nbytes (multiple of 4). buf starts at the chunk."""
    seed = struct.unpack_from("<I", buf, 0)[0]
    r = rng_for(seed, table)
    n = min(nbytes, len(buf)) // 4
    w = list(struct.unpack_from("<%dI" % n, buf, 0))
    w[1] ^= r.word()
    w[2] ^= r.word()
    for i in range(3, n):
        w[i] ^= r.word()
    return struct.pack("<%dI" % n, *w)


def parse_drp(path):
    """Return (archive_info dict, [entry dict]). Reads headers only."""
    size = os.path.getsize(path)
    ents = []
    with open(path, "rb") as f:
        hd = bytearray(f.read(0x94))
        if len(hd) < 0x94 or struct.unpack_from("<I", hd, 0)[0] != DRPF_MAGIC:
            return None, []
        table = bytes(hd[0x14:0x94])
        seed = struct.unpack_from("<I", hd, 4)[0]
        marker = struct.unpack_from("<I", hd, 8)[0]
        encrypted = marker != FHDR
        if encrypted:
            r = rng_for(seed, table)
            for off in (8, 0xC, 0x10):
                v = struct.unpack_from("<I", hd, off)[0] ^ r.word()
                struct.pack_into("<I", hd, off, v)
        crc_ok = zlib.crc32(bytes(hd[0x10:0x94])) == seed
        count = struct.unpack_from("<I", hd, 0x10)[0]
        info = dict(size=size, encrypted=int(encrypted), crc_ok=int(crc_ok), count=count)
        if not crc_ok or count > 1000000:
            return info, []
        pos = 0x94
        for i in range(count):
            f.seek(pos)
            raw = bytearray(f.read(0x200))
            if len(raw) < 0x20:
                info["error"] = "truncated"
                break
            dec = bytearray(decrypt_chunk_prefix(raw, table, len(raw) & ~3) if encrypted else raw)
            seed_e, magic, total, extcrc, csz, dsz = struct.unpack_from("<IIIIII", dec, 0)
            if magic != RESD or total < 0x20 or pos + total > size:
                info["error"] = "bad chunk %d at 0x%x" % (i, pos)
                break
            nul = dec.find(b"\0", 0x18)
            if nul < 0:
                info["error"] = "long name"
                break
            name = dec[0x18:nul].decode("ascii", "replace")
            doff = (nul + 4) & ~3
            lead = struct.unpack_from("<I", dec, doff)[0] if doff + 4 <= len(dec) else 0
            oodle = lead == OODL
            magic4 = "" if oodle else bytes(dec[doff:doff + 4]).hex()
            ents.append(dict(idx=i, off=pos, total=total, name=name, extcrc=extcrc,
                             ext=DRP_EXT.get(extcrc, "%08X" % extcrc), oodle=int(oodle),
                             csize=csz, dsize=dsz, magic4=magic4, data_off=pos + doff,
                             encrypted=int(encrypted)))
            pos += total
        info["footer"] = size - pos
    return info, ents


def set_belownormal():
    try:
        import ctypes
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)
    except Exception:
        pass


def sha_and_magic(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        first = f.read(1 << 20)
        h.update(first)
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest(), first[:4].hex()


SHADER_PATTERNS = [
    ("NVgpuprog", re.compile(rb"!!NV[a-z]{2}\d\.\d")),
    ("glsl_version", re.compile(rb"#version [1-4]\d\d")),
    ("spirv", re.compile(rb"\x03\x02\x23\x07\x00[\x00-\x06]\x01\x00")),
    ("GFSH", re.compile(rb"GFSH")),
    ("NVN_str", re.compile(rb"NVN[a-zA-Z_ ]{3,}")),
]


def shader_scan(root, rows, ents_by_path, wr):
    """Raw scan of every non-encrypted byte: non-DRP files fully, unencrypted DRPs fully.
    Encrypted DRPs: decrypt (whole entry) only shader-ish, non-Oodle entries."""
    import mmap
    shaderish = {"shb", "nushdb", "nufxlb", "matinstb", "numatb", "efxbn", "reflectb"}
    for rel, size, _, ext, magic in rows:
        if ext == "wem" or size == 0:
            continue
        info = ents_by_path.get(rel)
        if info and info[0].get("encrypted"):
            continue
        with open(root / rel, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            for name, pat in SHADER_PATTERNS:
                hits = [mt.start() for mt in pat.finditer(m)]
                if hits:
                    wr.write("%s\t-\t%s\t%d\t0x%x\n" % (rel, name, len(hits), hits[0]))
    for rel, (info, ents) in ents_by_path.items():
        if not info.get("encrypted"):
            continue
        with open(root / rel, "rb") as f:
            table = f.read(0x94)[0x14:0x94]
            for e in ents:
                if e["ext"] not in shaderish or e["oodle"] or e["total"] > 64 << 20:
                    continue
                f.seek(e["off"])
                raw = bytearray(f.read(e["total"]))
                dec = decrypt_chunk_prefix(raw, table, len(raw) & ~3)
                for name, pat in SHADER_PATTERNS:
                    n = len(pat.findall(dec))
                    if n:
                        wr.write("%s\t%s\t%s\t%d\t-\n" % (rel, e["name"], name, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT / "extracted" / "romfs-v262144"))
    ap.add_argument("--no-hash", action="store_true")
    ap.add_argument("--shaders", action="store_true")
    a = ap.parse_args()
    set_belownormal()
    root = Path(a.root)
    OUT.mkdir(parents=True, exist_ok=True)
    paths = sorted(p for p in root.rglob("*") if p.is_file())
    rels = [p.relative_to(root).as_posix() for p in paths]
    if a.no_hash:
        res = [("", open(p, "rb").read(4).hex()) for p in paths]
    else:
        with ThreadPoolExecutor(4) as ex:
            res = list(ex.map(sha_and_magic, paths))
    rows = []
    for rel, p, (sha, mg) in zip(rels, paths, res):
        ext = p.suffix.lstrip(".").lower() or "(none)"
        rows.append((rel, p.stat().st_size, sha, ext, mg))
    with open(OUT / "files.tsv", "w", encoding="utf-8", newline="\n") as w:
        w.write("path\tsize\tsha256\text\tmagic4\n")
        for r in rows:
            w.write("%s\t%d\t%s\t%s\t%s\n" % r)

    tally = collections.defaultdict(lambda: [0, 0])

    def add(k, key, size):
        t = tally[(k, key)]
        t[0] += 1
        t[1] += size

    for rel, size, _, ext, mg in rows:
        add("ext", ext, size)
        add("magic4", mg, size)
        add("ext+magic4", "%s:%s" % (ext, mg), size)
        parts = rel.split("/")
        add("group", "/".join(parts[:3]) if len(parts) > 3 else "/".join(parts[:-1]), size)

    ents_by_path = {}
    with open(OUT / "drp_entries.tsv", "w", encoding="utf-8", newline="\n") as we, \
         open(OUT / "drp_archives.tsv", "w", encoding="utf-8", newline="\n") as wa:
        we.write("archive\tidx\tname\text\toodle\tcsize\tdsize\tmagic4_if_raw\n")
        wa.write("archive\tsize\tencrypted\tcrc_ok\tentries\tfooter\terror\n")
        for (rel, size, _, ext, mg), p in zip(rows, paths):
            if mg != "59530e7f":
                continue
            info, ents = parse_drp(p)
            if info is None:
                continue
            ents_by_path[rel] = (info, ents)
            wa.write("%s\t%d\t%d\t%d\t%d\t%s\t%s\n" % (rel, size, info["encrypted"], info["crc_ok"],
                     len(ents), info.get("footer", ""), info.get("error", "")))
            add("drp_archive_encrypted", str(info["encrypted"]), size)
            if info.get("error") or not info["crc_ok"]:
                add("drp_archive_error", info.get("error", "crc").split(" ")[0], size)
            for e in ents:
                we.write("%s\t%d\t%s\t%s\t%d\t%d\t%d\t%s\n" % (rel, e["idx"], e["name"], e["ext"],
                         e["oodle"], e["csize"], e["dsize"], e["magic4"]))
                add("drp_entry_ext", e["ext"], e["dsize"])
                add("drp_entry_ext_oodle" if e["oodle"] else "drp_entry_ext_raw", e["ext"], e["dsize"])
                if not e["oodle"]:
                    add("drp_entry_raw_ext+magic4", "%s:%s" % (e["ext"], e["magic4"]), e["dsize"])
                add("drp_entry_compression", "oodle" if e["oodle"] else "raw", e["dsize"])
    with open(OUT / "formats.tsv", "w", encoding="utf-8", newline="\n") as w:
        w.write("kind\tkey\tcount\tbytes\n")
        for (k, key), (c, b) in sorted(tally.items(), key=lambda kv: (kv[0][0], -kv[1][0], kv[0][1])):
            w.write("%s\t%s\t%d\t%d\n" % (k, key, c, b))
    if a.shaders:
        with open(OUT / "shader_scan.tsv", "w", encoding="utf-8", newline="\n") as w:
            w.write("file\tentry\tpattern\tcount\tfirst_offset\n")
            shader_scan(root, rows, ents_by_path, w)
    print("files", len(rows), "drp", len(ents_by_path), "bytes", sum(r[1] for r in rows))


if __name__ == "__main__":
    main()
