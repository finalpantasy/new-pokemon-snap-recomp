// SPDX-License-Identifier: MIT
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "access.h"
#include "btn_icons.h"
#include "hle.h"
#include "hle_config.h"
#include "input_core.h"
#include "live.h"
#include "profiles.h"
#include "visual_defaults.h"   // Look row (gfx_look)

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace hle::settings {

namespace {
std::mutex g_mu;
std::map<std::string, std::string> g_kv;
std::map<std::string, std::string> g_over;   // override_for_run (--set): wins over g_kv in get(), never saved
std::string g_path;
std::vector<std::string> g_exported;   // env vars this layer set (so a restart does not pass them on as if the user had set them)
bool g_readonly = false;               // safe mode: the file on disk is never rewritten from the trimmed in-memory copy

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string path_str(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

void write_file();
void read_file() {
  g_kv.clear();
  if (g_path.empty()) return;
  std::ifstream f(P(g_path));
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    g_kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  // Migration (2026-11-13): the old "gfx_draw_distance" (env SNAP_DRAW_DISTANCE, 1..4 in the UI, moved LOD switches and cull together) became
  // "gfx_lod_dist" (switches) plus "gfx_draw_dist" (cull only), both 1..8. Carry an old value over so it keeps the same look.
  auto old = g_kv.find("gfx_draw_distance");
  if (old != g_kv.end()) {
    double v = std::atof(old->second.c_str());
    if (v >= 1.0) {
      if (v > 8.0) v = 8.0;
      char b[32];
      std::snprintf(b, sizeof b, "%.2f", v);
      if (!g_kv.count("gfx_lod_dist")) g_kv["gfx_lod_dist"] = b;
      if (!g_kv.count("gfx_draw_dist")) g_kv["gfx_draw_dist"] = b;
    }
    g_kv.erase(old);
    write_file();
  }
  // Migration (2026-11-16): "gfx_lod_dist" became a choice of 1 (Original), 3 (High) and 8 (Max); an old slider value goes to the nearest step.
  auto lod = g_kv.find("gfx_lod_dist");
  if (lod != g_kv.end() && lod->second != "1" && lod->second != "3" && lod->second != "8") {
    const double v = std::atof(lod->second.c_str());
    if (v < 1.5) g_kv.erase(lod);
    else lod->second = v < 5.5 ? "3" : "8";
    write_file();
  }
}

// Flushes a just-written file to disk before it replaces the real one: a rename can reach the disk before the data, so a power
// loss right after it could leave an empty or torn file under the real name. A file that cannot be reopened passes (it was
// written whole); a refused flush fails. SNAP_ATOMIC_FLUSH=0 skips the flush, as before.
static bool flush_written(const std::filesystem::path& p) {
#ifdef _WIN32
  static const bool off = [] { const char* e = std::getenv("SNAP_ATOMIC_FLUSH"); return e && *e == '0'; }();
  if (off) return true;
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return true;
  const bool ok = FlushFileBuffers(h) != 0;
  CloseHandle(h);
  return ok;
#else
  (void)p;
  return true;
#endif
}

void write_file() {
  if (g_path.empty() || g_readonly) return;
  std::error_code ec;
  fs::create_directories(P(g_path).parent_path(), ec);
  std::string tmp = g_path + ".tmp";
  {
    std::ofstream f(P(tmp), std::ios::trunc);
    f << "# per-profile settings (edited by the F1 menu)\n";
    for (auto& [k, v] : g_kv) f << k << "=" << v << "\n";
    f.flush();
    if (!f.good()) { f.close(); fs::remove(P(tmp), ec); return; }   // a short write must not replace the good file
  }
  if (!flush_written(P(tmp))) { std::fprintf(stderr, "settings: flush of %s failed (kept the previous file)\n", tmp.c_str()); fs::remove(P(tmp), ec); return; }
  // Replace in one step and never delete the target first: a failed replace (antivirus or an indexer holding the .tmp) keeps the
  // old file. A few short retries, then give up, log, and drop the .tmp.
  bool ok = false;
  for (int i = 0; i < 5 && !ok; i++) {
#ifdef _WIN32
    ok = MoveFileExW(P(tmp).wstring().c_str(), P(g_path).wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) Sleep(20);
#else
    fs::rename(P(tmp), P(g_path), ec);
    ok = !ec;
#endif
  }
  if (!ok) {
    std::fprintf(stderr, "settings: could not replace %s (kept the previous file)\n", g_path.c_str());
    fs::remove(P(tmp), ec);
  }
}

// Settings that reach the game through an env var read at start (the F1 menu's "restart needed" options). The key is stored only when the user chose
// something; "" and "default" mean "leave the variable alone" (the game's own behavior). An env var set by hand always wins (set_env_if_unset).
const struct { const char* key; const char* env; } kEnvKeys[] = {
    {"gfx_render_scale", "SNAP_RENDER_SCALE"}, {"gfx_aniso", "SNAP_ANISO"},           {"gfx_lod_bias", "SNAP_LOD_BIAS"},
    {"gfx_shadow_res", "SNAP_SHADOW_RES"},     {"ssao_quality", "SNAP_SSAO_QUALITY"}, {"gfx_lighting", "SNAP_LIGHTING"}, {"gfx_bounce", "SNAP_SSGI"}, {"rt_reflect", "SNAP_RT_REFLECT"}, {"rt_gi", "SNAP_RT_GI"}, {"rt_gi_rays", "SNAP_RT_GI_RAYS"},   // Experimental rows (runtime/nvn/nvn_rt.cpp; restart)
   
    {"gfx_aa", "SNAP_AA"},                     {"gfx_smaa_quality", "SNAP_SMAA_QUALITY"}, {"return_to_launcher", "SNAP_RETURN_TO_LAUNCHER"}, {"gfx_sharpen", "SNAP_SHARPEN"},       {"gfx_upscaler", "SNAP_UPSCALER"},
    {"gfx_upscale_quality", "SNAP_UPSCALE_QUALITY"}, {"gfx_hdr", "SNAP_HDR"},        {"gfx_hdr_peak", "SNAP_HDR_PEAK"},
    {"gfx_hdr_paper", "SNAP_HDR_PAPER"},       {"gfx_draw_dist", "SNAP_DRAW_DIST"}, {"gfx_lod_dist", "SNAP_LOD_DIST"}, {"gfx_lod_force0", "SNAP_LOD_FORCE0"},   // gfx_fov removed (owner 2026-11-12): a saved value is ignored
    {"gfx_distant_scenery", "SNAP_DISTANT_SCENERY"}, {"gfx_full_rate_anim", "SNAP_FULL_RATE_ANIM"}, {"gfx_ui_font", "SNAP_UI_FONT_SCALE"}, {"fast_boot", "SNAP_FAST_BOOT"},
    {"gfx_ssao_samples", "SNAP_SSAO_SAMPLES"}, {"gfx_ssao_hbao", "SNAP_SSAO_HBAO"},
    {"gfx_shadow_dist", "SNAP_SHADOW_DISTANCE"}, {"gfx_hdr_expand", "SNAP_HDR_EXPAND"},   // 2026-11-14, docs/advanced-graphics-options.md
    {"gfx_ultrawide", "SNAP_ULTRAWIDE"},   // auto (default) | off (docs/ultrawide.md)
    {"gfx_gpl", "SNAP_GPL"},   // Pipeline libraries row (auto | on | off; runtime/nvn/nvn_vk.cpp; restart)
    {"vsync", "SNAP_VSYNC"},   // the F1 / native Vsync row (live through snap_vsync_set; this carries it to the next start)
    {"contact_shadows", "SNAP_CONTACT_SHADOWS"},
    {"water_ssr", "SNAP_WATER_SSR"}, {"water_caustics", "SNAP_WATER_CAUSTICS"}, {"water_foam", "SNAP_WATER_FOAM"}, {"water_ocean", "SNAP_WATER_OCEAN"},   // water rows ("1" or unset; live through snap_water_fx_set, runtime/nvn/nvn_ssr.cpp)
    {"fps_overlay", "SNAP_FPS_OVERLAY"},                 // performance overlay "fps" / "detailed" or unset (live, runtime/ui/ui_fps.cpp)
    {"fps_overlay_corner", "SNAP_FPS_OVERLAY_CORNER"},   // tl / tr / bl / br, unset = tr   // Contact shadows row: "1" or unset (live through snap_contact_shadows_set)
    {"photo_export", "SNAP_PHOTO_EXPORT"},    {"photo_dir", "SNAP_PHOTO_DIR"}, {"hd_photos", "SNAP_HD_PHOTOS"},    {"neo_speed", "SNAP_NEO_SPEED"}, {"film", "SNAP_FILM"},
    {"freecam", "SNAP_FREECAM"}, {"freecam_freeze", "SNAP_FREECAM_FREEZE"},   // free camera rows (host/mods/freecam.cpp, docs/freecam.md)
    {"atmos_shafts", "SNAP_ATMOS_SHAFTS"}, {"atmos_fog", "SNAP_ATMOS_FOG"}, {"atmos_exposure", "SNAP_ATMOS_EXPOSURE"}, {"atmos_bloom", "SNAP_ATMOS_BLOOM"}, {"atmos_clouds", "SNAP_ATMOS_CLOUDS"}, {"atmos_sky", "SNAP_ATMOS_SKY"},   // atmosphere rows (runtime/nvn/nvn_atmos.cpp; "1" or unset; restart)
    {"skip_cutscenes", "SNAP_SKIP_CUTSCENES"},
    {"mods_readahead", "SNAP_MODS_READAHEAD"},   // "0" / "1": read the loading course's HD pack files ahead (runtime/mods/mods.cpp, docs/mods.md)
    {"skip_course_intro", "SNAP_SKIP_INTRO"},   // "1" or unset: skip the 10.6 s course fly-over (host/mods/introskip.cpp)
    {"ui_smooth", "SNAP_UI_SMOOTH"},            // "0" or unset: smooth interface above 60 fps off / on (runtime/nvn/nvn_uismooth.h; live in F1)
    {"hud_scale", "SNAP_HUD_SCALE"},            // auto | 100 | 125 | 150 | 175 | 200 (runtime/nvn/nvn_hud.cpp)
    {"frame_generation", "SNAP_FRAMEGEN"},   // Frame generation row ("fsr" or the older "1" = FSR, "dlss" = NVIDIA DLSS, unset = off; runtime/nvn/nvn_framegen.cpp; restart)
    {"foliage_aa", "SNAP_FOLIAGE_AA"}, {"leaf_translucency", "SNAP_LEAF_TRANSLUCENCY"}, {"soft_particles", "SNAP_SOFT_PARTICLES"}, {"gfx_hd_effects", "SNAP_HD_EFFECTS"},   // foliage / particle rows (runtime/nvn/nvn_foliage.cpp; restart)
    {"specular_aa", "SNAP_SPECULAR_AA"},   // image stability row (runtime/nvn/nvn_motionaa.cpp; live)
    {"gfx_pom", "SNAP_POM"},   // Parallax detail row (off | low | high; runtime/nvn/nvn_pom.cpp, docs/pom.md; restart)
    {"gfx_bokeh_dof", "SNAP_BOKEH_DOF"}, {"gfx_motion_blur", "SNAP_MOTION_BLUR"},   // lens effect rows (runtime/nvn/nvn_lens_fx.cpp; strength, unset = off; restart)
    {"spawn_rand", "SNAP_SPAWN_RAND"}, {"spawn_seed", "SNAP_SPAWN_SEED"},   // Spawn randomizer rows (off | within | all; seed text): read by scripts/mods/make_spawn_randomizer.py, which builds the pack
    {"game_language", "SNAP_GAME_LANGUAGE"},   // Game language row: ja | en | fr | de | it | es | ko | zh-hans | zh-hant, unset = Auto (Windows language, else English); docs/player-options.md
    {"fps_target", "SNAP_FPS"},   // Frame rate target row (any rate 61 to 1000, "monitor", "unlocked"; "default" = 60, the game's own); host/mods/highfps_core.h
    {"access_cvd", "SNAP_ACCESS_CVD"}, {"access_cvd_strength", "SNAP_ACCESS_CVD_STRENGTH"}, {"access_ui_size", "SNAP_ACCESS_UI_SIZE"}, {"access_contrast", "SNAP_ACCESS_CONTRAST"},   // Accessibility rows (hle/access.h, docs/accessibility.md); live through hle::access::set
    {"audio_latency", "SNAP_AUDIO_LATENCY"}, {"audio_resampler", "SNAP_AUDIO_RESAMPLER"}, {"audio_range", "SNAP_AUDIO_RANGE"}, {"audio_device", "SNAP_AUDIO_DEVICE"},   // audio rows (runtime/audio/audio_out.cpp, docs/audio.md; range is live through snap_audio_set_range, the rest restart)
    {"access_flash", "SNAP_ACCESS_FLASH"}, {"access_mono", "SNAP_ACCESS_MONO"}, {"access_toggle", "SNAP_ACCESS_TOGGLE"},   // access_toggle: comma list of buttons that toggle (read at start, hle/input_core.cpp)
};

// F1 post-process rows: key -> render pass names (comma list) the host switches off through SNAP_PASS_OFF.
const struct { const char* key; const char* passes; } kPassKeys[] = {
    {"gfx_pass_bloom", "BloomCreate,BloomDraw"}, {"gfx_pass_dof", "DOFBlurX,DOFBlurY,DOF"},
    {"gfx_pass_grain", "FilmGrain"},            {"gfx_pass_aberration", "ChromaticAberration"},
};

// Rows that apply with no restart (runtime/ui/ui_options.cpp kLive rows that are read through an env var): export_live() below writes their
// variable again the moment the row changes and moves hle::live::gen(), and the code that reads the variable parses it again (hle/live.h).
// A key is added here only when everything that reads its variable follows a change; the rest stay restart rows.
const char* const kLiveKeys[] = {
    "atmos_shafts", "atmos_fog", "atmos_exposure", "atmos_bloom", "atmos_clouds", "atmos_sky",   // nvn_atmos.cpp GetWants
    "gfx_motion_blur", "gfx_bokeh_dof",                                                          // nvn_lens_fx.cpp
    "gfx_sharpen", "gfx_smaa_quality",                                                           // nvn_aa.cpp AaLiveRefresh
    "ssao_quality", "gfx_ssao_samples", "gfx_ssao_hbao",                                         // host/mods/gamemods.cpp
    "gfx_lighting", "gfx_bounce", "gfx_shadow_dist",                                             // hle/lighting_preset.h, nvn_lighting_fx.cpp, nvn_shadowpcf.cpp, nvn_draw.cpp
    "foliage_aa", "leaf_translucency", "gfx_pom",                                                // nvn_foliage.cpp, nvn_pom.cpp
    "specular_aa",                                                                               // nvn_motionaa.cpp
    "gfx_lod_bias", "gfx_aniso",                                                                 // nvn_gfx.cpp (samplers are made again)
    "gfx_distant_scenery", "gfx_lod_dist", "gfx_lod_force0", "gfx_draw_dist", "gfx_full_rate_anim",                     // host/mods/lodanim.cpp
    "fps_target",                                                                                // host/mods/highfps.cpp
    "gfx_render_scale", "gfx_aa", "gfx_shadow_res", "gfx_ultrawide", "gfx_hd_effects",           // nvn_retarget.cpp (render targets are made again at a present)
    "gfx_hdr_expand", "gfx_hdr", "gfx_hdr_peak", "gfx_hdr_paper",                                                                            // nvn_hdr.cpp
};
bool live_key(const char* key) {
  for (const char* k : kLiveKeys) if (!std::strcmp(k, key)) return true;
  return false;
}

float to_float(const std::string& s, float def) { return s.empty() ? def : (float)std::atof(s.c_str()); }

void set_env_if_unset(const char* name, const std::string& value) {
  const char* cur = std::getenv(name);
  if (cur && *cur) return;
  if (value.empty()) return;
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 0);
#endif
  g_exported.push_back(name);
}
}  // namespace

std::string file_path() { std::lock_guard<std::mutex> g(g_mu); return g_path; }

void use_path_for_test(const std::string& path) {
  { std::lock_guard<std::mutex> g(g_mu); g_path = path; read_file(); }
  apply(false);
}

bool safe_mode() {
  static const bool on = [] { const char* e = std::getenv("SNAP_SAFE_MODE"); return e && e[0] == '1'; }();
  return on;
}

namespace {
// Safe mode keeps only settings that do not change what is drawn or loaded: language, accessibility, input feel, HUD size, window size.
bool safe_keeps(const std::string& k) {
  static const char* const kKeep[] = {"game_language", "return_to_launcher", "hud_scale", "gfx_ui_font", "swap_ab", "button_icons", "win_size",
                                      "gyro_on", "gyro_sens", "gyro_invert_x", "gyro_invert_y", "rumble_on", "rumble_strength",
                                      "stick_curve", "stick_deadzone", "gyro_smooth", "gyro_dead", "gyro_avg", "raw_mouse", "mouse_window_ms"};
  if (k.compare(0, 7, "access_") == 0) return true;
  for (const char* n : kKeep) if (k == n) return true;
  return false;
}

// Safe mode: the feature switches that could also arrive from the environment (set by hand or by a script) are cleared too, so the run really is plain.
void safe_clear_env() {
  static const char* const kExtra[] = {"SNAP_RT", "SNAP_RT_SHADOWS", "SNAP_RT_REFLECT", "SNAP_LOOK", "SNAP_PASS_OFF", "SNAP_SHINY", "SNAP_SHINY_ODDS", "SNAP_FRAMEGEN", "SNAP_UPSCALER",
                                       "SNAP_MODEL_REFINE", "SNAP_MODS_AB", "SNAP_MODEL_AB", "SNAP_SPAWN_RAND", "SNAP_INPUT_SCRIPT_MODS"};
  for (auto& m : kEnvKeys) if (!safe_keeps(m.key)) _putenv_s(m.env, "");
  for (const char* n : kExtra) _putenv_s(n, "");
  _putenv_s("SNAP_VISUALS", "original");    // every visual row that is unset follows the original column
  _putenv_s("SNAP_PIPELINE_SEED", "0");     // no seeded shader keys (the shader cache folder itself is swapped in nvn_pipeline.cpp)
}
}  // namespace

void load() {
  {
    std::lock_guard<std::mutex> g(g_mu);
    const profiles::Active& a = profiles::active();
    if (a.on && !a.id.empty()) g_path = path_str(fs::path(P(a.root)) / a.id / "settings.cfg");
    else g_path = path_str(P(config().data_root) / "settings.cfg");
    read_file();
    if (safe_mode()) {   // keep a trimmed copy in memory; the file on disk stays as it was
      for (auto it = g_kv.begin(); it != g_kv.end();) it = safe_keeps(it->first) ? std::next(it) : g_kv.erase(it);
      g_readonly = true;
    }
  }
  if (safe_mode()) safe_clear_env();
  apply(true);
}

void override_for_run(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> g(g_mu);
  g_over[key] = value;
}

std::string get(const std::string& key, const std::string& def) {
  std::lock_guard<std::mutex> g(g_mu);
  if (!g_over.empty()) {   // --set key=value: this run only, never written to the file; safe mode keeps only what it keeps from the file
    auto ov = g_over.find(key);
    if (ov != g_over.end() && (!safe_mode() || safe_keeps(key))) return ov->second;
  }
  auto it = g_kv.find(key);
  return it == g_kv.end() ? def : it->second;
}
bool get_bool(const std::string& key, bool def) {
  std::string v = get(key, def ? "1" : "0");
  return !v.empty() && v != "0";
}
float get_float(const std::string& key, float def) { return to_float(get(key, ""), def); }

void set(const std::string& key, const std::string& value) {
  {
    std::lock_guard<std::mutex> g(g_mu);
    g_over.erase(key);   // a choice made in the menu replaces a --set value for the rest of the run
    g_kv[key] = value;
    write_file();
  }
  apply(false);
}
void set_bool(const std::string& key, bool v) { set(key, v ? "1" : "0"); }
void set_float(const std::string& key, float v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); set(key, b); }

