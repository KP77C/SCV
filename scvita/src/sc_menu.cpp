// sc_menu.cpp — the controls menu, see sc_menu.h. Platform-free: it reads the
// physical pad, edits controls(), and draws itself into a 960x544 RGBA image
// that the presenter lays over the game.
#include "sc_menu.h"
#include "sc_controls.h"
#include "sc_font.h"
#include "sc_host.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sc {

namespace {

// ---- state -------------------------------------------------------------------

enum Page { PG_MAIN, PG_BUTTONS, PG_KEYS, PG_CURSOR, PG_SCROLL, PG_COUNT };
const char* kPageTitle[PG_COUNT] = { "Controls", "Button layout", "Keys", "Cursor and sticks", "Scrolling, grid and touch" };

enum Kind { K_PAGE, K_JOB, K_KEY, K_INT, K_CHOICE, K_RESET, K_CLOSE, K_BACK };
struct Row {
    Kind kind; std::string label; std::string help;
    int arg = 0;                       // page / job
    uint32_t* key = nullptr;           // K_KEY
    int* ival = nullptr; int lo = 0, hi = 0, step = 1; const char* unit = "";   // K_INT
    int* choice = nullptr; bool* flag = nullptr; std::vector<std::string> opts;  // K_CHOICE (int or bool)
};

bool g_open = false;
int g_page = PG_MAIN, g_sel = 0, g_sel_of[PG_COUNT];
int g_capture = -1;                    // job waiting for a button press
bool g_capture_armed = false;          // all buttons were let go since the capture started
uint64_t g_capture_us = 0;
bool g_confirm_reset = false;
std::string g_msg; uint64_t g_msg_until = 0;
uint64_t g_now = 0;
uint32_t g_prev_raw = 0, g_swallow = 0;
uint32_t g_rep_btn = 0; uint64_t g_rep_next = 0, g_rep_start = 0;
std::vector<uint32_t> g_img;
uint32_t g_seq = 0;
bool g_dirty = true;
bool g_changed = false;                // anything edited since the menu opened

void say(const std::string& m) { g_msg = m; g_msg_until = g_now + 3000000; g_dirty = true; }

const char* bname(int job) { return button_name(controls().button[job]); }

std::vector<Row> build(int page) {
    Controls& c = controls();
    std::vector<Row> v;
    auto add = [&](Kind k, const std::string& l, const std::string& h) { Row r; r.kind = k; r.label = l; r.help = h; v.push_back(r); return &v.back(); };
    const std::string mod = bname(JOB_MOD);
    switch (page) {
    case PG_MAIN:
        add(K_PAGE, "Button layout", "Choose which button does each job.")->arg = PG_BUTTONS;
        add(K_PAGE, "Keys", "Choose the keyboard key each button sends, alone and with " + mod + ".")->arg = PG_KEYS;
        add(K_PAGE, "Cursor and sticks", "Cursor speeds, which stick moves the cursor, stick feel.")->arg = PG_CURSOR;
        add(K_PAGE, "Scrolling, grid and touch", "Edge scrolling, map scroll stick, command grid, touch screen.")->arg = PG_SCROLL;
        add(K_RESET, g_confirm_reset ? "Press Cross again to reset everything" : "Reset everything to defaults",
            "Puts every button, key and option back to the original StarCraft Vita controls.");
        add(K_CLOSE, "Save and close", "Settings are saved to " + std::string(controls_path()) + ".");
        break;
    case PG_BUTTONS: {
        static const char* names[JOB_COUNT] = { "Left click", "Modifier", "Command grid", "Ctrl (save groups)", "Numbers (select groups)",
                                                "Fast cursor", "Start keys", "Select key", "Key button 1", "Key button 2", "Key button 3", "Key button 4" };
        const std::string help[JOB_COUNT] = {
            mod + " + this = right click. With Numbers or Ctrl held it sends 6.",
            "Hold for each button's second key, right click and edge scrolling. With Numbers or Ctrl held it sends 5.",
            "Opens the command card picker: aim with the cursor stick, " + std::string(bname(JOB_CLICK)) + " presses the button. " + mod + " + this sends its own key.",
            "Holds Ctrl. With a key button: Ctrl + 1-4 (save a group).",
            "With a key button: 1-4 (select a group).",
            "Makes the cursor faster (hold or toggle, see Cursor and sticks).",
            "Sends the Start keys (game menu, " + mod + ": Esc).",
            "Sends the Select key (Enter). " + mod + " + this opens the on-screen keyboard.",
            "Sends its keys (see Keys). With Numbers: 1, with Ctrl: Ctrl + 1.",
            "Sends its keys (see Keys). With Numbers: 2, with Ctrl: Ctrl + 2.",
            "Sends its keys (see Keys). With Numbers: 3, with Ctrl: Ctrl + 3.",
            "Sends its keys (see Keys). With Numbers: 4, with Ctrl: Ctrl + 4.",
        };
        for (int j = 0; j < JOB_COUNT; ++j) add(K_JOB, names[j], "Cross, then press the new button. " + help[j])->arg = j;
        break;
    }
    case PG_KEYS: {
        for (int i = 0; i < 4; ++i) {
            const std::string b = bname(JOB_KEY1 + i);
            add(K_KEY, b, "Key sent by " + b + ". Left/Right to change.")->key = &c.key[i];
            add(K_KEY, mod + " + " + b, "Key sent by " + mod + " + " + b + ".")->key = &c.key_mod[i];
        }
        add(K_KEY, bname(JOB_START), "Key sent by " + std::string(bname(JOB_START)) + " (F10 opens the game menu).")->key = &c.key_start;
        add(K_KEY, mod + " + " + bname(JOB_START), "Key sent by " + mod + " + " + bname(JOB_START) + ".")->key = &c.key_mod_start;
        add(K_KEY, bname(JOB_SELECT), "Key sent by " + std::string(bname(JOB_SELECT)) + ".")->key = &c.key_select;
        add(K_KEY, mod + " + " + bname(JOB_GRID), "Key sent by " + mod + " + " + bname(JOB_GRID) + " (Space jumps to the last alert).")->key = &c.key_mod_grid;
        break;
    }
    case PG_CURSOR: {
        Row* r = add(K_INT, "Cursor speed", "Normal cursor speed, in pixels per second at full stick.");
        r->ival = &c.cursor_speed; r->lo = 100; r->hi = 3000; r->step = 20; r->unit = "";
        r = add(K_INT, "Fast cursor speed", "Speed while " + std::string(bname(JOB_FAST)) + " is held (or toggled on).");
        r->ival = &c.fast_speed; r->lo = 100; r->hi = 4000; r->step = 20; r->unit = "";
        r = add(K_CHOICE, "Fast cursor", "Hold: fast while " + std::string(bname(JOB_FAST)) + " is held. Toggle: tap it to switch fast on and off.");
        r->flag = &c.fast_toggle; r->opts = { "Hold", "Toggle" };
        r = add(K_CHOICE, "Cursor stick", "The other stick scrolls the map.");
        r->flag = &c.cursor_left_stick; r->opts = { "Right stick", "Left stick" };
        r = add(K_CHOICE, "Stick response", "Smooth: slow near the centre for precise aiming. Linear: speed follows the stick evenly.");
        r->flag = &c.linear; r->opts = { "Smooth", "Linear" };
        r = add(K_CHOICE, "Invert cursor up/down", "Pushing the cursor stick up moves the cursor down.");
        r->flag = &c.invert_y; r->opts = { "Off", "On" };
        r = add(K_INT, "Stick dead zone", "How far a stick must move before it counts (both sticks).");
        r->ival = &c.deadzone_pct; r->lo = 0; r->hi = 50; r->step = 1; r->unit = "%";
        break;
    }
    case PG_SCROLL: {
        Row* r = add(K_CHOICE, "Edge scrolling", "Scroll the map by pushing the cursor against the screen edge.");
        r->choice = &c.edge; r->opts = { "On", "Hold " + mod, "Toggle (tap " + mod + ")", "Off" };
        r = add(K_INT, "Map scroll stick push", "How far to push the map-scroll stick before the map moves.");
        r->ival = &c.scroll_pct; r->lo = 10; r->hi = 90; r->step = 5; r->unit = "%";
        r = add(K_CHOICE, "Command grid", "Hold: the grid shows while " + std::string(bname(JOB_GRID)) + " is held. Toggle: tap to open, tap again to close.");
        r->flag = &c.grid_toggle; r->opts = { "Hold", "Toggle" };
        r = add(K_CHOICE, "Touch screen", "Move and click: a tap clicks, touch and move drags. Move only: the finger only moves the cursor.");
        r->choice = &c.touch; r->opts = { "Move and click", "Move only", "Off" };
        break;
    }
    }
    if (page != PG_MAIN) add(K_BACK, "Back", "Back to the main page.");
    return v;
}

std::string value_of(const Row& r) {
    const Controls& c = controls();
    char b[64];
    switch (r.kind) {
    case K_JOB: return button_name(c.button[r.arg]);
    case K_KEY: return key_name(*r.key);
    case K_INT: std::snprintf(b, sizeof b, "%d%s", *r.ival, r.unit); return b;
    case K_CHOICE: { int i = r.flag ? (*r.flag ? 1 : 0) : *r.choice; return i >= 0 && i < (int)r.opts.size() ? r.opts[i] : "?"; }
    case K_PAGE: return ">";
    default: return "";
    }
}

// ---- drawing ----------------------------------------------------------------------

constexpr uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }

