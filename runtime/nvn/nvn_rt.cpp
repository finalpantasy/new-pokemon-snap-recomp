// SPDX-License-Identifier: MIT
// Ray tracing track: device setup (M0), transform-feedback geometry capture (M1), BLAS/TLAS build and ray-query debug overlay (M2).
// Overview, switches and the design notes: nvn_rt.h and docs/lighting-rt-plan.md section 4. Everything is inert without SNAP_RT=1.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "nvn_internal.h"
#include "nvn_fx.h"   // photo-frame guard, scene depth, the post-tonemap slot
#include "nvn_rt.h"
#include "nvn_rt_spv.h"

using namespace nvn;

namespace snap_rt {
namespace {

bool EnvOn(const char* n, bool def = false) { const char* e = getenv(n); return e && *e ? e[0] != '0' : def; }
int EnvInt(const char* n, int def) { const char* e = getenv(n); return e && *e ? atoi(e) : def; }
double Now() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }

bool g_on = false, g_capture = false, g_failed = false;
VkPhysicalDeviceAccelerationStructureFeaturesKHR g_fAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
VkPhysicalDeviceRayQueryFeaturesKHR g_fRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
VkPhysicalDeviceTransformFeedbackFeaturesEXT g_fTf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
uint32_t g_scratchAlign = 128;
uint64_t g_maxPrims = 0; uint32_t g_maxInstances = 0;

PFN_vkCreateAccelerationStructureKHR pCreateAs;
PFN_vkDestroyAccelerationStructureKHR pDestroyAs;
PFN_vkGetAccelerationStructureBuildSizesKHR pBuildSizes;
PFN_vkCmdBuildAccelerationStructuresKHR pCmdBuild;
PFN_vkGetAccelerationStructureDeviceAddressKHR pAsAddr;
PFN_vkCmdBindTransformFeedbackBuffersEXT pBindXfb;
PFN_vkCmdBeginTransformFeedbackEXT pBeginXfb;
PFN_vkCmdEndTransformFeedbackEXT pEndXfb;

// ---- SPIR-V ----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16, kOpCapability = 17, kOpMemberDecorate = 72;
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------
struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; uint64_t size = 0; void* map = nullptr; };

bool MakeBuf(Host* h, uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buf* o, bool addr) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = usage | (addr ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
  if (vkCreateBuffer(h->device, &bi, nullptr, &o->buf) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(h->device, o->buf, &mr);
  VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, props);
  if (addr) ai.pNext = &fl;
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(h->device, &ai, nullptr, &o->mem) != VK_SUCCESS) { vkDestroyBuffer(h->device, o->buf, nullptr); *o = Buf{}; return false; }
  vkBindBufferMemory(h->device, o->buf, o->mem, 0);
  o->size = size;
  if (addr) { VkBufferDeviceAddressInfo ba{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; ba.buffer = o->buf; o->addr = vkGetBufferDeviceAddress(h->device, &ba); }
  if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) vkMapMemory(h->device, o->mem, 0, VK_WHOLE_SIZE, 0, &o->map);
  return true;
}
void FreeBuf(Host* h, Buf& b) {
  if (b.buf) vkDestroyBuffer(h->device, b.buf, nullptr);
  if (b.mem) vkFreeMemory(h->device, b.mem, nullptr);
  b = Buf{};
}

// First-fit allocator over one big buffer (offsets and sizes in 256 byte units).
struct Ranges {
  std::map<uint64_t, uint64_t> freeMap; uint64_t cap = 0, used = 0, hwm = 0;
  void Init(uint64_t c) { cap = c; used = hwm = 0; freeMap.clear(); freeMap[0] = c; }
  static uint64_t Round(uint64_t n) { return (n + 255) & ~255ull; }
  bool Alloc(uint64_t n, uint64_t* off) {
    n = Round(n);
    for (auto it = freeMap.begin(); it != freeMap.end(); ++it) {
      if (it->second < n) continue;
      *off = it->first;
      const uint64_t rest = it->second - n, at = it->first + n;
      freeMap.erase(it);
      if (rest) freeMap[at] = rest;
      used += n; hwm = std::max(hwm, used);
      return true;
    }
    return false;
  }
  void Free(uint64_t off, uint64_t n) {
    n = Round(n); used -= std::min(used, n);
    auto it = freeMap.emplace(off, n).first;
    auto nx = std::next(it);
    if (nx != freeMap.end() && it->first + it->second == nx->first) { it->second += nx->second; freeMap.erase(nx); }
    if (it != freeMap.begin()) { auto pv = std::prev(it); if (pv->first + pv->second == it->first) { pv->second += it->second; freeMap.erase(it); } }
  }
};

struct Mat { double m[4][4]; };   // row-major, clip = M * p
bool Invert(const Mat& a, Mat& r) {
  double m[4][8];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int i = c + 1; i < 4; i++) if (std::fabs(m[i][c]) > std::fabs(m[p][c])) p = i;
    if (std::fabs(m[p][c]) < 1e-30) return false;
    if (p != c) for (int j = 0; j < 8; j++) std::swap(m[p][j], m[c][j]);
    const double inv = 1.0 / m[c][c];
    for (int j = 0; j < 8; j++) m[c][j] *= inv;
    for (int i = 0; i < 4; i++) if (i != c) { const double f = m[i][c]; if (f != 0) for (int j = 0; j < 8; j++) m[i][j] -= f * m[c][j]; }
  }
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = m[i][j + 4];
  return true;
}

// ---- state -----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxInstances = 8192;
constexpr uint32_t kSlots = 8;   // stats / timestamp / descriptor ring (frames in flight stay below this)

struct Entry {
  uint32_t vcount = 0;
  uint64_t arenaOff = 0, arenaBytes = 0;
  VkAccelerationStructureKHR as = VK_NULL_HANDLE; uint64_t blasOff = 0, blasBytes = 0, scratchNeed = 0; VkDeviceAddress blasAddr = 0;
  int64_t lastSeen = 0; uint32_t seen = 0; bool built = false;
};
struct FrameDraw { uint64_t key; uint32_t vcount; uint64_t arenaOff; float inv[16]; float eye[3]; };
struct Dead { VkAccelerationStructureKHR as; uint64_t frame; };

