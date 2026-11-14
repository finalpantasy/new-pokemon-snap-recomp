// SPDX-License-Identifier: MIT
// "Fx" plumbing for host-side screen-space effects (project code, ours). See nvn_fx.cpp and AGENTS.md "Fx plumbing".
//   SNAP_FX=0 forces every effect off; an effect runs only when its own switch is on (SNAP_CONTACT_SHADOWS, SNAP_SSR, ...).
//   SNAP_FX_AB=<P>: effects run only while (present count / P) is odd (same-view A/B in one run).   SNAP_FX_SPLIT=1: the left half of the frame
//   keeps the game's own picture.   SNAP_FX_LOG=1: log frame phases, photo holds and effect runs.
//
// Two hook slots for effects (an effect registers one FxEffect from a static initializer in its own file):
//   preTonemap  - recorded when the game binds the window texture after its scene passes, BEFORE the upscaler reads the scene colour
//                 (UpscaleOnSetRenderTargets / Resolve runs right after, so the upscaler, DLSS/FSR/XeSS and the tonemap all see the result). The scene
//                 colour is linear HDR (RGBA16F) at render resolution (1920x1080 x SNAP_RENDER_SCALE / upscaler scene scale), jittered by the upscaler's
//                 viewport jitter (FxJitter). No rendering scope is open; the hook records its own passes.
//   postTonemap - recorded after the draw that ran one of the game's tonemap programs (like AaAfterDraw): the target is the LDR window texture.
// Neither slot runs on photo frames (FxIsPhotoFrame: Evaluation / 360x240 photo passes and the presents a photo reads) or inside an occlusion query scope.
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace nvn {
struct ReplayCtx;
struct TextureImpl;
struct CmdList;

// The scene camera of this frame (PerViewCBuffer c3, read at the first scene draw; see upscale.cpp for the layout).
struct FxCam {
  bool valid = false;
  double vp[4][4];    // row-major: clip = vp * (world, 1)
  double ivp[4][4];   // inverse of vp: (ndc.xy, depth, 1) -> homogeneous world; w = 1 / clip.w
  double eye[3], fwd[3];
  float nearZ = 0.1f, farZ = 1e7f, fovY = 0.6f;
  bool invDepth = true, infFar = true;   // reversed infinite z (the course): ndc z 1 = near, 0 = infinity
};
// The dynamic shadow caster seen this present or the one before (slot 9 of the ShadowCast draws; the light is only bound there).
struct FxCaster {
  bool valid = false;
  float m[16];        // the 64-byte caster matrix as the game stores it (column-major, clip = M * world)
  float axis[3];      // unit world direction of the matrix' depth row (rows 0-2 of column z: m[2], m[6], m[10]); toward or away from the light, see FxLightToward
  int present = -100; // present count it was seen at
  uint32_t mapSize = 0;
};

struct FxEffect {
  const char* name;
  bool (*enabled)();                                   // the effect's own SNAP_* switch (cheap, called per frame)
  void (*preTonemap)(ReplayCtx& cx);                   // may be null
  void (*postTonemap)(ReplayCtx& cx, const char* fragKey12);   // may be null
};
void FxRegister(const FxEffect* e);

// ---- called by the layer (one line each) -----------------------------------------------------------------------------
bool FxOnSetRenderTargets(ReplayCtx& cx);   // nvn_vk.cpp Replay, SetRenderTargets, before UpscaleOnSetRenderTargets. true: commands were recorded (re-emit viewports / scissors)
void FxOnDraw(ReplayCtx& cx, const uint8_t* const* vsUbo, const uint64_t* vsRange, const uint8_t* slot9, uint64_t slot9Range);   // nvn_draw.cpp DoDraw: camera + caster capture only
void FxAfterDraw(ReplayCtx& cx, const char* fragKey12);   // nvn_draw.cpp DoDraw, next to AaAfterDraw
void FxBeforeSubmit(int n, CmdList* const* lists);        // nvn_vk.cpp SubmitLists: photo frames by look-ahead
void FxOnPresent();                                       // nvn_vk.cpp present

// ---- for effects -----------------------------------------------------------------------------------------------------
bool FxActive();                              // SNAP_FX is not 0 and at least one registered effect is enabled
const FxCam* FxCamera();                      // null until a sane camera was read this frame
TextureImpl* FxSceneDepth();                  // the 1920x1080 (x scale) DEPTH32F scene target of this frame, null outside a scene frame
TextureImpl* FxSceneColor();                  // the RGBA16F scene colour drawn with it
bool FxIsPhotoFrame();                        // this present must stay native (photo / Evaluation frame, or the hold after one)
void FxJitter(float* jx, float* jy);          // scene viewport jitter in render pixels (0, 0 without a jittering upscaler)
const FxCaster* FxCasterNow();                // null when no dynamic shadow pass was recorded this or the previous present (static-only course, night)
bool FxAbSkip();                              // SNAP_FX_AB says "off" this present
uint32_t FxSplitX(uint32_t w);                // SNAP_FX_SPLIT: first pixel column effects may touch (0 when off)
bool FxWritable(const TextureImpl* t);        // false (and one log line) for depth formats, the Evaluation depth target and 360x240 photo targets: effects never write those
bool FxIsTonemapKey(const char* fragKey12);

// Vulkan helpers shared by effects. Pipelines are created once (callers cache them) with dynamic viewport/scissor and dynamic rendering.
struct Host;
VkShaderModule FxMakeModule(Host* h, const uint32_t* words, size_t bytes);
VkShaderModule FxFullscreenVs(Host* h);   // shared vertex shader: one oversized triangle, no vertex input
// Full-screen triangle pipeline writing RGBA of `fmt`. multiply: colour = dst * src.rgb (alpha untouched), the form contact shadows use.
VkPipeline FxMakeFullscreenPipeline(Host* h, VkPipelineLayout pl, VkShaderModule fs, VkFormat fmt, bool multiply);
VkDescriptorSet FxAllocSet(ReplayCtx& cx, VkDescriptorSetLayout layout);   // from the command buffer's pools (recycled when it retires)
// Draws one full-screen triangle into `target` (whole image, LOAD), scissored to x >= FxSplitX. The target must satisfy FxWritable.
// Moves the target to COLOR_ATTACHMENT, records a memory barrier after the pass. `pc` goes to the VERTEX|FRAGMENT push-constant range.
void FxDrawFullscreen(ReplayCtx& cx, TextureImpl* target, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet set, const void* pc, uint32_t pcBytes);
// Scene depth for sampling in a later pass: switches to DEPTH_STENCIL_READ_ONLY and returns the sampled view; FxDepthDone restores the previous layout
// (the next CopyDepth and the upscaler rely on the tracked layout). Never write scene depth.
VkImageView FxDepthRead(ReplayCtx& cx, TextureImpl* z, VkImageLayout* prev);
void FxDepthDone(ReplayCtx& cx, TextureImpl* z, VkImageLayout prev);
}  // namespace nvn
