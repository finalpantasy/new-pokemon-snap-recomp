# SPDX-License-Identifier: MIT
"""HD asset cache builder (phase 1). See docs/hd-assets.md for the cache contract.

  python -E -P scripts\\hd\\build_cache.py --assets p001,p002,other001,florio_ground --variants x4_realesrgan,x4_anime
  python -E -P scripts\\hd\\build_cache.py --course florio --variants none
  python -E -P scripts\\hd\\build_cache.py --all-pokemon --variants x4_realesrgan

Asset specs:  pNNN (species NNN, form 00 model SK_CH_NNN_00_0_A00), pNNN:<model stem>, otherNNN (SK_CH_Other_NNN_00_A00),
              an alias such as florio_ground, or env:<LC pack>/<model stem>.
Everything is written under build/hd/cache (git-ignored). Incremental: existing outputs are skipped unless --force.
All inputs are the owner's own local game data; nothing from them is committed.
"""
import argparse, json, os, re, struct, subprocess, sys, time, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tests' / 'nvn'))
import fbmini  # noqa: E402
import nutexb_ref as nx  # noqa: E402

PACK = ROOT / 'extracted' / 'drp-v262144' / 'nx' / 'Pack'
TEXT_EN = ROOT / 'extracted' / 'drp-v262144' / 'common' / 'Text' / 'master' / 'TextResource_en.drp'
BUILD = ROOT / 'build' / 'hd'
CACHE = BUILD / 'cache'
TMP = BUILD / 'tmp'
LOCK = ROOT / 'build' / 'heavy.lock'
UPSCALER = ROOT / 'tools' / 'upscale' / 'realesrgan' / 'realesrgan-ncnn-vulkan.exe'
CONVERTER = BUILD / 'target' / 'release' / 'ssbh_to_gltf.exe'
PIPELINE = 4

VARIANTS = {
    'x4_realesrgan': {'label': 'Real-ESRGAN x4plus', 'scale': 4, 'model': 'realesrgan-x4plus'},
    'x4_anime': {'label': 'Real-ESRGAN x4plus anime', 'scale': 4, 'model': 'realesrgan-x4plus-anime'},
    'x4_enhanced': {'label': 'Enhanced (best AI, colour lock, edge clamp)', 'scale': 4, 'model': None},  # made by scripts/hd/enhance.py
}
ROLE_BY_PARAM = [  # (substring of the matinstb parameter name, role); first match wins
    ('normal', 'normal'), ('params', 'mask'), ('specular', 'mask'), ('emissive', 'emissive'), ('basecolor', 'albedo'),
]
ROLE_BY_SUFFIX = [('_col', 'albedo'), ('_dif', 'albedo'), ('_nor', 'normal'), ('_par', 'mask'), ('_prm', 'mask'), ('_emi', 'emissive')]
STARTER = ['p001', 'p032', 'p006', 'p112', 'p113', 'p097', 'p034', 'other001', 'florio_ground', 'florio_grass', 'florio_rock', 'florio_tree', 'florio_flower']  # Pikachu, Bidoof, Pichu, Grookey, Scorbunny, Bouffalant, Vivillon
ALIASES = {  # Florio = course 1 (LC_001_A, SM_ENV_001_1_*)
    'florio_ground': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_Ground_LightmapUV', 'Florio ground', 'course'),
    'florio_ground2': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_Ground2_LightmapUV', 'Florio ground 2', 'course'),
    'florio_grass': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_GrassB1_LightmapUV', 'Florio grass', 'course'),
    'florio_flower': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_Flower1_LightmapUV', 'Florio flowers', 'course'),
    'florio_rock': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_Rock1_LightmapUV', 'Florio rock', 'course'),
    'florio_tree': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_TreeA_LightmapUV', 'Florio tree', 'course'),
    'florio_wall': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_StonewallA_LightmapUV', 'Florio stone wall', 'course'),
    'florio_hedge': ('LevelComposite/master/LC_001_A.drp', 'SM_ENV_001_1_Hedge_LightmapUV', 'Florio hedge', 'course'),
}
COURSES = {'florio': ['florio_ground', 'florio_ground2', 'florio_grass', 'florio_flower', 'florio_rock', 'florio_tree', 'florio_wall', 'florio_hedge']}


def log(*a):
    print(*a, flush=True)


