#!/usr/bin/env python3
"""Place a replacement file at the right mod path for one DRP entry (code only; no game data inside).

  python -I scripts/mods/make_override.py <extracted entry> <replacement> [--pack NAME] [--mods-root DIR] [--dry-run]

<extracted entry>  a file under extracted/drp-v262144/ (the output of scripts/drp_unpack.py), e.g.
                   extracted/drp-v262144/common/UI/master/Foo.drp/icon_a.nutexb
<replacement>      your decompressed file. Same format as the original; it may be any size (docs/mods.md).
Result: <mods root>/<pack>/<archive path>/<entry file name>, plus a pack.json when the pack is new.
Mods root: --mods-root, else SNAP_MODS_DIR, else %APPDATA%\\SnapRecomp\\mods. Pack name default: "my-pack".
"""
import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXTRACTED = ROOT / "extracted" / "drp-v262144"


def mod_relpath(entry: Path, extracted: Path = EXTRACTED) -> str:
    """'<archive>.drp/<entry file>' (forward slashes) for a path below the extraction root."""
    rel = entry.resolve().relative_to(extracted.resolve()).as_posix()
    parts = rel.split("/")
    if len(parts) < 2 or not any(p.lower().endswith(".drp") for p in parts[:-1]):
        raise ValueError("not a DRP entry path (expected <...>.drp/<entry file>): " + rel)
    if parts[-2].lower().endswith(".drp") is False:
        raise ValueError("entry must sit directly inside its .drp folder: " + rel)
    return rel


def default_root() -> Path:
    env = os.environ.get("SNAP_MODS_DIR")
    if env:
        return Path(env)
    return Path(os.environ.get("APPDATA", ".")) / "SnapRecomp" / "mods"


def place(entry: Path, replacement: Path, pack_dir: Path, extracted: Path = EXTRACTED, dry_run: bool = False):
    rel = mod_relpath(entry, extracted)
    notes = []
    name = rel.rsplit("/", 1)[1]
    if re.search(r"__\d+\.[^.]+$", name):
        notes.append("this entry has a duplicate name in its archive (the unpacker added __<index>); the loader cannot tell "
                     "duplicates apart, so the override applies to every entry with that name and type")
    if replacement.suffix.lower() != entry.suffix.lower():
        notes.append(f"replacement extension {replacement.suffix} differs from {entry.suffix}; the file is stored under the original name")
    dest = pack_dir / rel
    if not dry_run:
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(replacement, dest)
        pj = pack_dir / "pack.json"
        if not pj.exists():
            pj.write_text(json.dumps({"name": pack_dir.name, "version": "1.0.0", "author": "", "description": ""}, indent=2) + "\n", encoding="utf-8")
    return dest, notes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("entry", type=Path)
    ap.add_argument("replacement", type=Path)
    ap.add_argument("--pack", default="my-pack")
    ap.add_argument("--mods-root", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if not a.replacement.is_file():
        print("replacement not found:", a.replacement, file=sys.stderr)
        return 2
    try:
        dest, notes = place(a.entry, a.replacement, (a.mods_root or default_root()) / a.pack, dry_run=a.dry_run)
    except ValueError as e:
        print("error:", e, file=sys.stderr)
        return 2
    except OSError as e:
        print("error:", e, file=sys.stderr)
        return 1
    for n in notes:
        print("note:", n)
    print(("would write " if a.dry_run else "wrote ") + str(dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
