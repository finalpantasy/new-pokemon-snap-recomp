"""Extract saved photos from a SnapRecomp save into build/gallery/<profile>/ (photos.json + jpgs).

Save layout (analysis/save/album.md): file 7 = 1200 full-size photo entries (0x10 header + JFIF JPEG),
file 10 = 1200 thumbnails, file 6 = 1200 x 0x148 byte metadata records. Entry index links the three.
Metadata field offsets are NOT decoded yet: every record keeps its raw u32 words in `raw_words`, and
META_LAYOUT (field -> (offset, struct fmt)) fills course/species/score/date once confirmed from a real run.
"""
import argparse, binascii, json, os, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHOTO_FILE, THUMB_FILE, META_FILE = 7, 10, 6
META_LAYOUT = {}  # unconfirmed on purpose, e.g. {'score': (0x20, 'I')}
META_INUSE = 0x14  # file 6 byte: 1 = album slot in use, 0 = freed (Cancel Save leaves the image on disk)


def find_jpeg(data):
    """Return the JPEG bytes inside an entry (SOI .. last EOI) or None."""
    d = bytes(data)
    i = d.find(b'\xff\xd8\xff')
    if i < 0:
        return None
    j = d.rfind(b'\xff\xd9')
    return d[i:j + 2] if j > i else None


