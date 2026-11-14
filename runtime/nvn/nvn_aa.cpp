// SPDX-License-Identifier: MIT
// SMAA 1x and contrast adaptive sharpening, run on the game's tonemap output (project code, ours).
//
// The game's last scene pass (`Final`, shaders TonemappingPS / TonemappingFXAAPS / TonemappingLut* / FXAAOnlyPS) writes the tonemapped, FXAA'd picture
// into the window texture, and the `UI` pass draws text and HUD over it afterwards. The pass objects pick the FXAA variant themselves, so instead of
// changing what the game selects:
//   * SNAP_AA=off|smaa: the four FXAA shaders (and FXAAOnlyPS) are patched in SPIR-V (AaPatchFragment): their first early-exit test, "luma range >= threshold",
//     is made always false, which is the shader's own no-edge path (the plain tonemapped center pixel).
//   * SNAP_AA=smaa: right after any of those programs draws (AaAfterDraw, called from DoDraw), the render target is copied to a scratch image and SMAA 1x
//     (edge detection, blend weights, neighborhood blend; reference SMAA.hlsl compiled with dxc, area/search textures from the reference scripts) writes
//     the result back in place. UI draws later, so text is untouched.
//   * SNAP_SHARPEN=s: CAS (FidelityFX algorithm) after the AA step, also before the UI.
// Everything runs in the replaying command buffer; no other guest-visible state changes. Photos and the Evaluation pass read their own targets.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

#include "nvn_aa.h"
#include "nvn_aa_spv.h"
#include "nvn_internal.h"
#include "hle/visual_defaults.h"   // the one table of visual defaults

