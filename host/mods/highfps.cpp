// SPDX-License-Identifier: MIT
// High frame rate spike (docs/high-fps-plan.md, WP1 first half). Everything here is inert unless an env switch is set, so the default
// game (SNAP_FPS unset or 60) keeps its original 60 fps pacing and 14 ms sleep floor, byte for byte.
//
//   SNAP_FPS=120|144            target simulation rate. Re-applies nvnHostSetFramePacing(fps) from the present hook (also right after the
//                               SNAP_FAST_BOOT window, which hard-codes 60) and clamps the main loop's 14 ms sleep floor (see below).
//   SNAP_FPS_FLOOR_MS=<ms>      the floor that replaces the game's 14 ms (default 0 = no floor; the limiter in the present path paces).
//   SNAP_FPS_UNCAPPED=1         measurement only: limiter off (nvnHostSetFramePacing(0)) so the real uncapped rate shows. Works with SNAP_FPS.
//   SNAP_FPS_TRACE=<csv>        diagnostic: per clock update row (see below). Forces g_fnprof (slow; not for perf numbers).
//   SNAP_FPS_MSWATCH=1          diagnostic: hardware read/write watchpoints on the clock's three integer ms channels (+0x10, +0x28, +0x40);
//                               a VEH counts the rip of every access; table printed to stderr as "[highfps-w]" (resolve rva in the link map).
//
// Sleep floor: the main loop (main+0x4e0f64..0x4e1018) sleeps (14 - floor(elapsed_ms)) ms through PLT stub f_7100a36ad0 (SleepThread) with
// x30 = 0x71004e1010. os_thread.cpp's ov_SleepThread passes (x30, ns) to snap_highfps_sleep_ns below, which shortens exactly that call.
// No main regenerate. The pacing worker's files (nvn_pace.*, nvn_vk.cpp) are not touched: the only call into them is the public setter.
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

extern "C" volatile int g_fnprof;
extern "C" void nvnHostSetFramePacing(double hz);
extern "C" uint64_t snap_present_count();

namespace {

const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kClockUpdate = kMainBase + 0x6abea0;
const uint64_t kFloorLr = kMainBase + 0x4e1010;   // return address of the sleep call in the main loop

int g_fps;                    // 0 = inert
bool g_uncapped, g_watch_on;
double g_floor_ms;
FILE* g_csv;
std::atomic<uint64_t> g_clock{0};   // guest address of the clock object (x0 of the update)
int g_fast_frames;
std::atomic<uint64_t> g_sl_lr[32], g_sl_n[32];
std::atomic<uint64_t> g_sl_calls{0}, g_sl_in_ns{0}, g_sl_out_ns{0}, g_sl_zero{0};

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

void arm_threads(uint64_t clk) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    const uint64_t offs[3] = {0x10, 0x28, 0x40};
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
                for (int i = 0; i < 3; i++) {
                    *dr[i] = clk + offs[i];
                    dr7 |= (1ull << (i * 2)) | (3ull << (16 + i * 4)) | (2ull << (18 + i * 4));   // enable, read/write, 8 bytes
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
        uint64_t clk = g_clock.load();
        if (!clk) continue;
        if (sec % 3 == 0) arm_threads(clk);
        if (sec % 10 == 9) {
            std::lock_guard<std::mutex> lk(g_wmu);
            for (auto& kv : g_hits)
                fprintf(stderr, "[highfps-w] t=%d dr=%x rva=%#llx n=%llu\n", sec, (unsigned)(kv.first >> 56), (unsigned long long)(kv.first & 0x00ffffffffffffffull), (unsigned long long)kv.second);
        }
    }
}

