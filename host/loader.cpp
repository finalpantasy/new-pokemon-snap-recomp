// Two-module loader and runtime boundary (project code, ours). See host/loader.h and docs/runtime-boundary.md.
// The ELF mapping and relocation code follows the host spike (host/spike.cpp), extended to two modules.
#include "loader.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern const uint64_t main_fn_addrs[];
extern const GuestFn main_fn_ptrs[];
extern const uint64_t main_fn_count;
extern const char *const main_import_names[];
extern const uint64_t main_import_stub_addrs[];
extern const uint64_t main_import_count;
extern const uint64_t sdk_fn_addrs[];
extern const GuestFn sdk_fn_ptrs[];
extern const uint64_t sdk_fn_count;
}

namespace loader {
GuestModule g_main, g_sdk;
RelocStats g_rstats[2];
std::vector<InitRecord> g_init_log;
std::vector<std::string> g_unbound;
SvcHandler g_svc_handler = nullptr;
bool g_probe = false;
std::vector<BlockerEvent> g_blockers;
uint64_t g_probe_limit = 0;

std::vector<std::string> g_weak_used;   // weak overrides that bound (no definition anywhere)
static std::vector<Thunk *> g_thunks;
static std::unordered_map<std::string, Thunk *> g_by_name;
static std::vector<uint64_t> g_main_import_target;
static std::vector<GuestFn> g_main_import_fn;   // lifted target of each import (null: thunk / host override); dtab never changes after load
extern "C" volatile int g_fnprof;
extern "C" void snap_fn_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void snap_skips_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void snap_neo_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void snap_highfps_observe(uint64_t a, uint64_t lr, uint64_t x0);
extern "C" void loader_init_hit(uint64_t a);
static thread_local GuestThread *g_cur = nullptr;
thread_local Cpu *loader::g_tls_cpu = nullptr;   // the Cpu of the last host call on this thread (crash reports)

static void die(const char *msg) {
    fprintf(stderr, "loader: %s\n", msg);
    fflush(stderr);
    exit(2);
}

// ------------------------------------------------------------------------------------------------ boot trace

FILE *g_trace_file = nullptr;
bool g_trace_imports = false;
uint64_t g_svc_counts[0x80];
uint64_t g_exc_count = 0;
void (*g_spin_gate_fn)(Cpu *, uint64_t) = nullptr;
uint64_t (*g_ticks_fn)() = nullptr;
static LARGE_INTEGER g_t0;
double trace_now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); QueryPerformanceCounter(&g_t0); return x; }();
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - g_t0.QuadPart) / (double)f.QuadPart;
}
double g_last_trace_time = 0;
void trace(const char *fmt, ...) {
    static std::mutex mu;
    char buf[1536];
    int n = snprintf(buf, sizeof buf, "[%9.4f] ", trace_now());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
    if (len > sizeof buf - 2) len = sizeof buf - 2;
    buf[len++] = '\n';
    std::lock_guard<std::mutex> lk(mu);
    g_last_trace_time = trace_now();
    fwrite(buf, 1, len, stderr);
    if (g_trace_file) { fwrite(buf, 1, len, g_trace_file); fflush(g_trace_file); }
    fflush(stderr);
}

// ------------------------------------------------------------------------------------------------ thunks

static Thunk *new_thunk(const std::string &name) {
    if ((g_thunks.size() + 2) * 16 >= THUNK_SPAN) die("out of thunk slots");
    auto *t = new Thunk();
    t->name = name;
    t->addr = THUNK_BASE + 16 * g_thunks.size();
    g_thunks.push_back(t);
    g_by_name[name] = t;
    return t;
}

Thunk *add_override(const std::string &sym, GuestFn fn) {
    Thunk *t = new_thunk("ovr:" + sym);
    t->fn1 = fn;
    g_by_name["sym:" + sym] = t;
    return t;
}
Thunk *add_weak_override(const std::string &sym, GuestFn fn) {
    Thunk *t = new_thunk("weak:" + sym);
    t->fn1 = fn;
    g_by_name["wsym:" + sym] = t;
    return t;
}
Thunk *add_stub(const std::string &name, void (*fn)(Cpu *, Thunk *)) {
    Thunk *t = new_thunk(name);
    t->fn2 = fn;
    return t;
}
Thunk *find_override(const std::string &sym) {
    auto it = g_by_name.find("sym:" + sym);
    return it == g_by_name.end() ? nullptr : it->second;
}
uint64_t thunk_addr(const std::string &name) {
    auto it = g_by_name.find(name);
    return it == g_by_name.end() ? 0 : it->second->addr;
}

// ------------------------------------------------------------------------------------------------ symbolization

static const GuestModule *module_of(uint64_t a) {
    if (a - g_main.base < g_main.span) return &g_main;
    if (a - g_sdk.base < g_sdk.span) return &g_sdk;
    return nullptr;
}

