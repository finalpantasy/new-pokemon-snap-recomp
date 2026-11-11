// Host-boundary spike (project code, ours): loads the Snap `main` ELF at its guest address, applies relocations, sets up
// TLS and a stack, binds five imports to host code and calls the lifted NVN bootstrap (0x910270) natively.
// The runtime types (Cpu, GuestFn, rt_*) come from runtime/cpu.h (TotK/LAS, MIT). Not for distribution with game data.
//
// Usage: snap_spike.exe <main.elf> <out.tsv>
// Output (tsv): "bootstrap <n> <name>" per nvnBootstrapLoader request, "name <n> <name>" per mock getProcAddress request in
// order, "slot <module offset> <name>" for every 8-byte image slot that holds a mock proc, "import_call", "reloc", "result".
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "cpu.h"
extern const uint64_t main_fn_addrs[];
extern const GuestFn main_fn_ptrs[];
extern const uint64_t main_fn_count;
extern const char *const main_import_names[];
extern const uint64_t main_import_stub_addrs[];
extern const uint64_t main_import_count;
void f_7100910270(Cpu *c);
}

static const uint64_t GUEST_BASE = 0x7100000000ull;   // module offset 0
static const uint64_t THUNK_BASE = 0x7300000000ull;   // host functions callable through guest pointers (16 bytes each)
static const uint64_t STACK_SIZE = 1u << 20;

// ---------------------------------------------------------------------------------------------------- host thunks
struct Thunk {
    void (*fn)(Cpu *, Thunk *);
    std::string name;
    uint64_t addr;
    uint64_t calls = 0;
};
static std::vector<Thunk *> g_thunks;
static std::map<std::string, Thunk *> g_by_name;

static Thunk *make_thunk(const std::string &name, void (*fn)(Cpu *, Thunk *)) {
    auto *t = new Thunk{fn, name, THUNK_BASE + 16 * g_thunks.size()};
    g_thunks.push_back(t);
    g_by_name[name] = t;
    return t;
}

static void die(const char *what) {
    fprintf(stderr, "spike: %s\n", what);
    fflush(stderr);
    exit(2);
}

// ---------------------------------------------------------------------------------------------------- runtime hooks
extern "C" {
volatile int g_fnprof;
void fnprof_hit(uint64_t, uint64_t, uint64_t) {}
uint64_t rt_ticks(void) { return 0; }
void rt_spin_gate(Cpu *, uint64_t) {}
void rt_svc(Cpu *, uint32_t imm, uint64_t pc) { fprintf(stderr, "SVC %#x at %#llx\n", imm, (unsigned long long)pc); abort(); }
void rt_brk(Cpu *, uint32_t imm, uint64_t pc) { fprintf(stderr, "BRK %#x at %#llx\n", imm, (unsigned long long)pc); abort(); }
void rt_udf(Cpu *, uint64_t pc) { fprintf(stderr, "UDF trap at %#llx\n", (unsigned long long)pc); abort(); }

void rt_unresolved(Cpu *c, uint64_t addr, const char *what) {
    fprintf(stderr, "UNRESOLVED call to %#llx (%s), lr=%#llx\n", (unsigned long long)addr, what, (unsigned long long)c->x[30]);
    fflush(stderr);
    abort();
}

// Indirect call or tail call to the guest address in c->pc: a host thunk, or a lifted function found by binary search.
void rt_dispatch(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= THUNK_BASE && pc < THUNK_BASE + 16 * g_thunks.size() && ((pc - THUNK_BASE) & 15) == 0) {
        Thunk *t = g_thunks[(pc - THUNK_BASE) >> 4];
        t->calls++;
        t->fn(c, t);
        return;
    }
    uint64_t lo = 0, hi = main_fn_count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (main_fn_addrs[mid] < pc) lo = mid + 1; else hi = mid;
    }
    if (lo < main_fn_count && main_fn_addrs[lo] == pc) { main_fn_ptrs[lo](c); return; }
    rt_unresolved(c, pc, "indirect branch/call (rt_dispatch): not a function start");
}
}

// ---------------------------------------------------------------------------------------------------- the five bound imports
// The PLT stubs of the generated plt_stubs.c call a weak imp_<name>(Cpu *); these definitions replace the default
// log-and-abort ones. Arguments are in c->x[0..7], the result goes to c->x[0].
static std::map<std::string, uint64_t> g_import_calls;
static std::vector<std::string> g_proc_requests;       // names asked of the mock getProcAddress, in order
static std::vector<std::string> g_bootstrap_requests;  // names asked of nvnBootstrapLoader

