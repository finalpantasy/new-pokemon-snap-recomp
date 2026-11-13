// nn::os threads, TLS slots, fibers and time as host overrides (written for this project).
// Fibers: each guest fiber has its own Cpu and a Win32 host fiber, because lifted code keeps its call frames on the
// host stack; the guest stack the game passes to InitializeFiber is still the fiber's guest sp. Thread-level
// registers (tpidrro_el0, tpidr_el0, FPCR/FPSR) follow the thread, not the fiber, as with Horizon's
// DispatchFiberContext (sdk+0x3483ec saves x19-x30, sp and q8-q15 only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "os_internal.h"

using namespace oslay;
using namespace osk;

namespace {

Thread *thread_of(uint64_t tt) {
    std::lock_guard<std::mutex> lk(G);
    Thread *t = find_tt(tt);
    if (!t) fatal("unknown ThreadType %#llx", (unsigned long long)tt);
    return t;
}
void check_prio(int p) {
    if (p < 0 || p > 31) fatal("nn::os thread priority %d out of range 0..31", p);
}

// ---- threads
void ov_CreateThread(Cpu *c, int ideal) {
    uint64_t tt = c->x[0], fn = c->x[1], arg = c->x[2], stack = c->x[3], size = c->x[4];
    int prio = (int)(int32_t)c->x[5];
    check_prio(prio);
    init_thread_type(tt, fn, arg, stack, size, prio, TS_INITIALIZED);
    thread_create(tt, fn, arg, prio, ideal);
    c->x[0] = 0;
}
void ov_CreateThread6(Cpu *c) { ov_CreateThread(c, -2); }
void ov_CreateThread7(Cpu *c) { ov_CreateThread(c, (int)(int32_t)c->x[6]); }
void ov_StartThread(Cpu *c) { thread_start(thread_of(c->x[0])); }
void ov_WaitThread(Cpu *c) { thread_wait_exit(thread_of(c->x[0])); }
void ov_TryWaitThread(Cpu *c) { c->x[0] = thread_of(c->x[0])->exited.load() ? 1 : 0; }
void ov_DestroyThread(Cpu *c) {
    uint64_t tt = c->x[0];
    thread_destroy(thread_of(tt));
    gst<uint8_t>(tt + TT_STATE, TS_NOT_INITIALIZED);
}
void ov_GetCurrentThread(Cpu *c) { c->x[0] = self()->tt; }
void ov_SetThreadName(Cpu *c) {
    uint64_t tt = c->x[0], p = c->x[1];
    if (!p) set_default_thread_name(tt);
    else set_thread_type_name(tt, (const char *)(uintptr_t)p);
}
void ov_SetThreadNamePointer(Cpu *c) {
    uint64_t tt = c->x[0], p = c->x[1];
    if (!p) set_default_thread_name(tt);
    else gst<uint64_t>(tt + TT_NAME_POINTER, p);
}
void ov_GetThreadNamePointer(Cpu *c) { c->x[0] = gld<uint64_t>(c->x[0] + TT_NAME_POINTER); }
void ov_GetThreadId(Cpu *c) { c->x[0] = gld<uint64_t>(c->x[0] + TT_THREAD_ID); }
void ov_GetThreadPriority(Cpu *c) { c->x[0] = (uint64_t)(int64_t)gld<int16_t>(c->x[0] + TT_BASE_PRIORITY); }
void ov_GetThreadCurrentPriority(Cpu *c) {
    Thread *t = thread_of(c->x[0]);
    std::lock_guard<std::mutex> lk(G);
    c->x[0] = (uint64_t)(int64_t)t->eff_prio;
}
void ov_ChangeThreadPriority(Cpu *c) {   // returns the previous base priority (sdk+0x341ee0)
    uint64_t tt = c->x[0];
    int p = (int)(int32_t)c->x[1];
    check_prio(p);
    Thread *t = thread_of(tt);
    int old = gld<int16_t>(tt + TT_BASE_PRIORITY);
    change_priority(t, p);
    gst<int16_t>(tt + TT_BASE_PRIORITY, (int16_t)p);
    c->x[0] = (uint64_t)(int64_t)old;
    poll_preempt(self());
}
void ov_SetThreadCoreMask(Cpu *c) {
    set_core_mask(thread_of(c->x[0]), (int)(int32_t)c->x[1], c->x[2]);
    poll_preempt(self());
}
void ov_GetThreadCoreMask(Cpu *c) {
    uint64_t pi = c->x[0], pm = c->x[1];
    Thread *t = thread_of(c->x[2]);
    std::lock_guard<std::mutex> lk(G);
    if (pi) gst<int32_t>(pi, t->ideal);
    if (pm) gst<uint64_t>(pm, t->mask);
}
void ov_GetCurrentCoreNumber(Cpu *c) {
    Thread *t = self();
    std::lock_guard<std::mutex> lk(G);
    c->x[0] = (uint64_t)guest_core_number(t);
}
void ov_YieldThread(Cpu *) { yield_thread(self()); }
void ov_SleepThread(Cpu *c) {   // TimeoutHelperImplByHorizon::Sleep sdk+0x34a020: 0 returns at once
    int64_t ns = (int64_t)c->x[0];
    Thread *t = self();
    if (ns > 0) sleep_ns(t, ns);
    else poll_preempt(t);
}

// ---- TLS slots (TlsManager sdk+0x345050): user slots 0..15 upwards, sdk slots 31..17 downwards
std::mutex g_tlsmu;
uint64_t g_tls_dtor[TLS_SLOTS];
bool g_tls_used[TLS_SLOTS];

void tls_alloc(Cpu *c, bool sdk) {
    uint64_t out = c->x[0], dtor = c->x[1];
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(g_tlsmu);
        if (sdk) {
            for (int i = TLS_SDK_FIRST; i >= TLS_SDK_LAST && slot < 0; i--)
                if (!g_tls_used[i]) slot = i;
        } else {
            for (int i = TLS_USER_FIRST; i <= TLS_USER_LAST && slot < 0; i++)
                if (!g_tls_used[i]) slot = i;
        }
        if (slot >= 0) {
            g_tls_used[slot] = true;
            g_tls_dtor[slot] = dtor;
        }
    }
    if (slot < 0) {
        c->x[0] = RESULT_OUT_OF_RESOURCE;
        return;
    }
    for (uint64_t tt : all_thread_types()) gst<uint64_t>(tt + TT_TLS + 8 * (uint64_t)slot, 0);   // SetZeroToAllThreadsTlsSafe
    gst<uint32_t>(out, (uint32_t)slot);
    c->x[0] = 0;
}
void ov_AllocateTlsSlot(Cpu *c) { tls_alloc(c, false); }
void ov_SdkAllocateTlsSlot(Cpu *c) { tls_alloc(c, true); }
void ov_FreeTlsSlot(Cpu *c) {
    uint32_t s = (uint32_t)c->x[0];
    if (s >= TLS_SLOTS) fatal("FreeTlsSlot(%u)", s);
    std::lock_guard<std::mutex> lk(g_tlsmu);
    g_tls_used[s] = false;
    g_tls_dtor[s] = 0;
}
void ov_GetTlsValue(Cpu *c) {
    c->x[0] = gld<uint64_t>(gld<uint64_t>(c->tpidrro + TLR_THREAD) + TT_TLS + 8 * (uint64_t)(uint32_t)c->x[0]);
}
void ov_SetTlsValue(Cpu *c) {
    gst<uint64_t>(gld<uint64_t>(c->tpidrro + TLR_THREAD) + TT_TLS + 8 * (uint64_t)(uint32_t)c->x[0], c->x[1]);
}
void ov_GetUsedTlsSlotCount(Cpu *c) {
    std::lock_guard<std::mutex> lk(g_tlsmu);
    uint64_t n = 0;
    for (int i = TLS_USER_FIRST; i <= TLS_USER_LAST; i++) n += g_tls_used[i];
    c->x[0] = n;
}

