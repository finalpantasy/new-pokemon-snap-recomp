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
