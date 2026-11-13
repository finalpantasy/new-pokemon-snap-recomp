// Gameplay mods #14 (NEO-ONE speed control, SNAP_NEO_SPEED) and #22 (more photos per run, SNAP_FILM) (project code, ours),
// plus a small dev tracer (SNAP_NEO_DUMP, SNAP_NEO_WATCH) used to find the rail code. Everything is off unless its env var is set.
#include <windows.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "loader.h"

using namespace loader;

extern "C" volatile int g_fnprof;
extern "C" uint64_t snap_present_count();   // host/boot.cpp

namespace {

const uint64_t kMainBase = 0x7100000000ull;

// ---------------------------------------------------------------------------------------------------- dev tracer
// SNAP_NEO_DUMP=<module offset>,<bytes>[;..]   logs the floats at main+off once a second.
// SNAP_NEO_WATCH=<module offset>[,..]          hardware write watchpoints (DR0..3, 4 bytes) on every thread; a VEH counts the rip of each write
//                                              (rip - exe base, resolve with the linker map) and prints the table every 2 s.
struct Dump { uint64_t addr; int bytes; bool deref; };
std::vector<Dump> g_dumps;
std::vector<uint64_t> g_watch;
std::mutex g_mu;
std::map<uint64_t, uint64_t> g_hits;   // rip rva -> count
uint64_t g_modbase;
uint64_t g_camfn;                    // SNAP_NEO_CAMFN=<module offset>: function whose x0 is the object to inspect
volatile uint64_t g_camobj;
int g_camdump = 0x200;               // SNAP_NEO_CAMDUMP=<bytes>
std::vector<int> g_camoffs;          // SNAP_NEO_WATCH_OBJ=<off>[,..]: hardware-watch obj+off (else auto: offsets that change, first 4)
std::vector<float> g_prev;
bool g_autodone;
uint64_t g_watchbase;
int g_auto_t = 110;                  // SNAP_NEO_AUTO_T: second at which the auto watch picks

LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t rip = ep->ContextRecord->Rip;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_hits[((uint64_t)(dr6 & 0xf) << 56) | (rip - g_modbase)]++;
    }
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_threads() {
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
                DWORD64 dr7 = 0;
                DWORD64 *dr[4] = {&cx.Dr0, &cx.Dr1, &cx.Dr2, &cx.Dr3};
                for (size_t i = 0; i < g_watch.size() && i < 4; i++) {
                    *dr[i] = g_watch[i];
                    dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (3ull << (18 + i * 4));   // enable, write, 4 bytes
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

void tracer_thread() {
    g_modbase = (uint64_t)GetModuleHandleA(nullptr);
    if (!g_watch.empty()) AddVectoredExceptionHandler(1, veh);
    for (int sec = 0;; sec++) {
        Sleep(1000);
        if (!g_watch.empty() && (sec % 3) == 0) arm_threads();
        for (const Dump &d : g_dumps) {
            uint64_t base = d.addr;
            if (d.deref) { if (IsBadReadPtr((void *)d.addr, 8)) continue; base = *(const uint64_t *)d.addr; }
            const float *f = (const float *)base;
            if (IsBadReadPtr(f, d.bytes)) continue;
            char line[1100];
            int n = snprintf(line, sizeof line, "[neo-dump] t=%d main+%#llx base=%#llx:", sec, (unsigned long long)(d.addr - kMainBase), (unsigned long long)base);
            for (int i = 0; i < d.bytes / 4 && n < 1000; i++) n += snprintf(line + n, sizeof line - n, " %g", f[i]);
            fprintf(stderr, "%s\n", line);
        }
        if (g_camfn && g_camobj && !IsBadReadPtr((void *)g_camobj, g_camdump)) {
            const float *f = (const float *)g_camobj;
            int nf = g_camdump / 4;
            char line[2400];
            int n = snprintf(line, sizeof line, "[neo-cam] t=%d obj=%#llx:", sec, (unsigned long long)g_camobj);
            for (int i = 0; i < nf && n < 2300; i++) n += snprintf(line + n, sizeof line - n, " %g", f[i]);
            fprintf(stderr, "%s\n", line);
            if (!g_camoffs.empty() && sec >= g_auto_t && g_watchbase != g_camobj) {   // explicit offsets: follow the object when it changes
                static bool veh_added;
                g_watchbase = g_camobj;
                g_watch.clear();
                for (int o : g_camoffs) { fprintf(stderr, "[neo-cam] watch obj+%#x (obj %#llx)\n", o, (unsigned long long)g_camobj); g_watch.push_back(g_camobj + o); }
                if (!veh_added) { veh_added = true; AddVectoredExceptionHandler(1, veh); }
                arm_threads();
            }
            if (!g_autodone && g_camoffs.empty() && g_watch.empty() && !g_prev.empty() && sec >= g_auto_t) {
                // auto: floats that changed since the last second and stayed finite and small; keep the first four, one per 3-float group
                std::vector<int> pick;
                for (int i = 0; i + 2 < nf && pick.size() < 4; i++) {
                    bool all = true;
                    for (int k = 0; k < 3; k++) {
                        float a = f[i + k], b = g_prev[i + k];
                        if (!(a != b && fabsf(a) < 1e6f && fabsf(a) > 1e-3f && fabsf(a - b) < 1e3f)) all = false;
                    }
                    if (all) { pick.push_back(i * 4); i += 2; }
                }
                if (!pick.empty()) {
                    g_autodone = true;
                    for (int o : pick) { fprintf(stderr, "[neo-cam] auto watch obj+%#x\n", o); g_watch.push_back(g_camobj + o); }
                    AddVectoredExceptionHandler(1, veh);
                    arm_threads();
                }
            }
            g_prev.assign(f, f + nf);
        }
        if (!g_watch.empty() && (sec % 2) == 1) {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto &kv : g_hits) fprintf(stderr, "[neo-watch] dr=%llx exe+%#llx hits=%llu\n", (unsigned long long)(kv.first >> 56), (unsigned long long)(kv.first & 0xffffffffffffffull), (unsigned long long)kv.second);
        }
        fflush(stderr);
    }
}

// ---------------------------------------------------------------------------------------------------- #22 SNAP_FILM
// The play HUD object keeps the photo counter at [this+0x2b8] (int, 72 on most courses). Its live per-frame update is main+0x393c90, which
// decrements it per shot (`ldr w8,[x26,#0x2b8]; subs w10,w8,#1; ... str w10,[x26,#0x2b8]`, main+0x393d98..0x393f84) and ends the expedition at
// 0. (Module offsets are real vaddrs; scripts/elf_xref.py `func` prints addresses shifted by 0x1000, use knobs_lib.dis.) The initial value is
// written elsewhere (main+0x37fa48 and 0x3807fc looked right but never ran), so rt_gm_film_c, hooked at main+0x393d9c, overwrites the counter
// with SNAP_FILM once per HUD object, the first time it still reads 72.
int g_film;           // 0 = off
bool g_film_log;

void film_apply(uint64_t this_, const char *site, bool only72) {
    static bool inited;
    if (!inited) {
        inited = true;
        if (const char *e = getenv("SNAP_FILM")) { int v = atoi(e); if (v >= 1) g_film = v > 999 ? 999 : v; }
        g_film_log = getenv("SNAP_FILM_LOG") && getenv("SNAP_FILM_LOG")[0] != '0';
    }
    if (!this_ || IsBadReadPtr((void *)(this_ + 0x2b8), 4)) return;
    int *p = (int *)(this_ + 0x2b8);
    if (g_film_log) fprintf(stderr, "[film] %s hud=%#llx counter %d -> %d\n", site, (unsigned long long)this_, *p, g_film ? g_film : *p);
    if (g_film && (!only72 || *p == 72)) *p = g_film;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- #14 SNAP_NEO_SPEED
// Found 2026-11-12 (dev tracer, florio_photos): the render camera (builder 0x295170, eye at obj+0xd0) is placed by the camera controller
// update 0x38b710, which samples the NEO-ONE rail object R = *(controller+0x68) through 0x4bf670: R+0x30 = the rail curve (its length is
// curve+0x3c), R+0x70 = progress along it (a float, about 0.9 per second on Florio), the position is curve(R+0x70 / length). The progress
// is advanced once per frame by the rail update 0x4c0090: `R+0x70 += dt * speed * slope factor` at main+0x4c2de8..0x4c2df0
// (`ldr s1,[x19,#0x70]; fadd s1,s12,s1; str s1,[x19,#0x70]`, x19 = R). MAIN_INLINE_HOOKS runs rt_gm_neo_speed right after that store. It
// remembers the progress it left last time per rail object, takes this frame's increment (new - last) and stores last + increment * speed.
// Nothing else is touched: the animation clock, Pokemon scripts and the camera's own timers keep real time, so only the ride covers
// ground faster or slower (events keyed to rail position fire earlier or later in real time; photo scoring reads positions, not time).
namespace {
double g_neo = 1.0;
bool g_neo_log;
struct RailState { uint64_t obj; float last; bool have; };
RailState g_rails[8];
int g_nrails;
bool g_neo_inited;

void neo_init() {
    g_neo_inited = true;
    if (const char *e = getenv("SNAP_NEO_SPEED")) { double v = atof(e); if (v >= 0.5 && v <= 3.0 && v != 1.0) g_neo = v; else if (v > 3.0) g_neo = 3.0; else if (v > 0.0 && v < 0.5) g_neo = 0.5; }
    g_neo_log = getenv("SNAP_NEO_LOG") && getenv("SNAP_NEO_LOG")[0] != '0';
    if (g_neo != 1.0) fprintf(stderr, "[neo] SNAP_NEO_SPEED=%g: rail progress increments scaled\n", g_neo);
}

}  // namespace

namespace hle::input { bool route_on(); void route_rail(uint64_t obj, float prog); void route_film(int count); }   // runtime/hle/input_core.cpp (route "wait" signals)

extern "C" void rt_gm_neo_speed(Cpu *c) {
    if (hle::input::route_on() && c->x[19] && !IsBadReadPtr((void *)(c->x[19] + 0x70), 4)) hle::input::route_rail(c->x[19], *(float *)(c->x[19] + 0x70));
    if (!g_neo_inited) neo_init();
    if (g_neo == 1.0 && !g_neo_log) return;
    uint64_t obj = c->x[19];
    if (!obj || IsBadReadPtr((void *)(obj + 0x70), 4)) return;
    float *pp = (float *)(obj + 0x70);
    RailState *rs = nullptr;
    for (int i = 0; i < g_nrails; i++) if (g_rails[i].obj == obj) { rs = &g_rails[i]; break; }
    if (!rs) {
        if (g_nrails >= 8) return;
        rs = &g_rails[g_nrails++];
        rs->obj = obj; rs->have = false;
        if (g_neo_log) fprintf(stderr, "[neo] rail object %#llx (progress %g)\n", (unsigned long long)obj, *pp);
    }
    float cur = *pp;
    if (rs->have && cur > rs->last && cur - rs->last < 5.0f && g_neo != 1.0) {
        float scaled = rs->last + (cur - rs->last) * (float)g_neo;
        *pp = scaled;
        cur = scaled;
    }
    if (g_neo_log) {
        static int n;
        if ((++n % 120) == 1) fprintf(stderr, "[neo] present=%llu rail %#llx progress %g\n", (unsigned long long)snap_present_count(), (unsigned long long)obj, cur);
    }
    rs->last = cur;
    rs->have = true;
}

namespace {
}  // namespace

extern "C" void snap_neo_observe(uint64_t a, uint64_t lr, uint64_t x0) {
    (void)lr;
    if (g_camfn && a == g_camfn) g_camobj = x0;
}
// Live HUD update main+0x393c90, hooked before `subs w10, w8, #1` at main+0x393d9c (x26 = HUD, w8 = counter).
extern "C" void rt_gm_film_c(Cpu *c) {
    static uint64_t addr[8];
    static int last[8];
    static int nd;
    uint64_t hud = c->x[26];
    if (!hud || IsBadReadPtr((void *)(hud + 0x2b8), 4)) return;
    hle::input::route_film(*(int *)(hud + 0x2b8));
    int i = 0;
    while (i < nd && addr[i] != hud) i++;
    if (i == nd) { if (nd == 8) i = 7; else { addr[nd] = hud; last[nd] = -1; nd++; } }
    // A new expedition reads 72 after any value but 73 (a normal shot 73 -> 72 is not a refill), so the boost works again on every
    // expedition even when the allocator reuses the HUD address.
    if (*(int *)(hud + 0x2b8) == 72 && last[i] != 73 && last[i] != 72) film_apply(hud, "hud", true);
    last[i] = *(int *)(hud + 0x2b8);
}

namespace loader {
void snap_neofilm_install() {
    if (const char *d = getenv("SNAP_NEO_DUMP")) {
        std::string s = d;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(';', p);
            if (e == std::string::npos) e = s.size();
            std::string item = s.substr(p, e - p);
            size_t c = item.find(',');
            if (c != std::string::npos) g_dumps.push_back({kMainBase + strtoull(item.c_str(), nullptr, 0), atoi(item.c_str() + c + 1), item.find(",d", c + 1) != std::string::npos});
            p = e + 1;
        }
    }
    if (const char *w = getenv("SNAP_NEO_WATCH")) {
        const char *s = w;
        while (s && *s) { char *end; g_watch.push_back(kMainBase + strtoull(s, &end, 0)); s = *end == ',' ? end + 1 : nullptr; }
    }
    if (const char *cf = getenv("SNAP_NEO_CAMFN")) { g_camfn = kMainBase + strtoull(cf, nullptr, 0);  }
    if (const char *cd = getenv("SNAP_NEO_CAMDUMP")) g_camdump = atoi(cd) & ~3;
    if (const char *wo = getenv("SNAP_NEO_WATCH_OBJ")) {
        const char *s = wo;
        while (s && *s) { char *end; g_camoffs.push_back((int)strtoul(s, &end, 0)); s = *end == ',' ? end + 1 : nullptr; }
    }
    if (const char *at = getenv("SNAP_NEO_AUTO_T")) g_auto_t = atoi(at);
    if (g_camfn) g_fnprof = 1;
    if (!g_dumps.empty() || !g_watch.empty() || g_camfn) std::thread(tracer_thread).detach();
}
}  // namespace loader
