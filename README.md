# PSPC

PSPC runs PlayStation Portable software as native Windows programs. Game and homebrew code
(Allegrex MIPS) is **recompiled ahead of time into C++ and compiled into DLLs**, with an
interpreter as a fallback. The PSP OS, as of firmware 4.01 with the M33 custom firmware, is
**reimplemented natively**, using the decompiled firmware as its specification.

Read [PLAN.md](PLAN.md) for the goals and phases, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
and [docs/RECOMPILER.md](docs/RECOMPILER.md) for how it works.

## Status

Retail games boot and play natively (AOT) and in the interpreter: Daxter (intro movie and
gameplay), Syphon Filter: Logan's Shadow (story mode), Syphon Filter: Combat Ops, Monster Hunter
Freedom Unite, Star Wars Battlefront: Elite Squadron and Renegade Squadron - see
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). pspautotests: 517 of 559 pass, with identical
results in the interpreter and as native code.

| Area | State |
|---|---|
| Allegrex CPU: decoder, interpreter, FPU, VFPU (bit-exact hardware model) | Working. `cpu/` 40/42 (the other two can't pass, see docs) |
| AOT recompiler (`pspc-recomp`) + DLL pipeline | Working. 99%+ code coverage on retail games, whole discs build in about a minute |
| Kernel: threads (fibers), sync objects, memory, modules, interrupts, timing | Working. `threads/` 148/158; ctrl, power, RTC, intr, modules, sysmem all complete |
| GE graphics (multithreaded software renderer), display timing | Working. `gpu/` 107/111 |
| Audio (sceAudio, SAS, ATRAC3/3+, MP3, AAC, audiocodec) | Working. `audio/` 72/74 |
| I/O, UMD, memory stick, ISO/CSO/ZSO/DAX | Working |
| sceUtility (save data, dialogs, OSK), fonts (PGF), network (offline) | Working |
| M33 layer (SystemCtrl, KUBridge, VshCtrl, config.se, plugins, NID resolver) | Working |
| Video (sceMpeg, PSMF, psmfplayer, videocodec), JPEG | Working. H.264 via Media Foundation; `video/` 24/30, `jpeg/` 8/11; game movies play with sound |
| Firmware tooling (M33 extraction, inventory, NID database, Ghidra pipeline, pspc-decrypt) | Working |
| Real 4.01 XMB | Needs the official 4.01 update file (see below) |

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 with C++ tools, CMake, Ninja, LLVM (clang-cl,
for AOT builds), Python 3.12. SDL2 2.32.10 (VC dev package) in `third_party/SDL2-2.32.10`.

```bat
build.cmd                       :: configure + build into build\RelWithDebInfo
build\RelWithDebInfo\bin\pspc_unittests.exe
```

## Playing

Put your game dumps (ISO/CSO, or EBOOT.PBP folders) in `Games\` and start the library window:

```bat
build\RelWithDebInfo\bin\pspc-launcher.exe
```

Double-click (or Enter) plays a game; right-click > *Build native code* compiles it ahead of time
(about a minute per disc; otherwise it runs in the interpreter, which is slower but complete).
From the command line:

```bat
python -I tools\pspc_build.py Games\MyGame.iso     :: once per game: native code for every module
build\RelWithDebInfo\bin\pspc.exe Games\MyGame.iso
```

Keyboard: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R,
Enter = Start, Space = Select, Esc = Home, I/J/K/L = analog stick. Game controllers work through SDL.

Useful options (`pspc --help` lists them all):

| Option | |
|---|---|
| `--interp` | ignore native code, interpret everything |
| `--headless --fast-time` | no window or audio, skip idle time (tests, batch runs) |
| `--dump-frames <dir> --dump-every <n>` | save the displayed frame as BMP every n VBlanks |
| `--press <vblank>:<BUTTONS>[:<frames>]` | scripted input, e.g. `--press 600:START --press 900:CROSS+UP:4` |
| `--memstick <dir>`, `--flash0 <dir>`, `--host0 <dir>` | device roots |
| `--umd <image>` | insert a disc while running a .prx/.elf/.pbp |
| `--fw 6.60` | firmware version reported to games |
| `--timeout <s>`, `--log <level>`, `--log-file <path>` | |

Save data goes to `<pspc dir>\ms0\PSP\SAVEDATA`, and M33 settings to `<pspc dir>\flash1\config.se`.
Plugins listed in `ms0\seplugins\game.txt` are loaded the way M33 loads them. A game that chains
to another executable (`sceKernelLoadExec`) is restarted in a fresh runtime automatically.

**Game files:** use dumps of discs and downloads you own. Encrypted executables are decrypted by
`pspc-decrypt.exe`, which must sit next to `pspc.exe`. PSN titles need your own license files.

**Fonts:** games that draw text with the system fonts need `flash0\font\*.pgf`. They come with the
official firmware (below); until then any compatible PGF set placed in `<pspc dir>\flash0\font`
works. Without fonts that text is invisible but the game keeps running.

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
python -I tools\run_autotests.py                    :: pspautotests, interpreter
python -I tools\pspc_build.py @list.txt --modules-in-parallel 6 --jobs 3   :: test DLLs
python -I tools\run_autotests.py --aot              :: the same with AOT-compiled modules
```

Comparing the two runs is a differential test of the recompiler. On a fatal error or a host crash
pspc prints a symbolized backtrace (frames in AOT DLLs are named by guest function).

## License

PSPC's own code is GPL-2.0-or-later. Code ported from PPSSPP (GPL-2.0-or-later) is marked as such.
`tools/pspc-decrypt` is a separate GPL-3.0-or-later program (it uses libkirk) and is not linked
into PSPC. See [THIRD_PARTY.md](THIRD_PARTY.md).
