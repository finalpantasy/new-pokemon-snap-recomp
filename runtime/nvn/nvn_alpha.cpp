// SPDX-License-Identifier: MIT
// Alpha test emulation (NVN has a fixed-function alpha test, Vulkan does not): a fragment SPIR-V module is wrapped so
// that a new entry point calls the original, reads output location 0 .w and discards the fragment when
// `a <func> ref` is false. The reference value is specialization constant 0 so one patched module serves every ref.
#include <algorithm>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>
#include "nvn_internal.h"

namespace nvn {
namespace {
enum : uint32_t {
  OpEntryPoint = 15, OpExecutionMode = 16, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
  OpTypePointer = 32, OpVariable = 59, OpFunction = 54, OpFunctionCall = 57, OpLoad = 61, OpLabel = 248, OpBranchConditional = 250,
  OpReturn = 253, OpKill = 252, OpFunctionEnd = 56, OpSelectionMerge = 247, OpDecorate = 71, OpMemberDecorate = 72,
  OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75, OpSpecConstant = 50, OpCompositeExtract = 81,
  OpFOrdEqual = 180, OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188,
  OpFOrdGreaterThanEqual = 190, OpConstantFalse = 42,
};
void Emit(std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> w) {
  v.push_back(((uint32_t)(w.size() + 1) << 16) | op);
  v.insert(v.end(), w.begin(), w.end());
}
}  // namespace

// func: NVN compare enum 1..8 (NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS). Returns false when the module has
// no float vec4 output at location 0 (the module is then left alone).
bool PatchAlphaTest(const std::vector<uint32_t>& in, int func, std::vector<uint32_t>& out) {
  if (in.size() < 6 || in[0] != 0x07230203) return false;
  const uint32_t bound = in[3];
  uint32_t entry = 0, var0 = 0, voidT = 0, fnT = 0, boolT = 0, annotEnd = 0, firstFn = 0, oldMainIdx = 0;
  uint32_t varType = 0;                                  // OpVariable result type for var0 (pointer type)
  std::vector<std::pair<uint32_t, uint32_t>> ptrs;       // pointer type id -> pointee
  struct Vec { uint32_t id, comp, n; };
  std::vector<Vec> vecs;
  std::vector<uint32_t> floats;                          // 32-bit float type ids
  std::vector<std::pair<uint32_t, uint32_t>> varSc;      // variable id -> pointer type
  size_t i = 5;
  while (i < in.size()) {
    uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    switch (op) {
      case OpEntryPoint: if (in[i + 1] == 4 && !entry) { entry = in[i + 2]; oldMainIdx = (uint32_t)i; } break;
      case OpDecorate: if (wc >= 4 && in[i + 2] == 30 && in[i + 3] == 0) var0 = in[i + 1]; annotEnd = (uint32_t)(i + wc); break;
      case OpMemberDecorate: case OpDecorationGroup: case OpGroupDecorate: case OpGroupMemberDecorate: annotEnd = (uint32_t)(i + wc); break;
      case OpTypeVoid: voidT = in[i + 1]; break;
      case OpTypeBool: boolT = in[i + 1]; break;
      case OpTypeFloat: if (in[i + 2] == 32) floats.push_back(in[i + 1]); break;
      case OpTypeVector: vecs.push_back({in[i + 1], in[i + 2], in[i + 3]}); break;
      case OpTypePointer: ptrs.push_back({in[i + 1], in[i + 3]}); break;
      case OpVariable: varSc.push_back({in[i + 2], in[i + 1]}); break;
      case OpFunction: if (!firstFn) firstFn = (uint32_t)i; if (in[i + 2] == entry) { fnT = in[i + 4]; voidT = in[i + 1]; } break;
      default: break;
    }
    i += wc;
  }
  (void)oldMainIdx;
  if (!entry || !var0 || !fnT || !firstFn || !annotEnd) return false;
  for (auto& v : varSc) if (v.first == var0) varType = v.second;
  uint32_t vecT = 0, floatT = 0;
  for (auto& p : ptrs) if (p.first == varType) vecT = p.second;
  for (auto& v : vecs) if (v.id == vecT && v.n == 4) for (uint32_t f : floats) if (f == v.comp) floatT = f;
  if (!floatT) return false;

  uint32_t next = bound;
  const uint32_t refId = next++, newMain = next++, lbl = next++, callId = next++, loadId = next++, aId = next++, condId = next++, killLbl = next++, mergeLbl = next++;
  const bool addBool = !boolT;
  if (addBool) boolT = next++;

  out.assign(in.begin(), in.begin() + 5);
  out[3] = 0;  // patched below
  // copy up to the end of annotations, retargeting OpEntryPoint / OpExecutionMode at the new entry function
  size_t k = 5;
  while (k < in.size()) {
    uint32_t wc = in[k] >> 16, op = in[k] & 0xFFFF;
    if (k == annotEnd) break;
    size_t base = out.size();
    out.insert(out.end(), in.begin() + k, in.begin() + k + wc);
    if (op == OpEntryPoint && in[k + 2] == entry) out[base + 2] = newMain;
    if (op == OpExecutionMode && in[k + 1] == entry) out[base + 1] = newMain;
    k += wc;
  }
  Emit(out, OpDecorate, {refId, 1, 0});   // SpecId 0
  // copy the rest up to the first function, retargeting remaining entry-related instructions (none live there)
  while (k < firstFn) { uint32_t wc = in[k] >> 16; out.insert(out.end(), in.begin() + k, in.begin() + k + wc); k += wc; }
  if (addBool) Emit(out, OpTypeBool, {boolT});
  Emit(out, OpSpecConstant, {floatT, refId, 0});
  // original functions
  out.insert(out.end(), in.begin() + k, in.end());
  // wrapper
  Emit(out, OpFunction, {voidT, newMain, 0, fnT});
  Emit(out, OpLabel, {lbl});
  Emit(out, OpFunctionCall, {voidT, callId, entry});
  if (func != 1) {
    Emit(out, OpLoad, {vecT, loadId, var0});
    Emit(out, OpCompositeExtract, {floatT, aId, loadId, 3});
    uint32_t cmp = 0;
    switch (func) {
      case 2: cmp = OpFOrdLessThan; break; case 3: cmp = OpFOrdEqual; break; case 4: cmp = OpFOrdLessThanEqual; break;
      case 5: cmp = OpFOrdGreaterThan; break; case 6: cmp = OpFUnordNotEqual; break; case 7: cmp = OpFOrdGreaterThanEqual; break;
      default: break;
    }
    if (cmp) {
      Emit(out, cmp, {boolT, condId, aId, refId});
      Emit(out, OpSelectionMerge, {mergeLbl, 0});
      Emit(out, OpBranchConditional, {condId, mergeLbl, killLbl});
      Emit(out, OpLabel, {killLbl});
      Emit(out, OpKill, {});
      Emit(out, OpLabel, {mergeLbl});
      Emit(out, OpReturn, {});
    } else Emit(out, OpReturn, {});
  } else {   // NEVER
    Emit(out, OpKill, {});
  }
  Emit(out, OpFunctionEnd, {});
  out[3] = next;
  (void)callId;
  return true;
}


// ---- SPIR-V patches for the translated modules -------------------------------------------------------------------------
// The translator's output is used as is except for the fixes below (each one a Vulkan validity rule the translator does not know).
namespace {
enum : uint32_t { OpCapability = 17, OpTypeImage = 25, OpTypeArray = 28, OpTypeMatrix = 24, OpConstant = 43, OpAccessChain = 65, OpInBoundsAccessChain = 66,
                  OpImageRead = 98, OpImageWrite = 99 };
constexpr uint32_t kDecLocation = 30, kDecComponent = 31, kDecIndex = 32, kDecBuiltIn = 11;
constexpr uint32_t kScInput = 1, kScOutput = 3, kScPrivate = 6;
}  // namespace

// A storage image declared with an unknown format needs StorageImageRead/WriteWithoutFormat; the translator omits the capability.
bool SpvAddStorageImageCaps(std::vector<uint32_t>& spv) {
  if (spv.size() < 6 || spv[0] != 0x07230203) return false;
  bool unknownFmt = false, rd = false, wr = false, hasRd = false, hasWr = false;
  size_t firstNonCap = 0;
  for (size_t i = 5; i < spv.size();) {
    const uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
    if (!wc || i + wc > spv.size()) return false;
    if (op == OpCapability) { if (spv[i + 1] == 55) hasRd = true; if (spv[i + 1] == 56) hasWr = true; }
    else if (!firstNonCap) firstNonCap = i;
    if (op == OpTypeImage && wc >= 9 && spv[i + 7] == 2 && spv[i + 8] == 0) unknownFmt = true;
    if (op == OpImageRead) rd = true;
    if (op == OpImageWrite) wr = true;
    i += wc;
  }
  if (!unknownFmt || !firstNonCap) return false;
  std::vector<uint32_t> add;
  if (rd && !hasRd) Emit(add, OpCapability, {55});
  if (wr && !hasWr) Emit(add, OpCapability, {56});
  if (add.empty()) return false;
  spv.insert(spv.begin() + firstNonCap, add.begin(), add.end());
  return true;
}

// Bit n set = the vertex shader reads input location n (arrays and matrices cover several locations).
uint32_t SpvVertexInputMask(const std::vector<uint32_t>& in) {
  if (in.size() < 6 || in[0] != 0x07230203) return 0xFFFFFFFFu;
  std::unordered_map<uint32_t, uint32_t> loc, builtin, ptrPointee, varType, constVal, arrElem, arrLen, matCols;
  std::vector<uint32_t> inputVars;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return 0xFFFFFFFFu;
    switch (op) {
      case OpDecorate:
        if (wc >= 4 && in[i + 2] == kDecLocation) loc[in[i + 1]] = in[i + 3];
        if (wc >= 3 && in[i + 2] == kDecBuiltIn) builtin[in[i + 1]] = 1;
        break;
      case OpTypePointer: ptrPointee[in[i + 1]] = in[i + 3]; break;
      case OpVariable: varType[in[i + 2]] = in[i + 1]; if (in[i + 3] == kScInput) inputVars.push_back(in[i + 2]); break;
      case OpConstant: if (wc >= 4) constVal[in[i + 2]] = in[i + 3]; break;
      case OpTypeArray: arrElem[in[i + 1]] = in[i + 2]; arrLen[in[i + 1]] = in[i + 3]; break;
      case OpTypeMatrix: matCols[in[i + 1]] = in[i + 3]; break;
      default: break;
    }
    i += wc;
  }
  uint32_t mask = 0;
  for (uint32_t v : inputVars) {
    if (builtin.count(v) || !loc.count(v)) continue;
    uint32_t t = ptrPointee[varType[v]], n = 1;
    while (arrElem.count(t)) { n *= std::max(1u, constVal.count(arrLen[t]) ? constVal[arrLen[t]] : 1u); t = arrElem[t]; }
    if (matCols.count(t)) n *= matCols[t];
    for (uint32_t k = 0; k < n && loc[v] + k < 32; k++) mask |= 1u << (loc[v] + k);
  }
  return mask;
}

