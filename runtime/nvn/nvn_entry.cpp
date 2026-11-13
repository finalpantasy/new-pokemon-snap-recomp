// SPDX-License-Identifier: MIT
// NVN entry points (phase 0 plus a few extras) and the getProcAddress table.
// Signatures: inferred from the game's call sites (see [CALL] tags in nvn_types.h and the per-function notes)
// and cross-checked against tools/ref/totk/runtime/nvn.h (MIT, community reconstruction) [TOTK-NVN].
// The guest only ever sees these functions through nvnBootstrapLoader / nvnDeviceGetProcAddress.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "nvn_aa.h"
#include "nvn_internal.h"
#include "upscale/upscale_nvn.h"   // [upscale]

namespace nvn {
namespace {

using Dev = void;  // guest object pointers are opaque to the callers; we type them as void*

Host* H(const void* dev) {
  auto* h = Impl<Host>(dev, MAGIC_DEVICE);
  if (!h) LogError("invalid Device object");
  return h;
}

// ---- Device ----------------------------------------------------------------------------------------------
void DeviceBuilderSetDefaults(void* b) { reinterpret_cast<DeviceBuilder*>(b)->flags = 0; }  // [CALL 0x90d654]

Boolean DeviceInitialize(void* dev, const void* builder) {  // [CALL 0x90d668]: (Device*, const DeviceBuilder*) -> bool
  if (g_host) { LogError("DeviceInitialize: a device already exists (single device supported)"); return 0; }
  auto* b = reinterpret_cast<const DeviceBuilder*>(builder);
  bool wantVal = (g_validationRequest == 1) || (b && (b->flags & 1));  // NVN_DEVICE_FLAG_DEBUG_ENABLE
  if (const char* e = getenv("NVN_VK_VALIDATION")) wantVal = wantVal || *e == '1';
  ApplyHostEnvPolicy();
  auto* h = new Host();
  if (!h->Init(wantVal)) { h->Shutdown(); delete h; return 0; }
  g_host = h;
  Bind(dev, MAGIC_DEVICE, h);
  return 1;
}

void DeviceFinalize(void* dev) {
  Host* h = H(dev);
  if (!h) return;
  h->Shutdown();
  Unbind(dev);
  delete h;
  g_host = nullptr;
}

// Placeholder limits for the Switch-like environment the game expects. Provenance per entry: the enum value
// comes from nvn_types.h; the numeric answer is our choice (Maxwell/NVN 53 typical) except where noted.
// Unverified-by-game values are the open item: the game stores 0..0x5f into a 0x60-int table at 0x90d6f0.
int DeviceInfoValue(const Host* h, int w) {
  const auto& L = h->props.limits;
  switch (w) {
    case DI_API_MAJOR_VERSION: return 53;
    case DI_API_MINOR_VERSION: return 105;
    case DI_UNIFORM_BUFFER_BINDINGS_PER_STAGE: return 14;
    case DI_MAX_UNIFORM_BUFFER_SIZE: return 65536;
    case DI_UNIFORM_BUFFER_ALIGNMENT: return 256;
    case DI_COLOR_BUFFER_BINDINGS: return 8;
    case DI_VERTEX_BUFFER_BINDINGS: return 16;
    case DI_TRANSFORM_FEEDBACK_BUFFER_BINDINGS: return 4;
    case DI_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE: return 16;
    case DI_TEXTURE_BINDINGS_PER_STAGE: return 32;
    case DI_COUNTER_ALIGNMENT: return 16;
    case DI_TRANSFORM_FEEDBACK_BUFFER_ALIGNMENT: return 4;
    case DI_TRANSFORM_FEEDBACK_CONTROL_ALIGNMENT: return 4;
    case DI_INDIRECT_DRAW_ALIGNMENT: return 4;
    case DI_VERTEX_ATTRIBUTES: return 16;
    case DI_TEXTURE_DESCRIPTOR_SIZE: return 32;
    case DI_SAMPLER_DESCRIPTOR_SIZE: return 32;
    case DI_RESERVED_TEXTURE_DESCRIPTORS: return 256;
    case DI_RESERVED_SAMPLER_DESCRIPTORS: return 256;
    case DI_COMMAND_BUFFER_COMMAND_ALIGNMENT: return 4;
    case DI_COMMAND_BUFFER_CONTROL_ALIGNMENT: return 8;
    case DI_COMMAND_BUFFER_MIN_COMMAND_SIZE: return 0x1000;
    case DI_COMMAND_BUFFER_MIN_CONTROL_SIZE: return 0x1000;
    case DI_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM: return 1;
    case DI_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED: return 2;
    case DI_SHADER_SCRATCH_MEMORY_ALIGNMENT: return 0x1000;
    case DI_SHADER_SCRATCH_MEMORY_GRANULARITY: return 0x20000;
    case DI_MAX_TEXTURE_ANISOTROPY: return 16;
    case DI_MAX_COMPUTE_WORK_GROUP_SIZE_X: return 1024;
    case DI_MAX_COMPUTE_WORK_GROUP_SIZE_Y: return 1024;
    case DI_MAX_COMPUTE_WORK_GROUP_SIZE_Z: return 64;
    case DI_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS: return 1024;
    case DI_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X: case DI_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y:
    case DI_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z: return 65535;
    case DI_IMAGE_BINDINGS_PER_STAGE: return 8;
    case DI_MAX_TEXTURE_POOL_SIZE: return 1 << 20;
    case DI_MAX_SAMPLER_POOL_SIZE: return 4096;
    case DI_MAX_VIEWPORTS: return (int)std::min<uint32_t>(16, L.maxViewports);
    case DI_MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT: return 0x1000;
    case DI_SUPPORTS_MIN_MAX_FILTERING: case DI_SUPPORTS_STENCIL8_FORMAT: case DI_SUPPORTS_ASTC_FORMATS: return 1;
    case DI_L2_SIZE: return 0x40000;
    case DI_MAX_TEXTURE_LEVELS: return 16;
    case DI_MAX_TEXTURE_LAYERS: return 2048;
    // GLSLC GPU-code version window: the game reads these first (0x2f, 0x31, 0x2e, 0x30). Real values come from
    // the nushdb control sections; 0 means "accept anything" until shader phase picks them up. UNRESOLVED.
    case DI_GLSLC_MAX_GPU_CODE_MAJOR: case DI_GLSLC_MIN_GPU_CODE_MAJOR:
    case DI_GLSLC_MAX_GPU_CODE_MINOR: case DI_GLSLC_MIN_GPU_CODE_MINOR: return 0;
    case DI_SUBPIXEL_BITS: return 8;
    case DI_INDIRECT_DISPATCH_ALIGNMENT: return 4;
    case DI_LINEAR_TEXTURE_STRIDE_ALIGNMENT: case DI_LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT: return 32;
    case DI_MEMORY_POOL_PAGE_SIZE: return 0x1000;
    case DI_UNIFORM_BUFFER_UPDATE_ALIGNMENT: return 4;
    case DI_MAX_TEXTURE_SIZE: case DI_MAX_CUBE_MAP_TEXTURE_SIZE: case DI_MAX_RECTANGLE_TEXTURE_SIZE: return 16384;
    case DI_MAX_BUFFER_TEXTURE_SIZE: return 1 << 27;
    case DI_MAX_3D_TEXTURE_SIZE: return 2048;
    case DI_MAX_PRESENT_INTERVAL: return 4;
    case DI_QUEUE_COMMAND_MEMORY_GRANULARITY: case DI_QUEUE_COMPUTE_MEMORY_GRANULARITY:
    case DI_QUEUE_CONTROL_MEMORY_GRANULARITY: return 0x1000;
    case DI_QUEUE_COMMAND_MEMORY_MIN_SIZE: case DI_QUEUE_COMPUTE_MEMORY_MIN_SIZE: case DI_QUEUE_CONTROL_MEMORY_MIN_SIZE: return 0x4000;
    case DI_QUEUE_COMMAND_MEMORY_DEFAULT_SIZE: case DI_QUEUE_COMPUTE_MEMORY_DEFAULT_SIZE: return 0x100000;
    case DI_QUEUE_CONTROL_MEMORY_DEFAULT_SIZE: return 0x10000;
    case DI_QUEUE_COMMAND_MEMORY_MIN_FLUSH_THRESHOLD: return 0x1000;
    case DI_MAX_TEXTURES_PER_WINDOW: return 4;
    case DI_MIN_TEXTURES_PER_WINDOW: return 2;
    case DI_SUPPORTS_DEBUG_LAYER: return 1;
    default: Log("DeviceGetInteger: unhandled info 0x%x -> 0", w); return 0;
  }
}
void DeviceGetInteger(const void* dev, int which, int* out) {  // [CALL 0x90d6ac]: (const Device*, int, int*)
  Host* h = H(dev);
  if (out) *out = h ? DeviceInfoValue(h, which) : 0;
}

TextureHandle DeviceGetTextureHandle(const void*, int texId, int samplerId) {  // [CALL 0x915e80..]: (dev, int, int)
  // NVN packs the texture-pool id in the low 20 bits and the sampler-pool id above (unverified for ids >= 2^20).
  return ((uint64_t)(uint32_t)samplerId << 20) | (uint32_t)texId;
}
void DeviceSetWindowOriginMode(void* dev, int mode) { if (Host* h = H(dev)) h->originMode = mode; }
void DeviceSetDepthMode(void* dev, int mode) { if (Host* h = H(dev)) h->depthMode = mode; }
Boolean DeviceRegisterFastClearColor(void* dev, const float* color, int fmt) {  // [CALL 0x90d74c]: (dev, float4*, 0x38)
  (void)dev; (void)color; (void)fmt;  // Only a hint to the Maxwell clear path; Vulkan clears are always exact.
  return 1;
}
Boolean DeviceRegisterFastClearDepth(void* dev, float d) { if (Host* h = H(dev)) h->fastClearDepth = d; return 1; }
GenericProc DeviceGetProcAddress(const void*, const char* name);

// ---- MemoryPool ------------------------------------------------------------------------------------------------
void MemoryPoolBuilderSetDevice(void* b, void* dev) { reinterpret_cast<PoolBuilder*>(b)->device = dev; }
void MemoryPoolBuilderSetDefaults(void* b) {
  auto* p = reinterpret_cast<PoolBuilder*>(b);
  void* d = p->device;   // SetDefaults keeps the device [TOTK-NVN: nvnBufferBuilderSetDefaults note]
  *p = {};
  p->device = d;
  p->flags = MP_CPU_UNCACHED | MP_GPU_CACHED;
}
void MemoryPoolBuilderSetStorage(void* b, void* mem, size_t size) {  // [CALL 0x90df94]: (b, ptr, size)
  auto* p = reinterpret_cast<PoolBuilder*>(b);
  p->storage = mem;
  p->size = size;
}
void MemoryPoolBuilderSetFlags(void* b, int flags) { reinterpret_cast<PoolBuilder*>(b)->flags = flags; }
int MemoryPoolBuilderGetFlags(const void* b) { return reinterpret_cast<const PoolBuilder*>(b)->flags; }
Boolean MemoryPoolInitialize(void* pool, const void* b) {
  auto* pb = reinterpret_cast<const PoolBuilder*>(b);
  Host* h = H(pb->device);
  if (!h) return 0;
  auto* p = new PoolImpl();
  p->host = h; p->storage = pb->storage; p->size = pb->size; p->flags = pb->flags; p->guest = pool;
  h->RegisterPool(p);
  Log("MemoryPoolInitialize pool=%p storage=%p size=0x%llx flags=0x%x", pool, pb->storage, (unsigned long long)pb->size, pb->flags);
  Bind(pool, MAGIC_POOL, p);
  return 1;
}
void MemoryPoolSetDebugLabel(void*, const char*) {}
void MemoryPoolFinalize(void* pool) {
  if (auto* p = Impl<PoolImpl>(pool, MAGIC_POOL)) DestroyPool(p);
  Unbind(pool);
}
void* MemoryPoolMap(const void* pool) {  // CPU address of the pool = the guest pointer (guest ptr == host ptr)
  auto* p = Impl<PoolImpl>(pool, MAGIC_POOL);
  return (p && p->Cpu()) ? p->storage : nullptr;
}
void MemoryPoolFlushMappedRange(const void* pool, ptrdiff_t off, size_t size) {
  auto* p = Impl<PoolImpl>(pool, MAGIC_POOL);
  if (!p || !p->Cpu() || !p->storage || (uint64_t)off + size > p->size) return;
  if (!p->buffer) {
    if (p->size > (256ull << 20) || !EnsureBacking(p)) return;  // EnsureBacking copies the whole pool once
  }
  if (p->mapped) memcpy(p->mapped + off, static_cast<uint8_t*>(p->storage) + off, size);
}
size_t MemoryPoolGetSize(const void* pool) {
  auto* p = Impl<PoolImpl>(pool, MAGIC_POOL);
  return p ? (size_t)p->size : 0;
}

// ---- Texture ----------------------------------------------------------------------------------------------------
TextureBuilder* TB(void* b) { return reinterpret_cast<TextureBuilder*>(b); }
void TextureBuilderSetDevice(void* b, void* dev) { TB(b)->device = dev; }
void TextureBuilderSetDefaults(void* b) {
  auto* t = TB(b);
  void* d = t->device;
  *t = {};
  t->device = d;
  t->target = TT_2D; t->width = t->height = t->depth = 1; t->levels = 1; t->samples = 0;
}
void TextureBuilderSetFlags(void* b, int f) { TB(b)->flags = f; }
void TextureBuilderSetTarget(void* b, int t) { TB(b)->target = t; }
void TextureBuilderSetSize2D(void* b, int w, int h) { TB(b)->width = w; TB(b)->height = h; TB(b)->depth = 1; }
void TextureBuilderSetLevels(void* b, int l) { TB(b)->levels = l; }
void TextureBuilderSetFormat(void* b, int f) { TB(b)->format = f; }
void TextureBuilderSetSamples(void* b, int s) { TB(b)->samples = s; }
size_t TextureBuilderGetStorageSize(const void* b) {
  return (size_t)TextureStorageSize(*reinterpret_cast<const TextureBuilder*>(b));  // exact Maxwell layout size, nvn_tex_vk.cpp
}
size_t TextureBuilderGetStorageAlignment(const void*) { return 0x1000; }
void TextureBuilderSetStorage(void* b, void* pool, ptrdiff_t off) { TB(b)->pool = pool; TB(b)->poolOffset = off; }
int TextureBuilderGetFlags(const void* b) { return reinterpret_cast<const TextureBuilder*>(b)->flags; }
// Guest call chain of the current TextureInitialize (nvnHostSetGuestFrame): lr, then the saved lr of each frame record
// ([fp] = caller fp, [fp+8] = return address). Guest memory is identity-mapped; each frame record is checked before the read.
thread_local uint64_t t_guestFp = 0, t_guestLr = 0;
static constexpr uint64_t kGuestMain = 0x7100000000ull;
static constexpr uint64_t kEvalDepthRet = 0x5c3d3c;      // return address of the "Evaluation DepthStencil" bl 0x5b8b10 at main+0x5c3d38
static constexpr uint32_t kTagEvalDepth = 0x45564450;    // 'EVDP', exempt by default (ScalePolicy::ScalePolicy)
static bool GuestReadable(uint64_t p) {
  MEMORY_BASIC_INFORMATION mbi;
  if (p < 0x10000 || (p & 7) || !VirtualQuery((void*)p, &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + 16 <= (uint64_t)mbi.BaseAddress + mbi.RegionSize;
}
static int GuestCallChain(uint64_t* out, int max) {
  int n = 0; uint64_t fp = t_guestFp;
  if (t_guestLr) out[n++] = t_guestLr;
  while (n < max && GuestReadable(fp)) { const uint64_t* r = (const uint64_t*)fp; if (!r[1]) break; out[n++] = r[1]; if (r[0] <= fp) break; fp = r[0]; }
  t_guestFp = t_guestLr = 0;
  return n;
}
Boolean TextureInitialize(void* tex, const void* b) {
  auto* tb = reinterpret_cast<const TextureBuilder*>(b);
  Host* h = H(tb->device);
  if (!h) return 0;
  auto* t = new TextureImpl();
  t->host = h; t->b = *tb; t->fi = LookupFormat(tb->format);
  if (!t->fi) Log("TextureInitialize: format 0x%x not mapped yet", tb->format);
  {
    uint32_t tag = g_scale.nextTag; g_scale.nextTag = 0;
    uint64_t chain[8] = {}; const int nChain = GuestCallChain(chain, 8);
    if (!tag) for (int i = 0; i < nChain; i++) if (chain[i] == kGuestMain + kEvalDepthRet) tag = kTagEvalDepth;   // call-site tag
    t->scale = g_scale.For(*tb, tag);
    { uint64_t rel[8]; for (int i = 0; i < nChain; i++) rel[i] = chain[i] - kGuestMain; UpscaleTagTexture(t, rel, nChain, tag); }   // [upscale]
    if (g_scale.scale != 1.0f || g_scale.shadowRes) { static const bool lg = getenv("SNAP_SCALE_LOG") != nullptr; if (lg && (tb->width >= 192 || t->scale != 1.0f)) {
      char cs[160] = ""; for (int i = 0, n = 0; i < nChain && n < (int)sizeof cs - 16; i++) n += snprintf(cs + n, sizeof cs - n, " %llx", (unsigned long long)(chain[i] - kGuestMain));
      fprintf(stderr, "[scale] tex %dx%d fmt=0x%x flags=0x%x tgt=%d lv=%d tag=%x -> scale %.2f; main+%s\n", tb->width, tb->height, tb->format, tb->flags, tb->target, tb->levels, tag, t->scale, cs); } }
    if (t->scale != 1.0f) {
      t->hostW = (uint32_t)std::max(1.0f, std::ceil(tb->width * t->scale));
      t->hostH = (uint32_t)std::max(1.0f, std::ceil(tb->height * t->scale));
    }
  }
  Bind(tex, MAGIC_TEXTURE, t);
  return 1;  // the VkImage is created lazily (first use as render target / present source)
}
void TextureFinalize(void* tex) {
  if (auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE)) DestroyTexture(t);
  Unbind(tex);
}
int TextureGetTarget(const void* t) { auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE); return i ? i->b.target : 0; }
int TextureGetWidth(const void* t) { auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE); return i ? i->b.width : 0; }
int TextureGetHeight(const void* t) { auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE); return i ? i->b.height : 0; }
int TextureGetDepth(const void* t) { auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE); return i ? i->b.depth : 0; }
int TextureGetFormat(const void* t) { auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE); return i ? i->b.format : 0; }
int TextureGetSamples(const void* t) { auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE); return i ? i->b.samples : 0; }
int TextureGetStorageSize(const void* t) {
  auto* i = Impl<TextureImpl>(t, MAGIC_TEXTURE);
  return i ? (int)TextureBuilderGetStorageSize(&i->b) : 0;
}

