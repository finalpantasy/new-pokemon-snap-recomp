// Skip cutscenes / dialogue (#18) and fast evaluation (#17): discovery recorder (project code, ours).
//
// SNAP_SKIPS_REC=<from>,<len>,<out>  records, for presents [from, from+len), every main function that is entered in few distinct
// presents (at most 40), with the presents it was entered in and x0 of the first entry. Used to find the code that handles one
// button press or one state (a function entered only on the presents around a scripted press). Off unless set.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "loader.h"

using namespace loader;

extern "C" volatile int g_fnprof;
extern "C" uint64_t snap_present_count();   // host/boot.cpp
namespace hle::input { extern std::atomic<uint64_t> g_inject_buttons; extern std::atomic<int64_t> g_inject_until_ns; }   // runtime/hle/input_core.cpp

namespace {

const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kMainSpan = 0xa40000ull;

bool g_rec;
// #18 dialogue fast-forward: main+0x676630 is a per-present UI predicate that the cutscene / skit dialogue window runs once per present while it
// waits for the A press (about once per 3 presents while the text is still typing). A streak of consecutive presents with a call = waiting.
const uint64_t kWaitFn = kMainBase + 0x676630;
bool g_skip_dlg, g_skip_log;
int g_skip_delay = 6;            // presents the window must have been active before the host presses A
std::atomic<uint64_t> g_last_fr{~0ull};
std::atomic<uint64_t> g_next_fire{0};
std::atomic<uint64_t> g_fired{0};
uint64_t g_from, g_len, g_bucket = 50;
std::string g_out;
bool g_dumped;
std::unordered_map<uint32_t, std::vector<uint16_t>> g_hist;   // function index -> entries per bucket of presents

void dump() {
    FILE* f = fopen(g_out.c_str(), "w");
    if (!f) return;
    size_t nb = (size_t)((g_len + g_bucket - 1) / g_bucket);
    for (auto& kv : g_hist) {
        uint64_t tot = 0;
        for (uint16_t v : kv.second) tot += v;
        if (tot > 20000) continue;
        fprintf(f, "0x%x", kv.first << 2);
        for (size_t i = 0; i < nb; i++) fprintf(f, "\t%u", (unsigned)kv.second[i]);
        fprintf(f, "\n");
    }
    fclose(f);
    trace("skips: recorder wrote %s (%zu functions seen, bucket %llu presents)", g_out.c_str(), g_hist.size(), (unsigned long long)g_bucket);
}

}  // namespace

static void skip_wait(uint64_t x0) {
    {        uint64_t fr = snap_present_count();
        uint64_t last = g_last_fr.exchange(fr);
        if (fr != last) {
            // the window is active while the predicate runs at least every ~8 presents (every present while waiting, every 3rd while the text types)
            if (last == ~0ull || fr - last > 8) g_next_fire = fr + (uint64_t)g_skip_delay;
            if (fr >= g_next_fire) {   // press A for about 3 presents: finishes the typing, or moves to the next line; again every 12 presents
                using namespace hle::input;
                g_next_fire = fr + 12;
                g_inject_buttons = 1ull << 0;
                g_inject_until_ns = std::chrono::steady_clock::now().time_since_epoch().count() + 100000000ll;
                uint64_t n = ++g_fired;
                if (g_skip_log && n <= 400) {
                    uint64_t v0 = 0, o1 = 0, v1 = 0;
                    if (x0 && !IsBadReadPtr((void*)x0, 16)) { v0 = *(uint64_t*)x0; o1 = *(uint64_t*)(x0 + 8); if (o1 && !IsBadReadPtr((void*)o1, 8)) v1 = *(uint64_t*)o1; }
                    trace("skips: dialogue window active at present %llu (object %#llx vtable main+%#llx, [+8] %#llx vtable main+%#llx): A", (unsigned long long)fr,
                          (unsigned long long)x0, (unsigned long long)(v0 - kMainBase), (unsigned long long)o1, (unsigned long long)(v1 - kMainBase));
                }
            }
        }
    }
}

// Inline hook (recomp/snap_gen.py MAIN_INLINE_HOOKS, before the first instruction of main+0x676630): x0 = the dialogue window object.
namespace hle::input { void route_dialog(); }   // runtime/hle/input_core.cpp (route "wait dialog")

extern "C" void rt_gm_skip_wait(Cpu* c) {
    hle::input::route_dialog();
    if (g_skip_dlg) skip_wait(c->x[0]);
}

extern "C" void snap_skips_observe(uint64_t a, uint64_t lr, uint64_t x0) {
    (void)lr;
    (void)x0;
    if (!g_rec) return;
    uint64_t fr = snap_present_count();
    if (fr < g_from) return;
    static std::mutex mu;   // lifted code runs on several guest threads
    std::lock_guard<std::mutex> lk(mu);
    if (fr >= g_from + g_len) {
        if (!g_dumped) { g_dumped = true; dump(); }
        return;
    }
    uint64_t off = a - kMainBase;
    if (off >= kMainSpan) return;
    auto& v = g_hist[(uint32_t)(off >> 2)];
    size_t nb = (size_t)((g_len + g_bucket - 1) / g_bucket);
    if (v.empty()) v.assign(nb, 0);
    uint16_t& c = v[(size_t)((fr - g_from) / g_bucket)];
    if (c < 0xffff) c++;
}

namespace loader {
void snap_skips_install() {
    const char* sc = getenv("SNAP_SKIP_CUTSCENES");
    if (sc && *sc && *sc != '0') {
        g_skip_dlg = true;   // needs no function profile: the inline hook rt_gm_skip_wait does the watching
        g_skip_log = getenv("SNAP_SKIPS_LOG") && *getenv("SNAP_SKIPS_LOG") != '0';
        if (const char* d = getenv("SNAP_SKIP_DELAY")) { int v = atoi(d); if (v >= 2 && v <= 600) g_skip_delay = v; }
    }
    const char* e = getenv("SNAP_SKIPS_REC");
    if (e && *e) {
        char out[260] = "analysis/mods/skips_rec.tsv";
        unsigned long long from = 0, len = 0;
        const char* c1 = strchr(e, ',');
        if (c1) {
            from = strtoull(e, nullptr, 0);
            len = strtoull(c1 + 1, nullptr, 0);
            const char* c2 = strchr(c1 + 1, ',');
            if (c2) { snprintf(out, sizeof out, "%s", c2 + 1); if (char* c3 = strchr(out, ',')) *c3 = 0; }
        }
        if (len) {
            g_from = from;
            g_len = len;
            g_out = out;
            if (const char* c2 = strchr(strchr(e, ',') + 1, ',')) {
                const char* c3 = strchr(c2 + 1, ',');
                if (c3) g_bucket = strtoull(c3 + 1, nullptr, 0);
                if (g_bucket < 1) g_bucket = 50;
            }
            g_rec = true;
            g_fnprof = 1;
        }
    }
}
}  // namespace loader