# ---------------------------------------------------------------- names
def msbt_labels(path):
    d = Path(path).read_bytes()
    pos = 0x20
    labels = {}
    strings = []
    while pos + 16 <= len(d):
        tag = d[pos:pos + 4]
        size = struct.unpack_from('<I', d, pos + 4)[0]
        body = pos + 16
        if tag == b'LBL1':
            n = struct.unpack_from('<I', d, body)[0]
            for i in range(n):
                cnt, off = struct.unpack_from('<II', d, body + 4 + i * 8)
                p = body + off
                for _ in range(cnt):
                    ln = d[p]
                    name = d[p + 1:p + 1 + ln].decode('ascii', 'replace')
                    idx = struct.unpack_from('<I', d, p + 1 + ln)[0]
                    labels[name] = idx
                    p += 1 + ln + 4
        elif tag == b'TXT2':
            n = struct.unpack_from('<I', d, body)[0]
            offs = struct.unpack_from('<%dI' % n, d, body + 4)
            for i in range(n):
                s = body + offs[i]
                e = body + offs[i + 1] if i + 1 < n else body + size
                strings.append(d[s:e].decode('utf-16le', 'replace').rstrip('\x00'))
        pos = body + size
        pos = (pos + 15) & ~15
    return {k: strings[v] for k, v in labels.items() if v < len(strings)}


_names = None


def species_name(n):
    global _names
    if _names is None:
        try:
            _names = msbt_labels(TEXT_EN / 'Poke_Name.msbt')
        except Exception as e:  # noqa: BLE001
            log('warn: Poke_Name.msbt unreadable:', e)
            _names = {}
    return _names.get('P%03d_Name' % n) or 'Pokemon %03d' % n


# ---------------------------------------------------------------- texture index and decode
_tex_index = None


def tex_index():
    global _tex_index
    f = TMP / 'nutexb_index.json'
    if _tex_index is None:
        if f.exists():
            _tex_index = json.loads(f.read_text())
        else:
            idx = {}
            for dp, dn, fn in os.walk(PACK):
                for x in fn:
                    if x.endswith('.nutexb'):
                        idx.setdefault(x[:-7], []).append(os.path.relpath(os.path.join(dp, x), PACK).replace('\\', '/'))
            TMP.mkdir(parents=True, exist_ok=True)
            f.write_text(json.dumps(idx))
            _tex_index = idx
    return _tex_index


def find_texture(name, packdir):
    p = packdir / (name + '.nutexb')
    if p.exists():
        return p
    c = tex_index().get(name)
    return PACK / c[0] if c else None


FMT_NAMES = {'r8': 'R8', 'rgba8': 'RGBA8', 'rgba16f': 'RGBA16F', 'rgba32f': 'RGBA32F', 'bc1': 'BC1', 'bc2': 'BC2', 'bc3': 'BC3',
             'bc4': 'BC4', 'bc5': 'BC5', 'bc6u': 'BC6H', 'bc6s': 'BC6H', 'bc7': 'BC7'}


