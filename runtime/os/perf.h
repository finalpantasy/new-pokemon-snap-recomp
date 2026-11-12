// Env-gated performance instrumentation (off unless SNAP_PERF=<output base path> is set; see analysis/perf/).
// Counters are process-wide atomics added to by any thread; frame_mark() (called once per presented frame) turns the deltas since
// the previous mark into one CSV row. A sampler thread writes per-thread CPU time once a second. When SNAP_PERF is unset every
// entry point is a single predictable branch.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace snap_perf {

enum Ctr {
    C_REPLAY = 0,    // SubmitLists: replaying NVN command lists into Vulkan (game thread CPU), includes several items below
    C_QSUBMIT,       // vkQueueSubmit2 calls (draw lists and present)
    C_PACE,          // frame limiter sleep
    C_ACQUIRE,       // vkAcquireNextImageKHR (blocks on the display)
    C_ACQSEM,        // waiting for the timeline value that last used an acquire semaphore
    C_PRESENT,       // vkQueuePresentKHR
    C_WAITSYNC,      // guest nvnSyncWait (CPU waits for the GPU)
    C_FINISH,        // guest nvnQueueFinish
    C_PIPE,          // vkCreateGraphicsPipelines / compute (synchronous compile on the replaying thread)
    C_SHADER,        // program creation (cache read, vkCreateShaderModule, harness)
    C_POOLSYNC,      // SyncPoolRange: guest memory copied into the mirrored buffers
    C_UPLOAD,        // UploadStaticTexture (deswizzle, ASTC/BC7 transcode, staging copy)
    C_HASH,          // RefreshDynamicTexture hashing of CPU-written textures
    C_DESC,          // WriteDescriptors (set allocation + vkUpdateDescriptorSets)
    C_GETPIPE,       // GetPipeline lookup including key building (includes compile)
    C_READBACK,      // FlushReadbacks / GPU->guest copies
    C_SCRATCH,       // NewScratch (vkCreateBuffer + vkAllocateMemory + map)
    C_REAP,          // QueueImpl::Reap
    C_COUNT
};
enum Val {   // plain counters (no time)
    V_DRAWS = 0, V_SUBMITS, V_CMDS, V_POOLSYNC_BYTES, V_POOLSYNC_CALLS, V_UPLOADS, V_UPLOAD_BYTES, V_HASH_BYTES, V_HASH_CALLS,
    V_PIPES_CREATED, V_SCRATCH_ALLOCS, V_DESC_SETS, V_BARRIERS, V_SHADERS, V_PIPES_SKIPPED, V_COUNT
};

inline const bool on = std::getenv("SNAP_PERF") != nullptr;

inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern std::atomic<uint64_t> g_ns[C_COUNT], g_calls[C_COUNT], g_val[V_COUNT];

inline void add(Ctr c, uint64_t ns) {
    g_ns[c].fetch_add(ns, std::memory_order_relaxed);
    g_calls[c].fetch_add(1, std::memory_order_relaxed);
}
inline void count(Val v, uint64_t n = 1) { if (on) g_val[v].fetch_add(n, std::memory_order_relaxed); }

struct Scope {
    Ctr c; uint64_t t0;
    explicit Scope(Ctr c_) : c(c_), t0(on ? now_ns() : 0) {}
    ~Scope() { if (on) add(c, now_ns() - t0); }
};

// One row per presented frame. gpu_* come from the NVN layer's timestamp queries (-1 when unavailable).
void frame_mark(uint64_t present_no, double gpu_busy_ms, double gpu_span_ms, int gpu_cbs);
// A named occurrence with a duration (pipeline compiles, shader translations, texture uploads over 2 ms ...).
void event(const char* kind, double ms, const char* detail);

}  // namespace snap_perf
