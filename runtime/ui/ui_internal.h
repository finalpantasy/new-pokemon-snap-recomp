// SPDX-License-Identifier: MIT
// Shared declarations inside runtime/ui (overlay core, controls menu, scripted test).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "hle/input_core.h"
#include "imgui.h"
#include "ui_theme.h"

namespace snap::ui {

// ---- controls menu (ui_controls.cpp)
// Draws the whole menu as a full-frame ImGui layer; `size` is the output size in pixels. Called between NewFrame and Render.
void draw_menu(ImVec2 size, bool* want_close);
// Called every frame while the menu is open or about to open: re-reads the mapping the hub holds.
void menu_opened();
void menu_closed();                 // saves pending edits
void menu_tick_save();              // debounced save of live edits
// Small notice at the bottom of the screen (invalid input file, migration, save errors); true while it is on screen.
bool draw_toast(ImVec2 size, double now);
// Shiny capture effect (runtime/ui/ui_shiny.cpp): badge, sparkles; active for ~2.6 s after snap_shiny_fx_trigger.
bool shiny_fx_active();
void draw_shiny_fx(ImVec2 size);
void menu_select_tab(const char* name);   // scripted test: Bindings, Mouse, Controller, Accessibility, Test
// Rebinding as the UI performs it (a click on a slot); used by the scripted test.
bool menu_begin_rebind(int action, hle::input::Device dev, int slot);
bool menu_rebinding();

// ---- saves tab (ui_saves.cpp): save profiles
void tab_saves();
void saves_tab_opened();
bool saves_test_import(const std::string& fixture, std::string* name_out, std::string* err);   // scripted test
void saves_test_switch(const std::string& name);

// ---- game-style drawing helpers (ui_style.cpp)
void style_shadow(ImDrawList* dl, ImVec2 pos, ImVec2 size, float round);
float style_prompt(const char* glyph, const char* label);   // "A"/"B" make the orange chevron, longer text ("LB") a grey pill; stays on the line
void style_footer_prompts(bool tabs, bool back);

// ---- audio tab (ui_audio.cpp)
void tab_audio();

// ---- shader precompile UI (ui_shaders.cpp)
bool shader_screen_active(double now);   // first-launch prompt or the progress screen is wanted this frame
void draw_shader_screen(ImVec2 size);
void shader_screen_closed();
void shader_cache_section();   // the shader cache controls, shown inside the Graphics page
// shared helpers (ui_saves.cpp)
bool hold_confirm(const char* label, float secs, const ImVec4& color);   // true once when held long enough (mouse, Enter or gamepad)
void restart_game();                                                      // relaunch with the same arguments and the active profile

// ---- extras (ui_extras.cpp): gyro and rumble settings and the photo gallery
void extras_controller();   // appended to the Controller tab
void gallery_section();     // inside the Photos page

// ---- option pages (ui_options.cpp): table-driven, values in hle/settings.h
void tab_graphics();
void tab_display();
void tab_photos();
void tab_mods();
void audio_output_section();   // appended to the Audio tab
void options_menu_opened();    // remembers the start-up values (what "needs a restart" is measured against)
int options_pending_restart(std::string* names);
void options_restart_bar();    // the "N changes need a restart / Apply and restart" strip
// scripted test (ui_test.cpp)
bool options_test_set(const std::string& key, const std::string& value);
std::string options_test_get(const std::string& key);
void options_test_scroll(float fraction);   // scroll the visible option list (0 top, 1 bottom)
bool options_test_option_when(const std::string& key, std::string* when);
bool options_test_mods(const std::string& op, const std::string& id, int arg, std::string* state);

// ---- overlay core (ui_overlay.cpp)
bool overlay_open();
void overlay_set_open(bool open);
void overlay_set_modal(bool on);   // the shader prompt / progress screen: input goes to ImGui and the game reads idle
void* overlay_hwnd();

// ---- scripted test (ui_test.cpp), enabled by SNAP_UI_TEST=<file>
void test_tick(double now);
bool test_active();

}  // namespace snap::ui
