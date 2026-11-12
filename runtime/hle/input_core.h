// SPDX-License-Identifier: MIT
// Input pipeline for nn::hid: backend (gamepad / keyboard / mouse) -> RawInput -> remapping table -> Npad sample.
// Pure host code with no guest types so the tests can drive it with a fake backend.
//
// Remapping table: JSON (format version 2), default %APPDATA%\SnapRecomp\input.json, folder overridable with SNAP_CONFIG_DIR.
// The in-game controls menu (runtime/ui) edits it live through Hub::set_mapping and saves it atomically (Hub::save).
// The old config\input_map.json (version 1) is migrated on first load. The schema is documented in docs/hle-services.md and
// kept in Mapping::to_json.
#pragma once
#include <array>
#include <atomic>
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
  kPadLShoulder, kPadRShoulder, kPadDUp, kPadDDown, kPadDLeft, kPadDRight, kPadLTrigger, kPadRTrigger,
  // an analog stick pushed past half-way, as a button (so a stick direction can be bound to any Switch button)
  kPadLStickLeft, kPadLStickRight, kPadLStickUp, kPadLStickDown, kPadRStickLeft, kPadRStickRight, kPadRStickUp, kPadRStickDown,
  kPadCount
};
enum MouseBtn : int { kMouseLeft = 0, kMouseRight, kMouseMiddle, kMouseX1, kMouseX2, kMouseWheelUp, kMouseWheelDown, kMouseCount };
const char* mouse_name(int b);   // "left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"
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
  uint8_t mouse_buttons = 0;           // MouseBtn bits: left, right, middle, x1, x2, wheel up and down (a short pulse per notch)
  float mouse_dx = 0, mouse_dy = 0;    // pixels moved since the previous backend poll
  float mouse_vx = 0, mouse_vy = 0;    // pixels per second, filled by the Hub from mouse_dx/dy (used by mouse-look); y down positive
  bool gyro_present = false;           // a connected pad reports a gyroscope (SDL3 sensor API)
  float gyro_x = 0, gyro_y = 0, gyro_z = 0;   // angular rate in deg/s, SDL axes: x = pitch (positive tips the front up), y = yaw (positive turns left), z = roll
};

// Controller extras, set from the profile settings (runtime/hle/settings.cpp) and read on every poll.
struct PadFx {
  std::atomic<bool> gyro_on{false};              // gyro drives the right (camera) stick, added to the stick and mouse-look
  std::atomic<float> gyro_sens{1.0f};            // 1 = full stick deflection at kGyroFullDegPerSec
  std::atomic<bool> gyro_invert_x{false}, gyro_invert_y{false};
  std::atomic<bool> rumble_on{true};
  std::atomic<float> rumble_strength{1.0f};      // 0..1.5 scale on both motors
};
PadFx& pad_fx();
bool rumble_log_enabled();
bool gyro_aim_enabled();     // profile gyro_on or SNAP_GYRO   // SNAP_RUMBLE_LOG=1
constexpr float kGyroFullDegPerSec = 90.0f;     // turn rate that gives full stick deflection at sensitivity 1
constexpr float kGyroDeadDegPerSec = 1.5f;      // below this the sensor noise is ignored
// Gyro rate to camera stick (-1..1, y up positive). Pure; used by map_pad and the tests.
void gyro_to_stick(float yaw_rate_left_pos_dps, float pitch_rate_up_pos_dps, float sens, bool inv_x, bool inv_y, float* sx, float* sy);

class Backend {
 public:
  virtual ~Backend() = default;
  virtual const char* name() const = 0;
  virtual void poll(RawInput& out) = 0;
  virtual void rumble(float low, float high) { (void)low; (void)high; }  // 0..1 each
  // Cursor policy, set by the Hub before every poll: `lock` confines the cursor to the game window (only while it has the focus),
  // `recenter` also re-centers it after every poll and reports mouse_dx/dy relative to the center (mouse-look).
  virtual void set_cursor_mode(bool lock, bool recenter) { (void)lock; (void)recenter; }
};
std::unique_ptr<Backend> make_default_backend();  // SDL3 if built with HLE_WITH_SDL3, else Win32 XInput/keyboard

