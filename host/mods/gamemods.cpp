// Game-side mods #16 (higher-quality SSAO) and #7 (per-pass toggles) (project code, ours).
//
// Everything here is driven by MAIN_INLINE_HOOKS in recomp/snap_gen.py: the generated code calls the rt_gm_* functions below just before
// the named instruction. Registers x19..x28 are always in the Cpu at that point; caller-saved ones may not be, so each hook reads
// only callee-saved registers. Every hook returns after one static flag check when its feature is off.
//
//   SNAP_SSAO_QUALITY=default|high|ultra   SimpleAO kernel size 32 / 96 / 256 (the pass already generates a 512-entry kernel), HBAO
//                                          8 directions x 32 samples (game default) / 12 x 48 / 16 x 64.
//   SNAP_SSAO_SAMPLES=n                    SimpleAO / Alchemy / Hemisphere sample count (1..512), overrides the preset.
//   SNAP_SSAO_HBAO=dirs,samples            HBAO directions and samples per direction, overrides the preset.
//   SNAP_SSAO_RADIUS=f                     SimpleAO kernel radius (game default 0.2; world-space units of the AO hemisphere).
//   SNAP_SSAO_STRENGTH=f                   SimpleAO strength (game default 1.0).
//   SNAP_LIGHTING=high|ultra               preset (parsed once in runtime/hle/lighting_preset.h): high = SNAP_SSAO_QUALITY=high, 2048 shadow maps, 2x shadow
//                                          distance; ultra = quality ultra, 4096 maps, 4x distance (maps and distance read by runtime/nvn);
//                                          every explicit switch above still wins.
//   SNAP_SSAO_FORCE=1                      switches the game's own SSAO passes (SSAOCreate/BlurX/BlurY/Composite) on where the game has them off (the
//                                          course), for the frame's loop only and never while a photo/evaluation/capture pass is enabled. Off by default.
//   SNAP_UI_FONT_SCALE=auto|1|2..4         UI glyphs are rasterized at k times the size into a k times larger atlas (#29, sharper fonts); the
//                                          layout fields of every glyph entry keep the 1x values, so text size and position do not change.
//                                          Unset or auto: k = 2 when the game window's client height is 1440 or more, else 1 (off). 1 = off.
//   SNAP_GM_TRACE=1                        logs the values the game had before the override (first 8 hits of each hook).
//   SNAP_PASS_DUMP=1                       logs each distinct render pass object once (vtable, flags, strings) for pass identification.
//   SNAP_PASS_OFF=name,name,...            switches render passes off by name (case-insensitive, names as SNAP_PASS_DUMP prints them; the pass
//                                          object keeps its enable byte at +0xf0 at zero, so the game never records it). Bloom: BloomCreate,BloomDraw;
//                                          DOF: DOFBlurX,DOFBlurY,DOF; FilmGrain; ChromaticAberration; also SSAOCreate/BlurX/BlurY/Composite.
//                                          Photo and evaluation passes are refused (see pass_class below); names not verified safe are refused too
//                                          unless SNAP_PASS_FORCE=1 (test aid, never for the photo passes). A pass is NOT skipped when the game
//                                          itself wants it off: only the byte is forced to 0, the game's own writes are not touched.
//   SNAP_PASS_AB=<P>                       test aid: the passes named in SNAP_PASS_OFF are off only in even blocks of P presents (same-view A/B).
//
// Addresses (module offsets, build id DDB900FF...): analysis/mods/ and docs/mod-ideas.md rows 7 and 16.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"
#include "hle/visual_defaults.h"   // default font scale (SNAP_VISUALS)
#include "hle/lighting_preset.h"   // SNAP_LIGHTING parser

using namespace loader;

