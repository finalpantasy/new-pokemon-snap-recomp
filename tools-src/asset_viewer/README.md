# Asset Studio: local before/after asset viewer

A static three.js (MIT, pinned 0.180.0) viewer for AI-upscaled textures and models, plus a stdlib-only
localhost server. Read-only. It shows no game branding and ships no game content.

Contract: `docs/hd-assets.md`, **cache `manifest.json` version 1** (rejected with a message if another version)
and **`alt_models.json` version 1** (optional).

## Run

```powershell
cd tools-src\asset_viewer
npm ci                      # once, needs network (three.js, plus Playwright for tests); then offline
python -E -P serve.py       # http://127.0.0.1:8765, cache = build\hd\cache
python -E -P serve.py --cache path\to\cache --external path\to\external --port 9000
```

- Binds to 127.0.0.1 only; GET/HEAD only; rejects traversal and symlinks leaving the served roots.
- `/cache/` maps to the cache dir, `/external/` to `--external` (default `<cache>/../external`, i.e. `build\hd\external`),
  `/vendor/three/` to `node_modules/three`, `GET /api/manifest` to the manifest merged with alt models.
- Empty or missing cache: the page shows the build command `python -E -P scripts\hd\build_cache.py ...`.

## Features

- Asset list with search, category filter and texture counts.
- 3D: orbit/zoom/pan, studio lighting, normal maps, alpha-tested materials, wireframe, LOD, dark/light/gray background.
- **Model A / Model B** dropdowns: `Original (game)`, each `manifest.variants` entry (swaps every texture), and alt models.
  A/B split uses one shared camera and a draggable (or arrow-key) divider with scissor rendering.
  Each side shows triangle and vertex counts.
- Alt models (`<cache>/alt_models.json`): merged per asset by `asset_id`, falling back to a case-insensitive match on
  asset name. Paths are relative to the cache dir; the files carry their own textures. Missing or invalid file is ignored.
- Textures tab: grid (role, size, format), compare slider over the same region, 1x-16x nearest-neighbor zoom,
  shared pan, RGB/R/G/B/A channels, original and upscaled size readouts. Alt models are not shown here.
- Keys: `A`/`B` swap sides, `W` wireframe, `1`-`4` pick Model B from the dropdown order.

## Tests and test data

```powershell
python -E -P generate_testdata.py   # procedural, game-free cache: testdata\cache + testdata\external
npm test                            # headless Edge, writes docs\*.png and docs\verification.json
```

## Known gaps

- Alt models are not rescaled to match the original; a model in different units looks bigger or smaller.
- Skinned and animated models show the bind pose only.
- Texture compare uses the 2D canvas; very large images (over 8k) are untested.
- No pipeline-produced real cache was available during development; only synthetic data was tested.
- Packed masks are available in the texture inspector but are not applied to 3D materials; version 1 does not define their channel packing. Alpha testing uses a preview cutoff of 0.5.

The independent core checks can also be run with `python -B -E -P tests\test_server.py` and `node tests/core-browser.mjs`. Core screenshots are `docs/core-3d-ab.png` and `docs/core-texture-slider.png`; results are in `docs/core-verification.json`.
