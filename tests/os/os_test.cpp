// Host-only tests of runtime/os (no game data). Guest "functions" are host functions: the harness rt_dispatch calls
// c->pc as a GuestFn, so thread entries and fiber functions here are plain C++ functions taking Cpu*. Every nn::os call
// goes through os_override_find(mangled symbol), the same table the loader binds.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../runtime/os/os.h"
#include "../../runtime/os/os_layout.h"

using namespace oslay;

extern "C" void rt_dispatch(Cpu *c) { ((GuestFn)(uintptr_t)c->pc)(c); }

static int g_fail = 0, g_checks = 0;
static bool g_verbose = false;
#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        g_checks++;                                                                        \
        if (!(cond)) {                                                                     \
            g_fail++;                                                                      \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fprintf(stderr, "\n");                                                         \
        }                                                                                  \
    } while (0)

static GuestFn need(const char *s) {
    GuestFn f = os_override_find(s);
    if (!f) { fprintf(stderr, "missing override %s\n", s); exit(2); }
    return f;
}
template <class... A> static uint64_t call(Cpu *c, const char *sym, A... a) {
    uint64_t v[] = {(uint64_t)a..., 0};
    for (size_t i = 0; i < sizeof...(A); i++) c->x[i] = v[i];
    need(sym)(c);
    return c->x[0];
}
static uint64_t obj(size_t n = 0x200) {   // zeroed guest object memory
    void *p = _aligned_malloc(n, 64);
    memset(p, 0, n);
    return (uint64_t)(uintptr_t)p;
}
static uint64_t gfn(void (*f)(Cpu *)) { return (uint64_t)(uintptr_t)f; }
static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
static constexpr int64_t MS = 1000000;
static uint8_t gld8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }

// ---- symbols
#define S_CREATE "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"
#define S_CREATE7 "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"
#define S_START "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define S_WAITT "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"
#define S_DESTROY "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"
#define S_CURT "_ZN2nn2os16GetCurrentThreadEv"
#define S_CURPRIO "_ZN2nn2os24GetThreadCurrentPriorityEPKNS0_10ThreadTypeE"
#define S_PRIO "_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE"
#define S_CHPRIO "_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi"
#define S_SETMASK "_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim"
#define S_GETMASK "_ZN2nn2os17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"
#define S_CORE "_ZN2nn2os20GetCurrentCoreNumberEv"
#define S_YIELD "_ZN2nn2os11YieldThreadEv"
#define S_SLEEP "_ZN2nn2os11SleepThreadENS_8TimeSpanE"
#define S_NAMEP "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE"
#define S_SETNAME "_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc"
#define S_TLSALLOC "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"
#define S_TLSFREE "_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE"
#define S_TLSGET "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE"
#define S_TLSSET "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm"
#define S_FINIT "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define S_FFINAL "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define S_FSWITCH "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"
#define S_FCUR "_ZN2nn2os15GetCurrentFiberEv"
#define S_TICK "_ZN2nn2os13GetSystemTickEv"
#define S_FREQ "_ZN2nn2os22GetSystemTickFrequencyEv"
#define S_TOTS "_ZN2nn2os17ConvertToTimeSpanENS0_4TickE"
#define S_TOTICK "_ZN2nn2os13ConvertToTickENS_8TimeSpanE"
#define S_MXINIT "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi"
#define S_LOCK "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"
#define S_TRYLOCK "_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE"
#define S_UNLOCK "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"
#define S_MXMINE "_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE"
#define S_CVINIT "_ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE"
#define S_CVSIG "_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE"
#define S_CVBC "_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE"
#define S_CVWAIT "_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE"
#define S_CVTWAIT "_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE"
#define S_EVINIT "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"
#define S_EVSIG "_ZN2nn2os11SignalEventEPNS0_9EventTypeE"
#define S_EVWAIT "_ZN2nn2os9WaitEventEPNS0_9EventTypeE"
#define S_EVTRY "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE"
#define S_EVTWAIT "_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE"
#define S_EVCLR "_ZN2nn2os10ClearEventEPNS0_9EventTypeE"
#define S_EVFIN "_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE"
#define S_LEINIT "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE"
#define S_LESIG "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE"
#define S_LEWAIT "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE"
#define S_LETWAIT "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE"
#define S_SMINIT "_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii"
#define S_SMACQ "_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE"
#define S_SMTACQ "_ZN2nn2os21TimedAcquireSemaphoreEPNS0_13SemaphoreTypeENS_8TimeSpanE"
#define S_SMREL "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE"
#define S_SMCNT "_ZN2nn2os24GetCurrentSemaphoreCountEPKNS0_13SemaphoreTypeE"
#define S_BRINIT "_ZN2nn2os17InitializeBarrierEPNS0_11BarrierTypeEi"
#define S_BRAWAIT "_ZN2nn2os12AwaitBarrierEPNS0_11BarrierTypeE"
#define S_RWINIT "_ZN2nn2os26InitializeReaderWriterLockEPNS0_20ReaderWriterLockTypeE"
#define S_RLOCK "_ZN2nn2os15AcquireReadLockEPNS0_20ReaderWriterLockTypeE"
#define S_RUNLOCK "_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE"
#define S_WLOCK "_ZN2nn2os16AcquireWriteLockEPNS0_20ReaderWriterLockTypeE"
#define S_WUNLOCK "_ZN2nn2os16ReleaseWriteLockEPNS0_20ReaderWriterLockTypeE"
#define S_TEINIT "_ZN2nn2os20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE"
#define S_TEPERIODIC "_ZN2nn2os23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_"
#define S_TESTOP "_ZN2nn2os14StopTimerEventEPNS0_14TimerEventTypeE"
#define S_TEWAIT "_ZN2nn2os14WaitTimerEventEPNS0_14TimerEventTypeE"
#define S_MQINIT "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm"
#define S_MQSEND "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm"
#define S_MQJAM "_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm"
#define S_MQRECV "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"
#define S_MQPEEK "_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE"
#define S_MQTRYSEND "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm"
#define S_MWINIT "_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE"
#define S_MWFIN "_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE"
#define S_WAITANY "_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE"
#define S_TWAITANY "_ZN2nn2os12TimedWaitAnyEPNS0_13MultiWaitTypeENS_8TimeSpanE"
#define S_HEV "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE"
#define S_HTHREAD "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_10ThreadTypeE"
#define S_HMQ "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_16MessageQueueTypeENS0_20MessageQueueWaitTypeE"
#define S_HFIN "_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"
#define S_LINK "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE"
#define S_UNLINKALL "_ZN2nn2os24UnlinkAllMultiWaitHolderEPNS0_13MultiWaitTypeE"
#define S_SETUD "_ZN2nn2os26SetMultiWaitHolderUserDataEPNS0_19MultiWaitHolderTypeEm"
#define S_GETUD "_ZN2nn2os26GetMultiWaitHolderUserDataEPKNS0_19MultiWaitHolderTypeE"
#define S_SDKCVTW "_ZN2nn2os24SdkConditionVariableType9TimedWaitERNS0_12SdkMutexTypeENS_8TimeSpanE"
#define S_SDKLOCK "_ZN2nn2os12SdkMutexType4LockEv"
#define S_SDKUNLOCK "_ZN2nn2os12SdkMutexType6UnlockEv"
#define S_SDKMINE "_ZNK2nn2os12SdkMutexType23IsLockedByCurrentThreadEv"
#define S_SRMLOCK "_ZN2nn2os21SdkRecursiveMutexType4LockEv"
#define S_SRMUNLOCK "_ZN2nn2os21SdkRecursiveMutexType6UnlockEv"
#define S_CSINIT "_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon10InitializeEv"