struct Stats {   // per-window aggregates for the log line
  uint32_t frames = 0; uint64_t draws = 0, tris = 0, xfbBytes = 0, blasBuilt = 0, blasTris = 0, instances = 0, skipped = 0;
  double cpuCapMs = 0, cpuBuildMs = 0, gpuBuildMs = 0, gpuBlasMs = 0, gpuOvlMs = 0; uint32_t gpuN = 0, ovlN = 0;
  uint64_t cls[8] = {}; uint32_t clsN = 0;
};

struct G {
  std::mutex mu;
  bool ready = false;
  Host* h = nullptr;
  Buf arena, blasStore, scratch, instStore, tlasStore, stats;
  Ranges arenaR, blasR;
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE; VkDeviceAddress tlasAddr = 0; uint64_t tlasScratch = 0;
  VkQueryPool qp = VK_NULL_HANDLE; float tsPeriod = 1.0f;
  VkPipelineLayout convPl = VK_NULL_HANDLE; VkPipeline convPipe = VK_NULL_HANDLE; VkShaderModule convMod = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout ovlPl = VK_NULL_HANDLE; VkShaderModule ovlVs = VK_NULL_HANDLE, ovlFs = VK_NULL_HANDLE;
  VkPipeline ovlPipe = VK_NULL_HANDLE; VkFormat ovlFmt = VK_FORMAT_UNDEFINED;
  VkDescriptorPool dpool = VK_NULL_HANDLE; VkDescriptorSet sets[kSlots] = {}; VkSampler samp = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, Entry> entries;
  std::vector<FrameDraw> frame; int framePresent = -1;
  std::vector<Dead> dead;
  TextureImpl* sceneDepth = nullptr;   // dereferenced only between a capture and the same frame's tonemap draw
  const TextureImpl* lastDepth = nullptr;   // compared only (a new scene depth target = a new course: cache flushed)
  uint64_t capFrame = 0;       // frames that were built (age unit of the cache)
  uint32_t slot = 0; bool slotUsed[kSlots] = {}; bool slotTs[kSlots] = {}; uint64_t slotFrame[kSlots] = {};
  int debug = 0, logEvery = 120, ttl = 900, flipY = 0;
  Stats st; int lastLogPresent = 0;
  bool loggedCamFail = false, loggedNoPos = false;
  int keyCb = 1;   // SNAP_RT_KEYCB: the draw key includes the per-object vertex constants (default); 0 = mesh identity only
  std::unordered_set<uint64_t> curA, curB, prevA, prevB;   // per-frame key sets for both key forms (identity only / with constants)
  uint64_t kDraws = 0, kA = 0, kB = 0, kStA = 0, kStB = 0, kFrames = 0;
} g;

bool IsSceneDepth(const TextureImpl* t) {
  return t && t->b.format == FMT_DEPTH32F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && t->b.samples <= 1;
}
bool IsSceneColor(const TextureImpl* t) {
  return t && t->b.format == FMT_RGBA16F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && !(t->b.flags & TF_DISPLAY);
}
void Bar(VkCommandBuffer cb) {   // everything before -> everything after (a few per frame; the capture, build and trace phases are serialized on purpose)
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}

void Fail(const char* why) { if (!g_failed) fprintf(stderr, "[rt] disabled: %s\n", why); g_failed = true; }

