# SPDX-License-Identifier: MIT
r"""Material-aware re-texture of Pokemon body textures: new _dif, _nor and _par together, consistent with each other.

  python -I scripts\hd\material_author.py author p006_00 [--scale 4] [--feature 1]   # writes <tex>.x4_mat.png (or x1_mat for --scale 1)
  python -I scripts\hd\material_author.py pack p006_00 --root build\mods_mat --pack mat-pichu [--variant x1_mat]

Method (docs/hd-assets.md "Material re-texture"):
  1. UV islands and a per-island fur-flow guess come from the cache model.gltf (UV connectivity, 3D tangent plane, gravity+back direction).
  2. Region masks: k-means of the albedo in Lab, each cluster mapped to a material class by a per-species colour rule (pokemon_materials.json regions).
  3. Per class: height/strand field (line-integral convolution along the flow, stopped at island borders), then albedo detail
     (multiplicative, mean locked to the original at low frequency), normal (original XY + detail XY, clamped), _par (R rough, G AO, B spec, A metal where the format has A).
  4. Eye and Mouth textures are upscale only (no material detail).
Output files are game-derived and stay in build/hd/cache (git-ignored). Normal convention written: R = +X right, G = +Y up (image rows decrease), linear.
"""
import argparse, json, sys, time
from pathlib import Path

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage as ndi
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
ROOT = HERE.parents[1]
CACHE = ROOT / 'build' / 'hd' / 'cache'
cv2.setNumThreads(4)

# ---------------------------------------------------------------- species config
def _lab_rules(fn):
    return fn

SPECIES = {
    'p006_00': dict(name='Pichu', num='006', default='fur', sets=[
        dict(name='Body', mat='Body', dif='Body_dif', nor='Body_nor', par='Body_par',
             rule=lambda L, C, h, rgb: 'smooth_skin' if L < 30 else 'fur'),
        dict(name='Cheek', mat='Cheek', dif='Cheek_dif', nor=None, par='Cheek_par',
             rule=lambda L, C, h, rgb: 'fur'),
    ], upscale_only=['Eye_dif', 'Mouth_dif']),
    'p001_00': dict(name='Pikachu', num='001', default='fur', sets=[
        dict(name='Body', mat='Body', dif='Body_dif', nor='Body_nor', par='Body_par',
             rule=lambda L, C, h, rgb: 'fur'),
        dict(name='Cheek', mat='Cheek', dif='Cheek_dif', nor=None, par=None,
             rule=lambda L, C, h, rgb: 'fur'),
    ], upscale_only=['Eye_dif', 'Mouth_dif']),
    'p032_00': dict(name='Bidoof', num='032', default='fur', sets=[
        dict(name='Body', mat='Body', dif='Body_dif', nor='Body_nor', par='Body_par',
             rule=lambda L, C, h, rgb: 'none' if L < 14 else 'fur'),
    ], upscale_only=['Eye_dif']),
    'p015_00': dict(name='Magikarp', num='015', default='scales', sets=[
        dict(name='Body', mat='Body', dif='Body_dif', nor='Body_nor', par='Body_par',
             rule=lambda L, C, h, rgb: 'scales' if (C > 35 and (h < 55 or h > 340) and 30 < L < 75) else 'smooth_skin'),
    ], upscale_only=['Eye_dif']),
    'p107_00': dict(name='Steelix', num='107', default='metal', sets=[
        dict(name='Body', mat='Body', dif='Body_dif', nor='Body_nor', par='Body_par',
             rule=lambda L, C, h, rgb: 'none' if L < 14 else 'metal'),
    ], upscale_only=['Eye_dif']),
}

# ---------------------------------------------------------------- io helpers
def tex_dir(asset):
    return CACHE / asset / 'textures'


def load_png(path):
    return np.asarray(Image.open(path).convert('RGBA'))


def save_png(arr, path):
    Image.fromarray(np.clip(arr + 0.5, 0, 255).astype(np.uint8) if arr.dtype != np.uint8 else arr, 'RGBA').save(path, compress_level=1)


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).astype(np.float32)


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055).astype(np.float32)


def up(a, out, interp=cv2.INTER_CUBIC):
    if a.shape[0] == out:
        return a.astype(np.float32)
    return cv2.resize(a.astype(np.float32), (out, out), interpolation=interp)


