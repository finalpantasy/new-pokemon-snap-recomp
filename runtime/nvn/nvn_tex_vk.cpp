// SPDX-License-Identifier: MIT
// Static texture upload: Maxwell block-linear deswizzle, ASTC CPU decode / BC7 transcode (with a disk cache).
#include <windows.h>   // [perf] SetThreadPriority for the upload prefetch workers
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "nvn_internal.h"
#include "nvn_tex.h"
#include "os/perf.h"   // SNAP_PERF instrumentation

namespace nvn {
namespace {

// Guest-side layout parameters of a texture (what the packaged data uses), which differ from the Vulkan format's when
// ASTC is transcoded.
struct GuestFmt { uint32_t bpb, bw, bh; };
GuestFmt GuestFormat(const FormatInfo* fi) {
  if (fi->astcW) return {16, fi->astcW, fi->astcH};
  return {fi->bytes, fi->bw, fi->bh};
}
uint32_t Levels(const TextureBuilder& b) {
  uint32_t m = 1, d = (uint32_t)std::max(b.width, std::max(b.height, b.target == TT_3D ? b.depth : 1));
  while (d >>= 1) m++;
  return std::min<uint32_t>((uint32_t)std::max(b.levels, 1), m);
}
void Fnv(const uint8_t* p, size_t n, uint64_t& h1, uint64_t& h2) {
  for (size_t i = 0; i < n; i++) { h1 = (h1 ^ p[i]) * 0x100000001b3ull; h2 = (h2 + p[i] + 1) * 0x9E3779B97F4A7C15ull; h2 ^= h2 >> 29; }
}
}  // namespace

int g_presentCount = 0;
uint64_t g_replaySeq = 1;
static uint64_t HashBytes(const void* p, uint64_t n) {   // 64-bit word mix; cheap enough to run per replay on 1 MB
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint64_t h = 0x9E3779B97F4A7C15ull ^ n, i = 0;
  for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
  for (; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}
// Guest source bytes of a pool-resident texture the CPU may rewrite after creation (nullptr otherwise).
static const uint8_t* DynamicSource(const TextureImpl* t, uint64_t* bytes) {
  const TextureBuilder& b = t->b;
  if (b.packaged || !b.pool || !(b.flags & TF_LINEAR) || (b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET))) return nullptr;
  auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL);
  if (!p || !p->storage || !t->fi || b.samples > 1) return nullptr;
  const uint64_t wb = ((uint64_t)b.width + t->fi->bw - 1) / t->fi->bw, hb = ((uint64_t)b.height + t->fi->bh - 1) / t->fi->bh;
  const uint64_t stride = std::max<uint64_t>((uint64_t)std::max(b.stride, 0), wb * t->fi->bytes);
  if (b.poolOffset < 0 || (uint64_t)b.poolOffset + stride * hb > p->size) return nullptr;
  *bytes = stride * hb;
  return static_cast<const uint8_t*>(p->storage) + b.poolOffset;
}
void DebugDumpTexture(const TextureImpl* t, const char* tag, const void* data, uint64_t bytes, uint32_t w, uint32_t h) {
  static const char* dir = getenv("NVN_TEX_DUMP");
  static int seq = 0;
  if (!dir || !*dir || !data || bytes > (8u << 20)) return;
  char name[512];
  snprintf(name, sizeof name, "%s/%04d_%s_t%p_f%x_%dx%d_%ux%u.raw", dir, seq++, tag, (const void*)t, t->b.format, t->b.width, t->b.height, w, h);
  if (FILE* f = fopen(name, "wb")) { fwrite(data, 1, (size_t)bytes, f); fclose(f); }
}
static FILE* DrawLogFile() {
  static FILE* f = [] { const char* p = getenv("NVN_DRAW_LOG"); return (p && *p) ? fopen(p, "w") : (FILE*)nullptr; }();
  return f;
}
bool DebugDrawLogActive() {
  if (!DrawLogFile()) return false;
  static int a = -1, b = -1;
  static double sa = -1, sb = -1;
  static const auto t0 = std::chrono::steady_clock::now();
  if (a < 0) {
    a = 0; b = 1 << 30;
    if (const char* e = getenv("NVN_DRAW_LOG_FRAMES")) sscanf(e, "%d-%d", &a, &b);
    if (const char* e = getenv("NVN_DRAW_LOG_SEC")) sscanf(e, "%lf-%lf", &sa, &sb);   // wall-clock window since the first draw log query
  }
  if (sa >= 0) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return t >= sa && t < sb;
  }
  return g_presentCount >= a && g_presentCount < b;
}
void DebugDrawLog(const char* fmt, ...) {
  FILE* f = DrawLogFile();
  if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
}