// ---- Sync -----------------------------------------------------------------------------------------------------------
Boolean SyncInitialize(void* sync, void* dev) {
  Host* h = H(dev);
  if (!h) return 0;
  auto* s = new SyncImpl();
  s->host = h;
  Bind(sync, MAGIC_SYNC, s);
  return 1;
}
void SyncFinalize(void* sync) {
  delete Impl<SyncImpl>(sync, MAGIC_SYNC);
  Unbind(sync);
}
int SyncWait(const void* sync, uint64_t timeoutNs) {  // [CALL 0x985820 / 0x98ccxx]: timeout -1 or 0
  auto* s = Impl<SyncImpl>(sync, MAGIC_SYNC);
  return s ? (int)WaitSync(s, timeoutNs) : SYNC_FAILED;
}

// ---- Queue ----------------------------------------------------------------------------------------------------------
QueueBuilder* QB(void* b) { return reinterpret_cast<QueueBuilder*>(b); }
void QueueBuilderSetDevice(void* b, void* dev) { QB(b)->device = dev; }
void QueueBuilderSetDefaults(void* b) { void* d = QB(b)->device; *QB(b) = {}; QB(b)->device = d; QB(b)->cmdSize = 0x100000; QB(b)->computeSize = 0x100000; QB(b)->ctrlSize = 0x10000; }
void QueueBuilderSetFlags(void* b, int f) { QB(b)->flags = f; }
void QueueBuilderSetCommandMemorySize(void* b, size_t s) { QB(b)->cmdSize = s; }    // [CALL 0x90e4f0]: 0x200000
void QueueBuilderSetComputeMemorySize(void* b, size_t s) { QB(b)->computeSize = s; }  // 0x200000
void QueueBuilderSetControlMemorySize(void* b, size_t s) { QB(b)->ctrlSize = s; }    // 0x20000
Boolean QueueInitialize(void* queue, const void* b) {
  Host* h = H(reinterpret_cast<const QueueBuilder*>(b)->device);
  if (!h) return 0;
  auto* q = new QueueImpl();
  q->host = h;
  q->flags = reinterpret_cast<const QueueBuilder*>(b)->flags;
  if (!InitQueue(q)) { DestroyQueue(q); return 0; }
  Bind(queue, MAGIC_QUEUE, q);
  return 1;
}
void QueueFinalize(void* queue) {
  if (auto* q = Impl<QueueImpl>(queue, MAGIC_QUEUE)) DestroyQueue(q);
  Unbind(queue);
}
void QueueSubmitCommands(void* queue, int n, const CommandHandle* handles) {  // [CALL 0x903e8c]: (q, count, handle*)
  auto* q = Impl<QueueImpl>(queue, MAGIC_QUEUE);
  if (!q || n <= 0 || !handles) return;
  std::vector<CmdList*> lists(n);
  for (int i = 0; i < n; i++) lists[i] = reinterpret_cast<CmdList*>(handles[i]);
  SubmitLists(q, n, lists.data());
}
void QueueFlush(void*) {}  // submissions are issued immediately; nothing is buffered
void QueueFinish(void* queue) { if (auto* q = Impl<QueueImpl>(queue, MAGIC_QUEUE)) QueueFinishImpl(q); }
void QueuePresentTexture(void* queue, void* window, int slot) {  // [CALL 0x98ccac]: (q, window, int index)
  auto* q = Impl<QueueImpl>(queue, MAGIC_QUEUE);
  auto* w = Impl<WindowImpl>(window, MAGIC_WINDOW);
  if (q && w) PresentWindow(q, w, slot);
}
void QueueFenceSync(void* queue, void* sync, int cond, int flags) {  // [CALL 0x903e9c]: (q, sync, 0, 0)
  (void)cond; (void)flags;
  auto* q = Impl<QueueImpl>(queue, MAGIC_QUEUE);
  auto* s = Impl<SyncImpl>(sync, MAGIC_SYNC);
  if (q && s) QueueFence(q, s);
}

