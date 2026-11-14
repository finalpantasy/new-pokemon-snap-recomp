// [perf pass 7] Block sub-allocator for texture images and scratch buffers (2026-11-13, perf worker).
//
// The hub-entry frame creates ~240 textures and ~240 staging buffers in one replay; each had its own vkAllocateMemory
// (~0.15-0.2 ms each on the NVIDIA driver: 53 ms ensureimg + 34 ms scratch in that frame). Small allocations now come from
// shared VkDeviceMemory blocks: first fit with alignment, coalescing free list, one spare empty block kept per pool.
//   SNAP_TEXMEM_BLOCK_MB=<mb>     device-local image blocks (default 64; 0 = one vkAllocateMemory per image, the old path)
//   SNAP_SCRATCH_BLOCK_MB=<mb>    host-visible mapped scratch blocks (default 64; 0 = one vkAllocateMemory per buffer)
//   SNAP_SUBMEM_SPARE=<n>         empty blocks per pool a helper thread keeps ready (default 2; 0 = none, allocate on demand)
//   SNAP_SUBMEM_RESERVE=<n>       image pools are kept at n blocks or more from boot (default 8; 0 = spare blocks only)
// An allocation larger than a quarter block, or one whose driver requires a dedicated allocation, still gets its own memory.
// Only optimal-tiling images share image blocks and only buffers share scratch blocks, so bufferImageGranularity never applies.
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "nvn_internal.h"

