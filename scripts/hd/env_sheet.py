# SPDX-License-Identifier: MIT
r"""Before/after sheet per area: analysis/hd/env_<area>.png (all rows) and env_<area>_pN.png (6 rows each, for review).
Per texture: dif original | dif HD, nor original | nor HD, par original | par HD. The crop is the same footprint of the texture
(64 source texels, picked where the original has the most structure); the original is shown bicubic 4x (what the game shows),
the HD at its native pixels (4x: 1:1) or bicubic when it is only 2x or 1x. Label = class and variant used.
  python -E -P scripts\hd\env_sheet.py <area>"""
import sys
from pathlib import Path
import cv2
import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import env_batch as eb  # noqa: E402
import detail_boost as db  # noqa: E402

C = 256


def crop(path, x, y, fp, mode):
    im = np.asarray(Image.open(path).convert('RGB'))
    h, w = im.shape[:2]
    s = w / SRC_W[0]
    x0, y0, n = int(x * s), int(y * (h / SRC_H[0])), max(4, int(fp * s))
    c = im[y0:y0 + n, x0:x0 + n]
    return Image.fromarray(c).resize((C, C), Image.BICUBIC if mode else Image.BICUBIC)


SRC_W = [1]
SRC_H = [1]


def best_region(orig_png, fp=64):
    a = db.load(orig_png)[..., :3]
    y = a.mean(-1)
    keep = 1 - db.smear_mask(a)
    best, bp = -1, (0, 0)
    h, w = y.shape
    for yy in range(0, max(1, h - fp), 32):
        for xx in range(0, max(1, w - fp), 32):
            v = float(y[yy:yy + fp, xx:xx + fp].std() * keep[yy:yy + fp, xx:xx + fp].mean() ** 4)
            if v > best:
                best, bp = v, (xx, yy)
    return bp


def main():
    area = sys.argv[1]
    rows = []
    for it in eb.SETS[area]['items']:
        d = eb.tdir(it)
        stem = it['dif'][:-4]
        dif_hd = eb.choose_dif(it)
        if dif_hd is None:
            continue
        o = d / (it['dif'] + '.orig.png')
        wo, ho = Image.open(o).size
        SRC_W[0], SRC_H[0] = wo, ho
        x, y = best_region(o)
        cells = [crop(o, x, y, 64, 0), crop(dif_hd, x, y, 64, 1)]
        for role in ('nor', 'par'):
            po = d / (stem + '_' + role + '.orig.png')
            ph = next((d / (stem + '_' + role + '.' + v + '.png') for v in ('x2_env', 'x2_detail') if (d / (stem + '_' + role + '.' + v + '.png')).exists()), None)
            for p in (po, ph):
                if p is not None and p.exists():
                    wi, hi = Image.open(p).size
                    SRC_W[0], SRC_H[0] = (wi if p == po else wi * wo // max(1, Image.open(po).size[0]) if False else None) or wo, ho
                    # map the dif footprint into this map (maps may differ in size): use fractions
                    im = Image.open(p).convert('RGB')
                    fx, fy = x / wo, y / ho
                    n = int(64 / wo * im.width)
                    cells.append(im.crop((int(fx * im.width), int(fy * im.height), int(fx * im.width) + n, int(fy * im.height) + n)).resize((C, C), Image.BICUBIC))
                else:
                    cells.append(Image.new('RGB', (C, C), (40, 40, 40)))
        lab = '%s [%s] %s' % (it['stem'].replace('SM_ENV_', ''), it['class'], dif_hd.name.split('.')[-2])
        rows.append((lab, cells))
    out = Path(HERE.parents[1] / 'analysis' / 'hd')
    out.mkdir(parents=True, exist_ok=True)
    heads = ['albedo orig', 'albedo HD', 'normal orig', 'normal HD', 'params orig', 'params HD']

    def render(rs, name):
        W = Image.new('RGB', (6 * C, len(rs) * (C + 16) + 16), (20, 20, 20))
        d = ImageDraw.Draw(W)
        for i, h in enumerate(heads):
            d.text((i * C + 4, 2), h, fill=(180, 180, 180))
        for r, (lab, cells) in enumerate(rs):
            yy = 16 + r * (C + 16)
            d.text((4, yy + 1), lab, fill=(255, 255, 0))
            for i, c in enumerate(cells):
                W.paste(c, (i * C, yy + 16))
        W.save(out / name)

    render(rows, 'env_%s.png' % area)
    for i in range(0, len(rows), 6):
        render(rows[i:i + 6], 'env_%s_p%d.png' % (area, i // 6 + 1))
    print(len(rows), 'rows')


main()
