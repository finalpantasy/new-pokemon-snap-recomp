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
#include <deque>
#include <fstream>

#include "nvn_internal.h"
#include "nvn_clear_spv.h"
#include "nvn_state.h"

namespace nvn {

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

// ---- pipelines ----------------------------------------------------------------------------------------------------------
struct RtInfo { VkFormat color[8]; int nColor; VkFormat depth; VkFormat stencil; int samples; };

VkPipeline GetPipeline(ReplayCtx& cx, DrawState& d, VkPrimitiveTopology topo, const RtInfo& rt) {
  Host* h = cx.h;
  ProgramImpl* p = d.prog;
  std::string key;
  Put(key, p->id); Put(key, (int)topo);
  Put(key, d.nattr);
  for (int i = 0; i < d.nattr; i++) Put(key, d.attr[i].v);
  Put(key, d.nstream);
  for (int i = 0; i < d.nstream; i++) Put(key, d.stream[i]);
  for (int i = 0; i < rt.nColor; i++) { Put(key, d.blend[i]); Put(key, rt.color[i]); }
  Put(key, rt.nColor); Put(key, d.color.blendEn); Put(key, d.chmask.mask);
  const int af = d.color.alphaFunc;
  const bool alpha = af >= 1 && af <= 7 && !p->st[1].spv.empty() && p->st[1].valid;
  Put(key, alpha ? af : 0); if (alpha && af != 1) Put(key, d.alphaRef);
  if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) Put(key, d.patchSize);
  Put(key, d.ms); Put(key, d.poly); Put(key, d.ds.bits); Put(key, rt.depth); Put(key, rt.stencil); Put(key, rt.samples);
  {
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    auto it = h->pipelines.find(key);
    if (it != h->pipelines.end()) return it->second;
  }

