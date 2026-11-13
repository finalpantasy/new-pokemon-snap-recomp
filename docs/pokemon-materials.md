# Pokémon materials: what the textures do and what a mod can change

Study date 2026-11-12. Static only: no game run was needed. Everything below comes from the shipped files and from the translated pixel shaders.

How it was done:

- All 1,369 `CharactersMaterials_CharacterShader_<id>` pixel shaders (815 unique; Resident.drp) were translated to SPIR-V with the project translator (harness `build/xlate/r2`) and decompiled to GLSL with `spirv-cross`. Each fetch was traced to the sampler name from the shader reflection (`scripts/nushdb_parse.py`, sampler binding b = (texture handle - 8) / 2, the rule in `analysis/shaders/translator.md`).
- Material instances (`MI_CH_*.matinstb`, FlatBuffers) were read for all 760 Pokémon materials in the 234 `CH_*.drp` packs.
- Scripts (read-only): `scripts/hd/material_channels.py` (channel use per sampler), `scripts/hd/shader_refl.py` (reflection dump), `scripts/hd/shader_twins.py`, `scripts/hd/matinst_census.py`, `scripts/hd/matinst_dump.py`.
- Local outputs (git-ignored): `analysis/materials/channels.txt`, `analysis/materials/census.txt`, `analysis/materials/names.tsv`, and decompiled shaders in `analysis/materials/glsl/<shader id>.frag`. "G:line" below means that file and line.

Pikachu (`CH_001`), Pichu (`CH_006`), Bidoof (`CH_032`), Vivillon (`CH_034`), Bouffalant (`CH_097`), Grookey (`CH_112`) and Scorbunny (`CH_113`) are the seven cached assets.

## 1. Texture roles

### Slots

Every material lists texture slots by name. The slot names in all 760 materials (`analysis/materials/census.txt`) are `baseColorTexture`, `paramsTexture`, `normalTexture`, `specularTexture`, `emissiveTexture`, `emissive2Texture`, `flockyInnerColorTexture`, `flowingLightBase/Color/PhaseTexture`, `tangentTexture`, `blendEmissiveTexture`, and cube maps. File suffixes map to slots: `_dif` = `baseColorTexture`, `_par` = `paramsTexture`, `_nor` = `normalTexture`, `_emi` = `emissiveTexture`, `_emi2` = `emissive2Texture`, `_flb/_flc/_flp` = the three `flowingLight*Texture`.

The shader supplies a default for any slot the material leaves out. The defaults are listed in the shader's `.reflectb` (for example `CharactersMaterials_CharacterShader_12075339.reflectb`: `paramsTexture` = `dummy_rosm_texture`, `normalTexture` = `dummy_normal_texture`, `emissiveTexture` = `dummy_black_texture`). The dummy textures are in `resource/nx/resource.drp`. Decoded:

| dummy | size / format | texel value | meaning |
|---|---|---|---|
| `dummy_rosm_texture` | 4x4 BC3 | R 156, G 255, B 255, A 0 | roughness 0.61, no occlusion, full specular, not metal |
| `dummy_normal_texture` | 4x4 BC5 | R 128, G 128 | flat normal |
| `dummy_flocky_texture` | 1024x1024 BC5 | mean 126, 126 | a noise normal map (fur grain), see section 3 |
| `dummy_black_texture` | 4x4 BC1 | 0,0,0 | no emission |

`dummy_flocky_texture` does exist (`analysis/romfs/drp_unpacked.tsv:14108`, entry 166 of `resource.drp`). It is not in a character pack, which is why a search of `CH_*.drp` misses it.

### `_dif` (`baseColorTexture`)

RGB = albedo, always. Alpha (W) is read by every one of the 1,363 shaders that sample it (`analysis/materials/channels.txt`) and written to the color target's alpha (`12075339.frag:670`, `3436703351.frag:865`). The `.nufxlb` files list the pass as `Opaque`, so alpha is not blended. In flocky (fur) shaders, alpha also scales the UV jitter (section 3). Keep alpha at the original's value (BC1 = opaque).

### `_par` (`paramsTexture`) is a "ROSM" map

The name comes from the debug parameters `debug_override_ROSM_occlusion` and `debug_override_ROSM_specularMask` in the materials, and from the default texture `dummy_rosm_texture`. Roughness, Occlusion, Specular mask, Metallic:

| channel | meaning | evidence |
|---|---|---|
| R | roughness. 0 = sharp reflection, 1 = blurred | The lit shaders use `max(R, c)` times 6 as the mip level of both environment cube maps (`3436703351.frag:431-440`; `_582 = _2027` at :433) |
| G | ambient occlusion. 1 = unoccluded | G multiplies the three ambient (spherical-harmonics probe) color terms (`3436703351.frag:444,505,691,718`; Pikachu: `12075339.frag:218,524,540`, the only channel that flocky shader reads) |
| B | specular / reflection mask | B scales the Fresnel-weighted reflection terms (`3436703351.frag:667,672`) that are summed into the output at `:824,830`. Inferred from the data flow plus the `ROSM_specularMask` name, not proven by a pixel test |
| A | metallic | only the 117 shaders that read `.w` (`channels.txt`), such as the metal shader `1096415554` (`paramsTexture: xyzw`, parameters `metal_coef`, `metal_color`). Default texture A = 0. BC1 has no alpha, so BC1 `_par` files cannot be metallic |

`_par` is BC1 on every cached Pokémon (BC1 stores no alpha), so for those R, G and B are the live channels.

### `_nor` (`normalTexture`)

Three shapes occur across the 733 shaders that sample a normal map (`channels.txt`):