// ---- Window ----------------------------------------------------------------------------------------------------------
WindowBuilder* WB(void* b) { return reinterpret_cast<WindowBuilder*>(b); }
void WindowBuilderSetDevice(void* b, void* dev) { WB(b)->device = dev; }
void WindowBuilderSetDefaults(void* b) { void* d = WB(b)->device; *WB(b) = {}; WB(b)->device = d; }
void WindowBuilderSetNativeWindow(void* b, void* native) { WB(b)->native = native; }
void WindowBuilderSetTextures(void* b, int n, void* const* textures) { WB(b)->count = n; WB(b)->textures = textures; }  // [CALL 0x985b..]
Boolean WindowInitialize(void* window, const void* b) {
  auto* wb = reinterpret_cast<const WindowBuilder*>(b);
  Host* h = H(wb->device);
  if (!h || !wb->native || wb->count < 1 || !wb->textures) { LogError("WindowInitialize: bad builder"); return 0; }
  auto* w = new WindowImpl();
  w->host = h;
  w->hwnd = wb->native;
  for (int i = 0; i < wb->count; i++) {
    auto* t = Impl<TextureImpl>(wb->textures[i], MAGIC_TEXTURE);
    if (!t || !t->EnsureImage()) { LogError("WindowInitialize: texture %d unusable", i); delete w; return 0; }
    w->tex.push_back(t);
  }
  w->slotValue.assign(w->tex.size(), 0);
  w->slotSem.assign(w->tex.size(), VK_NULL_HANDLE);
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = GetModuleHandleW(nullptr);
  sci.hwnd = static_cast<HWND>(wb->native);
  if (vkCreateWin32SurfaceKHR(h->instance, &sci, nullptr, &w->surface) != VK_SUCCESS) { LogError("surface creation failed"); delete w; return 0; }
  VkBool32 ok = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(h->phys, h->family, w->surface, &ok);
  if (!ok) { LogError("queue family cannot present to this surface"); vkDestroySurfaceKHR(h->instance, w->surface, nullptr); delete w; return 0; }
  if (!w->Recreate()) { vkDestroySurfaceKHR(h->instance, w->surface, nullptr); delete w; return 0; }
  h->windows.push_back(w);
  Bind(window, MAGIC_WINDOW, w);
  return 1;
}
void WindowFinalize(void* window) {
  if (auto* w = Impl<WindowImpl>(window, MAGIC_WINDOW)) DestroyWindow(w);
  Unbind(window);
}
int WindowAcquireTexture(void* window, void* sync, int* index) {  // [CALL 0x985800]: (window, sync, int*)
  auto* w = Impl<WindowImpl>(window, MAGIC_WINDOW);
  auto* s = Impl<SyncImpl>(sync, MAGIC_SYNC);
  if (!w) return ACQUIRE_NATIVE_ERROR;
  int slot = w->nextSlot;
  w->nextSlot = (w->nextSlot + 1) % (int)w->tex.size();
  if (index) *index = slot;
  if (s) {  // the slot is free once the present that last read it has finished on the GPU
    s->host = w->host; s->sem = w->slotSem[slot]; s->value = w->slotValue[slot]; s->armed = s->sem != VK_NULL_HANDLE;
  }
  return ACQUIRE_SUCCESS;
}
void WindowSetPresentInterval(void* window, int interval) {
  if (auto* w = Impl<WindowImpl>(window, MAGIC_WINDOW)) {
    if ((w->presentInterval <= 0) != (interval <= 0)) w->needRecreate = true;
    w->presentInterval = interval;
  }
}
void WindowSetCrop(void* window, int x, int y, int w_, int h_) {
  if (auto* w = Impl<WindowImpl>(window, MAGIC_WINDOW)) { w->crop[0] = x; w->crop[1] = y; w->crop[2] = w_; w->crop[3] = h_; }
}

