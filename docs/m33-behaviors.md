# 4.01 M33 behaviour catalogue

This is the specification for PSPC's native reimplementation of the M33 custom
firmware layer. It is written from analysing the 11 M33 modules extracted from
the installer (`tools/m33extract.py` -> `private/fw/401/m33/`) with Ghidra
(`tools/ghidra_analyze.cmd`, image base `0x88000000` for kernel modules and
`0x08804000` for user/VSH modules) and pspdecompiler, and from the M33 SDK
headers under `reference/401M33/SDK/include/`.

Each claim is tagged with how it is known:
- **[hdr]** a prototype, struct or enum in an M33 SDK header (a published fact).
- **[exp]** an export table entry (library + NID) read by `fwinventory.py`.
- **[dis <module>@<offset>]** behaviour read from the decompilation of that
  module at that module offset.
- **[str <module>]** a string constant present in that module.

No decompiled source is reproduced here; only facts (NIDs, offsets, struct
fields, string and path constants, control flow described in prose).

Note on names: M33's own kernel libraries (`SystemCtrlForKernel`,
`SystemCtrlForUser`, `KUBridge`, `VshCtrlLib`, `pspUsbDevice`) have their NIDs
named from the M33 SDK stub archives; many functions M33 *imports* from the
4.01 Sony kernel have randomised (non-hashed) NIDs whose names are not public,
so they appear below as `Library_NID`.

---

## 1. Module map

| flash0 path | module name | attr | role |
|---|---|---|---|
| `kd/systemctrl.prx` | SystemControl | 0x3007 | HEN/SE core; patches Sony kernel, NID resolver, SE config, LoadExec, plugins |
| `kd/systemctrl_02g.prx` | SystemControl | 0x3007 | same, PSP-2000 (TA-085 / 64 MB) variant |
| `kd/vshctrl.prx` | VshControl | 0x1007 | XMB/VSH patches: ISO browsing, VSH menu loading, USB, updates |
| `vsh/module/satelite.prx` | VshCtrlSatelite | 0x0000 | the in-XMB VSH menu (user mode) |
| `vsh/module/recovery.prx` | Recovery mode | 0x0800 | recovery menu (standalone VSH app) |
| `kd/march33.prx` | pspMarch33_Driver | 0x1006 | M33 UMD/ISO driver: reimplements `sceUmd*`, `sceUmd9660` |
| `kd/galaxy.prx` | M33GalaxyController | 0x1006 | ISO read back-end / reboot helper used by march33 |
| `kd/popcorn.prx` (with `kd/np9660`) | M33PopcornManager | 0x1007 | PS1 (POPS) manager |
| `kd/idcanager.prx` | M33IdCanager_Driver | 0x5006 | id / `act.dat` (DRM activation) handling |
| `kd/usbdevice.prx` | pspUsbDev_Driver | 0x1006 | exposes flash0-3 / UMD over USB |
| (installer only) | pspIplUpdate | 0x1007 | IPL update helper, not part of the running firmware |

Attribute bit `0x1000` = kernel, `0x0800` = VSH, `0x4000` on an export library
= syscall-exported to user mode. **[exp]**

---

## 2. systemctrl (SystemControl)

systemctrl is loaded very early (its `pspbtcnf` entry sits near `init.prx`) and
is the HEN/SE core. It imports only from the Sony kernel libraries
`SysMemForKernel`, `ModuleMgrForKernel`, `LoadCoreForKernel`,
`LoadExecForKernel`, `IoFileMgrForKernel`, `ThreadManForKernel`,
`InterruptManagerForKernel`, `InitForKernel`, `UtilsForKernel`,
`SysclibForKernel`. **[exp]**

### 2.1 Exported libraries

- **SystemCtrlForKernel** (kernel) and **SystemCtrlForUser** (syscall) export
  the HEN/SE API; the user library is a subset of the kernel one. **[exp]**
- **KUBridge** (syscall) lets user mode reach kernel-only operations. **[exp]**

Behaviours of the named exports (NIDs from the M33 SDK stubs):

