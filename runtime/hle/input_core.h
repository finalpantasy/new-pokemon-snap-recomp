// SPDX-License-Identifier: MIT
// Input pipeline for nn::hid: backend (gamepad / keyboard / mouse) -> RawInput -> remapping table -> Npad sample.
// Pure host code with no guest types so the tests can drive it with a fake backend.
//
// Remapping table: JSON, default %APPDATA%\SnapRecomp\config\input_map.json (written on first run by
// write_default_mapping). The settings UI will edit the same file later; the schema is documented in
// docs/hle-services.md and kept in Mapping::to_json.
#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json_lite.h"

namespace hle::input {

// ---- Switch side: nn::hid::NpadButton bit numbers (SDK order)
enum Button : int {
  kA = 0, kB, kX, kY, kStickL, kStickR, kL, kR, kZL, kZR, kPlus, kMinus, kLeft, kUp, kRight, kDown,
  kStickLLeft, kStickLUp, kStickLRight, kStickLDown, kStickRLeft, kStickRUp, kStickRRight, kStickRDown,
  kLeftSL, kLeftSR, kRightSL, kRightSR, kButtonCount
};
const char* button_name(int b);  // "A", "B", ... "ZL"
int button_from_name(const std::string& n);  // -1 if unknown
constexpr int kStickMax = 32767;

// ---- host side
// Gamepad buttons are named by position (SDL3 convention), so one table serves XInput and SDL: south = Xbox A.
enum PadButton : int {
  kPadSouth = 0, kPadEast, kPadWest, kPadNorth, kPadBack, kPadGuide, kPadStart, kPadLStick, kPadRStick,
  kPadLShoulder, kPadRShoulder, kPadDUp, kPadDDown, kPadDLeft, kPadDRight, kPadLTrigger, kPadRTrigger, kPadCount
};
const char* pad_button_name(int b);
int pad_button_from_name(const std::string& n);

// Keyboard keys by name (letters, digits, F1-F12, arrows, ...). Backends translate host key codes to these ids.
enum Key : int { kKeyNone = 0, kKeyCount = 128 };
const char* key_name(int k);
int key_from_name(const std::string& n);  // case-insensitive; 0 if unknown
int key_from_ascii(char ch);              // 'A'..'Z', '0'..'9'

struct RawInput {
  bool focused = true;                 // the game window has the keyboard focus
  bool pad_connected = false;
  std::bitset<32> pad;                 // PadButton bits (triggers are also given as axes)
  float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up positive
  float lt = 0, rt = 0;                // 0..1
  std::bitset<kKeyCount> keys;
  bool mouse_in_window = false;
  float mouse_x = 0, mouse_y = 0;      // 0..1 over the window client area
  uint8_t mouse_buttons = 0;           // bit0 left, bit1 right, bit2 middle
  float mouse_dx = 0, mouse_dy = 0;    // pixels moved since the previous poll
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual const char* name() const = 0;
  virtual void poll(RawInput& out) = 0;
  virtual void rumble(float low, float high) { (void)low; (void)high; }  // 0..1 each
};
std::unique_ptr<Backend> make_default_backend();  // SDL3 if built with HLE_WITH_SDL3, else Win32 XInput/keyboard

// ---- mapping
struct Source {
  enum Kind { None, Pad, KeyboardKey, Mouse } kind = None;
  int id = 0;  // PadButton / Key / mouse button bit
};
bool parse_source(const std::string& s, Source* out);
std::string source_to_string(const Source& s);

struct StickMap {
  std::string analog = "left";                      // which physical stick drives it: "left", "right" or "none"
  std::vector<Source> up, down, left, right;        // digital sources (keyboard, d-pad ...)
  float deadzone = 0.15f;                           // radial, applied to the analog source
};

struct Mapping {
  std::array<std::vector<Source>, kButtonCount> buttons;
  StickMap left, right;
  float trigger_threshold = 0.5f;                   // LT/RT analog value that counts as pressed
  float stick_button_threshold = 0.5f;              // virtual StickLLeft..StickRDown bits
  bool mouse_touch = true;                          // left mouse button drives the touch screen
  float mouse_look_sensitivity = 0.0f;              // >0: mouse movement drives the right stick (pixels per full deflection = 1/this)
  static Mapping defaults();
  Json to_json() const;
  bool from_json(const Json& j, std::string* err);  // starts from defaults(): keys not present keep their default
};

struct PadSample {
  uint64_t buttons = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;           // +-32767, y up positive
  bool connected = false;
};
struct TouchPoint { int32_t x, y; };                // 1280x720 panel coordinates
struct TouchSample { int count = 0; TouchPoint pt[1]; };

PadSample map_pad(const Mapping& m, const RawInput& in);
TouchSample map_touch(const Mapping& m, const RawInput& in);

// ---- the hub the nn::hid shims read
class Hub {
 public:
  Hub();
  void set_backend(std::unique_ptr<Backend> b);
  void set_mapping(const Mapping& m);
  Mapping mapping() const;
  // Reads the JSON table; on a missing file writes the defaults there. Returns false (and keeps the old table) if invalid.
  bool load_mapping(const std::string& path, std::string* err);
  // Polls the backend if the last poll is older than `min_interval_ms` and returns the newest samples.
  void poll(int min_interval_ms = 1);
  PadSample pad() const;
  TouchSample touch() const;
  int64_t sampling_number() const;
  void rumble(float low, float high);
  RawInput raw() const;
 private:
  mutable std::mutex mu_;
  std::unique_ptr<Backend> backend_;
  Mapping map_;
  RawInput raw_;
  PadSample pad_;
  TouchSample touch_;
  int64_t seq_ = 0;
  uint64_t last_poll_ns_ = 0;
};
Hub& hub();
// Raises a game event for SNAP_INPUT_SCRIPT "anchor" lines (no effect without a script).
void script_event(const char* name);
// Prints every pad device and then the live pad state for `seconds` (snap_boot pads).
void list_pads(int seconds);

// Writes the default mapping JSON (used on first run and by the tests); returns false on I/O error.
bool write_default_mapping(const std::string& path);

}  // namespace hle::input
