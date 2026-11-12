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
//   SNAP_LIGHTING=high                     preset: SNAP_SSAO_QUALITY=high plus SNAP_SHADOW_RES=2048 (the latter read by runtime/nvn
//                                          ApplyHostEnvPolicy); every explicit switch above still wins.
//   SNAP_UI_FONT_SCALE=auto|1|2..4         UI glyphs are rasterized at k times the size into a k times larger atlas (#29, sharper fonts); the
//                                          layout fields of every glyph entry keep the 1x values, so text size and position do not change.
//                                          Unset or auto: k = 2 when the game window's client height is 1440 or more, else 1 (off). 1 = off.
//   SNAP_GM_TRACE=1                        logs the values the game had before the override (first 8 hits of each hook).
//   SNAP_PASS_DUMP=1                       logs each distinct render pass object once (vtable, flags, strings) for pass identification.
//   SNAP_PASS_OFF=name,name,...            disables post passes by name (see pass_name_matches below).
//
// Addresses (module offsets, build id DDB900FF...): analysis/mods/ and docs/mod-ideas.md rows 7 and 16.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"

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

void init() {
    if (g_init_done) return;
    g_init_done = true;
    const char* q = getenv("SNAP_SSAO_QUALITY");
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

// Pass loop (main+0x988fe0, entries of 32 bytes at [ctx+0x1b8, ctx+0x1c0)): at main+0x989048 x20 = the entry, [x20+8] = the pass object, whose
// byte at +0xf0 is the enable flag the loop copies to +0xf1 and tests. See pass_toggle below for how names are matched.
extern "C" void rt_gm_pass(Cpu* c) {
    init();
    if (!g_pass_dump && g_pass_off.empty()) return;
    uint8_t* ent = (uint8_t*)c->x[20];
    if (!ent || !readable(ent, 16)) return;
    uint8_t* pass = *(uint8_t**)(ent + 8);
    if (!pass || !readable(pass, 0x100)) return;
    if (g_pass_dump) {
        static std::vector<uint8_t*> seen;
        for (auto p : seen) if (p == pass) goto toggled;
        if (seen.size() < 256) {
            seen.push_back(pass);
            uint64_t vt = *(uint64_t*)pass;
            trace("gamemods: pass %p vtable main+%#llx type[+0x5c]=%u on[+0xf0]=%u [+0x60]=%u", (void*)pass, (unsigned long long)(vt - kMainBase),
                  *(uint32_t*)(pass + 0x5c), pass[0xf0], pass[0x60]);
            // printable strings anywhere in the first 0x100 bytes, and one level down through pointers (libc++ short and long strings)
            for (int off = 0; off < 0x100; off += 8) {
                uint64_t q = *(uint64_t*)(pass + off);
                char buf[48];
                int n = 0;
                const char* cand = nullptr;
                if (off + 1 < 0x100 && (pass[off] & 1) == 0 && (pass[off] >> 1) > 2 && (pass[off] >> 1) < 23) cand = (const char*)(pass + off + 1);
                else if (q > 0x10000 && q < 0x7fffffffffffull && readable((const void*)q, 16)) cand = (const char*)q;
                if (!cand) continue;
                while (n < 40 && cand[n] >= 0x20 && cand[n] < 0x7f) n++;
                if (n >= 4) { memcpy(buf, cand, n); buf[n] = 0; trace("gamemods:   +%#x string \"%s\"", off, buf); }
            }
        }
    }
toggled:
    return;
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
// unset / auto / default = 2 when the output is 1440 lines or taller (the F1 menu stores "default" as unset).
void decide_font_scale() {
    g_font_decided = true;
    const char* f = getenv("SNAP_UI_FONT_SCALE");
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