uint64_t TextureStorageSize(const TextureBuilder& b) {
  const FormatInfo* fi = LookupFormat(b.format);
  GuestFmt g = fi ? GuestFormat(fi) : GuestFmt{4, 1, 1};
  uint32_t depth = b.target == TT_3D ? (uint32_t)std::max(b.depth, 1) : 1;
  if ((b.flags & TF_LINEAR) && fi && !fi->astcW && b.target != TT_3D && b.target != TT_CUBEMAP) {   // pitch-linear: stride * rows
    const uint64_t wb = ((uint64_t)std::max(b.width, 1) + fi->bw - 1) / fi->bw, hb = ((uint64_t)std::max(b.height, 1) + fi->bh - 1) / fi->bh;
    const uint64_t stride = std::max<uint64_t>((uint64_t)std::max(b.stride, 0), wb * fi->bytes);
    return (stride * hb + 0xFFF) & ~0xFFFull;
  }
  SurfaceLayout l = ComputeSurfaceLayout((uint32_t)std::max(b.width, 1), (uint32_t)std::max(b.height, 1), depth,
                                         TextureLayerCount(b), (uint32_t)std::max(b.levels, 1), g.bpb, g.bw, g.bh);
  uint64_t total = l.total * (uint64_t)std::max(b.samples, 1);
  return (total + 0xFFF) & ~0xFFFull;
}

