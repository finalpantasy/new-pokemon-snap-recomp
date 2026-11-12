// AArch64-guest to x86-64-host ABI bridge for the 16 nn::audio overrides in runtime/audio (project code, ours).
// snap_audio_overrides() returns host-C-ABI functions; the lifted code calls overrides with its arguments in c->x[0..7]
// and the result in c->x[0]. All parameters are integers or pointers except the last of SetAudioDeviceOutputVolume
// (float in s0). The mangled name tells which: a trailing 'f' parameter code. The result is returned as a 64-bit value.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio/audio_out.h"
#include "loader.h"

namespace {
constexpr int kMax = 32;
void *g_fn[kMax];

typedef uint64_t (*FnI)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*FnF)(uint64_t, float);

bool g_hook_list[kMax];   // bridge I is ListAudioDeviceName (see patch_output_settings)
bool g_hook_init[kMax];   // bridge I is InitializeAudioOutParameter (Wwise sink Open, see patch_sink_config)
bool g_hook_poll[kMax];   // bridge I is GetReleasedAudioOutBuffer: the Wwise sink thread polls it every audio frame
void apply_category_volumes(Cpu *c);
void patch_sink_config(Cpu *c);
void patch_output_settings(Cpu *c);

template <int I>
void bridge_int(Cpu *c) {
    if (g_hook_init[I]) patch_sink_config(c);
    if (g_hook_list[I]) patch_output_settings(c);
    c->x[0] = ((FnI)g_fn[I])(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5]);
    if (g_hook_poll[I]) apply_category_volumes(c);
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

// ---- BGM / SE / voice volume (docs/audio.md, "Volume") -------------------------------------------------------------
// The game has its own 0..10 BGM/SE/voice settings. Setter main+0x67c610 (this = audio manager, w1 = 0 bgm / 1 se /
// 2 voice, w2 = level) builds "Set_volume_<cat>_NN" and posts that Wwise event by name (vtable+0x78 of this->[8]); the
// events set the category bus volume inside the soundbank mix. We call the same function with
// round(gameLevel * userVolume) from the Wwise sink thread (a guest thread with its own Cpu), whenever the user volume
// changed, whenever the game wrote its own level (it would overwrite ours), and once a second while any category is
// below 1.0. With every category at 1.0 and never touched nothing is called: the original mix.
constexpr uint64_t kSetVolFn = GUEST_MAIN_BASE + 0x67c610;
constexpr uint64_t kMgrPtrAddr = GUEST_MAIN_BASE + 0x12fef28;   // shared_ptr<AudioManager>.ptr
constexpr uint64_t kSettingsGot = GUEST_MAIN_BASE + 0xcfab68;   // GOT slot -> game settings object (+0x6f bgm, +0x70 se, +0x71 voice)
bool g_trace_vol = getenv("SNAP_AUDIO_TRACE") != nullptr;
FILE *trace_out() {   // SNAP_AUDIO_TRACE=<path> appends to a file, anything else (e.g. 1) goes to stderr
    static FILE *f = [] { const char *e = getenv("SNAP_AUDIO_TRACE"); FILE *x = (e && strlen(e) > 1) ? fopen(e, "ab") : nullptr; if (x) setvbuf(x, nullptr, _IONBF, 0); return x; }();
    return f ? f : stderr;
}

