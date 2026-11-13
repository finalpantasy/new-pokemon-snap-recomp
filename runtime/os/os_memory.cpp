// nn::os memory as host overrides (written for this project): the virtual address memory manager (VAMM:
// AllocateAddressRegion / AllocateMemoryPages / ...) and the memory heap (SetMemoryHeapSize / AllocateMemoryBlock).
// Guest addresses are host addresses, so both are thin layers over VirtualAlloc. The sdk implementation keeps its state
// in OsResourceManager (GOT sdk+0xc88838), which nothing constructs on the host; every entry point that reads it
// is replaced here or in os_thread.cpp (see docs/horizon-os.md, "boot").
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "os_internal.h"

using namespace osk;

namespace {

// Results seen in the sdk: VammManager::AllocateAddressRegion sdk+0x346f48/+0x346f6c, AllocateMemoryPages
// sdk+0x3476fc/+0x347754.
constexpr uint32_t R_OUT_OF_MEMORY = 0x1003, R_OUT_OF_ADDRESS_SPACE = 0x1803, R_INVALID_RANGE = 0xe03;
constexpr uint64_t PAGE = 0x1000;
constexpr uint64_t HEAP_UNIT = 0x200000;           // nn::os::MemoryHeapUnitSize
constexpr uint64_t HEAP_RESERVE = 16ull << 30;     // address space reserved for the heap (committed on demand)

// [perf] Guest reservations carry MEM_WRITE_WATCH so the NVN layer can copy only the pages the game wrote into its mirrored
// buffers (GetWriteWatch, runtime/nvn/nvn_vk.cpp SyncPoolRange; perf pass 4). The flag can only be set at reserve time.
// SNAP_SYNC_FULL=1 or SNAP_WRITE_WATCH=0 reserve without it (old behavior; the NVN layer then copies whole ranges).
DWORD ww_flag() {
    static const DWORD f = [] {
        const char *a = getenv("SNAP_SYNC_FULL"), *b = getenv("SNAP_WRITE_WATCH");
        return ((a && *a == '1') || (b && *b == '0')) ? (DWORD)0 : (DWORD)MEM_WRITE_WATCH;
    }();
    return f;
}

std::mutex g_mm;
struct Region { uint64_t size, committed; };
std::map<uint64_t, Region> g_regions;              // VAMM address regions by base
uint64_t g_reserved_total = 0, g_committed_total = 0;

uint64_t page_up(uint64_t v) { return (v + PAGE - 1) & ~(PAGE - 1); }

Region *region_of(uint64_t a, uint64_t size) {   // under g_mm: the region that contains [a, a+size)
    auto it = g_regions.upper_bound(a);
    if (it == g_regions.begin()) return nullptr;
    --it;
    if (a + size > it->first + it->second.size || a + size < a) return nullptr;
    return &it->second;
}

// Crash main+0x99c88c / 0x99c910 (2 runs, 2026-11-12): the game's 2 MiB page heap (main+0x8dc340, nu::PageHeap) takes its large
// blocks from AllocateAddressRegion + AllocateMemoryPages and, when either fails, its caller (0x99c7f0) memsets the NULL result.
// On the Switch these never fail inside the game's budget; on the host they fail only under transient Windows memory pressure
// (commit limit reached by other processes). So a failed reserve/commit with a transient error (commit limit, out of memory, no
// system resources) is retried, without holding g_mm, with a backoff of 1, 2, 4 .. 100 ms for up to SNAP_VAMM_RETRY_MS (default 5000;
// 0 = fail at once, the old behavior); any other error fails at once. The guest caller holds the page heap's lock meanwhile, so the
// budget is short: it only has to outlast a brief pressure spike, and the alternative is the crash.
// Test switch SNAP_FAULT_VAMM=<s>:<n>: from <s> seconds after the first VAMM call, fail the next n reserves/commits (simulated
// pressure, off by default).
uint64_t retry_ms() {
    static const uint64_t v = [] { const char *e = getenv("SNAP_VAMM_RETRY_MS"); return e && *e ? strtoull(e, nullptr, 0) : 5000ull; }();
    return v;
}
bool fault_now() {
    static long long s = 0, n = 0;
    static std::atomic<long long> hits{0};
    static const bool on = [] { const char *e = getenv("SNAP_FAULT_VAMM"); return e && sscanf(e, "%lld:%lld", &s, &n) == 2 && n > 0; }();
    if (!on) return false;
    static const ULONGLONG t0 = GetTickCount64();
    if (GetTickCount64() - t0 < (ULONGLONG)s * 1000) return false;
    if (hits.fetch_add(1) >= n) return false;
    SetLastError(ERROR_COMMITMENT_LIMIT);
    return true;
}
bool vamm_wait(const char *what, uint64_t size, uint64_t &waited, DWORD err) {   // true = retry after a pause
    const bool transient = err == ERROR_COMMITMENT_LIMIT || err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY ||
                           err == ERROR_NO_SYSTEM_RESOURCES || err == ERROR_COMMITMENT_MINIMUM;
    const uint64_t lim = transient ? retry_ms() : 0;
    if (waited == 0) {
        uint64_t res, com;
        size_t nreg;
        {   // review finding 9: the totals are read under g_mm
            std::lock_guard<std::mutex> lk(g_mm);
            res = g_reserved_total, com = g_committed_total, nreg = g_regions.size();
        }
        fprintf(stderr, "[os-mem] %s(%#llx) failed: error %lu (reserved %#llx, committed %#llx, %zu regions)%s\n", what, (unsigned long long)size, err,
                (unsigned long long)res, (unsigned long long)com, nreg, lim ? ": retrying" : "");
    }
    if (waited >= lim) {
        if (lim) fprintf(stderr, "[os-mem] %s(%#llx) still failing after %llu ms: giving up\n", what, (unsigned long long)size, (unsigned long long)waited);
        return false;
    }
    static thread_local uint64_t step = 1;
    if (waited == 0) step = 1;
    os_io_begin();   // the backoff does not hold a virtual core (finding 9; SNAP_SCHED_LEGACY=hostblock keeps it)
    Sleep((DWORD)step);
    os_io_end();
    waited += step;
    step = std::min<uint64_t>(step * 2, 100);
    return true;
}
uint32_t vamm_alloc_region(uint64_t out, uint64_t size) {
    if (!size) return R_INVALID_RANGE;
    size = page_up(size);
    void *p = nullptr;
    for (uint64_t waited = 0;;) {
        p = fault_now() ? nullptr : VirtualAlloc(nullptr, size, MEM_RESERVE | ww_flag(), PAGE_NOACCESS);
        if (p) {
            if (waited) fprintf(stderr, "[os-mem] AllocateAddressRegion(%#llx) succeeded after %llu ms\n", (unsigned long long)size, (unsigned long long)waited);
            break;
        }
        if (!vamm_wait("AllocateAddressRegion", size, waited, GetLastError())) return R_OUT_OF_ADDRESS_SPACE;
    }
    std::lock_guard<std::mutex> lk(g_mm);
    g_regions[(uint64_t)(uintptr_t)p] = Region{size, 0};
    g_reserved_total += size;
    gst<uint64_t>(out, (uint64_t)(uintptr_t)p);
    return R_OK;
}
uint32_t vamm_alloc_pages(uint64_t a, uint64_t size) {
    if (!size || (a & (PAGE - 1))) return R_INVALID_RANGE;
    size = page_up(size);
    for (uint64_t waited = 0;;) {
        DWORD err = 0;
        {
            std::lock_guard<std::mutex> lk(g_mm);
            Region *r = region_of(a, size);
            if (!r) return R_INVALID_RANGE;
            // Count only pages that were not committed yet (committing twice is legal and keeps the contents).
            uint64_t fresh = 0;
            for (uint64_t p = a; p < a + size;) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) break;
                uint64_t end = std::min<uint64_t>(a + size, (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize);
                if (mi.State != MEM_COMMIT) fresh += end - p;
                p = end;
            }
            if (!fault_now() && VirtualAlloc((void *)(uintptr_t)a, size, MEM_COMMIT, PAGE_READWRITE)) {
                r->committed += fresh;
                g_committed_total += fresh;
                if (waited) fprintf(stderr, "[os-mem] AllocateMemoryPages(%#llx) succeeded after %llu ms\n", (unsigned long long)size, (unsigned long long)waited);
                return R_OK;
            }
            err = GetLastError();
        }
        if (!vamm_wait("AllocateMemoryPages", size, waited, err)) return R_OUT_OF_MEMORY;   // waits without g_mm: frees can proceed
    }
}
uint32_t vamm_free_pages(uint64_t a, uint64_t size) {
    if (!size || (a & (PAGE - 1))) return R_INVALID_RANGE;
    size = page_up(size);
    std::lock_guard<std::mutex> lk(g_mm);
    Region *r = region_of(a, size);
    if (!r) return R_INVALID_RANGE;
    uint64_t gone = 0;
    for (uint64_t p = a; p < a + size;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) break;
        uint64_t end = std::min<uint64_t>(a + size, (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize);
        if (mi.State == MEM_COMMIT) gone += end - p;
        p = end;
    }
    VirtualFree((void *)(uintptr_t)a, size, MEM_DECOMMIT);
    r->committed -= std::min(r->committed, gone);
    g_committed_total -= std::min(g_committed_total, gone);
    return R_OK;
}
uint32_t vamm_free_region(uint64_t a) {
    std::lock_guard<std::mutex> lk(g_mm);
    auto it = g_regions.find(a);
    if (it == g_regions.end()) return R_INVALID_RANGE;
    VirtualFree((void *)(uintptr_t)a, 0, MEM_RELEASE);
    g_reserved_total -= it->second.size;
    g_committed_total -= std::min(g_committed_total, it->second.committed);
    g_regions.erase(it);
    return R_OK;
}
uint32_t vamm_alloc_memory(uint64_t out, uint64_t size) {   // reserve + commit
    uint32_t r = vamm_alloc_region(out, size);
    if (r) return r;
    uint64_t a = gld<uint64_t>(out);
    r = vamm_alloc_pages(a, size);
    if (r) {
        vamm_free_region(a);
        gst<uint64_t>(out, 0);
    }
    return r;
}