static void mock_proc(Cpu *c, Thunk *) { c->x[0] = 0; }   // every mock NVN proc: a no-op returning 0

static Thunk *proc_thunk(const std::string &name) {
    auto it = g_by_name.find("nvn:" + name);
    if (it != g_by_name.end()) return it->second;
    return make_thunk("nvn:" + name, mock_proc);
}

// mock `nvnDeviceGetProcAddress(device, name)`: records the name, returns a per-name mock proc
static void mock_get_proc_address(Cpu *c, Thunk *) {
    const char *name = (const char *)(uintptr_t)c->x[1];
    g_proc_requests.push_back(name ? name : "(null)");
    c->x[0] = name ? proc_thunk(name)->addr : 0;
}

extern "C" {
void imp_nvnBootstrapLoader(Cpu *c) {
    g_import_calls["nvnBootstrapLoader"]++;
    const char *name = (const char *)(uintptr_t)c->x[0];
    g_bootstrap_requests.push_back(name ? name : "(null)");
    if (name && !strcmp(name, "nvnDeviceGetProcAddress")) {
        static Thunk *gp = make_thunk("mock_getProcAddress", mock_get_proc_address);
        c->x[0] = gp->addr;
    } else {
        c->x[0] = name ? proc_thunk(name)->addr : 0;   // e.g. nvnDeviceInitialize
    }
}
void imp_memalign(Cpu *c) {
    g_import_calls["memalign"]++;
    c->x[0] = (uint64_t)(uintptr_t)_aligned_malloc((size_t)c->x[1], (size_t)c->x[0]);
}
void imp_memcpy(Cpu *c) { g_import_calls["memcpy"]++; memcpy((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
void imp_memmove(Cpu *c) { g_import_calls["memmove"]++; memmove((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
void imp_memset(Cpu *c) { g_import_calls["memset"]++; memset((void *)(uintptr_t)c->x[0], (int)c->x[1], (size_t)c->x[2]); }
}

// ---------------------------------------------------------------------------------------------------- ELF loader
#pragma pack(push, 1)
struct Ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t val; };
struct Rela { uint64_t off; uint64_t info; int64_t addend; };
struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
#pragma pack(pop)

struct Image {
    uint8_t *base = nullptr;    // == GUEST_BASE
    uint64_t span = 0;
    std::map<std::string, uint64_t> reloc_by_type;
};

static std::vector<uint8_t> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open the ELF");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v((size_t)n);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) die("short read");
    fclose(f);
    return v;
}

static void unbound_import(Cpu *c, Thunk *th) {
    fprintf(stderr, "UNBOUND import %s called, lr=%#llx\n", th->name.c_str(), (unsigned long long)c->x[30]);
    abort();
}

static void load_image(const std::vector<uint8_t> &raw, Image &im) {
    auto *eh = (const Ehdr *)raw.data();
    if (memcmp(eh->id, "\x7f" "ELF", 4) || eh->machine != 0xB7) die("not an AArch64 ELF");
    const Phdr *ph = (const Phdr *)(raw.data() + eh->phoff);
    uint64_t hi = 0, dynva = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == 1) hi = std::max<uint64_t>(hi, ph[i].vaddr + ph[i].memsz);
        if (ph[i].type == 2) dynva = ph[i].vaddr;
    }
    im.span = (hi + 0xFFFF) & ~0xFFFFull;
    // Guest pointer == host pointer: the module lives at its guest address.
    im.base = (uint8_t *)VirtualAlloc((void *)GUEST_BASE, im.span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (im.base != (uint8_t *)GUEST_BASE) die("cannot map the module at its guest address");
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == 1) memcpy(im.base + ph[i].vaddr, raw.data() + ph[i].off, ph[i].filesz);
    // relocations through PT_DYNAMIC (the dynamic section lives in the image)
    std::map<int64_t, uint64_t> dyn;
    for (const Dyn *d = (const Dyn *)(im.base + dynva); d->tag != 0; d++) dyn.emplace(d->tag, d->val);
    const Sym *syms = (const Sym *)(im.base + dyn[6]);        // DT_SYMTAB
    const char *strs = (const char *)(im.base + dyn[5]);      // DT_STRTAB
    std::map<std::string, uint64_t> stub_of;
    for (uint64_t i = 0; i < main_import_count; i++) stub_of[main_import_names[i]] = main_import_stub_addrs[i];
    const int64_t tabs[2][2] = {{7, 8}, {23, 2}};             // DT_RELA/RELASZ, DT_JMPREL/PLTRELSZ
    for (auto &t : tabs) {
        if (!dyn.count(t[0])) continue;
        const Rela *r = (const Rela *)(im.base + dyn[t[0]]);
        for (uint64_t i = 0; i < dyn[t[1]] / sizeof(Rela); i++) {
            uint32_t type = (uint32_t)(r[i].info & 0xFFFFFFFF), sym = (uint32_t)(r[i].info >> 32);
            uint64_t *where = (uint64_t *)(im.base + r[i].off);
            uint64_t val;
            if (type == 1027) {                                // R_AARCH64_RELATIVE
                val = GUEST_BASE + r[i].addend;
                im.reloc_by_type["RELATIVE"]++;
            } else if (type == 257 || type == 1025 || type == 1026) {   // ABS64, GLOB_DAT, JUMP_SLOT
                const Sym &s = syms[sym];
                const char *nm = strs + s.name;
                if (s.shndx) {
                    val = GUEST_BASE + s.value + (type == 257 ? r[i].addend : 0);
                    im.reloc_by_type["defined"]++;
                } else {
                    // import: the PLT stub if it has one (a guest function address rt_dispatch resolves), else an aborting thunk
                    auto it = stub_of.find(nm);
                    if (it != stub_of.end()) val = it->second;
                    else {
                        auto bt = g_by_name.find(std::string("unbound:") + nm);
                        Thunk *th = bt != g_by_name.end() ? bt->second : make_thunk(std::string("unbound:") + nm, unbound_import);
                        val = th->addr;
                    }
                    if (type == 257) val += r[i].addend;
                    im.reloc_by_type["import"]++;
                }
            } else {
                im.reloc_by_type["other"]++;
                continue;
            }
            *where = val;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s main.elf out.tsv\n", argv[0]); return 2; }
    // thunk range: committed so the guest pointers are real, readable addresses
    if (!VirtualAlloc((void *)THUNK_BASE, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) die("cannot map the thunk range");
    Image im;
    load_image(read_file(argv[1]), im);

    // TLS block (Horizon thread-local region, 0x200 bytes, plus a user area) for tpidr_el0 / tpidrro_el0, and the stack
    uint8_t *tls = (uint8_t *)VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *stack = (uint8_t *)VirtualAlloc(nullptr, STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    static Cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.tpidr = (uint64_t)(uintptr_t)tls;
    cpu.tpidrro = (uint64_t)(uintptr_t)tls;
    cpu.sp = ((uint64_t)(uintptr_t)stack + STACK_SIZE - 64) & ~15ull;
    cpu.excl_addr = ~0ull;
    static uint8_t arg[64];           // x0 of 0x910270: a struct whose byte at +8 is the "debugger enabled" flag
    cpu.x[0] = (uint64_t)(uintptr_t)arg;

    f_7100910270(&cpu);               // the lifted NVN bootstrap, called natively

    FILE *o = fopen(argv[2], "w");
    if (!o) die("cannot write the result");
    for (size_t i = 0; i < g_bootstrap_requests.size(); i++) fprintf(o, "bootstrap\t%zu\t%s\n", i, g_bootstrap_requests[i].c_str());
    for (size_t i = 0; i < g_proc_requests.size(); i++) fprintf(o, "name\t%zu\t%s\n", i, g_proc_requests[i].c_str());
    // every 8-byte slot of the image's data+bss holding a mock proc pointer
    const uint64_t *w = (const uint64_t *)im.base;
    uint64_t slots = 0;
    for (uint64_t i = 0xc61000 / 8; i < im.span / 8; i++) {
        uint64_t v = w[i];
        if (v >= THUNK_BASE && v < THUNK_BASE + 16 * g_thunks.size() && ((v - THUNK_BASE) & 15) == 0) {
            const Thunk *t = g_thunks[(v - THUNK_BASE) >> 4];
            fprintf(o, "slot\t%#llx\t%s\n", (unsigned long long)(i * 8), t->name.c_str());
            slots++;
        }
    }
    for (auto &kv : g_import_calls) fprintf(o, "import_call\t%s\t%llu\n", kv.first.c_str(), (unsigned long long)kv.second);
    for (auto &kv : im.reloc_by_type) fprintf(o, "reloc\t%s\t%llu\n", kv.first.c_str(), (unsigned long long)kv.second);
    fprintf(o, "result\tslots\t%llu\nresult\tproc_requests\t%zu\nresult\tthunks\t%zu\nresult\tfinal_x0\t%#llx\n", (unsigned long long)slots,
            g_proc_requests.size(), g_thunks.size(), (unsigned long long)cpu.x[0]);
    fclose(o);
    fprintf(stderr, "spike: bootstrap done, %zu getProcAddress requests, %llu slots hold mock procs\n", g_proc_requests.size(), (unsigned long long)slots);
    return 0;
}
