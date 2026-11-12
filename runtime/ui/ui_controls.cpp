// SPDX-License-Identifier: MIT
// The controls menu: bindings per device (keyboard, mouse, controller), mouse and controller options, accessibility, live npad readout.
// Edits go to the input hub at once (live) and are saved to input.json a moment later (atomic write). Colors and sizes come from
// ui_theme.h only.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "imgui.h"
#include "ui_internal.h"
#include "ui_theme.h"

namespace snap::ui {

using namespace hle::input;

namespace {

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Rebind {
  bool active = false;
  int action = 0;
  Device dev = kDevKeyboard;
  int slot = 0;
};

struct MenuState {
  Mapping edit;
  bool dirty = false;
  double dirty_t = 0;
  Rebind rb;
  const char* want_tab = nullptr;
  int reset_dev = -1;            // device whose reset is awaiting confirmation
  std::string note;              // short feedback line ("Bound A to P", "Saved")
  double note_until = 0;
  uint64_t dismissed_serial = 0;
  float ui_scale_edit = 1.0f;
  double saved_flash_until = 0;
} S;

// scale a design size (pixels at font 18) with the current font size
float px(float v) { return v * ImGui::GetFontSize() / theme().font_px; }

void say(const std::string& s, double secs = 4.0) {
  S.note = s;
  S.note_until = now_s() + secs;
}

void apply() {
  hub().set_mapping(S.edit);
  S.dirty = true;
  S.dirty_t = now_s();
}

ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }

// ---- rebinding -------------------------------------------------------------------------------------------------------
void finish_rebind() {
  S.rb.active = false;
  hub().capture_cancel();
}

void begin_rebind(int action, Device dev, int slot) {
  S.rb = Rebind{true, action, dev, slot};
  hub().capture_begin(1u << (int)dev);
}

void resolve_rebind(bool* esc_used) {
  if (!S.rb.active) return;
  Source s;
  bool cancel = false, clear = false, got = false;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
  else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) clear = true;
  else if (hub().capture_result(&s)) {
    got = true;
    if (s.kind == Source::KeyboardKey && s.id == key_from_name("Escape")) { cancel = true; got = false; }
    else if (s.kind == Source::KeyboardKey && s.id == key_from_name("Delete")) { clear = true; got = false; }
  } else if (hub().capture_timed_out()) {
    finish_rebind();
    say("Rebinding timed out. Nothing changed.");
    return;
  }
  if (!cancel && !clear && !got) return;
  *esc_used = true;
  auto& v = action_sources(S.edit, S.rb.action);
  if (got) {
    slot_set(v, S.rb.dev, S.rb.slot, s);
    apply();
    say(std::string(action_label(S.rb.action)) + " (" + device_name(S.rb.dev) + ") = " + source_label(s));
  } else if (clear) {
    slot_clear(v, S.rb.dev, S.rb.slot);
    apply();
    say(std::string(action_label(S.rb.action)) + " (" + device_name(S.rb.dev) + ") cleared");
  } else {
    say("Rebinding cancelled.");
  }
  finish_rebind();
}

