# SPDX-License-Identifier: MIT
# Independent reference for the NVN texture tests: parses a NUTEXB, deswizzles it with a numpy implementation of the
# Tegra X1 block-linear layout (written from the public layout description, structured differently from the C++ in
# runtime/nvn/nvn_tex.cpp) and decodes it with texture2ddecoder (MIT) to RGBA8.
# Footer layout (last 0xb0 bytes; derived from 28,369 game files, see analysis/nvn/texture_survey.md in the design doc):
#   +0x00 u32 mip_sizes[16]   +0x40 char name[0x40] (starts with "TNX ")   +0x80 u32 width, height, depth, format,
#   +0x90 u32 dim (2 = 2D, 3 = 3D, 4 = cube)   +0x94 u32 mips   +0x98 u32 tile (0x1000)   +0x9c u32 layers
#   +0xa0 u32 data_size   +0xa4 " XET"   +0xa8 u32 version (2)
import struct, sys
import numpy as np

# nn::gfx image format (the nutexb format field) -> (bytes per block, block w, block h, decoder name, srgb)
FORMATS = {
    0x100: (1, 1, 1, 'r8', False), 0x400: (4, 1, 1, 'rgba8', False), 0x405: (4, 1, 1, 'rgba8', True),
    0x434: (16, 1, 1, 'rgba32f', False), 0x435: (8, 1, 1, 'rgba16f', False),
    0x480: (8, 4, 4, 'bc1', False), 0x485: (8, 4, 4, 'bc1', True), 0x490: (16, 4, 4, 'bc2', False),
    0x4a0: (16, 4, 4, 'bc3', False), 0x4a5: (16, 4, 4, 'bc3', True),
    0x180: (8, 4, 4, 'bc4', False), 0x280: (16, 4, 4, 'bc5', False),
    0x4d7: (16, 4, 4, 'bc6u', False), 0x4d8: (16, 4, 4, 'bc6s', False),
    0x4e0: (16, 4, 4, 'bc7', False), 0x4e5: (16, 4, 4, 'bc7', True),
}

def parse(path):
    d = open(path, 'rb').read()
    t = d[-0xb0:]
    sizes = struct.unpack_from('<16I', t, 0)
    w, h, dep, fmt, dim, mips, tile, layers, dsz, magic, ver = struct.unpack_from('<11I', t, 0xb0 - 44)
    assert magic == 0x54455820, 'not a nutexb'
    return dict(data=d[:-0xb0], mip_sizes=sizes, w=w, h=h, d=dep, fmt=fmt, dim=dim, mips=mips, tile=tile, layers=layers, data_size=dsz)