// ---- CommandBuffer ---------------------------------------------------------------------------------------------------
CommandBufferImpl* CBI(void* cb) {
  auto* c = Impl<CommandBufferImpl>(cb, MAGIC_CB);
  if (!c) LogError("invalid CommandBuffer");
  return c;
}
Boolean CommandBufferInitialize(void* cb, void* dev) {
  Host* h = H(dev);
  if (!h) return 0;
  auto* c = new CommandBufferImpl();
  c->host = h;
  Bind(cb, MAGIC_CB, c);
  return 1;
}
void CommandBufferFinalize(void* cb) {
  delete Impl<CommandBufferImpl>(cb, MAGIC_CB);
  Unbind(cb);
}
void CommandBufferSetDebugLabel(void*, const char*) {}
void CommandBufferSetMemoryCallback(void* cb, CommandBufferMemoryCallback fn) { if (auto* c = CBI(cb)) c->cb = fn; }
void CommandBufferSetMemoryCallbackData(void* cb, void* d) { if (auto* c = CBI(cb)) c->cbData = d; }
// Guest command/control memory is accepted and counted but never written: recording is host-side only.
void CommandBufferAddCommandMemory(void* cb, const void*, ptrdiff_t, size_t size) { if (auto* c = CBI(cb)) c->commandBytes += size; }
void CommandBufferAddControlMemory(void* cb, void*, size_t size) { if (auto* c = CBI(cb)) c->controlBytes += size; }
void CommandBufferBeginRecording(void* cb) {
  auto* c = CBI(cb);
  if (!c) return;
  if (c->recording) LogError("BeginRecording while already recording");
  c->recording = true;
  c->cmds.clear();
}
CommandHandle CommandBufferEndRecording(void* cb) {
  auto* c = CBI(cb);
  if (!c) return 0;
  if (!c->recording) { LogError("EndRecording without BeginRecording"); return 0; }
  c->recording = false;
  auto* l = new CmdList();
  l->cmds = std::move(c->cmds);
  c->cmds.clear();
  return reinterpret_cast<CommandHandle>(l);
}
Boolean CommandBufferIsRecording(const void* cb) {
  auto* c = Impl<CommandBufferImpl>(cb, MAGIC_CB);
  return c && c->recording;
}
Cmd* Rec(void* cb, Op op) {
  auto* c = CBI(cb);
  if (!c) return nullptr;
  if (!c->recording) { LogError("command recorded outside Begin/EndRecording"); return nullptr; }
  c->cmds.emplace_back();
  c->cmds.back().op = op;
  return &c->cmds.back();
}
void CommandBufferClearColor(void* cb, int index, const float* color, int mask) {  // [CALL 0x912ff0]: (cb, idx, float4*, 0xf)
  if (Cmd* c = Rec(cb, Op::ClearColor)) {
    c->i[0] = index; c->i[1] = mask;
    memcpy(c->f, color, sizeof c->f);
  }
}
void CommandBufferClearDepthStencil(void* cb, float depth, Boolean depthMask, int stencil, int stencilMask) {
  if (Cmd* c = Rec(cb, Op::ClearDepthStencil)) {
    c->f[0] = depth; c->i[0] = depthMask; c->i[1] = stencil; c->i[2] = stencilMask;
  }
}
void CommandBufferSetViewports(void* cb, int first, int count, const float* v) {  // [CALL 0x91fef0..]: (cb, first, count, float4*)
  for (int k = 0; k < count; k++)
    if (Cmd* c = Rec(cb, Op::SetViewports)) { c->i[0] = first + k; memcpy(c->f, v + 4 * k, sizeof c->f); }
}
void CommandBufferSetScissors(void* cb, int first, int count, const int* s) {
  for (int k = 0; k < count; k++)
    if (Cmd* c = Rec(cb, Op::SetScissors)) { c->i[0] = first + k; for (int j = 0; j < 4; j++) c->i[1 + j] = s[4 * k + j]; }
}
void CommandBufferSetRenderTargets(void* cb, int n, void* const* colors, const void* const* views, const void* depth,
                                   const void* depthView) {  // [CALL 0x9135b0]: 6 args, depth in x4
  if (Cmd* c = Rec(cb, Op::SetRenderTargets)) {
    // blob: 9 x (1 byte present, TexView) -- the NVNtextureView selects the mip level / array layer that is rendered to
    c->blob.assign(9 * (1 + sizeof(TexView)), 0);
    auto put = [&](int k, const void* v) {
      if (!v) return;
      c->blob[k * (1 + sizeof(TexView))] = 1;
      memcpy(&c->blob[k * (1 + sizeof(TexView)) + 1], v, sizeof(TexView));
    };
    for (int k = 0; k < n && k < 8; k++) { c->tex[k] = colors ? Impl<TextureImpl>(colors[k], MAGIC_TEXTURE) : nullptr; if (views) put(k, views[k]); }
    c->tex[8] = depth ? Impl<TextureImpl>(depth, MAGIC_TEXTURE) : nullptr;
    put(8, depthView);
  }
}
void CommandBufferBarrier(void* cb, int bits) {  // [CALL 0x9047d0 etc.]: (cb, int bits)
  if (Cmd* c = Rec(cb, Op::Barrier)) c->i[0] = bits;
}

