// SPDX-License-Identifier: MIT
// Pure helpers for audio.json (no OS calls, so tests/audio/mix_test.cpp can include it).
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>

namespace snap_audio::cfg {

// Pointer to the first character after the ':' of the KEY `"name"` (a string that is followed by a colon), or nullptr.
// A string VALUE equal to the key name is skipped: {"output": "speakers", "speakers": 8} must find the second one.
inline const char* find_key(const char* buf, const char* name) {
  const std::string k = std::string("\"") + name + "\"";
  for (const char* p = std::strstr(buf, k.c_str()); p; p = std::strstr(p + 1, k.c_str())) {
    const char* q = p + k.size();
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') ++q;
    if (*q == ':') return q + 1;
  }
  return nullptr;
}

// Integer value of `name`; false when the key is missing or not a number.
inline bool read_int(const char* buf, const char* name, int* out) {
  const char* p = find_key(buf, name);
  if (!p) return false;
  char* end = nullptr;
  long v = std::strtol(p, &end, 10);
  if (end == p) return false;
  *out = (int)v;
  return true;
}

}  // namespace snap_audio::cfg
