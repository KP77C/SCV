# WarCraft III Vita

A PS Vita port of PC WarCraft III: Reign of Chaos / The Frozen Throne, patch
1.27a. It runs the unmodified Windows game (War3.exe, Game.dll) through the
winx86 x86-to-ARM dynamic recompiler in `tools/winvita`, with Win32, OpenGL
(vitaGL), DirectSound and Winsock replaced by native Vita code. The game draws
with its own OpenGL renderer (`-opengl -window`).

**No game files are included.** You need your own legal copy of WarCraft III
patched to 1.27a. Nothing in the game is cracked, patched or bypassed.

Status: single player (custom games against the computer) runs on the test
host. Not yet verified on Vita hardware. No movies, no online or LAN play.

## Building

```sh
cd tools/winvita && TARGET=vita ./build.sh                 # engine
cd ../../wc3vita && TARGET=vita ./build.sh                 # -> build-vita/w3vita.vpk
```

`tools/build_vitagl.sh` builds vitaGL (pinned revisions) on the first Vita
build. Without `TARGET=vita`, `build.sh` makes a Linux ARM test binary that
runs under `qemu-arm` with Mesa's OSMesa.

## Installing

1. Install `w3vita.vpk` with VitaShell.
2. Copy your WarCraft III 1.27a folder (War3.exe, Game.dll, Storm.dll,
   Mss32.dll, ijl15.dll, msvcr120.dll, msvcp120.dll, the .mpq files, Maps/)
   to `ux0:data/WarCraft3/`.
3. You need `ur0:data/libshacccg.suprx` (the shader compiler).

Optional settings, one per line, in `ux0:data/WarCraft3/w3vita_env.txt`:

- `W3_RES=720x408` render size: 960x544 (default), 720x408, 640x368 or
  480x272. The Vita's display scales a smaller picture up to the screen for
  free, so a lower size saves GPU time.
- `SC_FPS=60` unlocks 60 fps (default: locked 30).

Settings changed in the game's options are kept in
`ux0:data/WarCraft3/w3vita_registry.txt`; profiles and saves are in
`ux0:data/WarCraft3/Save/`. Logs: `w3vita_log.txt` and `w3vita_stdout.txt`.

## Controls

The StarCraft port's scheme, with the command grid moved to WarCraft III's
4x3 card.

| Input | Action |
|---|---|
| Right stick | Cursor (L held: faster, and edge scrolling) |
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
| D-pad up (hold) | Command card: the highlight starts top left; each push of the right stick moves it one button; R presses it |
| D-pad right | Space |
| Start / L+Start | F10 (menu) / Escape |
| Select / L+Select | Enter / on-screen keyboard |
