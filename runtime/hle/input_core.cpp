// SPDX-License-Identifier: MIT
#include "input_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "hle.h"

namespace hle::input {

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
                                           "ltrigger", "rtrigger"};
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
    int id = l == "left" ? 0 : l == "right" ? 1 : l == "middle" ? 2 : -1;
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
    case Source::Mouse: return std::string("mouse:") + (s.id == 0 ? "left" : s.id == 1 ? "right" : "middle");
    default: return "";
  }
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
  o.set("up", sources_json(s.up));
  o.set("down", sources_json(s.down));
  o.set("left", sources_json(s.left));
  o.set("right", sources_json(s.right));
  return o;
}

Json Mapping::to_json() const {
  Json j = Json::object();
  j.set("version", Json::number(1));
  j.set("_comment", Json::string("Sources: pad:<south|east|west|north|back|guide|start|lstick|rstick|lshoulder|rshoulder|dpad_up|dpad_down|dpad_left|dpad_right|ltrigger|rtrigger>, key:<name>, mouse:<left|right|middle>. Switch button names: A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down."));
  j.set("trigger_threshold", Json::number(trigger_threshold));
  Json mouse = Json::object();
  mouse.set("touch", Json::boolean_(mouse_touch));
  mouse.set("look_sensitivity", Json::number(mouse_look_sensitivity));
  j.set("mouse", mouse);
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
  if (auto* v = j.get("trigger_threshold")) m.trigger_threshold = std::clamp((float)v->num(0.5), 0.05f, 1.0f);
  if (auto* v = j.get("mouse")) {
    if (auto* t = v->get("touch")) m.mouse_touch = t->boolean(true);
    if (auto* s = v->get("look_sensitivity")) m.mouse_look_sensitivity = std::max(0.0f, (float)s->num(0));
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

static bool source_down(const Source& s, const RawInput& in, float trigger_thr) {
  switch (s.kind) {
    case Source::Pad:
      if (s.id == kPadLTrigger) return in.pad.test((size_t)s.id) || in.lt >= trigger_thr;
      if (s.id == kPadRTrigger) return in.pad.test((size_t)s.id) || in.rt >= trigger_thr;
      return in.pad.test((size_t)s.id);
    case Source::KeyboardKey: return in.keys.test((size_t)s.id);
    case Source::Mouse: return (in.mouse_buttons >> s.id) & 1;
    default: return false;
  }
}
static bool any_down(const std::vector<Source>& v, const RawInput& in, float thr) {
  for (auto& s : v) if (source_down(s, in, thr)) return true;
  return false;
}

static void stick_value(const StickMap& sm, const Mapping& m, const RawInput& in, float mouse_x, float mouse_y, float* ox, float* oy) {
  float x = 0, y = 0;
  if (sm.analog != "none") {
    float ax = sm.analog == "left" ? in.lx : in.rx, ay = sm.analog == "left" ? in.ly : in.ry;
    float mag = std::sqrt(ax * ax + ay * ay);
    if (mag > sm.deadzone) {
      float scale = std::min(1.0f, (mag - sm.deadzone) / (1.0f - sm.deadzone)) / mag;  // radial deadzone, rescaled
      x = ax * scale; y = ay * scale;
    }
  }
  x += (any_down(sm.right, in, m.trigger_threshold) ? 1.f : 0.f) - (any_down(sm.left, in, m.trigger_threshold) ? 1.f : 0.f);
  y += (any_down(sm.up, in, m.trigger_threshold) ? 1.f : 0.f) - (any_down(sm.down, in, m.trigger_threshold) ? 1.f : 0.f);
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
  for (int b = 0; b <= kDown; ++b)
    if (any_down(m.buttons[(size_t)b], eff, m.trigger_threshold)) out.buttons |= 1ull << b;
  float lx, ly, rx, ry;
  stick_value(m.left, m, eff, 0, 0, &lx, &ly);
  float mlx = 0, mly = 0;
  if (m.mouse_look_sensitivity > 0 && eff.focused) {
    mlx = std::clamp(eff.mouse_dx * m.mouse_look_sensitivity, -1.0f, 1.0f);
    mly = std::clamp(-eff.mouse_dy * m.mouse_look_sensitivity, -1.0f, 1.0f);
  }
  stick_value(m.right, m, eff, mlx, mly, &rx, &ry);
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
  if (m.mouse_touch && in.focused && in.mouse_in_window && (in.mouse_buttons & 1)) {
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

bool write_default_mapping(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::string text;
  json_dump(Mapping::defaults().to_json(), text);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << text << "\n";
  return (bool)f;
}

bool Hub::load_mapping(const std::string& path, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    write_default_mapping(path);  // first run: leave an editable template
    set_mapping(Mapping::defaults());
    return true;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  std::string e;
  if (!json_parse(ss.str(), j, e)) { if (err) *err = path + ": " + e; return false; }
  Mapping m;
  if (!m.from_json(j, &e)) { if (err) *err = path + ": " + e; return false; }
  set_mapping(m);
  return true;
}

// ---- scripted input (SNAP_INPUT_SCRIPT=<file>): timed presses OR-ed onto whatever the host devices report.
// One entry per line, '#' starts a comment, times are seconds since the first poll:
//   <start> <hold> <token>...                      press the tokens from start for hold seconds
//   repeat <start> <end> <period> <hold> <token>...  the same press every period seconds in [start, end)
// A token is a Switch button name (A B X Y L R ZL ZR Plus Minus Left Up Right Down StickL StickR ...) or an axis
// lx=<f> ly=<f> rx=<f> ry=<f> with f in [-1, 1] (y up positive).
namespace {
struct ScriptEntry { double start, end, period, hold; uint64_t buttons; int32_t ax[4]; bool has_ax[4]; };
struct Script {
  bool loaded = false;
  uint64_t t0 = 0;
  std::vector<ScriptEntry> e;
};
Script g_script;

bool script_token(const std::string& t, ScriptEntry& en) {
  size_t eq = t.find('=');
  if (eq != std::string::npos) {
    std::string k = t.substr(0, eq);
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
    ScriptEntry en{};
    bool rep = w == "repeat";
    if (rep) {
      if (!(is >> en.start >> en.end >> en.period >> en.hold) || en.period <= 0) { log("input script: bad line '%s'", line.c_str()); continue; }
    } else {
      en.start = std::atof(w.c_str());
      en.end = en.start;
      en.period = 1;
      if (!(is >> en.hold)) { log("input script: bad line '%s'", line.c_str()); continue; }
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
void script_apply(double now, PadSample& p) {
  static uint64_t last = ~0ull;
  uint64_t cur = 0;
  for (const ScriptEntry& en : g_script.e) {
    bool on;
    if (en.end > en.start) {   // repeat
      on = now >= en.start && now < en.end && std::fmod(now - en.start, en.period) < en.hold;
    } else {
      on = now >= en.start && now < en.start + en.hold;
    }
    if (!on) continue;
    cur |= en.buttons;
    p.buttons |= en.buttons;
    for (int i = 0; i < 4; ++i) if (en.has_ax[i]) (&p.lx)[i] = en.ax[i];
    p.connected = true;
  }
  if (cur != last) { last = cur; log("input script: t=%.2f buttons=%#llx", now, (unsigned long long)cur); }
}
}  // namespace

void Hub::poll(int min_interval_ms) {
  std::lock_guard<std::mutex> g(mu_);
  if (!backend_) backend_ = make_default_backend();
  uint64_t t = now_ns();
  if (last_poll_ns_ && t - last_poll_ns_ < (uint64_t)min_interval_ms * 1000000ull) return;
  last_poll_ns_ = t;
  RawInput r;
  backend_->poll(r);
  raw_ = r;
  pad_ = map_pad(map_, r);
  if (!g_script.loaded) { script_load(); g_script.t0 = t; }
  if (!g_script.e.empty()) script_apply((double)(t - g_script.t0) * 1e-9, pad_);
  touch_ = map_touch(map_, r);
  ++seq_;
}
PadSample Hub::pad() const { std::lock_guard<std::mutex> g(mu_); return pad_; }
TouchSample Hub::touch() const { std::lock_guard<std::mutex> g(mu_); return touch_; }
int64_t Hub::sampling_number() const { std::lock_guard<std::mutex> g(mu_); return seq_; }
RawInput Hub::raw() const { std::lock_guard<std::mutex> g(mu_); return raw_; }
void Hub::rumble(float low, float high) {
  std::lock_guard<std::mutex> g(mu_);
  if (backend_) backend_->rumble(low, high);
}

Hub& hub() {
  static Hub h;
  return h;
}

}  // namespace hle::input
