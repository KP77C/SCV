# Findings

## Executable

| | |
|---|---|
| File | `Default.xbe` (the disc also has an identical-size `DX2Release.xbe`) |
| Original name | `DX2Release.exe` (`h:\dx2builds\dx2\build\dx2\xbox\release\`) |
| Build date | 2003-10-04 |
| Title ID | `0x45530005` |
| XDK | 5558 (XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, LIBC, LIBCP) |
| Base | `0x10000`, entry `0x22a07` (XOR key `0xA8FC57AB`) |
| Kernel thunks | `0x663720`, 158 imports (XOR key `0x5B6D40B6`) |
| `.text` | `0x12000`, 5.9 MB virtual |
| Other code sections | `D3D`, `D3DX`, `XGRPH`, `DSOUND`, `XPP`, Bink (`BINK*`, statically linked) |
| RTTI | 14 type names only |

The launcher file `Media/xboxlaunch.ini` lists Thief: Deadly Shadows (`T3`) maps next to the DX2 ones: both games share the Flesh engine.

## Size

First Ghidra pass (no missed-function sweep yet): 23,262 functions, 21,434 of them in `.text` (4.37 MB).

| Range in `.text` | Contents | Functions | Bytes |
|---|---|---|---|
| `0x12000`-`0x470000` | Engine and game | 17,025 | 3,459,629 |
| `0x470000`-`0x480000` | CppUnit-style unit tests (not needed) | 276 | 47,236 |
| `0x480000`-`0x4d0000` | Havok 2.1.2 (`Tt*`, `hk*`) | 1,040 | 279,345 |
| `0x4d0000`-`0x500000` | qhull (public source) | 742 | 177,565 |
| `0x500000`-`0x510000` | C++ runtime (demangler etc.) | 235 | 53,903 |
| `0x510000`-`0x5c0000` | Engine and game (block loading, TriggerScript, effects, physics glue) | 2,016 | 345,973 |

The ranges are rough (from referenced strings per 64 KB) and will be refined.

For comparison, Republic Commando (deltasquad) counted 12,356 functions / 2.7 MB. The PC build `DX2Main.exe` (2004-02-09, D3D8, no exports) has 30,817 functions / 4.4 MB in Ghidra's first pass.

## Packages

All `.d2u` packages are Unreal package version 95, licensee 69 (Republic Commando is 159). They hold almost no UnrealScript code:

| Package | Function exports |
|---|---|
| Core | 161 |
| Editor | 31 |
| Engine | 2 |
| DX2Game | 1 |
| AICore, DX2AI, DX2Gamesys, Fire | 0 |

`DX2Gamesys.d2u` holds 2,554 data-only classes (archetypes). Gameplay logic is C++, plus Ion Storm's own TriggerScript (`Content/DX2/TriggerScript.zip`). Only the Core natives are named by string (`intUObjectexecIsA` style, 254 entries).

## Data

Maps are Ion Storm formats, not `.unr`: 50 `.gmp` plus 62 `.xbd` on Xbox (603 MB), `.ibd` on PC (1.2 GB). Textures and static meshes ship as `.zip`. Videos are Bink (`VideoTextures`, 255 MB on Xbox).