void fill(int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < kMenuH; ++y)
        for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < kMenuW; ++x) g_img[y * kMenuW + x] = c;
}
int text_w(const std::string& s) {
    int w = 0;
    for (unsigned char ch : s) { if (ch < 32 || ch > 126) ch = '?'; w += font::kGlyphs[ch - 32].adv; }
    return w;
}
// Draws text with its top at y; blends glyph coverage over what is there.
void text(int x, int y, const std::string& s, int r, int g, int b, int max_x = kMenuW) {
    for (unsigned char ch : s) {
        if (ch < 32 || ch > 126) ch = '?';
        const font::Glyph& gl = font::kGlyphs[ch - 32];
        if (x + gl.adv > max_x) break;
        for (int yy = 0; yy < font::kH; ++yy) {
            const int py = y + yy; if (py < 0 || py >= kMenuH) continue;
            for (int xx = 0; xx < gl.w; ++xx) {
                const int px = x + gl.left + xx; if (px < 0 || px >= kMenuW) continue;
                const int a = font::kAtlas[yy * font::kAtlasW + gl.x + xx]; if (!a) continue;
                uint32_t& d = g_img[py * kMenuW + px];
                const int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF, da = d >> 24;
                d = rgba(dr + (r - dr) * a / 255, dg + (g - dg) * a / 255, db + (b - db) * a / 255, da > a ? da : a);
            }
        }
        x += gl.adv;
    }
}
// Wraps text into lines no wider than w.
std::vector<std::string> wrap(const std::string& s, int w) {
    std::vector<std::string> out; std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        const std::string t = line.empty() ? word : line + " " + word;
        if (text_w(t) > w && !line.empty()) { out.push_back(line); line = word; } else line = t;
        word.clear();
    };
    for (char ch : s) { if (ch == ' ') flush_word(); else word += ch; }
    flush_word(); if (!line.empty()) out.push_back(line);
    return out;
}

