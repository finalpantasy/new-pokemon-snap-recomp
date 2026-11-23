// SPDX-License-Identifier: MIT
// The themed launcher window (see launcher_ui.h). One Win32 window, a D3D11 swap chain and Dear ImGui used only as a draw-list and input
// layer: every shape is drawn here in a 1280 x 720 design space scaled to the window.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "launcher_ui.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <algorithm>
#include <ctime>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include "album.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "hle/input_core.h"
#include "hle/json_lite.h"
#include "hle/picture_rows.h"
#include "hle/presets.h"
#include "hle/discord_app.h"
#include "hle/visual_defaults.h"
#include "steamdeck.h"
#include "mods/mods_core.h"
#include "mods/mods_thumb.h"
#include "shaderprep.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "windowscodecs")

namespace fs = std::filesystem;

namespace snap_launcher {
namespace {

// ---------------------------------------------------------------------------------------------------------------- files
std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
fs::path P(const std::string& s) { return fs::path(W(s)); }

bool write_atomic(const std::string& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(P(path).parent_path(), ec);
  const std::wstring tmp = P(path).wstring() + L".tmp";
  {
    std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
    f << text;
    f.flush();
    if (!f.good()) { f.close(); fs::remove(fs::path(tmp), ec); return false; }
  }
  bool ok = false;
  for (int i = 0; i < 5 && !ok; ++i) {
    ok = MoveFileExW(tmp.c_str(), P(path).wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) Sleep(20);
  }
  if (!ok) fs::remove(fs::path(tmp), ec);
  return ok;
}

// settings.cfg of the profile: key=value lines, '#' comments (the same file hle::settings reads and writes).
struct CfgFile {
  std::string path;
  std::map<std::string, std::string> kv;
  void load() {
    kv.clear();
    std::ifstream f(P(path));
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  void save() {
    std::string t = "# per-profile settings (edited by the F1 menu)\n";
    for (auto& e : kv) t += e.first + "=" + e.second + "\n";
    write_atomic(path, t);
  }
  std::string get(const std::string& k, const std::string& d) const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
  void set(const std::string& k, const std::string& v) { kv[k] = v; }
  void erase(const std::string& k) { kv.erase(k); }
};

// window.json and audio.json: one JSON object, read, changed, written back with every other key kept.
struct JsonFile {
  std::string path;
  hle::Json j = hle::Json::object();
  void load() {
    j = hle::Json::object();
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    hle::Json x;
    std::string err;
    if (hle::json_parse(ss.str(), x, err) && x.type == hle::Json::Obj) j = x;
  }
  void save() {
    std::string out;
    hle::json_dump(j, out);
    write_atomic(path, out + "\n");
  }
};

// ---------------------------------------------------------------------------------------------------------------- settings model
enum Store { S_CFG, S_WIN, S_AUD, S_BAK };   // S_BAK: backup.json in the data folder (the profile backups, runtime/hle/sync_backup.h)
enum Kind { K_CHOICE, K_PRESET, K_ACTION, K_LOOK, K_PACING, K_AA, K_PERF, K_PROFILE, K_DEVICE, K_INFO };   // K_INFO: a read-only row (label and a line of text in cur), e.g. the key bindings summary   // K_PRESET / K_LOOK / K_PACING / K_AA / K_PERF / K_PROFILE are worked out from or write several stored values
enum Enc { E_STR, E_INT, E_VOL, E_BOOL };   // how a window.json / audio.json value is stored

struct Ch {
  std::string v, label;
  bool avail = true;
};

struct Item {
  std::string label, key, help, def;
  Store store = S_CFG;
  Kind kind = K_CHOICE;
  Enc enc = E_STR;
  std::vector<Ch> ch;
  UiResult act = UiResult::Quit;   // K_ACTION
  std::string cur;
  std::string note;   // one line under the label: which value is in effect when another row overrides this one (hle/picture_rows.h)
  std::string brief;  // one short line under the label saying what the row does (the full help shows in the bar at the bottom for the selected row)
};

struct Page {
  std::string name;
  std::vector<Item> items;
};

// One loose-file mod pack (docs/mods.md): a folder in the mods root with a pack.json, listed in mods.json.
struct ModPack {
  std::string id, name, version, author, desc;
  bool enabled = false;
  bool zip = false;   // <id>.zip in the mods folder rather than a folder
};

// The quality preset is not stored; it is worked out from these rows (the same table and values as runtime/ui/ui_options.cpp: keep them in step).
struct PresetKey { const char* key; const char* def; };
const PresetKey kPresetKeys[] = {
    {"gfx_render_scale", "auto"}, {"gfx_aa", "default"}, {"gfx_sharpen", nullptr}, {"gfx_aniso", nullptr}, {"gfx_lod_bias", "auto"},
    {"gfx_shadow_res", "default"}, {"ssao_quality", "default"}, {"gfx_ssao_samples", "default"}, {"gfx_ssao_hbao", "default"},
    {"gfx_pass_bloom", "default"}, {"gfx_pass_dof", "default"}, {"gfx_shadow_dist", "default"},
};
constexpr int kPresetKeyN = 12;
struct PresetDef { const char* id; const char* label; const char* v[kPresetKeyN]; };
const PresetDef kPresets[] = {
    {"verylow", "Very Low", {"0.50", "none", "0.00", "0", "1", "512", "", "4", "", "off", "off", ""}},
    {"low", "Low", {"", "off", "0.00", "4", "", "", "", "8", "", "", "", ""}},
    {"normal", "Default", {"", "", "", "", "", "", "", "", "", "", "", ""}},
    {"high", "High", {"", "ssaa2,smaa", "0.30", "16", "", "2048", "high", "", "", "", "", "2"}},
    {"ultra", "Ultra", {"2.00", "smaa", "0.30", "16", "", "4096", "ultra", "", "", "", "", "4"}},
    {"maximum", "Maximum", {"4.00", "smaa", "0.50", "16", "", "8192", "ultra", "512", "32,128", "", "", "8"}},
};
std::string preset_def(int k) {
  if (kPresetKeys[k].def) return kPresetKeys[k].def;
  return k == 2 ? snapdef::def("SNAP_SHARPEN") : snapdef::def("SNAP_ANISO");
}
bool preset_eq(int k, const std::string& a, const std::string& b) {
  if (a == b) return true;
  if (k == 0 && (a == "auto" || b == "auto")) return false;   // Automatic and a typed 1.00 are different rows
  if (k == 0 || k == 2) return std::fabs((float)std::atof(a.c_str()) - (float)std::atof(b.c_str())) < 1e-4f;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------- launcher state
struct G {
  UiInfo* info = nullptr;
  HWND hwnd = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain* sc = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  UINT rw = 0, rh = 0;
  bool resize = false, done = false;
  UiResult result = UiResult::Quit;
  ImFont* font = nullptr;
  ImFont* art_font = nullptr;    // for the Steam artwork: a system font, never the game's own
  float s = 1, ox = 0, oy = 0;   // design space -> window pixels
  float tb = 0;                  // height of our own title bar in window pixels (the system frame is removed: WM_NCCALCSIZE)

  // the display the game will use (read when the window opens and when the Monitor row changes)
  int disp_w = 1920, disp_h = 1080, disp_hz = 60;
  std::string gpu_name;
  bool gpu_rtx = false, gpu_nvidia = false, pad_found = false;

  // Add to Steam
  enum SteamState { ST_NONE, ST_ASK, ST_WORKING, ST_DONE, ST_FAIL } steam_state = ST_NONE;
  std::string steam_msg;
  int steam_status = 0;          // 0 Steam not found, 1 found and not added, 2 added
  bool steam_running = false, steam_work = false;
  bool block_hot = false;        // a dialog is open: the screen under it ignores the mouse

  CfgFile cfg;
  JsonFile win, aud, bak;
  std::vector<Page> pages;
  size_t profile_idx = 0;

  // Mods page and the shader-cache chip
  std::vector<ModPack> mods;                          // visible packs in priority order (the first wins)
  std::vector<std::pair<std::string, bool>> mods_keep;   // entries mods.json keeps that the page does not show (hidden system packs, missing folders)
  std::string mods_root;
  int mods_top = 0;
  int cache_state = 0;           // 0 missing, 1 ready, 2 building
  unsigned long long cache_bytes = 0;
  std::string cache_dir;
  snap_shaderprep::Progress prep;   // <shadercache>\prepare.json (host/shaderprep.cpp): the first-run "prepare shaders" sweep
  double prep_at = 0;               // ImGui time of the last refresh_cache()
  bool prep_driver = false;         // the stale reason is a new graphics driver (the driver's own shader cache does not carry over)
  bool prep_stale = false;          // prepared, but with other shadow / foliage / parallax / effect settings than the player has now

  enum View { HOME, SETTINGS, SAVES, STEAM, MODS, ALBUM, MODCMP } view = HOME;
  int home_sel = 0, tab = 0, row = 0;
  bool pulldown = false;
  int pd_sel = 0, pd_top = 0;
  bool confirm_reset = false;
  bool bindings_custom = false;   // input.json holds the player's own bindings (Controls page caption)
  double t0 = 0;
  std::string toast;
  double toast_until = 0;
};
G g;

// design-space helpers ---------------------------------------------------------------------------------------------
ImVec2 V(float x, float y) { return ImVec2(g.ox + x * g.s, g.oy + y * g.s); }
ImU32 C(unsigned hex, int a = 255) { return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, a); }
// The palette is variable so the Accessibility page's high-contrast row can swap it (apply_access below).
unsigned kCream = 0xf4f2f0, kWhite = 0xfcfcfc, kPanel = 0x6e7180, kTab = 0x9a9da7, kRow = 0x989899, kGreen = 0xb6d291, kYellow = 0xf8dc56,
         kOrange = 0xf8ab79, kOrDark = 0xda743a, kText = 0x646464, kBrown = 0x4f3d29, kGrey = 0x8b8a8b;
bool g_hc = false;

float text_w(float sz, const char* t) { return g.font->CalcTextSizeA(sz * g.s, FLT_MAX, 0.0f, t).x / g.s; }
// Draws text with its left edge (al 0), centre (1) or right edge (2) at x and its vertical centre at y, in design units.
void text(float x, float y, float sz, ImU32 col, const char* t, int al = 0) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 ts = g.font->CalcTextSizeA(sz * g.s, FLT_MAX, 0.0f, t);
  float px = g.ox + x * g.s;
  if (al == 1) px -= ts.x * 0.5f;
  else if (al == 2) px -= ts.x;
  dl->AddText(g.font, sz * g.s, ImVec2(px, g.oy + y * g.s - ts.y * 0.5f), col, t);
}
void text_clip(float x, float y, float sz, ImU32 col, const char* t, float maxw, int al = 0) {   // shortens with ".." when wider than maxw
  std::string s = t;
  if (text_w(sz, s.c_str()) > maxw) {
    while (s.size() > 3 && text_w(sz, (s + "..").c_str()) > maxw) s.pop_back();
    s += "..";
  }
  text(x, y, sz, col, s.c_str(), al);
}
// Word-wraps into at most two lines of maxw (design units) and draws them centred on y.
void text_wrap2(float x, float y, float sz, ImU32 col, const std::string& t, float maxw) {
  if (text_w(sz, t.c_str()) <= maxw) { text(x, y, sz, col, t.c_str()); return; }
  std::string a, b;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t sp = t.find(' ', pos);
    std::string word = t.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
    std::string trial = a.empty() ? word : a + " " + word;
    if (b.empty() && text_w(sz, trial.c_str()) <= maxw) a = trial;
    else b = b.empty() ? word : b + " " + word;
    if (sp == std::string::npos) break;
    pos = sp + 1;
  }
  text(x, y - sz * 0.62f, sz, col, a.c_str());
  text_clip(x, y + sz * 0.62f, sz, col, b.c_str(), maxw);
}
// Word-wraps t into lines of at most maxw, up to maxlines, the first line's vertical centre at y, lines lineh apart. Returns the number of lines drawn.
int text_para(float x, float y, float sz, ImU32 col, const std::string& t, float maxw, float lineh, int maxlines) {
  size_t pos = 0;
  int n = 0;
  while (pos < t.size() && n < maxlines) {
    std::string a;
    while (pos < t.size()) {
      const size_t sp = t.find(' ', pos);
      const std::string word = t.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
      const std::string trial = a.empty() ? word : a + " " + word;
      if (!a.empty() && text_w(sz, trial.c_str()) > maxw) break;
      a = trial;
      pos = sp == std::string::npos ? t.size() : sp + 1;
    }
    text(x, y + (float)n * lineh, sz, col, a.c_str());
    ++n;
  }
  return n;
}

// A bar with pointed ends (the game's rows and value boxes).
void pointed(float x, float y, float w, float h, float nose, ImU32 col, bool left_point = true) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 p[6] = {V(x + (left_point ? nose : 0), y), V(x + w - nose, y), V(x + w, y + h / 2), V(x + w - nose, y + h),
                 V(x + (left_point ? nose : 0), y + h), V(x + (left_point ? 0 : 0), y + h / 2)};
  if (left_point) { dl->AddConvexPolyFilled(p, 6, col); }
  else {
    ImVec2 q[5] = {V(x, y), V(x + w - nose, y), V(x + w, y + h / 2), V(x + w - nose, y + h), V(x, y + h)};
    dl->AddConvexPolyFilled(q, 5, col);
  }
}
void rrect(float x, float y, float w, float h, float r, ImU32 col, ImDrawFlags fl = 0) {
  ImGui::GetForegroundDrawList()->AddRectFilled(V(x, y), V(x + w, y + h), col, r * g.s, fl);
}
void shadow(float x, float y, float w, float h, float r) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (int i = 0; i < 6; ++i) {
    const float gr = 3.0f + i * 3.5f;
    dl->AddRectFilled(V(x - gr, y - gr + 8), V(x + w + gr, y + h + gr + 8), IM_COL32(60, 45, 30, 5), (r + gr) * g.s);
  }
}
// The yellow diamond pattern of the selected value box.
void pattern(float x, float y, float w, float h) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(V(x, y), V(x + w, y + h), true);
  const float cell = h;
  for (float cx = x - cell; cx < x + w + cell; cx += cell) {
    ImVec2 a[3] = {V(cx, y), V(cx + cell / 2, y + h / 2), V(cx, y + h)};
    ImVec2 b[3] = {V(cx + cell, y), V(cx + cell, y + h), V(cx + cell / 2, y + h / 2)};
    dl->AddConvexPolyFilled(a, 3, C(0xf1b93a, 120));
    dl->AddConvexPolyFilled(b, 3, C(0xf1b93a, 120));
  }
  dl->PopClipRect();
}
// Orange A / B chevron prompt: x,y = left edge and centre, h = height.
void prompt(float x, float y, float h, const char* glyph) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float w = h * 1.55f, nose = h * 0.45f;
  ImVec2 p[5] = {V(x, y - h / 2), V(x + w - nose, y - h / 2), V(x + w, y), V(x + w - nose, y + h / 2), V(x, y + h / 2)};
  dl->AddConvexPolyFilled(p, 5, C(kOrange));
  const ImVec2 c = V(x + (w - nose) * 0.52f, y);
  dl->AddCircleFilled(c, h * 0.33f * g.s, C(kCream), 24);
  const ImVec2 ts = g.font->CalcTextSizeA(h * 0.42f * g.s, FLT_MAX, 0, glyph);
  dl->AddText(g.font, h * 0.42f * g.s, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), C(kOrDark), glyph);
}
// The shoulder badge: a pointed orange tab with a white tile (L on the left edge, R on the right).
void shoulder(bool left, float x, float y, float w, float h) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float n = h * 0.55f;
  if (left) { ImVec2 p[5] = {V(x, y + h / 2), V(x + n, y), V(x + w, y), V(x + w, y + h), V(x + n, y + h)}; dl->AddConvexPolyFilled(p, 5, C(kOrange)); }
  else { ImVec2 p[5] = {V(x, y), V(x + w - n, y), V(x + w, y + h / 2), V(x + w - n, y + h), V(x, y + h)}; dl->AddConvexPolyFilled(p, 5, C(kOrange)); }
  const float tx = left ? x + w * 0.42f : x + w * 0.1f;
  rrect(tx, y + h * 0.16f, h * 0.7f, h * 0.68f, 6, C(kCream));
  text(tx + h * 0.35f, y + h / 2, h * 0.5f, C(kOrDark), left ? "L" : "R", 1);
}
void diamond(float cx, float cy, float r, ImU32 col) {
  ImVec2 p[4] = {V(cx, cy - r), V(cx + r, cy), V(cx, cy + r), V(cx - r, cy)};
  ImGui::GetForegroundDrawList()->AddConvexPolyFilled(p, 4, col);
}
// Our own lens-and-aperture mark (original art): a ring and six blades.
void aperture(float cx, float cy, float r, ImU32 col) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddCircle(V(cx, cy), r * g.s, col, 48, r * 0.16f * g.s);
  for (int i = 0; i < 6; ++i) {
    const float a = (float)i * 1.0471976f;
    const ImVec2 p0 = V(cx + std::cos(a) * r * 0.30f, cy + std::sin(a) * r * 0.30f);
    const ImVec2 p1 = V(cx + std::cos(a + 0.55f) * r * 0.78f, cy + std::sin(a + 0.55f) * r * 0.78f);
    const ImVec2 p2 = V(cx + std::cos(a + 1.0471976f) * r * 0.30f, cy + std::sin(a + 1.0471976f) * r * 0.30f);
    ImVec2 t[3] = {p0, p1, p2};
    dl->AddConvexPolyFilled(t, 3, col);
  }
}

// ---------------------------------------------------------------------------------------------------------------- values
std::string num_text(double v, Enc e) {
  char b[32];
  if (e == E_VOL) std::snprintf(b, sizeof b, "%.1f", std::round(v * 10.0) / 10.0);
  else std::snprintf(b, sizeof b, "%d", (int)std::lround(v));
  return b;
}

std::string get_preset() {
  for (const PresetDef& p : kPresets) {
    bool all = true;
    for (int k = 0; k < kPresetKeyN && all; ++k) {
      const std::string want = p.v[k][0] ? std::string(p.v[k]) : preset_def(k);
      all = preset_eq(k, g.cfg.get(kPresetKeys[k].key, preset_def(k)), want);
    }
    if (all) return p.id;
  }
  return "custom";
}
void set_preset(const std::string& v) {
  for (const PresetDef& p : kPresets) {
    if (v != p.id) continue;
    for (int k = 0; k < kPresetKeyN; ++k) {
      if (p.v[k][0]) g.cfg.set(kPresetKeys[k].key, p.v[k]);
      else g.cfg.erase(kPresetKeys[k].key);
    }
    g.cfg.save();
    return;
  }
}

// The Device row (host/steamdeck.h, docs/steam-deck.md): "steamdeck" writes the whole Steam Deck table, "desktop" erases it again. The row shows

// Steam Deck only while every value of the table is stored, so changing any one of them afterwards shows Desktop.

std::string get_device() {

  for (const snapdeck::Kv& kv : snapdeck::cfg()) if (g.cfg.get(kv.key, "") != kv.val) return "desktop";

  const hle::Json* m = g.win.j.get("mode");

  const hle::Json* w = g.win.j.get("width");

  const hle::Json* h = g.win.j.get("height");

  if (!m || m->type != hle::Json::Str || m->s != snapdeck::window_mode()) return "desktop";

  if (!w || w->type != hle::Json::Num || (int)w->n != snapdeck::kWidth || !h || h->type != hle::Json::Num || (int)h->n != snapdeck::kHeight) return "desktop";

  return "steamdeck";

}

void set_device(const std::string& v) {

  if (v == "steamdeck") {

    for (const snapdeck::Kv& kv : snapdeck::cfg()) g.cfg.set(kv.key, kv.val);

    g.win.j.set("mode", hle::Json::string(snapdeck::window_mode()));

    g.win.j.set("width", hle::Json::number(snapdeck::kWidth));

    g.win.j.set("height", hle::Json::number(snapdeck::kHeight));

    g.win.j.set("maximized", hle::Json::boolean_(false));

  } else if (v == "desktop") {

    for (const snapdeck::Kv& kv : snapdeck::cfg()) g.cfg.erase(kv.key);

    g.win.j.set("mode", hle::Json::string("windowed"));

  } else return;

  g.cfg.save();

  g.win.save();

}



// ---- the display (for VRR and "fit to display") ----------------------------------------------------------------------
BOOL CALLBACK mon_handle_cb(HMONITOR m, HDC, LPRECT, LPARAM lp) { ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE; }
void read_display() {
  std::vector<HMONITOR> ms;
  EnumDisplayMonitors(nullptr, nullptr, mon_handle_cb, (LPARAM)&ms);
  int idx = -1;
  if (const hle::Json* x = g.win.j.get("monitor")) if (x->type == hle::Json::Num) idx = (int)x->n;
  HMONITOR m = (idx >= 0 && idx < (int)ms.size()) ? ms[(size_t)idx] : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof mi;
  DEVMODEW dm{};
  dm.dmSize = sizeof dm;
  if (GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
    if (dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) { g.disp_w = (int)dm.dmPelsWidth; g.disp_h = (int)dm.dmPelsHeight; }
    g.disp_hz = dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 60;
  }
}
// VRR = vsync on plus a frame limit just under the refresh rate, so a G-SYNC / FreeSync screen never reaches the top of its range (the usual "refresh minus 3").
int vrr_cap() { return std::max(30, g.disp_hz - 3); }
// "Fit to display": the render scale whose 16:9 picture is as tall as the screen (rounded to 0.05, the same range the Render scale row has).
std::string fit_scale() {
  const float s = std::min(4.0f, std::max(0.5f, std::round((float)g.disp_h / 1080.0f * 20.0f) / 20.0f));
  char b[16];
  std::snprintf(b, sizeof b, "%.2f", s);
  return b;
}
std::string fit_label() {
  const float s = (float)std::atof(fit_scale().c_str());
  return "Fit to display (" + std::to_string((int)std::lround(1920 * s)) + " x " + std::to_string((int)std::lround(1080 * s)) + ")";
}
int win_cap() { const hle::Json* x = g.win.j.get("frame_cap"); return x && x->type == hle::Json::Num ? (int)x->n : 0; }
void win_set_cap(int v) { g.win.j.set("frame_cap", hle::Json::number(v)); g.win.save(); }

std::string get_pacing() {
  const std::string vs = g.cfg.get("vsync", "on");
  const int cap = win_cap();
  if (vs == "off" && cap == 0) return "off";
  if (vs == "on" && cap == 0) return "vsync";
  if (vs == "on" && cap == vrr_cap()) return "vrr";
  return "custom";
}
void set_pacing(const std::string& v) {
  if (v == "off") { g.cfg.set("vsync", "off"); g.cfg.save(); win_set_cap(0); }
  else if (v == "vsync") { g.cfg.erase("vsync"); g.cfg.save(); win_set_cap(0); }
  else if (v == "vrr") { g.cfg.erase("vsync"); g.cfg.save(); win_set_cap(vrr_cap()); }
}