def block_height_log2(hb, rule):
    if rule == 'tegra_swizzle':   # ScanMountGoat/tegra_swizzle: block_height_mip0
        hh = hb + hb // 2
        return 4 if hh >= 128 else 3 if hh >= 64 else 2 if hh >= 32 else 1 if hh >= 16 else 0
    gobs = -(-hb // 8)
    l = 0
    while (1 << l) < gobs and l < 4: l += 1
    return l

def level_bh(bh0, hb):
    while bh0 > 0 and (8 << (bh0 - 1)) >= hb: bh0 -= 1
    return bh0

def level_bd(bd0, d):
    while bd0 > 0 and (1 << (bd0 - 1)) >= d: bd0 -= 1
    return bd0

def layout(w, h, d, bpb, bw, bh, mips, layers, rule='tegra_swizzle'):
    """returns (list of (offset, wb, hb, d, bh_log2, bd_log2), layer_size)"""
    hb0 = -(-h // bh)
    bh0 = block_height_log2(hb0, rule)
    bd0 = 0
    if d > 1:   # 3D rule (see docs/nvn-vulkan-design.md): depth log2 capped at 4, block height limited so bh*bd <= 16 GOBs
        while (1 << bd0) < d and bd0 < 4: bd0 += 1
        bh0 = min(bh0, 4 - bd0)
    out = []; off = 0
    for m in range(mips):
        mw, mh, md = max(w >> m, 1), max(h >> m, 1), max(d >> m, 1)
        wb, hb = -(-mw // bw), -(-mh // bh)
        lbh, lbd = level_bh(bh0, hb), level_bd(bd0, md)
        size = (-(-wb * bpb // 64)) * 512 * (1 << lbh) * (1 << lbd) * (-(-hb // (8 << lbh))) * (-(-md // (1 << lbd)))
        out.append((off, wb, hb, md, lbh, lbd, size)); off += size
    if layers > 1:
        al = 512 << bh0
        off = -(-off // al) * al
    return out, off

def deswizzle_level(src, off, wb, hb, d, bpb, bhl, bdl):
    """block-linear -> linear bytes, shape (d, hb, wb*bpb); vectorised GOB address computation."""
    rowb = wb * bpb
    x = np.arange(rowb, dtype=np.int64)[None, None, :]
    y = np.arange(hb, dtype=np.int64)[None, :, None]
    z = np.arange(d, dtype=np.int64)[:, None, None]
    bh, bd = 1 << bhl, 1 << bdl
    width_gobs = -(-rowb // 64)
    gob_rows = 8 * bh
    blocks_y = -(-hb // gob_rows)
    block_size = 512 * bh * bd
    addr = ((z // bd) * blocks_y + (y // gob_rows)) * width_gobs * block_size
    addr = addr + (x // 64) * block_size
    addr = addr + (((z % bd) * bh) + ((y % gob_rows) // 8)) * 512
    addr = addr + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)
    buf = np.frombuffer(src, dtype=np.uint8)
    return buf[off + addr]

def decode_blocks(lin, w, h, fmt):
    """linear block data (d,hb,wb*bpb) -> RGBA8 (h,w,4) of slice 0 (uint8) or float for hdr formats"""
    import texture2ddecoder as t2d
    bpb, bw, bh, kind, srgb = FORMATS[fmt]
    sl = np.ascontiguousarray(lin[0])
    if kind in ('r8',):
        a = sl.reshape(-1, sl.shape[1])[:h, :w]
        out = np.zeros((h, w, 4), np.uint8); out[..., 0] = a; out[..., 3] = 255
        return out
    if kind == 'rgba8':
        return sl.reshape(sl.shape[0], -1, 4)[:h, :w].copy()
    if kind == 'rgba32f':
        return sl.view('<f4').reshape(sl.shape[0], -1, 4)[:h, :w].copy()
    if kind == 'rgba16f':
        return sl.view('<f2').reshape(sl.shape[0], -1, 4)[:h, :w].astype(np.float32)
    fn = {'bc1': 'decode_bc1', 'bc3': 'decode_bc3', 'bc4': 'decode_bc4', 'bc5': 'decode_bc5',
          'bc6u': 'decode_bc6', 'bc6s': 'decode_bc6', 'bc7': 'decode_bc7'}[kind]
    fn = getattr(t2d, fn)
    wb = -(-w // 4); hb = -(-h // 4)
    data = np.ascontiguousarray(sl[:hb, :wb * bpb]).tobytes()
    if kind == 'bc6u' or kind == 'bc6s':
        raise NotImplementedError('bc6 float decode handled by caller')
    px = fn(data, wb * 4, hb * 4)   # BGRA bytes
    a = np.frombuffer(px, np.uint8).reshape(hb * 4, wb * 4, 4)[:h, :w, [2, 1, 0, 3]]
    return np.ascontiguousarray(a)

def level_linear(tex, mip, layer=0, rule='tegra_swizzle'):
    bpb, bw, bh, kind, srgb = FORMATS[tex['fmt']]
    lv, layer_size = layout(tex['w'], tex['h'], tex['d'], bpb, bw, bh, tex['mips'], tex['layers'], rule)
    off, wb, hb, d, bhl, bdl, size = lv[mip]
    return deswizzle_level(tex['data'], layer * layer_size + off, wb, hb, d, bpb, bhl, bdl), lv, layer_size

if __name__ == '__main__':
    import os, random
    root = sys.argv[1]; step = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    files = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.endswith('.nutexb'): files.append(os.path.join(dp, f))
    files = files[::step]
    bad = {'tegra_swizzle': 0, 'gobs': 0}; tot = 0; seen = set()
    for p in files:
        t = parse(p)
        if t['fmt'] not in FORMATS or t['tile'] == 0: continue
        bpb, bw, bh, kind, srgb = FORMATS[t['fmt']]
        key = (t['w'], t['h'], t['d'], t['fmt'], t['mips'], t['layers'])
        if key in seen: continue
        seen.add(key); tot += 1
        for rule in bad:
            lv, ls = layout(t['w'], t['h'], t['d'], bpb, bw, bh, t['mips'], t['layers'], rule)
            ok = ls * t['layers'] == t['data_size'] and all(max(l[6], 0) == t['mip_sizes'][i] or t['layers'] > 1 for i, l in enumerate(lv))
            if not ok:
                bad[rule] += 1
                if bad[rule] <= 3: print(rule, 'MISMATCH', key, ls * t['layers'], t['data_size'], [l[6] for l in lv][:4], t['mip_sizes'][:4])
    print('distinct geometries', tot, 'mismatches', bad)
