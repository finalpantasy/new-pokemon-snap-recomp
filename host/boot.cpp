// Boot and end-to-end test driver for the two-module lifted runtime (project code, ours).
//   snap_boot.exe [--main main.elf] [--sdk sdk.elf] <command> [options]
//   commands: info | shared | init [--probe N] [--libc] | run [--trace file] [--timeout s] [--stop-after-init] | eh | lua | bench | all
// Run from the project root (the default ELF paths are relative to it). Output goes to stderr.
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"
#include "os/os.h"
#include "hle/hle.h"
#include "hle/hle_config.h"
#include "audio/audio_out.h"
#include "nvn/nvn_host.h"

void snap_memlog_find(uint64_t addr);
int snap_bind_audio();     // host/abi_audio.cpp
void snap_bind_nvn();      // generated/boot/nvn_bridge.cpp
int snap_nvn_bridge_count();

using namespace loader;

static uint64_t G(const char *name) {
    uint64_t a = lookup(name);
    if (!a) { fprintf(stderr, "boot: symbol %s not found\n", name); exit(2); }
    return a;
}
static void *gp(uint64_t v) { return (void *)(uintptr_t)v; }
static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static Cpu *g_cpu_dbg;   // main thread's Cpu (fallback)
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    Cpu *g_cpu_dbg = g_tls_cpu ? g_tls_cpu : ::g_cpu_dbg;
    fprintf(stderr, "CRASH on guest thread type %#llx (tid %lu)\n", (unsigned long long)os_current_thread_type(), GetCurrentThreadId());
    fprintf(stderr, "HOST CRASH code=%#lx at host %p, accessed %p\n", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress,
            ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
    fprintf(stderr, "  host image base %p, rip offset %#llx; guest cpu x0=%#llx x1=%#llx x30=%s sp=%#llx\n", (void *)GetModuleHandleA(nullptr),
            (unsigned long long)((uint8_t *)ep->ExceptionRecord->ExceptionAddress - (uint8_t *)GetModuleHandleA(nullptr)),
            (unsigned long long)(g_cpu_dbg ? g_cpu_dbg->x[0] : 0), (unsigned long long)(g_cpu_dbg ? g_cpu_dbg->x[1] : 0),
            g_cpu_dbg ? symbolize(g_cpu_dbg->x[30]).c_str() : "-", (unsigned long long)(g_cpu_dbg ? g_cpu_dbg->sp : 0));
    if (g_cpu_dbg) {
        for (int i = 19; i <= 28; i++) fprintf(stderr, "  x%d=%#llx", i, (unsigned long long)g_cpu_dbg->x[i]);
        fprintf(stderr, "\n");
        // dump guest memory around x25 and x21 (tree node / owner) for null-deref triage; guarded by SEH-safe probing
        for (int r : {25, 21, 23}) {
            uint64_t p = g_cpu_dbg->x[r];
            if (p > 0x10000 && p < 0x00007fffffffffffull && !IsBadReadPtr((void *)p, 0x80)) {
                fprintf(stderr, "  [x%d=%#llx]:", r, (unsigned long long)p);
                for (int o = 0; o < 0x80; o += 8) fprintf(stderr, " %llx", (unsigned long long)*(uint64_t *)(p + o));
                fprintf(stderr, "\n");
                if (r == 25) snap_memlog_find(p);
            }
        }
    }
    if (const char *dump = getenv("SNAP_DUMP")) {   // debug: SNAP_DUMP=0xaddr[,0xaddr..] prints 8 qwords at each address at crash time
        while (dump && *dump) {
            char *end; uint64_t p = strtoull(dump, &end, 0);
            if (p > 0x10000 && !IsBadReadPtr((void *)p, 0x40)) {
                fprintf(stderr, "  [dump %#llx]:", (unsigned long long)p);
                for (int o = 0; o < 0x40; o += 8) fprintf(stderr, " %llx", (unsigned long long)*(uint64_t *)(p + o));
                fprintf(stderr, "\n");
            }
            dump = *end == ',' ? end + 1 : nullptr;
        }
    }
    if (g_cpu_dbg) { for (auto &l : guest_chain(g_cpu_dbg, g_cpu_dbg->x[30], 14)) fprintf(stderr, "    %s\n", l.c_str()); }
    if (g_cpu_dbg) fprintf(stderr, "NEXT-BLOCKER crash %s\n", blocker_api(g_cpu_dbg, g_cpu_dbg->x[30]).c_str());
    if (!g_init_log.empty()) {
        auto &r = g_init_log.back();
        fprintf(stderr, "  last initializer started: %s[%d] %s\n", r.module, r.index, symbolize(r.fn).c_str());
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------------------------------- test (a): C++ exceptions

static uint64_t g_throw_count = 0;
static uint8_t *g_short_string;   // a guest std::string "hi" in libc++'s short layout: byte 0 = size << 1, data at +1

// A host function with xsputn/overflow's calling convention whose body is `std::string::at(7)` on a 2-character string: the
// guest libc++ throws a real std::out_of_range ("basic_string") through __cxa_throw. It tail-calls with the caller's x30 and
// sp intact, so the guest unwinder sees the lifted frame that called it (a host frame has no unwind data).
static void thrower(Cpu *c, Thunk *) {
    g_throw_count++;
    c->x[0] = (uint64_t)g_short_string;
    c->x[1] = 7;
    c->pc = G("_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE2atEm");
    rt_dispatch(c);
}

static int test_eh(Cpu *c) {
    int fails = 0;
    g_short_string = (uint8_t *)_aligned_malloc(32, 16);
    memset(g_short_string, 0, 32);
    g_short_string[0] = 2 << 1; g_short_string[1] = 'h'; g_short_string[2] = 'i';
    uint64_t thr = add_stub("thrower", thrower)->addr;
    // a streambuf whose xsputn and overflow throw
    uint8_t *sb = (uint8_t *)_aligned_malloc(0x100, 16);
    memset(sb, 0, 0x100);
    call_guest(c, G("_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC1Ev"), {(uint64_t)sb});
    uint64_t *vt = (uint64_t *)_aligned_malloc(16 * 8, 16);
    memcpy(vt, gp(G("_ZTVNSt3__115basic_streambufIcNS_11char_traitsIcEEEE")), 16 * 8);
    vt[2 + 12] = thr;   // xsputn
    vt[2 + 13] = thr;   // overflow
    vt[2 + 6] = thr;    // sync
    *(uint64_t *)sb = (uint64_t)(vt + 2);
    uint8_t *os = (uint8_t *)_aligned_malloc(0x400, 16);
    memset(os, 0, 0x400);
    call_guest(c, G("_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEEC1EPNS_15basic_streambufIcS2_EE"), {(uint64_t)os, (uint64_t)sb});
    uint64_t vptr = *(uint64_t *)os;
    uint8_t *ios = os + *(int64_t *)(vptr - 24);
    uint32_t *rdstate = (uint32_t *)(ios + 0x20);
    fprintf(stderr, "eh: ostream at %p, ios_base at +%lld, rdbuf slot %#llx (streambuf %#llx), rdstate %u\n", os, (long long)(ios - os),
            (unsigned long long)*(uint64_t *)(ios + 0x28), (unsigned long long)(uint64_t)sb, *rdstate);
    struct Case { const char *name; const char *sym; std::vector<uint64_t> args; };
    Case cases[] = {
        {"ostream::write(\"hello\", 5)", "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5writeEPKcl", {(uint64_t)os, (uint64_t) "hello", 5}},
        {"ostream::operator<<(int)", "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEi", {(uint64_t)os, 12345}},
        {"ostream::operator<<(long)", "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEl", {(uint64_t)os, 99}},
        {"ostream::flush()", "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5flushEv", {(uint64_t)os}},
    };
    for (auto &cs : cases) {
        uint64_t f = lookup(cs.sym);
        if (!f) { fprintf(stderr, "eh: %s: symbol missing, skipped\n", cs.name); continue; }
        *rdstate = 0;
        uint64_t before = g_throw_count;
        uint64_t sp0 = c->sp;
        std::vector<uint64_t> a = cs.args;
        c->x[0] = a[0];
        uint64_t r;
        if (a.size() == 1) r = call_guest(c, f, {a[0]});
        else if (a.size() == 2) r = call_guest(c, f, {a[0], a[1]});
        else r = call_guest(c, f, {a[0], a[1], a[2]});
        (void)r;
        bool thrown = g_throw_count > before;
        bool caught = (*rdstate & 1) != 0;   // badbit set by the guest catch handler
        bool ok = thrown && caught && c->sp == sp0 && !c->resuming;
        fprintf(stderr, "eh: %-32s thrown=%d caught(badbit)=%d sp restored=%d resuming=%llu -> %s\n", cs.name, thrown, caught,
                c->sp == sp0, (unsigned long long)c->resuming, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    // many throw/catch cycles: no leak of the resume state and a stable stack pointer
    uint64_t sp0 = c->sp, before = g_throw_count;
    for (int i = 0; i < 2000; i++) {
        *rdstate = 0;
        call_guest(c, G("_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5writeEPKcl"), {(uint64_t)os, (uint64_t) "x", 1});
        if (!(*rdstate & 1)) { fails++; break; }
    }
    fprintf(stderr, "eh: 2000 more throws %llu, sp stable=%d -> %s\n", (unsigned long long)(g_throw_count - before), c->sp == sp0,
            (g_throw_count - before == 2000 && c->sp == sp0) ? "PASS" : "FAIL");
    if (g_throw_count - before != 2000 || c->sp != sp0) fails++;
    return fails;
}

// ---------------------------------------------------------------------------------------------------- test (b): Lua error via setjmp/longjmp

static void lua_alloc(Cpu *c, Thunk *) {   // lua_Alloc(ud, ptr, osize, nsize)
    void *p = gp(c->x[1]);
    size_t ns = (size_t)c->x[3];
    if (!ns) { _aligned_free(p); c->x[0] = 0; return; }
    c->x[0] = (uint64_t)_aligned_realloc(p, ns, 16);
}
struct ReaderState { const char *s; size_t n; int done; };
static void lua_reader(Cpu *c, Thunk *) {   // const char *reader(lua_State *, void *ud, size_t *size)
    auto *st = (ReaderState *)gp(c->x[1]);
    size_t *sz = (size_t *)gp(c->x[2]);
    if (st->done) { *sz = 0; c->x[0] = 0; return; }
    st->done = 1;
    *sz = st->n;
    c->x[0] = (uint64_t)st->s;
}

// Lua 5.1 in `main` (v262144 offsets): luaD_rawrunprotected 0x57bb0 (setjmp 0x57be8), luaD_throw 0x57a30 (longjmp 0x57abc),
// lua_pcall 0x54860, lua_load 0x549d0, lua_newstate 0x68270.
static const uint64_t LUA_NEWSTATE = GUEST_MAIN_BASE + 0x68270, LUA_LOAD = GUEST_MAIN_BASE + 0x549d0, LUA_PCALL = GUEST_MAIN_BASE + 0x54860;

static std::string lua_top_string(uint64_t L, std::string &kind) {
    uint64_t top = *(uint64_t *)(L + 0x10);
    uint64_t val = *(uint64_t *)(top - 16);
    uint32_t tt = *(uint32_t *)(top - 8);
    if (tt == 4) { kind = "string"; return (const char *)(val + 24); }
    char b[64];
    if (tt == 3) { kind = "number"; snprintf(b, sizeof b, "%g", *(double *)(top - 16)); return b; }
    snprintf(b, sizeof b, "type %u", tt);
    kind = b;
    return "";
}

static int test_lua(Cpu *c) {
    int fails = 0;
    uint64_t alloc = add_stub("lua_alloc", lua_alloc)->addr, reader = add_stub("lua_reader", lua_reader)->addr;
    uint64_t L = call_guest(c, LUA_NEWSTATE, {alloc, 0});
    fprintf(stderr, "lua: lua_newstate -> L=%#llx\n", (unsigned long long)L);
    if (!L) return 1;
    struct T { const char *name, *src; int want_load, want_pcall; const char *want_msg; };
    T ts[] = {
        {"runtime error (index a nil local)", "local t = nil; return t.x", 0, 2, "attempt to index local 't' (a nil value)"},
        {"syntax error in the parser", "x = = 1", 3, -1, "unexpected symbol near '='"},
        {"arithmetic on a string", "local s = {}; return s + 1", 0, 2, "attempt to perform arithmetic on local 's' (a table value)"},
        {"call a nil value", "local f; f()", 0, 2, "attempt to call local 'f' (a nil value)"},
        {"no error: return 1+1", "return 1+1", 0, 0, "2"},
    };
    for (auto &t : ts) {
        ReaderState st{t.src, strlen(t.src), 0};
        uint64_t sp0 = c->sp;
        int ld = (int)call_guest(c, LUA_LOAD, {L, reader, (uint64_t)&st, (uint64_t) "=chunk"});
        int pc = -1;
        std::string kind, msg;
        if (ld == 0) {
            pc = (int)call_guest(c, LUA_PCALL, {L, 0, 1, 0});
            msg = lua_top_string(L, kind);
        } else {
            msg = lua_top_string(L, kind);
        }
        bool ok = ld == t.want_load && pc == t.want_pcall && msg.find(t.want_msg) != std::string::npos && c->sp == sp0 && !c->resuming;
        fprintf(stderr, "lua: %-36s load=%d pcall=%d msg=\"%s\" -> %s\n", t.name, ld, pc, msg.c_str(), ok ? "PASS" : "FAIL");
        if (!ok) fails++;
        *(uint64_t *)(L + 0x10) = *(uint64_t *)(L + 0x18);   // lua_settop(L, 0): L->top = L->base
    }
    // 3000 errors through setjmp/longjmp: parse once, call repeatedly
    {
        const char *src = "local t = nil; return t.x";
        uint64_t sp0 = c->sp;
        int bad = 0;
        for (int i = 0; i < 3000; i++) {
            ReaderState st{src, strlen(src), 0};
            int ld = (int)call_guest(c, LUA_LOAD, {L, reader, (uint64_t)&st, (uint64_t) "=chunk"});
            int pc = (int)call_guest(c, LUA_PCALL, {L, 0, 1, 0});
            if (ld != 0 || pc != 2) bad++;
            *(uint64_t *)(L + 0x10) = *(uint64_t *)(L + 0x18);
        }
        fprintf(stderr, "lua: 3000 load+pcall error cycles, bad=%d sp stable=%d -> %s\n", bad, c->sp == sp0, (!bad && c->sp == sp0) ? "PASS" : "FAIL");
        if (bad || c->sp != sp0) fails++;
    }
    return fails;
}

// ---------------------------------------------------------------------------------------------------- bench

static double bench_dispatch(Cpu *c) {
    uint64_t fn = G("abs");
    const int N = 20000000;
    GuestFn direct = lookup_fn(fn);
    double t0 = now_s();
    for (int i = 0; i < N; i++) { c->x[0] = (uint64_t)i; direct(c); }
    double t_direct = now_s() - t0;
    t0 = now_s();
    for (int i = 0; i < N; i++) { c->x[0] = (uint64_t)i; c->pc = fn; rt_dispatch(c); }
    double t_dtab = now_s() - t0;
    t0 = now_s();
    for (int i = 0; i < N; i++) { c->x[0] = (uint64_t)i; lookup_fn_bsearch(fn)(c); }
    double t_bs = now_s() - t0;
    // a spread of 4096 different addresses defeats the cache the single-address loops enjoy
    std::vector<uint64_t> addrs;
    for (uint64_t i = 0; i < g_sdk.dtab_count && addrs.size() < 4096; i++)
        if (g_sdk.dtab[i]) addrs.push_back(g_sdk.base + g_sdk.text_off + i * 4);
    volatile uint64_t sink = 0;
    t0 = now_s();
    for (int i = 0; i < N; i++) sink += (uint64_t)lookup_fn(addrs[(i * 7) & 4095]);
    double t_dtab_spread = now_s() - t0;
    t0 = now_s();
    for (int i = 0; i < N; i++) sink += (uint64_t)lookup_fn_bsearch(addrs[(i * 7) & 4095]);
    double t_bs_spread = now_s() - t0;
    fprintf(stderr, "bench dispatch (%d calls of sdk abs()): direct %.2f ns, rt_dispatch (table) %.2f ns, binary search %.2f ns\n", N,
            t_direct / N * 1e9, t_dtab / N * 1e9, t_bs / N * 1e9);
    fprintf(stderr, "bench lookup only, 4096 distinct sdk functions: table %.2f ns, binary search %.2f ns\n", t_dtab_spread / N * 1e9,
            t_bs_spread / N * 1e9);
    return t_dtab / N * 1e9;
}

static void cmp_thunk(Cpu *c, Thunk *) {   // qsort comparator on u64 values
    uint64_t a = *(uint64_t *)gp(c->x[0]), b = *(uint64_t *)gp(c->x[1]);
    c->x[0] = (uint64_t)(int64_t)(a < b ? -1 : a > b);
}

// Lifted libc workload: formatting, parsing, sorting, string handling. Returns seconds for the whole set (best of reps).
static void bench_libc(Cpu *c, int reps) {
    uint64_t snprintf_f = G("snprintf"), strtod_f = G("strtod"), qsort_f = G("qsort"), strstr_f = G("strstr"), strtol_f = G("strtol");
    uint64_t cmp = add_stub("qsort_cmp", cmp_thunk)->addr;
    char *buf = (char *)_aligned_malloc(256, 16);
    uint64_t *arr = (uint64_t *)_aligned_malloc(8 * 50000, 16);
    char *hay = (char *)_aligned_malloc(4096, 16);
    memset(hay, 'a', 4095); hay[4095] = 0; memcpy(hay + 4000, "needle", 6);
    struct W { const char *name; double best = 1e9; uint64_t sink = 0; } ws[] = {{"snprintf(%.6f %d %s) x20000"}, {"strtod x20000"}, {"qsort 50000 u64 (host cmp)"}, {"strstr 4 KB x5000"}, {"strtol x50000"}};
    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_s();
        for (int i = 0; i < 20000; i++) {
            // variadic call: the double goes in d0, the ints and pointer in x
            c->v[0] = 0; double d = i * 0.25 + 3.14159; memcpy(&c->v[0], &d, 8);
            ws[0].sink += call_guest(c, snprintf_f, {(uint64_t)buf, 256, (uint64_t) "%.6f %d %s", (uint64_t)i, (uint64_t) "tail"});
        }
        ws[0].best = std::min(ws[0].best, now_s() - t0);
        t0 = now_s();
        for (int i = 0; i < 20000; i++) { ws[1].sink += call_guest(c, strtod_f, {(uint64_t) "3.14159265358979e5", 0}); }
        ws[1].best = std::min(ws[1].best, now_s() - t0);
        t0 = now_s();
        uint64_t s = 12345;
        for (int i = 0; i < 50000; i++) { s = s * 6364136223846793005ull + 1442695040888963407ull; arr[i] = s >> 20; }
        ws[2].sink += call_guest(c, qsort_f, {(uint64_t)arr, 50000, 8, cmp});
        ws[2].best = std::min(ws[2].best, now_s() - t0);
        t0 = now_s();
        for (int i = 0; i < 5000; i++) ws[3].sink += call_guest(c, strstr_f, {(uint64_t)hay, (uint64_t) "needle"});
        ws[3].best = std::min(ws[3].best, now_s() - t0);
        t0 = now_s();
        for (int i = 0; i < 50000; i++) ws[4].sink += call_guest(c, strtol_f, {(uint64_t) "123456789", 0, 10});
        ws[4].best = std::min(ws[4].best, now_s() - t0);
    }
    for (auto &w : ws) fprintf(stderr, "bench libc: %-32s best of %d: %8.2f ms  (checksum %llx)\n", w.name, reps, w.best * 1000, (unsigned long long)w.sink);
    fprintf(stderr, "bench libc: sample output: \"%s\"; sorted ok=%d\n", buf, [&] { for (int i = 1; i < 50000; i++) if (arr[i - 1] > arr[i]) return 0; return 1; }());
}


// ---------------------------------------------------------------------------------------------------- driver

struct Opts { std::vector<std::string> stubs; std::string main_elf = "analysis/import/v262144-main.elf", sdk_elf = "analysis/import/v262144-sdk.elf", cmd; int probe = 0, iters = 5; bool libc = false, stop_after_init = false; std::string trace_file, timeout_s, arg; };
static Opts g_o;

// ---------------------------------------------------------------------------------------------------- full boot ("run")

static void hle_bind_one(const char *sym, GuestFn fn, void *) {
    if (hle::find_override(sym) == fn) add_override(sym, fn);   // a real HLE override
    else add_weak_override(sym, fn);                            // generated backstop: only for symbols nothing defines
}

// Registers every host layer. Order matters: a later registration of the same symbol wins, so the generated "not implemented"
// stubs (inside hle::bind_all) come first and the real audio / nvn / os overrides replace them.
// nn::codec::HardwareOpusDecoder is an object the game builds, asks for work-buffer sizes (vtable slot 2: rate, channels) and
// destroys (slot 0); neither main nor sdk defines it (it is a system-side class), so the host provides a vtable. The real decoder
// is not needed: Wwise decodes Opus in software.
static uint64_t g_opus_vtbl[4];
static void opus_ctor(Cpu *c) { *(uint64_t *)c->x[0] = (uint64_t)g_opus_vtbl; }
static void opus_nop(Cpu *) {}
static void opus_work_size(Cpu *c) { c->x[0] = 0x20000ull * (c->x[2] ? c->x[2] : 1); }   // 4 KB multiple, generous per channel
static uint64_t g_ngc_vtbl[4];
static void ngc_ctor(Cpu *c) { *(uint64_t *)c->x[0] = (uint64_t)g_ngc_vtbl; }
static void bind_codec_stubs() {
    g_opus_vtbl[0] = add_override("host:HardwareOpusDecoder::dtor", opus_nop)->addr;
    g_opus_vtbl[1] = add_override("host:HardwareOpusDecoder::deleting_dtor", opus_nop)->addr;
    g_opus_vtbl[2] = add_override("host:HardwareOpusDecoder::GetWorkBufferSize", opus_work_size)->addr;
    add_override("_ZN2nn5codec19HardwareOpusDecoderC1Ei", opus_ctor);
    // nn::ngc::ProfanityFilter: the game deletes it through the vtable (slot 1) after a name-entry round; the HLE layer answers the
    // other calls (Initialize, Finalize), so only the destructors need real code.
    g_ngc_vtbl[0] = add_override("host:ProfanityFilter::dtor", opus_nop)->addr;
    g_ngc_vtbl[1] = add_override("host:ProfanityFilter::deleting_dtor", opus_nop)->addr;
    add_override("_ZN2nn3ngc15ProfanityFilterC1Ev", ngc_ctor);
}

// Frame counters from the NVN hooks (presented frames and submitted command lists), traced every 30 presents.
static std::atomic<uint64_t> g_presents{0}, g_submits{0};
static void hook_present(void *, int slot, int w, int h) {
    uint64_t n = ++g_presents;
    if (n <= 3 || n % 30 == 0) trace("nvn: present #%llu slot %d %dx%d (submits %llu)", (unsigned long long)n, slot, w, h, (unsigned long long)g_submits.load());
}
static void hook_submit(void *, int, int) { ++g_submits; }
// The game window. nn::vi::GetNativeWindow must hand the NVN layer a real HWND (nvnWindowBuilderSetNativeWindow -> Vulkan Win32
// surface); the HLE vi layer only returns a token. A dedicated UI thread owns the window and pumps its messages; closing the
// window ends the process.
static HWND g_game_hwnd;
// Debug: SNAP_SHOT=<file.bmp> SNAP_SHOT_AT=<seconds> copies the window client area from the screen at that time (the DWM composes the
// Vulkan swapchain, so a screen BitBlt sees what is presented).
static void snap_capture_window(HWND h) {
    static const char *path = getenv("SNAP_SHOT");
    static const char *at = getenv("SNAP_SHOT_AT");
    static ULONGLONG t0 = GetTickCount64();
    static int done;
    if (!path || done >= 6 || (GetTickCount64() - t0) < (ULONGLONG)((at ? atoi(at) : 10) + done * (getenv("SNAP_SHOT_EVERY") ? atoi(getenv("SNAP_SHOT_EVERY")) : 10)) * 1000) return;
    char pathn[512]; snprintf(pathn, sizeof pathn, "%s.%d.bmp", path, done++);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(h);
    Sleep(700);
    RECT cr; GetClientRect(h, &cr);
    POINT o = {0, 0}; ClientToScreen(h, &o);
    int w = cr.right, ht = cr.bottom;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, ht);
    SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, w, ht, sdc, o.x, o.y, SRCCOPY);
    BITMAPINFOHEADER bi = {sizeof(bi), w, -ht, 1, 32, BI_RGB};
    std::vector<uint8_t> px((size_t)w * ht * 4);
    GetDIBits(mdc, bmp, 0, ht, px.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(sizeof(fh) + sizeof(bi) + px.size()), 0, 0, sizeof(fh) + sizeof(bi)};
    if (FILE *f = fopen(pathn, "wb")) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&bi, sizeof(bi), 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); }
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    fprintf(stderr, "[host] window captured to %s (%dx%d)\n", pathn, w, ht);
}