// ---- the Look (Enhanced / Original): the stored key and the rows it clears are the ones in hle/visual_defaults.h ----
std::string get_look() {
  const std::string l = g.cfg.get("gfx_look", "");
  if (l == "original" || l == "enhanced") return l;
  return snapdef::kDefaultEnhanced ? "enhanced" : "original";
}
void apply_look_to_defaults() {   // the visual rows' defaults follow the Look (the game does the same from the saved key on start)
  const std::string l = g.cfg.get("gfx_look", "");
  snapdef::saved_look().store(l == "original" ? 0 : l == "enhanced" ? 1 : -1, std::memory_order_relaxed);
}
void set_look(const std::string& v) {
  if (v == "original" || !snapdef::kDefaultEnhanced) g.cfg.set("gfx_look", v);
  else g.cfg.erase("gfx_look");
  int n = 0;
  const char* const* keys = snapdef::look_keys(&n);
  for (int i = 0; i < n; ++i) g.cfg.erase(keys[i]);
  g.cfg.save();
  apply_look_to_defaults();
}

// ---- the picture rows (Internal resolution / Anti-aliasing / Performance scaling): views over gfx_render_scale, gfx_aa, gfx_upscaler and gfx_upscale_quality
// (the pure rules are in hle/picture_rows.h, which tests/hle/picture_rows_test.cpp checks)
bool file_exists(const std::string& rel) {
  std::error_code ec;
  return fs::exists(P(g.info->exe_dir + "\\" + rel), ec);
}
bool has_dlss() { return file_exists("nvngx_dlss.dll"); }
bool has_fsr() { return file_exists("upscalers\\amd_fidelityfx_upscaler_vk.dll"); }
bool has_xess() { return file_exists("libxess.dll"); }
int pic_window_h() {   // the height the game window will have: what the runtime scales the upscaled picture from
  if (const char* e = std::getenv("SNAP_WINDOW_H")) { const int v = std::atoi(e); if (v > 0) return v; }
  const hle::Json* m = g.win.j.get("mode");
  if (m && m->type == hle::Json::Str && m->s == "borderless") return g.disp_h;
  if (const hle::Json* mx = g.win.j.get("maximized")) if (mx->boolean(false)) return g.disp_h;
  const hle::Json* h = g.win.j.get("height");
  return h && h->type == hle::Json::Num && h->n >= 240 ? (int)h->n : 720;
}
snappic::Ctx pic_ctx() {
  snappic::Ctx x;
  x.enhanced = snapdef::enhanced();
  x.window_h = pic_window_h();
  x.dlss = has_dlss(); x.fsr = has_fsr(); x.xess = has_xess();
  return x;
}
snappic::Cfg pic_cfg() {
  snappic::Cfg c;
  c.aa = g.cfg.get("gfx_aa", "");                 if (c.aa == "default") c.aa.clear();
  c.scale = g.cfg.get("gfx_render_scale", "");    if (c.scale == "auto") c.scale.clear();
  c.upscaler = g.cfg.get("gfx_upscaler", "");     if (c.upscaler == "off") c.upscaler.clear();
  c.quality = g.cfg.get("gfx_upscale_quality", ""); if (c.quality == "native") c.quality.clear();
  c.sharpen_set = g.cfg.kv.count("gfx_sharpen") != 0;
  return c;
}
void pic_store(const snappic::Cfg& c) {   // the three keys the AA and Performance rows own; "" erases (the default)
  auto put = [](const char* k, const std::string& v) { if (v.empty()) g.cfg.erase(k); else g.cfg.set(k, v); };
  put("gfx_aa", c.aa); put("gfx_upscaler", c.upscaler); put("gfx_upscale_quality", c.quality);
  g.cfg.save();
}

// The Graphics preset row (hle/presets.h): worked out from the stored rows, so changing any single row afterwards shows Custom.
std::string get_profile() { return snappreset::current([](const std::string& k) { return g.cfg.get(k, ""); }); }
void set_profile(const std::string& v) {
  const int p = snappreset::from_id(v);
  if (p < 0) return;
  snappreset::apply(p, [](const std::string& k, const std::string& val) {
    if (k == "gfx_look") set_look(val.empty() ? "enhanced" : val);   // clears the Look's own rows too
    else if (val.empty()) g.cfg.erase(k);
    else g.cfg.set(k, val);
  }, pic_ctx());
  g.cfg.save();
  apply_look_to_defaults();
}

std::string read_value(const Item& it) {
  if (it.kind == K_PROFILE) return get_profile();
  if (it.kind == K_DEVICE) return get_device();
  if (it.kind == K_AA) return snappic::aa_row(pic_cfg());
  if (it.kind == K_PERF) return snappic::perf_row(pic_cfg());
  if (it.kind == K_PRESET) return get_preset();
  if (it.kind == K_LOOK) return get_look();
  if (it.kind == K_PACING) return get_pacing();
  if (it.store == S_CFG) {
    std::string v = g.cfg.get(it.key, it.def);
    if (it.key == "gfx_render_scale" && v != "auto" && std::fabs((float)std::atof(v.c_str()) - (float)std::atof(fit_scale().c_str())) < 0.004f) {
      for (const Ch& c : it.ch) if (c.v == "fit") return "fit";
    }
    return v;
  }
  const JsonFile& f = it.store == S_WIN ? g.win : it.store == S_BAK ? g.bak : g.aud;
  const hle::Json* x = f.j.get(it.key);
  if (!x) return it.def;
  if (it.enc == E_STR) return x->type == hle::Json::Str ? x->s : it.def;
  if (it.enc == E_BOOL) return x->boolean(it.def != "0") ? "1" : "0";
  return x->type == hle::Json::Num ? num_text(x->n, it.enc) : it.def;
}

void write_value(Item& it, const std::string& val) {
  if (it.kind == K_PROFILE) { set_profile(val); return; }
  if (it.kind == K_DEVICE) { set_device(val); return; }
  if (it.kind == K_AA) { pic_store(snappic::with_aa(pic_cfg(), val)); return; }
  if (it.kind == K_PERF) { pic_store(snappic::with_perf(pic_cfg(), val, pic_ctx())); return; }
  if (it.kind == K_PRESET) { set_preset(val); return; }
  if (it.kind == K_LOOK) { set_look(val); return; }
  if (it.kind == K_PACING) { set_pacing(val); return; }
  const std::string v = (it.key == "gfx_render_scale" && val == "fit") ? fit_scale() : val;
  if (it.store == S_CFG) {
    if (v == it.def) g.cfg.erase(it.key);
    else g.cfg.set(it.key, v);
    g.cfg.save();
    return;
  }
  JsonFile& f = it.store == S_WIN ? g.win : it.store == S_BAK ? g.bak : g.aud;
  if (it.enc == E_STR) f.j.set(it.key, hle::Json::string(v));
  else if (it.enc == E_BOOL) f.j.set(it.key, hle::Json::boolean_(v != "0"));
  else f.j.set(it.key, hle::Json::number(std::atof(v.c_str())));
  f.save();
}

// The Automatic label, the DLAA / DLSS name and the one-line notes follow the other picture rows, so they are worked out again after every change.
void refresh_picture_text() {
  const snappic::Cfg c = pic_cfg();
  const snappic::Ctx x = pic_ctx();
  const bool dlss_on = snappic::perf_row(c) != "off";
  for (Page& p : g.pages) for (Item& it : p.items) {
    it.note.clear();
    if (it.key == "gfx_render_scale") {
      it.note = snappic::note_resolution(c, x);
      for (Ch& ch : it.ch) if (ch.v == "auto") ch.label = snappic::auto_label(c, x);
    } else if (it.kind == K_AA) {
      it.note = snappic::note_aa(c, x);
      for (Ch& ch : it.ch) {
        if (ch.v == "default") ch.label = x.enhanced ? "Default (SSAA + SMAA)" : "Default (FXAA)";
        else if (ch.v == "dlaa") ch.label = std::string(dlss_on ? "DLSS (NVIDIA)" : "DLAA (NVIDIA)") + (x.dlss ? "" : " (not installed)");
        else if (ch.v == "fsr") ch.label = std::string("FSR (AMD)") + (x.fsr ? "" : " (not installed)");
        else if (ch.v == "xess") ch.label = std::string("XeSS (Intel)") + (x.xess ? "" : " (not installed)");
      }
    } else if (it.kind == K_PERF) {
      it.note = snappic::note_perf(c, x);
    } else if (it.key == "gfx_sharpen") {
      it.note = snappic::note_sharpen(c);
    } else if (it.key == "gfx_smaa_quality") {
      it.note = snappic::note_smaa(c, x);
    }
  }
}
// The two Backups rows that show a state under their label: the folder chosen, and how the last backup went.
void refresh_backup_text() {
  const hle::Json* f = g.bak.j.get("folder");
  const std::string folder = f ? f->str() : std::string();
  for (Page& p : g.pages) for (Item& it : p.items) {
    if (it.kind != K_ACTION) continue;
    if (it.act == UiResult::BackupFolder) it.brief = folder.empty() ? "No folder chosen yet" : folder;
    else if (it.act == UiResult::BackupNow) it.brief = g.info && !g.info->backup_status.empty() ? g.info->backup_status : "Copies your profile to the backup folder now";
  }
}
void refresh_values() {
  for (Page& p : g.pages) for (Item& it : p.items) if (it.kind != K_ACTION && it.kind != K_INFO) it.cur = read_value(it);
  refresh_picture_text();
  refresh_backup_text();
}

std::string cfg_path_for(const std::string& id) {
  const UiInfo& i = *g.info;
  if (i.profiles_root.empty() || id.empty()) return i.settings_noprofile;
  return i.profiles_root + "\\" + id + "\\settings.cfg";
}

// The defaults that depend on the Look (and the display) are fixed in the items when they are built; this puts them right again after the Look changes.
void refresh_defaults() {
  for (Page& p : g.pages) for (Item& it : p.items) {
    if (it.key == "gfx_sharpen") it.def = snapdef::def("SNAP_SHARPEN");
    else if (it.key == "gfx_aniso") it.def = snapdef::def("SNAP_ANISO");
    else if (it.key == "gfx_render_scale") { for (Ch& c : it.ch) if (c.v == "fit") c.label = fit_label(); }
    else if (it.kind == K_PACING && it.ch.size() > 2) it.ch[2].label = "VRR (" + std::to_string(vrr_cap()) + " FPS limit)";
  }
}

void load_stores() {
  g.cfg.path = cfg_path_for(g.info->profile_id);
  g.cfg.load();
  g.win.path = g.info->data_dir + "\\window.json";
  g.win.load();
  g.aud.path = g.info->data_dir + "\\audio.json";
  g.aud.load();
  g.bak.path = g.info->data_dir + "\\backup.json";
  g.bak.load();
  apply_look_to_defaults();
  read_display();
  refresh_defaults();
  refresh_values();
}

std::string label_of(const Item& it, const std::string& v) {
  for (const Ch& c : it.ch) if (c.v == v) return c.label;
  if (it.key == "frame_cap" && v == std::to_string(vrr_cap()) && get_pacing() == "vrr") return v + " (auto)";
  if (it.key == "gfx_render_scale" && v != "auto" && !v.empty()) { const float f = (float)std::atof(v.c_str()); if (f > 0.f) return snappic::dims(f) + " (typed in)"; }
  if (it.key == "gfx_sharpen" && !v.empty()) {   // the F1 slider moves in 0.05 steps; an in-between saved value shows its nearest choice (the saved value is not touched)
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end != v.c_str() && *end == 0) {
      const Ch* best = nullptr; double bd = 1e9;
      for (const Ch& c : it.ch) { const double e = std::fabs(std::atof(c.v.c_str()) - d); if (e < bd) { bd = e; best = &c; } }
      char b[48];
      if (best && bd < 0.0005) return best->label;
      std::snprintf(b, sizeof b, "custom (%.2f)", d);
      return b;
    }
  }
  if (it.store != S_CFG && it.enc == E_VOL) return std::to_string((int)std::lround(std::atof(v.c_str()) * 100)) + "%";
  return v.empty() ? "Default" : v;
}

int index_of(const Item& it, const std::string& v) {
  for (size_t i = 0; i < it.ch.size(); ++i) if (it.ch[i].v == v) return (int)i;
  return -1;
}

// Read-only summary of the key, mouse and controller bindings (input.json in the data folder), on the Controls page. The page that edits them is the game's F1
// menu, Bindings tab (runtime/ui/ui_controls.cpp); both read the same file, so what shows here is what the game will use.
void add_binding_summary(Page& p) {
  hle::input::Mapping m = hle::input::Mapping::defaults();
  bool custom = false;
  {
    std::ifstream f(g.info->data_dir + "\\input.json", std::ios::binary);
    if (f) {
      std::stringstream ss; ss << f.rdbuf();
      hle::Json j; std::string err;
      hle::JsonParser jp(ss.str());
      if (jp.parse(j, err) && m.from_json(j, &err)) custom = true;
      else m = hle::input::Mapping::defaults();
    }
  }
  // One K_INFO row per button: cur = "keys and mouse<TAB>controller", drawn as a two-column table under the setting rows (draw_settings)
  auto devs = [&](const std::vector<hle::input::Source>& v, int d0, int d1) {
    std::string out;
    for (int d = d0; d <= d1; ++d) {
      for (int sl = 0; sl < hle::input::kMaxSlotsPerDevice; ++sl) {
        const hle::input::Source s = hle::input::slot_get(v, (hle::input::Device)d, sl);
        if (s.kind != hle::input::Source::None) out += (out.empty() ? "" : ", ") + hle::input::source_label(s);
      }
    }
    return out.empty() ? std::string("-") : out;
  };
  auto info = [&](const std::string& label, const std::string& kb, const std::string& pad) { Item it; it.label = label; it.kind = K_INFO; it.cur = kb + "\t" + pad; p.items.push_back(it); };
  using namespace hle::input;
  for (int b : {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus}) {
    const auto src = action_sources(m, b);
    info(action_label(b), devs(src, kDevKeyboard, kDevMouse), devs(src, kDevController, kDevController));
  }
  auto keys4 = [&](int first) {   // the four directions of a stick on the keyboard, in the order up, down, left, right
    std::string out;
    for (int a = first; a < first + 4; ++a) { const Source s = slot_get(action_sources(m, a), kDevKeyboard, 0); out += (out.empty() ? "" : " ") + (s.kind == Source::None ? std::string("-") : source_label(s)); }
    return out == "Up Down Left Right" ? std::string("Arrow keys") : out + " (up, down, left, right)";
  };
  info("Left stick", keys4(16), "Left stick");
  info("Right stick", keys4(20), "Right stick");
  g.bindings_custom = custom;
}

void access_changed();   // below

void choose(Item& it, int idx) {
  if (idx < 0 || idx >= (int)it.ch.size() || !it.ch[(size_t)idx].avail) return;
  const bool was_vrr = it.key == "monitor" && get_pacing() == "vrr";
  const snappic::Cfg before = (it.kind == K_AA || it.kind == K_PERF) ? pic_cfg() : snappic::Cfg();
  write_value(it, it.ch[(size_t)idx].v);
  if (it.kind == K_AA || it.kind == K_PERF) {   // one row moved the other: say so, the row itself shows the new value
    const snappic::Cfg now = pic_cfg();
    if (it.kind == K_AA && snappic::perf_row(before) != "off" && snappic::perf_row(now) == "off") { g.toast = "Performance scaling is off: it needs an upscaler"; g.toast_until = ImGui::GetTime() + 3; }
    if (it.kind == K_PERF && snappic::aa_row(before) != snappic::aa_row(now)) {
      const std::string a = snappic::aa_row(now);
      g.toast = std::string("Anti-aliasing is now ") + (a == "dlaa" ? "DLSS" : a == "fsr" ? "FSR" : "XeSS") + ": Performance scaling needs an upscaler";
      g.toast_until = ImGui::GetTime() + 3;
    }
  }
  if (it.key == "monitor") {
    read_display();             // the screen changed: its refresh rate and size follow
    if (was_vrr) set_pacing("vrr");
    refresh_defaults();
  }
  if (it.kind == K_LOOK || it.kind == K_AA || it.kind == K_PERF || it.kind == K_PROFILE || it.kind == K_DEVICE) refresh_defaults();
  if (it.key.rfind("access_", 0) == 0) access_changed();
  // a preset, the Look, the pacing, the monitor and the render scale each change what other rows show
  refresh_values();
}

void cycle(Item& it, int dir) {
  if (it.ch.empty()) return;
  const int n = (int)it.ch.size();
  int i = index_of(it, it.cur);
  if (i < 0) i = dir > 0 ? -1 : 0;
  for (int k = 0; k < n; ++k) {
    i = ((i + dir) % n + n) % n;
    if (it.ch[(size_t)i].avail) { choose(it, i); return; }
  }
}

void reset_page(Page& p) {
  for (Item& it : p.items) {
    if (it.kind == K_ACTION || it.kind == K_PROFILE || it.kind == K_DEVICE || it.kind == K_INFO) continue;   // the Graphics preset is worked out from the rows reset below
    if (it.kind == K_PRESET) { set_preset("normal"); continue; }
    if (it.kind == K_AA) { g.cfg.erase("gfx_aa"); g.cfg.erase("gfx_upscaler"); g.cfg.erase("gfx_upscale_quality"); continue; }
    if (it.kind == K_PERF) { g.cfg.erase("gfx_upscale_quality"); if (g.cfg.get("gfx_upscaler", "") == "dlss") g.cfg.set("gfx_upscaler", "dlaa"); continue; }
    if (it.kind == K_LOOK) { set_look(it.def); continue; }
    if (it.kind == K_PACING) { set_pacing("vsync"); continue; }
    if (it.store == S_CFG) { g.cfg.erase(it.key); }
    else write_value(it, it.def);
  }
  g.cfg.save();
  read_display();
  refresh_defaults();
  refresh_values();
  access_changed();
}

struct Mon { RECT r; };
BOOL CALLBACK mon_cb(HMONITOR, HDC, LPRECT r, LPARAM lp) { ((std::vector<Mon>*)lp)->push_back({*r}); return TRUE; }

