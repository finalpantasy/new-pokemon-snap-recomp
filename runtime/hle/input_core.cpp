// SPDX-License-Identifier: MIT
#include "input_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#include "hle.h"

namespace nvn { extern int g_presentCount; extern int g_fdBurst; }   // runtime/nvn/nvn_tex_vk.cpp: frame clock for '@f' script times; nvn_vk.cpp: NVN_FRAME_DUMP burst (script token shot=<n>)

namespace hle::input {
std::atomic<uint64_t> g_inject_buttons{0};   // NpadButton bits a host mod holds down (see Hub::poll); 0 = none
std::atomic<int64_t> g_inject_until_ns{0};   // steady_clock ns until which g_inject_buttons is held

// ---------------------------------------------------------------------------------------------------------------
// names
static const char* kButtonNames[kButtonCount] = {
    "A", "B", "X", "Y", "StickL", "StickR", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
    "StickLLeft", "StickLUp", "StickLRight", "StickLDown", "StickRLeft", "StickRUp", "StickRRight", "StickRDown",
    "LeftSL", "LeftSR", "RightSL", "RightSR"};
const char* button_name(int b) { return b >= 0 && b < kButtonCount ? kButtonNames[b] : "?"; }
int button_from_name(const std::string& n) {
  for (int i = 0; i < kButtonCount; ++i) if (n == kButtonNames[i]) return i;
  return -1;
}

static const char* kPadNames[kPadCount] = {"south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick",
                                           "lshoulder", "rshoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                                           "ltrigger", "rtrigger", "lstick_left", "lstick_right", "lstick_up", "lstick_down",
                                           "rstick_left", "rstick_right", "rstick_up", "rstick_down"};
static_assert(sizeof(kPadNames) / sizeof(kPadNames[0]) == kPadCount, "pad names");
static const char* kMouseNames[kMouseCount] = {"left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"};
const char* mouse_name(int b) { return b >= 0 && b < kMouseCount ? kMouseNames[b] : "?"; }
const char* pad_button_name(int b) { return b >= 0 && b < kPadCount ? kPadNames[b] : "?"; }
int pad_button_from_name(const std::string& n) {
  for (int i = 0; i < kPadCount; ++i) if (n == kPadNames[i]) return i;
  return -1;
}

static std::vector<std::string> make_key_names() {
  std::vector<std::string> k(1, "");
  for (char c = 'A'; c <= 'Z'; ++c) k.push_back(std::string(1, c));
  for (char c = '0'; c <= '9'; ++c) k.push_back(std::string(1, c));
  for (int i = 1; i <= 12; ++i) k.push_back("F" + std::to_string(i));
  for (const char* s : {"Up", "Down", "Left", "Right", "Space", "Enter", "Escape", "Tab", "Backspace", "LShift", "RShift",
                        "LCtrl", "RCtrl", "LAlt", "RAlt", "Comma", "Period", "Slash", "Semicolon", "Quote", "LBracket",
                        "RBracket", "Minus", "Equals", "Backslash", "Grave", "Insert", "Delete", "Home", "End", "PageUp",
                        "PageDown"})
    k.push_back(s);
  for (int i = 0; i <= 9; ++i) k.push_back("Numpad" + std::to_string(i));
  return k;
}
static const std::vector<std::string>& key_names() {
  static const std::vector<std::string> k = make_key_names();
  return k;
}
const char* key_name(int k) { return k > 0 && k < (int)key_names().size() ? key_names()[(size_t)k].c_str() : ""; }
static std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
int key_from_name(const std::string& n) {
  std::string l = lower(n);
  for (size_t i = 1; i < key_names().size(); ++i) if (lower(key_names()[i]) == l) return (int)i;
  return 0;
}
int key_from_ascii(char ch) {
  if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
  if (ch >= 'A' && ch <= 'Z') return 1 + (ch - 'A');
  if (ch >= '0' && ch <= '9') return 27 + (ch - '0');
  return 0;
}

bool parse_source(const std::string& s, Source* out) {
  size_t c = s.find(':');
  if (c == std::string::npos) return false;
  std::string kind = lower(s.substr(0, c)), name = s.substr(c + 1);
  if (kind == "pad") {
    int id = pad_button_from_name(lower(name));
    if (id < 0) return false;
    out->kind = Source::Pad; out->id = id;
    return true;
  }
  if (kind == "key") {
    int id = key_from_name(name);
    if (!id) return false;
    out->kind = Source::KeyboardKey; out->id = id;
    return true;
  }
  if (kind == "mouse") {
    std::string l = lower(name);
    int id = -1;
    for (int i = 0; i < kMouseCount; ++i) if (l == kMouseNames[i]) id = i;
    if (id < 0) return false;
    out->kind = Source::Mouse; out->id = id;
    return true;
  }
  return false;
}
std::string source_to_string(const Source& s) {
  switch (s.kind) {
    case Source::Pad: return std::string("pad:") + pad_button_name(s.id);
    case Source::KeyboardKey: return std::string("key:") + key_name(s.id);
    case Source::Mouse: return std::string("mouse:") + mouse_name(s.id);
    default: return "";
  }
}

const char* device_name(Device d) { return d == kDevKeyboard ? "Keyboard" : d == kDevMouse ? "Mouse" : "Controller"; }
Device device_of(const Source& s) { return s.kind == Source::Pad ? kDevController : s.kind == Source::Mouse ? kDevMouse : kDevKeyboard; }

std::string source_label(const Source& s) {
  switch (s.kind) {
    case Source::KeyboardKey: {
      std::string n = key_name(s.id);
      static const char* const from[] = {"LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt", "PageUp", "PageDown", "LBracket", "RBracket",
                                         "Backslash", "Semicolon", "Comma", "Period", "Slash", "Quote", "Minus", "Equals", "Grave"};
      static const char* const to[] = {"Left Shift", "Right Shift", "Left Ctrl", "Right Ctrl", "Left Alt", "Right Alt", "Page Up", "Page Down", "[", "]",
                                       "\\", ";", ",", ".", "/", "'", "-", "=", "`"};
      for (size_t i = 0; i < sizeof from / sizeof from[0]; ++i) if (n == from[i]) return to[i];
      if (n.rfind("Numpad", 0) == 0) return "Num " + n.substr(6);
      return n;
    }
    case Source::Mouse: {
      static const char* const t[kMouseCount] = {"Mouse Left", "Mouse Right", "Mouse Middle", "Mouse Back", "Mouse Forward", "Wheel Up", "Wheel Down"};
      return s.id >= 0 && s.id < kMouseCount ? t[s.id] : "Mouse ?";
    }
    case Source::Pad: {
      static const char* const t[kPadCount] = {"South (Xbox A)", "East (Xbox B)", "West (Xbox X)", "North (Xbox Y)", "Back / View", "Guide", "Start / Menu",
                                               "L-Stick click", "R-Stick click", "L Shoulder", "R Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
                                               "D-pad Right", "L Trigger", "R Trigger", "L-Stick Left", "L-Stick Right", "L-Stick Up", "L-Stick Down",
                                               "R-Stick Left", "R-Stick Right", "R-Stick Up", "R-Stick Down"};
      return s.id >= 0 && s.id < kPadCount ? t[s.id] : "?";
    }
    default: return "";
  }
}

const char* action_label(int a) {
  static const char* const t[kActionCount] = {"A", "B", "X", "Y", "Left stick click", "Right stick click", "L", "R", "ZL", "ZR", "Plus (+)", "Minus (-)",
                                              "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down",
                                              "Left stick up", "Left stick down", "Left stick left", "Left stick right",
                                              "Right stick up", "Right stick down", "Right stick left", "Right stick right"};
  return a >= 0 && a < kActionCount ? t[a] : "?";
}

// ---------------------------------------------------------------------------------------------------------------
// mapping
static Source S(const char* t) { Source s; parse_source(t, &s); return s; }

Mapping Mapping::defaults() {
  Mapping m;
  auto set = [&](int b, std::initializer_list<const char*> l) { for (auto* t : l) m.buttons[(size_t)b].push_back(S(t)); };
  // Nintendo layout by position: A is the east face button, B south, X north, Y west.
  set(kA, {"pad:east", "key:L"});
  set(kB, {"pad:south", "key:K"});
  set(kX, {"pad:north", "key:I"});
  set(kY, {"pad:west", "key:J"});
  set(kStickL, {"pad:lstick"});
  set(kStickR, {"pad:rstick"});
  set(kL, {"pad:lshoulder", "key:Q"});
  set(kR, {"pad:rshoulder", "key:E"});
  set(kZL, {"pad:ltrigger", "key:LShift"});
  set(kZR, {"pad:rtrigger", "key:Space"});
  set(kPlus, {"pad:start", "key:Enter"});
  set(kMinus, {"pad:back", "key:Backspace"});
  set(kLeft, {"pad:dpad_left", "key:F"});
  set(kUp, {"pad:dpad_up", "key:T"});
  set(kRight, {"pad:dpad_right", "key:H"});
  set(kDown, {"pad:dpad_down", "key:G"});
  m.left.analog = "left";
  m.left.up = {S("key:W")}; m.left.down = {S("key:S")}; m.left.left = {S("key:A")}; m.left.right = {S("key:D")};
  m.right.analog = "right";
  m.right.up = {S("key:Up")}; m.right.down = {S("key:Down")}; m.right.left = {S("key:Left")}; m.right.right = {S("key:Right")};
  return m;
}

static Json sources_json(const std::vector<Source>& v) {
  Json a = Json::array();
  for (auto& s : v) a.a.push_back(Json::string(source_to_string(s)));
  return a;
}
static Json stick_json(const StickMap& s) {
  Json o = Json::object();
  o.set("analog", Json::string(s.analog));
  o.set("deadzone", Json::number(s.deadzone));
  o.set("outer_deadzone", Json::number(s.outer_deadzone));
  o.set("invert_y", Json::boolean_(s.invert_y));
  o.set("up", sources_json(s.up));
  o.set("down", sources_json(s.down));
  o.set("left", sources_json(s.left));
  o.set("right", sources_json(s.right));
  return o;
}

Json Mapping::to_json() const {
  Json j = Json::object();
  j.set("version", Json::number(kVersion));
  j.set("_comment", Json::string("Sources: pad:<south|east|west|north|back|guide|start|lstick|rstick|lshoulder|rshoulder|dpad_up|dpad_down|dpad_left|dpad_right|ltrigger|rtrigger|lstick_left..rstick_down>, key:<name>, mouse:<left|right|middle|x1|x2|wheel_up|wheel_down>. Switch button names: A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down. Edited by the in-game controls menu (F1)."));
  Json c = Json::object();
  c.set("face_layout", Json::string(face_layout == 1 ? "nintendo" : "position"));
  c.set("swap_sticks", Json::boolean_(swap_sticks));
  c.set("zl_threshold", Json::number(zl_threshold));
  c.set("zr_threshold", Json::number(zr_threshold));
  c.set("stick_button_threshold", Json::number(stick_button_threshold));
  j.set("controller", c);
  Json mouse = Json::object();
  mouse.set("touch", Json::boolean_(mouse_touch));
  mouse.set("look", Json::boolean_(mouse_look));
  mouse.set("sensitivity", Json::number(mouse_sensitivity));
  mouse.set("invert_x", Json::boolean_(mouse_invert_x));
  mouse.set("invert_y", Json::boolean_(mouse_invert_y));
  mouse.set("smoothing", Json::number(mouse_smoothing));
  mouse.set("lock_cursor", Json::boolean_(lock_cursor));
  j.set("mouse", mouse);
  Json acc = Json::object();
  acc.set("toggle_zl", Json::boolean_(toggle_zl));
  acc.set("toggle_zr", Json::boolean_(toggle_zr));
  acc.set("ui_scale", Json::number(ui_scale));
  j.set("accessibility", acc);
  Json b = Json::object();
  for (int i = 0; i <= kDown; ++i) b.set(button_name(i), sources_json(buttons[(size_t)i]));
  j.set("buttons", b);
  j.set("left_stick", stick_json(left));
  j.set("right_stick", stick_json(right));
  return j;
}

static bool read_sources(const Json& a, std::vector<Source>* out, std::string* err, const std::string& where) {
  if (a.type != Json::Arr) { *err = where + ": expected an array of source names"; return false; }
  out->clear();
  for (auto& e : a.a) {
    Source s;
    if (e.type != Json::Str || !parse_source(e.s, &s)) { *err = where + ": bad source \"" + e.str() + "\""; return false; }
    out->push_back(s);
  }
  return true;
}
static bool read_stick(const Json& j, StickMap* s, std::string* err, const std::string& where) {
  if (auto* v = j.get("analog")) {
    s->analog = v->str("left");
    if (s->analog != "left" && s->analog != "right" && s->analog != "none") { *err = where + ".analog must be left, right or none"; return false; }
  }
  if (auto* v = j.get("deadzone")) s->deadzone = std::clamp((float)v->num(0.15), 0.0f, 0.9f);
  if (auto* v = j.get("outer_deadzone")) s->outer_deadzone = std::clamp((float)v->num(1.0), 0.5f, 1.0f);
  if (auto* v = j.get("invert_y")) s->invert_y = v->boolean(false);
  s->outer_deadzone = std::max(s->outer_deadzone, s->deadzone + 0.05f);
  if (auto* v = j.get("up")) if (!read_sources(*v, &s->up, err, where + ".up")) return false;
  if (auto* v = j.get("down")) if (!read_sources(*v, &s->down, err, where + ".down")) return false;
  if (auto* v = j.get("left")) if (!read_sources(*v, &s->left, err, where + ".left")) return false;
  if (auto* v = j.get("right")) if (!read_sources(*v, &s->right, err, where + ".right")) return false;
  return true;
}

bool Mapping::from_json(const Json& j, std::string* err) {
  Mapping m = defaults();
  std::string e;
  if (j.type != Json::Obj) { *err = "input map must be a JSON object"; return false; }
  int version = 1;
  if (auto* v = j.get("version")) version = (int)v->num(1);
  if (version > kVersion) { *err = "format version " + std::to_string(version) + " is newer than this build understands (" + std::to_string(kVersion) + ")"; return false; }
  auto clampf = [](const Json* v, double def, float lo, float hi) { return std::clamp((float)v->num(def), lo, hi); };
  if (auto* v = j.get("trigger_threshold")) m.zl_threshold = m.zr_threshold = clampf(v, 0.5, 0.05f, 1.0f);   // version 1
  if (auto* c = j.get("controller")) {
    if (c->type != Json::Obj) { *err = "\"controller\" must be an object"; return false; }
    if (auto* v = c->get("face_layout")) {
      std::string f = v->str("position");
      if (f != "position" && f != "nintendo") { *err = "controller.face_layout must be position or nintendo"; return false; }
      m.face_layout = f == "nintendo" ? 1 : 0;
    }
    if (auto* v = c->get("swap_sticks")) m.swap_sticks = v->boolean(false);
    if (auto* v = c->get("zl_threshold")) m.zl_threshold = clampf(v, 0.5, 0.05f, 1.0f);
    if (auto* v = c->get("zr_threshold")) m.zr_threshold = clampf(v, 0.5, 0.05f, 1.0f);
    if (auto* v = c->get("stick_button_threshold")) m.stick_button_threshold = clampf(v, 0.5, 0.1f, 0.95f);
  }
  if (auto* v = j.get("mouse")) {
    if (v->type != Json::Obj) { *err = "\"mouse\" must be an object"; return false; }
    if (auto* t = v->get("touch")) m.mouse_touch = t->boolean(true);
    if (auto* t = v->get("look")) m.mouse_look = t->boolean(false);
    if (auto* t = v->get("sensitivity")) m.mouse_sensitivity = clampf(t, 1.0, 0.05f, 10.0f);
    if (auto* t = v->get("invert_x")) m.mouse_invert_x = t->boolean(false);
    if (auto* t = v->get("invert_y")) m.mouse_invert_y = t->boolean(false);
    if (auto* t = v->get("smoothing")) m.mouse_smoothing = clampf(t, 0.0, 0.0f, 1.0f);
    if (auto* t = v->get("lock_cursor")) m.lock_cursor = t->boolean(true);
    if (auto* s = v->get("look_sensitivity")) {   // version 1: > 0 turned mouse-look on
      float old = std::max(0.0f, (float)s->num(0));
      if (old > 0) { m.mouse_look = true; m.mouse_sensitivity = 1.0f; }
    }
  }
  if (auto* v = j.get("accessibility")) {
    if (v->type != Json::Obj) { *err = "\"accessibility\" must be an object"; return false; }
    if (auto* t = v->get("toggle_zl")) m.toggle_zl = t->boolean(false);
    if (auto* t = v->get("toggle_zr")) m.toggle_zr = t->boolean(false);
    if (auto* t = v->get("ui_scale")) m.ui_scale = clampf(t, 1.0, 0.5f, 3.0f);
  }
  if (auto* b = j.get("buttons")) {
    if (b->type != Json::Obj) { *err = "\"buttons\" must be an object"; return false; }
    for (auto& kv : b->o) {
      int id = button_from_name(kv.first);
      if (id < 0 || id > kDown) { *err = "buttons: unknown Switch button \"" + kv.first + "\""; return false; }
      if (!read_sources(kv.second, &m.buttons[(size_t)id], &e, "buttons." + kv.first)) { *err = e; return false; }
    }
  }
  if (auto* v = j.get("left_stick")) if (!read_stick(*v, &m.left, &e, "left_stick")) { *err = e; return false; }
  if (auto* v = j.get("right_stick")) if (!read_stick(*v, &m.right, &e, "right_stick")) { *err = e; return false; }
  *this = m;
  return true;
}

// ---- action table and slot editing
std::vector<Source>& action_sources(Mapping& m, int a) {
  static std::vector<Source> dummy;
  if (a >= 0 && a <= kDown) return m.buttons[(size_t)a];
  if (a >= 16 && a < 20) { StickMap& s = m.left; return a == 16 ? s.up : a == 17 ? s.down : a == 18 ? s.left : s.right; }
  if (a >= 20 && a < 24) { StickMap& s = m.right; return a == 20 ? s.up : a == 21 ? s.down : a == 22 ? s.left : s.right; }
  dummy.clear();
  return dummy;
}
const std::vector<Source>& action_sources(const Mapping& m, int a) { return action_sources(const_cast<Mapping&>(m), a); }

int slot_count(const std::vector<Source>& v, Device d) {
  int n = 0;
  for (auto& s : v) if (device_of(s) == d) ++n;
  return n;
}
Source slot_get(const std::vector<Source>& v, Device d, int slot) {
  int n = 0;
  for (auto& s : v) if (device_of(s) == d && n++ == slot) return s;
  return Source{};
}
bool slot_set(std::vector<Source>& v, Device d, int slot, const Source& src) {
  if (src.kind == Source::None || device_of(src) != d || slot < 0 || slot >= kMaxSlotsPerDevice) return false;
  int have = slot_count(v, d);
  if (slot > have) slot = have;   // fill the first free slot
  // a duplicate in the other slot of the same device would be a silent no-op binding: drop it
  for (int o = 0; o < have; ++o) if (o != slot && slot_get(v, d, o) == src) { slot_clear(v, d, o); if (o < slot) --slot; have = slot_count(v, d); break; }
  if (slot >= have) { v.push_back(src); return true; }
  int n = 0;
  for (auto& s : v) if (device_of(s) == d && n++ == slot) { s = src; break; }
  return true;
}
void slot_clear(std::vector<Source>& v, Device d, int slot) {
  int n = 0;
  for (size_t i = 0; i < v.size(); ++i)
    if (device_of(v[i]) == d && n++ == slot) { v.erase(v.begin() + (long)i); return; }
}
std::vector<int> actions_using(const Mapping& m, const Source& src) {
  std::vector<int> out;
  for (int a = 0; a < kActionCount; ++a)
    for (auto& s : action_sources(m, a)) if (s == src) { out.push_back(a); break; }
  return out;
}
void reset_device(Mapping& m, Device d) {
  Mapping def = Mapping::defaults();
  for (int a = 0; a < kActionCount; ++a) {
    auto& v = action_sources(m, a);
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Source& s) { return device_of(s) == d; }), v.end());
    for (auto& s : action_sources(def, a)) if (device_of(s) == d) v.push_back(s);
  }
  if (d == kDevController) {
    m.zl_threshold = def.zl_threshold; m.zr_threshold = def.zr_threshold; m.stick_button_threshold = def.stick_button_threshold;
    m.swap_sticks = false; m.face_layout = 0;
    for (StickMap* s : {&m.left, &m.right}) { StickMap* ds = s == &m.left ? &def.left : &def.right; s->analog = ds->analog; s->deadzone = ds->deadzone; s->outer_deadzone = ds->outer_deadzone; s->invert_y = false; }
  } else if (d == kDevMouse) {
    m.mouse_touch = def.mouse_touch; m.mouse_look = false; m.mouse_sensitivity = def.mouse_sensitivity;
    m.mouse_invert_x = m.mouse_invert_y = false; m.mouse_smoothing = 0; m.lock_cursor = def.lock_cursor;
  }
}
void set_face_layout(Mapping& m, int layout) {
  layout = layout == 1 ? 1 : 0;
  if (layout == m.face_layout) return;
  m.face_layout = layout;
  auto flip = [](int id) { return id == kPadSouth ? kPadEast : id == kPadEast ? kPadSouth : id == kPadWest ? kPadNorth : id == kPadNorth ? kPadWest : id; };
  for (int a = 0; a < kActionCount; ++a)
    for (auto& s : action_sources(m, a)) if (s.kind == Source::Pad) s.id = flip(s.id);
}

