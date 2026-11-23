// sc_input.cpp — K's control scheme (2026-11-20), see sc_input.h.
//
// Everything below is the DEFAULT layout. The player can move every job to
// another button, change the keys, and change the cursor/stick/touch options
// in the controls menu (D-pad right + Start, sc_menu.cpp); sc_controls.h holds
// those settings. This file works on logical buttons (the default ones), and
// the physical pad is remapped onto them first.
//
//   Right stick   cursor (D-pad right held: faster; L held: edge scrolling)
//   Touch screen  cursor under the finger; tap = left click, touch and move = drag
//   Left stick    map scroll (arrow keys)
//   R             left click        L+R      right click (double R = double click)
//   Cross         A
//   Triangle      R                 L+Tri    G
//   Square        S                 L+Sq     H
//   Circle        P                 L+Circle R
//   D-pad left    Ctrl (held)       L + D-pad up  Space (D-pad right unbound)
//   D-pad left + face/L/R  Ctrl+1-6 (Cross 1, Square 2, Triangle 3, Circle 4, L 5, R 6)
//   D-pad down + face/L/R  1-6
//   L + cursor at a screen edge: scroll (without L the cursor stops 2 px short)
//   D-pad up      hold: 3x3 command grid, right stick picks the cell,
//                 R clicks it (grid stays up until D-pad up is released)
//   Select        Enter             L+Select on-screen keyboard (no Enter)
//   Start         F10               L+Start Escape
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
uint32_t g_prev = 0;             // previous buttons
bool     g_lbtn = false, g_rbtn = false;
uint32_t g_r_owner = 0;          // which input holds the right mouse button (PAD_R or PAD_CROSS)
uint32_t g_face_key[16];         // key a face button pressed, so release matches press
bool     g_ctrl_by_dpad = false;
bool     g_grid = false; int g_cell = 4;
bool     g_ime = false;
// D-pad up/down swapped at K's request (build 16): grid on up, Ctrl on down.
constexpr uint32_t kBtnGrid = PAD_UP, kBtnNum = PAD_DOWN;   // grid on up, numbers on down (K, build 20)
constexpr double kEdgeGuard = 2;   // px kept from the screen edge unless L is held (no edge scroll)
bool     g_r_grid = false;     // current R press was used by the grid
bool     g_ctrl_by_left = false; // D-pad left is holding Ctrl
bool     g_touch_btn = false;   // the left button is held by a finger on the touch screen
uint32_t g_sh_owned = 0;         // shoulder buttons currently sending a digit (not mouse/grid)
bool     g_stick_hold = false;   // after a grid click, ignore the right stick until it recentres
double   g_fx = 320, g_fy = 240; // sub-pixel cursor

// Centres of the 3x3 command card at 640x480, row-major. Rows 0-1 measured on
// Zeratul's card in Protoss mission 1; row 2 follows the same spacing.
constexpr int kCardX0 = 522, kCardDX = 46, kCardY0 = 376, kCardDY = 39;

