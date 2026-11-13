# Save and load state (retry a photo): feasibility study and design

Read-only study, updated 2026-11-12. Sources: the code under `runtime/os`, `runtime/nvn`, `runtime/hle`, `runtime/audio`, `host/`, plus `docs/horizon-os.md`, `docs/nvn-vulkan-design.md`, `docs/audio.md` and `analysis/runtime/threads.md`. No prototype was built and no game was run. Sizes and effort are estimates unless a line reference says otherwise.

Confidence tags: **H** = read in code or docs, **M** = reasoned from code, **L** = guess.

Goal (`AGENTS.md`, next step 7): a save state button and a load state button, so a player can retry one photo. States live in RAM, in the same process, for the current session only.

## Summary

- **Recommendation: go with conditions.** The plan is a process-level checkpoint of the guest threads, not a plain memory copy. Feasible, because the runtime already owns every point where a guest thread can stop. Hard, because lifted code keeps its call frames on host stacks.
- **Core mechanism:** drain the three virtual cores, park every guest thread at a "clean" point, copy guest memory and each thread's used host stack, and keep every host object alive that the saved guest memory can still point to.
- **First milestone (S, 2 to 3 days):** a drain-and-census debug hook that stops all guest threads after a present, measures how long that takes, records where each thread parked, and prints the bytes a state would hold. Game behavior stays unchanged.
- **Effort:** about 6 to 9 weeks to buttons that work in a course with audio (L).

## 1. What state exists

### 1.1 Guest memory

Guest addresses are host addresses (`runtime/os/os_memory.cpp:3`). Every region below is plain host memory that a save copies and a load writes back.

| Region | Where it comes from | Size | Conf. |
|---|---|---|---|
| Heap (`SetMemoryHeapSize`) | one 16 GiB reservation, committed in 2 MiB units (`os_memory.cpp:23-24`, `:132-153`) | game-chosen; the runtime reports a 4 GiB application budget (`os_memory.cpp:185`, `:227`) | H bound, L actual |
| VAMM regions (`AllocateAddressRegion`, `AllocateMemoryPages`) | reserve then commit per page (`os_memory.cpp:27-29`, `:41-97`) | counted in `g_committed_total`; system resource budget 1 GiB (`os_memory.cpp:186`) | H bound, L actual |
| `main` RW segment (.data + .bss) | mapped at its guest address (`host/loader.cpp:196-200`) | 0x1c306f8 = 28.2 MiB (PT_LOAD of `analysis/import/v262144-main.elf`) | H |
| `sdk` RW segment | same | 0x10af38 = 1.0 MiB (`v262144-sdk.elf`) | H |
| Main thread guest stack, TLS (64 KiB), TLR, ThreadType | `host/loader.cpp:359-362` | stack size passed to `new_thread` (`loader.cpp:355`), plus about 72 KiB | H |
| Other guest thread stacks | passed by the game from its heap (`os_kernel.cpp:786-787`) | inside the heap row | H |
| Guest objects on the host CRT heap: ThreadType (0x1c0), TLR (0x200), own-fiber FiberType (0x58) | `os_kernel.cpp:132-143`, `:234-235`, `:747-749`, `:987-990`; `os_thread.cpp:176`; sizes `os_layout.h:10`, `:21`, `:52` | under 1 KiB per thread | H |
| Alias region (2 GiB) and stack region (1 GiB) | reserved only, VAMM does not allocate from them (`os_memory.cpp:183-184`) | expected 0 committed | M |

Estimate for a course: 1.5 to 3.5 GiB committed, of which only part is touched. Nothing logs the real number today: `os_memory.cpp` has no log line and no run log records process memory. The census milestone measures it.

### 1.2 Guest threads and fibers

