# Firmware Manager

Open **Firmware Manager** in either launcher (Windows sidebar or Linux action bar).
Stop running games/XMB and finish or cancel builds first. Also close PSPC instances
started outside this launcher before migrating or deleting firmware.

- **Build from update…**: enter a unique name such as `4.01`, choose your Sony update
  `EBOOT.PBP`, and watch the progress in the launcher log (Linux) or build console
  (Windows). PSPC decrypts the update for the PSP-2000/02g model and builds the core
  XMB modules when an AOT SDK/build tree is available; otherwise it installs files
  for interpreter use. A successful new installation becomes active. A failed build
  never replaces an installed profile or changes the selection.
- **Rebuild native**: rebuild the selected profile's core native modules after updating
  PSPC. This requires the AOT SDK/build tree and does not replace its flash files or
  reset PSP settings. It does not change the active profile.
- **Use selected**: persist the selection for subsequent game launches and **Boot XMB**,
  including launches from the command line. The active row is marked in the manager.
- **Delete…**: confirm the exact profile and path before permanently removing its
  firmware and its PSP settings/themes. Active firmware cannot be deleted: switch
  first. Game saves, other profiles, and the shared content-addressed AOT cache remain.

**Compatibility:** the native runtime implements 4.01/M33. Storing/building another
version does not establish that its XMB or modules work. The `fw` setting only changes
what version is reported to games; it is separate from the selected firmware files.
No Sony firmware is bundled or downloaded automatically. M33 UI modules can be included
through the decrypted-tree CLI installer below.

## Storage and existing installations

Profiles live under `<data root>/firmware/<name>/{flash0,flash1}`. The data root is
`$XDG_DATA_HOME/pspc` (default `~/.local/share/pspc`) on Linux, or the executable
folder on Windows. Names use 1–64 lowercase letters, digits, dots, underscores and
hyphens, begin/end with a letter or digit, and cannot be Windows device names.

The previous top-level `flash0` and `flash1` trees are moved once into
`firmware/default` when opening the manager or starting a runtime using default flash
paths. Nothing is copied or deleted; the existing registry is preserved. A conflict
with an existing destination is reported instead of merging or overwriting files.
Back up important firmware settings and stop all instances before this migration.
The manager and runtime use only the new profile layout afterward.

`firmware=<name>` in `pspc.ini` selects a profile (`default` initially). AOT modules
remain in the shared cache, keyed by content and PSPC semantics version. Saves remain
on the shared memory stick. Builds use temporary staging directories and publish only
on success; normal failure/cancellation cleans these up. A hard kill/power loss can
leave a hidden `.building-*` directory under `firmware`; inspect it and confirm no
build is running before manually removing it.

## Command line

Install a new named profile from a decrypted tree (Linux source-build example):

```sh
python3 -I tools/pspc_install_fw.py --profile 4.01 --fw /path/to/decrypted \
  --pspc-dir "$PWD/build/LinuxRuntime/bin" --build-dir "$PWD/build/LinuxRuntime" \
  --data-dir "${XDG_DATA_HOME:-$HOME/.local/share}/pspc" \
  --cache "${XDG_CACHE_HOME:-$HOME/.cache}/pspc/aot" \
  --work "${XDG_CACHE_HOME:-$HOME/.cache}/pspc/firmware-work" --aot-core
```

Use `--no-aot` instead of the AOT options to install files only. Existing names are
never overwritten. Use `--rebuild --profile <name>` with the same destination/SDK/cache
options to compile an installed profile; `--force` recompiles up-to-date modules too.
Omit `--aot-core` to compile all user-mode VSH modules. Windows defaults `--data-dir` to
`--pspc-dir` and can use its bundled SDK/Python. See `--help` for full options.

The CLI installer does not select the new profile. Use the manager, set
`firmware=4.01` in `pspc.ini`, or launch once with `pspc --set firmware=4.01 --vsh`.
Explicit `--flash0` and `--flash1` override profile paths independently. A supplied
root is never migrated; only the omitted root uses profile defaults and participates
in migration. A later launch without overrides can migrate the remaining legacy tree.
When both roots are supplied, no migration is performed.

`pspc --firmware-paths` prepares the same default roots (including migration) and prints
JSON containing `flash0` and `flash1`, without booting a guest. It honors `--config`,
`--set firmware=<name>` and explicit flash overrides. This is not a read-only query:
stop other PSPC instances first if legacy flash trees still need migration.

`tools/run_autotests.py` and `tools/smoke_games.py` use this command once before their
parallel runs. They use the selected profile (or their explicit `--flash0` override)
and pass both resolved roots to every guest; `--config none` still isolates other user
settings. Autotest dependency checks and smoke reference metadata therefore use the
same firmware files as the runtime, with no executable-local or other-build fallback.
