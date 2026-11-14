// SPDX-License-Identifier: MIT
// HDR10 present pass; see nvn_hdr.h.
#include "nvn_hdr.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "nvn_hdr_spv.h"

namespace snap_hdr {
namespace {

float EnvF(const char* n, float d) { const char* e = getenv(n); return (e && *e) ? (float)atof(e) : d; }
// SNAP_HDR=off|hdr10|scrgb (1 = hdr10, 0 = off). g_active is what the swapchain actually uses after the fallback chain scrgb -> hdr10 -> SDR.
enum Mode { M_OFF, M_HDR10, M_SCRGB };
// Read lazily (first use), not in a static initializer: settings::load exports gfx_hdr as SNAP_HDR after process start.
Mode Want() {
  static const Mode m = [] {
    const char* e = getenv("SNAP_HDR");
    if (!e || !*e || !strcmp(e, "0") || !_stricmp(e, "off")) return M_OFF;
    if (!_stricmp(e, "scrgb")) return M_SCRGB;
    return M_HDR10;
  }();
  return m;
}
Mode g_active = M_OFF;
bool Req() { return Want() != M_OFF; }
float EnvF2(const char* a, const char* b, float d) { const char* e = getenv(a); if (e && *e) return (float)atof(e); return EnvF(b, d); }
float g_peak = 1000.f;      // resolved in PickFormat (SNAP_HDR_PEAK_NITS, else the display's MaxLuminance, else 1000)
float g_paper = 200.f;
bool g_peakResolved = false;
float Expand() { static const float v = std::clamp(EnvF("SNAP_HDR_EXPAND", 0.3f), 0.f, 1.f); return v; }   // how far near-white is lifted (a plain SDR frame cannot tell UI white from a highlight)

// DXGI_OUTPUT_DESC1::MaxLuminance of the output the window is on (0 when unavailable, e.g. no HDR display or a hidden desktop).
float QueryDisplayPeak(void* hwnd) {
  typedef HRESULT(WINAPI * PFN_CF1)(REFIID, void**);
  HMODULE m = LoadLibraryA("dxgi.dll");
  if (!m) return 0;
  auto cf = (PFN_CF1)GetProcAddress(m, "CreateDXGIFactory1");
  IDXGIFactory1* fac = nullptr;
  float peak = 0;
  if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void**)&fac)) && fac) {
    HMONITOR mon = hwnd ? MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr;
    float any = 0;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
      IDXGIOutput* out = nullptr;
      for (UINT j = 0; ad->EnumOutputs(j, &out) == S_OK; j++) {
        IDXGIOutput6* o6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6)) && o6) {
          DXGI_OUTPUT_DESC1 d{};
          if (SUCCEEDED(o6->GetDesc1(&d))) {
            if (!any) any = d.MaxLuminance;
            if (mon && d.Monitor == mon) peak = d.MaxLuminance;
          }
          o6->Release();
        }
        out->Release();
      }
      ad->Release();
    }
    if (peak <= 0) peak = any;
    fac->Release();
  }
  return peak;
}
void ResolveNits(void* hwnd) {
  if (g_peakResolved) return;
  g_peakResolved = true;
  g_paper = EnvF2("SNAP_HDR_PAPER_NITS", "SNAP_HDR_PAPER", 200.f);
  const char* e = getenv("SNAP_HDR_PEAK_NITS"); if (!e || !*e) e = getenv("SNAP_HDR_PEAK");
  if (e && *e) g_peak = (float)atof(e);
  else {
    const float d = QueryDisplayPeak(hwnd);
    if (d >= 300.f) { g_peak = d; fprintf(stderr, "hdr: display MaxLuminance %.0f nits (DXGI_OUTPUT_DESC1) used as the peak\n", d); }
    else fprintf(stderr, "hdr: display MaxLuminance %s; peak defaults to 1000 nits (set SNAP_HDR_PEAK_NITS)\n", d > 0 ? "is an SDR value" : "unavailable");
  }
  g_peak = std::max(g_peak, 100.f);
  g_paper = std::clamp(g_paper, 20.f, g_peak);
}
PFN_vkSetHdrMetadataEXT pSetHdr = nullptr;
bool g_haveMeta = false;

struct State {
  VkDevice dev = VK_NULL_HANDLE;
  VkFormat fmt = VK_FORMAT_UNDEFINED;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkSampler samp = VK_NULL_HANDLE;
  struct SrcRes { VkImageView view; VkImageView layerView; VkImage layer; VkDescriptorSet set; };
  std::map<VkImage, SrcRes> src;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  std::map<VkImage, VkImageView> dstViews;
  uint32_t sets = 0;
  bool failed = false;
} S;

