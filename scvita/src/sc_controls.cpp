// sc_controls.cpp — control settings: the action list, defaults, presets, file I/O.
#include "sc_controls.h"
#include "sc_input.h"
#include "sc_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sc {

namespace {
constexpr uint16_t VK_SHIFT = 0x10, VK_CTRL = 0x11;

struct ActStore { std::string id, name; Action a; };
std::vector<ActStore>& store() {
    static std::vector<ActStore> v;
    if (!v.empty()) return v;
    auto add = [&](const std::string& id, const std::string& name, uint8_t kind, uint8_t grp, uint16_t vk = 0, uint16_t mod = 0) {
        ActStore s; s.id = id; s.name = name; s.a = { nullptr, nullptr, kind, grp, vk, mod }; v.push_back(s);
    };
    add("left_click", "Left click", ACT_LCLICK, GRP_MOUSE);
    add("right_click", "Right click", ACT_RCLICK, GRP_MOUSE);
    add("command_grid", "Command grid", ACT_GRID, GRP_PORT);
    add("fast_cursor", "Fast cursor", ACT_FAST, GRP_PORT);
    add("edge_scroll", "Edge scrolling button", ACT_EDGE, GRP_PORT);
    add("keyboard", "On-screen keyboard", ACT_IME, GRP_PORT);
    for (char c = 'A'; c <= 'Z'; ++c) add(std::string("key_") + (char)(c + 32), std::string(1, c), ACT_KEY, GRP_LETTERS, (uint16_t)c);
    const char digits[] = "1234567890";
    for (int i = 0; i < 10; ++i) add(std::string("key_") + digits[i], std::string(1, digits[i]), ACT_KEY, GRP_NUMBERS, (uint16_t)digits[i]);
    for (int i = 0; i < 10; ++i) add(std::string("ctrl_") + digits[i], std::string("Ctrl + ") + digits[i], ACT_KEY, GRP_NUMBERS, (uint16_t)digits[i], VK_CTRL);
    for (int i = 0; i < 10; ++i) add(std::string("shift_") + digits[i], std::string("Shift + ") + digits[i], ACT_KEY, GRP_NUMBERS, (uint16_t)digits[i], VK_SHIFT);
    const struct { const char* id; const char* name; uint16_t vk; } other[] = {
        { "space", "Space", 0x20 }, { "enter", "Enter", 0x0D }, { "esc", "Esc", 0x1B }, { "tab", "Tab", 0x09 },
        { "backspace", "Backspace", 0x08 }, { "delete", "Delete", 0x2E }, { "shift", "Shift (hold)", VK_SHIFT },
        { "ctrl", "Ctrl (hold)", VK_CTRL }, { "alt", "Alt (hold)", 0x12 }, { "home", "Home", 0x24 }, { "pause", "Pause", 0x13 },
        { "page_up", "Page Up", 0x21 }, { "page_down", "Page Down", 0x22 },
        { "up", "Arrow up", 0x26 }, { "down", "Arrow down", 0x28 }, { "left", "Arrow left", 0x25 }, { "right", "Arrow right", 0x27 },
        { "plus", "+ (game speed up)", 0xBB }, { "minus", "- (game speed down)", 0xBD },
        { "f1", "F1", 0x70 }, { "f2", "F2", 0x71 }, { "f3", "F3", 0x72 }, { "f4", "F4", 0x73 }, { "f5", "F5", 0x74 }, { "f6", "F6", 0x75 },
        { "f7", "F7", 0x76 }, { "f8", "F8", 0x77 }, { "f9", "F9", 0x78 }, { "f10", "F10 (game menu)", 0x79 }, { "f11", "F11", 0x7A }, { "f12", "F12", 0x7B },
    };
    for (auto& o : other) add(o.id, o.name, ACT_KEY, GRP_OTHER, o.vk);
    for (int i = 0; i < kMacros; ++i) add("macro_" + std::to_string(i + 1), "Macro " + std::to_string(i + 1) + " button", ACT_MACRO, GRP_MACRO, (uint16_t)i);
    for (auto& s : v) { s.a.id = s.id.c_str(); s.a.name = s.name.c_str(); }
    return v;
}

Controls g_c;
bool g_init = false;

const struct { uint32_t bit; const char* name; } kButtons[] = {
    { PAD_L, "L" }, { PAD_R, "R" }, { PAD_UP, "D-pad up" }, { PAD_DOWN, "D-pad down" }, { PAD_LEFT, "D-pad left" },
    { PAD_RIGHT, "D-pad right" }, { PAD_CROSS, "Cross" }, { PAD_CIRCLE, "Circle" }, { PAD_SQUARE, "Square" },
    { PAD_TRIANGLE, "Triangle" }, { PAD_START, "Start" }, { PAD_SELECT, "Select" },
};

void bind(Controls& c, const char* id, uint32_t b0, uint32_t b1 = 0) {
    const int i = action_find(id);
    if (i >= 0) { c.bind[i][0] = b0; c.bind[i][1] = b1; }
}
}  // namespace

