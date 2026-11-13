# SPDX-License-Identifier: MIT
r"""Faithful detail synthesis for tiling environment textures (CPU only, numpy + cv2), local use.

For each texture of the chosen assets in build/hd/cache, writes <tex>.x2_detail.png next to the .orig.png:
  albedo : Lanczos 2x, plus a fine band made from the texture's OWN finest band-pass detail, re-used one octave finer
           (the band is tiled 2x2 with a half-tile shift, so it wraps like the texture and its statistics, colours and
           grain match the original), plus a mild unsharp. Detail is zero-mean, so the average colour does not move.
  normal : Lanczos 2x on the xy components, plus a bump term from the fine albedo band (cracks, grain, fibres
           catch the light), renormalised (z is rebuilt by the shader, so it stays in range).
  mask / emissive / other : plain Lanczos 2x (no invented detail).
Nothing is AI generated; nothing outside the texture's own content is added.

  python -E -P scripts\hd\detail_boost.py --assets florio_ground,florio_rock [--amount 0.8] [--force]
"""
import argparse, json, sys
from pathlib import Path
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / 'build' / 'hd' / 'cache'
OUT = 'x2_detail'


def load(p):
    return cv2.cvtColor(cv2.imread(str(p), cv2.IMREAD_UNCHANGED), cv2.COLOR_BGRA2RGBA).astype(np.float32) / 255


def save(p, a):
    cv2.imwrite(str(p), cv2.cvtColor((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), cv2.COLOR_RGBA2BGRA))


def up2(a):
    return cv2.resize(a, (a.shape[1] * 2, a.shape[0] * 2), interpolation=cv2.INTER_LANCZOS4)


def wrap_blur(a, sigma):
    k = int(sigma * 4) | 1
    pad = k
    b = cv2.copyMakeBorder(a, pad, pad, pad, pad, cv2.BORDER_WRAP)
    b = cv2.GaussianBlur(b, (0, 0), sigma)
    return b[pad:-pad, pad:-pad]


def smear_mask(rgb, run=12, tol=1.5 / 255):
    """1 where the texture is extruded padding (a pixel repeated along x or y for >= run texels, e.g. the smeared borders of rock
    atlases); 0 elsewhere. The band must not be added there: sharpening a smear makes diagonal zigzag streaks (Florio cliff,
    2026-11-13). Result is dilated and softened."""
    y = rgb
    m = np.zeros(y.shape[:2], np.float32)
    for ax in (0, 1):
        d = (np.abs(np.diff(y, axis=ax, append=y.take([-1], axis=ax))).max(-1) < tol)          # pixel equals its next neighbour
        d = d.astype(np.float32)
        k = (run, 1) if ax == 0 else (1, run)
        full = cv2.boxFilter(d, -1, k, anchor=(0, 0), normalize=True, borderType=cv2.BORDER_REFLECT) > 0.999   # run texels all equal
        m = np.maximum(m, cv2.dilate(full.astype(np.float32), np.ones((run + 1, run + 1), np.uint8)))
    for sgn in (1, -1):                                                                  # diagonal smear (corner rays of the atlas)
        sh = np.roll(y, (-1, -sgn), axis=(0, 1))
        d = (np.abs(y - sh).max(-1) < tol).astype(np.float32)
        kern = np.eye(run, dtype=np.float32)[::sgn] / run
        full = cv2.filter2D(d, -1, kern, anchor=(0, 0), borderType=cv2.BORDER_REFLECT) > 0.999
        m = np.maximum(m, cv2.dilate(full.astype(np.float32), np.ones((run + 1, run + 1), np.uint8)))
    return cv2.GaussianBlur(m, (0, 0), 1.5)


