// Host scheduler under the nn::os layer: threads, three virtual cores, keyed wait queues, Horizon lock arbitration
// with priority inheritance, ticks and the SVC helpers. Written for this project; the structure (one global lock G,
// one condition variable per thread, wake order by priority then arrival) follows TotK Recomp runtime/kernel.c
// @ 7a37fc3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh). Design notes: docs/horizon-os.md.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <random>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "os_internal.h"
#include "perf.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using namespace oslay;

namespace osk {

Config g_cfg;
Stats g_stats;
std::mutex G;

static std::unordered_map<uint32_t, Thread *> g_handles;
static std::unordered_map<uint64_t, Thread *> g_by_tt;
static std::unordered_map<uint64_t, std::vector<Thread *>> g_keyq;
static uint32_t g_next_handle = 0x10000;
static uint64_t g_next_id = 0x40;
static uint64_t g_seq = 0;
static std::vector<Thread *> g_cores;       // owner of each virtual core, nullptr = free
static std::vector<Thread *> g_core_wait;   // ready threads waiting for a core
static int g_overflow = 0;
static thread_local Thread *t_self = nullptr;

// ---- logging
void log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[os] %s\n", buf);
    fflush(stderr);
}
void fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[os] FATAL: %s\n", buf);
    fflush(stderr);
    abort();
}

// ---- time
static int64_t qpc_freq() {
    static const int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return (int64_t)x.QuadPart; }();
    return f;
}
static int64_t qpc_start() {
    static const int64_t s = [] { LARGE_INTEGER x; QueryPerformanceCounter(&x); return (int64_t)x.QuadPart; }();
    return s;
}
int64_t tick_to_ns(int64_t tick) {   // TickManager::ConvertToTimeSpan sdk+0x3451e0
    if (tick > 0x0275254609abe000ll) return INT64_MAX;
    if (tick < (int64_t)0xfd8adab9f6542000ull) return INT64_MIN;
    int64_t sec = tick / (int64_t)OS_TICK_FREQUENCY;
    int64_t rem = tick - sec * (int64_t)OS_TICK_FREQUENCY;
    return sec * 1000000000ll + rem * 1000000000ll / (int64_t)OS_TICK_FREQUENCY;
}
int64_t ns_to_tick(int64_t ns) {     // TickManager::ConvertToTick sdk+0x345270 (fraction rounded up)
    if (ns == INT64_MIN) return INT64_MIN;
    int64_t sec = ns / 1000000000ll, rem = ns - sec * 1000000000ll;
    int64_t remabs = ns < 0 ? -rem : rem, secabs = ns < 0 ? -sec : sec;
    int64_t t = secabs * (int64_t)OS_TICK_FREQUENCY + (remabs * (int64_t)OS_TICK_FREQUENCY + 999999999ll) / 1000000000ll;
    return ns < 0 ? -t : t;
}
uint64_t now_ns() { return (uint64_t)tick_to_ns((int64_t)os_ticks()); }
static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
Deadline deadline_after(int64_t ns) {
    if (ns >= NS_INFINITE / 2) return Deadline::max();
    if (ns < 0) ns = 0;
    return std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
}
bool is_infinite(Deadline d) { return d == Deadline::max(); }

static void precise_sleep(int64_t ns) {
    struct Timer { HANDLE h = nullptr; ~Timer() { if (h) CloseHandle(h); } };
    static thread_local Timer tm;
    if (!tm.h) tm.h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while (ns > 0) {
        int64_t step = std::min<int64_t>(ns, 1000000000ll * 3600);   // one hour per wait
        if (tm.h) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(step / 100, 1);
            SetWaitableTimer(tm.h, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(tm.h, INFINITE);
        } else {
            Sleep((DWORD)std::max<int64_t>(step / 1000000, 1));
        }
        ns -= step;
    }
}

// ---- guest-visible allocations
Cpu *alloc_cpu() {
    Cpu *c = (Cpu *)_aligned_malloc(sizeof(Cpu), 64);
    if (!c) fatal("out of memory (Cpu)");
    memset(c, 0, sizeof(Cpu));
    c->excl_addr = ~0ull;
    return c;
}
static uint8_t *new_tlr() {
    uint8_t *p = (uint8_t *)_aligned_malloc(TLR_SIZE, TLR_SIZE);
    if (!p) fatal("out of memory (TLR)");
    memset(p, 0, TLR_SIZE);
    return p;
}
uint64_t alloc_guest(size_t size) {
    void *p = _aligned_malloc(size, 16);
    if (!p) fatal("out of memory");
    memset(p, 0, size);
    return (uint64_t)(uintptr_t)p;
}

void free_cpu(Cpu *c) { _aligned_free(c); }
void free_guest(uint64_t p) { _aligned_free((void *)(uintptr_t)p); }
void set_default_thread_name(uint64_t tt) {   // SetInitialThreadNameUnsafe
    uint64_t entry = gld<uint64_t>(tt + TT_FUNCTION);
    char name[32];
    if (entry) snprintf(name, sizeof name, "Thread_0x%016llx", (unsigned long long)entry);
    else snprintf(name, sizeof name, "MainThread");
    set_thread_type_name(tt, name);
}
std::vector<uint64_t> all_thread_types() {
    std::lock_guard<std::mutex> lk(G);
    std::vector<uint64_t> v;
    v.reserve(g_by_tt.size());
    for (auto &kv : g_by_tt) v.push_back(kv.first);
    return v;
}

static uint32_t alloc_handle() {   // under G; never 0, never the waiters bit, below the pseudo handles
    for (;;) {
        uint32_t h = g_next_handle++ & 0x3fffffffu;
        if (h == 0 || h >= 0x3fff0000u) { g_next_handle = 0x10000; continue; }
        if (!g_handles.count(h)) return h;
    }
}

Thread *find_handle(uint32_t h) {
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}
Thread *find_tt(uint64_t tt) {
    auto it = g_by_tt.find(tt);
    return it == g_by_tt.end() ? nullptr : it->second;
}

// ThreadType fields as ThreadManager::CreateThread writes them (sdk+0x344480).
void init_thread_type(uint64_t tt, uint64_t entry, uint64_t arg, uint64_t stack, uint64_t size, int prio, uint8_t state) {
    memset((void *)(uintptr_t)tt, 0, TT_SIZE);
    gst<uint64_t>(tt + TT_ALL_NODE, tt);
    gst<uint64_t>(tt + TT_ALL_NODE + 8, tt);
    gst<uint64_t>(tt + TT_WAITLIST, tt + TT_WAITLIST);
    gst<uint64_t>(tt + TT_WAITLIST + 8, tt + TT_WAITLIST);
    gst<int16_t>(tt + TT_BASE_PRIORITY, (int16_t)prio);
    gst<int16_t>(tt + TT_VERSION, 1);
    gst<uint64_t>(tt + TT_ORIGINAL_STACK, stack);
    gst<uint64_t>(tt + TT_STACK, stack);
    gst<uint64_t>(tt + TT_STACK_SIZE, size);
    gst<uint64_t>(tt + TT_ARGUMENT, arg);
    gst<uint64_t>(tt + TT_FUNCTION, entry);
    char name[32];
    snprintf(name, sizeof name, "Thread_0x%016llx", (unsigned long long)entry);   // default name, sdk+0x3445c0
    memcpy((void *)(uintptr_t)(tt + TT_NAME_BUFFER), name, strlen(name) + 1);
    gst<uint64_t>(tt + TT_NAME_POINTER, tt + TT_NAME_BUFFER);
    gst<uint8_t>(tt + TT_STATE, state);
}
void set_thread_type_name(uint64_t tt, const char *s) {
    size_t n = strnlen(s, 31);
    memcpy((void *)(uintptr_t)(tt + TT_NAME_BUFFER), s, n);
    gst<uint8_t>(tt + TT_NAME_BUFFER + n, 0);
    gst<uint64_t>(tt + TT_NAME_POINTER, tt + TT_NAME_BUFFER);
}