bool Setup(Host* h) {
  if (g.ready) return true;
  if (g_failed) return false;
  g.h = h;
  g.debug = EnvInt("SNAP_RT_DEBUG", 0);
  g.logEvery = EnvInt("SNAP_RT_LOG", g.debug ? 120 : 0);
  g.ttl = EnvInt("SNAP_RT_TTL", 900);
  g.keyCb = EnvInt("SNAP_RT_KEYCB", 1);
  g.flipY = EnvInt("SNAP_RT_FLIPY", h->originMode == ORIGIN_UPPER_LEFT ? 1 : 0);
  const uint64_t arenaB = (uint64_t)EnvInt("SNAP_RT_ARENA_MB", 384) << 20, blasB = (uint64_t)EnvInt("SNAP_RT_BLAS_MB", 512) << 20, scrB = (uint64_t)EnvInt("SNAP_RT_SCRATCH_MB", 256) << 20;
  const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  const VkBufferUsageFlags asIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (!MakeBuf(h, arenaB, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | asIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, dl, &g.arena, true) ||
      !MakeBuf(h, blasB, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, dl, &g.blasStore, true) ||
      !MakeBuf(h, scrB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, dl, &g.scratch, true) ||
      !MakeBuf(h, (uint64_t)kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR), asIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, dl, &g.instStore, true) ||
      !MakeBuf(h, kSlots * 8 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.stats, false)) {
    Fail("buffer allocation failed"); return false;
  }
  g.arenaR.Init(arenaB); g.blasR.Init(blasB);
  memset(g.stats.map, 0, g.stats.size);
  // TLAS (rebuilt in place every frame; sized for kMaxInstances)
  {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.data.deviceAddress = g.instStore.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.geometryCount = 1; bi.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const uint32_t n = std::min(kMaxInstances, g_maxInstances ? g_maxInstances : kMaxInstances);
    pBuildSizes(h->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sz);
    g.tlasScratch = sz.buildScratchSize;
    if (!MakeBuf(h, sz.accelerationStructureSize + 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, dl, &g.tlasStore, true)) { Fail("TLAS storage allocation failed"); return false; }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = g.tlasStore.buf; ci.offset = 0; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (pCreateAs(h->device, &ci, nullptr, &g.tlas) != VK_SUCCESS) { Fail("vkCreateAccelerationStructureKHR (TLAS) failed"); return false; }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; ai.accelerationStructure = g.tlas;
    g.tlasAddr = pAsAddr(h->device, &ai);
  }
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(h->phys, &pp); g.tsPeriod = pp.limits.timestampPeriod;
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * 4;
  if (vkCreateQueryPool(h->device, &qi, nullptr, &g.qp) != VK_SUCCESS) g.qp = VK_NULL_HANDLE;
  // convert pipeline
  {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kRtConvSpv; mi.pCode = kRtConvSpv;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    if (vkCreateShaderModule(h->device, &mi, nullptr, &g.convMod) != VK_SUCCESS || vkCreatePipelineLayout(h->device, &pl, nullptr, &g.convPl) != VK_SUCCESS) { Fail("convert pipeline layout"); return false; }
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, g.convMod, "main", nullptr}; ci.layout = g.convPl;
    if (vkCreateComputePipelines(h->device, VK_NULL_HANDLE, 1, &ci, nullptr, &g.convPipe) != VK_SUCCESS) { Fail("convert pipeline"); return false; }
  }
  if (g.debug) {   // overlay objects (the pipeline is made per target format)
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 3; li.pBindings = b;
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kSlots}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots}};
    VkDescriptorPoolCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; di.maxSets = kSlots; di.poolSizeCount = 3; di.pPoolSizes = ps;
    VkShaderModuleCreateInfo m1{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}, m2{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    m1.codeSize = sizeof kRtOvlVertSpv; m1.pCode = kRtOvlVertSpv; m2.codeSize = sizeof kRtOvlFragSpv; m2.pCode = kRtOvlFragSpv;
    VkDescriptorSetLayout lay[kSlots]; for (auto& l : lay) l = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(h->device, &li, nullptr, &g.dsl) != VK_SUCCESS) { Fail("overlay set layout"); return false; }
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &g.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pl, nullptr, &g.ovlPl) != VK_SUCCESS || vkCreateSampler(h->device, &si, nullptr, &g.samp) != VK_SUCCESS ||
        vkCreateDescriptorPool(h->device, &di, nullptr, &g.dpool) != VK_SUCCESS || vkCreateShaderModule(h->device, &m1, nullptr, &g.ovlVs) != VK_SUCCESS ||
        vkCreateShaderModule(h->device, &m2, nullptr, &g.ovlFs) != VK_SUCCESS) { Fail("overlay objects"); return false; }
    for (auto& l : lay) l = g.dsl;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = g.dpool; ai.descriptorSetCount = kSlots; ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(h->device, &ai, g.sets) != VK_SUCCESS) { Fail("overlay descriptor sets"); return false; }
  }
  g.ready = true;
  fprintf(stderr, "[rt] ready: arena %llu MB, BLAS store %llu MB, scratch %llu MB, TTL %d, debug %d, flipY %d, timestamps %s\n", (unsigned long long)(arenaB >> 20), (unsigned long long)(blasB >> 20),
          (unsigned long long)(scrB >> 20), g.ttl, g.debug, g.flipY, g.qp ? "on" : "off");
  return true;
}

void KillEntry(Host* h, Entry& e) {
  if (e.arenaBytes) g.arenaR.Free(e.arenaOff, e.arenaBytes);
  if (e.blasBytes) g.blasR.Free(e.blasOff, e.blasBytes);
  if (e.as) g.dead.push_back({e.as, g.capFrame});
  (void)h;
  e = Entry{};
}
void Evict(Host* h, uint64_t keep = 0) {   // drop entries past their TTL (unstable ones, seen fewer than 3 frames, after 2)
  for (auto it = g.entries.begin(); it != g.entries.end();) {
    Entry& e = it->second;
    const int64_t age = (int64_t)g.capFrame - e.lastSeen, lim = e.seen >= 3 ? (int64_t)g.ttl : 2;
    if (it->first != keep && age > lim) { KillEntry(h, e); it = g.entries.erase(it); } else ++it;
  }
}
void Flush(Host* h) { for (auto& kv : g.entries) KillEntry(h, kv.second); g.entries.clear(); g.frame.clear(); }
void ReapDead(Host* h) {
  for (size_t i = 0; i < g.dead.size();) {
    if (g.capFrame - g.dead[i].frame > 8) { pDestroyAs(h->device, g.dead[i].as, nullptr); g.dead[i] = g.dead.back(); g.dead.pop_back(); } else i++;
  }
}

uint64_t HashKey(const DrawInfo& di, bool withCb) {
  uint64_t x = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { x ^= v; x *= 1099511628211ull; };
  if (di.vsKey) for (int i = 0; i < 12 && di.vsKey[i]; i++) mix((uint8_t)di.vsKey[i]);
  for (uint64_t v : di.vbAddr) mix(v);
  mix(di.ibAddr); mix((uint64_t)(int64_t)di.first); mix((uint64_t)(int64_t)di.base); mix((uint64_t)(int64_t)di.count); mix(di.instances); mix((uint64_t)di.topo);
  if (withCb) mix(di.cbHash);
  return x ? x : 1;
}

uint32_t TriVerts(VkPrimitiveTopology t, uint32_t n) {
  switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return n / 3 * 3;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return n >= 3 ? (n - 2) * 3 : 0;
    default: return 0;
  }
}

}  // namespace

namespace {
std::mutex g_patchedMu; std::set<std::string> g_patched;   // first 12 hex digits of the vertex programs whose module carries the Xfb decorations
bool IsPatched(const char* k) { if (!k) return false; std::lock_guard<std::mutex> lk(g_patchedMu); return g_patched.count(std::string(k, 12)) != 0; }
}
bool Wanted() { static const bool w = EnvOn("SNAP_RT"); return w; }
bool On() { return g_on; }
bool Capture() { return g_on && g_capture && !g_failed; }

