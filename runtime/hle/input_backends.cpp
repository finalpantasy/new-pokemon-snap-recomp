// SPDX-License-Identifier: MIT
// Input backends. Win32: XInput (loaded dynamically, no SDK dependency), GetAsyncKeyState, GetCursorPos.
// SDL3 (zlib license; build with -DHLE_WITH_SDL3 and the official SDL3 headers/lib): gamepads through the SDL gamepad
// API (any controller SDL knows), keyboard and mouse from SDL's state, which the window owner keeps fresh by pumping
// events. make_default_backend picks SDL3 when built with it, otherwise Win32.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <cmath>
#include <cstdlib>

#include "hle.h"
#include "input_core.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef HLE_WITH_SDL3
#include <SDL3/SDL.h>
#endif

namespace hle::input {

#ifdef _WIN32
namespace {

struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XVibration { WORD left, right; };

// Windows virtual-key code for each Key id.
int vk_for_key(int k) {
  if (k >= 1 && k <= 26) return 'A' + (k - 1);
  if (k >= 27 && k <= 36) return '0' + (k - 27);
  if (k >= 37 && k <= 48) return VK_F1 + (k - 37);
  static const int fixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_SPACE, VK_RETURN, VK_ESCAPE, VK_TAB, VK_BACK,
                              VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_OEM_COMMA,
                              VK_OEM_PERIOD, VK_OEM_2, VK_OEM_1, VK_OEM_7, VK_OEM_4, VK_OEM_6, VK_OEM_MINUS,
                              VK_OEM_PLUS, VK_OEM_5, VK_OEM_3, VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT};
  if (k >= 49 && k < 49 + (int)(sizeof fixed / sizeof fixed[0])) return fixed[k - 49];
  int np = 49 + (int)(sizeof fixed / sizeof fixed[0]);
  if (k >= np && k < np + 10) return VK_NUMPAD0 + (k - np);
  return 0;
}

// Folds the pad fields of `src` into `dst`: buttons OR-ed, the larger stick or trigger value wins. Several devices (an XInput
// interface and a HID interface of the same physical pad, or two pads) therefore act as one npad 0 and a press is never counted twice.
void merge_pad(RawInput& dst, const RawInput& src) {
  if (!src.pad_connected) return;
  dst.pad_connected = true;
  dst.pad |= src.pad;
  auto big = [](float& d, float s) { if (std::fabs(s) > std::fabs(d)) d = s; };
  big(dst.lx, src.lx); big(dst.ly, src.ly); big(dst.rx, src.rx); big(dst.ry, src.ry);
  dst.lt = std::max(dst.lt, src.lt); dst.rt = std::max(dst.rt, src.rt);
  if (src.gyro_present) { dst.gyro_present = true; dst.gyro_x = src.gyro_x; dst.gyro_y = src.gyro_y; dst.gyro_z = src.gyro_z; }
}

#ifdef HLE_WITH_SDL3
// Gamepads through SDL3 (PlayStation, Switch Pro, Xbox, generic HID) for the Win32 backend, which keeps keyboard and mouse
// (the game window is a plain HWND, so SDL's keyboard state has no focus window to read). Every gamepad SDL knows is opened and
// merged; the joystick list is rescanned once a second, so a pad that is plugged in or wakes up later is picked up (SDL events are
// never pumped here). Each enumerated device is printed to stdout and logged.
struct SdlPads {
  bool inited = false;
  uint64_t last_scan_ns = 0;
  std::vector<SDL_Gamepad*> pads;
  std::vector<SDL_JoystickID> seen;   // joystick ids already reported