void ov_AllocateAddressRegion(Cpu *c) { c->x[0] = vamm_alloc_region(c->x[0], c->x[1]); }
void ov_AllocateMemoryPages(Cpu *c) { c->x[0] = vamm_alloc_pages(c->x[0], c->x[1]); }
void ov_FreeMemoryPages(Cpu *c) { c->x[0] = vamm_free_pages(c->x[0], c->x[1]); }
void ov_FreeAddressRegion(Cpu *c) { c->x[0] = vamm_free_region(c->x[0]); }
void ov_AllocateMemory(Cpu *c) { c->x[0] = vamm_alloc_memory(c->x[0], c->x[1]); }
void ov_InitializeVam(Cpu *) {}
void ov_IsVamEnabled(Cpu *c) { c->x[0] = 1; }
void ov_VamUsage(Cpu *c) {   // VirtualAddressMemoryResourceUsage {assigned, used} in x0:x1
    std::lock_guard<std::mutex> lk(g_mm);
    c->x[0] = g_reserved_total;
    c->x[1] = g_committed_total;
}
// VammManager members: x0 = this (unused on the host)
template <GuestFn F> void member(Cpu *c) {
    for (int i = 0; i < 7; i++) c->x[i] = c->x[i + 1];
    F(c);
}

// ---- memory heap: one reservation, committed up to the requested size (svcSetHeapSize semantics)
uint64_t g_heap_base = 0, g_heap_size = 0;
std::map<uint64_t, uint64_t> g_heap_blocks;   // allocated blocks: base -> size

