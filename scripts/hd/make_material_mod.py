# SPDX-License-Identifier: MIT
"""Material swap mod: give a Pokemon body that has no normal sampler a normal map (writes a mod pack, local files only).

A no-normal body (for example Bidoof Body) is re-pointed to a "twin" pixel shader: same samplers plus `normalTexture`,
same ParameterList, and the same lighting code (the translated SPIR-V differs only by one normal fetch, the z rebuild,
and the tangent-space normalize). The tool rewrites the material's `.matinstb` (FlatBuffers: shader names in the two
name vectors, plus one new `normalTexture` slot) and its `.numatb` (main shader name) and writes a normal texture.

  python -E -P scripts/hd/make_material_mod.py --pack <mods dir>/<pack id> --material CH_032/MI_CH_032_00_0_A00_Body \\
        --normal test|flat|<file.png> [--entry-name <existing nutexb entry>] [--size 1024] [--twin old=new,...] [--list]

--list       print the twin candidates for the material's shaders and exit (nothing written).
--entry-name file name of the new normal texture. The game only serves entries that exist in the archive index, so a brand
             new name is never loaded; default `<material label>_nor.nutexb` (new name) -- pass an existing, unused entry
             of the same archive (a "decoy") to have the loader replace that entry instead.
Reads the extracted RomFS (`extracted/drp-v262144`), never writes outside --pack. Needs numpy, Pillow and etcpak (the
same as scripts/hd/nutexb_encode.py), hence `python -E -P`.
"""
import argparse, os, re, struct, sys, zlib, glob
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'scripts' / 'hd'))
import fb_walk as fw          # noqa: E402
import fb_roundtrip as rt     # noqa: E402

RES = ROOT / 'extracted/drp-v262144/nx/Pack/Resident/master/Resident.drp'
CHARS = ROOT / 'extracted/drp-v262144/nx/Pack/Characters/master'
PREFIX = 'CharactersMaterials_CharacterShader_'


# ---------------------------------------------------------------- shader twins
def shader_info(sid):
    """(sampler base names, ParameterList member names, instruction count) of the pixel shader, from the nushdb reflection."""
    import nushdb_parse as P
    for p in P.parse_file(str(RES / (PREFIX + sid + '.nushdb'))):
        if p['name'].endswith('PS'):
            rf = p['blob']['refl']
            smp = frozenset(s['name'].rsplit('_', 1)[0] for s in rf['samplers'])
            par = frozenset(u['name'] for u in rf['uniforms'] if rf['ubos'][u['block']]['name'] == 'ParameterList')
            return smp, par, p['blob']['insn']
    raise KeyError(sid)


_DB = {}


def shader_db():
    if not _DB:
        for f in glob.glob(str(RES / (PREFIX + '*.nushdb'))):
            sid = os.path.basename(f)[len(PREFIX):-len('.nushdb')]
            _DB[sid] = shader_info(sid)
    return _DB


def twin_candidates(sid):
    """Exact twins: samplers + normalTexture, identical ParameterList. Sorted by instruction count (normal block = +42 on
    the CharacterShader family: 505->547, 535->577, 553->595)."""
    db = shader_db()
    smp, par, n = db[sid]
    if 'normalTexture' in smp:
        return []
    return sorted(((v[2], k) for k, v in db.items() if v[0] == smp | {'normalTexture'} and v[1] == par),
                  key=lambda t: (abs(t[0] - n - 42), t[1]))


def observed_partners():
    """Shader name tuples (without _zprepass) used together by real materials: {main id: (variant ids)}; used to pick twins that
    already exist as a full set in a shipped Pokemon material."""
    out = {}
    for f in glob.glob(str(CHARS / 'CH_*.drp' / 'MI_*.matinstb')):
        b = open(f, 'rb').read()
        ids = [m.group(1).decode() for m in re.finditer(rb'CharactersMaterials_CharacterShader_(\d+)\x00', b)]
        if len(ids) == 3:    # [pass A, pass B, main] (the _zprepass name does not match: it ends with a suffix)
            out.setdefault(ids[2], set()).add(tuple(ids))
    return out


# Twins whose translated SPIR-V was compared with the original by scripts/hd/shader_twin_check.py: the original plus exactly one
# normal fetch, the z rebuild, two normalizes and one extra uniform component (sssNormalMapWeight). Bidoof Body set; each one
# also appears as a full pass set in a shipped material (CH_056 Mouth: 3921109201, 747291159, 3238596505).
VERIFIED = {'3436703351': '747291159', '1019458661': '3238596505', '1059205888': '3921109201'}


