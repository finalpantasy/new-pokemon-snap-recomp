// SPDX-License-Identifier: MIT
// Window options: borderless fullscreen (F11, Alt+Enter), monitor choice, remembered size and position, pause and mute while the
// window is in the background, frame cap. See window_opts.h and host/README.md ("Window options").
#include "window_opts.h"

#include <windows.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hle/json_lite.h"
#include "audio/audio_out.h"
#include "nvn/nvn_host.h"

extern "C" void os_time_pause(int on);   // runtime/os/os_kernel.cpp: freeze guest ticks
namespace hle { void oe_post_message(uint32_t msg); }   // runtime/hle/services_sys.cpp: applet notification queue

namespace {

constexpr UINT WM_SNAP_APPLY = WM_APP + 0x51;
constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kBorderlessStyle = WS_POPUP;

struct Settings {
    int mode = SNAP_WINDOW_WINDOWED;
    int monitor = -1;                 // -1: the monitor the window is on (primary at first start)
    int width = 1280, height = 720;   // windowed client size
    int x = INT_MIN, y = INT_MIN;     // windowed outer position (INT_MIN: default)
    bool maximized = false;
    bool pause_on_focus_loss = true;
    bool mute_in_background = true;
    bool game_notify = false;         // also post the nn::oe FocusStateChanged / Resume messages to the game
    int frame_cap = 0;                // fps, 0 = none
};

Settings g_s;
std::mutex g_mu;                      // guards g_s, g_norm, g_have_norm
HWND g_hwnd;
RECT g_norm = {0, 0, 0, 0};           // windowed outer rectangle (restore target)
bool g_have_norm = false;
std::atomic<int> g_cap{0};
std::atomic<bool> g_focused{true}, g_paused{false};
HANDLE g_run_ev;                      // signaled while not paused
bool g_muted = false;
float g_saved_master = 1.0f;
bool g_save_enabled = true;

std::string cfg_dir() {
    const char* d = std::getenv("SNAP_CONFIG_DIR");
    if (d && *d) return d;
    const char* a = std::getenv("APPDATA");
    return std::string(a ? a : ".") + "\\SnapRecomp";
}
std::string cfg_path() { return cfg_dir() + "\\window.json"; }
std::string muted_marker() { return cfg_dir() + "\\window.muted"; }

bool env_int(const char* n, int* out) { const char* e = std::getenv(n); if (!e || !*e) return false; *out = std::atoi(e); return true; }

void load_settings() {
    std::lock_guard<std::mutex> lk(g_mu);
    FILE* f = std::fopen(cfg_path().c_str(), "rb");
    if (f) {
        std::string text; char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        hle::Json j; std::string err;
        if (hle::json_parse(text, j, err) && j.type == hle::Json::Obj) {
            if (auto* v = j.get("mode")) g_s.mode = v->str() == "borderless" ? SNAP_WINDOW_BORDERLESS : SNAP_WINDOW_WINDOWED;
            if (auto* v = j.get("monitor")) g_s.monitor = (int)v->num(-1);
            if (auto* v = j.get("width")) g_s.width = (int)v->num(g_s.width);
            if (auto* v = j.get("height")) g_s.height = (int)v->num(g_s.height);
            if (auto* v = j.get("x")) g_s.x = (int)v->num(0);
            if (auto* v = j.get("y")) g_s.y = (int)v->num(0);
            if (auto* v = j.get("maximized")) g_s.maximized = v->boolean(false);
            if (auto* v = j.get("pause_on_focus_loss")) g_s.pause_on_focus_loss = v->boolean(true);
            if (auto* v = j.get("mute_in_background")) g_s.mute_in_background = v->boolean(true);
            if (auto* v = j.get("game_notify")) g_s.game_notify = v->boolean(false);
            if (auto* v = j.get("frame_cap")) g_s.frame_cap = (int)v->num(0);
        } else std::fprintf(stderr, "[window] %s is invalid (%s): defaults used\n", cfg_path().c_str(), err.c_str());
    }
    int v;
    if (const char* e = std::getenv("SNAP_WINDOW_MODE")) g_s.mode = (std::strcmp(e, "borderless") == 0 || std::strcmp(e, "1") == 0) ? SNAP_WINDOW_BORDERLESS : SNAP_WINDOW_WINDOWED;
    if (env_int("SNAP_WINDOW_MONITOR", &v)) g_s.monitor = v;
    if (env_int("SNAP_WINDOW_W", &v)) g_s.width = v;
    if (env_int("SNAP_WINDOW_H", &v)) g_s.height = v;
    if (env_int("SNAP_WINDOW_X", &v)) g_s.x = v;
    if (env_int("SNAP_WINDOW_Y", &v)) g_s.y = v;
    if (env_int("SNAP_WINDOW_PAUSE", &v)) g_s.pause_on_focus_loss = v != 0;
    if (env_int("SNAP_WINDOW_MUTE_BG", &v)) g_s.mute_in_background = v != 0;
    if (env_int("SNAP_WINDOW_NOTIFY", &v)) g_s.game_notify = v != 0;
    if (env_int("SNAP_WINDOW_FRAME_CAP", &v)) g_s.frame_cap = v;
    if (g_s.width < 320) g_s.width = 320;
    if (g_s.height < 180) g_s.height = 180;
    if (g_s.frame_cap < 0) g_s.frame_cap = 0;
    g_cap = g_s.frame_cap;
    // test runs (a save dir override) must not rewrite the owner's remembered placement unless asked
    const char* sd = std::getenv("SNAP_SAVE_DIR");
    g_save_enabled = !(sd && *sd) || std::getenv("SNAP_WINDOW_SAVE");
}

void save_settings() {
    if (!g_save_enabled) return;
    hle::Json j = hle::Json::object();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        j.set("mode", hle::Json::string(g_s.mode == SNAP_WINDOW_BORDERLESS ? "borderless" : "windowed"));
        j.set("monitor", hle::Json::number(g_s.monitor));
        j.set("width", hle::Json::number(g_s.width));
        j.set("height", hle::Json::number(g_s.height));
        if (g_s.x != INT_MIN) { j.set("x", hle::Json::number(g_s.x)); j.set("y", hle::Json::number(g_s.y)); }
        j.set("maximized", hle::Json::boolean_(g_s.maximized));
        j.set("pause_on_focus_loss", hle::Json::boolean_(g_s.pause_on_focus_loss));
        j.set("mute_in_background", hle::Json::boolean_(g_s.mute_in_background));
        j.set("game_notify", hle::Json::boolean_(g_s.game_notify));
        j.set("frame_cap", hle::Json::number(g_s.frame_cap));
    }
    std::string out; hle::json_dump(j, out); out += "\n";
    CreateDirectoryA(cfg_dir().c_str(), nullptr);
    std::string tmp = cfg_path() + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
        MoveFileExA(tmp.c_str(), cfg_path().c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

// ---- monitors
struct Mon { HMONITOR h; RECT rc; std::string name; };
BOOL CALLBACK enum_mon(HMONITOR hm, HDC, LPRECT, LPARAM p) {
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(hm, &mi)) return TRUE;
    char b[160]; std::snprintf(b, sizeof b, "%s %ldx%ld%s", mi.szDevice, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                               (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
    ((std::vector<Mon>*)p)->push_back({hm, mi.rcMonitor, b});
    return TRUE;
}
std::vector<Mon> monitors() { std::vector<Mon> v; EnumDisplayMonitors(nullptr, nullptr, enum_mon, (LPARAM)&v); return v; }

// The monitor a window goes to: the chosen index, else the one holding `near_rc` (or the primary one).
HMONITOR pick_monitor(int idx, const RECT* near_rc) {
    auto v = monitors();
    if (idx >= 0 && idx < (int)v.size()) return v[idx].h;
    if (near_rc) return MonitorFromRect(near_rc, MONITOR_DEFAULTTONEAREST);
    return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}
RECT mon_rect(HMONITOR m) { MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(m, &mi); return mi.rcMonitor; }
RECT work_rect(HMONITOR m) { MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(m, &mi); return mi.rcWork; }
RECT outer_for_client(int w, int h) { RECT r = {0, 0, w, h}; AdjustWindowRect(&r, kWindowedStyle, FALSE); return r; }
RECT centered(const RECT& wr, int w, int h) {
    int x = wr.left + ((wr.right - wr.left) - w) / 2, y = wr.top + ((wr.bottom - wr.top) - h) / 2;
    return {x, y, x + w, y + h};
}

// ---- audio mute (master volume; a marker file restores it after a crash while muted)
void mute_on() {
    if (g_muted) return;
    g_saved_master = snap_audio_get_volume(SNAP_VOL_MASTER);
    CreateDirectoryA(cfg_dir().c_str(), nullptr);
    if (FILE* f = std::fopen(muted_marker().c_str(), "wb")) { std::fprintf(f, "%.4f\n", g_saved_master); std::fclose(f); }
    snap_audio_set_volume(SNAP_VOL_MASTER, 0.0f);
    g_muted = true;
}
void mute_off() {
    if (!g_muted) return;
    snap_audio_set_volume(SNAP_VOL_MASTER, g_saved_master);
    DeleteFileA(muted_marker().c_str());
    g_muted = false;
}
void restore_after_crash() {
    FILE* f = std::fopen(muted_marker().c_str(), "rb");
    if (!f) return;
    float v = 1.0f; if (std::fscanf(f, "%f", &v) != 1) v = 1.0f;
    std::fclose(f);
    DeleteFileA(muted_marker().c_str());
    if (snap_audio_get_volume(SNAP_VOL_MASTER) < 0.001f && v > 0.0f) snap_audio_set_volume(SNAP_VOL_MASTER, v);
}

// ---- focus
void set_focus(bool focused, const char* why) {
    if (g_focused.exchange(focused) == focused) return;
    Settings s; { std::lock_guard<std::mutex> lk(g_mu); s = g_s; }
    std::fprintf(stderr, "[window] focus %s (%s): pause=%d mute=%d notify=%d\n", focused ? "gained" : "lost", why, s.pause_on_focus_loss, s.mute_in_background, s.game_notify);
    if (!focused) {
        if (s.pause_on_focus_loss) { ResetEvent(g_run_ev); g_paused = true; os_time_pause(1); }
        if (s.mute_in_background) mute_on();
        if (s.game_notify) hle::oe_post_message(15);   // nn::oe::MessageFocusStateChanged
    } else {
        if (g_paused.exchange(false)) { os_time_pause(0); SetEvent(g_run_ev); }
        mute_off();
        if (s.game_notify) { hle::oe_post_message(15); hle::oe_post_message(16); }   // FocusStateChanged, Resume
    }
}

// ---- mode / monitor
void remember_normal(HWND h) {
    WINDOWPLACEMENT wp = {sizeof wp};
    if (!GetWindowPlacement(h, &wp)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_s.mode != SNAP_WINDOW_WINDOWED) return;   // while borderless the placement is the fullscreen rectangle
    g_norm = wp.rcNormalPosition; g_have_norm = true;
    g_s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    g_s.x = g_norm.left; g_s.y = g_norm.top;
    RECT fr = outer_for_client(0, 0);
    g_s.width = (g_norm.right - g_norm.left) - (fr.right - fr.left);
    g_s.height = (g_norm.bottom - g_norm.top) - (fr.bottom - fr.top);
    if (g_s.width < 320) g_s.width = 320;
    if (g_s.height < 180) g_s.height = 180;
}

void apply_mode(HWND h) {
    Settings s; RECT n; bool have;
    { std::lock_guard<std::mutex> lk(g_mu); s = g_s; n = g_norm; have = g_have_norm; }
    if (s.mode == SNAP_WINDOW_BORDERLESS) {
        RECT cur; GetWindowRect(h, &cur);
        RECT r = mon_rect(pick_monitor(s.monitor, &cur));
        if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
        SetWindowLongPtrW(h, GWL_STYLE, kBorderlessStyle | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, kWindowedStyle | WS_VISIBLE);
        if (!have || n.right <= n.left) {
            RECT cur; GetWindowRect(h, &cur);
            RECT o = outer_for_client(s.width, s.height);
            n = centered(work_rect(pick_monitor(s.monitor, &cur)), o.right - o.left, o.bottom - o.top);
        }
        SetWindowPos(h, HWND_NOTOPMOST, n.left, n.top, n.right - n.left, n.bottom - n.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (s.maximized) ShowWindow(h, SW_MAXIMIZE);
    }
}

void do_toggle(HWND h) {
    int newmode;
    { std::lock_guard<std::mutex> lk(g_mu); newmode = g_s.mode == SNAP_WINDOW_BORDERLESS ? SNAP_WINDOW_WINDOWED : SNAP_WINDOW_BORDERLESS; }
    if (newmode == SNAP_WINDOW_BORDERLESS) remember_normal(h);
    { std::lock_guard<std::mutex> lk(g_mu); g_s.mode = newmode; }
    apply_mode(h);
    save_settings();
    std::fprintf(stderr, "[window] mode -> %s\n", newmode == SNAP_WINDOW_BORDERLESS ? "borderless" : "windowed");
}

// ---- test helper: SNAP_WINDOW_TEST="blur@60,focus@75,f11@90,f11@95" (seconds after the window exists). It sends the same messages the OS
// sends, from another thread, and logs the state (boot_stderr.log lines starting "[window] test").
DWORD WINAPI test_thread(LPVOID p) {
    std::string spec = (const char*)p; free(p);
    struct Ev { double t; std::string name; };
    std::vector<Ev> evs;
    size_t i = 0;
    while (i < spec.size()) {
        size_t e = spec.find(',', i); if (e == std::string::npos) e = spec.size();
        std::string tok = spec.substr(i, e - i); i = e + 1;
        size_t at = tok.find('@'); if (at == std::string::npos) continue;
        evs.push_back({std::atof(tok.c_str() + at + 1), tok.substr(0, at)});
    }
    ULONGLONG t0 = GetTickCount64();
    for (auto& ev : evs) {
        while ((GetTickCount64() - t0) < (ULONGLONG)(ev.t * 1000)) Sleep(50);
        std::fprintf(stderr, "[window] test: %s at t=%.1f (paused=%d, master=%.2f)\n", ev.name.c_str(), ev.t, (int)g_paused.load(), snap_audio_get_volume(SNAP_VOL_MASTER));
        if (ev.name == "blur") SendMessageW(g_hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        else if (ev.name == "focus") SendMessageW(g_hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        else if (ev.name == "kill") SendMessageW(g_hwnd, WM_KILLFOCUS, 0, 0);
        else if (ev.name == "setfocus") SendMessageW(g_hwnd, WM_SETFOCUS, 0, 0);
        else if (ev.name == "f11") SendMessageW(g_hwnd, WM_KEYDOWN, VK_F11, 0);
        else if (ev.name == "altenter") SendMessageW(g_hwnd, WM_SYSKEYDOWN, VK_RETURN, 1 << 29);
        Sleep(1500);
        RECT r; GetClientRect(g_hwnd, &r);
        std::fprintf(stderr, "[window] test: after %s: paused=%d master=%.2f client=%ldx%ld mode=%d\n", ev.name.c_str(), (int)g_paused.load(), snap_audio_get_volume(SNAP_VOL_MASTER), r.right, r.bottom, snap_window_get_mode());
    }
    return 0;
}

}  // namespace

// ================= boot.cpp call sites =================
void snap_window_initial(DWORD* style, RECT* outer) {
    g_run_ev = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    load_settings();
    restore_after_crash();
    Settings s; { std::lock_guard<std::mutex> lk(g_mu); s = g_s; }
    if (s.mode == SNAP_WINDOW_BORDERLESS) {
        *style = kBorderlessStyle | WS_VISIBLE;
        *outer = mon_rect(pick_monitor(s.monitor, nullptr));
        return;
    }
    *style = kWindowedStyle | WS_VISIBLE;
    RECT o = outer_for_client(s.width, s.height);
    const int w = o.right - o.left, h = o.bottom - o.top;
    RECT r = {100, 100, 100 + w, 100 + h};
    if (s.x != INT_MIN) {
        RECT saved = {s.x, s.y, s.x + w, s.y + h};
        HMONITOR at = MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);   // null when the old monitor is gone
        if (at && (s.monitor < 0 || at == pick_monitor(s.monitor, nullptr))) r = saved;
        else r = centered(work_rect(pick_monitor(s.monitor, nullptr)), w, h);
        RECT wr = work_rect(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));   // keep the title bar reachable
        if (r.right > wr.right) OffsetRect(&r, wr.right - r.right, 0);
        if (r.bottom > wr.bottom) OffsetRect(&r, 0, wr.bottom - r.bottom);
        if (r.left < wr.left) OffsetRect(&r, wr.left - r.left, 0);
        if (r.top < wr.top) OffsetRect(&r, 0, wr.top - r.top);
    } else if (s.monitor >= 0) {
        r = centered(work_rect(pick_monitor(s.monitor, nullptr)), w, h);
    }
    *outer = r;
    std::lock_guard<std::mutex> lk(g_mu);
    g_norm = r; g_have_norm = true;
}

void snap_window_attach(HWND h) {
    g_hwnd = h;
    bool max; { std::lock_guard<std::mutex> lk(g_mu); max = g_s.maximized && g_s.mode == SNAP_WINDOW_WINDOWED; }
    if (max) ShowWindow(h, SW_MAXIMIZE);
    if (const char* t = std::getenv("SNAP_WINDOW_TEST"); t && *t) CreateThread(nullptr, 0, test_thread, _strdup(t), 0, nullptr);
}

bool snap_window_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* res) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) { do_toggle(h); *res = 0; return true; }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30))) { do_toggle(h); *res = 0; return true; }
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN) { *res = 0; return true; }   // no beep
        break;
    case WM_ACTIVATE: set_focus(LOWORD(w) != WA_INACTIVE && !HIWORD(w), "WM_ACTIVATE"); break;
    case WM_KILLFOCUS: set_focus(false, "WM_KILLFOCUS"); break;
    case WM_SETFOCUS: set_focus(true, "WM_SETFOCUS"); break;
    case WM_EXITSIZEMOVE: remember_normal(h); save_settings(); break;
    case WM_CLOSE: case WM_DESTROY:
        remember_normal(h); save_settings(); mute_off();
        if (g_paused.exchange(false)) { os_time_pause(0); SetEvent(g_run_ev); }
        break;
    case WM_SNAP_APPLY: apply_mode(h); save_settings(); *res = 0; return true;
    }
    return false;
}

