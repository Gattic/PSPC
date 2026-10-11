# Compatibility

Games tested with PSPC, newest results first. "Native" means the game's modules were built with
`tools/pspc_build.py` and ran as AOT DLLs; "interpreter" means `--interp`. Testing is mostly
headless (`--fast-time --dump-frames`, scripted input with `--press`), so "reaches" describes the
furthest screen confirmed by a frame dump.

| Game | Disc ID | Firmware req. | Native | Interpreter | Notes |
|---|---|---|---|---|---|
| Daxter | NPUG80329 | 5.50 | Gameplay (first level) | Gameplay | Intro movie plays with sound (sceMpeg, H.264 via Media Foundation). Renderer runs ~1.6x real time headless. |
| Syphon Filter: Combat Ops | NPUG80114 | 3.71 | Online match (two instances) | Menus | Intro, save-data notice, keyboard and menus render correctly. Audio plays. Infrastructure: connects to the access point ("PSPC 1"), signs in to PSN through PSPC Network (hosted account or local), and with PSPC's Combat Ops server (`server/pspc_medius`, docs/combat-ops-server.md) enters the lobby, lists and downloads missions, hosts and joins games; two instances on one PC go through staging, the briefing and setup, play a full deathmatch together, rate the mission and return to the lobby; four play a ranked deathmatch whose results and stats reach the server. Cells, the Community pages (leaderboards, message board) and the game's own profile screens work. Ad hoc: two PSPC instances host and join a game through `pspc --adhoc-server` (staging area, 2/8 players). |
| Syphon Filter: Logan's Shadow | UCUS98606 | 3.52 | Gameplay (training, story episode 1) | Same | Main code lives in USRDIR PRX modules, loaded and run natively. Story cutscenes use sceMpeg (now implemented). |
| Monster Hunter Freedom Unite | ULUS10391 | 5.03 | Game menu, character creation | Same | Loads its font library through `sceKernelLoadModuleByID`; needs system fonts (`flash0/font`). Naming the character uses the keyboard dialog (windowed: a text box; headless: `--osk-text`). |
| Star Wars Battlefront: Elite Squadron | ULUS10390 | 5.03 | Online match (two instances) | Profile prompt | Title, in-engine space intro and menus render. Infrastructure, with PSPC's GameSpy server (`server/pspc_gamespy`, docs/gamespy-server.md): creates a GameSpy account and logs in, hosts a Conquest match that a second instance finds and joins (team selection), adds a friend, shows Player Rankings. Joining needs both instances in real time (not `--fast-time`). |
| Star Wars Battlefront: Renegade Squadron | ULUS10292 | 3.51 | New-profile screen | Same | Menus render. |

## Test suite

pspautotests (hardware-recorded expectations), run with `tools/run_autotests.py`:

| Run | Passed |
|---|---|
| Interpreter | 539 / 559, the other 20 known differences |
| AOT (native) | 539 / 559, identical to the interpreter test by test |

The runner's gate is "0 unexpected failures": the 20 tests whose recording pspc can't reproduce
(hardware jitter, the recording PSP's uptime and memory map, modules the suite doesn't ship,
behaviour the tests don't pin down) are listed with their reasons in its KNOWN_DIFFERENCES and
reported apart. A known difference that starts passing is reported so it can come off the list.

The expected output was recorded on PSPs, some with a screen and with psplink's host0:. Where a
test's recording differs from what pspc gives a test by default, `run_autotests.py` reproduces it
(its ENVIRONMENT table: the screen path for `gpu/simple/simple`, `io/open/tty0`, `font/fonttest`
and `jpeg/mjpegcsc`, a generated disc for the `umd/` tests, the file date `io/file/file`
expects, psplink's host0: names and the recording folder's build products for `io/io/io`, the
recording PSP's extended user memory for `threads/threads/create`).
Two hardware details that games must not see are modelled only on request, in the interpreter:
VRAM's swizzled mirrors (`--edram-mirrors`, for `gpu/ge/edramswizzle`) and the data cache
(`--dcache-model`, for `cpu/lsu/cacheop`).
`cpu/vfpu/vregs` ships a .prx older than its .expected: `tools/rebuild_autotests.sh` rebuilds it
with pspdev (under Linux or WSL) into `build/autotest-rebuilt`, and the runner uses that one (it
skips the test until then). Font tests need the firmware fonts: run it with `--flash0` pointing
at an installed flash0. The investigation behind all of this: [autotests-plan.md](autotests-plan.md).

## Large disc images

A UMD holds at most 1.8 GB (dual layer), but nothing in PSPC depends on that, so images of
patched or extended games can be bigger:

| | Limit |
|---|---|
| ISO, CSO, ZSO image | 8 TB (the PSP counts sectors in 32 bits) |
| DAX image | under 4 GB (its header has a 32-bit size) |
| One file on the image | under 4 GB (ISO 9660). A file split into extents for 4 GB and more (ISO level 3) shows only its first extent, with a warning in the log. |
| Seeks inside a file | 64-bit with `sceIoLseek`; `sceIoLseek32` takes and returns an `int`, as on a PSP |

Recommended limits:

- **PSPC** enforces none.
- **Images that should also run on a PSP or in PPSSPP:** at most 4 GB - 1 byte. That is the most
  a FAT32 memory stick holds, and PPSSPP reads data past 4 GB of an image from the wrong place.
  The launcher notes it on bigger images.
- **Mods:** every game was written for 1.8 GB, and some do byte arithmetic on disc positions in
  32 bits. Keep what a game addresses by byte offset (its archives, when it reads them through
  `umd0:` or `/sce_lbn`) below 2 GB in the image, and add bulk data after it.

A CSO or ZSO bigger than 2 GB needs an index shift of at least 1. maxcso and `tools/mkcso.py`
choose one; an image made without one is refused with a message saying so. The memory stick's
`ISO` folder (for the XMB) takes images of any size, unlike a FAT32 stick.

Tests: `fs_large_*` in `pspc_unittests` (images of up to 8 GB in memory), `tools/test_mkcso.py`,
and `tools/large_image_test.py`, which boots an 8.75 GB sparse image (`tools/make_big_iso.py`,
under 1 MB of disk) and checks what the guest program `tests/guest/large_image` reads past 2, 4
and 8 GB, in the interpreter and with `--aot`. That program is built with pspdev by
`tools/build_guest_tests.sh` (under Linux or WSL), and the test skips until it is. The plan behind
this: [large-iso-plan.md](large-iso-plan.md).

## Reporting a problem

Run the game with `--log info --log-file run.log`; on a crash pspc prints a symbolized backtrace
(AOT frames are named by guest function) to stderr. `--interp` tells recompiler problems from HLE
problems: a game that works interpreted but not natively points at the recompiler.