  static const char* type_name(SDL_GamepadType t) {
    switch (t) {
      case SDL_GAMEPAD_TYPE_XBOX360: return "xbox360"; case SDL_GAMEPAD_TYPE_XBOXONE: return "xboxone";
      case SDL_GAMEPAD_TYPE_PS3: return "ps3"; case SDL_GAMEPAD_TYPE_PS4: return "ps4"; case SDL_GAMEPAD_TYPE_PS5: return "ps5";
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "switch-pro"; case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
      default: return "other";
    }
  }
  void say(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char b[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    std::fprintf(stdout, "pad: %s\n", b); std::fflush(stdout);
    log("pad: %s", b);
  }
  void init() {
    if (inited) return;
    inited = true;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // The RAWINPUT driver delivers through window messages that nobody pumps here; WGI, XInput, DirectInput and HIDAPI poll.
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) say("sdl3 init failed: %s", SDL_GetError());
  }
  void scan() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    // drop closed or unplugged pads
    for (size_t i = 0; i < pads.size();) {
      if (!SDL_GamepadConnected(pads[i])) {
        say("sdl gamepad removed: '%s'", SDL_GetGamepadName(pads[i]));
        SDL_CloseGamepad(pads[i]);
        pads.erase(pads.begin() + (long)i);
      } else ++i;
    }
    for (int k = 0; k < n; ++k) {
      SDL_JoystickID id = ids[k];
      bool open = false;
      for (SDL_Gamepad* g : pads) if (SDL_GetGamepadID(g) == id) open = true;
      if (open) continue;
      bool is_pad = SDL_IsGamepad(id);
      bool first = std::find(seen.begin(), seen.end(), id) == seen.end();
      if (first) {
        seen.push_back(id);
        char* map = is_pad ? SDL_GetGamepadMappingForID(id) : nullptr;
        say("sdl device %u: '%s' vid:pid=%04x:%04x gamepad=%s type=%s mapping=%s", (unsigned)id,
            SDL_GetJoystickNameForID(id) ? SDL_GetJoystickNameForID(id) : "?", SDL_GetJoystickVendorForID(id),
            SDL_GetJoystickProductForID(id), is_pad ? "yes" : "no", is_pad ? type_name(SDL_GetGamepadTypeForID(id)) : "-",
            map ? "yes" : "no");
        SDL_free(map);
      }
      if (!is_pad) continue;
      if (SDL_Gamepad* g = SDL_OpenGamepad(id)) {
        pads.push_back(g);
        say("sdl gamepad opened: '%s' gyro=%s rumble=%s", SDL_GetGamepadName(g), SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) ? "yes" : "no",
            SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no");
      }
      else say("sdl gamepad open failed (id %u): %s", (unsigned)id, SDL_GetError());
    }
    // forget ids that disappeared so a re-plug is reported again
    seen.erase(std::remove_if(seen.begin(), seen.end(), [&](SDL_JoystickID id) {
      for (int k = 0; k < n; ++k) if (ids && ids[k] == id) return false;
      return true; }), seen.end());
    SDL_free(ids);
  }
  // Merges every open gamepad into `out`; false when none is open.
  bool poll(RawInput& out) {
    init();
    uint64_t t = (uint64_t)SDL_GetTicksNS();
    if (!last_scan_ns || t - last_scan_ns > 1000000000ull) { last_scan_ns = t; scan(); }
    SDL_UpdateGamepads();
    for (SDL_Gamepad* pad : pads) {
      RawInput r;
      r.pad_connected = true;
      auto b = [&](SDL_GamepadButton sb, int bit) { if (SDL_GetGamepadButton(pad, sb)) r.pad.set((size_t)bit); };
      b(SDL_GAMEPAD_BUTTON_SOUTH, kPadSouth); b(SDL_GAMEPAD_BUTTON_EAST, kPadEast);
      b(SDL_GAMEPAD_BUTTON_WEST, kPadWest); b(SDL_GAMEPAD_BUTTON_NORTH, kPadNorth);
      b(SDL_GAMEPAD_BUTTON_BACK, kPadBack); b(SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide); b(SDL_GAMEPAD_BUTTON_START, kPadStart);
      b(SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLStick); b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRStick);
      b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLShoulder); b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRShoulder);
      b(SDL_GAMEPAD_BUTTON_DPAD_UP, kPadDUp); b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDDown);
      b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadDLeft); b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadDRight);
      auto ax = [&](SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(pad, a) / 32767.0f, -1.0f, 1.0f); };
      r.lx = ax(SDL_GAMEPAD_AXIS_LEFTX); r.ly = -ax(SDL_GAMEPAD_AXIS_LEFTY);  // SDL y is down positive
      r.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX); r.ry = -ax(SDL_GAMEPAD_AXIS_RIGHTY);
      r.lt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); r.rt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
      if (pad_fx().gyro_on.load() && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        float g[3] = {0, 0, 0};
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3)) {   // rad/s
          constexpr float kDeg = 57.29578f;
          r.gyro_present = true;
          r.gyro_x = g[0] * kDeg; r.gyro_y = g[1] * kDeg; r.gyro_z = g[2] * kDeg;
        }
      }
      merge_pad(out, r);
    }
    return !pads.empty();
  }
  // Rumble on every open pad that supports it (DualSense, Xbox, Switch Pro ...). True when at least one pad accepted it.
  bool rumble(float low, float high) {
    bool any = false;
    const Uint16 lo = (Uint16)(std::clamp(low, 0.0f, 1.0f) * 65535), hi = (Uint16)(std::clamp(high, 0.0f, 1.0f) * 65535);
    for (SDL_Gamepad* pad : pads) {
      if (!SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) continue;
      if (SDL_RumbleGamepad(pad, lo, hi, (lo || hi) ? 500 : 0)) any = true;   // 500 ms: the game repeats while it wants rumble
    }
    if (rumble_log_enabled()) hle::log("rumble: SDL_RumbleGamepad lo=%u hi=%u pads=%d accepted=%d", (unsigned)lo, (unsigned)hi, (int)pads.size(), (int)any);
    return any;
  }
};
#endif

