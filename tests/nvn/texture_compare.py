# SPDX-License-Identifier: MIT
# Drives build/nvn/texture_test.exe over real NUTEXB files from extracted/ (one per major format, picked at run time) and
# compares the layer's output with an independent numpy deswizzle + texture2ddecoder decode (tests/nvn/nutexb_ref.py).
#   python -I tests/nvn/texture_compare.py [--root extracted/romfs-v262144] [--files a.nutexb b.nutexb] [--out analysis/nvn]
# Raw block data must match byte for byte (deswizzle, layout, mips, cube faces, 3D slices). The GPU-decoded level 0 is
# compared per channel with a tolerance (BC decoders differ by up to 2 levels between implementations).
import os, subprocess, sys, struct, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import nutexb_ref as ref

EXE = os.path.join(ROOT, 'build', 'nvn', 'texture_test.exe')
WANT = [  # (label, predicate on parsed footer)
    ('bc1', lambda t: t['fmt'] in (0x480, 0x485) and t['dim'] == 2 and t['mips'] > 3),
    ('bc3', lambda t: t['fmt'] in (0x4a0, 0x4a5) and t['dim'] == 2 and t['mips'] > 3),
    ('bc4', lambda t: t['fmt'] == 0x180 and t['dim'] == 2),
    ('bc5', lambda t: t['fmt'] == 0x280 and t['dim'] == 2),
    ('bc6h', lambda t: t['fmt'] in (0x4d7, 0x4d8) and t['dim'] == 2),
    ('bc7', lambda t: t['fmt'] in (0x4e0, 0x4e5) and t['dim'] == 2 and t['mips'] > 2),
    ('rgba8_srgb', lambda t: t['fmt'] == 0x405 and t['dim'] == 2),
    ('rgba8', lambda t: t['fmt'] == 0x400 and t['dim'] == 2),
    ('rgba16f_3d', lambda t: t['fmt'] == 0x435 and t['dim'] == 3),
    ('bc1_3d', lambda t: t['fmt'] in (0x480, 0x485) and t['dim'] == 3),
    ('cube', lambda t: t['dim'] == 4),
]

def footer(path):
    sz = os.path.getsize(path)
    if sz < 0xb0 + 16: return None
    with open(path, 'rb') as f:
        f.seek(sz - 0xb0); t = f.read(0xb0)
    w, h, dep, fmt, dim, mips, tile, layers, dsz, magic, ver = struct.unpack_from('<11I', t, 0xb0 - 44)
    if magic != 0x54455820: return None
    return dict(w=w, h=h, d=dep, fmt=fmt, dim=dim, mips=mips, layers=layers, size=sz)

def pick(root, limit=60000):
    import json
    cache = os.path.join(ROOT, 'build', 'nvn', 'picks.json')
    if os.path.exists(cache):
        c = json.load(open(cache))
        if all(os.path.exists(v) for v in c.values()): return c
    found = {}
    n = 0
    for dp, _, fs in os.walk(root):
        for fn in fs:
            if not fn.endswith('.nutexb'): continue
            p = os.path.join(dp, fn)
            t = footer(p)
            n += 1
            if t is None or t['fmt'] not in ref.FORMATS or t['size'] > 6 << 20 or t['w'] < 16: continue
            for label, pred in WANT:
                if label not in found and pred(t): found[label] = p
            if len(found) == len(WANT) or n > limit:
                json.dump(found, open(cache, 'w')); return found
    json.dump(found, open(cache, 'w'))
    return found

def run_one(label, path, outdir):
    prefix = os.path.join(outdir, label)
    p = subprocess.run([EXE, path, prefix], capture_output=True, text=True,
                       creationflags=0x00004000, env=dict(os.environ, NVN_VK_VALIDATION='1'), timeout=300)  # BELOW_NORMAL
    sys.stdout.write('    ' + p.stdout.strip().replace('\n', '\n    ') + '\n')
    if p.returncode not in (0, 3):
        print('    texture_test failed rc=%d: %s' % (p.returncode, p.stderr.strip()[-300:])); return None
    man = {}
    levels = []
    for line in open(prefix + '.manifest.tsv'):
        k = line.rstrip('\n').split('\t')
        if k[0] == 'level': levels.append(tuple(int(x) for x in k[1:]))
        else: man[k[0]] = k[1]
    val = [l for l in p.stdout.splitlines() if l.startswith('wrote')]
    errs = int(val[0].split('errors=')[1].split()[0]) if val else -1
    return man, levels, errs

