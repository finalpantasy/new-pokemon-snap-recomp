// Sampling CPU profiler (off unless SNAP_SAMPLE is set; 2026-11-13, CPU profile worker).
//
//   SNAP_SAMPLE=1 | <base path>    on; output <base>.prof (default analysis\perf\sample_<pid>.prof, relative to the working dir)
//   SNAP_SAMPLE_US=<us>             sampling period (default 1000)
//   SNAP_SAMPLE_WRITE_S=<s>         rewrite the output every s seconds (default 20; a run killed by PID keeps the last copy)
//   SNAP_SAMPLE_MIN_KCYC=<k>        sample a thread only when it used at least k thousand cycles since its last sample (default 100)
//
// One host thread wakes every period, and for every thread of the process that did enough work since its last sample
// (QueryThreadCycleTime delta >= SNAP_SAMPLE_MIN_KCYC) it suspends the thread, reads its context, copies the top 16 KiB of its stack into a static buffer and resumes it.
// Nothing between SuspendThread and ResumeThread touches the heap or any user-mode lock (GetThreadContext, VirtualQuery, memcpy
// only), so a thread suspended while it holds the heap lock or an SRW lock cannot deadlock the sampler (same rule as the stall
// watchdog in host\boot.cpp). The stack copy is unwound after the resume with RtlVirtualUnwind (no heap, no dbghelp), giving up to
// kDepth host frames; lifted guest functions are host functions (f_<guest address>), so the host stack is the guest call chain.
// Stacks are counted in a fixed table (allocated once), keyed by thread, present-count bucket (presents / 500) and frames.
// The file holds raw absolute addresses plus the module list; analysis\perf\cpu_profile.py symbolizes them with the linker map
// (snap_boot.map) and the guest ELF symbols, and prints flat and caller views per thread.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

namespace {

constexpr int kDepth = 8;                  // frames kept per sample (leaf first)
constexpr int kMaxThreads = 512;
constexpr size_t kStackCopy = 16 * 1024;
constexpr uint32_t kTableBits = 19;        // 512K stacks, ~44 MB
constexpr uint32_t kTableSize = 1u << kTableBits;
constexpr int kBucketPresents = 500;

struct Entry {
    uint64_t pc[kDepth];
    uint32_t tid;
    uint16_t bucket;
    uint8_t depth, flags;   // flags bit0: leaf rip sits right after a syscall instruction (thread blocked in the kernel)
    uint64_t count;
    uint64_t cyc;           // thread cycles since its previous sample, summed (CPU-weighted view)
};
struct Thr {
    DWORD tid;
    HANDLE h;
    uint64_t last_cyc, cyc, n_samples, n_sys;
    bool alive;
    char name[64];
};

Entry *g_tab;
uint64_t g_used, g_dropped, g_ticks, g_self_cyc, g_suspend_fail;
Thr g_thr[kMaxThreads];
int g_nthr;
alignas(16) uint8_t g_stack[kStackCopy];
const int *g_frame;          // present counter (nvn::g_presentCount), may be null
char g_path[MAX_PATH];
unsigned g_period_us = 1000, g_write_s = 20;
uint64_t g_min_cyc = 100000;   // SNAP_SAMPLE_MIN_KCYC (thousands of cycles of work since the last sample; ~30 us at 3.5 GHz)
HANDLE g_flush_req, g_flush_done;
DWORD g_self_tid;
std::atomic<bool> g_on{false};

uint64_t hash_entry(const Entry &e) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ ((uint64_t)e.tid << 20) ^ ((uint64_t)e.bucket << 8) ^ e.flags;
    for (int i = 0; i < e.depth; i++) { h ^= e.pc[i]; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; }
    return h;
}
void add(const Entry &e, uint64_t cyc) {
    uint64_t i = hash_entry(e) & (kTableSize - 1);
    for (uint32_t probe = 0; probe < 64; probe++, i = (i + 1) & (kTableSize - 1)) {
        Entry &s = g_tab[i];
        if (s.count == 0) {
            s = e;
            s.count = 1;
            s.cyc = cyc;
            g_used++;
            return;
        }
        if (s.tid == e.tid && s.bucket == e.bucket && s.flags == e.flags && s.depth == e.depth &&
            memcmp(s.pc, e.pc, sizeof(uint64_t) * e.depth) == 0) {
            s.count++;
            s.cyc += cyc;
            return;
        }
    }
    g_dropped++;
}

