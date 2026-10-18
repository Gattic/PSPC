# Opt-in local SDL fullscreen fix (Linux)

PSPC normally links/loads the host's SDL2. On SDL2-compat/SDL3 systems, the
Wayland/libdecor restore defect described in [fullscreen validation](fullscreen-validation.md)
can remain after the host-side fix. The optional `pspc-local-sdl` target builds
**SDL 3.4.18 with that exact patch and sdl2-compat 2.32.74**. The latter is needed
because original SDL2 does not use SDL3 at all.

The current patch's full matrix sample passed **20/20 Wayland and 20/20 XWayland**
cases on the tested Fedora/GNOME setup; every case loaded both intended libraries.
**The bounded old-stack comparison is complete:** both older stacks failed 3/3
focused samples, while the current stack passed 3/3. After an initial budget stop,
user-authorized, verified cache archival allowed only the five remaining samples
to run within the unchanged caps. See the [reconciled results](fullscreen-validation.md#bounded-old-stack-reconciliation-2026-10-18)
and [scale/configure regression](fullscreen-validation.md#scaleconfigure-follow-up-2026-10-18).
These finite observations support the tested fix; they do not establish a zero
intermittent-failure rate, other-compositor conformance or retail gameplay.

This is a source-build workflow, not a system upgrade. There is no sudo, install
step, global loader setting, application RPATH change, or save migration. Normal
builds, direct launches, menu entries and runtime archives continue to use host
libraries. Windows is unchanged.

## Build

First use the normal [Linux runtime prerequisites](linux-port.md) and build:

```sh
JOBS=2 PSPC_CMAKE_ARGS='-DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSPC_BUILD_RUNTIME=ON' ./build.sh LinuxRuntime
# Explicit opt-in; normal ALL/install/CPack targets do not build or download SDL.
cmake --build build/LinuxRuntime --target pspc-local-sdl
```

The local target additionally needs `patch`, network access to GitHub for the
first configure, and SDL's development dependencies. In particular, it requires
Wayland (including wayland-scanner), xkbcommon, libdecor, X11, OpenGL/EGL, and at
least one of ALSA, PulseAudio or PipeWire. Install the development packages using
your distro's normal procedures, or provide a private SDK; the target itself
never installs packages. See SDL's pinned [Linux dependency list](https://github.com/libsdl-org/SDL/blob/release-3.4.18/docs/README-linux.md).
On Fedora the relevant packages include `wayland-devel`, `libxkbcommon-devel`,
`libdecor-devel`, the X11 development libraries including `libXScrnSaver-devel`
and `libXtst-devel`, `mesa-libGL-devel`, `mesa-libEGL-devel`, `fribidi-devel`, and
`pulseaudio-libs-devel` (or the ALSA/PipeWire equivalent). SDL's configure output
lists additional detected features.

The target uses two compiler jobs and a separate Release CMake tree at
`build/LinuxRuntime/local-sdl/`. It pins both archive SHA-256 hashes in
`cmake/local-sdl/CMakeLists.txt`; `PatchSDL.cmake` checks the Wayland source and
header before and after patching, refuses drift, and accepts an already-correct
patch. The checked-in `wayland-configure-size.patch` separates raw libdecor
configure history from pre-scale geometry and initializes the floating state.
Repeated target builds reuse downloads and compile only changed inputs.
When updating from an older patch, recreate only the generated `local-sdl/` tree
(or use a fresh project build directory); stale patched inputs deliberately fail
rather than silently retaining the old fix. Preserve private SDK configuration
arguments before recreating that tree. The
resulting libraries are in `local-sdl/lib/`; no dependency comes from a historical
validation directory. Upstream source and license files remain in `local-sdl/_deps/`.

Missing required video or desktop-audio features fail configuration rather than
silently creating a dummy-only library. Other optional SDL features still depend
on available headers/libraries; this is not a promise of complete distro-feature
parity. The target runs its display-independent SDL regression and then checks
actual loaded SDL2 and SDL3 paths.

For a private SDK, configure the child tree once with your actual prefix and
include paths, then run the project target above. For example:

```sh
PKG_CONFIG_PATH="$SDK/lib64/pkgconfig" cmake -S cmake/local-sdl \
  -B build/LinuxRuntime/local-sdl -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$SDK" -DCMAKE_C_FLAGS="-I$SDK/include"
```

SDK pkg-config files must reference their actual private paths. Extra include
roots, such as `$SDK/include/fribidi`, may be needed for relocated SDKs. The target
reuses this child cache. For another project build directory, substitute it in
all commands. The archive/patch inputs reproduce the source change; compiler,
system headers and optional features can change binary hashes.

## Enable for one launch

From the repository root:

```sh
python3 -I tools/run_local_sdl.py --build-dir build/LinuxRuntime --check
python3 -I tools/run_local_sdl.py --build-dir build/LinuxRuntime -- \
  build/LinuxRuntime/bin/pspc-launcher
# Or launch the CLI directly, passing its normal arguments unchanged:
python3 -I tools/run_local_sdl.py --build-dir build/LinuxRuntime -- \
  build/LinuxRuntime/bin/pspc --version
```

`--check` loads by SONAME in a fresh process, invokes SDL2's compatibility layer,
and verifies **both actual mapped library paths** against the local build. A
missing or unloadable library is an error, not a fallback to system SDL.

The helper prepends the local library directory to `LD_LIBRARY_PATH` only for
the requested command and its descendants. GTK-launched runtimes inherit it;
arguments are passed without a shell and the command's exit status/signals are
preserved. Existing nonempty library-search entries are retained. Conflicting
`LD_PRELOAD`, `SDL_DYNAMIC_API` or `SDL3_DYNAMIC_API` overrides must be unset
explicitly. Local build paths containing `:` are rejected because it is the loader's
search-path separator. The helper does not change video/audio drivers, settings, user paths
or render resolution. Run it around PSPC commands, not your whole desktop session.
Do not use loader-injected/setuid binaries with this workflow.

For loader evidence from the **actual runtime**, rather than the independent
probe, inspect this command's stderr (both SDL paths should be under `local-sdl/lib`):

```sh
LD_DEBUG=libs python3 -I tools/run_local_sdl.py --build-dir build/LinuxRuntime -- \
  build/LinuxRuntime/bin/pspc --version
```

## Disable

Close the wrapped launcher **and its games**, then launch without the helper:

```sh
build/LinuxRuntime/bin/pspc-launcher
# Or:
build/LinuxRuntime/bin/pspc --version
```

No environment change persists in the invoking shell; no rebuild, uninstall or
system-library restoration is needed. An already-running wrapped launcher keeps
its inherited selection until it exits. Merely omitting the helper does not undo
loader overrides that you set independently in your shell. Menu/packaged launches
remain system-SDL launches unless you explicitly wrap them yourself.

Keeping `local-sdl/` does not enable it. To reclaim its generated source/build
files, remove only that selected build directory's `local-sdl/` after closing
wrapped applications; the next explicit target build recreates it. Never remove
user data or the separate historical validation evidence as part of disabling.

## Checks

The default `local_sdl_launch` CTest covers arguments (including spaces), child
environment inheritance, unchanged unwrapped launches, exit codes, missing/invalid
libraries and conflicting overrides. It requires neither downloaded SDL nor a display.

```sh
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^local_sdl_launch$'
python3 -I tools/run_local_sdl.py --build-dir build/LinuxRuntime -- \
  ctest --test-dir build/LinuxRuntime --output-on-failure \
  -R '^(host_audio|host_fullscreen_restore_.*|host_video_reset_.*)$'
# Native desktop only: creates/toggles a real window; never opens a game/save.
SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=dummy \
  python3 -I tools/run_local_sdl.py --build-dir build/LinuxRuntime -- \
  build/LinuxRuntime/bin/pspc_host_fullscreen_tests windowed 16 opengl resize
```

The optional build also registers `wayland_scale_restore` in its **child** CTest
tree. It compiles the pinned SDL initialization, scale-handler and libdecor sizing
code with SDL's real private types, substituting native inputs/side effects rather
than reimplementing the sizing algorithm. It covers initial scale changes, both
stale-size histories, genuine user resizes, and fixed fullscreen geometry:

```sh
ctest --test-dir build/LinuxRuntime/local-sdl --output-on-failure -R '^wayland_scale_restore$'
```

The full native matrix uses presenters `sdl`/`opengl`, startup `windowed`/`fullscreen`,
scales `1 4 8 16`, plus `resize` at scale 16 for all four presenter/startup pairs
(20 cases per driver). Repeat with `SDL_VIDEODRIVER=x11` for XWayland.
These synthetic checks do not establish retail progression, real audio quality,
exclusive fullscreen, Windows behavior or broad SDL conformance.

The host-only Linux runtime package policy is unchanged. Its matching source
snapshot includes the optional build recipe, patch and helper, but the runtime
archive does not bundle these downloaded libraries.
