// SPDX-License-Identifier: MIT
// NVN entry points, milestone 2: programs, buffers, textures views, samplers, texture/sampler pools, state objects,
// binds and draws. Calls record host data only (commands go to the recorded stream, objects to Impl structs);
// Vulkan work happens in nvn_draw.cpp at submit.
// Signatures: the game's call sites (see analysis/runtime/nvn_usage.md for the census; BindTexture/GetTextureHandle
// were found by reading the sampler-bind loop at 0x915e80) cross-checked against tools/ref/totk/runtime
// nvn_api_*.c (MIT community reconstruction, [TOTK-NVN]); state-object defaults follow the same reference.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nvn_internal.h"
#include "nvn_state.h"

namespace nvn {
extern bool g_customBorder;   // nvn_vk.cpp: VK_EXT_custom_border_color enabled
namespace {

Host* H(const void* dev) {
  auto* h = Impl<Host>(dev, MAGIC_DEVICE);
  if (!h) LogError("invalid Device object");
  return h;
}
Cmd* Rec(void* cb, Op op) {
  auto* c = Impl<CommandBufferImpl>(cb, MAGIC_CB);
  if (!c) return nullptr;
  if (!c->recording) { LogError("command recorded outside Begin/EndRecording"); return nullptr; }
  c->cmds.emplace_back();
  c->cmds.back().op = op;
  return &c->cmds.back();
}

// ---- Program --------------------------------------------------------------------------------------------------------
Boolean ProgramInitialize(void* prog, void* dev) {   // [CALL]: (NVNprogram*, NVNdevice*) -> bool
  Host* h = H(dev);
  if (!h) return 0;
  auto* p = new ProgramImpl();
  p->host = h;
  Bind(prog, MAGIC_PROGRAM, p);
  return 1;
}
void ProgramFinalize(void* prog) {
  if (auto* p = Impl<ProgramImpl>(prog, MAGIC_PROGRAM)) Retire(GK_PROGRAM, p);   // [nvnfix] destroyed after the GPU; persistDs forgets it first
  Unbind(prog);
}
Boolean ProgramSetShaders(void* prog, int n, const ShaderData* sd) {   // (program, count, NVNshaderData{addr, control}*)
  auto* p = Impl<ProgramImpl>(prog, MAGIC_PROGRAM);
  return p && ProgramSetShadersImpl(p, n, sd);
}

// ---- Pools of descriptors ----------------------------------------------------------------------------------------------
// The real pools live in GPU memory (descriptor blocks); ours are host tables: id -> (texture, view) and id -> sampler.
// The memory pool + offset arguments only matter for the size, which the tables do not need.
Boolean TexturePoolInitialize(void* tp, const void* /*pool*/, ptrdiff_t /*offset*/, int /*n*/) {
  auto* p = new TexPoolImpl();
  p->host = g_host;
  Bind(tp, MAGIC_TEXPOOL, p);
  TexPoolLive(p, true);   // [nvnfix] slots naming a freed texture are nulled (nvn_life.cpp)
  return 1;
}
void TexturePoolFinalize(void* tp) {   // [nvnfix] recorded SetTexturePool commands and persistDs hold the pointer: free it after the GPU
  if (auto* p = Impl<TexPoolImpl>(tp, MAGIC_TEXPOOL)) { TexPoolLive(p, false); Retire(GK_TEXPOOL, p); }
  Unbind(tp);
}
void TexturePoolRegisterTexture(const void* tp, int id, const void* tex, const void* view) {
  auto* p = Impl<TexPoolImpl>(tp, MAGIC_TEXPOOL);
  auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE);
  if (!p || !t) { LogError("TexturePoolRegisterTexture: bad pool or texture"); return; }
  static const bool raceLog = getenv("SNAP_RACE_LOG") && atoi(getenv("SNAP_RACE_LOG")) > 0;
  if (raceLog) {   // registrations that overlap a replay on another thread (the race the SlotTable makes safe)
    static std::atomic<int> total{0}, overlap{0};
    const unsigned long rt = g_replayTid.load(std::memory_order_relaxed);
    const int o = (rt && rt != GetCurrentThreadId()) ? ++overlap : overlap.load();
    const int n = ++total;
    if ((rt && rt != GetCurrentThreadId() && (o <= 5 || o % 200 == 0)) || n % 2000 == 0)
      fprintf(stderr, "[race] TexturePoolRegisterTexture id %d tid %lu during replay on tid %lu: overlap %d of %d\n", id, GetCurrentThreadId(), rt, o, n);
  }
  TexSlot* s = p->slots.At(id);
  if (!s) { LogError("TexturePoolRegisterTexture: id %d out of range", id); return; }
  // Sequence lock (TexSlot::seq): odd while writing, so a concurrent ResolveHandle retries instead of pairing old and new fields.
  uint32_t q = s->seq.load(std::memory_order_relaxed);
  for (;;) {
    if (!(q & 1) && s->seq.compare_exchange_weak(q, q + 1, std::memory_order_acquire, std::memory_order_relaxed)) break;
    YieldProcessor();
    q = s->seq.load(std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_release);
  if (view) memcpy(&s->view, view, sizeof(TexView));
  s->hasView = view != nullptr;
  s->tex = t;
  s->seq.store(q + 2, std::memory_order_release);
}
void TexturePoolRegisterImage(const void* tp, int id, const void* tex, const void* view) { TexturePoolRegisterTexture(tp, id, tex, view); }
void TexturePoolSetDebugLabel(void*, const char*) {}
Boolean SamplerPoolInitialize(void* sp, const void* /*pool*/, ptrdiff_t /*offset*/, int /*n*/) {
  auto* p = new SampPoolImpl();
  p->host = g_host;
  Bind(sp, MAGIC_SAMPPOOL, p);
  SampPoolLive(p, true);   // [nvnfix]
  return 1;
}
void SamplerPoolFinalize(void* sp) {   // [nvnfix] as TexturePoolFinalize
  if (auto* p = Impl<SampPoolImpl>(sp, MAGIC_SAMPPOOL)) { SampPoolLive(p, false); Retire(GK_SAMPPOOL, p); }
  Unbind(sp);
}
void SamplerPoolRegisterSampler(const void* sp, int id, const void* samp) {
  auto* p = Impl<SampPoolImpl>(sp, MAGIC_SAMPPOOL);
  auto* s = Impl<SamplerImpl>(samp, MAGIC_SAMPLER);
  if (!p || !s) { LogError("SamplerPoolRegisterSampler: bad pool or sampler"); return; }
  if (SamplerImpl** slot = p->slots.At(id)) *slot = s;
  else LogError("SamplerPoolRegisterSampler: id %d out of range", id);
}
void SamplerPoolSetDebugLabel(void*, const char*) {}
uint64_t DeviceGetImageHandle(const void*, int id) { return (uint64_t)(uint32_t)id; }   // [TOTK-NVN] image handle = pool index

// ---- Buffer ---------------------------------------------------------------------------------------------------------
BufferBuilder* BB(void* b) { return reinterpret_cast<BufferBuilder*>(b); }
void BufferBuilderSetDevice(void* b, void* dev) { BB(b)->device = dev; }
void BufferBuilderSetDefaults(void* b) { void* d = BB(b)->device; *BB(b) = {}; BB(b)->device = d; }
void BufferBuilderSetStorage(void* b, void* pool, ptrdiff_t off, size_t size) { BB(b)->pool = pool; BB(b)->offset = off; BB(b)->size = size; }
extern "C" { void (*snap_nvn_guest_chain)(const char* tag) = nullptr; }   // set by host/boot.cpp: prints the calling guest frames (diagnostics)
extern "C" { void (*snap_nvn_on_buffer_init)() = nullptr; }               // set by host/boot.cpp: SNAP_STRESS_ARENA (off unless set)
Boolean BufferInitialize(void* buf, const void* builder) {
  if (snap_nvn_on_buffer_init) snap_nvn_on_buffer_init();
  auto* b = reinterpret_cast<const BufferBuilder*>(builder);
  auto* p = Impl<PoolImpl>(b->pool, MAGIC_POOL);
  if (!p && snap_nvn_guest_chain) {
    static std::atomic<int> n{0};
    if (n++ < 8) {
      snap_nvn_guest_chain("BufferInitialize with no memory pool");
      if (PoolAcctOn()) PoolAcctDump("BufferInitialize with no memory pool");
      if (ArenaAcctOn()) ArenaDump("BufferInitialize with no memory pool");
      // the game's GPU arena allocator (main+0x911fc0, object at [main+0x12febc0]): per arena type t, +t*0x838 + 0x21e0 base,
      // 0x21e8 size, 0x21f0 largest allowed request. A null pool here means that allocator returned no space.
      const uint64_t* gp = reinterpret_cast<const uint64_t*>(0x7100000000ull + 0x12febc0);
      if (!IsBadReadPtr(gp, 8) && *gp && !IsBadReadPtr(reinterpret_cast<const void*>(*gp + 9 * 0x838 + 0x21f8), 8))
        for (int t = 0; t < 10; t++) {
          const uint64_t* a = reinterpret_cast<const uint64_t*>(*gp + t * 0x838 + 0x21e0);
          if (a[0]) fprintf(stderr, "[arena] type %d base=0x%llx size=0x%llx maxreq=0x%llx\n", t, (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2]);
        }
      if (!IsBadReadPtr(gp, 8) && *gp && !IsBadReadPtr(reinterpret_cast<const void*>(*gp + 0x10), 0x38)) {   // arena pools the free path (0x912140) compares
        const uint64_t* pp = reinterpret_cast<const uint64_t*>(*gp + 0x10);
        fprintf(stderr, "[arena] pools +0x10..+0x40: %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)pp[0], (unsigned long long)pp[1],
                (unsigned long long)pp[2], (unsigned long long)pp[3], (unsigned long long)pp[4], (unsigned long long)pp[5], (unsigned long long)pp[6]);
      }
    }
  }
  // Crash B1 (2026-11-13): the game's buffer setup main+0x8f64b0 takes vertex/index memory from its GPU arena type 7 (56 MiB,
  // allocator main+0x911fc0) and, unlike its other paths, does not check for failure: when that arena is full it calls
  // BufferInitialize with pool = null, offset 0. Failing here left the model's mesh unbuilt, a stale resource handle followed,
  // and the guest crashed at main+0x95d54c (reproduced every run with SNAP_STRESS_ARENA=7:60). The function only touches the
  // buffer through Map / FlushMappedRange / GetAddress, so the host gives it private storage instead (what the game's own
  // fallback path, a dedicated pool, does). The arena free (0x912140) finds no arena for a null pool and changes nothing.
  // NVN_NO_POOL_FALLBACK=1 restores the old failure (A/B).
  if (!b->pool && b->size) {
    static const bool off = getenv("NVN_NO_POOL_FALLBACK") && atoi(getenv("NVN_NO_POOL_FALLBACK")) > 0;
    Host* h = g_host;
    if (!off && h) {
      const uint64_t sz = (b->size + 0xFFF) & ~0xFFFull;
      void* mem = _aligned_malloc((size_t)sz, 0x1000);
      if (mem) {
        memset(mem, 0, (size_t)sz);
        auto* fp = new PoolImpl();
        fp->host = h; fp->storage = mem; fp->size = sz; fp->flags = MP_CPU_UNCACHED | MP_GPU_CACHED; fp->guest = nullptr;
        h->RegisterPool(fp);
        auto* i = new BufferImpl();
        i->pool = fp; i->offset = 0; i->size = b->size; i->ownPool = true;
        Bind(buf, MAGIC_BUFFER, i);
        static std::atomic<int> n{0};
        const int k = ++n;
        if (k <= 8 || k % 100 == 0) LogError("BufferInitialize: no memory pool (game arena full), size 0x%llx: private host storage (%d so far)", (unsigned long long)b->size, k);
        return 1;
      }
    }
  }
  if (!p) { LogError("BufferInitialize: builder has no memory pool (pool=%p hdr magic=%llx impl=%p off=0x%llx size=0x%llx)", b->pool, b->pool ? (unsigned long long)static_cast<const ObjHdr*>(b->pool)->magic : 0ull, b->pool ? static_cast<const ObjHdr*>(b->pool)->impl : nullptr, (unsigned long long)b->offset, (unsigned long long)b->size); return 0; }
  auto* i = new BufferImpl();
  i->pool = p; i->offset = b->offset; i->size = b->size;
  if (PoolAcctOn()) { i->acctVa = p->va; PoolAcct(p, p->va, (int64_t)i->size); }
  if (ArenaAcctOn()) { i->arenaSlot = ArenaAcct(p, i->offset, (int64_t)i->size, 0); i->arenaBorn = (int)g_presentCount; }
  ArenaPoolCheck();
  Bind(buf, MAGIC_BUFFER, i);
  return 1;
}
void BufferFinalize(void* buf) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  if (i && i->acctVa) PoolAcct(i->pool, i->acctVa, -(int64_t)i->size);
  if (i && i->arenaSlot >= 0) ArenaUnacct(i->arenaSlot, (int64_t)i->size, i->arenaBorn);
  if (i && i->ownPool) OwnPoolRetire(i->pool);   // private storage from the null-pool fallback above: freed once the GPU is past it
  delete i;
  Unbind(buf);
}
void* BufferMap(const void* buf) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  return (i && i->pool->Cpu() && i->pool->storage) ? static_cast<uint8_t*>(i->pool->storage) + i->offset : nullptr;
}
uint64_t BufferGetAddress(const void* buf) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  return i ? i->pool->va + (uint64_t)i->offset : 0;
}
void BufferFlushMappedRange(const void* buf, ptrdiff_t off, size_t size) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  if (i) SyncPoolRange(i->pool, (uint64_t)(i->offset + off), size);
}
void BufferInvalidateMappedRange(const void*, ptrdiff_t, size_t) { if (g_host) FlushReadbacks(g_host, false); }   // GPU->guest transfers land in FlushReadbacks
void BufferSetDebugLabel(void*, const char*) {}
void* BufferGetMemoryPool(const void* buf) { auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER); return i ? i->pool->guest : nullptr; }
ptrdiff_t BufferGetMemoryOffset(const void* buf) { auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER); return i ? (ptrdiff_t)i->offset : 0; }
size_t BufferGetSize(const void* buf) { auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER); return i ? (size_t)i->size : 0; }

