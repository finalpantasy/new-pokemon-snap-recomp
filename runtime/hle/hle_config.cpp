// SPDX-License-Identifier: MIT
#include "hle_config.h"

#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "hle.h"
#include "save_gen.h"
#include "json_lite.h"
#include "profiles.h"
#include "settings.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace hle {

static bool g_save_explicit = false;

static Config g_cfg;
static bool g_cfg_init = false;

static std::string join(const std::string& a, const char* b) {
#ifdef _WIN32
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

#ifdef _WIN32
// %APPDATA% as UTF-8. getenv returns the ANSI code page, which the u8path users (fs_core, settings, profiles) would misread for a
// non-ASCII user name; _wgetenv (or the known-folder API when the variable is missing) is exact.
std::string appdata_utf8() {
  std::wstring w;
  if (const wchar_t* e = _wgetenv(L"APPDATA"); e && *e) w = e;
  else {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) w = p;
    if (p) CoTaskMemFree(p);
  }
  if (w.empty()) return ".";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
#endif

std::string default_data_root() {
#ifdef _WIN32
  return appdata_utf8() + "\\SnapRecomp";
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

// Older builds decoded %APPDATA% from the ANSI code page and read it as UTF-8, so a user name with non-ASCII letters put the saves in
// a differently spelled folder. When that folder has a save and the correct one has none, copy it over once (the old one stays).
// Both roots are UTF-8. A string that is not valid UTF-8 is no folder any build could have used (std::filesystem::u8path throws
// on it with the MSVC STL), so it is skipped, and no error here ever leaves this function: migration must never stop boot.
static bool valid_utf8(const std::string& s) {
#ifdef _WIN32
  return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0;
#else
  (void)s;
  return true;
#endif
}

// A save root holds a save when some <uid>/ under it has a loadable generation, by the mount's own rule (save_gen.h: CURRENT
// or the newest complete, non-orphaned generation). The root's .snap.lock, empty folders or a marker-less partial copy do not.
static bool has_save(const std::filesystem::path& root) { return savegen::has_save(root); }

// Every entry under `root` (except a top-level .snap.lock) as relative path -> size (-1 for directories).
using Manifest = std::map<std::string, int64_t>;
static bool manifest_of(const std::filesystem::path& root, Manifest* m, std::error_code& why) {
  namespace sf = std::filesystem;
  m->clear();
  sf::recursive_directory_iterator it(root, why), end;
  for (; !why && it != end; it.increment(why)) {
    const sf::path rel = it->path().lexically_relative(root);
    if (it.depth() == 0 && rel == ".snap.lock") continue;
    std::error_code e1;
    const bool isdir = it->is_directory(e1);
    int64_t size = -1;
    if (!e1 && !isdir) size = (int64_t)it->file_size(e1);
    if (e1) { why = e1; return false; }
    const auto u = rel.generic_u8string();
    (*m)[std::string(u.begin(), u.end())] = size;
  }
  return !why;
}

// Copies the tree `from` into the new folder `to` (except a top-level .snap.lock). Any error stops.
static bool copy_tree(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& why) {
  namespace sf = std::filesystem;
  sf::create_directory(to, why);
  if (why) return false;
  sf::recursive_directory_iterator it(from, why), end;
  for (; !why && it != end; it.increment(why)) {
    std::error_code e1;
    const sf::path rel = it->path().lexically_relative(from), dst = to / rel;
    if (it.depth() == 0 && rel == ".snap.lock") continue;
    const bool isdir = it->is_directory(e1);
    if (!e1 && isdir) sf::create_directories(dst, e1);
    else if (!e1) sf::copy_file(it->path(), dst, sf::copy_options::none, e1);
    if (e1) { why = e1; return false; }
  }
  return !why;
}

#ifdef _WIN32
// Cross-process lock for the migration: a byte lock on <new_root>/.migrate.lock (dies with its process). Waits up to `ms`.
struct MigrateLock {
  HANDLE h = INVALID_HANDLE_VALUE;
  OVERLAPPED ov{};
  bool take(const std::filesystem::path& file, int ms) {
    h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    for (int waited = 0;; waited += 50) {
      if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return true;
      if (waited >= ms) { CloseHandle(h); h = INVALID_HANDLE_VALUE; return false; }
      Sleep(50);
    }
  }
  ~MigrateLock() { if (h != INVALID_HANDLE_VALUE) { UnlockFileEx(h, 0, 1, 0, &ov); CloseHandle(h); } }
};
#endif

// Runs once per data root, under a cross-process lock (two copies starting together never share or race on a stage).
// The copy goes to a per-process stage <new_root>/save.migrating-<pid>, is checked against a manifest of the source (every
// file and folder with its size; it must also hold a loadable save), and only then renamed to <new_root>/save in one step, so
// a failed or interrupted copy never leaves a partial save where the game looks; the next start tries again. A target without
// a save (empty, or a marker-less leftover of an older build's partial copy) is moved aside to save.premigrate-<n> (never
// deleted); a target that holds a save is never touched. <new_root>/.legacy-migrated records that the question is settled
// (copied, or the new root already had a save), so a save later moved away (profiles) is not re-migrated as a stale copy.
bool migrate_legacy_data_root(const std::string& old_root, const std::string& new_root) {
  namespace sf = std::filesystem;
  try {
    std::error_code ec;
    if (old_root.empty() || old_root == new_root) return false;
    if (!valid_utf8(old_root) || !valid_utf8(new_root)) {
      log("config: legacy data folder name is not valid UTF-8; skipping the save migration");
      return false;
    }
    const sf::path nr = sf::u8path(new_root), o = sf::u8path(old_root) / "save", n = nr / "save", done = nr / ".legacy-migrated";
    if (sf::exists(done, ec) || !sf::is_directory(o, ec) || !has_save(o)) return false;
    sf::create_directories(nr, ec);
    auto settle = [&]() { std::ofstream(done, std::ios::binary) << "legacy save migration settled\n"; };
#ifdef _WIN32
    MigrateLock lock;
    if (!lock.take(nr / ".migrate.lock", 120000)) { log("config: save migration: another copy holds the migration lock; will retry at the next start"); return false; }
    const sf::path stage = nr / ("save.migrating-" + std::to_string(GetCurrentProcessId()));
#else
    const sf::path stage = nr / "save.migrating";
#endif
    if (sf::exists(done, ec)) return false;   // another copy finished while this one waited
    if (has_save(n)) { settle(); return false; }
    // stages of crashed earlier attempts (no one else is migrating: we hold the lock)
    for (sf::directory_iterator it(nr, ec), end; !ec && it != end; it.increment(ec)) {
      const auto f = it->path().filename().u8string();
      if (f.rfind(u8"save.migrating", 0) == 0) { std::error_code e2; sf::remove_all(it->path(), e2); }
    }
    if (sf::exists(stage, ec)) { log("config: save migration: cannot clear the staging folder; will retry at the next start"); return false; }
    std::error_code why;
    Manifest want, got;
    bool ok = manifest_of(o, &want, why) && copy_tree(o, stage, why) && manifest_of(stage, &got, why);
    if (ok && want != got) { ok = false; why = std::make_error_code(std::errc::io_error); }
    if (ok && !has_save(stage)) { ok = false; why = std::make_error_code(std::errc::no_such_file_or_directory); }
    if (!ok) {
      std::error_code e2;
      sf::remove_all(stage, e2);
      log("config: save migration from %s failed while copying or checking (%s); will retry at the next start", old_root.c_str(), why.message().c_str());
      return false;
    }
    sf::path aside;
    if (sf::exists(n, ec)) {
      for (int k = 1; k < 1000 && aside.empty(); ++k)
        if (!sf::exists(nr / ("save.premigrate-" + std::to_string(k)), ec)) aside = nr / ("save.premigrate-" + std::to_string(k));
      sf::rename(n, aside, ec);
      if (ec || aside.empty()) {
        std::error_code e2;
        sf::remove_all(stage, e2);
        log("config: save migration: cannot move the empty save folder aside (%s); will retry at the next start", ec.message().c_str());
        return false;
      }
    }
    sf::rename(stage, n, ec);
    if (ec) {
      std::error_code e2;
      if (!aside.empty()) sf::rename(aside, n, e2);
      sf::remove_all(stage, e2);
      log("config: save migration: cannot move the copy into place (%s); will retry at the next start", ec.message().c_str());
      return false;
    }
    settle();
    log("config: copied saves from the old data folder %s to %s", old_root.c_str(), new_root.c_str());
    return true;
  } catch (const std::exception& e) {
    log("config: save migration from the old data folder failed (%s); continuing", e.what());
  } catch (...) {
    log("config: save migration from the old data folder failed; continuing");
  }
  return false;
}

#ifdef _WIN32
// The folder older builds used: %APPDATA% as the ANSI code page bytes (getenv), read as if they were UTF-8. Built from the wide
// variable; empty when those bytes are not valid UTF-8 (such a build could not open the folder, so there is nothing to move).
static std::string legacy_appdata_spelling() {
  const wchar_t* w = _wgetenv(L"APPDATA");
  if (!w || !*w) return "";
  const int wn = (int)wcslen(w);
  int n = WideCharToMultiByte(CP_ACP, 0, w, wn, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "";
  std::string ansi((size_t)n, '\0');
  WideCharToMultiByte(CP_ACP, 0, w, wn, ansi.data(), n, nullptr, nullptr);
  return valid_utf8(ansi) ? ansi : std::string();
}
#endif

void apply_defaults() {
  Config& c = g_cfg;
  if (c.data_root.empty()) {
    c.data_root = default_data_root();
#ifdef _WIN32
    if (std::string a = legacy_appdata_spelling(); !a.empty()) migrate_legacy_data_root(a + "\\SnapRecomp", c.data_root);
#endif
  }
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
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
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
      if (auto* v = j.get("player_name")) { if (!v->str().empty()) c.player_name = v->str(); }
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
  if (const char* e = std::getenv("SNAP_PLAYER_NAME")) if (*e) c.player_name = e;
  if (const char* e = std::getenv("SNAP_SAVE_DIR")) if (*e) c.save_root = e;   // wins over SNAP_SAVE_ROOT; default %APPDATA%\SnapRecomp\save
  if (const char* e = std::getenv("SNAP_HLE_HEADLESS")) if (*e && *e != '0') c.headless = true;
  g_save_explicit = !c.save_root.empty();
  g_cfg_init = false;
  apply_defaults();
  return ok;
}

// Picker window (runtime/ui/ui_picker.cpp); the stub keeps builds without it (unit tests) linking.
extern "C" int snap_ui_pick_profile(const profiles::PickItem* items, int count);
extern "C" int snap_ui_pick_profile_none(const profiles::PickItem*, int) { return -1; }
#pragma comment(linker, "/alternatename:snap_ui_pick_profile=snap_ui_pick_profile_none")

void activate_profiles() {
  Config& c = config();
  const char* pd = std::getenv("SNAP_PROFILES_DIR");
  bool has_pd = pd && *pd;
  if (g_save_explicit && !has_pd) return;   // SNAP_SAVE_DIR / save_root override everything (test runs)
  std::string root = has_pd ? std::string(pd) : join(c.data_root, "profiles");
  std::string legacy = has_pd ? std::string() : join(c.data_root, "save");
  if (const char* e = std::getenv("SNAP_LEGACY_SAVE_DIR")) legacy = e;
  const char* want = std::getenv("SNAP_PROFILE");
  const char* pk = std::getenv("SNAP_PROFILE_PICKER");
  bool no_picker = c.headless || (pk && *pk == '0');
  bool force = (pk && *pk == '1');
#ifdef _WIN32
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) force = true;   // hold Shift while starting to pick a profile even with only one
#endif
  profiles::Active a = profiles::startup(root, legacy, want ? want : "", no_picker ? nullptr : snap_ui_pick_profile, force);
  if (!a.save_dir.empty()) c.save_root = a.save_dir;
  profiles::set_active(a);
  log("profiles: %s%s%s -> %s", a.on ? "profile " : "profiles off", a.on ? a.name.c_str() : "", a.note.empty() ? "" : " (note)", c.save_root.c_str());
  settings::load();   // this profile's settings.cfg: live controller options, and env vars for the look / SSAO mods (read later, at their first use)
}

}  // namespace hle
