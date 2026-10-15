# Firmware analysis tooling

How to run PSPC's firmware extraction, inventory, NID and decompilation tools.
All of them are our own code (GPL-2.0-or-later) and take input paths as
arguments. Outputs that are derived from copyrighted firmware go under
`private/` (gitignored); `data/` and `docs/` hold only facts and our words.

The paths and Windows commands below record the original analysis setup, not Linux
requirements. For Linux builds and current verification scope, see [Linux port status](linux-port.md#current-linux-milestone);
for portable Ghidra/JDK selection, see [the headless pipeline](#6-ghidra-headless-pipeline----toolsghidraghidra_analyzepy).

Original Windows prerequisites:
- Python 3.12 at `C:\Users\yoelr\AppData\Local\Programs\Python\Python312\python.exe`.
  **Always run with `-I`** (isolated: ignores `PYTHONPATH`/user site, so a
  planted module next to an untrusted input cannot be imported). All tools are
  stdlib-only.
- Ghidra 12.1.3 at `C:\Users\yoelr\Tools\ghidra_12.1.3_PUBLIC` with the
  `ghidra-allegrex` extension already unpacked under
  `Ghidra/Extensions/ghidra-allegrex`.
- JDK 21 at `C:\Program Files\Eclipse Adoptium\jdk-21*` (an older JDK 8 is also
  present; the scripts pick jdk-21 explicitly).
- MSVC + CMake for building pspdecompiler.

Shorthand below: `PY` = the Python path above.

---

## 1. Extract the M33 modules -- `tools/m33extract.py`

Unpacks the modules embedded in the M33 installer's `DATA.PSP`.

```
PY -I tools/m33extract.py reference/401M33/UPDATE/EBOOT.PBP -o private/fw/401/m33
```

Produces under `private/fw/401/m33/`:
- 11 plain PRX files named by their flash0 destinations
  (`systemctrl.prx`, `systemctrl_02g.prx`, `vshctrl.prx`, `usbdevice.prx`,
  `satelite.prx`, `recovery.prx`, `popcorn.prx`, `march33.prx`,
  `idcanager.prx`, `galaxy.prx`, `ipl_update.prx`). Destination names are not
  guessed -- they come from the `{path, data, size}` write tables and write
  call sites inside the installer's own `uranium235` ELF.
- `installer/` -- the installer's own plain ELFs (`u235.prx`, the two
  `plutonium_updater` stages `data.psp.prx`/`pupd_data.psp.prx`,
  `plutonium_driver.prx`, `degeneration_driver.prx`, `dl_data.psp.prx`) and the
  nested `PUPD.PBP` / `DL.PBP`.
- the `pspbtcnf*.bin` boot configs, `es_recovery.txt`, and `payloads/`
  (gzip blobs embedded inside systemctrl, e.g. its reboot patch code).
- `manifest.json` -- name, module name, attributes, size, sha1, source offset,
  M33 tag and the evidence used to name each file.

## 2. Inventory -- `tools/fwinventory.py`

Parses ELF/PRX (and `~PSP`, PBP, `pspbtcnf`) files: ELF/program/section
headers, PSP module info (from `.rodata.sceModuleInfo` or, for a PRX with no
section headers, `phdr[0].p_paddr`), exports (`.lib.ent`) and imports
(`.lib.stub`) with NIDs, type-A relocation sections (`SHT 0x700000A0`) and
type-B relocations (program header type `0x700000A1`) with a type histogram,
and sha1. NIDs are named from `data/nids.csv` when present.

```
PY -I tools/fwinventory.py private/fw/401/m33 -o private/fw/401/inventory.json
```

Writes `inventory.json`, `inventory.md` (a readable summary with an
imports-by-NID table) and `imports.dot` (a Graphviz dependency graph:
`module -> providing module`, or `module -> external library` when the
provider is outside the set). Render with
`dot -Tsvg private/fw/401/imports.dot -o imports.svg` if Graphviz is installed.
Pass `--relocs` to include every relocation entry in the JSON.

## 3. NID database -- `tools/nidgen.py`

Harvests library/NID/name triples from `reference/pspsdk` (`.S` stubs),
`reference/ppsspp` (HLE tables + `RegisterHLEModule`), `reference/uofw`
(`exports.exp`) and the M33 SDK stub archives, dedupes (preferring
hash-verified names), and writes `data/nids.csv`
(`library,nid,name,source,hash_ok`) and `data/nids_by_library.json`.

```
PY -I tools/nidgen.py --pspsdk reference/pspsdk --ppsspp reference/ppsspp \
   --uofw reference/uofw --m33 reference/401M33 \
   -o data/nids.csv --json data/nids_by_library.json
```

`hash_ok=1` means the first 4 bytes of SHA-1(name), little-endian, equal the
NID (true for user-mode NIDs; many kernel NIDs are randomised). Current output:
5463 rows across 192 libraries, 60.5% hash-verified.

## 4. M33 NID resolver table -- `tools/extract_m33_nid_resolver.py`

Extracts systemctrl's old->new kernel NID translation tables (facts) to
`data/m33_nid_resolver.csv` (`library,old_nid,new_nid`).

```
PY -I tools/extract_m33_nid_resolver.py private/fw/401/m33/systemctrl.prx \
   -o data/m33_nid_resolver.csv
```

996 pairs across 26 kernel libraries. (Running it on `systemctrl_02g.prx`
yields the same set.)

## 5. pspdecompiler (cross-check decompiler)

Humberto Naves' PRX decompiler (AFL-2.0), built **out-of-tree** so its
GPL-incompatible code is never linked into PSPC. Nothing in `reference/` is
modified; all Windows fixes live under `tools/pspdecompiler/compat/`.

```
cmd /c tools\build_pspdecompiler.cmd          REM Release, x64, static CRT
```

Output: `build\tools\pspdecompiler\pspdecompiler.exe`. Useful options:
`-vi` module info (needs `-v`; `-vvi` adds relocations), `-c` writes
`<name>.c`/`.h` to the current directory, `-g` writes per-subroutine `.dot`
graphs, `-n <psplibdoc.xml>` applies NID names.

### psplibdoc XML -- `tools/nids_to_psplibdoc.py`

Converts `data/nids.csv` into the XML `pspdecompiler -n` reads (also usable by
prxtool and Ghidra importers).

```
PY -I tools/nids_to_psplibdoc.py data/nids.csv -o data/psplibdoc.xml
```

The writer follows the parser's strict rules (lowercase `0x` NIDs, one-line
names with no whitespace/entities, library `<NAME>` before its functions, one
entry per (library,nid), `<NUMARGS>-1</NUMARGS>` for unknown arg counts).
Validate on a few modules (outputs under `private/fw/401/pspdecompiler/`):

