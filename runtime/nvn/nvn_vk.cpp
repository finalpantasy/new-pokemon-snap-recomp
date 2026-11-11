// SPDX-License-Identifier: MIT
// Vulkan 1.3 backend of the NVN layer: instance/device, images, queues, replay of recorded commands,
// swapchain presentation. NVN calls only record host data; all Vulkan work happens here at submit.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "nvn_internal.h"

namespace nvn {

Host* g_host = nullptr;
std::atomic<uint32_t> g_valErrors{0}, g_valWarnings{0};
NvnHostHooks g_hooks{};
int g_validationRequest = -1;
double g_paceHz = 60.0;

static bool LogEnabled() {
  static int v = -1;
  if (v < 0) { const char* e = getenv("NVN_LOG"); v = (e && *e == '1') ? 1 : 0; }
  return v == 1;
}
void Log(const char* fmt, ...) {
  if (!LogEnabled()) return;
  va_list a; va_start(a, fmt); fprintf(stderr, "[nvn] "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n"); va_end(a);
}
void LogError(const char* fmt, ...) {
  va_list a; va_start(a, fmt); fprintf(stderr, "[nvn][error] "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n"); va_end(a);
}

#define VKCHK(x)                                                                         \
  do {                                                                                   \
    VkResult r_ = (x);                                                                   \
    if (r_ < 0) { LogError("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); } \
  } while (0)

// ---- format table -------------------------------------------------------------------------------------------
// NVN format -> Vulkan. Values: see nvn_types.h. Byte sizes are per texel (uncompressed formats only here).
static FormatInfo g_fmts[0xA0];
static bool g_fmtInit = false;
static void SetF(int n, VkFormat vk, uint32_t bytes, VkImageAspectFlags asp = VK_IMAGE_ASPECT_COLOR_BIT,
                 uint8_t kind = 0) {
  g_fmts[n] = {vk, bytes, asp, kind};
}
static void SetBc(int n, VkFormat vk, uint32_t bytesPerBlock, bool srgb = false) {
  g_fmts[n] = {vk, bytesPerBlock, VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 4, 0, 0, srgb};
}
static const uint8_t kAstcDims[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
static const VkFormat kAstcUnorm[14] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
  VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_UNORM_BLOCK,
  VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_UNORM_BLOCK,
  VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
static VkFormat AstcSrgb(VkFormat u) { return (VkFormat)(u + (VK_FORMAT_ASTC_4x4_SRGB_BLOCK - VK_FORMAT_ASTC_4x4_UNORM_BLOCK)); }
static void InitFormats() {
  if (g_fmtInit) return;
  g_fmtInit = true;
  const auto C = VK_IMAGE_ASPECT_COLOR_BIT;
  SetF(FMT_R8, VK_FORMAT_R8_UNORM, 1); SetF(FMT_R8SN, VK_FORMAT_R8_SNORM, 1);
  SetF(FMT_R8UI, VK_FORMAT_R8_UINT, 1, C, 1); SetF(FMT_R8I, VK_FORMAT_R8_SINT, 1, C, 2);
  SetF(FMT_R16F, VK_FORMAT_R16_SFLOAT, 2); SetF(FMT_R16, VK_FORMAT_R16_UNORM, 2); SetF(FMT_R16SN, VK_FORMAT_R16_SNORM, 2);
  SetF(FMT_R16UI, VK_FORMAT_R16_UINT, 2, C, 1); SetF(FMT_R16I, VK_FORMAT_R16_SINT, 2, C, 2);
  SetF(FMT_R32F, VK_FORMAT_R32_SFLOAT, 4); SetF(FMT_R32UI, VK_FORMAT_R32_UINT, 4, C, 1); SetF(FMT_R32I, VK_FORMAT_R32_SINT, 4, C, 2);
  SetF(FMT_RG8, VK_FORMAT_R8G8_UNORM, 2); SetF(FMT_RG8SN, VK_FORMAT_R8G8_SNORM, 2);
  SetF(FMT_RG8UI, VK_FORMAT_R8G8_UINT, 2, C, 1); SetF(FMT_RG8I, VK_FORMAT_R8G8_SINT, 2, C, 2);
  SetF(FMT_RG16F, VK_FORMAT_R16G16_SFLOAT, 4); SetF(FMT_RG16, VK_FORMAT_R16G16_UNORM, 4); SetF(FMT_RG16SN, VK_FORMAT_R16G16_SNORM, 4);
  SetF(FMT_RG16UI, VK_FORMAT_R16G16_UINT, 4, C, 1); SetF(FMT_RG16I, VK_FORMAT_R16G16_SINT, 4, C, 2);
  SetF(FMT_RG32F, VK_FORMAT_R32G32_SFLOAT, 8); SetF(FMT_RG32UI, VK_FORMAT_R32G32_UINT, 8, C, 1); SetF(FMT_RG32I, VK_FORMAT_R32G32_SINT, 8, C, 2);
  // 3-component formats (vertex attributes only; NVN values 0x19..0x24 [TOTK-NVN])
  SetF(0x19, VK_FORMAT_R8G8B8_UNORM, 3); SetF(0x1A, VK_FORMAT_R8G8B8_SNORM, 3);
  SetF(0x1B, VK_FORMAT_R8G8B8_UINT, 3, C, 1); SetF(0x1C, VK_FORMAT_R8G8B8_SINT, 3, C, 2);
  SetF(0x1D, VK_FORMAT_R16G16B16_SFLOAT, 6); SetF(0x1E, VK_FORMAT_R16G16B16_UNORM, 6); SetF(0x1F, VK_FORMAT_R16G16B16_SNORM, 6);
  SetF(0x20, VK_FORMAT_R16G16B16_UINT, 6, C, 1); SetF(0x21, VK_FORMAT_R16G16B16_SINT, 6, C, 2);
  SetF(0x22, VK_FORMAT_R32G32B32_SFLOAT, 12); SetF(0x23, VK_FORMAT_R32G32B32_UINT, 12, C, 1); SetF(0x24, VK_FORMAT_R32G32B32_SINT, 12, C, 2);
  SetF(FMT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4); SetF(FMT_RGBA8SN, VK_FORMAT_R8G8B8A8_SNORM, 4);
  SetF(FMT_RGBA8UI, VK_FORMAT_R8G8B8A8_UINT, 4, C, 1); SetF(FMT_RGBA8I, VK_FORMAT_R8G8B8A8_SINT, 4, C, 2);
  SetF(FMT_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8); SetF(FMT_RGBA16, VK_FORMAT_R16G16B16A16_UNORM, 8);
  SetF(FMT_RGBA16SN, VK_FORMAT_R16G16B16A16_SNORM, 8);
  SetF(FMT_RGBA16UI, VK_FORMAT_R16G16B16A16_UINT, 8, C, 1); SetF(FMT_RGBA16I, VK_FORMAT_R16G16B16A16_SINT, 8, C, 2);
  SetF(FMT_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16); SetF(FMT_RGBA32UI, VK_FORMAT_R32G32B32A32_UINT, 16, C, 1);
  SetF(FMT_RGBA32I, VK_FORMAT_R32G32B32A32_SINT, 16, C, 2);
  SetF(FMT_STENCIL8, VK_FORMAT_S8_UINT, 1, VK_IMAGE_ASPECT_STENCIL_BIT);
  SetF(FMT_DEPTH16, VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT);
  SetF(FMT_DEPTH24, VK_FORMAT_X8_D24_UNORM_PACK32, 4, VK_IMAGE_ASPECT_DEPTH_BIT);
  SetF(FMT_DEPTH32F, VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT);
  SetF(FMT_DEPTH24_STENCIL8, VK_FORMAT_D24_UNORM_S8_UINT, 4, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  SetF(FMT_DEPTH32F_STENCIL8, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  SetF(FMT_RGBX8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4); SetF(FMT_RGBA8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4);
  SetF(FMT_RGB10A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4); SetF(FMT_R11G11B10F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4);
  SetF(FMT_BGRX8, VK_FORMAT_B8G8R8A8_UNORM, 4); SetF(FMT_BGRA8, VK_FORMAT_B8G8R8A8_UNORM, 4);
  // Block-compressed: BC1-3 0x42..0x48 (unorm, srgb), BC4/BC5 0x49..0x4C, BC7 0x4D/0x4E, BC6H 0x4F (signed) / 0x50 (unsigned).
  // Values taken from the game's nn::gfx -> NVN format conversion (0x924d60); BC6H/BC4/BC5 verified there too.
  SetBc(0x42, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8); SetBc(0x46, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, true);
  SetBc(0x43, VK_FORMAT_BC2_UNORM_BLOCK, 16); SetBc(0x47, VK_FORMAT_BC2_SRGB_BLOCK, 16, true);
  SetBc(0x44, VK_FORMAT_BC3_UNORM_BLOCK, 16); SetBc(0x48, VK_FORMAT_BC3_SRGB_BLOCK, 16, true);
  SetBc(0x49, VK_FORMAT_BC4_UNORM_BLOCK, 8); SetBc(0x4A, VK_FORMAT_BC4_SNORM_BLOCK, 8);
  SetBc(0x4B, VK_FORMAT_BC5_UNORM_BLOCK, 16); SetBc(0x4C, VK_FORMAT_BC5_SNORM_BLOCK, 16);
  SetBc(0x4D, VK_FORMAT_BC7_UNORM_BLOCK, 16); SetBc(0x4E, VK_FORMAT_BC7_SRGB_BLOCK, 16, true);
  SetBc(0x4F, VK_FORMAT_BC6H_SFLOAT_BLOCK, 16); SetBc(0x50, VK_FORMAT_BC6H_UFLOAT_BLOCK, 16);
  // ASTC LDR 0x79..0x94: 14 block sizes, unorm then sRGB. [ASSUMPTION: order not confirmed, the game uses no ASTC.]
  for (int i = 0; i < 14; i++) {
    g_fmts[0x79 + i] = {kAstcUnorm[i], 16, VK_IMAGE_ASPECT_COLOR_BIT, 0, kAstcDims[i][0], kAstcDims[i][1], kAstcDims[i][0], kAstcDims[i][1], false};
    g_fmts[0x87 + i] = {AstcSrgb(kAstcUnorm[i]), 16, VK_IMAGE_ASPECT_COLOR_BIT, 0, kAstcDims[i][0], kAstcDims[i][1], kAstcDims[i][0], kAstcDims[i][1], true};
  }
  SetF(FMT_BGRX8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, 4); SetF(FMT_BGRA8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, 4);
}
const FormatInfo* LookupFormat(int f) {
  InitFormats();
  if (f <= 0 || f >= (int)(sizeof g_fmts / sizeof *g_fmts) || g_fmts[f].vk == VK_FORMAT_UNDEFINED) return nullptr;
  return &g_fmts[f];
}
static void PatchFormatFallbacks(VkPhysicalDevice pd) {
  InitFormats();
  VkFormatProperties p{};
  vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D24_UNORM_S8_UINT, &p);
  if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    g_fmts[FMT_DEPTH24_STENCIL8].vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
    g_fmts[FMT_DEPTH24_STENCIL8].bytes = 8;
    Log("D24S8 unsupported, using D32S8");
  }
  // ASTC LDR: native when the device has it, else CPU decode and transcode to BC7 (RGBA8 if BC7 is missing or
  // NVN_ASTC_FALLBACK=rgba8). The decode happens in UploadStaticTexture.
  vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &p);
  if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
    VkFormatProperties pb{};
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_BC7_UNORM_BLOCK, &pb);
    const char* e = getenv("NVN_ASTC_FALLBACK");
    bool bc7 = (pb.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !(e && !strcmp(e, "rgba8"));
    for (int i = 0x79; i <= 0x94; i++) {
      FormatInfo& f = g_fmts[i];
      if (bc7) { f.vk = f.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK; f.bytes = 16; f.bw = f.bh = 4; }
      else { f.vk = f.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; f.bytes = 4; f.bw = f.bh = 1; }
    }
    Log("ASTC not supported by the device: decoding on the CPU, sampled as %s", bc7 ? "BC7 (transcoded)" : "RGBA8");
  }
  vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_X8_D24_UNORM_PACK32, &p);
  if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    g_fmts[FMT_DEPTH24].vk = VK_FORMAT_D32_SFLOAT;
    Log("D24 unsupported, using D32F");
  }
}

// ---- Host ----------------------------------------------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
  if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    g_valErrors++;
    fprintf(stderr, "[vk][ERROR] %s\n", d->pMessage);
  } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    g_valWarnings++;
    fprintf(stderr, "[vk][warn] %s\n", d->pMessage);
  }
  return VK_FALSE;
}

