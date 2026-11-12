# SPDX-License-Identifier: MIT
# Build-time helper for host/make_icon.py: draws the small window/taskbar icons (a Pikachu head with a camera badge).
#   python -E -P host/icon_pikachu.py <out_dir> <a|b|c|d|d1..d4> <size> [<size> ...]
# Source art is the game's own Photodex portrait, read from the user's local extracted data and decoded with the project's
# NUTEXB reference decoder (tests/nvn/nutexb_ref.py). The decoded portrait is cached in <out_dir>/icon_src/ (ignored, never
# committed). Needs numpy, texture2ddecoder and Pillow; exit code 3 means "source missing", 4 means "decoder deps missing".
import sys, os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
ENTRY = 'common/UI/master/Textures/p_seitai.drp/p001_00_1.nutexb'   # P001 = Pikachu, 148x148 BC3 portrait, transparent
HEAD_BOX = (40, 22, 100, 82)                                         # head and ears inside the 148 px portrait
BADGE_MIN = 20                                                       # below this the camera badge is noise, so it is dropped


def decode(src, cache):
    if cache.exists():
        return
    sys.path.insert(0, str(ROOT / 'tests' / 'nvn'))
    import nutexb_ref as r
    from PIL import Image
    t = r.parse(str(src))
    a = r.decode_blocks(r.level_linear(t, 0)[0], t['w'], t['h'], t['fmt'])
    cache.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(a).convert('RGBA').save(cache)


DARK, MID, LIGHT, ORANGE = (0x26, 0x2A, 0x33, 255), (0x5B, 0x6B, 0x82, 255), (255, 255, 255, 255), (0xFF, 0x7A, 0x1A, 255)
STYLES = ('a', 'b', 'c', 'd', 'd1', 'd2', 'd3', 'd4', 'e1', 'e2', 'e3', 'e4', 'f1', 'f2', 'f3', 'f4', 'f5', 'f6', 'g1', 'g2', 'g3', 'g4', 'g5', 'h1', 'h2', 'h3', 'h4')
S = 4


