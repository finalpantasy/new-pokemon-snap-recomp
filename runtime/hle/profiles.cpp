// SPDX-License-Identifier: MIT
// Save profiles (see profiles.h).
#include "profiles.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "hle.h"
#include "json_lite.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hle {
namespace profiles {

namespace fs = std::filesystem;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

int64_t now_seconds() { return (int64_t)std::time(nullptr); }

static std::tm local_tm(int64_t t) {
  std::time_t tt = (std::time_t)t;
  std::tm out{};
#ifdef _WIN32
  localtime_s(&out, &tt);
#else
  localtime_r(&tt, &out);
#endif
  return out;
}
std::string format_time(int64_t t) {
  if (t <= 0) return "never";
  std::tm m = local_tm(t);
  char b[40];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min);
  return b;
}
static std::string stamp(int64_t t) {
  std::tm m = local_tm(t);
  char b[40];
  std::snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min, m.tm_sec);
  return b;
}
std::string format_size(int64_t bytes) {
  char b[40];
  if (bytes < 1024) std::snprintf(b, sizeof b, "%lld B", (long long)bytes);
  else if (bytes < (1 << 20)) std::snprintf(b, sizeof b, "%.1f KB", bytes / 1024.0);
  else if (bytes < (1ll << 30)) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
  else std::snprintf(b, sizeof b, "%.2f GB", bytes / 1073741824.0);
  return b;
}

std::string sanitize_name(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 32 || std::strchr("<>:\"/\\|?*", c)) o += '_';
    else o += (char)c;
  }
  while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
  if (o.empty()) o = "profile";
  return o;
}

// ---- file helpers -------------------------------------------------------------------------------------------------

static void tree_stats(const fs::path& d, int64_t* files, int64_t* bytes) {
  std::error_code ec;
  if (!fs::exists(d, ec)) return;
  for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) { ++*files; auto sz = it->file_size(e2); if (!e2) *bytes += (int64_t)sz; }
  }
}
static int64_t tree_bytes(const fs::path& d) { int64_t f = 0, b = 0; tree_stats(d, &f, &b); return b; }

static bool read_text(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}
static bool write_atomic(const fs::path& target, const std::string& text) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    f.flush();
    if (!f) return false;
  }
  fs::rename(tmp, target, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}
static bool same_bytes(const fs::path& a, const fs::path& b) {
  std::string x, y;
  return read_text(a, &x) && read_text(b, &y) && x == y;
}

// "SNAPSAVE1\n<n>\n<check>\n"; returns false when missing or torn
static bool read_current(const fs::path& uid_dir, uint64_t* n) {
  std::ifstream f(uid_dir / "CURRENT", std::ios::binary);
  if (!f) return false;
  std::string magic;
  unsigned long long g = 0, chk = 0;
  if (!(f >> magic >> g >> chk) || magic != "SNAPSAVE1") return false;
  if ((uint32_t)chk != ((uint32_t)(g * 2654435761u) ^ 0x534e4150u)) return false;
  *n = g;
  return true;
}
static bool gen_complete(const fs::path& uid_dir, uint64_t n) {
  std::error_code ec;
  return fs::exists(uid_dir / "gens" / (std::to_string(n) + ".ok"), ec) && fs::is_directory(uid_dir / "gens" / std::to_string(n), ec);
}
static bool newest_complete(const fs::path& uid_dir, uint64_t* out) {
  std::error_code ec;
  bool found = false;
  for (fs::directory_iterator it(uid_dir / "gens", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".ok") continue;
    uint64_t n = std::strtoull(it->path().stem().string().c_str(), nullptr, 10);
    if (gen_complete(uid_dir, n) && (!found || n > *out)) { *out = n; found = true; }
  }
  return found;
}

