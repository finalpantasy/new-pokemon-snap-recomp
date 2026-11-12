// SPDX-License-Identifier: MIT
// Shader precompile UI over the C API in runtime/nvn/shaderprecompile.h (analysis/perf/shader_precompile_api.md):
//   - first-launch prompt (mode UNSET): compile now vs compile while playing
//   - progress screen while the game is held (mode NOW and a run in progress): percentage, ETA, Cancel / Continue in background
//   - "Graphics" tab of the F1 menu: mode, status, "Compile all known shaders now", "Rebuild shader cache"
// All three work with mouse, keyboard and controller (ImGui navigation).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "hle/hle_config.h"
#include "hle/profiles.h"
#include "nvn/shaderprecompile.h"
#include "ui_internal.h"

namespace snap::ui {

namespace {

struct ShaderUi {
  bool prompt_done = false;     // the user chose (or the prompt is off for this run)
  double first_seen = -1;
  bool was_screen = false;
  bool focus_next = true;
  std::string msg;
  bool msg_err = false;
  double msg_until = 0;
  bool confirm_rebuild = false;
  bool demo = false;            // SNAP_SHADER_UI_DEMO=1: a simulated progress screen for screenshots and controller tests (no compiling)
  double demo_start = -1;
  double demo_t = 0;            // seconds since the demo screen started (same clock as `now` in shader_screen_active)
};
ShaderUi Q;

bool env_set(const char* n) { const char* e = std::getenv(n); return e && *e; }

// Test runs must never get a modal prompt: they use SNAP_SAVE_DIR / SNAP_UI_TEST / SNAP_SHADER_PRECOMPILE.
bool prompt_enabled() {
  if (const char* e = std::getenv("SNAP_SHADER_PROMPT")) if (*e) return *e != '0';
  if (env_set("SNAP_SAVE_DIR") || env_set("SNAP_UI_TEST") || env_set("SNAP_SHADER_PRECOMPILE") || hle::config().headless) return false;
  return true;
}

std::string cache_dir() {
  if (const char* e = std::getenv("SNAP_SHADERCACHE_DIR")) if (*e) return e;
  if (const char* e = std::getenv("SNAP_CONFIG_DIR")) if (*e) return std::string(e) + "\\shadercache";
  return hle::default_data_root() + "\\shadercache";
}

std::string minutes_text(double sec) {
  if (sec < 0) return "estimating";
  if (sec < 45) return "under a minute";
  int m = (int)std::ceil(sec / 60.0);
  return "about " + std::to_string(m) + (m == 1 ? " minute" : " minutes");
}
std::string eta_text(double sec) {
  if (sec < 0) return "estimating the time left...";
  int s = (int)std::ceil(sec);
  char b[64];
  if (s >= 60) std::snprintf(b, sizeof b, "about %d min %02d s left", s / 60, s % 60);
  else std::snprintf(b, sizeof b, "about %d s left", std::max(s, 1));
  return b;
}

void centered_window(const ImVec2& size, float w_frac, float max_w, const char* id, ImVec2* out_pos, float* out_w, float* out_h, float h_lines_hint) {
  const float w = std::min(size.x * w_frac, max_w * size.y / 1080.0f);
  const float h = ImGui::GetTextLineHeightWithSpacing() * h_lines_hint;
  *out_pos = ImVec2((size.x - w) * 0.5f, (size.y - h) * 0.5f);
  *out_w = w;
  *out_h = h;
  (void)id;
}

void say(const std::string& m, bool err) { Q.msg = m; Q.msg_err = err; Q.msg_until = ImGui::GetTime() + 8; }

}  // namespace

bool shader_screen_active(double now) {
  if (Q.first_seen < 0) { Q.first_seen = now; Q.demo = env_set("SNAP_SHADER_UI_DEMO"); }
  if (Q.demo) {
    if (Q.demo_start < 0) Q.demo_start = now;
    Q.demo_t = now - Q.demo_start;
    return Q.demo_t > 1.0 && Q.demo_t < 60.0;
  }
  if (shaderprecompile_is_holding_game()) return true;   // progress screen
  if (!Q.prompt_done) {
    if (!prompt_enabled() || shaderprecompile_get_mode() != SHADERPRECOMPILE_MODE_UNSET) { Q.prompt_done = true; return false; }
    return now - Q.first_seen > 1.5;   // a short beat so the first frame of the game is not covered at once
  }
  return false;
}

void draw_shader_screen(ImVec2 size) {
  const Theme& t = theme();
  const bool holding = Q.demo || shaderprecompile_is_holding_game() != 0;
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(0, 0), size, holding ? ImGui::GetColorU32(ImVec4(0.957f, 0.949f, 0.941f, 0.97f)) : ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.42f)));

  ImVec2 pos;
  float w, h;
  centered_window(size, 0.62f, 980.0f, "shader", &pos, &w, &h, holding ? 15.0f : 19.0f);
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  if (!holding) style_shadow(bg, pos, ImVec2(w, h), t.window_round * ImGui::GetFontSize() / t.font_px);
  ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, t.panel);
  ImGui::PushStyleColor(ImGuiCol_Border, t.accent_hover);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(t.window_pad * 1.6f * ImGui::GetFontSize() / t.font_px, t.window_pad * 1.4f * ImGui::GetFontSize() / t.font_px));
  if (ImGui::Begin("##shaderscreen", nullptr, wf)) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    const float bh = ImGui::GetTextLineHeight() * 2.3f;
    if (holding) {
      uint32_t done = 0, total = 0;
      double eta = -1;
      shaderprecompile_progress(&done, &total, &eta);
      if (Q.demo) {   // fake numbers: 800 pipelines at 25 per second
        total = 800;
        done = std::min<uint32_t>(total, (uint32_t)((Q.demo_t - 1.0) * 25.0));
        eta = (total - done) / 25.0;
      }
      const float frac = total ? std::min(1.0f, (float)done / (float)total) : 0.0f;
      ImGui::TextColored(t.title, "Compiling shaders");
      ImGui::Spacing();
      ImGui::TextUnformatted("The game starts when this finishes. This makes play smoother, with fewer pauses when new effects appear.");
      ImGui::Spacing();
      char ov[96];
      std::snprintf(ov, sizeof ov, "%d%%   (%u of %u)", (int)(frac * 100.0f + 0.5f), done, total);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, t.accent);
      ImGui::ProgressBar(frac, ImVec2(-1, bh * 0.8f), ov);
      ImGui::PopStyleColor();
      ImGui::TextColored(t.text_dim, "%s", total ? eta_text(eta).c_str() : "starting...");
      ImGui::Spacing();
      ImGui::Spacing();
      if (Q.focus_next) { ImGui::SetKeyboardFocusHere(); Q.focus_next = false; }
      if (ImGui::Button("Continue in background", ImVec2(w * 0.44f, bh))) {
        if (Q.demo) Q.demo_start = -1e9; else shaderprecompile_set_mode(SHADERPRECOMPILE_MODE_PLAYING);   // releases the game; the compile goes on at low priority
        Q.prompt_done = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(w * 0.28f, bh))) {
        if (Q.demo) Q.demo_start = -1e9; else shaderprecompile_cancel();
        Q.prompt_done = true;
      }
      ImGui::TextColored(t.text_dim, "Continue in background starts the game now and keeps compiling while you play (this also sets the mode to \"Compile while playing\"). Cancel stops compiling; missing shaders are then built when the game first needs them.");
    } else {
      const double est = shaderprecompile_estimate_seconds();
      const bool can_now = est > 0.0;
      ImGui::TextColored(t.title, "Shader compilation");
      ImGui::Spacing();
      ImGui::TextUnformatted("The game's graphics shaders are built for your graphics card the first time they are needed. Doing that in advance avoids pauses and stutter during play. Choose when to do it:");
      ImGui::Spacing();
      if (can_now) {
        ImGui::TextColored(t.text_dim, "Compile now covers the shaders this game has already used on this PC, about %s. Shaders it has not used yet are still built while you play.", minutes_text(est).c_str());
      } else {
        ImGui::TextColored(t.conflict, "Compile now is not available yet. This is the first run on this PC, so no shaders have been recorded and there is no list to compile in advance. The game records them as you play, and from the next launch you can compile them all at startup (F1 > Graphics).");
      }
      ImGui::Spacing();
      ImGui::Spacing();
      std::string l1 = can_now ? "Compile shaders now (" + minutes_text(est) + ", smoother play)" : "Compile shaders now (nothing recorded yet)";
      if (!can_now) ImGui::BeginDisabled();
      if (can_now && Q.focus_next) { ImGui::SetKeyboardFocusHere(); Q.focus_next = false; }
      if (ImGui::Button(l1.c_str(), ImVec2(-1, bh))) {
        shaderprecompile_set_mode(SHADERPRECOMPILE_MODE_NOW);
        Q.prompt_done = true;
      }
      if (!can_now) ImGui::EndDisabled();
      if (!can_now && Q.focus_next) { ImGui::SetKeyboardFocusHere(); Q.focus_next = false; }
      if (ImGui::Button("Compile while playing (start now, possible brief stutter)", ImVec2(-1, bh))) {
        shaderprecompile_set_mode(SHADERPRECOMPILE_MODE_PLAYING);
        Q.prompt_done = true;
      }
      ImGui::Spacing();
      ImGui::TextColored(t.text_dim, "You can change this later in the F1 menu, Graphics tab.");
      ImGui::Spacing();
      style_footer_prompts(false, false);
    }
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  Q.was_screen = true;
}