std::string symbolize(uint64_t a) {
    char buf[512];
    if (a - THUNK_BASE < THUNK_SPAN) {
        uint64_t i = (a - THUNK_BASE) >> 4;
        if (i < g_thunks.size()) return "host:" + g_thunks[i]->name;
        return a == HOST_SENTINEL ? "host:sentinel" : "host:?";
    }
    const GuestModule *m = module_of(a);
    if (!m) {
        snprintf(buf, sizeof buf, "?%#llx", (unsigned long long)a);
        return buf;
    }
    uint64_t off = a - m->base;
    auto it = std::upper_bound(m->by_addr.begin(), m->by_addr.end(), std::make_pair(off, (size_t)~0ull));
    if (it != m->by_addr.begin()) {
        --it;
        const SymInfo &s = m->syms[it->second];
        uint64_t d = off - s.value;
        bool inside = s.size ? d < s.size : d < 0x40;
        snprintf(buf, sizeof buf, "%s+%#llx %s%s+%#llx", m->name, (unsigned long long)off, inside ? "" : "~", s.name.c_str(),
                 (unsigned long long)d);
        return buf;
    }
    snprintf(buf, sizeof buf, "%s+%#llx", m->name, (unsigned long long)off);
    return buf;
}

// ------------------------------------------------------------------------------------------------ ELF

#pragma pack(push, 1)
struct Ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t val; };
struct Rela { uint64_t off; uint64_t info; int64_t addend; };
struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
#pragma pack(pop)

static std::vector<uint8_t> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "loader: cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v((size_t)n);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) die("short read");
    fclose(f);
    return v;
}

static void map_module(GuestModule &m, const char *name, const char *path, uint64_t base) {
    m.name = name;
    m.path = path;
    m.base = base;
    std::vector<uint8_t> raw = read_file(path);
    auto *eh = (const Ehdr *)raw.data();
    if (memcmp(eh->id, "\x7f" "ELF", 4) || eh->machine != 0xB7) die("not an AArch64 ELF");
    const Phdr *ph = (const Phdr *)(raw.data() + eh->phoff);
    uint64_t hi = 0, dynva = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == 1) hi = std::max<uint64_t>(hi, ph[i].vaddr + ph[i].memsz);
        if (ph[i].type == 2) dynva = ph[i].vaddr;
        if (ph[i].type == 0x6474e550) { m.eh_hdr = ph[i].vaddr; m.eh_hdr_size = ph[i].memsz; }
        if (ph[i].type == 1 && (ph[i].flags & 1)) { m.text_off = ph[i].vaddr; m.text_size = ph[i].filesz; }
    }
    m.span = (hi + 0xFFFF) & ~0xFFFFull;
    void *p = VirtualAlloc((void *)base, m.span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void *)base) die("cannot map a module at its guest address");
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == 1) memcpy((uint8_t *)base + ph[i].vaddr, raw.data() + ph[i].off, ph[i].filesz);
    std::unordered_map<int64_t, uint64_t> dyn;
    for (const Dyn *d = (const Dyn *)(base + dynva); d->tag != 0; d++) dyn.emplace(d->tag, d->val);
    const Sym *syms = (const Sym *)(base + dyn[6]);
    const char *strs = (const char *)(base + dyn[5]);
    uint32_t nsyms = dyn.count(4) ? ((const uint32_t *)(base + dyn[4]))[1] : 0;   // DT_HASH nchain
    m.syms.reserve(nsyms);
    for (uint32_t i = 0; i < nsyms; i++) {
        SymInfo s{strs + syms[i].name, syms[i].value, syms[i].size, syms[i].info, syms[i].shndx};
        if (s.shndx && !s.name.empty()) {
            m.defs.emplace(s.name, i);
            if (s.value) m.by_addr.emplace_back(s.value, i);
        }
        m.syms.push_back(std::move(s));
    }
    std::sort(m.by_addr.begin(), m.by_addr.end());
    m.init_fn = dyn.count(12) ? dyn[12] : 0;
    m.init_array = dyn.count(25) ? dyn[25] : 0;
    m.init_count = dyn.count(27) ? dyn[27] / 8 : 0;
    m.rela = dyn[7]; m.relasz = dyn[8];
    m.jmprel = dyn.count(23) ? dyn[23] : 0;
    m.pltrelsz = dyn.count(2) ? dyn[2] : 0;
}

// Direct-indexed dispatch table: one slot per text word (main 22 MB, sdk 12 MB of host memory).
static void build_dtab(GuestModule &m, const uint64_t *addrs, const GuestFn *ptrs, uint64_t n) {
    m.dtab_count = m.text_size >> 2;
    m.dtab = (GuestFn *)VirtualAlloc(nullptr, m.dtab_count * sizeof(GuestFn), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m.dtab) die("cannot allocate a dispatch table");
    m.fn_count = n;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t off = addrs[i] - m.base - m.text_off;
        if ((off >> 2) < m.dtab_count) m.dtab[off >> 2] = ptrs[i];
    }
}

// ------------------------------------------------------------------------------------------------ symbol resolution

static void missing_stub(Cpu *c, Thunk *t);

static uint64_t special_symbol(const std::string &n) {
    // rtld's own data, read by the sdk's runtime support: the relocation tables of the main module
    if (n == "__rel_dyn_start") return g_main.base + g_main.rela;
    if (n == "__rel_dyn_end") return g_main.base + g_main.rela + g_main.relasz;
    if (n == "__rel_plt_start") return g_main.base + g_main.jmprel;
    if (n == "__rel_plt_end") return g_main.base + g_main.jmprel + g_main.pltrelsz;
    return 0;
}

