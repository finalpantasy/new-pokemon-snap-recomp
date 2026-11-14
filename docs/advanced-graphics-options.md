# Advanced graphics options (design)

Date: 2026-11-14. Status: design only, nothing implemented. Written from a read of the option code, not from a running game.

Goal: finer graphics tuning inside the game's own Settings screen (never the F1 overlay), with the original look as the default for every row and nothing that changes photo scoring.

## 1. Where it lives

The native menu has no scrolling and no sub-pages. Every PC tab is a fixed page of 6 row slots, and the only secondary screens are the pulldown popups. So there are two ways to add rows:

1. Widen rows that already exist. A pulldown holds up to 10 items, and several current rows offer only 4 to 6, so most "more granular" wishes are solved by more steps on the same row, with no new tab.
2. Add new rows for switches that have no row yet. Quality, Textures, Effects, Display, Controller and Mouse are full (6 of 6). Output and View each have 2 free slots. Anything else needs a new tab.

Decision:

- Widen existing rows in place (section 4, group "Widen").
- Put the one output-related new row (HDR expand) in the 2 free slots on Output.
- Append one new tab, "Shadows & AO", at the END of `TABS` in `scripts/ui/make_native_options.py`. It becomes tab 11 and opens a third tab set. Appending keeps every existing tab index and row where it is. The generic code (`go_tab`, base = (t // 5) * 5) already supports a third set, but a third set has never been run in the game. Verifying it in-game is the first implementation chunk.
- The lighting, shadow, AO and ray-tracing stream adds its rows to this tab first (3 reserved slots), then to a second new tab (tab 12, "Lighting & RT") when it outgrows 6 rows.

Sliders are not used. The slider widget exists in the generator but no PC page drives the knob yet, so every multi-step value is a pulldown of 3 to 10 labelled steps.

## 2. Ground rules for every row

- Default equals the original look. A row's default must equal the F1 `Opt.def`, so Restore Default erases the key instead of storing an explicit value.
- Restart notice: no badge exists in the native menu, so every restart row ends its help text with " Applies after restart." Live rows omit it. Help text is at most 112 characters (generator assert). Labels stay near 20 characters.
- No row may touch photo scoring. Excluded on purpose: `SNAP_FOV`, `SNAP_LOD_FORCE0`, `SNAP_FULL_RATE_ANIM`, `SNAP_PASS_FORCE`, `SNAP_VSYNC_HZ`, legacy pace, `NVN_NO_*_FIX/EXEMPT`, and `SNAP_PHOTO_MIN_W`. `SNAP_LOD_DIST` and `SNAP_DRAW_DIST` keep their current View rows (already flagged "may change photo scores"); no new granular steps there.
- Env switches that are debug or compatibility only stay env-only (upscaler jitter/mv/log, AA dump/split, pass dump/ab, HDR debug, Vulkan, memory and shader-cache switches, `SNAP_GAME_FONT`, `SNAP_VISUALS`).

## 3. Grouping

The groups below are how rows are described in this document and in help text. On screen they map to tabs as shown.

| Group | Screen location |
|---|---|
| Resolution & upscaling | Quality tab (widen render scale) and Output/Display (unchanged) |
| Anti-aliasing & sharpening | Quality tab (widen sharpening) |
| Textures & LOD | Textures tab (widen LOD bias) |
| Shadows & lighting | New tab "Shadows & AO" |
| Ambient occlusion | New tab "Shadows & AO" (radius, strength); existing AO rows stay on Effects |
| Post effects & HDR | Output tab (HDR expand, HDR mode widen, HDR peak Auto) |
| Frame rate & pacing | Display tab (widen frame cap) |

## 4. Row table

Kind is two-choice (c) or pulldown (p). "Widen" means the key already has a row; the table lists the new step list. "New" means a new row. "Reserved" means a slot held for the lighting/RT stream; it ships empty (not drawn).

| Group | Label | Kind | Values | Default | Env / settings key | Live? | Help (one line) | GPU cost |
|---|---|---|---|---|---|---|---|---|
| Resolution & upscaling | Render scale (widen) | p, 10 | 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0 | 1.0 | `SNAP_RENDER_SCALE` / `gfx_render_scale` | Restart | Scene resolution as a multiple of 1080p. Applies after restart. | Very high; scales with pixels and video memory |
| Anti-aliasing & sharpening | Sharpening (widen) | p, 10 | 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.85, 1.0 | 0 (original) | `SNAP_SHARPEN` / `gfx_sharpen` | Restart | Contrast-adaptive sharpening after AA. Applies after restart. | Very low |
| Textures & LOD | Texture LOD bias (widen) | p, 10 | auto, -2, -1.5, -1, -0.5, 0, +0.25, +0.5, +1, +2 | auto | `SNAP_LOD_BIAS` / `gfx_lod_bias` | Restart (live plausible, unverified) | Sharper (negative) or softer (positive) texture detail. Applies after restart. | Small bandwidth cost when negative; can shimmer |
| Shadows & lighting | Shadow distance (New) | p, 9 | default (1x), 1.5x, 2x, 3x, 4x, 6x, 8x, 12x, 16x | default (1x) | `SNAP_SHADOW_DISTANCE` / `gfx_shadow_dist` | Restart | Widens how far dynamic shadows reach. Pair with shadow size. Applies after restart. | None directly; blurrier at a fixed map size |
| Ambient occlusion | AO radius (New) | p, 9 | default, 0.5x, 0.75x, 1.25x, 1.5x, 2x, 3x, 4x | default | `SNAP_SSAO_RADIUS` / `gfx_ssao_radius` | Restart | Size of the ambient occlusion reach (simple AO). Photo-select screen only. Applies after restart. | None |
| Ambient occlusion | AO strength (New) | p, 9 | default, 0.5x, 0.75x, 1.25x, 1.5x, 2x, 3x, 4x, 8x | default | `SNAP_SSAO_STRENGTH` / `gfx_ssao_strength` | Restart | How dark the occlusion looks (simple AO). Photo-select screen only. Applies after restart. | None |
| Post effects & HDR | HDR expand (New) | p, 9 | 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0 | 0.3 | `SNAP_HDR_EXPAND` / `gfx_hdr_expand` | Restart | How far near-white scene pixels lift into HDR. Needs HDR on. Applies after restart. | None |
| Post effects & HDR | HDR mode (Widen: gfx_hdr from on/off to 3 choices) | p, 3 | Off, HDR10, scRGB | Off | `SNAP_HDR` / `gfx_hdr` | Restart | Output format. scRGB has far less banding. Applies after restart. | Low |
| Post effects & HDR | HDR peak (Widen: add Auto) | p, 9 | Auto, 400, 600, 800, 1000, 1500, 2000, 3000, 4000 | Auto (no key; monitor value, else 1000) | `SNAP_HDR_PEAK` / `gfx_hdr_peak` | Restart | Peak brightness in nits. Auto reads the monitor. Applies after restart. | None |
| Frame rate & pacing | Frame rate limit (Widen) | p, 10 | None, 30, 45, 60, 75, 90, 120, 144, 165, 240 | None | `SNAP_WINDOW_FRAME_CAP` / `frame_cap` (window API) | Live | Caps frames per second. Only lowers the game's own pacing. | Lowers load |
| Shadows & lighting | Reserved 1: Lighting preset | p | default, high, ultra (existing F1 row `gfx_lighting`, currently Soon) | default | `SNAP_LIGHTING` / `gfx_lighting` | Restart | Reserved. Lands when the F1 row is flipped from Soon and `ultra` is handled in code. | Moderate |
| Shadows & lighting | Reserved 2: shadow softness / filtering | p | set by the lighting stream | original | set by the lighting stream | TBD | Reserved. | TBD |
| Shadows & lighting | Reserved 3: ray-traced effects on/off | c or p | set by the RT stream | off | set by the RT stream | TBD | Reserved. | High |

Notes on specific rows:

- Existing rows that stay as they are: AA mode, upscaler, upscale quality, aniso, shadow size, SSAO quality/samples/HBAO, Bloom/DOF/grain/aberration toggles, Look and its intensity, HDR paper white, UI font scale, window size, fast boot, photo export.
- HDR mode: the old row wrote only `1` (hdr10). Migration: a saved `1` reads as HDR10. `scRGB` writes `scrgb`. Test that scRGB works on the current present path before shipping; the scoring recheck so far covers hdr10 only.
- HDR peak Auto: today the row shows 1000 and writes `SNAP_HDR_PEAK` on every save, which overrides the monitor's reported value. Auto must erase the key.
- Frame cap steps 75/165/240 are new values for the existing live row; `get_cap`/`set_cap` already take any integer.
- Render scale above 3.0 needs a note in help: at 4x the scene is about 33 MP at 4K output. If this is too easy to hit by accident, drop 4.0 from the list and keep it env-only. Open question for the owner (see end).

## 5. Quality preset (Custom), SNAP_VISUALS and hand-set env

How the preset is derived: `get_preset` compares the current values of the keys in `kPresetKeys[]` (11 keys) with each entry of `kPresets`. No match means "Custom". A row whose key is not in `kPresetKeys` never flips the preset to Custom.

Rules for the advanced rows:

1. Rows that change look at a noticeable level (render scale, sharpening, LOD bias are already counted if in `kPresetKeys`; shadow distance) should be in `kPresetKeys`. Add `{"gfx_shadow_dist", "default"}` and bump `kPresetKeyN`. Each preset in `kPresets` gets one value for it: `""` (erased) for Original and Normal, `"2"` for High (matches the existing `SNAP_LIGHTING=high` meaning), `"4"` for Ultra. Numeric strings need `preset_eq` to compare floats for the new index.
2. AO radius, AO strength, HDR expand, HDR mode and peak, and frame cap are personal taste or hardware settings, not quality tiers. They stay out of `kPresetKeys`: changing them does not turn the preset into Custom, and choosing a preset does not reset them.
3. `set_preset` writes or erases all listed keys, so picking Normal also erases shadow distance. This is intended.
4. Known mismatch to fix first (chunk 0): the native AA row stores `original` and `ssaa2+smaa`, while `kPresetKeys`/`kPresets` use `default` and `ssaa2,smaa`. Picking Original by hand stores a non-default string and reads as Custom, and "SMAA + supersampling" never matches High. Align the native row values with the F1 values before adding more preset keys.

SNAP_VISUALS: `original` (default) and `enhanced` only change the snapdef defaults (AA, sharpen, aniso, UI font scale, PNG export). A new row whose default differs between the two looks needs a snapdef entry in `runtime/hle/visual_defaults.h`; the native default then comes from that file at pack build. None of the new rows need this, since their defaults are the same under both.

Hand-set env wins: `runtime/hle/settings.cpp` exports a saved key to its `SNAP_*` variable only when that variable is not already set. So an env var set by hand always beats the menu value, and the menu shows its saved value with no sign of the override. The F1 overlay has an "env wins" warning; the native menu does not. Plan: document this in `host/README.md`, and add an optional chunk that appends a short "(env)" marker to the row's current-value text when the env var is set at start.

Existing behaviors to keep in mind: an explicit `SNAP_RENDER_SCALE` beats `ssaa2` and turns it into smaa; an upscaler turns AA off and sharpening to 0 unless `SNAP_AA` / `SNAP_SHARPEN` are set. Help text for those rows should say so in one clause.

## 6. Reserved slots for the lighting / RT stream

The lighting stream (see `docs/lighting-rt-plan.md`) uses this contract:

- First add the option to the F1 table (`graphics_opts()` in `runtime/ui/ui_options.cpp`) with `kRestart` or `kLive`. A native row for a `kSoon` option is refused (the trace logs "(refused)").
- Then add the key to `kEnvKeys` in `runtime/hle/settings.cpp`, the switch to `host/README.md`, and the native `row(...)` in tab 11, slots 4 to 6 (slot 1 to 3 are shadow distance, AO radius, AO strength).
- Reserved row order in tab 11: Lighting preset, Shadow softness, RT toggle. A second new tab (12, "Lighting & RT") takes the rest. Tab 12 starts empty and is not added to `TABS` until it has at least one row.
- `gfx_lighting` `ultra` currently does nothing; either implement it or remove it from the F1 choices before the native row ships.

## 7. Testing and docs per row

Each row needs the same checks as any native row: F1 test lines in `tests/hle/ui_options_script1.txt` (`opt`, `expect_opt`, `expect_when`), `expect_env` in `ui_options_script2.txt`, the `expect_pending` count at line 61 bumped, a spaced route update in `host/routes/native_options_tabs.txt`, a regenerated `analysis/ui/native_options_tab_*.png`, a dated paragraph in `AGENTS.md`, and a table line in `host/README.md`.

## 8. Implementation checklist (independent chunks)

Order matters only where noted. Each chunk can be a separate worker chat.

- Chunk 0, preset/AA value alignment (do first). Files: `scripts/ui/make_native_options.py` (AA row choices and default), `runtime/ui/ui_options.cpp` (only if values change there), `tests/hle/ui_options_script1.txt`. Done when picking Original leaves the preset on its named value and High matches after picking SMAA + supersampling.
- Chunk 1, third tab set check. Files: `scripts/ui/make_native_options.py` (append a tab "Shadows & AO" with the single Shadow distance row), `host/routes/native_options_tabs.txt`, `analysis/ui/native_options_tab_*.png`. Done when the game shows tab 11 in the strip, L/R chain works from Photos and from Game, and the screenshot is saved.
- Chunk 2, Shadow distance row (needs chunk 1). Files: `runtime/hle/settings.cpp` (`kEnvKeys`), `runtime/ui/ui_options.cpp` (F1 `Opt`, preset key), `scripts/ui/make_native_options.py`, tests script1/script2, `host/README.md`, `AGENTS.md`.
- Chunk 3, AO radius and strength rows (needs chunk 1). Same file set as chunk 2 minus the preset edits. Both are no-ops outside the photo-select screen, so test there.
- Chunk 4, widen existing rows: render scale, sharpening, LOD bias, frame cap. Files: `scripts/ui/make_native_options.py` (choice lists only), `runtime/ui/ui_options.cpp` (matching F1 choices, keep defaults), tests script1, route. No new key, no env change. Check that a saved value from the old list still displays correctly.
- Chunk 5, HDR rows: HDR mode (3 choices with migration of `1`), HDR peak Auto, HDR expand (new key). Files: `runtime/hle/settings.cpp`, `runtime/ui/ui_options.cpp`, `scripts/ui/make_native_options.py` (Output tab, uses the 2 free slots), `host/mods/nativeopt.cpp` only if migration needs code, tests, `host/README.md`. Needs an HDR display to verify scRGB.
- Chunk 6, optional env-wins marker. Files: `host/mods/nativeopt.cpp`, `host/README.md`. No row changes.
- Chunk 7, lighting stream hookup (waits for `docs/lighting-rt-plan.md` work). Files: per section 6.

Common to every chunk: run `python -I scripts/ui/make_native_options.py --out <mods root>` (play.ps1 does this with `--if-stale`), keep each tab to 6 rows (add an assert for `len(rows) <= 6` in the generator in chunk 1), keep pulldowns to 10 items and help to 112 characters.

## 9. Risks

- Third tab set untested in the game (chunk 1 settles this).
- No scrolling: a seventh row on any tab silently misbehaves until the assert is added.
- The pulldown popup cursor starts on the header, not the current value (known gap in `host/README.md`), which is more noticeable with 10-step lists.
- Label and choice lengths are not limited by the generator; keep them short.
- Memory: render scale 3 to 4 and shadow maps 4096/8192 can run out of video memory on smaller GPUs.

## 10. Open questions for the owner

1. Keep 4.0 in the render scale list, or leave 4x env-only?
2. Should High/Ultra presets set shadow distance (2x, 4x), or should shadow distance stay a free tuning row that never changes the preset?
3. Add the "(env)" marker (chunk 6), or document the env override only?
