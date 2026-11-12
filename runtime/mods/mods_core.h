// SPDX-License-Identifier: MIT
// Loose-file DRP entry mods (project code, ours): pack discovery, the (archive, entry) index, path mapping and the
// replacement-chunk builder. Host-only, no dependency on the lifted game; tests/mods/mods_tests.cpp covers it.
// Design and user guide: docs/mods.md.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mods {

struct Pack {
  std::string id;            // folder name under the mods root
  std::string dir;           // absolute path
  std::string name, version, author, description;
  bool enabled = true;
};

struct Override {
  std::string file;          // absolute path of the replacement
  int pack = 0;              // index into Index::packs
  std::string label;         // "archive/entry" for logs
};

uint32_t crc32(const void* data, size_t n);
// DRP entry extension id: crc32 of the lowercase extension (verified: "nutexb" -> 0x5c156dbc). An 8-digit hex extension
// (the unpacker's name for ids it does not know) is taken as the id itself.
uint32_t ext_crc(const std::string& ext);
// "Common/UI\\x.drp" or "rom:/common/UI/x.drp" -> "common/ui/x.drp"
std::string norm_archive(const std::string& a);
// A path inside a pack ("common/UI/master/Foo.drp/icon.nutexb", forward slashes) -> archive, entry stem and ext id. The
// first path component that ends in ".drp" ends the archive path; exactly one file name must follow it. A trailing
// ".<ext>" is dropped from the stem when it repeats the extension (the game stores some names with their extension).
bool parse_pack_path(const std::string& rel, std::string* archive, std::string* stem, uint32_t* extcrc);
std::string make_key(const std::string& archive_norm, const std::string& stem_lower, uint32_t extcrc);

class Index {
 public:
  std::vector<Pack> packs;
  std::unordered_map<std::string, Override> map;
  std::vector<std::string> conflicts;   // "key: pack A beats pack B"
  bool empty() const { return map.empty(); }
  void add(int pack, const std::string& archive, const std::string& stem, uint32_t extcrc, const std::string& file);
  // `name` is the entry name as stored in the chunk; the plain name and the name minus a ".<ext>" suffix whose crc is
  // the chunk's ext id are both tried.
  const Override* find(const std::string& archive_norm, const std::string& name, uint32_t extcrc) const;
};

// Mods root: SNAP_MODS_DIR, else %APPDATA%\SnapRecomp\mods.
std::string default_root();
// Reads <root>/mods.json (priority order, "enabled" flags; without the file every folder with a pack.json is enabled,
// in name order), each pack.json, and indexes the files of enabled packs. Problems go to `warnings`.
void load(const std::string& root, Index& out, std::vector<std::string>& warnings);

// Replacement chunk for the DRP entry worker: the original chunk's header (words, name and padding, `hdr_len` bytes)
// followed by the raw data, stored the way the game stores uncompressed entries (csize == dsize) with the data's CRC32.
// The size fields are rewritten, so the data may be any length.
std::vector<uint8_t> build_chunk(const uint8_t* orig, size_t hdr_len, const uint8_t* data, size_t n);
// Header length of a chunk with a NUL-terminated name of `name_len` bytes at +0x18: 0x18 + ((len + 4) & ~3)
size_t chunk_header_len(size_t name_len);

}  // namespace mods
