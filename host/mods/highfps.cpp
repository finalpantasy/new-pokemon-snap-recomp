// SPDX-License-Identifier: MIT
// High frame rate spike (docs/high-fps-plan.md, WP1 first half). Everything here is inert unless an env switch is set, so the default
// game (SNAP_FPS unset or 60) keeps its original 60 fps pacing, byte for byte.
//
//   SNAP_FPS=<61..1000>|unlocked|monitor  target simulation rate (any number, decimals allowed; highfps_core.h). Applies
//                               nvnHostSetFramePacing from the present hook, after the SNAP_FAST_BOOT window (which hard-codes 0, then 60),
//                               and again whenever the target, the frame rate limit or the monitor's refresh changes. The pacing rate is
//                               the lower of the target and the frame rate limit, so the guest vsync clock and the limiter agree.
//                               unlocked: limiter off (or at the frame rate limit when one is set); monitor: the window's monitor refresh.
//   SNAP_FPS_AFTER_LOAD=<target>  test aid (same-photo score check across rates): once a save state has been loaded, this target replaces
//                               SNAP_FPS (route hifps_score_ck: photos taken and scored at one rate, the state loaded, scored again at another).
//   SNAP_FPS_UNCAPPED=1         measurement only: limiter off (nvnHostSetFramePacing(0)) so the real uncapped rate shows. Works with SNAP_FPS.
//                               Unlocked and uncapped also tell nvn the limiter is off on purpose (nvnHostSetFramePacingUnlocked), so the
//                               interface pacing (uipace.cpp) and smoothing (nvn_uismooth.h) follow the measured present rate.
//   SNAP_FPS_TRACE=<csv>        diagnostic: per clock update row (see below). Forces g_fnprof (slow; not for perf numbers).
//   SNAP_FPS_MSWATCH=1          diagnostic: hardware read/write watchpoints on the clock's three integer ms channels (+0x10, +0x28, +0x40);
//                               a VEH counts the rip of every access; table printed to stderr as "[highfps-w]" (resolve rva in the link map).
//   SNAP_FPS_WATCH=<fn>,<off>[,<present>]  diagnostic: the same watchpoint (read/write, 4 bytes) on x0+off of the first call of main+<fn>
//                               at or after that present (e.g. 0x8ad7c0,0x2d0: the object that stores 1/60 there), in the same
//                               "[highfps-w]" table, and every change of the float there with its time ("[highfps-s] <ms> <value>").
//                               With SNAP_FNTRACE also set the function hook stays on, the object is the latest call's and only the
//                               value is logged (no watchpoint: suspending threads to arm it causes hitches).
//
// The 14 ms sleep floor (main+0x4e0f64..0x4e1018, SleepThread call with x30 = 0x71004e1010) is NOT clamped: measured 2026-11-14, that loop
// never runs in play. All 17,920 clock updates of a title-to-evaluation Florio run came from the other loop (lr main+0x4e09c0, no
// floor), and the sleep call at 0x4e1010 was hit 0 times (analysis/perf/high-fps.md). No OS-layer or main change is needed.
// The pacing worker's files (nvn_pace.*, nvn_vk.cpp) are not touched: the only call into them is the public setter.
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "hle/live.h"   // rows that apply with no restart
#include "highfps_core.h"

extern "C" volatile int g_fnprof;
extern "C" void nvnHostSetFramePacing(double hz);
extern "C" void nvnHostSetFramePacingUnlocked(int on);
extern "C" double snap_pace_panel_hz();      // runtime/nvn/nvn_pace.cpp: refresh of the window's monitor, 0 = unknown
extern "C" int snap_window_frame_cap();      // host/window_opts.cpp: fps, 0 = none
extern "C" int snap_state_count(const char* kind);   // runtime/os/os_state.cpp: save-state requests finished ("loads" = loads ok)
namespace nvn { extern double g_paceHz; }   // runtime/nvn/nvn_vk.cpp
extern "C" uint64_t snap_present_count();