// Precedence (rtld load order, main first): host override, then special rtld symbols, then main's definition, then sdk's. `weak`: an unresolved weak reference is 0. Missing strong references get an aborting stub.
static uint64_t resolve(const std::string &name, bool weak, int who) {
    RelocStats &rs = g_rstats[who];
    if (Thunk *t = find_override(name)) { rs.res_override++; return t->addr; }
    if (uint64_t v = special_symbol(name)) { rs.res_special++; return v; }
    auto it = g_main.defs.find(name);
    if (it != g_main.defs.end()) { rs.res_main++; return g_main.base + g_main.syms[it->second].value; }
    it = g_sdk.defs.find(name);
    if (it != g_sdk.defs.end()) { rs.res_sdk++; return g_sdk.base + g_sdk.syms[it->second].value; }
    auto wt = g_by_name.find("wsym:" + name);
    if (wt != g_by_name.end()) { rs.res_override++; g_weak_used.push_back(name); return wt->second->addr; }
    if (weak) { rs.res_weak0++; g_unbound.push_back(std::string(who ? "sdk" : "main") + " weak->0: " + name); return 0; }
    rs.res_missing++;
    g_unbound.push_back(std::string(who ? "sdk" : "main") + " missing: " + name);
    auto bt = g_by_name.find("missing:" + name);
    Thunk *t = bt != g_by_name.end() ? bt->second : add_stub("missing:" + name, missing_stub);
    return t->addr;
}

uint64_t lookup(const std::string &name) {
    auto it = g_main.defs.find(name);
    if (it != g_main.defs.end()) return g_main.base + g_main.syms[it->second].value;
    it = g_sdk.defs.find(name);
    if (it != g_sdk.defs.end()) return g_sdk.base + g_sdk.syms[it->second].value;
    return 0;
}

std::vector<std::string> shared_symbols() {
    std::vector<std::string> v;
    for (auto &kv : g_main.defs) if (g_sdk.defs.count(kv.first)) v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

static void relocate(GuestModule &m, int who) {
    RelocStats &rs = g_rstats[who];
    const std::pair<uint64_t, uint64_t> tabs[2] = {{m.rela, m.relasz}, {m.jmprel, m.pltrelsz}};
    for (auto &t : tabs) {
        if (!t.first) continue;
        const Rela *r = (const Rela *)(m.base + t.first);
        for (uint64_t i = 0; i < t.second / sizeof(Rela); i++) {
            uint32_t type = (uint32_t)(r[i].info & 0xFFFFFFFF), sym = (uint32_t)(r[i].info >> 32);
            uint64_t *where = (uint64_t *)(m.base + r[i].off);
            if (type == 1027) { *where = m.base + r[i].addend; rs.relative++; continue; }
            if (type != 257 && type != 1025 && type != 1026) { rs.other++; continue; }
            if (!sym) { *where = m.base + r[i].addend; rs.other++; continue; }
            const SymInfo &s = m.syms[sym];
            uint64_t v = resolve(s.name, (s.info >> 4) == 2, who);
            if (s.shndx) rs.own_defined++;
            if (type == 257) { v += r[i].addend; rs.abs64++; }
            else if (type == 1025) rs.glob_dat++;
            else rs.jump_slot++;
            *where = v;
        }
    }
}

bool load(const char *main_elf, const char *sdk_elf) {
    // the thunk range (readable so that guest code can compare against it)
    if (!VirtualAlloc((void *)THUNK_BASE, THUNK_SPAN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) die("cannot map the thunk range");
    map_module(g_main, "main", main_elf, GUEST_MAIN_BASE);
    map_module(g_sdk, "sdk", sdk_elf, GUEST_SDK_BASE);
    build_dtab(g_main, main_fn_addrs, main_fn_ptrs, main_fn_count);
    build_dtab(g_sdk, sdk_fn_addrs, sdk_fn_ptrs, sdk_fn_count);
    // the order of the two passes does not matter to the result, all symbols are known
    relocate(g_sdk, 1);
    relocate(g_main, 0);
    // main's PLT stubs call the loader-resolved target of their import (generated imp_<name> -> rt_import_call)
    g_main_import_target.resize(main_import_count);
    RelocStats saved = g_rstats[0];
    for (uint64_t i = 0; i < main_import_count; i++) {
        const char *nm = main_import_names[i];
        bool weak = false;
        for (const SymInfo &sy : g_main.syms) if (!sy.shndx && sy.name == nm) { weak = (sy.info >> 4) == 2; break; }
        size_t before = g_unbound.size();
        g_main_import_target[i] = resolve(nm, weak, 0);
        g_unbound.resize(before);   // already reported by the relocation pass
    }
    // opt 2026-11-13: resolve lifted import targets once, so a PLT call into sdk skips rt_dispatch's range checks and dtab load
    g_main_import_fn.resize(main_import_count);
    for (uint64_t i = 0; i < main_import_count; i++) g_main_import_fn[i] = lookup_fn(g_main_import_target[i]);
    g_rstats[0] = saved;
    return true;
}

// ------------------------------------------------------------------------------------------------ dispatch

GuestFn lookup_fn(uint64_t pc) {
    uint64_t o = pc - g_main.base - g_main.text_off;
    if (o < g_main.text_size) return g_main.dtab[o >> 2];
    o = pc - g_sdk.base - g_sdk.text_off;
    if (o < g_sdk.text_size) return g_sdk.dtab[o >> 2];
    return nullptr;
}

GuestFn lookup_fn_bsearch(uint64_t pc) {
    const uint64_t *a; const GuestFn *p; uint64_t n;
    if (pc - g_main.base < g_main.span) { a = main_fn_addrs; p = main_fn_ptrs; n = main_fn_count; }
    else { a = sdk_fn_addrs; p = sdk_fn_ptrs; n = sdk_fn_count; }
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (a[mid] < pc) lo = mid + 1; else hi = mid;
    }
    return lo < n && a[lo] == pc ? p[lo] : nullptr;
}

// ------------------------------------------------------------------------------------------------ guest threads and calls

GuestThread *new_thread(size_t stack_size) {
    auto *t = new GuestThread();
    t->c = (Cpu *)_aligned_malloc(sizeof(Cpu), 64);
    memset(t->c, 0, sizeof(Cpu));
    uint8_t *stack = (uint8_t *)VirtualAlloc(nullptr, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *tls = (uint8_t *)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // no PT_TLS in the ELFs
    uint8_t *tlr = (uint8_t *)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);    // Thread Local Region
    uint8_t *tt = (uint8_t *)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);     // ThreadType
    if (!stack || !tls || !tlr || !tt) die("cannot allocate a guest thread");
    t->stack_lo = (uint64_t)stack;
    t->stack_hi = (uint64_t)stack + stack_size;
    // runtime/os/os_layout.h: TLR+0x1f0 = ELF thread pointer, TLR+0x1f8 = ThreadType*; ThreadType state byte at +0x40
    *(uint64_t *)(tlr + 0x1f0) = (uint64_t)(tls + 0x8000);
    *(uint64_t *)(tlr + 0x1f8) = (uint64_t)tt;
    tt[0x40] = 3;                                   // TS_STARTED
    *(uint64_t *)(tt + 0x50) = (uint64_t)stack;     // stack, stack size
    *(uint64_t *)(tt + 0x58) = stack_size;
    t->c->tpidr = (uint64_t)(tls + 0x8000);
    t->c->tpidrro = (uint64_t)tlr;
    t->c->sp = (t->stack_hi - 64) & ~15ull;
    t->c->excl_addr = ~0ull;
    rt_fpcr_sync(t->c);
    g_cur = t;
    return t;
}