static void register_thread(Thread *t) {   // under G
    t->handle = alloc_handle();
    t->id = g_next_id++;
    g_handles[t->handle] = t;
    if (t->tt) g_by_tt[t->tt] = t;
    gst<uint32_t>(t->tt + TT_HANDLE, t->handle);
    gst<uint64_t>(t->tt + TT_THREAD_ID, t->id);
}

// ---- self: host helper threads (HLE callbacks, audio, tests) are adopted on first use
namespace {
struct HelperGuard {
    Thread *t = nullptr;
    ~HelperGuard() {
        if (!t) return;
        std::lock_guard<std::mutex> lk(G);
        g_handles.erase(t->handle);
        g_by_tt.erase(t->tt);
        // the object itself is kept: a stale lock word may still name its handle in a log
    }
};
thread_local HelperGuard t_helper;
}  // namespace

static Thread *adopt_helper() {
    Thread *t = new Thread;
    t->is_host = true;
    t->owns_tt = true;
    t->tt = alloc_guest(TT_SIZE);
    t->tlr = new_tlr();
    t->base_prio = t->eff_prio = 16;
    init_thread_type(t->tt, 0, 0, 0, 0, 16, TS_STARTED);
    set_thread_type_name(t->tt, "HostHelper");
    gst<uint64_t>((uint64_t)(uintptr_t)t->tlr + TLR_THREAD, t->tt);
    t->started = 1;
    {
        std::lock_guard<std::mutex> lk(G);
        register_thread(t);
    }
    t_self = t;
    t_helper.t = t;
    return t;
}

Thread *self() {
    Thread *t = t_self;
    if (__builtin_expect(t != nullptr, 1)) return t;
    return adopt_helper();
}
__attribute__((noinline)) Thread *self_fresh() {
    __asm__ volatile("" ::: "memory");
    Thread *t = t_self;
    return t ? t : adopt_helper();
}

uint64_t call_guest(Cpu *c, uint64_t fn, uint64_t a0) {
    c->x[0] = a0;
    c->x[30] = OS_RET_HOST;
    c->pc = fn;
    rt_fpcr_sync(c);
    rt_dispatch(c);
    return c->x[0];
}

// ---- keyed wait queues
static bool better_wait(const Thread *a, const Thread *b) {
    return a->eff_prio < b->eff_prio || (a->eff_prio == b->eff_prio && a->wseq < b->wseq);
}
void keyq_push(uint64_t key, Thread *t) {
    t->wseq = ++g_seq;
    g_keyq[key].push_back(t);
}
bool keyq_remove(uint64_t key, Thread *t) {
    auto it = g_keyq.find(key);
    if (it == g_keyq.end()) return false;
    auto &v = it->second;
    auto p = std::find(v.begin(), v.end(), t);
    if (p == v.end()) return false;
    v.erase(p);
    if (v.empty()) g_keyq.erase(it);
    return true;
}
Thread *keyq_pop_best(uint64_t key) {
    auto it = g_keyq.find(key);
    if (it == g_keyq.end()) return nullptr;
    auto &v = it->second;
    size_t bi = 0;
    for (size_t i = 1; i < v.size(); i++)
        if (better_wait(v[i], v[bi])) bi = i;
    Thread *t = v[bi];
    v.erase(v.begin() + (ptrdiff_t)bi);
    if (v.empty()) g_keyq.erase(it);
    return t;
}
bool keyq_empty(uint64_t key) { return !g_keyq.count(key); }
void wake(Thread *t, uint32_t result) {
    t->waker = t_self;
    t->woken = true;
    t->wresult = result;
    t->wkind = W_NONE;
    t->cv.notify_one();
}
int keyq_wake_all(uint64_t key, uint32_t result) {
    auto it = g_keyq.find(key);
    if (it == g_keyq.end()) return 0;
    std::vector<Thread *> v;
    v.swap(it->second);
    g_keyq.erase(it);
    std::stable_sort(v.begin(), v.end(), better_wait);   // notify in Horizon order
    for (Thread *t : v) wake(t, result);
    return (int)v.size();
}
// ---- perf pass 3: blocked time by cause (SNAP_PERF / SNAP_PERF_SHARED only; see perf.h)
static const char *tname(const Thread *t) {
    if (!t) return "host";
    uint64_t p = t->tt ? gld<uint64_t>(t->tt + TT_NAME_POINTER) : 0;
    return p ? (const char *)(uintptr_t)p : "?";
}
static bool is_frame_thread() { return snap_perf::key_thread(); }   // a presenting JobWorker or the main thread
static const char *role_tag() { return snap_perf::tl_role == snap_perf::R_MAIN ? "mt" : "gt"; }
static void note_wait(Thread *t, int kind, uint64_t ns) {
    snap_perf::Wt w;
    const char *kn;
    switch (kind) {
    case W_LOCK: w = snap_perf::WT_LOCK; kn = "lock"; break;
    case W_CVAR: w = snap_perf::WT_CVAR; kn = "cvar"; break;
    case W_MULTI: w = snap_perf::WT_MULTI; kn = "multi"; break;
    case W_KEY: case W_JOIN: w = snap_perf::WT_KEY; kn = "key"; break;
    default: return;
    }
    snap_perf::wait_add(w, ns);
    if (ns >= snap_perf::g_wait_event_ns && is_frame_thread()) {
        char det[200];
        snprintf(det, sizeof det, "%s addr=%#llx key=%#llx woken_by=%.24s/p%d %s", kn, (unsigned long long)t->waddr,
                 (unsigned long long)t->wkey, tname(t->waker), t->waker ? t->waker->eff_prio : -1, t->woken ? "" : "timeout");
        char kind[16];
        snprintf(kind, sizeof kind, "%s_wait", role_tag());
        snap_perf::event(kind, (double)ns / 1e6, det);
    }
}
static thread_local char t_core_snap[320];   // core holders when this thread started waiting for a core
static void snap_core_holders(Thread *t);

