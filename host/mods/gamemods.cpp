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
    g_trace = getenv("SNAP_GM_TRACE") && getenv("SNAP_GM_TRACE")[0] != '0';
    g_pass_dump = getenv("SNAP_PASS_DUMP") && getenv("SNAP_PASS_DUMP")[0] != '0';
    if (const char* o = getenv("SNAP_PASS_OFF")) {
        std::string s = o, cur;
        for (char ch : s + ",") {
            if (ch == ',') { if (!cur.empty()) g_pass_off.push_back(cur); cur.clear(); }
            else cur.push_back((char)tolower((unsigned char)ch));
        }
    }
    if (g_simple_n || g_hbao_dirs || g_trace || g_pass_dump || !g_pass_off.empty())
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
