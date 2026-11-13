# SPDX-License-Identifier: MIT
r"""Crop one Pokemon from an original/new 4K frame pair, write 1:1 and 2x crops and a side-by-side sheet.
usage: python -I scripts\hd\mat_crop_pair.py <orig.png|bmp> <new.png|bmp> <x,y,w,h> <species> [--out analysis\mods\compare\4k]
Writes mat_<species>_{orig,new}.png (1:1), mat_<species>_{orig,new}_2x.png and mat_<species>_pair.png (orig | new, 2x, labelled)."""
import argparse, sys
from pathlib import Path
import site; sys.path.append(site.getusersitepackages())  # user-site numpy under python -I
from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument('orig'); ap.add_argument('new'); ap.add_argument('box'); ap.add_argument('species')
ap.add_argument('--out', default='analysis/mods/compare/4k')
a = ap.parse_args()
x, y, w, h = [int(v) for v in a.box.split(',')]
out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
ims = {}
for k, f in (('orig', a.orig), ('new', a.new)):
    c = Image.open(f).convert('RGB').crop((x, y, x + w, y + h))
    c.save(out / ('mat_%s_%s.png' % (a.species, k)))
    c2 = c.resize((w * 2, h * 2), Image.LANCZOS)
    c2.save(out / ('mat_%s_%s_2x.png' % (a.species, k)))
    ims[k] = c2
sheet = Image.new('RGB', (w * 4 + 10, h * 2 + 24), (30, 30, 30))
sheet.paste(ims['orig'], (0, 24)); sheet.paste(ims['new'], (w * 2 + 10, 24))
d = ImageDraw.Draw(sheet); d.text((4, 6), 'original', fill=(255, 255, 255)); d.text((w * 2 + 14, 6), 'new (material)', fill=(255, 255, 255))
sheet.save(out / ('mat_%s_pair.png' % a.species))
print('wrote', out)
