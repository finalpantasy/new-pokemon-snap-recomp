// SPDX-License-Identifier: MIT
// F1 menu extras backed by the per-profile settings file (hle/settings.h): gyro aim and rumble (Controller tab, live) and the photo gallery
// (inside the Photos page; opens the local gallery server in the browser). Graphics options moved to ui_options.cpp.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "hle/input_core.h"
#include "hle/profiles.h"
#include "hle/settings.h"
#include "ui_internal.h"

#pragma comment(lib, "ws2_32.lib")

namespace snap::ui {

namespace {

namespace st = hle::settings;

std::atomic<int> g_gallery_state{0};   // 0 idle, 1 starting, 2 opened, 3 failed
std::atomic<unsigned> g_gallery_port{0};   // port the server we started reported (8777, or whatever the OS picked)

bool port_open(unsigned short port) {
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  bool ok = false;
  if (s != INVALID_SOCKET) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ok = connect(s, (sockaddr*)&a, sizeof a) == 0;
    closesocket(s);
  }
  WSACleanup();
  return ok;
}

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w((size_t)std::max(n, 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(w.size() - 1);
  return w;
}

// Project root: the folder holding tools-src\gallery (the game runs from the project root; the exe sits in build\play or build\snap).
std::filesystem::path find_project_root() {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> starts = {fs::current_path(ec)};
  wchar_t exe[MAX_PATH * 2];
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) starts.push_back(fs::path(exe).parent_path());
  if (const char* r = std::getenv("SNAP_PLAY_ROOT")) starts.insert(starts.begin(), fs::u8path(r));
  for (fs::path p : starts) {
    for (int i = 0; i < 5 && !p.empty(); ++i, p = p.parent_path())
      if (fs::exists(p / "tools-src" / "gallery" / "serve.py", ec)) return p;
  }
  return {};
}

std::string gallery_profile_id() {
  const hle::profiles::Active& a = hle::profiles::active();
  return a.on && !a.id.empty() ? a.id : "default";
}

// Starts tools-src\gallery\serve.py (extracts the photos, serves http://127.0.0.1:<port>/) unless the server this game started earlier still answers, then opens
// the browser at the port the server reports through --port-file (it tries 8777, then lets the OS pick; 8765 belongs to other software on some PCs).
void open_gallery_async() {
  if (g_gallery_state.exchange(1) == 1) return;
  std::string prof = gallery_profile_id();
  std::thread([prof] {
    namespace fs = std::filesystem;
    unsigned port = g_gallery_port.load();
    bool ok = port && port_open((unsigned short)port);
    if (!ok) {
      port = 0;
      fs::path root = find_project_root();
      if (root.empty()) { g_gallery_state = 3; return; }
      fs::path py = root / "tools" / "venv-recomp" / "Scripts" / "python.exe";
      std::error_code ec;
      std::wstring exe = fs::exists(py, ec) ? py.wstring() : L"python.exe";
      fs::path pf = fs::temp_directory_path(ec) / ("snap_gallery_port_" + std::to_string(GetCurrentProcessId()) + ".txt");
      fs::remove(pf, ec);
      std::wstring cmd = L"\"" + exe + L"\" -E -P \"" + (root / "tools-src" / "gallery" / "serve.py").wstring() + L"\" --no-open --profile \"" + widen(prof) +
                         L"\" --port 8777 --port-file \"" + pf.wstring() + L"\"";
      STARTUPINFOW si{};
      si.cb = sizeof si;
      PROCESS_INFORMATION pi{};
      if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, root.wstring().c_str(), &si, &pi)) {
        g_gallery_state = 3;
        return;
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);   // the server outlives the game; it only listens on 127.0.0.1
      for (int i = 0; i < 240 && !ok; ++i) {   // extraction runs first, then the port file appears
        Sleep(500);
        std::ifstream f(pf);
        unsigned p = 0;
        if (f >> p && p > 0 && p < 65536 && port_open((unsigned short)p)) { port = p; ok = true; }
      }
      fs::remove(pf, ec);
    }
    if (ok) {
      g_gallery_port = port;
      std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
      ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      g_gallery_state = 2;
    } else {
      g_gallery_state = 3;
    }
  }).detach();
}

}  // namespace

void extras_controller() {
  const Theme& t = theme();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(t.title, "Gyro aim");
  ImGui::TextWrapped("Tilting a controller with a gyroscope (DualSense, Switch Pro, Joy-Con) moves the camera, added to the right stick. Off by default.");
  bool on = st::get_bool("gyro_on", false);
  if (ImGui::Checkbox("Gyro aim", &on)) st::set_bool("gyro_on", on);
  ImGui::BeginDisabled(!on);
  float sens = st::get_float("gyro_sens", 1.0f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  if (ImGui::SliderFloat("Gyro sensitivity", &sens, 0.1f, 4.0f, "%.2f")) st::set_float("gyro_sens", sens);
  bool ix = st::get_bool("gyro_invert_x", false), iy = st::get_bool("gyro_invert_y", false);
  if (ImGui::Checkbox("Invert gyro X", &ix)) st::set_bool("gyro_invert_x", ix);
  ImGui::SameLine();
  if (ImGui::Checkbox("Invert gyro Y", &iy)) st::set_bool("gyro_invert_y", iy);
  ImGui::EndDisabled();
  hle::input::RawInput r = hle::input::hub().raw();
  if (r.gyro_present) ImGui::TextColored(t.text_dim, "Gyro: %.0f, %.0f deg/s", r.gyro_x, r.gyro_y);
  else ImGui::TextColored(t.text_dim, "Gyro: none reported (turn gyro aim on with a controller that has one)");
  ImGui::TextColored(t.text_dim, "Applies at once.");

  ImGui::Spacing();
  ImGui::TextColored(t.title, "Rumble");
  bool ron = st::get_bool("rumble_on", true);
  if (ImGui::Checkbox("Controller rumble", &ron)) st::set_bool("rumble_on", ron);
  ImGui::BeginDisabled(!ron);
  float rs = st::get_float("rumble_strength", 1.0f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
  if (ImGui::SliderFloat("Rumble strength", &rs, 0.0f, 1.5f, "%.2f")) st::set_float("rumble_strength", rs);
  ImGui::EndDisabled();
  ImGui::TextColored(t.text_dim, "Rumble reaches DualSense, Xbox and Switch Pro pads. Applies at once.");
}

void gallery_section() {
  const Theme& t = theme();
  ImGui::TextColored(t.title, "Photo gallery");
  ImGui::TextWrapped("Opens your saved photos for the running profile (\"%s\") in your web browser, from a small server on this PC (127.0.0.1). Nothing leaves the machine.",
                     hle::profiles::active().on ? hle::profiles::active().name.c_str() : "default");
  ImGui::Spacing();
  const int s = g_gallery_state.load();
  ImGui::BeginDisabled(s == 1);
  if (ImGui::Button("Open gallery in browser")) open_gallery_async();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (s == 1) ImGui::TextColored(t.text_dim, "Reading photos from the save...");
  else if (s == 2) ImGui::TextColored(t.good, "Opened. Use it again after taking new photos: it re-reads the save.");
  else if (s == 3) ImGui::TextColored(t.error, "Could not start the gallery server (needs tools-src\\gallery and Python).");
}

}  // namespace snap::ui