// The guest state a nested call may change, restored after it (loader.h call_guest_nested).
struct NestedSave {
    uint64_t x[31], sp, pc, nzcv, excl_addr, excl_val, excl_val_hi, fpcr, fpsr;
    __uint128_t v[32];
    explicit NestedSave(const Cpu *c)
        : sp(c->sp), pc(c->pc), nzcv(c->nzcv), excl_addr(c->excl_addr), excl_val(c->excl_val), excl_val_hi(c->excl_val_hi),
          fpcr(c->fpcr), fpsr(c->fpsr) {
        memcpy(x, c->x, sizeof x);
        memcpy(v, c->v, sizeof v);
    }
    void restore(Cpu *c) const {
        memcpy(c->x, x, sizeof x);
        memcpy(c->v, v, sizeof v);
        c->sp = sp; c->pc = pc; c->nzcv = nzcv;
        c->excl_addr = excl_addr; c->excl_val = excl_val; c->excl_val_hi = excl_val_hi;
        const bool fp = c->fpcr != fpcr;
        c->fpcr = fpcr; c->fpsr = fpsr;
        if (fp) rt_fpcr_sync(c);
    }
};

uint64_t call_guest_nested(Cpu *c, uint64_t fn, std::initializer_list<uint64_t> args) {
    const NestedSave saved(c);
    // Inside rt_inline_hook c->pc carries HOOK_RA_TAG | the hook site's return address: the callee returns there, so the
    // guest unwinder walks on into the hooked function (its LSDA) instead of stopping at HOST_SENTINEL.
    const uint64_t ra = (saved.pc & HOOK_RA_TAG) ? saved.pc & ~HOOK_RA_TAG : 0;
    static bool trace = getenv("SNAP_TRACE") != nullptr;
    if (trace) { fprintf(stderr, "call_guest %s"+0, symbolize(fn).c_str()); fprintf(stderr, " args"); int k = 0; for (uint64_t a : args) { if (k++ < 4) fprintf(stderr, " %#llx", (unsigned long long)a); } fprintf(stderr, "%c", 10); fflush(stderr); }
    int i = 0;
    for (uint64_t a : args) c->x[i++] = a;
    c->x[30] = ra ? ra : HOST_SENTINEL;
    g_tls_cpu = c;
    c->pc = fn;
    rt_dispatch(c);
    if (c->resuming) {
        if (ra) throw GuestResume{};   // c->pc / c->sp name the resume target; rt_inline_hook returns to the generated site
        fprintf(stderr, "loader: an exception or longjmp left guest code for the host (resume target pc=%#llx sp=%#llx)\n",
                (unsigned long long)c->pc, (unsigned long long)c->sp);
        fflush(stderr);
        abort();
    }
    uint64_t r = c->x[0];
    saved.restore(c);
    return r;
}