static bool wait_woken_raw(Thread *t, Lock &lk, Deadline d) {
    while (!t->woken) {
        if (is_infinite(d)) {
            t->cv.wait(lk);
        } else if (t->cv.wait_until(lk, d) == std::cv_status::timeout) {
            return t->woken;
        }
    }
    return true;
}
bool wait_woken(Thread *t, Lock &lk, Deadline d) {
    if (!snap_perf::on) return wait_woken_raw(t, lk, d);
    const int kind = t->wkind;
    t->waker = nullptr;
    const uint64_t t0 = snap_perf::now_ns();
    bool r = wait_woken_raw(t, lk, d);
    note_wait(t, kind, snap_perf::now_ns() - t0);
    return r;
}
uint32_t block_on_key(Thread *t, Lock &lk, uint64_t key, int kind, Deadline d) {
    t->woken = false;
    t->wkind = kind;
    t->wkey = key;
    keyq_push(key, t);
    release_core(t);
    bool ok = wait_woken(t, lk, d);
    if (!ok) {
        keyq_remove(key, t);
        t->wkind = W_NONE;
    }
    acquire_core(t, lk);
    return ok ? t->wresult : R_TIMEOUT;
}

// ---- virtual cores
static int ncores() { return g_cfg.num_cores; }
static uint64_t usable(const Thread *t) {
    uint64_t all = (1ull << ncores()) - 1, app = (1ull << g_cfg.app_cores) - 1, m = t->mask & app;
    return (m ? m : app) | (all & ~app);   // spare cores (load pass, SNAP_OS_SPARE_CORES) are open to every thread
}
static bool can_use(const Thread *t, int c) { return (usable(t) >> c) & 1; }
static bool better_ready(const Thread *a, const Thread *b) {
    return a->eff_prio < b->eff_prio || (a->eff_prio == b->eff_prio && a->ready_seq < b->ready_seq);
}
static Thread *best_waiter_for(int c) {
    Thread *best = nullptr;
    for (Thread *w : g_core_wait)
        if (can_use(w, c) && (!best || better_ready(w, best))) best = w;
    return best;
}
static void note_cores_in_use() {
    int n = g_overflow;
    for (Thread *o : g_cores) n += o != nullptr;
    int m = g_stats.max_cores_in_use.load();
    if (n > m) g_stats.max_cores_in_use = n;
}
// Core handoff invariant (under G): for every free core, its best waiter has been notified since the last change to the
// free cores, the wait list, or a waiter's priority or mask. A waiter takes a core only if it is that core's best waiter,
// so a best waiter left asleep blocks the core for every other waiter (intro hang 2026-11-12: three pinned threads slept
// in acquire_core with all cores free). Call this after any such change.
static void kick_free_cores() {
    for (int i = 0; i < ncores(); i++)
        if (!g_cores[i])
            if (Thread *w = best_waiter_for(i)) {
                w->core_kicked = true;
                w->cv.notify_one();
            }
}
static void take_core(Thread *t, int c) {   // t has already left g_core_wait
    t->core = c;
    if (c < ncores()) {
        g_cores[c] = t;
        t->last_core = c;
    }
    t->core_since = now_us();
    t->preempt = 0;
    note_cores_in_use();
    kick_free_cores();   // two cores may have been freed for one best waiter: pass the other one on
}
static void leave_core_wait(Thread *t) {
    g_core_wait.erase(std::find(g_core_wait.begin(), g_core_wait.end(), t));
    t->waiting_core = false;
}

static void acquire_core_raw(Thread *t, Lock &lk) {
    t->ready_seq = ++g_seq;
    t->waiting_core = true;
    g_core_wait.push_back(t);
    uint64_t deadline = 0;
    bool counted = false, rescue_wait = false;
    const uint64_t slice = g_cfg.rr_slice_us, over = g_cfg.overcommit_us;
    for (;;) {
        int cand[8], nc = 0;
        if (t->ideal >= 0 && t->ideal < ncores()) cand[nc++] = t->ideal;
        cand[nc++] = t->last_core;
        int pick = -1;
        for (int i = 0; i < nc && pick < 0; i++) {
            int c = cand[i];
            if (!g_cores[c] && can_use(t, c) && best_waiter_for(c) == t) pick = c;
        }
        for (int c = 0; c < ncores() && pick < 0; c++)
            if (!g_cores[c] && can_use(t, c) && best_waiter_for(c) == t) pick = c;
        if (pick >= 0) {
            if (rescue_wait) {
                g_stats.core_wake_rescues++;
                log("os: lost core wakeup: thread %#x prio %d found core %d free after %u ms", t->handle, t->eff_prio, pick,
                    g_cfg.core_rescue_ms);
            }
            leave_core_wait(t);   // before take_core, so its kick cannot pick t again for another free core
            take_core(t, pick);
            return;
        }
        rescue_wait = false;
        if (!counted) {
            g_stats.core_waits++;
            counted = true;
            if (snap_perf::on) snap_core_holders(t);
        }
        // The thread Horizon would preempt for t: the lowest-priority holder of a core t may use.
        Thread *victim = nullptr;
        for (int c = 0; c < ncores(); c++) {
            Thread *o = g_cores[c];
            if (o && can_use(t, c) &&
                (!victim || o->eff_prio > victim->eff_prio ||
                 (o->eff_prio == victim->eff_prio && o->core_since < victim->core_since)))
                victim = o;
        }
        uint64_t now = now_us();
        int64_t wait_us = -1;
        if (victim) {
            bool outranks = victim->eff_prio > t->eff_prio;
            bool rr = victim->eff_prio == t->eff_prio && t->eff_prio >= 31;   // kernel 59 round robin
            if (outranks || (rr && now - victim->core_since >= slice)) {
                if (!victim->preempt.exchange(1)) g_stats.preempt_requests++;
                if (!deadline) deadline = now + over;
                if (now >= deadline) {   // the holder does not poll: run on an overflow core
                    leave_core_wait(t);
                    take_core(t, ncores());
                    g_overflow++;
                    g_stats.overflow_grants++;
                    note_cores_in_use();
                    if (g_cfg.trace)
                        log("overflow core for thread %#x prio %d (holder %#x prio %d did not yield)", t->handle,
                            t->eff_prio, victim->handle, victim->eff_prio);
                    return;
                }
                wait_us = (int64_t)(deadline - now);
            } else if (rr) {
                wait_us = (int64_t)(victim->core_since + slice - now);
            }
        }
        if (wait_us < 0) {   // nothing to time: wait for a handoff, with a safety-net recheck that counts a missed one
            t->core_kicked = false;
            bool timed_out = t->cv.wait_for(lk, std::chrono::milliseconds(g_cfg.core_rescue_ms)) == std::cv_status::timeout;
            rescue_wait = timed_out && !t->core_kicked;   // a handoff that raced the timeout is not a lost one
        } else {
            t->cv.wait_for(lk, std::chrono::microseconds(wait_us + 50));
        }
    }
}

