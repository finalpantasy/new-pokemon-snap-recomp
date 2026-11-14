// SPDX-License-Identifier: MIT
// Shiny Pokemon mod, photo side (docs/shiny-mod-plan.md M3; renderer side: runtime/nvn/nvn_shiny.cpp, effect: runtime/ui/ui_shiny.cpp).
// When the game saves a photo while a shiny instance was drawn in the last two seconds, the JPEG's CRC32 and the species go to a
// host sidecar (shiny_photos.jsonl in the pack folder; never the game save) and the capture effect (sparkles, badge, chime) starts.
// Off unless SNAP_SHINY=1. Called from the JpegEncoder::Encode override in host/mods/photoexport.cpp.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" int snap_shiny_recent(int frames, char* species, int n);   // runtime/nvn/nvn_shiny.cpp
extern "C" void snap_shiny_fx_trigger(const char* species);          // runtime/ui/ui_shiny.cpp

namespace { void note_crc(uint32_t crc); }   // below
namespace {
uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}
bool shiny_on() {
  static const bool on = [] { const char* e = getenv("SNAP_SHINY"); return e && *e && *e != '0'; }();
  return on;
}
}  // namespace

extern "C" void snap_shiny_photo_encoded(const uint8_t* jpg, size_t n) {
  if (!shiny_on() || !jpg || !n || n > (1u << 26)) return;
  char sp[64] = {};
  static const int window = [] { const char* e = getenv("SNAP_SHINY_WINDOW"); return e && atoi(e) > 0 ? atoi(e) : 120; }();
  if (!snap_shiny_recent(window, sp, sizeof sp)) return;
  const uint32_t crc = crc32(jpg, n);
  note_crc(crc);   // badge it at once
  std::string dir;
  if (const char* d = getenv("SNAP_SHINY_DIR")) dir = d;
  else if (const char* a = getenv("APPDATA")) dir = std::string(a) + "\\SnapRecomp\\mods\\shiny-pokemon";
  if (!dir.empty()) {
    SYSTEMTIME t; GetLocalTime(&t);
    if (FILE* f = fopen((dir + "\\shiny_photos.jsonl").c_str(), "ab")) {
      fprintf(f, "{\"jpeg_crc32\": \"%08x\", \"jpeg_bytes\": %zu, \"species\": \"%s\", \"time\": \"%04d-%02d-%02d %02d:%02d:%02d\"}\n", crc, n, sp, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      fclose(f);
    }
  }
  fprintf(stderr, "[shiny] photo saved with a shiny %s in view: jpeg crc32 %08x\n", sp, crc);
  static std::chrono::steady_clock::time_point last{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last > std::chrono::milliseconds(2500)) { last = now; snap_shiny_fx_trigger(sp); }   // a capture also encodes thumbnails: one effect
}

// ---- Badge on shiny photos (M3): the game decodes every photo it shows (photo select, evaluation, album, thumbnails) with
// nn::image::JpegDecoder::DecodeWithPreconfiguredDecoder from the saved JPEG. When the JPEG's CRC32 is in the host sidecar, a
// gold star badge is composited into the DECODED pixel buffer, a transient copy, so the save and the JPEG stay vanilla and every
// screen that shows the photo shows the badge.
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader.h"

namespace {
using namespace loader;
const char* kDecode = "_ZN2nn5image11JpegDecoder30DecodeWithPreconfiguredDecoderEPvmiPKvmRKS1_S2_m";
const char* kSetFmt = "_ZN2nn5image11JpegDecoder14SetPixelFormatENS0_11PixelFormatE";
std::mutex g_crcMu;
std::unordered_set<uint32_t> g_shinyCrc;
std::unordered_map<uint64_t, int> g_fmt;   // decoder object -> pixel format
bool g_loaded = false;

uint32_t crc32t(const uint8_t* p, size_t n) {
  static uint32_t tab[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); tab[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}

void load_sidecar();
void note_crc(uint32_t crc) { std::lock_guard<std::mutex> lk(g_crcMu); if (!g_loaded) load_sidecar(); g_shinyCrc.insert(crc); }

void load_sidecar() {
  g_loaded = true;
  std::string dir;
  if (const char* d = getenv("SNAP_SHINY_DIR")) dir = d;
  else if (const char* a = getenv("APPDATA")) dir = std::string(a) + "\\SnapRecomp\\mods\\shiny-pokemon";
  FILE* f = dir.empty() ? nullptr : fopen((dir + "\\shiny_photos.jsonl").c_str(), "rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    const char* k = strstr(line, "jpeg_crc32\": \"");
    if (k) g_shinyCrc.insert((uint32_t)strtoul(k + 14, nullptr, 16));
  }
  fclose(f);
}

bool jpeg_size(const uint8_t* p, size_t n, int* w, int* h) {   // SOF0..SOF2 marker
  for (size_t i = 2; i + 9 < n;) {
    if (p[i] != 0xFF) { i++; continue; }
    const uint8_t m = p[i + 1];
    if (m == 0xFF) { i++; continue; }
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) { *h = (p[i + 5] << 8) | p[i + 6]; *w = (p[i + 7] << 8) | p[i + 8]; return true; }
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
    i += 2 + ((p[i + 2] << 8) | p[i + 3]);
  }
  return false;
}

// Signed distance (pixels, negative inside) to a pts-point star centered at c.
float star_sd(float px, float py, float cx, float cy, float R, float r, int pts) {
  float vx[12], vy[12];
  for (int i = 0; i < pts * 2; i++) { const float a = -1.5707963f + i * 3.14159265f / pts, rr = (i & 1) ? r : R; vx[i] = cx + std::cos(a) * rr; vy[i] = cy + std::sin(a) * rr; }
  bool in = false; float best = 1e9f;
  for (int i = 0, j = pts * 2 - 1; i < pts * 2; j = i++) {
    if (((vy[i] > py) != (vy[j] > py)) && (px < (vx[j] - vx[i]) * (py - vy[i]) / (vy[j] - vy[i]) + vx[i])) in = !in;
    const float ex = vx[j] - vx[i], ey = vy[j] - vy[i], l2 = ex * ex + ey * ey;
    float t = l2 > 0 ? ((px - vx[i]) * ex + (py - vy[i]) * ey) / l2 : 0; t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float dx = px - (vx[i] + t * ex), dy = py - (vy[i] + t * ey);
    best = std::min(best, std::sqrt(dx * dx + dy * dy));
  }
  return in ? -best : best;
}

void blend(uint8_t* px, float a, int r, int g, int b) {
  if (a <= 0) return; if (a > 1) a = 1;
  px[0] = (uint8_t)(px[0] + (r - px[0]) * a); px[1] = (uint8_t)(px[1] + (g - px[1]) * a); px[2] = (uint8_t)(px[2] + (b - px[2]) * a);
}

void paint_badge(uint8_t* buf, int w, int h, size_t stride) {
  const float s = std::max(8.0f, std::min(w, h) * 0.085f);   // star outer radius
  struct St { float cx, cy, R; int pts; } sts[4] = {{s * 1.55f, s * 1.55f, s, 5}, {s * 3.0f, s * 0.75f, s * 0.38f, 4}, {s * 0.7f, s * 3.1f, s * 0.34f, 4}, {s * 3.1f, s * 2.7f, s * 0.26f, 4}};
  for (auto& st : sts) {
    const float inner = st.pts == 5 ? 0.45f : 0.2f, ow = std::max(1.5f, s * 0.14f);
    const int x0 = std::max(0, (int)(st.cx - st.R - ow * 2 - 2)), x1 = std::min(w - 1, (int)(st.cx + st.R + ow * 2 + 2));
    const int y0 = std::max(0, (int)(st.cy - st.R - ow * 2 - 2)), y1 = std::min(h - 1, (int)(st.cy + st.R + ow * 2 + 2));
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        const float sd = star_sd(x + 0.5f, y + 0.5f, st.cx, st.cy, st.R, st.R * inner, st.pts);
        uint8_t* px = buf + (size_t)y * stride + (size_t)x * 4;
        blend(px, 0.85f * std::clamp(0.5f - (sd - ow * 1.6f), 0.0f, 1.0f), 217, 116, 58);   // orange rim
        blend(px, std::clamp(0.5f - (sd - ow * 0.7f), 0.0f, 1.0f), 255, 250, 238);           // white ring
        blend(px, std::clamp(0.5f - sd, 0.0f, 1.0f), 248, 213, 71);                          // gold fill (the game's selected yellow)
      }
  }
}

