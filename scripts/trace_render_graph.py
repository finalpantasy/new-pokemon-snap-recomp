"""Render-pass graph trace -> analysis/maps/render_passes_final.tsv (deterministic).
Pass nodes are created by three code paths that all call the node factory 0x97a680 (vtable 0xcc4058):
  - 0x5cd800 (CreatePass(name), 35 call sites inside 0x5bd940)
  - 0x5bd940 itself ("DynamicShadow", direct call to 0x97a680)
  - 0x4d0f50 ("StaticShadow", called from 0x5bd940)
Names are the rodata string referenced (adrp+add) between the previous graph-helper call and the call site."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scan_vtable_writers import *

FACTORY = 0x97a680; CREATE = 0x5cd800; SHADOW_S = 0x4d0f50; BUILDER = 0x5bd940
HELPERS = {0x5cd800, 0x5cd2a0, 0x987d20, 0x4d0f50, 0x97a680, 0x9863c0, 0x5b8340, 0x5b9a70, 0x5b9390, 0x5ba450,
           0x5cdba0, 0x5ce300, 0x5cea90, 0x4ca580, 0x4caa90, 0x5b8240}
BAD = ('output', 'Input', 'rendertarget', 'RenderTarget', 'depthstencil', 'DepthStencil', 'plug', 'constantBuffer',
       'Constant', 'ConstantBuffer', 'renertarget', 'Layout')


def cstr(img, a):
    e = a
    while e < len(img) and img[e] != 0 and e - a < 96:
        e += 1
    b = bytes(img[a:e])
    if len(b) < 2 or not all(32 <= c < 127 for c in b):
        return None
    return b.decode()


def main():
    img = load_image(); funcs = [f for f in load_funcs() if f[0] in (BUILDER, SHADOW_S)]
    res = scan(img, funcs, {}, load_relocs())
    r = res[BUILDER]
    calls = sorted(r['calls'])
    rows = []
    prev = -1
    for (i, t, k, _) in calls:
        if t in HELPERS:
            if t in (CREATE, FACTORY):
                names = []
                for (j, a) in r['strrefs']:
                    if prev < j < i:
                        st = cstr(img, a)
                        if st and not any(b in st for b in BAD):
                            names.append(st)
                name = names[-1] if names else '(unresolved)'
                rows.append((name, 'CreatePass_0x5cd800' if t == CREATE else 'inline_0x5bd940', BUILDER + 4 * i))
            prev = i
    # StaticShadow from 0x4d0f50
    r2 = res[SHADOW_S]
    nm = None
    for (j, a) in r2['strrefs']:
        st = cstr(img, a)
        if st == 'StaticShadow':
            nm = st
    if nm:
        site = [BUILDER + 4 * i for (i, t, k, _) in calls if t == SHADOW_S][0]
        rows.append((nm, 'factory_0x4d0f50', site))
    # order: by call site address
    rows.sort(key=lambda x: x[2])
    cid = 'Ccc4058'
    with open(os.path.join(MAPS, 'render_passes_final.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# file: render_passes_final.tsv\n')
        f.write('# one row per graph pass node created in the pass-graph builder 0x5bd940 (scripts/trace_render_graph.py). All nodes share one class: the generic RenderPassBase node (0xf8-byte object) built by factory 0x97a680.\n')
        f.write('# execute_slot_fn = vtable slot 5 (+0x28) = per-frame Execute called by the execute loop 0x988fe0; identical for every pass (no per-pass override found); per-pass behavior lives in plug/task objects attached to the node (see render_graph.md)\n')
        f.write('# build_id: DDB900FF04A912ED42430508389853CFAF1ACCCE\n')
        f.write('pass_name\tclass_id\tctor\tvtable\texecute_slot_fn\tcreator\tcreate_site\tbase_class_id\tconfidence\n')
        for name, creator, site in rows:
            conf = 'medium' if name != '(unresolved)' else 'low'
            f.write('\t'.join([name, cid, '0x97a680', '0xcc4058', '0x982210', creator, '0x%x' % site, 'Ccc3fe8', conf]) + '\n')
    print('pass rows', len(rows), 'unresolved', sum(1 for x in rows if x[0] == '(unresolved)'))


if __name__ == '__main__':
    main()