// Accessibility rows that change the launcher itself: the text size (the window grows, up to the work area) and the high-contrast palette. A SNAP_ACCESS_*
// variable set by hand wins over the saved value, like in the game.
std::string access_value(const char* key, const char* env) {
  const char* e = std::getenv(env);
  return e && *e ? std::string(e) : g.cfg.get(key, "");
}
int access_ui_percent() {
  const int n = std::atoi(access_value("access_ui_size", "SNAP_ACCESS_UI_SIZE").c_str());
  return n >= 175 ? 200 : n >= 138 ? 150 : n >= 113 ? 125 : 100;
}
void fit_window() {
  using GetDpi = UINT(WINAPI*)(HWND);
  GetDpi gd = (GetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  const char* dpi_test = std::getenv("SNAP_LAUNCHER_DPI");   // test hook: pretend the display is at this DPI (96 = 100%, 144 = 150%, 192 = 200%)
  const int dpi_forced = dpi_test ? std::atoi(dpi_test) : 0;
  const float sc = (dpi_forced >= 96 && dpi_forced <= 384 ? dpi_forced / 96.0f : gd ? gd(g.hwnd) / 96.0f : 1.0f) * (float)access_ui_percent() / 100.0f;
  g.tb = std::round(34.0f * sc);
  RECT r{0, 0, (LONG)(1280 * sc), (LONG)(720 * sc + g.tb)};   // the title bar is ours, so the client area is the whole window
  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  const int w = std::min<int>(r.right - r.left, wa.right - wa.left), h = std::min<int>(r.bottom - r.top, wa.bottom - wa.top);
  SetWindowPos(g.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void apply_access_palette() {
  const std::string c = access_value("access_contrast", "SNAP_ACCESS_CONTRAST");
  g_hc = !(c.empty() || c == "0" || c == "off");
  if (g_hc) { kPanel = 0x000000; kTab = 0x383838; kRow = 0x1e1e1e; kGreen = 0x1654d9; kText = 0x000000; kBrown = 0x000000; kGrey = 0x202020; }
  else { kPanel = 0x6e7180; kTab = 0x9a9da7; kRow = 0x989899; kGreen = 0xb6d291; kText = 0x646464; kBrown = 0x4f3d29; kGrey = 0x8b8a8b; }
}
void access_changed() { apply_access_palette(); if (g.hwnd && !IsZoomed(g.hwnd)) fit_window(); }

void build_pages() {
  g.pages.clear();
  auto choice = [](const char* label, const char* key, Store st, const char* def, std::vector<Ch> ch, const char* help, Enc enc = E_STR) {
    Item it;
    it.label = label; it.key = key; it.store = st; it.def = def; it.ch = std::move(ch); it.help = help; it.enc = enc;
    return it;
  };
  const std::string aa_def = std::string("Default (") + (snapdef::enhanced() ? "SMAA" : "FXAA") + ")";
  std::vector<Ch> vols;
  for (int i = 0; i <= 10; ++i) { char v[8], l[8]; std::snprintf(v, sizeof v, "%.1f", i / 10.0); std::snprintf(l, sizeof l, "%d%%", i * 10); vols.push_back({v, l}); }

  // Display
  {
    Page p; p.name = "Display";
    Item dv = choice("Device", "", S_CFG, "desktop", {{"desktop", "Desktop"}, {"steamdeck", "Steam Deck (1280 x 800, controller only)"}},
                     "Steam Deck sets a borderless 1280 x 800 window, a lighter 3D picture (1440 x 810, SMAA, 4x filtering), a 150% HUD with sharper menu text and Xbox button icons, all at once. "
                     "Desktop puts those rows back to their defaults. Changing any one of them afterwards shows Desktop again. Command line: --steam-deck.");
    dv.kind = K_DEVICE;
    p.items.push_back(dv);
    p.items.push_back(choice("Window mode", "mode", S_WIN, "windowed", {{"windowed", "Windowed"}, {"borderless", "Borderless fullscreen"}},
                             "Windowed, or borderless fullscreen. F11 and Alt+Enter switch it in the game too."));
    std::vector<Mon> mons;
    EnumDisplayMonitors(nullptr, nullptr, mon_cb, (LPARAM)&mons);
    std::vector<Ch> mch = {{"-1", "Where the window is"}};
    for (size_t i = 0; i < mons.size(); ++i)
      mch.push_back({std::to_string(i), std::to_string(i + 1) + ": " + std::to_string(mons[i].r.right - mons[i].r.left) + " x " + std::to_string(mons[i].r.bottom - mons[i].r.top)});
    p.items.push_back(choice("Monitor", "monitor", S_WIN, "-1", mch, "Which screen the borderless window covers. A windowed game stays where you put it.", E_INT));
    p.items.push_back(choice("Ultrawide", "gfx_ultrawide", S_CFG, "auto", {{"auto", "Auto (wider 3D view)"}, {"off", "Off (16:9 with side bars)"}},
                             "On a screen wider than 16:9, Auto shows more of the world at the sides. Menus, the interface and your photos stay 16:9."));
    p.items.push_back(choice("HUD scale", "hud_scale", S_CFG, "auto",
                             {{"auto", "Auto (100%)"}, {"100", "100%"}, {"125", "125%"}, {"150", "150%"}, {"175", "175%"}, {"200", "200%"}},
                             "Makes the viewfinder HUD, menus and dialogue bigger on large screens. The 3D view and photos stay as they are. Auto is 100%: pick a size to scale."));
    p.items.push_back(choice("Pause in background", "pause_on_focus_loss", S_WIN, "1", {{"1", "On"}, {"0", "Off"}},
                             "Pauses the game when you switch to another window.", E_BOOL));
    p.items.push_back(choice("Mute in background", "mute_in_background", S_WIN, "1", {{"1", "On"}, {"0", "Off"}},
                             "Silences the game while another window is in front.", E_BOOL));
    p.items.push_back(choice("When the game closes", "return_to_launcher", S_CFG, "off", {{"off", "Close everything"}, {"on", "Back to this launcher"}},
                             "Back to this launcher opens it again once the game has closed (saves and settings are written first). Close everything just ends. If the game crashes, the launcher always comes back and offers safe mode."));
    g.pages.push_back(p);
  }
  // Picture: the four rows in the order the picture is made (Look, then how many pixels, then the edge smoothing, then the upscaler's share), each
  // with a one-line note when another row overrides it (hle/picture_rows.h)
  {
    Page p; p.name = "Picture";
    Item gp = choice("Graphics preset", "", S_CFG, "enhanced",
                     {{"original", "Original"}, {"balanced", "Balanced"}, {"enhanced", "Enhanced"}, {"max", "Max"}, {"custom", "Custom"}},
                     "Sets the picture rows together. Original is the game as shipped, Balanced is lighter, Enhanced is the new-install look, Max needs a fast graphics card. Any single change shows Custom.");
    gp.kind = K_PROFILE;
    gp.ch[4].avail = false;   // "Custom" is only ever shown, never picked
    p.items.push_back(gp);
    Item lk = choice("Look", "gfx_look", S_CFG, snapdef::kDefaultEnhanced ? "enhanced" : "original", {{"enhanced", "Enhanced"}, {"original", "Original"}},
                     "Enhanced is the look new installs start with: smoother edges, sharpening, 16x filtering, sharper menu text and a PNG file for every photo. Original is the game as it shipped. "
                     "Picking one sets all of those together; you can still change any row after. Photo scores are the same either way.");
    lk.kind = K_LOOK;
    p.items.push_back(lk);
    p.items.push_back(choice("Internal resolution", "gfx_render_scale", S_CFG, "auto",
                             {{"auto", "Automatic"}, {"fit", fit_label()}, {"0.50", "960 x 540"}, {"0.75", "1440 x 810"}, {"1.00", "1920 x 1080 (the game's own)"}, {"1.25", "2400 x 1350"},
                              {"1.50", "2880 x 1620"}, {"2.00", "3840 x 2160 (4K)"}, {"3.00", "5760 x 3240"}, {"4.00", "7680 x 4320 (8K)"}},
                             "How many pixels the 3D picture is drawn with. Automatic lets the Look and the Anti-aliasing row decide. A size sets it yourself. Fit to display matches your screen's height. Lower is faster and softer."));
    Item aa = choice("Anti-aliasing", "gfx_aa", S_CFG, "default",
                     {{"default", "Default"}, {"off", "FXAA (the game's own)"}, {"smaa", "SMAA"}, {"ssaa2", "2x supersampling + FXAA"}, {"ssaa2,smaa", "2x supersampling + SMAA"}, {"none", "None"},
                      {"dlaa", "DLAA (NVIDIA)"}, {"fsr", "FSR (AMD)"}, {"xess", "XeSS (Intel)"}},
                     "Smooths jagged edges. FXAA is what the game ships with. SMAA keeps more detail. Supersampling draws at twice the size first and costs a lot of GPU time. "
                     "DLAA, FSR and XeSS use an upscaler at full size and need the files from Install upscalers on the Upscaling page.");
    aa.kind = K_AA;
    const bool dl = has_dlss(), fs_ = has_fsr(), xe = has_xess();
    aa.ch[6].avail = dl; aa.ch[7].avail = fs_; aa.ch[8].avail = xe;
    p.items.push_back(aa);
    p.items.push_back(choice("SMAA quality", "gfx_smaa_quality", S_CFG, "high",
                             {{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"ultra", "Ultra"}},
                             "Only SMAA reads this. Low is the fastest and misses fine edges. Medium is a good balance. High is the default. Ultra catches the faintest edges and costs the most. Low and Medium skip diagonal and corner handling."));
    Item pf = choice("Performance scaling", "gfx_upscale_quality", S_CFG, "off",
                     {{"off", "Off"}, {"quality", "Quality"}, {"balanced", "Balanced"}, {"performance", "Performance"}},
                     "Draws fewer pixels and enlarges them with DLSS, FSR or XeSS, so a slower graphics card keeps up. Quality draws 1.5x fewer, Balanced 1.7x, Performance 2x. "
                     "It needs an upscaler: picking one here switches Anti-aliasing to the best one you have.");
    pf.kind = K_PERF;
    const bool any_up = dl || fs_ || xe;
    for (size_t i = 1; i < pf.ch.size(); ++i) if (!any_up) { pf.ch[i].avail = false; pf.ch[i].label += " (needs an upscaler)"; }
    p.items.push_back(pf);
    p.items.push_back(choice("Sharpening", "gfx_sharpen", S_CFG, snapdef::def("SNAP_SHARPEN"),
                             {{"0.00", "Off"}, {"0.15", "0.15"}, {"0.30", "0.30"}, {"0.50", "0.50"}, {"0.75", "0.75"}, {"1.00", "1.00"}},
                             "A gentle sharpening pass after anti-aliasing. Too much makes edges look hard."));
    Item pr = choice("Detail preset", "", S_CFG, "normal",
                     {{"verylow", "Very Low"}, {"low", "Low"}, {"normal", "Default"}, {"high", "High"}, {"ultra", "Ultra"}, {"maximum", "Maximum"}, {"custom", "Custom"}},
                     "Sets the picture rows and the Detail page together. Default follows the Look row. It leaves an upscaler alone. Higher presets are sharper and slower; Maximum needs a very fast graphics card.");
    pr.kind = K_PRESET;
    pr.ch[6].avail = false;   // "Custom" is only ever shown, never picked
    p.items.push_back(pr);
    g.pages.push_back(p);
  }
  // Detail
  {
    Page p; p.name = "Detail";
    p.items.push_back(choice("Anisotropic filtering", "gfx_aniso", S_CFG, snapdef::def("SNAP_ANISO"),
                             {{"0", "Off (the game's own)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}},
                             "Keeps ground and walls sharp at shallow angles. 16x costs almost nothing on a PC graphics card."));
    p.items.push_back(choice("Lighting", "gfx_lighting", S_CFG, "default",
                             {{"default", "Original"}, {"enhanced", "Enhanced"}, {"maximum", "Maximum"}},
                             "One switch for the whole lighting look. Enhanced adds soft shadows that sharpen where they touch the ground, shading in corners and clean contact shadows. Maximum adds a larger shadow map, four times the reach and finer shading. Photos and scoring are untouched."));
    p.items.push_back(choice("Bounce light", "gfx_bounce", S_CFG, "default",
                             {{"default", "Off"}, {"1", "On"}},
                             "Colored light that bounces off nearby surfaces, such as green from grass onto a Pokemon. Display only; photos and scoring are untouched."));
    p.items.push_back(choice("Shadow resolution", "gfx_shadow_res", S_CFG, "default",
                             {{"512", "512 (blocky)"}, {"default", "1024 (the game's own)"}, {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192 (very heavy)"}},
                             "Size of the moving shadow map. Higher gives crisper shadow edges and uses more video memory."));
    p.items.push_back(choice("Ambient occlusion", "ssao_quality", S_CFG, "default",
                             {{"default", "Default"}, {"high", "High"}, {"ultra", "Ultra"}},
                             "Soft contact shadows. It mostly shows on the photo-select screen."));
    p.items.push_back(choice("HD effects", "gfx_hd_effects", S_CFG, snapdef::def("SNAP_HD_EFFECTS"),
                             {{"0", "Off (the game's own)"}, {"1", "On"}},
                             "Draws the sea, splashes, dust and glow at the scene's full resolution instead of half, so they stay sharp at 1440p and 4K. Photo scoring is untouched."));
    p.items.push_back(choice("Specular sparkle filter", "specular_aa", S_CFG, "default",
                             {{"default", "Off (the game's own)"}, {"1", "On"}},
                             "Calms the single-pixel glints that flicker on shiny or wet surfaces and Pokemon while the camera moves. Nothing is blurred. Photos are unchanged."));
    g.pages.push_back(p);
  }
  // Upscaling
  {
    Page p; p.name = "Upscaling";   // the upscaler itself is the Anti-aliasing row and its share the Performance scaling row (Picture page); this page holds what goes with it
    {
      Item fg = choice("Frame generation", "frame_generation", S_CFG, "default", {{"default", "Off"}, {"fsr", "AMD FSR"}, {"dlss", "NVIDIA DLSS"}},
                       "Adds a computed frame between two real ones, so 60 fps looks like about 120. AMD FSR needs the files from Install upscalers. NVIDIA DLSS needs an RTX 40 or 50 card and the Streamline files in the upscalers\\streamline folder. It adds about one frame of delay and turns itself off under 50 fps. Photos and scoring never see it.");
      fg.ch[1].avail = file_exists("upscalers\\amd_fidelityfx_framegeneration_vk.dll");
      if (!fg.ch[1].avail) fg.ch[1].label += " (not installed)";
      fg.ch[2].avail = file_exists("upscalers\\streamline\\sl.interposer.dll");
      if (!fg.ch[2].avail) fg.ch[2].label += " (not installed)";
      p.items.push_back(fg);
    }
    p.items.push_back(choice("HDR output", "gfx_hdr", S_CFG, "0", {{"0", "Off"}, {"1", "HDR10"}, {"scrgb", "scRGB"}},
                             "For an HDR monitor with Windows HDR turned on. scRGB has far less banding."));
    Item inst; inst.label = "Install upscalers..."; inst.kind = K_ACTION; inst.act = UiResult::Upscalers;
    inst.help = "Download FSR, DLSS or XeSS from their makers (you read and accept each license first). Nothing installs without you.";
    p.items.push_back(inst);
    g.pages.push_back(p);
  }
  // Frame rate
  {
    Page p; p.name = "Frame rate";
    {
      Item pc = choice("Frame pacing", "", S_CFG, "vsync",
                       {{"off", "Off (uncapped, may tear)"}, {"vsync", "VSync"}, {"vrr", "VRR (G-SYNC / FreeSync)"}, {"custom", "Custom"}},
                       "VSync waits for the screen. VRR is VSync plus a frame limit just under your screen's refresh rate, which keeps a G-SYNC or FreeSync screen smooth without extra delay; "
                       "the limit follows the monitor. Custom means you set vsync and the limit yourself below.");
      pc.kind = K_PACING;
      pc.ch[3].avail = false;
      pc.ch[2].label = "VRR (" + std::to_string(std::max(30, g.disp_hz - 3)) + " FPS limit)";
      p.items.push_back(pc);
    }
    p.items.push_back(choice("Frame rate target", "fps_target", S_CFG, "default",
                             {{"default", "60 (the game's own)"}, {"72", "72"}, {"90", "90"}, {"100", "100"}, {"120", "120"}, {"144", "144"}, {"165", "165"}, {"180", "180"},
                              {"200", "200"}, {"240", "240"}, {"300", "300"}, {"360", "360"}, {"480", "480"}, {"monitor", "Match monitor"}, {"unlocked", "Unlocked"}},
                             "How fast the game runs, in real frames. Higher rates need a fast processor; rides, Pokemon, scores, menus and dialogue keep their original speed. "
                             "Match monitor uses the monitor's refresh rate, Unlocked runs as fast as the PC allows. Any rate from 61 to 1000 works with --fps <n>."));
    p.items.push_back(choice("Frame rate limit", "frame_cap", S_WIN, "0",
                             {{"0", "Off (the game's pacing)"}, {"30", "30"}, {"45", "45"}, {"60", "60"}, {"75", "75"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
                              {"180", "180"}, {"200", "200"}, {"240", "240"}, {"360", "360"}},
                             "Stops the game from drawing faster than this. Off keeps the game's own pacing.", E_INT));
    p.items.push_back(choice("Vsync", "vsync", S_CFG, "on", {{"on", "On"}, {"adaptive", "Adaptive"}, {"fast", "Fast"}, {"off", "Off"}},
                             "On: no tearing. Adaptive: tears only below the refresh rate. Fast: no tearing, lower latency. Off: lowest latency, may tear."));
    p.items.push_back(choice("Performance overlay", "fps_overlay", S_CFG, "default", {{"default", "Off"}, {"fps", "FPS"}, {"detailed", "Detailed"}},
                             "Shows the frame rate in a corner of the game. Detailed adds frame time, 1% low and a graph."));
    g.pages.push_back(p);
  }
  // Sound
  {
    Page p; p.name = "Sound";
    p.items.push_back(choice("Master volume", "master", S_AUD, "1.0", vols, "The overall volume.", E_VOL));
    p.items.push_back(choice("Music", "bgm", S_AUD, "1.0", vols, "Background music volume.", E_VOL));
    p.items.push_back(choice("Sound effects", "se", S_AUD, "1.0", vols, "Effects volume.", E_VOL));
    p.items.push_back(choice("Voices", "voice", S_AUD, "1.0", vols, "Voice and Professor volume.", E_VOL));
    p.items.push_back(choice("Output", "output", S_AUD, "stereo", {{"stereo", "Stereo"}, {"speakers", "Surround speakers"}, {"spatial", "Spatial (headphones)"}},
                             "Stereo is right for most setups. Surround speakers and spatial need Windows to be set up for them."));
    p.items.push_back(choice("Audio latency", "audio_latency", S_CFG, "normal", {{"low", "Low"}, {"normal", "Normal"}, {"safe", "Safe"}},
                             "Low is the shortest delay, Safe a bigger buffer for crackle or Bluetooth."));
    p.items.push_back(choice("Sample-rate conversion", "audio_resampler", S_CFG, "system", {{"system", "Windows default"}, {"sinc", "High quality"}, {"linear", "Linear"}},
                             "Only matters when your device runs at 44.1 or 96 kHz instead of 48."));
    p.items.push_back(choice("Dynamic range", "audio_range", S_CFG, "full", {{"full", "Full"}, {"reduced", "Reduced"}, {"night", "Night mode"}},
                             "Night mode keeps quiet sounds audible and loud ones down."));
    g.pages.push_back(p);
  }
  // Controls
  {
    Page p; p.name = "Controls";
    p.items.push_back(choice("Button icons", "button_icons", S_CFG, "auto",
                             {{"auto", "Automatic"}, {"switch", "Switch"}, {"xbox", "Xbox"}, {"playstation", "PlayStation"}, {"keyboard", "Keyboard and mouse"}},
                             "Which buttons the game shows. Automatic shows key caps without a controller and follows the controller you connect."));
    p.items.push_back(choice("Swap A and B", "swap_ab", S_CFG, "0", {{"0", "Off"}, {"1", "On"}},
                             "Swaps confirm and back on the controller: the south button confirms and the east button goes back, like an Xbox or PlayStation pad. The game's button pictures follow."));
    p.items.push_back(choice("Rumble", "rumble_on", S_CFG, "1", {{"1", "On"}, {"0", "Off"}}, "Controller vibration."));
    p.items.push_back(choice("Motion aiming (gyro)", "gyro_on", S_CFG, "0", {{"0", "Off"}, {"1", "On"}}, "Aim the camera by moving a controller that has motion sensors."));
    p.items.push_back(choice("Stick response", "stick_curve", S_CFG, "linear", {{"linear", "Linear (default)"}, {"classic", "Classic"}, {"precise", "Precise"}},
                             "How far the camera stick turns the camera for a given push. Classic and Precise give finer control near the center."));
    p.items.push_back(choice("Stick dead zone", "stick_deadzone", S_CFG, "-1", {{"-1", "Follow the controls menu"}, {"0", "None"}, {"0.05", "5%"}, {"0.10", "10%"}, {"0.15", "15%"}, {"0.20", "20%"}, {"0.30", "30%"}},
                             "The part of the stick's travel the game ignores. Follow the controls menu keeps each stick's own setting."));
    p.items.push_back(choice("Gyro smoothing", "gyro_smooth", S_CFG, "0", {{"0", "Off"}, {"0.25", "Light"}, {"0.5", "Medium"}, {"1", "Strong"}},
                             "Smooths motion aiming. More smoothing steadies the camera and adds a little delay."));
    p.items.push_back(choice("Gyro sampling", "gyro_avg", S_CFG, "0", {{"0", "Latest sample (default)"}, {"1", "Average between frames"}},
                             "Average between frames reads the motion sensor about every millisecond and gives the game the mean, so fast turns are not missed."));
    p.items.push_back(choice("Raw mouse input", "raw_mouse", S_CFG, "0", {{"0", "Off (default)"}, {"1", "On"}},
                             "Mouse aiming reads the mouse directly: no pointer acceleration and no movement lost between frames."));
    p.items.push_back(choice("Mouse response window", "mouse_window_ms", S_CFG, "40", {{"40", "40 ms (default)"}, {"24", "24 ms"}, {"16", "16 ms (lowest delay)"}, {"8", "8 ms"}},
                             "How much recent mouse movement is averaged for aiming. Shorter reacts faster and feels less smooth at low frame rates."));
    add_binding_summary(p);
    g.pages.push_back(p);
  }
  // Gameplay
  {
    Page p; p.name = "Gameplay";
    // docs/player-options.md: only the languages the game ships text for; Auto = the Windows display language mapped to the closest one, else English
    p.items.push_back(choice("Game language", "game_language", S_CFG, "auto",
                             {{"auto", "Auto (Windows language)"}, {"en", "English"}, {"ja", "Japanese"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"}, {"es", "Spanish"}, {"ko", "Korean"},
                              {"zh-hans", "Chinese (Simplified)"}, {"zh-hant", "Chinese (Traditional)"}},
                             "The language of the game's own menus and text. Auto follows your Windows display language and falls back to English. Takes effect the next time the game starts."));
    // docs/player-options.md: dormant (row hidden) until an application id is set, see runtime/hle/discord_app.h
    if (!snapdiscord::app_id(g.cfg.get("discord_app_id", "")).empty())
      p.items.push_back(choice("Discord Rich Presence", "discord_presence", S_CFG, "0", {{"0", "Off"}, {"1", "On"}},
                               "Shows the course you are riding and what you are doing on your Discord profile while the game runs. It talks only to the Discord app on this PC."));
    {
      Item rep; rep.label = "Create problem report...";rep.kind = K_ACTION; rep.act = UiResult::ProblemReport;
      rep.help = "Saves one zip on your Desktop with the logs, your settings and your PC's specs (Windows, graphics card and driver) for a bug report. Your user name and folders are removed; no game files or saves go in. Nothing is uploaded.";
      p.items.push_back(rep);
      Item chk; chk.label = "Check install..."; chk.kind = K_ACTION; chk.act = UiResult::CheckInstall;
      chk.help = "Compares your extracted game files and this build's program files with the known-good lists and says in plain words what is wrong: modified, wrong game version, missing or a mixed update. It only reads files and shows no hashes. Nothing is uploaded.";
      p.items.push_back(chk);
    }
    p.items.push_back(choice("Skip course intro", "skip_course_intro", S_CFG, "0", {{"0", "Off"}, {"1", "On"}},
                             "Plays the fixed fly-over at the start of every course about 8 times faster, so the ride starts after a second or two. Rides, Pokemon and photo scores are not changed."));
    g.pages.push_back(p);
  }
  // Accessibility (docs/accessibility.md): every row starts off; the same rows and keys as the F1 menu's Accessibility tab
  {
    Page p; p.name = "Accessibility";
    p.items.push_back(choice("Color vision", "access_cvd", S_CFG, "off", {{"off", "Off"}, {"protanopia", "Protanopia"}, {"deuteranopia", "Deuteranopia"}, {"tritanopia", "Tritanopia"}},
                             "Shifts colors that are hard to tell apart into ones you can see, over the whole game picture including the HUD. Photos and photo scores are not changed."));
    std::vector<Ch> strength;
    for (int i = 10; i <= 100; i += 10) { char v[8], l[8]; std::snprintf(v, sizeof v, "%d", i); std::snprintf(l, sizeof l, "%d%%", i); strength.push_back({v, l}); }
    p.items.push_back(choice("Correction strength", "access_cvd_strength", S_CFG, "100", strength, "How much of the color shift is applied. 100% is the full correction."));
    p.items.push_back(choice("Reduce flashing", "access_flash", S_CFG, "off", {{"off", "Off"}, {"gentle", "Gentle"}, {"standard", "Standard"}, {"strong", "Strong"}},
                             "Limits how fast the whole picture can brighten or darken, so bright bursts and camera flashes fade in over a few frames."));
    p.items.push_back(choice("Text and menu size", "access_ui_size", S_CFG, "100", {{"100", "100%"}, {"125", "125%"}, {"150", "150%"}, {"200", "200%"}},
                             "Makes this launcher and the in-game F1 menu bigger. The launcher window grows up to the size of your screen."));
    p.items.push_back(choice("High-contrast menus", "access_contrast", S_CFG, "0", {{"0", "Off"}, {"1", "On"}},
                             "Black panels with white text and a bright selection, for this launcher and the F1 menu."));
    p.items.push_back(choice("Hold to toggle", "access_toggle", S_CFG, "off",
                             {{"off", "Off"}, {"ZL", "ZL (zoom)"}, {"ZR", "ZR (shutter)"}, {"ZL,ZR", "ZL and ZR"}, {"ZL,ZR,L,R,X,Y,A,B", "Every action button"}},
                             "A press holds the button down until the next press, for buttons the game wants held. Applies the next time the game starts."));
    p.items.push_back(choice("Mono audio", "access_mono", S_CFG, "0", {{"0", "Off"}, {"1", "On"}}, "Plays the same sound in both ears."));
    g.pages.push_back(p);
  }
  // Backups (docs/sync-backups.md): one zip of the profile in a folder you pick, kept in step by the game and this page; stored in backup.json
  {
    Page p; p.name = "Backups";
    p.items.push_back(choice("Back up my profile", "enabled", S_BAK, "0", {{"0", "Off"}, {"1", "On"}},
                             "Copies your save, settings and photos into one zip in the backup folder each time the game closes after your save changed. Pick a OneDrive or Dropbox folder and the copies leave this PC too. A save that is being written or waits for recovery is never copied; the launcher says so here. Command line: --backup on|off.", E_BOOL));
    Item fo; fo.label = "Backup folder..."; fo.kind = K_ACTION; fo.act = UiResult::BackupFolder;
    fo.help = "Pick the folder the backup zips go in, for example a folder inside OneDrive or Dropbox. Each zip is written under a temporary name and renamed when it is finished and checked, so a sync program never uploads half a file. Command line: --backup-folder.";
    p.items.push_back(fo);
    p.items.push_back(choice("Copies to keep", "keep", S_BAK, "3", {{"1", "1"}, {"2", "2"}, {"3", "3"}, {"5", "5"}, {"10", "10"}, {"20", "20"}},
                             "How many backup zips of each profile stay in the folder. The oldest is deleted only after the new one has been written and checked. Command line: --backup-keep.", E_INT));
    p.items.push_back(choice("Back up saves", "saves", S_BAK, "1", {{"1", "On"}, {"0", "Off"}},
                             "The save of the profile you start with: the current generation of every user, copied only while the game is not writing it.", E_BOOL));
    p.items.push_back(choice("Back up settings", "settings", S_BAK, "1", {{"1", "On"}, {"0", "Off"}},
                             "This profile's settings, the window and audio files and the controls mapping.", E_BOOL));
    p.items.push_back(choice("Back up photos", "photos", S_BAK, "1", {{"1", "On"}, {"0", "Off"}},
                             "Your exported photos and screenshots. They can be large; the zip handles more than 4 GB. HD texture packs and shader caches are never included: they can be rebuilt.", E_BOOL));
    Item now; now.label = "Back up now..."; now.kind = K_ACTION; now.act = UiResult::BackupNow;
    now.help = "Makes a backup right now, even if nothing changed. Command line: --backups now.";
    p.items.push_back(now);
    Item rs; rs.label = "Restore a backup..."; rs.kind = K_ACTION; rs.act = UiResult::RestoreSyncBackup;
    rs.help = "Lists the backups in the folder with their date and size and puts one into a NEW profile. Your current profile is not touched unless you pick that and confirm twice. Command line: --backups restore.";
    p.items.push_back(rs);
    g.pages.push_back(p);
  }
  // The line under each row's label: a short version of its help. Rows not listed here use the first sentence of the help.
  static const struct { const char* label; const char* brief; } kBriefs[] = {
      {"Device", "Sets the window, picture and HUD for a handheld in one go."},
      {"When the game closes", "Come back to this launcher, or close everything."},
      {"Look", "Enhanced is the new-install look. Original is the game as shipped."},
      {"SMAA quality", "How carefully edges are searched. Only SMAA uses it."},
      {"Performance scaling", "Draws fewer pixels and upscales them, so a slower card keeps up."},
      {"Bounce light", "Colored light bouncing between nearby surfaces."},
      {"Frame generation", "Adds computed frames in between, so 60 fps looks like about 120."},
      {"Frame pacing", "VSync, VRR (G-SYNC or FreeSync), or your own limit."},
      {"Vsync", "How the picture waits for the screen, to avoid tearing."},
      {"Swap A and B", "Confirm on the south button, back on the east button."},
      {"Discord Rich Presence", "Shows the course you are riding on your Discord profile."},
      {"Create problem report...", "Saves a zip on your Desktop for a bug report. Nothing is uploaded."},
      {"Check install...", "Checks your game files and says in plain words what is wrong."},
      {"Install upscalers...", "Download FSR, DLSS or XeSS from their makers. Nothing installs without you."},
      {"Skip course intro", "Plays the course fly-over about 8 times faster."},
      {"Color vision", "Shifts colors that are hard to tell apart into ones you can see."},
      {"Reduce flashing", "Fades bright bursts and camera flashes in over a few frames."},
      {"High-contrast menus", "Black panels, white text and a bright selection."},
      {"Hold to toggle", "A press holds the button until you press it again."},
  };
  for (Page& p : g.pages) for (Item& it : p.items) {
    if (it.kind == K_INFO) continue;
    for (const auto& b : kBriefs) if (it.label == b.label) it.brief = b.brief;
    if (it.brief.empty()) {
      const size_t dot = it.help.find(". ");
      it.brief = it.help.substr(0, dot == std::string::npos ? it.help.size() : dot);
      if (!it.brief.empty() && it.brief.back() == '.') it.brief.pop_back();
    }
  }
  refresh_values();
}

// ---------------------------------------------------------------------------------------------------------------- Add to Steam
// A non-Steam shortcut: one entry in userdata\<account>\config\shortcuts.vdf (binary VDF) plus five pictures in config\grid. Steam rewrites that file when
// it exits, so Steam has to be closed while we write. The pictures are drawn here from our own shapes (never from the game's files).
uint32_t crc32_of(const std::string& s) {
  static uint32_t t[256];
  static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) c = t[(c ^ ch) & 255] ^ (c >> 8);
  return ~c;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
std::string reg_string(HKEY root, const wchar_t* sub, const wchar_t* val) {
  HKEY k = nullptr;
  if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return "";
  wchar_t buf[1024] = {};
  DWORD sz = sizeof buf - sizeof(wchar_t), ty = 0;
  const bool ok = RegQueryValueExW(k, val, nullptr, &ty, (LPBYTE)buf, &sz) == ERROR_SUCCESS && ty == REG_SZ;
  RegCloseKey(k);
  return ok ? narrow(buf) : "";
}
bool steam_test_mode() { return std::getenv("SNAP_STEAM_DIR") != nullptr; }   // tests point this at a scratch folder; the real Steam is never touched
std::string steam_dir() {
  std::string d;
  if (const char* e = std::getenv("SNAP_STEAM_DIR")) d = e;
  else {
    d = reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (d.empty()) d = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
  }
  for (char& c : d) if (c == '/') c = '\\';
  std::error_code ec;
  return (!d.empty() && fs::is_directory(P(d + "\\userdata"), ec)) ? d : "";
}
bool steam_is_running() {
  if (steam_test_mode()) { const char* r = std::getenv("SNAP_STEAM_RUNNING"); return r && *r == '1'; }
  bool found = false;
  const HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (s == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof pe;
  for (BOOL ok = Process32FirstW(s, &pe); ok && !found; ok = Process32NextW(s, &pe)) found = _wcsicmp(pe.szExeFile, L"steam.exe") == 0;
  CloseHandle(s);
  return found;
}
std::vector<std::string> steam_users(const std::string& dir) {
  std::vector<std::string> u;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(P(dir + "\\userdata"), ec)) {
    const std::string n = narrow(e.path().filename().wstring());
    if (!e.is_directory(ec) || n.empty() || n == "0" || n.find_first_not_of("0123456789") != std::string::npos) continue;
    u.push_back(n);
  }
  return u;
}
std::string self_exe() {
  wchar_t b[1024] = {};
  GetModuleFileNameW(nullptr, b, 1023);
  return narrow(b);
}

// binary VDF
struct VNode { int t = 0; std::string k, s; int32_t i = 0; std::vector<VNode> c; };
bool vdf_read(const std::string& d, size_t& p, std::vector<VNode>& out, int depth = 0) {
  while (p < d.size()) {
    const int t = (unsigned char)d[p++];
    if (t == 8) return true;
    VNode n; n.t = t;
    size_t z = d.find('\0', p);
    if (z == std::string::npos) return false;
    n.k = d.substr(p, z - p); p = z + 1;
    if (t == 0) { if (depth > 8 || !vdf_read(d, p, n.c, depth + 1)) return false; }
    else if (t == 1) { z = d.find('\0', p); if (z == std::string::npos) return false; n.s = d.substr(p, z - p); p = z + 1; }
    else if (t == 2) { if (p + 4 > d.size()) return false; std::memcpy(&n.i, d.data() + p, 4); p += 4; }
    else if (t == 3 || t == 4 || t == 6) { if (p + 4 > d.size()) return false; n.s = d.substr(p, 4); p += 4; }
    else if (t == 7 || t == 10) { if (p + 8 > d.size()) return false; n.s = d.substr(p, 8); p += 8; }
    else return false;
    out.push_back(std::move(n));
  }
  return true;   // a file with no closing byte: take what was read
}
void vdf_write(const std::vector<VNode>& v, std::string& o) {
  for (const VNode& n : v) {
    o.push_back((char)n.t); o += n.k; o.push_back('\0');
    if (n.t == 0) { vdf_write(n.c, o); o.push_back((char)8); }
    else if (n.t == 1) { o += n.s; o.push_back('\0'); }
    else if (n.t == 2) { char b[4]; std::memcpy(b, &n.i, 4); o.append(b, 4); }
    else o += n.s;
  }
}
VNode vs(const char* k, const std::string& v) { VNode n; n.t = 1; n.k = k; n.s = v; return n; }
VNode vi(const char* k, int32_t v) { VNode n; n.t = 2; n.k = k; n.i = v; return n; }

struct ShortcutDef { std::string name, exe_q, start_q, icon, launch_opts; uint32_t appid = 0; };
ShortcutDef shortcut_def(const std::string& grid_dir) {
  ShortcutDef d;
  d.name = "Snap Recomp";
  const std::string exe = self_exe();
  d.exe_q = "\"" + exe + "\"";
  d.start_q = "\"" + exe.substr(0, exe.find_last_of('\\') + 1) + "\"";
  d.appid = crc32_of(d.exe_q + d.name) | 0x80000000u;   // how Steam names a non-Steam game's grid pictures
  if (!grid_dir.empty()) d.icon = grid_dir + "\\" + std::to_string(d.appid) + "_icon.png";
  return d;
}
VNode shortcut_entry(const char* key, const ShortcutDef& d) {
  VNode e; e.t = 0; e.k = key;
  e.c = {vi("appid", (int32_t)d.appid), vs("AppName", d.name), vs("Exe", d.exe_q), vs("StartDir", d.start_q), vs("icon", d.icon), vs("ShortcutPath", ""),
         vs("LaunchOptions", d.launch_opts), vi("IsHidden", 0), vi("AllowDesktopConfig", 1), vi("AllowOverlay", 1), vi("OpenVR", 0), vi("Devkit", 0), vs("DevkitGameID", ""),
         vi("DevkitOverrideAppID", 0), vi("LastPlayTime", 0), vs("FlatpakAppID", "")};
  VNode tags; tags.t = 0; tags.k = "tags";
  e.c.push_back(tags);
  return e;
}
bool read_file_bytes(const std::string& path, std::string& out) {
  std::ifstream f(P(path), std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
// index of our entry in the shortcuts list, or -1
int find_entry(const std::vector<VNode>& list, const ShortcutDef& d) {
  for (size_t i = 0; i < list.size(); ++i)
    for (const VNode& f : list[i].c) if (f.t == 1 && _stricmp(f.k.c_str(), "Exe") == 0 && _stricmp(f.s.c_str(), d.exe_q.c_str()) == 0) return (int)i;
  return -1;
}
bool shortcut_present(const std::string& vdf_path) {
  std::string d;
  if (!read_file_bytes(vdf_path, d)) return false;
  std::vector<VNode> root;
  size_t p = 0;
  if (!vdf_read(d, p, root)) return false;
  const ShortcutDef def = shortcut_def("");
  for (const VNode& n : root) if (n.t == 0 && _stricmp(n.k.c_str(), "shortcuts") == 0) return find_entry(n.c, def) >= 0;
  return false;
}
bool shortcut_write(const std::string& vdf_path, const ShortcutDef& def_in) {
  ShortcutDef def = def_in;
  std::string d;
  std::vector<VNode> root;
  if (read_file_bytes(vdf_path, d)) {
    size_t p = 0;
    if (!vdf_read(d, p, root)) return false;   // a file we cannot read is left alone
    std::error_code ec;
    const std::string bak = vdf_path + ".snaprecomp.bak";   // first change only
    if (!fs::exists(P(bak), ec)) fs::copy_file(P(vdf_path), P(bak), ec);
  }
  VNode* sc = nullptr;
  for (VNode& n : root) if (n.t == 0 && _stricmp(n.k.c_str(), "shortcuts") == 0) sc = &n;
  if (!sc) { VNode n; n.t = 0; n.k = "shortcuts"; root.push_back(n); sc = &root.back(); }
  const int at = find_entry(sc->c, def);
  if (get_device() == "steamdeck") def.launch_opts = "--play --steam-deck";   // Device is Steam Deck: start straight into the game with the Deck preset
  if (at >= 0) for (const VNode& f : sc->c[(size_t)at].c) if (f.t == 1 && _stricmp(f.k.c_str(), "LaunchOptions") == 0 && !f.s.empty()) def.launch_opts = f.s;   // keep what the entry already has
  if (at >= 0) {
    // Update the fields we own in place: the user's tags, IsHidden, LastPlayTime and other settings on the entry stay.
    VNode& e = sc->c[(size_t)at];
    const auto set_s = [&e](const char* k, const std::string& v) {
      for (VNode& f : e.c) if (f.t == 1 && _stricmp(f.k.c_str(), k) == 0) { f.s = v; return; }
      e.c.push_back(vs(k, v));
    };
    set_s("Exe", def.exe_q);
    set_s("StartDir", def.start_q);
    set_s("icon", def.icon);
    bool had_appid = false;
    for (VNode& f : e.c) if (f.t == 2 && _stricmp(f.k.c_str(), "appid") == 0) { f.i = (int32_t)def.appid; had_appid = true; }
    if (!had_appid) e.c.insert(e.c.begin(), vi("appid", (int32_t)def.appid));
  }
  else {
    int next = 0;
    for (const VNode& e : sc->c) next = std::max(next, std::atoi(e.k.c_str()) + 1);
    sc->c.push_back(shortcut_entry(std::to_string(next).c_str(), def));
  }
  std::string out;
  vdf_write(root, out);
  out.push_back((char)8);
  return write_atomic(vdf_path, out);
}
void refresh_steam_status() {
  const std::string dir = steam_dir();
  g.steam_status = 0;
  if (dir.empty()) return;
  g.steam_status = 1;
  for (const std::string& u : steam_users(dir)) if (shortcut_present(dir + "\\userdata\\" + u + "\\config\\shortcuts.vdf")) g.steam_status = 2;
}

// PNG through the Windows imaging component (the same way the photo export writes its files)
bool write_png(const std::string& path, int w, int h, const unsigned char* bgra) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IWICImagingFactory* f = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
    const std::wstring tmp = P(path).wstring() + L".tmp";
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize((UINT)w, (UINT)h))) {
      WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
      fr->SetPixelFormat(&pf);
      ok = SUCCEEDED(fr->WritePixels((UINT)h, (UINT)w * 4, (UINT)(w * h * 4), (BYTE*)bgra)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (pb) pb->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
    if (ok) ok = MoveFileExW(tmp.c_str(), P(path).wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

// ---- the artwork: a vector sky and hills, a lens mark and the name; drawn in "board" units and scaled to the picture or to a preview ----
struct Board { int aw, ah; };
const Board kBoards[5] = {{300, 450}, {460, 215}, {960, 310}, {480, 270}, {128, 128}};   // capsule, wide capsule, hero, logo, icon
const char* const kBoardFile[5] = {"p.png", ".png", "_hero.png", "_logo.png", "_icon.png"};
const int kBoardPx[5][2] = {{600, 900}, {920, 430}, {1920, 620}, {960, 540}, {256, 256}};

void art_hill(float aw, float ah, float base, float amp, float ph, ImU32 col) {
  std::vector<ImVec2> pts;
  for (float x = -10; x <= aw + 10; x += 6) pts.push_back(V(x, base + amp * std::sin(x * 5.2f / aw + ph) + 0.4f * amp * std::sin(x * 12.0f / aw + ph * 1.7f)));
  pts.push_back(V(aw + 10, ah + 10));
  pts.push_back(V(-10, ah + 10));
  ImGui::GetForegroundDrawList()->AddConcavePolyFilled(pts.data(), (int)pts.size(), col);
}
void art_scene(float aw, float ah) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilledMultiColor(V(0, 0), V(aw, ah), C(0x8fbde6), C(0x8fbde6), C(0xf3ecd2), C(0xf3ecd2));
  const float r = std::min(aw, ah) * 0.11f;
  dl->AddCircleFilled(V(aw * 0.80f, ah * 0.22f), r * 1.7f * g.s, C(0xfff6d6, 70), 48);
  dl->AddCircleFilled(V(aw * 0.80f, ah * 0.22f), r * g.s, C(0xfff2c0), 48);
  art_hill(aw, ah, ah * 0.60f, ah * 0.05f, 0.4f, C(0x9ccf9a));
  art_hill(aw, ah, ah * 0.73f, ah * 0.05f, 2.0f, C(0x6fb589));
  art_hill(aw, ah, ah * 0.88f, ah * 0.04f, 4.1f, C(0x4f9a82));
}
void art_title(float x, float y, float sz, const char* t, int al) {
  text(x + sz * 0.035f, y + sz * 0.05f, sz, C(0x28503f, 150), t, al);
  text(x, y, sz, C(0xffffff), t, al);
}
void art_mark(float cx, float cy, float r) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddCircleFilled(V(cx, cy), r * 1.05f * g.s, C(0x28503f, 70), 48);
  aperture(cx, cy, r, C(0xffffff));
}
void art_draw(int kind) {   // draws board `kind` at the current mapping (g.ox / g.oy / g.s)
  const float aw = (float)kBoards[kind].aw, ah = (float)kBoards[kind].ah;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(V(0, 0), V(aw, ah), true);
  if (kind == 0) { art_scene(aw, ah); art_mark(aw / 2, ah * 0.37f, 52); art_title(aw / 2, ah * 0.70f, 56, "Snap", 1); art_title(aw / 2, ah * 0.82f, 56, "Recomp", 1); }
  else if (kind == 1) { art_scene(aw, ah); art_mark(86, ah * 0.5f, 40); art_title(140, ah * 0.5f, 54, "Snap Recomp", 0); }
  else if (kind == 2) { art_scene(aw, ah); }
  else if (kind == 3) { art_mark(66, ah * 0.5f, 40); art_title(122, ah * 0.5f, 56, "Snap Recomp", 0); }
  else {
    rrect(0, 0, aw, ah, 26, C(0x6e7180));
    rrect(14, 14, aw - 28, ah - 28, 20, C(0x96c283));
    aperture(aw / 2, ah / 2, 38, C(0xffffff));
  }
  dl->PopClipRect();
}
// a preview inside the dialog: the board scaled into the design-space rectangle (x, y, w)
void art_preview(int kind, float x, float y, float w) {
  const ImVec2 o = V(x, y);
  const float os = g.s, ox = g.ox, oy = g.oy;
  ImFont* of = g.font;
  g.ox = o.x; g.oy = o.y; g.s = os * (w / (float)kBoards[kind].aw);
  if (g.art_font) g.font = g.art_font;
  art_draw(kind);
  g.s = os; g.ox = ox; g.oy = oy; g.font = of;
}

struct Img { int w = 0, h = 0; std::vector<unsigned char> bgra; };
// Renders board `kind` into a texture, outside the normal frame, and reads it back as straight (non-premultiplied) BGRA.
bool render_art(int kind, Img& out) {
  const int w = kBoardPx[kind][0], h = kBoardPx[kind][1];
  const bool alpha = kind == 3;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = (UINT)w; d.Height = (UINT)h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11Texture2D* st = nullptr;
  bool ok = false;
  if (SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &tex)) && SUCCEEDED(g.dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ods = io.DisplaySize;
    const float os = g.s, ox = g.ox, oy = g.oy;
    ImFont* of = g.font;
    io.DisplaySize = ImVec2((float)w, (float)h);
    g.ox = 0; g.oy = 0; g.s = (float)w / (float)kBoards[kind].aw;
    if (g.art_font) g.font = g.art_font;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    art_draw(kind);
    ImGui::Render();
    const float clear[4] = {alpha ? 0.0f : 1.0f, alpha ? 0.0f : 1.0f, alpha ? 0.0f : 1.0f, alpha ? 0.0f : 1.0f};
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    io.DisplaySize = ods; g.s = os; g.ox = ox; g.oy = oy; g.font = of;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(g.dev->CreateTexture2D(&sd, nullptr, &st))) {
      g.ctx->CopyResource(st, tex);
      D3D11_MAPPED_SUBRESOURCE mp{};
      if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp))) {
        out.w = w; out.h = h; out.bgra.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y) {
          const unsigned char* s = (const unsigned char*)mp.pData + (size_t)y * mp.RowPitch;
          unsigned char* o = out.bgra.data() + (size_t)y * w * 4;
          for (int x = 0; x < w; ++x) {
            unsigned r = s[x * 4], gg = s[x * 4 + 1], b = s[x * 4 + 2], a = alpha ? s[x * 4 + 3] : 255;
            if (alpha && a > 0 && a < 255) { r = std::min(255u, r * 255 / a); gg = std::min(255u, gg * 255 / a); b = std::min(255u, b * 255 / a); }
            o[x * 4] = (unsigned char)b; o[x * 4 + 1] = (unsigned char)gg; o[x * 4 + 2] = (unsigned char)r; o[x * 4 + 3] = (unsigned char)a;
          }
        }
        g.ctx->Unmap(st, 0);
        ok = true;
      }
    }
  }
  if (st) st->Release();
  if (rtv) rtv->Release();
  if (tex) tex->Release();
  return ok;
}

// The whole job (called between frames, after the "Working" frame was shown).
void steam_add() {
  auto fail = [](const std::string& m) { g.steam_state = G::ST_FAIL; g.steam_msg = m; };
  const std::string dir = steam_dir();
  if (dir.empty()) { fail("Steam was not found on this PC."); return; }
  const std::vector<std::string> users = steam_users(dir);
  if (users.empty()) { fail("No Steam account has signed in on this PC yet. Start Steam once, sign in, close it, and try again."); return; }
  bool closed = false;
  if (steam_is_running()) {
    if (steam_test_mode()) { fail("Steam is running. Close it and try again."); return; }
    // Steam sets these for a game it started; shutting Steam down would close this launcher with it, half way through the job.
    wchar_t sv[8];
    if (GetEnvironmentVariableW(L"SteamGameId", sv, 8) || GetEnvironmentVariableW(L"SteamAppId", sv, 8)) {
      fail("This launcher was started from Steam, and the shortcut can only be added while Steam is closed. Close Steam yourself, start the launcher from its own exe, and try again.");
      return;
    }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + P(dir + "\\steam.exe").wstring() + L"\" -shutdown";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    for (int i = 0; i < 60 && steam_is_running(); ++i) Sleep(500);
    if (steam_is_running()) { fail("Steam did not close. Close it yourself, then try again."); return; }
    closed = true;
    Sleep(1500);   // it writes its files while it exits
  }
  Img im[5];
  for (int k = 0; k < 5; ++k) if (!render_art(k, im[k])) { fail("The artwork could not be drawn."); return; }
  std::string exe_dir_unused;
  int done = 0;
  uint32_t appid = 0;
  for (const std::string& u : users) {
    const std::string cfgdir = dir + "\\userdata\\" + u + "\\config";
    const std::string grid = cfgdir + "\\grid";
    std::error_code ec;
    fs::create_directories(P(grid), ec);
    const ShortcutDef def = shortcut_def(grid);
    appid = def.appid;
    bool ok = true;
    for (int k = 0; k < 5; ++k) ok = write_png(grid + "\\" + std::to_string(def.appid) + kBoardFile[k], im[k].w, im[k].h, im[k].bgra.data()) && ok;
    ok = shortcut_write(cfgdir + "\\shortcuts.vdf", def) && ok;
    if (ok) ++done;
  }
  if (closed) {   // put Steam back the way it was
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + P(dir + "\\steam.exe").wstring() + L"\"";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  }
  (void)appid;
  refresh_steam_status();
  if (done == 0) fail("The shortcut could not be written. Is the Steam folder read-only?");
  else {
    g.steam_state = G::ST_DONE;
    g.steam_msg = std::string("Added for ") + std::to_string(done) + (done == 1 ? " Steam account." : " Steam accounts.") + (closed ? " Steam was started again." : " Open Steam to find it in your library.");
  }
}

// ---------------------------------------------------------------------------------------------------------------- mods
// The same folder and mods.json the game reads (runtime/mods/mods_core.cpp): <root>\mods.json = {"packs":[{"id":"folder","enabled":true}, ...]} in
// priority order, a plain string means enabled, each pack is a folder with a pack.json. A pack that is on disk but not listed is off.
// "native-options" is the game's own PC settings tab; it is never shown and its entry is kept as it is. Changes apply at the next game start.
std::string mods_root_dir() {
  const char* e = std::getenv("SNAP_MODS_DIR");
  if (e && *e) return e;
  const char* ap = std::getenv("APPDATA");
  return std::string(ap ? ap : ".") + "\\SnapRecomp\\mods";
}
std::string lower_s(std::string t) { for (char& c : t) c = (char)std::tolower((unsigned char)c); return t; }

// Files two enabled packs both change, worked out by the game's own loader from mods.json as saved (zips are read from
// their file list, not unpacked). Refreshed after every load and save.
struct ModClash { std::string path, winner, loser, winner_file, loser_file; };   // winner / loser: pack ids; the files on disk (empty for a listed-only zip)
std::vector<ModClash> g_mod_clash;
int g_mod_clash_top = 0;   // first line shown in the details panel (mouse wheel)
void mods_find_conflicts() {
  g_mod_clash.clear();
  g_mod_clash_top = 0;
  mods::Index ix;
  std::vector<std::string> warn;
  mods::load(g.mods_root, ix, warn, false);
  for (const mods::Conflict& c : ix.conflicts) g_mod_clash.push_back({c.path, ix.packs[(size_t)c.winner].id, ix.packs[(size_t)c.loser].id, c.winner_file, c.loser_file});
}
// The conflicts one pack is part of (it won or lost).
std::vector<const ModClash*> mods_clash_of(const std::string& id) {
  std::vector<const ModClash*> v;
  for (const ModClash& c : g_mod_clash) if (c.winner == id || c.loser == id) v.push_back(&c);
  return v;
}

void load_mods() {
  g.mods.clear();
  g.mods_keep.clear();
  g.mods_root = mods_root_dir();
  JsonFile mj;
  mj.path = g.mods_root + "\\mods.json";
  mj.load();
  std::vector<std::pair<std::string, bool>> order;
  if (const hle::Json* arr = mj.j.get("packs")) {
    if (arr->type == hle::Json::Arr) {
      for (const hle::Json& e : arr->a) {
        if (e.type == hle::Json::Str) order.push_back({e.s, true});
        else if (e.type == hle::Json::Obj) {
          const std::string id = e.get("id") ? e.get("id")->str() : e.get("folder") ? e.get("folder")->str() : "";
          if (!id.empty()) order.push_back({id, e.get("enabled") ? e.get("enabled")->boolean(true) : true});
        }
      }
    }
  }
  // A pack is a folder or <id>.zip; its name and details come from the same reader the game uses (a zip is not unpacked for this).
  std::vector<std::string> warn;
  auto info = [&](const std::string& id, bool on, ModPack& out) {
    mods::Pack p;
    if (!mods::pack_info(g.mods_root, id, p, warn)) return false;
    out.id = id; out.enabled = on; out.zip = p.zip;
    out.name = p.name.empty() ? id : p.name; out.version = p.version; out.author = p.author; out.desc = p.description;
    return true;
  };
  std::vector<std::string> seen;
  auto known = [&](const std::string& id) { for (const std::string& t : seen) if (lower_s(t) == lower_s(id)) return true; return false; };
  for (const auto& o : order) {
    if (known(o.first)) continue;
    seen.push_back(o.first);
    ModPack p;
    if (o.first == "native-options" || !info(o.first, o.second, p)) { g.mods_keep.push_back(o); continue; }
    g.mods.push_back(p);
  }
  for (const std::string& id : mods::packs_on_disk(g.mods_root)) {   // packs mods.json does not list (name order): off until switched on
    ModPack p;
    if (id != "native-options" && !known(id) && info(id, false, p)) g.mods.push_back(p);
  }
  mods_find_conflicts();
}

void save_mods() {
  JsonFile mj;
  mj.path = g.mods_root + "\\mods.json";
  mj.load();
  hle::Json arr = hle::Json::array();
  auto add = [&](const std::string& id, bool on) {
    hle::Json o = hle::Json::object();
    o.set("id", hle::Json::string(id));
    o.set("enabled", hle::Json::boolean_(on));
    arr.a.push_back(o);
  };
  for (const ModPack& p : g.mods) add(p.id, p.enabled);
  for (const auto& k : g.mods_keep) add(k.first, k.second);
  mj.j.set("packs", arr);
  mj.save();
  mods_find_conflicts();   // the list follows mods.json as saved
}

int mods_on() { int n = 0; for (const ModPack& p : g.mods) n += p.enabled ? 1 : 0; return n; }

void mods_toast(const std::string& t) { g.toast = t; g.toast_until = ImGui::GetTime() + 2.5; }

void mods_toggle(int i) {
  if (i < 0 || i >= (int)g.mods.size()) return;
  g.mods[(size_t)i].enabled = !g.mods[(size_t)i].enabled;
  save_mods();
  mods_toast(g.mods[(size_t)i].name + (g.mods[(size_t)i].enabled ? " is on. It loads at the next start." : " is off."));
}
void mods_move(int i, int d) {   // d -1 = higher priority
  const int j = i + d;
  if (i < 0 || j < 0 || i >= (int)g.mods.size() || j >= (int)g.mods.size()) return;
  std::swap(g.mods[(size_t)i], g.mods[(size_t)j]);
  save_mods();
  g.row = j + 1;
}
void mods_open_folder() {
  std::error_code ec;
  fs::create_directories(P(g.mods_root), ec);
  if (std::getenv("SNAP_LAUNCHER_NO_SHELL")) { mods_toast("Mods folder: " + g.mods_root); return; }   // test switch: never opens Explorer
  ShellExecuteW(nullptr, L"explore", P(g.mods_root).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The shader cache the game fills (runtime/nvn/nvn_pipeline.cpp): <data dir>\shadercache, or SNAP_SHADERCACHE_DIR. Missing = no cache blob or keys yet;
// Building = another copy of the game is writing into it right now (a file changed in the last minute, or a half-written .tmp);
// Ready = the rest. The size is the whole folder.
void refresh_cache() {
  std::string dir;
  const char* e = std::getenv("SNAP_SHADERCACHE_DIR");
  dir = (e && *e) ? e : g.info->data_dir + "\\shadercache";
  g.cache_dir = dir;
  g.prep = snap_shaderprep::Progress();
  snap_shaderprep::read_progress(dir, g.prep);
  g.prep_stale = g.prep.state == 2 && !g.prep.settings.empty() && g.prep.settings != snap_shaderprep::settings_hash(g.info->data_dir);   // the picture settings changed since the sweep
  g.prep_driver = g.prep.state == 2 && !g.prep.driver.empty() && !snap_shaderprep::driver_stamp().empty() && g.prep.driver != snap_shaderprep::driver_stamp();   // the graphics driver changed since the sweep
  if (g.prep_driver) g.prep_stale = true;
  g.cache_state = 0;
  g.cache_bytes = 0;
  std::error_code ec;
  bool have_blob = false, fresh = false;
  const auto now = fs::file_time_type::clock::now();
  int files = 0;
  for (fs::recursive_directory_iterator it(P(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && files < 50000; it.increment(ec), ++files) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    g.cache_bytes += (unsigned long long)it->file_size(e2);
    const std::wstring n = it->path().filename().wstring();
    if (n == L"pipeline_cache.bin" || n == L"pipeline_keys.bin") have_blob = true;
    const auto age = now - it->last_write_time(e2);
    if (!e2 && age < std::chrono::seconds(60)) fresh = true;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, L".tmp") == 0) fresh = true;
  }
  g.cache_state = !have_blob ? 0 : fresh ? 2 : 1;
}
std::string size_text(unsigned long long b) {
  char t[32];
  if (b >= (1ull << 30)) std::snprintf(t, sizeof t, "%.1f GB", (double)b / (1ull << 30));
  else if (b >= (1ull << 20)) std::snprintf(t, sizeof t, "%d MB", (int)((b + (1ull << 19)) >> 20));
  else std::snprintf(t, sizeof t, "%d KB", (int)((b + 512) >> 10));
  return t;
}

// ---------------------------------------------------------------------------------------------------------------- drawing
std::string album_home_sub();   // launcher_album.inc
void modcmp_open(int mods_row);   // launcher_modcmp.inc: the "Which pack wins" page
struct Home { const char* label; const char* help; };
const Home kHome[] = {
    {"Play", "Starts the game with the settings shown on the right. This window closes while you play. Left and right switch profile; the last slot creates a new one."},
    {"Settings", "Display, picture quality, upscaling, frame rate, sound and controls. Everything you change applies when you press Play."},
    {"Saves", "Open the save folder, or import and export a save for a save manager or an emulator."},
    {"Album", "Browse the photos in your game's album, filter them by course, Pokemon, stars and score, open one full size, and export them as PNG files with a JSON and CSV list. The save is only read, never changed."},
    {"Mods", "Turn mod packs on or off, set which pack wins when two change the same file, and open the mods folder. Changes apply at the next start."},
    {"HD textures", "Sharper textures made from your own game files with a local AI model. Nothing is uploaded."},
    {"Add to Steam", "Adds Snap Recomp to your Steam library as its own game, with artwork, so you can start it from Big Picture or the Steam Deck."},
    {"Updates", "Check for a new version, or go back to the previous one. Nothing installs without you."},
    {"Benchmark", "Plays one scripted ride through Florio Nature Park on a scratch save (yours is never touched), then shows average fps, 1% and 0.1% lows, a frame-time graph and GPU time per pass. Takes about two minutes."},
    {"Game folder", "Pick a different folder with your extracted game files."},
};
constexpr int kHomeN = 10;
// 0 = still checking the game folder, 1 = passed, 2 = needs attention (UiInfo::verify, else the plain game_ok flag)
// Test switch: SNAP_LAUNCHER_UI_CHECKING=1 shows the "checking" look for a picture; "2:24:63" also shows reason 2, 24 of 63 files.
int verify_state() {
  static const char* const forced = std::getenv("SNAP_LAUNCHER_UI_CHECKING");
  static const bool force_checking = forced != nullptr;
  if (force_checking) {
    int why = 0, n = 0, t = 0;
    if (g.info->verify && std::sscanf(forced, "%d:%d:%d", &why, &n, &t) == 3) { g.info->verify->why.store(why); g.info->verify->files.store(n); g.info->verify->total.store(t); }
    return 0;
  }
  return g.info->verify ? g.info->verify->state.load() : (g.info->game_ok ? 1 : 2);
}
// Words for the verify state: why a full look was needed, how long ago the folder last passed, how far along the look is.
const char* verify_why_text(int why) {
  switch (why) {
    case 1: return "First check of this folder";
    case 2: return "Update installed";
    case 3: return "Game folder changed";
    case 4: return "Game files changed";
    case 5: return "Checked again on request";
    default: return "";
  }
}
std::string ago_text(long long at) {
  if (at <= 0) return "";
  const long long d = (long long)std::time(nullptr) - at;
  if (d < 90) return "just now";
  if (d < 3600) return std::to_string(d / 60) + " min ago";
  if (d < 86400) return std::to_string(d / 3600) + " h ago";
  return std::to_string(d / 86400) + (d < 2 * 86400 ? " day ago" : " days ago");
}
// 0..1 when the folder's file count is known (the builder publishes it), else -1: the bar then just moves, steadily.
float verify_fraction() {
  if (!g.info->verify) return -1.f;
  const int t = g.info->verify->total.load(), n = g.info->verify->files.load();
  return t > 0 ? std::min(1.f, (float)n / (float)t) : -1.f;
}
// Play waits for the check; when it failed the same button takes you to the folder picker instead.
void go_play(bool safe = false) {
  const int vs = verify_state();
  if (vs == 0) { g.toast = "Still checking your game files"; g.toast_until = ImGui::GetTime() + 2.5; return; }
  g.result = vs == 2 ? UiResult::GameFolder : safe ? UiResult::SafeMode : UiResult::Play;   // safe mode: docs/safe-mode.md
  g.done = true;
}
const UiResult kHomeAct[kHomeN] = {UiResult::Play, UiResult::Quit, UiResult::Quit, UiResult::Quit, UiResult::Quit, UiResult::HdTextures, UiResult::Quit, UiResult::Updates, UiResult::Benchmark, UiResult::GameFolder};
enum { H_PLAY, H_SETTINGS, H_SAVES, H_ALBUM, H_MODS, H_HD, H_STEAM, H_UPDATES, H_BENCH, H_FOLDER };

void hill(float base, float amp, float freq, float ph, ImU32 col) {
  std::vector<ImVec2> pts;
  for (float x = -20; x <= 1300; x += 20) pts.push_back(V(x, base + amp * std::sin(x * freq + ph) + 0.4f * amp * std::sin(x * freq * 2.3f + ph * 1.7f)));
  pts.push_back(V(1300, 740));
  pts.push_back(V(-20, 740));
  ImGui::GetForegroundDrawList()->AddConcavePolyFilled(pts.data(), (int)pts.size(), col);
}
void scene() {   // original vector landscape, softened under a haze like the game's blurred backdrop
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 a = ImVec2(0, 0), b = ImGui::GetIO().DisplaySize;
  dl->AddRectFilledMultiColor(a, b, C(0x9ec6e8), C(0x9ec6e8), C(0xf3ecd2), C(0xf3ecd2));
  dl->AddCircleFilled(V(1020, 180), 78 * g.s, C(0xfff6d6, 200), 48);
  dl->AddCircleFilled(V(1020, 180), 120 * g.s, C(0xfff6d6, 60), 48);
  hill(380, 36, 0.006f, 0.4f, C(0x9ccf9a));
  hill(480, 34, 0.007f, 2.0f, C(0x6fb589));
  hill(610, 28, 0.008f, 4.1f, C(0x4f9a82));
  for (int i = 0; i < 5; ++i) {
    const float x = 150.0f + i * 260.0f, y = 430.0f + (i % 2) * 40.0f;
    dl->AddCircleFilled(V(x, y), (40 + (i % 3) * 6) * g.s, C(0x3f7f6a, 220), 24);
    dl->AddRectFilled(V(x - 8, y + 6), V(x + 8, y + 90), C(0x3f7f6a, 220));
  }
  dl->AddRectFilled(a, b, C(0x8f9a90, 120));   // haze
}

bool hot(float x, float y, float w, float h) {
  if (g.block_hot) return false;
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float mx = (m.x - g.ox) / g.s, my = (m.y - g.oy) / g.s;
  return mx >= x && mx < x + w && my >= y && my < y + h;
}

void help_bar(const std::string& msg) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float H = ImGui::GetIO().DisplaySize.y;
  dl->AddRectFilled(ImVec2(0, g.oy + 656 * g.s), ImVec2(ImGui::GetIO().DisplaySize.x, H), C(0x5b5b5b, 225));
  diamond(22, 688, 5, C(0xffffff, 160));
  text_wrap2(40, 688, 19, C(0xffffff), msg, 1110);
  // back prompt
  const float x = 1186, y = 656, h = 64;
  ImVec2 p[5] = {V(x, y + h / 2), V(x + 28, y), V(1280, y), V(1280, y + h), V(x + 28, y + h)};
  dl->AddConvexPolyFilled(p, 5, C(kOrange));
  const ImVec2 c = V(1224, y + h / 2);
  dl->AddCircleFilled(c, 14 * g.s, C(kCream), 24);
  const ImVec2 ts = g.font->CalcTextSizeA(15 * g.s, FLT_MAX, 0, "B");
  dl->AddText(g.font, 15 * g.s, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), C(kOrDark), "B");
  {
    ImVec2 ar[5] = {V(1262, y + 20), V(1262, y + 34), V(1246, y + 34), V(1252, y + 28), V(1246, y + 34)};
    dl->AddPolyline(ar, 3, C(kOrDark), 0, 3.5f * g.s);
    ImVec2 ah[3] = {V(1254, y + 25), V(1246, y + 34), V(1254, y + 43)};
    dl->AddPolyline(ah, 3, C(kOrDark), 0, 3.5f * g.s);
  }
}

void draw_settings(bool* click_back) {
  Page& pg = g.pages[(size_t)g.tab];
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  scene();
  // tabs
  const int nt = (int)g.pages.size();
  const float tx0 = 98, tw = (1084 - (nt - 1) * 6.0f) / nt;
  for (int i = 0; i < nt; ++i) {
    const float x = tx0 + i * (tw + 6);
    ImVec2 q[4] = {V(x, 92), V(x + 6, 48), V(x + tw - 6, 48), V(x + tw, 92)};
    dl->AddConvexPolyFilled(q, 4, C(i == g.tab ? kPanel : kTab));
    dl->AddRectFilled(V(x + 6, 46), V(x + tw - 6, 56), C(i == g.tab ? kPanel : kTab), 8 * g.s);
    text(x + tw / 2, 69, 20, C(0xf4f4f6), g.pages[(size_t)i].name.c_str(), 1);
    if (hot(x, 46, tw, 46) && ImGui::IsMouseClicked(0)) { g.tab = i; g.row = 0; g.pulldown = false; }
  }
  shoulder(true, 14, 44, 62, 48);
  shoulder(false, 1204, 44, 62, 48);
  // the green double-chevron under the selected tab
  {
    const float cx = tx0 + g.tab * (tw + 6) + tw / 2;
    for (int s = -1; s <= 1; s += 2) {
      for (int k = 0; k < 2; ++k) {
        const float x = cx + s * (52 + k * 7);
        ImVec2 t[3] = {V(x, 89), V(x - s * 7, 85), V(x - s * 7, 93)};
        dl->AddConvexPolyFilled(t, 3, C(kGreen));
      }
    }
    dl->AddLine(V(cx - 42, 89), V(cx + 42, 89), C(kGreen), 2 * g.s);
  }
  // panel
  shadow(86, 92, 1108, 544, 14);
  dl->AddRectFilled(V(86, 92), V(1194, 636), C(kPanel), 14 * g.s, ImDrawFlags_RoundCornersBottom);
  diamond(104, 112, 6, C(0xffffff, 60)); diamond(118, 124, 6, C(0xffffff, 60));
  diamond(1176, 112, 6, C(0xffffff, 60)); diamond(1162, 124, 6, C(0xffffff, 60));
  // rows
  // one pitch on every page (eight rows fill the panel above the restore button); each row is a label with a short description under it
  const float ry0 = 134, pitch = 55, rh = 49, rx = 138, rw = 1004, vw = 340;
  int nrows = 0;
  for (size_t i = 0; i < pg.items.size(); ++i) {
    Item& it = pg.items[i];
    if (it.kind == K_INFO) continue;   // read-only rows are drawn as a table below the settings
    ++nrows;
    const float y = ry0 + (float)i * pitch;
    const bool sel = (int)i == g.row;
    if (hot(rx, y, rw, rh) && !g.pulldown) {
      if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) g.row = (int)i;
      if (ImGui::IsMouseClicked(0)) { g.row = (int)i; g.confirm_reset = false; if (it.kind == K_ACTION) { g.result = it.act; g.done = true; } else if (it.kind != K_INFO) { g.pulldown = true; g.pd_sel = std::max(0, index_of(it, it.cur)); g.pd_top = 0; } }
    }
    pointed(rx, y, rw, rh, 22, C(sel ? kGreen : kRow));
    // label, and under it what the row does (or, when another row overrides it, which value is in effect)
    text(rx + 44, y + rh * 0.35f, 21, C(0xf7f7f7), it.label.c_str());
    const std::string& sub = (!it.note.empty() && it.kind != K_ACTION) ? it.note : it.brief;
    if (!sub.empty()) text_clip(rx + 44, y + rh * 0.76f, 14, C(sel && !g_hc ? 0x3f4d30 : 0xf0f0f2), sub.c_str(), 600.0f);
    if (it.kind == K_ACTION) {
      prompt(rx + rw - 120, y + rh / 2, 36, "A");
      continue;
    }
    const float vx = rx + rw - 20 - vw, vy = y + 3;
    pointed(vx, vy, vw, rh - 6, 16, C(sel ? kYellow : kWhite));
    if (sel) pattern(vx + 16, vy, vw - 32, rh - 6);
    const float cy = vy + (rh - 6) / 2;
    ImVec2 tri[3] = {V(vx + 22, cy - 5), V(vx + 40, cy - 5), V(vx + 31, cy + 7)};
    dl->AddConvexPolyFilled(tri, 3, C(0x7a7b80));
    const std::string lab = label_of(it, it.cur);
    const int ci = index_of(it, it.cur);
    const bool dim = ci >= 0 && !it.ch[(size_t)ci].avail;
    text_clip(vx + vw / 2 + 12, cy, 21, C(dim ? 0x9a9a9a : kText), lab.c_str(), vw - 100, 1);
    if (sel) {
      // selection ring at the right end, like the game's cursor
      const ImVec2 c = V(vx + vw - 32, cy + 1);
      dl->AddCircleFilled(c, 13 * g.s, C(kWhite), 24);
      dl->AddCircle(c, 13 * g.s, C(kOrange), 24, 4 * g.s);
      dl->AddCircleFilled(c, 5 * g.s, C(kOrDark), 16);
    }
  }
  // the key, mouse and controller bindings (Controls page): two tables side by side, button / keys and mouse / controller
  {
    std::vector<const Item*> info;
    for (const Item& q : pg.items) if (q.kind == K_INFO) info.push_back(&q);
    if (!info.empty()) {
      const float ty = ry0 + (float)nrows * pitch + 4, colw = 492, rowh = 25;
      const size_t half = (info.size() + 1) / 2;
      for (int blk = 0; blk < 2; ++blk) {
        const float x0 = rx + blk * (rw - colw);
        text(x0 + 12, ty + 11, 14, C(0xd6d7de), "Button");
        text(x0 + 100, ty + 11, 14, C(0xd6d7de), "Keys and mouse");
        text(x0 + 345, ty + 11, 14, C(0xd6d7de), "Controller");
        const size_t b0 = blk == 0 ? 0 : half, b1 = blk == 0 ? half : info.size();
        for (size_t k = b0; k < b1; ++k) {
          const float yy = ty + 24 + (float)(k - b0) * (rowh + 2);
          rrect(x0, yy, colw, rowh, 12, C(0xffffff, 26));
          const Item& q = *info[k];
          const size_t tab = q.cur.find('\t');
          text(x0 + 12, yy + rowh / 2, 15, C(0xf7f7f7), q.label.c_str());
          text_clip(x0 + 100, yy + rowh / 2, 15, C(0xf7f7f7), q.cur.substr(0, tab).c_str(), 235.0f);
          text_clip(x0 + 345, yy + rowh / 2, 15, C(0xf7f7f7), tab == std::string::npos ? "" : q.cur.substr(tab + 1).c_str(), 140.0f);
        }
      }
      text(rx + 12, 584, 14, C(0xd6d7de), g.bindings_custom ? "These are your own bindings. Change them in the game: F1, then Bindings." : "Default bindings. Change them in the game: F1, then Bindings.");
    }
  }
  // restore defaults pill
  {
    const float x = 930, y = 594, w = 212, h = 34;
    const bool h_ = hot(x, y, w, h);
    dl->AddRectFilled(V(x, y), V(x + w, y + h), C(h_ || g.confirm_reset ? kYellow : kCream), 17 * g.s);
    dl->AddCircleFilled(V(x + 17, y + 17), 10 * g.s, C(0x8f9097), 16);
    dl->AddRectFilled(V(x + 12, y + 15.5f), V(x + 22, y + 18.5f), C(0xffffff));
    text(x + 36, y + 17, 15, C(0x666666), g.confirm_reset ? "Press X again to reset" : "Restore Default Settings");
    if (h_ && ImGui::IsMouseClicked(0)) { if (g.confirm_reset) { reset_page(pg); g.confirm_reset = false; g.toast = "Defaults restored"; g.toast_until = ImGui::GetTime() + 2; } else g.confirm_reset = true; }
  }
  // pulldown
  if (g.pulldown) {
    Item& it = pg.items[(size_t)g.row];
    const int vis = std::min<int>(8, (int)it.ch.size());
    const float ih = 46, w = 420, h = vis * ih + 20;
    const float x = rx + rw - 20 - vw + (vw - w);
    float y = ry0 + (float)g.row * pitch + rh + 4;
    if (y + h > 650) y = std::max(100.0f, ry0 + (float)g.row * pitch - h - 4);
    dl->AddRectFilled(V(86, 92), V(1194, 636), C(0x000000, 70), 14 * g.s, ImDrawFlags_RoundCornersBottom);
    shadow(x, y, w, h, 16);
    rrect(x, y, w, h, 16, C(kCream));
    if (g.pd_sel < g.pd_top) g.pd_top = g.pd_sel;
    if (g.pd_sel >= g.pd_top + vis) g.pd_top = g.pd_sel - vis + 1;
    for (int k = 0; k < vis; ++k) {
      const int i = g.pd_top + k;
      const Ch& c = it.ch[(size_t)i];
      const float yy = y + 10 + k * ih;
      const bool s = i == g.pd_sel;
      if (hot(x + 10, yy, w - 20, ih - 2)) {
        if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) g.pd_sel = i;
        if (ImGui::IsMouseClicked(0)) { choose(it, i); g.pulldown = false; }
      }
      if (s) { rrect(x + 10, yy, w - 20, ih - 2, 12, C(kYellow)); pattern(x + 22, yy, w - 44, ih - 2); dl->AddRect(V(x + 10, yy), V(x + w - 10, yy + ih - 2), C(kOrange), 12 * g.s, 0, 3 * g.s); }
      text(x + 30, yy + (ih - 2) / 2, 21, C(!c.avail ? 0xa8a6a4 : s ? kBrown : kText), c.label.c_str());
    }
  }
  // clicking outside closes a pulldown
  if (g.pulldown && ImGui::IsMouseClicked(0)) {
    // handled inside the list above; a click elsewhere on the dim area is ignored
  }
  (void)click_back;
  help_bar(g.confirm_reset ? "Press X (or click the button) again to put this page back to its defaults. Any other key cancels."
           : g.pulldown ? "Up and down choose, A confirms, B closes the list."
           : pg.items[(size_t)g.row].help);
  // toast
  if (ImGui::GetTime() < g.toast_until) {
    const float w = text_w(18, g.toast.c_str()) + 40;
    rrect(640 - w / 2, 108, w, 36, 18, C(kYellow));
    text(640, 126, 18, C(kBrown), g.toast.c_str(), 1);
  }
}

void draw_home_bg() {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 b = ImGui::GetIO().DisplaySize;
  dl->AddRectFilledMultiColor(ImVec2(0, 0), b, C(0xfbfaf8), C(0xf4f2f0), C(0xe9e6e2), C(0xf0eeeb));
  // a big faint emblem behind the card
  for (int r : {470, 410, 300}) dl->AddCircle(V(1130, 250), (float)r * g.s, C(0xc9c3bd, 140), 96, 2.0f * g.s);
  diamond(1130, 250, 120, C(0xd8d3ce, 140));
}

struct Chip { std::string text; unsigned dot, fg, bg; float bar = -2.f; };   // bar: -2 none, -1 steady moving bar (amount unknown), 0..1 the real amount

// (page, row) of the first item that matches, for the loadout rows that jump to a setting
bool find_pos(bool (*pred)(const Item&), int& pg, int& row) {
  for (size_t p = 0; p < g.pages.size(); ++p)
    for (size_t i = 0; i < g.pages[p].items.size(); ++i)
      if (pred(g.pages[p].items[i])) { pg = (int)p; row = (int)i; return true; }
  return false;
}
Item* find_item(bool (*pred)(const Item&)) {
  for (Page& p : g.pages) for (Item& it : p.items) if (pred(it)) return &it;
  return nullptr;
}

// Small line icons for the home menu rows, drawn from shapes in a 20-unit box with its centre at (cx, cy): no image files, they follow the palette.
void nav_icon(int row, float cx, float cy, ImU32 ink) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float th = 1.9f * g.s;
  auto L = [&](float x0, float y0, float x1, float y1) { dl->AddLine(V(cx + x0, cy + y0), V(cx + x1, cy + y1), ink, th); };
  auto P = [&](std::initializer_list<std::pair<float, float>> pts) {
    std::vector<ImVec2> v;
    for (const auto& q : pts) v.push_back(V(cx + q.first, cy + q.second));
    dl->AddPolyline(v.data(), (int)v.size(), ink, 0, th);
  };
  auto R = [&](float x0, float y0, float x1, float y1, float r) { dl->AddRect(V(cx + x0, cy + y0), V(cx + x1, cy + y1), ink, r * g.s, 0, th); };
  switch (row) {
    case H_SETTINGS:   // gear: ring with eight teeth
      dl->AddCircle(V(cx, cy), 5.2f * g.s, ink, 24, th);
      dl->AddCircleFilled(V(cx, cy), 1.8f * g.s, ink, 12);
      for (int i = 0; i < 8; ++i) { const float a = (float)i * 0.785398f; L(std::cos(a) * 7.0f, std::sin(a) * 7.0f, std::cos(a) * 9.6f, std::sin(a) * 9.6f); }
      break;
    case H_SAVES:      // memory card: a card with a cut corner and a contact strip
      P({{-7, -9}, {3, -9}, {8, -4}, {8, 9}, {-7, 9}, {-7, -9}});
      for (int i = 0; i < 3; ++i) L(-3.5f + i * 3.5f, -5.0f, -3.5f + i * 3.5f, -2.0f);
      L(-3.5f, 4.0f, 4.5f, 4.0f);
      break;
    case H_ALBUM:      // photo: frame, sun and hills
      R(-9, -7, 9, 7, 2.5f);
      dl->AddCircle(V(cx - 4, cy - 2.5f), 1.8f * g.s, ink, 12, th);
      P({{-9, 6}, {-3, 0}, {1, 4}, {4, 1}, {9, 6}});
      break;
    case H_MODS:       // puzzle block: a square with a knob on top
      R(-7, -4, 7, 9, 2.0f);
      dl->AddCircle(V(cx, cy - 6.5f), 2.6f * g.s, ink, 14, th);
      break;
    case H_HD:         // texture tiles: 2x2 squares and a sparkle
      R(-9, -9, -1, -1, 1.5f); R(1, -9, 9, -1, 1.5f); R(-9, 1, -1, 9, 1.5f);
      L(5, 2, 5, 8); L(2, 5, 8, 5);
      break;
    case H_STEAM:      // library: three books on a shelf line
      R(-8, -8, -3, 5, 1.2f); R(-2, -8, 3, 5, 1.2f);
      P({{4.5f, -7.5f}, {8.5f, -6.5f}, {6.0f, 5.0f}, {2.0f, 4.0f}});
      L(-9, 8.5f, 9, 8.5f);
      break;
    case H_UPDATES:    // arrow into a tray
      L(0, -9, 0, 3); L(-4.5f, -1.5f, 0, 3.2f); L(4.5f, -1.5f, 0, 3.2f);
      L(-8, 4, -8, 8.5f); L(-8, 8.5f, 8, 8.5f); L(8, 8.5f, 8, 4);
      break;
    case H_BENCH:      // stopwatch
      dl->AddCircle(V(cx, cy + 1.5f), 7.2f * g.s, ink, 28, th);
      L(0, 1.5f, 3.6f, -1.8f); L(-2.2f, -8.4f, 2.2f, -8.4f); L(0, -8.4f, 0, -5.6f);
      break;
    case H_FOLDER:     // folder with a tab
      P({{-9, 7.5f}, {-9, -6.5f}, {-3.5f, -6.5f}, {-1.5f, -4}, {9, -4}, {9, 7.5f}, {-9, 7.5f}});
      break;
  }
}