void shader_screen_closed() { Q.focus_next = true; Q.was_screen = false; }

// ---- Graphics tab -------------------------------------------------------------------------------------------------------
void tab_graphics() {
  const Theme& t = theme();
  ImGui::TextColored(t.title, "Shader cache");
  ImGui::TextWrapped("Pipelines are compiled from shaders as the game needs them and saved in %s. Compiling ahead of time avoids stutter.", cache_dir().c_str());
  ImGui::Spacing();

  int mode = shaderprecompile_get_mode();
  ImGui::Text("When to compile:");
  bool now_mode = mode == SHADERPRECOMPILE_MODE_NOW;
  bool play_mode = mode == SHADERPRECOMPILE_MODE_PLAYING;
  if (ImGui::RadioButton("Compile at startup (the game waits, smoothest play)", now_mode)) shaderprecompile_set_mode(SHADERPRECOMPILE_MODE_NOW);
  if (ImGui::RadioButton("Compile while playing (starts at once, possible brief stutter)", play_mode)) shaderprecompile_set_mode(SHADERPRECOMPILE_MODE_PLAYING);
  if (mode == SHADERPRECOMPILE_MODE_UNSET) ImGui::TextColored(t.text_dim, "Not chosen yet; the game behaves like \"while playing\".");
  ImGui::TextColored(t.text_dim, "Takes effect at the next start for \"at startup\"; \"while playing\" applies now.");
  ImGui::Spacing();

  uint32_t done = 0, total = 0;
  double eta = -1;
  shaderprecompile_progress(&done, &total, &eta);
  const int st = shaderprecompile_state();
  const char* stn = st == SHADERPRECOMPILE_RUNNING ? "compiling" : st == SHADERPRECOMPILE_DONE ? "finished" : st == SHADERPRECOMPILE_CANCELLED ? "cancelled" : "idle";
  ImGui::Text("Status: %s", stn);
  if (st == SHADERPRECOMPILE_RUNNING && total) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, t.accent);
    ImGui::ProgressBar((float)done / (float)total, ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(eta_text(eta).c_str());
  } else if (total) {
    ImGui::TextColored(t.text_dim, "Last run: %u of %u pipelines.", done, total);
  }
  const uint32_t pending = shaderprecompile_background_pending();
  if (pending) ImGui::TextColored(t.conflict, "%u shaders the game asked for are still compiling in the background.", pending);
  const double est = shaderprecompile_estimate_seconds();
  if (est > 0) ImGui::TextColored(t.text_dim, "A full compile takes %s on this PC.", minutes_text(est).c_str());
  ImGui::Spacing();

  if (ImGui::Button("Compile all known shaders now")) {
    int n = shaderprecompile_begin();
    say(n > 0 ? "Queued " + std::to_string(n) + " pipelines." : "Nothing to compile: everything known is already built.", false);
  }
  ImGui::SameLine();
  ImGui::TextColored(t.text_dim, mode == SHADERPRECOMPILE_MODE_NOW ? "The game pauses on a progress screen while it runs." : "Runs in the background while you play.");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(t.title, "Rebuild shader cache");
  ImGui::TextWrapped("Throws away the saved graphics-driver cache and compiles every recorded shader again, for example after a driver update or when you see graphical glitches. The game restarts and shows the progress screen; unsaved progress is lost.");
  if (!Q.confirm_rebuild) {
    if (ImGui::Button("Rebuild shader cache...")) Q.confirm_rebuild = true;
  } else {
    if (hold_confirm("Hold to rebuild and restart", 1.5f, t.error)) {
      std::error_code ec;
      std::filesystem::remove(std::filesystem::u8path(cache_dir()) / "pipeline_cache.bin", ec);
      shaderprecompile_set_mode(SHADERPRECOMPILE_MODE_NOW);
      Q.confirm_rebuild = false;
      restart_game();
      say("Could not restart the game; the cache was cleared and the next start will rebuild it.", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) Q.confirm_rebuild = false;
  }
  if (ImGui::GetTime() < Q.msg_until) { ImGui::Spacing(); ImGui::TextColored(Q.msg_err ? t.error : t.good, "%s", Q.msg.c_str()); }
  extras_graphics();
}

}  // namespace snap::ui
