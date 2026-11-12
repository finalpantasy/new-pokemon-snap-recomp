# SPDX-License-Identifier: MIT
r"""Builds the HD texture pack from build/hd/cache upscales: every cached texture that has an upscale is re-encoded to a nutexb in
the same format as the original (BC7/BC1/BC5/..., mips, block-linear) and written to <root>/<pack>/<archive>.drp/<name>.nutexb.

  python -E -P scripts\hd\make_hd_pack.py --root build\mods_hd --pack hd-starter [--assets p001_00,p032_00] [--max-edge 4096]

Variant choice: albedo = x4_enhanced (colour-locked, edge-clamped, faithful) else x4_realesrgan; other roles = x4_realesrgan
(a Lanczos result for normal and mask maps). A texture is skipped (and listed) when no upscale exists. An upscale larger than
--max-edge is shrunk with Lanczos first. Entries get a Lanczos x2 (no AI) when --fallback-lanczos is set and no AI variant exists.
Output is local only (game-derived); nothing here is committed.
"""
import argparse, json, os, sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_cache as bc  # noqa: E402
import nutexb_encode as ne  # noqa: E402

ARCHIVE_ROOT = 'nx/Pack'
SD_OK = {'SM_ENV_com_barkA_00_dif', 'SM_ENV_001_flowerA_00_dif'}   # x4_sd albedo checked by the owner


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.environ.get('APPDATA', '.'), 'SnapRecomp', 'mods'))
    ap.add_argument('--pack', default='hd-starter')
    ap.add_argument('--assets', default='', help='comma list of cache asset ids (default: all with upscales)')
    ap.add_argument('--albedo', default='x4_enhanced,x4_realesrgan')
    ap.add_argument('--other', default='x4_realesrgan')
    ap.add_argument('--max-edge', type=int, default=4096)
    ap.add_argument('--roles', default='', help='only these roles (albedo,normal,mask,emissive,other)')
    ap.add_argument('--no-detail', action='store_true', help='ignore x2_detail files (scripts/hd/detail_boost.py)')
    ap.add_argument('--no-sd', action='store_true', help='ignore x4_sd albedo files')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    man = json.loads((bc.CACHE / 'manifest.json').read_text())
    want = [s for s in a.assets.split(',') if s]
    roles = [r for r in a.roles.split(',') if r]
    pack = Path(a.root) / a.pack
    n_ok = n_skip = 0
    for asset in man['assets']:
        if want and asset['id'] not in want:
            continue
        info = json.loads((bc.CACHE / asset['id'] / 'info.json').read_text())
        src_pack = info['source_pack']
        dst = pack / ARCHIVE_ROOT / src_pack
        for t in asset['textures']:
            if roles and t['role'] not in roles:
                continue
            prefs = (a.albedo if t['role'] == 'albedo' else a.other).split(',')
            var = next((v for v in prefs if v in t['variants']), None)
            dpng = bc.CACHE / asset['id'] / 'textures' / (t['name'] + '.x2_detail.png')
            if dpng.exists() and not a.no_detail:
                var = 'x2_detail'
            sdpng = bc.CACHE / asset['id'] / 'textures' / (t['name'] + '.x4_sd.png')
            bpng = bc.CACHE / asset['id'] / 'textures' / (t['name'] + '.x4_blend.png')
            if bpng.exists() and t['role'] == 'albedo' and t['name'] != 'SM_ENV_001_groundC_00_dif' and not a.no_sd:   # groundC blend has artifacts
                var, dpng = 'x4_sd', bpng           # 50/50 Real-ESRGAN + SD blend (texture worker)
            elif sdpng.exists() and t['role'] == 'albedo' and t['name'] in SD_OK and not a.no_sd:
                var, dpng = 'x4_sd', sdpng          # AI re-textured albedo (owner-approved for ground, bark, flower; rocks drifted)
            orig = bc.PACK / src_pack / (t['name'] + '.nutexb')
            if var is None or not orig.exists():
                n_skip += 1
                print('skip %s/%s (%s)' % (asset['id'], t['name'], 'no upscale' if var is None else 'no source nutexb'))
                continue
            png = dpng if var in ('x2_detail', 'x4_sd') else bc.CACHE / t['variants'][var]
            im = Image.open(png).convert('RGBA')
            tmp = None
            if max(im.size) > a.max_edge:
                s = a.max_edge / max(im.size)
                im = im.resize((max(4, round(im.width * s)), max(4, round(im.height * s))), Image.LANCZOS)
                tmp = bc.TMP / ('_pack_%s.png' % t['name'])
                im.save(tmp)
                png = tmp
            if a.dry_run:
                print('would write %s/%s (%s %dx%d)' % (asset['id'], t['name'], var, im.width, im.height))
                continue
            dst.mkdir(parents=True, exist_ok=True)
            r = ne.encode(png, orig, dst / orig.name)
            n_ok += 1
            print('%s/%s <- %s %s' % (asset['id'], t['name'], var, r), flush=True)
            if tmp:
                tmp.unlink(missing_ok=True)
    if not a.dry_run:
        (pack / 'pack.json').write_text(json.dumps({
            'name': 'HD starter textures', 'version': '1.0.0', 'author': 'local',
            'description': 'Upscaled Pokemon, Professor/Rita and Florio textures (modest detail, original colours).'}, indent=1))
    print('done: %d textures, %d skipped' % (n_ok, n_skip))


if __name__ == '__main__':
    main()
