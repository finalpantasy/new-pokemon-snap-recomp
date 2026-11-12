"""Build-time icon generator: writes build/snap_icon.ico (+ build/snap_icon.rc) for the exe resource.

  python -I host/make_icon.py [--out build]

Sizes up to SMALL_MAX use a Pikachu head with a camera badge from the game's own Photodex portrait (host/icon_pikachu.py,
decoded at build time, cached in build/icon_src/); without game data they use the original lens tile below.
Source: the game's own icon from the local extracted control data (JPEG, converted with PowerShell System.Drawing),
or, when missing, an original camera-lens icon drawn here (same design as host/icon_fallback.svg).
Env SNAP_ICON_STYLE=r1|r2|r3 (renders of the game's Research Camera model from build/icon_src/camera_r*.png, made by host/render_camera_icon.py; r1 teal lens, r2 red lens cap, r3 front view; also used at 48+ px) or a|b|c|d|d1-d4|e1-e4|f1-f6|g1-g5|h1-h4 picks (h: detailed Research Camera; h1/h2 also supply the 48-256 px sizes, or set SNAP_ICON_LARGE=h1|h2|h3|h4|box to choose them separately; e: head stickers; f: whole-body Pikachu; g: Research Camera) the small-icon design (default a: Pikachu + camera badge; b: peeking over a camera;
c: in viewfinder brackets; d: inside a lens). Env SNAP_ICON_SRC overrides the source path; SNAP_ICON_SRC=none forces the fallback (tests the public-repo path).
Outputs are generated, never committed.
"""
import argparse, math, os, struct, subprocess, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIZES = [16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256]   # in-between sizes are for 125-250% display scaling
SMALL_MAX = 40  # sizes up to this use the simple lens design (title bar, taskbar, small Explorer views)


def png_bytes(w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def shade(x, y):
    """Design in 256-unit space; returns (r,g,b,a) 0-255 for the point."""
    def inrr(px, py, lo, hi, r):
        qx, qy = abs(px - 128) - (128 - lo - r), abs(py - 128) - (128 - lo - r)
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) <= r
    if not inrr(x, y, 8, 248, 48):
        return (0, 0, 0, 0)
    # Bold shapes only, so it stays readable at 16-32 px: warm orange tile, cream lens ring, dark lens.
    t = y / 256.0
    col = (int(0xF7 * (1 - t) + 0xE0 * t), int(0x8A * (1 - t) + 0x4A * t), int(0x2E * (1 - t) + 0x1E * t))
    d = math.hypot(x - 128, y - 128)
    if d <= 92: col = (0xFF, 0xF4, 0xE0)
    if d <= 70: col = (0x1E, 0x2A, 0x3A)
    for k in range(6):  # six thick shutter blades, 60 degrees apart, from r=26 to r=70
        a = -math.pi / 2 + k * math.pi / 3
        along = (x - 128) * math.cos(a) + (y - 128) * math.sin(a)
        perp = abs(-(x - 128) * math.sin(a) + (y - 128) * math.cos(a))
        if 26 <= along <= 70 and perp <= 6 and d <= 70: col = (0x3A, 0x4E, 0x68)
    if d <= 26: col = (0x0B, 0x12, 0x1C)
    if math.hypot(x - 104, y - 104) <= 14:
        col = (0xFF, 0xFF, 0xFF)
    return col + (255,)


def draw_fallback(n):
    ss = 4 if n <= 64 else 2
    out = bytearray()
    for py in range(n):
        for px in range(n):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    r, g, b, a = shade((px + (sx + .5) / ss) * 256 / n, (py + (sy + .5) / ss) * 256 / n)
                    acc[0] += r * a; acc[1] += g * a; acc[2] += b * a; acc[3] += a
            cnt = ss * ss
            a = acc[3]
            out += bytes((acc[0] // a, acc[1] // a, acc[2] // a, a // cnt)) if a else b'\0\0\0\0'
    return png_bytes(n, n, out)


def convert_jpeg(src, tmp):
    tmp.mkdir(parents=True, exist_ok=True)
    ps = ("Add-Type -AssemblyName System.Drawing; $src=[System.Drawing.Image]::FromFile($args[0]); foreach($n in %s){"
          "$b=New-Object System.Drawing.Bitmap $n,$n,([System.Drawing.Imaging.PixelFormat]::Format32bppArgb);"
          "$g=[System.Drawing.Graphics]::FromImage($b);$g.InterpolationMode='HighQualityBicubic';$g.PixelOffsetMode='HighQuality';"
          "$g.DrawImage($src,0,0,$n,$n);$g.Dispose();$b.Save((Join-Path $args[1] ('i'+$n+'.png')),[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose()};$src.Dispose()"
          % ','.join(map(str, SIZES)))
    sc = tmp / 'conv.ps1'
    sc.write_text(ps)
    r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(sc), str(src), str(tmp)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-500:])
    return {n: (tmp / f'i{n}.png').read_bytes() for n in SIZES}