uint32_t heap_set_size(uint64_t size) {
    if (size % HEAP_UNIT || size > HEAP_RESERVE) return R_INVALID_RANGE;
    std::lock_guard<std::mutex> lk(g_mm);
    if (!g_heap_base) {
        void *p = VirtualAlloc(nullptr, HEAP_RESERVE, MEM_RESERVE | ww_flag(), PAGE_NOACCESS);
        if (!p) return R_OUT_OF_ADDRESS_SPACE;
        g_heap_base = (uint64_t)(uintptr_t)p;
    }
    if (size > g_heap_size) {
        if (!VirtualAlloc((void *)(uintptr_t)(g_heap_base + g_heap_size), size - g_heap_size, MEM_COMMIT, PAGE_READWRITE))
            return R_OUT_OF_MEMORY;
    } else if (size < g_heap_size) {
        if (!g_heap_blocks.empty()) {
            auto last = std::prev(g_heap_blocks.end());
            if (last->first + last->second > g_heap_base + size) return R_INVALID_STATE;   // blocks still in use
        }
        VirtualFree((void *)(uintptr_t)(g_heap_base + size), g_heap_size - size, MEM_DECOMMIT);
    }
    g_heap_size = size;
    return R_OK;
}
uint32_t heap_alloc_block(uint64_t out, uint64_t size) {   // first fit, page aligned
    if (!size) return R_INVALID_RANGE;
    size = page_up(size);
    std::lock_guard<std::mutex> lk(g_mm);
    uint64_t at = g_heap_base;
    for (auto &b : g_heap_blocks) {
        if (b.first - at >= size) break;
        at = b.first + b.second;
    }
    if (!g_heap_base || at + size > g_heap_base + g_heap_size) {
        fprintf(stderr, "[os-mem] AllocateMemoryBlock(%#llx) failed: heap %#llx bytes, %zu blocks\n", (unsigned long long)size, (unsigned long long)g_heap_size, g_heap_blocks.size());
        return R_OUT_OF_MEMORY;
    }
    g_heap_blocks[at] = size;
    memset((void *)(uintptr_t)at, 0, size);
    gst<uint64_t>(out, at);
    return R_OK;
}
void ov_SetMemoryHeapSize(Cpu *c) { c->x[0] = heap_set_size(c->x[0]); }
void ov_GetMemoryHeapAddress(Cpu *c) { std::lock_guard<std::mutex> lk(g_mm); c->x[0] = g_heap_base; }
void ov_GetMemoryHeapSize(Cpu *c) { std::lock_guard<std::mutex> lk(g_mm); c->x[0] = g_heap_size; }
void ov_AllocateMemoryBlock(Cpu *c) { c->x[0] = heap_alloc_block(c->x[0], c->x[1]); }
void ov_FreeMemoryBlock(Cpu *c) {
    std::lock_guard<std::mutex> lk(g_mm);
    auto it = g_heap_blocks.find(c->x[0]);
    if (it == g_heap_blocks.end()) fatal("FreeMemoryBlock(%#llx): not a block", (unsigned long long)c->x[0]);
    g_heap_blocks.erase(it);
}

