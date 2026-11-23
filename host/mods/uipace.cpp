// SPDX-License-Identifier: MIT
// Interface pacing and pad sampling above 60 fps (UI-HZ-01, INPUT-HZ-01; analysis/perf/high-fps.md).
//
// The game's interface runs once per game frame and counts frames, so with SNAP_FPS=120|144 it ran 2 to 2.4 times too fast (menus,
// dialogue, hint panels) while the 3D scene, which uses the measured timer, kept its speed. Three inline hooks in main (recomp/snap_gen.py
// UI_PACE_INLINE; they exist only in a main generated with SNAP_GEN_UI_PACE=1) fix that. Each returns at once when the effective pace is
// 60 fps or below (SNAP_FPS unset or 60, or a frame limit under 60), so the default game is unchanged. With SNAP_FPS=unlocked the measured
// present rate decides (nvnHostEffectivePaceHz), so the interface keeps 60 steps a second at any uncapped rate.
//
//   rt_gm_ui_enter    handler main+0x7835d0 after it saved its object (x0). x16 = 1 run the handler, 0 branch to its drawing section
//                     (0x794b4c): the object's logic, timeline advance and event 22 are skipped, the movies are still drawn. Only an
//                     object the handler would carry on with (started +0xc0, ready +0xc6, not held +0xc4) is skipped.
//   rt_gm_ui_event    before the handler raises the per-frame event 22. x16 = 0 leaves through the handler's exit (0x795f08).
//   rt_gm_ui_pad      the pad update's epilogue (main+0x925b60 ... 0x9263ac), x19 = pad. Marks the game frame (one decision per game
//                     frame, so pad and handlers agree) and, above 60 fps, makes the pad change only on interface steps: other frames
//                     show the last step's buttons and no new edges; a step compares with the last step; a press that came and went
//                     in between still counts. Sticks are not touched.
//
//   rt_gm_ui_pointer  the pointer update main+0x643a70, before `fcmp s1, #0.0` at 0x643ccc: s1 = the frame time it was given ([x19+0x1c],
//                     seconds). With a positive time the game scales the pointer step by time * 60; with 0 (the game clock stopped,
//                     as in the pause menu) it moves one fixed step per frame. Above 60 fps a zero time is replaced by the measured
//                     game-frame time (smoothed over about 16 frames, 1 to 100 ms), so the step is per second. FPS-02 in
//                     analysis/perf/fps_audit.md.
//
//   SNAP_UI_PACE=0     kill switch for the handler and event hooks (every frame steps, as without this file).
//   SNAP_PAD_PACE=0    kill switch for the pad sampling (the pad stays as the game wrote it).
//   SNAP_PTR_PACE=0    kill switch for the pointer time (a stopped clock moves the pointer per frame, as without this file).
//   SNAP_UI_PACE_HOVER=0  kill switch for keeping the pointer's "over an entry" flag on frames without a step (the cursor arc blinks again).
//   SNAP_UI_PACE_TRACE=1  logs the handler counts every 5 s ("[uipace]" in stderr).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loader.h"
#include "uipace_core.h"

extern "C" uint64_t snap_present_count();   // host/boot.cpp
extern "C" double nvnHostEffectivePaceHz();   // runtime/nvn/nvn_entry.cpp: the rate the presents follow (pace target or the lower frame
                                              // rate limit; the measured rate when unlocked), 0 = unknown (fast boot window)