// ---- [perf] upload prefetch (perf pass 4, pass2_design.md item 1 step 2) -----------------------------------------------------
// Scene changes upload hundreds of static textures in one replay (bursts of 100 to 400 ms on the presenting thread). Most of
// that is the block-linear deswizzle, which needs no Vulkan and no shared state. TexPrefetch (called from TextureInitialize)
// queues it on a few below-normal workers that deswizzle into host memory; UploadStaticTexture then only checks that the guest
// bytes are unchanged (FastHash of the source, taken before and after the job's deswizzle and again at upload) and copies the
// result into the staging buffer. Anything else (queued but not started, hash mismatch, ASTC, linear, multisample, over budget)
// takes the old synchronous path, so the first sample still sees the final bytes. Staged data is capped at
// SNAP_UPLOAD_PREFETCH_MB (default 384, oldest ready entries dropped first); SNAP_UPLOAD_PREFETCH_MB=0 restores the old path.
namespace {
uint64_t FastHash(const uint8_t* p, uint64_t n) {   // change detection only (xxh3-style accumulate, position-mixed)
  __m256i a0 = _mm256_set_epi64x(1, 2, 3, 4), a1 = _mm256_set_epi64x(5, 6, 7, 8);
  const __m256i k0 = _mm256_set_epi64x((long long)0x9E3779B97F4A7C15ull, (long long)0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ll, (long long)0x85EBCA77C2B2AE63ull);
  const __m256i k1 = _mm256_set_epi64x(0x27D4EB2F165667C5ll, (long long)0xFF51AFD7ED558CCDull, (long long)0xC4CEB9FE1A85EC53ull, 0x4CF5AD432745937Fll);
  uint64_t i = 0;
  for (; i + 64 <= n; i += 64) {
    const __m256i vi = _mm256_set1_epi64x((long long)i);
    const __m256i d0 = _mm256_loadu_si256((const __m256i*)(p + i)), d1 = _mm256_loadu_si256((const __m256i*)(p + i + 32));
    const __m256i x0 = _mm256_xor_si256(d0, _mm256_add_epi64(k0, vi)), x1 = _mm256_xor_si256(d1, _mm256_add_epi64(k1, vi));
    a0 = _mm256_add_epi64(_mm256_add_epi64(a0, _mm256_mul_epu32(x0, _mm256_srli_epi64(x0, 32))), _mm256_shuffle_epi32(d0, 0x4E));
    a1 = _mm256_add_epi64(_mm256_add_epi64(a1, _mm256_mul_epu32(x1, _mm256_srli_epi64(x1, 32))), _mm256_shuffle_epi32(d1, 0x4E));
  }
  alignas(32) uint64_t lanes[8];
  _mm256_store_si256((__m256i*)lanes, a0); _mm256_store_si256((__m256i*)(lanes + 4), a1);
  uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
  for (uint64_t v : lanes) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; }
  for (; i < n; i++) { h = (h ^ p[i]) * 0x100000001b3ull; h ^= h >> 29; }
  return h;
}
struct Prep {
  std::atomic<int> state{0};          // 0 queued, 1 running, 2 ready, 3 failed
  std::atomic<bool> cancel{false};
  TextureBuilder b{}; const FormatInfo* fi = nullptr;
  const uint8_t* src = nullptr; uint64_t srcBytes = 0, hash = 0;
  const void* pool = nullptr;         // PoolImpl* the source lives in (nullptr for packaged data outside pools)
  std::vector<uint8_t> data;          // Vulkan-side layout, same offsets as UploadStaticTexture
  uint64_t seq = 0;
  Host* host = nullptr; uint32_t hw = 0, hh = 0;   // host image size at TexPrefetch
  TexImage img;                       // created by the worker (SNAP_UPLOAD_PREFETCH_IMAGE); handed to the texture or destroyed here
  ~Prep() {
    if (img.image && host) vkDestroyImage(host->device, img.image, nullptr);
    if (img.memory && host) vkFreeMemory(host->device, img.memory, nullptr);
  }
};
bool PrepHelpOn() {   // [perf] pass 5, opt-in SNAP_UPLOAD_PREFETCH_HELP=1 (no measured gain: bursts' misses are rarely "not started")
  static const bool on = [] { const char* e = getenv("SNAP_UPLOAD_PREFETCH_HELP"); return e && *e == '1'; }();
  return on;
}
// [perf] SNAP_UPLOAD_PREFETCH_IMAGE=1: the workers also create the VkImage + memory. Off by default: in two A/B pairs (pass 5) it cut
// ensureimg per burst (41-49 to 15-29 ms) but the hub burst's upload time doubled and scratch allocation slowed (allocation contention).
bool PrepImageOn() {
  static const bool on = [] { const char* e = getenv("SNAP_UPLOAD_PREFETCH_IMAGE"); return e && *e == '1'; }();
  return on;
}
std::mutex g_prepMu;
std::condition_variable g_prepCv, g_prepDoneCv;
std::deque<std::shared_ptr<Prep>> g_prepQ;
std::unordered_map<const TextureImpl*, std::shared_ptr<Prep>> g_prepOf;
std::deque<std::pair<const TextureImpl*, uint64_t>> g_prepOrder;   // ready order for eviction (texture, seq)
uint64_t g_prepBytes = 0, g_prepSeq = 0;
std::vector<const Prep*> g_prepRunning;   // jobs reading guest memory right now (TexPrefetchPoolGone waits for them)
uint64_t PrepBudget() {
  static const uint64_t b = [] { const char* e = getenv("SNAP_UPLOAD_PREFETCH_MB"); long v = e && *e ? strtol(e, nullptr, 10) : 384; return (uint64_t)std::clamp(v, 0L, 8192L) << 20; }();   // MB, clamped (negative wrapped)
  return b;
}
// Total Vulkan-side bytes and per-(layer, level) offsets, exactly as UploadStaticTexture computes them.
uint64_t PlanOffsets(const TextureBuilder& b, const FormatInfo* fi, const SurfaceLayout& lay, uint32_t layers, uint32_t levels, std::vector<uint64_t>& offs) {
  uint64_t total = 0;
  offs.clear();
  for (uint32_t ly = 0; ly < layers; ly++)
    for (uint32_t m = 0; m < levels; m++) {
      const LevelLayout& l = lay.levels[m];
      offs.push_back(total);
      total += ((uint64_t)((l.w + fi->bw - 1) / fi->bw) * ((l.h + fi->bh - 1) / fi->bh) * l.d * fi->bytes + 15) & ~15ull;
    }
  (void)b;
  return total;
}
bool PrepRun(Prep& pr) {   // state stays 1 (running); the worker publishes the result under g_prepMu
  const TextureBuilder& b = pr.b;
  const FormatInfo* fi = pr.fi;
  const uint32_t depth = b.target == TT_3D ? (uint32_t)std::max(b.depth, 1) : 1, layers = TextureLayerCount(b), levels = Levels(b);
  SurfaceLayout lay = ComputeSurfaceLayout((uint32_t)b.width, (uint32_t)b.height, depth, layers, (uint32_t)std::max(b.levels, 1), fi->bytes, fi->bw, fi->bh);
  std::vector<uint64_t> offs;
  const uint64_t total = PlanOffsets(b, fi, lay, layers, levels, offs);
  pr.srcBytes = lay.total;
  const uint64_t h1 = FastHash(pr.src, pr.srcBytes);
  pr.data.resize((size_t)total);
  std::vector<uint8_t> lin;
  for (uint32_t ly = 0; ly < layers; ly++)
    for (uint32_t m = 0; m < levels; m++) {
      if (pr.cancel.load(std::memory_order_relaxed)) return false;
      const LevelLayout& l = lay.levels[m];
      lin.resize((size_t)l.wb * l.hb * l.d * fi->bytes);
      DeswizzleLevel(pr.src + ly * lay.layerSize + l.offset, lin.data(), l, fi->bytes);
      memcpy(pr.data.data() + offs[ly * levels + m], lin.data(), lin.size());
    }
  if (FastHash(pr.src, pr.srcBytes) != h1) return false;   // written meanwhile
  pr.hash = h1;
  return true;
}
void PrepWorker() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    std::shared_ptr<Prep> pr;
    {
      std::unique_lock<std::mutex> lk(g_prepMu);
      g_prepCv.wait(lk, [] { return !g_prepQ.empty(); });
      pr = g_prepQ.front(); g_prepQ.pop_front();
      if (pr->cancel.load() || pr->state.load() != 0) { if (pr->state.load() == 0) pr->state = 3; continue; }
      pr->state = 1;
      g_prepRunning.push_back(pr.get());
    }
    bool ok = PrepRun(*pr);
    if (ok && PrepImageOn() && pr->host && !pr->cancel.load() && !CreateTexImage(pr->host, pr->b, pr->fi, pr->hw, pr->hh, pr->img)) pr->img = TexImage{};
    {
      std::lock_guard<std::mutex> lk(g_prepMu);
      g_prepRunning.erase(std::find(g_prepRunning.begin(), g_prepRunning.end(), pr.get()));
      if (ok && !pr->cancel.load()) { g_prepBytes += pr->data.size(); pr->state = 2; }
      else { pr->data.clear(); pr->data.shrink_to_fit(); pr->state = 3; }   // failed, or forgotten / evicted while running
    }
    g_prepDoneCv.notify_all();
  }
}
// Under g_prepMu: drop ready entries, oldest first, until the staged bytes fit the budget.
void PrepEvictLocked() {
  while (g_prepBytes > PrepBudget() && !g_prepOrder.empty()) {
    auto [t, seq] = g_prepOrder.front(); g_prepOrder.pop_front();
    auto it = g_prepOf.find(t);
    if (it == g_prepOf.end() || it->second->seq != seq) continue;
    if (it->second->state.load() == 1) { g_prepOrder.push_back({t, seq}); break; }   // running: keep, try later
    if (it->second->state.load() == 2) g_prepBytes -= std::min<uint64_t>(g_prepBytes, it->second->data.size());
    it->second->cancel = true;
    g_prepOf.erase(it);
  }
}
// Removes the entry of `t`; waits while its job runs. Returns it only when its data is ready.
std::shared_ptr<Prep> PrepTake(const TextureImpl* t, int* why) {   // why: 1 none queued, 2 job not usable
  std::unique_lock<std::mutex> lk(g_prepMu);
  auto it = g_prepOf.find(t);
  if (it == g_prepOf.end()) { *why = 1; return nullptr; }
  std::shared_ptr<Prep> pr = it->second;
  g_prepOf.erase(it);
  if (pr->state.load() == 0 && !PrepHelpOn()) { pr->cancel = true; pr->state = 3; *why = 3; return nullptr; }   // not started: synchronous path
  // [perf] pass 5: not started yet, so the replay thread works the queue too. It takes up to 8 jobs from the front (textures
  // initialized just before this one, usually drawn in the same frame), then its own. The workers keep going in parallel. Before,
  // every not-started texture of a scene-change burst was deswizzled serially here.
  for (int helped = 0; pr->state.load() == 0;) {
    std::shared_ptr<Prep> j;
    if (helped < 8 && !g_prepQ.empty()) { j = g_prepQ.front(); g_prepQ.pop_front(); }
    else { j = pr; auto q = std::find(g_prepQ.begin(), g_prepQ.end(), pr); if (q != g_prepQ.end()) g_prepQ.erase(q); }
    if (j != pr && (j->cancel.load() || j->state.load() != 0)) { if (j->state.load() == 0) j->state = 3; continue; }
    j->state = 1;
    g_prepRunning.push_back(j.get());
    lk.unlock();
    bool ok = PrepRun(*j);
    if (ok && j != pr && PrepImageOn() && j->host && !j->cancel.load() && !CreateTexImage(j->host, j->b, j->fi, j->hw, j->hh, j->img)) j->img = TexImage{};
    lk.lock();
    g_prepRunning.erase(std::find(g_prepRunning.begin(), g_prepRunning.end(), j.get()));
    if (ok && (j == pr || !j->cancel.load())) { g_prepBytes += j->data.size(); j->state = 2; }
    else { j->data.clear(); j->data.shrink_to_fit(); j->state = 3; }
    g_prepDoneCv.notify_all();
    if (j != pr) helped++;
  }
  g_prepDoneCv.wait(lk, [&] { return pr->state.load() != 1; });
  if (pr->state.load() != 2) { *why = 2; return nullptr; }
  g_prepBytes -= std::min<uint64_t>(g_prepBytes, pr->data.size());
  return pr;
}
}  // namespace