// ---- Texture builder extras + views ----------------------------------------------------------------------------------
TextureBuilder* TB(void* b) { return reinterpret_cast<TextureBuilder*>(b); }
void TextureBuilderSetSize1D(void* b, int w) { TB(b)->width = w; TB(b)->height = 1; TB(b)->depth = 1; }
void TextureBuilderSetSize3D(void* b, int w, int h, int d) { TB(b)->width = w; TB(b)->height = h; TB(b)->depth = d; }
void TextureBuilderSetStride(void* b, ptrdiff_t s) { TB(b)->stride = (int)s; }
// Packaged data is the GPU-native (block-linear) layout; deswizzling it is phase 3. The pointer is only recorded.
void TextureBuilderSetPackagedTextureData(void* b, const void* d) { TB(b)->packaged = d; }
int TextureGetDepth(const void* t);   // implemented in nvn_entry.cpp (kept there)

TexView* TV(void* v) { return reinterpret_cast<TexView*>(v); }
void TextureViewSetDefaults(void* v) { memset(v, 0, sizeof(TexView)); }
void TextureViewSetLevels(void* v, int base, int n) { TV(v)->flags |= 1; TV(v)->lvlBase = (uint8_t)base; TV(v)->lvlNum = (uint8_t)n; }
void TextureViewSetLayers(void* v, int first, int n) { TV(v)->flags |= 2; TV(v)->layerBase = (uint16_t)first; TV(v)->layerNum = (uint16_t)n; }
void TextureViewSetFormat(void* v, int f) { TV(v)->flags |= 4; TV(v)->format = (uint8_t)f; }
void TextureViewSetTarget(void* v, int t) { TV(v)->flags |= 8; TV(v)->target = (uint8_t)t; }
void TextureViewSetDepthStencilMode(void* v, int m) { TV(v)->flags |= 0x10; TV(v)->dsMode = (uint8_t)m; }

