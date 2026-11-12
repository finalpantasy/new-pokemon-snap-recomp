// SPDX-License-Identifier: MIT
// Intel XeSS Super Resolution through libxess.dll (XeSS SDK 3.0.2, Intel Simplified Software License; local copy in
// tools\intel\, git-ignored, copied beside the exe by host/build_snap.py). No Intel header is compiled in: the few types
// and entry points used are declared here from the SDK's documented C ABI (xess.h, xess_vk.h). On non-Intel GPUs XeSS runs
// its DP4a path.
// Adapted from the Astral Chain PC recomp, runtime/vk_xess.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh, commit e858fc6;
// see THIRD_PARTY_NOTICES.md): the ABI declarations, the device feature check and the quality pick.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstring>

#include "upscaler.h"

namespace snap_upscale::xess {
namespace {

typedef int XessResult;   // xess_result_t: 0 success, 1..2 warnings, negative errors
inline bool XessOk(XessResult r) { return r >= 0; }
struct XessCtx;
typedef XessCtx* XessHandle;
struct XessVersion { uint16_t major, minor, patch, reserved; };
struct Xess2d { uint32_t x, y; };
enum { XESS_QUALITY_FIRST = 100, XESS_QUALITY_LAST_SCALED = 105, XESS_QUALITY_AA = 106 };
enum { XESS_FLAG_INVERTED_DEPTH = 1 << 1, XESS_FLAG_LDR = 1 << 6, XESS_FLAG_AUTOEXPOSURE = 1 << 8 };
enum { XESS_LOG_DEBUG, XESS_LOG_INFO, XESS_LOG_WARNING, XESS_LOG_ERROR };
struct XessView { VkImageView imageView; VkImage image; VkImageSubresourceRange subresourceRange; VkFormat format; unsigned width, height; };
struct XessExec {
  XessView color, velocity, depth, exposure, responsive, output;
  float jitterX, jitterY, exposureScale;
  uint32_t resetHistory, inputWidth, inputHeight;
  Xess2d colorBase, mvBase, depthBase, responsiveBase, reserved0, outputBase;
};
struct XessInit {
  Xess2d outputResolution; int qualitySetting; uint32_t initFlags, creationNodeMask, visibleNodeMask;
  VkDeviceMemory tempBufferHeap; uint64_t bufferHeapOffset; VkDeviceMemory tempTextureHeap; uint64_t textureHeapOffset;
  VkPipelineCache pipelineCache;
};
static_assert(sizeof(XessView) == 48, "xess_vk_image_view_info layout");
static_assert(sizeof(XessExec) == 6 * 48 + 24 + 6 * 8, "xess_vk_execute_params_t layout");
static_assert(sizeof(XessInit) == 64, "xess_vk_init_params_t layout");

typedef void (*XessLogFn)(const char* message, int level);
typedef XessResult (*PFN_GetVersion)(XessVersion*);
typedef XessResult (*PFN_InstExts)(uint32_t*, const char* const**, uint32_t*);
typedef XessResult (*PFN_DevExts)(VkInstance, VkPhysicalDevice, uint32_t*, const char* const**);
typedef XessResult (*PFN_DevFeats)(VkInstance, VkPhysicalDevice, void**);
typedef XessResult (*PFN_Create)(VkInstance, VkPhysicalDevice, VkDevice, XessHandle*);
typedef XessResult (*PFN_Build)(XessHandle, VkPipelineCache, bool, uint32_t);
typedef XessResult (*PFN_Init)(XessHandle, const XessInit*);
typedef XessResult (*PFN_Execute)(XessHandle, VkCommandBuffer, const XessExec*);
typedef XessResult (*PFN_Destroy)(XessHandle);
typedef XessResult (*PFN_SetLog)(XessHandle, int, XessLogFn);
typedef XessResult (*PFN_Optimal)(XessHandle, const Xess2d*, int, Xess2d*, Xess2d*, Xess2d*);

struct State {
  HMODULE lib = nullptr;
  PFN_GetVersion version = nullptr; PFN_InstExts instExts = nullptr; PFN_DevExts devExts = nullptr; PFN_DevFeats devFeats = nullptr;
  PFN_Create create = nullptr; PFN_Build build = nullptr; PFN_Init init = nullptr; PFN_Execute execute = nullptr;
  PFN_Destroy destroy = nullptr; PFN_SetLog setLog = nullptr; PFN_Optimal optimal = nullptr;
  bool tried = false, loaded = false, deviceOk = false, failed = false;
  DeviceCtx dc;
  XessHandle ctx = nullptr;
  uint32_t w = 0, h = 0, ow = 0, oh = 0, flags = 0; int quality = 0;
  uint64_t evals = 0;
} X;

void OnLog(const char* message, int level) { if (level >= XESS_LOG_WARNING) Logf("xess: %s: %s", level >= XESS_LOG_ERROR ? "error" : "warning", message); }
bool Fail(const char* what, XessResult r) { Logf("xess: %s failed (%d): XeSS off for this run", what, r); X.failed = true; return false; }

// The feature structures XeSS may ask for that this device setup can satisfy: the core 1.0-1.3 ones (upscale.cpp enables
// every supported core feature while an upscaler is wanted) and VK_EXT_mutable_descriptor_type (chained by upscale.cpp).
bool KnownFeature(VkStructureType s, size_t* size) {
#define FS(stype, type) case VK_STRUCTURE_TYPE_##stype: *size = sizeof(type); return true;
  switch (s) {
    FS(PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2)
    FS(PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features)
    FS(PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features)
    FS(PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features)
    FS(PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures)
    FS(PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures)
    FS(PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features)
    FS(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures)
    FS(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures)
    FS(PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures)
    FS(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures)
    FS(PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures)
    FS(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures)
    FS(PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures)
    FS(PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures)
    FS(PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures)
    FS(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features)
    FS(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures)
    FS(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features)
    FS(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures)
    FS(PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT)
    default: return false;
  }
#undef FS
}

bool FeatureSupported(VkPhysicalDevice pd, const VkBaseOutStructure* want, size_t size) {
  alignas(16) uint8_t have[1024];
  if (size > sizeof have) return false;
  memset(have, 0, size);
  ((VkBaseOutStructure*)have)->sType = want->sType;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  const bool isF2 = want->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  if (!isF2) f2.pNext = have;
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  const VkBool32* w = (const VkBool32*)((const uint8_t*)want + sizeof(VkBaseOutStructure));
  const VkBool32* h = isF2 ? (const VkBool32*)&f2.features : (const VkBool32*)(have + sizeof(VkBaseOutStructure));
  const size_t n = (size - sizeof(VkBaseOutStructure)) / sizeof(VkBool32);
  for (size_t i = 0; i < n; i++)
    if (w[i] && !h[i]) { Logf("xess: the device lacks feature %zu of structure %d", i, (int)want->sType); return false; }
  return true;
}

bool Listed(const std::vector<const char*>& v, const char* s) { for (auto* e : v) if (!strcmp(e, s)) return true; return false; }

int PickQuality(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
  if (w >= ow && h >= oh) return XESS_QUALITY_AA;
  int best = 0; uint32_t bestD = ~0u;
  Xess2d out{ow, oh};
  for (int q = XESS_QUALITY_FIRST; X.optimal && q <= XESS_QUALITY_LAST_SCALED; q++) {
    Xess2d opt{}, lo{}, hi{};
    if (!XessOk(X.optimal(X.ctx, &out, q, &opt, &lo, &hi))) continue;
    if (w < lo.x || h < lo.y || w > hi.x || h > hi.y) continue;
    const uint32_t d = opt.x > w ? opt.x - w : w - opt.x;
    if (d < bestD) { best = q; bestD = d; }
  }
  if (best) return best;
  const double r = (double)ow / w;
  return r >= 2.65 ? 100 : r >= 2.15 ? 101 : r >= 1.85 ? 102 : r >= 1.6 ? 103 : r >= 1.4 ? 104 : 105;
}

double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

bool Load() {
  if (X.tried) return X.loaded;
  X.tried = true;
  char path[MAX_PATH + 32] = {};
  DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
  while (len && path[len - 1] != '\\') len--;
  strcpy(path + len, "libxess.dll");
  X.lib = LoadLibraryA(path);
  if (!X.lib) { Logf("xess: cannot load %s: XeSS unavailable", path); return false; }
#define FN(f, name) X.f = (decltype(X.f))(void*)GetProcAddress(X.lib, name)
  FN(version, "xessGetVersion"); FN(instExts, "xessVKGetRequiredInstanceExtensions"); FN(devExts, "xessVKGetRequiredDeviceExtensions");
  FN(devFeats, "xessVKGetRequiredDeviceFeatures"); FN(create, "xessVKCreateContext"); FN(build, "xessVKBuildPipelines");
  FN(init, "xessVKInit"); FN(execute, "xessVKExecute"); FN(destroy, "xessDestroyContext"); FN(setLog, "xessSetLoggingCallback");
  FN(optimal, "xessGetOptimalInputResolution");
#undef FN
  if (!X.version || !X.instExts || !X.devExts || !X.devFeats || !X.create || !X.init || !X.execute || !X.destroy) {
    Logf("xess: %s lacks Vulkan entry points: XeSS unavailable", path);
    return false;
  }
  XessVersion v{};
  X.version(&v);
  Logf("xess: %s, XeSS %u.%u.%u", path, v.major, v.minor, v.patch);
  return X.loaded = true;
}

void InstanceExtensions(std::vector<const char*>& ext) {
  if (!X.loaded) return;
  uint32_t ni = 0, api = 0;
  const char* const* ie = nullptr;
  if (!XessOk(X.instExts(&ni, &ie, &api))) { Logf("xess: no instance extension list: XeSS unavailable"); X.loaded = false; return; }
  for (uint32_t i = 0; i < ni; i++) if (!Listed(ext, ie[i])) ext.push_back(ie[i]);
}

bool DeviceExtensions(VkInstance inst, VkPhysicalDevice pd, std::vector<const char*>& ext, bool* needMutableDesc) {
  if (!X.loaded) return false;
  uint32_t nd = 0;
  const char* const* de = nullptr;
  if (!XessOk(X.devExts(inst, pd, &nd, &de))) { Logf("xess: no device extension list for this GPU: XeSS unavailable"); return false; }
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, have.data());
  for (uint32_t i = 0; i < nd; i++) {
    bool ok = false;
    for (auto& h : have) if (!strcmp(h.extensionName, de[i])) { ok = true; break; }
    if (!ok) { Logf("xess: the device lacks %s: XeSS unavailable", de[i]); return false; }
  }
  void* chain = nullptr;
  if (!XessOk(X.devFeats(inst, pd, &chain))) { Logf("xess: no device feature list: XeSS unavailable"); return false; }
  for (const VkBaseOutStructure* s = (const VkBaseOutStructure*)chain; s; s = s->pNext) {
    size_t size = 0;
    if (!KnownFeature(s->sType, &size)) { Logf("xess: XeSS asks for features in structure %d, not enabled here: XeSS unavailable", (int)s->sType); return false; }
    if (!FeatureSupported(pd, s, size)) return false;
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT) *needMutableDesc = true;
  }
  for (uint32_t i = 0; i < nd; i++) if (!Listed(ext, de[i])) ext.push_back(de[i]);
  X.deviceOk = true;
  return true;
}

