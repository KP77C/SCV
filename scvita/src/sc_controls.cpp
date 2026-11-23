// sc_controls.cpp — control settings: defaults, layout remap, file I/O.
#include "sc_controls.h"
#include "sc_input.h"
#include "sc_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sc {

namespace {
Controls g_c;
bool g_init = false;

constexpr uint32_t kLogical[JOB_COUNT] = {
    PAD_R, PAD_L, PAD_UP, PAD_LEFT, PAD_DOWN, PAD_RIGHT, PAD_START, PAD_SELECT,
    PAD_CROSS, PAD_SQUARE, PAD_TRIANGLE, PAD_CIRCLE,
};
const char* kJobId[JOB_COUNT] = {
    "click", "modifier", "grid", "ctrl", "numbers", "fast", "start", "select", "key1", "key2", "key3", "key4",
};
const struct { uint32_t bit; const char* name; } kButtons[] = {
    { PAD_CROSS, "Cross" }, { PAD_CIRCLE, "Circle" }, { PAD_SQUARE, "Square" }, { PAD_TRIANGLE, "Triangle" },
    { PAD_L, "L" }, { PAD_R, "R" }, { PAD_UP, "D-pad up" }, { PAD_DOWN, "D-pad down" },
    { PAD_LEFT, "D-pad left" }, { PAD_RIGHT, "D-pad right" }, { PAD_START, "Start" }, { PAD_SELECT, "Select" },
};

// Keys offered in the menu. Letters and digits are their own VK codes.
const struct { uint32_t vk; const char* name; } kNamedKeys[] = {
    { 0, "None" }, { 0x20, "Space" }, { 0x0D, "Enter" }, { 0x1B, "Esc" }, { 0x09, "Tab" }, { 0x08, "Backspace" },
    { 0x2E, "Delete" }, { 0x10, "Shift" }, { 0x11, "Ctrl" }, { 0x24, "Home" }, { 0x13, "Pause" },
    { 0x70, "F1" }, { 0x71, "F2" }, { 0x72, "F3" }, { 0x73, "F4" }, { 0x74, "F5" }, { 0x75, "F6" },
    { 0x76, "F7" }, { 0x77, "F8" }, { 0x78, "F9" }, { 0x79, "F10" }, { 0x7A, "F11" }, { 0x7B, "F12" },
    { 0xBB, "+" }, { 0xBD, "-" },
};
constexpr int kNamed = sizeof kNamedKeys / sizeof kNamedKeys[0];
}  // namespace

void controls_defaults(Controls& c) {
    for (int j = 0; j < JOB_COUNT; ++j) c.button[j] = kLogical[j];
    const uint32_t k[4] = { 'A', 'S', 'R', 'P' }, km[4] = { 'A', 'H', 'G', 'R' };
    for (int i = 0; i < 4; ++i) { c.key[i] = k[i]; c.key_mod[i] = km[i]; }
    c.key_start = 0x79; c.key_mod_start = 0x1B; c.key_select = 0x0D; c.key_mod_grid = 0x20;
    c.cursor_speed = 560; c.fast_speed = 1400;
    c.cursor_left_stick = false; c.deadzone_pct = 18;
    c.edge = EDGE_HOLD; c.touch = TOUCH_CLICK;
    c.fast_toggle = false; c.grid_toggle = false; c.linear = false; c.invert_y = false; c.scroll_pct = 35;
}

Controls& controls() { if (!g_init) { controls_defaults(g_c); g_init = true; } return g_c; }

uint32_t job_logical(int job) { return kLogical[job]; }

uint32_t controls_remap(uint32_t phys) {
    const Controls& c = controls();
    uint32_t out = 0;
    for (int j = 0; j < JOB_COUNT; ++j) if (phys & c.button[j]) out |= kLogical[j];
    return out;
}

