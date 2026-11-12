# SPDX-License-Identifier: MIT
r"""Builds two texture mod packs for Pikachu (species 001) under the mods root (default %APPDATA%\SnapRecomp\mods):
  golden_pikachu  body fur recoloured toward gold (hue/saturation shift; eyes and cheeks are separate textures and untouched)
  hd_pikachu      all Pikachu textures from the Real-ESRGAN x4 cache outputs (build/hd/cache/p001_00), re-encoded to nutexb
Pack layout follows runtime/mods/mods_core.h: <pack>/nx/Pack/Characters/master/CH_001.drp/<entry>.nutexb + pack.json.
usage: python -E -P scripts\hd\make_pikachu_packs.py [--root DIR] [--variant x4_realesrgan]"""
import argparse, json, os, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_cache as bc  # noqa: E402
import nutexb_encode as ne  # noqa: E402

SRC = bc.PACK / 'Characters' / 'master' / 'CH_001.drp'
REL = 'nx/Pack/Characters/master/CH_001.drp'


def write_pack(root, pid, name, desc):
    d = root / pid
    (d / REL).mkdir(parents=True, exist_ok=True)
    (d / 'pack.json').write_text(json.dumps({'name': name, 'version': '1.0.0', 'author': 'local', 'description': desc}, indent=1))
    return d / REL


def golden(png_in, out_png):
    im = Image.open(png_in).convert('RGBA')
    rgb = im.convert('RGB')
    hsv = np.asarray(rgb.convert('HSV')).astype(np.float32)
    h, s, v = hsv[..., 0], hsv[..., 1] / 255, hsv[..., 2] / 255
    wgt = np.clip((s - 0.25) / 0.25, 0, 1)             # only saturated (yellow) fur is shifted; black ear tips stay black
    h2 = h - 9.0 * wgt * (255 / 360)                    # about 9 degrees toward orange
    s2 = np.clip(s * (1 + 0.12 * wgt), 0, 1)
    v2 = np.clip(v * (1 - 0.10 * wgt), 0, 1)
    out = np.stack([np.mod(h2, 255), s2 * 255, v2 * 255], -1).round().clip(0, 255).astype(np.uint8)
    rgb2 = Image.fromarray(out, 'HSV').convert('RGB')
    res = Image.merge('RGBA', (*rgb2.split(), im.getchannel('A')))
    res.save(out_png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.environ.get('APPDATA', '.'), 'SnapRecomp', 'mods'))
    ap.add_argument('--variant', default='x4_realesrgan')
    a = ap.parse_args()
    root = Path(a.root)
    tmp = Path(tempfile.mkdtemp(prefix='pikapack_'))
    # golden
    dst = write_pack(root, 'golden_pikachu', 'Golden Pikachu', 'Recolors the Pikachu body fur toward gold. Texture CH_001_00_0_A00_Body_dif only.')
    src = SRC / 'CH_001_00_0_A00_Body_dif.nutexb'
    px, _ = bc.decode_nutexb(src)
    Image.fromarray(px, 'RGBA').save(tmp / 'body.png')
    golden(tmp / 'body.png', tmp / 'body_gold.png')
    r = ne.encode(tmp / 'body_gold.png', src, dst / src.name)
    print('golden_pikachu:', src.name, r)
    # hd
    dst = write_pack(root, 'hd_pikachu', 'HD Pikachu (Real-ESRGAN x4)', 'All Pikachu textures upscaled 4x (albedo: Real-ESRGAN, normal and mask: Lanczos).')
    tex = Path(bc.CACHE) / 'p001_00' / 'textures'
    for f in sorted(SRC.glob('CH_001_00_0_A00_*.nutexb')):
        up = tex / (f.stem + '.' + a.variant + '.png')
        if not up.exists():
            print('skip (no upscale yet):', f.name)
            continue
        r = ne.encode(up, f, dst / f.name)
        print('hd_pikachu:', f.name, r)


if __name__ == '__main__':
    main()