def layer(fn, rot=None, place=None):
    """Vector layer drawn at 4x and reduced to 256 px; fn(draw, rr, ell) gets 256-unit coordinates."""
    from PIL import Image, ImageDraw
    im = Image.new('RGBA', (256 * S, 256 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rr = lambda box, r, **k: d.rounded_rectangle([v * S for v in box], r * S, **k)
    ell = lambda cx, cy, rad, col: d.ellipse([(cx - rad) * S, (cy - rad) * S, (cx + rad) * S, (cy + rad) * S], fill=col)
    fn(d, rr, ell)
    if rot:   # (degrees counter-clockwise, centre x, centre y) in 256-unit space, rotated at 4x before reducing
        im = im.rotate(rot[0], center=(rot[1] * S, rot[2] * S), resample=Image.BICUBIC)
    if place:   # (scale, dx, dy): shrink the whole layer about the canvas centre so a tilted design stays inside the square
        sc, dx, dy = place
        n = round(256 * S * sc)
        im = im.resize((n, n), Image.LANCZOS)
        full = Image.new('RGBA', (256 * S, 256 * S), (0, 0, 0, 0))
        full.alpha_composite(im, ((256 * S - n) // 2 + round(dx * S), (256 * S - n) // 2 + round(dy * S)))
        im = full
    return im.resize((256, 256), Image.LANCZOS)


def head(size, pos, outline=True, round_bottom=True):
    """Pikachu head cropped from the portrait, bottom softened with a round mask (no flat cut), 1 px-ish dark outline."""
    from PIL import Image, ImageDraw, ImageFilter
    src = Image.open(CACHE).convert('RGBA').crop(HEAD_BOX).convert('RGBa').resize((size, size), Image.LANCZOS).convert('RGBA')
    if round_bottom:
        m = Image.new('L', (size * 4, size * 4), 0)
        ImageDraw.Draw(m).rounded_rectangle([0, -size * 4, size * 4 - 1, size * 4 - 1], size * 4 * 0.30, fill=255)
        m = m.resize((size, size), Image.LANCZOS)
        from PIL import ImageChops
        src.putalpha(ImageChops.multiply(src.getchannel('A'), m))
    out = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    out.alpha_composite(src, pos)
    if outline:
        a = out.getchannel('A').point(lambda v: 255 if v > 90 else 0)
        ring = a.filter(ImageFilter.MaxFilter(13)).filter(ImageFilter.GaussianBlur(1.5))
        base = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
        base.paste(Image.new('RGBA', (256, 256), (0x2B, 0x20, 0x12, 255)), (0, 0), ring)
        base.alpha_composite(out)
        return base
    return out


def lens(d, rr, ell, cx, cy, r):
    ell(cx, cy, r, LIGHT); ell(cx, cy, r * .82, MID); ell(cx, cy, r * .58, (0x0E, 0x12, 0x1A, 255))
    ell(cx - r * .3, cy - r * .32, r * .2, LIGHT)


def style_a():
    def badge(d, rr, ell):
        rr((148, 166, 252, 250), 24, fill=LIGHT)                    # white rim keeps it readable on any background
        rr((156, 174, 244, 242), 18, fill=DARK)                     # body
        rr((164, 182, 188, 196), 4, fill=LIGHT)                     # flash
        rr((212, 158, 238, 174), 6, fill=ORANGE)                    # shutter button
        lens(d, rr, ell, 205, 213, 30)
    img = head(236, (6, 0))
    img.alpha_composite(layer(badge))
    return img, head(236, (6, 0))


def style_b():
    def cam(d, rr, ell):
        rr((10, 146, 246, 252), 30, fill=LIGHT)
        rr((18, 154, 238, 244), 24, fill=DARK)
        rr((18, 154, 238, 180), 14, fill=(0xC9, 0xCE, 0xD6, 255))   # silver top plate
        rr((18, 168, 238, 180), 2, fill=(0xC9, 0xCE, 0xD6, 255))
        rr((190, 134, 226, 156), 8, fill=ORANGE)                    # shutter button
        rr((32, 160, 62, 174), 5, fill=LIGHT)                       # flash
        lens(d, rr, ell, 128, 206, 36)
    img = head(190, (33, 0))
    c = layer(cam)
    img.alpha_composite(c)
    return img, img


def style_c():
    from PIL import Image
    def tile(d, rr, ell):
        rr((6, 6, 250, 250), 44, fill=(0xBF, 0xE6, 0xF5, 255), outline=(0x26, 0x2A, 0x33, 255), width=8)
    def brackets(d, rr, ell):
        for sx in (0, 1):
            for sy in (0, 1):
                x0, x1 = (26, 230) if not sx else (230, 26)
                y0, y1 = (26, 230) if not sy else (230, 26)
                ax, ay = (1 if not sx else -1), (1 if not sy else -1)
                d.rectangle([min(x0, x0 + 70 * ax) * S, min(y0, y0 + 24 * ay) * S, max(x0, x0 + 70 * ax) * S, max(y0, y0 + 24 * ay) * S], fill=ORANGE if False else DARK)
                d.rectangle([min(x0, x0 + 24 * ax) * S, min(y0, y0 + 70 * ay) * S, max(x0, x0 + 24 * ax) * S, max(y0, y0 + 70 * ay) * S], fill=DARK)
    img = layer(tile)
    img.alpha_composite(head(168, (44, 48), outline=False))
    img.alpha_composite(layer(brackets))
    return img, img


def style_d():
    from PIL import Image, ImageChops
    def glass(d, rr, ell):
        ell(128, 128, 122, (0x20, 0x24, 0x2C, 255)); ell(128, 128, 112, (0xD5, 0xDA, 0xE2, 255))
        ell(128, 128, 100, (0x8A, 0x93, 0xA0, 255)); ell(128, 128, 94, (0x15, 0x18, 0x1F, 255))
    def inner(d, rr, ell):
        ell(128, 128, 86, (0x8F, 0xC6, 0xE8, 255))
    def glint(d, rr, ell):
        d.arc([60 * S, 60 * S, 196 * S, 196 * S], 200, 250, fill=(255, 255, 255, 230), width=9 * S)
        ell(78, 74, 9, LIGHT)
    img = layer(glass)
    inn = layer(inner)
    h = head(172, (42, 38), outline=False, round_bottom=False)
    inn.alpha_composite(h)
    mask = layer(lambda d, rr, ell: ell(128, 128, 86, (255, 255, 255, 255))).getchannel('A')
    inn.putalpha(ImageChops.multiply(inn.getchannel('A'), mask))
    img.alpha_composite(inn)
    img.alpha_composite(layer(glint))
    return img, img


def lens_style(cam, pop, R, cy, hs, voff=0):
    """Refined D: dark barrel, metal ring, blue coating ring, sky-blue glass with Pikachu; optional camera body and ear pop-out."""
    from PIL import Image, ImageChops
    cx = 128; k = R / 124.0
    def barrel(d, rr, ell):
        ell(cx, cy, R, (0x20, 0x24, 0x2C, 255)); ell(cx, cy, R * .95, (0xE4, 0xE8, 0xEE, 255))
        ell(cx, cy, R * .87, (0x8A, 0x93, 0xA0, 255)); ell(cx, cy, R * .82, (0x15, 0x18, 0x1F, 255))
        ell(cx, cy, R * .78, (0x35, 0x62, 0xA8, 255)); ell(cx, cy, R * .74, (0x15, 0x18, 0x1F, 255))
    def glass(d, rr, ell):
        ell(cx, cy, R * .71, (0x8F, 0xC6, 0xE8, 255))
    def glint(d, rr, ell):
        r = R * .80
        d.arc([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], 292, 338, fill=(255, 255, 255, 235), width=int(7 * k * S))
        ell(cx + r * .52, cy - r * .84, 4.5 * k, LIGHT)
    def body(d, rr, ell):
        rr((6, 30, 250, 248), 36, fill=LIGHT)
        rr((12, 36, 244, 242), 31, fill=DARK)
        rr((16, 36, 240, 60), 14, fill=(0xC9, 0xCE, 0xD6, 255))
        rr((188, 12, 226, 36), 8, fill=ORANGE)           # shutter button
        rr((26, 40, 66, 54), 5, fill=LIGHT)               # flash
    img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    if cam:
        img.alpha_composite(layer(body))
    img.alpha_composite(layer(barrel))
    gl = layer(glass)
    h = head(hs, (int(cx - hs * .5), int(cy - hs * .53 + voff)), outline=pop, round_bottom=True)
    circ = gl.getchannel('A')
    if pop:   # glass circle plus everything above the rim line, so the ear tips draw over the barrel
        from PIL import ImageDraw
        top = Image.new('L', (256, 256), 0)
        ImageDraw.Draw(top).rectangle([0, 0, 255, int(cy - R * .30)], fill=255)
        mask = ImageChops.lighter(circ, top)
    else:
        mask = circ
    h.putalpha(ImageChops.multiply(h.getchannel('A'), mask))
    gl.alpha_composite(h)
    img.alpha_composite(gl)
    img.alpha_composite(layer(glint))
    return img, img


def style_d1(): return lens_style(False, False, 124, 128, 122, voff=4)
def style_d2(): return lens_style(False, True, 124, 134, 150, voff=-34)
def style_d3(): return lens_style(True, False, 100, 154, 100, voff=3)
def style_d4(): return lens_style(True, True, 100, 156, 140, voff=-40)


CUT_BOX = (38, 20, 104, 90)   # source px around ears, face and cheeks (the portrait itself is a complete, uncropped figure)


def cutout():
    """Head-only cutout of the portrait: both ears, face, cheeks and a rounded chin; arms, body and tail are masked away."""
    from PIL import Image, ImageDraw, ImageChops, ImageFilter
    im = Image.open(CACHE).convert('RGBA')
    K = 8
    m = Image.new('L', (im.width * K, im.height * K), 0)
    d = ImageDraw.Draw(m)
    sc = lambda pts: [(x * K, y * K) for x, y in pts]
    d.rectangle([40 * K, 22 * K, 99 * K, 49 * K], fill=255)                  # ears and top of head
    d.ellipse([44 * K, 41 * K, 88 * K, 85 * K], fill=255)                    # face, cheeks, round chin
    m = m.filter(ImageFilter.GaussianBlur(K * 0.6)).resize(im.size, Image.LANCZOS)
    im.putalpha(ImageChops.multiply(im.getchannel('A'), m))
    return im.crop(CUT_BOX)


def sticker(h, dark=8, white=10):
    """Head cutout scaled to h px tall with a dark outline and a white sticker outline; returns an RGBA tile with margin."""
    from PIL import Image, ImageFilter
    c = cutout()
    w = max(1, round(c.width * h / c.height))
    c = c.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')
    pad = dark + white + 6
    t = Image.new('RGBA', (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    t.alpha_composite(c, (pad, pad))
    a = t.getchannel('A').point(lambda v: 255 if v > 70 else 0)
    def ring(n):   # round (disk) dilation so ear tips stay round, not squared off
        from PIL import ImageChops
        acc = a
        for dy in range(-n, n + 1):
            for dx in range(-n, n + 1):
                if (dx or dy) and dx * dx + dy * dy <= n * n + n:
                    acc = ImageChops.lighter(acc, ImageChops.offset(a, dx, dy))
        return acc.filter(ImageFilter.GaussianBlur(1.0))
    out = Image.new('RGBA', t.size, (0, 0, 0, 0))
    out.paste(Image.new('RGBA', t.size, (255, 255, 255, 255)), (0, 0), ring(dark + white))
    out.paste(Image.new('RGBA', t.size, (0x2B, 0x20, 0x12, 255)), (0, 0), ring(dark))
    out.alpha_composite(t)
    return out


def place(img, st, cx, cy):
    img.alpha_composite(st, (round(cx - st.width / 2), round(cy - st.height / 2)))


def fit_sticker(maxr, cx_off=0, cy_off=0, dark=8, white=10):
    """Largest sticker whose farthest opaque pixel stays within maxr of the glass centre (head centred on its bbox + offset)."""
    lo, hi = 40, 200
    while hi - lo > 1:
        mid = (lo + hi) // 2
        st = sticker(mid, dark, white)
        bb = st.getchannel('A').point(lambda v: 255 if v > 20 else 0).getbbox()
        cx, cy = st.width / 2 + cx_off, st.height / 2 + cy_off
        al = st.getchannel('A'); px = al.load(); far = 0
        for y in range(0, st.height, 2):
            for x in range(0, st.width, 2):
                if px[x, y] > 20:
                    far = max(far, ((x - cx) ** 2 + (y - cy) ** 2) ** .5)
        lo, hi = (mid, hi) if far <= maxr else (lo, mid)
    return sticker(lo, dark, white)


def sticker_style(kind):
    from PIL import Image
    cam = kind in ('e3', 'e4')
    R, cy = (100, 154) if cam else (124, 128)
    cx = 128; k = R / 124.0
    def barrel(d, rr, ell):
        ell(cx, cy, R, (0x20, 0x24, 0x2C, 255)); ell(cx, cy, R * .95, (0xE4, 0xE8, 0xEE, 255))
        ell(cx, cy, R * .87, (0x8A, 0x93, 0xA0, 255)); ell(cx, cy, R * .82, (0x15, 0x18, 0x1F, 255))
        ell(cx, cy, R * .78, (0x35, 0x62, 0xA8, 255)); ell(cx, cy, R * .74, (0x15, 0x18, 0x1F, 255))
    def glass(d, rr, ell): ell(cx, cy, R * .71, (0x8F, 0xC6, 0xE8, 255))
    def glint(d, rr, ell):
        r = R * .80
        d.arc([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], 292, 338, fill=(255, 255, 255, 235), width=int(7 * k * S))
    def body(d, rr, ell):
        rr((6, 30, 250, 248), 36, fill=LIGHT); rr((12, 36, 244, 242), 31, fill=DARK)
        rr((16, 36, 240, 60), 14, fill=(0xC9, 0xCE, 0xD6, 255))
        rr((188, 12, 226, 36), 8, fill=ORANGE); rr((26, 40, 66, 54), 5, fill=LIGHT)
    img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    if cam: img.alpha_composite(layer(body))
    img.alpha_composite(layer(barrel)); img.alpha_composite(layer(glass))
    if kind == 'e1':
        st = fit_sticker(R * .71 * .93)           # whole sticker stays inside the glass with margin
        place(img, st, cx, cy + 2)
        img.alpha_composite(layer(glint))
    else:
        img.alpha_composite(layer(glint)) if kind == 'e2' else None
        if kind == 'e2': st = sticker(150); place(img, st, cx, cy + 4)
        elif kind == 'e3': st = sticker(104); place(img, st, cx, cy - 14)
        else: st = sticker(124, 9, 11); place(img, st, cx, cy - 22)
    return img, img


def style_e1(): return sticker_style('e1')
def style_e2(): return sticker_style('e2')
def style_e3(): return sticker_style('e3')
def style_e4(): return sticker_style('e4')


FIG_BOX = (42, 25, 122, 122)   # whole figure: the portrait's opaque bbox is (43,26)-(121,121), nothing masked off, tail included


def figure_sticker(h, dark=6, white=9):
    """Complete Pikachu (tail included) scaled to h px tall, with a dark outline and a white sticker outline."""
    from PIL import Image, ImageFilter, ImageChops
    c = Image.open(CACHE).convert('RGBA').crop(FIG_BOX)
    w = max(1, round(c.width * h / c.height))
    c = c.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')
    pad = dark + white + 4
    t = Image.new('RGBA', (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    t.alpha_composite(c, (pad, pad))
    a = t.getchannel('A').point(lambda v: 255 if v > 60 else 0)
    def ring(n):
        acc = a
        for dy in range(-n, n + 1):
            for dx in range(-n, n + 1):
                if (dx or dy) and dx * dx + dy * dy <= n * n + n:
                    acc = ImageChops.lighter(acc, ImageChops.offset(a, dx, dy))
        return acc.filter(ImageFilter.GaussianBlur(0.9))
    out = Image.new('RGBA', t.size, (0, 0, 0, 0))
    out.paste(Image.new('RGBA', t.size, (255, 255, 255, 255)), (0, 0), ring(dark + white))
    out.paste(Image.new('RGBA', t.size, (0x2B, 0x20, 0x12, 255)), (0, 0), ring(dark))
    out.alpha_composite(t)
    return out, pad


def camera(x0, y0, x1, y1, plate=True):
    """Compact camera in 256-unit space: white rim, dark body, silver plate, flash, orange shutter, lens."""
    w, h = x1 - x0, y1 - y0
    def fn(d, rr, ell):
        rr((x0, y0, x1, y1), h * .26, fill=LIGHT)
        rr((x0 + 5, y0 + 5, x1 - 5, y1 - 5), h * .22, fill=DARK)
        if plate:
            rr((x0 + 5, y0 + 5, x1 - 5, y0 + h * .3), h * .14, fill=(0xC9, 0xCE, 0xD6, 255))
        rr((x1 - w * .30, y0 - h * .10, x1 - w * .10, y0 + h * .16), h * .07, fill=ORANGE)    # shutter button
        rr((x0 + w * .10, y0 + h * .12, x0 + w * .30, y0 + h * .24), 3, fill=LIGHT)            # flash
        r = h * .31; cx, cy = x0 + w * .5, y0 + h * .62
        ell(cx, cy, r, LIGHT); ell(cx, cy, r * .80, (0x5B, 0x6B, 0x82, 255)); ell(cx, cy, r * .56, (0x0E, 0x12, 0x1A, 255))
        ell(cx - r * .28, cy - r * .30, r * .17, LIGHT)
    return layer(fn)


def full_style(kind):
    from PIL import Image
    img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    if kind == 'f1':      # Pikachu large and in front, small camera beside him at the lower right
        img.alpha_composite(camera(150, 168, 250, 248))
        st, pad = figure_sticker(200); img.alpha_composite(st, (2, 128 - st.height // 2))
    elif kind == 'f2':    # standing on top of the camera body, feet on the top plate
        img.alpha_composite(camera(8, 168, 248, 250))
        st, pad = figure_sticker(150, 5, 8)
        img.alpha_composite(st, (round(128 - st.width / 2), 180 + pad - st.height))
    elif kind == 'f3':    # sky-blue rounded tile, camera badge in the lower-right corner
        def tile(d, rr, ell): rr((6, 6, 250, 250), 46, fill=(0xBF, 0xE6, 0xF5, 255), outline=(0x26, 0x2A, 0x33, 255), width=8)
        img.alpha_composite(layer(tile))
        st, pad = figure_sticker(198, 5, 7); place(img, st, 112, 126)
        img.alpha_composite(camera(168, 178, 244, 242, plate=False))
    else:                 # f4: biggest figure, camera held in front of the belly
        st, pad = figure_sticker(210, 6, 9); img.alpha_composite(st, (2, 128 - st.height // 2))
        img.alpha_composite(camera(112, 166, 238, 248))
    return img, img


REFS = ROOT / 'analysis' / 'ui' / 'ingame_refs'    # the game's own UI glyphs, decoded locally (ignored, never committed)


def outlined(im, dark, white):
    from PIL import Image, ImageFilter, ImageChops
    pad = dark + white + 3
    t = Image.new('RGBA', (im.width + 2 * pad, im.height + 2 * pad), (0, 0, 0, 0)); t.alpha_composite(im, (pad, pad))
    a = t.getchannel('A').point(lambda v: 255 if v > 60 else 0)
    def ring(n):
        acc = a
        for dy in range(-n, n + 1):
            for dx in range(-n, n + 1):
                if (dx or dy) and dx * dx + dy * dy <= n * n + n:
                    acc = ImageChops.lighter(acc, ImageChops.offset(a, dx, dy))
        return acc.filter(ImageFilter.GaussianBlur(0.9))
    out = Image.new('RGBA', t.size, (0, 0, 0, 0))
    out.paste(Image.new('RGBA', t.size, (255, 255, 255, 255)), (0, 0), ring(dark + white))
    if dark:
        out.paste(Image.new('RGBA', t.size, (0x2B, 0x20, 0x12, 255)), (0, 0), ring(dark))
    out.alpha_composite(t)
    return out


def game_camera(w):
    """The game's flat camera glyph (main_menu 00015) recolored: dark body, white lens ring, orange flash dot, white sticker outline."""
    from PIL import Image, ImageDraw
    src = Image.open(REFS / 'main_menu_00015.png').convert('RGBA')
    bb = src.getchannel('A').getbbox()
    px = src.load()
    for y in range(src.height):
        for x in range(src.width):
            r, g, b, al = px[x, y]
            if al:
                px[x, y] = (0x26, 0x2A, 0x33, al) if (r + g + b) / 3 < 100 else (255, 255, 255, al)
    ImageDraw.Draw(src).ellipse([66, 26, 71, 31], fill=ORANGE)                  # flash dot
    src = src.crop(bb)
    h = round(src.height * w / src.width)
    src = src.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')
    return outlined(src, 0, 7)


def game_emblem(d):
    """The game's Photodex emblem (zukan_index 00040), tinted sky blue so Pikachu and the white sticker outline stand out."""
    from PIL import Image
    src = Image.open(REFS / 'zukan_index_00040.png').convert('RGBA')
    bb = src.getchannel('A').getbbox(); px = src.load()
    for y in range(src.height):
        for x in range(src.width):
            r, g, b, al = px[x, y]
            if al:
                px[x, y] = (0xBF, 0xE6, 0xF5, al) if (r + g + b) / 3 > 190 else (0x6F, 0xA8, 0xC9, al)
    src = src.crop(bb).convert('RGBa').resize((d, d), Image.LANCZOS).convert('RGBA')
    return outlined(src, 5, 0)


def game_style(kind):
    from PIL import Image
    img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    if kind == 'f5':      # full body Pikachu with the game's camera glyph in front at the lower right
        cam = game_camera(112); img.alpha_composite(cam, (254 - cam.width, 254 - cam.height))
        st, pad = figure_sticker(200); img.alpha_composite(st, (2, 128 - st.height // 2))
        img.alpha_composite(cam, (254 - cam.width, 254 - cam.height))
    else:                 # f6: Pikachu inside the game's Photodex emblem
        em = game_emblem(246); img.alpha_composite(em, (5, 5))
        h = 176
        while True:
            st, pad = figure_sticker(h, 5, 7)
            al = st.getchannel('A').load(); far = 0
            ox, oy = 128 - st.width // 2 - 4, 128 - st.height // 2 + 2
            for y in range(0, st.height, 2):
                for x in range(0, st.width, 2):
                    if al[x, y] > 20: far = max(far, ((x + ox - 128) ** 2 + (y + oy - 128) ** 2) ** .5)
            if far <= 112 or h < 100: break
            h -= 4
        img.alpha_composite(st, (ox, oy))
    return img, img


def style_f5(): return game_style('f5')
def style_f6(): return game_style('f6')


def style_f1(): return full_style('f1')
def style_f2(): return full_style('f2')
def style_f3(): return full_style('f3')
def style_f4(): return full_style('f4')


RED, TEAL, CYAN, LIME, CHAR = (0xE2, 0x36, 0x2F, 255), (0x2B, 0xB0, 0xA2, 255), (0x6B, 0xE6, 0xEA, 255), (0x9A, 0xE6, 0x3C, 255), (0x2A, 0x2D, 0x33, 255)


def research_camera(cx=128, cy=128, k=1.0, tilt=7, lens_k=1.0, cap=False):
    """Original flat-vector Research Camera: red grip with a white lotus, charcoal body, teal screen, big lens (teal glass or red cap).
    Returns (layer, (lens_x, lens_y, glass_radius)) in 256-unit space."""
    import math
    P = lambda x, y: (cx + (x - 128) * k, cy + (y - 128) * k)
    LX, LY, LR = 176, 138, 66 * lens_k
    def fn(d, rr, ell):
        def R(box, r, **kw):
            x0, y0 = P(box[0], box[1]); x1, y1 = P(box[2], box[3]); rr((x0, y0, x1, y1), r * k, **kw)
        def E(x, y, rad, col):
            px, py = P(x, y); ell(px, py, rad * k, col)
        R((12, 56, 244, 200), 26, fill=(255, 255, 255, 255))                 # thin white keyline keeps it readable on dark taskbars
        R((16, 60, 240, 196), 22, fill=CHAR)
        R((16, 60, 96, 196), 22, fill=RED)                                    # grip
        R((90, 64, 96, 192), 2, fill=(0xFF, 0xB0, 0x30, 255))               # thin yellow-orange edge stripe
        for i, ang in enumerate((-40, 0, 40)):                                # white lotus hint: three petals
            a = math.radians(ang)
            E(52 + 11 * math.sin(a), 118 - 9 * math.cos(a) + (4 if ang else 0), 9 if ang == 0 else 7, (255, 255, 255, 255))
        R((100, 76, 214, 160), 10, fill=TEAL)                                # screen
        R((106, 82, 160, 90), 3, fill=CYAN)
        R((104, 66, 196, 71), 2, fill=CYAN)                                   # cyan stripe along the top
        E(226, 74, 7, LIME)                                                   # status light
        for x in (110, 128, 146): E(x, 184, 6, TEAL)                          # three small circles
        E(LX, LY, LR, (0x14, 0x16, 0x1A, 255))                                # thick dark barrel
        E(LX, LY, LR * .92, (0x3A, 0x3F, 0x47, 255))
        if cap:
            E(LX, LY, LR * .78, RED)                                          # red lens cap
            px, py = P(LX, LY)
            rr((px - LR * .78 * k, py - 2.2 * k, px + LR * .78 * k, py + 2.2 * k), 1, fill=(0x7A, 0x14, 0x12, 255))   # seam
            E(LX, LY, 6.5, (0x7A, 0x14, 0x12, 255)); E(LX, LY, 3.5, (255, 255, 255, 255))                             # centre notch
            ex, ey = P(LX - LR * .35, LY - LR * .45); ell(ex, ey, 5 * k, (255, 150, 140, 255))
        else:
            E(LX, LY, LR * .78, (0x1B, 0x7F, 0x74, 255)); E(LX, LY, LR * .56, TEAL)
            E(LX - LR * .28, LY - LR * .30, LR * .13, (255, 255, 255, 255))
    lx, ly = P(LX, LY)
    t = math.radians(tilt); dx, dy = lx - cx, ly - cy
    rx, ry = cx + dx * math.cos(t) + dy * math.sin(t), cy - dx * math.sin(t) + dy * math.cos(t)
    return layer(fn, (tilt, cx, cy)), (rx, ry, LR * .78 * k)


def g_style(kind):
    from PIL import Image
    img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    if kind == 'g1':
        img.alpha_composite(research_camera(128, 128, 1.0)[0])
    elif kind == 'g5':
        img.alpha_composite(research_camera(128, 128, 1.0, cap=True)[0])
    elif kind == 'g2':      # Pikachu peeking up from behind the camera
        st, pad = figure_sticker(128, 5, 8)
        img.alpha_composite(st, (round(128 - st.width / 2), 176 - st.height))
        img.alpha_composite(research_camera(128, 184, 0.80, tilt=5)[0])
    elif kind == 'g3':      # Pikachu seen through the lens glass
        cam, (lx, ly, gr) = research_camera(122, 128, 0.96, lens_k=1.12)
        img.alpha_composite(cam)
        h = int(gr * 1.7)
        while True:
            st, pad = figure_sticker(h, 3, 3)
            al = st.getchannel('A').load(); far = 0
            ox, oy = round(lx - st.width / 2), round(ly - st.height / 2)
            for y in range(0, st.height, 2):
                for x in range(0, st.width, 2):
                    if al[x, y] > 20: far = max(far, ((x + ox - lx) ** 2 + (y + oy - ly) ** 2) ** .5)
            if far <= gr * .92 or h < 30: break
            h -= 3
        img.alpha_composite(st, (ox, oy))
    else:                   # g4: Pikachu in front at the left, camera lower right with its lens clear of him, red cap
        img.alpha_composite(research_camera(150, 166, 0.86, tilt=6, cap=True)[0])
        st, pad = figure_sticker(190, 6, 9); img.alpha_composite(st, (2, 252 - st.height))
    return img, img


def style_g1(): return g_style('g1')
def style_g2(): return g_style('g2')
def style_g3(): return g_style('g3')
def style_g4(): return g_style('g4')
def style_g5(): return g_style('g5')


def petal(d, cx, cy, w, h, ang, col):
    """Pointed lotus petal: base at (cx, cy), tip rotated ang degrees from straight up."""
    import math
    pts = []
    for i in range(0, 21):
        u = i / 20.0
        hw = w / 2 * math.sin(math.pi * u) ** 0.8 * (0.55 + 0.45 * u)
        pts.append((-hw, -u * h))
    pts = pts[:] + [(-x, y) for x, y in reversed(pts)]
    a = math.radians(ang)
    out = [((cx + x * math.cos(a) - y * math.sin(a)) * S, (cy + x * math.sin(a) + y * math.cos(a)) * S) for x, y in pts]
    d.polygon(out, fill=col)


def rc_device(detail=True, cap=False, small=False):
    """The Research Camera, flat vector in upright 256-unit space (tilted afterwards): red grip bulge with lotus and speaker dots,
    charcoal body, teal screen, three ring buttons, green light, big overlapping lens (thick barrel, thin red accent ring)."""
    WHITE = (255, 255, 255, 255)
    def fn(d, rr, ell):
        def arcw(cx, cy, r, a0, a1, w, col): d.arc([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], a0, a1, fill=col, width=int(w * S))
        LX, LY = (176, 140) if small else (171, 142)
        # body
        rr((58, 70, 246, 178), 16, fill=(0x2B, 0x2E, 0x34, 255))
        if detail:
            rr((58, 70, 246, 178), 16, outline=(0x4A, 0x4E, 0x57, 255), width=2 * S)                 # bevel
            rr((66, 160, 240, 176), 6, fill=(0x3A, 0x3D, 0x45, 255))                               # strip under the screen
            rr((104, 75, 236, 79), 2, fill=CYAN)                                                    # thin cyan top stripe
            rr((216, 63, 238, 70), 2, fill=(0x5A, 0x5E, 0x64, 255))
        else:
            rr((100, 74, 238, 81), 3, fill=CYAN)
        # red cylinder of the lens barrel behind the front face (the red accent that shows above and to the right)
        k4 = 4 if small else 0
        ell(LX + 13, LY - 15, 51 + k4, (0xB0, 0x38, 0x33, 255)); ell(LX + 12, LY - 14, 49 + k4, (0xE2, 0x68, 0x61, 255))
        # screen
        rr((66, 84, 214, 160), 9, fill=(0x6B, 0xE6, 0xEA, 255))
        rr((69, 87, 211, 157), 7, fill=(0x1C, 0x9E, 0x99, 255))
        rr((69, 87, 211, 118), 7, fill=(0x14, 0x88, 0x8C, 255))
        if detail:
            ell(103, 125, 15, (0x57, 0xD8, 0xCB, 255)); ell(103, 125, 13.2, (0x1C, 0x9E, 0x99, 255)); ell(103, 126, 6, (0xBF, 0xF3, 0xEE, 255))
            ell(79, 96, 5, (0x9D, 0xEE, 0xEA, 255)); rr((88, 92, 118, 97), 2, fill=(0x9D, 0xEE, 0xEA, 255))
            rr((78, 146, 118, 149), 1, fill=(0x8D, 0xE6, 0xE1, 255)); rr((82, 152, 114, 155), 1, fill=(0x8D, 0xE6, 0xE1, 255))
        else:
            ell(100, 124, 17, (0x57, 0xD8, 0xCB, 255)); ell(100, 124, 11, (0x1C, 0x9E, 0x99, 255))
        # grip
        rr((12, 56, 74, 200), 26, fill=(0xD6, 0x2F, 0x2B, 255))
        rr((60, 66, 100, 80), 5, fill=(0xD6, 0x2F, 0x2B, 255))
        rr((14, 58, 72, 92), 24, fill=(0xE8, 0x48, 0x40, 255))                                    # lighter top face of the grip
        rr((12, 84, 74, 200), 26, fill=(0xD6, 0x2F, 0x2B, 255))
        rr((65, 90, 74, 190), 4, fill=(0xA4, 0x1F, 0x1D, 255))                                    # shaded right edge
        rr((18, 72, 28, 182), 5, fill=(0xF4, 0x6A, 0x62, 255))                                     # glossy left highlight
        if detail:
            rr((18, 72, 24, 100), 3, fill=(0xFF, 0xA6, 0xA0, 255))
            d.line([(x * S, y * S) for x, y in ((14, 94), (24, 90), (44, 84), (72, 82))], fill=LIME, width=int(2.2 * S))
            ell(31, 66, 10, CYAN); ell(31, 66, 8.2, (0x1D, 0x20, 0x26, 255)); ell(31, 66, 5.5, (0x30, 0x34, 0x3B, 255))
            # lotus: three petals, two angled petals and two wide side leaves, plus a small flame dot
            for w_, h_, a_ in ((6, 15, -98), (6, 15, 98), (7, 17, -66), (7, 17, 66), (8, 19, -34), (8, 19, 34), (9, 22, 0)):
                petal(d, 43, 148, w_, h_, a_, WHITE)
            ell(43, 134, 2.2, (0xD6, 0x2F, 0x2B, 255))
            for (x, y) in ((36, 168), (46, 168), (31, 176), (41, 176), (51, 176)): ell(x, y, 1.9, (0x8E, 0x1D, 0x1B, 255))
        else:
            petal(d, 40, 142, 9, 19, 0, WHITE); petal(d, 40, 143, 8, 16, -42, WHITE); petal(d, 40, 143, 8, 16, 42, WHITE)
        if not small:
            for x, c in ((84, (0x35, 0xD6, 0xC2, 255)), (98, (0x35, 0xD6, 0xC2, 255)), (112, (0x4D, 0xA8, 0xE8, 255))):
                ell(x, 168, 5.5, c); ell(x, 168, 3, (0x1E, 0x23, 0x28, 255))
        # green status light
        gr = 9 if small else 8
        ell(230, 82, gr, (0x3E, 0x5A, 0x1C, 255)); ell(230, 82, gr - 1.8, (0x8C, 0xE0, 0x30, 255)); ell(228, 80, 2, (0xE4, 0xFF, 0xB0, 255))
        # lens
        R = 58 if small else 56
        ell(LX, LY, R, (0x6A, 0x6E, 0x74, 255)); ell(LX, LY, R * .92, (0x4A, 0x4D, 0x53, 255)); ell(LX, LY, R * .84, (0x23, 0x25, 0x2A, 255))
        if cap:
            ell(LX, LY, R * .80, (0xE0, 0x4B, 0x43, 255)); ell(LX, LY, R * .76, (0x15, 0x17, 0x1B, 255)); ell(LX, LY, R * .72, (0xDB, 0x3A, 0x33, 255))
            ell(LX, LY, R * .60, (0xE8, 0x4A, 0x42, 255))
            rr((LX - R * .72, LY - 2, LX + R * .72, LY + 2), 1, fill=(0x7A, 0x14, 0x12, 255))
            ell(LX, LY, 6.5, (0x7A, 0x14, 0x12, 255)); ell(LX, LY, 3.6, WHITE)
            arcw(LX, LY, R * .55, 200, 248, 3, (255, 190, 184, 220))
        else:
            ell(LX, LY, R * .78, (0xE0, 0x4B, 0x43, 255)); ell(LX, LY, R * .74, (0x15, 0x17, 0x1B, 255))      # thin red accent ring + dark inner ring
            ell(LX, LY, R * .70, (0x10, 0x72, 0x6C, 255)); ell(LX + 3, LY + 8, R * .52, (0x1E, 0x8C, 0x85, 255)); ell(LX + 8, LY + 15, R * .30, (0x2F, 0x9E, 0x98, 255))
            arcw(LX, LY, R * .56, 196, 252, 3.2, (255, 255, 255, 215)); ell(LX - R * .38, LY - R * .30, 2.2, WHITE)
    return fn


def research_h(cap=False, small=False, scale=0.88, dx=0, dy=0, shadow=True):
    from PIL import Image, ImageFilter, ImageChops
    dev = layer(rc_device(detail=not small, cap=cap, small=small), (8, 128, 128), (scale, dx, dy))   # rotate 8 degrees counter-clockwise
    dev = dev.resize((256, 288), Image.LANCZOS).crop((0, 16, 256, 272))   # 12% taller: the reference's lens and grip read slightly oval
    out = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    a = dev.getchannel('A')
    if small:      # white keyline for dark taskbars
        acc = a
        for ddy in range(-5, 6):
            for ddx in range(-5, 6):
                if ddx * ddx + ddy * ddy <= 30: acc = ImageChops.lighter(acc, ImageChops.offset(a, ddx, ddy))
        out.paste(Image.new('RGBA', (256, 256), (255, 255, 255, 255)), (0, 0), acc.filter(ImageFilter.GaussianBlur(0.8)))
    elif shadow:   # soft drop shadow
        sh = ImageChops.offset(a, 3, 6).filter(ImageFilter.GaussianBlur(5)).point(lambda v: int(v * 0.42))
        out.paste(Image.new('RGBA', (256, 256), (0, 0, 0, 255)), (0, 0), sh)
    out.alpha_composite(dev)
    return out


def h_style(kind):
    if kind == 'h1': img = research_h()
    elif kind == 'h2': img = research_h(cap=True)
    elif kind == 'h3': img = research_h(small=True, scale=0.94)
    else:
        from PIL import Image
        img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
        st, pad = figure_sticker(124, 5, 8)
        img.alpha_composite(st, (round(128 - st.width / 2), 168 - st.height))
        img.alpha_composite(research_h(small=True, scale=0.82, dy=34))
    return img, img


def style_h1(): return h_style('h1')
def style_h2(): return h_style('h2')
def style_h3(): return h_style('h3')
def style_h4(): return h_style('h4')


def main():
    out = Path(sys.argv[1]); style = sys.argv[2].lower(); sizes = [int(s) for s in sys.argv[3:]]
    src = ROOT / 'extracted' / 'drp-v262144' / ENTRY
    if not src.exists():
        return 3
    global CACHE
    CACHE = out / 'icon_src' / 'pikachu_p001_00_1.png'
    try:
        decode(src, CACHE)
    except ImportError:
        return 4
    from PIL import Image
    full, small = globals()['style_' + style]()
    for n in sizes:
        im = small if (style == 'a' and n < BADGE_MIN) else full
        im.convert('RGBa').resize((n, n), Image.LANCZOS).convert('RGBA').save(out / 'icon_src' / f'small_{style}_{n}.png')
    print('icon_pikachu: ok', style, ENTRY)
    return 0


if __name__ == '__main__':
    sys.exit(main())
