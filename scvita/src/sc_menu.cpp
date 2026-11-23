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

constexpr uint32_t kAllButtons = 0xF3F9u;
constexpr uint32_t kMenuCombo = PAD_L | PAD_RIGHT | PAD_START;

// ---- state -------------------------------------------------------------------

enum Page { PG_MAIN, PG_PC, PG_PORT, PG_CURSOR, PG_TOUCH, PG_MACRO, PG_COUNT };
const char* kPageTitle[PG_COUNT] = { "Controls", "PC controls", "Port features", "Cursor and sticks", "Touch screen", "Cheat macros" };

enum Kind { K_HEADER, K_PAGE, K_BIND, K_INT, K_CHOICE, K_MACROTEXT, K_MACRORUN, K_PRESET, K_RESET, K_CLOSE, K_BACK };
struct Row {
    Kind kind; std::string label; std::string help;
    int arg = 0;                       // page / action
    int* ival = nullptr; int lo = 0, hi = 0, step = 1; const char* unit = "";
    int* choice = nullptr; bool* flag = nullptr; std::vector<std::string> opts;
};

bool g_open = false;
int g_page = PG_MAIN, g_sel = 0, g_top = 0, g_sel_of[PG_COUNT];
int g_rec = -1, g_rec_slot = 0;        // action being recorded, and which of its two bindings
bool g_rec_armed = false;              // all buttons were let go since recording started
uint32_t g_rec_mask = 0;               // buttons held so far in this recording
uint64_t g_rec_us = 0;
int g_confirm = 0;                     // 1 = reset, 2 = touch preset: waiting for a second Cross
std::string g_msg; uint64_t g_msg_until = 0;
uint64_t g_now = 0;
uint32_t g_prev_raw = 0, g_swallow = 0;
uint32_t g_rep_btn = 0; uint64_t g_rep_next = 0, g_rep_start = 0;
std::vector<uint32_t> g_img;
uint32_t g_seq = 0;
bool g_dirty = true;
bool g_changed = false;
int g_text_for = -1;                   // macro slot waiting for keyboard text

void say(const std::string& m) { g_msg = m; g_msg_until = g_now + 3500000; g_dirty = true; }

std::string chord(uint32_t m) { char b[96]; return chord_name(m, b, sizeof b); }
std::string binds_of(int a) {
    const Controls& c = controls();
    if (!c.bind[a][0] && !c.bind[a][1]) return "-";
    std::string s;
    for (int k = 0; k < 2; ++k) if (c.bind[a][k]) { if (!s.empty()) s += ",  "; s += chord(c.bind[a][k]); }
    return s;
}
// A binding is a conflict when another action uses exactly the same buttons,
// or it is the menu's own L + D-pad right + Start.
bool conflict(int a, int slot) {
    const Controls& c = controls();
    const uint32_t m = c.bind[a][slot];
    if (!m) return false;
    if (m == kMenuCombo) return true;
    for (int b = 0; b < action_count(); ++b)
        for (int s = 0; s < 2; ++s) if (b != a && c.bind[b][s] == m) return true;
    return false;
}
std::string conflict_with(int a) {
    const Controls& c = controls();
    for (int s = 0; s < 2; ++s) {
        const uint32_t m = c.bind[a][s]; if (!m) continue;
        if (m == kMenuCombo) return chord(m) + " opens this menu, so it never reaches the game.";
        for (int b = 0; b < action_count(); ++b) if (b != a && (c.bind[b][0] == m || c.bind[b][1] == m))
            return chord(m) + " is also bound to " + action(b).name + ". Only one of them will work.";
    }
    return "";
}