bool source_down(const Source& s, const RawInput& in, float trigger_thr) {
  switch (s.kind) {
    case Source::Pad:
      switch (s.id) {
        case kPadLTrigger: return in.pad.test((size_t)s.id) || in.lt >= trigger_thr;
        case kPadRTrigger: return in.pad.test((size_t)s.id) || in.rt >= trigger_thr;
        case kPadLStickLeft: return in.lx <= -0.5f;
        case kPadLStickRight: return in.lx >= 0.5f;
        case kPadLStickUp: return in.ly >= 0.5f;
        case kPadLStickDown: return in.ly <= -0.5f;
        case kPadRStickLeft: return in.rx <= -0.5f;
        case kPadRStickRight: return in.rx >= 0.5f;
        case kPadRStickUp: return in.ry >= 0.5f;
        case kPadRStickDown: return in.ry <= -0.5f;
        default: return in.pad.test((size_t)s.id);
      }
    case Source::KeyboardKey: return in.keys.test((size_t)s.id);
    case Source::Mouse: return (in.mouse_buttons >> s.id) & 1;
    default: return false;
  }
}
static bool any_down(const std::vector<Source>& v, const RawInput& in, float thr) {
  for (auto& s : v) if (source_down(s, in, thr)) return true;
  return false;
}