// ---- thread helper: core < 0 = 6-argument CreateThread (process default core)
struct GThread { uint64_t tt; };
static GThread spawn(Cpu *c, void (*f)(Cpu *), uint64_t arg, int prio, int core = -1, bool start = true) {
    GThread t{obj(TT_SIZE)};
    uint64_t stack = obj(0x4000);
    uint64_t r = core < 0 ? call(c, S_CREATE, t.tt, gfn(f), arg, stack, 0x4000ull, prio)
                          : call(c, S_CREATE7, t.tt, gfn(f), arg, stack, 0x4000ull, prio, core);
    CHECK(r == 0, "CreateThread %llx", (unsigned long long)r);
    if (start) call(c, S_START, t.tt);
    return t;
}
static void join(Cpu *c, GThread t) { call(c, S_WAITT, t.tt); call(c, S_DESTROY, t.tt); }

// =========================================================================================== layouts and time
static void test_time(Cpu *c) {
    CHECK(call(c, S_FREQ) == 19200000ull, "freq");
    CHECK((int64_t)call(c, S_TOTS, 19200000ull) == 1000000000ll, "1 s of ticks");
    CHECK((int64_t)call(c, S_TOTS, 1ull) == 52, "1 tick = 52 ns (trunc of 52.08)");
    CHECK((int64_t)call(c, S_TOTICK, 1000000000ull) == 19200000ll, "1 s -> ticks");
    CHECK((int64_t)call(c, S_TOTICK, 1ull) == 1, "1 ns rounds up to 1 tick");
    CHECK((int64_t)call(c, S_TOTICK, (uint64_t)-1ll) == -1, "-1 ns -> -1 tick");
    CHECK((int64_t)call(c, S_TOTS, (uint64_t)INT64_MAX) == INT64_MAX, "clamp high");
    CHECK((int64_t)call(c, S_TOTS, (uint64_t)INT64_MIN) == INT64_MIN, "clamp low");
    uint64_t t0 = call(c, S_TICK);
    CHECK(os_ticks() >= t0, "os_ticks monotonic with GetSystemTick");
    auto w = std::chrono::steady_clock::now();
    call(c, S_SLEEP, 20 * MS);
    double el = ms_since(w);
    uint64_t dt = call(c, S_TICK) - t0;
    CHECK(el >= 19.5 && el < 200, "SleepThread 20 ms took %.2f ms", el);
    CHECK(dt >= 19200000ull / 1000 * 19 && dt < 19200000ull / 1000 * 200, "tick delta %llu", (unsigned long long)dt);
    // ThreadType of the adopted main thread
    uint64_t me = call(c, S_CURT);
    CHECK(me && me == os_current_thread_type(), "GetCurrentThread");
    CHECK(gld8(me + TT_STATE) == TS_STARTED, "main state %u", gld8(me + TT_STATE));
    CHECK(strcmp((const char *)(uintptr_t)call(c, S_NAMEP, me), "MainThread") == 0, "main name");
    CHECK(*(uint64_t *)(uintptr_t)(c->tpidrro + TLR_THREAD) == me, "TLR+0x1F8 = ThreadType");
    CHECK((c->tpidrro & 0x1ff) == 0, "TLR 0x200-aligned");
    CHECK((int)call(c, S_PRIO, me) == 16, "main priority 16");
}

// =========================================================================================== threads basics + TLS
static std::atomic<int> g_tls_dtor{0};
static uint32_t g_slot;
static void tls_dtor(Cpu *c) { if (c->x[0] == 0x1234) g_tls_dtor++; }
static void th_basic(Cpu *c) {
    uint64_t out = c->x[0];
    uint64_t me = call(c, S_CURT);
    *(uint64_t *)(uintptr_t)out = me;
    *(int *)(uintptr_t)(out + 8) = (int)call(c, S_CORE);
    call(c, S_TLSSET, (uint64_t)g_slot, 0x1234ull);
    *(uint64_t *)(uintptr_t)(out + 16) = call(c, S_TLSGET, (uint64_t)g_slot);
}
static void test_threads(Cpu *c) {
    uint64_t slot = obj(16);
    CHECK(call(c, S_TLSALLOC, slot, gfn(tls_dtor)) == 0, "AllocateTlsSlot");
    g_slot = *(uint32_t *)(uintptr_t)slot;
    CHECK(g_slot < 16, "user slot %u", g_slot);
    uint64_t out = obj(64);
    GThread t = spawn(c, th_basic, out, 23, 2, false);
    CHECK(gld8(t.tt + TT_STATE) == TS_INITIALIZED, "state before start");
    char nm[40];
    snprintf(nm, sizeof nm, "Thread_0x%016llx", (unsigned long long)gfn(th_basic));
    CHECK(strcmp((const char *)(uintptr_t)call(c, S_NAMEP, t.tt), nm) == 0, "default name");
    call(c, S_SETNAME, t.tt, (uint64_t)(uintptr_t)"Worker");
    CHECK(strcmp((const char *)(uintptr_t)call(c, S_NAMEP, t.tt), "Worker") == 0, "SetThreadName");
    uint64_t core = obj(16), mask = obj(16);
    call(c, S_GETMASK, core, mask, t.tt);
    CHECK(*(int *)(uintptr_t)core == 2 && *(uint64_t *)(uintptr_t)mask == 4, "pinned to ideal core 2 (mask %llx)",
          (unsigned long long)*(uint64_t *)(uintptr_t)mask);
    call(c, S_START, t.tt);
    call(c, S_WAITT, t.tt);
    CHECK(gld8(t.tt + TT_STATE) == TS_TERMINATED, "state after exit %u", gld8(t.tt + TT_STATE));
    CHECK(*(uint64_t *)(uintptr_t)out == t.tt, "GetCurrentThread in thread");
    CHECK(*(int *)(uintptr_t)(out + 8) == 2, "ran on core %d", *(int *)(uintptr_t)(out + 8));
    CHECK(*(uint64_t *)(uintptr_t)(out + 16) == 0x1234, "TLS value");
    CHECK(g_tls_dtor == 1, "TLS destructor ran %d", g_tls_dtor.load());
    call(c, S_DESTROY, t.tt);
    CHECK(gld8(t.tt + TT_STATE) == TS_NOT_INITIALIZED, "state after destroy %u", gld8(t.tt + TT_STATE));
    call(c, S_TLSFREE, (uint64_t)g_slot);
}