class Win32Backend final : public Backend {
 public:
  Win32Backend() {
    xinput_ = LoadLibraryA("xinput1_4.dll");
    if (!xinput_) xinput_ = LoadLibraryA("xinput1_3.dll");
    if (xinput_) {
      get_ = reinterpret_cast<GetFn>(GetProcAddress(xinput_, "XInputGetState"));
      set_ = reinterpret_cast<SetFn>(GetProcAddress(xinput_, "XInputSetState"));
    }
    const char* bg = std::getenv("SNAP_INPUT_BACKGROUND");
    background_ = bg && *bg && *bg != '0';
  }
  ~Win32Backend() override {
    if (clipped_) ClipCursor(nullptr);
    if (set_ && rumbling_) { XVibration v{0, 0}; set_(pad_index_, &v); }
    if (xinput_) FreeLibrary(xinput_);
  }
  const char* name() const override { return "win32"; }
  void set_cursor_mode(bool lock, bool recenter) override { lock_ = lock; recenter_ = recenter; }

  void poll(RawInput& out) override {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool fg_ours = fg && pid == GetCurrentProcessId();   // the cursor policy only ever acts on our own foreground window
    out.focused = background_ || fg_ours;
    // Pads: every SDL gamepad and every XInput slot, merged. XInput is always read as well (not only as a fallback), so a pad SDL
    // lists as an unmapped joystick or fails to open still works through its XInput interface.
#ifdef HLE_WITH_SDL3
    sdl_.poll(out);
#endif
    if (get_) {
      for (DWORD i = 0; i < 4; ++i) {
        XState s{};
        bool ok = get_(i, &s) == 0;  // ERROR_SUCCESS
        if (ok != xslot_[i]) {
          xslot_[i] = ok;
          std::fprintf(stdout, "pad: xinput slot %u %s\n", (unsigned)i, ok ? "connected" : "disconnected");
          std::fflush(stdout);
          log("pad: xinput slot %u %s", (unsigned)i, ok ? "connected" : "disconnected");
        }
        if (!ok) continue;
        if (pad_index_ == 0 || !xslot_[pad_index_]) pad_index_ = i;
        RawInput r;
        r.pad_connected = true;
        WORD b = s.buttons;
        auto set = [&](int bit, bool v) { if (v) r.pad.set((size_t)bit); };
        set(kPadSouth, b & 0x1000); set(kPadEast, b & 0x2000); set(kPadWest, b & 0x4000); set(kPadNorth, b & 0x8000);
        set(kPadBack, b & 0x20); set(kPadStart, b & 0x10); set(kPadLStick, b & 0x40); set(kPadRStick, b & 0x80);
        set(kPadLShoulder, b & 0x100); set(kPadRShoulder, b & 0x200);
        set(kPadDUp, b & 0x1); set(kPadDDown, b & 0x2); set(kPadDLeft, b & 0x4); set(kPadDRight, b & 0x8);
        auto ax = [](SHORT v) { return std::clamp(v / 32767.0f, -1.0f, 1.0f); };
        r.lx = ax(s.lx); r.ly = ax(s.ly); r.rx = ax(s.rx); r.ry = ax(s.ry);
        r.lt = s.lt / 255.0f; r.rt = s.rt / 255.0f;
        merge_pad(out, r);
      }
    }
    if (!out.focused) { out.pad.reset(); out.lx = out.ly = out.rx = out.ry = out.lt = out.rt = 0; }
    if (out.focused) {
      for (int k = 1; k < kKeyCount; ++k) {
        int vk = vk_for_key(k);
        if (vk && (GetAsyncKeyState(vk) & 0x8000)) out.keys.set((size_t)k);
      }
      POINT p;
      if (GetCursorPos(&p) && fg) {
        POINT c = p;
        RECT rc;
        if (ScreenToClient(fg, &c) && GetClientRect(fg, &rc) && rc.right > 0 && rc.bottom > 0) {
          out.mouse_in_window = c.x >= 0 && c.y >= 0 && c.x < rc.right && c.y < rc.bottom;
          out.mouse_x = (float)c.x / (float)rc.right;
          out.mouse_y = (float)c.y / (float)rc.bottom;
          // cursor policy: confine to the client area while locked; mouse-look re-centers it after every read
          POINT tl{0, 0}, br{rc.right, rc.bottom};
          ClientToScreen(fg, &tl); ClientToScreen(fg, &br);
          if (lock_ && fg_ours) {
            RECT clip{tl.x, tl.y, br.x, br.y};
            if (!clipped_ || memcmp(&clip, &clip_, sizeof clip) != 0) { ClipCursor(&clip); clip_ = clip; clipped_ = true; }
          } else if (clipped_) { ClipCursor(nullptr); clipped_ = false; }
          if (recenter_ && fg_ours) {
            POINT mid{(tl.x + br.x) / 2, (tl.y + br.y) / 2};
            if (have_last_ && was_recenter_) { out.mouse_dx = (float)(p.x - mid.x); out.mouse_dy = (float)(p.y - mid.y); }
            SetCursorPos(mid.x, mid.y);
            last_ = mid;
            have_last_ = true;
            was_recenter_ = true;
          } else {
            was_recenter_ = false;
            if (have_last_) { out.mouse_dx = (float)(p.x - last_.x); out.mouse_dy = (float)(p.y - last_.y); }
            last_ = p;
            have_last_ = true;
          }
        }
      }
      if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) out.mouse_buttons |= 1;
      if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) out.mouse_buttons |= 2;
      if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) out.mouse_buttons |= 4;
      if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) out.mouse_buttons |= 8;
      if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) out.mouse_buttons |= 16;
    } else {
      have_last_ = false;
      if (clipped_) { ClipCursor(nullptr); clipped_ = false; }   // window lost the focus: free the cursor
    }
  }

  void rumble(float low, float high) override {
#ifdef HLE_WITH_SDL3
    if (sdl_.rumble(low, high)) { if (set_ && rumbling_) { XVibration z{0, 0}; set_(pad_index_, &z); rumbling_ = false; } return; }   // SDL drives it; no double rumble through XInput
#endif
    if (!set_ || !out_connected()) return;
    XVibration v{(WORD)(std::clamp(low, 0.0f, 1.0f) * 65535), (WORD)(std::clamp(high, 0.0f, 1.0f) * 65535)};
    if (rumble_log_enabled()) hle::log("rumble: XInput pad %d left=%u right=%u", (int)pad_index_, (unsigned)v.left, (unsigned)v.right);
    set_(pad_index_, &v);
    rumbling_ = v.left || v.right;
  }

 private:
  using GetFn = DWORD(WINAPI*)(DWORD, XState*);
  using SetFn = DWORD(WINAPI*)(DWORD, XVibration*);
  bool out_connected() const { return set_ != nullptr; }
  HMODULE xinput_ = nullptr;
  GetFn get_ = nullptr;
  SetFn set_ = nullptr;
  DWORD pad_index_ = 0;
  bool xslot_[4] = {false, false, false, false};
  bool rumbling_ = false, background_ = false, have_last_ = false;
  bool lock_ = false, recenter_ = false, clipped_ = false, was_recenter_ = false;
  RECT clip_{};