static bool copy_uid(const fs::path& src, const fs::path& dst, std::string* err) {
  std::error_code ec;
  uint64_t n = 0;
  bool have_ptr = read_current(src, &n) && gen_complete(src, n);
  if (!have_ptr && !newest_complete(src, &n)) return true;   // nothing saved for this user
  fs::create_directories(dst / "gens", ec);
  fs::copy(src / "gens" / std::to_string(n), dst / "gens" / std::to_string(n), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) { if (err) *err = "copy failed: " + ec.message(); return false; }
  if (tree_bytes(src / "gens" / std::to_string(n)) != tree_bytes(dst / "gens" / std::to_string(n))) { if (err) *err = "copy verification failed"; return false; }
  { std::ofstream f(dst / "gens" / (std::to_string(n) + ".ok"), std::ios::binary); f << "ok\n"; if (!f) { if (err) *err = "cannot write .ok"; return false; } }
  // pointer last, atomically; when the source pointer was torn the mount repairs it from the newest complete generation
  if (have_ptr) {
    std::string cur;
    if (!read_text(src / "CURRENT", &cur) || !write_atomic(dst / "CURRENT", cur)) { if (err) *err = "cannot write CURRENT"; return false; }
  }
  return true;
}

bool copy_consistent(const std::string& src_root, const std::string& dst_root, std::string* err) {
  std::error_code ec;
  fs::create_directories(P(dst_root), ec);
  for (fs::directory_iterator it(P(src_root), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    if (!copy_uid(it->path(), P(dst_root) / it->path().filename(), err)) return false;
  }
  return true;
}

bool looks_like_save(const std::string& dir) {
  std::error_code ec;
  fs::path d = P(dir);
  if (!fs::is_directory(d, ec)) return false;
  if (fs::exists(d / "CURRENT", ec) || fs::is_directory(d / "gens", ec)) return true;
  for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_directory(e2) && (fs::exists(it->path() / "CURRENT", e2) || fs::is_directory(it->path() / "gens", e2))) return true;
  }
  return false;
}

// ---- Store --------------------------------------------------------------------------------------------------------

Store::Store(std::string root) : root_(std::move(root)) {}

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

const Profile* Store::find(const std::string& k) const {
  for (auto& p : profs_) if (p.id == k) return &p;
  for (auto& p : profs_) if (lower(p.name) == lower(k)) return &p;
  return nullptr;
}
std::string Store::save_dir(const std::string& id) const { return S(P(root_) / P(id) / "save"); }

bool Store::load() {
  profs_.clear(); pending_.clear(); last_used_.clear(); msg_.clear(); next_n_ = 1;
  fs::path jp = P(root_) / "profiles.json";
  std::error_code ec;
  if (!fs::exists(jp, ec)) return true;
  std::string text;
  Json j;
  std::string e;
  if (!read_text(jp, &text) || !json_parse(text, j, e) || j.type != Json::Obj) {
    fs::path bad = jp; bad += ".invalid";
    fs::copy_file(jp, bad, fs::copy_options::overwrite_existing, ec);
    msg_ = "profiles.json was unreadable (kept as profiles.json.invalid); profiles rebuilt from their folders.";
    for (fs::directory_iterator it(P(root_), ec), end; !ec && it != end; it.increment(ec)) {
      std::string n = it->path().filename().string();
      std::error_code e2;
      if (it->is_directory(e2) && n.size() > 1 && n[0] == 'p' && std::isdigit((unsigned char)n[1])) {
        Profile p; p.id = n; p.name = "Recovered " + n; p.created = now_seconds();
        profs_.push_back(p);
        next_n_ = std::max(next_n_, atoi(n.c_str() + 1) + 1);
      }
    }
    save();
    return false;
  }
  if (auto* v = j.get("next")) next_n_ = std::max(1, (int)v->num(1));
  if (auto* v = j.get("last_used")) last_used_ = v->str();
  if (auto* v = j.get("profiles")) for (auto& o : v->a) {
    Profile p;
    if (auto* x = o.get("id")) p.id = x->str();
    if (auto* x = o.get("name")) p.name = x->str();
    if (auto* x = o.get("created")) p.created = (int64_t)x->num();
    if (auto* x = o.get("last_played")) p.last_played = (int64_t)x->num();
    if (!p.id.empty() && !p.name.empty()) profs_.push_back(p);
  }
  if (auto* v = j.get("pending")) for (auto& o : v->a) {
    PendingOp q;
    if (auto* x = o.get("op")) q.op = x->str();
    if (auto* x = o.get("id")) q.id = x->str();
    if (!q.op.empty() && !q.id.empty()) pending_.push_back(q);
  }
  return true;
}

