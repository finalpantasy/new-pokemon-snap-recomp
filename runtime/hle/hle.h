// SPDX-License-Identifier: MIT
// Horizon service HLE for the Snap recompilation (runtime/hle). Host overrides are plain GuestFn functions keyed by
// the mangled symbol, following the TotK HOST_OVERRIDE convention (tools/ref/totk/runtime/host.h, MIT): the loader
// binds each Entry by symbol name over every GOT/GLOB_DAT/ABS64 of `main` and `sdk`.
//
// Guest pointers are host pointers (AGENTS.md "CPU and runtime"), so an override reads its arguments straight from the
// AAPCS64 registers in Cpu and dereferences them. Nothing here depends on the lifted game: tests call the overrides
// with a hand-built Cpu.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../cpu.h"

namespace hle {

// ---- nn::Result: module in bits 0-8, description in bits 9-21 (evidence: main 0x602c90 masks 0x1ff / 0x3ffe00)
constexpr uint32_t result(uint32_t module, uint32_t desc) { return module | (desc << 9); }
constexpr uint32_t result_module(uint32_t r) { return r & 0x1ff; }
constexpr uint32_t result_desc(uint32_t r) { return (r >> 9) & 0x1fff; }
constexpr uint32_t kSuccess = 0;

// ---- logging: "[hle] ..." on stderr, plus an optional sink (tests, the host log)
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
using LogSink = void (*)(const char* line, size_t n);
void set_log_sink(LogSink sink);
// bit 0 fs, 1 hid, 2 other services; env SNAP_HLE_TRACE=fs,hid,svc or all
enum : unsigned { kTraceFs = 1, kTraceHid = 2, kTraceSvc = 4 };
extern unsigned trace_flags;
void init_trace_from_env();

// ---- AAPCS64 argument access
inline uint64_t arg(const Cpu* c, int i) { return c->x[i]; }
inline uint32_t arg32(const Cpu* c, int i) { return (uint32_t)c->x[i]; }
inline void ret(Cpu* c, uint64_t v) { c->x[0] = v; }
template <class T> inline T* ptr(uint64_t a) { return reinterpret_cast<T*>(static_cast<uintptr_t>(a)); }
template <class T> inline T load(const void* p) { T v; std::memcpy(&v, p, sizeof v); return v; }
template <class T> inline void store(void* p, T v) { std::memcpy(p, &v, sizeof v); }

// ---- registry
struct Entry {
  const char* symbol;   // mangled name (or C name) of the guest function this replaces
  GuestFn fn;
  const char* service;  // fs, hid, oe, ...
  const char* status;   // "implemented" or "stub-noop"
};
// Each service file defines one `void register_<svc>(std::vector<Entry>& out)` using these two macros. The
// coverage generator (runtime/hle/gen_hle_stubs.py) parses them, so keep one registration per line.
#define HLE_IMPL(svc, sym, fn) out.push_back(::hle::Entry{sym, fn, svc, "implemented"})
#define HLE_NOOP(svc, sym, fn) out.push_back(::hle::Entry{sym, fn, svc, "stub-noop"})

void register_fs(std::vector<Entry>& out);
void register_hid(std::vector<Entry>& out);
void register_sys(std::vector<Entry>& out);   // oe, pctl, settings, time, account, err, diag, album, friends, ngc, swkbd, vi, nv, crypto, codec
void register_net(std::vector<Entry>& out);   // nifm, nsd, prepo, socket, ssl

const std::vector<Entry>& overrides();         // every override, built once
GuestFn find_override(const char* symbol);     // nullptr if none
// Calls bind(symbol, fn, user) for every override and for every generated "not implemented" stub
// (gen_hle_stubs.py output, optional). The loader passes a hostfn_add_override-style binder (PROPOSED_SHARED_CHANGES.md).
using BindFn = void (*)(const char* symbol, GuestFn fn, void* user);
void bind_all(BindFn bind, void* user);

// Logs the function name, caller (x30) and x0-x7, then aborts. Used by the generated stubs.
// Test hook: when `unimplemented_hook` is set, unimplemented() calls it, sets x0 = 0 and returns instead of aborting.
void unimplemented(Cpu* c, const char* symbol);
extern void (*unimplemented_hook)(const char* symbol);
// Fatal guest errors (nn::diag abort): logs, then aborts unless `fatal_hook` is set (tests).
void fatal(const std::string& message);
extern void (*fatal_hook)(const char* message);

// nn::err and friends show errors through this; the default logs and, unless Config::headless, opens a message box.
using ErrorUiFn = void (*)(const char* title, const char* message);
void set_error_ui(ErrorUiFn fn);
const std::string& last_error_message();

// nn::vi: the opaque native-window handle the guest passes to the NVN window builder (runtime/nvn may use it as a
// token), and the 60 Hz vsync event feeder.
void* vi_native_window();
void vi_stop_vsync();

// gen_hle_stubs.py writes generated/hle_unimplemented.cpp (ignored dir): one logging stub per import without an
// override, registered from a static initializer through set_generated_stubs. Linking it in is optional.
struct StubEntry { const char* symbol; GuestFn fn; };
void set_generated_stubs(const StubEntry* stubs, size_t n);

// ---- services the os worker provides (runtime/os); optional until wired up, see PROPOSED_SHARED_CHANGES.md
struct OsHooks {
  // Initialize a guest nn::os::SystemEventType in place (auto-clear) so it can be signaled from any host thread.
  void (*init_system_event)(void* system_event_type, bool manual_clear) = nullptr;
  void (*signal_system_event)(void* system_event_type) = nullptr;
};
OsHooks& os_hooks();

// ---- small helpers
std::string utf16_to_utf8(const char16_t* s, size_t max = 4096);
std::string hex_u64(uint64_t v);
uint64_t now_unix_seconds();

}  // namespace hle