namespace {

const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kClockUpdate = kMainBase + 0x6abea0;

highfps::Target g_target;     // K_DEFAULT = inert
bool g_uncapped, g_watch_on;
uint64_t g_wfn, g_woff, g_wfrom;     // SNAP_FPS_WATCH
bool g_wkeep;
std::atomic<uint64_t> g_wobj{0};
FILE* g_csv;
std::atomic<uint64_t> g_clock{0};   // guest address of the clock object (x0 of the update)
int g_fast_frames;

double qpc_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return 1000.0 * (double)n.QuadPart / (double)f.QuadPart;
}

// ---- hardware watchpoints (pattern of host/mods/neofilm.cpp, but read+write and 8 bytes)
std::mutex g_wmu;
std::map<uint64_t, uint64_t> g_hits;   // (dr6 bits << 56) | rip rva
uint64_t g_modbase;

LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    {
        std::lock_guard<std::mutex> lk(g_wmu);
        g_hits[((uint64_t)(dr6 & 0xf) << 56) | (ep->ContextRecord->Rip - g_modbase)]++;
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_threads(const uint64_t* addr, int n, uint64_t lenbits) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT cx{};
            cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &cx)) {
                DWORD64* dr[3] = {&cx.Dr0, &cx.Dr1, &cx.Dr2};
                DWORD64 dr7 = 0;
                for (int i = 0; i < n && i < 3; i++) {
                    *dr[i] = addr[i];
                    dr7 |= (1ull << (i * 2)) | (3ull << (16 + i * 4)) | (lenbits << (18 + i * 4));   // enable, read/write, length
                }
                cx.Dr7 = dr7;
                SetThreadContext(h, &cx);
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

DWORD WINAPI watch_thread(LPVOID) {
    g_modbase = (uint64_t)GetModuleHandleA(nullptr);
    AddVectoredExceptionHandler(1, veh);
    for (int sec = 0;; sec++) {
        Sleep(1000);
        uint64_t clk = g_clock.load(), obj = g_wobj.load();
        if (g_wfn) {
            if (!obj) continue;
            const uint64_t a = obj + g_woff;
            if (sec % 3 == 0 && !g_wkeep) arm_threads(&a, 1, 3);   // 4 bytes; not with SNAP_FNTRACE (value log only: the object moves)
        } else {
            if (!clk) continue;
            const uint64_t a[3] = {clk + 0x10, clk + 0x28, clk + 0x40};
            if (sec % 3 == 0) arm_threads(a, 3, 2);   // 8 bytes
        }
        if (sec % 10 == 9) {
            std::lock_guard<std::mutex> lk(g_wmu);
            if (obj && !IsBadReadPtr((const void*)(obj + g_woff), 8)) {
                float v[2];
                std::memcpy(v, (const void*)(obj + g_woff), 8);
                fprintf(stderr, "[highfps-w] t=%d object %#llx +%#llx = %.7f, %.7f\n", sec, (unsigned long long)obj, (unsigned long long)g_woff, v[0], v[1]);
            }
            for (auto& kv : g_hits)
                fprintf(stderr, "[highfps-w] t=%d dr=%x rva=%#llx n=%llu\n", sec, (unsigned)(kv.first >> 56), (unsigned long long)(kv.first & 0x00ffffffffffffffull), (unsigned long long)kv.second);
        }
    }
}

// SNAP_FPS_WATCH: logs every change of the watched float (polled every 1 ms) with its time.
DWORD WINAPI sample_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    uint32_t last = 0;
    for (;;) {
        Sleep(1);
        const uint64_t obj = g_wobj.load();
        if (!obj || IsBadReadPtr((const void*)(obj + g_woff), 4)) continue;
        static uint64_t last_obj;
        uint32_t v;
        std::memcpy(&v, (const void*)(obj + g_woff), 4);
        if (v == last && obj == last_obj) continue;
        last = v, last_obj = obj;
        float f;
        std::memcpy(&f, &v, 4);
        fprintf(stderr, "[highfps-s] %.3f %.6f p%llu obj %#llx\n", qpc_ms(), f, (unsigned long long)snap_present_count(), (unsigned long long)obj);
    }
}

