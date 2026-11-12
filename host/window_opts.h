// SPDX-License-Identifier: MIT
// Window options for the game window (borderless fullscreen, monitor, remembered size and position, pause and mute on focus
// loss, frame cap). Settings: %APPDATA%\SnapRecomp\window.json (SNAP_CONFIG_DIR moves the folder) plus SNAP_WINDOW_* env overrides.
// Documented in host/README.md ("Window options"). boot.cpp calls only snap_window_initial / attach / proc / on_present.
#pragma once
#ifdef _WIN32
#include <windows.h>
// boot.cpp call sites. initial: client size, style and outer rect for CreateWindowW. attach: after creation. proc: first thing in
// the window procedure (returns true when it handled the message; *res is the result). on_present: from the NVN present hook
// (blocks the game thread while the window is paused).
void snap_window_initial(DWORD* style, RECT* outer);
void snap_window_attach(HWND h);
bool snap_window_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* res);
void snap_window_on_present();
#endif

// Public C API (callable from any thread; changes are applied on the window thread and saved to window.json).
#ifdef __cplusplus
extern "C" {
#endif
enum { SNAP_WINDOW_WINDOWED = 0, SNAP_WINDOW_BORDERLESS = 1 };
int  snap_window_get_mode(void);
void snap_window_set_mode(int mode);
void snap_window_toggle_fullscreen(void);               // F11 and Alt+Enter call this
int  snap_window_monitor_count(void);
const char* snap_window_monitor_name(int index);        // "\.\DISPLAY1 1920x1080" style; NULL when out of range
int  snap_window_get_monitor(void);                     // -1 = wherever the window is
void snap_window_set_monitor(int index);                // -1 or 0..count-1; moves a borderless window at once
int  snap_window_get_pause_on_focus_loss(void);
void snap_window_set_pause_on_focus_loss(int on);
int  snap_window_get_mute_in_background(void);
void snap_window_set_mute_in_background(int on);
int  snap_window_frame_cap(void);                       // fps, 0 = no extra cap (the game's own pacing); read by nvn Pace()
void snap_window_set_frame_cap(int fps);
int  snap_window_focused(void);
int  snap_window_paused(void);
#ifdef __cplusplus
}
#endif
