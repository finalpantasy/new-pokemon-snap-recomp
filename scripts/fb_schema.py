"""Generate .fbs schemas, structure JSON and a survey/round-trip table for every FlatBuffers type.

  python -I scripts/fb_schema.py [ext ...]        (default: all known types)
Outputs under analysis/registries/: schemas/<ext>.fbs, structure/<ext>.json, survey.tsv
Reads extracted game data (not shipped); output contains structure plus short sample-free metadata only.
"""
import os, sys, json, struct, collections, re, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fb_walk as fw, fb_roundtrip as rt, fb_names as fn
from fb_stats import scalar_values

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'analysis', 'registries')
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'extracted', 'drp-v262144')

REGISTRY = ['mdfb', 'misd', 'rtd', 'path', 'navb', 'pcnb', 'genderb', 'pdcd', 'silb', 'ecbd', 'mdcd', 'achd',
            'mdrp', 'nvhb', 'ldtb', 'picd', 'pstb', 'pfrb', 'pflb', 'paramb']
CONTENT = ['courseb', 'levelb', 'ptsb', 'cbsb', 'cesb', 'characterb', 'facb']
OTHER = ['modelb', 'matinstb', 'lprb', 'cutb', 'shb', 'skb', 'navmshb', 'reflectb', 'nuanmb', 'DC4A8177']
MAXSIZE = {'lprb': 3_000_000, 'DC4A8177': 700_000}   # python-speed guard: very large files are skipped (counted in 'files')
LIMITS = {'modelb': 150, 'matinstb': 150, 'lprb': 12, 'cutb': 100, 'shb': 64, 'skb': 103, 'navmshb': 54,
          'reflectb': 150, 'nuanmb': 150, 'DC4A8177': 6}


def plausible_float(raw_list):
    nz = 0
    for v in raw_list:
        u = int.from_bytes(v, 'little')
        if u == 0:
            continue
        f = struct.unpack('<f', v)[0]
        if not (1e-6 < abs(f) < 1e7) or u < 0x00800000:
            return False
        nz += 1
    return nz > 0


def ident(s):
    return re.sub(r'[^0-9A-Za-z_]', '_', s)


def field_type(w, cls, slot, info, structs, ext):
    k = info['kind']
    if k == 'table':
        return tname(ext, cls.key + '.' + str(slot)), ''
    if k == 'string':
        return 'string', ''
    if k == 'vec':
        ek = info.get('elem')
        if ek == 'table':
            return '[%s]' % tname(ext, cls.key + '.' + str(slot) + '[]'), ''
        if ek == 'string':
            return '[string]', ''
        es = ek['scalar']
        note = ' (empty in every instance: element type unresolved)' if ek.get('unresolved') else ''
        if es == 1:
            return '[ubyte]', note
        if es == 2:
            return '[ushort]', note
        if es == 4:
            vals = []
            for fi, t in cls.inst[:3000]:
                b = w.bufs[fi]
                _, _, _, sl = fw.read_table(b, t)
                if slot in sl:
                    v = fw.fwd(b, t + sl[slot])
                    n = fw.U32(b, v)[0]
                    vals += [bytes(b[v + 4 + 4 * i: v + 8 + 4 * i]) for i in range(min(n, 50))]
            return ('[float]' if plausible_float(vals) else '[uint]'), note
        return '[%s]' % struct_name(es, None, structs), note
    sz = info['size']
    if sz == 1:
        return 'ubyte', ''
    if sz == 2:
        return 'ushort', ''
    if sz == 4:
        return ('float' if plausible_float(scalar_values(w, cls, slot)) else 'uint'), ''
    vals = scalar_values(w, cls, slot)
    lanes_float = sz % 4 == 0 and all(plausible_or_zero(v, sz) for v in vals)
    return struct_name(sz, lanes_float, structs), ''


def plausible_or_zero(v, sz):
    any_nz = False
    for i in range(0, sz, 4):
        u = int.from_bytes(v[i:i + 4], 'little')
        if u == 0:
            continue
        f = struct.unpack('<f', v[i:i + 4])[0]
        if not (1e-6 < abs(f) < 1e7) or u < 0x00800000:
            return False
        any_nz = True
    return any_nz


def struct_name(sz, as_float, structs):
    if as_float and sz in (8, 12, 16):
        n = {8: 'Vec2', 12: 'Vec3', 16: 'Vec4'}[sz]
    else:
        n = 'Raw%d' % sz
    structs[n] = sz
    return n


def tname(ext, key):
    names = fn.NAMES.get(ext, {})
    if key in names:
        return names[key][0]
    if key == 'R':
        return fn.ROOT_NAMES.get(ext, (ext.upper() + 'Root',))[0]
    return ident(ext + '_' + key.replace('[]', '_i').replace('.', '_'))


