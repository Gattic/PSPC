# Booting the real 4.01 XMB (`pspc --vsh`)

PSPC boots Sony's own 4.01 VSH (the XMB) on top of its native kernel: `vshmain`, `paf`,
`common_gui`, `common_util` and the `*_plugin` modules run as guest code (AOT DLLs when built,
otherwise interpreted), exactly as they ship. Every kernel module, including `vshbridge.prx`, and
M33's `systemctrl`/`vshctrl` stay native. M33's user-mode UI (the VSH menu `satelite.prx` and
`recovery.prx`) runs as guest code too.

The map for this work was `reference/ppsspp/docs/VSHBootInvestigation.md` (a PPSSPP branch that
boots the VSH by running the real `kd/` drivers as MIPS code). PSPC takes the other road the PLAN
prescribes: no Sony kernel code runs, so none of that document's driver/MMIO workarounds apply.

## Quick start

```bat
:: once: decrypt the update and the M33 installer (docs/firmware-tooling.md)
pspc-decrypt.exe psar 401.PBP private\fw\401 --model 02g
python -I tools\m33extract.py reference\401M33\UPDATE\EBOOT.PBP -o private\fw\401\m33
:: once per PSPC build tree: copy flash0, prepare flash1, compile the firmware modules
python -I tools\pspc_install_fw.py --build-dir build\RelWithDebInfo
:: boot it (or "Boot XMB" in pspc-launcher)
build\RelWithDebInfo\bin\pspc.exe --vsh
```

UMD images (`.iso/.cso/.zso/.dax`) in `<pspc dir>\ms0\ISO` appear under Game > Memory Stick (a
hard link works: `mklink /H ms0\ISO\game.iso Games\game.iso`). `--umd <image>` puts a disc in the
drive instead. SELECT opens M33's VSH menu; `--recovery` (or R held while the XMB boots) starts
M33's recovery menu.

Headless check, as used during development:

```bat
pspc --vsh --headless --fast-time --timeout 60 --dump-frames out --dump-every 60 --press 600:LEFT --press 720:DOWN
```

The XMB is up at about VBlank 300 (the Sony Computer Entertainment logo and the wave come
first). A full frame costs about 7.5 ms in the software GE renderer; the CPU side is negligible
with AOT DLLs.

## The install step: `tools/pspc_install_fw.py`

1. Copies `flash0/{vsh,font,dic,data,kd}` of the decrypted firmware into `<pspc dir>\flash0`. The
   kernel modules are never run, but `kd/pspbtcnf_02g.bin` says which modules boot the VSH and
   the XMB loads some drivers by path (they become native placeholders, see below).
2. Copies M33's `satelite.prx` and `recovery.prx` into `flash0/vsh/module`, as M33's installer does.
3. Creates `<pspc dir>\flash1\{registry,vsh\theme,dic,net\http,gps}`. `flash1:/registry/init.dat`
   must not exist (the VSH reads it as "reset the registry"); a stray one is moved aside.
4. Runs `tools/pspc_build.py` on every user-mode module of `flash0/vsh/module` plus the two M33
   modules (85 modules, about 2 minutes on 14 cores; `--aot-core` builds only the 16 the XMB needs
   to start, in about a minute).

The files go only into the (git-ignored) build output; nothing from the firmware enters the
repository.

## Boot design (`src/runtime/vsh.cpp`)

`pspc --vsh` runs no file of its own (`runtime::Options::vsh`). `RunOnce` sets M33's apitype to
VSH (so `ms0:/seplugins/vsh.txt` plugins load), empties the UMD drive unless `--umd` was given,
puts the ISO browser in front of `ms0:` and calls `vsh::Boot()`, which starts a kernel thread
(`SceKernelInitThread`, priority 0x20). On that thread, like `init.prx`:

* the module list of pspbtcnf's VSH mode (flag 0x01) is read from
  `flash0:/kd/pspbtcnf_02g.bin` (the 4.01 list is built in as a fallback);
* kernel modules (and the user libraries PSPC implements natively: `usersystemlib`,
  `libatrac3plus`) are skipped; `heaparea1`, `paf`, `common_gui`, `common_util` and `vshmain`
  are loaded through the normal module loader and started **one at a time, each `module_start`
  waited for** before the next module loads.