static void snap_core_holders(Thread *t) {   // under G
    const uint64_t now = now_us();
    int n = snprintf(t_core_snap, sizeof t_core_snap, "self=%.20s/p%d waiters=%zu overflow=%d holders:", tname(t), t->eff_prio,
                     g_core_wait.size(), g_overflow);
    for (int c = 0; c < ncores() && n < (int)sizeof t_core_snap - 48; c++) {
        Thread *o = g_cores[c];
        if (o) n += snprintf(t_core_snap + n, sizeof t_core_snap - n, " %.20s/p%d/%llums", tname(o), o->eff_prio,
                             (unsigned long long)((now - o->core_since) / 1000));
    }
}
void acquire_core(Thread *t, Lock &lk) {
    if (t->is_host || ncores() < 0 || t->core >= 0 || t->block_depth > 0) return;
    if (!snap_perf::on) return acquire_core_raw(t, lk);
    t_core_snap[0] = 0;
    const uint64_t t0 = snap_perf::now_ns();
    acquire_core_raw(t, lk);
    const uint64_t ns = snap_perf::now_ns() - t0;
    snap_perf::wait_add(snap_perf::WT_CORE, ns);
    const bool ft = is_frame_thread();
    if (t_core_snap[0] && ns >= snap_perf::g_wait_event_ns * (ft ? 1 : 4))
        snap_perf::event(!ft ? "core_wait" : snap_perf::tl_role == snap_perf::R_MAIN ? "mt_core_wait" : "gt_core_wait", (double)ns / 1e6, t_core_snap);
}

void release_core(Thread *t) {
    if (t->core < 0) return;
    int c = t->core;
    t->core = -1;
    t->preempt = 0;
    if (c < ncores()) {
        g_cores[c] = nullptr;
        kick_free_cores();
    } else {
        g_overflow--;
    }
}

void poll_preempt(Thread *t) {
    if (__builtin_expect(!t->preempt.load(std::memory_order_relaxed), 1)) return;
    Lock lk(G);
    if (!t->preempt.exchange(0) || t->core < 0) return;
    g_stats.preempt_yields++;
    release_core(t);
    acquire_core(t, lk);
}

// ---- priority inheritance (Horizon: a lock owner runs at the best priority of the threads waiting for its locks)
void recompute_priority(Thread *t) {
    for (int depth = 0; t && depth < 64; depth++) {
        int p = t->base_prio;
        for (Thread *w : t->lock_waiters) p = std::min(p, w->eff_prio);
        if (p == t->eff_prio) break;
        if (p < t->eff_prio) g_stats.pi_boosts++;
        t->eff_prio = p;
        if (t->waiting_core) {   // t re-evaluates its victim; a free core may now have a different best waiter
            t->cv.notify_one();
            kick_free_cores();
        }
        t = t->wkind == W_LOCK ? t->lock_owner : nullptr;
    }
}
static void add_lock_waiter(Thread *owner, Thread *w) {
    w->lock_owner = owner;
    w->wseq = ++g_seq;
    owner->lock_waiters.push_back(w);
    recompute_priority(owner);
}

// Release the lock word at addr owned by me and hand it to the best waiter (KConditionVariable::SignalToAddress).
static void unlock_locked(Thread *me, uint64_t addr) {
    Thread *next = nullptr;
    int n = 0;
    for (Thread *w : me->lock_waiters)
        if (w->waddr == addr) {
            n++;
            if (!next || better_wait(w, next)) next = w;
        }
    if (!next) {
        gword(addr).store(0, std::memory_order_release);
        return;
    }
    auto &lw = me->lock_waiters;
    lw.erase(std::find(lw.begin(), lw.end(), next));
    for (size_t i = 0; i < lw.size();) {
        Thread *w = lw[i];
        if (w->waddr == addr) {
            lw.erase(lw.begin() + (ptrdiff_t)i);
            w->lock_owner = next;
            next->lock_waiters.push_back(w);
        } else {
            i++;
        }
    }
    gword(addr).store(next->wtag | (n > 1 ? LOCK_WAITERS : 0), std::memory_order_release);
    next->lock_owner = nullptr;
    wake(next, R_OK);
    recompute_priority(me);
    recompute_priority(next);
}

uint32_t k_arbitrate_lock(Thread *me, uint32_t owner_handle, uint64_t addr, uint32_t tag) {
    Lock lk(G);
    Thread *owner = find_handle(owner_handle);
    if (!owner) return R_INVALID_HANDLE;
    if (gword(addr).load(std::memory_order_acquire) != (owner_handle | LOCK_WAITERS)) return R_OK;
    if (owner == me) fatal("thread %#x waits for lock %#llx that it owns", me->handle, (unsigned long long)addr);
    g_stats.lock_contended++;
    me->woken = false;
    me->wkind = W_LOCK;
    me->waddr = addr;
    me->wtag = tag;
    add_lock_waiter(owner, me);
    release_core(me);
    wait_woken(me, lk, Deadline::max());
    acquire_core(me, lk);
    return me->wresult;
}

uint32_t k_arbitrate_unlock(Thread *me, uint64_t addr) {
    Lock lk(G);
    if ((gword(addr).load(std::memory_order_acquire) & ~LOCK_WAITERS) != me->handle) return R_INVALID_STATE;
    unlock_locked(me, addr);
    return R_OK;
}

uint32_t k_wait_key_atomic(Thread *me, uint64_t addr, uint64_t key, uint32_t tag, int64_t ns) {
    Lock lk(G);
    Deadline d = deadline_after(ns);
    // Publish the waiter (key word = 1) before the mutex is released: a signaller can take the mutex without G
    // the moment its word turns 0, and it skips the signal when the key word is still 0.
    me->woken = false;
    me->wkind = W_CVAR;
    me->waddr = addr;
    me->wtag = tag;
    me->wkey = mkkey(K_CVAR, key);
    keyq_push(me->wkey, me);
    gword(key).store(1, std::memory_order_seq_cst);
    if ((gword(addr).load(std::memory_order_acquire) & ~LOCK_WAITERS) == me->handle) unlock_locked(me, addr);
    g_stats.cv_waits++;
    release_core(me);
    uint32_t r;
    if (wait_woken(me, lk, d)) {
        r = me->wresult;
    } else if (me->wkind == W_CVAR) {   // timed out while still waiting for the signal
        keyq_remove(me->wkey, me);
        me->wkind = W_NONE;
        r = R_TIMEOUT;
    } else {                            // signalled and moved to the lock's waiters: the lock is not timed
        wait_woken(me, lk, Deadline::max());
        r = me->wresult;
    }
    acquire_core(me, lk);
    return r;
}

void k_signal_key(uint64_t key, int count) {
    Lock lk(G);
    uint64_t k = mkkey(K_CVAR, key);
    for (int n = 0; count <= 0 || n < count; n++) {
        Thread *t = keyq_pop_best(k);
        if (!t) break;
        uint64_t a = t->waddr;
        for (;;) {   // acquire the mutex for t, or queue t on its owner
            uint32_t v = 0;
            if (gword(a).compare_exchange_strong(v, t->wtag, std::memory_order_acq_rel)) {
                wake(t, R_OK);
                break;
            }
            if (!(v & LOCK_WAITERS)) {
                if (!gword(a).compare_exchange_strong(v, v | LOCK_WAITERS, std::memory_order_acq_rel)) continue;
                v |= LOCK_WAITERS;
            }
            Thread *owner = find_handle(v & ~LOCK_WAITERS);
            if (!owner) {
                wake(t, R_INVALID_HANDLE);
                break;
            }
            t->wkind = W_LOCK;
            add_lock_waiter(owner, t);
            break;
        }
    }
    if (keyq_empty(k)) gword(key).store(0, std::memory_order_release);
}

