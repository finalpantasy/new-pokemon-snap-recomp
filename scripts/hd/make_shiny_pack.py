# SPDX-License-Identifier: MIT
r"""Builds the local shiny texture pack (docs/shiny-mod-plan.md section 3.3) from the user's own extracted textures.

  python -E -P scripts\hd\make_shiny_pack.py [--species 7,16,...] [--out DIR] [--src <Characters\master dir>] [--sheet DIR] [--jobs N]

Every species in scripts/hd/shiny/shiny_colors.txt (rules in words and hue degrees; palettes.json overrides a species with
explicit regions) gets ALL of its body textures (every *_dif of its model archive except eyes and mouths) recolored in OKLab
(hue rotation, chroma and lightness scale on gated regions; alpha untouched), re-encoded to the original format, size and mip
count, and written to the pack folder (default %APPDATA%\SnapRecomp\mods\shiny-pokemon, outside git) together with
shiny_table.txt: `<key> <w> <h> <mips> <file> <species>`. <key> is FNV-1a 64 of the first 4096 bytes of the texture data (the
bytes the game holds), which the renderer computes the same way (runtime/nvn/nvn_shiny.cpp). `--sheet DIR` also writes a
normal|shiny contact image per species. Nothing is downloaded; no game data is printed.
"""
import argparse, json, os, sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests' / 'nvn'))
sys.path.insert(0, str(ROOT / 'scripts' / 'hd'))
import nutexb_ref as nx  # noqa: E402
import nutexb_encode as enc  # noqa: E402
from build_cache import decode_nutexb  # noqa: E402

HUES = dict(red=29, orange=55, yellow=100, lime=125, green=145, teal=180, cyan=205, azure=235, blue=262, violet=292, magenta=330, pink=355)
SKIP_TEX = ('Eye', 'Mouth')


def fnv_key(data, n=4096):
    h = 1469598103934665603
    for b in data[:n]:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def s2l(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def l2s(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255.0


M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566], [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099], [0.0259040371, 0.7827717662, -0.8086757660]])


def to_oklab(rgb):
    return np.cbrt(s2l(rgb.astype(np.float64)) @ M1.T) @ M2.T


def from_oklab(lab):
    return l2s(((lab @ np.linalg.inv(M2).T) ** 3) @ np.linalg.inv(M1).T)