Waiting is what removes the investigation's scePaf heap-pool patch: `heaparea1`'s
`module_start` has filled its pool pointers before `paf` or `vshmain` run. `vshmain`'s
`module_start` creates `SCE_VSH_GRAPHICS` (the VSH main thread) and returns; the init thread then
exits. From there the VSH loads its plugins itself (`opening_plugin`, `impose_plugin`,
`game_plugin`, `sysconf_plugin`...) through `vshKernelLoadModuleVSH`.

Module loading policy for what the VSH loads by path:

| What | Result |
|---|---|
| user modules (`flash0:/vsh/module/*`, `flash0:/kd/libasfparser.prx`...) | guest code, AOT when built |
| kernel modules from `flash0:/kd` (`usbstor.prx`, `mlnbridge.prx`...) | native placeholder (`IsFirmwareKernelModule`, modulemgr.cpp) |
| firmware libraries PSPC implements (`mpeg_vsh.prx` = `sceMpegVsh_library`...) | native placeholder (`kNativeModules`) |

## HLE added for the VSH

* **sceVshBridge** (`hle/modules/vsh_bridge.cpp`). `vshbridge.prx` exports 144 functions; each
  checks the caller (user level 4, k1) and calls one kernel function with the same arguments.
  Disassembling every export the VSH modules import (80 NIDs) gave that kernel function's 4.01
  NID; `data/m33_nid_resolver.csv` names it through its 1.50 NID. The table forwards to PSPC's
  implementation of that function (controller, display, devctl/ioctl, impose, model, RTC alarm,
  hprm, chkreg, vaudio), implements the module loader and LoadExec entries natively, and answers
  the rest (TV out, ME power, MagicGate memory stick audio/video, USB storage) with no-ops or a
  clean error.
* **The controller read** goes through a kernel-mode path (`ctrl::ReadBufferPositiveKernel`):
  nothing is masked, and the memory-stick-present bit (0x02000000, plus 0x01000000 for a disc) is
  set, as the kernel read reports it. M33's VSH menu relies on it.
* **sceResmgr_9DC14891** decrypts `flash0:/vsh/etc/index.dat` (KIRK, tag 0x0B2B11F0 on 4.01
  02g). The work happens in `pspc-decrypt` (GPL-3, never linked), which now accepts the
  `PSPsysGP` container (`FileKind::SysResource`). Without it the XMB stops at a red error screen.
* **sceImpose_driver**: GetParam/SetParam/Changes (after PPSSPP's sceImpose.cpp, including the
  real `0x40000000` parameter), SetStatus, CheckVideoOut, SetVideoOutMode.
* **ModuleMgrForKernel**: `sceKernelLoadModuleVSH` (1.50 NID 0xA4370E7C, 4.01 0xFE586962 through
  the resolver) and `sceKernelSearchModuleByName`.
* **sceRtc**: Register/UnregisterCallback (remembered; the clock never jumps under the VSH).
* **sceUmd**: an empty drive while the XMB runs (`umd::SetDriveEmpty`); inserting/removing an
  image notifies the drive callback with PRESENT/NOT_PRESENT.
* **`msstor0p1:`**, the memory stick's raw partition, which the Saved Data Utility queries before
  listing saves (ioctl 0x02125008 writable, 0x02125009 write protected, devctl 0x02125802 stick
  type, as msstor.prx answers them). Mounted only while the XMB runs.
* **sceReg persistence**: what the XMB writes (System Settings) is saved to
  `flash1:/registry/pspc.reg` (text, one key per line) and read back by the XMB and the games it
  launches. A game started on its own still sees the built-in registry, so tests don't depend on
  XMB use.
* **Loader fixes** found on the way, both generic: a PRX with no entry point (e_entry 0xFFFFFFFF)
  is no longer "started" at base - 1; variable imports are bound when the exporting library loads
  after the importer (music_main_plugin uses variables of music_browser, loaded later).

Deliberately left as they are: devctl 0x01F20001 answers "game disc" even with an empty drive
(answering 0 made the 4.01 XMB show "The disc could not be read" at boot), and the registry keeps
`category_version` 1 and `umd_autoboot` 1 (with a disc in the drive the XMB boots it at once, as a
PSP with UMD auto-start does).

## M33 behaviours reproduced natively (`src/runtime/m33`)