std::vector<Row> build(int page) {
    Controls& c = controls();
    std::vector<Row> v;
    auto add = [&](Kind k, const std::string& l, const std::string& h) { Row r; r.kind = k; r.label = l; r.help = h; v.push_back(r); return &v.back(); };
    const std::string bind_help = "Cross: record buttons.  Triangle: record a second binding.  Square: clear.";
    auto add_bind = [&](int a, const std::string& extra) {
        Row* r = add(K_BIND, action(a).name, extra.empty() ? bind_help : extra + " " + bind_help); r->arg = a;
    };
    switch (page) {
    case PG_MAIN:
        add(K_PAGE, "PC controls", "Record a button or combination for every PC key and mouse click.")->arg = PG_PC;
        add(K_PAGE, "Port features", "Command grid, fast cursor, edge scrolling and on-screen keyboard: buttons and options.")->arg = PG_PORT;
        add(K_PAGE, "Cursor and sticks", "Cursor speeds, which stick moves the cursor, stick feel, map scroll stick.")->arg = PG_CURSOR;
        add(K_PAGE, "Touch screen", "Front touch clicks and right clicks, back touchpad.")->arg = PG_TOUCH;
        add(K_PAGE, "Cheat macros", "Buttons that type a cheat code (or any chat text) for you: Enter, the text, Enter.")->arg = PG_MACRO;
        add(K_PRESET, g_confirm == 2 ? "Press Cross again to load the touch preset" : "Load preset: touch screen mouse",
            "Touch moves and clicks (long press or two fingers = right click), the buttons are all keyboard, R = Shift.");
        add(K_RESET, g_confirm == 1 ? "Press Cross again to reset everything" : "Reset everything to defaults",
            "Puts every binding and option back to the original StarCraft Vita controls.");
        add(K_CLOSE, "Save and close", "Settings are saved to " + std::string(controls_path()) + ".");
        break;
    case PG_PC: {
        static const char* heads[GRP_COUNT] = { "", "Mouse", "Letter keys", "Number keys and groups", "Other keys", "" };
        for (int g = GRP_MOUSE; g <= GRP_OTHER; ++g) {
            add(K_HEADER, heads[g], "");
            for (int a = 0; a < action_count(); ++a) if (action(a).group == g) add_bind(a, "");
        }
        break;
    }
    case PG_PORT: {
        add(K_HEADER, "Command grid", "");
        add_bind(action_find("command_grid"), "Shows the 3x3 command card picker; aim with the cursor stick, left click presses the button.");
        Row* r = add(K_CHOICE, "Command grid mode", "Hold: shown while held. Toggle: press to open, press again to close.");
        r->flag = &c.grid_toggle; r->opts = { "Hold", "Toggle" };
        add(K_HEADER, "Fast cursor", "");
        add_bind(action_find("fast_cursor"), "Makes the cursor faster.");
        r = add(K_CHOICE, "Fast cursor mode", "Hold: fast while held. Toggle: press to switch fast on and off.");
        r->flag = &c.fast_toggle; r->opts = { "Hold", "Toggle" };
        r = add(K_INT, "Fast cursor speed", "Speed with fast cursor on, in pixels per second at full stick.");
        r->ival = &c.fast_speed; r->lo = 100; r->hi = 4000; r->step = 20;
        add(K_HEADER, "Edge scrolling", "");
        r = add(K_CHOICE, "Edge scrolling", "Scroll the map by pushing the cursor against the screen edge. Hold / Toggle use the button below (toggle: a quick tap on its own).");
        r->choice = &c.edge; r->opts = { "On", "Hold button", "Toggle (tap button)", "Off" };
        add_bind(action_find("edge_scroll"), "The button for Hold / Toggle edge scrolling.");
        add(K_HEADER, "Controls menu", "");
        r = add(K_CHOICE, "When this menu opens", "Open game menu: also presses F10, so a single-player game pauses behind this menu. Keep playing: the game keeps running.");
        r->flag = &c.menu_pauses; r->opts = { "Keep playing", "Open game menu (pause)" };
        add(K_HEADER, "On-screen keyboard", "");
        add_bind(action_find("keyboard"), "Opens the Vita keyboard to type (chat, cheats). Enter is not sent: press your Enter binding.");
        break;
    }
    case PG_CURSOR: {
        Row* r = add(K_INT, "Cursor speed", "Normal cursor speed, in pixels per second at full stick.");
        r->ival = &c.cursor_speed; r->lo = 100; r->hi = 3000; r->step = 20;
        r = add(K_INT, "Fast cursor speed", "Speed with fast cursor on (see Port features).");
        r->ival = &c.fast_speed; r->lo = 100; r->hi = 4000; r->step = 20;
        r = add(K_CHOICE, "Cursor stick", "The other stick scrolls the map.");
        r->flag = &c.cursor_left_stick; r->opts = { "Right stick", "Left stick" };
        r = add(K_CHOICE, "Stick response", "Smooth: slow near the centre for precise aiming. Linear: speed follows the stick evenly.");
        r->flag = &c.linear; r->opts = { "Smooth", "Linear" };
        r = add(K_CHOICE, "Invert cursor up/down", "Pushing the cursor stick up moves the cursor down.");
        r->flag = &c.invert_y; r->opts = { "Off", "On" };
        r = add(K_INT, "Stick dead zone", "How far a stick must move before it counts (both sticks).");
        r->ival = &c.deadzone_pct; r->lo = 0; r->hi = 50; r->step = 1; r->unit = "%";
        r = add(K_INT, "Map scroll stick push", "How far to push the map-scroll stick before the map moves.");
        r->ival = &c.scroll_pct; r->lo = 10; r->hi = 90; r->step = 5; r->unit = "%";
        break;
    }
    case PG_MACRO: {
        for (int i = 0; i < kMacros; ++i) {
            const std::string n = std::to_string(i + 1);
            add(K_HEADER, "Macro " + n, "");
            add(K_MACROTEXT, "Macro " + n + " text", "Cross: type the text on the Vita keyboard.  Square: clear. Cheats work in single player.")->arg = i;
            char id[16]; std::snprintf(id, sizeof id, "macro_%d", i + 1);
            add_bind(action_find(id), "The button that sends macro " + n + ".");
            add(K_MACRORUN, "Use macro " + n + " now", "Cross: close this menu (and the game menu) and send the macro.")->arg = i;
        }
        break;
    }
    case PG_TOUCH: {
        Row* r = add(K_CHOICE, "Front touch screen", "Move and click: the cursor follows the finger and a tap clicks, touch and move drags a box. Move only: no clicks.");
        r->choice = &c.touch; r->opts = { "Move and click", "Move only", "Off" };
        r = add(K_CHOICE, "Touch right click", "Long press: hold the finger still. Two fingers: tap a second finger while the first is down.");
        r->choice = &c.touch_right; r->opts = { "Off", "Long press", "Two-finger tap", "Both" };
        r = add(K_INT, "Long press time", "How long to hold still for a right click. With long press on, a click lands when the finger lifts.");
        r->ival = &c.long_ms; r->lo = 200; r->hi = 1500; r->step = 50; r->unit = " ms";
        r = add(K_CHOICE, "Back touchpad", "Cursor: slide a finger on the back to move the cursor like a laptop trackpad. Scroll map: drag to scroll.");
        r->choice = &c.rear; r->opts = { "Off", "Cursor", "Scroll map" };
        r = add(K_INT, "Back touchpad speed", "Cursor speed on the back touchpad.");
        r->ival = &c.rear_speed_pct; r->lo = 20; r->hi = 400; r->step = 10; r->unit = "%";
        break;
    }
    }
    if (page != PG_MAIN) add(K_BACK, "Back", "Back to the main page.");
    return v;
}