// ---- user-side lock and condition variable over guest words
void cs_init(uint64_t a) { gword(a).store(0, std::memory_order_release); }
void cs_enter(uint64_t a) {
    Thread *me = self();
    const uint32_t cur = me->handle;
    for (;;) {
        uint32_t v = 0;
        if (gword(a).compare_exchange_strong(v, cur, std::memory_order_acquire)) return;
        if ((v & ~LOCK_WAITERS) == cur) fatal("recursive lock of %#llx by thread %#x", (unsigned long long)a, cur);
        if (!(v & LOCK_WAITERS)) {
            if (!gword(a).compare_exchange_strong(v, v | LOCK_WAITERS, std::memory_order_acquire)) continue;
            v |= LOCK_WAITERS;
        }
        uint32_t r = k_arbitrate_lock(me, v & ~LOCK_WAITERS, a, cur);
        if (r == R_INVALID_HANDLE)
            fatal("lock %#llx is owned by unknown thread handle %#x", (unsigned long long)a, v & ~LOCK_WAITERS);
        if ((gword(a).load(std::memory_order_acquire) & ~LOCK_WAITERS) == cur) return;
    }
}
bool cs_try_enter(uint64_t a) {
    uint32_t v = 0;
    return gword(a).compare_exchange_strong(v, self()->handle, std::memory_order_acquire);
}
void cs_leave(uint64_t a) {
    Thread *me = self();
    uint32_t v = me->handle;
    if (gword(a).compare_exchange_strong(v, 0, std::memory_order_release)) return;
    if ((v & ~LOCK_WAITERS) != me->handle)
        fatal("unlock of %#llx by thread %#x, word %#x", (unsigned long long)a, me->handle, v);
    k_arbitrate_unlock(me, a);
}
bool cs_locked_by_me(uint64_t a) {
    return (gword(a).load(std::memory_order_acquire) & ~LOCK_WAITERS) == self()->handle;
}
uint32_t cv_wait(uint64_t cv, uint64_t cs, int64_t ns) {
    Thread *me = self();
    uint32_t r = k_wait_key_atomic(me, cs, cv, me->handle, ns);
    if ((gword(cs).load(std::memory_order_acquire) & ~LOCK_WAITERS) != me->handle) cs_enter(cs);
    return r == R_TIMEOUT ? R_TIMEOUT : R_OK;
}
void cv_signal(uint64_t cv) {
    if (gword(cv).load(std::memory_order_acquire)) k_signal_key(cv, 1);
}
void cv_broadcast(uint64_t cv) {
    if (gword(cv).load(std::memory_order_acquire)) k_signal_key(cv, -1);
}

// ---- threads
Thread *thread_create(uint64_t tt, uint64_t entry, uint64_t arg, int prio, int ideal) {
    Thread *t = new Thread;
    t->tt = tt;
    t->entry = entry;
    t->arg = arg;
    t->cpu = alloc_cpu();
    t->cur = t->cpu;
    t->tlr = new_tlr();
    t->base_prio = t->eff_prio = prio;
    if (ideal < 0) ideal = g_cfg.default_core;   // -2: the process default core (main.npdm)
    t->ideal = ideal;
    t->mask = 1ull << ideal;                      // svcCreateThread pins to the ideal core until SetThreadCoreMask
    t->last_core = ideal < std::max(ncores(), 1) ? ideal : 0;
    std::lock_guard<std::mutex> lk(G);
    if (Thread *old = find_tt(tt))
        if (g_cfg.trace) log("ThreadType %#llx re-created without DestroyThread (old handle %#x)", (unsigned long long)tt, old->handle);
    register_thread(t);
    return t;
}

static void set_host_name(uint64_t tt) {
    using Fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static Fn fn = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (!fn) return;
    uint64_t p = gld<uint64_t>(tt + TT_NAME_POINTER);
    wchar_t w[40] = L"guest:";
    if (p) {
        const char *s = (const char *)(uintptr_t)p;
        size_t i = 6;
        for (size_t j = 0; j < 31 && s[j] && i < 39; j++) w[i++] = (wchar_t)(unsigned char)s[j];
        w[i] = 0;
    }
    fn(GetCurrentThread(), w);
}

static unsigned __stdcall host_entry(void *arg) {
    Thread *t = (Thread *)arg;
    t_self = t;
    set_host_name(t->tt);
    Cpu *c = t->cpu;
    uint64_t tlr = (uint64_t)(uintptr_t)t->tlr;
    c->thread = t;
    c->tpidrro = tlr;
    gst<uint64_t>(tlr + TLR_THREAD, t->tt);   // sdk+0x348e8c
    uint64_t stack = gld<uint64_t>(t->tt + TT_STACK), size = gld<uint64_t>(t->tt + TT_STACK_SIZE);
    c->sp = (stack + size - 0x20) & ~15ull;    // the sdk passes stack top - 0x20 to svcCreateThread (sdk+0x348cf4)
    {
        Lock lk(G);
        acquire_core(t, lk);
    }
    g_stats.threads_started++;
    if (g_cfg.trace) log("thread %#x start entry %#llx prio %d", t->handle, (unsigned long long)t->entry, t->base_prio);
    if (g_cfg.tp_init_var)
        if (uint64_t fn = gld<uint64_t>(g_cfg.tp_init_var)) call_guest(c, fn, t->tt);   // sdk+0x348ea0
    c->tpidr = gld<uint64_t>(tlr + TLR_TP);                                              // sdk+0x348eb4
    call_guest(c, t->entry, t->arg);                                                     // InvokeThread sdk+0x344700
    tls_run_destructors(t);                                                              // sdk+0x34471c
    if (g_cfg.tp_destroy_var)
        if (uint64_t fn = gld<uint64_t>(g_cfg.tp_destroy_var)) call_guest(c, fn, t->tt); // sdk+0x348ed4
    fiber_thread_exit(t);
    {
        Lock lk(G);
        gst8a(t->tt + TT_STATE, TS_TERMINATED);   // sdk+0x34473c
        t->exited = 1;
        keyq_wake_all(mkkey(K_JOIN, t->tt), R_OK);
        mw_notify_locked(t->tt);
        release_core(t);
    }
    g_stats.threads_exited++;
    if (g_cfg.trace) log("thread %#x exit", t->handle);
    return 0;
}

