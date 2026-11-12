// SPDX-License-Identifier: MIT
// "Saves" tab of the F1 overlay: save profiles (hle/profiles.h). Switching profile, clearing or deleting the active one takes
// effect on restart; nothing here touches the files of the profile the running game has mounted, except copying one consistent
// generation (Duplicate). Clear and Delete move data to profiles\_trash (newest 10 kept).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/hle.h"
#include "hle/profiles.h"
#include "hle/settings.h"
#include "ui_internal.h"

namespace snap::ui {

namespace {

namespace pr = hle::profiles;

struct SavesState {
  std::unique_ptr<pr::Store> store;
  std::string root;
  double loaded_at = -100;
  std::vector<int64_t> sizes;
  std::vector<pr::TrashEntry> trash;
  int sel = 0;
  char new_name[64] = "";
  char rename_buf[64] = "";
  char dup_buf[64] = "";
  char import_name[64] = "";
  std::string msg;
  bool msg_err = false;
  double msg_until = 0;
  enum Confirm { None, Clear, Delete } confirm = None;
  bool restart_needed = false;
  int last_sel = -1;
  std::vector<std::string> fixtures;
  double fixtures_at = -100;
  // folder dialog result (filled by a worker thread)
  std::mutex mu;
  std::string picked;
  std::atomic<bool> picking{false};
  std::string import_src;
};
SavesState Z;

double now_secs() { return ImGui::GetTime(); }

void say(const std::string& m, bool err) { Z.msg = m; Z.msg_err = err; Z.msg_until = now_secs() + 8; }

std::string find_root() {
  if (pr::active().on) return pr::active().root;
  if (const char* e = std::getenv("SNAP_PROFILES_DIR")) if (*e) return e;
  return "";
}

void reload() {
  if (!Z.store || Z.store->root() != Z.root) Z.store.reset(new pr::Store(Z.root));
  Z.store->load();
  Z.sizes.clear();
  for (auto& p : Z.store->list()) Z.sizes.push_back(Z.store->size_bytes(p.id));
  Z.trash = Z.store->trash_list();
  Z.loaded_at = now_secs();
  if (Z.sel >= (int)Z.store->list().size()) Z.sel = std::max(0, (int)Z.store->list().size() - 1);
}

std::vector<std::string> scan_fixtures() {
  std::vector<std::string> out;
  namespace fs = std::filesystem;
  std::vector<fs::path> bases;
  if (const char* e = std::getenv("SNAP_FIXTURES_DIR")) if (*e) bases.push_back(fs::u8path(e));
  std::error_code ec;
  bases.push_back(fs::current_path(ec) / "build" / "fixtures" / "saves");
  wchar_t exe[MAX_PATH];
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
    fs::path d = fs::path(exe).parent_path();
    for (int i = 0; i < 4 && d.has_parent_path(); ++i, d = d.parent_path()) bases.push_back(d / "fixtures" / "saves");
    bases.push_back(fs::path(exe).parent_path() / ".." / ".." / "build" / "fixtures" / "saves");
  }
  for (auto& b : bases) {
    if (!fs::is_directory(b, ec)) continue;
    for (fs::directory_iterator it(b, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_directory(e2)) continue;
      auto u = fs::weakly_canonical(it->path(), e2).u8string();
      std::string s(u.begin(), u.end());
      if (pr::looks_like_save(s) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    if (!out.empty()) break;
  }
  return out;
}

void open_folder(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  std::wstring w = std::filesystem::u8path(dir).wstring();
  ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void pick_folder_async() {
  if (Z.picking.exchange(true)) return;
  std::thread([] {
    std::string result;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      IFileOpenDialog* dlg = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        DWORD fl = 0;
        dlg->GetOptions(&fl);
        dlg->SetOptions(fl | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Choose a save folder to import");
        if (SUCCEEDED(dlg->Show(nullptr))) {
          IShellItem* it = nullptr;
          if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
              auto u = std::filesystem::path(p).u8string();
              result.assign(u.begin(), u.end());
              CoTaskMemFree(p);
            }
            it->Release();
          }
        }
        dlg->Release();
      }
      CoUninitialize();
    }
    { std::lock_guard<std::mutex> g(Z.mu); Z.picked = result; }
    Z.picking = false;
  }).detach();
}

// Working directory at startup: host\play.ps1 and run_boot.ps1 start the game from the project root and the command line holds relative paths
// (--trace analysisoot\play_trace.log), so the relaunch uses the same directory even if something changed it since.
const std::wstring g_start_cwd = [] { wchar_t b[MAX_PATH * 2]; DWORD n = GetCurrentDirectoryW(MAX_PATH * 2, b); return n && n < MAX_PATH * 2 ? std::wstring(b, n) : std::wstring(); }();

// Relaunch the game the way host\play.ps1 starts it: same exe and arguments, same working directory, same environment with the one-shot variables
// removed (SNAP_INPUT_SCRIPT: the -SkipIntro script must not replay; SNAP_PROFILE_PICKER: no picker again), SNAP_PROFILE set to the chosen profile, and
// the redirected log handles inherited. When play.ps1 launched this process (SNAP_PLAY_ROOT) the new pid is written to build\play.restart so play.ps1 keeps
// its play.lock and follows the new process. The new process waits for this one to exit.
void restart_now(const std::string& profile_name) {
  wchar_t exe[MAX_PATH * 2];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) { say("Cannot find the game executable.", true); return; }
  std::wstring cmd = GetCommandLineW();
  int n = MultiByteToWideChar(CP_UTF8, 0, profile_name.c_str(), -1, nullptr, 0);
  std::wstring wn((size_t)std::max(n, 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, profile_name.c_str(), -1, &wn[0], n);
  SetEnvironmentVariableW(L"SNAP_PROFILE", wn.c_str());
  SetEnvironmentVariableW(L"SNAP_RESTART_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
  SetEnvironmentVariableW(L"SNAP_PROFILE_PICKER", L"0");
  SetEnvironmentVariableW(L"SNAP_INPUT_SCRIPT", nullptr);
  hle::settings::forget_exported_env();   // SNAP_LOOK / SNAP_SSAO_QUALITY come from the profile settings; the new process reads the saved values
  if (const char* alt = std::getenv("SNAP_UI_TEST_RESTART")) if (*alt) SetEnvironmentVariableA("SNAP_UI_TEST", alt);   // the relaunched game runs the follow-up script
  STARTUPINFOW si{};
  si.cb = sizeof si;
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE), hout = GetStdHandle(STD_OUTPUT_HANDLE), herr = GetStdHandle(STD_ERROR_HANDLE);
  BOOL inherit = FALSE;
  if (hout && hout != INVALID_HANDLE_VALUE && herr && herr != INVALID_HANDLE_VALUE) {   // keep the log redirection of the parent
    SetHandleInformation(hout, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(herr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hin && hin != INVALID_HANDLE_VALUE) ? hin : nullptr;
    si.hStdOutput = hout;
    si.hStdError = herr;
    inherit = TRUE;
  }
  PROCESS_INFORMATION pi{};
  const wchar_t* cwd = g_start_cwd.empty() ? nullptr : g_start_cwd.c_str();
  // Break away from the launcher's job object when it allows that (hidden test runs close the job when the first process exits and would kill the new game);
  // without permission start it inside the job.
  BOOL started = CreateProcessW(exe, &cmd[0], nullptr, nullptr, inherit, CREATE_BREAKAWAY_FROM_JOB, nullptr, cwd, &si, &pi);
  if (!started) started = CreateProcessW(exe, &cmd[0], nullptr, nullptr, inherit, 0, nullptr, cwd, &si, &pi);
  if (!started) {
    say("Could not start the game again (error " + std::to_string(GetLastError()) + ").", true);
    return;
  }
  hle::log("ui: restart: relaunched as pid %lu with profile \"%s\"", (unsigned long)pi.dwProcessId, profile_name.c_str());
  if (const char* pr = std::getenv("SNAP_PLAY_ROOT")) if (*pr) {
    std::ofstream f(std::filesystem::u8path(pr) / "build" / "play.restart", std::ios::trunc);
    f << pi.dwProcessId << '\n';
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  TerminateProcess(GetCurrentProcess(), 0);   // the save layer commits atomically; unsaved progress is lost
}

// A button that must be held; returns true once when the hold completes. Works with mouse, keyboard (Enter) and the gamepad.
bool hold_button(const char* label, float secs, const ImVec4& color) {
  static ImGuiID held = 0;
  static double since = 0;
  ImGui::PushStyleColor(ImGuiCol_Button, color);
  ImGui::Button(label);
  ImGui::PopStyleColor();
  ImGuiID id = ImGui::GetItemID();
  if (ImGui::IsItemActive()) {
    if (held != id) { held = id; since = now_secs(); }
    float f = (float)((now_secs() - since) / secs);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::min(1.0f, f), b.y), IM_COL32(255, 255, 255, 70), ImGui::GetStyle().FrameRounding);
    if (f >= 1.0f) { held = 0; return true; }
  } else if (held == id) {
    held = 0;
  }
  return false;
}

// "Import test save" button body (also used by the scripted test): imports a fixture folder as a new profile named after it.
std::string import_fixture(pr::Store& st, const std::string& f, std::string* err) {
  std::string base = std::filesystem::u8path(f).filename().string(), name = base;
  for (int i = 2; st.find(name); ++i) name = base + " " + std::to_string(i);
  if (st.import_folder(f, name, err).empty()) return "";
  return name;
}

void copy_name(char* dst, size_t n, const std::string& s) { std::snprintf(dst, n, "%s", s.c_str()); }

}  // namespace