uint32_t scan(uint32_t vk) {
    switch (vk) {
    case VK_RETURN: return 0x1C; case VK_CONTROL: return 0x1D; case VK_SPACE: return 0x39;
    case VK_LEFT: return 0x4B; case VK_RIGHT: return 0x4D; case VK_UP: return 0x48; case VK_DOWN: return 0x50;
    case VK_F10: return 0x44; case VK_ESCAPE: return 0x01;
    case 0x09: return 0x0F; case 0x08: return 0x0E; case 0x2E: return 0x53; case 0x10: return 0x2A;
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
void tap(uint32_t vk) { key(vk, true); key(vk, false); }

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
    // Physical -> logical: the player's layout, then the stick swap.
    Pad p = raw;
    p.buttons = controls_remap(raw.buttons);
    if (cfg.cursor_left_stick) { p.lx = raw.rx; p.ly = raw.ry; p.rx = raw.lx; p.ry = raw.ly; }
    const uint32_t b = p.buttons, pressed = b & ~g_prev, released = g_prev & ~b;
    const bool L = b & PAD_L;
    const double dt = dt_us / 1e6;
    g_clock_us += dt_us;

    // Toggles: fast cursor (tap D-pad right) and edge scrolling (tap L alone:
    // pressed and let go within 350 ms with no other button pressed meanwhile,
    // so L combos never flip it).
    static bool fast_on = false, edge_on = false, l_alone = false;
    static uint64_t l_down_us = 0;
    if (cfg.fast_toggle && (pressed & PAD_RIGHT)) fast_on = !fast_on;
    if (!cfg.fast_toggle) fast_on = false;
    if (pressed & PAD_L) { l_alone = true; l_down_us = g_clock_us; }
    if (pressed & ~PAD_L) l_alone = false;
    if ((released & PAD_L) && l_alone && g_clock_us - l_down_us <= 350000 && cfg.edge == EDGE_TOGGLE) {
        edge_on = !edge_on;
        log("[not-bound] controls: edge scrolling %s", edge_on ? "on" : "off");
    }
    const bool fast = cfg.fast_toggle ? fast_on : (b & PAD_RIGHT) != 0;
    const bool edge = cfg.edge == EDGE_ON || (cfg.edge == EDGE_HOLD && L) || (cfg.edge == EDGE_TOGGLE && edge_on);
    const double guard = edge ? 0 : kEdgeGuard;

    // Command grid: hold D-pad up, pick with the right stick, press R to
    // click the cell (K, 2026-11-20). The grid stays up while D-pad up is
    // held, so several buttons can be pressed in a row; releasing closes it.
    // L + D-pad up = Space (K, build 21); the grid opens only without L.
    // Grid toggle (player option): a tap opens it, another tap closes it.
    static uint32_t mod_grid_vk = 0;
    static bool grid_opened_now = false;
    grid_opened_now = false;
    if ((pressed & kBtnGrid) && L) { if (cfg.key_mod_grid) { mod_grid_vk = cfg.key_mod_grid; key(mod_grid_vk, true); } }
    else if ((pressed & kBtnGrid) && cfg.grid_toggle && g_grid) g_grid = false;
    else if (pressed & kBtnGrid) { g_grid = true; g_cell = 4; grid_opened_now = true; }
    if ((released & kBtnGrid) && mod_grid_vk) { key(mod_grid_vk, false); mod_grid_vk = 0; }
    if (g_grid) g_cell = grid_cell(p);
    if ((released & kBtnGrid) && g_grid && !cfg.grid_toggle) g_grid = false;
    (void)grid_opened_now;
    // Shoulders as digits (K, build 18): D-pad left + L/R = Ctrl+5/6 (Ctrl is
    // already held by D-pad left), D-pad down + L/R = 5/6 (K, build 20). D-pad up
    // is only the grid now, so R there always clicks the picked cell, centre included.
    static uint32_t sh_key[2] = { 0, 0 };
    const struct { uint32_t bit; uint32_t digit; } shoulders[2] = { { PAD_L, '5' }, { PAD_R, '6' } };
    for (int i = 0; i < 2; ++i) {
        if (pressed & shoulders[i].bit) {
            const bool by_left = b & PAD_LEFT, by_num = b & kBtnNum;
                        if (by_left || by_num) { key(shoulders[i].digit, true); sh_key[i] = shoulders[i].digit; }
        }
        if ((released & shoulders[i].bit) && sh_key[i]) { key(sh_key[i], false); sh_key[i] = 0; }
        if (sh_key[i]) g_sh_owned |= shoulders[i].bit;
    }
    if ((pressed & PAD_R) && g_grid && !(g_sh_owned & PAD_R)) {
        g_r_grid = true;                      // this R press belongs to the grid, not to the mouse
        uint32_t ox, oy; wx86_get_cursor(&ox, &oy);
        move_to(kCardX0 + kCardDX * (g_cell % 3), kCardY0 + kCardDY * (g_cell / 3));
        lbutton(true); lbutton(false);
        move_to((int)ox, (int)oy);
        g_fx = ox; g_fy = oy;
        g_stick_hold = true;
    }

    // Front touch screen: the cursor goes where the finger is and follows a
    // drag (K, 2026-11-20). Same edge rule as the stick: without L it stops 2 px short.
    if (cfg.touch == TOUCH_OFF) p.touch = false;
    if (!g_grid && p.touch) {
        const double m = guard;
        double x = p.tx, y = p.ty;
        x = x < m ? m : x > 639 - m ? 639 - m : x; y = y < m ? m : y > 479 - m ? 479 - m : y;
        if ((int)x != (int)g_fx || (int)y != (int)g_fy) move_to((int)x, (int)y);
        g_fx = x; g_fy = y;
        // Touch clicks too (K, build 20): finger down = left button down where
        // it landed, finger up = release. A tap is a click (two quick taps a
        // double click), and moving while touching drags a selection box.
        if (cfg.touch == TOUCH_CLICK && !g_touch_btn && !g_lbtn) { lbutton(true); g_touch_btn = true; }
    }
    if (!p.touch && g_touch_btn) { lbutton(false); g_touch_btn = false; }

    // Cursor from the right stick (not while the grid owns it).
    if (!g_grid && !p.touch) {
        double x = axis(p.rx), y = axis(p.ry);
        if (cfg.invert_y) y = -y;
        if (g_stick_hold) { if (x || y) x = y = 0; else g_stick_hold = false; }
        if (x || y) {
            // The game can move the cursor itself (SetCursorPos when a mission starts),
            // so pick up from where it really is.
            uint32_t cx, cy; wx86_get_cursor(&cx, &cy);
            if ((int)cx != (int)g_fx || (int)cy != (int)g_fy) { g_fx = cx; g_fy = cy; }
            const double speed = (fast ? cfg.fast_speed : cfg.cursor_speed) * dt;   // px per second at full deflection; D-pad right = fast (K, build 21)
            const double cx2 = cfg.linear ? x : x * std::fabs(x), cy2 = cfg.linear ? y : y * std::fabs(y);
            g_fx += cx2 * speed; g_fy += cy2 * speed;
            const double m = guard;
            g_fx = g_fx < m ? m : g_fx > 639 - m ? 639 - m : g_fx; g_fy = g_fy < m ? m : g_fy > 479 - m ? 479 - m : g_fy;
            move_to((int)g_fx, (int)g_fy);
        } else if (guard > 0) {
            // Edge scrolling only while L is held (K, 2026-11-20): when L is let
            // go with the cursor on an edge, step it back inside.
            uint32_t cx, cy; wx86_get_cursor(&cx, &cy);
            const int m = (int)kEdgeGuard;
            const int nx = (int)cx < m ? m : (int)cx > 639 - m ? 639 - m : (int)cx;
            const int ny = (int)cy < m ? m : (int)cy > 479 - m ? 479 - m : (int)cy;
            if (nx != (int)cx || ny != (int)cy) { move_to(nx, ny); g_fx = nx; g_fy = ny; }
        }
    }

    // Map scroll from the left stick, as held arrow keys. StarCraft scrolls at
    // its own set speed, so L does not change it yet.
    {
        double x = axis(p.lx), y = axis(p.ly);
        const double t = cfg.scroll_pct / 100.0;
        key(VK_LEFT, x < -t); key(VK_RIGHT, x > t);
        key(VK_UP, y < -t);   key(VK_DOWN, y > t);
    }

    // Mouse buttons. R = left click, L+R = right click, Cross = right click.
    if ((pressed & PAD_R) && !g_r_grid && !(g_sh_owned & PAD_R)) { if (L) { g_r_owner = PAD_R; rbutton(true); } else lbutton(true); }
    if (released & PAD_R) {
        if (g_sh_owned & PAD_R) {}            // that R press sent 6, not a click
        else if (g_r_grid) g_r_grid = false;
        else if (g_r_owner == PAD_R) { rbutton(false); g_r_owner = 0; } else lbutton(false);
    }

    g_sh_owned &= ~released;

    // Face buttons.
    // Keys come from the settings (defaults A/A, S/H, R/G, P/R).
    struct Face { uint32_t bit; uint32_t plain, with_l, digit; };
    const Face faces[] = {
        { PAD_CROSS, cfg.key[0], cfg.key_mod[0], '1' }, { PAD_SQUARE, cfg.key[1], cfg.key_mod[1], '2' },
        { PAD_TRIANGLE, cfg.key[2], cfg.key_mod[2], '3' }, { PAD_CIRCLE, cfg.key[3], cfg.key_mod[3], '4' },
    };
    for (int i = 0; i < 4; ++i) {
        const Face& f = faces[i];
        if (pressed & f.bit) {
            if (b & PAD_LEFT) {                                                  // D-pad left (Ctrl held) + face: Ctrl+1-4
                if (!g_down[VK_CONTROL]) key(VK_CONTROL, true);
                key(f.digit, true); g_face_key[i] = f.digit;
            } else if (b & kBtnNum) {                                            // D-pad down + face: 1-4 (K, build 20)
                const bool ctrl = g_down[VK_CONTROL];
                if (ctrl) key(VK_CONTROL, false);
                key(f.digit, true); g_face_key[i] = f.digit | (ctrl ? 0x100 : 0);
            } else {
                uint32_t vk = L ? f.with_l : f.plain; if (vk) key(vk, true); g_face_key[i] = vk;
            }
        }
        if (released & f.bit) {
            uint32_t k = g_face_key[i];
            if (k) {
                key(k & 0xFF, false);
                if ((k & 0x100) && g_ctrl_by_dpad) key(VK_CONTROL, true);   // Ctrl back while D-pad up stays held
            }
            g_face_key[i] = 0;
        }
    }

    // Single keys.
    // D-pad left: Ctrl held for as long as it is (K, build 17); G moved to L+Triangle.
    if (pressed & PAD_LEFT) { g_ctrl_by_left = true; key(VK_CONTROL, true); }
    if ((released & PAD_LEFT) && g_ctrl_by_left) { g_ctrl_by_left = false; if (!g_ctrl_by_dpad) key(VK_CONTROL, false); }
    // D-pad right: unbound (Space moved to L + D-pad up, K build 21).
    // Start = F10 (game menu), L+Start = Escape (K, 2026-11-20). The key that
    // went down is the one released, even if L changes in between.
    static uint32_t start_vk = 0;
    if (pressed & PAD_START) { start_vk = L ? cfg.key_mod_start : cfg.key_start; if (start_vk) key(start_vk, true); }
    if ((released & PAD_START) && start_vk) { key(start_vk, false); start_vk = 0; }
    static uint32_t select_vk = 0;
    if (pressed & PAD_SELECT) { if (L) g_ime = true; else if (cfg.key_select) { select_vk = cfg.key_select; key(select_vk, true); } }
    if ((released & PAD_SELECT) && select_vk) { key(select_vk, false); select_vk = 0; }

    g_prev = b;
}

}  // namespace sc
