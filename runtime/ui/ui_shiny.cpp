// SPDX-License-Identifier: MIT
// Shiny capture effect: a game-styled "Shiny <name>!" badge with a star, gold sparkles and a short chime, shown for about 2.6 s
// after a photo with a shiny Pokemon is saved (host/mods/shiny.cpp). Host overlay only; nothing of the game is touched.
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui_internal.h"
#include "ui_theme.h"

namespace {
double now_secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::atomic<double> g_start{-100.0};
std::mutex g_mu;
std::string g_species;
constexpr double kLen = 2.6;

// Four rising notes with a soft decay (C6 E6 G6 C7), 22.05 kHz mono 16-bit, built once.
const std::vector<uint8_t>& chime() {
  static const std::vector<uint8_t> wav = [] {
    const int sr = 22050, n = sr * 7 / 10;
    std::vector<int16_t> pcm(n, 0);
    const double notes[4] = {1046.5, 1318.5, 1568.0, 2093.0};
    for (int k = 0; k < 4; k++) {
      const int s0 = k * sr / 11;
      for (int i = s0; i < n; i++) {
        const double t = double(i - s0) / sr, env = std::exp(-t * 7.0) * std::min(1.0, t * 400.0);
        const double v = std::sin(2 * 3.14159265358979 * notes[k] * t) + 0.35 * std::sin(2 * 3.14159265358979 * notes[k] * 2 * t);
        pcm[i] = (int16_t)std::clamp<double>(pcm[i] + v * env * 3200.0, -30000.0, 30000.0);
      }
    }
    std::vector<uint8_t> w(44 + n * 2);
    auto put32 = [&](size_t o, uint32_t v) { memcpy(&w[o], &v, 4); };
    auto put16 = [&](size_t o, uint16_t v) { memcpy(&w[o], &v, 2); };
    memcpy(&w[0], "RIFF", 4); put32(4, 36 + n * 2); memcpy(&w[8], "WAVEfmt ", 8); put32(16, 16); put16(20, 1); put16(22, 1);
    put32(24, sr); put32(28, sr * 2); put16(32, 2); put16(34, 16); memcpy(&w[36], "data", 4); put32(40, n * 2);
    memcpy(&w[44], pcm.data(), (size_t)n * 2);
    return w;
  }();
  return wav;
}

void star(ImDrawList* dl, ImVec2 c, float r, float inner, ImU32 col, int pts) {   // pts-pointed star
  ImVec2 v[16];
  for (int i = 0; i < pts * 2; i++) {
    const float a = -1.5707963f + i * 3.14159265f / pts, rr = (i & 1) ? r * inner : r;
    v[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
  }
  dl->AddConvexPolyFilled(v, pts * 2, col);
}
}  // namespace

extern "C" void snap_shiny_fx_trigger(const char* species) {
  { std::lock_guard<std::mutex> lk(g_mu); g_species = species ? species : ""; }
  g_start.store(now_secs());
  const auto& w = chime();
  PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

namespace snap::ui {
bool shiny_fx_active() { return now_secs() - g_start.load() < kLen; }

void draw_shiny_fx(ImVec2 size) {
  const double t = now_secs() - g_start.load();
  if (t < 0 || t >= kLen) return;
  std::string sp; { std::lock_guard<std::mutex> lk(g_mu); sp = g_species; }
  const Theme& th = theme();
  const float scale = ImGui::GetFontSize() / th.font_px;
  const float fade = (float)std::clamp(std::min(t / 0.25, (kLen - t) / 0.6), 0.0, 1.0);
  const float slide = (1.0f - std::min(1.0f, (float)t / 0.3f)) * -24.0f * scale;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  char text[96]; snprintf(text, sizeof text, "Shiny %s!", sp.empty() ? "Pokemon" : sp.c_str());
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const float padx = 26.0f * scale, h = ts.y + 18.0f * scale, w = ts.x + padx * 2 + 34.0f * scale;
  const ImVec2 p0(size.x * 0.5f - w * 0.5f, size.y * 0.13f + slide), p1(p0.x + w, p0.y + h);
  auto a = [&](float al) { return (ImU32)(std::clamp(al * fade, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT; };
  dl->AddRectFilled(p0, p1, IM_COL32(255, 241, 230, 0) | a(0.97f), h * 0.5f);
  dl->AddRect(p0, p1, IM_COL32(248, 213, 71, 0) | a(1.0f), h * 0.5f, 0, 3.0f * scale);
  star(dl, ImVec2(p0.x + padx + 6.0f * scale, p0.y + h * 0.5f), 13.0f * scale, 0.45f, IM_COL32(248, 213, 71, 0) | a(1.0f), 5);
  dl->AddText(ImVec2(p0.x + padx + 30.0f * scale, p0.y + (h - ts.y) * 0.5f), IM_COL32(90, 83, 80, 0) | a(1.0f), text);
  // sparkles: four-point twinkles drifting up around the badge
  for (int i = 0; i < 16; i++) {
    const float ph = std::fmod((float)t * 0.9f + i * 0.137f, 1.0f), seed = i * 12.9898f;
    const float fx = std::fmod(std::sin(seed) * 43758.5453f, 1.0f); const float ux = fx < 0 ? fx + 1 : fx;
    const float fy = std::fmod(std::sin(seed * 1.7f) * 24634.6345f, 1.0f); const float uy = fy < 0 ? fy + 1 : fy;
    const ImVec2 c(p0.x - 40.0f * scale + ux * (w + 80.0f * scale), p0.y - 10.0f * scale + uy * (h + 40.0f * scale) - ph * 30.0f * scale);
    const float tw = std::sin(ph * 3.14159265f), r = (7.0f + 9.0f * ux) * scale * tw;
    if (r < 0.5f) continue;
    star(dl, c, r, 0.18f, IM_COL32(255, 232, 120, 0) | a(0.95f * tw), 4);
  }
}
}  // namespace snap::ui