// =========================================================================================== producer/consumer
static uint64_t g_pc_mq;
static std::atomic<uint64_t> g_pc_sum{0}, g_pc_cnt{0};
static constexpr int PC_ITEMS = 5000, PC_PROD = 4;
static void th_producer(Cpu *c) {
    uint64_t base = c->x[0];
    for (int i = 1; i <= PC_ITEMS; i++) call(c, S_MQSEND, g_pc_mq, base * 100000 + i);
}
static void th_consumer(Cpu *c) {
    uint64_t out = obj(16);
    for (;;) {
        call(c, S_MQRECV, out, g_pc_mq);
        uint64_t v = *(uint64_t *)(uintptr_t)out;
        if (v == 0) break;   // poison
        g_pc_sum += v;
        g_pc_cnt++;
    }
}
static void test_producer_consumer(Cpu *c) {
    g_pc_mq = obj(0x48);
    uint64_t buf = obj(8 * 8);
    call(c, S_MQINIT, g_pc_mq, buf, 8ull);
    const int prios[8] = {31, 23, 16, 8, 1, 31, 23, 16};
    std::vector<GThread> ts;
    for (int i = 0; i < 4; i++) ts.push_back(spawn(c, th_consumer, 0, prios[4 + i], i % 3));
    for (int i = 0; i < PC_PROD; i++) ts.push_back(spawn(c, th_producer, (uint64_t)(i + 1), prios[i], (i + 1) % 3));
    for (int i = 4; i < 8; i++) call(c, S_WAITT, ts[i].tt);
    for (int i = 0; i < 4; i++) call(c, S_MQSEND, g_pc_mq, 0ull);
    for (auto &t : ts) join(c, t);
    uint64_t want = 0;
    for (int p = 1; p <= PC_PROD; p++)
        for (int i = 1; i <= PC_ITEMS; i++) want += (uint64_t)p * 100000 + i;
    CHECK(g_pc_cnt == (uint64_t)PC_PROD * PC_ITEMS, "received %llu", (unsigned long long)g_pc_cnt.load());
    CHECK(g_pc_sum == want, "sum %llu want %llu", (unsigned long long)g_pc_sum.load(), (unsigned long long)want);
    // MessageQueue ordering: Jam goes to the front, TryPeek does not remove
    uint64_t q = obj(0x48), b = obj(64), out = obj(16);
    call(c, S_MQINIT, q, b, 3ull);
    call(c, S_MQSEND, q, 2ull);
    call(c, S_MQSEND, q, 3ull);
    call(c, S_MQJAM, q, 1ull);
    CHECK(call(c, S_MQTRYSEND, q, 4ull) == 0, "TrySend on a full queue");
    CHECK(call(c, S_MQPEEK, out, q) == 1 && *(uint64_t *)(uintptr_t)out == 1, "peek");
    for (uint64_t want_v = 1; want_v <= 3; want_v++) {
        call(c, S_MQRECV, out, q);
        CHECK(*(uint64_t *)(uintptr_t)out == want_v, "recv %llu want %llu", (unsigned long long)*(uint64_t *)(uintptr_t)out,
              (unsigned long long)want_v);
    }
}

// =========================================================================================== priority inversion
static uint64_t g_pi_mx, g_pi_locked, g_pi_go;
static std::atomic<int> g_pi_low_prio_seen{-1};
static std::atomic<bool> g_pi_high_got{false}, g_pi_medium_done{false};
static std::atomic<double> g_pi_high_ms{0};
static std::chrono::steady_clock::time_point g_pi_t0;
static void th_low(Cpu *c) {
    call(c, S_LOCK, g_pi_mx);
    call(c, S_EVSIG, g_pi_locked);
    call(c, S_EVWAIT, g_pi_go);   // blocks holding the mutex; the main thread then starts M and H
    g_pi_low_prio_seen = (int)call(c, S_CURPRIO, call(c, S_CURT));
    call(c, S_UNLOCK, g_pi_mx);
}
static void th_medium(Cpu *c) {   // CPU hog at priority 16 on core 1, polling like a lifted back-edge would
    auto t0 = std::chrono::steady_clock::now();
    while (ms_since(t0) < 600) call(c, S_TICK);
    g_pi_medium_done = true;
}
static void th_high(Cpu *c) {
    call(c, S_LOCK, g_pi_mx);
    g_pi_high_ms = ms_since(g_pi_t0);
    g_pi_high_got = !g_pi_medium_done;
    call(c, S_UNLOCK, g_pi_mx);
}
static void test_priority_inversion(Cpu *c) {
    g_pi_mx = obj(0x20); g_pi_locked = obj(0x28); g_pi_go = obj(0x28);
    call(c, S_MXINIT, g_pi_mx, 0ull, 0ull);
    call(c, S_EVINIT, g_pi_locked, 0ull, 0ull);
    call(c, S_EVINIT, g_pi_go, 0ull, 0ull);
    OsStats s0;
    os_get_stats(&s0);
    GThread L = spawn(c, th_low, 0, 31, 1);
    call(c, S_EVWAIT, g_pi_locked);
    GThread H = spawn(c, th_high, 0, 1, 2);
    call(c, S_SLEEP, 30 * MS);   // H is now blocked on the mutex L holds
    int boosted = (int)call(c, S_CURPRIO, L.tt);
    CHECK(boosted == 1, "L current priority while H waits: %d (want 1)", boosted);
    CHECK((int)call(c, S_PRIO, L.tt) == 31, "L base priority unchanged");
    GThread M = spawn(c, th_medium, 0, 16, 1);
    call(c, S_SLEEP, 20 * MS);   // M owns core 1
    g_pi_t0 = std::chrono::steady_clock::now();
    call(c, S_EVSIG, g_pi_go);   // L (boosted to 1) must preempt M on core 1 at M's next poll
    join(c, H);
    join(c, L);
    join(c, M);
    OsStats s1;
    os_get_stats(&s1);
    CHECK(g_pi_low_prio_seen == 1, "L saw current priority %d while H waited", g_pi_low_prio_seen.load());
    CHECK(g_pi_high_got, "H got the mutex only after M finished (%.1f ms): no priority inheritance", g_pi_high_ms.load());
    CHECK(g_pi_high_ms < 100, "H waited %.1f ms after go", g_pi_high_ms.load());
    CHECK(s1.pi_boosts > s0.pi_boosts, "pi_boosts did not grow");
    CHECK(s1.preempt_requests > s0.preempt_requests, "no preemption request");
    printf("  priority inversion: H got the lock %.2f ms after go (M spins 600 ms); pi_boosts +%llu, preempt +%llu\n",
           g_pi_high_ms.load(), (unsigned long long)(s1.pi_boosts - s0.pi_boosts),
           (unsigned long long)(s1.preempt_requests - s0.preempt_requests));
}

