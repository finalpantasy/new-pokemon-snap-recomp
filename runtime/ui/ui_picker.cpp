// SPDX-License-Identifier: MIT
// Startup save-profile picker: a small native window shown before the save mounts (the game window and Vulkan do not exist
// for this purpose yet, so it uses plain GDI). Keyboard (Up/Down, Enter, Esc), mouse (hover, click, wheel) and controller
// (D-pad or left stick, A to choose, B to use the last played profile) all work.
// Test hooks: SNAP_PICKER_SHOT=<file.bmp> saves a picture of the window after it has drawn and then closes it;
// SNAP_PICKER_AUTO=<index> is the choice returned then (default -1: the last played profile).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hle/profiles.h"
#include "ui_font.h"

namespace {

using hle::profiles::PickItem;

struct State {
  const PickItem* items = nullptr;
  int n = 0;           // profiles; row n is "New profile"
  int sel = 0;
  int top = 0;         // first visible row
  int result = -2;     // -2 still open
  float scale = 1.0f;
  HFONT f_title = nullptr, f_name = nullptr, f_small = nullptr;
  int shot_ticks = -1;
  std::wstring shot_path;
  int auto_choice = -1;
  WORD prev_buttons = 0;
  int pad_repeat = 0;
};
State* g = nullptr;

// game palette (measured from the game's menus, see analysis/ui/style_ref.md): cream page, white cards, yellow selection with an orange outline
constexpr COLORREF kBg = RGB(244, 242, 240), kPanel = RGB(252, 252, 252), kSel = RGB(248, 213, 71), kSelLine = RGB(248, 171, 121),
                   kText = RGB(90, 83, 80), kDim = RGB(139, 138, 139), kBorder = RGB(224, 220, 215), kTitle = RGB(218, 116, 58);

int px(int v) { return (int)(v * g->scale + 0.5f); }
constexpr int kVisible = 6;

std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

RECT row_rect(int i, const RECT& cl) {
  int vis = i - g->top;
  int y = px(96) + vis * px(64);
  RECT r{px(28), y, cl.right - px(28), y + px(56)};
  return r;
}
int total_rows() { return g->n + 1; }

void keep_visible() {
  if (g->sel < g->top) g->top = g->sel;
  if (g->sel >= g->top + kVisible) g->top = g->sel - kVisible + 1;
  g->top = std::max(0, std::min(g->top, std::max(0, total_rows() - kVisible)));
}

void paint(HWND h, HDC dc) {
  RECT cl;
  GetClientRect(h, &cl);
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
  HGDIOBJ old = SelectObject(mem, bmp);
  HBRUSH bg = CreateSolidBrush(kBg);
  FillRect(mem, &cl, bg);
  DeleteObject(bg);
  SetBkMode(mem, TRANSPARENT);
  SelectObject(mem, g->f_title);
  SetTextColor(mem, kTitle);
  TextOutW(mem, px(28), px(22), L"Choose a save profile", 21);
  SelectObject(mem, g->f_small);
  SetTextColor(mem, kDim);
  const wchar_t* sub = L"Each profile keeps its own save. You can rename, copy, clear or delete them later in the F1 menu (Saves tab).";
  TextOutW(mem, px(28), px(60), sub, (int)wcslen(sub));
  for (int i = g->top; i < total_rows() && i < g->top + kVisible; ++i) {
    RECT r = row_rect(i, cl);
    bool sel = i == g->sel;
    HBRUSH b = CreateSolidBrush(sel ? kSel : kPanel);
    HPEN pen = CreatePen(PS_SOLID, sel ? px(3) : 1, sel ? kSelLine : kBorder);
    HGDIOBJ ob = SelectObject(mem, b), op = SelectObject(mem, pen);
    RoundRect(mem, r.left, r.top, r.right, r.bottom, px(26), px(26));
    SelectObject(mem, ob); SelectObject(mem, op);
    DeleteObject(b); DeleteObject(pen);
    SetTextColor(mem, kText);
    SelectObject(mem, g->f_name);
    if (i < g->n) {
      std::wstring nm = widen(g->items[i].name);
      TextOutW(mem, r.left + px(18), r.top + px(8), nm.c_str(), (int)nm.size());
      SelectObject(mem, g->f_small);
      SetTextColor(mem, sel ? RGB(120, 100, 80) : kDim);
      std::wstring info = L"Last played: " + widen(hle::profiles::format_time(g->items[i].last_played)) + L"     Save size: " +
                          widen(hle::profiles::format_size(g->items[i].size));
      TextOutW(mem, r.left + px(18), r.top + px(31), info.c_str(), (int)info.size());
    } else {
      const wchar_t* t = L"+  New profile";
      TextOutW(mem, r.left + px(18), r.top + px(14), t, (int)wcslen(t));
    }
  }
  if (total_rows() > kVisible) {   // scroll hint
    SelectObject(mem, g->f_small);
    SetTextColor(mem, kDim);
    wchar_t t[64];
    std::swprintf(t, 64, L"%d-%d of %d", g->top + 1, std::min(total_rows(), g->top + kVisible), total_rows());
    TextOutW(mem, cl.right - px(130), px(60), t, (int)wcslen(t));
  }
  SelectObject(mem, g->f_small);
  SetTextColor(mem, kDim);
  {   // controller prompts: the game's orange chevron with a white disc
    auto prompt = [&](int x, const wchar_t* letter, const wchar_t* label) {
      const int y = cl.bottom - px(38), hh = px(24), ww = px(34), nose = px(10);
      POINT pts[5] = {{x, y}, {x + ww - nose, y}, {x + ww, y + hh / 2}, {x + ww - nose, y + hh}, {x, y + hh}};
      HBRUSH ob = CreateSolidBrush(kSelLine);
      HPEN op = CreatePen(PS_SOLID, 1, RGB(150, 100, 70));
      HGDIOBJ o1 = SelectObject(mem, ob), o2 = SelectObject(mem, op);
      Polygon(mem, pts, 5);
      HBRUSH wb = CreateSolidBrush(RGB(255, 250, 245));
      SelectObject(mem, wb);
      SelectObject(mem, GetStockObject(NULL_PEN));
      const int cx = x + (ww - nose) / 2 + px(3), cy = y + hh / 2, rr = px(8);
      Ellipse(mem, cx - rr, cy - rr, cx + rr, cy + rr);
      SelectObject(mem, o1); SelectObject(mem, o2);
      DeleteObject(ob); DeleteObject(op); DeleteObject(wb);
      SelectObject(mem, g->f_small);
      SetTextColor(mem, kTitle);
      SIZE ts; GetTextExtentPoint32W(mem, letter, 1, &ts);
      TextOutW(mem, cx - ts.cx / 2, cy - ts.cy / 2, letter, 1);
      SetTextColor(mem, kDim);
      TextOutW(mem, x + ww + px(8), cy - ts.cy / 2, label, (int)wcslen(label));
      GetTextExtentPoint32W(mem, label, (int)wcslen(label), &ts);
      return x + ww + px(8) + ts.cx + px(26);
    };
    int x = px(28);
    x = prompt(x, L"A", L"Choose");
    prompt(x, L"B", L"Last played profile");
    SetTextColor(mem, kDim);
    const wchar_t* hint = L"Up/Down, Enter or click also work";
    SIZE hs; GetTextExtentPoint32W(mem, hint, (int)wcslen(hint), &hs);
    TextOutW(mem, cl.right - px(28) - hs.cx, cl.bottom - px(38) + px(4), hint, (int)wcslen(hint));
  }
  BitBlt(dc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
}

void save_bmp(HWND h, const std::wstring& path) {
  RECT cl;
  GetClientRect(h, &cl);
  HDC dc = GetDC(h);
  HDC mem = CreateCompatibleDC(dc);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = cl.right;
  bi.bmiHeader.biHeight = cl.bottom;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, cl.right, cl.bottom, dc, 0, 0, SRCCOPY);
  int stride = ((cl.right * 3 + 3) / 4) * 4;
  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4d42;
  fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
  fh.bfSize = fh.bfOffBits + (DWORD)stride * cl.bottom;
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (f) {
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
    fwrite(bits, 1, (size_t)stride * cl.bottom, f);
    fclose(f);
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(h, dc);
}

void move_sel(int d) {
  g->sel = std::max(0, std::min(total_rows() - 1, g->sel + d));
  keep_visible();
}

int hit(HWND h, int x, int y) {
  RECT cl;
  GetClientRect(h, &cl);
  for (int i = g->top; i < total_rows() && i < g->top + kVisible; ++i) {
    RECT r = row_rect(i, cl);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
  }
  return -1;
}

typedef DWORD(WINAPI* XGetState)(DWORD, void*);
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };

void poll_pad(HWND h) {
  static XGetState fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryW(dll)) { fn = (XGetState)GetProcAddress(m, "XInputGetState"); if (fn) break; }
    }
  }
  if (!fn) return;
  WORD b = 0;
  int ly = 0;
  for (DWORD i = 0; i < 4; ++i) {
    XState s{};
    if (fn(i, &s) == ERROR_SUCCESS) { b |= s.buttons; if (std::abs(s.ly) > std::abs(ly)) ly = s.ly; }
  }
  if (ly > 16000) b |= 0x0001;      // up
  if (ly < -16000) b |= 0x0002;     // down
  WORD pressed = b & (WORD)~g->prev_buttons;
  bool held_dir = (b & 0x0003) && (b == g->prev_buttons);
  if (held_dir) { if (++g->pad_repeat > 12 && g->pad_repeat % 4 == 0) pressed |= (b & 0x0003); }
  else g->pad_repeat = 0;
  g->prev_buttons = b;
  if (pressed & 0x0001) move_sel(-1);
  if (pressed & 0x0002) move_sel(1);
  if (pressed & 0x1000) g->result = g->sel;       // A
  if (pressed & 0x2000) g->result = -1;           // B
  if (pressed) InvalidateRect(h, nullptr, FALSE);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (!g) return DefWindowProcW(h, m, w, l);
  switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(h, dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN:
      if (w == VK_UP) move_sel(-1);
      else if (w == VK_DOWN) move_sel(1);
      else if (w == VK_HOME) { g->sel = 0; keep_visible(); }
      else if (w == VK_END) { g->sel = total_rows() - 1; keep_visible(); }
      else if (w == VK_RETURN || w == VK_SPACE) g->result = g->sel;
      else if (w == VK_ESCAPE) g->result = -1;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_MOUSEMOVE: {
      int i = hit(h, (short)LOWORD(l), (short)HIWORD(l));
      if (i >= 0 && i != g->sel) { g->sel = i; InvalidateRect(h, nullptr, FALSE); }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      int i = hit(h, (short)LOWORD(l), (short)HIWORD(l));
      if (i >= 0) g->result = i;
      return 0;
    }
    case WM_MOUSEWHEEL:
      move_sel((short)HIWORD(w) > 0 ? -1 : 1);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_TIMER:
      poll_pad(h);
      if (g->shot_ticks >= 0 && ++g->shot_ticks == 25) {   // about 0.4 s: the window has drawn
        save_bmp(h, g->shot_path);
        g->result = g->auto_choice;
      }
      return 0;
    case WM_CLOSE: g->result = -1; return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

}  // namespace