PadFx& pad_fx() { static PadFx f; return f; }

// Environment overrides (test and power-user switches): SNAP_RUMBLE=0..1.5 (0 = off), SNAP_RUMBLE_LOG=1, SNAP_GYRO=0/1,
// SNAP_GYRO_SENS=<float>, SNAP_GYRO_FAKE=<yaw_dps>,<pitch_dps> (a pretend gyro on the pad, left/up positive like SDL).
struct EnvFx {
  bool rumble_set = false; float rumble = 1.0f; bool rumble_log = false;
  bool gyro_set = false, gyro = false; bool sens_set = false; float sens = 1.0f;
  bool fake = false; float fake_yaw = 0, fake_pitch = 0;
};
static const EnvFx& env_fx() {
  static const EnvFx e = [] {
    EnvFx r;
    if (const char* v = std::getenv("SNAP_RUMBLE"); v && *v) { r.rumble_set = true; r.rumble = std::clamp((float)std::atof(v), 0.0f, 1.5f); }
    if (const char* v = std::getenv("SNAP_RUMBLE_LOG"); v && *v && *v != '0') r.rumble_log = true;
    if (const char* v = std::getenv("SNAP_GYRO"); v && *v) { r.gyro_set = true; r.gyro = *v != '0'; }
    if (const char* v = std::getenv("SNAP_GYRO_SENS"); v && *v) { r.sens_set = true; r.sens = std::max(0.0f, (float)std::atof(v)); }
    if (const char* v = std::getenv("SNAP_GYRO_FAKE"); v && *v) {
      r.fake = true; r.fake_yaw = (float)std::atof(v);
      if (const char* c = std::strchr(v, ',')) r.fake_pitch = (float)std::atof(c + 1);
      if (!r.gyro_set) { r.gyro_set = true; r.gyro = true; }
    }
    return r;
  }();
  return e;
}
bool rumble_log_enabled() { return env_fx().rumble_log; }
bool gyro_aim_enabled() { return env_fx().gyro_set ? env_fx().gyro : pad_fx().gyro_on.load(); }

