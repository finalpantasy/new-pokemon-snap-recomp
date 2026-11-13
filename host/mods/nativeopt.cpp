// Native PC options inside the game's own Settings screen (owner 2026-11-13: "native, in game menu for options, not an F1
// menu"). The screens themselves are data: scripts/ui/make_native_options.py patches the screen's behavior file option.lmb
// (a local mod pack, built on the user's PC from their own files) with ten PC tabs (states PcTab0..9) that follow the game's
// "Other" tab, plus one popup state per pulldown row (PcPull<tab>_<slot>). It also writes rows.txt (the row table) next to
// the pack. This file drives those states from two inline hooks (MAIN_INLINE_HOOKS in recomp/snap_gen.py):
//
//   main+0x728e10  rt_ui_opt_event  entry of the option screen's event handler (a std::function body; x0 = handler object,
//                                   w1 = event, x2 = the Lumen player, x3 = std::string* state, x4 = element name for event 8
//                                   (focus moved) and 10 (decide); 11/19 = cancel (B), 22 = a per-frame update sent all game
//                                   long). In a PC state the game's handler would not know the state, so the hook handles the
//                                   event itself and turns w1 into 7 (a no-op case), except cancel and the per-frame update,
//                                   which it passes on as if the state were "Others" (B closes the screen as on any tab).
//   SetState calls rt_ui_opt_state  the `blr` of the handler's 8 calls to player->vtable[0x140](player, desc): x1 = the state
//                                   descriptor. Used to keep a template element entry (see below) and for the route anchor.
//
// Changing state is the Lumen player's own work, driven by option.lmb: an element whose f2 names a state (and f6 = 1) goes
// there on decide (tab elements, the pulldown rows), f6 = 4 returns to the previous state (popup items), and L/R open the
// state named at +0x30 of the page's Prev/Next element entries (BuildPage: the neighbor game tabs; the hook points them at
// the PC tabs: R on "Other" to PC tab 1, L on "Game" to the last, and along the PC tabs). The player then sends event 5 with the new state, and the game's handler answers
// with BuildPage + SetState (the page data: variables and the element list). For a PC state the hook does that part:
// it builds a descriptor with the game's own BuildPage (main+0x737360: state name of a game tab, the
// tab-bar variables TabNum/Tab%dLabel/..., all in a 0xf8-byte object), renames the state, adds the PC variables (current
// choices, pulldown texts, slider dots) to its std::map<string,string> at +0xa8 and lists the focusable elements in its
// element vector at +0x90 (0x130-byte entries, name at +0 and +0x18; the cursor only reaches listed elements), then calls
// SetState (calling SetState at any other time does not switch states). Values are read and written where the F1 overlay keeps them: settings keys through the F1 option table
// (snap::ui::options_test_get/set: same defaults, custom stores, presets), the controls mapping (hle::input::hub()), the
// master volume (audio layer). Restore Default Settings (Minus) on a PC tab resets that tab's rows only.
//
// Without the pack (rows.txt missing, or the pack is not active) every hook returns at once and the game is unchanged.
// SNAP_NATIVE_OPT_TRACE=1 logs events, states and element lists. SNAP_NATIVE_OPT_SETTINGS=<file>: test aid for runs that
// skip settings::load() (SNAP_SAVE_DIR set): use this settings.cfg.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "loader.h"
#include "hle/settings.h"
#include "hle/input_core.h"
#include "audio/audio_out.h"
#include "mods/mods_core.h"

namespace hle::input { void script_event(const char* name); }   // runtime/hle/input_core.cpp
namespace snap::ui {                                              // runtime/ui/ui_options.cpp (the F1 option table)
bool options_test_set(const std::string& key, const std::string& value);
std::string options_test_get(const std::string& key);
}

using namespace loader;