namespace nvn {
namespace {

struct Block {
  VkDeviceMemory mem = VK_NULL_HANDLE;
  uint64_t size = 0;
  uint32_t type = 0;
  int kind = 0;
  uint8_t* map = nullptr;
  std::map<uint64_t, uint64_t> free;   // offset -> size
  uint32_t live = 0;
};
struct Owned { Block* b; uint64_t off, size; };

std::mutex g_mu;
std::vector<Block*> g_blocks;
std::unordered_map<uint64_t, Owned> g_owned[2];   // per kind, keyed by the VkImage / VkBuffer handle

uint64_t EnvMB(const char* name, long def) {
  const char* e = getenv(name);
  long v = e && *e ? strtol(e, nullptr, 10) : def;
  return (uint64_t)std::clamp(v, 0L, 1024L) << 20;
}

bool TakeFrom(Block* b, uint64_t size, uint64_t align, uint64_t* off) {
  if (align == 0) align = 1;
  for (auto it = b->free.begin(); it != b->free.end(); ++it) {
    const uint64_t fo = it->first, fs = it->second;
    const uint64_t o = (fo + align - 1) / align * align;
    if (o + size > fo + fs) continue;
    b->free.erase(it);
    if (o > fo) b->free[fo] = o - fo;
    if (o + size < fo + fs) b->free[o + size] = fo + fs - (o + size);
    *off = o;
    return true;
  }
  return false;
}

void GiveBack(Block* b, uint64_t off, uint64_t size) {
  auto it = b->free.emplace(off, size).first;
  auto nx = std::next(it);
  if (nx != b->free.end() && it->first + it->second == nx->first) { it->second += nx->second; b->free.erase(nx); }
  if (it != b->free.begin()) {
    auto pv = std::prev(it);
    if (pv->first + pv->second == it->first) { pv->second += it->second; b->free.erase(it); }
  }
}

}  // namespace

uint64_t SubMemBlockBytes(int kind) {
  static const uint64_t img = EnvMB("SNAP_TEXMEM_BLOCK_MB", 64), scr = EnvMB("SNAP_SCRATCH_BLOCK_MB", 64);
  return kind == SUBMEM_IMAGE ? img : scr;
}

namespace {
Block* NewBlock(Host* h, int kind, uint32_t type) {   // no lock held: vkAllocateMemory of a whole block is the slow part
  const uint64_t blk = SubMemBlockBytes(kind);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = blk; mai.memoryTypeIndex = type;
  VkDeviceMemory m = VK_NULL_HANDLE;
  if (vkAllocateMemory(h->device, &mai, nullptr, &m) != VK_SUCCESS) return nullptr;
  Block* b = new Block;
  b->mem = m; b->size = blk; b->type = type; b->kind = kind;
  if (kind == SUBMEM_SCRATCH) {
    void* p = nullptr;
    if (vkMapMemory(h->device, m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) { vkFreeMemory(h->device, m, nullptr); delete b; return nullptr; }
    b->map = (uint8_t*)p;
  }
  b->free[0] = blk;
  return b;
}
// Spare blocks ahead: once a pool is in use, a background thread keeps SNAP_SUBMEM_SPARE empty blocks of it ready, so a burst (the hub entry
// creates ~240 images) takes blocks that already exist instead of allocating 64 MB on the replay thread. SNAP_SUBMEM_SPARE=0 = off.
std::condition_variable g_cv;
std::vector<std::pair<int, uint32_t>> g_want;   // (kind, type) pools that need a spare
bool g_busy = false, g_stop = false;
Host* g_host = nullptr;
int SpareN() {
  static const int n = [] { const char* e = getenv("SNAP_SUBMEM_SPARE"); return e && *e ? std::clamp(atoi(e), 0, 8) : 2; }();
  return n;
}
int EmptyLocked(int kind, uint32_t type) {
  int n = 0;
  for (Block* b : g_blocks) if (b->kind == kind && b->type == type && b->live == 0) n++;
  return n;
}
// [perf pass 9] Boot reservation: from the first use of an image pool (boot, title screen) the helper thread keeps the pool at
// SNAP_SUBMEM_RESERVE blocks or more (default 8 = 512 MB of VRAM): empty-block target = max(spare, reserve - blocks in use). The
// first hub entry (~240 images, 6+ blocks) then finds its blocks ready instead of allocating on the replay thread; once more than
// `reserve` blocks are in use the target is the plain SNAP_SUBMEM_SPARE. Scratch pools have no reserve. 0 = no reserve.
int ReserveN(int kind) {
  static const int n = [] { const char* e = getenv("SNAP_SUBMEM_RESERVE"); return e && *e ? std::clamp(atoi(e), 0, 32) : 8; }();
  return kind == SUBMEM_IMAGE ? n : 0;
}
int TargetLocked(int kind, uint32_t type) {
  int used = 0;
  for (Block* b : g_blocks) if (b->kind == kind && b->type == type && b->live) used++;
  return std::max(SpareN(), ReserveN(kind) - used);
}
void SpareThread() {
  std::unique_lock<std::mutex> lk(g_mu);
  for (;;) {
    g_cv.wait(lk, [] { return g_stop || !g_want.empty(); });
    if (g_stop) { g_want.clear(); return; }
    auto [kind, type] = g_want.back(); g_want.pop_back();
    if (EmptyLocked(kind, type) >= TargetLocked(kind, type)) continue;
    g_busy = true;
    lk.unlock();
    Block* b = NewBlock(g_host, kind, type);
    lk.lock();
    g_busy = false;
    if (b) g_blocks.push_back(b);
    if (b && EmptyLocked(kind, type) < TargetLocked(kind, type)) g_want.push_back({kind, type});   // until the target exists
    g_cv.notify_all();
  }
}
void WantSpareLocked(Host* h, int kind, uint32_t type) {
  if (!TargetLocked(kind, type) || g_stop || EmptyLocked(kind, type) >= TargetLocked(kind, type)) return;
  if (!g_host) { g_host = h; std::thread(SpareThread).detach(); }
  for (auto& w : g_want) if (w.first == kind && w.second == type) return;
  g_want.push_back({kind, type});
  g_cv.notify_all();
}
}  // namespace

bool SubMemAlloc(Host* h, int kind, uint64_t handle, const VkMemoryRequirements& mr, uint32_t type, VkDeviceMemory* mem,
                 uint64_t* off, uint8_t** map) {
  const uint64_t blk = SubMemBlockBytes(kind);
  if (!blk || mr.size > blk / 4 || type == UINT32_MAX) return false;
  std::unique_lock<std::mutex> lk(g_mu);
  Block* use = nullptr;
  uint64_t o = 0;
  for (Block* b : g_blocks)   // fullest first would pack tighter; first fit in creation order is close enough and cheap
    if (b->kind == kind && b->type == type && b->live && TakeFrom(b, mr.size, mr.alignment, &o)) { use = b; break; }
  if (!use)
    for (Block* b : g_blocks)   // an empty (spare) block
      if (b->kind == kind && b->type == type && TakeFrom(b, mr.size, mr.alignment, &o)) { use = b; break; }
  if (!use) {
    lk.unlock();
    Block* b = NewBlock(h, kind, type);
    lk.lock();
    if (!b) return false;   // caller falls back to its own allocation
    g_blocks.push_back(b);
    if (!TakeFrom(b, mr.size, mr.alignment, &o)) return false;   // cannot happen (size <= blk / 4)
    use = b;
  }
  use->live++;
  g_owned[kind][handle] = Owned{use, o, mr.size};
  *mem = use->mem; *off = o;
  if (map) *map = use->map ? use->map + o : nullptr;
  WantSpareLocked(h, kind, type);
  return true;
}

bool SubMemFree(Host* h, int kind, uint64_t handle) {
  Block* drop = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_owned[kind].find(handle);
    if (it == g_owned[kind].end()) return false;
    Owned w = it->second;
    g_owned[kind].erase(it);
    GiveBack(w.b, w.off, w.size);
    if (--w.b->live == 0 && EmptyLocked(w.b->kind, w.b->type) > std::max(TargetLocked(w.b->kind, w.b->type), 1)) {   // keep the target
      g_blocks.erase(std::find(g_blocks.begin(), g_blocks.end(), w.b));
      drop = w.b;
    }
  }
  if (drop) {   // [perf pass 9] unlisted, no allocation left in it: free a 64 MB block without holding g_mu (the replay may be allocating)
    if (drop->map) vkUnmapMemory(h->device, drop->mem);
    vkFreeMemory(h->device, drop->mem, nullptr);
    delete drop;
  }
  return true;
}

void SubMemDrain(Host* h) {   // Host::Shutdown, before vkDestroyDevice (images still alive are never used again)
  std::unique_lock<std::mutex> lk(g_mu);
  g_stop = true;
  g_cv.notify_all();
  g_cv.wait(lk, [] { return !g_busy; });   // a spare allocation in flight finishes first
  for (Block* b : g_blocks) {
    if (b->map) vkUnmapMemory(h->device, b->mem);
    vkFreeMemory(h->device, b->mem, nullptr);
    delete b;
  }
  g_blocks.clear();
  g_owned[0].clear(); g_owned[1].clear();
}

}  // namespace nvn