// =========================================================================================== fibers
static uint64_t g_fa, g_fb;
static std::string g_flog;
static constexpr int PING = 2000;
static void fiber_a(Cpu *c) {
    for (int i = 0; i < PING; i++) {
        g_flog += 'A';
        if (call(c, S_FCUR) != g_fa) g_flog += '!';
        call(c, S_FSWITCH, g_fb);
    }
    c->x[0] = 0;   // return to the thread
}
static void fiber_b(Cpu *c) {
    for (;;) {
        g_flog += 'B';
        call(c, S_FSWITCH, g_fa);
    }
}
// migration: a fiber that bounces back to whichever thread switched into it
static uint64_t g_fm;
static std::atomic<int> g_fm_count{0};
static std::atomic<uint64_t> g_fm_seen_tt[3];
static void fiber_m(Cpu *c) {
    for (;;) {
        int n = g_fm_count++;
        if (n < 3) g_fm_seen_tt[n] = call(c, S_CURT);
        call(c, S_FSWITCH, 0ull);
    }
}
static void th_fiber_user(Cpu *c) {
    call(c, S_FSWITCH, g_fm);
    CHECK(call(c, S_FCUR) == 0 || call(c, S_FCUR) != g_fm, "back on own context");
}
static void test_fibers(Cpu *c) {
    g_fa = obj(FB_SIZE); g_fb = obj(FB_SIZE);
    call(c, S_FINIT, g_fa, gfn(fiber_a), 0ull, obj(0x4000), 0x4000ull, 0ull);
    call(c, S_FINIT, g_fb, gfn(fiber_b), 0ull, obj(0x4000), 0x4000ull, 0ull);
    CHECK(gld8(g_fa + FB_STATE) == FS_READY, "fiber ready");
    CHECK(call(c, S_FCUR) == 0, "no current fiber");
    call(c, S_FSWITCH, g_fa);
    CHECK(call(c, S_FCUR) == 0, "current fiber after return %llx", (unsigned long long)call(c, S_FCUR));
    bool alt = g_flog.size() == 2 * PING;
    for (size_t i = 0; alt && i < g_flog.size(); i++) alt = g_flog[i] == (i % 2 ? 'B' : 'A');
    CHECK(alt, "ping-pong log size %zu", g_flog.size());
    uint64_t me = call(c, S_CURT);
    CHECK(*(uint64_t *)(uintptr_t)(me + TT_CURRENT_FIBER) == 0 && *(uint64_t *)(uintptr_t)(me + TT_THREAD_FIBER) == 0,
          "ThreadType fiber fields cleared");
    call(c, S_FFINAL, g_fa);
    call(c, S_FFINAL, g_fb);
    // a fiber resumed by three different threads (the job scheduler pattern)
    g_fm = obj(FB_SIZE);
    call(c, S_FINIT, g_fm, gfn(fiber_m), 0ull, obj(0x4000), 0x4000ull, 0ull);
    GThread ts[3];
    for (int i = 0; i < 3; i++) {
        ts[i] = spawn(c, th_fiber_user, 0, 16, i);
        join(c, ts[i]);
    }
    CHECK(g_fm_count == 3, "migrating fiber ran %d times", g_fm_count.load());
    for (int i = 0; i < 3; i++) CHECK(g_fm_seen_tt[i] == ts[i].tt, "fiber saw thread %d as current", i);
    call(c, S_FFINAL, g_fm);
}

// =========================================================================================== MultiWait
static uint64_t g_mw_ev[3];
static void th_signal_later(Cpu *c) {
    int idx = (int)c->x[0];
    call(c, S_SLEEP, 20 * MS);
    call(c, S_EVSIG, g_mw_ev[idx]);
}
static void th_quick(Cpu *c) { call(c, S_SLEEP, 10 * MS); }
static void test_multiwait(Cpu *c) {
    uint64_t mw = obj(0x30), h[3], ht = obj(0x40);
    call(c, S_MWINIT, mw);
    for (int i = 0; i < 3; i++) {
        g_mw_ev[i] = obj(0x28);
        h[i] = obj(0x40);
        call(c, S_EVINIT, g_mw_ev[i], 0ull, 0ull);   // manual clear
        call(c, S_HEV, h[i], g_mw_ev[i]);
        call(c, S_SETUD, h[i], (uint64_t)(100 + i));
        call(c, S_LINK, mw, h[i]);
    }
    auto t0 = std::chrono::steady_clock::now();
    CHECK(call(c, S_TWAITANY, mw, 15 * MS) == 0, "TimedWaitAny with nothing signalled");
    double el = ms_since(t0);
    CHECK(el >= 14.5 && el < 200, "timeout took %.2f ms", el);
    GThread t = spawn(c, th_signal_later, 1, 16, 1);
    uint64_t got = call(c, S_WAITANY, mw);
    CHECK(got == h[1] && call(c, S_GETUD, got) == 101, "WaitAny got %llx", (unsigned long long)got);
    join(c, t);
    call(c, S_EVSIG, g_mw_ev[2]);
    call(c, S_EVSIG, g_mw_ev[0]);
    CHECK(call(c, S_WAITANY, mw) == h[0], "link order wins when several are signalled");
    for (int i = 0; i < 3; i++) call(c, S_EVCLR, g_mw_ev[i]);
    // a thread holder: signalled when the thread exits
    GThread q = spawn(c, th_quick, 0, 16, 2, false);
    call(c, S_HTHREAD, ht, q.tt);
    call(c, S_LINK, mw, ht);
    call(c, S_START, q.tt);
    CHECK(call(c, S_WAITANY, mw) == ht, "thread holder");
    call(c, S_UNLINKALL, mw);
    for (int i = 0; i < 3; i++) call(c, S_HFIN, h[i]);
    call(c, S_HFIN, ht);
    call(c, S_MWFIN, mw);
    join(c, q);
}