namespace {

const uint64_t kMainBase = 0x7100000000ull;
const uint64_t kMapEmplace = kMainBase + 0x6d7df0;   // map<string,string>::__emplace_unique_key_args(map, key, key&&, const value&)
const uint64_t kElemCopy = kMainBase + 0x656170;     // element entry copy constructor (dst, const src&)
const uint64_t kVecPushSlow = kMainBase + 0x6565c0;  // vector<element entry>::__push_back_slow_path(vec, const value&)
const uint64_t kBuildPage = kMainBase + 0x737360;    // BuildPage(desc, handler, tab id): a fresh descriptor for a game tab
const uint64_t kTabTable = kMainBase + 0x12ffc18;    // per tab id: std::string state name (+0), label (+0x18); 0x30 apart
const uint64_t kElemSize = 0x130;
const uint64_t kDescSize = 0xf8;
const char* const kGameTabs[5] = {"Game", "Camera", "Button", "Sound", "Others"};

bool trace_on() {
    static int t = -1;
    if (t < 0) { const char* e = getenv("SNAP_NATIVE_OPT_TRACE"); t = (e && *e && *e != '0') ? 1 : 0; }
    return t == 1;
}

// ---- guest libc++ strings (24 bytes): short if bit 0 of byte 0 is clear (size = byte0 >> 1, chars at +1), long otherwise
// (capacity word with bit 0 set, size at +8, data pointer at +16). Guest memory is identity mapped, so the guest reads ours.
std::string read_str(uint64_t p) {
    if (!p) return {};
    const uint8_t* s = (const uint8_t*)p;
    if (!(s[0] & 1)) return std::string((const char*)s + 1, s[0] >> 1);
    uint64_t n = *(const uint64_t*)(s + 8), d = *(const uint64_t*)(s + 16);
    if (!d || n > 4096) return {};
    return std::string((const char*)d, (size_t)n);
}
struct GStr {
    alignas(8) uint8_t b[24] = {};
    std::string keep;   // backing chars of a long string
    explicit GStr(const std::string& s) : keep(s) {
        if (s.size() <= 22) { b[0] = (uint8_t)(s.size() << 1); memcpy(b + 1, s.data(), s.size()); return; }
        uint64_t cap = (keep.size() + 16) | 1, n = keep.size(), d = (uint64_t)(uintptr_t)keep.c_str();
        memcpy(b, &cap, 8); memcpy(b + 8, &n, 8); memcpy(b + 16, &d, 8);
    }
    GStr(const GStr&) = delete;
    uint64_t p() const { return (uint64_t)(uintptr_t)b; }
};
bool put_short(uint64_t at, const std::string& s) {   // overwrite a short guest string in place
    if (s.size() > 22 || (*(const uint8_t*)at & 1)) return false;
    memset((void*)at, 0, 24);
    *(uint8_t*)at = (uint8_t)(s.size() << 1);
    memcpy((void*)(at + 1), s.data(), s.size());
    return true;
}

// ---- the row table (rows.txt from the generator)
struct Row { int tab = 0, slot = 0; char widget = 'c'; std::string store, key, def; std::vector<std::pair<std::string, std::string>> ch; };
struct Tab { std::string state, name; std::vector<Row> rows; };
std::vector<Tab> g_tabs;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) { size_t b = s.find(sep, a); out.push_back(s.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
    return out;
}

// The pack is active when the mods index (same root and enable rules as the loader) has its option.lmb override.
bool pack_active(const std::string& root) {
    mods::Index idx;
    std::vector<std::string> warn;
    mods::load(root, idx, warn);
    for (const auto& kv : idx.map)
        if (idx.packs[kv.second.pack].id == "native-options" && kv.second.label.find("option.drp") != std::string::npos) return true;
    return false;
}

bool rows_ready() {
    static int ok = -1;
    if (ok >= 0) return ok == 1;
    ok = 0;
    std::string root = mods::default_root();
    std::ifstream f(root + "\\native-options\\rows.txt");
    if (!f) return false;
    if (!pack_active(root)) { trace("nativeopt: native-options pack not active: PC tabs off"); return false; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto v = split(line, '\t');
        if (v[0] == "T" && v.size() >= 4) {
            int t = atoi(v[1].c_str());
            if (t != (int)g_tabs.size()) return false;
            g_tabs.push_back(Tab{v[2], v[3], {}});
        } else if (v[0] == "R" && v.size() >= 8) {
            Row r;
            r.tab = atoi(v[1].c_str()); r.slot = atoi(v[2].c_str()); r.widget = v[3].empty() ? 'c' : v[3][0];
            r.store = v[4]; r.key = v[5]; r.def = v[6];
            if (!v[7].empty())
                for (const std::string& c : split(v[7], '|')) { size_t e = c.find('='); if (e != std::string::npos) r.ch.push_back({c.substr(0, e), c.substr(e + 1)}); }
            if (r.tab < 0 || r.tab >= (int)g_tabs.size()) return false;
            g_tabs[r.tab].rows.push_back(r);
        }
    }
    ok = g_tabs.empty() ? 0 : 1;
    if (ok) trace("nativeopt: %zu PC tabs from %s\\native-options\\rows.txt", g_tabs.size(), root.c_str());
    return ok == 1;
}

void ensure_settings() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* f = getenv("SNAP_NATIVE_OPT_SETTINGS");
    if (f && *f && hle::settings::file_path().empty()) {
        hle::settings::use_path_for_test(f);
        trace("nativeopt: settings file %s (SNAP_NATIVE_OPT_SETTINGS)", f);
    }
}