// ---- fibers
std::mutex g_fmu;
std::unordered_map<uint64_t, Fiber *> g_fibers;

Fiber *fiber_find(uint64_t g) {
    std::lock_guard<std::mutex> lk(g_fmu);
    auto it = g_fibers.find(g);
    return it == g_fibers.end() ? nullptr : it->second;
}

void release_leaving(Thread *t) {
    if (Fiber *f = t->leaving) {
        t->leaving = nullptr;
        f->running.store(false, std::memory_order_release);
    }
}

void ensure_own(Thread *t) {
    if (!t->own_fiber) {
        t->own_fiber = alloc_guest(FB_SIZE);
        t->own_shadow = new Fiber;
        t->own_shadow->is_thread_ctx = true;
        t->own_shadow->guest = t->own_fiber;
        t->own_shadow->running = true;
    }
    if (!t->base_fiber) {
        t->base_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (!t->base_fiber) fatal("ConvertThreadToFiberEx failed (%lu)", GetLastError());
    }
    t->own_shadow->host = t->base_fiber;
    t->own_shadow->cpu = t->cpu;
}

void WINAPI fiber_proc(void *p);

// SwitchToFiber semantics of sdk+0x33f780: target 0 = the thread's own context.
void do_switch(Cpu *c, uint64_t target) {
    Thread *t = self_fresh();
    uint64_t tt = t->tt;
    ensure_own(t);
    if (!target) {
        target = gld<uint64_t>(tt + TT_THREAD_FIBER);
        if (!target) return;   // already on the thread's own context
    }
    uint64_t cur_g = gld<uint64_t>(tt + TT_CURRENT_FIBER);
    Fiber *from;
    if (!cur_g) {   // leaving the thread's own context: it becomes a fiber (sdk+0x33f808)
        if (target == t->own_fiber) return;
        uint64_t of = t->own_fiber;
        gst<uint8_t>(of + FB_STATE, FS_READY);
        gst<uint64_t>(of + FB_FUNCTION, gld<uint64_t>(tt + TT_FUNCTION));
        gst<uint64_t>(of + FB_ARGUMENT, gld<uint64_t>(tt + TT_ARGUMENT));
        gst<uint64_t>(of + FB_ORIGINAL_STACK, gld<uint64_t>(tt + TT_STACK));
        gst<uint64_t>(of + FB_STACK, gld<uint64_t>(tt + TT_STACK));
        gst<uint64_t>(of + FB_STACK_SIZE, gld<uint64_t>(tt + TT_STACK_SIZE));
        gst<uint64_t>(tt + TT_THREAD_FIBER, of);
        from = t->own_shadow;
    } else {
        from = cur_g == t->own_fiber ? t->own_shadow : fiber_find(cur_g);
        if (!from) fatal("current fiber %#llx has no host state", (unsigned long long)cur_g);
        gst<uint8_t>(cur_g + FB_STATE, FS_READY);
    }
    Fiber *to = target == t->own_fiber ? t->own_shadow : fiber_find(target);
    if (!to) fatal("SwitchToFiber(%#llx): not initialised", (unsigned long long)target);
    if (to == from) return;
    if (to->finished) fatal("SwitchToFiber(%#llx): its function returned", (unsigned long long)target);
    if (!to->is_thread_ctx && gld<uint8_t>(target + FB_STATE) != FS_READY && !to->running)
        fatal("SwitchToFiber(%#llx): state %u", (unsigned long long)target, gld<uint8_t>(target + FB_STATE));
    // Claim it. Another thread may still be leaving it (it published the fiber before its switch finished).
    for (bool exp = false; !to->running.compare_exchange_weak(exp, true, std::memory_order_acquire); exp = false)
        SwitchToThread();
    gst<uint8_t>(target + FB_STATE, FS_RUNNING);
    gst<uint64_t>(tt + TT_CURRENT_FIBER, target);
    Cpu *tc = to->cpu;
    tc->tpidrro = c->tpidrro;
    tc->tpidr = c->tpidr;
    tc->thread = c->thread;
    tc->fpcr = c->fpcr;
    tc->fpsr = c->fpsr;
    tc->excl_addr = ~0ull;
    t->cur = tc;
    t->cur_fiber = to->is_thread_ctx ? nullptr : to;
    t->leaving = from;
    if (!to->host) {
        to->host = CreateFiberEx(64 * 1024, g_cfg.fiber_host_stack, FIBER_FLAG_FLOAT_SWITCH, fiber_proc, to);
        if (!to->host) fatal("CreateFiberEx failed (%lu)", GetLastError());
    }
    g_stats.fiber_switches++;
    SwitchToFiber(to->host);
    // resumed, possibly on another host thread
    Thread *t2 = self_fresh();
    release_leaving(t2);
    if (from->is_thread_ctx) {   // back on the thread's own context (sdk+0x33f844)
        gst<uint64_t>(t2->tt + TT_CURRENT_FIBER, 0);
        gst<uint64_t>(t2->tt + TT_THREAD_FIBER, 0);
    }
    c->excl_addr = ~0ull;
    rt_fpcr_sync(c);
}

