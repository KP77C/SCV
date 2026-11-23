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
WXTAG=_prof TARGET=vita ./build.sh      # -> scvita/build-vita/scvita.vpk
```

Leave out `TARGET=vita` to build for Linux ARM (`qemu-arm`) instead, for testing on a PC.

## Installing

1. Install `scvita.vpk` with VitaShell.
2. Copy your StarCraft 1.16.1 folder to `ux0:data/StarCraft/`.
3. You need `ur0:data/libshacccg.suprx` (the shader compiler).

Logs are written to `ux0:data/StarCraft/` (`scvita_log.txt`, `scvita_progress.txt`, `scvita_stdout.txt`).

## Controls

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

Double click a unit to select all units of that type on screen.

These are the default controls. Every one of them can be changed in game.

### Controls menu

Hold **D-pad right** and press **Start**, anywhere (main menu or in a match), to open the controls menu. The game keeps running underneath, so pause it with the game menu first if a match is on. The menu always uses the D-pad, Cross, Circle and Start, whatever layout you set, so you can't lock yourself out.

- **Button layout**: move any job (left click, modifier, command grid, Ctrl, numbers, fast cursor, Start keys, Select key, the four key buttons) to any button. The job that was on that button moves to the old one, so nothing is lost.
- **Keys**: choose the key each key button sends, alone and with the modifier, plus the Start, modifier + Start, Select and modifier + D-pad up keys. Letters, digits, F1-F12, Space, Enter, Esc, Tab and more, or None.
- **Cursor and sticks**: normal and fast cursor speed, fast cursor on hold or toggle, cursor on the right or left stick, smooth or linear stick response, inverted up/down, stick dead zone.
- **Scrolling, grid and touch**: edge scrolling On, Hold modifier, Toggle (tap the modifier on its own) or Off; how far to push the map-scroll stick; command grid on hold or toggle; touch screen Move and click, Move only or Off.
- **Reset everything to defaults**.

Settings are saved to `ux0:data/StarCraft/scvita_controls.txt` when the menu closes and loaded at start. It is plain text; delete it to go back to the defaults.

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