// ---- values: the same stores as the F1 overlay
bool* map_bool(hle::input::Mapping& m, const std::string& k) {
    if (k == "swap_sticks") return &m.swap_sticks;
    if (k == "left_invert_y") return &m.left.invert_y;
    if (k == "right_invert_y") return &m.right.invert_y;
    if (k == "toggle_zl") return &m.toggle_zl;
    if (k == "toggle_zr") return &m.toggle_zr;
    if (k == "mouse_look") return &m.mouse_look;
    if (k == "mouse_invert_x") return &m.mouse_invert_x;
    if (k == "mouse_invert_y") return &m.mouse_invert_y;
    if (k == "mouse_touch") return &m.mouse_touch;
    if (k == "lock_cursor") return &m.lock_cursor;
    return nullptr;
}

std::string get_value(const Row& r) {
    ensure_settings();
    if (r.store == "opt") return snap::ui::options_test_get(r.key);
    if (r.store == "vol") { char b[8]; snprintf(b, sizeof b, "%d", (int)std::lround(snap_audio_get_volume(SNAP_VOL_MASTER) * 10.0f)); return b; }
    hle::input::Mapping m = hle::input::hub().mapping();
    if (r.key == "face_layout") return std::to_string(m.face_layout);
    if (r.key == "mouse_sensitivity") { char b[16]; snprintf(b, sizeof b, "%.2f", m.mouse_sensitivity); return b; }
    bool* p = map_bool(m, r.key);
    return p ? (*p ? "1" : "0") : r.def;
}

void set_value(const Row& r, const std::string& v) {
    ensure_settings();
    if (r.store == "opt") {
        std::string sv = v;
        if (r.widget == 'n') { char b[16]; snprintf(b, sizeof b, "%.2f", atoi(v.c_str()) / 10.0); sv = b; }
        bool ok = snap::ui::options_test_set(r.key, sv);
        trace("nativeopt: %s = %s%s", r.key.c_str(), sv.c_str(), ok ? "" : " (refused)");
        return;
    }
    if (r.store == "vol") { snap_audio_set_volume(SNAP_VOL_MASTER, atoi(v.c_str()) / 10.0f); trace("nativeopt: master volume = %s/10", v.c_str()); return; }
    hle::input::Hub& h = hle::input::hub();
    hle::input::Mapping m = h.mapping();
    if (r.key == "face_layout") { int l = atoi(v.c_str()); if (l != m.face_layout) hle::input::set_face_layout(m, l); }
    else if (r.key == "mouse_sensitivity") m.mouse_sensitivity = (float)atof(v.c_str());
    else if (bool* p = map_bool(m, r.key)) *p = v == "1";
    h.set_mapping(m);
    h.save();
    trace("nativeopt: controls %s = %s", r.key.c_str(), v.c_str());
}

