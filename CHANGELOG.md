# Changelog

## 0.2

### New: controls menu
Press **L + D-pad right + Start** anywhere (main menu or in a match) to open it.

- **Rebind everything.** Every PC key and mouse button is its own control:
  - letters A-Z and numbers 0-9
  - Ctrl + 0-9 and Shift + 0-9 (control groups)
  - F1-F12
  - Space, Enter, Esc, Tab, Backspace, Delete, Shift, Ctrl, Alt, Home, Pause, Page Up/Down, the arrow keys, + and -
  - left and right click
- **Record any button or combination.** Select a control, press Cross, then hold any buttons you like (for example L + D-pad down + Cross) and let go. A combo can use as many buttons as you want.
- **Two bindings per control.** Cross records the first, Triangle the second, Square clears.
- **Red conflicts.** A binding used by two controls is shown in red.
- **Smart combos.** When you press a button, the binding with the most buttons held wins, so Cross can send A while D-pad down + Cross sends 1. Keys stay held as long as their buttons are, so Ctrl or Shift on a button works as a real modifier.
- **Can't lock you out.** The menu always uses the D-pad, Cross, Circle and Start, whatever you bind.
- **Pauses the game.** Opening the menu also opens the game menu (F10), which pauses a single-player game. You can turn this off.

### Port features page
Each port feature has its own binding and options:
- **Command grid:** hold or toggle.
- **Fast cursor:** hold or toggle, and its speed.
- **Edge scrolling:** On, Hold button, Toggle button or Off.
- **On-screen keyboard:** its binding.
- **Controls menu:** when it opens, keep playing or open the game menu (pause).

### Cursor and sticks page
- Normal and fast cursor speed.
- Cursor on the right or left stick (map scrolling goes on the other stick).
- Smooth or linear stick response.
- Invert up/down.
- Dead zone.
- How far to push the map-scroll stick before it scrolls.

### Touch screen page
- **Front touch:** move and click, move only, or off.
- **Right click by touch:** long press, two-finger tap, both, or off, with an adjustable long-press time.
- **Back touchpad (new):** use it as a trackpad for the cursor or to scroll the map, with adjustable speed.
- **Touch mouse preset:** the touch screen is the mouse (tap to click, long press or two fingers to right click), and the buttons are all keyboard keys (R is Shift).

### Cheat macros
- 8 slots. Type the text with the on-screen keyboard (for example `power overwhelming`).
- Bind each slot to any button or combo. Pressing it opens chat, types the text and presses Enter.
- Or send one straight from the menu with **Use macro N now**. This closes the controls menu and the game menu first.

### Settings
- Saved to `ux0:data/StarCraft/scvita_controls.txt` when the menu closes, and loaded at start.
- Plain text, so you can edit it by hand. Delete it to go back to the defaults.
- **Reset to defaults** is in the menu (press Cross twice).
- The defaults are the same as 0.1a.

### Other
- The VPK is now named `scv.vpk`.
- The app version in the bubble info is 0.2.
- The menu font is Inter (SIL Open Font License).

### Notes
- Upgrading from 0.1a: your old controls file lines are ignored, so you start on the defaults.
- The controls menu has been tried on a Vita. Still to be tried there: the back touchpad, and long-press and two-finger right click.