void draw_home() {
  draw_home_bg();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  UiInfo& in = *g.info;
  // title
  diamond(116, 72, 44, C(0x8f8e90));
  aperture(116, 72, 21, C(kCream));
  text(172, 62, 40, C(0x6a6561), "Snap Recomp");
  text(174, 98, 17, C(kGrey), "Works with your own copy of the game");
  if (in.update_available) {
    const std::string t = "Update " + in.update_version + " ready";
    const float w = text_w(18, t.c_str()) + 80;
    rrect(1250 - w, 38, w, 40, 20, C(kYellow));
    text(1250 - w + 20, 58, 18, C(kBrown), t.c_str());
    const float cx = 1250 - 24;
    dl->AddCircleFilled(V(cx, 58), 13 * g.s, C(kWhite), 20);
    text(cx, 58, 15, C(kOrDark), "Y", 1);
    if (hot(1250 - w, 38, w, 40) && ImGui::IsMouseClicked(0)) { g.result = UiResult::Updates; g.done = true; }
  }
  auto enter = [] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); };
  const bool mouse_moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;

  // the Play tile
  {
    const bool sel = g.home_sel == H_PLAY;
    const float x = 84, y = 128, w = 470, h = 104;
    if (hot(x, y, w, h)) {
      if (mouse_moved) g.home_sel = H_PLAY;
      if (ImGui::IsMouseClicked(0)) { g.home_sel = H_PLAY; enter(); }
    }
    shadow(x, y, w, h, 26);
    rrect(x, y, w, h, 26, C(kYellow));
    pattern(x + 22, y, w - 44, h);
    dl->AddRect(V(x, y), V(x + w, y + h), C(kOrange), 26 * g.s, 0, (sel ? 5.0f : 2.5f) * g.s);
    {   // "Safe mode" pill (also the X key while Play is selected): the same start with mods, HD packs, enhanced effects, frame generation, RT and the shader cache off
      const float px = x + w - 138, py = y + 14, pw = 116, ph = 28;
      const bool ph_hot = hot(px, py, pw, ph);
      if (ph_hot && ImGui::IsMouseClicked(0)) go_play(true);
      rrect(px, py, pw, ph, 14, C(ph_hot ? 0xfff3c4 : 0xfbe28a));
      dl->AddRect(V(px, py), V(px + pw, py + ph), C(kOrange), 14 * g.s, 0, (ph_hot ? 2.5f : 1.2f) * g.s);
      text(px + pw / 2, py + ph / 2, 15, C(kBrown), "Safe mode (X)", 1);
    }
    diamond(x + 66, y + h / 2, 46, C(0x8f8e90));
    aperture(x + 66, y + h / 2, 22, C(kCream));
    const int vs = verify_state();
    text(x + 134, y + 44, 48, C(kBrown), vs == 0 ? "Verify" : vs == 2 ? "Fix folder" : "Play");
    std::string pl;
    if (vs == 0) {   // the game folder is being verified: a bar under the label, Play comes back by itself
      const int n = in.verify ? in.verify->files.load() : 0;
      const int why = in.verify ? in.verify->why.load() : 0;
      const float fr = verify_fraction();
      pl = std::string(why ? verify_why_text(why) : "Looking at your game files") + (n ? " - " + std::to_string(n) + (in.verify && in.verify->total.load() ? " of " + std::to_string(in.verify->total.load()) : std::string()) + " files" : std::string());
      const double t = ImGui::GetTime();
      const float bx = x + 134, bw = w - 168 - 62, by = y + 72;
      rrect(bx, by, bw, 6, 3, C(0xe9c96a));
      if (fr >= 0.f) {   // the real amount
        rrect(bx, by, std::max(6.f, bw * fr), 6, 3, C(kOrange));
      } else {           // amount not known: a steady moving bar
        const float seg = bw * 0.28f, pos = (float)std::fmod(t * 0.8, 1.0) * (bw + seg) - seg;
        rrect(bx + std::max(0.f, pos), by, std::min(bw, pos + seg) - std::max(0.f, pos), 6, 3, C(kOrange));
      }
    } else if (vs == 2) {
      pl = "Press to pick the game folder again";
    } else if (!in.profiles.empty()) {
      pl = g.profile_idx < in.profiles.size() ? in.profiles[g.profile_idx].name : "+ New profile";
      if (sel) pl = "<  " + pl + "  >";
    }
    if (!pl.empty()) text_clip(x + 134, y + (vs == 0 ? 90 : 86), 19, C(0x7a6440), pl.c_str(), w - 150 - 62);   // leaves room for the A prompt at the right
  }

  // status chips
  std::vector<Chip> chips;
  {
    const int vs = verify_state();
    const int n = in.verify ? in.verify->files.load() : 0;
    const long long vat = in.verify ? in.verify->at.load() : 0;
    const std::string when = vs == 1 && vat > 0 ? " (" + ago_text(vat) + ")" : std::string();
    if (vs == 0) chips.push_back({"Verifying game files" + (n ? " (" + std::to_string(n) + ")" : std::string()), 0xe0a03au, 0x5f5f5fu, 0xffffffu, verify_fraction()});
    else chips.push_back({(vs == 1 ? "Game files ready" + when : std::string("Game folder needs attention")), vs == 1 ? 0x5aa070u : 0xc84a3au, vs == 1 ? 0x5f5f5fu : 0xc84a3au, 0xffffffu});
  }
  bool prep_chip_click = false;   // the shader chip starts or stops the first-run preparation (host/shaderprep.cpp)
  {
    static const char* const names[3] = {"Shader cache missing", "Shader cache ready", "Shader cache building"};
    static const unsigned dots[3] = {0xe0a03au, 0x5aa070u, 0xe0733au};
    const snap_shaderprep::Progress& pp = g.prep;
    std::string t = std::string(names[g.cache_state]) + (g.cache_state ? " (" + size_text(g.cache_bytes) + ")" : "");
    unsigned dot = dots[g.cache_state];
    float bar = -2.f;
    if (pp.state == 1) { t = "Preparing shaders " + std::to_string(pp.index) + " of " + std::to_string(pp.total) + " - press to stop"; dot = 0xe0733au; prep_chip_click = true; bar = pp.total > 0 ? std::min(1.f, (float)std::max(0, pp.index - 1) / (float)pp.total) : -1.f; }
    else if (pp.state == 2 && g.prep_stale) { t = g.prep_driver ? "Graphics driver changed - press to prepare shaders again" : "Settings changed - press to prepare shaders again"; dot = 0xe0a03au; prep_chip_click = true; }
    else if (pp.state == 2) t = "Shaders prepared (" + std::to_string(pp.pipelines) + " pipelines)";
    else if (pp.state == 3 || pp.state == 4) { t = std::string(pp.state == 3 ? "Shader preparation stopped" : "Shader preparation had problems") + " - press to retry"; dot = 0xe0a03au; prep_chip_click = true; }
    else { t += " - press to prepare"; prep_chip_click = true; }
    chips.push_back({t, dot, 0x5f5f5fu, 0xffffffu, bar});
  }
  if (in.update_available) chips.push_back({"Update " + in.update_version, 0xe0733au, 0xb85a28u, 0xfcefe6u});
  if (g.pad_found) chips.push_back({"Controller found", 0x5aa070u, 0x5f5f5fu, 0xffffffu});
  if (!g.gpu_name.empty()) chips.push_back({g.gpu_name + (g.gpu_rtx && file_exists("nvngx_dlss.dll") ? " - DLSS available" : ""), 0x3fa9a0u, 0x5f5f5fu, 0xffffffu});
  float cx = 84, cy = 246;
  int chip_rows = 1;
  bool chip_hover = false;
  for (const Chip& c : chips) {
    std::string t = c.text;
    float tw = text_w(15, t.c_str());
    if (tw > 330) { t = c.text; while (t.size() > 4 && text_w(15, (t + "..").c_str()) > 330) t.pop_back(); t += ".."; tw = text_w(15, t.c_str()); }
    const float w = tw + 44;
    if (cx + w > 554 && cx > 84) { if (chip_rows >= 2) break; cx = 84; cy += 37; ++chip_rows; }   // two rows at most; the menu below needs the room
    const bool verify_chip = &c == &chips[0];
    const bool chip_hot = verify_chip && verify_state() != 0 && in.verify_again && hot(cx, cy, w, 30);   // click = check the files again
    if (chip_hot) {
      chip_hover = true;
      if (ImGui::IsMouseClicked(0)) { in.verify_again(); g.toast = "Checking your game files again"; g.toast_until = ImGui::GetTime() + 2; }
    }
    const bool prep_chip = &c == &chips[1] && prep_chip_click;
    const bool prep_hot = prep_chip && hot(cx, cy, w, 30);
    if (prep_hot) {
      chip_hover = true;
      if (ImGui::IsMouseClicked(0)) {
        std::string perr;
        if (g.prep.state == 1) { snap_shaderprep::request_stop(g.cache_dir); g.toast = "Stopping after this course"; }
        else if (snap_shaderprep::start_detached(perr)) g.toast = "Preparing shaders in the background. You can keep using the launcher";
        else g.toast = perr;
        g.toast_until = ImGui::GetTime() + 3;
        g.prep_at = 0;
      }
    }
    shadow(cx, cy, w, 30, 15);
    rrect(cx, cy, w, 30, 15, C(c.bg));
    if (chip_hot || prep_hot) dl->AddRect(V(cx, cy), V(cx + w, cy + 30), C(kOrange), 15 * g.s, 0, 2.0f * g.s);
    dl->AddCircleFilled(V(cx + 16, cy + 15), 5 * g.s, C(c.dot), 12);
    text(cx + 30, cy + 15, 15, C(c.fg), t.c_str());
    if (c.bar > -2.f) {   // a thin bar along the bottom of the chip: the real amount, or a steady moving one when it is not known
      const float bx = cx + 20, bw = w - 40, by = cy + 25;
      rrect(bx, by, bw, 3, 1.5f, C(0xe9c96a));
      if (c.bar >= 0.f) rrect(bx, by, std::max(3.f, bw * c.bar), 3, 1.5f, C(kOrange));
      else { const float seg = bw * 0.3f, pos = (float)std::fmod(ImGui::GetTime() * 0.8, 1.0) * (bw + seg) - seg; rrect(bx + std::max(0.f, pos), by, std::min(bw, pos + seg) - std::max(0.f, pos), 3, 1.5f, C(kOrange)); }
    }
    cx += w + 8;
  }

  // the menu rows
  float y = cy + 30 + 12;
  for (int i = H_SETTINGS; i < kHomeN; ++i) {
    const bool sel = i == g.home_sel;
    const float x = 84, w = 470, h = 33;
    if (hot(x, y, w, h)) {
      if (mouse_moved) g.home_sel = i;
      if (ImGui::IsMouseClicked(0)) { g.home_sel = i; enter(); }
    }
    shadow(x, y, w, h, 18);
    rrect(x, y, w, h, 18, C(sel ? kYellow : 0xffffff));
    if (sel) { pattern(x + 18, y, w - 36, h); dl->AddRect(V(x, y), V(x + w, y + h), C(kOrange), 18 * g.s, 0, 3.5f * g.s); }
    nav_icon(i, x + 36, y + h / 2, C(sel ? kBrown : 0x8a8683));
    text(x + 62, y + h / 2, sel ? 23 : 21, C(sel ? kBrown : 0x6a6561), kHome[i].label);
    std::string sub;
    if (i == H_SETTINGS) sub = std::to_string(g.pages.size()) + " pages";
    else if (i == H_SAVES) sub = in.profiles.empty() ? "" : std::to_string(in.profiles.size()) + (in.profiles.size() == 1 ? " profile" : " profiles");
    else if (i == H_ALBUM) sub = album_home_sub();
    else if (i == H_MODS) sub = g.mods.empty() ? "No packs yet" : std::to_string(mods_on()) + " of " + std::to_string(g.mods.size()) + " on";
    else if (i == H_HD) sub = "Optional";
    else if (i == H_STEAM) sub = g.steam_status == 0 ? "Steam not found" : g.steam_status == 2 ? "Added" : "Not added";
    else if (i == H_UPDATES) sub = in.update_available ? in.update_version + " ready" : in.version;
    else if (i == H_BENCH) sub = "About 2 min";
    else if (i == H_FOLDER) sub = verify_state() == 0 ? "Verifying" : verify_state() == 1 ? (in.verify && in.verify->at.load() > 0 ? "Verified " + ago_text(in.verify->at.load()) : std::string("Ready")) : "Needs attention";
    if (!sub.empty()) text(x + w - 22, y + h / 2, 15, C(sel ? 0x7a6440 : 0x9a9795), sub.c_str(), 2);
    y += h + 3;
  }

  // TODO(hd light maps): add an "HD light maps" row to this loadout and a toggle on the HD textures page once the hdtex session defines a runtime
  // settings key for it (today the lighting level is builder-only, --set lighting.scale=).
  // the loadout card
  shadow(600, 128, 600, 492, 26);
  rrect(600, 128, 600, 492, 26, C(0xffffff));
  rrect(600, 128, 600, 70, 26, C(0x96c283), ImDrawFlags_RoundCornersTop);
  text(630, 163, 29, C(0xffffff), "Your loadout");
  {
    const std::string pn = (g.profile_idx < in.profiles.size()) ? in.profiles[g.profile_idx].name : "New profile";
    const float w = text_w(16, pn.c_str()) + 36;
    rrect(1170 - w, 146, w, 34, 17, C(0xffffff));
    text(1170 - w / 2, 163, 16, C(0x5f8a62), pn.c_str(), 1);
  }
  struct L { const char* label; bool (*pred)(const Item&); };
  const L ls[] = {
      {"Look", [](const Item& it) { return it.kind == K_LOOK; }},
      {"Window", [](const Item& it) { return it.key == "mode"; }},
      {"Frame pacing", [](const Item& it) { return it.kind == K_PACING; }},
      {"Resolution", [](const Item& it) { return it.key == "gfx_render_scale"; }},
      {"Anti-aliasing", [](const Item& it) { return it.kind == K_AA; }},
      {"Graphics preset", [](const Item& it) { return it.kind == K_PROFILE; }},
      {"Sound", [](const Item& it) { return it.key == "master"; }},
  };
  for (int i = 0; i < 7; ++i) {
    const float ly = 206 + i * 52, lx = 628, lw = 544;
    pointed(lx, ly, lw, 44, 20, C(kRow));
    text(lx + 40, ly + 22, 20, C(0xf7f7f7), ls[i].label);
    std::string val = "-";
    if (Item* it = find_item(ls[i].pred)) {
      val = label_of(*it, it->cur);
      if (it->kind == K_PACING) { if (it->cur == "vrr") val = "VRR - " + std::to_string(vrr_cap()) + " FPS"; }
      else if (it->key == "gfx_render_scale") {   // what the 3D picture really is drawn at, whichever row decided it
        const snappic::Eff e = snappic::effective(pic_cfg(), pic_ctx());
        val = snappic::dims(e.scene);
      }
      else if (it->kind == K_AA) {
        const snappic::Cfg c = pic_cfg();
        if (snappic::on(c)) {
          const std::string p = snappic::perf_row(c);
          val = snappic::backend_name(c) + (p == "off" ? std::string() : " " + std::string(1, (char)std::toupper((unsigned char)p[0])) + p.substr(1));
        }
      }
      else if (it->key == "master") val = "Volume " + label_of(*it, it->cur);
    }
    pointed(lx + 270, ly + 4, 252, 36, 14, C(kWhite));
    text_clip(lx + 270 + 126, ly + 22, 18, C(kText), val.c_str(), 214, 1);
    if (hot(lx, ly, lw, 44) && ImGui::IsMouseClicked(0)) {
      int pg = 0, row = 0;
      if (find_pos(ls[i].pred, pg, row)) { g.view = G::SETTINGS; g.tab = pg; g.row = row; }
    }
  }
  {
    std::string d = std::to_string(g.disp_w) + " x " + std::to_string(g.disp_h) + "  -  " + std::to_string(g.disp_hz) + " Hz";
    if (!g.gpu_name.empty()) d += "  -  " + g.gpu_name;
    text_clip(628, 592, 15, C(0x5f5f5f), d.c_str(), 340);
    text(1172, 592, 15, C(kGrey), "Change these in Settings", 2);
  }
  help_bar(chip_hover ? "Checks your game files again: the folder, and that it is the 2.0.1 game this build was made from." : kHome[g.home_sel].help);
  if (g.home_sel == H_PLAY) prompt(476, 198, 38, "A");   // inside the Play tile, bottom right (outside it the arrow ran into the loadout card)
  if (ImGui::GetTime() < g.toast_until) {
    const float w = text_w(18, g.toast.c_str()) + 40;
    rrect(640 - w / 2, 108, w, 36, 18, C(kYellow));
    text(640, 126, 18, C(kBrown), g.toast.c_str(), 1);
  }
}

