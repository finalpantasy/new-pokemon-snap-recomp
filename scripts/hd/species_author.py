# SPDX-License-Identifier: MIT
r"""Bespoke per-species material authoring, driven by a hand-written spec per species (scripts/hd/species/<species>.json).

  python -I scripts\hd\species_author.py author magikarp [--debug DIR]   # x4_mat (4096 master) + x1_mat (pack size, area-downsampled master)
  python -I scripts\hd\species_author.py pack magikarp --root build\mods_mat --pack mat-magikarp

Replaces the first pass (material_author.py: one k-means colour rule and one global pattern per class), which the owner rejected
on 2026-11-12 ("Magikarp texture/relief kinda creepy ... more bespoke"). Rules: docs/hd-assets.md "Material re-texture: authoring rules".
  - Each region is hand-placed (UV island ids, Lab colour ranges, UV polygons, distance-to-island-edge), first match wins, so
    exclusions (lips, mouth, markings, fin borders) are listed first with treatment "none".
  - Every treatment starts from what the artist painted: lattices measured from painted outlines, fur flow from the painted
    strokes (structure tensor), seams and ribs from painted lines (black-hat / top-hat), grain from the painted high-pass.
  - Amplitudes are normal tilts (RMS inside the region) and multiplicative albedo offsets, low by default.
Outputs are game-derived and stay in build/hd/cache (git-ignored). Normal convention: R = +X right, G = +Y up, linear.
"""
import argparse, ctypes, json, sys, time
from pathlib import Path

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage as ndi
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import material_author as ma  # noqa: E402

CACHE = ma.CACHE
SPEC_DIR = HERE / 'species'
cv2.setNumThreads(4)
F32 = np.float32


def below_normal():
    try:
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)
    except Exception:
        pass


def load_spec(name):
    for f in sorted(SPEC_DIR.glob('*.json')):
        s = json.loads(f.read_text())
        if name.lower() in (f.stem.lower(), s['asset'], s['name'].lower()):
            return s
    raise SystemExit('no spec for %s in %s' % (name, SPEC_DIR))


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return (t * t * (3 - 2 * t)).astype(F32)


def up(a, out, interp=cv2.INTER_CUBIC):
    a = np.ascontiguousarray(a, dtype=F32)
    return a if a.shape[0] == out else cv2.resize(a, (out, out), interpolation=interp)


def gb(a, s):
    return cv2.GaussianBlur(np.ascontiguousarray(a, dtype=F32), (0, 0), max(float(s), 0.3))


def hp(a, s):
    return (a - gb(a, s)).astype(F32)


def unit_std(a, m=None):
    sel = a[m > 0.5] if m is not None and (m > 0.5).any() else a
    return (a - float(sel.mean())) / max(float(sel.std()), 1e-6)


def tilt(h, amp, m):
    """height -> normal XY with RMS tilt `amp` measured inside mask m (OpenGL +Y up)."""
    gx = cv2.Sobel(h, cv2.CV_32F, 1, 0, ksize=3) / 8.0
    gy = cv2.Sobel(h, cv2.CV_32F, 0, 1, ksize=3) / 8.0
    w = m > 0.5
    if not w.any():
        return gx * 0, gy * 0
    rms = float(np.sqrt(((gx[w] ** 2 + gy[w] ** 2).mean()) / 2.0))
    s = amp / max(rms, 1e-9)
    return (-gx * s).astype(F32), (gy * s).astype(F32)