def decode_nutexb(path):
    """-> (uint8 RGBA array HxWx4, meta). Top mip, layer 0 (cube faces: +X face only)."""
    import numpy as np
    t = nx.parse(str(path))
    bpb, bw, bh, kind, srgb = nx.FORMATS[t['fmt']]
    lin, lv, ls = nx.level_linear(t, 0)
    meta = {'w': t['w'], 'h': t['h'], 'format': FMT_NAMES.get(kind, kind), 'srgb': bool(srgb), 'mips': t['mips'], 'dim': t['dim'], 'layers': t['layers']}
    w, h = t['w'], t['h']
    if kind in ('bc6u', 'bc6s'):  # decoder returns clamped 8-bit BGRA
        import texture2ddecoder as t2d
        wb, hb = -(-w // 4), -(-h // 4)
        sl = np.ascontiguousarray(lin[0])
        raw = t2d.decode_bc6(np.ascontiguousarray(sl[:hb, :wb * bpb]).tobytes(), wb * 4, hb * 4)
        px = np.frombuffer(raw, np.uint8).reshape(hb * 4, wb * 4, 4)[:h, :w, [2, 1, 0, 3]].copy()
        meta['note'] = 'BC6H clamped to 8 bit'
    else:
        px = nx.decode_blocks(lin, w, h, t['fmt'])
    if px.dtype != np.uint8:  # HDR formats: simple clamp tonemap
        px = np.clip(px, 0, 1)
        px = (px * 255 + 0.5).astype(np.uint8)
    if kind == 'bc5':  # two-channel normal map: rebuild Z so the PNG is a usable tangent-space normal
        x = px[..., 0].astype(np.float32) / 127.5 - 1
        y = px[..., 1].astype(np.float32) / 127.5 - 1
        z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
        px = px.copy()
        px[..., 2] = np.clip((z * 0.5 + 0.5) * 255 + 0.5, 0, 255).astype(np.uint8)
        px[..., 3] = 255
        meta['note'] = 'BC5: Z reconstructed'
    return px, meta


def role_of(param, tex):
    p = (param or '').lower()
    for s, r in ROLE_BY_PARAM:
        if s in p:
            return r
    t = tex.lower()
    for s, r in ROLE_BY_SUFFIX:
        if t.endswith(s):
            return r
    return 'other'


# ---------------------------------------------------------------- model
def ensure_converter():
    if CONVERTER.exists():
        return
    env = dict(os.environ, CARGO_TARGET_DIR=str(BUILD / 'target'))
    log('building ssbh_to_gltf ...')
    subprocess.run(['cargo', 'build', '--release', '--manifest-path', str(HERE / 'ssbh_to_gltf' / 'Cargo.toml')], env=env, check=True,
                   creationflags=0x4000)


def matinst_textures(packdir, label):
    """material label -> [(param, texture name)] from the .matinstb next to the model."""
    p = packdir / (label + '.matinstb')
    if not p.exists():
        return None
    r = fbmini.root(p.read_bytes())
    out = []
    for t in r.vec(4):
        tex = (t.string(1) or '')
        if tex.endswith('.nutexb'):
            tex = tex[:-7]
        param = t.string(7) or t.string(0) or ''
        if tex:
            out.append((param, tex))
    return out


def patch_gltf(gpath, packdir, notes):
    """add images/textures/roles from the matinstb files to the converter's glTF; returns the per-material info."""
    g = json.loads(gpath.read_text())
    images, tex_of, mats_info = [], {}, []
    for m in g.get('materials', []):
        tl = matinst_textures(packdir, m['name'])
        roles = {}
        entries = []
        if tl is None:
            notes.append('no matinstb for material ' + m['name'])
            tl = []
        for param, tex in sorted(tl, key=lambda e: e[0].lower().startswith('blend')):  # plain slots win over blend-layer slots
            r = role_of(param, tex)
            entries.append({'param': param, 'name': tex, 'role': r})
            roles.setdefault(r, tex)
        for k in ('normalTexture', 'emissiveTexture', 'emissiveFactor'):
            m.pop(k, None)
        m.setdefault('pbrMetallicRoughness', {}).pop('baseColorTexture', None)

        def img(n):
            if n not in tex_of:
                images.append({'name': n, 'uri': 'textures/%s.orig.png' % n})
                tex_of[n] = len(images) - 1
            return tex_of[n]
        if 'albedo' in roles:
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': img(roles['albedo'])}
        if 'normal' in roles:
            m['normalTexture'] = {'index': img(roles['normal'])}
        if 'emissive' in roles:
            m['emissiveTexture'] = {'index': img(roles['emissive'])}
            m['emissiveFactor'] = [1, 1, 1]
        if 'mask' in roles:
            img(roles['mask'])
        m['extras']['textures'] = entries
        mats_info.append({'name': m['name'], 'albedo': roles.get('albedo'), 'normal': roles.get('normal'), 'mask': roles.get('mask'),
                          'emissive': roles.get('emissive'), 'shader': m['extras'].get('shader'), 'blend': m['extras'].get('blend'),
                          'textures': entries})
    for n, _ in [(e['name'], 0) for mi in mats_info for e in mi['textures']]:
        if n not in tex_of:
            images.append({'name': n, 'uri': 'textures/%s.orig.png' % n})
            tex_of[n] = len(images) - 1
    if images:
        g['images'] = images
        g['textures'] = [{'source': i} for i in range(len(images))]
    else:
        g.pop('images', None)
        g.pop('textures', None)
    gpath.write_text(json.dumps(g, separators=(',', ':')))
    return mats_info


def convert_model(packdir, stem, outdir, fname):
    ensure_converter()
    info_tmp = TMP / (fname + '.info.json')
    TMP.mkdir(parents=True, exist_ok=True)
    gl = outdir / fname
    r = subprocess.run([str(CONVERTER), str(packdir / (stem + '.nusrcmdlb')), str(gl), '--info', str(info_tmp)], capture_output=True, text=True,
                       creationflags=0x4000)
    if r.returncode != 0:
        raise RuntimeError('converter failed: ' + r.stderr[-400:])
    info = json.loads(info_tmp.read_text())
    return info


def facb_expressions(packdir):
    out = []
    for f in sorted(packdir.glob('*.facb')):
        try:
            r = fbmini.root(f.read_bytes())
            for t in r.vec(2):
                out.append({'name': t.string(0), 'param_hash': t.u32(1), 'u': round(t.f32(2), 4), 'v': round(t.f32(3), 4)})
        except Exception as e:  # noqa: BLE001
            log('warn: facb', f.name, e)
        break
    return out


# ---------------------------------------------------------------- assets
def resolve(spec):
    """-> dict(id, name, category, packdir, stem)"""
    if spec in ALIASES:
        pk, stem, name, cat = ALIASES[spec]
        return dict(id=spec, name=name, category=cat, packdir=PACK / pk, stem=stem)
    m = re.fullmatch(r'p(\d{3})(?::(.+))?', spec)
    if m:
        n = int(m.group(1))
        packdir = PACK / 'Characters' / 'master' / ('CH_%03d.drp' % n)
        stem = m.group(2) or 'SK_CH_%03d_00_0_A00' % n
        mm = re.match(r'SK_CH_\d+_(\d\d)_(\d)_(A\d\d)', stem)
        aid = 'p%03d_%s' % (n, mm.group(1) if mm else '00') + ('' if stem == 'SK_CH_%03d_00_0_A00' % n else '_' + stem.split('_', 3)[3].lower())
        return dict(id=aid, name=species_name(n), category='pokemon', packdir=packdir, stem=stem)
    m = re.fullmatch(r'other(\d{3})', spec)
    if m:
        n = m.group(1)
        return dict(id='other%s_00' % n, name='Player character pack %s (with Research Camera)' % n if n == '001' else 'Other character %s' % n,
                    category='character', packdir=PACK / 'Other' / 'master' / ('SK_CH_Other_%s_00.drp' % n), stem='SK_CH_Other_%s_00_A00' % n)
    m = re.fullmatch(r'env:([^/]+)/(.+)', spec)
    if m:
        return dict(id=re.sub(r'\W+', '_', m.group(2)).lower(), name=m.group(2), category='course',
                    packdir=PACK / 'LevelComposite' / 'master' / (m.group(1) + ('' if m.group(1).endswith('.drp') else '.drp')), stem=m.group(2))
    raise SystemExit('unknown asset spec: ' + spec)


def build_asset(a, force, decode_only=False):
    out = CACHE / a['id']
    (out / 'textures').mkdir(parents=True, exist_ok=True)
    infof = out / 'info.json'
    if infof.exists() and not force:
        old = json.loads(infof.read_text())
        if old.get('pipeline') == PIPELINE:
            log('[%s] model up to date' % a['id'])
            ensure_textures(a, out, old, force)
            return old
    packdir, stem = a['packdir'], a['stem']
    if not (packdir / (stem + '.nusrcmdlb')).exists():
        raise SystemExit('missing model %s in %s' % (stem, packdir))
    lods, lod_info, notes = [], [], []
    mats_info = None
    for i, suffix in enumerate(['', '_LOD1', '_LOD2', '_LOD3']):
        s = stem + suffix
        if not (packdir / (s + '.nusrcmdlb')).exists():
            continue
        fname = 'model.gltf' if i == 0 else 'model_lod%d.gltf' % i
        inf = convert_model(packdir, s, out, fname)
        mi = patch_gltf(out / fname, packdir, notes)
        if mats_info is None:
            mats_info = mi
        lods.append('%s/%s' % (a['id'], fname))
        lod_info.append({'file': fname, 'model': inf['model'], 'meshes': len(inf['meshes']),
                         'vertices': sum(m['vertices'] for m in inf['meshes']), 'triangles': sum(m['triangles'] for m in inf['meshes']),
                         'bones': inf['bones'], 'skinned_meshes': sum(1 for m in inf['meshes'] if m['skinned']),
                         'uv_sets': max([m['uv_sets'] for m in inf['meshes']] or [0]), 'tangents': all(m['has_tangents'] for m in inf['meshes']) if inf['meshes'] else False})
        notes += ['%s: %s' % (fname, w) for w in inf['warnings']]
    # textures used (union over LODs)
    names = []
    for lod in lods:
        g = json.loads((CACHE / lod).read_text())
        for im in g.get('images', []):
            if im['name'] not in names:
                names.append(im['name'])
    role_of_tex = {}
    for mi in mats_info:
        for e in mi['textures']:
            role_of_tex.setdefault(e['name'], e['role'])
    info = {'pipeline': PIPELINE, 'id': a['id'], 'name': a['name'], 'category': a['category'], 'source_pack': str(packdir.relative_to(PACK)).replace('\\', '/'),
            'model_stem': stem, 'lods': lod_info, 'materials': mats_info, 'texture_roles': {n: role_of_tex.get(n, 'other') for n in names},
            'notes': notes, 'textures': {}}
    expr = facb_expressions(packdir)
    if expr:
        info['expressions'] = expr
        info['notes'].append('face parts use an expression atlas; mesh UVs sit in cell (0,0) = e_open / m_close (neutral). '
                             'Other expressions are UV offsets (u, v in atlas cell units) listed in info.expressions; they are not baked.')
    infof.write_text(json.dumps(info, indent=1))
    ensure_textures(a, out, info, force)
    return info


def ensure_textures(a, out, info, force):
    changed = False
    for n, role in info['texture_roles'].items():
        f = out / 'textures' / (n + '.orig.png')
        if f.exists() and not force and n in info['textures']:
            continue
        src = find_texture(n, a['packdir'])
        if src is None:
            info['textures'][n] = {'error': 'texture not found'}
            changed = True
            log('  warn: texture %s not found' % n)
            continue
        try:
            from PIL import Image
            px, meta = decode_nutexb(src)
            Image.fromarray(px, 'RGBA').save(f, optimize=False)
            meta['role'] = role
            meta['source'] = str(src.relative_to(PACK)).replace('\\', '/')
            meta['alpha'] = bool((px[..., 3] != 255).any())
            info['textures'][n] = meta
        except Exception as e:  # noqa: BLE001
            info['textures'][n] = {'error': '%s: %s' % (type(e).__name__, e), 'role': role}
            log('  warn: texture %s: %s' % (n, e))
        changed = True
    if changed:
        (out / 'info.json').write_text(json.dumps(info, indent=1))


# ---------------------------------------------------------------- upscaling
PRIORITY = ROOT / 'build' / 'heavy.priority'


def acquire_lock(tag, wait=7200):
    """Atomic create of build/heavy.lock (stale after 20 min). While build/heavy.priority exists, only take the lock after
    it has been free for 2 straight minutes (photo-capture work has priority)."""
    t0 = time.time()
    free_since = None
    while True:
        held = LOCK.exists()
        if held:
            free_since = None
            try:
                if time.time() - LOCK.stat().st_mtime > 20 * 60:
                    LOCK.unlink()
                    continue
            except OSError:
                pass
        elif free_since is None:
            free_since = time.time()
        if not held and (not PRIORITY.exists() or time.time() - free_since >= 120):
            try:
                fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, ('hd-assets pid=%d %s %s' % (os.getpid(), time.strftime('%Y-%m-%d %H:%M:%S'), tag) + chr(10)).encode())
                os.close(fd)
                return True
            except FileExistsError:
                free_since = None
        if time.time() - t0 > wait:
            return False
        time.sleep(1)


