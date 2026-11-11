#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

HEADER_SIZE = 0x10
ENTRY_SIZE = 0x18


def read_string(data, offset):
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated PFS0 filename")
    return data[offset:end].decode("utf-8")


def inventory(path):
    with path.open("rb") as source:
        header = source.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE:
            raise ValueError("truncated PFS0 header")
        magic, count, strings_size, _ = struct.unpack("<4sIII", header)
        if magic != b"PFS0":
            raise ValueError(f"expected PFS0 magic, got {magic!r}")
        entries_data = source.read(count * ENTRY_SIZE)
        strings = source.read(strings_size)
        if len(entries_data) != count * ENTRY_SIZE or len(strings) != strings_size:
            raise ValueError("truncated PFS0 metadata")
        data_offset = HEADER_SIZE + len(entries_data) + strings_size
        entries = []
        for index in range(count):
            offset, size, name_offset, _ = struct.unpack_from("<QQII", entries_data, index * ENTRY_SIZE)
            name = read_string(strings, name_offset)
            entries.append({
                "index": index,
                "name": name,
                "offset": data_offset + offset,
                "size": size,
            })
        return entries


def extract(path, entry, output, overwrite):
    if output.exists() and not overwrite:
        raise FileExistsError(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".part")
    digest = hashlib.sha256()
    remaining = entry["size"]
    try:
        with path.open("rb") as source, temporary.open("xb") as destination:
            source.seek(entry["offset"])
            while remaining:
                chunk = source.read(min(8 * 1024 * 1024, remaining))
                if not chunk:
                    raise ValueError("truncated PFS0 member")
                destination.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
        if output.exists():
            if not overwrite:
                raise FileExistsError(f"output already exists: {output}")
            os.replace(temporary, output)
        else:
            temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return {**entry, "output": str(output), "sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description="List or selectively extract an uncompressed PFS0/NSP member")
    parser.add_argument("command", choices=("list", "extract"))
    parser.add_argument("input", type=Path)
    parser.add_argument("member", nargs="?")
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    entries = inventory(args.input)
    if args.command == "list":
        print(json.dumps(entries, indent=2))
        return
    if args.member is None or args.output is None:
        parser.error("extract requires member and output")
    matches = [entry for entry in entries if entry["name"] == args.member]
    if len(matches) != 1:
        raise ValueError(f"expected one member named {args.member!r}, found {len(matches)}")
    print(json.dumps(extract(args.input, matches[0], args.output, args.overwrite), indent=2))


if __name__ == "__main__":
    main()