#ifdef HLE_WITH_SDL3
  SdlPads sdl_;
#endif
  POINT last_{};
};

}  // namespace
#endif

#ifdef HLE_WITH_SDL3
namespace {

class Sdl3Backend final : public Backend {
 public:
  Sdl3Backend() {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log("sdl3: gamepad init failed: %s", SDL_GetError());
  }
  ~Sdl3Backend() override {
    if (pad_) SDL_CloseGamepad(pad_);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  const char* name() const override { return "sdl3"; }

  void poll(RawInput& out) override {
    SDL_UpdateGamepads();
    if (!pad_ || !SDL_GamepadConnected(pad_)) {
      if (pad_) { SDL_CloseGamepad(pad_); pad_ = nullptr; }
      int n = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&n);
      if (ids && n > 0) pad_ = SDL_OpenGamepad(ids[0]);
      SDL_free(ids);
    }
    out.focused = SDL_GetKeyboardFocus() != nullptr;
    if (pad_) {
      out.pad_connected = true;
      auto b = [&](SDL_GamepadButton sb, int bit) { if (SDL_GetGamepadButton(pad_, sb)) out.pad.set((size_t)bit); };
      b(SDL_GAMEPAD_BUTTON_SOUTH, kPadSouth); b(SDL_GAMEPAD_BUTTON_EAST, kPadEast);
      b(SDL_GAMEPAD_BUTTON_WEST, kPadWest); b(SDL_GAMEPAD_BUTTON_NORTH, kPadNorth);
      b(SDL_GAMEPAD_BUTTON_BACK, kPadBack); b(SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide); b(SDL_GAMEPAD_BUTTON_START, kPadStart);
      b(SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLStick); b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRStick);
      b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLShoulder); b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRShoulder);
      b(SDL_GAMEPAD_BUTTON_DPAD_UP, kPadDUp); b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDDown);
      b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadDLeft); b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadDRight);
      auto ax = [&](SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(pad_, a) / 32767.0f, -1.0f, 1.0f); };
      out.lx = ax(SDL_GAMEPAD_AXIS_LEFTX); out.ly = -ax(SDL_GAMEPAD_AXIS_LEFTY);  // SDL y is down positive
      out.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX); out.ry = -ax(SDL_GAMEPAD_AXIS_RIGHTY);
      out.lt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); out.rt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    }
    if (out.focused) {
      int n = 0;
      const bool* ks = SDL_GetKeyboardState(&n);
      auto key = [&](int id, SDL_Scancode sc) { if (ks && sc < n && ks[sc]) out.keys.set((size_t)id); };
      for (int k = 1; k <= 26; ++k) key(k, (SDL_Scancode)(SDL_SCANCODE_A + (k - 1)));
      key(27, SDL_SCANCODE_0);
      for (int k = 28; k <= 36; ++k) key(k, (SDL_Scancode)(SDL_SCANCODE_1 + (k - 28)));
      for (int k = 37; k <= 48; ++k) key(k, (SDL_Scancode)(SDL_SCANCODE_F1 + (k - 37)));
      static const SDL_Scancode fixed[] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
          SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_BACKSPACE,
          SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_LALT,
          SDL_SCANCODE_RALT, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_SEMICOLON,
          SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_MINUS,
          SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_GRAVE, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,
          SDL_SCANCODE_HOME, SDL_SCANCODE_END, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN};
      constexpr int nf = (int)(sizeof fixed / sizeof fixed[0]);
      for (int i = 0; i < nf; ++i) key(49 + i, fixed[i]);
      key(49 + nf, SDL_SCANCODE_KP_0);
      for (int i = 1; i <= 9; ++i) key(49 + nf + i, (SDL_Scancode)(SDL_SCANCODE_KP_1 + (i - 1)));
      float mx = 0, my = 0;
      SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
      if (mb & SDL_BUTTON_LMASK) out.mouse_buttons |= 1;
      if (mb & SDL_BUTTON_RMASK) out.mouse_buttons |= 2;
      if (mb & SDL_BUTTON_MMASK) out.mouse_buttons |= 4;
      SDL_Window* w = SDL_GetMouseFocus();
      int ww = 0, wh = 0;
      if (w && SDL_GetWindowSize(w, &ww, &wh) && ww > 0 && wh > 0) {
        out.mouse_in_window = true;
        out.mouse_x = mx / (float)ww;
        out.mouse_y = my / (float)wh;
      }
      float dx = 0, dy = 0;
      SDL_GetRelativeMouseState(&dx, &dy);
      out.mouse_dx = dx; out.mouse_dy = dy;
    }
  }

  void rumble(float low, float high) override {
    if (pad_) SDL_RumbleGamepad(pad_, (Uint16)(std::clamp(low, 0.0f, 1.0f) * 65535), (Uint16)(std::clamp(high, 0.0f, 1.0f) * 65535), 100);
  }

 private:
  SDL_Gamepad* pad_ = nullptr;
};

}  // namespace
#endif

