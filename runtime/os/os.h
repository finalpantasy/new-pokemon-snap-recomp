// Host implementation of the Horizon nn::os layer for New Pokemon Snap (written for this project; the override
// convention follows TotK Recomp runtime/host.h @ 7a37fc3, MIT, Copyright (c) 2026 Valiantsin Dzerakh).
//
// Public C interface used by the loader, the SVC backstop, other HLE modules and the tests. Everything else in
// runtime/os is internal. Design, guest layouts and evidence: docs/horizon-os.md.
//
// Model in one paragraph: every guest thread is one host thread with its own Cpu, a guest stack (the one the guest
// passed to CreateThread) and a 0x200-byte guest Thread Local Region (TLR) that c->tpidrro points at. nn::os APIs are
// replaced by mangled symbol (os_override_table) with host functions that keep the real guest object layouts
// up to date. Blocking goes through one host scheduler lock; lock words keep Horizon's owner-handle encoding so that
// lifted sdk code that still touches them (inline arbitration in ReaderWriterLockImpl, IsLockedByCurrentThread)
// agrees with the host. A guest thread runs guest code only while it holds one of three virtual cores.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {   // cpu.h has no linkage guards; its rt_* services are C functions
#endif
#include "../cpu.h"

// ---- configuration (os_init). Zero-initialised fields take the defaults in brackets.
typedef struct OsConfig {
    int num_cores;               // virtual application cores [3]; -1 = unlimited (free host scheduling)
    int default_core;            // process default core, main.npdm META+0xF [0]
    int main_priority;           // nn::os priority of the adopted main thread; main.npdm META+0xE is kernel 44 -> [16]
    uint32_t overcommit_us;      // a ready thread that Horizon would run (it outranks a core holder, or round-robin is
                                 // due) and still has no core after this long runs on an overflow core [2000]
    uint32_t rr_slice_us;        // round-robin slice of the lowest priority (kernel 59 = nn::os 31) [10000]
    size_t host_stack_size;      // reserved host stack per guest thread; every guest BL is a host call [256 MiB]
    size_t fiber_host_stack;     // reserved host stack per guest fiber [64 MiB]
    uint64_t tp_init_var;        // guest address of sdk's nnmuslThreadPointerInitializer variable (0 = none)
    uint64_t tp_destroy_var;     // guest address of sdk's nnmuslThreadPointerDestroyer variable (0 = none)
    uint64_t tick_base;          // first GetSystemTick value [19.2e6 * 10, i.e. 10 s]
    int trace;                   // 1 = log thread starts/exits, overflow cores, unexpected states to stderr
} OsConfig;

void os_init(const OsConfig *cfg);   // once, before any other os_* call; cfg may be NULL

// ---- threads
// Makes the calling host thread a guest thread. tt = guest ThreadType address to use (0: the runtime allocates one
// and names it "MainThread"), prio = nn::os priority (-1: cfg main_priority). Sets c->tpidrro/c->thread and takes a
// virtual core. Use it for the guest main thread before nnMain / initializers.
void os_adopt_main_thread(Cpu *c, uint64_t tt, int prio);
// Releases the calling thread's virtual core while host code blocks outside guest code (GPU fence waits, file I/O,
// audio device waits). Must be paired, may nest. A host thread that is not a guest thread may call both freely.
void os_host_block_begin(void);
void os_host_block_end(void);
uint64_t os_current_thread_type(void);   // guest ThreadType* of the calling thread, 0 if none

// ---- polling hooks for the lifter and the runtime (proposed wiring in runtime/os/PROPOSED_SHARED_CHANGES.md)
void os_spin_gate(Cpu *c, uint64_t head);   // body of rt_spin_gate: a pass of a spin-wait back-edge
void os_poll(void);                         // honour a pending preemption request (cheap when none)
uint64_t os_ticks(void);                    // cntpct_el0: 19.2 MHz guest ticks; body of rt_ticks
#define OS_TICK_FREQUENCY 19200000ull

// ---- events for HLE services (vi vsync, audio device notification, account async, ...)
// Initialise a guest nn::os::SystemEventType as an intra-process event (state byte +0x28 = 1) and signal or clear it
// from any host thread, guest or not. autoclear: 1 = EventClearMode_AutoClear.
void os_system_event_create(uint64_t sev, int autoclear);
void os_system_event_signal(uint64_t sev);
void os_system_event_clear(uint64_t sev);
void os_event_signal(uint64_t ev);           // the same for a plain nn::os::EventType
// Readable-event handles an HLE service returns to the guest (e.g. a vsync event). The guest binds them with
// nn::os::AttachSystemEvent; os_handle_signal signals every SystemEvent attached to the handle (returns how many).
uint32_t os_handle_alloc(void);
int os_handle_signal(uint32_t handle);

// ---- SVC backstop helpers: return 1 and set c->x[0]/w1 like the kernel if the SVC is handled, else 0.
// Covers the thread and arbitration SVCs that lifted sdk code can still reach (see docs/horizon-os.md).
int os_svc(Cpu *c, uint32_t imm);

// ---- override table: mangled symbol -> host function (TotK HOST_OVERRIDE convention: x0..x7 in, x0 out)
typedef struct OsOverride { const char *sym; GuestFn fn; } OsOverride;
const OsOverride *os_override_table(size_t *count);
GuestFn os_override_find(const char *sym);   // NULL if not overridden

// ---- statistics (tests and TOTK_TRACE-style logs)
typedef struct OsStats {
    uint64_t threads_started, threads_exited;
    uint64_t lock_contended, cv_waits, core_waits, preempt_requests, preempt_yields, overflow_grants;
    uint64_t pi_boosts, fiber_switches, spin_yields;
    int max_cores_in_use;
    uint64_t core_wake_rescues;   // core waits ended by the safety-net timeout instead of a handoff (a lost wakeup; should stay 0)
} OsStats;
void os_get_stats(OsStats *out);

#ifdef __cplusplus
}
#endif