# ---------------------------------------------------------------- geometry: islands (same ids as material_author) + signed flow
def geometry(asset, mat, R, dirs):
    mats = mat if isinstance(mat, list) else [mat]                # several materials can share one texture set (Skarmory body + wings)
    prims = [p for p in ma.load_prims(asset) if any(p['mat'].endswith('_' + x) for x in mats)]
    if not prims:
        raise SystemExit('no mesh uses material ' + mat)
    tri_id = np.full((R, R), -1, np.int32)
    labs, vecs = [], {k: [] for k in dirs}
    base = off = 0
    for pr in prims:
        UV, I = pr['UV'], pr['I']
        key = np.round(UV * 1e5).astype(np.int64)
        kk = key[:, 0] * 4000003 + key[:, 1]
        uniq, inv = np.unique(kk, return_inverse=True)
        r = np.concatenate([inv[I[:, 0]], inv[I[:, 1]]])
        c = np.concatenate([inv[I[:, 1]], inv[I[:, 2]]])
        g = coo_matrix((np.ones(len(r)), (r, c)), shape=(len(uniq), len(uniq)))
        _, comp = connected_components(g, directed=False)
        labs.append(comp[inv[I[:, 0]]] + 1 + base)
        base += comp.max() + 1
        for k, d in dirs.items():
            d = np.asarray(d, float)
            fu, fv, conf = ma.tri_flow(pr, d / np.linalg.norm(d))
            n = np.hypot(fu, fv)
            n = np.where(n > 0, n, 1.0)
            vecs[k].append(np.stack([fu / n * conf, fv / n * conf, conf], 1))
        uvt = UV[I]
        uvt = uvt - np.floor(uvt.mean(1, keepdims=True))
        px = uvt * R
        for ti in range(len(I)):
            cv2.fillConvexPoly(tri_id, np.round(px[ti] * 16).astype(np.int32), int(ti + off), lineType=cv2.LINE_8, shift=4)
        off += len(I)
    labs = np.concatenate(labs).astype(np.int32)
    cov = tri_id >= 0
    tid = np.where(cov, tri_id, 0)
    lab = np.where(cov, labs[tid], 0).astype(np.int32)
    _, (iy, ix) = ndi.distance_transform_edt(~cov, return_indices=True)
    lab_ext = lab[iy, ix]
    sg = max(R / 128.0, 3.0)
    flows = {}
    for k in dirs:
        v = np.concatenate(vecs[k])[tid] * cov[..., None]
        fx = np.zeros((R, R), F32)
        fy = np.zeros((R, R), F32)
        cf = np.zeros((R, R), F32)
        for i in np.unique(lab[cov]):                       # smooth per island so mirrored neighbours do not cancel
            m = (lab == i).astype(F32)
            w = gb(m, sg)
            a = gb(v[..., 0] * m, sg)
            b = gb(v[..., 1] * m, sg)
            cc = gb(v[..., 2] * m, sg)
            mag = np.hypot(a, b)
            sel = m > 0
            fx[sel] = (a / np.maximum(mag, 1e-9))[sel]
            fy[sel] = (b / np.maximum(mag, 1e-9))[sel]
            cf[sel] = np.clip(mag / np.maximum(w, 1e-6), 0, 1)[sel]
        flows[k] = (np.stack([fx[iy, ix], fy[iy, ix]], -1).astype(F32), cf[iy, ix].astype(F32))
    # distance (texels) to the island border, 0 in the gutter
    border = cov.copy()
    border[:-1] &= lab[:-1] == lab[1:]
    border[1:] &= lab[1:] == lab[:-1]
    border[:, :-1] &= lab[:, :-1] == lab[:, 1:]
    border[:, 1:] &= lab[:, 1:] == lab[:, :-1]
    d_edge = ndi.distance_transform_edt(border).astype(F32)
    return lab, lab_ext.astype(np.int32), cov, flows, d_edge


# ---------------------------------------------------------------- painted-feature detectors (source resolution)
def channel(C, name):
    src, ch = name.split('.')
    if src == 'dif':
        return C['L'] if ch == 'L' else C['dif'][..., 'RGB'.index(ch)] * 100
    p = C['par']
    if p is None:
        return C['L']
    return p[..., 'RGBA'.index(ch)] * 100


def line_resp(L, polarity, width):
    k = int(2 * round(width * 1.5) + 3)
    se = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
    op = cv2.MORPH_BLACKHAT if polarity == 'dark' else cv2.MORPH_TOPHAT
    return cv2.morphologyEx(np.ascontiguousarray(L, F32), op, se)


def lines_src(C, p, mask_src):
    L = channel(C, p.get('source', 'dif.L'))
    r = line_resp(L, p.get('polarity', 'dark'), p.get('width', 1.5))
    lo, hi = p.get('thr', [4, 12])
    m = smoothstep(lo, hi, r) * (mask_src > 0.05)
    ma_ = p.get('min_area', 0)
    if ma_:                                                   # lines are long: drop isolated specks of the painted mottling
        n, lab, stats, _ = cv2.connectedComponentsWithStats((m > 0.25).astype(np.uint8), connectivity=8)
        keep = np.zeros(n, bool)
        keep[1:] = np.maximum(stats[1:, cv2.CC_STAT_WIDTH], stats[1:, cv2.CC_STAT_HEIGHT]) >= ma_
        m = m * keep[lab]
    return m


def crisp_lines(C, base, p, m_out, mask_src):
    """refine painted lines: same line, narrower profile at 4096 (power on the line depth relative to the local background)."""
    out, k = C['out'], C['k']
    lm = up(lines_src(C, p, mask_src), out, cv2.INTER_LINEAR)
    lm = cv2.dilate(lm, np.ones((3, 3), np.uint8), iterations=max(1, int(round(k))))
    sel = (lm > 0.02) & (m_out > 0.02)
    if not sel.any():
        return base
    ks = int(2 * round(p.get('width', 1.5) * k * 1.2) + 3)
    se = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (ks, ks))
    dark = p.get('polarity', 'dark') == 'dark'
    pw = p.get('power', 2.0)
    res = base.copy()
    for c in range(3):
        ch = np.ascontiguousarray(base[..., c])
        bg = cv2.morphologyEx(ch, cv2.MORPH_CLOSE if dark else cv2.MORPH_OPEN, se)
        bg = gb(bg, k * 0.6)
        if dark:
            bg = np.maximum(bg, ch)
            d = np.clip(1 - ch / np.maximum(bg, 1e-5), 0, 1)
            pk = cv2.dilate(d, se)
            dn = pk * (d / np.maximum(pk, 1e-5)) ** pw
            new = bg * (1 - dn)
        else:
            bg = np.minimum(bg, ch)
            d = np.clip(ch / np.maximum(bg, 1e-5) - 1, 0, 4)
            pk = cv2.dilate(d, se)
            dn = pk * (d / np.maximum(pk, 1e-5)) ** pw
            new = bg * (1 + dn)
        w = lm * m_out
        res[..., c] = np.where(sel, ch * (1 - w) + new * w, ch)
    return res


