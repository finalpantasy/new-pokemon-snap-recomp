// SPDX-License-Identifier: MIT
// Texture helpers that do not depend on Vulkan: Maxwell (Tegra X1) block-linear layout, deswizzle/swizzle, ASTC LDR
// decode and a BC7 (mode 6) encoder used to transcode ASTC when the GPU cannot sample it.
//
// Layout provenance (no Nintendo header or driver code was used):
//   * GOB = 64 bytes x 8 rows (512 B), laid out as 16-byte sectors; blocks stack GOBs in y (block height) and z (block
//     depth). Reference: tools/ref/las/runtime/nvn/tegra.c (MIT) and the public tegra_swizzle layout description.
//   * Default block height: the tegra_swizzle rule (height_in_blocks + height_in_blocks / 2 against 16/32/64/128), which
//     reproduced the footer sizes of 239 distinct geometries and decoded smoother than the "next power of two" rule on
//     six textures where the two differ (survey in docs/nvn-vulkan-design.md).
//   * Per-level block height shrinks while the level is shorter than half a block; arrays/cubes align the layer size to
//     512 << blockHeightLog2 (verified against the 108 cubemaps in the game's NUTEXB set).
//   * 3D textures: block depth from the depth (same thresholds, up to 16), block height limited so that
//     blockHeight * blockDepth <= 16 GOBs. Verified on the 33^3 and 65^3 RGBA16F colour LUTs (decoded volume is smooth
//     only for height 1 / depth 16); the 1024x1024x8 BC1 3D texture has no checkable content.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nvn {

struct LevelLayout {
  uint64_t offset = 0;                  // from the start of the layer
  uint32_t w = 0, h = 0, d = 0;         // texels
  uint32_t wb = 0, hb = 0;              // blocks
  uint32_t bhLog2 = 0, bdLog2 = 0;      // GOBs per block (log2) in y / z for this level
  uint64_t size = 0;                    // bytes of this level in the swizzled layout
};
struct SurfaceLayout {
  uint32_t bpb = 4, bw = 1, bh = 1;     // bytes per block, block size in texels
  uint32_t layers = 1;
  std::vector<LevelLayout> levels;
  uint64_t layerSize = 0, total = 0;
};

uint32_t DefaultBlockHeightLog2(uint32_t heightInBlocks);
uint32_t DefaultBlockDepthLog2(uint32_t depth);
// width/height in texels; depth > 1 only for 3D textures (layers is the array/cube layer count, 1 otherwise).
SurfaceLayout ComputeSurfaceLayout(uint32_t w, uint32_t h, uint32_t depth, uint32_t layers, uint32_t levels,
                                   uint32_t bpb, uint32_t bw, uint32_t bh);
// One level (all depth slices) between block-linear memory and tightly packed rows (pitch = wb * bpb, then hb rows,
// then d slices).
void DeswizzleLevel(const uint8_t* src, uint8_t* dst, const LevelLayout& l, uint32_t bpb);
void SwizzleLevel(const uint8_t* src, uint8_t* dst, const LevelLayout& l, uint32_t bpb);

// ASTC LDR (2D) -> RGBA8, nvn_astc.cpp.
bool AstcDecodeBlock(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t* rgbaOut);
void AstcDecodeLdr(const uint8_t* blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                   uint8_t* rgbaOut);

// RGBA8 (w*h*4, tightly packed) -> BC7 blocks (ceil(w/4)*ceil(h/4)*16 bytes), nvn_bc7.cpp. Mode 6 only.
void EncodeBc7(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out);

}  // namespace nvn
