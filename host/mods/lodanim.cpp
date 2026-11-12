// Draw-distance mods #26 (full-rate distant animation) and #27 (LOD bias, draw distance): dynamic tracer and knobs (project code, ours).
//
// Tracer (env SNAP_TRACE_LOD=1): the lifted code calls fnprof_hit on every function entry when g_fnprof is set (runtime/cpu.h FN_ENTER),
// and host/loader.cpp forwards it to snap_fn_observe below. Three tools, all off unless asked:
//   * parity profile: counts every main function's entries on even and odd rendered frames. A function that runs every frame for
//     near objects but every other frame for far ones shows an even/odd imbalance (the 30 Hz distant-animation signature).
//     SNAP_TRACE_LOD_FRAMEFN (default main+0x988fe0, the per-frame pass loop), SNAP_TRACE_LOD_FROM (first frame, default 300),
//     SNAP_TRACE_LOD_FRAMES (window, default 600), SNAP_TRACE_LOD_OUT (default analysis/mods/fn_parity.tsv).
//   * watch list: SNAP_TRACE_LOD_FN=0xaddr[,..] (module offset or absolute) logs the first hits of each function with x0-x7, the low
//     float of v0-v3 and (SNAP_TRACE_LOD_MEM=bytes) a float/hex dump of the memory x0 points at.
//   * SNAP_TRACE_LOD_CALLERS=0xaddr[,..] counts the callers (lr) of those functions over the window.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"

using namespace loader;

extern "C" volatile int g_fnprof;