def program_sha():
    """{shader id: sha of its translated pixel program}: ids with the same sha are the same binary (analysis/materials/names.tsv,
    written by the census scripts; {} when absent)."""
    out = {}
    p = ROOT / 'analysis/materials/names.tsv'
    if p.exists():
        for l in open(p):
            f = l.split()
            if len(f) >= 3 and f[0].startswith('CharacterShader_') and f[0].endswith('PS'):
                out[f[0][len('CharacterShader_'):-2]] = f[1]
    return out


def pick_twins(shader_ids, main):
    """-> {old id: new id} for the three real pass shaders (zprepass names follow the main). Picks, per shader, a candidate with the
    same pass-set as an observed material when possible."""
    if all(s in VERIFIED for s in shader_ids):
        return {s: VERIFIED[s] for s in shader_ids}, True
    sha = program_sha()
    by_sha = {sha[k]: v for k, v in VERIFIED.items() if k in sha}
    if sha and all(sha.get(s) in by_sha for s in shader_ids):
        # same program binaries as a verified set: same twins (a verified twin for the binary, whatever id the material uses)
        return {s: by_sha[sha[s]] for s in shader_ids}, True
    obs = observed_partners()
    cand = {s: [k for _, k in twin_candidates(s)] for s in shader_ids}
    if any(not c for c in cand.values()):
        raise SystemExit('no exact twin for: ' + ', '.join(s for s, c in cand.items() if not c))
    best = None
    for tm in cand[main]:
        for ids in obs.get(tm, ()):
            others = [s for s in shader_ids if s != main]
            if all(o in cand[s] for o, s in zip((ids[0], ids[1]), others)):
                best = {main: tm}
                best.update({s: o for o, s in zip((ids[0], ids[1]), others)})
                return best, True
    return {s: cand[s][0] for s in shader_ids}, False


# ---------------------------------------------------------------- matinstb rewrite
def crc(s):
    return zlib.crc32(s.encode())


def build_graph_corpus():
    files = sorted(glob.glob(str(CHARS / 'CH_*.drp' / 'MI_*.matinstb')))
    return files, [open(f, 'rb').read() for f in files]


def rewrite_matinstb(src_path, repl, slot, fname, insert_before='paramsTexture'):
    """src_path: original matinstb. repl: {old string: new string} (whole-string replacements). slot/fname: new texture slot
    (slot name, file name); None to skip. Returns new bytes (round-trip encoder of scripts/fb_roundtrip.py, verified identical
    on unmodified files)."""
    files, bufs = build_graph_corpus()
    src = os.path.abspath(src_path)
    fi = [i for i, f in enumerate(files) if os.path.abspath(f) == src]
    if not fi:
        raise SystemExit('material not in the matinstb corpus: ' + src_path)
    fi = fi[0]
    w, graphs = fw.analyse_all(bufs)
    g = graphs[fi]
    b = bufs[fi]
    if rt.encode(w, fi, g) != b:
        raise SystemExit('round trip of the unmodified file differs: encoder not safe for this file')
    return _encode(w, fi, g, repl, slot, fname, insert_before)


def _encode(w, fi, g, repl, slot, fname, insert_before):
    b = w.bufs[fi]
    # the original encode() retries with a lead of zero bytes when the file has trailing padding; mirror it
    out = _encode_lead(w, fi, g, repl, slot, fname, insert_before, 0)
    base_ok = len(rt.encode(w, fi, g)) - len(_encode_lead(w, fi, g, {}, None, None, None, 0))
    if base_ok:
        extra = len(b) - len(_encode_lead(w, fi, g, {}, None, None, None, 0))
        if 0 < extra <= 16 and not any(b[len(b) - extra:]) and extra % 4 == 0:
            out = _encode_lead(w, fi, g, repl, slot, fname, insert_before, extra)
    return out


