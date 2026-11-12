# SPDX-License-Identifier: MIT
"""Round-trip test for nutexb_encode: decode an original, re-encode its own PNG, decode again (PSNR >= 40 dB), compare header/footer,
then encode a 4x upscale and check the decode. usage: python -E -P scripts\\hd\\test_nutexb_encode.py [orig.nutexb ...]"""
import sys, tempfile, os
from pathlib import Path
import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_cache as bc  # noqa: E402
import nutexb_encode as ne  # noqa: E402
import nutexb_ref as nx  # noqa: E402

PK = bc.PACK / 'Characters' / 'master' / 'CH_001.drp'
DEFAULTS = [PK / 'CH_001_00_0_A00_Body_dif.nutexb', PK / 'CH_001_00_0_A00_Body_nor.nutexb', PK / 'CH_001_00_0_A00_Eye_par.nutexb',
            PK / 'CH_001_00_0_A00_Cheek_dif.nutexb', bc.PACK / 'Other' / 'master' / 'SK_CH_Other_001_00.drp' / 'CH_Other_001_00_BodyA00_nor.nutexb',
            bc.PACK / 'Other' / 'master' / 'SK_CH_Other_001_00.drp' / 'CH_Other_001_00_BodyA00_par.nutexb']


def psnr(a, b, ch):
    d = a[..., :ch].astype(np.float64) - b[..., :ch].astype(np.float64)
    m = (d ** 2).mean()
    return 99.0 if m == 0 else 10 * np.log10(255 ** 2 / m)


def main():
    files = [Path(p) for p in sys.argv[1:]] or DEFAULTS
    ok = True
    tmp = Path(tempfile.mkdtemp(prefix='nutexb_t_'))
    for f in files:
        t0 = nx.parse(str(f))
        px, meta = bc.decode_nutexb(f)
        png = tmp / 'a.png'
        Image.fromarray(px, 'RGBA').save(png)
        out = tmp / 'o.nutexb'
        r = ne.encode(png, f, out)
        t1 = nx.parse(str(out))
        px2, _ = bc.decode_nutexb(out)
        ch = 2 if meta['format'] == 'BC5' else 3 if meta['format'] != 'BC4' else 1
        p = psnr(px, px2, ch)
        raw0, raw1 = f.read_bytes(), out.read_bytes()
        f0, f1 = raw0[-0xb0:], raw1[-0xb0:]
        same_foot = f0 == f1
        same_len = len(raw0) == len(raw1)
        keys = ('w', 'h', 'd', 'fmt', 'dim', 'mips', 'tile', 'layers', 'data_size', 'mip_sizes')
        hdr = all(t0[k] == t1[k] for k in keys)
        # 4x upscale (Lanczos stand-in for the AI output): decode must work and show the same picture
        big = tmp / 'b.png'
        Image.fromarray(px, 'RGBA').resize((px.shape[1] * 4, px.shape[0] * 4), Image.LANCZOS).save(big)
        out4 = tmp / 'o4.nutexb'
        r4 = ne.encode(big, f, out4)
        px4, m4 = bc.decode_nutexb(out4)
        t4 = nx.parse(str(out4))
        lv, ls = nx.layout(t4['w'], t4['h'], 1, *nx.FORMATS[t4['fmt']][:3], t4['mips'], 1)
        size_ok = ls == t4['data_size'] and list(t4['mip_sizes'][:t4['mips']]) == [l[6] for l in lv]
        back = np.asarray(Image.fromarray(px4, 'RGBA').resize((px.shape[1], px.shape[0]), Image.LANCZOS))
        if (px[..., 3] != 255).any():  # compare alpha-weighted so colour under transparent pixels does not count
            wgt = (px[..., 3:4].astype(np.float32) / 255)
            p = psnr((px * wgt).astype(np.uint8), (px2 * wgt).astype(np.uint8), ch)
            p4 = psnr((px * wgt).astype(np.uint8), (back * wgt).astype(np.uint8), ch)
        else:
            p4 = psnr(px, back, ch)
        good = p >= 40 and hdr and same_len and size_ok and p4 >= 30
        ok &= good
        print('%-46s %-5s %4dx%-4d psnr=%.1f dB  hdr=%s foot_identical=%s len=%s | x4: %dx%d mips=%d size_ok=%s psnr_back=%.1f  %s' % (
            f.name, meta['format'], t0['w'], t0['h'], p, hdr, same_foot, same_len, r4['w'], r4['h'], r4['mips'], size_ok, p4, 'PASS' if good else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
