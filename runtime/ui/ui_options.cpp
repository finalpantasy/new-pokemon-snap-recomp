// SPDX-License-Identifier: MIT
// F1 menu option pages: Graphics, Display, Photos and Mods, plus the output-mode row of the Audio tab. One table drives every option (label, plain-language
// help, default, whether it applies now or needs a restart). Values live in the per-profile settings file (hle/settings.h); hle/settings.cpp exports them as
// the documented SNAP_* env vars at start, so an env var set by hand still wins and the page says so. Live window options go through the window API (host/window_opts.h,
// remembered in window.json), the audio output mode through snap_audio_set_output (audio.json). "Apply and restart" uses restart_game().
// Layout like the game's own options screen: options on the left, the explanation of the highlighted one on the right.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "hle/hle.h"
#include "hle/json_lite.h"
#include "hle/settings.h"
#include "mods/mods_core.h"
#include "window_opts.h"
#include "ui_internal.h"

namespace snap::ui {

namespace {

namespace st = hle::settings;
namespace fs = std::filesystem;

enum Kind { kSection, kChoice, kSlider, kToggle, kPath };
enum When { kLive, kRestart, kSoon };   // applies now / needs a restart / the feature is not in this build yet (control disabled)

using Choices = std::vector<std::pair<std::string, std::string>>;   // (stored value, label)

struct Opt {
  const char* key;       // settings key, or "" for a section heading (label is its title)
  const char* label;
  Kind kind;
  When when;
  const char* help;      // plain language: what it does, what it costs
  const char* def;       // default value as stored (the game as shipped)
  Choices choices;
  float lo = 0, hi = 1;
  const char* fmt = "%.2f";
  const char* env = nullptr;                    // env var for options that do not go through settings.cpp (window options)
  std::string (*get)() = nullptr;               // custom store (window API, audio layer); null = the settings file
  void (*set)(const std::string&) = nullptr;
  Choices (*dyn)() = nullptr;                   // choices that depend on the PC (monitors)
};

float px(float v) { return v * ImGui::GetFontSize() / theme().font_px; }

// ---- custom stores
std::string get_win_mode() { return std::to_string(snap_window_get_mode()); }
void set_win_mode(const std::string& v) { snap_window_set_mode(std::atoi(v.c_str())); }
std::string get_monitor() { return std::to_string(snap_window_get_monitor()); }
void set_monitor(const std::string& v) { snap_window_set_monitor(std::atoi(v.c_str())); }
std::string get_pause() { return snap_window_get_pause_on_focus_loss() ? "1" : "0"; }
void set_pause(const std::string& v) { snap_window_set_pause_on_focus_loss(v != "0"); }
std::string get_mute() { return snap_window_get_mute_in_background() ? "1" : "0"; }
void set_mute(const std::string& v) { snap_window_set_mute_in_background(v != "0"); }
std::string get_cap() { return std::to_string(snap_window_frame_cap()); }
void set_cap(const std::string& v) { snap_window_set_frame_cap(std::atoi(v.c_str())); }
// ---- quality presets. The preset is not stored: it is worked out from the nine rows below, so changing any row by hand shows "Custom" and
// setting the rows to a preset's values shows that preset. "" in the table = the row's default (the key is erased, the game's own behavior).
// Normal is all defaults. Left out on purpose: draw distance and full-rate animation (photo scores unverified), the lighting preset and
// upscaler rows (other workers), and the HDR/color rows (not quality).
struct PresetKey { const char* key; const char* def; };
const PresetKey kPresetKeys[] = {
    {"gfx_render_scale", "1.00"}, {"gfx_aa", "default"}, {"gfx_sharpen", "0.00"}, {"gfx_aniso", "16"}, {"gfx_lod_bias", "auto"},
    {"gfx_shadow_res", "default"}, {"ssao_quality", "default"}, {"gfx_ssao_samples", "default"}, {"gfx_ssao_hbao", "default"},
};
constexpr int kPresetKeyN = 9;
struct PresetDef { const char* id; const char* label; const char* v[kPresetKeyN]; };
const PresetDef kPresets[] = {
    // Very Low draws at 0.5 (960 x 540). Sub-native scale used to lose the GPU device after the photo score screen: the album thumbnail
    // readback copied a 240x200 guest region out of a 120x100 scaled image; transfers are render-scale aware since 2026-11-12 (nvn_xfer.cpp).
    // The upscaler rows stay out of the presets (they need a backend DLL and pick their own scene scale).
    {"verylow", "Very Low", {"0.50", "none", "", "0", "1", "512", "", "4", ""}},
    {"low", "Low", {"", "off", "", "4", "", "", "", "8", ""}},
    {"normal", "Normal", {"", "", "", "", "", "", "", "", ""}},
    {"high", "High", {"", "ssaa2,smaa", "0.30", "", "", "2048", "high", "", ""}},
    {"ultra", "Ultra", {"2.00", "smaa", "0.30", "", "", "4096", "ultra", "", ""}},
    {"maximum", "Maximum", {"4.00", "smaa", "0.50", "", "", "8192", "ultra", "512", "32,128"}},
};
bool preset_eq(int k, const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (k == 0 || k == 2) return std::fabs((float)std::atof(a.c_str()) - (float)std::atof(b.c_str())) < 1e-4f;
  return false;
}
std::string get_preset() {
  for (const PresetDef& p : kPresets) {
    bool all = true;
    for (int k = 0; k < kPresetKeyN && all; k++) {
      const std::string want = p.v[k][0] ? p.v[k] : kPresetKeys[k].def;
      all = preset_eq(k, st::get(kPresetKeys[k].key, kPresetKeys[k].def), want);
    }
    if (all) return p.id;
  }
  return "custom";
}
void set_preset(const std::string& v) {
  for (const PresetDef& p : kPresets) {
    if (v != p.id) continue;
    for (int k = 0; k < kPresetKeyN; k++) {
      if (p.v[k][0]) st::set(kPresetKeys[k].key, p.v[k]);
      else st::erase(kPresetKeys[k].key);
    }
    return;
  }
}
std::string get_audio_out() {
  const int m = snap_audio_get_output_mode();
  return m == SNAP_AUDIO_OUT_SPEAKERS ? "speakers" : m == SNAP_AUDIO_OUT_SPATIAL ? "spatial" : "stereo";
}
void set_audio_out(const std::string& v) {
  snap_audio_set_output(v == "speakers" ? SNAP_AUDIO_OUT_SPEAKERS : v == "spatial" ? SNAP_AUDIO_OUT_SPATIAL : SNAP_AUDIO_OUT_STEREO, -1, -1);
}
// The monitor list comes from EnumDisplayMonitors, which can be slow (and stalls the presenting thread) on some desktops: enumerate on a worker thread, at most
// every 10 s, and draw the last result.
Choices monitors() {
  static std::mutex mu;
  static Choices cached{{"-1", "Where the window is"}};
  static std::atomic<bool> busy{false};
  static double last = -100;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now - last > 10.0 && !busy.exchange(true)) {
    last = now;
    std::thread([] {
      const auto t0 = std::chrono::steady_clock::now();
      Choices c{{"-1", "Where the window is"}};
      for (int i = 0, n = snap_window_monitor_count(); i < n; ++i) {
        const char* nm = snap_window_monitor_name(i);
        c.push_back({std::to_string(i), std::string("Monitor ") + std::to_string(i + 1) + (nm ? std::string("  ") + nm : "")});
      }
      { std::lock_guard<std::mutex> g(mu); cached = c; }
      hle::log("ui: monitor list: %zu monitors in %lld ms", c.size() - 1, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
      busy = false;
    }).detach();
  }
  std::lock_guard<std::mutex> g(mu);
  return cached;
}

// Flip a row from kSoon to kRestart (or kLive) in this table when its feature lands; its settings key is already wired to the documented env var
// in hle/settings.cpp, so nothing else changes.
std::vector<Opt>& graphics_opts() {
  static std::vector<Opt> v = {
      {"", "Picture quality", kSection, kLive, "", ""},
      {"gfx_preset", "Quality preset", kChoice, kRestart,
       "Sets the picture-quality rows below together: render scale, anti-aliasing, sharpening, anisotropic filtering, texture sharpness, shadow resolution and ambient occlusion. "
       "Very Low: for really weak hardware (draws the 3D scene at half size, 960 x 540, then stretches it to the window; no anti-aliasing, no filtering, softer textures, 512 shadows, few occlusion samples). "
       "Low: for weak or older graphics cards (1920 x 1080, the game's own FXAA, 4x filtering, fewer occlusion samples). Normal: this port's defaults. "
       "High: 2x supersampling, SMAA, 2048 shadows, High occlusion. Ultra: 2x scale, 4096 shadows, Ultra occlusion. "
       "Maximum: 4x scale (7680 x 4320), 8192 shadows and the most occlusion samples. It will slow even a very fast graphics card and needs a lot of video memory. "
       "In the course the costly rows are render scale, anti-aliasing, filtering and texture sharpness; shadow size only sharpens Pokemon shadows and the occlusion rows only act on the photo-select screen. "
       "Changing any row by hand shows Custom. Draw distance and full-rate animation are not part of any preset.",
       "normal",
       {{"verylow", "Very Low"}, {"low", "Low"}, {"normal", "Normal"}, {"high", "High"}, {"ultra", "Ultra"}, {"maximum", "Maximum"}, {"custom", "Custom"}}, 0, 0, "", nullptr, get_preset, set_preset},
      {"gfx_render_scale", "Render scale", kSlider, kRestart,
       "How sharp the 3D picture is drawn, as a multiple of the game's own 1920 x 1080. 2.00x draws 3840 x 2160 and then shows it on your screen. Higher is crisper and slower. "
       "Below 1.00x the scene is drawn smaller and stretched (0.50x = 960 x 540): blurrier, but much lighter for a weak graphics card. "
       "Photo scoring still uses the original-size data. Saved photos stay 1080 x 720 but are as sharp as the scene they were taken from.",
       "1.00", {}, 0.5f, 4.0f, "%.2fx", "SNAP_RENDER_SCALE"},
      {"gfx_aa", "Anti-aliasing", kChoice, kRestart,
       "Smooths jagged edges. FXAA is what the game ships with. SMAA keeps more detail. 2x supersampling draws at twice the window height first: very sharp, and costs a lot of GPU time.",
       "default",
       {{"default", "FXAA (the game's own)"}, {"smaa", "SMAA"}, {"ssaa2", "2x supersampling + FXAA"}, {"ssaa2,smaa", "2x supersampling + SMAA"}, {"off", "Off"}, {"none", "None (not even the game's FXAA)"}}, 0, 0, "", "SNAP_AA"},
      {"gfx_sharpen", "Sharpening", kSlider, kRestart,
       "A gentle sharpening pass after anti-aliasing. 0 is off. Too much makes edges look hard.", "0.00", {}, 0.0f, 1.0f, "%.2f", "SNAP_SHARPEN"},
      {"gfx_upscaler", "Upscaler", kChoice, kRestart,
       "DLSS, DLAA, FSR and XeSS rebuild a sharp picture from a smaller one so you can render fewer pixels. DLSS and DLAA need an NVIDIA RTX card. It replaces the render scale and the 2x supersampling default; the game's FXAA is turned off unless you pick an anti-aliasing mode. Photos stay at native size.",
       "off", {{"off", "Off"}, {"dlaa", "DLAA (native, anti-aliased)"}, {"dlss", "DLSS"}, {"fsr", "FSR"}, {"xess", "XeSS"}}, 0, 0, "", "SNAP_UPSCALER"},
      {"gfx_upscale_quality", "Upscaler quality", kChoice, kRestart,
       "How much smaller the picture is before the upscaler enlarges it. Native draws at full size and only cleans up edges; Quality is 1.5x smaller per side, Balanced about 1.7x, Performance 2x.", "native",
       {{"native", "Native (no downscale)"}, {"quality", "Quality"}, {"balanced", "Balanced"}, {"performance", "Performance"}}, 0, 0, "", "SNAP_UPSCALE_QUALITY"},
      {"", "Textures and shadows", kSection, kLive, "", ""},
      {"gfx_aniso", "Anisotropic filtering", kChoice, kRestart,
       "Keeps ground and walls sharp when you look at them from a shallow angle. 16x is the default here (the Switch version used none) and costs almost nothing on a PC GPU.",
       "16", {{"0", "Off (1x)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, 0, 0, "", "SNAP_ANISO"},
      {"gfx_lod_bias", "Texture sharpness", kChoice, kRestart,
       "Picks which texture detail level is used. Automatic sharpens textures when the render scale is above 1 and leaves the game alone at 1. Sharper values can shimmer.",
       "auto", {{"auto", "Automatic"}, {"-1", "Sharper (-1.0)"}, {"-0.5", "A little sharper (-0.5)"}, {"0", "As the game ships (0)"}, {"0.5", "Softer (+0.5)"}, {"1", "Much softer (+1.0)"}}, 0, 0, "", "SNAP_LOD_BIAS"},
      {"gfx_shadow_res", "Shadow resolution", kChoice, kRestart,
       "Size of the moving shadow map. The game uses 1024. Higher gives crisper shadow edges and uses more video memory.", "default",
       {{"512", "512 (fast, blocky shadows)"}, {"default", "1024 (the game's own)"}, {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192 (very heavy)"}}, 0, 0, "", "SNAP_SHADOW_RES"},
      {"ssao_quality", "Ambient occlusion", kChoice, kRestart,
       "Soft contact shadows in corners and under objects. High and Ultra take more samples. The courses draw no ambient occlusion while you play (their contact shadows are baked in); the effect shows only on the photo-select screen, so this does not change the course view or its frame rate.", "default",
       {{"default", "Default"}, {"high", "High (96 samples)"}, {"ultra", "Ultra (256 samples)"}}, 0, 0, "", "SNAP_SSAO_QUALITY"},
      {"gfx_ssao_samples", "Occlusion samples", kChoice, kRestart,
       "How many samples the simple ambient-occlusion pass takes per pixel. Overrides the Ambient occlusion choice above. Like the row above, it only matters on the photo-select screen, not in the course. 512 is the most the game's pass holds.", "default",
       {{"default", "Default (the game's own)"}, {"8", "8"}, {"16", "16"}, {"64", "64"}, {"128", "128"}, {"256", "256"}, {"512", "512"}}, 0, 0, "", "SNAP_SSAO_SAMPLES"},
      {"gfx_ssao_hbao", "Horizon occlusion", kChoice, kRestart,
       "Directions and steps per direction for the horizon-based occlusion pass (used where the game selects it). More is smoother and costs more.", "default",
       {{"default", "Default (the game's own)"}, {"12,48", "12 x 48"}, {"16,64", "16 x 64"}, {"24,96", "24 x 96"}, {"32,128", "32 x 128 (very heavy)"}}, 0, 0, "", "SNAP_SSAO_HBAO"},
      {"gfx_lighting", "Lighting preset", kChoice, kSoon,
       "One switch for shadow, ambient-occlusion and light-probe detail together. Not in this build yet; the separate shadow and occlusion options above work now.", "default",
       {{"default", "Original"}, {"high", "Enhanced"}, {"ultra", "Maximum"}}, 0, 0, "", "SNAP_LIGHTING"},
      {"", "Color", kSection, kLive, "", ""},
      {"look", "Color look", kChoice, kRestart,
       "A color grade on the finished picture. Neutral leaves the game as shipped.", "neutral",
       {{"neutral", "Neutral (as shipped)"}, {"vivid", "Vivid"}, {"warm", "Warm"}, {"cool", "Cool"}}, 0, 0, "", "SNAP_LOOK"},
      {"look_intensity", "Look strength", kSlider, kRestart, "How strongly the color look is applied. Has no effect while the look is Neutral.", "1.00", {}, 0.0f, 1.5f, "%.2f", "SNAP_LOOK"},
      {"gfx_hdr", "HDR output", kToggle, kRestart,
       "Sends a high-dynamic-range picture to an HDR monitor. Turn Windows HDR on first. On a normal monitor leave this off.", "0", {}, 0, 0, "", "SNAP_HDR"},
      {"gfx_hdr_peak", "HDR peak brightness", kSlider, kRestart,
       "The brightest highlight to send, in nits. Use your monitor's rated peak (400 to 1000 is typical).", "1000", {}, 400.0f, 4000.0f, "%.0f nits", "SNAP_HDR_PEAK"},
      {"gfx_hdr_paper", "HDR paper white", kSlider, kRestart,
       "How bright ordinary white (menus, snow) is, in nits. Lower it if the picture feels too bright in a dark room.", "200", {}, 80.0f, 400.0f, "%.0f nits", "SNAP_HDR_PAPER"},
      {"", "View", kSection, kLive, "", ""},
      // Field of view row removed (owner 2026-11-12: "turn off fov, hide the option, stop working on it for now"; it changes photo
      // scores). SNAP_FOV still works as a developer env switch, off by default.
      {"gfx_draw_distance", "Draw distance", kSlider, kRestart,
       "Pushes out the distances where scenery and Pokemon switch to simpler models or disappear (1 to 4, 1 is the game's own). May change photo scores, being tested.",
       "1", {}, 1.0f, 4.0f, "%.1fx", "SNAP_DRAW_DISTANCE"},
      {"gfx_full_rate_anim", "Full-rate distant animation", kToggle, kRestart,
       "Far-away Pokemon animate every 2nd or 4th frame in the original. On animates them every frame: smoother, a little more CPU. May change photo scores, being tested.", "0", {}, 0, 0, "", "SNAP_FULL_RATE_ANIM"},
      {"", "Frame rate", kSection, kLive, "", ""},
      {"frame_cap", "Frame rate limit", kChoice, kLive,
       "Stops the game from drawing faster than this. Off keeps the game's own 60 FPS pacing. Remembered in window.json.", "0",
       {{"0", "Off (the game's pacing)"}, {"30", "30"}, {"45", "45"}, {"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}}, 0, 0, "", "SNAP_WINDOW_FRAME_CAP", get_cap, set_cap},
  };
  return v;
}

std::vector<Opt>& display_opts() {
  static std::vector<Opt> v = {
      {"", "Window", kSection, kLive, "", ""},
      {"win_mode", "Window mode", kChoice, kLive,
       "Windowed or borderless fullscreen (F11 and Alt+Enter switch as well). Remembered in window.json.", "0",
       {{"0", "Windowed"}, {"1", "Borderless fullscreen"}}, 0, 0, "", "SNAP_WINDOW_MODE", get_win_mode, set_win_mode},
      {"win_monitor", "Monitor", kChoice, kLive,
       "Which screen the borderless window covers. A windowed game stays where you put it.", "-1", {}, 0, 0, "", "SNAP_WINDOW_MONITOR", get_monitor, set_monitor, monitors},
      {"win_size", "Windowed size", kChoice, kRestart,
       "The size of the picture area when the game is windowed. Without a choice the game remembers the size you dragged it to. Output resolution of the finished picture follows the window.",
       "", {{"", "Remembered"}, {"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"}, {"1920x1080", "1920 x 1080"}, {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160"}}, 0, 0, "", "SNAP_WINDOW_W"},
      {"", "Text", kSection, kLive, "", ""},
      {"gfx_ui_font", "Menu text sharpness", kChoice, kRestart,
       "The game's own menus and labels are drawn from a low-resolution letter sheet. Higher draws the letters sharper on large screens, at the same size and position. Automatic uses 2x when the game window is 1440 lines tall or more.", "default",
       {{"default", "Automatic (2x at 1440p and above)"}, {"1", "Off (as the game ships)"}, {"2", "Sharper (2x)"}, {"3", "Sharper (3x)"}, {"4", "Sharpest (4x)"}}, 0, 0, "", "SNAP_UI_FONT_SCALE"},
      {"", "Focus", kSection, kLive, "", ""},
      {"win_pause", "Pause when unfocused", kToggle, kLive,
       "Freezes the game while you are in another window, so nothing happens while you are away. Remembered in window.json.", "1", {}, 0, 0, "", "SNAP_WINDOW_PAUSE", get_pause, set_pause},
      {"win_mute", "Mute in the background", kToggle, kLive,
       "Silences the game while its window is not in front.", "1", {}, 0, 0, "", "SNAP_WINDOW_MUTE_BG", get_mute, set_mute},
      {"", "Start-up", kSection, kLive, "", ""},
      {"fast_boot", "Fast boot", kToggle, kRestart,
       "Skips the frame limiter during the first minute of loading, so the title screen comes up sooner (about 16 s faster here). Gameplay is not affected.", "0", {}, 0, 0, "", "SNAP_FAST_BOOT"},
  };
  return v;
}

std::vector<Opt>& photo_opts() {
  static std::vector<Opt> v = {
      {"", "Photo files", kSection, kLive, "", ""},
      {"photo_export", "Save a PNG of every photo", kToggle, kRestart,
       "Each photo you keep is also written as a full-quality PNG file you can share, next to the game's own in-save copy. The save and scoring are not affected.", "1"},
      {"photo_dir", "PNG folder", kPath, kRestart,
       "Where the PNG files go. Empty uses %APPDATA%\\SnapRecomp\\photos. The folder is created when the first photo is saved.", ""},
  };
  return v;
}

std::vector<Opt>& audio_opts() {
  static std::vector<Opt> v = {
      {"audio_output", "Output mode", kChoice, kRestart,
       "Stereo is the game's own mix. Speakers sends 5.1 or 7.1 to a surround system (the stereo mix is spread around you). Spatial uses Windows Spatial Sound (Dolby Atmos, Windows Sonic) for headphones. The game opens its sound output once, so this needs a restart. Saved in audio.json.",
       "stereo", {{"stereo", "Stereo"}, {"speakers", "Speakers (5.1 / 7.1)"}, {"spatial", "Spatial (headphones)"}}, 0, 0, "", "SNAP_AUDIO_OUTPUT", get_audio_out, set_audio_out},
  };
  return v;
}

// ---- value access
std::string cur_of(const Opt& o) { return o.get ? o.get() : st::get(o.key, o.def); }

bool same_value(const Opt& o, const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (o.kind == kSlider) return std::fabs((float)std::atof(a.c_str()) - (float)std::atof(b.c_str())) < 1e-4f;
  return false;
}

void set_of(const Opt& o, const std::string& v) {
  if (o.set) { o.set(v); return; }
  if (same_value(o, v, o.def)) st::erase(o.key);   // the default is "not set": the env var is left alone and the game keeps its own behavior
  else st::set(o.key, v);
}

std::string fmt_float(float f) { char b[32]; std::snprintf(b, sizeof b, "%.2f", f); return b; }

bool env_set_by_hand(const Opt& o) {
  if (o.set && o.when == kLive) return false;   // changed through the API at once; the env var only matters at the next start
  if (st::env_name(o.key)) return st::env_wins(o.key);
  if (o.env) { const char* e = std::getenv(o.env); return e && *e; }
  return false;
}

// ---- restart tracking: the saved value of every restart option when the game started, compared with now
std::map<std::string, std::string> g_start;
std::string g_mods_start;
bool g_started = false;

std::string mods_state();   // below
void mods_reload();

void for_each_opt(void (*f)(const Opt&, void*), void* u) {
  for (auto* list : {&graphics_opts(), &display_opts(), &photo_opts(), &audio_opts()})
    for (const Opt& o : *list) if (o.kind != kSection) f(o, u);
}

void capture_start() {
  if (g_started) return;
  g_started = true;
  for_each_opt([](const Opt& o, void*) { g_start[o.key] = cur_of(o); }, nullptr);
  mods_reload();
  g_mods_start = mods_state();
}

struct Pend { std::vector<std::string> names; };

void pend_one(const Opt& o, void* u) {
  if (o.when != kRestart || env_set_by_hand(o)) return;
  auto it = g_start.find(o.key);
  if (it != g_start.end() && !same_value(o, it->second, cur_of(o))) ((Pend*)u)->names.push_back(o.label);
}

// look strength has no effect on a neutral look: do not count it alone
bool is_pending(const Opt& o) {
  if (o.when != kRestart || env_set_by_hand(o)) return false;
  auto it = g_start.find(o.key);
  if (it == g_start.end() || same_value(o, it->second, cur_of(o))) return false;
  if (!std::strcmp(o.key, "look_intensity") && st::get("look", "neutral") == "neutral" && g_start["look"] == "neutral") return false;
  return true;
}

// ---- row drawing
const Opt* g_help = nullptr;   // the option under the pointer or the keyboard/pad focus this frame

float g_scroll_req = -1;   // scripted test: scroll the option list to this fraction of its length
std::map<std::string, float> g_edit;   // slider value while it is held (committed on release: each commit writes a file)
std::map<std::string, std::string> g_text;

void badge(const Opt& o) {
  const Theme& t = theme();
  ImGui::SameLine();
  if (o.when == kSoon) ImGui::TextColored(t.text_dim, "coming soon");
  else if (o.when == kLive) ImGui::TextColored(t.good, "applies now");
  else if (is_pending(o)) ImGui::TextColored(t.title, "changed: restart to apply");
  else ImGui::TextColored(t.text_dim, "restart needed");
}

void draw_choice(const Opt& o) {
  const std::string cur = cur_of(o);
  Choices dyn;
  const Choices& list = o.dyn ? (dyn = o.dyn()) : o.choices;
  std::string label = cur.empty() && o.choices.empty() ? "" : cur;
  for (auto& c : list) if (c.first == cur) label = c.second;
  if (label == cur && !cur.empty() && !std::strcmp(o.key, "frame_cap")) label = cur + " fps (custom)";
  ImGui::SetNextItemWidth(px(250));
  if (ImGui::BeginCombo("##v", label.c_str())) {
    for (auto& c : list) {
      if (ImGui::Selectable(c.second.c_str(), c.first == cur)) set_of(o, c.first);
      if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) g_help = &o;
    }
    ImGui::EndCombo();
  }
}

void draw_slider(const Opt& o) {
  const std::string cur = cur_of(o);
  float v = (float)std::atof(cur.c_str());
  auto it = g_edit.find(o.key);
  float tmp = it != g_edit.end() ? it->second : v;
  ImGui::SetNextItemWidth(px(250));
  const bool changed = ImGui::SliderFloat("##v", &tmp, o.lo, o.hi, o.fmt, ImGuiSliderFlags_AlwaysClamp);
  if (changed) g_edit[o.key] = tmp;
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    set_of(o, fmt_float(std::round(tmp * 100.0f) / 100.0f));
    g_edit.erase(o.key);
  } else if (!ImGui::IsItemActive() && !changed) g_edit.erase(o.key);
}

// An unchecked box is the same pale grey as the panel: give it an outline.
bool boxed_checkbox(const char* id, bool* b) {
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, px(1.5f)));
  ImGui::PushStyleColor(ImGuiCol_Border, theme().text_dim);
  const bool ch = ImGui::Checkbox(id, b);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  return ch;
}

void draw_toggle(const Opt& o) {
  bool b = cur_of(o) != "0" && !cur_of(o).empty();
  if (boxed_checkbox("##v", &b)) set_of(o, b ? "1" : "0");
}

void draw_path(const Opt& o) {
  auto it = g_text.find(o.key);
  if (it == g_text.end()) it = g_text.emplace(o.key, cur_of(o)).first;
  char buf[520];
  std::snprintf(buf, sizeof buf, "%s", it->second.c_str());
  ImGui::SetNextItemWidth(px(420));
  if (ImGui::InputText("##v", buf, sizeof buf)) it->second = buf;
  if (ImGui::IsItemDeactivatedAfterEdit()) set_of(o, it->second);
}

void draw_row(const Opt& o, bool inline_help) {
  if (o.kind == kSection) {
    ImGui::Spacing();
    ImGui::TextColored(theme().title, "%s", o.label);
    ImGui::Separator();
    return;
  }
  ImGui::PushID(o.key);
  ImGui::BeginGroup();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(o.label);
  ImGui::SameLine(px(275));
  ImGui::BeginDisabled(o.when == kSoon);
  switch (o.kind) {
    case kChoice: draw_choice(o); break;
    case kSlider: draw_slider(o); break;
    case kToggle: draw_toggle(o); break;
    case kPath: draw_path(o); break;
    default: break;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused() || ImGui::IsItemActive()) g_help = &o;
  badge(o);
  ImGui::EndGroup();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) g_help = &o;
  if (env_set_by_hand(o)) ImGui::TextColored(theme().error, "    %s is set in the environment and wins over this choice.", st::env_name(o.key) ? st::env_name(o.key) : o.env);
  if (inline_help) {
    ImGui::Indent(px(24));
    ImGui::PushStyleColor(ImGuiCol_Text, theme().text_dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(o.help);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(px(24));
    ImGui::Spacing();
  }
  ImGui::PopID();
}

std::string default_label(const Opt& o) {
  if (o.dyn) return "";
  for (auto& c : o.choices) if (c.first == o.def) return c.second;
  if (o.kind == kToggle) return std::string(o.def) == "0" ? "Off" : "On";
  if (o.kind == kSlider) { char b[48]; std::snprintf(b, sizeof b, o.fmt, (float)std::atof(o.def)); return b; }
  return o.def;
}

void draw_help(const std::vector<Opt>& list, const Opt** remembered) {
  const Theme& t = theme();
  const Opt* o = g_help ? g_help : *remembered;
  if (!o) for (auto& x : list) if (x.kind != kSection) { o = &x; break; }
  *remembered = o;
  if (!o) return;
  ImGui::TextColored(t.title, "%s", o->label);
  ImGui::Spacing();
  ImGui::TextWrapped("%s", o->help);
  ImGui::Spacing();
  auto wrapped = [](const ImVec4& c, const char* fmt, const char* a) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text(fmt, a);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  };
  if (o->when == kSoon) wrapped(t.text_dim, "Coming soon: not in this build yet, so the control is off.%s", "");
  else if (o->when == kLive) wrapped(t.good, "Applies now.%s", "");
  else wrapped(t.title, "Restart needed: use \"Apply and restart\" at the top.%s", "");
  const std::string dl = default_label(*o);
  if (!dl.empty()) wrapped(t.text_dim, "Default: %s", dl.c_str());
  if (o->key && std::strcmp(o->key, "gfx_render_scale") == 0) {
    const float s = (float)std::atof(cur_of(*o).c_str());
    char b[64];
    std::snprintf(b, sizeof b, "%d x %d", (int)std::lround(1920 * s), (int)std::lround(1080 * s));
    wrapped(t.text_dim, "Now: scene drawn at %s", b);
  }
  const char* en = st::env_name(o->key) ? st::env_name(o->key) : o->env;
  if (en) { wrapped(t.text_dim, "%s", "Override with the environment variable:"); wrapped(t.text_dim, "%s", en); }
}

// Two-pane page: `extra` draws below the options in the left pane (the shader cache on the Graphics tab).
void two_pane(const char* id, std::vector<Opt>& list, const Opt** remembered, void (*extra)()) {
  g_help = nullptr;
  const float w = ImGui::GetContentRegionAvail().x;
  const float lw = w * 0.64f;
  if (ImGui::BeginChild(id, ImVec2(lw, 0))) {
    if (g_scroll_req >= 0) { ImGui::SetScrollY(g_scroll_req * ImGui::GetScrollMaxY()); g_scroll_req = -1; }
    for (const Opt& o : list) draw_row(o, false);
    if (extra) extra();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().panel_alt);
  if (ImGui::BeginChild((std::string(id) + "_help").c_str(), ImVec2(0, 0), ImGuiChildFlags_Borders)) draw_help(list, remembered);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ---- mods ------------------------------------------------------------------------------------------------------------
struct ModRow {
  std::string id, name, version, author, description;
  bool enabled = true, has_json = true, exists = true;
};
double now_secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::vector<ModRow> g_mods;
double g_mods_loaded = -100;
std::string g_mods_root;
std::string g_mods_msg;
bool g_mods_err = false;

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void mods_reload() {
  g_mods_loaded = now_secs();
  g_mods_root = mods::default_root();
  g_mods.clear();
  std::error_code ec;
  std::vector<std::pair<std::string, bool>> order;
  bool have_file = false;
  hle::Json j;
  std::string err, txt = read_all(fs::path(g_mods_root) / "mods.json");
  if (!txt.empty() && hle::json_parse(txt, j, err)) {
    have_file = true;
    if (auto* arr = j.get("packs"))
      for (auto& e : arr->a) {
        if (e.type == hle::Json::Str) order.push_back({e.s, true});
        else if (e.type == hle::Json::Obj) {
          std::string id = e.get("id") ? e.get("id")->str() : e.get("folder") ? e.get("folder")->str() : "";
          if (!id.empty()) order.push_back({id, e.get("enabled") ? e.get("enabled")->boolean(true) : true});
        }
      }
  }
  std::vector<std::string> disk;
  if (fs::is_directory(g_mods_root, ec))
    for (auto& d : fs::directory_iterator(g_mods_root, ec)) if (d.is_directory(ec)) disk.push_back(d.path().filename().string());
  std::sort(disk.begin(), disk.end());
  auto listed = [&](const std::string& id) { for (auto& p : order) if (p.first == id) return true; return false; };
  for (auto& d : disk)   // folders the loader would pick up on its own (pack.json), after the ones in mods.json
    if (!listed(d) && fs::exists(fs::path(g_mods_root) / d / "pack.json", ec)) order.push_back({d, !have_file});
  for (auto& [id, en] : order) {
    ModRow r;
    r.id = id;
    r.enabled = en;
    const fs::path dir = fs::path(g_mods_root) / id;
    r.exists = fs::is_directory(dir, ec);
    r.name = id;
    hle::Json pj;
    std::string perr, ptxt = read_all(dir / "pack.json");
    r.has_json = !ptxt.empty();
    if (r.has_json && hle::json_parse(ptxt, pj, perr)) {
      if (auto* v = pj.get("name")) if (!v->str().empty()) r.name = v->str();
      if (auto* v = pj.get("version")) r.version = v->str();
      if (auto* v = pj.get("author")) r.author = v->str();
      if (auto* v = pj.get("description")) r.description = v->str();
    }
    g_mods.push_back(r);
  }
}

// What the loader reads next start: ids and enabled flags in priority order.
std::string mods_state() {
  std::string s;
  for (auto& r : g_mods) if (r.exists) s += (s.empty() ? "" : ",") + r.id + (r.enabled ? ":1" : ":0");
  return s;
}

bool mods_save() {
  hle::Json j = hle::Json::object();
  hle::Json arr = hle::Json::array();
  for (auto& r : g_mods) {
    hle::Json e = hle::Json::object();
    e.set("id", hle::Json::string(r.id));
    e.set("enabled", hle::Json::boolean_(r.enabled));
    arr.a.push_back(e);
  }
  j.set("packs", arr);
  std::string out;
  hle::json_dump(j, out);
  out += "\n";
  std::error_code ec;
  fs::create_directories(g_mods_root, ec);
  const fs::path p = fs::path(g_mods_root) / "mods.json", tmp = fs::path(g_mods_root) / "mods.json.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << out;
    if (!f.good()) { g_mods_msg = "Could not write mods.json in " + g_mods_root; g_mods_err = true; return false; }
  }
  fs::rename(tmp, p, ec);
  if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
  if (ec) { g_mods_msg = "Could not replace mods.json: " + ec.message(); g_mods_err = true; return false; }
  g_mods_msg = "Saved mods.json. The change applies after a restart.";
  g_mods_err = false;
  return true;
}

void open_folder(const std::string& dir) {
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  ShellExecuteW(nullptr, L"explore", fs::u8path(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

// ---- pages -----------------------------------------------------------------------------------------------------------
void options_menu_opened() { capture_start(); }

int options_pending_restart(std::string* names) {
  capture_start();
  Pend p;
  for_each_opt(pend_one, &p);
  if (mods_state() != g_mods_start) p.names.push_back("Mods");
  if (names) {
    names->clear();
    for (size_t i = 0; i < p.names.size(); ++i) *names += (i ? ", " : "") + p.names[i];
  }
  return (int)p.names.size();
}

void options_restart_bar() {
  std::string names;
  const int n = options_pending_restart(&names);
  if (!n) return;
  const Theme& t = theme();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, t.toast_bg);
  if (ImGui::BeginChild("pending", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.6f), ImGuiChildFlags_Borders)) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - px(250));
    std::string shown = names;
    size_t c1 = 0;
    for (int k = 0; k < 3 && c1 != std::string::npos; ++k) c1 = names.find(", ", c1 == 0 && k == 0 ? 0 : c1 + 2);
    if (c1 != std::string::npos) shown = names.substr(0, c1) + " and " + std::to_string(n - 3) + " more";
    ImGui::TextColored(t.conflict, "%d change%s need%s a restart: %s", n, n == 1 ? "" : "s", n == 1 ? "s" : "", shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine(ImGui::GetWindowWidth() - px(240));
    if (ImGui::Button("Apply and restart")) restart_game();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void tab_graphics() {
  static const Opt* remembered = nullptr;
  two_pane("gfx", graphics_opts(), &remembered, [] {
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Shader cache and compile settings")) shader_cache_section();
  });
}

void tab_display() {
  static const Opt* remembered = nullptr;
  two_pane("disp", display_opts(), &remembered, nullptr);
}

void tab_photos() {
  static const Opt* remembered = nullptr;
  two_pane("photos", photo_opts(), &remembered, [] {
    ImGui::Spacing();
    if (ImGui::Button("Open PNG folder")) {
      std::string d = st::get("photo_dir", "");
      if (d.empty()) { const char* a = std::getenv("APPDATA"); d = std::string(a ? a : ".") + "\\SnapRecomp\\photos"; }
      open_folder(d);
    }
    ImGui::SameLine();
    ImGui::TextColored(theme().text_dim, "Opens the folder in Explorer.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    gallery_section();
  });
}

void audio_output_section() {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(theme().title, "Output");
  for (const Opt& o : audio_opts()) draw_row(o, true);
}

void tab_mods() {
  const Theme& t = theme();
  if (now_secs() - g_mods_loaded > 3.0) mods_reload();
  ImGui::TextWrapped("Mod packs are folders in %s. A pack higher in the list wins when two packs replace the same game file. The loader reads this list when the game starts, so every change here needs a restart.",
                     g_mods_root.c_str());
  if (const char* e = std::getenv("SNAP_MODS_DIR")) if (*e) ImGui::TextColored(t.conflict, "SNAP_MODS_DIR is set in the environment: this is the folder it points to.");
  ImGui::Spacing();
  if (ImGui::Button("Open mods folder")) open_folder(g_mods_root);
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) mods_reload();
  ImGui::SameLine();
  ImGui::TextColored(t.conflict, "restart needed");
  if (!g_mods_msg.empty()) ImGui::TextColored(g_mods_err ? t.error : t.good, "%s", g_mods_msg.c_str());
  ImGui::Spacing();
  ImGui::Separator();
  if (g_mods.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("No packs found. A pack is a folder with a pack.json next to its <archive>.drp\\<entry> files; see docs\\mods.md.");
    return;
  }
  int move_from = -1, move_to = -1;
  bool changed = false;
  for (int i = 0; i < (int)g_mods.size(); ++i) {
    ModRow& r = g_mods[i];
    ImGui::PushID(i);
    ImGui::BeginDisabled(i == 0);
    if (ImGui::ArrowButton("up", ImGuiDir_Up)) { move_from = i; move_to = i - 1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == (int)g_mods.size());
    if (ImGui::ArrowButton("down", ImGuiDir_Down)) { move_from = i; move_to = i + 1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!r.exists);
    if (boxed_checkbox("##en", &r.enabled)) changed = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(r.enabled ? t.text : t.text_dim, "%d. %s", i + 1, r.name.c_str());
    if (!r.version.empty()) { ImGui::SameLine(); ImGui::TextColored(t.text_dim, "v%s", r.version.c_str()); }
    if (!r.author.empty()) { ImGui::SameLine(); ImGui::TextColored(t.text_dim, "by %s", r.author.c_str()); }
    ImGui::SameLine();
    ImGui::TextColored(r.enabled ? t.good : t.text_dim, r.enabled ? "on" : "off");
    ImGui::Indent(px(70));
    if (!r.exists) ImGui::TextColored(t.error, "Listed in mods.json but the folder %s is missing.", r.id.c_str());
    else if (!r.has_json) ImGui::TextColored(t.conflict, "Folder \"%s\" has no pack.json (name and version unknown).", r.id.c_str());
    else if (!r.description.empty()) ImGui::TextColored(t.text_dim, "%s", r.description.c_str());
    ImGui::Unindent(px(70));
    ImGui::PopID();
  }
  if (move_from >= 0 && move_to >= 0 && move_to < (int)g_mods.size()) { std::swap(g_mods[move_from], g_mods[move_to]); changed = true; }
  if (changed) mods_save();
}

// ---- scripted-test entry points (ui_test.cpp): the same code the controls run
namespace {
const Opt* find_opt(const std::string& key) {
  for (auto* list : {&graphics_opts(), &display_opts(), &photo_opts(), &audio_opts()})
    for (const Opt& o : *list) if (o.kind != kSection && key == o.key) return &o;
  return nullptr;
}
}  // namespace

bool options_test_set(const std::string& key, const std::string& value) {
  capture_start();
  const Opt* o = find_opt(key);
  if (!o || o->when == kSoon) return false;
  set_of(*o, value);
  return true;
}
void options_test_scroll(float fraction) { g_scroll_req = fraction; }
std::string options_test_get(const std::string& key) {
  const Opt* o = find_opt(key);
  return o ? cur_of(*o) : "<unknown option>";
}
bool options_test_option_when(const std::string& key, std::string* when) {
  const Opt* o = find_opt(key);
  if (!o) return false;
  *when = o->when == kLive ? "live" : o->when == kRestart ? "restart" : "soon";
  return true;
}
bool options_test_mods(const std::string& op, const std::string& id, int arg, std::string* state) {
  capture_start();
  mods_reload();
  if (op == "toggle" || op == "move") {
    int i = -1;
    for (int k = 0; k < (int)g_mods.size(); ++k) if (g_mods[k].id == id) i = k;
    if (i < 0) return false;
    if (op == "toggle") g_mods[i].enabled = !g_mods[i].enabled;
    else {
      int j = i + arg;
      if (j < 0 || j >= (int)g_mods.size()) return false;
      std::swap(g_mods[i], g_mods[j]);
    }
    if (!mods_save()) return false;
  }
  *state = mods_state();
  return true;
}

}  // namespace snap::ui
