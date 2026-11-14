// SPDX-License-Identifier: MIT
// P5 (lighting plan): wider shadow filtering. Every depth-compare tap of a fragment shader (OpImageSampleDrefExplicitLod, Lod 0, a 2D
// sampler2DShadow: 177 programs have one tap and 679 two, none rely on anything else; census in analysis/lighting/p5_census.md) is replaced by
// N taps on a Vogel (golden-angle) disk around the original coordinate and their average. The kernel radius is a fixed UV distance, so the softness of
// an edge is the same at 1024, 2048 and 4096 (a larger map just resolves it more smoothly). The coordinate and reference depth operands are reused as they
// are, so the game's NaN guard stays and a 1x1 "no shadow map" stand-in view (always lit) still returns 1 for every tap.
// SNAP_SHADOW_FILTER=0|8|16 (0 = off: this function is not called and every pipeline key is the original one).
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "hle/lighting_preset.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
enum : uint32_t { kOpTypeFloat = 22, kOpTypeVector = 23, kOpConstant = 43, kOpConstantComposite = 44, kOpFunction = 54, kOpFAdd = 129, kOpFSub = 131, kOpFMul = 133,
                  kOpImageSampleDrefExplicitLod = 90 };
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
}  // namespace

// SNAP_SHADOW_FILTER = 0 (off) | 8 | 16 (taps); SNAP_SHADOW_SOFTNESS = kernel radius in texels of the game's own 1024 map (default 1.5).
// SNAP_SHADOW_BIAS=<n>: receiver-side bias, n * 1e-4 subtracted from the reference depth of every tap (the game applies none and draws its casters without polygon offset).
float ShadowFilterBias() {
  static const float b = [] { const char* e = getenv("SNAP_SHADOW_BIAS"); const float f = e ? (float)atof(e) : 0.0f; return (f > 0.0f && f <= 1000.0f) ? f * 1.0e-4f : 0.0f; }();
  return b;
}
int ShadowFilterTaps() {
  static const int taps = [] {
    const char* e = getenv("SNAP_SHADOW_FILTER");
    const int v = (!e || !*e || !strcmp(e, "default")) ? 0 : atoi(e);
    if (v == 4 || v == 8 || v == 16 || v == 32) return v;
    return ShadowFilterBias() > 0.0f ? 1 : 0;   // a bias alone: one tap at the original coordinate, reference depth lowered
  }();
  return snaplight::ab_on((unsigned long long)g_presentCount) ? taps : 0;   // SNAP_LIGHT_AB
}
float ShadowFilterRadius() {
  static const float r = [] {
    float t = 1.5f;
    if (const char* e = getenv("SNAP_SHADOW_SOFTNESS")) { const float f = (float)atof(e); if (f >= 0.25f && f <= 16.0f) t = f; }
    return t / 1024.0f;
  }();
  return r;
}
bool ShadowFilterApplies(const StageProg& fs) {
  static std::mutex mu;
  static std::unordered_map<std::string, bool> cache;
  const std::string key = fs.key;
  { std::lock_guard<std::mutex> l(mu); auto it = cache.find(key); if (it != cache.end()) return it->second; }
  bool has = false;
  const std::vector<uint32_t>& w = fs.spv;
  for (size_t i = 5; i < w.size();) {
    const uint32_t wc = w[i] >> 16;
    if (!wc) break;
    if ((w[i] & 0xFFFF) == kOpImageSampleDrefExplicitLod) { has = true; break; }
    i += wc;
  }
  std::lock_guard<std::mutex> l(mu);
  cache[key] = has;
  return has;
}