const char* button_name(uint32_t bit) {
    for (auto& b : kButtons) if (b.bit == bit) return b.name;
    return "?";
}
const char* key_name(uint32_t vk) {
    static char one[2];
    for (auto& k : kNamedKeys) if (k.vk == vk) return k.name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { one[0] = (char)vk; one[1] = 0; return one; }
    return "?";
}
int key_choice_count() { return kNamed + 26 + 10; }
uint32_t key_choice(int i) {
    if (i < 26) return 'A' + i;
    if (i < 36) return '0' + (i - 26);
    return kNamedKeys[i - 36].vk;
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
uint32_t parse_button(const char* v) {
    for (auto& b : kButtons) if (!std::strcmp(b.name, v)) return b.bit;
    return 0;
}
bool parse_key(const char* v, uint32_t* out) {
    for (int i = 0; i < key_choice_count(); ++i) if (!std::strcmp(key_name(key_choice(i)), v)) { *out = key_choice(i); return true; }
    return false;
}
const char* kEdgeId[EDGE_COUNT] = { "on", "hold", "toggle", "off" };
const char* kTouchId[TOUCH_COUNT] = { "click", "move", "off" };
}  // namespace

bool controls_load() {
    Controls& c = controls();
    FILE* f = std::fopen(controls_path(), "r");
    if (!f) { log("[not-bound] controls: %s not found, defaults", controls_path()); return false; }
    Controls n = c;
    char line[160];
    int bad = 0;
    while (std::fgets(line, sizeof line, f)) {
        char* nl = std::strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char* eq = std::strchr(line, '='); if (!eq) { ++bad; continue; }
        *eq = 0; const char* k = line; const char* v = eq + 1;
        bool ok = false;
        for (int j = 0; j < JOB_COUNT && !ok; ++j) {
            char id[32]; std::snprintf(id, sizeof id, "button.%s", kJobId[j]);
            if (!std::strcmp(k, id)) { uint32_t b = parse_button(v); if (b) { n.button[j] = b; ok = true; } }
        }
        for (int i = 0; i < 4 && !ok; ++i) {
            char id[32];
            std::snprintf(id, sizeof id, "key.key%d", i + 1);     if (!std::strcmp(k, id)) ok = parse_key(v, &n.key[i]);
            std::snprintf(id, sizeof id, "key.mod_key%d", i + 1); if (!ok && !std::strcmp(k, id)) ok = parse_key(v, &n.key_mod[i]);
        }
        if (ok) continue;
        if (!std::strcmp(k, "key.start")) ok = parse_key(v, &n.key_start);
        else if (!std::strcmp(k, "key.mod_start")) ok = parse_key(v, &n.key_mod_start);
        else if (!std::strcmp(k, "key.select")) ok = parse_key(v, &n.key_select);
        else if (!std::strcmp(k, "key.mod_grid")) ok = parse_key(v, &n.key_mod_grid);
        else if (!std::strcmp(k, "cursor_speed")) { n.cursor_speed = std::atoi(v); ok = n.cursor_speed >= 100 && n.cursor_speed <= 4000; }
        else if (!std::strcmp(k, "fast_speed")) { n.fast_speed = std::atoi(v); ok = n.fast_speed >= 100 && n.fast_speed <= 4000; }
        else if (!std::strcmp(k, "cursor_stick")) { ok = !std::strcmp(v, "left") || !std::strcmp(v, "right"); n.cursor_left_stick = !std::strcmp(v, "left"); }
        else if (!std::strcmp(k, "deadzone")) { n.deadzone_pct = std::atoi(v); ok = n.deadzone_pct >= 0 && n.deadzone_pct <= 50; }
        else if (!std::strcmp(k, "edge_scroll")) { for (int e = 0; e < EDGE_COUNT; ++e) if (!std::strcmp(v, kEdgeId[e])) { n.edge = e; ok = true; } }
        else if (!std::strcmp(k, "touch")) { for (int t = 0; t < TOUCH_COUNT; ++t) if (!std::strcmp(v, kTouchId[t])) { n.touch = t; ok = true; } }
        else if (!std::strcmp(k, "fast_cursor")) { ok = !std::strcmp(v, "hold") || !std::strcmp(v, "toggle"); n.fast_toggle = !std::strcmp(v, "toggle"); }
        else if (!std::strcmp(k, "grid")) { ok = !std::strcmp(v, "hold") || !std::strcmp(v, "toggle"); n.grid_toggle = !std::strcmp(v, "toggle"); }
        else if (!std::strcmp(k, "stick_response")) { ok = !std::strcmp(v, "smooth") || !std::strcmp(v, "linear"); n.linear = !std::strcmp(v, "linear"); }
        else if (!std::strcmp(k, "invert_y")) { ok = !std::strcmp(v, "yes") || !std::strcmp(v, "no"); n.invert_y = !std::strcmp(v, "yes"); }
        else if (!std::strcmp(k, "scroll_push")) { n.scroll_pct = std::atoi(v); ok = n.scroll_pct >= 10 && n.scroll_pct <= 90; }
        if (!ok) ++bad;
    }
    std::fclose(f);
    // The layout must use every button exactly once, or some job would be lost.
    uint32_t seen = 0; bool perm = true;
    for (int j = 0; j < JOB_COUNT; ++j) { if (seen & n.button[j]) perm = false; seen |= n.button[j]; }
    if (!perm) { log("[not-bound] controls: button layout uses a button twice, layout reset"); for (int j = 0; j < JOB_COUNT; ++j) n.button[j] = kLogical[j]; }
    // Out-of-range numbers fall back to their defaults.
    Controls d; controls_defaults(d);
    if (n.cursor_speed < 100 || n.cursor_speed > 4000) n.cursor_speed = d.cursor_speed;
    if (n.fast_speed < 100 || n.fast_speed > 4000) n.fast_speed = d.fast_speed;
    if (n.deadzone_pct < 0 || n.deadzone_pct > 50) n.deadzone_pct = d.deadzone_pct;
    if (n.scroll_pct < 10 || n.scroll_pct > 90) n.scroll_pct = d.scroll_pct;
    c = n;
    log("[not-bound] controls: loaded %s (%d bad lines ignored)", controls_path(), bad);
    return true;
}

