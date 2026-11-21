// sc_input.h — Vita controls -> the game's keyboard and mouse (K's scheme).
//
// The layer is platform-free: it takes one controller sample per presented
// frame and turns it into Win32 input messages plus the key-state table that
// GetKeyState/GetAsyncKeyState read. The Vita build feeds it from
// sceCtrlPeekBufferPositive; the host build feeds it from a script
// (SC_PAD_SCRIPT) so the mapping can be tested against the real game.
#pragma once
#include <cstdint>
#include <string>

namespace sc {

// Button bits, same values as SceCtrlButtons.
enum : uint32_t {
    PAD_SELECT = 0x0001, PAD_START = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L = 0x0100, PAD_R = 0x0200,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
};

struct Pad {
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;   // 0..255, 128 = centre
    // Front touch screen, already in game pixels (see input_screen); touch is
    // false when no finger is down or the finger is on a side bar.
    bool touch = false;
    int  tx = 0, ty = 0;
};

// One sample per frame; dt_us is the time since the previous sample.
void input_tick(const Pad& p, uint64_t dt_us);

// Text typed on the on-screen keyboard (L+Select), delivered as characters
// only; Enter is left to the player (Select), as K asked.
void input_text(const std::string& utf8);

// Win32 GetKeyState value for a virtual key (0x8000 = down).
uint32_t key_state(uint32_t vk);

// Command-card overlay state for the presenter: grid shown while D-pad up
// is held, with the highlighted cell (row-major, input_card_cells() of them).
bool input_grid_active(int* cell);
int  input_card_cells();
// A card cell in game pixels: top-left corner and size.
void input_card_rect(int cell, int* x, int* y, int* w, int* h);

// Test scripts only: put the cursor at x,y (sends a mouse move).
void input_warp(int x, int y);
void input_set_screen(int w, int h);   // cursor bounds = the game's screen size
void input_screen(int* w, int* h);

// L+Select asks the platform for its on-screen keyboard.
bool input_take_ime_request();

}  // namespace sc
