# Horizon nn::os on the host (runtime/os)

`runtime/os` replaces the sdk's `nn::os` layer with host code. It covers threads, scheduling, synchronization, fibers, time, TLS slots, and memory (VAMM and heap). Each function is a host override keyed by mangled symbol. The calling convention follows TotK's `HOST_OVERRIDE`: arguments arrive in guest x0-x7 and the result goes back in x0.

- **Table:** `os_override_table()` holds 197 entries, listed in `runtime/os/overrides.tsv`. Every name was checked against sdk's dynsym.
- **Tests:** `tests/os/os_test.cpp` has 145 host-only checks and uses no game data. Build it with `tests/os/build.bat`.
- **Integration:** the remaining loader work is in `runtime/os/PROPOSED_SHARED_CHANGES.md`.

Addresses below are sdk module offsets (`analysis/import/v262144-sdk.elf`) unless they say `main+`.

## 1. Design

| piece | file | model |
|---|---|---|
| scheduler, arbitration, SVC backstop | `os_kernel.cpp` | one global lock `G`; one host condition variable per guest thread; keyed wait queues (wake order: lower priority value first, then first come) |
| threads, TLS, fibers, time | `os_thread.cpp` | one Win32 thread per guest thread; one Win32 fiber per guest fiber |
| sync objects, MultiWait | `os_sync.cpp` | guest-layout objects; host state keyed by guest address |
| VAMM, heap | `os_memory.cpp` | VirtualAlloc reserve and commit |
| table | `os_overrides.cpp` | merged name to function map |

The scheduler shape (one lock, a condition variable per thread, priority-then-FIFO order) follows TotK Recomp `runtime/kernel.c` (MIT). No code was copied from it.

**Two override levels.**

- The public `nn::os` API is replaced, for example `LockMutex` and `WaitEvent`.
- The sdk-internal primitives under it are replaced too: `InternalCriticalSectionImplByHorizon`, `InternalConditionVariableImplByHorizon`, `SdkMutexType`, `SdkRecursiveMutexType`, `SdkConditionVariableType`, and the `ThreadManager`/`TlsManager`/`VammManager` members.

sdk calls its own exports through its PLT, so internal callers reach the host versions as well.

**Lock words keep Horizon's encoding.** A lock word holds the owner thread handle, OR'd with 0x40000000 when there are waiters. A condition-variable key is a u32 that is 1 while it has waiters. Because of this, sdk code that is still lifted and touches these words inline agrees with the host. Examples are the arbitration in `ReaderWriterLockImpl`, `IsLockedByCurrentThread`, and libc++'s pthread condvar, which reaches SVC 0x1d directly.

**SVC backstop.** `os_svc` handles these SVCs on the same queues:

- thread SVCs: 0x0b-0x10 and 0x25;
- arbitration SVCs: 0x1a ArbitrateLock, 0x1b ArbitrateUnlock, 0x1c WaitProcessWideKeyAtomic, 0x1d SignalProcessWideKey;
- 0x1e GetSystemTick.

**No OsResourceManager.** sdk keeps all `nn::os` state in `OsResourceManager` (GOT sdk+0xc88838). The host never constructs it.

- **Overridden:** every function on the boot path that dereferences it. The full xref list has 66 functions; it was made with a scratch ADRP/LDR scanner.
- **Still lifted:** the functions listed in `PROPOSED_SHARED_CHANGES.md` section 6. They would crash on the null singleton if reached.

This fixes boot blocker 1 in `analysis/recomp/sdk_and_boot.md`: `ThreadManager::SetZeroToAllThreadsTlsSafe` (sdk+0x344af0) on a null `this`.

## 2. Guest layouts and evidence

All offsets are in `runtime/os/os_layout.h`, with the instruction that proves each one. Summary:

| object | size | key fields (offset: meaning) | evidence |
|---|---|---|---|
| TLR (tpidrro_el0) | 0x200 | 0x0 IPC buffer, 0x100 disable count, 0x1d8 errno, 0x1f0 ELF thread pointer, 0x1f8 ThreadType* | trampoline sdk+0x348e80 |
| ThreadType | 0x1c0 | see the ThreadType rows below | sdk+0x344480 (CreateThread), sdk+0x3446a0 (InvokeThread) |
| FiberType | 0x58 | 0x20 state (1 ready, 2 running), 0x28 func, 0x30 arg, 0x38 original stack, 0x40 stack, 0x48 size, 0x50 saved context | sdk+0x33f6b0..0x33f860 |
| MutexType | 0x20 | 0 state, 1 recursive, 4 lock level, 8 nest count, 0x10 owner ThreadType*, 0x18 CS word | sdk+0x340ff0..0x341160 |
| ConditionVariableType | 8 | 0 state, 4 key | sdk+0x33e9e0 |
| EventType | 0x28 | 0 holder list, 0x10 signaled, 0x11 initially signaled, 0x12 clear mode, 0x13 state, 0x14 broadcast counter u64, 0x1c CS, 0x20 CV | sdk+0x33ed20 |
| SystemEventType | 0x30 | EventType plus 0x28 state (1 intra-process, 2 inter-process) | sdk+0x33ffd8 |
| LightEventType | 0xc | 0 word (2 = signaled), 4 autoclear, 5 state | sdk+0x33f040 |
| SemaphoreType | 0x28 | 0x14 count, 0x18 max, 0x1c CS, 0x20 CV | sdk+0x3417e0 |
| BarrierType | 0x14 | 0 max u16, 2 arrived u16, 4 generation u64, 0xc CS, 0x10 CV | sdk+0x33e6e0 |
| ReaderWriterLockType | 0x2c+ | 0 CS, 0x10 state, 0x18 write owner, 0x24/0x28 CVs (0x4-0xf are host counters) | sdk+0x3411f0, +0x3412e4 |
| TimerEventType | 0x40 | 0x13 timer state (0 stop, 1 one-shot, 2 periodic), 0x20 next expiry, 0x28 first, 0x30 interval | sdk+0x33f8d0, +0x33f9c0 |
| MessageQueueType | 0x48 | 0x20 buffer, 0x28 max, 0x2c count, 0x30 offset, 0x38 CS, 0x3c/0x40 CVs | sdk+0x3402f0 |
| MultiWaitHolderType | 0x40 | 0x28 owning MultiWait, 0x30 object, 0x38 user data (0 = host kind tag instead of the vtable) | sdk+0x340e8c, +0x340fb0 |

ThreadType fields:

| offset | meaning |
|---|---|
| 0x40 | state: 0 not initialized, 1 initialized, 2 destroyed before start, 3 started, 4 terminated |
| 0x44 | base priority (s16) |
| 0x48 / 0x50 / 0x58 | original stack, stack, stack size |
| 0x60 / 0x68 | argument, function |
| 0x70 / 0x78 | current fiber, the thread's own fiber |
| 0x80 | 32 TLS values |
| 0x180 / 0x1a0 | name buffer, name pointer |
| 0x1a8 / 0x1ac | CS, CV |
| 0x1b0 / 0x1b8 | handle, thread id |

**Confidence.**

- **High:** ThreadType, TLR, Mutex, Event, Semaphore, MessageQueue, TimerEvent and FiberType. Each offset was read from at least one load or store in the functions named above. The fiber and TLS offsets are also exercised by the tests.
- **Medium:** ReaderWriterLock counters (host-defined) and MultiWait list internals. The guest never reads those through anything other than the overridden API.

**Priorities.** nn::os priorities run 0..31, and kernel priority = nn::os priority + 28 (sdk+0x348d20). The main thread is kernel 44, from `main.npdm`, which is nn::os 16. The game's wrapper at main+0x8dea70 uses 31, 23, 16, 8 and 1.

**Thread creation.** As in svcCreateThread, a new thread is pinned to its ideal core (mask = 1 << ideal) until `SetThreadCoreMask` is called. The default ideal core is the npdm default core 0. The default name is `Thread_0x<entry hex>` (sdk+0x3445c0).

**Time.** `GetSystemTick` is 19.2 MHz counts. `ConvertToTimeSpan` and `ConvertToTick` are exact replicas of TickManager sdk+0x3451e0 and sdk+0x345270, including their clamps and round-up.

## 3. Scheduler model and how it approximates Horizon

