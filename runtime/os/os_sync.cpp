// nn::os synchronization objects as host overrides over their guest layouts (written for this project).
// Objects whose sdk implementation sits on InternalCriticalSection/InternalConditionVariable (Mutex, ConditionVariable,
// Event, Semaphore, Barrier, MessageQueue, the Sdk* types) keep doing so here, with the same guest lock and key words
// (os_kernel.cpp cs_*/cv_*), so the guest-visible state matches what the lifted sdk would have written.
// LightEvent, TimerEvent, ReaderWriterLock and MultiWait keep their state in the guest fields but wait in host
// queues under the scheduler lock G. Layout evidence: os_layout.h.
#include <algorithm>

#include "os_internal.h"

using namespace oslay;
using namespace osk;

namespace {

constexpr uint32_t LIGHT_HANDOFF = 1;   // wake result: an AutoClear LightEvent signal was handed to this waiter
std::atomic<int> g_linked{0};           // linked MultiWait holders; 0 = notifications skip G
std::unordered_map<uint64_t, std::vector<uint64_t>> g_mw;            // MultiWait -> holders in link order
std::unordered_map<uint64_t, std::vector<uint64_t>> g_obj_holders;   // object -> linked holders

uint64_t tt_self() { return self()->tt; }

// ---- Mutex (sdk+0x340ff0..0x341160)
void ov_InitializeMutex(Cpu *c) {
    uint64_t m = c->x[0];
    cs_init(m + MX_CS);
    gst<uint8_t>(m + MX_RECURSIVE, (uint8_t)(c->x[1] & 1));
    gst<int32_t>(m + MX_LOCK_LEVEL, (int32_t)c->x[2]);
    gst<int32_t>(m + MX_NEST_COUNT, 0);
    gst<uint64_t>(m + MX_OWNER, 0);
    gst<uint8_t>(m + MX_STATE, 1);
}
void ov_FinalizeMutex(Cpu *c) { gst<uint8_t>(c->x[0] + MX_STATE, 0); }
void ov_LockMutex(Cpu *c) {
    uint64_t m = c->x[0], me = tt_self();
    if (!(gld<uint8_t>(m + MX_RECURSIVE) && gld<uint64_t>(m + MX_OWNER) == me)) cs_enter(m + MX_CS);
    gst<int32_t>(m + MX_NEST_COUNT, gld<int32_t>(m + MX_NEST_COUNT) + 1);
    gst<uint64_t>(m + MX_OWNER, me);
}
void ov_TryLockMutex(Cpu *c) {
    uint64_t m = c->x[0], me = tt_self();
    if (!(gld<uint8_t>(m + MX_RECURSIVE) && gld<uint64_t>(m + MX_OWNER) == me) && !cs_try_enter(m + MX_CS)) {
        c->x[0] = 0;
        return;
    }
    gst<int32_t>(m + MX_NEST_COUNT, gld<int32_t>(m + MX_NEST_COUNT) + 1);
    gst<uint64_t>(m + MX_OWNER, me);
    c->x[0] = 1;
}
void ov_UnlockMutex(Cpu *c) {
    uint64_t m = c->x[0];
    int32_t n = gld<int32_t>(m + MX_NEST_COUNT) - 1;
    gst<int32_t>(m + MX_NEST_COUNT, n);
    if (n == 0) {
        gst<uint64_t>(m + MX_OWNER, 0);
        cs_leave(m + MX_CS);
    }
    poll_preempt(self());
}
void ov_IsMutexLockedByCurrentThread(Cpu *c) { c->x[0] = gld<uint64_t>(c->x[0] + MX_OWNER) == tt_self(); }

// ---- ConditionVariable (sdk+0x33e9e0..0x33ebb0); Wait releases one nesting level, as the sdk does
void ov_InitializeConditionVariable(Cpu *c) {
    cs_init(c->x[0] + CV_KEY);
    gst<uint8_t>(c->x[0] + CV_STATE, 1);
}
void ov_FinalizeConditionVariable(Cpu *c) { gst<uint8_t>(c->x[0] + CV_STATE, 0); }
void ov_SignalConditionVariable(Cpu *c) { cv_signal(c->x[0] + CV_KEY); }
void ov_BroadcastConditionVariable(Cpu *c) { cv_broadcast(c->x[0] + CV_KEY); }
uint32_t cond_wait(uint64_t cv, uint64_t m, int64_t ns) {
    uint64_t me = tt_self();
    if (gld<uint64_t>(m + MX_OWNER) != me) fatal("WaitConditionVariable without owning mutex %#llx", (unsigned long long)m);
    int32_t n = gld<int32_t>(m + MX_NEST_COUNT) - 1;
    gst<int32_t>(m + MX_NEST_COUNT, n);
    if (n == 0) gst<uint64_t>(m + MX_OWNER, 0);
    uint32_t r = ns <= 0 && ns != NS_INFINITE ? R_TIMEOUT : R_OK;
    if (r == R_TIMEOUT) {   // TimeSpan 0: unlock and relock (sdk+0x33eb4c)
        cs_leave(m + MX_CS);
        cs_enter(m + MX_CS);
    } else {
        r = cv_wait(cv + CV_KEY, m + MX_CS, ns);
    }
    gst<int32_t>(m + MX_NEST_COUNT, gld<int32_t>(m + MX_NEST_COUNT) + 1);
    gst<uint64_t>(m + MX_OWNER, me);
    return r;
}
void ov_WaitConditionVariable(Cpu *c) { cond_wait(c->x[0], c->x[1], NS_INFINITE); }
void ov_TimedWaitConditionVariable(Cpu *c) {   // ConditionVariableStatus: 0 Timeout, 1 NoTimeout
    c->x[0] = cond_wait(c->x[0], c->x[1], (int64_t)c->x[2]) == R_TIMEOUT ? 0 : 1;
}

// ---- Event (sdk+0x33ed20..0x33f000)
void event_init(uint64_t e, bool signaled, int mode) {
    cs_init(e + EV_CS);
    cs_init(e + EV_CV);
    gst<uint64_t>(e + EV_HOLDERS, e);
    gst<uint64_t>(e + EV_HOLDERS + 8, e);
    gst<uint8_t>(e + EV_SIGNALED, signaled);
    gst<uint8_t>(e + EV_INITIALLY_SIGNALED, signaled);
    gst<uint8_t>(e + EV_CLEAR_MODE, (uint8_t)mode);
    gst<uint64_t>(e + EV_BROADCAST, 0);
    gst<uint8_t>(e + EV_STATE, 1);
}
void event_signal(uint64_t e) {
    cs_enter(e + EV_CS);
    if (!gld<uint8_t>(e + EV_SIGNALED)) {
        gst8a(e + EV_SIGNALED, 1);
        if (gld<uint8_t>(e + EV_CLEAR_MODE) == 1) {
            cv_signal(e + EV_CV);
        } else {
            gst<uint64_t>(e + EV_BROADCAST, gld<uint64_t>(e + EV_BROADCAST) + 1);
            cv_broadcast(e + EV_CV);
        }
        mw_notify(e);
    }
    cs_leave(e + EV_CS);
}
bool event_wait(uint64_t e, int64_t ns) {   // true = signalled
    cs_enter(e + EV_CS);
    bool ok = true;
    if (!gld<uint8_t>(e + EV_SIGNALED)) {
        uint64_t gen = gld<uint64_t>(e + EV_BROADCAST);
        if (ns == 0) {
            ok = false;
        } else {
            Deadline d = deadline_after(ns);
            while (!gld<uint8_t>(e + EV_SIGNALED) && gld<uint64_t>(e + EV_BROADCAST) == gen) {
                int64_t left = NS_INFINITE;
                if (!is_infinite(d))
                    left = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    d - std::chrono::steady_clock::now()).count());
                if (cv_wait(e + EV_CV, e + EV_CS, left) == R_TIMEOUT) {
                    ok = gld<uint8_t>(e + EV_SIGNALED) || gld<uint64_t>(e + EV_BROADCAST) != gen;
                    break;
                }
            }
        }
    }
    if (ok && gld<uint8_t>(e + EV_CLEAR_MODE) == 1) gst8a(e + EV_SIGNALED, 0);
    cs_leave(e + EV_CS);
    return ok;
}
bool event_try_wait(uint64_t e) {
    cs_enter(e + EV_CS);
    bool s = gld<uint8_t>(e + EV_SIGNALED);
    if (s && gld<uint8_t>(e + EV_CLEAR_MODE) == 1) gst8a(e + EV_SIGNALED, 0);
    cs_leave(e + EV_CS);
    return s;
}
void event_clear(uint64_t e) {
    cs_enter(e + EV_CS);
    gst8a(e + EV_SIGNALED, 0);
    cs_leave(e + EV_CS);
}
void ov_InitializeEvent(Cpu *c) { event_init(c->x[0], c->x[1] & 1, (int)(uint32_t)c->x[2]); }
void ov_FinalizeEvent(Cpu *c) { gst<uint8_t>(c->x[0] + EV_STATE, 0); mw_forget_object(c->x[0]); }
void ov_SignalEvent(Cpu *c) { event_signal(c->x[0]); poll_preempt(self()); }
// A wait that returns at once never reaches the scheduler, so a guest loop around it never gives up its virtual core.
// main+0x47d3e0 (LevelManager_LoadResource) loops on WaitEvent of a manual-clear Event that stays signalled while its
// queue is empty, for minutes, and pinned higher-priority threads got that core only through overflow grants (jungle
// ride at 7 fps, 2026-11-13). Such a wait is a check-in, and a manual-clear one that stays signalled is a spin pass.
// TryWait is a spin pass unless it consumed an AutoClear signal: a failed TryWait is the polling form of the same loop.
// SystemEvent waits run on the same Event fields and get the same treatment.
static void event_checkin(Cpu *c, bool spin) {
    if (legacy(LG_EV_SPIN)) return;
    if (spin) os_spin_gate(c, 0);   // polls preempt, yields on long spins
    else poll_preempt(self());
}
static bool manual_clear(uint64_t e) { return gld<uint8_t>(e + EV_CLEAR_MODE) != 1; }
static void wait_checked(Cpu *c, uint64_t e, int64_t ns, bool timed) {
    const bool spin = gld<uint8_t>(e + EV_SIGNALED) && manual_clear(e);   // returns at once and stays signalled
    const bool ok = event_wait(e, ns);
    if (timed) c->x[0] = ok;
    event_checkin(c, spin);
}
static void try_wait_checked(Cpu *c, uint64_t e) {
    const bool ok = event_try_wait(e);
    c->x[0] = ok;
    event_checkin(c, !ok || manual_clear(e));
}
void ov_WaitEvent(Cpu *c) { wait_checked(c, c->x[0], NS_INFINITE, false); }
void ov_TryWaitEvent(Cpu *c) { try_wait_checked(c, c->x[0]); }
void ov_TimedWaitEvent(Cpu *c) { wait_checked(c, c->x[0], (int64_t)c->x[1], true); }
void ov_ClearEvent(Cpu *c) { event_clear(c->x[0]); }

