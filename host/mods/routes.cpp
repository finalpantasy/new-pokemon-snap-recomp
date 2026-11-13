// Game-state signals for scripted routes (SNAP_INPUT_SCRIPT "wait" lines, runtime/hle/input_core.cpp). Project code, ours.
//
// Two inline hooks (recomp/snap_gen.py MAIN_INLINE_HOOKS) read the Lumen UI layer; the course rail, the film counter and the
// dialogue window come from hooks that already exist (host/mods/neofilm.cpp, host/mods/skips.cpp) and call the same sink.
//
//   main+0x6c1d90  rt_gm_route_ui_event     entry of the std::function operator() that the Lumen player calls for every event of a
//                                        UI scene built on the scene base class (ctor 0x6c1a30: main_menu, map_select, common_navi,
//                                        zukan_map, initial_setting, extra_shooting, pause_menu, evaluation, option, photo_edit,
//                                        result_reward, sns_window, photo_submit, ...). x1 = int* event (5 = state changed,
//                                        8 = focus moved, 10 = decide, 11/19 = cancel, 22 = per-frame update), x2 = Player**,
//                                        x3 = std::string* state, x4 = std::string* element. The player (vtable main+0xc92020,
//                                        built by 0x77cc40) holds its scene name, the .lm/.drp base name, at +0x10.
//   main+0x79a6d0  rt_gm_route_ui_setstate  entry of Player::SetState (player vtable +0x140): x0 = player, x1 = page descriptor,
//                                        state name std::string at desc+0x18. Covers scenes outside the base class too.
//
// All hooks return at once unless a route uses "wait" lines or SNAP_ROUTE_LOG=1 (then every scene, state and event is logged).
#include <windows.h>

#include <cstdint>
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

// scene name of a player (cached per player pointer; players live as long as their scene)
struct NameCache { uint64_t player; char name[40]; };
NameCache g_names[96];
int g_nnames;
SRWLOCK g_lock = SRWLOCK_INIT;

const char* player_name(uint64_t player) {
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_nnames; i++)
        if (g_names[i].player == player) { const char* r = g_names[i].name; ReleaseSRWLockShared(&g_lock); return r; }
    ReleaseSRWLockShared(&g_lock);
    char nm[40] = "?";
    if (player && !IsBadReadPtr((void*)player, 0x30) && *(const uint64_t*)player == kPlayerVt) read_str(player + 0x10, nm, sizeof nm);
    if (!nm[0]) strcpy(nm, "?");
    AcquireSRWLockExclusive(&g_lock);
    int k = g_nnames < 96 ? g_nnames++ : 95;
    g_names[k].player = player;
    memcpy(g_names[k].name, nm, sizeof nm);
    const char* r = g_names[k].name;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
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
    hle::input::route_ui(player, player_name(player), st, ev, el, c->x[30]);
}

extern "C" void rt_gm_route_ui_setstate(Cpu* c) {
    if (!hle::input::route_on()) return;
    uint64_t player = c->x[0], desc = c->x[1];
    char st[64];
    if (!read_str(desc + 0x18, st, sizeof st)) return;
    hle::input::route_ui(player, player_name(player), st, -1, "", c->x[30]);
}
