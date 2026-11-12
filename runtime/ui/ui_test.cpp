// SPDX-License-Identifier: MIT
// Scripted end-to-end test of the controls menu inside the real game window: SNAP_UI_TEST=<file>.
// One command per line, '#' starts a comment, times are seconds after the first present:
//   <t> key F1                      post a real F1 key press to the game window (goes through the window procedure)
//   <t> tab <Bindings|Mouse|Controller|Accessibility|Test_input>
//   <t> rebind <A|B|...|Down> <kb|mouse|pad> <slot>     what a click on that slot does (starts the capture)
//   <t> press <key:P|pad:east|mouse:left|...> <seconds> hold a source down through the input hub
//   <t> expect_button <A|ZL|...> <0|1>                  the bit the game reads from npad 0
//   <t> expect_preview <A|...> <0|1>                    the mapped state while the menu hides input from the game
//   <t> expect_open <0|1> / expect_rebinding <0|1>
//   <t> expect_bind <A|...> <kb|mouse|pad> <slot> <src|none>
//   <t> shot <file.bmp>             copy the window client area from the screen (needs a visible window)
//   <t> quit                        print the summary and exit the process
// Every check logs "ui-test: ... PASS|FAIL"; the summary line is "ui-test: SUMMARY n checks, m failed".
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hle/hle.h"
#include "hle/input_core.h"
#include "ui_internal.h"

namespace snap::ui {

using namespace hle::input;

namespace {

struct Cmd {
  double at;
  std::string line;
  bool done = false;
};
struct Release {
  double at;
  Source s;
};
std::vector<Cmd> g_cmds;
std::vector<Release> g_rel;
bool g_loaded = false, g_active = false;
double g_t0 = 0;
int g_checks = 0, g_failed = 0;

void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  hle::log("ui-test: %s", b);
  std::fprintf(stderr, "ui-test: %s\n", b);
  std::fflush(stderr);
}

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) ++g_failed;
  out("%s %s", what.c_str(), ok ? "PASS" : "FAIL");
}

Source parse_src(const std::string& s) {
  Source out_s;
  if (s != "none") parse_source(s, &out_s);
  return out_s;
}
bool parse_dev(const std::string& s, Device* d) {
  if (s == "kb") *d = kDevKeyboard;
  else if (s == "mouse") *d = kDevMouse;
  else if (s == "pad") *d = kDevController;
  else return false;
  return true;
}

void capture_window(std::string path) {
  HWND h = static_cast<HWND>(overlay_hwnd());
  if (!h) { out("shot: no window"); return; }
  std::thread([h, path]() {   // off the present thread: the swapchain must keep presenting while the screen is copied
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    Sleep(900);
    RECT cr;
    GetClientRect(h, &cr);
    POINT o = {0, 0};
    ClientToScreen(h, &o);
    int w = cr.right, ht = cr.bottom;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, ht);
    SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, ht, sdc, o.x, o.y, SRCCOPY);
    BITMAPINFOHEADER bi = {sizeof(bi), w, -ht, 1, 32, BI_RGB};
    std::vector<uint8_t> px((size_t)w * ht * 4);
    GetDIBits(mdc, bmp, 0, ht, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(sizeof(fh) + sizeof(bi) + px.size()), 0, 0, sizeof(fh) + sizeof(bi)};
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(&fh, sizeof(fh), 1, f);
      std::fwrite(&bi, sizeof(bi), 1, f);
      std::fwrite(px.data(), 1, px.size(), f);
      std::fclose(f);
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    out("shot: wrote %s (%dx%d)", path.c_str(), w, ht);
  }).detach();
}