// ---- SystemEvent: always an intra-process Event here (state byte 1); inter-process events have no meaning on
// the host, so CreateSystemEvent(..., interProcess = true) also makes an intra-process one (docs/horizon-os.md).
void sev_check(uint64_t s) {
    uint8_t st = gld<uint8_t>(s + SE_STATE);
    if (st != 1) fatal("SystemEvent %#llx state %u (not created, or an inter-process handle)", (unsigned long long)s, st);
}
void ov_CreateSystemEvent(Cpu *c) {
    event_init(c->x[0], false, (int)(uint32_t)c->x[1]);
    gst<uint8_t>(c->x[0] + SE_STATE, 1);
    c->x[0] = 0;
}
void handle_forget(uint64_t s);
void ov_DestroySystemEvent(Cpu *c) {
    { Lock lk(G); handle_forget(c->x[0]); }
    gst<uint8_t>(c->x[0] + SE_STATE, 0);
    gst<uint8_t>(c->x[0] + EV_STATE, 0);
    mw_forget_object(c->x[0]);
}
void ov_SignalSystemEvent(Cpu *c) { sev_check(c->x[0]); event_signal(c->x[0]); }
void ov_WaitSystemEvent(Cpu *c) { sev_check(c->x[0]); wait_checked(c, c->x[0], NS_INFINITE, false); }
void ov_TryWaitSystemEvent(Cpu *c) { sev_check(c->x[0]); try_wait_checked(c, c->x[0]); }
void ov_TimedWaitSystemEvent(Cpu *c) { sev_check(c->x[0]); wait_checked(c, c->x[0], (int64_t)c->x[1], true); }
void ov_ClearSystemEvent(Cpu *c) { sev_check(c->x[0]); event_clear(c->x[0]); }
// AttachSystemEvent(sev, read handle, managed, write handle, managed, clear mode): the handle is one an HLE service
// returned (os_handle_alloc). The guest object becomes an intra-process event bound to the handle; the service
// signals it with os_handle_signal. Handle 0 / unknown handles still give a working (never signalled) event.
std::unordered_map<uint32_t, std::vector<uint64_t>> g_handle_events;   // under G
std::atomic<uint32_t> g_next_event_handle{0x20000000u};
void ov_AttachSystemEvent(Cpu *c) {
    uint64_t s = c->x[0];
    uint32_t h = (uint32_t)c->x[1] ? (uint32_t)c->x[1] : (uint32_t)c->x[3];
    event_init(s, false, (int)(uint32_t)c->x[5]);
    gst<uint8_t>(s + SE_STATE, 1);
    Lock lk(G);
    if (h) g_handle_events[h].push_back(s);
}
void handle_forget(uint64_t s) {   // under G
    for (auto it = g_handle_events.begin(); it != g_handle_events.end();) {
        auto &v = it->second;
        v.erase(std::remove(v.begin(), v.end(), s), v.end());
        it = v.empty() ? g_handle_events.erase(it) : std::next(it);
    }
}
void ov_GetIdleTickCount(Cpu *c) { c->x[0] = 0; }   // no idle accounting on the host

// ---- LightEvent (host queue K_LIGHT under G; word 2 = signalled)
void ov_InitializeLightEvent(Cpu *c) {
    uint64_t e = c->x[0];
    gst32a(e + LE_WORD, (c->x[1] & 1) ? 2 : 0);
    gst<uint8_t>(e + LE_AUTOCLEAR, (uint32_t)c->x[2] == 1);
    gst<uint32_t>(e + 8, 0);
    gst<uint8_t>(e + LE_STATE, 1);
}
void ov_FinalizeLightEvent(Cpu *c) { gst<uint8_t>(c->x[0] + LE_STATE, 0); }
void ov_SignalLightEvent(Cpu *c) {
    uint64_t e = c->x[0];
    Lock lk(G);
    uint64_t k = mkkey(K_LIGHT, e);
    if (gld<uint8_t>(e + LE_AUTOCLEAR)) {
        if (Thread *t = keyq_pop_best(k)) wake(t, LIGHT_HANDOFF);
        else gst32a(e + LE_WORD, 2);
    } else {
        gst32a(e + LE_WORD, 2);
        // Manual clear: every waiter woken here returns signalled, even if ClearLightEvent runs before it re-checks the
        // word (finding 11). Legacy: waiters re-check the word and can lose a Signal-then-Clear pulse.
        keyq_wake_all(k, legacy(LG_EVENTS) ? R_OK : LIGHT_HANDOFF);
    }
}
bool light_wait(uint64_t e, int64_t ns) {
    Thread *me = self();
    Lock lk(G);
    Deadline d = deadline_after(ns);
    for (;;) {
        if (gld32a(e + LE_WORD) == 2) {
            if (gld<uint8_t>(e + LE_AUTOCLEAR)) gst32a(e + LE_WORD, 0);
            return true;
        }
        if (ns == 0) return false;
        uint32_t r = block_on_key(me, lk, mkkey(K_LIGHT, e), W_KEY, d);
        if (r == LIGHT_HANDOFF) return true;
        if (r == R_TIMEOUT) return false;
    }
}
void ov_WaitLightEvent(Cpu *c) { light_wait(c->x[0], NS_INFINITE); }
void ov_TryWaitLightEvent(Cpu *c) { c->x[0] = light_wait(c->x[0], 0); }
void ov_TimedWaitLightEvent(Cpu *c) { c->x[0] = light_wait(c->x[0], (int64_t)c->x[1]); }
void ov_ClearLightEvent(Cpu *c) { Lock lk(G); gst32a(c->x[0] + LE_WORD, 0); }