bool Init(const DeviceCtx& c) {
  if (!X.loaded || !X.deviceOk) return false;
  X.dc = c;
  return true;
}

bool Evaluate(const Eval& e) {
  if (!X.loaded || !X.deviceOk || X.failed) return false;
  const uint32_t flags = (e.hdr ? XESS_FLAG_AUTOEXPOSURE : XESS_FLAG_LDR) | (e.invDepth ? XESS_FLAG_INVERTED_DEPTH : 0u);
  if (!X.ctx) {
    const double t0 = NowMs();
    XessResult r = X.create(X.dc.instance, X.dc.phys, X.dc.device, &X.ctx);
    if (!XessOk(r) || !X.ctx) return Fail("context creation", r);
    if (X.setLog) X.setLog(X.ctx, XESS_LOG_WARNING, OnLog);
    if (X.build && !XessOk(r = X.build(X.ctx, VK_NULL_HANDLE, true, flags))) return Fail("pipeline build", r);
    Logf("xess: context made, pipelines built in %.0f ms", NowMs() - t0);
  }
  if (!(X.ow == e.ow && X.oh == e.oh && X.flags == flags && X.w == e.w && X.h == e.h)) {
    const int q = PickQuality(e.w, e.h, e.ow, e.oh);
    XessInit ip{};
    ip.outputResolution = {e.ow, e.oh}; ip.qualitySetting = q; ip.initFlags = flags;
    // XeSS may submit its own copies while it initialises: the caller holds the queue lock (replay), and the GPU must be done
    // with the previous size's work.
    if (X.ow) vkDeviceWaitIdle(X.dc.device);
    XessResult r = X.init(X.ctx, &ip);
    if (!XessOk(r)) return Fail("initialisation", r);
    X.w = e.w; X.h = e.h; X.ow = e.ow; X.oh = e.oh; X.flags = flags; X.quality = q;
    Logf("xess: %ux%u -> %ux%u, quality setting %d%s, %s colour", e.w, e.h, e.ow, e.oh, q, q == XESS_QUALITY_AA ? " (native AA)" : "", e.hdr ? "HDR" : "LDR");
  }
  XessExec p; memset(&p, 0, sizeof p);
  auto vw = [](const Image& m, VkImageAspectFlags a, uint32_t w, uint32_t h) { return XessView{m.view, m.image, {a, 0, 1, 0, 1}, m.format, w, h}; };
  p.color = vw(e.color, VK_IMAGE_ASPECT_COLOR_BIT, e.w, e.h);
  p.velocity = vw(e.mv, VK_IMAGE_ASPECT_COLOR_BIT, e.w, e.h);
  p.depth = vw(e.depth, VK_IMAGE_ASPECT_DEPTH_BIT, e.w, e.h);
  p.output = vw(e.out, VK_IMAGE_ASPECT_COLOR_BIT, e.ow, e.oh);
  p.jitterX = e.jx; p.jitterY = e.jy;
  p.exposureScale = 1.0f;
  p.resetHistory = e.reset ? 1u : 0u;
  p.inputWidth = e.w; p.inputHeight = e.h;
  XessResult r = X.execute(X.ctx, e.cb, &p);
  if (!XessOk(r)) return Fail("evaluation", r);
  X.evals++;
  return true;
}

void Release() {
  if (X.ctx) X.destroy(X.ctx);
  X.ctx = nullptr; X.w = X.h = X.ow = X.oh = 0;
}

}  // namespace snap_upscale::xess