void reset_row(const Row& r) {
    if (r.store == "map") {
        hle::input::Mapping d;
        hle::input::Mapping m = d;
        std::string v = r.key == "face_layout" ? std::to_string(d.face_layout)
                      : r.key == "mouse_sensitivity" ? std::to_string(d.mouse_sensitivity)
                      : (map_bool(m, r.key) && *map_bool(m, r.key)) ? "1" : "0";
        set_value(r, v);
        return;
    }
    if (r.store == "opt" && r.widget == 'n') { set_value(r, std::to_string((int)std::lround(atof(r.def.c_str()) * 10))); return; }
    set_value(r, r.def);
}

// Index of the stored value among the row's choices (numbers compare as numbers: "2" == "2.00"); -1 when none matches.
int choice_of(const Row& r, const std::string& v) {
    for (size_t i = 0; i < r.ch.size(); i++) if (r.ch[i].first == v) return (int)i;
    char* e1 = nullptr;
    double a = strtod(v.c_str(), &e1);
    if (v.empty() || *e1) return -1;
    for (size_t i = 0; i < r.ch.size(); i++) {
        char* e2 = nullptr;
        double b = strtod(r.ch[i].first.c_str(), &e2);
        if (!r.ch[i].first.empty() && !*e2 && std::fabs(a - b) < 1e-4) return (int)i;
    }
    return -1;
}
int slider_of(const Row& r) {
    std::string v = get_value(r);
    int n = r.store == "opt" ? (int)std::lround(atof(v.c_str()) * 10.0) : atoi(v.c_str());
    return n < 0 ? 0 : n > 10 ? 10 : n;
}

// ---- guest calls
struct Saved { uint64_t x[31]; };
uint64_t vfn(uint64_t obj, uint64_t off) { return *(const uint64_t*)(*(const uint64_t*)obj + off); }

std::string g_slider_landed;   // the slider the cursor is on: its first focus event only reports the knob it shows
alignas(16) uint8_t g_tmpl[kElemSize];   // a copy of one of the game's own element entries (all fields but the names zero)
bool g_have_tmpl = false;
int g_tab_id[5] = {-1, -1, -1, -1, -1};  // BuildPage tab id of each game tab position

bool find_tab_ids() {
    if (g_tab_id[0] >= 0) return true;
    for (int id = 0; id < 24; id++) {
        std::string n = read_str(kTabTable + (uint64_t)id * 0x30);
        for (int k = 0; k < 5; k++) if (n == kGameTabs[k] && g_tab_id[k] < 0) g_tab_id[k] = id;
    }
    for (int k = 0; k < 5; k++) if (g_tab_id[k] < 0) { trace("nativeopt: tab id of %s not found", kGameTabs[k]); return false; }
    trace("nativeopt: tab ids %d %d %d %d %d", g_tab_id[0], g_tab_id[1], g_tab_id[2], g_tab_id[3], g_tab_id[4]);
    return true;
}

void add_var(Cpu* c, uint64_t desc, const std::string& k, const std::string& v) {
    GStr k1(k), k2(k), val(v);
    call_guest_nested(c, kMapEmplace, {desc + 0xa8, k1.p(), k2.p(), val.p()});
}

// Element entries (0x130 bytes): name at +0 and +0x18, a target state at +0x30 (Prev/Next: the tab L/R open), the
// entry's variables (std::map) at +0x60.
uint64_t find_entry(uint64_t desc, const char* name) {
    const uint64_t* vec = (const uint64_t*)(desc + 0x90);
    for (uint64_t p = vec[0]; p && p < vec[1]; p += kElemSize) if (read_str(p) == name) return p;
    return 0;
}
void set_target(uint64_t desc, const char* name, const std::string& target) {
    uint64_t p = find_entry(desc, name);
    if (!p || !put_short(p + 0x30, target)) trace("nativeopt: could not point %s at %s", name, target.c_str());
}

