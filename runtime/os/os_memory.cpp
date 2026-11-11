// nn::os memory as host overrides (written for this project): the virtual address memory manager (VAMM:
// AllocateAddressRegion / AllocateMemoryPages / ...) and the memory heap (SetMemoryHeapSize / AllocateMemoryBlock).
// Guest addresses are host addresses, so both are thin layers over VirtualAlloc. The sdk implementation keeps its state
// in OsResourceManager (GOT sdk+0xc88838), which nothing constructs on the host; every entry point that reads it
// is replaced here or in os_thread.cpp (see docs/horizon-os.md, "boot").
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
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

uint32_t vamm_alloc_region(uint64_t out, uint64_t size) {
    if (!size) return R_INVALID_RANGE;
    size = page_up(size);
    void *p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return R_OUT_OF_ADDRESS_SPACE;
    std::lock_guard<std::mutex> lk(g_mm);
    g_regions[(uint64_t)(uintptr_t)p] = Region{size, 0};
    g_reserved_total += size;
    gst<uint64_t>(out, (uint64_t)(uintptr_t)p);
    return R_OK;
}
uint32_t vamm_alloc_pages(uint64_t a, uint64_t size) {
    if (!size || (a & (PAGE - 1))) return R_INVALID_RANGE;
    size = page_up(size);
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
    if (!VirtualAlloc((void *)(uintptr_t)a, size, MEM_COMMIT, PAGE_READWRITE)) return R_OUT_OF_MEMORY;
    r->committed += fresh;
    g_committed_total += fresh;
    return R_OK;
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
        void *p = VirtualAlloc(nullptr, HEAP_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
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
    if (!g_heap_base || at + size > g_heap_base + g_heap_size) return R_OUT_OF_MEMORY;
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
            void *p = VirtualAlloc(nullptr, HEAP_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
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