// ---- Sampler ---------------------------------------------------------------------------------------------------------
SamplerBuilder* SB(void* b) { return reinterpret_cast<SamplerBuilder*>(b); }
void SamplerBuilderSetDevice(void* b, void* dev) { SB(b)->device = dev; }
void SamplerBuilderSetDefaults(void* b) {   // GL defaults [TOTK-NVN nvnSamplerBuilderSetDefaults]
  auto* s = SB(b);
  void* d = s->device;
  *s = {};
  s->device = d;
  s->minFilter = 4;  // NEAREST_MIPMAP_LINEAR
  s->magFilter = 1;  // LINEAR
  s->wrap[0] = s->wrap[1] = s->wrap[2] = 1;  // REPEAT
  s->lodMin = -1000.f; s->lodMax = 1000.f;
  s->compareFunc = CMP_LESS;
  s->aniso = 1.f;
}
void SamplerBuilderSetMinMagFilter(void* b, int mn, int mg) { SB(b)->minFilter = mn; SB(b)->magFilter = mg; }
void SamplerBuilderSetWrapMode(void* b, int s, int t, int r) { SB(b)->wrap[0] = s; SB(b)->wrap[1] = t; SB(b)->wrap[2] = r; }
void SamplerBuilderSetLodClamp(void* b, float mn, float mx) { SB(b)->lodMin = mn; SB(b)->lodMax = mx; }
void SamplerBuilderSetLodBias(void* b, float v) { SB(b)->lodBias = v; }
void SamplerBuilderSetCompare(void* b, int mode, int func) { SB(b)->compareMode = mode; SB(b)->compareFunc = func; }
void SamplerBuilderSetBorderColor(void* b, const float* c) { memcpy(SB(b)->border, c, 16); }
void SamplerBuilderSetMaxAnisotropy(void* b, float a) { SB(b)->aniso = a; }