- Each guest thread is a host thread made with `_beginthreadex` and a 256 MiB reserved host stack (`os_kernel.cpp:815-822`, `os_internal.h:98`). Lifted code calls lifted code natively: a guest `BL` is a host call, and each lifted function caches guest registers in C locals (`generated/v262144/main/*.c`, for example `x0 = c->x[0], ... sp_ = c->sp` at function entry). The host stack therefore holds part of the guest state. A snapshot of guest memory alone cannot resume a thread. (H)
- Each thread has a `Cpu` (1 KiB, `runtime/cpu.h:9-26`) on the CRT heap (`os_kernel.cpp:125-131`) and a `Thread` record with its scheduling and wait state (`os_internal.h:45-81`). (H)
- Fibers: each guest fiber gets a `Cpu` and a Win32 fiber with a 64 MiB reserved stack (`os_thread.cpp:241`, `:283`). A fiber can resume on a different host thread than the one that suspended it (`os_thread.cpp:246`). Fibers are keyed by guest address in `g_fibers` (`os_thread.cpp:158-159`). (H)
- The game has 10 thread-creation sites, five of them Wwise, and one fiber job scheduler at `main+0x548000..0x54f000`. `InitializeFiber` has one call site; `FinalizeFiber` is called only from `0x4e3f70` and `0x54a4b0` (`analysis/runtime/threads.md:7-13`, `:88-94`, `:127`). So the fiber set is probably created once and stable inside a course. (M, the census checks it)

How the scheduler can quiesce them:

- A guest thread runs guest code only while it holds one of three virtual cores (`docs/horizon-os.md:93`; `acquire_core`, `os_kernel.cpp:433-531`).
- A core holder gives the core up at a check-in when its `preempt` flag is set (`poll_preempt`, `os_kernel.cpp:546-554`). Check-ins: `GetSystemTick` (`os_thread.cpp:319-322`), `os_poll` and `os_spin_gate` from lifted loops (`os_kernel.cpp:1057-1075`), `yield_thread` (`os_kernel.cpp:878-891`), and every blocking wait, which releases the core before it waits (`block_on_key`, `os_kernel.cpp:368-383`; `sleep_ns`, `os_kernel.cpp:852-876`).
- So "no core may be granted" plus "preempt every holder" stops all guest code. Two features have to be switched off during a drain: overflow grants after `overcommit_us` (`os_internal.h:96`) and unlimited mode `SNAP_OS_CORES=-1` (`os_kernel.cpp:965`). (M)
- The existing focus pause is not a full quiesce. `snap_window_on_present` (`host/window_opts.cpp:358-362`) blocks inside the present hook (`host/boot.cpp:367-368`), which runs with `queueMutex` held (`runtime/nvn/nvn_vk.cpp:1265-1269`) and, unless `SNAP_NVN_RELEASE_CORE` is set, with the virtual core held (`host/gen_nvn_bridge.py:83-85`, `:113`). Other threads keep running on the other two cores. (H)

### 1.3 Host runtime state

| State | Where | Kind | Conf. |
|---|---|---|---|
| Scheduler: handles, threads by ThreadType, keyed wait queues, core owners, core waiters, sequence counters | `os_kernel.cpp:39-47` | `std::` containers of `Thread*`, copyable by value | H |
| Per-thread wait state (`wkind`, `wkey`, `woken`, `core`, priorities) | `os_internal.h:52-75` | plain fields | H |
| MultiWait links, handle events | `os_sync.cpp:17-19`, `:189-190` | containers of guest addresses | H |
| Mutex, event, timer state | guest fields; the host only waits (`os_sync.cpp:5`, `:395-412`) | comes back with guest memory | H |
| Memory maps (regions, heap blocks) | `os_memory.cpp:27-29`, `:132-133` | containers | H |
| Guest time | `os_ticks()` = QPC minus paused span (`os_kernel.cpp:933-946`) | two counters | H |
| Timed waits | host `steady_clock` deadlines (`os_internal.h:131`, `os_kernel.cpp:99-104`) | on the waiting thread's stack | H |
| Open files and directories | `fs_core.cpp:427-429`, `OpenFile` and `OpenDir` (`:184-196`) | reads take an explicit offset (`runtime/hle/fs.cpp:86-94`), so a file has no position to restore | H |
| Save data | generations on disk plus a working copy; `restore_generation` exists (`fs_core.h:84-90`, `fs_core.cpp:197-215`) | disk | H |
| Input | `input::hub().sampling_number()` only grows (`runtime/hle/hid.cpp:47`) | nothing to restore | M |
| AudioOut | per slot: `submit` and `released` rings of guest buffer pointers, the buffer being played and its frame (`runtime/audio/audio_out.cpp:36-46`), consumed by `mix_slot` (`:299-328`) | small, copyable | H |
| Wwise | guest memory plus five guest threads; its file reads are synchronous `nn::fs` calls | comes with memory and threads | M |
| Random bytes for the game | `crypto_random_bytes` uses a host `mt19937_64` (`runtime/hle/services_sys.cpp:399-403`) | not restored; harmless | M |
| Mods, upscalers, perf counters, UI | `host/mods/*`, `runtime/nvn/upscale`, `runtime/os/perf.cpp`, `runtime/ui` | host only; history buffers must be reset on load | M |

