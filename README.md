# StarCraft-Vita

A PS Vita port of the original PC StarCraft / Brood War 1.16.1. It runs the
unmodified Windows game through an x86-to-ARM dynamic recompiler (winx86,
derived from Box86), with Win32, DirectDraw and DirectSound replaced by native
Vita code.

**No game files are included, and none ever will be.** You need your own legal
copy of StarCraft 1.16.1 (StarCraft.exe, storm.dll, local.dll, StarDat.mpq,
BrooDat.mpq, and StarCraft.mpq/BroodWar.mpq from your CDs). This port does not
crack, patch or bypass anything: 1.16.1 reads the CD data from the .mpq files
in its own folder.

## Layout

- `scvita/`: the port (boot harness, Win32 shims, Vita presenter, controls, input layer).
- `tools/winvita/`: the winx86 engine (dynarec, runtime, DirectSound emulation), with this port's fixes applied.

## Building

Requires [VitaSDK](https://vitasdk.org).

```sh
# engine
cd tools/winvita
LIBTAG=_prof EXTRA="-DPROF_COUNTERS" TARGET=vita ./build.sh
# game
cd ../../scvita
WXTAG=_prof TARGET=vita ./build.sh      # -> scvita/build-vita/scv.vpk
```

Art in `scvita/sce_sys/` (icon and LiveArea) is packed in if present; `NOART=1` leaves it out.

Leave out `TARGET=vita` to build for Linux ARM (`qemu-arm`) instead, for testing on a PC.

See [CHANGELOG.md](CHANGELOG.md) for what's new in each version.

## Installing

1. Install `scv.vpk` with VitaShell.
2. Copy your StarCraft 1.16.1 folder to `ux0:data/StarCraft/`.
3. You need `ur0:data/libshacccg.suprx` (the shader compiler).

Logs are written to `ux0:data/StarCraft/` (`scvita_log.txt`, `scvita_progress.txt`, `scvita_stdout.txt`).

## Controls

These are the defaults. Hold **D-pad right** and press **Start** at any time to open the controls menu and change any of them.

| Input | Action |
|---|---|
| Right stick | Cursor (D-pad right held: faster; L held: edge scrolling) |
| Touch screen | Cursor under the finger; tap = left click, touch and move = drag a selection box |
| Left stick | Scroll the map |
| R | Left click |
| L+R | Right click |
| Cross | A (attack) |
| Square | S (L: H) |
| Triangle | R (L: G) |
| Circle | P (L: R) |
| D-pad left (hold) + Cross/Square/Triangle/Circle/L/R | Ctrl+1 to 6 (assign group) |
| D-pad down (hold) + Cross/Square/Triangle/Circle/L/R | 1 to 6 (select group) |
| D-pad up (hold) + right stick + R | Click a cell of the 3x3 command card |
| L + D-pad up | Space |
| Start / L+Start | F10 (menu) / Escape |
| Select / L+Select | Enter / on-screen keyboard |
| **L + D-pad right + Start** | **Controls menu**: rebind everything (see below) |

Double click a unit to select all units of that type on screen.

These are the default controls. Every one of them can be changed in game.

### Controls menu

Hold **L** and **D-pad right** and press **Start**, anywhere (main menu or in a match), to open the controls menu. By default it also opens the game menu (F10), which pauses a single-player game; you can turn that off under Port features > Controls menu > When this menu opens. The menu always uses the D-pad, Cross, Circle and Start, whatever you bind, so you can't lock yourself out.

- **PC controls**: every PC key (letters, numbers, Ctrl + number, Shift + number, F1-F12, Space, Enter, Esc, Tab, Shift, Ctrl, Alt, arrows and more) and the left and right mouse buttons. Select one and press Cross, then hold any button or combination of buttons (for example L + Cross) and let go to record it. Triangle records a second binding, Square clears. A binding used by two controls shows in red.
- **Port features**: the command grid, fast cursor, edge scrolling and on-screen keyboard, each with its binding and its options (hold or toggle, fast cursor speed, edge scrolling On / Hold / Toggle / Off).
- **Cursor and sticks**: normal and fast cursor speed, cursor on the right or left stick, smooth or linear response, inverted up/down, dead zone, map-scroll stick push.
- **Touch screen**: front touch Move and click / Move only / Off, right click by long press and/or two-finger tap, long-press time, back touchpad as a trackpad or to scroll the map, and its speed.
- **Cheat macros**: 8 slots. Type the text with the on-screen keyboard (for example `power overwhelming`) and bind each slot to any button or combination. Pressing it opens chat, types the text and presses Enter.
- **Presets**: a touch screen mouse preset (touch clicks, buttons are all keyboard, R = Shift) and Reset to defaults.

When a button is pressed, the binding with the most buttons that are all held wins, so Cross can send A while D-pad down + Cross sends 1. Keys stay held as long as their buttons are held, so Ctrl or Shift on a button work as real modifiers.

Settings are saved to `ux0:data/StarCraft/scvita_controls.txt` when the menu closes and loaded at start. It is plain text (`bind.key_a=Cross, L + Cross`, `macro.1=power overwhelming`); delete it to go back to the defaults.

## Acknowledgements

- **Franck Roustan (Franckrst) and [D2Vita](https://github.com/Franckrst/D2Vita)**: the project that inspired this whole port, and the winx86 engine it runs on.
- **The PS Vita homebrew community**, whose work makes any of this possible, including:
  - **TheFloW**, for HENkaku, h-encore, Ensō and VitaShell.
  - **Rinnegatamante**, for vitaGL and many ports that showed what the Vita can do.
  - **Team Molecule** (Davee, Proxima, xyz, YifanLu), for HENkaku and taiHEN.
  - **The VitaSDK contributors**, for the open toolchain.
  - **ptitSeb**, for Box86, whose dynarec sits at the heart of the engine.
  - Everyone else who has written tools, plugins, guides and ports for the Vita.
- **Claude** (Anthropic), which wrote the port and the engine fixes with KP77C.
- **Blizzard Entertainment**, for creating StarCraft and Brood War, one of the greatest RTS games ever made.

## Licenses

- winx86 (`tools/winvita/`): MIT, Copyright (c) 2026 Franck Roustan. See `tools/winvita/LICENSE` and `THIRD-PARTY-NOTICES.md` (Box86: MIT, ptitSeb).
- Controls menu font: Inter (The Inter Project Authors), SIL Open Font License 1.1. See `scvita/third_party/Inter-OFL.txt`.
- StarCraft and Brood War are trademarks of Blizzard Entertainment. This project is not affiliated with or endorsed by Blizzard.
