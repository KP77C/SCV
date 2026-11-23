// sc_input.cpp — Vita controls -> the game's keyboard and mouse.
//
// Every button press goes through the player's bindings (sc_controls.h):
// each PC key, Ctrl/Shift + number and mouse click, and each port feature
// (command grid, fast cursor, edge scrolling, on-screen keyboard) is an
// action bound to a button or a combination of buttons. The defaults are
// K's scheme (build 21):
//
//   Right stick   cursor (D-pad right held: faster; L held: edge scrolling)
//   Touch screen  cursor under the finger; tap = left click, touch and move = drag
//   Left stick    map scroll (arrow keys)
//   R             left click        L+R      right click (double R = double click)
//   Cross         A                 L+Cross  A
//   Triangle      R                 L+Tri    G
//   Square        S                 L+Sq     H
//   Circle        P                 L+Circle R
//   D-pad left    Ctrl (held)       L + D-pad up  Space
//   D-pad left + face/L/R  Ctrl+1-6 (Cross 1, Square 2, Triangle 3, Circle 4, L 5, R 6)
//   D-pad down + face/L/R  1-6
//   D-pad up      hold: 3x3 command grid, the cursor stick picks the cell,
//                 left click presses it (grid stays up until D-pad up is released)
//   Select        Enter             L+Select on-screen keyboard (no Enter)
//   Start         F10               L+Start Escape
//
// The controls menu (D-pad right + Start, sc_menu.cpp) changes all of it.
#include <cstdlib>
#include "sc_input.h"
#include "sc_host.h"
#include "sc_controls.h"

#include "runtime/win32_shims_window.h"

#include <cmath>