static LRESULT CALLBACK game_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { fflush(stderr); ExitProcess(0); }
    if (m == WM_TIMER) { snap_capture_window(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static DWORD WINAPI game_window_thread(LPVOID ev) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = game_wndproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SnapRecompWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    RECT r = {0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_game_hwnd = CreateWindowW(wc.lpszClassName, L"New Pokemon Snap (native recompilation)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    SetTimer(g_game_hwnd, 1, 1000, nullptr);
    SetEvent((HANDLE)ev);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
static void vi_get_native_window_host(Cpu *c) {
    if (c->x[0]) *(uint64_t *)c->x[0] = (uint64_t)g_game_hwnd;
    c->x[0] = 0;
}
static void bind_window() {
    SetProcessDPIAware();   // physical pixels: the 1280x720 client really is 1280x720 (screen captures, window size)
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, game_window_thread, ev, 0, nullptr);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
    add_override("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window_host);
}
static void bind_host_layers() {
    hle::init_trace_from_env();
    hle::load_config("");
    hle::bind_all(hle_bind_one, nullptr);
    int na = snap_bind_audio();
    snap_bind_nvn();
    bind_codec_stubs();
    bind_window();
    hle::set_log_sink([](const char *l, size_t n) { trace("%.*s", (int)n, l); });   // timestamped HLE log lines
    { static NvnHostHooks hk = {nullptr, hook_present, hook_submit}; nvnHostSetHooks(&hk); }
    size_t no = 0;
    const OsOverride *ot = os_override_table(&no);
    for (size_t i = 0; i < no; i++) add_override(ot[i].sym, ot[i].fn);   // os layer: one table, swap this block to change layers
    trace("bound host layers: hle %zu overrides, audio %d, nvn %d procs (+nvnBootstrapLoader), os %zu", hle::overrides().size(), na, snap_nvn_bridge_count(), no);
    hle::os_hooks().init_system_event = [](void *ev, bool manual) { os_system_event_create((uint64_t)ev, manual ? 0 : 1); };
    hle::os_hooks().signal_system_event = [](void *ev) { os_system_event_signal((uint64_t)ev); };
    snap_audio_set_os_hooks([](void *ev) { os_system_event_signal((uint64_t)ev); });
}

// The musl thread-pointer hooks. Both modules have an empty PT_TLS, so the guest's thread-local words (tpidr_el0 + small
// offsets: errno, a locale flag at +0x20, ...) live in a per-thread zeroed 64 KB block with tp in its middle. sdk's thread
// trampoline (sdk+0x348e80) calls *nnmuslThreadPointerInitializer(x0 = ThreadType) before the entry and
// reads TLR[0x1f0] into tpidr_el0 afterwards; the nothing in the lifted modules installs these pointers, so the host does.
static void tp_init_hook(Cpu *c, Thunk *) {
    uint8_t *blk = (uint8_t *)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    *(uint64_t *)(c->tpidrro + 0x1f0) = (uint64_t)(blk + 0x8000);   // runtime/os calls the hook itself with only x0 set
}
static void tp_destroy_hook(Cpu *c, Thunk *) {
    uint64_t tp = *(uint64_t *)(c->tpidrro + 0x1f0);
    if (tp) VirtualFree((void *)(tp - 0x8000), 0, MEM_RELEASE);
}

// os_svc serves every SVC the game uses (svcGetInfo 0x29 included, runtime/os/svc_getinfo.md); anything else is left unhandled.
static int svc_dispatch(Cpu *c, uint32_t imm) {
    return os_svc(c, imm) ? 1 : 0;
}

static void summarize_run() {
    trace("run summary: %llu guest exceptions; SVCs seen:", (unsigned long long)g_exc_count);
    for (int i = 0; i < 0x80; i++) if (g_svc_counts[i]) trace("  svc %#x %s x%llu", i, svc_name(i), (unsigned long long)g_svc_counts[i]);
}

// Stall watchdog: if nothing has been traced for `stall` seconds, print where the main guest thread is (host RIP and the guest
// frame chain read from its Cpu) every `stall` seconds. Reads race with the running thread; the values are only diagnostics.
// Host call stack of a suspended thread through dbghelp (needs build\snap\snap_boot.pdb next to the exe); diagnostics only.
#include <dbghelp.h>
static void host_stack(HANDLE th, CONTEXT ctx) {
    static bool init = false;
    HANDLE proc = GetCurrentProcess();
    if (!init) { SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, nullptr, TRUE); init = true; }
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rsp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 28; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (!sf.AddrPC.Offset) break;
        alignas(8) char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, si)) trace("        host %s+%#llx", si->Name, (unsigned long long)disp);
        else trace("        host %#llx", (unsigned long long)sf.AddrPC.Offset);
    }
}
extern "C" void os_dump_threads(void (*emit)(void *, uint64_t, uint64_t, int, uint64_t, uint64_t, int, int, Cpu *), void *ud);
static void dump_thread_cb(void *, uint64_t tt, uint64_t id, int wkind, uint64_t wkey, uint64_t waddr, int core, int prio, Cpu *cpu) {
    static const char *wn[] = {"run", "lock", "cvar", "key", "join", "multi", "sleep"};
    if (tt == 0xC0DE) { trace("    core %llu held by prio %d", (unsigned long long)id, prio); return; }
    trace("    guest thread (waiting_core=%d mask=%#llx ideal=%d)", (int)((id >> 62) & 1), (unsigned long long)((id >> 48) & 0xff), (int)((id >> 56) & 0xf) - 4);
    id &= 0xffffffffffffull;
    trace("    guest thread tt=%#llx id=%llu %s wkey=%#llx waddr=%#llx core=%d prio=%d x30=%s sp=%#llx", (unsigned long long)tt, (unsigned long long)id,
          wkind >= 0 && wkind < 7 ? wn[wkind] : "?", (unsigned long long)wkey, (unsigned long long)waddr, core, prio, cpu ? symbolize(cpu->x[30]).c_str() : "-", cpu ? (unsigned long long)cpu->sp : 0ull);
    if (cpu) for (auto &l : guest_chain(cpu, cpu->x[30], 10)) trace("        %s", l.c_str());
}