* **ISO browsing** (`vsh_iso.cpp`, vshctrl). While the XMB runs, `ms0:` gets a file system
  overlay: listing `ms0:/PSP/GAME` adds a folder `MMMMMISO<n>` per image in `ms0:/ISO`, holding a
  virtual `EBOOT.PBP` built from the image's PARAM.SFO (CATEGORY changed to "MG", a memory stick
  game), ICON0, ICON1.PMF, PIC0, PIC1 and SND0. The XMB shows icons, backgrounds and animated
  icons for each image. Deleting such an entry is refused (it would delete the image).
* **Launching**: the XMB calls `vshKernelLoadExecVSHMs2("ms0:/PSP/GAME/MMMMMISO<n>/EBOOT.PBP")`;
  `m33::LoadExecFromVsh` maps it to the image and the runtime restarts with it (a new pspc
  process, `--return-to-vsh` added). UMD launches (`vshKernelLoadExecVSHDisc`) restart with the
  disc in the drive; homebrew `EBOOT.PBP`s restart with their host path.
* **Back to the XMB**: in a game started that way, `sceKernelExitGame` restarts pspc with
  `--vsh`. HOME asks "Quit the game?" (a host message box; headless runs skip the question) and
  then runs the game's exit callback, as the PSP's HOME screen does; a game without one quits at
  once. `sctrlKernelExitVSH` and `sceKernelExitVSHVSH` restart the XMB too.
* **VSH menu**: after each XMB controller read, SELECT loads and starts `satelite.prx` (not while
  the browser, OSK or camera plugin is loaded, nor with config.se `novshmenu`); while it runs the
  callback it registered (`vctrlVSHRegisterVshMenu`) gets every read; after
  `vctrlVSHExitVSHMenu` it is stopped and unloaded. Its settings go to `flash1:/config.se`.
* **Recovery**: `--recovery`, or R held when the XMB boots, starts `recovery.prx` alone (the
  module list of M33's recovery boot configuration ends with it and needs nothing else).

## Verified (frame dumps)

* Boot: Sony Computer Entertainment logo, the wave, then the XMB (clock, battery, category icons,
  Game > Saved Data Utility / Memory Stick) at about VBlank 300; the October theme colour.
* Navigation with `--press`: every category (Settings, Photo, Music, Video, Game, Network) and
  their items; Settings > Date & Time Settings > Date Format submenu; Music > Memory Stick ("There
  are no tracks").
* Game > Memory Stick lists the images of `ms0:/ISO` with ICON0, the animated ICON1 and PIC1;
  launching Syphon Filter: Combat Ops reaches its title screen; HOME there returns to the XMB.
* `--umd`: the XMB auto-boots the disc (`umd_autoboot`).
* Game > Saved Data Utility lists the saves on the memory stick (icon, title, details) and opens
  their Options menu (Copy / Delete / Information).
* The VSH menu opens on SELECT, moves, changes settings (UMD ISO MODE), closes on SELECT.
* The recovery menu boots and navigates.

## Remaining work

* The HOME screen of a game is a host message box, not the PSP's overlay.
* GAME150/GAME4XX folders and video ISOs (`ms0:/ISO/VIDEO`, VSH menu ISO VIDEO MOUNT) are not
  browsed; vshctrl's `isocache.bin` isn't written (images are read on demand instead).
* Unimplemented VSH-only libraries log once and return 0: `sceUtility_private` (the plugins'
  dialogs), `sceChnnlsv` (savedata hashes in Saved Data Utility), `sceMgVideo_driver` (protected
  video), network/adhoc/NP pieces (`sceNetAdhocDownload`, `sceNetApctl_lib`...), `sceUsbCam`,
  `sceG729`, `sceAsfParser` exports, `sceMlnBridge`. None of them is on the boot path.
* `sceIdStorageLookup` reports every key missing (System Information shows no MAC address).
* System Settings > Date & Time can't change the clock (it follows the host's).
* The registry dump is a 6.6x PSP's: some keys the 4.01 sysconf reads may be missing or have
  6.6x values; STR keys the XMB rewrites keep the size it wrote.
* A game launched with `--umd` from the XMB comes back to an XMB with an empty drive.
* Windowed mode was only exercised through the same code paths as headless.
