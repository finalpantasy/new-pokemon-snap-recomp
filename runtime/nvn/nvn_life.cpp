// SPDX-License-Identifier: MIT
// Object lifetime against the GPU timeline (analysis/core/review_nvn.md findings 1-4 and 10, worker nvnfix, 2026-11-13).
//  - Command list handles: CommandBufferEndRecording returns an id, not a heap pointer. A submit resolves ids through a table, so a handle
//    whose list was already freed is skipped and counted instead of replaying freed memory, and the same handle twice in one submit (or
//    again while in flight) takes a second reference instead of a double delete.
//  - Graveyard: finalized textures, samplers, memory pools, texture / sampler pools and programs are destroyed only after the queue
//    timeline passes the last submit made before they were retired, on every queue (stamped at the next present). Before they are
//    freed, texture / sampler pool slots that still point at them are nulled and the queue's persistent draw state forgets them.
//  - SNAP_NVN_STATS=1: one "[nvnstats]" line every 300 presents with the counters below (measurement only, no behavior change).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "nvn_internal.h"

namespace nvn {

std::atomic<uint64_t> g_lifeStat[LS_COUNT];

static const char* const kLifeStatName[LS_COUNT] = {
  "resubmit", "staleHandle", "listsMade", "retired", "destroyed", "slotsCleared", "persistScrub", "rbWaitPoolGone",
  "pipeSkip", "bcRecreate", "aaResize", "cubeCopy", "flushGpuOverlap", "clearScissored", "xAlphaViews", "backingRace",
  "listCtlRetire", "listNoCtl",
};

// ---- command list handles ------------------------------------------------------------------------------------------------------
// [audit F5, 2026-11-13] A recorded handle stays submittable until the control memory it was recorded in is reused (NVN; TotK
// runtime/nvn_api_cmd.c nvnCommandBufferEndRecording / release_copies). The recording keeps its own reference (CmdList::owned) apart
// from the submissions' references: adding its control memory again (ListRetireCtl) drops the handle, and the list is freed once no
// submission still has it in flight. It used to hand its only reference to the first submission, so a second submit of the same
// handle after that one completed found nothing (staleHandle).
void CmdVecGive(std::vector<Cmd>& v);   // below
namespace {
std::mutex g_listMu;
std::unordered_map<uint64_t, CmdList*> g_lists;
uint64_t g_nextList = 0x10000;
std::multimap<uintptr_t, CmdList*> g_ctlIndex;                  // owned recordings by control memory address
std::unordered_multimap<const void*, CmdList*> g_noCtl;         // owned recordings made without control memory, by recorder

// Under g_listMu: the recording's reference goes; true when nothing else holds the list (the caller frees it outside the lock).
bool DropOwnerLocked(CmdList* l) {
  if (!l->owned) return false;
  l->owned = false;
  g_lists.erase(l->id);   // the handle is stale from now on
  return --l->refs <= 0;
}
void FreeList(CmdList* l) { CmdVecGive(l->cmds); delete l; }
}  // namespace

CommandHandle ListRegister(CmdList* l, const void* recorder, uintptr_t ctl) {
  std::vector<CmdList*> dead;
  CommandHandle id;
  {
    std::lock_guard<std::mutex> lk(g_listMu);
    l->id = g_nextList++;
    l->recorder = recorder;
    l->ctl = ctl;
    g_lists[l->id] = l;
    if (ctl) {
      // [2026-11-14] a recording at an address that already owns one replaces it: the control memory wrapped (ctlUsed >= ctlSize puts every later
      // recording at ctlBase), so the older handle's memory is reused and it ends here. A submission in flight keeps its own reference.
      auto r = g_ctlIndex.equal_range(ctl);
      uint64_t n = 0;
      for (auto it = r.first; it != r.second; ++it, n++) if (DropOwnerLocked(it->second)) dead.push_back(it->second);
      g_ctlIndex.erase(r.first, r.second);
      if (n) LifeCount(LS_LIST_CTL_RETIRE, n);
      g_ctlIndex.emplace(ctl, l);
    }
    else { g_noCtl.emplace(recorder, l); LifeCount(LS_LIST_NO_CTL); }
    LifeCount(LS_LISTS_MADE);
    id = l->id;
  }
  for (CmdList* d : dead) FreeList(d);
  return id;
}

void ListRetireCtl(const void* recorder, uintptr_t lo, uintptr_t hi) {
  std::vector<CmdList*> dead;
  {
    std::lock_guard<std::mutex> lk(g_listMu);
    uint64_t n = 0;
    if (hi > lo)
      for (auto it = g_ctlIndex.lower_bound(lo); it != g_ctlIndex.end() && it->first < hi; it = g_ctlIndex.erase(it), n++)
        if (DropOwnerLocked(it->second)) dead.push_back(it->second);
    auto r = g_noCtl.equal_range(recorder);   // the recorder's recordings that had no control memory end with its next block
    for (auto it = r.first; it != r.second; ++it, n++) if (DropOwnerLocked(it->second)) dead.push_back(it->second);
    g_noCtl.erase(r.first, r.second);
    if (n) LifeCount(LS_LIST_CTL_RETIRE, n);
  }
  for (CmdList* l : dead) FreeList(l);
}

void ListRecorderGone(const void* recorder, uintptr_t ctlLo, uintptr_t ctlHi) {
  std::vector<CmdList*> dead;
  {
    std::lock_guard<std::mutex> lk(g_listMu);
    // [2026-11-14] the recorder's control memory range too: its recordings used to stay indexed (and leak) after the buffer was finalized
    if (ctlHi > ctlLo)
      for (auto it = g_ctlIndex.lower_bound(ctlLo); it != g_ctlIndex.end() && it->first < ctlHi; ) {
        if (it->second->recorder == recorder) { if (DropOwnerLocked(it->second)) dead.push_back(it->second); it = g_ctlIndex.erase(it); }
        else ++it;
      }
    auto r = g_noCtl.equal_range(recorder);
    for (auto it = r.first; it != r.second; ++it) if (DropOwnerLocked(it->second)) dead.push_back(it->second);
    g_noCtl.erase(r.first, r.second);
  }
  for (CmdList* l : dead) FreeList(l);
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
    if (l->submitted) LifeCount(LS_RESUBMIT);   // same handle again (in this call, still in flight, or after completion)
    l->submitted = true;
    l->refs++;                                   // [audit F5] each submission holds its own reference; the recording keeps its own
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

void ListRelease(CmdList* l) {   // a submission's reference (the recording's own is dropped by DropOwnerLocked)
  if (!l) return;
  {
    std::lock_guard<std::mutex> lk(g_listMu);
    if (--l->refs > 0) return;
    if (l->owned) {   // not reachable while the recording holds its reference; kept so a miscount cannot leave a dangling handle
      g_lists.erase(l->id);
      if (l->ctl) { auto r = g_ctlIndex.equal_range(l->ctl); for (auto it = r.first; it != r.second; ++it) if (it->second == l) { g_ctlIndex.erase(it); break; } }
      else { auto r = g_noCtl.equal_range(l->recorder); for (auto it = r.first; it != r.second; ++it) if (it->second == l) { g_noCtl.erase(it); break; } }
    }
  }
  FreeList(l);
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
// [audit F4, 2026-11-13] The game creates two queues (main+0x90e524, main+0x90e590), each with its own timeline semaphore, so one
// queue's counter says nothing about the other's. A grave is stamped with (queue, last submitted value) for EVERY live queue and is
// destroyed only when each of those queues has completed its value; a queue destroyed since (DestroyQueue finished it first) counts
// as passed. Queues are named by a serial, never by a pointer or semaphore that could be reused. Persistent draw state is scrubbed in
// every live queue, not only the presenting one.
namespace {
struct QueueRec { QueueImpl* q; uint64_t serial; };
std::mutex g_qMu;                 // guards g_queues; held across GraveReap so a listed QueueImpl stays valid (DestroyQueue unlists it)
std::vector<QueueRec> g_queues;
uint64_t g_qSerial = 0;
using Marks = std::vector<std::pair<uint64_t, uint64_t>>;   // (queue serial, timeline value)
}  // namespace

void QueueLive(QueueImpl* q, bool live) {
  if (!q) return;
  std::lock_guard<std::mutex> lk(g_qMu);
  auto it = std::find_if(g_queues.begin(), g_queues.end(), [&](const QueueRec& r) { return r.q == q; });
  if (live && it == g_queues.end()) g_queues.push_back({q, ++g_qSerial});
  if (!live && it != g_queues.end()) g_queues.erase(it);
}

namespace {
struct Grave { GraveKind k; void* obj; std::shared_ptr<const Marks> need; };   // need null = not stamped yet
std::mutex g_graveMu2;
std::vector<Grave> g_graves;

bool Passed(const Marks& need, const Marks& done) {
  for (auto& n : need) {
    auto it = std::find_if(done.begin(), done.end(), [&](const std::pair<uint64_t, uint64_t>& d) { return d.first == n.first; });
    if (it != done.end() && it->second < n.second) return false;   // missing = that queue was finished and destroyed
  }
  return true;
}

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
  g_graves.push_back({k, obj, nullptr});
  LifeCount(LS_RETIRED);
}

void RetireFn(std::function<void()> fn) { if (fn) Retire(GK_FN, new std::function<void()>(std::move(fn))); }

void GraveReap(QueueImpl* q) {   // under Host::queueMutex: no queue submits or replays meanwhile, so every queue's value is stable
  if (!q || !q->host) return;
  QueueLive(q, true);   // a queue that presents is live (InitQueue already listed it)
  std::lock_guard<std::mutex> qlk(g_qMu);
  std::vector<Grave> dead;
  {
    std::lock_guard<std::mutex> lk(g_graveMu2);
    if (g_graves.empty()) return;
    auto need = std::make_shared<Marks>();   // what each queue has submitted: every submit that can use a grave stamped now is <= this
    Marks done;                              // what each queue has completed
    for (auto& r : g_queues) {
      if (r.q->value) need->push_back({r.serial, r.q->value});
      uint64_t d = 0;
      if (r.q->timeline && r.q->host) vkGetSemaphoreCounterValue(r.q->host->device, r.q->timeline, &d);
      done.push_back({r.serial, d});
    }
    size_t keep = 0;
    for (auto& g : g_graves) {
      if (!g.need) { g.need = need; g_graves[keep++] = g; }
      else if (Passed(*g.need, done)) dead.push_back(g);
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
  for (auto& r : g_queues) PersistScrub(r.q, all);   // [audit F4] every queue's persistent draw state, not only the presenting one
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