def render(ext, w, graphs, ident4):
    structs = {}
    body = []
    names = fn.NAMES.get(ext, {})
    for key, cls in w.classes.items():
        tn = tname(ext, key)
        nm = names.get(key, (tn, {}))[1]
        lines = ['// class %s: %d instances, %d distinct vtable layouts' % (key, len(cls.inst), len(cls.vtables))]
        lines.append('table %s {' % tn)
        for s in sorted(cls.slots):
            info = cls.slots[s]
            ty, note = field_type(w, cls, s, info, structs, ext)
            fname, conf, comment = nm.get(s, ('s%d' % s, '?', ''))
            pres = '%d/%d' % (info['count'], len(cls.inst))
            lines.append('  %s:%s (id: %d);  // conf=%s present=%s %s%s' % (fname, ty, s, conf, pres, comment, note))
        lines.append('}')
        body.append('\n'.join(lines))
    head = ['// Inferred FlatBuffers schema for .%s (New Pokemon Snap, v262144 RomFS). Generated by scripts/fb_schema.py.' % ext,
            '// Confidence per field: H = confirmed (>= 2 evidence kinds), M = consistent with data, L = guess, ? = unnamed.',
            '// Slot ids are the on-disk vtable slot numbers (id: N). Absent fields are omitted in the data when equal to default.',
            '// Sizes/layout were verified by byte-identical re-encode (scripts/fb_roundtrip.py).',
            '']
    if ident4:
        head.append('file_identifier "%s";' % ident4)
    st = []
    for n, sz in sorted(structs.items()):
        if n in ('Vec2', 'Vec3', 'Vec4'):
            f = ' '.join('%s:float;' % c for c in 'xyzw'[:sz // 4])
        elif sz % 4 == 0:
            f = ' '.join('w%d:uint;' % i for i in range(sz // 4))
        else:
            f = 'b:[ubyte:%d];' % sz
        st.append('struct %s { %s }' % (n, f))
    root = tname(ext, 'R')
    return '\n'.join(head + st + [''] + body + ['', 'root_type %s;' % root, ''])


def structure_json(ext, w, files, graphs):
    out = {'ext': ext, 'files': len(files), 'classes': {}}
    for key, cls in w.classes.items():
        slots = {}
        for s, info in sorted(cls.slots.items()):
            d = {'kind': info['kind'], 'size': info.get('size'), 'present': info['count']}
            if 'align' in info:
                d['align'] = info['align']
            if info['kind'] == 'vec':
                e = info.get('elem')
                d['elem'] = e if isinstance(e, str) else e
            if info.get('overhang'):
                d['overhang'] = True
            slots[s] = d
        out['classes'][key] = {'instances': len(cls.inst), 'vtable_layouts': len(cls.vtables), 'slots': slots}
    return out


def main():
    exts = sys.argv[1:] or (REGISTRY + CONTENT + OTHER)
    os.makedirs(os.path.join(OUT, 'schemas'), exist_ok=True)
    os.makedirs(os.path.join(OUT, 'structure'), exist_ok=True)
    rows = ['ext\tfiles\tanalysed\tclasses\ttables\tdead_objects\toverlap\tnonzero_gaps\tfiles_roundtrip_identical\tfiles_checked\tseconds']
    for ext in exts:
        t0 = time.time()
        files = fw.load_files(ROOT, ext)
        total = len(files)
        if ext in MAXSIZE:
            files = [f for f in files if os.path.getsize(f) <= MAXSIZE[ext]]
        lim = LIMITS.get(ext, 10 ** 9)
        if len(files) > lim:
            st = len(files) / lim
            files = [files[int(i * st)] for i in range(lim)]
        bufs = [open(f, 'rb').read() for f in files]
        w, graphs = fw.analyse_all(bufs)
        cov = fw.coverage(w, graphs)
        ok = 0
        for fi, g in enumerate(graphs):
            if rt.encode(w, fi, g, 'first') == bufs[fi]:
                ok += 1
        ident4 = None
        ids = {g.ident for g in graphs}
        if len(ids) == 1 and None not in ids:
            ident4 = list(ids)[0].decode()
        with open(os.path.join(OUT, 'schemas', ext + '.fbs'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(render(ext, w, graphs, ident4))
        with open(os.path.join(OUT, 'structure', ext + '.json'), 'w', encoding='utf-8') as f:
            json.dump(structure_json(ext, w, files, graphs), f, indent=1)
        ntab = sum(len(c.inst) for c in w.classes.values())
        rows.append('\t'.join(map(str, [ext, total, len(files), len(w.classes), ntab,
                                        sum(c['orphans'] for c in cov), sum(c['overlap'] for c in cov),
                                        sum(c['gaps_nonzero'] for c in cov), ok, len(files), int(time.time() - t0)])))
        print(rows[-1], flush=True)
    path = os.path.join(OUT, 'survey.tsv')
    mode = 'a' if (sys.argv[1:] and os.path.exists(path)) else 'w'
    with open(path, mode, encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(rows if mode == 'w' else rows[1:]) + '\n')


if __name__ == '__main__':
    main()
