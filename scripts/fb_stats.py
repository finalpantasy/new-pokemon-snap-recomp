"""Per-slot value statistics for schemaless-decoded registries (helps naming fields).

  python -I scripts/fb_stats.py <ext> [class-key-prefix] [--samples N]
Prints, for each class and slot: kind, size, presence, distinct values, min/max, and samples
(ints, float reinterpretation when plausible, strings).
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fb_walk as fw


def scalar_values(w, cls, slot, limit=100000):
    info = cls.slots[slot]
    out = []
    for fi, t in cls.inst[:limit]:
        b = w.bufs[fi]
        _, _, _, sl = fw.read_table(b, t)
        if slot in sl:
            p = t + sl[slot]
            out.append(bytes(b[p:p + info['size']]))
    return out


def as_num(raw):
    n = len(raw)
    if n in (1, 2, 4, 8):
        return int.from_bytes(raw, 'little')
    return None


def plausible_float(vals):
    ok = 0
    for v in vals:
        f = struct.unpack('<f', v)[0]
        if v == b'\0\0\0\0' or (1e-6 < abs(f) < 1e7):
            ok += 1
    return ok == len(vals)


def describe(w, prefix='', samples=6):
    for key, cls in w.classes.items():
        if prefix and not key.startswith(prefix):
            continue
        print('== class %s  instances=%d  vtables=%d' % (key, len(cls.inst), len(cls.vtables)))
        for s, info in sorted(cls.slots.items()):
            k = info['kind']
            line = '  slot %-3d %-6s sz=%-2s n=%d/%d' % (s, k, info.get('size'), info['count'], len(cls.inst))
            if k == 'scalar':
                raw = scalar_values(w, cls, s)
                sz = info['size']
                if sz in (1, 2, 4, 8):
                    nums = [int.from_bytes(r, 'little') for r in raw]
                    d = collections.Counter(nums)
                    line += ' distinct=%d min=%d max=%d' % (len(d), min(nums), max(nums))
                    if sz == 4 and plausible_float(raw) and max(nums) > 0x00800000:
                        fl = [struct.unpack('<f', r)[0] for r in raw]
                        line += ' FLOAT[%.4g..%.4g]' % (min(fl), max(fl))
                    top = d.most_common(samples)
                    line += ' top=' + ','.join('%d:%d' % x for x in top)
                else:
                    d = collections.Counter(raw)
                    fl = [struct.unpack('<%df' % (sz // 4), r) for r in raw] if sz % 4 == 0 else []
                    line += ' struct distinct=%d' % len(d)
                    if fl:
                        line += ' e.g. ' + ' | '.join('(' + ','.join('%.3g' % x for x in f) + ')' for f in fl[:3])
            elif k == 'string':
                vals = []
                for fi, t in cls.inst:
                    b = w.bufs[fi]
                    _, _, _, sl = fw.read_table(b, t)
                    if s in sl:
                        vals.append(fw.read_string(b, fw.fwd(b, t + sl[s])))
                d = collections.Counter(vals)
                line += ' distinct=%d e.g. ' % len(d) + ' | '.join(repr(x)[:48] for x, _ in d.most_common(samples))
            elif k == 'vec':
                line += ' elem=%s' % (info.get('elem'),)
            print(line)


def main():
    ext = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else ''
    files = fw.load_files('extracted/drp-v262144', ext)
    bufs = [open(f, 'rb').read() for f in files]
    w, graphs = fw.analyse_all(bufs)
    describe(w, prefix)


if __name__ == '__main__':
    main()