// ---- table ---------------------------------------------------------------------------------------------------------------
#define X(n) {"nvn" #n, reinterpret_cast<GenericProc>(&n)}
const Entry kTable[] = {
    // Device
    X(DeviceBuilderSetDefaults), X(DeviceInitialize), X(DeviceFinalize), X(DeviceGetInteger), X(DeviceGetTextureHandle),
    X(DeviceSetWindowOriginMode), X(DeviceSetDepthMode), X(DeviceRegisterFastClearColor), X(DeviceRegisterFastClearDepth),
    X(DeviceGetProcAddress),
    // Queue
    X(QueueBuilderSetDevice), X(QueueBuilderSetDefaults), X(QueueBuilderSetFlags), X(QueueBuilderSetCommandMemorySize),
    X(QueueBuilderSetComputeMemorySize), X(QueueBuilderSetControlMemorySize), X(QueueInitialize), X(QueueFinalize),
    X(QueueSubmitCommands), X(QueueFlush), X(QueueFinish), X(QueuePresentTexture), X(QueueFenceSync),
    // Sync
    X(SyncInitialize), X(SyncFinalize), X(SyncWait),
    // Window
    X(WindowBuilderSetDevice), X(WindowBuilderSetDefaults), X(WindowBuilderSetNativeWindow), X(WindowBuilderSetTextures),
    X(WindowInitialize), X(WindowFinalize), X(WindowAcquireTexture), X(WindowSetPresentInterval), X(WindowSetCrop),
    // MemoryPool
    X(MemoryPoolBuilderSetDevice), X(MemoryPoolBuilderSetDefaults), X(MemoryPoolBuilderSetStorage),
    X(MemoryPoolBuilderSetFlags), X(MemoryPoolBuilderGetFlags), X(MemoryPoolInitialize), X(MemoryPoolSetDebugLabel),
    X(MemoryPoolFinalize), X(MemoryPoolMap), X(MemoryPoolFlushMappedRange), X(MemoryPoolGetSize),
    // Texture
    X(TextureBuilderSetDevice), X(TextureBuilderSetDefaults), X(TextureBuilderSetFlags), X(TextureBuilderSetTarget),
    X(TextureBuilderSetSize2D), X(TextureBuilderSetLevels), X(TextureBuilderSetFormat), X(TextureBuilderSetSamples),
    X(TextureBuilderGetStorageSize), X(TextureBuilderGetStorageAlignment), X(TextureBuilderSetStorage),
    X(TextureBuilderGetFlags), X(TextureInitialize), X(TextureFinalize), X(TextureGetTarget), X(TextureGetWidth),
    X(TextureGetHeight), X(TextureGetDepth), X(TextureGetFormat), X(TextureGetSamples), X(TextureGetStorageSize),
    // CommandBuffer
    X(CommandBufferInitialize), X(CommandBufferFinalize), X(CommandBufferSetDebugLabel), X(CommandBufferSetMemoryCallback),
    X(CommandBufferSetMemoryCallbackData), X(CommandBufferAddCommandMemory), X(CommandBufferAddControlMemory),
    X(CommandBufferBeginRecording), X(CommandBufferEndRecording), X(CommandBufferClearColor),
    X(CommandBufferClearDepthStencil), X(CommandBufferSetViewports), X(CommandBufferSetScissors),
    X(CommandBufferSetRenderTargets), X(CommandBufferBarrier), X(CommandBufferIsRecording),
};
#undef X