### 1.4 NVN and Vulkan objects

- A guest NVN object holds only `{magic, impl}`, a host pointer to its implementation (`runtime/nvn/nvn_internal.h:42-58`). Restored guest memory therefore points at whatever host objects were alive at the save. Every such object must still exist at load time. (H)
- Command lists are one-shot: `CommandBufferEndRecording` allocates a `CmdList` and hands its pointer to the guest (`nvn_entry.cpp:427-436`); it is deleted when its submit retires (`nvn_vk.cpp:765`). (H)
- Pools are guest memory plus a lazy Vulkan mirror (`nvn_internal.h:138-158`). Every submit copies the bound ranges from guest memory again (`SyncPoolRange`, `nvn_vk.cpp:642-665`), except ranges the GPU writes (`gpuRanges`). (H)
- Textures own a `VkImage` whose contents exist only on the GPU (`nvn_internal.h:174-210`). Pitch-linear textures are re-hashed against guest bytes (`srcHash`, `nvn_tex_vk.cpp:137`); other textures are uploaded once. (H)
- Syncs are timeline semaphores with monotonic values (`nvn_internal.h:302-307`). (H)
- GPU-to-guest writes only happen through readbacks, landed by `FlushReadbacks` (`nvn_xfer.cpp:382`) from `WaitSync`, `QueueFinish` and each submit (`nvn_vk.cpp:1112-1122`). (H)

Two kinds of host fields have to be told apart:

| Kind | Examples | On load |
|---|---|---|
| Guest-API state: what the game set through NVN calls | texture and sampler pool slots (`nvn_internal.h:171-172`), `SyncImpl::value` and `armed`, `QueueImpl::persistDs` (`:313`), `CommandBufferImpl::recording` and `cmds` (`:128-136`), `WindowImpl::nextSlot` (`:340`) | restore the saved value |
| GPU truth: what the device actually holds | image layouts and `subLayout`, `uploaded`, pool mirrors, `gpuRanges`, queue timeline values, swapchain and acquire semaphores | keep the current value |

## 2. Recommended design: session-only states

### 2.1 Rules

1. One or two slots, in RAM, freed at exit. Nothing goes to disk.
2. A load is refused if the scene changed since the save (a scene epoch counter, bumped by course loads and bulk texture teardown), if a guest thread was created or exited since the save, or if a save commit ran since the save.
3. Save and load are refused during photo select, evaluation and the score screen: while any shader write-back is pending (`PoolImpl::wbPending`, `nvn_internal.h:156`) or an evaluation program drew in the last 30 presents.
4. If any step times out, the state is dropped and the game continues untouched.

### 2.2 Quiesce point

The request is raised from the UI (hotkey or an F1 tab next to `runtime/ui/ui_saves.cpp`) and served by a coordinator host thread, not by a guest thread.

1. **Arm after a present.** Add a post-present hook in the `QueuePresentTexture` bridge, after `PresentWindowOnce` has released `queueMutex` (`nvn_vk.cpp:1258-1269`). The drain starts there, so the state always sits just after a presented frame. Do not park inside the existing present hook: it holds `queueMutex`.
2. **Drain the cores.** Under `G`: set a `draining` flag that makes `acquire_core_raw` grant nothing (no overflow grants either), and set `preempt` on every core holder. Each holder releases its core at its next check-in.
3. **Wait for clean parks.** Every guest thread must end up in one of the clean park sites (section 2.3). A thread that is in a host call with no core (file read, NVN submit, a sleep) finishes it and then waits for a core, which is a park site. Time limit 100 ms, then abort.
4. **GPU idle.** The coordinator runs `QueueFinishImpl` on every queue (`nvn_vk.cpp:1112-1116`), which retires all submits and lands all readbacks. After that no GPU work and no pending write to guest memory exists.
5. **Audio stop.** A flag makes the device thread output silence and stop calling `mix_slot`, so the rings stand still.

