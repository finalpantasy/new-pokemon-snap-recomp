# Pokemon material catalog

Material class and surface notes for every Pokemon in New Pokemon Snap, as input for new HD textures with depth maps. Owner goal (2026-11-12): fur Pokemon get real fur, smooth ones get smooth skin, metal looks metallic, plastic-like ones look like plastic. Keep each Pokemon faithful to its official look. Reference images stay local.

Data: `scripts/hd/pokemon_materials.json` (species facts and ids only, nothing extracted from the game).

## Method

1. Species list and ids come from the game itself. `Poke_Name.msbt` in `TextResource_en.drp` has labels `P<nnn>_Name`. The model and texture ids are `p<nnn>_00` (model `SK_CH_<nnn>_00_0_A00`). `scripts/hd/build_cache.py` already reads this table, so the match is exact. There are 234 species, ids 1 to 271 with gaps (for example 33, 43, 178 and 180 are unused).
2. Dex numbers, material class and surface notes come from the official designs and Pokedex descriptions, written as design judgments. I did not fetch each species page, so confidence is per entry: "high" means the class is not in doubt, "medium" or "low" means a form question (see below).
3. Detective Pikachu (2019) flags come from two published cast lists (Shacknews and ComicBook.com, listed in the JSON sources). Both mix main creatures and brief cameos, so each flagged entry carries `dp_prominence`: "prominent in film" or "brief cameo or background in film". Relatives (Ludicolo for Lotad, Noctowl for Hoothoot, Loudred, Greninja for Sobble) do not count; only the exact species does. The film's look is the reference for fur depth, skin sheen and scale scale.
4. Class codes in the JSON are the full words: fur_short, fur_long, fur_dense_fluffy, smooth_skin, rubbery, scales, feathers, chitin_shell, metal, rock_crystal, plant, slime_water_gas, flame, ice, bone_horn, hair, cloth_ethereal. Gloss: matte, satin, glossy, metallic, emissive or glowing.

## Limits

- Surface notes for the later entries (roughly ids 110 and up) are short; expand them when a species is picked up.
- Forms are not checked: Exeggutor, Sandshrew, Sandslash and Vulpix may be Alolan forms in Snap (medium or low confidence). Vivillon, Sawsbuck, Deerling, Pumpkaboo and Lycanroc have form variants.
- Region names are plain-language guesses. The top-20 species (2026-11-12) now carry a `slot` (real material slot from `info.json`), `slot_guess` (true when the region shares a slot with others, so its place on the texture is a guess) and `textures`. Others still need matching.
- The film list is a union of two published sources and may miss a cameo.

## Counts

Main material by first region: smooth_skin 45, fur_short 39, feathers 24, scales 22, fur_dense_fluffy 20, chitin_shell 17, plant 14, rock_crystal 13, rubbery 12, fur_long 10, slime_water_gas 9, ice 4, metal 3, cloth_ethereal 1, bone_horn 1.

Detective Pikachu references (36): Pikachu, Eevee, Dodrio, Aipom, Magikarp, Emolga, Pidgeot, Feraligatr, Slaking, Comfey, Blastoise, Machamp, Flareon, Snorlax, Octillery, Torterra, Bouffalant, Squirtle, Morelull, Charmander, Bulbasaur, Steelix, Venusaur, Psyduck, Gyarados, Mew, Rattata, Audino, Braviary, Charizard, Graveler, Gengar, Lugia, Joltik, Pancham, Golurk.

## Summary table