void add_entry(Cpu* c, uint64_t desc, const std::string& name) {
    uint64_t* vec = (uint64_t*)(desc + 0x90);
    if (vec[1] != vec[2]) { call_guest_nested(c, kElemCopy, {vec[1], (uint64_t)(uintptr_t)g_tmpl}); vec[1] += kElemSize; }
    else call_guest_nested(c, kVecPushSlow, {desc + 0x90, (uint64_t)(uintptr_t)g_tmpl});
    uint64_t e = vec[1] - kElemSize;
    put_short(e, name);
    put_short(e + 0x18, name);
}

uint64_t new_desc(Cpu* c, uint64_t handler, int pos, const std::string& state) {
    uint64_t desc = (uint64_t)(uintptr_t)calloc(1, 0x100);   // not freed: SetState copies it, a few hundred bytes per switch
    call_guest_nested(c, kBuildPage, {desc, handler, (uint64_t)g_tab_id[pos]});
    if (!put_short(desc, state)) trace("nativeopt: could not rename state to %s", state.c_str());
    return desc;
}

// player->vtable[0x158](player, &state, &element, bool): the game's slider drawing calls it for every dot sub-element
// ("<slider>_NN") with true on the current value; used here for our sliders and for the popup's current item.
void mark(Cpu* c, uint64_t player, const std::string& state, const std::string& element, bool on) {
    GStr s(state), e(element);
    call_guest_nested(c, vfn(player, 0x158), {player, s.p(), e.p(), on ? 1ull : 0ull});
}

void set_state(Cpu* c, uint64_t player, uint64_t desc) {
    trace("nativeopt: SetState '%s' (host)", read_str(desc).c_str());
    call_guest_nested(c, vfn(player, 0x140), {player, desc});
}

void row_vars(Cpu* c, uint64_t desc, int t, const Row& r) {
    char p[32];
    snprintf(p, sizeof p, "PcT%dS%d", t, r.slot);
    std::string pre = p;
    if (r.widget == 'n') {
        int n = slider_of(r);
        for (int d = 1; d <= 10; d++) { char k[8]; snprintf(k, sizeof k, "D%02d", d); add_var(c, desc, pre + k, d <= n ? "lb_on" : "lb_off"); }
        add_var(c, desc, pre + "Num", std::to_string(n));
        return;
    }
    std::string v = get_value(r);
    int cur = choice_of(r, v);
    if (r.widget == 'c') {
        for (int k = 0; k < 2; k++) add_var(c, desc, pre + "C" + std::to_string(k + 1), k == cur ? "lb_decide" : "lb_unselect");
        return;
    }
    add_var(c, desc, pre + "V", cur >= 0 ? r.ch[(size_t)cur].second : v);
    for (size_t k = 0; k < r.ch.size(); k++) add_var(c, desc, pre + "I" + std::to_string(k + 1), (int)k == cur ? "lb_active" : "lb_unselect");
}

void redraw_row(Cpu* c, uint64_t player, const Row& r);

