// SPDX-License-Identifier: MIT
// Replay of the draw-related recorded commands: pipelines, descriptor sets, vertex/index binds, draws, buffer->texture
// copies. Everything here runs at SubmitLists time (see nvn_vk.cpp Replay); the NVN entry points only recorded data.
//
// Binding model (full description in docs/nvn-vulkan-design.md):
//   * NVN state is recorded by value and replayed into a per-command-buffer DrawState; the first draw after a change
//     builds (or finds) a VkPipeline keyed by the complete state.
//   * set 0: binding 0 = Ryujinx support buffer; binding 1 + stage*16 + s = constant buffer c[s] of that stage.
//       c[0] driver constants (zeros), c[1] the program's constant-data blob, c[2] reserved (zeros),
//       c[3 + i] = NVN BindUniformBuffers index i. (The SASS reads reflection binding b as c[b + 3].)
//   * set 1: storage buffers; set 2: combined image samplers at stage*32 + n (NVN BindTexture index b =
//       (handleIndex - 8) / 2, found from the game's own sampler-bind loop at 0x915e80); set 3: storage images.
//   * Texture handles are `texId | samplerId << 20`; ids index the host tables of the bound texture/sampler pool.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <cmath>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include "nvn_shiny.h"   // shiny mod
#include "nvn_btnicon.h"   // button icon mod
#include "nvn_capture.h"   // photo capture filter under render scale
#include "nvn_hdphoto.h"   // HD photos (SNAP_HD_PHOTOS)
#include <deque>
#include <set>
#include <string>
#include <fstream>

#include "nvn_aa.h"
#include "nvn_fx.h"   // [fx]
#include "nvn_ssr.h"  // [water]
#include "hle/lighting_preset.h"   // SNAP_LIGHTING parser (shared with host/mods/gamemods.cpp, nvn_entry.cpp)
#include "nvn_internal.h"
#include "nvn_foliage.h"
#include "nvn_pipeline.h"
#include "nvn_rt.h"   // [rt] SNAP_RT=1 ray tracing track
#include "os/perf.h"   // SNAP_PERF instrumentation
#include "nvn_clear_spv.h"
#include "nvn_state.h"
#include "upscale/upscale_nvn.h"   // [upscale]
#include "nvn_framegen.h"          // [framegen]
#include "nvn_freecam.h"           // free camera clean shot
#include "nvn_shot.h"              // screenshot key (F12)
#include "nvn_pom.h"               // parallax occlusion mapping
#include "nvn_uismooth.h"          // [uismooth] smooth interface above 60 fps

