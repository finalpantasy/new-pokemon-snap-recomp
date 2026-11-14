// SPDX-License-Identifier: MIT
// The visual settings whose defaults can differ from the original game, in ONE table (project code, ours).
//
// Each row has two values: the original game's ("original") and this port's extras ("enhanced"). kDefaultEnhanced picks which column is
// the default when nothing is set. The owner chose the original look as the default on 2026-11-13; flip the one constant below to change that.
// SNAP_VISUALS=original|enhanced overrides the choice for one run (all rows at once). (It was SNAP_PROFILE until 2026-11-13, which
// clashed with the save-profile name read by hle_config.cpp; SNAP_PROFILE now means only the save profile.) A value set on its own (env var, or a saved F1 menu
// choice, which settings.cpp exports as env) always wins over the profile, so every extra still works through its own switch.
//
// Readers use snapdef::get("SNAP_...") instead of getenv: it returns the env value when set (non-empty), else the profile's value.
// A code path that must know whether the user chose a value explicitly (SNAP_AA with an upscaler) keeps using getenv.
// SNAP_LOD_BIAS is not a row: "auto" already follows the render scale and is a no-op at 1x, so original and enhanced behave the same.
#pragma once
#include <cstdlib>
#include <cstring>

namespace snapdef {
constexpr bool kDefaultEnhanced = false;   // the one flip: true = ssaa2+smaa, CAS 0.3, 16x aniso, font supersampling, PNG export by default

struct Def {
  const char* key;        // env var name (the settings layer exports the F1 menu choice under this name)
  const char* original;   // the original game's behavior
  const char* enhanced;   // this port's extra
  const char* original_text;   // plain-language names for the F1 menu labels
  const char* enhanced_text;
};
inline const Def* table(int* n) {
  static const Def t[] = {
      {"SNAP_AA", "original", "ssaa2+smaa", "the game's own FXAA", "2x supersampling + SMAA"},
      {"SNAP_SHARPEN", "0.00", "0.30", "off", "0.30"},                                    // CAS sharpening strength
      {"SNAP_ANISO", "0", "16", "off, the game's own filtering", "16x"},                  // anisotropic filtering (0 = leave the game's)
      {"SNAP_UI_FONT_SCALE", "off", "auto", "off, as the game ships", "automatic: 2x at 1440p and above"},
      {"SNAP_PHOTO_EXPORT", "0", "1", "off", "on"},                                       // a PNG file per kept photo
  };
  *n = (int)(sizeof t / sizeof t[0]);
  return t;
}
inline bool enhanced() {
  const char* p = getenv("SNAP_VISUALS");
  if (p && *p) {
    if (!_stricmp(p, "enhanced")) return true;
    if (!_stricmp(p, "original")) return false;
  }
  return kDefaultEnhanced;
}
inline const Def* find(const char* key) {
  int n; const Def* t = table(&n);
  for (int i = 0; i < n; i++) if (!strcmp(t[i].key, key)) return &t[i];
  return nullptr;
}
// The profile's value for a key, ignoring the environment (what the F1 menu shows as the default). nullptr for an unknown key.
inline const char* def(const char* key) { const Def* d = find(key); return d ? (enhanced() ? d->enhanced : d->original) : nullptr; }
inline const char* def_text(const char* key) { const Def* d = find(key); return d ? (enhanced() ? d->enhanced_text : d->original_text) : ""; }
// The effective value: the env var when set and non-empty, else the profile's value.
inline const char* get(const char* key) {
  const char* e = getenv(key);
  return (e && *e) ? e : def(key);
}
}  // namespace snapdef