def release_lock():
    try:
        if LOCK.exists() and ('hd-assets pid=%d' % os.getpid()) in LOCK.read_text():
            LOCK.unlink()
    except OSError:
        pass


def lanczos_up(arr, scale, channels):
    import numpy as np
    from PIL import Image
    h, w = arr.shape[:2]
    out = []
    for c in range(channels):
        im = Image.fromarray(arr[..., c], 'L').resize((w * scale, h * scale), Image.LANCZOS)
        out.append(np.asarray(im))
    return np.stack(out, -1)


def up_normal(px, scale):
    import numpy as np
    rgb = lanczos_up(px, scale, 3).astype(np.float32) / 127.5 - 1
    n = np.linalg.norm(rgb, axis=-1, keepdims=True)
    rgb = rgb / np.maximum(n, 1e-6)
    rgb = np.where(n < 1e-6, np.array([0, 0, 1], np.float32), rgb)
    out = np.clip((rgb * 0.5 + 0.5) * 255 + 0.5, 0, 255).astype(np.uint8)
    a = np.full(out.shape[:2] + (1,), 255, np.uint8)
    return np.concatenate([out, a], -1)


def up_channels(px, scale):
    return lanczos_up(px, scale, 4)


def run_realesrgan(model, jobs, scale):
    """jobs: list of (in_png_rgb, out_png). One process for the whole chunk; BelowNormal; killed by PID on timeout."""
    indir, outdir = TMP / ('ai_in_%d' % os.getpid()), TMP / ('ai_out_%d' % os.getpid())
    for d in (indir, outdir):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
    for i, (src, dst) in enumerate(jobs):
        shutil.copyfile(src, indir / ('%03d.png' % i))
    cmd = [str(UPSCALER), '-i', str(indir), '-o', str(outdir), '-n', model, '-s', str(scale), '-f', 'png', '-m', str(UPSCALER.parent / 'models')]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=0x4000)
    try:
        so, _ = p.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
        raise RuntimeError('realesrgan timeout')
    if p.returncode != 0:
        raise RuntimeError('realesrgan failed: ' + (so or '')[-300:])
    for i, (src, dst) in enumerate(jobs):
        o = outdir / ('%03d.png' % i)
        if not o.exists():
            raise RuntimeError('realesrgan produced no output for ' + str(src))
        yield dst, o


