# M33 plugins

On a PSP running 4.01 M33, plugins are PRX modules on the memory stick, listed in
`ms0:/seplugins/game.txt`, `vsh.txt` or `pops.txt` and switched on in the recovery menu. SystemControl
loads the listed ones when a game (or the XMB, or a PS1 game) boots. Most are kernel modules that
hook the firmware: cheat devices (DarkFrost, NitePR, CWCheat), screenshot and remote-play
plugins, button remappers and PSN enablers.

pspc runs them the same way. Its kernel is native code rather than Sony's modules, so the parts of
the firmware that plugins look at are reproduced: the boot sequence they wait on, the module
records and export tables they walk, and the kernel-only functions they call.

User-facing setup is in [plugins/README.md](../plugins/README.md).

## Files

`runtime::Options::pluginsDir` (pspc.ini `plugins_dir`, `--plugins`; default: the nearest `plugins`
folder above pspc.exe) becomes a fallback root of ms0:'s `vfs::HostFileSystem`
(`AddFallbackRoot`, through `io::AddHostFallback`).

- **Lookups:** a path missing from `<pspc dir>\ms0` is looked up in the plugins folder.
- **Listings:** they merge, and the volume's entries win on name clashes.
- **New files:** a file or folder is created in the volume when its parent exists there, else in
  the plugins folder.

