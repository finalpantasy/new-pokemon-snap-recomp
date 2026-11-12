"""Photoscan detail transfer: base = x4_realesrgan; add the high-pass (Gaussian sigma) of a CC0 scan tiled seamlessly
(1024 px period divides the 4x size), then per-channel histogram match to the source. Output <tex>.x4_scan.png, registered.
Usage: python -I detail_scan.py <tex.orig.png> <scan.jpg> [--sigma 3] [--gain 0.5]"""
import sys, os, json, argparse, numpy as np
from PIL import Image, ImageFilter
ap = argparse.ArgumentParser(); ap.add_argument("tex"); ap.add_argument("scan")
ap.add_argument("--sigma", type=float, default=3.0); ap.add_argument("--gain", type=float, default=0.5)
a = ap.parse_args()
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
im = Image.open(a.tex).convert("RGBA"); src = np.array(im.convert("RGB"))
H, W = src.shape[:2]
base = Image.open(a.tex.replace(".orig.png", ".x4_realesrgan.png")).convert("RGB")
assert base.size == (W * 4, H * 4)
sc = Image.open(a.scan).convert("L")
scf = np.array(sc, np.float32)
hp = scf - np.array(sc.filter(ImageFilter.GaussianBlur(a.sigma)), np.float32)   # blur on the tile itself (scan is seamless)
reps = (-(-H * 4 // hp.shape[0]), -(-W * 4 // hp.shape[1]))
hp = np.tile(hp, reps)[:H * 4, :W * 4]
res = np.clip(np.array(base, np.float32) + a.gain * hp[..., None], 0, 255).astype(np.uint8)
out = np.empty_like(res)
for c in range(3):
    sv = np.sort(src[..., c].ravel()); rv = res[..., c]
    rs = np.sort(rv.ravel()[::max(1, rv.size // 1000000)])
    rq = np.interp(np.arange(256), rs, np.linspace(0, 1, rs.size))
    out[..., c] = np.clip(np.interp(rq, np.linspace(0, 1, sv.size), sv), 0, 255).astype(np.uint8)[rv]
o = Image.fromarray(out).convert("RGBA"); o.putalpha(im.getchannel("A").resize((W * 4, H * 4), Image.BICUBIC))
outp = a.tex.replace(".orig.png", ".x4_scan.png"); o.save(outp)
cache = os.path.join(root, "build", "hd", "cache"); man = os.path.join(cache, "manifest.json")
m = json.load(open(man))
if not any(v["id"] == "x4_scan" for v in m["variants"]):
    m["variants"].append({"id": "x4_scan", "label": "ESRGAN + CC0 photoscan detail (ambientCG Ground111)", "scale": 4})
rel = lambda p: os.path.relpath(p, cache).replace("\\", "/")
for asset in m["assets"]:
    for t in asset["textures"]:
        if t["orig"] == rel(a.tex): t["variants"]["x4_scan"] = rel(outp)
json.dump(m, open(man, "w"), indent=2)
print("done", outp, "mean diff", np.abs(out.reshape(-1, 3).mean(0) - src.reshape(-1, 3).mean(0)).round(2))
