// SPDX-License-Identifier: MIT
// Shiny Pokemon mod, renderer side (docs/shiny-mod-plan.md). Cosmetic only: a draw that samples a known body texture of a shiny
// instance samples a recolored twin of it instead. Nothing in the guest, the saves or the scoring is touched.
//   SNAP_SHINY=1            master switch (off by default)
//   SNAP_SHINY_ODDS=<n>     1 in n per instance (default 4096; 1 = every instance)
//   SNAP_SHINY_FORCE=<key>  hex data key of one body texture whose draws are always shiny (testing)
//   SNAP_SHINY_TRACE=<key>  log the draw state of draws sampling that key (hex key, or "all" = one line per texture)
//   SNAP_SHINY_DIR=<dir>    pack folder (default %APPDATA%\SnapRecomp\mods\shiny-pokemon), holds shiny_table.txt + twin .nutexb files
#include "nvn_internal.h"
#include "nvn_shiny.h"
#include <windows.h>
#include <cinttypes>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace nvn {
namespace {
struct TwinFile { uint32_t w = 0, h = 0, mips = 0; std::string file, species; const uint8_t* data = nullptr; bool tried = false; };
struct Cfg {
  bool on = false; unsigned odds = 4096; bool forceAll = false; uint64_t force = 0; bool trace = false, traceAll = false; uint64_t traceKey = 0;
  std::unordered_map<uint64_t, TwinFile> table; std::string dir;
};
const Cfg& C() {
  static const Cfg c = [] {
    Cfg c;
    const char* e = getenv("SNAP_SHINY");
    c.on = e && *e && *e != '0';
    if ((e = getenv("SNAP_SHINY_ODDS")) && *e) c.odds = (unsigned)std::max(1ul, strtoul(e, nullptr, 0));
    if ((e = getenv("SNAP_SHINY_FORCE")) && *e) { c.force = strtoull(e, nullptr, 16); c.forceAll = !c.force; }
    if ((e = getenv("SNAP_SHINY_TRACE")) && *e && *e != '0') {
      c.trace = true;
      if (!strcmp(e, "all") || !strcmp(e, "1")) c.traceAll = true; else c.traceKey = strtoull(e, nullptr, 16);
    }
    if ((e = getenv("SNAP_SHINY_DIR")) && *e) c.dir = e;
    else if ((e = getenv("APPDATA")) && *e) c.dir = std::string(e) + "\\SnapRecomp\\mods\\shiny-pokemon";
    if (c.on) {
      std::ifstream f(c.dir + "\\shiny_table.txt");
      std::string line;
      while (std::getline(f, line)) {
        std::istringstream is(line); std::string k; TwinFile t;
        if (line.empty() || line[0] == '#' || !(is >> k >> t.w >> t.h >> t.mips >> t.file)) continue;
        is >> t.species;
        c.table[strtoull(k.c_str(), nullptr, 16)] = t;
      }
      fprintf(stderr, "[shiny] on, odds 1/%u, %zu twin textures from %s%s\n", c.odds, c.table.size(), c.dir.c_str(), c.forceAll ? ", force all" : (c.force ? ", force key" : ""));
    }
    return c;
  }();
  return c;
}
uint64_t Fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
std::mutex g_mu;
// M3 data (photo badge): the last present at which a shiny twin was drawn, and which species. A photo-time consumer asks
// snap_shiny_last(); nothing here changes rendering.
std::atomic<int> g_lastFrame{-1};
std::atomic<uint64_t> g_lastKey{0};
std::string g_lastSpecies;
std::mutex g_spMu;   // guards g_lastSpecies only (NoteShiny can run under g_mu)
void NoteShiny(uint64_t key, const std::string& sp) {
  g_lastFrame.store(g_presentCount, std::memory_order_relaxed);
  if (g_lastKey.exchange(key) != key) { std::lock_guard<std::mutex> g(g_spMu); g_lastSpecies = sp; }
}

// Per-instance roll (M2). An instance is identified by its first vertex buffer address; an address not drawn for 600 presents
// is a new instance (arena addresses get reused), which takes the next ordinal of its species. The roll is a hash of
// (seed, species key, ordinal): same seed and same spawn order give the same shinies. Never touches the game's own RNG.
// Instances: a Pokemon draws several meshes (body, heads) from separate vertex buffers, so the roll belongs to a cluster of
// buffers that the game created together (same thread, consecutive buffer sequence numbers, within one or two presents),
// not to one vb. BufferFinalize (nvn_gfx.cpp) ends a buffer's life: its vb leaves its cluster and an empty cluster is gone, so
// an address reused by a later spawn is rolled anew and one still alive keeps its color for as long as it lives.
struct BufRec { uint64_t size = 0; uint64_t seq = 0; unsigned tid = 0; int present = 0; };
struct Cluster { bool shiny = false; int refs = 0; uint64_t key = 0; };
std::mutex g_bufMu;
std::map<uint64_t, BufRec> g_bufs;                 // buffer address -> creation record
std::unordered_map<uint64_t, Cluster> g_clusters;   // instance buffer address -> roll
std::unordered_map<uint64_t, unsigned> g_ordinal;

uint64_t g_bufSeq = 0;
uint64_t Seed() {
  static const uint64_t s = [] {
    if (const char* e = getenv("SNAP_SHINY_SEED")) if (*e) return (uint64_t)strtoull(e, nullptr, 0) * 0x9E3779B97F4A7C15ull + 1;
    // profile seed: random once, kept beside the pack (host side, never in the game save); the launch counter makes each launch roll anew
    const std::string f = C().dir + "\\profile_seed.txt";
    unsigned long long seed = 0, launch = 0;
    if (FILE* fp = fopen(f.c_str(), "r")) { if (fscanf(fp, "%llx %llu", &seed, &launch) != 2) seed = launch = 0; fclose(fp); }
    if (!seed) seed = ((uint64_t)GetTickCount64() << 20) ^ (uint64_t)GetCurrentProcessId() ^ 0xA5F152C3D0B7ull;
    if (FILE* fp = fopen(f.c_str(), "w")) { fprintf(fp, "%llx %llu\n", seed, launch + 1); fclose(fp); }
    return seed ^ (launch * 0x9E3779B97F4A7C15ull);
  }();
  return s;
}
bool RollInstance(uint64_t key, const ShinyDraw& d, const std::string& species) {
  (void)key;
  const uint64_t vb0 = d.vb0;
  std::lock_guard<std::mutex> g(g_mu);
  std::lock_guard<std::mutex> gb(g_bufMu);
  const int now = g_presentCount;
  // The instance is the buffer that holds this vertex buffer: the game creates ONE buffer per spawned model (trace 3-20261114_024944:
  // every mesh of a Dodrio, body and heads, lies inside one buffer; the species' textures differ per mesh, the buffer does not).
  uint64_t cid = vb0; const BufRec* me = nullptr;
  auto it = g_bufs.upper_bound(vb0);
  if (it != g_bufs.begin()) { --it; if (vb0 < it->first + it->second.size) { cid = it->first; me = &it->second; } }
  auto f = g_clusters.find(cid);
  if (f == g_clusters.end()) {
    const uint64_t sk = Fnv(reinterpret_cast<const uint8_t*>(species.data()), species.size());
    const unsigned ord = g_ordinal[sk]++;
    uint64_t h = Seed() ^ sk; h ^= ord * 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 29;
    Cluster cl; cl.shiny = (h % C().odds) == 0; cl.refs = 1; cl.key = sk;
    f = g_clusters.emplace(cid, cl).first;
    fprintf(stderr, "[shiny] roll: %s ordinal %u buffer %llx (size %llx) first mesh vb %llx at present %d: %s (1 in %u)\n", species.c_str(), ord, (unsigned long long)cid,
            me ? (unsigned long long)me->size : 0ull, (unsigned long long)vb0, now, cl.shiny ? "SHINY" : "normal", C().odds);
  }
  const bool shiny = f->second.shiny;
  {   // diagnostics: every new (vertex buffer, render target size, program) combination that draws this instance
    static std::set<std::tuple<uint64_t, int, int, int>> ctx; static int n = 0;
    if (ctx.insert({vb0, d.rtW, d.rtH, d.prog}).second && n++ < 400)
      fprintf(stderr, "[shiny] ctx: %s vb %llx buffer %llx rt %dx%d prog %d at present %d (%s)\n", species.c_str(), (unsigned long long)vb0, (unsigned long long)cid, d.rtW, d.rtH, d.prog, now, shiny ? "shiny" : "normal");
  }
  return shiny;
}
}  // namespace