// ---- Semaphore (sdk+0x3417e0..0x341a50)
void ov_InitializeSemaphore(Cpu *c) {
    uint64_t s = c->x[0];
    cs_init(s + SM_CS);
    cs_init(s + SM_CV);
    gst<uint64_t>(s + SM_HOLDERS, s);
    gst<uint64_t>(s + SM_HOLDERS + 8, s);
    gst32a(s + SM_COUNT, (int32_t)c->x[1]);
    gst<int32_t>(s + SM_MAX, (int32_t)c->x[2]);
    gst<uint8_t>(s + SM_STATE, 1);
}
void ov_FinalizeSemaphore(Cpu *c) { gst<uint8_t>(c->x[0] + SM_STATE, 0); mw_forget_object(c->x[0]); }
bool sem_acquire(uint64_t s, int64_t ns) {
    cs_enter(s + SM_CS);
    Deadline d = deadline_after(ns);
    while (gld32a(s + SM_COUNT) == 0) {
        if (ns == 0) { cs_leave(s + SM_CS); return false; }
        int64_t left = NS_INFINITE;
        if (!is_infinite(d))
            left = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            d - std::chrono::steady_clock::now()).count());
        if (cv_wait(s + SM_CV, s + SM_CS, left) == R_TIMEOUT && gld32a(s + SM_COUNT) == 0) {
            cs_leave(s + SM_CS);
            return false;
        }
    }
    gst32a(s + SM_COUNT, gld32a(s + SM_COUNT) - 1);
    cs_leave(s + SM_CS);
    return true;
}
void sem_release(uint64_t s, int n) {
    cs_enter(s + SM_CS);
    int32_t v = gld32a(s + SM_COUNT) + n;
    if (n <= 0 || v > gld<int32_t>(s + SM_MAX)) fatal("ReleaseSemaphore(%#llx, %d) over max", (unsigned long long)s, n);
    gst32a(s + SM_COUNT, v);
    if (n == 1) cv_signal(s + SM_CV);
    else cv_broadcast(s + SM_CV);
    mw_notify(s);
    cs_leave(s + SM_CS);
}
void ov_AcquireSemaphore(Cpu *c) { sem_acquire(c->x[0], NS_INFINITE); }
void ov_TryAcquireSemaphore(Cpu *c) { c->x[0] = sem_acquire(c->x[0], 0); }
void ov_TimedAcquireSemaphore(Cpu *c) { c->x[0] = sem_acquire(c->x[0], (int64_t)c->x[1]); }
void ov_ReleaseSemaphore(Cpu *c) { sem_release(c->x[0], 1); }
void ov_ReleaseSemaphoreN(Cpu *c) { sem_release(c->x[0], (int)(int32_t)c->x[1]); }
void ov_GetCurrentSemaphoreCount(Cpu *c) { c->x[0] = (uint64_t)(int64_t)gld32a(c->x[0] + SM_COUNT); }

// ---- Barrier (sdk+0x33e6e0)
void ov_InitializeBarrier(Cpu *c) {
    uint64_t b = c->x[0];
    cs_init(b + BR_CS);
    cs_init(b + BR_CV);
    gst<uint16_t>(b + BR_MAX, (uint16_t)c->x[1]);
    gst<uint16_t>(b + BR_ARRIVED, 0);
    gst<uint64_t>(b + BR_GENERATION, 0);
}
void ov_FinalizeBarrier(Cpu *c) { gst<uint16_t>(c->x[0] + BR_MAX, 0); }
void ov_IsBarrierInitialized(Cpu *c) { c->x[0] = gld<uint16_t>(c->x[0] + BR_MAX) != 0; }
void ov_AwaitBarrier(Cpu *c) {
    uint64_t b = c->x[0];
    cs_enter(b + BR_CS);
    uint64_t gen = gld<uint64_t>(b + BR_GENERATION);
    uint16_t n = (uint16_t)(gld<uint16_t>(b + BR_ARRIVED) + 1);
    if (n >= gld<uint16_t>(b + BR_MAX)) {
        gst<uint16_t>(b + BR_ARRIVED, 0);
        gst<uint64_t>(b + BR_GENERATION, gen + 1);
        cv_broadcast(b + BR_CV);
    } else {
        gst<uint16_t>(b + BR_ARRIVED, n);
        while (gld<uint64_t>(b + BR_GENERATION) == gen) cv_wait(b + BR_CV, b + BR_CS, NS_INFINITE);
    }
    cs_leave(b + BR_CS);
}

// ---- ReaderWriterLock (write-preferring; the write owner may also take read locks)
void ov_InitializeReaderWriterLock(Cpu *c) {
    uint64_t r = c->x[0];
    cs_init(r + RW_CS);
    gst<uint32_t>(r + RW_READERS, 0);
    gst<uint32_t>(r + RW_WRITE_COUNT, 0);
    gst<uint32_t>(r + RW_WAITING_WRITERS, 0);
    gst<uint64_t>(r + RW_OWNER, 0);
    cs_init(r + RW_CV_READ);
    cs_init(r + RW_CV_WRITE);
    gst<uint8_t>(r + RW_STATE, 1);
}
void ov_FinalizeReaderWriterLock(Cpu *c) { gst<uint8_t>(c->x[0] + RW_STATE, 0); }
bool rw_read(uint64_t r, bool try_only) {
    Thread *me = self();
    Lock lk(G);
    for (;;) {
        uint64_t owner = gld<uint64_t>(r + RW_OWNER);
        if (owner == me->tt || (!owner && !gld<uint32_t>(r + RW_WAITING_WRITERS))) {
            gst<uint32_t>(r + RW_READERS, gld<uint32_t>(r + RW_READERS) + 1);
            return true;
        }
        if (try_only) return false;
        block_on_key(me, lk, mkkey(K_RW, r), W_KEY, Deadline::max());
    }
}
bool rw_write(uint64_t r, bool try_only) {
    Thread *me = self();
    Lock lk(G);
    if (gld<uint64_t>(r + RW_OWNER) == me->tt) {
        gst<uint32_t>(r + RW_WRITE_COUNT, gld<uint32_t>(r + RW_WRITE_COUNT) + 1);
        return true;
    }
    if (try_only && (gld<uint64_t>(r + RW_OWNER) || gld<uint32_t>(r + RW_READERS))) return false;
    gst<uint32_t>(r + RW_WAITING_WRITERS, gld<uint32_t>(r + RW_WAITING_WRITERS) + 1);
    while (gld<uint64_t>(r + RW_OWNER) || gld<uint32_t>(r + RW_READERS))
        block_on_key(me, lk, mkkey(K_RW, r), W_KEY, Deadline::max());
    gst<uint32_t>(r + RW_WAITING_WRITERS, gld<uint32_t>(r + RW_WAITING_WRITERS) - 1);
    gst<uint64_t>(r + RW_OWNER, me->tt);
    gst<uint32_t>(r + RW_WRITE_COUNT, 1);
    return true;
}
void ov_AcquireReadLock(Cpu *c) { rw_read(c->x[0], false); }
void ov_TryAcquireReadLock(Cpu *c) { c->x[0] = rw_read(c->x[0], true); }
void ov_ReleaseReadLock(Cpu *c) {
    uint64_t r = c->x[0];
    Lock lk(G);
    uint32_t n = gld<uint32_t>(r + RW_READERS);
    if (!n) fatal("ReleaseReadLock(%#llx) without a read lock", (unsigned long long)r);
    gst<uint32_t>(r + RW_READERS, n - 1);
    if (n == 1) keyq_wake_all(mkkey(K_RW, r), R_OK);
}
void ov_AcquireWriteLock(Cpu *c) { rw_write(c->x[0], false); }
void ov_TryAcquireWriteLock(Cpu *c) { c->x[0] = rw_write(c->x[0], true); }
void ov_ReleaseWriteLock(Cpu *c) {
    uint64_t r = c->x[0];
    Thread *me = self();
    Lock lk(G);
    if (gld<uint64_t>(r + RW_OWNER) != me->tt) fatal("ReleaseWriteLock(%#llx) by a non-owner", (unsigned long long)r);
    uint32_t n = gld<uint32_t>(r + RW_WRITE_COUNT) - 1;
    gst<uint32_t>(r + RW_WRITE_COUNT, n);
    if (n == 0) {
        gst<uint64_t>(r + RW_OWNER, 0);
        keyq_wake_all(mkkey(K_RW, r), R_OK);
    }
}
void ov_IsReadLockHeld(Cpu *c) { c->x[0] = gld<uint32_t>(c->x[0] + RW_READERS) != 0; }
void ov_IsWriteLockHeldByCurrentThread(Cpu *c) {
    c->x[0] = gld<uint64_t>(c->x[0] + RW_OWNER) == tt_self() && gld<uint32_t>(c->x[0] + RW_WRITE_COUNT) != 0;
}
void ov_IsReaderWriterLockOwnerThread(Cpu *c) { c->x[0] = gld<uint64_t>(c->x[0] + RW_OWNER) == tt_self(); }

