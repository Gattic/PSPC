# PSPC

PSPC runs PlayStation Portable software as native Windows programs. Game and homebrew code
(Allegrex MIPS) is **recompiled ahead of time into C++ and compiled into DLLs**, with an
interpreter as a fallback. The PSP OS, as of firmware 4.01 with the M33 custom firmware, is
**reimplemented natively**, using the decompiled firmware as its specification.

Read [PLAN.md](PLAN.md) for the goals and phases, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
for how it works.

## Status

| Area | State |
|---|---|
| Allegrex CPU: decoder, interpreter, FPU, VFPU | Working. Validated against pspautotests `cpu/` |
| AOT recompiler (`pspc-recomp`) + DLL pipeline | Working. `cpu/cpu_alu` runs 100% native with output identical to hardware |
| Kernel: threads (fibers), sync objects, memory, modules, interrupts | Working. Being completed against `threads/` and `sysmem/` |
| Display, controller, power, RTC, UMD, I/O, ISO/CSO | Working / being completed |
| GE graphics (software renderer) | In progress |
| Audio (sceAudio, SAS, ATRAC3/3+, MP3) | In progress |
| sceUtility (save data, dialogs), network (offline) | In progress |
| M33 layer (SystemCtrl, KUBridge, VshCtrl, config.se, plugins, NID resolver) | Working |
| Firmware tooling (M33 extraction, inventory, NID database, Ghidra pipeline, pspc-decrypt) | Working |
| Real 4.01 XMB | Needs the official 4.01 update file (see below) |

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 with C++ tools, CMake, Ninja, LLVM (clang-cl,
for AOT builds), Python 3.12. SDL2 2.32.10 (VC dev package) in `third_party/SDL2-2.32.10`.

```bat
build.cmd                       :: configure + build into build\RelWithDebInfo
build\RelWithDebInfo\bin\pspc_unittests.exe
```

## Running

```bat
build\RelWithDebInfo\bin\pspc.exe Games\MyHomebrew\EBOOT.PBP
build\RelWithDebInfo\bin\pspc.exe Games\MyGame.iso
```

Keyboard: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R,
Enter = Start, Space = Select, Esc = Home, I/J/K/L = analog stick. Game controllers work through SDL.

Before first launch, build the game's native code (otherwise it runs in the interpreter):

```bat
python -I tools\pspc_build.py Games\MyHomebrew\EBOOT.PBP
```

Options: `pspc --help`. Save data goes to `<pspc dir>\ms0\PSP\SAVEDATA`, and M33 settings to
`<pspc dir>\flash1\config.se`. Plugins listed in `ms0\seplugins\game.txt` are loaded the way M33
loads them.

**Game files:** use dumps of discs and downloads you own. Encrypted executables are decrypted by
`pspc-decrypt.exe`, which must sit next to `pspc.exe`. PSN titles need your own license files.

## Firmware (optional)

The XMB and firmware-provided resources (system fonts, dialogs as Sony draws them) need Sony's
official 4.01 system update (`401.PBP`). It is not included and is never downloaded by PSPC. If you
have it:

```bat
build\RelWithDebInfo\bin\pspc-decrypt.exe psar 401.PBP private\fw\401 --model 02g
```

See [docs/firmware-tooling.md](docs/firmware-tooling.md).

## Testing

```bat
python -I tools\run_autotests.py            :: pspautotests, interpreter
python -I tools\run_autotests.py --aot cpu/ :: using AOT-compiled modules
```

## License

PSPC's own code is GPL-2.0-or-later. Code ported from PPSSPP (GPL-2.0-or-later) is marked as such.
`tools/pspc-decrypt` is a separate GPL-3.0-or-later program (it uses libkirk) and is not linked
into PSPC. See [THIRD_PARTY.md](THIRD_PARTY.md).
