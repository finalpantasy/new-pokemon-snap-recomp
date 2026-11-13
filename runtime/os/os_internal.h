// Internal interface of runtime/os (written for this project). The scheduler design follows the public Horizon
// kernel semantics (KConditionVariable, KAddressArbiter, KScheduler) and the shape of TotK Recomp runtime/kernel.c
// @ 7a37fc3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh): one global scheduler lock, one host condition variable per
// thread, wake order = lower priority value first, then first come. No code is copied from it.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "os.h"
#include "os_layout.h"

namespace osk {

// ---- guest memory: guest addresses are host addresses (runtime/cpu.h)
template <class T> static inline T gld(uint64_t a) { T v; std::memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
template <class T> static inline void gst(uint64_t a, T v) { std::memcpy((void *)(uintptr_t)a, &v, sizeof v); }
static inline std::atomic<uint32_t> &gword(uint64_t a) { return *reinterpret_cast<std::atomic<uint32_t> *>((uintptr_t)a); }
static inline uint8_t gld8a(uint64_t a) { return __atomic_load_n((uint8_t *)(uintptr_t)a, __ATOMIC_ACQUIRE); }
static inline void gst8a(uint64_t a, uint8_t v) { __atomic_store_n((uint8_t *)(uintptr_t)a, v, __ATOMIC_RELEASE); }
static inline int32_t gld32a(uint64_t a) { return __atomic_load_n((int32_t *)(uintptr_t)a, __ATOMIC_ACQUIRE); }
static inline void gst32a(uint64_t a, int32_t v) { __atomic_store_n((int32_t *)(uintptr_t)a, v, __ATOMIC_RELEASE); }

constexpr uint64_t OS_RET_HOST = 0x70200ff000ull;   // x30 sentinel of host->guest calls (same value as TotK RT_RET_HOST)
constexpr int64_t NS_INFINITE = INT64_MAX;
constexpr uint32_t R_OK = 0, R_TIMEOUT = oslay::RESULT_TIMED_OUT, R_INVALID_HANDLE = 0xe401, R_INVALID_STATE = 0xfa01;
constexpr uint32_t PSEUDO_CURRENT_THREAD = 0xffff8000u;
constexpr uint32_t PSEUDO_CURRENT_PROCESS = 0xffff8001u;
constexpr uint32_t R_INVALID_ENUM_VALUE = 0x7801, R_INVALID_COMBINATION = 0xe001;   // svc module 1, desc 60 / 112
// svcGetInfo memory-region values (os_memory.cpp): types 2-7, 12-17, 21, 22; returns R_OK or the Horizon result.
uint32_t mem_get_info(uint32_t type, uint64_t *out);

enum Wait : int { W_NONE, W_LOCK, W_CVAR, W_KEY, W_JOIN, W_MULTI, W_SLEEP };
// keyed wait queues: a key is the guest address plus the queue kind in the top byte (guest addresses are < 2^48)
enum KeyKind : uint64_t { K_CVAR = 1, K_LIGHT = 2, K_JOIN = 3, K_MULTI = 4, K_RW = 5, K_TIMER = 6 };
static inline uint64_t mkkey(KeyKind k, uint64_t addr) { return addr | ((uint64_t)k << 56); }

struct Fiber;

struct Thread {
    uint64_t tt = 0;                 // guest ThreadType
    uint32_t handle = 0;
    uint64_t id = 0;
    uint8_t *tlr = nullptr;          // guest TLR (0x200 bytes, 0x200-aligned)
    Cpu *cpu = nullptr;              // the thread's own context
    Cpu *cur = nullptr;              // the context running now (own or a fiber's)
    bool is_host = false;            // a host helper thread adopted on first use: holds no virtual core
    bool owns_tt = false;            // tt was allocated by the runtime
    uint64_t entry = 0, arg = 0;
    // scheduling (under G)
    int base_prio = 16, eff_prio = 16;
    int ideal = 0;
    uint64_t mask = 1;
    int core = -1, last_core = 0;
    uint64_t core_since = 0, ready_seq = 0;
    int block_depth = 0;
    std::atomic<int> preempt{0};
    bool waiting_core = false;
    bool core_kicked = false;        // a core handoff notify arrived since the waiter last checked (rescue accounting)
    // current wait (under G)
    std::condition_variable cv;
    int wkind = W_NONE;
    uint64_t wkey = 0, waddr = 0, wseq = 0;
    uint32_t wtag = 0, wresult = 0;
    bool woken = false;
    Thread *waker = nullptr;                 // the guest thread that last woke this one (nullptr: a host thread); perf only
    Thread *lock_owner = nullptr;            // W_LOCK: the thread whose lock we wait for
    std::vector<Thread *> lock_waiters;      // threads in W_LOCK on locks we own (priority inheritance)
    // lifecycle
    std::atomic<int> started{0}, exited{0};
    void *host_thread = nullptr;
    // fibers (Win32 host fibers)
    void *base_fiber = nullptr;
    uint64_t own_fiber = 0;                  // guest FiberType standing for the thread's own context
    Fiber *own_shadow = nullptr;
    Fiber *cur_fiber = nullptr;              // nullptr = own context
    Fiber *leaving = nullptr;                // context just switched away from, released once the next one runs
};

struct Fiber {
    uint64_t guest = 0;
    Cpu *cpu = nullptr;
    void *host = nullptr;                    // Win32 fiber
    bool finished = false;                   // its function returned; needs InitializeFiber before reuse
    bool is_thread_ctx = false;              // stands for a thread's own context
    std::atomic<bool> running{false};        // claimed by a thread; released by the next context ("leaving")
};

struct Config {
    int num_cores = 3, default_core = 0, main_priority = 16;
    int app_cores = 3;   // load pass: cores app_cores..num_cores-1 are spare cores (SNAP_OS_SPARE_CORES) any thread may use, whatever its mask
    uint32_t overcommit_us = 2000, rr_slice_us = 10000;
    uint32_t core_rescue_ms = 20;   // an untimed core wait rechecks this often (safety net; counted in core_wake_rescues)
    size_t host_stack = 256ull << 20, fiber_host_stack = 64ull << 20;
    uint64_t tp_init_var = 0, tp_destroy_var = 0, tick_base = OS_TICK_FREQUENCY * 10;
    int trace = 0;
};
extern Config g_cfg;

struct Stats {
    std::atomic<uint64_t> threads_started{0}, threads_exited{0}, lock_contended{0}, cv_waits{0}, core_waits{0},
        preempt_requests{0}, preempt_yields{0}, overflow_grants{0}, pi_boosts{0}, fiber_switches{0}, spin_yields{0},
        core_wake_rescues{0};   // a core waiter found a free core on its safety-net timeout: a missed handoff (should stay 0)
    std::atomic<int> max_cores_in_use{0};
};
extern Stats g_stats;

// ---- the scheduler lock and helpers (os_kernel.cpp)
extern std::mutex G;
using Lock = std::unique_lock<std::mutex>;
Thread *self();                          // the calling thread; adopts a host helper if it is not a guest thread
Thread *self_fresh();                    // same, never cached by the compiler (after a fiber switch)
Thread *find_handle(uint32_t h);         // under G
Thread *find_tt(uint64_t tt);            // under G
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t now_ns();                       // guest TimeSpan of the current tick (ConvertToTimeSpan(os_ticks()))
int64_t tick_to_ns(int64_t tick);        // exact replica of TickManager::ConvertToTimeSpan sdk+0x3451e0
int64_t ns_to_tick(int64_t ns);          // exact replica of TickManager::ConvertToTick sdk+0x345270
uint64_t call_guest(Cpu *c, uint64_t fn, uint64_t a0);   // host -> guest call through rt_dispatch

// virtual cores (under G)
void acquire_core(Thread *t, Lock &lk);
void release_core(Thread *t);
void poll_preempt(Thread *t);
// waits (under G). wait_woken releases nothing; callers release the core before and reacquire after.
using Deadline = std::chrono::steady_clock::time_point;
Deadline deadline_after(int64_t ns);     // ns >= NS_INFINITE/2 -> far future
bool is_infinite(Deadline d);
bool wait_woken(Thread *t, Lock &lk, Deadline d);         // true if woken, false on timeout
void wake(Thread *t, uint32_t result);
void keyq_push(uint64_t key, Thread *t);
bool keyq_remove(uint64_t key, Thread *t);
Thread *keyq_pop_best(uint64_t key);
bool keyq_empty(uint64_t key);
int keyq_wake_all(uint64_t key, uint32_t result);
// Generic blocking wait on a keyed queue: registers, releases the core, waits, reacquires. Returns R_OK if woken,
// R_TIMEOUT on timeout.
uint32_t block_on_key(Thread *t, Lock &lk, uint64_t key, int kind, Deadline d);
void recompute_priority(Thread *t);

// arbitration (takes G): Horizon svcArbitrateLock / ArbitrateUnlock / WaitProcessWideKeyAtomic / SignalProcessWideKey
uint32_t k_arbitrate_lock(Thread *me, uint32_t owner_handle, uint64_t addr, uint32_t tag);
uint32_t k_arbitrate_unlock(Thread *me, uint64_t addr);
uint32_t k_wait_key_atomic(Thread *me, uint64_t addr, uint64_t key, uint32_t tag, int64_t ns);
void k_signal_key(uint64_t key, int count);

// user-side primitives over guest words (InternalCriticalSection / InternalConditionVariable semantics)
void cs_init(uint64_t a);
void cs_enter(uint64_t a);
bool cs_try_enter(uint64_t a);
void cs_leave(uint64_t a);
bool cs_locked_by_me(uint64_t a);
uint32_t cv_wait(uint64_t cv, uint64_t cs, int64_t ns);   // R_OK or R_TIMEOUT; the lock is held again on return
void cv_signal(uint64_t cv);
void cv_broadcast(uint64_t cv);

// thread life (os_kernel.cpp)
Thread *thread_create(uint64_t tt, uint64_t entry, uint64_t arg, int prio, int ideal);
void thread_start(Thread *t);
void thread_wait_exit(Thread *t);
void thread_destroy(Thread *t);
void sleep_ns(Thread *t, int64_t ns);
void yield_thread(Thread *t);
void set_core_mask(Thread *t, int ideal, uint64_t mask);
int guest_core_number(const Thread *t);   // under G: the core the guest sees (0..app_cores-1, also on a spare or overflow core)
void change_priority(Thread *t, int prio);
std::vector<uint64_t> all_thread_types();   // under no lock; snapshot of every live ThreadType
void init_thread_type(uint64_t tt, uint64_t entry, uint64_t arg, uint64_t stack, uint64_t size, int prio, uint8_t state);
void set_thread_type_name(uint64_t tt, const char *s);
void set_default_thread_name(uint64_t tt);
Cpu *alloc_cpu();
void free_cpu(Cpu *c);
uint64_t alloc_guest(size_t size);           // zeroed, 16-aligned host memory the guest may use
void free_guest(uint64_t p);

// multi-wait (os_sync.cpp)
void mw_notify(uint64_t obj);            // takes G
void mw_notify_locked(uint64_t obj);     // under G
void mw_forget_object(uint64_t obj);

// TLS slot destructors at thread exit (os_thread.cpp)
void tls_run_destructors(Thread *t);

// fiber bookkeeping (os_thread.cpp)
void fiber_thread_exit(Thread *t);

}  // namespace osk