std::string value_of(const Row& r) {
    char b[64];
    switch (r.kind) {
    case K_BIND: return binds_of(r.arg);
    case K_INT: std::snprintf(b, sizeof b, "%d%s", *r.ival, r.unit); return b;
    case K_CHOICE: { int i = r.flag ? (*r.flag ? 1 : 0) : *r.choice; return i >= 0 && i < (int)r.opts.size() ? r.opts[i] : "?"; }
    case K_PAGE: return ">";
    case K_MACRORUN: return controls().macro[r.arg][0] ? "Cross" : "(no text)";
    case K_MACROTEXT: { const char* t = controls().macro[r.arg]; return t[0] ? std::string("\"") + t + "\"" : "(empty)"; }
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

constexpr int kPX = 100, kPY = 8, kPW = 760, kPH = 528, kRowH = 26, kRowsY = 62;
constexpr int kHelpY = kPY + kPH - 108;
constexpr int kVisible = (kHelpY - 14 - kRowsY) / kRowH;   // rows that fit above the help text

void keep_visible(int n) {
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kVisible) g_top = g_sel - kVisible + 1;
    if (g_top > n - kVisible) g_top = n - kVisible;
    if (g_top < 0) g_top = 0;
    // Show the section header above the first row of a section.
    if (g_top > 0 && g_top == g_sel) { std::vector<Row> rows = build(g_page); if (rows[g_top - 1].kind == K_HEADER) --g_top; }
}

void draw() {
    if (g_img.empty()) g_img.assign(kMenuW * kMenuH, 0);
    fill(0, 0, kMenuW, kMenuH, rgba(0, 0, 0, 150));                   // dim the game
    fill(kPX, kPY, kPW, kPH, rgba(14, 18, 28, 250));                    // panel
    fill(kPX, kPY, kPW, 44, rgba(24, 44, 86, 255));                     // title bar
    fill(kPX, kPY + 44, kPW, 2, rgba(70, 120, 210, 255));
    std::string title = g_page == PG_MAIN ? "StarCraft Vita controls" : std::string("Controls  >  ") + kPageTitle[g_page];
    text(kPX + 18, kPY + 10, title, 235, 240, 255);
    const std::vector<Row> rows = build(g_page);
    const int n = (int)rows.size();
    if (g_sel >= n) g_sel = n - 1;
    keep_visible(n);
    for (int i = g_top; i < n && i < g_top + kVisible; ++i) {
        const Row& r = rows[i];
        const int y = kRowsY + (i - g_top) * kRowH;
        const bool sel = i == g_sel;
        if (r.kind == K_HEADER) { text(kPX + 16, y + 1, r.label, 120, 170, 255); fill(kPX + 16 + text_w(r.label) + 10, y + 14, kPW - 52 - text_w(r.label), 1, rgba(50, 70, 110, 255)); continue; }
        if (sel) fill(kPX + 8, y - 1, kPW - 16, kRowH, rgba(40, 80, 150, 255));
        const bool rec = sel && g_rec >= 0 && r.kind == K_BIND;
        const bool bad = r.kind == K_BIND && (conflict(r.arg, 0) || conflict(r.arg, 1));
        const int lx = kPX + 22 + ((r.kind == K_BIND || r.kind == K_INT || r.kind == K_CHOICE) && g_page != PG_MAIN ? 8 : 0);
        text(lx, y + 1, r.label, bad ? 255 : sel ? 255 : 215, bad ? 110 : sel ? 255 : 220, bad ? 110 : sel ? 255 : 230);
        std::string v = rec ? (g_rec_mask ? chord(g_rec_mask) + " ..." : (g_rec_slot ? "second binding: press..." : "press buttons...")) : value_of(r);
        if (sel && (r.kind == K_INT || r.kind == K_CHOICE)) v = "<  " + v + "  >";
        if (!v.empty()) {
            int vr = 255, vg = 214, vb = 110;
            if (rec) { vr = 255; vg = 150; vb = 90; } else if (bad) { vr = 255; vg = 90; vb = 90; }
            text(kPX + kPW - 22 - text_w(v), y + 1, v, vr, vg, vb);
        }
    }
    if (g_top > 0) text(kPX + kPW / 2 - 6, kRowsY - 18, "^", 120, 150, 200);
    if (g_top + kVisible < n) text(kPX + kPW / 2 - 6, kRowsY + kVisible * kRowH - 6, "v", 120, 150, 200);
    // Help for the highlighted row, then the status message and the key guide.
    fill(kPX + 8, kHelpY - 8, kPW - 16, 1, rgba(60, 70, 95, 255));
    if (g_sel >= 0 && g_sel < n) {
        std::string h = rows[g_sel].help;
        if (g_rec >= 0) h = "Hold the button or buttons you want (for example L and Cross together), then let go. Wait 6 seconds to cancel.";
        else if (rows[g_sel].kind == K_BIND) { const std::string cw = conflict_with(rows[g_sel].arg); if (!cw.empty()) h = cw; }
        const bool red = g_rec < 0 && rows[g_sel].kind == K_BIND && !conflict_with(rows[g_sel].arg).empty();
        const std::vector<std::string> lines = wrap(h, kPW - 40);
        for (int i = 0; i < (int)lines.size() && i < 2; ++i) text(kPX + 20, kHelpY + i * 23, lines[i], red ? 255 : 170, red ? 120 : 185, red ? 120 : 210);
    }
    if (!g_msg.empty() && g_now < g_msg_until) text(kPX + 20, kHelpY + 47, g_msg, 130, 230, 140);
    const std::string guide = "D-pad: move / change     Cross: select     Circle: back     Start: save and close";
    fill(kPX, kPY + kPH - 36, kPW, 36, rgba(20, 28, 46, 255));
    text(kPX + (kPW - text_w(guide)) / 2, kPY + kPH - 31, guide, 150, 165, 195);
    ++g_seq;
    g_dirty = false;
}

// ---- actions ----------------------------------------------------------------------

void close_menu() {
    g_open = false; g_rec = -1; g_confirm = 0;
    if (g_changed) controls_save();
    g_changed = false;
    log("[not-bound] controls menu closed");
}

void change(const Row& r, int dir, int mult) {
    switch (r.kind) {
    case K_INT: {
        int v = *r.ival + dir * r.step * mult;
        *r.ival = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
        break;
    }
    case K_CHOICE:
        if (r.flag) *r.flag = !*r.flag;
        else { const int n = (int)r.opts.size(); *r.choice = ((*r.choice + dir) % n + n) % n; }
        break;
    default: return;
    }
    g_changed = true; g_dirty = true;
}

void go(int page) { g_sel_of[g_page] = g_sel; g_page = page; g_sel = g_sel_of[page]; g_top = 0; g_confirm = 0; }

int first_selectable(const std::vector<Row>& rows, int from, int dir) {
    const int n = (int)rows.size();
    for (int k = 0, i = from; k < n; ++k, i = (i + dir + n) % n) if (rows[i].kind != K_HEADER) return i;
    return 0;
}

void activate(const Row& r, uint32_t pressed) {
    if (r.kind == K_MACROTEXT) {
        if (pressed & PAD_SQUARE) { controls().macro[r.arg][0] = 0; g_changed = true; say("Macro text cleared."); }
        else if (pressed & PAD_CROSS) { g_text_for = r.arg; input_request_ime(); }
        g_dirty = true;
        return;
    }
    if (r.kind == K_MACRORUN) {
        if (!(pressed & PAD_CROSS)) return;
        if (!controls().macro[r.arg][0]) { say("Type a text for this macro first."); g_dirty = true; return; }
        const bool paused = controls().menu_pauses;
        close_menu();
        if (paused) input_tap_key(0x1B);          // Esc: close the game menu we opened, so chat can open
        input_run_macro(r.arg, paused ? 10 : 2);  // a few frames later, once the game menu is gone
        return;
    }
    if (r.kind == K_BIND) {
        if (pressed & PAD_SQUARE) {
            controls().bind[r.arg][0] = controls().bind[r.arg][1] = 0; g_changed = true;
            say(std::string(action(r.arg).name) + " is now unbound.");
        } else if (pressed & (PAD_CROSS | PAD_TRIANGLE)) {
            g_rec = r.arg; g_rec_slot = (pressed & PAD_TRIANGLE) ? 1 : 0; g_rec_armed = false; g_rec_mask = 0; g_rec_us = g_now;
        }
        g_dirty = true;
        return;
    }
    if (!(pressed & PAD_CROSS)) return;
    switch (r.kind) {
    case K_PAGE: go(r.arg); break;
    case K_BACK: go(PG_MAIN); break;
    case K_CHOICE: change(r, +1, 1); break;
    case K_RESET:
        if (g_confirm != 1) { g_confirm = 1; break; }
        controls_defaults(controls()); g_confirm = 0; g_changed = true; say("Everything is back to the defaults.");
        break;
    case K_PRESET:
        if (g_confirm != 2) { g_confirm = 2; break; }
        controls_preset_touch(controls()); g_confirm = 0; g_changed = true; say("Touch screen mouse preset loaded.");
        break;
    case K_CLOSE: close_menu(); break;
    default: break;
    }
    g_dirty = true;
}

void record_step(uint32_t raw) {
    if (!g_rec_armed) { if (!(raw & kAllButtons)) { g_rec_armed = true; g_rec_us = g_now; } return; }   // wait until Cross is let go
    const uint32_t held = raw & kAllButtons;
    if (held & ~g_rec_mask) { g_rec_mask |= held; g_dirty = true; }
    if (!held && g_rec_mask) {
        Controls& c = controls();
        uint32_t* slot = &c.bind[g_rec][g_rec_slot];
        if (g_rec_slot == 1 && !c.bind[g_rec][0]) slot = &c.bind[g_rec][0];   // no first binding yet: fill that
        *slot = g_rec_mask;
        if (c.bind[g_rec][0] == c.bind[g_rec][1]) c.bind[g_rec][1] = 0;
        g_changed = true;
        const std::string cw = conflict_with(g_rec);
        say(std::string(action(g_rec).name) + ": " + chord(g_rec_mask) + (cw.empty() ? "." : "  (shared, see the red text)"));
        g_rec = -1; g_dirty = true;
        return;
    }
    if (!g_rec_mask && g_now - g_rec_us > 6000000) { g_rec = -1; say("Nothing pressed, nothing changed."); }
}

void menu_tick(uint32_t raw) {
    const uint32_t pressed = raw & ~g_prev_raw & ~g_swallow;
    g_swallow &= raw;
    if (g_rec >= 0) { record_step(raw); return; }
    // Held D-pad repeats: first after 400 ms, then every 70 ms (numbers speed up after 1.5 s).
    uint32_t act = pressed;
    const uint32_t dirs = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
    if (pressed & dirs) { g_rep_btn = pressed & dirs; g_rep_start = g_now; g_rep_next = g_now + 400000; }
    else if (g_rep_btn && (raw & g_rep_btn) == g_rep_btn && g_now >= g_rep_next) { act |= g_rep_btn; g_rep_next = g_now + 70000; }
    else if (g_rep_btn && (raw & g_rep_btn) != g_rep_btn) g_rep_btn = 0;
    const int mult = g_rep_btn && g_now - g_rep_start > 1500000 ? 5 : 1;

    std::vector<Row> rows = build(g_page);
    const int n = (int)rows.size();
    if (rows[g_sel].kind == K_HEADER) g_sel = first_selectable(rows, g_sel, +1);
    if (act & (PAD_UP | PAD_DOWN)) {
        const int dir = (act & PAD_DOWN) ? 1 : -1;
        g_sel = first_selectable(rows, (g_sel + dir + n) % n, dir);
        g_confirm = 0; g_dirty = true;
    }
    if (act & (PAD_LEFT | PAD_RIGHT)) change(rows[g_sel], (act & PAD_RIGHT) ? 1 : -1, mult);
    if (pressed & (PAD_CROSS | PAD_TRIANGLE | PAD_SQUARE)) activate(rows[g_sel], pressed);
    else if (pressed & PAD_CIRCLE) {
        if (g_page == PG_MAIN) close_menu();
        else { go(PG_MAIN); g_dirty = true; }
    } else if (pressed & PAD_START) close_menu();
}

}  // namespace

