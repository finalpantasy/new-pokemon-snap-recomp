#!/bin/bash
# Builds the patched translator source tree build/xlate/<name>/ryujinx from the unpatched Ryubing clone plus the selected
# patches in scripts/xlate_patches (adapted from the Astral Chain PC recomp, MIT; see THIRD_PARTY_NOTICES.md).
#   bash scripts/xlate_patch.sh r2 0001 0002 0003 0005 0006 0007 0009
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
name="$1"; shift
d="$root/build/xlate/$name/ryujinx"
rm -rf "$d"; mkdir -p "$d/src"
for x in Ryujinx.Graphics.Shader Ryujinx.Common Spv.Generator; do cp -r "$root/tools/ref/ryubing-ryujinx/src/$x" "$d/src/"; done
cp "$root"/tools/ref/ryubing-ryujinx/Directory.* "$d/"
rm -rf "$d"/src/*/obj "$d"/src/*/bin
cd "$d"
for n in "$@"; do patch -p1 --binary -s < "$root"/scripts/xlate_patches/$n-*.patch; echo "applied $n"; done
