#!/usr/bin/env python3
"""Unpack DRP archive entries (raw + Oodle) to extracted/drp-v262144/<archive path>/<name>.<ext>.

Run (private venv, BelowNormal is set automatically):
  tools\\venv-oodle\\Scripts\\python -I scripts\\drp_unpack.py --set priority --max-minutes 25
  options: --set priority|rest|all   --archive <substring>   --ext <ext[,ext]>   --retry-failed
           --limit N (entries)   --max-minutes M   --verify-only (re-hash existing outputs)
Resumable: analysis/romfs/drp_unpacked.tsv records every entry; entries with status ok whose output
file exists with the right size are skipped.  Source only; writes game-derived bytes ONLY under
extracted/drp-v262144 (git-ignored).

Per chunk layout (confirmed): word0 = CRC32 of the decompressed data (also the crypto seed), word1 = 'resd',
word2 = total, word3 = ext crc, word4 = csize (payload bytes incl. the 4-byte 'Oodl' tag), word5 = dsize.
Oodle payload = 'Oodl' + stream; decoder call = OodleLZ_Decompress(payload+4, csize-4, dst, dsize, 1,0,0,0,...,3).
Raw entry (no 'Oodl' tag): payload[:dsize] is the data.
Encrypted archives: whole chunk (words 1..) XORed with the per-chunk xorshift stream; done by
tools/oodle_build/drp_xor.dll (built from scripts/drp_xor/drp_xor.rs).
"""
import argparse, ctypes, hashlib, os, struct, sys, time, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import romfs_census as rc          # noqa: E402
import oodle_unicorn as ou         # noqa: E402

ROOT = rc.ROOT
RFS = ROOT / "extracted" / "romfs-v262144"
OUTDIR = ROOT / "extracted" / "drp-v262144"
MANIFEST = ROOT / "analysis" / "romfs" / "drp_unpacked.tsv"
DLL = ROOT / "tools" / "oodle_build" / "drp_xor.dll"
HDR = "archive\tidx\tentry\ttype\tcsize\tsize\tsha256\tstatus\n"

PRI_EXT = {"nushdb", "luo", "courseb", "levelb", "msbt", "msbp", "allb", "mdfb", "genderb", "pdcd", "silb",
           "ecbd", "mdcd", "navb", "rtd", "path", "paramb", "pcnb", "misd", "achd", "mdrp", "nvhb", "ldtb",
           "picd", "pstb", "pfrb", "pflb", "lm", "lmb", "lme", "nufxlb", "reflectb", "shb"}


def is_priority(arch, ext):
    a = arch
    if a.startswith("common/Text/") or a.startswith("common/UI/master/Scenes/"):
        return True
    if a in ("resource/nx/resource.drp",) or a.startswith("nx/Pack/Resident/") or a.endswith("/LevelCommon.drp"):
        return True
    return ext in PRI_EXT and not a.startswith("nx/Sound/")


def load_manifest():
    done = {}
    if MANIFEST.exists():
        for ln in MANIFEST.read_text(encoding="utf-8").splitlines()[1:]:
            p = ln.split("\t")
            if len(p) == 8:
                done[(p[0], int(p[1]))] = p
    return done


