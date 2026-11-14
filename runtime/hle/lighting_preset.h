// SPDX-License-Identifier: MIT
// The one parser of SNAP_LIGHTING (project code, ours). Header-only so the host mods (host/mods/gamemods.cpp) and the NVN layer
// (nvn_entry.cpp shadow resolution, nvn_draw.cpp shadow distance) read the same preset and "ultra" means the same thing everywhere.
//
//   SNAP_LIGHTING unset / default / original -> level 0 (the game's own lighting)
//   SNAP_LIGHTING=high                       -> level 1: 2048 shadow maps, 2x shadow distance, SSAO quality high
//   SNAP_LIGHTING=ultra                      -> level 2: 4096 shadow maps, 4x shadow distance, SSAO quality ultra
// Every explicit switch (SNAP_SHADOW_RES, SNAP_SHADOW_DISTANCE, SNAP_SSAO_QUALITY, ...) still wins over the preset.
#pragma once
#include <cstdlib>
#include <cstring>

namespace snaplight {
inline int level() {
  static const int v = [] {
    const char* l = getenv("SNAP_LIGHTING");
    if (!l || !*l) return 0;
    if (!_stricmp(l, "high")) return 1;
    if (!_stricmp(l, "ultra")) return 2;
    return 0;
  }();
  return v;
}
// SNAP_LIGHT_AB=<P> (test aid): the lighting features (shadow distance and fade, bias, filter, SSAO force) are on only in odd blocks of P presents,
// off in even ones: same-view A/B pairs inside one run. Unset = always on.
inline int ab_period() { static const int p = [] { const char* e = getenv("SNAP_LIGHT_AB"); return e ? atoi(e) : 0; }(); return p; }
inline bool ab_on(unsigned long long present) { const int p = ab_period(); return p <= 0 || ((present / (unsigned long long)p) & 1ull) != 0; }
inline int shadow_res() { const int l = level(); return l == 2 ? 4096 : l == 1 ? 2048 : 0; }          // 0 = the game's 1024
inline float shadow_distance() { const int l = level(); return l == 2 ? 4.0f : l == 1 ? 2.0f : 1.0f; }
inline const char* ssao_quality() { const int l = level(); return l == 2 ? "ultra" : l == 1 ? "high" : nullptr; }
}  // namespace snaplight