// Unwind a context whose stack lo..lo+n was copied to g_stack (see raw_unwind in host\boot.cpp). Every integer register that points
// into the copied range is rebased before each step, since a function's frame register need not be rbp. Any value read outside
// the copy is from the live stack (the thread runs again) and can be garbage, hence the SEH guard in unwind().
void rebase_all(CONTEXT &ctx, uint64_t lo, uint64_t hi, uint64_t slo) {
    DWORD64 *r = &ctx.Rax;   // Rax..R15 are contiguous in CONTEXT
    for (int i = 0; i < 16; i++) if (r[i] >= lo && r[i] < hi) r[i] = slo + (r[i] - lo);
}
void unwind_steps(CONTEXT &ctx, uint64_t lo, size_t n, uint64_t *pcs, int *depth) {
    const uint64_t hi = lo + n, slo = (uintptr_t)g_stack, shi = slo + n;
    rebase_all(ctx, lo, hi, slo);
    while (*depth < kDepth && ctx.Rip) {
        pcs[(*depth)++] = ctx.Rip;
        if (ctx.Rsp < slo || ctx.Rsp + 8 > shi) break;
        DWORD64 img = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &img, nullptr);
        if (!fe) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; }
        else {
            void *hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, ctx.Rip, fe, &ctx, &hd, &est, nullptr);
        }
        rebase_all(ctx, lo, hi, slo);
        if (ctx.Rsp < slo || ctx.Rsp > shi) break;
    }
}
uint64_t g_unwind_faults;
int unwind(CONTEXT ctx, uint64_t lo, size_t n, uint64_t *pcs) {
    int d = 0;
    __try {
        unwind_steps(ctx, lo, n, pcs, &d);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_unwind_faults++;
    }
    return d;
}

