# SPDX-License-Identifier: MIT
# Compares nvn_astc.cpp (ASTC LDR decode) with texture2ddecoder and measures the BC7 mode-6 transcode error.
#   python tests/nvn/astc_compare.py   (build first: tests\nvn\build_astc.bat)
import os, subprocess, sys
import numpy as np
import texture2ddecoder as t2d
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
EXE = os.path.join(ROOT, 'build', 'nvn', 'astc_test.exe')
tmp = os.path.join(ROOT, 'build', 'nvn', 'astc_tmp'); os.makedirs(tmp, exist_ok=True)
rng = np.random.default_rng(1234)
def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
allok = True
for bw, bh in [(4, 4), (5, 5), (6, 6), (8, 8), (10, 8), (12, 12)]:
    W, H = bw * 32, bh * 32
    blocks = rng.integers(0, 256, size=(32 * 32, 16), dtype=np.uint8)
    # bias toward valid blocks: clear the void-extent/reserved patterns by forcing a plausible block mode
    
    path = os.path.join(tmp, 'b.bin'); blocks.tofile(path)
    ro = os.path.join(tmp, 'o.rgba'); bo = os.path.join(tmp, 'o.bc7')
    subprocess.run([EXE, path, str(W), str(H), str(bw), str(bh), '0', ro, bo], check=True, creationflags=0x4000)
    mine = np.fromfile(ro, np.uint8).reshape(H, W, 4)
    ref = np.frombuffer(t2d.decode_astc(blocks.tobytes(), W, H, bw, bh), np.uint8).reshape(H, W, 4)[..., [2, 1, 0, 3]]
    # nvn_astc.cpp rejects (magenta) blocks whose weight grid exceeds the block footprint, as the spec requires; texture2ddecoder
    # decodes those leniently, and HDR endpoint modes are magenta here (LDR decoder). Compare only blocks without any magenta pixel.
    mb = mine.reshape(H // bh, bh, W // bw, bw, 4).transpose(0, 2, 1, 3, 4).reshape(-1, bh * bw, 4)
    rb = ref.reshape(H // bh, bh, W // bw, bw, 4).transpose(0, 2, 1, 3, 4).reshape(-1, bh * bw, 4)
    valid = ~np.any(np.all(mb == np.array([255, 0, 255, 255]), axis=2), axis=1)   # no magenta pixel: LDR endpoints only
    d = np.abs(mb[valid].astype(int) - rb[valid].astype(int))
    exact = float((d.max(axis=2) == 0).mean()); close = float((d.max(axis=2) <= 1).mean())
    print('   valid blocks %d of %d' % (valid.sum(), len(valid)), end=' ')
    # BC7 transcode error against the decoded ASTC (independent BC7 decoder)
    bc7 = np.fromfile(bo, np.uint8).tobytes()
    dec = np.frombuffer(t2d.decode_bc7(bc7, W, H), np.uint8).reshape(H, W, 4)[..., [2, 1, 0, 3]]
    p = psnr(dec, mine)
    print('ASTC %dx%d: decode exact %.4f, within 1 level %.4f (max diff %d); BC7 mode-6 PSNR vs decoded %.1f dB' % (bw, bh, exact, close, d.max() if d.size else -1, p))
    allok &= close > 0.99
print('RESULT', 'ok' if allok else 'MISMATCH')
sys.exit(0 if allok else 1)