void slot_button(int action, Device dev, int slot) {
  const Theme& t = theme();
  auto& v = action_sources(S.edit, action);
  Source s = slot_get(v, dev, slot);
  const bool listening = S.rb.active && S.rb.action == action && S.rb.dev == dev && S.rb.slot == slot;
  std::vector<int> users;
  if (s.kind != Source::None) users = actions_using(S.edit, s);
  const bool conflict = users.size() > 1;
  std::string label = listening ? "Press a key..." : s.kind == Source::None ? "-" : source_label(s);
  if (dev == kDevMouse && listening) label = "Press a button...";
  if (dev == kDevController && listening) label = "Press a button...";
  if (conflict && !listening) label = "! " + label;   // a cue besides the color
  int pushed = 0;
  if (listening) { ImGui::PushStyleColor(ImGuiCol_Button, t.slot_listening); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1)); pushed = 2; }
  else if (conflict) { ImGui::PushStyleColor(ImGuiCol_Button, t.conflict); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1)); pushed = 2; }
  else if (s.kind == Source::None) { ImGui::PushStyleColor(ImGuiCol_Text, t.text_dim); pushed = 1; }
  ImGui::PushID(action * 16 + (int)dev * 4 + slot);
  if (ImGui::Button(label.c_str(), ImVec2(px(t.slot_w), 0))) {
    if (listening) { finish_rebind(); say("Rebinding cancelled."); }
    else begin_rebind(action, dev, slot);
  }
  const bool hovered = ImGui::IsItemHovered();
  if (s.kind != Source::None && !listening &&
      ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) || (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))))) {
    slot_clear(v, dev, slot);
    apply();
    say(std::string(action_label(action)) + " (" + device_name(dev) + ") cleared");
  }
  if (hovered && ImGui::BeginTooltip()) {
    if (conflict) {
      std::string others;
      for (int a : users) if (a != action) others += std::string(others.empty() ? "" : ", ") + action_label(a);
      ImGui::TextColored(t.conflict, "Also bound to: %s", others.c_str());
      ImGui::TextDisabled("Allowed: both actions fire together.");
    }
    ImGui::TextDisabled("Click, then press the key, button or axis.\nEsc cancels. Delete or right-click clears.");
    ImGui::EndTooltip();
  }
  ImGui::PopID();
  if (pushed) ImGui::PopStyleColor(pushed);
}

// ---- small widgets ---------------------------------------------------------------------------------------------------
void desc(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, theme().text_dim);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

bool checkbox(const char* label, bool* v, const char* d) {
  bool ch = ImGui::Checkbox(label, v);
  if (d) { ImGui::Indent(px(30)); desc(d); ImGui::Unindent(px(30)); }
  ImGui::Spacing();
  return ch;
}

bool slider(const char* label, const char* id, float* v, float lo, float hi, const char* fmt, const char* d) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(px(250));
  ImGui::SetNextItemWidth(px(340));
  bool ch = ImGui::SliderFloat(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
  if (d) { ImGui::Indent(px(30)); desc(d); ImGui::Unindent(px(30)); }
  ImGui::Spacing();
  return ch;
}

void stick_plot(const char* id, float x, float y, float dz, float outer) {
  const Theme& t = theme();
  float sz = px(t.stick_plot);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(sz, sz));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
  float r = sz * 0.5f - 2;
  dl->AddCircleFilled(c, r, col(t.slot), 48);
  if (dz > 0) dl->AddCircle(c, r * dz, col(t.border), 32);
  if (outer < 0.995f) dl->AddCircle(c, r * outer, col(t.border), 32);
  dl->AddCircle(c, r, col(t.border), 48);
  dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col(t.border));
  dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col(t.border));
  float dx = std::clamp(x, -1.0f, 1.0f) * r, dy = -std::clamp(y, -1.0f, 1.0f) * r;
  dl->AddCircleFilled(ImVec2(c.x + dx, c.y + dy), px(7), col(std::fabs(x) + std::fabs(y) > 0.01f ? t.accent_hover : t.text_dim), 16);
  (void)id;
}

void confirm_reset() {
  if (S.reset_dev >= 0) ImGui::OpenPopup("Reset to defaults?");
  ImVec2 c = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Reset to defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    Device d = (Device)std::clamp(S.reset_dev, 0, (int)kDevCount - 1);
    ImGui::Text("Reset all %s bindings and options to the defaults?", device_name(d));
    ImGui::TextDisabled("Other devices keep their settings.");
    ImGui::Spacing();
    if (ImGui::Button("Reset", ImVec2(px(140), 0))) {
      reset_device(S.edit, d);
      apply();
      say(std::string(device_name(d)) + " reset to defaults");
      S.reset_dev = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(140), 0))) { S.reset_dev = -1; ImGui::CloseCurrentPopup(); }
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  } else if (!ImGui::IsPopupOpen("Reset to defaults?")) {
    S.reset_dev = -1;
  }
}

void reset_button(Device d) {
  std::string l = std::string("Reset ") + device_name(d) + " to defaults";
  if (ImGui::Button(l.c_str())) S.reset_dev = (int)d;
}