void WINAPI fiber_proc(void *p) {
    Fiber *f = (Fiber *)p;
    release_leaving(self_fresh());
    Cpu *c = f->cpu;
    uint64_t g = f->guest;
    c->sp = (gld<uint64_t>(g + FB_STACK) + gld<uint64_t>(g + FB_STACK_SIZE)) & ~15ull;
    c->x[29] = 0;   // CallFiberFunction sdk+0x348468 clears the frame pointer
    uint64_t next = call_guest(c, gld<uint64_t>(g + FB_FUNCTION), gld<uint64_t>(g + FB_ARGUMENT));
    f->finished = true;
    do_switch(c, next);   // sdk+0x33f6fc: switch to the returned fiber, 0 = the thread
    fatal("fiber %#llx resumed after its function returned", (unsigned long long)g);
}

void ov_InitializeFiber(Cpu *c) {
    uint64_t g = c->x[0], fn = c->x[1], arg = c->x[2], stack = c->x[3], size = c->x[4];
    gst<uint64_t>(g + FB_STACK_SIZE, size);
    gst<uint64_t>(g + FB_FUNCTION, fn);
    gst<uint64_t>(g + FB_ARGUMENT, arg);
    gst<uint8_t>(g + FB_STATE, FS_READY);
    gst<uint8_t>(g + FB_STACK_ALIASED, 0);   // no alias mapping on the host
    gst<uint64_t>(g + FB_ORIGINAL_STACK, stack);
    gst<uint64_t>(g + FB_STACK, stack);
    std::lock_guard<std::mutex> lk(g_fmu);
    Fiber *&f = g_fibers[g];
    if (!f) {
        f = new Fiber;
        f->cpu = alloc_cpu();
    } else {
        if (f->running) fatal("InitializeFiber(%#llx) on a running fiber", (unsigned long long)g);
        if (f->host) DeleteFiber(f->host);
        f->host = nullptr;
        memset(f->cpu, 0, sizeof(Cpu));
        f->cpu->excl_addr = ~0ull;
    }
    f->guest = g;
    f->finished = false;
}
void ov_FinalizeFiber(Cpu *c) {
    uint64_t g = c->x[0];
    Fiber *f = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_fmu);
        auto it = g_fibers.find(g);
        if (it != g_fibers.end()) {
            f = it->second;
            g_fibers.erase(it);
        }
    }
    if (f) {
        if (f->running) fatal("FinalizeFiber(%#llx) on a running fiber", (unsigned long long)g);
        if (f->host) DeleteFiber(f->host);
        free_cpu(f->cpu);
        delete f;
    }
    gst<uint8_t>(g + FB_STACK_ALIASED, 0);
    gst<uint64_t>(g + FB_STACK, gld<uint64_t>(g + FB_ORIGINAL_STACK));
    gst<uint8_t>(g + FB_STATE, FS_NONE);
}
void ov_SwitchToFiber(Cpu *c) { do_switch(c, c->x[0]); }
void ov_GetCurrentFiber(Cpu *c) { c->x[0] = gld<uint64_t>(gld<uint64_t>(c->tpidrro + TLR_THREAD) + TT_CURRENT_FIBER); }