void thread_start(Thread *t) {
    if (t->started.exchange(1)) return;
    gst8a(t->tt + TT_STATE, TS_STARTED);
    size_t ss = std::min<size_t>(g_cfg.host_stack, 0xfff00000u);
    uintptr_t h = _beginthreadex(nullptr, (unsigned)ss, host_entry, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) fatal("cannot create a host thread for ThreadType %#llx", (unsigned long long)t->tt);
    t->host_thread = (void *)h;
}

void thread_wait_exit(Thread *t) {
    Thread *me = self();
    Lock lk(G);
    while (!t->exited) block_on_key(me, lk, mkkey(K_JOIN, t->tt), W_JOIN, Deadline::max());
}

void thread_destroy(Thread *t) {
    if (t->started) {
        thread_wait_exit(t);
        if (t->host_thread) {
            WaitForSingleObject((HANDLE)t->host_thread, INFINITE);
            CloseHandle((HANDLE)t->host_thread);
            t->host_thread = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lk(G);
        g_handles.erase(t->handle);
        auto it = g_by_tt.find(t->tt);
        if (it != g_by_tt.end() && it->second == t) g_by_tt.erase(it);
    }
    mw_forget_object(t->tt);
    free_cpu(t->cpu);
    _aligned_free(t->tlr);
    if (t->owns_tt) free_guest(t->tt);
    delete t;
}

void sleep_ns(Thread *t, int64_t ns) {
    if (ns <= 0) {
        if (ns < 0) yield_thread(t);
        return;
    }
    {
        Lock lk(G);
        release_core(t);
    }
    if (snap_perf::on) {
        const uint64_t t0 = snap_perf::now_ns();
        precise_sleep(ns);
        const uint64_t d = snap_perf::now_ns() - t0;
        snap_perf::wait_add(snap_perf::WT_SLEEP, d);
        if (d >= snap_perf::g_wait_event_ns && is_frame_thread()) {
            char det[64];
            snprintf(det, sizeof det, "sleep requested %.3f ms", (double)ns / 1e6);
            snap_perf::event(snap_perf::tl_role == snap_perf::R_MAIN ? "mt_sleep" : "gt_sleep", (double)d / 1e6, det);
        }
    } else {
        precise_sleep(ns);
    }
    Lock lk(G);
    acquire_core(t, lk);
}

void yield_thread(Thread *t) {
    {
        Lock lk(G);
        if (t->core >= 0 && t->core < ncores()) {
            for (Thread *w : g_core_wait)
                if (can_use(w, t->core) && w->eff_prio <= t->eff_prio) {
                    release_core(t);
                    acquire_core(t, lk);
                    return;
                }
        }
    }
    SwitchToThread();
}

int guest_core_number(const Thread *t) {
    const int n = g_cfg.app_cores > 0 ? g_cfg.app_cores : 3;
    if (t->core >= 0 && t->core < n) return t->core;
    if (t->last_core >= 0 && t->last_core < n) return t->last_core;
    if (t->ideal >= 0 && t->ideal < n) return t->ideal;   // on a spare core (SNAP_OS_SPARE_CORES): report one the thread may use
    const uint64_t m = t->mask & ((1ull << n) - 1);
    return m ? __builtin_ctzll(m) : 0;
}
void set_core_mask(Thread *t, int ideal, uint64_t mask) {
    std::lock_guard<std::mutex> lk(G);
    if (ideal == -3) ideal = t->ideal;            // IdealCoreNoUpdate
    else if (ideal == -2) ideal = g_cfg.default_core;   // IdealCoreUseProcessValue
    if (mask == 0) mask = ideal >= 0 ? 1ull << ideal : t->mask;
    t->ideal = ideal;   // -1: IdealCoreDontCare
    t->mask = mask;
    if (t->core >= 0 && t->core < ncores() && !can_use(t, t->core)) t->preempt = 1;   // migrate at the next poll
    if (t->waiting_core) {
        t->cv.notify_one();
        kick_free_cores();   // t may no longer be the best waiter of a free core
    }
}

void change_priority(Thread *t, int prio) {
    std::lock_guard<std::mutex> lk(G);
    t->base_prio = prio;
    int old = t->eff_prio, p = prio;
    for (Thread *w : t->lock_waiters) p = std::min(p, w->eff_prio);
    if (p != old) {
        t->eff_prio = p;
        if (t->wkind == W_LOCK && t->lock_owner) recompute_priority(t->lock_owner);
    }
    if (t->eff_prio > old && t->core >= 0 && t->core < ncores())
        for (Thread *w : g_core_wait)
            if (can_use(w, t->core) && w->eff_prio < t->eff_prio) t->preempt = 1;
    if (t->waiting_core) {
        t->cv.notify_one();
        kick_free_cores();   // t may no longer be the best waiter of a free core
    }
}

}  // namespace osk

using namespace osk;

// ---- C interface
// Guest time freeze (host/window_opts.cpp pauses the game when the window loses focus): while paused, GetSystemTick stands still and the
// paused span is subtracted afterwards, so the game's frame delta does not jump on resume.
static std::mutex g_pause_mu;   // one lock so a tick read never sees the pause flag and the total out of step (time must not go backwards)
static int64_t g_paused_total = 0, g_pause_start = 0;
extern "C" void os_time_pause(int on) {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    std::lock_guard<std::mutex> lk(g_pause_mu);
    if (on) { if (!g_pause_start) g_pause_start = n.QuadPart; }
    else if (g_pause_start) { g_paused_total += n.QuadPart - g_pause_start; g_pause_start = 0; }
}
extern "C" uint64_t os_ticks(void) {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    { std::lock_guard<std::mutex> lk(g_pause_mu); if (g_pause_start) n.QuadPart = g_pause_start; n.QuadPart -= g_paused_total; }
    uint64_t d = (uint64_t)(n.QuadPart - qpc_start());
    uint64_t f = (uint64_t)qpc_freq();   // split so no 128-bit division (__udivti3) is needed; (d % f) * 19.2e6 < 2^63
    return g_cfg.tick_base + d / f * OS_TICK_FREQUENCY + d % f * OS_TICK_FREQUENCY / f;
}