constexpr int kPX = 100, kPY = 8, kPW = 760, kPH = 528, kRowH = 26, kRowsY = 64;

void draw() {
    if (g_img.empty()) g_img.assign(kMenuW * kMenuH, 0);
    fill(0, 0, kMenuW, kMenuH, rgba(0, 0, 0, 150));                   // dim the game
    fill(kPX, kPY, kPW, kPH, rgba(14, 18, 28, 250));                    // panel
    fill(kPX, kPY, kPW, 44, rgba(24, 44, 86, 255));                     // title bar
    fill(kPX, kPY + 44, kPW, 2, rgba(70, 120, 210, 255));
    std::string title = g_page == PG_MAIN ? "StarCraft Vita controls" : std::string("Controls  >  ") + kPageTitle[g_page];
    text(kPX + 18, kPY + 10, title, 235, 240, 255);
    const std::vector<Row> rows = build(g_page);
    if (g_sel >= (int)rows.size()) g_sel = (int)rows.size() - 1;
    int gap = 0;
    for (int i = 0; i < (int)rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.kind == K_BACK || r.kind == K_RESET) gap += 8;   // small break before the action rows
        const int y = kRowsY + i * kRowH + gap;
        const bool sel = i == g_sel;
        if (sel) fill(kPX + 8, y - 1, kPW - 16, kRowH, rgba(40, 80, 150, 255));
        const bool capturing = sel && g_capture >= 0 && r.kind == K_JOB;
        text(kPX + 22, y + 1, r.label, sel ? 255 : 215, sel ? 255 : 220, sel ? 255 : 230);
        std::string v = capturing ? "press a button..." : value_of(r);
        if (sel && (r.kind == K_KEY || r.kind == K_INT || r.kind == K_CHOICE)) v = "<  " + v + "  >";
        if (!v.empty()) {
            const bool hot = capturing;
            text(kPX + kPW - 22 - text_w(v), y + 1, v, hot ? 255 : 255, hot ? 120 : 214, hot ? 90 : 110);
        }
    }
    // Help for the highlighted row, then the status message and the key guide.
    const int help_y = kPY + kPH - 108;
    fill(kPX + 8, help_y - 8, kPW - 16, 1, rgba(60, 70, 95, 255));
    if (g_sel >= 0 && g_sel < (int)rows.size()) {
        const std::vector<std::string> lines = wrap(g_capture >= 0 ? "Press the new button for this job. Any button works, the job it had moves to the old button. Wait 5 seconds to cancel." : rows[g_sel].help, kPW - 40);
        for (int i = 0; i < (int)lines.size() && i < 2; ++i) text(kPX + 20, help_y + i * 23, lines[i], 170, 185, 210);
    }
    if (!g_msg.empty() && g_now < g_msg_until) text(kPX + 20, help_y + 47, g_msg, 130, 230, 140);
    const std::string guide = "D-pad: move / change     Cross: select     Circle: back     Start: save and close";
    fill(kPX, kPY + kPH - 36, kPW, 36, rgba(20, 28, 46, 255));
    text(kPX + (kPW - text_w(guide)) / 2, kPY + kPH - 31, guide, 150, 165, 195);
    ++g_seq;
    g_dirty = false;
}