namespace nvn {

extern bool g_provokingLast;                  // nvn_vk.cpp: VK_EXT_provoking_vertex enabled (NVN flat varyings come from the LAST vertex)
extern bool g_gplOn;                          // nvn_vk.cpp: VK_EXT_graphics_pipeline_library enabled (gfx_gpl)
extern VkShaderStageFlags g_sgPinStages;      // nvn_vk.cpp: stages that accept requiredSubgroupSize = 32
bool ModuleUsesSubgroup(VkShaderModule m);    // nvn_shader.cpp

// Pins a stage to 32 lanes when its SPIR-V uses subgroup ops (the translator assumes Maxwell's 32-lane warp; RDNA may run wave64).
static void PinSubgroup32(VkPipelineShaderStageCreateInfo* si, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo* rq, VkShaderModule baseMod) {
  if (!ModuleUsesSubgroup(baseMod)) return;
  if (!(g_sgPinStages & si->stage)) {   // no 32-lane pin on this GPU (for example a software rasterizer with 8 lanes): the lane math runs at the native width
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      fprintf(stderr, "[nvn] warning: a shader that assumes 32-lane subgroups cannot be pinned to 32 on this GPU (stage 0x%x, pin mask 0x%x); its results may differ from the Switch (photo scoring, light culling)\n",
              (unsigned)si->stage, (unsigned)g_sgPinStages);
    return;
  }
  *rq = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
  rq->requiredSubgroupSize = 32;
  rq->pNext = const_cast<void*>(si->pNext);
  si->pNext = rq;
}

struct Dummies {
  VkBuffer zero = VK_NULL_HANDLE, support = VK_NULL_HANDLE;
  VkDeviceMemory zeroMem = VK_NULL_HANDLE, supportMem = VK_NULL_HANDLE;
  VkImage sampledImg = VK_NULL_HANDLE, storageImg = VK_NULL_HANDLE;
  VkDeviceMemory sampledMem = VK_NULL_HANDLE, storageMem = VK_NULL_HANDLE;
  VkImageView sampledView = VK_NULL_HANDLE, storageView = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  bool imagesReady = false;
  // typed fallbacks for a shader-declared dimension the bound texture cannot be viewed as (nvn_draw.cpp DummyView)
  VkImage arrImg = VK_NULL_HANDLE, img3d = VK_NULL_HANDLE, img1d = VK_NULL_HANDLE;                 // 2D x6 (cube-compatible), 3D 1x1x1, 1D x2
  VkDeviceMemory arrMem = VK_NULL_HANDLE, mem3d = VK_NULL_HANDLE, mem1d = VK_NULL_HANDLE;
  VkImage depImg = VK_NULL_HANDLE; VkDeviceMemory depMem = VK_NULL_HANDLE; VkImageView vdep = VK_NULL_HANDLE;   // 1x1 D32F = 1.0: stand-in for a shadow-map binding the texture cannot serve
  VkSampler cmpSampler = VK_NULL_HANDLE;                                                                      // compare (LEQUAL) sampler for it
  VkImageView v2d = VK_NULL_HANDLE, v2da = VK_NULL_HANDLE, vcube = VK_NULL_HANDLE, vcubea = VK_NULL_HANDLE, v3d = VK_NULL_HANDLE,
              v1d = VK_NULL_HANDLE, v1da = VK_NULL_HANDLE;
};

struct BufBind { VkBuffer buf = VK_NULL_HANDLE; uint64_t off = 0, range = 0; PoolImpl* pool = nullptr; };

struct DrawState {
  ProgramImpl* prog = nullptr;        // effective program (ProgramResolve of stageProg)
  ProgramImpl* stageProg[6] = {};     // what BindProgram bound per stage
  BlendSt blend[8];
  ColorSt color{0, LOGIC_COPY, CMP_ALWAYS, 0};
  ChMaskSt chmask{0xFFFFFFFFu};
  MsSt ms{};
  PolySt poly{FACE_NONE, FRONT_CCW, POLY_FILL, 0};
  DsSt ds{};
  VaSt attr[16] = {};
  int nattr = 0;
  VsSt stream[16] = {};
  int nstream = 0;
  BufBind vb[16];
  uint64_t vbAddr[16] = {};          // guest address bound per vertex stream (draw log only)
  BufBind ubo[6][16];
  BufBind ssbo[6][16];
  uint64_t tex[6][32] = {};
  bool texSet[6][32] = {};
  uint64_t img[6][8] = {};
  bool imgSet[6][8] = {};
  TexPoolImpl* tp = nullptr;
  SampPoolImpl* sp = nullptr;
  int stCompare[2] = {0xFF, 0xFF}, stWrite[2] = {0xFF, 0xFF}, stRef[2] = {0, 0};   // front, back
  float biasFactor = 0, biasUnits = 0, biasClamp = 0;
  float alphaRef = 0;
  int patchSize = 3;
  bool warnedPrim = false;
  DrawState() {
    for (auto& b : blend) b = {BF_ONE, BF_ZERO, BF_ONE, BF_ZERO, BE_ADD, BE_ADD, 0, 0};
    ms.enable = 1;
    DsSet(ds, DS_DW, 1, 1); DsSet(ds, DS_DF, 4, CMP_LESS);
    DsSet(ds, DS_SF_F, 4, CMP_ALWAYS); DsSet(ds, DS_SF_B, 4, CMP_ALWAYS);
    for (int sh : {DS_FAIL_F, DS_ZFAIL_F, DS_ZPASS_F, DS_FAIL_B, DS_ZFAIL_B, DS_ZPASS_B}) DsSet(ds, sh, 4, SOP_KEEP);
  }
};

static float ShadowDistK();
static bool ShadowDynTexture(const TextureImpl* t);
static const TextureImpl* g_dynShadowTex = nullptr;
bool ModelDrawAdjust(ReplayCtx& cx, const Cmd& c, Cmd& adj);   // nvn_model.cpp: refined model packs (docs/hd-models.md)
namespace {

constexpr uint64_t kMaxUboRange = 65536;   // translated shaders declare vec4[4096]

// ---- state translation ------------------------------------------------------------------------------------------------
VkBlendFactor BlendFactor(int f) {
  switch (f) {
    case BF_ZERO: return VK_BLEND_FACTOR_ZERO;
    case BF_ONE: return VK_BLEND_FACTOR_ONE;
    case BF_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case BF_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BF_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BF_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BF_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case BF_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BF_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case BF_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BF_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case BF_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case BF_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case BF_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case BF_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case BF_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BF_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BF_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case BF_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
  }
}
VkBlendOp BlendOp(int e) {
  switch (e) {
    case BE_SUB: return VK_BLEND_OP_SUBTRACT;
    case BE_REVSUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BE_MIN: return VK_BLEND_OP_MIN;
    case BE_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
  }
}
VkCompareOp Cmp(uint32_t f) { return (VkCompareOp)std::clamp<int>((int)f - 1, 0, 7); }   // NVN 1..8 NEVER..ALWAYS
VkStencilOp SOp(uint32_t o) { return (VkStencilOp)std::clamp<int>((int)o - 1, 0, 7); }   // NVN 1..8 KEEP..DECR_WRAP
bool Topology(int prim, VkPrimitiveTopology* t) {
  switch (prim) {   // NVNdrawPrimitive
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    case 6: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case 0xA: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return true;
    case 0xB: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; return true;
    case 0xC: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return true;
    case 0xD: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; return true;
    case 0xE: *t = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; return true;
    default: return false;   // LINE_LOOP, QUADS, QUAD_STRIP, POLYGON: phase 3 (index rewrite)
  }
}

template <class T> void Put(std::string& k, const T& v) { k.append(reinterpret_cast<const char*>(&v), sizeof v); }

Dummies* GetDummies(Host* h) {
  if (h->dummies) return h->dummies;
  auto* d = new Dummies();
  void* mp = nullptr;
  if (CreateHostBuffer(h, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &d->zero, &d->zeroMem, &mp))
    memset(mp, 0, 65536);
  if (CreateHostBuffer(h, 4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &d->support, &d->supportMem, &mp)) {
    // Ryujinx SupportBuffer layout (1408 B): alpha_test @0, is_bgra[8] @16, viewport_inverse @144, viewport_size @160,
    // frag_scale_count @176, render_scale[73] @192 (16 B stride, all 1.0f), tfe_offset @1360, tfe_vertex_count @1376.
    memset(mp, 0, 4096);
    auto* f = static_cast<float*>(mp);
    for (int i = 0; i < 73; i++) f[(192 + 16 * i) / 4] = 1.0f;
  }
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(h->device, &sci, nullptr, &d->sampler);
  auto mk = [&](VkImageUsageFlags usage, VkImage* img, VkDeviceMemory* mem, VkImageView* view) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {1, 1, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(h->device, &ci, nullptr, img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, *img, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(h->device, &mai, nullptr, mem);
    vkBindImageMemory(h->device, *img, *mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(h->device, &vi, nullptr, view);
  };
  mk(VK_IMAGE_USAGE_SAMPLED_BIT, &d->sampledImg, &d->sampledMem, &d->sampledView);
  mk(VK_IMAGE_USAGE_STORAGE_BIT, &d->storageImg, &d->storageMem, &d->storageView);
  {
    auto mkTyped = [&](VkImageType type, VkExtent3D ext, uint32_t layers, VkImageCreateFlags fl, VkImage* img, VkDeviceMemory* mem) {
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.flags = fl; ci.imageType = type; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = ext; ci.mipLevels = 1; ci.arrayLayers = layers;
      ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      vkCreateImage(h->device, &ci, nullptr, img);
      VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, *img, &mr);
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = mr.size; mai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      vkAllocateMemory(h->device, &mai, nullptr, mem);
      vkBindImageMemory(h->device, *img, *mem, 0);
    };
    auto mkView = [&](VkImage img, VkImageViewType vt, uint32_t layers) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = img; vi.viewType = vt; vi.format = VK_FORMAT_R8G8B8A8_UNORM; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      VkImageView v = VK_NULL_HANDLE; vkCreateImageView(h->device, &vi, nullptr, &v); return v;
    };
    mkTyped(VK_IMAGE_TYPE_2D, {1, 1, 1}, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &d->arrImg, &d->arrMem);
    d->v2da = mkView(d->arrImg, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 6); d->vcube = mkView(d->arrImg, VK_IMAGE_VIEW_TYPE_CUBE, 6);
    d->vcubea = mkView(d->arrImg, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6);
    mkTyped(VK_IMAGE_TYPE_3D, {1, 1, 1}, 1, 0, &d->img3d, &d->mem3d);
    d->v3d = mkView(d->img3d, VK_IMAGE_VIEW_TYPE_3D, 1);
    mkTyped(VK_IMAGE_TYPE_1D, {1, 1, 1}, 2, 0, &d->img1d, &d->mem1d);
    d->v1d = mkView(d->img1d, VK_IMAGE_VIEW_TYPE_1D, 1); d->v1da = mkView(d->img1d, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 2);
  }
  {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_D32_SFLOAT; ci.extent = {1, 1, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(h->device, &ci, nullptr, &d->depImg);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, d->depImg, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(h->device, &mai, nullptr, &d->depMem);
    vkBindImageMemory(h->device, d->depImg, d->depMem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = d->depImg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format; vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(h->device, &vi, nullptr, &d->vdep);
    VkSamplerCreateInfo cs = sci;
    cs.compareEnable = VK_TRUE; cs.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(h->device, &cs, nullptr, &d->cmpSampler);
  }
  h->dummies = d;
  return d;
}

void ImgBarrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t layers = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img; b.subresourceRange = {aspect, 0, 1, 0, layers};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

struct PomDrawState { uint8_t cls = 0; int handle = -1; bool push = false; };
static thread_local PomDrawState g_pomDraw;   // parallax: the first table texture bound to this draw's fragment stage (PrepareTextures), whether the pipeline is a parallax variant (GetPipeline)

// ---- pipelines ----------------------------------------------------------------------------------------------------------
struct RtInfo { VkFormat color[8]; int nColor; VkFormat depth; VkFormat stencil; int samples; };

// Normalised description of the pipeline a draw needs (stable across runs: stage cache keys, not program ids).
static void MakePipeDesc(PipeDesc& pd, DrawState& d, VkPrimitiveTopology topo, const RtInfo& rt) {
  ProgramImpl* p = d.prog;
  memset(&pd, 0, sizeof pd);
  pd.pad0 = (uint32_t)XlateAbVariant();   // SNAP_XLATE_AB: separate pipelines for the alternate translation (0 when unset)
  {   // pad1: bits 16..23 = RT shadow rays (M3), bits 0..7 = soft-shadow taps (P5), bit 7 = PCSS, bits 8..15 = shadow-distance factor * 4 (receivers remap the dynamic map's lookup, see ShadowDistanceOnBind)
    const uint32_t kc = ShadowDistK() > 1.0f ? (uint32_t)(ShadowDistK() * 4.0f + 0.5f) : 0u;
    const int rtR = snap_rt::ShadowRays();   // [rt] M3: ray-traced sun shadows replace the dynamic map's taps (bits 16..23 = rays per tap; the PCF bits stay 0)
    pd.pad1 = (p->st[1].valid && (ShadowFilterTaps() || kc || rtR) && ShadowFilterApplies(p->st[1])) ? (rtR ? ((uint32_t)std::min(rtR, 255) << 16) : ((uint32_t)ShadowFilterTaps() | (kc << 8) | (ShadowPcss() ? 0x80u : 0u))) : 0u;
  }   // SNAP_SHADOW_FILTER (P5): own pipeline keys for the patched fragment modules (0 when off: the original keys)
  for (int s = 0; s < 5; s++) if (p->st[s].valid) snprintf(pd.stage[s], sizeof pd.stage[s], "%s", p->st[s].key);
  pd.topo = (int)topo;
  pd.nattr = std::clamp(d.nattr, 0, 16);
  for (int i = 0; i < pd.nattr; i++) pd.attr[i] = d.attr[i];
  pd.nstream = std::clamp(d.nstream, 0, 16);
  for (int i = 0; i < pd.nstream; i++) pd.stream[i] = d.stream[i];
  pd.nColor = rt.nColor;
  for (int i = 0; i < rt.nColor; i++) {
    BlendSt b = d.blend[i]; b.target = 0; b.pad = 0;
    pd.blend[i] = b; pd.colorFmt[i] = (int32_t)rt.color[i];
  }
  pd.blendEn = d.color.blendEn; pd.chmask = d.chmask.mask;
  const int af = d.color.alphaFunc;
  const bool alpha = af >= 1 && af <= 7 && !p->st[1].spv.empty() && p->st[1].valid;
  pd.alphaFunc = alpha ? af : 0;
  pd.alphaRef = (alpha && af != 1) ? d.alphaRef : 0.0f;
  pd.patchSize = topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? d.patchSize : 0;
  pd.ms.samples = d.ms.samples; pd.ms.a2c = d.ms.a2c;
  pd.poly = d.poly; pd.dsBits = d.ds.bits;
  pd.depthFmt = (int32_t)rt.depth; pd.stencilFmt = (int32_t)rt.stencil; pd.samples = rt.samples;
  // the effective multisample count (render target samples win over the multisample state), as the pipeline uses it
  if (!(rt.samples > 1)) pd.samples = d.ms.samples > 1 ? d.ms.samples : 1;
  {   // pad2 bit 0: hashed / covered alpha edge for alpha-tested scene draws (SNAP_FOLIAGE_AA, nvn_foliage.cpp); 0 = the game's own test
    DsSt dsv; dsv.bits = d.ds.bits;
    pd.pad2 = FoliagePipeBits(pd.alphaFunc, pd.alphaRef, pd.nColor ? pd.colorFmt[0] : 0, pd.depthFmt, DsGet(dsv, DS_DT, 1) != 0, pd.nColor, (pd.blendEn & 1u) != 0, (int)DsGet(dsv, DS_DF, 4), DsGet(dsv, DS_DW, 1) != 0);
  }
}

// 1 for an unsigned-integer colour format, 2 for a signed one, else 0 (used to bitcast float fragment outputs, PatchIntOutputs).
static int IntColorKind(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT: case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
  }
}

// ---- graphics pipeline libraries (setting gfx_gpl, VK_EXT_graphics_pipeline_library) --------------------------------------------------------------
// A pipeline the game needs now but the driver has not compiled yet (not in the VkPipelineCache) is fast-linked from four libraries instead:
// vertex input, pre-raster (vertex stage + raster state), fragment shader (+ depth/stencil) and fragment output (blend + formats). Each library is built
// once per distinct input and shared by every pipeline that has it, so a new pipeline that only differs in blend or depth state links in about a millisecond.
// The caller then compiles the whole pipeline in the background (CreateGraphicsPipeline, filling the VkPipelineCache) and swaps it in; the draw never waits.
// Library keys are built from explicit fields (no struct padding), so equal state always finds the same library.
struct GplKey {
  std::string s;
  template <class T> void add(const T& v) { s.append(reinterpret_cast<const char*>(&v), sizeof v); }
  void stage(const VkPipelineShaderStageCreateInfo& si) {
    add(si.stage); add(si.module);
    if (si.pSpecializationInfo) { add(si.pSpecializationInfo->dataSize); s.append(static_cast<const char*>(si.pSpecializationInfo->pData), si.pSpecializationInfo->dataSize); } else add((size_t)0);
    add((int)(si.pNext != nullptr));   // the required-subgroup-size pin (the only thing hung on a stage)
  }
};
struct GplCache {
  std::mutex mu;
  std::unordered_map<std::string, VkPipeline> lib[4];   // 0 vertex input, 1 pre-raster, 2 fragment shader, 3 fragment output
  std::atomic<uint32_t> built{0}, reused{0}, links{0}, linkFail{0}, hits{0};
  std::atomic<uint64_t> linkNs{0}, libNs{0};
};
static GplCache& Gpl() { static GplCache* g = new GplCache; return *g; }   // never destroyed: the pipelines are freed with the device

// Builds (or finds) library `kind`; `gi` is the create info with its state pointers set. Returns VK_NULL_HANDLE on failure.
static VkPipeline GplGetLib(Host* h, int kind, const GplKey& key, VkGraphicsPipelineCreateInfo gi, VkGraphicsPipelineLibraryFlagsEXT flag) {
  GplCache& g = Gpl();
  { std::lock_guard<std::mutex> lk(g.mu); auto it = g.lib[kind].find(key.s); if (it != g.lib[kind].end()) { g.reused++; return it->second; } }
  VkGraphicsPipelineLibraryCreateInfoEXT lci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
  lci.flags = flag; lci.pNext = const_cast<void*>(gi.pNext);
  gi.pNext = &lci; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
  const uint64_t t0 = snap_perf::now_ns();
  VkPipeline p = VK_NULL_HANDLE;
  // No VkPipelineCache here: the libraries are only a stand-in until the whole pipeline is compiled, so they stay out of the saved cache blob.
  if (vkCreateGraphicsPipelines(h->device, VK_NULL_HANDLE, 1, &gi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
  g.libNs += snap_perf::now_ns() - t0;
  std::lock_guard<std::mutex> lk(g.mu);
  auto ins = g.lib[kind].emplace(key.s, p);
  if (!ins.second) { vkDestroyPipeline(h->device, p, nullptr); g.reused++; return ins.first->second; }   // another worker built the same library first
  g.built++;
  return p;
}

// Builds the Vulkan graphics pipeline for `pd` from stage objects st[0..4] and `layout`. Safe to call from any thread.
// `optLater` != null (a pipeline the game waits for) with gfx_gpl on: the driver's cache is asked first (FAIL_ON_PIPELINE_COMPILE_REQUIRED, so a hit costs what it
// always did); on a miss the pipeline is fast-linked from libraries and *optLater is set: the caller must compile the whole pipeline later and replace this one.
VkPipeline CreateGraphicsPipelineImpl(Host* h, const PipeDesc& pd, StageProg* const st[6], VkPipelineLayout layout, bool* optLater) {
  static std::mutex fragMu;   // StageProg::fragMods is shared by every pipeline built from the same fragment stage
  auto valid = [&](int s) { return st[s] && st[s]->valid; };
  const VkPrimitiveTopology topo = (VkPrimitiveTopology)pd.topo;
  const int af = pd.alphaFunc;
  const bool alpha = af != 0;

  // Dual-source blending: attachment 0 uses a SRC1 factor and the fragment shader has a second color output to feed it. The
  // translator knows nothing about it, so the two lowest outputs are re-decorated as Location L Index 0/1 (PatchFragmentOutputs).
  // Without a second output, or on any other attachment (Vulkan allows only maxFragmentDualSrcAttachments), SRC1 degrades to SRC.
  auto usesSrc1 = [&](const BlendSt& b) {
    for (int f : {b.srcRgb, b.dstRgb, b.srcA, b.dstA}) if (f >= BF_SRC1_COLOR && f <= BF_ONE_MINUS_SRC1_ALPHA) return true;
    return false;
  };
  const bool dual = pd.nColor > 0 && ((pd.blendEn >> 0) & 1) && usesSrc1(pd.blend[0]) && h->features.dualSrcBlend &&
                    h->props.limits.maxFragmentDualSrcAttachments >= 1 && valid(1) && st[1]->outLocs.size() >= 2;
  std::vector<VkPipelineShaderStageCreateInfo> stages;
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sgPin[6];   // storage for PinSubgroup32 (stages hold a pointer to it)
  float alphaRefVal = pd.alphaRef;
  VkSpecializationMapEntry alphaMap{0, 0, 4};
  VkSpecializationInfo alphaSpec{};
  float swayVals[4] = {};   // SNAP_WIND_SWAY (nvn_foliage.cpp): vertex wrapper amplitude and debug time offset, specialization constants 0 and 1 of the patched vertex module
  const VkSpecializationMapEntry swayMap[4] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4}};
  VkSpecializationInfo swaySpec{};
  for (int s : {0, 3, 4, 2, 1}) {
    if (!valid(s)) continue;
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    static const VkShaderStageFlagBits bits[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    si.stage = bits[s]; si.module = st[s]->mod; si.pName = "main";
    PinSubgroup32(&si, &sgPin[s], st[s]->mod);
    if (s == 0 && ((pd.pad2 >> 17) & 15u) != 0) {   // SNAP_WIND_SWAY: alpha-tested scene draws get the wind displacement wrapper (pad2 bits 17..20 = level)
      if (VkShaderModule sm = FoliageSwayModule(h, *st[0])) {
        swayVals[0] = ((pd.pad2 >> 24) & 1u) ? 0.0f : FoliageSwayAmpFor((int)((pd.pad2 >> 17) & 15u)); swayVals[1] = (float)((pd.pad2 >> 21) & 7u) * 0.35f; { static const bool stat = getenv("SNAP_WIND_STATIC") != nullptr; swayVals[2] = stat ? 0.0f : 1.0f; }   // amplitude, debug time offset
        { static const float zs = getenv("SNAP_WIND_ZSLACK") ? (float)atof(getenv("SNAP_WIND_ZSLACK")) : 0.0f; swayVals[3] = ((pd.pad2 >> 25) & 1u) ? zs : 0.0f; }  swaySpec.dataSize = 16; swaySpec.pData = swayVals; swaySpec.mapEntryCount = 4; swaySpec.pMapEntries = swayMap;
        si.module = sm; si.pSpecializationInfo = &swaySpec;
      }
    }
    if (s == 1) {
      StageProg& fp = *st[1];
      uint32_t intKindBits = 0; int intKinds[8] = {};   // [vkval] attachments with an integer format: 1 uint, 2 sint (2 bits each from bit 16)
      for (int i = 0; i < pd.nColor && i < 8; i++) { const int k = IntColorKind((VkFormat)pd.colorFmt[i]); intKinds[i] = k; intKindBits |= (uint32_t)k << (16 + 2 * i); }
      const bool useAlt = pd.pad0 != 0 && fp.modAlt != VK_NULL_HANDLE && !fp.spvAlt.empty();   // SNAP_XLATE_AB alternate translation
      const int pcfTaps = (intKindBits == 0 && !useAlt) ? (int)(pd.pad1 & 0xFFFFu) : 0;   // (pad1 = taps; the variant key below only needs "on")   // P5: soft shadow taps (nvn_shadowpcf.cpp), fragment color outputs only
      const int rtRays = (intKindBits == 0 && !useAlt) ? (int)((pd.pad1 >> 16) & 0xFFu) : 0;   // M3: RT sun shadows (nvn_shadowrt.cpp)
      const bool foliageHash = alpha && (pd.pad2 & 1) != 0;   // SNAP_FOLIAGE_AA: dithered edge (0x200) or alpha-to-coverage output (0x400, multisampled targets)
      const bool foliageA2c = foliageHash && pd.samples > 1;
      const int foliageLeaf = alpha ? (int)(((pd.pad2 >> 4) & 15u) | (((pd.pad2 >> 1) & 1u) << 4)) : 0;   // SNAP_LEAF_TRANSLUCENCY level 0..8 (vkey bits 11..14)
      const bool foliageCov = alpha && (pd.pad2 & 4u) != 0;   // SNAP_FOLIAGE_STABLE: alpha scaled with the mip level (vkey bit 49)
      const uint64_t vkey = (uint64_t)((uint32_t)(alpha ? af : 0) | ((uint32_t)pd.nColor << 4) | (dual ? 0x100u : 0u) | intKindBits | (useAlt ? 0x80000000u : 0u) | ((uint32_t)pcfTaps << 16) |
                            (foliageHash ? (foliageA2c ? 0x400u : 0x200u) : 0u) | ((uint32_t)foliageLeaf << 11)) | ((uint64_t)rtRays << 32) | ((uint64_t)((pd.pad2 >> 8) & 0x1FFu) << 40) | (foliageCov ? (1ull << 49) : 0ull);
      std::lock_guard<std::mutex> fl(fragMu);
      auto it = fp.fragMods.find(vkey);
      if (it == fp.fragMods.end()) {
        std::vector<uint32_t> cur = useAlt ? fp.spvAlt : fp.spv, tmp;
        bool alphaOk = false;
        if (alpha) {
          uint32_t lodVar = 0;
          if (foliageCov && PatchCaptureAlphaLod(cur, tmp, &lodVar)) cur = tmp;   // Foliage stability: the mip level the alpha texture is read at
          if ((foliageHash || foliageLeaf || lodVar) && PatchAlphaHash(cur, af, foliageA2c, foliageHash, foliageLeaf, tmp, lodVar)) { cur = tmp; alphaOk = true; }
          else if (PatchAlphaTest(cur, af, tmp)) { cur = tmp; alphaOk = true; }
          else LogError("alpha test (func %d): fragment shader has no float vec4 output at location 0, test ignored", af);
        }
        const VkShaderModule baseMod = useAlt ? fp.modAlt : fp.mod;
        VkShaderModule m = baseMod;
        const PcssParams pcssP = ShadowPcssParams();
        if (const uint32_t pb = (pd.pad2 >> 8) & 0x1FFu; pb && !rtRays && PatchPom(cur, (int)((pb >> 2) & 0x7Fu), (int)(pb & 3u), tmp)) { cur = tmp; m = VK_NULL_HANDLE; }   // parallax occlusion mapping (nvn_pom.cpp)
        if (rtRays && PatchShadowRt(cur, rtRays, tmp)) { cur = tmp; m = VK_NULL_HANDLE; }   // M3: ray queries instead of the map taps
        else if (pcfTaps && PatchShadowPcf(cur, std::max(1, pcfTaps & 0x7F), ShadowFilterRadius(), ShadowFilterBias(), (float)((pcfTaps >> 8) & 0xFF) / 4.0f, tmp, (pcfTaps & 0x80) ? &pcssP : nullptr)) { cur = tmp; m = VK_NULL_HANDLE; }
        if (PatchFragmentOutputs(cur, pd.nColor, dual, tmp)) cur = tmp, m = VK_NULL_HANDLE;
        if (intKindBits) {
          int skippedInt = 0;
          if (PatchIntOutputs(cur, intKinds, tmp, &skippedInt)) cur = tmp, m = VK_NULL_HANDLE;
          static bool loggedSkip = false;
          if (skippedInt && !loggedSkip) { loggedSkip = true; Log("fragment output(s) into an integer attachment are arrays or not 32-bit float: left as float (not bitcast)"); }
        }
        else if (alphaOk) m = VK_NULL_HANDLE;
        if (!m) {
          VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
          mi.codeSize = cur.size() * 4; mi.pCode = cur.data();
          if (vkCreateShaderModule(h->device, &mi, nullptr, &m) != VK_SUCCESS) { m = baseMod; alphaOk = false; LogError("fragment variant: vkCreateShaderModule failed"); }
          else { std::lock_guard<std::mutex> lk(h->pipeMutex); h->variantMods.push_back(m); }
        }
        it = fp.fragMods.emplace(vkey, std::make_pair(m, alphaOk)).first;
      }
      si.module = it->second.first;
      if (it->second.second) {
        alphaSpec.dataSize = 4; alphaSpec.pData = &alphaRefVal; alphaSpec.mapEntryCount = 1; alphaSpec.pMapEntries = &alphaMap;
        si.pSpecializationInfo = &alphaSpec;
      }
    }
    stages.push_back(si);
  }
  // vertex input: attribute i of BindVertexAttribState feeds shader location i
  std::vector<VkVertexInputBindingDescription> binds;
  std::vector<VkVertexInputAttributeDescription> attrs;
  bool have[16] = {};
  for (int i = 0; i < pd.nstream; i++) {
    binds.push_back({(uint32_t)i, pd.stream[i].stride, pd.stream[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    have[i] = true;
    if (pd.stream[i].divisor > 1) Log("vertex stream %d divisor %u: only 0/1 supported, using per-instance", i, pd.stream[i].divisor);
  }
  for (int i = 0; i < pd.nattr; i++) {
    uint32_t f = VaFmt(pd.attr[i]);
    if (!f) continue;
    if (i < 32 && valid(0) && !((st[0]->inputMask >> i) & 1)) continue;   // the vertex shader does not read this location
    const FormatInfo* fi = LookupFormat((int)f);
    if (!fi) { LogError("vertex attribute %d: format 0x%x not mapped", i, f); continue; }
    uint32_t sidx = VaStream(pd.attr[i]);
    if (!have[sidx]) { binds.push_back({sidx, 0, VK_VERTEX_INPUT_RATE_VERTEX}); have[sidx] = true; }
    attrs.push_back({(uint32_t)i, sidx, fi->vk, VaOffset(pd.attr[i])});
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = (uint32_t)binds.size(); vi.pVertexBindingDescriptions = binds.data();
  vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size(); vi.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topo;
  VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  ts.patchControlPoints = (uint32_t)std::clamp(pd.patchSize, 1, 32);
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = pd.poly.mode == POLY_LINE ? VK_POLYGON_MODE_LINE : pd.poly.mode == POLY_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
  rs.cullMode = (VkCullModeFlags)pd.poly.cull;   // NVNface NONE/FRONT/BACK/BOTH == the Vulkan bit values 0/1/2/3
  // NVN CCW/CW map 1:1 to Vulkan: with window origin upper-left both APIs define winding in y-down window space.
  rs.frontFace = pd.poly.front == FRONT_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  const int offBit = pd.poly.mode == POLY_POINT ? 1 : pd.poly.mode == POLY_LINE ? 2 : 4;
  rs.depthBiasEnable = (pd.poly.offsetEn & offBit) ? VK_TRUE : VK_FALSE;
  rs.lineWidth = 1.0f;
  VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
  if (g_provokingLast) { provoking.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT; rs.pNext = &provoking; }   // NVN/Maxwell flat shading
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = (VkSampleCountFlagBits)std::max(pd.samples, 1);
  ms.alphaToCoverageEnable = (pd.ms.a2c || ((pd.pad2 & 1) && pd.samples > 1 && alpha)) ? VK_TRUE : VK_FALSE;
  DsSt dsv; dsv.bits = pd.dsBits;
  VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dss.depthTestEnable = DsGet(dsv, DS_DT, 1); dss.depthWriteEnable = DsGet(dsv, DS_DW, 1);
  dss.depthCompareOp = Cmp(DsGet(dsv, DS_DF, 4));
  dss.stencilTestEnable = DsGet(dsv, DS_ST, 1);
  dss.front = {SOp(DsGet(dsv, DS_FAIL_F, 4)), SOp(DsGet(dsv, DS_ZPASS_F, 4)), SOp(DsGet(dsv, DS_ZFAIL_F, 4)), Cmp(DsGet(dsv, DS_SF_F, 4)), 0, 0, 0};
  dss.back = {SOp(DsGet(dsv, DS_FAIL_B, 4)), SOp(DsGet(dsv, DS_ZPASS_B, 4)), SOp(DsGet(dsv, DS_ZFAIL_B, 4)), Cmp(DsGet(dsv, DS_SF_B, 4)), 0, 0, 0};
  std::vector<VkPipelineColorBlendAttachmentState> cba(pd.nColor);
  for (int i = 0; i < pd.nColor; i++) {
    const BlendSt& b = pd.blend[i];
    cba[i].blendEnable = (pd.blendEn >> i) & 1;
    const bool src1Ok = dual && i == 0;
    auto bf = [&](int f) {   // SRC1 factors only where the second source exists; elsewhere they read as the matching SRC factor
      if (!src1Ok && f >= BF_SRC1_COLOR && f <= BF_ONE_MINUS_SRC1_ALPHA) f = f - BF_SRC1_COLOR + BF_SRC_COLOR;
      return BlendFactor(f);
    };
    cba[i].srcColorBlendFactor = bf(b.srcRgb); cba[i].dstColorBlendFactor = bf(b.dstRgb);
    cba[i].srcAlphaBlendFactor = bf(b.srcA); cba[i].dstAlphaBlendFactor = bf(b.dstA);
    cba[i].colorBlendOp = BlendOp(b.eqRgb); cba[i].alphaBlendOp = BlendOp(b.eqA);
    cba[i].colorWriteMask = (pd.chmask >> (4 * i)) & 0xF;   // r,g,b,a bits 0..3 == VK_COLOR_COMPONENT_R..A_BIT
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = (uint32_t)cba.size(); cb.pAttachments = cba.data();
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof *dyn); ds.pDynamicStates = dyn;
  VkFormat colorFmts[8];
  for (int i = 0; i < 8; i++) colorFmts[i] = (VkFormat)pd.colorFmt[i];
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = (uint32_t)pd.nColor; ri.pColorAttachmentFormats = colorFmts;
  ri.depthAttachmentFormat = (VkFormat)pd.depthFmt; ri.stencilAttachmentFormat = (VkFormat)pd.stencilFmt;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri;
  gi.stageCount = (uint32_t)stages.size(); gi.pStages = stages.data();
  gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
  gi.pTessellationState = topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? &ts : nullptr;
  gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &dss;
  gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = layout;
  VkPipeline pipe = VK_NULL_HANDLE;
  if (optLater && g_gplOn) {
    // 1) whole pipeline from the driver cache only: a hit is as fast as before and needs no later compile
    const uint64_t gT0 = snap_perf::on ? snap_perf::now_ns() : 0;
    auto account = [&](const char* what) {   // the same SNAP_PERF accounting as a whole-pipeline compile (pipe_ms, pipes_created, a "pipeline" event)
      if (!snap_perf::on) return;
      const uint64_t dt = snap_perf::now_ns() - gT0;
      snap_perf::add(snap_perf::C_PIPE, dt); snap_perf::count(snap_perf::V_PIPES_CREATED);
      char det[128]; snprintf(det, sizeof det, "gpl %s vs=%s fs=%s", what, pd.stage[0], pd.stage[1]);
      snap_perf::event("pipeline", (double)dt / 1e6, det);
    };
    gi.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    const VkResult cr = vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &pipe);
    gi.flags = 0;
    if (cr == VK_SUCCESS) { Gpl().hits++; account("cache-hit"); return pipe; }
    pipe = VK_NULL_HANDLE;
    if (cr == VK_PIPELINE_COMPILE_REQUIRED) {
      // 2) a miss: fast-link the four libraries
      const uint64_t t0 = snap_perf::now_ns();
      GplCache& g = Gpl();
      std::vector<VkPipelineShaderStageCreateInfo> preStages, fsStages;
      for (const auto& si : stages) (si.stage == VK_SHADER_STAGE_FRAGMENT_BIT ? fsStages : preStages).push_back(si);
      GplKey kVi, kPr, kFs, kFo;
      for (const auto& b : binds) { kVi.add(b.binding); kVi.add(b.stride); kVi.add(b.inputRate); }
      kVi.add((int)-1);
      for (const auto& a : attrs) { kVi.add(a.location); kVi.add(a.binding); kVi.add(a.format); kVi.add(a.offset); }
      kVi.add((int)topo);
      for (const auto& si : preStages) kPr.stage(si);
      kPr.add(layout); kPr.add((int)rs.polygonMode); kPr.add((uint32_t)rs.cullMode); kPr.add((int)rs.frontFace); kPr.add((uint32_t)rs.depthBiasEnable); kPr.add((int)(rs.pNext != nullptr));
      kPr.add(topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? ts.patchControlPoints : 0u);
      for (const auto& si : fsStages) kFs.stage(si);
      kFs.add(layout); kFs.add((uint32_t)dss.depthTestEnable); kFs.add((uint32_t)dss.depthWriteEnable); kFs.add((int)dss.depthCompareOp); kFs.add((uint32_t)dss.stencilTestEnable);
      for (const VkStencilOpState* so : {&dss.front, &dss.back}) { kFs.add((int)so->failOp); kFs.add((int)so->passOp); kFs.add((int)so->depthFailOp); kFs.add((int)so->compareOp); }
      kFs.add((int)ms.rasterizationSamples); kFs.add((uint32_t)ms.alphaToCoverageEnable); kFs.add((int)ri.depthAttachmentFormat); kFs.add((int)ri.stencilAttachmentFormat);
      kFo.add((int)ms.rasterizationSamples); kFo.add((uint32_t)ms.alphaToCoverageEnable); kFo.add((int)ri.depthAttachmentFormat); kFo.add((int)ri.stencilAttachmentFormat);
      kFo.add(cb.attachmentCount);
      for (uint32_t i = 0; i < cb.attachmentCount; i++) {
        const auto& a = cba[i];
        kFo.add((int)colorFmts[i]); kFo.add((uint32_t)a.blendEnable); kFo.add((int)a.srcColorBlendFactor); kFo.add((int)a.dstColorBlendFactor); kFo.add((int)a.colorBlendOp);
        kFo.add((int)a.srcAlphaBlendFactor); kFo.add((int)a.dstAlphaBlendFactor); kFo.add((int)a.alphaBlendOp); kFo.add((uint32_t)a.colorWriteMask);
      }
      VkGraphicsPipelineCreateInfo lg{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      lg.layout = layout; lg.pDynamicState = &ds;
      VkGraphicsPipelineCreateInfo gVi = lg, gPr = lg, gFs = lg, gFo = lg;
      gVi.pVertexInputState = &vi; gVi.pInputAssemblyState = &ia;
      gPr.stageCount = (uint32_t)preStages.size(); gPr.pStages = preStages.data(); gPr.pViewportState = &vp; gPr.pRasterizationState = &rs;
      gPr.pTessellationState = topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? &ts : nullptr;
      gFs.stageCount = (uint32_t)fsStages.size(); gFs.pStages = fsStages.data(); gFs.pDepthStencilState = &dss; gFs.pMultisampleState = &ms; gFs.pNext = &ri;
      gFo.pColorBlendState = &cb; gFo.pMultisampleState = &ms; gFo.pNext = &ri;
      const VkPipeline lVi = GplGetLib(h, 0, kVi, gVi, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
      const VkPipeline lPr = lVi ? GplGetLib(h, 1, kPr, gPr, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) : VK_NULL_HANDLE;
      const VkPipeline lFs = lPr ? GplGetLib(h, 2, kFs, gFs, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) : VK_NULL_HANDLE;
      const VkPipeline lFo = lFs ? GplGetLib(h, 3, kFo, gFo, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) : VK_NULL_HANDLE;
      if (lFo) {
        const VkPipeline libs[4] = {lVi, lPr, lFs, lFo};
        VkPipelineLibraryCreateInfoKHR li{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
        li.libraryCount = 4; li.pLibraries = libs;
        VkGraphicsPipelineCreateInfo link{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        link.pNext = &li; link.layout = layout;   // no LINK_TIME_OPTIMIZATION flag: a fast link
        if (vkCreateGraphicsPipelines(h->device, VK_NULL_HANDLE, 1, &link, nullptr, &pipe) == VK_SUCCESS) {
          const uint64_t dt = snap_perf::now_ns() - t0;
          g.links++; g.linkNs += dt;
          account("link");
          *optLater = true;
          return pipe;
        }
        pipe = VK_NULL_HANDLE;
      }
      g.linkFail++;
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) LogError("gpl: fast link failed, building whole pipelines (further failures are counted, not logged)");
    }
    // 3) anything else: the normal whole-pipeline compile below
  }
  const uint64_t perfT0 = snap_perf::on ? snap_perf::now_ns() : 0;
  if (vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &pipe) != VK_SUCCESS) {
    LogError("vkCreateGraphicsPipelines failed");
    return VK_NULL_HANDLE;
  }
  if (snap_perf::on) {
    const uint64_t dt = snap_perf::now_ns() - perfT0;
    snap_perf::add(snap_perf::C_PIPE, dt); snap_perf::count(snap_perf::V_PIPES_CREATED);
    char det[128]; snprintf(det, sizeof det, "gfx vs=%s fs=%s", pd.stage[0], pd.stage[1]);
    snap_perf::event("pipeline", (double)dt / 1e6, det);
  }
  return pipe;
}

// SNAP_PIPELINE_DEFER=1: see GetPipeline. True when the draw's target is the main scene colour (RGBA16F 1920x1080, not a display image) or scene depth
// (DEPTH32F 1920x1080); the photo (360x240), Evaluation (DEPTH32F_STENCIL8) and UI targets are never matched.
static bool PipelineDeferScene(const ReplayCtx& cx) {
  static const bool on = [] { const char* e = getenv("SNAP_PIPELINE_DEFER"); return e && *e == '1'; }();
  if (!on) return false;
  const TextureImpl* t = cx.rt[0] ? cx.rt[0] : cx.rt[8];
  if (!t || t->b.width != 1920 || t->b.height != 1080 || t->b.target != TT_2D || t->b.samples > 1) return false;
  return (t->b.format == FMT_RGBA16F && !(t->b.flags & TF_DISPLAY)) || t->b.format == FMT_DEPTH32F;
}

VkPipeline GetPipeline(ReplayCtx& cx, DrawState& d, VkPrimitiveTopology topo, const RtInfo& rt) {
  Host* h = cx.h;
  snap_perf::Scope perfGet(snap_perf::C_GETPIPE);
  PipeDesc pd;
  MakePipeDesc(pd, d, topo, rt);
  g_pomDraw.push = false;
  if (g_pomDraw.cls && !((pd.pad1 >> 16) & 0xFFu)) {   // parallax (not together with RT shadows: both use the fragment push block)
    pd.pad2 |= PomPipeBits(cx.rt[0], PomLevel(), g_pomDraw.handle, g_pomDraw.cls);
    g_pomDraw.push = ((pd.pad2 >> 8) & 0x1FFu) != 0;
  }
  bool pending = false;
  // A draw into a target that is not rendered every frame (photo thumbnail, evaluation/capture targets, one-shot passes) cannot be skipped
  // while its pipeline compiles: nothing redraws it, so the result would stay empty. Only per-frame targets (3+ consecutive frames) may skip.
  bool mustHave = true;
  if (TextureImpl* rt0 = cx.rt[0] ? cx.rt[0] : cx.rt[8]) {
    const int now = g_presentCount;
    if (rt0->lastDrawPresent != now) { rt0->drawStreak = rt0->lastDrawPresent == now - 1 ? rt0->drawStreak + 1 : 1; rt0->lastDrawPresent = now; }
    mustHave = rt0->drawStreak < 3;
  }
  if (!mustHave && PipelineShutterWindow()) mustHave = true;   // a photo is being taken: no skipped draws
  // SNAP_PIPELINE_DEFER=1 (off by default): a draw into the game's main scene colour or depth target (RGBA16F / DEPTH32F, 1920x1080 guest size: redrawn every
  // frame, never a photo, Evaluation or UI target) that is only must-have because the target is in its first two frames is skipped like any per-frame draw,
  // and its pipeline compiles on the worker pool. Not during the shutter window. Costs a missing object for a frame or two after a course load; saves the
  // 20-110 ms inline compile on the replay thread (analysis/perf/results.md, hitch sweep).
  if (mustHave && PipelineDeferScene(cx) && !PipelineShutterWindow()) mustHave = false;
  const uint64_t dbgT0 = snap_perf::now_ns();   // DEFER-DEBUG (temporary)
  VkPipeline pipe = PipelineAcquire(h, pd, d.prog, &pending, mustHave);   // nvn_pipeline.cpp: ready map, inline or background compile, key recording
  { static const bool dl = getenv("SNAP_PIPELINE_DEFER_LOG") != nullptr; const double ms = (snap_perf::now_ns() - dbgT0) / 1e6;   // DEFER-DEBUG (temporary)
    if (dl && ms > 4.0) { const TextureImpl* t = cx.rt[0] ? cx.rt[0] : cx.rt[8]; fprintf(stderr, "[defer-dbg] p%d inline %.1f ms must %d tgt %s %ux%u fmt %d tgt %d samples %d flags %x streak %d vs=%s fs=%s\n", g_presentCount, ms, (int)mustHave, cx.rt[0] ? "rt0" : "rt8", t ? t->b.width : 0, t ? t->b.height : 0, t ? (int)t->b.format : -1, t ? (int)t->b.target : -1, t ? (int)t->b.samples : 0, t ? (unsigned)t->b.flags : 0, t ? t->drawStreak : -1, pd.stage[0], pd.stage[1]); } }
  if (!pipe && pending) { snap_perf::count(snap_perf::V_PIPES_SKIPPED); LifeCount(LS_PIPE_SKIP); }   // [nvnfix] SNAP_NVN_STATS
  return pipe;
}

// ---- descriptors ----------------------------------------------------------------------------------------------------------
VkDescriptorSet AllocSet(ReplayCtx& cx, VkDescriptorSetLayout layout) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (cx.pools->empty() || attempt == 1) {
      const VkDescriptorPool np = AcquireDescriptorPool(cx.q);
      if (!np) break;   // [nvnfix] creation failed (logged): no null pool in the list (Reap resets every entry)
      cx.pools->push_back(np);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cx.pools->back(); ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(cx.h->device, &ai, &set) == VK_SUCCESS) return set;
  }
  LogError("descriptor set allocation failed");
  return VK_NULL_HANDLE;
}

struct ResolvedTex {
  bool shadowDummy = false;   // a depth-compare binding served by the 1.0 depth stand-in
  TextureImpl* t = nullptr; VkImageView view = VK_NULL_HANDLE; VkSampler samp = VK_NULL_HANDLE;
  SubRange range{};        // subresources the view covers (what must be in the sampled layout)
  bool feedback = false;   // the same subresource is also an attachment of this draw (handled by a copy)
};

// Removes the subresources a draw renders to from a sampled range. Returns false when nothing is left.
bool ExcludeAttachment(SubRange& rg, const SubRange& a) {
  if (!rg.Overlaps(a)) return true;
  // levels first (a mip-chain pass samples the level above the one it renders to), then layers
  if (rg.lb < a.lb) { rg.ln = a.lb - rg.lb; return true; }
  if (rg.lb + rg.ln > a.lb + 1) { const uint32_t e = rg.lb + rg.ln; rg.lb = a.lb + 1; rg.ln = e - rg.lb; return true; }
  const uint32_t rEnd = rg.yb + rg.yn, aEnd = a.yb + a.yn;
  if (rg.yb < a.yb) { rg.yn = a.yb - rg.yb; return true; }
  if (rEnd > aEnd) { rg.yb = aEnd; rg.yn = rEnd - aEnd; return true; }
  return false;
}

// SNAP_MODS_AB=<P> (test aid, off when unset): a texture replaced by a mod pack is paired once, on first use and before its
// upload, with an image built from the original file (runtime/mods/mods.cpp finds it by comparing the guest bytes with the
// replacement). While (present count / P) is odd the draw samples the original image, otherwise the replacement.
extern "C" unsigned snap_mods_ab_period();
extern "C" bool snap_mods_ab_live(unsigned present);   // true while the original should show (period parity, or F8)
extern "C" const uint8_t* snap_mods_ab_original(const uint8_t* src, uint32_t w, uint32_t h, uint32_t mips, uint32_t* ow, uint32_t* oh, uint32_t* om);
TextureImpl* AbSubstitute(ReplayCtx&, TextureImpl* t) {
  static const unsigned P = snap_mods_ab_period();
  if (!P) return t;
  if (!t->abTried && !t->uploaded) {
    t->abTried = true;
    const TextureBuilder& b = t->b;
    const uint8_t* src = static_cast<const uint8_t*>(b.packaged);
    if (!src && b.pool && !(b.flags & (TF_COMPRESSIBLE | TF_DISPLAY | TF_LINEAR_RENDER_TARGET | TF_LINEAR)))
      if (auto* p = Impl<PoolImpl>(b.pool, MAGIC_POOL)) if (p->storage) src = static_cast<const uint8_t*>(p->storage) + b.poolOffset;
    uint32_t ow = 0, oh = 0, om = 0;
    if (src && t->scale == 1.0f && b.samples <= 1 && b.target == TT_2D) {
      if (const uint8_t* orig = snap_mods_ab_original(src, (uint32_t)b.width, (uint32_t)b.height, (uint32_t)b.levels, &ow, &oh, &om)) {
        auto* s = new TextureImpl();
        s->host = t->host; s->fi = t->fi; s->b = b;
        s->b.width = (int)ow; s->b.height = (int)oh; s->b.levels = (int)om; s->b.pool = nullptr; s->b.poolOffset = 0; s->b.packaged = orig;
        t->abAlt = s;
        static int n = 0;
        if (n++ < 6 || n % 50 == 0) Log("[mods-ab] pair %d: texture %p %dx%d (replacement) <-> %ux%u (original)", n, (void*)t, b.width, b.height, ow, oh);
      }
    }
  }
  return (t->abAlt && snap_mods_ab_live((unsigned)g_presentCount)) ? t->abAlt : t;   // SNAP_MODS_AB=key: F8 toggles
}

ResolvedTex ResolveHandle(ReplayCtx& cx, DrawState& d, uint64_t handle, bool image, int shaderType) {
  ResolvedTex r;
  if (!d.tp) return r;
  const int texId = (int)(handle & 0xFFFFF), sampId = (int)(handle >> 20);
  const TexSlot* slotp = d.tp->slots.Find(texId);   // lock-free: loader threads register slots during replay (see SlotTable)
  if (!slotp) return r;
  struct { TextureImpl* tex; TexView view; bool hasView; } slot{};
  for (;;) {   // sequence-lock read (TexSlot::seq): retry while a registration is rewriting this slot
    const uint32_t a = slotp->seq.load(std::memory_order_acquire);
    if (a & 1) { YieldProcessor(); continue; }
    slot.tex = slotp->tex;
    slot.hasView = slotp->hasView;
    if (slot.hasView) slot.view = slotp->view;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slotp->seq.load(std::memory_order_relaxed) == a) break;
  }
  if (!slot.tex) return r;
  r.t = slot.tex;
  TextureImpl* const up = image ? nullptr : UpscaleSubstitute(cx, r.t);   // [upscale] the final pass reads the upscaled scene
  if (up) r.t = up;
  if (!image && !up) r.t = AbSubstitute(cx, r.t);   // SNAP_MODS_AB test aid
  if (!image && !up) r.t = WideCropSubstitute(cx, r.t);   // [wide] a 16:9 target reads the middle of an ultrawide image
  if (!image && !up && ShinyActive()) {   // shiny mod (nvn_shiny.cpp)
    ShinyDraw sd; sd.vb0 = d.vbAddr[0]; sd.vb1 = d.vbAddr[1];
    for (int i = 0; i < 8; i++) { sd.uboPool[i] = d.ubo[0][i].pool; sd.uboOff[i] = d.ubo[0][i].off; sd.uboRange[i] = d.ubo[0][i].range; }
    sd.rtW = cx.rt[0] ? cx.rt[0]->b.width : 0; sd.rtH = cx.rt[0] ? cx.rt[0]->b.height : 0; sd.prog = d.prog ? (int)d.prog->id : -1; sd.rt0 = cx.rt[0]; if (cx.scHave & 1) { for (int k = 0; k < 4; k++) sd.sc[k] = cx.scGuest[0][k]; sd.scValid = sd.sc[2] > 0 && sd.sc[3] > 0; }
    { PoolImpl* vp = nullptr; uint64_t vo = 0; if (d.vbAddr[0] && r.t->host && r.t->host->ResolveAddress(d.vbAddr[0], &vp, &vo) && vp && vp->storage) sd.vbf = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vp->storage) + vo); }
    r.t = ShinySubstitute(r.t, sd);
  }
  if (!image && !up && BtnIconActive()) {   // button icon mod (nvn_btnicon.cpp): UI glyph sprites
    BtnDraw bd; bd.rtW = cx.rt[0] ? cx.rt[0]->b.width : 0; bd.rtH = cx.rt[0] ? cx.rt[0]->b.height : 0; bd.prog = d.prog ? (int)d.prog->id : -1;
    PoolImpl* vp = nullptr; uint64_t vo = 0;
    if (d.vbAddr[0] && r.t->host && r.t->host->ResolveAddress(d.vbAddr[0], &vp, &vo) && vp && vp->storage && vo + 4 * 14 * 4 <= vp->size)
      bd.vbf = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vp->storage) + vo);
    r.t = BtnIconSubstitute(r.t, bd);
  }
  bool hdSub = false;
  if (!image && !up && HdPhotoOn() && cx.rt[0] && cx.rt[0]->b.width == 1920) {   // HD photos (nvn_hdphoto.cpp): a photo drawn large in the UI
    float drawnW = 0;
    PoolImpl* vp = nullptr; uint64_t vo = 0;
    if (d.vbAddr[0] && r.t->host && r.t->host->ResolveAddress(d.vbAddr[0], &vp, &vo) && vp && vp->storage && vo + 4 * 14 * 3 <= vp->size) {
      const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vp->storage) + vo);   // UI sprite vertices, 14 floats each (as nvn_shiny.cpp)
      drawnW = std::max(std::fabs(f[14 * 2] - f[0]), std::fabs(f[14] - f[0]));
      if (!(drawnW < 4096.0f)) drawnW = 0;
    }
    if (TextureImpl* hs = HdPhotoSubstitute(r.t, drawnW, (float)cx.rt[0]->HW() / 1920.0f)) { r.t = hs; hdSub = true; }
  }
  if (!image && !up && !hdSub) PomNoteTexture(r.t);   // parallax class by content key, before the first upload
  if (!r.t->EnsureImage()) return r;
  const TexView* tv = (slot.hasView && !up && !hdSub) ? &slot.view : nullptr;
  r.range = r.t->RangeOf(tv);
  if (!image) {
    for (int i = 0; i < 9; i++) {
      if (cx.rt[i] != r.t) continue;
      const SubRange a = RtRange(r.t, cx.rtHasView[i] ? &cx.rtView[i] : nullptr);
      if (!ExcludeAttachment(r.range, a)) r.feedback = true;
    }
  }
  if (!image && (shaderType & 0x400) && (r.t->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
    // depth-compare sampling of a color-format texture (the game binds a small R32F stand-in for "no shadow map"): Vulkan has
    // no compare sampling on color formats, so the draw gets a 1.0 depth stand-in (everything lit)
    static int logged = 0;
    if (logged++ < 4) Log("shadow-compare binding on color texture %p (%dx%d, format 0x%x): using the depth stand-in", (void*)r.t, r.t->b.width, r.t->b.height, r.t->b.format);
    r.t = nullptr;
    r.shadowDummy = true;
    return r;
  }
  if (!r.feedback) r.view = r.t->View(tv, !image, shaderType, &r.range);
  if (!image && d.sp) {
    SamplerImpl* const* s = d.sp->slots.Find(sampId);
    if (s && *s) r.samp = SamplerLive(*s);   // a destroyed sampler can leave a null slot
  }
  return r;
}
int TexBindIndex(const MTex& t) { return t.handleIndex >= 8 ? (t.handleIndex - 8) / 2 : t.handleIndex; }

VkImageLayout SampledLayout(const TextureImpl* t) {
  return (t->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

void BeginRendering(ReplayCtx& cx, const RtInfo& rt) {
  snap_perf::Scope perfScope(snap_perf::C_RENDBEGIN);   // [perf] replay sub-scope
  for (int i = 0; i < 9; i++) WideMarkDrawn(cx.rt[i]);   // [wide] crop stand-ins of these targets need a fresh copy
  VkRenderingAttachmentInfo col[8] = {};
  VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stc{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  uint32_t w = 0, hgt = 0, layerCount = 1;
  for (int i = 0; i < rt.nColor; i++) {
    TextureImpl* t = cx.rt[i];
    col[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (!t) continue;
    t->uploaded = true;
    SubRange rr;
    col[i].imageView = t->RenderView(cx.rtHasView[i] ? &cx.rtView[i] : nullptr, &rr, true);   // the mip level / layer (and sRGB/UNORM format) the game selected
    TransitionRange(cx.cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
    col[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (!w) { w = t->LevelW(rr.lb); hgt = t->LevelH(rr.lb); layerCount = rr.yn; }
  }
  TextureImpl* dt = cx.rt[8];
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  if (dt) {
    dt->uploaded = true;
    SubRange rr;
    dep.imageView = stc.imageView = dt->RenderView(cx.rtHasView[8] ? &cx.rtView[8] : nullptr, &rr);
    TransitionRange(cx.cb, dt, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, rr);
    dep.imageLayout = stc.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    dep.loadOp = stc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; dep.storeOp = stc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (!w) { w = dt->LevelW(rr.lb); hgt = dt->LevelH(rr.lb); layerCount = rr.yn; }
    if (dt->fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &dep;
    if (dt->fi->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stc;
  }
  ri.renderArea = {{0, 0}, {w, hgt}};
  ri.layerCount = layerCount;
  ri.colorAttachmentCount = (uint32_t)rt.nColor; ri.pColorAttachments = col;
  if (g_perfPassOn) {
    char lb[96];
    snprintf(lb, sizeof lb, "rp %ux%u c%d f%x%s%s t%x", w, hgt, rt.nColor, cx.rt[0] ? cx.rt[0]->b.format : 0, dt ? " z" : "", dt ? "" : "", (unsigned)(((uintptr_t)(cx.rt[0] ? (void*)cx.rt[0] : (void*)cx.rt[8]) >> 6) & 0xfff));
    PerfMark(cx.h, cx.cb, lb);
  }
  vkCmdBeginRendering(cx.cb, &ri);
  cx.rendering = true;
  for (int i = 0; i < 8; i++) if (cx.bcDst[i]) cx.bcDirty = true;
  if (cx.h->wantSamples && cx.h->occPool) {   // samples-passed counter: one occlusion query per rendering scope
    const uint32_t qi = cx.h->occNext++ % 8192;
    vkResetQueryPool(cx.h->device, cx.h->occPool, qi, 1);
    vkCmdBeginQuery(cx.cb, cx.h->occPool, qi, cx.h->features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    cx.occOpen = true;
  }
  if (!cx.vpSet) { VkViewport v = MakeNvnViewport(cx.h->originMode, 0, 0, (float)w, (float)hgt); vkCmdSetViewport(cx.cb, 0, 1, &v); cx.vpSet = true; }
  if (!cx.scSet) { VkRect2D r{{0, 0}, {w, hgt}}; vkCmdSetScissor(cx.cb, 0, 1, &r); cx.scSet = true; }
}

BufBind ResolveBuf(Host* h, uint64_t addr, uint64_t size, uint64_t cap) {
  BufBind b;
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!addr) return b;   // address 0 = nothing bound (the game binds it for unused slots); the dummy descriptors cover it
  if (!h->ResolveAddress(addr, &pool, &off)) { LogError("buffer address 0x%llx is not in any memory pool", (unsigned long long)addr); return b; }
  if (!PoolBacked(pool)) return b;   // [audit F6] acquire: buffer, memory and mapped are visible together
  uint64_t n = std::min<uint64_t>(size ? size : pool->size - off, pool->size - off);
  // A fragment-shader write-back into this pool is still in flight (photo evaluation counts): land it in guest memory first, or the
  // sync below would copy the stale guest bytes over the GPU's result before the game has read it.
  if (pool->wbPending.load(std::memory_order_relaxed) > 0) FlushReadbacks(h, true);
  SyncPoolRangeOnce(pool, off, n);
  b.buf = pool->buffer; b.off = off; b.range = cap ? std::min(n, cap) : n; b.pool = pool;
  return b;
}

struct TexWrite { int set, binding, idx; ResolvedTex r; bool image; int stype; };

// [cpuprof 2026-11-13] Per-draw scratch vectors reused per thread: PrepareTextures / WriteDescriptors / DoDraw built fresh vectors and
// deques for every draw (malloc + free were ~5% of the replaying JobWorker in the sampling profile). A nested call (re-entry on the same
// thread) gets its own vector. SNAP_REPLAY_SCRATCH=0 restores a fresh vector per call.
static const bool kReplayScratch = [] { const char* e = getenv("SNAP_REPLAY_SCRATCH"); return !(e && *e == '0'); }();
template <class T> struct ScratchVec {
  static thread_local std::vector<T> tl;
  static thread_local bool busy;
  std::vector<T> own;
  const bool useTl;
  ScratchVec() : useTl(kReplayScratch && !busy) { if (useTl) { busy = true; tl.clear(); } }
  ~ScratchVec() { if (useTl) { tl.clear(); busy = false; } }
  ScratchVec(const ScratchVec&) = delete;
  std::vector<T>& get() { return useTl ? tl : own; }
};
template <class T> thread_local std::vector<T> ScratchVec<T>::tl;
template <class T> thread_local bool ScratchVec<T>::busy = false;
struct PrepWant { TextureImpl* t; VkImageLayout layout; SubRange range; };   // PrepareTextures: a layout a sampled image must be in

// ---- photo evaluation under render scale ----------------------------------------------------------------------------------
// EvaluationCopyDepthPS copies the photo's scene depth into the 1920x1080 Evaluation target (exempt from render scale) with
// texture(DepthTexture, uv). With SNAP_RENDER_SCALE=2 that depth texture is 3840x2160, so a 1920 pixel center lands between four
// samples and nearest sampling returns one taken 1/4 pixel away. The subjects re-drawn at 1920 then fail their GEQUAL test against
// their own surface on about half their pixels, so visibility dropped (Florio, 2026-11-12: median visible fraction 0.27 vs 0.65-0.94
// at scale 1, no fully visible subject) and photos scored lower or became unselectable. When the source is scaled and the target is
// not, this replacement takes the farthest (minimum, reversed Z) of the 2x2 footprint instead (textureGather), so a subject's own
// surface always passes and an occluder still blocks. Scale 1 keeps the game's shader. NVN_NO_EVAL_DEPTH_FIX=1 disables it.
// GLSL (glslangValidator -V, Vulkan 1.1): layout(set=2,binding=32) uniform sampler2D DepthTexture; layout(location=1) in vec4 uv;
//   void main() { vec4 g = textureGather(DepthTexture, uv.xy, 0); gl_FragDepth = min(min(g.x, g.y), min(g.z, g.w)); }
static const uint32_t kCopyDepthMinSpv[] = {
  0x07230203,0x00010300,0x0008000b,0x0000002a,0x00000000,0x00020011,0x00000001,0x0006000b,
  0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
  0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000010,0x00000018,0x00030010,
  0x00000004,0x00000007,0x00030010,0x00000004,0x0000000c,0x00030003,0x00000002,0x000001c2,
  0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000009,0x00000067,0x00060005,
  0x0000000d,0x74706544,0x78655468,0x65727574,0x00000000,0x00060005,0x00000010,0x545f7376,
  0x4f435845,0x3044524f,0x00000000,0x00060005,0x00000018,0x465f6c67,0x44676172,0x68747065,
  0x00000000,0x00040047,0x0000000d,0x00000021,0x00000020,0x00040047,0x0000000d,0x00000022,
  0x00000002,0x00040047,0x00000010,0x0000001e,0x00000001,0x00040047,0x00000018,0x0000000b,
  0x00000016,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,
  0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000007,
  0x00000007,0x00090019,0x0000000a,0x00000006,0x00000001,0x00000000,0x00000000,0x00000000,
  0x00000001,0x00000000,0x0003001b,0x0000000b,0x0000000a,0x00040020,0x0000000c,0x00000000,
  0x0000000b,0x0004003b,0x0000000c,0x0000000d,0x00000000,0x00040020,0x0000000f,0x00000001,
  0x00000007,0x0004003b,0x0000000f,0x00000010,0x00000001,0x00040017,0x00000011,0x00000006,
  0x00000002,0x00040015,0x00000014,0x00000020,0x00000001,0x0004002b,0x00000014,0x00000015,
  0x00000000,0x00040020,0x00000017,0x00000003,0x00000006,0x0004003b,0x00000017,0x00000018,
  0x00000003,0x00040015,0x00000019,0x00000020,0x00000000,0x0004002b,0x00000019,0x0000001a,
  0x00000000,0x00040020,0x0000001b,0x00000007,0x00000006,0x0004002b,0x00000019,0x0000001e,
  0x00000001,0x0004002b,0x00000019,0x00000022,0x00000002,0x0004002b,0x00000019,0x00000025,
  0x00000003,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,
  0x0004003b,0x00000008,0x00000009,0x00000007,0x0004003d,0x0000000b,0x0000000e,0x0000000d,
  0x0004003d,0x00000007,0x00000012,0x00000010,0x0007004f,0x00000011,0x00000013,0x00000012,
  0x00000012,0x00000000,0x00000001,0x00060060,0x00000007,0x00000016,0x0000000e,0x00000013,
  0x00000015,0x0003003e,0x00000009,0x00000016,0x00050041,0x0000001b,0x0000001c,0x00000009,
  0x0000001a,0x0004003d,0x00000006,0x0000001d,0x0000001c,0x00050041,0x0000001b,0x0000001f,
  0x00000009,0x0000001e,0x0004003d,0x00000006,0x00000020,0x0000001f,0x0007000c,0x00000006,
  0x00000021,0x00000001,0x00000025,0x0000001d,0x00000020,0x00050041,0x0000001b,0x00000023,
  0x00000009,0x00000022,0x0004003d,0x00000006,0x00000024,0x00000023,0x00050041,0x0000001b,
  0x00000026,0x00000009,0x00000025,0x0004003d,0x00000006,0x00000027,0x00000026,0x0007000c,
  0x00000006,0x00000028,0x00000001,0x00000025,0x00000024,0x00000027,0x0007000c,0x00000006,
  0x00000029,0x00000001,0x00000025,0x00000021,0x00000028,0x0003003e,0x00000018,0x00000029,
  0x000100fd,0x00010038
};

// The CopyDepth program with the fragment stage swapped for kCopyDepthMinSpv (same layout and bindings), or nullptr when not needed.
static ProgramImpl* EvalCopyDepthVariant(ReplayCtx& cx, ProgramImpl* p, const std::vector<TexWrite>& texWrites) {
  static const bool off = getenv("NVN_NO_EVAL_DEPTH_FIX") != nullptr;
  if (off || !p->st[1].valid || strncmp(p->st[1].key, "fae2ba6353af", 12) || !cx.rt[8]) return nullptr;
  bool scaled = false;
  for (auto& w : texWrites) if (w.r.t && w.r.t->scale != cx.rt[8]->scale) scaled = true;
  if (!scaled) return nullptr;
  static std::mutex mu;
  static std::map<ProgramImpl*, ProgramImpl*> vars;
  std::lock_guard<std::mutex> lk(mu);
  auto it = vars.find(p);
  if (it != vars.end()) return it->second;
  ProgramImpl* v = nullptr;
  std::vector<uint32_t> words(kCopyDepthMinSpv, kCopyDepthMinSpv + sizeof kCopyDepthMinSpv / 4);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.size() * 4; ci.pCode = words.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(cx.h->device, &ci, nullptr, &mod) == VK_SUCCESS) {
    v = new ProgramImpl(*p);   // kept for the process lifetime (pipelines and the pipeline service refer to it)
    v->composite = true;
    StageProg& f = v->st[1];
    snprintf(f.key, sizeof f.key, "evcpdepthmin");
    snprintf(f.fkey, sizeof f.fkey, "evcpdepthmin");
    f.mod = mod; f.spv = std::move(words); f.fragMods.clear(); f.outLocs.clear();
    Log("photo evaluation: CopyDepth samples a scaled depth texture, using the 2x2 minimum variant");
  } else LogError("photo evaluation: CopyDepth min variant module failed");
  vars[p] = v;
  return v;
}
// Collects the sampled textures / storage images a program reads, uploads static data and fixes layouts. Must run
// before a rendering scope is open (graphics) or at any time (compute).
bool PrepareTextures(ReplayCtx& cx, DrawState& d, ProgramImpl* p, std::vector<TexWrite>& texWrites, bool graphics) {
  Host* h = cx.h; (void)h;
  // sampled textures / storage images (layouts must be right before a rendering scope is open)
  g_pomDraw = PomDrawState();
  ScratchVec<PrepWant> wantS;   // [cpuprof]
  std::vector<PrepWant>& wantLayout = wantS.get();
  for (int s = 0; s < 6; s++) {
    if (!p->st[s].valid) continue;
    for (auto& t : p->st[s].tex) {
      if (t.separate) continue;
      for (int k = 0; k < t.array; k++) {
        int bi = TexBindIndex(t) + k;
        ResolvedTex r;
        if (bi >= 0 && bi < 32 && d.texSet[s][bi]) r = ResolveHandle(cx, d, d.tex[s][bi], false, t.type);
        if (s == 1 && r.t && r.t->pomCls && !g_pomDraw.cls) { g_pomDraw.cls = r.t->pomCls; g_pomDraw.handle = t.handleIndex; }
        texWrites.push_back({t.set, t.binding, k, r, false, t.type});
        if (r.t && r.view) wantLayout.push_back({r.t, SampledLayout(r.t), r.range});
      }
    }
    for (auto& t : p->st[s].img) {
      for (int k = 0; k < t.array; k++) {
        int bi = t.handleIndex + k;
        ResolvedTex r;
        if (bi >= 0 && bi < 8 && d.imgSet[s][bi]) r = ResolveHandle(cx, d, d.img[s][bi], true, t.type);
        texWrites.push_back({t.set, t.binding, k, r, true, t.type});
        if (r.t && r.view) wantLayout.push_back({r.t, VK_IMAGE_LAYOUT_GENERAL, r.range});
      }
    }
  }
  for (auto& w : wantLayout) {
    if (!w.t->uploaded) UploadStaticTexture(cx, w.t);
    else RefreshDynamicTexture(cx, w.t);
    if (!InLayout(w.t, w.layout, w.range)) { EndRendering(cx); TransitionRange(cx.cb, w.t, w.layout, w.range); }
  }
  for (auto& w : texWrites) if (w.r.feedback) {
    static int warned = 0;
    if (warned++ < 8) LogError("texture %p (%dx%d f%x) is sampled on the subresource it renders to; bound a typed dummy", (void*)w.r.t, w.r.t->b.width, w.r.t->b.height, w.r.t->b.format);
  }
  (void)graphics;
  return true;
}

// Fallback view of the dimension the shader declares (unbound slot, or a texture that cannot be viewed that way).
VkImageView DummyView(const Dummies* dm, int shaderType) {
  const bool arr = (shaderType & 0x100) != 0;
  switch (shaderType & 0xff) {
    case 1: return arr ? dm->v1da : dm->v1d;
    case 3: return arr ? dm->v2da : dm->sampledView;
    case 4: return dm->v3d;
    case 5: return arr ? dm->vcubea : dm->vcube;
    default: return dm->sampledView;
  }
}

// NVN storage-buffer index of a shader's storage buffer. The translator numbers them in the order the shader uses them ("slot"), but the
// SASS loads each address from the driver constant buffer c0, and that offset is the real binding: Ryujinx GlobalMemory puts the
// per-stage tables at word 0x44 + 64 * {VS 0, TCS 1, TES 2, GS 3, FS 4} (compute: like TES) with 4 words per index. Example:
// EvaluationAggregatePS (GLSL binding = 1) has slot 0 at word 328 = 324 + 4, i.e. index 1; with the ordinal it read unbound slot 0
// and its atomics went into the dummy buffer. Offsets outside the table keep the ordinal.
int SsboIndex(int stage, const MSb& sb) {
  static const int base[6] = {0x44, 0x44 + 4 * 64, 0x44 + 3 * 64, 0x44 + 64, 0x44 + 2 * 64, 0x44 + 2 * 64};   // NVN stage order: VS FS GS TCS TES CS
  if (sb.sbCbSlot == 0 && stage >= 0 && stage < 6) {
    const int w = sb.sbCbOffset - base[stage];
    if (w >= 0 && (w & 3) == 0 && w / 4 < 16) return w / 4;
  }
  return sb.slot;
}

bool WriteDescriptors(ReplayCtx& cx, DrawState& d, ProgramImpl* p, const std::vector<TexWrite>& texWrites, VkPipelineBindPoint bind) {
  Host* h = cx.h;
  snap_perf::Scope perfDesc(snap_perf::C_DESC);
  // descriptors
  Dummies* dm = h->dummies;
  // [cpuprof] reused per-thread vectors; pBufferInfo / pImageInfo hold index + 1 until the fix-up before vkUpdateDescriptorSets (the
  // vectors may grow while the writes are built)
  ScratchVec<VkDescriptorBufferInfo> bufS;
  ScratchVec<VkDescriptorImageInfo> imgS;
  ScratchVec<VkWriteDescriptorSet> wrS;
  std::vector<VkDescriptorBufferInfo>& bufInfos = bufS.get();
  std::vector<VkDescriptorImageInfo>& imgInfos = imgS.get();
  std::vector<VkWriteDescriptorSet>& writes = wrS.get();
  VkDescriptorSet sets[4] = {};
  for (int set = 0; set < 4; set++) if (p->setUsed[set]) { sets[set] = AllocSet(cx, p->setLayout[set]); if (!sets[set]) return false; }   // [nvnfix] logged in AllocSet: skip the draw
  auto wBuf = [&](int set, int binding, VkDescriptorType t, const BufBind& b) {
    bufInfos.push_back({b.buf, b.off, b.range});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[set]; w.dstBinding = (uint32_t)binding; w.descriptorCount = 1; w.descriptorType = t;
    w.pBufferInfo = reinterpret_cast<const VkDescriptorBufferInfo*>((uintptr_t)bufInfos.size());   // index + 1, fixed up below
    writes.push_back(w);
  };
  if (sets[0]) wBuf(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, {dm->support, 0, 1408});
  const BufBind zero{dm->zero, 0, 65536};
  for (int s = 0; s < 6; s++) {
    const StageProg& sp = p->st[s];
    if (!sp.valid) continue;
    for (auto& cb : sp.cb) {
      BufBind b = zero;
      if (cb.slot == 1 && sp.constBuf) b = {sp.constBuf, 0, sp.constSize};
      else if (cb.slot >= 3 && cb.slot - 3 < 16 && d.ubo[s][cb.slot - 3].buf) b = d.ubo[s][cb.slot - 3];
      else if (cb.slot >= 3) Log("stage %d: uniform buffer %d not bound, using zeros", s, cb.slot - 3);
      if (sets[cb.set]) wBuf(cb.set, cb.binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b);
    }
    for (auto& sb : sp.sb) {
      BufBind b = {dm->zero, 0, 4096};
      const int sbIdx = SsboIndex(s, sb);
      if (sbIdx >= 0 && sbIdx < 16 && d.ssbo[s][sbIdx].buf) b = d.ssbo[s][sbIdx];
      if (sb.write && b.pool && DebugDrawLogActive()) DebugDrawLog("  SSBO write stage %d slot %d pool %p off 0x%llx size 0x%llx poolsize 0x%llx\n", s, sb.slot, (void*)b.pool, (unsigned long long)b.off, (unsigned long long)b.range, (unsigned long long)b.pool->size);
      if (s == 1 && sb.write) {   // debug (NVN_XFER_LOG): fragment-shader stores (photo evaluation, photo editing)
        static const bool xl = getenv("NVN_XFER_LOG") != nullptr;
        static int n = 0;
        if (xl && n++ < 300) {
          fprintf(stderr, "[xfer] p%d fragment SSBO write prog=%d fs=%s slot=%d pool=%p cpu=%d off=0x%llx range=0x%llx bound:", g_presentCount, (int)p->id, sp.key, sb.slot, (void*)b.pool, b.pool ? (int)b.pool->Cpu() : -1, (unsigned long long)b.off, (unsigned long long)b.range);
          for (int k = 0; k < 16; k++) if (d.ssbo[1][k].buf) fprintf(stderr, " [%d]=%p+0x%llx", k, (void*)d.ssbo[1][k].pool, (unsigned long long)d.ssbo[1][k].off);
          fprintf(stderr, "\n");
        }
      }
      if (sb.write && b.pool) {
        // Fragment-shader stores into a CPU pool are results the game reads back (EvaluationAggregatePS counts the visible pixels of each
        // Pokemon with atomicAdd; the photo score comes from them). They are copied back into guest memory once the submit completes.
        // Everything else (skinning compute output, megabytes per frame) stays GPU-owned: never read by the CPU, and reading it back
        // from BAR memory would be slow.
        static const bool noWb = getenv("NVN_NO_SHADER_WB") != nullptr;   // A/B switch: the old behavior (GPU-owned, never copied back)
        if (!noWb && s == 1 && b.pool->Cpu() && b.pool->storage && b.pool->mapped && b.range <= (1u << 20)) QueueShaderWriteback(cx, b.pool, b.off, b.range);
        else MarkGpuWritten(b.pool, b.off, b.range);   // later CPU->GPU syncs must not clobber what the shader writes
      }
      if (sets[sb.set]) wBuf(sb.set, sb.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b);
    }
  }
  // images: consecutive array elements of one binding are written together
  for (size_t i = 0; i < texWrites.size();) {
    size_t j = i;
    while (j < texWrites.size() && texWrites[j].set == texWrites[i].set && texWrites[j].binding == texWrites[i].binding && texWrites[j].image == texWrites[i].image) j++;
    const size_t first = imgInfos.size();
    for (size_t k = i; k < j; k++) {
      const ResolvedTex& r = texWrites[k].r;
      VkDescriptorImageInfo ii{};
      if (texWrites[k].image) {
        ii.imageView = r.view ? r.view : dm->storageView; ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      } else {
        const bool shadow = (texWrites[k].stype & 0x400) != 0 && !r.view;
        ii.imageView = r.view ? r.view : shadow ? dm->vdep : DummyView(dm, texWrites[k].stype);
        ii.sampler = shadow ? (r.samp ? r.samp : dm->cmpSampler) : r.samp ? r.samp : dm->sampler;
        ii.imageLayout = (r.t && r.view) ? SampledLayout(r.t) : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      imgInfos.push_back(ii);
    }
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[texWrites[i].set]; w.dstBinding = (uint32_t)texWrites[i].binding; w.descriptorCount = (uint32_t)(j - i);
    w.descriptorType = texWrites[i].image ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>((uintptr_t)first + 1);   // index + 1, fixed up below
    writes.push_back(w);
    i = j;
  }
  for (auto& w : writes) if (!w.dstSet) return false;
  for (auto& w : writes) {
    if (w.pBufferInfo) w.pBufferInfo = &bufInfos[(uintptr_t)w.pBufferInfo - 1];
    if (w.pImageInfo) w.pImageInfo = &imgInfos[(uintptr_t)w.pImageInfo - 1];
  }
  if (!writes.empty()) vkUpdateDescriptorSets(h->device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
  for (int set = 0; set < 4; set++)
    if (sets[set]) vkCmdBindDescriptorSets(cx.cb, bind, p->layout, (uint32_t)set, 1, &sets[set], 0, nullptr);

  return true;
}

// [rt] what nvn_rt.cpp needs to know about a draw (DrawState is private to this file)
static void RtFillInfo(snap_rt::DrawInfo& ri, const DrawState& d, const ProgramImpl* p, const Cmd& c, bool indexed, VkPrimitiveTopology topo, uint32_t vcount) {
  ri.vsKey = p->st[0].key; ri.hasGeomTess = p->st[2].valid || p->st[3].valid || p->st[4].valid; ri.topo = topo; ri.vertexCount = vcount;
  ri.instances = (uint32_t)std::max(indexed ? c.i[5] : c.i[4], 1);
  ri.ibAddr = indexed ? c.a[0] : 0; for (int i = 0; i < 4; i++) ri.vbAddr[i] = d.vbAddr[i];
  ri.first = c.i[indexed ? 4 : 1]; ri.base = c.i[3]; ri.count = c.i[2];
  ri.depthTest = DsGet(d.ds, DS_DT, 1) != 0; ri.depthWrite = DsGet(d.ds, DS_DW, 1) != 0; ri.blend0 = (d.color.blendEn & 1) != 0;
  const BufBind& u = d.ubo[0][0];
  if (u.pool && u.pool->storage) { ri.cam = (const uint8_t*)u.pool->storage + u.off; ri.camRange = u.range; }
  { uint64_t x = 1469598103934665603ull;   // [rt] per-object constants (every vertex bank but the camera bank): draws of one mesh with different transforms
    for (int b = 1; b < 16; b++) { const BufBind& o = d.ubo[0][b]; if (!o.pool || !o.pool->storage) continue; const uint64_t n = std::min<uint64_t>(o.range, 256) / 8; const uint64_t* w = (const uint64_t*)((const uint8_t*)o.pool->storage + o.off); x = (x ^ (uint64_t)b) * 1099511628211ull; for (uint64_t k = 0; k < n; k++) x = (x ^ w[k]) * 1099511628211ull; }
    ri.cbHash = x; }
}

// Debug (NVN_DRAW_CENSUS=<file> [NVN_DRAW_CENSUS_FROM=<present>]): per present, the set of (vertex/fragment program, colour target size)
// drawn, tagged I (the game issued the draw) and D (it reached vkCmdDraw). One line per present that differs from the one
// before: "+" keys that appeared, "-" keys that vanished. Finds draws that come and go between frames (worker eyes, 2026-11-14).
static void DrawCensus(const ReplayCtx& cx, const ProgramImpl* p, char tag) {
  static const char* path = getenv("NVN_DRAW_CENSUS");
  if (!path || !p || !cx.rt[0]) return;
  static const int from = getenv("NVN_DRAW_CENSUS_FROM") ? atoi(getenv("NVN_DRAW_CENSUS_FROM")) : 0;
  if (g_presentCount < from) return;
  static std::mutex mu; static FILE* f = fopen(path, "w"); static int present = -1;
  static std::set<std::string> cur, prev;
  char k[64]; snprintf(k, sizeof k, "%c:%s/%s:%dx%d", tag, p->st[0].key, p->st[1].key, cx.rt[0]->b.width, cx.rt[0]->b.height);
  std::lock_guard<std::mutex> g(mu);
  if (g_presentCount != present) {
    if (f && present >= 0 && cur != prev) {
      fprintf(f, "p%d n=%zu", present, cur.size());
      for (auto& s : cur) if (!prev.count(s)) fprintf(f, " +%s", s.c_str());
      for (auto& s : prev) if (!cur.count(s)) fprintf(f, " -%s", s.c_str());
      fprintf(f, "\n"); fflush(f);
    }
    prev.swap(cur); cur.clear(); present = g_presentCount;
  }
  cur.insert(k);
}

void DoDraw(ReplayCtx& cx, const Cmd& c0) {
  Cmd adjC;
  const Cmd& c = ModelDrawAdjust(cx, c0, adjC) ? adjC : c0;
  Host* h = cx.h;
  DrawState& d = *cx.ds;
  ProgramImpl* p = d.prog;
  DrawCensus(cx, p, 'I');
  snap_perf::count(snap_perf::V_DRAWS);
  const bool indexed = c.op == Op::DrawIndexed;
  if (!p || !p->layout || !p->st[0].valid) { LogError("draw without a usable graphics program"); return; }
  VkPrimitiveTopology topo;
  // Primitives Vulkan lacks (LINE_LOOP, QUADS, QUAD_STRIP, POLYGON) and u8 indices are rewritten on the CPU into a
  // scratch u32 index buffer. Index data is read straight from guest memory (guest pointer == host pointer).
  std::vector<uint32_t> rw;
  bool rewrite = false;
  {
    const int prim = c.i[0];
    const bool native = Topology(prim, &topo);
    const bool u8 = indexed && c.i[1] == IDX_U8;
    if (!native || u8) {
      if (!native && prim != 2 && prim != 7 && prim != 8 && prim != 9) {
        if (!d.warnedPrim) { d.warnedPrim = true; LogError("draw primitive %d is not supported", prim); }
        return;
      }
      const int count = std::max(indexed ? c.i[2] : c.i[2], 0);
      std::vector<uint32_t> src(count);
      if (indexed) {
        PoolImpl* pool = nullptr; uint64_t off = 0;
        if (!h->ResolveAddress(c.a[0], &pool, &off) || !pool->storage) { LogError("index buffer address 0x%llx is not in any memory pool", (unsigned long long)c.a[0]); return; }
        const uint8_t* b = static_cast<const uint8_t*>(pool->storage) + off;
        for (int i = 0; i < count; i++)
          src[i] = c.i[1] == IDX_U8 ? b[i] : c.i[1] == IDX_U16 ? ((const uint16_t*)b)[i] : ((const uint32_t*)b)[i];
      } else {
        for (int i = 0; i < count; i++) src[i] = (uint32_t)(c.i[1] + i);
      }
      switch (native ? -1 : prim) {
        case -1: rw = src; break;
        case 2:   // LINE_LOOP -> LINE_STRIP closed with the first vertex
          topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; rw = src; if (count > 1) rw.push_back(src[0]); break;
        case 7:   // QUADS (v0 v1 v2 v3) -> (v0 v1 v2)(v0 v2 v3)
          topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
          for (int q = 0; q + 3 < count; q += 4) { rw.insert(rw.end(), {src[q], src[q + 1], src[q + 2], src[q], src[q + 2], src[q + 3]}); }
          break;
        case 8:   // QUAD_STRIP: quad k = (2k, 2k+1, 2k+3, 2k+2)
          topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
          for (int q = 0; q + 3 < count; q += 2) { rw.insert(rw.end(), {src[q], src[q + 1], src[q + 3], src[q], src[q + 3], src[q + 2]}); }
          break;
        case 9:   // POLYGON == fan
          topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; rw = src; break;
      }
      rewrite = true;
      if (rw.empty()) return;
    }
  }
  // render targets
  RtInfo rt{};
  rt.depth = rt.stencil = VK_FORMAT_UNDEFINED; rt.samples = 1;
  for (int i = 0; i < 8; i++) {
    TextureImpl* t = cx.rt[i];
    if (t && !t->EnsureImage()) return;
    rt.color[i] = t ? t->RtFormat(cx.rtHasView[i] ? &cx.rtView[i] : nullptr)->vk : VK_FORMAT_UNDEFINED;   // [colorfix] the view's sRGB/UNORM twin
    if (t) { rt.nColor = i + 1; rt.samples = std::max(rt.samples, t->b.samples); }
  }
  if (TextureImpl* t = cx.rt[8]) {
    if (!t->EnsureImage()) return;
    if (t->fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) rt.depth = t->fi->vk;
    if (t->fi->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) rt.stencil = t->fi->vk;
  }
  if (rt.nColor == 0 && !cx.rt[8]) { LogError("draw with no render targets"); return; }

  ScratchVec<TexWrite> texWritesS;   // [cpuprof]
  std::vector<TexWrite>& texWrites = texWritesS.get();
  if (!PrepareTextures(cx, d, p, texWrites, true)) return;
  static const char* const kDrawSig = getenv("NVN_DRAW_SIG");   // load pass: read once (getenv per draw showed in a host profile)
  static const bool kRtDump = getenv("NVN_RT_DUMP") != nullptr;
  {   // debug (SNAP_DRAW_SKIP_FS=<fragment shader key[,key...]>): skip every draw whose fragment shader key is listed (A/B of which draws make a pixel)
    static const char* const kSkipFs = getenv("SNAP_DRAW_SKIP_FS");
    if (kSkipFs && p->st[1].key && strstr(kSkipFs, p->st[1].key)) return;
    // [water] same-moment A/B: SNAP_DRAW_SKIP_GROUPS=<keys of group 1>;<keys of group 2>;... skips group k on the presents with (p % (4*(n+1))) / 4 == k; block 0 stays normal
    static const char* const kGroups = getenv("SNAP_DRAW_SKIP_GROUPS");
    if (kGroups && p->st[1].key) {
      int n = 1; for (const char* c = kGroups; *c; c++) if (*c == ';') n++;
      const int ph = (g_presentCount % (4 * (n + 1))) / 4;
      if (ph >= 1) { const char* g = kGroups; for (int k = 1; k < ph && g; k++) { g = strchr(g, ';'); if (g) g++; } const char* e = g ? strchr(g, ';') : nullptr; const std::string grp = g ? (e ? std::string(g, e) : std::string(g)) : std::string(); if (grp.find(p->st[1].key) != std::string::npos) return; }
    }
  }
  if (const char* sigPath = kDrawSig) {   // debug: one line per distinct (render targets, program, sampled textures) combination, the first time it is drawn
    static FILE* sf = fopen(sigPath, "w");
    static std::set<std::string> seen;
    static const auto t0 = std::chrono::steady_clock::now();
    char buf[1024]; int n = snprintf(buf, sizeof buf, "rt0=%p(%dx%d f%x) rt8=%p prog=%d vs=%s fs=%s:", (void*)cx.rt[0], cx.rt[0] ? cx.rt[0]->b.width : 0, cx.rt[0] ? cx.rt[0]->b.height : 0, cx.rt[0] ? cx.rt[0]->b.format : 0, (void*)cx.rt[8], (int)p->id, p->st[0].key, p->st[1].key);
    for (auto& w : texWrites) if (w.r.t && n < 900) n += snprintf(buf + n, sizeof buf - n, " [b%d t=%p f%x %dx%d fl%x pool=%d up=%d]", w.binding, (void*)w.r.t, w.r.t->b.format, w.r.t->b.width, w.r.t->b.height, w.r.t->b.flags, w.r.t->b.pool != nullptr, (int)w.r.t->uploaded);
    if (sf && seen.insert(buf).second) fprintf(sf, "[%.2f p%d] %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), g_presentCount, buf), fflush(sf);
  }
  if (kRtDump) {   // debug: dump the 360x240 photo thumbnail textures and the display texture they are made from
    static std::set<uintptr_t> dumped;
    const bool smallRt = cx.rt[0] && cx.rt[0]->b.width == 360 && cx.rt[0]->b.height == 240;
    for (auto& w : texWrites) if (w.r.t && !w.r.feedback) {
      const bool thumb = w.r.t->b.width == 360 && w.r.t->b.height == 240, disp = smallRt && w.r.t->b.width == 1920;
      if (!(thumb || disp)) continue;
      if (!dumped.insert((uintptr_t)w.r.t * 31u + (uintptr_t)p->id).second) continue;
      char tag[64]; snprintf(tag, sizeof tag, "p%d_prog%d", g_presentCount, (int)p->id);
      DebugDumpSampledTexture(cx, w.r.t, tag);
    }
  }
  { static const bool folLog = getenv("SNAP_FOLIAGE_LOG") != nullptr || getenv("SNAP_PARTICLE_CENSUS") != nullptr;   // [foliage] debug census of alpha-tested / blended scene draws (nvn_foliage.cpp)
    if (folLog && cx.rt[0] && p->st[1].valid) {
      FoliageDrawInfo fi{}; fi.present = g_presentCount; fi.vs = p->st[0].key; fi.fs = p->st[1].key; fi.alphaFunc = d.color.alphaFunc; fi.alphaRef = d.alphaRef;
      fi.nColor = rt.nColor; fi.colorFmt0 = cx.rt[0]->b.format; fi.rtW = cx.rt[0]->b.width; fi.rtH = cx.rt[0]->b.height; fi.rtSamples = cx.rt[0]->b.samples; fi.depthFmt = cx.rt[8] ? cx.rt[8]->b.format : 0;
      fi.blend0 = (d.color.blendEn & 1) != 0; fi.depthWrite = DsGet(d.ds, DS_DW, 1) != 0; fi.depthTest = DsGet(d.ds, DS_DT, 1) != 0;
      fi.srcRgb = d.blend[0].srcRgb; fi.dstRgb = d.blend[0].dstRgb; fi.srcA = d.blend[0].srcA; fi.dstA = d.blend[0].dstA;
      for (auto& w : texWrites) if (w.r.t && fi.nTex < 8) fi.tex[fi.nTex++] = {(int)w.binding, (uint32_t)w.r.t->b.format, (uint32_t)w.r.t->b.width, (uint32_t)w.r.t->b.height};
      fi.nVerts = c.i[2]; fi.nInst = c.i[indexed ? 5 : 4];
      fi.rtLabel = FoliageTexLabel(cx.rt[0]); fi.dsLabel = FoliageTexLabel(cx.rt[8]);
      FoliageLogDraw(fi);
    } }
  if (DebugDrawLogActive()) {
    DebugDrawLog("draw prog=%d prim=%d n=%d rt0=%p(%dx%d f%x) vp=%d:", (int)p->id, c.i[0], c.i[2], (void*)cx.rt[0], cx.rt[0] ? cx.rt[0]->b.width : 0, cx.rt[0] ? cx.rt[0]->b.height : 0, cx.rt[0] ? cx.rt[0]->b.format : 0, cx.vpSet);
    for (auto& w : texWrites) if (w.r.t) DebugDrawLog(" [b%d t=%p f%x %dx%d fl%x pool=%d]", w.binding, (void*)w.r.t, w.r.t->b.format, w.r.t->b.width, w.r.t->b.height, w.r.t->b.flags, w.r.t->b.pool != nullptr);
    DebugDrawLog("\n");
    for (auto& w : texWrites) if (w.r.t) { char sd[160]; DebugSamplerDesc((void*)w.r.samp, sd, sizeof sd); DebugDrawLog("   smp b%d lv%d %s\n", w.binding, w.r.t->b.levels, sd); }
    DebugDrawLog("   vs=%s fs=%s idx=%d first=%d n=%d inst=%d base=%d cull=%d depthT=%d depthW=%d nattr=%d:", p->st[0].key, p->st[1].key, (int)indexed, c.i[indexed ? 4 : 1], c.i[2], c.i[indexed ? 5 : 4], c.i[3],
                 (int)d.poly.cull, (int)DsGet(d.ds, DS_DT, 1), (int)DsGet(d.ds, DS_DW, 1), d.nattr);
    for (int i = 0; i < d.nattr; i++) DebugDrawLog(" a%d(f%u s%u o%u)", i, d.attr[i].v & 0x1FF, (d.attr[i].v >> 9) & 15, d.attr[i].v >> 13);
    DebugDrawLog(" streams:");
    for (int i = 0; i < d.nstream; i++) DebugDrawLog(" s%d(str%u buf%d)", i, d.stream[i].stride, d.vb[i].buf ? 1 : 0);
    DebugDrawLog(" ubo:");
    for (int i = 0; i < 16; i++) if (d.ubo[0][i].buf) DebugDrawLog(" v%d(%llu)", i, (unsigned long long)d.ubo[0][i].range);
    if (indexed && d.nattr > 0 && c.i[2] >= 20000) {   // big mesh: position bounds over the referenced vertices (CPU view of guest memory)
      PoolImpl *ip = nullptr, *vp = nullptr; uint64_t io = 0, vo = 0;
      const int s0 = (d.attr[0].v >> 9) & 15; const uint32_t o0 = d.attr[0].v >> 13;
      if (h->ResolveAddress(c.a[0], &ip, &io) && ip->storage && h->ResolveAddress(d.vbAddr[s0], &vp, &vo) && vp->storage) {
        for (int src = 0; src < 2; src++) {   // 0: CPU view of guest memory, 1: the Vulkan mirror (what the GPU wrote)
          if (src == 1 && !vp->mapped) break;
          float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}; int bad = 0, zero = 0; uint32_t maxIdx = 0;
          const uint8_t* ib = static_cast<const uint8_t*>(ip->storage) + io; const uint8_t* vbp = (src ? vp->mapped : static_cast<const uint8_t*>(vp->storage)) + vo;
          for (int k = 0; k < c.i[2]; k++) {
            const uint32_t ix = c.i[1] == IDX_U32 ? ((const uint32_t*)ib)[k] : c.i[1] == IDX_U16 ? ((const uint16_t*)ib)[k] : ib[k];
            maxIdx = std::max(maxIdx, ix);
            const float* f = (const float*)(vbp + (uint64_t)ix * d.stream[s0].stride + o0);
            if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) { bad++; continue; }
            if (f[0] == 0 && f[1] == 0 && f[2] == 0) zero++;
            for (int j = 0; j < 3; j++) { mn[j] = std::min(mn[j], f[j]); mx[j] = std::max(mx[j], f[j]); }
          }
          DebugDrawLog(" %s[%.2f %.2f %.2f .. %.2f %.2f %.2f bad=%d zero=%d maxIdx=%u vboff=0x%llx]", src ? "GPUPOS" : "POSBOUNDS", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], bad, zero, maxIdx, (unsigned long long)vo);
        }
      }
    }
    DebugDrawLog(" sbo:");
    for (int i = 0; i < 16; i++) if (d.ssbo[0][i].buf) DebugDrawLog(" v%d(%llu)", i, (unsigned long long)d.ssbo[0][i].range);
    DebugDrawLog("\n");
  }
  static const bool evalLog = getenv("NVN_EVAL_LOG") != nullptr;   // debug: depth/stencil state and FS cbuffer of the photo evaluation draws
  const bool evalDs = cx.rt[8] && cx.rt[8]->b.format == 0x36 && cx.rt[8]->b.width == 1920;   // the Evaluation DepthStencil (DEPTH32F_STENCIL8)
  if (evalLog && (evalDs || (p->st[1].valid && (!strncmp(p->st[1].key, "6e4fc3880af9", 12) || !strncmp(p->st[1].key, "10774e331b21", 12) || !strncmp(p->st[1].key, "fae2ba6353af", 12))))) {
    static int n = 0;
    static const int evalMax = getenv("NVN_EVAL_LOG_MAX") ? atoi(getenv("NVN_EVAL_LOG_MAX")) : 1500;
    if (n++ < evalMax) {
      const char* fk = p->st[1].valid ? p->st[1].key : "none";
      const char* nm = !strncmp(fk, "6e4fc3880af9", 12) ? "Aggregate" : !strncmp(fk, "10774e331b21", 12) ? "Sphere" : !strncmp(fk, "fae2ba6353af", 12) ? "CopyDepth" : fk;
      fprintf(stderr, "[eval] p%d %s vs=%.12s rt0=%p n=%d ds=%llx (dt=%u dw=%u st=%u df=%u sf=%u fail=%u zfail=%u zpass=%u) ref=%d cmp=%x wr=%x rt8=%p f%x cull=%d vp=(%.0f %.0f %.0f %.0f) cb:",
              g_presentCount, nm, p->st[0].key, (void*)cx.rt[0], c.i[2], (unsigned long long)d.ds.bits, DsGet(d.ds, DS_DT, 1), DsGet(d.ds, DS_DW, 1), DsGet(d.ds, DS_ST, 1), DsGet(d.ds, DS_DF, 4),
              DsGet(d.ds, DS_SF_F, 4), DsGet(d.ds, DS_FAIL_F, 4), DsGet(d.ds, DS_ZFAIL_F, 4), DsGet(d.ds, DS_ZPASS_F, 4), (int)d.stRef[0], (unsigned)d.stCompare[0], (unsigned)d.stWrite[0],
              (void*)cx.rt[8], cx.rt[8] ? cx.rt[8]->b.format : 0, (int)d.poly.cull, cx.vpGuest[0][0], cx.vpGuest[0][1], cx.vpGuest[0][2], cx.vpGuest[0][3]);
      for (int st = 0; st < 2; st++) {
        const BufBind& u = d.ubo[st][0];
        if (u.pool && u.pool->storage) { const float* f = (const float*)((const uint8_t*)u.pool->storage + u.off); fprintf(stderr, " s%d[", st); for (int k = 0; k < 16; k++) fprintf(stderr, "%g ", f[k]); fprintf(stderr, "]"); }
      }
      for (auto& w : texWrites) if (w.r.t) fprintf(stderr, " tex%d=%ux%u f%x scale%.2f", w.binding, w.r.t->b.width, w.r.t->b.height, w.r.t->b.format, w.r.t->scale);   // CopyDepth's source size under render scale
      if (cx.rt[8]) fprintf(stderr, " rt8scale%.2f", cx.rt[8]->scale);
      if (d.ssbo[1][1].pool && d.ssbo[1][1].pool->storage) fprintf(stderr, " agg=%p", (const void*)((const uint8_t*)d.ssbo[1][1].pool->storage + d.ssbo[1][1].off));   // the subject's 0x40-byte counter slot (matches `wb values`)
      fprintf(stderr, "\n");
    }
  }
  static const bool thumbLog = getenv("NVN_THUMB_LOG") != nullptr;
  const bool thumbDraw = thumbLog && cx.rt[0] && ((cx.rt[0]->b.width == 360 && cx.rt[0]->b.height == 240) || (cx.bcDst[0] && cx.bcDst[0]->b.width == 360));
  if (cx.bcDst[0] && cx.bcDst[0]->b.width == 360 && cx.bcDst[0]->b.height == 240 && ShinyActive()) ShinyThumbCapture(cx.bcDst[0]);   // shiny mod: flag the thumbnail of a shiny photo
  if (cx.bcDst[0] && HdPhotoOn()) HdPhotoCompress(cx.bcDst[0], texWrites.empty() ? nullptr : texWrites[0].r.t);   // HD photos: the BC1 thumbnail shows its source capture
  if (thumbDraw) {
    fprintf(stderr, "[thumb] p%d draw prog=%d prim=%d n=%d idx=%d rt0=%p f%x cull=%d vpSet=%d scSet=%d rendering=%d ntex=%zu:", g_presentCount, (int)p->id, c.i[0], c.i[2], (int)indexed, (void*)cx.rt[0], cx.rt[0]->b.format, (int)d.poly.cull, (int)cx.vpSet, (int)cx.scSet, (int)cx.rendering, texWrites.size());
    fprintf(stderr, " vp0=(%.0f %.0f %.0f %.0f) view=%d flags=%x fmt=%x", cx.vpGuest[0][0], cx.vpGuest[0][1], cx.vpGuest[0][2], cx.vpGuest[0][3], (int)cx.rtHasView[0], cx.rtView[0].flags, cx.rtView[0].format);
    for (auto& w : texWrites) fprintf(stderr, " [b%d t=%p view=%p fb=%d]", w.binding, (void*)w.r.t, (void*)w.r.view, (int)w.r.feedback);
    fprintf(stderr, "\n");
  }
  for (auto& w : texWrites) if (w.binding == 33 && w.r.t && w.r.t->b.width == 1024 && w.r.t->b.height == 1024 && (w.r.t->b.format >= FMT_DEPTH16 && w.r.t->b.format <= FMT_DEPTH32F_STENCIL8)) g_dynShadowTex = w.r.t;   // dynamic shadow map (see ShadowDistanceOnBind)
  { static const int shLog = getenv("NVN_SHADOW_LOG") ? atoi(getenv("NVN_SHADOW_LOG")) : 0;   // debug: NVN_SHADOW_LOG=<present>: constants of the shadow-map casters and receivers at that present
    if (shLog && g_presentCount >= shLog && g_presentCount < shLog + 3000) {
      static int n = 0;
      auto isDep = [](int f) { return f == FMT_STENCIL8 || (f >= FMT_DEPTH16 && f <= FMT_DEPTH32F_STENCIL8); };
      const bool cast = cx.rt[8] && isDep(cx.rt[8]->b.format) && cx.rt[8]->b.width >= 512 && cx.rt[8]->b.width <= 4096 && cx.rt[8]->b.width == cx.rt[8]->b.height;
      bool recv = false; for (auto& w : texWrites) if (w.r.t && isDep(w.r.t->b.format) && w.r.t->b.width >= 512 && w.r.t->b.width <= 4096 && w.r.t->b.width == w.r.t->b.height) recv = true;
      static int lastP = -1, perC = 0, perR = 0, nc = 0, nr = 0; if (lastP != g_presentCount) { lastP = g_presentCount; perC = perR = 0; }
      if ((cast || recv) && (cast ? perC++ : perR++) < 3 && (cast ? nc++ : nr++) < 24) {
        fprintf(stderr, "[shadow] p%d %s rt8=%ux%u n=%d", g_presentCount, cast ? "CAST" : "RECV", cx.rt[8] ? cx.rt[8]->b.width : 0u, cx.rt[8] ? cx.rt[8]->b.height : 0u, c.i[2]);
        fprintf(stderr, " rt8tex=%p", (void*)cx.rt[8]); for (auto& w : texWrites) if (w.r.t && isDep(w.r.t->b.format)) fprintf(stderr, " depthTex(b%d)=%p %ux%u", w.binding, (void*)w.r.t, w.r.t->b.width, w.r.t->b.height);
        for (int st = 0; st < 2; st++) for (int b = 0; b < 16; b++) { const BufBind& u = d.ubo[st][b]; if (u.pool && u.pool->storage && (b != 3 || !cast)) { const float* f = (const float*)((const uint8_t*)u.pool->storage + u.off);
          fprintf(stderr, "\n   s%d b%d @%p range=%llu:", st, b, (const void*)f, (unsigned long long)u.range); for (int k = 0; k < 160 && (uint64_t)k * 4 < u.range; k++) fprintf(stderr, " %g", f[k]); } }
        fprintf(stderr, "\n");
      }
    } }
  { const uint8_t* vu[2] = {}; uint64_t vr[2] = {};   // [upscale] the camera bank c3 of the scene's draws
    for (int i = 0; i < 2; i++) { const BufBind& u = d.ubo[0][i]; if (u.pool && u.pool->storage) { vu[i] = (const uint8_t*)u.pool->storage + u.off; vr[i] = u.range; } }
    UpscaleOnDraw(cx, vu, vr, 2);
    { const BufBind& u9 = d.ubo[0][9]; FxOnDraw(cx, vu, vr, (u9.pool && u9.pool->storage) ? (const uint8_t*)u9.pool->storage + u9.off : nullptr, u9.range); } }   // [fx] camera and shadow caster capture (nvn_fx.cpp)
  if (FxMidPass(cx, (d.color.blendEn & 1) != 0, DsGet(d.ds, DS_DW, 1) != 0)) FxReemitViewState(cx);   // [fx] opaque-only effects at the first blended scene draw
  snap_rt::ShadowPush rtShPush; bool rtShOk = false;   // [rt] M3: a receiver draw (its fragment program was patched to trace rays): builds the frame's shadow TLAS on the first one
  if (cx.rt[0] && p->st[1].valid && snap_rt::ShadowRays() && ShadowFilterApplies(p->st[1])) {
    bool reemit = false;
    const BufBind& r9 = d.ubo[1][9];   // the receivers' own matrix: fragment-stage binding 9 (the shader's c12), 132 bytes
    rtShOk = snap_rt::ShadowReceiverPrepare(cx, (r9.pool && r9.pool->storage && r9.range >= 128) ? (const uint8_t*)r9.pool->storage + r9.off : nullptr, r9.range, &rtShPush, &reemit);
    if (reemit) FxReemitViewState(cx);
  }
  if (WaterBeforeDraw(cx, p->st[1].valid ? p->st[1].key : nullptr, (d.color.blendEn & 1) != 0, DsGet(d.ds, DS_DW, 1) != 0)) FxReemitViewState(cx);   // [water] SNAP_WATER_*: copy before the sea, effect pass after it (nvn_ssr.cpp)
  if (ShinyActive() && cx.rt[0] && !cx.bcDst[0] && texWrites.size() == 1 && !texWrites[0].image && texWrites[0].r.t && texWrites[0].r.t->b.width >= 1280) ShinyCaptureDraw(cx.rt[0]);   // shiny mod: capture pass into a photo target
  if (HdPhotoOn() && cx.rt[0] && !cx.bcDst[0] && texWrites.size() == 1 && !texWrites[0].image && texWrites[0].r.t) {   // HD photos: a 2160x1440 copy of each capture (nvn_hdphoto.cpp)
    const BufBind& u4 = d.ubo[1][1];   // c4 = guest uniform buffer 1 (cbuf slot - 3)
    // CaptureVS: position = attribute 0, texture coordinate = attribute 2. The texture coordinates at the target's edges give the
    // scene region the capture samples (the shutter takes the centered 3:2 part; the album re-render samples another region).
    float uv[4]; const float* uvp = nullptr;
    const TextureBuilder& rb = cx.rt[0]->b;
    if (d.nattr > 2 && c.i[2] >= 3 && ((rb.width == 360 && rb.height == 240) || (rb.width == 1080 && rb.height == 720))) {
      const int sp = (d.attr[0].v >> 9) & 15, st = (d.attr[2].v >> 9) & 15;
      const uint32_t op = d.attr[0].v >> 13, ot = d.attr[2].v >> 13;
      PoolImpl *pp = nullptr, *tp = nullptr, *ip = nullptr; uint64_t po = 0, to = 0, io = 0;
      const int n = std::min(c.i[2], 6);
      int vi[6] = {}; bool okIdx = true;
      if (indexed) {   // the game draws the capture quad indexed (4 vertices)
        const int isz = c.i[1] == IDX_U32 ? 4 : c.i[1] == IDX_U16 ? 2 : 1;
        okIdx = h->ResolveAddress(c.a[0], &ip, &io) && ip && ip->storage && io + (uint64_t)(c.i[4] + n) * isz <= ip->size;
        if (okIdx) {
          const uint8_t* ib = static_cast<const uint8_t*>(ip->storage) + io + (uint64_t)c.i[4] * isz;
          for (int k = 0; k < n; k++) vi[k] = isz == 4 ? (int)((const uint32_t*)ib)[k] : isz == 2 ? (int)((const uint16_t*)ib)[k] : (int)ib[k];
          for (int k = 0; k < n; k++) { vi[k] += c.i[3]; okIdx = okIdx && vi[k] >= 0 && vi[k] < 65536; }   // base vertex
        }
      } else {
        for (int k = 0; k < n; k++) vi[k] = c.i[1] + k;
        okIdx = c.i[1] >= 0;
      }
      int vmax = 0; for (int k = 0; k < n; k++) vmax = std::max(vmax, vi[k]);
      if (okIdx && h->ResolveAddress(d.vbAddr[sp], &pp, &po) && pp && pp->storage && h->ResolveAddress(d.vbAddr[st], &tp, &to) && tp && tp->storage &&
          po + (uint64_t)(vmax + 1) * d.stream[sp].stride + op + 8 <= pp->size && to + (uint64_t)(vmax + 1) * d.stream[st].stride + ot + 8 <= tp->size) {
        float P[6][2], T[6][2];
        for (int k = 0; k < n; k++) {
          memcpy(P[k], static_cast<const uint8_t*>(pp->storage) + po + (uint64_t)vi[k] * d.stream[sp].stride + op, 8);
          memcpy(T[k], static_cast<const uint8_t*>(tp->storage) + to + (uint64_t)vi[k] * d.stream[st].stride + ot, 8);
        }
        bool ok = true;
        for (int ax = 0; ax < 2 && ok; ax++) {   // texture coordinate along each axis is linear in position: evaluate it at -1 and +1
          int lo = 0, hi = 0;
          for (int k = 1; k < n; k++) { if (P[k][ax] < P[lo][ax]) lo = k; if (P[k][ax] > P[hi][ax]) hi = k; }
          const float dx = P[hi][ax] - P[lo][ax];
          if (!(dx > 1e-3f) || !std::isfinite(T[lo][ax]) || !std::isfinite(T[hi][ax])) { ok = false; break; }
          const float a = (T[hi][ax] - T[lo][ax]) / dx;
          const float e0 = T[lo][ax] + a * (-1.0f - P[lo][ax]), e1 = T[lo][ax] + a * (1.0f - P[lo][ax]);
          uv[ax] = std::min(e0, e1); uv[ax + 2] = std::max(e0, e1);
        }
        if (ok) uvp = uv;
      }
    }
    static const bool hdLog = getenv("SNAP_HD_PHOTOS_LOG") != nullptr;
    static int hdLogN = 0;
    if (hdLog && !uvp && hdLogN < 8 && ((rb.width == 360 && rb.height == 240) || (rb.width == 1080 && rb.height == 720))) {   // diagnosis
      hdLogN++;
      fprintf(stderr, "[hdphoto] capture draw vertices unread: indexed %d nattr %d first %d n %d", (int)indexed, d.nattr, c.i[indexed ? 4 : 1], c.i[2]);
      for (int i = 0; i < d.nattr && i < 4; i++) fprintf(stderr, " a%d(f%u s%u o%u str%u addr %llx)", i, d.attr[i].v & 0x1FF, (d.attr[i].v >> 9) & 15, d.attr[i].v >> 13,
                                                         d.stream[(d.attr[i].v >> 9) & 15].stride, (unsigned long long)d.vbAddr[(d.attr[i].v >> 9) & 15]);
      fprintf(stderr, "\n");
    }
    HdPhotoCapture(cx, p, texWrites[0].r.t, cx.rt[0], (u4.pool && u4.pool->storage && u4.range >= 8) ? reinterpret_cast<const float*>(static_cast<const uint8_t*>(u4.pool->storage) + u4.off) : nullptr, uvp);
  }
  struct WideUboRestore {   // [wide] put back the c3 bindings a widened-camera copy replaced, on every exit of this draw
    DrawState* d = nullptr; BufBind saved[6]; bool swapped[6] = {};
    ~WideUboRestore() { if (d) for (int s = 0; s < 6; s++) if (swapped[s]) d->ubo[s][0] = saved[s]; }
  } wideUbo;
  // the box (min/max x, y of attribute 0 over the draw's vertices) of a draw of at most maxN vertices; false when the vertices are not readable
  auto quadBox = [&](float* box, int maxN) -> bool {
    if (d.nattr <= 0 || c.i[2] < 3 || c.i[2] > maxN) return false;
    const int sp = (d.attr[0].v >> 9) & 15; const uint32_t op = d.attr[0].v >> 13;
    PoolImpl *pp = nullptr, *ip = nullptr; uint64_t po = 0, io = 0;
    const int n = c.i[2]; static thread_local std::vector<int> vi; vi.assign((size_t)n, 0); bool ok = true;
    if (indexed) {
      const int isz = c.i[1] == IDX_U32 ? 4 : c.i[1] == IDX_U16 ? 2 : 1;
      ok = h->ResolveAddress(c.a[0], &ip, &io) && ip && ip->storage && io + (uint64_t)(c.i[4] + n) * isz <= ip->size;
      if (ok) {
        const uint8_t* ib = static_cast<const uint8_t*>(ip->storage) + io + (uint64_t)c.i[4] * isz;
        for (int k = 0; k < n; k++) { vi[k] = (isz == 4 ? (int)((const uint32_t*)ib)[k] : isz == 2 ? (int)((const uint16_t*)ib)[k] : (int)ib[k]) + c.i[3]; ok = ok && vi[k] >= 0 && vi[k] < 65536; }
      }
    } else { for (int k = 0; k < n; k++) vi[k] = c.i[1] + k; ok = c.i[1] >= 0; }
    int vmax = 0; for (int k = 0; k < n; k++) vmax = std::max(vmax, vi[k]);
    if (!(ok && d.stream[sp].stride && h->ResolveAddress(d.vbAddr[sp], &pp, &po) && pp && pp->storage && po + (uint64_t)vmax * d.stream[sp].stride + op + 8 <= pp->size)) return false;
    box[0] = box[1] = 1e30f; box[2] = box[3] = -1e30f;
    for (int k = 0; k < n && ok; k++) {
      float P[2]; memcpy(P, static_cast<const uint8_t*>(pp->storage) + po + (uint64_t)vi[k] * d.stream[sp].stride + op, 8);
      ok = std::isfinite(P[0]) && std::isfinite(P[1]) && std::fabs(P[0]) < 1e5f && std::fabs(P[1]) < 1e5f;
      box[0] = std::min(box[0], P[0]); box[1] = std::min(box[1], P[1]); box[2] = std::max(box[2], P[0]); box[3] = std::max(box[3], P[1]);
    }
    return ok;
  };
  for (int k = 0; k < 4; k++) cx.sm[k] = cx.smBox[k] = 0.0f;
  {   // [uismooth] (nvn_uismooth.h) an interface draw that the game steps at 60 Hz moves a fraction of its step ahead each displayed frame; [hudtrace] SNAP_HUD_TRACE=1 logs every interface draw (present, draw number in the frame, box as drawn)
    static const bool trace = [] { const char* e = getenv("SNAP_HUD_TRACE"); return e && *e && *e != '0'; }();
    UiSmoothState& smooth = UiSmoothShared();
    const bool sm = UiSmoothOn();
    if (sm || trace) {
      std::lock_guard<std::mutex> smLock(UiSmoothMutex());
      const TextureImpl* t0 = cx.rt[0];
      if (t0 && (t0->b.flags & TF_DISPLAY) && !cx.rt[8] && t0->b.target == TT_2D && t0->b.width == 1920 && t0->b.height == 1080) {
        float tb[4];
        if (quadBox(tb, 600)) {
          if (tb[2] <= 256.0f && tb[3] <= 256.0f) {   // glyph quads: add the text origin (vertex uniform block 1 floats 12-13), as the HUD scale below does
            const BufBind& hu = d.ubo[0][1];
            if (hu.pool && hu.pool->storage && hu.range >= 64 && hu.off + 64 <= hu.pool->size) {
              const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(hu.pool->storage) + hu.off);
              if (std::isfinite(f[12]) && std::isfinite(f[13]) && f[12] >= -1.0f && f[12] <= 1.0f && f[13] >= -1.0f && f[13] <= 1.0f && f[14] == 0.0f && f[15] == 1.0f) {
                const float ox = (f[12] + 1.0f) * 960.0f, oy = (1.0f - f[13]) * 540.0f;
                tb[0] += ox; tb[2] += ox; tb[1] += oy; tb[3] += oy;
              }
            }
          }
          float sh[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          if (sm) { UiSmoothOnDraw(smooth, g_presentCount, c.i[2], tb, sh, (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); for (int k = 0; k < 4; k++) { cx.sm[k] = sh[k]; cx.smBox[k] = tb[k]; } }
          if (trace) fprintf(stderr, "[hudtrace] f=%d i=%d n=%d %.2f %.2f %.2f %.2f   %.2f %.2f %.2f %.2f t=%.2f s=%d\n", g_presentCount, smooth.ord - 1, c.i[2], tb[0] + sh[0], tb[1] + sh[1], tb[2] + sh[2], tb[3] + sh[3], tb[0], tb[1], tb[2], tb[3],
                                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(), (int)sm);   // drawn box, the game's box, ms, smoothing on
        }
      }
    }
  }
  {   // [hud] HUD scale (nvn_hud.cpp): an interface draw into the 1920x1080 window texture (no depth) is drawn scaled; every other draw is not
    HudXf hx;
    if (HudScaleNow() > 1.001f) {
      const TextureImpl* t0 = cx.rt[0];
      if (t0 && (t0->b.flags & TF_DISPLAY) && !cx.rt[8] && t0->b.target == TT_2D && t0->b.width == 1920 && t0->b.height == 1080) {
        float hb[4];
        if (quadBox(hb, 3000)) {
          if (hb[2] <= 256.0f && hb[3] <= 256.0f) {   // glyph quads are in pixels local to the text; the text origin is the NDC position in floats 12-13 (z 0, w 1) of vertex uniform block 1 (the film counter digits)
            const BufBind& hu = d.ubo[0][1];
            if (hu.pool && hu.pool->storage && hu.range >= 64 && hu.off + 64 <= hu.pool->size) {
              const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(hu.pool->storage) + hu.off);
              if (std::isfinite(f[12]) && std::isfinite(f[13]) && f[12] >= -1.0f && f[12] <= 1.0f && f[13] >= -1.0f && f[13] <= 1.0f && f[14] == 0.0f && f[15] == 1.0f) {
                const float ox = (f[12] + 1.0f) * 960.0f, oy = (1.0f - f[13]) * 540.0f;
                hb[0] += ox; hb[2] += ox; hb[1] += oy; hb[3] += oy;
              }
            }
          }
          HudOnDraw(hb, &hx);
        }
      }
    }
    cx.hud = hx;
  }
  if (g_wideK > 1.0f) {   // [wide] ultrawide: choose how this draw maps onto a widened target (nvn_wide.cpp)
    TextureImpl* samp[8]; int ns = 0;
    for (auto& w : texWrites) if (!w.image && w.r.t && ns < 8) samp[ns++] = w.r.t;
    const BufBind& u0 = d.ubo[0][0];
    float box[4]; const float* boxp = nullptr;
    TextureImpl* frt = nullptr; for (int i = 0; i < 9 && !frt; i++) frt = cx.rt[i];
    if (frt && frt->wide > 1.0f && c.i[2] <= 64 && quadBox(box, 64)) boxp = box;   // a few quads (a panel, a 9-slice): the box its positions cover
    bool c3 = false, c4 = false;   // the camera only from c3 (no per-object matrix in c4): a widened c3 widens the draw
    for (int s = 0; s < 6; s++) if (p->st[s].valid) for (auto& cb : p->st[s].cb) { if (cb.slot == 3) c3 = true; if (cb.slot == 4) c4 = true; }
    const uint8_t* c3p = (u0.pool && u0.pool->storage) ? (const uint8_t*)u0.pool->storage + u0.off : nullptr;
    cx.wideFs = p->st[1].valid ? p->st[1].key : nullptr;
    if (WideOnDraw(cx, samp, ns, c3p, u0.range, boxp, c3 && !c4)) {
      const BufBind orig = u0;
      const uint64_t rng = std::min<uint64_t>({orig.range, orig.pool->size > orig.off ? orig.pool->size - orig.off : 0, 0x400});
      if (VkBuffer nb = rng >= 0x180 ? WideC3Copy(cx, c3p, rng) : VK_NULL_HANDLE) {
        wideUbo.d = &d;
        for (int s = 0; s < 6; s++)
          if (d.ubo[s][0].buf == orig.buf && d.ubo[s][0].off == orig.off) { wideUbo.saved[s] = d.ubo[s][0]; wideUbo.swapped[s] = true; d.ubo[s][0] = BufBind{nb, 0, rng, nullptr}; }
      }
    }
  }
  if (!cx.rendering) BeginRendering(cx, rt);
  if (cx.hud != cx.hudEmitted || memcmp(cx.sm, cx.smEmitted, sizeof cx.sm) != 0 || memcmp(cx.smBox, cx.smEmitted + 4, sizeof cx.smBox) != 0) FxReemitViewState(cx);   // [hud] this draw's transform differs from the one the viewport and scissor carry
  g_perfDraws++;

  ProgramImpl* evalVar = EvalCopyDepthVariant(cx, p, texWrites);
  if (!evalVar && cx.rt[0] && !texWrites.empty() && !texWrites[0].image)   // photo capture Lanczos on a scaled source (nvn_capture.cpp)
    evalVar = CaptureLanczosVariant(cx.h, p, texWrites[0].r.t, cx.rt[0], texWrites.size());
  if (evalVar) d.prog = evalVar;   // pipeline only; descriptors below still use p (same layout)
  const auto savedDsWater = d.ds;   // [water] the sea writes depth while the water effects run (the pass restores the pre-water depth, nvn_ssr.cpp)
  if (WaterForceDepthWrite()) DsSet(d.ds, DS_DW, 1, 1);
  VkPipeline pipe = GetPipeline(cx, d, topo, rt);
  d.ds = savedDsWater;
  if (evalVar) d.prog = p;
  if (!pipe) { if (thumbDraw) fprintf(stderr, "[thumb] NO PIPELINE\n"); return; }
  vkCmdBindPipeline(cx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

  // dynamic state that lives outside the pipeline
  vkCmdSetStencilCompareMask(cx.cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)d.stCompare[0]);
  vkCmdSetStencilCompareMask(cx.cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)d.stCompare[1]);
  vkCmdSetStencilWriteMask(cx.cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)d.stWrite[0]);
  vkCmdSetStencilWriteMask(cx.cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)d.stWrite[1]);
  vkCmdSetStencilReference(cx.cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)d.stRef[0]);
  vkCmdSetStencilReference(cx.cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)d.stRef[1]);
  {   // P1 probe (NVN_P1_LOG=1): state of the shadow-map caster draws. Result: depthBiasEnable is OFF on them (no polygon offset), so SNAP_SHADOW_BIAS is a
      // receiver-side reference-depth offset in the soft-shadow patch (nvn_shadowpcf.cpp), not a vkCmdSetDepthBias scale.
    static const bool p1 = getenv("NVN_P1_LOG") != nullptr;
    if (p1 && cx.rt[8] && cx.rt[8]->b.width == cx.rt[8]->b.height && cx.rt[8]->b.width >= 512 && cx.rt[8]->b.width <= 8192 && cx.rt[8]->b.format != 0x36) {
      const int offBit = d.poly.mode == POLY_POINT ? 1 : d.poly.mode == POLY_LINE ? 2 : 4;
      static int n = 0;
      if (n++ < 12) fprintf(stderr, "[p1] shadow caster draw p%d rt0=%p rt8 %ux%u fmt=%x depthBiasEnable=%d (offsetEn=%x mode=%d) factor=%g units=%g clamp=%g cull=%d\n", g_presentCount, (void*)cx.rt[0], cx.rt[8]->b.width, cx.rt[8]->b.height, cx.rt[8]->b.format, (int)((d.poly.offsetEn & offBit) != 0), (unsigned)d.poly.offsetEn, (int)d.poly.mode, d.biasFactor, d.biasUnits, d.biasClamp, (int)d.poly.cull);
    }
  }
  vkCmdSetDepthBias(cx.cb, d.biasUnits, h->features.depthBiasClamp ? d.biasClamp : 0.f, d.biasFactor);
  const float bc[4] = {0, 0, 0, 0};
  vkCmdSetBlendConstants(cx.cb, bc);

  if (!WriteDescriptors(cx, d, p, texWrites, VK_PIPELINE_BIND_POINT_GRAPHICS)) { if (thumbDraw) fprintf(stderr, "[thumb] WriteDescriptors FAILED\n"); return; }

  Dummies* dm = h->dummies;
  // vertex buffers (one binding per referenced stream; an unbound stream reads zeros)
  for (int i = 0; i < std::max(d.nstream, 0); i++) {
    VkBuffer b = d.vb[i].buf ? d.vb[i].buf : dm->zero;
    VkDeviceSize o = d.vb[i].buf ? d.vb[i].off : 0;
    vkCmdBindVertexBuffers(cx.cb, (uint32_t)i, 1, &b, &o);
  }
  const uint32_t shinyQ = cx.occOpen ? (ShinyQueryDiscard(), UINT32_MAX) : ShinyQueryBegin(h, cx.cb);   // shiny twin draw: own samples-passed query
  snap_rt::DrawCapture rtCap; snap_rt::DrawInfo rtInfo;   // [rt] SNAP_RT=1: transform-feedback capture of scene opaque draws (nvn_rt.cpp)
  const bool rtWant = snap_rt::Capture();
  if (rtWant) {
    RtFillInfo(rtInfo, d, p, c, indexed, topo, rewrite ? (uint32_t)rw.size() : (uint32_t)std::max(c.i[2], 0));
    const BufBind& u9 = d.ubo[0][9];   // M3: a draw of the dynamic shadow map's caster pass (same test as FxOnDraw): depth-only square target, 64-byte caster matrix in vertex slot 9
    if (u9.pool && u9.pool->storage && u9.range == 64 && cx.rt[8] && !cx.rt[0] && cx.rt[8]->b.width == cx.rt[8]->b.height && cx.rt[8]->b.width >= 512 && cx.rt[8]->b.width <= 8192 &&
        cx.rt[8]->b.format >= FMT_DEPTH16 && cx.rt[8]->b.format <= FMT_DEPTH32F_STENCIL8 && ShadowDynTexture(cx.rt[8])) {
      rtInfo.cast = true; rtInfo.s9 = (const uint8_t*)u9.pool->storage + u9.off; rtInfo.s9Range = u9.range;
    }
  }
  if (rtShOk) vkCmdPushConstants(cx.cb, p->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rtShPush, &rtShPush);   // M3: the receivers' ray-query constants
  if (g_pomDraw.push) { PomPush pp; if (PomFillPush(&pp)) vkCmdPushConstants(cx.cb, p->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 128, sizeof pp, &pp); }   // parallax: camera, sun, fade (second half of the fragment push block)
  if (rewrite) {
    VkBuffer ib = VK_NULL_HANDLE;
    uint8_t* m = NewScratch(cx, rw.size() * 4, &ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!m) { LogError("index rewrite: scratch allocation failed"); return; }
    memcpy(m, rw.data(), rw.size() * 4);
    vkCmdBindIndexBuffer(cx.cb, ib, 0, VK_INDEX_TYPE_UINT32);
    if (rtWant) rtCap.Begin(cx, rtInfo);
    if (indexed) vkCmdDrawIndexed(cx.cb, (uint32_t)rw.size(), (uint32_t)std::max(c.i[5], 1), 0, c.i[3], (uint32_t)c.i[4]);
    else vkCmdDrawIndexed(cx.cb, (uint32_t)rw.size(), (uint32_t)std::max(c.i[4], 1), 0, 0, (uint32_t)c.i[3]);
  } else if (indexed) {
    PoolImpl* pool = nullptr; uint64_t off = 0;
    if (!h->ResolveAddress(c.a[0], &pool, &off)) { LogError("index buffer address 0x%llx is not in any memory pool", (unsigned long long)c.a[0]); return; }
    const uint64_t isz = c.i[1] == IDX_U32 ? 4 : 2;
    if (!PoolBacked(pool)) return;   // [audit F6] acquire
    SyncPoolRangeOnce(pool, off, isz * (uint64_t)std::max(c.i[2], 0));
    vkCmdBindIndexBuffer(cx.cb, pool->buffer, off, c.i[1] == IDX_U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    if (rtWant) rtCap.Begin(cx, rtInfo);
    vkCmdDrawIndexed(cx.cb, (uint32_t)c.i[2], (uint32_t)std::max(c.i[5], 1), 0, c.i[3], (uint32_t)c.i[4]);
  } else {
    if (rtWant) rtCap.Begin(cx, rtInfo);
    vkCmdDraw(cx.cb, (uint32_t)c.i[2], (uint32_t)std::max(c.i[4], 1), (uint32_t)c.i[1], (uint32_t)c.i[3]);
  }
  ShinyQueryEnd(cx.cb, shinyQ);
  DrawCensus(cx, p, 'D');   // debug census (NVN_DRAW_CENSUS)
  rtCap.End();   // [rt] (a no-op unless this draw was captured; the build runs from the Fx post-tonemap slot, nvn_rt.cpp)
  FxAfterDraw(cx, p->st[1].key);   // [fx] post-tonemap slot (nvn_fx.cpp)
  AaAfterDraw(cx, p->st[1].key);   // SNAP_AA=smaa / SNAP_SHARPEN: in-place pass after the game's tonemap draw (nvn_aa.cpp)
  FgAfterDraw(cx, p->st[1].key);   // [framegen] HUD-less copy of the finished 3D scene (nvn_framegen.cpp)
  FcShotAfterDraw(cx, p->st[1].key);   // free camera clean shot (nvn_freecam.cpp)
  ShotAfterDraw(cx, p->st[1].key);   // screenshot key without the HUD (nvn_shot.cpp)
  if (thumbDraw) {
    fprintf(stderr, "[thumb] recorded\n");
    if (g_fdBurst <= 0) g_fdBurst = 60;   // request a burst of NVN_FRAME_DUMP frames
    if (getenv("NVN_RT_DUMP")) { char tag[64]; snprintf(tag, sizeof tag, "p%d_out_prog%d", g_presentCount, (int)p->id); DebugDumpSampledTexture(cx, cx.rt[0], tag); }
  }
}


// Indirect draws: the argument block is read from guest memory when the command replays (GPU-written argument buffers
// are not supported) and turned into an ordinary draw.
void DoIndirectDraw(ReplayCtx& cx, const Cmd& c) {
  PoolImpl* pool = nullptr; uint64_t off = 0;
  const bool indexed = c.op == Op::DrawIndexedIndirect;
  if (!cx.h->ResolveAddress(c.a[1], &pool, &off) || !pool->storage) { LogError("indirect draw: argument address 0x%llx is not in a CPU-visible pool", (unsigned long long)c.a[1]); return; }
  const uint32_t* a = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pool->storage) + off);
  Cmd d = c;
  if (!indexed) {   // {count, instanceCount, first, baseInstance}
    d.op = Op::Draw; d.i[1] = (int)a[2]; d.i[2] = (int)a[0]; d.i[3] = (int)a[3]; d.i[4] = (int)a[1];
  } else {          // {count, instanceCount, firstIndex, baseVertex, baseInstance}
    d.op = Op::DrawIndexed; d.i[2] = (int)a[0]; d.i[3] = (int)a[3]; d.i[4] = (int)a[4]; d.i[5] = (int)a[1];
    d.a[0] = c.a[0] + (uint64_t)a[2] * (c.i[1] == IDX_U32 ? 4 : c.i[1] == IDX_U16 ? 2 : 1);
  }
  DoDraw(cx, d);
}

static std::atomic<uint64_t> g_csElidedN{0}, g_csForcedN{0};
void DoDispatch(ReplayCtx& cx, const Cmd& c) {
  Host* h = cx.h;
  // a dispatch whose trailing barrier was left out (below) is closed by the next DoDispatch call, on every path out of it, early returns included
  bool keepOpen = false;
  struct Flush {   // a dispatch whose barrier was left out is followed by one that closes it, on every path out of here
    ReplayCtx& cx; const bool& keep;
    ~Flush() {
      if (!cx.csOpen || keep) return;
      VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
      m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      VkDependencyInfo di0{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di0.memoryBarrierCount = 1; di0.pMemoryBarriers = &m;
      vkCmdPipelineBarrier2(cx.cb, &di0);
      cx.csOpen = false; cx.csReads.clear(); cx.csWrites.clear();
    }
  } flush{cx, keepOpen};
  DrawState& d = *cx.ds;
  ProgramImpl* p = d.prog;
  if (!p || !p->layout || !p->st[5].valid) { LogError("dispatch without a usable compute program"); return; }
  GetDummies(h);
  EndRendering(cx);
  ScratchVec<TexWrite> twS;   // [cpuprof]
  std::vector<TexWrite>& tw = twS.get();
  if (!PrepareTextures(cx, d, p, tw, false)) return;
  std::string key = "CS" + std::to_string(p->id);
  VkPipeline pipe = VK_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    auto it = h->pipelines.find(key);
    if (it != h->pipelines.end()) pipe = it->second;
  }
  if (!pipe) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, p->st[5].mod, "main", nullptr};
    ci.layout = p->layout;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sgPinCs;
    PinSubgroup32(&ci.stage, &sgPinCs, p->st[5].mod);
    if (vkCreateComputePipelines(h->device, h->pipelineCache, 1, &ci, nullptr, &pipe) != VK_SUCCESS) { LogError("vkCreateComputePipelines failed"); return; }
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    h->pipelines[key] = pipe;
  }
  if (g_perfPassOn) { char lb[96]; snprintf(lb, sizeof lb, "cs p%d %dx%dx%d", (int)p->id, c.i[0], c.i[1], c.i[2]); PerfMark(cx.h, cx.cb, lb); }
  vkCmdBindPipeline(cx.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
  if (!WriteDescriptors(cx, d, p, tw, VK_PIPELINE_BIND_POINT_COMPUTE)) return;
  // Barrier elision between back-to-back skinning dispatches (perf pass 18, SNAP_CS_ELIDE, default on, =0 turns it off). The skinning shader (cache key
  // ada6de87ec03; SPIR-V checked: SSBOs 0 and 1 are only loaded, SSBO 2 is only stored) is dispatched about 25 times per frame, once per model, each
  // writing its own vertex range of SSBO 2 (cb0: destination byte offset +8, stride +12, count +16). NVN has no implicit ordering between
  // dispatches (the game puts a Barrier command where one is needed), so the full barrier we emit after every dispatch only serializes dispatches
  // that do not depend on each other. It is left out when the next work command is another dispatch and the check below finds no overlap between the
  // two dispatches' buffers and destination ranges; otherwise it is emitted before that next dispatch, exactly where the old one would have run.
  static const bool elide = [] { const char* e = getenv("SNAP_CS_ELIDE"); return !(e && *e == '0'); }();
  const bool skin = elide && !strncmp(p->st[5].key, "ada6de87ec03", 12);
  ReplayCtx::CsRange rdA{}, rdB{}, wr{}, wrDst{};
  bool hazard = false;
  if (skin) {
    auto rng = [&](int i) { const BufBind& bb = d.ssbo[5][i]; return ReplayCtx::CsRange{bb.pool, bb.off, bb.off + bb.range}; };
    rdA = rng(0); rdB = rng(1); wr = rng(2);
    wrDst = wr;
    const BufBind& ub = d.ubo[5][0];
    bool ok = wr.pool && rdA.pool && rdB.pool && ub.pool && ub.pool->storage && ub.range >= 20;
    if (ok) {
      const uint32_t* w = (const uint32_t*)((const uint8_t*)ub.pool->storage + ub.off);
      const uint64_t lo = wr.lo + w[2], hi = lo + (uint64_t)w[3] * w[4];
      ok = w[3] && hi > lo && hi <= wr.hi;
      wrDst = {wr.pool, lo, hi};
    }
    auto hit = [](const ReplayCtx::CsRange& a, const ReplayCtx::CsRange& bq) { return a.pool == bq.pool && a.lo < bq.hi && bq.lo < a.hi; };
    hazard = !ok || hit(wr, rdA) || hit(wr, rdB);   // unknown destination, or the output aliases the shader's own inputs
    if (cx.csOpen && !hazard) {
      for (const auto& r : cx.csWrites) if (hit(r, wrDst) || hit(r, rdA) || hit(r, rdB)) hazard = true;   // WAW, RAW
      for (const auto& r : cx.csReads) if (hit(r, wr)) hazard = true;                                      // WAR
    }
    if (cx.csOpen && hazard) { snap_perf::g_cs_forced.fetch_add(1, std::memory_order_relaxed); if ((++g_csForcedN & 255) == 1) Log("skin dispatch barrier forced (%llu so far, %llu elided)", (unsigned long long)g_csForcedN.load(), (unsigned long long)g_csElidedN.load()); }   // Flush closes the open run before this dispatch's own barrier decision below
  }
  if (cx.csOpen && (hazard || !skin)) {   // overlap, or a different dispatch follows: the barrier goes in before it, not after
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di0{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di0.memoryBarrierCount = 1; di0.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cx.cb, &di0);   // the barrier the previous dispatch left out, before this one starts
    cx.csOpen = false; cx.csReads.clear(); cx.csWrites.clear();
  }
  vkCmdDispatch(cx.cb, (uint32_t)std::max(c.i[0], 1), (uint32_t)std::max(c.i[1], 1), (uint32_t)std::max(c.i[2], 1));
  if (skin && cx.nextIsDispatch && !hazard) {
    cx.csOpen = true; cx.csReads.push_back(rdA); cx.csReads.push_back(rdB); cx.csWrites.push_back(wrDst);
    keepOpen = true; snap_perf::g_cs_elided.fetch_add(1, std::memory_order_relaxed); if ((++g_csElidedN & 8191) == 1) Log("skin dispatch barriers elided %llu, forced %llu", (unsigned long long)g_csElidedN.load(), (unsigned long long)g_csForcedN.load());
    return;
  }
  cx.csOpen = false; cx.csReads.clear(); cx.csWrites.clear();   // the barrier below also covers the dispatches before this one
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // make compute writes visible to everything that follows
  mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cx.cb, &di);
}

