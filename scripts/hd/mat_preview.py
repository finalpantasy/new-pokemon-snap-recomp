# SPDX-License-Identifier: MIT
r"""Offline lit preview + comparison sheet for material-authored textures (original vs <variant>).

  python -I scripts\hd\mat_preview.py magikarp [--variant x4_mat] [--out analysis\hd\mat_magikarp.png]

Spec-driven (scripts/hd/species/<species>.json, see species_author.py); falls back to material_author.SPECIES for old assets.
Renders the cache model.gltf (bind pose, orthographic, yaw about Y) with a small CPU shader that uses _dif, _nor (OpenGL convention,
glTF tangents) and _par (R roughness, G AO, B spec mask): key + fill light, hemisphere ambient x AO, Blinn-Phong by roughness, a fake
sky/ground reflection. An approximation of the game's shader, only meant to judge the maps against each other.
Sheet rows: two angles and a zoomed close-up, each as original | new "in game today" (only the textures the material's shader binds)
| new "full" (also the cache-only maps, e.g. a normal map for a body whose shader has no normal sampler; shown only when it differs);
last row: 1:1 crops at 4096 of the original (Lanczos-upscaled) and the new _dif (and _nor if any).
"""
import argparse, json, sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import material_author as ma  # noqa: E402

CACHE = ma.CACHE
ROOT = ma.ROOT


def load_cfg(name):
    for f in sorted((HERE / 'species').glob('*.json')):
        s = json.loads(f.read_text())
        if name.lower() in (f.stem.lower(), s['asset'], s['name'].lower()):
            return s
    old = ma.SPECIES[name]
    return dict(asset=name, num=old['num'], name=old['name'], preview={},
                sets=[dict(name=s['name'], mat=s['mat'], dif=s['dif'], nor=s.get('nor'), par=s.get('par')) for s in old['sets']])


def raster_gbuffer(asset, yaw_deg, size, zoom=1.0, center=(0.5, 0.5)):
    prims = ma.load_prims(asset)
    g = json.loads((CACHE / asset / 'model.gltf').read_text())
    buf = (CACHE / asset / g['buffers'][0]['uri']).read_bytes()
    for pr, m in zip(prims, g['meshes']):
        a = m['primitives'][0]['attributes']
        pr['T'] = ma.acc(g, buf, a['TANGENT']).astype(np.float64)
    c, s = np.cos(np.radians(yaw_deg)), np.sin(np.radians(yaw_deg))
    Ry = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    allp = np.concatenate([p['P'] for p in prims]) @ Ry.T
    lo, hi = allp.min(0), allp.max(0)
    ctr = (lo + hi) / 2
    span = max(hi[0] - lo[0], hi[1] - lo[1]) * 1.06
    H = W = size
    zb = np.full((H, W), -1e9, np.float32)
    uv = np.zeros((H, W, 2), np.float32)
    N = np.zeros((H, W, 3), np.float32)
    T = np.zeros((H, W, 4), np.float32)
    mid = np.full((H, W), -1, np.int16)
    for mi, pr in enumerate(prims):
        P = pr['P'] @ Ry.T
        Nn = pr['N'] @ Ry.T
        Tt = np.concatenate([pr['T'][:, :3] @ Ry.T, pr['T'][:, 3:4]], 1)
        X = ((P[:, 0] - ctr[0]) / span - (center[0] - 0.5)) * zoom * W + W / 2
        Y = H / 2 - ((P[:, 1] - ctr[1]) / span + (center[1] - 0.5)) * zoom * H
        Z = P[:, 2]
        for t in pr['I']:
            x, y, z = X[t], Y[t], Z[t]
            x0, x1 = int(max(np.floor(x.min()), 0)), int(min(np.ceil(x.max()), W - 1))
            y0, y1 = int(max(np.floor(y.min()), 0)), int(min(np.ceil(y.max()), H - 1))
            if x1 < x0 or y1 < y0:
                continue
            d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
            if abs(d) < 1e-9:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            l0 = ((y[1] - y[2]) * (xs - x[2]) + (x[2] - x[1]) * (ys - y[2])) / d
            l1 = ((y[2] - y[0]) * (xs - x[2]) + (x[0] - x[2]) * (ys - y[2])) / d
            l2 = 1 - l0 - l1
            mk = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not mk.any():
                continue
            zz = l0 * z[0] + l1 * z[1] + l2 * z[2]
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            ok = mk & (zz > sub)
            if not ok.any():
                continue
            uvt = pr['UV'][t]
            off = np.floor(uvt.mean(0))
            u = l0 * (uvt[0, 0] - off[0]) + l1 * (uvt[1, 0] - off[0]) + l2 * (uvt[2, 0] - off[0])
            v = l0 * (uvt[0, 1] - off[1]) + l1 * (uvt[1, 1] - off[1]) + l2 * (uvt[2, 1] - off[1])
            sel = np.where(ok)
            sub[sel] = zz[sel]
            ww = [l0[sel], l1[sel], l2[sel]]
            uv[y0:y1 + 1, x0:x1 + 1][sel] = np.stack([u[sel], v[sel]], 1)
            N[y0:y1 + 1, x0:x1 + 1][sel] = sum(ww[i][:, None] * Nn[t[i]] for i in range(3))
            T[y0:y1 + 1, x0:x1 + 1][sel] = np.concatenate([sum(ww[i][:, None] * Tt[t[i], :3] for i in range(3)), Tt[t[0], 3:4] * np.ones((len(ww[0]), 1))], 1)
            mid[y0:y1 + 1, x0:x1 + 1][sel] = mi
    names = [p['mat'] for p in prims]
    return uv, N, T, mid, names