void h_decode(Cpu* c) {
  const uint64_t a[8] = {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]};
  const uint64_t orig = lookup(kDecode);
  const uint64_t res = orig ? call_guest_nested(c, orig, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}) : 0;
  c->x[0] = res;
  if (!shiny_on() || (uint32_t)res != 0 || !a[0] || !a[3] || a[4] < 4 || a[4] > (1u << 26)) return;
  bool hit;
  const uint32_t crc = crc32t(reinterpret_cast<const uint8_t*>(a[3]), (size_t)a[4]);
  { std::lock_guard<std::mutex> lk(g_crcMu); if (!g_loaded) load_sidecar(); hit = g_shinyCrc.count(crc) != 0; }
  { static int tr = 0; static const bool trace = getenv("SNAP_SHINY_TRACE") != nullptr; if (trace && tr++ < 40) fprintf(stderr, "[shiny] decode: %llu bytes crc %08x %s\n", (unsigned long long)a[4], crc, hit ? "SHINY" : "plain"); }
  if (!hit) return;
  int w = 0, h = 0, fmt = 14;
  { std::lock_guard<std::mutex> lk(g_crcMu); auto it = g_fmt.find(a[5]); if (it != g_fmt.end()) fmt = it->second; }
  if (!jpeg_size(reinterpret_cast<const uint8_t*>(a[3]), (size_t)a[4], &w, &h) || w < 32 || h < 32) return;
  size_t stride = (size_t)w * 4;
  const int align = (int)a[2];
  if (align > 1) stride = (stride + align - 1) / align * align;
  if (fmt != 14 || stride * (size_t)h > a[1]) { static int n = 0; if (n++ < 4) fprintf(stderr, "[shiny] photo badge skipped: format %d %dx%d stride %zu buffer %llu\n", fmt, w, h, stride, (unsigned long long)a[1]); return; }
  paint_badge(reinterpret_cast<uint8_t*>(a[0]), w, h, stride);
  static int n = 0;
  if (n++ < 12) fprintf(stderr, "[shiny] photo badge drawn on a decoded shiny photo (crc %08x, %dx%d)\n", crc, w, h);
}

void h_setfmt(Cpu* c) {
  { std::lock_guard<std::mutex> lk(g_crcMu); g_fmt[c->x[0]] = (int)c->x[1]; }
  const uint64_t o = lookup(kSetFmt);
  if (o) c->x[0] = call_guest_nested(c, o, {c->x[0], c->x[1]});
}
}  // namespace

namespace loader {
void snap_shiny_install() {
  add_override(kDecode, h_decode);
  add_override(kSetFmt, h_setfmt);
}
}  // namespace loader
