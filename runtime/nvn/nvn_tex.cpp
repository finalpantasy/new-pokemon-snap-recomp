// SPDX-License-Identifier: MIT
// Maxwell block-linear layout, see nvn_tex.h for provenance.
#include "nvn_tex.h"
#include <algorithm>
#include <cstring>

namespace nvn {

uint32_t DefaultBlockHeightLog2(uint32_t hb) {
  uint32_t hh = hb + hb / 2;
  return hh >= 128 ? 4 : hh >= 64 ? 3 : hh >= 32 ? 2 : hh >= 16 ? 1 : 0;
}
uint32_t DefaultBlockDepthLog2(uint32_t d) {
  uint32_t l = 0;
  while ((1u << l) < d && l < 4) l++;
  return l;
}
static uint32_t CeilDiv(uint64_t a, uint64_t b) { return (uint32_t)((a + b - 1) / b); }

SurfaceLayout ComputeSurfaceLayout(uint32_t w, uint32_t h, uint32_t depth, uint32_t layers, uint32_t levels,
                                   uint32_t bpb, uint32_t bw, uint32_t bh) {
  SurfaceLayout s;
  s.bpb = bpb; s.bw = bw; s.bh = bh; s.layers = layers ? layers : 1;
  uint32_t bh0 = DefaultBlockHeightLog2(CeilDiv(h, bh)), bd0 = 0;
  if (depth > 1) {
    bd0 = DefaultBlockDepthLog2(depth);
    bh0 = std::min(bh0, 4 - bd0);
  }
  uint64_t off = 0;
  for (uint32_t m = 0; m < std::max(levels, 1u); m++) {
    LevelLayout l;
    l.w = std::max(w >> m, 1u); l.h = std::max(h >> m, 1u); l.d = std::max(depth >> m, 1u);
    l.wb = CeilDiv(l.w, bw); l.hb = CeilDiv(l.h, bh);
    uint32_t lbh = bh0, lbd = bd0;
    while (lbh > 0 && (8u << (lbh - 1)) >= l.hb) lbh--;
    while (lbd > 0 && (1u << (lbd - 1)) >= l.d) lbd--;
    l.bhLog2 = lbh; l.bdLog2 = lbd; l.offset = off;
    l.size = (uint64_t)CeilDiv((uint64_t)l.wb * bpb, 64) * 512 * (1u << lbh) * (1u << lbd) *
             CeilDiv(l.hb, 8u << lbh) * CeilDiv(l.d, 1u << lbd);
    off += l.size;
    s.levels.push_back(l);
  }
  if (s.layers > 1) { uint64_t al = 512ull << bh0; off = (off + al - 1) / al * al; }
  s.layerSize = off;
  s.total = off * s.layers;
  return s;
}

// Address of byte (x, y, z) inside a level (x in bytes). Same arithmetic as tests/nvn/nutexb_ref.py.
static inline uint64_t GobAddr(const LevelLayout& l, uint32_t x, uint32_t y, uint32_t z, uint64_t widthGobs,
                               uint64_t blocksY) {
  uint32_t bh = 1u << l.bhLog2, bd = 1u << l.bdLog2, gobRows = 8 * bh;
  uint64_t blockSize = 512ull * bh * bd;
  uint64_t a = ((z / bd) * blocksY + (y / gobRows)) * widthGobs * blockSize;
  a += (uint64_t)(x / 64) * blockSize;
  a += (uint64_t)(((z % bd) * bh) + ((y % gobRows) / 8)) * 512;
  a += ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
  return a;
}

template <bool Swz>
static void Walk(const uint8_t* src, uint8_t* dst, const LevelLayout& l, uint32_t bpb) {
  uint32_t rowb = l.wb * bpb;
  uint64_t widthGobs = CeilDiv(rowb, 64), blocksY = CeilDiv(l.hb, 8u << l.bhLog2);
  for (uint32_t z = 0; z < l.d; z++)
    for (uint32_t y = 0; y < l.hb; y++) {
      uint8_t* lin = Swz ? nullptr : dst + ((uint64_t)z * l.hb + y) * rowb;
      const uint8_t* linS = Swz ? src + ((uint64_t)z * l.hb + y) * rowb : nullptr;
      for (uint32_t x = 0; x < rowb; x += 16) {  // 16-byte sectors never straddle, rows are multiples of bpb>=1 though
        uint32_t n = std::min(16u, rowb - x);
        uint64_t a = GobAddr(l, x, y, z, widthGobs, blocksY);
        if (Swz) std::memcpy(dst + l.offset * 0 + a, linS + x, n);
        else std::memcpy(lin + x, src + a, n);
      }
    }
}
void DeswizzleLevel(const uint8_t* src, uint8_t* dst, const LevelLayout& l, uint32_t bpb) { Walk<false>(src, dst, l, bpb); }
void SwizzleLevel(const uint8_t* src, uint8_t* dst, const LevelLayout& l, uint32_t bpb) { Walk<true>(src, dst, l, bpb); }

}  // namespace nvn
