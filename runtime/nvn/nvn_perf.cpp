// SPDX-License-Identifier: MIT
// GPU-side half of the SNAP_PERF instrumentation (runtime/os/perf.h): a ring of timestamp pairs around every command buffer the
// layer submits. Slots are collected at the next present once the queue's timeline has passed them. Only active with SNAP_PERF set.
// All entry points except PerfFrameMark are called with Host::queueMutex held (SubmitLists / PresentWindow); it takes the lock itself.
#include <algorithm>

#include "nvn_internal.h"
#include "os/perf.h"

namespace nvn {

namespace {
constexpr uint32_t kSlots = 2048;
struct Slot { uint64_t value = 0; bool submitted = false; bool used = false; };
VkQueryPool g_pool = VK_NULL_HANDLE;
Slot g_slots[kSlots];
uint32_t g_next = 0;
bool g_failed = false;
}  // namespace

bool PerfGpuReady(Host* h) {
  if (!snap_perf::on || g_failed) return false;
  if (g_pool) return true;
  if (!h->hostQueryReset) { g_failed = true; return false; }
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * 2;
  if (vkCreateQueryPool(h->device, &qi, nullptr, &g_pool) != VK_SUCCESS) { g_failed = true; g_pool = VK_NULL_HANDLE; return false; }
  return true;
}

int PerfGpuBegin(Host* h, VkCommandBuffer cb) {
  if (!PerfGpuReady(h)) return -1;
  const uint32_t s = g_next++ % kSlots;
  if (g_slots[s].used) return -1;   // ring full of uncollected slots: skip this one
  g_slots[s] = {0, false, true};
  vkResetQueryPool(h->device, g_pool, s * 2, 2);
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_pool, s * 2);
  return (int)s;
}

void PerfGpuEnd(Host*, VkCommandBuffer cb, int slot) {
  if (slot < 0) return;
  vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pool, (uint32_t)slot * 2 + 1);
}

void PerfGpuSubmitted(int slot, uint64_t value) {
  if (slot < 0) return;
  g_slots[slot].value = value; g_slots[slot].submitted = true;
}

void PerfFrameMark(QueueImpl* q, uint64_t presentNo) {
  if (!snap_perf::on) return;
  std::lock_guard<std::mutex> lk(q->host->queueMutex);
  double busy = -1, span = -1; int cbs = 0;
  if (g_pool) {
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(q->host->device, q->timeline, &done);
    const double period = q->host->props.limits.timestampPeriod;
    uint64_t lo = UINT64_MAX, hi = 0, sum = 0;
    for (uint32_t s = 0; s < kSlots; s++) {
      Slot& sl = g_slots[s];
      if (!sl.used || !sl.submitted || sl.value > done) continue;
      uint64_t t[2] = {0, 0};
      if (vkGetQueryPoolResults(q->host->device, g_pool, s * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
        sum += t[1] - t[0]; lo = std::min(lo, t[0]); hi = std::max(hi, t[1]); cbs++;
      }
      sl.used = false; sl.submitted = false;
    }
    busy = (double)sum * period / 1e6;
    span = hi > lo ? (double)(hi - lo) * period / 1e6 : 0;
  }
  snap_perf::frame_mark(presentNo, busy, span, cbs);
}

}  // namespace nvn
