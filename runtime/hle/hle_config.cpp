// SPDX-License-Identifier: MIT
#include "hle_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "hle.h"
#include "json_lite.h"

namespace hle {

static Config g_cfg;
static bool g_cfg_init = false;

static std::string join(const std::string& a, const char* b) {
#ifdef _WIN32
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

std::string default_data_root() {
#ifdef _WIN32
  const char* a = std::getenv("APPDATA");
  return std::string(a ? a : ".") + "\\SnapRecomp";
#else
  const char* h = std::getenv("HOME");
  return std::string(h ? h : ".") + "/.local/share/SnapRecomp";
#endif
}

std::string default_input_path() {
  const char* d = std::getenv("SNAP_CONFIG_DIR");
  if (d && *d) return join(d, "input.json");
  return join(g_cfg.data_root.empty() ? default_data_root() : g_cfg.data_root, "input.json");
}
std::string legacy_input_path() { return join(join(g_cfg.data_root.empty() ? default_data_root() : g_cfg.data_root, "config"), "input_map.json"); }

std::string uid_dir_name(const uint64_t uid[2]) {
  char b[40];
  std::snprintf(b, sizeof b, "%016llx%016llx", (unsigned long long)uid[0], (unsigned long long)uid[1]);
  return b;
}

void apply_defaults() {
  Config& c = g_cfg;
  if (c.data_root.empty()) c.data_root = default_data_root();
  if (c.save_root.empty()) c.save_root = join(c.data_root, "save");
  if (c.input_map_path.empty()) c.input_map_path = default_input_path();
  if (c.screenshot_dir.empty()) c.screenshot_dir = join(c.data_root, "screenshots");
  g_cfg_init = true;
}

Config& config() {
  if (!g_cfg_init) apply_defaults();
  return g_cfg;
}

static bool parse_uid(const std::string& s, uint64_t out[2]) {
  if (s.size() != 32) return false;
  for (char ch : s) if (!std::isxdigit((unsigned char)ch)) return false;
  out[0] = std::strtoull(s.substr(0, 16).c_str(), nullptr, 16);
  out[1] = std::strtoull(s.substr(16).c_str(), nullptr, 16);
  return true;
}

bool load_config(const std::string& path_in) {
  Config& c = g_cfg;
  c = Config();
  std::string path = path_in;
  if (path.empty()) {
    const char* e = std::getenv("SNAP_HLE_CONFIG");
    if (e && *e) path = e;
    else path = join(join(default_data_root(), "config"), "hle.json");
  }
  bool ok = true;
  std::ifstream f(path, std::ios::binary);
  if (f) {
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    std::string err;
    if (!json_parse(ss.str(), j, err) || j.type != Json::Obj) {
      log("config %s: invalid JSON (%s); using defaults", path.c_str(), err.c_str());
      ok = false;
    } else {
      if (auto* v = j.get("romfs_root")) c.romfs_root = v->str(c.romfs_root);
      if (auto* v = j.get("rom_layers")) for (auto& e : v->a) if (e.type == Json::Str) c.rom_layers.push_back(e.s);
      if (auto* v = j.get("data_root")) c.data_root = v->str();
      if (auto* v = j.get("save_root")) c.save_root = v->str();
      if (auto* v = j.get("save_backups")) c.save_backups = (int)v->num(c.save_backups);
      if (auto* v = j.get("save_data_size")) c.save_data_size = (int64_t)v->num((double)c.save_data_size);
      if (auto* v = j.get("save_journal_size")) c.save_journal_size = (int64_t)v->num((double)c.save_journal_size);
      if (auto* v = j.get("user_name")) c.user_name = v->str(c.user_name);
      if (auto* v = j.get("uid")) {
        if (!parse_uid(v->str(), c.uid)) log("config: \"uid\" must be 32 hex digits; keeping the default");
      }
      if (auto* v = j.get("language")) c.language = (int)v->num(c.language);
      if (auto* v = j.get("region")) c.region = v->str(c.region);
      if (auto* v = j.get("docked")) c.docked = v->boolean(c.docked);
      if (auto* v = j.get("display_version")) c.display_version = v->str(c.display_version);
      if (auto* v = j.get("headless")) c.headless = v->boolean(c.headless);
      if (auto* v = j.get("input_map")) c.input_map_path = v->str();
      if (auto* v = j.get("screenshot_dir")) c.screenshot_dir = v->str();
      if (auto* v = j.get("controller_style")) {
        c.controller_style = (v->type == Json::Str && v->s == "handheld") ? 1 : (int)v->num(0);
      }
    }
  }
  if (const char* e = std::getenv("SNAP_ROMFS")) if (*e) c.romfs_root = e;
  if (const char* e = std::getenv("SNAP_SAVE_ROOT")) if (*e) c.save_root = e;
  if (const char* e = std::getenv("SNAP_SAVE_DIR")) if (*e) c.save_root = e;   // wins over SNAP_SAVE_ROOT; default %APPDATA%\SnapRecomp\save
  if (const char* e = std::getenv("SNAP_HLE_HEADLESS")) if (*e && *e != '0') c.headless = true;
  g_cfg_init = false;
  apply_defaults();
  return ok;
}

}  // namespace hle
