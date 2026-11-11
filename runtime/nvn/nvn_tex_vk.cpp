// SPDX-License-Identifier: MIT
// Static texture upload: Maxwell block-linear deswizzle, ASTC CPU decode / BC7 transcode (with a disk cache).
#include <chrono>
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

void UploadStaticTexture(ReplayCtx& cx, TextureImpl* t) {
  if (t->uploaded) return;
  t->uploaded = true;   // one attempt only
  const TextureBuilder& b = t->b;
  if (!t->EnsureImage()) return;
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

  // Disk cache for the (expensive) ASTC transcode: keyed by a 128-bit hash of the guest data plus format/size.
  std::string cachePath;
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
  if (HashBytes(src, n) == t->srcHash) return;
  t->uploaded = false;
  UploadStaticTexture(cx, t);
}

}  // namespace nvn
