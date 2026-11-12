// SPDX-License-Identifier: MIT
// Colors and sizes of the in-game overlay in one place. A game-themed skin (NEO-ONE lab look, see AGENTS.md "Settings and
// accessibility UX") replaces the values below later; nothing else in runtime/ui hard-codes a color or a size.
#pragma once
#include "imgui.h"

namespace snap::ui {

struct Theme {
  // colors (sRGB, as they should look on screen)
  ImVec4 backdrop        {0.00f, 0.00f, 0.00f, 0.55f};   // dims the game behind the menu
  ImVec4 panel           {0.09f, 0.11f, 0.14f, 0.97f};
  ImVec4 panel_alt       {0.12f, 0.15f, 0.19f, 1.00f};   // table stripes, child panels
  ImVec4 border          {0.28f, 0.34f, 0.42f, 1.00f};
  ImVec4 text            {0.93f, 0.95f, 0.97f, 1.00f};
  ImVec4 text_dim        {0.62f, 0.68f, 0.74f, 1.00f};
  ImVec4 accent         {0.20f, 0.62f, 0.86f, 1.00f};    // selected tab, slider grab, focus
  ImVec4 accent_hover    {0.28f, 0.72f, 0.95f, 1.00f};
  ImVec4 accent_active   {0.14f, 0.50f, 0.74f, 1.00f};
  ImVec4 slot            {0.17f, 0.21f, 0.27f, 1.00f};   // an empty or bound binding slot
  ImVec4 slot_hover      {0.24f, 0.30f, 0.38f, 1.00f};
  ImVec4 slot_listening  {0.86f, 0.58f, 0.12f, 1.00f};   // waiting for a key press
  ImVec4 conflict        {0.78f, 0.52f, 0.10f, 1.00f};   // a source bound to several actions
  ImVec4 good            {0.30f, 0.78f, 0.45f, 1.00f};
  ImVec4 error           {0.92f, 0.34f, 0.34f, 1.00f};
  ImVec4 toast_bg        {0.14f, 0.06f, 0.06f, 0.95f};

  // sizes (pixels at UI scale 1 and a 1080p output; the overlay scales them with the output height and the user's UI scale)
  float font_px        = 18.0f;
  float rounding       = 6.0f;
  float window_pad     = 16.0f;
  float item_spacing   = 8.0f;
  float frame_pad_x    = 10.0f;
  float frame_pad_y    = 5.0f;
  float panel_width    = 0.94f;     // fraction of the output width (max)
  float panel_height   = 0.90f;     // fraction of the output height (max)
  float panel_max_w    = 1500.0f;   // at scale 1
  float label_w        = 170.0f;    // action column
  float slot_w         = 128.0f;    // one binding slot
  float stick_plot     = 110.0f;    // live stick plot in the test page
  float toast_w        = 560.0f;
  const char* font_file = "segoeui.ttf";   // looked up in %SystemRoot%\Fonts; the embedded font is the fallback
};

Theme& theme();

}  // namespace snap::ui