struct CopyRegion { int x, y, z, w, h, d; };
void DoCopyBufferToTexture(ReplayCtx& cx, const Cmd& c) {
  Host* h = cx.h;
  TextureImpl* t = c.tex[0];
  if (!t || !t->EnsureImage() || c.blob.size() < sizeof(CopyRegion) + sizeof(TexView)) return;
  CopyRegion r; TexView v;
  memcpy(&r, c.blob.data(), sizeof r);
  memcpy(&v, c.blob.data() + sizeof r, sizeof v);
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(c.a[0], &pool, &off)) { LogError("CopyBufferToTexture: source 0x%llx is not in any memory pool", (unsigned long long)c.a[0]); return; }
  if (!PoolBacked(pool)) return;   // [audit F6] acquire
  if (ShinyActive()) ShinyBufUpload(t, pool->storage ? static_cast<const uint8_t*>(pool->storage) + off : nullptr);
  if (HdPhotoOn()) HdPhotoUploadFrom(t, pool->storage ? static_cast<const uint8_t*>(pool->storage) + off : nullptr);   // HD photos: a decoded album photo
  static const bool kTexDump = getenv("NVN_TEX_DUMP") != nullptr;   // load pass: read once
  if (pool->storage && kTexDump) {
    const uint64_t n = (uint64_t)r.w * std::max(r.h, 1) * std::max(r.d, 1) * t->fi->bytes / std::max<uint32_t>(t->fi->bw * t->fi->bh, 1);
    DebugDumpTexture(t, "copy", static_cast<const uint8_t*>(pool->storage) + off, std::min<uint64_t>(n, pool->size - off), (uint32_t)r.w, (uint32_t)r.h);
  }
  if (DebugDrawLogActive()) DebugDrawLog("CopyBufferToTexture t=%p fmt=%x %dx%d lvl=%u region(%d,%d,%d %dx%dx%d) flags=%d\n", (void*)t, t->b.format, t->b.width, t->b.height, c.i[0], r.x, r.y, r.z, r.w, r.h, r.d, c.i[1]);
  EndRendering(cx);
  t->uploaded = true;
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  // [nvnfix] cube maps are layered images too (face = layer, as IsLayered in nvn_xfer.cpp): r.z used to land in imageOffset.z of a 2D image
  const bool cube = t->b.target == TT_CUBEMAP || t->b.target == TT_CUBEMAP_ARRAY;
  const bool layered = t->b.target == TT_2D_ARRAY || t->b.target == TT_1D_ARRAY || cube;
  if (cube) {
    LifeCount(LS_CUBE_COPY);
    static int logged = 0;
    if (logged++ < 12 && getenv("SNAP_NVN_STATS"))
      fprintf(stderr, "[nvnstats] cube CopyBufferToTexture t=%p %dx%d fmt 0x%x target %d levels %d: level %u region (%d,%d,%d %dx%dx%d) view layers %u\n",
              (void*)t, t->b.width, t->b.height, t->b.format, t->b.target, t->b.levels, (c.i[0] && (v.flags & 1)) ? v.lvlBase : 0u, r.x, r.y, r.z, r.w, r.h, r.d,
              (c.i[0] && (v.flags & 2)) ? v.layerBase : 0u);
  }
  const uint32_t level = (c.i[0] && (v.flags & 1)) ? v.lvlBase : 0;
  const uint32_t layerBase = (c.i[0] && (v.flags & 2)) ? v.layerBase : 0;
  VkBufferImageCopy bc{};
  bc.bufferOffset = off;
  bc.imageSubresource.aspectMask = (t->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_ASPECT_COLOR_BIT : (t->fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
  bc.imageSubresource.mipLevel = level;
  if (layered) {
    bc.imageSubresource.baseArrayLayer = layerBase + (uint32_t)r.z; bc.imageSubresource.layerCount = (uint32_t)std::max(r.d, 1);
    bc.imageOffset = {r.x, r.y, 0}; bc.imageExtent = {(uint32_t)r.w, (uint32_t)std::max(r.h, 1), 1};
  } else {
    bc.imageSubresource.baseArrayLayer = 0; bc.imageSubresource.layerCount = 1;
    bc.imageOffset = {r.x, r.y, r.z}; bc.imageExtent = {(uint32_t)r.w, (uint32_t)std::max(r.h, 1), (uint32_t)std::max(r.d, 1)};
  }
  SyncPoolRangeOnce(pool, off, (uint64_t)bc.imageExtent.width * bc.imageExtent.height * std::max<uint64_t>(bc.imageExtent.depth, bc.imageSubresource.layerCount) * t->fi->bytes / (t->fi->bw * t->fi->bh) + 16);
  if (XferUploadScaled(cx, t, pool->buffer, bc)) return;   // render-scaled target: guest-size stand-in + blit (nvn_xfer.cpp)
  if (!XferFitCopy(t, bc, "CopyBufferToTexture")) return;
  vkCmdCopyBufferToImage(cx.cb, pool->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
}

}  // namespace

extern "C" void snap_ui_smooth_set(int on) { UiSmoothUser().store(on ? 1 : 0, std::memory_order_relaxed); }   // F1 > Frame rate > Smooth interface (runtime/ui/ui_options.cpp)
extern "C" int snap_ui_smooth_get() { return UiSmoothUser().load(std::memory_order_relaxed); }

VkPipeline CreateGraphicsPipeline(Host* h, const PipeDesc& pd, StageProg* const st[6], VkPipelineLayout layout) { return CreateGraphicsPipelineImpl(h, pd, st, layout, nullptr); }
}  // namespace nvn
extern "C" void snap_gpl_stats(uint32_t* links, uint32_t* libsBuilt, uint32_t* libsReused, uint32_t* hits, uint32_t* fails, double* linkMs, double* libMs) {
  nvn::GplCache& g = nvn::Gpl();
  if (links) *links = g.links;
  if (libsBuilt) *libsBuilt = g.built;
  if (libsReused) *libsReused = g.reused;
  if (hits) *hits = g.hits;
  if (fails) *fails = g.linkFail;
  if (linkMs) *linkMs = (double)g.linkNs.load() / 1e6;
  if (libMs) *libMs = (double)g.libNs.load() / 1e6;
}
namespace nvn {
VkPipeline CreateGraphicsPipelineLatency(Host* h, const PipeDesc& pd, StageProg* const st[6], VkPipelineLayout layout, bool* optLater) { return CreateGraphicsPipelineImpl(h, pd, st, layout, optLater); }

// ---- exported to nvn_vk.cpp -------------------------------------------------------------------------------------------------
void EndRendering(ReplayCtx& c) {
  if (!c.rendering) { FlushCompressedTargets(c); return; }
  if (c.occOpen) { vkCmdEndQuery(c.cb, c.h->occPool, (c.h->occNext - 1) % 8192); c.occOpen = false; }
  vkCmdEndRendering(c.cb);
  c.rendering = false;
  // Rendering scopes only order attachment access inside one scope: make the writes visible to everything after.
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(c.cb, &di);
  FlushCompressedTargets(c);
}

// Block-compressed render targets (photo thumbnails, 2026-11-12). The game makes each 360x240 photo thumbnail in two passes: a
// downscale into an RGBA8 target, then a fragment-shader BC1 encoder whose render target is the BC1 texture itself (one output
// texel = one 4x4 block, 8 bytes). Vulkan cannot render into a compressed image, so the draw went nowhere and every thumbnail
// (shutter pop-up, photo select grid and preview) stayed black. The draw now renders into an uncompressed stand-in of
// ceil(w/4) x ceil(h/4) texels with the block's size (the view's format when it has one of that size, else RG32F / RGBA32F, whose
// float output keeps the raw bits), and each finished scope copies it block for block into the BC texture (vkCmdCopyImage
// allows compressed <-> uncompressed copies of equal block size). NVN_NO_BC_RT=1 restores the old behavior.
void RedirectCompressedTargets(ReplayCtx& c) {
  static const bool off = getenv("NVN_NO_BC_RT") != nullptr;
  for (int k = 0; k < 8; k++) {
    c.bcDst[k] = nullptr;
    TextureImpl* t = c.rt[k];
    if (off || !t || !t->EnsureImage() || !t->fi || t->fi->bw <= 1 || t->b.samples > 1) continue;
    const TexView* v = c.rtHasView[k] ? &c.rtView[k] : nullptr;
    const SubRange rr = RtRange(t, v);
    const uint32_t lw = t->LevelW(rr.lb), lh = t->LevelH(rr.lb);
    const uint32_t sw = (lw + t->fi->bw - 1) / t->fi->bw, sh = (lh + t->fi->bh - 1) / t->fi->bh;
    int fmt = t->fi->bytes == 8 ? FMT_RG32F : t->fi->bytes == 16 ? FMT_RGBA32F : 0;
    if (v && (v->flags & 4)) if (const FormatInfo* vf = LookupFormat(v->format)) if (vf->bw == 1 && vf->bytes == t->fi->bytes) fmt = v->format;
    if (!fmt) continue;
    TextureImpl* s = t->bcShadow;
    if (s && (s->b.width != (int)sw || s->b.height != (int)sh || s->b.format != fmt)) { Retire(GK_TEXTURE, s); s = t->bcShadow = nullptr; LifeCount(LS_BC_RECREATE); }   // [nvnfix] earlier commands of this submit may still use it: freed after the GPU
    if (!s) {
      s = new TextureImpl();
      s->host = t->host;
      s->b = TextureBuilder{};
      s->b.device = t->b.device; s->b.target = TT_2D; s->b.width = (int)sw; s->b.height = (int)sh; s->b.depth = 1; s->b.levels = 1;
      s->b.format = fmt; s->b.samples = 1;
      if (!s->EnsureImage()) { delete s; continue; }
      t->bcShadow = s;
      static int logged = 0;
      if (logged++ < 8) Log("compressed render target %p (%dx%d format 0x%x, view flags 0x%x format 0x%x): drawing into a %ux%u format 0x%x stand-in",
                            (void*)t, t->b.width, t->b.height, t->b.format, v ? v->flags : 0, v ? v->format : 0, sw, sh, fmt);
    }
    s->uploaded = true;
    c.bcDst[k] = t;
    c.rt[k] = s;
    c.rtHasView[k] = false; c.rtView[k] = TexView{};
    // the stand-in keeps the level / layer the game selected on the BC texture in bcSub
    s->bcSubLevel = rr.lb; s->bcSubLayer = rr.yb;
  }
}

void FlushCompressedTargets(ReplayCtx& c) {
  if (!c.bcDirty) return;
  c.bcDirty = false;
  for (int k = 0; k < 8; k++) {
    TextureImpl* t = c.bcDst[k]; TextureImpl* s = c.rt[k];
    if (!t || !s || t->bcShadow != s) continue;
    Transition(c.cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    SubRange dr; dr.lb = s->bcSubLevel; dr.ln = 1; dr.yb = s->bcSubLayer; dr.yn = 1;
    TransitionRange(c.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dr);
    VkImageCopy ic{};
    ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->bcSubLevel, s->bcSubLayer, 1};
    ic.extent = {(uint32_t)s->b.width, (uint32_t)s->b.height, 1};
    vkCmdCopyImage(c.cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    t->uploaded = true;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(c.cb, &di);
  }
}

// ClearColor with a partial channel mask: a fullscreen triangle with a color write mask (vkCmdClearColorImage cannot mask).
void DoMaskedClear(ReplayCtx& cx, TextureImpl* t, const TexView* tv, int mask, const float* color) {
  Host* h = cx.h;
  static VkShaderModule mods[4];
  static VkPipelineLayout layout;
  static Host* owner;
  if (owner != h) {
    owner = h;
    auto mk = [&](const uint32_t* code, size_t bytes) {
      VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      mi.codeSize = bytes; mi.pCode = code;
      VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(h->device, &mi, nullptr, &m); return m;
    };
    mods[0] = mk(kClearVertSpv, sizeof kClearVertSpv); mods[1] = mk(kClearFloatSpv, sizeof kClearFloatSpv);
    mods[2] = mk(kClearUintSpv, sizeof kClearUintSpv); mods[3] = mk(kClearIntSpv, sizeof kClearIntSpv);
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(h->device, &li, nullptr, &layout);
    h->clearLayout = layout;   // freed with the host (DestroyHostExtras); the modules join the host's variant list
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    for (VkShaderModule m : mods) h->variantMods.push_back(m);
  }
  const int kind = t->fi->kind;   // 0 float/norm, 1 uint, 2 sint
  const std::string key = "CLR" + std::to_string((int)t->fi->vk) + "_" + std::to_string(mask) + "_" + std::to_string(t->b.samples > 1 ? t->b.samples : 1);
  VkPipeline pipe = VK_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    auto it = h->pipelines.find(key);
    if (it != h->pipelines.end()) pipe = it->second;
  }
  if (!pipe) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mods[0], "main", nullptr},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mods[1 + kind], "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (VkSampleCountFlagBits)(t->b.samples > 1 ? t->b.samples : 1);
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = (VkColorComponentFlags)(mask & 0xF);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkFormat fmt = t->fi->vk;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
    gi.layout = layout;
    if (vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &pipe) != VK_SUCCESS) { LogError("masked clear: pipeline creation failed"); return; }
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    h->pipelines[key] = pipe;
  }
  EndRendering(cx);
  t->uploaded = true;
  SubRange rr;
  VkImageView cv = t->RenderView(tv, &rr);
  TransitionRange(cx.cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
  const uint32_t cw = t->LevelW(rr.lb), ch = t->LevelH(rr.lb);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = cv; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {cw, ch}}; ri.layerCount = rr.yn; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cx.cb, &ri);
  VkViewport vpt{0, 0, (float)cw, (float)ch, 0, 1}; VkRect2D sc{{0, 0}, {cw, ch}};
  vkCmdSetViewport(cx.cb, 0, 1, &vpt); vkCmdSetScissor(cx.cb, 0, 1, &sc);
  vkCmdBindPipeline(cx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdPushConstants(cx.cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color);
  vkCmdDraw(cx.cb, 3, 1, 0, 0);
  vkCmdEndRendering(cx.cb);
  cx.vpSet = cx.scSet = false;   // the next scope sets its own defaults
}

