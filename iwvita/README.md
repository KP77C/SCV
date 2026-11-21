# iwvita: Deus Ex: Invisible War on PS Vita

A decompilation of the USA Xbox release of *Deus Ex: Invisible War* (`Default.xbe`), with the aim of a native PS Vita port, in the style of the deltasquad project for *Republic Commando*.

No game files are in this repository. You need your own copy of the game.

## Status

Milestone 1 (map the code) is in progress. See [docs/findings.md](docs/findings.md).

## Tools

| Tool | Use |
|---|---|
| `tools/xiso.py <iso> <outdir>` | Extracts an Xbox XISO / XDVDFS image |
| `tools/xbe.py <xbe>` | Prints XBE header, libraries and sections |
| `tools/xbe2elf.py <xbe> <elf>` | Converts a retail XBE into an ELF32 i386 image for Ghidra (one segment per section, decoded entry point) |
| `tools/upkg.py <pkg>` | Minimal reader for the game's version 95 Unreal packages (`.d2u`) |
| `tools/xboxkrnl_ordinals.txt` | xboxkrnl export ordinals and names |
| `tools/ghidra/*.java` | Headless Ghidra scripts |

## Ghidra setup

```
python3 tools/xbe2elf.py game/Default.xbe Default.elf
analyzeHeadless <proj> iwxbox -import Default.elf -loader ElfLoader \
  -processor x86:LE:32:default -cspec windows -scriptPath tools/ghidra \
  -preScript XbeLabel.java tools/xboxkrnl_ordinals.txt 0x663720 0x22a07 \
  -postScript ExportFuncs.java funcs.csv
```

`XbeLabel.java` names the 158 kernel thunk slots (table at `0x663720`) and marks the entry point (`0x22a07`).
