// SPDX-License-Identifier: MIT
// Shared one-line handlers and host-chosen result codes for the small services.
#pragma once
#include "hle.h"

namespace hle {

// Results the host returns for "this service is unavailable offline". The game only tests success versus failure
// for these (nnResultIsFailure at 25 sites); the module/description values follow the SDK ranges but are not read
// by any compare found in `main`.
constexpr uint32_t kResultNifmUnavailable = result(110, 300);   // nifm: network communication disabled
constexpr uint32_t kResultSslUnavailable = result(123, 1);      // ssl: not initialized / offline
constexpr uint32_t kResultAccountOffline = result(124, 5);      // account: network service account unavailable
constexpr uint32_t kResultSwkbdCanceled = result(101, 1);       // swkbd: user canceled (host-chosen)

inline void noop(Cpu*) {}
inline void ret_ok(Cpu* c) { ret(c, 0); }
inline void ret_zero(Cpu* c) { ret(c, 0); }
inline void ret_true(Cpu* c) { ret(c, 1); }
inline void ret_false(Cpu* c) { ret(c, 0); }
inline void ret_offline_nifm(Cpu* c) { ret(c, kResultNifmUnavailable); }
inline void ret_offline_ssl(Cpu* c) { ret(c, kResultSslUnavailable); }
inline void ret_offline_account(Cpu* c) { ret(c, kResultAccountOffline); }

// A guest C string, checked for readability (host pointers: a stray guest pointer must not crash the log line).
std::string safe_cstr(uint64_t addr, size_t max = 512);
// The thread's BSD errno of the offline socket layer (Horizon uses FreeBSD numbers).
int& sock_errno();

}  // namespace hle
