// SPDX-License-Identifier: MIT
// P8 (lighting plan): SNAP_SSAO_GTAO=1 replaces the game's HBAO fragment program (SSAOHorizonBasedAOPS) with our GTAO shader (shaders/gtao.frag). The game's own
// passes stay: SSAOCreate runs this program into its 1920x1080 target, BlurX/BlurY and Composite follow unchanged. Same interface as the original: two cbuffers
// (ConstantBuffer, HBAOConstantBuffer), one depth texture, one vec2 input, one color output. The replacement module is built from the original module's decorations:
// the cbuffer, texture and input location numbers are copied from the original, so the pipeline layout the layer already built for the program still fits.
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "nvn_gtao_spv.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
bool KeyListed(const char* key) {
  static const char* kKnown[] = {nullptr};   // 12-hex translation-cache key prefixes of SSAOHorizonBasedAOPS (filled in from a captured run)
  if (const char* e = getenv("SNAP_GTAO_KEY")) { if (*e && !strncmp(key, e, strlen(e))) return true; }
  for (const char* k : kKnown) if (k && !strncmp(key, k, strlen(k))) return true;
  return false;
}
struct Mod {
  std::map<uint32_t, std::string> names;                 // OpName
  std::map<uint32_t, uint32_t> ptrPointee, varType;      // pointer type -> pointee, variable -> pointer type
  std::map<uint32_t, uint32_t> varStorage;
  std::map<uint32_t, size_t> setAt, bindAt, locAt;       // OpDecorate word index of the literal operand
  std::map<uint32_t, uint32_t> sampledImgTypes;
};
bool Scan(const std::vector<uint32_t>& w, Mod& m) {
  if (w.size() < 6 || w[0] != 0x07230203) return false;
  for (size_t i = 5; i < w.size();) {
    const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFF;
    if (!wc || i + wc > w.size()) return false;
    if (op == 5 && wc >= 3) m.names[w[i + 1]] = std::string((const char*)&w[i + 2], strnlen((const char*)&w[i + 2], (wc - 2) * 4));   // OpName
    else if (op == 27) m.sampledImgTypes[w[i + 1]] = 1;                                   // OpTypeSampledImage
    else if (op == 32 && wc >= 4) m.ptrPointee[w[i + 1]] = w[i + 3];                      // OpTypePointer
    else if (op == 59 && wc >= 4) { m.varType[w[i + 2]] = w[i + 1]; m.varStorage[w[i + 2]] = w[i + 3]; }
    else if (op == 71 && wc >= 4) {                                                       // OpDecorate
      if (w[i + 2] == 34) m.setAt[w[i + 1]] = i + 3;
      else if (w[i + 2] == 33) m.bindAt[w[i + 1]] = i + 3;
      else if (w[i + 2] == 30) m.locAt[w[i + 1]] = i + 3;
    }
    i += wc;
  }
  return true;
}
uint32_t PointeeOf(const Mod& m, uint32_t var) {
  auto t = m.varType.find(var);
  if (t == m.varType.end()) return 0;
  auto p = m.ptrPointee.find(t->second);
  return p == m.ptrPointee.end() ? 0 : p->second;
}
uint32_t FindByName(const Mod& m, const char* name) {
  for (auto& n : m.names) if (n.second == name && m.varType.count(n.first)) return n.first;
  return 0;
}
}  // namespace

// Replaces `words` (a fragment program, original translation) by the GTAO module when SNAP_SSAO_GTAO=1 and `key` names the HBAO program.
bool GtaoPatchFragment(std::vector<uint32_t>& words, const char* key) {
  static const bool on = getenv("SNAP_SSAO_GTAO") && getenv("SNAP_SSAO_GTAO")[0] == '1';
  if (!on || !key || !KeyListed(key)) return false;
  Mod o, g;
  std::vector<uint32_t> repl(kGtaoFrag, kGtaoFrag + sizeof kGtaoFrag / 4);
  if (!Scan(words, o) || !Scan(repl, g)) return false;
  // original: cbuffers fp_c3 / fp_c4 (ConstantBuffer, HBAOConstantBuffer), the sampled-image variable, the Input variable
  const uint32_t oc0 = FindByName(o, "fp_c3"), oc1 = FindByName(o, "fp_c4");
  uint32_t otex = 0, oin = 0;
  for (auto& v : o.varType) {
    const uint32_t pt = PointeeOf(o, v.first);
    if (o.varStorage[v.first] == 0 && o.sampledImgTypes.count(pt)) otex = v.first;
    if (o.varStorage[v.first] == 1 && o.locAt.count(v.first) && !oin) oin = v.first;
  }
  // replacement: variables by their block type name / own name
  uint32_t gc0 = 0, gc1 = 0, gtex = FindByName(g, "depthTex"), gin = FindByName(g, "vUv");
  for (auto& v : g.varType) {
    const uint32_t pt = PointeeOf(g, v.first);
    auto n = g.names.find(pt);
    if (n != g.names.end()) { if (n->second == "ConstantBuffer") gc0 = v.first; else if (n->second == "HBAOConstantBuffer") gc1 = v.first; }
  }
  if (!oc0 || !oc1 || !otex || !oin || !gc0 || !gc1 || !gtex || !gin || !o.bindAt.count(oc0) || !o.bindAt.count(oc1) || !o.bindAt.count(otex)) {
    LogError("GTAO: key %s does not look like the HBAO program (c3 %u c4 %u tex %u in %u); left alone", key, oc0, oc1, otex, oin);
    return false;
  }
  auto copyBind = [&](uint32_t from, uint32_t to) {
    repl[g.bindAt[to]] = words[o.bindAt[from]];
    if (o.setAt.count(from) && g.setAt.count(to)) repl[g.setAt[to]] = words[o.setAt[from]];
  };
  copyBind(oc0, gc0); copyBind(oc1, gc1); copyBind(otex, gtex);
  repl[g.locAt[gin]] = words[o.locAt[oin]];
  words = std::move(repl);
  static bool logged = false;
  if (!logged) { logged = true; Log("GTAO: replaced fragment program %s", key); }
  return true;
}
}  // namespace nvn