// A pill button of the Steam dialog; returns true when clicked.
bool dlg_button(float x, float y, float w, float h, const char* label, const char* glyph, bool primary, bool enabled = true) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const bool over = enabled && hot(x, y, w, h);
  rrect(x, y, w, h, h / 2, C(!enabled ? 0xd8d6d4 : primary ? kYellow : 0xffffff));
  if (primary && enabled) dl->AddRect(V(x, y), V(x + w, y + h), C(kOrange), h / 2 * g.s, 0, (over ? 4.0f : 3.0f) * g.s);
  const ImVec2 c = V(x + 28, y + h / 2);
  dl->AddCircleFilled(c, 13 * g.s, C(0xf4f2f0), 20);
  const ImVec2 ts = g.font->CalcTextSizeA(14 * g.s, FLT_MAX, 0, glyph);
  dl->AddText(g.font, 14 * g.s, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), C(kOrDark), glyph);
  text(x + 54 + (w - 54) / 2 - 10, y + h / 2, 21, C(!enabled ? 0x9a9795 : primary ? kBrown : 0x6a6561), label, 1);
  return over && ImGui::IsMouseClicked(0);
}

void steam_close() { g.view = G::HOME; g.steam_state = G::ST_NONE; }
void steam_start() { if (g.steam_state == G::ST_ASK && g.steam_status > 0) { g.steam_state = G::ST_WORKING; g.steam_work = true; } }

