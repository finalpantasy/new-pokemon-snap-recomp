"""Independent round-trip check using the stock Python `flatbuffers` package (tools/venv-fbs).

Re-encodes decoded registry/course files with flatbuffers.Builder (not the in-tree builder), driven only
by the inferred structure, and compares with the original bytes.

  tools/venv-fbs/Scripts/python.exe -I scripts/fb_roundtrip_stock.py <ext> [<ext> ...]
Supports the structure used by mdfb/misd/courseb/etc.: tables with 1/2/4-byte scalars, strings,
vectors of tables/strings/scalars. Skips (reports) files that need structs or raw dead objects.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flatbuffers
import fb_walk as fw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'extracted', 'drp-v262144')


def encode(w, fi, g):
    b = w.bufs[fi]
    bl = flatbuffers.Builder(0)
    created = {}
    for addr, o in sorted(g.objs.items(), key=lambda kv: -kv[0]):
        k = o['kind']
        if k == 'S':
            ln = fw.U32(b, addr)[0]
            created[addr] = bl.CreateString(bytes(b[addr + 4: addr + 4 + ln]))
        elif k in ('VT', 'VS'):
            n = o['n']
            ch = [created[fw.fwd(b, addr + 4 + 4 * i)] for i in range(n)]
            bl.StartVector(4, n, 4)
            for c in reversed(ch):
                bl.PrependUOffsetTRelative(c)
            created[addr] = bl.EndVector()
        elif k == 'VX':
            n, es = o['n'], o['es']
            if es not in (1, 2, 4):
                return None
            bl.StartVector(es, n, es)
            for i in reversed(range(n)):
                v = int.from_bytes(b[addr + 4 + es * i: addr + 4 + es * (i + 1)], 'little')
                {1: bl.PrependUint8, 2: bl.PrependUint16, 4: bl.PrependUint32}[es](v)
            created[addr] = bl.EndVector()
        elif k == 'T':
            cls = w.classes[o['cls']]
            vt, vsize, tsize, slots = fw.read_table(b, addr)
            nslots = (vsize - 4) // 2
            bl.StartObject(nslots)
            for sl, off in sorted(slots.items(), key=lambda kv: -kv[1]):
                info = cls.slots[sl]
                kind = info['kind']
                if kind == 'scalar':
                    sz = info['size']
                    if sz not in (1, 2, 4):
                        return None
                    v = int.from_bytes(b[addr + off: addr + off + sz], 'little')
                    {1: bl.PrependUint8Slot, 2: bl.PrependUint16Slot, 4: bl.PrependUint32Slot}[sz](sl, v, v + 1 if v < 2 ** (8 * sz) - 1 else v - 1)
                else:
                    bl.PrependUOffsetTRelativeSlot(sl, created[o['fields'][sl][2]], 0)
            created[addr] = bl.EndObject()
        else:
            return None
    ident = g.ident
    bl.Finish(created[g.root], ident) if ident else bl.Finish(created[g.root])
    return bytes(bl.Output())


def main():
    for ext in sys.argv[1:]:
        files = fw.load_files(ROOT, ext)
        bufs = [open(f, 'rb').read() for f in files]
        w, graphs = fw.analyse_all(bufs)
        ok = skip = bad = 0
        first = None
        for fi, g in enumerate(graphs):
            out = encode(w, fi, g)
            if out is None:
                skip += 1
            elif out == bufs[fi]:
                ok += 1
            else:
                bad += 1
                first = first or (files[fi][-30:], len(out), len(bufs[fi]))
        print(ext, 'files', len(files), 'identical', ok, 'differ', bad, 'skipped(unsupported)', skip, first or '')


if __name__ == '__main__':
    main()
