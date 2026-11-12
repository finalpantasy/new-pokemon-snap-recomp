// SPDX-License-Identifier: MIT
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "hle.h"
#include "hle_config.h"
#include "input_core.h"
#include "profiles.h"

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
std::string g_path;
std::vector<std::string> g_exported;   // env vars this layer set (so a restart does not pass them on as if the user had set them)

fs::path P(const std::string& s) { return fs::u8path(s); }
std::string path_str(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

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
}

void write_file() {
  if (g_path.empty()) return;
  std::error_code ec;
  fs::create_directories(P(g_path).parent_path(), ec);
  std::string tmp = g_path + ".tmp";
  {
    std::ofstream f(P(tmp), std::ios::trunc);
    f << "# per-profile settings (edited by the F1 menu)\n";
    for (auto& [k, v] : g_kv) f << k << "=" << v << "\n";
    if (!f.good()) return;
  }
  fs::rename(P(tmp), P(g_path), ec);
  if (ec) { fs::remove(P(g_path), ec); fs::rename(P(tmp), P(g_path), ec); }
}

// Settings that reach the game through an env var read at start (the F1 menu's "restart needed" options). The key is stored only when the user chose
// something; "" and "default" mean "leave the variable alone" (the game's own behavior). An env var set by hand always wins (set_env_if_unset).
const struct { const char* key; const char* env; } kEnvKeys[] = {
    {"gfx_render_scale", "SNAP_RENDER_SCALE"}, {"gfx_aniso", "SNAP_ANISO"},           {"gfx_lod_bias", "SNAP_LOD_BIAS"},
    {"gfx_shadow_res", "SNAP_SHADOW_RES"},     {"ssao_quality", "SNAP_SSAO_QUALITY"}, {"gfx_lighting", "SNAP_LIGHTING"},
    {"gfx_aa", "SNAP_AA"},                     {"gfx_sharpen", "SNAP_SHARPEN"},       {"gfx_upscaler", "SNAP_UPSCALER"},
    {"gfx_upscale_quality", "SNAP_UPSCALE_QUALITY"}, {"gfx_hdr", "SNAP_HDR"},        {"gfx_hdr_peak", "SNAP_HDR_PEAK"},
    {"gfx_hdr_paper", "SNAP_HDR_PAPER"},       {"gfx_draw_distance", "SNAP_DRAW_DISTANCE"},   // gfx_fov removed (owner 2026-11-12): a saved value is ignored
    {"gfx_full_rate_anim", "SNAP_FULL_RATE_ANIM"}, {"gfx_ui_font", "SNAP_UI_FONT_SCALE"}, {"fast_boot", "SNAP_FAST_BOOT"},
    {"photo_export", "SNAP_PHOTO_EXPORT"},     {"photo_dir", "SNAP_PHOTO_DIR"},
};

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

void load() {
  {
    std::lock_guard<std::mutex> g(g_mu);
    const profiles::Active& a = profiles::active();
    if (a.on && !a.id.empty()) g_path = path_str(fs::path(P(a.root)) / a.id / "settings.cfg");
    else g_path = path_str(P(config().data_root) / "settings.cfg");
    read_file();
  }
  apply(true);
}

std::string get(const std::string& key, const std::string& def) {
  std::lock_guard<std::mutex> g(g_mu);
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
    g_kv[key] = value;
    write_file();
  }
  apply(false);
}
void set_bool(const std::string& key, bool v) { set(key, v ? "1" : "0"); }
void set_float(const std::string& key, float v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); set(key, b); }

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
  if (!env) return;
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
  { std::lock_guard<std::mutex> g(g_mu); g_kv.erase(key); write_file(); }
  apply(false);
}

}  // namespace hle::settings
