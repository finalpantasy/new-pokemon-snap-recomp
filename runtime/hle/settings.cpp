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
  std::string q = get("ssao_quality", "default");
  if (q == "high" || q == "ultra") set_env_if_unset("SNAP_SSAO_QUALITY", q);
}

}  // namespace hle::settings