void go_tab(Cpu* c, uint64_t handler, uint64_t player, int t) {
    if (!g_have_tmpl || !find_tab_ids()) { trace("nativeopt: no element template yet"); return; }
    const Tab& tab = g_tabs[(size_t)t];
    uint64_t desc = new_desc(c, handler, t % 5, tab.state);
    // BuildPage listed the five game tabs: they become this set's PC tabs; L/R go where Prev/Next name at +0x30
    const int base = (t / 5) * 5, n = (int)g_tabs.size();
    for (int i = 0; i < 5 && base + i < n; i++)
        if (uint64_t p = find_entry(desc, kGameTabs[i])) { put_short(p, g_tabs[(size_t)base + i].state); put_short(p + 0x18, g_tabs[(size_t)base + i].state); }
    set_target(desc, "Prev", t == 0 ? "Others" : g_tabs[(size_t)t - 1].state);
    set_target(desc, "Next", t + 1 < n ? g_tabs[(size_t)t + 1].state : "Game");
    for (const Row& r : tab.rows) {
        row_vars(c, desc, t, r);
        char n[32];
        if (r.widget == 'c') for (int k = 1; k <= 2; k++) { snprintf(n, sizeof n, "PcT%dS%dC%d", t, r.slot, k); add_entry(c, desc, n); }
        else if (r.widget == 'p') { snprintf(n, sizeof n, "PcT%dS%dP", t, r.slot); add_entry(c, desc, n); }
        else { snprintf(n, sizeof n, "PcT%dS%dN", t, r.slot); add_entry(c, desc, n); snprintf(n, sizeof n, "PcT%dS%dM", t, r.slot); add_entry(c, desc, n); }
    }
    set_state(c, player, desc);
    for (const Row& r : tab.rows) {
        if (r.widget == 'p') redraw_row(c, player, r);   // back from a popup the player keeps the old pulldown texts
        if (r.widget != 'n') continue;
        int v = slider_of(r);   // the cursor lands on the knob of the value (on 0: dot 1, so Right/Left still work)
        for (int d = 1; d <= 10; d++) { char n[40]; snprintf(n, sizeof n, "PcT%dS%dN_%02d", t, r.slot, d); mark(c, player, tab.state, n, d == (v < 1 ? 1 : v)); }
    }
    g_slider_landed.clear();
}

void open_pull(Cpu* c, uint64_t handler, uint64_t player, int t, const Row& r) {
    if (!g_have_tmpl || !find_tab_ids()) return;
    char st[32];
    snprintf(st, sizeof st, "PcPull%d_%d", t, r.slot);
    uint64_t desc = new_desc(c, handler, t % 5, st);
    row_vars(c, desc, t, r);
    add_entry(c, desc, "PulldownMenu");   // the game's own list widget (type 6): Up/Down inside it, items report PulldownMenuItemNN
    set_state(c, player, desc);
}

// Decide on a pulldown row, before the player switches to the popup state: as the game's list helper does at the same
// point (main+0x776c80), register the items with player->vtable[0x148](player, &popup state, &"PulldownMenuItemNN", true).
// (The game then calls player->vtable[0x1f0](player, &"PulldownMenuItemNN") for the current value (0x776bb0); here the
// player focuses the list header a frame after SetState anyway, so the cursor starts above item 1; the current value is
// marked lb_active.)
void prepare_pull(Cpu* c, uint64_t player, int t, const Row& r) {
    char st[32];
    snprintf(st, sizeof st, "PcPull%d_%d", t, r.slot);
    GStr sname{std::string(st)};
    for (size_t k = 1; k <= r.ch.size(); k++) {
        char n[32];
        snprintf(n, sizeof n, "PulldownMenuItem%02zu", k);
        GStr item{std::string(n)};
        call_guest_nested(c, vfn(player, 0x148), {player, sname.p(), item.p(), 1ull});
    }
}

// A slider moved: draw its dots and number at once (the game's own sliders do the same through these two player calls).
void draw_slider(Cpu* c, uint64_t player, const Row& r, int n) {
    for (int d = 1; d <= 10; d++) {
        char path[96];
        snprintf(path, sizeof path, "selector_parent_mc/selector_%02d_mc/selector_number_mc/number_dot_mc/dot_%02d_mc", r.slot, d);
        GStr p(path), l(d <= n ? "lb_on" : "lb_off");
        call_guest_nested(c, vfn(player, 0x90), {player, p.p(), l.p()});
    }
    char path[112];
    snprintf(path, sizeof path, "selector_parent_mc/selector_%02d_mc/selector_number_mc/dt_select_number_mc/dt_select_number", r.slot);
    GStr p(path), txt(std::to_string(n));
    alignas(8) uint64_t args[3] = {0, 0, 0};   // empty std::vector of format arguments
    call_guest_nested(c, vfn(player, 0xa8), {player, p.p(), txt.p(), (uint64_t)(uintptr_t)args});
}