void gyro_to_stick(float yaw, float pitch, float sens, bool inv_x, bool inv_y, float* sx, float* sy) {
  auto dz = [](float v) {
    float a = std::fabs(v);
    if (a <= kGyroDeadDegPerSec) return 0.0f;
    return std::copysign(a - kGyroDeadDegPerSec, v);   // subtract the dead zone so there is no jump at its edge
  };
  float s = std::max(0.0f, sens) / (kGyroFullDegPerSec - kGyroDeadDegPerSec);
  float x = -dz(yaw) * s;      // turning the pad to the right (negative yaw rate) pushes the stick right
  float y = dz(pitch) * s;     // tipping the front up pushes the stick up
  *sx = std::clamp(inv_x ? -x : x, -1.0f, 1.0f);
  *sy = std::clamp(inv_y ? -y : y, -1.0f, 1.0f);
}

static void stick_value(const StickMap& sm, bool is_left, const Mapping& m, const RawInput& in, float mouse_x, float mouse_y, float* ox, float* oy) {
  float x = 0, y = 0;
  std::string analog = sm.analog;
  if (m.swap_sticks && analog != "none") analog = analog == "left" ? "right" : "left";
  (void)is_left;
  if (analog != "none") {
    float ax = analog == "left" ? in.lx : in.rx, ay = analog == "left" ? in.ly : in.ry;
    if (sm.invert_y) ay = -ay;
    float mag = std::sqrt(ax * ax + ay * ay);
    if (mag > sm.deadzone) {
      float span = std::max(0.01f, sm.outer_deadzone - sm.deadzone);
      float scale = std::min(1.0f, (mag - sm.deadzone) / span) / mag;  // radial: inner dead zone, saturation at the outer one
      x = ax * scale; y = ay * scale;
    }
  }
  float thr = m.zl_threshold;
  x += (any_down(sm.right, in, thr) ? 1.f : 0.f) - (any_down(sm.left, in, thr) ? 1.f : 0.f);
  y += (any_down(sm.up, in, thr) ? 1.f : 0.f) - (any_down(sm.down, in, thr) ? 1.f : 0.f);
  x += mouse_x; y += mouse_y;
  float mag = std::sqrt(x * x + y * y);
  if (mag > 1.0f) { x /= mag; y /= mag; }  // keep the diagonal on the unit circle
  *ox = x; *oy = y;
}

PadSample map_pad(const Mapping& m, const RawInput& in) {
  PadSample out;
  RawInput eff = in;
  if (!in.focused) { eff.keys.reset(); eff.mouse_buttons = 0; }
  out.connected = in.pad_connected || in.focused;  // the keyboard is always a controller while the window has focus
  for (int b = 0; b <= kDown; ++b) {
    float thr = b == kZR ? m.zr_threshold : m.zl_threshold;
    for (auto& s : m.buttons[(size_t)b]) {
      // the trigger threshold of the physical trigger, not of the Switch button it is bound to
      float t = s.kind == Source::Pad && s.id == kPadRTrigger ? m.zr_threshold : s.kind == Source::Pad && s.id == kPadLTrigger ? m.zl_threshold : thr;
      if (source_down(s, eff, t)) { out.buttons |= 1ull << b; break; }
    }
  }
  float lx, ly, rx, ry;
  stick_value(m.left, true, m, eff, 0, 0, &lx, &ly);
  float mlx = 0, mly = 0;
  if (m.mouse_look && eff.focused) {
    mlx = std::clamp(eff.mouse_vx * m.mouse_sensitivity / (float)kMouseLookFullPxPerSec, -1.0f, 1.0f) * (m.mouse_invert_x ? -1.f : 1.f);
    mly = std::clamp(-eff.mouse_vy * m.mouse_sensitivity / (float)kMouseLookFullPxPerSec, -1.0f, 1.0f) * (m.mouse_invert_y ? -1.f : 1.f);
  }
  const EnvFx& ev = env_fx();
  if (eff.gyro_present && (ev.gyro_set ? ev.gyro : pad_fx().gyro_on.load())) {
    float gx, gy;
    gyro_to_stick(eff.gyro_y, eff.gyro_x, ev.sens_set ? ev.sens : pad_fx().gyro_sens.load(), pad_fx().gyro_invert_x.load(), pad_fx().gyro_invert_y.load(), &gx, &gy);
    mlx += gx; mly += gy;
  }
  stick_value(m.right, false, m, eff, mlx, mly, &rx, &ry);
  auto q = [](float v) { return (int32_t)std::lround(std::clamp(v, -1.0f, 1.0f) * kStickMax); };
  out.lx = q(lx); out.ly = q(ly); out.rx = q(rx); out.ry = q(ry);
  float t = m.stick_button_threshold;
  if (lx <= -t) out.buttons |= 1ull << kStickLLeft;
  if (ly >= t) out.buttons |= 1ull << kStickLUp;
  if (lx >= t) out.buttons |= 1ull << kStickLRight;
  if (ly <= -t) out.buttons |= 1ull << kStickLDown;
  if (rx <= -t) out.buttons |= 1ull << kStickRLeft;
  if (ry >= t) out.buttons |= 1ull << kStickRUp;
  if (rx >= t) out.buttons |= 1ull << kStickRRight;
  if (ry <= -t) out.buttons |= 1ull << kStickRDown;
  return out;
}

TouchSample map_touch(const Mapping& m, const RawInput& in) {
  TouchSample t;
  if (m.mouse_touch && !m.mouse_look && in.focused && in.mouse_in_window && (in.mouse_buttons & 1)) {
    t.count = 1;
    t.pt[0].x = (int32_t)std::clamp(in.mouse_x * 1280.0f, 0.0f, 1279.0f);
    t.pt[0].y = (int32_t)std::clamp(in.mouse_y * 720.0f, 0.0f, 719.0f);
  }
  return t;
}

// ---------------------------------------------------------------------------------------------------------------
// hub
static uint64_t now_ns() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Hub::Hub() : map_(Mapping::defaults()) {}

void Hub::set_backend(std::unique_ptr<Backend> b) {
  std::lock_guard<std::mutex> g(mu_);
  backend_ = std::move(b);
  last_poll_ns_ = 0;
}
void Hub::set_mapping(const Mapping& m) { std::lock_guard<std::mutex> g(mu_); map_ = m; }
Mapping Hub::mapping() const { std::lock_guard<std::mutex> g(mu_); return map_; }

bool write_mapping_file(const Mapping& m, const std::string& path, std::string* err) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path target = fs::u8path(path);
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  std::string text;
  json_dump(m.to_json(), text);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + tmp.string(); return false; }
    f << text << "\n";
    f.flush();
    if (!f) { if (err) *err = "write failed: " + tmp.string(); return false; }
  }
  fs::rename(tmp, target, ec);   // replaces an existing file in one step
  if (ec) { fs::remove(tmp, ec); if (err) *err = "cannot replace " + path; return false; }
  return true;
}

bool write_default_mapping(const std::string& path) { return write_mapping_file(Mapping::defaults(), path, nullptr); }

static bool read_mapping_file(const std::string& path, Mapping* m, std::string* err) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) { *err = path + ": cannot open"; return false; }
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  std::string e;
  if (!json_parse(ss.str(), j, e)) { *err = path + ": " + e; return false; }
  if (!m->from_json(j, &e)) { *err = path + ": " + e; return false; }
  return true;
}

bool Hub::load_mapping(const std::string& path, std::string* err) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    write_default_mapping(path);  // first run: leave an editable template
    set_mapping(Mapping::defaults());
    std::lock_guard<std::mutex> g(mu_);
    path_ = path;
    return true;
  }
  Mapping m;
  std::string e;
  if (!read_mapping_file(path, &m, &e)) { if (err) *err = e; return false; }
  set_mapping(m);
  std::lock_guard<std::mutex> g(mu_);
  path_ = path;
  return true;
}

void Hub::set_message_locked(const std::string& m) { message_ = m; ++message_serial_; }
std::string Hub::message() const { std::lock_guard<std::mutex> g(mu_); return message_; }
uint64_t Hub::message_serial() const { std::lock_guard<std::mutex> g(mu_); return message_serial_; }
std::string Hub::config_path() const { std::lock_guard<std::mutex> g(mu_); return path_; }

