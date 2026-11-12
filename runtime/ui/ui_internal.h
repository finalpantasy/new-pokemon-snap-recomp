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
void menu_select_tab(const char* name);   // scripted test: Bindings, Mouse, Controller, Accessibility, Test
// Rebinding as the UI performs it (a click on a slot); used by the scripted test.
bool menu_begin_rebind(int action, hle::input::Device dev, int slot);
bool menu_rebinding();

// ---- overlay core (ui_overlay.cpp)
bool overlay_open();
void overlay_set_open(bool open);
void* overlay_hwnd();

// ---- scripted test (ui_test.cpp), enabled by SNAP_UI_TEST=<file>
void test_tick(double now);
bool test_active();

}  // namespace snap::ui
