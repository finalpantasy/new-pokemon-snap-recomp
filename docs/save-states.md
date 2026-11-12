# Save and load state (retry a photo): feasibility study

Read-only study, 2026-11-12. Source reading only: `docs/horizon-os.md`, `runtime/os/`, `runtime/nvn/`, `docs/audio.md`, `docs/hle-services.md`, `analysis/runtime/threads.md`. No prototype, no game run. Effort numbers are estimates, not measurements.
Confidence: H = read in code or docs, M = reasoned from code, L = guess.

Goal (`AGENTS.md`, next step 7): two buttons, save state and load state, so a player can retry one photo. Session-only states. States are never written to disk and never cross a scene load.

## 1. Findings

1. Feasible, but it is a process-level checkpoint, not a memory copy. Lifted code keeps its call frames on host stacks (256 MiB per thread, 64 MiB per fiber), so a snapshot that copies only guest memory cannot work. (H)
2. The design that can work: park every guest thread at a point we control, copy the stacks and the guest memory, serialize or relocate the host runtime state, and keep every Vulkan object alive that was alive at snapshot time. (M)
3. Honest estimate: 3 to 4 weeks to a demo that works in one quiet scene (lab yard) with glitches. 7 to 10 weeks to something that holds across courses with audio. It may stay flaky in Wwise streaming. (L)
4. A much cheaper partial answer exists for "retry the course": the game's own reload plus the existing save fixtures. It does not give a mid-course retry.

## 2. What a state has to contain