// Surround switch (docs/audio.md "Surround and spatial output"). The Wwise Switch sink Open (main+0x124780) copies its
// channel config word (AkChannelConfig: mask << 12 | type << 8 | count) into sink+0xc, calls
// InitializeAudioOutParameter (return address main+0x1247d8, x20 = sink), then picks AudioOut channelCount 6 when the
// mask is 0x60f and 2 for 0x3. The game's own config is stereo, so when the host wants 6 channels the word is rewritten to
// 5.1 right here, before the sink reads it back.
// Trace line to stderr and, when SNAP_AUDIO_CAPTURE=<dir> is set, to <dir>\trace.log (stderr of concurrent runs shares one file).
void patch_trace(const char *fmt, unsigned a, unsigned b) {
    fprintf(stderr, fmt, a, b);
    if (const char *d = getenv("SNAP_AUDIO_CAPTURE")) {
        std::string f = std::string(d) + "\\trace.log";
        if (FILE *x = fopen(f.c_str(), "ab")) { fprintf(x, fmt, a, b); fclose(x); }
    }
}
void patch_sink_config(Cpu *c) {
    if (c->x[30] != GUEST_MAIN_BASE + 0x1247d8) return;
    SnapAudioStats st;
    snap_audio_get_stats(&st);
    uint32_t *w = (uint32_t *)(c->x[20] + 0xc);
    patch_trace("[audio-trace] sink config word 0x%08x (guest channels wanted %u)\n", *w, st.guestChannels);
    if (st.guestChannels == 6) *w = (0x60fu << 12) | (1u << 8) | 6u;
}
// The game builds its AkOutputSettings in the settings function main+0x67ffa0 with the channel config hard-coded to
// stereo (0x3102 at sp+0x90c; the 0x3102 constant is stored at main+0x68023c) and only then calls ListAudioDeviceName
// (return address main+0x680270, same sp). Rewriting the word there makes Wwise build a 5.1 master mix.
void patch_output_settings(Cpu *c) {
    patch_trace("[audio-trace] ListAudioDeviceName called from main+0x%x (%u)\n", (unsigned)(c->x[30] - GUEST_MAIN_BASE), 0);
    if (c->x[30] != GUEST_MAIN_BASE + 0x680270) return;
    const int want = snap_audio_game_channels();
    uint32_t *w = (uint32_t *)(c->sp + 0x90c);
    patch_trace("[audio-trace] AkOutputSettings channel config 0x%08x (guest channels wanted %u)\n", *w, (unsigned)want);
    if (want == 6 && *w == 0x3102u) *w = (0x60fu << 12) | (1u << 8) | 6u;
}
void apply_category_volumes(Cpu *c) {
    static uint32_t lastGen = 0;
    static int lastGame[3] = {-1, -1, -1};
    static int lastPosted[3] = {-1, -1, -1};
    static uint64_t lastTick = 0;
    static bool logged = false;
    const uint32_t gen = snap_audio_volume_generation();
    const uint64_t mgr = *(volatile uint64_t *)kMgrPtrAddr;
    const uint64_t st = *(volatile uint64_t *)kSettingsGot;
    if (!mgr || !st || !*(volatile uint64_t *)(mgr + 8)) return;   // game audio not up yet
    const uint64_t now = GetTickCount64();
    const bool tick = now - lastTick >= 1000;
    bool any = false;
    for (int cat = 0; cat < 3; ++cat) {
        const int game = *(volatile uint8_t *)(st + 0x6f + cat);
        if (game > 10) continue;                                    // not a plausible level: layout guess wrong, stay out
        const int mode = *(volatile uint32_t *)(mgr + 0x128);
        if (cat == 2 && mode == 2) continue;                        // game forces voice to 0 in this mode
        if (!logged) { logged = true; if (g_trace_vol) fprintf(trace_out(), "[audio] game levels bgm=%d se=%d voice=%d\n", game, *(volatile uint8_t *)(st + 0x70), *(volatile uint8_t *)(st + 0x71)); }
        const float v = snap_audio_get_volume(cat + 1);
        const bool touched = snap_audio_volume_touched(cat + 1) != 0;
        if (!touched) { lastGame[cat] = game; continue; }
        int eff = (int)lroundf((float)game * v);
        if (eff < 0) eff = 0; if (eff > 10) eff = 10;
        const bool gameChanged = game != lastGame[cat];
        lastGame[cat] = game;
        if (gen == lastGen && !gameChanged && !(tick && v < 0.999f) && lastPosted[cat] >= 0) continue;
        any = true;
        lastPosted[cat] = eff;
        if (g_trace_vol) fprintf(trace_out(), "[audio] set category %d: game=%d user=%.2f -> level %d\n", cat, game, v, eff);
        loader::call_guest_nested(c, kSetVolFn, {mgr, (uint64_t)cat, (uint64_t)eff});
    }
    lastGen = gen;
    if (tick) lastTick = now;
    (void)any;
}
}  // namespace

// Binds the nn::audio overrides into the loader. Returns how many were bound.
int snap_bind_audio() {
    typedef Make<kMax>::type T;
    int n = 0;
    const SnapAudioOverride *t = snap_audio_overrides(&n);
    for (int i = 0; i < n && i < kMax; i++) {
        g_fn[i] = t[i].fn;
        std::string m = t[i].mangled;
        g_hook_list[i] = m.find("ListAudioDeviceName") != std::string::npos;
        g_hook_init[i] = m.find("InitializeAudioOutParameter") != std::string::npos;
        g_hook_poll[i] = m.find("GetReleasedAudioOutBuffer") != std::string::npos;
        bool is_float = m.size() > 2 && m.compare(m.size() - 2, 2, "Ef") == 0;   // ..._Ef : last parameter is float
        loader::add_override(m, is_float ? T::floats[i] : T::ints[i]);
    }
    return n;
}