// =========================================================================================== stress (10K iterations)
static constexpr int ITER = 10000;
static uint64_t g_sx, g_cv, g_e1, g_e2, g_le1, g_le2, g_sem, g_rw, g_bar;
static uint64_t g_counter;          // protected by g_sx (non-atomic on purpose)
static volatile uint64_t g_rw_a, g_rw_b;
static std::atomic<int> g_rw_bad{0}, g_bar_bad{0};
static std::atomic<int> g_bar_round[3];
static void th_mutex_stress(Cpu *c) {
    for (int i = 0; i < ITER; i++) {
        call(c, S_LOCK, g_sx);
        uint64_t v = g_counter;
        if (i % 97 == 0) call(c, S_YIELD);
        g_counter = v + 1;
        call(c, S_UNLOCK, g_sx);
    }
}
static void th_event_pong(Cpu *c) {
    for (int i = 0; i < ITER; i++) {
        call(c, S_EVWAIT, g_e1);
        call(c, S_EVSIG, g_e2);
    }
}
static void th_light_pong(Cpu *c) {
    for (int i = 0; i < ITER; i++) {
        call(c, S_LEWAIT, g_le1);
        call(c, S_LESIG, g_le2);
    }
}
static void th_sem_producer(Cpu *c) { for (int i = 0; i < ITER; i++) call(c, S_SMREL, g_sem); }
static uint64_t g_cv_items;   // under g_sx
static void th_cv_consumer(Cpu *c) {
    for (int i = 0; i < ITER / 2; i++) {
        call(c, S_LOCK, g_sx);
        while (g_cv_items == 0) call(c, S_CVWAIT, g_cv, g_sx);
        g_cv_items--;
        call(c, S_UNLOCK, g_sx);
    }
}
static void th_rw(Cpu *c) {
    int id = (int)c->x[0];
    for (int i = 0; i < ITER / 4; i++) {
        if (id < 2) {
            call(c, S_WLOCK, g_rw);
            g_rw_a = g_rw_a + 1;
            if (i % 31 == 0) call(c, S_YIELD);
            g_rw_b = g_rw_a;
            call(c, S_WUNLOCK, g_rw);
        } else {
            call(c, S_RLOCK, g_rw);
            if (g_rw_a != g_rw_b) g_rw_bad++;
            call(c, S_RUNLOCK, g_rw);
        }
    }
}
static void th_barrier(Cpu *c) {
    int id = (int)c->x[0];
    for (int r = 0; r < 1000; r++) {
        g_bar_round[id] = r;
        call(c, S_BRAWAIT, g_bar);
        for (int j = 0; j < 3; j++) if (g_bar_round[j] < r) g_bar_bad++;
        call(c, S_BRAWAIT, g_bar);
    }
}
static void test_stress(Cpu *c) {
    auto t0 = std::chrono::steady_clock::now();
    g_sx = obj(0x20); g_cv = obj(8);
    call(c, S_MXINIT, g_sx, 0ull, 0ull);
    call(c, S_CVINIT, g_cv);
    if (g_verbose) { printf("  step 1\n"); fflush(stdout); }
    // 1. mutex: 6 threads x 10K on 3 cores, mixed priorities
    std::vector<GThread> ts;
    for (int i = 0; i < 6; i++) ts.push_back(spawn(c, th_mutex_stress, 0, i % 2 ? 23 : 16, i % 3));
    for (auto &t : ts) join(c, t);
    CHECK(g_counter == 6ull * ITER, "mutex counter %llu", (unsigned long long)g_counter);
    if (g_verbose) { printf("  step 2\n"); fflush(stdout); }
    // 2. recursive mutex
    uint64_t rm = obj(0x20);
    call(c, S_MXINIT, rm, 1ull, 0ull);
    call(c, S_LOCK, rm);
    CHECK(call(c, S_TRYLOCK, rm) == 1, "recursive TryLock");
    CHECK(call(c, S_MXMINE, rm) == 1, "IsMutexLockedByCurrentThread");
    call(c, S_UNLOCK, rm);
    call(c, S_UNLOCK, rm);
    CHECK(call(c, S_MXMINE, rm) == 0, "unlocked");
    if (g_verbose) { printf("  step 3\n"); fflush(stdout); }
    // 3. AutoClear Event ping-pong, 10K round trips
    g_e1 = obj(0x28); g_e2 = obj(0x28);
    call(c, S_EVINIT, g_e1, 0ull, 1ull);
    call(c, S_EVINIT, g_e2, 0ull, 1ull);
    GThread pe = spawn(c, th_event_pong, 0, 16, 1);
    for (int i = 0; i < ITER; i++) {
        call(c, S_EVSIG, g_e1);
        call(c, S_EVWAIT, g_e2);
    }
    join(c, pe);
    CHECK(call(c, S_EVTRY, g_e1) == 0 && call(c, S_EVTRY, g_e2) == 0, "events consumed");
    CHECK(call(c, S_EVTWAIT, g_e1, 2 * MS) == 0, "TimedWaitEvent times out");
    if (g_verbose) { printf("  step 4\n"); fflush(stdout); }
    // 4. LightEvent ping-pong
    g_le1 = obj(16); g_le2 = obj(16);
    call(c, S_LEINIT, g_le1, 0ull, 1ull);
    call(c, S_LEINIT, g_le2, 0ull, 1ull);
    GThread pl = spawn(c, th_light_pong, 0, 23, 2);
    for (int i = 0; i < ITER; i++) {
        call(c, S_LESIG, g_le1);
        call(c, S_LEWAIT, g_le2);
    }
    join(c, pl);
    CHECK(call(c, S_LETWAIT, g_le2, 2 * MS) == 0, "TimedWaitLightEvent times out");
    if (g_verbose) { printf("  step 5\n"); fflush(stdout); }
    // 5. Semaphore 10K
    g_sem = obj(0x28);
    call(c, S_SMINIT, g_sem, 0ull, (uint64_t)ITER);
    GThread ps = spawn(c, th_sem_producer, 0, 16, 1);
    for (int i = 0; i < ITER; i++) call(c, S_SMACQ, g_sem);
    join(c, ps);
    CHECK(call(c, S_SMCNT, g_sem) == 0, "semaphore count %llu", (unsigned long long)call(c, S_SMCNT, g_sem));
    CHECK(call(c, S_SMTACQ, g_sem, 2 * MS) == 0, "TimedAcquireSemaphore times out");
    if (g_verbose) { printf("  step 6\n"); fflush(stdout); }
    // 6. ConditionVariable: 2 consumers x 5K, main produces
    GThread cc[2] = {spawn(c, th_cv_consumer, 0, 16, 1), spawn(c, th_cv_consumer, 0, 31, 2)};
    for (int i = 0; i < ITER; i++) {
        call(c, S_LOCK, g_sx);
        g_cv_items++;
        if (i % 2) call(c, S_CVSIG, g_cv); else call(c, S_CVBC, g_cv);
        call(c, S_UNLOCK, g_sx);
    }
    join(c, cc[0]);
    join(c, cc[1]);
    CHECK(g_cv_items == 0, "cv items left %llu", (unsigned long long)g_cv_items);
    call(c, S_LOCK, g_sx);
    CHECK(call(c, S_CVTWAIT, g_cv, g_sx, 2 * MS) == 0, "TimedWaitConditionVariable -> Timeout");
    CHECK(call(c, S_MXMINE, g_sx) == 1, "mutex held again after the timed wait");
    call(c, S_UNLOCK, g_sx);
    if (g_verbose) { printf("  step 7\n"); fflush(stdout); }
    // 7. ReaderWriterLock: 2 writers, 4 readers
    g_rw = obj(0x40);
    call(c, S_RWINIT, g_rw);
    std::vector<GThread> rw;
    for (int i = 0; i < 6; i++) rw.push_back(spawn(c, th_rw, (uint64_t)i, i < 2 ? 16 : 23, i % 3));
    for (auto &t : rw) join(c, t);
    CHECK(g_rw_bad == 0 && g_rw_a == 2ull * (ITER / 4), "rwlock bad %d a %llu", g_rw_bad.load(), (unsigned long long)g_rw_a);
    if (g_verbose) { printf("  step 8\n"); fflush(stdout); }
    // 8. Barrier: 3 threads x 1000 rounds
    g_bar = obj(0x20);
    call(c, S_BRINIT, g_bar, 3ull);
    std::vector<GThread> br;
    for (int i = 0; i < 3; i++) br.push_back(spawn(c, th_barrier, (uint64_t)i, 16, i));
    for (auto &t : br) join(c, t);
    CHECK(g_bar_bad == 0, "barrier violations %d", g_bar_bad.load());
    if (g_verbose) { printf("  step 9\n"); fflush(stdout); }
    // 9. SdkMutex / SdkConditionVariable timed wait, SdkRecursiveMutex
    uint64_t sm = obj(16), scv = obj(16), srm = obj(16);
    call(c, S_SDKLOCK, sm);
    CHECK(call(c, S_SDKMINE, sm) == 1, "SdkMutex IsLockedByCurrentThread");
    CHECK((*(uint32_t *)(uintptr_t)sm & ~LOCK_WAITERS) != 0, "lock word holds the owner handle");
    CHECK(call(c, S_SDKCVTW, scv, sm, 3 * MS) == 0, "SdkConditionVariable::TimedWait -> false");
    call(c, S_SDKUNLOCK, sm);
    CHECK(*(uint32_t *)(uintptr_t)sm == 0, "lock word 0 after unlock");
    call(c, S_SRMLOCK, srm);
    call(c, S_SRMLOCK, srm);
    call(c, S_SRMUNLOCK, srm);
    CHECK(*(uint32_t *)(uintptr_t)srm != 0, "recursive still held");
    call(c, S_SRMUNLOCK, srm);
    CHECK(*(uint32_t *)(uintptr_t)srm == 0, "recursive released");
    if (g_verbose) { printf("  step 10\n"); fflush(stdout); }
    // 10. periodic TimerEvent, 5 periods of 5 ms
    uint64_t te = obj(0x40);
    call(c, S_TEINIT, te, 1ull);
    auto tt0 = std::chrono::steady_clock::now();
    call(c, S_TEPERIODIC, te, 5 * MS, 5 * MS);
    for (int i = 0; i < 5; i++) call(c, S_TEWAIT, te);
    double tel = ms_since(tt0);
    call(c, S_TESTOP, te);
    CHECK(tel >= 24 && tel < 250, "5 timer periods took %.2f ms", tel);
    printf("  stress: %.0f ms total\n", ms_since(t0));
}