void TexPrefetch(TextureImpl* t) {
  if (!PrepBudget() || !t || !t->fi) return;
  const TextureBuilder& b = t->b;
  const FormatInfo* fi = t->fi;
  if (fi->astcW || fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT || b.samples > 1 || (b.flags & TF_LINEAR) || b.width <= 0 || b.height <= 0) return;
  if (b.target != TT_2D && b.target != TT_2D_ARRAY && b.target != TT_CUBEMAP && b.target != TT_CUBEMAP_ARRAY && b.target != TT_3D) return;
  if (TextureStorageSize(b) > (48ull << 20)) return;   // render-target sized: not worth holding
  const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
  const void* poolKey = nullptr;
  if (b.pool) {
    if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) {
      poolKey = p;
      if (!src && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET)) && p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
    }
  }
  if (!src) return;
  static std::once_flag once;
  std::call_once(once, [] {
    const unsigned n = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 8));
    for (unsigned i = 0; i < n; i++) std::thread(PrepWorker).detach();
  });
  auto pr = std::make_shared<Prep>();
  pr->b = b; pr->fi = fi; pr->src = src; pr->pool = poolKey;
  pr->host = t->host; pr->hw = t->HW(); pr->hh = t->HH();
  {
    std::lock_guard<std::mutex> lk(g_prepMu);
    PrepEvictLocked();
    if (g_prepBytes >= PrepBudget()) return;
    pr->seq = ++g_prepSeq;
    auto old = g_prepOf.find(t);
    if (old != g_prepOf.end()) {
      old->second->cancel = true;
      if (old->second->state.load() == 2) g_prepBytes -= std::min<uint64_t>(g_prepBytes, old->second->data.size());
    }
    if (g_prepOrder.size() > 2 * g_prepOf.size() + 4096) {   // drop records of entries already taken or forgotten
      std::deque<std::pair<const TextureImpl*, uint64_t>> keep;
      for (auto& o : g_prepOrder) { auto it = g_prepOf.find(o.first); if (it != g_prepOf.end() && it->second->seq == o.second) keep.push_back(o); }
      g_prepOrder.swap(keep);
    }
    g_prepOf[t] = pr;
    g_prepOrder.push_back({t, pr->seq});
    g_prepQ.push_back(pr);
  }
  g_prepCv.notify_one();
}
// [perf] TextureImpl::EnsureImage: hand over the image a prefetch worker created for `t` (ready jobs only, never waits), when the
// builder and host size still match. The deswizzled data stays queued for UploadStaticTexture.
bool TexPrefetchTakeImage(TextureImpl* t) {
  if (!PrepBudget() || !PrepImageOn()) return false;
  std::lock_guard<std::mutex> lk(g_prepMu);
  auto it = g_prepOf.find(t);
  if (it == g_prepOf.end()) return false;
  Prep& pr = *it->second;
  if (pr.state.load() != 2 || !pr.img.image || pr.hw != t->HW() || pr.hh != t->HH() || pr.fi != t->fi || memcmp(&pr.b, &t->b, sizeof pr.b) != 0) return false;
  t->image = pr.img.image; t->memory = pr.img.memory;
  t->layout = VK_IMAGE_LAYOUT_UNDEFINED; t->subLayout.clear();
  t->imgLevels = pr.img.levels; t->imgLayers = pr.img.layers; t->ci_flags = pr.img.flags;
  pr.img = TexImage{};   // owned by the texture now
  snap_perf::count(snap_perf::V_UPLOAD_PREF_IMG);
  return true;
}