// ---- mapping
struct Source {
  enum Kind { None, Pad, KeyboardKey, Mouse } kind = None;
  int id = 0;  // PadButton / Key / MouseBtn
  bool operator==(const Source& o) const { return kind == o.kind && id == o.id; }
};
bool parse_source(const std::string& s, Source* out);
std::string source_to_string(const Source& s);

enum Device : int { kDevKeyboard = 0, kDevMouse, kDevController, kDevCount };
const char* device_name(Device d);          // "Keyboard", "Mouse", "Controller"
Device device_of(const Source& s);          // None counts as Keyboard (never stored)
std::string source_label(const Source& s);  // for the UI: "W", "Left Shift", "Mouse Left", "Wheel Up", "South (Xbox A)"

struct StickMap {
  std::string analog = "left";                      // which physical stick drives it: "left", "right" or "none"
  std::vector<Source> up, down, left, right;        // digital sources (keyboard, d-pad, mouse button ...)
  float deadzone = 0.15f;                           // radial inner dead zone, applied to the analog source
  float outer_deadzone = 1.0f;                      // magnitude at which the analog source reaches full deflection (0.5..1)
  bool invert_y = false;                            // analog source only
};

// The bindable actions: 0..15 are the Switch buttons A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down (Button enum),
// then the four directions of the left stick (16..19: up, down, left, right) and of the right stick (20..23).
constexpr int kActionCount = 24;
constexpr int kMaxSlotsPerDevice = 2;
const char* action_label(int action);        // "A", "ZL", "D-pad Up", "Left stick up" ...
constexpr int kMouseLookFullPxPerSec = 1500; // mouse speed that gives full stick deflection at sensitivity 1

struct Mapping {
  static constexpr int kVersion = 2;
  std::array<std::vector<Source>, kButtonCount> buttons;
  StickMap left, right;
  // controller
  float zl_threshold = 0.5f, zr_threshold = 0.5f;   // LT/RT analog value that counts as pressed
  float stick_button_threshold = 0.5f;              // virtual StickLLeft..StickRDown bits
  bool swap_sticks = false;                         // the left stick's analog source is the physical right stick and vice versa
  int face_layout = 0;                              // 0 by position (Xbox B = Switch A), 1 Nintendo by label (Xbox A = Switch A)
  // mouse
  bool mouse_touch = true;                          // left mouse button drives the touch screen
  bool mouse_look = false;                          // mouse movement drives the right stick
  float mouse_sensitivity = 1.0f;                   // 1 = full deflection at kMouseLookFullPxPerSec
  bool mouse_invert_x = false, mouse_invert_y = false;
  float mouse_smoothing = 0.0f;                     // 0 off .. 1 (averaging window 40 ms .. 340 ms)
  bool lock_cursor = true;                          // confine the cursor to the window while playing (not while the menu is open)
  // accessibility
  bool toggle_zl = false, toggle_zr = false;        // a press flips the trigger on or off instead of holding it
  float ui_scale = 1.0f;
  static Mapping defaults();
  Json to_json() const;
  bool from_json(const Json& j, std::string* err);  // starts from defaults(): keys not present keep their default; version 1 files load too
};

std::vector<Source>& action_sources(Mapping& m, int action);
const std::vector<Source>& action_sources(const Mapping& m, int action);
// Slot editing (a slot is the 0th or 1st binding of one device on one action).
Source slot_get(const std::vector<Source>& v, Device d, int slot);              // kind None when empty
int slot_count(const std::vector<Source>& v, Device d);
bool slot_set(std::vector<Source>& v, Device d, int slot, const Source& s);     // false if s is of another device; drops a duplicate in the other slot
void slot_clear(std::vector<Source>& v, Device d, int slot);
std::vector<int> actions_using(const Mapping& m, const Source& s);              // action ids that use s (more than one is a conflict)
void reset_device(Mapping& m, Device d);                                        // defaults for that device's bindings and settings only
void set_face_layout(Mapping& m, int layout);                                   // swaps south/east and west/north in every controller binding

struct PadSample {
  uint64_t buttons = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;           // +-32767, y up positive
  bool connected = false;
};
struct TouchPoint { int32_t x, y; };                // 1280x720 panel coordinates
struct TouchSample { int count = 0; TouchPoint pt[1]; };