void ReplayBegin(ReplayCtx& c) {
  if (!c.q->persistDs) c.q->persistDs = new DrawState();
  c.ds = c.q->persistDs;
  Dummies* d = GetDummies(c.h);
  if (!d->imagesReady) {
    d->imagesReady = true;
    ImgBarrier(c.cb, d->sampledImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue white{{1, 1, 1, 1}};
    VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c.cb, d->sampledImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &rg);
    ImgBarrier(c.cb, d->sampledImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImgBarrier(c.cb, d->storageImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    ImgBarrier(c.cb, d->depImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue one{1.0f, 0};
    VkImageSubresourceRange drg{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(c.cb, d->depImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &drg);
    ImgBarrier(c.cb, d->depImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    const struct { VkImage img; uint32_t layers; } typed[] = {{d->arrImg, 6}, {d->img3d, 1}, {d->img1d, 2}};
    for (auto& ty : typed) {
      ImgBarrier(c.cb, ty.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ty.layers);
      VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ty.layers};
      vkCmdClearColorImage(c.cb, ty.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &rg);
      ImgBarrier(c.cb, ty.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ty.layers);
    }
  }
}

void ReplayEnd(ReplayCtx& c) {
  EndRendering(c);
  c.ds = nullptr;   // the state itself lives in the queue (QueueImpl::persistDs)
}

// [nvnfix] review_nvn.md 10: the persistent draw state outlives command lists. Before the graveyard frees memory pools, programs and
// texture / sampler pools, every binding that still names one is dropped (a later draw that relies on it binds nothing instead of a
// destroyed VkBuffer or a freed program). Called under queueMutex, so no replay is running.
void PersistScrub(QueueImpl* q, const std::unordered_set<const void*>& dead) {
  DrawState* d = q ? q->persistDs : nullptr;
  if (!d || dead.empty()) return;
  uint64_t n = 0;
  auto buf = [&](BufBind& b) { if (b.pool && dead.count(b.pool)) { b = BufBind{}; n++; } };
  for (auto& b : d->vb) buf(b);
  for (auto& st : d->ubo) for (auto& b : st) buf(b);
  for (auto& st : d->ssbo) for (auto& b : st) buf(b);
  bool progGone = d->prog && dead.count(d->prog);
  for (auto& p : d->stageProg) if (p && dead.count(p)) { p = nullptr; progGone = true; }
  if (progGone) { d->prog = ProgramResolve(q->host, d->stageProg); n++; }
  if (d->tp && dead.count(d->tp)) { d->tp = nullptr; n++; }
  if (d->sp && dead.count(d->sp)) { d->sp = nullptr; n++; }
  if (n) LifeCount(LS_PERSIST_SCRUB, n);
}

// SNAP_SHADOW_DISTANCE=<k> (1 = original; SNAP_LIGHTING=high uses 2): widens the dynamic shadow map's coverage k times. The game fits one 1024^2
// orthographic map per frame (light view-projection in a 64-byte VS buffer at slot 9 of the ShadowCast draws; the receivers read the same matrix,
// remapped to texture space, at floats 16..31 of a 132-byte buffer at slot 9). Called from BindUbo BEFORE the buffer is mirrored to the GPU: the
// x and y columns are scaled by 1/k about the map centre. Idempotent: a buffer the game did not rewrite (it still holds our output) is skipped.
static float ShadowDistK() {
  const float k = SNAP_LIVE(float, ([] {   // the Lighting preset and Shadow distance rows apply with no restart (hle/live.h)
    float v = snaplight::shadow_distance();   // SNAP_LIGHTING preset (hle/lighting_preset.h)
    if (const char* e = getenv("SNAP_SHADOW_DISTANCE")) { float f = (float)atof(e); if (f >= 1.0f && f <= 16.0f) v = f; }
    return v;
  }()));
  return snaplight::ab_on((unsigned long long)g_presentCount) ? k : 1.0f;   // SNAP_LIGHT_AB: even blocks use the original
}

// P3 (lighting plan): the fade of the dynamic shadow lives in PerViewCBuffer (NVN slot c3 = ubo index 0, 744 bytes): dynamicShadowFarClip +0x2e0 and
// dynamicShadowFadeLength +0x2e4. Scaled by the same k as the coverage (SNAP_SHADOW_FADE=<scale> overrides). Only those two floats change, and only
// on binds whose targets are not the Evaluation depth (DEPTH32F_STENCIL8) or a 360x240 photo target. Idempotent (the game rewrites the bank each
// frame; a bank that still holds our output is skipped). NVN_P1_LOG=1 logs the original and scaled values (probe for the plan's P1).
static void ShadowFadeOnBind(Host* h, ReplayCtx& cx, int slot, uint64_t addr, uint64_t size) {
  static const bool p1 = getenv("NVN_P1_LOG") != nullptr;
  static const float fkSet = [] { const char* e = getenv("SNAP_SHADOW_FADE"); const float f = e ? (float)atof(e) : 0.0f; return (f >= 1.0f && f <= 16.0f) ? f : 0.0f; }();
  const float fk = !snaplight::ab_on((unsigned long long)g_presentCount) ? 1.0f : fkSet > 0.0f ? fkSet : ShadowDistK();
  if ((fk <= 1.0f && !p1) || slot != 0 || !addr || size < 0x2e8) return;   // PerViewCBuffer = ubo binding index 0 (NVN c3), 744 bytes
  if (cx.rt[8] && cx.rt[8]->b.format == 0x36) return;                                              // Evaluation depth target
  if (cx.rt[0] && cx.rt[0]->b.width == 360 && cx.rt[0]->b.height == 240) return;                  // photo / thumbnail target
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(addr, &pool, &off) || !pool->storage || off + size > pool->size) return;
  float* f = (float*)((uint8_t*)pool->storage + off);
  const float farc = f[0x2e0 / 4], fade = f[0x2e4 / 4];
  if (!(farc > 0.0f && farc < 1.0e6f && fade >= 0.0f && fade < 1.0e6f)) return;                     // not a PerViewCBuffer (or garbage)
  static std::unordered_map<float*, std::array<float, 2>> done;
  auto it = done.find(f);
  if (p1) {
    static int n = 0;
    if (n++ < 40) fprintf(stderr, "[p1] c3 bind p%d @%p size %llu rt0=%ux%u rt8fmt=%x farclip %g fade %g (cached ours %d)", g_presentCount, (void*)f, (unsigned long long)size,
                      cx.rt[0] ? cx.rt[0]->b.width : 0u, cx.rt[0] ? cx.rt[0]->b.height : 0u, cx.rt[8] ? cx.rt[8]->b.format : 0, farc, fade,
                      (int)(it != done.end() && it->second[0] == farc && it->second[1] == fade));
  }
  if (fk <= 1.0f) return;
  if (it != done.end() && it->second[0] == farc && it->second[1] == fade) return;                  // our own output from an earlier bind
  f[0x2e0 / 4] = farc * fk; f[0x2e4 / 4] = fade * fk;
  if (done.size() > 4096) done.clear();
  done[f] = {f[0x2e0 / 4], f[0x2e4 / 4]};
  static int n2 = 0;
  if (n2++ < 6) fprintf(stderr, "shadow fade x%.2f applied: farclip %g -> %g, fade length %g -> %g at %p", fk, farc, f[0x2e0 / 4], fade, f[0x2e4 / 4], (void*)f);
}

// The dynamic shadow map is the depth texture the receivers bind as texture control block C (set 2 binding 33); recorded per draw in DoDraw. Until one has been
// seen every 1024 depth target counts (the first frame only).
static bool ShadowDynTexture(const TextureImpl* t) { return !g_dynShadowTex || t == g_dynShadowTex; }

// Shadow distance, caster side only: the light projection of the dynamic map's casters is widened k times (x and y columns scaled by 1/k, 64-byte VS buffer
// at slot 9). The receivers are NOT edited in guest memory (an earlier version also scaled the 132-byte buffer's matrix, which only some receiver shaders
// read: the others kept the unscaled lookup, giving 1/k-size, displaced and speckled shadows). Instead PatchShadowPcf remaps the dynamic map's lookup
// coordinate in every receiver fragment shader: (uv - 0.5) / k + 0.5. Idempotent per buffer.
static void ShadowDistanceOnBind(Host* h, ReplayCtx& cx, int stage, int slot, uint64_t addr, uint64_t size) {
  const float k = ShadowDistK();
  ShadowFadeOnBind(h, cx, slot, addr, size);
  if (k <= 1.0f || slot != 9 || !addr) return;
  const bool cast = stage == 0 && size == 64 && cx.rt[8] && !cx.rt[0] && cx.rt[8]->b.width == 1024 && cx.rt[8]->b.height == 1024 && ShadowDynTexture(cx.rt[8]);
  if (!cast) return;
  PoolImpl* pool = nullptr; uint64_t off = 0;
  if (!h->ResolveAddress(addr, &pool, &off) || !pool->storage || off + size > pool->size) return;
  float* f = (float*)((uint8_t*)pool->storage + off);
  float* m = f;
  if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return;
  static std::unordered_map<float*, std::array<float, 16>> done;
  auto it = done.find(f);
  if (it != done.end() && !memcmp(it->second.data(), m, 64)) return;   // our own output from an earlier bind
  const float inv = 1.0f / k;
  for (int r = 0; r < 4; r++) for (int c = 0; c < 2; c++) m[r * 4 + c] *= inv;
  std::array<float, 16> o; memcpy(o.data(), m, 64);
  if (done.size() > 4096) done.clear();
  done[f] = o;
  static int n = 0;
  if (n++ < 6) Log("shadow distance x%.2f applied (cast) at %p", k, (void*)f);
}

void ReplayDrawOp(ReplayCtx& cx, const Cmd& c) {
  DrawState& d = *cx.ds;
  Host* h = cx.h;
  switch (c.op) {
    case Op::BindProgram: {
      ProgramImpl* pr = static_cast<ProgramImpl*>(c.p[0]);
      const int mask = c.i[0] ? c.i[0] : 0x3F;
      for (int s = 0; s < 6; s++) if (mask & (1 << s)) d.stageProg[s] = pr;
      d.prog = ProgramResolve(h, d.stageProg);
      break;
    }
    case Op::BindBlend: {
      BlendSt b; memcpy(&b, &c.a[0], sizeof b);
      if (b.target < 8) d.blend[b.target] = b;
      break;
    }
    case Op::BindColor: memcpy(&d.color, &c.a[0], sizeof d.color); break;
    case Op::BindChannelMask: memcpy(&d.chmask, &c.a[0], sizeof d.chmask); break;
    case Op::BindMultisample: memcpy(&d.ms, &c.a[0], sizeof d.ms); break;
    case Op::BindPolygon: memcpy(&d.poly, &c.a[0], sizeof d.poly); break;
    case Op::BindDepthStencil: memcpy(&d.ds, &c.a[0], sizeof d.ds); break;
    case Op::BindVertexAttribs:
      d.nattr = std::min(c.i[0], 16);
      memcpy(d.attr, c.blob.data(), (size_t)d.nattr * sizeof(VaSt));
      break;
    case Op::BindVertexStreams:
      d.nstream = std::min(c.i[0], 16);
      memcpy(d.stream, c.blob.data(), (size_t)d.nstream * sizeof(VsSt));
      break;
    case Op::BindVertexBuffers:
      for (int k = 0; k < c.i[1]; k++) {
        int idx = c.i[0] + k;
        if (idx < 0 || idx >= 16) continue;
        uint64_t addr, size;
        memcpy(&addr, c.blob.data() + 16 * k, 8); memcpy(&size, c.blob.data() + 16 * k + 8, 8);
        d.vb[idx] = ResolveBuf(h, addr, size, 0); d.vbAddr[idx] = addr;
      }
      break;
    case Op::BindUbo:
      ShadowDistanceOnBind(h, cx, c.i[0], c.i[1], c.a[0], c.a[1]);
      if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 16) d.ubo[c.i[0]][c.i[1]] = ResolveBuf(h, c.a[0], c.a[1], kMaxUboRange);
      break;
    case Op::BindSsbo:
      if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 16) d.ssbo[c.i[0]][c.i[1]] = ResolveBuf(h, c.a[0], c.a[1], 0);
      break;
    case Op::BindTexture:
      if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 32) { d.tex[c.i[0]][c.i[1]] = c.a[0]; d.texSet[c.i[0]][c.i[1]] = true; }
      break;
    case Op::BindImage:
      if (c.i[0] >= 0 && c.i[0] < 6 && c.i[1] >= 0 && c.i[1] < 8) { d.img[c.i[0]][c.i[1]] = c.a[0]; d.imgSet[c.i[0]][c.i[1]] = true; }
      break;
    case Op::SetTexturePool: d.tp = static_cast<TexPoolImpl*>(c.p[0]); break;
    case Op::SetSamplerPool: d.sp = static_cast<SampPoolImpl*>(c.p[0]); break;
    case Op::StencilValueMask: case Op::StencilMask: case Op::StencilRef: {
      int* arr = c.op == Op::StencilValueMask ? d.stCompare : c.op == Op::StencilMask ? d.stWrite : d.stRef;
      if (c.i[0] & FACE_FRONT) arr[0] = c.i[1];
      if (c.i[0] & FACE_BACK) arr[1] = c.i[1];
      break;
    }
    case Op::SetAlphaRef: d.alphaRef = c.f[0]; break;
    case Op::SetPatchSize: d.patchSize = c.i[0]; break;
    case Op::PolygonOffset: d.biasFactor = c.f[0]; d.biasUnits = c.f[1]; d.biasClamp = c.f[2]; break;
    case Op::Draw: case Op::DrawIndexed: DoDraw(cx, c); break;
    case Op::CopyBufferToTexture: DoCopyBufferToTexture(cx, c); break;
    case Op::DrawIndirect: case Op::DrawIndexedIndirect:
      if (DebugDrawLogActive()) DebugDrawLog("INDIRECT op=%d\n", (int)c.op);
      DoIndirectDraw(cx, c); break;
    case Op::Dispatch:
      if (DebugDrawLogActive()) DebugDrawLog("DISPATCH prog=%d %d %d %d cs=%s\n", d.prog ? (int)d.prog->id : -1, c.i[0], c.i[1], c.i[2], d.prog ? d.prog->st[5].key : "");
      if (DebugDrawLogActive() && d.prog) {
        for (auto& cb : d.prog->st[5].cb) {
          if (cb.slot < 3 || cb.slot - 3 >= 16) continue;
          const BufBind& ub = d.ubo[5][cb.slot - 3];
          DebugDrawLog("   cb%d off 0x%llx range %llu:", cb.slot - 3, (unsigned long long)ub.off, (unsigned long long)ub.range);
          if (ub.pool && ub.pool->storage) { const uint32_t* w = (const uint32_t*)((const uint8_t*)ub.pool->storage + ub.off); for (int k = 0; k < 12 && (uint64_t)k * 4 < ub.range; k++) DebugDrawLog(" %08x", w[k]); }
          DebugDrawLog("\n");
        }
        for (int i = 0; i < 4; i++) if (d.ssbo[5][i].pool) DebugDrawLog("   ssbo%d off 0x%llx size 0x%llx\n", i, (unsigned long long)d.ssbo[5][i].off, (unsigned long long)d.ssbo[5][i].range);
      }
      DoDispatch(cx, c); break;
    case Op::CopyTextureToBuffer: DoCopyTextureToBuffer(cx, c); break;
    case Op::CopyTextureToTexture: DoCopyTextureToTexture(cx, c); break;
    case Op::ReportCounter: DoReportCounter(cx, c); break;
    default: break;
  }
}

