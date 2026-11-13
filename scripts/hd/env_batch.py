# SPDX-License-Identifier: MIT
r"""Batch helpers for the environment HD packs (local, game-derived output stays in build/hd).
  python -E -P scripts\hd\env_batch.py esr <area>      Real-ESRGAN x4 albedo for the sd and esr classes (GPU; caller holds a run slot)
  python -E -P scripts\hd\env_batch.py list <area> <class> <variant>   print the .orig.png paths that still lack <variant> (for retexture_sd)
  python -E -P scripts\hd\env_batch.py detail <area>   x2_detail (own detail, CPU) for the detail class
  python -E -P scripts\hd\env_batch.py manifest         rewrite the viewer manifest
Areas and textures: scripts/hd/env_sets.json."""
import json, subprocess, sys
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_cache as bc  # noqa: E402

SETS = json.loads((HERE / 'env_sets.json').read_text())


def tdir(it):
    return bc.CACHE / it['asset'] / 'textures'


def ensure_maps(it):
    """The cache keeps one copy of each texture per model; the nor/par of a set may sit in another model's folder. Copy them next to the dif."""
    import shutil
    d = tdir(it)
    stem = it['dif'][:-4]
    for role in ('dif', 'nor', 'par'):
        f = d / (stem + '_' + role + '.orig.png')
        if f.exists():
            continue
        for o in bc.CACHE.glob('sm_env_*/textures/' + stem + '_' + role + '.orig.png'):
            shutil.copyfile(o, f)
            break


def choose_dif(it):
    """Albedo variant by class: sd -> x4_blend (else x4_realesrgan), esr -> x4_realesrgan, detail -> x2_detail. Returns a path or None."""
    d = tdir(it)
    order = {'sd': ['x4_blend', 'x4_realesrgan', 'x2_detail'], 'esr': ['x4_realesrgan', 'x2_detail'], 'detail': ['x2_detail']}[it.get('override') or it['class']]
    for v in order:
        f = d / (it['dif'] + '.' + v + '.png')
        if f.exists():
            return f
    return None


def esr(area):
    scale = 4
    jobs = []
    for it in SETS[area]['items']:
        if it['class'] not in ('sd', 'esr') or it['size'] > 1024:
            continue
        orig = tdir(it) / (it['dif'] + '.orig.png')
        dst = tdir(it) / (it['dif'] + '.x4_realesrgan.png')
        if dst.exists():
            continue
        jobs.append((it, orig, dst))
    print(len(jobs), 'to upscale', flush=True)
    for i in range(0, len(jobs), 4):
        part = jobs[i:i + 4]
        if not bc.acquire_lock('env esr ' + area):
            raise SystemExit('no heavy.lock')
        try:
            tmp = []
            for k, (it, orig, dst) in enumerate(part):
                px = np.asarray(Image.open(orig).convert('RGBA'))
                p = bc.TMP / ('env_src_%d_%d.png' % (i, k))
                bc.TMP.mkdir(parents=True, exist_ok=True)
                Image.fromarray(px[..., :3], 'RGB').save(p)
                tmp.append((p, dst))
            res = list(bc.run_realesrgan('realesrgan-x4plus', tmp, scale))
        finally:
            bc.release_lock()
        for (it, orig, dst), (d2, o), (p, _) in zip(part, res, tmp):
            rgb = np.asarray(Image.open(o).convert('RGB'))
            px = np.asarray(Image.open(orig).convert('RGBA'))
            al = bc.lanczos_up(px[..., 3:4], scale, 1) if (px[..., 3] != 255).any() else np.full(rgb.shape[:2] + (1,), 255, np.uint8)
            Image.fromarray(np.concatenate([rgb, al], -1), 'RGBA').save(dst)
            Path(p).unlink(missing_ok=True)
            print('esr', it['dif'], rgb.shape[1], flush=True)


def main():
    cmd = sys.argv[1]
    if cmd == 'esr':
        esr(sys.argv[2])
    elif cmd == 'list':
        area, cls, var = sys.argv[2:5]
        for it in SETS[area]['items']:
            if it['class'] == cls and not (tdir(it) / (it['dif'] + '.' + var + '.png')).exists():
                print(tdir(it) / (it['dif'] + '.orig.png'))
    elif cmd == 'detail':
        by = {}
        for it in SETS[sys.argv[2]]['items']:
            by.setdefault(it['asset'], []).append(it['dif'])
        for a, names in by.items():
            subprocess.run([sys.executable, '-E', '-P', str(HERE / 'detail_boost.py'), '--assets', a, '--textures', ','.join(names)], check=True)
    elif cmd == 'manifest':
        bc.write_manifest()


if __name__ == '__main__':
    main()