PadSample map_pad(const Mapping& m, const RawInput& in);   // stateless (no toggle mode): what the Hub computes before suppression
bool source_down(const Source& s, const RawInput& in, float trigger_threshold = 0.5f);
TouchSample map_touch(const Mapping& m, const RawInput& in);

// ---- the hub the nn::hid shims read
class Hub {
 public:
  Hub();
  void set_backend(std::unique_ptr<Backend> b);
  void set_mapping(const Mapping& m);   // live: the next poll uses it
  Mapping mapping() const;
  // Reads the JSON table; on a missing file writes the defaults there. Returns false (and keeps the old table) if invalid.
  bool load_mapping(const std::string& path, std::string* err);
  // Startup load with the user-facing rules: `path` (input.json) if present, else the version 1 file `legacy` (migrated and saved
  // to `path`), else defaults. An invalid file is kept as <path>.invalid, the defaults are used and message() says so.
  // Returns false when the defaults were used because of an error.
  bool load_config(const std::string& path, const std::string& legacy);
  bool save(std::string* err = nullptr);                 // atomic write of the current mapping to the loaded path
  std::string config_path() const;
  std::string message() const;                           // last load / save problem or note ("" none)
  uint64_t message_serial() const;                       // increments whenever message() changes
  // Game input is suppressed while the menu is open (the npad reads neutral); the mapped state stays visible as preview().
  void set_menu_open(bool open);
  bool menu_open() const { return menu_open_.load(); }
  bool consume_menu_toggle();                            // true once per Back+Start chord held 0.5 s
  bool cursor_hidden() const { return cursor_hidden_.load(); }
  PadSample preview() const;                             // mapped pad state ignoring the suppression
  // Rebinding: after capture_begin the next newly pressed source of an allowed device (bit per Device) is reported once.
  // Sources already held at the start are ignored until released. F1 is never reported (it opens the menu). 10 s timeout.
  void capture_begin(unsigned device_mask);
  void capture_cancel();
  bool capturing() const { return capturing_.load(); }
  bool capture_result(Source* out);                      // true once a source was caught
  bool capture_timed_out();                              // true once after the timeout
  // Test hook (SNAP_UI_TEST): hold a source down in addition to the real devices.
  void inject_source(const Source& s, bool down);
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
  PadSample pad_, preview_;
  TouchSample touch_;
  int64_t seq_ = 0;
  uint64_t last_poll_ns_ = 0;
  std::string path_, message_;
  uint64_t message_serial_ = 0;
  std::atomic<bool> menu_open_{false}, cursor_hidden_{false}, toggle_req_{false}, capturing_{false};
  bool tog_state_[2] = {false, false}, tog_prev_[2] = {false, false};   // toggle-mode triggers
  struct MSample { uint64_t t; float dx, dy; };
  std::vector<MSample> msamples_;                                       // mouse-look velocity window
  uint64_t chord_since_ns_ = 0;                                         // Back+Start
  bool chord_fired_ = false;
  unsigned cap_mask_ = 0;                                               // capture
  uint64_t cap_deadline_ns_ = 0;
  std::vector<uint32_t> cap_ignore_;
  bool cap_have_ = false, cap_timeout_ = false;
  Source cap_src_;
  RawInput inject_;                                                     // injected sources (test hook)
  uint64_t wheel_up_until_ns_ = 0, wheel_down_until_ns_ = 0;
  void set_message_locked(const std::string& m);
};
Hub& hub();
// Window-thread entry for the mouse wheel (WM_MOUSEWHEEL delta in 1/120 notches); each notch becomes a 100 ms press of Wheel Up/Down.
void post_wheel(int delta);
// Raises a game event for SNAP_INPUT_SCRIPT "anchor" lines (no effect without a script).
void script_event(const char* name);
// Prints every pad device and then the live pad state for `seconds` (snap_boot pads).
void list_pads(int seconds);

// Writes the default mapping JSON (used on first run and by the tests); returns false on I/O error.
bool write_default_mapping(const std::string& path);
// Atomic JSON write: <path>.tmp then rename over <path>.
bool write_mapping_file(const Mapping& m, const std::string& path, std::string* err);

}  // namespace hle::input