```
build\tools\pspdecompiler\pspdecompiler.exe -vi -n data/psplibdoc.xml \
   private\fw\401\m33\systemctrl.prx
build\tools\pspdecompiler\pspdecompiler.exe -vi -n data/psplibdoc.xml \
   reference\401M33\SDK\samples\bootload\bootload.prx
```

## 6. Ghidra headless pipeline -- `tools/ghidra/ghidra_analyze.py`

Imports each module into a Ghidra project under `private/ghidra/` with the
Allegrex loader/language, names imports and exports from `data/nids.csv`
(`PspcApplyNids.java`, pre-analysis), runs auto-analysis, and exports
`private/fw/401/analysis/<module>.json` (function boundaries, call graph, jump
tables, string refs, code pointers) and `private/fw/401/decomp/<module>.c`
(decompiled C) via `PspcExportAnalysis.java`.

Install Ghidra with the `ghidra-allegrex` loader/language and a full JDK compatible
with that Ghidra version (not just a JRE). Select the installation explicitly;
there is no machine-specific default Ghidra directory.

Linux:

```sh
python3 -I tools/ghidra/ghidra_analyze.py --ghidra /opt/ghidra \
    --java-home /path/to/jdk private/fw/401/m33
```

Windows (the wrapper accepts the same options):

