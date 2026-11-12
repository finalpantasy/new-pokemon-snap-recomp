"""Contact sheet: orig 4x nearest | x4_realesrgan (if present) | x4_sd. Row 1: 1:1 crop. Row 2: 3x zoom of a smaller crop.
Usage: python -I sd_sheet.py <tex.orig.png> <out.png> [cx cy]   (cx, cy in 4x pixels; default image center)"""
import sys, os
from PIL import Image, ImageDraw
o = sys.argv[1]; out = sys.argv[2]
base = o.replace(".orig.png", "")
ims = [("orig 4x nearest", Image.open(o).convert("RGB"))]
ims[0] = (ims[0][0], ims[0][1].resize((ims[0][1].width * 4, ims[0][1].height * 4), Image.NEAREST))
for lab, suf in (("x4_realesrgan", ".x4_realesrgan.png"), ("x4_sd", ".x4_sd.png")):
    if os.path.exists(base + suf):
        ims.append((lab, Image.open(base + suf).convert("RGB")))
W, H = ims[0][1].size
C = min(480, W, H); Z = min(160, C)
cx = int(sys.argv[3]) if len(sys.argv) > 4 else W // 2
cy = int(sys.argv[4]) if len(sys.argv) > 4 else H // 2
x0 = max(0, min(W - C, cx - C // 2)); y0 = max(0, min(H - C, cy - C // 2))
zx = x0 + (C - Z) // 2; zy = y0 + (C - Z) // 2
sheet = Image.new("RGB", (len(ims) * (C + 4), C + 4 + Z * 3 + 24), (30, 30, 30))
for i, (lab, im) in enumerate(ims):
    sheet.paste(im.crop((x0, y0, x0 + C, y0 + C)), (i * (C + 4), 0))
    z = im.crop((zx, zy, zx + Z, zy + Z)).resize((Z * 3, Z * 3), Image.NEAREST)
    sheet.paste(z.crop((0, 0, min(Z * 3, C), Z * 3)), (i * (C + 4), C + 4))
    ImageDraw.Draw(sheet).text((i * (C + 4) + 4, C + 4 + Z * 3 + 6), lab, fill=(255, 255, 0))
os.makedirs(os.path.dirname(out), exist_ok=True)
sheet.save(out)