bool ShinyActive() { return C().on || C().trace; }

void ShinyBufferInit(uint64_t va, uint64_t size) {
  std::lock_guard<std::mutex> g(g_bufMu);
  BufRec r; r.size = size; r.seq = ++g_bufSeq; r.tid = GetCurrentThreadId(); r.present = g_presentCount;
  g_bufs[va] = r;
}
void ShinyBufferFinalize(uint64_t va, uint64_t size) {
  (void)size;
  std::lock_guard<std::mutex> g(g_mu);
  std::lock_guard<std::mutex> gb(g_bufMu);
  g_bufs.erase(va);
  if (g_clusters.erase(va)) { static int n = 0; if (n++ < 60) fprintf(stderr, "[shiny] instance buffer %llx ended at present %d\n", (unsigned long long)va, g_presentCount); }
}

TextureImpl* ShinySubstitute(TextureImpl* t, const ShinyDraw& d) {
  const Cfg& c = C();
  if (!t->shTried && !t->uploaded) {
    t->shTried = true;
    const TextureBuilder& b = t->b;
    const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
    if (!src && b.pool && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET | TF_LINEAR)))
      if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
    if (src && t->scale == 1.0f && b.samples <= 1 && b.target == TT_2D && b.width >= 64 && b.height >= 64) {
      t->shKey = Fnv(src, 4096);
      if (c.traceAll) {
        static int n = 0;
        if (n++ < 4000) fprintf(stderr, "[shiny] tex key %016" PRIx64 " %dx%d f%x lv%d tex %p\n", t->shKey, b.width, b.height, b.format, b.levels, (void*)t);
      }
    }
  }
  if (!t->shKey) return t;
  const uint64_t key = t->shKey;
  if (c.trace && !c.traceAll && key == c.traceKey) {
    static std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>> seen; static int n = 0;
    uint64_t us = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) { us = (us ^ d.uboOff[i]) * 1099511628211ull; us = (us ^ d.uboRange[i]) * 1099511628211ull; }
    std::lock_guard<std::mutex> g(g_mu);
    if (seen.insert({d.vb0, d.vb1, us, (uint64_t)(uintptr_t)t}).second && n++ < 600) {
      fprintf(stderr, "[shiny-trace] f=%d key=%016" PRIx64 " tex=%p vb0=%llx vb1=%llx\n", g_presentCount, key, (void*)t, (unsigned long long)d.vb0, (unsigned long long)d.vb1);
    }
  }
  if (!c.on) return t;
  auto it = c.table.find(key);
  if (it == c.table.end()) return t;
  const bool shiny = c.forceAll || (c.force && key == c.force) || c.odds == 1 || RollInstance(key, d, it->second.species);
  if (!shiny) return t;
  if (t->shAlt) { NoteShiny(key, it->second.species); return t->shAlt; }
  std::lock_guard<std::mutex> g(g_mu);
  if (t->shAlt) return t->shAlt;
  TwinFile& tw = const_cast<TwinFile&>(it->second);
  if (!tw.tried) {
    tw.tried = true;
    if (FILE* f = fopen((c.dir + "\\" + tw.file).c_str(), "rb")) {
      fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
      auto* buf = new uint8_t[(size_t)n];   // never freed: the twin image is built from it on first use
      if (fread(buf, 1, (size_t)n, f) == (size_t)n) tw.data = buf;
      fclose(f);
    }
    fprintf(stderr, "[shiny] twin for key %016" PRIx64 " (%s) %s\n", key, tw.species.c_str(), tw.data ? "loaded" : "MISSING");
  }
  const TextureBuilder& b = t->b;
  if (!tw.data || tw.w != (uint32_t)b.width || tw.h != (uint32_t)b.height || tw.mips != (uint32_t)b.levels) return t;
  auto* s = new TextureImpl();
  s->host = t->host; s->fi = t->fi; s->b = b;
  s->b.pool = nullptr; s->b.poolOffset = 0; s->b.packaged = tw.data;
  s->shTried = true;
  t->shAlt = s;
  NoteShiny(key, tw.species);
  return s;
}
}  // namespace nvn

// Present index of the last shiny draw (-1 = none yet) and its species name (copied into out).
extern "C" int snap_shiny_last(char* out, int n) {
  if (out && n > 0) { std::lock_guard<std::mutex> g(nvn::g_spMu); snprintf(out, (size_t)n, "%s", nvn::g_lastSpecies.c_str()); }
  return nvn::g_lastFrame.load(std::memory_order_relaxed);
}

// 1 when a shiny twin was drawn within the last `frames` presents (species name copied into out), else 0 (host/mods/shiny.cpp).
extern "C" int snap_shiny_recent(int frames, char* out, int n) {
  const int last = nvn::g_lastFrame.load(std::memory_order_relaxed);
  if (last < 0 || nvn::g_presentCount - last > frames) return 0;
  if (out && n > 0) { std::lock_guard<std::mutex> g(nvn::g_spMu); snprintf(out, (size_t)n, "%s", nvn::g_lastSpecies.c_str()); }
  return 1;
}
