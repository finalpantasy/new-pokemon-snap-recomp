// SPDX-License-Identifier: MIT
// Frame rate target (SNAP_FPS, settings key fps_target): parsing and the pacing rate it gives, with no guest, host or time access so
// tests/mods/highfps_test.cpp can check it directly. host/mods/highfps.cpp applies the result from the present hook.
//
// Values: "default" / "" / 60  the game's own 60 fps (nothing is touched)
//         any number 61 to 1000 (decimals allowed: 120, 128, 143.988, 180, 200, 360 ...)   that simulation and present rate
//         "unlocked" (also uncapped, unlimited, max)   no limiter: the game runs as fast as the PC allows
//         "monitor" (also refresh, display, match)     the refresh rate of the window's monitor (143.988 Hz panels get 143.988, locked to vblank)
// The frame rate limit (window.json frame_cap, SNAP_WINDOW_FRAME_CAP) still applies: the pacing rate is the lower of the two, so the
// game's own vsync clock and the limiter tick together (a 144 target under a 120 limit simulates and presents at 120). A limit of 60 or
// lower leaves the game's own 60 and lets the limiter (nvn_vk.cpp Pace) slow the presents, as it always did.
#pragma once
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace highfps {

enum Kind { K_DEFAULT = 0, K_FIXED, K_UNLOCKED, K_MONITOR, K_INVALID };

struct Target {
    Kind kind = K_DEFAULT;
    double hz = 0;   // K_FIXED only
};

constexpr double kMinHz = 60.5, kMaxHz = 1000.0;

inline bool word_is(const char* s, const char* w) {
    for (; *s && *w; s++, w++)
        if (std::tolower((unsigned char)*s) != *w) return false;
    return !*s && !*w;
}

inline Target parse(const char* e) {
    Target t;
    if (!e) return t;
    while (*e == ' ') e++;
    if (!*e || word_is(e, "default") || word_is(e, "original")) return t;
    if (word_is(e, "unlocked") || word_is(e, "uncapped") || word_is(e, "unlimited") || word_is(e, "max")) { t.kind = K_UNLOCKED; return t; }
    if (word_is(e, "monitor") || word_is(e, "refresh") || word_is(e, "display") || word_is(e, "match")) { t.kind = K_MONITOR; return t; }
    char* end = nullptr;
    const double v = std::strtod(e, &end);
    while (end && *end == ' ') end++;
    if (!end || end == e || *end) { t.kind = K_INVALID; return t; }
    if (v >= 59.5 && v < kMinHz) return t;   // 60 (and 59.94): the game's own
    if (v < kMinHz || v > kMaxHz) { t.kind = K_INVALID; return t; }
    t.kind = K_FIXED;
    t.hz = v;
    return t;
}

// The rate to pass to nvnHostSetFramePacing: -1 = leave the game's own pacing alone (default target), 0 = limiter off (unlocked, no limit).
// cap = frame rate limit in fps (0 = none); panel = refresh of the window's monitor in Hz (0 = not known yet).
inline double pace_hz(const Target& t, int cap, double panel) {
    double hz;
    switch (t.kind) {
        case K_FIXED: hz = t.hz; break;
        case K_MONITOR: hz = panel >= kMinHz ? panel : 60.0; break;
        case K_UNLOCKED:
            if (cap <= 0) return 0.0;
            return cap > 60 ? (double)cap : 60.0;
        default: return -1.0;
    }
    if (cap > 0 && cap < hz) hz = cap > 60 ? (double)cap : 60.0;
    return hz;
}

}  // namespace highfps