// SNAP_FAST_BOOT=1: the load phase renders black frames at the game's 60 Hz pace. The frame limiter is switched off for the first
// SNAP_FAST_BOOT_FRAMES presents (default 3200; display vsync still applies when the game asks for present interval 1) and then
// restored to 60 Hz, so the title and everything after run at normal speed.
static void fast_boot_step(uint64_t n) {
    static const int frames = [] {
        const char* e = std::getenv("SNAP_FAST_BOOT");
        if (!e || !*e || *e == '0') return 0;
        const char* f = std::getenv("SNAP_FAST_BOOT_FRAMES");
        int v = f ? std::atoi(f) : 3200;
        return v > 0 ? v : 3200;
    }();
    if (!frames) return;
    if (n == 1) { nvnHostSetFramePacing(0); std::fprintf(stderr, "[window] fast boot: frame limiter off for %d presents\n", frames); }
    else if (n == (uint64_t)frames) { nvnHostSetFramePacing(60); std::fprintf(stderr, "[window] fast boot: frame limiter back to 60 Hz at present %llu\n", (unsigned long long)n); }
}

void snap_window_on_present() {
    while (g_paused.load()) WaitForSingleObject(g_run_ev, 250);
    static uint64_t n = 0;
    fast_boot_step(++n);
}