// ---- tabs ------------------------------------------------------------------------------------------------------------
bool begin_tab(const char* name) {
  ImGuiTabItemFlags fl = 0;
  if (S.want_tab && std::string(S.want_tab) == name) { fl = ImGuiTabItemFlags_SetSelected; S.want_tab = nullptr; }
  return ImGui::BeginTabItem(name, nullptr, fl);
}

void tab_bindings(float footer_h) {
  const Theme& t = theme();
  desc("Click a slot, then press a key, mouse button, wheel notch, controller button or stick direction. Two slots per device for each Switch input. "
       "Esc cancels, Delete or right-click clears. A '!' marks a source used by more than one input (allowed).");
  ImGui::Spacing();
  reset_button(kDevKeyboard); ImGui::SameLine();
  reset_button(kDevMouse); ImGui::SameLine();
  reset_button(kDevController);
  ImGui::Spacing();
  const ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  float h = ImGui::GetContentRegionAvail().y - footer_h;
  if (ImGui::BeginTable("bindings", 7, tf, ImVec2(0, std::max(h, px(120))))) {
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Switch input", ImGuiTableColumnFlags_WidthFixed, px(t.label_w));
    const char* heads[6] = {"Keyboard 1", "Keyboard 2", "Mouse 1", "Mouse 2", "Controller 1", "Controller 2"};
    for (const char* hname : heads) ImGui::TableSetupColumn(hname, ImGuiTableColumnFlags_WidthFixed, px(t.slot_w) + px(8));
    ImGui::TableHeadersRow();
    struct Group { const char* title; int first, count; };
    static const int buttons[] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight, kStickL, kStickR};
    int conflicts = 0;
    for (int a = 0; a < kActionCount; ++a)
      for (int d = 0; d < kDevCount; ++d)
        for (int sl = 0; sl < kMaxSlotsPerDevice; ++sl) {
          Source s = slot_get(action_sources(S.edit, a), (Device)d, sl);
          if (s.kind != Source::None && actions_using(S.edit, s).size() > 1) ++conflicts;
        }
    auto section = [&](const char* title) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(t.accent_hover, "%s", title);
    };
    auto row = [&](int action) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(action_label(action));
      int ci = 1;
      for (int d = 0; d < kDevCount; ++d)
        for (int sl = 0; sl < kMaxSlotsPerDevice; ++sl) {
          ImGui::TableSetColumnIndex(ci++);
          slot_button(action, (Device)d, sl);
        }
    };
    section("Buttons");
    for (int b : buttons) row(b);
    section("Left stick (movement)");
    for (int a = 16; a < 20; ++a) row(a);
    section("Right stick (camera aim)");
    for (int a = 20; a < 24; ++a) row(a);
    ImGui::EndTable();
    if (conflicts) ImGui::TextColored(t.conflict, "! %d slot%s share a source with another input.", conflicts, conflicts == 1 ? "" : "s");
    else ImGui::TextDisabled("No conflicts.");
  }
}

void tab_mouse() {
  Mapping& m = S.edit;
  desc("Mouse buttons and the wheel are bound on the Bindings tab. The left button is also the touch screen.");
  ImGui::Spacing();
  reset_button(kDevMouse);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  bool ch = false;
  ch |= checkbox("Mouse look (mouse moves the camera)", &m.mouse_look,
                 "Mouse movement drives the right stick, the camera aim. Off by default. While on, the cursor is hidden and held at the window center, and the left button no longer acts as the touch screen.");
  ImGui::BeginDisabled(!m.mouse_look);
  ch |= slider("Sensitivity", "##sens", &m.mouse_sensitivity, 0.1f, 5.0f, "%.2f",
               "How far the stick moves for a given mouse speed. 1.00 reaches full deflection at about 1500 pixels per second.");
  ch |= checkbox("Invert horizontal", &m.mouse_invert_x, "Moving the mouse right turns the camera left.");
  ch |= checkbox("Invert vertical", &m.mouse_invert_y, "Moving the mouse up aims down, like a flight stick.");
  ch |= slider("Smoothing", "##smooth", &m.mouse_smoothing, 0.0f, 1.0f, m.mouse_smoothing <= 0.001f ? "Off" : "%.2f",
               "Averages the movement over a longer window. Off is the most direct; higher values trade a little lag for steadier aim.");
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::Spacing();
  ch |= checkbox("Left mouse button is the touch screen", &m.mouse_touch,
                 "Clicking in the window taps the Switch touch screen (used by some menus). Ignored while mouse look is on.");
  ch |= checkbox("Lock the cursor to the window while playing", &m.lock_cursor,
                 "Keeps the cursor inside the game window during play. It is released while this menu is open and whenever the window loses focus.");
  if (ch) apply();
}

