// SPDX-License-Identifier: MIT
// Ray tracing track (2026-11-14, rt worker; docs/lighting-rt-plan.md section 4). Everything is behind SNAP_RT=1: with it unset the device,
// the shader modules, the pipelines and every buffer are exactly what they were (no extension, no feature, no hook does anything).
//
//   M0  device: VK_KHR_acceleration_structure + ray_query + deferred_host_operations + EXT_transform_feedback, Vulkan 1.2 bufferDeviceAddress.
//       Buffers the game's memory pools create are NOT touched: only this file's own buffers carry the device-address flags.
//   M1  geometry capture: every vertex shader gets a transform-feedback capture of gl_Position (SPIR-V patch, harmless when no capture
//       is active). A scene opaque draw (1920x1080 DEPTH32F target, depth write on, no blending) is captured by the same draw call that
//       renders it: post-vertex-shader clip positions, expanded to a triangle list, into a persistent arena. A compute pass maps them to
//       world space with the inverse of the camera bank's viewProjection (c3 +0x100); no vertex or index data is read from guest memory.
//   M2  one BLAS per captured draw (persistent cache keyed by draw identity, rebuilt while the draw is visible, kept while it is not) and
//       one TLAS per frame, built at the game's tonemap draw. SNAP_RT_DEBUG=1|2|3 draws a ray-query overlay (see rt_ovl.frag).
//
// Switches: SNAP_RT=1 (master), SNAP_RT_DEBUG=<1|2|3>, SNAP_RT_LOG=<n> (stats line every n presents, default 120 with SNAP_RT_DEBUG),
// SNAP_RT_ARENA_MB (default 384), SNAP_RT_BLAS_MB (512), SNAP_RT_SCRATCH_MB (256), SNAP_RT_TTL (frames a stable draw stays in the TLAS
// after it was last seen, default 900), SNAP_RT_FLIPY=0|1 (clip y to screen y), SNAP_RT_CAPTURE=0 (device features only), SNAP_RT_KEYCB=0|1 (per-object constants in the draw key, default 1).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace nvn { struct Host; struct ReplayCtx; struct TextureImpl; }

namespace snap_rt {

bool Wanted();    // SNAP_RT=1
bool On();        // the device was created with the ray tracing features
bool Capture();   // On() and SNAP_RT_CAPTURE != 0

// Device setup (nvn_vk.cpp): adds the extensions and chains the feature structs after f11; sets f12.bufferDeviceAddress.
void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& dext, VkPhysicalDeviceVulkan11Features* f11, VkPhysicalDeviceVulkan12Features* f12);
void DeviceCreated(nvn::Host* h);
void DeviceShutdown(nvn::Host* h);

// Vertex module patch (nvn_shader.cpp): decorates gl_Position for transform feedback. Returns true when it changed the module.
bool PatchVertexXfb(std::vector<uint32_t>& spv, const char* key12);

// What the draw replay knows about a draw (nvn_draw.cpp fills it; DrawState is private to that file).
struct DrawInfo {
  const char* vsKey = nullptr;          // vertex stage cache key (hex)
  bool hasGeomTess = false;             // a geometry / tessellation stage is bound: not captured
  VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  uint32_t vertexCount = 0;             // vertices the draw assembles (after the host's index rewrite)
  uint32_t instances = 1;
  uint64_t ibAddr = 0, vbAddr[4] = {};  // guest addresses (identity of the draw)
  int first = 0, base = 0, count = 0;
  bool depthTest = false, depthWrite = false, blend0 = false;
  const uint8_t* cam = nullptr; uint64_t camRange = 0;   // vertex constant bank c3 (PerViewCBuffer) in guest memory
  uint64_t cbHash = 0;                  // hash of the first 256 bytes of every other bound vertex constant bank (per-object constants: tells apart draws of one mesh)
};

// One per draw: Begin right before the vkCmdDraw*, End right after. Does nothing unless the draw is a scene opaque draw being captured.
struct DrawCapture {
  nvn::ReplayCtx* cx = nullptr; bool open = false;
  void Begin(nvn::ReplayCtx& c, const DrawInfo& di);
  void End();
  ~DrawCapture() { End(); }
};

// The game's tonemap draw has just been recorded (nvn_draw.cpp, next to AaAfterDraw): convert, build the BLAS/TLAS, draw the overlay.
void OnTonemap(nvn::ReplayCtx& cx, const char* fsKey);
void OnPresent();   // frame bookkeeping (nvn_vk.cpp present)

}  // namespace snap_rt