extern "C" void os_init(const OsConfig *in) {
    Config c;
    if (in) {
        if (in->num_cores) c.num_cores = in->num_cores;
        c.default_core = in->default_core;
        if (in->main_priority) c.main_priority = in->main_priority;
        if (in->overcommit_us) c.overcommit_us = in->overcommit_us;
        if (in->rr_slice_us) c.rr_slice_us = in->rr_slice_us;
        if (in->host_stack_size) c.host_stack = in->host_stack_size;
        if (in->fiber_host_stack) c.fiber_host_stack = in->fiber_host_stack;
        c.tp_init_var = in->tp_init_var;
        c.tp_destroy_var = in->tp_destroy_var;
        if (in->tick_base) c.tick_base = in->tick_base;
        c.trace = in->trace;
    }
    // perf switches (pass 3): override the virtual-core model without a rebuild
    if (const char *e = getenv("SNAP_OS_CORES")) c.num_cores = atoi(e);              // -1 = unlimited (free host scheduling)
    if (const char *e = getenv("SNAP_OS_OVERCOMMIT_US")) c.overcommit_us = (uint32_t)atoi(e);
    if (const char *e = getenv("SNAP_OS_RR_US")) c.rr_slice_us = (uint32_t)atoi(e);
    if (c.num_cores > 64) c.num_cores = 64;
    c.app_cores = c.num_cores;
    // load pass (2026-11-13): SNAP_OS_SPARE_CORES=<n> adds n virtual cores that any thread may take whatever its core mask, so pinned
    // loader threads (ResourceUncompress p31) do not wait for one of the 3 cores while JobWorkers hold them. Guests still see cores 0..2.
    if (const char *e = getenv("SNAP_OS_SPARE_CORES"))
        if (c.num_cores > 0) c.num_cores = std::min(c.num_cores + std::max(atoi(e), 0), 62);
    g_cfg = c;
    std::lock_guard<std::mutex> lk(G);
    g_cores.assign(c.num_cores > 0 ? (size_t)c.num_cores : 0, nullptr);
    (void)qpc_freq();
    (void)qpc_start();
    // 1 ms system timer resolution for timed waits (SleepConditionVariableSRW); sleeps use high-resolution timers.
    using TB = UINT(WINAPI *)(UINT);
    if (HMODULE m = LoadLibraryW(L"winmm.dll"))
        if (TB tb = (TB)(void *)GetProcAddress(m, "timeBeginPeriod")) tb(1);
}

extern "C" void os_adopt_main_thread(Cpu *c, uint64_t tt, int prio) {
    if (prio < 0) prio = g_cfg.main_priority;
    snap_perf::set_role(snap_perf::R_MAIN);
    Thread *t = t_self;
    bool fresh = !t;
    if (fresh) {
        t = new Thread;
        t->tlr = new_tlr();
    }
    if (!tt) {
        tt = t->tt && t->owns_tt ? t->tt : alloc_guest(TT_SIZE);
        t->owns_tt = true;
        init_thread_type(tt, 0, 0, 0, 0, prio, TS_STARTED);
        set_thread_type_name(tt, "MainThread");   // sdk+0x3445ec
    }
    {
        std::lock_guard<std::mutex> lk(G);
        if (!fresh) {
            g_by_tt.erase(t->tt);
            g_handles.erase(t->handle);
        }
        t->tt = tt;
        register_thread(t);
    }
    t->is_host = false;
    t->cpu = t->cur = c;
    t->base_prio = t->eff_prio = prio;
    t->ideal = g_cfg.default_core;
    t->mask = 1ull << t->ideal;
    t->started = 1;
    gst<int16_t>(tt + TT_BASE_PRIORITY, (int16_t)prio);
    gst<uint64_t>((uint64_t)(uintptr_t)t->tlr + TLR_THREAD, tt);
    c->tpidrro = (uint64_t)(uintptr_t)t->tlr;
    c->thread = t;
    c->excl_addr = ~0ull;
    t_self = t;
    t_helper.t = nullptr;
    Lock lk(G);
    acquire_core(t, lk);
}

extern "C" void os_host_block_begin(void) {
    Thread *t = t_self;
    if (!t) return;
    std::lock_guard<std::mutex> lk(G);
    if (t->block_depth++ == 0) release_core(t);
}
extern "C" void os_host_block_end(void) {
    Thread *t = t_self;
    if (!t) return;
    Lock lk(G);
    if (t->block_depth > 0 && --t->block_depth == 0) acquire_core(t, lk);
}
extern "C" uint64_t os_current_thread_type(void) { return t_self ? t_self->tt : 0; }
// Diagnostics (host watchdog): visit every live guest thread with its wait state and the Cpu whose x30/sp the caller can symbolize.
extern "C" void os_dump_threads(void (*emit)(void *ud, uint64_t tt, uint64_t id, int wkind, uint64_t wkey, uint64_t waddr, int core, int prio, Cpu *cpu), void *ud) {
    bool locked = G.try_lock();   // diagnostic only: go on without the lock if a stuck thread holds it
    for (auto &kv : g_by_tt) {
        Thread *t = kv.second;
        emit(ud, t->tt, t->id | ((uint64_t)(t->waiting_core ? 1 : 0) << 62) | ((t->mask & 0xff) << 48) | ((uint64_t)((t->ideal + 4) & 0xf) << 56), t->wkind, t->wkey, t->waddr, t->core, t->eff_prio, t->cur ? t->cur : t->cpu);
    }
    for (int c = 0; c < g_cfg.num_cores; c++) emit(ud, 0xC0DE, (uint64_t)c, 99, 0, 0, c, g_cores[c] ? g_cores[c]->eff_prio : -1, nullptr);   // core occupancy: id = core, prio = holder prio
    {   // the core handoff state: who waits for a core, and which waiter each free core is reserved for
        char buf[768];
        int n = snprintf(buf, sizeof buf, "dump: G %s, overflow %d, rescues %llu, core waiters:", locked ? "locked" : "BUSY (unlocked read)", g_overflow,
                         (unsigned long long)g_stats.core_wake_rescues.load());
        for (Thread *w : g_core_wait)
            if (n < (int)sizeof buf - 40) n += snprintf(buf + n, sizeof buf - n, " id%llu/p%d/m%#llx", (unsigned long long)w->id, w->eff_prio, (unsigned long long)w->mask);
        for (int c = 0; c < g_cfg.num_cores; c++)
            if (!g_cores[c] && n < (int)sizeof buf - 40)
                if (Thread *b = best_waiter_for(c)) n += snprintf(buf + n, sizeof buf - n, " | free core %d -> id%llu", c, (unsigned long long)b->id);
        log("%s", buf);
    }
    if (locked) G.unlock();
}


extern "C" void os_poll(void) {
    if (Thread *t = t_self) poll_preempt(t);
}

extern "C" void os_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    (void)head;
    static thread_local uint32_t passes;
    Thread *t = t_self;
    if (t) poll_preempt(t);
    uint32_t n = ++passes;
    if ((n & 63) == 0) {   // a spinner may wait for a thread that has no core: offer ours
        g_stats.spin_yields++;
        if (t) yield_thread(t);
        else SwitchToThread();
    } else {
        _mm_pause();
    }
}

extern "C" void os_get_stats(OsStats *o) {
    o->threads_started = g_stats.threads_started;
    o->threads_exited = g_stats.threads_exited;
    o->lock_contended = g_stats.lock_contended;
    o->cv_waits = g_stats.cv_waits;
    o->core_waits = g_stats.core_waits;
    o->preempt_requests = g_stats.preempt_requests;
    o->preempt_yields = g_stats.preempt_yields;
    o->overflow_grants = g_stats.overflow_grants;
    o->pi_boosts = g_stats.pi_boosts;
    o->fiber_switches = g_stats.fiber_switches;
    o->spin_yields = g_stats.spin_yields;
    o->max_cores_in_use = g_stats.max_cores_in_use;
    o->core_wake_rescues = g_stats.core_wake_rescues;
}