void run(const std::string& line, double t) {
  std::istringstream is(line);
  std::string cmd;
  is >> cmd;
  if (cmd == "key") {
    std::string k;
    is >> k;
    HWND h = static_cast<HWND>(overlay_hwnd());
    if (k == "F1" && h) {
      PostMessageW(h, WM_KEYDOWN, VK_F1, 0x003b0001);
      PostMessageW(h, WM_KEYUP, VK_F1, (LPARAM)0xC03b0001);
      out("t=%.1f posted F1 to the game window", t);
    }
  } else if (cmd == "tab") {
    static std::string name;
    is >> name;
    for (auto& c : name) if (c == '_') c = ' ';
    menu_select_tab(name.c_str());
  } else if (cmd == "rebind") {
    std::string a, d;
    int slot = 0;
    is >> a >> d >> slot;
    Device dev;
    int act = button_from_name(a);
    check(act >= 0 && act <= kDown && parse_dev(d, &dev) && menu_begin_rebind(act, dev, slot), "rebind " + a + " " + d + " " + std::to_string(slot) + " started");
  } else if (cmd == "press") {
    std::string src;
    double secs = 0.3;
    is >> src >> secs;
    Source s = parse_src(src);
    check(s.kind != Source::None, "press " + src);
    hub().inject_source(s, true);
    g_rel.push_back({t + secs, s});
  } else if (cmd == "expect_button" || cmd == "expect_preview") {
    std::string n;
    int want = 1;
    is >> n >> want;
    int b = button_from_name(n);
    hub().poll(0);
    PadSample p = cmd == "expect_button" ? hub().pad() : hub().preview();
    bool down = b >= 0 && ((p.buttons >> b) & 1);
    check(b >= 0 && down == (want != 0), cmd + " " + n + " == " + std::to_string(want) + " (buttons=" + std::to_string((unsigned long long)p.buttons) + ")");
  } else if (cmd == "expect_open") {
    int want = 1;
    is >> want;
    check(overlay_open() == (want != 0), "menu open == " + std::to_string(want));
  } else if (cmd == "expect_rebinding") {
    int want = 1;
    is >> want;
    check(menu_rebinding() == (want != 0), "rebinding == " + std::to_string(want));
  } else if (cmd == "expect_bind") {
    std::string a, d, src;
    int slot = 0;
    is >> a >> d >> slot >> src;
    Device dev;
    int act = button_from_name(a);
    bool ok = act >= 0 && parse_dev(d, &dev);
    Source got = ok ? slot_get(action_sources(hub().mapping(), act), dev, slot) : Source{};
    check(ok && got == parse_src(src), "binding " + a + " " + d + " " + std::to_string(slot) + " == " + src + " (is '" + source_to_string(got) + "')");
  } else if (cmd == "save") {
    std::string err;
    check(hub().save(&err), "save " + err);
  } else if (cmd == "shot") {
    std::string p;
    is >> p;
    capture_window(p);
  } else if (cmd == "quit") {
    out("SUMMARY %d checks, %d failed", g_checks, g_failed);
    std::fflush(stderr);
    ExitProcess(g_failed ? 1 : 0);
  } else {
    out("unknown command '%s'", line.c_str());
  }
}

void load() {
  g_loaded = true;
  const char* path = std::getenv("SNAP_UI_TEST");
  if (!path || !*path) return;
  std::ifstream f(path);
  if (!f) { std::fprintf(stderr, "ui-test: cannot open %s\n", path); return; }
  std::string line;
  while (std::getline(f, line)) {
    size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    std::istringstream is(line);
    double at;
    if (!(is >> at)) continue;
    std::string rest;
    std::getline(is, rest);
    g_cmds.push_back({at, rest});
  }
  g_active = !g_cmds.empty();
  std::fprintf(stderr, "ui-test: %zu commands from %s\n", g_cmds.size(), path);
}

}  // namespace

bool test_active() { return g_active; }

void test_tick(double now) {
  if (!g_loaded) { load(); g_t0 = now; }
  if (!g_active) return;
  const double t = now - g_t0;
  for (size_t i = 0; i < g_rel.size();) {
    if (t >= g_rel[i].at) { hub().inject_source(g_rel[i].s, false); g_rel.erase(g_rel.begin() + (long)i); } else ++i;
  }
  for (auto& c : g_cmds) {
    if (c.done || t < c.at) continue;
    c.done = true;
    run(c.line, t);
    break;   // one command per frame: the menu and the input hub get a frame to react
  }
}

}  // namespace snap::ui