bool Hub::load_config(const std::string& path, const std::string& legacy) {
  namespace fs = std::filesystem;
  std::error_code ec;
  Mapping m;
  std::string e;
  bool ok = true;
  std::string note;
  if (fs::exists(path, ec)) {
    if (!read_mapping_file(path, &m, &e)) {
      ok = false;
      m = Mapping::defaults();
      fs::path bad = path;
      bad += ".invalid";
      fs::copy_file(path, bad, fs::copy_options::overwrite_existing, ec);
      note = "Controls file is invalid, using default controls. " + e + " (kept as " + bad.string() + "; saving a change overwrites input.json)";
    }
  } else if (!legacy.empty() && fs::exists(legacy, ec)) {
    if (read_mapping_file(legacy, &m, &e)) {
      std::string werr;
      write_mapping_file(m, path, &werr);
      note = "Migrated the old controls file " + legacy + " to " + path;
    } else {
      ok = false;
      m = Mapping::defaults();
      note = "The old controls file is invalid, using default controls. " + e;
    }
  } else {
    m = Mapping::defaults();
    std::string werr;
    write_mapping_file(m, path, &werr);
  }
  std::lock_guard<std::mutex> g(mu_);
  map_ = m;
  path_ = path;
  if (!note.empty()) { set_message_locked(note); log("input: %s", note.c_str()); }
  return ok;
}

bool Hub::save(std::string* err) {
  Mapping m;
  std::string path;
  {
    std::lock_guard<std::mutex> g(mu_);
    m = map_;
    path = path_;
  }
  if (path.empty()) { if (err) *err = "no controls file path"; return false; }
  std::string e;
  bool ok = write_mapping_file(m, path, &e);
  std::lock_guard<std::mutex> g(mu_);
  if (!ok) set_message_locked("Could not save the controls: " + e);
  if (err && !ok) *err = e;
  return ok;
}

// ---- scripted input (SNAP_INPUT_SCRIPT=<file>): timed presses OR-ed onto whatever the host devices report.
// One entry per line, '#' starts a comment, times are seconds since the first poll:
//   <start> <hold> <token>...                      press the tokens from start for hold seconds
//   repeat <start> <end> <period> <hold> <token>...  the same press every period seconds in [start, end)
//   anchor <event>                                 starts a new segment: its entries count seconds from the moment the game
//                                                  raises <event> (script_event; "swkbd" = the name prompt opened) and the
//                                                  entries of earlier segments stop. Until the event fires the old segment runs on.
//   wait <cond> [timeout=<s>] [for=<n>]            the same, but the segment starts when a game-state condition holds (for n
//                                                  presents in a row, default 1). timeout (default 300 s, 0 = none; anchors take
//                                                  it too) counts from the start of the segment before: when it runs out the
//                                                  run logs "input script: TIMEOUT" with the state it saw and exits (code 3).
//     conditions (host/mods/routes.cpp, neofilm.cpp, skips.cpp report them; '!' in front negates):
//       ui=<scene>             a Lumen UI scene (.drp base names: title, main_menu, map_select, photo_submit, evaluation,
//                              pause_menu, option, result_reward, ...) is open (between its "opened" and "closed" events)
//       ui=<scene>:<state>     that scene is open and in that layout state (a trailing '*' matches a prefix); states seen:
//                              title PressStart, main_menu MainMenu, map_select Start/StageSelect/CourseSelect/Exit,
//                              photo_submit FolderSelect (subject folders) / PhotoSubmit (photos of one folder),
//                              evaluation Start, per photo EvaluationNew/Comment/Score/Announce/PhotoRegister, then End
//       dialog                 the dialogue / skit text window waits for A (main+0x676630 ran in the last 8 presents)
//       ride                   the course rail is moving (NEO-ONE progress advanced in the last 20 presents)
//       ride>=<x>              moving and the progress along the rail (rail object +0x70, about 0.9 per second) is at least x
//       ride_end               a ride was seen and the rail has not moved for 90 presents
//       film<=<n>              the course HUD's film counter (72 at the start) is at most n
// Frame timing (load-independent): write the start as @f<frame> (e.g. "@f600 30 A", "repeat @f0 @f3000 90 15 A") and the whole line counts
// guest presents (nvn present count) since the segment began, so a slow PC does not shift it; end/period/hold may carry an optional @f prefix.
// A token is a Switch button name (A B X Y L R ZL ZR Plus Minus Left Up Right Down StickL StickR ...) or an axis
// lx=<f> ly=<f> rx=<f> ry=<f> with f in [-1, 1] (y up positive).
namespace {
struct ScriptEntry { double start, end, period, hold; uint64_t buttons; int32_t ax[4]; bool has_ax[4]; int seg; bool frames; float rumble_lo, rumble_hi; bool has_rumble; float gyro_yaw, gyro_pitch; bool has_gyro; bool is_end; int shot_n; };
struct Script {
  bool loaded = false;
  uint64_t t0 = 0;
  std::vector<ScriptEntry> e;
  std::vector<std::string> seg_event;   // seg_event[k] raises segment k+1 ("?<cond>" = a "wait" line)
  std::vector<double> seg_timeout;       // seconds segment k may wait for seg_event[k] (0 = forever)
  std::vector<int> seg_hold;             // presents the wait condition must hold
  int load_seg = 0;                      // segment being parsed
  std::atomic<int> cur{0};               // segment running
  std::atomic<uint64_t> cur_t0{0};       // start time (ns) of the running segment
  std::atomic<int> cur_f0{0};            // present count at the start of the running segment
};
Script g_script;

// ---- route signals: game state that inline hooks report (host/mods/routes.cpp, neofilm.cpp, skips.cpp); "wait" lines read it.
// Off (every report returns at once) unless the script has a "wait" line or SNAP_ROUTE_LOG=1 (which also logs scenes and states).
struct UiScene { uint64_t player; char name[40]; char state[64]; int f_event, f_state; bool open; };
struct RailSig { uint64_t obj; float prog; int f_adv, f_start; };
std::mutex g_rs_mu;
UiScene g_ui[64];
int g_nui;
RailSig g_rail[8];
int g_nrail, g_rail_cur = -1;
std::atomic<bool> g_route_on{false};
bool g_route_log;
std::atomic<int> g_dialog_f{-1000000}, g_film{-1}, g_film_f{-1000000};
void route_enable() { g_route_on = true; }
struct RouteInit { RouteInit() { const char* e = std::getenv("SNAP_ROUTE_LOG"); g_route_log = e && *e && *e != '0'; if (g_route_log) g_route_on = true; } } g_route_init;

// evaluates one "wait" condition (text after '?'); `why` gets a short state note for the log
bool route_cond(const std::string& c0, std::string* why) {
  const bool neg = !c0.empty() && c0[0] == '!';
  const std::string c = neg ? c0.substr(1) : c0;
  const int f = nvn::g_presentCount;
  bool r = false;
  char b[160] = "";
  std::lock_guard<std::mutex> lk(g_rs_mu);
  if (c.rfind("ui=", 0) == 0) {
    std::string scene = c.substr(3), st;
    size_t col = scene.find(':');
    if (col != std::string::npos) { st = scene.substr(col + 1); scene.resize(col); }
    for (int i = 0; i < g_nui && !r; ++i) {
      const UiScene& s = g_ui[i];
      if (scene != s.name) continue;
      r = s.open;
      if (r && !st.empty()) r = st.back() == '*' ? std::strncmp(s.state, st.c_str(), st.size() - 1) == 0 : st == s.state;
      std::snprintf(b, sizeof b, "%s %s, state '%s' for %d presents", s.name, s.open ? "open" : "closed", s.state, f - s.f_state);
    }
    if (!b[0]) std::snprintf(b, sizeof b, "scene %s not seen", scene.c_str());
  } else if (c == "dialog") {
    r = f - g_dialog_f.load() <= 8;
    std::snprintf(b, sizeof b, "dialog window last waited %d presents ago", f - g_dialog_f.load());
  } else if (c == "ride" || c.rfind("ride>=", 0) == 0 || c == "ride_end") {
    const RailSig* rs = g_rail_cur >= 0 ? &g_rail[g_rail_cur] : nullptr;
    if (c == "ride_end") r = rs && f - rs->f_adv > 90;
    else r = rs && f - rs->f_adv <= 20 && (c == "ride" || rs->prog >= (float)std::atof(c.c_str() + 6));
    if (rs) std::snprintf(b, sizeof b, "rail progress %.2f, last advance %d presents ago", rs->prog, f - rs->f_adv);
    else std::snprintf(b, sizeof b, "no rail seen");
  } else if (c.rfind("film<=", 0) == 0) {
    r = g_film.load() >= 0 && g_film.load() <= std::atoi(c.c_str() + 6);
    std::snprintf(b, sizeof b, "film counter %d", g_film.load());
  } else {
    std::snprintf(b, sizeof b, "unknown condition");
  }
  if (why) *why = b;
  return neg ? !r : r;
}

// one line per known scene plus rail, film and dialog (timeouts)
void route_dump() {
  std::lock_guard<std::mutex> lk(g_rs_mu);
  const int f = nvn::g_presentCount;
  for (int i = 0; i < g_nui; ++i)
    log("input script:   ui %-16s %s, state '%s' for %d presents, last event %d presents ago", g_ui[i].name, g_ui[i].open ? "open  " : "closed",
        g_ui[i].state, f - g_ui[i].f_state, f - g_ui[i].f_event);
  if (g_rail_cur >= 0) log("input script:   rail progress %.2f, last advance %d presents ago", g_rail[g_rail_cur].prog, f - g_rail[g_rail_cur].f_adv);
  log("input script:   film %d, dialog last waited %d presents ago, present %d", g_film.load(), f - g_dialog_f.load(), f);
}
}  // namespace

