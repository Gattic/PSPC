# Compatibility

Games tested with PSPC, newest results first. "Native" means the game's modules were built with
`tools/pspc_build.py` and ran as AOT DLLs; "interpreter" means `--interp`. Testing is mostly
headless (`--fast-time --dump-frames`, scripted input with `--press`), so "reaches" describes the
furthest screen confirmed by a frame dump.

| Game | Disc ID | Firmware req. | Native | Interpreter | Notes |
|---|---|---|---|---|---|
| Daxter | NPUG80329 | 5.50 | Gameplay (first level) | Gameplay | Intro movie plays with sound (sceMpeg, H.264 via Media Foundation). Renderer runs ~1.6x real time headless. |
| Syphon Filter: Combat Ops | NPUG80114 | 3.71 | Online match (two instances) | Menus | Intro, save-data notice, keyboard and menus render correctly. Audio plays. Infrastructure: connects to the access point ("PSPC 1"), signs in to PSN through PSPC Network (hosted account or local), and with PSPC's Combat Ops server (`server/pspc_medius`, docs/combat-ops-server.md) enters the lobby, lists and downloads missions, hosts and joins games; two instances on one PC go through staging, the briefing and setup into a deathmatch together. Ad hoc: two PSPC instances host and join a game through `pspc --adhoc-server` (staging area, 2/8 players). |
| Syphon Filter: Logan's Shadow | UCUS98606 | 3.52 | Gameplay (training, story episode 1) | Same | Main code lives in USRDIR PRX modules, loaded and run natively. Story cutscenes use sceMpeg (now implemented). |
| Monster Hunter Freedom Unite | ULUS10391 | 5.03 | Game menu, character creation | Same | Loads its font library through `sceKernelLoadModuleByID`; needs system fonts (`flash0/font`). Naming the character uses the keyboard dialog (windowed: a text box; headless: `--osk-text`). |
| Star Wars Battlefront: Elite Squadron | ULUS10390 | 5.03 | Profile prompt | Same | Title, in-engine space intro and menus render. |
| Star Wars Battlefront: Renegade Squadron | ULUS10292 | 3.51 | New-profile screen | Same | Menus render. |

## Test suite

pspautotests (hardware-recorded expectations), run with `tools/run_autotests.py`:

| Run | Passed |
|---|---|
| Interpreter | 517 / 559 |
| AOT (native) | 517 / 559, identical to the interpreter test by test |

Known not-passable tests include `cpu/vfpu/vregs` (the shipped `.prx` predates its `.expected`),
`cpu/cacheop` (needs a data-cache model), `gpu/simple/simple` (depends on the PSP data cache), and
several timing tests whose expected output records hardware jitter.

## Reporting a problem

Run the game with `--log info --log-file run.log`; on a crash pspc prints a symbolized backtrace
(AOT frames are named by guest function) to stderr. `--interp` tells recompiler problems from HLE
problems: a game that works interpreted but not natively points at the recompiler.