namespace sc {

namespace {
constexpr uint32_t WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_MOUSEMOVE = 0x0200,
                   WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205;
constexpr uint32_t VK_RETURN = 0x0D, VK_CONTROL = 0x11, VK_SPACE = 0x20, VK_LEFT = 0x25, VK_UP = 0x26,
                   VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F10 = 0x79, VK_ESCAPE = 0x1B;

uint8_t  g_down[256];            // key-state table (1 = down)
uint8_t  g_ref[256];             // how many inputs hold each key (bindings, the scroll stick, ...)
uint32_t g_prev = 0;             // previous buttons
bool     g_lbtn = false, g_rbtn = false;
int      g_lref = 0, g_rref = 0; // holders of the mouse buttons
bool     g_grid = false; int g_cell = 4;
bool     g_ime = false;
constexpr double kEdgeGuard = 2;   // px kept from the screen edge when edge scrolling is off
bool     g_stick_hold = false;   // after a grid click, ignore the cursor stick until it recentres
double   g_fx = 320, g_fy = 240; // sub-pixel cursor
bool     g_fast_held = false, g_fast_on = false;
bool     g_edge_held = false, g_edge_on = false;

// Centres of the 3x3 command card at 640x480, row-major. Rows 0-1 measured on
// Zeratul's card in Protoss mission 1; row 2 follows the same spacing.
constexpr int kCardX0 = 522, kCardDX = 46, kCardY0 = 376, kCardDY = 39;

uint32_t scan(uint32_t vk) {
    switch (vk) {
    case VK_RETURN: return 0x1C; case VK_CONTROL: return 0x1D; case VK_SPACE: return 0x39;
    case VK_LEFT: return 0x4B; case VK_RIGHT: return 0x4D; case VK_UP: return 0x48; case VK_DOWN: return 0x50;
    case VK_F10: return 0x44; case VK_ESCAPE: return 0x01;
    case 0x09: return 0x0F; case 0x08: return 0x0E; case 0x2E: return 0x53; case 0x10: return 0x2A;
    case 0x12: return 0x38; case 0x21: return 0x49; case 0x22: return 0x51;
    case 0x24: return 0x47; case 0x13: return 0x45; case 0x7A: return 0x57; case 0x7B: return 0x58;
    case 0xBB: return 0x0D; case 0xBD: return 0x0C; case '0': return 0x0B;
    default: {
        if (vk >= 0x70 && vk <= 0x78) return 0x3B + (vk - 0x70);   // F1-F9
        static const char* row = "QWERTYUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0ZXCVBNM";
        if (vk >= '1' && vk <= '9') return 2 + (vk - '1');
        for (int i = 0; i < 38; ++i) if (row[i] && (uint32_t)row[i] == vk) return 0x10 + i;
        return 0;
    }
    }
}
void key(uint32_t vk, bool down) {
    if (!!g_down[vk] == down) return;
    g_down[vk] = down;
    uint32_t lp = 1u | (scan(vk) << 16) | (down ? 0u : 0xC0000000u);
    static const bool keylog = std::getenv("SC_KEYLOG") != nullptr;
    if (keylog) log("[keylog] key 0x%x %s", vk, down ? "down" : "up");
    post_input(down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}
// Counted holds: a key goes up only when the last input holding it lets go
// (Ctrl held by D-pad left stays down through a Ctrl+1 binding).
void hold_key(uint32_t vk, bool down) {
    vk &= 0xFF;
    if (down) { if (g_ref[vk]++ == 0) key(vk, true); }
    else if (g_ref[vk] && --g_ref[vk] == 0) key(vk, false);
}

uint32_t mk() {
    return (g_lbtn ? 1u : 0u) | (g_rbtn ? 2u : 0u) | (g_down[0x10] ? 4u : 0u) | (g_down[VK_CONTROL] ? 8u : 0u);
}
uint32_t xy() { uint32_t x, y; wx86_get_cursor(&x, &y); return (y << 16) | (x & 0xFFFF); }
void mouse(uint32_t msg) { post_input(msg, mk(), xy()); }
void move_to(int x, int y) {
    x = x < 0 ? 0 : x > 639 ? 639 : x; y = y < 0 ? 0 : y > 479 ? 479 : y;
    uint32_t ox, oy; wx86_get_cursor(&ox, &oy);
    if ((int)ox == x && (int)oy == y) return;
    wx86_set_cursor((uint32_t)x, (uint32_t)y);
    mouse(WM_MOUSEMOVE);
}
// VK_LBUTTON/VK_RBUTTON in the key-state table too: GetAsyncKeyState(VK_LBUTTON) is how
// Win32 code polls the mouse buttons.
// Double click (K, build 17): a second left press within 500 ms and 4 px of
// the first is sent as WM_LBUTTONDBLCLK, as Windows does for a window class
// with CS_DBLCLKS. StarCraft handles 0x203 itself: double-clicking a unit
// selects every unit of that type on screen.
uint64_t g_clock_us = 0, g_last_ldown_us = 0;
int      g_last_ldown_x = -100, g_last_ldown_y = -100;
void lbutton(bool down) {
    if (g_lbtn == down) return;
    g_lbtn = down; g_down[1] = down;
    if (!down) { mouse(WM_LBUTTONUP); return; }
    uint32_t x, y; wx86_get_cursor(&x, &y);
    const bool dbl = g_last_ldown_us && g_clock_us - g_last_ldown_us <= 500000 &&
                     std::abs((int)x - g_last_ldown_x) <= 4 && std::abs((int)y - g_last_ldown_y) <= 4;
    mouse(dbl ? 0x0203u /*WM_LBUTTONDBLCLK*/ : WM_LBUTTONDOWN);
    g_last_ldown_us = dbl ? 0 : (g_clock_us ? g_clock_us : 1);
    g_last_ldown_x = (int)x; g_last_ldown_y = (int)y;
}
void rbutton(bool down) { if (g_rbtn != down) { g_rbtn = down; g_down[2] = down; mouse(down ? WM_RBUTTONDOWN : WM_RBUTTONUP); } }
void hold_left(bool down) { if (down) { if (g_lref++ == 0) lbutton(true); } else if (g_lref && --g_lref == 0) lbutton(false); }
void hold_right(bool down) { if (down) { if (g_rref++ == 0) rbutton(true); } else if (g_rref && --g_rref == 0) rbutton(false); }

// Stick deflection in -1..1 with a dead zone.
double axis(uint8_t v) {
    double d = (int)v - 128; d /= 127.0;
    const double dz = controls().deadzone_pct / 100.0;
    if (std::fabs(d) < dz) return 0;
    return (d > 0 ? d - dz : d + dz) / (1 - dz);
}
int grid_cell(const Pad& p) {
    double x = axis(p.rx), y = axis(p.ry);
    int col = x < -0.5 ? 0 : x > 0.5 ? 2 : 1, row = y < -0.5 ? 0 : y > 0.5 ? 2 : 1;
    return row * 3 + col;
}
void grid_click() {
    uint32_t ox, oy; wx86_get_cursor(&ox, &oy);
    move_to(kCardX0 + kCardDX * (g_cell % 3), kCardY0 + kCardDY * (g_cell / 3));
    lbutton(true); lbutton(false);
    move_to((int)ox, (int)oy);
    g_fx = ox; g_fy = oy;
    g_stick_hold = true;
}
int popcount(uint32_t v) { int n = 0; while (v) { v &= v - 1; ++n; } return n; }

// ---- bindings ------------------------------------------------------------------

struct Active { int act; uint32_t chord; bool grid_click; };
Active g_active[24]; int g_nactive = 0;
// Edge-scroll toggle by a tap: the button pressed and let go within 350 ms
// with no other button pressed meanwhile, so combos using it never flip it.
bool g_edge_tap = false; uint64_t g_edge_tap_us = 0;

bool is_active(int a) { for (int i = 0; i < g_nactive; ++i) if (g_active[i].act == a) return true; return false; }

void activate(int a, uint32_t chord) {
    const Controls& c = controls();
    const Action& ac = action(a);
    Active st = { a, chord, false };
    switch (ac.kind) {
    case ACT_KEY: if (ac.mod) hold_key(ac.mod, true); hold_key(ac.vk, true); break;
    case ACT_LCLICK: if (g_grid) { grid_click(); st.grid_click = true; } else hold_left(true); break;
    case ACT_RCLICK: hold_right(true); break;
    case ACT_FAST: if (c.fast_toggle) g_fast_on = !g_fast_on; else g_fast_held = true; break;
    case ACT_EDGE:
        if (c.edge == EDGE_TOGGLE) { g_edge_tap = true; g_edge_tap_us = g_clock_us; } else g_edge_held = true;
        break;
    case ACT_GRID:
        if (c.grid_toggle) g_grid = !g_grid; else g_grid = true;
        if (g_grid) g_cell = 4;
        break;
    case ACT_IME: g_ime = true; break;
    }
    if (g_nactive < 24) g_active[g_nactive++] = st;
}
void deactivate(const Active& st) {
    const Controls& c = controls();
    const Action& ac = action(st.act);
    switch (ac.kind) {
    case ACT_KEY: hold_key(ac.vk, false); if (ac.mod) hold_key(ac.mod, false); break;
    case ACT_LCLICK: if (!st.grid_click) hold_left(false); break;
    case ACT_RCLICK: hold_right(false); break;
    case ACT_FAST: g_fast_held = false; break;
    case ACT_EDGE:
        g_edge_held = false;
        if (c.edge == EDGE_TOGGLE && g_edge_tap && g_clock_us - g_edge_tap_us <= 350000) {
            g_edge_on = !g_edge_on;
            log("[not-bound] controls: edge scrolling %s", g_edge_on ? "on" : "off");
        }
        g_edge_tap = false;
        break;
    case ACT_GRID: if (!c.grid_toggle) g_grid = false; break;
    default: break;
    }
}

void bindings_step(uint32_t b, uint32_t pressed, uint32_t released) {
    const Controls& c = controls();
    // Let go first: an action ends when any button of its binding is released.
    for (int i = 0; i < g_nactive;) {
        if (g_active[i].chord & released) { const Active st = g_active[i]; g_active[i] = g_active[--g_nactive]; deactivate(st); }
        else ++i;
    }
    if (pressed) g_edge_tap = false;                 // any new press cancels a pending edge tap
    // Each newly pressed button: the binding with the most buttons that uses
    // it and is fully held wins (first in the list on a tie).
    for (uint32_t bit = 1; bit && bit <= pressed; bit <<= 1) {
        if (!(pressed & bit)) continue;
        int best = -1, best_n = 0; uint32_t best_chord = 0;
        for (int a = 0; a < action_count(); ++a)
            for (int s = 0; s < 2; ++s) {
                const uint32_t ch = c.bind[a][s];
                if (!ch || !(ch & bit) || (ch & ~b)) continue;
                const int n = popcount(ch);
                if (n > best_n) { best = a; best_n = n; best_chord = ch; }
            }
        if (best >= 0 && !is_active(best)) activate(best, best_chord);
    }
}

// ---- touch ------------------------------------------------------------------------

enum TouchState { T_IDLE, T_PENDING, T_DRAG, T_DONE };
TouchState g_ts = T_IDLE;
int g_t0x = 0, g_t0y = 0; uint64_t g_t0_us = 0;
bool g_touch_left = false;           // the finger holds the left button
bool g_prev_touch2 = false;
bool g_rear_prev = false; int g_rear_lx = 0, g_rear_ly = 0, g_rear_ox = 0, g_rear_oy = 0;
uint32_t g_rear_keys = 0;            // arrow keys held by the back touchpad (bit 0..3 = left right up down)

void touch_right_click() { rbutton(true); rbutton(false); }

void touch_step(Pad& p, double guard) {
    const Controls& c = controls();
    if (c.touch == TOUCH_OFF) p.touch = p.touch2 = false;
    const bool clicks = c.touch == TOUCH_CLICK;
    const bool long_rc = clicks && (c.touch_right == TR_LONG || c.touch_right == TR_BOTH);
    const bool two_rc = clicks && (c.touch_right == TR_TWO || c.touch_right == TR_BOTH);
    if (!g_grid && p.touch) {
        double x = p.tx, y = p.ty;
        x = x < guard ? guard : x > 639 - guard ? 639 - guard : x; y = y < guard ? guard : y > 479 - guard ? 479 - guard : y;
        if ((int)x != (int)g_fx || (int)y != (int)g_fy) move_to((int)x, (int)y);
        g_fx = x; g_fy = y;
        if (g_ts == T_IDLE) {
            g_t0x = p.tx; g_t0y = p.ty; g_t0_us = g_clock_us;
            // Without long-press right click the button goes down at once (build 20 behaviour).
            if (clicks && !long_rc && !g_lbtn) { lbutton(true); g_touch_left = true; g_ts = T_DRAG; }
            else g_ts = clicks ? T_PENDING : T_DONE;
        } else if (g_ts == T_PENDING) {
            const int dx = p.tx - g_t0x, dy = p.ty - g_t0y;
            if (dx * dx + dy * dy > 8 * 8) {                      // moved: a drag, from where it started
                move_to(g_t0x, g_t0y); lbutton(true); g_touch_left = true; move_to((int)x, (int)y); g_ts = T_DRAG;
            } else if (g_clock_us - g_t0_us >= (uint64_t)c.long_ms * 1000) {
                touch_right_click(); g_ts = T_DONE;                // held still: right click
            }
        }
        if (two_rc && p.touch2 && !g_prev_touch2) {                 // second finger: right click at the first
            if (g_touch_left) { lbutton(false); g_touch_left = false; }
            touch_right_click(); g_ts = T_DONE;
        }
    }
    if (!p.touch && g_ts != T_IDLE) {
        if (g_ts == T_PENDING) { lbutton(true); lbutton(false); }  // quick tap
        if (g_touch_left) { lbutton(false); g_touch_left = false; }
        g_ts = T_IDLE;
    }
    g_prev_touch2 = p.touch && p.touch2;

    // Back touchpad: a trackpad for the cursor, or drag to scroll the map.
    if (c.rear == REAR_CURSOR && p.rear) {
        if (g_rear_prev) {
            const double k = c.rear_speed_pct / 100.0 * 640.0 / 725.0;
            g_fx += (p.rtx - g_rear_lx) * k; g_fy += (p.rty - g_rear_ly) * k;
            g_fx = g_fx < guard ? guard : g_fx > 639 - guard ? 639 - guard : g_fx;
            g_fy = g_fy < guard ? guard : g_fy > 479 - guard ? 479 - guard : g_fy;
            move_to((int)g_fx, (int)g_fy);
        }
    }
    uint32_t want = 0;
    if (c.rear == REAR_SCROLL && p.rear) {
        if (!g_rear_prev) { g_rear_ox = p.rtx; g_rear_oy = p.rty; }
        const int dx = p.rtx - g_rear_ox, dy = p.rty - g_rear_oy;
        if (dx < -30) want |= 1; if (dx > 30) want |= 2; if (dy < -30) want |= 4; if (dy > 30) want |= 8;
    }
    const uint32_t vks[4] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN };
    for (int i = 0; i < 4; ++i) if ((want ^ g_rear_keys) & (1u << i)) hold_key(vks[i], want & (1u << i));
    g_rear_keys = want;
    g_rear_prev = p.rear; g_rear_lx = p.rtx; g_rear_ly = p.rty;
}
}  // namespace

uint32_t key_state(uint32_t vk) { return g_down[vk & 0xFF] ? 0x8000u : 0u; }
bool input_grid_active(int* cell) { if (cell) *cell = g_cell; return g_grid; }
bool input_take_ime_request() { bool r = g_ime; g_ime = false; return r; }
void input_text(const std::string& s) {
    for (unsigned char ch : s) if (ch >= 0x20 && ch < 0x7F) post_input(WM_CHAR, ch, 1);
}

void input_warp(int x, int y) { move_to(x, y); g_fx = x; g_fy = y; }

void input_tick(const Pad& raw, uint64_t dt_us) {
    const Controls& cfg = controls();
    Pad p = raw;
    if (cfg.cursor_left_stick) { p.lx = raw.rx; p.ly = raw.ry; p.rx = raw.lx; p.ry = raw.ly; }
    const uint32_t b = p.buttons, pressed = b & ~g_prev, released = g_prev & ~b;
    const double dt = dt_us / 1e6;
    g_clock_us += dt_us;

    bindings_step(b, pressed, released);
    if (!cfg.fast_toggle) g_fast_on = false;
    if (cfg.edge != EDGE_TOGGLE) g_edge_on = false;
    const bool fast = g_fast_held || g_fast_on;
    const bool edge = cfg.edge == EDGE_ON || (cfg.edge == EDGE_HOLD && g_edge_held) || (cfg.edge == EDGE_TOGGLE && g_edge_on);
    const double guard = edge ? 0 : kEdgeGuard;

    if (g_grid) g_cell = grid_cell(p);
    touch_step(p, guard);

    // Cursor from the cursor stick (not while the grid owns it).
    if (!g_grid && !p.touch) {
        double x = axis(p.rx), y = axis(p.ry);
        if (cfg.invert_y) y = -y;
        if (g_stick_hold) { if (x || y) x = y = 0; else g_stick_hold = false; }
        if (x || y) {
            // The game can move the cursor itself (SetCursorPos when a mission starts),
            // so pick up from where it really is.
            uint32_t cx, cy; wx86_get_cursor(&cx, &cy);
            if ((int)cx != (int)g_fx || (int)cy != (int)g_fy) { g_fx = cx; g_fy = cy; }
            const double speed = (fast ? cfg.fast_speed : cfg.cursor_speed) * dt;   // px per second at full deflection
            const double cx2 = cfg.linear ? x : x * std::fabs(x), cy2 = cfg.linear ? y : y * std::fabs(y);
            g_fx += cx2 * speed; g_fy += cy2 * speed;
            const double m = guard;
            g_fx = g_fx < m ? m : g_fx > 639 - m ? 639 - m : g_fx; g_fy = g_fy < m ? m : g_fy > 479 - m ? 479 - m : g_fy;
            move_to((int)g_fx, (int)g_fy);
        } else if (guard > 0 && !p.rear) {
            // Edge scrolling off: when it switches off with the cursor on an
            // edge, step the cursor back inside.
            uint32_t cx, cy; wx86_get_cursor(&cx, &cy);
            const int m = (int)kEdgeGuard;
            const int nx = (int)cx < m ? m : (int)cx > 639 - m ? 639 - m : (int)cx;
            const int ny = (int)cy < m ? m : (int)cy > 479 - m ? 479 - m : (int)cy;
            if (nx != (int)cx || ny != (int)cy) { move_to(nx, ny); g_fx = nx; g_fy = ny; }
        }
    }

    // Map scroll from the other stick, as held arrow keys (counted, so an
    // arrow-key binding and the stick can share a key).
    {
        static bool held[4] = { false, false, false, false };
        double x = axis(p.lx), y = axis(p.ly);
        const double t = cfg.scroll_pct / 100.0;
        const bool want[4] = { x < -t, x > t, y < -t, y > t };
        const uint32_t vks[4] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN };
        for (int i = 0; i < 4; ++i) if (want[i] != held[i]) { hold_key(vks[i], want[i]); held[i] = want[i]; }
    }

    g_prev = b;
}

}  // namespace sc
