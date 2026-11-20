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
| Right stick / touch screen | Cursor (L held: faster, and edge scrolling) |
| Left stick | Scroll the map |
| R | Left click (L+R: right click) |
| D-pad down | Right click |
| Cross | A (attack) |
| Square | S (L: H) |
| Triangle | R (L: G) |
| Circle | P (L: R) |
| D-pad left (hold) + Cross/Square/Triangle/Circle/L/R | Ctrl+1 to 6 (assign group) |
| D-pad up (hold) + Cross/Square/Triangle/Circle/L/R | 1 to 6 (select group) |
| D-pad up (hold) + right stick + R | Click a cell of the 3x3 command card |
| D-pad right | Space |
| Start / L+Start | F10 (menu) / Escape |
| Select / L+Select | Enter / on-screen keyboard |

Double click a unit to select all units of that type on screen.

## Licenses

- winx86 (`tools/winvita/`): MIT, Copyright (c) 2026 Franck Roustan. See `tools/winvita/LICENSE` and `THIRD-PARTY-NOTICES.md` (Box86: MIT, ptitSeb).
- StarCraft and Brood War are trademarks of Blizzard Entertainment. This project is not affiliated with or endorsed by Blizzard.