VkSamplerAddressMode Wrap(int w) {
  switch (w) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;            // CLAMP (GL clamp ~ clamp to edge on this hardware path)
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;   // MIRROR_CLAMP, MIRROR_CLAMP_TO_EDGE
    case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;  // MIRROR_CLAMP_TO_BORDER (approximated), CLAMP_TO_BORDER
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;           // 7 CLAMP_TO_EDGE
  }
}
Boolean SamplerInitialize(void* samp, const void* builder) {
  auto* b = reinterpret_cast<const SamplerBuilder*>(builder);
  Host* h = H(b->device);
  if (!h) return 0;
  VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  const int mn = b->minFilter;   // 0 N, 1 L, 2 N_MIP_N, 3 L_MIP_N, 4 N_MIP_L, 5 L_MIP_L
  ci.minFilter = (mn == 1 || mn == 3 || mn == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  ci.magFilter = b->magFilter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  const bool mips = mn >= 2;
  ci.mipmapMode = (mn == 4 || mn == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU = Wrap(b->wrap[0]); ci.addressModeV = Wrap(b->wrap[1]); ci.addressModeW = Wrap(b->wrap[2]);
  // Host overrides (SNAP_ANISO, SNAP_LOD_BIAS): only mipmapped, non-comparison samplers. Aniso also needs linear min and mag
  // filtering, so point/nearest samplers and shadow-compare samplers keep the game's values.
  float bias = b->lodBias;
  if (mips && b->compareMode == 0) {
    float extra = g_lodBiasExtra;
    // auto: -log2(output / render), the upscaler mip bias. A higher render scale already picks finer mips by itself, so native
    // scaling (window textures scaled too) gives 0, and supersampling (scaleDisplay off) is clamped to 0.
    if (g_lodBiasAuto) { const float out = g_scale.displayScale > 0.f ? g_scale.displayScale : g_scale.scaleDisplay ? g_scale.scale : 1.f; /* [upscale] displayScale */ extra = g_scale.scale > 0.f ? std::min(0.f, -std::log2(out / g_scale.scale)) : 0.f; }
    bias = std::clamp(bias + extra, -15.99f, 15.99f);
  }
  ci.mipLodBias = bias;
  float aniso = b->aniso;
  const bool linearMinMag = (mn == 1 || mn == 3 || mn == 5) && b->magFilter == 1;
  if (g_anisoOverride > 1.f && mips && b->compareMode == 0 && linearMinMag) aniso = std::max(aniso, g_anisoOverride);
  if (aniso > 1.f && h->props.limits.maxSamplerAnisotropy > 1.f) {
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(h->phys, &f);
    if (f.samplerAnisotropy) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = std::min(aniso, h->props.limits.maxSamplerAnisotropy); }
  }
  if (b->compareMode != 0) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)std::clamp(b->compareFunc - 1, 0, 7); }
  ci.minLod = mips ? std::max(b->lodMin, 0.f) : 0.f;
  ci.maxLod = mips ? std::max(b->lodMax, ci.minLod) : 0.f;
  const float* c = b->border;
  VkSamplerCustomBorderColorCreateInfoEXT customBorder{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
  if (c[0] == 0.f && c[1] == 0.f && c[2] == 0.f && c[3] == 0.f) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  else if (c[0] == 0.f && c[1] == 0.f && c[2] == 0.f && c[3] == 1.f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  else if (c[0] == 1.f && c[1] == 1.f && c[2] == 1.f && c[3] == 1.f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  else if (g_customBorder) {   // any other RGBA (VK_EXT_custom_border_color, enabled in nvn_vk.cpp)
    memcpy(customBorder.customBorderColor.float32, c, 16);
    ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    ci.pNext = &customBorder;
  } else if (c[3] == 0.f) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  else if (c[0] > 0.5f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  else ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  static int borderLogged = 0;
  if (getenv("NVN_BORDER_LOG") && borderLogged < 40 && (ci.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || (c[3] != 0.f && c[3] != 1.f))) fprintf(stderr, "[nvn] sampler border color %g %g %g %g (address modes %d %d %d)\n", c[0], c[1], c[2], c[3], (int)ci.addressModeU, (int)ci.addressModeV, (int)ci.addressModeW), borderLogged++;
  auto* s = new SamplerImpl();
  s->host = h; s->b = *b;
  if (vkCreateSampler(h->device, &ci, nullptr, &s->sampler) != VK_SUCCESS) { delete s; LogError("vkCreateSampler failed"); return 0; }
  Bind(samp, MAGIC_SAMPLER, s);
  return 1;
}
void SamplerFinalize(void* samp) {
  // [nvnfix] the VkSampler may still be in descriptors of submitted work, and pool slots may still name it: both are handled
  // when the graveyard frees it (nvn_life.cpp)
  if (auto* s = Impl<SamplerImpl>(samp, MAGIC_SAMPLER)) Retire(GK_SAMPLER, s);
  Unbind(samp);
}

// ---- State objects ---------------------------------------------------------------------------------------------------
template <class T> T* ST(void* p) { return reinterpret_cast<T*>(p); }
template <class T> const T* ST(const void* p) { return reinterpret_cast<const T*>(p); }

void BlendStateSetDefaults(void* s) { *ST<BlendSt>(s) = {BF_ONE, BF_ZERO, BF_ONE, BF_ZERO, BE_ADD, BE_ADD, 0, 0}; }
void BlendStateSetBlendTarget(void* s, int t) { ST<BlendSt>(s)->target = (uint8_t)t; }
void BlendStateSetBlendFunc(void* s, int a, int b, int c, int d) {
  auto* o = ST<BlendSt>(s);
  o->srcRgb = (uint8_t)a; o->dstRgb = (uint8_t)b; o->srcA = (uint8_t)c; o->dstA = (uint8_t)d;
}
void BlendStateSetBlendEquation(void* s, int rgb, int a) { ST<BlendSt>(s)->eqRgb = (uint8_t)rgb; ST<BlendSt>(s)->eqA = (uint8_t)a; }

void ColorStateSetDefaults(void* s) { *ST<ColorSt>(s) = {0, LOGIC_COPY, CMP_ALWAYS, 0}; }
void ColorStateSetBlendEnable(void* s, int i, Boolean en) {
  if (i < 0 || i > 7) return;
  auto* o = ST<ColorSt>(s);
  o->blendEn = en ? (uint8_t)(o->blendEn | (1u << i)) : (uint8_t)(o->blendEn & ~(1u << i));
}
void ColorStateSetAlphaTest(void* s, int func) { ST<ColorSt>(s)->alphaFunc = (uint8_t)func; }   // phase 3: shader specialization
int ColorStateGetAlphaTest(const void* s) { return ST<ColorSt>(s)->alphaFunc; }

void ChannelMaskStateSetDefaults(void* s) { ST<ChMaskSt>(s)->mask = 0xFFFFFFFFu; }
void ChannelMaskStateSetChannelMask(void* s, int i, Boolean r, Boolean g, Boolean b, Boolean a) {
  if (i < 0 || i > 7) return;
  uint32_t m = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
  auto* o = ST<ChMaskSt>(s);
  o->mask = (o->mask & ~(0xFu << (4 * i))) | (m << (4 * i));
}

void MultisampleStateSetDefaults(void* s) { memset(s, 0, sizeof(MsSt)); ST<MsSt>(s)->enable = 1; }
void MultisampleStateSetMultisampleEnable(void* s, Boolean v) { ST<MsSt>(s)->enable = v != 0; }
void MultisampleStateSetSamples(void* s, int n) { ST<MsSt>(s)->samples = (uint8_t)n; }
void MultisampleStateSetAlphaToCoverageEnable(void* s, Boolean v) { ST<MsSt>(s)->a2c = v != 0; }
void MultisampleStateSetAlphaToCoverageDither(void* s, Boolean v) { ST<MsSt>(s)->a2cDither = v != 0; }
Boolean MultisampleStateGetMultisampleEnable(const void* s) { return ST<MsSt>(s)->enable; }
int MultisampleStateGetSamples(const void* s) { return ST<MsSt>(s)->samples; }
Boolean MultisampleStateGetAlphaToCoverageEnable(const void* s) { return ST<MsSt>(s)->a2c; }
Boolean MultisampleStateGetAlphaToCoverageDither(const void* s) { return ST<MsSt>(s)->a2cDither; }
int MultisampleStateGetCoverageModulationMode(const void* s) { return ST<MsSt>(s)->covMod; }
Boolean MultisampleStateGetCoverageToColorEnable(const void* s) { return ST<MsSt>(s)->covToColor; }
int MultisampleStateGetCoverageToColorOutput(const void* s) { return ST<MsSt>(s)->covToColorOut; }
Boolean MultisampleStateGetSampleLocationsEnable(const void* s) { return ST<MsSt>(s)->slEn; }
Boolean MultisampleStateGetSampleLocationsGridEnable(const void* s) { return ST<MsSt>(s)->slGrid; }

void PolygonStateSetDefaults(void* s) { *ST<PolySt>(s) = {FACE_NONE, FRONT_CCW, POLY_FILL, 0}; }
void PolygonStateSetCullFace(void* s, int f) { ST<PolySt>(s)->cull = (uint8_t)f; }
void PolygonStateSetFrontFace(void* s, int f) { ST<PolySt>(s)->front = (uint8_t)f; }
void PolygonStateSetPolygonMode(void* s, int m) { ST<PolySt>(s)->mode = (uint8_t)m; }
void PolygonStateSetPolygonOffsetEnables(void* s, int e) { ST<PolySt>(s)->offsetEn = (uint8_t)e; }
int PolygonStateGetPolygonOffsetEnables(const void* s) { return ST<PolySt>(s)->offsetEn; }

void DepthStencilStateSetDefaults(void* s) {
  auto* o = ST<DsSt>(s);
  o->bits = 0;
  DsSet(*o, DS_DW, 1, 1); DsSet(*o, DS_DF, 4, CMP_LESS);
  DsSet(*o, DS_SF_F, 4, CMP_ALWAYS); DsSet(*o, DS_SF_B, 4, CMP_ALWAYS);
  for (int sh : {DS_FAIL_F, DS_ZFAIL_F, DS_ZPASS_F, DS_FAIL_B, DS_ZFAIL_B, DS_ZPASS_B}) DsSet(*o, sh, 4, SOP_KEEP);
}
void DepthStencilStateSetDepthTestEnable(void* s, Boolean v) { DsSet(*ST<DsSt>(s), DS_DT, 1, v != 0); }
void DepthStencilStateSetDepthWriteEnable(void* s, Boolean v) { DsSet(*ST<DsSt>(s), DS_DW, 1, v != 0); }
void DepthStencilStateSetDepthFunc(void* s, int f) { DsSet(*ST<DsSt>(s), DS_DF, 4, (uint32_t)f); }
void DepthStencilStateSetStencilTestEnable(void* s, Boolean v) { DsSet(*ST<DsSt>(s), DS_ST, 1, v != 0); }
void DepthStencilStateSetStencilFunc(void* s, int face, int fn) {
  if (face & FACE_FRONT) DsSet(*ST<DsSt>(s), DS_SF_F, 4, (uint32_t)fn);
  if (face & FACE_BACK) DsSet(*ST<DsSt>(s), DS_SF_B, 4, (uint32_t)fn);
}
void DepthStencilStateSetStencilOp(void* s, int face, int fail, int zfail, int zpass) {
  auto& o = *ST<DsSt>(s);
  if (face & FACE_FRONT) { DsSet(o, DS_FAIL_F, 4, (uint32_t)fail); DsSet(o, DS_ZFAIL_F, 4, (uint32_t)zfail); DsSet(o, DS_ZPASS_F, 4, (uint32_t)zpass); }
  if (face & FACE_BACK) { DsSet(o, DS_FAIL_B, 4, (uint32_t)fail); DsSet(o, DS_ZFAIL_B, 4, (uint32_t)zfail); DsSet(o, DS_ZPASS_B, 4, (uint32_t)zpass); }
}

void VertexAttribStateSetDefaults(void* s) { ST<VaSt>(s)->v = 0; }
void VertexAttribStateSetFormat(void* s, int fmt, ptrdiff_t off) {
  auto* o = ST<VaSt>(s);
  o->v = (o->v & ~0x1FFu & ~(0x7FFFFu << 13)) | ((uint32_t)fmt & 0x1FF) | (((uint32_t)off & 0x7FFFF) << 13);
}
void VertexAttribStateSetStreamIndex(void* s, int i) { auto* o = ST<VaSt>(s); o->v = (o->v & ~(0xFu << 9)) | (((uint32_t)i & 0xF) << 9); }
void VertexStreamStateSetDefaults(void* s) { *ST<VsSt>(s) = {0, 0}; }
void VertexStreamStateSetStride(void* s, ptrdiff_t st) { ST<VsSt>(s)->stride = (uint32_t)st; }

// ---- Command buffer: state binds -------------------------------------------------------------------------------------
template <class T> void BindStateCmd(void* cb, Op op, const void* s) {
  if (Cmd* c = Rec(cb, op)) memcpy(&c->a[0], s, sizeof(T));
}
void CommandBufferBindBlendState(void* cb, const void* s) { BindStateCmd<BlendSt>(cb, Op::BindBlend, s); }
void CommandBufferBindColorState(void* cb, const void* s) { BindStateCmd<ColorSt>(cb, Op::BindColor, s); }
void CommandBufferBindChannelMaskState(void* cb, const void* s) { BindStateCmd<ChMaskSt>(cb, Op::BindChannelMask, s); }
void CommandBufferBindPolygonState(void* cb, const void* s) { BindStateCmd<PolySt>(cb, Op::BindPolygon, s); }
void CommandBufferBindDepthStencilState(void* cb, const void* s) { BindStateCmd<DsSt>(cb, Op::BindDepthStencil, s); }
void CommandBufferBindMultisampleState(void* cb, const void* s) {
  if (Cmd* c = Rec(cb, Op::BindMultisample)) memcpy(&c->a[0], s, sizeof(MsSt));
}
static_assert(sizeof(MsSt) <= sizeof(uint64_t) * 4);
void CommandBufferBindVertexAttribState(void* cb, int n, const void* states) {
  if (Cmd* c = Rec(cb, Op::BindVertexAttribs)) {
    c->i[0] = n;
    c->blob.assign(static_cast<const uint8_t*>(states), static_cast<const uint8_t*>(states) + (size_t)n * sizeof(VaSt));
  }
}
void CommandBufferBindVertexStreamState(void* cb, int n, const void* states) {
  if (Cmd* c = Rec(cb, Op::BindVertexStreams)) {
    c->i[0] = n;
    c->blob.assign(static_cast<const uint8_t*>(states), static_cast<const uint8_t*>(states) + (size_t)n * sizeof(VsSt));
  }
}
void CommandBufferBindProgram(void* cb, const void* prog, int stages) {
  if (Cmd* c = Rec(cb, Op::BindProgram)) { c->p[0] = Impl<ProgramImpl>(prog, MAGIC_PROGRAM); c->i[0] = stages; }
}
struct BufferRange { uint64_t addr, size; };   // NVNbufferRange {address, size}
void CommandBufferBindVertexBuffers(void* cb, int first, int n, const BufferRange* r) {
  if (Cmd* c = Rec(cb, Op::BindVertexBuffers)) {
    c->i[0] = first; c->i[1] = n;
    c->blob.assign(reinterpret_cast<const uint8_t*>(r), reinterpret_cast<const uint8_t*>(r) + (size_t)n * sizeof(BufferRange));
  }
}
void CommandBufferBindVertexBuffer(void* cb, int idx, uint64_t addr, size_t size) {
  BufferRange r{addr, size};
  CommandBufferBindVertexBuffers(cb, idx, 1, &r);
}
void BindBufferCmd(void* cb, Op op, int stage, int idx, uint64_t addr, uint64_t size) {
  if (Cmd* c = Rec(cb, op)) { c->i[0] = stage; c->i[1] = idx; c->a[0] = addr; c->a[1] = size; }
}
void CommandBufferBindUniformBuffer(void* cb, int stage, int idx, uint64_t addr, size_t size) { BindBufferCmd(cb, Op::BindUbo, stage, idx, addr, size); }
void CommandBufferBindUniformBuffers(void* cb, int stage, int first, int n, const BufferRange* r) {
  for (int k = 0; k < n; k++) BindBufferCmd(cb, Op::BindUbo, stage, first + k, r[k].addr, r[k].size);
}
void CommandBufferBindStorageBuffer(void* cb, int stage, int idx, uint64_t addr, size_t size) { BindBufferCmd(cb, Op::BindSsbo, stage, idx, addr, size); }
void CommandBufferBindStorageBuffers(void* cb, int stage, int first, int n, const BufferRange* r) {
  for (int k = 0; k < n; k++) BindBufferCmd(cb, Op::BindSsbo, stage, first + k, r[k].addr, r[k].size);
}
// BindTexture(cb, stage, index, handle): index == the GLSL sampler binding (the game's sampler loop at 0x915e80 passes the
// loop index 0..31 and the handle from GetTextureHandle).
void CommandBufferBindTexture(void* cb, int stage, int idx, uint64_t handle) {
  if (Cmd* c = Rec(cb, Op::BindTexture)) { c->i[0] = stage; c->i[1] = idx; c->a[0] = handle; }
}
void CommandBufferBindTextures(void* cb, int stage, int first, int n, const uint64_t* h) {
  for (int k = 0; k < n; k++) CommandBufferBindTexture(cb, stage, first + k, h[k]);
}
void CommandBufferBindImages(void* cb, int stage, int first, int n, const uint64_t* h) {
  for (int k = 0; k < n; k++)
    if (Cmd* c = Rec(cb, Op::BindImage)) { c->i[0] = stage; c->i[1] = first + k; c->a[0] = h[k]; }
}
void CommandBufferSetTexturePool(void* cb, const void* tp) { if (Cmd* c = Rec(cb, Op::SetTexturePool)) c->p[0] = Impl<TexPoolImpl>(tp, MAGIC_TEXPOOL); }
void CommandBufferSetSamplerPool(void* cb, const void* sp) { if (Cmd* c = Rec(cb, Op::SetSamplerPool)) c->p[0] = Impl<SampPoolImpl>(sp, MAGIC_SAMPPOOL); }
void CommandBufferSetStencilValueMask(void* cb, int face, int v) { if (Cmd* c = Rec(cb, Op::StencilValueMask)) { c->i[0] = face; c->i[1] = v; } }
void CommandBufferSetStencilMask(void* cb, int face, int v) { if (Cmd* c = Rec(cb, Op::StencilMask)) { c->i[0] = face; c->i[1] = v; } }
void CommandBufferSetStencilRef(void* cb, int face, int v) { if (Cmd* c = Rec(cb, Op::StencilRef)) { c->i[0] = face; c->i[1] = v; } }
void CommandBufferSetPolygonOffsetClamp(void* cb, float factor, float units, float clamp) {
  if (Cmd* c = Rec(cb, Op::PolygonOffset)) { c->f[0] = factor; c->f[1] = units; c->f[2] = clamp; }
}
void CommandBufferSetShaderScratchMemory(void*, const void*, ptrdiff_t, size_t) {}   // Maxwell local-memory scratch: not needed on Vulkan

// ---- Command buffer: draws -------------------------------------------------------------------------------------------
void CommandBufferDrawArrays(void* cb, int prim, int first, int count) {
  if (Cmd* c = Rec(cb, Op::Draw)) { c->i[0] = prim; c->i[1] = first; c->i[2] = count; c->i[3] = 0; c->i[4] = 1; }
}
void CommandBufferDrawArraysInstanced(void* cb, int prim, int first, int count, int baseInstance, int instances) {
  if (Cmd* c = Rec(cb, Op::Draw)) { c->i[0] = prim; c->i[1] = first; c->i[2] = count; c->i[3] = baseInstance; c->i[4] = instances; }
}
void CommandBufferDrawElementsBaseVertex(void* cb, int prim, int type, int count, uint64_t indices, int baseVertex) {
  if (Cmd* c = Rec(cb, Op::DrawIndexed)) {
    c->i[0] = prim; c->i[1] = type; c->i[2] = count; c->i[3] = baseVertex; c->i[4] = 0; c->i[5] = 1; c->a[0] = indices;
  }
}
void CommandBufferDrawElements(void* cb, int prim, int type, int count, uint64_t indices) { CommandBufferDrawElementsBaseVertex(cb, prim, type, count, indices, 0); }
void CommandBufferDrawElementsInstanced(void* cb, int prim, int type, int count, uint64_t indices, int baseVertex, int baseInstance, int instances) {
  if (Cmd* c = Rec(cb, Op::DrawIndexed)) {
    c->i[0] = prim; c->i[1] = type; c->i[2] = count; c->i[3] = baseVertex; c->i[4] = baseInstance; c->i[5] = instances; c->a[0] = indices;
  }
}
struct CopyRegion { int x, y, z, w, h, d; };   // NVNcopyRegion
void CommandBufferCopyBufferToTexture(void* cb, uint64_t src, const void* tex, const void* view, const CopyRegion* r, int flags) {
  auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE);
  if (!t) { LogError("CopyBufferToTexture: bad texture"); return; }
  if (Cmd* c = Rec(cb, Op::CopyBufferToTexture)) {
    c->tex[0] = t; c->a[0] = src; c->i[0] = view != nullptr; c->i[1] = flags;
    c->blob.resize(sizeof(CopyRegion) + sizeof(TexView));
    memcpy(c->blob.data(), r, sizeof(CopyRegion));
    TexView v{};
    if (view) memcpy(&v, view, sizeof v);
    memcpy(c->blob.data() + sizeof(CopyRegion), &v, sizeof v);
  }
}

void CommandBufferSetAlphaRef(void* cb, float ref) { if (Cmd* c = Rec(cb, Op::SetAlphaRef)) c->f[0] = ref; }
void CommandBufferSetPatchSize(void* cb, int n) { if (Cmd* c = Rec(cb, Op::SetPatchSize)) c->i[0] = n; }
void CommandBufferDrawArraysIndirect(void* cb, int prim, uint64_t indirect) {
  if (Cmd* c = Rec(cb, Op::DrawIndirect)) { c->i[0] = prim; c->a[1] = indirect; }
}
void CommandBufferDrawElementsIndirect(void* cb, int prim, int type, uint64_t indices, uint64_t indirect) {
  if (Cmd* c = Rec(cb, Op::DrawIndexedIndirect)) { c->i[0] = prim; c->i[1] = type; c->a[0] = indices; c->a[1] = indirect; }
}
void CommandBufferDispatchCompute(void* cb, int x, int y, int z) { if (Cmd* c = Rec(cb, Op::Dispatch)) { c->i[0] = x; c->i[1] = y; c->i[2] = z; } }
void CommandBufferReportCounter(void* cb, int type, uint64_t addr) {
  if (type == 1 && g_host) g_host->wantSamples = true;   // from now on rendering scopes are wrapped in occlusion queries
  if (Cmd* c = Rec(cb, Op::ReportCounter)) { c->i[0] = type; c->a[0] = addr; }
}
void CommandBufferCopyTextureToBuffer(void* cb, const void* tex, const void* view, const CopyRegion* r, uint64_t dst, int flags) {
  auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE);
  if (!t) { LogError("CopyTextureToBuffer: bad texture"); return; }
  if (Cmd* c = Rec(cb, Op::CopyTextureToBuffer)) {
    c->tex[0] = t; c->a[0] = dst; c->i[0] = view != nullptr; c->i[1] = flags;
    c->blob.resize(sizeof(CopyRegion) + sizeof(TexView));
    memcpy(c->blob.data(), r, sizeof(CopyRegion));
    TexView v{}; if (view) memcpy(&v, view, sizeof v);
    memcpy(c->blob.data() + sizeof(CopyRegion), &v, sizeof v);
  }
}
void CommandBufferCopyTextureToTexture(void* cb, const void* src, const void* sview, const CopyRegion* sr, const void* dst, const void* dview, const CopyRegion* dr, int flags) {
  auto* s = Impl<TextureImpl>(src, MAGIC_TEXTURE); auto* d = Impl<TextureImpl>(dst, MAGIC_TEXTURE);
  if (!s || !d) { LogError("CopyTextureToTexture: bad texture"); return; }
  if (Cmd* c = Rec(cb, Op::CopyTextureToTexture)) {
    c->tex[0] = s; c->tex[1] = d; c->i[0] = (sview ? 1 : 0) | (dview ? 2 : 0); c->i[1] = flags;
    c->blob.resize(2 * (sizeof(CopyRegion) + sizeof(TexView)));
    uint8_t* b = c->blob.data();
    TexView v{};
    memcpy(b, sr, sizeof(CopyRegion)); if (sview) memcpy(&v, sview, sizeof v);
    memcpy(b + sizeof(CopyRegion), &v, sizeof v); b += sizeof(CopyRegion) + sizeof v;
    memcpy(b, dr, sizeof(CopyRegion)); v = TexView{}; if (dview) memcpy(&v, dview, sizeof v);
    memcpy(b + sizeof(CopyRegion), &v, sizeof v);
  }
}
uint64_t DeviceGetTimestampInNanoseconds(const void*, const void* counterData) {   // NVNcounterData{counter, timestamp}: the timestamp is already in ns
  uint64_t v[2] = {};
  if (counterData) memcpy(v, counterData, 16);
  return v[1];
}

// ---- table ----------------------------------------------------------------------------------------------------------------
#define X(n) {"nvn" #n, reinterpret_cast<GenericProc>(&n)}
const Entry kGfx[] = {
    X(DeviceGetImageHandle),
    X(ProgramInitialize), X(ProgramFinalize), X(ProgramSetShaders),
    X(TexturePoolInitialize), X(TexturePoolFinalize), X(TexturePoolRegisterTexture), X(TexturePoolRegisterImage), X(TexturePoolSetDebugLabel),
    X(SamplerPoolInitialize), X(SamplerPoolFinalize), X(SamplerPoolRegisterSampler), X(SamplerPoolSetDebugLabel),
    X(BufferBuilderSetDevice), X(BufferBuilderSetDefaults), X(BufferBuilderSetStorage), X(BufferInitialize), X(BufferFinalize), X(BufferMap),
    X(BufferGetAddress), X(BufferFlushMappedRange), X(BufferInvalidateMappedRange), X(BufferGetMemoryPool), X(BufferGetMemoryOffset),
    X(BufferGetSize), X(BufferSetDebugLabel),
    X(TextureBuilderSetSize1D), X(TextureBuilderSetSize3D), X(TextureBuilderSetPackagedTextureData), X(TextureBuilderSetStride),
    X(TextureViewSetDefaults), X(TextureViewSetLevels), X(TextureViewSetLayers), X(TextureViewSetFormat), X(TextureViewSetDepthStencilMode),
    X(TextureViewSetTarget),
    X(SamplerBuilderSetDevice), X(SamplerBuilderSetDefaults), X(SamplerBuilderSetMinMagFilter), X(SamplerBuilderSetWrapMode),
    X(SamplerBuilderSetLodClamp), X(SamplerBuilderSetLodBias), X(SamplerBuilderSetCompare), X(SamplerBuilderSetBorderColor),
    X(SamplerBuilderSetMaxAnisotropy), X(SamplerInitialize), X(SamplerFinalize),
    X(BlendStateSetDefaults), X(BlendStateSetBlendTarget), X(BlendStateSetBlendFunc), X(BlendStateSetBlendEquation),
    X(ColorStateSetDefaults), X(ColorStateSetBlendEnable), X(ColorStateSetAlphaTest), X(ColorStateGetAlphaTest),
    X(ChannelMaskStateSetDefaults), X(ChannelMaskStateSetChannelMask),
    X(MultisampleStateSetDefaults), X(MultisampleStateSetMultisampleEnable), X(MultisampleStateSetSamples),
    X(MultisampleStateSetAlphaToCoverageEnable), X(MultisampleStateSetAlphaToCoverageDither), X(MultisampleStateGetMultisampleEnable),
    X(MultisampleStateGetSamples), X(MultisampleStateGetAlphaToCoverageEnable), X(MultisampleStateGetAlphaToCoverageDither),
    X(MultisampleStateGetCoverageModulationMode), X(MultisampleStateGetCoverageToColorEnable), X(MultisampleStateGetCoverageToColorOutput),
    X(MultisampleStateGetSampleLocationsEnable), X(MultisampleStateGetSampleLocationsGridEnable),
    X(PolygonStateSetDefaults), X(PolygonStateSetCullFace), X(PolygonStateSetFrontFace), X(PolygonStateSetPolygonMode),
    X(PolygonStateSetPolygonOffsetEnables), X(PolygonStateGetPolygonOffsetEnables),
    X(DepthStencilStateSetDefaults), X(DepthStencilStateSetDepthTestEnable), X(DepthStencilStateSetDepthWriteEnable),
    X(DepthStencilStateSetDepthFunc), X(DepthStencilStateSetStencilTestEnable), X(DepthStencilStateSetStencilFunc),
    X(DepthStencilStateSetStencilOp),
    X(VertexAttribStateSetDefaults), X(VertexAttribStateSetFormat), X(VertexAttribStateSetStreamIndex),
    X(VertexStreamStateSetDefaults), X(VertexStreamStateSetStride),
    X(CommandBufferBindBlendState), X(CommandBufferBindChannelMaskState), X(CommandBufferBindColorState),
    X(CommandBufferBindMultisampleState), X(CommandBufferBindPolygonState), X(CommandBufferBindDepthStencilState),
    X(CommandBufferBindVertexAttribState), X(CommandBufferBindVertexStreamState), X(CommandBufferBindProgram),
    X(CommandBufferBindVertexBuffers), X(CommandBufferBindVertexBuffer), X(CommandBufferBindUniformBuffer),
    X(CommandBufferBindUniformBuffers), X(CommandBufferBindStorageBuffer), X(CommandBufferBindStorageBuffers),
    X(CommandBufferBindTexture), X(CommandBufferBindTextures), X(CommandBufferBindImages),
    X(CommandBufferDrawArrays), X(CommandBufferDrawElementsBaseVertex), X(CommandBufferDrawElements), X(CommandBufferDrawArraysInstanced),
    X(CommandBufferDrawElementsInstanced),
    X(CommandBufferSetAlphaRef), X(CommandBufferSetPatchSize), X(CommandBufferDrawArraysIndirect), X(CommandBufferDrawElementsIndirect),
    X(CommandBufferDispatchCompute), X(CommandBufferReportCounter), X(CommandBufferCopyTextureToBuffer), X(CommandBufferCopyTextureToTexture),
    X(DeviceGetTimestampInNanoseconds),
    X(CommandBufferSetStencilValueMask), X(CommandBufferSetStencilMask), X(CommandBufferSetStencilRef),
    X(CommandBufferSetPolygonOffsetClamp), X(CommandBufferCopyBufferToTexture), X(CommandBufferSetTexturePool),
    X(CommandBufferSetSamplerPool), X(CommandBufferSetShaderScratchMemory),
};
#undef X

}  // namespace