uint32_t Host::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want &&
        !(memProps.memoryTypes[i].propertyFlags & avoid))
      return i;
  return UINT32_MAX;
}

bool Host::Init(bool wantValidation) {
  uint32_t n = 0;
  bool haveLayer = false, haveValFeat = false;
  if (wantValidation) {
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> lp(n);
    vkEnumerateInstanceLayerProperties(&n, lp.data());
    for (auto& l : lp) if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) haveLayer = true;
    if (haveLayer) {
      vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &n, nullptr);
      std::vector<VkExtensionProperties> ep(n);
      vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &n, ep.data());
      for (auto& e : ep) if (!strcmp(e.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) haveValFeat = true;
    }
    if (!haveLayer) LogError("validation requested but VK_LAYER_KHRONOS_validation not found (set VK_LAYER_PATH)");
  }
  validation = haveLayer;

  std::vector<const char*> iext = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  if (validation) {
    iext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (haveValFeat) iext.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
  }
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
  VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
  vf.enabledValidationFeatureCount = 1;
  vf.pEnabledValidationFeatures = enables;
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "NVN-Vulkan layer";
  ai.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai;
  ici.enabledExtensionCount = (uint32_t)iext.size();
  ici.ppEnabledExtensionNames = iext.data();
  if (validation) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    if (haveValFeat) ici.pNext = &vf;
  }
  if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { LogError("vkCreateInstance failed"); return false; }

  if (validation) {
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = DebugCb;
    if (fn) fn(instance, &mi, nullptr, &messenger);
  }

  // Pick a physical device: Vulkan 1.3, graphics+compute queue family with Win32 presentation support.
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  vkEnumeratePhysicalDevices(instance, &n, pds.data());
  int bestScore = -1;
  for (auto pd : pds) {
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd, &pr);
    if (pr.apiVersion < VK_API_VERSION_1_3) continue;
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn; i++) {
      const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      if ((qf[i].queueFlags & need) == need && vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, i)) {
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) { bestScore = score; phys = pd; family = i; props = pr; }
        break;
      }
    }
  }
  if (!phys) { LogError("no Vulkan 1.3 GPU with graphics+present queue"); return false; }
  vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
  Log("GPU: %s", props.deviceName);

  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  vkGetPhysicalDeviceFeatures2(phys, &f2);
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.synchronization2 = VK_TRUE;
  f13.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.timelineSemaphore = VK_TRUE;
  f12.pNext = &f13;
  VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  f11.shaderDrawParameters = VK_TRUE;   // translated shaders use DrawParameters (base vertex / base instance builtins)
  f13.pNext = &f11;
  {
    VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12};
    vkGetPhysicalDeviceFeatures2(phys, &q2);
    f12.samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge;
    f12.hostQueryReset = q12.hostQueryReset;   // vkResetQueryPool in the replay (queries are reset on the host while recording)
    hostQueryReset = q12.hostQueryReset == VK_TRUE;
  }
  VkPhysicalDeviceFeatures2 en{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  // Every core feature the GPU has: translated shaders declare Float64/Int64/ImageGatherExtended/ClipDistance...,
  // and cbuffers are declared as vec4[4096], so robustBufferAccess is needed for short UBO ranges.
  en.features = f2.features;
  features = f2.features;
  en.pNext = &f12;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &en;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dext;
  if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) { LogError("vkCreateDevice failed"); return false; }
  vkGetDeviceQueue(device, family, 0, &queue);
  VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  VKCHK(vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache));
  {
    const char* e = getenv("NVN_CACHE_DIR");
    if (e && *e) cacheDir = e;
    else {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(nullptr, path, MAX_PATH);
      std::string d = path;
      size_t k = d.find_last_of("\\/");
      cacheDir = (k == std::string::npos ? std::string(".") : d.substr(0, k)) + "\\nvn_cache";
    }
    CreateDirectoryA(cacheDir.c_str(), nullptr);
    LoadPipelineCache(this);
  }
  PatchFormatFallbacks(phys);
  return true;
}