```bat
cmd /c tools\ghidra_analyze.cmd --ghidra C:\Tools\ghidra private\fw\401\m33
```

Details:
- Kernel modules (attribute `0x1000`) load at image base `0x88000000`, all
  others at `0x08804000`; modules sharing a project folder and base are
  imported in one `analyzeHeadless` run. Logs go to `private/ghidra/logs/`.
- `--ghidra` overrides `GHIDRA_INSTALL_DIR`. Linux runs `support/analyzeHeadless`;
  Windows runs `support/analyzeHeadless.bat`.
- JDK selection order is `--java-home`, `PSPC_JAVA_HOME`, `JAVA_HOME`, then
  Windows `jdk-21*` installation discovery, then `javac` on `PATH`. Linux
  resolves alternatives/symlink chains to the actual JDK home. Both `java` and
  `javac` must exist (and be executable on Linux); an invalid explicitly selected
  home fails rather than silently selecting another installation. Ghidra checks
  Java-version compatibility. The helper sets `JAVA_HOME` and prepends its `bin`
  to the child process's `PATH`.
- `--dry-run` prints commands without starting Ghidra, using POSIX shell quoting
  on Linux. It still validates the installation/JDK and creates the project/log
  directories. Use `--project-dir` and `--out` for isolated analysis output.
- Options (pass through to `tools/ghidra/ghidra_analyze.py`): `--no-decomp`,
  `--timeout <s>`, `--max-cpu <n>`, `--folder <name>`, `--out <dir>`,
  `--raw-base <addr>` to import a raw code blob (e.g. a systemctrl reboot
  payload at `0x88FC0000`) with the BinaryLoader.
- The JSON schema is documented in `docs/analysis-json.md`; the recompiler
  consumes it as function-boundary hints.
- The `ghidra-allegrex` loader has a "Use reboot.bin Type B Relocation
  Mapping" import option (command line `-psp-useRebootBinTypeBRelocationMapping`)
  needed only for `sysmem.prx`/`loadcore.prx`, which are relocated by the PSP's
  reboot code. Headless cannot set loader options, so those two must be
  imported through the Ghidra GUI with that checkbox; every other module
  (all M33 modules and all normal game modules) needs no special handling.

Regression coverage: the Linux CTest `ghidra_headless` runs
`tests/tools/test_ghidra_analyze.py` with temporary launcher/JDK fixtures and
original synthetic module headers. It exercises real Python CLI/subprocess
execution, module grouping, arguments/environment, discovery, configuration
errors, dry runs and exit propagation, plus simulated Windows selection. It does
not establish actual Ghidra/Allegrex analysis or native Windows batch execution.

---

## 7. Extract the official Sony 4.01 update

The M33 installer carries the 11 M33 patch modules, not the Sony kernel, drivers,
codecs, XMB, fonts and resources. The user must supply Sony's 4.01 update as `401.PBP`;
it is not distributed with PSPC. The implemented `pspc-decrypt` tool unpacks and decrypts it:

```
pspc-decrypt psar 401.PBP private/fw/401 --model 02g
```