// =========================================================================================== priority wake order
static std::vector<int> g_order;
static uint64_t g_wo_mx, g_wo_cv;
static std::atomic<int> g_wo_waiting{0};
static void th_waiter(Cpu *c) {
    int prio = (int)c->x[0];
    call(c, S_LOCK, g_wo_mx);
    g_wo_waiting++;
    call(c, S_CVWAIT, g_wo_cv, g_wo_mx);
    g_order.push_back(prio);
    call(c, S_UNLOCK, g_wo_mx);
}
static void test_wake_order(Cpu *c) {
    g_wo_mx = obj(0x20); g_wo_cv = obj(8);
    call(c, S_MXINIT, g_wo_mx, 0ull, 0ull);
    call(c, S_CVINIT, g_wo_cv);
    const int pr[5] = {31, 8, 23, 1, 16};
    std::vector<GThread> ts;
    for (int p : pr) {
        ts.push_back(spawn(c, th_waiter, (uint64_t)p, p, 1));
        while (g_wo_waiting < (int)ts.size()) call(c, S_SLEEP, 1 * MS);
    }
    call(c, S_SLEEP, 5 * MS);
    for (int i = 0; i < 5; i++) {
        call(c, S_CVSIG, g_wo_cv);
        call(c, S_SLEEP, 5 * MS);
    }
    for (auto &t : ts) join(c, t);
    bool ok = g_order.size() == 5 && g_order[0] == 1 && g_order[1] == 8 && g_order[2] == 16 && g_order[3] == 23 &&
              g_order[4] == 31;
    CHECK(ok, "wake order %d %d %d %d %d", g_order.size() > 0 ? g_order[0] : -1, g_order.size() > 1 ? g_order[1] : -1,
          g_order.size() > 2 ? g_order[2] : -1, g_order.size() > 3 ? g_order[3] : -1, g_order.size() > 4 ? g_order[4] : -1);
}

// =========================================================================================== core handoff (lost core wakeup)
// Regression for the intro hang (analysis/boot/runs/1-20261112_001041, photo r5/r8): pinned threads sat in acquire_core
// with every virtual core free. Two cores are freed back to back while a better, unpinned waiter A sleeps; both releases
// notify A. A takes its ideal core 2 and must hand the wakeup for core 0 on to the core-0-pinned waiter P. Before the fix,
// take_core still saw A in the wait list and notified A itself, so P (and the main thread joining it) slept forever.
extern "C" void os_dump_threads(void (*emit)(void *, uint64_t, uint64_t, int, uint64_t, uint64_t, int, int, Cpu *), void *ud);
static std::atomic<int> g_ch_go{0}, g_ch_on_core{0}, g_ch_p_ran{0}, g_ch_round{0};
static void th_ch_holder(Cpu *c) {   // holds its pinned core, polling like a lifted loop, until go
    g_ch_on_core++;
    while (!g_ch_go.load()) call(c, S_TICK);
}
static void th_ch_a(Cpu *c) {}
static void th_ch_p(Cpu *c) { g_ch_p_ran = 1; }
static void ch_dump(void *, uint64_t tt, uint64_t id, int wkind, uint64_t, uint64_t, int core, int prio, Cpu *) {
    if (tt == 0xC0DE) fprintf(stderr, "    core %llu holder prio %d\n", (unsigned long long)id, prio);
    else fprintf(stderr, "    thread %llu waiting_core=%d mask=%#llx wkind=%d core=%d prio=%d\n", (unsigned long long)(id & 0xffffffffffffull),
                 (int)((id >> 62) & 1), (unsigned long long)((id >> 48) & 0xff), wkind, core, prio);
}
static void test_core_handoff(Cpu *c) {
    const int ROUNDS = 300;
    std::atomic<bool> done{false};
    std::thread wd([&] {   // a lost wakeup hangs the test: report it and fail instead of waiting forever
        int last = -1;
        auto t = std::chrono::steady_clock::now();
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            int r = g_ch_round.load();
            if (r != last) { last = r; t = std::chrono::steady_clock::now(); continue; }
            if (ms_since(t) > 3000) {
                fprintf(stderr, "FAIL core handoff: round %d stuck 3 s with free cores (lost core wakeup)\n", r);
                os_dump_threads(ch_dump, nullptr);
                fflush(stderr);
                _exit(1);
            }
        }
    });
    OsStats s0;
    os_get_stats(&s0);
    uint64_t me = call(c, S_CURT);
    int old = (int)call(c, S_PRIO, me);
    call(c, S_CHPRIO, me, 5ull);   // the main thread is the core 0 holder
    for (int r = 0; r < ROUNDS; r++) {
        g_ch_go = 0; g_ch_on_core = 0; g_ch_p_ran = 0;
        GThread h1 = spawn(c, th_ch_holder, 0, 5, 1), h2 = spawn(c, th_ch_holder, 0, 5, 2);
        while (g_ch_on_core < 2) std::this_thread::yield();   // host wait: core 0 stays held
        OsStats w0;
        os_get_stats(&w0);
        GThread a = spawn(c, th_ch_a, 0, 10, 2, false);
        call(c, S_SETMASK, a.tt, 2ull, 7ull);   // ideal 2, may run anywhere
        call(c, S_START, a.tt);
        GThread p = spawn(c, th_ch_p, 0, 16, 0);
        for (;;) {   // both wait for a core: every core is held by a better thread
            OsStats w1;
            os_get_stats(&w1);
            if (w1.core_waits >= w0.core_waits + 2) break;
            std::this_thread::yield();
        }
        g_ch_go = 1;     // H2 exits (frees core 2) while this thread blocks in the join (frees core 0)
        join(c, p);
        join(c, a);
        join(c, h1);
        join(c, h2);
        CHECK(g_ch_p_ran == 1, "P did not run");
        g_ch_round++;
    }
    call(c, S_CHPRIO, me, (uint64_t)old);
    done = true;
    wd.join();
    OsStats s1;
    os_get_stats(&s1);
    CHECK(s1.core_wake_rescues == s0.core_wake_rescues, "%llu core waits were rescued by the safety-net timeout (lost handoff)",
          (unsigned long long)(s1.core_wake_rescues - s0.core_wake_rescues));
    printf("  core handoff: %d rounds, core_waits +%llu, rescues +%llu\n", ROUNDS, (unsigned long long)(s1.core_waits - s0.core_waits),
           (unsigned long long)(s1.core_wake_rescues - s0.core_wake_rescues));
}

