# Boot path

Traced from the XBE entry point with `tools/ghidra/CallTree.java` and the decompiler. Names are applied in the Ghidra project; they are best guesses from strings and structure, matching UE2 naming where the role is clear.

```
XbeEntry 0x22a07                 XAPI start: creates the main thread at 0x22993
  0x22993                        XAPI/CRT init (0x24c4b, 0x244cf, 0x24bb7, 0x24b5f), then main(0,0,0)
  main 0x1cf00
    XboxMountDrives 0x223a6      \Device\Cdrom0, \Device\Harddisk0\Partition%d -> D:, T:, Z: ...
    0x3df10                      base dir "D:\System\"
    EarlyGetIniString 0x3e370    reads Z:\user.ini / ..\user.ini before Core is up
                                 (Engine.Engine CatchTopLevelExceptions)
    GuardedMain 0x19800          10 KB, mostly inlined code:
      appInit 0x26c4e0           command line, "Detected: Microsoft X-box", memory manager
                                 (GetMemoryManager 0x15970, Engine.Engine MemoryManager),
                                 UObject_StaticInit 0x24be90, PlayIntroMovies 0x313370
                                 (ShortIntroMovies / XboxStartup), appPlatformInitCPU 0x25c860,
                                 loading screen ("Loading1"), material categories.txt
      ~315 StaticClass registrations (every native class, inlined: if (!G) { G = Get(); Init(); })
      InitEngine 0x470ec0        StaticLoadClass(GameEngine, "ini:Engine.Engine.GameEngine"),
                                 StaticConstructObject, vtable slot 31 = UGameEngine_Init
      MainLoop 0x470ba0          while (!GIsRequestingExit): streaming, sound memory (Cadence),
                                 timing (rdtsc), vtable slot 30 = UGameEngine_Tick, fps counter
      appPreExit 0x269950        UObject_StaticExit 0x244ab0 ("Object subsystem successfully closed.")
    appExit 0x26b780             log flush, streaming thread wait, FName_StaticExit 0x268110
    RelaunchXbe 0x1c9c0          builds a command line ("?-CONTROLLER", map URL) and relaunches
                                 D:\DX2Release.xbe; the Xbox build reboots its own XBE between some loads
```

## Engine objects

| Class | vtable | Init (slot 31) | Tick (slot 30) |
|---|---|---|---|
| GameEngine | `0x697620` | `UGameEngine_Init` `0x2b11e0` (4.7 KB) | `UGameEngine_Tick` `0x2b69a0` |
| XboxClient | `0x68aca8` | `0x310350` | `0x21b650` |

`UGameEngine_Init` loads the render device (`ini:Engine.Engine.RenderDevice`) and the client, reads the tag database and motions (`BasePath`, `TagDatabase`, `MotionsFileName`), creates the AI, game and physics subsystems (`AISubsystem`, `GameSubsystem`, `PhysicsSubsystem`), parses the start URL (`FURL`) and opens the `MainMenu` map through the `Kernel` package (`0x35c100`).

## Order of work for "main menu on the host"

1. Core: memory manager, `FName`, `FString`/`TArray`, log, config cache, `UObject` (StaticInit, StaticConstructObject, StaticLoadClass/Object), linker for package version 95, `UClass`/`UProperty` serialisation.
2. Class registration: generate the ~315 `StaticClass` registrations from `symbols/classes.csv`.
3. Platform layer: replace `XboxMountDrives`, XAPI file I/O, intro movies (skip at first), `RelaunchXbe` (load the map in-process instead).
4. Engine: `UGameEngine_Init` with stub render device and client, then the menu map load and the menu UI code it calls.
