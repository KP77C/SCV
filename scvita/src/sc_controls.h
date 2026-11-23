// sc_controls.h — the player's control settings, as data (K: keep the
// mapping as data so it can be rebound from a menu).
//
// Every PC control (each key, Ctrl/Shift + number, left/right click) and
// every Vita feature (command grid, fast cursor, edge scrolling, on-screen
// keyboard) is an ACTION. Each action has up to two BINDINGS, and a binding
// is any button or combination of Vita buttons the player records, e.g.
// "Cross" or "L + Cross" or "D-pad left + R".
//
// How a press is resolved (sc_input.cpp): when a button goes down, of the
// bindings that use that button and whose buttons are all held, the one
// with the most buttons wins. So with "Cross" = A and "D-pad down + Cross" = 1,
// Cross alone sends A and Cross with D-pad down held sends 1. The action
// stays active until any button of its binding is let go. Key actions are
// held keys (holding the binding holds the key), so Ctrl or Shift bound to a
// button act as real modifiers for the other bindings.
//
// Stored as KEY=VALUE text in ux0:data/StarCraft/scvita_controls.txt (host:
// SC_CONTROLS, default ./scvita_controls.txt), loaded at boot and written
// when the controls menu closes. A missing or broken line keeps its default.
#pragma once
#include <cstdint>

namespace sc {

enum ActKind : uint8_t {
    ACT_KEY,        // a key held while the binding is held (vk, plus mod held around it)
    ACT_LCLICK, ACT_RCLICK,
    ACT_FAST,       // fast cursor (hold or toggle)
    ACT_EDGE,       // edge scrolling button (hold or toggle by a tap)
    ACT_GRID,       // command grid (hold or toggle)
    ACT_IME,        // on-screen keyboard
    ACT_MACRO,      // cheat macro: opens chat, types the slot's text, presses Enter (vk = slot)
};
enum ActGroup : uint8_t { GRP_PORT, GRP_MOUSE, GRP_LETTERS, GRP_NUMBERS, GRP_OTHER, GRP_MACRO, GRP_COUNT };

struct Action {
    const char* id;          // file name, e.g. "key_a"
    const char* name;        // menu name, e.g. "A"
    uint8_t kind, group;
    uint16_t vk, mod;        // ACT_KEY: key and an optional modifier key held with it (VK_CONTROL / VK_SHIFT / 0)
};

int action_count();
const Action& action(int i);
int action_find(const char* id);

enum EdgeScroll : int { EDGE_ON, EDGE_HOLD, EDGE_TOGGLE, EDGE_OFF, EDGE_COUNT };
enum TouchMode : int { TOUCH_CLICK, TOUCH_MOVE, TOUCH_OFF, TOUCH_COUNT };
enum TouchRight : int { TR_OFF, TR_LONG, TR_TWO, TR_BOTH, TR_COUNT };
enum RearMode : int { REAR_OFF, REAR_CURSOR, REAR_SCROLL, REAR_COUNT };

constexpr int kMaxActions = 128;
constexpr int kMacros = 8, kMacroLen = 64;

struct Controls {
    uint32_t bind[kMaxActions][2];   // button masks (PAD_* bits), 0 = unbound
    int cursor_speed;                // px/s at full stick (normal)
    int fast_speed;                  // px/s with fast cursor on
    bool fast_toggle;                // fast cursor: press to switch instead of hold
    bool cursor_left_stick;          // cursor on the left stick, map scroll on the right
    bool linear;                     // stick response: linear instead of smooth (squared)
    bool invert_y;                   // cursor stick up/down inverted
    int deadzone_pct;
    int scroll_pct;                  // map-scroll stick: how far to push before it scrolls
    int edge;                        // EdgeScroll
    bool grid_toggle;                // command grid: press to open/close instead of hold
    int touch;                       // TouchMode (front screen)
    int touch_right;                 // TouchRight: how the front screen right-clicks
    int long_ms;                     // long-press time
    int rear;                        // RearMode (back touchpad)
    int rear_speed_pct;              // back touchpad cursor speed
    char macro[kMacros][kMacroLen];  // cheat macro texts (empty = unused)
    bool menu_pauses;                // opening the controls menu also opens the game menu (F10), which pauses
};

Controls& controls();
void controls_defaults(Controls& c);
void controls_preset_touch(Controls& c);   // touch screen mouse, buttons as keyboard
bool controls_load();
bool controls_save();
const char* controls_path();

const char* button_name(uint32_t pad_bit);
// "L + Cross" for a mask; out must hold 64 bytes.
const char* chord_name(uint32_t mask, char* out, int n);

}  // namespace sc
