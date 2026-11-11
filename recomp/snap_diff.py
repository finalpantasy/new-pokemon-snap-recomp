"""Function-list diff: the TotK generator's own discovery (recomp/gen.py discover) against
analysis/maps/functions_final.tsv, plus its jump-table resolver against analysis/maps/jump_tables.tsv.
Writes analysis/recomp/function_diff.md.
Run: tools/venv-recomp/Scripts/python -I recomp/snap_diff.py"""
import bisect
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import time
from collections import Counter

from snap_common import BASE, MAPS, ROOT, functions, load_module, tsv
import gen
import jumptables


def main():
    from snap_common import lower_priority
    lower_priority()
    t0 = time.time()
    mod = load_module()
    assert mod.lo == BASE
    starts, stats = gen.discover(mod)
    theirs = {int(a) * 4 for a in starts}   # word index * 4 = module offset (text starts at 0)
    t_disc = time.time() - t0
    fns = functions()
    ours = {s: (e, k, t) for s, e, k, t in fns}
    ours_fn = {s for s, (e, k, t) in ours.items() if k in ('function', 'thunk')}
    ours_plt = {s for s, (e, k, t) in ours.items() if k == 'plt_stub'}
    ours_unres = {s for s, (e, k, t) in ours.items() if k == 'unresolved'}
    all_ours = set(ours)
    sorted_ours = sorted(all_ours)
    both = theirs & all_ours
    only_theirs = sorted(theirs - all_ours)
    only_ours = sorted(all_ours - theirs)
    kind_only_ours = Counter(ours[s][1] for s in only_ours)
    tl = sorted(theirs)
    t_end = {s: (tl[i + 1] if i + 1 < len(tl) else mod.hi - BASE) for i, s in enumerate(tl)}
    gaps0 = [(int(r['start'], 16), int(r['end'], 16), r['kind']) for r in tsv(MAPS / 'functions_final.tsv')
             if r['kind'].startswith('gap')]
    pad = {g[0]: g[1] for g in gaps0 if g[2] == 'gap_padding'}

    def explained_by_padding(a, b):
        # theirs-longer is benign when [a, b) is exactly one or more of our padding gaps
        while a < b and a in pad:
            a = pad[a]
        return a == b

    end_mismatch = [s for s in both if ours[s][1] != 'plt_stub' and t_end[s] != ours[s][0]
                    and not (t_end[s] > ours[s][0] and explained_by_padding(ours[s][0], t_end[s]))]
    benign_pad = sum(1 for s in both if ours[s][1] != 'plt_stub' and t_end[s] > ours[s][0]
                     and explained_by_padding(ours[s][0], t_end[s]))
    mism_dir = Counter('theirs_shorter' if t_end[s] < ours[s][0] else 'theirs_longer' for s in end_mismatch)
    inside, ex_inside = Counter(), []
    for s in only_theirs:
        i = bisect.bisect_right(sorted_ours, s) - 1
        if i >= 0 and s < ours[sorted_ours[i]][0]:
            o = sorted_ours[i]
            inside[f'inside_our_{ours[o][1]}'] += 1
            if len(ex_inside) < 12:
                ex_inside.append((s, o, ours[o][0], ours[o][1], ours[o][2]))
        else:
            inside['in_gap_of_ours'] += 1
    gap_kinds = Counter()
    gaps = [(int(r['start'], 16), int(r['end'], 16), r['kind'], r['notes']) for r in tsv(MAPS / 'functions_final.tsv')
            if r['kind'].startswith('gap')]
    gs = [g[0] for g in gaps]
    gap_ex = []
    for s in only_theirs:
        i = bisect.bisect_right(gs, s) - 1
        if i >= 0 and s < gaps[i][1]:
            gap_kinds[gaps[i][2] + ':' + gaps[i][3]] += 1
            if len(gap_ex) < 10:
                gap_ex.append((s, gaps[i]))

    # ---- jump tables
    jt = tsv(MAPS / 'jump_tables.tsv')
    tj, jt_bad = Counter(), []
    t1 = time.time()
    fn_sorted = sorted((s, e) for s, e, k, _ in fns if k in ('function', 'thunk'))
    fs = [s for s, _ in fn_sorted]
    for r in jt:
        cls = r['class']
        if cls not in ('jump_table', 'jump_table_unrecovered'):
            continue
        br = int(r['br_site'], 16)
        entry, end = fn_sorted[bisect.bisect_right(fs, br) - 1]
        got = jumptables.resolve(mod, BASE + br, BASE + entry, BASE + end)
        want = sorted({int(x, 16) for x in r['targets'].split(';') if x}) if r['targets'] else []
        if cls == 'jump_table':
            if got is None:
                tj['ours_recovered_theirs_missing'] += 1
                jt_bad.append((br, entry, f'theirs none, ours {len(want)} targets'))
            elif sorted(x - BASE for x in got) == want:
                tj['identical_targets'] += 1
            else:
                tj['target_set_differs'] += 1
                g = {x - BASE for x in got}
                rel = 'theirs_subset_of_ours' if g < set(want) else 'ours_subset_of_theirs' if set(want) < g else 'incomparable'
                tj['  differs: ' + rel] += 1
                jt_bad.append((br, entry, f'{rel}: theirs {len(g)} distinct targets vs ours {len(set(want))}'))
        elif got is None:
            tj['both_unrecovered'] += 1
        else:
            tj['theirs_recovered_ours_missing'] += 1
            jt_bad.append((br, entry, f'ours unrecovered, theirs {len(got)}'))
    t_jt = time.time() - t1

    out = ROOT / 'analysis' / 'recomp'
    out.mkdir(parents=True, exist_ok=True)
    nofde_only = sum(1 for s in only_ours if ours[s][1] in ('function', 'thunk') and 'nofde' in ours[s][2])
    L = ['# Function-list diff: TotK generator discovery vs functions_final.tsv\n',
         'Inputs: `analysis/import/v262144-main.elf` (via `recomp/elfimg.py`), `analysis/maps/functions_final.tsv`, '
         '`analysis/maps/jump_tables.tsv`. Generator: `recomp/gen.py discover()` (TotK @ 7a37fc3). Module offsets '
         f'throughout (guest base {BASE:#x} subtracted). Re-run: `tools/venv-recomp/Scripts/python -I recomp/snap_diff.py` '
         f'(discovery {t_disc:.1f}s, jump tables {t_jt:.1f}s).\n',
         '## Discovery\n',
         'Seeds: .eh_frame_hdr FDEs, exported FUNC dynsyms, RELATIVE relocation targets in .text; then every BL target, '
         'ADR/ADRP+ADD materialised code addresses, and out-of-function direct branch targets, to a fixpoint. No Ghidra input.\n',
         f'- Generator stats: {stats}',
         f'- Generator starts: **{len(theirs):,}**',
         f'- Ours (function + thunk + plt_stub + unresolved): **{len(all_ours):,}** '
         f'({len(ours_fn):,} function/thunk, {len(ours_plt)} plt_stub, {len(ours_unres)} unresolved)',
         f'- In both: **{len(both):,}**',
         f'- Generator-only: **{len(only_theirs):,}**; ours-only: **{len(only_ours):,}** {dict(kind_only_ours)}',
         f'- Ours-only function/thunk starts tagged `nofde` (no unwind data; Ghidra/reconcile found them): {nofde_only}; '
         f'with an FDE: {len([s for s in only_ours if ours[s][1] in ("function", "thunk")]) - nofde_only}',
         f'- Common starts whose end differs (generator end = next start): {len(end_mismatch):,} {dict(mism_dir)}; '
         f'a further {benign_pad:,} differ only by trailing padding (our end plus padding gaps = next start), not counted',
         f'- Generator-only starts relative to our list: {dict(inside)}',
         f'- Generator-only starts inside our classified gaps: {dict(gap_kinds)}\n',
         '### Generator-only starts inside one of our functions (first 12): start, our function, kind, tags\n']
    L += [f'- `{s:#x}` in `{o:#x}-{e:#x}` {k} {t}' for s, o, e, k, t in ex_inside]
    L.append('\n### Generator-only starts inside a classified gap (first 10)\n')
    L += [f'- `{s:#x}` in gap `{g[0]:#x}-{g[1]:#x}` {g[2]} {g[3]}' for s, g in gap_ex]
    L.append('\n### Ours-only (first 25): start, end, kind, tags\n')
    L += [f'- `{s:#x}-{ours[s][0]:#x}` {ours[s][1]} {ours[s][2]}' for s in only_ours[:25]]
    L.append('\n### Common start, different end (first 10): start, ours end, generator end\n')
    L += [f'- `{s:#x}` ours `{ours[s][0]:#x}` generator `{t_end[s]:#x}`' for s in sorted(end_mismatch)[:10]]
    L.append('\n## Jump tables (`jumptables.resolve` vs jump_tables.tsv)\n')
    L.append(f'- {dict(tj)}')
    L.append('\nDisagreements (first 15): br site, function, detail\n')
    L += [f'- `{br:#x}` in `{f:#x}`: {d}' for br, f, d in jt_bad]
    (out / 'function_diff.md').write_text('\n'.join(L) + '\n', encoding='utf-8')
    print('\n'.join(L[8:18]))
    print(dict(tj))


if __name__ == '__main__':
    main()
