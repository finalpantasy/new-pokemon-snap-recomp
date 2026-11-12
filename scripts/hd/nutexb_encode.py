# SPDX-License-Identifier: MIT
"""PNG -> NUTEXB encoder (same format as an original nutexb, any new size, full mip chain, Tegra block-linear swizzle).

  python -E -P scripts\\hd\\nutexb_encode.py <in.png> <original.nutexb> <out.nutexb>

The format (BC1/BC3/BC4/BC5/BC7, sRGB or not), name string and other footer fields come from the original file; the
footer's mip sizes, width, height, mip count and data size are rewritten. 2D, single layer only.
BC compression uses `etcpak` (pip; MIT wrapper around the BSD-3 etcpak by Bartosz Taudul), imported at run time, never vendored.
Mips are box-filtered (in linear light for sRGB formats). The layout code is shared with tests/nvn/nutexb_ref.py (the decoder).
"""
import struct, sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests' / 'nvn'))
import nutexb_ref as nx  # noqa: E402


def swizzle_level(lin, size, wb, hb, d, bpb, bhl, bdl):
    """linear block rows (hb, wb*bpb) uint8 -> block-linear bytes of `size` (inverse of nx.deswizzle_level for one slice)."""
    rowb = wb * bpb
    x = np.arange(rowb, dtype=np.int64)[None, :]
    y = np.arange(hb, dtype=np.int64)[:, None]
    bh = 1 << bhl
    width_gobs = -(-rowb // 64)
    gob_rows = 8 * bh
    block_size = 512 * bh
    addr = (y // gob_rows) * width_gobs * block_size
    addr = addr + (x // 64) * block_size
    addr = addr + ((y % gob_rows) // 8) * 512
    addr = addr + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)
    out = np.zeros(size, np.uint8)
    out[addr] = lin
    return out


def srgb_to_lin(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255.0


def mip_chain(px, nmips, srgb):
    """list of uint8 RGBA arrays, level 0 first; box filter with edge replication for odd sizes."""
    f = px.astype(np.float32)
    if srgb:
        f[..., :3] = srgb_to_lin(f[..., :3]) * 255.0
    out = [px]
    for _ in range(nmips - 1):
        h, w = f.shape[:2]
        nh, nw = max(h // 2, 1), max(w // 2, 1)
        if h > 1 and h % 2:
            f = np.concatenate([f, f[-1:]], 0)
        if w > 1 and w % 2:
            f = np.concatenate([f, f[:, -1:]], 1)
        if f.shape[0] > 1:
            f = (f[0::2] + f[1::2]) / 2
        if f.shape[1] > 1:
            f = (f[:, 0::2] + f[:, 1::2]) / 2
        f = f[:nh, :nw]
        g = f.copy()
        if srgb:
            g[..., :3] = lin_to_srgb(g[..., :3] / 255.0)
        out.append(np.clip(g + 0.5, 0, 255).astype(np.uint8))
    return out


def _e565(c):
    r = np.clip(np.rint(c[..., 0] / 255 * 31), 0, 31).astype(np.int64)
    g = np.clip(np.rint(c[..., 1] / 255 * 63), 0, 63).astype(np.int64)
    b = np.clip(np.rint(c[..., 2] / 255 * 31), 0, 31).astype(np.int64)
    return (r << 11) | (g << 5) | b


def _d565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def bc1_color_blocks(rgb):
    """rgb uint8 (ph, pw, 3), multiples of 4 -> (N, 8) uint8 BC1 colour blocks (4-colour mode).
    PCA line fit, then iterated least-squares endpoint refinement, vectorised over all blocks."""
    ph, pw = rgb.shape[:2]
    blk = rgb.reshape(ph // 4, 4, pw // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(np.float32)
    n = len(blk)
    mean = blk.mean(1, keepdims=True)
    cen = blk - mean
    cov = np.einsum('nki,nkj->nij', cen, cen)
    w, v = np.linalg.eigh(cov + np.eye(3, dtype=np.float32) * 1e-6)
    axis = v[:, :, -1]
    t = np.einsum('nki,ni->nk', cen, axis)
    lo = mean[:, 0] + axis * t.min(1)[:, None]
    hi = mean[:, 0] + axis * t.max(1)[:, None]
    best_err = np.full(n, np.inf, np.float32)
    best = np.zeros((n, 2), np.int64)
    best_idx = np.zeros((n, 16), np.int64)
    wts = np.array([0, 1 / 3, 2 / 3, 1], np.float32)
    for it in range(5):
        c0, c1 = _e565(hi), _e565(lo)
        swap = c0 < c1
        c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
        eq = c0 == c1
        pal = np.stack([_d565(c0), (2 * _d565(c0) + _d565(c1)) / 3, (_d565(c0) + 2 * _d565(c1)) / 3, _d565(c1)], 1)  # (n,4,3)
        d = ((blk[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)  # (n,16,4)
        idx = d.argmin(-1)
        err = d.min(-1).sum(1)
        better = (err < best_err) & ~eq
        best_err = np.where(better, err, best_err)
        best[better] = np.stack([c0, c1], 1)[better]
        best_idx[better] = idx[better]
        # LS refit: colour = A*e0 + B*e1 with A = 1 - w, B = w, w by palette position (idx 0 -> e0, 3 -> e1)
        wi = np.array([0, 2 / 3, 1 / 3, 1], np.float32)[idx]  # idx 0 = c0, 1 = 2c0+c1, 2 = c0+2c1, 3 = c1 ; weight of c1
        A = 1 - wi
        B = wi
        aa, bb, ab = (A * A).sum(1), (B * B).sum(1), (A * B).sum(1)
        det = aa * bb - ab * ab
        ok = np.abs(det) > 1e-4
        det = np.where(ok, det, 1)
        ya = np.einsum('nk,nkc->nc', A, blk)
        yb = np.einsum('nk,nkc->nc', B, blk)
        e0 = (bb[:, None] * ya - ab[:, None] * yb) / det[:, None]
        e1 = (aa[:, None] * yb - ab[:, None] * ya) / det[:, None]
        # idx 0 = c0 (hi side) after the swap above; refit in the same orientation
        hi = np.where(ok[:, None], e0, hi)
        lo = np.where(ok[:, None], e1, lo)
    # constant blocks (c0 == c1 everywhere): fall back to single colour
    flat = ~np.isfinite(best_err)
    if flat.any():
        c = _e565(mean[flat, 0])
        best[flat] = np.stack([c, c], 1)
        best_idx[flat] = 0
    # BC1 index order: 0 = c0, 1 = c1, 2 = (2c0+c1)/3, 3 = (c0+2c1)/3
    remap = np.array([0, 2, 3, 1])
    bi = remap[best_idx]
    bits = (bi.astype(np.uint64) << (np.arange(16, dtype=np.uint64) * 2)).sum(1)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0] = best[:, 0] & 255
    out[:, 1] = best[:, 0] >> 8
    out[:, 2] = best[:, 1] & 255
    out[:, 3] = best[:, 1] >> 8
    for k in range(4):
        out[:, 4 + k] = (bits >> np.uint64(8 * k)) & np.uint64(255)
    return out


def compress(px, kind):
    """uint8 RGBA (h, w, 4) -> (hb, wb*bpb) block bytes. Image is padded to multiples of 4 by edge replication."""
    import etcpak
    h, w = px.shape[:2]
    ph, pw = -(-h // 4) * 4, -(-w // 4) * 4
    if (ph, pw) != (h, w):
        px = np.pad(px, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    raw = np.ascontiguousarray(px).tobytes()
    if kind in ('bc1', 'bc3'):
        col = bc1_color_blocks(np.ascontiguousarray(px[..., :3]))
        if kind == 'bc3':
            a = np.zeros_like(px)
            a[..., 0] = px[..., 3]
            al = np.frombuffer(etcpak.compress_bc4(a.tobytes(), pw, ph), np.uint8).reshape(-1, 8)
            col = np.concatenate([al, col], 1)
        return col.reshape(ph // 4, (pw // 4) * (16 if kind == 'bc3' else 8))
    fn = {'bc1': etcpak.compress_bc1, 'bc3': etcpak.compress_bc3, 'bc4': etcpak.compress_bc4,
          'bc5': etcpak.compress_bc5, 'bc7': etcpak.compress_bc7}.get(kind)
    if fn is None:
        raise NotImplementedError('format ' + kind)
    data = np.frombuffer(fn(raw, pw, ph), np.uint8)
    bpb = nx.FORMATS_BY_KIND[kind]
    return data.reshape(ph // 4, (pw // 4) * bpb)


nx.FORMATS_BY_KIND = {'bc1': 8, 'bc3': 16, 'bc4': 8, 'bc5': 16, 'bc7': 16}


def encode(png, orig, out, size=None):
    from PIL import Image
    t = nx.parse(str(orig))
    bpb, bw, bh, kind, srgb = nx.FORMATS[t['fmt']]
    if t['dim'] != 2 or t['layers'] != 1 or t['d'] != 1:
        raise NotImplementedError('only 2D single-layer textures')
    im = Image.open(png).convert('RGBA')
    if size and im.size != tuple(size):
        im = im.resize(tuple(size), Image.LANCZOS)
    px = np.asarray(im)
    h, w = px.shape[:2]
    nm = t['mips'] if (w, h) == (t['w'], t['h']) else max(w, h).bit_length()
    if t['mips'] == 1:
        nm = 1
    if kind == 'bc1' and (px[..., 3] < 255).any():
        print('warn: BC1 original has no alpha, alpha is dropped', file=sys.stderr)
    lv, layer_size = nx.layout(w, h, 1, bpb, bw, bh, nm, 1)
    chain = mip_chain(px, nm, srgb)
    buf = bytearray()
    sizes = []
    for (off, wb, hb, d, bhl, bdl, size_), mpx in zip(lv, chain):
        blocks = compress(mpx, kind)[:hb, :wb * bpb]
        sw = swizzle_level(blocks, size_, wb, hb, d, bpb, bhl, bdl)
        assert len(buf) == off
        buf += sw.tobytes()
        sizes.append(size_)
    foot = bytearray(Path(orig).read_bytes()[-0xb0:])
    struct.pack_into('<16I', foot, 0, *(sizes + [0] * (16 - len(sizes))))
    struct.pack_into('<II', foot, 0x84, w, h)
    struct.pack_into('<I', foot, 0x98, nm)
    struct.pack_into('<I', foot, 0xa4, len(buf))
    Path(out).write_bytes(bytes(buf) + bytes(foot))
    return dict(w=w, h=h, mips=nm, kind=kind, srgb=srgb, data_size=len(buf))


if __name__ == '__main__':
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    print(encode(sys.argv[1], sys.argv[2], sys.argv[3]))
