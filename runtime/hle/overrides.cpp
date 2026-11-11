// SPDX-License-Identifier: MIT
// Registry of every host override (symbol -> function), logging, and the "not implemented" backstop.
// The list of overrides is the union of the register_* functions; `gen_hle_stubs.py` reads the same HLE_IMPL /
// HLE_NOOP lines to build analysis/runtime/hle_coverage.tsv and the generated stubs for the imports left over.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "hle.h"
#include "svc_common.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace hle {

// ---- logging
static LogSink g_sink = nullptr;
unsigned trace_flags = 0;

void set_log_sink(LogSink sink) { g_sink = sink; }

void log(const char* fmt, ...) {
  char buf[1024];
  int n = std::snprintf(buf, sizeof buf, "[hle] ");
  va_list ap;
  va_start(ap, fmt);
  int m = std::vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
  va_end(ap);
  size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
  if (len > sizeof buf - 2) len = sizeof buf - 2;
  buf[len++] = '\n';
  buf[len] = 0;
  if (g_sink) g_sink(buf, len);
  else std::fwrite(buf, 1, len, stderr);
}

void init_trace_from_env() {
  const char* e = std::getenv("SNAP_HLE_TRACE");
  if (!e) return;
  std::string s = e;
  if (s == "all") { trace_flags = ~0u; return; }
  if (s.find("fs") != std::string::npos) trace_flags |= kTraceFs;
  if (s.find("hid") != std::string::npos) trace_flags |= kTraceHid;
  if (s.find("svc") != std::string::npos) trace_flags |= kTraceSvc;
}

OsHooks& os_hooks() {
  static OsHooks h;
  return h;
}

// ---- helpers
std::string utf16_to_utf8(const char16_t* s, size_t max) {
  std::string out;
  for (size_t i = 0; s && s[i] && i < max; ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  }
  return out;
}

std::string hex_u64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}

uint64_t now_unix_seconds() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string safe_cstr(uint64_t addr, size_t max) {
  if (addr < 0x10000) return addr ? "<bad pointer>" : "<null>";
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    return "<unreadable>";
  size_t avail = (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)addr);
  max = max < avail ? max : avail;
#endif
  const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(addr));
  std::string s;
  for (size_t i = 0; i < max && p[i]; ++i) s += (p[i] >= 32 && p[i] < 127) ? p[i] : '?';
  return s;
}

// ---- registry
const std::vector<Entry>& overrides() {
  static const std::vector<Entry> all = [] {
    std::vector<Entry> v;
    register_fs(v);
    register_hid(v);
    register_sys(v);
    register_net(v);
    return v;
  }();
  return all;
}

GuestFn find_override(const char* symbol) {
  static const std::unordered_map<std::string, GuestFn> map = [] {
    std::unordered_map<std::string, GuestFn> m;
    for (auto& e : overrides()) {
      auto r = m.emplace(e.symbol, e.fn);
      if (!r.second && r.first->second != e.fn) std::fprintf(stderr, "[hle] duplicate override for %s\n", e.symbol);
    }
    return m;
  }();
  auto it = map.find(symbol);
  return it == map.end() ? nullptr : it->second;
}

static const StubEntry* g_stubs = nullptr;
static size_t g_stub_count = 0;
void set_generated_stubs(const StubEntry* stubs, size_t n) { g_stubs = stubs; g_stub_count = n; }

void bind_all(BindFn bind, void* user) {
  for (auto& e : overrides()) bind(e.symbol, e.fn, user);
  for (size_t i = 0; i < g_stub_count; ++i)
    if (!find_override(g_stubs[i].symbol)) bind(g_stubs[i].symbol, g_stubs[i].fn, user);
}

// ---- backstop for imports without an override
void (*unimplemented_hook)(const char*) = nullptr;

void unimplemented(Cpu* c, const char* symbol) {
  std::fprintf(stderr,
               "[hle] NOT IMPLEMENTED: %s\n[hle]   caller x30=%#llx sp=%#llx\n[hle]   x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx x6=%#llx x7=%#llx\n",
               symbol, (unsigned long long)c->x[30], (unsigned long long)c->sp, (unsigned long long)c->x[0],
               (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
               (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[6],
               (unsigned long long)c->x[7]);
  // Pointer-looking arguments are often C strings (paths, names): print what is readable.
  for (int i = 0; i < 4; ++i) {
    uint64_t v = c->x[i];
    if (v > 0x100000 && v < (1ull << 47)) {
      std::string s = safe_cstr(v, 80);
      if (s.size() >= 3 && s[0] != '<' && s.find('?') == std::string::npos) std::fprintf(stderr, "[hle]   x%d -> \"%s\"\n", i, s.c_str());
    }
  }
  std::fflush(stderr);
  if (unimplemented_hook) { unimplemented_hook(symbol); c->x[0] = 0; return; }
  std::abort();
}

void (*fatal_hook)(const char*) = nullptr;

void fatal(const std::string& message) {
  std::fprintf(stderr, "[hle] FATAL: %s\n", message.c_str());
  std::fflush(stderr);
  if (fatal_hook) { fatal_hook(message.c_str()); return; }
  std::abort();
}

}  // namespace hle
