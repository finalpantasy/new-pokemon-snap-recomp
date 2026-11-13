# SPDX-License-Identifier: MIT
r"""Thin wrapper around species_author.py / mat_preview.py that adds two things without editing them:

  1. Texture sets shared by several materials: a spec set may name "mat": "Body|BodyB" (Bouffalant body + flocky afro share
     Body_dif; Torterra BodyB + BodyC share BodyB_*). Islands come from every listed material, so one texture is authored once
     over all its UV islands. Optional "mat_in_game": {"BodyB": {"dif": true, "par": false, "nor": false}} sets the per-material
     binding for the preview (BodyB of Bouffalant is a flocky material without a _par slot).
  2. Extra treatments registered into species_author.TREAT (scripts/hd/species_ext_*.py).

  python -I scripts\hd\species_author_ext.py author bouffalant [--debug DIR]
  python -I scripts\hd\species_author_ext.py pack bouffalant --root DIR --pack NAME
  python -I scripts\hd\species_author_ext.py preview bouffalant [--variant x4_mat] [--only-closeup]
"""
import copy, json, site, sys
from pathlib import Path

if site.getusersitepackages() not in sys.path:               # run with python -I: user site (cv2, scipy, Pillow) is not on the path
    sys.path.append(site.getusersitepackages())

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import material_author as ma  # noqa: E402
import species_author as sa  # noqa: E402
import mat_preview as mp  # noqa: E402
import species_ext_curls  # noqa: E402,F401  (registers its treatments into sa.TREAT)

_geometry = sa.geometry


def geometry(asset, mat, R, dirs):
    if '|' not in mat:
        return _geometry(asset, mat, R, dirs)
    mats = mat.split('|')
    lp0 = ma.load_prims

    def lp(a):
        out = []
        for p in lp0(a):
            if any(p['mat'].endswith('_' + m) for m in mats):
                q = dict(p)
                q['mat'] = p['mat'] + '_' + mat                 # so species_author's endswith('_' + mat) filter keeps it
                out.append(q)
        return out
    ma.load_prims = lp
    try:
        return _geometry(asset, mat, R, dirs)
    finally:
        ma.load_prims = lp0


sa.geometry = geometry
_load_cfg = mp.load_cfg


def load_cfg(name):
    cfg = copy.deepcopy(_load_cfg(name))
    sets = []
    for s in cfg['sets']:
        if '|' not in s.get('mat', ''):
            sets.append(s)
            continue
        for m in s['mat'].split('|'):
            c = dict(s)
            c['mat'] = m
            c['in_game'] = s.get('mat_in_game', {}).get(m, s.get('in_game', {}))
            sets.append(c)
    cfg['sets'] = sets
    return cfg


mp.load_cfg = load_cfg


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'preview':
        sys.argv = [sys.argv[0]] + sys.argv[2:]
        sa.below_normal()
        mp.main()
    else:
        sa.main()


if __name__ == '__main__':
    main()