void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& dext, VkPhysicalDeviceVulkan11Features* f11, VkPhysicalDeviceVulkan12Features* f12) {
  if (!Wanted()) return;
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ep(n);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, ep.data());
  auto has = [&](const char* s) { for (auto& e : ep) if (!strcmp(e.extensionName, s)) return true; return false; };
  const char* need[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
  for (const char* s : need) if (!has(s)) { fprintf(stderr, "[rt] off: device lacks %s\n", s); return; }
  VkPhysicalDeviceAccelerationStructureFeaturesKHR qa{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR qr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceTransformFeedbackFeaturesEXT qt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
  VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  qa.pNext = &qr; qr.pNext = &qt; qt.pNext = &q12;
  VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qa};
  vkGetPhysicalDeviceFeatures2(phys, &q2);
  if (!qa.accelerationStructure || !qr.rayQuery || !qt.transformFeedback || !q12.bufferDeviceAddress) {
    fprintf(stderr, "[rt] off: features accelerationStructure=%d rayQuery=%d transformFeedback=%d bufferDeviceAddress=%d\n", (int)qa.accelerationStructure, (int)qr.rayQuery, (int)qt.transformFeedback, (int)q12.bufferDeviceAddress);
    return;
  }
  for (const char* s : need) dext.push_back(s);
  g_fAs.accelerationStructure = VK_TRUE; g_fRq.rayQuery = VK_TRUE; g_fTf.transformFeedback = VK_TRUE;
  g_fAs.pNext = &g_fRq; g_fRq.pNext = &g_fTf; g_fTf.pNext = f11->pNext; f11->pNext = &g_fAs;
  f12->bufferDeviceAddress = VK_TRUE;
  g_on = true;
  g_capture = EnvOn("SNAP_RT_CAPTURE", true);
}

static const nvn::FxEffect kFxEffect = {"rt", [] { return snap_rt::Capture(); }, nullptr, [](nvn::ReplayCtx& cx, const char* k) { snap_rt::OnTonemap(cx, k); }};

void DeviceCreated(Host* h) {
  if (!g_on) return;
  nvn::FxRegister(&kFxEffect);   // the build runs from the Fx post-tonemap slot (photo frames and occlusion scopes are filtered there)
  VkDevice d = h->device;
#define L(v, n) v = (decltype(v))vkGetDeviceProcAddr(d, n)
  L(pCreateAs, "vkCreateAccelerationStructureKHR"); L(pDestroyAs, "vkDestroyAccelerationStructureKHR"); L(pBuildSizes, "vkGetAccelerationStructureBuildSizesKHR");
  L(pCmdBuild, "vkCmdBuildAccelerationStructuresKHR"); L(pAsAddr, "vkGetAccelerationStructureDeviceAddressKHR");
  L(pBindXfb, "vkCmdBindTransformFeedbackBuffersEXT"); L(pBeginXfb, "vkCmdBeginTransformFeedbackEXT"); L(pEndXfb, "vkCmdEndTransformFeedbackEXT");
#undef L
  if (!pCreateAs || !pDestroyAs || !pBuildSizes || !pCmdBuild || !pAsAddr || !pBindXfb || !pBeginXfb || !pEndXfb) { fprintf(stderr, "[rt] off: entry points missing\n"); g_on = false; return; }
  VkPhysicalDeviceAccelerationStructurePropertiesKHR pa{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceTransformFeedbackPropertiesEXT pt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
  pa.pNext = &pt;
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pa};
  vkGetPhysicalDeviceProperties2(h->phys, &p2);
  g_scratchAlign = std::max(1u, pa.minAccelerationStructureScratchOffsetAlignment);
  g_maxPrims = pa.maxPrimitiveCount; g_maxInstances = (uint32_t)std::min<uint64_t>(pa.maxInstanceCount, 0xFFFFFFFFu);
  fprintf(stderr, "[rt] device: ray query + acceleration structure + transform feedback on; scratch offset alignment %u, max geometries %llu, max instances %llu, max primitives %llu; xfb buffers %u, max xfb buffer size %llu, stride %u, queries %d\n",
          g_scratchAlign, (unsigned long long)pa.maxGeometryCount, (unsigned long long)pa.maxInstanceCount, (unsigned long long)pa.maxPrimitiveCount, pt.maxTransformFeedbackBuffers,
          (unsigned long long)pt.maxTransformFeedbackBufferSize, pt.maxTransformFeedbackBufferDataStride, (int)pt.transformFeedbackQueries);
}

void DeviceShutdown(Host* h) {
  if (!g.ready) return;
  VkDevice d = h->device;
  Flush(h);
  for (auto& x : g.dead) pDestroyAs(d, x.as, nullptr);
  g.dead.clear();
  if (g.tlas) pDestroyAs(d, g.tlas, nullptr);
  for (Buf* b : {&g.arena, &g.blasStore, &g.scratch, &g.instStore, &g.tlasStore, &g.stats}) FreeBuf(h, *b);
  if (g.qp) vkDestroyQueryPool(d, g.qp, nullptr);
  if (g.convPipe) vkDestroyPipeline(d, g.convPipe, nullptr);
  if (g.convPl) vkDestroyPipelineLayout(d, g.convPl, nullptr);
  if (g.convMod) vkDestroyShaderModule(d, g.convMod, nullptr);
  if (g.ovlPipe) vkDestroyPipeline(d, g.ovlPipe, nullptr);
  if (g.ovlPl) vkDestroyPipelineLayout(d, g.ovlPl, nullptr);
  if (g.dpool) vkDestroyDescriptorPool(d, g.dpool, nullptr);
  if (g.dsl) vkDestroyDescriptorSetLayout(d, g.dsl, nullptr);
  if (g.samp) vkDestroySampler(d, g.samp, nullptr);
  if (g.ovlVs) vkDestroyShaderModule(d, g.ovlVs, nullptr);
  if (g.ovlFs) vkDestroyShaderModule(d, g.ovlFs, nullptr);
  g.ready = false;
}