// ---- actions ----------------------------------------------------------------------

void close_menu() {
    g_open = false; g_capture = -1; g_confirm_reset = false;
    if (g_changed) controls_save();
    g_changed = false;
    log("[not-bound] controls menu closed");
}

void change(const Row& r, int dir, int mult) {
    Controls& c = controls();
    switch (r.kind) {
    case K_KEY: {
        const int n = key_choice_count(); int i = 0;
        for (; i < n; ++i) if (key_choice(i) == *r.key) break;
        if (i == n) i = 0;
        *r.key = key_choice(((i + dir) % n + n) % n);
        break;
    }
    case K_INT: {
        int v = *r.ival + dir * r.step * mult;
        v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
        *r.ival = v;
        break;
    }
    case K_CHOICE:
        if (r.flag) *r.flag = !*r.flag;
        else { const int n = (int)r.opts.size(); *r.choice = ((*r.choice + dir) % n + n) % n; }
        break;
    default: return;
    }
    (void)c;
    g_changed = true; g_dirty = true;
}

void activate(const Row& r) {
    switch (r.kind) {
    case K_PAGE: g_sel_of[g_page] = g_sel; g_page = r.arg; g_sel = g_sel_of[g_page]; break;
    case K_BACK: g_sel_of[g_page] = g_sel; g_page = PG_MAIN; g_sel = g_sel_of[PG_MAIN]; break;
    case K_JOB: g_capture = r.arg; g_capture_armed = false; g_capture_us = g_now; break;
    case K_KEY: case K_CHOICE: change(r, +1, 1); break;
    case K_RESET:
        if (!g_confirm_reset) { g_confirm_reset = true; break; }
        controls_defaults(controls()); g_confirm_reset = false; g_changed = true; say("Everything is back to the defaults.");
        break;
    case K_CLOSE: close_menu(); break;
    default: break;
    }
    g_dirty = true;
}