def out_path(arch, e, used):
    nm = e["name"]
    base = nm if nm.lower().endswith("." + e["ext"].lower()) else "%s.%s" % (nm, e["ext"])
    key = (arch, base.lower())
    if used.get(key):
        base = "%s__%d.%s" % (nm[:-len(e["ext"]) - 1] if nm.lower().endswith("." + e["ext"].lower()) else nm, e["idx"], e["ext"])
    used[key] = True
    return OUTDIR / arch / base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default="priority", choices=["priority", "rest", "all"])
    ap.add_argument("--archive", default="")
    ap.add_argument("--ext", default="")
    ap.add_argument("--retry-failed", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--max-minutes", type=float, default=25)
    ap.add_argument("--no-write", action="store_true", help="decode and verify only")
    ap.add_argument("--sample", type=int, default=0, help="K entries per ext spread over all archives (implies --no-write)")
    ap.add_argument("--manifest", default="")
    a = ap.parse_args()
    rc.set_belownormal()
    xor = ctypes.CDLL(str(DLL))
    xor.drp_xor.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    global MANIFEST
    if a.manifest:
        MANIFEST = Path(a.manifest)
    if a.sample:
        a.no_write = True
    exts = set(x for x in a.ext.split(",") if x)
    sample_keys = None
    if a.sample:
        import collections
        byext = collections.defaultdict(list)
        for ln in (ROOT / "analysis/romfs/drp_entries.tsv").read_text(encoding="utf-8").splitlines()[1:]:
            q = ln.split("	")
            byext[q[3]].append((q[0], int(q[1]), int(q[6])))
        sample_keys = set()
        for ex, lst in byext.items():
            lst = [x for x in lst if x[2] <= 40 << 20] or lst
            step = max(1, len(lst) // a.sample)
            for x in lst[::step][:a.sample]:
                sample_keys.add((x[0], x[1]))
            big = max(byext[ex], key=lambda x: x[2])   # also the largest of each ext
            if big[2] <= 110 << 20:
                sample_keys.add((big[0], big[1]))
    done = {} if a.sample else load_manifest()
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    newfile = not MANIFEST.exists() or a.sample
    mf = open(MANIFEST, "w" if a.sample else "a", encoding="utf-8", newline="\n")
    if newfile:
        mf.write(HDR)
    vm = ou.OodleVM()
    t_end = time.time() + a.max_minutes * 60
    arches = sorted(p.relative_to(RFS).as_posix() for p in RFS.rglob("*.drp"))
    cnt = dict(ok=0, fail=0, skipped=0)
    bytes_out = 0
    t_dec = 0.0
    n_done = 0
    stop = False
    for arch in arches:
        if a.archive and a.archive not in arch:
            continue
        info, ents = rc.parse_drp(RFS / arch)
        if not ents:
            continue
        sel = []
        for e in ents:
            if exts and e["ext"] not in exts:
                continue
            if sample_keys is not None:
                if (arch, e["idx"]) in sample_keys:
                    sel.append(e)
                continue
            pri = is_priority(arch, e["ext"])
            if a.set == "priority" and not pri:
                continue
            if a.set == "rest" and pri:
                continue
            sel.append(e)
        todo = []
        used = {}
        for e in ents:   # path assignment must be stable over the whole archive
            e["_path"] = out_path(arch, e, used)
        for e in sel:
            m = done.get((arch, e["idx"]))
            if m and (m[7] == "ok" and e["_path"].exists() and e["_path"].stat().st_size == int(m[5])):
                cnt["skipped"] += 1
                continue
            if m and m[7].startswith("fail") and not a.retry_failed:
                cnt["skipped"] += 1
                continue
            todo.append(e)
        if not todo:
            continue
        with open(RFS / arch, "rb") as f:
            table = f.read(0x94)[0x14:0x94]
            for e in todo:
                if time.time() > t_end or (a.limit and n_done >= a.limit):
                    stop = True
                    break
                n_done += 1
                status, sha, size = "ok", "", e["dsize"]
                try:
                    f.seek(e["off"])
                    buf = bytearray(f.read(e["total"]))
                    if e["encrypted"]:
                        n4 = len(buf) // 4
                        seed = struct.unpack_from("<I", buf, 0)[0]
                        r = rc.rng_for(seed, table)
                        st = (ctypes.c_uint32 * 4)(r.x, r.y, r.z, r.w)
                        cb = (ctypes.c_char * len(buf)).from_buffer(buf)
                        xor.drp_xor(ctypes.addressof(cb), n4, ctypes.addressof(st))
                        del cb
                    crc_expect = struct.unpack_from("<I", buf, 0)[0]
                    magic, total, extcrc, csz, dsz = struct.unpack_from("<5I", buf, 4)
                    assert magic == rc.RESD and dsz == e["dsize"] and csz == e["csize"], "chunk header mismatch"
                    d0 = e["data_off"] - e["off"]
                    pay = bytes(buf[d0:d0 + csz])
                    if e["oodle"]:
                        t0 = time.perf_counter()
                        n, out = vm.decompress(pay[4:], dsz)
                        t_dec += time.perf_counter() - t0
                        if n != dsz:
                            raise RuntimeError("oodle returned %d, expected %d" % (n, dsz))
                    else:
                        out = pay[:dsz]
                        if len(out) != dsz:
                            raise RuntimeError("raw entry shorter than dsize")
                    if zlib.crc32(out) != crc_expect:
                        status = "crc_mismatch"
                    sha = hashlib.sha256(out).hexdigest()
                    if not a.no_write:
                        p = e["_path"]
                        p.parent.mkdir(parents=True, exist_ok=True)
                        tmp = p.with_name(p.name + ".part")
                        tmp.write_bytes(out)
                        os.replace(tmp, p)
                    bytes_out += len(out)
                    cnt["ok" if status == "ok" else "fail"] += 1
                except Exception as ex:   # record and continue; rebuild the VM (state may be dirty)
                    status = "fail:" + str(ex).replace("\t", " ").replace("\n", " ")[:120]
                    cnt["fail"] += 1
                    vm = ou.OodleVM()
                mf.write("%s\t%d\t%s\t%s\t%d\t%d\t%s\t%s\n" % (arch, e["idx"], e["name"], e["ext"], e["csize"],
                                                                 size, sha, status))
                mf.flush()
        if stop:
            break
    mf.close()
    print("ok=%d fail=%d skipped=%d  wrote=%.3f GB  decode_time=%.1fs (%.2f MB/s decoded)  stopped_early=%s" % (
        cnt["ok"], cnt["fail"], cnt["skipped"], bytes_out / 1e9, t_dec,
        (bytes_out / 1e6 / t_dec) if t_dec else 0, stop))


if __name__ == "__main__":
    main()