The output root contains `flash0/`, mirroring the on-device layout
(`kd/*.prx`, `vsh/module/*.prx`, `font/*.pgf`, `vsh/resource/*.rco`, `data/*`),
plus a manifest and other extracted outputs; see the [decrypt tool's output contract](../tools/pspc-decrypt/README.md#firmware-updates-psar).
Genuine Linux Sony 4.01 extraction, CLI/GTK installation and bounded XMB workflows
have passed; their distinct inputs and limits are recorded in [Linux firmware validation](linux-port.md#sony-401-firmware-installation-and-xmb-validation).
This is not evidence of actual Linux Ghidra analysis.

The inventory and analysis commands can then use the extracted tree (Windows examples):

```
REM inventory every decrypted module + the M33 modules together
PY -I tools/fwinventory.py private\fw\401\flash0 private\fw\401\m33 ^
   -o private\fw\401\inventory.json

REM Ghidra-analyse the kernel + driver modules (kernel ones auto-base 0x88000000)
cmd /c tools\ghidra_analyze.cmd private\fw\401\flash0\kd

REM and the VSH/user modules (auto-base 0x08804000)
cmd /c tools\ghidra_analyze.cmd private\fw\401\flash0\vsh\module
```

Notes for that run:
- `fwinventory.py` over both trees gives a complete dependency graph: M33
  modules' imports then resolve to the Sony modules that provide them instead
  of showing as external.
- Re-run `tools/nidgen.py` is not needed, but the inventory and Ghidra naming
  improve automatically as more Sony NIDs land in `data/nids.csv`.
- `sysmem.prx` and `loadcore.prx` need the GUI import with the reboot type-B
  relocation option (see 6); script the rest headless.
- The `pspbtcnf*.bin` files already extracted (and the decrypted
  `flash0:/kd/pspbtcnf.bin`) list the Sony module load order and modes; feed
  them to `fwinventory.py` to read the mode/module tables.

## 7b. Installing the firmware for pspc -- `tools/pspc_install_fw.py`

Copies the decrypted firmware's `flash0/{vsh,font,dic,data,kd}` and optional M33
`satelite.prx` / `recovery.prx` into a named profile's `flash0`, prepares its `flash1`, and AOT-compiles the
user-mode modules of `flash0/vsh/module` with `tools/pspc_build.py`. Then `pspc --vsh` boots
the XMB ([VSH boot](vsh-boot.md)).

Profiles live under `<data dir>/firmware/<profile>`. Use [Firmware Manager](firmware-manager.md)
to build, select, rebuild or delete them. The CLI requires `--profile`; existing names are
not overwritten. Use `--rebuild` to update native code without resetting PSP settings.

On Linux, `--data-dir` is required for the writable data root; firmware AOT
also requires explicit writable `--cache` and `--work` paths. Use the runtime's
[XDG user-data/cache roots](linux-port.md#linux-runtime-and-gtk-user-data-paths), not the installed
executable directory. `--build-dir` or `--pspc-dir` selects the build/installed SDK separately.
On Windows, the data directory defaults to `<pspc dir>`; the original examples below use that default:

```
PY -I tools/pspc_install_fw.py --profile 4.01 --fw private/fw/401 --build-dir build/RelWithDebInfo
PY -I tools/pspc_install_fw.py --profile 4.01 --rebuild --aot-core  REM core native modules only
PY -I tools/pspc_install_fw.py --profile 4.01-files --no-aot       REM new files-only profile
```

`pspc-decrypt prx` also decrypts the XMB's index (`flash0/vsh/etc/index.dat`, a `PSPsysGP`
container keyed like a module); the runtime's sceResmgr calls it for that.

## 8. One-shot: everything from the installer

From the repo root, with the toolchain in place:

```
PY -I tools/m33extract.py reference/401M33/UPDATE/EBOOT.PBP -o private/fw/401/m33
PY -I tools/nidgen.py --pspsdk reference/pspsdk --ppsspp reference/ppsspp ^
   --uofw reference/uofw --m33 reference/401M33 ^
   -o data/nids.csv --json data/nids_by_library.json
PY -I tools/extract_m33_nid_resolver.py private/fw/401/m33/systemctrl.prx ^
   -o data/m33_nid_resolver.csv
PY -I tools/nids_to_psplibdoc.py data/nids.csv -o data/psplibdoc.xml
PY -I tools/fwinventory.py private/fw/401/m33 -o private/fw/401/inventory.json
cmd /c tools\build_pspdecompiler.cmd
cmd /c tools\ghidra_analyze.cmd private\fw\401\m33
```