bool Store::save() {
  Json j = Json::object();
  j.set("version", Json::number(1));
  j.set("next", Json::number(next_n_));
  j.set("last_used", Json::string(last_used_));
  Json a = Json::array();
  for (auto& p : profs_) {
    Json o = Json::object();
    o.set("id", Json::string(p.id));
    o.set("name", Json::string(p.name));
    o.set("created", Json::number((double)p.created));
    o.set("last_played", Json::number((double)p.last_played));
    a.a.push_back(o);
  }
  j.set("profiles", a);
  Json pe = Json::array();
  for (auto& q : pending_) {
    Json o = Json::object();
    o.set("op", Json::string(q.op));
    o.set("id", Json::string(q.id));
    pe.a.push_back(o);
  }
  j.set("pending", pe);
  std::string text;
  json_dump(j, text);
  return write_atomic(P(root_) / "profiles.json", text + "\n");
}

std::string Store::next_id() {
  std::error_code ec;
  for (;;) {
    std::string id = "p" + std::to_string(next_n_++);
    if (!fs::exists(P(root_) / P(id), ec)) return id;
  }
}

std::string Store::next_free_name() const {
  for (int i = 1;; ++i) {
    std::string n = "Profile " + std::to_string(i);
    if (!find(n)) return n;
  }
}

static bool valid_name(const Store& s, const std::string& raw, const std::string& self_id, std::string* name, std::string* err) {
  size_t b = raw.find_first_not_of(" \t"), e = raw.find_last_not_of(" \t");
  std::string n = b == std::string::npos ? "" : raw.substr(b, e - b + 1);
  if (n.empty()) { if (err) *err = "The name is empty."; return false; }
  if (n.size() > 40) { if (err) *err = "The name is longer than 40 characters."; return false; }
  for (unsigned char c : n) if (c < 32) { if (err) *err = "The name has control characters."; return false; }
  if (n[0] == '_') { if (err) *err = "The name cannot start with an underscore."; return false; }
  if (const Profile* o = s.find(n)) if (o->id != self_id && lower(o->name) == lower(n)) { if (err) *err = "A profile named \"" + n + "\" already exists."; return false; }
  *name = n;
  return true;
}

std::string Store::create(const std::string& raw, std::string* err) {
  std::string name;
  if (!valid_name(*this, raw, "", &name, err)) return "";
  Profile p;
  p.id = next_id();
  p.name = name;
  p.created = now_seconds();
  std::error_code ec;
  fs::create_directories(P(save_dir(p.id)), ec);
  if (ec) { if (err) *err = "Cannot create " + save_dir(p.id) + ": " + ec.message(); return ""; }
  profs_.push_back(p);
  if (!save()) { if (err) *err = "Cannot write profiles.json"; return ""; }
  return p.id;
}

bool Store::rename(const std::string& id, const std::string& raw, std::string* err) {
  std::string name;
  if (!valid_name(*this, raw, id, &name, err)) return false;
  for (auto& p : profs_) if (p.id == id) { p.name = name; return save(); }
  if (err) *err = "No such profile.";
  return false;
}

void Store::set_last_used(const std::string& id) {
  last_used_ = id;
  for (auto& p : profs_) if (p.id == id) p.last_played = now_seconds();
  save();
}

int64_t Store::size_bytes(const std::string& id) const { return tree_bytes(P(save_dir(id))); }
bool Store::has_data(const std::string& id) const {
  int64_t f = 0, b = 0;
  tree_stats(P(save_dir(id)), &f, &b);
  return f > 0;
}