def painted_flow(C, p, flow_geo, conf_geo):
    """per-texel strand direction (unit, src res): structure tensor of the painted strokes where coherent, geometric guess elsewhere."""
    src = channel(C, p.get('stroke_source', 'dif.L'))
    sd = p.get('stroke_sigma', 0.8)
    g = gb(hp(src, 6.0), sd)
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    st = p.get('tensor_sigma', 5.0)
    jxx, jxy, jyy = gb(gx * gx, st), gb(gx * gy, st), gb(gy * gy, st)
    coh = np.sqrt((jxx - jyy) ** 2 + 4 * jxy ** 2) / np.maximum(jxx + jyy, 1e-9)
    tg = 0.5 * np.arctan2(2 * jxy, jxx - jyy) + np.pi / 2           # strokes run across the gradient
    w = smoothstep(*p.get('coherence', [0.15, 0.45]), coh)
    th_g = np.arctan2(flow_geo[..., 1], flow_geo[..., 0])
    cs = w * np.cos(2 * tg) + (1 - w) * conf_geo * np.cos(2 * th_g)
    sn = w * np.sin(2 * tg) + (1 - w) * conf_geo * np.sin(2 * th_g)
    cs, sn = gb(cs, 2.0), gb(sn, 2.0)
    th = 0.5 * np.arctan2(sn, cs)
    fx, fy = np.cos(th), np.sin(th)
    s = np.where(fx * flow_geo[..., 0] + fy * flow_geo[..., 1] < 0, -1.0, 1.0)
    return np.stack([fx * s, fy * s], -1).astype(F32), w


# ---------------------------------------------------------------- treatments: f(C, p, m_out, m_src) -> fields
def t_none(C, p, m, ms):
    return {}


def t_scales_lattice(C, p, m, ms):
    """large overlapping scales continuing the lattice of the painted scale outlines (Magikarp zigzag column).
    Voronoi of a staggered lattice in (s along the tail, t across); each scale rises toward its rear (tail-side) edge,
    the next column starts low (tucked under), so the step reads as an overlapping rear edge."""
    out, k = C['out'], C['k']
    P = p['row_period']
    tip, corner = p['tip_s'], p['corner_s']
    step = P * P / (4.0 * (tip - corner))                    # column spacing that reproduces the painted zigzag
    s0 = tip - (step * step + P * P / 4) / (2 * step)
    Rs = tip - s0
    h = np.zeros((out, out), F32)
    rim = np.zeros((out, out), F32)
    for lat in p['lattices']:
        sel = (C['lab_out'] == lat['island']) & (m > 0.01)
        if not sel.any():
            continue
        ys, xs = np.nonzero(sel)
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        Y, X = np.mgrid[y0:y1, x0:x1].astype(F32)
        x = (X + 0.5) / k - 0.5
        y = (Y + 0.5) / k - 0.5
        s = lat['dir'] * (x - lat['line_x'])
        t = y - p['row_y0']
        jc = np.floor((s - s0) / step + 0.5)
        b1 = np.full(s.shape, 1e9, F32)
        b2 = np.full(s.shape, 1e9, F32)
        ds = np.zeros(s.shape, F32)
        dt = np.zeros(s.shape, F32)
        for dj in (-1, 0, 1):
            j = jc + dj
            sc = s0 + j * step
            off = np.where(np.mod(j, 2) == 0, P / 2, 0.0)
            mr = np.round((t - off) / P)
            for dm in (-1, 0, 1):
                tc = off + (mr + dm) * P
                d = np.hypot(s - sc, t - tc)
                n1 = d < b1
                b2 = np.where(n1, b1, np.minimum(b2, d))
                ds = np.where(n1, s - sc, ds)
                dt = np.where(n1, t - tc, dt)
                b1 = np.where(n1, d, b1)
        u = ds / Rs
        prof = smoothstep(-1.05, 0.9, u) ** 1.4                  # low at the front (tucked), high at the rear edge
        dome = 1 - np.clip(np.abs(dt) / (P / 2), 0, 1) ** 2
        hh = prof * (0.85 + 0.15 * dome)
        hh = np.where(s < 0, 0, hh)
        h[y0:y1, x0:x1] = np.where(sel[y0:y1, x0:x1], hh, h[y0:y1, x0:x1])
        rim[y0:y1, x0:x1] = np.where(sel[y0:y1, x0:x1], prof ** 4 * smoothstep(0, 2.5, b2 - b1), rim[y0:y1, x0:x1])
    h = gb(h, p.get('soften', 0.7) * k)
    cav = np.clip((gb(h, 2.0 * k) - h) * 4.0, 0, 1)
    nx, ny = tilt(h, p['amp'], m)
    return dict(nx=nx, ny=ny, alb=p.get('alb_rim', 0.02) * rim - p.get('alb_tuck', 0.03) * cav,
                cav=cav * p.get('cav', 0.5), h=h)