// =========================================================================================== memory + sdk-internal shims
static void th_noop(Cpu *c) {}
static void test_memory(Cpu *c) {
    uint64_t out = obj(16);
    CHECK(call(c, "_ZN2nn2os21AllocateAddressRegionEPmm", out, 0x200000ull) == 0, "AllocateAddressRegion");
    uint64_t r = *(uint64_t *)(uintptr_t)out;
    CHECK(r != 0 && (r & 0xfff) == 0, "region %llx", (unsigned long long)r);
    CHECK(call(c, "_ZN2nn2os19AllocateMemoryPagesEmm", r + 0x10000, 0x20000ull) == 0, "AllocateMemoryPages");
    memset((void *)(uintptr_t)(r + 0x10000), 0xab, 0x20000);
    CHECK(call(c, "_ZN2nn2os19AllocateMemoryPagesEmm", r + 0x1ff000, 0x2000ull) != 0, "pages past the region fail");
    uint64_t usage = call(c, "_ZN2nn2os36GetVirtualAddressMemoryResourceUsageEv");
    CHECK(usage >= 0x200000 && c->x[1] >= 0x20000, "usage %llx/%llx", (unsigned long long)usage, (unsigned long long)c->x[1]);
    CHECK(call(c, "_ZN2nn2os15FreeMemoryPagesEmm", r + 0x10000, 0x20000ull) == 0, "FreeMemoryPages");
    // the VammManager member form (this in x0) used by sdk-internal callers
    CHECK(call(c, "_ZN2nn2os6detail11VammManager19AllocateMemoryPagesEmm", 0ull, r, 0x1000ull) == 0, "member form");
    *(volatile uint32_t *)(uintptr_t)r = 7;
    CHECK(call(c, "_ZN2nn2os17FreeAddressRegionEm", r) == 0, "FreeAddressRegion");
    CHECK(call(c, "_ZN2nn2os14AllocateMemoryEPmm", out, 0x5000ull) == 0, "AllocateMemory");
    *(volatile uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)out + 0x4ff8) = 1;
    // heap
    CHECK(call(c, "_ZN2nn2os17SetMemoryHeapSizeEm", 0x400000ull) == 0, "SetMemoryHeapSize");
    uint64_t hb = call(c, "_ZN2nn2os20GetMemoryHeapAddressEv");
    CHECK(hb && call(c, "_ZN2nn2os17GetMemoryHeapSizeEv") == 0x400000, "heap size");
    uint64_t b1 = obj(16), b2 = obj(16);
    CHECK(call(c, "_ZN2nn2os19AllocateMemoryBlockEPmm", b1, 0x200000ull) == 0, "block 1");
    CHECK(call(c, "_ZN2nn2os19AllocateMemoryBlockEPmm", b2, 0x200000ull) == 0, "block 2");
    CHECK(call(c, "_ZN2nn2os19AllocateMemoryBlockEPmm", obj(16), 0x1000ull) != 0, "heap full");
    memset((void *)(uintptr_t)*(uint64_t *)(uintptr_t)b2, 1, 0x200000);
    call(c, "_ZN2nn2os15FreeMemoryBlockEmm", *(uint64_t *)(uintptr_t)b1, 0x200000ull);
    CHECK(call(c, "_ZN2nn2os19AllocateMemoryBlockEPmm", b1, 0x1000ull) == 0 && *(uint64_t *)(uintptr_t)b1 == hb, "reuse");
    // TlsManager / ThreadManager internals (null `this`, as before OsResourceManager exists)
    uint64_t slot = obj(16);
    CHECK(call(c, "_ZN2nn2os6detail10TlsManager15AllocateTlsSlotEPNS0_7TlsSlotEPFvmEb", 0ull, slot, 0ull, 1ull) == 1,
          "TlsManager::AllocateTlsSlot");
    uint32_t sl = *(uint32_t *)(uintptr_t)slot;
    CHECK(sl >= 17 && sl <= 31, "sdk slot %u", sl);
    call(c, S_TLSSET, (uint64_t)sl, 55ull);
    call(c, "_ZN2nn2os6detail13ThreadManager26SetZeroToAllThreadsTlsSafeEi", 0ull, (uint64_t)sl);
    CHECK(call(c, S_TLSGET, (uint64_t)sl) == 0, "SetZeroToAllThreadsTlsSafe");
    call(c, S_TLSFREE, (uint64_t)sl);
    uint64_t tt = obj(TT_SIZE);
    CHECK(call(c, "_ZN2nn2os6detail13ThreadManager12CreateThreadEPNS0_10ThreadTypeEPFvPvES5_S5_mii", 0ull, tt,
               gfn(th_noop), 0ull, obj(0x4000), 0x4000ull, 16ull, 1ull) == 0, "ThreadManager::CreateThread");
    call(c, "_ZN2nn2os6detail13ThreadManager11StartThreadEPNS0_10ThreadTypeE", 0ull, tt);
    call(c, "_ZN2nn2os6detail13ThreadManager13DestroyThreadEPNS0_10ThreadTypeE", 0ull, tt);
    CHECK(call(c, "_ZN2nn2os26GetThreadAvailableCoreMaskEv") == 7, "available core mask");
}

