# HD assets: preview pipeline and viewer

Goal: preview higher-resolution textures, and later better models, outside the game, with before/after comparison. Then pack the good ones as a mod that the game loads through the RomFS overlay.

Everything produced from game data stays local, under `build/hd/`, which is git-ignored. The repo holds only code.

## Phase 1: textures and preview (now)

There are two parts, joined by the cache layout below.

1. **Pipeline:** `scripts/hd/`, Python, run with `-I` or `-E -P`.
   - It finds an asset's model and textures in `extracted/drp-v262144/`.
   - It converts the SSBH model (numdlb/numshb/numatb/nusktb) to glTF 2.0, mesh and skeleton only, with no textures embedded.
   - It decodes the textures to PNG.
   - It runs local AI upscalers on them.
2. **Viewer:** `tools-src/asset_viewer/`, a static web app (three.js) plus a tiny localhost server. It reads the cache and shows the assets before and after.

### Cache layout (the contract)

```
build/hd/cache/
  manifest.json
  <asset_id>/
    model.gltf (+ .bin)          mesh, skeleton, UVs, material slots; textures referenced by name only
    textures/<tex>.orig.png      decoded original (RGBA8, all mips dropped)
    textures/<tex>.<variant>.png upscaled variants, e.g. x4_realesrgan, x4_anime
    info.json                    per-asset details (LODs, material -> texture map)
```

`manifest.json`:

```json
{
  "version": 1,
  "variants": [{"id": "x4_realesrgan", "label": "Real-ESRGAN x4plus", "scale": 4}],
  "assets": [{
    "id": "pm0025_00",
    "name": "Pikachu",
    "category": "pokemon|course|character|prop|ui",
    "model": "pm0025_00/model.gltf",
    "lods": ["pm0025_00/model.gltf", "pm0025_00/model_lod1.gltf"],
    "materials": [{"name": "body", "albedo": "tex_a", "normal": "tex_n", "mask": "tex_m", "emissive": null}],
    "textures": [{
      "name": "tex_a",
      "role": "albedo|normal|mask|emissive|other",
      "orig": "pm0025_00/textures/tex_a.orig.png",
      "w": 512, "h": 512, "format": "BC7",
      "variants": {"x4_realesrgan": "pm0025_00/textures/tex_a.x4_realesrgan.png"}
    }]
  }]
}
```

### Viewer features

- An asset list with search and category filter.
- **3D view:** orbit and zoom, a variant toggle (original or any upscale), a split-screen A/B mode, wireframe, LOD switch, and a neutral studio light plus an HDRI-free sky.
- **Texture view:** a before/after slider at 1:1 and zoomed, pixel-peeping with nearest filtering, and per-channel display (RGB, A).

## Phase 2: in-game test (after phase 1 looks good)

1. Re-encode the chosen upscales to nutexb, in the same format as the originals (BC7/BC1/BC5, mips, block-linear).
2. Place them in a mod folder that the HLE `rom` overlay mounts over RomFS.
3. Compare screenshots in game.

## Phase 3: better models (later)

- A numshb/numdlb writer.
- Retargeting the existing skeletons and rigs onto higher-detail meshes.
- Sources can be the owner's own game data, models we make ourselves (subdivision and sculpting, or local generation), or third-party model files the owner chooses for personal use.
- Third-party files live only in `build/hd/external/` with a local `SOURCES.md` provenance list, and are never committed or distributed. Only model and texture formats are allowed, never executables.

## AI re-texture (SD1.5 + tile ControlNet, 2026-11-12)

Local only. Models in `tools/sd/` (see `tools/sd/SOURCES.md`), venv `tools/venv-sd`. Script: `scripts/hd/retexture_sd.py` (run with `tools\venv-sd\Scripts\python.exe -I`), contact sheets: `scripts/hd/sd_sheet.py` -> `analysis/hd/sd_<tex>.png`.

- Method: wrap-pad (if the source tiles), Lanczos 4x, 512 px tiles with 128 px feathered overlap, img2img strength 0.4 plus tile ControlNet, per-channel histogram match back to the source, alpha resampled only. Output `<tex>.x4_sd.png`, variant `x4_sd` in the manifest. `--max-seconds N` pauses and resumes (the 2048 ground = 8192 px takes about 7.5 min on a shared 4090; other textures 10 s to 2 min).
- Verified: mean RGB within 0.6 of the source on groundB, groundC and bark; edge wrap difference no worse than the source on those three.
- Verdict from contact sheets: the flower, bark and groundB variants are visibly different at a glance and faithful. It adds legible structure (pebbles, bark plates, petal veins), but it is smoother and more painterly than the original grain. At strength 0.4 and above SD invents content: rockK (terraced cracks, dark holes) and rockD (moss turned to straw) drift, so rocks are not ready. Lower strength (0.25-0.3) trades detail for faithfulness.
- Not run on normal or mask maps.

### groundB round 2 (owner: round-1 SD "a bit smooth")

