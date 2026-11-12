"""Extract saved photos from a SnapRecomp save into build/gallery/<profile>/ (photos.json + jpgs).

Save layout (analysis/save/album.md): file 7 = 1200 full-size photo entries (0x10 header + JFIF JPEG),
file 10 = 1200 thumbnails, file 6 = 1200 x 0x148 byte metadata records. Entry index links the three.
Metadata field offsets are NOT decoded yet: every record keeps its raw u32 words in `raw_words`, and
META_LAYOUT (field -> (offset, struct fmt)) fills course/species/score/date once confirmed from a real run.
"""
import argparse, json, os, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PHOTO_FILE, THUMB_FILE, META_FILE = 7, 10, 6
META_LAYOUT = {}  # unconfirmed on purpose, e.g. {'score': (0x20, 'I')}


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


def extract(files, out_dir):
    """files: {index: object with .count and .read(e) -> None | (meta, data)}. Returns the photo list."""
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
            rec = {'id': e, 'full': 'full/%04d.jpg' % e, 'bytes': len(jpg)}
            (out / rec['full']).write_bytes(jpg)
            t = tf.read(e) if tf is not None and e < tf.count else None
            tj = find_jpeg(t[1]) if t else None
            if tj:
                rec['thumb'] = 'thumb/%04d.jpg' % e
                (out / rec['thumb']).write_bytes(tj)
            m = mf.read(e) if mf is not None and e < mf.count else None
            if m:
                rec.update(parse_meta(m[1]))
            photos.append(rec)
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
    a = ap.parse_args(argv)
    roots = save_roots(a.profile)
    src = a.save or (str(roots[0]) if roots else None)
    if not src:
        sys.exit('no save found')
    sys.path.insert(0, str(ROOT / 'scripts'))
    import snap_save
    gdir, _ = snap_save.resolve_gen(src)
    out = a.out or ROOT / 'build' / 'gallery' / a.profile
    photos = extract(snap_save.open_gen(gdir), out)
    print('%d photos -> %s' % (len(photos), out))


if __name__ == '__main__':
    main()
