// SPDX-License-Identifier: MIT
// GPU -> guest transfers: CopyTextureToBuffer, CopyTextureToTexture, ReportCounter (timestamps, samples passed).
// Guest memory is written after the submit's timeline value completes (FlushReadbacks), from WaitSync, QueueFinish and
// BufferInvalidateMappedRange. Signatures follow the public NVN entry points as seen at the game's call sites.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nvn_internal.h"
#include <windows.h>
#include <atomic>

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
bool XferLog() { static const bool on = getenv("NVN_XFER_LOG") != nullptr; return on; }
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

// ---- render scale (SNAP_RENDER_SCALE, ssaa2, the upscalers' scene scale) ----
// A scaled texture's VkImage is HW() x HH() texels while every guest copy region is in guest texels. Before this, copies used guest
// regions on the scaled image: below scale 1 they ran past the image (the photo/album thumbnail readbacks after the score screen),
// which lost the device; above 1 they read or wrote only the top-left part. NVN_NO_XFER_SCALE=1 restores that old behavior (A/B).
bool XferScaleOn() { static const bool on = getenv("NVN_NO_XFER_SCALE") == nullptr; return on; }
bool Scaled(const TextureImpl* t) { return t->hostW != 0 || t->hostH != 0; }
// Guest box at `level` -> image texels (render scale applied, edges rounded). Layers and 3D depth are never scaled.
CopyRegion ToImage(const TextureImpl* t, uint32_t level, const CopyRegion& r) {
  if (!Scaled(t) || !XferScaleOn()) return r;
  const double gw = std::max(1, t->b.width >> level), gh = std::max(1, t->b.height >> level);
  const double sx = t->LevelW(level) / gw, sy = t->LevelH(level) / gh;
  const int x0 = (int)std::lround(r.x * sx), y0 = (int)std::lround(r.y * sy);
  const int x1 = (int)std::lround((r.x + r.w) * sx), y1 = (int)std::lround((r.y + std::max(r.h, 1)) * sy);
  CopyRegion o = r;
  o.x = x0; o.y = y0; o.w = std::max(1, x1 - x0); o.h = std::max(1, y1 - y0);
  return o;
}
// True when x/y/w/h (and z/d for 3D) fit the image level. Otherwise logs it and clamps it (unless NVN_NO_XFER_SCALE); false = skip.
bool FitBox(const TextureImpl* t, uint32_t level, CopyRegion& r, const char* what) {
  const int lw = (int)t->LevelW(level), lh = (int)t->LevelH(level);
  const bool is3d = t->b.target == TT_3D;
  const int ld = is3d ? std::max(1, std::max(t->b.depth, 1) >> level) : 1;
  const int h = std::max(r.h, 1), d = std::max(r.d, 1);
  const bool badLevel = level >= std::max(t->imgLevels, 1u);
  const bool bad = badLevel || r.x < 0 || r.y < 0 || r.w <= 0 || r.x + r.w > lw || r.y + h > lh || (is3d && (r.z < 0 || r.z + d > ld));
  if (!bad) return true;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 40)
    LogError("%s: region (%d,%d,%d %dx%dx%d) level %u runs past the %dx%d image (guest %dx%d, scale %.2f, %u levels, tex %p fmt 0x%x)%s",
             what, r.x, r.y, r.z, r.w, h, d, level, lw, lh, t->b.width, t->b.height, t->scale, t->imgLevels, (const void*)t, t->b.format,
             XferScaleOn() ? ", clamped" : ", NOT clamped (NVN_NO_XFER_SCALE)");
  if (!XferScaleOn()) return true;
  if (badLevel) return false;
  r.x = std::max(r.x, 0); r.y = std::max(r.y, 0);
  r.w = std::min(r.w, lw - r.x); r.h = std::min(h, lh - r.y);
  if (is3d) { r.z = std::max(r.z, 0); r.d = std::min(d, ld - r.z); }
  return r.w > 0 && r.h > 0 && (!is3d || r.d > 0);
}
bool BlitOk(const TextureImpl* t, bool* linear) {
  if (!t->fi || t->fi->bw != 1 || t->b.samples > 1) return false;
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(t->host->phys, t->fi->vk, &fp);
  const auto f = fp.optimalTilingFeatures;
  if (linear) *linear = t->fi->kind == 0 && (t->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  return (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (f & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}
// Unscaled stand-in (guest level-0 size, one level, the texture's layers) for guest-resolution copies; lives as long as the texture,
// so it is never destroyed while a submitted command buffer may still use it.
TextureImpl* XferShadow(TextureImpl* t) {
  if (t->xferShadow) return t->xferShadow;
  auto* s = new TextureImpl();
  s->host = t->host;
  s->b = TextureBuilder{};
  const bool layered = t->imgLayers > 1;
  s->b.device = t->b.device; s->b.target = layered ? TT_2D_ARRAY : TT_2D;
  s->b.width = t->b.width; s->b.height = std::max(t->b.height, 1); s->b.depth = layered ? (int)t->imgLayers : 1; s->b.levels = 1;
  s->b.format = t->b.format; s->b.samples = 1;
  if (!s->EnsureImage()) { delete s; return nullptr; }
  t->xferShadow = s;
  static int n = 0;
  if (n++ < 8) Log("render scale: %dx%d texture %p (scale %.2f, fmt 0x%x) gets a guest-size stand-in for guest-resolution copies", t->b.width, t->b.height, (void*)t, t->scale, t->b.format);
  return s;
}
VkImageBlit BlitBoxes(VkImageAspectFlags sa, uint32_t sl, VkImageAspectFlags da, uint32_t dl, uint32_t layer, uint32_t layers,
                      const CopyRegion& s, const CopyRegion& d) {
  VkImageBlit ib{};
  ib.srcSubresource = {sa, sl, layer, layers};
  ib.dstSubresource = {da, dl, layer, layers};
  ib.srcOffsets[0] = {s.x, s.y, 0}; ib.srcOffsets[1] = {s.x + s.w, s.y + std::max(s.h, 1), 1};
  ib.dstOffsets[0] = {d.x, d.y, 0}; ib.dstOffsets[1] = {d.x + d.w, d.y + std::max(d.h, 1), 1};
  return ib;
}
}  // namespace

bool XferFitCopy(const TextureImpl* t, VkBufferImageCopy& bc, const char* what) {
  CopyRegion r{bc.imageOffset.x, bc.imageOffset.y, bc.imageOffset.z, (int)bc.imageExtent.width, (int)bc.imageExtent.height, (int)bc.imageExtent.depth};
  const CopyRegion before = r;
  if (!FitBox(t, bc.imageSubresource.mipLevel, r, what)) return false;
  if (r.x != before.x || r.y != before.y || r.z != before.z || r.w != before.w || r.h != before.h || r.d != before.d) {
    if (!bc.bufferRowLength) bc.bufferRowLength = bc.imageExtent.width;   // keep the guest row / slice layout of the buffer
    if (!bc.bufferImageHeight) bc.bufferImageHeight = bc.imageExtent.height;
    const uint32_t bw = std::max<uint32_t>(t->fi->bw, 1), bh = std::max<uint32_t>(t->fi->bh, 1);
    const uint64_t rowB = (uint64_t)((bc.bufferRowLength + bw - 1) / bw) * t->fi->bytes, sliceB = rowB * ((bc.bufferImageHeight + bh - 1) / bh);
    bc.bufferOffset += (uint64_t)((r.x - before.x) / (int)bw) * t->fi->bytes + (uint64_t)((r.y - before.y) / (int)bh) * rowB + (uint64_t)(r.z - before.z) * sliceB;
    bc.imageOffset = {r.x, r.y, r.z};
    bc.imageExtent = {(uint32_t)r.w, (uint32_t)r.h, (uint32_t)std::max(r.d, 1)};
  }
  return true;
}

bool XferUploadScaled(ReplayCtx& cx, TextureImpl* t, VkBuffer src, const VkBufferImageCopy& gc) {
  bool linear = false;
  if (!Scaled(t) || !XferScaleOn() || !BlitOk(t, &linear)) return false;
  const uint32_t level = gc.imageSubresource.mipLevel;
  if (level >= std::max(t->imgLevels, 1u)) return false;
  TextureImpl* s = XferShadow(t);
  if (!s) return false;
  // guest region of this level, in the stand-in's level 0 (a guest level-l region fits the level-0 size)
  VkBufferImageCopy sc = gc;
  sc.imageSubresource.mipLevel = 0;
  sc.imageOffset.z = 0; sc.imageExtent.depth = 1;
  if (!XferFitCopy(s, sc, "CopyBufferToTexture (stand-in)")) return true;
  const CopyRegion g{sc.imageOffset.x, sc.imageOffset.y, 0, (int)sc.imageExtent.width, (int)std::max(sc.imageExtent.height, 1u), 1};
  CopyRegion hr = ToImage(t, level, g);
  if (!FitBox(t, level, hr, "CopyBufferToTexture (scaled target)")) return true;
  EndRendering(cx);
  Transition(cx.cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(cx.cb, src, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sc);
  Transition(cx.cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkImageBlit ib = BlitBoxes(AspectOf(s), 0, AspectOf(t), level, gc.imageSubresource.baseArrayLayer, gc.imageSubresource.layerCount, g, hr);
  vkCmdBlitImage(cx.cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib,
                 linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  if (XferLog()) fprintf(stderr, "[xfer] scaled upload tex=%p %dx%d scale %.2f: guest (%d,%d %dx%d) -> image (%d,%d %dx%d) lvl=%u\n", (void*)t, t->b.width, t->b.height, t->scale, g.x, g.y, g.w, g.h, hr.x, hr.y, hr.w, hr.h, level);
  return true;
}

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
  if (XferLog()) fprintf(stderr, "[xfer] CopyTextureToBuffer tex=%p fmt=%x %dx%d (image %ux%u scale %.2f) tgt=%d region(%d,%d,%d %dx%dx%d) lvl=%u dst=0x%llx uploaded=%d\n", (void*)t, t->b.format, t->b.width, t->b.height, t->HW(), t->HH(), t->scale, t->b.target, r.x, r.y, r.z, r.w, r.h, r.d, level, (unsigned long long)c.a[0], (int)t->uploaded);
  const bool layered = IsLayered(t->b.target);
  const uint32_t bw = std::max<uint32_t>(t->fi->bw, 1), bh = std::max<uint32_t>(t->fi->bh, 1);
  const uint32_t w = (uint32_t)r.w, hgt = (uint32_t)std::max(r.h, 1), dep = (uint32_t)std::max(r.d, 1);
  const uint64_t size = (uint64_t)((w + bw - 1) / bw) * ((hgt + bh - 1) / bh) * dep * t->fi->bytes;
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, size, &buf);
  if (!map) return;
  EndRendering(cx);
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  TextureImpl* src = t;
  uint32_t srcLevel = level;
  bool linear = false;
  if (Scaled(t) && XferScaleOn() && level < std::max(t->imgLevels, 1u) && BlitOk(t, &linear)) {
    // scaled source: resample the region into the guest-size stand-in, then copy that (the guest gets guest-resolution texels)
    if (TextureImpl* s = XferShadow(t)) {
      CopyRegion hr = ToImage(t, level, r), g{0, 0, 0, (int)w, (int)hgt, 1};
      if (FitBox(t, level, hr, "CopyTextureToBuffer (scaled source)") && FitBox(s, 0, g, "CopyTextureToBuffer (stand-in)")) {
        Transition(cx.cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageBlit ib = BlitBoxes(AspectOf(t), level, AspectOf(s), 0, layer + (layered ? (uint32_t)r.z : 0), layered ? dep : 1, hr, g);
        vkCmdBlitImage(cx.cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ib,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        Transition(cx.cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (XferLog()) fprintf(stderr, "[xfer]   scaled source: image (%d,%d %dx%d) -> stand-in %ux%u\n", hr.x, hr.y, hr.w, hr.h, w, hgt);
        src = s; srcLevel = 0;
      }
    }
  }
  VkBufferImageCopy bc{};
  bc.imageSubresource = {AspectOf(src), srcLevel, layer + (layered ? (uint32_t)r.z : 0), layered ? dep : 1};
  bc.imageOffset = src == t ? VkOffset3D{r.x, r.y, layered ? 0 : r.z} : VkOffset3D{0, 0, 0};
  bc.imageExtent = {w, hgt, layered ? 1 : dep};
  if (!XferFitCopy(src, bc, "CopyTextureToBuffer")) return;   // scratch stays with the command buffer; the guest bytes are left as they were
  vkCmdCopyImageToBuffer(cx.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  HostReadBarrier(cx.cb);
  Readback rb;
  rb.map = map; rb.dst = static_cast<uint8_t*>(pool->storage) + off; rb.size = std::min<uint64_t>(size, pool->size - off); rb.kind = 0;
  rb.sb = cx.scratch->back();
  cx.scratch->pop_back();   // ownership moves to the readback
  cx.readbacks->push_back(rb);
}

// Debug (NVN_RT_DUMP=<dir>): writes the current contents of a color texture to <dir>/NNNN_<tag>_t<ptr>_vk<fmt>_<w>x<h>.raw once the GPU is done.
void DebugDumpSampledTexture(ReplayCtx& cx, TextureImpl* t, const char* tag) {
  static const char* dir = getenv("NVN_RT_DUMP");
  static int seq = 0;
  if (!dir || !*dir || !t || seq >= 200 || !t->EnsureImage() || !t->fi || t->fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT || t->fi->bw != 1 || t->b.samples > 1) return;
  if (!t->subLayout.empty() || t->b.target == TT_3D || t->b.target == TT_CUBEMAP) return;
  const uint64_t size = (uint64_t)t->b.width * t->b.height * t->fi->bytes;
  if (size > (40u << 20)) return;
  VkBuffer buf = VK_NULL_HANDLE;
  uint8_t* map = NewScratch(cx, size, &buf);
  if (!map) return;
  const VkImageLayout before = t->layout;
  EndRendering(cx);
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bc.imageExtent = {(uint32_t)t->b.width, (uint32_t)t->b.height, 1};
  vkCmdCopyImageToBuffer(cx.cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  HostReadBarrier(cx.cb);
  if (before != VK_IMAGE_LAYOUT_UNDEFINED) Transition(cx.cb, t, before);
  char name[600];
  snprintf(name, sizeof name, "%s/%04d_%s_t%p_vk%d_%dx%d_b%u.raw", dir, seq++, tag, (void*)t, (int)t->fi->vk, t->b.width, t->b.height, (unsigned)t->fi->bytes);
  Readback rb;
  rb.map = map; rb.dst = nullptr; rb.size = size; rb.kind = 2; rb.dumpPath = name;
  rb.sb = cx.scratch->back();
  cx.scratch->pop_back();
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
  if (XferLog()) fprintf(stderr, "[xfer] CopyTextureToTexture src=%p f%x %dx%d scale %.2f (uploaded=%d) region(%d,%d,%d %dx%dx%d) -> dst=%p f%x %dx%d scale %.2f region(%d,%d,%d %dx%dx%d) flags=%d\n", (void*)s, s->b.format, s->b.width, s->b.height, s->scale, (int)s->uploaded, sr.x, sr.y, sr.z, sr.w, sr.h, sr.d, (void*)d, d->b.format, d->b.width, d->b.height, d->scale, dr.x, dr.y, dr.z, dr.w, dr.h, dr.d, c.i[1]);
  // guest regions -> image texels of each side (render scale), then fitted to the images
  sr = ToImage(s, sl, sr); dr = ToImage(d, dl, dr);
  if (!FitBox(s, sl, sr, "CopyTextureToTexture (source)") || !FitBox(d, dl, dr, "CopyTextureToTexture (destination)")) return;
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
  const bool canBlit = BlitOk(s, nullptr) && BlitOk(d, nullptr);
  if (!canBlit && !(sameSize && sameFmt) && XferScaleOn()) {   // blits need BLIT_SRC/DST (no block-compressed or multisampled images)
    static int n = 0;
    if (n++ < 16) LogError("CopyTextureToTexture: %dx%d f%x -> %dx%d f%x needs a blit the formats do not support; %s", sr.w, sr.h, s->b.format, dr.w, dr.h, d->b.format,
                           sameFmt ? "copying the overlap" : "skipped");
    if (!sameFmt) return;
    sr.w = dr.w = std::min(sr.w, dr.w); sr.h = dr.h = std::min(std::max(sr.h, 1), std::max(dr.h, 1));
  }
  if (sr.w == dr.w && sr.h == dr.h && sr.d == dr.d && sameFmt && !(c.i[1] & (4 | 8 | 16))) {
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
  if (XferLog()) fprintf(stderr, "[xfer] ReportCounter type=%d dst=0x%llx\n", c.i[0], (unsigned long long)c.a[0]);
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

// Records a fragment-shader SSBO range for copy-back into guest memory after this submit (Readback kind 3). The guest bytes are
// snapshotted now: if the CPU rewrites the range before the copy-back lands (it resets the counters for the next photo), the CPU wins.
void QueueShaderWriteback(ReplayCtx& cx, PoolImpl* pool, uint64_t off, uint64_t size) {
  for (auto& r : *cx.readbacks) if (r.kind == 3 && r.pool == pool && r.poolOff == off && r.size == size) return;
  Readback rb;
  rb.kind = 3; rb.pool = pool; rb.poolOff = off; rb.size = size;
  rb.map = pool->mapped + off; rb.dst = static_cast<uint8_t*>(pool->storage) + off;
  rb.expect.assign(rb.dst, rb.dst + size);
  static const bool xl = getenv("NVN_XFER_LOG") != nullptr;
  if (xl) { static int n = 0; if (n++ < 20000) fprintf(stderr, "[xfer] shader write-back queued pool %p (size 0x%llx) off=0x%llx size=0x%llx dst=%p p%d\n", (void*)pool, (unsigned long long)pool->size, (unsigned long long)off, (unsigned long long)size, (void*)rb.dst, g_presentCount); }
  cx.readbacks->push_back(std::move(rb));
}

// Debug (NVN_WB_WATCH=1): after a fragment-shader write-back lands, the page gets PAGE_GUARD; the first access logs the host RIP
// (resolve it with build\snap\snap_boot.map: lifted functions are f_71xxxxxxxx) so the CPU reader of the evaluation counts can be found.
static std::atomic<int> g_wbWatchHits{0};
static LONG CALLBACK WbWatchHandler(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  if (g_wbWatchHits.fetch_add(1) < 64) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    fprintf(stderr, "[wbwatch] p%d access %s 0x%llx by rip=0x%llx (exe+0x%llx) thread=%lu\n", g_presentCount,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
            (unsigned long long)ep->ContextRecord->Rip, (unsigned long long)(ep->ContextRecord->Rip - base), GetCurrentThreadId());
    void* fr[24]; const USHORT nf = RtlCaptureStackBackTrace(0, 24, fr, nullptr);
    fprintf(stderr, "[wbwatch]   stack:");
    for (USHORT i = 0; i < nf; i++) fprintf(stderr, " exe+0x%llx", (unsigned long long)((uintptr_t)fr[i] - base));
    fprintf(stderr, "\n");
    fflush(stderr);
  }
  return EXCEPTION_CONTINUE_EXECUTION;   // the guard bit is cleared by the fault; the access then succeeds
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
    if (XferLog()) { uint64_t nz = 0; if (rb.kind == 0 || rb.kind == 3) for (uint64_t i = 0; i < rb.size; i++) nz += rb.map[i] != 0; fprintf(stderr, "[xfer] flush kind=%d dst=%p size=%llu nonzero=%llu\n", rb.kind, (void*)rb.dst, (unsigned long long)rb.size, (unsigned long long)nz); }
    if (rb.kind == 2) {
      if (FILE* f = fopen(rb.dumpPath.c_str(), "wb")) { fwrite(rb.map, 1, (size_t)rb.size, f); fclose(f); }
    } else if (rb.kind == 0) {
      memcpy(rb.dst, rb.map, (size_t)rb.size);
    } else if (rb.kind == 3) {
      if (memcmp(rb.dst, rb.expect.data(), (size_t)rb.size) == 0) {
        memcpy(rb.dst, rb.map, (size_t)rb.size);
        if (XferLog()) {   // the counts themselves (EvaluationAggregatePS: one uint per bucket)
          fprintf(stderr, "[xfer] wb values %p:", (void*)rb.dst);
          for (uint64_t i = 0; i + 4 <= rb.size && i < 128; i += 4) { uint32_t v; memcpy(&v, rb.dst + i, 4); fprintf(stderr, " %u", v); }
          fprintf(stderr, "\n");
        }
        static const bool watch = getenv("NVN_WB_WATCH") != nullptr;
        if (watch) {
          static bool installed = false;
          if (!installed) { installed = true; AddVectoredExceptionHandler(1, WbWatchHandler); }
          DWORD old = 0;
          VirtualProtect(rb.dst, (size_t)rb.size, PAGE_READWRITE | PAGE_GUARD, &old);
        }
      }
      else if (XferLog()) fprintf(stderr, "[xfer] shader write-back skipped: the CPU rewrote %p since the submit\n", (void*)rb.dst);
      rb.pool->wbPending.fetch_sub(1, std::memory_order_relaxed);
      continue;   // no scratch buffer: the source is the pool's own mapped mirror
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
