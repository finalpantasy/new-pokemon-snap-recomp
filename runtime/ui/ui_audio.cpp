// SPDX-License-Identifier: MIT
// "Audio" tab of the F1 menu: Master (continuous) and Music / Sound effects / Voice (the game's own 0-10 levels, 10% steps).
// Volumes live in the audio layer (docs/audio.md, "Volume API for UI"); it applies them live and persists audio.json itself.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "audio/audio_out.h"
#include "ui_internal.h"

namespace snap::ui {

namespace {

struct Row {
  const char* label;
  const char* hint;
  int cat;
  bool stepped;
};
const Row kRows[] = {
    {"Master", "Overall loudness, any value from 0 to 100%.", SNAP_VOL_MASTER, false},
    {"Music", "Background music, in the game's own 10% steps.", SNAP_VOL_BGM, true},
    {"Sound effects", "Footsteps, camera, Pokemon sounds other than voices, in 10% steps.", SNAP_VOL_SE, true},
    {"Voice", "Spoken lines, in 10% steps.", SNAP_VOL_VOICE, true},
};

double now_secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double g_last_set[SNAP_VOL_COUNT] = {0, 0, 0, 0};
float g_drag[SNAP_VOL_COUNT] = {-1, -1, -1, -1};   // value shown while the slider is held

}  // namespace

void tab_audio() {
  const Theme& t = theme();
  ImGui::TextWrapped("Volume changes apply at once and are saved in audio.json next to your controls file.");
  ImGui::Spacing();
  for (const Row& r : kRows) {
    ImGui::PushID(r.cat);
    float cur = std::clamp(snap_audio_get_volume(r.cat), 0.0f, 1.0f);
    float shown = g_drag[r.cat] >= 0 ? g_drag[r.cat] : cur;
    ImGui::TextUnformatted(r.label);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, ImGui::GetFontSize() * 30.0f));
    bool changed = false;
    float next = shown;
    if (r.stepped) {
      int lv = (int)std::lround(shown * 10.0f);
      char fmt[24];
      std::snprintf(fmt, sizeof fmt, "%d%%", lv * 10);
      if (ImGui::SliderInt("##v", &lv, 0, 10, "%d / 10")) { changed = true; next = lv / 10.0f; }
    } else {
      int pct = (int)std::lround(shown * 100.0f);
      if (ImGui::SliderInt("##v", &pct, 0, 100, "%d%%")) { changed = true; next = pct / 100.0f; }
    }
    const bool active = ImGui::IsItemActive();
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    if (changed) {
      g_drag[r.cat] = next;
      const double n = now_secs();
      if (n - g_last_set[r.cat] > 0.2) { snap_audio_set_volume(r.cat, next); g_last_set[r.cat] = n; }   // persisting writes a file: a few times per second at most
    }
    if (released) { snap_audio_set_volume(r.cat, g_drag[r.cat] >= 0 ? g_drag[r.cat] : next); g_drag[r.cat] = -1; }
    else if (!active && !changed) g_drag[r.cat] = -1;
    if (r.stepped) { ImGui::SameLine(); ImGui::TextColored(t.text_dim, "%d%%", (int)std::lround(shown * 100.0f)); }
    ImGui::TextColored(t.text_dim, "%s", r.hint);
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (ImGui::Button("Reset to original mix")) {
    for (const Row& r : kRows) { snap_audio_set_volume(r.cat, 1.0f); g_drag[r.cat] = -1; }
  }
  ImGui::SameLine();
  ImGui::TextColored(t.text_dim, "All four back to 100%, the way the game ships.");
  audio_output_section();
}

}  // namespace snap::ui