### 2.3 Clean park sites and self-capture

A parked thread's state is its registers plus its used host stack. Three facts decide the method:

- A thread waiting in `std::condition_variable` has live wait blocks on its own stack. Another thread must not overwrite that stack, and `SetThreadContext` on a waiting thread is unsafe.
- The frames between lifted code and the park may hold host heap objects (a `shared_ptr` copy, a `std::vector`). If such a frame is restored after the object was freed, it frees it twice.
- Windows `longjmp` unwinds frames with SEH. Jumping into a restored frame must not unwind anything.

So the thread captures itself, and only at clean sites:

1. **Clean site** = a park inside `wait_woken_raw` (`os_kernel.cpp:349-358`) or `acquire_core_raw` whose host frames above the lifted code hold no owning heap objects. The `nn::os` wait paths are clean: `block_on_key`, `poll_preempt`, `yield_thread` and the waits in `os_sync.cpp` hold only scalars, `Thread*` and the `Lock` on the static `G`. One `std::vector` local exists at `os_sync.cpp:945`, outside the wait path.
2. **Not clean** = a park reached from `os_host_block_end` (`os_kernel.cpp:1027-1032`) inside NVN, fs or audio code, or any guest code called back from a host override that keeps heap locals (for example the JPEG encoder wrappers in `host/mods/photoexport.cpp`). During a drain, a thread at such a site is granted a core once and runs to its next check-in, where its pending `preempt` parks it cleanly. Overrides that call back into guest code mark their scope with a per-thread counter; a thread inside such a scope is never captured.
3. **Capture.** At a clean site, with `G` held, the thread calls a small assembly routine that saves the callee-saved registers, `rsp` and a resume address (as `__builtin_setjmp` does, without unwinding), then copies `[rsp, stack top)` of its current stack into the state, then goes back to waiting. The copy happens before the frame changes, so it is consistent.
4. **Restore.** On load, the coordinator wakes every parked thread with a restore generation. Each thread, still holding `G`, switches to a 64 KiB side stack, copies its saved bytes back over its real stack, and jumps to the saved resume point. The restored frames believe `G` is held, and it is. The wait loop then re-checks the restored `Thread` fields and waits again or continues.
5. **Check:** the build has no CET shadow stack flag (`host/build_boot.py:36`, `:46`); a shadow stack would reject the jump. Keep it that way, or switch the jump to a shadow-stack-aware form.

### 2.4 Fibers need their own context switch

A thread parked while running a fiber is parked on that fiber's stack. Win32 keeps the fiber's saved registers and the thread's current fiber and stack bounds (TEB fields) in places the runtime does not own. A restore that puts thread A back on fiber F, while the TEB says A runs fiber G, corrupts the next `SwitchToFiber`.

Fix: replace `CreateFiberEx` / `ConvertThreadToFiberEx` / `SwitchToFiber` (`os_thread.cpp:183`, `:241`, `:245`) with a small assembly context switch that saves registers on the fiber's own stack and updates the TIB stack fields, as Boost.Context `fcontext` does on Windows x64. Fiber stacks are then plain `VirtualAlloc` ranges the runtime knows.

- A suspended fiber's state is its saved `rsp` plus `[rsp, top)` of its stack. The coordinator copies it directly, because no thread runs on it.
- A running fiber's state is part of the thread that runs it (section 2.3).
- `InitializeFiber` on an existing fiber and `FinalizeFiber` delete the host fiber today (`os_thread.cpp:286`, `:307`). While a state exists, they keep the stack alive instead (see 2.6).
- Cost: one extra TIB update per switch. The scheduler counts switches (`g_stats.fiber_switches`), so the census can measure the switch rate first.

### 2.5 What a save copies

