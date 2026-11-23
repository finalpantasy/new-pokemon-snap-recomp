// SPDX-License-Identifier: MIT
// F1 menu option pages: Graphics, Display, Photos and Mods, plus the output-mode row of the Audio tab. One table drives every option (label, plain-language
// help, default, whether it applies now or needs a restart). Values live in the per-profile settings file (hle/settings.h); hle/settings.cpp exports them as
// the documented SNAP_* env vars at start, so an env var set by hand still wins and the page says so. Live window options go through the window API (host/window_opts.h,
// remembered in window.json), the audio output mode through snap_audio_set_output (audio.json). "Apply and restart" uses restart_game().
// Layout like the game's own options screen: options on the left, the explanation of the highlighted one on the right.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "hle/hle.h"
#include "hle/json_lite.h"
#include "hle/picture_rows.h"   // the Internal resolution / Anti-aliasing / Performance scaling rules, shared with the launcher and the game's own Settings screen
#include "hle/presets.h"        // the Original / Balanced / Enhanced / Max graphics presets (docs/presets.md), one table for the launcher, F1 and the game's Settings
#include "hle/discord_app.h"    // Discord rows only with an application id (dormant otherwise)
#include "nvn/nvn_aa.h"           // HostWindowHeight
#include "nvn/nvn_rt.h"           // FallbackNotice
namespace nvn { const char* FgNotice(); }   // nvn_framegen.cpp: why frame generation turned itself off (null while fine)
#include "hle/settings.h"
#include "hle/visual_defaults.h"   // the one table of visual defaults (original look vs extras)
#include "mods/mods_core.h"
#include "window_opts.h"
#include "ui_internal.h"
#include "ui_skin.h"
#include "ui_i18n.h"
#include "imgui_internal.h"   // NavMoveRequestCancel: Left / Right on a focused row change its value

extern "C" void snap_vsync_set(const char* mode);   // runtime/nvn/nvn_vk.cpp [vsync]: live, the next present rebuilds the swapchain
extern "C" const char* snap_vsync_get();
extern "C" const char* snap_vsync_fallback();   // "" or the present mode used instead of the chosen one
extern "C" const char* snap_gpl_state();   // runtime/nvn/nvn_vk.cpp: "on: <why>" or "off: <why>", decided at device creation
extern "C" void snap_contact_shadows_set(int on);   // runtime/nvn/nvn_contact.cpp: live, the next scene frame
extern "C" int snap_contact_shadows_get();
extern "C" int snap_live_size_blocked(void);   // runtime/nvn/nvn_retarget.cpp: bit 0 the anti-aliasing mode, bit 1 the size rows could not follow (restart)
extern "C" void snap_water_fx_set(int which, int on);   // runtime/nvn/nvn_ssr.cpp: 0 reflections, 1 caustics, 2 foam; live, the next scene frame
extern "C" int snap_water_fx_get(int which);
extern "C" void snap_hud_scale_set(const char* v);   // runtime/nvn/nvn_hud.cpp: live, the next frame ("auto", "100".."200")
extern "C" const char* snap_hud_scale_get();
extern "C" void snap_ui_smooth_set(int on);   // runtime/nvn/nvn_draw.cpp: live, the next frame (smooth interface above 60 fps)
extern "C" int snap_ui_smooth_get();

extern "C" int snap_problem_report(const char* who, char* path_out, int path_cap, char* why_out, int why_cap);   // host/mods/problem_report.cpp