// ---- time
void ov_GetSystemTick(Cpu *c) {
    c->x[0] = os_ticks();
    if (Thread *t = c->thread ? (Thread *)c->thread : nullptr) poll_preempt(t);
}
void ov_GetSystemTickFrequency(Cpu *c) { c->x[0] = OS_TICK_FREQUENCY; }
void ov_ConvertToTimeSpan(Cpu *c) { c->x[0] = (uint64_t)tick_to_ns((int64_t)c->x[0]); }
void ov_ConvertToTick(Cpu *c) { c->x[0] = (uint64_t)ns_to_tick((int64_t)c->x[0]); }


// ---- sdk-internal entry points that read OsResourceManager (GOT sdk+0xc88838), which the host never constructs.
// Members take `this` in x0; the host ignores it and shifts the arguments down.
template <GuestFn F> void member(Cpu *c) {
    for (int i = 0; i < 7; i++) c->x[i] = c->x[i + 1];
    F(c);
}
void ov_TlsManagerAllocate(Cpu *c) {   // TlsManager::AllocateTlsSlot(this, slot*, dtor, sdk) -> bool (sdk+0x57cf80)
    bool sdk = c->x[3] & 1;
    c->x[0] = c->x[1];
    c->x[1] = c->x[2];
    tls_alloc(c, sdk);
    c->x[0] = c->x[0] == 0;
}
void ov_SetZeroToAllThreadsTls(Cpu *c) {   // ThreadManager::SetZeroToAllThreadsTlsSafe(this, slot) sdk+0x344af0
    uint32_t s = (uint32_t)c->x[1];
    if (s >= TLS_SLOTS) fatal("SetZeroToAllThreadsTlsSafe(%u)", s);
    for (uint64_t tt : all_thread_types()) gst<uint64_t>(tt + TT_TLS + 8 * (uint64_t)s, 0);
}
void ov_SetInitialThreadName(Cpu *c) { set_default_thread_name(c->x[1]); }
void ov_GetThreadAvailableCoreMask(Cpu *c) {
    c->x[0] = g_cfg.app_cores > 0 && g_cfg.app_cores < 64 ? (1ull << g_cfg.app_cores) - 1 : 7;   // guest cores only (spare cores hidden)
}
void ov_QuickExit(Cpu *) {
    log("guest QuickExit / ExitProcess");
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
}
void ov_nnosInitialize(Cpu *) {}   // the host runtime is initialised by os_init before any guest code runs
}  // namespace