| Model id | Species | Dex | Main material | DP reference |
|---|---|---|---|---|
| p001_00 | Pikachu | 25 | fur_short | yes |
| p002_00 | Eevee | 133 | fur_dense_fluffy | yes |
| p003_00 | Heracross | 214 | chitin_shell | no |
| p004_00 | Pinsir | 127 | chitin_shell | no |
| p005_00 | Torchic | 255 | feathers | no |
| p006_00 | Pichu | 172 | fur_short | no |
| p007_00 | Dodrio | 85 | feathers | yes |
| p008_00 | Caterpie | 10 | rubbery | no |
| p009_00 | Lapras | 131 | rubbery | no |
| p010_00 | Swanna | 581 | feathers | no |
| p011_00 | Aipom | 190 | fur_short | yes |
| p012_00 | Bounsweet | 761 | smooth_skin | no |
| p013_00 | Metapod | 11 | chitin_shell | no |
| p014_00 | Beautifly | 267 | scales | no |
| p015_00 | Magikarp | 129 | scales | yes |
| p016_00 | Wurmple | 265 | rubbery | no |
| p017_00 | Ducklett | 580 | feathers | no |
| p018_00 | Wooper | 194 | slime_water_gas | no |
| p019_00 | Emolga | 587 | fur_short | yes |
| p020_00 | Noibat | 714 | fur_dense_fluffy | no |
| p021_00 | Tyrantrum | 697 | scales | no |
| p022_00 | Hippowdon | 450 | smooth_skin | no |
| p023_00 | Wingull | 278 | feathers | no |
| p024_00 | Ninetales | 38 | fur_long | no |
| p025_00 | Cubchoo | 613 | fur_dense_fluffy | no |
| p026_00 | Chandelure | 609 | metal | no |
| p027_00 | Skorupi | 451 | chitin_shell | no |
| p028_00 | Pikipek | 731 | feathers | no |
| p029_00 | Beartic | 614 | fur_long | no |
| p030_00 | Toucannon | 733 | feathers | no |
| p031_00 | Quagsire | 195 | slime_water_gas | no |
| p032_00 | Bidoof | 399 | fur_dense_fluffy | no |
| p034_00 | Vivillon | 666 | scales | no |
| p035_00 | Aerodactyl | 142 | scales | no |
| p036_00 | Flygon | 330 | smooth_skin | no |
| p037_00 | Wailord | 321 | rubbery | no |
| p038_00 | Wailmer | 320 | rubbery | no |
| p039_00 | Slugma | 218 | rock_crystal | no |
| p040_00 | Alomomola | 594 | slime_water_gas | no |
| p041_00 | Mantine | 226 | rubbery | no |
| p042_00 | Mamoswine | 473 | fur_long | no |
| p044_00 | Sawsbuck | 586 | fur_short | no |
| p045_00 | Carbink | 703 | rock_crystal | no |
| p046_00 | Sableye | 302 | smooth_skin | no |
| p047_00 | Luvdisc | 370 | scales | no |
| p048_00 | Pidgeot | 18 | feathers | yes |
| p049_00 | Onix | 95 | rock_crystal | no |
| p050_00 | Exeggutor | 103 | plant | no |
| p051_00 | Feraligatr | 160 | scales | yes |
| p052_00 | Bellossom | 182 | plant | no |
| p053_00 | Slaking | 289 | fur_short | yes |
| p054_00 | Sharpedo | 319 | smooth_skin | no |
| p055_00 | Cacnea | 331 | plant | no |
| p056_00 | Finneon | 456 | scales | no |
| p057_00 | Yanmega | 469 | chitin_shell | no |
| p058_00 | Liepard | 510 | fur_short | no |
| p059_00 | Stunfisk | 618 | slime_water_gas | no |
| p060_00 | Mandibuzz | 630 | feathers | no |
| p061_00 | Florges | 671 | plant | no |
| p062_00 | Primarina | 730 | smooth_skin | no |
| p063_00 | Crabrawler | 739 | chitin_shell | no |
| p064_00 | Lycanroc | 745 | fur_long | no |
| p065_00 | Comfey | 764 | plant | yes |
| p066_00 | Pyukumuku | 771 | slime_water_gas | no |
| p067_00 | Jolteon | 135 | fur_short | no |
| p068_00 | Blastoise | 9 | smooth_skin | yes |
| p069_00 | Raichu | 26 | fur_short | no |
| p070_00 | Vaporeon | 134 | smooth_skin | no |
| p071_00 | Machamp | 68 | smooth_skin | yes |
| p072_00 | Flareon | 136 | fur_dense_fluffy | yes |
| p073_00 | Snorlax | 143 | fur_dense_fluffy | yes |
| p074_00 | Meganium | 154 | smooth_skin | no |
| p075_00 | Umbreon | 197 | fur_short | no |
| p076_00 | Hoothoot | 163 | feathers | no |
| p077_00 | Espeon | 196 | fur_short | no |
| p078_00 | Murkrow | 198 | feathers | no |
| p079_00 | Corsola | 222 | chitin_shell | no |
| p080_00 | Octillery | 224 | smooth_skin | yes |
| p081_00 | Taillow | 276 | feathers | no |
| p082_00 | Pelipper | 279 | feathers | no |
| p083_00 | Zangoose | 335 | fur_short | no |
| p084_00 | Shroomish | 285 | smooth_skin | no |
| p085_00 | Seviper | 336 | scales | no |
| p086_00 | Combee | 415 | chitin_shell | no |
| p087_00 | Torterra | 389 | rock_crystal + plant | yes |
| p088_00 | Clamperl | 366 | chitin_shell | no |
| p089_00 | Vespiquen | 416 | chitin_shell | no |
| p090_00 | Glaceon | 471 | fur_short | no |
| p091_00 | Tangrowth | 465 | plant | no |
| p092_00 | Leafeon | 470 | fur_short | no |
| p093_00 | Drifblim | 426 | rubbery | no |
| p094_00 | Manaphy | 490 | slime_water_gas | no |
| p095_00 | Shaymin | 492 | fur_dense_fluffy | no |
| p096_00 | Foongus | 590 | rubbery | no |
| p097_00 | Bouffalant | 626 | fur_long | yes |
| p098_00 | Inkay | 686 | rubbery | no |
| p099_00 | Sylveon | 700 | fur_short | no |
| p100_00 | Mareanie | 748 | smooth_skin | no |
| p101_00 | Sandygast | 769 | rock_crystal | no |
| p102_00 | Squirtle | 7 | smooth_skin | yes |
| p103_00 | Morelull | 755 | plant | yes |
| p104_00 | Charmander | 4 | smooth_skin | yes |
| p105_00 | Ariados | 168 | chitin_shell | no |
| p106_00 | Bulbasaur | 1 | smooth_skin | yes |
| p107_00 | Steelix | 208 | metal | yes |
| p108_00 | Milotic | 350 | scales | no |
| p109_00 | Volcarona | 637 | fur_dense_fluffy | no |
| p110_00 | Xerneas | 716 | fur_short | no |
| p111_00 | Wishiwashi | 746 | scales | no |
| p112_00 | Grookey | 810 | fur_short | no |
| p113_00 | Scorbunny | 813 | fur_short | no |
| p114_00 | Sobble | 816 | smooth_skin | no |
| p115_00 | Venusaur | 3 | smooth_skin | yes |
| p116_00 | Arbok | 24 | scales | no |
| p117_00 | Sandshrew | 27 | scales | no |
| p118_00 | Diglett | 50 | smooth_skin | no |
| p119_00 | Psyduck | 54 | smooth_skin | yes |
| p120_00 | Koffing | 109 | rubbery | no |
| p121_00 | Kangaskhan | 115 | fur_short | no |
| p122_00 | Gyarados | 130 | scales | yes |
| p123_00 | Mew | 151 | fur_short | yes |
| p124_00 | Ledian | 166 | chitin_shell | no |
| p125_00 | Cleffa | 173 | smooth_skin | no |
| p126_00 | Ursaring | 217 | fur_dense_fluffy | no |
| p127_00 | Tyranitar | 248 | scales | no |
| p128_00 | Swalot | 317 | smooth_skin | no |
| p129_00 | Trapinch | 328 | chitin_shell | no |
| p130_00 | Tropius | 357 | scales | no |
| p131_00 | Swampert | 260 | smooth_skin | no |
| p132_00 | Shinx | 403 | fur_short | no |
| p133_00 | Gliscor | 472 | smooth_skin | no |
| p134_00 | Tepig | 498 | smooth_skin | no |
| p135_00 | Drilbur | 529 | fur_short | no |
| p136_00 | Scolipede | 545 | chitin_shell | no |
| p137_00 | Crustle | 558 | rock_crystal | no |
| p138_00 | Heliolisk | 695 | smooth_skin | no |
| p139_00 | Rockruff | 744 | fur_long | no |
| p140_00 | Salazzle | 758 | smooth_skin | no |
| p141_00 | Minior | 774 | rock_crystal | no |
| p142_00 | Zeraora | 807 | fur_short | no |
| p143_00 | Rattata | 19 | fur_short | yes |
| p144_00 | Sandslash | 28 | scales | no |
| p145_00 | Vulpix | 37 | fur_long | no |
| p146_00 | Meowth | 52 | fur_short | no |
| p147_00 | Dewgong | 87 | fur_short | no |
| p148_00 | Jynx | 124 | smooth_skin | no |
| p149_00 | Furret | 162 | fur_short | no |
| p150_00 | Sudowoodo | 185 | plant | no |
| p151_00 | Swinub | 220 | fur_dense_fluffy | no |
| p152_00 | Delibird | 225 | feathers | no |
| p153_00 | Skarmory | 227 | metal | no |
| p154_00 | Suicune | 245 | smooth_skin | no |
| p155_00 | Mightyena | 262 | fur_short | no |
| p156_00 | Snorunt | 361 | smooth_skin | no |
| p157_00 | Glalie | 362 | ice | no |
| p158_00 | Spheal | 363 | fur_dense_fluffy | no |
| p159_00 | Piplup | 393 | feathers | no |
| p160_00 | Starly | 396 | feathers | no |
| p161_00 | Abomasnow | 460 | fur_long | no |
| p162_00 | Weavile | 461 | fur_short | no |
| p163_00 | Froslass | 478 | ice | no |
| p164_00 | Stoutland | 508 | fur_long | no |
| p165_00 | Audino | 531 | fur_short | yes |
| p166_00 | Trubbish | 568 | rubbery | no |
| p167_00 | Vanilluxe | 583 | ice | no |
| p168_00 | Braviary | 628 | feathers | yes |
| p169_00 | Bunnelby | 659 | fur_short | no |
| p170_00 | Aurorus | 699 | smooth_skin | no |
| p171_00 | Dedenne | 702 | fur_short | no |
| p172_00 | Avalugg | 713 | ice | no |
| p173_00 | Crabominable | 740 | fur_long | no |
| p174_00 | Cutiefly | 742 | fur_dense_fluffy | no |
| p175_00 | Charizard | 6 | scales | yes |
| p176_00 | Clefairy | 35 | smooth_skin | no |
| p177_00 | Tentacruel | 73 | slime_water_gas | no |
| p179_00 | Geodude | 74 | rock_crystal | no |
| p181_00 | Graveler | 75 | rock_crystal | yes |
| p184_00 | Gengar | 94 | fur_dense_fluffy | yes |
| p185_00 | Starmie | 121 | chitin_shell | no |
| p187_00 | Typhlosion | 157 | fur_short | no |
| p188_00 | Crobat | 169 | smooth_skin | no |
| p189_00 | Lanturn | 171 | smooth_skin | no |
| p191_00 | Chinchou | 170 | smooth_skin | no |
| p192_00 | Natu | 177 | feathers | no |
| p193_00 | Qwilfish | 211 | smooth_skin | no |
| p194_00 | Houndoom | 229 | fur_short | no |
| p196_00 | Lugia | 249 | feathers | yes |
| p197_00 | Lotad | 270 | smooth_skin | no |
| p198_00 | Ho-Oh | 250 | feathers | no |
| p199_00 | Shiftry | 275 | plant | no |
| p200_00 | Celebi | 251 | smooth_skin | no |
| p202_00 | Mawile | 303 | smooth_skin | no |
| p203_00 | Torkoal | 324 | rock_crystal | no |
| p204_00 | Gardevoir | 282 | cloth_ethereal | no |
| p207_00 | Altaria | 334 | feathers | no |
| p209_00 | Cradily | 346 | plant | no |
| p210_00 | Kecleon | 352 | scales | no |
| p216_00 | Jirachi | 385 | smooth_skin | no |
| p217_00 | Monferno | 391 | fur_short | no |
| p218_00 | Luxray | 405 | fur_short | no |
| p219_00 | Rampardos | 409 | bone_horn | no |
| p220_00 | Drifloon | 425 | rubbery | no |
| p222_00 | Croagunk | 453 | smooth_skin | no |
| p223_00 | Lumineon | 457 | scales | no |
| p228_00 | Serperior | 497 | scales | no |
| p230_00 | Unfezant | 521 | feathers | no |
| p231_00 | Woobat | 527 | fur_dense_fluffy | no |
| p234_00 | Sigilyph | 561 | rock_crystal | no |
| p235_00 | Archeops | 567 | feathers | no |
| p238_00 | Frillish | 592 | slime_water_gas | no |
| p239_00 | Joltik | 595 | fur_dense_fluffy | yes |
| p242_00 | Beheeyem | 606 | smooth_skin | no |
| p244_00 | Hydreigon | 635 | smooth_skin | no |
| p246_00 | Pancham | 674 | fur_short | yes |
| p247_00 | Talonflame | 663 | feathers | no |
| p249_00 | Espurr | 677 | fur_dense_fluffy | no |
| p251_00 | Clawitzer | 693 | chitin_shell | no |
| p252_00 | Goodra | 706 | slime_water_gas | no |
| p253_00 | Trevenant | 709 | plant | no |
| p254_00 | Diancie | 719 | rock_crystal | no |
| p256_00 | Salandit | 757 | smooth_skin | no |
| p257_00 | Bewear | 760 | fur_dense_fluffy | no |
| p258_00 | Golisopod | 768 | chitin_shell | no |
| p260_00 | Drampa | 780 | fur_dense_fluffy | no |
| p262_00 | Absol | 359 | fur_short | no |
| p263_00 | Deerling | 585 | fur_short | no |
| p264_00 | Golurk | 623 | rock_crystal | yes |
| p265_00 | Noivern | 715 | fur_dense_fluffy | no |
| p266_00 | Eldegoss | 829 | plant | no |
| p267_00 | Applin | 840 | plant | no |
| p268_00 | Silicobra | 843 | scales | no |
| p269_00 | Snom | 872 | smooth_skin | no |
| p270_00 | Frosmoth | 873 | fur_dense_fluffy | no |
| p271_00 | Pumpkaboo | 710 | plant | no |

