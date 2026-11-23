// sc_controls.h — the player's control settings, as data (K: keep the
// mapping as data so it can be rebound from a menu).
//
// Three parts:
//   * a button layout: which physical Vita button does each job. The input
//     layer works on "logical" buttons (the default layout, PAD_* bits); the
//     layout is a permutation that turns physical presses into logical ones,
//     so swapping two jobs never breaks a combo;
//   * the keyboard keys sent by the key buttons and by Start/Select;
//   * cursor, stick, edge-scroll and touch options.
//
// Stored as KEY=VALUE text in ux0:data/StarCraft/scvita_controls.txt (host:
// SC_CONTROLS, default ./scvita_controls.txt), loaded at boot and written
// when the controls menu closes. A missing or broken line keeps its default.
#pragma once
#include <cstdint>

namespace sc {

enum Job : int {
    JOB_CLICK, JOB_MOD, JOB_GRID, JOB_CTRL, JOB_NUM, JOB_FAST, JOB_START, JOB_SELECT,
    JOB_KEY1, JOB_KEY2, JOB_KEY3, JOB_KEY4, JOB_COUNT
};

enum EdgeScroll : int { EDGE_ON, EDGE_HOLD, EDGE_TOGGLE, EDGE_OFF, EDGE_COUNT };
enum TouchMode : int { TOUCH_CLICK, TOUCH_MOVE, TOUCH_OFF, TOUCH_COUNT };

struct Controls {
    uint32_t button[JOB_COUNT];      // physical PAD_* bit doing each job
    uint32_t key[4], key_mod[4];     // key buttons 1-4: key alone / with the modifier (VK, 0 = none)
    uint32_t key_start, key_mod_start, key_select, key_mod_grid;
    int cursor_speed;                // px/s at full stick (normal)
    int fast_speed;                  // px/s with the fast-cursor button held
    bool cursor_left_stick;          // cursor on the left stick, map scroll on the right
    int deadzone_pct;
    int edge;                        // EdgeScroll
    int touch;                       // TouchMode
    bool fast_toggle;                // fast cursor: tap to switch instead of hold
    bool grid_toggle;                // command grid: tap to open/close instead of hold
    bool linear;                     // stick response: linear instead of smooth (squared)
    bool invert_y;                   // cursor stick up/down inverted
    int scroll_pct;                  // map-scroll stick: how far to push before it scrolls
};

Controls& controls();
void controls_defaults(Controls& c);
bool controls_load();
bool controls_save();
const char* controls_path();

// Logical bit of each job (the default layout).
uint32_t job_logical(int job);
// Physical buttons -> logical buttons through the layout.
uint32_t controls_remap(uint32_t physical);

const char* button_name(uint32_t pad_bit);
const char* key_name(uint32_t vk);
// The keys a key slot can take, in menu order.
int key_choice_count();
uint32_t key_choice(int i);

}  // namespace sc