def upscale_assets(assets, variants, force, ai_max, chunk, roles=None):
    import numpy as np
    from PIL import Image
    timings_f = BUILD / 'timings.json'
    timings = json.loads(timings_f.read_text()) if timings_f.exists() else {}
    for vid in variants:
        v = VARIANTS[vid]
        scale = v['scale']
        pending = []  # (asset id, tex name, role, orig path, dst path)
        for a in assets:
            out = CACHE / a['id']
            info = json.loads((out / 'info.json').read_text())
            for n, meta in info['textures'].items():
                if 'error' in meta or (roles and meta['role'] not in roles):
                    continue
                dst = out / 'textures' / ('%s.%s.png' % (n, vid))
                if dst.exists() and not force:
                    continue
                pending.append((a['id'], n, meta['role'], out / 'textures' / (n + '.orig.png'), dst, meta))
        log('[%s] %d textures to upscale' % (vid, len(pending)))
        ai, direct = [], []
        for item in pending:
            role, meta = item[2], item[5]
            if role in ('albedo', 'emissive') and max(meta['w'], meta['h']) <= ai_max:
                ai.append(item)
            else:
                direct.append(item)
        t0 = time.time()
        for aid, n, role, orig, dst, meta in direct:
            px = np.asarray(Image.open(orig).convert('RGBA'))
            if role == 'normal':
                o, method = up_normal(px, scale), 'lanczos+renormalize'
            else:
                o, method = up_channels(px, scale), 'lanczos' if role != 'albedo' and role != 'emissive' else 'lanczos (over ai-max)'
            Image.fromarray(o, 'RGBA').save(dst)
            timings.setdefault(vid, {})[aid + '/' + n] = {'method': method, 'src': [meta['w'], meta['h']], 'sec': round(time.time() - t0, 3)}
            t0 = time.time()
        for i in range(0, len(ai), chunk):
            part = ai[i:i + chunk]
            if not acquire_lock('upscale %s' % vid):
                raise SystemExit('could not take heavy.lock')
            try:
                tmpjobs = []
                for aid, n, role, orig, dst, meta in part:
                    px = np.asarray(Image.open(orig).convert('RGBA'))
                    rgbp = TMP / ('ai_src_%d_%d.png' % (os.getpid(), len(tmpjobs)))
                    TMP.mkdir(parents=True, exist_ok=True)
                    Image.fromarray(px[..., :3], 'RGB').save(rgbp)
                    tmpjobs.append((rgbp, dst))
                t0 = time.time()
                res = list(run_realesrgan(v['model'], tmpjobs, scale))
                dt = time.time() - t0
            finally:
                release_lock()
            for (aid, n, role, orig, dst, meta), (d2, o) in zip(part, res):
                rgb = np.asarray(Image.open(o).convert('RGB'))
                px = np.asarray(Image.open(orig).convert('RGBA'))
                if (px[..., 3] != 255).any():
                    al = lanczos_up(px[..., 3:4], scale, 1)
                else:
                    al = np.full(rgb.shape[:2] + (1,), 255, np.uint8)
                Image.fromarray(np.concatenate([rgb, al], -1), 'RGBA').save(dst)
                timings.setdefault(vid, {})[aid + '/' + n] = {'method': 'realesrgan', 'src': [meta['w'], meta['h']], 'sec': round(dt / len(part), 3)}
            log('  %s: %d textures in %.1fs (%.1fs each)' % (vid, len(part), dt, dt / len(part)))
            timings_f.write_text(json.dumps(timings, indent=1))
        timings_f.write_text(json.dumps(timings, indent=1))


