// SPDX-License-Identifier: MIT
// nn::nifm, nn::nsd, nn::prepo, nn::socket, nn::ssl: the console is permanently offline. Every network entry point
// fails the way a console without a connection does (never hangs, never crashes).
// Registration list: reg_net.inc (generated from imports_usage.tsv; one HLE_IMPL/HLE_NOOP per line).
#include <cstdio>
#include <cstring>

#include "svc_common.h"

namespace hle {

namespace {
constexpr int kBsdEBADF = 9;
constexpr int kBsdENETDOWN = 50;  // FreeBSD numbering, as Horizon uses
constexpr int kBsdEAI_NONAME = 8;
constexpr int kBsdAF_INET = 2;
thread_local char t_ntoa[16];
}  // namespace

int& sock_errno() {
  thread_local int e = 0;
  return e;
}

void sock_bswap32(Cpu* c) { uint32_t v = arg32(c, 0); ret(c, __builtin_bswap32(v)); }
void sock_bswap16(Cpu* c) { uint16_t v = (uint16_t)arg(c, 0); ret(c, __builtin_bswap16(v)); }
void sock_get_last_errno(Cpu* c) { ret(c, (uint64_t)(uint32_t)sock_errno()); }
void sock_set_last_errno(Cpu* c) { sock_errno() = (int)arg32(c, 0); }

void sock_fail(Cpu* c) {
  sock_errno() = kBsdENETDOWN;
  ret(c, (uint64_t)(int64_t)-1);
}

void sock_close(Cpu* c) {
  sock_errno() = kBsdEBADF;  // no descriptor is ever handed out
  ret(c, (uint64_t)(int64_t)-1);
}

void sock_inet_ntoa(Cpu* c) {
  uint32_t a = arg32(c, 0);  // in_addr by value, network byte order in memory order
  std::snprintf(t_ntoa, sizeof t_ntoa, "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
  ret(c, (uint64_t)(uintptr_t)t_ntoa);
}

void sock_inet_ntop(Cpu* c) {
  if ((int)arg32(c, 0) != kBsdAF_INET || !arg(c, 1) || !arg(c, 2) || arg32(c, 3) < 16) { sock_errno() = 47; ret(c, 0); return; }
  const uint8_t* a = ptr<const uint8_t>(arg(c, 1));
  std::snprintf(ptr<char>(arg(c, 2)), arg32(c, 3), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  ret(c, arg(c, 2));
}

void sock_inet_pton(Cpu* c) {
  if ((int)arg32(c, 0) != kBsdAF_INET) { ret(c, (uint64_t)(int64_t)-1); return; }
  unsigned v[4];
  char tail;
  if (!arg(c, 1) || std::sscanf(ptr<const char>(arg(c, 1)), "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4 ||
      v[0] > 255 || v[1] > 255 || v[2] > 255 || v[3] > 255) { ret(c, 0); return; }
  uint8_t* o = ptr<uint8_t>(arg(c, 2));
  for (int i = 0; i < 4; ++i) o[i] = (uint8_t)v[i];
  ret(c, 1);
}

void sock_get_addr_info(Cpu* c) {
  if (uint64_t p = arg(c, 3)) store<void*>(ptr<void>(p), nullptr);
  ret(c, kBsdEAI_NONAME);
}

void nsd_resolve_ex(Cpu* c) {
  // Result ResolveEx(Fqdn* out, const Fqdn& in): the name resolves to itself (no environment switching offline).
  if (arg(c, 0) && arg(c, 1)) std::memmove(ptr<void>(arg(c, 0)), ptr<void>(arg(c, 1)), 256);
  ret(c, 0);
}

void register_net(std::vector<Entry>& out) {
#include "reg_net.inc"
  // libc write()/read() on a socket descriptor route here (sdk libc); no descriptor exists, so they fail.
  HLE_IMPL("socket", "_ZN2nn6socket5WriteEiPKvm", sock_fail);
  HLE_IMPL("socket", "_ZN2nn6socket4ReadEiPvm", sock_fail);
}

}  // namespace hle
