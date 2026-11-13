# SPDX-License-Identifier: MIT
r"""Normal (_nor) and parameter (_par) maps for the environment packs, derived from the chosen HD albedo and the ORIGINAL maps.
Local use; writes <tex>.x2_env.png next to the .orig.png in build/hd/cache.

  python -E -P scripts\hd\env_author.py <area> [--amount 0.6] [--ao 0.12] [--only <stem>]

Rules (docs/hd-assets.md "Environment retexture"):
- Channel layout checked in the translated StageShader pixel shaders (scripts/hd/env_channels.py): _nor uses XY only (Z rebuilt),
  _par is ROSM like the Pokemon materials (R roughness, G occlusion, B specular, A metallic; some shaders read G only).
- Normal = the original normal, resampled 2x (Lanczos, same size when the original is 2048), plus a bump from the band of the HD
  albedo that the original did not have. The bump's sign and scale are FITTED to the original data (regression of the original
  normal on the gradient of the original albedo luminance). If the two are not correlated (painted atlases, flat colour) no bump is
  added: nothing is invented.
- _par: R, B, A resampled only. G (occlusion) gets a small cavity darkening from the same albedo band (at most --ao), only where a
  bump was added.
"""
import argparse, json, sys
from pathlib import Path
import cv2
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import env_batch as eb  # noqa: E402
import detail_boost as db  # noqa: E402


CAP = 0.35


def lum(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def rs(a, w, h):
    return cv2.resize(a, (w, h), interpolation=cv2.INTER_LANCZOS4 if w > a.shape[1] else cv2.INTER_AREA)


def wblur(a, s):
    return db.wrap_blur(a, s)


def fit(nxy, y):
    """Height model: nx = -s * dY/dx and ny = +s * dY/d(row) (Y = original luminance). Least squares over both axes.
    Returns (s, corr); s < 0 means dark = high. Used to give the HD albedo band a bump consistent with the original normal."""
    ys = cv2.GaussianBlur(y, (0, 0), 1.0)
    gx = cv2.Sobel(ys, cv2.CV_32F, 1, 0, ksize=3) / 8
    gy = cv2.Sobel(ys, cv2.CV_32F, 0, 1, ksize=3) / 8
    u = np.concatenate([(-gx).ravel(), gy.ravel()]); v = np.concatenate([nxy[..., 0].ravel(), nxy[..., 1].ravel()])
    u = u - u.mean(); v = v - v.mean()
    c = float((u * v).mean() / (u.std() * v.std() + 1e-9))
    return float(c * v.std() / (u.std() + 1e-9)), c


def author(it, amount, ao_amt, log):
    eb.ensure_maps(it)
    d = eb.tdir(it)
    stem = it['dif'][:-4]
    dif_hd = eb.choose_dif(it)
    if dif_hd is None:
        log('no albedo variant', it['dif']); return
    alb = db.load(dif_hd)[..., :3]
    alb0 = db.load(d / (it['dif'] + '.orig.png'))[..., :3]
    y0 = lum(alb0)
    nrm_p, par_p = d / (stem + '_nor.orig.png'), d / (stem + '_par.orig.png')
    bump_on = False
    res = {}
    if nrm_p.exists():
        n0 = db.load(nrm_p)
        h0, w0 = n0.shape[:2]
        sn = 2 if w0 <= 1024 else 1
        w, h = w0 * sn, h0 * sn
        xy0 = n0[..., :2] * 2 - 1
        xy = rs(xy0, w, h) if sn > 1 else xy0.copy()
        y0r = rs(y0, w0, h0)
        sfit, cc = fit(xy0, rs(y0, w0, h0) if y0.shape != (h0, w0) else y0)
        ok = abs(cc) > 0.12
        cx = cy = cc
        yh = rs(lum(alb), w, h)
        # only frequencies the ORIGINAL normal could not carry: the albedo's detail above one source texel (the original normal already
        # has the coarser relief; HD minus original would instead cancel grain that Real-ESRGAN removes)
        band = wblur(yh, 0.5 * sn) - wblur(yh, 1.2 * sn)
        keep = 1 - rs(db.smear_mask(alb0), w, h)
        band = band * keep
        if ok:
            gx = cv2.Sobel(band, cv2.CV_32F, 1, 0, ksize=3) / 8 * sn      # per source texel
            gy = cv2.Sobel(band, cv2.CV_32F, 0, 1, ksize=3) / 8 * sn
            # only the part of the band that the original did not already carry: remove the original's own gradient share
            bump = sfit * np.stack([-gx, gy], -1)                          # sfit carries the fitted polarity and slope
            k = min(amount, CAP * float(xy0.std()) / (float(bump.std()) + 1e-9))   # restraint: the new relief is at most CAP of the original's
            xy = xy + k * bump
            bump_on = True
        n = np.sqrt((xy ** 2).sum(-1, keepdims=True))
        xy = np.where(n > 0.98, xy / np.maximum(n, 1e-6) * 0.98, xy)
        z = np.sqrt(np.clip(1 - (xy ** 2).sum(-1, keepdims=True), 0, 1))
        out = np.concatenate([xy * 0.5 + 0.5, z * 0.5 + 0.5, np.ones_like(z)], -1)
        db.save(d / (stem + '_nor.x2_env.png'), out)
        res['nor'] = (w, h, round(cx, 2), round(cy, 2), 'bump' if bump_on else 'resample only')
    if par_p.exists():
        p0 = db.load(par_p)
        h0, w0 = p0.shape[:2]
        sn = 2 if w0 <= 1024 else 1
        w, h = w0 * sn, h0 * sn
        out = rs(p0, w, h) if sn > 1 else p0.copy()
        if bump_on and ao_amt > 0:
            yh = rs(lum(alb), w, h)
            cav = np.clip(wblur(yh, 8.0 * sn) - wblur(yh, 1.5 * sn), 0, None)     # broad cavities only, no grain speckle
            cav = np.clip(cav / (np.percentile(cav, 98) + 1e-6), 0, 1)
            cav = cav * (1 - rs(db.smear_mask(alb0), w, h))
            out[..., 1] = out[..., 1] * (1 - ao_amt * cav)
        db.save(d / (stem + '_par.x2_env.png'), np.clip(out, 0, 1))
        res['par'] = (w, h, 'ao' if bump_on and ao_amt > 0 else 'resample only')
    log(it['stem'], res)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('area')
    ap.add_argument('--amount', type=float, default=1.0)
    ap.add_argument('--ao', type=float, default=0.10)
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    for it in eb.SETS[a.area]['items']:
        if a.only and it['stem'] != a.only:
            continue
        author(it, a.amount, a.ao, lambda *x: print(*x, flush=True))


if __name__ == '__main__':
    main()
