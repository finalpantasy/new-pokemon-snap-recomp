# SPDX-License-Identifier: MIT
"""Extra treatments (curls, fur_ce) for species_author, registered into species_author.TREAT (loaded by species_author_ext.py).

curls: dense curly fur (Bouffalant afro). The artist painted the afro as soft cloudy clumps (mottling ~6-12 texels at 1024).
  The strands follow the iso-lines of a stream function = the painted clump high-pass plus a little noise at the coil size,
  so the short curved strands wrap around the painted clumps (coils), instead of a new pattern on top of them. The painted
  clumps themselves are kept as the large form. No cells or dots: everything is line-integral-convolved strands.
"""
import cv2
import numpy as np

import material_author as ma
import species_author as sa

F32 = np.float32


def t_curls(C, p, m, ms):
    out, k, S = C['out'], C['k'], C['S']
    ks = k * 1024.0 / S                                    # texels at 1024 -> output px
    rng = np.random.default_rng(C['seed'] + p.get('seed', 0) + 101)
    lab = C['lab_out']
    src = sa.channel(C, p.get('source', 'dif.L'))
    s1024 = S / 1024.0
    # painted clumps (large form) and the stream function at source resolution
    clump_src = sa.gb(sa.hp(src, p.get('clump_sigma', 8.0) * s1024), p.get('clump_soft', 1.2) * s1024)
    coil = p.get('curl_texels', 3.0)
    nz = sa.gb(rng.standard_normal((S, S)).astype(F32), coil * s1024)
    phi = sa.unit_std(clump_src, ms) * p.get('follow_paint', 0.6) + sa.unit_std(nz, ms) * (1 - p.get('follow_paint', 0.6))
    gx = cv2.Sobel(phi, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(phi, cv2.CV_32F, 0, 1, ksize=3)
    th_iso = np.arctan2(-gx, gy)                            # along the iso-lines (wraps around the painted clumps)
    # a wandering angle field at the coil size bends the strands into short kinks, so no closed rings form
    nz2 = sa.unit_std(sa.gb(rng.standard_normal((S, S)).astype(F32), coil * 0.6 * s1024), ms)
    th = th_iso + p.get('kink', 1.2) * np.pi * nz2
    flow = np.stack([np.cos(th), np.sin(th)], -1).astype(F32)
    L = p.get('len_texels', 4.0)
    W = p.get('width_texels', 0.5)
    n1 = sa.gb(rng.standard_normal((out, out)).astype(F32), max(0.5, W * ks * 0.5))
    strands = sa.unit_std(ma.lic(n1, flow, lab, L * ks / 2, max(1.0, L * ks / 10)), m)
    # painted micro grain kept along the same coils (the artist's own speckle, refined)
    ph = sa.up(sa.hp(src, 2.0), out)
    ph = sa.unit_std(ma.lic(ph, flow, lab, L * ks / 4, max(1.0, L * ks / 10)), m)
    mix = p.get('painted_mix', 0.3)
    strands = sa.unit_std(mix * ph + (1 - mix) * strands, m)
    form = sa.unit_std(sa.up(clump_src, out), m)
    h = strands * 0.75 + form * p.get('clump_w', 0.5)
    nx, ny = sa.tilt(h, p['amp'], m)
    cav = np.clip(-(0.5 * strands + 0.8 * form) * 0.5, 0, 1)
    alb = p.get('alb_amp', 0.04) * (0.8 * strands + 0.4 * form) / 1.2
    return dict(nx=nx, ny=ny, alb=alb, cav=cav * p.get('cav', 0.4), h=h)


sa.TREAT['curls'] = t_curls


def t_fur_ce(C, p, m, ms):
    """species_author's fur, but the painted-stroke source has colour-patch edges removed first: the high-pass of the painted
    luminance is faded to zero where the low-pass has a strong step (cream/violet patch borders, white/black fur borders) and
    clipped to the fur grain's own range, so a patch border does not turn into a groove outlining the patch."""
    L0 = C['L']
    lo = sa.gb(L0, 2.0)
    g = np.hypot(cv2.Sobel(lo, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(lo, cv2.CV_32F, 0, 1, ksize=3)) / 8.0
    e0, e1 = p.get('edge_grad', [0.8, 2.0])
    keep = 1 - sa.smoothstep(e0, e1, g)
    keep = 1 - cv2.dilate(1 - keep, np.ones((5, 5), np.uint8))
    keep = sa.gb(keep, 1.5)
    hpv = L0 - sa.gb(L0, 4.0)
    sel = (ms > 0.5) & (keep > 0.9)
    s = float(hpv[sel].std()) if sel.any() else float(hpv.std())
    hpv = np.clip(hpv, -2.5 * s, 2.5 * s) * keep
    C['L'] = (float(lo.mean()) + hpv).astype(F32)                   # flat base + clean grain: no patch steps at all
    # species_author's LIC step is len/10; for long thin strands (len/width > ~7) that step is several times the noise width and the
    # strands come out beaded (a periodic chain along each strand). Cap the step so long silky fur stays continuous.
    cap = p.get('max_step_px', 2.0)
    lic0, pf0 = ma.lic, sa.painted_flow
    ma.lic = lambda nz, fl, lab, hl, step, *a: lic0(nz, fl, lab, hl, min(step, max(cap, hl / 24.0)), *a)
    if p.get('flow_from_paint_edges', False):              # flow from the original paint (jagged lock edges steer the strands)
        def pf(C_, p_, fg, cg):
            C_['L'] = L0
            try:
                return pf0(C_, p_, fg, cg)
            finally:
                C_['L'] = (float(lo.mean()) + hpv).astype(F32)
        sa.painted_flow = pf
    try:
        return sa.t_fur(C, p, m, ms)
    finally:
        C['L'] = L0
        ma.lic, sa.painted_flow = lic0, pf0


sa.TREAT['fur_ce'] = t_fur_ce


def _ce(base):
    """same colour-edge removal for the grain-based treatments (skin, plates): the painted grain is kept, patch borders are not
    turned into grooves (Torterra stone/dirt/turf borders)."""
    def t(C, p, m, ms):
        L0 = C['L']
        lo = sa.gb(L0, 2.0)
        g = np.hypot(cv2.Sobel(lo, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(lo, cv2.CV_32F, 0, 1, ksize=3)) / 8.0
        e0, e1 = p.get('edge_grad', [0.8, 2.0])
        keep = sa.gb(1 - cv2.dilate(sa.smoothstep(e0, e1, g), np.ones((5, 5), np.uint8)), 1.5)
        hpv = L0 - sa.gb(L0, 4.0)
        sel = (ms > 0.5) & (keep > 0.9)
        s = float(hpv[sel].std()) if sel.any() else float(hpv.std())
        C['L'] = (float(lo.mean()) + np.clip(hpv, -2.5 * s, 2.5 * s) * keep).astype(F32)
        try:
            return base(C, p, m, ms)
        finally:
            C['L'] = L0
    return t


sa.TREAT['skin_ce'] = _ce(sa.t_skin)
sa.TREAT['plates_ce'] = _ce(sa.t_plates)