extern "C" int snap_ui_pick_profile(const PickItem* items, int count) {
  State st;
  st.items = items;
  st.n = count;
  g = &st;
  for (int i = 1; i < count; ++i) if (items[i].last_played > items[st.sel].last_played) st.sel = i;   // start on the last played
  HDC sdc = GetDC(nullptr);
  st.scale = std::max(1.0f, GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f);
  ReleaseDC(nullptr, sdc);
  // the game's own UI font, decoded from the extracted files and registered for this process only (never written to disk)
  static std::vector<unsigned char> game_font;
  const wchar_t* face = L"Segoe UI";
  if (snap::ui::game_font_ttf(game_font)) {
    DWORD nfonts = 0;
    if (AddFontMemResourceEx(game_font.data(), (DWORD)game_font.size(), nullptr, &nfonts) && nfonts > 0) face = L"AsiaKNGD-E";
  }
  auto mkfont = [&](int h, int weight) {
    return CreateFontW(-px(h), 0, 0, 0, face[0] == L'S' ? weight : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
  };
  st.f_title = mkfont(26, FW_SEMIBOLD);
  st.f_name = mkfont(18, FW_SEMIBOLD);
  st.f_small = mkfont(14, FW_NORMAL);
  if (const char* e = std::getenv("SNAP_PICKER_SHOT")) { if (*e) { st.shot_ticks = 0; st.shot_path = widen(e); } }
  if (const char* e = std::getenv("SNAP_PICKER_AUTO")) if (*e) st.auto_choice = std::atoi(e);

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"SnapProfilePicker";
  RegisterClassW(&wc);
  int w = px(720), hgt = px(96 + kVisible * 64 + 64);
  RECT r{0, 0, w, hgt};
  AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
  int ww = r.right - r.left, wh = r.bottom - r.top;
  int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
  HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, wc.lpszClassName, L"New Pokemon Snap - choose a save profile", WS_CAPTION | WS_SYSMENU,
                           (sx - ww) / 2, (sy - wh) / 2, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
  int ret = -1;
  if (h) {
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetTimer(h, 1, 16, nullptr);
    MSG msg;
    while (st.result == -2) {
      if (!GetMessageW(&msg, nullptr, 0, 0)) break;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    ret = st.result == -2 ? -1 : st.result;
    KillTimer(h, 1);
    DestroyWindow(h);
  }
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  DeleteObject(st.f_title); DeleteObject(st.f_name); DeleteObject(st.f_small);
  g = nullptr;
  return ret;
}
