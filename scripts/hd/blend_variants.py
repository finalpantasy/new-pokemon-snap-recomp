"""x4_blend = 50/50 pixel blend of x4_realesrgan and x4_sd (RGB), alpha from x4_realesrgan; registered in the manifest.
Usage: python -I blend_variants.py <tex.orig.png> [...]   (skips textures lacking either input)"""
import sys, os, json, numpy as np
from PIL import Image
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
cache = os.path.join(root, "build", "hd", "cache"); man = os.path.join(cache, "manifest.json")
rel = lambda p: os.path.relpath(p, cache).replace("\\", "/")
m = json.load(open(man))
if not any(v["id"] == "x4_blend" for v in m["variants"]):
    m["variants"].append({"id": "x4_blend", "label": "50/50 Real-ESRGAN + SD1.5 tile", "scale": 4})
for t in sys.argv[1:]:
    t = os.path.abspath(t); a = t.replace(".orig.png", ".x4_realesrgan.png"); b = t.replace(".orig.png", ".x4_sd.png")
    if not (os.path.exists(a) and os.path.exists(b)):
        print("skip", os.path.basename(t)); continue
    A = Image.open(a).convert("RGBA"); B = Image.open(b).convert("RGBA")
    assert A.size == B.size and max(A.size) <= 8192, (A.size, B.size)
    x = (np.array(A, np.float32)[..., :3] + np.array(B, np.float32)[..., :3]) / 2
    srcm = np.array(Image.open(t).convert("RGB"), np.float32).mean((0, 1))
    x += srcm - x[::4, ::4].mean((0, 1))   # additive per-channel mean lock (Real-ESRGAN shifts the mean)
    out = Image.fromarray(np.clip(x + 0.5, 0, 255).astype(np.uint8)).convert("RGBA"); out.putalpha(A.getchannel("A"))
    o = t.replace(".orig.png", ".x4_blend.png"); out.save(o)
    for asset in m["assets"]:
        for tx in asset["textures"]:
            if tx["orig"] == rel(t): tx["variants"]["x4_blend"] = rel(o)
    src = np.array(Image.open(t).convert("RGB"), np.float32).mean((0, 1))
    print("done", rel(o), A.size, "mean diff", np.abs(np.array(out.convert("RGB"))[::4, ::4].astype(np.float32).mean((0, 1)) - src).round(2))
json.dump(m, open(man, "w"), indent=2)
