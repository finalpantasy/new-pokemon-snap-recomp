// SPDX-License-Identifier: MIT
// CPU-only checks for the NVN lifetime fixes of the 2026-11-13 audit (analysis/core/codex-audit-20261113, review_nvn.md "Audit F3-F6").
// Links runtime/nvn/nvn_life.cpp unchanged; the Vulkan and renderer functions it calls are stubbed below. No device is created.
//   F3  CopyStageMeta copies a stage without touching fragMods, also while another thread inserts variants into it
//   F4  a grave waits for every live queue's own timeline; a destroyed queue counts as passed; every queue's draw state is scrubbed
//   F5  a recorded handle can be submitted again after its submission completed; it ends when its control memory is added again,
//       and the list is freed only when no submission is in flight
//   F6  PoolBacked takes the acquire fast path when the buffer is published and calls EnsureBacking otherwise
// Build: tests\nvn\build_life.bat. Exit 0 = every check passed.
#include "nvn_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <thread>

namespace nvn {
void LogError(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vfprintf(stderr, fmt, a);
  va_end(a);
  std::fputc('\n', stderr);
}
int g_presentCount = 0;
void DestroyTexture(TextureImpl*) { std::abort(); }
void DestroyPool(PoolImpl*) { std::abort(); }
void ProgramDestroy(ProgramImpl*) { std::abort(); }
void TexPrefetchPoolGone(const void*) { std::abort(); }
void Host::UnregisterPool(PoolImpl*) { std::abort(); }
void FlushReadbacks(Host*, bool) { std::abort(); }
std::set<const QueueImpl*> g_scrubbed;
void PersistScrub(QueueImpl* q, const std::unordered_set<const void*>&) { g_scrubbed.insert(q); }
int g_ensureCalls = 0;
bool EnsureBacking(PoolImpl* p) { g_ensureCalls++; __atomic_store_n(&p->buffer, (VkBuffer)(uintptr_t)0x5000, __ATOMIC_RELEASE); return true; }
}  // namespace nvn

// completed timeline value per fake semaphore handle (1..7)
static uint64_t g_done[8];
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice, VkSemaphore sem, uint64_t* value) {
  *value = g_done[reinterpret_cast<uintptr_t>(sem) & 7];
  return VK_SUCCESS;
}
#undef vkDestroySampler
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks*) { std::abort(); }

// Watch for the CmdList being freed (ListRelease / ListRetireCtl delete it).
static void* g_watch = nullptr;
static bool g_watchFreed = false;
void operator delete(void* p) noexcept { if (p && p == g_watch) g_watchFreed = true; std::free(p); }
void operator delete(void* p, size_t) noexcept { if (p && p == g_watch) g_watchFreed = true; std::free(p); }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

using namespace nvn;

static CmdList* Acquire1(CommandHandle h) { CmdList* l = nullptr; ListAcquire(1, &h, &l); return l; }