def smooth(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def recolor(px, regions):
    """regions: dicts. Hue regions {src_hue:[lo,hi] | all:true, min_c, hue_shift, chroma_scale, light_scale, feather};
    tints {grey:[Lmin,Lmax], hue, chroma}. Returns (rgba, lightness correlation)."""
    lab = to_oklab(px[..., :3])
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    C = np.hypot(a, b)
    H = np.degrees(np.arctan2(b, a)) % 360
    oa, ob, oL = a.copy(), b.copy(), L.copy()
    for r in regions:
        if 'grey' in r:
            lo, hi = r['grey']
            w = smooth((0.06 - C) / 0.04) * smooth((L - lo) / 0.05) * smooth((hi - L) / 0.05)
            th = np.radians(r['hue'])
            oa = oa * (1 - w) + (np.cos(th) * r['chroma']) * w
            ob = ob * (1 - w) + (np.sin(th) * r['chroma']) * w
            continue
        f = r.get('feather', 12)
        if r.get('all'):
            wh = 1.0
        else:
            lo, hi = r['src_hue']
            d = (H - lo) % 360
            span = (hi - lo) % 360
            edge = np.minimum(smooth(1 - (d - span) / f), smooth(1 - (360 - d) / f)) * (d > span)
            wh = np.maximum(np.where(d <= span, 1.0, 0.0), edge)
        wc = smooth((C - r['min_c'] * 0.5) / max(r['min_c'] * 0.5, 1e-6))
        w = wh * wc
        th = np.radians(r['hue_shift'])
        cs, sn = np.cos(th), np.sin(th)
        k = r.get('chroma_scale', 1.0)
        oa = oa * (1 - w) + ((a * cs - b * sn) * k) * w
        ob = ob * (1 - w) + ((a * sn + b * cs) * k) * w
        ls = r.get('light_scale', 1.0)
        if ls != 1.0:
            oL = oL * (1 - w) + (L * ls) * w
    rgb = from_oklab(np.stack([oL, oa, ob], -1))
    res = px.copy()
    res[..., :3] = np.clip(rgb + 0.5, 0, 255).astype(np.uint8)
    return res, float(np.corrcoef(L.ravel(), to_oklab(res[..., :3])[..., 0].ravel())[0, 1])


def hue_of(tok):
    return HUES[tok] if tok in HUES else float(tok)


def parse_rules(rules):
    """'255>25:40:1.1:0.5;grey:0.2-0.9>145:0.07;auto' -> region list (None for skip)."""
    out = []
    for r in rules.split(';'):
        r = r.strip()
        if not r:
            continue
        if r == 'skip':
            return None
        if r == 'auto':
            out.append(dict(all=True, min_c=0.04, hue_shift=150, chroma_scale=1.0))
        elif r.startswith('grey:'):
            rng, rest = r[5:].split('>')
            lo, hi = rng.split('-')
            hue, ch = rest.split(':')
            out.append(dict(grey=[float(lo), float(hi)], hue=hue_of(hue), chroma=float(ch)))
        else:
            head, *opt = r.split(':')
            s, d = head.split('>')
            s, d = hue_of(s), hue_of(d)
            w = float(opt[0]) if len(opt) > 0 else 35.0
            cs = float(opt[1]) if len(opt) > 1 else 1.0
            ls = float(opt[2]) if len(opt) > 2 else 1.0
            out.append(dict(src_hue=[(s - w) % 360, (s + w) % 360], min_c=0.04, hue_shift=(d - s) % 360, chroma_scale=cs, light_scale=ls, feather=12))
    return out


def load_species():
    sp = {}
    for ln in (ROOT / 'scripts' / 'hd' / 'shiny' / 'shiny_colors.txt').read_text(encoding='utf-8').splitlines():
        if not ln.strip() or ln.startswith('#'):
            continue
        n, name, desc, rules = ln.split('|')
        sp[int(n)] = dict(name=name, desc=desc, regions=parse_rules(rules))
    pal = json.load(open(ROOT / 'scripts' / 'hd' / 'shiny' / 'palettes.json'))
    for k, v in pal.items():   # explicit overrides: regions, optional tex list
        if k.startswith('_'):
            continue
        e = sp.setdefault(int(k), dict(name=v['name'], desc='override'))
        e['regions'] = v['regions']
        e['name'] = v['name']
        if 'tex' in v:
            e['tex'] = v['tex']
    return sp


def build_species(args):
    n, e, srcdir, outdir, sheet = args
    from PIL import Image
    arch = 'CH_%03d' % n
    d = Path(srcdir) / (arch + '.drp')
    if e.get('tex'):
        stems = [t.split('/')[1] for t in e['tex']]
    else:
        stems = sorted(f[:-7] for f in os.listdir(d) if f.endswith('_dif.nutexb') and not any(s in f for s in SKIP_TEX))
    rows, lines, pairs = [], [], []
    for stem in stems:
        src = d / (stem + '.nutexb')
        data = src.read_bytes()
        t = nx.parse(str(src))
        key = '%016x' % fnv_key(data)
        px, meta = decode_nutexb(src)
        res, corr = recolor(px, e['regions'])
        changed = float((res[..., :3] != px[..., :3]).any(-1).mean())
        assert (res[..., 3] == px[..., 3]).all()
        tmp = Path(outdir) / ('_tmp_%d.png' % n)
        Image.fromarray(res).save(tmp)
        fn = '%s_shiny.nutexb' % stem
        info = enc.encode(str(tmp), str(src), str(Path(outdir) / fn))
        tmp.unlink()
        assert (info['w'], info['h'], info['mips']) == (t['w'], t['h'], t['mips'])
        rows.append("%s %d %d %d %s %s" % (key, t['w'], t['h'], t['mips'], fn, e['name']))
        lines.append('%s: %.0f%% texels changed, lightness corr %.4f' % (stem, changed * 100, corr))
        pairs.append(np.concatenate([np.asarray(Image.fromarray(x).resize((256, 256))) for x in (px, res)], 1))
    if sheet and pairs:
        Image.fromarray(np.concatenate(pairs[:3], 0)).save(Path(sheet) / ('%03d_%s.png' % (n, e['name'])))
    return n, e['name'], rows, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--species', default='', help='comma list of game numbers (default: all in the color table)')
    ap.add_argument('--out', default=os.path.join(os.environ.get('APPDATA', '.'), 'SnapRecomp', 'mods', 'shiny-pokemon'))
    ap.add_argument('--src', default=str(ROOT / 'extracted' / 'drp-v262144' / 'nx' / 'Pack' / 'Characters' / 'master'))
    ap.add_argument('--sheet', default='', help='directory for normal|shiny contact images')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    sp = load_species()
    want = [int(s) for s in a.species.split(',') if s] or sorted(sp)
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    if a.sheet:
        Path(a.sheet).mkdir(parents=True, exist_ok=True)
    tab = out / 'shiny_table.txt'
    rows = {}
    if tab.exists():
        for ln in tab.read_text().splitlines():
            if ln and not ln.startswith('#'):
                rows[ln.split()[0]] = ln
    jobs = [(n, sp[n], a.src, str(out), a.sheet) for n in want if n in sp and sp[n].get('regions')]
    skipped = [n for n in want if n in sp and not sp[n].get('regions')]
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for n, name, rs, lines in ex.map(build_species, jobs):
            for r in rs:
                rows[r.split()[0]] = r
            print('%03d %s: %d textures' % (n, name, len(rs)), '; '.join(lines)[:160], flush=True)
    tab.write_text('# key w h mips file species (built by scripts/hd/make_shiny_pack.py)\n' + '\n'.join(rows.values()) + '\n')
    print('table: %d textures; skipped species: %s' % (len(rows), skipped))


if __name__ == '__main__':
    main()