// Thread list: NtGetNextThread walks this process's threads only. The first version used a Toolhelp snapshot, which copies every
// thread of the whole system; on the shared PC that took ~250 ms per refresh and most of the sampler's CPU.
using NtGetNextThreadFn = LONG(NTAPI *)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
void name_thread(Thr &t) {
    using GetDesc = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static GetDesc gd = (GetDesc)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    wchar_t *d = nullptr;
    if (gd && SUCCEEDED(gd(t.h, &d)) && d) {
        if (*d) {
            snprintf(t.name, sizeof t.name, "%ls", d);
            for (char *c = t.name; *c; c++) if (*c == ' ' || *c == '\t') *c = '_';
        }
        LocalFree(d);
    }
}
void refresh_threads() {
    static NtGetNextThreadFn next_thread = (NtGetNextThreadFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread");
    static int pass;
    pass++;
    for (int i = 0; i < g_nthr; i++) {   // exited threads keep their rows (their samples) and drop the handle
        Thr &t = g_thr[i];
        if (!t.h) continue;
        if (WaitForSingleObject(t.h, 0) == WAIT_OBJECT_0) { CloseHandle(t.h); t.h = nullptr; continue; }
        if (!t.name[0] || (pass & 7) == 0) name_thread(t);   // guest threads name themselves right after they start
    }
    if (!next_thread) return;
    const ACCESS_MASK acc = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE;
    HANDLE cur = nullptr;
    bool keep_cur = false;
    for (;;) {
        HANDLE nx = nullptr;
        const LONG st = next_thread(GetCurrentProcess(), cur, acc, 0, 0, &nx);
        if (cur && !keep_cur) CloseHandle(cur);
        if (st != 0 || !nx) break;
        cur = nx;
        keep_cur = false;
        const DWORD tid = GetThreadId(nx);
        if (tid == g_self_tid || tid == 0) continue;
        bool known = false;
        for (int i = 0; i < g_nthr && !known; i++) known = g_thr[i].tid == tid && g_thr[i].h;
        if (known || g_nthr >= kMaxThreads) continue;
        Thr &t = g_thr[g_nthr++];
        memset(&t, 0, sizeof t);
        t.tid = tid;
        t.h = nx;
        keep_cur = true;
        QueryThreadCycleTime(nx, &t.last_cyc);
        name_thread(t);
    }
}

uint64_t g_t_qtc, g_t_susp, g_t_unwind, g_t_refresh, g_t_write, g_n_sampled;   // sampler cost by phase (rdtsc), in the header
void sample_one(Thr &t, uint16_t bucket) {
    uint64_t cyc = 0;
    const uint64_t r0 = __rdtsc();
    if (!QueryThreadCycleTime(t.h, &cyc)) return;
    const uint64_t dc = cyc - t.last_cyc;
    g_t_qtc += __rdtsc() - r0;
    // Below g_min_cyc the thread only woke briefly (or only ran our own suspend APC, which is charged to the target thread: with a
    // plain "ran since the last tick" test every blocked thread got sampled every tick and the sampler took 70% of a core).
    if (dc < g_min_cyc) return;
    g_n_sampled++;
    const uint64_t r1 = __rdtsc();
    t.last_cyc = cyc;
    t.cyc += dc;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(t.h) == (DWORD)-1) { g_suspend_fail++; return; }
    // ---- suspended: no heap, no user-mode locks below until ResumeThread
    size_t n = 0;
    const bool got = GetThreadContext(t.h, &ctx) != FALSE;
    if (got) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)(uintptr_t)ctx.Rsp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)) {
            const uint64_t end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            n = (size_t)(end - ctx.Rsp < kStackCopy ? end - ctx.Rsp : kStackCopy);
            memcpy(g_stack, (void *)(uintptr_t)ctx.Rsp, n);
        }
    }
    ResumeThread(t.h);
    // ---- resumed
    QueryThreadCycleTime(t.h, &t.last_cyc);   // after the resume: the suspend/resume APC cost is not counted as the thread's work
    const uint64_t r2 = __rdtsc();
    g_t_susp += r2 - r1;
    if (!got) return;
    Entry e;
    memset(&e, 0, sizeof e);
    e.tid = t.tid;
    e.bucket = bucket;
    if (n) e.depth = (uint8_t)unwind(ctx, ctx.Rsp, n, e.pc);
    else { e.pc[0] = ctx.Rip; e.depth = 1; }
    const uint8_t *ip = (const uint8_t *)(uintptr_t)ctx.Rip;
    if (ctx.Rip > 0x10000 && !IsBadReadPtr(ip - 2, 2) && ip[-2] == 0x0f && ip[-1] == 0x05) e.flags |= 1;
    t.n_samples++;
    if (e.flags & 1) t.n_sys++;
    add(e, dc);
    g_t_unwind += __rdtsc() - r2;
}

void write_out() {
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# snap sampler v2\nmin_cycles %llu\nunwind_faults %llu\n", (unsigned long long)g_min_cyc, (unsigned long long)g_unwind_faults);
    fprintf(f, "cost_mcyc_qtc %llu\ncost_mcyc_suspend %llu\ncost_mcyc_unwind %llu\ncost_mcyc_refresh %llu\ncost_mcyc_write %llu\nsampled %llu\n",
            (unsigned long long)(g_t_qtc / 1000000), (unsigned long long)(g_t_susp / 1000000), (unsigned long long)(g_t_unwind / 1000000),
            (unsigned long long)(g_t_refresh / 1000000), (unsigned long long)(g_t_write / 1000000), (unsigned long long)g_n_sampled);
    fprintf(f, "period_us %u\nticks %llu\nstacks %llu\ndropped %llu\nsuspend_fail %llu\nself_cycles %llu\nframe %d\nbucket_presents %d\n",
            g_period_us, (unsigned long long)g_ticks, (unsigned long long)g_used, (unsigned long long)g_dropped,
            (unsigned long long)g_suspend_fail, (unsigned long long)g_self_cyc, g_frame ? *g_frame : -1, kBucketPresents);
    HMODULE mods[1024];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
            MODULEINFO mi{};
            char nm[MAX_PATH] = "?";
            GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
            GetModuleBaseNameA(GetCurrentProcess(), mods[i], nm, sizeof nm);
            for (char *c = nm; *c; c++) if (*c == ' ') *c = '_';
            fprintf(f, "module %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, (unsigned long)mi.SizeOfImage, nm);
        }
    }
    for (int i = 0; i < g_nthr; i++) {
        const Thr &t = g_thr[i];
        if (!t.n_samples) continue;
        fprintf(f, "thread %lu %s %llu %llu %llu\n", t.tid, t.name[0] ? t.name : "-", (unsigned long long)t.cyc,
                (unsigned long long)t.n_samples, (unsigned long long)t.n_sys);
    }
    for (uint32_t i = 0; i < kTableSize; i++) {
        const Entry &e = g_tab[i];
        if (!e.count) continue;
        fprintf(f, "s %lu %u %u %llu %llu", e.tid, e.bucket, e.flags, (unsigned long long)e.count, (unsigned long long)e.cyc);
        for (int k = 0; k < e.depth; k++) fprintf(f, " %llx", (unsigned long long)e.pc[k]);
        fputc('\n', f);
    }
    fclose(f);
    MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING);
}