def load_tex(asset, num, name, variant):
    td = ma.tex_dir(asset)
    f = td / ('CH_%s_00_0_A00_%s.%s.png' % (num, name, variant))
    if not f.exists():
        f = td / ('CH_%s_00_0_A00_%s.orig.png' % (num, name))
    if not f.exists():
        return None
    return np.asarray(Image.open(f).convert('RGBA')).astype(np.float32) / 255


def samp(tex, uv):
    mx = (uv[..., 0] % 1.0) * tex.shape[1] - 0.5
    my = (uv[..., 1] % 1.0) * tex.shape[0] - 0.5
    return cv2.remap(tex, mx.astype(np.float32), my.astype(np.float32), cv2.INTER_LINEAR, borderMode=cv2.BORDER_WRAP)


def shade(cfg, gb, variant, mode):
    """mode: 'orig' (all originals), 'game' (variant only for the maps the material binds), 'full' (all variant maps)."""
    asset, num = cfg['asset'], cfg['num']
    uv, N, T, mid, names = gb
    H, W = mid.shape
    img = np.zeros((H, W, 3), np.float32)
    img[:] = np.array([0.20, 0.21, 0.24], np.float32)
    Nn = N / np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-9)
    Tt = T[..., :3] - (T[..., :3] * Nn).sum(-1, keepdims=True) * Nn
    Tt /= np.maximum(np.linalg.norm(Tt, axis=-1, keepdims=True), 1e-9)
    Bt = np.cross(Nn, Tt) * T[..., 3:4]
    L = np.array([-0.55, 0.6, 0.6], np.float32); L /= np.linalg.norm(L)
    L2 = np.array([0.7, 0.1, 0.5], np.float32); L2 /= np.linalg.norm(L2)
    V = np.array([0, 0, 1], np.float32)
    for mi, nm in enumerate(names):
        m = mid == mi
        if not m.any():
            continue
        suf = nm.split('_A00_')[-1]
        st = next((s for s in cfg['sets'] if suf in (s['mat'] if isinstance(s['mat'], list) else [s['mat']])), None)
        ig = (st or {}).get('in_game', {})

        def var(key):
            if mode == 'orig':
                return 'orig'
            if mode == 'game' and not ig.get(key, True):
                return 'orig'
            return variant
        dif = load_tex(asset, num, st['dif'] if st else suf + '_dif', var('dif'))
        if dif is None:
            continue
        alb = ma.srgb_to_lin(samp(dif, uv)[..., :3])
        nrm = load_tex(asset, num, st['nor'], var('nor')) if st and st.get('nor') else None
        par = load_tex(asset, num, st['par'], var('par')) if st and st.get('par') and (mode == 'full' or ig.get('par', True)) else None
        n = Nn
        if nrm is not None:
            t = samp(nrm, uv)
            nx, ny = t[..., 0] * 2 - 1, t[..., 1] * 2 - 1
            nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0.02, 1))
            n = Tt * nx[..., None] + Bt * ny[..., None] + Nn * nz[..., None]
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
        if par is not None:
            p = samp(par, uv)
            rough, ao, spec = np.clip(p[..., 0], 0.05, 1), p[..., 1], p[..., 2]
        else:
            rough, ao, spec = np.full((H, W), 0.61, np.float32), np.ones((H, W), np.float32), np.ones((H, W), np.float32)
        ndl = np.clip((n * L).sum(-1), 0, 1)
        ndl2 = np.clip((n * L2).sum(-1), 0, 1)
        hemi = 0.5 + 0.5 * n[..., 1]
        amb = (0.18 + 0.30 * hemi) * ao
        diff = alb * (amb + 1.05 * ndl + 0.22 * ndl2)[..., None]
        Hh = L + V; Hh /= np.linalg.norm(Hh)
        nh = np.clip((n * Hh).sum(-1), 0, 1)
        shin = np.clip(2.0 / np.maximum(rough ** 4, 1e-3) - 2, 4, 400)
        sp = (nh ** shin) * (shin + 8) / 25.0 * ndl
        refl = 2 * (n[..., 2:3]) * n - V
        sky = np.clip(refl[..., 1] * 0.5 + 0.5, 0, 1)
        env = (0.25 + 0.75 * sky)[..., None] * np.array([0.55, 0.65, 0.85], np.float32) * (1 - 0.5 * (1 - sky))[..., None]
        fres = 0.04 + 0.96 * (1 - np.clip(n[..., 2], 0, 1)) ** 5
        gl = ((1 - rough) ** 2 * (0.15 + fres) * spec * ao)[..., None]
        col = diff + (sp * spec)[..., None] * 0.8 + env * gl
        img[m] = col[m]
    return np.clip(ma.lin_to_srgb(img), 0, 1)


