"""Re-encode a schemaless-decoded FlatBuffers file with a FlatBuffers-builder-style encoder and compare.

The encoder uses only the inferred structure from fb_walk (class/slot kinds and sizes) plus the decoded
values. Objects are created in descending original address order (a builder writes back to front),
table fields are prepended highest-offset first with natural alignment, vtables are deduplicated, and
dead (unreferenced) objects found by fb_walk.add_orphans are re-emitted in place.

Usage (reads game data, so run with -I):
  python -I scripts/fb_roundtrip.py <ext> [--limit N] [--vt first|last]
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fb_walk as fw


class Builder:
    def __init__(self, lead=0):
        self.parts = []      # written downward: parts[0] is the highest address
        self.off = 0         # bytes written so far
        if lead:
            self.put(b'\0' * lead)   # writer quirk: extra zero bytes after the first object
        self.minalign = 1
        self.vtables = []    # (offset_after_write, bytes)

    def prep(self, size, additional=0):
        if size > self.minalign:
            self.minalign = size
        pad = (-(self.off + additional)) & (size - 1)
        if pad:
            self.put(b'\0' * pad)

    def put(self, data):
        self.parts.append(bytearray(data))
        self.off += len(data)

    def uoffset(self, child):
        self.prep(4)
        self.put(struct.pack('<I', self.off + 4 - child))

    def table(self, fields, vsize, tsize, vt_policy):
        """fields: list of (slot, raw_bytes | ('ref', child_created), align). Returns created offset."""
        pos = {}
        for slot, val, align, _o in sorted(fields, key=lambda f: -f[3]):
            if isinstance(val, tuple):
                self.uoffset(val[1])
            else:
                self.prep(align)
                self.put(val)
            pos[slot] = self.off
        self.prep(4)
        self.put(b'\0\0\0\0')
        tab = self.parts[-1]
        obj = self.off
        nslots = (vsize - 4) // 2
        ent = [(obj - pos[s]) if s in pos else 0 for s in range(nslots)]
        vt = struct.pack('<HH', vsize, tsize) + b''.join(struct.pack('<H', e) for e in ent)
        found = None
        cands = self.vtables if vt_policy == 'first' else list(reversed(self.vtables))
        for voff, vb in cands:
            if vb == vt:
                found = voff
                break
        if found is None:
            self.prep(2)
            self.put(vt)
            found = self.off
            self.vtables.append((found, vt))
        tab[:] = struct.pack('<i', found - obj)
        return obj

    def string(self, raw):
        self.prep(4, len(raw) + 1)
        self.put(b'\0')
        self.put(raw)
        self.put(struct.pack('<I', len(raw)))
        return self.off

    def vec_scalar(self, data, n, es, align):
        self.prep(4, len(data))
        self.prep(align, len(data))
        self.put(data)
        self.put(struct.pack('<I', n))
        return self.off

    def vec_offsets(self, children):
        self.prep(4, 4 * len(children))
        for c in reversed(children):
            self.uoffset(c)
        self.put(struct.pack('<I', len(children)))
        return self.off

    def raw(self, data):
        self.put(data)
        return self.off

    def finish(self, root, ident):
        self.prep(self.minalign, 8 if ident else 4)
        if ident:
            self.put(ident)
        self.uoffset(root)
        return b''.join(bytes(p) for p in reversed(self.parts))


def field_align(info):
    return info.get('align', 4)


def encode(w, fi, g, vt_policy='first'):
    b = w.bufs[fi]
    out = _encode(w, fi, g, vt_policy, 0)
    extra = len(b) - len(out)
    if 0 < extra <= 16 and not any(b[len(out) - 0:]) and extra % 4 == 0:
        return _encode(w, fi, g, vt_policy, extra)
    return out


def _encode(w, fi, g, vt_policy, lead):
    b = w.bufs[fi]
    bl = Builder(lead)
    if len(b) % 8 == 0:
        bl.minalign = 8     # an 8-aligned object exists, or the final size is 8-aligned anyway (never hurts)
    created = {}
    order = sorted(g.objs.items(), key=lambda kv: -kv[0])
    for addr, o in order:
        k = o['kind']
        if k == 'T':
            cls = w.classes[o['cls']]
            vt, vsize, tsize, slots = fw.read_table(b, addr)
            flds = []
            for sl, off in slots.items():
                kind, _, extra = o['fields'][sl]
                info = cls.slots[sl]
                if kind == 'scalar':
                    flds.append((sl, bytes(b[addr + off: addr + off + info['size']]), field_align(info), off))
                else:
                    flds.append((sl, ('ref', created[extra]), 4, off))
            created[addr] = bl.table(flds, vsize, tsize, vt_policy)
        elif k == 'S':
            ln = fw.U32(b, addr)[0]
            created[addr] = bl.string(bytes(b[addr + 4: addr + 4 + ln]))
        elif k in ('VT', 'VS'):
            n = o['n']
            ch = [created[fw.fwd(b, addr + 4 + 4 * i)] for i in range(n)]
            created[addr] = bl.vec_offsets(ch)
        elif k == 'VX':
            n, es = o['n'], o['es']
            al = es if es in (1, 2, 4, 8) else 4
            created[addr] = bl.vec_scalar(bytes(b[addr + 4: addr + 4 + n * es]), n, es, al)
        elif k == 'RAW':
            created[addr] = bl.raw(bytes(b[addr: addr + o['size']]))
        else:
            raise ValueError(k)
    return bl.finish(created[g.root], g.ident)


def compare(a, b):
    if a == b:
        return None
    n = min(len(a), len(b))
    i = next((i for i in range(n) if a[i] != b[i]), n)
    return i


def main():
    ext = sys.argv[1]
    lim = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 10 ** 9
    pol = sys.argv[sys.argv.index('--vt') + 1] if '--vt' in sys.argv else 'first'
    files = fw.load_files('extracted/drp-v262144', ext)
    if len(files) > lim:
        st = len(files) / lim
        files = [files[int(i * st)] for i in range(lim)]
    bufs = [open(f, 'rb').read() for f in files]
    w, graphs = fw.analyse_all(bufs)
    ok = 0
    bad = []
    for fi, g in enumerate(graphs):
        out = encode(w, fi, g, pol)
        d = compare(out, bufs[fi])
        if d is None:
            ok += 1
        else:
            bad.append((files[fi], d, len(out), len(bufs[fi])))
    print(ext, 'files', len(files), 'identical', ok, 'differ', len(bad))
    for x in bad[:5]:
        print('  first diff', x)


if __name__ == '__main__':
    main()