// taps: 1 (bias only) or 4..32. radiusUv: kernel radius in texture-coordinate units. Returns false (out untouched) when the module has no patchable tap.
bool PatchShadowPcf(const std::vector<uint32_t>& in, int taps, float radiusUv, float bias, std::vector<uint32_t>& out) {
  if (in.size() < 6 || in[0] != 0x07230203 || taps < 1 || taps > 64) return false;
  // pass 1: float32 types, vec2-of-float32 types, result types of the instructions that define the taps' coordinates, first function, taps
  std::map<uint32_t, bool> isF32;
  std::map<uint32_t, uint32_t> vec2Of;           // vec2 type id -> component type id
  std::map<uint32_t, uint32_t> resType;          // result id -> result type id (only instructions with word1 = type, word2 = result)
  size_t firstFn = 0;
  std::vector<size_t> tapAt;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    if (op == kOpTypeFloat && wc >= 3 && in[i + 2] == 32) isF32[in[i + 1]] = true;
    else if (op == kOpTypeVector && wc >= 4 && in[i + 3] == 2) vec2Of[in[i + 1]] = in[i + 2];
    else if (op == kOpFunction) { if (!firstFn) firstFn = i; }
    else if (firstFn && wc >= 3) resType[in[i + 2]] = in[i + 1];
    if (op == kOpImageSampleDrefExplicitLod && wc >= 8 && firstFn) tapAt.push_back(i);
    i += wc;
  }
  if (!firstFn || tapAt.empty()) return false;
  // vec2 float32 type
  std::vector<std::pair<uint32_t, uint32_t>> usable;   // (tap index into tapAt, vec2 type)
  for (size_t t = 0; t < tapAt.size(); t++) {
    const size_t i = tapAt[t];
    const uint32_t resT = in[i + 1], coord = in[i + 4];
    auto ct = resType.find(coord);
    if (!isF32.count(resT) || ct == resType.end() || !vec2Of.count(ct->second) || vec2Of[ct->second] != resT) continue;
    if (in[i + 6] != 2) continue;   // operands: only "Lod" (mask 2) is understood
    usable.push_back({(uint32_t)t, ct->second});
  }
  if (usable.empty()) return false;

  uint32_t next = in[3];
  // constants: one scalar per distinct value, one vec2 constant per offset, 1/N
  struct Off { float x, y; };
  std::vector<Off> offs;
  const float golden = 2.39996323f;
  for (int k = 0; k < taps; k++) {
    const float r = taps == 1 ? 0.0f : radiusUv * std::sqrt(((float)k + 0.5f) / (float)taps), a = (float)k * golden;
    offs.push_back({r * std::cos(a), r * std::sin(a)});
  }
  // The float type of the taps: all taps of one module use the same one in practice; constants are made per (float type, vec2 type) pair.
  std::vector<uint32_t> constBlock, body;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> offIds;   // (float, vec2) -> composite ids per offset
  std::map<uint32_t, uint32_t> invN, biasId;
  auto fbits = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
  for (auto& u : usable) {
    const uint32_t fT = in[tapAt[u.first] + 1], v2T = u.second;
    if (!offIds.count({fT, v2T})) {
      std::vector<uint32_t> ids;
      std::map<uint32_t, uint32_t> sc;   // float bits -> scalar constant id
      auto scalar = [&](float f) {
        const uint32_t key = fbits(f);
        auto it = sc.find(key);
        if (it != sc.end()) return it->second;
        const uint32_t id = next++;
        Emit(constBlock, kOpConstant, {fT, id, key});
        sc[key] = id;
        return id;
      };
      for (auto& o : offs) {
        const uint32_t cx = scalar(o.x), cy = scalar(o.y), id = next++;
        Emit(constBlock, kOpConstantComposite, {v2T, id, cx, cy});
        ids.push_back(id);
      }
      offIds[{fT, v2T}] = ids;
    }
    if (!invN.count(fT)) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {fT, id, fbits(1.0f / (float)taps)}); invN[fT] = id; }
    if (bias > 0.0f && !biasId.count(fT)) { const uint32_t id = next++; Emit(constBlock, kOpConstant, {fT, id, fbits(bias)}); biasId[fT] = id; }
  }
  // pass 2: copy, inserting the constants before the first function and expanding every usable tap
  out.assign(in.begin(), in.begin() + 5);
  size_t ui = 0;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16;
    if (i == firstFn) out.insert(out.end(), constBlock.begin(), constBlock.end());
    if (ui < usable.size() && tapAt[usable[ui].first] == i) {
      const uint32_t fT = in[i + 1], res = in[i + 2], img = in[i + 3], coord = in[i + 4], dref = in[i + 5], lod = in[i + 7];
      const uint32_t v2T = usable[ui].second;
      const std::vector<uint32_t>& ids = offIds[{fT, v2T}];
      uint32_t acc = 0, drefUse = dref;
      if (bias > 0.0f) { drefUse = next++; Emit(out, kOpFSub, {fT, drefUse, dref, biasId[fT]}); }
      for (int k = 0; k < taps; k++) {
        const uint32_t c = next++, s = next++;
        Emit(out, kOpFAdd, {v2T, c, coord, ids[k]});
        Emit(out, kOpImageSampleDrefExplicitLod, {fT, s, img, c, drefUse, 2, lod});
        if (!acc) acc = s;
        else { const uint32_t a = next++; Emit(out, kOpFAdd, {fT, a, acc, s}); acc = a; }
      }
      if (taps == 1) { Emit(out, kOpFMul, {fT, res, acc, invN[fT]}); }   // 1/1: keeps the original result id as the definition
      else Emit(out, kOpFMul, {fT, res, acc, invN[fT]});
      ui++;
    } else out.insert(out.end(), in.begin() + i, in.begin() + i + wc);
    i += wc;
  }
  out[3] = next;
  return true;
}
}  // namespace nvn