- **Cores and stacks.** Every guest thread is a host thread with its own `Cpu`, a 256 MiB reserved host stack (every guest BL is a host call), the guest stack it was given, and a TLR. A guest thread runs guest code only while it holds one of three virtual cores, subject to its core mask.
- **Blocking.** A thread releases its core whenever it blocks: in a mutex, condvar, event, sleep, MultiWait, or `os_host_block_begin`. It takes a core back before returning to guest code.
- **Free core.** A free core goes to the best waiter whose mask allows it: lowest effective priority value first, then longest ready.
- **Core handoff (fixed 2026-11-12).** A waiter takes a free core only if it is that core's best waiter, so the best waiter of every free core must be woken after any change (core freed, waiter leaves the wait list, waiter priority or mask changes); `kick_free_cores` does this. Before the fix, a waiter that took one of two freed cores still counted as waiting while it handed on the other core's wakeup, so it woke itself and a core-pinned waiter slept forever with all cores free. That was the intermittent lab-intro hang (main thread 65, timer thread and audio thread in `run` with `waiting_core=1`, cores free). An untimed core wait now rechecks every `core_rescue_ms` (20 ms) as a safety net; any wait it ends is a missed handoff and is logged and counted in `OsStats::core_wake_rescues`, which should stay 0. The watchdog dump prints the core wait list and the waiter each free core is reserved for.
- **Preemption** is cooperative.
  - **Request:** a ready thread that outranks a core holder sets the holder's preempt flag. So does an equal-priority thread at priority 31 once the holder has run longer than the 10 ms round-robin slice (Horizon round-robins only kernel priority 59).
  - **Where the holder yields:** at its next poll point. Poll points are `GetSystemTick`, `UnlockMutex`, `SignalEvent`, `SleepThread(0)`, `YieldThread`, every blocking call, and `os_spin_gate`/`os_poll`.
  - **Overflow core:** if the holder does not yield within `overcommit_us` (2 ms), the waiter runs on an overflow core, so a non-polling spin loop cannot deadlock the game. This is counted in `overflow_grants`. The deadline restarts when the holder asked to yield changes (2026-11-13).
  - **Migration (2026-11-13, review finding 1):** a waiter that outranks no holder, but whose core is held by a thread that may run on a free core, asks that holder to move (Horizon's idle core pulls a suggested thread whose core has a next thread). At its poll point the holder moves straight to the free core and keeps running; the waiter takes the vacated core. A holder that does not poll gives the waiter an overflow core after 2 ms. A freed core that no waiter may use wakes the waiters that have such a migration. Before this, a pinned loader (p31, core 2) waited behind a JobWorker on core 2 for as long as the job ran, with core 0 idle.
  - **Priority drops (2026-11-13, finding 3):** when a holder's priority gets worse (end of a priority-inheritance boost or ChangeThreadPriority), the core waiters it no longer outranks are woken and the holder is asked to yield. Before, they noticed only at their 20 ms safety-net recheck.
  - **Yield to any (2026-11-13, finding 2):** `svcSleepThread(-2)` and a spin gate after 4,096 consecutive passes hand the core straight to its best waiter of any priority; the yielder does not preempt that thread for `yield_any_us` (1 ms). `svcSleepThread(0/-1)` and `YieldThread` still yield only to equal or better waiters.
  - **Blocking host work (2026-11-13, finding 9):** `Fs::read_file` and the VAMM retry backoff release the virtual core (`os_io_begin`/`os_io_end`), like a Horizon IPC wait.
  - All four are switchable back with `SNAP_SCHED_LEGACY` (`host\README.md` "Scheduler switches").
- **Priority inheritance.**
  - A thread waiting in ArbitrateLock is recorded on the lock owner. The owner's effective priority becomes the minimum of its base priority and its waiters' priorities. The boost propagates along chains (owner waiting on another lock) up to depth 64.
  - Unlocking hands the lock directly to the best waiter, like KConditionVariable::SignalToAddress.
  - A condvar waiter that is signalled while its mutex is held moves to that mutex's waiter list.
- **Wake order.** All waits wake in priority order, then FIFO. This covers condvar, LightEvent, timer, ReaderWriterLock, join and MultiWait queues. `test_wake_order` checks order 1, 8, 16, 23, 31.

**Where it differs from Horizon.**

- **Per-core run queues:** there are none. Horizon keeps per-core run queues and migrates threads; here a thread may take any free core its mask allows.
- **Preemption:** it happens at poll points, not at any instruction.
- **Time quanta:** the host OS decides time slicing between the host threads that hold cores.
- **Core numbers:** `GetCurrentCoreNumber` returns the virtual core held, so code that indexes per-core data stays consistent while a thread holds its core.

## 4. Spin loops and the lifter's back-edge poll

The lifter turns marked spin back-edges into `rt_spin_gate(c, head)`. The loader forwards that to `os_spin_gate`, which does the following:

- every pass runs `_mm_pause`;
- every 64 passes it yields to an equal or better waiter, and after 4,096 consecutive passes (no gap of about 3 ms) it yields to any waiter of its core;
- it always honors a pending preemption.

Spin loops that need the gate (see `PROPOSED_SHARED_CHANGES.md` section 4):

- `nn::os::BusyMutex`. It is deliberately left lifted; its word is touched inline.
- The job scheduler at main+0x548000..0x54f000.
- LL/SC retry loops in libc++ (`call_once`, atomic waits).

Without a gate, such a loop still makes progress: a thread Horizon would run gets an overflow core after 2 ms. The spinning thread still burns its core, though.

## 5. Fibers

The job scheduler switches fibers. Each guest fiber gets its own `Cpu` and a Win32 fiber with a 64 MiB reserved stack, because lifted code keeps its frames on the host stack. The fiber's guest stack is still the one the game passed to `InitializeFiber`.

**What is saved per fiber.** Horizon's DispatchFiberContext (sdk+0x3483ec) saves only x19-x30, sp and q8-q15. So tpidr, tpidrro, FPCR and FPSR follow the thread: the switch copies them into the target fiber's `Cpu`.

**Sdk semantics kept:**

- target 0 means the thread's own context;
- a fiber function returns the next fiber, and 0 returns to the thread;
- ThreadType+0x70 and +0x78 are cleared when the thread context resumes.

**Migration.** A fiber can migrate between threads. It is claimed with an atomic `running` flag, and the context that was switched away from is released only after the next context runs. That way a fiber is never resumed on two threads at once.

**Tests:** 2,000-round ping-pong between two fibers, and one fiber resumed by three different threads.

## 6. Memory

- **VAMM.**
  - `AllocateAddressRegion` reserves with `VirtualAlloc(MEM_RESERVE)`.
  - `AllocateMemoryPages` commits read/write pages inside a reserved region.
  - `FreeMemoryPages` decommits, and `FreeAddressRegion` releases.
  - `AllocateMemory` does reserve plus commit.
  - Result codes are the sdk's own: 0x1003 out of memory, 0x1803 out of address space, 0xe03 range not inside a region (VammManager sdk+0x346f00, +0x347630).
  - This fixes boot blockers 3-5. main's first request is a 0x200000-byte region.
- **Heap.**
  - `SetMemoryHeapSize` uses one 16 GiB reservation and commits up to the requested size, in 2 MiB units.
  - `AllocateMemoryBlock` is first fit and page aligned; `FreeMemoryBlock` returns a block.

## 7. Deviations from Horizon (deliberate)

- **SystemEvents** are always intra-process (state byte 1).
  - `AttachSystemEvent(handle)` binds the guest event to a host handle from `os_handle_alloc`. HLE services signal it with `os_handle_signal`.
  - `InitializeMultiWaitHolder(holder, handle)` resolves to the first event attached to that handle.
- **MultiWaitHolder.** The guest vtable slot at +0 holds a host kind tag. The holder lists live in a host registry, not in the guest intrusive lists.
- **Not supported:** `SuspendThread`, `ResumeThread`, `GetThreadContext`, InterProcessEvent and InterruptEvent waits, and shared or transfer memory mapping. They are not on the boot path.
- **QuickExit and ExitProcess** terminate the host process.
- **`GetIdleTickCount`** returns 0.
- **Alias stacks** are not created. `stack aliased` stays 0, and the guest stack address is used directly.

## 8. Tests (`tests/os/os_test.cpp`)

The harness's `rt_dispatch` calls `c->pc` as a host function. Thread entries and fiber functions in the test are therefore plain C++ functions, and every `nn::os` call goes through `os_override_find(mangled)`.

| case | what it checks |
|---|---|
| time+layout | tick frequency, ConvertToTimeSpan/ConvertToTick values and clamps, a 20 ms sleep, main ThreadType state, name, TLR+0x1f8 and priority 16 |
| threads+tls | state transitions, default name, pinning to the ideal core, running on core 2, TLS value and destructor |
| producer/consumer | 4 producers and 4 consumers at priorities 31/23/16/8/1 over an 8-slot MessageQueue, 20,000 items; exact count and sum; Jam/Peek order |
| priority inversion | low-priority L holds a mutex, high-priority H waits, medium M spins 600 ms on L's core; L's current priority is 1 while H waits, and H gets the lock about 0.2-3 ms after L is released (before M finishes) |
| fibers | ping-pong alternation and fiber field reset; migration across 3 threads |
| multiwait | timed WaitAny timeout, WaitAny on 3 events, link order, thread holder |
| wake order | 5 condvar waiters woken one at a time, in priority order |
| memory+boot | VAMM, heap, TlsManager/ThreadManager members with a null `this`, available core mask |
| core handoff | 300 rounds: cores 0 and 2 freed back to back while a better unpinned waiter sleeps; the core-0-pinned waiter must still run (hung on round 0-2 before the fix) and no wait may end by the safety net |
| sched migrate | 2 x 250 rounds: a p16 JobWorker-like thread (mask 7) on core 2 spins (no poll, then through the gate) on a flag that a p31 thread pinned to core 2 sets; core 0 is then freed. The p31 thread must run within 50 ms (before the fix: 300 ms, the spinner's own timeout; after: median 3 ms no poll, 0.01 ms gated) |
| sched yield any | 300 rounds: p23 holds a busy lock and is preempted at GetSystemTick by a p16 thread on the same core that spins on the lock through the gate; the spinner must get the lock (before: 276 of 300 stuck) |
| sched unboost | 60 rounds: a p31 holder boosted to p1 unlocks; the p8 core waiter must run within 3 ms median (before: 21 ms) |
| sched mw race / time mono / cv far / destroy race / event pulse / link wake / vamm block / dump busy / event spin | findings 4, 5, 6, 8, 11, 13, 9, 12 and the LoadResource event spin of `analysis\core\review_os.md` (vamm block needs `SNAP_FAULT_VAMM=0:7 OS_TEST_ONLY=vamm`; destroy race shows the old use-after-free only under `-fsanitize=address`; dump busy holds G and asserts the watchdog dump walks no thread; event spin times a pinned p16 thread behind a WaitEvent spinner, SNAP_SCHED_LEGACY=evspin restores the old path) |
| stress 10K | mutex 6x10K, Event and LightEvent ping-pong 10K, Semaphore 10K, condvar 10K, RW lock, Barrier 3x1000, Sdk* types, periodic timer |

**Result:** 145 checks, 0 failed, on 7 consecutive runs. One run takes about 4 s on a loaded PC.

**Sanitizers:** TSan is not available for clang on Windows, so the stress test relies on assertions over non-atomic shared data protected by the primitives under test. The stress test found one real bug, now fixed: a condvar waiter published its key word after releasing the mutex, which lost wakeups.

## 9. Risks

1. **Poll coverage.** A tight loop with no poll point holds a core until the 2 ms overflow. Frame pacing may need more gates in the job scheduler.
2. **Lifted nn::os code that reads OsResourceManager** (section 1) crashes if reached. The probe run will show which, if any, are on the game's path.
3. **Thread-pointer hooks.** Until the loader passes `nnmuslThreadPointerInitializer` and `nnmuslThreadPointerDestroyer` to `OsConfig`, new guest threads start with tpidr 0, which breaks musl TLS.
4. **Host stack size.** 256 MiB is reserved per thread and 64 MiB per fiber. Deep guest recursion is a host stack overflow. The job scheduler may create many fibers; each reserves address space but commits only what it uses.
5. **Inter-process events, interrupt events and shared memory** are unimplemented. HLE services must use `os_system_event_*` or `os_handle_*`.
6. **Timer precision** is the Windows high-resolution timer, about 0.5-1 ms. A periodic 60 Hz TimerEvent can drift by up to one period under load. Expiry catches up and never accumulates.
7. **Scheduling is not cycle accurate.** Races that happen to be benign on Switch timing may show up. The `SNAP_OS_TRACE` log and the `OsStats` counters are the first diagnostic.