const Row* find_row(int t, int slot) {
    if (t < 0 || t >= (int)g_tabs.size()) return nullptr;
    for (const Row& r : g_tabs[(size_t)t].rows) if (r.slot == slot) return &r;
    return nullptr;
}

// Draw a row's current value again (after Restore Default Settings), with the same player calls the game's own handler uses.
void redraw_row(Cpu* c, uint64_t player, const Row& r) {
    char sel[48];
    snprintf(sel, sizeof sel, "selector_parent_mc/selector_%02d_mc", r.slot);
    if (r.widget == 'n') { draw_slider(c, player, r, slider_of(r)); return; }
    std::string v = get_value(r);
    int cur = choice_of(r, v);
    if (r.widget == 'c') {
        for (int k = 1; k <= 2; k++) {
            char path[96];
            snprintf(path, sizeof path, "%s/selector_common_%02d_mc", sel, k);
            GStr p(path), l(k - 1 == cur ? "lb_decide" : "lb_unselect");
            call_guest_nested(c, vfn(player, 0x90), {player, p.p(), l.p()});
        }
        return;
    }
    std::string path = std::string(sel) + "/selector_pulldown_mc/dt_select_pulldown_mc/dt_select_pulldown";
    GStr p(path), txt(cur >= 0 ? r.ch[(size_t)cur].second : v);
    alignas(8) uint64_t args[3] = {0, 0, 0};
    call_guest_nested(c, vfn(player, 0xa8), {player, p.p(), txt.p(), (uint64_t)(uintptr_t)args});
}

alignas(8) uint8_t g_others[24];   // "Others": the state name handed to the game handler for B and the per-frame update
void init_names() { put_short((uint64_t)(uintptr_t)g_others, "Others"); }

}  // namespace

// The player switches states itself (option.lmb: tab elements and the pulldown row name their target state, popup items
// return to the previous one) and then sends event 5 with the new state; the game's handler answers with BuildPage and
// SetState. For a PC state the hook does that part.
extern "C" void rt_ui_opt_event(Cpu* c) {
    unsigned ev = (unsigned)c->x[1];
    if (!rows_ready()) return;
    std::string st = read_str(c->x[3]);
    bool tab = st.compare(0, 5, "PcTab") == 0, pull = st.compare(0, 6, "PcPull") == 0;
    if (!tab && !pull) {
        if (trace_on() && (ev == 5 || ev == 10)) trace("nativeopt: event %u state '%s' element '%s'", ev, st.c_str(), ev == 10 ? read_str(c->x[4]).c_str() : "");
        return;
    }
    static bool names = false;
    if (!names) { init_names(); names = true; }
    const uint64_t handler = c->x[0], player = c->x[2];
    std::string e = (ev == 8 || ev == 10) ? read_str(c->x[4]) : std::string();
    if (trace_on() && ev != 22) trace("nativeopt: event %u state '%s' element '%s'", ev, st.c_str(), e.c_str());
    if (ev == 22 || (tab && (ev == 11 || ev == 19))) { c->x[3] = (uint64_t)(uintptr_t)g_others; return; }   // B closes the screen as on "Other"
    int t = atoi(st.c_str() + (tab ? 5 : 6));
    if (t < 0 || t >= (int)g_tabs.size()) { c->x[1] = 7; return; }
    Saved s; memcpy(s.x, c->x, sizeof s.x);
    if (ev == 5) {   // entering a PC state: its page data
        if (tab) go_tab(c, handler, player, t);
        else if (const Row* r = find_row(t, atoi(st.c_str() + st.find('_') + 1))) open_pull(c, handler, player, t, *r);
    } else if (pull) {
        const Row* r = find_row(t, atoi(st.c_str() + st.find('_') + 1));
        if (r && ev == 10 && e.compare(0, 16, "PulldownMenuItem") == 0) {   // an item: set it; the list returns to the tab
            size_t k = (size_t)atoi(e.c_str() + 16);
            if (k >= 1 && k <= r->ch.size()) set_value(*r, r->ch[k - 1].first);
        }
    } else if (ev == 8 && e.compare(0, 3, "PcT") == 0) {   // a slider's knob or mute button got the focus
        int tt = 0, slot = 0, n = -1;
        char kind = 0;
        if (sscanf(e.c_str(), "PcT%dS%d%c_%d", &tt, &slot, &kind, &n) >= 3 && (kind == 'N' || kind == 'M')) {
            const Row* r = find_row(tt, slot);
            char key[24];
            snprintf(key, sizeof key, "%d_%d", tt, slot);
            bool landing = g_slider_landed != key;   // the cursor just arrived: not a change (never store what the widget starts at)
            g_slider_landed = key;
            if (r && r->widget == 'n' && !landing) {
                if (kind == 'M') n = 0;
                if (n >= 0 && n <= 10 && n != slider_of(*r)) { set_value(*r, std::to_string(n)); draw_slider(c, player, *r, n); }
            }
        } else {
            g_slider_landed.clear();
        }
    } else if (ev == 8) {
        g_slider_landed.clear();
    } else if (ev == 10) {
        int tt = 0, slot = 0, k = 0;
        char kind = 0;
        if (e == "Reset") {   // Restore Default Settings on a PC tab: this tab's rows only
            for (const Row& r : g_tabs[(size_t)t].rows) { reset_row(r); redraw_row(c, player, r); }
        } else if (sscanf(e.c_str(), "PcT%dS%d%c%d", &tt, &slot, &kind, &k) == 4 && kind == 'C') {
            const Row* r = find_row(tt, slot);
            if (r && k >= 1 && k <= (int)r->ch.size()) set_value(*r, r->ch[(size_t)k - 1].first);
        } else if (sscanf(e.c_str(), "PcT%dS%d%c", &tt, &slot, &kind) == 3 && kind == 'P') {   // a pulldown row: its popup opens next
            if (const Row* r = find_row(tt, slot)) prepare_pull(c, player, tt, *r);
        }
    }
    memcpy(c->x, s.x, sizeof s.x);
    c->x[1] = 7;   // the game's handler skips the event
}