void DropDst() {
  for (auto& kv : S.dstViews) vkDestroyImageView(S.dev, kv.second, nullptr);
  S.dstViews.clear();
}

bool Build(const Frame& f) {
  if (S.dev && (S.dev != f.device || S.fmt != f.dstFmt)) {
    vkDeviceWaitIdle(S.dev);
    DropDst();
    if (S.pipe) vkDestroyPipeline(S.dev, S.pipe, nullptr);
    S.pipe = VK_NULL_HANDLE;
  }
  S.dev = f.device;
  VkDevice d = S.dev;
  if (!S.samp) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &si, nullptr, &S.samp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &dl, nullptr, &S.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &S.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &S.pl) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; dp.maxSets = 16; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d, &dp, nullptr, &S.pool) != VK_SUCCESS) return false;
  }
  if (!S.pipe) {
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof kHdrVertSpv; sm.pCode = kHdrVertSpv;
    if (vkCreateShaderModule(d, &sm, nullptr, &vs) != VK_SUCCESS) return false;
    sm.codeSize = sizeof kHdrFragSpv; sm.pCode = kHdrFragSpv;
    if (vkCreateShaderModule(d, &sm, nullptr, &fs) != VK_SUCCESS) { vkDestroyShaderModule(d, vs, nullptr); return false; }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &f.dstFmt;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = S.pl;
    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &S.pipe);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (r != VK_SUCCESS) return false;
    S.fmt = f.dstFmt;
  }
  return true;
}

void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
  b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

bool IsSrgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

}  // namespace

bool Requested() { return Req(); }
bool IsScrgb() { return g_active == M_SCRGB; }
float Peak() { return g_peak; }
float Paper() { return g_paper; }
const char* ModeName() { return g_active == M_SCRGB ? "scrgb (inverse tone map)" : "hdr10 (inverse tone map)"; }

void AppendInstanceExt(std::vector<const char*>& ext) {
  if (!Req()) return;
  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> p(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, p.data());
  for (auto& e : p)
    if (!strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) { ext.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME); return; }
  fprintf(stderr, "hdr: SNAP_HDR=1 but the instance lacks %s; the output stays SDR\n", VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
}

void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& ext) {
  if (!Req()) return;
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> p(n);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, p.data());
  for (auto& e : p)
    if (!strcmp(e.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME)) { ext.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME); g_haveMeta = true; return; }
  fprintf(stderr, "hdr: device lacks %s; HDR10 will be created without static metadata\n", VK_EXT_HDR_METADATA_EXTENSION_NAME);
}

void LoadDeviceFuncs(VkDevice dev) {
  if (g_haveMeta) pSetHdr = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(dev, "vkSetHdrMetadataEXT");
}

