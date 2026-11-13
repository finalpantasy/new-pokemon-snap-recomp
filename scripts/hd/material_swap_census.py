# SPDX-License-Identifier: MIT
"""Read-only census: which Pokemon materials can use the twin-shader swap of make_material_mod.py.

  python -I scripts/hd/material_swap_census.py [--tsv out.tsv]

For every MI_CH_*.matinstb: the pass shaders (three real ones plus one _zprepass name), whether the main pixel shader already
has a normal sampler, and for the others whether every pass shader has an exact twin (same samplers plus normalTexture, same
ParameterList) whose instruction count is the original's +42 (the size of the normal block on the CharacterShader family;
all three twins checked on Bidoof's set differ from the original by exactly one fetch, the z rebuild, two normalizes and
one extra uniform component, sssNormalMapWeight). Outputs counts and an optional per-material table.
"""
import collections, glob, os, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'scripts' / 'hd'))
import make_material_mod as M  # noqa: E402


def main():
    db = M.shader_db()
    by_key = collections.defaultdict(list)
    for sid, (smp, par, n) in db.items():
        by_key[(smp, par)].append((n, sid))
    rows = []
    for f in sorted(glob.glob(str(M.CHARS / 'CH_*.drp' / 'MI_*.matinstb'))):
        b = open(f, 'rb').read()
        ids = [m.group(1).decode() for m in re.finditer(rb'CharactersMaterials_CharacterShader_(\d+)\x00', b)]
        z = [m.group(1).decode() for m in re.finditer(rb'CharactersMaterials_CharacterShader_(\d+)_zprepass\x00', b)]
        label = os.path.basename(f)[:-9]
        ch = os.path.basename(os.path.dirname(f))[:-4]
        if len(ids) != 3 or len(z) != 1 or ids[2] != z[0]:
            rows.append((ch, label, 'odd-shader-list', ids, ''))
            continue
        if any(s not in db for s in ids):
            rows.append((ch, label, 'shader-missing', ids, ''))
            continue
        main_smp = db[ids[2]][0]
        if 'normalTexture' in main_smp:
            rows.append((ch, label, 'has-normal', ids, ''))
            continue
        ok42, anyt = True, True
        for s in ids:
            smp, par, n = db[s]
            tw = [k for (n2, k) in by_key.get((smp | {'normalTexture'}, par), [])]
            if not tw:
                anyt = False
            if not any(db[k][2] == n + 42 for k in tw):
                ok42 = False
        rows.append((ch, label, 'swap-ok(+42)' if ok42 else ('twin-other-delta' if anyt else 'no-exact-twin'), ids, ''))
    c = collections.Counter(r[2] for r in rows)
    print('materials', len(rows))
    for k, v in c.most_common():
        print('  %-18s %d' % (k, v))
    sha = M.program_sha()
    if sha:
        grp = collections.Counter()
        for r in rows:
            if r[2] in ('has-normal', 'odd-shader-list', 'shader-missing') or not all(s in sha for s in r[3]):
                continue
            grp[tuple(sha[s][:8] for s in r[3])] += 1
        print('no-normal materials by program-binary triple (identical translated programs share one twin set): %d triples' % len(grp))
        for k, v in grp.most_common(6):
            print('  ', k, v)
        bid = tuple(sha[s][:8] for s in ('1059205888', '1019458661', '3436703351'))
        print('same three program binaries as Bidoof Body: %d materials' % grp[bid])
    sw = [r for r in rows if r[2] == 'swap-ok(+42)']
    print('species with at least one swap-ok material:', len({r[0] for r in sw}))
    if '--tsv' in sys.argv:
        p = sys.argv[sys.argv.index('--tsv') + 1]
        with open(p, 'w') as o:
            for r in rows:
                o.write('\t'.join([r[0], r[1], r[2], ','.join(r[3])]) + '\n')
        print('wrote', p)


if __name__ == '__main__':
    main()
