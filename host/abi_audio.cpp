// AArch64-guest to x86-64-host ABI bridge for the 16 nn::audio overrides in runtime/audio (project code, ours).
// snap_audio_overrides() returns host-C-ABI functions; the lifted code calls overrides with its arguments in c->x[0..7]
// and the result in c->x[0]. All parameters are integers or pointers except the last of SetAudioDeviceOutputVolume
// (float in s0). The mangled name tells which: a trailing 'f' parameter code. The result is returned as a 64-bit value.
#include <cstring>
#include <string>

#include "audio/audio_out.h"
#include "loader.h"

namespace {
constexpr int kMax = 32;
void *g_fn[kMax];

typedef uint64_t (*FnI)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*FnF)(uint64_t, float);

template <int I>
void bridge_int(Cpu *c) {
    c->x[0] = ((FnI)g_fn[I])(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5]);
}
template <int I>
void bridge_float(Cpu *c) {
    float f;
    memcpy(&f, &c->v[0], 4);
    c->x[0] = ((FnF)g_fn[I])(c->x[0], f);
}
template <int... Is>
struct Table {
    static constexpr GuestFn ints[sizeof...(Is)] = {bridge_int<Is>...};
    static constexpr GuestFn floats[sizeof...(Is)] = {bridge_float<Is>...};
};
template <int N, int... Is>
struct Make : Make<N - 1, N - 1, Is...> {};
template <int... Is>
struct Make<0, Is...> { typedef Table<Is...> type; };
}  // namespace

// Binds the nn::audio overrides into the loader. Returns how many were bound.
int snap_bind_audio() {
    typedef Make<kMax>::type T;
    int n = 0;
    const SnapAudioOverride *t = snap_audio_overrides(&n);
    for (int i = 0; i < n && i < kMax; i++) {
        g_fn[i] = t[i].fn;
        std::string m = t[i].mangled;
        bool is_float = m.size() > 2 && m.compare(m.size() - 2, 2, "Ef") == 0;   // ..._Ef : last parameter is float
        loader::add_override(m, is_float ? T::floats[i] : T::ints[i]);
    }
    return n;
}