// ---- TimerEvent (state in the guest fields, lazily updated like the sdk's TimerEventHelper; under G)
void timer_update(uint64_t t) {   // under G
    uint8_t st = gld<uint8_t>(t + TE_TIMER_STATE);
    if (!st) return;
    int64_t now = (int64_t)now_ns(), next = gld<int64_t>(t + TE_NEXT);
    if (now < next) return;
    gst8a(t + TE_SIGNALED, 1);
    gst<uint64_t>(t + TE_BROADCAST, gld<uint64_t>(t + TE_BROADCAST) + 1);
    if (st == 2) {
        int64_t iv = std::max<int64_t>(gld<int64_t>(t + TE_INTERVAL), 1);
        next += ((now - next) / iv + 1) * iv;
        gst<int64_t>(t + TE_NEXT, next);
    } else {
        gst<uint8_t>(t + TE_TIMER_STATE, 0);
    }
}
void timer_notify(uint64_t t) {   // under G
    keyq_wake_all(mkkey(K_TIMER, t), R_OK);
    mw_notify_locked(t);
}
void ov_InitializeTimerEvent(Cpu *c) {
    uint64_t t = c->x[0];
    cs_init(t + TE_CS);
    cs_init(t + TE_CV);
    gst<uint64_t>(t + TE_HOLDERS, t);
    gst<uint64_t>(t + TE_HOLDERS + 8, t);
    gst<uint8_t>(t + TE_CLEAR_MODE, (uint8_t)c->x[1]);
    gst<uint8_t>(t + TE_SIGNALED, 0);
    gst<uint8_t>(t + TE_TIMER_STATE, 0);
    gst<uint64_t>(t + TE_BROADCAST, 0);
    gst<int64_t>(t + TE_NEXT, 0);
    gst<int64_t>(t + TE_FIRST, 0);
    gst<int64_t>(t + TE_INTERVAL, 0);
    gst<uint8_t>(t + TE_STATE, 1);
}
void ov_FinalizeTimerEvent(Cpu *c) {
    Lock lk(G);
    gst<uint8_t>(c->x[0] + TE_TIMER_STATE, 0);
    gst<uint8_t>(c->x[0] + TE_STATE, 0);
}
void timer_start(uint64_t t, int64_t first, int64_t interval, uint8_t mode) {
    Lock lk(G);
    int64_t now = (int64_t)now_ns();
    int64_t next = first > INT64_MAX - now ? INT64_MAX : now + first;   // SaturatedAdd sdk+0x33f9f4
    gst<int64_t>(t + TE_NEXT, next);
    gst<int64_t>(t + TE_FIRST, first);
    gst<int64_t>(t + TE_INTERVAL, interval);
    gst<uint8_t>(t + TE_TIMER_STATE, mode);
    timer_notify(t);   // waiters recompute their deadline
}
void ov_StartOneShotTimerEvent(Cpu *c) { timer_start(c->x[0], (int64_t)c->x[1], 0, 1); }
void ov_StartPeriodicTimerEvent(Cpu *c) { timer_start(c->x[0], (int64_t)c->x[1], (int64_t)c->x[2], 2); }
void ov_StopTimerEvent(Cpu *c) { Lock lk(G); gst<uint8_t>(c->x[0] + TE_TIMER_STATE, 0); }
void ov_SignalTimerEvent(Cpu *c) {
    Lock lk(G);
    gst8a(c->x[0] + TE_SIGNALED, 1);
    gst<uint64_t>(c->x[0] + TE_BROADCAST, gld<uint64_t>(c->x[0] + TE_BROADCAST) + 1);
    timer_notify(c->x[0]);
}
void ov_ClearTimerEvent(Cpu *c) { Lock lk(G); timer_update(c->x[0]); gst8a(c->x[0] + TE_SIGNALED, 0); }
bool timer_wait(uint64_t t, bool try_only) {
    Thread *me = self();
    Lock lk(G);
    // Manual clear: a signal (broadcast count bump) since this wait began counts even if ClearTimerEvent ran before the
    // woken waiter re-checked (finding 11; Horizon's TimerEvent compares the broadcast counter the same way).
    const bool manual = gld<uint8_t>(t + TE_CLEAR_MODE) != 1 && !legacy(LG_EVENTS);
    const uint64_t gen = gld<uint64_t>(t + TE_BROADCAST);
    for (;;) {
        timer_update(t);
        if (manual && gld<uint64_t>(t + TE_BROADCAST) != gen) return true;
        if (gld8a(t + TE_SIGNALED)) {
            if (gld<uint8_t>(t + TE_CLEAR_MODE) == 1) gst8a(t + TE_SIGNALED, 0);
            return true;
        }
        if (try_only) return false;
        Deadline d = Deadline::max();
        if (gld<uint8_t>(t + TE_TIMER_STATE)) {
            int64_t left = gld<int64_t>(t + TE_NEXT) - (int64_t)now_ns();
            d = deadline_after(std::max<int64_t>(left, 0));
        }
        block_on_key(me, lk, mkkey(K_TIMER, t), W_KEY, d);
    }
}
void ov_WaitTimerEvent(Cpu *c) { timer_wait(c->x[0], false); }
void ov_TryWaitTimerEvent(Cpu *c) { c->x[0] = timer_wait(c->x[0], true); }