uint64_t call_guest(Cpu *c, uint64_t fn, std::initializer_list<uint64_t> args) { return call_guest_nested(c, fn, args); }

}  // namespace loader

// Every MAIN_INLINE_HOOKS site (recomp/gen.py Emitter.function inline_hook) runs its hook through this. ra: the return
// address nested guest calls get (0: HOST_SENTINEL). A guest exception or longjmp out of a nested call unwinds the hook
// (GuestResume) and returns with c->resuming set; the site then takes its landing pad or returns, as after a call.
extern "C" void rt_inline_hook(Cpu *c, uint64_t ra, void (*fn)(Cpu *c)) {
    const uint64_t pc = c->pc;
    c->pc = ra ? ra | loader::HOOK_RA_TAG : 0;
    try {
        fn(c);
    } catch (const loader::GuestResume &) {
        return;
    }
    c->pc = pc;
}

namespace loader {

// ------------------------------------------------------------------------------------------------ initializers

// Initializer entries seen by the FN_ENTER hook while DT_INIT runs them: address -> (module, index).
static std::unordered_map<uint64_t, std::pair<const char *, int>> g_init_watch;
static std::unordered_map<uint64_t, int> g_init_seen;

// rtld order: sdk, then main. Each module's DT_INIT (`_init`) walks that module's init_array itself (checked on main: the
// generated f_7100000030 loops over the table and dispatches each entry, guarded by a flag word), so calling the array again
// afterwards would run every static initializer twice (and corrupt lists that assume one run). With log, each entry is
// reported with a timestamp the first time it is entered.
int run_initializers(Cpu *c, bool log) {
    int n = 0;
    GuestModule *mods[2] = {&g_sdk, &g_main};
    for (GuestModule *m : mods) {
        const uint64_t *arr = (const uint64_t *)(m->base + m->init_array);
        for (uint64_t i = 0; i < m->init_count; i++) {
            uint64_t fn = arr[i];
            if (fn == 0 || fn == ~0ull) continue;
            g_init_watch[fn] = {m->name, (int)i};
        }
    }
    const int fnprof_before = g_fnprof;
    if (log) g_fnprof = 1;
    for (GuestModule *m : mods) {
        if (m->init_fn) {
            g_init_log.push_back({m->name, -1, m->base + m->init_fn});
            trace("init %s DT_INIT %#llx %s (%llu init_array entries)", m->name, (unsigned long long)(m->base + m->init_fn),
                  symbolize(m->base + m->init_fn).c_str(), (unsigned long long)m->init_count);
            call_guest(c, m->base + m->init_fn);
            n++;
        } else {
            const uint64_t *arr = (const uint64_t *)(m->base + m->init_array);   // no DT_INIT: call the array
            for (uint64_t i = 0; i < m->init_count; i++) if (arr[i] && arr[i] != ~0ull) { call_guest(c, arr[i]); n++; }
        }
    }
    int seen = (int)g_init_seen.size();
    trace("init: DT_INIT of sdk and main done; %d of %zu init_array entries were entered", seen, g_init_watch.size());
    // Load pass (2026-11-13): the init log turned the FN_ENTER hook on for the whole run, so every lifted function entry paid
    // fnprof_hit (a hash lookup in g_init_watch plus three observer calls), about a fifth of guest CPU in a host profile. Turn it
    // off again unless a tracer had it on before (SNAP_FNTRACE, SNAP_TRACE_LOD, SNAP_SKIPS_REC, SNAP_NEO_CAMFN set it at start).
    // SNAP_FNPROF_KEEP=1 restores the old always-on hook.
    if (log && !fnprof_before && !getenv("SNAP_FNPROF_KEEP")) {
        // Only switch the hook off; leave g_init_watch in place so a guest thread still inside
        // loader_init_hit never sees the unordered_map being cleared under it.
        g_fnprof = 0;
    }
    return n + seen;
}

}  // namespace loader

// ------------------------------------------------------------------------------------------------ extern "C" runtime hooks

using namespace loader;

static void record_blocker(const char *kind, const std::string &what, Cpu *c, uint64_t pc) {
    BlockerEvent e;
    e.kind = kind;
    e.what = what;
    e.caller = symbolize(c->x[30]);
    for (int i = 0; i < 4; i++) e.args[i] = c->x[i];
    e.chain = guest_chain(c, pc, 14);
    e.api = blocker_api(c, pc);
    g_blockers.push_back(e);
    fprintf(stderr, "BLOCKER #%zu %s %s\n  args x0..x3 = %#llx %#llx %#llx %#llx\n", g_blockers.size(), kind, what.c_str(),
            (unsigned long long)e.args[0], (unsigned long long)e.args[1], (unsigned long long)e.args[2], (unsigned long long)e.args[3]);
    for (size_t i = 0; i < e.chain.size(); i++) fprintf(stderr, "    #%zu %s\n", i, e.chain[i].c_str());
    fprintf(stderr, "NEXT-BLOCKER %s %s\n", kind, e.api.c_str());
    fflush(stderr);
    if (g_probe_limit && g_blockers.size() >= g_probe_limit) {
        fprintf(stderr, "loader: probe limit of %llu events reached\n", (unsigned long long)g_probe_limit);
        fflush(stderr);
        exit(3);
    }
}

