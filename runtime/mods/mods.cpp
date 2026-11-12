// SPDX-License-Identifier: MIT
// Loose-file DRP entry mods: the hook the lifted DRP entry worker calls (project code, ours). Guide: docs/mods.md.
//
// Hook point: main+0x5e3a58, inside the worker at main+0x5e38b0 that decompresses one DRP entry (the only caller of
// OodleLZ_Decompress, main+0x7240, at 0x5e3b84). recomp/snap_gen.py MAIN_INLINE_HOOKS makes the generated code call
// rt_mod_entry(Cpu*) just before the instruction there. At that point (callee-saved registers are always in the Cpu):
//   x20 = the entry's chunk in memory (already decrypted): crc32 of the data, 'resd', total, ext crc, csize, dsize,
//         NUL-terminated name at +0x18, padded to 4 bytes, then the payload
//   x24 = the request object (a std::string with the entry name at +0x10)
// The worker allocates the output buffer from the chunk's dsize, then either memcpy()s the payload (csize == dsize), runs
// Oodle, and finally checks crc32(output, dsize) against the chunk's first word. A replacement therefore needs no other
// patch: the hook swaps x20 for a host-built chunk whose csize == dsize == the file size and whose crc is the file's.
// A larger file just means a larger dsize, so the game allocates what it needs.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

#include "../cpu.h"
#include "mods_core.h"

namespace {

mods::Index g_index;
std::atomic<int> g_state{0};   // 0 = not loaded, 1 = no overrides, 2 = overrides present
std::once_flag g_once;
std::atomic<unsigned> g_hits{0}, g_misses{0}, g_failed{0};

struct HitCount { std::atomic<unsigned> n{0}; };
std::vector<HitCount*> g_counts;   // by index into g_labels
std::vector<const mods::Override*> g_ovr;

void summary() {
  if (g_state.load() != 2) return;
  fprintf(stderr, "[mods] summary: %u replacements served, %u failed, %zu overrides installed\n",
          g_hits.load(), g_failed.load(), g_index.map.size());
  for (size_t i = 0; i < g_ovr.size(); i++)
    if (g_counts[i]->n) fprintf(stderr, "[mods]   %s <- %s x%u\n", g_ovr[i]->label.c_str(), g_index.packs[g_ovr[i]->pack].id.c_str(), g_counts[i]->n.load());
}

void init() {
  std::string root = mods::default_root();
  std::vector<std::string> warn;
  mods::load(root, g_index, warn);
  for (auto& w : warn) fprintf(stderr, "[mods] warning: %s\n", w.c_str());
  for (auto& c : g_index.conflicts) fprintf(stderr, "[mods] conflict: %s\n", c.c_str());
  if (g_index.empty()) {
    if (!g_index.packs.empty()) fprintf(stderr, "[mods] root %s: %zu packs, no overrides\n", root.c_str(), g_index.packs.size());
    g_state = 1;
    return;
  }
  fprintf(stderr, "[mods] root %s: %zu overrides\n", root.c_str(), g_index.map.size());
  for (auto& kv : g_index.map) {
    g_ovr.push_back(&kv.second);
    g_counts.push_back(new HitCount());
    fprintf(stderr, "[mods] %s <- %s\n", kv.second.label.c_str(), g_index.packs[kv.second.pack].id.c_str());
  }
  atexit(summary);
  g_state = 2;
}

std::vector<uint8_t>& scratch() { static thread_local std::vector<uint8_t> v; return v; }

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 31);
  if (ok) {
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (ok && done < out.size()) {
      DWORD got = 0;
      ok = ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 24), &got, nullptr) && got;
      done += got;
    }
  }
  CloseHandle(h);
  return ok;
}

// Structural check of a replacement nutexb (ext id 5C156DBC) so a malformed file falls back to the original instead of
// reaching the renderer. Footer (last 0xb0 bytes): u32 w,h,depth,format,dim,mips,tile,layers,data_size,"XET ",version at
// footer+0x84. Returns a reason string, or nullptr when it looks sane. Sizes are NOT compared with the original (any size is fine).
const char* nutexb_problem(const std::vector<uint8_t>& d) {
  if (d.size() < 0xb0 + 16) return "smaller than a nutexb footer";
  const uint8_t* f = d.data() + d.size() - 0xb0;
  uint32_t v[11];
  std::memcpy(v, f + 0x84, sizeof v);
  if (v[9] != 0x54455820u) return "no XET footer magic";
  uint32_t w = v[0], h = v[1], depth = v[2], dim = v[4], mips = v[5], layers = v[7], dsz = v[8];
  if (!w || !h || w > 16384 || h > 16384) return "bad width or height";
  if (!depth || depth > 2048 || !layers || layers > 64) return "bad depth or layer count";
  if (dim != 2 && dim != 3 && dim != 4) return "bad dimension";
  if (!mips || mips > 15) return "bad mip count";
  if ((w >> (mips - 1)) == 0 && (h >> (mips - 1)) == 0) return "more mips than the size allows";
  if ((uint64_t)dsz + 0xb0 > d.size()) return "data size larger than the file";
  uint64_t sum = 0;
  uint32_t ms[16];
  std::memcpy(ms, f, sizeof ms);
  for (uint32_t i = 0; i < mips; i++) sum += ms[i];
  if (sum == 0 || sum > dsz) return "mip sizes do not fit the data size";
  return nullptr;
}

}  // namespace

// Archive path (normalized, e.g. "common/ui/master/foo.drp") of the entry worker's current chunk; false when unknown.
bool mods_archive_of(Cpu* c, std::string* out);

static std::mutex g_file_mu;
static std::unordered_map<const mods::Override*, std::shared_ptr<const std::vector<uint8_t>>> g_files;