// ---- svcGetInfo memory-region values (see runtime/os/svc_getinfo.md)
constexpr uint64_t ALIAS_RESERVE = 2ull << 30;        // alias region: a real host reservation, VAMM does not allocate from it
constexpr uint64_t STACK_RESERVE = 1ull << 30;        // stack region: same
constexpr uint64_t TOTAL_NON_SYSTEM = 4ull << 30;     // application memory budget
constexpr uint64_t SYSTEM_RESOURCE = 1ull << 30;      // system resource size; nonzero = VAMM enabled (sdk+0x346dbc)
uint64_t g_alias_base = 0, g_stack_base = 0;

uint64_t reserve_once(uint64_t &slot, uint64_t size) {   // under g_mm
    if (!slot) {
        void *p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
        if (p) slot = (uint64_t)(uintptr_t)p;
    }
    return slot;
}

}  // namespace

// SNAP_SOAK_LOG (runtime/nvn/nvn_soak.cpp): guest VAMM and memory-heap totals. out = {vamm reserved, vamm committed, region count,
// heap size, heap block count, heap bytes in blocks}.
extern "C" void snap_os_mem_stats(uint64_t *out) {
    std::lock_guard<std::mutex> lk(g_mm);
    out[0] = g_reserved_total; out[1] = g_committed_total; out[2] = g_regions.size();
    out[3] = g_heap_size; out[4] = g_heap_blocks.size();
    uint64_t b = 0;
    for (auto &kv : g_heap_blocks) b += kv.second;
    out[5] = b;
}

