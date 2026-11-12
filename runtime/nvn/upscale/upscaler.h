// SPDX-License-Identifier: MIT
// Temporal upscalers for the NVN->Vulkan layer: NVIDIA DLSS / DLAA (the driver's NGX core), AMD FSR 3.1 (FidelityFX API,
// amd_fidelityfx_vk.dll) and Intel XeSS (libxess.dll). One front end (upscale.cpp) finds the scene in the replayed frame,
// jitters its draws, makes camera motion vectors and runs the chosen backend; the backends (upscale_dlss.cpp,
// upscale_fsr.cpp, upscale_xess.cpp) only bring their library up and record one evaluation.
//
// Settings (environment, read once at device creation):
//   SNAP_UPSCALER=off|dlss|dlaa|fsr|xess        dlaa = DLSS at the output size (same as dlss with quality native)
//   SNAP_UPSCALE_QUALITY=native|quality|balanced|performance
//       native: scene = output (DLAA / FSR native AA / XeSS AA); quality 1.5x, balanced ~1.724x, performance 2x per axis.
//   Output size is the window (SNAP_WINDOW_W/H); the game's 1920x1080 targets are scaled to the scene size and the window
//   textures (where the game draws its UI) to the output size.
//   A backend that cannot run falls back to the next in DLSS -> FSR -> XeSS order, then to plain render scale (logged).
//   Test knobs: SNAP_UPSCALE_JITTER=0 (no jitter), SNAP_UPSCALE_MV=0 (zero motion), SNAP_UPSCALE_LOG=1 (per-frame log).
//
// Photo safety: the jitter is applied only to draws into the scene depth target, never to the Evaluation depth target or
// photo passes; frames that run the Evaluation or a photo capture are rendered unjittered and are not upscaled (upscale.cpp,
// "photo frames").
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace snap_upscale {

enum Backend : int { BK_OFF = 0, BK_DLSS = 1, BK_FSR = 2, BK_XESS = 3 };

// One evaluation, as the backends get it. Images are in the layouts named here and are left in them.
//   color, depth, mv: SHADER_READ_ONLY_OPTIMAL (depth: DEPTH_STENCIL_READ_ONLY_OPTIMAL); out: GENERAL.
struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
};
struct Eval {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  Image color, depth, mv, out;
  uint32_t w = 0, h = 0, ow = 0, oh = 0;   // render (scene) size, output size
  float jx = 0, jy = 0;                    // jitter in render pixels, x right, y down (the shift of the picture)
  uint32_t phases = 16;                    // jitter sequence length
  bool reset = false;
  bool hdr = true;                         // linear scene light (auto exposure) vs tonemapped 0..1
  bool invDepth = false;                   // depth 1 = near (reversed z)
  bool infFar = false;                     // no far plane (infinite projection)
  float dtMs = 16.67f;
  float nearZ = 0.1f, farZ = 1000.0f, fovY = 0.6f;
};

struct DeviceCtx {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
};

// ---- front end (upscale.cpp), called by the NVN layer -------------------------------------------------------------
// Device creation (nvn_vk.cpp Host::Init): instance extensions, then device extensions and features for every backend
// the setting may fall back to. `chainHead` receives extra feature structures (owned by this module) to link into pNext.
void AddInstanceExtensions(std::vector<const char*>& ext);
void AddDeviceExtensions(VkInstance inst, VkPhysicalDevice pd, std::vector<const char*>& ext, void** chainHead,
                         VkPhysicalDeviceVulkan11Features* f11, VkPhysicalDeviceVulkan12Features* f12,
                         VkPhysicalDeviceVulkan13Features* f13);
void DeviceCreated(const DeviceCtx& c);
void Shutdown();   // device idle

// Setting state (nvn_entry.cpp ApplyHostEnvPolicy): the scene scale and output scale relative to the game's 1920x1080.
bool Wanted();                 // SNAP_UPSCALER names a backend
float SceneScale(float outputScale);   // render scale for the game's scene targets (output scale times the quality ratio)
const char* Name();            // active backend name ("DLSS Quality", "off", ...)
int ActiveBackend();

// ---- backends ------------------------------------------------------------------------------------------------------
// Each: Load (library, before/after the instance as noted), extensions, Init after the device, Evaluate, Release.
namespace dlss {
bool LoadAfterInstance();   // the driver's NGX core beside NVIDIA's Vulkan ICD
void DeviceExtensions(VkPhysicalDevice pd, std::vector<const char*>& ext);
bool Init(const DeviceCtx& c);
bool Evaluate(const Eval& e);
void Release();
}  // namespace dlss
namespace fsr {
bool Load();   // amd_fidelityfx_vk.dll beside the exe
bool Init(const DeviceCtx& c);
bool Evaluate(const Eval& e);
void Release();
}  // namespace fsr
namespace xess {
bool Load();   // libxess.dll beside the exe
void InstanceExtensions(std::vector<const char*>& ext);
bool DeviceExtensions(VkInstance inst, VkPhysicalDevice pd, std::vector<const char*>& ext, bool* needMutableDesc);
bool Init(const DeviceCtx& c);
bool Evaluate(const Eval& e);
void Release();
}  // namespace xess

void Logf(const char* fmt, ...);   // "[upscale] ..." on stderr

}  // namespace snap_upscale