def find_source():
    ov = os.environ.get('SNAP_ICON_SRC')
    if ov:
        return None if ov.lower() == 'none' else Path(ov)
    for v in ('v262144', 'v0'):
        p = ROOT / 'extracted' / v / 'control' / 'part0' / 'icon_AmericanEnglish.dat'
        if p.exists():
            return p
    return None


STYLES = ('a', 'b', 'c', 'd', 'd1', 'd2', 'd3', 'd4', 'e1', 'e2', 'e3', 'e4', 'f1', 'f2', 'f3', 'f4', 'f5', 'f6', 'g1', 'g2', 'g3', 'g4', 'g5', 'h1', 'h2', 'h3', 'h4')
RENDER_STYLES = ('r1p', 'r1', 'r2', 'r3')   # renders of the game's own Research Camera model (host/render_camera_icon.py -> build/icon_src/camera_r*.png)
DEFAULT_STYLE = 'r1p'   # the Research Camera render (best angle, native render per size); falls back to Pikachu when the render is missing
DETAILED = ('h1', 'h2') + RENDER_STYLES   # styles whose detailed master also replaces the box art at 48+ px


def pikachu_small(out, style=None, sizes=None):
    """Small-size icons from host/icon_pikachu.py (needs the extracted game data and numpy/Pillow); {} when unavailable."""
    if os.environ.get('SNAP_ICON_SRC', '').lower() == 'none':
        return {}
    small = sizes or [n for n in SIZES if n <= SMALL_MAX]
    style = (style or os.environ.get('SNAP_ICON_STYLE', DEFAULT_STYLE)).lower()
    have = (out / 'icon_src' / (f'camera_{style}.png' if style != 'r1p' else 'camera_r1p/render_256.png')).exists()
    if style in RENDER_STYLES and not have:
        if sizes:
            return {}   # large sizes: keep the box art
        style = 'a'     # no local render: the existing Pikachu + camera design
    if style in RENDER_STYLES:
        try:
            r = subprocess.run([sys.executable, '-E', '-P', str(Path(__file__).with_name('render_camera_icon.py')), 'size', str(out), style] + list(map(str, small)),
                               capture_output=True, text=True, timeout=120)
            return {} if r.returncode else {n: (out / 'icon_src' / f'small_{style}_{n}.png').read_bytes() for n in small}
        except Exception:
            return {}
    if style not in STYLES:
        style = 'a'
    try:
        r = subprocess.run([sys.executable, '-E', '-P', str(Path(__file__).with_name('icon_pikachu.py')), str(out), style] + list(map(str, small)),
                           capture_output=True, text=True, timeout=300)
        if r.returncode:
            return {}
        return {n: (out / 'icon_src' / f'small_{style}_{n}.png').read_bytes() for n in small}
    except Exception:
        return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build'))
    out = Path(ap.parse_args().out)
    out.mkdir(parents=True, exist_ok=True)
    src = find_source()
    pngs = None
    if src and src.exists():
        try:
            pngs = convert_jpeg(src, out / 'icon_tmp')
            print(f'icon: game icon {src.name}')
        except Exception as e:
            print('icon: conversion failed, using fallback:', e)
    if pngs is None:
        pngs = {n: draw_fallback(n) for n in SIZES}
        print('icon: original fallback')
    else:
        # The game's box-art icon is unreadable at title-bar and taskbar sizes; use Pikachu (or the lens) there.
        small = pikachu_small(out)
        for n in SIZES:
            if n <= SMALL_MAX:
                pngs[n] = small.get(n) or draw_fallback(n)
        print('icon: small sizes', 'Pikachu + camera / render' if small else 'original lens')
    # Opt-in large sizes: SNAP_ICON_LARGE=<style>, or the selected style when it is a detailed one (default: the game's box art).
    sel = os.environ.get('SNAP_ICON_STYLE', DEFAULT_STYLE).lower()
    large = os.environ.get('SNAP_ICON_LARGE', '').lower() or (sel if sel in DETAILED else '')
    if (large in STYLES or large in RENDER_STYLES) and os.environ.get('SNAP_ICON_SRC', '').lower() != 'none':
        big = pikachu_small(out, large, [n for n in SIZES if n > SMALL_MAX])
        for n, data in big.items():
            pngs[n] = data
        print('icon: large sizes', large if big else 'box art (style unavailable)')
    head = struct.pack('<HHH', 0, 1, len(SIZES))
    off = 6 + 16 * len(SIZES)
    ents, data = b'', b''
    for n in SIZES:
        p = pngs[n]
        ents += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(p), off + len(data))
        data += p
    (out / 'snap_icon.ico').write_bytes(head + ents + data)
    (out / 'snap_icon.rc').write_text('1 ICON "snap_icon.ico"\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