namespace {
// One live variable: `value` ("" = not set). A variable set by hand keeps winning, exactly as at start. Caller holds env_mu exclusively and g_mu.
void put_live(const char* name, const std::string& value) {
  bool ours = false;
  for (auto& e : g_exported) if (e == name) { ours = true; break; }
  const char* cur = std::getenv(name);
  if (cur && *cur && !ours) return;
  if (std::string(cur ? cur : "") == value) return;
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
  if (!ours) g_exported.push_back(name);
}

// The live rows' variables follow the saved values now (not only at the next start), then every reader is told to parse again.
void export_live() {
  if (safe_mode()) return;   // safe mode cleared these variables and keeps them clear
  std::vector<std::pair<const char*, std::string>> want;
  for (auto& m : kEnvKeys) {
    if (!live_key(m.key)) continue;
    std::string v = get(m.key, "");
    if (v == "default") v.clear();
    want.push_back({m.env, v});
  }
  {   // Color look + strength -> SNAP_LOOK (host/mods/look.cpp)
    const std::string look = get("look", "neutral");
    char b[64] = "";
    if (look != "neutral" && !look.empty()) std::snprintf(b, sizeof b, "%s:%.2f", look.c_str(), get_float("look_intensity", 1.0f));
    want.push_back({"SNAP_LOOK", b});
  }
  {   // the game's own post-process passes -> SNAP_PASS_OFF (host/mods/gamemods.cpp)
    std::string off;
    for (auto& m : kPassKeys) if (get(m.key, "") == "off") off += (off.empty() ? "" : ",") + std::string(m.passes);
    want.push_back({"SNAP_PASS_OFF", off});
  }
  {
    std::unique_lock<std::shared_mutex> env(live::env_mu());
    std::lock_guard<std::mutex> g(g_mu);
    for (auto& w : want) put_live(w.first, w.second);
  }
  live::bump();
}
}  // namespace

