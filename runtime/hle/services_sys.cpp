// SPDX-License-Identifier: MIT
// nn::oe, nn::err, nn::time, nn::settings, nn::account, nn::album, nn::ngc, nn::swkbd, nn::vi, nn::crypto, nn::diag.
// Registration list: reg_sys.inc (generated from imports_usage.tsv; one HLE_IMPL/HLE_NOOP per line).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

#include "hle_config.h"
#include "input_core.h"
#include "svc_common.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace hle {

namespace {

// ---- nn::settings::LanguageCode: 8 chars, returned by value in x0 (little endian)
const char* const kLanguageCodes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt",
                                      "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans-CN", "zh-Hant-TW", "pt-BR"};
constexpr int kLanguageCount = (int)(sizeof kLanguageCodes / sizeof kLanguageCodes[0]);

uint64_t language_code(int lang) {
  if (lang < 0 || lang >= kLanguageCount) lang = 1;
  char b[8] = {};
  std::strncpy(b, kLanguageCodes[lang], 7);
  uint64_t v;
  std::memcpy(&v, b, 8);
  return v;
}

std::string g_last_error;
ErrorUiFn g_error_ui = nullptr;

void default_error_ui(const char* title, const char* message) {
  log("%s: %s", title, message);
#ifdef _WIN32
  if (!config().headless) MessageBoxA(nullptr, message, title, MB_OK | MB_ICONERROR);
#endif
}

void show_error(const std::string& message) {
  g_last_error = message;
  (g_error_ui ? g_error_ui : default_error_ui)("Pokemon Snap", message.c_str());
}

// ---- days <-> civil date (Howard Hinnant's algorithms)
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

// ---- PNG (stored deflate blocks; no compression library needed)
uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static std::once_flag once;
  std::call_once(once, [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  });
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

void put_be32(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s += (char)(v >> (i * 8));
}

void png_chunk(std::string& out, const char* type, const std::string& data) {
  put_be32(out, (uint32_t)data.size());
  std::string td = std::string(type, 4) + data;
  out += td;
  put_be32(out, crc32_update(0, reinterpret_cast<const uint8_t*>(td.data()), td.size()));
}

std::string encode_png_rgba(const uint8_t* rgba, uint32_t w, uint32_t h) {
  std::string raw;
  raw.reserve((size_t)(w * 4 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw += '\0';
    raw.append(reinterpret_cast<const char*>(rgba + (size_t)y * w * 4), (size_t)w * 4);
  }
  std::string z = "\x78\x01";
  uint32_t a = 1, b = 0;
  for (unsigned char ch : raw) { a = (a + ch) % 65521; b = (b + a) % 65521; }
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z += (char)(last ? 1 : 0);
    z += (char)(n & 0xff); z += (char)(n >> 8);
    z += (char)(~n & 0xff); z += (char)((~n >> 8) & 0xff);
    z.append(raw, pos, n);
    pos += n;
    if (last) break;
  }
  put_be32(z, (b << 16) | a);
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  put_be32(ihdr, w); put_be32(ihdr, h);
  ihdr += '\x08'; ihdr += '\x06'; ihdr += '\0'; ihdr += '\0'; ihdr += '\0';
  png_chunk(out, "IHDR", ihdr);
  png_chunk(out, "IDAT", z);
  png_chunk(out, "IEND", "");
  return out;
}

// ---- vsync feeder
struct Vsync {
  std::thread th;
  std::atomic<bool> run{false};
  void* event = nullptr;
};
Vsync& vsync() { static Vsync v; return v; }
int g_display_token, g_layer_token;

}  // namespace

void set_error_ui(ErrorUiFn fn) { g_error_ui = fn; }
const std::string& last_error_message() { return g_last_error; }
void* vi_native_window() { static int token; return &token; }

void vi_stop_vsync() {
  auto& v = vsync();
  v.run = false;
  if (v.th.joinable()) v.th.join();
}

// ---- nn::oe
void oe_get_operation_mode(Cpu* c) { ret(c, config().docked ? 1 : 0); }  // OperationMode_Console = 1

void oe_set_performance_configuration(Cpu* c) {
  if (trace_flags & kTraceSvc) log("oe::SetPerformanceConfiguration(mode=%u, config=%#x)", arg32(c, 0), arg32(c, 1));
}

void oe_get_desired_language(Cpu* c) { ret(c, language_code(config().language)); }

