// SPDX-License-Identifier: MIT
// The NVN layer's side of the temporal upscalers (runtime/nvn/upscale/upscale.cpp). Every call is a cheap no-op while
// SNAP_UPSCALER is off. Call sites in nvn_vk.cpp / nvn_draw.cpp / nvn_entry.cpp / nvn_gfx.cpp are marked "[upscale]".
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace nvn {
struct Host;
struct ReplayCtx;
struct TextureImpl;
struct CmdList;

// nvn_entry.cpp ApplyHostEnvPolicy: picks the scene and output scale. Returns true when an upscaler is wanted; then *sceneScale is
// the render scale of the game's scene targets and *outScale the scale of the window (DISPLAY) textures.
bool UpscaleApplyPolicy(float* sceneScale, float* outScale);
// nvn_entry.cpp TextureInitialize: sees every new texture with the guest call chain of its creation (return addresses relative
// to main) and its scale tag (the Evaluation depth target is tagged 'EVDP').
void UpscaleTagTexture(TextureImpl* t, const uint64_t* chainRel, int n, uint32_t tag);
void UpscaleForgetTexture(TextureImpl* t);   // nvn_vk.cpp DestroyTexture

// nvn_vk.cpp Host::Init / Host::Shutdown
void UpscaleInstanceExtensions(std::vector<const char*>& ext);
void UpscaleDeviceExtensions(Host* h, std::vector<const char*>& ext, void** chainHead, VkPhysicalDeviceVulkan11Features* f11,
                             VkPhysicalDeviceVulkan12Features* f12, VkPhysicalDeviceVulkan13Features* f13);
void UpscaleDeviceCreated(Host* h);
void UpscaleDestroy(Host* h);   // device idle

// nvn_vk.cpp Replay: after SetRenderTargets (no rendering scope open). May record the upscaler. Returns true when the jitter
// for the new targets differs from the one the current viewports were emitted with (re-emit them).
bool UpscaleOnSetRenderTargets(ReplayCtx& cx);
// The jitter in host pixels for the bound targets (0, 0 when none); added to every viewport 0..3 emitted for them.
void UpscaleViewportJitter(const ReplayCtx& cx, float* jx, float* jy);
// [fx] jitter of this frame's scene draws in host pixels (nvn_fx.cpp), 0, 0 when the upscaler is off or the frame is held.
void UpscaleSceneJitter(float* jx, float* jy);
// nvn_draw.cpp ResolveHandle: the texture to sample instead of t (the upscaled scene for the game's final pass), or nullptr.
TextureImpl* UpscaleSubstitute(const ReplayCtx& cx, TextureImpl* t);
// nvn_vk.cpp SubmitLists: before the lists of one submit are replayed (photo frames are found by looking ahead in them).
void UpscaleBeforeSubmit(int n, CmdList* const* lists);
// nvn_draw.cpp DoDraw: a draw's vertex-stage uniform buffers (guest bytes, nullptr when unbound), for the camera.
void UpscaleOnDraw(ReplayCtx& cx, const uint8_t* const* vsUbo, const uint64_t* vsRange, int n);
// nvn_vk.cpp PresentWindowOnce: once per present.
void UpscaleOnPresent();
}  // namespace nvn