void draw_steam() {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, C(0x20201f, 110));
  const float x = 210, y = 88, w = 860, h = 530;
  shadow(x, y, w, h, 32);
  rrect(x, y, w, h, 32, C(kCream));
  rrect(x, y, w, 64, 32, C(kPanel), ImDrawFlags_RoundCornersTop);
  text(x + 30, y + 32, 28, C(0xffffff), "Add to Steam");
  {
    const char* t = g.steam_status == 0 ? "Steam not found" : "Steam found";
    const float tw = text_w(16, t) + 34;
    rrect(x + w - 28 - tw, y + 16, tw, 32, 16, C(0xffffff));
    text(x + w - 28 - tw / 2, y + 32, 16, C(g.steam_status == 0 ? 0xc84a3a : 0x5f5f5f), t, 1);
  }
  // the artwork
  const float ay = y + 86;
  art_preview(0, x + 30, ay, 150);
  art_preview(1, x + 200, ay, 250);
  art_preview(2, x + 200, ay + 132, 250);
  rrect(x + 200, ay + 236, 64, 64, 14, C(0x6e7180));
  art_preview(4, x + 200, ay + 236, 64);
  text(x + 30, ay + 238, 12, C(kGrey), "Capsule");
  text(x + 200, ay + 224, 12, C(kGrey), "Wide capsule, hero banner and logo");
  text(x + 276, ay + 268, 12, C(kGrey), "Library icon");
  // the text and the facts
  const float tx = x + 480;
  text(tx, ay + 16, 18, C(0x666666), "Adds Snap Recomp to your Steam library as");
  text(tx, ay + 40, 18, C(0x666666), "its own game, with artwork, so you can");
  text(tx, ay + 64, 18, C(0x666666), "start it from Big Picture or the Steam Deck.");
  struct R { const char* a; std::string b; unsigned col; };
  std::string sv; unsigned sc;
  if (g.steam_status == 0) { sv = "not found"; sc = 0xc84a3a; }
  else if (g.steam_running) { sv = "running: closes first"; sc = 0xe0733a; }
  else { sv = "ready"; sc = 0x5f8a62; }
  const R rows[] = {{"Shortcut name", "Snap Recomp", 0x6a6561}, {"Artwork", "5 images", 0x5f8a62}, {"Steam", sv, sc}, {"Shortcut", g.steam_status == 2 ? "added (this updates it)" : "not added yet", g.steam_status == 2 ? 0x5f8a62u : 0x6a6561u}};
  for (int i = 0; i < 4; ++i) {
    const float ry = ay + 94 + i * 50;
    rrect(tx, ry, 340, 42, 14, C(0xffffff));
    text(tx + 16, ry + 21, 17, C(0x6a6561), rows[i].a);
    text(tx + 324, ry + 21, 17, C(rows[i].col), rows[i].b.c_str(), 2);
  }
  text_wrap2(x + 30, y + 410, 15, C(kGrey), "The shortcut starts this launcher. Nothing is installed into Steam, and you can remove it from Steam at any time.", w - 60);
  // the buttons and the result
  const float by = y + h - 76;
  if (g.steam_state == G::ST_ASK) {
    if (dlg_button(x + w - 30 - 280 - 16 - 150, by, 150, 50, "Cancel", "B", false)) steam_close();
    if (dlg_button(x + w - 30 - 280, by, 280, 50, g.steam_status == 0 ? "Steam not found" : g.steam_running ? "Close Steam and add" : "Add to Steam", "A", true, g.steam_status > 0)) steam_start();
  } else if (g.steam_state == G::ST_WORKING) {
    text(x + w - 30, by + 25, 20, C(0x6a6561), "Working...", 2);
  } else {
    const bool bad = g.steam_state == G::ST_FAIL;
    text_wrap2(x + 30, by + 25, 18, C(bad ? 0xc84a3a : 0x5f8a62), g.steam_msg, w - 30 - 200 - 40);
    if (dlg_button(x + w - 30 - 170, by, 170, 50, bad ? "Close" : "Done", "A", true)) steam_close();
  }
}