// ---- SVC helpers for the logging backstop: the thread and arbitration SVCs that lifted sdk code can reach.
static Thread *svc_thread(uint32_t h) {
    if (h == PSEUDO_CURRENT_THREAD) return self();
    std::lock_guard<std::mutex> lk(G);
    return find_handle(h);
}
extern "C" int os_svc(Cpu *c, uint32_t imm) {
    Thread *me = self();
    switch (imm) {
    case 0x0b:   // SleepThread(ns): 0 / -1 / -2 are yields
        if ((int64_t)c->x[0] > 0) sleep_ns(me, (int64_t)c->x[0]);
        else yield_thread(me);
        return 1;
    case 0x0c: {   // GetThreadPriority(out w1, handle w1)
        Thread *t = svc_thread((uint32_t)c->x[1]);
        if (!t) { c->x[0] = R_INVALID_HANDLE; return 1; }
        c->x[0] = R_OK;
        c->x[1] = (uint64_t)(t->eff_prio + 28);
        return 1;
    }
    case 0x0d: {   // SetThreadPriority(handle w0, prio w1)
        Thread *t = svc_thread((uint32_t)c->x[0]);
        if (!t) { c->x[0] = R_INVALID_HANDLE; return 1; }
        change_priority(t, (int)(uint32_t)c->x[1] - 28);
        c->x[0] = R_OK;
        return 1;
    }
    case 0x0e: {   // GetThreadCoreMask(out w1 core, out x2 mask, handle w2)
        Thread *t = svc_thread((uint32_t)c->x[2]);
        if (!t) { c->x[0] = R_INVALID_HANDLE; return 1; }
        c->x[0] = R_OK;
        c->x[1] = (uint64_t)(uint32_t)t->ideal;
        c->x[2] = t->mask;
        return 1;
    }
    case 0x0f: {   // SetThreadCoreMask(handle w0, core w1, mask x2)
        Thread *t = svc_thread((uint32_t)c->x[0]);
        if (!t) { c->x[0] = R_INVALID_HANDLE; return 1; }
        set_core_mask(t, (int)(int32_t)c->x[1], c->x[2]);
        c->x[0] = R_OK;
        return 1;
    }
    case 0x10: {   // GetCurrentProcessorNumber
        std::lock_guard<std::mutex> lk(G);
        c->x[0] = (uint64_t)guest_core_number(me);
        return 1;
    }
    case 0x1a:     // ArbitrateLock(owner w0, addr x1, tag w2)
        c->x[0] = k_arbitrate_lock(me, (uint32_t)c->x[0], c->x[1], (uint32_t)c->x[2]);
        return 1;
    case 0x1b:     // ArbitrateUnlock(addr x0)
        c->x[0] = k_arbitrate_unlock(me, c->x[0]);
        return 1;
    case 0x1c:     // WaitProcessWideKeyAtomic(addr x0, key x1, tag w2, timeout x3)
        c->x[0] = k_wait_key_atomic(me, c->x[0], c->x[1], (uint32_t)c->x[2], (int64_t)c->x[3]);
        return 1;
    case 0x1d:     // SignalProcessWideKey(key x0, count w1)
        k_signal_key(c->x[0], (int)(int32_t)c->x[1]);
        c->x[0] = R_OK;
        return 1;
    case 0x1e:     // GetSystemTick
        c->x[0] = os_ticks();
        return 1;
    case 0x25: {   // GetThreadId(out x1, handle w1)
        Thread *t = svc_thread((uint32_t)c->x[1]);
        if (!t) { c->x[0] = R_INVALID_HANDLE; return 1; }
        c->x[0] = R_OK;
        c->x[1] = t->id;
        return 1;
    }
    case 0x26: {   // Break(reason w0, info x1, size x2); bit 31 of reason = notification only
        uint32_t reason = (uint32_t)c->x[0];
        log("svcBreak reason %#x info %#llx size %#llx", reason, (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
        if (!(reason & 0x80000000u)) fatal("svcBreak reason %#x (lr %#llx)", reason, (unsigned long long)c->x[30]);
        c->x[0] = R_OK;
        return 1;
    }
    case 0x27: {   // OutputDebugString(str x0, len x1)
        uint64_t n = std::min<uint64_t>(c->x[1], 1024);
        if (c->x[0] && n) log("svcOutputDebugString: %.*s", (int)n, (const char *)(uintptr_t)c->x[0]);
        c->x[0] = R_OK;
        return 1;
    }
    case 0x29: {   // GetInfo: in x1 type, x2 handle, x3 subtype; out w0 result, x1 value. See runtime/os/svc_getinfo.md
        uint32_t type = (uint32_t)c->x[1], handle = (uint32_t)c->x[2];
        uint64_t sub = c->x[3], v = 0;
        uint32_t r = R_OK;
        bool proc_type = !(type == 8 || type == 10 || type == 11);   // 8, 10, 11 take handle 0; the rest the process
        switch (type) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 12: case 13: case 14: case 15: case 16:
        case 17: case 18: case 20: case 21: case 22: case 23: case 8: case 10: case 11: break;
        default:
            log("svcGetInfo: unknown type %u (handle %#x sub %#llx)", type, handle, (unsigned long long)sub);
            c->x[0] = R_INVALID_ENUM_VALUE;
            return 1;
        }
        if (proc_type ? handle != PSEUDO_CURRENT_PROCESS : handle != 0) { c->x[0] = R_INVALID_HANDLE; return 1; }
        if (type != 10 && type != 11 && sub != 0) { c->x[0] = R_INVALID_COMBINATION; return 1; }
        switch (type) {
        case 0: { int n = g_cfg.app_cores; v = n > 0 && n < 64 ? (1ull << n) - 1 : 7; break; }   // CoreMask (guest cores; spare cores are hidden)
        case 1: v = 0xffffffffffffffffull; break;                                         // PriorityMask
        case 8: v = 0; break;                                                             // DebuggerAttached
        case 10: {                                                                        // IdleTickCount: no idle accounting
            int n = g_cfg.app_cores > 0 ? g_cfg.app_cores : 3;
            if ((int64_t)sub != -1 && sub >= (uint64_t)n) r = R_INVALID_COMBINATION;
            v = 0;
            break;
        }
        case 11: {                                                                        // RandomEntropy[0..3], fixed per run
            if (sub > 3) { r = R_INVALID_COMBINATION; break; }
            static const std::array<uint64_t, 4> ent = [] {
                std::random_device rd;
                std::array<uint64_t, 4> e;
                for (auto &x : e) x = ((uint64_t)rd() << 32) ^ rd();
                return e;
            }();
            v = ent[sub];
            break;
        }
        case 18: v = 0x0100F4300BF2C000ull; break;                                        // ProgramId
        case 20: {                                                                        // UserExceptionContextAddress
            static uint8_t *uctx = (uint8_t *)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            v = (uint64_t)(uintptr_t)uctx;
            break;
        }
        case 23: v = 1; break;                                                            // IsApplication
        default: r = mem_get_info(type, &v); break;
        }
        c->x[0] = r;
        if (r == R_OK) c->x[1] = v;
        return 1;
    }
    default:
        return 0;
    }
}
