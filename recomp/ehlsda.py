# Project code (ours). Reads a module's .eh_frame_hdr / .eh_frame and the LSDA call-site tables behind it, for the
# exception resume checks of recomp/gen.py (docs/runtime-boundary.md section 2). The parsing follows
# scripts/sdk_boundary.py (same project), but needs only the loaded image, not ELF section headers.
"""FDEs and LSDA call-site tables of one module, with absolute guest addresses (module base added)."""
import struct


def uleb(b, p):
    r = s = 0
    while True:
        c = b[p]
        p += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, p


def sleb(b, p):
    r = s = 0
    while True:
        c = b[p]
        p += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            if c & 0x40:
                r -= 1 << s
            return r, p


def read_enc(img, pos, enc, datarel_base=0):
    """(value, next position). pcrel adds the field's own offset (module offsets: the image is linked at 0)."""
    if enc == 0xFF:
        return None, pos
    fmt, app = enc & 0x0F, enc & 0x70
    at = pos
    if fmt == 0x01:
        v, pos = uleb(img, pos)
    elif fmt == 0x09:
        v, pos = sleb(img, pos)
    elif fmt in (0x02, 0x0A):
        v = struct.unpack_from('<h' if fmt == 0x0A else '<H', img, pos)[0]
        pos += 2
    elif fmt in (0x03, 0x0B):
        v = struct.unpack_from('<i' if fmt == 0x0B else '<I', img, pos)[0]
        pos += 4
    elif fmt in (0x00, 0x04, 0x0C):
        v = struct.unpack_from('<q' if fmt == 0x0C else '<Q', img, pos)[0]
        pos += 8
    else:
        raise ValueError(f'pointer encoding {enc:#x}')
    if app == 0x10:
        v += at
    elif app == 0x30:
        v += datarel_base
    return v, pos


def fdes(img, hdr):
    """[(pc_start, pc_range, fde_offset, lsda_offset or 0)] module offsets, from the .eh_frame_hdr search table."""
    version, ptr_enc, cnt_enc, tbl_enc = img[hdr:hdr + 4]
    pos = hdr + 4
    _, pos = read_enc(img, pos, ptr_enc, hdr)
    count, pos = read_enc(img, pos, cnt_enc, hdr)
    cies, out = {}, []
    for _ in range(count):
        loc, pos = read_enc(img, pos, tbl_enc, hdr)
        fde, pos = read_enc(img, pos, tbl_enc, hdr)
        ln, cid = struct.unpack_from('<II', img, fde)
        cie_off = fde + 4 - cid
        cie = cies.get(cie_off)
        if cie is None:
            p = cie_off + 4 + 4 + 1
            aug = bytes(img[p:img.index(b'\0', p)]).decode()
            p += len(aug) + 1
            _, p = uleb(img, p)
            _, p = sleb(img, p)
            _, p = uleb(img, p)
            cie = {'aug': aug, 'R': 0, 'L': 0xFF}
            if aug.startswith('z'):
                _, p = uleb(img, p)
                for ch in aug[1:]:
                    if ch == 'R':
                        cie['R'] = img[p]
                        p += 1
                    elif ch == 'L':
                        cie['L'] = img[p]
                        p += 1
                    elif ch == 'P':
                        pe = img[p]
                        p += 1
                        _, p = read_enc(img, p, pe)
                    elif ch in 'S':
                        pass
            cies[cie_off] = cie
        p = fde + 8
        pc, p = read_enc(img, p, cie['R'])
        rng, p = read_enc(img, p, cie['R'] & 0x0F)
        lsda = 0
        if cie['aug'].startswith('z'):
            alen, p = uleb(img, p)
            if cie['L'] != 0xFF and 'L' in cie['aug']:
                v, _ = read_enc(img, p, cie['L'] & 0x0F)   # raw value first: zero means no LSDA
                if v:
                    lsda, _ = read_enc(img, p, cie['L'])
        out.append((pc, rng, fde, lsda))
    return out


def lsda_sites(img, lsda, fstart):
    """Call-site table of one LSDA: [(start, length, landing_pad or 0, action)] module offsets."""
    p = lsda
    lpe = img[p]
    p += 1
    lpstart = fstart
    if lpe != 0xFF:
        lpstart, p = read_enc(img, p, lpe)
    tte = img[p]
    p += 1
    if tte != 0xFF:
        _, p = uleb(img, p)
    cse = img[p]
    p += 1
    cslen, p = uleb(img, p)
    end, sites = p + cslen, []
    while p < end:
        s, p = read_enc(img, p, cse & 0x0F)
        n, p = read_enc(img, p, cse & 0x0F)
        lp, p = read_enc(img, p, cse & 0x0F)
        act, p = uleb(img, p)
        sites.append((fstart + s, n, lpstart + lp if lp else 0, act))
    return sites


def module_pads(mod):
    """{function start (absolute): [(site start, length, landing pad absolute, action)]} for every FDE with an LSDA,
    keeping only call sites that have a landing pad."""
    img, base = mod.image, mod.base
    out = {}
    for pc, rng, fde, lsda in fdes(img, mod.nso.eh_start):
        if not lsda:
            continue
        sites = [(base + s, n, base + lp, act) for s, n, lp, act in lsda_sites(img, lsda, pc) if lp]
        if sites:
            out[base + pc] = (rng, sites)
    return out


if __name__ == '__main__':
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from module import Module
    m = Module('x', sys.argv[1], 0, 'c')
    d = module_pads(m)
    pads = {lp for _, (r, s) in d.items() for _, _, lp, _ in s}
    print(f'{len(fdes(m.image, m.nso.eh_start))} FDEs, {len(d)} functions with pads, '
          f'{sum(len(s) for _, (r, s) in d.items())} call sites, {len(pads)} distinct pads')
