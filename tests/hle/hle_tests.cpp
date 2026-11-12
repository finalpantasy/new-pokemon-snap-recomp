// SPDX-License-Identifier: MIT
// Host-only tests for runtime/hle: no lifted game, no GPU, no real controller. Overrides are called the way the
// lifted code calls them: through the registry by mangled symbol, with a hand-built Cpu.
//   fs    : RomFS reads checked against SHA-256 in analysis/romfs/files.tsv, layered overlay, result codes
//   save  : write / commit / reload, crash mid-write, crash at every commit stage, backups and restore
//   hid   : fake gamepad / keyboard / mouse through the JSON mapping into Npad and touch states
//   input : binding resolution, slot editing, input.json round trip / migration / invalid fallback, capture, toggle, suppression
//   misc  : registry, account, oe, settings, time, album PNG, unimplemented backstop
// Usage: hle_tests [project_root]   (default: three levels above this file)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../runtime/hle/fs_core.h"
#include "../../runtime/hle/hle.h"
#include "../../runtime/hle/hle_config.h"
#include "../../runtime/hle/input_core.h"
#include "../../runtime/hle/profiles.h"
#include "../../runtime/hle/settings.h"

namespace stdfs = std::filesystem;
using namespace hle;
namespace nvn { int g_presentCount = 0; }   // input_core.cpp reads the nvn frame clock; the renderer is not linked into this test
#include "../../runtime/os/os.h"
extern "C" void os_get_stats(OsStats* o) { *o = OsStats{}; }   // runtime/os/perf.cpp (linked for fs_core's counters) reads the os layer, which is not linked here


