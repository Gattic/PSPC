# Linux runtime package

PSPC's Linux runtime build can install a complete **host-dependent application tree** or
produce a relocatable `.tar.gz` containing that tree. It includes the runtime, GTK launcher,
AOT SDK/tools, firmware helper, read-only data, Python server packages, documentation,
license notices and a matching Linux source/build-input snapshot. No games, Sony firmware,
saves, account databases, caches, Python interpreter or compiler are bundled.

This is not an AppImage or a cross-distribution binary guarantee. The package uses the
build's architecture and ABI; build it on the distro/ABI baseline you intend to support.
It has **no installed RPATH/RUNPATH** and no bundled shared libraries. Host library updates,
GPU drivers, fonts, GTK modules and TLS certificates remain the distro's responsibility.

## Build, package and install

Configure a runtime build with the [Linux prerequisites](linux-port.md). For the existing
`LinuxRuntime` build directory used by the validation instructions:

```sh
JOBS=4 PSPC_CMAKE_ARGS='-DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPC_BUILD_RUNTIME=ON' ./build.sh LinuxRuntime
cpack --config build/LinuxRuntime/CPackConfig.cmake -B build/linux-package
```

The result is `PSPC-<version>-Linux-<architecture>.tar.gz` and its `.sha256` file. CPack strips
installed executables, not the build-tree binaries. The archive has one top-level directory;
extract it into an **empty, dedicated** location and run `./pspc-launcher` there. The tree can
be moved or made read-only. It is not an FHS `/usr/bin`/`lib`/`share` layout: do not extract it
into `/usr` or overlay it on another application.

For direct installation without an archive, use the same CMake rules:

```sh
cmake --install build/LinuxRuntime --prefix "$HOME/Applications/PSPC-0.1.1"
"$HOME/Applications/PSPC-0.1.1/pspc" --version
"$HOME/Applications/PSPC-0.1.1/pspc-launcher"
```

Use a versioned prefix appropriate to your build. Installing without `--component` installs
`Runtime`, `AOT` and `Sources`; `Runtime` alone is not the complete application (tools and
shared license files belong to `AOT`). The existing tools-only `--component AOT` install is
unchanged. Only the runtime build enables the full CPack archive. Use `CPackConfig.cmake`,
not a blanket source-tree archive: private/reference/build folders must never be packaged.

## Application menu registration

After installing/extracting into its final versioned location, register **for the current
user**, without sudo:

```sh
python3 -I "$HOME/Applications/PSPC-0.1.1/tools/pspc_desktop.py" install
# Before removing the currently registered installation:
python3 -I "$HOME/Applications/PSPC-0.1.1/tools/pspc_desktop.py" remove
```

The helper writes `io.github.Gattic.PSPC.desktop` under `$XDG_DATA_HOME/applications`
(or `$HOME/.local/share/applications`). Relative XDG overrides are ignored; the fallback
requires an absolute HOME. CMake/CPack never modify user menus automatically. The entry
opens the game-library launcher; it does not claim ISO/PBP MIME associations or accept URLs.
It uses absolute executable/icon paths, without a shell, PATH changes or writes inside the
package. `data/pspc.png` is the existing 256px PNG from `src/frontend/pspc.ico`, unchanged.
Python 3 and `/usr/bin/env` are host prerequisites; desktop-file-utils is needed only for tests.

Registration supports spaces, Unicode, literal percent signs and shell metacharacters in
install paths. Paths containing `=` or control characters are rejected before writing an
entry. Re-run `install` after moving the tree, or from a new version to switch the managed
entry to that version. It atomically replaces only PSPC-managed entries, never an unmanaged
entry or symlink. `remove` is idempotent and removes only the exact entry generated for that
installation: an older version cannot remove a newer version's registration, and edited
entries are left intact with a diagnostic. Managed entries are regenerated, not merged, on
`install`; do not store custom edits there. Neither action removes user data or the package.
No system-wide registration, desktop-folder shortcut, MIME cache or package-manager hook is
installed.

## Host dependencies and ABI boundary

Install compatible **runtime** libraries through the target distro: SDL2, GTK3 (3.22+),
OpenGL/GLX and a driver, fontconfig/FreeType, curl/TLS, FFmpeg's avcodec/avutil/swresample/
swscale, and the C/C++ runtimes. Exact required SONAMEs are in the installed ELF binaries:

```sh
readelf -d ./pspc
ldd ./pspc
ldd ./pspc-launcher
```

`not found` means the matching distro dependency must be installed; copying libraries from
a random developer tree or restoring its RUNPATH is not the package policy. Versioned
SONAMEs alone do not establish a minimum glibc/libstdc++ symbol baseline. No older-distro ABI
promise is made. The initial relocation gate targets this build host (Fedora 44 x86-64;
FFmpeg 62/60/6/9 SONAME family), not every Linux distribution.

Python 3 is required for servers/firmware/native builds. Native compilation also requires
a compatible C++20 GCC/Clang toolchain with development headers/libraries. Clang is the
builder default; `CXX=/usr/bin/g++ ./pspc-launcher` selects GCC. Direct native builds need
no CMake/Ninja; the optional `--cmake` route needs both. Hardware rendering additionally
requires the documented OpenGL capabilities; headless/interpreter tests do not prove them.

## User data, updates and removal

