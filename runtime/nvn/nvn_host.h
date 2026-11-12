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
//   - by signature: nvnHostAddScaleExemption(width, height, nvnFormat, depthOnly), -1 = any,
//   - by tag: call nvnHostSetNextTextureTag(tag) right before the allocation and nvnHostAddScaleExemptionTag(tag) once,
//   - by guest call site (built in): the Evaluation depth target, created from main+0x5c3d38 ("Evaluation DepthStencil"), is
//     found in the guest frame-pointer chain of its TextureInitialize call (nvnHostSetGuestFrame, set by the generated bridge).
//     nvnHostClearScaleExemptions also turns this off.
// nvnHostGetTextureHostSize reports the Vulkan image size of a guest texture (0 if it is not a texture).
NVN_HOST_EXPORT void nvnHostSetRenderScale(float scale);
NVN_HOST_EXPORT float nvnHostGetRenderScale(void);
NVN_HOST_EXPORT void nvnHostAddScaleExemption(int width, int height, int nvnFormat, int depthOnly);
NVN_HOST_EXPORT void nvnHostClearScaleExemptions(void);
NVN_HOST_EXPORT void nvnHostSetNextTextureTag(uint32_t tag);
NVN_HOST_EXPORT void nvnHostAddScaleExemptionTag(uint32_t tag);
NVN_HOST_EXPORT int nvnHostGetTextureHostSize(const void* guestTexture, int* width, int* height);
// Guest frame pointer (x29) and link register (x30) of the NVN call about to run; the bridge sets it for TextureInitialize only.
NVN_HOST_EXPORT void nvnHostSetGuestFrame(uint64_t fp, uint64_t lr);
// Anisotropic filtering override applied at SamplerInitialize to mipmapped, non-comparison samplers (the game uses 1x).
// Default 16 (host default since 2026-11-12); 0 = off (original 1x). Must be set before samplers are created. Env SNAP_ANISO=0..16.
NVN_HOST_EXPORT void nvnHostSetAnisotropyOverride(float maxAnisotropy);
// Anisotropy applies only to mipmapped samplers with linear min and mag filters and no depth compare; point, nearest and shadow-compare
// samplers keep the game's value. Clamped to the device limit. Read-back: nvnHostGetAnisotropyOverride (0 = off).
NVN_HOST_EXPORT float nvnHostGetAnisotropyOverride(void);
// LOD bias added to the game's bias on mipmapped, non-compare samplers (set before samplers are created). Auto = -log2(output /
// render resolution), the upscaler bias: 0 at native render scale (a 2x target already samples one mip finer), negative only when
// the scene renders below the output size. Env: SNAP_LOD_BIAS=<float>|auto; unset = auto. At native 4K, -1 is sharper still in
// stills (analysis/mods/compare/lod_crop_s2.png) but samples two mips finer than 1080p and may shimmer in motion; it is opt-in.
NVN_HOST_EXPORT void nvnHostSetLodBias(float bias);
NVN_HOST_EXPORT void nvnHostSetLodBiasAuto(int on);
// Side of the 1024x1024 depth shadow maps (1024 = original, 2048, 4096; set before the game creates them). Independent of the render scale.
NVN_HOST_EXPORT void nvnHostSetShadowResolution(int side);
NVN_HOST_EXPORT int nvnHostGetShadowResolution(void);
// Whether window (DISPLAY) textures follow the render scale (default 1). Needed for a real 4K frame: the game composes UI into them.
NVN_HOST_EXPORT void nvnHostSetScaleDisplay(int on);
// Environment read once at DeviceInitialize: SNAP_RENDER_SCALE, SNAP_ANISO (1..16), SNAP_LOD_BIAS, SNAP_SHADOW_RES. The output
// (swapchain) resolution is the window size: SNAP_WINDOW_W / SNAP_WINDOW_H (host/window_opts.cpp).
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