extern "C" void rt_mod_entry(Cpu* c) {
  int st = g_state.load(std::memory_order_acquire);
  if (st == 1) return;
  if (st == 0) { std::call_once(g_once, init); st = g_state.load(); if (st == 1) return; }
  if (g_index.empty()) return;
  const uint8_t* chunk = (const uint8_t*)c->x[20];
  if (!chunk) return;
  std::string archive;
  if (!mods_archive_of(c, &archive)) { if (g_misses++ < 5) fprintf(stderr, "[mods] archive of chunk %s unknown (no recorded read covers it)\n", (const char*)chunk + 0x18); return; }
  const char* name = (const char*)chunk + 0x18;
  size_t nlen = strnlen(name, 256);
  uint32_t extcrc;
  std::memcpy(&extcrc, chunk + 0x0c, 4);
  static const bool trace = getenv("SNAP_MODS_TRACE") != nullptr;   // log every entry the worker sees: archive/name.extcrc
  if (trace) fprintf(stderr, "[mods] entry %s/%s.%08x\n", archive.c_str(), name, extcrc);
  const mods::Override* o = g_index.find(archive, std::string(name, nlen), extcrc);
  if (!o) return;
  auto& buf = scratch();
  // Replacement files are read and validated once, then shared: the game reloads the same entries over and over, and a
  // disk read per hit (up to 22 MB each) made loading about 15x slower with a large mod set.
  std::shared_ptr<const std::vector<uint8_t>> sp;
  {
    std::lock_guard<std::mutex> g(g_file_mu);
    auto f = g_files.find(o);
    if (f != g_files.end()) sp = f->second;
  }
  if (!sp) {
    auto fresh = std::make_shared<std::vector<uint8_t>>();
    if (!read_file(o->file, *fresh)) {
      if (g_failed++ < 20) fprintf(stderr, "[mods] cannot read %s (%s): using the original\n", o->file.c_str(), o->label.c_str());
      return;
    }
    if (extcrc == 0x5c156dbcu) {
      if (const char* why = nutexb_problem(*fresh)) {
        if (g_failed++ < 20) fprintf(stderr, "[mods] skipped %s <- %s: invalid nutexb (%s), using the original\n", o->label.c_str(), g_index.packs[o->pack].id.c_str(), why);
        fresh->clear();   // cache the rejection (empty) so it is not re-read
      }
    }
    sp = fresh;
    std::lock_guard<std::mutex> g(g_file_mu);
    g_files.emplace(o, sp);
  }
  if (sp->empty()) return;
  const std::vector<uint8_t>& data = *sp;
  buf = mods::build_chunk(chunk, mods::chunk_header_len(nlen), data.data(), data.size());
  size_t idx = 0;
  for (; idx < g_ovr.size(); idx++) if (g_ovr[idx] == o) break;
  if (idx < g_ovr.size() && g_counts[idx]->n++ == 0)
    fprintf(stderr, "[mods] hit %s <- %s (%zu bytes)\n", o->label.c_str(), g_index.packs[o->pack].id.c_str(), data.size());
  g_hits++;
  c->x[20] = (uint64_t)(uintptr_t)buf.data();   // held until this thread's next replacement: the worker is done with it by then
}

// ---- which archive is a chunk from? The request object carries only the entry name, so the host remembers, per read
// buffer, the DRP the game read into it (nn::fs::ReadFile taps in runtime/hle/fs.cpp). A chunk pointer lies inside the
// buffer of the read that loaded it; a later read into the same memory replaces the older record.
extern void (*g_snap_fs_tap_open)(uint64_t id, const char* path);
extern void (*g_snap_fs_tap_close)(uint64_t id);
extern void (*g_snap_fs_tap_read)(uint64_t id, const void* buf, size_t n);

namespace {
std::mutex g_tap_mu;
std::map<uint64_t, std::string> g_handles;                           // file handle -> normalized .drp path
struct Range { uint64_t end; std::string archive; };
std::map<uint64_t, Range> g_ranges;                                  // buffer start -> record

void tap_open(uint64_t id, const char* path) {
  std::string a = mods::norm_archive(path);
  if (a.size() < 4 || a.compare(a.size() - 4, 4, ".drp") != 0) return;
  std::lock_guard<std::mutex> g(g_tap_mu);
  g_handles[id] = a;
}
void tap_close(uint64_t id) {
  std::lock_guard<std::mutex> g(g_tap_mu);
  g_handles.erase(id);
}
void tap_read(uint64_t id, const void* buf, size_t n) {
  std::lock_guard<std::mutex> g(g_tap_mu);
  auto h = g_handles.find(id);
  if (h == g_handles.end() || !n) return;
  uint64_t b = (uint64_t)(uintptr_t)buf, e = b + n;
  auto it = g_ranges.lower_bound(b);
  if (it != g_ranges.begin()) { auto p = std::prev(it); if (p->second.end > b) it = p; }
  while (it != g_ranges.end() && it->first < e) it = g_ranges.erase(it);   // overlapped records are stale
  g_ranges[b] = Range{e, h->second};
}

struct Installer {
  Installer() {
    std::call_once(g_once, init);
    if (g_state.load() == 2) { g_snap_fs_tap_open = tap_open; g_snap_fs_tap_close = tap_close; g_snap_fs_tap_read = tap_read; }
  }
} g_installer;
}  // namespace

bool mods_archive_of(Cpu* c, std::string* out) {
  uint64_t a = c->x[20];
  std::lock_guard<std::mutex> g(g_tap_mu);
  auto it = g_ranges.upper_bound(a);
  if (it == g_ranges.begin()) return false;
  --it;
  if (a >= it->second.end) return false;
  *out = it->second.archive;
  return true;
}
