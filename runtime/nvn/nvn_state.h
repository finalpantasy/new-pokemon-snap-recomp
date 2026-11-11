// SPDX-License-Identifier: MIT
// Guest-visible NVN state objects, packed in place by the Set* entry points and copied by value into the recorded
// command stream. The layouts are ours (the game never looks inside); sizes fit the real objects
// ([TOTK-NVN] sizes: blendState 8, colorState 4, channelMaskState 4, polygonState 4, depthStencilState 8,
// multisampleState 0x18, vertexAttribState 4, vertexStreamState 8).
// Enum values are NVN's (printed from tools/ref/totk/runtime/nvn.h, MIT reference); the mapping to Vulkan is in
// nvn_draw.cpp.
#pragma once
#include <cstdint>

namespace nvn {

enum : int {  // NVNblendFunc
  BF_ZERO = 1, BF_ONE = 2, BF_SRC_COLOR = 3, BF_ONE_MINUS_SRC_COLOR = 4, BF_SRC_ALPHA = 5, BF_ONE_MINUS_SRC_ALPHA = 6,
  BF_DST_ALPHA = 7, BF_ONE_MINUS_DST_ALPHA = 8, BF_DST_COLOR = 9, BF_ONE_MINUS_DST_COLOR = 0xA, BF_SRC_ALPHA_SATURATE = 0xB,
  BF_SRC1_COLOR = 0x10, BF_ONE_MINUS_SRC1_COLOR = 0x11, BF_SRC1_ALPHA = 0x12, BF_ONE_MINUS_SRC1_ALPHA = 0x13,
  BF_CONSTANT_COLOR = 0x61, BF_ONE_MINUS_CONSTANT_COLOR = 0x62, BF_CONSTANT_ALPHA = 0x63, BF_ONE_MINUS_CONSTANT_ALPHA = 0x64,
};
enum : int { BE_ADD = 1, BE_SUB = 2, BE_REVSUB = 3, BE_MIN = 4, BE_MAX = 5 };                    // NVNblendEquation
enum : int { FACE_NONE = 0, FACE_FRONT = 1, FACE_BACK = 2, FACE_FRONT_AND_BACK = 3 };            // NVNface
enum : int { FRONT_CW = 0, FRONT_CCW = 1 };                                                     // NVNfrontFace
enum : int { POLY_POINT = 0, POLY_LINE = 1, POLY_FILL = 2 };                                    // NVNpolygonMode
enum : int { CMP_NEVER = 1, CMP_LESS = 2, CMP_EQUAL = 3, CMP_LEQUAL = 4, CMP_GREATER = 5, CMP_NOTEQUAL = 6, CMP_GEQUAL = 7, CMP_ALWAYS = 8 };
enum : int { SOP_KEEP = 1 };                                                                    // NVNstencilOp (1..8)
enum : int { IDX_U8 = 0, IDX_U16 = 1, IDX_U32 = 2 };                                            // NVNindexType
enum : int { LOGIC_COPY = 3 };                                                                  // placeholder default (unused by replay)

struct BlendSt { uint8_t srcRgb, dstRgb, srcA, dstA, eqRgb, eqA, target, pad; };
struct ColorSt { uint8_t blendEn, logicOp, alphaFunc, pad; };            // blendEn: bit per render target
struct ChMaskSt { uint32_t mask; };                                       // 4 bits (r,g,b,a) per render target
struct MsSt { uint8_t enable, samples, a2c, a2cDither, covMod, covToColor, covToColorOut, slEn, slGrid, pad[15]; };
struct PolySt { uint8_t cull, front, mode, offsetEn; };                   // offsetEn: 1 point, 2 line, 4 fill
struct DsSt { uint64_t bits; };
struct VaSt { uint32_t v; };                                              // fmt:9 | stream:4 << 9 | offset:19 << 13
struct VsSt { uint32_t stride, divisor; };
static_assert(sizeof(BlendSt) == 8 && sizeof(ColorSt) == 4 && sizeof(ChMaskSt) == 4 && sizeof(MsSt) == 0x18 &&
              sizeof(PolySt) == 4 && sizeof(DsSt) == 8 && sizeof(VaSt) == 4 && sizeof(VsSt) == 8);

// DepthStencil bit layout
enum : int { DS_DT = 0, DS_DW = 1, DS_ST = 2, DS_DF = 3, DS_SF_F = 7, DS_SF_B = 11, DS_FAIL_F = 15, DS_ZFAIL_F = 19, DS_ZPASS_F = 23,
             DS_FAIL_B = 27, DS_ZFAIL_B = 31, DS_ZPASS_B = 35 };
inline uint32_t DsGet(const DsSt& s, int sh, int bits) { return (uint32_t)((s.bits >> sh) & ((1u << bits) - 1)); }
inline void DsSet(DsSt& s, int sh, int bits, uint32_t v) {
  uint64_t m = (uint64_t)((1u << bits) - 1) << sh;
  s.bits = (s.bits & ~m) | ((uint64_t)v << sh & m);
}

inline uint32_t VaFmt(const VaSt& s) { return s.v & 0x1FF; }
inline uint32_t VaStream(const VaSt& s) { return (s.v >> 9) & 0xF; }
inline uint32_t VaOffset(const VaSt& s) { return s.v >> 13; }

}  // namespace nvn