// Applet notification queue (nn::oe::Message values: 15 FocusStateChanged, 16 Resume, 30 OperationModeChanged). The host window code
// (host/window_opts.cpp) posts here; the game polls TryPopNotificationMessage once per frame (its handler is at main+0x5dc3f0).
static std::mutex g_oe_mu;
static std::vector<uint32_t> g_oe_q;
void oe_post_message(uint32_t msg) { std::lock_guard<std::mutex> lk(g_oe_mu); if (g_oe_q.size() < 16) g_oe_q.push_back(msg); }
void oe_try_pop_notification_message(Cpu* c) {
  uint32_t m = 0; bool have = false;
  { std::lock_guard<std::mutex> lk(g_oe_mu); if (!g_oe_q.empty()) { m = g_oe_q.front(); g_oe_q.erase(g_oe_q.begin()); have = true; } }
  if (uint64_t p = arg(c, 0)) store<uint32_t>(ptr<void>(p), m);
  ret(c, have ? 1 : 0);
}

void oe_get_program_total_active_time(Cpu* c) {
  static const auto start = std::chrono::steady_clock::now();
  ret(c, (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

// ---- nn::settings
void settings_language_code_make(Cpu* c) { ret(c, language_code((int)arg32(c, 0))); }

void settings_language_code_eq(Cpu* c) {
  uint64_t code = load<uint64_t>(ptr<void>(arg(c, 0)));
  int lang = load<int32_t>(ptr<void>(arg(c, 1)));
  ret(c, code == language_code(lang) ? 1 : 0);
}

// ---- nn::time (UTC only; the game reads the calendar for save stamps and the album)
void time_get_device_location_name(Cpu* c) {
  if (uint64_t p = arg(c, 0)) { std::memset(ptr<void>(p), 0, 36); std::memcpy(ptr<void>(p), "UTC", 3); }
  ret(c, 0);
}

void time_load_time_zone_rule(Cpu* c) {
  if (uint64_t p = arg(c, 0)) std::memset(ptr<void>(p), 0, 0x4000);  // TimeZoneRule is opaque; UTC needs no data
  ret(c, 0);
}

void time_to_calendar_time(Cpu* c) {
  // Result ToCalendarTime(CalendarTime*, CalendarAdditionalInfo*, const PosixTime&, const TimeZoneRule&)
  int64_t t = load<int64_t>(ptr<void>(arg(c, 2)));
  int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  int64_t secs = t - days * 86400;
  int64_t y; unsigned m, d;
  civil_from_days(days, y, m, d);
  if (uint64_t p = arg(c, 0)) {
    uint8_t* o = ptr<uint8_t>(p);
    store<int16_t>(o, (int16_t)y);
    o[2] = (uint8_t)m; o[3] = (uint8_t)d;
    o[4] = (uint8_t)(secs / 3600); o[5] = (uint8_t)(secs / 60 % 60); o[6] = (uint8_t)(secs % 60);
  }
  if (uint64_t p = arg(c, 1)) {
    uint8_t* o = ptr<uint8_t>(p);
    std::memset(o, 0, 24);
    int64_t dow = ((days % 7) + 11) % 7;  // 1970-01-01 was a Thursday (4)
    store<int32_t>(o, (int32_t)dow);
    store<int32_t>(o + 4, (int32_t)(days - days_from_civil(y, 1, 1)));
    std::memcpy(o + 8, "UTC", 3);
  }
  ret(c, 0);
}

void time_to_posix_time_from_utc(Cpu* c) {
  const uint8_t* o = ptr<uint8_t>(arg(c, 0));
  int64_t y = load<int16_t>(o);
  int64_t t = days_from_civil(y, o[2], o[3]) * 86400 + o[4] * 3600 + o[5] * 60 + o[6];
  ret(c, (uint64_t)t);
}

void time_clock_now(Cpu* c) { ret(c, now_unix_seconds()); }
void time_to_time_t(Cpu* c) { ret(c, (uint64_t)load<int64_t>(ptr<void>(arg(c, 0)))); }

void time_get_current_time(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<int64_t>(ptr<void>(p), (int64_t)now_unix_seconds());
  ret(c, 0);
}

// ---- nn::account: one local user
void account_get_user_count(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<int32_t>(ptr<void>(p), 1);
  ret(c, 0);
}

void account_try_open_preselected_user(Cpu* c) {
  // UserHandle = {Uid(16 bytes), context(8)}
  if (uint64_t p = arg(c, 0)) {
    store<uint64_t>(ptr<uint8_t>(p), config().uid[0]);
    store<uint64_t>(ptr<uint8_t>(p) + 8, config().uid[1]);
    store<uint64_t>(ptr<uint8_t>(p) + 16, 1);
  }
  ret(c, 1);
}

void account_get_user_id(Cpu* c) {
  if (uint64_t p = arg(c, 0)) {
    store<uint64_t>(ptr<uint8_t>(p), config().uid[0]);
    store<uint64_t>(ptr<uint8_t>(p) + 8, config().uid[1]);
  }
  ret(c, 0);
}

void account_get_nickname(Cpu* c) {
  if (uint64_t p = arg(c, 0)) {
    char* o = ptr<char>(p);  // Nickname: char[33]
    std::memset(o, 0, 33);
    std::strncpy(o, config().user_name.c_str(), 32);
  }
  ret(c, 0);
}

void account_is_nsa_available(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<uint8_t>(ptr<void>(p), 0);
  ret(c, 0);
}

void account_async_has_done(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<uint8_t>(ptr<void>(p), 1);
  ret(c, 0);
}

void account_async_get_system_event(Cpu* c) {
  void* ev = ptr<void>(arg(c, 1));
  if (ev && os_hooks().init_system_event) {
    os_hooks().init_system_event(ev, true);
    if (os_hooks().signal_system_event) os_hooks().signal_system_event(ev);
  } else {
    log("account::AsyncContext::GetSystemEvent: no os hooks installed (see PROPOSED_SHARED_CHANGES.md)");
  }
  ret(c, 0);
}

// ---- nn::err
void err_application_error_arg(Cpu* c) {
  // ApplicationErrorArg(u32 error, const char* dialog, const char* fullscreen, const LanguageCode&)
  uint8_t* o = ptr<uint8_t>(arg(c, 0));
  std::memset(o, 0, 0x1010);
  store<uint32_t>(o, arg32(c, 1));
  if (uint64_t lc = arg(c, 4)) std::memcpy(o + 8, ptr<void>(lc), 8);
  if (uint64_t s = arg(c, 2)) std::strncpy(reinterpret_cast<char*>(o + 16), ptr<char>(s), 2047);
  if (uint64_t s = arg(c, 3)) std::strncpy(reinterpret_cast<char*>(o + 2064), ptr<char>(s), 2047);
  ret(c, arg(c, 0));
}

void err_show_application_error(Cpu* c) {
  const uint8_t* o = ptr<uint8_t>(arg(c, 0));
  char msg[2200];
  std::snprintf(msg, sizeof msg, "Application error %u\n%.2047s", load<uint32_t>(o), reinterpret_cast<const char*>(o + 16));
  show_error(msg);
  ret(c, 0);
}

void err_show_error_result(Cpu* c) {
  uint32_t r = arg32(c, 0);
  char msg[96];
  std::snprintf(msg, sizeof msg, "Error %04u-%04u (%#x)", 2000 + result_module(r), result_desc(r), r);
  show_error(msg);
  ret(c, 0);
}

void err_show_error_variant(Cpu* c) {
  (void)c;
  show_error("nn::err::ShowError(ErrorResultVariant)");
  ret(c, 0);
}

// ---- nn::diag
void diag_abort(Cpu* c) {
  std::string m = "nn::diag abort: expr=\"" + safe_cstr(arg(c, 0), 200) + "\" func=\"" + safe_cstr(arg(c, 1), 200) +
                  "\" file=\"" + safe_cstr(arg(c, 2), 260) + "\" line=" + std::to_string((int)arg32(c, 3)) +
                  " caller=" + hex_u64(c->x[30]);
  fatal(m);
}

// ---- nn::album
void album_save_screenshot(Cpu* c) {
  const uint8_t* data = ptr<const uint8_t>(arg(c, 0));
  size_t size = (size_t)arg(c, 1);
  uint32_t image_size = arg32(c, 2);
  // ImageSize values are not seen at a call site in `main`: 0 = 1280x720, 1 = 1920x1080 are the SDK order; anything
  // else is derived from the data size (RGBA8, 16:9).
  uint32_t w = 1280, h = 720;
  if (image_size == 1) { w = 1920; h = 1080; }
  if (size != (size_t)w * h * 4) {
    if (size % 4 || size < 16) { ret(c, 0); return; }
    size_t px = size / 4;
    h = (uint32_t)std::llround(std::sqrt((double)px * 9.0 / 16.0));
    w = h ? (uint32_t)(px / h) : 0;
    if (!w || (size_t)w * h * 4 != size) { log("album::SaveScreenshot: unexpected size %zu", size); ret(c, 0); return; }
  }
  std::string dir = config().screenshot_dir.empty() ? config().data_root + "/screenshots" : config().screenshot_dir;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  static std::atomic<int> counter{0};
  char name[64];
  std::snprintf(name, sizeof name, "/snap_%llu_%03d.png", (unsigned long long)now_unix_seconds(), counter++);
  std::string png = encode_png_rgba(data, w, h);
  if (FILE* f = std::fopen((dir + name).c_str(), "wb")) {
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    log("album: saved %s%s (%ux%u)", dir.c_str(), name, w, h);
  }
  ret(c, 0);
}

// ---- nn::ngc, swkbd, crypto
void ngc_count_numbers(Cpu* c) {
  const char16_t* s = ptr<const char16_t>(arg(c, 0));
  int n = 0;
  for (; s && *s; ++s) if ((*s >= u'0' && *s <= u'9') || (*s >= 0xFF10 && *s <= 0xFF19)) ++n;
  ret(c, (uint64_t)n);
}

// There is no on-screen keyboard in the host: the player name is fixed ("Ash", or SNAP_PLAYER_NAME). The game calls this every
// frame while a name prompt is open and re-opens it on cancel, so cancelling stalls the profile creation.
// nn::swkbd::String {char16_t* ptr; int bufferChars;} (layout confirmed from the call site: ptr nonzero, size > 0).
void swkbd_show_keyboard(Cpu* c) {
  uint8_t* st = ptr<uint8_t>(arg(c, 0));
  char16_t* out = st ? load<char16_t*>(st) : nullptr;
  int cap = st ? (int)load<int32_t>(st + 8) : 0;
  static bool once = false;
  if (!once) { once = true; log("swkbd::ShowKeyboard: String ptr %p cap %d (host answers with a fixed name)", (void*)out, cap); }
  if (!out) { ret(c, kResultSwkbdCanceled); return; }
  input::script_event("swkbd");
  const char* nm = std::getenv("SNAP_PLAYER_NAME");
  if (!nm || !*nm) nm = "Ash";
  int n = 0;
  int lim = cap > 1 && cap < 64 ? cap - 1 : 8;
  for (; nm[n] && n < lim; ++n) out[n] = (char16_t)(uint8_t)nm[n];
  out[n] = 0;
  ret(c, 0);
}

void crypto_random_bytes(Cpu* c) {
  static std::mutex m;
  static std::mt19937_64 rng{std::random_device{}()};
  uint8_t* p = ptr<uint8_t>(arg(c, 0));
  size_t n = (size_t)arg(c, 1);
  std::lock_guard<std::mutex> lk(m);
  for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(rng() >> 24);
  ret(c, 0);
}

// ---- nn::vi: just what the NVN window needs
void vi_open_default_display(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<void*>(ptr<void>(p), &g_display_token);
  ret(c, 0);
}

void vi_create_layer(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<void*>(ptr<void>(p), &g_layer_token);
  ret(c, 0);
}

void vi_get_native_window(Cpu* c) {
  if (uint64_t p = arg(c, 0)) store<void*>(ptr<void>(p), vi_native_window());
  ret(c, 0);
}

void vi_get_display_vsync_event(Cpu* c) {
  void* ev = ptr<void>(arg(c, 0));
  auto& v = vsync();
  if (!os_hooks().init_system_event || !os_hooks().signal_system_event) {
    log("vi::GetDisplayVsyncEvent: no os hooks installed (see PROPOSED_SHARED_CHANGES.md)");
    ret(c, 0);
    return;
  }
  os_hooks().init_system_event(ev, false);
  vi_stop_vsync();
  v.event = ev;
  v.run = true;
  v.th = std::thread([&v] {
    auto next = std::chrono::steady_clock::now();
    while (v.run) {
      next += std::chrono::microseconds(16667);
      std::this_thread::sleep_until(next);
      os_hooks().signal_system_event(v.event);
    }
  });
  ret(c, 0);
}

void register_sys(std::vector<Entry>& out) {
#include "reg_sys.inc"
}

}  // namespace hle