DWORD WINAPI sampler_main(void *) {
    g_self_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    if (auto sd = (SetDesc)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")) sd(GetCurrentThread(), L"snap:sampler");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)g_period_us * 10;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);   // re-armed each tick (one-shot): a late tick does not queue more
    ULONGLONG last_refresh = 0, last_write = GetTickCount64();
    HANDLE self = GetCurrentThread();
    for (;;) {
        HANDLE hs[2] = {timer, g_flush_req};
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, 1000);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        const ULONGLONG now = GetTickCount64();
        if (w == WAIT_OBJECT_0 + 1 || now - last_write >= g_write_s * 1000ull) {
            const uint64_t w0 = __rdtsc();
            write_out();
            g_t_write += __rdtsc() - w0;
            last_write = now;
            if (w == WAIT_OBJECT_0 + 1) { SetEvent(g_flush_done); continue; }
        }
        uint64_t c0 = 0, c1 = 0;
        QueryThreadCycleTime(self, &c0);
        if (now - last_refresh >= 500) { const uint64_t f0 = __rdtsc(); refresh_threads(); g_t_refresh += __rdtsc() - f0; last_refresh = now; }
        const int fr = g_frame ? *(volatile const int *)g_frame : 0;
        const uint16_t bucket = (uint16_t)(fr < 0 ? 0 : (fr / kBucketPresents > 65535 ? 65535 : fr / kBucketPresents));
        for (int i = 0; i < g_nthr; i++) if (g_thr[i].h) sample_one(g_thr[i], bucket);
        g_ticks++;
        QueryThreadCycleTime(self, &c1);
        g_self_cyc += c1 - c0;
    }
}

}  // namespace

// Called once from cmd_run (host\boot.cpp). `frame` = the present counter used for the bucket key.
void snap_sampler_start(const int *frame) {
    const char *v = getenv("SNAP_SAMPLE");
    if (!v || !*v || !strcmp(v, "0")) return;
    if (!strcmp(v, "1")) snprintf(g_path, sizeof g_path, "analysis\\perf\\sample_%lu.prof", GetCurrentProcessId());
    else snprintf(g_path, sizeof g_path, "%s.prof", v);
    if (const char *p = getenv("SNAP_SAMPLE_US")) { int u = atoi(p); g_period_us = u < 100 ? 100 : (u > 100000 ? 100000 : (unsigned)u); }
    if (const char *p = getenv("SNAP_SAMPLE_WRITE_S")) { int s = atoi(p); g_write_s = s < 2 ? 2 : (unsigned)s; }
    if (const char *p = getenv("SNAP_SAMPLE_MIN_KCYC")) g_min_cyc = (uint64_t)strtoull(p, nullptr, 10) * 1000;
    g_tab = (Entry *)VirtualAlloc(nullptr, sizeof(Entry) * kTableSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_tab) return;
    g_frame = frame;
    g_flush_req = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_flush_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE h = CreateThread(nullptr, 256 * 1024, sampler_main, nullptr, 0, nullptr)) { CloseHandle(h); g_on = true; }
    fprintf(stderr, "sampler: on, period %u us, output %s\n", g_period_us, g_path);
}

// Final write before the process ends (the --timeout path in host\boot.cpp calls this before TerminateProcess). Waits at most 5 s.
void snap_sampler_flush() {
    if (!g_on) return;
    SetEvent(g_flush_req);
    WaitForSingleObject(g_flush_done, 5000);
}