namespace loader {
// A frame pointer is plausible if it lies in this thread's guest stack: the thread created by new_thread, or (threads made by
// runtime/os, whose stacks come from the guest) within 64 MB above the current sp.
static bool fp_ok(Cpu *c, uint64_t fp) {
    if (fp & 7) return false;
    // a thread that has not run guest code yet has sp = 0 and fp = 0: the stall watchdog's thread dump read [0+8] and crashed
    // (2026-11-12/13, 6 runs, "CRASH on guest thread type 0" right after a STALL dump)
    if (fp < 0x10000 || c->sp < 0x10000) return false;
    if (g_cur) return fp >= g_cur->stack_lo && fp + 16 <= g_cur->stack_hi;
    return fp >= c->sp && fp - c->sp < (64ull << 20);
}
std::vector<std::string> guest_chain(Cpu *c, uint64_t pc, int depth) {
    std::vector<std::string> out;
    out.push_back(symbolize(pc));
    out.push_back(symbolize(c->x[30]) + " (lr)");
    uint64_t fp = c->x[29];
    for (int i = 0; i < depth; i++) {
        if (!fp_ok(c, fp)) break;
        if (IsBadReadPtr((void *)fp, 16)) break;   // another thread's stack read by the stall dump can be gone (thread exited)
        uint64_t lr = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];
        if (lr == 0 || lr == HOST_SENTINEL) { out.push_back(lr ? "host:sentinel" : "(end)"); break; }
        out.push_back(symbolize(lr));
        if (next <= fp) break;
        fp = next;
    }
    return out;
}
void print_chain(Cpu *c, uint64_t pc, int depth) {
    for (auto &s : guest_chain(c, pc, depth)) fprintf(stderr, "    %s\n", s.c_str());
}

static bool exported_inside(uint64_t a, std::string &name) {
    const GuestModule *m = module_of(a);
    if (!m) return false;
    uint64_t off = a - m->base;
    auto it = std::upper_bound(m->by_addr.begin(), m->by_addr.end(), std::make_pair(off, (size_t)~0ull));
    if (it == m->by_addr.begin()) return false;
    --it;
    const SymInfo &sy = m->syms[it->second];
    if (!sy.size || off - sy.value >= sy.size) return false;
    name = sy.name;
    return true;
}

std::string blocker_api(Cpu *c, uint64_t pc) {
    std::vector<uint64_t> frames{pc, c->x[30]};
    uint64_t fp = c->x[29];
    for (int i = 0; i < 24; i++) {
        if (!fp_ok(c, fp)) break;
        uint64_t lr = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];
        if (!lr || lr == HOST_SENTINEL) break;
        frames.push_back(lr);
        if (next <= fp) break;
        fp = next;
    }
    std::string any, n;
    for (uint64_t f : frames) {
        if (!exported_inside(f, n)) continue;
        if (n.rfind("_ZN2nn", 0) == 0 || n.rfind("nn", 0) == 0 && n.size() > 2 && n[2] >= 'a' && n[2] <= 'z' && n.find('_') == std::string::npos) return n;
        if (any.empty() && n.rfind("_ZNSt3__1", 0) != 0) any = n;
    }
    return any;
}

static void probe_stub_fn(Cpu *c, Thunk *t);
void add_probe_stub(const std::string &sym) {
    Thunk *t = add_stub("probe:" + sym, probe_stub_fn);
    g_by_name["sym:" + sym] = t;
}

const char *svc_name(uint32_t imm) {
    static const char *names[0x80] = {};
    static bool init = false;
    if (!init) {
        init = true;
        struct { uint32_t n; const char *s; } t[] = {
            {1, "SetHeapSize"}, {2, "SetMemoryPermission"}, {3, "SetMemoryAttribute"}, {4, "MapMemory"}, {5, "UnmapMemory"},
            {6, "QueryMemory"}, {7, "ExitProcess"}, {8, "CreateThread"}, {9, "StartThread"}, {10, "ExitThread"},
            {11, "SleepThread"}, {12, "GetThreadPriority"}, {13, "SetThreadPriority"}, {14, "GetThreadCoreMask"},
            {15, "SetThreadCoreMask"}, {16, "GetCurrentProcessorNumber"}, {17, "SignalEvent"}, {18, "ClearEvent"},
            {19, "MapSharedMemory"}, {20, "UnmapSharedMemory"}, {21, "CreateTransferMemory"}, {22, "CloseHandle"},
            {23, "ResetSignal"}, {24, "WaitSynchronization"}, {25, "CancelSynchronization"}, {26, "ArbitrateLock"},
            {27, "ArbitrateUnlock"}, {28, "WaitProcessWideKeyAtomic"}, {29, "SignalProcessWideKey"}, {30, "GetSystemTick"},
            {31, "ConnectToNamedPort"}, {33, "SendSyncRequest"}, {34, "SendSyncRequestWithUserBuffer"},
            {35, "SendAsyncRequestWithUserBuffer"}, {37, "GetThreadId"}, {38, "Break"}, {39, "OutputDebugString"},
            {41, "GetInfo"}, {42, "FlushEntireDataCache"}, {44, "MapPhysicalMemory"}, {45, "UnmapPhysicalMemory"},
            {47, "GetLastThreadInfo"}, {50, "SetThreadActivity"}, {51, "GetThreadContext3"}, {52, "WaitForAddress"},
            {53, "SignalToAddress"}, {54, "SynchronizePreemptionState"}, {64, "CreateSession"}, {65, "AcceptSession"},
            {67, "ReplyAndReceive"}, {68, "ReplyAndReceiveWithUserBuffer"}, {69, "CreateEvent"}, {72, "MapPhysicalMemoryUnsafe?"},
            {73, "UnmapPhysicalMemoryUnsafe?"}, {75, "?"}, {80, "CreateDeviceAddressSpace"}, {81, "AttachDeviceAddressSpace"},
            {82, "DetachDeviceAddressSpace"}, {83, "MapDeviceAddressSpaceByForce"}, {112, "GetSystemInfo?"}, {114, "?"}};
        for (auto &e : t) if (e.n < 0x80) names[e.n] = e.s;
    }
    return imm < 0x80 && names[imm] ? names[imm] : "svc";
}
}  // namespace loader