- X and Y only (all 733). Z is rebuilt: `n.xy = fma(t, 2, -1)` then `z = sqrt(1 - x² - y²)` (`12075339.frag:233`). BC5 fits.
- X, Y and Z (311 shaders): the texture's third channel is read directly. Eye shaders do this (`1083376570`). The texture must be BC7 or BC3, because BC5 returns Z = 0.
- X, Y and W (51 shaders, such as Grookey's body shader `2701501019`, `2701501019.frag:80-83`): the alpha channel is a weight, not geometry. It feeds the lerp terms near the end of the shader (lines 736-1059) that sit beside the `innerColor` / `sssContrast` / `sssNormalMapWeight` parameters. I read it as a subsurface or thickness weight; this is inferred, not proven.

Originals use BC5 (Pichu body, Vivillon and Grookey eyes) and BC7 (Pikachu body and eyes). Pikachu's BC7 body normal only needs XY (`12075339.frag:73`).

### `specularTexture`, `tangentTexture`, emissive

- `emissiveTexture` (264 shaders) and `emissive2Texture` (56): RGB added as glow, scaled by the material's `emissiveScale`, `emissiveScale2`, `emissive2_externalScale`.
- `specularTexture` (15 shaders) and `tangentTexture` (21 shaders, XY only): used by the anisotropic-specular materials (`anisotropicSpecular_boost`, `anisotropic_aspect`; shader `1001224846` and 26 more). These exist only on a few species (`CH_036 062 087 148 187 220 223 238 260`); `tangentTexture` only on `CH_148`. The tangent texture gives a per-texel highlight direction (hair or brushed-metal style). The seven cached assets have none.

### Flowing light (`_flb`, `_flc`, `_flp`)

Used by 21 shaders, for example `3452202033` (`CH_107`; also `CH_074`, `CH_108`, `CH_109`, `CH_110`). `CH_107` files: `_flb` 2048 BC3 (RGBA base), `_flc` 2048 BC1 (flow color), `_flp` 2048 BC4 (one channel: phase). The shader reads phase `.x`, color `.xyz`, base `.xyzw` (`3452202033.frag:154-156,557-575`), with the material parameters `flowingLight_speed`, `_repeat`, `_onePieceLength`, `_brightness`, `_weight_offset`. So it is an animated glow that travels along the body: the phase map offsets the animation per texel, the color map tints it, the base map is the mask. `_emi2` is the second emissive map (`emissive2Texture`).

## 2. Normal maps on bodies that have none

Short answer: yes, but it takes a changed material file, not only a texture, for most species. Whether it works in game was not tested.

What decides it (`scripts/hd/matinst_census.py` style census over 760 materials, run 2026-11-12):

- 490 of 760 Pokémon materials use a pixel shader that has no normal sampler at all. Bidoof Body (`3436703351`), Vivillon Body and Mouth (`3116720224`, `1421326133`), Vivillon Wing (`2670489024`), Bouffalant Body (`1731682751`) and Grookey Body2/Body3 (`2972460439`) are in this group. `channels.txt` / `shader_refl.py` show no `normalTexture` in their samplers. A texture added to such a material is never read.
- 270 materials use a shader that does sample a normal map; 263 of those bind an authored `_nor`. Seven bind nothing, so the shader reads `dummy_normal_texture`. Grookey's main Body is one (`2701501019`). For these seven, **replacing or adding the texture is enough** if the material file adds the slot (below), because the shader already samples it.
- Scorbunny Body and Bouffalant BodyB bind `dummy_flocky_texture` as their normal (manifest, `build/hd/cache/manifest.json`). That is the fur noise normal, so their normal slot is already in use as a fur grain, not a flat default.

The vertex side is not the obstacle: all four bodies checked (Bidoof, Pikachu, Grookey, Vivillon) carry `Tangent0` and `Binormal0` in the `.numshb`. The no-normal vertex shader simply does not read them (`CharacterShader_3436703351VS` inputs: NORMAL, POSITION, TEXCOORD0 only; `CharacterShader_12075339VS` inputs add TANGENT0 and BINORMAL0, from `scripts/nushdb_parse.py` output). The mesh therefore needs no change.

So a body normal map needs this chain:

1. Re-point the material to a shader that has a normal sampler. In the matinstb the shader is named four times (main, `_zprepass`, two other passes), and the numatb names the main one. Both files list shader ids. `scripts/hd/shader_twins.py 3436703351` (`analysis/materials/twins_bidoof.txt`) finds 51 shaders with the same sampler list plus `normalTexture` and the same `ParameterList` names, and 41 with at most three parameter differences. Which one has the same lighting options as the original is not known: the shader id is a hash of option flags and no name table was found. Each candidate has to be tried in game. A wrong one shows as a lighting change.
2. Add a `normalTexture` slot with the new nutexb name to the matinstb (the slot layout is the same FlatBuffers table the other slots use; see `analysis/materials/census.txt` and `scripts/hd/matinst_dump.py` for the string order).
3. Ship the new `_nor` nutexb. Use BC5 if the chosen shader reads XY only, BC7 if it reads Z or W (check with `scripts/hd/material_channels.py ... --shader <id>`).

What the mod loader can do (`runtime/mods/mods.cpp`):

- It replaces any DRP entry by archive, name and extension id, not only textures. The hook swaps the entry chunk for a host-built uncompressed one (`mods.cpp:1-12`), and `docs/mods.md:3,27-30` says textures, models, FlatBuffers, Lua, UI and text. Only nutexb files are structure-checked (`mods.cpp:93`).
- A matinstb or numatb replacement is mechanically the same path, but no non-texture Pokémon mod has been run in game yet. Treat step 1 and 2 as untested.
- A new entry name that the game never requests (a `_nor` for a material that had none) is not loaded by itself; only an overridden entry is served. The slot has to come from the replaced matinstb.

## 3. Fur, parallax, subsurface, rim, emissive

Fur ("flocky"):

- 69 of 815 unique pixel shaders are "flocky" shaders (parameters `flocky_*`, `blend_NoV_scale`, `keep_specular`); 93 shaders hold two `baseColorTexture` samplers (the second is `flockyInnerColorTexture`). 20 species and 44 materials use `sampler_flocky` (`scripts` census 2026-11-12), including Pikachu, Pichu, Scorbunny, Bouffalant, Eevee and Aipom.
- It is a screen-space fuzz trick, not shells or fins. Pikachu's Body shader (`12075339`): `1 - N.V` (grazing angle) scales a UV offset taken from the normal's XY times the flocky texture's alpha (`12075339.frag:62-212`); the diffuse is fetched at the shifted UV and at a mip bias that grows toward the silhouette (`:195,212`, the `flocky_grazingAngle_uvNoise` and `flocky_grazingAngle_mipLebel` parameters). The result is a soft, blurred, noisy edge. `flocky_inner_*` and `flocky_outer_*` tint and contrast the edge.
- The normal slot supplies the noise: authored `_nor` where it exists (Pikachu body), otherwise `dummy_flocky_texture` (Scorbunny, Bouffalant BodyB, Eevee, Aipom). No fur geometry, no strand-direction anisotropy.
- A real "fur" look is therefore: a fine fur-grain normal map in `_nor`, plus diffuse that already holds fur strand detail. The shader will soften it toward the rim.

Anisotropic specular: 27 shaders (`anisotropicSpecular_boost`, `anisotropic_aspect`), species listed in section 1. Not on Pikachu or Eevee-line fur in the cached set.

Parallax and height: none. No shader has a height, depth or parallax sampler (full sampler list in section 1), and no material has a height or parallax parameter (`analysis/materials/census.txt`). `Eye_par` is an ordinary ROSM map (R, G, B read; `1083376570`); eyes get depth from the normal map and the environment cube reflections, not from UV parallax. A depth map in a new texture would be ignored.

Subsurface: no true subsurface. Two approximations exist. `innerColor`, `sssContrast`, `sssNormalMapWeight` (266 shaders, 458 materials) tint and wrap the diffuse term. `backScatter_gain`, `_weight`, `_chroma`, `_shadowBias` (27 shaders, 26 materials) add a back-lit transmission term (shader `1116359253`). Both are parameters, not textures; the only texture hook is the normal alpha described above.

Rim and Fresnel: `fresnel_gain`, `fresnel_chroma`, `rim_emissive` (512 shaders), `FrontReflection`, `specularColorRatio` (673). Parameters only.

Emissive: textures (`_emi`, `_emi2`) plus `emissiveScale`; flowing light as in section 1.

Refraction (gems, glass): `RefractionRatio_BgCube`, `RoughnessScale_BgCube`, `multiply_BG_color` (45 shaders, with a `cubeMap` sampler; shader `1030507707` samples a cube map with the normal).

Environment: 1,003 shaders sample two environment cube maps (`EnvironmentMap`, `LocalEnvironmentMap`) by direction and roughness; those come from the course, not from the character.

## 4. Authoring pipeline

Sizes: originals are 512 to 1024 on the cached Pokémon, 2048 on some (`CH_107`). The hub crashed at 4096 and 2048 for Pikachu and ran at 1024 (`docs/hd-assets.md`, Pikachu pack section, 1 run each), so 1024 is the safe edge; 2048 for a body was not verified. Mips: a full chain. Keep each file's original format class unless the table below says otherwise (the footer holds the format; a format change in game was not tested).

| class | `_dif` | `_par` (R rough, G occl, B spec, A metal) | `_nor` | notes |
|---|---|---|---|---|
| fur (flocky shader) | strand detail in RGB; keep alpha as is | R 0.6 to 0.9; G = clump and root shading (dark at the base); B 0.2 to 0.5; not metal | fur-grain noise, BC5 (XY); it doubles as the rim jitter source | G is the only `_par` channel the Pikachu fur shader reads (`12075339.frag:218`); the rest of `_par` is ignored there |
| smooth skin | pore-level albedo, no baked light | R 0.45 to 0.6; G light cavity; B 0.4 to 0.6 | BC5 or BC7 (shader decides) | needs a normal-capable shader, see section 2 |
| scales, hard shell | scale albedo with the edge darkening baked in | R 0.3 to 0.5; G cavities between scales; B 0.7 to 1.0 | scale relief | BC7 if the shader reads Z or W |
| metal | base tint only | R 0.15 to 0.35; B 1.0; **A = 1** for metal | optional | only shaders that read `.w` (metal shader family) honor A; BC3 or BC7 `_par`, not BC1 |
| plastic-like | flat colors | R 0.25 to 0.4; G 1; B 0.8 | none or mild | |
| rock, bark | albedo with cracks | R 0.7 to 1.0; G deep cracks; B 0.1 to 0.3 | strong relief | |
| plant, leaf, feather | veins or barbs in albedo | R 0.5 to 0.8; G inter-leaf occlusion; B 0.3 | vein or barb relief | the leaf and feather materials use the same shader family; only `backScatter_*` shaders add transmission |
| slime, water-like | saturated albedo | R 0.05 to 0.15 (sharp); B 1.0 | wet ripple | strong reflection needs the cube-map shaders (1,003 of them) |

Values are authoring targets derived from the channel meanings above and the default texture (R 0.61, G 1, B 1, A 0); none was judged on screen. There is no height or depth input to author. A "depth map" can only be baked into the normal map and into `_par` G (occlusion).

Texture encoding:

- BC1, BC3, BC4, BC5 and BC7 are already written by `scripts/hd/nutexb_encode.py` through `etcpak.compress_bc7` (etcpak 0.9.15 lists `compress_bc7`; the wrapper is MIT over the BSD-3 core). `scripts/hd/make_hd_pack.py` hands the format choice to it from the original's footer, so BC7 is available. Not checked here: BC7 output quality against the originals, or a BC7 file loading in game (the originals are BC7, so the format itself is supported). If etcpak's BC7 looks weak on normals, `bc7enc` (MIT / public domain) or Intel's ISPC Texture Compressor (MIT) are drop-in alternatives; neither is installed.
- Normals in BC5 hold X and Y only. A shader that reads Z needs BC7 or BC3.
- sRGB: `_dif` is sRGB, `_par` and `_nor` are linear (checked on Pikachu, Pichu and Bidoof files via the footer flag). The encoder copies this from the original footer (`nutexb_encode.py:6`); keep the original flag.

Test order for a new material (all untested): replace `_dif` and `_par` only (no shader change, always safe); then `_nor` for the seven default-slot materials and the 263 authored ones; last, shader re-pointing for the 490 no-normal materials, with a visual A/B per candidate shader id.

## Material swap mod: result

Tested 2026-11-13 on Dodrio (`CH_007` Body, same program binaries as Bidoof Body) and Bidoof (`CH_032` Body) in `florio_photos`, `SNAP_RENDER_SCALE=2`. A material-file mod works: the game loads a rewritten `.matinstb` and `.numatb`, draws with the normal-mapped twin shader and samples a new normal texture. Runs `1-20261113_082456` (test pattern), `2-20261113_082456` (flat normal), `0-20261113_085038` (no mod): no crash, no shader errors, `[mods] hit` for the matinstb, numatb and the normal texture entry.

**Twin shader (Bidoof / Dodrio Body main `3436703351` -> `747291159`).** It has the same sampler set plus `normalTexture`, identical ParameterList names, and, compared in SPIR-V (`scripts/hd/shader_twin_check.py`), the same lighting model: the uniform components and call histogram match the original and the only additions are the normal fetch, the z rebuild (sqrt), two normalizes, about 21 fma and one extra uniform component (`sssNormalMapWeight`, taken from the shader default when the material omits it). `86799625`, `894430371` and `1012618182` also add exactly that delta; `1564010981` does not. Companion passes: `1059205888` -> `3921109201`, `1019458661` -> `3238596505`, and `_zprepass` renamed with the main id. The vertex shader of the twin also reads TANGENT/BINORMAL, which the meshes carry. The translator compiled the twin on first use (cache key `2f36fb44d5b2`, original `df8356c1e5be`; normal sampler binding b32).

**Recipe** (`scripts/hd/make_material_mod.py`, MIT-clean, writes only to the pack folder):

1. `python -E -P scripts/hd/make_material_mod.py --pack <mods>/<packid> --material CH_007/MI_CH_007_00_0_A00_Body --normal test|flat|<png> --entry-name CH_007_00_1_A00_Body_dif` (`--list` shows candidates; `--twin old=new` overrides; `--size` default 1024).
2. It rewrites the matinstb (four shader strings, long and short forms, plus a new `normalTexture` element cloned from the `paramsTexture` element: slot name, file name, crc32s) and the numatb (main shader string), and writes the normal texture as a nutexb encoded with the format of an existing `_nor` template.
3. Start with `SNAP_MODS_DIR=<mods>`; check `[mods] hit` lines for the three entries.

**Loader limit that shapes the recipe.** A new entry name is never loaded (only entries in the DRP index are served). So the normal texture is written over an existing, always-loaded, otherwise-unused entry (`--entry-name`; used `CH_007_00_1_A00_Body_dif` for Dodrio and `CH_032_00_1_A00_Body_par` for Bidoof, both variant-1 textures) and the matinstb slot names that entry. A loader fix that adds new index entries would remove the decoy; it was not made (another worker owns `runtime/mods/mods.cpp`).

**What was seen** (`analysis/mods/compare/matswap_dodrio_orig.png`, `_flat.png`, `_test.png`; opened and checked; git-ignored game output). Runs are not phase-deterministic, so the three frames are the same camera and scene at slightly different ride poses, not the same present. Original and flat-normal look alike (smooth orange fur-card body). The test pattern shows round bumps over the whole body, beak and claws, so the normal sampler is live and the lighting reacts to it.

**Coverage** (`scripts/hd/material_swap_census.py`, `analysis/materials/swap_census.tsv`, 760 materials, 490 with no normal sampler): 302 have a twin with the same +normal delta (swappable with the recipe); 154 of those share the exact program binaries of Bidoof Body (so the verified triple above applies: Taillow, Wurmple, Dodrio, Bouffalant, Grookey Body2/Body3, Bidoof and more); 114 have a twin with a different delta (check with `shader_twin_check.py` first); 74 have no exact twin. 90 distinct program-binary triples. The matching list per material is in the TSV; batch it with the tool by looping `--material`.

**Risks.** The twin reads `sssNormalMapWeight` from the shader default, which was not tuned per species. The decoy entry is a real variant-1 texture, so any material that uses it (variant 1 of the species) shows the normal image instead; pick decoys per pack. Ship the normal at 1024 or below for BC7 (larger was not tested here). The matinstb/numatb rewrite is validated by the FlatBuffers walker and in game on two materials only. A real authored normal (not a test grid) was not looked at; the flat normal showed no regression by eye.

## Open items

- B channel of `_par` as a specular mask is inferred from data flow. A pixel test (solid B = 0 and B = 1 on a Bidoof body, `SNAP_MODS_AB`) would settle it.
- Normal alpha as a subsurface weight is inferred.
- No non-texture Pokémon mod (matinstb, numatb) has been loaded in game.
- Which of the 51 exact-twin shaders matches a given material's lighting is not known.
