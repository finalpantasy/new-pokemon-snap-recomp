// SPDX-License-Identifier: MIT
// Object lifetime against the GPU timeline (analysis/core/review_nvn.md findings 1-4 and 10, worker nvnfix, 2026-11-13).
//  - Command list handles: CommandBufferEndRecording returns an id, not a heap pointer. A submit resolves ids through a table, so a handle
//    whose list was already freed is skipped and counted instead of replaying freed memory, and the same handle twice in one submit (or
//    again while in flight) takes a second reference instead of a double delete.
//  - Graveyard: finalized textures, samplers, memory pools, texture / sampler pools and programs are destroyed only after the queue
//    timeline passes the last submit made before they were retired (the same stamp-at-present rule as OwnPoolReap). Before they are
//    freed, texture / sampler pool slots that still point at them are nulled and the queue's persistent draw state forgets them.
//  - SNAP_NVN_STATS=1: one "[nvnstats]" line every 300 presents with the counters below (measurement only, no behavior change).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "nvn_internal.h"

namespace nvn {

std::atomic<uint64_t> g_lifeStat[LS_COUNT];

static const char* const kLifeStatName[LS_COUNT] = {
  "resubmit", "staleHandle", "listsMade", "retired", "destroyed", "slotsCleared", "persistScrub", "rbWaitPoolGone",
  "pipeSkip", "bcRecreate", "aaResize", "cubeCopy", "flushGpuOverlap", "clearScissored", "xAlphaViews", "backingRace",
};

// ---- command list handles ------------------------------------------------------------------------------------------------------
namespace {
std::mutex g_listMu;
std::unordered_map<uint64_t, CmdList*> g_lists;
uint64_t g_nextList = 0x10000;
}  // namespace

CommandHandle ListRegister(CmdList* l) {
  std::lock_guard<std::mutex> lk(g_listMu);
  l->id = g_nextList++;
  g_lists[l->id] = l;
  LifeCount(LS_LISTS_MADE);
  return l->id;
}

void ListAcquire(int n, const CommandHandle* handles, CmdList** out) {
  std::lock_guard<std::mutex> lk(g_listMu);
  for (int i = 0; i < n; i++) {
    auto it = g_lists.find(handles[i]);
    if (it == g_lists.end()) {
      out[i] = nullptr;
      static int logged = 0;
      if (logged++ < 8) LogError("QueueSubmitCommands: handle 0x%llx is not a live command list (already retired or never recorded): skipped",
                                 (unsigned long long)handles[i]);
      LifeCount(LS_STALE_HANDLE);
      continue;
    }
    CmdList* l = it->second;
    if (l->submitted) { l->refs++; LifeCount(LS_RESUBMIT); }   // same handle again (in this call or still in flight): one more reference
    else l->submitted = true;                                  // the recording's reference moves to its first submit
    out[i] = l;
  }
}

// [cpuprof 2026-11-13] Recycled command vectors: EndRecording moved the buffer's vector into the list, so every recording grew a fresh
// vector from empty (vector<Cmd>::_Emplace_reallocate + malloc were ~25% of the NVN recording thread mt_asyc_dispatcher_owner_worker).
// A released list's vector is cleared (capacity kept) and handed to the next EndRecording. SNAP_NVN_CMD_RECYCLE=0 restores the old path.
static const bool kCmdRecycle = [] { const char* e = getenv("SNAP_NVN_CMD_RECYCLE"); return !(e && *e == '0'); }();
static std::mutex g_cmdVecMu;
static std::vector<std::vector<Cmd>> g_cmdVecs;
void CmdVecTake(std::vector<Cmd>& out, size_t want) {
  if (kCmdRecycle) {
    std::lock_guard<std::mutex> lk(g_cmdVecMu);
    if (!g_cmdVecs.empty()) { out.swap(g_cmdVecs.back()); g_cmdVecs.pop_back(); }
  }
  if (kCmdRecycle && out.capacity() < want) out.reserve(want);
}
void CmdVecGive(std::vector<Cmd>& v) {
  if (!kCmdRecycle || v.capacity() == 0 || v.capacity() > (1u << 16)) return;
  v.clear();   // destroys the commands (their blobs) outside the pool lock; keeps the capacity
  std::lock_guard<std::mutex> lk(g_cmdVecMu);
  if (g_cmdVecs.size() < 64) { g_cmdVecs.emplace_back(); g_cmdVecs.back().swap(v); }
}

void ListRelease(CmdList* l) {
  if (!l) return;
  {
    std::lock_guard<std::mutex> lk(g_listMu);
    if (--l->refs > 0) return;
    g_lists.erase(l->id);
  }
  CmdVecGive(l->cmds);
  delete l;
}

static size_t ListsLive() { std::lock_guard<std::mutex> lk(g_listMu); return g_lists.size(); }

// ---- live descriptor pools (scanned when a texture or sampler they may still name is freed) ------------------------------------
namespace {
std::mutex g_regMu;
std::vector<TexPoolImpl*> g_texPools;
std::vector<SampPoolImpl*> g_sampPools;
template <class T> void RegSet(std::vector<T*>& v, T* p, bool live) {
  std::lock_guard<std::mutex> lk(g_regMu);
  auto it = std::find(v.begin(), v.end(), p);
  if (live && it == v.end()) v.push_back(p);
  if (!live && it != v.end()) v.erase(it);
}
}  // namespace
void TexPoolLive(TexPoolImpl* p, bool live) { RegSet(g_texPools, p, live); }
void SampPoolLive(SampPoolImpl* p, bool live) { RegSet(g_sampPools, p, live); }

// ---- graveyard ------------------------------------------------------------------------------------------------------------------
namespace {
struct Grave { GraveKind k; void* obj; uint64_t value; };   // value 0 = not stamped yet
std::mutex g_graveMu2;
std::vector<Grave> g_graves;

void DestroyNow(const Grave& g) {
  switch (g.k) {
    case GK_TEXTURE: DestroyTexture(static_cast<TextureImpl*>(g.obj)); break;
    case GK_SAMPLER: {
      auto* s = static_cast<SamplerImpl*>(g.obj);
      if (s->host && s->host->device && s->sampler) vkDestroySampler(s->host->device, s->sampler, nullptr);
      delete s;
      break;
    }
    case GK_POOL: DestroyPool(static_cast<PoolImpl*>(g.obj)); break;
    case GK_TEXPOOL: delete static_cast<TexPoolImpl*>(g.obj); break;
    case GK_SAMPPOOL: delete static_cast<SampPoolImpl*>(g.obj); break;
    case GK_PROGRAM: { auto* p = static_cast<ProgramImpl*>(g.obj); ProgramDestroy(p); delete p; break; }
    case GK_FN: { auto* f = static_cast<std::function<void()>*>(g.obj); (*f)(); delete f; break; }
  }
  LifeCount(LS_DESTROYED);
}

// Null every live pool slot that names a texture / sampler about to be freed. Runs under queueMutex (no replay reads the tables
// meanwhile); loader threads may register concurrently, so texture slots use the TexSlot sequence lock and sampler slots a CAS.
void ScrubSlots(const std::unordered_set<const void*>& deadTex, const std::unordered_set<const void*>& deadSamp) {
  std::lock_guard<std::mutex> lk(g_regMu);
  uint64_t n = 0;
  if (!deadTex.empty())
    for (TexPoolImpl* tp : g_texPools)
      for (auto& ch : tp->slots.chunks) {
        TexSlot* c = ch.load(std::memory_order_acquire);
        if (!c) continue;
        for (unsigned i = 0; i < SlotTable<TexSlot>::kChunk; i++) {
          TexSlot& s = c[i];
          if (!s.tex || !deadTex.count(s.tex)) continue;
          uint32_t q = s.seq.load(std::memory_order_relaxed);
          for (;;) {
            if (!(q & 1) && s.seq.compare_exchange_weak(q, q + 1, std::memory_order_acquire, std::memory_order_relaxed)) break;
            YieldProcessor();
            q = s.seq.load(std::memory_order_relaxed);
          }
          std::atomic_thread_fence(std::memory_order_release);
          if (s.tex && deadTex.count(s.tex)) { s.tex = nullptr; s.hasView = false; n++; }   // re-check: a registration may have replaced it
          s.seq.store(q + 2, std::memory_order_release);
        }
      }
  if (!deadSamp.empty())
    for (SampPoolImpl* sp : g_sampPools)
      for (auto& ch : sp->slots.chunks) {
        SamplerImpl** c = ch.load(std::memory_order_acquire);
        if (!c) continue;
        for (unsigned i = 0; i < SlotTable<SamplerImpl*>::kChunk; i++) {
          SamplerImpl* cur = __atomic_load_n(&c[i], __ATOMIC_ACQUIRE);
          if (!cur || !deadSamp.count(cur)) continue;
          if (__atomic_compare_exchange_n(&c[i], &cur, (SamplerImpl*)nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) n++;
        }
      }
  if (n) LifeCount(LS_SLOTS_CLEARED, n);
}
}  // namespace

void Retire(GraveKind k, void* obj) {
  if (!obj) return;
  std::lock_guard<std::mutex> lk(g_graveMu2);
  g_graves.push_back({k, obj, 0});
  LifeCount(LS_RETIRED);
}

void RetireFn(std::function<void()> fn) { if (fn) Retire(GK_FN, new std::function<void()>(std::move(fn))); }

void GraveReap(QueueImpl* q) {
  if (!q || !q->host) return;
  std::vector<Grave> dead;
  {
    std::lock_guard<std::mutex> lk(g_graveMu2);
    if (g_graves.empty()) return;
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(q->host->device, q->timeline, &done);
    size_t keep = 0;
    for (auto& g : g_graves) {
      if (!g.value) { g.value = q->value ? q->value : 1; g_graves[keep++] = g; }   // every submit that can use it is <= this value
      else if (g.value <= done) dead.push_back(g);
      else g_graves[keep++] = g;
    }
    g_graves.resize(keep);
  }
  if (dead.empty()) return;
  std::unordered_set<const void*> all, deadTex, deadSamp;
  for (auto& g : dead) {
    all.insert(g.obj);
    if (g.k == GK_TEXTURE) deadTex.insert(g.obj);
    if (g.k == GK_SAMPLER) deadSamp.insert(g.obj);
  }
  ScrubSlots(deadTex, deadSamp);
  PersistScrub(q, all);
  for (auto& g : dead) DestroyNow(g);
}

void GraveDrain(Host* h) {   // Host::Shutdown after vkDeviceWaitIdle: nothing is in flight any more
  (void)h;
  std::vector<Grave> all;
  { std::lock_guard<std::mutex> lk(g_graveMu2); all.swap(g_graves); }
  for (auto& g : all) DestroyNow(g);
}

// ---- memory pool finalize --------------------------------------------------------------------------------------------------------
// The guest frees the pool's storage right after MemoryPoolFinalize. Readbacks still in flight that land in that storage (shader
// write-backs, CopyTextureToBuffer, counters) are completed now, while the storage is still valid; the Vulkan mirror waits for the timeline.
void PoolRetire(PoolImpl* p) {
  if (!p) return;
  TexPrefetchPoolGone(p);   // [perf] no prefetch job may read the storage after this
  Host* h = p->host;
  if (h) {
    h->UnregisterPool(p);   // no later replay resolves an address into it
    bool wait = p->wbPending.load(std::memory_order_relaxed) > 0;
    if (!wait && p->storage) {
      const uint8_t* b = static_cast<const uint8_t*>(p->storage);
      std::lock_guard<std::mutex> lk(h->rbMutex);
      for (auto& rb : h->readbacks)
        if (rb.pool == p || (rb.dst && rb.dst >= b && rb.dst < b + p->size)) { wait = true; break; }
    }
    if (wait) { LifeCount(LS_RB_WAIT_POOL_GONE); FlushReadbacks(h, true); }
  }
  Retire(GK_POOL, p);
}

// ---- stats -----------------------------------------------------------------------------------------------------------------------
void LifeStatsTick() {
  static const bool on = [] { const char* e = getenv("SNAP_NVN_STATS"); return e && atoi(e) > 0; }();
  if (!on || g_presentCount % 300 != 0) return;
  char line[1024];
  int o = snprintf(line, sizeof line, "[nvnstats] p%d listsLive=%zu graves=%zu", g_presentCount, ListsLive(),
                   [] { std::lock_guard<std::mutex> lk(g_graveMu2); return g_graves.size(); }());
  for (int i = 0; i < LS_COUNT && o > 0 && o < (int)sizeof line - 40; i++)
    o += snprintf(line + o, sizeof line - o, " %s=%llu", kLifeStatName[i], (unsigned long long)g_lifeStat[i].load(std::memory_order_relaxed));
  fprintf(stderr, "%s\n", line);
  fflush(stderr);
}

}  // namespace nvn