# ---------------------------------------------------------------- manifest
def write_manifest():
    entries = []
    vlist = [dict(id=k, label=v['label'], scale=v['scale']) for k, v in VARIANTS.items()]
    for d in sorted(CACHE.iterdir()):
        f = d / 'info.json'
        if not d.is_dir() or not f.exists():
            continue
        info = json.loads(f.read_text())
        aid = info['id']
        lods = ['%s/%s' % (aid, l['file']) for l in info['lods']]
        texs = []
        for n, meta in info['textures'].items():
            if 'error' in meta:
                continue
            vs = {vid: '%s/textures/%s.%s.png' % (aid, n, vid) for vid in VARIANTS if (d / 'textures' / ('%s.%s.png' % (n, vid))).exists()}
            texs.append({'name': n, 'role': meta['role'], 'orig': '%s/textures/%s.orig.png' % (aid, n), 'w': meta['w'], 'h': meta['h'],
                         'format': meta['format'], 'variants': vs})
        mats = [{'name': m['name'], 'albedo': m['albedo'], 'normal': m['normal'], 'mask': m['mask'], 'emissive': m['emissive']} for m in info['materials']]
        entries.append({'id': aid, 'name': info['name'], 'category': info['category'], 'model': lods[0], 'lods': lods, 'materials': mats, 'textures': texs})
    (CACHE / 'manifest.json').write_text(json.dumps({'version': 1, 'variants': vlist, 'assets': entries}, indent=1))
    log('manifest: %d assets' % len(entries))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', default='', help='comma list: pNNN, pNNN:<stem>, otherNNN, florio_*, env:<pack>/<stem>')
    ap.add_argument('--course', default='', help='florio')
    ap.add_argument('--all-pokemon', action='store_true')
    ap.add_argument('--starter', action='store_true', help='the default starter set')
    ap.add_argument('--variants', default='x4_realesrgan,x4_anime', help='comma list or "none"')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--ai-max', type=int, default=1024, help='largest source edge that gets the AI upscale (larger: Lanczos)')
    ap.add_argument('--chunk', type=int, default=4, help='textures per GPU batch (heavy.lock held per batch)')
    ap.add_argument('--roles', default='', help='only upscale these roles, e.g. albedo')
    ap.add_argument('--skip-upscale', action='store_true')
    a = ap.parse_args()
    specs = [s for s in a.assets.split(',') if s]
    if a.starter:
        specs += STARTER
    if a.course:
        specs += COURSES[a.course]
    if a.all_pokemon:
        for d in sorted((PACK / 'Characters' / 'master').iterdir()):
            m = re.fullmatch(r'CH_(\d{3})\.drp', d.name)
            if m and (d / ('SK_CH_%s_00_0_A00.nusrcmdlb' % m.group(1))).exists():
                specs.append('p' + m.group(1))
    if not specs:
        ap.error('no assets given')
    seen, assets = set(), []
    for s in specs:
        r = resolve(s)
        if r['id'] not in seen:
            seen.add(r['id'])
            assets.append(r)
    CACHE.mkdir(parents=True, exist_ok=True)
    TMP.mkdir(parents=True, exist_ok=True)
    for r in assets:
        t = time.time()
        info = build_asset(r, a.force)
        log('[%s] %s: %d LODs, %d textures (%.1fs)' % (r['id'], r['name'], len(info['lods']), len(info['textures']), time.time() - t))
    variants = [] if a.variants == 'none' or a.skip_upscale else [v for v in a.variants.split(',') if v]
    for v in variants:
        if v not in VARIANTS or not VARIANTS[v]['model']:
            ap.error('unknown variant ' + v)
    if variants:
        if not UPSCALER.exists():
            raise SystemExit('missing ' + str(UPSCALER))
        upscale_assets(assets, variants, a.force, a.ai_max, a.chunk, [r for r in a.roles.split(',') if r])
    write_manifest()


if __name__ == '__main__':
    main()