namespace {

const uint64_t kMainBase = 0x7100000000ull;

bool g_init_done;
int g_simple_n;                 // 0 = leave the game's value
int g_hbao_dirs, g_hbao_samples;  // 0 = leave
bool g_trace, g_pass_dump;
int g_font_k;                   // 0 = off
bool g_font_decided;
float g_ao_radius, g_ao_strength;  // 0 = leave the game's value
std::vector<std::string> g_pass_off;
int g_pass_ab;
bool g_pass_force;
bool g_ao_force;                // SNAP_SSAO_FORCE=1|2 (P4)
int g_ao_force_level;           // 1 = strict gate (the Evaluation pass enabled also blocks), 2 = relaxed (only capture / photo / report / resize passes block)

void init() {
    if (g_init_done) return;
    g_init_done = true;
    const char* q = getenv("SNAP_SSAO_QUALITY");
    if (!q || !*q) q = snaplight::ssao_quality();   // SNAP_LIGHTING preset (hle/lighting_preset.h, shared with nvn_entry.cpp and nvn_draw.cpp)
    if (q) {
        if (!strcmp(q, "high")) { g_simple_n = 96; g_hbao_dirs = 12; g_hbao_samples = 48; }
        else if (!strcmp(q, "ultra")) { g_simple_n = 256; g_hbao_dirs = 16; g_hbao_samples = 64; }
    }
    if (const char* s = getenv("SNAP_SSAO_SAMPLES")) { int n = atoi(s); if (n >= 1 && n <= 512) g_simple_n = n; }
    if (const char* h = getenv("SNAP_SSAO_HBAO")) {
        int d = 0, s = 0;
        if (sscanf(h, "%d,%d", &d, &s) == 2 && d >= 1 && d <= 64 && s >= 1 && s <= 128) { g_hbao_dirs = d; g_hbao_samples = s; }
    }
    if (const char* r = getenv("SNAP_SSAO_RADIUS")) { float v = (float)atof(r); if (v > 0.0f && v <= 4.0f) g_ao_radius = v; }
    if (const char* r = getenv("SNAP_SSAO_STRENGTH")) { float v = (float)atof(r); if (v > 0.0f && v <= 8.0f) g_ao_strength = v; }
    g_trace = getenv("SNAP_GM_TRACE") && getenv("SNAP_GM_TRACE")[0] != '0';
    g_pass_dump = getenv("SNAP_PASS_DUMP") && getenv("SNAP_PASS_DUMP")[0] != '0';
    g_pass_ab = getenv("SNAP_PASS_AB") ? atoi(getenv("SNAP_PASS_AB")) : 0;
    g_pass_force = getenv("SNAP_PASS_FORCE") && getenv("SNAP_PASS_FORCE")[0] == '1';
    g_ao_force_level = getenv("SNAP_SSAO_FORCE") ? atoi(getenv("SNAP_SSAO_FORCE")) : 0;
    g_ao_force = g_ao_force_level == 1 || g_ao_force_level == 2;
    if (const char* o = getenv("SNAP_PASS_OFF")) {
        std::string s = o, cur;
        for (char ch : s + ",") {
            if (ch == ',') { if (!cur.empty()) g_pass_off.push_back(cur); cur.clear(); }
            else cur.push_back((char)tolower((unsigned char)ch));
        }
    }
    if (g_simple_n || g_hbao_dirs || g_ao_radius > 0.0f || g_ao_strength > 0.0f || g_trace || g_pass_dump || !g_pass_off.empty())
        fprintf(stderr, "gamemods: SimpleAO samples %d, HBAO %dx%d, trace %d, pass dump %d, %zu pass(es) off\n", g_simple_n, g_hbao_dirs,
                g_hbao_samples, (int)g_trace, (int)g_pass_dump, g_pass_off.size());
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

}  // namespace

// Inside the SimpleAO / Hemisphere / Alchemy pass execute (main+0x5a5970), after `mov x21, x9` at main+0x5a6c90: x21 = the pass object.
// +0x17c is kernelSize (u32), +0x180 kernelRadius, +0x184 strength; the cbuffer is built from them just after. Hooked before 0x5a6c94.
extern "C" void rt_gm_ao_simple(Cpu* c) {
    init();
    uint8_t* self = (uint8_t*)c->x[21];
    if (!self || !readable(self + 0x174, 0x14)) return;
    static int hits;
    if (g_trace && hits < 8) {
        hits++;
        float r, s;
        memcpy(&r, self + 0x180, 4);
        memcpy(&s, self + 0x184, 4);
        trace("gamemods: SimpleAO pass %p kernelSize %u radius %g strength %g (hemisphere flag %u)", (void*)self, *(uint32_t*)(self + 0x17c), r, s,
              (unsigned)self[0x188]);
    }
    if (g_simple_n) *(uint32_t*)(self + 0x17c) = (uint32_t)g_simple_n;
}

// Inside the HBAO pass execute (main+0x5a7880), before `ldr w8, [x21, #0x174]` at main+0x5a7bac: x21 = the pass object, +0x174 numDirections,
// +0x178 numSamples (ints, copied from the preset tables at main+0xb15c40 / 0xb15ca0 earlier in the same function).
extern "C" void rt_gm_ao_hbao(Cpu* c) {
    init();
    uint8_t* self = (uint8_t*)c->x[21];
    if (!self || !readable(self + 0x174, 8)) return;
    static int hits;
    if (g_trace && hits < 8) {
        hits++;
        trace("gamemods: HBAO pass %p numDirections %d numSamples %d", (void*)self, *(int*)(self + 0x174), *(int*)(self + 0x178));
    }
    if (g_hbao_dirs) {
        *(int*)(self + 0x174) = g_hbao_dirs;
        *(int*)(self + 0x178) = g_hbao_samples;
    }
}

namespace {
extern "C" uint64_t snap_present_count();   // host/boot.cpp
struct PassState { uint8_t* pass; int orig_on; int refused; int zeroed; int seen; int game_on; uint64_t uuid[2]; };   // uuid = pass+0x30: unique per pass object, so a freed and reused address starts fresh
std::vector<PassState> g_pass_state;

// Pass objects (class at vtable main+0xcc4058, 0xf8 bytes) hold their name as a libc++ std::string at +0x40 (short form: byte0 = len << 1, text at +1;
// long form: size at +8, pointer at +16).
std::string pass_name(const uint8_t* pass) {
    const uint8_t* s = pass + 0x40;
    const char* p;
    size_t n;
    if ((s[0] & 1) == 0) { n = s[0] >> 1; p = (const char*)s + 1; }
    else {
        n = *(const uint64_t*)(s + 8);
        p = *(const char* const*)(s + 16);
        if (n > 64 || !p || !readable(p, n)) return "";
    }
    if (n > 40) return "";
    for (size_t i = 0; i < n; i++) if (p[i] < 0x20 || p[i] > 0x7e) return "";
    return std::string(p, n);
}
// 0 = never (photo, evaluation, capture, UI path), 1 = verified safe, 2 = unverified. Input is the lower-case name.
int pass_class(const std::string& n) {
    static const char* never[] = {"eval", "photo", "capture", "thumb", "report", "resize", "final", "ui", "fade", "downsizetarget", "compositeblendbuffer"};
    for (auto k : never) if (n.find(k) != std::string::npos) return 0;
    static const char* safe[] = {"bloomcreate", "bloomdraw", "dofblurx", "dofblury", "dof", "filmgrain", "chromaticaberration"};
    for (auto k : safe) if (n == k) return 1;
    return 2;
}
const char* pass_class_text(const std::string& name) {
    std::string low;
    for (char ch : name) low.push_back((char)tolower((unsigned char)ch));
    int c = pass_class(low);
    return c == 0 ? "protected" : c == 1 ? "toggle" : "unverified";
}

// ---- SNAP_SSAO_FORCE=1 (P4): switch the game's own SSAO passes ON in scenes where it has them off (the course). A separate allow-list from
// SNAP_PASS_OFF (that path is unchanged). Gate: never while any photo / evaluation / capture pass of the frame's list is enabled. The byte is forced
// only for the duration of the frame's loop: it is put back to the game's value at the LAST entry visit, so the game's own setters (and a later disable)
// always act on their own value; a pass the game itself has on is never touched. Loop: x20 = entry, x19 = end of the list (stride 32).
std::vector<uint8_t*> g_forced;   // passes whose byte WE set to 1 in this frame (the game had 0)
bool g_gate_open;
uint8_t* g_last_end;
uint64_t g_last_ent;
int g_force_frames, g_force_blocked_frames;

std::string lower(const std::string& s) { std::string o; for (char ch : s) o.push_back((char)tolower((unsigned char)ch)); return o; }
bool is_ao_pass(const std::string& low) { return low == "ssaocreate" || low == "ssaoblurx" || low == "ssaobluey" || low == "ssaocomposite"; }
bool is_photo_pass(const std::string& low) {
    static const char* k[] = {"eval", "photo", "capture", "thumb", "report", "resize"};
    for (auto s : k) {
        if (g_ao_force_level == 2 && !strcmp(s, "eval")) continue;   // relaxed: the Evaluation pass alone is not a reason (it writes only its own depth target)
        if (low.find(s) != std::string::npos) return true;
    }
    return false;
}

void ao_force_visit(Cpu* c, uint8_t* ent, uint8_t* pass, const std::string& name) {
    uint8_t* end = (uint8_t*)c->x[19];
    // first entry of a frame: the entry address does not increase (or the list moved): decide the gate for the whole list
    if (end != g_last_end || (uint64_t)ent <= g_last_ent) {
        for (uint8_t* f : g_forced) if (readable(f, 0x100)) f[0xf0] = 0;   // leftover from a frame whose last entry was forced
        g_forced.clear();
        g_gate_open = true;
        const bool abOn = snaplight::ab_on(snap_present_count());   // SNAP_LIGHT_AB: even blocks leave the game's own state
        std::string why;
        for (uint8_t* e = ent; e + 16 <= end && (size_t)(end - e) < 32 * 512; e += 32) {
            if (!readable(e, 16)) { g_gate_open = false; why = "unreadable list"; break; }
            uint8_t* p = *(uint8_t**)(e + 8);
            if (!p || !readable(p, 0x100)) continue;
            std::string nm = lower(pass_name(p));
            if (!nm.empty() && is_photo_pass(nm) && p[0xf0]) { g_gate_open = false; why = nm; break; }
        }
        if (!abOn) g_gate_open = false;
        if (g_gate_open) g_force_frames++; else if (abOn) g_force_blocked_frames++;
        static std::string lastWhy = "?";
        static int lastState = -1, changes;
        const std::string state = g_gate_open ? std::string("open") : why;
        if (g_trace && abOn && ((int)g_gate_open != lastState || state != lastWhy)) {
            if (changes++ < 60) trace("gamemods: SSAO force gate %s (present %llu; %d open, %d blocked frames so far)", g_gate_open ? "open" : ("closed by \"" + why + "\"").c_str(),
                                      (unsigned long long)snap_present_count(), g_force_frames, g_force_blocked_frames);
            lastState = g_gate_open; lastWhy = state;
        }
        g_last_end = end;
    }
    g_last_ent = (uint64_t)ent;
    const std::string low = lower(name);
    if (g_gate_open && is_ao_pass(low) && !pass[0xf0]) {
        if (g_forced.size() < 16) g_forced.push_back(pass);
        pass[0xf0] = 1;
        static int n;
        if (g_trace && n++ < 8) trace("gamemods: SSAO force: pass \"%s\" %p switched on for this frame", name.c_str(), (void*)pass);
    }
    if (ent + 32 >= end && !g_forced.empty()) {   // last entry: give every forced byte back to the game (its value was 0)
        for (uint8_t* f : g_forced) if (f != pass && readable(f, 0x100)) f[0xf0] = 0;
        if (std::find(g_forced.begin(), g_forced.end(), pass) == g_forced.end()) g_forced.clear();
        else { g_forced.assign(1, pass); }   // the last entry itself is forced: restored at the next frame's first visit
    }
}

}  // namespace

// Pass loop (main+0x988fe0, entries of 32 bytes at [ctx+0x1b8, ctx+0x1c0)): at main+0x989048 x20 = the entry, [x20+8] = the pass object, whose
// byte at +0xf0 is the enable flag the loop copies to +0xf1 and tests. Names: pass_name (libc++ string at +0x40).
extern "C" void rt_gm_pass(Cpu* c) {
    init();
    if (!g_pass_dump && g_pass_off.empty() && !g_ao_force) return;
    uint8_t* ent = (uint8_t*)c->x[20];
    if (!ent || !readable(ent, 16)) return;
    uint8_t* pass = *(uint8_t**)(ent + 8);
    if (!pass || !readable(pass, 0x100)) return;
    std::string name = pass_name(pass);
    if (g_ao_force) ao_force_visit(c, ent, pass, name);
    if (g_pass_dump) {
        static std::vector<uint8_t*> seen;
        bool known = false;
        for (auto p : seen) if (p == pass) known = true;
        if (!known && seen.size() < 256) {
            seen.push_back(pass);
            uint64_t vt = *(uint64_t*)pass;
            trace("gamemods: pass %p name \"%s\" order[+0x5c]=%u on[+0xf0]=%u vtable main+%#llx class %s", (void*)pass, name.c_str(), *(uint32_t*)(pass + 0x5c),
                  pass[0xf0], (unsigned long long)(vt - kMainBase), pass_class_text(name));
        }
    }
    if (g_pass_off.empty() || name.empty()) return;
    std::string low;
    for (char ch : name) low.push_back((char)tolower((unsigned char)ch));
    bool listed = false;
    for (auto& n : g_pass_off) if (n == low) listed = true;
    if (!listed) return;
    int cls = pass_class(low);
    PassState* st = nullptr;
    uint64_t uu[2];
    memcpy(uu, pass + 0x30, 16);
    for (size_t i = 0; i < g_pass_state.size(); i++) {
        PassState& s = g_pass_state[i];
        if (s.pass != pass) continue;
        if (s.uuid[0] == uu[0] && s.uuid[1] == uu[1]) st = &s;
        else { g_pass_state.erase(g_pass_state.begin() + i); i--; }   // the address was freed and reused by another pass object
    }
    if (!st) {
        if (g_pass_state.size() >= 128) return;
        g_pass_state.push_back({pass, 0, 0, 0, 0, 0, {uu[0], uu[1]}});
        st = &g_pass_state.back();
        if (cls == 0) { trace("gamemods: pass \"%s\" is a photo/evaluation pass: SNAP_PASS_OFF refused", name.c_str()); st->refused = 1; }
        else if (cls == 2 && !g_pass_force) { trace("gamemods: pass \"%s\" is not verified safe to switch off: refused (SNAP_PASS_FORCE=1 overrides)", name.c_str()); st->refused = 1; }
        else trace("gamemods: pass \"%s\" switched off%s", name.c_str(), g_pass_ab ? " (A/B)" : "");
    }
    if (st->refused) return;
    bool off = !g_pass_ab || ((snap_present_count() / (uint64_t)g_pass_ab) & 1) == 0;
    // The game may rewrite the byte (a scene turns the pass on or off): remember its last value unless we wrote the zero ourselves.
    if (!st->zeroed || pass[0xf0]) st->orig_on = pass[0xf0];
    st->seen++;
    if (pass[0xf0]) st->game_on++;
    if (g_trace && (st->seen == 1 || st->seen % 1800 == 0)) trace("gamemods: pass \"%s\" seen %d times, game had it on %d times", name.c_str(), st->seen, st->game_on);
    if (off) { pass[0xf0] = 0; st->zeroed = 1; }
    else { if (st->zeroed) pass[0xf0] = (uint8_t)st->orig_on; st->zeroed = 0; }
}

// ------------------------------------------------------------------------------------------------ sharper UI fonts (#29)
// The glyph cache (main+0x435560) asks nn::fontll for a glyph bitmap (PLT 0xa37dc0 after SetScale 0xa37d90), packs it into an R8 atlas
// (dims at cache+0x2188/+0x218c, created in 0x63fdc0) and fills the glyph entry: +0x1a/+0x1c bitmap size in the atlas and the UV source, +0x1e/+0x20
// layout size, +0x22 advance, +0x24.. bearings (the quad builder 0x438960 uses +0x1a/+0x1c for UV and the rest for the screen quad).
// With SNAP_UI_FONT_SCALE=k: the atlas texture is k times larger in both axes; right after each GetGlyph the hook renders the same glyph again at
// k times the scale and hands the cache a composite (the k-times bitmap and size, but the 1x bearings and advance of the first call); a third hook
// writes the 1x bitmap size back into the entry's layout size, so text layout is exactly the original and only the UV rect covers more texels.
const uint64_t kMagic = 0x32544e4650414e53ull;   // "SNAPFNT2"
thread_local uint32_t t_font_scale;               // 16.16 scale of the current glyph request (w24 at the SetScale call)
thread_local std::vector<uint8_t> t_font_ring[8];
thread_local int t_font_ring_i;

void font_composite(Cpu* c, bool first) {
    uint8_t* r1 = (uint8_t*)c->x[0];
    if (!r1 || !readable(r1, 0x30)) return;
    uint16_t w1 = *(uint16_t*)(r1 + 0x1c), h1 = *(uint16_t*)(r1 + 0x1e);
    if (!w1 || !h1 || !readable(r1, 0x28 + (size_t)w1 * h1)) return;
    uint8_t hdr1[0x28];
    memcpy(hdr1, r1, 0x28);
    uint64_t engine = *(uint64_t*)(c->x[29] - 0x68);
    uint32_t cp = *(uint32_t*)(c->sp + 0x3c);
    if (first) t_font_scale = (uint32_t)c->x[24];
    uint32_t s = t_font_scale;
    if (!engine || !s || (uint64_t)s * g_font_k > 0x7fffffffull) return;
    uint64_t saved[8];
    for (int i = 0; i < 8; i++) saved[i] = c->x[i];
    uint32_t sk = s * (uint32_t)g_font_k;
    call_guest_nested(c, kMainBase + 0xa37d90, {engine, sk, 0, 0, sk});
    uint8_t* r2 = (uint8_t*)call_guest_nested(c, kMainBase + 0xa37dc0, {engine, cp, 8});
    call_guest_nested(c, kMainBase + 0xa37d90, {engine, s, 0, 0, s});
    for (int i = 1; i < 8; i++) c->x[i] = saved[i];
    c->x[0] = (uint64_t)r1;
    if (!r2 || !readable(r2, 0x30)) return;
    uint16_t w2 = *(uint16_t*)(r2 + 0x1c), h2 = *(uint16_t*)(r2 + 0x1e);
    if (w2 < w1 || h2 < h1 || !readable(r2, 0x28 + (size_t)w2 * h2)) return;
    std::vector<uint8_t>& b = t_font_ring[t_font_ring_i++ & 7];
    b.assign(16 + 0x28 + (size_t)w2 * h2, 0);
    memcpy(b.data(), &kMagic, 8);
    memcpy(b.data() + 8, &w1, 2);
    memcpy(b.data() + 10, &h1, 2);
    memcpy(b.data() + 16, r2, 0x28 + (size_t)w2 * h2);
    memcpy(b.data() + 16 + 0xc, hdr1 + 0xc, 6);   // bearing x, bearing y, advance: the 1x values
    c->x[0] = (uint64_t)(b.data() + 16);
    static int n, n2, total[2];
    total[first ? 0 : 1]++;
    if (g_trace && first && n < 6) { n++; trace("gamemods: font glyph %#x 1x %ux%u -> %ux%u (scale %#x)", cp, w1, h1, w2, h2, s); }
    if (g_trace && !first && n2 < 6) { n2++; trace("gamemods: font glyph2 %#x 1x %ux%u -> %ux%u (scale %#x)", cp, w1, h1, w2, h2, s); }
    if (g_trace && (total[0] + total[1]) % 500 == 0) trace("gamemods: font glyph calls %d first-site, %d second-site", total[0], total[1]);
}

// Client height of this process's largest visible top-level window (the real output: borderless fullscreen gives the monitor height); 0 if none.
BOOL CALLBACK font_enum_win(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT r;
    if (GetClientRect(h, &r) && r.bottom > *(int*)lp) *(int*)lp = r.bottom;
    return TRUE;
}

// Decides g_font_k once, at the first atlas creation (the window exists by then). SNAP_UI_FONT_SCALE: 1 or off = off, 2..4 = that scale,
// auto / default = 2 when the output is 1440 lines or taller. Unset = the shared defaults table (hle/visual_defaults.h: off in the
// original profile, auto in the enhanced one); the F1 menu stores "default" as unset.
void decide_font_scale() {
    g_font_decided = true;
    const char* f = snapdef::get("SNAP_UI_FONT_SCALE");
    if (f && *f && strcmp(f, "auto") && strcmp(f, "default")) {
        int k = !strcmp(f, "off") ? 1 : atoi(f);
        if (k >= 2 && k <= 4) g_font_k = k;
        fprintf(stderr, "gamemods: UI font scale %d (SNAP_UI_FONT_SCALE=%s)\n", g_font_k ? g_font_k : 1, f);
        return;
    }
    int h = 0;
    EnumWindows(font_enum_win, (LPARAM)&h);
    if (h <= 0) { if (const char* e = getenv("SNAP_WINDOW_H")) h = atoi(e); }
    if (h >= 1440) g_font_k = 2;
    fprintf(stderr, "gamemods: UI font scale %d (auto, output height %d)\n", g_font_k ? g_font_k : 1, h);
}

// ----- the five hook sites (recomp/snap_gen.py MAIN_INLINE_HOOKS)
// Before main+0x6410bc (the call that creates the atlas texture, texture info at sp+0xe0, width/height as two u32 at +0x18): enlarge it.
extern "C" void rt_gm_font_atlas_info(Cpu* c) {
    init();
    if (!g_font_decided) decide_font_scale();
    if (!g_font_k) return;
    uint32_t* wh = (uint32_t*)(c->sp + 0xf8);
    if (wh[0] == 512 && wh[1] == 2048) { wh[0] *= g_font_k; wh[1] *= g_font_k; }
}
// Before main+0x641288 (just after `str x20, [x25, #0x2188]`): the cache's own atlas size, used for the pitch and the UV divide.
extern "C" void rt_gm_font_atlas_dims(Cpu* c) {
    if (!g_font_k) return;
    uint32_t* wh = (uint32_t*)(c->x[25] + 0x2188);
    if (wh[0] == 512 && wh[1] == 2048) { wh[0] *= g_font_k; wh[1] *= g_font_k; }
}
// Before main+0x4358c0 and main+0x435eb0 (just after the GetGlyph calls): x0 = the glyph result.
extern "C" void rt_gm_font_glyph1(Cpu* c) { if (g_font_k) font_composite(c, true); }
extern "C" void rt_gm_font_glyph2(Cpu* c) { if (g_font_k) font_composite(c, false); }
// Before main+0x435cf0 (copies the bitmap size to the entry's +0x1a/+0x1c): x1 = the glyph result, x21 = the entry.
extern "C" void rt_gm_font_fix(Cpu* c) {
    if (!g_font_k) return;
    uint8_t* r = (uint8_t*)c->x[1];
    uint8_t* e = (uint8_t*)c->x[21];
    if (!r || !e || !readable(r - 16, 16) || memcmp(r - 16, &kMagic, 8)) return;
    memcpy(e + 0x1e, r - 8, 2);
    memcpy(e + 0x20, r - 6, 2);
}