static HANDLE g_main_thread_handle;
static Cpu *g_main_cpu;
static DWORD WINAPI watchdog(LPVOID) {
    double last_report = 0;
    for (;;) {
        Sleep(1000);
        double now = trace_now();
        static const double dump_at = getenv("SNAP_DUMP_AT") ? atof(getenv("SNAP_DUMP_AT")) : 0.0;   // debug: one guest thread dump at that time, no host stacks
        static int dumped = 0;
        const bool force = dump_at > 0 && now >= dump_at + dumped * 1.5 && dumped < 6;
        if (!force && (now - g_last_trace_time < 8.0 || now - last_report < 8.0)) continue;
        if (force) dumped++;
        last_report = now;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        SuspendThread(g_main_thread_handle);
        bool ok = GetThreadContext(g_main_thread_handle, &ctx);
        uint64_t rip = ok ? ctx.Rip : 0;
        Cpu *c = g_main_cpu;
        std::vector<std::string> ch;
        if (c) ch = guest_chain(c, c->x[30], 12);
        ResumeThread(g_main_thread_handle);
        trace("STALL: no trace output for %.1f s; main thread host rip=%#llx (exe+%#llx), guest x0=%#llx x30=%s", now - g_last_trace_time,
              (unsigned long long)rip, (unsigned long long)(rip - (uint64_t)GetModuleHandleA(nullptr)), c ? (unsigned long long)c->x[0] : 0ull, c ? symbolize(c->x[30]).c_str() : "-");
        for (auto &l : ch) trace("    %s", l.c_str());
        os_dump_threads(dump_thread_cb, nullptr);
        if (force) continue;
        {   // every other host thread: rip as exe offset (resolve with build\\snap\\snap_boot.map; ntdll/kernel rips mean it is blocked)
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof te};
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                CONTEXT tc = {}; tc.ContextFlags = CONTEXT_FULL;
                if (SuspendThread(h) != (DWORD)-1) {
                    if (GetThreadContext(h, &tc)) {
                        host_stack(h, tc);
                        uint64_t base = (uint64_t)GetModuleHandleA(nullptr);
                        bool inexe = tc.Rip - base < 0x10000000ull;
                        trace("    thread %lu rip=%s%#llx", te.th32ThreadID, inexe ? "exe+" : "", (unsigned long long)(inexe ? tc.Rip - base : tc.Rip));
                    }
                    ResumeThread(h);
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        }
        if (c) {   // saved frame words at sp+16..: the usual AArch64 prologue pair (x19, x30) and the next pair
            uint64_t *f = (uint64_t *)c->sp;
            trace("    sp=%#llx [sp+16]=%#llx [sp+24]=%s", (unsigned long long)c->sp, (unsigned long long)f[2], symbolize(f[3]).c_str());
        }
        if (c && c->x[0] - 0x7000000000ull < 0x1000000000ull) {   // a guest pointer in x0: dump the object and its vtable
            uint64_t *o = (uint64_t *)c->x[0];
            trace("    [x0] = %#llx %#llx %#llx %#llx", (unsigned long long)o[0], (unsigned long long)o[1], (unsigned long long)o[2], (unsigned long long)o[3]);
            if (o[0] - 0x7000000000ull < 0x1000000000ull) {
                uint64_t *vt = (uint64_t *)o[0];
                trace("    vtable %s: %s %s %s", symbolize(o[0]).c_str(), symbolize(vt[5]).c_str(), symbolize(vt[6]).c_str(), symbolize(vt[4]).c_str());
            }
        }
    }
}

