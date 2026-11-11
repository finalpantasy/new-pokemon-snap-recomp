// SPDX-License-Identifier: MIT
// Small BC7 encoder: mode 6 only (one subset, 7-bit RGBA endpoints with a shared p-bit each, 4-bit indices).
// Written for this project from the public BC7 format description. Used to transcode ASTC when the GPU cannot sample it.
#include "nvn_tex.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nvn {
namespace {
const int kW[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {
  uint8_t b[16] = {};
  int pos = 0;
  void Put(uint32_t v, int n) { for (int i = 0; i < n; i++, pos++) if ((v >> i) & 1) b[pos >> 3] |= 1 << (pos & 7); }
};
inline float Interp(float e0, float e1, int w) { return (e0 * (64 - w) + e1 * w) / 64.f; }

// Quantize endpoint value v (0..255 float) to 7 bits + p-bit; returns reconstructed 8-bit value.
inline int Quant(float v, int p, int& q7) {
  int q = (int)std::lround((v - p) / 2.f);
  q = std::clamp(q, 0, 127);
  q7 = q;
  return q * 2 + p;
}
}  // namespace

static void EncodeBlock(const uint8_t px[16][4], uint8_t* out) {
  float mean[4] = {}, lo[4], hi[4];
  for (int c = 0; c < 4; c++) { lo[c] = 255; hi[c] = 0; }
  for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { mean[c] += px[i][c]; lo[c] = std::min<float>(lo[c], px[i][c]); hi[c] = std::max<float>(hi[c], px[i][c]); }
  for (int c = 0; c < 4; c++) mean[c] /= 16.f;
  // principal axis by power iteration on the covariance matrix
  float cov[4][4] = {};
  for (int i = 0; i < 16; i++) for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) cov[a][b] += (px[i][a] - mean[a]) * (px[i][b] - mean[b]);
  float ax[4] = {hi[0] - lo[0] + 1e-3f, hi[1] - lo[1] + 1e-3f, hi[2] - lo[2] + 1e-3f, hi[3] - lo[3] + 1e-3f};
  for (int it = 0; it < 6; it++) {
    float n[4] = {};
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) n[a] += cov[a][b] * ax[b];
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
    if (len < 1e-6f) break;
    for (int c = 0; c < 4; c++) ax[c] = n[c] / len;
  }
  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < 16; i++) {
    float t = 0; for (int c = 0; c < 4; c++) t += (px[i][c] - mean[c]) * ax[c];
    tmin = std::min(tmin, t); tmax = std::max(tmax, t);
  }
  float e0[4], e1[4];
  for (int c = 0; c < 4; c++) { e0[c] = std::clamp(mean[c] + ax[c] * tmin, 0.f, 255.f); e1[c] = std::clamp(mean[c] + ax[c] * tmax, 0.f, 255.f); }

  int idx[16], q0[4], q1[4], p0 = 0, p1 = 0;
  for (int pass = 0; pass < 3; pass++) {
    // endpoints -> quantized (choose p-bits by error), then indices, then least-squares refinement
    float best = 1e30f;
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
      float err = 0; int t0[4], t1[4], r0[4], r1[4];
      for (int c = 0; c < 4; c++) {
        r0[c] = Quant(e0[c], a, t0[c]); r1[c] = Quant(e1[c], b, t1[c]);
        err += (r0[c] - e0[c]) * (r0[c] - e0[c]) + (r1[c] - e1[c]) * (r1[c] - e1[c]);
      }
      if (err < best) { best = err; p0 = a; p1 = b; for (int c = 0; c < 4; c++) { q0[c] = t0[c]; q1[c] = t1[c]; } }
    }
    float d0[4], d1[4];
    for (int c = 0; c < 4; c++) { d0[c] = q0[c] * 2 + p0; d1[c] = q1[c] * 2 + p1; }
    for (int i = 0; i < 16; i++) {
      int bi = 0; float be = 1e30f;
      for (int k = 0; k < 16; k++) {
        float e = 0;
        for (int c = 0; c < 4; c++) { float d = Interp(d0[c], d1[c], kW[k]) - px[i][c]; e += d * d; }
        if (e < be) { be = e; bi = k; }
      }
      idx[i] = bi;
    }
    if (pass == 2) break;
    // least squares for endpoints given indices
    float aa = 0, bb = 0, ab = 0;
    float ra[4] = {}, rb[4] = {};
    for (int i = 0; i < 16; i++) {
      float w1 = kW[idx[i]] / 64.f, w0 = 1.f - w1;
      aa += w0 * w0; bb += w1 * w1; ab += w0 * w1;
      for (int c = 0; c < 4; c++) { ra[c] += w0 * px[i][c]; rb[c] += w1 * px[i][c]; }
    }
    float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-4f) break;
    for (int c = 0; c < 4; c++) {
      e0[c] = std::clamp((ra[c] * bb - rb[c] * ab) / det, 0.f, 255.f);
      e1[c] = std::clamp((rb[c] * aa - ra[c] * ab) / det, 0.f, 255.f);
    }
  }
  if (idx[0] >= 8) {  // anchor index has an implicit top bit of 0
    for (int c = 0; c < 4; c++) std::swap(q0[c], q1[c]);
    std::swap(p0, p1);
    for (int i = 0; i < 16; i++) idx[i] = 15 - idx[i];
  }
  Bits b;
  b.Put(1 << 6, 7);
  for (int c = 0; c < 4; c++) { b.Put(q0[c], 7); b.Put(q1[c], 7); }
  b.Put(p0, 1); b.Put(p1, 1);
  for (int i = 0; i < 16; i++) b.Put(idx[i], i == 0 ? 3 : 4);
  std::memcpy(out, b.b, 16);
}

void EncodeBc7(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out) {
  uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  for (uint32_t by = 0; by < bh; by++)
    for (uint32_t bx = 0; bx < bw; bx++) {
      uint8_t px[16][4];
      for (int i = 0; i < 16; i++) {
        uint32_t x = std::min(bx * 4 + (i & 3), w - 1), y = std::min(by * 4 + (i >> 2), h - 1);
        std::memcpy(px[i], rgba + ((size_t)y * w + x) * 4, 4);
      }
      EncodeBlock(px, out + ((size_t)by * bw + bx) * 16);
    }
}

}  // namespace nvn
