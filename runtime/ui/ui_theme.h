// SPDX-License-Identifier: MIT
// Colors and sizes of the in-game overlay in one place. A game-themed skin (NEO-ONE lab look, see AGENTS.md "Settings and
// accessibility UX") replaces the values below later; nothing else in runtime/ui hard-codes a color or a size.
#pragma once
#include "imgui.h"

namespace snap::ui {

struct Theme {
  // colors (sRGB, as they should look on screen). Light "lab" palette measured from the game's own menus: analysis/ui/style_ref.md
  ImVec4 backdrop        {0.00f, 0.00f, 0.00f, 0.38f};   // dims the game behind the menu
  ImVec4 panel           {0.957f, 0.949f, 0.941f, 0.985f};   // #f4f2f0 menu card white
  ImVec4 panel_alt       {0.918f, 0.902f, 0.886f, 1.00f};    // child panels, table stripes
  ImVec4 border          {0.847f, 0.831f, 0.812f, 1.00f};
  ImVec4 text            {0.353f, 0.325f, 0.314f, 1.00f};    // warm dark grey, like the game's labels
  ImVec4 text_dim        {0.545f, 0.541f, 0.545f, 1.00f};    // #8b8a8b header grey
  ImVec4 title           {0.855f, 0.455f, 0.227f, 1.00f};    // darker orange for headings on white
  ImVec4 accent          {0.973f, 0.835f, 0.278f, 1.00f};    // #f8d547 selected yellow: selected tab, slider grab, focus fill
  ImVec4 accent_hover    {0.973f, 0.671f, 0.475f, 1.00f};    // #f8ab79 orange outline, hover
  ImVec4 accent_active   {0.949f, 0.757f, 0.176f, 1.00f};
  ImVec4 on_accent       {0.31f, 0.24f, 0.16f, 1.00f};       // text on yellow or orange
  ImVec4 slot            {0.894f, 0.875f, 0.855f, 1.00f};    // an empty or bound binding slot, buttons
  ImVec4 slot_hover      {0.973f, 0.918f, 0.667f, 1.00f};
  ImVec4 slot_listening  {0.973f, 0.671f, 0.475f, 1.00f};    // waiting for a key press
  ImVec4 conflict        {0.90f, 0.62f, 0.20f, 1.00f};       // a source bound to several actions
  ImVec4 good            {0.25f, 0.58f, 0.37f, 1.00f};
  ImVec4 error           {0.78f, 0.28f, 0.24f, 1.00f};
  ImVec4 toast_bg        {1.00f, 0.945f, 0.90f, 0.97f};
  ImVec4 teal            {0.588f, 0.812f, 0.792f, 1.00f};    // #96cfca banner accent

  // sizes (pixels at UI scale 1 and a 1080p output; the overlay scales them with the output height and the user's UI scale)
  float font_px        = 19.0f;
  float rounding       = 12.0f;      // buttons, slots, tabs (cards use window_round)
  float window_round   = 22.0f;
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
  const char* font_file = "segoeui.ttf";   // fallback when the game font cannot be decoded (ui_font.h): %SystemRoot%\Fonts, then the embedded font
};

Theme& theme();

}  // namespace snap::ui
