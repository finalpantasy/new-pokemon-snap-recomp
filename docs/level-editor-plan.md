# Level editor plan

Status (2026-11-14): milestone 1 works. `tools/level-editor` is a read-only Rust course viewer that opens any `LC_*` course pack from the local unpacked data, lists its parts, and draws the course geometry, the camera rail, other paths and the spawn points in a 3D view. Everything past milestone 1 below is a plan, not built.

This tool was an owner to-do that was parked until the core game and graphics work land (AGENTS.md, "New Pokémon and courses"). Rust work on the recompiler itself is paused (owner, 2026-11-12); this editor is a separate tool and does not touch the recompiler, the runtime or `main`.

## Ground rules

- The tool reads the local unpacked data (`extracted/drp-v262144/`) and never writes to it. Later milestones write only into a mod pack folder.
- No game data, extracted assets or generated code go into the repository or into any tool output that is shared. Screenshots stay local (`tools/level-editor/shots/` is git-ignored).
- A new or changed course loads through the existing opt-in mod path. Original files are never edited. With no mod pack enabled the game is the original game.

## What a course is made of

A course is one pack under `nx/Pack/LevelComposite/master/LC_<id>.drp/` (275 packs). Florio (`LC_001_A`) is the reference course below.

| Part | Where it lives | Format | Read today | Write today |
|---|---|---|---|---|
| Course index (legs) | `LC_001_A.courseb` | FlatBuffers (`analysis/registries/schemas/courseb.fbs`) | yes: variants (legs) and their level hashes | no |
| Level list per leg | courseb variant `level_hashes` | zlib CRC32 of entry name without extension | yes: 7 of 7 refs resolve for Florio leg 1 | no |
| Environment placement | `LV_ENV_001_1_A.levelb` | FlatBuffers, version 6 | yes: 45 model instances (name, position, rotation, scale), lights, decals | no |
| Geometry | `SM_ENV_*.nusrcmdlb` + `numshb` + `nusktb` + `numatb` + `nutexb` | SSBH (ssbh_data) | yes: meshes, skeleton transforms; materials and textures not drawn yet | ssbh_data can write meshes; not wired up |
| Camera rail | `LV_MAP_001_1_A.levelb` | FlatBuffers path component | yes: 3 segments, segment 0 is 115.7 units (runtime log shows the rail at 0..116) | no |
| Spawns | `LV_MAP_*.levelb` spawn components (slot 10) | FlatBuffers | yes: 230 spawns on leg 1, with species template, position or linked path | no |
| Movement paths | `LV_MAP_*.levelb` path components (slot 8) | FlatBuffers | yes: 180 non-rail paths with point lists and event links | no |
| Character templates | `LV_MAP_*.levelb` slots 6, 9, 11 | FlatBuffers | yes: skeleton name, animation pack path (`%CharacterComposites%/Pokemon/<nnn>/...`) | no |
| Behavior scripts | `*.luo` (13 in the Florio pack) | compiled Lua 5.1 | listed only | no |
| Collision | `SM_ENV_*_Ground.DC4A8177` and `.navmshb` next to each ground mesh | FlatBuffers-like, schemas partial | no | no |
| Trigger shapes | `LV_MAP_*.levelb` component slot 7 | FlatBuffers | counted (202 on leg 1), not decoded | no |
| Lighting | `LV_ENV_*.lprb` (light probes, 16 MB), env params at levelb root slot 13 | binary / FlatBuffers | env parameter names only | no |
| Sound | `LV_SND_001_1.levelb` | FlatBuffers | counted | no |
| Cutscenes | `IGC_MAP_*.cutb` | unknown | names only | no |
| Course registration | `RouteData.rtd` in `nx/Pack/Resident/master/Resident.drp/` | FlatBuffers | schema exists in `analysis/registries` | no |
| Scoring and research | `ResearchPoint`, `GameParameter` level params, `MissionData`, `PokemonData` | FlatBuffers registries | partial schemas | no |
| Course text | MSBT in 10 languages | MSBT | outside this tool | no |

### levelb layout as decoded so far