namespace {

bool g_ui_on = true, g_pad_on = true, g_ptr_on = true, g_hover_on = true, g_trace;
std::atomic<bool> g_cfg{false};

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void cfg_once() {
    if (g_cfg.load(std::memory_order_acquire)) return;
    if (const char* e = std::getenv("SNAP_UI_PACE")) g_ui_on = !(*e == '0' && !e[1]);
    if (const char* e = std::getenv("SNAP_PAD_PACE")) g_pad_on = !(*e == '0' && !e[1]);
    if (const char* e = std::getenv("SNAP_PTR_PACE")) g_ptr_on = !(*e == '0' && !e[1]);
    if (const char* e = std::getenv("SNAP_UI_PACE_TRACE")) g_trace = *e && *e != '0';
    if (const char* e = std::getenv("SNAP_UI_PACE_HOVER")) g_hover_on = !(*e == '0' && !e[1]);
    if (g_trace || !g_ui_on || !g_pad_on || !g_ptr_on)
        std::fprintf(stderr, "[uipace] ui=%d pad=%d ptr=%d trace=%d\n", (int)g_ui_on, (int)g_pad_on, (int)g_ptr_on, (int)g_trace);
    g_cfg.store(true, std::memory_order_release);
}

// The period the presents follow (nvn_vk.cpp Pace(): the target's period, never shorter than the frame limit's; with the frame rate unlocked
// the measured present interval, so an unlocked 200 fps interface still takes 60 steps a second). 0 = unknown: every frame steps.
uint64_t period_ns() {
    const double hz = nvnHostEffectivePaceHz();
    return hz > 1.0 ? (uint64_t)(1e9 / hz) : 0;
}

// The game frame: the pad update runs once in each, so the decision is made there and the handlers until its next update use it.
// With no pad update for PAD_GAP_NS the decision falls back to once per presented frame.
constexpr uint64_t kPadGapNs = 100000000ull;
uipace::Pace g_pace, g_pace_presented;
std::atomic<uint64_t> g_pad_tick_ns{0}, g_pad_dt_ns{0};
uint64_t g_pad_tick;
uipace::PadState g_pads[4];
double g_ptr_avg;   // smoothed game-frame time for the pointer (ns)
uint64_t g_pad_ptr[4];

int pace_step(uint64_t now) {
    const uint64_t seen = g_pad_tick_ns.load(std::memory_order_acquire);
    if (seen && now >= seen && now - seen < kPadGapNs) return g_pace.step;
    return uipace::decide(g_pace_presented, snap_present_count(), now, period_ns());
}

uint64_t g_ran, g_drawn, g_win_ns, g_win_present, g_edges, g_steps, g_pad_frames;
uint64_t g_hover_frames, g_hover_set, g_hover_fixed, g_hover_present = ~0ull;   // the pointer flag below (UI-HZ-02)

void trace_counts(uint64_t now, int ran) {
    if (ran) g_ran++; else g_drawn++;
    const uint64_t fr = snap_present_count();
    if (!g_win_ns) g_win_ns = now, g_win_present = fr;
    if (now - g_win_ns < 5000000000ull) return;
    const double s = (double)(now - g_win_ns) / 1e9;
    std::fprintf(stderr, "[uipace] %.1f s: %.1f presents/s, handlers run %.1f/s, draw only %.1f/s, period %.3f ms; game frames %.1f/s, steps %.1f/s, pad press edges %llu\n", s,
                 (double)(snap_present_count() - g_win_present) / s, (double)g_ran / s, (double)g_drawn / s, (double)period_ns() / 1e6, (double)g_pad_frames / s,
                 (double)g_steps / s, (unsigned long long)g_edges);
    std::fprintf(stderr, "[uipace] pointer over an entry in %llu of %llu frames (put back after a skipped step: %llu)\n", (unsigned long long)g_hover_set,
                 (unsigned long long)g_hover_frames, (unsigned long long)g_hover_fixed);
    g_win_ns = now, g_win_present = fr, g_ran = g_drawn = g_pad_frames = g_steps = 0, g_hover_frames = g_hover_set = g_hover_fixed = 0;
}

}  // namespace

// The pointer's "over an entry" flag (pointer +0x128, one byte). The end of every frame rebuilds it from what the menu logic reported in that
// frame, and the cursor draws its orange arc from it in the next one. A frame whose logic is skipped reports nothing, so the frame after it
// found the flag clear and drew no arc: at 72 to 240 fps the arc blinked after every frame between two steps (UI-HZ-02). The pointer update
// (first thing in a frame) keeps the value a stepped frame left and puts it back after a skipped one. SNAP_UI_PACE_HOVER=0 turns it off.
constexpr uint32_t kPtrHoverOff = 0x128;
std::atomic<uint64_t> g_ptr_obj{0};
int g_ptr_hover = -1;   // -1 = not known yet
bool g_prev_step = true;

void hover_seen(uint64_t ptr) {
    if (!ptr || IsBadWritePtr((void*)(ptr + kPtrHoverOff), 1)) return;
    g_ptr_obj.store(ptr, std::memory_order_relaxed);
    uint8_t* f = (uint8_t*)(ptr + kPtrHoverOff);
    if (g_prev_step) g_ptr_hover = *f;
    else if (g_hover_on && g_ptr_hover >= 0 && *f != (uint8_t)g_ptr_hover) *f = (uint8_t)g_ptr_hover, g_hover_fixed++;
    g_prev_step = g_pace.step != 0;
}

void hover_count() {   // trace: the flag as the frame's first handler finds it
    const uint64_t ptr = g_ptr_obj.load(std::memory_order_relaxed), fr = snap_present_count();
    if (!ptr || fr == g_hover_present) return;
    g_hover_present = fr, g_hover_frames++;
    g_hover_set += *(const uint8_t*)(ptr + kPtrHoverOff) != 0;
}