// ---- tiny test framework
static int g_checks = 0, g_failed = 0;
static const char* g_test = "";
#define CHECK(cond)                                                                      \
  do {                                                                                   \
    ++g_checks;                                                                          \
    if (!(cond)) { ++g_failed; std::printf("  FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond); } \
  } while (0)
#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    ++g_checks;                                                                          \
    auto va = (a); auto vb = (b);                                                        \
    if (!(va == vb)) { ++g_failed; std::printf("  FAIL [%s] %s:%d: %s == %s (got %lld vs %lld)\n", g_test, __FILE__, __LINE__, #a, #b, (long long)va, (long long)vb); } \
  } while (0)

// ---- SHA-256
struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  size_t blen = 0;
  uint64_t total = 0;
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total += n;
    while (n) {
      size_t k = std::min(n, 64 - blen);
      std::memcpy(buf + blen, p, k);
      blen += k; p += k; n -= k;
      if (blen == 64) { block(buf); blen = 0; }
    }
  }
  std::string hex() {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (blen != 56) update(&z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(l, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
  }
};

// ---- helpers
static stdfs::path g_root, g_tmp;

static std::string slurp(const stdfs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

// Calls an override by mangled name with up to 8 integer arguments. Returns x0.
static uint64_t call(const char* sym, std::initializer_list<uint64_t> args = {}) {
  GuestFn fn = find_override(sym);
  if (!fn) { std::printf("  FAIL [%s] no override for %s\n", g_test, sym); ++g_failed; ++g_checks; return ~0ull; }
  Cpu c{};
  int i = 0;
  for (uint64_t a : args) c.x[i++] = a;
  fn(&c);
  return c.x[0];
}
static uint64_t P(const void* p) { return (uint64_t)(uintptr_t)p; }

// guest-style fs wrappers
static const char* kOpenFile = "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci";
static const char* kCloseFile = "_ZN2nn2fs9CloseFileENS0_10FileHandleE";
static const char* kReadFile = "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm";
static const char* kGetFileSize = "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE";

static bool read_whole_via_shims(const std::string& path, std::string* data, uint32_t* rc_out = nullptr) {
  uint64_t h = 0;
  uint32_t rc = (uint32_t)call(kOpenFile, {P(&h), P(path.c_str()), 1});
  if (rc_out) *rc_out = rc;
  if (rc) return false;
  int64_t size = 0;
  CHECK_EQ(call(kGetFileSize, {P(&size), h}), 0u);
  data->assign((size_t)size, '\0');
  // strict 4-argument ReadFile, in 1 MiB steps like the game's generic reader
  for (int64_t off = 0; off < size; off += 1 << 20) {
    size_t n = (size_t)std::min<int64_t>(1 << 20, size - off);
    uint32_t r = (uint32_t)call(kReadFile, {h, (uint64_t)off, P(&(*data)[(size_t)off]), n});
    if (r) { CHECK_EQ(r, 0u); break; }
  }
  call(kCloseFile, {h});
  return true;
}

static std::string rel_slash(std::string s) { for (auto& ch : s) if (ch == '\\') ch = '/'; return s; }

// ---- fs: RomFS
static void test_fs_rom() {
  g_test = "fs.rom";
  config().romfs_root = (g_root / "extracted" / "romfs-v262144").string();
  config().rom_layers.clear();
  fs::instance().set_rom_layers({config().romfs_root});
  CHECK_EQ(call("_ZN2nn2fs8MountRomEPKcPvm", {P("rom"), 0, 0x10000}), 0u);

  // hash files listed in files.tsv: the known one first, then a sample across the tree
  std::ifstream tsv(g_root / "analysis" / "romfs" / "files.tsv");
  CHECK(tsv.good());
  std::string line;
  std::getline(tsv, line);  // header
  std::vector<std::vector<std::string>> rows;
  std::map<std::string, int> dir_counts;
  while (std::getline(tsv, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) f.push_back(cell);
    if (f.size() < 3) continue;
    rows.push_back(f);
    size_t slash = f[0].rfind('/');
    dir_counts[slash == std::string::npos ? "" : f[0].substr(0, slash)]++;
  }
  CHECK(rows.size() > 100);
  const std::string known = "common/Text/master/TextResource_en.drp";
  std::vector<size_t> pick;
  for (size_t i = 0; i < rows.size(); ++i) if (rows[i][0] == known) pick.push_back(i);
  CHECK_EQ(pick.size(), 1u);
  for (size_t i = 0, step = std::max<size_t>(1, rows.size() / 40); i < rows.size(); i += step)
    if (std::stoll(rows[i][1]) < (8ll << 20)) pick.push_back(i);
  int hashed = 0;
  for (size_t i : pick) {
    std::string data;
    if (!read_whole_via_shims("rom:/" + rows[i][0], &data)) { CHECK(false); continue; }
    Sha256 sha;
    sha.update(data.data(), data.size());
    bool ok = sha.hex() == rows[i][2] && (long long)data.size() == std::stoll(rows[i][1]);
    CHECK(ok);
    if (!ok) std::printf("  mismatch: %s\n", rows[i][0].c_str());
    ++hashed;
  }
  std::printf("  hashed %d RomFS files via nn::fs shims (incl. %s)\n", hashed, known.c_str());

  // directory enumeration: count and names for one directory agree with files.tsv
  uint64_t d = 0;
  CHECK_EQ(call("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", {P(&d), P("rom:/common/Text/master"), 3}), 0u);
  int64_t n = 0;
  CHECK_EQ(call("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", {P(&n), d}), 0u);
  int expect = 0;
  for (auto& kv : dir_counts) if (kv.first == "common/Text/master") expect = kv.second;
  // subdirectories of that directory also count as entries
  std::set<std::string> subdirs;
  for (auto& r : rows) if (r[0].rfind("common/Text/master/", 0) == 0) {
    std::string rest = r[0].substr(19);
    size_t s = rest.find('/');
    if (s != std::string::npos) subdirs.insert(rest.substr(0, s));
  }
  CHECK_EQ(n, (long long)(expect + subdirs.size()));
  std::vector<uint8_t> ents((size_t)n * 0x310);
  int64_t got = 0;
  CHECK_EQ(call("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", {P(&got), P(ents.data()), d, (uint64_t)n}), 0u);
  CHECK_EQ(got, n);
  bool found = false;
  for (int64_t i = 0; i < got; ++i)
    if (std::string(reinterpret_cast<char*>(&ents[(size_t)i * 0x310])) == "TextResource_en.drp") {
      found = true;
      CHECK_EQ((int)ents[(size_t)i * 0x310 + 0x304], 1);
      CHECK_EQ(load<int64_t>(&ents[(size_t)i * 0x310 + 0x308]), 221884);
    }
  CHECK(found);
  call("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", {d});

  // entry types: 0 directory, 1 file
  int type = -1;
  CHECK_EQ(call("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", {P(&type), P("rom:/common/Text/master")}), 0u);
  CHECK_EQ(type, 0);
  CHECK_EQ(call("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", {P(&type), P("rom:/common/Text/master/TextResource_en.drp")}), 0u);
  CHECK_EQ(type, 1);

  // result codes
  uint32_t rc = 0;
  std::string data;
  CHECK(!read_whole_via_shims("rom:/no/such/file.bin", &data, &rc));
  CHECK_EQ(rc, fs::kResultPathNotFound);
  CHECK_EQ(result_module(rc), 2u);
  CHECK_EQ(rc & 0x3ffe00, 0x200u);  // the game's test: description 1 -> game error 0xd
  uint64_t h = 0;
  CHECK_EQ(call(kOpenFile, {P(&h), P("rom:/common/Text/master/TextResource_en.drp"), 1}), 0u);
  char tiny[16];
  CHECK(call(kReadFile, {h, 221884 - 4, P(tiny), 16}) != 0);  // strict read past the end fails
  int64_t wsz = 0;
  (void)wsz;
  CHECK(call("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", {h, 0, P(tiny), 4, P(&wsz)}) != 0);  // rom is read only
  call(kCloseFile, {h});
  CHECK(call("_ZN2nn2fs10CreateFileEPKcl", {P("rom:/newfile"), 4}) != 0);
  CHECK(call("_ZN2nn2fs8MountRomEPKcPvm", {P("rom"), 0, 0x10000}) == 0u);  // remounting is harmless
}

// ---- fs: overlay (mod loader hook)
static void test_fs_overlay() {
  g_test = "fs.overlay";
  stdfs::path mod = g_tmp / "mod";
  stdfs::create_directories(mod / "common" / "Text" / "master");
  { std::ofstream f(mod / "common" / "Text" / "master" / "TextResource_en.drp", std::ios::binary); f << "MODDED"; }
  { std::ofstream f(mod / "common" / "Text" / "master" / "ExtraFile.bin", std::ios::binary); f << "extra"; }
  fs::Fs local;
  local.set_rom_layers({mod.string(), (g_root / "extracted" / "romfs-v262144").string()});
  CHECK_EQ(local.mount_rom("rom"), 0u);
  fs::FileId f = 0;
  CHECK_EQ(local.open_file(&f, "rom:/common/Text/master/TextResource_en.drp", fs::kModeRead), 0u);
  int64_t sz = 0;
  local.get_file_size(f, &sz);
  CHECK_EQ(sz, 6);  // the first layer wins
  local.close_file(f);
  CHECK_EQ(local.open_file(&f, "rom:/common/Text/master/TextResource_fr.drp", fs::kModeRead), 0u);  // falls through to the base
  local.get_file_size(f, &sz);
  CHECK_EQ(sz, 233384);
  local.close_file(f);
  fs::DirId d = 0;
  int64_t cnt = 0;
  CHECK_EQ(local.open_directory(&d, "rom:/common/Text/master", fs::kOpenFiles), 0u);
  local.get_directory_entry_count(d, &cnt);
  std::vector<fs::DirEntry> es(256);
  int64_t got = 0;
  local.read_directory(d, es.data(), 256, &got);
  std::set<std::string> names;
  for (int64_t i = 0; i < got; ++i) names.insert(es[(size_t)i].name);
  CHECK(names.count("ExtraFile.bin"));
  CHECK(names.count("TextResource_en.drp"));
  CHECK_EQ((long long)names.size(), (long long)got);  // a file in two layers is listed once
  local.close_directory(d);
}

// ---- save data
static void put(fs::Fs& f, const std::string& path, const std::string& body) {
  fs::FileId id = 0;
  uint32_t rc = f.create_file(path, (int64_t)body.size());
  CHECK(rc == 0 || rc == fs::kResultPathAlreadyExists);
  CHECK_EQ(f.open_file(&id, path, fs::kModeRead | fs::kModeWrite), 0u);
  CHECK_EQ(f.set_file_size(id, (int64_t)body.size()), 0u);
  CHECK_EQ(f.write_file(id, 0, body.data(), body.size(), true), 0u);
  f.close_file(id);
}
static bool get(fs::Fs& f, const std::string& path, std::string* body) {
  fs::FileId id = 0;
  if (f.open_file(&id, path, fs::kModeRead)) return false;
  int64_t sz = 0;
  f.get_file_size(id, &sz);
  body->assign((size_t)sz, '\0');
  size_t got = 0;
  f.read_file(id, 0, body->data(), (size_t)sz, &got);
  f.close_file(id);
  return got == (size_t)sz;
}

static const uint64_t kUid[2] = {0x1111222233334444ull, 0x5555666677778888ull};

static void test_save_roundtrip() {
  g_test = "save.roundtrip";
  stdfs::path root = g_tmp / "save_rt";
  const std::string a = "savedata:/0000000100000002", b = "savedata:/0000000100000003";
  {
    fs::Fs f;
    f.set_save_root(root.string());
    CHECK_EQ(f.mount_save("savedata", kUid), 0u);
    CHECK_EQ(f.create_file(a, 16), 0u);
    CHECK_EQ(f.create_file(a, 16), fs::kResultPathAlreadyExists);
    put(f, a, "first version 123");
    put(f, b, "second file");
    CHECK_EQ(f.commit("savedata"), 0u);
    int64_t free_bytes = 0;
    CHECK_EQ(f.get_free_space(&free_bytes, "savedata"), 0u);
    CHECK(free_bytes >= (4ll << 30));  // the game needs >= 100 MiB or it calls ExtendSaveData
    CHECK_EQ(f.unmount("savedata"), 0u);
  }
  {
    fs::Fs f;  // new process
    f.set_save_root(root.string());
    CHECK_EQ(f.mount_save("savedata", kUid), 0u);
    std::string body;
    CHECK(get(f, a, &body));
    CHECK(body == "first version 123");
    CHECK(get(f, b, &body));
    CHECK(body == "second file");
    int type = -1;
    CHECK_EQ(f.get_entry_type(&type, a), 0u);
    CHECK_EQ(type, 1);
    // change, delete, rename, commit
    put(f, a, "second version");
    CHECK_EQ(f.delete_file(b), 0u);
    CHECK_EQ(f.delete_file(b), fs::kResultPathNotFound);
    CHECK_EQ(f.commit("savedata"), 0u);
  }
  {
    fs::Fs f;
    f.set_save_root(root.string());
    CHECK_EQ(f.mount_save("savedata", kUid), 0u);
    std::string body;
    CHECK(get(f, a, &body));
    CHECK(body == "second version");
    CHECK(!get(f, b, &body));
    int64_t n = 0;
    fs::DirId d = 0;
    CHECK_EQ(f.open_directory(&d, "savedata:/", fs::kOpenAll), 0u);
    f.get_directory_entry_count(d, &n);
    CHECK_EQ(n, 1);
    f.close_directory(d);
  }
  // the same through the guest shims (the game's path): MountSaveData, WriteFile with flush, Commit
  {
    config().save_root = (g_tmp / "save_shim").string();
    fs::instance().set_save_root(config().save_root);
    uint64_t uid[2] = {kUid[0], kUid[1]};
    CHECK_EQ(call("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", {P("savedata"), P(uid)}), 0u);
    const char* path = "savedata:/00000000deadbeef";
    CHECK_EQ(call("_ZN2nn2fs10CreateFileEPKcl", {P(path), 8}), 0u);
    CHECK_EQ(call("_ZN2nn2fs10CreateFileEPKcl", {P(path), 8}), (uint64_t)fs::kResultPathAlreadyExists);
    uint64_t h = 0;
    CHECK_EQ(call(kOpenFile, {P(&h), P(path), 6}), 0u);  // Read|Write... mode bits as the game passes
    uint32_t opt = 1;
    CHECK_EQ(call("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", {h, 0, P("ABCDEFGH"), 8, P(&opt)}), 0u);
    call(kCloseFile, {h});
    CHECK_EQ(call("_ZN2nn2fs6CommitEPKc", {P("savedata")}), 0u);
    call("_ZN2nn2fs7UnmountEPKc", {P("savedata")});
    CHECK_EQ(call("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", {P("savedata"), P(uid)}), 0u);
    std::string body;
    CHECK(read_whole_via_shims(path, &body));
    CHECK(body == "ABCDEFGH");
    call("_ZN2nn2fs7UnmountEPKc", {P("savedata")});
  }
}

static void test_save_crash_midwrite() {
  g_test = "save.crash_midwrite";
  stdfs::path root = g_tmp / "save_mid";
  const std::string a = "savedata:/0000000000000001";
  {
    fs::Fs f;
    f.set_save_root(root.string());
    CHECK_EQ(f.mount_save("savedata", kUid), 0u);
    put(f, a, "GOOD-OLD-SAVE");
    CHECK_EQ(f.commit("savedata"), 0u);
    // overwrite without committing, add a file, then "crash": the process dies without Unmount/Commit
    put(f, a, "HALF-WRITTEN-GARBAGE-LONGER");
    put(f, "savedata:/0000000000000002", "orphan");
    // destructor runs without commit
  }
  fs::Fs f;
  f.set_save_root(root.string());
  CHECK_EQ(f.mount_save("savedata", kUid), 0u);
  std::string body;
  CHECK(get(f, a, &body));
  CHECK(body == "GOOD-OLD-SAVE");
  CHECK(!get(f, "savedata:/0000000000000002", &body));
}

static void test_save_crash_commit_stages() {
  g_test = "save.crash_commit";
  const char* stages[] = {"commit:begin", "commit:carried", "commit:file", "commit:files-done", "commit:marker", "commit:pointer-tmp", "commit:pointer-done"};
  int idx = 0;
  for (const char* stage : stages) {
    stdfs::path root = g_tmp / ("save_cs" + std::to_string(idx++));
    const std::string a = "savedata:/0000000000000001", b = "savedata:/0000000000000002";
    {
      fs::Fs f;
      f.set_save_root(root.string());
      CHECK_EQ(f.mount_save("savedata", kUid), 0u);
      put(f, a, "OLD-A");
      put(f, b, "OLD-B-UNCHANGED");
      CHECK_EQ(f.commit("savedata"), 0u);
      put(f, a, "NEW-A-CHANGED");
      put(f, "savedata:/0000000000000003", "NEW-C");
      bool crashed = false;
      f.set_fault_hook([&](const char* s) { if (std::string(s) == stage) throw fs::SimulatedCrash{s}; });
      try { f.commit("savedata"); } catch (const fs::SimulatedCrash&) { crashed = true; }
      CHECK(crashed);
    }
    fs::Fs f;
    f.set_save_root(root.string());
    CHECK_EQ(f.mount_save("savedata", kUid), 0u);
    std::string ba, bb, bc;
    bool ha = get(f, a, &ba), hb = get(f, b, &bb), hc = get(f, "savedata:/0000000000000003", &bc);
    CHECK(ha && hb);
    CHECK(bb == "OLD-B-UNCHANGED");
    bool is_old = ba == "OLD-A" && !hc;
    bool is_new = ba == "NEW-A-CHANGED" && hc && bc == "NEW-C";
    CHECK(is_old != is_new);  // exactly one complete state, never torn
    bool expect_new = std::string(stage) == "commit:pointer-done";
    CHECK_EQ(is_new, expect_new);
    std::printf("  crash at %-20s -> %s save\n", stage, is_new ? "new" : "old");
    // the save must still be usable: commit again works
    put(f, a, "AFTER-RECOVERY");
    CHECK_EQ(f.commit("savedata"), 0u);
  }
}

static void test_save_backups() {
  g_test = "save.backups";
  stdfs::path root = g_tmp / "save_bk";
  fs::Fs f;
  f.set_save_root(root.string());
  f.set_save_backups(3);
  CHECK_EQ(f.mount_save("savedata", kUid), 0u);
  const std::string a = "savedata:/0000000000000001";
  for (int i = 1; i <= 7; ++i) {
    put(f, a, "VERSION-" + std::to_string(i));
    CHECK_EQ(f.commit("savedata"), 0u);
  }
  auto gens = f.list_generations("savedata");
  CHECK(gens.size() >= 2 && gens.size() <= 4);  // current plus at most 3 backups
  int current = 0;
  for (auto& g : gens) current += g.current ? 1 : 0;
  CHECK_EQ(current, 1);
  // restore the oldest kept backup
  uint64_t oldest = gens.front().gen;
  for (auto& g : gens) oldest = std::min(oldest, g.gen);
  CHECK_EQ(f.restore_generation("savedata", oldest), 0u);
  std::string body;
  CHECK(get(f, a, &body));
  CHECK(body != "VERSION-7");
  CHECK(body.rfind("VERSION-", 0) == 0);
  std::printf("  restored generation %llu -> %s (kept %zu generations)\n", (unsigned long long)oldest, body.c_str(), gens.size());
  // restored state survives a remount
  fs::Fs f2;
  f2.set_save_root(root.string());
  CHECK_EQ(f2.mount_save("savedata", kUid), 0u);
  std::string body2;
  CHECK(get(f2, a, &body2));
  CHECK(body2 == body);
}

// ---- hid
struct FakeBackend : input::Backend {
  input::RawInput* shared;
  float rumble_low = -1, rumble_high = -1;
  explicit FakeBackend(input::RawInput* s) : shared(s) {}
  const char* name() const override { return "fake"; }
  void poll(input::RawInput& out) override { out = *shared; }
  void rumble(float l, float h) override { rumble_low = l; rumble_high = h; }
};

struct NpadOut { int64_t seq; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attrs; };
static_assert(sizeof(NpadOut) == 0x28, "npad state layout");

static NpadOut npad(const char* sym, uint32_t id) {
  std::this_thread::sleep_for(std::chrono::milliseconds(3));  // the hub caches samples for 1 ms
  NpadOut o{};
  uint8_t buf[0x28] = {};
  call(sym, {P(buf), P(&id)});
  std::memcpy(&o, buf, sizeof o);
  return o;
}

static const char* kFullKey = "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj";
static const char* kHandheld = "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj";
static const char* kTouch = "_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE";

static void test_hid() {
  g_test = "hid";
  using namespace input;
  static RawInput raw;
  config().input_map_path = (g_tmp / "input_map.json").string();
  config().controller_style = 0;
  auto fb = std::make_unique<FakeBackend>(&raw);
  FakeBackend* fbp = fb.get();
  hub().set_backend(std::move(fb));
  CHECK(write_default_mapping(config().input_map_path));  // the file the settings UI will edit
  call("_ZN2nn3hid14InitializeNpadEv");
  uint32_t ids[2] = {0, 0x20};
  call("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", {P(ids), 2});
  call("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", {3});

  // style set for No1
  uint32_t no1 = 0;
  CHECK_EQ(call("_ZN2nn3hid15GetNpadStyleSetERKj", {P(&no1)}), 1u);  // FullKey

  // idle
  raw = RawInput{};
  raw.pad_connected = true;
  NpadOut o = npad(kFullKey, 0);
  CHECK_EQ(o.buttons, 0u);
  CHECK(o.attrs & 1);

  // pad: south = B, east = A, shoulders, start = Plus, triggers (analog) = ZL/ZR, d-pad
  raw.pad.set(kPadEast);
  raw.pad.set(kPadSouth);
  raw.pad.set(kPadStart);
  raw.pad.set(kPadLShoulder);
  raw.pad.set(kPadDUp);
  raw.rt = 1.0f;
  o = npad(kFullKey, 0);
  CHECK(o.buttons & (1ull << kA));
  CHECK(o.buttons & (1ull << kB));
  CHECK(!(o.buttons & (1ull << kX)));
  CHECK(o.buttons & (1ull << kPlus));
  CHECK(o.buttons & (1ull << kL));
  CHECK(o.buttons & (1ull << kUp));
  CHECK(o.buttons & (1ull << kZR));
  CHECK(!(o.buttons & (1ull << kZL)));

  // sticks: analog with deadzone, then keyboard W/D
  raw = RawInput{};
  raw.pad_connected = true;
  raw.lx = 1.0f; raw.ly = 0.5f; raw.rx = -1.0f;
  o = npad(kFullKey, 0);
  CHECK(o.lx > 25000);  // radial: (1, 0.5) is clamped to the unit circle
  CHECK(o.ly > 10000 && o.ly < 20000);
  CHECK(o.rx < -30000);
  CHECK(o.buttons & (1ull << kStickLRight));
  CHECK(o.buttons & (1ull << kStickRLeft));
  raw.lx = 0.05f; raw.ly = 0.05f;  // inside the dead zone
  o = npad(kFullKey, 0);
  CHECK_EQ(o.lx, 0);
  CHECK_EQ(o.ly, 0);
  raw = RawInput{};
  raw.keys.set((size_t)key_from_name("W"));
  raw.keys.set((size_t)key_from_name("D"));
  o = npad(kFullKey, 0);
  CHECK(o.ly > 20000 && o.lx > 20000 && o.ly == o.lx);  // keyboard W+D: diagonal on the unit circle, y up positive
  raw = RawInput{};
  raw.keys.set((size_t)key_from_name("L"));       // keyboard A
  raw.keys.set((size_t)key_from_name("LShift"));  // ZL
  raw.keys.set((size_t)key_from_name("Right"));   // right stick right
  o = npad(kFullKey, 0);
  CHECK(o.buttons & (1ull << kA));
  CHECK(o.buttons & (1ull << kZL));
  CHECK(o.rx > 30000);

  // focus loss releases everything
  raw.focused = false;
  o = npad(kFullKey, 0);
  CHECK_EQ(o.buttons, 0u);
  CHECK_EQ(o.lx, 0);
  raw.focused = true;

  // other npad ids and styles read as empty
  o = npad(kFullKey, 3);
  CHECK_EQ(o.buttons, 0u);
  o = npad(kHandheld, 0x20);
  CHECK_EQ(o.buttons, 0u);

  // remapping table: JSON edit swaps A and B, adds a mouse button, and is validated
  Mapping m = Mapping::defaults();
  std::string err;
  Json j = m.to_json();
  std::string text;
  json_dump(j, text);
  CHECK(text.size() > 100);
  Json parsed;
  CHECK(json_parse(text, parsed, err));
  Mapping m2;
  CHECK(m2.from_json(parsed, &err));
  {
    std::ofstream f(g_tmp / "remap.json");
    f << "// custom\n{\"buttons\": {\"A\": [\"pad:south\"], \"B\": [\"pad:east\", \"mouse:right\"]}}\n";
  }
  CHECK(hub().load_mapping((g_tmp / "remap.json").string(), &err));
  raw = RawInput{};
  raw.pad.set(kPadSouth);
  o = npad(kFullKey, 0);
  CHECK(o.buttons & (1ull << kA));
  CHECK(!(o.buttons & (1ull << kB)));
  raw = RawInput{};
  raw.mouse_buttons = 2;
  o = npad(kFullKey, 0);
  CHECK(o.buttons & (1ull << kB));
  {
    std::ofstream f(g_tmp / "bad.json");
    f << "{\"buttons\": {\"A\": [\"pad:nonsense\"]}}";
  }
  CHECK(!hub().load_mapping((g_tmp / "bad.json").string(), &err));
  CHECK(!err.empty());
  hub().set_mapping(Mapping::defaults());

  // touch: the left mouse button is one finger on the 1280x720 panel
  raw = RawInput{};
  raw.mouse_in_window = true;
  raw.mouse_x = 0.5f; raw.mouse_y = 0.25f;
  raw.mouse_buttons = 1;
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  uint8_t ts[16 + 6 * 0x28] = {};
  call(kTouch, {P(ts)});
  CHECK_EQ(load<int32_t>(ts + 8), 1);
  CHECK_EQ(load<int32_t>(ts + 16 + 16), 640);
  CHECK_EQ(load<int32_t>(ts + 16 + 20), 180);
  raw.mouse_buttons = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  call(kTouch, {P(ts)});
  CHECK_EQ(load<int32_t>(ts + 8), 0);

  // handheld style: id 0x20, attributes mark all handheld features
  config().controller_style = 1;
  raw = RawInput{};
  raw.pad_connected = true;
  raw.pad.set(kPadEast);
  o = npad(kHandheld, 0x20);
  CHECK(o.buttons & (1ull << kA));
  CHECK_EQ(o.attrs, 0x3fu);
  o = npad(kFullKey, 0);
  CHECK_EQ(o.buttons, 0u);
  config().controller_style = 0;

  // six-axis: one handle, at rest; vibration reaches the backend
  uint32_t handle = 0x7fffffff;
  uint32_t zero = 0;
  CHECK_EQ(call("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", {P(&handle), 1, P(&zero), 1}), 1u);
  CHECK(handle != 0x7fffffff);
  CHECK_EQ(call("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", {P(&handle)}) & 1, 1u);
  uint8_t sx[0x60 * 2] = {};
  CHECK_EQ(call("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", {P(sx), 1, P(&handle)}), 1u);
  CHECK_EQ(load<float>(sx + 0x1c), 0.0f);  // angular velocity x
  uint32_t vh[2] = {};
  CHECK_EQ(call("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", {P(vh), 2, P(&zero), 1}), 2u);
  float vib[4] = {0.5f, 160.0f, 0.25f, 320.0f};
  call("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", {P(&vh[0]), P(vib)});
  CHECK(fbp->rumble_low == 0.5f && fbp->rumble_high == 0.25f);
  hub().set_backend(nullptr);
}


// ---- input mapping: binding resolution, slot editing, JSON round trip and migration, capture, toggle, suppression
static input::PadSample mapped(const input::Mapping& m, const input::RawInput& r) { return input::map_pad(m, r); }
static uint64_t bit(int b) { return 1ull << b; }
static input::Source SRC(const char* t) { input::Source s; bool ok = input::parse_source(t, &s); (void)ok; return s; }

static void test_input_mapping() {
  g_test = "input.mapping";
  using namespace input;
  Mapping m = Mapping::defaults();
  RawInput r;
  r.pad_connected = true;

  // --- binding resolution: every device drives the same Switch button
  r.keys.set((size_t)key_from_name("L"));
  CHECK(mapped(m, r).buttons & bit(kA));
  r = RawInput{}; r.pad.set(kPadEast);
  CHECK(mapped(m, r).buttons & bit(kA));
  // mouse buttons and the wheel can be bound to any button
  slot_set(m.buttons[kX], kDevMouse, 0, SRC("mouse:right"));
  slot_set(m.buttons[kY], kDevMouse, 0, SRC("mouse:wheel_up"));
  slot_set(m.buttons[kB], kDevMouse, 0, SRC("mouse:x1"));
  r = RawInput{}; r.mouse_buttons = 1u << kMouseRight;
  CHECK(mapped(m, r).buttons & bit(kX));
  r.mouse_buttons = 1u << kMouseWheelUp;
  CHECK(mapped(m, r).buttons & bit(kY));
  CHECK(!(mapped(m, r).buttons & bit(kX)));
  r.mouse_buttons = 1u << kMouseX1;
  CHECK(mapped(m, r).buttons & bit(kB));
  r.focused = false;   // keyboard and mouse bindings are released without the focus
  CHECK(!(mapped(m, r).buttons & bit(kB)));
  // a controller axis (stick direction, trigger) as a button source
  slot_set(m.buttons[kPlus], kDevController, 1, SRC("pad:rstick_up"));
  r = RawInput{}; r.pad_connected = true; r.ry = 0.8f;
  CHECK(mapped(m, r).buttons & bit(kPlus));
  r.ry = 0.2f;
  CHECK(!(mapped(m, r).buttons & bit(kPlus)));

  // --- face layout: by position (default) and Nintendo by label
  m = Mapping::defaults();
  r = RawInput{}; r.pad.set(kPadSouth);
  CHECK(mapped(m, r).buttons & bit(kB));
  set_face_layout(m, 1);
  CHECK_EQ(m.face_layout, 1);
  CHECK(mapped(m, r).buttons & bit(kA));     // Xbox A = Switch A
  CHECK(!(mapped(m, r).buttons & bit(kB)));
  r = RawInput{}; r.pad.set(kPadNorth);
  CHECK(mapped(m, r).buttons & bit(kY));     // Xbox Y = Switch Y
  set_face_layout(m, 0);
  CHECK(mapped(m, r).buttons & bit(kX));
  r = RawInput{}; r.pad.set(kPadSouth);
  CHECK(mapped(m, r).buttons & bit(kB));

  // --- trigger thresholds are per trigger
  m = Mapping::defaults();
  m.zl_threshold = 0.2f; m.zr_threshold = 0.9f;
  r = RawInput{}; r.pad_connected = true; r.lt = 0.3f; r.rt = 0.5f;
  CHECK(mapped(m, r).buttons & bit(kZL));
  CHECK(!(mapped(m, r).buttons & bit(kZR)));
  r.rt = 0.95f;
  CHECK(mapped(m, r).buttons & bit(kZR));

  // --- sticks: inner and outer dead zone, invert Y, swap
  m = Mapping::defaults();
  m.left.deadzone = 0.2f; m.left.outer_deadzone = 0.8f;
  r = RawInput{}; r.pad_connected = true; r.lx = 0.15f;
  CHECK_EQ(mapped(m, r).lx, 0);                      // inside the inner dead zone
  r.lx = 0.5f;
  CHECK(mapped(m, r).lx > 15000 && mapped(m, r).lx < 18000);   // (0.5-0.2)/(0.8-0.2) = 0.5
  r.lx = 0.85f;
  CHECK_EQ(mapped(m, r).lx, kStickMax);              // past the outer dead zone: full deflection
  m = Mapping::defaults();
  m.left.invert_y = true;
  r = RawInput{}; r.pad_connected = true; r.ly = 1.0f;
  CHECK(mapped(m, r).ly < -30000);
  CHECK(mapped(m, r).ry == 0);
  m = Mapping::defaults();
  m.swap_sticks = true;
  r = RawInput{}; r.pad_connected = true; r.lx = 1.0f; r.ry = -1.0f;
  PadSample p = mapped(m, r);
  CHECK(p.lx == 0 && p.rx > 30000 && p.ry == 0 && p.ly < -30000);   // physical left drives the right stick and the other way round
  m = Mapping::defaults();
  m.left.analog = "none";
  r = RawInput{}; r.pad_connected = true; r.lx = 1.0f;
  CHECK_EQ(mapped(m, r).lx, 0);

  // --- keyboard stick directions are rebindable
  m = Mapping::defaults();
  slot_set(m.right.up, kDevKeyboard, 0, SRC("key:Numpad8"));   // replaces the Up arrow in slot 0
  r = RawInput{}; r.keys.set((size_t)key_from_name("Numpad8"));
  CHECK(mapped(m, r).ry > 30000);
  r = RawInput{}; r.keys.set((size_t)key_from_name("Up"));
  CHECK_EQ(mapped(m, r).ry, 0);

  // --- mouse-look: velocity drives the right stick, invert, off by default, disables mouse touch
  m = Mapping::defaults();
  CHECK(!m.mouse_look);
  r = RawInput{}; r.mouse_vx = 3000;
  CHECK_EQ(mapped(m, r).rx, 0);
  m.mouse_look = true;
  r.mouse_vx = kMouseLookFullPxPerSec / 2; r.mouse_vy = -kMouseLookFullPxPerSec / 4;   // right and up
  p = mapped(m, r);
  CHECK(p.rx > 15000 && p.rx < 17500);
  CHECK(p.ry > 7500 && p.ry < 9000);
  m.mouse_invert_y = true; m.mouse_invert_x = true;
  p = mapped(m, r);
  CHECK(p.rx < -15000 && p.ry < -7500);
  m.mouse_invert_x = m.mouse_invert_y = false;
  m.mouse_sensitivity = 2.0f;
  r.mouse_vx = kMouseLookFullPxPerSec; r.mouse_vy = 0;
  CHECK(mapped(m, r).rx > 30000);                    // saturates
  r = RawInput{}; r.mouse_in_window = true; r.mouse_buttons = 1; r.mouse_x = r.mouse_y = 0.5f;
  CHECK_EQ(map_touch(m, r).count, 0);                // mouse-look owns the mouse
  m.mouse_look = false;
  CHECK_EQ(map_touch(m, r).count, 1);

  // --- slots: two per device, replacement, clear, duplicates, wrong device
  std::vector<Source> v;
  CHECK(slot_set(v, kDevKeyboard, 0, SRC("key:W")));
  CHECK(slot_set(v, kDevKeyboard, 1, SRC("key:Up")));
  CHECK(slot_set(v, kDevController, 0, SRC("pad:south")));
  CHECK(!slot_set(v, kDevMouse, 0, SRC("key:Q")));    // a key cannot go in a mouse slot
  CHECK(!slot_set(v, kDevKeyboard, 2, SRC("key:Q"))); // only two slots
  CHECK_EQ(slot_count(v, kDevKeyboard), 2);
  CHECK(slot_get(v, kDevKeyboard, 1) == SRC("key:Up"));
  CHECK(slot_get(v, kDevController, 0) == SRC("pad:south"));
  CHECK(slot_get(v, kDevMouse, 0).kind == Source::None);
  CHECK(slot_set(v, kDevKeyboard, 0, SRC("key:S")));  // replace slot 0
  CHECK(slot_get(v, kDevKeyboard, 0) == SRC("key:S") && slot_get(v, kDevKeyboard, 1) == SRC("key:Up"));
  CHECK(slot_set(v, kDevKeyboard, 1, SRC("key:S")));  // same key in the other slot: the old copy goes
  CHECK_EQ(slot_count(v, kDevKeyboard), 1);
  CHECK(slot_get(v, kDevKeyboard, 0) == SRC("key:S"));
  slot_clear(v, kDevKeyboard, 0);
  CHECK_EQ(slot_count(v, kDevKeyboard), 0);
  CHECK_EQ(slot_count(v, kDevController), 1);         // other devices untouched
  CHECK(slot_set(v, kDevMouse, 1, SRC("mouse:left")));  // an empty slot 1 fills the first free slot
  CHECK(slot_get(v, kDevMouse, 0) == SRC("mouse:left"));

  // --- conflicts are reported and allowed
  m = Mapping::defaults();
  CHECK_EQ(actions_using(m, SRC("key:Space")).size(), 1u);
  slot_set(m.buttons[kA], kDevKeyboard, 1, SRC("key:Space"));
  std::vector<int> users = actions_using(m, SRC("key:Space"));
  CHECK_EQ(users.size(), 2u);
  r = RawInput{}; r.keys.set((size_t)key_from_name("Space"));
  CHECK((mapped(m, r).buttons & bit(kA)) && (mapped(m, r).buttons & bit(kZR)));   // both fire

  // --- per-device reset keeps the other devices
  m = Mapping::defaults();
  slot_set(m.buttons[kA], kDevKeyboard, 0, SRC("key:P"));
  slot_set(m.buttons[kA], kDevController, 0, SRC("pad:lshoulder"));
  slot_set(m.buttons[kB], kDevMouse, 0, SRC("mouse:left"));
  m.swap_sticks = true; m.zl_threshold = 0.9f; m.mouse_look = true;
  reset_device(m, kDevController);
  CHECK(slot_get(m.buttons[kA], kDevController, 0) == SRC("pad:east"));
  CHECK(!m.swap_sticks && m.zl_threshold == 0.5f);
  CHECK(slot_get(m.buttons[kA], kDevKeyboard, 0) == SRC("key:P"));   // keyboard kept
  CHECK(slot_get(m.buttons[kB], kDevMouse, 0) == SRC("mouse:left")); // mouse kept
  CHECK(m.mouse_look);
  reset_device(m, kDevKeyboard);
  CHECK(slot_get(m.buttons[kA], kDevKeyboard, 0) == SRC("key:L"));
  reset_device(m, kDevMouse);
  CHECK_EQ(slot_count(m.buttons[kB], kDevMouse), 0);
  CHECK(!m.mouse_look);
}

static void test_input_config() {
  g_test = "input.config";
  using namespace input;
  namespace fs = std::filesystem;
  fs::path dir = g_tmp / "inputcfg";
  fs::create_directories(dir);
  auto dump = [](const Mapping& m) { std::string t; json_dump(m.to_json(), t); return t; };

  // --- JSON round trip of a heavily edited mapping
  Mapping m = Mapping::defaults();
  slot_set(m.buttons[kA], kDevKeyboard, 0, SRC("key:P"));
  slot_set(m.buttons[kA], kDevKeyboard, 1, SRC("key:Numpad5"));
  slot_set(m.buttons[kZL], kDevMouse, 0, SRC("mouse:wheel_down"));
  slot_set(m.buttons[kZL], kDevController, 1, SRC("pad:lstick_left"));
  slot_set(m.right.up, kDevMouse, 0, SRC("mouse:x2"));
  m.left.deadzone = 0.25f; m.left.outer_deadzone = 0.9f; m.right.invert_y = true; m.right.analog = "left";
  m.swap_sticks = true; m.face_layout = 1; m.zl_threshold = 0.3f; m.zr_threshold = 0.7f;
  m.mouse_look = true; m.mouse_sensitivity = 1.75f; m.mouse_invert_x = true; m.mouse_smoothing = 0.4f; m.lock_cursor = false;
  m.toggle_zl = true; m.toggle_zr = true; m.ui_scale = 1.5f;
  Json parsed; std::string err;
  CHECK(json_parse(dump(m), parsed, err));
  CHECK_EQ((int)parsed.get("version")->num(), 2);
  Mapping back;
  CHECK(back.from_json(parsed, &err));
  CHECK_EQ(dump(back) == dump(m), true);
  CHECK(slot_get(back.buttons[kA], kDevKeyboard, 1) == SRC("key:Numpad5"));
  CHECK(back.face_layout == 1 && back.swap_sticks && back.toggle_zr && !back.lock_cursor && back.mouse_invert_x && back.ui_scale == 1.5f);
  CHECK(back.right.analog == "left" && back.right.invert_y && back.left.outer_deadzone == 0.9f);

  // --- save atomically, then load it back through the hub
  std::string path = (dir / "input.json").string();
  Hub h;
  h.set_mapping(m);
  CHECK(write_mapping_file(m, path, &err));
  CHECK(!fs::exists(path + ".tmp"));
  CHECK(h.load_config(path, ""));
  CHECK_EQ(dump(h.mapping()) == dump(m), true);
  CHECK(h.message().empty());
  Mapping m3 = h.mapping();
  m3.swap_sticks = false;
  h.set_mapping(m3);
  CHECK(h.save(&err));          // live edit saved over the file
  CHECK(!fs::exists(path + ".tmp"));
  Hub h2;
  CHECK(h2.load_config(path, ""));
  CHECK(!h2.mapping().swap_sticks && h2.mapping().face_layout == 1);

  // --- a newer format version is refused, a missing key keeps its default, unknown keys are ignored
  {
    std::ofstream f(dir / "future.json"); f << "{\"version\": 3}";
  }
  Hub hf;
  CHECK(!hf.load_config((dir / "future.json").string(), ""));
  CHECK(hf.message().find("newer") != std::string::npos);
  {
    std::ofstream f(dir / "partial.json"); f << "{\"version\": 2, \"whatever\": 5, \"mouse\": {\"look\": true}}";
  }
  Hub hp;
  CHECK(hp.load_config((dir / "partial.json").string(), ""));
  CHECK(hp.mapping().mouse_look && hp.mapping().mouse_sensitivity == 1.0f);
  CHECK(slot_get(hp.mapping().buttons[kA], kDevController, 0) == SRC("pad:east"));

  // --- invalid file: defaults, a visible message, the bad file kept
  {
    std::ofstream f(dir / "bad.json"); f << "{\"version\": 2, \"buttons\": {\"A\": [\"pad:nonsense\"]}}";
  }
  Hub hb;
  uint64_t serial0 = hb.message_serial();
  CHECK(!hb.load_config((dir / "bad.json").string(), ""));
  CHECK(!hb.message().empty());
  CHECK(hb.message_serial() > serial0);
  CHECK(fs::exists(dir / "bad.json.invalid"));
  CHECK_EQ(dump(hb.mapping()) == dump(Mapping::defaults()), true);
  {
    std::ofstream f(dir / "garbage.json"); f << "this is not json";
  }
  Hub hg;
  CHECK(!hg.load_config((dir / "garbage.json").string(), ""));
  CHECK(hg.message().find("invalid") != std::string::npos);

  // --- version 1 file (input_map.json): migrated to input.json, the old file stays
  {
    std::ofstream f(dir / "input_map.json");
    f << "{\"version\": 1, \"trigger_threshold\": 0.7, \"mouse\": {\"touch\": false, \"look_sensitivity\": 0.05},"
         " \"buttons\": {\"A\": [\"pad:south\", \"key:Z\"]}, \"left_stick\": {\"deadzone\": 0.3, \"up\": [\"key:Up\"]}}";
  }
  Hub hm;
  std::string newp = (dir / "migrated" / "input.json").string();
  CHECK(hm.load_config(newp, (dir / "input_map.json").string()));
  CHECK(fs::exists(newp));
  CHECK(fs::exists(dir / "input_map.json"));
  CHECK(!hm.message().empty());
  Mapping mm = hm.mapping();
  CHECK(slot_get(mm.buttons[kA], kDevController, 0) == SRC("pad:south"));
  CHECK(slot_get(mm.buttons[kA], kDevKeyboard, 0) == SRC("key:Z"));
  CHECK(mm.zl_threshold == 0.7f && mm.zr_threshold == 0.7f);
  CHECK(!mm.mouse_touch && mm.mouse_look);
  CHECK(mm.left.deadzone == 0.3f);
  Hub hm2;   // second start reads the new file, no migration note
  CHECK(hm2.load_config(newp, (dir / "input_map.json").string()));
  CHECK(hm2.message().empty());
  CHECK_EQ(dump(hm2.mapping()) == dump(mm), true);

  // --- first run writes the defaults
  Hub hn;
  std::string fresh = (dir / "fresh" / "input.json").string();
  CHECK(hn.load_config(fresh, ""));
  CHECK(fs::exists(fresh));
  CHECK_EQ(dump(hn.mapping()) == dump(Mapping::defaults()), true);

  // --- SNAP_CONFIG_DIR picks the folder
  _putenv_s("SNAP_CONFIG_DIR", (dir / "envdir").string().c_str());
  CHECK_EQ(default_input_path() == (dir / "envdir").string() + "\\input.json", true);
  _putenv_s("SNAP_CONFIG_DIR", "");
  CHECK(default_input_path().find("input.json") != std::string::npos);
}

static void test_input_hub() {
  g_test = "input.hub";
  using namespace input;
  static RawInput raw;
  raw = RawInput{};
  Hub h;
  h.set_backend(std::make_unique<FakeBackend>(&raw));
  h.set_mapping(Mapping::defaults());
  auto poll = [&]() { std::this_thread::sleep_for(std::chrono::milliseconds(3)); h.poll(1); };

  // --- toggle mode: a press flips ZL on, it stays after the release, the next press flips it off
  Mapping m = Mapping::defaults();
  m.toggle_zl = true;
  h.set_mapping(m);
  raw.keys.set((size_t)key_from_name("LShift"));
  poll();
  CHECK(h.pad().buttons & bit(kZL));
  raw.keys.reset();
  poll();
  CHECK(h.pad().buttons & bit(kZL));
  raw.keys.set((size_t)key_from_name("LShift"));
  poll();
  CHECK(!(h.pad().buttons & bit(kZL)));   // second press flips it off
  raw.keys.reset();
  poll();
  CHECK(!(h.pad().buttons & bit(kZL)));
  raw.keys.set((size_t)key_from_name("Space"));   // ZR keeps hold behavior
  poll();
  CHECK(h.pad().buttons & bit(kZR));
  raw.keys.reset();
  poll();
  CHECK(!(h.pad().buttons & bit(kZR)));
  h.set_mapping(Mapping::defaults());

  // --- menu open: the game sees an idle pad, the mapped state stays in preview()
  raw = RawInput{};
  raw.keys.set((size_t)key_from_name("L"));
  raw.keys.set((size_t)key_from_name("D"));
  poll();
  CHECK(h.pad().buttons & bit(kA));
  h.set_menu_open(true);
  poll();
  CHECK_EQ(h.pad().buttons, 0u);
  CHECK_EQ(h.pad().lx, 0);
  CHECK(h.pad().connected);
  CHECK(h.preview().buttons & bit(kA));
  CHECK(h.preview().lx > 30000);
  raw.mouse_in_window = true; raw.mouse_buttons = 1; raw.mouse_x = raw.mouse_y = 0.5f;
  poll();
  CHECK_EQ(h.touch().count, 0);
  h.set_menu_open(false);
  poll();
  CHECK(h.pad().buttons & bit(kA));
  raw = RawInput{};

  // --- rebinding capture: held sources are ignored, device filter, F1 never captured, one result
  raw.keys.set((size_t)key_from_name("Q"));          // already held when the capture starts
  poll();
  h.capture_begin(1u << kDevKeyboard);
  CHECK(h.capturing());
  poll();
  Source got;
  CHECK(!h.capture_result(&got));
  raw.keys.set((size_t)key_from_name("F1"));
  poll();
  CHECK(!h.capture_result(&got));                    // F1 belongs to the menu
  raw.keys.reset();
  raw.pad.set(kPadSouth);                            // a controller press while a keyboard slot waits
  poll();
  CHECK(!h.capture_result(&got) && h.capturing());
  raw.pad.reset();
  poll();
  raw.keys.set((size_t)key_from_name("Q"));          // released and pressed again: now it counts
  poll();
  CHECK(h.capture_result(&got));
  CHECK(got == SRC("key:Q"));
  CHECK(!h.capturing());
  CHECK(!h.capture_result(&got));                    // reported once
  raw = RawInput{};
  poll();
  h.capture_begin(1u << kDevController);
  raw.pad_connected = true; raw.ry = -0.9f;          // an axis counts as a source
  poll();
  CHECK(h.capture_result(&got));
  CHECK(got == SRC("pad:rstick_down"));
  raw = RawInput{};
  poll();
  h.capture_begin(1u << kDevMouse);
  raw.mouse_buttons = 1u << kMouseWheelDown;
  poll();
  CHECK(h.capture_result(&got));
  CHECK(got == SRC("mouse:wheel_down"));
  raw = RawInput{};
  poll();
  h.capture_begin(1u << kDevKeyboard);
  h.capture_cancel();
  CHECK(!h.capturing());

  // --- the wheel arrives as window messages: a notch is a short press
  post_wheel(120);
  poll();
  CHECK(h.raw().mouse_buttons & (1u << kMouseWheelUp));
  std::this_thread::sleep_for(std::chrono::milliseconds(130));
  poll();
  CHECK(!(h.raw().mouse_buttons & (1u << kMouseWheelUp)));
  m = Mapping::defaults();
  slot_set(m.buttons[kR], kDevMouse, 0, SRC("mouse:wheel_down"));
  h.set_mapping(m);
  post_wheel(-120);
  poll();
  CHECK(h.pad().buttons & bit(kR));

  // --- mouse-look velocity comes from the movement of the last 40 ms
  m = Mapping::defaults();
  m.mouse_look = true;
  h.set_mapping(m);
  raw = RawInput{};
  raw.mouse_dx = 30;                                  // 30 px in one poll
  poll();
  raw.mouse_dx = 0;
  CHECK(h.raw().mouse_vx > 300);
  CHECK(h.pad().rx > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  poll();
  CHECK_EQ(h.pad().rx, 0);                            // movement stopped: the stick returns to center

  // --- chord: Back + Start for 0.5 s toggles the menu once
  raw = RawInput{};
  raw.pad_connected = true;
  raw.pad.set(kPadBack); raw.pad.set(kPadStart);
  poll();
  CHECK(!h.consume_menu_toggle());
  std::this_thread::sleep_for(std::chrono::milliseconds(560));
  poll();
  CHECK(h.consume_menu_toggle());
  poll();
  CHECK(!h.consume_menu_toggle());                    // still held: no repeat
  raw.pad.reset();
  poll();
  raw.pad.set(kPadBack); raw.pad.set(kPadStart);
  poll();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  poll();
  raw.pad.reset();                                    // released early: nothing
  poll();
  CHECK(!h.consume_menu_toggle());

  // --- injected sources (scripted UI test) behave like a device
  raw = RawInput{};
  h.set_mapping(Mapping::defaults());
  h.inject_source(SRC("key:L"), true);
  poll();
  CHECK(h.pad().buttons & bit(kA));
  h.inject_source(SRC("key:L"), false);
  poll();
  CHECK(!(h.pad().buttons & bit(kA)));
}

// ---- other services
static void test_services() {
  g_test = "services";
  // registry: every symbol once, non-empty, resolvable
  std::set<std::string> seen;
  for (auto& e : overrides()) {
    CHECK(e.symbol && *e.symbol && e.fn);
    if (!seen.insert(e.symbol).second) { std::printf("  duplicate override %s\n", e.symbol); CHECK(false); }
  }
  CHECK(overrides().size() > 150);

  config().uid[0] = kUid[0]; config().uid[1] = kUid[1];
  config().user_name = "Default";
  int count = 0;
  CHECK_EQ(call("_ZN2nn7account12GetUserCountEPi", {P(&count)}), 0u);
  CHECK_EQ(count, 1);
  uint8_t handle[24] = {};
  CHECK_EQ(call("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", {P(handle)}) & 1, 1u);
  uint64_t uid[2] = {};
  CHECK_EQ(call("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", {P(uid), P(handle)}), 0u);
  CHECK(uid[0] == kUid[0] && uid[1] == kUid[1]);
  char nick[33] = {};
  call("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", {P(nick), P(uid)});
  CHECK(std::string(nick) == "Default");
  bool nsa = true;
  call("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", {P(&nsa), P(handle)});
  CHECK(!nsa);

  CHECK_EQ(call("_ZN2nn2oe16GetOperationModeEv"), 1u);  // docked
  CHECK(call("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", {0, 0x20004}) == call("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", {0, 0x20004}));
  uint32_t msg = 99;
  CHECK_EQ(call("_ZN2nn2oe25TryPopNotificationMessageEPj", {P(&msg)}) & 1, 0u);
  config().language = 1;
  uint64_t lc = call("_ZN2nn2oe18GetDesiredLanguageEv");
  CHECK(std::string(reinterpret_cast<char*>(&lc), 5) == "en-US");
  int32_t lang = 1;
  CHECK_EQ(call("_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", {P(&lc), P(&lang)}) & 1, 1u);
  lang = 2;
  CHECK_EQ(call("_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", {P(&lc), P(&lang)}) & 1, 0u);

  // time: 2024-03-01 12:34:56 UTC = 1709296496
  uint8_t cal[8] = {};
  int16_t y = 2024; std::memcpy(cal, &y, 2);
  cal[2] = 3; cal[3] = 1; cal[4] = 12; cal[5] = 34; cal[6] = 56;
  CHECK_EQ((long long)call("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", {P(cal)}), 1709296496ll);
  int64_t posix = 1709296496;
  uint8_t rule[0x4000], out_cal[8] = {}, info[24] = {};
  call("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", {P(rule), P("UTC")});
  call("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", {P(out_cal), P(info), P(&posix), P(rule)});
  CHECK(std::memcmp(cal, out_cal, 7) == 0);
  CHECK_EQ(load<int32_t>(info), 5);  // Friday
  CHECK_EQ(load<int32_t>(info + 4), 60);  // day of year (0-based)

  // album PNG
  config().screenshot_dir = (g_tmp / "shots").string();
  std::vector<uint8_t> px(1280 * 720 * 4, 0x80);
  call("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE", {P(px.data()), px.size(), 0, 0, 0});
  int pngs = 0;
  for (auto& e : stdfs::directory_iterator(config().screenshot_dir)) {
    std::string d = slurp(e.path());
    CHECK(d.size() > 8 && d.compare(1, 3, "PNG") == 0);
    ++pngs;
  }
  CHECK_EQ(pngs, 1);

  // offline services fail cleanly
  CHECK(call("_ZN2nn6socket6SocketEiii", {2, 1, 0}) == (uint64_t)-1);
  CHECK_EQ(call("_ZN2nn6socket12GetLastErrnoEv"), 50u);  // ENETDOWN
  CHECK_EQ(call("_ZN2nn4nifm18IsNetworkAvailableEv") & 1, 0u);
  CHECK_EQ(call("_ZN2nn6socket9InetHtonlEj", {0x01020304}), 0x04030201u);
  char ip[16] = {};
  CHECK_EQ(call("_ZN2nn6socket8InetNtopEiPKvPcj", {2, P("\x7f\x00\x00\x01"), P(ip), 16}), P(ip));
  CHECK(std::string(ip) == "127.0.0.1");
  uint32_t in4 = 0;
  CHECK_EQ(call("_ZN2nn6socket8InetPtonEiPKcPv", {2, P("10.20.30.40"), P(&in4)}), 1u);
  CHECK_EQ(in4, 0x281e140au);
  uint8_t rnd[32] = {};
  call("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", {P(rnd), 32});
  int nz = 0;
  for (uint8_t b : rnd) nz += b != 0;
  CHECK(nz > 8);
  CHECK(call("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", {0, 0}) != 0);  // canceled

  // the "not implemented" backstop logs and (in tests) returns through the hook instead of aborting
  static const char* last = nullptr;
  unimplemented_hook = [](const char* s) { last = s; };
  Cpu c{};
  c.x[0] = 7;
  unimplemented(&c, "_ZN2nn3foo3barEv");
  CHECK(last && std::string(last) == "_ZN2nn3foo3barEv");
  CHECK_EQ(c.x[0], 0u);
  unimplemented_hook = nullptr;
  static std::string fatal_msg;
  fatal_hook = [](const char* m) { fatal_msg = m; };
  call("_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", {P("expr"), P("func"), P("file.cpp"), 12});
  CHECK(fatal_msg.find("expr") != std::string::npos && fatal_msg.find("file.cpp") != std::string::npos);
  fatal_hook = nullptr;
}


// ---- save profiles
static std::string read_via_fs(const std::string& save_root, const std::string& path) {
  fs::Fs f;
  f.set_save_root(save_root);
  if (f.mount_save("savedata", kUid) != 0u) return "<mount failed>";
  std::string body;
  if (!get(f, "savedata:/" + path, &body)) body = "<missing>";
  f.unmount("savedata");
  return body;
}
static void write_via_fs(const std::string& save_root, const std::string& path, const std::string& text, bool create) {
  fs::Fs f;
  f.set_save_root(save_root);
  f.mount_save("savedata", kUid);
  if (create) f.create_file("savedata:/" + path, 64);
  put(f, "savedata:/" + path, text);
  f.commit("savedata");
  f.unmount("savedata");
}

static int g_picked_count = 0;
static int pick_new(const hle::profiles::PickItem*, int n) { g_picked_count = n; return n; }   // "New profile"
static int pick_first(const hle::profiles::PickItem*, int n) { g_picked_count = n; return 0; }

static void test_profiles() {
  g_test = "profiles";
  namespace pr = hle::profiles;
  stdfs::path base = g_tmp / "profiles";
  std::string err;

  // --- migration of the old single save: copy, verify, rename, never delete
  stdfs::path legacy = base / "legacy_save";
  write_via_fs(legacy.string(), "0000000100000002", "legacy progress", true);
  stdfs::path root = base / "profiles";
  {
    pr::Store s(root.string());
    CHECK(s.load());
    CHECK(s.list().empty());
    CHECK(s.migrate_legacy(legacy.string(), &err));
    CHECK_EQ((int)s.list().size(), 1);
    CHECK(s.list()[0].name == "Profile 1");
    CHECK(read_via_fs(s.save_dir(s.list()[0].id), "0000000100000002") == "legacy progress");
    CHECK(!stdfs::exists(legacy));                       // renamed away ...
    int migrated = 0;
    for (auto& e : stdfs::directory_iterator(base)) if (e.path().filename().string().rfind("legacy_save.migrated-", 0) == 0) ++migrated;
    CHECK_EQ(migrated, 1);                               // ... but kept
  }
  // --- persistence, create, rename, name rules
  pr::Store s(root.string());
  CHECK(s.load());
  CHECK_EQ((int)s.list().size(), 1);
  std::string p1 = s.list()[0].id;
  std::string p2 = s.create("Second", &err);
  CHECK(!p2.empty());
  CHECK(s.create("second", &err).empty());               // duplicate, case-insensitive
  CHECK(s.create("  ", &err).empty());
  CHECK(s.create("_trash", &err).empty());
  CHECK(!s.rename(p2, "Profile 1", &err));
  CHECK(s.rename(p2, "Photo run", &err));
  { pr::Store again(root.string()); CHECK(again.load()); CHECK_EQ((int)again.list().size(), 2); CHECK(again.find("photo run") != nullptr); }
  CHECK(s.next_free_name() == "Profile 2");

  // --- duplicate copies one consistent generation; later changes to the source do not leak into the copy
  write_via_fs(s.save_dir(p1), "0000000100000002", "newer progress", false);
  std::string p3 = s.duplicate(p1, "Profile 1 copy", &err);
  CHECK(!p3.empty());
  CHECK(read_via_fs(s.save_dir(p3), "0000000100000002") == "newer progress");
  write_via_fs(s.save_dir(p1), "0000000100000002", "even newer", false);
  CHECK(read_via_fs(s.save_dir(p3), "0000000100000002") == "newer progress");
  CHECK(read_via_fs(s.save_dir(p1), "0000000100000002") == "even newer");
  // a save with a torn pointer file still imports and mounts (the layer repairs it from the newest complete generation)
  {
    stdfs::path torn = base / "torn";
    write_via_fs(torn.string(), "0000000100000002", "torn test", true);
    for (auto& e : stdfs::recursive_directory_iterator(torn)) if (e.path().filename() == "CURRENT") { std::ofstream(e.path(), std::ios::trunc) << "garbage"; }
    std::string id = s.import_folder(torn.string(), "Torn import", &err);
    CHECK(!id.empty());
    CHECK(read_via_fs(s.save_dir(id), "0000000100000002") == "torn test");
  }
  // an import of a folder with no save fails and leaves no profile behind
  { stdfs::create_directories(base / "empty_dir"); size_t n = s.list().size(); CHECK(s.import_folder((base / "empty_dir").string(), "Nothing", &err).empty()); CHECK_EQ(s.list().size(), n); }

  // --- clear moves the save to the trash; the profile stays and is empty
  CHECK(s.has_data(p1));
  int64_t sz = s.size_bytes(p1);
  CHECK(sz > 0);
  CHECK(s.clear(p1, &err));
  CHECK(!s.has_data(p1));
  CHECK(s.find(p1) != nullptr);
  auto tr = s.trash_list();
  CHECK_EQ((int)tr.size(), 1);
  CHECK(tr[0].kind == "clear" && tr[0].name == "Profile 1" && tr[0].size == sz);
  CHECK(read_via_fs(s.save_dir(p1), "0000000100000002") == "<missing>");   // a cleared profile mounts fresh

  // --- restore brings the data back as a new profile
  std::string r = s.restore(tr[0].folder, &err);
  CHECK(!r.empty());
  CHECK(s.find(r)->name == "Profile 1 (restored)");
  CHECK(read_via_fs(s.save_dir(r), "0000000100000002") == "even newer");
  CHECK_EQ((int)s.trash_list().size(), 0);

  // --- delete: moved to the trash, entry gone, last_used moves on
  s.set_last_used(p3);
  CHECK(s.remove(p3, &err));
  CHECK(s.find(p3) == nullptr);
  CHECK(s.last_used() != p3);
  CHECK(!stdfs::exists(stdfs::path(s.save_dir(p3)).parent_path()));
  tr = s.trash_list();
  CHECK_EQ((int)tr.size(), 1);
  CHECK(tr[0].kind == "delete");

  // --- the trash keeps the newest 10
  for (int i = 0; i < 13; ++i) {
    std::string id = s.create("tmp" + std::to_string(i), &err);
    write_via_fs(s.save_dir(id), "0000000100000002", "x", true);
    CHECK(s.remove(id, &err));
  }
  CHECK_EQ((int)s.trash_list().size(), 10);

  // --- pending actions for the mounted profile are applied at the next startup, before anything mounts
  s.queue("clear", p2);
  write_via_fs(s.save_dir(p2), "0000000100000002", "will be cleared", true);
  { pr::Store q(root.string()); CHECK(q.load()); CHECK(q.is_pending(p2, "clear")); }
  CHECK(s.has_data(p2));                                 // queueing touched nothing
  pr::Active a = pr::startup(root.string(), "", "", nullptr, false);
  CHECK(a.on);
  { pr::Store q(root.string()); CHECK(q.load()); CHECK(!q.is_pending(p2)); CHECK(!q.has_data(p2)); CHECK(q.find(p2) != nullptr); }

  // --- startup: selection by name, picker with several profiles, "New profile" from the picker
  g_picked_count = 0;
  pr::Active b = pr::startup(root.string(), "", "Photo run", pick_new, false);
  CHECK(b.on && b.name == "Photo run");
  CHECK_EQ(g_picked_count, 0);                           // a named profile skips the picker
  pr::Active c = pr::startup(root.string(), "", "", pick_new, false);
  CHECK(g_picked_count >= 2);
  CHECK(c.on && c.name.rfind("Profile ", 0) == 0);
  pr::Active d = pr::startup(root.string(), "", "", pick_first, false);
  CHECK(d.on);
  pr::Active e2 = pr::startup(root.string(), "", "Brand new", nullptr, false);   // an unknown name is created
  CHECK(e2.on && e2.name == "Brand new");
  stdfs::path root1 = base / "single";
  g_picked_count = -1;
  pr::Active f1 = pr::startup(root1.string(), "", "", pick_new, false);          // one profile, no hold key: no picker
  CHECK(f1.on && f1.name == "Profile 1");
  CHECK_EQ(g_picked_count, -1);
  pr::startup(root1.string(), "", "", pick_first, true);                         // hold key: the picker is shown
  CHECK_EQ(g_picked_count, 1);

  // --- an invalid profiles.json is kept aside and the profiles are rebuilt from their folders
  { std::ofstream(root / "profiles.json", std::ios::trunc) << "{ not json"; }
  { pr::Store q(root.string()); CHECK(!q.load()); CHECK(!q.message().empty()); CHECK(stdfs::exists(root / "profiles.json.invalid")); CHECK(!q.list().empty()); }

  // --- startup migrates the old save once; the second start does not migrate again
  stdfs::path legacy2 = base / "legacy2";
  write_via_fs(legacy2.string(), "0000000100000002", "again", true);
  stdfs::path root2 = base / "profiles2";
  pr::Active g1 = pr::startup(root2.string(), legacy2.string(), "", nullptr, false);
  CHECK(g1.on && g1.name == "Profile 1" && !g1.note.empty());
  CHECK(read_via_fs(g1.save_dir, "0000000100000002") == "again");
  pr::Active g2 = pr::startup(root2.string(), legacy2.string(), "", nullptr, false);
  CHECK(g2.on && g2.id == g1.id);

  pr::set_active(pr::Active());
  CHECK(!pr::active().on);
}

// ---- gyro aim, rumble and per-profile settings (fake sensor feed: the owner has no gyro controller)
static void test_padfx() {
  g_test = "padfx";
  using namespace input;
  float sx, sy;
  gyro_to_stick(0, 0, 1.0f, false, false, &sx, &sy);
  CHECK(sx == 0 && sy == 0);
  gyro_to_stick(1.0f, -1.0f, 1.0f, false, false, &sx, &sy);   // inside the dead zone
  CHECK(sx == 0 && sy == 0);
  gyro_to_stick(-kGyroFullDegPerSec, 0, 1.0f, false, false, &sx, &sy);   // turn right at full rate: stick right, full
  CHECK(std::fabs(sx - 1.0f) < 1e-4f && sy == 0);
  gyro_to_stick(kGyroFullDegPerSec, 0, 1.0f, false, false, &sx, &sy);
  CHECK(std::fabs(sx + 1.0f) < 1e-4f);
  gyro_to_stick(0, kGyroFullDegPerSec * 0.5f, 1.0f, false, false, &sx, &sy);   // pitch up: stick up, about half
  CHECK(sy > 0.4f && sy < 0.6f);
  float hx, hy;
  gyro_to_stick(0, kGyroFullDegPerSec * 0.5f, 2.0f, false, true, &hx, &hy);     // double sensitivity, inverted Y
  CHECK(hy < 0 && std::fabs(hy + std::min(1.0f, sy * 2.0f)) < 1e-4f);
  gyro_to_stick(-1000, 1000, 4.0f, false, false, &sx, &sy);                     // clamped
  CHECK(sx == 1.0f && sy == 1.0f);

  // through the hub: fake gyro feed moves the right stick only when enabled
  static RawInput raw;
  raw = RawInput{};
  Hub h;
  auto fb = std::make_unique<FakeBackend>(&raw);
  FakeBackend* fbp = fb.get();
  h.set_backend(std::move(fb));
  h.set_mapping(Mapping::defaults());
  auto poll = [&]() { std::this_thread::sleep_for(std::chrono::milliseconds(3)); h.poll(1); };
  raw.pad_connected = true;
  raw.gyro_present = true;
  raw.gyro_y = -kGyroFullDegPerSec * 0.5f;   // turning right
  pad_fx().gyro_on = false;
  poll();
  CHECK(h.pad().rx == 0);                    // off by default
  pad_fx().gyro_on = true;
  pad_fx().gyro_sens = 1.0f;
  poll();
  CHECK(h.pad().rx > kStickMax * 4 / 10 && h.pad().rx < kStickMax * 6 / 10);
  CHECK(h.pad().ry == 0);
  raw.gyro_present = false;                  // pad without a gyro
  poll();
  CHECK(h.pad().rx == 0);
  pad_fx().gyro_on = false;

  // rumble: scaled and switched off by the settings, forwarded to the backend otherwise
  pad_fx().rumble_on = true; pad_fx().rumble_strength = 1.0f;
  h.rumble(0.5f, 0.25f);
  CHECK(fbp->rumble_low == 0.5f && fbp->rumble_high == 0.25f);
  pad_fx().rumble_strength = 0.5f;
  h.rumble(0.5f, 0.25f);
  CHECK(std::fabs(fbp->rumble_low - 0.25f) < 1e-5f && std::fabs(fbp->rumble_high - 0.125f) < 1e-5f);
  pad_fx().rumble_on = false;
  h.rumble(1.0f, 1.0f);
  CHECK(fbp->rumble_low == 0 && fbp->rumble_high == 0);
  pad_fx().rumble_on = true; pad_fx().rumble_strength = 1.0f;

  // settings: persisted per file, live values pushed to pad_fx, env vars exported at load only when unset, forgotten on restart
  std::string f = (g_tmp / "settings_test.cfg").string();
  stdfs::remove(f);
  _putenv_s("SNAP_LOOK", "");
  _putenv_s("SNAP_SSAO_QUALITY", "");
  settings::use_path_for_test(f);
  CHECK(!pad_fx().gyro_on.load() && pad_fx().rumble_on.load());
  settings::set_bool("gyro_on", true);
  settings::set_float("gyro_sens", 2.5f);
  settings::set("look", "warm");
  settings::set("ssao_quality", "high");
  CHECK(pad_fx().gyro_on.load() && std::fabs(pad_fx().gyro_sens.load() - 2.5f) < 1e-4f);
  settings::use_path_for_test(f);            // re-read from disk
  CHECK(settings::get("look") == "warm" && settings::get_bool("gyro_on", false) && std::fabs(settings::get_float("gyro_sens", 0) - 2.5f) < 1e-3f);
  CHECK(!std::getenv("SNAP_LOOK") || !*std::getenv("SNAP_LOOK"));   // env is exported only by load(), not while editing
  settings::apply(true);
  CHECK(std::getenv("SNAP_LOOK") && std::string(std::getenv("SNAP_LOOK")).rfind("warm:", 0) == 0);
  CHECK(std::getenv("SNAP_SSAO_QUALITY") && std::string(std::getenv("SNAP_SSAO_QUALITY")) == "high");
  settings::forget_exported_env();
  CHECK(!std::getenv("SNAP_LOOK") || !*std::getenv("SNAP_LOOK"));
  _putenv_s("SNAP_LOOK", "vivid:0.5");       // an env var set by hand wins
  settings::apply(true);
  CHECK(std::string(std::getenv("SNAP_LOOK")) == "vivid:0.5");
  settings::forget_exported_env();
  _putenv_s("SNAP_LOOK", "");
  _putenv_s("SNAP_SSAO_QUALITY", "");
  settings::set_bool("gyro_on", false);
  settings::set_float("gyro_sens", 1.0f);
  stdfs::remove(f);
}

int main(int argc, char** argv) {
  if (argc > 1) g_root = argv[1];
  else g_root = stdfs::path(__FILE__).parent_path().parent_path().parent_path();
  g_root = stdfs::absolute(g_root);
  g_tmp = stdfs::temp_directory_path() / ("snap_hle_test_" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()));
  stdfs::create_directories(g_tmp);
  set_log_sink([](const char*, size_t) {});  // keep the output readable; failures print through printf
  apply_defaults();
  config().headless = true;

  struct T { const char* name; void (*fn)(); } tests[] = {
      {"fs.rom", test_fs_rom},
      {"fs.overlay", test_fs_overlay},
      {"save.roundtrip", test_save_roundtrip},
      {"save.crash_midwrite", test_save_crash_midwrite},
      {"save.crash_commit", test_save_crash_commit_stages},
      {"save.backups", test_save_backups},
      {"hid", test_hid},
      {"input.mapping", test_input_mapping},
      {"input.config", test_input_config},
      {"input.hub", test_input_hub},
      {"profiles", test_profiles},
      {"padfx", test_padfx},
      {"services", test_services},
  };
  int before_failed;
  for (auto& t : tests) {
    before_failed = g_failed;
    int before_checks = g_checks;
    try { t.fn(); }
    catch (const std::exception& e) { std::printf("  FAIL [%s] exception: %s\n", t.name, e.what()); ++g_failed; }
    catch (...) { std::printf("  FAIL [%s] unknown exception\n", t.name); ++g_failed; }
    std::printf("%-22s %s (%d checks)\n", t.name, g_failed == before_failed ? "PASS" : "FAIL", g_checks - before_checks);
  }
  std::error_code ec;
  stdfs::remove_all(g_tmp, ec);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