bool controls_save() {
    const Controls& c = controls();
    FILE* f = std::fopen(controls_path(), "w");
    if (!f) { log("[not-bound] [cause: cannot write %s] controls not saved", controls_path()); return false; }
    std::fprintf(f, "# StarCraft Vita controls. Written by the controls menu (D-pad right + Start).\n"
                    "# Delete this file to go back to the default controls.\n");
    for (int j = 0; j < JOB_COUNT; ++j) std::fprintf(f, "button.%s=%s\n", kJobId[j], button_name(c.button[j]));
    // key_name() reuses one buffer for letters: one call per printf.
    for (int i = 0; i < 4; ++i) {
        std::fprintf(f, "key.key%d=%s\n", i + 1, key_name(c.key[i]));
        std::fprintf(f, "key.mod_key%d=%s\n", i + 1, key_name(c.key_mod[i]));
    }
    std::fprintf(f, "key.start=%s\n", key_name(c.key_start));
    std::fprintf(f, "key.mod_start=%s\n", key_name(c.key_mod_start));
    std::fprintf(f, "key.select=%s\n", key_name(c.key_select));
    std::fprintf(f, "key.mod_grid=%s\n", key_name(c.key_mod_grid));
    std::fprintf(f, "cursor_speed=%d\nfast_speed=%d\ncursor_stick=%s\ndeadzone=%d\nedge_scroll=%s\ntouch=%s\n",
                 c.cursor_speed, c.fast_speed, c.cursor_left_stick ? "left" : "right", c.deadzone_pct, kEdgeId[c.edge], kTouchId[c.touch]);
    std::fprintf(f, "fast_cursor=%s\ngrid=%s\nstick_response=%s\ninvert_y=%s\nscroll_push=%d\n", c.fast_toggle ? "toggle" : "hold",
                 c.grid_toggle ? "toggle" : "hold", c.linear ? "linear" : "smooth", c.invert_y ? "yes" : "no", c.scroll_pct);
    const bool ok = std::fclose(f) == 0;
    log("[not-bound] controls: saved %s%s", controls_path(), ok ? "" : " (close failed)");
    return ok;
}

}  // namespace sc
