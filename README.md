# PSPC

PSPC runs PlayStation Portable software as native PC programs. Game and homebrew code
(Allegrex MIPS) is **recompiled ahead of time into C++ and compiled into DLLs (Windows) or `.so` modules (Linux)**, with an
interpreter as a fallback. The PSP OS, as of firmware 4.01 with the M33 custom firmware, is
**reimplemented natively**, using the decompiled firmware as its specification.

Read [PLAN.md](PLAN.md) for the goals and phases, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
and [docs/RECOMPILER.md](docs/RECOMPILER.md) for how it works.

## Installing

For Linux source builds, the OpenGL renderer and GTK launcher, see
[Linux build instructions and validation limits](docs/linux-port.md). Runtime builds also provide a
[relocatable Linux runtime archive/install](docs/linux-package.md) using host libraries, Python and
a compiler, with optional [per-user menu registration](docs/linux-package.md#application-menu-registration)
and a [fixture-free Linux CI gate](docs/linux-package.md#fixture-free-ci). The MSI and
bundled-toolchain instructions below are Windows-only.

1. Download `PSPC-<version>-x64.msi` from the [Releases page](https://github.com/Gattic/PSPC/releases)
   and open it. PSPC installs for your Windows account in `%LOCALAPPDATA%\Programs\PSPC`, without
   administrator rights, and adds **PSPC** to the Start menu and the desktop. (The installer isn't
   signed yet, so Windows may say it's from an unknown publisher: choose *More info*, then *Run
   anyway*.)
2. Start PSPC. Put your games (ISO, CSO or EBOOT.PBP dumps of games you own) in its Games folder,
   or click *Choose a folder...* where they already are.
3. Double-click a game to play it. Keyboard and controller controls are under *Settings*, and so
   are the renderer, the resolution (up to 8K with Direct3D 11) and texture upscaling.
4. **Online:** the sidebar's *Online* page runs PSPC Network (sign-in and friends, PSPC's
   PlayStation Network stand-in), the Syphon Filter: Combat Ops server, a GameSpy server (Star
   Wars Battlefront: Elite Squadron) and the ad hoc server on your PC with one click, for you and
   your friends, or points your games at someone else's (or, for GameSpy, at OpenSpy).
   Everything they need comes with PSPC.
5. Optional: the PSP's own menu (the XMB) needs Sony's firmware, which PSPC can't include. With
   Sony's PSP system software update 4.01 (its `EBOOT.PBP`), open **Firmware Manager** and
   choose **Build from update…**. The manager also rebuilds, switches and deletes named
   firmware profiles; **Boot XMB** starts the active one. See [Firmware Manager](docs/firmware-manager.md).
   That also
   brings the PSP's system fonts, which some games draw their text with (without them that text
   doesn't show).

Updating: open a newer MSI; it replaces the old version and keeps your settings, saves and server
accounts. Uninstalling (Windows Settings > Apps > PSPC) removes PSPC's programs and leaves what
PSPC created in its folder (settings, saves, firmware, server databases).

The installed PSPC includes everything needed for native (AOT) code. Choose *Build native code*
in the launcher, or let *Play* build it first. No Visual Studio, LLVM or Python installation is
needed. Native code is cached between runs; after a PSPC update the launcher offers to rebuild
outdated code. Games can also run in the interpreter.

## Status

The compatibility results below are not a cross-platform parity claim. For the current Linux
build/test milestone, genuine GTK/Sony XMB workflows and remaining limits, see
[Linux milestone status](docs/linux-port.md#current-linux-milestone). The port is still in
progress; Windows/D3D11 has not been rebuilt or retested after the Linux changes.

Retail games boot and play natively (AOT) and in the interpreter: Daxter (intro movie and
gameplay), Syphon Filter: Logan's Shadow (story mode), Syphon Filter: Combat Ops, Monster Hunter
Freedom Unite, Star Wars Battlefront II, Elite Squadron and Renegade Squadron - see
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). pspautotests: 539 of 559 pass (the other 20 are
recorded hardware differences), with identical results in the interpreter and as native code.

| Area | State |
|---|---|
| Allegrex CPU: decoder, interpreter, FPU, VFPU (bit-exact hardware model) | Working. `cpu/` 42/42 |
| AOT recompiler (`pspc-recomp`) + DLL pipeline | Working. 99%+ code coverage on retail games, whole discs build in about a minute |
| Kernel: threads (fibers), sync objects, memory, modules, interrupts, timing | Working. `threads/` 152/158; ctrl, power, RTC, intr, modules all complete |
| GE graphics: multithreaded software renderer (default, reference) and optional Direct3D 11 (Windows) and OpenGL (Linux) renderers: 1-16x resolution (up to 8K, supersampled to the window), xBR texture upscaling, texture filtering. Vertex work runs in parallel on worker threads; with Direct3D 11, transform, lighting and clipping run on the GPU, bit for bit as on the CPU ([docs/gpu-transform.md](docs/gpu-transform.md)) | Working. `gpu/` 111/111 (software) |
| Audio (sceAudio, SAS, ATRAC3/3+, MP3, AAC, audiocodec) | Working. `audio/` 73/74 |
| I/O, UMD, memory stick, ISO/CSO/ZSO/DAX | Working |
| sceUtility (save data, dialogs, OSK), fonts (PGF) | Working |
| Network: infrastructure (virtual access point on the host's network, BSD sockets, DNS, HTTP/HTTPS) and ad hoc play (PPSSPP-compatible ad hoc server, `pspc --adhoc-server`) | Working. [docs/networking.md](docs/networking.md) |
| PlayStation Network stand-in (PSPC Network: accounts, signed NP tickets, friend lists; redirects to revival servers, PPSSPP infra-dns.json compatible) | Working: Combat Ops signs in. [docs/pspc-network.md](docs/pspc-network.md), server in [server/](server/README.md) |
| Combat Ops game server (`server/pspc_medius`: SCE-RT/Medius lobby and game servers, SVO portal, mission sharing, leaderboards, cells, message board) | Working on one PC: instances meet in the lobby and play unranked and ranked matches; leaderboards and the message board show in the game. [docs/combat-ops-server.md](docs/combat-ops-server.md) |
| GameSpy server (`server/pspc_gamespy`: Presence, QR2 master server, server browser, NAT negotiation, stats, leaderboards) for Star Wars Battlefront: Elite Squadron | Working on one PC: accounts are created in the game, players log in, add friends, host and join matches; Player Rankings show. [docs/gamespy-server.md](docs/gamespy-server.md) |
| M33 layer (SystemCtrl, KUBridge, VshCtrl, config.se, plugins, NID resolver) | Working |
| Video (sceMpeg, PSMF, psmfplayer, videocodec), JPEG | Working. H.264 via Media Foundation; `video/` 24/30, `jpeg/` 8/11; game movies play with sound |
| Firmware tooling (M33 extraction, inventory, NID database, Ghidra pipeline, pspc-decrypt) | Working |
| Real 4.01 XMB (`pspc --vsh`) | Working with the official 4.01 update (see below): boots, navigates, lists and launches ISOs and EBOOT.PBP folders, returns from games; Saved Data Utility (including Copy), clock setting, Memory Stick Video MP4 playback; M33 VSH menu and recovery. [docs/vsh-boot.md](docs/vsh-boot.md) |

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 with C++ tools, CMake, Ninja, LLVM (clang-cl,
for AOT builds), Python 3.12. SDL2 2.32.10 (VC dev package) in `third_party/SDL2-2.32.10`, which
`tools\fetch_deps.ps1` downloads from the official libsdl-org release.

```bat
powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1   :: once: SDL2 into third_party\
build.cmd                       :: configure + build into build\RelWithDebInfo
build\RelWithDebInfo\bin\pspc_unittests.exe
```

For release packaging, run `python -I tools\fetch_aot.py` once to download the pinned, SHA-256
verified LLVM/MinGW toolchain. Then `python -I tools\make_installer.py` (Python 3.12: it uses
`msilib`, gone in 3.13) packages `build\RelWithDebInfo` into
`build\installer\PSPC-<version>-x64.msi`. Use `--build-dir build\Release` for a Release tree.
Every installer includes the recompiler, portable compiler, matching AOT SDK and private Python
runtime for builds and servers. Packaging checks that native code builds and runs using only
the installed tools, with no developer tools on PATH. The version is the one in `CMakeLists.txt`.

## Playing

Put your game dumps (ISO/CSO, or EBOOT.PBP folders) in `Games\` and start the library window:

```bat
build\RelWithDebInfo\bin\pspc-launcher.exe
```

Double-click (or Enter) plays a game. The panel on the right shows the selected game with *Play*,
*Build native code* (compiles it ahead of time, about a minute per disc; otherwise it runs in the
interpreter, which is slower but complete) and *Interpreter*; Ctrl+F searches the library, and the
sidebar has Boot XMB, Plugins, Settings, the Online page (servers and sign-in) and your PSPC
Network account.
From the command line:

```bat
python -I tools\pspc_build.py Games\MyGame.iso     :: once per game: native code for every module
build\RelWithDebInfo\bin\pspc.exe Games\MyGame.iso
```

In an installed copy, the equivalent commands (from its folder) are:

```bat
python\python.exe -I -B -X utf8 tools\pspc_build.py Games\MyGame.iso
pspc.exe Games\MyGame.iso
```

Keyboard: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L/R,
Enter = Start, Space = Select, Esc = Home, I/J/K/L = analog stick, Page Up/Page Down = VOL+/VOL-,
N = Note, B = Screen (the last four are for plugins; games never see them). Game controllers work
through SDL; both sticks drive the PSP analog stick, and clicking them is VOL- (left) and VOL+ (right).
Home (Esc, or the controller's guide button) brings up the PSP's "Quit the game?" screen; the
sceUtility dialogs (messages, save/load screens, on-screen keyboard) are drawn over the game as
on a PSP and answered with the PSP buttons.

Useful options (`pspc --help` lists them all):

| Option | |
|---|---|
| `--interp` | ignore native code, interpret everything |
| `--headless --fast-time` | no window or audio, skip idle time (tests, batch runs) |
| `--dump-frames <dir> --dump-every <n>` | save the displayed frame as BMP every n VBlanks |
| `--press <vblank>:<BUTTONS>[:<frames>]` | scripted input, e.g. `--press 600:START --press 900:CROSS+UP:4` |
| `--memstick <dir>`, `--flash0 <dir>`, `--host0 <dir>` | device roots |
| `--plugins <dir>`, `--no-plugins` | M33 plugins folder laid over ms0: (default: `plugins`), or none |
| `--umd <image>` | insert a disc while running a .prx/.elf/.pbp |
| `--renderer software\|opengl\|d3d11`, `--render-scale <1-16>` | Software (default), OpenGL (Linux) or D3D11 (Windows); GPU renderers support 1-16 times 480x272 (8 = 4K, 16 = 8K) |
| `--texture-upscale <1-4>`, `--texture-filter auto\|nearest\|linear` | GPU textures: scaled up with xBR; filtering as the game sets it, or forced |
| `--osk-text <text>` | headless: what the on-screen keyboard returns (e.g. a character name) |
| `--config <file\|none>`, `--set key=value` | settings file to use (default `pspc.ini`), or override one entry |
| `--fw 6.60` | firmware version reported to games |
| `--np register\|signin\|status\|friends\|...` | PSPC Network account (see Network below) |
| `--vsh`, `--recovery` | boot the installed firmware's XMB, or M33's recovery menu, instead of a file |
| `--timeout <s>`, `--log <level>`, `--log-file <path>` | |

### Settings

pspc.exe reads `pspc.ini` from its own folder at startup; command-line options override it. The
launcher's Settings button edits it, or edit it by hand:

```ini
scale=2            # window scale 1-4
fullscreen=0       # 1 = borderless fullscreen
vsync=1
aot=1              # 0 = interpreter only (like --interp)
fw=6.60            # firmware version reported to games (like --fw)
renderer=d3d11     # software (default), opengl (Linux: GL 4.5 + fragment interlock), d3d11 (Windows: D3D11.3 ROVs)
render_scale=8     # d3d11 internal resolution, 1-16 times 480x272: 4 = 1920x1088, 8 = 3840x2176 (4K),
                   # 16 = 7680x4352 (8K); above the window's size it is averaged down (supersampling)
texture_upscale=4  # d3d11: 1 = off, 2-4 = textures scaled up with xBR (smooth edges instead of blocky texels)
texture_filter=auto  # d3d11: auto (as the game sets it), nearest or linear
volume=100         # 0-100 (volume=mute or mute=1 silences)
key.cross=Z        # SDL key names; several keys: key.select=Space,Right Shift
pad.cross=a        # SDL controller buttons: a b x y back guide start leftstick rightstick
                   # leftshoulder rightshoulder dpup dpdown dpleft dpright lefttrigger righttrigger
pad.stick=both     # stick(s) that drive the PSP analog stick: both, left, right or none
plugins=1          # load M33 plugins (see Plugins below)
plugins_dir=       # folder laid over the memory stick; empty = the plugins folder
network=1          # 0 = offline: the WLAN switch reads off
adhoc_server=localhost  # ad hoc server (host or host:port) to meet other players through
np_server=         # PSPC Network server (PSN stand-in); empty = games sign in locally as np_online_id
np_online_id=      # your online ID; np_token= is written when you sign in (keep it private)
infra_dns=         # optional revival DNS server for every game's online services
```

Inputs: up down left right cross circle square triangle l r start select home vol_up vol_down note screen analog_up
analog_down analog_left analog_right. While playing: **F11** fullscreen, **F12** screenshot
(`screenshots\<game>_<date>_<time>.bmp`), **Ctrl+Q** quit.

The launcher tags games whose native code is built (NATIVE), partly built (PARTIAL) or out of date
(REBUILD); Play offers to build it first (`tools\pspc_build.py` records builds in
`cache\aot\games.json`).

Save data goes to `<pspc dir>\ms0\PSP\SAVEDATA`, and M33 settings to `<pspc dir>\flash1\config.se`.
A game that chains to another executable (`sceKernelLoadExec`) is restarted in a fresh runtime
automatically.

### Network and ad hoc play

Games and the XMB are online through the host's network: connect with the network configuration
"PSPC 1" when asked. For ad hoc multiplayer run an ad hoc server and start the game in two
instances (or on two PCs, with `adhoc_server=` set to the server PC's address):

```bat
build\RelWithDebInfo\bin\pspc.exe --adhoc-server --adhoc-bind 127.0.0.1   :: two instances on this PC
build\RelWithDebInfo\bin\pspc.exe --adhoc-server                          :: LAN (allow it through the firewall)
```

It speaks PPSSPP's protocol, so PPSSPP players and public PPSSPP servers work too. See
[docs/networking.md](docs/networking.md) (LAN setup, the XMB's browser and Network Update, limits).

**PlayStation Network.** Games that sign in to PSN (Syphon Filter: Combat Ops) sign in to
*PSPC Network* instead: accounts, signed tickets and friend lists on a server anyone can run
([server/README.md](server/README.md)). Click the account at the bottom of the launcher's sidebar
(**Network**), enter the server's address, and create an account or sign in; the same dialog manages friends. Without a server,
games sign in locally under the name you choose. Games' old server names can be pointed at
community revival servers with an `infrastructure.json` (PPSSPP's `infra-dns.json` format works
too). See [docs/pspc-network.md](docs/pspc-network.md). Combat Ops has a server of its own in this
repository: [docs/combat-ops-server.md](docs/combat-ops-server.md).

**GameSpy.** Games that went online through GameSpy (Star Wars Battlefront: Elite Squadron) use
the GameSpy server in this repository ([docs/gamespy-server.md](docs/gamespy-server.md)): accounts
made in the game, friends, hosting and joining matches, and leaderboards. The launcher's *Online*
page runs it, or points the game at another one or at OpenSpy, the public GameSpy revival.

```bat
build\RelWithDebInfo\bin\pspc.exe --np register --server https://np.example.org --id Gabe_Logan
build\RelWithDebInfo\bin\pspc.exe --np friend-add --id Lian_Xing
```

### Plugins

M33 plugins (cheat devices such as DarkFrost or CWCheat, screenshot and button-remap plugins) run as
on a PSP with 4.01 M33. The `plugins` folder is laid over the memory stick, so put them there as you
would in `ms0:/` and list them in `plugins\seplugins\game.txt` (games), `vsh.txt` (XMB) or
`pops.txt` (PS1). Listed plugins load when the game starts: no Pandora battery or recovery menu.
**Plugins** in the launcher's sidebar turns them on and off and adds new ones. See
[plugins/README.md](plugins/README.md) and [docs/plugins.md](docs/plugins.md).

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
python -I tools\pspc_install_fw.py --profile 4.01  :: flash0/flash1 + native code for the XMB
build\RelWithDebInfo\bin\pspc.exe --set firmware=4.01 --vsh            :: the real XMB (or "Boot XMB" in the launcher)
```

UMD images in `ms0\ISO` show under Game > Memory Stick; SELECT opens M33's VSH menu, `--recovery`
boots M33's recovery menu. See [docs/vsh-boot.md](docs/vsh-boot.md) and
[docs/firmware-tooling.md](docs/firmware-tooling.md).

## Testing

```bat
python -I tools\run_autotests.py                    :: pspautotests, interpreter
python -I tools\pspc_build.py @list.txt --modules-in-parallel 6 --jobs 3   :: test DLLs
python -I tools\run_autotests.py --aot              :: the same with AOT-compiled modules
```

Comparing the two runs is a differential test of the recompiler. On a fatal error or a host crash
pspc prints a symbolized backtrace (frames in AOT DLLs are named by guest function).

Games and PSPC's own guest programs:

```bat
python -I tools\smoke_games.py --record             :: once: this machine's checkpoint frames (Games\ folder)
python -I tools\smoke_games.py                      :: the games still reach their checkpoints, frame for frame
python -I tools\guest_tests.py                      :: tests/guest programs, interpreted and freshly compiled
python -I tools\test_mkcso.py                       :: the CSO/ZSO/DAX writer's index shift and limits
python -I tools\large_image_test.py --aot           :: boots an 8.75 GB sparse disc image
```

The guest programs are built with pspdev by `tools/build_guest_tests.sh` (under Linux or WSL).
Checkpoints are listed in `tools/checkpoints.txt`; the reference frames stay on your machine
(`build/checkpoints`). Disc images past 1.8 GB: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md#large-disc-images).

Interpreter throughput, high-resolution GPU benchmarks and AOT rendering checks:
[docs/performance.md](docs/performance.md).

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

PSPC's own code is GPL-2.0-or-later. Code ported from PPSSPP (GPL-2.0-or-later) is marked as such.
`tools/pspc-decrypt` is a separate GPL-3.0-or-later program (it uses libkirk) and is not linked
into PSPC. See [THIRD_PARTY.md](THIRD_PARTY.md).