namespace {

struct Watch { uint64_t addr; int hits; };

const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kMainSpan = 0xa40000ull;   // lifted .text is 0xa38e40 bytes
bool g_on;
uint64_t g_frame_fn, g_frame;
uint64_t g_from = 300, g_win = 600;
bool g_dumped;
std::vector<uint32_t> g_even, g_odd;
Watch g_watch[16];
int g_nwatch;
Watch g_callers[8];
int g_ncallers;
struct CallerKey { uint64_t fn, lr; uint32_t n; };
std::vector<CallerKey> g_caller_counts;
int g_memdump;
Watch g_seq[12];
int g_nseq;
uint8_t g_seq_cnt[12][160];
void fov_mark(bool end);
std::string g_out = "analysis/mods/fn_parity.tsv";
double g_t_from, g_t_to;

double now_s() {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (double)n.QuadPart / (double)f.QuadPart;
}

uint64_t norm(uint64_t a) { return a < 0x1000000000ull ? kMainBase + a : a; }

void parse_list(const char *e, Watch *w, int *n, int max) {
    while (e && *e && *n < max) {
        char *end;
        w[(*n)++] = Watch{norm(strtoull(e, &end, 0)), 0};
        e = *end == ',' ? end + 1 : nullptr;
    }
}

bool readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

void dump_parity() {
    FILE *f = fopen(g_out.c_str(), "wb");
    if (!f) { trace("lodanim: cannot write %s", g_out.c_str()); return; }
    double secs = g_t_to - g_t_from;
    fprintf(f, "# frames %llu..%llu, %.2f s (%.1f frames/s); columns: module offset, even-frame entries, odd-frame entries, entries per frame\n",
            (unsigned long long)g_from, (unsigned long long)(g_from + g_win), secs, secs > 0 ? g_win / secs : 0.0);
    for (size_t i = 0; i < g_even.size(); i++) {
        uint32_t e = g_even[i], o = g_odd[i];
        if (e + o < 20) continue;
        fprintf(f, "%#llx\t%u\t%u\t%.2f\n", (unsigned long long)(i * 4), e, o, (double)(e + o) / (double)g_win);
    }
    fclose(f);
    for (int i = 0; i < g_nseq; i++) {
        char line[400];
        int n = 0;
        for (int k = 0; k < 160; k++) n += snprintf(line + n, sizeof line - n, "%d", g_seq_cnt[i][k] > 9 ? 9 : g_seq_cnt[i][k]);
        trace("lodanim: seq %#llx per frame from %llu: %s", (unsigned long long)(g_seq[i].addr - kMainBase), (unsigned long long)g_from, line);
    }
    for (int i = 0; i < g_ncallers; i++)
        for (auto &c : g_caller_counts)
            if (c.fn == g_callers[i].addr) trace("lodanim: caller of %#llx: %#llx x%u", (unsigned long long)(c.fn - kMainBase), (unsigned long long)(c.lr - kMainBase), c.n);
    trace("lodanim: parity profile written to %s (%.1f frames/s over the window)", g_out.c_str(), secs > 0 ? g_win / secs : 0.0);
}

void log_watch(Watch &w, uint64_t lr, uint64_t x0) {
    Cpu *c = g_tls_cpu;
    if (!c) { trace("lodanim: fn %#llx hit (frame %llu) lr=%#llx x0=%#llx (no cpu)", (unsigned long long)(w.addr - kMainBase), (unsigned long long)g_frame, (unsigned long long)(lr - kMainBase), (unsigned long long)x0); return; }
    float s[4];
    for (int k = 0; k < 4; k++) { uint32_t b = (uint32_t)c->v[k]; memcpy(&s[k], &b, 4); }
    trace("lodanim: fn %#llx frame %llu lr=%#llx x0-3=%#llx %#llx %#llx %#llx x4-7=%#llx %#llx %#llx %#llx s0-3=%g %g %g %g",
          (unsigned long long)(w.addr - kMainBase), (unsigned long long)g_frame, (unsigned long long)(lr - kMainBase),
          (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
          (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7],
          s[0], s[1], s[2], s[3]);
    if (g_memdump && x0 > 0x10000 && readable((void *)x0, (size_t)g_memdump)) {
        char line[1024];
        int n = 0;
        const uint32_t *p = (const uint32_t *)x0;
        for (int i = 0; i < g_memdump / 4 && n < 1000; i++) {
            float fv;
            memcpy(&fv, &p[i], 4);
            n += snprintf(line + n, sizeof line - n, "%s+%x:%08x(%g)", i % 8 ? " " : "\n   ", i * 4, p[i], fv);
        }
        trace("lodanim:   [x0]%s", line);
    }
}

struct Init {
    Init() {
        const char *e = getenv("SNAP_TRACE_LOD");
        if (!e || e[0] == '0') return;
        g_on = true;
        g_frame_fn = norm(getenv("SNAP_TRACE_LOD_FRAMEFN") ? strtoull(getenv("SNAP_TRACE_LOD_FRAMEFN"), nullptr, 0) : 0x988fe0);
        if (getenv("SNAP_TRACE_LOD_FROM")) g_from = strtoull(getenv("SNAP_TRACE_LOD_FROM"), nullptr, 0);
        if (getenv("SNAP_TRACE_LOD_FRAMES")) g_win = strtoull(getenv("SNAP_TRACE_LOD_FRAMES"), nullptr, 0);
        if (getenv("SNAP_TRACE_LOD_OUT")) g_out = getenv("SNAP_TRACE_LOD_OUT");
        if (getenv("SNAP_TRACE_LOD_MEM")) g_memdump = atoi(getenv("SNAP_TRACE_LOD_MEM"));
        parse_list(getenv("SNAP_TRACE_LOD_FN"), g_watch, &g_nwatch, 16);
        parse_list(getenv("SNAP_TRACE_LOD_SEQ"), g_seq, &g_nseq, 12);
        parse_list(getenv("SNAP_TRACE_LOD_CALLERS"), g_callers, &g_ncallers, 8);
        g_even.assign(kMainSpan / 4, 0);
        g_odd.assign(kMainSpan / 4, 0);
        g_caller_counts.reserve(4000);
        g_fnprof = 1;
        fprintf(stderr, "lodanim: SNAP_TRACE_LOD on, frame marker main+%#llx, window frames %llu..%llu, %d watched, %d callers\n",
                (unsigned long long)(g_frame_fn - kMainBase), (unsigned long long)g_from, (unsigned long long)(g_from + g_win), g_nwatch, g_ncallers);
    }
} g_init;

}  // namespace

extern "C" void snap_fn_observe(uint64_t a, uint64_t lr, uint64_t x0) {
    if (!g_on) return;
    if (a == g_frame_fn) {
        g_frame++;
        if (g_frame == g_from) { g_t_from = now_s(); fov_mark(false); }
        if (g_frame == g_from + g_win && !g_dumped) { g_dumped = true; g_t_to = now_s(); fov_mark(true); dump_parity(); }
    }
    uint64_t off = a - kMainBase;
    bool in_win = g_frame >= g_from && g_frame < g_from + g_win;
    if (in_win && off < kMainSpan) {
        if (g_frame & 1) g_odd[off >> 2]++; else g_even[off >> 2]++;
    }
    if (g_nseq && g_frame >= g_from && g_frame < g_from + 160)
        for (int i = 0; i < g_nseq; i++)
            if (g_seq[i].addr == a && g_seq_cnt[i][g_frame - g_from] < 255) g_seq_cnt[i][g_frame - g_from]++;
    for (int i = 0; i < g_nwatch; i++)
        if (g_watch[i].addr == a && g_frame >= g_from && g_watch[i].hits < 400) { g_watch[i].hits++; log_watch(g_watch[i], lr, x0); }
    for (int i = 0; i < g_ncallers; i++)
        if (g_callers[i].addr == a && in_win) {
            bool found = false;
            for (auto &c : g_caller_counts)
                if (c.fn == a && c.lr == lr) { c.n++; found = true; break; }
            if (!found && g_caller_counts.size() < 4000) g_caller_counts.push_back({a, lr, 1});
        }
}

// ---------------------------------------------------------------------------------------------------- field of view (SNAP_FOV)
// The perspective builders in main compute 1/tanf(fov/2) through the libc import tanf (PLT main+0xa36be0, 42 call sites, 24 of
// them camera code). The host override below sees the caller (lr). SNAP_FOV scales the vertical field-of-view angle for the call
// sites in g_fov_sites only (filled from the trace); every other tanf call is exact. SNAP_TRACE_FOV=1 logs the distinct callers.
namespace {

double g_fov_scale = 1.0;      // multiplier of the angle
double g_fov_target_deg = 0;   // >0: absolute vertical FOV in degrees
bool g_fov_trace;
struct FovSite { uint64_t lr; uint64_t n; float last; };
FovSite g_fov_sites_seen[96];
int g_fov_nseen;
std::vector<uint64_t> g_fov_allow;   // module offsets of return addresses that get the FOV scale

uint64_t g_fov_snap[96];

void h_tanf(Cpu *c);
}  // namespace
namespace {
void fov_mark_impl(bool end) {
    for (int i = 0; i < g_fov_nseen; i++) {
        if (!end) g_fov_snap[i] = g_fov_sites_seen[i].n;
        else if (g_fov_sites_seen[i].n > g_fov_snap[i])
            trace("fov: window caller main+%#llx hits=%llu last fov %.1f deg", (unsigned long long)(g_fov_sites_seen[i].lr - kMainBase),
                  (unsigned long long)(g_fov_sites_seen[i].n - g_fov_snap[i]), g_fov_sites_seen[i].last * 114.5916);
    }
}
}  // namespace
namespace {
void h_tanf(Cpu *c) {
    uint32_t b = (uint32_t)c->v[0];
    float x;
    memcpy(&x, &b, 4);
    uint64_t lr = c->x[30];
    float xr = x;
    if (g_fov_scale != 1.0 || g_fov_target_deg > 0) {
        uint64_t off = lr - kMainBase;
        bool ok = false;
        for (uint64_t a : g_fov_allow) if (a == off) { ok = true; break; }
        if (ok && x > 0.02f && x < 1.55f) {   // half-angle in radians
            xr = g_fov_target_deg > 0 ? (float)(g_fov_target_deg * 3.14159265358979 / 360.0) : (float)(x * g_fov_scale);
            if (xr > 1.5f) xr = 1.5f;
            static int applied;
            if (++applied == 1 || applied == 1000) trace("fov: SNAP_FOV applied at main+%#llx: half-angle %.2f -> %.2f deg (%d)", (unsigned long long)off, x * 57.29578, xr * 57.29578, applied);
        }
    }
    if (g_fov_trace) {
        int i = 0;
        for (; i < g_fov_nseen; i++) if (g_fov_sites_seen[i].lr == lr) break;
        if (i == g_fov_nseen && i < 96) { g_fov_sites_seen[i] = {lr, 0, x}; g_fov_nseen++; }
        if (i < 96) {
            FovSite &s = g_fov_sites_seen[i];
            s.n++;
            s.last = x;
            if (s.n == 1 || s.n == 60 || s.n == 600 || s.n == 3000 || s.n == 12000)
                trace("fov: tanf caller main+%#llx x=%g rad (half-angle %.2f deg) hits=%llu", (unsigned long long)(lr - kMainBase), x, x * 57.29578, (unsigned long long)s.n);
        }
    }
    float r = tanf(xr);
    uint32_t rb;
    memcpy(&rb, &r, 4);
    c->v[0] = (__uint128_t)rb;
}

}  // namespace

namespace { void fov_mark(bool end) { fov_mark_impl(end); } }
namespace loader {
void snap_lodanim_install() {
    const char *e = getenv("SNAP_FOV");
    if (e && *e) {
        double v = atof(e);
        if (v >= 10.0) g_fov_target_deg = v; else if (v > 0.1) g_fov_scale = v;
    }
    g_fov_trace = getenv("SNAP_TRACE_FOV") && getenv("SNAP_TRACE_FOV")[0] != '0';
    if (const char *s = getenv("SNAP_FOV_SITES")) {   // override the allow list: comma separated module offsets
        g_fov_allow.clear();
        while (s && *s) { char *end; g_fov_allow.push_back(strtoull(s, &end, 0)); s = *end == ',' ? end + 1 : nullptr; }
    }
    if (g_fov_trace || g_fov_scale != 1.0 || g_fov_target_deg > 0) add_override("tanf", h_tanf);
}
}  // namespace loader
