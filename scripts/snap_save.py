#!/usr/bin/env python
"""snap_save.py - inspect and edit a SnapRecomp save directory (New Pokemon Snap). Code only, no game data.

Format (from kwsch/NewSnap, ISC, cloned to tools/ref/NewSnap; the constants are parsed from its SaveReader.cs at run
time so no key material lives in this repo):
  save root:  CURRENT ("SNAPSAVE1\\n<gen>\\n<check>\\n"), gens/<n>/<16 hex file names>, gens/<n>.ok
  save file:  0x30 header (AES-128-CBC, IV masked by an xorshift stream seeded 0xB519FC35 + file index), then
              <entry_count> entries of <enc_size> bytes: 16 byte masked IV + AES-CBC(0x20 meta + data).
  entry meta: adler32, crc32 (both over data[:data_size]), magic 0x9B7A0B7C, data_size, data_size again, 3 unknown words.
  A fully zero first qword marks an empty entry (left alone).

Needs the `cryptography` package: run with `python -E -P scripts/snap_save.py ...` (user-site packages).

  snap_save.py dump SRC [--entry FILE:ENTRY] [--all]
  snap_save.py unlock SRC DST [--profile unlock|unlock_routes|none] [--set FILE:ENTRY:OFFSET:SIZE=VALUE ...]
  snap_save.py patch SRC DST --set FILE:ENTRY:OFFSET:SIZE=VALUE [--set ...]
SRC is a save root (uses CURRENT), a gens/<n> dir, or a dir of save files. DST is a new save root; it must not exist
(never edits in place). Only the edited save files are rewritten; the rest are copied byte for byte.
"""
import argparse, os, re, shutil, struct, sys, zlib

M32 = 0xFFFFFFFF
HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, '..', 'tools', 'ref', 'NewSnap', 'NewSnap.Lib', 'Saves', 'SaveReader.cs')
HDR = 0x30


def load_consts():
    try:
        src = open(REF, encoding='utf-8-sig').read()
    except OSError:
        sys.exit('missing %s: git clone https://github.com/kwsch/NewSnap tools/ref/NewSnap' % os.path.normpath(REF))
    names = re.findall(r'"([0-9a-f]{16})"', src)
    keys = [int(x, 16) for x in re.findall(r'0x(B519FC[0-9A-F]{2})', src)]
    hk = re.search(r'HeaderKey\s*=\s*\{([^}]*)\}', src, re.S).group(1)
    key = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', hk))
    assert len(names) == 16 and len(keys) == 16 and len(key) == 16
    return names, keys, key


NAMES, KEYS, KEY = load_consts()


class XorShift:
    def __init__(s, seed):
        s.w = (0x41C64E6D * seed + 12345) & M32
        s.z = (0x41C64E6D * s.w + 12345) & M32
        s.y = (0x41C64E6D * s.z + 12345) & M32
        s.x = (0x41C64E6D * s.y + 12345) & M32

    def next(s):
        t = (s.x ^ (s.x << 11)) & M32
        sw = s.w - (1 << 32) if s.w & 0x80000000 else s.w
        st = t - (1 << 32) if t & 0x80000000 else t
        s.x, s.y, s.z = s.y, s.z, s.w
        s.w = (s.w ^ ((sw >> 19) & M32) ^ (t ^ ((st >> 8) & M32))) & M32
        return s.w & 0x7FFFFFFF

    def limit(s, lim):  # NewSnap GetNext(limit) for limit < 0x7FFFFFFF
        d = 0x80000000 // (lim + 1)
        while True:
            r = s.next() // d
            if r <= lim:
                return r


def mask_iv(raw16, seed):  # symmetric xor mask
    r = XorShift(seed)
    return bytes(raw16[i] ^ (r.limit(0xFE) + 1) for i in range(16))


def aes(iv, data, decrypt):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    c = Cipher(algorithms.AES(KEY), modes.CBC(iv))
    o = c.decryptor() if decrypt else c.encryptor()
    return o.update(data) + o.finalize()