| Root slot | Content |
|---|---|
| 1 | level name |
| 2 | entities (`components[]`, name hash) |
| 3 | scene nodes (`components`, `node_id`, `entity_hash`) |
| 6 | course parameters; field 0 is the scene node id of the camera rail (node, then entity, then that entity's path component) |
| 10 | point sets |
| 11 | species numbers used on the leg (Florio leg 1: 3, 6, 7, 10, 15, 16, 17, 19, 32, 34, 48, 61, 65, 73, 76, 81, 84, 87, 91, 95, 97, 99, 112, 113) |
| 13 | environment parameters (ambient color, fog, wind, rim light and others) |
| 14 | `Search_*` scan entries |

- A component is a table with a tag byte in field 0 and one sub-table in a higher field. The field number gives the kind: 1 light or probe, 4 transform, 5 model instance, 6 character template, 7 collision or trigger shape, 8 path, 9 character name, 10 spawn, 11 character behavior, 14 effect, 15 decal. These names are working labels from the data, not from code.
- Spawn (kind 10): field 1 position, field 2 rotation quaternion, field 3 records (field 0 = template entity hash, field 2 = node id of a path entity). 153 of 230 Florio leg 1 spawns carry their own position; the rest start at their linked path.
- Model instances name a mesh. The file is `<name>_LightmapUV.nusrcmdlb` or `<name>.nusrcmdlb` in the same pack (all 45 Florio leg 1 instances resolve this way; LOD1 and LOD2 files also exist).

Measured with the viewer: Florio leg 1 has 788,912 triangles in view with sky and lighting helper meshes hidden, and loads in about 0.6 s cold. Leg 2 (`LV_MAP_001_2_A`) has a 13-point rail of 24.6 units, 75 spawns and 44 model instances. Leg 2's rail length has not been compared with a runtime log yet.

## How a new or changed course loads at runtime

The existing mod loader (`docs/mods.md`, `runtime/mods/`) replaces single DRP entries from `%APPDATA%\SnapRecomp\mods\<pack>\<archive>.drp\<entry>.<ext>`. The hook sits in the DRP entry worker (`MAIN_INLINE_HOOKS` in `recomp/snap_gen.py`, before `main+0x5e3a58`). That gives two paths:

1. **Changed course (works with today's loader).** The editor writes edited copies of existing entries (for example `LV_MAP_001_1_A.levelb`) into a mod pack under `LC_001_A.drp\`. With the pack enabled the game reads the edited entry; with it disabled the game reads the original. Nothing in `extracted/` or the game dump changes.
2. **New course ID (needs loader work).** A new course needs entries that do not exist in any archive: a new `LC_<new>.drp` pack, new level entries with new name hashes, and new rows in `RouteData`, `GameParameter`, `ResearchPoint` and the text tables. The current hook only swaps entries the game already asks for. Two options, to be settled in milestone 2:
   - extend the loader so a mod pack can declare new archives and new entries that the DRP index lookup returns (cleanest, needs the index lookup function decompiled and a `main` regeneration);
   - first prove the pipeline by cloning into an unused existing ID: replace an existing course's entries with the cloned content under a mod pack. This shows the data path without loader changes but is not a real new ID.

The plan is to do the second as a quick proof, then the first for real new courses.

## Placing existing species

Everything needed to place a species already in the game is in the level data the viewer reads:

- a character template entity (kind 6 skeleton plus animation pack path, kind 9 `CH_nnn` name, kind 11 behavior);
- a spawn component pointing at that template, with a position or a path link;
- optionally a path entity for its movement, with events that link back to entities;
- the species number in root slot 11, which likely drives which `CH_nnn.drp` packs are loaded for the leg.

A placement editor clones a template already in the leg (or copies one from another course's levelb), adds a spawn, and writes the edited levelb into a mod pack. Unknowns: what behavior field values mean, how a species' behavior script is chosen, and whether slot 11 must list every species used or is only a preload hint. Each is checked by an in-game run after the edit.

## Importing new Pokémon models later

A new species needs, per `analysis/registries/README.md`: `PokemonData`, `MissionData`, `RouteData` hooks, `ResearchPoint` rows, `GameParameter` level params, `SilhouetteData`, text in 10 languages, and a `CH_nnn.drp` pack with model, skeleton, materials, textures and animations. Steps:

1. **Clone first.** Copy an existing species pack to a new ID with only names changed and prove it loads, spawns, animates, scores and saves.
2. **Model import.** glTF in, SSBH out: `nusrcmdlb`, `numshb`, `nusktb`, `numatb`, `nutexb`. ssbh_data writes mesh, skeleton, material and animation files; `scripts/hd/ssbh_to_gltf` already covers the reverse direction. Textures need a NUTEXB writer (block-linear layout, BC formats); the asset tooling list in AGENTS.md already calls for one.
3. **Skeleton and animation.** The simplest legal path reuses an existing species skeleton and its animation set, so behavior scripts that name bones and animation slots keep working. A fully new skeleton needs every animation the behavior asks for.
4. **Scoring and research.** Photo scoring reads per-species data (size, pose, research entries). These rows live in the registries above and must be decoded before a new species scores correctly.
5. **Save identity.** The save stores species numbers. A new ID outside the original range must be checked against save code before release so that modded saves cannot break an unmodded game.

## Tool stack

Rust, one Cargo project in `tools/level-editor/`, own `target/` folder inside it.

| Crate | Use | License |
|---|---|---|
| eframe / egui / egui-wgpu 0.37 | window, panels, lists, input | MIT OR Apache-2.0 |
| wgpu 30 (via egui-wgpu) | 3D view, offscreen render | MIT OR Apache-2.0 |
| glam 0.34.3 | math (same version ssbh_data uses) | MIT OR Apache-2.0 |
| ssbh_data (vendored in `tools/ref/ssbh_lib`) | SSBH mesh, skeleton, material, animation read and write | MIT |
| bytemuck | vertex buffers | Zlib OR Apache-2.0 OR MIT |
| pollster | block on wgpu futures in headless mode | MIT OR Apache-2.0 |
| png | screenshot output | MIT OR Apache-2.0 |
| walkdir | model index fallback | MIT OR Unlicense |
| crc32fast | entry name hashes | MIT OR Apache-2.0 |

Reasons: egui keeps the UI and the 3D view in one process with no web stack; wgpu runs on the 4090 through DX12 or Vulkan; ssbh_data is the only open SSBH library and is already used in this repo. FlatBuffers are read by a small bounds-checked reader (`src/fb.rs`) instead of generated code, because the schemas are inferred and still change. Writing levelb later can use the `flatbuffers` crate (Apache-2.0) once field meanings are stable, or a patch writer that keeps unknown fields byte for byte.

### Current prototype

- `src/fb.rs` FlatBuffers reader. `src/level.rs` levelb parser (components, paths, spawns, templates, rail). `src/course.rs` course packs, legs, level hash resolution, mesh lookup. `src/geom.rs` SSBH to triangles. `src/render.rs` offscreen wgpu renderer and orbit camera. `src/app.rs` the editor UI. `src/headless.rs` the screenshot mode.
- `level-editor` opens a window. `level-editor --course LC_001_A --variant 1 --shot shots\florio_leg1.png` renders the same UI offscreen to a PNG with no window. `--summary` prints the parts as text.
- Build: `cargo build --release` in `tools/level-editor` (about 7 s incremental).
- Known gaps: no textures or materials (flat colors by mesh name), no LOD switching, no collision view, no picking in the 3D view, no editing.

## Milestones

1. **Viewer (done).** Open a course pack, list parts, draw geometry, rail, paths and spawns. Proof: `shots/florio_leg1.png` (local).
2. **Round trip and clone.** Write a levelb back out byte-identical (patch writer), then clone Florio leg 1 into a mod pack with one visible change (move one spawn). Prove in the game through the run-slot system that the change shows and that disabling the pack restores the original. Then decide the new-ID loader path (above) and add a new course pack ID that loads and plays start to end.
3. **Placement editing.** Select, move and rotate spawns and model instances in the 3D view; edit rail points; add a spawn by cloning a template. Save to a mod pack. Show collision (`DC4A8177`, `navmshb`) so placements stay on walkable ground.
4. **Species clone.** Copy an existing species to a new ID across `CH_nnn.drp` and the registries; prove spawn, animation, scoring, research entry and save in game.
5. **Model import.** glTF to SSBH plus NUTEXB writer, reusing an existing skeleton first. Then new skeletons and animation sets.
6. **New course content.** New geometry (glTF to SSBH), lightmaps or probe bake, collision generation, course text and unlock rules.

## Risks and open questions

- **New entries need loader work.** The mod hook only replaces entries the game already requests. New archives and new IDs need the DRP index lookup decompiled and `main` regenerated.
- **Field meanings are inferred.** Most levelb fields are named from data patterns only. A writer must keep unknown fields exactly, and each edit type needs an in-game check.
- **Collision format unknown.** `DC4A8177` and `navmshb` are not decoded. Moved or new geometry without matching collision will let Pokémon walk through walls or fall.
- **Lighting.** Light probes (`lprb`) and lightmap UVs are baked. New geometry may look wrong until there is a probe bake path or a fallback.
- **Registries.** Course registration, unlock rules, scoring thresholds and research entries span several registries with partial schemas. Where scoring thresholds live is not confirmed.
- **Save identity.** How saves store course and species IDs, and what an unmodded game does with unknown IDs, is not checked.
- **Behavior scripts.** `luo` files are compiled Lua 5.1. Editing behavior needs a decompile and recompile path with matching bytecode format.
- **Unresolved hashes.** Some courseb level hashes do not match any entry name in their pack (Florio resolves fully; other packs not surveyed).
- **Repository tracking.** The root `.gitignore` ignores `tools/`, so the editor source is not tracked by git today. Tracking it needs the root rule changed from `tools/` to `tools/*` plus `!tools/level-editor/` (git cannot re-include a folder whose parent folder is ignored); the editor's own `.gitignore` already keeps `target/` and `shots/` out.