void draw_mods() {
  draw_home_bg();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  diamond(122, 82, 46, C(0x8f8e90));
  aperture(122, 82, 22, C(kCream));
  text(180, 70, 42, C(0x6a6561), "Mods");
  std::string head = g.mods.empty() ? "Loose-file mod packs" : "Packs higher in the list win when two change the same file";
  if (!g_mod_clash.empty())
    head = std::to_string(g_mod_clash.size()) + (g_mod_clash.size() == 1 ? " file is" : " files are") + " changed by two packs. The higher pack wins; select one to see which files.";
  text(182, 106, 17, C(g_mod_clash.empty() ? kGrey : 0xb0662e), head.c_str());
  if (!g_mod_clash.empty()) {   // opens the "Which pack wins" page (X)
    const float bx = 940, by = 92, bw = 260, bh = 38;
    const bool h = hot(bx, by, bw, bh);
    rrect(bx, by, bw, bh, 19, C(h ? 0xe0a06c : 0xd49a6a));
    text(bx + bw / 2, by + bh / 2, 17, C(0xffffff), "Compare conflicts (X)", 1);
    if (h && ImGui::IsMouseClicked(0)) modcmp_open(g.row);
  }
  const int n = (int)g.mods.size();
  g.row = std::clamp(g.row, 0, n);
  const bool moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
  const bool click = ImGui::IsMouseClicked(0);
  // row 0: the folder
  {
    const bool sel = g.row == 0;
    const float x = 84, y = 142, w = 740, h = 62;
    if (hot(x, y, w, h)) { if (moved) g.row = 0; if (click) { g.row = 0; mods_open_folder(); } }
    shadow(x, y, w, h, 20);
    rrect(x, y, w, h, 20, C(sel ? kYellow : 0xffffff));
    if (sel) { pattern(x + 20, y, w - 40, h); dl->AddRect(V(x, y), V(x + w, y + h), C(kOrange), 20 * g.s, 0, 4 * g.s); }
    text(x + 24, y + 22, sel ? 25 : 22, C(sel ? kBrown : 0x6a6561), "Open mods folder");
    text_clip(x + 24, y + 46, 14, C(sel ? 0x7a6440 : 0x9a9795), g.mods_root.c_str(), w - 110);
    if (sel) prompt(x + w - 20, y + h / 2, 44, "A");
  }
  // the packs
  const int vis = 5;
  if (g.row >= 1) { const int i = g.row - 1; if (i < g.mods_top) g.mods_top = i; if (i >= g.mods_top + vis) g.mods_top = i - vis + 1; }
  g.mods_top = std::clamp(g.mods_top, 0, std::max(0, n - vis));
  float y = 224;
  if (n == 0) {
    shadow(84, y, 740, 150, 22);
    rrect(84, y, 740, 150, 22, C(0xffffff));
    text(114, y + 36, 24, C(0x6a6561), "No mod packs yet");
    text_wrap2(114, y + 96, 18, C(kGrey), "A pack is a folder with a pack.json inside, or a .zip of one. Drop it into the mods folder, then come back to this page.", 680);
  }
  for (int k = g.mods_top; k < n && k < g.mods_top + vis; ++k) {
    const ModPack& p = g.mods[(size_t)k];
    const bool sel = g.row == k + 1;
    const float x = 84, w = 740, h = 62;
    const float bx = x + w - 20 - 92;       // the On/Off pill
    const float ux = bx - 14 - 40;          // move-down arrow; move-up arrow sits left of it
    const float up_x = ux - 8 - 40;
    if (hot(x, y, w, h)) {
      if (moved) g.row = k + 1;
      if (click) {
        g.row = k + 1;
        if (hot(bx, y + 13, 92, 36)) mods_toggle(k);
        else if (hot(up_x, y + 11, 40, 40)) mods_move(k, -1);
        else if (hot(ux, y + 11, 40, 40)) mods_move(k, +1);
      }
    }
    shadow(x, y, w, h, 20);
    rrect(x, y, w, h, 20, C(sel ? kYellow : 0xffffff));
    if (sel) { pattern(x + 20, y, w - 40, h); dl->AddRect(V(x, y), V(x + w, y + h), C(kOrange), 20 * g.s, 0, 4 * g.s); }
    dl->AddCircleFilled(V(x + 34, y + h / 2), 17 * g.s, C(p.enabled ? 0x96c283 : 0xb9b6b3), 24);
    text(x + 34, y + h / 2, 18, C(0xffffff), std::to_string(k + 1).c_str(), 1);
    text_clip(x + 66, y + 22, sel ? 24 : 22, C(sel ? kBrown : 0x6a6561), p.name.c_str(), up_x - 90);
    std::string sub = p.version.empty() ? p.id : "v" + p.version + "  -  " + p.id;
    if (p.zip) sub += "  -  zip";
    if (p.enabled) {
      const size_t nc = mods_clash_of(p.id).size();
      if (nc) sub += "  -  " + std::to_string(nc) + (nc == 1 ? " file shared" : " files shared");
    }
    text_clip(x + 66, y + 46, 14, C(sel ? 0x7a6440 : 0x9a9795), sub.c_str(), up_x - 90);
    for (int a = 0; a < 2; ++a) {   // move up / move down
      const float ax = a == 0 ? up_x : ux;
      const bool can = a == 0 ? k > 0 : k < n - 1;
      rrect(ax, y + 11, 40, 40, 12, C(can ? (hot(ax, y + 11, 40, 40) ? 0xe6e3df : 0xf0eeeb) : 0xf7f6f4));
      const float cx = ax + 20, cy = y + 31, d = a == 0 ? -1.0f : 1.0f;
      dl->AddTriangleFilled(V(cx - 8, cy - d * 4), V(cx + 8, cy - d * 4), V(cx, cy + d * 6), C(can ? 0x6a6561 : 0xd0cdc9));
    }
    rrect(bx, y + 13, 92, 36, 18, C(p.enabled ? 0x96c283 : 0xb9b6b3));
    text(bx + 46, y + 31, 18, C(0xffffff), p.enabled ? "On" : "Off", 1);
    y += h + 8;
  }
  if (n > 5) {   // scroll hint
    char t[32];
    std::snprintf(t, sizeof t, "%d - %d of %d", g.mods_top + 1, std::min(n, g.mods_top + vis), n);
    text(824, 626, 15, C(kGrey), t, 2);
  }
  // the selected pack
  {
    const float x = 860, y0 = 142, w = 340, h = 460;
    shadow(x, y0, w, h, 26);
    rrect(x, y0, w, h, 26, C(0xffffff));
    rrect(x, y0, w, 56, 26, C(0x96c283), ImDrawFlags_RoundCornersTop);
    text(x + 24, y0 + 28, 22, C(0xffffff), g.row >= 1 && g.row <= n ? "Pack details" : "How packs work");
    if (g.row >= 1 && g.row <= n) {
      const ModPack& p = g.mods[(size_t)g.row - 1];
      text_clip(x + 24, y0 + 90, 24, C(0x6a6561), p.name.c_str(), w - 48);
      std::string by = std::string(p.version.empty() ? "" : "Version " + p.version) + (p.version.empty() || p.author.empty() ? "" : "  -  ") + (p.author.empty() ? "" : "by " + p.author);
      if (!by.empty()) text_clip(x + 24, y0 + 122, 16, C(kGrey), by.c_str(), w - 48);
      std::string d = p.desc.empty() ? "No description." : p.desc;
      float ty = y0 + 164;
      size_t pos = 0;
      std::vector<const ModClash*> clash = p.enabled ? mods_clash_of(p.id) : std::vector<const ModClash*>();
      const int dlines = clash.empty() ? 6 : 2;   // the conflict list takes the rest of the panel
      for (int line = 0; line < dlines && pos < d.size(); ++line, ty += 26) {   // plain word wrap
        std::string a;
        while (pos < d.size()) {
          size_t sp = d.find(' ', pos);
          std::string word = d.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
          std::string trial = a.empty() ? word : a + " " + word;
          if (!a.empty() && text_w(17, trial.c_str()) > w - 48) break;
          a = trial;
          pos = sp == std::string::npos ? d.size() : sp + 1;
        }
        text(x + 24, ty, 17, C(0x6a6561), a.c_str());
      }
      if (!clash.empty()) {   // every file this pack shares with another enabled pack, grouped by who wins
        auto name_of = [](const std::string& id) { for (const ModPack& m : g.mods) if (m.id == id) return m.name; return id; };
        std::stable_sort(clash.begin(), clash.end(), [&](const ModClash* a, const ModClash* b) {
          const bool wa = a->winner == p.id, wb = b->winner == p.id;
          if (wa != wb) return wa;
          return (wa ? a->loser : a->winner) < (wb ? b->loser : b->winner);
        });
        std::vector<std::pair<std::string, ImU32>> lines;
        std::string last;
        for (const ModClash* c : clash) {
          const bool won = c->winner == p.id;
          const std::string h2 = (won ? "Wins over " : "Loses to ") + name_of(won ? c->loser : c->winner) + ":";
          if (h2 != last) { lines.push_back({h2, C(won ? 0x5f8a62 : 0xb0662e)}); last = h2; }
          lines.push_back({c->path, C(0x6a6561)});
        }
        const float ly = y0 + 226, lh = 20;
        const int vis_l = 7;
        text(x + 24, ly - 8, 15, C(kGrey), (std::to_string(clash.size()) + (clash.size() == 1 ? " file" : " files") + " also changed by another pack").c_str());
        if (hot(x, y0, w, h) && ImGui::GetIO().MouseWheel != 0) g_mod_clash_top -= (int)(ImGui::GetIO().MouseWheel * 3);
        static int last_row = -1;
        if (last_row != g.row) { g_mod_clash_top = 0; last_row = g.row; }
        g_mod_clash_top = std::clamp(g_mod_clash_top, 0, std::max(0, (int)lines.size() - vis_l));
        for (int k = 0; k < vis_l && g_mod_clash_top + k < (int)lines.size(); ++k) {
          const auto& L = lines[(size_t)(g_mod_clash_top + k)];
          const bool is_head = L.first.back() == ':';
          std::string s = L.first;
          const float maxw = w - 48 - (is_head ? 0 : 12);
          if (!is_head && text_w(14, s.c_str()) > maxw) {   // long paths keep their end (the file name)
            while (s.size() > 1 && text_w(14, (".." + s).c_str()) > maxw) s.erase(0, 1);
            s = ".." + s;
          }
          text(x + 24 + (is_head ? 0 : 12), ly + 14 + k * lh, is_head ? 15 : 14, L.second, s.c_str());
        }
        if ((int)lines.size() > vis_l) {
          char t[48];
          std::snprintf(t, sizeof t, "%d - %d of %d lines (scroll)", g_mod_clash_top + 1, std::min((int)lines.size(), g_mod_clash_top + vis_l), (int)lines.size());
          text(x + w - 24, ly + 14 + vis_l * lh, 13, C(kGrey), t, 2);
        }
      }
      text(x + 24, y0 + h - 76, 16, C(p.enabled ? 0x5f8a62 : kGrey), p.enabled ? "On: loads at the next start" : "Off");
      text_clip(x + 24, y0 + h - 44, 14, C(0x9a9795), (g.mods_root + "\\" + p.id + (p.zip ? ".zip" : "")).c_str(), w - 48);
    } else {
      float ty = y0 + 92;
      for (const char* para : {"Each pack replaces game files, such as textures, text and menus, without touching the game itself.",
                               "The order is the priority: pack 1 wins over pack 2. Use the arrows beside a pack, or Q and E, to move it.",
                               "Changes apply the next time you press Play."})
        ty += (float)text_para(x + 24, ty, 17, C(0x6a6561), para, w - 48, 26, 5) * 26 + 22;
    }
  }
  help_bar(g.row == 0 ? "Opens the mods folder in Explorer. Put each pack in its own folder with a pack.json, or drop in its .zip."
                       : "Enter turns the pack on or off. Q and E (or the arrows beside it) move it up or down the priority order. X compares the files two packs both change. Changes apply at the next start.");
  if (click && hot(1186, 656, 94, 64)) g.view = G::HOME;
}

#include "launcher_album.inc"   // the Album page (draw_album, album_input, album_open, album_test_view)
#include "launcher_modcmp.inc"   // the "Which pack wins" page (draw_modcmp, modcmp_input, modcmp_open)

void draw_saves() {
  draw_home_bg();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  (void)dl;
  diamond(122, 82, 46, C(0x8f8e90));
  aperture(122, 82, 22, C(kCream));
  text(180, 70, 42, C(0x6a6561), "Saves");
  text(182, 106, 17, C(kGrey), "Back up or move your save");
  const char* names[4] = {"Open save folder", "Import a save...", "Export the save...", "Restore a backup..."};
  const char* helps[4] = {"Opens the save folder of the selected profile in Explorer.",
                          "From a console backup (JKSV, Checkpoint) or an emulator. Your current save is kept as a backup.",
                          "Copy your save out for a save manager or an emulator.",
                          "Put back one of the automatic backups (made at every start and before every import), listed with date and time."};
  const char* briefs[4] = {"Opens the save folder of the selected profile in Explorer.",
                           "Bring in a save from a console backup or an emulator.",
                           "Copy your save out for a save manager or an emulator.",
                           "Go back to one of the automatic backups."};
  float y = 142;
  for (int i = 0; i < 4; ++i) {
    const bool sel = i == g.row;
    const float h = 72, x = 84, w = 740;   // the same height selected or not, so the rows below never jump
    if (hot(x, y, w, h)) {
      if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) g.row = i;
      if (ImGui::IsMouseClicked(0)) { g.row = i; ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); }
    }
    shadow(x, y, w, h, 20);
    rrect(x, y, w, h, 20, C(sel ? kYellow : 0xffffff));
    if (sel) { pattern(x + 20, y, w - 40, h); ImGui::GetForegroundDrawList()->AddRect(V(x, y), V(x + w, y + h), C(kOrange), 20 * g.s, 0, 4 * g.s); }
    text(x + 24, y + 26, sel ? 25 : 22, C(sel ? kBrown : 0x6a6561), names[i]);
    text_clip(x + 24, y + 53, 15, C(sel ? 0x7a6440 : 0x9a9795), briefs[i], w - 110);
    if (sel) prompt(x + w - 20, y + h / 2, 44, "A");
    y += h + 10;
  }
  help_bar(helps[std::min(std::max(g.row, 0), 3)]);
}

// ---------------------------------------------------------------------------------------------------------------- input
bool key(std::initializer_list<ImGuiKey> ks, bool repeat = false) {
  for (ImGuiKey k : ks) if (ImGui::IsKeyPressed(k, repeat)) return true;
  return false;
}