class SaveFile:
    def __init__(self, path, index):
        self.path, self.index = path, index
        self.f = open(path, 'rb')
        h = self.f.read(HDR)
        hd = aes(mask_iv(h[0x20:], KEYS[index]), h[:0x20], True)
        self.magic, self.enc_size, self.dec_size, self.count, self.size_maybe = struct.unpack('<IiiiI', hd[:20])
        self.raw_header = h

    def raw_entry(self, e):
        self.f.seek(HDR + e * self.enc_size)
        return self.f.read(self.enc_size)

    def read(self, e):
        """None for an empty entry, else (meta dict, bytearray of data_size bytes)."""
        b = self.raw_entry(e)
        if len(b) < self.enc_size or b[:8] == bytes(8):
            return None
        iv_masked = b[:16]
        d = aes(mask_iv(iv_masked, KEYS[self.index] + e), b[16:], True)
        adler, crc, magic, sz, sz2, u1, u2, u3 = struct.unpack('<IIIiiIII', d[:0x20])
        return dict(adler=adler, crc=crc, magic=magic, size=sz, size2=sz2, unk=(u1, u2, u3), iv=iv_masked,
                    pad=d[0x20 + sz:]), bytearray(d[0x20:0x20 + sz])

    @staticmethod
    def checks_ok(meta, data):
        return meta['adler'] == zlib.adler32(bytes(data)) and meta['crc'] == zlib.crc32(bytes(data))

    def build_entry(self, e, meta, data):
        body = struct.pack('<IIIiiIII', zlib.adler32(bytes(data)) & M32, zlib.crc32(bytes(data)) & M32, meta['magic'],
                           len(data), len(data), *meta['unk']) + bytes(data) + meta['pad']
        return meta['iv'] + aes(mask_iv(meta['iv'], KEYS[self.index] + e), body, False)


def resolve_gen(src):
    if os.path.exists(os.path.join(src, 'CURRENT')):
        n = open(os.path.join(src, 'CURRENT')).read().split()[1]
        return os.path.join(src, 'gens', n), int(n)
    base = os.path.basename(os.path.normpath(src))
    return src, int(base) if base.isdigit() else 1


def open_gen(gdir):
    out = {}
    for i, n in enumerate(NAMES):
        p = os.path.join(gdir, n)
        if os.path.exists(p):
            out[i] = SaveFile(p, i)
    return out


