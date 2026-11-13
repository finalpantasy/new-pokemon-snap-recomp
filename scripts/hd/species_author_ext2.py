# SPDX-License-Identifier: MIT
r"""Wrapper around species_author.py / mat_preview.py for the first-course species (Grookey, Scorbunny, Vivillon, Dodrio,
Wurmple, Pinsir, Wingull, Crabrawler, Bounsweet). Adds, without editing either script:

  1. Everything species_author_ext.py adds (texture sets shared by several materials, "mat": "Body|Wing"; its treatments).
  2. The wing_scales treatment (species_ext_wing.py).

  python -I scripts\hd\species_author_ext2.py author vivillon [--debug DIR]
  python -I scripts\hd\species_author_ext2.py pack vivillon --root DIR --pack NAME
  python -I scripts\hd\species_author_ext2.py preview vivillon [--variant x4_mat] [--only-closeup]
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import species_author as sa  # noqa: E402
import mat_preview as mp  # noqa: E402
import species_author_ext  # noqa: E402,F401  (multi-material sets, preview config, curls)
import species_ext_wing  # noqa: E402,F401  (registers wing_scales)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'preview':
        sys.argv = [sys.argv[0]] + sys.argv[2:]
        sa.below_normal()
        mp.main()
    else:
        sa.main()


if __name__ == '__main__':
    main()
