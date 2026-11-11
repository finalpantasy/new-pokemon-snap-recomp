"""Classify vtable candidates into probable classes -> analysis/maps/classes.tsv, class_functions.tsv"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scan_vtable_writers import *


def cstr(img, a):
    if a > 0 and img[a - 1] != 0:
        return None
    e = a
    while e < len(img) and img[e] != 0 and e - a < 96:
        e += 1
    b = bytes(img[a:e])
    if len(b) < 4 or img[e] != 0:
        return None
    if not all(32 <= c < 127 for c in b):
        return None
    return b.decode()


def analyze():
    img = load_image(); funcs = load_funcs(); vts = load_vtables(); rel = load_relocs()
    fsize = {s: e - s for s, e, k in funcs}
    vtmap = {v['start']: (v['start'], 0) for v in vts}
    byv = {v['start']: v for v in vts}
    res = scan(img, funcs, vtmap, rel)

    def slotfns(v):
        return [int(x, 16) for x in v['slots'] if x and not x.startswith('@')]
    in_slot = collections.defaultdict(set)
    for v in vts:
        for i, x in enumerate(v['slots']):
            if x and not x.startswith('@'):
                in_slot[int(x, 16)].add((v['start'], i))
    dele = {0x400}
    for _ in range(3):
        for s, e, k in funcs:
            if e - s <= 0x30:
                c = res[s]['calls']
                if len(c) == 1 and c[0][2] == 'b' and c[0][1] in dele:
                    dele.add(s)

    def is_secondary(v):
        try:
            return int(v['prefix'][0], 16) >= 0xf000000000000000
        except ValueError:
            return False
    writers = {}
    for s, r in res.items():
        if not r['writes']:
            continue
        stores = [(w[0][0], w[1], w[2], w[3], w[4], w[5]) for w in r['writes']]
        prim = [x for x in stores if x[5] == 0]
        writers[s] = dict(stores=stores, prim=prim, calls=r['calls'])
    role = {}
    for s, w in writers.items():
        own_slot = any(vt == vs for (vs, i) in in_slot.get(s, ()) for x in w['stores'] for vt in [x[0]])
        anyslot = s in in_slot
        calls_del = any(c[1] in dele for c in w['calls'])
        sd = 0
        if own_slot: sd += 3
        elif anyslot: sd += 2
        if calls_del: sd += 1
        first_bl = min([c[0] for c in w['calls']] or [10 ** 9])
        if w['prim'] and w['prim'][0][4] < first_bl and w['calls']: sd += 1
        role[s] = ['dtor' if sd >= 3 else 'ctor', sd]
    for s, w in sorted(writers.items()):
        if role[s][0] == 'dtor' and s in in_slot:
            for c in w['calls']:
                t = c[1]
                if t in role and role[t][0] == 'ctor' and t not in in_slot:
                    tw = writers[t]
                    if tw['prim'] and ((not tw['calls']) or tw['prim'][0][4] < tw['calls'][0][0]):
                        role[t] = ['dtor', 3]
    primary_of_writer = {s: w['prim'][-1][0] for s, w in writers.items() if w['prim']}
    secondary = collections.defaultdict(set)
    members = collections.defaultdict(set)
    for s, w in writers.items():
        P = primary_of_writer.get(s)
        if P is None:
            continue
        for x in w['stores']:
            vt, absoff = x[0], x[5]
            if absoff is None or absoff == 0:
                continue
            if is_secondary(byv[vt]):
                secondary[P].add(vt)
            else:
                members[P].add(vt)
    prim_set = set(primary_of_writer.values())
    sec_claim = {}
    for P in sorted(secondary):
        for sv in sorted(secondary[P]):
            if sv in prim_set:
                continue
            sec_claim.setdefault(sv, P)
    bases = collections.defaultdict(collections.Counter)
    for s, w in writers.items():
        P = primary_of_writer.get(s)
        if P is None:
            continue
        seen = []
        for x in w['prim']:
            if x[0] != P and x[0] not in seen:
                seen.append(x[0])
        for b in seen:
            bases[P][(b, 'inline_overwrite')] += 1
        pidx = w['prim'][-1][4]
        for c in w['calls']:
            i, t = c[0], c[1]
            if c[3] == 0 and t in primary_of_writer and primary_of_writer[t] != P:
                if role[s][0] == role[t][0]:
                    if role[s][0] == 'ctor' and i < pidx:
                        bases[P][(primary_of_writer[t], 'ctor_chain')] += 1
                    elif role[s][0] == 'dtor' and i > pidx:
                        bases[P][(primary_of_writer[t], 'dtor_chain')] += 1

    def slot_share(a, b):
        return sum(1 for x, y in zip(byv[a]['slots'], byv[b]['slots']) if x == y)
    classes = {}
    for v in vts:
        sv = v['start']
        if sv in sec_claim:
            continue
        classes[sv] = dict(secs=sorted(x for x in secondary.get(sv, ()) if sec_claim.get(x) == sv))
    cid = lambda a: 'C%06x' % a
    ctors = collections.defaultdict(list); dtors = collections.defaultdict(list)
    for s, P in sorted(primary_of_writer.items()):
        if P in classes:
            (ctors if role[s][0] == 'ctor' else dtors)[P].append(s)
    names = collections.defaultdict(list)
    for P in classes:
        cand = set(ctors[P]) | set(dtors[P])
        for f in slotfns(byv[P])[:6]:
            if fsize.get(f, 999) <= 0x40:
                cand.add(f)
        for f in sorted(cand):
            for (i, a) in res.get(f, {'strrefs': []})['strrefs']:
                st = cstr(img, a)
                if st and st not in names[P]:
                    names[P].append(st)
    out = []
    for P in sorted(classes):
        bc = bases.get(P)
        best = None
        if bc:
            cands = collections.Counter()
            for (b, ev), n in bc.items():
                cands[b] += n
            b = sorted(cands.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
            evs = sorted(set(ev for (bb, ev) in bc if bb == b))
            b = sec_claim.get(b, b)
            best = (b, '+'.join(evs), slot_share(P, b))
        out.append((P, byv[P], classes[P], best))
    return dict(img=img, funcs=funcs, vts=vts, byv=byv, res=res, writers=writers, role=role, in_slot=in_slot,
                primary_of_writer=primary_of_writer, sec_claim=sec_claim, ctors=ctors, dtors=dtors, names=names,
                members=members, out=out, fsize=fsize, cid=cid)


def main():
    A = analyze()
    out = A['out']; cid = A['cid']; ctors = A['ctors']; dtors = A['dtors']; names = A['names']
    h = lambda xs: ';'.join('0x%x' % x for x in xs)
    hdr = '# build_id: DDB900FF04A912ED42430508389853CFAF1ACCCE\n'
    with open(os.path.join(MAPS, 'classes.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# file: classes.tsv\n# probable classes from vtable-writer analysis (scripts/classify_vtables.py). class_id = C + primary vtable start (module offset); vtable = first function slot (the stored object pointer value).\n')
        f.write('# ctor/dtor = writers of the primary vtable at [this+0]; dtor = sits in own vtable slot, or stores vtable before first call and calls delete. base_evidence: inline_overwrite (vtable of base stored earlier in same function), ctor_chain (base ctor called before own store), dtor_chain (base dtor called after own store); base_shared_slots = equal slots at same index\n')
        f.write(hdr)
        f.write('class_id\tvtable\tslots\tsecondary_vtables\tctors\tdtors\tbase_class\tbase_evidence\tbase_shared_slots\tname_strings\tmember_vtables\tn_writers\n')
        for P, v, c, best in out:
            f.write('\t'.join([cid(P), '0x%x' % P, str(len(v['slots'])), h(c['secs']), h(ctors[P]), h(dtors[P]),
                               cid(best[0]) if best else '', best[1] if best else '', str(best[2]) if best else '',
                               ';'.join(names[P][:6]), h(sorted(A['members'].get(P, ()))), str(len(ctors[P]) + len(dtors[P]))]) + '\n')
    with open(os.path.join(MAPS, 'class_functions.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# file: class_functions.tsv\n# every function that stores a known vtable start; role heuristic score (dtor if >=3); stores = vtable@basereg+off\n')
        f.write(hdr)
        f.write('function\tsize\trole\tscore\tprimary_class\tin_vtable_slot\tstores\n')
        for s in sorted(A['writers']):
            w = A['writers'][s]; P = A['primary_of_writer'].get(s)
            pc = cid(A['sec_claim'].get(P, P)) if P is not None else ''
            f.write('\t'.join(['0x%x' % s, '0x%x' % A['fsize'][s], A['role'][s][0], str(A['role'][s][1]), pc,
                               ';'.join('0x%x#%d' % x for x in sorted(A['in_slot'].get(s, ()))),
                               ';'.join('0x%x@x%d+%d%s' % (x[0], x[1], x[2], '' if x[5] is None else '/this+%d' % x[5]) for x in w['stores'])]) + '\n')
    print('classes', len(out), 'writers', len(A['writers']), 'with ctors', sum(1 for o in out if ctors[o[0]]),
          'dtors', sum(1 for o in out if dtors[o[0]]), 'base', sum(1 for o in out if o[3]),
          'secondary claimed', len(A['sec_claim']), 'named', sum(1 for o in out if names[o[0]]))


if __name__ == '__main__':
    main()