def words(b):
    return struct.unpack('<%dI' % (len(b) // 4), bytes(b[:len(b) // 4 * 4]))


def nonzero_bytes(b):
    return [(hex(o), b[o]) for o in range(len(b)) if b[o]]


ALBUM_INUSE = 0x14  # file 6 data byte: 1 = album slot in use, 0 = free (analysis/save/album.md)


def album_slots(sf):
    """(in-use slots, freed slots that still hold data) of the album metadata file 6."""
    inuse, freed = [], []
    for e in range(sf.count):
        r = sf.read(e)
        if r is None or not any(r[1]):
            continue
        (inuse if len(r[1]) > ALBUM_INUSE and r[1][ALBUM_INUSE] else freed).append(e)
    return inuse, freed


def cmd_dump(a):
    gdir, n = resolve_gen(a.src)
    files = open_gen(gdir)
    print('generation', n, gdir)
    for i, sf in files.items():
        scan = sf.enc_size <= 0x4000 or a.all
        used = bad = 0
        if scan:
            for e in range(sf.count):
                r = sf.read(e)
                if r is None:
                    continue
                if any(r[1]):
                    used += 1
                if not sf.checks_ok(*r):
                    bad += 1
        print('file %2d %s size=%d entries=%d enc=%#x data=%#x nonzero_entries=%s bad_checksums=%s' % (
            i, NAMES[i], os.path.getsize(sf.path), sf.count, sf.enc_size, sf.dec_size,
            used if scan else '-', bad if scan else '-'))
    if 6 in files:  # album: file 6 byte 0x14 = slot in use; Cancel Save clears it and leaves the image (a freed slot)
        inuse, freed = album_slots(files[6])
        print('album: %d photos in use (slots %s), %d freed slots with data%s' % (
            len(inuse), inuse, len(freed), ' (slots %s)' % freed if a.freed else ' (--freed lists them)' if freed else ''))
    if a.entry:
        fi, e = [int(x, 0) for x in a.entry.split(':')]
        meta, data = files[fi].read(e)
        print('entry %d:%d size=%d checksums_ok=%s' % (fi, e, len(data), files[fi].checks_ok(meta, data)))
        for o in range(0, len(data), 32):
            if any(data[o:o + 32]) or a.all:
                print('  %04x %s' % (o, bytes(data[o:o + 32]).hex(' ', 4)))
    else:
        dump_fields(files)
        for fi in (1, 12):
            if fi in files:
                meta, data = files[fi].read(0)
                print('file %d words:' % fi, {i: w for i, w in enumerate(words(data)) if w})
        if 9 in files:
            meta, data = files[9].read(0)
            print('file 9 nonzero bytes:', nonzero_bytes(data)[:40])


def load_thresholds():
    """(w1, w2, w3) per route from ResearchPoint.mdrp (read at run time; nothing embedded)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for dp, _, fs in os.walk(os.path.join(root, 'extracted')):
        if 'ResearchPoint.mdrp' in fs:
            b = open(os.path.join(dp, 'ResearchPoint.mdrp'), 'rb').read()
            out = {}
            for i in range(ROUTES):
                p = 28 + 4 * i
                t = p + struct.unpack_from('<I', b, p)[0]
                out[i + 1] = struct.unpack_from('<3I', b, t + 12)
            return out
    return None


def level_of(pts, th):
    w1, w2, w3 = th
    return 4 if pts >= w3 else 3 if pts >= w2 else 2 if pts >= w1 else 1


def dump_fields(files):
    """Pretty-print the identified unlock / level / research fields (see analysis/save/fields.md)."""
    if 9 not in files or 11 not in files:
        return
    st = files[11].read(0)[1]
    rec = files[9].read(0)[1]
    th = load_thresholds()
    print('routes (status: 0 locked, 1 unlocked, 2 entered; level 1-4 from research points):')
    for r in range(ROUTES):
        base = r * REC
        plays = struct.unpack_from('<I', rec, base)[0]
        pts = n = 0
        for k in range(40):
            o = base + ENT0 + 12 * k
            if struct.unpack_from('<H', rec, o)[0] == 0:
                break
            n += 1
            pts += sum(struct.unpack_from('<4H', rec, o + 4))
        base_pts = int.from_bytes(rec[base + 0x1f1:base + 0x1f4], 'little')
        lv = '?' if th is None else level_of(pts, th[r + 1])
        print('  route %2d status=%d plays=%d entries=%d points=%d level=%s found=%#x prev_level=%d baseline=%d%s' % (
            r + 1, st[r], plays, n, pts, lv, struct.unpack_from('<Q', rec, base + 0x1e8)[0], rec[base + 0x1f0],
            base_pts, '' if th is None else ' thresholds=%s' % (th[r + 1],)))


def parse_set(s):
    m = re.fullmatch(r'(\w+):(\w+):(\w+):(\d+)=(\w+)', s)
    if not m:
        sys.exit('bad --set %r (want FILE:ENTRY:OFFSET:SIZE=VALUE)' % s)
    fi, e, off, sz, v = m.groups()
    return int(fi, 0), int(e, 0), int(off, 0), int(sz), int(v, 0)


PROFILES = {}


def profile(name):
    def deco(f):
        PROFILES[name] = f
        return f
    return deco


@profile('none')
def _none(files):
    return []


ROUTES = 30
REC = 0x1f8        # file 9 entry 0: ROUTES route records of 0x1f8 bytes, indexed by route_id-1
ENT0 = 8           # first research entry in a record: u16 id (0 ends the list), u16 skipped, 4 x u16 scores
ENT_ID = 1         # research-entry id used by `unlock` (UNVERIFIED: valid ids were not identified statically)


def route_edits(files, scored=True, entry_id=None):
    """Course unlock (file 11 status byte) and, when `scored`, research points >= every level threshold (file 9)."""
    if 11 not in files or 9 not in files:
        sys.exit('save lacks file 9 / file 11')
    st = files[11].read(0)[1]
    rec = files[9].read(0)[1]
    ed = []
    for r in range(ROUTES):
        if st[r] == 0:
            ed.append((11, 0, r, 1, 1))      # 1 = unlocked, not yet entered; the game promotes it to 2 on first entry
        if not scored:
            continue
        base = r * REC
        if struct.unpack_from('<I', rec, base)[0] == 0:
            ed.append((9, 0, base, 4, 1))    # played / cleared count (0 means never played)
        eid = struct.unpack_from('<H', rec, base + ENT0)[0]
        if eid == 0:
            ed.append((9, 0, base + ENT0, 2, ENT_ID if entry_id is None else entry_id))
        for k in range(4):                  # 4 x 0xFFFF = 262140 >= the largest threshold (200000)
            ed.append((9, 0, base + ENT0 + 4 + 2 * k, 2, 0xFFFF))
    return ed


@profile('unlock')
def _unlock(files):
    return route_edits(files, True)


@profile('unlock_routes')
def _unlock_routes(files):
    return route_edits(files, False)


PROFILES['all'] = _unlock


def apply_edits(files, edits):
    """edits: (file, entry, offset, size, value); returns {file index: {entry: (meta, data)}}"""
    changed = {}
    for fi, e, off, sz, v in edits:
        if fi not in files:
            sys.exit('save has no file %d' % fi)
        ent = changed.setdefault(fi, {})
        if e not in ent:
            r = files[fi].read(e)
            if r is None:
                sys.exit('file %d entry %d is empty' % (fi, e))
            ent[e] = r
        meta, data = ent[e]
        if off + sz > len(data):
            sys.exit('offset %#x+%d beyond entry data (%d)' % (off, sz, len(data)))
        data[off:off + sz] = (v & ((1 << (8 * sz)) - 1)).to_bytes(sz, 'little')
    return changed


def cmd_write(a):
    if os.path.exists(a.dst):
        sys.exit('refusing: DST exists (never edits in place): ' + a.dst)
    gdir, n = resolve_gen(a.src)
    files = open_gen(gdir)
    edits = list(PROFILES[a.profile](files))
    edits += [parse_set(s) for s in (a.set or [])]
    changed = apply_edits(files, edits)
    ngdir = os.path.join(a.dst, 'gens', str(n))
    os.makedirs(ngdir)
    for i, sf in files.items():
        out = os.path.join(ngdir, NAMES[i])
        if i not in changed:
            shutil.copyfile(sf.path, out)
            continue
        with open(out, 'wb') as o:
            o.write(sf.raw_header)
            for e in range(sf.count):
                if e in changed[i]:
                    meta, data = changed[i][e]
                    o.write(sf.build_entry(e, meta, data))
                else:
                    o.write(sf.raw_entry(e))
        chk = SaveFile(out, i)  # self-check: re-read what was just written
        for e, (meta, data) in changed[i].items():
            m2, d2 = chk.read(e)
            assert d2 == data and chk.checks_ok(m2, d2), 'verify failed %d:%d' % (i, e)
    open(ngdir + '.ok', 'w').write('ok\n')
    check = ((n * 2654435761) & M32) ^ 0x534e4150  # Save::check_of in runtime/hle/fs_core.cpp
    open(os.path.join(a.dst, 'CURRENT'), 'w', newline='\n').write('SNAPSAVE1\n%d\n%d\n' % (n, check))
    print('wrote %s (generation %d); edited files: %s; %d edits' % (a.dst, n, sorted(changed), len(edits)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('dump')
    d.add_argument('src'); d.add_argument('--entry'); d.add_argument('--all', action='store_true')
    d.add_argument('--freed', action='store_true', help='also list freed album slots (data left by Cancel Save)')
    u = sub.add_parser('unlock')
    u.add_argument('src'); u.add_argument('dst'); u.add_argument('--profile', default='unlock'); u.add_argument('--set', action='append')
    p = sub.add_parser('patch')
    p.add_argument('src'); p.add_argument('dst'); p.add_argument('--set', action='append', required=True)
    a = ap.parse_args()
    if a.cmd == 'dump':
        cmd_dump(a)
    else:
        if a.cmd == 'patch':
            a.profile = 'none'
        cmd_write(a)


if __name__ == '__main__':
    main()