bool hold_confirm(const char* label, float secs, const ImVec4& color) { return hold_button(label, secs, color); }
void restart_game() { const pr::Active& a = pr::active(); restart_now(a.on ? a.name : std::string()); }

// Scripted-test entry points: the same code the buttons run, without a mouse click.
bool saves_test_import(const std::string& fixture, std::string* name_out, std::string* err) {
  Z.root = find_root();
  if (Z.root.empty()) { *err = "profiles are off"; return false; }
  Z.store.reset(new pr::Store(Z.root));
  Z.store->load();
  for (auto& f : scan_fixtures()) {
    if (std::filesystem::u8path(f).filename().string() != fixture) continue;
    std::string n = import_fixture(*Z.store, f, err);
    if (n.empty()) return false;
    *name_out = n;
    return true;
  }
  *err = "fixture not found: " + fixture;
  return false;
}
void saves_test_switch(const std::string& name) { restart_now(name); }

void saves_tab_opened() { Z.loaded_at = -100; Z.confirm = SavesState::None; }

void tab_saves() {
  const Theme& t = theme();
  const float lh = ImGui::GetTextLineHeightWithSpacing();
  Z.root = find_root();
  if (Z.root.empty()) {
    ImGui::TextWrapped("Save profiles are off in this run (the save folder is set by SNAP_SAVE_DIR or hle.json). Start the game without that setting to use profiles.");
    return;
  }
  if (now_secs() - Z.loaded_at > 3.0 || !Z.store) reload();
  if (now_secs() - Z.fixtures_at > 5.0) { Z.fixtures = scan_fixtures(); Z.fixtures_at = now_secs(); }
  {   // results of the folder dialog
    std::string p;
    { std::lock_guard<std::mutex> g(Z.mu); p.swap(Z.picked); }
    if (!p.empty()) { Z.import_src = p; copy_name(Z.import_name, sizeof Z.import_name, Z.store->next_free_name()); }
  }
  pr::Store& st = *Z.store;
  const pr::Active& act = pr::active();

  ImGui::TextWrapped("A profile is a separate save. The game is running profile \"%s\". Switching profile, and clearing or deleting the running one, take effect after a restart.",
                     act.on ? act.name.c_str() : "?");
  if (Z.restart_needed) {
    ImGui::TextColored(t.conflict, "A change is waiting for a restart.");
    ImGui::SameLine();
    if (ImGui::Button("Restart now")) restart_now(act.on ? act.name : "");
    ImGui::SameLine();
    ImGui::TextDisabled("(unsaved progress in the running game is lost)");
  }
  if (!act.note.empty()) ImGui::TextColored(t.text_dim, "%s", act.note.c_str());
  if (ImGui::GetTime() < Z.msg_until) ImGui::TextColored(Z.msg_err ? t.error : t.good, "%s", Z.msg.c_str());
  ImGui::Spacing();

  // ---- list
  const auto& list = st.list();
  if (ImGui::BeginTable("profiles", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY, ImVec2(0, lh * 6.4f))) {
    ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Last played", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Save size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)list.size(); ++i) {
      const pr::Profile& p = list[(size_t)i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      if (ImGui::Selectable(p.name.c_str(), Z.sel == i, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, lh * 0.9f))) { Z.sel = i; Z.confirm = SavesState::None; }
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(pr::format_time(p.last_played).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(pr::format_size(i < (int)Z.sizes.size() ? Z.sizes[(size_t)i] : 0).c_str());
      ImGui::TableNextColumn();
      if (act.on && p.id == act.id) {
        if (st.is_pending(p.id, "delete")) ImGui::TextColored(t.error, "Running - delete on restart");
        else if (st.is_pending(p.id, "clear")) ImGui::TextColored(t.conflict, "Running - clear on restart");
        else ImGui::TextColored(t.good, "Running now");
      } else {
        ImGui::TextDisabled("-");
      }
    }
    ImGui::EndTable();
  }
  if (Z.sel < 0 || Z.sel >= (int)list.size()) { ImGui::TextDisabled("No profiles yet."); }
  else {
    const pr::Profile p = list[(size_t)Z.sel];   // a copy: reload() replaces the list
    const bool is_active = act.on && p.id == act.id;
    if (Z.last_sel != Z.sel) {
      Z.last_sel = Z.sel;
      copy_name(Z.rename_buf, sizeof Z.rename_buf, p.name);
      copy_name(Z.dup_buf, sizeof Z.dup_buf, p.name + " copy");
    }
    ImGui::Spacing();
    ImGui::TextColored(t.title, "Selected: %s", p.name.c_str());
    if (!is_active) {
      if (ImGui::Button("Switch to this profile (restarts the game)")) restart_now(p.name);
      ImGui::SameLine();
    }
    if (ImGui::Button("Open folder")) open_folder(st.save_dir(p.id));
    // rename and duplicate
    ImGui::SetNextItemWidth(lh * 11);
    ImGui::InputText("##rename", Z.rename_buf, sizeof Z.rename_buf);
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
      std::string e;
      if (st.rename(p.id, Z.rename_buf, &e)) { say("Renamed.", false); reload(); } else say(e, true);
    }
    ImGui::SameLine(0, lh * 1.5f);
    ImGui::SetNextItemWidth(lh * 11);
    ImGui::InputText("##dup", Z.dup_buf, sizeof Z.dup_buf);
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
      std::string e;
      std::string id = st.duplicate(p.id, Z.dup_buf, &e);
      if (!id.empty()) { say("Copied the latest saved state into a new profile.", false); reload(); } else say(e, true);
    }
    // clear and delete, two steps: arm, then hold
    if (Z.confirm == SavesState::None) {
      if (ImGui::Button("Clear save...")) Z.confirm = SavesState::Clear;
      ImGui::SameLine();
      if (ImGui::Button("Delete profile...")) Z.confirm = SavesState::Delete;
    } else {
      const bool clr = Z.confirm == SavesState::Clear;
      ImGui::TextColored(t.conflict, "%s \"%s\"? The data is moved to the trash (newest 10 kept) and can be restored below.%s",
                         clr ? "Clear the save of" : "Delete the profile", p.name.c_str(),
                         is_active ? " It is running now, so this happens at the next start." : "");
      if (hold_button(clr ? "Hold to clear the save" : "Hold to delete the profile", 1.5f, t.error)) {
        std::string e;
        bool ok = true;
        if (is_active) { st.queue(clr ? "clear" : "delete", p.id); Z.restart_needed = true; say("Queued: it happens at the next start. Use Restart now to start fresh.", false); }
        else if (clr) { ok = st.clear(p.id, &e); if (ok) say("Save cleared; the old data is in the trash.", false); }
        else { ok = st.remove(p.id, &e); if (ok) say("Profile deleted; its data is in the trash.", false); }
        if (!ok) say(e, true);
        Z.confirm = SavesState::None;
        reload();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) Z.confirm = SavesState::None;
    }
  }

  // ---- create and import
  ImGui::Separator();
  ImGui::SetNextItemWidth(lh * 11);
  ImGui::InputTextWithHint("##new", "New profile name", Z.new_name, sizeof Z.new_name);
  ImGui::SameLine();
  if (ImGui::Button("Create profile")) {
    std::string e;
    std::string name = Z.new_name[0] ? Z.new_name : st.next_free_name();
    if (!st.create(name, &e).empty()) { say("Created \"" + name + "\".", false); Z.new_name[0] = 0; reload(); } else say(e, true);
  }
  ImGui::SameLine(0, lh * 1.5f);
  if (ImGui::Button(Z.picking ? "Choosing a folder..." : "Import save from folder...")) pick_folder_async();
  if (!Z.import_src.empty()) {
    ImGui::TextWrapped("Import %s as", Z.import_src.c_str());
    ImGui::SetNextItemWidth(lh * 11);
    ImGui::InputText("##impname", Z.import_name, sizeof Z.import_name);
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
      std::string e;
      if (!st.import_folder(Z.import_src, Z.import_name, &e).empty()) { say("Imported into a new profile.", false); Z.import_src.clear(); reload(); } else say(e, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel import")) Z.import_src.clear();
  }
  if (!Z.fixtures.empty() && ImGui::TreeNode("Import test save")) {
    ImGui::TextDisabled("Test saves found next to the build. Each one becomes a new profile.");
    for (auto& f : Z.fixtures) {
      std::string base = std::filesystem::u8path(f).filename().string();
      std::string label = "Import \"" + base + "\"";
      if (ImGui::Button(label.c_str())) {
        std::string e, name = import_fixture(st, f, &e);
        if (!name.empty()) { say("Imported \"" + name + "\" into a new profile.", false); reload(); } else say(e, true);
      }
    }
    ImGui::TreePop();
  }

  // ---- trash
  if (ImGui::TreeNode("Restore from trash")) {
    if (Z.trash.empty()) ImGui::TextDisabled("The trash is empty. Cleared and deleted saves wait here; the newest 10 are kept.");
    for (size_t i = 0; i < Z.trash.size(); ++i) {
      const auto& e = Z.trash[i];
      ImGui::PushID((int)i);
      ImGui::Text("%s   %s   %s   %s", e.name.c_str(), e.kind == "delete" ? "deleted" : "cleared", pr::format_time(e.when).c_str(), pr::format_size(e.size).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Restore")) {
        std::string err;
        if (!st.restore(e.folder, &err).empty()) { say("Restored into a new profile.", false); reload(); } else say(err, true);
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
}

}  // namespace snap::ui