// Locations of the fragment color outputs (OpVariable Output with a Location decoration), ascending.
std::vector<int> SpvFragmentOutputs(const std::vector<uint32_t>& in) {
  std::vector<int> r;
  if (in.size() < 6 || in[0] != 0x07230203) return r;
  std::unordered_map<uint32_t, uint32_t> loc;
  std::vector<uint32_t> outs;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return r;
    if (op == OpDecorate && wc >= 4 && in[i + 2] == kDecLocation) loc[in[i + 1]] = in[i + 3];
    if (op == OpVariable && in[i + 3] == kScOutput) outs.push_back(in[i + 2]);
    i += wc;
  }
  for (uint32_t v : outs) if (loc.count(v)) r.push_back((int)loc[v]);
  std::sort(r.begin(), r.end());
  return r;
}

// Rewrites the fragment outputs for one pipeline:
//  - outputs at a location >= nColor (no such attachment) become Private variables, so the write is dropped inside the shader
//    instead of tripping the "write is unused" validation warning;
//  - dual = true: the two lowest outputs become source 0 and source 1 of one location (Location L Index 0 / 1) and every
//    other output is dropped (Vulkan allows dual-source output only at location 0 when maxFragmentDualSrcAttachments is 1).
// Returns false when the module needs no change.
bool PatchFragmentOutputs(const std::vector<uint32_t>& in, int nColor, bool dual, std::vector<uint32_t>& out) {
  if (in.size() < 6 || in[0] != 0x07230203) return false;
  const bool legacyIface = in[1] < 0x10400;   // before SPIR-V 1.4 the entry point interface lists only Input/Output variables
  std::unordered_map<uint32_t, uint32_t> loc, varType;
  std::vector<std::pair<uint32_t, uint32_t>> outs;   // (location, var id)
  std::vector<uint32_t> outVars;
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) return false;
    if (op == OpDecorate && wc >= 4 && in[i + 2] == kDecLocation) loc[in[i + 1]] = in[i + 3];
    if (op == OpVariable) { varType[in[i + 2]] = in[i + 1]; if (in[i + 3] == kScOutput) outVars.push_back(in[i + 2]); }
    i += wc;
  }
  for (uint32_t v : outVars) if (loc.count(v)) outs.push_back({loc[v], v});
  std::sort(outs.begin(), outs.end());
  std::set<uint32_t> strip;
  uint32_t second = 0, firstLoc = 0;
  if (dual && outs.size() >= 2) {
    firstLoc = outs[0].first; second = outs[1].second;
    for (size_t k = 2; k < outs.size(); k++) strip.insert(outs[k].second);
  } else {
    for (auto& o : outs) if ((int)o.first >= nColor) strip.insert(o.second);
  }
  if (strip.empty() && !second) return false;
  // access chains rooted at a stripped variable change pointer storage class too
  std::set<uint32_t> chain = strip;
  std::set<uint32_t> needPtr;   // Output pointer type ids that need a Private twin
  for (uint32_t v : strip) needPtr.insert(varType[v]);
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 4 && chain.count(in[i + 3])) { chain.insert(in[i + 2]); needPtr.insert(in[i + 1]); }
    i += wc;
  }
  uint32_t next = in[3];
  std::unordered_map<uint32_t, uint32_t> priv;   // Output pointer type -> Private pointer type
  for (uint32_t t : needPtr) priv[t] = next++;
  out.assign(in.begin(), in.begin() + 5);
  for (size_t i = 5; i < in.size();) {
    const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    const uint32_t* w = &in[i];
    if (op == OpEntryPoint && legacyIface) {
      // OpEntryPoint model id "name" interface...: the name is a NUL-terminated string packed in words, find its last word
      size_t k = 3;
      while (k < wc) {
        const bool hasNul = ((w[k] & 0xFF000000u) == 0) || ((w[k] & 0x00FF0000u) == 0) || ((w[k] & 0x0000FF00u) == 0) || ((w[k] & 0xFFu) == 0);
        k++;
        if (hasNul) break;
      }
      std::vector<uint32_t> ins(w, w + std::min<size_t>(k, wc));
      for (size_t j = k; j < wc; j++) if (!strip.count(w[j])) ins.push_back(w[j]);
      ins[0] = ((uint32_t)ins.size() << 16) | OpEntryPoint;
      out.insert(out.end(), ins.begin(), ins.end());
    } else if (op == OpDecorate && wc >= 3 && strip.count(w[1]) && (w[2] == kDecLocation || w[2] == kDecComponent || w[2] == kDecIndex)) {
      // dropped
    } else if (op == OpDecorate && wc >= 4 && second && w[1] == second && w[2] == kDecLocation) {
      Emit(out, OpDecorate, {second, kDecLocation, firstLoc});
      Emit(out, OpDecorate, {second, kDecIndex, 1});
    } else if (op == OpTypePointer && priv.count(w[1])) {
      out.insert(out.end(), w, w + wc);
      Emit(out, OpTypePointer, {priv[w[1]], kScPrivate, w[3]});
    } else if (op == OpVariable && strip.count(w[2])) {
      std::vector<uint32_t> v(w, w + wc);
      v[1] = priv[v[1]]; v[3] = kScPrivate;
      out.insert(out.end(), v.begin(), v.end());
    } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 4 && chain.count(w[3])) {
      std::vector<uint32_t> v(w, w + wc);
      v[1] = priv[v[1]];
      out.insert(out.end(), v.begin(), v.end());
    } else {
      out.insert(out.end(), w, w + wc);
    }
    i += wc;
  }
  out[3] = next;
  return true;
}

}  // namespace nvn