extern "C" void loader_enable_fntrace();
static int cmd_run(Cpu *c) {
    loader_enable_fntrace();
    g_main_cpu = c;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    double t0 = now_s();
    trace("boot: sdk and main loaded");
    // the main thread: a Horizon thread with a Thread Local Region; the ELF thread pointer goes into TLR+0x1f0
    os_adopt_main_thread(c, 0, -1);
    *(uint64_t *)(c->tpidrro + 0x1f0) = c->tpidr;
    g_probe = g_o.probe > 0;
    g_probe_limit = (uint64_t)g_o.probe;
    double ti = now_s();
    int n = run_initializers(c, true);
    trace("boot: %d initializers completed in %.3f s", n, now_s() - ti);
    if (g_o.stop_after_init) { summarize_run(); return 0; }
    uint64_t startup = lookup("nninitStartup"), nnmain = lookup("nnMain");
    trace("boot: nninitStartup=%#llx nnMain=%#llx", (unsigned long long)startup, (unsigned long long)nnmain);
    if (startup) { trace("boot: calling nninitStartup"); call_guest(c, startup); trace("boot: nninitStartup returned"); }
    if (nnmain) { trace("boot: calling nnMain"); call_guest(c, nnmain); trace("boot: nnMain returned"); }
    trace("boot: total %.3f s", now_s() - t0);
    summarize_run();
    return 0;
}