namespace osk {
uint32_t mem_get_info(uint32_t type, uint64_t *out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t lo = (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress, hi = (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress + 1;
    std::lock_guard<std::mutex> lk(g_mm);
    uint64_t used = g_heap_size + g_committed_total, sysused = std::min(g_committed_total, SYSTEM_RESOURCE);
    uint64_t v = 0;
    switch (type) {
    case 2: v = reserve_once(g_alias_base, ALIAS_RESERVE); if (!v) return R_OUT_OF_ADDRESS_SPACE; break;
    case 3: v = ALIAS_RESERVE; break;
    case 4:
        if (!g_heap_base) {
            void *p = VirtualAlloc(nullptr, HEAP_RESERVE, MEM_RESERVE | ww_flag(), PAGE_NOACCESS);
            if (!p) return R_OUT_OF_ADDRESS_SPACE;
            g_heap_base = (uint64_t)(uintptr_t)p;
        }
        v = g_heap_base;
        break;
    case 5: v = HEAP_RESERVE; break;
    case 6: v = TOTAL_NON_SYSTEM + SYSTEM_RESOURCE; break;
    case 7: v = used + sysused; break;
    case 12: v = lo; break;
    case 13: v = hi - lo; break;
    case 14: v = reserve_once(g_stack_base, STACK_RESERVE); if (!v) return R_OUT_OF_ADDRESS_SPACE; break;
    case 15: v = STACK_RESERVE; break;
    case 16: v = SYSTEM_RESOURCE; break;
    case 17: v = sysused; break;
    case 21: v = TOTAL_NON_SYSTEM; break;
    case 22: v = used; break;
    default: return R_INVALID_ENUM_VALUE;
    }
    *out = v;
    return R_OK;
}
}  // namespace osk

namespace osk {
extern const OsOverride g_memory_overrides[];
extern const size_t g_memory_override_count;
const OsOverride g_memory_overrides[] = {
    {"_ZN2nn2os21AllocateAddressRegionEPmm", ov_AllocateAddressRegion},
    {"_ZN2nn2os19AllocateMemoryPagesEmm", ov_AllocateMemoryPages},
    {"_ZN2nn2os15FreeMemoryPagesEmm", ov_FreeMemoryPages},
    {"_ZN2nn2os17FreeAddressRegionEm", ov_FreeAddressRegion},
    {"_ZN2nn2os14AllocateMemoryEPmm", ov_AllocateMemory},
    {"_ZN2nn2os30InitializeVirtualAddressMemoryEv", ov_InitializeVam},
    {"_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", ov_IsVamEnabled},
    {"_ZN2nn2os36GetVirtualAddressMemoryResourceUsageEv", ov_VamUsage},
    {"_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm", member<ov_AllocateAddressRegion>},
    {"_ZN2nn2os6detail11VammManager19AllocateMemoryPagesEmm", member<ov_AllocateMemoryPages>},
    {"_ZN2nn2os6detail11VammManager15FreeMemoryPagesEmm", member<ov_FreeMemoryPages>},
    {"_ZN2nn2os6detail11VammManager17FreeAddressRegionEm", member<ov_FreeAddressRegion>},
    {"_ZN2nn2os6detail11VammManager14AllocateMemoryEPmm", member<ov_AllocateMemory>},
    {"_ZN2nn2os6detail11VammManager19InitializeIfEnabledEv", ov_InitializeVam},
    {"_ZN2nn2os6detail11VammManager36GetVirtualAddressMemoryResourceUsageEv", member<ov_VamUsage>},
    {"_ZN2nn2os17SetMemoryHeapSizeEm", ov_SetMemoryHeapSize},
    {"_ZN2nn2os20GetMemoryHeapAddressEv", ov_GetMemoryHeapAddress},
    {"_ZN2nn2os17GetMemoryHeapSizeEv", ov_GetMemoryHeapSize},
    {"_ZN2nn2os19AllocateMemoryBlockEPmm", ov_AllocateMemoryBlock},
    {"_ZN2nn2os15FreeMemoryBlockEmm", ov_FreeMemoryBlock},
};
const size_t g_memory_override_count = sizeof g_memory_overrides / sizeof g_memory_overrides[0];
}  // namespace osk