// ---- MessageQueue (sdk+0x3402f0): ring buffer of uintptr_t
void ov_InitializeMessageQueue(Cpu *c) {
    uint64_t q = c->x[0];
    cs_init(q + MQ_CS);
    cs_init(q + MQ_CV_NOT_FULL);
    cs_init(q + MQ_CV_NOT_EMPTY);
    gst<uint64_t>(q + MQ_HOLDERS_NOT_FULL, q);
    gst<uint64_t>(q + MQ_HOLDERS_NOT_FULL + 8, q);
    gst<uint64_t>(q + MQ_HOLDERS_NOT_EMPTY, q + MQ_HOLDERS_NOT_EMPTY);
    gst<uint64_t>(q + MQ_HOLDERS_NOT_EMPTY + 8, q + MQ_HOLDERS_NOT_EMPTY);
    gst<uint64_t>(q + MQ_BUFFER, c->x[1]);
    gst<int32_t>(q + MQ_MAX, (int32_t)c->x[2]);
    gst32a(q + MQ_COUNT, 0);
    gst<int32_t>(q + MQ_OFFSET, 0);
    gst<uint8_t>(q + MQ_STATE, 1);
}
void ov_FinalizeMessageQueue(Cpu *c) { gst<uint8_t>(c->x[0] + MQ_STATE, 0); mw_forget_object(c->x[0]); }
// Waits on cv until pred() under the queue lock; false on timeout. The lock is held on return.
template <class P> bool mq_wait(uint64_t q, uint64_t cv, int64_t ns, P pred) {
    Deadline d = deadline_after(ns);
    while (!pred()) {
        if (ns == 0) return false;
        int64_t left = NS_INFINITE;
        if (!is_infinite(d))
            left = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            d - std::chrono::steady_clock::now()).count());
        if (cv_wait(cv, q + MQ_CS, left) == R_TIMEOUT && !pred()) return false;
    }
    return true;
}
bool mq_send(uint64_t q, uint64_t v, int64_t ns, bool jam) {
    cs_enter(q + MQ_CS);
    int32_t max = gld<int32_t>(q + MQ_MAX);
    if (!mq_wait(q, q + MQ_CV_NOT_FULL, ns, [&] { return gld32a(q + MQ_COUNT) < max; })) {
        cs_leave(q + MQ_CS);
        return false;
    }
    int32_t n = gld32a(q + MQ_COUNT), off = gld<int32_t>(q + MQ_OFFSET);
    uint64_t buf = gld<uint64_t>(q + MQ_BUFFER);
    if (jam) {
        off = (off + max - 1) % max;
        gst<int32_t>(q + MQ_OFFSET, off);
        gst<uint64_t>(buf + 8 * (uint64_t)off, v);
    } else {
        gst<uint64_t>(buf + 8 * (uint64_t)((off + n) % max), v);
    }
    gst32a(q + MQ_COUNT, n + 1);
    cv_broadcast(q + MQ_CV_NOT_EMPTY);
    mw_notify(q);
    cs_leave(q + MQ_CS);
    return true;
}
bool mq_receive(uint64_t q, uint64_t out, int64_t ns, bool peek) {
    cs_enter(q + MQ_CS);
    if (!mq_wait(q, q + MQ_CV_NOT_EMPTY, ns, [&] { return gld32a(q + MQ_COUNT) > 0; })) {
        cs_leave(q + MQ_CS);
        return false;
    }
    int32_t n = gld32a(q + MQ_COUNT), off = gld<int32_t>(q + MQ_OFFSET), max = gld<int32_t>(q + MQ_MAX);
    uint64_t v = gld<uint64_t>(gld<uint64_t>(q + MQ_BUFFER) + 8 * (uint64_t)off);
    if (out) gst<uint64_t>(out, v);
    if (!peek) {
        gst<int32_t>(q + MQ_OFFSET, (off + 1) % max);
        gst32a(q + MQ_COUNT, n - 1);
        cv_broadcast(q + MQ_CV_NOT_FULL);
        mw_notify(q);
    }
    cs_leave(q + MQ_CS);
    return true;
}
void ov_SendMessageQueue(Cpu *c) { mq_send(c->x[0], c->x[1], NS_INFINITE, false); }
void ov_TrySendMessageQueue(Cpu *c) { c->x[0] = mq_send(c->x[0], c->x[1], 0, false); }
void ov_TimedSendMessageQueue(Cpu *c) { c->x[0] = mq_send(c->x[0], c->x[1], (int64_t)c->x[2], false); }
void ov_JamMessageQueue(Cpu *c) { mq_send(c->x[0], c->x[1], NS_INFINITE, true); }
void ov_TryJamMessageQueue(Cpu *c) { c->x[0] = mq_send(c->x[0], c->x[1], 0, true); }
void ov_TimedJamMessageQueue(Cpu *c) { c->x[0] = mq_send(c->x[0], c->x[1], (int64_t)c->x[2], true); }
// Receive/Peek(uintptr_t *out, MessageQueueType *q, ...)
void ov_ReceiveMessageQueue(Cpu *c) { mq_receive(c->x[1], c->x[0], NS_INFINITE, false); }
void ov_TryReceiveMessageQueue(Cpu *c) { c->x[0] = mq_receive(c->x[1], c->x[0], 0, false); }
void ov_TimedReceiveMessageQueue(Cpu *c) { c->x[0] = mq_receive(c->x[1], c->x[0], (int64_t)c->x[2], false); }
void ov_PeekMessageQueue(Cpu *c) { mq_receive(c->x[1], c->x[0], NS_INFINITE, true); }
void ov_TryPeekMessageQueue(Cpu *c) { c->x[0] = mq_receive(c->x[1], c->x[0], 0, true); }
void ov_TimedPeekMessageQueue(Cpu *c) { c->x[0] = mq_receive(c->x[1], c->x[0], (int64_t)c->x[2], true); }