bool PickFormat(const VkSurfaceFormatKHR* fm, uint32_t n, VkSurfaceFormatKHR* out, void* hwnd) {
  ResolveNits(hwnd);
  if (S.failed) { fprintf(stderr, "hdr: the HDR pass failed earlier; staying SDR\n"); return false; }
  std::string l;
  for (uint32_t i = 0; i < n; i++) { char b[40]; snprintf(b, sizeof b, " %d/%d", (int)fm[i].format, (int)fm[i].colorSpace); l += b; }
  if (Want() == M_SCRGB) {
    for (uint32_t i = 0; i < n; i++)
      if (fm[i].format == VK_FORMAT_R16G16B16A16_SFLOAT && fm[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) { *out = fm[i]; g_active = M_SCRGB; return true; }
    fprintf(stderr, "hdr: RGBA16F + EXTENDED_SRGB_LINEAR (scRGB) not offered (surface formats format/colorspace:%s); trying HDR10\n", l.c_str());
  }
  for (uint32_t i = 0; i < n; i++)
    if (fm[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && fm[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) { *out = fm[i]; g_active = M_HDR10; return true; }
  fprintf(stderr, "hdr: A2B10G10R10 + HDR10_ST2084 not offered by the display/driver (surface formats format/colorspace:%s); falling back to SDR\n", l.c_str());
  g_active = M_OFF;
  return false;
}

void SetMetadata(VkDevice dev, VkSwapchainKHR sc) {
  if (!pSetHdr) return;
  VkHdrMetadataEXT m{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
  m.displayPrimaryRed = {0.708f, 0.292f}; m.displayPrimaryGreen = {0.170f, 0.797f}; m.displayPrimaryBlue = {0.131f, 0.046f};
  m.whitePoint = {0.3127f, 0.3290f};
  m.maxLuminance = g_peak; m.minLuminance = 0.001f;
  m.maxContentLightLevel = g_peak; m.maxFrameAverageLightLevel = g_paper;
  pSetHdr(dev, 1, &sc, &m);
}

namespace {
struct ScratchState {
  VkDevice dev = VK_NULL_HANDLE; VkExtent2D ext{};
  VkImage img[2]{}; VkDeviceMemory mem[2]{};
} SS;
bool MakeImg(VkPhysicalDevice phys, VkDevice d, VkExtent2D ext, VkFormat fmt, VkImageUsageFlags use, VkImage* im, VkDeviceMemory* mem) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {ext.width, ext.height, 1}; ci.mipLevels = ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = use; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(d, &ci, nullptr, im) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(d, *im, &mr);
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  uint32_t t = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { t = i; break; }
  if (t == UINT32_MAX) return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = t;
  if (vkAllocateMemory(d, &ai, nullptr, mem) != VK_SUCCESS) return false;
  return vkBindImageMemory(d, *im, *mem, 0) == VK_SUCCESS;
}
void DropScratch() {
  if (!SS.dev) return;
  vkDeviceWaitIdle(SS.dev);
  for (auto& kv : S.src) { vkDestroyImageView(SS.dev, kv.second.view, nullptr); vkDestroyImageView(SS.dev, kv.second.layerView, nullptr); vkFreeDescriptorSets(SS.dev, S.pool, 1, &kv.second.set); }
  S.src.clear(); S.sets = 0;
  for (int i = 0; i < 2; i++) { if (SS.img[i]) vkDestroyImage(SS.dev, SS.img[i], nullptr); if (SS.mem[i]) vkFreeMemory(SS.dev, SS.mem[i], nullptr); SS.img[i] = VK_NULL_HANDLE; SS.mem[i] = VK_NULL_HANDLE; }
  SS.dev = VK_NULL_HANDLE;
}
}  // namespace

bool PrepScratch(VkCommandBuffer cb, VkPhysicalDevice phys, VkDevice dev, VkExtent2D ext, Scratch* out) {
  if (S.failed) return false;
  if (SS.dev && (SS.dev != dev || SS.ext.width != ext.width || SS.ext.height != ext.height)) DropScratch();
  if (!SS.dev) {
    const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!MakeImg(phys, dev, ext, VK_FORMAT_R8G8B8A8_SRGB, u, &SS.img[0], &SS.mem[0]) || !MakeImg(phys, dev, ext, VK_FORMAT_R8G8B8A8_UNORM, u, &SS.img[1], &SS.mem[1])) {
      SS.dev = dev; DropScratch(); S.failed = true; fprintf(stderr, "hdr: scratch image setup failed\n"); return false;
    }
    SS.dev = dev; SS.ext = ext;
  }
  out->sdr = SS.img[0]; out->sdrFmt = VK_FORMAT_R8G8B8A8_SRGB; out->layer = SS.img[1]; out->layerFmt = VK_FORMAT_R8G8B8A8_UNORM;
  VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  out->dbg = getenv("SNAP_HDR_DBG") ? atoi(getenv("SNAP_HDR_DBG")) : 0;
  VkClearColorValue black{{out->dbg == 1 ? 1.f : 0.f, 0.f, 0.f, 1.f}}, clear{{0.f, 0.f, 0.f, 0.f}};
  for (int i = 0; i < 2; i++) {
    Barrier(cb, SS.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, SS.img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, i ? &clear : &black, 1, &rg);
    Barrier(cb, SS.img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  }
  return true;
}

bool Record(const Frame& f) {
  if (S.failed) return false;
  if (S.sc != f.sc) { if (S.dev) { vkDeviceWaitIdle(S.dev); DropDst(); } S.sc = f.sc; }
  if (!Build(f)) { S.failed = true; fprintf(stderr, "hdr: pass setup failed; blitting SDR\n"); return false; }
  VkDevice d = f.device;
  auto it = S.src.find(f.src);
  if (it != S.src.end() && it->second.layer != f.layer) { vkDeviceWaitIdle(d); vkDestroyImageView(d, it->second.view, nullptr); vkDestroyImageView(d, it->second.layerView, nullptr); vkFreeDescriptorSets(d, S.pool, 1, &it->second.set); S.sets--; S.src.erase(it); it = S.src.end(); }
  if (it == S.src.end()) {
    if (S.sets >= 16) { S.failed = true; return false; }
    State::SrcRes r{};
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = f.src; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f.srcFmt;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d, &vc, nullptr, &r.view) != VK_SUCCESS) { S.failed = true; return false; }
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = S.pool; da.descriptorSetCount = 1; da.pSetLayouts = &S.dsl;
    if (vkAllocateDescriptorSets(d, &da, &r.set) != VK_SUCCESS) { S.failed = true; return false; }
    S.sets++;
    vc.image = f.layer; vc.format = VK_FORMAT_R8G8B8A8_UNORM;
    if (vkCreateImageView(d, &vc, nullptr, &r.layerView) != VK_SUCCESS) { S.failed = true; return false; }
    r.layer = f.layer;
    VkDescriptorImageInfo di[2] = {{S.samp, r.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {S.samp, r.layerView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int i = 0; i < 2; i++) { w[i].dstSet = r.set; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &di[i]; }
    vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    it = S.src.emplace(f.src, r).first;
  }
  auto dv = S.dstViews.find(f.dst);
  if (dv == S.dstViews.end()) {
    VkImageView v;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = f.dst; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f.dstFmt;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d, &vc, nullptr, &v) != VK_SUCCESS) { S.failed = true; return false; }
    dv = S.dstViews.emplace(f.dst, v).first;
  }
  VkCommandBuffer cb = f.cb;
  for (VkImage im : {f.src, f.layer})   // the scratch images come back from the look/overlay passes in TRANSFER_DST
    Barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
  Barrier(cb, f.dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = dv->second; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue.color = {{0.f, 0.f, 0.f, 1.f}};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, f.dstExt}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cb, &ri);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl, 0, 1, &it->second.set, 0, nullptr);
  VkViewport vp{(float)f.dx, (float)f.dy, (float)f.dw, (float)f.dh, 0.f, 1.f};
  VkRect2D sc{{f.dx, f.dy}, {(uint32_t)f.dw, (uint32_t)f.dh}};
  vkCmdSetViewport(cb, 0, 1, &vp);
  vkCmdSetScissor(cb, 0, 1, &sc);
  const float iw = 1.0f / (float)f.srcW, ih = 1.0f / (float)f.srcH;
  const float pc[12] = {f.sx * iw, f.sy * ih, f.sw * iw, f.sh * ih, g_paper, g_peak, IsSrgb(f.srcFmt) ? 1.f : 0.f, Expand(), g_active == M_SCRGB ? 1.f : 0.f, 0.f, 0.f, 0.f};
  vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  Barrier(cb, f.dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  return true;
}