def to_im(a, size):
    return Image.fromarray((cv2.resize(a, (size, size), interpolation=cv2.INTER_AREA) * 255 + 0.5).astype(np.uint8))


def crop_pair(cfg, name, variant, cu, cv_, size):
    td = ma.tex_dir(cfg['asset'])
    o = td / ('CH_%s_00_0_A00_%s.orig.png' % (cfg['num'], name))
    m = td / ('CH_%s_00_0_A00_%s.%s.png' % (cfg['num'], name, variant))
    if not m.exists():
        return None, None
    mi = Image.open(m).convert('RGB')
    S = mi.width
    x0 = int(np.clip(cu * S - size / 2, 0, S - size))
    y0 = int(np.clip(cv_ * S - size / 2, 0, S - size))
    mc = mi.crop((x0, y0, x0 + size, y0 + size))
    oc = Image.open(o).convert('RGB').resize((S, S), Image.LANCZOS).crop((x0, y0, x0 + size, y0 + size)) if o.exists() else None
    return oc, mc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('species')
    ap.add_argument('--variant', default='x4_mat')
    ap.add_argument('--panel', type=int, default=640)
    ap.add_argument('--out', default=None)
    ap.add_argument('--only-closeup', action='store_true', help='quick look: write only the close-up row')
    a = ap.parse_args()
    cfg = load_cfg(a.species)
    pv = cfg.get('preview', {})
    P = a.panel
    full_differs = any(not all(s.get('in_game', {}).get(k, True) for k in ('dif', 'nor', 'par')) for s in cfg['sets'])
    modes = ['orig', 'game'] + (['full'] if full_differs else [])
    rows = []
    views = [] if a.only_closeup else [('yaw %d' % y, dict(yaw_deg=y)) for y in pv.get('yaws', [28, -60])]
    cu = pv.get('closeup', {'yaw': 28, 'center': [0.5, 0.5], 'zoom': 3})
    views.append(('close-up x%.1f' % cu['zoom'], dict(yaw_deg=cu['yaw'], zoom=cu['zoom'], center=tuple(cu['center']))))
    for label, kw in views:
        gbuf = raster_gbuffer(cfg['asset'], size=int(P * 1.5), **kw)
        rows.append((label, [to_im(shade(cfg, gbuf, a.variant, md), P) for md in modes]))
    ig_txt = '; '.join('%s: %s' % (s['name'], '+'.join(k for k in ('dif', 'par', 'nor') if s.get(k) and s.get('in_game', {}).get(k, True)))
                       for s in cfg['sets'])
    titles = {'orig': 'original', 'game': 'NEW, IN GAME TODAY (%s)' % ig_txt,
              'full': 'NEW, FULL (adds cache-only maps; needs material swap)'}
    # crops at 4096, 1:1
    st = next(s for s in cfg['sets'] if s['name'] == pv.get('crop_set', cfg['sets'][0]['name']))
    cuv = pv.get('crop', [0.5, 0.5])
    oc, mc = crop_pair(cfg, st['dif'], a.variant, cuv[0], cuv[1], P)
    crops = [(oc, 'orig %s (Lanczos to 4096), 1:1' % st['dif']), (mc, '%s %s, 1:1 at 4096' % (a.variant, st['dif']))]
    if st.get('nor'):
        _, nc = crop_pair(cfg, st['nor'], a.variant, cuv[0], cuv[1], P)
        if nc is not None:
            crops.append((nc, '%s %s%s, 1:1' % (a.variant, st['nor'], '' if st.get('in_game', {}).get('nor', True) else ' (cache-only)')))
    ncol = max(len(modes), len(crops))
    hdr = 26
    Wd = ncol * P
    Ht = (len(rows) + (0 if a.only_closeup else 1)) * (P + hdr) + 30
    sheet = Image.new('RGB', (Wd, Ht), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    d.text((8, 8), '%s material re-texture (%s); lit preview is an approximation of the game shader' % (cfg['name'], a.variant), fill=(255, 255, 255))
    y = 30
    for label, ims in rows:
        for i, (im, md) in enumerate(zip(ims, modes)):
            sheet.paste(im, (i * P, y + hdr))
            d.text((i * P + 6, y + 6), '%s | %s' % (label, titles[md]), fill=(255, 230, 120) if md == 'game' else (255, 255, 255))
        y += P + hdr
    if not a.only_closeup:
        for i, (im, lab) in enumerate(crops):
            if im is not None:
                sheet.paste(im, (i * P, y + hdr))
            d.text((i * P + 6, y + 6), lab, fill=(255, 255, 255))
    out = Path(a.out) if a.out else ROOT / 'analysis' / 'hd' / ('mat_%s.png' % cfg['name'].lower())
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main()