std::string Store::duplicate(const std::string& id, const std::string& raw, std::string* err) {
  if (!find(id)) { if (err) *err = "No such profile."; return ""; }
  std::string nid = create(raw, err);
  if (nid.empty()) return "";
  std::string e2;
  if (!copy_consistent(save_dir(id), save_dir(nid), &e2)) {
    if (err) *err = e2;
    std::error_code ec;
    fs::remove_all(P(root_) / P(nid), ec);   // our own half-made copy
    profs_.erase(std::remove_if(profs_.begin(), profs_.end(), [&](const Profile& p) { return p.id == nid; }), profs_.end());
    save();
    return "";
  }
  return nid;
}

std::string Store::trash_move(const Profile& p, const char* kind, std::string* err) {
  std::error_code ec;
  int64_t t = now_seconds();
  std::string base = sanitize_name(p.name) + "-" + stamp(t), folder = base;
  for (int i = 2; fs::exists(P(root_) / "_trash" / P(folder), ec); ++i) folder = base + "-" + std::to_string(i);
  fs::path dir = P(root_) / "_trash" / P(folder);
  fs::create_directories(dir, ec);
  if (ec) { if (err) *err = "Cannot create the trash folder: " + ec.message(); return ""; }
  fs::path src = P(save_dir(p.id));
  if (fs::exists(src, ec)) {
    fs::rename(src, dir / "save", ec);
    if (ec) {   // other volume or locked: copy then verify, and only then remove the source
      std::error_code e2;
      fs::copy(src, dir / "save", fs::copy_options::recursive, e2);
      int64_t f1 = 0, b1 = 0, f2 = 0, b2 = 0;
      tree_stats(src, &f1, &b1); tree_stats(dir / "save", &f2, &b2);
      if (e2 || f1 != f2 || b1 != b2) { fs::remove_all(dir, e2); if (err) *err = "Cannot move the save to the trash: " + ec.message(); return ""; }
      fs::remove_all(src, e2);
    }
  } else {
    fs::create_directories(dir / "save", ec);
  }
  Json m = Json::object();
  m.set("name", Json::string(p.name));
  m.set("kind", Json::string(kind));
  m.set("when", Json::number((double)t));
  m.set("profile_id", Json::string(p.id));
  std::string text;
  json_dump(m, text);
  write_atomic(dir / "meta.json", text + "\n");
  return folder;
}

bool Store::clear(const std::string& id, std::string* err) {
  const Profile* p = find(id);
  if (!p) { if (err) *err = "No such profile."; return false; }
  Profile copy = *p;
  if (trash_move(copy, "clear", err).empty()) return false;
  std::error_code ec;
  fs::create_directories(P(save_dir(copy.id)), ec);
  prune_trash();
  return true;
}

bool Store::remove(const std::string& id, std::string* err) {
  const Profile* p = find(id);
  if (!p) { if (err) *err = "No such profile."; return false; }
  Profile copy = *p;
  if (trash_move(copy, "delete", err).empty()) return false;
  std::error_code ec;
  fs::remove(P(root_) / P(copy.id), ec);   // empty folder only
  profs_.erase(std::remove_if(profs_.begin(), profs_.end(), [&](const Profile& q) { return q.id == copy.id; }), profs_.end());
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const PendingOp& q) { return q.id == copy.id; }), pending_.end());
  if (last_used_ == copy.id) last_used_ = profs_.empty() ? "" : profs_.front().id;
  save();
  prune_trash();
  return true;
}

