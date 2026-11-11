// SPDX-License-Identifier: MIT
// GPU -> guest transfers: CopyTextureToBuffer, CopyTextureToTexture, ReportCounter (timestamps, samples passed).
// Guest memory is written after the submit's timeline value completes (FlushReadbacks), from WaitSync, QueueFinish and
// BufferInvalidateMappedRange. Signatures follow the public NVN entry points as seen at the game's call sites.
#include <algorithm>
#include <cstring>
#include "nvn_internal.h"

namespace nvn {
namespace {
struct CopyRegion { int x, y, z, w, h, d; };
constexpr uint32_t kTsPool = 1024, kOccPool = 8192;

void EnsureQueryPools(Host* h) {
  if (!h->hostQueryReset) return;   // every NVIDIA/AMD/Intel Vulkan 1.2 driver has it; without it counters stay unsupported
  if (!h->tsPool) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kTsPool;
    vkCreateQueryPool(h->device, &qi, nullptr, &h->tsPool);
  }
  if (!h->occPool) {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = kOccPool;
    vkCreateQueryPool(h->device, &qi, nullptr, &h->occPool);
  }
}
VkImageAspectFlags AspectOf(const TextureImpl* t) {
  return (t->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_ASPECT_COLOR_BIT
         : (t->fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
}
// level/layer selected by an optional TexView (same convention as CopyBufferToTexture)
void ViewSub(const TexView& v, bool hasView, uint32_t* level, uint32_t* layer) {
  *level = (hasView && (v.flags & 1)) ? v.lvlBase : 0;
  *layer = (hasView && (v.flags & 2)) ? v.layerBase : 0;
}
bool IsLayered(int target) {
  return target == TT_2D_ARRAY || target == TT_1D_ARRAY || target == TT_CUBEMAP || target == TT_CUBEMAP_ARRAY;
}
void HostReadBarrier(VkCommandBuffer cb) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}
}  // namespace

void DoCopyTextureToBuffer(ReplayCtx& cx, const Cmd& c) {
  TextureImpl* t = c.tex[0];
  if (!t || !t->EnsureImage() || c.blob.size() < sizeof(CopyRegion) + sizeof(TexView)) return;
  CopyRegion r; TexView v;
  memcpy(&r, c.blob.data(), sizeof r); memcpy(&v, c.blob.data() + sizeof r, sizeof v);
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!cx.h->ResolveAddress(c.a[0], &pool, &off) || !pool->storage) {
    LogError("CopyTextureToBuffer: destination 0x%llx is not in a CPU-visible pool", (unsigned long long)c.a[0]);
    return;
  }
  if (!t->uploaded) UploadStaticTexture(cx, t);
  else RefreshDynamicTexture(cx, t);
  uint32_t level, layer; ViewSub(v, c.i[0] != 0, &level, &layer);
  const bool layered = IsLayered(t->b.target);
  const uint32_t bw = std::max<uint32_t>(t->fi->bw, 1), bh = std::max<uint32_t>(t->fi->bh, 1);
  const uint32_t w = (uint32_t)r.w, hgt = (uint32_t)std::max(r.h, 1), dep = (uint32_t)std::max(r.d, 1);
  const uint64_t size = (uint64_t)((w + bw - 1) / bw) * ((hgt + bh - 1) / bh) * dep * t->fi->bytes;
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, size, &buf);
  if (!map) return;
  EndRendering(cx);
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {AspectOf(t), level, layer + (layered ? (uint32_t)r.z : 0), layered ? dep : 1};
  bc.imageOffset = {r.x, r.y, layered ? 0 : r.z};
  bc.imageExtent = {w, hgt, layered ? 1 : dep};
  vkCmdCopyImageToBuffer(cx.cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  HostReadBarrier(cx.cb);
  Readback rb;
  rb.map = map; rb.dst = static_cast<uint8_t*>(pool->storage) + off; rb.size = std::min<uint64_t>(size, pool->size - off); rb.kind = 0;
  rb.sb = cx.scratch->back();
  cx.scratch->pop_back();   // ownership moves to the readback
  cx.readbacks->push_back(rb);
}

