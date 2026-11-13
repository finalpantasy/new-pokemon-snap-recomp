// See perf.h. Output files (SNAP_PERF=<base>): <base>_frames.csv, <base>_threads.csv, <base>_events.csv.
#include "perf.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "os.h"

namespace snap_perf {

std::atomic<uint64_t> g_ns[C_COUNT], g_calls[C_COUNT], g_val[V_COUNT];
thread_local Ledger tl_ledger;
thread_local uint8_t tl_role = R_OTHER;
std::atomic<uint64_t> g_role_ns[R_COUNT][WT_COUNT], g_fs_bytes;
uint64_t g_wait_event_ns = 8000000;
static const char* kWtName[WT_COUNT] = {"core", "lock", "cvar", "key", "multi", "sleep", "fs"};
static uint64_t g_prev_role[R_COUNT][WT_COUNT], g_prev_fs_bytes;
static std::mutex g_led_mu;
static std::map<unsigned long, Ledger*> g_ledgers;
void ledger_register() {
    tl_ledger.tid = GetCurrentThreadId();
    std::lock_guard<std::mutex> lk(g_led_mu);
    g_ledgers[tl_ledger.tid] = &tl_ledger;
    tl_ledger.reg = true;
}
Ledger::~Ledger() {
    if (!reg) return;
    std::lock_guard<std::mutex> lk(g_led_mu);
    g_ledgers.erase(tid);
}
static FILE* g_waits;

static const char* kCtrName[C_COUNT] = {"replay", "qsubmit", "pace", "acquire", "acqsem", "present", "waitsync", "finish", "pipe",
                                        "shader", "poolsync", "upload", "hash", "desc", "getpipe", "readback", "scratch", "reap",
                                        "ensureimg", "view", "rendbegin"};
static const char* kValName[V_COUNT] = {"draws", "submits", "cmds", "poolsync_bytes", "poolsync_calls", "uploads", "upload_bytes",
                                        "hash_bytes", "hash_calls", "pipes_created", "scratch_allocs", "desc_sets", "barriers", "shaders",
                                        "pipes_skipped", "upload_pref", "upload_pref_img",
                                        "pref_miss_none", "pref_miss_fail", "pref_miss_hash", "pref_miss_late",
                                        "pref_miss_drop", "pref_skip_kind", "pref_skip_nosrc", "pref_skip_budget"};

static FILE *g_frames, *g_threads, *g_events;
static std::mutex g_ev_mu;
static uint64_t g_t0, g_prev_ns[C_COUNT], g_prev_val[V_COUNT], g_prev_calls[C_COUNT], g_last_mark;
static OsStats g_prev_os;
static uint64_t g_prev_cycles;
static std::once_flag g_once;

static void sampler() {
    std::map<DWORD, std::pair<HANDLE, uint64_t>> h;   // tid -> handle, last cpu time (100 ns)
    uint64_t last = now_ns();
    for (;;) {
        Sleep(1000);
        const uint64_t now = now_ns();
        const double t = (double)(now - g_t0) / 1e9, wall_ms = (double)(now - last) / 1e6;
        last = now;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te{sizeof te};
        std::map<DWORD, bool> seen;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                seen[te.th32ThreadID] = true;
                auto it = h.find(te.th32ThreadID);
                if (it == h.end()) {
                    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!th) continue;
                    it = h.emplace(te.th32ThreadID, std::make_pair(th, (uint64_t)0)).first;
                }
                FILETIME c, e, k, u;
                if (!GetThreadTimes(it->second.first, &c, &e, &k, &u)) continue;
                const uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
                const double ms = (double)(cpu - it->second.second) / 1e4;
                it->second.second = cpu;
                if (ms < 2.0) continue;
                wchar_t* d = nullptr;
                using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
                static Fn gd = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
                char nm[64] = "";
                if (gd && SUCCEEDED(gd(it->second.first, &d)) && d) { snprintf(nm, sizeof nm, "%ls", d); LocalFree(d); }
                std::lock_guard<std::mutex> lk(g_ev_mu);
                fprintf(g_threads, "%.2f,%lu,%s,%.1f,%.1f\n", t, te.th32ThreadID, nm[0] ? nm : "-", ms, wall_ms);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        for (auto it = h.begin(); it != h.end();) {
            if (!seen.count(it->first)) { CloseHandle(it->second.first); it = h.erase(it); } else ++it;
        }
        if (g_waits) {   // per-thread blocked time over the last second (threads that waited 1 ms or more)
            static std::map<unsigned long, std::array<uint64_t, WT_COUNT>> prev;
            std::lock_guard<std::mutex> lk(g_led_mu);
            for (auto& [tid, l] : g_ledgers) {
                auto& pv = prev[tid];
                uint64_t d[WT_COUNT], sum = 0;
                for (int i = 0; i < WT_COUNT; i++) {
                    const uint64_t v = l->ns[i].load(std::memory_order_relaxed);
                    d[i] = v - pv[i];
                    pv[i] = v;
                    sum += d[i];
                }
                if (sum < 1000000) continue;
                static std::map<unsigned long, std::string> names;
                auto nit = names.find(tid);
                if (nit == names.end()) {
                    char nm[64] = "-";
                    using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
                    static Fn gd = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
                    if (HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
                        wchar_t* dsc = nullptr;
                        if (gd && SUCCEEDED(gd(th, &dsc)) && dsc) {
                            if (*dsc) snprintf(nm, sizeof nm, "%ls", dsc);
                            LocalFree(dsc);
                        }
                        CloseHandle(th);
                    }
                    for (char* c = nm; *c; c++) if (*c == ',') *c = ';';
                    nit = names.emplace(tid, nm).first;
                }
                fprintf(g_waits, "%.2f,%lu,%s", t, tid, nit->second.c_str());
                for (int i = 0; i < WT_COUNT; i++) fprintf(g_waits, ",%.1f", (double)d[i] / 1e6);
                fprintf(g_waits, "\n");
            }
            fflush(g_waits);
        }
        std::lock_guard<std::mutex> lk(g_ev_mu);
        fflush(g_threads);
    }
}

