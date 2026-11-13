# SPDX-License-Identifier: MIT
"""Extra treatment for species_author: butterfly wing scales (Vivillon). Registered into species_author.TREAT
(loaded by species_author_ext2.py).

wing_scales: a real butterfly wing is covered in tiny shingle scales set in rows that run across the veins; each scale's base
  sits in the membrane and its free, rounded tip points away from the wing root and overlaps the base of the next row.
  The artist painted the wing as flat colour blocks between thin black veins, with a fine felt-like grain (the "powder").
  So: rows of scales in a polar lattice around each wing's root (the point where the painted veins converge, given per island
  in the spec), tile size near the painted grain (about 2 x 1.4 texels at 1024), half-tile stagger, each tile rising toward
  its tip (overlap step), a tiny per-scale tone jitter (real scales vary slightly), optional per-scale colour averaging so
  colour-block borders step along whole scales like on a real wing. The painted veins stay the painted veins (crisp, slightly
  raised, scales tucked under them). Low amplitude: at pack size it averages into the painted powder; up close it reads as scales.
"""
import cv2
import numpy as np
from scipy import ndimage as ndi

import species_author as sa

F32 = np.float32


def _hash(i, j, s):
    v = np.sin(i * 12.9898 + j * 78.233 + s * 37.719) * 43758.5453
    return (v - np.floor(v)).astype(F32)


def t_wing_scales(C, p, m, ms):
    out, k, S = C['out'], C['k'], C['S']
    ks = k * 1024.0 / S                                    # texels at 1024 -> output px
    Lr, Wt = p.get('tile_len', 2.0), p.get('tile_w', 1.4)  # texels at 1024
    h = np.zeros((out, out), F32)
    tip = np.zeros((out, out), F32)
    jit = np.zeros((out, out), F32)
    tid = np.full((out, out), -1, np.int64)
    for o in p['roots']:
        sel = (C['lab_out'] == o['island']) & (m > 0.01)
        if not sel.any():
            continue
        ys, xs = np.nonzero(sel)
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        Y, X = np.mgrid[y0:y1, x0:x1].astype(F32)
        x = (X + 0.5) / ks - 0.5                           # texels at 1024
        y = (Y + 0.5) / ks - 0.5
        dx, dy = x - o['root'][0], y - o['root'][1]
        r = np.hypot(dx, dy)
        th = np.arctan2(dy, dx)
        # rows are not perfect circles on a real wing: a slow waviness along the row (in angle), so no long ring lines
        wv = p.get('row_wave', 0.0)
        if wv:
            rng = np.random.default_rng(o['island'] * 17 + 3)
            ph = rng.uniform(0, 2 * np.pi, 3)
            r = r + wv * Lr * (np.sin(th * 7 + ph[0]) * 0.5 + np.sin(th * 13 + ph[1]) * 0.3 + np.sin(th * 23 + ph[2]) * 0.2)
        i = np.floor(r / Lr)
        fr = r / Lr - i                                    # 0 at the scale base (inner), 1 at its tip (outer)
        a = th * (i + 0.5) * Lr / Wt                        # across-row coordinate in tiles, constant within a row
        # row phase: strict half-tile stagger makes diagonal "twill" lines when magnified; a random phase per row does not
        a = a + (_hash(i, 0.0, o['island'] + 7) if p.get('random_phase', False) else 0.5 * np.mod(i, 2))
        j = np.floor(a)
        fa = a - j                                         # 0..1 across the scale
        c = 2 * fa - 1
        edge = 1 - p.get('tip_round', 0.38) * c * c                            # rounded tip: the tip line bulges outward at the scale centre
        prof = np.where(fr < edge, sa.smoothstep(0.0, 1.0, fr / edge) ** 1.5, 0.15 * (1 - (fr - edge) / np.maximum(1 - edge, 1e-3)))
        side = sa.smoothstep(0.0, p.get('side_groove', 0.16), np.minimum(fa, 1 - fa))
        hh = prof * (0.8 + 0.2 * side)
        tp = sa.smoothstep(0.55, 0.95, fr / edge) * (fr < edge) * side
        jj = _hash(i, j, o['island']) * 2 - 1
        w = sel[y0:y1, x0:x1]
        h[y0:y1, x0:x1] = np.where(w, hh, h[y0:y1, x0:x1])
        tip[y0:y1, x0:x1] = np.where(w, tp, tip[y0:y1, x0:x1])
        jit[y0:y1, x0:x1] = np.where(w, jj, jit[y0:y1, x0:x1])
        tid[y0:y1, x0:x1] = np.where(w, (o['island'] * 100000 + i) * 100000 + (j + 50000), tid[y0:y1, x0:x1]).astype(np.int64)
    # painted veins: scales tuck under them; the vein itself is a soft raised tube
    vein = np.zeros((out, out), F32)
    if 'veins' in p:
        v = p['veins']
        vl = sa.up(sa.lines_src(C, v, ms), out, cv2.INTER_LINEAR)
        vc = sa.smoothstep(0.3, 0.7, vl)
        dv = ndi.distance_transform_edt(vc < 0.5).astype(F32) / ks
        near = 1 - sa.smoothstep(0.3, v.get('clear_texels', 1.2), dv)
        h = h * (1 - near)
        tip = tip * (1 - near)
        vein = sa.gb(vc, v.get('soft', 0.7) * ks)
    h = sa.gb(h, p.get('soften', 0.35) * ks)
    nx, ny = sa.tilt(h, p['amp'], m)
    if 'veins' in p:
        vx, vy = sa.tilt(vein, p['veins'].get('amp', 0.04), m)
        nx, ny = nx + vx, ny + vy
    cav = np.clip((sa.gb(h, 1.2 * ks) - h) * 3.0, 0, 1)
    alb = p.get('alb_tip', 0.02) * tip - p.get('alb_tuck', 0.02) * cav + p.get('alb_jitter', 0.012) * jit
    C['_tile_id'] = tid
    return dict(nx=nx, ny=ny, alb=alb, cav=cav * p.get('cav', 0.3), h=h)


sa.TREAT['wing_scales'] = t_wing_scales