bool route_on() { return g_route_on.load(std::memory_order_relaxed); }

void route_ui(uint64_t player, const char* scene, const char* state, int event, const char* elem, uint64_t lr) {
  const int f = nvn::g_presentCount;
  std::lock_guard<std::mutex> lk(g_rs_mu);
  UiScene* s = nullptr;
  for (int i = 0; i < g_nui; ++i) if (g_ui[i].player == player) { s = &g_ui[i]; break; }
  if (s && std::strcmp(s->name, scene) != 0) {   // the scene was freed and a new one got its address
    if (g_route_log) log("route: ui player %#llx was '%s', now '%s'", (unsigned long long)player, s->name, scene);
    s->player = 0;
    s = nullptr;
  }
  if (!s) {
    for (int i = 0; i < g_nui; ++i) if (!g_ui[i].player) { s = &g_ui[i]; break; }
    if (!s && g_nui >= 64) return;
    if (!s) s = &g_ui[g_nui++];
    *s = UiScene{};
    s->player = player;
    std::snprintf(s->name, sizeof s->name, "%s", scene);
    s->f_state = f;
    if (g_route_log) log("route: ui scene '%s' player %#llx first %s from main+%#llx, present %d", scene, (unsigned long long)player,
                         event < 0 ? "SetState" : "event", (unsigned long long)(lr - 0x7100000000ull), f);
  }
  if (event >= 0) s->f_event = f;
  if (event == 1 || event == 3) s->open = false;   // 1 = built (hidden), 3 = closed
  if (event == 2) s->open = true;                  // 2 = opened
  if (std::strcmp(s->state, state) != 0 && state[0]) {   // SetState also passes '' between two states of one page: ignored
    if (g_route_log) log("route: ui %s state '%s' -> '%s' (%s %d) present %d", s->name, s->state, state, event < 0 ? "SetState" : "event", event, f);
    std::snprintf(s->state, sizeof s->state, "%s", state);
    s->f_state = f;
  }
  if (g_route_log && event >= 0 && event != 22) log("route: ui %s event %d state '%s' element '%s' present %d", s->name, event, state, elem, f);
}

void route_rail(uint64_t obj, float prog) {
  if (!route_on()) return;
  const int f = nvn::g_presentCount;
  std::lock_guard<std::mutex> lk(g_rs_mu);
  int k = -1;
  for (int i = 0; i < g_nrail; ++i) if (g_rail[i].obj == obj) { k = i; break; }
  if (k < 0) {
    k = g_nrail < 8 ? g_nrail++ : 7;
    g_rail[k] = RailSig{obj, prog, -1000000, f};
    if (g_route_log) log("route: rail object %#llx progress %.2f present %d", (unsigned long long)obj, prog, f);
  }
  RailSig& r = g_rail[k];
  if (prog < r.prog - 0.5f) { r.f_start = f; if (g_route_log) log("route: rail %#llx restarts (%.2f -> %.2f) present %d", (unsigned long long)obj, r.prog, prog, f); }
  if (prog > r.prog) {
    if (g_route_log && f - r.f_adv > 20) log("route: rail %#llx moving at %.2f present %d", (unsigned long long)obj, prog, f);
    r.f_adv = f;
    g_rail_cur = k;
  }
  r.prog = prog;
}

void route_dialog() {
  if (!route_on()) return;
  const int f = nvn::g_presentCount;
  if (g_route_log && f - g_dialog_f.load() > 8) log("route: dialog window waiting, present %d", f);
  g_dialog_f = f;
}

void route_film(int count) {
  if (!route_on()) return;
  if (g_route_log && count != g_film.load()) log("route: film counter %d present %d", count, nvn::g_presentCount);
  g_film = count;
  g_film_f = nvn::g_presentCount;
}

namespace {
// "@f120" or "120" -> number; sets fr when the @f prefix is present
double script_num(const std::string& w, bool* fr = nullptr) {
  bool f = w.size() > 2 && w[0] == '@' && w[1] == 'f';
  if (fr && f) *fr = true;
  return std::atof(w.c_str() + (f ? 2 : 0));
}

bool script_token(const std::string& t, ScriptEntry& en) {
  size_t eq = t.find('=');
  if (eq != std::string::npos) {
    std::string k = t.substr(0, eq);
    if (k == "gyro") {   // gyro=<yaw_dps>,<pitch_dps>: a pretend gyro rate while the entry is active (needs SNAP_GYRO=1)
      en.has_gyro = true; en.gyro_yaw = (float)std::atof(t.c_str() + eq + 1);
      size_t c = t.find(',', eq); en.gyro_pitch = c == std::string::npos ? 0.0f : (float)std::atof(t.c_str() + c + 1);
      return true;
    }
    if (k == "shot") {   // shot=<n>: while the entry is active, ask NVN_FRAME_DUMP for a frame every n presents (needs NVN_FRAME_DUMP; test evidence aid)
      en.shot_n = std::max(1, std::atoi(t.c_str() + eq + 1));
      return true;
    }
    if (k == "rumble") {   // rumble=<low>,<high>: a host-side rumble request while the entry is active (test of the host chain)
      en.has_rumble = true; en.rumble_lo = (float)std::atof(t.c_str() + eq + 1);
      size_t c = t.find(',', eq); en.rumble_hi = c == std::string::npos ? en.rumble_lo : (float)std::atof(t.c_str() + c + 1);
      return true;
    }
    int i = k == "lx" ? 0 : k == "ly" ? 1 : k == "rx" ? 2 : k == "ry" ? 3 : -1;
    if (i < 0) return false;
    double f = std::atof(t.c_str() + eq + 1);
    en.ax[i] = (int32_t)std::lround(std::clamp(f, -1.0, 1.0) * 32767.0);
    en.has_ax[i] = true;
    return true;
  }
  int b = button_from_name(t);
  if (b < 0) return false;
  en.buttons |= 1ull << b;
  return true;
}

void script_load() {
  g_script.loaded = true;
  const char* path = std::getenv("SNAP_INPUT_SCRIPT");
  if (!path || !*path) return;
  std::ifstream f(path);
  if (!f) { log("input script: cannot open %s", path); return; }
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    std::istringstream is(line);
    std::string w;
    if (!(is >> w)) continue;
    if (w == "anchor" || w == "wait") {
      std::string ev, o;
      double to = w == "wait" ? 300.0 : 0.0;
      int hold = 1;
      if (!(is >> ev)) { log("input script: bad line '%s'", line.c_str()); continue; }
      while (is >> o) {
        if (o.rfind("timeout=", 0) == 0) to = std::atof(o.c_str() + 8);
        else if (o.rfind("for=", 0) == 0) hold = std::max(1, std::atoi(o.c_str() + 4));
        else log("input script: unknown option '%s' in '%s'", o.c_str(), line.c_str());
      }
      if (w == "wait") { ev = "?" + ev; route_enable(); }
      g_script.seg_event.push_back(ev); g_script.seg_timeout.push_back(to); g_script.seg_hold.push_back(hold);
      ++g_script.load_seg;
      continue;
    }
    ScriptEntry en{};
    en.seg = g_script.load_seg;
    if (w == "end") {   // end <start>: the route is done; the run exits with code 0 at that time of the segment
      std::string a;
      if (!(is >> a)) a = "0";
      en.start = en.end = script_num(a, &en.frames);
      en.period = 1; en.hold = 1e9; en.is_end = true;
      g_script.e.push_back(en);
      ++n;
      continue;
    }
    bool rep = w == "repeat";
    if (rep) {
      std::string a, b, c, d;
      if (is >> a >> b >> c >> d) { en.start = script_num(a, &en.frames); en.end = script_num(b); en.period = script_num(c); en.hold = script_num(d); }
      if (a.empty() || d.empty() || en.period <= 0) { log("input script: bad line '%s'", line.c_str()); continue; }
    } else {
      en.start = script_num(w, &en.frames);
      en.end = en.start;
      en.period = 1;
      std::string hs;
      if (is >> hs) en.hold = script_num(hs);
      else { log("input script: bad line '%s'", line.c_str()); continue; }
    }
    bool ok = true;
    while (is >> w) ok &= script_token(w, en);
    if (!ok) { log("input script: unknown token in '%s'", line.c_str()); continue; }
    g_script.e.push_back(en);
    ++n;
  }
  log("input script: %d entries from %s", n, path);
}