static void report_load() {
    const char *n[2] = {"main", "sdk"};
    for (int i = 0; i < 2; i++) {
        const RelocStats &r = g_rstats[i];
        const GuestModule &m = i ? g_sdk : g_main;
        fprintf(stderr, "load %s: base %#llx span %#llx text %#llx+%#llx, %llu lifted functions, %zu dynsyms, init %s + %llu init_array\n", n[i],
                (unsigned long long)m.base, (unsigned long long)m.span, (unsigned long long)m.text_off, (unsigned long long)m.text_size,
                (unsigned long long)m.fn_count, m.syms.size(), m.init_fn ? "DT_INIT" : "-", (unsigned long long)m.init_count);
        fprintf(stderr, "  relocs: RELATIVE %llu ABS64 %llu GLOB_DAT %llu JUMP_SLOT %llu other %llu; resolved: host override %llu, rtld special %llu, sdk %llu, main %llu, weak->0 %llu, missing %llu (defined in own module: %llu)\n",
                (unsigned long long)r.relative, (unsigned long long)r.abs64, (unsigned long long)r.glob_dat, (unsigned long long)r.jump_slot,
                (unsigned long long)r.other, (unsigned long long)r.res_override, (unsigned long long)r.res_special, (unsigned long long)r.res_sdk,
                (unsigned long long)r.res_main, (unsigned long long)r.res_weak0, (unsigned long long)r.res_missing, (unsigned long long)r.own_defined);
    }
}

