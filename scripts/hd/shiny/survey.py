# SPDX-License-Identifier: MIT
"""Per-species color survey of the user's own body textures (local tool for writing palettes; prints hue clusters, no pixels).
  python -E -P scripts\hd\shiny\survey.py [out.json]"""
import json, os, re, sys
import numpy as np
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts' / 'hd')); sys.path.insert(0, str(ROOT / 'tests' / 'nvn')); sys.path.insert(0, str(ROOT / 'scripts' / 'hd' / 'shiny'))
from build_cache import decode_nutexb, species_name  # noqa: E402
import importlib.util
spec = importlib.util.spec_from_file_location('msp', str(ROOT / 'scripts' / 'hd' / 'make_shiny_pack.py'))
src = open(spec.origin).read().replace('\nmain()\n', '\n')
ns = {'__file__': spec.origin, '__name__': 'msp'}
exec(compile(src, spec.origin, 'exec'), ns)
to_oklab = ns['to_oklab']
root = ROOT / 'extracted' / 'drp-v262144' / 'nx' / 'Pack' / 'Characters' / 'master'
out = {}
for d in sorted(os.listdir(root)):
    n = int(d[3:6])
    texs = [f for f in os.listdir(root / d) if f.endswith('_dif.nutexb') and not f.startswith('FX') and 'ye' not in f]
    H = np.zeros(36); A = []; tot = 0
    for f in texs:
        px, _ = decode_nutexb(root / d / f)
        px = px[::4, ::4]
        lab = to_oklab(px[..., :3].reshape(-1, 3)); C = np.hypot(lab[:, 1], lab[:, 2]); L = lab[:, 0]
        h = (np.degrees(np.arctan2(lab[:, 2], lab[:, 1])) % 360)
        m = C >= 0.04
        H += np.bincount((h[m] // 10).astype(int), minlength=36)
        A.append(L[~m]); tot += len(C)
    A = np.concatenate(A) if A else np.zeros(1)
    top = np.argsort(H)[::-1][:3]
    out[n] = dict(name=species_name(n), n=len(texs), chroma=float(H.sum() / max(tot, 1)), achroma_L=float(A.mean()),
                  top=[(int(t) * 10 + 5, round(float(H[t] / max(H.sum(), 1)), 2)) for t in top])
    print(n, out[n]['name'], len(texs), 'chromatic %.0f%%' % (out[n]['chroma'] * 100), 'achromL %.2f' % out[n]['achroma_L'], out[n]['top'], flush=True)
if len(sys.argv) > 1:
    json.dump(out, open(sys.argv[1], 'w'))