// Applies the script at time `now` (seconds since the first poll) to `p`.
float g_script_rumble_lo = 0, g_script_rumble_hi = 0;
bool g_script_gyro_on = false; float g_script_gyro_yaw = 0, g_script_gyro_pitch = 0;   // read by Hub::poll after script_apply
// "wait" lines and timeouts: starts the next segment when the pending condition holds, or ends the run when time runs out.
void script_check_wait(uint64_t now_ns_v) {
  static int held_k = -1, held_n = 0, held_f = -1;
  const int k = g_script.cur.load();
  if (k >= (int)g_script.seg_event.size()) return;
  const std::string& ev = g_script.seg_event[(size_t)k];
  const double el = (double)(now_ns_v - g_script.cur_t0.load()) * 1e-9;
  if (ev[0] == '?') {
    std::string why;
    const int f = nvn::g_presentCount;
    if (route_cond(ev.substr(1), &why)) {
      if (held_k != k) { held_k = k; held_n = 0; held_f = -1; }
      if (f != held_f) { held_f = f; ++held_n; }
      if (held_n >= g_script.seg_hold[(size_t)k]) {
        g_script.cur_t0 = now_ns_v;
        g_script.cur_f0 = f;
        g_script.cur = k + 1;
        log("input script: wait '%s' met after %.1f s at present %d (%s): segment %d", ev.c_str() + 1, el, f, why.c_str(), k + 1);
        return;
      }
    } else if (held_k == k) {
      held_n = 0;
    }
  }
  const double to = g_script.seg_timeout[(size_t)k];
  if (to > 0 && el > to) {
    std::string why;
    if (ev[0] == '?') route_cond(ev.substr(1), &why);
    log("input script: TIMEOUT after %.0f s waiting for %s '%s' (segment %d, %s); game state:", el, ev[0] == '?' ? "condition" : "event",
        ev.c_str() + (ev[0] == '?'), k, why.empty() ? "-" : why.c_str());
    route_dump();
    const char* e = std::getenv("SNAP_ROUTE_TIMEOUT");
    if (e && !std::strcmp(e, "continue")) { g_script.seg_timeout[(size_t)k] = 0; return; }   // keep running (debug)
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
  }
}

void script_apply(uint64_t now_ns_v, PadSample& p) {
  script_check_wait(now_ns_v);
  g_script_rumble_lo = g_script_rumble_hi = 0; g_script_gyro_on = false; g_script_gyro_yaw = g_script_gyro_pitch = 0;
  static uint64_t last = ~0ull;
  uint64_t cur = 0;
  const int seg = g_script.cur.load();
  const double now_t = (double)(now_ns_v - g_script.cur_t0.load()) * 1e-9;
  const double now_f = (double)(nvn::g_presentCount - g_script.cur_f0.load());
  double now = now_t;
  for (const ScriptEntry& en : g_script.e) {
    if (en.seg != seg) continue;
    now = en.frames ? now_f : now_t;
    bool on;
    if (en.end > en.start) {   // repeat
      on = now >= en.start && now < en.end && std::fmod(now - en.start, en.period) < en.hold;
    } else {
      on = now >= en.start && now < en.start + en.hold;
    }
    if (!on) continue;
    if (en.is_end) {
      log("input script: end of route reached (segment %d, present %d); exiting", seg, nvn::g_presentCount);
      std::fflush(stdout);
      std::fflush(stderr);
      std::_Exit(0);
    }
    cur |= en.buttons;
    p.buttons |= en.buttons;
    if (en.has_gyro) { g_script_gyro_on = true; g_script_gyro_yaw += en.gyro_yaw; g_script_gyro_pitch += en.gyro_pitch; }
    if (en.shot_n > 0 && nvn::g_presentCount % en.shot_n == 0 && nvn::g_fdBurst <= 0) nvn::g_fdBurst = 4;   // `shot=<n>` token
    if (en.has_rumble) { g_script_rumble_lo = std::max(g_script_rumble_lo, en.rumble_lo); g_script_rumble_hi = std::max(g_script_rumble_hi, en.rumble_hi); }
    for (int i = 0; i < 4; ++i) if (en.has_ax[i]) (&p.lx)[i] = en.ax[i];
    p.connected = true;
  }
  if (cur != last) { last = cur; log("input script: seg %d t=%.2f buttons=%#llx", seg, now, (unsigned long long)cur); }
}
}  // namespace

void script_event(const char* name) {
  if (!g_script.loaded) return;
  int k = g_script.cur.load();
  if (k < (int)g_script.seg_event.size() && g_script.seg_event[(size_t)k] == name) {
    g_script.cur_t0 = now_ns();
    g_script.cur_f0 = nvn::g_presentCount;
    g_script.cur = k + 1;
    log("input script: event '%s' starts segment %d", name, k + 1);
  }
}

// ---- window-thread wheel feed
static std::atomic<int> g_wheel_delta{0};
void post_wheel(int delta) { g_wheel_delta.fetch_add(delta); }

static uint32_t src_key(const Source& s) { return ((uint32_t)s.kind << 8) | (uint32_t)s.id; }
static bool capture_allowed(const Source& s, unsigned mask) { return (mask >> (int)device_of(s)) & 1; }
static void each_candidate(const std::function<void(const Source&)>& f) {
  for (int i = 0; i < kPadCount; ++i) f(Source{Source::Pad, i});
  for (int k = 1; k < kKeyCount; ++k)
    if (key_name(k)[0] && k != key_from_name("F1")) f(Source{Source::KeyboardKey, k});
  for (int i = 0; i < kMouseCount; ++i) f(Source{Source::Mouse, i});
}

void Hub::set_menu_open(bool open) {
  std::lock_guard<std::mutex> g(mu_);
  if (open == menu_open_.load()) return;
  menu_open_ = open;
  tog_state_[0] = tog_state_[1] = false;
  chord_fired_ = true;   // the chord that opened the menu must be released before it can fire again
  chord_since_ns_ = 0;
  if (!open) { capturing_ = false; cap_have_ = false; }
  else if (backend_) backend_->rumble(0, 0);   // Hub::rumble is ignored while the menu is open: stop what was running
}
bool Hub::consume_menu_toggle() { return toggle_req_.exchange(false); }
PadSample Hub::preview() const { std::lock_guard<std::mutex> g(mu_); return preview_; }

void Hub::capture_begin(unsigned device_mask) {
  std::lock_guard<std::mutex> g(mu_);
  cap_mask_ = device_mask;
  cap_deadline_ns_ = now_ns() + 10000000000ull;
  cap_ignore_.clear();
  cap_have_ = false;
  cap_timeout_ = false;
  each_candidate([&](const Source& s) { if (source_down(s, raw_, 0.6f)) cap_ignore_.push_back(src_key(s)); });
  capturing_ = true;
}
void Hub::capture_cancel() { std::lock_guard<std::mutex> g(mu_); capturing_ = false; cap_have_ = false; }
bool Hub::capture_result(Source* out) {
  std::lock_guard<std::mutex> g(mu_);
  if (!cap_have_) return false;
  *out = cap_src_;
  cap_have_ = false;
  return true;
}
bool Hub::capture_timed_out() { std::lock_guard<std::mutex> g(mu_); bool t = cap_timeout_; cap_timeout_ = false; return t; }

void Hub::inject_source(const Source& s, bool down) {
  std::lock_guard<std::mutex> g(mu_);
  switch (s.kind) {
    case Source::KeyboardKey: inject_.keys.set((size_t)s.id, down); break;
    case Source::Pad: inject_.pad.set((size_t)s.id, down); break;
    case Source::Mouse: if (down) inject_.mouse_buttons |= (uint8_t)(1u << s.id); else inject_.mouse_buttons &= (uint8_t) ~(1u << s.id); break;
    default: break;
  }
}