// ---- MultiWait (host registry under G; holders keep kind/object/owner/user data at their guest offsets)
bool holder_signaled(uint64_t h) {   // under G
    uint64_t o = gld<uint64_t>(h + MH_OBJECT);
    switch (gld<uint64_t>(h + MH_KIND)) {
    case HK_EVENT:
    case HK_SYSTEM_EVENT: return gld8a(o + EV_SIGNALED) != 0;
    case HK_SEMAPHORE: return gld32a(o + SM_COUNT) > 0;
    case HK_THREAD: return gld8a(o + TT_STATE) == TS_TERMINATED;
    case HK_TIMER: timer_update(o); return gld8a(o + TE_SIGNALED) != 0;
    case HK_MQ_NOT_FULL: return gld32a(o + MQ_COUNT) < gld<int32_t>(o + MQ_MAX);
    case HK_MQ_NOT_EMPTY: return gld32a(o + MQ_COUNT) > 0;
    default: fatal("MultiWait holder %#llx has kind %llu", (unsigned long long)h, (unsigned long long)gld<uint64_t>(h + MH_KIND));
    }
}
void holder_init(uint64_t h, uint64_t obj, HolderKind k) {
    gst<uint64_t>(h + MH_KIND, k);
    gst<uint64_t>(h + MH_MW_NODE, h + MH_MW_NODE);
    gst<uint64_t>(h + MH_MW_NODE + 8, h + MH_MW_NODE);
    gst<uint64_t>(h + MH_OBJ_NODE, h + MH_OBJ_NODE);
    gst<uint64_t>(h + MH_OBJ_NODE + 8, h + MH_OBJ_NODE);
    gst<uint64_t>(h + MH_MULTIWAIT, 0);
    gst<uint64_t>(h + MH_OBJECT, obj);
    gst<uint64_t>(h + MH_USER_DATA, 0);
}
void ov_HolderEvent(Cpu *c) { holder_init(c->x[0], c->x[1], HK_EVENT); }
void ov_HolderSystemEvent(Cpu *c) { sev_check(c->x[1]); holder_init(c->x[0], c->x[1], HK_SYSTEM_EVENT); }
void ov_HolderSemaphore(Cpu *c) { holder_init(c->x[0], c->x[1], HK_SEMAPHORE); }
void ov_HolderThread(Cpu *c) { holder_init(c->x[0], c->x[1], HK_THREAD); }
void ov_HolderTimer(Cpu *c) { holder_init(c->x[0], c->x[1], HK_TIMER); }
void ov_HolderMessageQueue(Cpu *c) {
    uint32_t w = (uint32_t)c->x[2];
    if (w != MQ_WAIT_NOT_FULL && w != MQ_WAIT_NOT_EMPTY) fatal("MessageQueueWaitType %u", w);
    holder_init(c->x[0], c->x[1], w == MQ_WAIT_NOT_FULL ? HK_MQ_NOT_FULL : HK_MQ_NOT_EMPTY);
}
void ov_HolderHandle(Cpu *c) {   // InitializeMultiWaitHolder(holder, native handle): the first event attached to it
    uint64_t s = 0;
    {
        Lock lk(G);
        auto it = g_handle_events.find((uint32_t)c->x[1]);
        if (it != g_handle_events.end()) s = it->second.front();
    }
    if (!s) fatal("InitializeMultiWaitHolder: native handle %#x has no attached SystemEvent", (unsigned)c->x[1]);
    holder_init(c->x[0], s, HK_SYSTEM_EVENT);
}
void erase_one(std::unordered_map<uint64_t, std::vector<uint64_t>> &m, uint64_t k, uint64_t v) {
    auto it = m.find(k);
    if (it == m.end()) return;
    auto p = std::find(it->second.begin(), it->second.end(), v);
    if (p != it->second.end()) it->second.erase(p);
    if (it->second.empty()) m.erase(it);
}
void unlink_locked(uint64_t h) {
    uint64_t mw = gld<uint64_t>(h + MH_MULTIWAIT);
    if (!mw) return;
    erase_one(g_mw, mw, h);
    erase_one(g_obj_holders, gld<uint64_t>(h + MH_OBJECT), h);
    gst<uint64_t>(h + MH_MULTIWAIT, 0);
    g_linked--;
}
void link_locked(uint64_t mw, uint64_t h) {
    if (gld<uint64_t>(h + MH_MULTIWAIT)) fatal("MultiWait holder %#llx is already linked", (unsigned long long)h);
    g_mw[mw].push_back(h);
    g_obj_holders[gld<uint64_t>(h + MH_OBJECT)].push_back(h);
    gst<uint64_t>(h + MH_MULTIWAIT, mw);
    g_linked++;
}
void ov_FinalizeMultiWaitHolder(Cpu *c) {
    Lock lk(G);
    unlink_locked(c->x[0]);
    gst<uint64_t>(c->x[0] + MH_KIND, HK_NONE);
}
void ov_LinkMultiWaitHolder(Cpu *c) {
    Lock lk(G);
    link_locked(c->x[0], c->x[1]);
    keyq_wake_all(mkkey(K_MULTI, c->x[0]), R_OK);   // a WaitAny already in progress re-checks the new holder (finding 13)
}
void ov_UnlinkMultiWaitHolder(Cpu *c) { Lock lk(G); unlink_locked(c->x[0]); }
void ov_UnlinkAllMultiWaitHolder(Cpu *c) {
    Lock lk(G);
    auto it = g_mw.find(c->x[0]);
    if (it == g_mw.end()) return;
    std::vector<uint64_t> v = it->second;
    for (uint64_t h : v) unlink_locked(h);
}
void ov_MoveAllMultiWaitHolder(Cpu *c) {   // (dst, src)
    Lock lk(G);
    auto it = g_mw.find(c->x[1]);
    if (it == g_mw.end()) return;
    std::vector<uint64_t> v = it->second;
    for (uint64_t h : v) {
        unlink_locked(h);
        link_locked(c->x[0], h);
    }
}
void ov_SetMultiWaitHolderUserData(Cpu *c) { gst<uint64_t>(c->x[0] + MH_USER_DATA, c->x[1]); }
void ov_GetMultiWaitHolderUserData(Cpu *c) { c->x[0] = gld<uint64_t>(c->x[0] + MH_USER_DATA); }
void ov_InitializeMultiWait(Cpu *c) {
    uint64_t mw = c->x[0];
    gst<uint64_t>(mw + MW_LIST, mw + MW_LIST);
    gst<uint64_t>(mw + MW_LIST + 8, mw + MW_LIST);
    gst<uint64_t>(mw + 0x18, 0);
    gst<uint64_t>(mw + 0x20, 0);
    cs_init(mw + MW_CS);
    gst<uint32_t>(mw + 0x2c, 0);
    gst<uint8_t>(mw + MW_STATE, 1);
}
void ov_FinalizeMultiWait(Cpu *c) {
    Lock lk(G);
    if (g_mw.count(c->x[0])) fatal("FinalizeMultiWait(%#llx) with linked holders", (unsigned long long)c->x[0]);
    gst<uint8_t>(c->x[0] + MW_STATE, 0);
}
uint64_t wait_any(uint64_t mw, int64_t ns) {   // the first signalled holder in link order, 0 on timeout
    Thread *me = self();
    Lock lk(G);
    Deadline user = deadline_after(ns);
    bool released = false;
    uint64_t result = 0;
    for (;;) {
        int64_t timer_left = INT64_MAX;
        auto it = g_mw.find(mw);
        if (it != g_mw.end())
            for (uint64_t h : it->second) {
                if (holder_signaled(h)) {
                    result = h;
                    goto done;
                }
                uint64_t o = gld<uint64_t>(h + MH_OBJECT);
                if (gld<uint64_t>(h + MH_KIND) == HK_TIMER && gld<uint8_t>(o + TE_TIMER_STATE))
                    timer_left = std::min<int64_t>(timer_left, gld<int64_t>(o + TE_NEXT) - (int64_t)now_ns());
            }
        if (ns == 0 || (!is_infinite(user) && std::chrono::steady_clock::now() >= user)) goto done;
        {
            Deadline d = user;
            if (timer_left != INT64_MAX) d = std::min(d, deadline_after(std::max<int64_t>(timer_left, 0)));
            uint64_t key = mkkey(K_MULTI, mw);
            me->woken = false;
            me->wkind = W_MULTI;
            keyq_push(key, me);
            if (!released) {
                release_core(me);
                released = true;
            }
            if (!wait_woken(me, lk, d)) {
                keyq_remove(key, me);
                me->wkind = W_NONE;
            }
        }
    }
done:
    if (released) acquire_core(me, lk);
    return result;
}
void ov_WaitAny(Cpu *c) { c->x[0] = wait_any(c->x[0], NS_INFINITE); }
void ov_TryWaitAny(Cpu *c) { c->x[0] = wait_any(c->x[0], 0); }
void ov_TimedWaitAny(Cpu *c) { c->x[0] = wait_any(c->x[0], (int64_t)c->x[1]); }

// ---- Sdk* types and the Internal* primitives under them
void ov_CsInit(Cpu *c) { cs_init(c->x[0]); }
void ov_CsEnter(Cpu *c) { cs_enter(c->x[0]); }
void ov_CsTryEnter(Cpu *c) { c->x[0] = cs_try_enter(c->x[0]); }
void ov_CsLeave(Cpu *c) { cs_leave(c->x[0]); }
void ov_CsIsLocked(Cpu *c) { c->x[0] = cs_locked_by_me(c->x[0]); }
void ov_SdkMutexLock(Cpu *c) {
    if (cs_locked_by_me(c->x[0])) fatal("SdkMutex %#llx locked twice", (unsigned long long)c->x[0]);
    cs_enter(c->x[0]);
}
void ov_SrmInit(Cpu *c) { cs_init(c->x[0]); gst<uint32_t>(c->x[0] + SRM_COUNT, 0); }
void ov_SrmLock(Cpu *c) {
    if (!cs_locked_by_me(c->x[0])) cs_enter(c->x[0]);
    gst<uint32_t>(c->x[0] + SRM_COUNT, gld<uint32_t>(c->x[0] + SRM_COUNT) + 1);
}
void ov_SrmTryLock(Cpu *c) {
    if (!cs_locked_by_me(c->x[0]) && !cs_try_enter(c->x[0])) { c->x[0] = 0; return; }
    gst<uint32_t>(c->x[0] + SRM_COUNT, gld<uint32_t>(c->x[0] + SRM_COUNT) + 1);
    c->x[0] = 1;
}
void ov_SrmUnlock(Cpu *c) {
    uint32_t n = gld<uint32_t>(c->x[0] + SRM_COUNT) - 1;
    gst<uint32_t>(c->x[0] + SRM_COUNT, n);
    if (!n) cs_leave(c->x[0]);
}
void ov_CvInit(Cpu *c) { cs_init(c->x[0]); }
void ov_CvSignal(Cpu *c) { cv_signal(c->x[0]); }
void ov_CvBroadcast(Cpu *c) { cv_broadcast(c->x[0]); }
void ov_CvWait(Cpu *c) { cv_wait(c->x[0], c->x[1], NS_INFINITE); }
// InternalConditionVariable::TimedWait(cs, const TimeoutHelper&): the helper holds the absolute target tick
// (sdk+0x33eb18..0x33eb38). Returns ConditionVariableStatus: 0 Timeout, 1 NoTimeout.
void ov_CvTimedWait(Cpu *c) {
    // Unsigned compare: a saturated (UINT64_MAX) or far target tick is a long wait, not an expired one (finding 6).
    const uint64_t target = gld<uint64_t>(c->x[2]), now = os_ticks();
    if (target <= now) {
        cs_leave(c->x[1]);
        cs_enter(c->x[1]);
        c->x[0] = 0;
        return;
    }
    const uint64_t left = std::min<uint64_t>(target - now, (uint64_t)INT64_MAX);
    c->x[0] = cv_wait(c->x[0], c->x[1], tick_to_ns((int64_t)left)) == R_TIMEOUT ? 0 : 1;
}
void ov_SdkCvWait(Cpu *c) { cv_wait(c->x[0], c->x[1], NS_INFINITE); }
void ov_SdkCvTimedWait(Cpu *c) {   // returns true when signalled (sdk+0x3413f0)
    int64_t ns = (int64_t)c->x[2];
    if (ns <= 0) {
        cs_leave(c->x[1]);
        cs_enter(c->x[1]);
        c->x[0] = 0;
        return;
    }
    c->x[0] = cv_wait(c->x[0], c->x[1], ns) == R_TIMEOUT ? 0 : 1;
}
uint32_t srm_cv_wait(uint64_t cv, uint64_t m, int64_t ns) {   // sdk+0x341440: only at nesting level 1
    if (gld<uint32_t>(m + SRM_COUNT) != 1) fatal("SdkConditionVariable wait on recursive mutex at level %u", gld<uint32_t>(m + SRM_COUNT));
    gst<uint32_t>(m + SRM_COUNT, 0);
    uint32_t r = cv_wait(cv, m, ns);
    gst<uint32_t>(m + SRM_COUNT, 1);
    return r;
}
void ov_SdkCvWaitR(Cpu *c) { srm_cv_wait(c->x[0], c->x[1], NS_INFINITE); }
void ov_SdkCvTimedWaitR(Cpu *c) {
    int64_t ns = (int64_t)c->x[2];
    c->x[0] = ns > 0 && srm_cv_wait(c->x[0], c->x[1], ns) != R_TIMEOUT;
}

}  // namespace