static void report_unbound() { for (auto &u : g_unbound) fprintf(stderr, "  unbound: %s\n", u.c_str()); }

// Single-threaded stand-in for the Horizon thread/arbitration SVCs, used only by the eh, lua and bench tests: with one thread
// there are no waiters, so signals and unlocks are no-ops. (The real handler is os_svc in runtime/os.)
static int test_svc(Cpu *c, uint32_t imm) {
    switch (imm) {
    case 0x0b: case 0x11: case 0x12: case 0x1b: case 0x1d: case 0x35: case 0x36: case 0x2a: case 0x2b:
        c->x[0] = 0; return 1;
    case 0x10: c->x[0] = 0; return 1;                       // GetCurrentProcessorNumber
    case 0x1e: c->x[0] = rt_ticks(); return 1;              // GetSystemTick
    default: return 0;
    }
}

// Minimal nn::os shims for the standalone tests (the real ones belong to runtime/os): TLS slot allocation without ThreadManager.
static int g_next_sdk_slot = 31;
static void shim_alloc_tls_slot(Cpu *c) {   // Result SdkAllocateTlsSlot(TlsSlot *slot, void (*dtor)(uintptr_t))
    *(uint32_t *)gp(c->x[0]) = (uint32_t)g_next_sdk_slot--;
    c->x[0] = 0;
}