# ---------------------------------------------------------------- mesh, islands, flow
def acc(g, buf, i):
    a = g['accessors'][i]
    bv = g['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    dt = {5126: '<f4', 5125: '<u4', 5123: '<u2'}[a['componentType']]
    return np.frombuffer(buf, dt, a['count'] * n, bv.get('byteOffset', 0) + a.get('byteOffset', 0)).reshape(a['count'], n)


def load_prims(asset):
    base = CACHE / asset
    g = json.loads((base / 'model.gltf').read_text())
    buf = (base / g['buffers'][0]['uri']).read_bytes()
    out = []
    for m in g['meshes']:
        p = m['primitives'][0]
        a = p['attributes']
        out.append(dict(mat=g['materials'][p['material']]['name'], P=acc(g, buf, a['POSITION']).astype(np.float64),
                        N=acc(g, buf, a['NORMAL']).astype(np.float64), UV=acc(g, buf, a['TEXCOORD_0']).astype(np.float64),
                        I=acc(g, buf, p['indices'])[:, 0].astype(np.int64).reshape(-1, 3)))
    return out


def tri_flow(pr, d):
    """per triangle: UV-plane direction (fu, fv) of 3D direction d projected on the surface, and confidence |d_tangent|/|d|."""
    P, N, UV, I = pr['P'], pr['N'], pr['UV'], pr['I']
    p0, p1, p2 = P[I[:, 0]], P[I[:, 1]], P[I[:, 2]]
    t0, t1, t2 = UV[I[:, 0]], UV[I[:, 1]], UV[I[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    a1, a2 = t1 - t0, t2 - t0
    det = a1[:, 0] * a2[:, 1] - a1[:, 1] * a2[:, 0]
    ok = np.abs(det) > 1e-14
    det = np.where(ok, det, 1.0)[:, None]
    Pu = (e1 * a2[:, 1:2] - e2 * a1[:, 1:2]) / det
    Pv = (e2 * a1[:, 0:1] - e1 * a2[:, 0:1]) / det
    n = N[I[:, 0]] + N[I[:, 1]] + N[I[:, 2]]
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    dt = d[None, :] - (n @ d)[:, None] * n
    A, B, C = (Pu * Pu).sum(1), (Pu * Pv).sum(1), (Pv * Pv).sum(1)
    r1, r2 = (Pu * dt).sum(1), (Pv * dt).sum(1)
    D = A * C - B * B
    ok &= np.abs(D) > 1e-24
    D = np.where(ok, D, 1.0)
    fu = (C * r1 - B * r2) / D
    fv = (A * r2 - B * r1) / D
    conf = np.where(ok, np.linalg.norm(dt, axis=1) / np.linalg.norm(d), 0.0)
    return fu, fv, conf


def build_geometry(asset, mat_suffix, R, flow_dir=(0.0, -1.0, -0.45)):
    """raster at R x R: island label (ext = nearest island everywhere), coverage mask, doubled-angle flow vectors.
    Returns lab_ext int32, cov bool, flow (R,R,2) unit direction (oriented downward in the image), conf (R,R)."""
    prims = [p for p in load_prims(asset) if p['mat'].endswith('_' + mat_suffix)]
    if not prims:
        return None
    d = np.array(flow_dir, float)
    d /= np.linalg.norm(d)
    tri_id = np.full((R, R), -1, np.int32)
    labs, vec = [], []
    base = 0
    off = 0
    for pr in prims:
        UV, I = pr['UV'], pr['I']
        key = np.round(UV * 1e5).astype(np.int64)
        k = key[:, 0] * 4000003 + key[:, 1]
        uniq, inv = np.unique(k, return_inverse=True)
        r = np.concatenate([inv[I[:, 0]], inv[I[:, 1]]])
        c = np.concatenate([inv[I[:, 1]], inv[I[:, 2]]])
        g = coo_matrix((np.ones(len(r)), (r, c)), shape=(len(uniq), len(uniq)))
        _, comp = connected_components(g, directed=False)
        labs.append(comp[inv[I[:, 0]]] + 1 + base)
        base += comp.max() + 1
        fu, fv, conf = tri_flow(pr, d)
        th = np.arctan2(fv, fu)
        vec.append(np.stack([np.cos(2 * th) * conf, np.sin(2 * th) * conf, conf], 1))
        uvt = UV[I]
        uvt = uvt - np.floor(uvt.mean(1, keepdims=True))          # material repeat/offset puts some meshes at u or v outside 0..1
        px = uvt * R
        for ti in range(len(I)):
            pts = np.round(px[ti] * 16).astype(np.int32)
            cv2.fillConvexPoly(tri_id, pts, int(ti + off), lineType=cv2.LINE_8, shift=4)
        off += len(I)
    labs = np.concatenate(labs).astype(np.int32)
    vec = np.concatenate(vec)
    cov = tri_id >= 0
    tid = np.where(cov, tri_id, 0)
    lab = np.where(cov, labs[tid], 0).astype(np.int32)
    v = vec[tid] * cov[..., None]
    # normalized convolution of the doubled-angle vectors inside coverage
    sg = max(R / 128.0, 3.0)
    cs = cv2.GaussianBlur((v[..., 0] * 1.0).astype(np.float32), (0, 0), sg)
    sn = cv2.GaussianBlur((v[..., 1]).astype(np.float32), (0, 0), sg)
    wn = cv2.GaussianBlur(cov.astype(np.float32), (0, 0), sg)
    cf = cv2.GaussianBlur((v[..., 2]).astype(np.float32), (0, 0), sg)
    mag = np.hypot(cs, sn) / np.maximum(cf, 1e-6)           # orientation coherence
    conf = np.clip(cf / np.maximum(wn, 1e-6) * 1.5, 0, 1) * np.clip(mag * 1.5, 0, 1)
    th = 0.5 * np.arctan2(sn, cs)
    # nearest-island extension for gutters
    _, (iy, ix) = ndi.distance_transform_edt(~cov, return_indices=True)
    lab_ext = lab[iy, ix]
    th = th[iy, ix]
    conf = conf[iy, ix]
    fx, fy = np.cos(th), np.sin(th)
    s = np.where(fy < 0, -1.0, 1.0)                          # orient downward in the image
    fx, fy = fx * s, fy * s
    return lab_ext.astype(np.int32), cov, np.stack([fx, fy], -1).astype(np.float32), conf.astype(np.float32)


# ---------------------------------------------------------------- field generators
def lic(noise, flow_lo, lab, half_len_px, step_px, rng_angle=None):
    """line integral convolution of `noise` (HxW float32) along flow_lo (r,r,2, lo-res) inside the same island label."""
    H = noise.shape[0]
    r = flow_lo.shape[0]
    ratio = r / H
    xs = np.arange(H, dtype=np.float32)
    x0 = np.broadcast_to(xs[None, :], (H, H)).copy()
    y0 = np.broadcast_to(xs[:, None], (H, H)).copy()
    labf = lab.astype(np.float32)
    total = noise.copy()
    wsum = np.ones_like(noise)
    n = int(round(half_len_px / step_px))
    fl = [np.ascontiguousarray(flow_lo[..., 0]), np.ascontiguousarray(flow_lo[..., 1])]
    for sign in (1.0, -1.0):
        px, py = x0.copy(), y0.copy()
        alive = np.ones((H, H), np.float32)
        pdx = np.zeros((H, H), np.float32)
        pdy = np.zeros((H, H), np.float32)
        for i in range(n):
            mx, my = px * ratio, py * ratio
            fx = cv2.remap(fl[0], mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            fy = cv2.remap(fl[1], mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            if i:
                sgn = np.where(fx * pdx + fy * pdy < 0, -1.0, 1.0).astype(np.float32)
                fx, fy = fx * sgn, fy * sgn
            pdx, pdy = fx, fy
            px = px + sign * step_px * fx
            py = py + sign * step_px * fy
            l = cv2.remap(labf, px, py, cv2.INTER_NEAREST, borderMode=cv2.BORDER_REPLICATE)
            alive *= (l == labf)
            total += alive * cv2.remap(noise, px, py, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            wsum += alive
    return total / wsum


def std_norm(h, sg_hp):
    h = h - cv2.GaussianBlur(h, (0, 0), sg_hp)
    return h / max(float(h.std()), 1e-6)


def grad_normal(h, strength):
    """unit-std height -> (nx, ny) with total std = strength (OpenGL: +Y is image up)."""
    gx = cv2.Sobel(h, cv2.CV_32F, 1, 0, ksize=3) / 8.0
    gy = cv2.Sobel(h, cv2.CV_32F, 0, 1, ksize=3) / 8.0
    s = strength / max(float(np.sqrt((gx * gx + gy * gy).mean() / 2.0)), 1e-9)
    return -gx * s, gy * s


def jitter_flow(flow_lo, amp, seed, sigma_px):
    """rotate the lo-res flow by a smooth random angle (curl, wave) so strands are not perfectly combed."""
    r = flow_lo.shape[0]
    a = np.random.default_rng(seed).standard_normal((r, r)).astype(np.float32)
    a = cv2.GaussianBlur(a, (0, 0), sigma_px)
    a = a / max(float(a.std()), 1e-6) * amp
    c, s_ = np.cos(a), np.sin(a)
    fx, fy = flow_lo[..., 0], flow_lo[..., 1]
    return np.stack([fx * c - fy * s_, fx * s_ + fy * c], -1).astype(np.float32)


def strand_fields(out, lab, flow_lo, conf_lo, ctx):
    k = out / 4096.0 * ctx['feature']
    rng = np.random.default_rng(ctx['seed'])
    nz = rng.standard_normal((out, out)).astype(np.float32)
    nz = cv2.GaussianBlur(nz, (0, 0), max(0.5, 0.9 * k))
    fl_f = jitter_flow(flow_lo, 0.28, ctx['seed'] + 1, max(2.0, 16.0 * k * flow_lo.shape[0] / out))
    fine = std_norm(lic(nz, fl_f, lab, 15 * k, max(1.0, 2.0 * k)), 12 * k)
    iso = std_norm(cv2.GaussianBlur(rng.standard_normal((out, out)).astype(np.float32), (0, 0), max(0.6, 1.2 * k)), 12 * k)
    fine = std_norm(fine * 0.95 + iso * 0.2, 12 * k)
    lo = max(out // 4, 64)
    nz2 = cv2.resize(rng.standard_normal((lo, lo)).astype(np.float32), (out, out), interpolation=cv2.INTER_CUBIC)
    nz2 = cv2.GaussianBlur(nz2, (0, 0), max(0.5, 1.5 * k))
    fl_c = jitter_flow(flow_lo, 0.2, ctx['seed'] + 2, max(3.0, 40.0 * k * flow_lo.shape[0] / out))
    clump = std_norm(lic(nz2, fl_c, lab, 45 * k, max(2.0, 5.0 * k)), 40 * k)
    return fine, clump


def cell_fields(out, cell_px, seed, jitter=0.35):
    """jittered hex-ish grid Voronoi: returns F1 (distance to nearest centre / cell), edge (F2-F1), cell id hash in [0,1)."""
    rng = np.random.default_rng(seed)
    n = int(out / cell_px) + 3
    jx = rng.random((n + 2, n + 2)).astype(np.float32)
    jy = rng.random((n + 2, n + 2)).astype(np.float32)
    hv = rng.random((n + 2, n + 2)).astype(np.float32)
    ys, xs = np.mgrid[0:out, 0:out].astype(np.float32)
    ux, uy = xs / cell_px + 0.0, ys / cell_px
    # offset every other row by half a cell (scale-like rows)
    ci = np.floor(uy).astype(np.int32)
    ux = ux + 0.5 * (ci & 1)
    cj = np.floor(ux).astype(np.int32)
    fx, fy = ux - cj, uy - ci
    best1 = np.full((out, out), 9.0, np.float32)
    best2 = np.full((out, out), 9.0, np.float32)
    cid = np.zeros((out, out), np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            ii = np.clip(ci + dy + 1, 0, n + 1)
            jj = np.clip(cj + dx + 1, 0, n + 1)
            # centre of the neighbour cell relative to the pixel's cell origin
            rowshift = 0.5 * (((ci + dy) & 1) - (ci & 1))
            cx = dx + 0.5 + (jx[ii, jj] - 0.5) * jitter * 2 + rowshift
            cy = dy + 0.5 + (jy[ii, jj] - 0.5) * jitter * 2
            d = np.hypot(fx - cx, fy - cy)
            new1 = d < best1
            best2 = np.where(new1, best1, np.minimum(best2, d))
            cid = np.where(new1, hv[ii, jj], cid)
            best1 = np.where(new1, d, best1)
    return best1, best2 - best1, cid


def smooth_fields(out, ctx, pore_density=1 / 150.0):
    k = out / 4096.0 * ctx['feature']
    rng = np.random.default_rng(ctx['seed'] + 7)
    p = (rng.random((out, out)) < pore_density / max(k * k, 1e-3) * (4.0 if out <= 1024 else 1.0)).astype(np.float32)
    pores = cv2.GaussianBlur(p, (0, 0), max(0.8, 2.0 * k))
    pores /= max(float(pores.max()), 1e-6)
    n1 = cv2.GaussianBlur(rng.standard_normal((out, out)).astype(np.float32), (0, 0), max(0.6, 1.6 * k))
    n2 = cv2.GaussianBlur(rng.standard_normal((out, out)).astype(np.float32), (0, 0), max(1.0, 6.0 * k))
    h = std_norm(n1, 30 * k) * 0.6 + std_norm(n2, 60 * k) * 0.4 - 0.8 * pores
    return std_norm(h, 80 * k), pores


# ---------------------------------------------------------------- classes
def author_classes(out, classes, wts, lab, flow_lo, conf_lo, ctx):
    """returns dict of full-res field accumulators blended by the class weights."""
    F = {k: np.zeros((out, out), np.float32) for k in ('nx', 'ny', 'alb', 'sat', 'cav', 'rt', 'rw', 'rv', 'bm', 'metal')}
    conf = cv2.resize(conf_lo, (out, out), interpolation=cv2.INTER_LINEAR)
    conf_s = np.clip((conf - 0.1) / 0.5, 0.25, 1.0)             # keep it subtle where the flow guess is unsure
    cache = {}
    for c in classes:
        w = wts[c]
        if w.max() < 0.01:
            continue
        t0 = time.time()
        if c == 'fur':
            fine, clump = strand_fields(out, lab, flow_lo, conf_lo, ctx)
            h = fine * 0.7 + clump * 0.55
            nx, ny = grad_normal(h, 0.08)
            cav = np.clip(0.55 - 0.45 * (fine + 0.9 * clump), 0, 1.2)
            d = dict(nx=nx * conf_s, ny=ny * conf_s, alb=(0.04 * fine + 0.035 * clump) * conf_s, sat=0.18 * cav,
                     cav=0.2 * cav, rt=0.72, rw=0.4, rv=0.05 * (fine + clump), bm=0.0 * h + 1.0 - 0.07 * cav, metal=0 * h)
            cache['fur_h'] = h
        elif c == 'smooth_skin':
            h, pores = smooth_fields(out, ctx)
            nx, ny = grad_normal(h, 0.035)
            d = dict(nx=nx, ny=ny, alb=0.02 * h - 0.04 * pores, sat=0.05 * pores, cav=0.08 * pores, rt=0.62, rw=0.7,
                     rv=0.03 * h, bm=0.65 + 0.05 * h, metal=0 * h)
        elif c == 'scales':
            k = out / 4096.0 * ctx['feature']
            cell = ctx.get('scale_cell', 36.0) * k * (1.0 if out > 1024 else 1.0)
            f1, edge, cid = cell_fields(out, cell, ctx['seed'] + 3, jitter=0.1)
            e = np.clip(edge / 0.18, 0, 1)                        # 0 at the cell border
            dome = np.clip(1 - f1 / 0.8, 0, 1)
            h = cv2.GaussianBlur((e * 0.7 + dome * 0.8).astype(np.float32), (0, 0), max(0.7, 1.0 * k))
            h = (h - h.mean()) / max(h.std(), 1e-6)
            nx, ny = grad_normal(h, 0.30)
            d = dict(nx=nx, ny=ny, alb=0.045 * (cid - 0.5) * 2 + 0.05 * (dome - 0.4) - 0.07 * (1 - e), sat=0.08 * (1 - e),
                     cav=0.30 * (1 - e), rt=0.34, rw=0.6, rv=0.05 * (cid - 0.5), bm=0.85 + 0.3 * dome, metal=0 * h)
        elif c == 'metal':
            k = out / 4096.0 * ctx['feature']
            rng = np.random.default_rng(ctx['seed'] + 11)
            nzb = rng.standard_normal((out, out)).astype(np.float32)
            nzb = cv2.GaussianBlur(nzb, (0, 0), max(0.5, 0.8 * k))
            # brushed: horizontal-ish flow per island (random angle per island label), hammered: low-frequency blotches
            nl = int(lab.max()) + 1
            ang = np.random.default_rng(ctx['seed'] + 5).uniform(-0.35, 0.35, nl).astype(np.float32)
            a = ang[lab]
            fl = np.stack([np.cos(a), np.sin(a)], -1)
            fl_lo = cv2.resize(fl, (flow_lo.shape[0], flow_lo.shape[0]), interpolation=cv2.INTER_NEAREST)
            brushed = std_norm(lic(nzb, fl_lo, lab, 60 * k, max(2.0, 4.0 * k)), 30 * k)
            ham = std_norm(cv2.GaussianBlur(rng.standard_normal((out, out)).astype(np.float32), (0, 0), 14 * k), 80 * k)
            h = 0.8 * brushed + 0.5 * ham
            nx, ny = grad_normal(h, 0.07)
            d = dict(nx=nx, ny=ny, alb=0.05 * brushed + 0.035 * ham, sat=0.0 * h, cav=0.1 * np.clip(-brushed, 0, 2), rt=0.2, rw=0.7,
                     rv=0.045 * brushed + 0.03 * ham, bm=1.0 + 0.0 * h, metal=1.0 + 0 * h)
        else:                                                    # 'none': upscale only
            continue
        for key in F:
            v = d[key]
            F[key] += w * (v if isinstance(v, np.ndarray) else np.float32(v))
        print('   class %-11s weight mean %.2f  %.1fs' % (c, float(w.mean()), time.time() - t0), flush=True)
        # rt / rw need class-weighted target mixing: handled by accumulating rt*rw and rw below
    return F


# ---------------------------------------------------------------- regions
def regions(rgb_src, cov_src, rule, k=6, seed=1):
    """k-means in Lab over the covered albedo. Returns label image (src res), {class: mask}, table."""
    lab = cv2.cvtColor(rgb_src.astype(np.float32), cv2.COLOR_RGB2Lab)
    ys, xs = np.nonzero(cov_src)
    sel = np.random.default_rng(seed).choice(len(ys), min(40000, len(ys)), replace=False) if len(ys) else np.array([], int)
    samp = lab[ys[sel], xs[sel]].astype(np.float32)
    cv2.setRNGSeed(seed)
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.5)
    _, _, cen = cv2.kmeans(samp, k, None, crit, 3, cv2.KMEANS_PP_CENTERS)
    H, W = lab.shape[:2]
    flat = lab.reshape(-1, 3)
    d = ((flat[:, None, :] - cen[None]) ** 2).sum(-1)
    idx = d.argmin(1).reshape(H, W)
    table = []
    cls = []
    for i in range(k):
        L, a, b = cen[i]
        C = float(np.hypot(a, b))
        hue = float(np.degrees(np.arctan2(b, a)) % 360)
        rgbc = cv2.cvtColor(cen[i].reshape(1, 1, 3).astype(np.float32), cv2.COLOR_Lab2RGB).reshape(3)
        c = rule(float(L), C, hue, rgbc)
        cls.append(c)
        table.append((i, round(float(L), 1), round(C, 1), round(hue), c, round(float((idx == i).mean()), 3)))
    return idx, cls, table


# ---------------------------------------------------------------- main authoring
def author_set(asset, cfg, st, out, ctx, outputs):
    """st = set config; writes <tex>.x{scale}_mat.png into the cache."""
    td = tex_dir(asset)
    pre = 'CH_%s_00_0_A00_' % cfg['num']
    tag = ctx['tag']
    dif = load_png(td / (pre + st['dif'] + '.orig.png'))
    src = dif.shape[0]
    R = 2048
    print(' set %s: src %d -> %d' % (st['name'], src, out), flush=True)
    geo = build_geometry(asset, st['mat'], R)
    if geo is None:
        print('  no mesh uses material', st['mat'])
        return
    lab_r, cov, flow, conf = geo
    lab = cv2.resize(lab_r.astype(np.float32), (out, out), interpolation=cv2.INTER_NEAREST).astype(np.int32)
    cov_src = cv2.resize(cov.astype(np.uint8), (src, src), interpolation=cv2.INTER_NEAREST) > 0
    rgb_src = dif[..., :3].astype(np.float32) / 255
    idx, cls, table = regions(rgb_src, cov_src, st['rule'])
    print('  regions (id, L, C, hue, class, share):', table, flush=True)
    classes = sorted(set(cls))
    wts = {}
    for c in classes:
        m = np.isin(idx, [i for i, cc in enumerate(cls) if cc == c]).astype(np.float32)
        m = cv2.GaussianBlur(m, (0, 0), max(1.0, src / 512.0))
        wts[c] = np.clip(up(m, out), 0, 1)
    ctx['regions'] = (idx, cls)
    # base images
    base_lin = srgb_to_lin(up(rgb_src, out, cv2.INTER_LANCZOS4).clip(0, 1))
    alpha = np.clip(up(dif[..., 3].astype(np.float32), out, cv2.INTER_LINEAR), 0, 255)
    F = author_classes(out, classes, wts, lab, flow, conf, ctx)
    # ---- roughness target mixing is done per class below (simple weighted mean of targets)
    rt = np.zeros((out, out), np.float32)
    rw = np.zeros((out, out), np.float32)
    for c in classes:
        tgt = dict(fur=(0.72, 0.4), smooth_skin=(0.62, 0.7), scales=(0.34, 0.6), metal=(0.2, 0.7)).get(c)
        if tgt is None:
            continue
        rt += wts[c] * tgt[0]
        rw += wts[c] * tgt[1]
    wsum = np.maximum(sum(wts[c] for c in classes if c != 'none'), 1e-3)
    # ---- albedo
    modulated = base_lin * (1.0 + F['alb'][..., None])
    gam = 1.0 + F['sat'][..., None]
    modulated = np.clip(modulated, 1e-6, None) ** gam
    modulated = modulated * (1.0 - 0.0 * F['cav'][..., None])
    sg = 2.5 * out / 1024.0
    lp_b = cv2.GaussianBlur(base_lin, (0, 0), sg)
    lp_m = cv2.GaussianBlur(modulated.astype(np.float32), (0, 0), sg)
    ratio = np.clip(lp_b / np.maximum(lp_m, 1e-5), 0.8, 1.25)
    final_lin = np.where(lp_b < 2e-3, base_lin, modulated * ratio)
    alb_out = np.empty((out, out, 4), np.uint8)
    alb_out[..., :3] = np.clip(lin_to_srgb(final_lin) * 255 + 0.5, 0, 255).astype(np.uint8)
    alb_out[..., 3] = alpha.astype(np.uint8)
    save_png(alb_out, td / (pre + st['dif'] + '.%s.png' % tag))
    mean_diff = np.abs(rgb_src.reshape(-1, 3).mean(0, dtype=np.float64) * 255 - alb_out[..., :3].reshape(-1, 3).mean(0, dtype=np.float64))
    print('  dif written, mean RGB drift vs source: %s' % np.round(mean_diff, 2), flush=True)
    del modulated, lp_b, lp_m, ratio, final_lin
    # ---- normal
    if st.get('nor'):
        norig = load_png(td / (pre + st['nor'] + '.orig.png')) if (td / (pre + st['nor'] + '.orig.png')).exists() else None
        if norig is not None:
            nxy0 = up(norig[..., :2].astype(np.float32) / 255 * 2 - 1, out) if norig.shape[0] != out else norig[..., :2].astype(np.float32) / 255 * 2 - 1
            bconst = float(norig[..., 2].std()) < 1.0
            bfill = float(norig[..., 2].mean())
        else:
            nxy0 = np.zeros((out, out, 2), np.float32)
            bconst, bfill = False, 255.0
        nx = nxy0[..., 0] + F['nx']
        ny = nxy0[..., 1] + F['ny']
        mag = np.hypot(nx, ny)
        sc = np.where(mag > 0.9, 0.9 / np.maximum(mag, 1e-6), 1.0)
        nx, ny = nx * sc, ny * sc
        nrm = np.empty((out, out, 4), np.uint8)
        nrm[..., 0] = np.clip((nx * 0.5 + 0.5) * 255 + 0.5, 0, 255)
        nrm[..., 1] = np.clip((ny * 0.5 + 0.5) * 255 + 0.5, 0, 255)
        nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
        nrm[..., 2] = int(round(bfill)) if bconst else np.clip((nz * 0.5 + 0.5) * 255 + 0.5, 0, 255)
        nrm[..., 3] = 255
        suffix = '' if norig is not None else '  (NEW file: the original has no normal)'
        save_png(nrm, td / (pre + st['nor'] + '.%s.png' % tag))
        print('  nor written%s; std xy %.3f' % (suffix, float(np.hypot(nx, ny).std())), flush=True)
        outputs.append(st['nor'])
        del nrm
    # ---- params (ROSM)
    if st.get('par'):
        pp = td / (pre + st['par'] + '.orig.png')
        if pp.exists():
            p0 = load_png(pp)
            par = up(p0[..., :3].astype(np.float32) / 255, out)
            a0 = up(p0[..., 3].astype(np.float32) / 255, out, cv2.INTER_LINEAR)
            has_a = bool(p0[..., 3].std() > 0.5)
        else:
            par = np.broadcast_to(np.array([156, 255, 255], np.float32) / 255, (out, out, 3)).copy()
            a0 = np.zeros((out, out), np.float32)
            has_a = False
        R_ = par[..., 0]
        tgt = rt / wsum
        wgt = rw / wsum
        R_new = R_ * (1 - wgt) + tgt * wgt + F['rv']
        G_new = par[..., 1] * (1 - F['cav'])
        B_new = par[..., 2] * F['bm']
        if 'metal' in classes:
            B_new = B_new * (1 - 0.6 * wts['metal']) + 0.6 * wts['metal']
        parn = np.empty((out, out, 4), np.uint8)
        parn[..., 0] = np.clip(R_new * 255 + 0.5, 0, 255)
        parn[..., 1] = np.clip(G_new * 255 + 0.5, 0, 255)
        parn[..., 2] = np.clip(B_new * 255 + 0.5, 0, 255)
        parn[..., 3] = np.clip(np.maximum(a0, F['metal']) * 255 + 0.5, 0, 255) if has_a else 255
        save_png(parn, td / (pre + st['par'] + '.%s.png' % tag))
        print('  par written%s; mean R/G/B %s -> %s' % ('' if pp.exists() else ' (NEW file: the original has no _par)',
              np.round(par.reshape(-1, 3).mean(0, dtype=np.float64) * 255), np.round(parn[..., :3].reshape(-1, 3).mean(0, dtype=np.float64))), flush=True)
        outputs.append(st['par'])
    outputs.append(st['dif'])
    # region debug image for sheets
    pal = dict(fur=(250, 200, 50), smooth_skin=(120, 200, 255), scales=(255, 100, 100), metal=(200, 200, 210), none=(40, 40, 40))
    dbg = np.zeros((src, src, 3), np.uint8)
    for i, c in enumerate(cls):
        dbg[idx == i] = pal.get(c, (255, 0, 255))
    Image.fromarray(dbg).save(td / (pre + st['name'] + '_regions.debug.png'))


def upscale_only(asset, cfg, name, out, tag):
    td = tex_dir(asset)
    f = td / ('CH_%s_00_0_A00_%s.orig.png' % (cfg['num'], name))
    if not f.exists():
        return
    a = np.asarray(Image.open(f).convert('RGBA'))
    r = cv2.resize(a, (out, out), interpolation=cv2.INTER_LANCZOS4)          # straight alpha: RGB under alpha 0 stays as authored
    Image.fromarray(r, 'RGBA').save(td / ('CH_%s_00_0_A00_%s.%s.png' % (cfg['num'], name, tag)), compress_level=1)
    print(' upscale-only', name, flush=True)


def register_manifest(asset, tag, files):
    mp = CACHE / 'manifest.json'
    man = json.loads(mp.read_text())
    if not any(v['id'] == tag for v in man['variants']):
        man['variants'].append({'id': tag, 'label': 'Material-authored (fur, scales, metal) %s' % tag, 'scale': int(tag[1])})
    for a in man['assets']:
        if a['id'] != asset:
            continue
        for t in a['textures']:
            n = t['name'].split('_00_0_A00_')[-1]
            if n in files:
                t['variants'][tag] = '%s/textures/%s.%s.png' % (asset, t['name'], tag)
    mp.write_text(json.dumps(man, indent=1))


def cmd_author(a):
    cfg = SPECIES[a.species]
    out = 1024 * a.scale
    tag = 'x%d_mat' % a.scale
    ctx = dict(tag=tag, feature=a.feature if a.feature else (1.0 if a.scale >= 4 else 3.0), seed=a.seed)
    files = []
    t0 = time.time()
    for st in cfg['sets']:
        w0 = Image.open(tex_dir(a.species) / ('CH_%s_00_0_A00_%s.orig.png' % (cfg['num'], st['dif']))).width
        author_set(a.species, cfg, st, w0 * a.scale, ctx, files)
    for n in cfg['upscale_only']:
        w = Image.open(tex_dir(a.species) / ('CH_%s_00_0_A00_%s.orig.png' % (cfg['num'], n))).width
        upscale_only(a.species, cfg, n, w * a.scale, tag)
        files.append(n)
    if not a.no_manifest:
        register_manifest(a.species, tag, set(files))
    print('done %s in %.0fs; files: %s' % (a.species, time.time() - t0, files))


def cmd_pack(a):
    import build_cache as bc
    import nutexb_encode as ne
    cfg = SPECIES[a.species]
    info = json.loads((CACHE / a.species / 'info.json').read_text()) if (CACHE / a.species / 'info.json').exists() else None
    src_pack = (info or {}).get('source_pack') or 'Characters/master/CH_%s.drp' % cfg['num']
    dst = Path(a.root) / a.pack / 'nx' / 'Pack' / src_pack
    dst.mkdir(parents=True, exist_ok=True)
    (Path(a.root) / a.pack / 'pack.json').write_text(json.dumps({'name': a.pack, 'version': '1.0.0', 'author': 'local', 'description': 'material-authored textures (%s)' % a.variant}, indent=1))
    td = tex_dir(a.species)
    for st in cfg['sets']:
        for key in ('dif', 'nor', 'par'):
            n = st.get(key)
            if not n:
                continue
            pre = 'CH_%s_00_0_A00_%s' % (cfg['num'], n)
            orig = bc.PACK / src_pack / (pre + '.nutexb')
            png = td / (pre + '.%s.png' % a.variant)
            if not orig.exists():
                print('skip (no original nutexb, new file not loadable):', pre)
                continue
            if not png.exists():
                print('skip (no variant png):', pre)
                continue
            im = Image.open(png)
            cap = min(a.max_edge, Image.open(td / (pre + '.orig.png')).width)      # never above the original texture size (hub crash B1)
            if im.width > cap:
                im = im.convert('RGBA').resize((cap, cap), Image.LANCZOS)
                png = Path(a.root) / ('_tmp_' + pre + '.png')
                im.save(png)
            r = ne.encode(png, orig, dst / orig.name)
            print('pack:', orig.name, r)
    for f in Path(a.root).glob('_tmp_*.png'):
        f.unlink()


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('author')
    p.add_argument('species')
    p.add_argument('--scale', type=int, default=4)
    p.add_argument('--feature', type=float, default=0.0, help='feature size multiplier (default 1 at 4x, 3 at 1x so detail survives at 1024)')
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--no-manifest', action='store_true')
    q = sp.add_parser('pack')
    q.add_argument('species')
    q.add_argument('--root', required=True)
    q.add_argument('--pack', required=True)
    q.add_argument('--variant', default='x1_mat')
    q.add_argument('--max-edge', type=int, default=1024)
    a = ap.parse_args()
    {'author': cmd_author, 'pack': cmd_pack}[a.cmd](a)


if __name__ == '__main__':
    main()