// Vertex modules: gl_Position (member 0 of the gl_PerVertex block) gets Xfb decorations (buffer 0, offset 0, stride 16) plus the TransformFeedback
// capability and the Xfb execution mode. A pipeline with these decorations behaves as before unless transform feedback is begun on the command buffer.
bool PatchVertexXfb(std::vector<uint32_t>& spv, const char* key12) {
  if (!g_on || !g_capture || spv.size() < 6 || spv[0] != 0x07230203) return false;
  size_t capEnd = 0, epEnd = 0, posDecl = 0; uint32_t entry = 0, blk = 0;
  for (size_t i = 5; i < spv.size();) {
    const uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
    if (!wc || i + wc > spv.size()) return false;
    if (op == kOpCapability) { capEnd = i + wc; if (spv[i + 1] == 53) return false; }
    if (op == kOpEntryPoint) { epEnd = i + wc; if (spv[i + 1] == 0 && !entry) entry = spv[i + 2]; }
    if (op == kOpExecutionMode && spv[i + 2] == 11) return false;   // already Xfb
    if (op == kOpMemberDecorate && wc == 5 && spv[i + 2] == 0 && spv[i + 3] == 11 && spv[i + 4] == 0) { blk = spv[i + 1]; posDecl = i + wc; }
    i += wc;
  }
  if (!entry || !capEnd || !epEnd || !posDecl) return false;
  std::vector<uint32_t> ann, cap, em;
  Emit(ann, kOpMemberDecorate, {blk, 0, 36, 0});    // XfbBuffer 0
  Emit(ann, kOpMemberDecorate, {blk, 0, 37, 16});   // XfbStride 16
  Emit(ann, kOpMemberDecorate, {blk, 0, 35, 0});    // Offset 0
  Emit(cap, kOpCapability, {53});                   // TransformFeedback
  Emit(em, kOpExecutionMode, {entry, 11});          // Xfb
  // later positions first so the earlier indices stay valid: annotations > entry points > capabilities (capabilities come first in a module)
  spv.insert(spv.begin() + posDecl, ann.begin(), ann.end());
  spv.insert(spv.begin() + epEnd, em.begin(), em.end());
  spv.insert(spv.begin() + capEnd, cap.begin(), cap.end());
  if (key12) { std::lock_guard<std::mutex> lk(g_patchedMu); g_patched.emplace(key12, 12); }
  return true;
}

void DrawCapture::Begin(ReplayCtx& c, const DrawInfo& di) {
  open = false;
  if (!Capture() || di.hasGeomTess || FxIsPhotoFrame()) return;   // photo / Evaluation frames get no capture work
  TextureImpl* t8 = c.rt[8]; TextureImpl* t0 = c.rt[0];
  if (!IsSceneDepth(t8) || !IsSceneColor(t0) || !di.depthTest || !di.depthWrite || di.blend0) return;   // scene opaque draws only; never the Evaluation (DEPTH32F_STENCIL8) or photo targets
  if (!IsPatched(di.vsKey)) { static int n; if (n++ < 8) fprintf(stderr, "[rt] scene draw with vertex program %.12s that has no gl_Position block member to decorate: not captured\n", di.vsKey ? di.vsKey : "?"); return; }   // a draw whose module lacks the Xfb decorations must not begin transform feedback
  if (!di.cam || di.camRange < 0x250) { if (!g.loggedNoPos) { g.loggedNoPos = true; fprintf(stderr, "[rt] scene draw without a camera bank (range %llu): not captured\n", (unsigned long long)di.camRange); } return; }
  const uint64_t nv64 = (uint64_t)TriVerts(di.topo, di.vertexCount) * std::max(di.instances, 1u);
  if (nv64 == 0) return;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!Setup(c.h)) return;
  if (nv64 * 16 > g.arena.size / 4) { g.st.skipped++; return; }
  const uint32_t nv = (uint32_t)nv64;
  const double t0ms = Now();
  if (g.framePresent != g_presentCount) { g.frame.clear(); g.framePresent = g_presentCount; }
  if (g.lastDepth != t8) { if (g.lastDepth) { fprintf(stderr, "[rt] scene depth target changed: capture cache flushed (%zu entries)\n", g.entries.size()); Flush(c.h); } g.lastDepth = t8; }
  g.sceneDepth = t8;
  const uint64_t keyA = HashKey(di, false), keyB = HashKey(di, true), key = g.keyCb ? keyB : keyA;
  g.curA.insert(keyA); g.curB.insert(keyB);
  Entry& e = g.entries[key];
  e.lastSeen = (int64_t)g.capFrame + 1;   // the frame being captured (capFrame advances at the tonemap draw)
  if (e.vcount != nv) {
    if (e.vcount) { const uint64_t seen = e.seen; KillEntry(c.h, e); e.seen = (uint32_t)seen; }
    uint64_t off = 0;
    if (!g.arenaR.Alloc((uint64_t)nv * 16, &off)) { Evict(c.h, key); if (!g.arenaR.Alloc((uint64_t)nv * 16, &off)) { g.st.skipped++; if (!e.vcount) g.entries.erase(key); return; } }
    e.vcount = nv; e.arenaOff = off; e.arenaBytes = (uint64_t)nv * 16;
  }
  // camera: world = inverse(viewProjection) * clip. The matrix is column-major at +0x100.
  Mat m;
  { float f[16]; memcpy(f, di.cam + 0x100, 64); for (int r = 0; r < 4; r++) for (int cc = 0; cc < 4; cc++) m.m[r][cc] = f[cc * 4 + r]; }
  Mat inv;
  if (!Invert(m, inv)) { if (!g.loggedCamFail) { g.loggedCamFail = true; fprintf(stderr, "[rt] camera viewProjection is singular: draw not captured\n"); } return; }
  bool dup = false;
  for (auto& fd : g.frame) if (fd.key == key) dup = true;
  if (!dup) {
    FrameDraw fd{key, nv, e.arenaOff, {}, {}};
    for (int cc = 0; cc < 4; cc++) for (int r = 0; r < 4; r++) fd.inv[cc * 4 + r] = (float)inv.m[r][cc];
    memcpy(fd.eye, di.cam + 0x240, 12);
    g.frame.push_back(fd);
  }
  VkDeviceSize off = e.arenaOff, size = (VkDeviceSize)nv * 16;
  pBindXfb(c.cb, 0, 1, &g.arena.buf, &off, &size);
  pBeginXfb(c.cb, 0, 0, nullptr, nullptr);
  open = true; cx = &c;
  g.st.draws++; g.st.tris += nv / 3; g.st.xfbBytes += size;
  g.st.cpuCapMs += Now() - t0ms;
}