def fine_band(rgb, amount):
    """Detail at 2x resolution: the original's finest band, one octave finer, tiled with a shift (zero mean)."""
    d = rgb - wrap_blur(rgb, 1.1)                              # finest band of the source (wavelength ~2-6 source px)
    h, w = d.shape[:2]
    t = np.tile(d, (2, 2, 1))                                  # 2x2 tile = same band at half the wavelength in output px
    t = np.roll(t, (h // 2 + 7, w // 2 + 13), axis=(0, 1))
    # keep it where the source has texture; flat painted areas (low local energy) get less
    en = wrap_blur(np.abs(d).mean(-1), 6.0)
    en = np.tile(en.reshape(h, w, 1), (2, 2, 1))
    en = np.roll(en, (h // 2 + 7, w // 2 + 13), axis=(0, 1))
    g = np.clip(en / (en.mean() + 1e-6), 0.2, 1.6)
    band = t * g * amount
    return band, band.mean(-1)


def do_albedo(a, amount):
    rgb, al = a[..., :3], a[..., 3:]
    u = up2(rgb)
    keep = (1 - cv2.resize(smear_mask(rgb), (u.shape[1], u.shape[0]), interpolation=cv2.INTER_LINEAR))[..., None]   # no detail on extruded padding
    band, y = fine_band(rgb, amount * 1.4)
    band = band * keep
    y = y * keep[..., 0]
    u = u + band
    sharp = u - cv2.GaussianBlur(u, (0, 0), 1.0)
    u = u + 0.45 * sharp * amount * keep
    out = np.concatenate([np.clip(u, 0, 1), np.clip(up2(al).reshape(u.shape[0], u.shape[1], 1), 0, 1)], -1)
    return out, y


def do_normal(a, y, strength):
    xy = a[..., :2] * 2 - 1
    xy = up2(xy)
    if y is not None:
        gx = cv2.Sobel(y, cv2.CV_32F, 1, 0, ksize=3) / 8
        gy = cv2.Sobel(y, cv2.CV_32F, 0, 1, ksize=3) / 8
        xy = xy + strength * np.stack([-gx, -gy], -1) * 2.0
    n = np.sqrt((xy ** 2).sum(-1, keepdims=True))
    xy = np.where(n > 0.999, xy / np.maximum(n, 1e-6) * 0.999, xy)
    z = np.sqrt(np.clip(1 - (xy ** 2).sum(-1, keepdims=True), 0, 1))
    return np.concatenate([xy * 0.5 + 0.5, z * 0.5 + 0.5, np.ones_like(z)], -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', required=True)
    ap.add_argument('--amount', type=float, default=0.8)
    ap.add_argument('--bump', type=float, default=1.0)
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--textures', default='', help='comma list of texture names (default: all of the asset)')
    a = ap.parse_args()
    only = set(x for x in a.textures.split(',') if x)
    man = json.loads((CACHE / 'manifest.json').read_text())
    want = a.assets.split(',')
    for asset in man['assets']:
        if asset['id'] not in want:
            continue
        d = CACHE / asset['id'] / 'textures'
        ys = {}
        order = sorted(asset['textures'], key=lambda t: t['role'] != 'albedo')   # albedo first: its fine band feeds the normal
        for t in order:
            if only and t['name'].rsplit('_', 1)[0] not in {o.rsplit('_', 1)[0] for o in only}:
                continue
            src = d / (t['name'] + '.orig.png')
            dst = d / (t['name'] + '.' + OUT + '.png')
            if dst.exists() and not a.force:
                continue
            im = load(src)
            stem = t['name'].rsplit('_', 1)[0]
            if t['role'] == 'albedo':
                out, y = do_albedo(im, a.amount)
                ys[stem] = y
            elif t['role'] == 'normal':
                y = ys.get(stem)
                if y is not None and y.shape[0] != im.shape[0] * 2:
                    y = cv2.resize(y, (im.shape[1] * 2, im.shape[0] * 2), interpolation=cv2.INTER_AREA)
                out = do_normal(im, y, a.bump)
            else:
                out = np.clip(up2(im), 0, 1)
            save(dst, out)
            print(asset['id'], t['name'], t['role'], im.shape[1], '->', out.shape[1], flush=True)


if __name__ == '__main__':
    main()
