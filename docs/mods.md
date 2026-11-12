# Mods: loose-file DRP entry replacement

A mod replaces single entries inside the game's DRP archives (textures, models, FlatBuffers registries, Lua, UI, text) by
dropping the decompressed file into a pack folder. No archive is repacked. Everything is local: the repo holds the loader,
never game data or mod payloads made from it.

## Folder layout

Mods root: `%APPDATA%\SnapRecomp\mods\`, or the folder named by the `SNAP_MODS_DIR` environment variable.

```
mods\
  mods.json                      priority order and enabled flags (optional)
  my-hd-pack\
    pack.json                    name, version, author, description
    common\UI\master\Foo.drp\    <- the archive path, exactly as under rom:/ (the folder name ends in .drp)
      icon_a.nutexb              <- one decompressed entry, named like the extracted file
```

The path below the pack folder is `<archive path>/<entry file>`, the same layout as
`extracted\drp-v262144\` (written by `scripts\drp_unpack.py`). The first folder whose name ends in `.drp` is the
archive; the file inside it is the entry. Files elsewhere in a pack (`pack.json`, readme, screenshots) are ignored.

How an entry is matched (case-insensitive):

- **Archive**: the path up to and including the `.drp` folder.
- **Name**: the file name without its last extension (`icon_a`). A name the game stores with its extension
  (`foo.nutexb`) is matched either way.
- **Type**: the extension, mapped to the game's extension id (crc32 of the lowercase extension; `nutexb` -> `5C156DBC`).
  An 8-digit hex extension (the unpacker's name for types it does not know, e.g. `DC4A8177`) is used as the id directly.

Limit: the unpacker names an entry whose name repeats inside one archive `name__<index>.ext`. The loader cannot tell such
entries apart, so that file overrides every entry with the same name and type in that archive.

## pack.json

```json
{ "name": "Magenta UI test", "version": "1.0.0", "author": "you", "description": "What it changes." }
```

All fields are optional text. The folder name is the pack id used in `mods.json` and in the log.

## mods.json and priority

```json
{ "packs": [
    { "id": "my-hd-pack", "enabled": true },
    { "id": "ui-tweaks",  "enabled": false },
    "older-pack"
] }
```

- The list is in priority order: the first pack wins when two packs replace the same entry. The loser is named in the log
  (`[mods] conflict: ...`).
- A plain string means enabled. A pack that is listed but missing, or on disk but not listed, is skipped (a warning is
  logged for the first case).
- Without a `mods.json`, every folder that contains a `pack.json` is enabled, in name order.
- Changes apply at the next game start. There is no hot reload.

## Making an override

1. Find the entry under `extracted\drp-v262144\` (the manifest `analysis\romfs\drp_unpacked.tsv` lists every entry).
2. Make your replacement in the same format as the original. For a texture, a `nutexb` with the same BC/ASTC format, or
   any new size you like (below).
3. Place it:

   ```
   python -I scripts\mods\make_override.py extracted\drp-v262144\common\UI\master\Foo.drp\icon_a.nutexb my_icon.nutexb --pack my-hd-pack
   ```

   This writes `%APPDATA%\SnapRecomp\mods\my-hd-pack\common\UI\master\Foo.drp\icon_a.nutexb` (and a starter `pack.json`).
   `--mods-root` and `--dry-run` are available.
4. Start the game and read the log (stderr, or the trace of `host\run_boot.ps1`).

Log lines:

```
[mods] root C:\Users\...\SnapRecomp\mods: 1 overrides
[mods] common/ui/master/foo.drp/icon_a <- my-hd-pack            (every override, once, at startup)
[mods] hit common/ui/master/foo.drp/icon_a <- my-hd-pack (4096 bytes)   (first load of each entry)
[mods] summary: 1 replacements served, 0 failed, 1 overrides installed  (at exit)
```

## Larger or smaller replacements

The replacement may be any size. The loader's output buffer is allocated from the entry's size field after the hook runs,
and the game then checks the data against a CRC32 that the hook recomputes, so an HD texture 4x the original just
allocates 4x the memory. Whether the rest of the game copes is a content question: a `nutexb` carries its own width,
height and mip sizes, so a self-consistent bigger texture works. Data whose size is fixed elsewhere (tables that other
code indexes by the original length) must keep its size.

## How it works (for developers)

- `recomp/snap_gen.py` `MAIN_INLINE_HOOKS` makes the generated code call `rt_mod_entry(Cpu*)` just before `main+0x5e3a58`,
  inside the DRP entry worker (`main+0x5e38b0`, the only caller of `OodleLZ_Decompress`). Regenerating `main` is needed
  once after pulling this change (`recomp/snap_gen.py --module main`, then `recomp/snap_build.py`).
- The hook (`runtime/mods/mods.cpp`) finds the archive and entry name, looks them up in a hash set built at startup
  (`runtime/mods/mods_core.cpp`), and on a hit points the worker at a host-built chunk: the original header with csize =
  dsize = file size, the file's CRC32, and the file as an uncompressed payload. With no packs installed the hook is one
  atomic load and a return.
- Tests: `tests\mods\run_tests.ps1` (path mapping, index priority, size override) and
  `python -I tests\mods\test_make_override.py`.
