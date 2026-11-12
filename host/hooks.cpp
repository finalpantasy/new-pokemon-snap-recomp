// Host hooks for the exception and longjmp resume mechanism, plus the libc leaf overrides (project code, ours).
// Design: docs/runtime-boundary.md sections 2 and 3. The functions below replace guest functions by address
// (recomp/snap_gen.py SDK_HOOKS): the generated f_<addr> of each one just calls it.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "loader.h"

using namespace loader;

extern "C" {

// libunwind Registers_arm64::jumpto (sdk+0x513794): install the context at x0 and jump to its pc. The lifted unwinder
// has already run phase 1 and 2 over guest memory; only the jump changes. Here: copy the context into the Cpu and set
// the resume flag; every lifted frame returns until the one whose landing pad has this pc and sp.
// Context layout (from the restore leaf): x0..x29 at 8*i, sp at 0xf8, pc at 0x100, d0..d31 at 0x110 + 8*k.
void rt_hook_unw_jumpto(Cpu *c) {
    const uint64_t *r = (const uint64_t *)c->x[0];
    for (int i = 0; i < 30; i++) c->x[i] = r[i];
    c->sp = r[31];
    c->pc = r[32];
    c->x[30] = r[32];
    for (int k = 0; k < 32; k++) c->v[k] = (__uint128_t)r[34 + k];
    c->resuming = 1;
    uint64_t n = ++g_exc_count;
    if (g_trace_imports && n <= 200) trace("guest exception #%llu: unwinder resumes at %s sp=%#llx", (unsigned long long)n, symbolize(c->pc).c_str(), (unsigned long long)c->sp);
}

// musl AArch64 jmp_buf (22 x 8 bytes): x19..x28 [0..9], x29 [10], x30 [11], sp [13], d8..d15 [14..21].
void rt_hook_setjmp(Cpu *c) {
    uint64_t *jb = (uint64_t *)c->x[0];
    for (int i = 0; i < 10; i++) jb[i] = c->x[19 + i];
    jb[10] = c->x[29];
    jb[11] = c->x[30];
    jb[13] = c->sp;
    for (int k = 0; k < 8; k++) jb[14 + k] = (uint64_t)c->v[8 + k];
    c->x[0] = 0;
}

void rt_hook_longjmp(Cpu *c) {
    const uint64_t *jb = (const uint64_t *)c->x[0];
    uint64_t val = c->x[1] ? c->x[1] : 1;
    for (int i = 0; i < 10; i++) c->x[19 + i] = jb[i];
    c->x[29] = jb[10];
    c->x[30] = jb[11];
    c->sp = jb[13];
    for (int k = 0; k < 8; k++) c->v[8 + k] = (__uint128_t)jb[14 + k];
    c->x[0] = val;
    c->pc = jb[11];
    c->resuming = 1;
}

// nn::ro::detail::GetExceptionInfo(ExceptionInfo *out, uint64_t pc) -> bool. Layout read from its caller
// (_Unwind_Find_FDE, sdk+0x5104f0): out[0] = module base, out[1] = mapped size, out[2] = .eh_frame_hdr offset from the base,
// out[3] = its size. The host answers for the lifted modules only; a pc elsewhere (host code) has no unwind data.
void rt_hook_get_exception_info(Cpu *c) {
    uint64_t *out = (uint64_t *)c->x[0];
    uint64_t pc = c->x[1];
    const GuestModule *m = nullptr;
    if (pc - g_main.base < g_main.span) m = &g_main;
    else if (pc - g_sdk.base < g_sdk.span) m = &g_sdk;
    if (!m || !m->eh_hdr) { c->x[0] = 0; return; }
    out[0] = m->base;
    out[1] = m->span;
    out[2] = m->eh_hdr;
    out[3] = m->eh_hdr_size;
    c->x[0] = 1;
}

}  // extern "C"

// ---------------------------------------------------------------------------------------------------- libc leaves

static void *gp(uint64_t v) { return (void *)(uintptr_t)v; }

