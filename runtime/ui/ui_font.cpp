// SPDX-License-Identifier: MIT
#include "ui_font.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hle/hle_config.h"

namespace snap::ui {

namespace {

bool read_file(const std::string& p, std::vector<unsigned char>& d) {
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 16 || n > 64L * 1024 * 1024) { std::fclose(f); return false; }
  d.resize((size_t)n);
  bool ok = std::fread(d.data(), 1, d.size(), f) == d.size();
  std::fclose(f);
  return ok;
}

bool decode(std::vector<unsigned char>& d) {
  if (d.size() < 16) return false;
  uint32_t magic;
  std::memcpy(&magic, d.data(), 4);
  const uint32_t key = magic ^ 0x18029A7Fu;
  unsigned char k[4];
  std::memcpy(k, &key, 4);
  std::vector<unsigned char> o(d.size() - 8);
  for (size_t i = 0; i < o.size(); ++i) o[i] = d[8 + i] ^ k[i & 3];
  const bool ttf = o[0] == 0 && o[1] == 1 && o[2] == 0 && o[3] == 0;   // sfnt version 1.0
  if (ttf) d.swap(o);
  return ttf;
}

std::mutex g_mu;
bool g_tried = false, g_ok = false;
std::vector<unsigned char> g_data;

}  // namespace

bool game_font_ttf(std::vector<unsigned char>& out) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_tried) {
    g_tried = true;
    const char* rel = "/drp-v262144/nx/Pack/Resident/master/Resident.drp/AsiaKNGD-E.bfttf";
    std::vector<std::string> cands;
    if (const char* e = std::getenv("SNAP_GAME_FONT")) cands.push_back(e);
    std::string rf = hle::config().romfs_root;
    for (char& c : rf) if (c == '\\') c = '/';
    while (!rf.empty() && rf.back() == '/') rf.pop_back();
    const size_t sl = rf.find_last_of('/');
    cands.push_back((sl == std::string::npos ? std::string(".") : rf.substr(0, sl)) + rel);
    cands.push_back(std::string("extracted") + rel);
    for (const std::string& c : cands) {
      std::vector<unsigned char> d;
      if (read_file(c, d) && decode(d)) { g_data.swap(d); g_ok = true; break; }
    }
  }
  if (!g_ok) return false;
  out = g_data;
  return true;
}

const char* game_font_family() { return "AsiaKNGD-E"; }

}  // namespace snap::ui