const Entry* GfxEntries(int* count) {
  *count = (int)(sizeof kGfx / sizeof *kGfx);
  return kGfx;
}

// ---- SNAP_POOL_LOG: sub-allocation accounting per memory pool (B1 triage: does a game arena run full?) ----------------------
bool PoolAcctOn() { static const bool on = getenv("SNAP_POOL_LOG") && atoi(getenv("SNAP_POOL_LOG")) > 0; return on; }
void PoolAcct(PoolImpl* p, uint64_t va, int64_t bytes) {
  Host* h = g_host;
  if (!h || !p) return;
  std::lock_guard<std::mutex> lk(h->vaMutex);
  auto it = h->vaMap.find(va);
  if (it == h->vaMap.end() || it->second != p) return;   // pool finalized first
  const int64_t live = p->subLive += bytes;
  p->subCount += bytes > 0 ? 1 : -1;
  if (p->size >= (64u << 20)) {   // big arenas: every 1/32 step up or down, with the present count (time series)
    const int64_t step = (int64_t)p->size / 32;
    if ((live - bytes) / step != live / step)
      fprintf(stderr, "[pool] %p size=0x%llx live %.1f%% count=%d at present %d\n", p->guest, (unsigned long long)p->size,
              100.0 * (double)live / (double)p->size, p->subCount.load(), (int)g_presentCount);
  }
  if (live > p->subPeak) {
    const int64_t step = std::max<int64_t>((int64_t)p->size / 16, 1);
    if (p->size >= (1u << 20) && live / step != p->subPeak / step)
      fprintf(stderr, "[pool] %p va=0x%llx size=0x%llx flags=0x%x live=0x%llx (%.1f%%) count=%d new peak\n", p->guest, (unsigned long long)p->va,
              (unsigned long long)p->size, p->flags, (unsigned long long)live, 100.0 * (double)live / (double)p->size, p->subCount.load());
    p->subPeak = live;
  }
}
void PoolAcctDump(const char* why) {
  Host* h = g_host;
  if (!h) return;
  std::lock_guard<std::mutex> lk(h->vaMutex);
  fprintf(stderr, "[pool] dump (%s):\n", why);
  for (auto& kv : h->vaMap) {
    PoolImpl* p = kv.second;
    if (p->size < (1u << 20) || !p->subCount) continue;
    fprintf(stderr, "[pool]   %p size=0x%llx flags=0x%x live=0x%llx (%.1f%%) peak=0x%llx count=%d\n", p->guest, (unsigned long long)p->size, p->flags,
            (unsigned long long)p->subLive.load(), 100.0 * (double)p->subLive.load() / (double)p->size, (unsigned long long)p->subPeak.load(), p->subCount.load());
  }
}