1. Guest memory: committed pages of the heap, every VAMM region, the module RW segments, the main thread's stack, TLS and TLR, and the guest objects on the CRT heap (section 1.1). Pages are copied with `memcpy`; untouched committed pages can be skipped with `QueryWorkingSetEx`.
2. Every `Cpu` of every thread and fiber.
3. Host stacks: self-captured thread stacks (2.3) and suspended fiber stacks (2.4).
4. Host runtime containers and fields, copied by value under `G` and `g_fmu`: the rows of 1.3 marked as containers, plus `Thread` and `Fiber` records. They hold `Thread*`, `Fiber*` and guest addresses, which stay valid because the thread set must match and fibers are kept alive.
5. HLE: the ids, paths and modes of open files and directories, directory cursors, and the save's current generation (`Save::cur`, `fs_core.cpp:204`).
6. Audio: per slot the two rings, `cur` and `curFrame`.
7. Guest time: the `os_ticks()` value.
8. NVN: the set of live impl pointers per type, and the guest-API fields of each (table in 1.4).

### 2.6 Objects created or destroyed after the save

While a state exists, frees are deferred ("zombies"):

- NVN: `DestroyTexture`, `DestroyPool`, `SyncFinalize`, `CommandBufferFinalize`, sampler, program and window paths (`nvn_entry.cpp:176-177`, `:262-263`, `:286-287`, `:313-314`, `:410-411`; `nvn_vk.cpp:566`, `:705`) move the impl to a zombie list instead of freeing it. `CmdList` deletes at retire (`nvn_vk.cpp:765`) are deferred the same way.
- `os`: fibers (2.4), `Cpu` and TLR blocks of exiting threads, and VAMM region releases (the address range stays reserved so a load can map it at the same address).
- `fs`: closed files stay open as zombies.

On load:

1. Objects created after the save are not in the saved set. Restored guest memory no longer refers to them, so they are destroyed after the GPU is idle.
2. Zombies that were alive at the save are put back; the rest are freed.
3. Guest-API fields are restored; GPU-truth fields keep their current values.
4. Pool mirrors need nothing extra today: each submit bumps `g_replayEpoch` (`nvn_vk.cpp:666`, `:1038`) and copies every bound range from guest memory again. If the planned write-watch sync lands (`analysis/perf/pass2_design.md:108-112`), a load marks all pages dirty instead.

Zombies cost VRAM. That is why the slot count stays at one or two.

### 2.7 GPU-written memory and in-flight submits

- At save and at load the GPU is idle and `FlushReadbacks` has run (2.2 step 4). No readback can land in guest memory after it was restored. This order matters on load: idle the GPU and land readbacks first, then write guest memory back.
- Restored `SyncImpl` values are at or below the current timeline value, so a restored wait returns at once. (M)
- Ranges in `gpuRanges` (skinning output) keep GPU contents from the last frame. The next frame recomputes them. (M)
- Render targets keep their current contents. Per-frame targets are redrawn. Targets that carry history (auto exposure, SSAO history if any) are one frame off for a moment. DLSS, FSR and XeSS history must be reset on load, and so must motion-vector history (`docs/motion-vectors.md`). (M)

### 2.8 Audio resync

- On save: record the rings. On load: device feed off, rings reset to the saved contents, `cur` and `curFrame` restored, feed on with a 10 ms fade-in. The buffers are guest memory, so the restored pointers point at restored samples. (M)
- Wwise reads its files with synchronous `nn::fs` calls that hold the core (`fs.cpp:86-94` has no `os_host_block` call), so a Wwise thread never parks in the middle of a read. Its stream state is restored with guest memory and its threads. The earlier version of this study ranked Wwise streaming as a top risk; it is now medium. (M)

### 2.9 Time, input and files

- Time: on load, set the paused span so `os_ticks()` returns the saved value. Timer events keep their state in guest fields and follow guest time. Timed waits use host `steady_clock` deadlines on the stack, so a restored timed wait may end early once. Moving `Deadline` to guest ticks fixes that later. (M)
- Input: release all held buttons for 3 frames after a load, so the hotkey does not leak into the game. (M)
- Files: close files opened after the save, reopen zombies, restore directory cursors. RomFS and the mod overlay are read-only.
- Save data: a load is refused if `Save::cur` changed. `restore_generation` could roll it back later, but the album is written at course end, so mid-course retries do not need it (`AGENTS.md`, photo export entry). (M)