static DWORD WINAPI run_thread(LPVOID) {
    SetUnhandledExceptionFilter(crash_filter);
    const bool full = g_o.cmd == "run";
    if (!full) {
        if (g_o.cmd == "init" && !g_o.libc) {}
        else if (g_o.cmd == "info" || g_o.cmd == "shared" || g_o.cmd == "findptr") {}
        else {   // tests: the real nn::os layer (main's operator new reaches nn::os memory), no HLE
            install_libc_overrides();
            size_t no = 0;
            const OsOverride *ot = os_override_table(&no);
            for (size_t i = 0; i < no; i++) add_override(ot[i].sym, ot[i].fn);
        }
        for (auto &st : g_o.stubs) add_probe_stub(st);
    } else {
        g_trace_imports = true;
        if (!g_o.trace_file.empty()) g_trace_file = fopen(g_o.trace_file.c_str(), "w");
        trace("boot: start (precedence: host override, main, sdk)");
        install_libc_overrides();
        bind_host_layers();
        for (auto &st : g_o.stubs) add_probe_stub(st);
    }
    if (!load(g_o.main_elf.c_str(), g_o.sdk_elf.c_str())) return 2;
    report_load();
    report_unbound();
    const bool cmd_uses_os = g_o.cmd == "eh" || g_o.cmd == "lua" || g_o.cmd == "bench" || g_o.cmd == "all";
    if (full || cmd_uses_os) {
        // item 1 of runtime/os/PROPOSED_SHARED_CHANGES.md: os_init after load(), with the musl thread-pointer hook variables
        OsConfig oc = {};
        oc.trace = getenv("SNAP_OS_TRACE") != nullptr;
        oc.tp_init_var = lookup("nnmuslThreadPointerInitializer");
        oc.tp_destroy_var = lookup("nnmuslThreadPointerDestroyer");
        os_init(&oc);
        if (oc.tp_init_var && !*(uint64_t *)oc.tp_init_var) *(uint64_t *)oc.tp_init_var = add_stub("tp_init", tp_init_hook)->addr;
        if (oc.tp_destroy_var && !*(uint64_t *)oc.tp_destroy_var) *(uint64_t *)oc.tp_destroy_var = add_stub("tp_destroy", tp_destroy_hook)->addr;
        trace("os_init: tp_init_var=%#llx (value %#llx) tp_destroy_var=%#llx", (unsigned long long)oc.tp_init_var, oc.tp_init_var ? (unsigned long long)*(uint64_t *)oc.tp_init_var : 0ull, (unsigned long long)oc.tp_destroy_var);
    }
    if (full) {
        g_svc_handler = svc_dispatch;
        g_spin_gate_fn = os_spin_gate;
        g_ticks_fn = os_ticks;
    } else if (g_o.cmd != "init" && g_o.cmd != "info" && g_o.cmd != "shared" && g_o.cmd != "findptr") {
        g_svc_handler = svc_dispatch;
        g_spin_gate_fn = os_spin_gate;
        g_ticks_fn = os_ticks;
    }
    GuestThread *th = new_thread();
    Cpu *c = th->c;
    g_cpu_dbg = c;
    int fails = 0;
    const std::string &cmd = g_o.cmd;
    if (cmd == "eh" || cmd == "lua" || cmd == "bench" || cmd == "all") {
        os_adopt_main_thread(c, 0, -1);
        *(uint64_t *)(c->tpidrro + 0x1f0) = c->tpidr;
    }
    if (cmd == "info") return 0;
    if (cmd == "shared") {
        auto v = shared_symbols();
        fprintf(stderr, "symbols defined in both main and sdk (main wins): %zu\n", v.size());
        for (auto &n : v) {
            fprintf(stderr, "  %s  main=%s sdk=%#llx\n", n.c_str(), symbolize(g_main.base + g_main.syms[g_main.defs[n]].value).c_str(), (unsigned long long)(g_sdk.syms[g_sdk.defs[n]].value));
        }
        return 0;
    }
    if (cmd == "findptr") {   // debug: every 8-byte slot of main's image that holds the given value (after relocation)
        uint64_t v = strtoull(g_o.arg.c_str(), nullptr, 0);
        for (GuestModule *m : {&g_main, &g_sdk}) for (uint64_t a = m->base; a < m->base + m->span; a += 8) if (*(uint64_t *)a == v) fprintf(stderr, "slot %s+%#llx\n", m->name, (unsigned long long)(a - m->base));
        return 0;
    }
    if (cmd == "run") return cmd_run(c);
    if (cmd == "init") {
        g_probe = g_o.probe > 0;
        g_probe_limit = (uint64_t)g_o.probe;
        double t0 = now_s();
        int n = run_initializers(c, true);
        fprintf(stderr, "init: %d initializers completed in %.3f s; %zu blocker events\n", n, now_s() - t0, g_blockers.size());
        return 0;
    }
    if (cmd == "eh" || cmd == "all") fails += test_eh(c);
    if (cmd == "lua" || cmd == "all") fails += test_lua(c);
    if (cmd == "bench" || cmd == "all") { bench_dispatch(c); bench_libc(c, g_o.iters); }
    fprintf(stderr, "boot: %s done, %d failing checks\n", cmd.c_str(), fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    static char errbuf[1 << 16];
    setvbuf(stderr, errbuf, _IOLBF, sizeof errbuf);   // MSVC stderr is unbuffered (one write per character when redirected): a log burst stalls the game threads
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--main" && i + 1 < argc) g_o.main_elf = argv[++i];
        else if (a == "--sdk" && i + 1 < argc) g_o.sdk_elf = argv[++i];
        else if (a == "--probe" && i + 1 < argc) g_o.probe = atoi(argv[++i]);
        else if (a == "--iters" && i + 1 < argc) g_o.iters = atoi(argv[++i]);
        else if (a == "--libc") g_o.libc = true;
        else if (a == "--stop-after-init") g_o.stop_after_init = true;
        else if (a == "--trace" && i + 1 < argc) g_o.trace_file = argv[++i];
        else if (a == "--timeout" && i + 1 < argc) g_o.timeout_s = argv[++i];
        else if (a == "--stub" && i + 1 < argc) g_o.stubs.push_back(argv[++i]);
        else if (!g_o.cmd.empty()) g_o.arg = a;
        else g_o.cmd = a;
    }
    if (g_o.cmd.empty()) { fprintf(stderr, "usage: snap_boot [--main elf] [--sdk elf] info|shared|init [--probe N] [--libc]|run [--probe N] [--trace file] [--timeout s] [--stop-after-init]|eh|lua|bench [--iters N]|all\n"); return 2; }
    // guest calls are host calls: run on a thread with a large host stack
    HANDLE h = CreateThread(nullptr, 256u << 20, run_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    DWORD wait_ms = g_o.timeout_s.empty() ? INFINITE : (DWORD)(atoi(g_o.timeout_s.c_str()) * 1000);
    if (WaitForSingleObject(h, wait_ms) == WAIT_TIMEOUT) {
        fprintf(stderr, "boot: timeout after %s s, exiting\n", g_o.timeout_s.c_str());
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    DWORD rc = 1;
    GetExitCodeThread(h, &rc);
    return (int)rc;
}