static void init() {
    const char* base = std::getenv("SNAP_PERF");
    if (!base || !*base) base = std::getenv("SNAP_PERF_SHARED");   // non-exclusive A/B runs (see perf.h)
    if (!base || !*base) return;
    if (const char* w = std::getenv("SNAP_PERF_WAIT_MS")) g_wait_event_ns = (uint64_t)(atof(w) * 1e6);
    std::string b = base;
    g_frames = fopen((b + "_frames.csv").c_str(), "w");
    g_threads = fopen((b + "_threads.csv").c_str(), "w");
    g_events = fopen((b + "_events.csv").c_str(), "w");
    if (!g_frames || !g_threads || !g_events) return;
    g_t0 = g_last_mark = now_ns();
    fprintf(g_frames, "frame,t_s,dt_ms,tid,cpu_mcycles,gpu_busy_ms,gpu_span_ms,gpu_cbs");
    for (int i = 0; i < C_COUNT; i++) fprintf(g_frames, ",%s_ms,%s_n", kCtrName[i], kCtrName[i]);
    for (int i = 0; i < V_COUNT; i++) fprintf(g_frames, ",%s", kValName[i]);
    fprintf(g_frames, ",os_preempt_req,os_preempt_yield,os_overflow,os_core_waits,os_spin_yields,os_lock_contended,os_cv_waits,os_fiber_sw");
    static const char* kRole[R_COUNT] = {"oth", "gt", "mt"};
    for (int r : {1, 2, 0})
        for (int i = 0; i < WT_COUNT; i++) fprintf(g_frames, ",%s_%s_ms", kRole[r], kWtName[i]);
    fprintf(g_frames, ",fs_bytes\n");
    fprintf(g_threads, "t_s,tid,name,cpu_ms,wall_ms\n");
    fprintf(g_events, "t_s,kind,ms,detail\n");
    g_waits = fopen((b + "_waits.csv").c_str(), "w");
    if (g_waits) {
        fprintf(g_waits, "t_s,tid,name");
        for (int i = 0; i < WT_COUNT; i++) fprintf(g_waits, ",%s_ms", kWtName[i]);
        fprintf(g_waits, "\n");
    }
    os_get_stats(&g_prev_os);
    std::thread(sampler).detach();
}

