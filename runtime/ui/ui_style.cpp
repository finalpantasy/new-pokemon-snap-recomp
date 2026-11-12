// SPDX-License-Identifier: MIT
// Drawing helpers for the game-style look (palette in ui_theme.h, measurements in analysis/ui/style_ref.md): a soft card shadow and the
// controller-prompt glyphs (orange chevron with a white disc for face buttons, grey pill for shoulder buttons).
#include <cstring>

#include "ui_internal.h"

namespace snap::ui {

static ImU32 col32(const ImVec4& c) { return ImGui::GetColorU32(c); }

void style_shadow(ImDrawList* dl, ImVec2 pos, ImVec2 size, float round) {
  const float s = ImGui::GetFontSize() / theme().font_px;
  for (int i = 0; i < 7; ++i) {   // stacked translucent rounded rects: cheap soft shadow, offset a little downward like the game's cards
    const float g = (3.0f + i * 3.5f) * s;
    dl->AddRectFilled(ImVec2(pos.x - g, pos.y - g + 8.0f * s), ImVec2(pos.x + size.x + g, pos.y + size.y + g + 8.0f * s), IM_COL32(60, 45, 30, 9), round + g);
  }
}

float style_prompt(const char* glyph, const char* label) {
  const Theme& t = theme();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float h = fs * 1.35f;
  const bool face = std::strlen(glyph) == 1;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float y0 = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
  float w;
  if (face) {
    w = h * 1.45f;
    const float nose = h * 0.42f;   // chevron pointing right: the game's A and B prompt shape
    ImVec2 pts[5] = {{p.x, y0}, {p.x + w - nose, y0}, {p.x + w, y0 + h * 0.5f}, {p.x + w - nose, y0 + h}, {p.x, y0 + h}};
    dl->AddConvexPolyFilled(pts, 5, col32(t.accent_hover));
    dl->AddPolyline(pts, 5, IM_COL32(150, 100, 70, 255), ImDrawFlags_Closed, 1.5f);
    const ImVec2 c(p.x + (w - nose) * 0.5f + h * 0.12f, y0 + h * 0.5f);
    dl->AddCircleFilled(c, h * 0.34f, IM_COL32(255, 250, 245, 255), 20);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col32(t.title), glyph);
  } else {
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    w = ts.x + h * 0.7f;
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), col32(t.text_dim), h * 0.5f);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, y0 + (h - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), glyph);
  }
  ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
  ImGui::SameLine(0, fs * 0.35f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(t.text_dim, "%s", label);
  ImGui::SameLine(0, fs * 1.2f);
  return w;
}

void style_footer_prompts(bool tabs, bool back) {
  style_prompt("A", "Select");
  if (back) style_prompt("B", "Back");
  if (tabs) { style_prompt("LB", ""); style_prompt("RB", "Switch page"); }
  ImGui::NewLine();
}

}  // namespace snap::ui