### 2.10 Photo scoring after a load

What the code and docs say:

- The shutter does not read pixels back. Photos stay in GPU images: the 360x240 thumbnail targets and the 1080x720 Capture3D target (`docs/nvn-vulkan-design.md:322`). Pixels reach guest memory only through readbacks (`:323`).
- Evaluation runs at the start of photo select. It copies "the photo's depth" into the Evaluation target and counts visible subject pixels into an SSBO that is written back to guest memory (`docs/nvn-vulkan-design.md:324-328`).

So per-photo data exists that lives only on the GPU until photo select. A load does not roll those images back. That is still correct if two things hold:

1. Photos taken before the save are never overwritten after it. The game's photo bookkeeping is guest memory, so after a load it again marks the post-save slots as free. The next photo overwrites one of them, which is what a retry should do.
2. Photos taken after the save leave nothing that the restored game reads, other than the slots it now treats as free.

Both are likely and neither is proven. Experiment E3 tests them. If they fail, the save also copies the photo images (found by their exempt scale tags, `kTagEvalDepth` and the thumbnail and Capture3D exemptions in `ScalePolicy`, `nvn_entry.cpp:224`, `:565`): about 3 MiB per 1080x720 RGBA8 image, plus the depth images if they are per photo.

The PNG export runs at JPEG encode time, at course end, and writes each distinct JPEG once per session (`AGENTS.md`, photo export entry), so a load cannot double-export.

### 2.11 Memory and time budget

- RAM per slot: about the touched guest memory plus 2 to 30 MiB of host stacks (L). Plan for up to 3.5 GiB per slot until the census measures it.
- Save time: one `memcpy` pass at roughly 8 to 10 GB/s, split over threads, so a few hundred ms for a full copy (L). Later saves can copy only dirty pages once `MEM_WRITE_WATCH` is added at the four reserve sites (`os_memory.cpp:44`, `:139`, `:191`, `:212`).
- Load time: the drain, the copy back, the object fix-up and the next submit's range sync. Target under 1 s (L).

### 2.12 Rejected alternatives

- **Guest memory only, threads re-entering at loop heads:** impossible, because frames live on host stacks (1.2).
- **Deterministic replay from course start:** thread timing, file timing and tick-based randomness make runs differ.
- **`RtlCloneUserProcess`:** the clone has one thread and no GPU or window state.
- **A fixed-address arena for all runtime heap memory** (the earlier version of this study): it would work but touches every allocation in `runtime/`. Clean park sites plus by-value copies plus zombies cover the same cases with fewer changes.
- **Retry the whole course** through the game's own flow: cheap, but it is not a retry of one photo.

## 3. Risks and unknowns, ranked

| # | Risk | Why it matters | Experiment that resolves it |
|---|---|---|---|
| 1 | Threads cannot all reach clean park sites, or a host frame on a parked stack owns heap memory | a load would crash or corrupt the heap later, hard to trace | E1 (census): at each park record the host return chain with `RtlCaptureStackBackTrace`, symbolize with `build\snap\snap_boot.map`, and list every frame that is not lifted code or a whitelisted `os` function. Run 20 drains in the yard and 20 in Florio. |
| 2 | The custom fiber switch is wrong or slow | the job scheduler stops, or frames slow down | E2: in `tests/os/os_test.cpp`, run a job-scheduler stress (fibers migrating between threads, waits inside fibers) on the new switch, then save and restore in the middle of it 1000 times and compare a checksum of all fiber results. Count `fiber_switches` per second in a course run to price the TIB update. |
| 3 | Per-photo GPU images are overwritten after a save | wrong thumbnails or scores after a retry | E3: route on `florio_eval`: photo A, save, photos B and C, load, photo D, finish the course. Expect A and D in photo select, correct thumbnails, a professor score, and exported PNGs of A and D only. |
| 4 | Drain time is too long (long host blocks, long sleeps, spin loops without a check-in) | save button lag, or aborted saves | E1 again: drain time per request, and the name of the last thread to park. |
| 5 | Threads, fibers, VAMM regions or NVN objects change often inside a course | loads refused, or large zombie lists | E1 extension: count creates and destroys of each kind over 10 s windows in Florio. |
| 6 | State size too big for RAM | one slot only, or none on smaller PCs | E1: committed and touched bytes per region (`QueryWorkingSetEx`). |
| 7 | Hidden host state missed (mods, upscalers, perf, ImGui) | small visual or logic glitches after load | E4: after M3, grep every `static` and `thread_local` in `runtime/` and `host/` (22 `thread_local` sites today) and classify each; then compare 20 frame dumps after a load with frames after the save. |
| 8 | Audio glitch after load | stuck voice or click | E5: `SNAP_AUDIO` capture directory across a load; check that appended and released buffer counts keep rising and the waveform resumes within 50 ms. |