| Part | Where it lives today | Snapshot method | Risk |
|---|---|---|---|
| Guest heap (`SetMemoryHeapSize`, one 16 GiB reservation, committed in 2 MiB units) | `runtime/os/os_memory.cpp` `g_heap_base`, `g_heap_size` | copy committed pages | size unknown, up to a few GiB |
| VAMM regions (`AllocateAddressRegion`, `AllocateMemoryPages`) | same file | copy committed pages of every region | region set must match |
| Module `.data` and `.bss` of `main`, `sdk` (and `rtld` if loaded) | loader, `host/loader.cpp` | copy | low |
| Guest thread stacks, TLS, TLR (0x200 bytes each, thread pointer at +0x1f0) | passed by the game, `os_kernel.cpp` | copy | low |
| Host stacks (one Win32 thread per guest thread, one Win32 fiber per guest fiber) | `_beginthreadex` (`os_kernel.cpp:819`), `ConvertThreadToFiberEx` / `SwitchToFiber` (`os_thread.cpp`) | copy used part plus saved registers | high, see section 4 |
| `nn::os` host state (keyed by guest address): wait queues, mutex owners, events, MultiWait maps, handle tables, timers | `os_kernel.cpp`, `os_sync.cpp` (`g_mw`, `g_obj_holders`, `g_handle_events`), all `std::` containers | serialize, or allocate from a snapshot arena | high |
| Guest time | `os_ticks()` in `os_kernel.cpp` (QPC based, `g_paused_total`) | set so ticks continue from the snapshot value | low |
| HLE state: open file handles and positions, hid, account, oe, settings | `runtime/hle/` | serialize | medium |
| Audio: AudioOut buffer queue (4 buffers of 1024 frames), sink ring, volumes | `runtime/audio/`, `docs/audio.md` | serialize queue, flush ring | medium |
| Wwise state | guest memory | comes with memory | high, streaming I/O in flight |
| NVN objects and the Vulkan objects behind them | `runtime/nvn/` (`PoolImpl`, `TextureImpl`, pipelines, descriptor tables, window slots) | journal plus deferred destroy | high |
| Save data on disk | `%APPDATA%\SnapRecomp\save\` | not restored, disk is the truth | medium |

## 3. Quiescing at a frame boundary

What exists:

- `host/window_opts.cpp` already blocks the game thread inside the present hook and freezes guest time (`os_time_pause`) when the window loses focus. That is half of a quiesce: the game thread stops at present and the clock stands still. (H)
- The scheduler has cooperative check-ins: `GetSystemTick`, `UnlockMutex`, `SignalEvent`, `SleepThread(0)`, `YieldThread`, every blocking call, and `os_spin_gate` / `os_poll`. A thread that skips them gets an overflow core after 2 ms (`docs/horizon-os.md` section 3). (H)
- There are 10 thread-creation sites. Five are Wwise (`AK::EventManager`, `AK::BankManager`, `AK::IOThread`, `AK::OpusDecodingThread`, `AK Suspended`). One fiber job scheduler sits at `0x548000-0x54f000`. (H, `analysis/runtime/threads.md`)

What is needed:

1. A request flag. At the next present, the game thread sets it. Every other thread parks at its next check-in or is already blocked in a runtime wait.
2. A rule that no thread is parked in the middle of a host call that cannot be repeated: a file read, a Vulkan submit, an audio call. Parking only at check-ins gives this for free, because those calls have no check-in inside them. (M)
3. A bound on the wait. If a thread does not reach a check-in in 100 ms, abort the snapshot and tell the player. Spin loops without a poll point are the known offenders (`docs/horizon-os.md` risk 1). (M)
4. GPU idle: `QueueFinish`, then readbacks flushed (`FlushReadbacks`) so no write-back is pending. Photo frames and the Evaluation passes must not be in flight. (H)
5. Refuse a snapshot while a save is being written (a counter in HLE `fs`) or while a scene load is running. (M)

## 4. The hard part: host stacks, fibers and host heap

Because lifted code runs as native C, a parked thread's state is its host stack plus registers. The stack holds pointers into the host heap (wait nodes, `std::` containers, lock guards) and into guest memory.

Why a plain copy fails: a thread blocked in a kernel wait (`SleepConditionVariable`, `WaitForSingleObject`) has live wait blocks on its own stack, linked into kernel or CRT structures. Overwriting that stack, or calling `SetThreadContext` on it, corrupts the wait. And after a restore, host heap objects that the stack points to may have been freed or moved.

Design that avoids this (all three parts are needed):

1. Every block goes through our own park function. Blocking waits in `os_kernel.cpp` already funnel through one wait path with a per-thread condition variable. Run that wait on a small side stack: capture the registers (`RtlCaptureContext`), switch to the side stack, wait there. While parked, the thread's real stack is idle and safe to read or overwrite. Restore means: copy the saved stack bytes back from another thread, then the thread resumes with `RtlRestoreContext` on the saved context. (M)
2. Replace Win32 fibers with our own context switch (a few dozen lines of assembly and a struct). A Win32 fiber hides its saved registers in an opaque heap structure we cannot snapshot reliably. Our own switch keeps registers in a struct we own. Cost: small; the job scheduler switches fibers often, so measure it. (M)
3. Keep host runtime state at fixed addresses. Route the `new` and `malloc` of our own runtime (`runtime/os`, `runtime/hle`, `runtime/audio`, the NVN layer's tables) to an arena at a fixed reserved address, and snapshot the arena bytes with the guest memory. Then pointers on restored stacks stay valid. Third-party DLLs (Vulkan driver, SDL, WASAPI, ImGui) keep their own heaps and are not touched. (M)

Things that must be forced to a known state first:

- Host mutexes and condition variables inside the arena must be unlocked with no waiters at snapshot time, or be replaced by our own types that hold only data.
- The scheduler's `G` lock must be free; parked threads wait on per-thread events, not on `G`.
- Thread set changes (a thread created or exited since the snapshot) reject the load. The fiber set may change only if the fiber stacks are also in the snapshot. (M)

Rejected alternatives:

- Windows process cloning (`RtlCloneUserProcess`): the clone keeps one thread and none of the GPU or window state. Not usable.
- Deterministic input replay from course start: thread timing, file I/O timing and tick-seeded random numbers make the game non-deterministic. Not usable. (M)
- Guest-memory-only snapshot with threads re-entering at loop heads: not possible with host-resident frames.

## 5. Guest time, input and files

- Time: guest time must continue from the snapshot value after a load, or the game sees a huge frame delta. `os_ticks()` already subtracts a paused span; a load sets `g_paused_total` so the returned tick equals the saved tick. Timers (`TimerEvent` deadlines) must be rebased the same way. (M)
- Input: the host only reports pad state, so nothing to restore. Hold buttons released across a load to avoid a stuck input. (M)
- Files: HLE file handles and positions are restored; reads are complete before any park because they contain no check-in. Files on disk are not rolled back. A photo saved after the snapshot stays on disk until the restored memory writes its own list over it (the game rewrites whole entries). Never snapshot or restore while a save commit is running. (M)
- RomFS and mod overlay are read-only, so nothing to do.

## 6. Audio

- The game uses AudioOut only. Wwise mixes in guest memory and hands 1024-frame buffers, 4 queued, to the sink at `main+0x124640` (`docs/audio.md`). (H)
- On save: record the queued buffer list and counters. On load: stop the WASAPI feed, drop the ring, reset the sink to the saved queue, restart. Expect a short silence or click. (M)
- Wwise streaming I/O (`AK::IOThread`) may have a request in flight whose completion is tracked in guest memory and in host file state. If the host completes or loses it differently, a voice can stall or loop. This is the largest unknown in audio. Mitigation: park the I/O thread only between requests (it blocks in `WaitConditionVariable` per the thread analysis, which is a safe point), and re-issue nothing. (L)

## 7. NVN and Vulkan

Facts from the layer (`runtime/nvn/`, `docs/nvn-vulkan-design.md`):

- Guest NVN objects are opaque and hold only a host pointer. A restored guest memory image therefore points to whatever host objects existed at snapshot time. (H)
- Pools are guest memory plus a lazy Vulkan mirror (`PoolImpl`), synced per range; ranges the GPU writes (skinning output) are GPU-owned and never copied back. (H)
- Syncs are timeline semaphores with monotonic values. A stale fence value is already signaled, so restoring guest memory that holds an old `{semaphore, value}` is safe. (M)
- Window textures are offscreen images with a round-robin slot index; the slot index is host state and must be restored with the snapshot. (M)

Plan:

1. Journal. Record every NVN object created and destroyed after a snapshot is armed (device-level counters in the layer's create and destroy paths; about 22 `vkDestroy*` sites in `nvn_vk.cpp` plus the texture, buffer, pool and pipeline paths).
2. Deferred destroy. While a snapshot exists, destroy requests move the Vulkan object to a zombie list instead of freeing it. The guest may restore a pointer to it. Zombies are released when the snapshot is replaced or dropped.
3. On load: destroy objects created after the snapshot (the restored guest no longer references them), resurrect zombies, restore the host tables that map ids to objects (texture and sampler pool tables, bound-state caches), and bump every cache epoch.
4. Invalidate everything derived from guest bytes: the pool mirrors (mark every range dirty and re-sync), texture upload hashes (`nvn_tex.cpp`), the ASTC to BC7 cache keys if keyed by address, the glyph atlas hash. A full re-sync of 1 to 2 GiB over PCIe is on the order of a second. (M)
5. GPU-only persistent content: render targets that the game fills once and reads later (static shadow maps, any baked probe) keep their current contents, not the snapshot ones. This is correct inside one scene and wrong across a scene load. So: a state records a scene epoch (a counter bumped on bulk texture destruction or course load) and load is refused when it differs. (M)
6. Memory cost: zombies and a second copy of pool data can double GPU memory in a bad case. Cap the number of states at one or two. (M)

Photo-specific:

- Evaluation and photo capture are one-shot GPU passes that write into guest memory by write-back. Snapshot only when the photo pipeline is idle (no photo select, no capture in flight); then a retry after load takes a fresh photo normally. (M)
- If DLSS is on, drop its history (`InReset = 1`) on load. Vector history from tier 1 of `docs/motion-vectors.md` is host state to clear too.

## 7a. Memory and time budget

- State size is the committed guest memory plus stacks. A Switch title is limited to about 3.2 GiB, so plan for up to that per slot. Measure the real number first (section 9, step 1). (L)
- Use `GetWriteWatch` on the guest regions (already planned for the pool sync in `analysis/perf/pass2_design.md`) to store later states as page deltas against a base. A save then copies only dirty pages. (M)
- Target: a save under 100 ms and a load under 1 second for the delta case; first full save a few hundred ms on a RAM copy. Not measured. (L)
- Keep states in RAM only. Compression is optional.

## 8. Riskiest parts, ranked

1. Host stacks and wait state of parked threads (side-stack park, own fiber switch, arena for runtime state). Wrong here means a hang or a crash after load, hard to debug.
2. Wwise streaming and the audio queue after a load. Wrong here means a stuck voice or a missing music loop.
3. GPU resource resurrection and cache invalidation. Wrong here means black or stale textures after a load.
4. Any thread that skips check-ins (spin loops) making quiesce time out.
5. Hidden host state outside the arena: static variables in third-party code, TLS slots, Windows handles owned by our runtime (timers, events). Needs a census of every `static`, `thread_local` and kernel handle in our runtime.

## 9. Order of work and estimate

1. Census, no snapshot yet (3 to 4 days): a debug command that requests a quiesce at the next present and prints each thread's state (running, blocked in an os wait, blocked in a host call), how long it took, committed guest bytes per region, and the list of statics and handles in our runtime. This proves or kills the quiesce idea cheaply.
2. Guest memory plus guest register and stack capture in the `tests/os` harness with fake threads (1 to 1.5 weeks): side-stack park, own fiber switch, restore with a re-run check.
3. Runtime state arena and `nn::os` and HLE serialization (2 to 3 weeks).
4. NVN journal, zombies, cache invalidation (1 to 2 weeks).
5. Audio, Wwise, UI buttons (hotkeys first, then a game-themed menu entry), tests across lab, yard and one course (2 weeks).

Total: roughly 7 to 10 weeks of focused work. First visible result in the lab yard after step 3 plus a minimal step 4, about 3 to 4 weeks. These are guesses; step 1 will sharpen them.

## 10. Recommended first step

Build the quiesce census (section 9, step 1) as a read-only debug hook behind an environment variable, run it once in the yard and once in a course, and record the thread table and memory sizes in `analysis/runtime/`. Wait until the photo and mod workers release `runtime/os` and `runtime/nvn`.