struct Init {
    Init() {
        // SNAP_FPS and SNAP_FAST_BOOT are not read here: this runs before main(), and the saved Frame rate target and Fast boot rows only become
        // environment variables when hle/settings loads the profile (a saved 120 was ignored until 2026-11-18). read_rows() below reads them.
        if (const char* u = std::getenv("SNAP_FPS_UNCAPPED")) g_uncapped = *u && *u != '0';
        if (const char* t = std::getenv("SNAP_FPS_TRACE")) if (*t) {
            g_csv = std::fopen(t, "wb");
            if (g_csv) { std::fprintf(g_csv, "idx,present,wall_ms,lr,mode,dt_prev,ms_prev,ms2_prev,ms3_prev,dt2_prev,dt3_prev\n"); g_fnprof = 1; }
        }
        if (const char* w = std::getenv("SNAP_FPS_MSWATCH")) if (*w && *w != '0') {
            g_watch_on = true;
            g_fnprof = 1;   // only to learn the clock address; observe() turns it off again when SNAP_FPS_TRACE is not set
            CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
        }
        if (const char* w = std::getenv("SNAP_FPS_WATCH")) if (*w && !g_watch_on) {
            char* end;
            g_wfn = std::strtoull(w, &end, 0);
            g_woff = *end == ',' ? std::strtoull(end + 1, &end, 0) : 0;
            g_wfrom = *end == ',' ? std::strtoull(end + 1, nullptr, 0) : 0;
            if (g_wfn) {
                g_wfn += kMainBase;
                g_watch_on = true;
                const char* ft = std::getenv("SNAP_FNTRACE");
                g_wkeep = ft && *ft;
                if (g_wfrom) CreateThread(nullptr, 0, [](LPVOID) -> DWORD { while (snap_present_count() < g_wfrom) Sleep(5); g_fnprof = 1; return 0; }, nullptr, 0, nullptr);
                else g_fnprof = 1;
                CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
                CreateThread(nullptr, 0, sample_thread, nullptr, 0, nullptr);
            }
        }
        if (g_uncapped || g_csv || g_watch_on)
            std::fprintf(stderr, "[highfps] uncapped=%d trace=%d mswatch=%d\n", (int)g_uncapped, g_csv != nullptr, (int)g_watch_on);
    }
} g_init;

// The rows the settings layer exports, read at the first present (the profile is loaded by then) and again whenever a row changes.
highfps::Target read_rows(bool first) {
    hle::live::EnvRead env_read;
    const char* e = std::getenv("SNAP_FPS");
    highfps::Target t = highfps::parse(e);
    if (t.kind == highfps::K_INVALID) {
        std::fprintf(stderr, "[highfps] SNAP_FPS=%s ignored (use 60, any number from 61 to 1000, unlocked or monitor)\n", e);
        t = highfps::Target{};
    }
    if (first) {
        const char* f = std::getenv("SNAP_FAST_BOOT");
        if (f && *f && *f != '0') { const char* n = std::getenv("SNAP_FAST_BOOT_FRAMES"); int fr = n ? std::atoi(n) : 3200; g_fast_frames = fr > 0 ? fr : 3200; }
    }
    return t;
}

const char* kind_name(const highfps::Target& t) {
    switch (t.kind) { case highfps::K_FIXED: return "fixed"; case highfps::K_UNLOCKED: return "unlocked"; case highfps::K_MONITOR: return "monitor"; default: return "default"; }
}

}  // namespace

// Called by loader.cpp fnprof_hit for every guest function entry while g_fnprof is set (diagnostics only).
extern "C" void snap_highfps_observe(uint64_t a, uint64_t lr, uint64_t x0) {
    if (g_wfn) {
        if (a == g_wfn) {   // the first call's object; with SNAP_FNTRACE also on (function hook stays on) the latest call's
            if (g_wkeep) g_wobj.store(x0);
            else if (!g_wobj.load()) { g_wobj.store(x0); if (!g_csv) g_fnprof = 0; }
        }
        if (a != kClockUpdate) return;
    }
    if (a != kClockUpdate) return;
    g_clock.store(x0);
    static uint64_t idx;
    static double prev;
    if (g_csv) {
        const uint8_t* p = (const uint8_t*)x0;
        double now = qpc_ms();
        uint32_t mode; float dt, dt2, dt3; uint64_t m1, m2, m3;
        std::memcpy(&mode, p + 0x18, 4); std::memcpy(&dt, p + 0x1c, 4); std::memcpy(&dt2, p + 0x34, 4); std::memcpy(&dt3, p + 0x4c, 4);
        std::memcpy(&m1, p + 0x10, 8); std::memcpy(&m2, p + 0x28, 8); std::memcpy(&m3, p + 0x40, 8);
        std::fprintf(g_csv, "%llu,%llu,%.3f,%#llx,%u,%.6f,%llu,%llu,%llu,%.6f,%.6f\n", (unsigned long long)idx, (unsigned long long)snap_present_count(), prev ? now - prev : 0.0,
                     (unsigned long long)(lr - kMainBase), mode, dt, (unsigned long long)m1, (unsigned long long)m2, (unsigned long long)m3, dt2, dt3);
        if ((idx & 255) == 255) std::fflush(g_csv);
        prev = now;
    }
    idx++;
    if (g_watch_on && !g_csv && !g_wfn) g_fnprof = 0;
}