void DrawCapture::End() {
  if (!open) return;
  open = false;
  pEndXfb(cx->cb, 0, 0, nullptr, nullptr);
}

void OnTonemap(ReplayCtx& cx, const char*) {   // called by FxAfterDraw: tonemap draws only, never on photo frames or inside an occlusion scope
  if (!Capture() || FxIsPhotoFrame()) return;
  std::lock_guard<std::mutex> lk(g.mu);
  if (!g.ready || g.frame.empty()) return;
  Host* h = cx.h; VkCommandBuffer cb = cx.cb;
  TextureImpl* t = cx.rt[0];
  std::vector<FrameDraw> draws; draws.swap(g.frame);
  { uint64_t sa = 0, sb = 0; for (uint64_t k : g.curA) sa += g.prevA.count(k); for (uint64_t k : g.curB) sb += g.prevB.count(k);   // key stability against the previous frame
    g.kA += g.curA.size(); g.kB += g.curB.size(); g.kStA += sa; g.kStB += sb; g.kFrames++;
    g.prevA.swap(g.curA); g.prevB.swap(g.curB); g.curA.clear(); g.curB.clear(); }
  if (!t || t->b.width < 640 || t->b.height < 360) return;   // photo / thumbnail targets: no work
  const double tc0 = Now();
  EndRendering(cx);
  g.capFrame++;
  const uint32_t slot = g.slot++ % kSlots;
  const bool timed = g.qp != VK_NULL_HANDLE;
  if (timed) { vkCmdResetQueryPool(cb, g.qp, slot * 4, 4); g.slotTs[slot] = false; }
  Bar(cb);   // transform feedback writes -> convert; previous frame's trace/build reads -> this frame's writes
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * 4 + 0);
  // 1. clip -> world, in place
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.convPipe);
  for (auto& d : draws) {
    struct { float m[16]; uint64_t addr; uint32_t count, pad; } pc;
    memcpy(pc.m, d.inv, 64); pc.addr = g.arena.addr + d.arenaOff; pc.count = d.vcount; pc.pad = 0;
    vkCmdPushConstants(cb, g.convPl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (d.vcount + 63) / 64, 1, 1);
  }
  Bar(cb);
  // 2. BLAS for every draw captured this frame (rebuilt in place when the size is unchanged)
  std::vector<VkAccelerationStructureGeometryKHR> geos(draws.size());
  std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  std::vector<uint64_t> keys;
  infos.reserve(draws.size()); ranges.reserve(draws.size()); keys.reserve(draws.size());
  uint64_t scratchUsed = 0, blasBuilt = 0, blasTris = 0;
  auto flush = [&]() {
    if (infos.empty()) return;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rp(infos.size());
    for (size_t i = 0; i < infos.size(); i++) rp[i] = &ranges[i];
    pCmdBuild(cb, (uint32_t)infos.size(), infos.data(), rp.data());
    Bar(cb);   // the next batch reuses the scratch buffer
    infos.clear(); ranges.clear(); scratchUsed = 0;
  };
  for (size_t di = 0; di < draws.size(); di++) {
    auto it = g.entries.find(draws[di].key);
    if (it == g.entries.end()) continue;
    Entry& e = it->second;
    e.lastSeen = (int64_t)g.capFrame;
    e.seen++;   // frames this draw was captured (stable = 3 or more)
    VkAccelerationStructureGeometryKHR& geo = geos[di];
    geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; geo.geometry.triangles.vertexData.deviceAddress = g.arena.addr + e.arenaOff;
    geo.geometry.triangles.vertexStride = 16; geo.geometry.triangles.maxVertex = e.vcount - 1; geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &geo;
    const uint32_t prims = e.vcount / 3;
    if (!e.as) {
      VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
      pBuildSizes(h->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
      uint64_t off = 0;
      if (!g.blasR.Alloc(sz.accelerationStructureSize, &off)) { Evict(h, draws[di].key); if (!g.blasR.Alloc(sz.accelerationStructureSize, &off)) { g.st.skipped++; continue; } }
      VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
      ci.buffer = g.blasStore.buf; ci.offset = off; ci.size = sz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      if (pCreateAs(h->device, &ci, nullptr, &e.as) != VK_SUCCESS) { g.blasR.Free(off, sz.accelerationStructureSize); e.as = VK_NULL_HANDLE; g.st.skipped++; continue; }
      e.blasOff = off; e.blasBytes = sz.accelerationStructureSize; e.scratchNeed = sz.buildScratchSize;
      VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; ai.accelerationStructure = e.as;
      e.blasAddr = pAsAddr(h->device, &ai);
    }
    const uint64_t need = (e.scratchNeed + g_scratchAlign - 1) / g_scratchAlign * g_scratchAlign;
    if (scratchUsed + need > g.scratch.size) flush();
    if (need > g.scratch.size) { g.st.skipped++; continue; }
    bi.dstAccelerationStructure = e.as; bi.scratchData.deviceAddress = g.scratch.addr + scratchUsed;
    scratchUsed += need;
    infos.push_back(bi); ranges.push_back({prims, 0, 0, 0}); keys.push_back(draws[di].key);
    e.built = true; blasBuilt++; blasTris += prims;
  }
  // infos hold pointers into geos (stable: sized up front); build the rest
  flush();
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * 4 + 1);
  // 3. TLAS over everything alive
  Evict(h);
  ReapDead(h);
  uint32_t ninst = 0;
  {
    uint64_t bytes = (uint64_t)std::min<size_t>(g.entries.size(), kMaxInstances) * sizeof(VkAccelerationStructureInstanceKHR);
    VkBuffer src = VK_NULL_HANDLE;
    uint8_t* mp = bytes ? NewScratch(cx, bytes, &src, 0) : nullptr;
    if (mp) {
      auto* inst = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(mp);
      for (auto& kv : g.entries) {
        const Entry& e = kv.second;
        if (!e.built || !e.as || ninst >= kMaxInstances) continue;
        VkAccelerationStructureInstanceKHR& I = inst[ninst];
        memset(&I, 0, sizeof I);
        I.transform.matrix[0][0] = I.transform.matrix[1][1] = I.transform.matrix[2][2] = 1.0f;
        I.instanceCustomIndex = ninst; I.mask = 0xFF; I.instanceShaderBindingTableRecordOffset = 0;
        I.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        I.accelerationStructureReference = e.blasAddr;
        ninst++;
      }
      if (ninst) { VkBufferCopy cp{0, 0, (uint64_t)ninst * sizeof(VkAccelerationStructureInstanceKHR)}; vkCmdCopyBuffer(cb, src, g.instStore.buf, 1, &cp); }
    }
  }
  if (ninst) {
    Bar(cb);
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geo.geometry.instances.data.deviceAddress = g.instStore.addr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR; bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1; bi.pGeometries = &geo; bi.dstAccelerationStructure = g.tlas; bi.scratchData.deviceAddress = g.scratch.addr;
    VkAccelerationStructureBuildRangeInfoKHR rg{ninst, 0, 0, 0}; const VkAccelerationStructureBuildRangeInfoKHR* rgp = &rg;
    pCmdBuild(cb, 1, &bi, &rgp);
    Bar(cb);
  }
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * 4 + 2);   // end of the build phase
  g.st.blasBuilt += blasBuilt; g.st.blasTris += blasTris; g.st.instances += ninst; g.st.frames++;
  // 4. debug overlay (and its statistics)
  bool ovl = false;
  TextureImpl* z = FxSceneDepth();
  g.sceneDepth = nullptr;
  g.slotUsed[slot] = false;
  if (g.debug && ninst && z && z->EnsureImage() && t->EnsureImage() && t->fi && FxWritable(t)) {
    const VkFormat fmt = t->fi->vk;
    if (g.ovlFmt != fmt && g.ovlPipe) { vkDeviceWaitIdle(h->device); vkDestroyPipeline(h->device, g.ovlPipe, nullptr); g.ovlPipe = VK_NULL_HANDLE; }
    if (!g.ovlPipe) {
      VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g.ovlVs, "main", nullptr},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g.ovlFs, "main", nullptr}};
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState ba{}; ba.blendEnable = VK_TRUE; ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      ba.colorBlendOp = VK_BLEND_OP_ADD; ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
      ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
      VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cbs.attachmentCount = 1; cbs.pAttachments = &ba;
      const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
      VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
      VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
      gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
      gp.pColorBlendState = &cbs; gp.pDynamicState = &ds; gp.layout = g.ovlPl;
      if (vkCreateGraphicsPipelines(h->device, VK_NULL_HANDLE, 1, &gp, nullptr, &g.ovlPipe) != VK_SUCCESS) { g.ovlPipe = VK_NULL_HANDLE; Fail("overlay pipeline"); }
      g.ovlFmt = fmt;
    }
    // camera of the first captured draw this frame (every scene draw shares the camera bank)
    SubRange rr; VkImageView rtv = t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
    const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
    if (g.ovlPipe && !g_failed && rr.yn == 1 && w >= 64 && hh >= 64) {
      VkImageLayout prevZ = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImageView zv = FxDepthRead(cx, z, &prevZ);   // never written: sampled in DEPTH_STENCIL_READ_ONLY, the tracked layout is restored below
      VkDescriptorSet set = g.sets[slot];
      VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &g.tlas;
      VkDescriptorImageInfo ii{g.samp, zv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      VkDescriptorBufferInfo bi{g.stats.buf, slot * 32ull, 32};
      VkWriteDescriptorSet wr[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
      wr[0].pNext = &wa; wr[0].dstSet = set; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      wr[1].dstSet = set; wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &ii;
      wr[2].dstSet = set; wr[2].dstBinding = 2; wr[2].descriptorCount = 1; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].pBufferInfo = &bi;
      vkUpdateDescriptorSets(h->device, 3, wr, 0, nullptr);
      vkCmdFillBuffer(cb, g.stats.buf, slot * 32ull, 32, 0);
      Bar(cb);
      TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
      struct { float inv[16]; float eye[4]; float p0[4]; float size[4]; } pc{};
      memcpy(pc.inv, draws[0].inv, 64);
      memcpy(pc.eye, draws[0].eye, 12); pc.p0[0] = (float)g.debug; pc.p0[1] = (float)g.flipY; pc.p0[2] = 0.01f; pc.p0[3] = 0.05f; pc.size[0] = (float)w; pc.size[1] = (float)hh;
      g.slotFrame[slot] = g.capFrame;
      VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      ca.imageView = rtv; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
      ri.renderArea = {{0, 0}, {w, hh}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
      vkCmdBeginRendering(cb, &ri);
      VkViewport vpt{0, 0, (float)w, (float)hh, 0, 1}; VkRect2D sc{{0, 0}, {w, hh}};
      vkCmdSetViewport(cb, 0, 1, &vpt); vkCmdSetScissor(cb, 0, 1, &sc);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ovlPipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ovlPl, 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cb, g.ovlPl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRendering(cb);
      FxDepthDone(cx, z, prevZ);
      t->uploaded = true;
      ovl = true;
      g.slotUsed[slot] = true;
    }
  }
  if (timed) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, slot * 4 + 3);
  Bar(cb);   // the capture of the next frame writes the arena again
  if (ovl) {   // put back the viewport and scissor the game had set (nvn_aa.cpp does the same after its passes)
    if (cx.vpHave & 1) { const float* v = cx.vpGuest[0]; VkViewport vp = MakeNvnViewport(h->originMode, v[0], v[1], v[2], v[3], t->scale); vkCmdSetViewport(cb, 0, 1, &vp); }
    else cx.vpSet = false;
    if (cx.scHave & 1) {
      int x = cx.scGuest[0][0], y = cx.scGuest[0][1], sw = cx.scGuest[0][2], sh = cx.scGuest[0][3];
      if (x < 0) { sw += x; x = 0; }
      if (y < 0) { sh += y; y = 0; }
      sw = std::max(sw, 0); sh = std::max(sh, 0);
      const float sc = t->scale;
      if (sc != 1.0f) { x = (int)std::floor(x * sc); y = (int)std::floor(y * sc); sw = (int)std::ceil(sw * sc); sh = (int)std::ceil(sh * sc); }
      VkRect2D r{{x, y}, {(uint32_t)sw, (uint32_t)sh}}; vkCmdSetScissor(cb, 0, 1, &r);
    } else cx.scSet = false;
  }
  g.st.cpuBuildMs += Now() - tc0;
  // read back an older slot (the GPU has long finished it)
  if (g.qp) {
    const uint32_t old = (slot + kSlots - 4) % kSlots;
    uint64_t r[8] = {};
    g.slotTs[slot] = true;   // all four stamps of this slot are recorded by now
    if (g.slotTs[old] && vkGetQueryPoolResults(h->device, g.qp, old * 4, 4, sizeof r, r, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && r[1] && r[3] && r[5] && r[7]) {
      const double ms = (double)g.tsPeriod * 1e-6;
      g.st.gpuBuildMs += (double)(r[4] - r[0]) * ms; g.st.gpuBlasMs += (double)(r[2] - r[0]) * ms; g.st.gpuN++;
      if (g.slotUsed[old]) { g.st.gpuOvlMs += (double)(r[6] - r[4]) * ms; g.st.ovlN++; }
      (void)r;
    }
    if (g.slotUsed[old]) {
      const uint32_t* s = static_cast<const uint32_t*>(g.stats.map) + old * 8;
      if (s[0]) { for (int i = 0; i < 8; i++) g.st.cls[i] += s[i]; g.st.clsN++; }
    }
  }
}