Sheet: `analysis/hd/groundB_round2.png` (1:1 crop and 4x zoom per column; test crop = 256 px source centre). Seam check: `analysis/hd/groundB_scan_seams.png`.
- A (SD retuned: sharper prompt/negatives, cn 1.3, 40 steps, DPM++ 2M Karras, strength 0.4, unsharp) and B (ESRGAN low + SD high, gain 1.0/1.5): still soft; SD at 512 px tiles cannot invent sharp grain from a blurry 4x input. Not worth shipping over round 1.
- C (`scripts/hd/detail_scan.py`, variant `x4_scan`): ESRGAN x4 base + high-pass (sigma 3, gain 0.5) of an ambientCG CC0 dirt scan (Ground111, `build/hd/external/`), histogram-matched to the source. Crisp, photo-like grain, layout and colors unchanged; mean RGB within 0.43 of the source (full 8192 px). Caveat: fine speckle also lands on smooth stones.
- Tileability: the source groundB itself is not seamless (edge wrap difference 8.8 vs neighbor 2.1); x4_scan adds only about 1.3 to it (the 1024 px scan tile divides 8192).
- D (SDXL + tile ControlNet) not installed: A to C did not need it.

### x4_blend (owner pick, 2026-11-12)

The owner rejected x4_scan (grain) on groundB and prefers Real-ESRGAN and SD, or a blend. `scripts/hd/blend_variants.py` writes `<tex>.x4_blend.png` = 50/50 RGB blend of x4_realesrgan and x4_sd (alpha from Real-ESRGAN), then an additive per-channel mean lock to the source (mean diff at most 0.25 on all). Made for flower, grassB, bark, groundB (8192), groundC, groundD, rockF; not rockD or rockK (SD drift). Every variant is at most 8192 px. Real-ESRGAN dif variants were created for these textures with the existing ncnn tool (`-n realesrgan-x4plus`). Seen on a groundC crop: the SD half adds a hatched pattern and shifts the leaf layout, and the blend keeps a faint trace of it.

## Phase 2 result (2026-11-12)

- **Tools:** `scripts/hd/detail_boost.py` makes `<tex>.x2_detail.png` for tiling environment textures: Lanczos 2x, plus the texture's own finest band-pass detail re-used one octave finer (tiled with a shift, zero mean, so colors do not move), a mild unsharp, and a bump term from that band added to the normal map. Mask and emissive maps get a plain 2x. `scripts/hd/make_hd_pack.py` writes a pack from the cache in the original nutexb format (BC1/BC3/BC5 as the source, full mip chain, block-linear, capped at 4096). Choice order for albedo: x4_blend (not groundC), x4_sd (bark, flower), x2_detail. Emissive (BC6H) is not encoded.
- **In game:** pack `build/mods_hd_florio/hd-florio` (28 Florio textures) loaded with 28 `[mods] hit` lines and no crash. Size 2x and 4x of the original works. Crops in `analysis/mods/compare/hd_ground_*` and `hd_path_*`: finer grain, crisper pebbles and grass, same colors. The gain is modest, and the ground is still magnified at 1080p.
- **Magenta wash:** not caused by larger textures. With only `hud-00024/25` (960x540) replaced at 1x, the same full-screen wash shows at the same fade (`analysis/mods/compare/magenta_wash_1x_hud2425.png`). Those two are a full-screen overlay used in fades. The text "Y" is the first typed letter of the text box.
- **Loader check:** a replacement nutexb with a bad footer, size or mip table is skipped with `[mods] skipped ...: invalid nutexb (reason)` and the original loads (`nutexb_problem`, `runtime/mods/mods.cpp`). All 28,369 original nutexb files and the packs pass the same rules in Python; the C++ was syntax-checked with clang only.
- **4K check (2026-11-12):** `hd-florio` at `SNAP_RENDER_SCALE=2` (3840x2160, groundB 8192) loaded 28 of 28 and did not crash. The pack is copied to `%APPDATA%\SnapRecomp\mods\hd-florio`. Cross-run frame pairing at 4K failed (image difference too high), so only unpaired crops exist in `analysis\mods\compare\4k\`.
- **Pikachu pack and the hub crash (QA bug B1):** `hd_pikachu` at 4096 (22 MB per texture) crashed the hub load. A 2048 build crashed the same way (`BufferInitialize: builder has no memory pool`), and a 1024 build (the original size) ran clean (1 run each, so weak evidence). `make_pikachu_packs.py` now caps at 1024 by default (`--max-edge`). Texture sizes above the original may exceed a fixed pool in the game. The missing eyes were not reproduced: no test route shows Pikachu on screen, and the decoded eye textures match the originals in format and alpha.
- **Loader cost:** replacement files are now read and validated once and shared (before, each hit re-read the file from disk). Present count at 125 s on `qa_lab1` with other workers running: no mods 6090, owner set plus hd-florio 5550, old loader with the same set 6330. The reported 15x slowdown did not reproduce with the current owner set.