extern "C" {
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t n = imm < 0x80 ? ++g_svc_counts[imm] : 1;
    if (g_trace_imports && n <= 3)
        trace("svc %#x %s #%llu at %s x0=%#llx x1=%#llx x2=%#llx", imm, svc_name(imm), (unsigned long long)n, symbolize(pc).c_str(),
              (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    if (g_svc_handler && g_svc_handler(c, imm)) return;
    char what[96];
    snprintf(what, sizeof what, "SVC %#x %s (stub at %s)", imm, svc_name(imm), symbolize(pc).c_str());
    if (g_probe) {
        record_blocker("svc", what, c, pc);
        c->x[0] = 0; c->x[1] = 0; c->x[2] = 0; c->x[3] = 0;
        return;
    }
    fprintf(stderr, "SVC backstop: %s, caller lr=%#llx\n", what, (unsigned long long)c->x[30]);
    print_chain(c, pc, 14);
    fflush(stderr);
    abort();
}
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    fprintf(stderr, "BRK %#x at %s lr=%s\n", imm, symbolize(pc).c_str(), symbolize(c->x[30]).c_str());
    print_chain(c, pc, 14);
    fflush(stderr);
    abort();
}
void rt_udf(Cpu *c, uint64_t pc) {
    fprintf(stderr, "UDF trap at %s lr=%s\n", symbolize(pc).c_str(), symbolize(c->x[30]).c_str());
    print_chain(c, pc, 14);
    fflush(stderr);
    abort();
}
void rt_unresolved(Cpu *c, uint64_t addr, const char *what) {
    // feed-back list for re-lifting: one TSV line per miss (target, its module offset, caller, what)
    if (FILE *f = fopen("analysis/recomp/indirect_misses.tsv", "ab")) {
        const GuestModule *m = addr - g_main.base < g_main.span ? &g_main : addr - g_sdk.base < g_sdk.span ? &g_sdk : nullptr;
        fprintf(f, "%#llx%s%s%s%#llx%s%s%s%s%s", (unsigned long long)addr, "\t", m ? m->name : "-", "\t", (unsigned long long)(m ? addr - m->base : 0), "\t", symbolize(c->x[30]).c_str(), "\t", what, "\n");
        fclose(f);
    }
    fprintf(stderr, "UNRESOLVED call to %#llx (%s), lr=%s\n", (unsigned long long)addr, what, symbolize(c->x[30]).c_str());
    print_chain(c, addr, 14);
    fflush(stderr);
    abort();
}
volatile int g_fnprof;
static void unused_fnprof_marker() {}
// Debug: SNAP_FNTRACE=0xaddr[,0xaddr...] logs every entry (and its caller) of those lifted functions (FN_ENTER hook).
static uint64_t g_fntrace[8];
static int g_fntrace_n;
void loader_enable_fntrace() {
    const char *e = getenv("SNAP_FNTRACE");
    while (e && *e && g_fntrace_n < 8) {
        char *end;
        g_fntrace[g_fntrace_n++] = strtoull(e, &end, 0);
        e = *end == ',' ? end + 1 : nullptr;
    }
    if (g_fntrace_n) g_fnprof = 1;
}
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    loader_init_hit(a);
    snap_fn_observe(a, lr, x0);   // host/mods/lodanim.cpp (SNAP_TRACE_LOD); returns at once when unused
    snap_skips_observe(a, lr, x0);   // host/mods/skips.cpp (SNAP_SKIPS_REC)
    snap_neo_observe(a, lr, x0);   // host/mods/neofilm.cpp (dev tracer SNAP_NEO_CAMFN)
    snap_highfps_observe(a, lr, x0);   // host/mods/highfps.cpp (SNAP_FPS_TRACE / SNAP_FPS_MSWATCH)
    for (int i = 0; i < g_fntrace_n; i++)
        if (g_fntrace[i] == a) { loader::trace("fn %#llx entered from %s x0=%#llx", (unsigned long long)a, loader::symbolize(lr).c_str(), (unsigned long long)x0); return; }
}
void rt_spin_gate(Cpu *c, uint64_t head) { if (g_spin_gate_fn) g_spin_gate_fn(c, head); }
uint64_t rt_ticks(void) {
    if (g_ticks_fn) return g_ticks_fn();
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    uint64_t q = (uint64_t)n.QuadPart, fq = (uint64_t)f.QuadPart;   // cntpct_el0: 19.2 MHz
    return (q / fq) * 19200000ull + (q % fq) * 19200000ull / fq;
}

