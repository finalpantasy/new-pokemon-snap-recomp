// Two-module loader and runtime boundary for the lifted New Pokemon Snap `main` and `sdk` (project code, ours).
// Replaces what Horizon's rtld does: maps both modules with guest pointer == host pointer, applies relocations with
// symbol resolution across modules, binds host overrides, runs the initializers in rtld's order, and provides the
// dispatch, SVC backstop, missing-override stubs and the unwinder / setjmp hooks (docs/runtime-boundary.md).
// The Cpu, GuestFn and rt_* declarations come from runtime/cpu.h (TotK/LAS, MIT).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "cpu.h"
}

constexpr uint64_t GUEST_MAIN_BASE = 0x7100000000ull;
constexpr uint64_t GUEST_SDK_BASE = 0x7110000000ull;
constexpr uint64_t THUNK_BASE = 0x7300000000ull;   // host functions callable through guest pointers, 16 bytes each
constexpr uint64_t THUNK_SPAN = 0x100000;
constexpr uint64_t HOST_SENTINEL = THUNK_BASE + THUNK_SPAN - 16;   // return address of a host-to-guest call: no FDE

struct Thunk {
    GuestFn fn1 = nullptr;                   // host override: arguments in c->x[0..7], result in c->x[0]
    void (*fn2)(Cpu *, Thunk *) = nullptr;   // internal stubs that need their own name
    std::string name;
    uint64_t addr = 0;
    uint64_t calls = 0;
};

struct SymInfo {
    std::string name;
    uint64_t value, size;
    uint8_t info;
    uint16_t shndx;
};

struct GuestModule {
    const char *name = "";
    uint64_t base = 0, span = 0;
    uint64_t text_off = 0, text_size = 0;
    uint64_t eh_hdr = 0, eh_hdr_size = 0;     // PT_GNU_EH_FRAME, module offsets
    std::vector<SymInfo> syms;
    std::unordered_map<std::string, size_t> defs;   // defined dynsyms
    std::vector<std::pair<uint64_t, size_t>> by_addr;   // (value, sym index) of defined symbols with a value, sorted
    GuestFn *dtab = nullptr;                  // one entry per text word: the lifted function that starts there
    uint64_t dtab_count = 0;
    uint64_t init_fn = 0, init_array = 0, init_count = 0;   // DT_INIT, DT_INIT_ARRAY (module offsets)
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    uint64_t fn_count = 0;                    // lifted functions in the table
    std::string path;
};

struct RelocStats {
    uint64_t relative = 0, abs64 = 0, glob_dat = 0, jump_slot = 0, other = 0;
    uint64_t res_override = 0, res_sdk = 0, res_main = 0, res_special = 0, res_missing = 0, res_weak0 = 0;
    uint64_t own_defined = 0;
};

