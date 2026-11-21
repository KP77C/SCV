# OpenDE for PS Vita

Deus Ex on the PS Vita through [OpenDE](https://github.com/jarmitage99/OpenDE)
(an open-source C++ reimplementation of the Deus Ex engine that runs the game's
own UnrealScript packages; MIT licence). Unlike `../dxvita`, which translates
the original Windows x86 code, this runs natively on the Vita's ARM CPU.

The game files are not included: copy your own Deus Ex GOTY (1.112fm) install
to `ux0:data/DeusEx/` so that `System/`, `Maps/`, `Textures/`, `Sounds/` and
`Music/` are directly inside it (the same place `dxvita` uses).
`ur0:data/libshacccg.suprx` is needed (the Vita's shader compiler).

## Build

    ./build.sh        # -> build-vita/opendevita.vpk

The first run clones OpenDE at the tested revision into `./OpenDE` and applies
`patches/opende-vita.patch`. Change the code in `./OpenDE`, then run
`./make_patch.sh` to update the patch.

## What the Vita patch changes

- Graphics: vitaGL. One small Cg shader per material type
  (`src/render/vita_shader.cc`) instead of the PC's single branching GLSL
  shader. No gamma/MSAA/FXAA/ambient-occlusion passes, no occlusion queries.
- Textures: 8-bit paletted textures stay paletted on the GPU, DXT1 stays DXT1.
- Memory: package files are read from the memory card when an object loads
  instead of being kept in memory whole; decoded sounds are 16-bit only and
  their cache is capped at 32 MiB.
- Sound: sceAudioOut thread at 22050 Hz, no reverb.
- Controls: as in `../dxvita` (see `src/game/main.cc`, `processVitaInput`).

## On the Vita

- Log: `ux0:data/DeusEx/opende_log.txt` (previous run: `opende_log_prev.txt`),
  with a speed and memory line every 5 seconds.
- Settings and saves: `ux0:data/DeusEx/deusex/`.
- Options, one `NAME=value` per line in `ux0:data/DeusEx/opende_env.txt`:
  `DX_LOOK_SPEED` (900), `DX_TEXTURE_SKIP=1` (half-size big textures),
  `DX_AUDIO_HZ` (22050), `DX_REVERB=1`, `DX_NO_VSYNC=1`,
  `DX_PACKAGE_RESIDENT=1` (keep whole packages in memory).