def t_fin_membrane(C, p, m, ms):
    """thin fin: ribs raised only along the painted rib lines, membrane sags slightly between them and reads a little thinner."""
    out, k = C['out'], C['k']
    rib = up(lines_src(C, p['ribs'], ms), out, cv2.INTER_LINEAR)
    ribc = smoothstep(0.3, 0.7, rib)
    d = ndi.distance_transform_edt(ribc < 0.5).astype(F32) / k        # texels at source res
    rr = gb(ribc, p.get('rib_soft', 0.8) * k)
    sag = -smoothstep(0, p.get('sag_texels', 10), d)
    nx1, ny1 = tilt(rr, p.get('rib_amp', 0.05), m)
    nx2, ny2 = tilt(gb(sag, 1.5 * k), p.get('sag_amp', 0.02), m)
    thin = smoothstep(1.0, p.get('sag_texels', 10), d)
    return dict(nx=nx1 + nx2, ny=ny1 + ny2, alb=p.get('alb_thin', 0.025) * thin, h=rr + 0.3 * sag)


def t_fur(C, p, m, ms):
    """fur following the painted strokes: strands at the painted stroke scale (length/width in texels at 1024),
    the painted stroke high-pass is kept as half of the strand field so the HD fur is the same fur seen closer."""
    out, k, S = C['out'], C['k'], C['S']
    flow_geo, conf_geo = C['flows'][p.get('fallback_flow', 'fur')]
    fl, coh = painted_flow(C, p, flow_geo, conf_geo)
    C.setdefault('_flowdbg', []).append((fl, coh, ms))
    rng = np.random.default_rng(C['seed'] + p.get('seed', 0))
    ks = k * 1024.0 / S                                          # texels at 1024 -> output px
    L = p.get('len_texels', 5.0)
    W = p.get('width_texels', 0.5)
    lab = C['lab_out']
    nz = gb(rng.standard_normal((out, out)).astype(F32), max(0.5, W * ks * 0.5))
    new = unit_std(ma.lic(nz, fl, lab, L * ks / 2, max(1.0, L * ks / 10)), None)
    src = channel(C, p.get('stroke_source', 'dif.L'))
    ph = up(hp(src, 3.0), out)
    ph = unit_std(ma.lic(ph, fl, lab, L * ks / 4, max(1.0, L * ks / 10)), None)
    mix = p.get('painted_mix', 0.5)
    strands = unit_std(mix * ph + (1 - mix) * new, m)
    lo = max(out // 8, 64)
    nz2 = up(rng.standard_normal((lo, lo)).astype(F32), out)
    cl = p.get('clump_texels', 3.0)
    clump = unit_std(ma.lic(gb(nz2, cl * ks * 0.3), fl, lab, cl * ks * 1.5, max(2.0, cl * ks / 4)), m)
    amp_map = np.ones((out, out), F32)
    if 'edge' in p:
        e = p['edge']
        de = up(C['d_edge'], out, cv2.INTER_LINEAR)
        ew = (1 - smoothstep(0, e['texels'], de)) * m
        if ew.max() > 0.01:
            nz3 = gb(rng.standard_normal((out, out)).astype(F32), max(0.6, W * ks * 0.7))
            lng = unit_std(ma.lic(nz3, fl, lab, L * e.get('len_mul', 2.0) * ks / 2, max(1.0, L * ks / 8)), m)
            strands = strands * (1 - ew) + lng * ew
            amp_map = 1 - ew * (1 - e.get('amp_mul', 0.6))
    h = strands * 0.75 + clump * p.get('clump_w', 0.45)
    nx, ny = tilt(h, p['amp'], m)
    cav = np.clip(-(0.6 * strands + 0.6 * clump) * 0.5, 0, 1)
    return dict(nx=nx * amp_map, ny=ny * amp_map,
                alb=(p.get('alb_amp', 0.03) * (0.7 * strands + 0.5 * clump) / 1.2) * amp_map,
                cav=cav * p.get('cav', 0.4), h=h)


def t_skin(C, p, m, ms):
    """smooth skin: only the painted grain (high-pass of the albedo at its own scale) as micro relief, plus a very soft large undulation."""
    out, k = C['out'], C['k']
    src = channel(C, p.get('source', 'dif.L'))
    g = up(gb(hp(src, p.get('grain_sigma', 3.0)), 0.6), out)
    g = gb(g, 0.6 * k)
    rng = np.random.default_rng(C['seed'] + 31)
    lo = max(out // 32, 16)
    und = up(rng.standard_normal((lo, lo)).astype(F32), out)
    h = unit_std(g, m) * 1.0 + unit_std(und, m) * p.get('undulation', 0.0)
    nx, ny = tilt(h, p['amp'], m)
    cav = np.zeros((out, out), F32)
    if 'creases' in p:                                        # painted muscle / fold lines -> soft grooves along the same lines
        c = p['creases']
        cr = gb(up(lines_src(C, c, ms), out, cv2.INTER_LINEAR), c.get('soft', 1.0) * k)
        cx, cy = tilt(-cr, c['amp'], m)
        nx, ny = nx + cx, ny + cy
        cav = cav + cr * c.get('cav', 0.3)
    if 'pores' in p:                                          # sparse, tiny, evenly spread (no clusters)
        pp = p['pores']
        ks = k * 1024.0 / C['S']
        cell = pp.get('spacing_texels', 5.0) * ks
        n = int(out / cell)
        jit = rng.uniform(0.15, 0.85, (n, n, 2))
        img = np.zeros((out, out), F32)
        ys = ((np.arange(n)[:, None] + jit[..., 1]) * cell).astype(int).ravel()
        xs = ((np.arange(n)[None, :] + jit[..., 0]) * cell).astype(int).ravel()
        keep = rng.random(len(ys)) < pp.get('fill', 0.6)
        img[np.clip(ys[keep], 0, out - 1), np.clip(xs[keep], 0, out - 1)] = 1.0
        pr = gb(img, pp.get('radius_texels', 0.3) * ks)
        pr /= max(float(pr.max()), 1e-6)
        px_, py_ = tilt(-pr, pp['amp'], m)
        nx, ny = nx + px_, ny + py_
        cav = cav + pr * pp.get('cav', 0.1)
    return dict(nx=nx, ny=ny, alb=-p.get('alb_cav', 0.0) * cav, cav=cav, h=h)


def t_plates(C, p, m, ms):
    """plates separated by painted seams (Squirtle scutes, Steelix rock segments): seam groove, bevel, gentle dome;
    optional growth lines parallel to the seams, edge chips (sparse, only at edges), stone grain from the painted mottling."""
    out, k, S = C['out'], C['k'], C['S']
    ks = k * 1024.0 / S
    rng = np.random.default_rng(C['seed'] + p.get('seed', 0) + 7)
    seam = up(lines_src(C, p['seams'], ms), out, cv2.INTER_LINEAR)
    seamc = smoothstep(0.3, 0.7, seam)
    walls = seamc >= 0.5
    if p.get('edge_is_seam', False):
        walls |= m < 0.5
    d = ndi.distance_transform_edt(~walls).astype(F32) / ks          # texels at 1024
    bev = p.get('bevel_texels', 3.0)
    dch = d
    chip = np.zeros((out, out), F32)
    if 'chips' in p:
        c = p['chips']
        lo = max(out // 64, 16)
        nzc = up(rng.standard_normal((lo, lo)).astype(F32), out) * 0.6 + gb(rng.standard_normal((out, out)).astype(F32), 1.5 * ks) * 3.0
        dch = d + c.get('edge_noise', 1.0) * nzc * smoothstep(bev * 2.5, 0, d)
        # sparse notches along edges
        edge = (d > 0.3) & (d < bev * 1.2) & (m > 0.5)
        ys, xs = np.nonzero(edge)
        n = int(len(ys) / (ks * ks) * c.get('per_texel2', 0.004))
        if n and len(ys):
            pick = rng.choice(len(ys), min(n, len(ys)), replace=False)
            img = np.zeros((out, out), F32)
            for i in pick:
                r = rng.uniform(*c.get('radius_texels', [1.0, 3.0])) * ks
                ax = (int(r), int(r * rng.uniform(0.5, 1.0)))
                cv2.ellipse(img, (int(xs[i]), int(ys[i])), ax, rng.uniform(0, 180), 0, 360, 1.0, -1, cv2.LINE_AA)
            chip = gb(img, 0.6 * ks)
    prof = smoothstep(0, bev, dch)
    dome = 1 - np.exp(-d / max(p.get('dome_texels', 20.0), 1e-3))
    h = prof * 1.0 + dome * p.get('dome', 0.6) - chip * p.get('chips', {}).get('depth', 0.6)
    if 'growth' in p:
        g = p['growth']
        rings = np.cos(2 * np.pi * d / g['spacing']) * np.exp(-d / g.get('fade', 25.0)) * smoothstep(bev * 0.5, bev * 1.5, d)
        h = h + g['amp'] * rings
    h = gb(h, 0.5 * ks)
    nx, ny = tilt(h, p['amp'], m)
    if 'grain' in p:
        gp = p['grain']
        src = channel(C, gp.get('source', 'dif.L'))
        gg = gb(up(gb(hp(src, gp.get('sigma', 3.0)), 0.6), out), 0.6 * k)
        mi = gb(rng.standard_normal((out, out)).astype(F32), max(0.6, gp.get('micro_texels', 0.6) * ks))
        gh = unit_std(gg, m) + gp.get('micro', 0.5) * unit_std(mi, m)
        gx_, gy_ = tilt(gh, gp['amp'], m)
        nx, ny = nx + gx_, ny + gy_
    cav = np.clip(seamc + chip * 0.5, 0, 1)
    alb = -p.get('alb_seam', 0.0) * seamc + p.get('alb_chip', 0.0) * chip
    if 'grain' in p and p['grain'].get('alb', 0):
        alb = alb + p['grain']['alb'] * np.clip(unit_std(mi, m), -2.5, 2.5) / 2.5
    return dict(nx=nx, ny=ny, alb=alb, cav=cav * p.get('cav', 0.4), h=h)


TREAT = dict(none=t_none, scales_lattice=t_scales_lattice, fin_membrane=t_fin_membrane, fur=t_fur, skin=t_skin, plates=t_plates)


# ---------------------------------------------------------------- regions
def select(C, sel):
    S = C['S']
    m = np.ones((S, S), bool)
    if 'islands' in sel:
        m &= np.isin(C['lab_ext'], sel['islands'])
    for key, rng_ in sel.get('lab', {}).items():
        v = C[key]
        lo, hi = rng_
        m &= ((v >= lo) & (v <= hi)) if lo <= hi else ((v >= lo) | (v <= hi))
    if 'uv_poly' in sel:
        pm = np.zeros((S, S), np.uint8)
        pts = (np.array(sel['uv_poly'], float) * S / 1024.0).astype(np.int32)
        cv2.fillPoly(pm, [pts], 1)
        m &= pm > 0
    if 'edge_within' in sel:
        m &= C['d_edge'] <= sel['edge_within'] * S / 1024.0
    if 'edge_beyond' in sel:
        m &= C['d_edge'] > sel['edge_beyond'] * S / 1024.0
    return m


def region_mask(C, r):
    S = C['S']
    m = np.zeros((S, S), bool)
    for s in r['include']:
        m |= select(C, s)
    for s in r.get('exclude', []):
        m &= ~select(C, s)
    g = r.get('grow', 0)
    if g:
        se = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * abs(g) + 1, 2 * abs(g) + 1))
        m = (cv2.dilate if g > 0 else cv2.erode)(m.astype(np.uint8), se) > 0
    mf = m.astype(F32)
    f = r.get('feather', 1.0)
    return gb(mf, f) if f > 0 else mf


# ---------------------------------------------------------------- per texture set
def author_set(spec, st, scale, seed, dbg_dir, written):
    asset, num = spec['asset'], spec['num']
    td = ma.tex_dir(asset)
    pre = 'CH_%s_00_0_A00_' % num
    dif = ma.load_png(td / (pre + st['dif'] + '.orig.png'))
    S = dif.shape[0]
    out = min(S * scale, spec.get('master_max', 4096))           # 2048 originals (Snorlax) get a 4096 master, not 8192
    k = out / S
    t0 = time.time()
    flows = {n: d for n, d in st.get('flows', {'fur': [0.0, -1.0, -0.45]}).items()}
    lab, lab_ext, cov, fl, d_edge = geometry(asset, st['mat'], S, flows)
    rgb = dif[..., :3].astype(F32) / 255
    labc = cv2.cvtColor(gb(rgb, 0.7), cv2.COLOR_RGB2Lab)
    C = dict(S=S, out=out, k=k, seed=seed, dif=rgb, par=None, lab=lab, lab_ext=lab_ext, cov=cov, flows=fl, d_edge=d_edge,
             L=labc[..., 0], C=np.hypot(labc[..., 1], labc[..., 2]), h=np.degrees(np.arctan2(labc[..., 2], labc[..., 1])) % 360)
    pp = td / (pre + st['par'] + '.orig.png') if st.get('par') else None
    p0 = ma.load_png(pp) if pp is not None and pp.exists() else None
    if p0 is not None:
        C['par'] = cv2.resize(p0.astype(F32) / 255, (S, S), interpolation=cv2.INTER_LINEAR) if p0.shape[0] != S else p0.astype(F32) / 255
    C['lab_out'] = cv2.resize(lab_ext.astype(F32), (out, out), interpolation=cv2.INTER_NEAREST).astype(np.int32)
    print(' set %s: %d -> %d, islands %s  (%.0fs)' % (st['name'], S, out, sorted(int(i) for i in np.unique(lab) if i), time.time() - t0), flush=True)
    base = ma.srgb_to_lin(np.clip(up(rgb, out, cv2.INTER_LANCZOS4), 0, 1))
    alpha = np.clip(up(dif[..., 3].astype(F32), out, cv2.INTER_LINEAR), 0, 255)
    acc = {key: np.zeros((out, out), F32) for key in ('nx', 'ny', 'alb', 'cav', 'rt', 'rw', 'bmul')}
    acc['bmul'][:] = 1.0
    claimed = np.zeros((S, S), F32)
    dbg = np.zeros((S, S, 3), F32)
    pal = np.random.default_rng(3).uniform(0.25, 1.0, (64, 3))
    hdbg = np.zeros((out, out), F32)
    for ri, r in enumerate(st['regions']):
        t1 = time.time()
        ms = region_mask(C, r) * (1 - claimed)
        claimed = np.clip(claimed + ms, 0, 1)
        if ms.max() < 0.01:
            print('  region %-16s empty' % r['id'], flush=True)
            continue
        dbg += ms[..., None] * pal[ri]
        m = np.clip(up(ms, out, cv2.INTER_LINEAR), 0, 1)
        if 'crisp_lines' in r:
            base = crisp_lines(C, base, r['crisp_lines'], m, ms)
        f = TREAT[r.get('treatment', 'none')](C, r.get('params', {}), m, ms)
        for key in ('nx', 'ny', 'alb', 'cav'):
            if key in f:
                acc[key] += m * f[key]
        if 'h' in f:
            hdbg += m * unit_std(f['h'], m) * 0.25
        par = r.get('par', {})
        if 'rough_to' in par:
            acc['rt'] += m * par['rough_to'] * par.get('rough_w', 0.3)
            acc['rw'] += m * par.get('rough_w', 0.3)
        if 'spec_mul' in par:
            acc['bmul'] *= 1 + m * (par['spec_mul'] - 1)
        print('  region %-16s %-15s share %.3f  %.1fs' % (r['id'], r.get('treatment', 'none'), float(ms[cov].mean()) if cov.any() else 0, time.time() - t1), flush=True)
        del f, m
    # ---- albedo: multiplicative detail + optional baked cavity (for bodies whose normal does not reach the game), colour lock
    bake = st.get('bake_ao', 0.0)
    mod = base * (1 + acc['alb'] - bake * acc['cav'])[..., None]
    sg = st.get('lock_sigma', 6.0) * k
    lp_b = cv2.GaussianBlur(base, (0, 0), sg)
    lp_m = cv2.GaussianBlur(mod.astype(F32), (0, 0), sg)
    ratio = np.clip(lp_b / np.maximum(lp_m, 1e-5), 0.8, 1.25)
    fin = np.where(lp_b < 2e-3, base, mod * ratio)
    alb = np.empty((out, out, 4), np.uint8)
    alb[..., :3] = np.clip(ma.lin_to_srgb(fin) * 255 + 0.5, 0, 255).astype(np.uint8)
    alb[..., 3] = alpha.astype(np.uint8)
    drift = np.abs(rgb.reshape(-1, 3).mean(0, dtype=np.float64) * 255 - alb[..., :3].reshape(-1, 3).mean(0, dtype=np.float64))
    res = {st['dif']: alb}
    print('  dif: mean RGB drift %s' % np.round(drift, 2), flush=True)
    del mod, lp_b, lp_m, ratio, fin
    # ---- normal
    if st.get('nor'):
        npth = td / (pre + st['nor'] + '.orig.png')
        if npth.exists():
            n0 = ma.load_png(npth)
            nxy = up(n0[..., :2].astype(F32) / 255 * 2 - 1, out)
            bconst = float(n0[..., 2].std()) < 1.0
            bfill = float(n0[..., 2].mean())
        else:
            nxy = np.zeros((out, out, 2), F32)
            bconst, bfill = False, 255.0
        nx, ny = nxy[..., 0] + acc['nx'], nxy[..., 1] + acc['ny']
        mag = np.hypot(nx, ny)
        sc = np.where(mag > 0.85, 0.85 / np.maximum(mag, 1e-6), 1.0)
        nx, ny = nx * sc, ny * sc
        nrm = np.empty((out, out, 4), np.uint8)
        nrm[..., 0] = np.clip((nx * 0.5 + 0.5) * 255 + 0.5, 0, 255)
        nrm[..., 1] = np.clip((ny * 0.5 + 0.5) * 255 + 0.5, 0, 255)
        nrm[..., 2] = int(round(bfill)) if bconst else np.clip((np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1)) * 0.5 + 0.5) * 255 + 0.5, 0, 255)
        nrm[..., 3] = 255
        res[st['nor']] = nrm
        print('  nor%s: detail tilt RMS %.3f' % ('' if npth.exists() else ' (NEW, cache-only: no normal slot in game)',
              float(np.sqrt((acc['nx'] ** 2 + acc['ny'] ** 2)[claimed.max() > 0].mean()))), flush=True)
    # ---- params (R rough, G occlusion, B spec mask)
    if C['par'] is not None:
        par = up(p0[..., :3].astype(F32) / 255, out)
        R_ = par[..., 0] * (1 - acc['rw']) + acc['rt']
        G_ = par[..., 1] * (1 - acc['cav'])
        B_ = par[..., 2] * acc['bmul']
        pn = np.empty((out, out, 4), np.uint8)
        pn[..., 0] = np.clip(R_ * 255 + 0.5, 0, 255)
        pn[..., 1] = np.clip(G_ * 255 + 0.5, 0, 255)
        pn[..., 2] = np.clip(B_ * 255 + 0.5, 0, 255)
        pn[..., 3] = np.clip(up(p0[..., 3].astype(F32), out, cv2.INTER_LINEAR) + 0.5, 0, 255) if p0[..., 3].std() > 0.5 else 255
        res[st['par']] = pn
        print('  par: mean RGB %s -> %s' % (np.round(par.reshape(-1, 3).mean(0, dtype=np.float64) * 255),
              np.round(pn[..., :3].reshape(-1, 3).mean(0, dtype=np.float64))), flush=True)
    # ---- write x{scale}_mat and the pack-size x1_mat (area-downsampled master, normals renormalised)
    for name, img in res.items():
        Image.fromarray(img, 'RGBA').save(td / (pre + name + '.x%d_mat.png' % scale), compress_level=1)
        if scale != 1:
            o = td / (pre + name + '.orig.png')
            S1 = Image.open(o).width if o.exists() else S                # pack size = that texture's own original size
            small = cv2.resize(img.astype(F32), (S1, S1), interpolation=cv2.INTER_AREA)
            if name == st.get('nor'):
                v = small[..., :2] / 255 * 2 - 1
                mg = np.hypot(v[..., 0], v[..., 1])
                v *= np.where(mg > 0.85, 0.85 / np.maximum(mg, 1e-6), 1.0)[..., None]
                small[..., :2] = (v * 0.5 + 0.5) * 255
                if not (img[..., 2].std() < 1.0):
                    small[..., 2] = (np.sqrt(np.clip(1 - (v ** 2).sum(-1), 0, 1)) * 0.5 + 0.5) * 255
            if name == st.get('par') and o.exists() and p0 is not None and p0[..., 3].std() > 0.5 and p0.shape[0] == S1:
                small[..., 3] = p0[..., 3]                                # authored metallic (BC3 alpha) back bit-exact at pack size
            Image.fromarray(np.clip(small + 0.5, 0, 255).astype(np.uint8), 'RGBA').save(td / (pre + name + '.x1_mat.png'), compress_level=1)
        written.append(name)
    if dbg_dir:
        Path(dbg_dir).mkdir(parents=True, exist_ok=True)
        Image.fromarray(np.clip((rgb * 0.45 + dbg * 0.55) * 255, 0, 255).astype(np.uint8)).save(Path(dbg_dir) / ('%s_%s_regions.png' % (spec['name'].lower(), st['name'])))
        hv = cv2.resize(hdbg, (2048, 2048), interpolation=cv2.INTER_AREA) if out > 2048 else hdbg
        Image.fromarray(np.clip(hv * 127 + 128, 0, 255).astype(np.uint8)).save(Path(dbg_dir) / ('%s_%s_height.png' % (spec['name'].lower(), st['name'])))
    print('  set %s done in %.0fs' % (st['name'], time.time() - t0), flush=True)


