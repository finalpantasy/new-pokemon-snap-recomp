// SPDX-License-Identifier: MIT
// nn::hid host overrides. Layouts are the public nn::hid ones, checked against how `main` uses them:
//   SetSupportedNpadIdType list ends with id 0x20 (Handheld) after No1 (0x9258f4 / 0x925944), then
//   SetSupportedNpadStyleSet(3) = FullKey | Handheld (0x925948); touch is initialized if a flag is clear (0x92595c).
//   Six-axis: handle is one 32-bit word stored next to the npad id (0x4c1514-0x4c152c, 0x7fffffff = none);
//   GetSixAxisSensorStates(buf, 16, handle) with a 0x60-byte state stride (0x4c15ec) and angular velocity at +0x1c.
// All four GetNpadState variants write a 0x28-byte state: {s64 samplingNumber; u64 buttons; s32 lx, ly, rx, ry; u32 attributes}.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "hle.h"
#include "hle_config.h"
#include "input_core.h"

namespace hle {
namespace {

enum : uint32_t { kStyleFullKey = 1, kStyleHandheld = 2, kStyleJoyDual = 4, kStyleJoyLeft = 8, kStyleJoyRight = 16 };
enum : uint32_t { kNpadNo1 = 0, kNpadHandheld = 0x20 };
enum : uint32_t { kAttrConnected = 1, kAttrHandheldAll = 0x3f };
constexpr size_t kNpadStateSize = 0x28;

struct HidState {
  uint32_t supported_styles = 0x1f;
  bool inited = false;
  bool six_axis_started = false;
};
HidState g_hid;

uint32_t active_id() { return config().controller_style == 1 ? kNpadHandheld : kNpadNo1; }
uint32_t active_style() { return config().controller_style == 1 ? kStyleHandheld : kStyleFullKey; }

void ensure_init() {
  if (g_hid.inited) return;
  g_hid.inited = true;
  // input.json (version 2); the version 1 input_map.json is migrated once. An invalid file falls back to the defaults and
  // leaves a message for the controls menu (Hub::message).
  const std::string legacy = config().input_map_path == default_input_path() ? legacy_input_path() : std::string();
  input::hub().load_config(config().input_map_path, legacy);
}

void write_npad_state(void* dst, uint32_t id, uint32_t style) {
  std::memset(dst, 0, kNpadStateSize);
  ensure_init();
  input::hub().poll();
  bool match = id == active_id() && style == active_style() && (g_hid.supported_styles & style);
  store<int64_t>(dst, input::hub().sampling_number());
  if (trace_flags & kTraceHid) {   // log each distinct (id, style, match, buttons) once in a row
    static uint64_t last = ~0ull; static int calls = 0; if (++calls % 600 == 0) log("hid: GetNpadState calls=%d buttons=%#llx", calls, (unsigned long long)input::hub().pad().buttons);
    uint64_t cur = match ? input::hub().pad().buttons : 0xffffffffull;
    uint64_t key = (uint64_t)id << 56 ^ (uint64_t)style << 40 ^ cur;
    static int32_t last_rx = 0, last_ry = 0;
    const int32_t rx = match ? input::hub().pad().rx : 0, ry = match ? input::hub().pad().ry : 0;
    if (key != last || rx != last_rx || ry != last_ry) { last = key; last_rx = rx; last_ry = ry; log("hid: GetNpadState id %u style %#x match=%d buttons=%#llx rx=%d ry=%d", id, style, (int)match, (unsigned long long)cur, rx, ry); }
  }
  if (!match) return;
  input::PadSample p = input::hub().pad();
  uint8_t* b = static_cast<uint8_t*>(dst);
  store<uint64_t>(b + 8, p.buttons);
  store<int32_t>(b + 0x10, p.lx); store<int32_t>(b + 0x14, p.ly);
  store<int32_t>(b + 0x18, p.rx); store<int32_t>(b + 0x1c, p.ry);
  store<uint32_t>(b + 0x20, p.connected ? (style == kStyleHandheld ? kAttrHandheldAll : kAttrConnected) : 0u);
}

// ---- initialization and styles
void hid_initialize_npad(Cpu*) { ensure_init(); }
void hid_set_supported_npad_id_type(Cpu* c) {  // (const NpadIdType* ids, size_t n)
  if (trace_flags & kTraceHid) {
    const uint32_t* ids = ptr<const uint32_t>(arg(c, 0));
    for (uint64_t i = 0; i < arg(c, 1); ++i) log("hid: supported npad id %#x", ids[i]);
  }
}
void hid_set_supported_npad_style_set(Cpu* c) { g_hid.supported_styles = arg32(c, 0); }
void hid_get_npad_style_set(Cpu* c) {  // NpadStyleSet GetNpadStyleSet(const NpadIdType&)
  uint32_t id = load<uint32_t>(ptr<void>(arg(c, 0)));
  ret(c, id == active_id() ? (active_style() & g_hid.supported_styles) : 0u);
}
void hid_disconnect_npad(Cpu*) {}
void hid_get_npad_joy_assignment(Cpu* c) { ret(c, 0); }  // Dual
void hid_set_npad_joy_hold_type(Cpu*) {}
void hid_noop(Cpu*) {}

void hid_get_npad_state_fullkey(Cpu* c) { write_npad_state(ptr<void>(arg(c, 0)), load<uint32_t>(ptr<void>(arg(c, 1))), kStyleFullKey); }
void hid_get_npad_state_handheld(Cpu* c) { write_npad_state(ptr<void>(arg(c, 0)), load<uint32_t>(ptr<void>(arg(c, 1))), kStyleHandheld); }
void hid_get_npad_state_joyleft(Cpu* c) { write_npad_state(ptr<void>(arg(c, 0)), load<uint32_t>(ptr<void>(arg(c, 1))), kStyleJoyLeft); }
void hid_get_npad_state_joyright(Cpu* c) { write_npad_state(ptr<void>(arg(c, 0)), load<uint32_t>(ptr<void>(arg(c, 1))), kStyleJoyRight); }

// ---- touch screen: the left mouse button is one finger on the 1280x720 panel
// TouchScreenState<6>: {s64 samplingNumber; s32 count; s32 pad; TouchState touches[6]}; TouchState (0x28 bytes):
// {s64 deltaTime; u32 attributes; s32 fingerId; s32 x, y, diameterX, diameterY, rotationAngle; s32 reserved}
void hid_get_touch_screen_state6(Cpu* c) {
  uint8_t* st = ptr<uint8_t>(arg(c, 0));
  std::memset(st, 0, 8 + 8 + 6 * 0x28);
  ensure_init();
  input::hub().poll();
  input::TouchSample t = input::hub().touch();
  store<int64_t>(st, input::hub().sampling_number());
  store<int32_t>(st + 8, t.count);
  if (t.count) {
    uint8_t* p = st + 16;
    store<uint32_t>(p + 8, 1);   // attributes: start of touch
    store<int32_t>(p + 12, 0);   // finger id
    store<int32_t>(p + 16, t.pt[0].x);
    store<int32_t>(p + 20, t.pt[0].y);
    store<int32_t>(p + 24, 8);   // diameter
    store<int32_t>(p + 28, 8);
  }
}
void hid_get_gesture_states(Cpu* c) { ret(c, 0); }  // int GetGestureStates(GestureState*, int): no gestures

// ---- six-axis motion: a resting sensor (no gyro source on PC yet)
constexpr size_t kSixAxisStateSize = 0x60;
void hid_get_six_axis_handles(Cpu* c) {  // int GetSixAxisSensorHandles(SixAxisSensorHandle* out, int n, const NpadIdType&, NpadStyleSet)
  uint32_t* out = ptr<uint32_t>(arg(c, 0));
  int n = (int)arg32(c, 1);
  uint32_t id = load<uint32_t>(ptr<void>(arg(c, 2)));
  uint32_t style = arg32(c, 3);
  int have = 0;
  if (id == active_id() && (style & active_style())) have = 1;
  else if ((style & kStyleJoyDual) && id == active_id()) have = 2;
  have = std::min(have, n);
  if (trace_flags & kTraceHid) log("hid: GetSixAxisSensorHandles id %u style %#x -> %d", id, style, have);
  for (int i = 0; i < have; ++i) out[i] = ((id & 0xffff) << 8) | (uint32_t)(i + 1);  // nonzero, never 0x7fffffff
  ret(c, (uint64_t)have);
}
void hid_start_six_axis(Cpu*) { g_hid.six_axis_started = true; if (trace_flags & kTraceHid) log("hid: StartSixAxisSensor"); }
void hid_stop_six_axis(Cpu*) { g_hid.six_axis_started = false; }
// With a real or fake gyro (input::RawInput::gyro_present and gyro aim on) the state carries its angular rate; the game never calls these
// today (verified with SNAP_TRACE hid), so this path is untested against the game itself.
bool gyro_live(float* yaw_dps, float* pitch_dps, float* roll_dps) {
  input::RawInput r = input::hub().raw();
  if (!r.gyro_present || !input::gyro_aim_enabled()) return false;
  *yaw_dps = r.gyro_y; *pitch_dps = r.gyro_x; *roll_dps = r.gyro_z;
  return true;
}
void hid_is_six_axis_at_rest(Cpu* c) { float a, b, d; ret(c, gyro_live(&a, &b, &d) && (a * a + b * b + d * d) > 4.0f ? 0 : 1); }  // bool: the game skips gyro aiming while true
void hid_get_six_axis_states(Cpu* c) {  // int GetSixAxisSensorStates(SixAxisSensorState* out, int n, const SixAxisSensorHandle&)
  int n = (int)arg32(c, 1);
  if (trace_flags & kTraceHid) { static int calls = 0; if (calls++ < 20) log("hid: GetSixAxisSensorStates n=%d", n); }
  if (n < 1) { ret(c, 0); return; }
  uint8_t* st = ptr<uint8_t>(arg(c, 0));
  std::memset(st, 0, kSixAxisStateSize);
  int64_t seq = input::hub().sampling_number();
  store<int64_t>(st + 0, 16666666);            // deltaTime ns
  store<int64_t>(st + 8, seq);                 // samplingNumber
  store<float>(st + 0x18, -1.0f);              // acceleration z (face up, at rest)
  float yaw, pitch, roll;
  if (gyro_live(&yaw, &pitch, &roll)) {   // nn::hid angular velocity: 1.0 = one turn per second; x = pitch, y = yaw, z = roll
    store<float>(st + 0x1c, pitch / 360.0f); store<float>(st + 0x20, yaw / 360.0f); store<float>(st + 0x24, roll / 360.0f);
  }
  store<float>(st + 0x34, 1.0f);               // direction matrix = identity
  store<float>(st + 0x44, 1.0f);
  store<float>(st + 0x54, 1.0f);
  ret(c, 1);
}

// ---- vibration: forwards to the backend rumble motors
void hid_get_vibration_handles(Cpu* c) {  // int GetVibrationDeviceHandles(VibrationDeviceHandle* out, int n, const NpadIdType&, NpadStyleSet)
  uint32_t* out = ptr<uint32_t>(arg(c, 0));
  int n = (int)arg32(c, 1);
  uint32_t id = load<uint32_t>(ptr<void>(arg(c, 2)));
  uint32_t style = arg32(c, 3);
  int have = (id == active_id() && (style & active_style())) ? 2 : 0;  // left and right actuators
  have = std::min(have, n);
  if (input::rumble_log_enabled()) log("rumble: GetVibrationDeviceHandles id %u style %#x n=%d -> %d from main+%#llx", id, style, n, have, (unsigned long long)(c->x[30] - 0x7100000000ull));
  for (int i = 0; i < have; ++i) out[i] = ((id & 0xffff) << 8) | (uint32_t)(i + 1);
  ret(c, (uint64_t)have);
}
void hid_send_vibration_value(Cpu* c) {  // (const VibrationDeviceHandle&, const VibrationValue&): {f32 ampLow, freqLow, ampHigh, freqHigh}
  const float* v = ptr<const float>(arg(c, 1));
  if (trace_flags & kTraceHid) log("hid: SendVibrationValue");
  if (input::rumble_log_enabled()) log("rumble: SendVibrationValue handle=%#x ampLow=%.3f freqLow=%.1f ampHigh=%.3f freqHigh=%.1f from main+%#llx", load<uint32_t>(ptr<void>(arg(c, 0))), v[0], v[1], v[2], v[3], (unsigned long long)(c->x[30] - 0x7100000000ull));
  // Two actuators (handle low byte 1 = left, 2 = right) feed one pad: remember each one's amplitudes and send the stronger of
  // the two per motor, so a left-then-right call pair per frame does not flip the motors between two values. NaN counts as 0.
  const auto amp = [](float a) { return std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 0.0f; };
  static float s_low[2] = {0, 0}, s_high[2] = {0, 0};
  const uint32_t h = load<uint32_t>(ptr<void>(arg(c, 0)));
  const int idx = ((h & 0xff) == 2) ? 1 : 0;
  s_low[idx] = amp(v[0]); s_high[idx] = amp(v[2]);
  input::hub().rumble(std::max(s_low[0], s_low[1]), std::max(s_high[0], s_high[1]));
  input::script_event("vibration");   // first call: the lab cutscene hands over the camera (scripted-input anchor, host/skip_intro.txt)
}

// ---- controller applet: one player, the active npad
void hid_show_controller_support(Cpu* c) {  // Result ShowControllerSupport(ControllerSupportResultInfo*, const ControllerSupportArg&)
  uint8_t* r = ptr<uint8_t>(arg(c, 0));
  if (r) { store<int32_t>(r, 1); store<uint32_t>(r + 4, active_id()); }
  ret(c, kSuccess);
}

}  // namespace

void register_hid(std::vector<Entry>& out) {
  HLE_IMPL("hid", "_ZN2nn3hid14InitializeNpadEv", hid_initialize_npad);
  HLE_IMPL("hid", "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", hid_set_supported_npad_id_type);
  HLE_IMPL("hid", "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_set_supported_npad_style_set);
  HLE_IMPL("hid", "_ZN2nn3hid15GetNpadStyleSetERKj", hid_get_npad_style_set);
  HLE_NOOP("hid", "_ZN2nn3hid14DisconnectNpadERKj", hid_disconnect_npad);
  HLE_IMPL("hid", "_ZN2nn3hid20GetNpadJoyAssignmentERKj", hid_get_npad_joy_assignment);
  HLE_NOOP("hid", "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hid_set_npad_joy_hold_type);
  HLE_IMPL("hid", "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_get_npad_state_fullkey);
  HLE_IMPL("hid", "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_get_npad_state_handheld);
  HLE_IMPL("hid", "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_get_npad_state_joyleft);
  HLE_IMPL("hid", "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_get_npad_state_joyright);
  HLE_NOOP("hid", "_ZN2nn3hid17InitializeGestureEv", hid_noop);
  HLE_IMPL("hid", "_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", hid_get_gesture_states);
  HLE_NOOP("hid", "_ZN2nn3hid21InitializeTouchScreenEv", hid_noop);
  HLE_IMPL("hid", "_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE", hid_get_touch_screen_state6);
  HLE_IMPL("hid", "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_get_six_axis_handles);
  HLE_NOOP("hid", "_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_start_six_axis);
  HLE_NOOP("hid", "_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_stop_six_axis);
  HLE_IMPL("hid", "_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", hid_is_six_axis_at_rest);
  HLE_IMPL("hid", "_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_get_six_axis_states);
  HLE_IMPL("hid", "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_get_vibration_handles);
  HLE_NOOP("hid", "_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_noop);
  HLE_IMPL("hid", "_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", hid_send_vibration_value);
  HLE_NOOP("hid", "_ZN2nn3hid20ControllerSupportArg10SetDefaultEv", hid_noop);
  HLE_IMPL("hid", "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", hid_show_controller_support);
}

}  // namespace hle