void DoCopyTextureToTexture(ReplayCtx& cx, const Cmd& c) {
  TextureImpl* s = c.tex[0]; TextureImpl* d = c.tex[1];
  if (!s || !d || !s->EnsureImage() || !d->EnsureImage() || c.blob.size() < 2 * (sizeof(CopyRegion) + sizeof(TexView))) return;
  if (s == d) { LogError("CopyTextureToTexture: source and destination are the same texture"); return; }
  CopyRegion sr, dr; TexView sv, dv;
  const uint8_t* b = c.blob.data();
  memcpy(&sr, b, sizeof sr); memcpy(&sv, b + sizeof sr, sizeof sv); b += sizeof sr + sizeof sv;
  memcpy(&dr, b, sizeof dr); memcpy(&dv, b + sizeof dr, sizeof dv);
  uint32_t sl, sy, dl, dy; ViewSub(sv, c.i[0] & 1, &sl, &sy); ViewSub(dv, c.i[0] & 2, &dl, &dy);
  const bool sLay = IsLayered(s->b.target), dLay = IsLayered(d->b.target);
  if (!s->uploaded) UploadStaticTexture(cx, s);
  else RefreshDynamicTexture(cx, s);
  EndRendering(cx);
  Transition(cx.cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(cx.cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  d->uploaded = true;
  const bool sameSize = sr.w == dr.w && sr.h == dr.h && sr.d == dr.d;
  const bool sameFmt = s->fi->vk == d->fi->vk;
  const bool wantFilter = (c.i[1] & 1) != 0;
  const VkImageAspectFlags sa = AspectOf(s), da = AspectOf(d);
  const uint32_t sdep = sLay ? (uint32_t)std::max(sr.d, 1) : 1, ddep = dLay ? (uint32_t)std::max(dr.d, 1) : 1;
  if (sameSize && sameFmt && !(c.i[1] & (4 | 8 | 16))) {
    VkImageCopy ic{};
    ic.srcSubresource = {s->fi->aspect, sl, sy + (sLay ? (uint32_t)sr.z : 0), sdep};
    ic.dstSubresource = {d->fi->aspect, dl, dy + (dLay ? (uint32_t)dr.z : 0), ddep};
    ic.srcOffset = {sr.x, sr.y, sLay ? 0 : sr.z}; ic.dstOffset = {dr.x, dr.y, dLay ? 0 : dr.z};
    ic.extent = {(uint32_t)sr.w, (uint32_t)std::max(sr.h, 1), sLay ? 1u : (uint32_t)std::max(sr.d, 1)};
    vkCmdCopyImage(cx.cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
  } else {   // different size (render scale) or format: blit; mirror flags flip the offsets
    VkImageBlit ib{};
    ib.srcSubresource = {sa, sl, sy + (sLay ? (uint32_t)sr.z : 0), sdep};
    ib.dstSubresource = {da, dl, dy + (dLay ? (uint32_t)dr.z : 0), ddep};
    auto box = [](const CopyRegion& r, bool lay, VkOffset3D o[2], bool mx, bool my) {
      int x0 = r.x, x1 = r.x + r.w, y0 = r.y, y1 = r.y + std::max(r.h, 1);
      if (mx) std::swap(x0, x1);
      if (my) std::swap(y0, y1);
      o[0] = {x0, y0, lay ? 0 : r.z}; o[1] = {x1, y1, lay ? 1 : r.z + std::max(r.d, 1)};
    };
    box(sr, sLay, ib.srcOffsets, false, false);
    box(dr, dLay, ib.dstOffsets, (c.i[1] & 4) != 0, (c.i[1] & 8) != 0);
    const bool color = sa == VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdBlitImage(cx.cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib,
                   (wantFilter && color) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  }
}

// NVNcounterType 0 = TIMESTAMP, 1 = SAMPLES_PASSED, 0x10 = TIMESTAMP_TOP. Other counters write 0 (not emulated).
void DoReportCounter(ReplayCtx& cx, const Cmd& c) {
  Host* h = cx.h;
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(c.a[0], &pool, &off) || !pool->storage) {
    LogError("ReportCounter: address 0x%llx is not in a CPU-visible pool", (unsigned long long)c.a[0]);
    return;
  }
  EnsureQueryPools(h);
  if (!h->tsPool || !h->occPool) return;
  EndRendering(cx);
  const int type = c.i[0];
  uint32_t nOcc = 0, first = 0;
  if (type == 1) {
    nOcc = std::min<uint32_t>(h->occNext - h->occReported, kOccPool / 2);
    first = h->occNext - nOcc;
    h->occReported = h->occNext;
  }
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, 8 + 8ull * std::max(nOcc, 1u), &buf);
  if (!map) return;
  const uint32_t ti = h->tsNext++ % kTsPool;
  vkResetQueryPool(h->device, h->tsPool, ti, 1);
  vkCmdWriteTimestamp2(cx.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, h->tsPool, ti);
  vkCmdCopyQueryPoolResults(cx.cb, h->tsPool, ti, 1, buf, 0, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  for (uint32_t done = 0; done < nOcc;) {   // the ring may wrap
    uint32_t q = (first + done) % kOccPool, n = std::min(nOcc - done, kOccPool - q);
    vkCmdCopyQueryPoolResults(cx.cb, h->occPool, q, n, buf, 8 + 8ull * done, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    done += n;
  }
  HostReadBarrier(cx.cb);
  Readback rb;
  rb.kind = 1; rb.counterType = type; rb.nOcc = nOcc; rb.map = map;
  rb.dst = static_cast<uint8_t*>(pool->storage) + off; rb.size = 16;
  rb.sb = cx.scratch->back(); cx.scratch->pop_back();
  cx.readbacks->push_back(rb);
}

void FlushReadbacks(Host* h, bool wait) {
  std::lock_guard<std::mutex> lk(h->rbMutex);
  size_t n = 0;
  for (; n < h->readbacks.size(); n++) {
    Readback& rb = h->readbacks[n];
    uint64_t cur = 0;
    vkGetSemaphoreCounterValue(h->device, rb.sem, &cur);
    if (cur < rb.value) {
      if (!wait) break;
      VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wi.semaphoreCount = 1; wi.pSemaphores = &rb.sem; wi.pValues = &rb.value;
      vkWaitSemaphores(h->device, &wi, UINT64_MAX);
    }
    if (rb.kind == 0) {
      memcpy(rb.dst, rb.map, (size_t)rb.size);
    } else {
      uint64_t ticks = 0, sum = 0;
      memcpy(&ticks, rb.map, 8);
      for (uint32_t i = 0; i < rb.nOcc; i++) { uint64_t v; memcpy(&v, rb.map + 8 + 8ull * i, 8); sum += v; }
      if (rb.counterType == 1) h->samplesTotal += sum;
      uint64_t out[2] = {rb.counterType == 1 ? h->samplesTotal : 0, (uint64_t)((double)ticks * h->props.limits.timestampPeriod)};
      memcpy(rb.dst, out, 16);
    }
    vkDestroyBuffer(h->device, rb.sb.buf, nullptr);
    vkFreeMemory(h->device, rb.sb.mem, nullptr);
  }
  h->readbacks.erase(h->readbacks.begin(), h->readbacks.begin() + n);
}

void DestroyQueryPools(Host* h) {
  FlushReadbacks(h, true);
  if (h->tsPool) vkDestroyQueryPool(h->device, h->tsPool, nullptr);
  if (h->occPool) vkDestroyQueryPool(h->device, h->occPool, nullptr);
  h->tsPool = h->occPool = VK_NULL_HANDLE;
}

}  // namespace nvn
