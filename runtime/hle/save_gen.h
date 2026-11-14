// SPDX-License-Identifier: MIT
// The on-disk save layout's validity rules, shared by the save mount (fs_core.cpp), profile copy/import (profiles.cpp) and the
// legacy data-root migration (hle_config.cpp). Header-only so every user agrees without a link dependency.
//   <uid>/CURRENT          "SNAPSAVE1\n<n>\n<check>\n", the published generation
//   <uid>/CURRENT.tmp      the same, mid-replace (valid only when CURRENT is missing or torn)
//   <uid>/gens/<n>/        generation n
//   <uid>/gens/<n>.ok      written last when generation n is complete
//   <uid>/gens/<n>.orphan  generation n was never published (a failed commit or removal could not delete its .ok): never valid
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace hle::savegen {

namespace sfs = std::filesystem;

inline uint32_t check_of(uint64_t n) { return (uint32_t)(n * 2654435761u) ^ 0x534e4150u; }
inline std::string pointer_text(uint64_t n) { return "SNAPSAVE1\n" + std::to_string(n) + "\n" + std::to_string(check_of(n)) + "\n"; }

inline bool read_pointer_file(const sfs::path& p, uint64_t* n) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::string magic;
  unsigned long long g = 0, chk = 0;
  if (!(f >> magic >> g >> chk) || magic != "SNAPSAVE1" || (uint32_t)chk != check_of(g)) return false;
  *n = g;
  return true;
}

inline sfs::path gen_dir(const sfs::path& uid, uint64_t n) { return uid / "gens" / std::to_string(n); }
inline sfs::path gen_ok(const sfs::path& uid, uint64_t n) { return uid / "gens" / (std::to_string(n) + ".ok"); }
inline sfs::path gen_orphan(const sfs::path& uid, uint64_t n) { return uid / "gens" / (std::to_string(n) + ".orphan"); }

// Complete and not orphaned. An orphan mark whose state cannot be read counts as present.
inline bool gen_valid(const sfs::path& uid, uint64_t n) {
  std::error_code ec;
  if (!sfs::exists(gen_ok(uid, n), ec) || ec) return false;
  if (!sfs::is_directory(gen_dir(uid, n), ec) || ec) return false;
  return sfs::symlink_status(gen_orphan(uid, n), ec).type() == sfs::file_type::not_found;
}

// Newest valid generation (any number); `skip(n)` excludes more (the mount's in-memory orphans).
template <class Skip>
inline bool newest_valid(const sfs::path& uid, uint64_t* out, Skip skip) {
  std::error_code ec;
  bool found = false;
  for (sfs::directory_iterator it(uid / "gens", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".ok") continue;
    const auto stem = it->path().stem().u8string();
    if (stem.empty() || stem.find_first_not_of(u8"0123456789") != std::u8string::npos) continue;
    const uint64_t n = std::strtoull(std::string(stem.begin(), stem.end()).c_str(), nullptr, 10);
    if (!skip(n) && gen_valid(uid, n) && (!found || n > *out)) { *out = n; found = true; }
  }
  return found;
}
inline bool newest_valid(const sfs::path& uid, uint64_t* out) { return newest_valid(uid, out, [](uint64_t) { return false; }); }

// The generation a mount would load: CURRENT if it names a valid generation, else a valid CURRENT.tmp (an interrupted
// replace), else the newest valid generation. `pointed` tells whether a pointer named it.
template <class Skip>
inline bool committed_gen(const sfs::path& uid, uint64_t* out, bool* pointed, Skip skip) {
  uint64_t n = 0;
  if (pointed) *pointed = true;
  if (read_pointer_file(uid / "CURRENT", &n) && !skip(n) && gen_valid(uid, n)) { *out = n; return true; }
  if (read_pointer_file(uid / "CURRENT.tmp", &n) && !skip(n) && gen_valid(uid, n)) { *out = n; return true; }
  if (pointed) *pointed = false;
  return newest_valid(uid, out, skip);
}
inline bool committed_gen(const sfs::path& uid, uint64_t* out, bool* pointed = nullptr) {
  return committed_gen(uid, out, pointed, [](uint64_t) { return false; });
}

// A save root (<root>/<uid>/...) holds a save when some <uid>/ has a loadable generation.
inline bool has_save(const sfs::path& root) {
  std::error_code ec;
  for (sfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e1;
    uint64_t n = 0;
    if (it->is_directory(e1) && !e1 && committed_gen(it->path(), &n)) return true;
  }
  return false;
}

}  // namespace hle::savegen
