// SPDX-License-Identifier: MIT
// Public surface of the NVN->Vulkan layer. The lifted game receives its NVN function table through
// nvnBootstrapLoader (the only real NVN import, [analysis/runtime/nvn_usage.md]).
#pragma once
#include <cstdint>

#if defined(_WIN32)
#define NVN_HOST_EXPORT extern "C" __declspec(dllexport)
#else
#define NVN_HOST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// The real NVN loader entry: returns the function for an exact NVN name ("nvnDeviceGetProcAddress",
// "nvnDeviceInitialize", ...) or nullptr. nvnDeviceGetProcAddress(device, name) returns the same table.
NVN_HOST_EXPORT void (*nvnBootstrapLoader(const char* name))(void);

// ---- Host-side controls (not part of NVN) -------------------------------------------------------------
// Must be called before nvnDeviceInitialize. Validation layers are also enabled by env NVN_VK_VALIDATION=1.
NVN_HOST_EXPORT void nvnHostSetValidation(int enable);
NVN_HOST_EXPORT uint32_t nvnHostValidationErrors(void);
NVN_HOST_EXPORT uint32_t nvnHostValidationWarnings(void);
// Frame limiter: rate in Hz of NVN present interval 1 (the Switch panel is 60 Hz). Interval n paces to n/hz s.
// 0 disables the limiter (presentation then follows the display's FIFO vsync only). Default 60.
NVN_HOST_EXPORT void nvnHostSetFramePacing(double hz);
// Render-target resolution scale (off by default: 1.0 matches the original). Applies to plain render targets
// (TextureFlags COMPRESSIBLE, not DISPLAY/LINEAR, 2D or 2D array, single sample) created after the call, so set it before the
// game creates its textures. The guest still sees the original size from every NVN getter; viewports, scissors and clear
// regions are scaled at replay; copies between targets of different scale become blits. Exempt targets keep scale 1:
//   - by signature: nvnHostAddScaleExemption(width, height, nvnFormat, depthOnly), -1 = any (a built-in exemption covers
//     1920-wide depth targets, the Evaluation depth target; nvnHostClearScaleExemptions removes it),
//   - by tag: call nvnHostSetNextTextureTag(tag) right before the allocation and nvnHostAddScaleExemptionTag(tag) once.
// nvnHostGetTextureHostSize reports the Vulkan image size of a guest texture (0 if it is not a texture).
NVN_HOST_EXPORT void nvnHostSetRenderScale(float scale);
NVN_HOST_EXPORT float nvnHostGetRenderScale(void);
NVN_HOST_EXPORT void nvnHostAddScaleExemption(int width, int height, int nvnFormat, int depthOnly);
NVN_HOST_EXPORT void nvnHostClearScaleExemptions(void);
NVN_HOST_EXPORT void nvnHostSetNextTextureTag(uint32_t tag);
NVN_HOST_EXPORT void nvnHostAddScaleExemptionTag(uint32_t tag);
NVN_HOST_EXPORT int nvnHostGetTextureHostSize(const void* guestTexture, int* width, int* height);
// Anisotropic filtering override applied at SamplerInitialize to mipmapped, non-comparison samplers (the game uses 1x).
// 0 = off (default, original behavior); e.g. 16 forces up to 16x. Must be set before samplers are created.
NVN_HOST_EXPORT void nvnHostSetAnisotropyOverride(float maxAnisotropy);
// Number of guest entry points registered in the getProcAddress table.
NVN_HOST_EXPORT int nvnHostProcCount(void);
// Name of the idx-th registered entry point (0 <= idx < count), for tests and coverage reports.
NVN_HOST_EXPORT const char* nvnHostProcName(int idx);

// Present/render-pass interception hooks (see docs/nvn-vulkan-design.md, "Interception hooks").
// All callbacks run on the thread that issued the NVN call. Observers in this milestone.
struct NvnHostHooks {
  void* user;
  // Called when QueuePresentTexture starts: guest slot, source size in pixels. Returns nothing yet; later
  // milestones let the hook replace the present blit (scene->output scaler, DLSS, UI composite).
  void (*on_present)(void* user, int slot, int src_w, int src_h);
  // Called for each submitted command list before replay: number of recorded commands.
  void (*on_submit)(void* user, int list_index, int command_count);
};
NVN_HOST_EXPORT void nvnHostSetHooks(const NvnHostHooks* hooks);