void tab_controller() {
  Mapping& m = S.edit;
  desc("Controller buttons are bound on the Bindings tab. Any controller SDL3 or XInput knows works; all of them act as one Switch Pro Controller.");
  ImGui::Spacing();
  reset_button(kDevController);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  bool ch = false;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Face button layout");
  int layout = m.face_layout;
  ImGui::SameLine(px(250));
  if (ImGui::RadioButton("By position (Xbox B = Switch A)", layout == 0)) layout = 0;
  ImGui::SameLine();
  if (ImGui::RadioButton("Nintendo labels (Xbox A = Switch A)", layout == 1)) layout = 1;
  if (layout != m.face_layout) { set_face_layout(m, layout); ch = true; }
  ImGui::Indent(px(30));
  desc("By position puts Switch A on the right-hand face button like a Switch. Nintendo labels match letters instead. Switching swaps the south/east and west/north bindings in place.");
  ImGui::Unindent(px(30));
  ImGui::Spacing();
  ch |= checkbox("Swap sticks", &m.swap_sticks, "The physical left stick drives the Switch right stick (camera) and the physical right stick drives the left stick (movement).");
  ImGui::Separator();
  ImGui::Spacing();
  struct S2 { const char* name; StickMap* sm; const char* id; };
  S2 sticks[2] = {{"Left stick (movement)", &m.left, "L"}, {"Right stick (camera)", &m.right, "R"}};
  RawInput raw = hub().raw();
  for (int i = 0; i < 2; ++i) {
    StickMap& sm = *sticks[i].sm;
    ImGui::PushID(sticks[i].id);
    ImGui::TextColored(theme().accent_hover, "%s", sticks[i].name);
    ImGui::BeginGroup();
    ch |= slider("Dead zone (inner)", "##dz", &sm.deadzone, 0.0f, 0.6f, "%.2f", "Ignores small movements near the center, which stops drift.");
    ch |= slider("Outer dead zone", "##oz", &sm.outer_deadzone, 0.5f, 1.0f, "%.2f", "Reaches full deflection before the stick hits its edge. Lower it if your stick cannot reach 100%.");
    sm.outer_deadzone = std::max(sm.outer_deadzone, sm.deadzone + 0.05f);
    ch |= checkbox("Invert Y", &sm.invert_y, "Pushing the stick up acts as down.");
    ImGui::EndGroup();
    ImGui::SameLine();
    float rx = (sm.analog == "right") ? raw.rx : raw.lx, ry = (sm.analog == "right") ? raw.ry : raw.ly;
    if (m.swap_sticks && sm.analog != "none") { rx = sm.analog == "right" ? raw.lx : raw.rx; ry = sm.analog == "right" ? raw.ly : raw.ry; }
    if (sm.analog == "none") rx = ry = 0;
    ImGui::BeginGroup();
    stick_plot("plot", rx, ry, sm.deadzone, sm.outer_deadzone);
    ImGui::TextDisabled("live input");
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::PopID();
    ImGui::Separator();
    ImGui::Spacing();
  }
  ch |= slider("ZL trigger threshold", "##zl", &m.zl_threshold, 0.05f, 1.0f, "%.2f", "How far the left analog trigger must be pulled to count as pressed.");
  ch |= slider("ZR trigger threshold", "##zr", &m.zr_threshold, 0.05f, 1.0f, "%.2f", "The same for the right trigger.");
  ImGui::TextDisabled("Live triggers: ZL %.2f   ZR %.2f   (%s)", raw.lt, raw.rt, raw.pad_connected ? "controller connected" : "no controller detected");
  if (ch) apply();
}