def check(label, path, outdir):
    t = ref.parse(path)
    r = run_one(label, path, outdir)
    if r is None: return dict(label=label, ok=False, note='run failed')
    man, levels, errs = r
    bpb, bw, bh, kind, srgb = ref.FORMATS[t['fmt']]
    layers = 6 if t['dim'] == 4 else 1
    depth = t['d'] if t['dim'] == 3 else 1
    lay, layer_size = ref.layout(t['w'], t['h'], depth, bpb, bw, bh, t['mips'], layers)
    data = t['data']
    prefix = os.path.join(outdir, label)
    raw_ok = 0; raw_bad = []
    for (m, z, w, h, d, size) in levels:
        off, wb, hb, md, bhl, bdl, lsz = lay[m]
        lin = ref.deswizzle_level(data, z * layer_size + off, wb, hb, md, bpb, bhl, bdl)
        got = np.fromfile('%s.l%d.z%d.bin' % (prefix, m, z), dtype=np.uint8)
        exp = np.ascontiguousarray(lin).reshape(-1)
        if got.size == exp.size and np.array_equal(got, exp): raw_ok += 1
        else: raw_bad.append((m, z, got.size, exp.size))
    res = dict(label=label, file=os.path.relpath(path, ROOT), fmt=hex(t['fmt']), dim=t['dim'], size='%dx%dx%d' % (t['w'], t['h'], depth),
               mips=t['mips'], layers=layers, subres=len(levels), raw_ok=raw_ok, raw_bad=raw_bad[:3], val_errors=errs)
    # decoded level 0 / layer 0
    dec_file = prefix + '.decoded.bin'
    w, h = t['w'], t['h']
    isf = kind in ('rgba16f', 'rgba32f', 'bc6u', 'bc6s')
    got = np.fromfile(dec_file, dtype=np.float16 if isf else np.uint8)
    lin0 = ref.deswizzle_level(data, lay[0][0], lay[0][1], lay[0][2], lay[0][3], bpb, lay[0][4], lay[0][5])
    try:
        exp = ref.decode_blocks(lin0, w, h, t['fmt'])
    except NotImplementedError:
        import texture2ddecoder as t2d
        sl = np.ascontiguousarray(lin0[0])
        wb4, hb4 = -(-w // 4), -(-h // 4)
        px = t2d.decode_bc6(np.ascontiguousarray(sl[:hb4, :wb4 * bpb]).tobytes(), wb4 * 4, hb4 * 4)
        exp = np.frombuffer(px, np.uint8).reshape(hb4 * 4, wb4 * 4, 4)[:h, :w, [2, 1, 0, 3]]
    got = got.reshape(h, w, 4)
    if isf and exp.dtype == np.uint8:   # texture2ddecoder tone-maps BC6 to 8 bit; compare after clamping the GPU float result
        g = np.clip(got.astype(np.float32), 0, 1)
        res['dec_note'] = 'bc6 float vs 8-bit decoder: compared after clamp to [0,1] (decoder range convention unverified)'
        diff = np.abs(g[..., :3] * 255 - exp[..., :3].astype(np.float32))
        res['dec_max'] = float(diff.max()); res['dec_mean'] = float(diff.mean()); res['dec_p999'] = float(np.percentile(diff, 99.9))
        res['dec_ok'] = bool(res['dec_mean'] < 3.0)
    elif isf:
        diff = np.abs(got.astype(np.float32) - exp.astype(np.float32))
        res['dec_max'] = float(diff.max()); res['dec_mean'] = float(diff.mean()); res['dec_p999'] = float(np.percentile(diff, 99.9))
        res['dec_ok'] = bool(res['dec_max'] <= 1e-3 * max(1.0, float(np.abs(exp).max())))
    else:
        ch = {'bc4': [0], 'bc5': [0, 1], 'r8': [0]}.get(kind, [0, 1, 2, 3])
        diff = np.abs(got[..., ch].astype(np.int16) - exp[..., ch].astype(np.int16))
        res['dec_max'] = int(diff.max()); res['dec_mean'] = float(diff.mean()); res['dec_p999'] = float(np.percentile(diff, 99.9))
        lim = (8, 3) if kind in ('bc4', 'bc5') else (6, 2)   # hardware BC4 deviates up to 7 from two software decoders on rare pixels
        res['dec_ok'] = bool(res['dec_max'] <= lim[0] and res['dec_p999'] <= lim[1])   # BC1 sRGB corner blocks differ by up to 5 between decoders
        # screenshot of the GPU-decoded pixels
        try:
            from PIL import Image
            Image.fromarray(got.astype(np.uint8)).save(prefix + '.png')
        except Exception:
            pass
    res['ok'] = (raw_ok == len(levels)) and res['dec_ok'] and errs == 0
    return res

def main():
    a = sys.argv[1:]
    root = os.path.join(ROOT, 'extracted', 'drp-v262144'); out = os.path.join(ROOT, 'analysis', 'nvn'); files = []
    i = 0
    while i < len(a):
        if a[i] == '--root': root = a[i + 1]; i += 2
        elif a[i] == '--out': out = a[i + 1]; i += 2
        elif a[i] == '--files': files = a[i + 1:]; break
        else: i += 1
    os.makedirs(out, exist_ok=True)
    picks = {os.path.basename(f): f for f in files} if files else pick(root)
    print('picked:', {k: os.path.relpath(v, ROOT) for k, v in picks.items()})
    results = []
    for label, path in picks.items():
        print('== %s' % label)
        r = check(label, path, out)
        print('   ', r)
        results.append(r)
    with open(os.path.join(out, 'texture_results.tsv'), 'w') as f:
        for r in results: f.write(repr(r) + '\n')
    bad = [r['label'] for r in results if not r.get('ok')]
    print('RESULT: %d/%d ok%s' % (len(results) - len(bad), len(results), (' failed: ' + ', '.join(bad)) if bad else ''))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