int action_count() { return (int)store().size(); }
const Action& action(int i) { return store()[i].a; }
int action_find(const char* id) {
    auto& v = store();
    for (int i = 0; i < (int)v.size(); ++i) if (v[i].id == id) return i;
    return -1;
}

// K's scheme (build 21) expressed as bindings.
void controls_defaults(Controls& c) {
    std::memset(c.bind, 0, sizeof c.bind);
    bind(c, "left_click", PAD_R);
    bind(c, "right_click", PAD_L | PAD_R);
    bind(c, "command_grid", PAD_UP);
    bind(c, "fast_cursor", PAD_RIGHT);
    bind(c, "edge_scroll", PAD_L);
    bind(c, "keyboard", PAD_L | PAD_SELECT);
    bind(c, "key_a", PAD_CROSS, PAD_L | PAD_CROSS);
    bind(c, "key_s", PAD_SQUARE);
    bind(c, "key_h", PAD_L | PAD_SQUARE);
    bind(c, "key_r", PAD_TRIANGLE, PAD_L | PAD_CIRCLE);
    bind(c, "key_g", PAD_L | PAD_TRIANGLE);
    bind(c, "key_p", PAD_CIRCLE);
    const uint32_t six[6] = { PAD_CROSS, PAD_SQUARE, PAD_TRIANGLE, PAD_CIRCLE, PAD_L, PAD_R };
    for (int i = 0; i < 6; ++i) {
        char id[16];
        std::snprintf(id, sizeof id, "key_%d", i + 1);  bind(c, id, PAD_DOWN | six[i]);
        std::snprintf(id, sizeof id, "ctrl_%d", i + 1); bind(c, id, PAD_LEFT | six[i]);
    }
    bind(c, "ctrl", PAD_LEFT);
    bind(c, "space", PAD_L | PAD_UP);
    bind(c, "f10", PAD_START);
    bind(c, "esc", PAD_L | PAD_START);
    bind(c, "enter", PAD_SELECT);
    c.cursor_speed = 560; c.fast_speed = 1400; c.fast_toggle = false;
    c.cursor_left_stick = false; c.linear = false; c.invert_y = false;
    c.deadzone_pct = 18; c.scroll_pct = 35;
    c.edge = EDGE_HOLD; c.grid_toggle = false;
    c.touch = TOUCH_CLICK; c.touch_right = TR_OFF; c.long_ms = 500;
    c.rear = REAR_OFF; c.rear_speed_pct = 100;
    std::memset(c.macro, 0, sizeof c.macro);
    c.menu_pauses = true;
}

// Touch screen mouse (K, 2026-11-23): the finger moves and clicks (tap = left
// click, long press or two-finger tap = right click, drag = selection box),
// and the buttons are all keyboard. R is Shift (queue orders, add to a group),
// edge scrolling is always on.
void controls_preset_touch(Controls& c) {
    char keep[kMacros][kMacroLen]; std::memcpy(keep, c.macro, sizeof keep);
    controls_defaults(c);
    std::memcpy(c.macro, keep, sizeof keep);
    bind(c, "left_click", 0); bind(c, "right_click", 0); bind(c, "edge_scroll", 0);
    bind(c, "shift", PAD_R);
    bind(c, "key_6", PAD_DOWN | PAD_TRIANGLE | PAD_CIRCLE);   // R is Shift now; 6 moves off D-pad down + R
    bind(c, "ctrl_6", PAD_LEFT | PAD_TRIANGLE | PAD_CIRCLE);
    c.touch = TOUCH_CLICK; c.touch_right = TR_BOTH; c.edge = EDGE_ON;
}