namespace snap::ui {

namespace {

namespace st = hle::settings;
namespace fs = std::filesystem;

enum Kind { kSection, kChoice, kSlider, kToggle, kPath };
enum When { kLive, kRestart, kSoon };   // applies now / needs a restart / the feature is not in this build yet (control disabled)

using Choices = std::vector<std::pair<std::string, std::string>>;   // (stored value, label)

// Text that must outlive the Opt tables (they hold const char*): kept for the life of the process.
const char* keep(const std::string& s) { static std::deque<std::string> store; store.push_back(s); return store.back().c_str(); }
// "Default (<what the default is>)": read from the shared defaults table, so the label follows SNAP_VISUALS and the table's one-line flip.
std::string default_label(const char* envKey) { return std::string("Default (") + snapdef::def_text(envKey) + ")"; }

// ---- translation at draw time (the tables above stay English; see analysis/i18n/WIRING.md)
// The first "%s" of a translated format line, replaced by `a`.
std::string sub(const char* t, const std::string& a) {
  std::string r = t;
  const size_t k = r.find("%s");
  if (k != std::string::npos) r.replace(k, 2, a);
  return r;
}
// A label that may be built at run time ("Default (<what it is>)", "<size> (the game's own)", "<n> fps (custom)", "Monitor <n> <name>"): the whole text first, else the
// fixed words around the data through one format key each. `data` = text from the PC (monitor list): only the fixed words are translated.
std::string tx(const std::string& s, bool data = false) {
  struct Pat { const char* key; const char* pre; const char* suf; bool inner; };
  static const Pat pats[] = {
      {"Default (%s)", "Default (", ")", true},
      {"%s (the game's own)", "", " (the game's own)", false},
      {"%s (4K)", "", " (4K)", false},
      {"%s (8K)", "", " (8K)", false},
      {"%s (typed in)", "", " (typed in)", false},
      {"%s fps (custom)", "", " fps (custom)", false},
      {"Monitor %s", "Monitor ", "", false},
  };
  bool alpha = false;
  for (unsigned char c : s) if (std::isalpha(c)) { alpha = true; break; }
  if (!alpha) return s;
  if (data) {
    if (s == "Where the window is") return tr("Where the window is");
    if (s.rfind("Monitor ", 0) == 0) return sub(tr("Monitor %s"), s.substr(8));
    return s;
  }
  const char* t = tr(s);
  if (t != s.c_str()) return t;
  for (const Pat& p : pats) {
    const size_t a = std::strlen(p.pre), b = std::strlen(p.suf);
    if (s.size() < a + b || s.compare(0, a, p.pre) != 0 || s.compare(s.size() - b, b, p.suf) != 0) continue;
    const std::string mid = s.substr(a, s.size() - a - b);
    return sub(tr(p.key), p.inner ? std::string(tr(mid)) : mid);
  }
  return s;
}

struct Opt {
  const char* key;       // settings key, or "" for a section heading (label is its title)
  const char* label;
  Kind kind;
  When when;
  const char* help;      // plain language: what it does, what it costs
  const char* def;       // default value as stored (the game as shipped)
  Choices choices;
  float lo = 0, hi = 1;
  const char* fmt = "%.2f";
  const char* env = nullptr;                    // env var for options that do not go through settings.cpp (window options)
  std::string (*get)() = nullptr;               // custom store (window API, audio layer); null = the settings file
  void (*set)(const std::string&) = nullptr;
  Choices (*dyn)() = nullptr;                   // choices that depend on the PC (monitors)
  const char* vis = nullptr;                    // the hle/visual_defaults.h row (env name) this option's default comes from: follows the Look row live
  std::string (*note)() = nullptr;              // one line under the row: which value is in effect when another row overrides this one (picture rows)
};

float px(float v) { return v * ImGui::GetFontSize() / theme().font_px; }

// ---- custom stores
std::string get_win_mode() { return std::to_string(snap_window_get_mode()); }
void set_win_mode(const std::string& v) { snap_window_set_mode(std::atoi(v.c_str())); }
std::string get_monitor() { return std::to_string(snap_window_get_monitor()); }
void set_monitor(const std::string& v) { snap_window_set_monitor(std::atoi(v.c_str())); }
std::string get_pause() { return snap_window_get_pause_on_focus_loss() ? "1" : "0"; }
void set_pause(const std::string& v) { snap_window_set_pause_on_focus_loss(v != "0"); }
std::string get_mute() { return snap_window_get_mute_in_background() ? "1" : "0"; }
void set_mute(const std::string& v) { snap_window_set_mute_in_background(v != "0"); }
std::string get_cap() { return std::to_string(snap_window_frame_cap()); }
void set_cap(const std::string& v) { snap_window_set_frame_cap(std::atoi(v.c_str())); }
// Vsync: live through the renderer, remembered as settings key "vsync" (exported as SNAP_VSYNC at the next start; "on" = erased)
std::string get_vsync() { return snap_vsync_get(); }
// Under the row: the driver lacks the chosen mode (snap_vsync_fallback names the mode used instead, "" = as chosen).
std::string vsync_note() {
  const std::string f = snap_vsync_fallback();
  if (f.empty()) return "";
  return f == "MAILBOX" ? "Your graphics driver does not offer this mode, so Vsync Fast (MAILBOX) is used instead."
                        : "Your graphics driver does not offer this mode, so Vsync On (FIFO) is used instead.";
}
// Under the Pipeline libraries row: what is in effect this run, and why (the row only applies at the next start).
std::string gpl_note() {
  std::string s = snap_gpl_state();
  if (s.empty() || s.find("not set up") != std::string::npos) return "";
  return "This run: " + s + ".";
}
void set_vsync(const std::string& v) {
  if (v == "on") st::erase("vsync");
  else st::set("vsync", v);
  snap_vsync_set(v.c_str());
}
// Performance overlay (ui_fps.cpp): live; keys fps_overlay ("default" = Off, erased) and fps_overlay_corner ("tr" = erased)
std::string get_fps_overlay() { return snap::ui::fps_overlay_value(); }
void set_fps_overlay(const std::string& v) { snap::ui::fps_overlay_set(v); }
std::string get_fps_corner() { return snap::ui::fps_overlay_corner_value(); }
void set_fps_corner(const std::string& v) { snap::ui::fps_overlay_set_corner(v); }
// Contact shadows: live, remembered as "contact_shadows" ("default" = off, erased; exported as SNAP_CONTACT_SHADOWS=1 at the next start)
std::string get_contact() { return snap_contact_shadows_get() ? "1" : "default"; }
void set_contact(const std::string& v) {
  if (v == "1") st::set("contact_shadows", "1");
  else st::erase("contact_shadows");
  snap_contact_shadows_set(v == "1");
}
// Water effects (nvn_ssr.cpp): live, remembered as water_ssr / water_caustics / water_foam / water_ocean ("default" = off, erased; exported as SNAP_WATER_*=1 at the next start)
template <int W> std::string get_water() { return snap_water_fx_get(W) ? "1" : "default"; }
template <int W> void set_water(const std::string& v) {
  static const char* const kKey[4] = {"water_ssr", "water_caustics", "water_foam", "water_ocean"};
  if (v == "1") st::set(kKey[W], "1"); else st::erase(kKey[W]);
  snap_water_fx_set(W, v == "1");
}
std::string get_water_ssr() { return get_water<0>(); }
void set_water_ssr(const std::string& v) { set_water<0>(v); }
std::string get_water_caustics() { return get_water<1>(); }
void set_water_caustics(const std::string& v) { set_water<1>(v); }
std::string get_water_foam() { return get_water<2>(); }
void set_water_foam(const std::string& v) { set_water<2>(v); }
std::string get_water_ocean() { return get_water<3>(); }
void set_water_ocean(const std::string& v) { set_water<3>(v); }
// HUD scale: live, remembered as "hud_scale" (auto = erased; exported as SNAP_HUD_SCALE at the next start)
std::string get_hud_scale() { return snap_hud_scale_get(); }
void set_hud_scale(const std::string& v) {
  if (v.empty() || v == "auto") st::erase("hud_scale");
  else st::set("hud_scale", v);
  snap_hud_scale_set(v.c_str());
}
// Smooth interface above 60 fps (nvn_uismooth.h): live; on is the default (erased), off is remembered as "ui_smooth" = "0" (exported as SNAP_UI_SMOOTH=0 at the next start)
std::string get_ui_smooth() { return snap_ui_smooth_get() ? "default" : "0"; }
void set_ui_smooth(const std::string& v) {
  if (v == "0") st::set("ui_smooth", "0"); else st::erase("ui_smooth");
  snap_ui_smooth_set(v != "0");
}
// ---- Look: Enhanced / Original (owner 2026-11-15: new installs start Enhanced). One row flips every visual extra of hle/visual_defaults.h together:
// anti-aliasing, sharpening, anisotropic filtering, menu text sharpness, PNG export and HD photos (the Lighting preset joins when Lighting v2 passes:
// add its key to snapdef::look_keys() and its row to the visual_defaults.h table). Stored as key gfx_look ("original"; Enhanced is the erased default), exported
// to the runtime through snapdef::saved_look (hle/settings.cpp apply). Picking either value also clears the rows' own keys, so the whole bundle follows the Look.
std::string get_look() { return snapdef::enhanced() ? "enhanced" : "original"; }
void refresh_look_defaults();   // below (needs the option tables)
void set_look(const std::string& v) {
  if (v == "original") st::set("gfx_look", "original");
  else st::erase("gfx_look");
  int n = 0;
  const char* const* keys = snapdef::look_keys(&n);
  for (int i = 0; i < n; i++) st::erase(keys[i]);
  refresh_look_defaults();
}

// ---- the picture rows: Look -> Internal resolution -> Anti-aliasing -> Performance scaling (owner 2026-11-15). They are views over gfx_render_scale, gfx_aa,
// gfx_upscaler and gfx_upscale_quality, so saved settings keep working; hle/picture_rows.h has the rules and says what the runtime does with each combination.
bool beside_exe(const wchar_t* rel) {
  wchar_t path[MAX_PATH * 2] = {};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
  wchar_t* sl = wcsrchr(path, L'\\');
  if (!sl) return false;
  wcscpy(sl + 1, rel);
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
snappic::Cfg pic_cfg() {
  snappic::Cfg c;
  c.aa = st::get("gfx_aa", "");                   if (c.aa == "default") c.aa.clear();
  c.scale = st::get("gfx_render_scale", "");      if (c.scale == "auto") c.scale.clear();
  c.upscaler = st::get("gfx_upscaler", "");       if (c.upscaler == "off") c.upscaler.clear();
  c.quality = st::get("gfx_upscale_quality", ""); if (c.quality == "native") c.quality.clear();
  c.sharpen_set = !st::get("gfx_sharpen", "").empty();
  return c;
}
snappic::Ctx pic_ctx() {
  snappic::Ctx x;
  x.enhanced = snapdef::enhanced();
  x.window_h = nvn::HostWindowHeight();
  x.dlss = beside_exe(L"nvngx_dlss.dll"); x.fsr = beside_exe(L"upscalers\\amd_fidelityfx_upscaler_vk.dll"); x.xess = beside_exe(L"libxess.dll");
  return x;
}
void pic_store(const snappic::Cfg& c) {
  auto put = [](const char* k, const std::string& v) { if (v.empty()) st::erase(k); else st::set(k, v); };
  put("gfx_aa", c.aa); put("gfx_upscaler", c.upscaler); put("gfx_upscale_quality", c.quality);
}
std::string get_aa() { return snappic::aa_row(pic_cfg()); }
void set_aa(const std::string& v) { pic_store(snappic::with_aa(pic_cfg(), v)); }
std::string get_perf() { return snappic::perf_row(pic_cfg()); }
void set_perf(const std::string& v) { pic_store(snappic::with_perf(pic_cfg(), v, pic_ctx())); }
std::string note_rt_reflect() {   // the Experimental rows: why the ray traced path is not in use this session (the same text as the log line)
  if (const char* w = snap_rt::ReflectionNotice()) return std::string("Fell back to the standard reflections: ") + w;
  return "";
}
std::string note_rt_gi() {
  if (const char* w = snap_rt::GiNotice()) return std::string("Fell back to the standard bounce light: ") + w;
  const char* e = std::getenv("SNAP_RT_GI");
  const char* s = std::getenv("SNAP_SSGI");
  if (e && *e && std::atoi(e) != 0 && !(s && *s && std::atoi(s) != 0)) return "Needs the Bounce light row above to be On.";
  return "";
}
std::string note_res() { return snappic::note_resolution(pic_cfg(), pic_ctx()); }
std::string note_aa() { return snappic::note_aa(pic_cfg(), pic_ctx()); }
std::string note_perf() { return snappic::note_perf(pic_cfg(), pic_ctx()); }
std::string note_smaa() { return snappic::note_smaa(pic_cfg(), pic_ctx()); }
std::string note_sharpen() { return snappic::note_sharpen(pic_cfg()); }
Choices res_choices() {
  Choices c = {{"auto", snappic::auto_label(pic_cfg(), pic_ctx())}};
  for (const char* v : {"0.50", "0.75", "1.00", "1.25", "1.50", "2.00", "3.00", "4.00"}) {
    std::string l = snappic::dims((float)std::atof(v));
    if (!std::strcmp(v, "1.00")) l += " (the game's own)"; else if (!std::strcmp(v, "2.00")) l += " (4K)"; else if (!std::strcmp(v, "4.00")) l += " (8K)";
    c.push_back({v, l});
  }
  return c;
}
Choices aa_choices() {   // upscalers that are not installed are left out (the launcher's Install upscalers button adds them)
  const snappic::Ctx x = pic_ctx();
  const snappic::Cfg c = pic_cfg();
  const bool dlss_on = snappic::perf_row(c) != "off";
  Choices o = {{"default", x.enhanced ? "Default (SSAA + SMAA)" : "Default (FXAA)"}, {"off", "FXAA (the game's own)"}, {"smaa", "SMAA"},
               {"ssaa2", "2x supersampling + FXAA"}, {"ssaa2,smaa", "2x supersampling + SMAA"}, {"none", "None (not even the game's FXAA)"}};
  const std::string cur = snappic::aa_row(c);
  if (x.dlss || cur == "dlaa") o.push_back({"dlaa", dlss_on ? "DLSS (NVIDIA)" : "DLAA (NVIDIA)"});
  if (x.fsr || cur == "fsr") o.push_back({"fsr", "FSR (AMD)"});
  if (x.xess || cur == "xess") o.push_back({"xess", "XeSS (Intel)"});
  return o;
}
Choices perf_choices() { return {{"off", "Off"}, {"quality", "Quality"}, {"balanced", "Balanced"}, {"performance", "Performance"}}; }

// ---- quality presets. The preset is not stored: it is worked out from the eleven rows below, so changing any row by hand shows "Custom" and
// setting the rows to a preset's values shows that preset. "" in the table = the row's default (the key is erased, the game's own behavior).
// Normal is all defaults. Left out on purpose: draw distance and full-rate animation (photo scores unverified), the lighting preset and
// upscaler rows (other workers), and the HDR/color rows (not quality).
struct PresetKey { const char* key; const char* def; const char* vis = nullptr; };   // vis: the visual_defaults.h row the default comes from (follows the Look)
const PresetKey kPresetKeys[] = {
    {"gfx_render_scale", "auto"}, {"gfx_aa", "default"}, {"gfx_sharpen", "0.00", "SNAP_SHARPEN"}, {"gfx_aniso", "0", "SNAP_ANISO"}, {"gfx_lod_bias", "auto"},
    {"gfx_shadow_res", "default"}, {"ssao_quality", "default"}, {"gfx_ssao_samples", "default"}, {"gfx_ssao_hbao", "default"},
    {"gfx_pass_bloom", "default"}, {"gfx_pass_dof", "default"}, {"gfx_shadow_dist", "default"},
};
constexpr int kPresetKeyN = 12;
struct PresetDef { const char* id; const char* label; const char* v[kPresetKeyN]; };
const PresetDef kPresets[] = {
    // Very Low draws at 0.5 (960 x 540). Sub-native scale used to lose the GPU device after the photo score screen: the album thumbnail
    // readback copied a 240x200 guest region out of a 120x100 scaled image; transfers are render-scale aware since 2026-11-12 (nvn_xfer.cpp).
    // The upscaler rows stay out of the presets (they need a backend DLL and pick their own scene scale).
    // sharpening "0.00", not "": "" is the visual_defaults.h default, which is 0.30 in the Enhanced look
    // "normal" (all defaults) is labeled Default: the Look row decides what that is (Enhanced from 2026-11-15, the game's own look with Original).
    // Shadow distance (last column) doubles with the shadow resolution: the same shadow texels per meter as the game, over more ground.
    {"verylow", "Very Low", {"0.50", "none", "0.00", "0", "1", "512", "", "4", "", "off", "off", ""}},
    {"low", "Low", {"", "off", "0.00", "4", "", "", "", "8", "", "", "", ""}},
    {"normal", "Default", {"", "", "", "", "", "", "", "", "", "", "", ""}},
    {"high", "High", {"", "ssaa2,smaa", "0.30", "16", "", "2048", "high", "", "", "", "", "2"}},
    {"ultra", "Ultra", {"2.00", "smaa", "0.30", "16", "", "4096", "ultra", "", "", "", "", "4"}},
    {"maximum", "Maximum", {"4.00", "smaa", "0.50", "16", "", "8192", "ultra", "512", "32,128", "", "", "8"}},
};
const char* preset_def(int k) { return kPresetKeys[k].vis ? snapdef::def(kPresetKeys[k].vis) : kPresetKeys[k].def; }
bool preset_eq(int k, const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (k == 0 && (a == "auto" || b == "auto")) return false;   // Automatic and a typed 1.00 are different rows
  if (k == 0 || k == 2) return std::fabs((float)std::atof(a.c_str()) - (float)std::atof(b.c_str())) < 1e-4f;
  return false;
}
std::string get_preset() {
  for (const PresetDef& p : kPresets) {
    bool all = true;
    for (int k = 0; k < kPresetKeyN && all; k++) {
      const std::string want = p.v[k][0] ? p.v[k] : preset_def(k);
      all = preset_eq(k, st::get(kPresetKeys[k].key, preset_def(k)), want);
    }
    if (all) return p.id;
  }
  return "custom";
}
void set_preset(const std::string& v) {
  for (const PresetDef& p : kPresets) {
    if (v != p.id) continue;
    for (int k = 0; k < kPresetKeyN; k++) {
      if (p.v[k][0]) st::set(kPresetKeys[k].key, p.v[k]);
      else st::erase(kPresetKeys[k].key);
    }
    return;
  }
}
// ---- graphics presets (hle/presets.h): worked out from the stored rows, never stored; Custom after any single change. The Look is set through set_look so its own
// keys are cleared and the defaults table refreshed; the other rows are plain keys.
const Opt* find_opt(const std::string& key);   // below
std::string get_profile() { return snappreset::current([](const std::string& k) { return st::get(k.c_str(), ""); }); }
void set_profile(const std::string& v) {
  const int p = snappreset::from_id(v);
  if (p < 0) return;   // Custom is only ever shown
  snappreset::apply(p, [](const std::string& k, const std::string& val) {
    if (k == "gfx_look") set_look(val.empty() ? "enhanced" : val);
    else if (k.rfind("water_", 0) == 0) { if (const Opt* o = find_opt(k); o && o->set) o->set(val.empty() ? o->def : val); }   // live rows: also change what is drawn now
    else if (val.empty()) st::erase(k.c_str());
    else st::set(k.c_str(), val);
  }, pic_ctx());
}
std::string get_audio_out() {
  const int m = snap_audio_get_output_mode();
  return m == SNAP_AUDIO_OUT_SPEAKERS ? "speakers" : m == SNAP_AUDIO_OUT_SPATIAL ? "spatial" : "stereo";
}
void set_audio_out(const std::string& v) {
  snap_audio_set_output(v == "speakers" ? SNAP_AUDIO_OUT_SPEAKERS : v == "spatial" ? SNAP_AUDIO_OUT_SPATIAL : SNAP_AUDIO_OUT_STEREO, -1, -1);
}
// Dynamic range (night mode): live through the audio layer, remembered as "audio_range" ("full" = erased; exported as SNAP_AUDIO_RANGE at the next start)
std::string get_audio_range() { const int r = snap_audio_get_range(); return r == SNAP_AUDIO_RANGE_NIGHT ? "night" : r == SNAP_AUDIO_RANGE_REDUCED ? "reduced" : "full"; }
void set_audio_range(const std::string& v) {
  if (v == "full") st::erase("audio_range"); else st::set("audio_range", v);
  snap_audio_set_range(v == "night" ? SNAP_AUDIO_RANGE_NIGHT : v == "reduced" ? SNAP_AUDIO_RANGE_REDUCED : SNAP_AUDIO_RANGE_FULL);
}
// Output devices: enumerated on a worker thread at most every 10 s (COM), the last result is drawn. The stored value is the device's name.
Choices audio_devices() {
  static std::mutex mu;
  static Choices cached{{"default", "Windows default"}};
  static std::atomic<bool> busy{false};
  static double last = -100;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now - last > 10.0 && !busy.exchange(true)) {
    last = now;
    std::thread([] {
      char buf[4096];
      const int n = snap_audio_list_devices(buf, (int)sizeof buf);
      Choices c{{"default", "Windows default"}};
      const char* p = buf;
      for (int i = 0; i < n && *p; ++i) { const std::string nm = p; c.push_back({nm, nm}); p += nm.size() + 1; }
      { std::lock_guard<std::mutex> g(mu); cached = c; }
      busy = false;
    }).detach();
  }
  std::lock_guard<std::mutex> g(mu);
  return cached;
}
// The monitor list comes from EnumDisplayMonitors, which can be slow (and stalls the presenting thread) on some desktops: enumerate on a worker thread, at most
// every 10 s, and draw the last result.
Choices monitors() {
  static std::mutex mu;
  static Choices cached{{"-1", "Where the window is"}};
  static std::atomic<bool> busy{false};
  static double last = -100;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now - last > 10.0 && !busy.exchange(true)) {
    last = now;
    std::thread([] {
      const auto t0 = std::chrono::steady_clock::now();
      Choices c{{"-1", "Where the window is"}};
      for (int i = 0, n = snap_window_monitor_count(); i < n; ++i) {
        const char* nm = snap_window_monitor_name(i);
        c.push_back({std::to_string(i), std::string("Monitor ") + std::to_string(i + 1) + (nm ? std::string("  ") + nm : "")});
      }
      { std::lock_guard<std::mutex> g(mu); cached = c; }
      hle::log("ui: monitor list: %zu monitors in %lld ms", c.size() - 1, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
      busy = false;
    }).detach();
  }
  std::lock_guard<std::mutex> g(mu);
  return cached;
}

// Flip a row from kSoon to kRestart (or kLive) in this table when its feature lands; its settings key is already wired to the documented env var
// in hle/settings.cpp, so nothing else changes.
std::vector<Opt>& graphics_opts() {
  static std::vector<Opt> v = {
      {"", "Picture quality", kSection, kLive, "", ""},
      {"gfx_profile", "Graphics preset", kChoice, kRestart,
       "Sets the picture rows below together. Original: the game as it shipped. Balanced: the Enhanced look, made lighter. Enhanced: what a new install runs. Max: the highest quality; it needs a fast graphics card and CPU. Changing any single row afterwards shows Custom. Draw distance, frame generation, motion blur, HDR, color, the frame rate limit and vsync are not part of any preset.",
       "enhanced", {{"original", "Original"}, {"balanced", "Balanced"}, {"enhanced", "Enhanced"}, {"max", "Max"}, {"custom", "Custom"}}, 0, 0, "", nullptr, get_profile, set_profile},
      {"gfx_look", "Look", kChoice, kRestart,
       "Enhanced is the look new installs start with: supersampling sized to your screen (2x at 1080p, 1.5x at 1440p, none extra at 4K) with SMAA, sharpening 0.30, 16x anisotropic filtering, sharper menu text, a PNG file for every photo and HD photos. "
       "Original is the game as it shipped (its own FXAA, no sharpening or filtering, no PNG files, no HD photos). Picking one sets all of those together and clears your own choices for them; "
       "you can still change any row afterwards. Photo scores are the same either way.",
       "enhanced", {{"enhanced", "Enhanced"}, {"original", "Original"}}, 0, 0, "", "SNAP_VISUALS", get_look, set_look},
      {"gfx_preset", "Detail preset", kChoice, kRestart,
       "Sets the picture-quality rows below together: render scale, anti-aliasing, sharpening, anisotropic filtering, texture sharpness, shadow resolution and distance, and ambient occlusion. "
       "Very Low: for really weak hardware (draws the 3D scene at half size, 960 x 540, then stretches it to the window; no anti-aliasing, no filtering, softer textures, 512 shadows, few occlusion samples). "
       "Low: for weak or older graphics cards (1920 x 1080, the game's own FXAA, 4x filtering, fewer occlusion samples). Default: the Look row's own settings (Enhanced, or the game's own look with Original). "
       "High: 2x supersampling, SMAA, 2048 shadows reaching 2x as far, High occlusion. Ultra: 2x scale, 4096 shadows at 4x distance, Ultra occlusion. "
       "Maximum: 4x scale (7680 x 4320), 8192 shadows at 8x distance and the most occlusion samples. It will slow even a very fast graphics card and needs a lot of video memory. "
       "In the course the costly rows are render scale, anti-aliasing, filtering and texture sharpness; shadow size only sharpens Pokemon shadows and the occlusion rows only act on the photo-select screen. "
       "Changing any row by hand shows Custom. Draw distance and full-rate animation are not part of any preset.",
       "normal",
       {{"verylow", "Very Low"}, {"low", "Low"}, {"normal", "Default"}, {"high", "High"}, {"ultra", "Ultra"}, {"maximum", "Maximum"}, {"custom", "Custom"}}, 0, 0, "", nullptr, get_preset, set_preset},
      {"gfx_render_scale", "Internal resolution", kChoice, kLive,
       "How many pixels the 3D picture is drawn with, as a size over the game's own 1920 x 1080. Automatic lets the Look and the Anti-aliasing row decide (the Enhanced look draws it supersampled); "
       "a size sets it yourself. Higher is crisper and slower. Below 1920 x 1080 the scene is drawn smaller and stretched: blurrier, but much lighter for a weak graphics card. "
       "Photo scoring still uses the original-size data. Saved photos stay 1080 x 720 but are as sharp as the scene they were taken from.",
       "auto", {}, 0, 0, "", "SNAP_RENDER_SCALE", nullptr, nullptr, res_choices, nullptr, note_res},
      {"gfx_aa", "Anti-aliasing", kChoice, kLive,
       "Smooths jagged edges. FXAA is what the game ships with. SMAA keeps more detail. 2x supersampling draws at twice the window height first (capped at 3x the game's size): very sharp, and costs a lot of GPU time. The Enhanced look sizes it to your screen instead, so 4K stays smooth. "
       "DLAA, FSR and XeSS use an upscaler at full size; they appear when their files are installed (the launcher's Install upscalers button).",
       "default", {}, 0, 0, "", "SNAP_AA", get_aa, set_aa, aa_choices, "SNAP_AA", note_aa},
      {"gfx_smaa_quality", "SMAA quality", kChoice, kLive,
       "Only SMAA reads this. Low is the fastest and misses fine edges. Medium is a good balance. High is the default. Ultra catches the faintest edges and costs the most. Low and Medium skip diagonal and corner handling. Takes effect when SMAA is the anti-aliasing in use.",
       "high", {{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"ultra", "Ultra"}}, 0, 0, "", "SNAP_SMAA_QUALITY", nullptr, nullptr, nullptr, nullptr, note_smaa},
      {"gfx_upscale_quality", "Performance scaling", kChoice, kRestart,
       "Draws fewer pixels and enlarges them with DLSS, FSR or XeSS, so a slower graphics card keeps up. Off draws at full size. Quality draws 1.5x fewer pixels per side, Balanced about 1.7x, Performance 2x. "
       "It needs an upscaler: picking one here switches Anti-aliasing to the best one you have installed. Photos stay at native size.",
       "off", {}, 0, 0, "", "SNAP_UPSCALE_QUALITY", get_perf, set_perf, perf_choices, nullptr, note_perf},
      {"gfx_sharpen", "Sharpening", kSlider, kLive,
       "A gentle sharpening pass after anti-aliasing. 0 is off. Too much makes edges look hard.", snapdef::def("SNAP_SHARPEN"), {}, 0.0f, 1.0f, "%.2f", "SNAP_SHARPEN", nullptr, nullptr, nullptr, "SNAP_SHARPEN", note_sharpen},
      {"frame_generation", "Frame generation", kChoice, kRestart,
       "AMD FSR frame generation shows an extra, computed frame between two real ones, so a 60 fps game looks like about 120 on a 120 Hz or faster screen. The menus and HUD are kept out of it. "
       "It adds about one frame of delay, never touches photos or scoring, and turns itself off while the game runs under about 50 fps. It does not count toward the real frame rate in the Frame rate target row. If it ever fails it turns itself off for that session and says so under the options; to undo it, set Off here or remove SNAP_FRAMEGEN. "
       "AMD FSR needs the files from the launcher's Upscalers entry (a one-time download you accept). NVIDIA DLSS needs an RTX 40 or 50 card and the Streamline files in the upscalers\\streamline folder.", "default",
       {{"default", "Off"}, {"fsr", "AMD FSR"}, {"dlss", "NVIDIA DLSS"}}, 0, 0, "", "SNAP_FRAMEGEN"},
      {"", "Textures and shadows", kSection, kLive, "", ""},
      {"gfx_aniso", "Anisotropic filtering", kChoice, kLive,
       "Keeps ground and walls sharp when you look at them from a shallow angle. The original game does not force any (Off keeps its own filtering); 16x costs almost nothing on a PC GPU.",
       snapdef::def("SNAP_ANISO"), {{"0", "Off (1x)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, 0, 0, "", "SNAP_ANISO", nullptr, nullptr, nullptr, "SNAP_ANISO"},
      {"gfx_lod_bias", "Texture sharpness", kChoice, kLive,
       "Picks which texture detail level is used. Automatic sharpens textures when the render scale is above 1 and leaves the game alone at 1. Sharper values can shimmer.",
       "auto", {{"auto", "Automatic"}, {"-2", "Sharpest (-2.0)"}, {"-1.5", "-1.5"}, {"-1", "Sharper (-1.0)"}, {"-0.5", "A little sharper (-0.5)"}, {"0", "As the game ships (0)"},
                {"0.25", "+0.25"}, {"0.5", "Softer (+0.5)"}, {"1", "Much softer (+1.0)"}, {"2", "Softest (+2.0)"}}, 0, 0, "", "SNAP_LOD_BIAS"},
      {"gfx_shadow_res", "Shadow resolution", kChoice, kLive,
       "Size of the moving shadow map. The game uses 1024. Higher gives crisper shadow edges and uses more video memory.", "default",
       {{"512", "512 (fast, blocky shadows)"}, {"default", "1024 (the game's own)"}, {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192 (very heavy)"}}, 0, 0, "", "SNAP_SHADOW_RES"},
      // in kPresetKeys (2026-11-14): High/Ultra/Maximum double it with the shadow resolution, so shadow detail per meter stays the game's own
      {"gfx_shadow_dist", "Shadow distance", kChoice, kLive,
       "How far the moving shadows reach (runtime/nvn/nvn_draw.cpp ShadowDistanceOnBind). The same shadow map covers more ground, so shadows get softer; pair it with a higher shadow resolution. Costs no GPU time.", "default",
       {{"default", "1x (the game's own)"}, {"1.5", "1.5x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"6", "6x"}, {"8", "8x"}, {"12", "12x"}, {"16", "16x"}}, 0, 0, "", "SNAP_SHADOW_DISTANCE"},
      {"contact_shadows", "Contact shadows", kChoice, kLive,
       "Thin screen-space shadows where Pokemon and objects touch the ground, traced toward the sun (runtime/nvn/nvn_contact.cpp). Only frames with the sun's moving shadows get them. Off is the game's own look.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_CONTACT_SHADOWS", get_contact, set_contact},
      // foliage and particle upgrades (runtime/nvn/nvn_foliage.cpp): display only, photos and their scores keep the game's own draw; restart to apply
      {"foliage_aa", "Leaf edge smoothing", kChoice, kLive,
       "Smooths the jagged edges of leaves and grass, which the game cuts out with a hard test that anti-aliasing cannot reach. Works best with Anti-aliasing or a render scale above 1. Photos are unchanged.", "default",
       {{"default", "Off (the game's own)"}, {"1", "On"}}, 0, 0, "", "SNAP_FOLIAGE_AA"},
      {"leaf_translucency", "Leaf translucency", kChoice, kLive,
       "Sunlight glows through leaves when you look at them from the shade side. Off is the game's own look. Photos are unchanged.", "default",
       {{"default", "Off (the game's own)"}, {"0.5", "Subtle"}, {"1", "Strong"}}, 0, 0, "", "SNAP_LEAF_TRANSLUCENCY"},
      // image stability in motion (runtime/nvn/nvn_motionaa.cpp, docs/advanced-graphics-options.md): display only, photos keep the game's own draw; live
      {"specular_aa", "Specular sparkle filter", kChoice, kLive,
       "Calms the single-pixel glints that flicker on shiny or wet surfaces and Pokemon while the camera moves. Nothing is blurred. Photos are unchanged.", "default",
       {{"default", "Off (the game's own)"}, {"1", "On"}}, 0, 0, "", "SNAP_SPECULAR_AA"},
      {"gfx_pom", "Parallax detail", kChoice, kLive,
       "Gives ground, rock and bark real depth: the surface shifts with your viewing angle and its crevices shade themselves (runtime/nvn/nvn_pom.cpp). Costs GPU time on the nearest terrain. Silhouettes, collision and photo scores are unchanged. Off is the game's own look.", "default",
       {{"default", "Off (the game's own)"}, {"low", "Low"}, {"high", "High"}}, 0, 0, "", "SNAP_POM"},
      {"soft_particles", "Soft particles", kChoice, kRestart,
       "Dust, splashes and pollen fade out where they meet the ground and objects instead of cutting a hard line, and draw at full resolution. Photos are unchanged.", "default",
       {{"default", "Off (the game's own)"}, {"1", "On"}}, 0, 0, "", "SNAP_SOFT_PARTICLES"},
      // runtime/nvn/nvn_hdfx.cpp: the game's half-size and smaller effect targets grow toward the scene size; live (nvn_retarget.cpp)
      {"gfx_hd_effects", "HD effects", kChoice, kLive,
       "The sea, splashes, dust and glow are drawn at the scene's full resolution instead of half, so they stay sharp at 1440p and 4K. Photo scoring is unchanged.",
       snapdef::def("SNAP_HD_EFFECTS"), {{"0", "Off (the game's own)"}, {"1", "On"}}, 0, 0, "", "SNAP_HD_EFFECTS", nullptr, nullptr, nullptr, "SNAP_HD_EFFECTS"},
      // lens effects (runtime/nvn/nvn_lens_fx.cpp): display only, never in the photo or in anything photo scoring reads; restart to apply
      {"gfx_bokeh_dof", "Zoom bokeh", kChoice, kLive,
       "While you zoom, the background blurs into round bokeh like a telephoto lens, focused on what is under the reticle. The strength grows with the zoom and it is off when you are not zoomed. Photos stay sharp and scores are unchanged.", "default",
       {{"default", "Off (the game's own)"}, {"0.6", "Light"}, {"1", "Medium"}, {"1.6", "Strong"}}, 0, 0, "", "SNAP_BOKEH_DOF"},
      {"gfx_motion_blur", "Motion blur", kChoice, kLive,
       "Blurs the picture along the camera's motion while the NEO-ONE moves or you turn, like a real shutter. Display only: photos stay sharp and scores are unchanged. Costs a little GPU time.", "default",
       {{"default", "Off (the game's own)"}, {"0.5", "Light"}, {"1", "Medium"}, {"2", "Strong"}}, 0, 0, "", "SNAP_MOTION_BLUR"},
      {"ssao_quality", "Ambient occlusion", kChoice, kLive,
       "Soft contact shadows in corners and under objects. High and Ultra take more samples. The courses draw no ambient occlusion while you play (their contact shadows are baked in); the effect shows only on the photo-select screen, so this does not change the course view or its frame rate.", "default",
       {{"default", "Default"}, {"high", "High (96 samples)"}, {"ultra", "Ultra (256 samples)"}}, 0, 0, "", "SNAP_SSAO_QUALITY"},
      {"gfx_ssao_samples", "Occlusion samples", kChoice, kLive,
       "How many samples the simple ambient-occlusion pass takes per pixel. Overrides the Ambient occlusion choice above. Like the row above, it only matters on the photo-select screen, not in the course. 512 is the most the game's pass holds.", "default",
       {{"default", "Default (the game's own)"}, {"8", "8"}, {"16", "16"}, {"64", "64"}, {"128", "128"}, {"256", "256"}, {"512", "512"}}, 0, 0, "", "SNAP_SSAO_SAMPLES"},
      {"gfx_ssao_hbao", "Horizon occlusion", kChoice, kLive,
       "Directions and steps per direction for the horizon-based occlusion pass (used where the game selects it). More is smoother and costs more.", "default",
       {{"default", "Default (the game's own)"}, {"12,48", "12 x 48"}, {"16,64", "16 x 64"}, {"24,96", "24 x 96"}, {"32,128", "32 x 128 (very heavy)"}}, 0, 0, "", "SNAP_SSAO_HBAO"},
      {"gfx_pass_bloom", "Bloom", kChoice, kLive,
       "The soft glow around bright areas. Off skips the bloom passes: no glow is added and the picture stays sharp. Saves a little GPU time.", "default",
       {{"default", "On (the game's own)"}, {"off", "Off"}}, 0, 0, "", "SNAP_PASS_OFF"},
      {"gfx_pass_dof", "Depth of field", kChoice, kLive,
       "The blur the game puts on out-of-focus distance in some scenes. Off keeps the whole picture sharp.", "default",
       {{"default", "On (the game's own)"}, {"off", "Off"}}, 0, 0, "", "SNAP_PASS_OFF"},
      {"gfx_pass_grain", "Film grain", kChoice, kLive,
       "The film-grain noise the game lays over the picture in some scenes. Off removes it.", "default",
       {{"default", "On (the game's own)"}, {"off", "Off"}}, 0, 0, "", "SNAP_PASS_OFF"},
      {"gfx_pass_aberration", "Chromatic aberration", kChoice, kLive,
       "The red and blue color fringing at the screen edges in some scenes. Off removes it.", "default",
       {{"default", "On (the game's own)"}, {"off", "Off"}}, 0, 0, "", "SNAP_PASS_OFF"},
      {"gfx_lighting", "Lighting preset", kChoice, kLive,
       "One switch for the whole lighting look. Enhanced: soft shadows that sharpen where they touch the ground (PCSS), a shadow map that follows your render resolution at twice the reach, ambient occlusion in corners and under objects, and clean contact shadows. Maximum adds a larger shadow map, four times the reach and a finer occlusion pass. The extra passes are skipped while a photo is taken, so scoring is untouched. The separate shadow and occlusion rows above still win over the preset.", "default",
       {{"default", "Original"}, {"enhanced", "Enhanced"}, {"maximum", "Maximum"}}, 0, 0, "", "SNAP_LIGHTING"},
      // Atmosphere effects (2026-11-15, runtime/nvn/nvn_atmos.cpp, docs/atmosphere.md): six display-only pre-tonemap effects, each off by default and skipped on photo frames
      {"atmos_shafts", "Light shafts", kChoice, kLive,
       "Soft sunbeams: sky seen through gaps in leaves and between trunks is smeared toward the sun into rays and added to the picture. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_ATMOS_SHAFTS"},
      {"atmos_fog", "Height fog", kChoice, kLive,
       "Haze that gets thicker toward the ground and with distance, tinted by the sky color of the moment. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_ATMOS_FOG"},
      {"atmos_exposure", "Auto exposure", kChoice, kLive,
       "Your eyes adjust: dark caves open up and bright beaches ease down over about a second. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_ATMOS_EXPOSURE"},
      {"atmos_bloom", "Soft glow", kChoice, kLive,
       "A wide, soft glow around the brightest highlights, built from a chain of blurred copies of the picture. It is added on top of the game's own bloom and does not wash the picture out. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_ATMOS_BLOOM"},
      {"atmos_clouds", "Cloud shadows", kChoice, kLive,
       "Slow cloud shadows drift across the ground and the water, darkening the surfaces they pass over. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_ATMOS_CLOUDS"},
      {"atmos_sky", "Sky gradient", kChoice, kLive,
       "A warm glow around the sun's side of the sky, pale haze at the horizon and a deeper blue overhead. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_ATMOS_SKY"},
      // Water effects (runtime/nvn/nvn_ssr.cpp, docs/water-effects.md): live, display only, skipped on photo frames
      {"water_ssr", "Water reflections", kChoice, kLive,
       "The sea mirrors the shore, rocks and Pokemon around it (screen-space reflections). Where the screen has nothing to reflect, the game's own sky reflection stays. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_WATER_SSR", get_water_ssr, set_water_ssr},
      {"water_caustics", "Water caustics", kChoice, kLive,
       "Rippling light patterns on shallow sea floors and the rocks below the waterline. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_WATER_CAUSTICS", get_water_caustics, set_water_caustics},
      {"water_foam", "Shoreline foam", kChoice, kLive,
       "White foam where the water meets sand and rock, found from how deep the water is at each pixel. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_WATER_FOAM", get_water_foam, set_water_foam},
      {"water_ocean", "Ocean waves", kChoice, kLive,
       "Rolling open-sea waves from a wave simulation on the GPU: the reflections follow the swell and whitecaps appear on the crests. Looks best with Water reflections on. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_WATER_OCEAN", get_water_ocean, set_water_ocean},
      {"gfx_bounce", "Bounce light", kChoice, kLive,
       "Colored light that bounces off nearby surfaces: green from the grass onto a Pokemon's flank, warm from a rock onto the ground beside it. Display only; the photo itself is never changed. Costs a little GPU time.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_SSGI"},
      {"", "Experimental", kSection, kLive, "", ""},
      {"rt_reflect", "Ray traced water reflections", kChoice, kRestart,
       "Water reflections that also trace rays into the scene, so things the screen cannot show (off screen or hidden) can still reflect. Needs a ray tracing graphics card and the Water reflections row On. Experimental: the gain is small on most views. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_RT_REFLECT", nullptr, nullptr, nullptr, nullptr, note_rt_reflect},
      {"rt_gi", "Ray traced bounce light", kChoice, kRestart,
       "Bounce light gathered with rays instead of the screen, so geometry off screen or behind a surface can tint it. Needs a ray tracing graphics card and the Bounce light row On. Experimental: the difference is subtle and it costs about 0.7 ms more per frame at 1440p. Never in photos, so scores do not change.", "default",
       {{"default", "Off"}, {"1", "On"}}, 0, 0, "", "SNAP_RT_GI", nullptr, nullptr, nullptr, nullptr, note_rt_gi},
      {"rt_gi_rays", "Ray traced bounce light quality", kChoice, kRestart,
       "Rays per pixel for the ray traced bounce light. More rays look less grainy and cost more: about 1.3 ms for one, 1.8 for two and 3.2 for four at 1440p.", "default",
       {{"default", "1 ray (fastest)"}, {"2", "2 rays"}, {"4", "4 rays"}}, 0, 0, "", "SNAP_RT_GI_RAYS"},
      {"", "Color", kSection, kLive, "", ""},
      {"look", "Color look", kChoice, kLive,
       "A color grade on the finished picture. Neutral leaves the game as shipped.", "neutral",
       {{"neutral", "Neutral (as shipped)"}, {"vivid", "Vivid"}, {"warm", "Warm"}, {"cool", "Cool"}}, 0, 0, "", "SNAP_LOOK"},
      {"look_intensity", "Look strength", kSlider, kLive, "How strongly the color look is applied. Has no effect while the look is Neutral.", "1.00", {}, 0.0f, 1.5f, "%.2f", "SNAP_LOOK"},
      // HDR mode (2026-11-14): was an on/off toggle that stored 1, which nvn_hdr.cpp reads as hdr10, so old saves keep working
      {"gfx_hdr", "HDR output", kChoice, kLive,
       "Sends a high-dynamic-range picture to an HDR monitor. Turn Windows HDR on first. On a normal monitor leave this off. scRGB (16-bit float) has far less banding than HDR10; if the display refuses it the game falls back to HDR10, then to SDR.", "0",
       {{"0", "Off"}, {"1", "HDR10"}, {"scrgb", "scRGB"}}, 0, 0, "", "SNAP_HDR"},
      // Auto = the key erased: the monitor's reported peak, else 1000 (nvn_hdr.cpp). The old slider stored a value on every save.
      {"gfx_hdr_peak", "HDR peak brightness", kChoice, kLive,
       "The brightest highlight to send, in nits. Auto uses the peak the monitor reports (1000 if it reports none); otherwise use your monitor's rated peak.", "default",
       {{"default", "Auto (the monitor's)"}, {"400", "400 nits"}, {"600", "600 nits"}, {"800", "800 nits"}, {"1000", "1000 nits"}, {"1500", "1500 nits"}, {"2000", "2000 nits"},
        {"3000", "3000 nits"}, {"4000", "4000 nits"}}, 0, 0, "", "SNAP_HDR_PEAK"},
      {"gfx_hdr_paper", "HDR paper white", kSlider, kLive,
       "How bright ordinary white (menus, snow) is, in nits. Lower it if the picture feels too bright in a dark room.", "200", {}, 80.0f, 400.0f, "%.0f nits", "SNAP_HDR_PAPER"},
      {"gfx_hdr_expand", "HDR highlights", kChoice, kLive,
       "How far near-white scene pixels are lifted above paper white into HDR highlights (0 = none, 1 = up to the peak). Needs HDR on.", "0.30",
       {{"0.00", "0"}, {"0.10", "0.1"}, {"0.20", "0.2"}, {"0.30", "0.3 (default)"}, {"0.40", "0.4"}, {"0.50", "0.5"}, {"0.60", "0.6"}, {"0.80", "0.8"}, {"1.00", "1"}}, 0, 0, "",
       "SNAP_HDR_EXPAND"},
      {"", "View", kSection, kLive, "", ""},
      // Field of view row removed (owner 2026-11-12: "turn off fov, hide the option, stop working on it for now"; it changes photo
      // scores). SNAP_FOV still works as a developer env switch, off by default.
      {"gfx_distant_scenery", "Distant scenery", kChoice, kLive,
       "Sharper far-away scenery and Pokemon: models keep their detailed versions farther out and are drawn farther out. High is 3 times the game's detail distance and 2 times its "
       "draw distance, Ultra 8 and 4 times. Display only: spawns, Pokemon behavior and photo scoring keep the game's own picks. Costs some speed. The two rows below can push either one further.",
       "off", {{"off", "Original"}, {"high", "High"}, {"ultra", "Ultra"}}, 0, 0, "", "SNAP_DISTANT_SCENERY"},
      {"gfx_lod_dist", "Detail distance", kChoice, kLive,
       "How far out scenery and Pokemon keep their detailed models. High is 3 times the game's distance, Max 8 times. Display only: photo scoring "
       "keeps the game's own models. Costs some speed.",
       "1", {{"1", "Original"}, {"3", "High"}, {"8", "Max"}}, 0, 0, "", "SNAP_LOD_DIST"},
      {"gfx_lod_force0", "Always full detail", kToggle, kLive,
       "Every model that is drawn uses its most detailed version at any distance. Costs speed.", "0", {}, 0, 0, "", "SNAP_LOD_FORCE0"},
      {"gfx_draw_dist", "Draw distance", kSlider, kLive,
       "Pushes out the distance where scenery and Pokemon stop being drawn (1 to 8, 1 is the game's own). May change photo scores, being tested.",
       "1", {}, 1.0f, 8.0f, "%.1fx", "SNAP_DRAW_DIST"},
      {"gfx_full_rate_anim", "Full-rate distant animation", kToggle, kLive,
       "Far-away Pokemon animate every 2nd or 4th frame in the original. On animates them every frame: smoother, a little more CPU. May change photo scores, being tested.", "0", {}, 0, 0, "", "SNAP_FULL_RATE_ANIM"},
      {"", "Frame rate", kSection, kLive, "", ""},
      {"fps_target", "Frame rate target", kChoice, kLive,
       "How fast the game runs. Default is the game's own 60 FPS. Higher rates run the game itself faster (real frames, not interpolated) and need a fast CPU; "
       "rides, Pokemon, scores, menus and dialogue keep their original speed. Match monitor uses your monitor's refresh rate; Unlocked runs as fast as the PC allows. "
       "Any rate from 61 to 1000 also works with --fps <n> or fps_target=<n> in settings.cfg. The frame rate limit below still applies. "
       "On a variable refresh rate (G-Sync, FreeSync) monitor any of them is smooth; on a fixed-refresh monitor pick the refresh rate or Match monitor.", "default",
       {{"default", "60 (the game's own)"}, {"72", "72"}, {"90", "90"}, {"100", "100"}, {"120", "120"}, {"144", "144"}, {"165", "165"}, {"180", "180"}, {"200", "200"},
        {"240", "240"}, {"300", "300"}, {"360", "360"}, {"480", "480"}, {"monitor", "Match monitor"}, {"unlocked", "Unlocked"}}, 0, 0, "", "SNAP_FPS"},
      {"frame_cap", "Frame rate limit", kChoice, kLive,
       "Stops the game from drawing faster than this. Off keeps the game's own 60 FPS pacing. Remembered in window.json.", "0",
       {{"0", "Off (the game's pacing)"}, {"30", "30"}, {"45", "45"}, {"60", "60"}, {"75", "75"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
        {"180", "180"}, {"200", "200"}, {"240", "240"}, {"360", "360"}}, 0, 0, "",
       "SNAP_WINDOW_FRAME_CAP", get_cap, set_cap},
      {"vsync", "Vsync", kChoice, kLive,
       "On: no tearing (the default). Adaptive: tears only while below the refresh rate. Fast: no tearing and lower latency, renders extra frames. "
       "Off: lowest latency, may tear. The game's own frame pacing is unchanged.", "on",
       {{"on", "On"}, {"adaptive", "Adaptive"}, {"fast", "Fast"}, {"off", "Off"}}, 0, 0, "", "SNAP_VSYNC", get_vsync, set_vsync, nullptr, nullptr, vsync_note},
      // runtime/nvn/nvn_vk.cpp (device setup), nvn_draw.cpp (library fast link), nvn_pipeline.cpp (background optimize); driver list: nvn_quirks.cpp
      {"gfx_gpl", "Pipeline libraries", kChoice, kRestart,
       "Experimental. A graphics pipeline the game needs before its shader cache is warm is linked from graphics pipeline libraries while the full version compiles in the "
       "background. On the one GPU measured (NVIDIA RTX 4090) it did not reduce stutter and doubled the compile work, so Automatic is Off on every GPU for now; On is for testing. "
       "Pictures, photos and scores are identical either way.", "default",
       {{"default", "Automatic"}, {"on", "On"}, {"off", "Off"}}, 0, 0, "", "SNAP_GPL", nullptr, nullptr, nullptr, nullptr, gpl_note},
      {"ui_smooth", "Smooth interface", kChoice, kLive,
       "Above 60 FPS the menus and the frame and reticle over a Pokemon step 60 times a second, as the game does. On draws them every displayed frame, "
       "moving a moving element a fraction of a step ahead along its own motion, with no added delay. Display only: the game, the score and your photos are unchanged. "
       "Does nothing at 60 FPS.", "default",
       {{"default", "On"}, {"0", "Off"}}, 0, 0, "", "SNAP_UI_SMOOTH", get_ui_smooth, set_ui_smooth},
      {"fps_overlay", "Performance overlay", kChoice, kLive,
       "Off is the original game. FPS: the frame rate in a corner. Detailed adds frame time and a frame-time graph, 1% low, CPU and GPU time, "
       "render and output size, upscaler and anti-aliasing, frame cap and vsync. F3 cycles it (unless F3 is bound to a game button). "
       "It is drawn over the finished picture, so it never appears in your photos.", "default",
       {{"default", "Off"}, {"fps", "FPS"}, {"detailed", "Detailed"}}, 0, 0, "", "SNAP_FPS_OVERLAY", get_fps_overlay, set_fps_overlay},
      {"fps_overlay_corner", "Overlay corner", kChoice, kLive,
       "Where the performance overlay sits.", "tr",
       {{"tl", "Top left"}, {"tr", "Top right"}, {"bl", "Bottom left"}, {"br", "Bottom right"}}, 0, 0, "", "SNAP_FPS_OVERLAY_CORNER",
       get_fps_corner, set_fps_corner},
  };
  return v;
}

std::vector<Opt>& display_opts() {
  static std::vector<Opt> v = {
      {"", "Window", kSection, kLive, "", ""},
      {"win_mode", "Window mode", kChoice, kLive,
       "Windowed or borderless fullscreen (F11 and Alt+Enter switch as well). Remembered in window.json.", "0",
       {{"0", "Windowed"}, {"1", "Borderless fullscreen"}}, 0, 0, "", "SNAP_WINDOW_MODE", get_win_mode, set_win_mode},
      {"win_monitor", "Monitor", kChoice, kLive,
       "Which screen the borderless window covers. A windowed game stays where you put it.", "-1", {}, 0, 0, "", "SNAP_WINDOW_MONITOR", get_monitor, set_monitor, monitors},
      {"win_size", "Windowed size", kChoice, kRestart,
       "The size of the picture area when the game is windowed. Without a choice the game remembers the size you dragged it to. Output resolution of the finished picture follows the window.",
       "", {{"", "Remembered"}, {"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"}, {"1920x1080", "1920 x 1080"}, {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160"}}, 0, 0, "", "SNAP_WINDOW_W"},
      {"gfx_ultrawide", "Ultrawide", kChoice, kLive,
       "On a screen wider than 16:9, Auto shows more of the world at the sides with the same height of view. Menus, the interface and your photos stay 16:9 in the middle. Off shows the 16:9 picture with side bars.",
       "auto", {{"auto", "Auto (wider 3D view)"}, {"off", "Off (16:9 with side bars)"}}, 0, 0, "", "SNAP_ULTRAWIDE"},
      {"hud_scale", "HUD scale", kChoice, kLive,
       "Makes the viewfinder HUD, menus and dialogue bigger on large screens. The 3D view and your photos are not changed. Auto is 100%: pick a size to scale. Crowded screens stay smaller so nothing leaves the picture.",
       "auto", {{"auto", "Auto (100%)"}, {"100", "100%"}, {"125", "125%"}, {"150", "150%"}, {"175", "175%"}, {"200", "200%"}}, 0, 0, "", "SNAP_HUD_SCALE", get_hud_scale, set_hud_scale},
      {"", "Text", kSection, kLive, "", ""},
      {"gfx_ui_font", "Menu text sharpness", kChoice, kRestart,
       "The game's own menus and labels are drawn from a low-resolution letter sheet. Higher draws the letters sharper on large screens, at the same size and position. Automatic uses 2x when the game window is 1440 lines tall or more.", "default",
       {{"default", default_label("SNAP_UI_FONT_SCALE")}, {"auto", "Automatic (2x at 1440p and above)"}, {"1", "Off (as the game ships)"}, {"2", "Sharper (2x)"}, {"3", "Sharper (3x)"}, {"4", "Sharpest (4x)"}}, 0, 0, "", "SNAP_UI_FONT_SCALE", nullptr, nullptr, nullptr, "SNAP_UI_FONT_SCALE"},
      {"", "Controller", kSection, kLive, "", ""},
      {"button_icons", "Button icons", kChoice, kLive,
       "Which button pictures the game's prompts show. Automatic shows key caps while no controller is connected or the last input came from the keyboard or mouse, and follows the controller otherwise (PlayStation, Xbox, Switch Pro). The picture is the button you press with your current controls, so a remapped button shows its new picture. Menus and hints change at once. Cosmetic only.", "auto",
       {{"auto", "Automatic (matches the controller)"}, {"switch", "Switch (as the game ships)"}, {"xbox", "Xbox"}, {"playstation", "PlayStation"}, {"keyboard", "Keyboard and mouse"}}, 0, 0, "", "SNAP_BUTTON_ICONS"},
      // docs/player-options.md: controller only (the keyboard keeps its keys); the prompts' pictures follow. input.json "controller.swap_ab" is the base value this row overrides.
      {"swap_ab", "Swap A and B", kToggle, kLive,
       "Swaps confirm and back on the controller: the south button confirms and the east button goes back, like on an Xbox or PlayStation pad. The game's button pictures follow. The keyboard is not changed.", "0", {}, 0, 0, "", "SNAP_SWAP_AB"},
      {"", "Discord", kSection, kLive, "", ""},
      // docs/player-options.md: host/mods/discord_presence.cpp talks to the Discord app on this PC over its local pipe; nothing else is contacted.
      {"discord_presence", "Discord Rich Presence", kToggle, kLive,
       "Shows the course you are riding and what you are doing on your Discord profile while the game runs. It talks only to the Discord app on this PC. Off by default.", "0", {}, 0, 0, "", "SNAP_DISCORD_PRESENCE"},
      {"", "Focus", kSection, kLive, "", ""},
      {"win_pause", "Pause when unfocused", kToggle, kLive,
       "Freezes the game while you are in another window, so nothing happens while you are away. Remembered in window.json.", "1", {}, 0, 0, "", "SNAP_WINDOW_PAUSE", get_pause, set_pause},
      {"win_mute", "Mute in the background", kToggle, kLive,
       "Silences the game while its window is not in front.", "1", {}, 0, 0, "", "SNAP_WINDOW_MUTE_BG", get_mute, set_mute},
      {"", "Start-up", kSection, kLive, "", ""},
      {"fast_boot", "Fast boot", kToggle, kRestart,
       "Skips the frame limiter during the first minute of loading, so the title screen comes up sooner (about 16 s faster here). Gameplay is not affected.", "0", {}, 0, 0, "", "SNAP_FAST_BOOT"},
      {"", "Language", kSection, kLive, "", ""},
      // docs/player-options.md. Only the languages the game ships text for; Auto maps the Windows display language to the closest one (else English).
      {"game_language", "Game language", kChoice, kRestart,
       "The language of the game's own menus and text. Auto follows your Windows display language and falls back to English. Voices stay as chosen in the game.", "auto",
       {{"auto", "Auto (Windows language)"}, {"en", "English"}, {"ja", "Japanese"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"}, {"es", "Spanish"}, {"ko", "Korean"},
        {"zh-hans", "Chinese (Simplified)"}, {"zh-hant", "Chinese (Traditional)"}}, 0, 0, "", "SNAP_GAME_LANGUAGE"},
  };
  // docs/player-options.md: Discord Rich Presence is dormant until an application id is set (runtime/hle/discord_app.h), so with none its section and row are not shown.
  static const bool pruned = [] {
    if (snapdiscord::app_id(hle::settings::get("discord_app_id", "")).empty())
      v.erase(std::remove_if(v.begin(), v.end(), [](const Opt& o) { return std::strcmp(o.key, "discord_presence") == 0 || (o.kind == kSection && std::strcmp(o.label, "Discord") == 0); }), v.end());
    return true;
  }();
  (void)pruned;
  return v;
}

std::vector<Opt>& photo_opts() {
  static std::vector<Opt> v = {
      {"", "Photo files", kSection, kLive, "", ""},
      {"photo_export", "Save a PNG of every photo", kToggle, kRestart,
       "Each photo you keep is also written as a full-quality PNG file you can share, next to the game's own in-save copy. The save and scoring are not affected.", snapdef::def("SNAP_PHOTO_EXPORT"), {}, 0, 1, "%.2f", nullptr, nullptr, nullptr, nullptr, "SNAP_PHOTO_EXPORT"},
      {"photo_dir", "PNG folder", kPath, kRestart,
       "Where the PNG files go. Empty uses %APPDATA%\\SnapRecomp\\photos. The folder is created when the first photo is saved.", ""},
      {"hd_photos", "HD photos", kToggle, kRestart,
       "With a render scale or upscaler at 4K, photo select and the album show each photo at 2160x1440 instead of the game's small copy. The game's own photo, save and scoring are unchanged. Uses more disk (about 1 to 2 MB per saved photo, in the save folder's photos_hd) and more GPU memory (up to 256 MB).", snapdef::def("SNAP_HD_PHOTOS"), {}, 0, 1, "%.2f", nullptr, nullptr, nullptr, nullptr, "SNAP_HD_PHOTOS"},
      // runtime/nvn/nvn_hdphoto.cpp (SNAP_HIRES_PHOTOS), read at start; opt-in
      {"hires_photos", "Full-size photo PNGs", kToggle, kRestart,
       "With HD photos on, each photo the game saves to the album is also written as a lossless PNG at the full render height in the photo's 3:2 shape (4860x3240 with Enhanced at 4K), named like its HD copy in the save folder's photos_hd. The game's photo, its save and scoring are unchanged. About 20 to 30 MB per photo.", "0", {}, 0, 0, "", "SNAP_HIRES_PHOTOS"},
      // runtime/nvn/nvn_hdrphoto.cpp (SNAP_HDR_PHOTOS), read at start; opt-in
      {"hdr_photos", "HDR photo copies", kToggle, kRestart,
       "Needs HDR output (Graphics tab). Each HD photo copy and each screenshot is also written as a 16-bit HDR PNG (PQ, BT.2020, with brightness data) next to the normal file. It rebuilds the bright parts from the normal picture the same way the HDR display does; it does not record the scene's real light. With HDR output off nothing is written. The game's photo, its save and scoring are unchanged.", "0", {}, 0, 0, "", "SNAP_HDR_PHOTOS"},      {"", "Expedition", kSection, kLive, "", ""},
      {"neo_speed", "NEO-ONE speed", kSlider, kRestart,
       "How fast the NEO-ONE rides through a course (0.5 to 3, 1 is the game's own). The ride is shorter or longer, so you see less or more of each scene; Pokemon behave as before. Photos and scoring are not changed.",
       "1", {}, 0.5f, 3.0f, "%.2fx", "SNAP_NEO_SPEED"},
      {"film", "Photos per expedition", kChoice, kRestart,
       "How many photos you can take before the expedition ends. The game gives 72 on most courses. Photo selection lists every shot you took.", "default",
       {{"default", "Game's own (72)"}, {"100", "100"}, {"150", "150"}, {"200", "200"}, {"300", "300"}, {"500", "500"}}, 0, 0, "", "SNAP_FILM"},
      // runtime/nvn/nvn_shot.cpp reads both keys at each shot (SNAP_SHOT_DIR / SNAP_SHOT_HUD win when set)
      {"", "Screenshots", kSection, kLive, "", ""},
      {"shot_dir", "Screenshot folder", kPath, kLive,
       "F12 (or View + R3 on a controller) saves a lossless PNG of the frame at the full render size, before it is scaled to your screen: 5760x3240 with Enhanced at 4K. Empty uses Pictures\\Snap Recomp. The folder is created with the first shot.", "", {}, 0, 0, "", "SNAP_SHOT_DIR"},
      {"shot_hud", "Include the HUD", kToggle, kLive,
       "Off saves the 3D scene without the game's HUD (the HUD stays on screen). On saves the frame as shown. Menus and loading screens are always saved as shown.", "0", {}, 0, 0, "", "SNAP_SHOT_HUD"},
      {"", "Free camera", kSection, kLive, "", ""},
      {"freecam", "Free camera", kToggle, kRestart,
       "During a ride, F6 (or both stick clicks on a controller) lets you fly the camera anywhere while the NEO-ONE waits on its rail. WASD and the mouse, or both sticks. Space and C go up and down, Shift is fast, mouse wheel changes speed, Z and X (or ZL and ZR) zoom, F9 (or Y) saves a clean PNG with no HUD. The photo shutter is off while it is on, so no photo can be scored from an off-rail angle. Press F6 again to go back to the ride.", "1", {}, 0, 0, "", "SNAP_FREECAM"},
      {"freecam_freeze", "Freeze the world in free camera", kToggle, kRestart,
       "Starts every free camera with the Pokemon and the scene stopped, for still shots. F7 (or X on a controller) switches it while flying. Off keeps everything moving.", "0", {}, 0, 0, "", "SNAP_FREECAM_FREEZE"},
      {"", "Skips", kSection, kLive, "", ""},
      {"skip_cutscenes", "Skip dialogue", kToggle, kRestart,
       "Presses A for you whenever a cutscene or professor dialogue box is waiting, so the lines go by without button presses. Choices are not answered for you. Story progress is not changed.", "0", {}, 0, 0, "", "SNAP_SKIP_CUTSCENES"},
      {"skip_course_intro", "Skip course intro", kToggle, kRestart,
       "Plays the fixed fly-over at the start of every course about 8 times faster, so the ride starts after a second or two. Rides, Pokemon and photo scores are not changed.", "0", {}, 0, 0, "", "SNAP_SKIP_INTRO"},
      // opt-in, off by default (runtime/nvn/nvn_shiny.cpp reads the switches once at start); settings.cpp turns the key into
      // SNAP_SHINY=1 + SNAP_SHINY_ODDS=<n>. Needs the shiny pack (scripts/hd/make_shiny_pack.py). No forced/test choice here.
      {"shiny", "Shiny Pokemon", kChoice, kRestart,
       "Very rarely, a Pokemon appears in its shiny colors. Cosmetic only; photo scores are unchanged. Needs the shiny texture pack.", "default",
       {{"default", "Off"}, {"4096", "Rare (1 in 4096)"}, {"512", "Common (1 in 512)"}}, 0, 0, "", "SNAP_SHINY"},
      {"", "Spawn randomizer", kSection, kLive, "", ""},
      // opt-in, off by default. The game files are untouched: scripts/mods/make_spawn_randomizer.py (run by host\play.ps1 at start,
      // or by hand) writes the "spawn-randomizer" mod pack from these two keys; see docs/spawn-randomizer.md.
      {"spawn_rand", "Spawn randomizer", kChoice, kRestart,
       "Shuffles which Pokemon appear at the spawn points of each course, from the seed below. Within course reuses a course's own Pokemon; All courses can put any Pokemon anywhere. Story and scripted Pokemon stay where they are. Photo scores work as usual. The pack is built when the game is started with host\\play.ps1, or by running scripts\\mods\\make_spawn_randomizer.py.", "default",
       {{"default", "Off"}, {"within", "Within course"}, {"all", "All courses"}}, 0, 0, "", "SNAP_SPAWN_RAND"},
      {"spawn_seed", "Randomizer seed", kPath, kRestart,
       "Any text. The same seed and mode give the same course layouts on every PC, so you can share it. Empty uses the seed \"snap\".", ""},
  };
  return v;
}

std::vector<Opt>& audio_opts() {
  static std::vector<Opt> v = {
      {"audio_output", "Output mode", kChoice, kRestart,
       "Stereo is the game's own mix. Speakers sends 5.1 or 7.1 to a surround system (the stereo mix is spread around you). Spatial uses Windows Spatial Sound (Dolby Atmos, Windows Sonic) for headphones. The game opens its sound output once, so this needs a restart. Saved in audio.json.",
       "stereo", {{"stereo", "Stereo"}, {"speakers", "Speakers (5.1 / 7.1)"}, {"spatial", "Spatial (headphones)"}}, 0, 0, "", "SNAP_AUDIO_OUTPUT", get_audio_out, set_audio_out},
      {"audio_device", "Output device", kChoice, kRestart,
       "Which sound device the game plays on. Windows default follows the device Windows uses (a headset you plug in takes over). A named device stays on that device; if it is unplugged the game uses the Windows default until it comes back.",
       "default", {}, 0, 0, "", nullptr, nullptr, nullptr, audio_devices},
      {"audio_latency", "Audio latency", kChoice, kRestart,
       "Normal suits most PCs. Low gives the shortest delay between the game and your speakers but needs a free CPU. Safe uses a bigger buffer: pick it if you hear crackle or dropouts, or use Bluetooth.",
       "normal", {{"low", "Low"}, {"normal", "Normal"}, {"safe", "Safe (fewer dropouts)"}}},
      {"audio_resampler", "Sample-rate conversion", kChoice, kRestart,
       "The game's sound is 48 kHz. If your device runs at 44.1 or 96 kHz, Windows converts it (Windows default). Sinc uses the game's own high-quality converter, clean up to 20 kHz; Linear is lighter and a little duller. No effect on 48 kHz devices.",
       "system", {{"system", "Windows default"}, {"sinc", "High quality (sinc)"}, {"linear", "Linear"}}},
      {"audio_range", "Dynamic range", kChoice, kLive,
       "Full is the game's own mix. Reduced narrows the gap between loud and quiet a little. Night mode narrows it a lot, so quiet sounds stay audible and loud ones do not wake the house.",
       "full", {{"full", "Full"}, {"reduced", "Reduced"}, {"night", "Night mode"}}, 0, 0, "", "SNAP_AUDIO_RANGE", get_audio_range, set_audio_range},
  };
  return v;
}

// Accessibility rows (hle/access.h, docs/accessibility.md): every one defaults to off. They are stored as settings keys; hle::settings::apply pushes them into
// hle::access, so a change shows on the next frame, in the game and in the launcher.
std::vector<Opt>& access_opts() {
  static std::vector<Opt> v = {
      {"", "Vision", kSection, kLive, "", ""},
      {"access_cvd", "Color vision correction", kChoice, kLive,
       "Shifts colors that are hard to tell apart into ones you can see, over the whole game picture including the HUD and menus. Pick the type that matches your color vision. This menu is not corrected, and photos, the Evaluation and saved screenshots are not changed.",
       "off", {{"off", "Off"}, {"protanopia", "Protanopia (red-blind)"}, {"deuteranopia", "Deuteranopia (green-blind)"}, {"tritanopia", "Tritanopia (blue-yellow)"}}},
      {"access_cvd_strength", "Correction strength", kChoice, kLive,
       "How much of the correction is applied: 10% is a light touch, 100% the full shift.", "100",
       {{"10", "10%"}, {"20", "20%"}, {"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"}, {"80", "80%"}, {"90", "90%"}, {"100", "100%"}}},
      {"access_flash", "Reduce flashing", kChoice, kLive,
       "Limits how fast the whole picture can get brighter or darker from one frame to the next, so Illumina Orb bursts, the camera flash and other bright flashes fade in over a few frames instead of snapping. Gentle changes little, Strong spreads a flash over about half a second.",
       "off", {{"off", "Off"}, {"gentle", "Gentle"}, {"standard", "Standard"}, {"strong", "Strong"}}},
      {"", "This menu and the launcher", kSection, kLive, "", ""},
      {"access_ui_size", "Text and menu size", kChoice, kLive,
       "Makes the launcher and this menu bigger. It multiplies the Menu size slider above. The game's own screens are not changed (see the HUD size row in Display).",
       "100", {{"100", "100%"}, {"125", "125%"}, {"150", "150%"}, {"200", "200%"}}},
      {"access_contrast", "High-contrast menus", kToggle, kLive,
       "Black panels with white text and a bright outline, for this menu and the launcher.", "0"},
      {"", "Sound", kSection, kLive, "", ""},
      {"access_mono", "Mono audio", kToggle, kLive,
       "Plays the same sound in both ears. Useful if you hear well in only one ear or use a single speaker.", "0"},
  };
  return v;
}

// After the Look flips: every option that follows a visual_defaults.h row takes that row's new default (and the "Default (...)" label of the choice rows).
void refresh_look_defaults() {
  for (auto* list : {&graphics_opts(), &display_opts(), &photo_opts()})
    for (Opt& o : *list) {
      if (!o.vis) continue;
      if (!std::strcmp(o.def, "default")) { if (!o.choices.empty() && o.choices[0].first == "default") o.choices[0].second = default_label(o.vis); }
      else o.def = snapdef::def(o.vis);
    }
}

// ---- value access
std::string cur_of(const Opt& o) { return o.get ? o.get() : st::get(o.key, o.def); }

bool same_value(const Opt& o, const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (o.kind == kSlider) return std::fabs((float)std::atof(a.c_str()) - (float)std::atof(b.c_str())) < 1e-4f;
  return false;
}

void set_of(const Opt& o, const std::string& v) {
  if (o.set) { o.set(v); return; }
  if (same_value(o, v, o.def)) st::erase(o.key);   // the default is "not set": the env var is left alone and the game keeps its own behavior
  else st::set(o.key, v);
}

std::string fmt_float(float f) { char b[32]; std::snprintf(b, sizeof b, "%.2f", f); return b; }

bool env_set_by_hand(const Opt& o) {
  if (o.set && o.when == kLive) return false;   // changed through the API at once; the env var only matters at the next start
  if (st::env_name(o.key)) return st::env_wins(o.key);
  if (o.env) { const char* e = std::getenv(o.env); return e && *e; }
  return false;
}

// ---- restart tracking: the saved value of every restart option when the game started, compared with now
std::map<std::string, std::string> g_start;
std::string g_mods_start;
bool g_started = false;

std::string mods_state();   // below
void mods_reload();

void for_each_opt(void (*f)(const Opt&, void*), void* u) {
  for (auto* list : {&graphics_opts(), &display_opts(), &photo_opts(), &audio_opts()})
    for (const Opt& o : *list) if (o.kind != kSection) f(o, u);
}

void capture_start() {
  if (g_started) return;
  g_started = true;
  for_each_opt([](const Opt& o, void*) { g_start[o.key] = cur_of(o); }, nullptr);
  mods_reload();
  g_mods_start = mods_state();
}

// The size rows (Internal resolution, Anti-aliasing, Shadow resolution, Ultrawide) apply with no restart (runtime/nvn/nvn_retarget.cpp) except where
// the running game cannot follow: an upscaler or frame generation in use, or a switch to or from an upscaler or the game's own FXAA. Those
// changes keep the restart note.
bool live_blocked(const Opt& o) {
  const bool aa = !std::strcmp(o.key, "gfx_aa");
  if (!aa && std::strcmp(o.key, "gfx_render_scale") && std::strcmp(o.key, "gfx_shadow_res") && std::strcmp(o.key, "gfx_ultrawide")) return false;
  if (env_set_by_hand(o)) return false;
  auto it = g_start.find(o.key);
  if (it == g_start.end()) return false;
  const std::string cur = cur_of(o);
  if (same_value(o, it->second, cur)) return false;
  const int b = snap_live_size_blocked();
  if (b & 2) return true;
  if (!aa) return false;
  auto up = [](const std::string& v) { return v == "dlaa" || v == "fsr" || v == "xess"; };
  return (b & 1) || up(it->second) || up(cur);
}

struct Pend { std::vector<std::string> names; };

void pend_one(const Opt& o, void* u) {
  if (live_blocked(o)) { ((Pend*)u)->names.push_back(tr(o.label)); return; }
  if (o.when != kRestart || env_set_by_hand(o)) return;
  auto it = g_start.find(o.key);
  if (it != g_start.end() && !same_value(o, it->second, cur_of(o))) ((Pend*)u)->names.push_back(tr(o.label));
}

// look strength has no effect on a neutral look: do not count it alone
bool is_pending(const Opt& o) {
  if (live_blocked(o)) return true;
  if (o.when != kRestart || env_set_by_hand(o)) return false;
  auto it = g_start.find(o.key);
  if (it == g_start.end() || same_value(o, it->second, cur_of(o))) return false;
  if (!std::strcmp(o.key, "look_intensity") && st::get("look", "neutral") == "neutral" && g_start["look"] == "neutral") return false;
  return true;
}

// ---- row drawing
const Opt* g_help = nullptr;   // the option under the pointer or the keyboard/pad focus this frame

float g_scroll_req = -1;   // scripted test: scroll the option list to this fraction of its length
std::map<std::string, float> g_edit;   // slider value while it is held (committed on release: each commit writes a file)
std::map<std::string, std::string> g_text;

// Small tag at the right end of a row bar: only what the player has to act on (the description bar says the rest).
void row_tag(const Opt& o, const SkinRow& r) {
  const SkinPal& p = skin_pal();
  const char* t = nullptr;
  ImVec4 c = p.bar_dim;
  if (o.when == kSoon) t = tr("Coming soon");
  else if (o.when == kRestart) {
    if (is_pending(o)) { t = tr("Restart to apply"); c = p.warn; }
    else t = tr("Restart needed");
  }
  if (!t && live_blocked(o)) { t = tr("Restart to apply"); c = p.warn; }
  if (!t) return;
  const ImVec2 ts = ImGui::CalcTextSize(t);
  const float s2 = 0.82f;
  const ImVec2 sz(ts.x * s2, ts.y * s2);
  const ImU32 col = skin_col(r.e > 0.5f ? p.row_focus_text : c);
  r.dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * s2, ImVec2(r.p.x + r.w - r.h * 0.7f - sz.x, r.p.y + (r.h - sz.y) * 0.5f), col, t);
}

// What a draw_xxx function found out about its control this frame.
struct Hit {
  bool hot = false;     // pointer over it, keyboard or pad focus on it, or being dragged
  bool focus = false;   // keyboard or pad focus
};
Hit hit_of_last_item() {
  Hit h;
  h.focus = ImGui::IsItemFocused();
  h.hot = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || h.focus || ImGui::IsItemActive();
  return h;
}

float row_label_w(const SkinRow& r) { return std::max(px(330), r.w * 0.44f); }

Hit draw_choice(const Opt& o) {
  const std::string cur = cur_of(o);
  Choices dyn;
  const Choices& list = o.dyn ? (dyn = o.dyn()) : o.choices;
  std::string label = cur.empty() && o.choices.empty() ? "" : cur;
  for (auto& c : list) if (c.first == cur) label = c.second;
  if (label == cur && !cur.empty() && (!std::strcmp(o.key, "frame_cap") || !std::strcmp(o.key, "fps_target"))) label = cur + " fps (custom)";
  if (label == cur && !cur.empty() && !std::strcmp(o.key, "gfx_render_scale") && std::atof(cur.c_str()) > 0) label = snappic::dims((float)std::atof(cur.c_str())) + " (typed in)";
  // two or three short answers show as pills side by side (like On / Off); a longer list is a drop-down
  bool pills = !list.empty() && list.size() <= 3 && !o.dyn;
  const bool data = o.dyn && !o.note;   // monitor names: data from the PC, only the fixed words are translated
  std::vector<std::string> shown_names;   // the labels as drawn, in the menu's language
  for (auto& c : list) shown_names.push_back(tx(c.second, data));
  const std::string shown_label = tx(label, data);
  float total = 0;
  for (auto& n : shown_names) { total += std::max(px(110), ImGui::CalcTextSize(n.c_str()).x + px(52)) + px(8); }
  if (total > px(520)) pills = false;
  Hit h;
  if (pills) {
    std::vector<const char*> names;
    int sel = -1;
    for (size_t i = 0; i < list.size(); ++i) { names.push_back(shown_names[i].c_str()); if (list[i].first == cur) sel = (int)i; }
    const int clicked = skin_segmented("##v", names.data(), (int)names.size(), sel, false);
    h = hit_of_last_item();
    if (clicked >= 0 && clicked != sel) {
      if (!((std::strcmp(o.key, "gfx_preset") == 0 || std::strcmp(o.key, "gfx_profile") == 0) && list[(size_t)clicked].first == "custom")) set_of(o, list[(size_t)clicked].first);
    }
    return h;
  }
  // as wide as the label needs (room is left for the "Restart needed" tag)
  const float room = std::max(px(300), ImGui::GetContentRegionAvail().x - px(190));
  const float w = std::clamp(ImGui::CalcTextSize(shown_label.c_str()).x + skin_control_height() * 2.3f, px(300), room);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (skin_dropdown("##v", shown_label.c_str(), w, false)) ImGui::OpenPopup("##pop");
  h = hit_of_last_item();
  ImGui::SetNextWindowPos(ImVec2(at.x, at.y + skin_control_height() + px(4)));
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(std::max(w, px(420)), px(420)));
  if (ImGui::BeginPopup("##pop")) {
    for (size_t ci = 0; ci < list.size(); ++ci) {
      const auto& c = list[ci];
      if (ImGui::Selectable(shown_names[ci].c_str(), c.first == cur)) set_of(o, c.first);
      if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) h.hot = true;
      if (c.first == cur && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndPopup();
  }
  return h;
}

Hit draw_slider(const Opt& o) {
  const std::string cur = cur_of(o);
  float v = (float)std::atof(cur.c_str());
  auto it = g_edit.find(o.key);
  float tmp = it != g_edit.end() ? it->second : v;
  const float span = std::max(1e-6f, o.hi - o.lo);
  float frac = std::clamp((tmp - o.lo) / span, 0.0f, 1.0f);
  char text[64];
  std::snprintf(text, sizeof text, o.fmt, tmp);
  bool released = false;
  const bool changed = skin_slider("##v", &frac, text, px(300), false, &released);
  const Hit h = hit_of_last_item();
  if (changed) g_edit[o.key] = o.lo + frac * span;
  if (released && g_edit.count(o.key)) {
    set_of(o, fmt_float(std::round(g_edit[o.key] * 100.0f) / 100.0f));
    g_edit.erase(o.key);
  } else if (!ImGui::IsItemActive() && !changed) g_edit.erase(o.key);
  return h;
}

// An unchecked box is the same pale grey as the panel: give it an outline.
bool boxed_checkbox(const char* id, bool* b) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, px(1.5f)));
  ImGui::PushStyleColor(ImGuiCol_Border, theme().text_dim);
  const bool ch = ImGui::Checkbox(id, b);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  return ch;
}

Hit draw_toggle(const Opt& o) {
  const bool on = cur_of(o) != "0" && !cur_of(o).empty();
  const char* const names[2] = {tr("On"), tr("Off")};
  const int clicked = skin_segmented("##v", names, 2, on ? 0 : 1, false);
  const Hit h = hit_of_last_item();
  // The gamepad's A only presses buttons: on a value row it must not change the value (Left / Right do, see step_focused_row). Mouse and keyboard still do.
  if (clicked >= 0 && (clicked == 0) != on) set_of(o, clicked == 0 ? "1" : "0");
  return h;
}

// Left / Right (d-pad, arrow keys) on the focused row change its value one step; nothing else about the row moves.
void step_focused_row(const Opt& o) {
  ImGuiContext& g = *GImGui;
  if (!g.NavMoveSubmitted || (g.NavMoveDir != ImGuiDir_Left && g.NavMoveDir != ImGuiDir_Right) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
  const int d = g.NavMoveDir == ImGuiDir_Left ? -1 : 1;
  const std::string cur = cur_of(o);
  if (o.kind == kToggle) set_of(o, (cur != "0" && !cur.empty()) ? "0" : "1");
  else if (o.kind == kSlider) {
    const float v = std::clamp((float)std::atof(cur.c_str()) + (float)d * (o.hi - o.lo) / 20.0f, o.lo, o.hi);
    set_of(o, fmt_float(std::round(v * 100.0f) / 100.0f));
  } else if (o.kind == kChoice) {
    Choices dyn;
    const Choices& list = o.dyn ? (dyn = o.dyn()) : o.choices;
    if (list.empty()) return;
    int i = -1;
    for (size_t k = 0; k < list.size(); ++k) if (list[k].first == cur) i = (int)k;
    if (i < 0) i = d > 0 ? -1 : 0;
    const int n = (int)list.size();
    const std::string next = list[(size_t)(((i + d) % n + n) % n)].first;
    if ((std::strcmp(o.key, "gfx_preset") == 0 || std::strcmp(o.key, "gfx_profile") == 0) && next == "custom") return;   // Custom is only ever shown
    set_of(o, next);
  } else return;
  ImGui::NavMoveRequestCancel();
}

Hit draw_path(const Opt& o) {
  auto it = g_text.find(o.key);
  if (it == g_text.end()) it = g_text.emplace(o.key, cur_of(o)).first;
  char buf[520];
  std::snprintf(buf, sizeof buf, "%s", it->second.c_str());
  ImGui::SetNextItemWidth(px(420));
  if (ImGui::InputText("##v", buf, sizeof buf)) it->second = buf;
  const Hit h = hit_of_last_item();
  if (ImGui::IsItemDeactivatedAfterEdit()) set_of(o, it->second);
  return h;
}

void draw_row(const Opt& o, bool inline_help) {
  if (o.kind == kSection) {
    ImGui::Dummy(ImVec2(0, px(8)));
    ImGui::Indent(px(10));
    ImGui::TextColored(theme().title, "%s", tr(o.label));
    ImGui::Unindent(px(10));
    ImGui::Dummy(ImVec2(0, px(2)));
    return;
  }
  ImGui::PushID(o.key);
  ImGuiContext& g = *GImGui;
  const ImGuiID cid = ImGui::GetID("##v");
  const bool focused = (g.NavCursorVisible && g.NavId == cid) || ImGui::IsPopupOpen("##pop");
  SkinRow r;
  skin_row_begin(r, cid, focused);
  const bool soon = o.when == kSoon;
  // label on the left of the bar, control in the second column
  r.dl->AddText(ImVec2(r.p.x + r.h * 0.75f, r.p.y + (r.h - ImGui::GetFontSize()) * 0.5f), skin_row_text(r, soon ? 0.6f : 1.0f), tr(o.label));
  ImGui::SetCursorScreenPos(ImVec2(r.p.x + row_label_w(r), r.p.y + (r.h - skin_control_height()) * 0.5f));
  Hit h;
  ImGui::BeginDisabled(soon);
  switch (o.kind) {
    case kChoice: h = draw_choice(o); break;
    case kSlider: h = draw_slider(o); break;
    case kToggle: h = draw_toggle(o); break;
    case kPath: h = draw_path(o); break;
    default: break;
  }
  ImGui::EndDisabled();
  if (h.hot) g_help = &o;
  if (h.focus && !soon) step_focused_row(o);
  row_tag(o, r);
  const bool over_bar = ImGui::IsMouseHoveringRect(r.p, ImVec2(r.p.x + r.w, r.p.y + r.h)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  skin_row_end(r, false);
  if (over_bar && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) g_help = &o;
  if (o.note) {   // which value is in effect when another row overrides this one
    const std::string n = o.note();
    if (!n.empty()) { ImGui::Indent(px(24)); ImGui::TextColored(theme().title, "%s", tx(n).c_str()); ImGui::Unindent(px(24)); ImGui::Dummy(ImVec2(0, px(4))); }
  }
  if (env_set_by_hand(o)) { ImGui::TextColored(theme().error, tr("    %s is set outside the game and wins over this choice."), st::env_name(o.key) ? st::env_name(o.key) : o.env); ImGui::Dummy(ImVec2(0, px(4))); }
  if (inline_help) {
    ImGui::Indent(px(24));
    ImGui::PushStyleColor(ImGuiCol_Text, theme().text_dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr(o.help));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(px(24));
    ImGui::Spacing();
  }
  ImGui::PopID();
}

std::string default_label(const Opt& o) {
  if (o.dyn && o.note) { for (auto& c : o.dyn()) if (c.first == o.def) return c.second; return ""; }   // the picture rows: their list is worked out live
  if (o.dyn) return "";
  for (auto& c : o.choices) if (c.first == o.def) return c.second;
  if (o.kind == kToggle) return std::string(o.def) == "0" ? "Off" : "On";
  if (o.kind == kSlider) { char b[48]; std::snprintf(b, sizeof b, o.fmt, (float)std::atof(o.def)); return b; }
  return o.def;
}

// The description of the focused row goes to the bar at the bottom of the menu (the game's Settings screen does the same).
void draw_help(const std::vector<Opt>& list, const Opt** remembered) {
  const Opt* o = g_help ? g_help : *remembered;
  if (!o) for (auto& x : list) if (x.kind != kSection) { o = &x; break; }
  *remembered = o;
  if (!o) return;
  std::string extra;
  auto add = [&](const std::string& t) { if (!extra.empty()) extra += "   "; extra += t; };
  if (o->when == kSoon) add(tr("Coming soon: not in this build yet, so the control is off."));
  else if (o->when == kLive) add(tr("Applies now."));
  else add(tr("Restart needed: use Apply and restart at the top."));
  const std::string dl = default_label(*o);
  if (!dl.empty()) add(sub(tr("Default: %s"), tx(dl, o->dyn && !o->note)));
  if (o->key && std::strcmp(o->key, "gfx_render_scale") == 0) add(sub(tr("Now: scene drawn at %s"), snappic::dims(snappic::effective(pic_cfg(), pic_ctx()).scene)));
  // (the environment variable name is not shown here: it is for the command line and is listed in docs/command-line.md)
  skin_help_set(tr(o->help), extra, o->when == kRestart);
}

// One page of option rows; the description goes to the menu's bottom bar.
void two_pane(const char* id, std::vector<Opt>& list, const Opt** remembered, void (*extra)()) {
  g_help = nullptr;
  if (ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_NavFlattened)) {   // flattened: the rows are the d-pad's targets (not a scroll box to enter with A)
    if (g_scroll_req >= 0) { ImGui::SetScrollY(g_scroll_req * ImGui::GetScrollMaxY()); g_scroll_req = -1; }
    for (const Opt& o : list) draw_row(o, false);
    if (extra) extra();
  }
  ImGui::EndChild();
  draw_help(list, remembered);
}

// ---- mods ------------------------------------------------------------------------------------------------------------
struct ModRow {
  std::string id, name, version, author, description;
  bool enabled = true, has_json = true, exists = true;
};
double now_secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::vector<ModRow> g_mods;
double g_mods_loaded = -100;
std::string g_mods_root;
std::string g_mods_msg;
bool g_mods_err = false;

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Files two enabled packs both change, from the game's own loader (mods::load) run on mods.json as saved. Worked out when the tab is opened,
// on Rescan and after a save, not every few seconds: a big HD pack has thousands of files to list.
bool g_conf_valid = false;
std::vector<mods::PairCount> g_conf_pairs;
std::vector<std::string> g_conf_lines;   // "<path>: <winner> beats <loser>", in loader order
void mods_conflicts_refresh() {
  g_conf_valid = true;
  g_conf_pairs.clear();
  g_conf_lines.clear();
  mods::Index ix;
  std::vector<std::string> warn;
  mods::load(mods::default_root(), ix, warn, false);
  g_conf_pairs = mods::conflict_pairs(ix);
  for (const mods::Conflict& c : ix.conflicts) g_conf_lines.push_back(c.path + ": " + ix.packs[(size_t)c.winner].id + " beats " + ix.packs[(size_t)c.loser].id);
}

void mods_reload() {
  g_mods_loaded = now_secs();
  g_mods_root = mods::default_root();
  g_mods.clear();
  std::error_code ec;
  std::vector<std::pair<std::string, bool>> order;
  bool have_file = false;
  hle::Json j;
  std::string err, txt = read_all(fs::path(g_mods_root) / "mods.json");
  if (!txt.empty() && hle::json_parse(txt, j, err)) {
    have_file = true;
    if (auto* arr = j.get("packs"))
      for (auto& e : arr->a) {
        if (e.type == hle::Json::Str) order.push_back({e.s, true});
        else if (e.type == hle::Json::Obj) {
          std::string id = e.get("id") ? e.get("id")->str() : e.get("folder") ? e.get("folder")->str() : "";
          if (!id.empty()) order.push_back({id, e.get("enabled") ? e.get("enabled")->boolean(true) : true});
        }
      }
  }
  std::vector<std::string> disk;
  if (fs::is_directory(g_mods_root, ec))
    for (auto& d : fs::directory_iterator(g_mods_root, ec)) if (d.is_directory(ec)) disk.push_back(d.path().filename().string());
  std::sort(disk.begin(), disk.end());
  auto listed = [&](const std::string& id) { for (auto& p : order) if (p.first == id) return true; return false; };
  for (auto& d : disk)   // folders the loader would pick up on its own (pack.json), after the ones in mods.json
    if (!listed(d) && fs::exists(fs::path(g_mods_root) / d / "pack.json", ec)) order.push_back({d, d == "native-options"});   // as mods::load: only the pack the game needs is on without a mods.json entry
  for (auto& [id, en] : order) {
    ModRow r;
    r.id = id;
    r.enabled = en;
    const fs::path dir = fs::path(g_mods_root) / id;
    r.exists = fs::is_directory(dir, ec);
    r.name = id;
    hle::Json pj;
    std::string perr, ptxt = read_all(dir / "pack.json");
    r.has_json = !ptxt.empty();
    if (r.has_json && hle::json_parse(ptxt, pj, perr)) {
      if (auto* v = pj.get("name")) if (!v->str().empty()) r.name = v->str();
      if (auto* v = pj.get("version")) r.version = v->str();
      if (auto* v = pj.get("author")) r.author = v->str();
      if (auto* v = pj.get("description")) r.description = v->str();
    }
    g_mods.push_back(r);
  }
}

// What the loader reads next start: ids and enabled flags in priority order.
std::string mods_state() {
  std::string s;
  for (auto& r : g_mods) if (r.exists) s += (s.empty() ? "" : ",") + r.id + (r.enabled ? ":1" : ":0");
  return s;
}

bool mods_save() {
  hle::Json j = hle::Json::object();
  hle::Json arr = hle::Json::array();
  for (auto& r : g_mods) {
    hle::Json e = hle::Json::object();
    e.set("id", hle::Json::string(r.id));
    e.set("enabled", hle::Json::boolean_(r.enabled));
    arr.a.push_back(e);
  }
  j.set("packs", arr);
  std::string out;
  hle::json_dump(j, out);
  out += "\n";
  std::error_code ec;
  fs::create_directories(g_mods_root, ec);
  const fs::path p = fs::path(g_mods_root) / "mods.json", tmp = fs::path(g_mods_root) / "mods.json.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << out;
    if (!f.good()) { g_mods_msg = sub(tr("Could not write mods.json in %s"), g_mods_root); g_mods_err = true; return false; }
  }
  fs::rename(tmp, p, ec);
  if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
  if (ec) { g_mods_msg = sub(tr("Could not replace mods.json: %s"), ec.message()); g_mods_err = true; return false; }
  g_mods_msg = tr("Saved mods.json. The change applies after a restart.");
  g_mods_err = false;
  g_conf_valid = false;
  return true;
}

void open_folder(const std::string& dir) {
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  ShellExecuteW(nullptr, L"explore", fs::u8path(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

// ---- pages -----------------------------------------------------------------------------------------------------------
void options_menu_opened() { capture_start(); }

int options_pending_restart(std::string* names) {
  capture_start();
  Pend p;
  for_each_opt(pend_one, &p);
  if (mods_state() != g_mods_start) p.names.push_back(tr("Mods"));
  if (names) {
    names->clear();
    for (size_t i = 0; i < p.names.size(); ++i) *names += (i ? ", " : "") + p.names[i];
  }
  return (int)p.names.size();
}

void options_restart_bar() {
  if (const char* why = nvn::FgNotice()) ImGui::TextColored(theme().conflict, tr("Frame generation is off for this session (%s). The game shows normal frames."), why);   // live fallback: it failed and turned itself off (SNAP_FRAMEGEN_FAIL=init|prepare|generate|present tests it)
  if (const char* why = snap_rt::FallbackNotice()) ImGui::TextColored(theme().conflict, tr("Ray tracing is off for this session (%s). The standard lighting is in use."), why);   // live fallback: SNAP_RT could not start
  std::string names;
  const int n = options_pending_restart(&names);
  if (!n) return;
  const Theme& t = theme();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, t.toast_bg);
  if (ImGui::BeginChild("pending", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.6f), ImGuiChildFlags_Borders)) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - px(250));
    std::string shown = names;
    size_t c1 = 0;
    for (int k = 0; k < 3 && c1 != std::string::npos; ++k) c1 = names.find(", ", c1 == 0 && k == 0 ? 0 : c1 + 2);
    if (c1 != std::string::npos) { char more[1024]; std::snprintf(more, sizeof more, tr("%s and %d more"), names.substr(0, c1).c_str(), n - 3); shown = more; }
    ImGui::TextColored(t.conflict, n == 1 ? tr("%d change needs a restart: %s") : tr("%d changes need a restart: %s"), n, shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine(ImGui::GetWindowWidth() - px(240));
    if (ImGui::Button(tr("Apply and restart"))) restart_game();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void tab_graphics() {
  static const Opt* remembered = nullptr;
  two_pane("gfx", graphics_opts(), &remembered, [] {
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(tr("Shader cache and compile settings"))) shader_cache_section();
  });
}

void tab_display() {
  static const Opt* remembered = nullptr;
  two_pane("disp", display_opts(), &remembered, [] {
    // docs/player-options.md: host/mods/problem_report.cpp. One zip on the Desktop; never uploaded.
    static std::string msg;
    static bool ok = false;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(tr("Create problem report"))) {
      char path[1024] = {}, why[512] = {};
      ok = snap_problem_report("game", path, (int)sizeof path, why, (int)sizeof why) != 0;
      msg = ok ? sub(tr("Saved on your Desktop: %s"), path) : sub(tr("Could not make the report: %s"), why);
    }
    ImGui::SameLine();
    ImGui::TextColored(theme().text_dim, "%s", tr("A zip with the logs, settings and PC specs. No game files or saves; your user name is removed; nothing is uploaded."));
    if (!msg.empty()) ImGui::TextColored(ok ? theme().good : theme().error, "%s", msg.c_str());
  });
}

void tab_photos() {
  static const Opt* remembered = nullptr;
  two_pane("photos", photo_opts(), &remembered, [] {
    ImGui::Spacing();
    if (ImGui::Button(tr("Open PNG folder"))) {
      std::string d = st::get("photo_dir", "");
      if (d.empty()) { const char* a = std::getenv("APPDATA"); d = std::string(a ? a : ".") + "\\SnapRecomp\\photos"; }
      open_folder(d);
    }
    ImGui::SameLine();
    ImGui::TextColored(theme().text_dim, "%s", tr("Opens the folder in Explorer."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    gallery_section();
  });
}

void audio_output_section() {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(theme().title, "%s", tr("Output"));
  for (const Opt& o : audio_opts()) draw_row(o, true);
}

void access_rows() {
  for (const Opt& o : access_opts()) draw_row(o, true);
}

void tab_mods() {
  const Theme& t = theme();
  if (now_secs() - g_mods_loaded > 3.0) mods_reload();
  ImGui::TextWrapped(tr("Mod packs are folders in %s. A pack higher in the list wins when two packs replace the same game file. The loader reads this list when the game starts, so every change here needs a restart."),
                     g_mods_root.c_str());
  if (const char* e = std::getenv("SNAP_MODS_DIR")) if (*e) ImGui::TextColored(t.conflict, "%s", tr("SNAP_MODS_DIR is set in the environment: this is the folder it points to."));
  ImGui::Spacing();
  if (ImGui::Button(tr("Open mods folder"))) open_folder(g_mods_root);
  ImGui::SameLine();
  if (ImGui::Button(tr("Rescan"))) { mods_reload(); g_conf_valid = false; }
  ImGui::SameLine();
  ImGui::TextColored(t.conflict, "%s", tr("restart needed"));
  if (!g_mods_msg.empty()) ImGui::TextColored(g_mods_err ? t.error : t.good, "%s", g_mods_msg.c_str());
  ImGui::Spacing();
  ImGui::Separator();
  if (g_mods.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("No packs found. A pack is a folder with a pack.json next to its <archive>.drp\\<entry> files; see docs\\mods.md."));
    return;
  }
  int move_from = -1, move_to = -1;
  bool changed = false;
  for (int i = 0; i < (int)g_mods.size(); ++i) {
    ModRow& r = g_mods[i];
    ImGui::PushID(i);
    ImGui::BeginDisabled(i == 0);
    if (ImGui::ArrowButton("up", ImGuiDir_Up)) { move_from = i; move_to = i - 1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == (int)g_mods.size());
    if (ImGui::ArrowButton("down", ImGuiDir_Down)) { move_from = i; move_to = i + 1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!r.exists);
    if (boxed_checkbox("##en", &r.enabled)) changed = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(r.enabled ? t.text : t.text_dim, "%d. %s", i + 1, r.name.c_str());
    if (!r.version.empty()) { ImGui::SameLine(); ImGui::TextColored(t.text_dim, "v%s", r.version.c_str()); }
    if (!r.author.empty()) { ImGui::SameLine(); ImGui::TextColored(t.text_dim, tr("by %s"), r.author.c_str()); }
    ImGui::SameLine();
    ImGui::TextColored(r.enabled ? t.good : t.text_dim, "%s", r.enabled ? tr("on") : tr("off"));
    ImGui::Indent(px(70));
    if (!r.exists) ImGui::TextColored(t.error, tr("Listed in mods.json but the folder %s is missing."), r.id.c_str());
    else if (!r.has_json) ImGui::TextColored(t.conflict, tr("Folder \"%s\" has no pack.json (name and version unknown)."), r.id.c_str());
    else if (!r.description.empty()) ImGui::TextColored(t.text_dim, "%s", r.description.c_str());
    ImGui::Unindent(px(70));
    ImGui::PopID();
  }
  if (move_from >= 0 && move_to >= 0 && move_to < (int)g_mods.size()) { std::swap(g_mods[move_from], g_mods[move_to]); changed = true; }
  if (changed) mods_save();
  // Conflicts: the same list as the launcher's Mods page and `--mods conflicts` (one loader). The first pack of a pair is the one the game uses.
  if (!g_conf_valid) mods_conflicts_refresh();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(t.text, "%s", tr("Conflicts (files two enabled packs both change)"));
  if (g_conf_pairs.empty()) ImGui::TextColored(t.text_dim, "%s", tr("No file is changed by two enabled packs."));
  for (const mods::PairCount& pc : g_conf_pairs) ImGui::TextColored(t.conflict, tr("%s beats %s on %d files"), pc.winner.c_str(), pc.loser.c_str(), pc.count);
  if (!g_conf_lines.empty() && ImGui::TreeNode(tr("Every file"))) {
    ImGuiListClipper clip;
    clip.Begin((int)g_conf_lines.size());
    while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextColored(t.text_dim, "%s", g_conf_lines[(size_t)i].c_str());
    ImGui::TreePop();
  }
}

// ---- scripted-test entry points (ui_test.cpp): the same code the controls run
namespace {
const Opt* find_opt(const std::string& key) {
  for (auto* list : {&graphics_opts(), &display_opts(), &photo_opts(), &audio_opts(), &access_opts()})
    for (const Opt& o : *list) if (o.kind != kSection && key == o.key) return &o;
  return nullptr;
}
}  // namespace

bool options_test_set(const std::string& key, const std::string& value) {
  capture_start();
  const Opt* o = find_opt(key);
  if (!o || o->when == kSoon) return false;
  // The in-game pages list the look's own value (Sharper menu text On / Off) instead of "Default (...)": picking the value the Look gives leaves the key
  // erased, so the row keeps following the Look. (The F1 page lists "default" itself; its explicit values stay explicit.)
  if (o->vis && !std::strcmp(o->def, "default") && std::strcmp(o->key, "gfx_aa") && value == snapdef::def(o->vis)) { set_of(*o, "default"); return true; }
  set_of(*o, value);
  return true;
}
std::string options_test_default(const std::string& key) {   // the value the row's Default choice stores
  const Opt* o = find_opt(key);
  return o ? std::string(o->def) : std::string();
}
void options_test_scroll(float fraction) { g_scroll_req = fraction; }
std::string options_test_get(const std::string& key) {
  const Opt* o = find_opt(key);
  if (!o) return "<unknown option>";
  const std::string v = cur_of(*o);
  if (o->vis && v == "default" && !std::strcmp(o->def, "default") && std::strcmp(o->key, "gfx_aa")) return snapdef::def(o->vis);   // the Look's own value (see options_test_set)
  return v;
}
// The one-line note of a picture row (which value is in effect when another row overrides it), for the game's own Settings screen (host/mods/nativeopt.cpp).
// "" when nothing overrides the row. The Internal resolution row says what Automatic comes to when nothing else has something to say.
std::string options_note(const std::string& key) {
  for (const Opt& o : graphics_opts()) {
    if (!o.key || key != o.key) continue;
    std::string n = o.note ? o.note() : std::string();
    if (n.empty() && key == "gfx_render_scale" && cur_of(o) == "auto") n = snappic::auto_label(pic_cfg(), pic_ctx());
    return n;
  }
  return "";
}
bool options_test_option_when(const std::string& key, std::string* when) {
  const Opt* o = find_opt(key);
  if (!o) return false;
  *when = o->when == kLive ? "live" : o->when == kRestart ? "restart" : "soon";
  return true;
}
bool options_test_mods(const std::string& op, const std::string& id, int arg, std::string* state) {
  capture_start();
  mods_reload();
  if (op == "toggle" || op == "move") {
    int i = -1;
    for (int k = 0; k < (int)g_mods.size(); ++k) if (g_mods[k].id == id) i = k;
    if (i < 0) return false;
    if (op == "toggle") g_mods[i].enabled = !g_mods[i].enabled;
    else {
      int j = i + arg;
      if (j < 0 || j >= (int)g_mods.size()) return false;
      std::swap(g_mods[i], g_mods[j]);
    }
    if (!mods_save()) return false;
  }
  *state = mods_state();
  return true;
}

}  // namespace snap::ui