| Export | Behaviour | Evidence |
|---|---|---|
| `sctrlHENFindFunction(mod, lib, nid)` | Finds a module by name via `LoadCoreForKernel`, searches its export table for the library+NID, returns the resolved address. Used throughout systemctrl to call patched Sony functions. | [dis systemctrl@0x8D0] |
| `sctrlHENSetStartModuleHandler(fn)` | Stores a single callback pointer, returns the previous one; the callback runs for every module just before its `module_start`. | [dis systemctrl@0x32C], [hdr] |
| `sctrlHENPatchSyscall(addr, newaddr)` | Rewrites the kernel syscall table so the syscall that pointed at `addr` points at `newaddr`. | [dis systemctrl@0x3D0], [hdr] |
| `sctrlHENLoadModuleOnReboot(after, buf, size, flags)` | Records one module (buffer, size, the module to insert it before, and a `BootLoadFlags` mask of VSH/GAME/UPDATER/POPS/UMDEMU) to be inserted into the next reboot's module list. | [dis systemctrl@0x1714], [hdr] |
| `sctrlHENGetVersion` / `sctrlSEGetVersion` | Return constants (HEN D / SE-C report 0x00000400). | [dis systemctrl@0x58B0/0x58B8], [hdr] |
| `sctrlHENIsSE` / `sctrlHENIsDevhook` | Return constants (full firmware is SE, not devhook). | [dis systemctrl@0x58A0/0x58A8] |
| `sctrlSEGetConfig(Ex)` / `sctrlSESetConfig(Ex)` | Read/write `flash1:/config.se` (see 2.4). | [dis systemctrl@0x54F0..0x5654], [str systemctrl], [hdr] |
| `sctrlSEMountUmdFromFile(file, noumd, isofs)` | Sets the current UMD file, adds the M33 isofs driver, and assigns `disc0:`/`umd0:`/`isofs0:` to it. | [dis systemctrl@0x5CAC], [str systemctrl], [hdr] |
| `sctrlSEUmountUmd` / `sctrlSESetDiscOut` / `sctrlSESetDiscType` | Control UMD presence and disc type (game/video/audio). | [dis systemctrl@0x4D0C/0x4E64/0x4638], [hdr] |
| `sctrlSEGetUmdFile` / `sctrlSESetUmdFile` | Get/set the current ISO path (kernel). | [dis systemctrl@0x417C/0x4768] |
| `sctrlSESetBootConfFileIndex(i)` | Selects the boot config for the next reboot: 0 normal, 1 march33 driver, 2 NP9660. | [dis systemctrl@0x1738], [hdr] |
| `sctrlKernelLoadExecVSH*` family (Disc, DiscUpdater, Ms1..Ms4, WithApitype) | Each sets a different apitype then calls the matching patched `LoadExecForKernel` entry to tear down and relaunch. Ms2 = games, Ms1 = ms updater, Ms3/Ms4 = other, Disc = UMD EBOOT, DiscUpdater = disc updater. | [dis systemctrl@0x5938..0x5BD4], [hdr] |
| `sctrlKernelExitVSH` | Restarts the VSH via `LoadExecForKernel`. | [dis systemctrl@0x59BC], [hdr] |
| `sctrlKernelSetInitApitype` / `SetInitFileName` / `SetInitKeyConfig`(=`SetInitMode`) / `SetUserLevel` / `SetDevkitVersion` | Override the values the kernel reports for the next/this launch; model/devkit spoofing. | [dis systemctrl@0x5F00/0x5E64/0x5EA4/0x5C30/0x5DE4], [hdr] |
| **KUBridge**: `kuKernelLoadModule(WithApitype2)`, `kuKernelGetModel`, `kuKernelGetUserLevel`, `kuKernelBootFrom`, `kuKernelInitApitype`, `kuKernelInitFileName`, `kuKernelInitKeyConfig`, `kuKernelSetDdrMemoryProtection` | Thin kernel-privileged wrappers callable from user mode; `kuKernelGetModel` wraps `sceKernelGetModel`. | [dis systemctrl@0x5758..0x5834], [hdr] |

### 2.2 Patches applied to Sony modules

