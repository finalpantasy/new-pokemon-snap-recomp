# Turns the overlay frame dumps of run_options_test.ps1 (analysis/ui/opt_dump_a, BMP names f<N>_p<present>_t<seconds>.bmp) into analysis/ui/f1_<page>.png.
# Run with: python -E -P tests/hle/opt_shots.py   (needs Pillow). The page of each frame is read from the yellow tab in the tab bar (1920x1080 window), so
# frame timing does not matter; of the frames of one page the last one is the settled one. Graphics has three (top, middle, bottom of the list).
import re
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[2]
src = root / 'analysis' / 'ui' / 'opt_dump_a'
tab_x = {'audio': 737, 'graphics': 833, 'display': 940, 'photos': 1036, 'mods': 1123}   # centers of the tabs at 1920x1080, y about 199

def selected(im):
    px = im.load()
    xs = [x for x in range(200, 1500) for y in (192, 199, 206) if abs(px[x, y][0] - 248) < 6 and abs(px[x, y][1] - 213) < 8 and abs(px[x, y][2] - 71) < 20]
    if len(xs) < 40: return None
    c = sum(xs) / len(xs)
    return min(tab_x, key=lambda k: abs(tab_x[k] - c))

frames = sorted((float(re.match(r'f\d+_p\d+_t(\d+(?:\.\d+)?)', f.name).group(1)), f) for f in src.glob('f*_p*_t*.bmp'))
by = {}
for t, f in frames:
    im = Image.open(f).convert('RGB')
    if im.size != (1920, 1080): continue
    k = selected(im)
    if k: by.setdefault(k, []).append((t, f, im))
for k, lst in by.items():
    if k == 'graphics':
        n = len(lst)
        picks = [('graphics', lst[max(0, n // 4)]), ('graphics_mid', lst[n // 2]), ('graphics_bottom', lst[-1])]
        for name, (t, f, im) in picks:
            im.save(root / 'analysis' / 'ui' / f'f1_{name}.png'); print(name, f.name)
    else:
        t, f, im = lst[-1]
        im.save(root / 'analysis' / 'ui' / f'f1_{k}.png'); print(k, f.name)