static void TestF5() {
  const int rec = 0;   // stands for a CommandBufferImpl
  const uint64_t stale0 = g_lifeStat[LS_STALE_HANDLE].load();
  // submit, complete, submit again: the second submit gets the list
  auto* l = new CmdList;
  g_watch = l; g_watchFreed = false;
  const CommandHandle h = ListRegister(l, &rec, 0x10000);
  CmdList* a = Acquire1(h);
  CHECK(a == l);
  ListRelease(a);   // the submission completed (QueueImpl::Reap)
  CHECK(!g_watchFreed);
  CmdList* b = Acquire1(h);
  CHECK(b == l);   // was nullptr + staleHandle before the fix
  // the same handle twice in one submit while b is still in flight
  CommandHandle hh[2] = {h, h};
  CmdList* two[2] = {};
  ListAcquire(2, hh, two);
  CHECK(two[0] == l && two[1] == l);
  // its control memory is added again while three submissions are in flight: the handle ends, the list lives on
  ListRetireCtl(&rec, 0x10000, 0x10000 + 0x400);
  CHECK(Acquire1(h) == nullptr);
  CHECK(!g_watchFreed);
  ListRelease(b); ListRelease(two[0]);
  CHECK(!g_watchFreed);
  ListRelease(two[1]);
  CHECK(g_watchFreed);   // freed with the last submission
  CHECK(g_lifeStat[LS_STALE_HANDLE].load() == stale0 + 1);   // only the deliberate submit after the retire

  // retired with nothing in flight: freed at once; a range that does not cover it leaves it alone
  l = new CmdList;
  g_watch = l; g_watchFreed = false;
  const CommandHandle h2 = ListRegister(l, &rec, 0x20040);
  ListRetireCtl(&rec, 0x30000, 0x30400);
  CHECK(!g_watchFreed && Acquire1(h2) == l);
  ListRelease(l);
  ListRetireCtl(&rec, 0x20000, 0x20400);
  CHECK(g_watchFreed);
  CHECK(Acquire1(h2) == nullptr);

  // recorded without control memory: ends with the recorder's next block or its finalize
  const int rec2 = 0;
  l = new CmdList;
  g_watch = l; g_watchFreed = false;
  const CommandHandle h3 = ListRegister(l, &rec2, 0);
  ListRetireCtl(&rec, 0x40000, 0x40400);   // another recorder's block: no effect
  CHECK(!g_watchFreed && Acquire1(h3) == l);
  ListRelease(l);
  ListRecorderGone(&rec2);
  CHECK(g_watchFreed);

  // control memory wrapped (2026-11-14): a second recording at an address that already owns one replaces it, so handles do not pile up
  l = new CmdList;
  g_watch = l; g_watchFreed = false;
  const CommandHandle h4 = ListRegister(l, &rec, 0x50000);
  auto* l2 = new CmdList;
  const CommandHandle h5 = ListRegister(l2, &rec, 0x50000);
  CHECK(g_watchFreed);                      // nothing in flight: the replaced list is freed at once
  CHECK(Acquire1(h4) == nullptr && Acquire1(h5) == l2);
  ListRelease(l2);
  // replaced while a submission is in flight: kept until that submission ends
  l = new CmdList;
  g_watch = l; g_watchFreed = false;
  const CommandHandle h6 = ListRegister(l, &rec, 0x60000);
  CmdList* fl = Acquire1(h6);
  auto* l3 = new CmdList;
  ListRegister(l3, &rec, 0x60000);
  CHECK(fl == l && !g_watchFreed && Acquire1(h6) == nullptr);
  ListRelease(fl);
  CHECK(g_watchFreed);
  // finalize retires the recorder's control range (and not another recorder's)
  const int rec3 = 0;
  g_watch = l2; g_watchFreed = false;
  ListRecorderGone(&rec3, 0x50000, 0x50400);   // another recorder: no effect on l2's handle
  CHECK(!g_watchFreed && Acquire1(h5) == l2);
  ListRelease(l2);
  ListRecorderGone(&rec, 0x50000, 0x50400);
  CHECK(g_watchFreed && Acquire1(h5) == nullptr);
  g_watch = l3; g_watchFreed = false;
  ListRecorderGone(&rec, 0x60000, 0x60400);
  CHECK(g_watchFreed);
  g_watch = nullptr;
}