void capture_step(uint32_t raw, uint32_t pressed) {
    if (!g_capture_armed) { if (!(raw & 0xF3F9u)) g_capture_armed = true; }   // wait until Cross is let go
    else if (pressed) {
        uint32_t bit = 0;
        for (uint32_t m = 1; m; m <<= 1) if ((pressed & m) && (m & 0xF3F9u)) { bit = m; break; }
        if (bit) {
            Controls& c = controls();
            const int job = g_capture;
            const uint32_t old = c.button[job];
            for (int j = 0; j < JOB_COUNT; ++j) if (j != job && c.button[j] == bit) {
                c.button[j] = old;
                char m[160];
                static const char* jn[JOB_COUNT] = { "Left click", "Modifier", "Command grid", "Ctrl", "Numbers", "Fast cursor",
                                                     "Start keys", "Select key", "Key button 1", "Key button 2", "Key button 3", "Key button 4" };
                std::snprintf(m, sizeof m, "%s moved to %s.", jn[j], button_name(old));
                if (old != bit) say(m);
            }
            c.button[job] = bit;
            g_capture = -1; g_changed = true; g_dirty = true;
            g_swallow |= bit;      // its release must not act in the menu
            return;
        }
    }
    if (g_now - g_capture_us > 5000000) { g_capture = -1; say("No button pressed, nothing changed."); }
}

void menu_tick(uint32_t raw) {
    const uint32_t pressed = raw & ~g_prev_raw & ~g_swallow;
    g_swallow &= raw;
    if (g_capture >= 0) { capture_step(raw, pressed); return; }
    // Held D-pad repeats: first after 400 ms, then every 70 ms (numbers speed up after 1.5 s).
    uint32_t act = pressed;
    const uint32_t dirs = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
    if (pressed & dirs) { g_rep_btn = pressed & dirs; g_rep_start = g_now; g_rep_next = g_now + 400000; }
    else if (g_rep_btn && (raw & g_rep_btn) == g_rep_btn && g_now >= g_rep_next) { act |= g_rep_btn; g_rep_next = g_now + 70000; }
    else if (g_rep_btn && (raw & g_rep_btn) != g_rep_btn) g_rep_btn = 0;
    const int mult = g_rep_btn && g_now - g_rep_start > 1500000 ? 5 : 1;

    std::vector<Row> rows = build(g_page);
    const int n = (int)rows.size();
    if (act & (PAD_UP | PAD_DOWN)) {
        g_sel = (g_sel + ((act & PAD_DOWN) ? 1 : -1) + n) % n;
        g_confirm_reset = false; g_dirty = true;
    }
    if (act & (PAD_LEFT | PAD_RIGHT)) change(rows[g_sel], (act & PAD_RIGHT) ? 1 : -1, mult);
    if (pressed & PAD_CROSS) activate(rows[g_sel]);
    else if (pressed & PAD_CIRCLE) {
        if (g_page == PG_MAIN) close_menu();
        else { g_sel_of[g_page] = g_sel; g_page = PG_MAIN; g_sel = g_sel_of[PG_MAIN]; g_dirty = true; }
    } else if (pressed & PAD_START) close_menu();
}

}  // namespace

bool menu_is_open() { return g_open; }

const uint32_t* menu_image(uint32_t* seq) {
    if (!g_open) return nullptr;
    if (seq) *seq = g_seq;
    return g_img.empty() ? nullptr : g_img.data();
}

void input_frame(const Pad& phys, uint64_t dt_us) {
    static bool loaded = false;
    if (!loaded) { loaded = true; controls_load(); }
    g_now += dt_us;
    const uint32_t raw = phys.buttons;
    const uint32_t pressed = raw & ~g_prev_raw;
    if (!g_open && (pressed & PAD_START) && (raw & PAD_RIGHT)) {
        // D-pad right + Start: physical, whatever the layout says.
        g_open = true; g_page = PG_MAIN; g_sel = 0; g_changed = false; g_confirm_reset = false;
        std::memset(g_sel_of, 0, sizeof g_sel_of);
        g_swallow = raw;                          // Start/Right must not act inside the menu
        g_dirty = true;
        log("[not-bound] controls menu opened");
    } else if (g_open) {
        menu_tick(raw);
        if (!g_open) g_swallow = raw;             // closing buttons must not reach the game
    }
    g_prev_raw = raw;
    if (g_open) {
        if (g_dirty || (!g_msg.empty() && g_now >= g_msg_until && g_msg_until)) { if (g_now >= g_msg_until) g_msg_until = 0; draw(); }
        input_tick(Pad(), dt_us);                 // the game sees nothing held: every key and button lets go
        return;
    }
    Pad p = phys;
    g_swallow &= raw;
    p.buttons &= ~g_swallow;                      // held over from the menu: ignored until let go
    input_tick(p, dt_us);
}

}  // namespace sc