const std::unordered_map<std::string, GenericProc>& Table() {
  static const auto m = [] {
    std::unordered_map<std::string, GenericProc> t;
    for (auto& e : kTable) t.emplace(e.name, e.fn);
    int n = 0;
    const Entry* g = GfxEntries(&n);   // nvn_gfx.cpp: buffers, textures, samplers, programs, state, draws
    for (int i = 0; i < n; i++) t.emplace(g[i].name, g[i].fn);
    return t;
  }();
  return m;
}
// Registration order for nvnHostProcName: the table above, then the gfx entries.
const std::vector<const char*>& Names() {
  static const auto v = [] {
    std::vector<const char*> n;
    for (auto& e : kTable) n.push_back(e.name);
    int c = 0;
    const Entry* g = GfxEntries(&c);
    for (int i = 0; i < c; i++) n.push_back(g[i].name);
    return n;
  }();
  return v;
}
GenericProc DeviceGetProcAddress(const void*, const char* name) {
  if (!name) return nullptr;
  auto it = Table().find(name);
  return it == Table().end() ? nullptr : it->second;
}

}  // namespace
}  // namespace nvn

// ---- exported surface --------------------------------------------------------------------------------------------------
NVN_HOST_EXPORT void (*nvnBootstrapLoader(const char* name))(void) {
  return nvn::DeviceGetProcAddress(nullptr, name);
}
NVN_HOST_EXPORT void nvnHostSetValidation(int enable) { nvn::g_validationRequest = enable ? 1 : 0; }
NVN_HOST_EXPORT uint32_t nvnHostValidationErrors(void) { return nvn::g_valErrors.load(); }
NVN_HOST_EXPORT uint32_t nvnHostValidationWarnings(void) { return nvn::g_valWarnings.load(); }
NVN_HOST_EXPORT void nvnHostSetFramePacing(double hz) { nvn::g_paceHz = hz < 0 ? 0 : hz; }