void OnPresent() {
  if (!g.ready || g.logEvery <= 0 || g_presentCount - g.lastLogPresent < g.logEvery) return;
  g.lastLogPresent = g_presentCount;
  std::lock_guard<std::mutex> lk(g.mu);
  Stats& s = g.st;
  if (!s.frames) return;
  const double f = (double)s.frames;
  fprintf(stderr, "[rt] p%d: %u frames: %.0f draws, %.0f tris captured (%.1f MB xfb) per frame; BLAS built %.0f (%.0f tris) per frame; %.0f instances; live entries %zu, arena %.1f/%llu MB (hwm %.1f), BLAS %.1f/%llu MB (hwm %.1f)%s\n",
          g_presentCount, s.frames, (double)s.draws / f, (double)s.tris / f, (double)s.xfbBytes / f / 1048576.0, (double)s.blasBuilt / f, (double)s.blasTris / f, (double)s.instances / f, g.entries.size(),
          (double)g.arenaR.used / 1048576.0, (unsigned long long)(g.arenaR.cap >> 20), (double)g.arenaR.hwm / 1048576.0, (double)g.blasR.used / 1048576.0, (unsigned long long)(g.blasR.cap >> 20), (double)g.blasR.hwm / 1048576.0,
          s.skipped ? " (some draws skipped: raise SNAP_RT_ARENA_MB/BLAS_MB/SCRATCH_MB)" : "");
  fprintf(stderr, "[rt]   cost: CPU capture %.3f ms + build record %.3f ms per frame; GPU convert+BLAS+TLAS %.3f ms (convert+BLAS %.3f, %u samples)", s.cpuCapMs / f, s.cpuBuildMs / f, s.gpuN ? s.gpuBuildMs / s.gpuN : 0.0, s.gpuN ? s.gpuBlasMs / s.gpuN : 0.0, s.gpuN);
  if (s.ovlN) fprintf(stderr, "; GPU overlay %.3f ms (%u samples)", s.gpuOvlMs / s.ovlN, s.ovlN);
  fprintf(stderr, "\n");
  if (g.kFrames) { const double kf = (double)g.kFrames; fprintf(stderr, "[rt]   draw keys per frame: %.1f draws; %.1f distinct by mesh identity (%.0f%% also in the previous frame), %.1f distinct with per-object constants (%.0f%% also in the previous frame); key in use: %s\n", (double)s.draws / f, (double)g.kA / kf, g.kA ? 100.0 * g.kStA / g.kA : 0.0, (double)g.kB / kf, g.kB ? 100.0 * g.kStB / g.kB : 0.0, g.keyCb ? "with constants" : "identity"); g.kA = g.kB = g.kStA = g.kStB = g.kFrames = 0; }
  if (s.clsN && s.cls[0]) {
    const double n = (double)s.cls[0];
    fprintf(stderr, "[rt]   sanity (%u frames, %.0f sampled px): match %.1f%%, rt-in-front %.1f%%, rt-behind %.1f%%, depth-no-hit %.1f%%, sky %.1f%%, hit-on-sky %.1f%%\n", s.clsN, n,
            100.0 * s.cls[1] / n, 100.0 * s.cls[2] / n, 100.0 * s.cls[3] / n, 100.0 * s.cls[4] / n, 100.0 * s.cls[5] / n, 100.0 * s.cls[6] / n);
  }
  s = Stats{};
}

}  // namespace snap_rt