struct Init {
    Init() {
        const char* e = std::getenv("SNAP_FPS");
        if (e && *e) {
            int v = std::atoi(e);
            if (v == 120 || v == 144) g_fps = v;
            else if (v != 60) std::fprintf(stderr, "[highfps] SNAP_FPS=%s ignored (use 60, 120 or 144)\n", e);
        }
        if (const char* u = std::getenv("SNAP_FPS_UNCAPPED")) g_uncapped = *u && *u != '0';
        g_floor_ms = std::getenv("SNAP_FPS_FLOOR_MS") ? std::atof(std::getenv("SNAP_FPS_FLOOR_MS")) : 0.0;
        if (const char* f = std::getenv("SNAP_FAST_BOOT")) {
            if (*f && *f != '0') { const char* n = std::getenv("SNAP_FAST_BOOT_FRAMES"); int fr = n ? std::atoi(n) : 3200; g_fast_frames = fr > 0 ? fr : 3200; }
        }
        if (const char* t = std::getenv("SNAP_FPS_TRACE")) if (*t) {
            g_csv = std::fopen(t, "wb");
            if (g_csv) { std::fprintf(g_csv, "idx,present,wall_ms,lr,mode,dt_prev,ms_prev,ms2_prev,ms3_prev,dt2_prev,dt3_prev\n"); g_fnprof = 1; }
        }
        if (const char* w = std::getenv("SNAP_FPS_MSWATCH")) if (*w && *w != '0') {
            g_watch_on = true;
            g_fnprof = 1;   // only to learn the clock address; observe() turns it off again when SNAP_FPS_TRACE is not set
            CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
        }
        if (g_fps || g_uncapped || g_csv || g_watch_on)
            std::fprintf(stderr, "[highfps] SNAP_FPS=%d uncapped=%d floor=%.2f ms trace=%d mswatch=%d\n", g_fps, (int)g_uncapped, g_floor_ms, g_csv != nullptr, (int)g_watch_on);
    }
} g_init;

}  // namespace

// Called by loader.cpp fnprof_hit for every guest function entry while g_fnprof is set (diagnostics only).
extern "C" void snap_highfps_observe(uint64_t a, uint64_t lr, uint64_t x0) {
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
    if (g_watch_on && !g_csv) g_fnprof = 0;
}

// Called from the present hook (host/boot.cpp hook_present) after snap_window_on_present. n = present number (1-based).
extern "C" void snap_highfps_on_present(uint64_t n) {
    if (!g_fps && !g_uncapped) return;
    // fast_boot_step (window_opts.cpp) sets 0 at present 1 and 60 at present g_fast_frames; our value replaces both.
    if (n == 1 || (g_fast_frames && n == (uint64_t)g_fast_frames)) {
        nvnHostSetFramePacing(g_uncapped ? 0.0 : (double)g_fps);
        std::fprintf(stderr, "[highfps] pacing target %s at present %llu\n", g_uncapped ? "off (uncapped measurement)" : (g_fps == 144 ? "144 Hz" : "120 Hz"), (unsigned long long)n);
    }
    if (n % 1200 == 0) {
        for (int i = 0; i < 32 && g_sl_lr[i].load(); i++)
            std::fprintf(stderr, "[highfps] SleepThread callers: main+%#llx x%llu\n", (unsigned long long)(g_sl_lr[i].load() - kMainBase), (unsigned long long)g_sl_n[i].load());
        uint64_t c = g_sl_calls.load();
        std::fprintf(stderr, "[highfps] present %llu: floor sleeps %llu (mean in %.2f ms, mean out %.2f ms, %llu zero)\n", (unsigned long long)n, (unsigned long long)c,
                     c ? g_sl_in_ns.load() / 1e6 / (double)c : 0.0, c ? g_sl_out_ns.load() / 1e6 / (double)c : 0.0, (unsigned long long)g_sl_zero.load());
    }
}

// Called by ov_SleepThread (runtime/os/os_thread.cpp) with the guest return address and the requested ns. Returns the ns to sleep.
extern "C" int64_t snap_highfps_sleep_ns(uint64_t lr, int64_t ns) {
    if (g_fps) {   // tally callers (lr is the guest return address) so a wrong floor address is visible; printed from on_present
        for (int i = 0; i < 32; i++) {
            uint64_t cur = g_sl_lr[i].load();
            if (cur == lr) { g_sl_n[i]++; break; }
            if (!cur) { uint64_t z = 0; if (g_sl_lr[i].compare_exchange_strong(z, lr)) { g_sl_n[i]++; break; } if (g_sl_lr[i].load() == lr) { g_sl_n[i]++; break; } }
        }
    }
    if (!g_fps || lr != kFloorLr || ns <= 0) return ns;
    // The game asks for (14 - elapsed_ms) ms; keep (floor - elapsed_ms) instead, never negative.
    int64_t out = ns - (int64_t)((14.0 - g_floor_ms) * 1e6);
    if (out < 0) out = 0;
    g_sl_calls++; g_sl_in_ns += (uint64_t)ns; g_sl_out_ns += (uint64_t)out; if (!out) g_sl_zero++;
    return out;
}