namespace osk {

void tls_run_destructors(Thread *t) {   // InvokeTlsDestructors sdk+0x344fa0
    uint64_t dtor[TLS_SLOTS];
    {
        std::lock_guard<std::mutex> lk(g_tlsmu);
        for (int i = 0; i < TLS_SLOTS; i++) dtor[i] = g_tls_used[i] ? g_tls_dtor[i] : 0;
    }
    for (int i = 0; i < TLS_SLOTS; i++) {
        uint64_t a = t->tt + TT_TLS + 8 * (uint64_t)i, v = gld<uint64_t>(a);
        if (dtor[i] && v) {
            gst<uint64_t>(a, 0);
            call_guest(t->cpu, dtor[i], v);
        }
    }
}

void fiber_thread_exit(Thread *t) {
    if (t->base_fiber) {
        ConvertFiberToThread();
        t->base_fiber = nullptr;
    }
    if (t->own_shadow) {
        delete t->own_shadow;
        t->own_shadow = nullptr;
    }
    if (t->own_fiber) {
        free_guest(t->own_fiber);
        t->own_fiber = 0;
    }
}

// table pieces for os_overrides.cpp
extern const OsOverride g_thread_overrides[];
extern const size_t g_thread_override_count;
const OsOverride g_thread_overrides[] = {
    {"_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi", ov_CreateThread6},
    {"_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii", ov_CreateThread7},
    {"_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE", ov_StartThread},
    {"_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE", ov_WaitThread},
    {"_ZN2nn2os13TryWaitThreadEPNS0_10ThreadTypeE", ov_TryWaitThread},
    {"_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE", ov_DestroyThread},
    {"_ZN2nn2os16GetCurrentThreadEv", ov_GetCurrentThread},
    {"_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc", ov_SetThreadName},
    {"_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc", ov_SetThreadNamePointer},
    {"_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE", ov_GetThreadNamePointer},
    {"_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE", ov_GetThreadId},
    {"_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE", ov_GetThreadPriority},
    {"_ZN2nn2os24GetThreadCurrentPriorityEPKNS0_10ThreadTypeE", ov_GetThreadCurrentPriority},
    {"_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi", ov_ChangeThreadPriority},
    {"_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim", ov_SetThreadCoreMask},
    {"_ZN2nn2os17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE", ov_GetThreadCoreMask},
    {"_ZN2nn2os20GetCurrentCoreNumberEv", ov_GetCurrentCoreNumber},
    {"_ZN2nn2os25GetCurrentProcessorNumberEv", ov_GetCurrentCoreNumber},
    {"_ZN2nn2os11YieldThreadEv", ov_YieldThread},
    {"_ZN2nn2os11SleepThreadENS_8TimeSpanE", ov_SleepThread},
    {"_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE", ov_AllocateTlsSlot},
    {"_ZN2nn2os18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE", ov_SdkAllocateTlsSlot},
    {"_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE", ov_FreeTlsSlot},
    {"_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", ov_GetTlsValue},
    {"_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", ov_SetTlsValue},
    {"_ZN2nn2os19GetUsedTlsSlotCountEv", ov_GetUsedTlsSlotCount},
    {"_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi", ov_InitializeFiber},
    {"_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", ov_FinalizeFiber},
    {"_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE", ov_SwitchToFiber},
    {"_ZN2nn2os15GetCurrentFiberEv", ov_GetCurrentFiber},
    {"_ZN2nn2os13GetSystemTickEv", ov_GetSystemTick},
    {"_ZN2nn2os20GetSystemTickOrderedEv", ov_GetSystemTick},
    {"_ZN2nn2os22GetSystemTickFrequencyEv", ov_GetSystemTickFrequency},
    {"_ZN2nn2os17ConvertToTimeSpanENS0_4TickE", ov_ConvertToTimeSpan},
    {"_ZN2nn2os13ConvertToTickENS_8TimeSpanE", ov_ConvertToTick},
    {"_ZN2nn2os26GetThreadAvailableCoreMaskEv", ov_GetThreadAvailableCoreMask},
    {"_ZN2nn2os9QuickExitEv", ov_QuickExit},
    {"nnosQuickExit", ov_QuickExit},
    {"nnosExitProcess", ov_QuickExit},
    {"nnosInitialize", ov_nnosInitialize},
    {"_ZN2nn2os6detail10TlsManager15AllocateTlsSlotEPNS0_7TlsSlotEPFvmEb", ov_TlsManagerAllocate},
    {"_ZN2nn2os6detail13ThreadManager26SetZeroToAllThreadsTlsSafeEi", ov_SetZeroToAllThreadsTls},
    {"_ZN2nn2os6detail13ThreadManager26SetInitialThreadNameUnsafeEPNS0_10ThreadTypeE", ov_SetInitialThreadName},
    {"_ZN2nn2os6detail13ThreadManager12CreateThreadEPNS0_10ThreadTypeEPFvPvES5_S5_mi", member<ov_CreateThread6>},
    {"_ZN2nn2os6detail13ThreadManager12CreateThreadEPNS0_10ThreadTypeEPFvPvES5_S5_mii", member<ov_CreateThread7>},
    {"_ZN2nn2os6detail13ThreadManager11StartThreadEPNS0_10ThreadTypeE", member<ov_StartThread>},
    {"_ZN2nn2os6detail13ThreadManager10WaitThreadEPNS0_10ThreadTypeE", member<ov_WaitThread>},
    {"_ZN2nn2os6detail13ThreadManager13TryWaitThreadEPNS0_10ThreadTypeE", member<ov_TryWaitThread>},
    {"_ZN2nn2os6detail13ThreadManager13DestroyThreadEPNS0_10ThreadTypeE", member<ov_DestroyThread>},
};
const size_t g_thread_override_count = sizeof g_thread_overrides / sizeof g_thread_overrides[0];

}  // namespace osk
