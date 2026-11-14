# Shiny Pokemon mod: plan

Status: plan only. Nothing here has been run in game. Every claim about spawn behavior is a static finding or an inference and is marked as such. Game data stays local: no extracted assets, keys or dumps go into git, and recolored textures are built from the user's own dumps into a git-ignored folder.

## 1. Goal and constraints

Very rarely, a spawned Pokemon is drawn in a shiny color variant. The base game has no shiny variants.

- Off by default. Own switch `SNAP_SHINY` and a native options row.
- Cosmetic only. Photo scoring, research progress, Photodex entries and saves behave exactly as in the vanilla game.
- Host side only. The guest code, guest memory and save files are not edited.
- No AI image generation. Recolors are computed with numeric color math from the local dumps.
- Local only until the owner decides to release. Public naming and README rules apply if that ever happens.

## 2. What the game does (findings)

- No shiny data exists. `docs/mod-ideas.md` row 24 records that no variant or odds field was found. Forms exist as `form_index` 0..19 with textures `A00..A19`. Form is Photodex and research identity (for example Vivillon patterns), so it must never be changed.
- Placement is by string. Levels place actors by name (`SK_CH_nnn_FF_0_Axx`), not numeric id.
- A few species have a second variant set (`_1_` in file names, own model, material instance and material files, shared mesh). Mean colors match variant 0 closely, so these are not shinies. What selects variant 0 or 1 at spawn is unknown. This is not needed for the chosen design.
- Textures: about 234 character archives, about 628 body (`_dif`) textures. Formats are BC1 (268), BC7 (237) and BC3 (123), mostly 1024 px. About 80 percent of species have exactly one non-eye `_dif`. Eye and mouth textures are usually separate and can stay untouched.
- A texture-pack replacement is global per entry name (`runtime/mods/mods.cpp`, `rt_mod_entry`). It cannot tell two spawns of one species apart, and the loader serves only entries that already exist in the archive index. So a plain pack cannot do "one spawn in thousands".
- Model instances share one generic class (vtable at main+0xc7c5e8, ctor main+0x32dc90). Update picks the LOD, pre-draw rebuilds the render job. Draws are recorded later on job threads, so a thread-local flag set in the update cannot reach the texture-bind record.
- `AGENTS.md` notes that arena 7 holds per-model-instance mesh buffers, created through main+0x8f3150, main+0x958430 and main+0x95e6d0. If true for same-species spawns, the vertex buffer is a per-instance key at draw time. Not yet confirmed by a draw log.
- Draw state already carries what is needed: `DrawState` in `runtime/nvn/nvn_draw.cpp` holds `vb[]`, `vbAddr[]`, UBOs and `tex[][]` handles. `PrepareTextures` calls `ResolveHandle` with the whole draw state for every sampled handle.
- A per-draw texture swap already exists as a test aid: `AbSubstitute` (`SNAP_MODS_AB`) builds a second `TextureImpl` (`abAlt`) from another file, matches it by hashing the first 4096 bytes of the guest texture data, and returns it per draw. Twins upload lazily and cache. This is the template to copy.
- There is no usable per-instance color constant in the character shaders. Parameters are per material and shared. A shader hue shift would also tint eyes, specular and fog, and would need patching about 815 programs. Rejected.
- Scoring is color neutral. Visibility comes from the evaluation depth copy, evaluation sphere and aggregate pixel counters (stencil based), then CPU thresholds. Breakdown categories are Pose, Size, Direction, Placement and Other Pokemon. None reads texel color. Caveat: foliage and other cutouts use alpha in the evaluation depth, so a twin texture must keep alpha byte-identical.
- Photos and album store rendered pixels only, so a shiny photo carries its color with no save change.
- A spawn-identity candidate exists: the skeleton-name parser at main+0x3bea50 (regex `SK_CH_(\d{3})_(\d{2})_(\d)_(A|B)(\d{2})`, two callers). It sees the full name once per call. Whether it fires once per actor or once per resource load is unknown. It is a fallback for tagging and a source of species identity, not the primary hook.

## 3. Chosen hook: per-instance tag, per-draw texture swap

Three parts, all host side.