namespace osk {

void mw_notify_locked(uint64_t obj) {
    auto it = g_obj_holders.find(obj);
    if (it == g_obj_holders.end()) return;
    for (uint64_t h : it->second)
        if (uint64_t mw = gld<uint64_t>(h + MH_MULTIWAIT)) keyq_wake_all(mkkey(K_MULTI, mw), R_OK);
}
void mw_notify(uint64_t obj) {
    // The caller just stored the object's signalled state. Without a full fence that store can still sit in the store
    // buffer when g_linked is read, while a linker (locked increment) reads the old state: a lost wakeup (finding 4).
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_linked.load(std::memory_order_acquire) == 0) return;
    Lock lk(G);
    mw_notify_locked(obj);
}
void mw_forget_object(uint64_t obj) {
    Lock lk(G);
    auto it = g_obj_holders.find(obj);
    if (it == g_obj_holders.end()) return;
    std::vector<uint64_t> v = it->second;
    for (uint64_t h : v) unlink_locked(h);
}

extern const OsOverride g_sync_overrides[];
extern const size_t g_sync_override_count;
const OsOverride g_sync_overrides[] = {
    {"_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi", ov_InitializeMutex},
    {"_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE", ov_FinalizeMutex},
    {"_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", ov_LockMutex},
    {"_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE", ov_TryLockMutex},
    {"_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", ov_UnlockMutex},
    {"_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE", ov_IsMutexLockedByCurrentThread},
    {"_ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE", ov_InitializeConditionVariable},
    {"_ZN2nn2os25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE", ov_FinalizeConditionVariable},
    {"_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE", ov_SignalConditionVariable},
    {"_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE", ov_BroadcastConditionVariable},
    {"_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE", ov_WaitConditionVariable},
    {"_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE", ov_TimedWaitConditionVariable},
    {"_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE", ov_InitializeEvent},
    {"_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE", ov_FinalizeEvent},
    {"_ZN2nn2os11SignalEventEPNS0_9EventTypeE", ov_SignalEvent},
    {"_ZN2nn2os9WaitEventEPNS0_9EventTypeE", ov_WaitEvent},
    {"_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE", ov_TryWaitEvent},
    {"_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE", ov_TimedWaitEvent},
    {"_ZN2nn2os10ClearEventEPNS0_9EventTypeE", ov_ClearEvent},
    {"_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb", ov_CreateSystemEvent},
    {"_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE", ov_DestroySystemEvent},
    {"_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE", ov_SignalSystemEvent},
    {"_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE", ov_WaitSystemEvent},
    {"_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE", ov_TryWaitSystemEvent},
    {"_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE", ov_TimedWaitSystemEvent},
    {"_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE", ov_ClearSystemEvent},
    {"_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE", ov_AttachSystemEvent},
    {"_ZN2nn2os16GetIdleTickCountEv", ov_GetIdleTickCount},
    {"_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", ov_InitializeLightEvent},
    {"_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", ov_FinalizeLightEvent},
    {"_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", ov_SignalLightEvent},
    {"_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE", ov_WaitLightEvent},
    {"_ZN2nn2os17TryWaitLightEventEPNS0_14LightEventTypeE", ov_TryWaitLightEvent},
    {"_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE", ov_TimedWaitLightEvent},
    {"_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE", ov_ClearLightEvent},
    {"_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii", ov_InitializeSemaphore},
    {"_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE", ov_FinalizeSemaphore},
    {"_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE", ov_AcquireSemaphore},
    {"_ZN2nn2os19TryAcquireSemaphoreEPNS0_13SemaphoreTypeE", ov_TryAcquireSemaphore},
    {"_ZN2nn2os21TimedAcquireSemaphoreEPNS0_13SemaphoreTypeENS_8TimeSpanE", ov_TimedAcquireSemaphore},
    {"_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE", ov_ReleaseSemaphore},
    {"_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi", ov_ReleaseSemaphoreN},
    {"_ZN2nn2os24GetCurrentSemaphoreCountEPKNS0_13SemaphoreTypeE", ov_GetCurrentSemaphoreCount},
    {"_ZN2nn2os17InitializeBarrierEPNS0_11BarrierTypeEi", ov_InitializeBarrier},
    {"_ZN2nn2os15FinalizeBarrierEPNS0_11BarrierTypeE", ov_FinalizeBarrier},
    {"_ZN2nn2os12AwaitBarrierEPNS0_11BarrierTypeE", ov_AwaitBarrier},
    {"_ZN2nn2os20IsBarrierInitializedEPNS0_11BarrierTypeE", ov_IsBarrierInitialized},
    {"_ZN2nn2os26InitializeReaderWriterLockEPNS0_20ReaderWriterLockTypeE", ov_InitializeReaderWriterLock},
    {"_ZN2nn2os24FinalizeReaderWriterLockEPNS0_20ReaderWriterLockTypeE", ov_FinalizeReaderWriterLock},
    {"_ZN2nn2os15AcquireReadLockEPNS0_20ReaderWriterLockTypeE", ov_AcquireReadLock},
    {"_ZN2nn2os18TryAcquireReadLockEPNS0_20ReaderWriterLockTypeE", ov_TryAcquireReadLock},
    {"_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE", ov_ReleaseReadLock},
    {"_ZN2nn2os16AcquireWriteLockEPNS0_20ReaderWriterLockTypeE", ov_AcquireWriteLock},
    {"_ZN2nn2os19TryAcquireWriteLockEPNS0_20ReaderWriterLockTypeE", ov_TryAcquireWriteLock},
    {"_ZN2nn2os16ReleaseWriteLockEPNS0_20ReaderWriterLockTypeE", ov_ReleaseWriteLock},
    {"_ZN2nn2os14IsReadLockHeldEPKNS0_20ReaderWriterLockTypeE", ov_IsReadLockHeld},
    {"_ZN2nn2os30IsWriteLockHeldByCurrentThreadEPKNS0_20ReaderWriterLockTypeE", ov_IsWriteLockHeldByCurrentThread},
    {"_ZN2nn2os29IsReaderWriterLockOwnerThreadEPKNS0_20ReaderWriterLockTypeE", ov_IsReaderWriterLockOwnerThread},
    {"_ZN2nn2os20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE", ov_InitializeTimerEvent},
    {"_ZN2nn2os18FinalizeTimerEventEPNS0_14TimerEventTypeE", ov_FinalizeTimerEvent},
    {"_ZN2nn2os22StartOneShotTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanE", ov_StartOneShotTimerEvent},
    {"_ZN2nn2os23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_", ov_StartPeriodicTimerEvent},
    {"_ZN2nn2os14StopTimerEventEPNS0_14TimerEventTypeE", ov_StopTimerEvent},
    {"_ZN2nn2os16SignalTimerEventEPNS0_14TimerEventTypeE", ov_SignalTimerEvent},
    {"_ZN2nn2os15ClearTimerEventEPNS0_14TimerEventTypeE", ov_ClearTimerEvent},
    {"_ZN2nn2os14WaitTimerEventEPNS0_14TimerEventTypeE", ov_WaitTimerEvent},
    {"_ZN2nn2os17TryWaitTimerEventEPNS0_14TimerEventTypeE", ov_TryWaitTimerEvent},
    {"_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm", ov_InitializeMessageQueue},
    {"_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE", ov_FinalizeMessageQueue},
    {"_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm", ov_SendMessageQueue},
    {"_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm", ov_TrySendMessageQueue},
    {"_ZN2nn2os21TimedSendMessageQueueEPNS0_16MessageQueueTypeEmNS_8TimeSpanE", ov_TimedSendMessageQueue},
    {"_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm", ov_JamMessageQueue},
    {"_ZN2nn2os18TryJamMessageQueueEPNS0_16MessageQueueTypeEm", ov_TryJamMessageQueue},
    {"_ZN2nn2os20TimedJamMessageQueueEPNS0_16MessageQueueTypeEmNS_8TimeSpanE", ov_TimedJamMessageQueue},
    {"_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE", ov_ReceiveMessageQueue},
    {"_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE", ov_TryReceiveMessageQueue},
    {"_ZN2nn2os24TimedReceiveMessageQueueEPmPNS0_16MessageQueueTypeENS_8TimeSpanE", ov_TimedReceiveMessageQueue},
    {"_ZN2nn2os16PeekMessageQueueEPmPKNS0_16MessageQueueTypeE", ov_PeekMessageQueue},
    {"_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE", ov_TryPeekMessageQueue},
    {"_ZN2nn2os21TimedPeekMessageQueueEPmPKNS0_16MessageQueueTypeENS_8TimeSpanE", ov_TimedPeekMessageQueue},
    {"_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE", ov_InitializeMultiWait},
    {"_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE", ov_FinalizeMultiWait},
    {"_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE", ov_WaitAny},
    {"_ZN2nn2os10TryWaitAnyEPNS0_13MultiWaitTypeE", ov_TryWaitAny},
    {"_ZN2nn2os12TimedWaitAnyEPNS0_13MultiWaitTypeENS_8TimeSpanE", ov_TimedWaitAny},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE", ov_HolderEvent},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE", ov_HolderSystemEvent},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_13SemaphoreTypeE", ov_HolderSemaphore},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_10ThreadTypeE", ov_HolderThread},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_14TimerEventTypeE", ov_HolderTimer},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_16MessageQueueTypeENS0_20MessageQueueWaitTypeE", ov_HolderMessageQueue},
    {"_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEj", ov_HolderHandle},
    {"_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE", ov_FinalizeMultiWaitHolder},
    {"_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE", ov_LinkMultiWaitHolder},
    {"_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE", ov_UnlinkMultiWaitHolder},
    {"_ZN2nn2os24UnlinkAllMultiWaitHolderEPNS0_13MultiWaitTypeE", ov_UnlinkAllMultiWaitHolder},
    {"_ZN2nn2os22MoveAllMultiWaitHolderEPNS0_13MultiWaitTypeES2_", ov_MoveAllMultiWaitHolder},
    {"_ZN2nn2os26SetMultiWaitHolderUserDataEPNS0_19MultiWaitHolderTypeEm", ov_SetMultiWaitHolderUserData},
    {"_ZN2nn2os26GetMultiWaitHolderUserDataEPKNS0_19MultiWaitHolderTypeE", ov_GetMultiWaitHolderUserData},
    {"_ZN2nn2os12SdkMutexType10InitializeEv", ov_CsInit},
    {"_ZN2nn2os12SdkMutexType4LockEv", ov_SdkMutexLock},
    {"_ZN2nn2os12SdkMutexType7TryLockEv", ov_CsTryEnter},
    {"_ZN2nn2os12SdkMutexType6UnlockEv", ov_CsLeave},
    {"_ZNK2nn2os12SdkMutexType23IsLockedByCurrentThreadEv", ov_CsIsLocked},
    {"_ZN2nn2os21SdkRecursiveMutexType10InitializeEv", ov_SrmInit},
    {"_ZN2nn2os21SdkRecursiveMutexType4LockEv", ov_SrmLock},
    {"_ZN2nn2os21SdkRecursiveMutexType7TryLockEv", ov_SrmTryLock},
    {"_ZN2nn2os21SdkRecursiveMutexType6UnlockEv", ov_SrmUnlock},
    {"_ZNK2nn2os21SdkRecursiveMutexType23IsLockedByCurrentThreadEv", ov_CsIsLocked},
    {"_ZN2nn2os24SdkConditionVariableType4WaitERNS0_12SdkMutexTypeE", ov_SdkCvWait},
    {"_ZN2nn2os24SdkConditionVariableType4WaitERNS0_21SdkRecursiveMutexTypeE", ov_SdkCvWaitR},
    {"_ZN2nn2os24SdkConditionVariableType9TimedWaitERNS0_12SdkMutexTypeENS_8TimeSpanE", ov_SdkCvTimedWait},
    {"_ZN2nn2os24SdkConditionVariableType9TimedWaitERNS0_21SdkRecursiveMutexTypeENS_8TimeSpanE", ov_SdkCvTimedWaitR},
    {"_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon10InitializeEv", ov_CsInit},
    {"_ZN2nn2os6detail36InternalCriticalSectionImplByHorizonC1Ev", ov_CsInit},
    {"_ZN2nn2os6detail36InternalCriticalSectionImplByHorizonC2Ev", ov_CsInit},
    {"_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5EnterEv", ov_CsEnter},
    {"_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon8TryEnterEv", ov_CsTryEnter},
    {"_ZN2nn2os6detail36InternalCriticalSectionImplByHorizon5LeaveEv", ov_CsLeave},
    {"_ZNK2nn2os6detail36InternalCriticalSectionImplByHorizon23IsLockedByCurrentThreadEv", ov_CsIsLocked},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizon10InitializeEv", ov_CvInit},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizonC1Ev", ov_CvInit},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizonC2Ev", ov_CvInit},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizon6SignalEv", ov_CvSignal},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizon9BroadcastEv", ov_CvBroadcast},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizon4WaitEPNS1_23InternalCriticalSectionE", ov_CvWait},
    {"_ZN2nn2os6detail38InternalConditionVariableImplByHorizon9TimedWaitEPNS1_23InternalCriticalSectionERKNS1_13TimeoutHelperE", ov_CvTimedWait},
};
const size_t g_sync_override_count = sizeof g_sync_overrides / sizeof g_sync_overrides[0];

}  // namespace osk

// ---- C interface for HLE services
extern "C" void os_system_event_create(uint64_t sev, int autoclear) {
    event_init(sev, false, autoclear ? 1 : 0);
    gst<uint8_t>(sev + SE_STATE, 1);
}
extern "C" void os_system_event_signal(uint64_t sev) { event_signal(sev); }
extern "C" void os_system_event_clear(uint64_t sev) { event_clear(sev); }
extern "C" void os_event_signal(uint64_t ev) { event_signal(ev); }
extern "C" uint32_t os_handle_alloc(void) { return g_next_event_handle++; }
extern "C" int os_handle_signal(uint32_t handle) {
    std::vector<uint64_t> v;
    {
        Lock lk(G);
        auto it = g_handle_events.find(handle);
        if (it != g_handle_events.end()) v = it->second;
    }
    for (uint64_t s : v) event_signal(s);
    return (int)v.size();
}
