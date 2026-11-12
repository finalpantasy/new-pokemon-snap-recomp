"""AI re-texture: 4x upscale with Stable Diffusion 1.5 + tile ControlNet, low-denoise img2img in
overlapping tiles. Local only. Run with the tools/venv-sd python:

  tools\\venv-sd\\Scripts\\python.exe -I scripts\\hd\\retexture_sd.py <tex.orig.png> [...] [--prompt ...]

Output: <tex>.x4_sd.png next to the input, registered in build/hd/cache/manifest.json.
- Tileable sources (auto-detected, or --wrap on/off) are wrap-padded so edges stay seamless.
- Alpha is carried over unchanged (resampled only).
- Per-channel histogram match back to the source (average color preserved).
- --max-seconds N: stop after N s, save state next to the output, exit 3; rerun the same command to resume.
"""
import argparse, json, os, sys, time, hashlib
import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MODELS = os.path.join(ROOT, "tools", "sd", "models")
MANIFEST = os.path.join(ROOT, "build", "hd", "cache", "manifest.json")
SAMPLER = "unipc"
VARIANT = {"id": "x4_sd", "label": "SD1.5 + tile ControlNet x4 (detail, color-matched)", "scale": 4}

PROMPTS = {
    "ground": "close-up photo of dry dirt and grass ground texture with small pebbles, sharp fine detail, natural, highly detailed",
    "rock": "close-up photo of mossy rock surface texture, fine cracks and grain, sharp fine detail, highly detailed",
    "bark": "close-up photo of tree bark texture, fine wood grain and fibers, natural, highly detailed",
    "grass": "close-up photo of grass blades texture, fine veins, natural, highly detailed",
    "flower": "close-up photo of flower petals texture, fine veins, natural, highly detailed",
    "leaf": "close-up photo of green leaf texture, fine veins, natural, highly detailed",
}
NEG = "blurry, lowres, text, watermark, new objects, people, animals, frame, border, jpeg artifacts"


def default_prompt(name):
    n = name.lower()
    for k in PROMPTS:
        if k in n:
            return PROMPTS[k]
    return "close-up photo of a natural surface texture, fine detail, highly detailed"


def detect_wrap(rgb):
    a = rgb.astype(np.float32)
    wrap = (np.abs(a[0] - a[-1]).mean() + np.abs(a[:, 0] - a[:, -1]).mean()) / 2
    adj = (np.abs(a[0] - a[1]).mean() + np.abs(a[:, 0] - a[:, 1]).mean()) / 2
    return wrap <= max(1.6 * adj, adj + 3.0)


