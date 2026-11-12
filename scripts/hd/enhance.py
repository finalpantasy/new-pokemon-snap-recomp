# SPDX-License-Identifier: MIT
"""Texture enhancement for albedo maps (phase 1, local only).

For each albedo texture that has x4_realesrgan and/or x4_anime variants in build/hd/cache:
  1. pick the variant that stays closer to the source (colour error after downscaling, with a sharpness requirement);
  2. colour lock: keep the AI result's detail but take its low frequencies from the Lanczos upscale of the original,
     which removes colour shifts and blotches;
  3. clamp to the local range of the original (+margin) so the AI cannot invent new features or ring around eyes
     and markings;
  4. mild edge sharpening where the source has strong edges (eyes, markings, outlines);
  5. fade to the Lanczos result near the image border (no seams or wrapped-edge artefacts);
  6. keep the original alpha (Lanczos upscaled, already in the variant file).
Result is written as <tex>.x4_enhanced.png and a per-texture report to build/hd/enhance_report.json.
usage: python -E -P scripts\\hd\\enhance.py --assets p001_00,p032_00 [--force]
"""
import argparse, json, sys
from pathlib import Path
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / 'build' / 'hd' / 'cache'
REPORT = ROOT / 'build' / 'hd' / 'enhance_report.json'
CANDS = ['x4_realesrgan', 'x4_anime']


def rd(p):
    return cv2.cvtColor(cv2.imread(str(p), cv2.IMREAD_UNCHANGED), cv2.COLOR_BGRA2RGBA).astype(np.float32) / 255


def enhance_one(orig_p, variants, out_p):
    o = rd(orig_p)
    h, w = o.shape[:2]
    S = 4
    L = cv2.resize(o[..., :3], (w * S, h * S), interpolation=cv2.INTER_LANCZOS4)
    L = np.clip(L, 0, 1)
    lap = lambda a: float(cv2.Laplacian(cv2.cvtColor(a, cv2.COLOR_RGB2GRAY), cv2.CV_32F).var())
    base_sharp = lap(L)
    scores = {}
    imgs = {}
    for v, p in variants.items():
        a = rd(p)
        imgs[v] = a
        small = cv2.resize(a[..., :3], (w, h), interpolation=cv2.INTER_AREA)
        err = float(np.abs(small - o[..., :3]).mean())
        scores[v] = dict(color_err=round(err, 5), sharp_ratio=round(lap(a[..., :3]) / max(base_sharp, 1e-9), 2))
    # choose: lowest colour error among variants that are at least as sharp as Lanczos
    ok = [v for v in scores if scores[v]['sharp_ratio'] >= 1.0] or list(scores)
    pick = min(ok, key=lambda v: scores[v]['color_err'])
    A = imgs[pick]
    alpha = A[..., 3:4]
    rgb = A[..., :3]
    # colour lock (low frequencies from the original)
    sig = 6.0
    rgb = rgb - cv2.GaussianBlur(rgb, (0, 0), sig) + cv2.GaussianBlur(L, (0, 0), sig)
    # clamp to local range of the original upscale (+margin): no new features, no ringing
    k = np.ones((9, 9), np.uint8)
    m = 0.05
    rgb = np.clip(rgb, cv2.erode(L, k) - m, cv2.dilate(L, k) + m)
    # edge sharpening where the source has strong edges
    g = cv2.cvtColor(L, cv2.COLOR_RGB2GRAY)
    gx, gy = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    mag = cv2.GaussianBlur(np.sqrt(gx * gx + gy * gy), (0, 0), 3)
    edge = np.clip(mag / (np.percentile(mag, 98) + 1e-6), 0, 1)[..., None]
    rgb = rgb + 0.45 * (rgb - cv2.GaussianBlur(rgb, (0, 0), 1.6)) * edge
    # border fade to Lanczos
    bw = 12
    yy = np.minimum(np.arange(h * S), np.arange(h * S)[::-1])[:, None]
    xx = np.minimum(np.arange(w * S), np.arange(w * S)[::-1])[None, :]
    t = np.clip(np.minimum(yy, xx) / bw, 0, 1)[..., None].astype(np.float32)
    rgb = L * (1 - t) + rgb * t
    rgb = np.clip(rgb, 0, 1)
    out = np.concatenate([rgb, alpha], -1)
    cv2.imwrite(str(out_p), cv2.cvtColor((out * 255 + 0.5).astype(np.uint8), cv2.COLOR_RGBA2BGRA))
    # post-check: downscaled enhanced vs original
    back = cv2.resize(rgb, (w, h), interpolation=cv2.INTER_AREA)
    return dict(pick=pick, candidates=scores, final_color_err=round(float(np.abs(back - o[..., :3]).mean()), 5),
                final_sharp_ratio=round(lap(rgb) / max(base_sharp, 1e-9), 2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', required=True)
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    rep = json.loads(REPORT.read_text()) if REPORT.exists() else {}
    for aid in a.assets.split(','):
        info = json.loads((CACHE / aid / 'info.json').read_text())
        for n, meta in info['textures'].items():
            if meta.get('role') != 'albedo' or 'error' in meta:
                continue
            td = CACHE / aid / 'textures'
            vs = {v: td / ('%s.%s.png' % (n, v)) for v in CANDS if (td / ('%s.%s.png' % (n, v))).exists()}
            if not vs:
                print('skip (no AI variants yet):', aid, n)
                continue
            out = td / ('%s.x4_enhanced.png' % n)
            r = enhance_one(td / (n + '.orig.png'), vs, out)
            rep[aid + '/' + n] = r
            print(aid, n, r['pick'], r['final_color_err'], r['final_sharp_ratio'])
    REPORT.write_text(json.dumps(rep, indent=1))


if __name__ == '__main__':
    main()