void Host::Shutdown() {
  if (device) vkDeviceWaitIdle(device);
  if (device) DestroyHostExtras(this);
  if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
  if (device) vkDestroyDevice(device, nullptr);
  if (messenger) {
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) fn(instance, messenger, nullptr);
  }
  if (instance) vkDestroyInstance(instance, nullptr);
  device = VK_NULL_HANDLE; instance = VK_NULL_HANDLE; messenger = VK_NULL_HANDLE; pipelineCache = VK_NULL_HANDLE;
}

// ---- images ------------------------------------------------------------------------------------------------
uint32_t TextureLayerCount(const TextureBuilder& b) {
  switch (b.target) {
    case TT_1D_ARRAY: case TT_2D_ARRAY: return (uint32_t)std::max(b.depth, 1);
    case TT_CUBEMAP: return 6;
    case TT_CUBEMAP_ARRAY: return (b.depth % 6 == 0 && b.depth > 0) ? (uint32_t)b.depth : (uint32_t)std::max(b.depth, 1) * 6;  // [ASSUMPTION] unused by the game
    default: return 1;
  }
}
bool TextureImpl::EnsureImage() {
  if (image) return true;
  if (!fi) fi = LookupFormat(b.format);
  if (!fi) { LogError("texture format 0x%x not mapped", b.format); return false; }
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(host->phys, fi->vk, &fp);
  const auto feat = fp.optimalTilingFeatures;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (fi->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
    if (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  } else if (feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
    usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  }
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // views may reinterpret the format (TextureView SetFormat)
  ci.format = fi->vk;
  ci.mipLevels = (uint32_t)std::max(b.levels, 1);
  ci.arrayLayers = 1;
  ci.samples = b.samples > 1 ? (VkSampleCountFlagBits)b.samples : VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  switch (b.target) {
    case TT_2D: case TT_RECTANGLE: case TT_2D_MULTISAMPLE:
      ci.imageType = VK_IMAGE_TYPE_2D; ci.extent = {HW(), HH(), 1}; break;
    case TT_1D:
      ci.imageType = VK_IMAGE_TYPE_1D; ci.extent = {(uint32_t)b.width, 1, 1}; break;
    case TT_1D_ARRAY:
      ci.imageType = VK_IMAGE_TYPE_1D; ci.extent = {(uint32_t)b.width, 1, 1};
      ci.arrayLayers = (uint32_t)std::max(b.depth, 1); break;
    case TT_2D_ARRAY:
      ci.imageType = VK_IMAGE_TYPE_2D; ci.extent = {HW(), HH(), 1};
      ci.arrayLayers = (uint32_t)std::max(b.depth, 1);
      if (ci.arrayLayers >= 6 && ci.extent.width == ci.extent.height) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;   // a shader may declare a cube
      break;
    case TT_CUBEMAP: case TT_CUBEMAP_ARRAY:
      ci.imageType = VK_IMAGE_TYPE_2D; ci.extent = {HW(), HH(), 1};
      ci.arrayLayers = TextureLayerCount(b); ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case TT_3D:
      ci.imageType = VK_IMAGE_TYPE_3D; ci.extent = {(uint32_t)b.width, (uint32_t)b.height, (uint32_t)std::max(b.depth, 1)}; break;
    default:
      LogError("texture target %d not supported yet", b.target); return false;
  }
  // clamp mip chain to what the extent allows
  uint32_t maxDim = std::max(ci.extent.width, std::max(ci.extent.height, ci.extent.depth)), maxMips = 1;
  while (maxDim >>= 1) maxMips++;
  ci.mipLevels = std::min(ci.mipLevels, maxMips);
  if (vkCreateImage(host->device, &ci, nullptr, &image) != VK_SUCCESS) { LogError("vkCreateImage failed"); image = VK_NULL_HANDLE; return false; }
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(host->device, image, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = host->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(host->device, &mai, nullptr, &memory) != VK_SUCCESS) {
    LogError("image memory allocation failed"); vkDestroyImage(host->device, image, nullptr); image = VK_NULL_HANDLE; return false;
  }
  vkBindImageMemory(host->device, image, memory, 0);
  layout = VK_IMAGE_LAYOUT_UNDEFINED;
  subLayout.clear();
  imgLevels = ci.mipLevels; imgLayers = ci.arrayLayers; ci_flags = ci.flags;
  return true;
}

static VkImageViewType ViewTypeFor(int target) {
  switch (target) {
    case TT_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TT_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case TT_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TT_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TT_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TT_CUBEMAP_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
  }
}

SubRange TextureImpl::RangeOf(const TexView* v) const {
  TexView z{};
  if (!v) v = &z;
  const uint32_t lvlTotal = std::max(imgLevels, 1u), layers = std::max(imgLayers, 1u);
  uint32_t lb = (v->flags & 1) ? v->lvlBase : 0, ln = (v->flags & 1) ? v->lvlNum : lvlTotal;
  if (lb >= lvlTotal) lb = lvlTotal - 1;
  ln = std::max(1u, std::min(ln, lvlTotal - lb));
  uint32_t yb = (v->flags & 2) ? v->layerBase : 0, yn = (v->flags & 2) ? v->layerNum : layers;
  if (yb >= layers) yb = layers - 1;
  yn = std::max(1u, std::min(yn, layers - yb));
  return {lb, ln, yb, yn};
}

SubRange RtRange(const TextureImpl* t, const TexView* v) {
  SubRange r = t->RangeOf(v);
  r.ln = 1;                                              // an attachment is a single mip level
  if (!v || !(v->flags & 2)) r.yn = 1;                   // no layer selection: layer 0 only (as before)
  return r;
}

// TexView flags: 1 levels, 2 layers, 4 format, 8 target, 0x10 depth/stencil mode.
// Views are cached per (type, format, aspect, level window, layer window, sampled). shaderType (the translator's SamplerType:
// low byte 1 = 1D, 3 = 2D, 4 = 3D, 5 = cube, bit 8 = array) makes the view match what the shader declares.
VkImageView TextureImpl::View(const TexView* v, bool sampled, int shaderType, const SubRange* range) {
  if (!EnsureImage()) return VK_NULL_HANDLE;
  TexView z{};
  if (!v) v = &z;
  const FormatInfo* vf = fi;
  if (v->flags & 4) { if (const FormatInfo* o = LookupFormat(v->format)) vf = o; }
  const int target = (v->flags & 8) ? v->target : b.target;
  SubRange r = range ? *range : RangeOf(v);
  VkImageViewType vt = ViewTypeFor(target);
  if (shaderType) {
    const int dim = shaderType & 0xff;
    const bool arr = (shaderType & 0x100) != 0;
    const bool img1D = b.target == TT_1D || b.target == TT_1D_ARRAY, img3D = b.target == TT_3D;
    const bool cubeOk = (ci_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
    if (img1D) {
      if (dim != 1) return VK_NULL_HANDLE;
      vt = arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
      if (!arr) r.yn = 1;
    } else if (img3D) {
      if (dim != 4) return VK_NULL_HANDLE;
      vt = VK_IMAGE_VIEW_TYPE_3D; r.yb = 0; r.yn = 1;
    } else {
      switch (dim) {
        case 3:   // 2D
          vt = arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
          if (!arr) r.yn = 1;
          break;
        case 5:   // cube
          if (!cubeOk) return VK_NULL_HANDLE;
          if (!arr) {
            vt = VK_IMAGE_VIEW_TYPE_CUBE;
            if (r.yb + 6 > imgLayers) return VK_NULL_HANDLE;
            r.yn = 6;
          } else {
            vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            r.yn = std::max(6u, r.yn / 6 * 6);
            if (r.yb + r.yn > imgLayers) return VK_NULL_HANDLE;
          }
          break;
        default: return VK_NULL_HANDLE;   // buffer textures, 1D on a 2D image, 3D on a 2D image
      }
    }
  } else if (vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_3D) {
    if (vt != VK_IMAGE_VIEW_TYPE_3D) r.yn = 1;
  }
  VkImageAspectFlags asp = vf->aspect;
  if (sampled && (asp & VK_IMAGE_ASPECT_DEPTH_BIT) && (asp & VK_IMAGE_ASPECT_STENCIL_BIT))
    asp = ((v->flags & 0x10) && v->dsMode == 1) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
  char key[64];
  snprintf(key, sizeof key, "%d.%d.%u.%u.%u.%u.%u.%u.%d", (int)vt, (int)vf->vk, (unsigned)asp, r.lb, r.ln, r.yb, r.yn, sampled ? 1 : 0, 0);
  auto it = views.find(key);
  if (it != views.end()) return it->second;
  VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ci.image = image;
  ci.viewType = vt;
  ci.format = vf->vk;
  ci.subresourceRange = {asp, r.lb, r.ln, r.yb, r.yn};
  VkImageView iv = VK_NULL_HANDLE;
  if (vkCreateImageView(host->device, &ci, nullptr, &iv) != VK_SUCCESS) { LogError("vkCreateImageView failed"); return VK_NULL_HANDLE; }
  views[key] = iv;
  return iv;
}
VkImageView TextureImpl::RenderView(const TexView* v, SubRange* out) {
  if (!EnsureImage()) return VK_NULL_HANDLE;
  SubRange r = RtRange(this, v);
  if (out) *out = r;
  const VkImageViewType vt = r.yn > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  char key[64];
  snprintf(key, sizeof key, "R%d.%d.%u.%u.%u.%u", (int)vt, (int)fi->vk, (unsigned)fi->aspect, r.lb, r.yb, r.yn);
  auto it = views.find(key);
  if (it != views.end()) return it->second;
  VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ci.image = image; ci.viewType = vt; ci.format = fi->vk;
  ci.subresourceRange = {fi->aspect, r.lb, 1, r.yb, r.yn};
  VkImageView iv = VK_NULL_HANDLE;
  if (vkCreateImageView(host->device, &ci, nullptr, &iv) != VK_SUCCESS) { LogError("vkCreateImageView (attachment) failed"); return VK_NULL_HANDLE; }
  views[key] = iv;
  return iv;
}

void DestroyTexture(TextureImpl* t) {
  if (!t) return;
  if (t->host && t->host->device) {
    for (auto& kv : t->views) vkDestroyImageView(t->host->device, kv.second, nullptr);
    if (t->image) vkDestroyImage(t->host->device, t->image, nullptr);
    if (t->memory) vkFreeMemory(t->host->device, t->memory, nullptr);
  }
  delete t;
}

// ---- memory pools --------------------------------------------------------------------------------------------
// Mirrored backing: a VkBuffer over one VkDeviceMemory block. CPU-accessible pools use HOST_VISIBLE|COHERENT
// memory (preferring DEVICE_LOCAL, i.e. resizable BAR) and are updated by memcpy from the guest pointer at
// MemoryPoolFlushMappedRange. GPU-only pools use DEVICE_LOCAL. Backing is created lazily (first flush / first
// buffer bind) because the game also creates many pools that only hold texture storage.
bool EnsureBacking(PoolImpl* p) {
  if (p->buffer) return true;
  Host* h = p->host;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = p->size;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (vkCreateBuffer(h->device, &bi, nullptr, &p->buffer) != VK_SUCCESS) { p->buffer = VK_NULL_HANDLE; return false; }
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(h->device, p->buffer, &mr);
  uint32_t type = UINT32_MAX;
  if (p->Cpu()) {
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    type = h->FindMemoryType(mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = h->FindMemoryType(mr.memoryTypeBits, hv);
  } else {
    type = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = type;
  if (type == UINT32_MAX || vkAllocateMemory(h->device, &mai, nullptr, &p->memory) != VK_SUCCESS) {
    LogError("pool backing allocation failed (%llu bytes)", (unsigned long long)p->size);
    vkDestroyBuffer(h->device, p->buffer, nullptr); p->buffer = VK_NULL_HANDLE; p->memory = VK_NULL_HANDLE; return false;
  }
  vkBindBufferMemory(h->device, p->buffer, p->memory, 0);
  if (h->memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    void* m = nullptr;
    vkMapMemory(h->device, p->memory, 0, VK_WHOLE_SIZE, 0, &m);
    p->mapped = static_cast<uint8_t*>(m);
    if (p->mapped && p->storage) memcpy(p->mapped, p->storage, (size_t)p->size);
  }
  return true;
}
// ---- synthetic GPU address space ----------------------------------------------------------------------------------
// The guest only treats an NVNbufferAddress as an opaque 64-bit value (BufferGetAddress + offset arithmetic).
// Every memory pool gets a 1 MiB-aligned range so an address maps back to (pool, offset) at replay.
void Host::RegisterPool(PoolImpl* p) {
  std::lock_guard<std::mutex> lk(vaMutex);
  p->va = nextVa;
  nextVa += (p->size + 0xFFFFF) & ~0xFFFFFull;
  nextVa += 0x100000;   // guard gap
  vaMap[p->va] = p;
}
void Host::UnregisterPool(PoolImpl* p) {
  std::lock_guard<std::mutex> lk(vaMutex);
  vaMap.erase(p->va);
}
bool Host::ResolveAddress(uint64_t addr, PoolImpl** pool, uint64_t* off) {
  std::lock_guard<std::mutex> lk(vaMutex);
  auto it = vaMap.upper_bound(addr);
  if (it == vaMap.begin()) return false;
  --it;
  PoolImpl* p = it->second;
  if (addr - p->va >= p->size) return false;
  *pool = p; *off = addr - p->va;
  return true;
}
void SyncPoolRange(PoolImpl* p, uint64_t off, uint64_t size) {
  if (!p || !p->Cpu() || !p->storage || off >= p->size) return;
  if (!p->buffer && !EnsureBacking(p)) return;
  if (!p->mapped) return;
  size = std::min<uint64_t>(size, p->size - off);
  uint64_t pos = off;
  const uint64_t end = off + size;
  std::lock_guard<std::mutex> lk(p->gpuMutex);
  // copy [off, end) except the ranges the GPU owns (their CPU bytes are stale: the GPU wrote the real data)
  while (pos < end) {
    uint64_t next = end;
    bool skip = false;
    for (auto& r : p->gpuRanges) {
      if (r.second <= pos || r.first >= end) continue;
      if (r.first <= pos) { pos = std::max(pos, std::min(r.second, end)); skip = true; break; }
      next = std::min(next, r.first);
    }
    if (skip) continue;
    memcpy(p->mapped + pos, static_cast<uint8_t*>(p->storage) + pos, (size_t)(next - pos));
    pos = next;
  }
}
void MarkGpuWritten(PoolImpl* p, uint64_t off, uint64_t size) {
  if (!p || !size || off >= p->size) return;
  const uint64_t end = std::min<uint64_t>(off + size, p->size);
  std::lock_guard<std::mutex> lk(p->gpuMutex);
  for (auto& r : p->gpuRanges) if (r.first <= off && end <= r.second) return;   // already covered
  p->gpuRanges.push_back({off, end});
  // merge overlapping ranges so the list stays short
  std::sort(p->gpuRanges.begin(), p->gpuRanges.end());
  size_t w = 0;
  for (size_t i = 1; i < p->gpuRanges.size(); i++) {
    if (p->gpuRanges[i].first <= p->gpuRanges[w].second) p->gpuRanges[w].second = std::max(p->gpuRanges[w].second, p->gpuRanges[i].second);
    else p->gpuRanges[++w] = p->gpuRanges[i];
  }
  p->gpuRanges.resize(w + 1);
}
void DestroyPool(PoolImpl* p) {
  if (!p) return;
  if (p->host) p->host->UnregisterPool(p);
  if (p->host && p->host->device) {
    if (p->mapped) vkUnmapMemory(p->host->device, p->memory);
    if (p->buffer) vkDestroyBuffer(p->host->device, p->buffer, nullptr);
    if (p->memory) vkFreeMemory(p->host->device, p->memory, nullptr);
  }
  delete p;
}

// ---- queue ---------------------------------------------------------------------------------------------------
bool InitQueue(QueueImpl* q) {
  Host* h = q->host;
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cp.queueFamilyIndex = h->family;
  if (vkCreateCommandPool(h->device, &cp, nullptr, &q->pool) != VK_SUCCESS) return false;
  VkSemaphoreTypeCreateInfo tc{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  tc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  tc.initialValue = 0;
  VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  sc.pNext = &tc;
  return vkCreateSemaphore(h->device, &sc, nullptr, &q->timeline) == VK_SUCCESS;
}
void DestroyQueue(QueueImpl* q) {
  Host* h = q->host;
  QueueFinishImpl(q);
  for (auto cb : q->freeCbs) vkFreeCommandBuffers(h->device, q->pool, 1, &cb);
  for (auto dp : q->freePools) vkDestroyDescriptorPool(h->device, dp, nullptr);
  if (q->pool) vkDestroyCommandPool(h->device, q->pool, nullptr);
  if (q->timeline) vkDestroySemaphore(h->device, q->timeline, nullptr);
  delete q;
}
VkCommandBuffer QueueImpl::AcquireCb() {
  if (!freeCbs.empty()) {
    VkCommandBuffer cb = freeCbs.back(); freeCbs.pop_back();
    vkResetCommandBuffer(cb, 0);
    return cb;
  }
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(host->device, &ai, &cb);
  return cb;
}
void QueueImpl::Reap(bool wait_all) {
  if (wait_all && value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1; wi.pSemaphores = &timeline; wi.pValues = &value;
    vkWaitSemaphores(host->device, &wi, UINT64_MAX);
  }
  uint64_t done = 0;
  vkGetSemaphoreCounterValue(host->device, timeline, &done);
  size_t keep = 0;
  for (auto& r : inflight) {
    if (r.value <= done) {
      freeCbs.push_back(r.cb); delete r.list;
      for (auto dp : r.pools) { vkResetDescriptorPool(host->device, dp, 0); freePools.push_back(dp); }
      for (auto& sb : r.scratch) { vkDestroyBuffer(host->device, sb.buf, nullptr); vkFreeMemory(host->device, sb.mem, nullptr); }
    }
    else inflight[keep++] = r;
  }
  inflight.resize(keep);
}

// ---- replay ---------------------------------------------------------------------------------------------------
static VkAccessFlags2 AccessFor(VkImageLayout l) {
  switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED: case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return 0;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_2_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_2_TRANSFER_READ_BIT;
    default: return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  }
}

uint8_t* NewScratch(ReplayCtx& cx, uint64_t size, VkBuffer* out, VkBufferUsageFlags extra) {
  Host* h = cx.h;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = std::max<uint64_t>(size, 16); bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra;
  ScratchBuf sb;
  if (vkCreateBuffer(h->device, &bi, nullptr, &sb.buf) != VK_SUCCESS) return nullptr;
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(h->device, sb.buf, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  void* map = nullptr;
  if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(h->device, &mai, nullptr, &sb.mem) != VK_SUCCESS) { vkDestroyBuffer(h->device, sb.buf, nullptr); return nullptr; }
  (void)fl;
  vkBindBufferMemory(h->device, sb.buf, sb.mem, 0);
  vkMapMemory(h->device, sb.mem, 0, VK_WHOLE_SIZE, 0, &map);
  cx.scratch->push_back(sb);
  *out = sb.buf;
  return (uint8_t*)map;
}

bool InLayout(const TextureImpl* t, VkImageLayout want, const SubRange& r) {
  if (t->subLayout.empty()) return t->layout == want;
  for (uint32_t y = r.yb; y < r.yb + r.yn && y < t->imgLayers; y++)
    for (uint32_t l = r.lb; l < r.lb + r.ln && l < t->imgLevels; l++)
      if (t->subLayout[y * t->imgLevels + l] != want) return false;
  return true;
}

// Moves the subresources of `r` to layout `to`. Layouts are tracked per (level, layer) once a transition covers part of the
// image: the game renders into one mip level while sampling the level above it. A whole-image transition from a uniform layout is
// a single barrier. Transitions to GENERAL always emit a barrier (execution + memory dependency, as before).
void TransitionRange(VkCommandBuffer cb, TextureImpl* t, VkImageLayout to, const SubRange& r0) {
  if (!t->image) return;
  const uint32_t L = std::max(t->imgLevels, 1u), Y = std::max(t->imgLayers, 1u);
  SubRange r = r0;
  r.lb = std::min(r.lb, L - 1); r.ln = std::max(1u, std::min(r.ln, L - r.lb));
  r.yb = std::min(r.yb, Y - 1); r.yn = std::max(1u, std::min(r.yn, Y - r.yb));
  std::vector<VkImageMemoryBarrier2> bars;
  auto mk = [&](VkImageLayout from, uint32_t lb, uint32_t ln, uint32_t yb, uint32_t yn) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = AccessFor(from);
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = AccessFor(to);
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = {t->fi->aspect, lb, ln, yb, yn};
    bars.push_back(b);
  };
  const bool whole = r.lb == 0 && r.ln == L && r.yb == 0 && r.yn == Y;
  if (t->subLayout.empty()) {
    if (t->layout == to && to != VK_IMAGE_LAYOUT_GENERAL) return;
    if (whole) {
      mk(t->layout, 0, L, 0, Y);
      t->layout = to;
    } else {
      t->subLayout.assign((size_t)L * Y, t->layout);
    }
  }
  if (!t->subLayout.empty()) {
    for (uint32_t y = r.yb; y < r.yb + r.yn; y++) {
      uint32_t l = r.lb;
      while (l < r.lb + r.ln) {
        const VkImageLayout from = t->subLayout[y * L + l];
        uint32_t e = l + 1;
        while (e < r.lb + r.ln && t->subLayout[y * L + e] == from) e++;
        if (from != to || to == VK_IMAGE_LAYOUT_GENERAL) {
          if (!bars.empty() && bars.back().oldLayout == from && bars.back().subresourceRange.baseMipLevel == l &&
              bars.back().subresourceRange.levelCount == e - l &&
              bars.back().subresourceRange.baseArrayLayer + bars.back().subresourceRange.layerCount == y)
            bars.back().subresourceRange.layerCount++;   // same level run on the previous layer: one barrier for both
          else mk(from, l, e - l, y, 1);
        }
        for (uint32_t k = l; k < e; k++) t->subLayout[y * L + k] = to;
        l = e;
      }
    }
    bool uniform = true;
    for (VkImageLayout x : t->subLayout) if (x != to) { uniform = false; break; }
    if (uniform) { t->layout = to; t->subLayout.clear(); }
  }
  if (bars.empty()) return;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = (uint32_t)bars.size();
  di.pImageMemoryBarriers = bars.data();
  vkCmdPipelineBarrier2(cb, &di);
}

void Transition(VkCommandBuffer cb, TextureImpl* t, VkImageLayout to) {
  if (!t->image) return;
  TransitionRange(cb, t, to, {0, std::max(t->imgLevels, 1u), 0, std::max(t->imgLayers, 1u)});
}

// scale of the current render targets (viewport/scissor are in guest pixels)
static float RtScale(const ReplayCtx& cx) {
  for (int i = 0; i < 9; i++) if (cx.rt[i]) return cx.rt[i]->scale;
  return 1.0f;
}

static void Replay(ReplayCtx& cx, const CmdList& list) {
  Host* h = cx.h;
  VkCommandBuffer cb = cx.cb;
  TextureImpl** rt = cx.rt;
  g_replaySeq++;
  ReplayBegin(cx);
  static bool warnedMask = false;
  for (const Cmd& c : list.cmds) {
    switch (c.op) {
      case Op::SetRenderTargets:
        EndRendering(cx);
        for (int k = 0; k < 9; k++) {
          rt[k] = c.tex[k];
          cx.rtHasView[k] = false; cx.rtView[k] = TexView{};
          const size_t o = (size_t)k * (1 + sizeof(TexView));
          if (rt[k] && c.blob.size() >= o + 1 + sizeof(TexView) && c.blob[o]) { memcpy(&cx.rtView[k], &c.blob[o + 1], sizeof(TexView)); cx.rtHasView[k] = true; }
        }
        if (DebugDrawLogActive()) DebugDrawLog("SetRenderTargets rt0=%p(%dx%d f%x) depth=%p\n", (void*)rt[0], rt[0] ? rt[0]->b.width : 0, rt[0] ? rt[0]->b.height : 0, rt[0] ? rt[0]->b.format : 0, (void*)rt[8]);
        break;
      case Op::ClearColor: {
        EndRendering(cx);
        TextureImpl* t = (c.i[0] >= 0 && c.i[0] < 8) ? rt[c.i[0]] : nullptr;
        if (!t || !t->EnsureImage()) { Log("ClearColor with no usable render target %d", c.i[0]); break; }
        if ((c.i[1] & 0xF) != 0xF) { if (c.i[1] & 0xF) DoMaskedClear(cx, t, cx.rtHasView[c.i[0]] ? &cx.rtView[c.i[0]] : nullptr, c.i[1] & 0xF, c.f); break; }
        t->uploaded = true;
        const SubRange rr = RtRange(t, cx.rtHasView[c.i[0]] ? &cx.rtView[c.i[0]] : nullptr);   // only the bound mip level / layers
        TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rr);
        VkClearColorValue v{};
        if (t->fi->kind == 0) memcpy(v.float32, c.f, sizeof v.float32);
        else if (t->fi->kind == 1) memcpy(v.uint32, c.f, sizeof v.uint32);   // NVN passes raw ints for integer formats
        else memcpy(v.int32, c.f, sizeof v.int32);
        VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, rr.lb, 1, rr.yb, rr.yn};
        vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
        break;
      }
      case Op::ClearDepthStencil: {
        EndRendering(cx);
        TextureImpl* t = rt[8];
        if (!t || !t->EnsureImage()) { Log("ClearDepthStencil with no depth target"); break; }
        VkImageAspectFlags asp = 0;
        if (c.i[0] && (t->fi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;     // depthMask
        if (c.i[2] && (t->fi->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;  // stencilMask
        if (!asp) break;
        t->uploaded = true;
        const SubRange rr = RtRange(t, cx.rtHasView[8] ? &cx.rtView[8] : nullptr);
        TransitionRange(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rr);
        VkClearDepthStencilValue v{c.f[0], (uint32_t)c.i[1]};
        VkImageSubresourceRange rg{asp, rr.lb, 1, rr.yb, rr.yn};
        vkCmdClearDepthStencilImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
        break;
      }
      case Op::SetViewports: {
        cx.vpSet = true;
        if (c.i[0] > 0 && h->props.limits.maxViewports <= 1) break;
        float x = c.f[0], y = c.f[1], w = c.f[2], ht = c.f[3];
        if (DebugDrawLogActive()) DebugDrawLog("SetViewport %d: %g %g %g %g (rt0 %p)\n", c.i[0], x, y, w, ht, (void*)rt[0]);
        if (w <= 0 || ht <= 0) break;
        VkViewport vp = MakeNvnViewport(h->originMode, x, y, w, ht, RtScale(cx));
        vkCmdSetViewport(cb, (uint32_t)c.i[0], 1, &vp);
        break;
      }
      case Op::SetScissors: {
        cx.scSet = true;
        if (c.i[0] > 0 && h->props.limits.maxViewports <= 1) break;
        int x = c.i[1], y = c.i[2], w = c.i[3], ht = c.i[4];
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { ht += y; y = 0; }
        if (w < 0) w = 0;
        if (ht < 0) ht = 0;
        { const float sc = RtScale(cx); if (sc != 1.0f) { x = (int)std::floor(x * sc); y = (int)std::floor(y * sc); w = (int)std::ceil(w * sc); ht = (int)std::ceil(ht * sc); } }
        VkRect2D r{{x, y}, {(uint32_t)w, (uint32_t)ht}};
        vkCmdSetScissor(cb, (uint32_t)c.i[0], 1, &r);
        break;
      }
      case Op::Barrier: {
        EndRendering(cx);
        // Conservative: every NVN barrier bit maps to a full memory barrier (refined in phase 1).
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cb, &di);
        break;
      }
      default:
        ReplayDrawOp(cx, c);
        break;
    }
  }
  ReplayEnd(cx);
}

void SubmitLists(QueueImpl* q, int n, CmdList* const* lists) {
  Host* h = q->host;
  std::lock_guard<std::mutex> lk(h->queueMutex);
  q->Reap(false);
  std::vector<VkCommandBufferSubmitInfo> infos;
  std::vector<Retired> pending;
  std::vector<Readback> readbacksOut;
  for (int k = 0; k < n; k++) {
    CmdList* l = lists[k];
    if (!l) continue;
    if (g_hooks.on_submit) g_hooks.on_submit(g_hooks.user, k, (int)l->cmds.size());
    VkCommandBuffer cb = q->AcquireCb();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    ReplayCtx cx;
    cx.h = h; cx.cb = cb; cx.q = q;
    std::vector<VkDescriptorPool> pools;
    cx.pools = &pools;
    std::vector<ScratchBuf> scratch;
    cx.scratch = &scratch;
    std::vector<Readback> rbs;
    cx.readbacks = &rbs;
    Replay(cx, *l);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cb;
    infos.push_back(ci);
    pending.push_back({0, cb, l, std::move(pools), std::move(scratch)});
    for (auto& rb : rbs) readbacksOut.push_back(rb);
  }
  if (infos.empty()) return;
  uint64_t v = ++q->value;
  VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  sig.semaphore = q->timeline; sig.value = v; sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = (uint32_t)infos.size(); si.pCommandBufferInfos = infos.data();
  si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sig;
  VKCHK(vkQueueSubmit2(h->queue, 1, &si, VK_NULL_HANDLE));
  for (auto& r : pending) { r.value = v; q->inflight.push_back(r); }
  if (!readbacksOut.empty()) {
    std::lock_guard<std::mutex> rl(h->rbMutex);
    for (auto& rb : readbacksOut) { rb.sem = q->timeline; rb.value = v; h->readbacks.push_back(rb); }
  }
}

void QueueFence(QueueImpl* q, SyncImpl* s) {
  Host* h = q->host;
  std::lock_guard<std::mutex> lk(h->queueMutex);
  uint64_t v = ++q->value;
  VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  sig.semaphore = q->timeline; sig.value = v; sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &sig;
  VKCHK(vkQueueSubmit2(h->queue, 1, &si, VK_NULL_HANDLE));
  s->sem = q->timeline; s->value = v; s->armed = true;
}

void QueueFinishImpl(QueueImpl* q) {
  { std::lock_guard<std::mutex> lk(q->host->queueMutex); q->Reap(true); }
  FlushReadbacks(q->host, false);
}

static SyncWaitResult WaitSyncRaw(const SyncImpl* s, uint64_t timeoutNs);
SyncWaitResult WaitSync(const SyncImpl* s, uint64_t timeoutNs) {
  SyncWaitResult r = WaitSyncRaw(s, timeoutNs);
  if (s->armed) FlushReadbacks(s->host, false);
  return r;
}
static SyncWaitResult WaitSyncRaw(const SyncImpl* s, uint64_t timeoutNs) {
  if (!s->armed) return SYNC_ALREADY_SIGNALED;
  uint64_t cur = 0;
  vkGetSemaphoreCounterValue(s->host->device, s->sem, &cur);
  if (cur >= s->value) return SYNC_ALREADY_SIGNALED;
  VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = 1; wi.pSemaphores = &s->sem; wi.pValues = &s->value;
  VkResult r = vkWaitSemaphores(s->host->device, &wi, timeoutNs);
  if (r == VK_SUCCESS) return SYNC_CONDITION_SATISFIED;
  if (r == VK_TIMEOUT) return SYNC_TIMEOUT_EXPIRED;
  return SYNC_FAILED;
}

// ---- window / swapchain ---------------------------------------------------------------------------------------
// The NVN-visible window textures are ordinary offscreen images (scene resolution). Present acquires a real
// swapchain image and blits (scales, letterboxes) the chosen slot into it. That decouples scene resolution from
// output resolution (native 4K / DLAA / DLSS later) and makes the present blit the single composition point.
bool WindowImpl::Recreate() {
  Host* h = host;
  vkDeviceWaitIdle(h->device);
  VkSurfaceCapabilitiesKHR caps{};
  VKCHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(h->phys, surface, &caps));
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) ext = {tex.empty() ? 1280u : (uint32_t)tex[0]->b.width, tex.empty() ? 720u : (uint32_t)tex[0]->b.height};
  if (ext.width == 0 || ext.height == 0) return false;  // minimized

  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(h->phys, surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> fm(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(h->phys, surface, &n, fm.data());
  const bool srgbSrc = !tex.empty() && tex[0]->fi && (tex[0]->fi->vk == VK_FORMAT_R8G8B8A8_SRGB || tex[0]->fi->vk == VK_FORMAT_B8G8R8A8_SRGB);
  const VkFormat want[2] = {srgbSrc ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM,
                            srgbSrc ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM};
  VkSurfaceFormatKHR pick = fm.empty() ? VkSurfaceFormatKHR{} : fm[0];
  bool found = false;
  for (int k = 0; k < 2 && !found; k++)
    for (auto& f : fm)
      if (f.format == want[k] && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = f; found = true; break; }

  vkGetPhysicalDeviceSurfacePresentModesKHR(h->phys, surface, &n, nullptr);
  std::vector<VkPresentModeKHR> pm(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(h->phys, surface, &n, pm.data());
  auto has = [&](VkPresentModeKHR m) { return std::find(pm.begin(), pm.end(), m) != pm.end(); };
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (presentInterval <= 0) mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                   : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;

  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { LogError("surface lacks TRANSFER_DST"); return false; }
  uint32_t minImages = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount) minImages = std::min(minImages, caps.maxImageCount);

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface;
  ci.minImageCount = minImages;
  ci.imageFormat = pick.format;
  ci.imageColorSpace = pick.colorSpace;
  ci.imageExtent = ext;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = mode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = swapchain;
  VkSwapchainKHR ns = VK_NULL_HANDLE;
  if (vkCreateSwapchainKHR(h->device, &ci, nullptr, &ns) != VK_SUCCESS) { LogError("vkCreateSwapchainKHR failed"); return false; }
  for (auto s : renderDone) vkDestroySemaphore(h->device, s, nullptr);
  renderDone.clear();
  if (swapchain) vkDestroySwapchainKHR(h->device, swapchain, nullptr);
  swapchain = ns;
  scFormat = pick.format;
  scExtent = ext;
  vkGetSwapchainImagesKHR(h->device, swapchain, &n, nullptr);
  scImages.resize(n);
  vkGetSwapchainImagesKHR(h->device, swapchain, &n, scImages.data());
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < n; i++) {
    VkSemaphore s; vkCreateSemaphore(h->device, &sci, nullptr, &s); renderDone.push_back(s);
  }
  while (acquireSems.size() < n + 1) {
    VkSemaphore s; vkCreateSemaphore(h->device, &sci, nullptr, &s);
    acquireSems.push_back(s); acquireValue.push_back(0);
  }
  needRecreate = false;
  Log("swapchain %ux%u images=%u fmt=%d mode=%d", ext.width, ext.height, n, (int)scFormat, (int)mode);
  return true;
}

void DestroyWindow(WindowImpl* w) {
  Host* h = w->host;
  if (h && h->device) {
    vkDeviceWaitIdle(h->device);
    for (auto s : w->renderDone) vkDestroySemaphore(h->device, s, nullptr);
    for (auto s : w->acquireSems) vkDestroySemaphore(h->device, s, nullptr);
    if (w->swapchain) vkDestroySwapchainKHR(h->device, w->swapchain, nullptr);
    if (w->surface) vkDestroySurfaceKHR(h->instance, w->surface, nullptr);
  }
  if (h) h->windows.erase(std::remove(h->windows.begin(), h->windows.end(), w), h->windows.end());
  delete w;
}

// Frame limiter for present intervals >= 1: absolute schedule on the steady clock, high-resolution timer for the
// bulk of the wait and a short spin for the rest. Resyncs when the game falls more than one period behind.
static void Pace(WindowImpl* w) {
  if (g_paceHz <= 0 || w->presentInterval <= 0) { w->nextPresentNs = 0; return; }
  using namespace std::chrono;
  const int64_t period = (int64_t)(1e9 * w->presentInterval / g_paceHz);
  const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  if (w->nextPresentNs == 0 || now - w->nextPresentNs > period) w->nextPresentNs = now;   // first frame or fell behind
  w->nextPresentNs += period;
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  int64_t left = w->nextPresentNs - now;
  if (left > 2000000 && timer) {   // sleep all but ~1.5 ms
    LARGE_INTEGER due; due.QuadPart = -(left - 1500000) / 100;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
  }
  while (duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() < w->nextPresentNs) YieldProcessor();
}

bool PresentWindow(QueueImpl* q, WindowImpl* w, int slot) {
  Host* h = q->host;
  if (slot < 0 || slot >= (int)w->tex.size()) { LogError("present of invalid slot %d", slot); return false; }
  TextureImpl* src = w->tex[slot];
  if (!src->EnsureImage()) return false;
  Pace(w);
  std::lock_guard<std::mutex> lk(h->queueMutex);
  q->Reap(false);
  g_presentCount++;
  if (g_hooks.on_present) g_hooks.on_present(g_hooks.user, slot, src->b.width, src->b.height);

  uint32_t idx = 0;
  // The acquire semaphore must have no pending wait: gate reuse on the timeline value of the submit that waited it.
  if (w->needRecreate && !w->Recreate()) return false;
  const uint32_t ak = w->acquireNext++ % (uint32_t)w->acquireSems.size();
  if (w->acquireValue[ak]) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1; wi.pSemaphores = &q->timeline; wi.pValues = &w->acquireValue[ak];
    vkWaitSemaphores(h->device, &wi, UINT64_MAX);
  }
  for (int attempt = 0; attempt < 3; attempt++) {
    if (w->needRecreate && !w->Recreate()) return false;
    VkResult r = vkAcquireNextImageKHR(h->device, w->swapchain, UINT64_MAX, w->acquireSems[ak], VK_NULL_HANDLE, &idx);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { if (r == VK_SUBOPTIMAL_KHR) w->needRecreate = true; break; }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { w->needRecreate = true; if (attempt == 2) return false; continue; }
    LogError("vkAcquireNextImageKHR failed: %d", (int)r);
    return false;
  }

  VkCommandBuffer cb = q->AcquireCb();
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);

  VkImage dst = w->scImages[idx];
  auto imgBarrier = [&](VkImage img, VkImageAspectFlags asp, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = AccessFor(from); b.dstAccessMask = AccessFor(to);
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
  };
  Transition(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  imgBarrier(dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue black{};
  VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
  imgBarrier(dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  // source rectangle: crop if set, else the whole texture
  int sx = 0, sy = 0, sw = src->b.width, sh = src->b.height;
  if (w->crop[2] > 0 && w->crop[3] > 0) { sx = w->crop[0]; sy = w->crop[1]; sw = w->crop[2]; sh = w->crop[3]; }
  sx = std::clamp(sx, 0, src->b.width - 1); sy = std::clamp(sy, 0, src->b.height - 1);
  sw = std::clamp(sw, 1, src->b.width - sx); sh = std::clamp(sh, 1, src->b.height - sy);
  // destination rectangle: aspect-preserving fit
  const double sa = (double)sw / sh, da = (double)w->scExtent.width / w->scExtent.height;
  int dw = w->scExtent.width, dh = w->scExtent.height;
  if (sa > da) dh = std::max(1, (int)(dw / sa + 0.5)); else dw = std::max(1, (int)(dh * sa + 0.5));
  int dx = ((int)w->scExtent.width - dw) / 2, dy = ((int)w->scExtent.height - dh) / 2;
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[0] = {sx, sy, 0}; blit.srcOffsets[1] = {sx + sw, sy + sh, 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = {dx, dy, 0}; blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
  const bool same = (sw == dw && sh == dh);
  vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                 same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  imgBarrier(dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  vkEndCommandBuffer(cb);

  uint64_t v = ++q->value;
  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = w->acquireSems[ak]; wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkSemaphoreSubmitInfo sig[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
  sig[0].semaphore = q->timeline; sig[0].value = v; sig[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  sig[1].semaphore = w->renderDone[idx]; sig[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = cb;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.waitSemaphoreInfoCount = 1; si.pWaitSemaphoreInfos = &wait;
  si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi;
  si.signalSemaphoreInfoCount = 2; si.pSignalSemaphoreInfos = sig;
  VKCHK(vkQueueSubmit2(h->queue, 1, &si, VK_NULL_HANDLE));
  q->inflight.push_back({v, cb, nullptr});

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &w->renderDone[idx];
  pi.swapchainCount = 1; pi.pSwapchains = &w->swapchain; pi.pImageIndices = &idx;
  VkResult pr = vkQueuePresentKHR(h->queue, &pi);
  if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) w->needRecreate = true;
  else if (pr < 0) LogError("vkQueuePresentKHR failed: %d", (int)pr);
  w->acquireValue[ak] = v;
  w->slotSem[slot] = q->timeline;
  w->slotValue[slot] = v;
  return true;
}

}  // namespace nvn