// Called from the present hook (host/boot.cpp hook_present) after snap_window_on_present. n = present number (1-based).
// Every present: the wanted pacing rate from the target, the frame rate limit and the monitor's refresh (all can change live); applied when
// it differs from what nvn has (also after the fast boot window, whose end sets 60).
extern "C" void snap_highfps_on_present(uint64_t n) {
    static std::atomic<unsigned> seen{0};
    static bool first = true, active = false;
    static double logged = -2;
    if (hle::live::changed(seen)) {   // the first present, then every change: the Frame rate target row applies with no restart (hle/live.h)
        const bool was_first = first;
        first = false;
        const highfps::Target t = read_rows(was_first);
        if (was_first || t.kind != g_target.kind || t.hz != g_target.hz) {
            g_target = t;
            if (t.kind != highfps::K_DEFAULT) std::fprintf(stderr, "[highfps] SNAP_FPS=%s (%s)\n", std::getenv("SNAP_FPS"), kind_name(t));
            if (!was_first) std::fprintf(stderr, "[highfps] frame rate target changed in the options at present %llu\n", (unsigned long long)n);
        }
    }
    static const char* after_load = [] { const char* e = std::getenv("SNAP_FPS_AFTER_LOAD"); return e && *e ? e : nullptr; }();
    if (after_load && n % 16 == 0 && snap_state_count("loads") >= 1) {
        g_target = highfps::parse(after_load);
        if (g_target.kind == highfps::K_INVALID) g_target = highfps::Target{};
        std::fprintf(stderr, "[highfps] state loaded: frame rate target now %s at present %llu (SNAP_FPS_AFTER_LOAD)\n", after_load, (unsigned long long)n);
        after_load = nullptr;
    }
    if (g_target.kind == highfps::K_DEFAULT && !g_uncapped) {
        if (active) {   // switched back to the game's own 60 in the options
            active = false;
            nvnHostSetFramePacingUnlocked(0);
            nvnHostSetFramePacing(60.0);
            std::fprintf(stderr, "[highfps] pacing target 60 Hz at present %llu (the game's own)\n", (unsigned long long)n);
            logged = -2;
        }
        return;
    }
    if (g_fast_frames && n < (uint64_t)g_fast_frames) return;   // fast boot window: its own limiter-off pacing until it ends
    active = true;
    const double want = g_uncapped ? 0.0 : highfps::pace_hz(g_target, snap_window_frame_cap(), snap_pace_panel_hz());
    if (want < 0) return;
    const bool unlocked = want == 0.0;
    nvnHostSetFramePacingUnlocked(unlocked ? 1 : 0);
    if (nvn::g_paceHz != want) nvnHostSetFramePacing(want);
    if (want != logged) {
        logged = want;
        char hz[64];
        std::snprintf(hz, sizeof hz, "%.6g Hz", want);
        std::fprintf(stderr, "[highfps] pacing target %s at present %llu (target %s%s, frame rate limit %d, monitor %.3f Hz)\n",
                     g_uncapped ? "off (uncapped measurement)" : unlocked ? "off (unlocked)" : hz, (unsigned long long)n, kind_name(g_target),
                     g_target.kind == highfps::K_FIXED ? (" " + std::to_string((int)g_target.hz)).c_str() : "", snap_window_frame_cap(), snap_pace_panel_hz());
    }
}