void tab_access() {
  Mapping& m = S.edit;
  desc("Options that change how the controls and this menu behave. They apply at once.");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  bool ch = false;
  ch |= checkbox("ZL: toggle instead of hold", &m.toggle_zl, "One press holds ZL down until the next press, for players who cannot keep a trigger held. Applies to every ZL binding.");
  ch |= checkbox("ZR: toggle instead of hold", &m.toggle_zr, "The same for ZR, the camera shutter in the game.");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Menu size (UI scale)");
  ImGui::SameLine(px(250));
  ImGui::SetNextItemWidth(px(340));
  ImGui::SliderFloat("##uiscale", &S.ui_scale_edit, 0.75f, 2.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemDeactivatedAfterEdit()) { m.ui_scale = S.ui_scale_edit; ch = true; }
  ImGui::Indent(px(30));
  desc("Scales text and buttons in this menu. The change applies when you release the slider. The menu also grows with the window on larger screens.");
  ImGui::Unindent(px(30));
  if (ch) apply();
}

void tab_test() {
  const Theme& t = theme();
  desc("What the game would read from npad 0 right now, after your bindings. The game itself sees no input while this menu is open.");
  ImGui::Spacing();
  PadSample p = hub().preview();
  RawInput raw = hub().raw();
  ImGui::Text("Controller: %s    Keyboard focus: %s", raw.pad_connected ? "connected" : "none detected", raw.focused ? "yes" : "no");
  ImGui::Spacing();
  ImGui::TextColored(t.accent_hover, "Buttons");
  static const int order[] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kUp, kDown, kLeft, kRight, kStickL, kStickR};
  int n = 0;
  for (int b : order) {
    bool on = (p.buttons >> b) & 1;
    std::string label = std::string(on ? "(*) " : "( ) ") + button_name(b);
    ImVec2 sz(px(110), 0);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? t.accent : t.slot);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? t.accent : t.slot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? t.accent : t.slot);
    ImGui::PushID(b);
    ImGui::Button(label.c_str(), sz);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (++n % 8) ImGui::SameLine();
  }
  ImGui::Spacing();
  ImGui::TextColored(t.accent_hover, "Sticks");
  ImGui::BeginGroup();
  stick_plot("l", p.lx / 32767.0f, p.ly / 32767.0f, 0, 1);
  ImGui::Text("L  %6d %6d", p.lx, p.ly);
  ImGui::EndGroup();
  ImGui::SameLine(0, px(40));
  ImGui::BeginGroup();
  stick_plot("r", p.rx / 32767.0f, p.ry / 32767.0f, 0, 1);
  ImGui::Text("R  %6d %6d", p.rx, p.ry);
  ImGui::EndGroup();
  ImGui::SameLine(0, px(40));
  ImGui::BeginGroup();
  ImGui::Text("Raw triggers");
  ImGui::ProgressBar(raw.lt, ImVec2(px(180), 0), "LT");
  ImGui::ProgressBar(raw.rt, ImVec2(px(180), 0), "RT");
  ImGui::Text("Raw mouse speed: %.0f, %.0f px/s", raw.mouse_vx, raw.mouse_vy);
  ImGui::EndGroup();
  ImGui::TextDisabled("Raw buttons held: %d    keys held: %d    mouse buttons: %d", (int)raw.pad.count(), (int)raw.keys.count(), (int)__builtin_popcount(raw.mouse_buttons));
}

}  // namespace

// ---- public ----------------------------------------------------------------------------------------------------------
void menu_opened() {
  S.edit = hub().mapping();
  S.ui_scale_edit = S.edit.ui_scale;
  S.rb.active = false;
  S.reset_dev = -1;
  S.note.clear();
}

void menu_closed() {
  finish_rebind();
  if (S.dirty) { hub().save(); S.dirty = false; }
}

void menu_tick_save() {
  if (S.dirty && now_s() - S.dirty_t > 0.5) {
    S.dirty = false;
    std::string err;
    if (hub().save(&err)) S.saved_flash_until = now_s() + 2.0;
  }
}

void menu_select_tab(const char* name) { S.want_tab = name; }
bool menu_rebinding() { return S.rb.active; }
bool menu_begin_rebind(int action, Device dev, int slot) {
  if (action < 0 || action >= kActionCount) return false;
  begin_rebind(action, dev, slot);
  return true;
}