// Debug: SNAP_MEMLOG=1 keeps a ring of libc memory events; crash_filter calls snap_memlog_find(addr) to list those that touched it.
struct MemEv { uint64_t addr, size, lr; uint32_t tid; char kind; };
static MemEv g_memring[1 << 20];
static volatile LONG64 g_memn;
static bool g_memlog = getenv("SNAP_MEMLOG") != nullptr;
static inline void memev(char k, uint64_t a, uint64_t sz, uint64_t lr) {
    if (!g_memlog) return;
    LONG64 i = InterlockedIncrement64(&g_memn) - 1;
    g_memring[i & ((1 << 20) - 1)] = MemEv{a, sz, lr, GetCurrentThreadId(), k};
}
void snap_memev(char k, uint64_t a, uint64_t sz, uint64_t lr) { memev(k, a, sz, lr); }
void snap_memlog_find(uint64_t addr) {
    if (!g_memlog) return;
    LONG64 n = g_memn;
    fprintf(stderr, "  memlog: %lld events\n", (long long)n);
    for (LONG64 i = n > (1 << 20) ? n - (1 << 20) : 0; i < n; i++) {
        const MemEv &e = g_memring[i & ((1 << 20) - 1)];
        if (addr >= e.addr && addr < e.addr + (e.size ? e.size : 1))
            fprintf(stderr, "  memev#%lld %c addr=%#llx size=%#llx tid=%u lr=%s\n", (long long)i, e.kind, (unsigned long long)e.addr, (unsigned long long)e.size, e.tid, symbolize(e.lr).c_str());
    }
}

static void h_malloc(Cpu *c) { uint64_t sz = c->x[0]; c->x[0] = (uint64_t)_aligned_malloc((size_t)c->x[0] ? (size_t)c->x[0] : 1, 16); memev('m', c->x[0], sz, c->x[30]); }
static void h_free(Cpu *c) { memev('f', c->x[0], 0, c->x[30]); _aligned_free(gp(c->x[0])); }
static void h_calloc(Cpu *c) {
    size_t n = (size_t)(c->x[0] * c->x[1]);
    void *p = _aligned_malloc(n ? n : 1, 16);
    if (p) memset(p, 0, n);
    c->x[0] = (uint64_t)p;
    memev('c', (uint64_t)p, n, c->x[30]);
}
static void h_realloc(Cpu *c) { c->x[0] = (uint64_t)_aligned_realloc(gp(c->x[0]), (size_t)c->x[1] ? (size_t)c->x[1] : 1, 16); }
static void h_aligned_alloc(Cpu *c) { uint64_t sz = c->x[1], lr = c->x[30]; c->x[0] = (uint64_t)_aligned_malloc((size_t)c->x[1] ? (size_t)c->x[1] : 1, (size_t)c->x[0]); memev('a', c->x[0], sz, lr); }
static void h_memalign(Cpu *c) { h_aligned_alloc(c); }
static void h_memcpy(Cpu *c) { memev('y', c->x[0], c->x[2], c->x[30]); memcpy(gp(c->x[0]), gp(c->x[1]), (size_t)c->x[2]); }   // returns x0 unchanged
static void h_memmove(Cpu *c) { memev('v', c->x[0], c->x[2], c->x[30]); memmove(gp(c->x[0]), gp(c->x[1]), (size_t)c->x[2]); }
static void h_memset(Cpu *c) { memev('s', c->x[0], c->x[2], c->x[30]); memset(gp(c->x[0]), (int)c->x[1], (size_t)c->x[2]); }
static void h_strlen(Cpu *c) { c->x[0] = strlen((const char *)gp(c->x[0])); }

namespace loader {
void snap_lodanim_install();
void snap_photoexport_install();   // host/mods/photoexport.cpp (SNAP_PHOTO_EXPORT, on by default)
void install_libc_overrides() {
    // No malloc/free/calloc/realloc/aligned_alloc/memalign overrides: main defines the first five (the nu heap, main+0x4e2f60..)
    // and sdk memalign routes into them; a host heap here mixes allocators (Oodle mallocs via memalign, frees via main free).
    add_override("memcpy", h_memcpy);
    add_override("memmove", h_memmove);
    add_override("memset", h_memset);
    add_override("strlen", h_strlen);
    snap_photoexport_install();
    snap_lodanim_install();   // host/mods/lodanim.cpp: SNAP_FOV / SNAP_TRACE_FOV (registers only when asked)
}
}  // namespace loader
