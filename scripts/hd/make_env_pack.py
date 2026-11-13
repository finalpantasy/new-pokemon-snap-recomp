# SPDX-License-Identifier: MIT
r"""Builds build\mods_hd_<area>\hd-<area> from the environment sets (scripts/hd/env_sets.json): albedo by class
(env_batch.choose_dif), _nor/_par from env_author (x2_env, else x2_detail). Same nutexb format as the original (BC1/BC3/BC5),
full mip chain, at most --max-edge (4096). A file is written to every LevelComposite archive that holds an identical original.
  python -E -P scripts\hd\make_env_pack.py <area> [--root build\mods_hd_<area>] [--dry-run]"""
import argparse, hashlib, json, sys
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import env_batch as eb  # noqa: E402
import nutexb_encode as ne  # noqa: E402
import build_cache as bc  # noqa: E402

LC = bc.PACK / 'LevelComposite' / 'master'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('area')
    ap.add_argument('--root', default='')
    ap.add_argument('--max-edge', type=int, default=4096)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    root = Path(a.root) if a.root else bc.ROOT / 'build' / ('mods_hd_' + a.area)
    pack = root / ('hd-' + a.area)
    s = eb.SETS[a.area]
    n = tot = 0
    skipped = []
    for it in s['items']:
        d = eb.tdir(it)
        stem = it['dif'][:-4]
        files = [(it['dif'], eb.choose_dif(it))]
        for role in ('nor', 'par'):
            nm = stem + '_' + role
            if (d / (nm + '.orig.png')).exists():
                f = next((d / (nm + '.' + v + '.png') for v in ('x2_env', 'x2_detail') if (d / (nm + '.' + v + '.png')).exists()), None)
                files.append((nm, f))
        for name, png in files:
            src = LC / s['archive'] / (name + '.nutexb')
            if png is None or not src.exists():
                skipped.append(name)
                continue
            im = Image.open(png).convert('RGBA')
            tmp = None
            if max(im.size) > a.max_edge:
                k = a.max_edge / max(im.size)
                im = im.resize((max(4, round(im.width * k)), max(4, round(im.height * k))), Image.LANCZOS)
                tmp = bc.TMP / ('_envpack_%s.png' % name)
                im.save(tmp)
                png = tmp
            h = hashlib.sha256(src.read_bytes()).digest()
            archives = [p.parent.name for p in LC.glob('LC_*.drp/' + name + '.nutexb') if hashlib.sha256(p.read_bytes()).digest() == h]
            if a.dry_run:
                print('would write', name, im.size, archives)
                continue
            first = None
            for arch in archives:
                dst = pack / 'nx' / 'Pack' / 'LevelComposite' / 'master' / arch
                dst.mkdir(parents=True, exist_ok=True)
                if first is None:
                    r = ne.encode(png, src, dst / src.name)
                    first = dst / src.name
                else:
                    (dst / src.name).write_bytes(first.read_bytes())
            n += 1
            tot += first.stat().st_size * len(archives)
            print('%s %s -> %s %s' % (name, png.name, im.size, archives), flush=True)
            if tmp:
                tmp.unlink(missing_ok=True)
    if not a.dry_run:
        pack.mkdir(parents=True, exist_ok=True)
        (pack / 'pack.json').write_text(json.dumps({'name': 'HD %s environment' % a.area, 'version': '1.0.0', 'author': 'local',
                                                    'description': 'Environment textures (albedo, normal, params) for the %s area: faithful HD refinement.' % a.area}, indent=1))
    print('done: %d textures, %.0f MB, skipped %s' % (n, tot / 1e6, skipped))


if __name__ == '__main__':
    main()