void draw_menu(ImVec2 size, bool* want_close) {
  const Theme& t = theme();
  bool esc_used = false;
  const bool was_rebinding = S.rb.active;
  resolve_rebind(&esc_used);
  if (!S.rb.active && !was_rebinding && !esc_used && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) *want_close = true;

  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), size, col(t.backdrop));
  const float scale = ImGui::GetFontSize() / t.font_px;
  const float w = std::min(size.x * t.panel_width, t.panel_max_w * scale);
  const float h = std::min(size.y * t.panel_height, 1100.0f * scale);
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (!ImGui::Begin("Controls##snapmenu", nullptr, wf)) { ImGui::End(); return; }

  // header
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(t.accent_hover, "Controls and input");
  ImGui::SameLine();
  ImGui::TextDisabled("   game input is paused while this menu is open");
  const char* closeLabel = "Close  (F1)";
  ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(closeLabel).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().WindowPadding.x);
  if (ImGui::Button(closeLabel)) *want_close = true;

  // notice from loading or saving the controls file
  const uint64_t serial = hub().message_serial();
  const std::string msg = hub().message();
  if (!msg.empty() && serial != S.dismissed_serial) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.toast_bg);
    float mh = ImGui::GetTextLineHeightWithSpacing() * 2.6f;
    if (ImGui::BeginChild("notice", ImVec2(0, mh), ImGuiChildFlags_Borders)) {
      ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - px(110));
      ImGui::TextColored(msg.find("invalid") != std::string::npos || msg.find("Could not") != std::string::npos ? t.error : t.good, "%s", msg.c_str());
      ImGui::PopTextWrapPos();
      ImGui::SameLine(ImGui::GetWindowWidth() - px(110));
      if (ImGui::Button("Dismiss")) S.dismissed_serial = serial;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
  if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_None)) {
    if (begin_tab("Bindings")) { tab_bindings(footer_h); ImGui::EndTabItem(); }
    if (begin_tab("Mouse")) {
      if (ImGui::BeginChild("p", ImVec2(0, -footer_h))) tab_mouse();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (begin_tab("Controller")) {
      if (ImGui::BeginChild("p", ImVec2(0, -footer_h))) tab_controller();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (begin_tab("Accessibility")) {
      if (ImGui::BeginChild("p", ImVec2(0, -footer_h))) tab_access();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (begin_tab("Test input")) {
      if (ImGui::BeginChild("p", ImVec2(0, -footer_h))) tab_test();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (S.want_tab) S.want_tab = nullptr;

  confirm_reset();

  // footer: feedback line and file
  ImGui::Separator();
  const double now = now_s();
  if (now < S.note_until) ImGui::TextColored(t.good, "%s", S.note.c_str());
  else if (S.dirty) ImGui::TextDisabled("Saving...");
  else if (now < S.saved_flash_until) ImGui::TextColored(t.good, "Saved");
  else ImGui::TextDisabled("Changes apply at once and are saved automatically.");
  ImGui::SameLine();
  std::string path = hub().config_path();
  std::string right = "F1 or hold Back+Start (0.5 s) closes the menu     " + path;
  float tw = ImGui::CalcTextSize(right.c_str()).x;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - tw - ImGui::GetStyle().WindowPadding.x));
  ImGui::TextDisabled("%s", right.c_str());
  ImGui::End();
}

bool draw_toast(ImVec2 size, double now) {
  (void)now;
  const std::string msg = hub().message();
  if (msg.empty()) return false;
  const Theme& t = theme();
  const float scale = ImGui::GetFontSize() / t.font_px;
  const float w = std::min(size.x * 0.9f, t.toast_w * scale * 1.6f);
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y - 24.0f * scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, t.toast_bg);
  const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##snaptoast", nullptr, wf)) {
    bool bad = msg.find("invalid") != std::string::npos || msg.find("Could not") != std::string::npos;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(bad ? t.error : t.good, "Controls: %s", msg.c_str());
    ImGui::TextDisabled("Press F1 to open the controls menu.");
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  return true;
}

}  // namespace snap::ui