So `ms0:/seplugins/game.txt` and `ms0:/darkfrost/darkfrost.prx` come from the plugins folder, and a
plugin's own writes (`ms0:/darkfrost/codes/...`) land next to it. Savedata, ISOs and everything else
still go to `ms0\`.

## Loading (`m33::LoadPlugins`)

The list file follows the boot type: `vsh.txt` for the XMB, `pops.txt` for PS1, `game.txt`
otherwise. `common/seplugins.h` parses it:

- **Format:** one `ms0:/` path per line.
- **Enable flag:** a trailing `1` or `0` turns the plugin on or off. Without one, the plugin's byte
  in `seplugins/conf.bin` decides (5 slots per list: vsh 0-4, game 5-9, pops 10-14). Without that
  either, the plugin is on, so no recovery menu is needed.
- **Load:** an enabled plugin is loaded with `kernel::LoadModuleFromMemory`. A kernel PRX (attribute
  0x1000) goes into kernel memory, and the plugin is started before the game's module.
  `--no-plugins` and `plugins=0` skip all of this; the test runners pass `--no-plugins`.

Before the first plugin loads, pspc publishes the firmware modules a real boot has loaded by then
(`kernel::RegisterFirmwareModule`, from `data/fw_boot_modules.csv`):

- **The list:** 4.01's game-mode boot list from `pspbtcnf_02g.bin` and M33's SystemControl,
  generated from the firmware inventory by `tools/gen_fw_modules.py`.
- **Each record:** a started module with its real name, attributes and version, and an export table
  in kernel memory. The table has the unnamed syslib entry first, then one entry per library with
  its 4.01 NIDs. Every function address is an 8-byte stub, a `syscall` into pspc's implementation
  (logged as unimplemented when there is none).
- **sceKernelLibrary:** the user-mode kernel library is registered after the plugins, because it is
  the last module before the game. DarkFrost waits for it.

`LoadCoreForKernel`'s lookups return SceModule2 descriptors (`m33::ModuleDescriptor`):
`sceKernelFindModuleByName`, `ByUID`, `ByAddress`, `GetModuleIdList` and `GetModuleGPByAddress`.

- Descriptors and their addresses use the kernel segment (`0x88...`) for kernel modules, as on
  hardware. DarkFrost rejects other pointers.
- `stub_top` follows `ent_top + ent_size` for modules without imports, because plugins measure the
  export table as `stub_top - ent_top`.

## Kernel functions for plugins

| Library | What |
|---|---|
| `SysclibForKernel` | the kernel's C library: memory and string functions, `strtol`, ctype tables, `sprintf`/`snprintf` (a full printf over guest varargs, with stack arguments and 64-bit values), 64-bit division |
| `StdioForKernel` | `printf`/`puts` to pspc's log (`KPRINTF`) |
| `LoadCoreForKernel` | the module lookups above, icache flush |
| `sceDisplay_driver` | `sceDisplaySetFrameBufferInternal` / `GetFrameBufferInternal` (390C7E0E / A1BEF1F6; 63E22A26 / 5B5AEFAD before M33's NID resolver) |
| `sceCtrl_driver` | reads that see every button (HOME, NOTE, SCREEN, VOL+/-, HOLD) and the disc/memory stick bits; `sceCtrlSetButtonIntercept`; `sceCtrlSetSpecialButtonCallback` |
| `LoadExecForKernel` | `sceKernelExitVSHVSH` (quit to the XMB) |

### Display priorities

The 4.01 display driver keeps four framebuffers by priority and shows the most urgent one that is
set. These details were read from display.prx:

- **The game:** `sceDisplaySetFrameBuf` is priority 2.
- **The system:** the HOME screen ("Quit the game?") uses priority 0. In pspc it is a real
  framebuffer in kernel memory (512x272, 16-bit 5650) holding a still of the game, shown at
  priority 0 while the HOME menu is open (`display::ShowSystemScreen`); the menu itself is drawn
  over it. It stays allocated after the menu closes.
- **Overrides:** a plugin draws its own screen by setting priority 0 or 1, and gives the screen
  back with a null address.
- **Reading:** `GetFrameBufferInternal(pri)` returns exactly what is set at that priority: nothing
  (address 0, format 8888, as `sceDisplayInit` leaves it) until something sets it.
- **Dialogs:** the sceUtility dialogs belong to the game's image, so they are drawn only while the
  game's framebuffer is shown (and are part of the HOME screen's still).

### Buttons

Plugins see the kernel buttons. On the keyboard: Page Up = VOL+, Page Down = VOL-, N = NOTE, B =
SCREEN. On a controller: right stick click = VOL+, left stick click = VOL-, misc1 = NOTE.

- **Intercepts:** buttons a plugin intercepts (`sceCtrlSetButtonIntercept` mode 1 = released,
  2 = pressed) change only what user-mode reads and latches see.
- **Special-button callbacks:** these run in interrupt context, with `(buttons, previous, arg)`,
  whenever one of their buttons changes between samples.
- `--press` accepts `VOLUP`, `VOLDOWN`, `NOTE` and `SCREEN`.

### Code patches

Cheats that rewrite game code are seen by AOT-compiled games when the plugin flushes the icache.
`sceKernelIcacheInvalidateRange`/`All` drop the native functions whose code changed, so that code
runs in the interpreter.

## Tested: DarkFrost 5.1 (Syphon Filter: Combat Ops, Daxter)

- **Startup:** it loads into kernel memory, waits for `sceKernelLibrary`, and creates its folders.
  It finds `sceOpenPSID_driver` through `sceOpenPSID_Service`'s export table and writes
  `config/DFimportant.bin`. It reads its configuration and registers its special-button callback
  (mask 0x00B08040: VOL+ and VOL- for the menu, SQUARE+DOWN for screenshots, NOTE).
- **In game:** until it has a screen of its own, DarkFrost polls priority 0 every 1.5 ms and
  prints "Welcome to DarkFrost v5.1! Double Tap the HOME button then press Vol +, -" at the top of
  VRAM, in the format priority 0 reports (8888). It shows on the frames where the game displays
  the buffer at the start of VRAM. In a 16-bit game (Syphon Filter) the 32-bit text comes out
  twice as wide; in a 32-bit game (Daxter) it is clean.
- **HOME twice:** the first press opens the HOME screen, and DarkFrost sees its framebuffer at
  priority 0 and keeps it; the second closes it. Its menu lives in that buffer from then on, so the
  game can't paint over it ("to save memory": it allocates none of its own).
- **The menu:** VOL+ and VOL- together open it. DarkFrost intercepts the game's buttons, shows the
  HOME screen's buffer at priority 0 (16-bit), and draws its tips page and then its tabs (Cheater,
  Options, Searcher, RAM, Decoder, Tools, Credits). Closing the menu gives the screen and buttons
  back. The game keeps running underneath unless DarkFrost's "Pause game" option is on.

### Hooks

The usual M33 ways of hooking work:

- **SystemControl's functions:** `sctrlHENFindFunction` hands out a stub for any library function,
  and `sctrlHENPatchSyscall` redirects that function to the plugin.
- **Rewriting an import stub:** a plugin can rewrite an import stub, the game's or a module
  record's, with `j hook`, usually from a start-module handler. A stub whose words are no longer
  `jr ra; syscall` stops being dispatched natively and runs as guest code.

## Limits

- Plugins that patch firmware code in memory find no code in the module records. These are hooks
  written into another module's text, such as ctrl.prx's sampling function.
- `sctrlHENFindDriver` has no driver structures to return.
- POPS has no plugins to load into yet, because pspc doesn't run PS1 games.