// Diagnostics for `snap_boot pads [seconds]` (host\play.ps1 -ListPads): enumerates every device (SDL joysticks and XInput slots, printed to
// stdout) and then, for `seconds`, prints the merged npad state whenever it changes, so a pad can be checked by pressing its buttons.
void list_pads(int seconds) {
#ifdef _WIN32
  _putenv_s("SNAP_INPUT_BACKGROUND", "1");   // this console is not the game window
  Win32Backend b;
  std::fprintf(stdout, "pad: backend %s, SDL3 %s; monitoring %d s (press buttons)\n", b.name(),
#ifdef HLE_WITH_SDL3
               "built in",
#else
               "not built in (XInput only)",
#endif
               seconds);
  std::fflush(stdout);
  uint64_t last_buttons = ~0ull;
  int32_t last_axes = 0;
  ULONGLONG t0 = GetTickCount64();
  while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000) {
    RawInput r;
    b.poll(r);
    int32_t axes = (int32_t)(r.lx * 10) * 1000003 + (int32_t)(r.ly * 10) * 10007 + (int32_t)(r.rx * 10) * 101 + (int32_t)(r.ry * 10) + (int32_t)(r.lt * 4) * 7 + (int32_t)(r.rt * 4) * 13;
    uint64_t btn = r.pad.to_ullong();
    if (btn != last_buttons || axes != last_axes) {
      last_buttons = btn; last_axes = axes;
      std::fprintf(stdout, "pad: connected=%d buttons=%#llx L=(%.2f,%.2f) R=(%.2f,%.2f) LT=%.2f RT=%.2f\n", (int)r.pad_connected,
                   (unsigned long long)btn, r.lx, r.ly, r.rx, r.ry, r.lt, r.rt);
      std::fflush(stdout);
    }
    Sleep(15);
  }
#else
  (void)seconds;
#endif
}

std::unique_ptr<Backend> make_default_backend() {
#ifdef _WIN32
  return std::make_unique<Win32Backend>();   // with HLE_WITH_SDL3 it reads gamepads through SDL3
#elif defined(HLE_WITH_SDL3)
  return std::make_unique<Sdl3Backend>();
#else
  struct Null final : Backend {
    const char* name() const override { return "none"; }
    void poll(RawInput&) override {}
  };
  return std::make_unique<Null>();
#endif
}

}  // namespace hle::input