Configuration, saves, firmware, game/plugin folders, caches and logs use the shared XDG
layout, **not** the package directory. See [paths and safe legacy-data migration](linux-port.md#linux-runtime-and-gtk-user-data-paths).
Existing executable-local saves/settings are not automatically copied or merged.

For an update, stop PSPC and hosted servers, back up user data, extract into a new empty
versioned directory, and test that installation before running its desktop helper's `install`
action to switch the application-menu entry (or changing any other user-managed shortcut).
Keep each runtime with its matching AOT SDK. Stale native caches are checked by semantics
version. Never overlay different versions or copy a build's whole `bin/` directory into
a package. Removing an old application tree does not remove XDG data; do not delete those
user directories as part of an application uninstall. Unregister the currently selected
installation before removing its tree. There is no package-manager database or automatic
upgrade/uninstall hook.

`source/` contains the current Linux build inputs, including uncommitted source edits, not
just a URL to a potentially different revision. To rebuild with the Linux development
dependencies installed, use an out-of-tree directory (the installed tree may be read-only):

```sh
cmake -S /path/to/PSPC/source -B "$HOME/pspc-rebuild" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$HOME/pspc-rebuild" --parallel 4
```

Third-party system libraries are not redistributed; their source/licenses come from the
distro. PSPC and its embedded components' notices are in `LICENSE`, `THIRD_PARTY.md`,
`licenses/` and the corresponding source headers. This Linux snapshot does not bundle
Windows dependency downloads or private firmware/reference assets.

## Verification and retention

```sh
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^(runtime_package|launcher_paths|desktop_integration)$'
python3 -I tests/launcher/test_desktop.py --build-dir build/LinuxRuntime --gui
python3 -I tests/launcher/test_package.py --build-dir build/LinuxRuntime \
  --compiler /usr/bin/g++ --isolate
python3 -I tests/launcher/test_ui.py --build-dir build/LinuxRuntime \
  --evidence-dir build/linux-runtime-package-review/ui
```

Use the configured compiler where it differs. `runtime_package` builds/checksums/extracts the
actual CPack archive, checks payload exclusions and matching source inputs, rejects RPATH and
absolute needed-library names, then checks native execution, resources, GTK listing, XDG
settings/save preservation and installed Python server/database operations. `--isolate`
requires bubblewrap/user namespaces and mount-hides the entire repository, including build
libraries; the relocated payload is read-only and loader/Python environment overrides cannot
supply missing project dependencies. Missing isolation support is an error, not a pass.
The opt-in real GTK workflow now also stages only actual CMake installs, with no manual
runtime/resource copy fallback. `desktop_integration` requires `desktop-file-validate` and
`gio`; it tests registration, real headless GIO activation, quoting, XDG selection, relocation,
update/removal ownership and unchanged package/user data. `--gui` additionally requires an
X11 display and PyGObject with GioUnix (GLib 2.80+), Gdk and Atspi typelibs: it opens the real
GTK launcher via the registered entry, identifies its owned PID/window and closes it cleanly.
For a virtual display use `dbus-run-session -- xvfb-run -a` before that Python command.
Neither mode touches the real user's application registry.

Tests use temporary directories and remove archives, CPack staging, native objects and test
user data on exit. Expect less than 1 GiB temporary space per check for this configuration.
For manual packaging, reuse one dedicated output directory: keep at most the current and
one previous validated archive/checksum, and remove disposable CPack staging when finished.
Never apply that retention rule to authored assets, saves or historical research evidence.

## Fixture-free CI

The Linux package workflow, `.github/workflows/linux-package.yml`, targets Ubuntu 24.04
x86-64 on pull requests, main-branch pushes and manual dispatch. The workflow and shared
script also ship in the archive's matching `source/` tree. Its shared local entry point
is `bash tools/ci_linux.sh` after installing the development/test dependencies listed in that
workflow. It builds a fresh runtime configuration, runs verbose CTest **excluding only
`pspc_decrypt_tests`**, exercises the mount-hidden read-only package, opens/closes GTK through
the registered desktop entry under Xvfb, and produces the TGZ/checksum. A missing isolation
or display prerequisite fails the gate. Default build parallelism is two; the hosted job is
bounded to 30 minutes.

The excluded decrypt suite requires user-supplied M33 binaries; it remains a strict local
gate when those fixtures are available. No fixtures are downloaded or assertions weakened.
Some unit cases also omit reference comparisons when inputs are absent, and OpenGL checks
can omit unsupported hardware paths. Those diagnostics remain in verbose logs; **a green CI
job does not mean those checks ran**. Required hardware-renderer validation and the broader
interactive GTK/native/server workflow remain separate host-side gates.

Actions are SHA-pinned, the token has read-only repository permissions, checkout credentials
are not persisted, and no release is published. Logs (including failures) and successful test
packages are separate artifacts with seven-day retention; build trees and CPack staging are
not uploaded or cached between runners. The artifact name records Ubuntu 24.04/x86-64, not a
universal Linux ABI. Local rehearsal is not evidence of a hosted Actions run or artifact
upload; first hosted execution remains to be observed after these changes are committed.

Local evidence: native Fedora checks passed both builds, tools 4/4 and runtime 20/20 CTest
entries, mount-hidden archive execution and real GTK desktop activation. The complete
Ubuntu 24.04/GCC 13 shared recipe also passed: all 19 selected fixture-free CTest entries,
strict mount-hidden/read-only archive execution, registered GTK launch and clean closure
under Xvfb, and final TGZ/checksum creation. The initial container-root namespace failure
was resolved by running CI as an ordinary user in a rootless container with an existing
SELinux nested-engine domain; default seccomp/capabilities were retained, no-new-privileges
was enabled, and no host-wide security policy or isolation assertion was changed. The GTK
verifier's GIO call uses an explicit receiver for the supported typelibs' function/method
annotations. This is a complete local recipe pass, not a hosted Actions or upload result.

Synthetic gates do not validate Sony firmware extraction/XMB, retail compatibility, other
hosts/drivers, Windows, or signing/publishing. Distro-native package hooks, a supported
multi-distro ABI matrix and release distribution remain roadmap follow-ups.