Controls& controls() { if (!g_init) { controls_defaults(g_c); g_init = true; } return g_c; }

const char* button_name(uint32_t bit) {
    for (auto& b : kButtons) if (b.bit == bit) return b.name;
    return "?";
}
const char* chord_name(uint32_t m, char* out, int n) {
    out[0] = 0; int k = 0;
    if (!m) { std::snprintf(out, n, "-"); return out; }
    for (auto& b : kButtons) if ((m & b.bit) && k < n) k += std::snprintf(out + k, n - k, "%s%s", k ? " + " : "", b.name);
    return out;
}

const char* controls_path() {
#ifdef __vita__
    return "ux0:data/StarCraft/scvita_controls.txt";
#else
    const char* p = std::getenv("SC_CONTROLS");
    return p ? p : "scvita_controls.txt";
#endif
}

namespace {
std::string trim(std::string t) {
    while (!t.empty() && t[0] == ' ') t.erase(0, 1);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}
// "L + Cross" -> mask; "-" / "none" -> 0; false when a name is unknown.
bool parse_chord(const std::string& in, uint32_t* out) {
    *out = 0;
    const std::string s = trim(in);
    if (s == "-" || s == "none" || s.empty()) return true;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find('+', p); if (q == std::string::npos) q = s.size();
        const std::string t = trim(s.substr(p, q - p));
        uint32_t bit = 0;
        for (auto& b : kButtons) if (t == b.name) bit = b.bit;
        if (!bit) return false;
        *out |= bit;
        p = q + 1;
    }
    return true;
}
struct Opt { const char* key; int* v; bool* b; int lo, hi; const char* const* names; int n; };
const char* kEdgeId[EDGE_COUNT] = { "on", "hold", "toggle", "off" };
const char* kTouchId[TOUCH_COUNT] = { "click", "move", "off" };
const char* kTrId[TR_COUNT] = { "off", "long_press", "two_finger", "both" };
const char* kRearId[REAR_COUNT] = { "off", "cursor", "scroll" };
const char* kHoldId[2] = { "hold", "toggle" };
const char* kStickId[2] = { "right", "left" };
const char* kRespId[2] = { "smooth", "linear" };
const char* kNoYes[2] = { "no", "yes" };
std::vector<Opt> opts(Controls& c) {
    return {
        { "cursor_speed", &c.cursor_speed, nullptr, 100, 4000, nullptr, 0 },
        { "fast_speed", &c.fast_speed, nullptr, 100, 4000, nullptr, 0 },
        { "fast_cursor", nullptr, &c.fast_toggle, 0, 1, kHoldId, 2 },
        { "cursor_stick", nullptr, &c.cursor_left_stick, 0, 1, kStickId, 2 },
        { "stick_response", nullptr, &c.linear, 0, 1, kRespId, 2 },
        { "invert_y", nullptr, &c.invert_y, 0, 1, kNoYes, 2 },
        { "deadzone", &c.deadzone_pct, nullptr, 0, 50, nullptr, 0 },
        { "scroll_push", &c.scroll_pct, nullptr, 10, 90, nullptr, 0 },
        { "edge_scroll", &c.edge, nullptr, 0, EDGE_COUNT - 1, kEdgeId, EDGE_COUNT },
        { "grid", nullptr, &c.grid_toggle, 0, 1, kHoldId, 2 },
        { "touch", &c.touch, nullptr, 0, TOUCH_COUNT - 1, kTouchId, TOUCH_COUNT },
        { "touch_right_click", &c.touch_right, nullptr, 0, TR_COUNT - 1, kTrId, TR_COUNT },
        { "long_press_ms", &c.long_ms, nullptr, 200, 1500, nullptr, 0 },
        { "rear_touch", &c.rear, nullptr, 0, REAR_COUNT - 1, kRearId, REAR_COUNT },
        { "rear_speed", &c.rear_speed_pct, nullptr, 20, 400, nullptr, 0 },
        { "menu_opens_game_menu", nullptr, &c.menu_pauses, 0, 1, kNoYes, 2 },
    };
}
}  // namespace