void TexPrefetchShutdown() {
  std::unique_lock<std::mutex> lk(g_prepMu);
  for (auto& pr : g_prepQ) pr->cancel = true;
  g_prepDoneCv.wait(lk, [] { return g_prepRunning.empty(); });
  g_prepQ.clear(); g_prepOf.clear(); g_prepOrder.clear(); g_prepBytes = 0;   // ~Prep destroys any worker image now, device still alive
}

// TextureFinalize / DestroyTexture: drop the entry and wait for a running job (it reads the guest source).
void TexPrefetchForget(const TextureImpl* t) {
  if (!PrepBudget()) return;
  std::unique_lock<std::mutex> lk(g_prepMu);
  auto it = g_prepOf.find(t);
  if (it == g_prepOf.end()) return;
  std::shared_ptr<Prep> pr = it->second;
  g_prepOf.erase(it);
  pr->cancel = true;
  g_prepDoneCv.wait(lk, [&] { return pr->state.load() != 1; });
  if (pr->state.load() == 2) g_prepBytes -= std::min<uint64_t>(g_prepBytes, pr->data.size());
}
// DestroyPool: cancel queued jobs that read this pool and wait for running ones.
void TexPrefetchPoolGone(const void* pool) {
  if (!PrepBudget() || !pool) return;
  std::unique_lock<std::mutex> lk(g_prepMu);
  for (auto& pr : g_prepQ) if (pr->pool == pool) pr->cancel = true;
  g_prepDoneCv.wait(lk, [&] {
    for (const Prep* pr : g_prepRunning) if (pr->pool == pool) return false;
    return true;
  });
}