// ---- Fallback pools (BufferInitialize with no memory pool): deferred destruction -------------------------------------------------
// A finalized fallback buffer may still be read by command lists already submitted. Its pool is destroyed only after the queue's
// timeline passes the last value submitted when the next present saw it (stamp), as the queue does for its own retired objects.
namespace {
struct OwnPoolGrave { PoolImpl* p; uint64_t value; };   // value 0 = not stamped yet
std::mutex g_graveMu;
std::vector<OwnPoolGrave> g_grave;
}  // namespace
void OwnPoolRetire(PoolImpl* p) {
  if (!p) return;
  std::lock_guard<std::mutex> lk(g_graveMu);
  g_grave.push_back({p, 0});
}
void OwnPoolReap(QueueImpl* q) {
  if (!q || !q->host) return;
  std::vector<PoolImpl*> dead;
  {
    std::lock_guard<std::mutex> lk(g_graveMu);
    if (g_grave.empty()) return;
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(q->host->device, q->timeline, &done);
    size_t keep = 0;
    for (auto& g : g_grave) {
      if (!g.value) { g.value = q->value ? q->value : 1; g_grave[keep++] = g; }
      else if (g.value <= done) dead.push_back(g.p);
      else g_grave[keep++] = g;
    }
    g_grave.resize(keep);
  }
  for (PoolImpl* p : dead) { void* mem = p->storage; DestroyPool(p); _aligned_free(mem); }
}

