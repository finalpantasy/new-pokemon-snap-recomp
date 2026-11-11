"""Decode the root-vector records of a registry type into plain Python dicts (slot -> value).

Used by the cross-reference checks (python -I scripts/fb_xref_check.py)."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fb_walk as fw

ROOT = 'extracted/drp-v262144'


def load(ext):
    files = fw.load_files(ROOT, ext)
    bufs = [open(f, 'rb').read() for f in files]
    w, graphs = fw.analyse_all(bufs)
    return files, w, graphs


def value(w, fi, t, cls, slot, o):
    info = cls.slots[slot]
    b = w.bufs[fi]
    p = t + o
    k = info['kind']
    if k == 'scalar':
        raw = bytes(b[p:p + info['size']])
        return int.from_bytes(raw, 'little') if info['size'] in (1, 2, 4, 8) else raw
    if k == 'string':
        return fw.read_string(b, fw.fwd(b, p))
    if k == 'table':
        return table(w, fi, fw.fwd(b, p), cls.key + '.' + str(slot))
    if k == 'vec':
        v = fw.fwd(b, p)
        n = fw.U32(b, v)[0]
        ek = info.get('elem')
        if ek == 'table':
            return [table(w, fi, fw.fwd(b, v + 4 + 4 * i), cls.key + '.' + str(slot) + '[]') for i in range(n)]
        if ek == 'string':
            return [fw.read_string(b, fw.fwd(b, v + 4 + 4 * i)) for i in range(n)]
        es = ek['scalar']
        if es in (1, 2, 4, 8):
            return [int.from_bytes(b[v + 4 + es * i: v + 4 + es * (i + 1)], 'little') for i in range(n)]
        return [bytes(b[v + 4 + es * i: v + 4 + es * (i + 1)]) for i in range(n)]
    return None


def table(w, fi, t, key):
    cls = w.classes[key]
    b = w.bufs[fi]
    _, _, _, sl = fw.read_table(b, t)
    return {s: value(w, fi, t, cls, s, o) for s, o in sl.items()}


def root_records(ext, field=0):
    files, w, graphs = load(ext)
    b = w.bufs[0]
    r = fw.fwd(b, 0)
    t = table(w, 0, r, 'R')
    return t[field]