## 4. Milestones

Sizes: **S** = up to 3 days, **M** = 1 to 2 weeks, **L** = 3 weeks or more.

| # | Milestone | Size | Proves | Test with existing tooling |
|---|---|---|---|---|
| M1 | Drain-and-census hook behind `SNAP_STATE_TEST=census@f<present>`. Drains the cores after that present, waits for parks, logs per thread: park site, host return chain, drain time; logs committed and touched bytes per region and live object counts; then releases the cores. No copy, no restore. | S | quiesce works in bounded time at clean sites; real sizes | `host\run_boot.ps1 -Route florio_photos -SaveFixture post_tutorial` with the env set to a few presents in the yard and in the course; check the log lines and that the run continues normally (presents keep counting) |
| M2 | Custom fiber switch and self-capture/restore in the `tests/os` harness | M | the context mechanics without the game | `tests/os/build.bat`, E2 stress test |
| M3 | First in-game rewind: memory, stacks, `os` state, time. NVN refuses the load if any NVN object was created or destroyed since the save; audio muted across it. `SNAP_STATE_TEST=save@f7000,load@f7600`. | M | the core idea end to end | `florio_photos` route; `NVN_FRAME_DUMP` shows the scene from f7000 again after the load; the run lives 3000 more presents; no new validation errors |
| M4 | NVN zombies, the object journal, guest-API field restore, scene epoch | M | loads that survive object churn | M3 test with the load moved 30 s after the save; then E3 |
| M5 | Audio ring restore with fade, fs zombies, deferred VAMM release, input release | M | a load you can hear and play through | E5; route plays on to photo select |
| M6 | UI: two hotkeys, an F1 tab with Save and Load, refusal messages; photo correctness checks; one more course | S | the owner's feature | E3 on Florio and a second course; `SNAP_UI_TEST` script that presses the hotkeys |

M1 is the smallest step that tests the core idea. If the drain cannot stop every thread at a clean site within 100 ms, the design changes before any copy code is written.

Test hygiene: one heavy run at a time, BelowNormal priority, exclusive runs (`-Exclusive`) for timing numbers, an empty `SNAP_MODS_DIR` for benches, and the test save directory, never the owner's.

## 5. Recommendation

**Go with conditions.**

Reasons for go:

- The runtime already owns every place a guest thread can stop (virtual cores, check-ins, all waits), so a full stop is a small change to `acquire_core_raw` and `poll_preempt`.
- Guest NVN objects are opaque host pointers, guest sync state is in guest fields, and file reads have no position. Most host state is either restored with guest memory or copied by value.
- The GPU can be made idle and readbacks landed with functions that already exist.

Conditions:

1. M1 shows every guest thread parks at a clean site within 100 ms, in the yard and in Florio, over 20 drains each.
2. M1 shows the thread and fiber sets do not change during a course. If they do, the plan needs a restore path for created and exited threads, which adds about 2 weeks.
3. The owner accepts the limits: session only, one or two slots, no load across a scene change, and disk saves are not rolled back.
4. M2 and later wait until no other worker is editing `runtime/os` and `runtime/nvn`.

If condition 1 fails, the fallback is "not yet": revisit after the CPU-side stall work, which also touches the scheduler.