def cmd_author(a):
    below_normal()
    spec = load_spec(a.species)
    asset, num = spec['asset'], spec['num']
    td = ma.tex_dir(asset)
    for f in list(td.glob('*.x4_mat.png')) + list(td.glob('*.x1_mat.png')) + list(td.glob('*_regions.debug.png')):
        f.unlink()                                            # first-pass outputs of this asset: replaced below
    t0 = time.time()
    written = []
    for st in spec['sets']:
        author_set(spec, st, a.scale, a.seed, a.debug, written)
    for n in spec.get('upscale_only', []):
        f = td / ('CH_%s_00_0_A00_%s.orig.png' % (num, n))
        if not f.exists():
            continue
        w = Image.open(f).width
        ma.upscale_only(asset, {'num': num}, n, min(w * a.scale, spec.get('master_max', 4096)), 'x%d_mat' % a.scale)
        if a.scale != 1:
            ma.upscale_only(asset, {'num': num}, n, w, 'x1_mat')
        written.append(n)
    for tag in sorted({'x%d_mat' % a.scale, 'x1_mat'}):
        ma.register_manifest(asset, tag, set(written))
    print('done %s (%s) in %.0fs: %s' % (spec['name'], asset, time.time() - t0, written))


def cmd_pack(a):
    spec = load_spec(a.species)
    sets = []
    for st in spec['sets']:
        ig = st.get('in_game', {})
        sets.append(dict(dif=st['dif'], par=st.get('par') if ig.get('par', True) else None, nor=st.get('nor') if ig.get('nor', True) else None))
    ma.SPECIES[spec['asset']] = dict(name=spec['name'], num=spec['num'], sets=sets)
    a.species = spec['asset']
    ma.cmd_pack(a)


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    p = sp.add_parser('author')
    p.add_argument('species')
    p.add_argument('--scale', type=int, default=4)
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--debug', default=None, help='directory for region/height debug PNGs (use a scratch folder)')
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