void UploadStaticTexture(ReplayCtx& cx, TextureImpl* t) {
  if (t->uploaded) return;
  t->uploaded = true;   // one attempt only
  struct PerfUp {
    uint64_t t0; const TextureImpl* t;
    ~PerfUp() {
      if (!snap_perf::on) return;
      const uint64_t dt = snap_perf::now_ns() - t0;
      snap_perf::add(snap_perf::C_UPLOAD, dt); snap_perf::count(snap_perf::V_UPLOADS);
      if (dt > 2000000) { char det[96]; snprintf(det, sizeof det, "%dx%d fmt=%x levels=%d", t->b.width, t->b.height, t->b.format, t->b.levels); snap_perf::event("texupload", (double)dt / 1e6, det); }
    }
  } perfUp{snap_perf::on ? snap_perf::now_ns() : 0, t};
  const TextureBuilder& b = t->b;
  if (!t->EnsureImage()) return;   // adopts the prefetch worker's image when there is one (TexPrefetchTakeImage)
  int prepWhy = 0;
  std::shared_ptr<Prep> pre = PrepBudget() ? PrepTake(t, &prepWhy) : nullptr;   // [perf] prefetched data, checked below
  const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
  if (!src && b.pool && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET))) {
    if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
  }
  if (!src) return;
  if (t->fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT || b.samples > 1) return;
  const FormatInfo* fi = t->fi;
  const GuestFmt g = GuestFormat(fi);
  if ((b.flags & TF_LINEAR) && !fi->astcW && b.target != TT_3D && b.target != TT_CUBEMAP) {
    // Pitch-linear texture (the game's CPU-rendered glyph atlases): rows are `stride` bytes apart, not block-linear.
    const uint32_t wb = ((uint32_t)b.width + fi->bw - 1) / fi->bw, hb = ((uint32_t)b.height + fi->bh - 1) / fi->bh;
    const uint64_t rowBytes = (uint64_t)wb * fi->bytes;
    const uint64_t stride = b.stride >= (int)rowBytes ? (uint64_t)b.stride : rowBytes;
    VkBuffer lbuf = VK_NULL_HANDLE;
    uint8_t* lmap = NewScratch(cx, rowBytes * hb, &lbuf);
    if (!lmap) { LogError("linear texture upload: staging allocation failed"); return; }
    for (uint32_t y = 0; y < hb; y++) memcpy(lmap + (uint64_t)y * rowBytes, src + (uint64_t)y * stride, (size_t)rowBytes);
    t->srcHash = HashBytes(src, stride * hb);
    DebugDumpTexture(t, "linear", lmap, rowBytes * hb, (uint32_t)b.width, (uint32_t)b.height);
    EndRendering(cx);
    Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {(uint32_t)b.width, (uint32_t)b.height, 1};
    vkCmdCopyBufferToImage(cx.cb, lbuf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    return;
  }
  const uint32_t depth = b.target == TT_3D ? (uint32_t)std::max(b.depth, 1) : 1, layers = TextureLayerCount(b);
  const uint32_t levels = Levels(b);
  SurfaceLayout lay = ComputeSurfaceLayout((uint32_t)b.width, (uint32_t)b.height, depth, layers, (uint32_t)std::max(b.levels, 1), g.bpb, g.bw, g.bh);
  const bool astc = fi->astcW != 0 && !(fi->vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && fi->vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
  const bool toBc7 = astc && fi->bw == 4;
  // Vulkan-side bytes of one (level, layer) subresource
  auto vkSize = [&](const LevelLayout& l) -> uint64_t {
    return (uint64_t)((l.w + fi->bw - 1) / fi->bw) * ((l.h + fi->bh - 1) / fi->bh) * l.d * fi->bytes;
  };
  uint64_t total = 0;
  std::vector<uint64_t> offs;
  for (uint32_t ly = 0; ly < layers; ly++) for (uint32_t m = 0; m < levels; m++) { offs.push_back(total); total += (vkSize(lay.levels[m]) + 15) & ~15ull; }

  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, total, &buf);
  if (!map) { LogError("texture upload: staging allocation failed (%llu bytes)", (unsigned long long)total); return; }
  snap_perf::count(snap_perf::V_UPLOAD_BYTES, total);   // [perf] was never wired

  // Disk cache for the (expensive) ASTC transcode: keyed by a 128-bit hash of the guest data plus format/size.
  std::string cachePath;
  if (!astc && pre) {   // [perf] deswizzled ahead of time by TexPrefetch, valid while the guest bytes are unchanged
    if (auto& pr = pre) {
      if (pr->src == src && pr->data.size() == total && pr->srcBytes == lay.total && FastHash(src, lay.total) == pr->hash) {
        memcpy(map, pr->data.data(), (size_t)total);
        snap_perf::count(snap_perf::V_UPLOAD_PREF);
        goto cached;
      }
      snap_perf::count(snap_perf::V_PREF_MISS_HASH);
    }
  } else if (!astc && PrepBudget() && !(b.flags & TF_LINEAR) && b.samples <= 1) {
    snap_perf::count(prepWhy == 1 ? snap_perf::V_PREF_MISS_NONE : prepWhy == 3 ? snap_perf::V_PREF_MISS_LATE : snap_perf::V_PREF_MISS_FAIL);
  }
  if (astc && !cx.h->cacheDir.empty()) {
    uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x1234567ull;
    Fnv(src, (size_t)lay.total, h1, h2);
    char name[160];
    snprintf(name, sizeof name, "/astc_%x_%dx%d_%dx%d_%u_%u_%d_%016llx%016llx.bin", b.format, b.width, b.height, b.depth, b.levels,
             layers, toBc7 ? 7 : 8, (int)b.target, (unsigned long long)h1, (unsigned long long)h2);
    cachePath = cx.h->cacheDir + name;
    std::ifstream f(cachePath, std::ios::binary);
    if (f) {
      f.read((char*)map, (std::streamsize)total);
      if ((uint64_t)f.gcount() == total) goto cached;
    }
  }
  {
    std::vector<uint8_t> lin, rgba;
    for (uint32_t ly = 0; ly < layers; ly++)
      for (uint32_t m = 0; m < levels; m++) {
        const LevelLayout& l = lay.levels[m];
        lin.resize((size_t)l.wb * l.hb * l.d * g.bpb);
        DeswizzleLevel(src + ly * lay.layerSize + l.offset, lin.data(), l, g.bpb);
        uint8_t* dst = map + offs[ly * levels + m];
        if (!astc) { memcpy(dst, lin.data(), lin.size()); continue; }
        // ASTC fallback (2D slices; 3D ASTC is not decoded)
        rgba.assign((size_t)l.w * l.h * 4, 0);
        AstcDecodeLdr(lin.data(), l.w, l.h, g.bw, g.bh, fi->srgb, rgba.data());
        if (toBc7) EncodeBc7(rgba.data(), l.w, l.h, dst);
        else memcpy(dst, rgba.data(), rgba.size());
      }
  }
  if (!cachePath.empty()) {
    std::ofstream f(cachePath, std::ios::binary | std::ios::trunc);
    if (f) f.write((const char*)map, (std::streamsize)total);
  }
cached:
  if (const char* dd = getenv("NVN_TEX_DUMP")) { (void)dd; DebugDumpTexture(t, "static", map + offs[0], vkSize(lay.levels[0]), lay.levels[0].w, lay.levels[0].h); }
  EndRendering(cx);
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  std::vector<VkBufferImageCopy> regions;
  for (uint32_t ly = 0; ly < layers; ly++)
    for (uint32_t m = 0; m < levels; m++) {
      const LevelLayout& l = lay.levels[m];
      VkBufferImageCopy r{};
      r.bufferOffset = offs[ly * levels + m];
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, ly, 1};
      r.imageExtent = {l.w, l.h, l.d};
      regions.push_back(r);
    }
  vkCmdCopyBufferToImage(cx.cb, buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
}

void RefreshDynamicTexture(ReplayCtx& cx, TextureImpl* t) {
  if (t->hashSeq == g_replaySeq) return;
  uint64_t n = 0;
  const uint8_t* src = DynamicSource(t, &n);
  if (!src || n > (32u << 20)) return;
  t->hashSeq = g_replaySeq;
  { snap_perf::Scope perfHash(snap_perf::C_HASH); snap_perf::count(snap_perf::V_HASH_CALLS); snap_perf::count(snap_perf::V_HASH_BYTES, n);
    if (HashBytes(src, n) == t->srcHash) return; }
  t->uploaded = false;
  UploadStaticTexture(cx, t);
}

}  // namespace nvn