std::vector<TrashEntry> Store::trash_list() const {
  std::vector<TrashEntry> v;
  std::error_code ec;
  for (fs::directory_iterator it(P(root_) / "_trash", ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    TrashEntry t;
    t.folder = it->path().filename().string();
    t.name = t.folder;
    std::string text;
    Json j;
    std::string e;
    if (read_text(it->path() / "meta.json", &text) && json_parse(text, j, e)) {
      if (auto* x = j.get("name")) t.name = x->str(t.name);
      if (auto* x = j.get("kind")) t.kind = x->str();
      if (auto* x = j.get("when")) t.when = (int64_t)x->num();
    }
    t.size = tree_bytes(it->path() / "save");
    v.push_back(t);
  }
  std::sort(v.begin(), v.end(), [](const TrashEntry& a, const TrashEntry& b) { return a.when != b.when ? a.when > b.when : a.folder > b.folder; });
  return v;
}

void Store::prune_trash(int keep) {
  auto v = trash_list();
  std::error_code ec;
  for (size_t i = (size_t)keep; i < v.size(); ++i) fs::remove_all(P(root_) / "_trash" / P(v[i].folder), ec);
}

std::string Store::restore(const std::string& folder, std::string* err) {
  fs::path dir = P(root_) / "_trash" / P(folder);
  std::error_code ec;
  if (!fs::is_directory(dir / "save", ec)) { if (err) *err = "That trash entry has no saved data."; return ""; }
  std::string name = folder;
  for (auto& t : trash_list()) if (t.folder == folder) name = t.name;
  std::string want = name + " (restored)", cand = want;
  for (int i = 2; find(cand); ++i) cand = want + " " + std::to_string(i);
  std::string id = create(cand, err);
  if (id.empty()) return "";
  fs::remove(P(save_dir(id)), ec);   // empty folder made by create
  fs::rename(dir / "save", P(save_dir(id)), ec);
  if (ec) {
    std::error_code e2;
    fs::copy(dir / "save", P(save_dir(id)), fs::copy_options::recursive, e2);
    if (e2) { if (err) *err = "Cannot restore: " + e2.message(); remove(id, nullptr); return ""; }
  }
  fs::remove_all(dir, ec);   // only meta.json (and a copied save) remain
  return id;
}

std::string Store::import_folder(const std::string& src_in, const std::string& raw, std::string* err) {
  fs::path src = P(src_in);
  std::error_code ec;
  if (fs::is_directory(src / "save", ec) && looks_like_save(S(src / "save"))) src = src / "save";
  if (!looks_like_save(S(src))) { if (err) *err = "That folder holds no save (no CURRENT file or gens folder)."; return ""; }
  std::string id = create(raw, err);
  if (id.empty()) return "";
  std::string e2;
  bool ok;
  if (fs::exists(src / "CURRENT", ec) || fs::is_directory(src / "gens", ec)) {
    // a single user folder: keep its name when it is a 32 hex digit uid, else use the default uid
    std::string n = src.filename().string();
    bool hex = n.size() == 32 && std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
    ok = copy_uid(src, P(save_dir(id)) / (hex ? n : std::string("534e4150524543310000000000000001")), &e2);
  } else {
    ok = copy_consistent(S(src), save_dir(id), &e2);
  }
  if (!ok || !has_data(id)) {
    if (err) *err = ok ? "The folder has no complete save generation." : e2;
    fs::remove_all(P(root_) / P(id), ec);
    profs_.erase(std::remove_if(profs_.begin(), profs_.end(), [&](const Profile& q) { return q.id == id; }), profs_.end());
    save();
    return "";
  }
  return id;
}

bool Store::migrate_legacy(const std::string& legacy, std::string* err) {
  std::error_code ec;
  fs::path src = P(legacy);
  if (!fs::is_directory(src, ec)) return true;
  std::string id = create("Profile 1", err);
  if (id.empty()) return false;
  fs::path dst = P(save_dir(id));
  fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  int64_t f1 = 0, b1 = 0, f2 = 0, b2 = 0;
  tree_stats(src, &f1, &b1); tree_stats(dst, &f2, &b2);
  bool ok = !ec && f1 == f2 && b1 == b2;
  if (ok) {   // pointer files must match byte for byte
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
      if (it->path().filename() == "CURRENT" && !same_bytes(it->path(), dst / fs::relative(it->path(), src))) ok = false;
  }
  if (!ok) {
    if (err) *err = "The old save could not be copied and verified; it was left where it is.";
    fs::remove_all(P(root_) / P(id), ec);
    profs_.clear();
    next_n_ = 1;
    save();
    return false;
  }
  char d[16];
  std::tm m = local_tm(now_seconds());
  std::snprintf(d, sizeof d, "%04d%02d%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday);
  fs::path moved = src;
  moved += std::string(".migrated-") + d;
  for (int i = 2; fs::exists(moved, ec); ++i) { moved = src; moved += std::string(".migrated-") + d + "-" + std::to_string(i); }
  fs::rename(src, moved, ec);
  if (ec) log("profiles: copied the old save into Profile 1 but could not rename %s: %s", legacy.c_str(), ec.message().c_str());
  else log("profiles: moved the old save into Profile 1; the original is %s", S(moved).c_str());
  return true;
}

void Store::queue(const std::string& op, const std::string& id) {
  if (!is_pending(id, op.c_str())) pending_.push_back({op, id});
  save();
}
bool Store::is_pending(const std::string& id, const char* op) const {
  for (auto& q : pending_) if (q.id == id && (!op || q.op == op)) return true;
  return false;
}
void Store::apply_pending() {
  auto ops = pending_;
  pending_.clear();
  for (auto& q : ops) {
    std::string e;
    bool ok = q.op == "delete" ? remove(q.id, &e) : clear(q.id, &e);
    if (!ok) log("profiles: pending %s of %s failed: %s", q.op.c_str(), q.id.c_str(), e.c_str());
  }
  save();
}

// ---- startup ------------------------------------------------------------------------------------------------------

static Active g_active;
const Active& active() { return g_active; }
void set_active(const Active& a) { g_active = a; }

static bool dir_nonempty(const fs::path& d) {
  std::error_code ec;
  if (!fs::is_directory(d, ec)) return false;
  return fs::directory_iterator(d, ec) != fs::directory_iterator();
}

Active startup(const std::string& root, const std::string& legacy, const std::string& want, PickerFn picker, bool force_picker) {
#ifdef _WIN32
  if (const char* w = std::getenv("SNAP_RESTART_WAIT_PID")) {   // a restart: wait for the old process to release its save
    DWORD pid = (DWORD)std::atoi(w);
    if (pid) if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) { WaitForSingleObject(h, 15000); CloseHandle(h); }
  }
#endif
  Active a;
  a.root = root;
  Store s(root);
  s.load();
  if (!s.message().empty()) a.note = s.message();
  if (s.list().empty() && !legacy.empty() && dir_nonempty(P(legacy))) {
    std::string e;
    if (!s.migrate_legacy(legacy, &e)) {
      log("profiles: %s", e.c_str());
      a.on = false;
      a.save_dir = legacy;
      a.note = e;
      return a;
    }
    a.note = "Your old save was moved into \"Profile 1\". The original folder was kept as save.migrated-<date>.";
  }
  s.apply_pending();
  std::string e;
  if (s.list().empty()) s.create("Profile 1", &e);
  std::string id;
  if (!want.empty()) {
    if (const Profile* p = s.find(want)) id = p->id;
    else id = s.create(want, &e);
  }
  if (id.empty() && picker && (s.list().size() > 1 || force_picker)) {
    std::vector<PickItem> items;
    for (auto& p : s.list()) items.push_back({p.name.c_str(), p.last_played, s.size_bytes(p.id)});
    int idx = picker(items.data(), (int)items.size());
    if (idx == (int)items.size()) id = s.create(s.next_free_name(), &e);
    else if (idx >= 0 && idx < (int)items.size()) id = s.list()[(size_t)idx].id;
  }
  if (id.empty()) {
    const Profile* p = s.find(s.last_used());
    id = p ? p->id : (s.list().empty() ? "" : s.list().front().id);
  }
  if (id.empty()) { a.note = "No profile could be created: " + e; a.on = false; a.save_dir = legacy; return a; }
  s.set_last_used(id);
  s.prune_trash();
  a.on = true;
  a.id = id;
  a.name = s.find(id)->name;
  a.save_dir = s.save_dir(id);
  g_active = a;
  return a;
}

}  // namespace profiles
}  // namespace hle