## First 20 to do

Chosen by how often the player photographs them, screen time, and how flat they look today. Pikachu, Pichu, Bidoof, Grookey, Scorbunny, Bouffalant and Vivillon are already in the preview cache, so the pipeline is proven there.

1. Pikachu (fur_short, DP prominent): the most on screen and the clearest film reference.
2. Bidoof (fur_dense_fluffy): common early target, flat brown fur.
3. Eevee (fur_dense_fluffy plus a long fur ruff, DP): fur depth needed.
4. Snorlax (fur_dense_fluffy and smooth belly, DP): huge on screen.
5. Flareon (fur_dense_fluffy, DP prominent): ruff and tail fur.
6. Charizard (scales, DP prominent): scale and wing membrane.
7. Torterra (rock and plant, DP prominent): moss, bark and rock need depth.
8. Machamp (smooth_skin plus metal belt, DP prominent): skin sheen and belt metal.
9. Gengar (fur_dense_fluffy, DP prominent): soft fuzz.
10. Bulbasaur (smooth_skin plus plant, DP prominent): skin pattern and bulb.
11. Squirtle (smooth_skin plus hard shell, DP prominent): shell versus skin.
12. Pancham (fur_short, DP prominent): fur and leaf.
13. Bouffalant (fur_long, DP): wool afro needs depth.
14. Aipom (fur_short plus skin tail, DP): fur contrast.
15. Mew (fur_short fine, DP): very fine pink fur.
16. Steelix (metal, DP): the clearest metal case.
17. Magikarp (scales, DP): glossy pearly scales.
18. Ninetales (fur_long): silky long fur, flat today.
19. Beartic (fur_long): shaggy fur plus ice.
20. Mamoswine (fur_long): shaggy fur plus tusks.

Later: Chandelure (metal and flame), Skarmory (metal), Trubbish and Foongus (plastic-like), Carbink and Diancie (crystal), the water Pokemon with smooth skin.
