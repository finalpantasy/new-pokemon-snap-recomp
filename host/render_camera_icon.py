# SPDX-License-Identifier: MIT
# Renders the owner's local copy of the game's Research Camera model into icon source PNGs, then copies them to build/icon_src/.
# Code only: no game data lives in this file. Inputs are read from ignored local folders.
#   python -I host/render_camera_icon.py render   # needs Blender; reads analysis/ui/camera_render/ (mesh dump + decoded textures)
#   python -E -P host/render_camera_icon.py size <out_dir> <r1|r2|r3> <px> [<px> ...]  # used by make_icon.py (needs Pillow)
#   python -I host/render_camera_icon.py install  # copies camera_r1/r2/r3.png into build/icon_src/ for SNAP_ICON_STYLE=r1|r2|r3
import sys, shutil, subprocess, os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'analysis' / 'ui' / 'camera_render'
DST = ROOT / 'build' / 'icon_src'
BLENDER = [Path(p) for p in (os.environ.get('BLENDER_EXE', ''), r'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe',
                             r'C:\Program Files\Blender Foundation\Blender 5.1\blender.exe') if p]
SCENE = Path(__file__).with_name('camera_scene_blender.py')


def install():
    DST.mkdir(parents=True, exist_ok=True)
    n = 0
    if (OUT / 'r1p').is_dir():
        shutil.copytree(OUT / 'r1p', DST / 'camera_r1p', dirs_exist_ok=True); n += 1
    for k in ('r1', 'r2', 'r3'):
        s = OUT / f'camera_{k}.png'
        if s.exists():
            shutil.copyfile(s, DST / f'camera_{k}.png'); n += 1
    print('installed', n, 'render(s) into', DST)
    return 0 if n else 3


def render():
    exe = next((b for b in BLENDER if b.exists()), None)
    if not exe:
        print('Blender not found'); return 3
    cmd = ['cmd', '/c', 'start', '/wait', '/belownormal', '/b', str(exe), '--background', '--python', str(SCENE), '--', str(OUT)]
    return subprocess.call(cmd)


def size_r1p(out, sizes):
    """r1p: each size was rendered natively at 4-8x supersampling (camera_scene_blender.py, CAM_SIZES); box-reduce, sharpen per size,
    add a soft anti-aliased shadow plus faint light halo (reads on light and dark taskbars)."""
    from PIL import Image, ImageFilter, ImageChops
    import io
    d = out / 'icon_src' / 'camera_r1p'
    (out / 'icon_src').mkdir(parents=True, exist_ok=True)
    for n in sizes:
        src = Image.open(d / f'render_{n}.png').convert('RGBA')
        ss = src.width // n
        im = src.convert('RGBa').reduce(ss).convert('RGBA')
        pct = 90 if n <= 24 else 70 if n <= 40 else 50 if n <= 72 else 35
        rad = 0.55 if n <= 40 else 0.7 if n <= 72 else 1.0
        rgb = im.convert('RGB').filter(ImageFilter.UnsharpMask(radius=rad, percent=pct, threshold=0))
        im = Image.merge('RGBA', (*rgb.split(), im.getchannel('A')))
        a = im.getchannel('A')
        sg = 0.5 + n / 70.0
        off = max(0.6, n / 56.0)
        sh = ImageChops.offset(a, 0, round(off)).filter(ImageFilter.GaussianBlur(sg)).point(lambda v: int(v * 0.55))
        halo = a.filter(ImageFilter.GaussianBlur(sg * 1.5)).point(lambda v: int(v * 0.3))
        out_im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
        hl = Image.new('RGBA', (n, n), (255, 255, 255, 255)); hl.putalpha(halo); out_im.alpha_composite(hl)
        sd = Image.new('RGBA', (n, n), (10, 14, 26, 255)); sd.putalpha(sh); out_im.alpha_composite(sd)
        out_im.alpha_composite(im)
        b = io.BytesIO(); out_im.save(b, 'PNG')
        (out / 'icon_src' / f'small_r1p_{n}.png').write_bytes(b.getvalue())
    return 0


def size_icons(out, style, sizes):
    """Downscale build/icon_src/camera_<style>.png to each size: tight crop, Lanczos reduction, 1 px dark keyline at 32 px and below."""
    from PIL import Image, ImageFilter, ImageChops
    import io
    src = Image.open(out / 'icon_src' / f'camera_{style}.png').convert('RGBA')
    box = src.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    src = src.crop(box)
    for n in sizes:
        small = n <= 32
        pad = 0.04 if small else 0.07
        w, h = src.size
        side = max(w, h) * (1 + 2 * pad)
        sq = Image.new('RGBA', (round(side), round(side)), (0, 0, 0, 0))
        sq.alpha_composite(src, ((sq.width - w) // 2, (sq.height - h) // 2))
        im = sq.convert('RGBa').resize((n, n), Image.LANCZOS).convert('RGBA')   # premultiplied reduce: no dark fringes
        if small:
            a = im.getchannel('A')
            grow = a.filter(ImageFilter.MaxFilter(3))
            line = Image.new('RGBA', (n, n), (0x12, 0x16, 0x20, 255)); line.putalpha(ImageChops.subtract(grow, a).point(lambda v: min(255, v * 2)))
            ring = Image.new('RGBA', (n, n), (0, 0, 0, 0)); ring.alpha_composite(line); ring.alpha_composite(im); im = ring
        b = io.BytesIO(); im.save(b, 'PNG')
        (out / 'icon_src').mkdir(parents=True, exist_ok=True)
        (out / 'icon_src' / f'small_{style}_{n}.png').write_bytes(b.getvalue())
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 4 and sys.argv[1] == 'size' and sys.argv[3] == 'r1p':
        sys.exit(size_r1p(Path(sys.argv[2]), [int(x) for x in sys.argv[4:]]))
    if len(sys.argv) > 4 and sys.argv[1] == 'size':
        sys.exit(size_icons(Path(sys.argv[2]), sys.argv[3], [int(x) for x in sys.argv[4:]]))
    a = sys.argv[1] if len(sys.argv) > 1 else 'install'
    sys.exit(render() if a == 'render' else install())