// ================= C API =================
extern "C" {
int snap_window_get_mode(void) { std::lock_guard<std::mutex> lk(g_mu); return g_s.mode; }
void snap_window_set_mode(int mode) {
    { std::lock_guard<std::mutex> lk(g_mu); mode = mode ? SNAP_WINDOW_BORDERLESS : SNAP_WINDOW_WINDOWED; if (g_s.mode == mode) return; }
    if (g_hwnd) PostMessageW(g_hwnd, WM_KEYDOWN, VK_F11, 0);   // same path as the key: remembers the windowed rectangle first
}
void snap_window_toggle_fullscreen(void) { if (g_hwnd) PostMessageW(g_hwnd, WM_KEYDOWN, VK_F11, 0); }
int snap_window_monitor_count(void) { return (int)monitors().size(); }
const char* snap_window_monitor_name(int index) {
    static thread_local std::string s;
    auto v = monitors();
    if (index < 0 || index >= (int)v.size()) return nullptr;
    s = v[index].name; return s.c_str();
}
int snap_window_get_monitor(void) { std::lock_guard<std::mutex> lk(g_mu); return g_s.monitor; }
void snap_window_set_monitor(int index) {
    { std::lock_guard<std::mutex> lk(g_mu); g_s.monitor = index < -1 ? -1 : index; g_have_norm = false; g_s.x = INT_MIN; }
    if (g_hwnd) PostMessageW(g_hwnd, WM_SNAP_APPLY, 0, 0);   // borderless: moves to that monitor; windowed: centers on it
}
int snap_window_get_pause_on_focus_loss(void) { std::lock_guard<std::mutex> lk(g_mu); return g_s.pause_on_focus_loss; }
void snap_window_set_pause_on_focus_loss(int on) { { std::lock_guard<std::mutex> lk(g_mu); g_s.pause_on_focus_loss = on != 0; } save_settings(); }
int snap_window_get_mute_in_background(void) { std::lock_guard<std::mutex> lk(g_mu); return g_s.mute_in_background; }
void snap_window_set_mute_in_background(int on) { { std::lock_guard<std::mutex> lk(g_mu); g_s.mute_in_background = on != 0; } save_settings(); }
int snap_window_frame_cap(void) { return g_cap.load(std::memory_order_relaxed); }
void snap_window_set_frame_cap(int fps) { if (fps < 0) fps = 0; { std::lock_guard<std::mutex> lk(g_mu); g_s.frame_cap = fps; } g_cap = fps; save_settings(); }
int snap_window_focused(void) { return g_focused.load(); }
int snap_window_paused(void) { return g_paused.load(); }
}
