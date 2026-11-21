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
| `0x470000`-`0x480000` | CppUnit-style unit tests, plus the launch code (`InitEngine`, `MainLoop`, see [boot.md](boot.md)) | 276 | 47,236 |
| `0x480000`-`0x4d0000` | Havok 2.1.2 (`Tt*`, `hk*`) | 1,040 | 279,345 |
| `0x4d0000`-`0x500000` | qhull (public source) | 742 | 177,565 |
| `0x500000`-`0x510000` | C++ runtime (demangler etc.) | 235 | 53,903 |
| `0x510000`-`0x5c0000` | Engine and game (block loading, TriggerScript, effects, physics glue) | 2,016 | 345,973 |

The ranges are rough (from referenced strings per 64 KB) and will be refined.

For comparison, Republic Commando (deltasquad) counted 12,356 functions / 2.7 MB. The PC build `DX2Main.exe` (2004-02-09, D3D8, no exports) has 30,817 functions / 4.4 MB in Ghidra's first pass.

## Missed functions

`tools/ghidra/FindMissed.java` adds functions at code pointers in the data sections and at 16-byte aligned starts after `CC`/`90` padding. The XBE marks every section executable, so `FixBlocks.java` first clears the execute flag on `.rdata` and later (`xbe2elf.py` now does this on import).

After the sweep there are 54,673 functions and 5.30 MB of the 6.04 MB `.text` is inside functions (88%, from 72%).

| Range | Functions | Bytes | Under 16 bytes |
|---|---|---|---|
| Engine and game (`0x12000`-`0x470000`, `0x510000`-`0x580000`) | 32,150 | 4,467,951 | 9,094 |
| Unit tests and launch code | 600 | 55,148 | 174 |
| Havok | 1,727 | 299,323 | 406 |
| qhull | 833 | 184,521 | 74 |
| C++ runtime | 437 | 58,390 | 146 |
| `0x580000`-`0x5d3980`: mostly SEH unwind funclets (`LEA ECX,[EBP-x]; JMP dtor`) and XAPI | 17,097 | 232,497 | 14,283 |

The unwind funclets are compiler generated and need no source. Leaving out functions under 16 bytes (accessors and thunks), the engine and game come to about 23,000 functions and 4.4 MB, against Republic Commando's 12,356 functions and 2.7 MB.

## Natives

`tools/ghidra/NameNatives.java` names the natives listed as `{"int<Class>exec<Name>", function}` pairs in `.data`: 253, all `UObject` (`symbols/core-natives.txt`). No other class registers natives this way, which fits the packages having almost no script code.

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

## Classes

Native classes register through one UClass constructor at `0x2570b0` (args: ..., size, flags, ..., `"Name"`, `"System"`, class flags, InternalConstructor, StaticConstructor). Each class has a getter that builds it (`<Name>_GetPrivateStaticClass`), a small `StaticClass` function of the form `if (!G) { G = Get("Package"); Init(); }`, and an InternalConstructor that jumps to the C++ constructor, which stores the vtable.

`tools/ghidra/FindClasses.java` finds 266 classes (`symbols/classes.csv`): Engine 157, DX2Game 29, AICore 26, DX2AI 6, XboxDrv 2, plus Core (package name not a plain string), D3DDrv, Editor, GamePhysics and DX2GamePhysics. `tools/ghidra/ClassVtables.java` recovers 237 constructors and vtables (`symbols/vtables.csv`). The base class column is the first call in the constructor and is wrong where LTCG inlined the parent constructor. Actor-derived vtables have 93 slots.

The Ghidra project with these names applied is kept outside the repo (K's project files, `iwvita/ghidra/`).