static void apply_rumble(Backend* be, float low, float high, const char* src, uint64_t buttons, bool log_it);
void Hub::poll(int min_interval_ms) {
  std::lock_guard<std::mutex> g(mu_);
  if (!backend_) backend_ = make_default_backend();
  uint64_t t = now_ns();
  if (last_poll_ns_ && t - last_poll_ns_ < (uint64_t)min_interval_ms * 1000000ull) return;
  last_poll_ns_ = t;
  const bool open = menu_open_.load();
  const bool lock = map_.lock_cursor && !open;
  const bool recenter = lock && map_.mouse_look;
  backend_->set_cursor_mode(lock, recenter);
  RawInput r;
  backend_->poll(r);
  if (g_script_gyro_on) { r.gyro_present = true; r.gyro_y = g_script_gyro_yaw; r.gyro_x = g_script_gyro_pitch; }   // scripted `gyro=` token (applied one poll late)
  if (env_fx().fake) { r.gyro_present = true; r.gyro_y = env_fx().fake_yaw; r.gyro_x = env_fx().fake_pitch; }   // SNAP_GYRO_FAKE
  cursor_hidden_ = recenter && r.focused;
  // injected sources (tests) and wheel pulses
  r.keys |= inject_.keys;
  r.pad |= inject_.pad;
  r.mouse_buttons |= inject_.mouse_buttons;
  if (inject_.pad.any()) r.pad_connected = true;
  if (int d = g_wheel_delta.exchange(0)) {
    if (d > 0) wheel_up_until_ns_ = t + 100000000ull; else wheel_down_until_ns_ = t + 100000000ull;
  }
  if (r.focused) {
    if (t < wheel_up_until_ns_) r.mouse_buttons |= 1u << kMouseWheelUp;
    if (t < wheel_down_until_ns_) r.mouse_buttons |= 1u << kMouseWheelDown;
  }
  // mouse-look velocity: pixels moved over a window of 40 ms (smoothing off) up to 340 ms (smoothing 1)
  if (r.focused && (r.mouse_dx != 0 || r.mouse_dy != 0)) msamples_.push_back({t, r.mouse_dx, r.mouse_dy});
  {
    const uint64_t win = (uint64_t)((0.04 + 0.30 * std::clamp(map_.mouse_smoothing, 0.0f, 1.0f)) * 1e9);
    while (!msamples_.empty() && t - msamples_.front().t > 400000000ull) msamples_.erase(msamples_.begin());
    double sx = 0, sy = 0;
    for (auto& m : msamples_) if (t - m.t <= win) { sx += m.dx; sy += m.dy; }
    r.mouse_vx = (float)(sx / ((double)win * 1e-9));
    r.mouse_vy = (float)(sy / ((double)win * 1e-9));
  }
  raw_ = r;
  // chord: hold Back + Start for 0.5 s toggles the menu (read from the physical pad, never remapped)
  const bool chord = r.focused && r.pad.test(kPadBack) && r.pad.test(kPadStart);
  if (!chord) { chord_since_ns_ = 0; chord_fired_ = false; }
  else if (!chord_since_ns_) chord_since_ns_ = t;
  else if (!chord_fired_ && t - chord_since_ns_ >= 500000000ull) { chord_fired_ = true; toggle_req_ = true; }
  // rebinding capture
  if (capturing_.load()) {
    for (size_t i = 0; i < cap_ignore_.size();) {   // forget held sources once released
      Source s{(Source::Kind)(cap_ignore_[i] >> 8), (int)(cap_ignore_[i] & 0xff)};
      if (!source_down(s, r, 0.6f)) cap_ignore_.erase(cap_ignore_.begin() + (long)i); else ++i;
    }
    if (!cap_have_) {
      each_candidate([&](const Source& s) {
        if (cap_have_ || !capture_allowed(s, cap_mask_) || !source_down(s, r, 0.6f)) return;
        if (std::find(cap_ignore_.begin(), cap_ignore_.end(), src_key(s)) != cap_ignore_.end()) return;
        cap_have_ = true; cap_src_ = s;
      });
      if (cap_have_) capturing_ = false;
      else if (t > cap_deadline_ns_) { capturing_ = false; cap_timeout_ = true; }
    }
  }
  PadSample p = map_pad(map_, r);
  // toggle mode: a ZL / ZR press flips the trigger instead of holding it
  const bool tg[2] = {map_.toggle_zl, map_.toggle_zr};
  const int bits[2] = {kZL, kZR};
  for (int i = 0; i < 2; ++i) {
    if (!tg[i]) { tog_state_[i] = false; tog_prev_[i] = false; continue; }
    bool down = (p.buttons >> bits[i]) & 1;
    if (down && !tog_prev_[i]) tog_state_[i] = !tog_state_[i];
    tog_prev_[i] = down;
    if (tog_state_[i]) p.buttons |= 1ull << bits[i]; else p.buttons &= ~(1ull << bits[i]);
  }
  preview_ = p;
  pad_ = p;
  touch_ = map_touch(map_, r);
  if (open) {   // the menu owns the input: the game sees a connected, idle pad and no touch
    pad_ = PadSample{};
    pad_.connected = true;
    touch_ = TouchSample{};
  }
  if (!g_script.loaded) { script_load(); g_script.t0 = t; g_script.cur_t0 = t; g_script.cur_f0 = nvn::g_presentCount; }
  if (!g_script.e.empty()) {
    script_apply(t, pad_);
    static float sr_lo = 0, sr_hi = 0;
    if (g_script_rumble_lo != sr_lo || g_script_rumble_hi != sr_hi) {   // scripted host rumble (`rumble=lo,hi` token)
      sr_lo = g_script_rumble_lo; sr_hi = g_script_rumble_hi;
      apply_rumble(backend_.get(), sr_lo, sr_hi, "script", pad_.buttons, true);
    }
  }
  if (!open) {   // host-side presses (host/mods/skips.cpp, SNAP_SKIP_CUTSCENES): OR-ed onto the pad like a script entry
    const uint64_t inj = g_inject_buttons.load(std::memory_order_relaxed);
    if (inj && std::chrono::steady_clock::now().time_since_epoch().count() < g_inject_until_ns.load(std::memory_order_relaxed)) { pad_.buttons |= inj; pad_.connected = true; }
  }
  ++seq_;
}
PadSample Hub::pad() const { std::lock_guard<std::mutex> g(mu_); return pad_; }
TouchSample Hub::touch() const { std::lock_guard<std::mutex> g(mu_); return touch_; }
int64_t Hub::sampling_number() const { std::lock_guard<std::mutex> g(mu_); return seq_; }
RawInput Hub::raw() const { std::lock_guard<std::mutex> g(mu_); return raw_; }
// Scaling, logging and the backend call; the caller holds mu_ (Hub::rumble from the game, Hub::poll for scripted `rumble=` tokens).
static void apply_rumble(Backend* be, float low, float high, const char* src, uint64_t buttons, bool log_it) {
  const PadFx& fx = pad_fx();
  const EnvFx& ev = env_fx();
  const float in_lo = low, in_hi = high;
  const float k = ev.rumble_set ? ev.rumble : std::clamp(fx.rumble_strength.load(), 0.0f, 1.5f);
  if (ev.rumble_set ? k <= 0.0f : !fx.rumble_on.load()) { low = high = 0; }
  else { low = std::min(1.0f, low * k); high = std::min(1.0f, high * k); }
  if (ev.rumble_log && log_it) {
    log("rumble: %s low=%.3f high=%.3f -> host low=%.3f high=%.3f (strength %.2f) present=%d buttons=%#llx", src, in_lo, in_hi, low, high, k, nvn::g_presentCount, (unsigned long long)buttons);
  }
  be->rumble(low, high);
}
void Hub::rumble(float low, float high) {
  std::lock_guard<std::mutex> g(mu_);
  if (!backend_ || menu_open_.load()) return;
  static float last_lo = -1, last_hi = -1;   // log only changes (the game repeats its values every frame)
  const bool changed = low != last_lo || high != last_hi;
  last_lo = low; last_hi = high;
  apply_rumble(backend_.get(), low, high, "game", pad_.buttons, changed);
}

Hub& hub() {
  static Hub h;
  return h;
}

}  // namespace hle::input