def parse_meta(data):
    d = bytes(data)
    rec = {'raw_words': list(struct.unpack('<%dI' % (len(d) // 4), d[:len(d) // 4 * 4]))}
    for name, (off, fmt) in META_LAYOUT.items():
        if off + struct.calcsize(fmt) <= len(d):
            rec[name] = struct.unpack_from('<' + fmt, d, off)[0]
    return rec


def photo_dir():
    """Folder the host writes full-resolution PNG exports to (host/mods/photoexport.cpp, SNAP_PHOTO_DIR overrides)."""
    return Path(os.environ.get('SNAP_PHOTO_DIR') or Path(os.environ.get('APPDATA', '')) / 'SnapRecomp' / 'photos')


def export_date(name):
    """photo_YYYYMMDD_HHMMSS_NNN -> unix-less sortable 'YYYY-MM-DD HH:MM:SS' or None."""
    p = name.split('_')
    if len(p) >= 3 and len(p[1]) == 8 and len(p[2]) == 6 and (p[1] + p[2]).isdigit():
        return '%s-%s-%s %s:%s:%s' % (p[1][:4], p[1][4:6], p[1][6:], p[2][:2], p[2][2:4], p[2][4:])
    return None


def load_exports(pdir):
    """PNG exports with a .json sidecar: [{name, png, width, height, jpeg_crc32, date}]."""
    out = []
    pdir = Path(pdir)
    if not pdir.is_dir():
        return out
    for j in sorted(pdir.glob('photo_*.json')):
        try:
            d = json.loads(j.read_text())
        except (OSError, ValueError):
            continue
        if not (pdir / (j.stem + '.png')).is_file():
            continue
        d['name'] = j.stem
        d['date'] = export_date(j.stem)
        out.append(d)
    return out


def make_thumb(png, dest, width=320):
    """Small JPEG of an exported PNG (needs Pillow; without it the page scales the PNG)."""
    try:
        from PIL import Image
    except ImportError:
        return False
    try:
        if dest.exists() and dest.stat().st_mtime >= png.stat().st_mtime:
            return True
        im = Image.open(png).convert('RGB')
        im.thumbnail((width, width * 10))
        im.save(dest, 'JPEG', quality=85)
        return True
    except OSError:
        return False


def _small(img_path, size=(24, 16)):
    from PIL import Image
    return list(Image.open(img_path).convert('L').resize(size).getdata())


def match_album(x, photos, taken, out, png):
    """The album entry an export came from: same JPEG byte size (the CRC differs because the game edits the JPEG after the
    encoder, so it is not used), confirmed by comparing 24x16 grey thumbnails when Pillow is installed."""
    cand = [p for p in photos if p.get('source') == 'album' and p['id'] not in taken and p['bytes'] == x.get('jpeg_bytes')]
    for p in cand:
        try:
            a, b = _small(png), _small(out / p['full'])
            if sum(abs(i - j) for i, j in zip(a, b)) / len(a) > 12:
                continue
        except (ImportError, OSError):
            pass
        taken.add(p['id'])
        return p
    return None


def extract(files, out_dir, export_dir=None, freed=False):
    """files: {index: object with .count and .read(e) -> None | (meta, data)}. Returns the photo list.
    export_dir: folder of PNG exports; each is joined to its album entry by the CRC32 of the album JPEG.
    freed: also list freed album slots (metadata byte 0x14 == 0), marked 'freed': True; skipped by default."""
    out = Path(out_dir)
    (out / 'full').mkdir(parents=True, exist_ok=True)
    (out / 'thumb').mkdir(exist_ok=True)
    photos = []
    pf, tf, mf = files.get(PHOTO_FILE), files.get(THUMB_FILE), files.get(META_FILE)
    if pf is not None:
        for e in range(pf.count):
            r = pf.read(e)
            jpg = find_jpeg(r[1]) if r else None
            if jpg is None:
                continue
            m = mf.read(e) if mf is not None and e < mf.count else None
            is_freed = m is not None and len(m[1]) > META_INUSE and not m[1][META_INUSE]
            if is_freed and not freed:
                continue
            rec = {'id': e, 'full': 'full/%04d.jpg' % e, 'bytes': len(jpg), 'source': 'album',
                   'crc': '%08x' % (binascii.crc32(jpg) & 0xffffffff)}
            rec['url'] = '/data/' + rec['full']
            (out / rec['full']).write_bytes(jpg)
            t = tf.read(e) if tf is not None and e < tf.count else None
            tj = find_jpeg(t[1]) if t else None
            if tj:
                rec['thumb'] = 'thumb/%04d.jpg' % e
                rec['thumb_url'] = '/data/' + rec['thumb']
                (out / rec['thumb']).write_bytes(tj)
            if m:
                rec.update(parse_meta(m[1]))
            if is_freed:
                rec['freed'] = True
            photos.append(rec)
    if export_dir is not None:
        taken = set()
        (out / 'xthumb').mkdir(exist_ok=True)
        for x in load_exports(export_dir):
            png = Path(export_dir) / (x['name'] + '.png')
            th = '/data/xthumb/%s.jpg' % x['name'] if make_thumb(png, out / 'xthumb' / (x['name'] + '.jpg')) else '/export/%s.png' % x['name']
            p = match_album(x, photos, taken, out, png)
            if p is None:
                p = {'id': 'x_' + x['name'], 'source': 'export', 'thumb_url': th}
                photos.append(p)
            else:
                p['source'] = 'album+export'
                p['thumb_url'] = th
            p.update(url='/export/%s.png' % x['name'], png=x['name'] + '.png', width=x['width'], height=x['height'])
            if x['date']:
                p['taken'] = x['date']
    (out / 'photos.json').write_text(json.dumps({'photos': photos, 'layout_known': bool(META_LAYOUT)}, indent=1))
    return photos


def save_roots(profile='default'):
    base = Path(os.environ.get('APPDATA', '')) / 'SnapRecomp'
    if profile != 'default':
        base = base / 'profiles' / profile
    sd = base / 'save'
    return [p for p in sorted(sd.glob('*')) if p.is_dir()] if sd.is_dir() else []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile', default='default')
    ap.add_argument('--save', help='save root dir (default: first under %%APPDATA%%\\SnapRecomp)')
    ap.add_argument('--out', help='output dir (default build/gallery/<profile>)')
    ap.add_argument('--freed', action='store_true', help='also show freed album slots (photos un-saved with Cancel Save)')
    a = ap.parse_args(argv)
    roots = save_roots(a.profile)
    src = a.save or (str(roots[0]) if roots else None)
    out = a.out or ROOT / 'build' / 'gallery' / a.profile
    files = {}
    if src:
        sys.path.insert(0, str(ROOT / 'scripts'))
        try:
            import snap_save
            gdir, _ = snap_save.resolve_gen(src)
            files = snap_save.open_gen(gdir)
        except (SystemExit, Exception) as e:   # no save yet or no crypto package: still list the PNG exports
            print('album not read:', e)
    else:
        print('no save found; listing PNG exports only')
    photos = extract(files, out, photo_dir(), freed=a.freed)
    print('%d photos -> %s' % (len(photos), out))


if __name__ == '__main__':
    main()