  // Dual-source blending: attachment 0 uses a SRC1 factor and the fragment shader has a second color output to feed it. The
  // translator knows nothing about it, so the two lowest outputs are re-decorated as Location L Index 0/1 (PatchFragmentOutputs).
  // Without a second output, or on any other attachment (Vulkan allows only maxFragmentDualSrcAttachments), SRC1 degrades to SRC.
  auto usesSrc1 = [&](const BlendSt& b) {
    for (int f : {b.srcRgb, b.dstRgb, b.srcA, b.dstA}) if (f >= BF_SRC1_COLOR && f <= BF_ONE_MINUS_SRC1_ALPHA) return true;
    return false;
  };
  const bool dual = rt.nColor > 0 && ((d.color.blendEn >> 0) & 1) && usesSrc1(d.blend[0]) && h->features.dualSrcBlend &&
                    h->props.limits.maxFragmentDualSrcAttachments >= 1 && p->st[1].valid && p->st[1].outLocs.size() >= 2;
  std::vector<VkPipelineShaderStageCreateInfo> stages;
  float alphaRefVal = d.alphaRef;
  VkSpecializationMapEntry alphaMap{0, 0, 4};
  VkSpecializationInfo alphaSpec{};
  for (int s : {0, 3, 4, 2, 1}) {
    if (!p->st[s].valid) continue;
    VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    static const VkShaderStageFlagBits bits[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    si.stage = bits[s]; si.module = p->st[s].mod; si.pName = "main";
    if (s == 1) {
      StageProg& fp = p->st[1];
      const uint32_t vkey = (uint32_t)(alpha ? af : 0) | ((uint32_t)rt.nColor << 4) | (dual ? 0x100u : 0u);
      auto it = fp.fragMods.find(vkey);
      if (it == fp.fragMods.end()) {
        std::vector<uint32_t> cur = fp.spv, tmp;
        bool alphaOk = false;
        if (alpha) {
          if (PatchAlphaTest(cur, af, tmp)) { cur = tmp; alphaOk = true; }
          else LogError("alpha test (func %d): fragment shader has no float vec4 output at location 0, test ignored", af);
        }
        VkShaderModule m = fp.mod;
        if (PatchFragmentOutputs(cur, rt.nColor, dual, tmp)) cur = tmp, m = VK_NULL_HANDLE;
        else if (alphaOk) m = VK_NULL_HANDLE;
        if (!m) {
          VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
          mi.codeSize = cur.size() * 4; mi.pCode = cur.data();
          if (vkCreateShaderModule(h->device, &mi, nullptr, &m) != VK_SUCCESS) { m = fp.mod; alphaOk = false; LogError("fragment variant: vkCreateShaderModule failed"); }
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
  for (int i = 0; i < d.nstream; i++) {
    binds.push_back({(uint32_t)i, d.stream[i].stride, d.stream[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    have[i] = true;
    if (d.stream[i].divisor > 1) Log("vertex stream %d divisor %u: only 0/1 supported, using per-instance", i, d.stream[i].divisor);
  }
  for (int i = 0; i < d.nattr; i++) {
    uint32_t f = VaFmt(d.attr[i]);
    if (!f) continue;
    if (i < 32 && !((p->st[0].inputMask >> i) & 1)) continue;   // the vertex shader does not read this location
    const FormatInfo* fi = LookupFormat((int)f);
    if (!fi) { LogError("vertex attribute %d: format 0x%x not mapped", i, f); continue; }
    uint32_t st = VaStream(d.attr[i]);
    if (!have[st]) { binds.push_back({st, 0, VK_VERTEX_INPUT_RATE_VERTEX}); have[st] = true; }
    attrs.push_back({(uint32_t)i, st, fi->vk, VaOffset(d.attr[i])});
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = (uint32_t)binds.size(); vi.pVertexBindingDescriptions = binds.data();
  vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size(); vi.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topo;
  VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  ts.patchControlPoints = (uint32_t)std::clamp(d.patchSize, 1, 32);
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = d.poly.mode == POLY_LINE ? VK_POLYGON_MODE_LINE : d.poly.mode == POLY_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
  rs.cullMode = (VkCullModeFlags)d.poly.cull;   // NVNface NONE/FRONT/BACK/BOTH == the Vulkan bit values 0/1/2/3
  // NVN CCW/CW map 1:1 to Vulkan: with window origin upper-left both APIs define winding in y-down window space.
  rs.frontFace = d.poly.front == FRONT_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  const int offBit = d.poly.mode == POLY_POINT ? 1 : d.poly.mode == POLY_LINE ? 2 : 4;
  rs.depthBiasEnable = (d.poly.offsetEn & offBit) ? VK_TRUE : VK_FALSE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  int samples = rt.samples > 1 ? rt.samples : (d.ms.samples > 1 ? d.ms.samples : 1);
  ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
  ms.alphaToCoverageEnable = d.ms.a2c ? VK_TRUE : VK_FALSE;
  VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dss.depthTestEnable = DsGet(d.ds, DS_DT, 1); dss.depthWriteEnable = DsGet(d.ds, DS_DW, 1);
  dss.depthCompareOp = Cmp(DsGet(d.ds, DS_DF, 4));
  dss.stencilTestEnable = DsGet(d.ds, DS_ST, 1);
  dss.front = {SOp(DsGet(d.ds, DS_FAIL_F, 4)), SOp(DsGet(d.ds, DS_ZPASS_F, 4)), SOp(DsGet(d.ds, DS_ZFAIL_F, 4)), Cmp(DsGet(d.ds, DS_SF_F, 4)), 0, 0, 0};
  dss.back = {SOp(DsGet(d.ds, DS_FAIL_B, 4)), SOp(DsGet(d.ds, DS_ZPASS_B, 4)), SOp(DsGet(d.ds, DS_ZFAIL_B, 4)), Cmp(DsGet(d.ds, DS_SF_B, 4)), 0, 0, 0};
  std::vector<VkPipelineColorBlendAttachmentState> cba(rt.nColor);
  for (int i = 0; i < rt.nColor; i++) {
    const BlendSt& b = d.blend[i];
    cba[i].blendEnable = (d.color.blendEn >> i) & 1;
    const bool src1Ok = dual && i == 0;
    auto bf = [&](int f) {   // SRC1 factors only where the second source exists; elsewhere they read as the matching SRC factor
      if (!src1Ok && f >= BF_SRC1_COLOR && f <= BF_ONE_MINUS_SRC1_ALPHA) f = f - BF_SRC1_COLOR + BF_SRC_COLOR;
      return BlendFactor(f);
    };
    cba[i].srcColorBlendFactor = bf(b.srcRgb); cba[i].dstColorBlendFactor = bf(b.dstRgb);
    cba[i].srcAlphaBlendFactor = bf(b.srcA); cba[i].dstAlphaBlendFactor = bf(b.dstA);
    cba[i].colorBlendOp = BlendOp(b.eqRgb); cba[i].alphaBlendOp = BlendOp(b.eqA);
    cba[i].colorWriteMask = (d.chmask.mask >> (4 * i)) & 0xF;   // r,g,b,a bits 0..3 == VK_COLOR_COMPONENT_R..A_BIT
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = (uint32_t)cba.size(); cb.pAttachments = cba.data();
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof *dyn); ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = (uint32_t)rt.nColor; ri.pColorAttachmentFormats = rt.color;
  ri.depthAttachmentFormat = rt.depth; ri.stencilAttachmentFormat = rt.stencil;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri;
  gi.stageCount = (uint32_t)stages.size(); gi.pStages = stages.data();
  gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
  gi.pTessellationState = topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST ? &ts : nullptr;
  gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &dss;
  gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = p->layout;
  VkPipeline pipe = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(h->device, h->pipelineCache, 1, &gi, nullptr, &pipe) != VK_SUCCESS) {
    LogError("vkCreateGraphicsPipelines failed");
    return VK_NULL_HANDLE;
  }
  std::lock_guard<std::mutex> lk(h->pipeMutex);
  h->pipelines[key] = pipe;
  return pipe;
}

// ---- descriptors ----------------------------------------------------------------------------------------------------------
VkDescriptorSet AllocSet(ReplayCtx& cx, VkDescriptorSetLayout layout) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (cx.pools->empty() || attempt == 1) cx.pools->push_back(AcquireDescriptorPool(cx.q));
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

ResolvedTex ResolveHandle(ReplayCtx& cx, DrawState& d, uint64_t handle, bool image, int shaderType) {
  ResolvedTex r;
  if (!d.tp) return r;
  const int texId = (int)(handle & 0xFFFFF), sampId = (int)(handle >> 20);
  auto it = d.tp->slots.find(texId);
  if (it == d.tp->slots.end() || !it->second.tex) return r;
  r.t = it->second.tex;
  if (!r.t->EnsureImage()) return r;
  const TexView* tv = it->second.hasView ? &it->second.view : nullptr;
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
    auto s = d.sp->slots.find(sampId);
    if (s != d.sp->slots.end()) r.samp = s->second->sampler;
  }
  return r;
}
int TexBindIndex(const MTex& t) { return t.handleIndex >= 8 ? (t.handleIndex - 8) / 2 : t.handleIndex; }

VkImageLayout SampledLayout(const TextureImpl* t) {
  return (t->fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}

void BeginRendering(ReplayCtx& cx, const RtInfo& rt) {
  VkRenderingAttachmentInfo col[8] = {};
  VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, stc{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  uint32_t w = 0, hgt = 0, layerCount = 1;
  for (int i = 0; i < rt.nColor; i++) {
    TextureImpl* t = cx.rt[i];
    col[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (!t) continue;
    t->uploaded = true;
    SubRange rr;
    col[i].imageView = t->RenderView(cx.rtHasView[i] ? &cx.rtView[i] : nullptr, &rr);   // the mip level / layer the game selected
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
  vkCmdBeginRendering(cx.cb, &ri);
  cx.rendering = true;
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
  if (!pool->buffer && !EnsureBacking(pool)) return b;
  uint64_t n = std::min<uint64_t>(size ? size : pool->size - off, pool->size - off);
  SyncPoolRange(pool, off, n);
  b.buf = pool->buffer; b.off = off; b.range = cap ? std::min(n, cap) : n; b.pool = pool;
  return b;
}

struct TexWrite { int set, binding, idx; ResolvedTex r; bool image; int stype; };
// Collects the sampled textures / storage images a program reads, uploads static data and fixes layouts. Must run
// before a rendering scope is open (graphics) or at any time (compute).
bool PrepareTextures(ReplayCtx& cx, DrawState& d, ProgramImpl* p, std::vector<TexWrite>& texWrites, bool graphics) {
  Host* h = cx.h; (void)h;
  // sampled textures / storage images (layouts must be right before a rendering scope is open)
  struct Want { TextureImpl* t; VkImageLayout layout; SubRange range; };
  std::vector<Want> wantLayout;
  for (int s = 0; s < 6; s++) {
    if (!p->st[s].valid) continue;
    for (auto& t : p->st[s].tex) {
      if (t.separate) continue;
      for (int k = 0; k < t.array; k++) {
        int bi = TexBindIndex(t) + k;
        ResolvedTex r;
        if (bi >= 0 && bi < 32 && d.texSet[s][bi]) r = ResolveHandle(cx, d, d.tex[s][bi], false, t.type);
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

bool WriteDescriptors(ReplayCtx& cx, DrawState& d, ProgramImpl* p, const std::vector<TexWrite>& texWrites, VkPipelineBindPoint bind) {
  Host* h = cx.h;
  // descriptors
  Dummies* dm = h->dummies;
  std::deque<VkDescriptorBufferInfo> bufInfos;
  std::deque<VkDescriptorImageInfo> imgInfos;
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorSet sets[4] = {};
  for (int set = 0; set < 4; set++) if (p->setUsed[set]) sets[set] = AllocSet(cx, p->setLayout[set]);
  auto wBuf = [&](int set, int binding, VkDescriptorType t, const BufBind& b) {
    bufInfos.push_back({b.buf, b.off, b.range});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[set]; w.dstBinding = (uint32_t)binding; w.descriptorCount = 1; w.descriptorType = t; w.pBufferInfo = &bufInfos.back();
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
      if (sb.slot >= 0 && sb.slot < 16 && d.ssbo[s][sb.slot].buf) b = d.ssbo[s][sb.slot];   // ordinal == NVN index (unverified)
      if (sb.write && b.pool && DebugDrawLogActive()) DebugDrawLog("  SSBO write stage %d slot %d pool %p off 0x%llx size 0x%llx poolsize 0x%llx\n", s, sb.slot, (void*)b.pool, (unsigned long long)b.off, (unsigned long long)b.range, (unsigned long long)b.pool->size);
      if (sb.write && b.pool) MarkGpuWritten(b.pool, b.off, b.range);   // later CPU->GPU syncs must not clobber what the shader writes
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
    w.pImageInfo = &imgInfos[first];
    writes.push_back(w);
    i = j;
  }
  for (auto& w : writes) if (!w.dstSet) return false;
  if (!writes.empty()) vkUpdateDescriptorSets(h->device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
  for (int set = 0; set < 4; set++)
    if (sets[set]) vkCmdBindDescriptorSets(cx.cb, bind, p->layout, (uint32_t)set, 1, &sets[set], 0, nullptr);

  return true;
}

void DoDraw(ReplayCtx& cx, const Cmd& c) {
  Host* h = cx.h;
  DrawState& d = *cx.ds;
  ProgramImpl* p = d.prog;
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
    rt.color[i] = t ? t->fi->vk : VK_FORMAT_UNDEFINED;
    if (t) { rt.nColor = i + 1; rt.samples = std::max(rt.samples, t->b.samples); }
  }
  if (TextureImpl* t = cx.rt[8]) {
    if (!t->EnsureImage()) return;
    if (t->fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) rt.depth = t->fi->vk;
    if (t->fi->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) rt.stencil = t->fi->vk;
  }
  if (rt.nColor == 0 && !cx.rt[8]) { LogError("draw with no render targets"); return; }

  std::vector<TexWrite> texWrites;
  if (!PrepareTextures(cx, d, p, texWrites, true)) return;
  if (DebugDrawLogActive()) {
    DebugDrawLog("draw prog=%d prim=%d n=%d rt0=%p(%dx%d f%x) vp=%d:", (int)p->id, c.i[0], c.i[2], (void*)cx.rt[0], cx.rt[0] ? cx.rt[0]->b.width : 0, cx.rt[0] ? cx.rt[0]->b.height : 0, cx.rt[0] ? cx.rt[0]->b.format : 0, cx.vpSet);
    for (auto& w : texWrites) if (w.r.t) DebugDrawLog(" [b%d t=%p f%x %dx%d fl%x pool=%d]", w.binding, (void*)w.r.t, w.r.t->b.format, w.r.t->b.width, w.r.t->b.height, w.r.t->b.flags, w.r.t->b.pool != nullptr);
    DebugDrawLog("\n");
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
  if (!cx.rendering) BeginRendering(cx, rt);

  VkPipeline pipe = GetPipeline(cx, d, topo, rt);
  if (!pipe) return;
  vkCmdBindPipeline(cx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

  // dynamic state that lives outside the pipeline
  vkCmdSetStencilCompareMask(cx.cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)d.stCompare[0]);
  vkCmdSetStencilCompareMask(cx.cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)d.stCompare[1]);
  vkCmdSetStencilWriteMask(cx.cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)d.stWrite[0]);
  vkCmdSetStencilWriteMask(cx.cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)d.stWrite[1]);
  vkCmdSetStencilReference(cx.cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)d.stRef[0]);
  vkCmdSetStencilReference(cx.cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)d.stRef[1]);
  vkCmdSetDepthBias(cx.cb, d.biasUnits, h->features.depthBiasClamp ? d.biasClamp : 0.f, d.biasFactor);
  const float bc[4] = {0, 0, 0, 0};
  vkCmdSetBlendConstants(cx.cb, bc);

  if (!WriteDescriptors(cx, d, p, texWrites, VK_PIPELINE_BIND_POINT_GRAPHICS)) return;

  Dummies* dm = h->dummies;
  // vertex buffers (one binding per referenced stream; an unbound stream reads zeros)
  for (int i = 0; i < std::max(d.nstream, 0); i++) {
    VkBuffer b = d.vb[i].buf ? d.vb[i].buf : dm->zero;
    VkDeviceSize o = d.vb[i].buf ? d.vb[i].off : 0;
    vkCmdBindVertexBuffers(cx.cb, (uint32_t)i, 1, &b, &o);
  }
  if (rewrite) {
    VkBuffer ib = VK_NULL_HANDLE;
    uint8_t* m = NewScratch(cx, rw.size() * 4, &ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!m) { LogError("index rewrite: scratch allocation failed"); return; }
    memcpy(m, rw.data(), rw.size() * 4);
    vkCmdBindIndexBuffer(cx.cb, ib, 0, VK_INDEX_TYPE_UINT32);
    if (indexed) vkCmdDrawIndexed(cx.cb, (uint32_t)rw.size(), (uint32_t)std::max(c.i[5], 1), 0, c.i[3], (uint32_t)c.i[4]);
    else vkCmdDrawIndexed(cx.cb, (uint32_t)rw.size(), (uint32_t)std::max(c.i[4], 1), 0, 0, (uint32_t)c.i[3]);
  } else if (indexed) {
    PoolImpl* pool = nullptr; uint64_t off = 0;
    if (!h->ResolveAddress(c.a[0], &pool, &off)) { LogError("index buffer address 0x%llx is not in any memory pool", (unsigned long long)c.a[0]); return; }
    const uint64_t isz = c.i[1] == IDX_U32 ? 4 : 2;
    if (!pool->buffer && !EnsureBacking(pool)) return;
    SyncPoolRange(pool, off, isz * (uint64_t)std::max(c.i[2], 0));
    vkCmdBindIndexBuffer(cx.cb, pool->buffer, off, c.i[1] == IDX_U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cx.cb, (uint32_t)c.i[2], (uint32_t)std::max(c.i[5], 1), 0, c.i[3], (uint32_t)c.i[4]);
  } else {
    vkCmdDraw(cx.cb, (uint32_t)c.i[2], (uint32_t)std::max(c.i[4], 1), (uint32_t)c.i[1], (uint32_t)c.i[3]);
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

void DoDispatch(ReplayCtx& cx, const Cmd& c) {
  Host* h = cx.h;
  DrawState& d = *cx.ds;
  ProgramImpl* p = d.prog;
  if (!p || !p->layout || !p->st[5].valid) { LogError("dispatch without a usable compute program"); return; }
  GetDummies(h);
  EndRendering(cx);
  std::vector<TexWrite> tw;
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
    if (vkCreateComputePipelines(h->device, h->pipelineCache, 1, &ci, nullptr, &pipe) != VK_SUCCESS) { LogError("vkCreateComputePipelines failed"); return; }
    std::lock_guard<std::mutex> lk(h->pipeMutex);
    h->pipelines[key] = pipe;
  }
  vkCmdBindPipeline(cx.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
  if (!WriteDescriptors(cx, d, p, tw, VK_PIPELINE_BIND_POINT_COMPUTE)) return;
  vkCmdDispatch(cx.cb, (uint32_t)std::max(c.i[0], 1), (uint32_t)std::max(c.i[1], 1), (uint32_t)std::max(c.i[2], 1));
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
  if (!pool->buffer && !EnsureBacking(pool)) return;
  if (pool->storage && getenv("NVN_TEX_DUMP")) {
    const uint64_t n = (uint64_t)r.w * std::max(r.h, 1) * std::max(r.d, 1) * t->fi->bytes / std::max<uint32_t>(t->fi->bw * t->fi->bh, 1);
    DebugDumpTexture(t, "copy", static_cast<const uint8_t*>(pool->storage) + off, std::min<uint64_t>(n, pool->size - off), (uint32_t)r.w, (uint32_t)r.h);
  }
  if (DebugDrawLogActive()) DebugDrawLog("CopyBufferToTexture t=%p fmt=%x %dx%d lvl=%u region(%d,%d,%d %dx%dx%d) flags=%d\n", (void*)t, t->b.format, t->b.width, t->b.height, c.i[0], r.x, r.y, r.z, r.w, r.h, r.d, c.i[1]);
  EndRendering(cx);
  t->uploaded = true;
  Transition(cx.cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const bool layered = t->b.target == TT_2D_ARRAY || t->b.target == TT_1D_ARRAY;
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
  SyncPoolRange(pool, off, (uint64_t)bc.imageExtent.width * bc.imageExtent.height * std::max<uint64_t>(bc.imageExtent.depth, bc.imageSubresource.layerCount) * t->fi->bytes / (t->fi->bw * t->fi->bh) + 16);
  vkCmdCopyBufferToImage(cx.cb, pool->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
}

}  // namespace

// ---- exported to nvn_vk.cpp -------------------------------------------------------------------------------------------------
void EndRendering(ReplayCtx& c) {
  if (!c.rendering) return;
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
  vkCreateDescriptorPool(q->host->device, &ci, nullptr, &p);
  return p;
}

// ---- pipeline cache persistence + shutdown ----------------------------------------------------------------------------------
void LoadPipelineCache(Host* h) {
  std::ifstream f(h->cacheDir + "\\pipeline_cache.bin", std::ios::binary);
  if (!f) return;
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.empty()) return;
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = data.size(); ci.pInitialData = data.data();
  VkPipelineCache pc = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(h->device, &ci, nullptr, &pc) == VK_SUCCESS) {
    if (h->pipelineCache) vkDestroyPipelineCache(h->device, h->pipelineCache, nullptr);
    h->pipelineCache = pc;
    Log("pipeline cache loaded (%zu bytes)", data.size());
  }
}

void DestroyHostExtras(Host* h) {
  DestroyQueryPools(h);
  if (h->pipelineCache) {
    size_t n = 0;
    if (vkGetPipelineCacheData(h->device, h->pipelineCache, &n, nullptr) == VK_SUCCESS && n) {
      std::vector<char> data(n);
      if (vkGetPipelineCacheData(h->device, h->pipelineCache, &n, data.data()) == VK_SUCCESS) {
        std::ofstream f(h->cacheDir + "\\pipeline_cache.bin", std::ios::binary | std::ios::trunc);
        f.write(data.data(), (std::streamsize)n);
        Log("pipeline cache saved (%zu bytes)", n);
      }
    }
  }
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