def _encode_lead(w, fi, g, repl, slot, fname, insert_before, lead):
    b = w.bufs[fi]
    bl = rt.Builder(lead)
    if len(b) % 8 == 0:
        bl.minalign = 8
    created = {}
    for addr, o in sorted(g.objs.items(), key=lambda kv: -kv[0]):
        k = o['kind']
        if k == 'T':
            cls = w.classes[o['cls']]
            vt, vsize, tsize, slots = fw.read_table(b, addr)
            flds = []
            for sl, off in slots.items():
                kind, _, extra = o['fields'][sl]
                info = cls.slots[sl]
                if kind == 'scalar':
                    flds.append((sl, bytes(b[addr + off: addr + off + info['size']]), rt.field_align(info), off))
                else:
                    flds.append((sl, ('ref', created[extra]), 4, off))
            created[addr] = bl.table(flds, vsize, tsize, 'first')
        elif k == 'S':
            ln = fw.U32(b, addr)[0]
            s = bytes(b[addr + 4: addr + 4 + ln])
            s = repl.get(s.decode('utf-8', 'replace'), s.decode('utf-8', 'replace')).encode()
            created[addr] = bl.string(s)
        elif k in ('VT', 'VS'):
            n = o['n']
            ch = [created[fw.fwd(b, addr + 4 + 4 * i)] for i in range(n)]
            if k == 'VT' and o['cls'] == 'R.4' and slot:
                addrs = [fw.fwd(b, addr + 4 + 4 * i) for i in range(n)]
                new = _new_texture_elem(w, fi, g, bl, created, addrs, slot, fname)
                names = [_str(b, g, a, 7) for a in addrs]
                pos = names.index(insert_before) if insert_before in names else len(ch)
                ch.insert(pos, new)
            created[addr] = bl.vec_offsets(ch)
        elif k == 'VX':
            n, es = o['n'], o['es']
            al = es if es in (1, 2, 4, 8) else 4
            created[addr] = bl.vec_scalar(bytes(b[addr + 4: addr + 4 + n * es]), n, es, al)
        elif k == 'RAW':
            created[addr] = bl.raw(bytes(b[addr: addr + o['size']]))
        else:
            raise ValueError(k)
    return bl.finish(created[g.root], g.ident)


def _str(b, g, taddr, slotno):
    kind, _, extra = g.objs[taddr]['fields'].get(slotno, (None, 0, None))
    if kind != 'string':
        return None
    return fw.read_string(b, extra)


def _new_texture_elem(w, fi, g, bl, created, addrs, slot, fname):
    """Clone of the material's paramsTexture element (same sampler state table) with the slot name, file and their crc32 swapped."""
    b = w.bufs[fi]
    tmpl = None
    for a in addrs:
        if _str(b, g, a, 7) == 'paramsTexture':
            tmpl = a
    if tmpl is None:
        tmpl = addrs[-1]
    cls = w.classes[g.objs[tmpl]['cls']]
    vt, vsize, tsize, slots = fw.read_table(b, tmpl)
    o = g.objs[tmpl]
    strs = {0: slot, 1: fname, 7: slot}      # s0 sampler name (= slot name on non-diffuse slots), s1 file, s7 slot name
    crcs = {2: crc(slot), 3: crc(fname), 8: crc(slot)}
    made = {}
    flds = []
    for sl, off in slots.items():
        kind, _, extra = o['fields'][sl]
        info = cls.slots[sl]
        if kind == 'scalar':
            raw = bytes(b[tmpl + off: tmpl + off + info['size']])
            if sl in crcs:
                raw = struct.pack('<I', crcs[sl])
            flds.append((sl, raw, rt.field_align(info), off))
        elif kind == 'string':
            flds.append((sl, ('ref', bl.string(strs[sl].encode())), 4, off))
        else:
            flds.append((sl, ('ref', created[extra]), 4, off))
    return bl.table(flds, vsize, tsize, 'first')


# ---------------------------------------------------------------- numatb rewrite
def rewrite_numatb(path, old, new):
    b = open(path, 'rb').read()
    o, n = old.encode() + b'\0', new.encode() + b'\0'
    if b.count(o) != 1:
        raise SystemExit('numatb: shader name not found exactly once: ' + old)
    return b.replace(o, n)