systemctrl registers a start-module handler and, as each target Sony module is
loaded, overwrites instruction words in its text (found relative to the
module's text base via `LoadCoreForKernel`). The per-module patch routines
observed: **[dis systemctrl@0x0A10..0x1604, 0xC30]**

- **sceLoaderCore** (`sceLoaderCore`): the largest patch set. Redirects the
  loader's decrypt/relocate entry points to systemctrl's own handlers
  (trampolines held in its data), disables several checks by writing
  `nop`/`li` constants, and points the loader's two memlmd decrypt hooks at
  `sceMemlmd` functions found by NID (`0xE74BE02B`, `0xA54D51D2`). This is where
  "run unsigned/encrypted PRX" comes from. [dis systemctrl@0xFF8]
- **sceModuleManager** (`sceModuleManager`): patches the module manager so it
  accepts modules the stock firmware would reject, and installs systemctrl's
  load hook (used for plugin loading, 2.5). [dis systemctrl@0xB30]
- **sceMemlmd** / **memlmd**: hooked for the decrypt bypass. [dis systemctrl@0xAA4]
- **sceInterruptManager**: three words zeroed. [dis systemctrl@0xA74]
- **sceMesgLed** (the signature/`mesg_led` check module): neutralised. [dis systemctrl@0xA10]
- Per-driver tweaks as they load, keyed by module name: `sceIsofs_driver`,
  `sceLowIO_Driver`, `sceUmdMan_driver`, `sceMediaSync`, `sceImpose_Driver`,
  `sceWlan_Driver`, `scePower_Service`, `sceLFatFs_Driver` (the last gets a
  marker word `0xDADADADA`). [dis systemctrl@0xC30]
- **scePower_driver**: after NID resolution, one import (NID `0x737486F2`) is
  rewritten to jump to an M33 replacement. [dis systemctrl@0xDE4]

### 2.3 Kernel NID resolver

Because kernel-library NIDs changed between firmware versions, M33 modules (and
1.50-era add-ons) are linked against the old NIDs. At module load systemctrl
walks each import stub and, for libraries it knows, translates the imported NID
to its 4.01 value through a static directory of 26 per-library tables of
`{old_nid, new_nid}` pairs. **[dis systemctrl@0xDE4 (resolver loop), @0x2E0
(lookup), @0x75C (table selector), directory at module offset 0x9644]**

The full table is extracted to **`data/m33_nid_resolver.csv`**
(`tools/extract_m33_nid_resolver.py`): 996 pairs across 26 libraries
(`SysMemForKernel` 96, `sceSysreg_driver` 153, `sceSyscon_driver` 111,
`scePower_driver` 83, `sceDmacplus_driver` 53, `ModuleMgrForKernel` 48,
`sceRtc_driver` 44, `sceDisplay_driver` 42, `InterruptManagerForKernel` 41,
`LoadCoreForKernel` 40, `sceGe_driver` 34, `sceCtrl_driver` 34, `sceUmd` 33,
`KDebugForKernel` 26, `sceHprm_driver` 24, `sceImpose_driver` 24,
`sceDve_driver` 22, `sceReg_driver` 21, `LoadExecForKernel` 20,
`sceDdr_driver` 13, `sceGpio_driver` 12, `IoFileMgrForKernel` 10,
`ExceptionManagerForKernel` 7, `memlmd` 3, `sceMesgLed_driver` 1,
`sceClockgen_driver` 1). For PSPC, the recompiled Sony/M33 modules link
against 4.01 NIDs directly, so this table is reproduced natively as the
compatibility shim for any 1.50-era module we load.

### 2.4 config.se layout

Stored at `flash1:/config.se`; `sctrlSEGetConfigEx`/`SetConfigEx` read/write it
raw. The structure is `SEConfig` **[hdr systemctrl_se.h]**, all fields `int`:

| off | field | meaning |
|---|---|---|
| 0x00 | `magic` | `0x47434553` |
| 0x04 | `hidecorrupt` | hide corrupt XMB icons |
| 0x08 | `skiplogo` | skip the Sony boot logo |
| 0x0C | `umdactivatedplaincheck` | run plain (decrypted) modules from UMD/ISO |
| 0x10 | `gamekernel150` | run GAME-folder homebrew under the 1.50 kernel |
| 0x14 | `executebootbin` | execute `BOOT.BIN` (not `EBOOT.BIN`) on disc |
| 0x18 | `startupprog` | autorun program slot |
| 0x1C | `umdmode` | UMD mode enum (2.4.1) |
| 0x20 | `useisofsonumdinserted` | use M33 isofs even with a real UMD inserted |
| 0x24 | `vshcpuspeed` | XMB CPU clock |
| 0x28 | `vshbusspeed` | XMB bus clock |
| 0x2C | `umdisocpuspeed` | UMD/ISO CPU clock |
| 0x30 | `umdisobusspeed` | UMD/ISO bus clock |
| 0x34 | `fakeregion` | region spoof enum (2.4.2) |
| 0x38 | `freeumdregion` | ignore UMD region lock |
| 0x3C | `hardresetHB` | hard reset for homebrew |
| 0x40 | `usbdevice` | which flash/UMD the XMB USB exposes |
| 0x44 | `novshmenu` | disable the VSH menu |
| 0x48 | `usbcharge` | charge over USB when cable plugged |
| 0x4C | `notusedaxupd` | (M33 network update flag) |
| 0x50 | `reserved[2]` | |

#### 2.4.1 UMD modes (`umdmode`) **[hdr enum SEUmdModes]**
`0` MODE_UMD (normal, real UMD required), `1` MODE_OE_LEGACY (OE isofs legacy,
no UMD), `2` MODE_MARCH33 (M33 driver, no UMD), `3` MODE_NP9660 (Sony NP9660,
no UMD). The recovery/VSH-menu wording confirms these four. **[str recovery]**

#### 2.4.2 Fake region (`fakeregion`) **[hdr enum, FAKE_REGION_*]**
`0` disabled, `1` Japan, `2` America, `3` Europe, `4` Korea, `5`/`6` unused,
`7` Australia, `8` Hong Kong, `9` Taiwan, `10` Russia, `11` China. (The header
warns Korea/Hong Kong/Taiwan/China may brick on "restore defaults".)

### 2.5 Plugin and config loading

systemctrl's module-load hook fires when `vsh_module` and other modules load.
**[dis systemctrl@0x60B8]** By the kernel's application type it loads per-mode
plugin lists:
- app type `0x100` (VSH/XMB): reads `ms0:/seplugins/conf.bin` (a 15-byte
  per-plugin enable array) then `ms0:/seplugins/vsh.txt`.
- app type `0x200` (GAME): `ms0:/seplugins/game.txt`.
- app type `0x300` (POPS/PS1): `ms0:/seplugins/pops.txt`.

Each `*.txt` is parsed line by line **[dis systemctrl@0x5FD8]**; a line's
enable flag is taken from the matching `conf.bin` byte, and enabled plugins are
loaded by path and started **[dis systemctrl@0x6048]**. For the XMB it also
loads the chosen ISO back-end (`sceNp9660_driver` vs `sceMediaSync` +
`sceMeCodecWrapper`) and, on app type `0x100`, `scePspNpDrm_Driver`, and
performs a memory-stick ready wait via `sceIoDevctl` on `fatms0:`.
**[dis systemctrl@0x60B8], [str systemctrl]**

### 2.6 Decrypt bypass (how M33 PRXs load)

The loader's two decrypt entry points are redirected to systemctrl handlers
**[dis systemctrl@0x400 (main/user), @0x5FC (kernel)]**. Each checks the `~PSP`
header's OE tag at header offset 0x130 against the known M33 tags
(`0x7316308C`, `0x55668D96`/`0x8555ABF2`, `0x3EAD0AEE`, `0x28796DAA`) with a
gzip payload (`1f 8b`) at 0x150; when it matches, it simply gunzips the payload
(`memmove` of `comp_size` at header+0xB0) in place instead of calling KIRK.
Otherwise it defers to the original Sony decrypt, and on failure retries via a
fallback. This is the `~PSP`+gzip format `psp-packer` produces, confirming M33
modules are *packed*, not encrypted. **[dis]**

---

## 3. vshctrl (VshControl)

A kernel module that patches the XMB (`vshmain`/VSH) to add M33 features.
Exports **VshCtrlLib** (`vctrlVSHRegisterVshMenu` NID 0xFD26DA72,
`vctrlVSHExitVSHMenu` NID 0xCD6B3913). **[exp], [hdr vshctrl.h]**

Behaviours (from functions and string constants **[dis/str vshctrl]**):
- **VSH menu loading**: on HOME it loads `flash0:/vsh/module/satelite.prx`, but
  suppresses it while the camera, OSK, or HTML-viewer plugin is active
  (`camera_plugin_module`, `sceVshOSK_Module`, `htmlviewer_plugin_module`) --
  this is the 4.01 "camera button" fix noted in the installer readme.
  `vctrlVSHRegisterVshMenu` registers the menu's per-frame controller callback
  (run in kernel mode even though satelite is a user module). [dis vshctrl@0x3A4, 0x1D0]
- **ISO browsing in the Game menu**: scans `ms0:/ISO` (and `ms0:/ISO/VIDEO`)
  and the `ms0:/PSP/GAME`, `GAME150`, `GAME4XX` folders, presenting ISOs as
  fake EBOOTs. It builds "virtual PBP" entries (`VirtualPBPMgr`) by reading
  `PARAM.SFO`, `ICON0.PNG`, `ICON1.PMF`, `PIC0.PNG`, `PIC1.PNG`, `SND0.AT3`
  from inside the ISO's `/PSP_GAME/`, and caches them in
  `ms0:/PSP/SYSTEM/isocache.bin`. The internal name tag is `MMMMMISO`. On
  launch it mounts the ISO and relaunches via LoadExecVSH. [dis vshctrl@0x5C0, 0x19D8, 0x36E4, 0xB28, 0xE00]
- **GAME4XX folder**: 4.01/higher homebrew lives in `GAME4XX` so updates don't
  need a rename (installer readme); vshctrl maps the GAME/GAME150/GAME4XX
  split and the 1.50-kernel path (`reboot150.prx`). [dis vshctrl@0x11D4, 0x334]
- **USB in the XMB**: toggles `usbdevice.prx` / `USBStor_Driver` to export the
  configured device. [dis vshctrl@0xF40, 0x1EF4]
- **Network update**: redirects the XMB updater to
  `http://updates.dark-alex.org/update` when M33 network update is enabled.
  [str vshctrl@0x202C]
- `vctrlVSHExitVSHMenu(conf, videoiso, disctype)` applies the new `SEConfig`
  that satelite passed, optionally mounts a video ISO, and unloads the menu.
  [dis vshctrl@0x1058], [hdr]
- **The hooks, in detail** (how the XMB sees them; PSPC reproduces these natively, see
  docs/vsh-boot.md): vshctrl redirects the XMB's `sceIoDopen/Dread/Getstat/Open/...` syscalls
  (`sctrlHENPatchSyscall`) [dis vshctrl@0x90]. Reading `ms0:/PSP/GAME` returns, after its own
  entries, those of `GAME150`/`GAME4XX` (names suffixed `__150`/`__4XX`) and one directory per
  image file in `ms0:/ISO`, named `MMMMMISO<n>` (mode 0x11FF, the image's times)
  [dis vshctrl@0x11D4, 0xD8C]. Paths under `ms0:/PSP/GAME/MMMMMISO<n>` reach a virtual-PBP
  manager ("VirtualPBPMgr", up to 0x80 images): the folder holds `EBOOT.PBP`, a PBP whose
  PARAM.SFO is a template filled with the image's TITLE and DISC_ID and whose ICON0/ICON1/PIC0/
  PIC1/SND0 sections are read from the image through `/sce_lbn` extents [dis vshctrl@0x36E4,
  0x2D10]. The XMB's controller read is hooked too: with no VSH menu loaded and none of
  `htmlviewer_plugin_module`, `sceVshOSK_Module`, `camera_plugin_module` loaded, SELECT in the
  first sample loads `satelite.prx` into partition 5, starts it and clears SELECT; while the menu
  is loaded its registered callback gets each read; once it unregistered, the menu is stopped and
  unloaded [dis vshctrl@0x3A4].

---

## 4. satelite (the VSH menu)

A user module (title "M33 VSH MENU"), started as thread `VshMenu_Thread`,
imports **SystemCtrlForUser** and **VshCtrlLib** plus scePaf/scePower/sceDisplay.
**[exp], [str satelite]** Menu rows (string table at module offset ~0x21B4):
**[dis satelite@0x914/0xAA4, str satelite]**

- **CPU CLOCK XMB** / **CPU CLOCK GAME** -- value `Default` or `%d/%d` MHz.
- **USB DEVICE** -- `Flash %d` / `UMD Disc` / `Memory Stick` / `NONE`.
- **UMD ISO MODE** -- `Normal` / `OE isofs` / `M33 driver` / `Sony NP9660`.
- **ISO VIDEO MOUNT** -- mount a video ISO from `ms0:/ISO/VIDEO` (reads
  `CD001`, `UMD_VIDEO`/`UMD_AUDIO`, caches in `ms0:/PSP/SYSTEM/isocachev.bin`).
- **RESET DEVICE**.

On exit it hands the updated `SEConfig` (and any chosen video ISO + disc type)
to `vctrlVSHExitVSHMenu`; saving to `config.se` is satelite's job via
`sctrlSESetConfig`. **[hdr vshctrl.h, dis satelite@0x1060]**

---

## 5. recovery (Recovery mode)

A standalone VSH app ("M33 Recovery Menu"). Its strings are fully translatable
from `flash1:/<lang>_recovery.txt` or `ms0:/seplugins/<lang>_recovery.txt`
(installer ships `es_recovery.txt`; `reference/401M33/translate/en_recovery.txt`
is the English template). **[str recovery, installer readme]**

Menu structure (string block + per-item handlers) **[str/dis recovery]**:
- **Toggle USB** -- enable/disable USB (`USBBusDriver`/`USBStor_Driver`).
- **Configuration** -- settings written to `config.se`:
  skip Sony logo, hide corrupt icons, game-folder homebrew (1.50 kernel),
  UMD mode, fake region, use VSH menu, XMB USB device, charge battery over USB,
  speed in XMB / in UMD-ISO, use M33 network update, hide PIC0/PIC1 in game
  menu, button assign (O vs X is enter).
- **Advanced / Advanced configuration** -- plain modules in UMD/ISO, execute
  `BOOT.BIN` in UMD/ISO, autorun program at, etc. (maps to the lower `SEConfig`
  fields).
- **Run program at** / **Autorun program at** -- runs
  `ms0:/PSP/GAME/RECOVERY/EBOOT.PBP` or `.../BOOT/EBOOT.PBP`.
- **CPU Speed**.
- **Plugins** -- toggles entries read from `conf.bin` + `vsh.txt`/`game.txt`/
  `game150.txt`/`pops.txt`, writing the enable bytes back to `conf.bin`.
- **Registry hacks** -- toggles registry keys: `wma_play`, `flash_play`,
  `flash_activated` (Activate WMA / Activate Flash Player), `button_assign`,
  `language`, under `/CONFIG/SYSTEM/XMB`, `/CONFIG/MUSIC`, `/CONFIG/BROWSER`;
  reached through `flash1:/registry` and `flash1:/vsh` paths.
- **Format flash1 and reset settings** -- `fatfmt` of `flash1:` (`lflash0:0,1`).
- **1.50 Kernel / 4.XX Kernel** -- select kernel for the next launch.
- **Exit** / **Back**.

---

## 6. march33 (the M33 UMD/ISO driver)

Kernel driver that reimplements the UMD stack so an ISO file can stand in for a
disc. **[exp]** It re-exports the standard UMD libraries so the rest of the OS
and games are unaware:
- **sceUmdUser** (syscall) and **sceUmd** (kernel): the full
  `sceUmdActivate`/`Deactivate`/`CheckMedium`/`GetDiscInfo`/`GetDriveStat`/
  `WaitDriveStat(CB/WithTimer)`/`RegisterUMDCallBack`/`ReplaceProhibit`/
  `ReplacePermit` API. [exp, dis march33]
- **sceUmdMan_driver** and **sceUmd9660_driver**: the lower driver +
  ISO9660 filesystem exports. [exp]

It assigns `disc0:`/`umd0:`/`isofs0:` to the mounted image and answers UMD
devctls (an "unknown devctl 0x%08X" diagnostic exists for unhandled ones).
**[dis march33@0x1B38, 0x4F4, str march33]** Which back-end is active is chosen
by `umdmode` / the boot-conf index set by `sctrlSESetBootConfFileIndex`
(march33 = index 1). **[dis systemctrl@0x1738, hdr]**

## 7. galaxy (M33GalaxyController)

Kernel helper tied to march33. Its `module_start` hooks the thread manager
(patches two pointers inside `sceThreadManager`'s data) and opens the
mass-storage device (`IoFileMgr` fd on device `0xF0001`), retrying until ready;
it drives the low-level block reads for the ISO/UMD emulation that march33
exposes. **[dis galaxy@0x340, 0x184]**

## 8. popcorn (M33PopcornManager)

Kernel module for PS1 (POPS) emulation. Exports only syslib; it manages the
POPS environment: it reads `/kd/pspbtcnf.txt`, cooperates with
`scePops_Manager` / `sceSystemMemoryManager` / `SysMemUserForUser`, and handles
PS1 title images (`IHDR`/`PSTITLE`). **[exp, dis/str popcorn]** It is the
POPS-side counterpart to `np9660`/`popsman` and is only relevant to the
stretch-goal PS1 support.

## 9. idcanager (M33IdCanager_Driver)

Kernel driver ("EcsIDCanager") for DRM identity/activation. **[exp
sceIdMgr_driver NID 0xF464F91C]** It works with `flash2:/act.dat` (the console
activation file, via `lflash0:0,2` / `flashfat2:`) and the NPDRM license store
`ms0:/PSP/LICENSE/`. **[dis idcanager@0x1A0, 0x3CC, str idcanager]** For PSPC
this is only needed if NPDRM/PSN content is ever supported; offline UMD/ISO
play does not use it.

## 10. usbdevice (pspUsbDev_Driver)

Kernel driver exposing internal storage over USB. **[exp pspUsbDevice /
pspUsbDevice_driver: `pspUsbDeviceSetDevice` NID 0xD4D90520,
`pspUsbDeviceFinishDevice` NID 0xB3CF0329]** `pspUsbDeviceSetDevice(device,
ronly, unassign_mask)` picks one of `PSP_USBDEVICE_FLASH0..3` or
`PSP_USBDEVICE_UMD9660` **[hdr pspusbdevice.h]**; it maps the chosen flash
(`lflash0:0,N` / `flashfat N:` / `flash N:`) or the UMD (`disc0:`/`umd0:`,
reading `CD001`/`UmdSector`) and can read-only-mount and auto-unassign flashes,
reassigning them in `pspUsbDeviceFinishDevice`. **[dis usbdevice@0xAA0, 0x4AC,
str usbdevice]** Used by vshctrl's "XMB USB device" and recovery's "Toggle USB".

---

## 11. Apitype / boot-mode summary (for the native loader)

systemctrl keys most behaviour off the kernel application type:
`0x100` VSH/XMB, `0x120` UMD/disc game, `0x200` ms GAME homebrew, `0x300` POPS.
**[dis systemctrl@0x60B8, 0xC30, 0x11A4]** The `BootLoadFlags`
(VSH=1, GAME=2, UPDATER=4, POPS=8, UMDEMU=64) select which reboot-time module
insertions apply. **[hdr]** PSPC's native kernel sets the same apitype when it
launches a title so the recompiled VSH/M33 modules behave identically; the
reboot-time patching is instead reproduced natively (PLAN fact 4).
