// Game-state signals for scripted routes (SNAP_INPUT_SCRIPT "wait" lines, runtime/hle/input_core.cpp). Project code, ours.
//
// Two inline hooks (recomp/snap_gen.py MAIN_INLINE_HOOKS) read the Lumen UI layer; the course rail, the film counter and the
// dialogue window come from hooks that already exist (host/mods/neofilm.cpp, host/mods/skips.cpp) and call the same sink.
//
//   main+0x6c1d90  rt_gm_route_ui_event     entry of the std::function operator() of the UI scene base class (ctor 0x6c1a30), which
//                                        forwards each Lumen player event to scene->vtable[2]. The player dispatch (lr main+0x782008)
//                                        reaches it for every scene seen so far (title, main_menu, map_select, photo_submit, myphoto,
//                                        option, pause_menu, ...). x1 = int* event, x2 = Player**, x3 = std::string* state,
//                                        x4 = std::string* element. Events seen (run 2-20261113_144339): 1 scene built (hidden),
//                                        2 opened (first state), 3 closed, 5 state entered, 6 state ready, 7 input on, 16 focus,
//                                        18 + 10 decide (A), 21 + 12 decide done, 22 per-frame (carries a new state the frame the
//                                        layout moves to it). The player (vtable main+0xc92020, built by 0x77cc40) holds its scene
//                                        name, the .lm/.drp base name, at +0x10.
//   main+0x79a6d0  rt_gm_route_ui_setstate  entry of Player::SetState (player vtable +0x140): x0 = player, x1 = page descriptor,
//                                        state name std::string at desc+0x18. Covers scenes outside the base class too.
//
// All hooks return at once unless a route uses "wait" lines or SNAP_ROUTE_LOG=1 (then every scene, state and event is logged).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "loader.h"

namespace hle::input {   // runtime/hle/input_core.cpp
bool route_on();
void route_ui(uint64_t player, const char* scene, const char* state, int event, const char* elem, uint64_t lr);
}

namespace {

const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kPlayerVt = kMainBase + 0xc92020;

// libc++ std::string (short: byte 0 = size * 2, text at +1; long: byte 0 bit 0 set, size at +8, data at +0x10)
bool read_str(uint64_t s, char* out, size_t cap) {
    out[0] = 0;
    if (!s || IsBadReadPtr((void*)s, 24)) return false;
    uint8_t b0 = *(const uint8_t*)s;
    const char* p;
    size_t n;
    if (b0 & 1) {
        n = *(const uint64_t*)(s + 8);
        p = *(const char* const*)(s + 16);
        if (n > 4096 || !p || IsBadReadPtr(p, n ? n : 1)) return false;
    } else {
        n = b0 >> 1;
        p = (const char*)(s + 1);
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

// scene name of a player, read on every call: players are freed with their scene and the allocator reuses the address for the next
// scene (a pointer-keyed cache named photo select "online_report" in run 2-20261113_144339)
void player_name(uint64_t player, char* nm, size_t cap) {
    nm[0] = 0;
    if (player && !IsBadReadPtr((void*)player, 0x30) && *(const uint64_t*)player == kPlayerVt) read_str(player + 0x10, nm, cap);
    if (!nm[0]) snprintf(nm, cap, "?");
}

}  // namespace

extern "C" void rt_gm_route_ui_event(Cpu* c) {
    if (!hle::input::route_on()) return;
    uint64_t pe = c->x[1], pp = c->x[2];
    if (!pe || !pp || IsBadReadPtr((void*)pe, 4) || IsBadReadPtr((void*)pp, 8)) return;
    int ev = *(const int*)pe;
    uint64_t player = *(const uint64_t*)pp;
    char st[64], el[64];
    read_str(c->x[3], st, sizeof st);
    el[0] = 0;
    if (ev != 22) read_str(c->x[4], el, sizeof el);
    char nm[40];
    player_name(player, nm, sizeof nm);
    hle::input::route_ui(player, nm, st, ev, el, c->x[30]);
    // prog debug (LenTalk / requests): main game object (GOT main+0xcfab68) flag +0x3e53 gates the mission list (main+0x501a80)
    {
        static int last = -2;
        uint64_t got = *(const uint64_t*)(kMainBase + 0xcfab68);
        int v = (got && !IsBadReadPtr((void*)(got + 0x3e53), 1)) ? *(const uint8_t*)(got + 0x3e53) : -1;
        if (v != last) { last = v; fprintf(stderr, "[hle] route: game flag +0x3e53 = %d (event %d scene %s)\n", v, ev, nm); }
    }
}

extern "C" void rt_gm_route_ui_setstate(Cpu* c) {
    if (!hle::input::route_on()) return;
    uint64_t player = c->x[0], desc = c->x[1];
    char st[64];
    if (!read_str(desc + 0x18, st, sizeof st)) return;
    char nm[40];
    player_name(player, nm, sizeof nm);
    hle::input::route_ui(player, nm, st, -1, "", c->x[30]);
}