namespace nvn {
int g_aaMode = AA_FXAA;
float g_aaSharpen = 0.0f;
// Window client height in pixels: SNAP_WINDOW_H, else window.json "height", else 720 (host/window_opts.cpp's default windowed size).
// Shared with the upscaler (upscale.cpp used its own copy that fell back to 1080).
int HostWindowHeight() {
  if (const char* e = getenv("SNAP_WINDOW_H")) if (atoi(e) > 0) return atoi(e);
  // Borderless or maximized: the swapchain is monitor-sized, not the saved windowed height (window_opts.cpp does not save `height` while borderless).
  bool big = false;
  if (const char* m = getenv("SNAP_WINDOW_MODE")) big = !strcmp(m, "borderless") || !strcmp(m, "1");
  std::string dir;
  if (const char* c = getenv("SNAP_CONFIG_DIR")) dir = c; else if (const char* a = getenv("APPDATA")) dir = std::string(a) + "\\SnapRecomp";
  int saved = 0;
  if (!dir.empty()) {
    if (FILE* f = fopen((dir + "\\window.json").c_str(), "rb")) {
      char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
      if (const char* p = strstr(buf, "\"height\"")) { p = strchr(p, ':'); if (p && atoi(p + 1) > 0) saved = atoi(p + 1); }
      if (!getenv("SNAP_WINDOW_MODE")) {
        if (const char* p = strstr(buf, "\"mode\"")) { p = strchr(p, ':'); if (p) { while (*++p == ' ') {} big = !strncmp(p, "\"borderless\"", 12); } }
        if (const char* p = strstr(buf, "\"maximized\"")) { p = strchr(p, ':'); if (p) { while (*++p == ' ') {} big = big || !strncmp(p, "true", 4); } }
      }
    }
  }
#ifdef _WIN32
  if (big) { int mh = GetSystemMetrics(SM_CYSCREEN); if (mh > 0) return mh; }   // primary monitor; the window may sit on another one
#endif
  return saved > 0 ? saved : 720;
}
namespace {
bool g_parsed;
float g_ssaaScale = 0.f;   // the scale AaParseEnv handed to g_scale; reset to 1 if the AA step fails (AaFail)
bool g_log;
bool g_split;   // SNAP_AA_SPLIT=1: the left half of the frame keeps the game's own picture (same-frame A/B shots)

// first 12 hex digits of the translation cache keys (computed from the game's nushdb data, see AGENTS.md "Anti-aliasing")
const char* kFxaaKeys[] = {"3df861d79c36" /*TonemappingFXAAPS*/, "874c3a9bc04b" /*TonemappingLutFXAAPS*/, "49a9f4c5b6d7" /*TonemappingLutFXAADistortionPS*/,
                           "e85954edec1f" /*FXAAOnlyPS*/};
const char* kPlainKeys[] = {"981bcb0e4161" /*TonemappingPS*/, "5729ba114d4b" /*TonemappingLutPS*/, "067ec8d5c8f9" /*TonemappingLutDistortionPS*/};
bool In(const char* const* l, size_t n, const char* k) { for (size_t i = 0; i < n; i++) if (!strncmp(l[i], k, 12)) return true; return false; }
bool IsFxaa(const char* k) { return In(kFxaaKeys, 4, k); }
bool IsTonemap(const char* k) { return IsFxaa(k) || In(kPlainKeys, 3, k); }

// ---- Vulkan objects ------------------------------------------------------------------------------------------------------
struct Pipes { VkPipeline edge = VK_NULL_HANDLE, weights = VK_NULL_HANDLE, blend = VK_NULL_HANDLE, cas = VK_NULL_HANDLE; VkFormat fmt = VK_FORMAT_UNDEFINED; };
struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
struct Sized {
  uint32_t w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED;
  Img c, e, wt, b;                                   // scene copy, edges, weights, blended (CAS input)
  VkDescriptorSet setEdge = VK_NULL_HANDLE, setW = VK_NULL_HANDLE, setBlend = VK_NULL_HANDLE, setCas = VK_NULL_HANDLE, setCasC = VK_NULL_HANDLE;
};
struct G {
  Host* h = nullptr; bool failed = false, ready = false, texUploaded = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
  VkShaderModule mod[8] = {};
  VkSampler lin = VK_NULL_HANDLE, pt = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  Img area, search; VkBuffer stage = VK_NULL_HANDLE; VkDeviceMemory stageMem = VK_NULL_HANDLE;
  Pipes pipes[4]; int npipes = 0;
  Sized sz;
  std::map<std::tuple<VkImage, uint32_t, uint32_t>, VkImageView> unormViews;   // sRGB targets: UNORM view of (image, level, layer); guarded by unormMu
  std::mutex unormMu;   // AaAfterDraw (replay thread) vs AaForgetTexture (graveyard reaper)
} g;

// SNAP_AA_DUMP=<dir> SNAP_AA_DUMP_AT=<p1,p2,...> (test aid): at the first tonemap draw at or after each present count, write the scene before the
// AA step (<dir>/aa_p<N>_before.bmp) and after it (aa_p<N>_after.bmp) from the SAME frame (no UI), so before/after pairs share one pose.
struct Dump {
  bool init = false, on = false; std::string dir; std::vector<int> at; size_t idx = 0;
  VkBuffer buf[3] = {}; VkDeviceMemory mem[3] = {}; void* map[3] = {}; uint64_t bytes = 0;
  int pendingAt = -1, pendingFrame = 0; uint32_t w = 0, h = 0; bool hasSmaa = false;
} dmp;

void WriteBmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
  uint8_t hdr[54] = {'B', 'M'};
  const uint32_t sz = 54 + w * h * 4, off = 54, dib = 40; const int32_t nh = -(int32_t)h; const uint16_t pl = 1, bpp = 32;
  memcpy(hdr + 2, &sz, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &nh, 4);
  memcpy(hdr + 26, &pl, 2); memcpy(hdr + 28, &bpp, 2);
  fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> row(w * 4);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* s = rgba + (uint64_t)y * w * 4;
    for (uint32_t x = 0; x < w; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = 255; }
    fwrite(row.data(), 1, row.size(), f);
  }
  fclose(f);
}

VkFormat Unorm(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return f;
    default: return VK_FORMAT_UNDEFINED;
  }
}

void ImgBar(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt, VkImageUsageFlags usage) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}
void FreeImg(Host* h, Img& o) {
  if (o.view) vkDestroyImageView(h->device, o.view, nullptr);
  if (o.img) vkDestroyImage(h->device, o.img, nullptr);
  if (o.mem) vkFreeMemory(h->device, o.mem, nullptr);
  o = Img{};
}

