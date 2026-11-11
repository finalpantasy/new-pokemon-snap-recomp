"""Schemaless FlatBuffers walker.

Infers tables / vectors / strings / scalars without a schema, pooled across all
files of one registry type, and verifies structure (bounds, overlap, coverage).

Usage (reads game data, so run with -I):
  python -I scripts/fb_walk.py <ext> [--root extracted/drp-v262144] [--json out.json]
  python -I scripts/fb_walk.py file <path>           # single-file structure dump

Model
  class key  = path of slots from the root, e.g. "R.0[].3"  ("[]" = vector element)
  slot kinds = 'table' | 'string' | 'vec' | 'scalar'
  vec elem   = 'table' | 'string' | scalar size N (N bytes per element, struct vectors)
Only the library part is source; no game data is embedded.
"""
import os, sys, struct, json, collections

U32 = struct.Struct('<I').unpack_from
I32 = struct.Struct('<i').unpack_from
U16 = struct.Struct('<H').unpack_from
OVERHANG_MAX = 40   # some writers record a table size that excludes trailing inline structs


# ----------------------------------------------------------------- primitives
def read_table(b, t):
    """Return (vt, vsize, tsize, {slot: off}) if t looks like a table, else None."""
    n = len(b)
    if t < 4 or t % 4 or t + 4 > n:
        return None
    vt = t - I32(b, t)[0]
    if vt < 0 or vt % 2 or vt + 4 > n:
        return None
    vsize, tsize = U16(b, vt)[0], U16(b, vt + 2)[0]
    if vsize < 4 or vsize % 2 or vt + vsize > n or tsize < 4 or t + tsize > n:
        return None
    slots = {}
    for i in range((vsize - 4) // 2):
        o = U16(b, vt + 4 + 2 * i)[0]
        if o:
            if o < 4 or o >= tsize + OVERHANG_MAX or t + o >= n:
                return None
            slots[i] = o
    return vt, vsize, tsize, slots


def read_string(b, s):
    """Return decoded string if s looks like an FB string, else None."""
    n = len(b)
    if s % 4 or s + 5 > n:
        return None
    ln = U32(b, s)[0]
    if s + 4 + ln + 1 > n or b[s + 4 + ln] != 0:
        return None
    raw = bytes(b[s + 4: s + 4 + ln])
    try:
        txt = raw.decode('utf-8')
    except UnicodeDecodeError:
        return None
    if any(ord(c) < 32 and c not in '\t\n\r' for c in txt):
        return None
    return txt


def fwd(b, p):
    """Follow a u32 forward offset at p; None if out of range."""
    if p + 4 > len(b):
        return None
    v = U32(b, p)[0]
    if v < 4 or (p + v) % 4 or p + v + 4 > len(b):
        return None
    return p + v


def pow2_floor(x):
    return 8 if x >= 8 else 4 if x >= 4 else 2 if x >= 2 else 1


# ----------------------------------------------------------------- class pool
class Cls:
    def __init__(self, key):
        self.key = key
        self.inst = []            # (file idx, table pos)
        self.slots = {}           # slot -> info dict
        self.vtables = set()


class Walker:
    def __init__(self, bufs):
        self.bufs = bufs                  # list of bytes
        self.classes = collections.OrderedDict()
        self.errors = []
        self.objs = [dict() for _ in bufs]   # per file: addr -> (kind, size, desc)

    # -- slot sizing from layout gaps across all instances
    def analyse(self, cls):
        gaps = {}
        offs_seen = collections.defaultdict(set)
        for fi, t in cls.inst:
            b = self.bufs[fi]
            vt, vsize, tsize, slots = read_table(b, t)
            cls.vtables.add(bytes(b[vt:vt + vsize]))
            lst = sorted(slots.items(), key=lambda kv: kv[1])
            for k, (s, o) in enumerate(lst):
                nxt = lst[k + 1][1] if k + 1 < len(lst) else max(tsize, o + 1)
                g = nxt - o
                gaps[s] = min(gaps.get(s, 99), g)
                offs_seen[s].add(o)
        return gaps, offs_seen

    def run(self):
        work = collections.deque()
        for fi, b in enumerate(self.bufs):
            r = fwd(b, 0)
            if r is None or read_table(b, r) is None:
                self.errors.append((fi, 'bad root'))
                continue
            c = self.cls('R')
            c.inst.append((fi, r))
        work.append('R')
        done = set()
        while work:
            key = work.popleft()
            if key in done:
                continue
            done.add(key)
            if key.count('.') > 12:
                continue
            for k in self.decide(self.classes[key]):
                work.append(k)

    def cls(self, key):
        if key not in self.classes:
            self.classes[key] = Cls(key)
        return self.classes[key]

    # -- decide kind of every slot of a class; returns child class keys
    def decide(self, cls):
        gaps, offs = self.analyse(cls)
        children = []
        for s in sorted(gaps):
            info = {'off': sorted(offs[s]), 'gap': gaps[s], 'count': 0}
            cls.slots[s] = info
            vals = []
            for fi, t in cls.inst:
                b = self.bufs[fi]
                _, _, _, sl = read_table(b, t)
                if s in sl:
                    vals.append((fi, t + sl[s]))
            info['count'] = len(vals)
            kind = self.classify_slot(vals, gaps[s], info)
            info['kind'] = kind
            ck = cls.key + '.' + str(s)
            if kind == 'table':
                c = self.cls(ck)
                for fi, p in vals:
                    c.inst.append((fi, fwd(self.bufs[fi], p)))
                children.append(ck)
            elif kind == 'vec':
                ek = self.classify_vec(vals, info)
                info['elem'] = ek
                if ek in ('table', 'tvec'):
                    pass
                if ek == 'table':
                    c = self.cls(ck + '[]')
                    for fi, p in vals:
                        v = fwd(self.bufs[fi], p)
                        n = U32(self.bufs[fi], v)[0]
                        for i in range(n):
                            q = v + 4 + 4 * i
                            c.inst.append((fi, fwd(self.bufs[fi], q)))
                    children.append(ck + '[]')
                elif ek == 'vecvec':
                    pass
        return children

    def classify_slot(self, vals, gap, info):
        # offset candidate: gap>=4, every value is a forward ref to string/table/vector
        if gap >= 4:
            kinds = collections.Counter()
            ok = True
            for fi, p in vals:
                b = self.bufs[fi]
                t = fwd(b, p)
                if t is None:
                    ok = False
                    break
                if read_string(b, t) is not None:
                    kinds['string'] += 1
                if read_table(b, t) is not None:
                    kinds['table'] += 1
                kinds['any'] += 1
            if ok and vals:
                nstr, ntab = kinds['string'], kinds['table']
                tot = kinds['any']
                # a zero-length vector/string reads as string: prefer decisions by majority
                if ntab == tot and nstr < tot:
                    return 'table'
                if nstr == tot and ntab < tot:
                    return 'string'
                if ntab == tot and nstr == tot:
                    # both plausible (e.g. tiny table); vector of empty? decide below
                    return 'vec' if self.vec_plausible(vals) else 'table'
                if self.vec_plausible(vals):
                    return 'vec'
        # scalar
        return 'scalar'

    def vec_plausible(self, vals):
        for fi, p in vals:
            b = self.bufs[fi]
            v = fwd(b, p)
            if v is None:
                return False
            n = U32(b, v)[0]
            if v + 4 + n > len(b):
                return False
        return True

    def classify_vec(self, vals, info):
        """Element kind for a vector slot: 'table','string','vec' (offset vectors) or int elem size."""
        votes = collections.Counter()
        sizes = None
        for fi, p in vals:
            b = self.bufs[fi]
            v = fwd(b, p)
            n = U32(b, v)[0]
            if n == 0:
                continue
            # offset elements?
            tabs = strs = 0
            for i in range(n):
                q = v + 4 + 4 * i
                t = fwd(b, q) if q + 4 <= len(b) else None
                if t is None:
                    tabs = strs = -10**9
                    break
                if read_table(b, t) is not None:
                    tabs += 1
                if read_string(b, t) is not None:
                    strs += 1
            if tabs == n and strs < n:
                votes['table'] += 1
            elif strs == n and tabs < n:
                votes['string'] += 1
            elif tabs == n and strs == n:
                votes['table?'] += 1
            else:
                votes['scalar'] += 1
        if (votes.get('table') or votes.get('table?')) and not votes.get('scalar') and not votes.get('string'):
            return 'table'
        if votes.get('string') and not votes.get('scalar'):
            return 'string'
        # scalar/struct vector: element size from the distance to the next object
        return self.vec_elem_size(vals)

    def vec_elem_size(self, vals):
        # candidate sizes; pick the largest that fits every instance within the file
        # and leaves the smallest total slack against the nearest following object.
        # Exact value is settled by round-trip (see fb_roundtrip.py); here use 4-aligned guess.
        best = None
        for es in (1, 2, 4, 8, 12, 16, 24, 32):
            ok = True
            for fi, p in vals:
                b = self.bufs[fi]
                v = fwd(b, p)
                n = U32(b, v)[0]
                if v + 4 + n * es > len(b):
                    ok = False
                    break
            if ok:
                best = best or es
        return {'scalar': best or 4}


# ----------------------------------------------------------------- sizing + object graph
SIZES = (1, 2, 4, 8, 12, 16, 20, 24, 32, 48, 64)


def _align(sz):
    return sz if sz in (1, 2) else 4


def size_candidates(w, cls, slot):
    """Infer (size, align) of an inline scalar/struct slot using the builder's placement rule.

    Fields are prepended highest address first, each aligned to its own alignment, so a field with
    size s / alignment a directly below a field at address A sits at floor((A - s)/a)*a. For the
    highest field of a table the end is the table end and trailing padding is < a. Scalars of 1, 2
    and 4 bytes align to their size; everything larger (Vec2/Vec3/Vec4 style structs) aligns to 4
    (no 8-byte-aligned scalar has been needed: file sizes are only 4-aligned in most registries).
    Returns (set of (size, align), overhang)."""
    cands = None
    over = False
    for fi, t in cls.inst:
        b = w.bufs[fi]
        vt, vsize, tsize, slots = read_table(b, t)
        if slot not in slots:
            continue
        lst = sorted(slots.values())
        o = slots[slot]
        k = lst.index(o)
        ok = set()
        pairs = [(s, _align(s)) for s in SIZES]
        if len(b) % 8 == 0:
            pairs.append((8, 8))
        if k + 1 < len(lst):
            nxt = t + lst[k + 1]
            for s, a in pairs:
                if (t + o) % a == 0 and t + o + s <= nxt and ((nxt - s) // a) * a == t + o                         and not any(b[t + o + s: nxt]):
                    ok.add((s, a))
        else:
            end = t + tsize
            if o >= tsize:
                over = True        # overhang: resolved from object distances later
                continue
            for s, a in pairs:
                if (t + o) % a == 0 and t + o + s <= end and end - (t + o) - s < a                         and not any(b[t + o + s: end]):
                    ok.add((s, a))
        cands = ok if cands is None else (cands & ok)
    return (cands or set()), over


def fill_sizes(w):
    for cls in w.classes.values():
        for s, info in cls.slots.items():
            if info['kind'] == 'scalar':
                c, over = size_candidates(w, cls, s)
                info['size_cands'] = sorted(str(x) for x in c)
                if over:
                    info['overhang'] = True
                    info['size'], info['align'] = 4, 4
                elif c:
                    info['size'], info['align'] = (8, 8) if (8, 8) in c else sorted(c)[0]
                else:
                    info['size'], info['align'] = max(info['gap'], 1), 4
                    info['unresolved'] = True
                if info['size'] > 8 or (info['size'] == 8):
                    info['struct'] = True
            elif info['kind'] in ('table', 'string', 'vec'):
                info['size'] = 4
                info['align'] = 4


def tight_scalar_sizes(w):
    pass



class FileGraph:
    """Objects of one file reachable from the root, with extents."""
    def __init__(self):
        self.objs = {}      # addr -> dict(kind, size, cls)
        self.vtables = {}   # addr -> size
        self.root = None
        self.ident = None
        self.problems = []


def parse_file(w, fi):
    b = w.bufs[fi]
    g = FileGraph()
    g.root = fwd(b, 0)
    if len(b) >= 8 and all(48 <= c < 123 for c in b[4:8]) and g.root >= 8:
        g.ident = bytes(b[4:8])
    stack = [('R', g.root)]
    seen = set()
    while stack:
        key, t = stack.pop()
        if (key, t) in seen:
            continue
        seen.add((key, t))
        cls = w.classes[key]
        tb = read_table(b, t)
        if tb is None:
            g.problems.append(('badtable', key, t))
            continue
        vt, vsize, tsize, slots = tb
        g.vtables[vt] = vsize
        tend = max([tsize] + [o + cls.slots[sl]['size'] for sl, o in slots.items() if sl in cls.slots])
        node = g.objs.setdefault(t, {'kind': 'T', 'size': tend, 'cls': key, 'fields': {}, 'tsize': tsize})
        for sl, o in slots.items():
            info = cls.slots.get(sl)
            if info is None:
                g.problems.append(('unknown-slot', key, sl))
                continue
            p = t + o
            k = info['kind']
            if k == 'scalar':
                node['fields'][sl] = ('scalar', o, info['size'])
            elif k == 'table':
                c = fwd(b, p)
                node['fields'][sl] = ('table', o, c)
                stack.append((key + '.' + str(sl), c))
            elif k == 'string':
                c = fwd(b, p)
                ln = U32(b, c)[0]
                g.objs.setdefault(c, {'kind': 'S', 'size': 4 + ln + 1, 'cls': None})
                node['fields'][sl] = ('string', o, c)
            elif k == 'vec':
                c = fwd(b, p)
                n = U32(b, c)[0]
                ek = info.get('elem')
                node['fields'][sl] = ('vec', o, c)
                if ek == 'table':
                    g.objs.setdefault(c, {'kind': 'VT', 'n': n, 'size': 4 + 4 * n, 'cls': key + '.' + str(sl)})
                    for i in range(n):
                        stack.append((key + '.' + str(sl) + '[]', fwd(b, c + 4 + 4 * i)))
                elif ek == 'string':
                    g.objs.setdefault(c, {'kind': 'VS', 'n': n, 'size': 4 + 4 * n})
                    for i in range(n):
                        sp = fwd(b, c + 4 + 4 * i)
                        ln = U32(b, sp)[0]
                        g.objs.setdefault(sp, {'kind': 'S', 'size': 4 + ln + 1, 'cls': None})
                else:
                    es = ek['scalar'] if isinstance(ek, dict) else 0
                    g.objs.setdefault(c, {'kind': 'VX', 'n': n, 'es': es, 'size': 4 + n * es, 'cls': key + '.' + str(sl)})
    return g


def resolve_vec_sizes(w, graphs):
    """Scalar/struct vector element size from distance to the next object (padding < 8)."""
    import bisect
    per_slot = collections.defaultdict(list)
    for fi, g in enumerate(graphs):
        starts = sorted(set(list(g.objs.keys()) + list(g.vtables.keys())))
        for a, o in g.objs.items():
            if o['kind'] != 'VX':
                continue
            i = bisect.bisect_right(starts, a)
            nxt = starts[i] if i < len(starts) else len(w.bufs[fi])
            per_slot[o['cls']].append((o['n'], nxt - a - 4))
    res = {}
    for ck, lst in per_slot.items():
        best = None
        for es in (1, 2, 4, 8, 12, 16, 20, 24, 32, 48, 64):
            slack = 0
            ok = True
            for n, avail in lst:
                if n == 0:
                    continue
                if n * es > avail or avail - n * es >= 8:
                    ok = False
                    break
                slack += avail - n * es
            if ok and (best is None or slack < best[0]):
                best = (slack, es)
        res[ck] = best[1] if best else 0
    return res


def apply_vec_sizes(w, res):
    for ck, es in res.items():
        parent, slot = ck.rsplit('.', 1)
        info = w.classes[parent].slots[int(slot)]
        info['elem'] = {'scalar': es or 4, 'unresolved': es == 0}


def refine_overhang(w, graphs):
    import bisect
    best = {}
    for fi, g in enumerate(graphs):
        b = w.bufs[fi]
        starts = sorted(set(list(g.objs.keys()) + list(g.vtables.keys())))
        for a, o in g.objs.items():
            if o['kind'] != 'T':
                continue
            cls = w.classes[o['cls']]
            vt, vsize, tsize, slots = read_table(b, a)
            for sl, off in slots.items():
                inf = cls.slots.get(sl)
                if not inf or not inf.get('overhang'):
                    continue
                i = bisect.bisect_right(starts, a)
                nxt = starts[i] if i < len(starts) else len(b)
                d = nxt - (a + off)
                key = (o['cls'], sl)
                best[key] = min(best.get(key, 999), d)
    for (ck, sl), d in best.items():
        w.classes[ck].slots[sl]['size'] = d
        w.classes[ck].slots[sl]['overhang_size'] = d


def analyse_all(bufs):
    w = Walker(bufs)
    w.run()
    fill_sizes(w)
    tight_scalar_sizes(w)
    graphs = [parse_file(w, i) for i in range(len(bufs))]
    refine_overhang(w, graphs)
    graphs = [parse_file(w, i) for i in range(len(bufs))]
    res = resolve_vec_sizes(w, graphs)
    apply_vec_sizes(w, res)
    graphs = [parse_file(w, i) for i in range(len(bufs))]
    add_orphans(w, graphs)
    return w, graphs


def add_orphans(w, graphs):
    """Unreferenced non-zero byte runs between objects (dead data left by the writer).
    Strings are recognised; anything else is kept as a raw blob."""
    for fi, g in enumerate(graphs):
        b = w.bufs[fi]
        spans = [(a, a + o['size']) for a, o in g.objs.items()]
        spans += [(a, a + sz) for a, sz in g.vtables.items()]
        spans.append((0, 8 if g.ident else 4))
        spans.sort()
        pos = 0
        gaps = []
        for a, e in spans:
            if a > pos:
                gaps.append((pos, a))
            pos = max(pos, e)
        if pos < len(b):
            gaps.append((pos, len(b)))
        first = min([a for a in g.objs] + [a for a in g.vtables] + [len(b)])
        for (x, y) in gaps:
            if x < first and not any(b[x:y]):
                continue          # front padding added by the final alignment step
            if not any(b[x:y]):
                r = (y - x) % 4
                if y - x - r >= 4:
                    # all-zero dead object (e.g. an empty string/vector nothing points to)
                    g.objs[x + r] = {'kind': 'RAW', 'size': y - x - r, 'cls': None, 'orphan': True, 'zero': True}
                continue
            p = x
            while p < y:
                if b[p:p + 4] == b'\x00\x00\x00\x00' and p % 4 == 0:
                    p += 4
                    continue
                st = read_string(b, p) if p % 4 == 0 else None
                if st is not None and p + 5 + U32(b, p)[0] <= y:
                    sz = 4 + U32(b, p)[0] + 1
                    g.objs[p] = {'kind': 'S', 'size': sz, 'cls': None, 'orphan': True}
                    p += sz
                    p += (-p) % 4
                else:
                    q = p
                    while q < y and not (b[q:q + 4] == b'\x00\x00\x00\x00' and q % 4 == 0 and q > p):
                        q += 4 if (q % 4 == 0) else 1
                    q = min(q, y)
                    g.objs[p] = {'kind': 'RAW', 'size': q - p, 'cls': None, 'orphan': True}
                    p = q


def pos_end(g):
    return max([a + o['size'] for a, o in g.objs.items()] + [a + sz for a, sz in g.vtables.items()] + [4])


def coverage(w, graphs):
    """Structural verification: per file, uncovered bytes (must be zero padding), overlaps."""
    out = []
    for fi, g in enumerate(graphs):
        b = w.bufs[fi]
        spans = [(a, a + o['size']) for a, o in g.objs.items()]
        spans += [(a, a + sz) for a, sz in g.vtables.items()]
        spans.append((0, 8 if g.ident else 4))
        spans.sort()
        pos = 0
        gaps_nz = gaps_bytes = overlap = 0
        for a, e in spans:
            if a < pos:
                overlap += 1
            elif a > pos:
                seg = b[pos:a]
                gaps_bytes += len(seg)
                if any(seg):
                    gaps_nz += 1
            pos = max(pos, e)
        orph = [(a, o) for a, o in g.objs.items() if o.get('orphan')]
        out.append({'file': fi, 'size': len(b), 'objs': len(g.objs),
                    'orphans': len(orph), 'orphan_raw': sum(1 for a, o in orph if o['kind'] == 'RAW'),
                    'orphan_bytes': sum(o['size'] for a, o in orph), 'tail_nonzero': any(b[pos_end(g):]), 'vtables': len(g.vtables),
                    'overlap': overlap, 'gap_bytes': gaps_bytes, 'gaps_nonzero': gaps_nz,
                    'tail': len(b) - pos, 'problems': len(g.problems)})
    return out


# ----------------------------------------------------------------- helpers for CLI
def load_files(root, ext):
    out = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.endswith('.' + ext):
                out.append(os.path.join(dp, f))
    return sorted(out)


def main():
    if len(sys.argv) < 3 and not (len(sys.argv) == 2):
        print(__doc__)
        return
    root = 'extracted/drp-v262144'
    ext = sys.argv[1]
    files = load_files(root, ext)
    bufs = [open(f, 'rb').read() for f in files]
    w, graphs = analyse_all(bufs)
    cov = coverage(w, graphs)
    print(ext, 'files', len(files), 'errors', len(w.errors),
          'overlap', sum(c['overlap'] for c in cov), 'gaps_nonzero', sum(c['gaps_nonzero'] for c in cov),
          'tail', sum(c['tail'] for c in cov), 'problems', sum(c['problems'] for c in cov))
    for k, c in w.classes.items():
        print(k, 'inst', len(c.inst), 'vtables', len(c.vtables))
        for s, i in c.slots.items():
            print('   slot', s, i['kind'], 'sz', i.get('size'), 'n', i['count'], 'off', i['off'][:4], i.get('elem', ''), i.get('size_cands', '') if i['kind']=='scalar' else '')


if __name__ == '__main__':
    main()