void handle_input() {
  const bool up = key({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}, true);
  const bool dn = key({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}, true);
  const bool lf = key({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}, true);
  const bool rt = key({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}, true);
  const bool ok = key({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown});
  const bool back = key({ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight});
  const bool prev = key({ImGuiKey_PageUp, ImGuiKey_Q, ImGuiKey_GamepadL1}, true);
  const bool next = key({ImGuiKey_PageDown, ImGuiKey_E, ImGuiKey_GamepadR1}, true);
  const bool x = key({ImGuiKey_X, ImGuiKey_Delete, ImGuiKey_GamepadFaceLeft});
  const bool y = key({ImGuiKey_Y, ImGuiKey_U, ImGuiKey_GamepadFaceUp});
  UiInfo& in = *g.info;
  if (g.view == G::STEAM) {
    if (g.steam_state == G::ST_WORKING) return;
    if (back) { steam_close(); return; }
    if (ok) { if (g.steam_state == G::ST_ASK) steam_start(); else steam_close(); }
    return;
  }
  if (g.view == G::HOME) {
    if (up) g.home_sel = (g.home_sel + kHomeN - 1) % kHomeN;
    if (dn) g.home_sel = (g.home_sel + 1) % kHomeN;
    if (y && in.update_available) { g.result = UiResult::Updates; g.done = true; return; }
    if (x && g.home_sel == H_PLAY) { go_play(true); return; }
    if (back) { g.result = UiResult::Quit; g.done = true; return; }
    if (g.home_sel == H_PLAY && !in.profiles.empty() && (lf || rt || ok)) {
      const int n = (int)in.profiles.size() + 1;   // the last slot is "New profile"
      if (ok && g.profile_idx == in.profiles.size()) {
        std::string id = in.new_profile ? in.new_profile() : "";
        if (!id.empty()) {
          UiProfile np; np.id = id; np.name = id;
          // the caller keeps names; re-read through the callback's side effect on info.profiles when it updates the list
          for (size_t i = 0; i < in.profiles.size(); ++i) if (in.profiles[i].id == id) { g.profile_idx = i; }
          if (g.profile_idx >= in.profiles.size()) { in.profiles.push_back(np); g.profile_idx = in.profiles.size() - 1; }
          in.profile_id = in.profiles[g.profile_idx].id;
          load_stores();
          g.toast = "New profile created"; g.toast_until = ImGui::GetTime() + 2.5;
        }
        return;
      }
      if (ok) { go_play(); return; }
      int i = (int)g.profile_idx + (lf ? -1 : 1);
      i = (i % n + n) % n;
      g.profile_idx = (size_t)i;
      if ((size_t)i < in.profiles.size()) { in.profile_id = in.profiles[(size_t)i].id; load_stores(); }
      return;
    }
    if (ok) {
      switch (g.home_sel) {
        case H_PLAY: go_play(); break;
        case H_SETTINGS: g.view = G::SETTINGS; g.row = 0; break;
        case H_SAVES: g.view = G::SAVES; g.row = 0; break;
        case H_ALBUM: album_open(); g.view = G::ALBUM; break;
        case H_MODS: load_mods(); g.view = G::MODS; g.row = 0; g.mods_top = 0; break;
        case H_STEAM:
          refresh_steam_status();
          g.steam_running = g.steam_status > 0 && steam_is_running();
          g.steam_state = G::ST_ASK; g.steam_msg.clear(); g.view = G::STEAM;
          break;
        default: g.result = kHomeAct[g.home_sel]; g.done = true; break;
      }
    }
    return;
  }
  if (g.view == G::ALBUM) { album_input(up, dn, lf, rt, ok, back, prev, next, x, y); return; }
  if (g.view == G::MODCMP) { modcmp_input(up, dn, ok, back); return; }
  if (g.view == G::MODS) {
    if (back) { g.view = G::HOME; return; }
    const int n = (int)g.mods.size();
    if (up) g.row = (g.row + n) % (n + 1);
    if (dn) g.row = (g.row + 1) % (n + 1);
    if (ok) { if (g.row == 0) mods_open_folder(); else mods_toggle(g.row - 1); }
    if (g.row >= 1 && prev) mods_move(g.row - 1, -1);
    if (g.row >= 1 && next) mods_move(g.row - 1, +1);
    if (x && !g_mod_clash.empty()) modcmp_open(g.row);
    return;
  }
  if (g.view == G::SAVES) {
    if (back) { g.view = G::HOME; return; }
    if (up) g.row = (g.row + 3) % 4;
    if (dn) g.row = (g.row + 1) % 4;
    if (ok) { g.result = g.row == 0 ? UiResult::OpenSaves : g.row == 1 ? UiResult::ImportSave : g.row == 2 ? UiResult::ExportSave : UiResult::RestoreBackup; g.done = true; }
    return;
  }
  // settings
  Page& pg = g.pages[(size_t)g.tab];
  if (g.pulldown) {
    Item& it = pg.items[(size_t)g.row];
    const int n = (int)it.ch.size();
    if (up) g.pd_sel = (g.pd_sel + n - 1) % n;
    if (dn) g.pd_sel = (g.pd_sel + 1) % n;
    if (ok) { choose(it, g.pd_sel); g.pulldown = false; }
    if (back) g.pulldown = false;
    return;
  }
  if (back) { if (g.confirm_reset) g.confirm_reset = false; else g.view = G::HOME; return; }
  const int nt = (int)g.pages.size();
  if (prev) { g.tab = (g.tab + nt - 1) % nt; g.row = 0; g.confirm_reset = false; }
  if (next) { g.tab = (g.tab + 1) % nt; g.row = 0; g.confirm_reset = false; }
  Page& p2 = g.pages[(size_t)g.tab];
  const int nr = (int)p2.items.size();
  int na = nr;   // the read-only rows (K_INFO) come last and are never selected
  while (na > 0 && p2.items[(size_t)na - 1].kind == K_INFO) --na;
  if (na < 1) na = 1;
  if (g.row >= na) g.row = na - 1;
  if (up) { g.row = (g.row + na - 1) % na; g.confirm_reset = false; }
  if (dn) { g.row = (g.row + 1) % na; g.confirm_reset = false; }
  Item& it = p2.items[(size_t)g.row];
  if (x) {
    if (g.confirm_reset) { reset_page(p2); g.confirm_reset = false; g.toast = "Defaults restored"; g.toast_until = ImGui::GetTime() + 2; }
    else g.confirm_reset = true;
    return;
  }
  if (it.kind == K_ACTION) { if (ok) { g.result = it.act; g.done = true; } return; }
  if (it.kind == K_INFO) return;
  if (lf) { g.confirm_reset = false; cycle(it, -1); }
  if (rt) { g.confirm_reset = false; cycle(it, +1); }
  if (ok) { g.confirm_reset = false; g.pulldown = true; g.pd_sel = std::max(0, index_of(it, it.cur)); g.pd_top = 0; }
}

// ---------------------------------------------------------------------------------------------------------------- window and D3D11
bool create_target() {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  const HRESULT hr = g.dev->CreateRenderTargetView(bb, nullptr, &g.rtv);
  bb->Release();
  return SUCCEEDED(hr);
}
void release_target() { if (g.rtv) { g.rtv->Release(); g.rtv = nullptr; } }

bool create_device() {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = g.hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL fl[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  for (D3D_DRIVER_TYPE t : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, t, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &sd, &g.sc, &g.dev, &got, &g.ctx))) return create_target();
  }
  return false;
}
void destroy_device() {
  release_target();
  if (g.sc) { g.sc->Release(); g.sc = nullptr; }
  if (g.ctx) { g.ctx->Release(); g.ctx = nullptr; }
  if (g.dev) { g.dev->Release(); g.dev = nullptr; }
}

// ---------------------------------------------------------------------------------------------------------------- title bar
// The window has no system frame (WM_NCCALCSIZE gives it all to the client area), so the title bar is drawn here: the lens mark, the name, and
// minimise / maximise / close at the right. Dragging, double-click to maximise and the resize edges are WM_NCHITTEST (wndproc). It is drawn in a
// 34-unit-high space scaled to g.tb pixels, on top of everything.
constexpr float kTbBtnW = 46;   // one caption button, in title-bar units
float tb_scale() { return g.tb > 0 ? g.tb / 34.0f : 1.0f; }
void draw_titlebar() {
  if (g.tb <= 0) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  const float os = g.s, ox = g.ox, oy = g.oy;
  g.s = tb_scale(); g.ox = 0; g.oy = 0;
  const float W = ds.x / g.s;
  dl->AddRectFilled(V(0, 0), V(W, 34), C(kPanel));
  dl->AddRectFilled(V(0, 33), V(W, 34), C(0x000000, 70));
  diamond(21, 17, 11, C(0x9a9da7));
  aperture(21, 17, 5.5f, C(kCream));
  text(40, 17, 15, C(0xffffff), "Snap Recomp");
  if (!g.info->version.empty()) text(40 + text_w(15, "Snap Recomp") + 10, 17, 13, C(0xd9dbe2), g.info->version.c_str());
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool zoomed = g.hwnd && IsZoomed(g.hwnd);
  for (int i = 0; i < 3; ++i) {   // 0 minimise, 1 maximise / restore, 2 close
    const float bx = W - kTbBtnW * (3 - i);
    const bool over = m.x >= bx * g.s && m.x < (bx + kTbBtnW) * g.s && m.y >= 0 && m.y < g.tb;
    if (over) rrect(bx, 0, kTbBtnW, 33, 0, i == 2 ? C(0xc42b1c) : C(0xffffff, 40));
    const ImU32 ink = C(0xffffff);
    const float cx = bx + kTbBtnW / 2, cy = 16.5f, th = 1.4f * g.s;
    if (i == 0) dl->AddLine(V(cx - 5, cy), V(cx + 5, cy), ink, th);
    else if (i == 1) {
      if (zoomed) {
        dl->AddRect(V(cx - 5, cy - 2), V(cx + 3, cy + 5), ink, 0, 0, th);
        dl->AddLine(V(cx - 2, cy - 5), V(cx + 5, cy - 5), ink, th);
        dl->AddLine(V(cx + 5, cy - 5), V(cx + 5, cy + 2), ink, th);
      } else dl->AddRect(V(cx - 5, cy - 5), V(cx + 5, cy + 5), ink, 0, 0, th);
    } else {
      dl->AddLine(V(cx - 5, cy - 5), V(cx + 5, cy + 5), ink, th);
      dl->AddLine(V(cx + 5, cy - 5), V(cx - 5, cy + 5), ink, th);
    }
    if (over && ImGui::IsMouseClicked(0) && g.hwnd) {
      if (i == 0) ShowWindow(g.hwnd, SW_MINIMIZE);
      else if (i == 1) ShowWindow(g.hwnd, zoomed ? SW_RESTORE : SW_MAXIMIZE);
      else { g.result = UiResult::Quit; g.done = true; }
    }
  }
  g.s = os; g.ox = ox; g.oy = oy;
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return TRUE;
  switch (m) {
    case WM_SIZE:
      if (g.dev && w != SIZE_MINIMIZED) { g.rw = LOWORD(l); g.rh = HIWORD(l); g.resize = true; }
      return 0;
    case WM_NCCALCSIZE:
      if (w && g.tb > 0) {   // no system frame: the client area is the whole window (a maximised window overhangs the screen by its frame, so pull it back)
        if (IsZoomed(h)) {
          using GetDpi = UINT(WINAPI*)(HWND);
          GetDpi gd = (GetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
          const UINT dpi = gd ? gd(h) : 96;
          using GetMetrics = int(WINAPI*)(int, UINT);
          GetMetrics gm = (GetMetrics)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
          const int fx = gm ? gm(SM_CXFRAME, dpi) + gm(SM_CXPADDEDBORDER, dpi) : GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
          const int fy = gm ? gm(SM_CYFRAME, dpi) + gm(SM_CXPADDEDBORDER, dpi) : GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
          RECT* rc = &((NCCALCSIZE_PARAMS*)l)->rgrc[0];
          rc->left += fx; rc->right -= fx; rc->top += fy; rc->bottom -= fy;
        }
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      if (g.tb <= 0) break;
      POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
      RECT wr{};
      GetWindowRect(h, &wr);
      const int edge = std::max(6, (int)(g.tb * 0.2f));
      const bool zoomed = IsZoomed(h) != 0;
      if (!zoomed) {
        const bool L = pt.x < wr.left + edge, R = pt.x >= wr.right - edge, T = pt.y < wr.top + edge, B = pt.y >= wr.bottom - edge;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
      }
      if (pt.y < wr.top + (int)g.tb && pt.x < wr.right - (int)(3 * kTbBtnW * tb_scale())) return HTCAPTION;   // the three buttons stay client area
      return HTCLIENT;
    }
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)l; mm->ptMinTrackSize.x = 640; mm->ptMinTrackSize.y = 400; return 0; }
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_CLOSE: g.result = UiResult::Quit; g.done = true; return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

// Writes the back buffer as a 32-bit BMP (test pictures).
bool save_bmp(const std::string& path) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  D3D11_TEXTURE2D_DESC d{};
  bb->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  bool ok = false;
  if (SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &st))) {
    g.ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp))) {
      BITMAPFILEHEADER bf{}; BITMAPINFOHEADER bi{};
      bi.biSize = sizeof bi; bi.biWidth = (LONG)d.Width; bi.biHeight = -(LONG)d.Height; bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
      bf.bfType = 0x4d42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + d.Width * d.Height * 4;
      std::vector<unsigned char> px((size_t)d.Width * d.Height * 4);
      for (UINT y = 0; y < d.Height; ++y) {
        const unsigned char* s = (const unsigned char*)mp.pData + (size_t)y * mp.RowPitch;
        unsigned char* o = px.data() + (size_t)y * d.Width * 4;
        for (UINT x = 0; x < d.Width; ++x) { o[x * 4 + 0] = s[x * 4 + 2]; o[x * 4 + 1] = s[x * 4 + 1]; o[x * 4 + 2] = s[x * 4 + 0]; o[x * 4 + 3] = 255; }
      }
      std::ofstream f(P(path), std::ios::binary);
      f.write((const char*)&bf, sizeof bf); f.write((const char*)&bi, sizeof bi); f.write((const char*)px.data(), (std::streamsize)px.size());
      ok = f.good();
      g.ctx->Unmap(st, 0);
    }
    st->Release();
  }
  bb->Release();
  return ok;
}

void apply_test_view(const std::string& v) {
  int a = 0, b = 0;
  if (v.rfind("settings", 0) == 0) { g.view = G::SETTINGS; std::sscanf(v.c_str(), "settings:%d", &a); g.tab = std::clamp(a, 0, (int)g.pages.size() - 1); }
  else if (v.rfind("pulldown", 0) == 0) {
    g.view = G::SETTINGS; std::sscanf(v.c_str(), "pulldown:%d:%d", &a, &b);
    g.tab = std::clamp(a, 0, (int)g.pages.size() - 1);
    g.row = std::clamp(b, 0, (int)g.pages[(size_t)g.tab].items.size() - 1);
    Item& it = g.pages[(size_t)g.tab].items[(size_t)g.row];
    g.pulldown = true; g.pd_sel = std::max(0, index_of(it, it.cur));
  } else if (v == "saves") { g.view = G::SAVES; g.row = 0; }
  else if (v.rfind("album", 0) == 0) album_test_view(v);
  else if (v.rfind("modcmp", 0) == 0) { load_mods(); std::sscanf(v.c_str(), "modcmp:%d", &a); g.row = a; g.mods_top = 0; modcmp_open(a); }
  else if (v.rfind("mods", 0) == 0) { load_mods(); g.view = G::MODS; std::sscanf(v.c_str(), "mods:%d", &a); g.row = a; g.mods_top = 0; }
  else if (v == "steam" || v == "steam_run") {   // steam_run really does the job (against SNAP_STEAM_DIR) and shows the result
    refresh_steam_status(); g.steam_running = g.steam_status > 0 && steam_is_running(); g.steam_state = G::ST_ASK; g.view = G::STEAM;
    if (v == "steam_run") steam_add();
  }
}

// The graphics card the window runs on, and whether a controller is plugged in (for the status chips).
void read_gpu_and_pad() {
  IDXGIDevice* dd = nullptr;
  if (g.dev && SUCCEEDED(g.dev->QueryInterface(IID_PPV_ARGS(&dd)))) {
    IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dd->GetAdapter(&ad))) {
      DXGI_ADAPTER_DESC d{};
      if (SUCCEEDED(ad->GetDesc(&d))) {
        std::string n = narrow(d.Description);
        for (const char* pre : {"NVIDIA ", "AMD ", "Intel(R) "}) if (n.rfind(pre, 0) == 0) n = n.substr(std::strlen(pre));
        g.gpu_name = n;
        g.gpu_nvidia = d.VendorId == 0x10de;
        g.gpu_rtx = g.gpu_nvidia && n.find("RTX") != std::string::npos;
      }
      ad->Release();
    }
    dd->Release();
  }
  using XGet = DWORD(WINAPI*)(DWORD, void*);
  if (HMODULE x = LoadLibraryW(L"xinput1_4.dll")) {
    if (XGet f = (XGet)GetProcAddress(x, "XInputGetState")) {
      unsigned char st[16];
      for (DWORD i = 0; i < 4 && !g.pad_found; ++i) if (f(i, st) == ERROR_SUCCESS) g.pad_found = true;
    }
  }
}

}  // namespace

UiResult show(UiInfo& info) {
  g = G();
  g.info = &info;
  // per-thread DPI awareness only while this window lives: the game window keeps the process default
  using SetCtx = HANDLE(WINAPI*)(HANDLE);
  SetCtx set_ctx = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
  HANDLE old_ctx = set_ctx ? set_ctx((HANDLE)(INT_PTR)-4) : nullptr;   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  auto restore = [&] { if (set_ctx && old_ctx) set_ctx(old_ctx); };

  build_pages();
  g.profile_idx = info.profiles.size();
  for (size_t i = 0; i < info.profiles.size(); ++i) if (info.profiles[i].id == info.profile_id) g.profile_idx = i;
  if (g.profile_idx >= info.profiles.size() && !info.profiles.empty()) { g.profile_idx = 0; info.profile_id = info.profiles[0].id; }
  load_stores();

  const char* shot = std::getenv("SNAP_LAUNCHER_UI_SHOT");
  const char* view = std::getenv("SNAP_LAUNCHER_UI_VIEW");

  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"SnapRecompLauncherUi";
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);
  using GetDpi = UINT(WINAPI*)(HWND);
  const DWORD style = WS_OVERLAPPEDWINDOW;
  g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Snap Recomp", style, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
  if (!g.hwnd) { UnregisterClassW(wc.lpszClassName, wc.hInstance); restore(); return UiResult::Failed; }
  apply_access_palette();
  fit_window();
  if (shot) {   // test hook (pictures only): SNAP_LAUNCHER_UI_SIZE=3840x2160 draws the picture at that client size, whatever the monitor is
    int sw = 0, sh = 0;
    if (const char* sz = std::getenv("SNAP_LAUNCHER_UI_SIZE")) if (std::sscanf(sz, "%dx%d", &sw, &sh) == 2 && sw >= 640 && sh >= 360)
      SetWindowPos(g.hwnd, nullptr, 0, 0, sw, sh, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  RECT cr{};
  GetClientRect(g.hwnd, &cr);
  g.rw = (UINT)(cr.right - cr.left); g.rh = (UINT)(cr.bottom - cr.top);
  if (!create_device()) { destroy_device(); DestroyWindow(g.hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance); restore(); return UiResult::Failed; }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  bool ok = ImGui_ImplWin32_Init(g.hwnd) && ImGui_ImplDX11_Init(g.dev, g.ctx);
  if (ok) {
    // font: the game's own (bytes decoded by the caller from the user's files), else Segoe UI Bold from Windows, else ImGui's built-in
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    if (!info.font_ttf.empty()) g.font = io.Fonts->AddFontFromMemoryTTF(info.font_ttf.data(), (int)info.font_ttf.size(), 22.0f, &fc);
    if (!g.font) {
      char sysdir[MAX_PATH] = {};
      GetWindowsDirectoryA(sysdir, MAX_PATH);
      for (const char* f : {"\\Fonts\\segoeuib.ttf", "\\Fonts\\segoeui.ttf"}) {
        const std::string p = std::string(sysdir) + f;
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) { g.font = io.Fonts->AddFontFromFileTTF(p.c_str(), 22.0f); if (g.font) break; }
      }
    }
    if (!g.font) g.font = io.Fonts->AddFontDefaultVector();
    // the Steam artwork is drawn with a Windows system font, never the game's own
    {
      char sysdir[MAX_PATH] = {};
      GetWindowsDirectoryA(sysdir, MAX_PATH);
      for (const char* f : {"\\Fonts\\seguibl.ttf", "\\Fonts\\segoeuib.ttf", "\\Fonts\\arialbd.ttf"}) {
        const std::string p = std::string(sysdir) + f;
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) { g.art_font = io.Fonts->AddFontFromFileTTF(p.c_str(), 22.0f); if (g.art_font) break; }
      }
      if (!g.art_font) g.art_font = g.font;
    }
    read_gpu_and_pad();
    refresh_steam_status();
    load_mods();
    refresh_cache();
  }
  if (!ok || !g.font) {
    if (ok) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); }
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(g.hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    restore();
    return UiResult::Failed;
  }

  if (view) apply_test_view(view);
  ShowWindow(g.hwnd, shot ? SW_HIDE : SW_SHOW);
  if (!shot) { SetForegroundWindow(g.hwnd); }

  int frames = 0;
  while (!g.done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) { g.done = true; g.result = UiResult::Quit; }
    }
    if (g.done) break;
    if (IsIconic(g.hwnd)) { Sleep(40); continue; }
    if (g.resize) {
      g.resize = false;
      release_target();
      g.sc->ResizeBuffers(0, g.rw, g.rh, DXGI_FORMAT_UNKNOWN, 0);
      create_target();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    g.s = std::min(ds.x / 1280.0f, (ds.y - g.tb) / 720.0f);
    g.ox = (ds.x - 1280 * g.s) / 2;
    g.oy = g.tb + (ds.y - g.tb - 720 * g.s) / 2;
    if (!shot) handle_input();
    if (g.done) { ImGui::EndFrame(); break; }
    if (g.view == G::HOME && ImGui::GetTime() - g.prep_at > 2.0) { g.prep_at = ImGui::GetTime(); refresh_cache(); }   // the shader chip follows a running preparation
    bool back = false;
    if (g.view == G::SETTINGS) draw_settings(&back);
    else if (g.view == G::SAVES) draw_saves();
    else if (g.view == G::MODS) draw_mods();
    else if (g.view == G::MODCMP) draw_modcmp();
    else if (g.view == G::ALBUM) draw_album();
    else if (g.view == G::STEAM) { g.block_hot = true; draw_home(); g.block_hot = false; draw_steam(); }
    else draw_home();
    draw_titlebar();
    static const char* const press_play = std::getenv("SNAP_LAUNCHER_UI_PLAY_AFTER_MS");   // test hook: act as if Play was pressed after this many milliseconds
    if (press_play && !g.done && ImGui::GetTime() * 1000.0 > std::atof(press_play)) go_play();
    ImGui::Render();
    const float clear[4] = {0.96f, 0.95f, 0.94f, 1.0f};
    g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
    g.ctx->ClearRenderTargetView(g.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ++frames;
    if (shot && frames >= 5 && (verify_state() != 0 || std::getenv("SNAP_LAUNCHER_UI_CHECKING"))) {   // a test picture waits for the folder check
      const bool saved = save_bmp(shot);
      std::fprintf(stderr, "launcher ui: picture %s %s\n", shot, saved ? "saved" : "FAILED");
      g.result = UiResult::Quit;
      break;
    }
    g.sc->Present(shot ? 0 : 1, 0);
    if (g.steam_work) { g.steam_work = false; steam_add(); }   // after the "Working" frame is on screen
  }
  info.profile_id = g.profile_idx < info.profiles.size() ? info.profiles[g.profile_idx].id : info.profile_id;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  destroy_device();
  DestroyWindow(g.hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  restore();
  return g.result;
}

}  // namespace snap_launcher