// SNAP_MXCSR_CHECK=1 (debug, off by default): lifted FP code relies on MXCSR matching the guest FPCR (cpu.h
// rt_fpcr_sync: FTZ/DAZ = FPCR.FZ, RC = nearest, exceptions masked). Every host override (HLE, NVN bridge, audio, os)
// returns to guest code through the thunk call below; this logs each override that comes back with a different
// MXCSR control word (status flags 0-5 ignored), once per override, and restores the value it had on entry.
static int g_mxcsr_check = [] { const char *e = getenv("SNAP_MXCSR_CHECK"); return e && *e == '1'; }();
static void mxcsr_checked_call(Cpu *c, Thunk *th) {
    unsigned before = __builtin_ia32_stmxcsr();
    unsigned want = (c->fpcr & (1u << 24) ? 0x8040u : 0) | 0x1F80u;   // FTZ|DAZ per FPCR.FZ, RC nearest, all masked
    if ((before & 0xFFC0u) != want) {
        static std::atomic<int> once{0};
        if (!once.exchange(1)) {
            fprintf(stderr, "mxcsr: guest code reached %s with MXCSR %#x, want %#x\n", th->name.c_str(), before, want);
            fflush(stderr);
        }
    }
    if (th->fn1) th->fn1(c); else th->fn2(c, th);
    unsigned after = __builtin_ia32_stmxcsr();
    if ((after ^ before) & ~0x3Fu) {
        static std::mutex mu;
        static std::unordered_set<std::string> seen;
        std::lock_guard<std::mutex> lk(mu);
        if (seen.insert(th->name).second) {
            fprintf(stderr, "mxcsr: %s changed MXCSR %#x -> %#x (guest fpcr %#llx); restored\n", th->name.c_str(), before, after,
                    (unsigned long long)c->fpcr);
            fflush(stderr);
        }
        __builtin_ia32_ldmxcsr((after & 0x3Fu) | (before & ~0x3Fu));
    }
}
void rt_dispatch(Cpu *c) {
    uint64_t pc = c->pc;
    GuestFn f = lookup_fn(pc);
    if (__builtin_expect(f != nullptr, 1)) { f(c); return; }
    uint64_t t = pc - THUNK_BASE;
    if (t < g_thunks.size() * 16 && !(t & 15)) {
        Thunk *th = g_thunks[t >> 4];
        g_tls_cpu = c;
        // the count saturates at 2 (only "first call" is ever read: here and probe_stub_fn's calls == 1), so a hot import no longer
        // writes its Thunk on every call from several threads (opt 2026-11-13)
        uint64_t n = th->calls;
        if (__builtin_expect(n < 2, 0)) th->calls = n + 1;
        if (n == 0 && g_trace_imports)
            trace("import %s called from %s x0=%#llx x1=%#llx x2=%#llx x3=%#llx", th->name.c_str(), symbolize(c->x[30]).c_str(),
                  (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
        if (__builtin_expect(g_mxcsr_check, 0)) { mxcsr_checked_call(c, th); return; }
        if (th->fn1) th->fn1(c); else th->fn2(c, th);
        return;
    }
    rt_unresolved(c, pc, "indirect branch/call (rt_dispatch): not a function start");
}

void rt_import_call(Cpu *c, uint32_t idx) {
    c->pc = g_main_import_target[idx];
    if (GuestFn f = g_main_import_fn[idx]) { f(c); return; }
    rt_dispatch(c);
}
}

// a missing override: the import has no definition anywhere (strong, undefined in both modules)
namespace loader {
static void missing_stub(Cpu *c, Thunk *t) {
    std::string what = "missing override " + t->name.substr(8);
    if (g_probe) {
        record_blocker("missing", what, c, t->addr);
        c->x[0] = 0;
        return;
    }
    fprintf(stderr, "MISSING OVERRIDE %s called, caller %s, args %#llx %#llx %#llx %#llx\n", t->name.c_str() + 8, symbolize(c->x[30]).c_str(),
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    print_chain(c, t->addr, 14);
    fflush(stderr);
    abort();
}
}  // namespace loader

namespace loader {
static void probe_stub_fn(Cpu *c, Thunk *t) {
    if (t->calls == 1) record_blocker("stub-hit", t->name.substr(6), c, t->addr);
    c->x[0] = 0;
}
}  // namespace loader

extern "C" void loader_init_hit(uint64_t a) {
    using namespace loader;
    if (g_init_watch.empty()) return;
    auto it = g_init_watch.find(a);
    if (it == g_init_watch.end()) return;
    int k = ++g_init_seen[a];
    trace("init %s[%d] %#llx %s%s", it->second.first, it->second.second, (unsigned long long)a, symbolize(a).c_str(), k > 1 ? " (AGAIN)" : "");
}
