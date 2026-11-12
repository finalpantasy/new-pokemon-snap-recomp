// SPDX-License-Identifier: MIT
#include "mods_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../hle/json_lite.h"

namespace fs = std::filesystem;

namespace mods {

uint32_t crc32(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

static std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

uint32_t ext_crc(const std::string& ext) {
  if (ext.size() == 8 && std::all_of(ext.begin(), ext.end(), [](unsigned char ch) { return std::isxdigit(ch); }))
    return (uint32_t)std::strtoul(ext.c_str(), nullptr, 16);
  std::string l = lower(ext);
  return crc32(l.data(), l.size());
}

std::string norm_archive(const std::string& a) {
  std::string s = a;
  for (auto& ch : s) if (ch == '\\') ch = '/';
  size_t colon = s.find(":/");
  if (colon != std::string::npos && colon < 12) s = s.substr(colon + 2);
  while (!s.empty() && s[0] == '/') s.erase(0, 1);
  return lower(s);
}

static bool ends_with_ci(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && lower(s.substr(s.size() - suf.size())) == lower(suf);
}

bool parse_pack_path(const std::string& rel_in, std::string* archive, std::string* stem, uint32_t* extcrc) {
  std::string rel = rel_in;
  for (auto& ch : rel) if (ch == '\\') ch = '/';
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) j = rel.size();
    if (j > i) parts.push_back(rel.substr(i, j - i));
    i = j + 1;
  }
  size_t da = parts.size();
  for (size_t k = 0; k + 1 < parts.size(); k++)
    if (ends_with_ci(parts[k], ".drp")) { da = k; break; }
  if (da >= parts.size() || da + 2 != parts.size()) return false;
  const std::string& fn = parts.back();
  size_t dot = fn.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= fn.size()) return false;
  std::string ext = fn.substr(dot + 1);
  std::string st = fn.substr(0, dot);
  if (ends_with_ci(st, "." + ext)) st.resize(st.size() - ext.size() - 1);
  std::string arch;
  for (size_t k = 0; k <= da; k++) { if (k) arch += '/'; arch += parts[k]; }
  *archive = norm_archive(arch);
  *stem = lower(st);
  *extcrc = ext_crc(ext);
  return true;
}

std::string make_key(const std::string& archive_norm, const std::string& stem_lower, uint32_t extcrc) {
  char buf[16];
  snprintf(buf, sizeof buf, "|%08x|", extcrc);
  return archive_norm + buf + stem_lower;
}

void Index::add(int pack, const std::string& archive, const std::string& stem, uint32_t extcrc, const std::string& file) {
  std::string key = make_key(archive, stem, extcrc);
  auto it = map.find(key);
  if (it != map.end()) {   // a higher-priority pack was added first
    conflicts.push_back(key + ": " + packs[it->second.pack].id + " beats " + packs[pack].id);
    return;
  }
  Override o;
  o.file = file;
  o.pack = pack;
  o.label = archive + "/" + stem;
  map.emplace(std::move(key), std::move(o));
}

const Override* Index::find(const std::string& archive_norm, const std::string& name, uint32_t extcrc) const {
  std::string l = lower(name);
  auto it = map.find(make_key(archive_norm, l, extcrc));
  if (it != map.end()) return &it->second;
  size_t dot = l.rfind('.');
  if (dot != std::string::npos && ext_crc(l.substr(dot + 1)) == extcrc) {
    it = map.find(make_key(archive_norm, l.substr(0, dot), extcrc));
    if (it != map.end()) return &it->second;
  }
  return nullptr;
}

std::string default_root() {
  if (const char* e = std::getenv("SNAP_MODS_DIR")) if (*e) return e;
  const char* ap = std::getenv("APPDATA");
  return std::string(ap ? ap : ".") + "\\SnapRecomp\\mods";
}

static bool read_text(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static void read_pack_json(Pack& p, std::vector<std::string>& warn) {
  std::string txt;
  if (!read_text(fs::path(p.dir) / "pack.json", &txt)) { warn.push_back(p.id + ": no pack.json (name and version unknown)"); p.name = p.id; return; }
  hle::Json j;
  std::string err;
  if (!hle::json_parse(txt, j, err)) { warn.push_back(p.id + ": pack.json: " + err); p.name = p.id; return; }
  if (auto* v = j.get("name")) p.name = v->str();
  if (auto* v = j.get("version")) p.version = v->str();
  if (auto* v = j.get("author")) p.author = v->str();
  if (auto* v = j.get("description")) p.description = v->str();
  if (p.name.empty()) p.name = p.id;
}

void load(const std::string& root, Index& out, std::vector<std::string>& warn) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  std::vector<std::pair<std::string, bool>> order;   // (folder, enabled)
  std::string txt;
  if (read_text(fs::path(root) / "mods.json", &txt)) {
    hle::Json j;
    std::string err;
    if (!hle::json_parse(txt, j, err)) warn.push_back("mods.json: " + err);
    else if (auto* arr = j.get("packs")) {
      for (auto& e : arr->a) {
        if (e.type == hle::Json::Str) order.push_back({e.s, true});
        else if (e.type == hle::Json::Obj) {
          std::string id = e.get("id") ? e.get("id")->str() : e.get("folder") ? e.get("folder")->str() : "";
          if (!id.empty()) order.push_back({id, e.get("enabled") ? e.get("enabled")->boolean(true) : true});
        }
      }
    }
  } else {
    std::vector<std::string> names;
    for (auto& d : fs::directory_iterator(root, ec))
      if (d.is_directory(ec) && fs::exists(d.path() / "pack.json", ec)) names.push_back(d.path().filename().string());
    std::sort(names.begin(), names.end());
    for (auto& n : names) order.push_back({n, true});
  }
  for (auto& [id, en] : order) {
    Pack p;
    p.id = id;
    p.dir = (fs::path(root) / id).string();
    p.enabled = en;
    if (!fs::is_directory(p.dir, ec)) { warn.push_back(id + ": listed in mods.json but the folder does not exist"); continue; }
    read_pack_json(p, warn);
    out.packs.push_back(p);
    if (!en) continue;
    int idx = (int)out.packs.size() - 1;
    for (auto it = fs::recursive_directory_iterator(p.dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec)) continue;
      std::string rel = fs::relative(it->path(), p.dir, ec).generic_string();
      if (rel == "pack.json" || lower(rel).find(".drp/") == std::string::npos) continue;   // only paths below an archive folder
      std::string a, s;
      uint32_t x;
      if (!parse_pack_path(rel, &a, &s, &x)) { warn.push_back(p.id + ": ignored " + rel + " (expected <archive>.drp/<entry>.<ext>)"); continue; }
      out.add(idx, a, s, x, it->path().string());
    }
  }
}

size_t chunk_header_len(size_t name_len) { return 0x18 + ((name_len + 4) & ~(size_t)3); }

std::vector<uint8_t> build_chunk(const uint8_t* orig, size_t hdr_len, const uint8_t* data, size_t n) {
  std::vector<uint8_t> v(hdr_len + n);
  std::memcpy(v.data(), orig, hdr_len);
  if (n) std::memcpy(v.data() + hdr_len, data, n);
  uint32_t crc = crc32(data, n), total = (uint32_t)(hdr_len + n), sz = (uint32_t)n;
  std::memcpy(v.data() + 0x00, &crc, 4);
  std::memcpy(v.data() + 0x08, &total, 4);
  std::memcpy(v.data() + 0x10, &sz, 4);
  std::memcpy(v.data() + 0x14, &sz, 4);
  return v;
}

}  // namespace mods