namespace {
float PqEotf(float e) {
  const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  const float p = std::pow(std::max(e, 0.f), 1.f / m2);
  return 10000.f * std::pow(std::max(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
}
float SrgbOetf(float x) { x = std::clamp(x, 0.f, 1.f); return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f; }
void WriteBmp(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
  std::vector<uint8_t> out((size_t)w * h * 3 + 54, 0);
  uint8_t* o = out.data();
  auto le32 = [&](int off, uint32_t x) { memcpy(o + off, &x, 4); };
  o[0] = 'B'; o[1] = 'M'; le32(2, (uint32_t)out.size()); le32(10, 54); le32(14, 40); le32(18, w); le32(22, (uint32_t)(-(int)h)); o[26] = 1; o[28] = 24;
  le32(34, (uint32_t)((size_t)w * h * 3));
  for (size_t i = 0; i < (size_t)w * h; i++) { o[54 + i * 3] = rgb[i * 3 + 2]; o[55 + i * 3] = rgb[i * 3 + 1]; o[56 + i * 3] = rgb[i * 3]; }
  if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
}
}  // namespace

namespace {
float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp((float)m, -24);
  else if (e == 31) v = m ? 0.f : 65504.f;
  else v = std::ldexp((float)(m + 1024), (int)e - 25);
  return s ? -v : v;
}
}  // namespace

void DumpPQ(const char* base, const void* px, uint32_t w, uint32_t h) {
  const bool sc = g_active == M_SCRGB;
  const uint32_t* p32 = static_cast<const uint32_t*>(px);
  const uint16_t* p16 = static_cast<const uint16_t*>(px);
  // linear Rec.709 nits of one pixel
  auto nitsAt = [&](uint32_t x, uint32_t y, float out[3]) {
    if (sc) {
      const uint16_t* t = p16 + ((size_t)y * w + x) * 4;
      for (int k = 0; k < 3; k++) out[k] = HalfToFloat(t[k]) * 80.f;
      return;
    }
    const uint32_t v = p32[(size_t)y * w + x];
    const float c[3] = {PqEotf((v & 1023) / 1023.f), PqEotf(((v >> 10) & 1023) / 1023.f), PqEotf(((v >> 20) & 1023) / 1023.f)};
    out[0] = 1.6605f * c[0] - 0.5876f * c[1] - 0.0728f * c[2];   // Rec.2020 -> Rec.709
    out[1] = -0.1246f * c[0] + 1.1329f * c[1] - 0.0083f * c[2];
    out[2] = -0.0182f * c[0] - 0.1006f * c[1] + 1.1187f * c[2];
  };
  const uint32_t W = w / 2, H = h / 2;   // half-size preview, point sampled
  std::vector<uint8_t> a((size_t)W * H * 3), b((size_t)W * H * 3);
  double sum = 0, maxN = 0; uint64_t over = 0, n = 0;
  for (uint32_t y = 0; y < H; y++)
    for (uint32_t x = 0; x < W; x++) {
      float c[3];
      nitsAt(x * 2, y * 2, c);
      const float r = c[0], g = c[1], bl = c[2];
      const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * bl;
      sum += lum; maxN = std::max<double>(maxN, lum); n++; if (lum > g_paper * 1.05f) over++;
      auto tm = [&](float nits) { float t = nits / g_paper; if (t > 0.8f) t = 0.8f + 0.2f * (1.f - std::exp(-(t - 0.8f) / 0.2f)); return SrgbOetf(t); };
      const size_t o = ((size_t)y * W + x) * 3;
      a[o] = (uint8_t)(tm(r) * 255.f + 0.5f); a[o + 1] = (uint8_t)(tm(g) * 255.f + 0.5f); a[o + 2] = (uint8_t)(tm(bl) * 255.f + 0.5f);
      if (lum <= g_paper) { const uint8_t gv = (uint8_t)(SrgbOetf(lum / g_paper) * 128.f); b[o] = b[o + 1] = b[o + 2] = gv; }
      else {   // above paper white: log scale to the peak, dark blue -> cyan -> yellow -> red
        const float t = std::clamp(std::log2(lum / g_paper) / std::max(std::log2(g_peak / g_paper), 0.01f), 0.f, 1.f);
        const float cr = std::clamp(t * 2.f - 0.6f, 0.f, 1.f), cg = std::clamp(t < 0.5f ? t * 2.f : 2.f - t * 2.f, 0.f, 1.f), cb2 = std::clamp(1.f - t * 2.f, 0.2f, 1.f);
        b[o] = (uint8_t)(255 * cr); b[o + 1] = (uint8_t)(255 * cg); b[o + 2] = (uint8_t)(255 * cb2);
      }
    }
  WriteBmp(std::string(base) + ".bmp", a, W, H);
  WriteBmp(std::string(base) + "_nits.bmp", b, W, H);
  // Banding probe: the green channel down the centre column of the top quarter (sky in the course frames). Counts distinct stored code
  // values and gives the relative size of one code step at the strip's mean level.
  std::map<uint32_t, int> codes; double sMean = 0; uint32_t sN = h / 4; uint32_t gMid = 0;
  for (uint32_t y = 0; y < sN; y++) {
    float c[3]; nitsAt(w / 2, y, c); sMean += c[1];
    const uint32_t code = sc ? (uint32_t)p16[((size_t)y * w + w / 2) * 4 + 1] : ((p32[(size_t)y * w + w / 2] >> 10) & 1023);
    codes[code]++; gMid = code;
  }
  sMean /= std::max<uint32_t>(sN, 1);
  double stepPct = 0;
  if (sc) { const float a0 = HalfToFloat((uint16_t)gMid), a1 = HalfToFloat((uint16_t)(gMid + 1)); stepPct = a0 > 0 ? 100.0 * (a1 - a0) / a0 : 0; }
  else { const float a0 = PqEotf(gMid / 1023.f), a1 = PqEotf((gMid + 1) / 1023.f); stepPct = a0 > 0 ? 100.0 * (a1 - a0) / a0 : 0; }
  fprintf(stderr, "hdr: dump %s %ux%u %s mean %.1f nits, max %.1f nits, %.2f%% of pixels above paper white (%.0f); sky strip: %zu distinct G codes in %u rows, mean %.1f nits, one code step = %.3f%%\n", base, w, h,
          sc ? "scRGB" : "HDR10", sum / std::max<uint64_t>(n, 1), maxN, 100.0 * (double)over / (double)std::max<uint64_t>(n, 1), g_paper, codes.size(), sN, sMean, stepPct);
}

}  // namespace snap_hdr