# ---------------------------------------------------------------- textures
def make_normal_png(kind, size, path):
    import numpy as np
    from PIL import Image
    if kind == 'flat':
        px = np.zeros((size, size, 4), np.uint8)
        px[..., 0] = px[..., 1] = 128
        px[..., 2] = 255
        px[..., 3] = 255
    elif kind == 'test':
        # strong test pattern: a grid of round bumps (a hemisphere normal field), tilt up to ~0.9, so that lighting shows a
        # clear dotted relief and the texel size is obvious (period size/8 texels)
        cell = max(size // 8, 8)
        yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
        fx = ((xx % cell) + 0.5) / cell * 2 - 1
        fy = ((yy % cell) + 0.5) / cell * 2 - 1
        r2 = fx * fx + fy * fy
        inside = r2 < 0.8
        nx_ = np.where(inside, fx * 0.9, 0.0)
        ny_ = np.where(inside, fy * 0.9, 0.0)
        nz = np.sqrt(np.clip(1 - nx_ * nx_ - ny_ * ny_, 0, 1))
        px = np.stack([nx_ * 0.5 + 0.5, ny_ * 0.5 + 0.5, nz * 0.5 + 0.5, np.ones_like(nz)], -1)
        px = (np.clip(px, 0, 1) * 255 + 0.5).astype(np.uint8)
    else:
        Image.open(kind).convert('RGBA').save(path)
        return
    Image.fromarray(px, 'RGBA').save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', required=True, help='output pack folder (created); the pack id is its name')
    ap.add_argument('--material', required=True, help='CH_032/MI_CH_032_00_0_A00_Body (character pack / material label)')
    ap.add_argument('--normal', default='flat', help='flat | test | path to a PNG')
    ap.add_argument('--entry-name', help='file name of the normal texture entry (default <label>_nor.nutexb)')
    ap.add_argument('--size', type=int, default=1024)
    ap.add_argument('--twin', help='old=new[,old=new] shader id overrides')
    ap.add_argument('--no-texture-slot', action='store_true', help='only swap the shader (the shader then reads dummy_normal_texture)')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()

    ch, label = a.material.split('/')
    drp = CHARS / (ch + '.drp')
    mi = drp / (label + '.matinstb')
    nu = drp / (label + '.numatb')
    data = mi.read_bytes()
    ids = [m.group(1).decode() for m in re.finditer(rb'CharactersMaterials_CharacterShader_(\d+)\x00', data)]
    zid = [m.group(1).decode() for m in re.finditer(rb'CharactersMaterials_CharacterShader_(\d+)_zprepass\x00', data)]
    if len(ids) != 3 or len(zid) != 1 or ids[2] != zid[0]:
        raise SystemExit('unexpected shader list %r %r' % (ids, zid))
    main_id = ids[2]
    print('material', label, 'shaders', ids, 'main', main_id)
    if a.list:
        for s in ids:
            print(s, 'insn', shader_db()[s][2], 'twins (insn, id):', twin_candidates(s)[:12])
        return
    if a.twin:
        mp = dict(x.split('=') for x in a.twin.split(','))
        seen = True
    else:
        mp, seen = pick_twins(ids, main_id)
    print('twin map', mp, '(matches a shipped material set)' if seen else '(no shipped set matches; first candidates)')
    repl = {}
    for old, new in mp.items():
        repl[PREFIX + old] = PREFIX + new
        repl['CharacterShader_' + old] = 'CharacterShader_' + new
    for old, new in [(PREFIX + main_id + '_zprepass', PREFIX + mp[main_id] + '_zprepass'),
                     ('CharacterShader_' + main_id + '_zprepass', 'CharacterShader_' + mp[main_id] + '_zprepass')]:
        repl[old] = new
    entry = a.entry_name or (label.replace('MI_', '', 1) + '_nor.nutexb')
    new_mi = rewrite_matinstb(mi, repl, None if a.no_texture_slot else 'normalTexture', entry)
    new_nu = rewrite_numatb(nu, PREFIX + main_id, PREFIX + mp[main_id])
    pack = Path(a.pack)
    arch = pack / 'nx/Pack/Characters/master' / (ch + '.drp')
    arch.mkdir(parents=True, exist_ok=True)
    (arch / (label + '.matinstb')).write_bytes(new_mi)
    (arch / (label + '.numatb')).write_bytes(new_nu)
    if not a.no_texture_slot:
        import nutexb_encode as enc
        png = pack / '_normal_source.png'
        make_normal_png(a.normal, a.size, png)
        tmpl = drp / (label.replace('MI_', '', 1).rsplit('_', 1)[0] + '_Eye_nor.nutexb')
        if not tmpl.exists():
            tmpl = next(iter(sorted(drp.glob('*_nor.nutexb'))), None)
        if tmpl is None:
            raise SystemExit('no normal texture in the archive to take the format footer from (BC7/BC5 template)')
        info = enc.encode(png, tmpl, arch / entry)
        print('normal texture', entry, info)
        png.unlink()
    (pack / 'pack.json').write_text('{"name": "Material swap test (%s)", "version": "0.1.0", "description": "twin shader + %s normal map"}'
                                    % (label, a.normal if a.normal in ('flat', 'test') else 'custom'))
    print('wrote', arch)


if __name__ == '__main__':
    main()