// ---- SNAP_ARENA_LOG: the game's GPU arenas (B1 follow-up: what fills arena 7?) --------------------------------------------------
// Arena object at [main+0x12febc0]; per type t the TLSF-style heap sits at +t*0x838+0x21d0 with base +0x21e0, size +0x21e8 and free
// bytes +0x21f0 (the allocator 0x911fc0 fails a request larger than that; 0x8db8d0 adds a freed block back to it). Each buffer /
// texture whose storage lies in an arena is charged to its guest call site (4 return addresses); the arena's true use (size - free)
// is logged at every 1/16 step with the charged total, so space the arena holds that no live object explains shows as "uncharged".
extern "C" { int (*snap_nvn_guest_frames)(uint64_t* out, int max) = nullptr; }   // set by host/boot.cpp: guest return addresses
namespace {
struct ArenaOwner { int type, kind; uint64_t key[4]; int64_t live, peak; int count, made; int64_t lifeSum, lifeN; int lifeMax; };
std::mutex g_arMu;
std::vector<ArenaOwner> g_arOwners;
std::unordered_map<std::string, int> g_arIndex;
int64_t g_arCharged[10] = {}, g_arUsedPeak[10] = {}, g_arLastStep[10] = {};
const uint64_t* ArenaObj() {
  const uint64_t* gp = reinterpret_cast<const uint64_t*>(0x7100000000ull + 0x12febc0);
  if (IsBadReadPtr(gp, 8) || !*gp || IsBadReadPtr(reinterpret_cast<const void*>(*gp + 9 * 0x838 + 0x21f8), 8)) return nullptr;
  return reinterpret_cast<const uint64_t*>(*gp);
}
const uint64_t* ArenaHeap(const uint64_t* obj, int t) { return reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(obj) + t * 0x838 + 0x21e0); }
void ArenaTopLocked(int type, int n) {   // the n owners with the most live bytes in this arena
  std::vector<int> idx;
  for (int i = 0; i < (int)g_arOwners.size(); i++) if (g_arOwners[i].type == type && (g_arOwners[i].live || g_arOwners[i].peak >= (1 << 20))) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [](int a, int b) { return g_arOwners[a].live > g_arOwners[b].live; });
  for (int k = 0; k < (int)idx.size() && k < n; k++) {
    const ArenaOwner& o = g_arOwners[idx[k]];
    fprintf(stderr, "[arena]     %s live=0x%llx peak=0x%llx count=%d made=%d life(presents) avg %.1f max %d  main+%llx < %llx < %llx < %llx\n", o.kind ? "tex" : "buf",
            (unsigned long long)o.live, (unsigned long long)o.peak, o.count, o.made, o.lifeN ? (double)o.lifeSum / (double)o.lifeN : -1.0, o.lifeMax,
            (unsigned long long)(o.key[0] - 0x7100000000ull),
            (unsigned long long)(o.key[1] - 0x7100000000ull), (unsigned long long)(o.key[2] - 0x7100000000ull), (unsigned long long)(o.key[3] - 0x7100000000ull));
  }
}
void ArenaStepLocked(int t) {   // log the arena's real use at every 1/16 step (time series), with the top owners at a new high-water mark
  const uint64_t* obj = ArenaObj();
  if (!obj) return;
  const uint64_t* a = ArenaHeap(obj, t);
  if (!a[0] || !a[1]) return;
  const int64_t size = (int64_t)a[1], used = size - (int64_t)a[2], step = used * 16 / size;
  if (step == g_arLastStep[t]) return;
  g_arLastStep[t] = step;
  const bool peak = used > g_arUsedPeak[t];
  if (peak) g_arUsedPeak[t] = used;
  fprintf(stderr, "[arena] type %d used 0x%llx of 0x%llx (%.1f%%) charged 0x%llx uncharged 0x%llx at present %d%s\n", t, (unsigned long long)used,
          (unsigned long long)size, 100.0 * (double)used / (double)size, (unsigned long long)g_arCharged[t], (unsigned long long)(used - g_arCharged[t]),
          (int)g_presentCount, peak ? " new high-water" : "");
  if (peak && step >= 3) ArenaTopLocked(t, 12);
}
}  // namespace
bool ArenaAcctOn() { static const bool on = getenv("SNAP_ARENA_LOG") && atoi(getenv("SNAP_ARENA_LOG")) > 0; return on; }
// Once per run (always on): every game GPU arena [base, base+size) must lie inside one memory pool's storage and not overlap
// another arena. Proves the arena-7 size hook (host/boot.cpp rt_gm_arena7_size) also sized the backing pool.
void ArenaPoolCheck() {
  static std::atomic<int> done{0};
  if (done.load(std::memory_order_relaxed)) return;
  const uint64_t* obj = ArenaObj();
  if (!obj || !ArenaHeap(obj, 7)[0]) return;
  Host* h = g_host;
  if (!h || done.exchange(1)) return;
  std::lock_guard<std::mutex> lk(h->vaMutex);
  for (int t = 0; t < 10; t++) {
    const uint64_t base = ArenaHeap(obj, t)[0], size = ArenaHeap(obj, t)[1];
    if (!base) continue;
    const PoolImpl* in = nullptr;
    for (auto& kv : h->vaMap) {
      const uint64_t s = reinterpret_cast<uint64_t>(kv.second->storage);
      if (s && base >= s && base + size <= s + kv.second->size) { in = kv.second; break; }
    }
    bool overlap = false;
    for (int u = 0; u < 10; u++) {
      const uint64_t b2 = ArenaHeap(obj, u)[0], s2 = ArenaHeap(obj, u)[1];
      if (u != t && b2 && base < b2 + s2 && b2 < base + size) overlap = true;
    }
    if (in && !overlap)
      fprintf(stderr, "[arena] check: type %d size 0x%llx inside pool %p (pool size 0x%llx, arena at +0x%llx)\n", t, (unsigned long long)size, in->guest,
              (unsigned long long)in->size, (unsigned long long)(base - reinterpret_cast<uint64_t>(in->storage)));
    else
      fprintf(stderr, "[arena] check ERROR: type %d base 0x%llx size 0x%llx %s\n", t, (unsigned long long)base, (unsigned long long)size,
              in ? "overlaps another arena" : "is not inside one memory pool");
  }
}
int ArenaAcct(const PoolImpl* p, int64_t off, int64_t bytes, int kind) {
  if (!p || !p->storage || bytes <= 0) return -1;
  const uint64_t* obj = ArenaObj();
  if (!obj) return -1;
  const uint64_t addr = reinterpret_cast<uint64_t>(p->storage) + (uint64_t)off;
  int type = -1;
  for (int t = 0; t < 10; t++) { const uint64_t* a = ArenaHeap(obj, t); if (a[0] && addr >= a[0] && addr < a[0] + a[1]) { type = t; break; } }
  if (type < 0) {
    std::lock_guard<std::mutex> lk(g_arMu);
    for (int t = 0; t < 10; t++) ArenaStepLocked(t);
    return -1;
  }
  uint64_t key[4] = {};
  if (snap_nvn_guest_frames) snap_nvn_guest_frames(key, 4);
  char ks[96];
  snprintf(ks, sizeof ks, "%d.%d.%llx.%llx.%llx.%llx", type, kind, (unsigned long long)key[0], (unsigned long long)key[1], (unsigned long long)key[2], (unsigned long long)key[3]);
  std::lock_guard<std::mutex> lk(g_arMu);
  auto it = g_arIndex.find(ks);
  int slot;
  if (it == g_arIndex.end()) {
    slot = (int)g_arOwners.size();
    g_arOwners.push_back(ArenaOwner{type, kind, {key[0], key[1], key[2], key[3]}, 0, 0, 0, 0, 0, 0, 0});
    g_arIndex.emplace(ks, slot);
  } else slot = it->second;
  ArenaOwner& o = g_arOwners[slot];
  o.live += bytes; o.count++; o.made++;
  if (o.live > o.peak) o.peak = o.live;
  g_arCharged[type] += bytes;
  for (int t = 0; t < 10; t++) ArenaStepLocked(t);   // all arenas: some hold memory no buffer/texture describes
  return slot;
}
void ArenaUnacct(int slot, int64_t bytes, int born) {
  if (slot < 0) return;
  std::lock_guard<std::mutex> lk(g_arMu);
  if (slot >= (int)g_arOwners.size()) return;
  ArenaOwner& o = g_arOwners[slot];
  o.live -= bytes; o.count--;
  if (born >= 0) { const int life = (int)g_presentCount - born; o.lifeSum += life; o.lifeN++; if (life > o.lifeMax) o.lifeMax = life; }
  { static int shown = 0; if (o.type == 7 && shown < 3 && snap_nvn_guest_chain) { shown++; snap_nvn_guest_chain("arena 7 buffer finalize"); } }
  g_arCharged[o.type] -= bytes;
  for (int t = 0; t < 10; t++) ArenaStepLocked(t);
}
void ArenaDump(const char* why) {
  std::lock_guard<std::mutex> lk(g_arMu);
  const uint64_t* obj = ArenaObj();
  fprintf(stderr, "[arena] dump (%s) at present %d:\n", why, (int)g_presentCount);
  if (!obj) return;
  for (int t = 0; t < 10; t++) {
    const uint64_t* a = ArenaHeap(obj, t);
    if (!a[0]) continue;
    fprintf(stderr, "[arena]   type %d size 0x%llx free 0x%llx charged 0x%llx high-water 0x%llx\n", t, (unsigned long long)a[1], (unsigned long long)a[2],
            (unsigned long long)g_arCharged[t], (unsigned long long)g_arUsedPeak[t]);
    ArenaTopLocked(t, 16);
  }
}

}  // namespace nvn