### 3.1 Instance tagging (the dice roll)

Goal: know, at draw time, that a given draw belongs to a shiny instance.

1. Add inline guest hooks at entry and exit of the model-resource creation function (main+0x95e6d0) through `MAIN_INLINE_HOOKS` in `recomp/snap_gen.py`, as other mods do. The entry hook sets a thread-local instance id.
2. In `BufferInitialize` (`runtime/nvn/nvn_gfx.cpp`), through a new callback beside `snap_nvn_on_buffer_init` (set in `host/boot.cpp`), every buffer created while an id is set is recorded in an interval map keyed by GPU address range, value is the instance id and the roll result.
3. Roll once per instance with a seeded generator. Probability is 1 in `SNAP_SHINY_ODDS`.
4. `BufferFinalize` erases the range, so arena address reuse never inherits a stale flag.

Fallback without regenerating `main`: walk the guest frame-pointer chain inside `BufferInitialize` (`snap_nvn_guest_chain` already does this) and key on the caller frame of the creation function.

Second fallback if same-species instances turn out to share vertex buffers: key on a per-instance skinning or bone UBO instead, or use the 0x3bea50 parser to get a per-actor id. The draw log in M1 decides this.

Roll rules:

- Never touch the game's own random generator.
- Seeded by a host-side profile seed (stored under `%APPDATA%\SnapRecomp\`, never in the game save) mixed with the course and the species and a per-name ordinal, so a reload of the same attempt gives the same answer and a fresh ride can re-roll. A per-ride counter is bumped only at a fresh course start.
- Log one line per roll: course, species, roll, yes or no.

### 3.2 Draw-time swap

In `ResolveHandle`, beside the `AbSubstitute` call: if the mod is on and the draw's `vbAddr[0]` falls inside a shiny-flagged range, call a new `ShinySubstitute`.

- Hash the first 4096 bytes of the texture source, look up a table of hash to recolored texture file.
- Build a twin `TextureImpl` lazily (new field `shinyAlt`, copying the `abAlt` pattern), cache it, return it.
- The key is the data hash, not the texture object, so it works whether species share textures or not, and it is evaluated per draw. A shiny and a normal instance of one species render correctly side by side.
- Draws with no table match fall through unchanged. Only body `_dif` samplers need twins. Eyes, mouths and effect textures are left alone in M1 and M2.
- Twins keep the original format, size, mip count and alpha. Same-size block-compressed twins are known to work through this path.
- Photo and evaluation draws must resolve the same twin as the live view. This needs one explicit test.

Why not the alternatives: a loader pack is global per entry; a shader recolor is shared and touches the wrong channels; editing the placement string or form would create a fake form and corrupt Photodex and research identity; duplicating materials needs new archive entries the loader will not serve.

### 3.3 Recolor pipeline (local tooling)

New script `scripts/hd/make_shiny_pack.py` plus `scripts/hd/shiny/palettes.json` and `scripts/hd/shiny/masks/`.

1. Decode each non-eye `_dif` with the existing decoder in `scripts/hd/build_cache.py`.
2. Convert sRGB to linear to OKLCh. Keep lightness so baked shading survives. Change hue and chroma only. Copy alpha untouched.
3. Per species, a palette entry lists regions: a source selector (hue band with optional chroma and lightness gates, a UV-island reference, or a mask file) and a destination (target hue, chroma scale, lightness shaping curve). Achromatic sources (white, grey, black) use a chroma-add mode instead of hue rotation.
4. Masks, in order of effort: (a) soft hue and chroma gating, zero hand work; (b) UV-island masks reused from `scripts/hd/species/*.json` (38 species already authored); (c) hand-painted grayscale mask. Feather 2 to 4 px at mip 0, before mip generation.
5. Protect list: eyes, mouth, tongue, teeth, sclera, nose. Skip eye and mouth textures. Do not touch `_par` or `_nor`.
6. Encode with `scripts/hd/nutexb_encode.py` using the original file as the footer source, so format, sRGB flag and mip chain match.
7. Output to a git-ignored local folder (for example `%APPDATA%\SnapRecomp\mods\shiny-pokemon\`) with a hash table `shiny_table.json` (source-hash to twin file). The table must be built from the same texture version the user runs (original or HD pack), or built from the replacement files, because the key is the served bytes.
8. Review aid: a contact sheet per species (normal and shiny, three shading buckets) under `analysis/mods/compare/`. Automatic checks: lightness correlation above 0.98, alpha byte-identical, flag textures where over 40 percent of pixels are protected or the mean shift is tiny.

Target palettes come from public references written down as a few hex colors per species (body, accent, belly, markings). No images are downloaded or copied.

Estimated species classes (estimate, not measured): about 110 auto (single texture, clean hue bands), about 70 semi-auto (overlapping bands, accents to keep), about 35 needing hand masks (white or black to color, glow regions, multi-form families such as the 20-form one). M2 covers the auto class only.

## 4. Switches and options

Environment variables (all read once at startup in `host/mods/gamemods.cpp`):

| Name | Default | Meaning |
| --- | --- | --- |
| `SNAP_SHINY` | 0 | Master switch. 1 enables the mod. |
| `SNAP_SHINY_ODDS` | 4096 | Chance is 1 in N per spawned instance. 1 forces every eligible instance (testing). 512 is a friendly preset. |
| `SNAP_SHINY_SEED` | profile seed | Optional fixed seed for repeatable tests. |
| `SNAP_SHINY_FORCE` | unset | Species number or `all`: roll always succeeds for that set (testing). |
| `SNAP_SHINY_TRACE` | 0 | Log spawn-hook calls and strings only, no recolor. |
| `SNAP_SHINY_GLINT` | 0 | M3 sparkle effect. |

Native options row: a "Shiny Pokemon" row on the Effects tab (Photos tab is the alternative). Values: Off, 1/4096, 1/512, Test (every spawn of eligible species). It follows the `nativeopt.cpp` pattern (patched `option.lmb` through `scripts/ui/make_native_options.py`, value stored under a `runtime/hle/settings.cpp` key, never in the game save). Env variables override the row.

## 5. Safety design

- No guest memory, form index, species number, placement string, research data or save file is written. All state is host side.
- Alpha is byte-identical in every twin. Only RGB changes.
- Non-Pokemon actors (props, environment) are excluded from tagging and have no table entries.
- Species without a clean single body texture are skipped until they have a reviewed palette entry.
- Photo, evaluation and thumbnail passes are untouched. Evaluation draws that sample character textures are checked to see the same twin.
- Instance flags are erased on buffer finalize and re-rolled on scene reload (the game recreates instances through its deferred-destroy ring). This is acceptable for a rare cosmetic roll.
- Twin uploads happen lazily and must not spike on the replay thread. Pre-warm shiny twins for the loading screen if a hitch shows up. Arena 7 memory is already tight and the Pikachu hub crash is known, so watch both.
- Nothing printed or committed from game data. Scripts and the plan contain no extracted content.

## 6. Milestones

### M1: one species, forced shiny on every spawn

Scope: one pilot species (Bidoof, whose islands and textures are already known, or another single-texture species). Odds forced to 1.

Work:
1. Draw-log run in a scene with two same-species spawns (existing `DebugDrawLog`) to confirm distinct vertex buffers and see which meshes own separate buffers. Run `SNAP_SHINY_TRACE=1` to see whether the name parser fires once per actor.
2. Add inline hooks, interval map and `BufferInitialize` callback.
3. Add `ShinySubstitute` and `shinyAlt`.
4. `make_shiny_pack.py` for the pilot species, with the hash table.
5. `SNAP_SHINY`, `SNAP_SHINY_ODDS`, `SNAP_SHINY_FORCE`.

Done means: with the mod on and odds 1, every instance of the pilot species is visibly recolored in the live view, in every LOD, and in the exported photo; the screenshot is saved as proof; one fixed photo scored with `florio_eval` has the same five category scores with the mod on and off (compare the evaluation counter lines too). Then odds 2 shows a shiny and a normal of that species at once. Note: `AGENTS.md` says photo scoring is not yet fully deterministic, so compare against two vanilla runs to measure noise first.

### M2: rarity and all auto-recolorable species

1. Real odds (default 1/4096), seeded deterministic roll, per-name ordinal, ride counter, roll log.
2. Native options row.
3. Palettes for the auto class (about 110 species), contact-sheet review, then one in-game check per batch by course in game order. Reuse the 38 authored island files for the semi-auto species as time allows.
4. Hash table built per texture version (original and HD pack).
5. Save and reload round trip: Photodex, research and save files unchanged apart from the photo.
6. Seeded rerun gives the same shinies.

### M3: sparkle effect and photo marker

1. Optional glint under `SNAP_SHINY_GLINT`: first a host-drawn additive sparkle overlay near the shiny's screen position for about 2 seconds plus a short host-played sound. Later, spawn the game's own spark effect (spark model and cheek spark effect assets exist) once spawning effects from the host is proven.
2. Photo marker: at each photo-size JPEG encode (already wrapped in `host/mods/photoexport.cpp`), record whether a shiny instance was drawn in view recently. Write `shiny: true`, species, form and course into the existing JSON sidecar and a host-side `shiny_photos.json` keyed by JPEG CRC. Show a star in the gallery tool (`tools-src/gallery`). Badging the in-game album is out of scope.

## 7. File sets

New:
- `docs/shiny-mod-plan.md` (this file)
- `host/mods/shiny.cpp` and `.h`: roll, interval map, options and env, log, photo marker
- `scripts/hd/make_shiny_pack.py`, `scripts/hd/shiny/palettes.json`, `scripts/hd/shiny/masks/`
- Local, git-ignored output: twin textures and `shiny_table.json`, contact sheets, `shiny.json` profile seed

Changed:
- `recomp/snap_gen.py` (two inline hooks) and a regenerated `main`
- `runtime/nvn/nvn_gfx.cpp` (buffer-init callback and finalize erase)
- `runtime/nvn/nvn_draw.cpp` (`ShinySubstitute` next to `AbSubstitute`)
- `runtime/nvn/nvn_internal.h` (`shinyAlt` on `TextureImpl`)
- `host/boot.cpp` (callback registration)
- `host/mods/gamemods.cpp`, `host/mods/nativeopt.cpp`, `scripts/ui/make_native_options.py`, `runtime/hle/settings.cpp` (switch and options row)
- `host/mods/photoexport.cpp`, `tools-src/gallery` (M3)
- `docs/mod-ideas.md` (update row 24), `AGENTS.md` (add a "Shiny mod" section once M1 lands)

Not touched: `runtime/mods/mods.cpp` loader (owned by another worker, and not needed by this design).

## 8. Risks

1. Unconfirmed assumption: that the creation function wraps all of one instance's buffers and that same-species spawns do not share vertex buffers. Mitigation: M1 starts with the draw log. Fallbacks in section 3.1.
2. Regenerating `main` is heavy. Queue it under `build\heavy.lock`, one heavy process at a time, BelowNormal priority.
3. Spawn semantics: the name parser may be per resource, not per actor, and scan-triggered or scripted spawns may follow other paths. Only matters if the buffer-based tag fails.
4. Color quality: baked shading in `_dif` can fight the transform for black or white to color shifts. Eyes, mouths and glow parts left uncolored may look odd, and the earlier material pass was rejected as creepy. Keep recolors conservative, review contact sheets, hand-mask only the few that need it.
5. HD pack interaction: the table key is the served texture bytes, so a table built for original textures will not match HD replacements. Build per pack.
6. Memory and hitches: one extra image per shown species body on first shiny draw. Lazy, so zero cost when off. Watch arena 7 pressure and the known hub crash.
7. Scoring noise: score comparison needs a baseline for run-to-run variation. Alpha must stay identical.
8. Estimates for hand-mask species vary by about 40 percent. Those species are explicitly out of M2.

## 9. Effort

| Milestone | Days |
| --- | --- |
| M1 (draw log, hooks, swap, one species, proof) | 4.5 |
| M2 (rarity, options row, tooling, about 110 auto species, round-trip tests) | 6 |
| M3 (glint, photo marker, gallery star) | 2 |
| Total | about 12 |

Semi-auto and hand-mask species (about 105 more) are a separate, optional batch of roughly 12 more days and are not part of this plan.