namespace loader {
// Boot trace: timestamped lines on stderr and, if g_trace_file is set, in that file. g_trace_imports enables the first-call
// log of every host function (imports, overrides) and the SVC log.
extern thread_local Cpu *g_tls_cpu;   // Cpu of the last host call on this thread
extern FILE *g_trace_file;
extern bool g_trace_imports;
extern uint64_t g_svc_counts[0x80];
extern uint64_t g_exc_count;
double trace_now();
extern double g_last_trace_time;   // time of the last trace line (seconds)
void trace(const char *fmt, ...);
// Optional runtime services (the nn::os layer sets these): rt_spin_gate and rt_ticks forward to them.
extern void (*g_spin_gate_fn)(Cpu *, uint64_t);
extern uint64_t (*g_ticks_fn)();
extern GuestModule g_main, g_sdk;
extern RelocStats g_rstats[2];   // main, sdk

// Host override registry. Register before load(): the symbol is bound by name in both modules' GOTs, ahead of any
// definition in sdk or main. fn: arguments in c->x[0..7], result in c->x[0].
Thunk *add_override(const std::string &sym, GuestFn fn);
// A fallback: binds only when neither main nor sdk defines the symbol (the generated not-implemented backstops).
Thunk *add_weak_override(const std::string &sym, GuestFn fn);
extern std::vector<std::string> g_weak_used;
Thunk *add_stub(const std::string &name, void (*fn)(Cpu *, Thunk *));
Thunk *find_override(const std::string &sym);
uint64_t thunk_addr(const std::string &name);   // host function as a guest pointer

// Map, relocate and bind both modules. Returns false (message on stderr) on failure.
bool load(const char *main_elf, const char *sdk_elf);

// Symbol lookup across modules (rtld-style precedence: main, then sdk): guest address or 0.
std::vector<std::string> shared_symbols();   // symbols defined in both modules (main wins), sorted
uint64_t lookup(const std::string &name);
std::string symbolize(uint64_t addr);   // "sdk+0x1234 name+0x10" or "main+0x..", "host:thunk-name", "?"

// A guest thread: Cpu, guest stack, TLS block, Thread Local Region.
struct GuestThread {
    Cpu *c;
    uint64_t stack_lo, stack_hi;
};
GuestThread *new_thread(size_t stack_size = 4u << 20);

// Call a guest function from host code. Aborts if an exception or longjmp escapes to the host (resuming set).
uint64_t call_guest(Cpu *c, uint64_t fn, std::initializer_list<uint64_t> args = {});
// Same, but for use inside a host override or inline hook that was itself called from lifted code. The guest state the
// callee may change is restored afterwards (x0-x30, sp, pc, NZCV, v0-v31, FPCR/FPSR, the exclusive monitor), so a
// hook between any two instructions sees the Cpu as it left it; the return value is the callee's x0. Inside an inline
// hook (rt_inline_hook) the callee returns to the hook site's address rather than HOST_SENTINEL, so a guest exception
// or longjmp reaches the hooked function's landing pad: call_guest_nested then throws GuestResume, which unwinds the
// hook and leaves c->resuming set for the generated site. Elsewhere it aborts as call_guest does.
uint64_t call_guest_nested(Cpu *c, uint64_t fn, std::initializer_list<uint64_t> args = {});
struct GuestResume {};   // thrown by call_guest_nested inside an inline hook; caught by rt_inline_hook only
constexpr uint64_t HOOK_RA_TAG = 1ull << 63;   // c->pc during an inline hook: tag | return address (0: none)

// Initializers in rtld's order: sdk DT_INIT + init_array, then main DT_INIT + init_array. Each runs on `c`. Returns the
// number run; stops at max_inits if non-zero. log: print each one.
struct InitRecord { const char *module; int index; uint64_t fn; };
extern std::vector<InitRecord> g_init_log;
extern std::vector<std::string> g_unbound;   // weak references bound to 0 and strong ones with no definition
int run_initializers(Cpu *c, bool log);

// SVC backstop. handler returns 1 if it handled the SVC (result in c->x[0]); otherwise the SVC is logged and aborts.
typedef int (*SvcHandler)(Cpu *c, uint32_t imm);
extern SvcHandler g_svc_handler;
const char *svc_name(uint32_t imm);

// Probe mode: a missing override or an unhandled SVC is logged with its guest call chain and returns 0 instead of
// aborting, so a whole initializer run can list its blockers in order.
struct BlockerEvent { std::string kind, what, caller, api; uint64_t args[4]; std::vector<std::string> chain; };
extern bool g_probe;
extern std::vector<BlockerEvent> g_blockers;
extern uint64_t g_probe_limit;           // stop (exit) after this many events; 0 = unlimited
void print_chain(Cpu *c, uint64_t pc, int depth);
std::vector<std::string> guest_chain(Cpu *c, uint64_t pc, int depth);
// The exported function to stub for a blocker hit at pc: the innermost frame inside an exported nn:: symbol, else the
// innermost frame inside any exported symbol (mangled name), else empty.
std::string blocker_api(Cpu *c, uint64_t pc);
// Stub: returns 0 in x0 (Result success), recorded in probe mode as a blocker event the first time it is called.
void add_probe_stub(const std::string &sym);

// Dispatch statistics and benchmark helpers.
GuestFn lookup_fn(uint64_t addr);   // lifted function starting at addr (dispatch table), or null
GuestFn lookup_fn_bsearch(uint64_t addr);   // the same by binary search of the sorted generated table (baseline)

void install_libc_overrides();      // malloc family and a few exact libc leaves, backed by the host CRT
}  // namespace loader
