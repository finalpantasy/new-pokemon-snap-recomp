// SPDX-License-Identifier: MIT
// AMD FSR 3.1 upscaling through the FidelityFX API (FidelityFX SDK v1.1.4 = FSR 3.1.4, MIT): the signed prebuilt
// amd_fidelityfx_vk.dll is loaded at run time from the folder of the exe (local copy in tools\amd\, git-ignored, copied by
// host/build_snap.py). The API headers (MIT) are used from tools\amd\ when present (SNAP_HAVE_FFX_API, set by the build);
// without them this backend reports itself unavailable and the front end falls back.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

#include "upscaler.h"

#if defined(SNAP_HAVE_FFX_API)
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

namespace snap_upscale::fsr {
namespace {
struct State {
  HMODULE lib = nullptr;
  PfnFfxCreateContext create = nullptr; PfnFfxDestroyContext destroy = nullptr; PfnFfxDispatch dispatch = nullptr;
  PfnFfxQuery query = nullptr;
  bool tried = false, loaded = false, failed = false;
  DeviceCtx dc;
  ffxContext ctx = nullptr;
  uint32_t ow = 0, oh = 0, mw = 0, mh = 0, flags = 0;
  uint64_t evals = 0;
} F;

void OnMessage(uint32_t type, const wchar_t* message) { Logf("fsr: %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message); }

FfxApiResource Res(const Image& m, uint32_t w, uint32_t h, uint32_t usage, uint32_t state) {
  FfxApiResourceDescription d{};
  d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  d.format = ffxApiGetSurfaceFormatVK(m.format);
  d.width = w; d.height = h; d.depth = 1; d.mipCount = 1;
  d.flags = FFX_API_RESOURCE_FLAGS_NONE;
  d.usage = usage;
  return ffxApiGetResourceVK((void*)m.image, d, state);
}
// The prebuilt backend resolves its device functions through the vkDeviceProcAddr we hand it and calls some without a
// null check (it crashed calling address 0 on context creation): KHR aliases of core functions return null unless their
// extension is enabled, and the debug-utils calls need VK_EXT_debug_utils. Map aliases to the core name, debug labels to
// no-ops, and log anything else that stays null.
VKAPI_ATTR void VKAPI_CALL NoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL NoEndLabel(VkCommandBuffer) {}
VKAPI_ATTR VkResult VKAPI_CALL NoName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ProcAddr(VkDevice dev, const char* name) {
  PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
  if (f) return f;
  const size_t n = strlen(name);
  if (n > 3 && (!strcmp(name + n - 3, "KHR") || !strcmp(name + n - 3, "EXT"))) {
    char core[128] = {};
    if (n - 3 < sizeof core) { memcpy(core, name, n - 3); f = vkGetDeviceProcAddr(dev, core); }
    if (f) return f;
  }
  if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)NoLabel;
  if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)NoEndLabel;
  if (!strcmp(name, "vkSetDebugUtilsObjectNameEXT")) return (PFN_vkVoidFunction)NoName;
  Logf("fsr: device function %s not available", name);
  return nullptr;
}
}  // namespace

bool Load() {
  if (F.tried) return F.loaded;
  F.tried = true;
  char path[MAX_PATH + 32] = {};
  DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
  while (len && path[len - 1] != '\\') len--;
  strcpy(path + len, "amd_fidelityfx_vk.dll");
  F.lib = LoadLibraryA(path);
  if (!F.lib) { Logf("fsr: cannot load %s: FSR unavailable", path); return false; }
  F.create = (PfnFfxCreateContext)(void*)GetProcAddress(F.lib, "ffxCreateContext");
  F.destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(F.lib, "ffxDestroyContext");
  F.dispatch = (PfnFfxDispatch)(void*)GetProcAddress(F.lib, "ffxDispatch");
  F.query = (PfnFfxQuery)(void*)GetProcAddress(F.lib, "ffxQuery");
  if (!F.create || !F.destroy || !F.dispatch) { Logf("fsr: %s lacks the FidelityFX API entry points: FSR unavailable", path); return false; }
  Logf("fsr: %s (FidelityFX API)", path);
  return F.loaded = true;
}

bool Init(const DeviceCtx& c) {
  if (!F.loaded) return false;
  F.dc = c;
  return true;
}

bool Evaluate(const Eval& e) {
  if (!F.loaded || F.failed) return false;
  const uint32_t flags = (e.hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) : 0u) | (e.invDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) | (e.infFar ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0u);
  bool reset = e.reset;
  if (F.ctx && (F.ow != e.ow || F.oh != e.oh || F.flags != flags || e.w > F.mw || e.h > F.mh)) {
    vkDeviceWaitIdle(F.dc.device);
    F.destroy(&F.ctx, nullptr);
    F.ctx = nullptr;
  }
  if (!F.ctx) {
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = F.dc.device; be.vkPhysicalDevice = F.dc.phys; be.vkDeviceProcAddr = ProcAddr;
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    cd.flags = flags;
    cd.maxRenderSize = {e.w, e.h};
    cd.maxUpscaleSize = {e.ow, e.oh};
    cd.fpMessage = OnMessage;
    ffxReturnCode_t r = F.create(&F.ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK || !F.ctx) { Logf("fsr: context creation failed (%u): FSR off for this run", r); F.failed = true; F.ctx = nullptr; return false; }
    F.ow = e.ow; F.oh = e.oh; F.mw = e.w; F.mh = e.h; F.flags = flags;
    reset = true;
    Logf("fsr: FSR 3.1 context %ux%u -> %ux%u, %s colour", e.w, e.h, e.ow, e.oh, e.hdr ? "HDR" : "LDR");
  }
  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = (void*)e.cb;
  d.color = Res(e.color, e.w, e.h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = Res(e.depth, e.w, e.h, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = Res(e.mv, e.w, e.h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = Res(e.out, e.ow, e.oh, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {e.jx, e.jy};
  d.motionVectorScale = {1.0f, 1.0f};   // vectors are in render pixels
  d.renderSize = {e.w, e.h};
  d.upscaleSize = {e.ow, e.oh};
  d.enableSharpening = false;
  d.sharpness = 0.0f;
  d.frameTimeDelta = e.dtMs;
  d.preExposure = 1.0f;
  d.reset = reset;
  d.cameraNear = e.nearZ; d.cameraFar = e.farZ;
  d.cameraFovAngleVertical = e.fovY;
  d.viewSpaceToMetersFactor = 1.0f;
  d.flags = 0;
  // FSR's Vulkan backend reads depth as a sampled image in SHADER_READ_ONLY_OPTIMAL; the caller hands it over in
  // DEPTH_STENCIL_READ_ONLY_OPTIMAL (validation VUID-vkCmdDispatch-imageLayout-00344), so switch around the dispatch.
  auto depthLayout = [&](VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = e.depth.image;
    const bool st = e.depth.format == VK_FORMAT_D24_UNORM_S8_UINT || e.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT || e.depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
    b.subresourceRange = {VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (st ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(e.cb, &di);
  };
  depthLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ffxReturnCode_t r = F.dispatch(&F.ctx, &d.header);
  depthLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  if (r != FFX_API_RETURN_OK) { Logf("fsr: dispatch failed (%u): FSR off for this run", r); F.failed = true; return false; }
  F.evals++;
  return true;
}

void Release() {
  if (F.ctx) F.destroy(&F.ctx, nullptr);
  F.ctx = nullptr;
}

}  // namespace snap_upscale::fsr

#else   // no FidelityFX API headers at build time

namespace snap_upscale::fsr {
bool Load() { Logf("fsr: built without the FidelityFX API headers (tools\\amd\\): FSR unavailable"); return false; }
bool Init(const DeviceCtx&) { return false; }
bool Evaluate(const Eval&) { return false; }
void Release() {}
}  // namespace snap_upscale::fsr

#endif