void forget_exported_env() {
  for (auto& n : g_exported) {
#ifdef _WIN32
    _putenv_s(n.c_str(), "");
#else
    unsetenv(n.c_str());
#endif
  }
  g_exported.clear();
}

void apply(bool env) {
  // live: controller effects
  input::PadFx& fx = input::pad_fx();
  fx.gyro_on = get_bool("gyro_on", false);
  fx.gyro_sens = get_float("gyro_sens", 1.0f);
  fx.gyro_invert_x = get_bool("gyro_invert_x", false);
  fx.gyro_invert_y = get_bool("gyro_invert_y", false);
  fx.rumble_on = get_bool("rumble_on", true);
  fx.rumble_strength = get_float("rumble_strength", 1.0f);
  {   // aim feel (docs/aim-feel.md): each SNAP_* variable beats its setting; every default is the behavior from before these options
    auto val = [&](const char* envn, const char* key, const char* def) {
      const char* e = std::getenv(envn);
      return e && *e ? std::string(e) : get(key, def);
    };
    auto num = [&](const char* envn, const char* key, float def) { const std::string s = val(envn, key, ""); return s.empty() ? def : (float)std::atof(s.c_str()); };
    auto flag = [&](const char* envn, const char* key) { const std::string s = val(envn, key, "0"); return !(s.empty() || s == "0" || s == "off"); };
    fx.stick_curve = input::stick_curve_from_name(val("SNAP_STICK_CURVE", "stick_curve", "linear"));
    const float dz = num("SNAP_STICK_DEADZONE", "stick_deadzone", -1.0f);
    fx.stick_deadzone = dz < 0.0f ? -1.0f : std::min(dz, 0.6f);
    fx.gyro_smooth = std::clamp(num("SNAP_GYRO_SMOOTH", "gyro_smooth", 0.0f), 0.0f, 1.0f);
    fx.gyro_dead = std::clamp(num("SNAP_GYRO_DEAD", "gyro_dead", input::kGyroDeadDegPerSecDefault), 0.0f, 10.0f);
    fx.gyro_avg = flag("SNAP_GYRO_AVG", "gyro_avg");
    fx.raw_mouse = flag("SNAP_RAW_MOUSE", "raw_mouse");
    fx.mouse_window_ms = std::clamp(num("SNAP_MOUSE_WINDOW_MS", "mouse_window_ms", 40.0f), 8.0f, 80.0f);
  }
  {   // A/B swap (confirm and back on the controller; docs/player-options.md): SNAP_SWAP_AB (1 / 0) beats the swap_ab setting; neither set follows input.json "controller.swap_ab". Live; the prompts' icons follow.
    const char* e = std::getenv("SNAP_SWAP_AB");
    const std::string v = e && *e ? std::string(e) : get("swap_ab", "");
    const int want = v.empty() || v == "default" ? -1 : (v == "0" || v == "off" ? 0 : 1);
    if (input::pad_fx().swap_ab.exchange(want) != want) icons::set_mapping(input::hub().mapping());
  }
  {   // button icon mod: live, the renderer reads it on its next draw; SNAP_BUTTON_ICONS (auto|switch|xbox|playstation) overrides the file for test runs
    const char* e = std::getenv("SNAP_BUTTON_ICONS");
    icons::set_style(icons::parse_style(e && *e ? std::string(e) : get("button_icons", "auto")));
  }
  {   // Look row (gfx_look "original" / "enhanced"; erased = the build's default): the column of hle/visual_defaults.h every unset visual row follows; SNAP_VISUALS set by hand wins
    const std::string l = get("gfx_look", "");
    snapdef::saved_look().store(l == "original" ? 0 : l == "enhanced" ? 1 : -1, std::memory_order_relaxed);
  }
  for (const char* k : {"access_cvd", "access_cvd_strength", "access_ui_size", "access_contrast", "access_flash", "access_mono"})   // Accessibility rows: live; a SNAP_ACCESS_* variable set by hand wins
    if (!env_wins(k)) hle::access::set(k, get(k, ""));
  if (!env) { export_live(); return; }   // a row changed while the game runs: the live rows apply now
  // next start: env-var mods
  std::string look = get("look", "neutral");
  if (look != "neutral" && !look.empty()) {
    char b[64];
    std::snprintf(b, sizeof b, "%s:%.2f", look.c_str(), get_float("look_intensity", 1.0f));
    set_env_if_unset("SNAP_LOOK", b);
  }
  for (auto& m : kEnvKeys) {
    std::string v = get(m.key, "");
    if (v.empty() || v == "default") continue;
    set_env_if_unset(m.env, v);
  }
  {   // post-process toggles: every gfx_pass_* key set to "off" adds its pass names to SNAP_PASS_OFF (host/mods/gamemods.cpp)
    std::string off;
    for (auto& m : kPassKeys) if (get(m.key, "") == "off") off += (off.empty() ? "" : ",") + std::string(m.passes);
    set_env_if_unset("SNAP_PASS_OFF", off);
  }
  {   // Shiny Pokemon row (runtime/nvn/nvn_shiny.cpp): key "shiny" = the odds (4096 or 512) -> SNAP_SHINY=1 + SNAP_SHINY_ODDS; unset = off
    std::string odds = get("shiny", "");
    if (!odds.empty() && odds != "default" && odds != "0" && !std::getenv("SNAP_SHINY")) {
      set_env_if_unset("SNAP_SHINY", "1");
      set_env_if_unset("SNAP_SHINY_ODDS", odds);
    }
  }
  std::string ws = get("win_size", "");   // "WxH": windowed client size (the remembered window.json size otherwise)
  int w = 0, h = 0;
  if (std::sscanf(ws.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180) {
    set_env_if_unset("SNAP_WINDOW_W", std::to_string(w));
    set_env_if_unset("SNAP_WINDOW_H", std::to_string(h));
  }
}

const char* env_name(const std::string& key) {
  if (key == "look" || key == "look_intensity") return "SNAP_LOOK";
  if (key == "win_size") return "SNAP_WINDOW_W";
  if (key == "gfx_look") return "SNAP_VISUALS";
  if (key == "shiny") return "SNAP_SHINY";
  for (auto& m : kPassKeys) if (key == m.key) return "SNAP_PASS_OFF";
  for (auto& m : kEnvKeys) if (key == m.key) return m.env;
  return nullptr;
}

bool env_wins(const std::string& key) {
  const char* n = env_name(key);
  if (!n) return false;
  const char* cur = std::getenv(n);
  if (!cur || !*cur) return false;
  std::lock_guard<std::mutex> g(g_mu);
  for (auto& e : g_exported) if (e == n) return false;   // we set it ourselves from this file
  return true;
}

bool has(const std::string& key) {
  std::lock_guard<std::mutex> g(g_mu);
  return g_kv.count(key) != 0;
}

void erase(const std::string& key) {
  { std::lock_guard<std::mutex> g(g_mu); g_over.erase(key); g_kv.erase(key); write_file(); }
  apply(false);
}

}  // namespace hle::settings