def match_hist(res, src):
    """Per-channel histogram match of res (HxWx3 uint8) to src distribution."""
    out = np.empty_like(res)
    for c in range(3):
        sv = np.sort(src[..., c].ravel())
        rv = res[..., c]
        rs = np.sort(rv.ravel()[:: max(1, rv.size // 1_000_000)])
        q = np.linspace(0, 1, 256)
        # map: result value -> quantile -> src value
        rq = np.interp(np.arange(256), rs, np.linspace(0, 1, rs.size))
        sq = np.interp(rq, np.linspace(0, 1, sv.size), sv)
        out[..., c] = np.clip(sq, 0, 255).astype(np.uint8)[rv]
    return out


def feather(t, ov):
    w = np.ones(t, np.float32)
    r = np.linspace(0, 1, ov + 2, dtype=np.float32)[1:-1]
    w[:ov] = r
    w[-ov:] = r[::-1]
    return w


def load_pipe():
    import torch
    from diffusers import ControlNetModel, StableDiffusionControlNetImg2ImgPipeline, UniPCMultistepScheduler
    cn = ControlNetModel.from_pretrained(os.path.join(MODELS, "cn_tile"), torch_dtype=torch.float16)
    pipe = StableDiffusionControlNetImg2ImgPipeline.from_pretrained(
        os.path.join(MODELS, "sd15"), controlnet=cn, torch_dtype=torch.float16,
        safety_checker=None, requires_safety_checker=False, use_safetensors=True, variant="fp16")
    from diffusers import DPMSolverMultistepScheduler, EulerAncestralDiscreteScheduler
    cfg = pipe.scheduler.config
    if SAMPLER == "dpm2m_karras":
        pipe.scheduler = DPMSolverMultistepScheduler.from_config(cfg, use_karras_sigmas=True, algorithm_type="dpmsolver++")
    elif SAMPLER == "euler_a":
        pipe.scheduler = EulerAncestralDiscreteScheduler.from_config(cfg)
    else:
        pipe.scheduler = UniPCMultistepScheduler.from_config(cfg)
    pipe.to("cuda")
    pipe.set_progress_bar_config(disable=True)
    return pipe


def process(path, pipe, a):
    import torch
    name = os.path.basename(path).replace(".orig.png", "")
    outp = path.replace(".orig.png", "." + a.suffix + ".png")
    statep = outp + ".state.npz"
    im = Image.open(path).convert("RGBA")
    src_rgb = np.array(im.convert("RGB"))
    alpha = im.getchannel("A")
    H, W = src_rgb.shape[:2]
    S = 4
    wrap = bool(detect_wrap(src_rgb)) if a.wrap == "auto" else a.wrap == "on"
    pad = max(16, min(64, H // 4, W // 4)) if wrap else 0
    padded = np.pad(src_rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap" if wrap else "reflect") if pad else src_rgb
    big = Image.fromarray(padded).resize((padded.shape[1] * S, padded.shape[0] * S), Image.LANCZOS)
    big = np.array(big)
    T, OV = a.tile, a.overlap
    ph, pw = big.shape[:2]
    # tiny images: reflect-pad up to one tile
    ex_h, ex_w = max(0, T - ph), max(0, T - pw)
    if ex_h or ex_w:
        big = np.pad(big, ((0, ex_h), (0, ex_w), (0, 0)), mode="reflect")
    bh, bw = big.shape[:2]
    stride = T - OV
    ys = list(range(0, max(1, bh - T) + 1, stride))
    xs = list(range(0, max(1, bw - T) + 1, stride))
    if ys[-1] != bh - T: ys.append(bh - T)
    if xs[-1] != bw - T: xs.append(bw - T)
    tiles = [(y, x) for y in ys for x in xs]
    prompt = a.prompt or default_prompt(name)
    key = hashlib.md5(json.dumps([prompt, a.neg, a.sampler, a.unsharp, a.strength, a.steps, a.cn, a.guidance, T, OV, wrap, a.seed]).encode()).hexdigest()
    acc = np.zeros((bh, bw, 3), np.float32)
    wsum = np.zeros((bh, bw, 1), np.float32)
    start = 0
    if os.path.exists(statep):
        st = np.load(statep)
        if str(st["key"]) == key:
            acc, wsum, start = st["acc"], st["wsum"], int(st["i"])
            print(f"resume {name} at tile {start}/{len(tiles)}")
    wy, wx = feather(T, OV), feather(T, OV)
    w2 = (wy[:, None] * wx[None, :])[..., None]
    t0 = time.time()
    for i in range(start, len(tiles)):
        if a.max_seconds and time.time() - t0 > a.max_seconds:
            np.savez(statep, acc=acc, wsum=wsum, i=i, key=key)
            print(f"paused {name} at tile {i}/{len(tiles)}")
            return 3
        y, x = tiles[i]
        tile = Image.fromarray(big[y:y + T, x:x + T])
        gen = torch.Generator("cuda").manual_seed(a.seed + i)
        r = pipe(prompt=prompt, negative_prompt=(a.neg or NEG), image=tile, control_image=tile, strength=a.strength,
                 num_inference_steps=a.steps, guidance_scale=a.guidance,
                 controlnet_conditioning_scale=a.cn, generator=gen).images[0]
        wt = w2
        acc[y:y + T, x:x + T] += np.array(r, np.float32) * wt
        wsum[y:y + T, x:x + T] += wt
        if (i + 1) % 50 == 0:
            print(f"{name} tile {i + 1}/{len(tiles)}", flush=True)
    res = (acc / np.maximum(wsum, 1e-6))
    res = np.clip(res, 0, 255).astype(np.uint8)[:ph, :pw]
    if pad:
        res = res[pad * S:-pad * S, pad * S:-pad * S]
    if a.unsharp:
        from PIL import ImageFilter
        res = np.array(Image.fromarray(res).filter(ImageFilter.UnsharpMask(radius=a.unsharp_radius, percent=int(a.unsharp * 100), threshold=0)))
    res = match_hist(res, src_rgb)
    a_big = alpha.resize((W * S, H * S), Image.BICUBIC)
    out = Image.fromarray(res).convert("RGBA")
    out.putalpha(a_big)
    out.save(outp)
    if os.path.exists(statep):
        os.remove(statep)
    if a.suffix == "x4_sd":
        register(path, outp)
    print(f"done {outp} wrap={wrap} {W}x{H}->{W*S}x{H*S} tiles={len(tiles)} {time.time()-t0:.0f}s")
    return 0


def register(orig, outp):
    cache = os.path.join(ROOT, "build", "hd", "cache")
    rel = lambda p: os.path.relpath(p, cache).replace("\\", "/")
    m = json.load(open(MANIFEST))
    if not any(v["id"] == "x4_sd" for v in m["variants"]):
        m["variants"].append(VARIANT)
    r = rel(orig)
    for asset in m["assets"]:
        for t in asset["textures"]:
            if t["orig"] == r:
                t["variants"]["x4_sd"] = rel(outp)
    json.dump(m, open(MANIFEST, "w"), indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("textures", nargs="+")
    ap.add_argument("--prompt")
    ap.add_argument("--strength", type=float, default=0.4)
    ap.add_argument("--steps", type=int, default=24)
    ap.add_argument("--guidance", type=float, default=7.0)
    ap.add_argument("--cn", type=float, default=1.0, help="controlnet conditioning scale")
    ap.add_argument("--tile", type=int, default=512)
    ap.add_argument("--overlap", type=int, default=128)
    ap.add_argument("--wrap", choices=["auto", "on", "off"], default="auto")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--max-seconds", type=int, default=0)
    ap.add_argument("--neg")
    ap.add_argument("--sampler", choices=["unipc", "dpm2m_karras", "euler_a"], default="unipc")
    ap.add_argument("--unsharp", type=float, default=0.0, help="unsharp amount (1.0 = 100 percent)")
    ap.add_argument("--unsharp-radius", type=float, default=2.0)
    ap.add_argument("--suffix", default="x4_sd", help="output variant id (file suffix)")
    a = ap.parse_args()
    global SAMPLER
    SAMPLER = a.sampler
    try:
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # BelowNormal
    except Exception:
        pass
    pipe = load_pipe()
    rc = 0
    for p in a.textures:
        rc = process(os.path.abspath(p), pipe, a) or rc
    sys.exit(rc)


if __name__ == "__main__":
    main()