extern "C" void rt_gm_ui_enter(Cpu* c) {
    c->x[16] = 1;
    cfg_once();
    if (!g_ui_on) return;
    const uint64_t now = now_ns();
    int step = pace_step(now);
    if (g_trace) hover_count();
    if (!step) {
        // Only an object the handler would carry on with may be drawn without a step: started (+0xc0), ready (+0xc6), not held (+0xc4).
        const uint8_t* o = (const uint8_t*)c->x[0];
        if (!o || !o[0xc0] || !o[0xc6] || o[0xc4]) step = 1;
    }
    c->x[16] = (uint64_t)step;
    if (g_trace) trace_counts(now, step);
}

extern "C" void rt_gm_ui_event(Cpu* c) {
    c->x[16] = 1;
    cfg_once();
    if (!g_ui_on) return;
    c->x[16] = (uint64_t)pace_step(now_ns());
}

extern "C" void rt_gm_ui_pad(Cpu* c) {
    cfg_once();
    const uint64_t pad = c->x[19];
    if (!pad || (!g_ui_on && !g_pad_on)) return;
    const uint64_t now = now_ns(), period = period_ns();
    int slot = 0;
    while (slot < 4 && g_pad_ptr[slot] && g_pad_ptr[slot] != pad) slot++;
    if (slot == 4) return;   // more pads than the game has: leave them as they are
    g_pad_ptr[slot] = pad;
    if (slot == 0) {   // the first pad marks the game frame
        const uint64_t before = g_pad_tick_ns.load(std::memory_order_relaxed);
        g_pad_dt_ns.store(before && now > before ? now - before : 0, std::memory_order_relaxed);
        uipace::decide(g_pace, g_pad_tick++, now, g_ui_on ? period : 0);
        g_pad_tick_ns.store(now, std::memory_order_release);
    }
    if (!g_pad_on) return;
    uint32_t* mask = (uint32_t*)(pad + uipace::kPadMaskOff);
    uint8_t* bytes = (uint8_t*)(pad + uipace::kPadBytesOff);
    uipace::sample_pad(g_pads[slot], mask, bytes, g_pace.step != 0, !g_ui_on || !uipace::above60(period));
    if (g_trace && slot == 0) {   // the pad as the game will read it: every press edge it sees (compare 60 and 144 with the same script)
        g_pad_frames++;
        g_steps += g_pace.step != 0;
        g_edges += (uint64_t)__builtin_popcount(mask[1]);
    }
}

extern "C" void rt_gm_ui_pointer(Cpu* c) {
    cfg_once();
    if (g_ui_on && uipace::above60(period_ns())) hover_seen(c->x[21]);
    float t;
    std::memcpy(&t, &c->v[1], sizeof t);
    if (g_trace) {   // per second: frames with a game time / with 0, and how far the pointer (x21+0x118, +0x11c) moved
        // speed = the last frame's step over the real time since the previous call; the median over moving frames is the pointer speed
        static uint64_t n_zero, n_time, win_ns, last_ns;
        static float last_x, last_y;
        static double moved, speeds[512];
        static int n_speeds;
        const uint64_t now = now_ns();
        (t > 0.0f ? n_time : n_zero)++;
        const uint64_t o = c->x[21];
        if (o && !IsBadReadPtr((const void*)(o + 0x118), 8)) {
            float x, y;
            std::memcpy(&x, (const void*)(o + 0x118), 4);
            std::memcpy(&y, (const void*)(o + 0x11c), 4);
            const double step = __builtin_sqrt((double)(x - last_x) * (x - last_x) + (double)(y - last_y) * (y - last_y));
            moved += step;
            if (step > 0.01 && last_ns && now > last_ns && n_speeds < 512) speeds[n_speeds++] = step * 1e9 / (double)(now - last_ns);
            last_x = x, last_y = y;
        }
        last_ns = now;
        if (!win_ns) win_ns = now, moved = 0, n_speeds = 0;
        if (now - win_ns >= 1000000000ull) {
            const double s = (double)(now - win_ns) / 1e9;
            double med = 0;
            if (n_speeds) {
                std::nth_element(speeds, speeds + n_speeds / 2, speeds + n_speeds);
                med = speeds[n_speeds / 2];
            }
            if (moved > 0.5 || n_zero)
                std::fprintf(stderr, "[uipace] pointer: %.1f s, %llu frames with a game time, %llu with 0, moved %.0f units/s, median speed %.0f units/s over %d moving frames (period %.3f ms, fix %d)\n",
                             s, (unsigned long long)n_time, (unsigned long long)n_zero, moved / s, med, n_speeds, (double)period_ns() / 1e6, (int)g_ptr_on);
            n_time = n_zero = 0, win_ns = now, moved = 0, n_speeds = 0;
        }
    }
    if (!g_ptr_on) return;
    const uint64_t period = period_ns();
    if (t > 0.0f || !uipace::above60(period)) return;
    const float dt = uipace::pointer_dt(g_ptr_avg, g_pad_dt_ns.load(std::memory_order_relaxed), period);
    std::memcpy(&c->v[1], &dt, sizeof dt);
}