void frame_mark(uint64_t present_no, double gpu_busy_ms, double gpu_span_ms, int gpu_cbs) {
    if (!on) return;
    std::call_once(g_once, init);
    if (!g_frames) return;
    if (tl_role == R_OTHER) tl_role = R_PRESENT;
    const uint64_t now = now_ns();
    ULONG64 cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    std::lock_guard<std::mutex> lk(g_ev_mu);
    fprintf(g_frames, "%llu,%.4f,%.3f,%lu,%.3f,%.3f,%.3f,%d", (unsigned long long)present_no, (double)(now - g_t0) / 1e9, (double)(now - g_last_mark) / 1e6,
            GetCurrentThreadId(), (double)(cyc - g_prev_cycles) / 1e6, gpu_busy_ms, gpu_span_ms, gpu_cbs);
    g_prev_cycles = cyc;
    g_last_mark = now;
    for (int i = 0; i < C_COUNT; i++) {
        const uint64_t ns = g_ns[i].load(std::memory_order_relaxed), n = g_calls[i].load(std::memory_order_relaxed);
        fprintf(g_frames, ",%.3f,%llu", (double)(ns - g_prev_ns[i]) / 1e6, (unsigned long long)(n - g_prev_calls[i]));
        g_prev_ns[i] = ns; g_prev_calls[i] = n;
    }
    for (int i = 0; i < V_COUNT; i++) {
        const uint64_t v = g_val[i].load(std::memory_order_relaxed);
        fprintf(g_frames, ",%llu", (unsigned long long)(v - g_prev_val[i]));
        g_prev_val[i] = v;
    }
    OsStats o;
    os_get_stats(&o);
    fprintf(g_frames, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu", (unsigned long long)(o.preempt_requests - g_prev_os.preempt_requests),
            (unsigned long long)(o.preempt_yields - g_prev_os.preempt_yields), (unsigned long long)(o.overflow_grants - g_prev_os.overflow_grants),
            (unsigned long long)(o.core_waits - g_prev_os.core_waits), (unsigned long long)(o.spin_yields - g_prev_os.spin_yields),
            (unsigned long long)(o.lock_contended - g_prev_os.lock_contended), (unsigned long long)(o.cv_waits - g_prev_os.cv_waits),
            (unsigned long long)(o.fiber_switches - g_prev_os.fiber_switches));
    g_prev_os = o;
    for (int r : {1, 2, 0})
        for (int i = 0; i < WT_COUNT; i++) {
            const uint64_t v = g_role_ns[r][i].load(std::memory_order_relaxed);
            fprintf(g_frames, ",%.3f", (double)(v - g_prev_role[r][i]) / 1e6);
            g_prev_role[r][i] = v;
        }
    const uint64_t fb = g_fs_bytes.load(std::memory_order_relaxed);
    fprintf(g_frames, ",%llu\n", (unsigned long long)(fb - g_prev_fs_bytes));
    g_prev_fs_bytes = fb;
    if (present_no % 120 == 0) { fflush(g_frames); fflush(g_events); }
}

void event(const char* kind, double ms, const char* detail) {
    if (!on) return;
    std::call_once(g_once, init);
    if (!g_events) return;
    std::lock_guard<std::mutex> lk(g_ev_mu);
    fprintf(g_events, "%.3f,%s,%.3f,%s\n", (double)(now_ns() - g_t0) / 1e9, kind, ms, detail ? detail : "");
}

}  // namespace snap_perf
