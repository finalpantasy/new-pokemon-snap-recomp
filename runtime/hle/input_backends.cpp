// SPDX-License-Identifier: MIT
// Input backends. Win32: XInput (loaded dynamically, no SDK dependency), GetAsyncKeyState, GetCursorPos.
// SDL3 (zlib license; build with -DHLE_WITH_SDL3 and the official SDL3 headers/lib): gamepads through the SDL gamepad
// API (any controller SDL knows), keyboard and mouse from SDL's state, which the window owner keeps fresh by pumping
// events. make_default_backend picks SDL3 when built with it, otherwise Win32.
#include <algorithm>
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

#ifdef HLE_WITH_SDL3
// Gamepads through SDL3 (PlayStation, Switch Pro, Xbox, generic HID) for the Win32 backend, which keeps keyboard and mouse
// (the game window is a plain HWND, so SDL's keyboard state has no focus window to read). Works while the window is in the
// background; the Win32 backend zeroes the pad when the game is not the foreground window unless SNAP_INPUT_BACKGROUND is set.
struct SdlPads {
  bool inited = false;
  SDL_Gamepad* pad = nullptr;
  bool poll(RawInput& out) {
    if (!inited) {
      inited = true;
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log("sdl3: gamepad init failed: %s", SDL_GetError());
    }
    SDL_UpdateGamepads();
    if (!pad || !SDL_GamepadConnected(pad)) {
      if (pad) { SDL_CloseGamepad(pad); pad = nullptr; }
      int n = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&n);
      if (ids && n > 0) { pad = SDL_OpenGamepad(ids[0]); if (pad) log("sdl3: gamepad '%s'", SDL_GetGamepadName(pad)); }
      SDL_free(ids);
    }
    if (!pad) return false;
    out.pad_connected = true;
    auto b = [&](SDL_GamepadButton sb, int bit) { if (SDL_GetGamepadButton(pad, sb)) out.pad.set((size_t)bit); };
    b(SDL_GAMEPAD_BUTTON_SOUTH, kPadSouth); b(SDL_GAMEPAD_BUTTON_EAST, kPadEast);
    b(SDL_GAMEPAD_BUTTON_WEST, kPadWest); b(SDL_GAMEPAD_BUTTON_NORTH, kPadNorth);
    b(SDL_GAMEPAD_BUTTON_BACK, kPadBack); b(SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide); b(SDL_GAMEPAD_BUTTON_START, kPadStart);
    b(SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLStick); b(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRStick);
    b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLShoulder); b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRShoulder);
    b(SDL_GAMEPAD_BUTTON_DPAD_UP, kPadDUp); b(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDDown);
    b(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadDLeft); b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadDRight);
    auto ax = [&](SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(pad, a) / 32767.0f, -1.0f, 1.0f); };
    out.lx = ax(SDL_GAMEPAD_AXIS_LEFTX); out.ly = -ax(SDL_GAMEPAD_AXIS_LEFTY);  // SDL y is down positive
    out.rx = ax(SDL_GAMEPAD_AXIS_RIGHTX); out.ry = -ax(SDL_GAMEPAD_AXIS_RIGHTY);
    out.lt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); out.rt = std::max(0.0f, ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    return true;
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
    if (set_ && rumbling_) { XVibration v{0, 0}; set_(pad_index_, &v); }
    if (xinput_) FreeLibrary(xinput_);
  }
  const char* name() const override { return "win32"; }

  void poll(RawInput& out) override {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    out.focused = background_ || (fg && pid == GetCurrentProcessId());
    bool have_sdl = false;
#ifdef HLE_WITH_SDL3
    have_sdl = sdl_.poll(out);
    if (have_sdl && !out.focused) { out.pad.reset(); out.lx = out.ly = out.rx = out.ry = out.lt = out.rt = 0; }
#endif
    if (get_ && !have_sdl) {
      for (DWORD i = 0; i < 4; ++i) {
        XState s{};
        if (get_(i, &s) == 0) {  // ERROR_SUCCESS
          pad_index_ = i;
          out.pad_connected = true;
          WORD b = s.buttons;
          auto set = [&](int bit, bool v) { if (v) out.pad.set((size_t)bit); };
          set(kPadSouth, b & 0x1000); set(kPadEast, b & 0x2000); set(kPadWest, b & 0x4000); set(kPadNorth, b & 0x8000);
          set(kPadBack, b & 0x20); set(kPadStart, b & 0x10); set(kPadLStick, b & 0x40); set(kPadRStick, b & 0x80);
          set(kPadLShoulder, b & 0x100); set(kPadRShoulder, b & 0x200);
          set(kPadDUp, b & 0x1); set(kPadDDown, b & 0x2); set(kPadDLeft, b & 0x4); set(kPadDRight, b & 0x8);
          auto ax = [](SHORT v) { return std::clamp(v / 32767.0f, -1.0f, 1.0f); };
          out.lx = ax(s.lx); out.ly = ax(s.ly); out.rx = ax(s.rx); out.ry = ax(s.ry);
          out.lt = s.lt / 255.0f; out.rt = s.rt / 255.0f;
          break;
        }
      }
      if (!out.focused) { out.pad.reset(); out.lx = out.ly = out.rx = out.ry = out.lt = out.rt = 0; }
    }
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
        }
        if (have_last_) { out.mouse_dx = (float)(p.x - last_.x); out.mouse_dy = (float)(p.y - last_.y); }
        last_ = p;
        have_last_ = true;
      }
      if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) out.mouse_buttons |= 1;
      if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) out.mouse_buttons |= 2;
      if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) out.mouse_buttons |= 4;
    } else {
      have_last_ = false;
    }
  }

  void rumble(float low, float high) override {
    if (!set_ || !out_connected()) return;
    XVibration v{(WORD)(std::clamp(low, 0.0f, 1.0f) * 65535), (WORD)(std::clamp(high, 0.0f, 1.0f) * 65535)};
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
  bool rumbling_ = false, background_ = false, have_last_ = false;
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