static void TestF4() {
  Host host;
  QueueImpl qa, qb, qc;
  qa.host = qb.host = qc.host = &host;
  qa.timeline = (VkSemaphore)(uintptr_t)1; qb.timeline = (VkSemaphore)(uintptr_t)2; qc.timeline = (VkSemaphore)(uintptr_t)3;
  QueueLive(&qa, true); QueueLive(&qb, true);
  // the audit case: B submitted 1 and completed nothing, A completed 100; reaping on A must not free what B may still read
  qa.value = 100; g_done[1] = 100;
  qb.value = 1; g_done[2] = 0;
  bool freed = false;
  RetireFn([&] { freed = true; });
  GraveReap(&qa);   // stamps {A:100, B:1}
  GraveReap(&qa);
  CHECK(!freed);
  GraveReap(&qb);
  CHECK(!freed);
  g_done[2] = 1;   // B completes
  g_scrubbed.clear();
  GraveReap(&qa);
  CHECK(freed);
  CHECK(g_scrubbed.count(&qa) && g_scrubbed.count(&qb));   // both queues' draw state forgot it, not only the presenting queue's
  // stamped while A is behind: B completing alone is not enough
  qa.value = 101; freed = false;
  RetireFn([&] { freed = true; });
  GraveReap(&qb);
  g_done[2] = 5;
  GraveReap(&qb);
  CHECK(!freed);
  g_done[1] = 101;
  GraveReap(&qb);
  CHECK(freed);
  // a queue destroyed after finishing counts as passed; a queue listed after the stamp is not waited for
  qb.value = 7; freed = false;
  RetireFn([&] { freed = true; });
  GraveReap(&qa);   // stamps {A:101, B:7}; B has completed 5
  QueueLive(&qc, true); qc.value = 3; g_done[3] = 0;
  GraveReap(&qa);
  CHECK(!freed);
  QueueLive(&qb, false);
  GraveReap(&qa);
  CHECK(freed);
  QueueLive(&qa, false); QueueLive(&qc, false);
}

static void TestF3() {
  StageProg src;
  src.valid = true; std::snprintf(src.key, sizeof src.key, "abcdef012345"); std::snprintf(src.fkey, sizeof src.fkey, "full-key");
  src.stage = 1; src.mod = (VkShaderModule)(uintptr_t)0x77; src.cb.push_back({}); src.tex.resize(3); src.fragOutputMap = 5;
  src.spv = {1, 2, 3}; src.inputMask = 0x7; src.outLocs = {0, 1}; src.constSize = 64;
  src.fragMods[1] = {(VkShaderModule)(uintptr_t)0x99, true};
  StageProg d;
  CopyStageMeta(d, src);
  CHECK(d.valid && !std::strcmp(d.key, src.key) && !std::strcmp(d.fkey, src.fkey) && d.stage == 1 && d.mod == src.mod);
  CHECK(d.cb.size() == 1 && d.tex.size() == 3 && d.fragOutputMap == 5 && d.spv == src.spv && d.inputMask == 7 && d.outLocs == src.outLocs);
  CHECK(d.constSize == 64 && d.fragMods.empty());
  // a worker inserts variants (as CreateGraphicsPipelineImpl does under its own lock) while another builds layouts from the stage
  std::atomic<bool> stop{false};
  std::thread w([&] {
    for (uint32_t i = 0; !stop.load(std::memory_order_relaxed); i++) {
      src.fragMods.emplace(i, std::make_pair((VkShaderModule)(uintptr_t)i, false));
      if (src.fragMods.size() > 512) src.fragMods.clear();
    }
  });
  size_t cb = 0;
  for (int i = 0; i < 200000; i++) { StageProg c; CopyStageMeta(c, src); cb += c.cb.size() + c.fragMods.size(); }
  stop = true;
  w.join();
  CHECK(cb == 200000);
}

static void TestF6() {
  PoolImpl p;
  g_ensureCalls = 0;
  CHECK(PoolBuffer(&p) == VK_NULL_HANDLE);
  CHECK(PoolBacked(&p) && g_ensureCalls == 1);   // not published: EnsureBacking creates it
  CHECK(PoolBacked(&p) && g_ensureCalls == 1);   // published: acquire fast path, no call
  CHECK(PoolBuffer(&p) == (VkBuffer)(uintptr_t)0x5000);
  p.buffer = VK_NULL_HANDLE;   // ~PoolImpl does not destroy anything; keep the destructor quiet
}

int main() {
  TestF5();
  TestF4();
  TestF3();
  TestF6();
  std::printf("LIFE_TEST: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