// svcGetInfo through os_svc: every type the game requests (sdk callers listed in runtime/os/svc_getinfo.md)
static uint32_t getinfo(Cpu *c, uint64_t type, uint64_t handle, uint64_t sub, uint64_t *v) {
    c->x[1] = type; c->x[2] = handle; c->x[3] = sub;
    CHECK(os_svc(c, 0x29) == 1, "svc 0x29 handled");
    uint32_t r = (uint32_t)c->x[0];
    if (r == 0) *v = c->x[1];
    return r;
}
static void test_getinfo(Cpu *c) {
    const uint64_t P = 0xffff8001, G = 0;
    uint64_t v = 0, w = 0;
    CHECK(getinfo(c, 0, P, G, &v) == 0 && v == 7, "core mask %llx", (unsigned long long)v);
    CHECK(getinfo(c, 8, 0, G, &v) == 0 && v == 0, "debugger attached");
    CHECK(getinfo(c, 10, 0, (uint64_t)-1, &v) == 0 && v == 0, "idle tick, current core");
    CHECK(getinfo(c, 10, 0, 1, &v) == 0, "idle tick core 1");
    CHECK(getinfo(c, 10, 0, 9, &v) == 0xe001, "idle tick bad core");
    uint64_t e[4];
    for (int i = 0; i < 4; i++) CHECK(getinfo(c, 11, 0, (uint64_t)i, &e[i]) == 0, "entropy %d", i);
    CHECK(e[0] != e[1] || e[1] != e[2] || e[2] != e[3], "entropy not constant");
    CHECK(getinfo(c, 11, 0, 3, &v) == 0 && v == e[3], "entropy stable");
    CHECK(getinfo(c, 11, 0, 4, &v) == 0xe001, "entropy bad subtype");
    CHECK(getinfo(c, 18, P, G, &v) == 0 && v == 0x0100F4300BF2C000ull, "program id");
    CHECK(getinfo(c, 23, P, G, &v) == 0 && v == 1, "is application");
    CHECK(getinfo(c, 20, P, G, &v) == 0 && v != 0, "user exception context");
    CHECK(getinfo(c, 20, P, G, &w) == 0 && w == v, "user exception context stable");
    memset((void *)(uintptr_t)v, 0xab, 0x200);   // must be writable guest memory
    // regions: alias (2,3), heap (4,5), aslr (12,13), stack (14,15)
    for (uint32_t base : {2u, 4u, 12u, 14u}) {
        uint64_t a = 0, s = 0;
        CHECK(getinfo(c, base, P, G, &a) == 0 && getinfo(c, base + 1, P, G, &s) == 0 && a && s, "region %u", base);
        CHECK(a + s > a, "region %u does not wrap", base);
        uint64_t a2 = 0;
        CHECK(getinfo(c, base, P, G, &a2) == 0 && a2 == a, "region %u stable", base);
    }
    uint64_t ha, hs, aa, as;
    getinfo(c, 4, P, G, &ha); getinfo(c, 5, P, G, &hs);
    getinfo(c, 12, P, G, &aa); getinfo(c, 13, P, G, &as);
    CHECK(ha >= aa && ha + hs <= aa + as, "heap inside the aslr space");
    // the heap region is the one SetMemoryHeapSize commits from
    CHECK(call(c, "_ZN2nn2os17SetMemoryHeapSizeEm", 0x400000ull) == 0, "SetMemoryHeapSize");
    CHECK(call(c, "_ZN2nn2os20GetMemoryHeapAddressEv") == ha, "heap address agrees with GetInfo");
    uint64_t used = 0, nsused = 0, tot = 0, ntot = 0, sys = 0, sysused = 0;
    CHECK(getinfo(c, 22, P, G, &nsused) == 0 && nsused >= 0x400000, "used non-system %llx", (unsigned long long)nsused);
    getinfo(c, 21, P, G, &ntot); getinfo(c, 6, P, G, &tot); getinfo(c, 7, P, G, &used);
    getinfo(c, 16, P, G, &sys); getinfo(c, 17, P, G, &sysused);
    CHECK(sys != 0, "system resource size nonzero (VAMM enabled)");
    CHECK(tot == ntot + sys && used >= nsused && nsused <= ntot && sysused <= sys, "memory totals consistent");
    call(c, "_ZN2nn2os17SetMemoryHeapSizeEm", 0ull);
    // errors
    CHECK(getinfo(c, 999, P, G, &v) == 0x7801, "unknown type -> invalid enum value");
    CHECK(getinfo(c, 9, 0, G, &v) == 0x7801, "resource limit not implemented");
    CHECK(getinfo(c, 2, 0x1234, G, &v) == 0xe401, "bad handle");
    CHECK(getinfo(c, 2, P, 5, &v) == 0xe001, "bad subtype");
    // other trivial SVCs
    c->x[0] = 0;
    CHECK(os_svc(c, 0x1e) == 1 && c->x[0] != 0, "GetSystemTick");
    const char msg[] = "svc test string";
    c->x[0] = (uint64_t)(uintptr_t)msg; c->x[1] = sizeof msg - 1;
    CHECK(os_svc(c, 0x27) == 1 && c->x[0] == 0, "OutputDebugString");
    c->x[0] = 0x80000000u; c->x[1] = 0; c->x[2] = 0;
    CHECK(os_svc(c, 0x26) == 1 && c->x[0] == 0, "Break notification-only returns");
}

int main(int argc, char **argv) {
    OsConfig cfg{};
    cfg.trace = argc > 1 && !strcmp(argv[1], "-v");
    g_verbose = cfg.trace;
    os_init(&cfg);
    Cpu *c = (Cpu *)_aligned_malloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof(Cpu));
    os_adopt_main_thread(c, 0, -1);
    size_t n = 0;
    os_override_table(&n);
    printf("os_test: %zu overrides\n", n);
    struct { const char *name; void (*fn)(Cpu *); } tests[] = {
        {"time+layout", test_time},          {"threads+tls", test_threads},
        {"producer/consumer", test_producer_consumer},
        {"priority inversion", test_priority_inversion},
        {"fibers", test_fibers},             {"multiwait", test_multiwait},
        {"wake order", test_wake_order},     {"memory+boot", test_memory},
        {"core handoff", test_core_handoff},
        {"stress 10K", test_stress},
        {"svcGetInfo", test_getinfo},
    };
    int reps = argc > 2 ? atoi(argv[2]) : 1;
    for (int r = 0; r < reps; r++)
        for (auto &t : tests) {
            if (r > 0 && (t.fn == test_threads || t.fn == test_producer_consumer || t.fn == test_fibers ||
                          t.fn == test_wake_order || t.fn == test_memory || t.fn == test_core_handoff || t.fn == test_stress))
                continue;   // these use one-shot globals
            int f0 = g_fail;
            auto t0 = std::chrono::steady_clock::now();
            t.fn(c);
            printf("%-20s %s (%.0f ms)\n", t.name, g_fail == f0 ? "ok" : "FAILED", ms_since(t0));
            fflush(stdout);
        }
    OsStats s;
    os_get_stats(&s);
    printf("stats: threads %llu/%llu contended %llu cv_waits %llu core_waits %llu preempt %llu/%llu overflow %llu "
           "pi %llu fiber_sw %llu max_cores %d\n",
           (unsigned long long)s.threads_started, (unsigned long long)s.threads_exited,
           (unsigned long long)s.lock_contended, (unsigned long long)s.cv_waits, (unsigned long long)s.core_waits,
           (unsigned long long)s.preempt_requests, (unsigned long long)s.preempt_yields,
           (unsigned long long)s.overflow_grants, (unsigned long long)s.pi_boosts, (unsigned long long)s.fiber_switches,
           s.max_cores_in_use);
    CHECK(s.max_cores_in_use <= 3 + (int)s.overflow_grants, "cores in use %d", s.max_cores_in_use);
    CHECK(s.core_wake_rescues == 0, "%llu lost core wakeups rescued by the safety net", (unsigned long long)s.core_wake_rescues);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
