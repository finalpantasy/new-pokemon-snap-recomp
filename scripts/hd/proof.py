# SPDX-License-Identifier: MIT
"""Textured before/after proof sheets: analysis/hd/tex_<species>.png (local only).
Same camera and scale for both columns. Row 1: full body (original | upscaled | official artwork reference).
Row 2: face close-up (original | upscaled | in-game photo reference).
usage: python -E -P scripts\\hd\\proof.py --assets p001_00:pikachu,p032_00:bidoof [--variant x4_enhanced]
"""
import argparse, sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_textured as rt

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'analysis' / 'hd'
REFS = ROOT / 'build' / 'hd' / 'refs'
N = 768


def label(img, text):
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype('arial.ttf', 26)
    except OSError:
        f = ImageFont.load_default()
    d.rectangle((0, 0, 14 + 15 * len(text), 36), fill=(0, 0, 0))
    d.text((8, 4), text, fill=(255, 255, 255), font=f)
    return img


def fit(path, size, bg=(40, 40, 44)):
    if not path.exists():
        return Image.new('RGB', (size, size), bg)
    im = Image.open(path).convert('RGBA')
    c = Image.new('RGBA', im.size, bg + (255,))
    c.alpha_composite(im)
    im = c.convert('RGB')
    im.thumbnail((size, size), Image.LANCZOS)
    out = Image.new('RGB', (size, size), bg)
    out.paste(im, ((size - im.width) // 2, (size - im.height) // 2))
    return out


def face_cam(m):
    pts = []
    for pr in m.prims:
        if pr['tex'] and 'Eye' in pr['tex'] and 'Eyes' not in pr['tex'][:0]:
            pts.append(pr['P'])
    if not pts:
        h = m.hi - m.lo
        return ((m.lo[0] + m.hi[0]) / 2, m.hi[1] - 0.2 * h[1]), 0.4 * h[1]
    P = np.concatenate(pts)
    c = (P.min(0) + P.max(0)) / 2
    span = max(P[:, 0].max() - P[:, 0].min(), 0.04)
    return (c[0], c[1]), span * 1.7


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', required=True, help='asset_id:species,...')
    ap.add_argument('--variant', default='x4_enhanced')
    ap.add_argument('--face-scale', type=float, default=0)
    ap.add_argument('--face-up', type=float, default=0, help='shift the face camera up by this fraction of its width')
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for item in a.assets.split(','):
        aid, sp = item.split(':')
        m = rt.Model(aid)
        h = m.hi - m.lo
        cen = ((m.lo[0] + m.hi[0]) / 2, (m.lo[1] + m.hi[1]) / 2)
        sc = max(h[0], h[1]) * 1.1
        fc, fs = face_cam(m)
        fc = (fc[0], fc[1] + a.face_up * fs)
        if a.face_scale:
            fs = a.face_scale
        tiles = []
        for cam in ((cen, sc), (fc, fs)):
            for var, nm in (('orig', 'ORIGINAL'), (a.variant, 'UPSCALED ' + a.variant)):
                tiles.append(label(Image.fromarray(rt.render(m, var, cam[0], cam[1], N)), nm))
        ref = REFS / sp
        snaps = sorted(ref.glob('snap_*'))
        sheet = Image.new('RGB', (N * 3, N * 2), (30, 30, 30))
        sheet.paste(tiles[0], (0, 0)); sheet.paste(tiles[1], (N, 0))
        sheet.paste(label(fit(ref / 'official_artwork.png', N), 'REF: official artwork'), (2 * N, 0))
        sheet.paste(tiles[2], (0, N)); sheet.paste(tiles[3], (N, N))
        sheet.paste(label(fit(snaps[-1], N) if snaps else Image.new('RGB', (N, N)), 'REF: in-game photo'), (2 * N, N))
        f = OUT / ('tex_%s.png' % sp)
        sheet.save(f)
        print('wrote', f)


if __name__ == '__main__':
    main()
