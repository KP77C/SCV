// sc_input.cpp — Deus Ex Vita controls (first default layout, 2026-11-21).
//
//   Left stick    move (W / S, strafe A / D)
//   Right stick   look (mouse); in menus it moves the pointer
//   R             fire / click (left mouse)
//   L             use, pick up, open (right mouse)
//   Cross         jump (Space)            Circle    crouch (X)
//   Square        reload (;)              Triangle  inventory (F1)
//   D-pad left/right  previous / next belt item (mouse wheel)
//   D-pad up      goals and notes (F2)    D-pad down  put weapon away (Backspace)
//   Select        augmentation 1 (F3)     L+Select  on-screen keyboard
//   Start         main menu (Escape)
//   Touch screen  pointer under the finger, tap = click (menus)
// DX_LOOK_SPEED (pixels per second at full stick, default 900) sets look speed.
#include <cstdlib>
#include "sc_input.h"
#include "sc_host.h"

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
    default: {
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
    const double dz = 0.18;
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

void input_tick(const Pad& p, uint64_t dt_us) {
    const uint32_t b = p.buttons, pressed = b & ~g_prev, released = g_prev & ~b;
    const bool L = b & PAD_L;
    const double dt = dt_us / 1e6;
    g_clock_us += dt_us;

    // Touch: pointer under the finger, finger down = left button (menus).
    if (p.touch) {
        if ((int)p.tx != (int)g_fx || (int)p.ty != (int)g_fy) move_to(p.tx, p.ty);
        g_fx = p.tx; g_fy = p.ty;
        if (!g_touch_btn && !g_lbtn) { lbutton(true); g_touch_btn = true; }
    }
    if (!p.touch && g_touch_btn) { lbutton(false); g_touch_btn = false; }

    // Look / pointer from the right stick. In game Unreal puts the cursor back
    // in the middle every frame (SetCursorPos) and reads how far it moved, so
    // always continue from where the cursor really is.
    if (!p.touch) {
        double x = axis(p.rx), y = axis(p.ry);
        static double rem_x = 0, rem_y = 0;
        if (x || y) {
            static const double look = std::getenv("DX_LOOK_SPEED") ? std::atof(std::getenv("DX_LOOK_SPEED")) : 900.0;
            // Squared response: fine aim near the centre, fast turns at the edge.
            const double sx = x * std::fabs(x) * look * dt + rem_x, sy = y * std::fabs(y) * look * dt + rem_y;
            const int ix = (int)sx, iy = (int)sy;
            rem_x = sx - ix; rem_y = sy - iy;
            if (ix || iy) {
                uint32_t cx, cy; wx86_get_cursor(&cx, &cy);
                move_to((int)cx + ix, (int)cy + iy);
                uint32_t nx, ny; wx86_get_cursor(&nx, &ny); g_fx = nx; g_fy = ny;
            }
        } else { rem_x = rem_y = 0; }
    }

    // Movement from the left stick, as held W/A/S/D.
    {
        const double x = axis(p.lx), y = axis(p.ly);
        key('A', x < -0.35); key('D', x > 0.35);
        key('W', y < -0.35); key('S', y > 0.35);
    }

    // Mouse buttons.
    if (pressed & PAD_R) lbutton(true);
    if (released & PAD_R) lbutton(false);
    if (pressed & PAD_L) rbutton(true);
    if (released & PAD_L) rbutton(false);

    // Held keys.
    const struct { uint32_t bit, vk; } held[] = {
        { PAD_CROSS, VK_SPACE }, { PAD_CIRCLE, 'X' }, { PAD_SQUARE, 0xBA /* ; */ }, { PAD_TRIANGLE, 0x70 /* F1 */ },
        { PAD_UP, 0x71 /* F2 */ }, { PAD_DOWN, 0x08 /* Backspace */ }, { PAD_START, VK_ESCAPE },
    };
    for (const auto& h : held) {
        if (pressed & h.bit) key(h.vk, true);
        if (released & h.bit) key(h.vk, false);
    }
    // Select: F3 (first augmentation); L+Select opens the on-screen keyboard.
    static bool sel_key = false;
    if (pressed & PAD_SELECT) { if (L) g_ime = true; else { key(0x72, true); sel_key = true; } }
    if ((released & PAD_SELECT) && sel_key) { key(0x72, false); sel_key = false; }
    // D-pad left/right: mouse wheel (previous / next belt item).
    if (pressed & (PAD_LEFT | PAD_RIGHT)) {
        const int16_t delta = (pressed & PAD_LEFT) ? 120 : -120;
        post_input(0x020A /* WM_MOUSEWHEEL */, ((uint32_t)(uint16_t)delta << 16) | (mk() & 0xFFFF), xy());
    }
    g_prev = b;
}

}  // namespace sc