extern "C" void rt_ui_opt_state(Cpu* c) {
    uint64_t desc = c->x[1];
    if (!desc || !rows_ready()) return;
    std::string state = read_str(desc);
    hle::input::script_event("options");   // scripted-input anchor: the Settings screen opened (host/routes/native_options_*.txt)
    const uint64_t* vec = (const uint64_t*)(desc + 0x90);
    if (trace_on()) {
        std::string names;
        for (uint64_t p = vec[0]; p && p < vec[1]; p += kElemSize) {
            names += " " + read_str(p);
            for (uint64_t o = 0x30; o < kElemSize; o += 8) if (*(const uint64_t*)(p + o)) { char b[40]; snprintf(b, sizeof b, "[+%llx=%llx]", (unsigned long long)o, (unsigned long long)*(const uint64_t*)(p + o)); names += b; }
        }
        trace("nativeopt: SetState '%s' elements:%s", state.c_str(), names.c_str());
    }
    bool game_tab = false;
    for (const char* n : kGameTabs) game_tab = game_tab || state == n;
    if (!game_tab) return;
    Saved s; memcpy(s.x, c->x, sizeof s.x);
    if (!g_have_tmpl) {
        for (uint64_t p = vec[0]; p && p < vec[1]; p += kElemSize) {   // keep an entry with short names (a tab's) as the template
            if ((*(const uint8_t*)p & 1) || (*(const uint8_t*)(p + 0x18) & 1)) continue;
            call_guest_nested(c, kElemCopy, {(uint64_t)(uintptr_t)g_tmpl, p});
            g_have_tmpl = true;
            trace("nativeopt: element template from '%s'", read_str(p).c_str());
            break;
        }
    }
    // R on "Other" opens the first PC tab, L on "Game" the last one
    if (!g_tabs.empty()) {
        if (state == "Others") set_target(desc, "Next", g_tabs.front().state);
        else if (state == "Game") set_target(desc, "Prev", g_tabs.back().state);
    }
    memcpy(c->x, s.x, sizeof s.x);
}