bool Setup(Host* h) {
  g.h = h; VkDevice d = h->device;
  static const uint32_t* const code[8] = {k_smaa_vs_edge, k_smaa_ps_edge, k_smaa_vs_weights, k_smaa_ps_weights, k_smaa_vs_blend, k_smaa_ps_blend, k_cas_vs, k_cas_ps};
  static const size_t size[8] = {sizeof k_smaa_vs_edge, sizeof k_smaa_ps_edge, sizeof k_smaa_vs_weights, sizeof k_smaa_ps_weights, sizeof k_smaa_vs_blend, sizeof k_smaa_ps_blend, sizeof k_cas_vs, sizeof k_cas_ps};
  for (int i = 0; i < 8; i++) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = size[i]; mi.pCode = code[i];
    if (vkCreateShaderModule(d, &mi, nullptr, &g.mod[i]) != VK_SUCCESS) return false;
  }
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  if (vkCreateSampler(d, &si, nullptr, &g.lin) != VK_SUCCESS) return false;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  if (vkCreateSampler(d, &si, nullptr, &g.pt) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding bn[6];
  for (int i = 0; i < 6; i++) bn[i] = {(uint32_t)i, i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 6; dl.pBindings = bn;
  if (vkCreateDescriptorSetLayout(d, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
  if (vkCreatePipelineLayout(d, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * 16}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * 16}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 16; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  if (vkCreateDescriptorPool(d, &dp, nullptr, &g.pool) != VK_SUCCESS) return false;
  if (!MakeImg(h, g.area, 160, 560, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
  if (!MakeImg(h, g.search, 64, 16, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
  void* mp = nullptr;
  const uint64_t asz = sizeof k_smaa_area, ssz = sizeof k_smaa_search;
  if (!CreateHostBuffer(h, asz + ssz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g.stage, &g.stageMem, &mp)) return false;
  memcpy(mp, k_smaa_area, asz); memcpy((uint8_t*)mp + asz, k_smaa_search, ssz);
  return true;
}

VkPipeline MakePipe(VkShaderModule vs, VkShaderModule fs, VkFormat fmt) {
  VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
  VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
  gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = g.pl;
  VkPipeline p = VK_NULL_HANDLE;
  const VkResult r = vkCreateGraphicsPipelines(g.h->device, VK_NULL_HANDLE, 1, &gi, nullptr, &p);
  if (r != VK_SUCCESS) { fprintf(stderr, "aa: vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
  return p;
}

Pipes* GetPipes(VkFormat fmt) {
  for (int i = 0; i < g.npipes; i++) if (g.pipes[i].fmt == fmt) return &g.pipes[i];
  if (g.npipes >= 4) return nullptr;
  Pipes& p = g.pipes[g.npipes];
  p.fmt = fmt;
  p.edge = MakePipe(g.mod[0], g.mod[1], VK_FORMAT_R8G8_UNORM);
  p.weights = MakePipe(g.mod[2], g.mod[3], VK_FORMAT_R8G8B8A8_UNORM);
  p.blend = MakePipe(g.mod[4], g.mod[5], fmt);
  p.cas = MakePipe(g.mod[6], g.mod[7], fmt);
  if (!p.edge || !p.weights || !p.blend || !p.cas) return nullptr;
  g.npipes++;
  return &p;
}

VkDescriptorSet MakeSet(VkImageView v0, VkImageView v1, VkImageView v2, VkImageView v3) {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = g.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g.dsl;
  VkDescriptorSet s = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(g.h->device, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorImageInfo ii[6] = {{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {VK_NULL_HANDLE, v2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, v3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {g.lin, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}, {g.pt, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
  VkWriteDescriptorSet w[6];
  for (int i = 0; i < 6; i++) {
    w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[i].dstSet = s; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
    w[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER; w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(g.h->device, 6, w, 0, nullptr);
  return s;
}

bool Resize(uint32_t w, uint32_t hh, VkFormat fmt) {
  Host* h = g.h;
  if (g.sz.w == w && g.sz.h == hh && g.sz.fmt == fmt && g.sz.c.img) return true;
  if (g.sz.c.img) {
    // [nvnfix] review_nvn.md 8: commands recorded earlier in this submit (not on the GPU yet, so vkDeviceWaitIdle never covered them) may
    // still use the old images and sets. They get a fresh descriptor pool, and the old images and pool go to the graveyard (nvn_life.cpp).
    LifeCount(LS_AA_RESIZE);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * 16}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * 16}};   // as in Init
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 16; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
    VkDescriptorPool fresh = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(h->device, &dp, nullptr, &fresh) != VK_SUCCESS) return false;
    Sized old = g.sz; const VkDescriptorPool oldPool = g.pool;
    g.pool = fresh;
    RetireFn([h, old, oldPool]() mutable {
      FreeImg(h, old.c); FreeImg(h, old.e); FreeImg(h, old.wt); FreeImg(h, old.b);
      vkDestroyDescriptorPool(h->device, oldPool, nullptr);
    });
  }
  g.sz = Sized{};
  g.sz.w = w; g.sz.h = hh; g.sz.fmt = fmt;
  const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!MakeImg(h, g.sz.c, w, hh, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !MakeImg(h, g.sz.e, w, hh, VK_FORMAT_R8G8_UNORM, rt) ||
      !MakeImg(h, g.sz.wt, w, hh, VK_FORMAT_R8G8B8A8_UNORM, rt) || !MakeImg(h, g.sz.b, w, hh, fmt, rt | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return false;
  g.sz.setEdge = MakeSet(g.sz.c.view, g.sz.c.view, g.area.view, g.search.view);
  g.sz.setW = MakeSet(g.sz.c.view, g.sz.e.view, g.area.view, g.search.view);
  g.sz.setBlend = MakeSet(g.sz.c.view, g.sz.wt.view, g.area.view, g.search.view);
  g.sz.setCas = MakeSet(g.sz.b.view, g.sz.b.view, g.area.view, g.search.view);
  g.sz.setCasC = MakeSet(g.sz.c.view, g.sz.c.view, g.area.view, g.search.view);
  return g.sz.setEdge && g.sz.setW && g.sz.setBlend && g.sz.setCas && g.sz.setCasC;
}

void FullPass(ReplayCtx& cx, VkImageView target, uint32_t w, uint32_t hh, VkAttachmentLoadOp load, VkPipeline pipe, VkDescriptorSet set, const float pc[4], uint32_t x0 = 0) {
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = target; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = load; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, hh}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cx.cb, &ri);
  VkViewport vp{0, 0, (float)w, (float)hh, 0, 1}; VkRect2D sc{{(int32_t)x0, 0}, {w - x0, hh}};
  vkCmdSetViewport(cx.cb, 0, 1, &vp); vkCmdSetScissor(cx.cb, 0, 1, &sc);
  vkCmdBindPipeline(cx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cx.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cx.cb, g.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
  vkCmdDraw(cx.cb, 3, 1, 0, 0);
  vkCmdEndRendering(cx.cb);
}
}  // namespace

// ssaa2 asks for 2x the window height, then is capped: 2x the window per side is the formula's own ceiling, and on top of that
//   * scale <= kSsaaMaxScale (3.0 = 5760x3240): a 2160p window used to render at 7680x4320 (4.0), 4x the pixels of 4K for no visible gain
//   * no render side over 8192 px (the largest 2D image size every desktop GPU guarantees) = scale <= 8192/1920
// Raise kSsaaMaxScale (or set SNAP_RENDER_SCALE explicitly) to opt into more.
constexpr float kSsaaMaxScale = 3.0f;
static float SsaaScale(int windowH) {
  const float want = 2.0f * (float)windowH / 1080.0f;
  const float cap = std::min(kSsaaMaxScale, 8192.0f / 1920.0f);
  const float v = std::clamp(want, 1.0f, cap);
  if (v < want) fprintf(stderr, "aa: ssaa2 scale capped %.3f -> %.3f (window height %d, max %.2f = %.0fx%.0f)\n", want, v, windowH, cap, 1920 * v, 1080 * v);
  return v;
}
// Called when the AA step gives up (setup, pipeline or scratch-image failure): drop the supersampling scale it raised, which has no benefit
// without the AA step. Only new textures see it (existing targets keep their size); an explicit SNAP_RENDER_SCALE is left alone.
static void AaFail() {
  if (g_ssaaScale > 0.f && g_scale.scale == g_ssaaScale) { fprintf(stderr, "aa: render scale %.3f reset to 1 (AA disabled)\n", g_ssaaScale); g_scale.scale = 1.0f; g_ssaaScale = 0.f; }
}

float AaParseEnv() {
  if (g_parsed) return 0.f;
  g_parsed = true;
  g_split = getenv("SNAP_AA_SPLIT") && getenv("SNAP_AA_SPLIT")[0] != '0';
  g_log = getenv("SNAP_AA_LOG") && getenv("SNAP_AA_LOG")[0] != '0';
  float scale = 0.f;
  {
    // Default (SNAP_AA unset) comes from hle/visual_defaults.h: the game's own FXAA ("original") or ssaa2+smaa+CAS ("enhanced", SNAP_VISUALS).
    // SNAP_AA=off is the original FXAA path. An explicit SNAP_RENDER_SCALE wins over the enhanced default's ssaa2.
    const char* e = getenv("SNAP_AA"); if (e && !*e) e = nullptr;
    std::string s = e ? e : snapdef::get("SNAP_AA");
    if (!e && getenv("SNAP_RENDER_SCALE") && s.find("ssaa2") != std::string::npos) s = "smaa";
    g_aaSharpen = std::clamp((float)atof(snapdef::get("SNAP_SHARPEN")), 0.0f, 1.0f);
    for (auto& ch : s) { ch = (char)tolower((unsigned char)ch); if (ch == '+') ch = ','; }
    size_t pos = 0;
    while (pos <= s.size()) {
      size_t c = s.find(',', pos); if (c == std::string::npos) c = s.size();
      const std::string t = s.substr(pos, c - pos); pos = c + 1;
      if (t == "smaa") g_aaMode = AA_SMAA;
      else if (t == "none") g_aaMode = AA_OFF;   // the game's FXAA removed, nothing in its place
      else if (t == "off" || t == "original" || t == "fxaa" || t.empty()) g_aaMode = AA_FXAA;   // the game's own FXAA, untouched
      else if (t == "ssaa2") scale = SsaaScale(HostWindowHeight());
      else fprintf(stderr, "aa: unknown SNAP_AA token '%s' (fxaa, off, smaa, ssaa2)\n", t.c_str());
    }
  }
  fprintf(stderr, "aa: mode %s, sharpen %.2f, ssaa render scale %.3f\n", g_aaMode == AA_SMAA ? "smaa" : g_aaMode == AA_OFF ? "off" : "fxaa", g_aaSharpen, scale);
  g_ssaaScale = scale;
  return scale;
}

void AaPatchFragment(std::vector<uint32_t>& w, const char* key) {
  if (!g_parsed) AaParseEnv();
  if (g_aaMode == AA_FXAA || !IsFxaa(key)) return;
  // first OpFOrdGreaterThanEqual (190) after the first OpImageGather (96): the luma-range early-exit test. Make it OpFOrdLessThan (184) of the same
  // operand twice, which is always false, so the shader takes its no-edge path.
  size_t i = 5; bool seenGather = false;
  while (i < w.size()) {
    const uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
    if (!n) break;
    if (op == 96) seenGather = true;
    if (op == 190 && seenGather && n == 5) { w[i] = (5u << 16) | 184u; w[i + 4] = w[i + 3]; fprintf(stderr, "aa: patched FXAA edge test in %.12s\n", key); return; }
    i += n;
  }
  fprintf(stderr, "aa: FXAA edge test not found in %.12s, left unchanged\n", key);
}

// DestroyTexture runs from the graveyard (nvn_life.cpp) only after the GPU has finished with the texture, so the views can go now.
// Without this the cache kept views of destroyed images, and a recycled VkImage handle could hand back a stale view.
void AaForgetTexture(const TextureImpl* t) {
  if (!t || !t->image || !g.h || !g.h->device) return;
  std::lock_guard<std::mutex> lk(g.unormMu);
  for (auto it = g.unormViews.lower_bound(std::make_tuple(t->image, 0u, 0u)); it != g.unormViews.end() && std::get<0>(it->first) == t->image;) {
    vkDestroyImageView(g.h->device, it->second, nullptr);
    it = g.unormViews.erase(it);
  }
}

bool AaIsTonemapKey(const char* key) { return IsTonemap(key); }   // [fx]

void AaAfterDraw(ReplayCtx& cx, const char* key) {
  if (g_aaMode != AA_SMAA && g_aaSharpen <= 0.0f) return;
  if (g.failed || !IsTonemap(key)) return;
  // SNAP_AA_AB=<P> (test aid): SMAA/CAS run only while (present count / P) is odd, for same-frame before/after shots in one run
  static const unsigned long long abP = getenv("SNAP_AA_AB") ? strtoull(getenv("SNAP_AA_AB"), nullptr, 0) : 0;
  if (abP && (((unsigned long long)g_presentCount / abP) & 1) == 0) return;
  TextureImpl* t = cx.rt[0];
  if (!t || !t->image || !t->fi) return;
  const VkFormat fmt = Unorm(t->fi->vk);
  if (fmt == VK_FORMAT_UNDEFINED) { static bool w; if (!w) { w = true; fprintf(stderr, "aa: tonemap target format %d not supported, AA step skipped\n", (int)t->fi->vk); } return; }
  Host* h = cx.h;
  if (!g.ready) { if (!Setup(h)) { g.failed = true; fprintf(stderr, "aa: setup failed, disabled\n"); AaFail(); return; } g.ready = true; }
  const bool smaa = g_aaMode == AA_SMAA, cas = g_aaSharpen > 0.0f;
  EndRendering(cx);
  SubRange rr;
  VkImageView rtv = t->RenderView(cx.rtHasView[0] ? &cx.rtView[0] : nullptr, &rr);
  const uint32_t w = t->LevelW(rr.lb), hh = t->LevelH(rr.lb);
  if (rr.yn != 1 || w < 64 || hh < 64) return;
  if (g_log) { static int n; if (n++ < 12) fprintf(stderr, "aa: p%d key %.12s target %ux%u fmt %d level %u layer %u fxaa=%d\n", g_presentCount, key, w, hh, (int)t->fi->vk, rr.lb, rr.yb, (int)IsFxaa(key)); }
  Pipes* pp = GetPipes(fmt);
  if (!pp) { g.failed = true; fprintf(stderr, "aa: pipeline creation failed (format %d), disabled\n", (int)fmt); AaFail(); return; }
  if (!Resize(w, hh, fmt)) { g.failed = true; fprintf(stderr, "aa: scratch image or descriptor setup failed (%ux%u), disabled\n", w, hh); AaFail(); return; }
  // write view: a UNORM view when the target is sRGB, so SMAA and CAS read and write the stored (gamma) values unchanged
  if (t->fi->vk != fmt) {
    const auto k = std::make_tuple(t->image, (uint32_t)rr.lb, (uint32_t)rr.yb);
    std::lock_guard<std::mutex> ulk(g.unormMu);
    auto it = g.unormViews.find(k);
    if (it == g.unormViews.end()) {
      VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vc.image = t->image; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, 1, rr.yb, 1};
      VkImageView v = VK_NULL_HANDLE;
      if (vkCreateImageView(h->device, &vc, nullptr, &v) != VK_SUCCESS) { g.failed = true; AaFail(); return; }
      it = g.unormViews.emplace(k, v).first;
    }
    rtv = it->second;
  }
  VkCommandBuffer cb = cx.cb;
  if (!g.texUploaded) {
    g.texUploaded = true;
    ImgBar(cb, g.area.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ImgBar(cb, g.search.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy a{}; a.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; a.imageExtent = {160, 560, 1};
    vkCmdCopyBufferToImage(cb, g.stage, g.area.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &a);
    VkBufferImageCopy s{}; s.bufferOffset = sizeof k_smaa_area; s.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; s.imageExtent = {64, 16, 1};
    vkCmdCopyBufferToImage(cb, g.stage, g.search.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &s);
    ImgBar(cb, g.area.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImgBar(cb, g.search.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  if (!dmp.init) {
    dmp.init = true;
    const char* d = getenv("SNAP_AA_DUMP"); const char* a = getenv("SNAP_AA_DUMP_AT");
    if (d && *d && a) { dmp.on = true; dmp.dir = d; for (const char* c = a; *c;) { dmp.at.push_back(atoi(c)); while (*c && *c != ',') c++; if (*c == ',') c++; } }
  }
  if (dmp.pendingAt >= 0 && g_presentCount >= dmp.pendingFrame + 3) {   // the copies recorded a few frames ago are done: write them
    vkDeviceWaitIdle(h->device);
    char nm[64];
    snprintf(nm, sizeof nm, "/aa_p%d_before.bmp", dmp.pendingAt); WriteBmp(dmp.dir + nm, (const uint8_t*)dmp.map[0], dmp.w, dmp.h);
    snprintf(nm, sizeof nm, "/aa_p%d_after.bmp", dmp.pendingAt); WriteBmp(dmp.dir + nm, (const uint8_t*)dmp.map[1], dmp.w, dmp.h);
    if (dmp.hasSmaa) { snprintf(nm, sizeof nm, "/aa_p%d_smaaonly.bmp", dmp.pendingAt); WriteBmp(dmp.dir + nm, (const uint8_t*)dmp.map[2], dmp.w, dmp.h); }
    fprintf(stderr, "aa: dumped before/after pair for present %d (%ux%u)\n", dmp.pendingAt, dmp.w, dmp.h);
    dmp.pendingAt = -1;
  }
  bool dumpNow = dmp.on && dmp.pendingAt < 0 && dmp.idx < dmp.at.size() && g_presentCount >= dmp.at[dmp.idx] && t->fi->bytes == 4;
  if (dumpNow && (dmp.bytes != (uint64_t)w * hh * 4)) {
    dmp.bytes = (uint64_t)w * hh * 4;
    for (int i = 0; i < 3; i++) if (!CreateHostBuffer(h, dmp.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dmp.buf[i], &dmp.mem[i], &dmp.map[i])) { dmp.on = false; dumpNow = false; }
  }
  // 1. copy the target into the scratch image C
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rr);
  ImgBar(cb, g.sz.c.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy cp{}; cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, rr.yb, 1}; cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.extent = {w, hh, 1};
  vkCmdCopyImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.sz.c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  ImgBar(cb, g.sz.c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
  const float rtm[4] = {1.0f / w, 1.0f / hh, (float)w, (float)hh};
  VkImageView first = rtv;   // where the blend pass writes
  if (smaa) {
    ImgBar(cb, g.sz.e.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    FullPass(cx, g.sz.e.view, w, hh, VK_ATTACHMENT_LOAD_OP_CLEAR, pp->edge, g.sz.setEdge, rtm);
    ImgBar(cb, g.sz.e.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImgBar(cb, g.sz.wt.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    FullPass(cx, g.sz.wt.view, w, hh, VK_ATTACHMENT_LOAD_OP_CLEAR, pp->weights, g.sz.setW, rtm);
    ImgBar(cb, g.sz.wt.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (cas) { ImgBar(cb, g.sz.b.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); first = g.sz.b.view; }
    FullPass(cx, first, w, hh, cas ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : (g_split ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE), pp->blend, g.sz.setBlend, rtm, cas ? 0 : (g_split ? w / 2 : 0));
    if (cas) ImgBar(cb, g.sz.b.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  if (cas) {
    const float cpc[4] = {1.0f / w, 1.0f / hh, g_aaSharpen, 0};
    FullPass(cx, rtv, w, hh, g_split ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, pp->cas, smaa ? g.sz.setCas : g.sz.setCasC, cpc, g_split ? w / 2 : 0);
  }
  if (dumpNow) {   // before: the scratch copy of the untouched frame; after: the target as it is now
    VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {w, hh, 1};
    ImgBar(cb, g.sz.c.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cb, g.sz.c.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dmp.buf[0], 1, &bc);
    ImgBar(cb, g.sz.c.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rr);
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, rr.yb, 1};
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dmp.buf[1], 1, &bc);
    TransitionRange(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rr);
    dmp.hasSmaa = smaa && cas;   // SMAA alone (before CAS) is the scratch image B
    if (dmp.hasSmaa) {
      VkBufferImageCopy b2{}; b2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; b2.imageExtent = {w, hh, 1};
      ImgBar(cb, g.sz.b.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      vkCmdCopyImageToBuffer(cb, g.sz.b.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dmp.buf[2], 1, &b2);
      ImgBar(cb, g.sz.b.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    dmp.pendingAt =dmp.at[dmp.idx++]; dmp.pendingFrame = g_presentCount; dmp.w = w; dmp.h = hh;
  }
  t->uploaded = true;
  // order everything above before later draws; put back the viewport and scissor the game had set
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
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
}  // namespace nvn
