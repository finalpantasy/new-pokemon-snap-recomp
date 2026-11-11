# Proposed changes outside runtime/os

These changes are for the owners of `host/` and `runtime/` shared files. runtime/os does not edit those files itself. Most of the wiring is already in `host/boot.cpp`: `bind_host_layers`, `svc_dispatch` -> `os_svc`, `g_spin_gate_fn` = `os_spin_gate`, `g_ticks_fn` = `os_ticks`, and `os_adopt_main_thread` before the initializers. The items below are what is still missing.

## 1. Pass the musl thread-pointer hooks to OsConfig (host/boot.cpp, both os_init calls)

Without these, guest threads created through `nn::os::CreateThread` start with `tpidr_el0 = TLR[0x1f0] = 0`, so musl TLS and errno break on the first access. sdk's thread trampoline (sdk+0x348e80) calls the function stored in the data symbol `nnmuslThreadPointerInitializer` (sdk dynsym 0xd30c08) before the entry, and calls `nnmuslThreadPointerDestroyer` (0xd30c10) after it. runtime/os does the same when it knows where the variables are:

```cpp
OsConfig oc = {};
oc.tp_init_var    = lookup("nnmuslThreadPointerInitializer");   // address of the variable, not its value
oc.tp_destroy_var = lookup("nnmuslThreadPointerDestroyer");
```

`load()` runs before `os_init` today. Either call `os_init` after `load()` (no os call happens before then), or add a setter. If the setter is preferred, runtime/os will add `void os_set_thread_pointer_hooks(uint64_t init_var, uint64_t destroy_var);`.

## 2. Main thread: keep the os TLR

`os_adopt_main_thread(c, 0, -1)` replaces `c->tpidrro` with a TLR owned by runtime/os and sets TLR+0x1f8 itself. Two consequences:

- The fake ThreadType and TLR from `new_thread()` (host/loader.cpp:356) are unused once the main thread is adopted. They can be dropped for adopted threads.
- `boot.cpp` line 336 (`*(tpidrro+0x1f8) = os_current_thread_type()`) does nothing and is safe to remove. Line 335 (`TLR+0x1f0 = c->tpidr`) is still needed.

## 3. Resume flag after host-to-guest calls

runtime/os calls guest code in four places: the thread entry, TLS-slot destructors, the musl thread-pointer hooks and fiber functions. It does this through `rt_dispatch` with x30 set to a sentinel (`OS_RET_HOST`).

- **Exception escapes guest code:** `c->resuming` is set when control reaches runtime/os. runtime/os does not check it. It is treated as std::terminate, matching Horizon, where an uncaught exception in a thread entry aborts.
- **Wanted from the loader:** if the loader prefers a diagnostic, export `bool rt_take_resuming(Cpu*)` or similar. runtime/os will call it after each guest call and log the thread name.

## 4. Spin-loop polling in the lifter (recomp/patches.py `spin` list)

`rt_spin_gate` already forwards to `os_spin_gate` (`g_spin_gate_fn`). Its behavior:

- every pass runs `_mm_pause`;
- every 64 passes it yields the virtual core to an equal or better waiter;
- it always honors a pending preemption.

Loops that need it, in priority order:

1. `nn::os::BusyMutex` lock (sdk `LockBusyMutex`/`TryLockBusyMutex`). This is deliberately not overridden, because its state is a guest word that inline code also touches.
2. The job scheduler at main 0x548000-0x54f000. Its workers spin on LL/SC words before falling back to fibers.
3. LDAXR/STLXR retry loops that can fail forever when another thread holds a core: libc++ `__atomic` spin-waits and `std::call_once`.

Plain back-edges without a gate still progress. A thread that Horizon would run but that holds no core gets an overflow core after `overcommit_us` (2 ms). Without a gate, a spinning low-priority thread keeps its core until it blocks.

## 5. Build

`host/build_snap.py` globs `runtime/os/*.cpp`, so the new `os_memory.cpp` (VAMM and heap) is picked up with no change. The os sources must not be compiled with MSVC: `cpu.h` uses `__uint128_t`, and the os code uses GCC/clang `__atomic` builtins.

## 6. Precedence note for the nn::os table

All 197 names in `runtime/os/overrides.tsv` exist in sdk's dynsym; `nnos*` C wrappers are included only where they do more than branch. Two kinds of entry are worth knowing about:

- **`ThreadManager`, `TlsManager` and `VammManager` member functions:** they take `this` in x0, and the host ignores it. They are overridden because their lifted bodies dereference `OsResourceManager` (GOT sdk+0xc88838), which nothing constructs on the host. That null dereference is boot blocker 1 in `analysis/recomp/sdk_and_boot.md`.
- **`nn::os` entry points still lifted that read OsResourceManager:** `SuspendThread`, `ResumeThread`, `GetThreadContext`, `MapSharedMemory`, `MapTransferMemory`, `QueryMemoryInfo`, `AllocateUnsafeMemory`, the `InterProcessEvent`/`InterruptEvent` timed waits, and `nn::os::detail::TimeoutHelper::GetLeftTimeOnTarget`. None of them is on the boot path seen so far. Each would crash on the null singleton, so if a probe run reaches one, report it.