VkDescriptorPool AcquireDescriptorPool(QueueImpl* q) {
  if (!q->freePools.empty()) { auto p = q->freePools.back(); q->freePools.pop_back(); return p; }
  const VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 * 24}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 * 8},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 * 32}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 * 8}};
  VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  ci.maxSets = 256; ci.poolSizeCount = 4; ci.pPoolSizes = sz;
  VkDescriptorPool p = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(q->host->device, &ci, nullptr, &p) != VK_SUCCESS) { LogError("vkCreateDescriptorPool failed (out of memory?)"); return VK_NULL_HANDLE; }   // [nvnfix]
  return p;
}

// ---- pipeline cache persistence + shutdown ----------------------------------------------------------------------------------
// The pipeline cache, the recorded pipeline keys and the background compile pool live in nvn_pipeline.cpp (versioned files in the shader
// cache directory, saved periodically, not only at teardown).
void LoadPipelineCache(Host* h) { PipelineServiceInit(h); }

void DestroyHostExtras(Host* h) {
  DestroyQueryPools(h);
  PipelineServiceShutdown(h);   // joins the compile workers, saves the pipeline cache, destroys the graphics pipelines
  for (auto& kv : h->pipelines) vkDestroyPipeline(h->device, kv.second, nullptr);
  h->pipelines.clear();
  for (VkShaderModule m : h->variantMods) vkDestroyShaderModule(h->device, m, nullptr);
  h->variantMods.clear();
  if (h->clearLayout) { vkDestroyPipelineLayout(h->device, h->clearLayout, nullptr); h->clearLayout = VK_NULL_HANDLE; }
  if (Dummies* d = h->dummies) {
    if (d->zero) vkDestroyBuffer(h->device, d->zero, nullptr);
    if (d->support) vkDestroyBuffer(h->device, d->support, nullptr);
    if (d->zeroMem) vkFreeMemory(h->device, d->zeroMem, nullptr);
    if (d->supportMem) vkFreeMemory(h->device, d->supportMem, nullptr);
    if (d->sampledView) vkDestroyImageView(h->device, d->sampledView, nullptr);
    if (d->storageView) vkDestroyImageView(h->device, d->storageView, nullptr);
    if (d->sampledImg) vkDestroyImage(h->device, d->sampledImg, nullptr);
    if (d->storageImg) vkDestroyImage(h->device, d->storageImg, nullptr);
    if (d->sampledMem) vkFreeMemory(h->device, d->sampledMem, nullptr);
    if (d->storageMem) vkFreeMemory(h->device, d->storageMem, nullptr);
    for (VkImageView v : {d->v2da, d->vcube, d->vcubea, d->v3d, d->v1d, d->v1da}) if (v) vkDestroyImageView(h->device, v, nullptr);
    for (VkImage i : {d->arrImg, d->img3d, d->img1d}) if (i) vkDestroyImage(h->device, i, nullptr);
    for (VkDeviceMemory m : {d->arrMem, d->mem3d, d->mem1d}) if (m) vkFreeMemory(h->device, m, nullptr);
    if (d->vdep) vkDestroyImageView(h->device, d->vdep, nullptr);
    if (d->depImg) vkDestroyImage(h->device, d->depImg, nullptr);
    if (d->depMem) vkFreeMemory(h->device, d->depMem, nullptr);
    if (d->cmpSampler) vkDestroySampler(h->device, d->cmpSampler, nullptr);
    if (d->sampler) vkDestroySampler(h->device, d->sampler, nullptr);
    delete d;
    h->dummies = nullptr;
  }
}

}  // namespace nvn
