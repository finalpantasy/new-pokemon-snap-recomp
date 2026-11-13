# SPDX-License-Identifier: MIT
r"""One mod pack per species from the material-authored `x4_mat` variant (4096 master).
Only textures the material binds in game (spec `in_game`: a body without a normal sampler gets _dif/_par only).
Each texture keeps its original nutexb format (BC1/BC3/BC5/BC7, sRGB or linear flag, mip count; nutexb_encode.py).
Pack layout: <root>/<species>/nx/Pack/<source_pack>/<entry>.nutexb + pack.json (runtime/mods/mods_core.h).
usage: python -I scripts\hd\make_species_pack.py [species ... | --all] [--root build\mods_mat] [--max-edge 4096]
       [--variant x4_mat] [--combined build\mods_mat_all\hd-pokemon-mat] (--combined merges the chosen species into one pack)"""
import argparse, json, sys, time
from pathlib import Path
import site; sys.path.append(site.getusersitepackages())  # user-site numpy under python -I
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_cache as bc          # noqa: E402
import nutexb_encode as ne        # noqa: E402
import material_author as ma      # noqa: E402
import species_author as sa       # noqa: E402


def pack_species(spec, dst_root, variant, max_edge, name=None, merge=False):
    asset, num = spec['asset'], spec['num']
    info_f = ma.CACHE / asset / 'info.json'
    info = json.loads(info_f.read_text()) if info_f.exists() else None
    src_pack = (info or {}).get('source_pack') or 'Characters/master/CH_%s.drp' % num
    dst = Path(dst_root) / 'nx' / 'Pack' / src_pack
    dst.mkdir(parents=True, exist_ok=True)
    pj = Path(dst_root) / 'pack.json'
    if not merge or not pj.exists():
        pj.write_text(json.dumps({'name': name or spec['name'], 'version': '1.0.0', 'author': 'local',
                                  'description': 'material-authored textures (%s, max edge %d)' % (variant, max_edge)}, indent=1))
    td = ma.tex_dir(asset)
    done = []
    for st in spec['sets']:
        ig = st.get('in_game', {})
        for key in ('dif', 'nor', 'par'):
            n = st.get(key)
            if not n or not ig.get(key, True):
                continue
            pre = 'CH_%s_00_0_A00_%s' % (num, n)
            orig = bc.PACK / src_pack / (pre + '.nutexb')
            png = td / (pre + '.%s.png' % variant)
            if not orig.exists() or not png.exists():
                print('  skip (missing %s):' % ('original' if not orig.exists() else 'variant png'), pre)
                continue
            im = Image.open(png).convert('RGBA')
            if max(im.size) > max_edge:
                im = im.resize((max_edge, max_edge), Image.LANCZOS)
            tmp = Path(dst_root) / '_tmp.png'
            im.save(tmp)
            r = ne.encode(tmp, orig, dst / orig.name)
            tmp.unlink()
            done.append(pre)
            print('  %s %s' % (orig.name, r), flush=True)
    return done


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('species', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--root', default='build/mods_mat')
    ap.add_argument('--combined', default=None)
    ap.add_argument('--max-edge', type=int, default=4096)
    ap.add_argument('--variant', default='x4_mat')
    a = ap.parse_args()
    names = a.species
    if a.all:
        names = [f.stem for f in sorted(sa.SPEC_DIR.glob('*.json'))]
    for n in names:
        spec = sa.load_spec(n)
        if not list(ma.tex_dir(spec['asset']).glob('*.%s.png' % a.variant)):
            print('skip %s: no %s in cache' % (n, a.variant))
            continue
        t0 = time.time()
        print(spec['name'])
        if a.combined:
            pack_species(spec, a.combined, a.variant, a.max_edge, name='HD Pokemon (material)', merge=True)
        else:
            pack_species(spec, Path(a.root) / n.lower(), a.variant, a.max_edge)
        print('  %.0fs' % (time.time() - t0))


if __name__ == '__main__':
    main()
