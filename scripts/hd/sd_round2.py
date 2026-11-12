"""groundB round-2 comparison. Usage: python -I sd_round2.py <workdir> <out.png>"""
import sys, os, numpy as np
from PIL import Image, ImageDraw, ImageFilter
W = sys.argv[1]; OUT = sys.argv[2]
cache = "build/hd/cache/florio_ground/textures/SM_ENV_001_groundB_00_dif"
box = (3584, 3584, 4608, 4608)
src = Image.open(W + "/gb.orig.png").convert("RGB")
srcA = np.array(src)
def lp(im, s): return np.array(im.filter(ImageFilter.GaussianBlur(s)), np.float32)
def match(res, ref=srcA):
    out = np.empty(res.shape, np.uint8)
    for c in range(3):
        sv = np.sort(ref[..., c].ravel()); rv = res[..., c]
        rs = np.sort(rv.ravel()[::max(1, rv.size // 1000000)])
        rq = np.interp(np.arange(256), rs, np.linspace(0, 1, rs.size))
        out[..., c] = np.clip(np.interp(rq, np.linspace(0, 1, sv.size), sv), 0, 255).astype(np.uint8)[rv]
    return out
def P(a): return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
orig = src.resize((1024, 1024), Image.NEAREST)
lanc = src.resize((1024, 1024), Image.LANCZOS)
re = Image.open(cache + ".x4_realesrgan.png").convert("RGB").crop(box)
s1 = Image.open(cache + ".x4_sd.png").convert("RGB").crop(box)
A = Image.open(W + "/gb.A.png").convert("RGB")
V = {"orig 4x nearest": orig, "Real-ESRGAN": re, "SD round 1": s1, "A: SD retuned": A}
SIG = 4.0
# B: low from Real-ESRGAN, high from A (SD)
for gain in (1.0, 1.5):
    hi = np.array(A, np.float32) - lp(A, SIG)
    V[f"B: ESRGAN low + SD high x{gain}"] = P(lp(re, SIG) + gain * hi)
# C: scan detail (luma high-pass), base = Lanczos of original
scan = Image.open("build/hd/external/ambientcg/Ground111/Ground111_1K-JPG_Color.jpg").convert("L")
sc = np.array(scan, np.float32)
sc = np.tile(sc, (2, 2))[:1024, :1024]
shp = sc - np.array(Image.fromarray(sc.astype(np.uint8)).filter(ImageFilter.GaussianBlur(SIG)), np.float32)
base = np.array(re, np.float32)
def hp(sig):
    return sc - np.array(Image.fromarray(sc.astype(np.uint8)).filter(ImageFilter.GaussianBlur(sig)), np.float32)
V["C1: ESRGAN + scan s3 x0.5"] = P(base + 0.5 * hp(3.0)[..., None])
V["C2: ESRGAN + scan s6 x0.7"] = P(base + 0.7 * hp(6.0)[..., None])
V["C3: SD(A) + scan s4 x0.6"] = P(np.array(A, np.float32) + 0.6 * hp(4.0)[..., None])
if os.path.exists(W + "/gb.D.png"):
    V["D: SDXL"] = Image.open(W + "/gb.D.png").convert("RGB")
for k in list(V):
    if k.startswith("orig"): continue
    V[k] = Image.fromarray(match(np.array(V[k])))
print("mean abs diff of RGB mean vs source:")
for k, v in V.items():
    print(f"  {k:36s}", np.abs(np.array(v, np.float32).mean((0, 1)) - srcA.mean((0, 1))).round(2))
C = 400; Z = 100
cx = (1024 - C) // 2; zx = (1024 - Z) // 2
n = len(V)
sheet = Image.new("RGB", (n * (C + 4), C + 4 + C + 24), (30, 30, 30))
d = ImageDraw.Draw(sheet)
for i, (k, im) in enumerate(V.items()):
    sheet.paste(im.crop((cx, cx, cx + C, cx + C)), (i * (C + 4), 0))
    sheet.paste(im.crop((zx, zx, zx + Z, zx + Z)).resize((C, C), Image.NEAREST), (i * (C + 4), C + 4))
    d.text((i * (C + 4) + 4, C + 4 + C + 6), k, fill=(255, 255, 0))
sheet.save(OUT)