// ---- render-scale policy and anisotropy override (docs/nvn-vulkan-design.md, "Render-target scaling") ----------------
namespace nvn {
ScalePolicy g_scale;
// Default 16x (2026-11-12): sharper oblique surfaces with no measured cost (analysis/mods/compare/aniso_floor_zoom.png).
// SNAP_ANISO=0 or 1, or nvnHostSetAnisotropyOverride(0), restores the game's 1x.
float g_anisoOverride = 16.0f;
ScalePolicy::ScalePolicy() {
  // The Evaluation depth target ("Evaluation DepthStencil", main+0x5c3d38) keeps the original size: photo scoring counts its
  // pixels. TextureInitialize tags it from the guest call chain. (The old 1920-wide depth signature also caught the main scene
  // depth, so scaled color met unscaled depth and the device was lost at render scale 2.)
  exemptTags.push_back(0x45564450);   // kTagEvalDepth
  // Photo capture targets keep their size too: at render scale 2 the photo preview and thumbnails showed only the top-left quarter of
  // the photo (runs 4-20261112_113015, 2-20261112_120107), i.e. something reads a scaled capture target at guest coordinates. Exempted:
  // the 360x240 thumbnail target (read texel by texel by the game's BC1 encoder) and the 1080x720 Capture3D photo target (the JPEG
  // source). The 360x240 exemption alone did not fix the preview; with the 1080x720 one it is whole at scale 2 (run 5-20261112_122638). NVN_NO_THUMB_EXEMPT=1 disables both.
  if (!getenv("NVN_NO_THUMB_EXEMPT")) { exempt.push_back({360, 240, -1, false}); exempt.push_back({1080, 720, -1, false}); }
}
static bool IsDepthFmt(int f) { return f == FMT_STENCIL8 || (f >= FMT_DEPTH16 && f <= FMT_DEPTH32F_STENCIL8); }
float ScalePolicy::For(const TextureBuilder& b, uint32_t tag) const {
  // The 1024x1024 depth shadow maps (dynamic shadow and the three static ones) follow SNAP_SHADOW_RES only, never the render scale.
  if (IsDepthFmt(b.format) && (b.target == TT_2D || b.target == TT_2D_ARRAY) && b.width == 1024 && b.height == 1024 && b.samples <= 1)
    return shadowRes > 0 ? (float)shadowRes / 1024.0f : 1.0f;   // 512 (Very Low preset) halves the side
  if (scale == 1.0f || scale <= 0.0f) return 1.0f;
  // Window textures: the game composes UI and the final image into these, so they must scale too for a real 4K frame. The Evaluation
  // depth target is not a window texture and keeps its exemption below.
  if ((b.flags & TF_DISPLAY) && displayScale > 0.f) return (b.target == TT_2D && b.samples <= 1) ? displayScale : 1.0f;   // [upscale] output size
  if (b.flags & TF_DISPLAY) return (scaleDisplay && b.target == TT_2D && b.samples <= 1) ? scale : 1.0f;
  if (!(b.flags & TF_COMPRESSIBLE) || (b.flags & (TF_DISPLAY | TF_LINEAR | TF_LINEAR_RENDER_TARGET))) return 1.0f;   // not a plain render target
  if (b.target != TT_2D && b.target != TT_2D_ARRAY) return 1.0f;
  if (b.samples > 1) return 1.0f;
  if (tag) for (uint32_t t : exemptTags) if (t == tag) return 1.0f;
  for (const Exempt& e : exempt) {
    if ((e.w < 0 || e.w == b.width) && (e.h < 0 || e.h == b.height) && (e.fmt < 0 || e.fmt == b.format) && (!e.depthOnly || IsDepthFmt(b.format))) return 1.0f;
  }
  return scale;
}
}  // namespace nvn
namespace nvn {
float g_lodBiasExtra = 0.0f;
bool g_lodBiasAuto = false;
// Environment policy (all optional; unset = original behavior except that LOD bias follows the render scale when it is not 1):
//   SNAP_ANISO=1|2|4|8|16   SNAP_LOD_BIAS=<float>|auto   SNAP_RENDER_SCALE=<float>   SNAP_SHADOW_RES=1024|2048|4096
void ApplyHostEnvPolicy() {
  if (const char* e = getenv("SNAP_RENDER_SCALE")) { float v = (float)atof(e); if (v >= 0.25f && v <= 8.f) g_scale.scale = v; }
  { const float ss = AaParseEnv(); if (ss > 0.f) g_scale.scale = ss; }   // SNAP_AA=ssaa2: 2x the window height (nvn_aa.cpp)
  { float sc = 0, oc = 0; if (UpscaleApplyPolicy(&sc, &oc)) {   // [upscale] SNAP_UPSCALER: scene at sc, window textures at oc; game FXAA off unless SNAP_AA set
      g_scale.scale = sc; g_scale.displayScale = oc; if (!getenv("SNAP_AA")) g_aaMode = AA_OFF; } }
  if (const char* l = getenv("SNAP_LIGHTING")) { if (!strcmp(l, "high")) g_scale.shadowRes = 2048; }   // lighting preset (host/mods/gamemods.cpp); SNAP_SHADOW_RES below wins
  if (const char* e = getenv("SNAP_SHADOW_RES")) { int v = atoi(e); g_scale.shadowRes = ((v > 1024 && v <= 8192) || v == 512) ? v : 0; }
  if (const char* e = getenv("SNAP_ANISO")) { float v = (float)atof(e); g_anisoOverride = v > 1.f ? std::min(v, 16.f) : 0.f; }
  if (const char* e = getenv("SNAP_LOD_BIAS")) {
    if (!_stricmp(e, "auto")) g_lodBiasAuto = true; else { g_lodBiasExtra = (float)atof(e); g_lodBiasAuto = false; }
  } else g_lodBiasAuto = g_scale.scale != 1.0f;
  Log("host policy: scale=%.2f shadowRes=%d aniso=%.0f lodBias=%s%.2f", g_scale.scale, g_scale.shadowRes, g_anisoOverride, g_lodBiasAuto ? "auto " : "", g_lodBiasExtra);
}
}  // namespace nvn
NVN_HOST_EXPORT void nvnHostSetLodBias(float bias) { nvn::g_lodBiasExtra = bias; nvn::g_lodBiasAuto = false; }
NVN_HOST_EXPORT void nvnHostSetLodBiasAuto(int on) { nvn::g_lodBiasAuto = on != 0; }
NVN_HOST_EXPORT void nvnHostSetShadowResolution(int side) { nvn::g_scale.shadowRes = (side > 1024 && side <= 8192) ? side : 0; }
NVN_HOST_EXPORT int nvnHostGetShadowResolution(void) { return nvn::g_scale.shadowRes ? nvn::g_scale.shadowRes : 1024; }
NVN_HOST_EXPORT void nvnHostSetScaleDisplay(int on) { nvn::g_scale.scaleDisplay = on != 0; }
NVN_HOST_EXPORT float nvnHostGetAnisotropyOverride(void) { return nvn::g_anisoOverride; }
NVN_HOST_EXPORT void nvnHostSetRenderScale(float scale) { nvn::g_scale.scale = scale; }
NVN_HOST_EXPORT float nvnHostGetRenderScale(void) { return nvn::g_scale.scale; }
NVN_HOST_EXPORT void nvnHostAddScaleExemption(int w, int h, int nvnFormat, int depthOnly) { nvn::g_scale.exempt.push_back({w, h, nvnFormat, depthOnly != 0}); }
NVN_HOST_EXPORT void nvnHostClearScaleExemptions(void) { nvn::g_scale.exempt.clear(); nvn::g_scale.exemptTags.clear(); }
NVN_HOST_EXPORT void nvnHostSetNextTextureTag(uint32_t tag) { nvn::g_scale.nextTag = tag; }
NVN_HOST_EXPORT void nvnHostSetGuestFrame(uint64_t fp, uint64_t lr) { nvn::t_guestFp = fp; nvn::t_guestLr = lr; }
NVN_HOST_EXPORT void nvnHostAddScaleExemptionTag(uint32_t tag) { nvn::g_scale.exemptTags.push_back(tag); }
NVN_HOST_EXPORT int nvnHostGetTextureHostSize(const void* guestTexture, int* w, int* h) {
  auto* t = nvn::Impl<nvn::TextureImpl>(guestTexture, nvn::MAGIC_TEXTURE);
  if (!t) return 0;
  if (w) *w = (int)t->HW();
  if (h) *h = (int)t->HH();
  return 1;
}
NVN_HOST_EXPORT void nvnHostSetAnisotropyOverride(float maxAniso) { nvn::g_anisoOverride = maxAniso < 0 ? 0 : maxAniso; }
NVN_HOST_EXPORT int nvnHostProcCount(void) { return (int)nvn::Names().size(); }
NVN_HOST_EXPORT const char* nvnHostProcName(int i) {
  return (i >= 0 && i < nvnHostProcCount()) ? nvn::Names()[i] : nullptr;
}
NVN_HOST_EXPORT void nvnHostSetHooks(const NvnHostHooks* h) { nvn::g_hooks = h ? *h : NvnHostHooks{}; }
