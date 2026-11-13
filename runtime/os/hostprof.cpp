// Host sampling profiler (load pass, 2026-11-13; debug only, off unless SNAP_HOST_PROF=<output file> is set).
// A host thread suspends every other thread of the process every SNAP_HOST_PROF_MS (default 2) ms, reads its RIP and resumes it.
// Samples between SNAP_HOST_PROF_FROM and SNAP_HOST_PROF_TO seconds (process time; default 0 and 1e9) go into a preallocated
// array (no allocation while a thread is suspended: it may hold the heap lock). Every 10 s and at the window end the file is
// rewritten as "<exe offset hex> <count>" lines (RIPs outside the exe as "ext <module offset> <count>" under a module line).
// Map offsets to lifted functions with analysis/perf/hostprof_report.py and build/<dir>/snap_boot.map.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Sample { uint64_t rip; uint32_t tid; };

struct HostProf {
    std::string path;
    double from = 0, to = 1e9;
    DWORD period_ms = 2;
    Sample *buf = nullptr;
    size_t cap = 0, n = 0;
    LARGE_INTEGER f0{}, freq{};

    double now() const {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return (double)(t.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
    }

    void dump() {
        std::unordered_map<uint64_t, uint32_t> cnt;
        for (size_t i = 0; i < n; i++) cnt[buf[i].rip]++;
        HMODULE exe = GetModuleHandleW(nullptr);
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof mi);
        const uint64_t lo = (uint64_t)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;
        std::map<std::string, uint64_t> ext;
        std::string tmp = path + ".tmp";
        FILE *f = fopen(tmp.c_str(), "w");
        if (!f) return;
        fprintf(f, "# samples %zu window %.1f-%.1f s period %lu ms\n", n, from, to, period_ms);
        for (auto &kv : cnt) {
            if (kv.first >= lo && kv.first < hi) { fprintf(f, "%llx %u\n", (unsigned long long)(kv.first - lo), kv.second); continue; }
            HMODULE m = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)kv.first, &m) && m)
                GetModuleBaseNameA(GetCurrentProcess(), m, name, sizeof name);
            ext[name] += kv.second;
        }
        for (auto &kv : ext) fprintf(f, "ext %s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
        fclose(f);
        MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void run() {
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        std::unordered_map<DWORD, HANDLE> threads;
        double next_scan = 0, next_dump = 10;
        bool final_done = false;
        for (;;) {
            Sleep(period_ms);
            const double t = now();
            if (t < from) continue;
            if (t > to) {
                if (!final_done) { dump(); final_done = true; }
                if (t > to + 1) return;
                continue;
            }
            if (t >= next_scan) {   // refresh the thread list twice a second
                next_scan = t + 0.5;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snap != INVALID_HANDLE_VALUE) {
                    THREADENTRY32 te{sizeof te};
                    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && !threads.count(te.th32ThreadID))
                            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))
                                threads[te.th32ThreadID] = h;
                    CloseHandle(snap);
                }
                for (auto it = threads.begin(); it != threads.end();) {   // drop exited threads
                    DWORD code = 0;
                    if (!GetExitCodeThread(it->second, &code) || code != STILL_ACTIVE) { CloseHandle(it->second); it = threads.erase(it); }
                    else ++it;
                }
            }
            for (auto &kv : threads) {
                if (n >= cap) break;
                if (SuspendThread(kv.second) == (DWORD)-1) continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(kv.second, &c)) buf[n++] = Sample{c.Rip, kv.first};
                ResumeThread(kv.second);
            }
            if (t >= next_dump) { next_dump = t + 10; dump(); }
        }
    }
};

HostProf *g_prof;

DWORD WINAPI prof_main(LPVOID) {
    g_prof->run();
    return 0;
}

struct Starter {
    Starter() {
        const char *p = getenv("SNAP_HOST_PROF");
        if (!p || !*p) return;
        g_prof = new HostProf;
        g_prof->path = p;
        if (const char *e = getenv("SNAP_HOST_PROF_FROM")) g_prof->from = atof(e);
        if (const char *e = getenv("SNAP_HOST_PROF_TO")) g_prof->to = atof(e);
        if (const char *e = getenv("SNAP_HOST_PROF_MS")) g_prof->period_ms = (DWORD)std::max(1, atoi(e));
        g_prof->cap = 8u << 20;   // 8 M samples (128 MB, demand-zero pages touched as used)
        g_prof->buf = (Sample *)VirtualAlloc(nullptr, g_prof->cap * sizeof(Sample), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_prof->buf) return;
        QueryPerformanceFrequency(&g_prof->freq);
        QueryPerformanceCounter(&g_prof->f0);
        if (HANDLE h = CreateThread(nullptr, 0, prof_main, nullptr, 0, nullptr)) {
            SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
            CloseHandle(h);
        }
    }
} g_starter;

}  // namespace