bool controls_load() {
    Controls& c = controls();
    FILE* f = std::fopen(controls_path(), "r");
    if (!f) { log("[not-bound] controls: %s not found, defaults", controls_path()); return false; }
    Controls n = c;
    std::vector<Opt> ov = opts(n);
    char line[256];
    int bad = 0;
    bool old_format = false;
    while (std::fgets(line, sizeof line, f)) {
        char* nl = std::strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char* eq = std::strchr(line, '='); if (!eq) { ++bad; continue; }
        *eq = 0; const std::string k = line, v = eq + 1;
        bool ok = false;
        if (k.compare(0, 5, "bind.") == 0) {
            const int a = action_find(k.c_str() + 5);
            if (a >= 0) {
                const size_t comma = v.find(',');
                uint32_t m0 = 0, m1 = 0;
                ok = parse_chord(v.substr(0, comma), &m0) && (comma == std::string::npos || parse_chord(v.substr(comma + 1), &m1));
                if (ok) { n.bind[a][0] = m0; n.bind[a][1] = m1; }
            }
        } else if (k.compare(0, 6, "macro.") == 0) {
            const int i = std::atoi(k.c_str() + 6) - 1;
            if (i >= 0 && i < kMacros) { std::snprintf(n.macro[i], kMacroLen, "%s", v.c_str()); ok = true; }
        } else if (k.compare(0, 7, "button.") == 0 || k.compare(0, 4, "key.") == 0) {
            old_format = true; ok = true;          // first menu build's layout: ignored, defaults kept
        } else {
            for (Opt& o : ov) {
                if (k != o.key) continue;
                if (o.names) { for (int i = 0; i < o.n; ++i) if (v == o.names[i]) { if (o.v) *o.v = i; else *o.b = i != 0; ok = true; } }
                else { const int x = std::atoi(v.c_str()); if (x >= o.lo && x <= o.hi) { *o.v = x; ok = true; } }
            }
        }
        if (!ok) ++bad;
    }
    std::fclose(f);
    c = n;
    log("[not-bound] controls: loaded %s (%d bad lines ignored%s)", controls_path(), bad, old_format ? ", old button layout ignored" : "");
    return true;
}

bool controls_save() {
    Controls& c = controls();
    FILE* f = std::fopen(controls_path(), "w");
    if (!f) { log("[not-bound] [cause: cannot write %s] controls not saved", controls_path()); return false; }
    std::fprintf(f, "# StarCraft Vita controls. Written by the controls menu (L + D-pad right + Start).\n"
                    "# bind.<action>=<buttons>[, <buttons>]  e.g. bind.key_a=Cross, L + Cross   (- = unbound)\n"
                    "# Delete this file to go back to the default controls.\n");
    for (int i = 0; i < action_count(); ++i) {
        char a[96], b[96];
        chord_name(c.bind[i][0], a, sizeof a); chord_name(c.bind[i][1], b, sizeof b);
        if (c.bind[i][1]) std::fprintf(f, "bind.%s=%s, %s\n", action(i).id, a, b);
        else std::fprintf(f, "bind.%s=%s\n", action(i).id, a);
    }
    for (const Opt& o : opts(c)) {
        if (o.names) std::fprintf(f, "%s=%s\n", o.key, o.names[o.v ? *o.v : (*o.b ? 1 : 0)]);
        else std::fprintf(f, "%s=%d\n", o.key, *o.v);
    }
    for (int i = 0; i < kMacros; ++i) if (c.macro[i][0]) std::fprintf(f, "macro.%d=%s\n", i + 1, c.macro[i]);
    const bool ok = std::fclose(f) == 0;
    log("[not-bound] controls: saved %s%s", controls_path(), ok ? "" : " (close failed)");
    return ok;
}

}  // namespace sc