bool menu_is_open() { return g_open; }
bool menu_wants_text() { return g_open && g_text_for >= 0; }
void menu_take_text(const std::string& s) {
    if (g_text_for < 0) return;
    if (s.empty()) say("Nothing typed, text unchanged.");    // keyboard cancelled
    else {
        std::snprintf(controls().macro[g_text_for], kMacroLen, "%s", s.c_str());
        say("Macro " + std::to_string(g_text_for + 1) + " text saved.");
        g_changed = true;
    }
    g_text_for = -1; g_dirty = true;
}

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
    if (!g_open && (pressed & PAD_START) && (raw & (PAD_L | PAD_RIGHT)) == (PAD_L | PAD_RIGHT)) {
        // L + D-pad right + Start: physical, whatever the bindings say.
        g_open = true; g_page = PG_MAIN; g_sel = 0; g_top = 0; g_changed = false; g_confirm = 0; g_rec = -1;
        std::memset(g_sel_of, 0, sizeof g_sel_of);
        g_swallow = raw;                          // Start/Right must not act inside the menu
        g_dirty = true;
        if (controls().menu_pauses) input_tap_key(0x79);   // F10: the game's own menu, which pauses single player
        log("[not-bound] controls menu opened");
    } else if (g_open) {
        menu_tick(raw);
        if (!g_open) g_swallow = raw;             // closing buttons must not reach the game
    }
    g_prev_raw = raw;
    if (g_open) {
        if (g_dirty || (g_msg_until && g_now >= g_msg_until)) { if (g_now >= g_msg_until) g_msg_until = 0; draw(); }
        input_tick(Pad(), dt_us);                 // the game sees nothing held: every key and button lets go
        return;
    }
    Pad p = phys;
    g_swallow &= raw;
    p.buttons &= ~g_swallow;                      // held over from the menu: ignored until let go
    input_tick(p, dt_us);
}

}  // namespace sc
