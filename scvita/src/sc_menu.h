// sc_menu.h — the controls menu (D-pad right + Start, anywhere).
//
// A full-screen overlay drawn by us, not by the game: the game keeps running
// underneath (pause it with its own menu first if a match is on) and gets no
// input while the menu is open. The menu always uses the physical buttons
// (D-pad, Cross, Circle, Start), so a bad layout can never lock the player out.
#pragma once
#include <cstdint>
#include "sc_input.h"

namespace sc {

// Per-frame entry point for the presenters: feeds the menu or the game.
void input_frame(const Pad& physical, uint64_t dt_us);

bool menu_is_open();
// The menu image, 960x544 RGBA (bytes R,G,B,A), or nullptr while closed.
// *seq changes whenever the image changes.
const uint32_t* menu_image(uint32_t* seq);

constexpr int kMenuW = 960, kMenuH = 544;

}  // namespace sc
