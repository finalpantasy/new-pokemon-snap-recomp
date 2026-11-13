// SPDX-License-Identifier: MIT
// Host-side objects behind the opaque guest NVN structs. Guest memory only holds {magic, impl*}.
#pragma once
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "nvn_host.h"
#include "nvn_types.h"

namespace nvn {

// ---- logging -------------------------------------------------------------------------------------------
void Log(const char* fmt, ...);        // only with env NVN_LOG=1
void LogError(const char* fmt, ...);   // always (stderr)

// ---- guest object header ---------------------------------------------------------------------------------
enum : uint64_t {
  MAGIC_DEVICE = 0x4e564e4445563031ull, MAGIC_QUEUE = 0x4e564e5155453031ull,
  MAGIC_CB = 0x4e564e4342303031ull, MAGIC_POOL = 0x4e564e504f4f4c31ull,
  MAGIC_TEXTURE = 0x4e564e5445583031ull, MAGIC_SYNC = 0x4e564e53594e4331ull,
  MAGIC_WINDOW = 0x4e564e57494e3031ull, MAGIC_BUFFER = 0x4e564e4255463031ull, MAGIC_PROGRAM = 0x4e564e5052473031ull,
  MAGIC_TEXPOOL = 0x4e564e5450303031ull, MAGIC_SAMPPOOL = 0x4e564e5350303031ull, MAGIC_SAMPLER = 0x4e564e534d503031ull,
};
struct ObjHdr { uint64_t magic; void* impl; };

template <class T>
T* Impl(const void* guest, uint64_t magic) {
  auto* h = static_cast<const ObjHdr*>(guest);
  if (!h || h->magic != magic || !h->impl) return nullptr;
  return static_cast<T*>(h->impl);
}
inline void Bind(void* guest, uint64_t magic, void* impl) {
  auto* h = static_cast<ObjHdr*>(guest);
  h->magic = magic;
  h->impl = impl;
}
inline void Unbind(void* guest) {
  auto* h = static_cast<ObjHdr*>(guest);
  h->magic = 0;
  h->impl = nullptr;
}

// ---- builders (live in guest memory, written in place; must fit kBuilderMax) -------------------------------
struct DeviceBuilder { int flags; };
struct QueueBuilder { void* device; int flags; uint64_t cmdSize, computeSize, ctrlSize; };
struct PoolBuilder { void* device; void* storage; uint64_t size; int flags; };
struct WindowBuilder { void* device; void* native; int count; void* const* textures; };
struct TextureBuilder {  // up to 0x80 bytes
  void* device; int flags, target, width, height, depth, levels, format, samples;
  void* pool; int64_t poolOffset; int stride; const void* packaged;
};
struct BufferBuilder { void* device; void* pool; int64_t offset; uint64_t size; };
struct SamplerBuilder {  // up to 0x60 bytes
  void* device; int minFilter, magFilter, wrap[3]; float lodMin, lodMax, lodBias; int compareMode, compareFunc;
  float border[4]; float aniso; int reduction;
};
// Guest NVNtextureView (0x10 bytes), written in place by TextureViewSet*. flags: 1 levels, 2 layers, 4 format, 8 target,
// 0x10 depth/stencil mode, 0x20 swizzle. [TOTK-NVN] object size 0x10.
struct TexView {
  uint8_t flags, target, format, dsMode, swz[4], lvlBase, lvlNum; uint16_t layerBase, layerNum;
};
static_assert(sizeof(TexView) <= 0x10);
static_assert(sizeof(BufferBuilder) <= kBuilderMax && sizeof(SamplerBuilder) <= 0x60);
static_assert(sizeof(DeviceBuilder) <= kBuilderMax && sizeof(QueueBuilder) <= kBuilderMax &&
              sizeof(PoolBuilder) <= kBuilderMax && sizeof(WindowBuilder) <= kBuilderMax);
static_assert(sizeof(TextureBuilder) <= kTextureBuilderMax);

struct Host;
struct TextureImpl;
struct WindowImpl;
struct QueueImpl;
// A window of an image's mip levels and array layers (all counts >= 1 once clamped).
struct SubRange {
  uint32_t lb = 0, ln = 1, yb = 0, yn = 1;
  bool Overlaps(const SubRange& o) const { return lb < o.lb + o.ln && o.lb < lb + ln && yb < o.yb + o.yn && o.yb < yb + yn; }
};

// ---- format table ------------------------------------------------------------------------------------------
// kind: 0 float/norm, 1 uint, 2 sint. bytes = bytes per texel, or per block for block-compressed formats (bw x bh texels).
// astcW/astcH != 0: the guest format is ASTC; when vk is not an ASTC format the texture is CPU-decoded (and transcoded to
// BC7 if vk is BC7) at upload time, so bytes/bw/bh then describe the Vulkan format and the guest block size is astcW x astcH.
struct FormatInfo { VkFormat vk; uint32_t bytes; VkImageAspectFlags aspect; uint8_t kind; uint8_t bw = 1, bh = 1, astcW = 0, astcH = 0; bool srgb = false; };
const FormatInfo* LookupFormat(int nvnFormat);  // nullptr if not mapped (logged by the caller)

// ---- recorded commands (Vulkan-free; replayed at submit) ---------------------------------------------------
enum class Op : uint8_t {
  SetRenderTargets, ClearColor, ClearDepthStencil, SetViewports, SetScissors, Barrier,
  // phase 1 (draw state is recorded by value; replay resolves addresses/handles at submit)
  BindProgram, BindBlend, BindColor, BindChannelMask, BindMultisample, BindPolygon, BindDepthStencil,
  BindVertexAttribs, BindVertexStreams, BindVertexBuffers, BindUbo, BindSsbo, BindTexture, BindImage,
  SetTexturePool, SetSamplerPool, StencilValueMask, StencilMask, StencilRef, PolygonOffset,
  Draw, DrawIndexed, CopyBufferToTexture,
  // phase 3
  SetAlphaRef, SetPatchSize, DrawIndirect, DrawIndexedIndirect, Dispatch, CopyTextureToBuffer, CopyTextureToTexture, ReportCounter,
};
struct TexPoolImpl;
struct SampPoolImpl;
struct ProgramImpl;
struct Cmd {
  Op op;
  int i[6] = {};
  float f[4] = {};
  TextureImpl* tex[9] = {};   // SetRenderTargets: colors[0..7], depth at [8]; CopyBufferToTexture: tex[0]
  uint64_t a[4] = {};         // addresses / packed state words / handles
  void* p[2] = {};            // ProgramImpl*, pool impls
  std::vector<uint8_t> blob;  // arrays (vertex attribs, streams, copy region + view)
};
struct CmdList { std::vector<Cmd> cmds; };

struct CommandBufferImpl {
  Host* host = nullptr;
  bool recording = false;
  CommandBufferMemoryCallback cb = nullptr;
  void* cbData = nullptr;
  std::vector<Cmd> cmds;
  // memory handed to us by the guest (kept for diagnostics; NVN command words are never written)
  uint64_t commandBytes = 0, controlBytes = 0;
};

struct PoolImpl {
  Host* host = nullptr;
  void* storage = nullptr;  // guest pointer == host pointer
  void* guest = nullptr;    // the guest NVNmemoryPool object (returned by BufferGetMemoryPool)
  uint64_t size = 0;
  uint64_t va = 0;          // synthetic GPU virtual address of the pool start (see Host::RegisterPool)
  int flags = 0;
  // Lazy Vulkan mirror (see EnsureBacking)
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  // Byte ranges the GPU writes (storage-buffer outputs, e.g. skinning compute). SyncPoolRange skips them: the CPU copy is stale there.
  std::mutex gpuMutex;
  std::vector<std::pair<uint64_t, uint64_t>> gpuRanges;   // [begin, end)
  // Ranges already copied into the mirror during the current SubmitLists (SyncPoolRangeOnce); valid while syncEpoch == g_replayEpoch.
  uint64_t syncEpoch = 0;
  std::vector<std::pair<uint64_t, uint64_t>> synced;
  // Fragment-shader SSBO writes into this CPU pool that are submitted but not yet copied back to guest memory (Readback kind 3).
  std::atomic<int> wbPending{0};
  // SNAP_POOL_LOG=1: bytes of live buffers and textures placed in this pool (sub-allocation arenas), for the B1 null-pool triage
  std::atomic<int64_t> subLive{0}, subPeak{0};
  std::atomic<int> subCount{0};
  // [perf] dirty-page sync (SyncPoolRangeOnce): 0 = not set up, 1 = GetWriteWatch tracks the storage, -1 = unusable (full copies).
  int8_t ww = 0;
  std::atomic<bool> wbPool{false};   // has fragment-shader write-backs (photo evaluation): always full copies, as before
  bool Cpu() const { return !(flags & MP_CPU_NO_ACCESS); }
};
bool EnsureBacking(PoolImpl* p);
bool PoolAcctOn();                                   // SNAP_POOL_LOG=1 (nvn_gfx.cpp)
void PoolAcct(PoolImpl* p, uint64_t va, int64_t bytes);   // +bytes when a buffer/texture is placed in p (va = p->va), -bytes at finalize; ignored if p is gone
void PoolAcctDump(const char* why);                  // every pool of 1 MiB or more: size, live, peak, count
// SNAP_ARENA_LOG=1 (nvn_gfx.cpp): the game's GPU arenas (allocator main+0x911fc0): which arena a buffer/texture sits in, per guest
// call-site owner. ArenaAcct returns an owner slot (-1 = not in an arena / off); ArenaUnacct(slot, bytes) at finalize.
bool ArenaAcctOn();
int ArenaAcct(const PoolImpl* p, int64_t off, int64_t bytes, int kind);   // kind 0 = buffer, 1 = texture
void ArenaUnacct(int slot, int64_t bytes, int born = -1);   // born = present count at creation (lifetime stats)
void ArenaDump(const char* why);
void ArenaPoolCheck();
void OwnPoolRetire(PoolImpl* p);        // fallback buffer pool finalized: destroy it once the GPU is past it (nvn_gfx.cpp)
void OwnPoolReap(struct QueueImpl* q);  // called at present under queueMutex: stamps / destroys retired fallback pools
// Copy [off, off+size) of the guest storage into the Vulkan mirror (call before the GPU reads that range).
void SyncPoolRange(PoolImpl* p, uint64_t off, uint64_t size);
// Replay-time variant: copies a range at most once per SubmitLists (the guest is blocked inside the submit, so the CPU bytes cannot change
// between two draws of the same list). Draws that bind the same big buffer range repeatedly used to copy it every time.
void SyncPoolRangeOnce(PoolImpl* p, uint64_t off, uint64_t size);
extern uint64_t g_replayEpoch;
extern std::atomic<unsigned long> g_replayTid;   // OS thread id inside SubmitLists (0 = none); SNAP_RACE_LOG diagnostics
void MarkGpuWritten(PoolImpl* p, uint64_t off, uint64_t size);   // the GPU writes this range from now on

// ownPool: the guest passed no memory pool (its GPU arena was full, crash B1) and the host gave the buffer private storage
struct BufferImpl { PoolImpl* pool = nullptr; int64_t offset = 0; uint64_t size = 0; uint64_t acctVa = 0; bool ownPool = false; int arenaSlot = -1; int arenaBorn = 0; };
struct SamplerImpl { Host* host = nullptr; VkSampler sampler = VK_NULL_HANDLE; SamplerBuilder b{}; };
// seq is a sequence lock: odd while TexturePoolRegisterTexture rewrites the slot, so ResolveHandle (replay thread) always copies a
// matching {tex, view} pair (re-registration of a live id during replay used to be able to pair the old texture with the new view).
struct TexSlot { std::atomic<uint32_t> seq{0}; TextureImpl* tex = nullptr; TexView view{}; bool hasView = false; };
// Descriptor table indexed by pool id (20-bit texture / 12-bit sampler handle fields). Loader threads register descriptors while the
// render thread replays draws that read them (on the GPU these are independent descriptor words). An unordered_map here crashed
// about 1.5% of runs: an insert rehashed the buckets under a concurrent find in ResolveHandle (host read of 0x10 inside
// QueueSubmitCommands, guest main+0x904304). Chunks are allocated once and never move, so Find needs no lock.
template <class T> struct SlotTable {
  static constexpr unsigned kChunkBits = 10, kChunk = 1u << kChunkBits, kIds = 1u << 20, kChunks = kIds >> kChunkBits;
  std::atomic<T*> chunks[kChunks] = {};
  SlotTable() = default;
  SlotTable(const SlotTable&) = delete;
  SlotTable& operator=(const SlotTable&) = delete;
  ~SlotTable() { for (auto& c : chunks) delete[] c.load(std::memory_order_relaxed); }
  // nullptr when the id is out of range or its chunk was never written (= never registered)
  T* Find(int id) const {
    if ((unsigned)id >= kIds) return nullptr;
    T* c = chunks[(unsigned)id >> kChunkBits].load(std::memory_order_acquire);
    return c ? &c[(unsigned)id & (kChunk - 1)] : nullptr;
  }
  T* At(int id) {   // the slot for a write (allocates its chunk); nullptr when out of range
    if ((unsigned)id >= kIds) return nullptr;
    std::atomic<T*>& a = chunks[(unsigned)id >> kChunkBits];
    T* c = a.load(std::memory_order_acquire);
    if (!c) {
      T* fresh = new T[kChunk]();
      if (a.compare_exchange_strong(c, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) c = fresh;
      else delete[] fresh;   // another thread installed it first; c now holds that chunk
    }
    return &c[(unsigned)id & (kChunk - 1)];
  }
};
struct TexPoolImpl { Host* host = nullptr; SlotTable<TexSlot> slots; };
struct SampPoolImpl { Host* host = nullptr; SlotTable<SamplerImpl*> slots; };

struct TextureImpl {
  PoolImpl* acctPool = nullptr; uint64_t acctVa = 0; int64_t acctBytes = 0;   // SNAP_POOL_LOG accounting (PoolAcct)
  int arenaSlot = -1; int64_t arenaBytes = 0;                                  // SNAP_ARENA_LOG accounting (ArenaAcct)
  int lastDrawPresent = -10; int drawStreak = 0;   // GetPipeline: consecutive presented frames this texture was drawn to (a pending pipeline may only be skipped for per-frame targets)
  Host* host = nullptr;
  TextureBuilder b{};
  const FormatInfo* fi = nullptr;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t hostW = 0, hostH = 0;   // image size when render-target scaling applies (0 = same as the guest size)
  float scale = 1.0f;
  uint32_t HW() const { return hostW ? hostW : (uint32_t)b.width; }
  uint32_t HH() const { return hostH ? hostH : (uint32_t)b.height; }
  TextureImpl* abAlt = nullptr; bool abTried = false;   // SNAP_MODS_AB test aid: the original-texture twin of a replaced texture (see ResolveHandle)
  bool uploaded = false;   // contents defined (static data uploaded, copied to, or rendered to)
  uint64_t srcHash = 0, hashSeq = 0;   // CPU-written (linear) pool textures: hash of the guest bytes at the last upload, replay that checked it
  std::unordered_map<std::string, VkImageView> views;   // see TextureImpl::View
  std::unordered_map<VkImageView, VkImageViewType> viewTypes;   // the type each cached view was created with (tests, diagnostics)
  VkImageViewType ViewTypeOf(VkImageView v) const { auto it = viewTypes.find(v); return it == viewTypes.end() ? VK_IMAGE_VIEW_TYPE_MAX_ENUM : it->second; }
  // Per-subresource layouts. `layout` is the layout of every subresource while subLayout is empty; after a ranged transition
  // subLayout holds one entry per (layer * imgLevels + level) and `layout` is meaningless until it collapses again.
  std::vector<VkImageLayout> subLayout;
  uint32_t imgLevels = 1, imgLayers = 1;   // of the VkImage (set by EnsureImage)
  VkImageCreateFlags ci_flags = 0;         // flags the VkImage was created with
  TextureImpl* bcShadow = nullptr;
  TextureImpl* xferShadow = nullptr;   // scaled texture: guest-size stand-in for guest-resolution copies (nvn_xfer.cpp), freed with it
  uint32_t bcSubLevel = 0, bcSubLayer = 0;  // on a bcShadow: the level / layer of the BC texture it stands in for          // block-compressed render target: the uncompressed stand-in drawn into (RedirectCompressedTargets)
  bool EnsureImage();
  uint32_t LevelW(uint32_t l) const { return std::max(1u, HW() >> l); }
  uint32_t LevelH(uint32_t l) const { return std::max(1u, HH() >> l); }
  // Subresource range (level/layer window) a TexView selects, clamped to the image.
  SubRange RangeOf(const TexView* v) const;
  // Image view for a TexView (nullptr = the whole texture); sampled = true gives a view usable in a descriptor
  // (single aspect: depth for depth/stencil formats unless the view asks for stencil). shaderType is the translated
  // shader's SamplerType (0 = use the texture's own target): the view type follows the shader's declared dimension and
  // arrayness. `range` overrides the level/layer window of the TexView; nullptr means v's own window. Returns
  // VK_NULL_HANDLE when the texture cannot be viewed as the shader's type (the caller binds a dummy of that type).
  VkImageView View(const TexView* v, bool sampled, int shaderType = 0, const SubRange* range = nullptr);
  // Attachment view: one level, `layers` layers from the view's base, the texture's own format.
  VkImageView RenderView(const TexView* v, SubRange* out = nullptr);
};

// ---- programs (nvn_shader.cpp) ---------------------------------------------------------------------------------
// Manifest of one translated stage (see scripts/shader_harness/Program.cs, --nvn mode).
struct MCb { int slot, set, binding; };
struct MSb { int slot, set, binding, sbCbSlot, sbCbOffset, write; };
struct MTex { int set, binding, handleIndex, cbufSlot, type, array, separate; };
struct StageProg {
  bool valid = false;
  char key[16] = {};                   // first 12 hex digits of the translation cache key (draw log)
  char fkey[72] = {};                  // the full translation cache key (nvn_pipeline.cpp reloads the stage from the cache by it)
  int stage = 0;                       // NVN stage: 0 VS 1 FS 2 GS 3 TCS 4 TES 5 CS
  VkShaderModule mod = VK_NULL_HANDLE;
  std::vector<MCb> cb;
  std::vector<MSb> sb;
  std::vector<MTex> tex, img;
  int fragOutputMap = 0;
  std::vector<uint32_t> spv;                      // kept for the fragment stage only (alpha-test patching)
  // fragment module variants (alpha test, output rewrite): key = alphaFunc | nColor << 4 | dual << 8 -> {module, has alpha spec constant}
  std::map<uint32_t, std::pair<VkShaderModule, bool>> fragMods;
  uint32_t inputMask = 0xFFFFFFFFu;    // vertex stage: input locations the shader reads
  std::vector<int> outLocs;            // fragment stage: color output locations, ascending
  VkBuffer constBuf = VK_NULL_HANDLE;  // c[1]: the program's constant-data blob (data section tail)
  VkDeviceMemory constMem = VK_NULL_HANDLE;
  uint64_t constSize = 0;
};
struct ShaderData { uint64_t data; const void* control; };   // [TOTK-NVN] NVNshaderData: {address, control*}
struct ProgramImpl {
  Host* host = nullptr;
  uint64_t id = 0;
  StageProg st[6];
  bool composite = false;   // stage copies of several separable programs: owns only the layout, never the stage objects
  bool setUsed[4] = {};
  VkDescriptorSetLayout setLayout[4] = {};
  VkPipelineLayout layout = VK_NULL_HANDLE;
};
bool PatchAlphaTest(const std::vector<uint32_t>& in, int func, std::vector<uint32_t>& out);   // nvn_alpha.cpp
bool SpvSetLocalSize(std::vector<uint32_t>& spv, uint32_t x, uint32_t y, uint32_t z);   // compute workgroup size from the NVN control section
bool SpvAddEarlyFragmentTests(std::vector<uint32_t>& spv);              // nvn_alpha.cpp: layout(early_fragment_tests) from control+0x718 bit 0
bool SpvAddStorageImageCaps(std::vector<uint32_t>& spv);                  // nvn_alpha.cpp: SPIR-V fixes for the translated modules
uint32_t SpvVertexInputMask(const std::vector<uint32_t>& in);
std::vector<int> SpvFragmentOutputs(const std::vector<uint32_t>& in);
bool PatchFragmentOutputs(const std::vector<uint32_t>& in, int nColor, bool dual, std::vector<uint32_t>& out);
bool ProgramSetShadersImpl(ProgramImpl* p, int n, const ShaderData* sd);
bool ProgramBuildLayout(ProgramImpl* p);
// BindProgram(prog, stagesMask) is per stage (the game binds a vertex-only and a fragment-only program). Returns the program whose stages
// are src[s]->st[s] for every s with a valid stage: the program itself when one program supplies every one of its stages, else a cached composite.
ProgramImpl* ProgramResolve(Host* h, ProgramImpl* const src[6]);
void ProgramDestroy(ProgramImpl* p);

// ---- replay context (shared by nvn_vk.cpp and nvn_draw.cpp) -----------------------------------------------------
struct DrawState;
struct ScratchBuf {
  VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
  uint64_t cap = 0; uint32_t usage = 0; uint8_t* map = nullptr;   // [perf] recyclable when cap != 0 (NewScratch / ReleaseScratch)
};
// GPU -> guest memory transfer finished after a timeline value completes (CopyTextureToBuffer, ReportCounter).
struct Readback {
  VkSemaphore sem = VK_NULL_HANDLE; uint64_t value = 0;
  ScratchBuf sb; uint8_t* map = nullptr;
  uint8_t* dst = nullptr; uint64_t size = 0;
  int kind = 0;           // 0 raw copy, 1 counter report: map = [timestamp ticks][n occlusion results], dst = {counter, timestamp ns}
                          // 3 shader write-back: map = the pool's mapped mirror (no scratch), expect = guest bytes when the GPU took them
  int counterType = 0; uint32_t nOcc = 0;
  struct PoolImpl* pool = nullptr; uint64_t poolOff = 0;   // kind 3
  std::vector<uint8_t> expect;                            // kind 3: the CPU changed the range since the submit if the guest bytes differ
  std::string dumpPath;   // kind 2: debug dump of a texture to this file (NVN_RT_DUMP)
};
struct ReplayCtx {
  Host* h = nullptr;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  QueueImpl* q = nullptr;
  std::vector<VkDescriptorPool>* pools = nullptr;   // descriptor pools used by this command buffer (recycled on reap)
  std::vector<Readback>* readbacks = nullptr;      // moved to Host::readbacks with the submit's timeline value
  bool occOpen = false;                            // an occlusion query is open in the current rendering scope
  std::vector<ScratchBuf>* scratch = nullptr;       // staging/scratch buffers freed when this command buffer retires
  TextureImpl* rt[9] = {};
  TexView rtView[9] = {};          // the NVNtextureView given to SetRenderTargets (level / layer selection)
  bool rtHasView[9] = {};
  TextureImpl* bcDst[8] = {};      // rt[k] is the bcShadow of this block-compressed texture; copied into it when a scope ends
  bool bcDirty = false;
  bool rendering = false;
  bool vpSet = false, scSet = false;
  // guest (unscaled) viewport / scissor 0..3 as last set, re-applied when SetRenderTargets changes the render scale (the game
  // often sets them before binding targets, so a value scaled for the previous target would land in a corner of a scaled one)
  float vpGuest[4][4] = {}; int scGuest[4][4] = {}; uint8_t vpHave = 0, scHave = 0; float vpScale = 1.0f, scScale = 1.0f;
  DrawState* ds = nullptr;
};

struct Entry { const char* name; GenericProc fn; };
const Entry* GfxEntries(int* count);               // nvn_gfx.cpp

struct SyncImpl {
  Host* host = nullptr;
  VkSemaphore sem = VK_NULL_HANDLE;  // timeline semaphore of the queue that signaled it
  uint64_t value = 0;
  bool armed = false;
};

struct Retired { uint64_t value; VkCommandBuffer cb; CmdList* list; std::vector<VkDescriptorPool> pools; std::vector<ScratchBuf> scratch; };

struct QueueImpl {
  Host* host = nullptr;
  DrawState* persistDs = nullptr;   // draw state (bound programs, buffers, textures) carries over between submits, as on the GPU channel
  int flags = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkSemaphore timeline = VK_NULL_HANDLE;
  uint64_t value = 0;                  // last value submitted to signal
  std::vector<Retired> inflight;
  std::vector<VkCommandBuffer> freeCbs;
  std::vector<VkDescriptorPool> freePools;   // reset, ready for reuse
  VkCommandBuffer AcquireCb();
  void Reap(bool wait_all);
};

struct WindowImpl {
  Host* host = nullptr;
  void* hwnd = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat scFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D scExtent{};
  std::vector<VkImage> scImages;
  std::vector<VkSemaphore> renderDone;   // per swapchain image
  std::vector<VkSemaphore> acquireSems;  // ring (images+1); reuse is gated on the timeline value below
  std::vector<uint64_t> acquireValue;
  uint32_t acquireNext = 0;
  std::vector<TextureImpl*> tex;         // NVN-visible textures (slots)
  std::vector<uint64_t> slotValue;       // timeline value of the last present that read each slot
  std::vector<VkSemaphore> slotSem;
  int nextSlot = 0;
  int presentInterval = 1;
  int crop[4] = {0, 0, 0, 0};
  int64_t nextPresentNs = 0;            // frame limiter schedule (steady clock, ns)
  bool needRecreate = true;
  bool hdr = false;                     // swapchain is A2B10G10R10 + HDR10_ST2084 (nvn_hdr.cpp draws the present pass)
  bool Recreate();
};

struct Host {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceFeatures features{};   // the features that were enabled (all the GPU supports)
  VkPhysicalDeviceMemoryProperties memProps{};
  VkPipelineCache pipelineCache = VK_NULL_HANDLE;
  std::mutex queueMutex;               // vkQueueSubmit / vkQueuePresentKHR need external sync
  bool validation = false;
  int originMode = ORIGIN_LOWER_LEFT;
  int depthMode = DEPTH_NEAR_IS_MINUS_W;
  float fastClearDepth = 1.0f;
  std::vector<WindowImpl*> windows;
  // Synthetic GPU address space: every memory pool gets a range [va, va+size); NVN buffer addresses are
  // poolVa + offset, and replay maps an address back to (pool, offset) with ResolveAddress.
  std::mutex vaMutex;
  std::map<uint64_t, PoolImpl*> vaMap;
  uint64_t nextVa = 0x100000000ull;
  void RegisterPool(PoolImpl* p);
  void UnregisterPool(PoolImpl* p);
  bool ResolveAddress(uint64_t addr, PoolImpl** pool, uint64_t* off);
  std::string cacheDir;                       // shader SPIR-V cache + VkPipelineCache file
  std::mutex pipeMutex;
  std::unordered_map<std::string, VkPipeline> pipelines;
  uint64_t nextProgramId = 1;
  std::mutex compMutex;                       // composite programs (ProgramResolve), keyed by the source program id per stage
  std::map<std::array<uint64_t, 6>, ProgramImpl*> composites;
  VkPipelineLayout clearLayout = VK_NULL_HANDLE;   // masked-clear pipeline layout
  std::vector<VkShaderModule> variantMods;    // patched fragment modules (alpha test / output rewrite); freed at shutdown, guarded by pipeMutex
  struct Dummies* dummies = nullptr;          // fallback resources for unbound descriptors (nvn_draw.cpp)
  // queries (nvn_xfer.cpp): timestamps and samples-passed counters
  VkQueryPool tsPool = VK_NULL_HANDLE, occPool = VK_NULL_HANDLE;
  uint32_t tsNext = 0, occNext = 0, occReported = 0;
  bool hostQueryReset = false;                // feature enabled (core 1.2); without it the query pools are never created
  bool wantSamples = false;                   // a SAMPLES_PASSED report has been recorded: wrap rendering scopes in queries
  uint64_t samplesTotal = 0;                  // running samples-passed counter (CPU side)
  std::mutex rbMutex;
  std::vector<Readback> readbacks;
  // host policy (nvn_host.h): render-target scale, anisotropy override
  float renderScale = 1.0f;
  float anisoOverride = 0.0f;

  bool Init(bool wantValidation);
  void Shutdown();
  uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;
};

extern Host* g_host;
extern std::atomic<uint32_t> g_valErrors, g_valWarnings;
extern NvnHostHooks g_hooks;
extern int g_validationRequest;  // -1 unset
// Render-target resolution policy (nvn_entry.cpp). Consulted once per TextureInitialize; off (scale 1) by default.
struct ScalePolicy {
  struct Exempt { int w, h, fmt; bool depthOnly; };   // w/h/fmt == -1 match anything
  float scale = 1.0f;
  std::vector<Exempt> exempt;
  std::vector<uint32_t> exemptTags;
  uint32_t nextTag = 0;                               // consumed by the next TextureInitialize
  int shadowRes = 0;                                  // 0 = original; else side of the 1024x1024 depth shadow maps
  bool scaleDisplay = true;                           // window (DISPLAY) textures follow the scale too, else the output stays 1080p
  float displayScale = 0.0f;                          // [upscale] > 0: window textures take this scale (the upscaler's output), not `scale`
  ScalePolicy();
  float For(const TextureBuilder& b, uint32_t tag) const;   // 1.0 when the texture is not scaled
};
extern ScalePolicy g_scale;
extern float g_lodBiasExtra;    // SNAP_LOD_BIAS: added to the game's bias on mipmapped, non-compare samplers
extern bool g_lodBiasAuto;      // SNAP_LOD_BIAS=auto: -log2(render scale)
extern void ApplyHostEnvPolicy();   // SNAP_ANISO, SNAP_LOD_BIAS, SNAP_RENDER_SCALE, SNAP_SHADOW_RES (called from DeviceInitialize)
extern float g_anisoOverride;   // 0 = off; else forced max anisotropy for mipmapped, non-compare samplers
extern double g_paceHz;          // rate of present interval 1; 0 = display-paced only

// ---- Vulkan-side operations used by the entry points ------------------------------------------------------
void SubmitLists(QueueImpl* q, int n, CmdList* const* lists);
void QueueFence(QueueImpl* q, SyncImpl* s);
void QueueFinishImpl(QueueImpl* q);
bool PresentWindow(QueueImpl* q, WindowImpl* w, int slot);
bool InitQueue(QueueImpl* q);
void DestroyQueue(QueueImpl* q);
void DestroyTexture(TextureImpl* t);
void DestroyHostExtras(Host* h);   // nvn_draw.cpp: pipelines, dummies, saves <cacheDir>/pipeline_cache.bin
void LoadPipelineCache(Host* h);   // nvn_draw.cpp: reads <cacheDir>/pipeline_cache.bin into h->pipelineCache
void ReplayBegin(ReplayCtx& c);    // nvn_draw.cpp: allocate draw state, one-time dummy resources
void ReplayEnd(ReplayCtx& c);      // nvn_draw.cpp: close rendering, free draw state
void EndRendering(ReplayCtx& c);   // nvn_draw.cpp: vkCmdEndRendering + barrier when a rendering scope is open
void RedirectCompressedTargets(ReplayCtx& c);   // nvn_draw.cpp: after SetRenderTargets, swap BC color targets for uncompressed stand-ins
void FlushCompressedTargets(ReplayCtx& c);      // nvn_draw.cpp: copy the stand-ins' blocks into the BC textures
void ReplayDrawOp(ReplayCtx& c, const Cmd& cmd);   // nvn_draw.cpp: every op except the legacy six
void Transition(VkCommandBuffer cb, TextureImpl* t, VkImageLayout to);   // whole image
void TransitionRange(VkCommandBuffer cb, TextureImpl* t, VkImageLayout to, const SubRange& r);   // only the given subresources
bool InLayout(const TextureImpl* t, VkImageLayout want, const SubRange& r);   // every subresource of r already in `want`
// Subresource a render target view selects (level, first layer, layer count) -- attachments are one level.
SubRange RtRange(const TextureImpl* t, const TexView* v);   // nvn_vk.cpp
// Host-visible TRANSFER_SRC buffer owned by the replaying command buffer (freed when it retires). nvn_vk.cpp
uint8_t* NewScratch(ReplayCtx& cx, uint64_t size, VkBuffer* buf, VkBufferUsageFlags extra = 0);
void ReleaseScratch(Host* h, const ScratchBuf& sb);   // [perf] retire a scratch buffer: recycled by size class or destroyed
// Texture storage layout of a builder (Maxwell block-linear sizes), nvn_tex_vk.cpp
uint64_t TextureStorageSize(const TextureBuilder& b);
uint32_t TextureLayerCount(const TextureBuilder& b);   // nvn_vk.cpp: array layers of the VkImage (cube = 6)
// Uploads packaged/pool-resident static texture data (deswizzle, ASTC fallback) into the image; leaves it in
// TRANSFER_DST. No-op when already defined. nvn_tex_vk.cpp
void UploadStaticTexture(ReplayCtx& cx, TextureImpl* t);
// [perf] upload prefetch (nvn_tex_vk.cpp, SNAP_UPLOAD_PREFETCH_MB=0 = off): deswizzle at TextureInitialize on worker threads
void TexPrefetch(TextureImpl* t);
void TexPrefetchForget(const TextureImpl* t);   // DestroyTexture
void TexPrefetchPoolGone(const void* pool);     // DestroyPool
// [perf] VkImage + memory for a builder at host size hw x hh (nvn_vk.cpp; TextureImpl::EnsureImage and the prefetch workers)
struct TexImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; uint32_t levels = 1, layers = 1; VkImageCreateFlags flags = 0; };
bool CreateTexImage(Host* host, const TextureBuilder& b, const FormatInfo* fi, uint32_t hw, uint32_t hh, TexImage& out);
bool TexPrefetchTakeImage(TextureImpl* t);   // nvn_tex_vk.cpp: EnsureImage adopts a prefetch worker's image (SNAP_UPLOAD_PREFETCH_IMAGE=1 = on)
void TexPrefetchShutdown();                  // Host::Shutdown: drop staged jobs (and their images) before the device goes
void ScratchPoolDrain(Host* h);              // Host::Shutdown: destroy the recycled scratch buffers (nvn_vk.cpp)
// NVN window-origin -> Vulkan viewport. NVN follows OpenGL clip control: NDC +y is up in both origin modes and window row
// 0 is memory row 0 (the viewport and scissor y are rows from the window origin). Vulkan's NDC +y points down, so
// UPPER_LEFT needs a negative-height viewport: NDC +1 lands on the first row of the viewport rectangle, which is the
// image top. LOWER_LEFT keeps the rectangle's rows as given (row 0 is the image bottom by convention, never presented
// flipped). Scissors are plain window rectangles in both modes. `scale` is the render-target scale (1 = original).
inline VkViewport MakeNvnViewport(int originMode, float x, float y, float w, float h, float scale = 1.0f) {
  x *= scale; y *= scale; w *= scale; h *= scale;
  if (originMode == ORIGIN_UPPER_LEFT) return VkViewport{x, y + h, w, -h, 0.0f, 1.0f};
  return VkViewport{x, y, w, h, 0.0f, 1.0f};
}
// Debug aids (env NVN_TEX_DUMP=<dir>, NVN_DRAW_LOG=<file> [NVN_DRAW_LOG_FRAMES=a-b]); no-ops when unset.
void DebugDumpTexture(const TextureImpl* t, const char* tag, const void* data, uint64_t bytes, uint32_t w, uint32_t h);
bool DebugDrawLogActive();
void DebugDrawLog(const char* fmt, ...);
extern int g_presentCount;
extern uint64_t g_replaySeq;           // incremented once per replayed command list
void RefreshDynamicTexture(ReplayCtx& cx, TextureImpl* t);   // re-uploads a CPU-written pool texture whose guest bytes changed
// nvn_xfer.cpp: replay of the GPU->guest transfer commands and the readback queue
void DoCopyTextureToBuffer(ReplayCtx& cx, const struct Cmd& c);
void DebugDumpSampledTexture(ReplayCtx& cx, TextureImpl* t, const char* tag);   // nvn_xfer.cpp: NVN_RT_DUMP=<dir>
void DoCopyTextureToTexture(ReplayCtx& cx, const struct Cmd& c);
// nvn_xfer.cpp, render scale: guest copy regions are in guest texels, a scaled texture's image is HW() x HH(). A buffer->image copy
// into a scaled texture goes through an unscaled stand-in and a blit (returns true when it did that; NVN_NO_XFER_SCALE=1 turns the
// scaled paths and the clamp off). XferFitCopy logs a region that runs past the image (undefined behavior: it lost the device below
// render scale 1) and clamps it; false = nothing left to copy.
bool XferUploadScaled(ReplayCtx& cx, TextureImpl* t, VkBuffer src, const VkBufferImageCopy& guestCopy);
bool XferFitCopy(const TextureImpl* t, VkBufferImageCopy& bc, const char* what);
void DoReportCounter(ReplayCtx& cx, const struct Cmd& c);
void DoMaskedClear(ReplayCtx& cx, TextureImpl* t, const TexView* tv, int mask, const float* color);   // nvn_draw.cpp
void QueueShaderWriteback(ReplayCtx& cx, PoolImpl* pool, uint64_t off, uint64_t size);   // nvn_draw.cpp: fragment SSBO copy-back (Readback kind 3)
void FlushReadbacks(Host* h, bool wait);   // applies completed readbacks to guest memory (in submission order)
void DestroyQueryPools(Host* h);
VkDescriptorPool AcquireDescriptorPool(QueueImpl* q);                    // nvn_draw.cpp
void DestroyPool(PoolImpl* p);
void DestroyWindow(WindowImpl* w);
SyncWaitResult WaitSync(const SyncImpl* s, uint64_t timeoutNs);
// Host-visible, coherent buffer (mapped persistently); used for constant data, dummy UBOs and the support buffer.
bool CreateHostBuffer(Host* h, uint64_t size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** mapped);
extern int g_fdBurst;   // NVN_FRAME_DUMP burst counter (set by the photo-thumbnail debug log)

}  // namespace nvn
