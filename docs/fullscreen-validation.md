# Fullscreen crash investigation

## Linux restore sizing (2026-10-18)

On Fedora 44 / GNOME with sdl2-compat 2.32.74 and SDL 3.4.18, the native
fullscreen fixture exposed two restore failures: XWayland could lose the
74-pixel title-bar height on each cycle, and native Wayland's SDL presenter
could restore 1920x1088 instead of 960x544 at 16x frame scale. These were sizing
failures, not device-reset crashes.

The host kept its own windowed size and called `SDL_SetWindowSize` immediately
after leaving fullscreen. That second request interferes with the native
restore's decoration/scale changes. An SDL-only XWayland probe reproduced the
height loss; removing only the extra resize preserved the original size.

SDL now owns the restore geometry rather than a second host cache. When SDL
presentation is selected at startup, the host creates the presenter **before**
requesting fullscreen, so renderer recreation does not lose the saved windowed
geometry. There is no delayed resize, retry, forced video driver, or change to
render resolution.

Four `host_fullscreen_restore_*` CTests exercise the real host loop and dummy SDL
presentation with injected native restore hazards: renderer recreation while
fullscreen, and a resize racing decoration or scale restoration. Both startup
modes also resize to 800x600 between fullscreen trips. All four fail before the
host change and pass afterward; they require no display server or guest saves.

```sh
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^host_fullscreen_restore_'
# Opt-in native windows; repeat with SDL_VIDEODRIVER=wayland and presenter opengl.
SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  build/LinuxRuntime/bin/pspc_host_fullscreen_tests fullscreen 16 sdl
# Also preserve an explicit user resize after the first fullscreen round trip.
SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=dummy \
  build/LinuxRuntime/bin/pspc_host_fullscreen_tests windowed 16 opengl resize
```

### Initial host-only results (system SDL)

The build and 20 affected host/runtime CTests pass. The native acceptance matrix
runs both presenters, both startup modes, scales 1/4/8/16, plus user-resize cases
at 16x: **XWayland 20/20 pass; native Wayland 15/20 pass** (320 transitions total).
Wayland's five failing cases are all at 16x, including direct OpenGL. This is a
**partial repair**, not a green fullscreen acceptance gate. The dummy fault
models protect the host ownership/order invariants, not all compositor behavior.

A native protocol trace shows the correct 960x544 restore and a completed frame,
followed by a later configure event overwriting the size with desktop-sized
geometry. A standalone, single-threaded SDL program with an asserted 960x544
windowed baseline reproduces 1920x1043 restoration with no PSPC code and no
post-fullscreen resize. SDL 3.4.18's `decoration_frame_configure` records the
substituted restore dimensions in `last_configure`; later repeated compositor
geometry can then pass its unchanged-size filter. This identified the dependency
repair boundary; the local dependency experiment below addresses it without
adding host retries or loosening native assertions.

### Locally patched SDL 3.4.18 (2026-10-18)

Built the exact upstream release twice with identical configuration: an
unmodified control and a narrow `decoration_frame_configure` patch. The patch
queries libdecor's supplied dimensions once and records those in `last_configure`,
separately from the floating dimensions substituted during restoration. A later
activation-only configure no longer looks like a new resize. Actual user resizes
remain effective; there is no forced resize, timer, driver override or retry.

| Gate | Result |
|---|---|
| Same-build unmodified SDL control | 17/20 native Wayland cases pass |
| Locally patched SDL | **20/20 native Wayland cases pass (160 transitions)** |
| Host CTests with patched SDL | **8/8 pass** |
| SDL-only probe | Both control and patched pass this sample; inconclusive alone |

The control fails SDL/startup-fullscreen/16x and both OpenGL/16x user-resize
cases, restoring 1920x1043 or 3840x2086 instead of the expected windowed size.
Every native case confirms the selected SDL3 library from its loader trace.
The native fixture, its assertions and timing are unchanged in this experiment.

The patch, both libraries, source archive, build recipe, hashes and all logs are
retained under `build/fullscreen-restore-fix-048f7f9/sdl-patched/`.
`manifest.json` pins the inputs and tested binaries; `BUILD.md` describes the
build and `native-patched/results.json` records the 20-case result. For example,
with a fresh output directory:

```sh
E=build/fullscreen-restore-fix-048f7f9/sdl-patched
python3 -I "$E/run_cases.py" "$E/patched/libSDL3.so.0" "$E/native-new-run"
```

**This validates a process-local dependency patch, not a system or packaging
upgrade.** The driver selects it with `LD_LIBRARY_PATH`; the installed SDL2,
SDL3 and libdecor hashes are unchanged. Default launches still use the unpatched
system dependency. This diagnostic build lacks optional audio development
backends; only dummy audio was exercised. XWayland's earlier 20/20 result used
the system library and was not rerun with this dependency. Retail progression,
Windows and broad SDL conformance remain outside this experiment.

Evidence and the SDL-only reproducer are retained locally in
`build/fullscreen-restore-fix-048f7f9/`; the original failures remain in
`build/linux-pull-check-048f7f9/`. These synthetic checks never open existing
memory sticks. They do not revalidate Windows, exclusive fullscreen, or retail
gameplay/save progression.

### Project build/launch integration (2026-10-18)

The initial [opt-in local SDL workflow](local-sdl.md) built the same hash-checked
patch from pinned upstream archives, plus sdl2-compat 2.32.74. It does not depend
on the earlier experiment's binaries or install anything into the system.
The separate build requires Wayland/libdecor, X11, OpenGL and a desktop audio
backend; this host's build includes PulseAudio (device quality remains untested).

Build and incremental rebuild pass. The launch helper's eight tests, patch
apply/idempotence/drift checks, eight host regressions plus the launcher CLI,
and default package/source-snapshot checks pass. Actual `pspc --version` loader
traces confirm system SDL before wrapping, both local libraries while wrapped,
and the original system libraries again afterward. Installed library hashes
remain unchanged.

**At this integration checkpoint, native geometry acceptance was partial:**
helper-mediated native tests passed **19/20 Wayland and 20/20 XWayland** cases. All 40 cases load the intended
local SDL2 and SDL3. Wayland's SDL/startup-fullscreen/16x case restores 1920x1043
at transitions 3/5/7 instead of 960x544, despite the exact patched source and
correct loader selection. The earlier 20/20 result above is a historical sample,
not a guarantee that this patch completely resolves native Wayland sizing.

A bounded protocol-traced comparison (three samples per stack) also finds a
480x272 restore in both the previous retained patched SDL3 + system shim and
the newly built pair. That different signature shows an additional limitation
on the previous stack too; it does **not** establish the cause of the untraced
1920x1043 failure. No assertions, timings or production patch were changed to
hide these results. These retained failures motivated the follow-up below,
separate from the passing reproducible-build and library-selection/disable checks.

Evidence: `build/local-sdl-integration/` (`selection.json`, `patch-check.json`,
`host-ctest.log`, `wayland/results.json`, `x11/results.json`, and `comparison-*`).
The default system-SDL launch/package policy and existing saves are unchanged.

### Scale/configure follow-up (2026-10-18)

The remaining failures exposed two SDL state-ownership problems:

- A newly allocated Wayland window started with `floating=false`. A preferred-scale
  event before the first configure therefore treated the application's pixel size
  as fixed logical geometry; a 2x-to-1x event could halve 960x544 to 480x272.
- The raw compositor history added by the first patch shared `last_configure`
  with substituted restore geometry and the pre-scale logical-size filter.
  A scale event overwrote that history, allowing a repeated desktop-sized
  configure to corrupt the restored size.

The maintained patch initializes floating state before scale callbacks and keeps
raw libdecor dimensions in separate backend fields. It preserves SDL's existing
pre-scale filter: simply removing that filter passed a narrower regression but
still failed the matrix (19/20 Wayland, a 1920x1088 restore). That failed attempt
is retained, not discarded or counted as acceptance.

The new `wayland_scale_restore` child CTest compiles actual pinned SDL geometry
code and private types. With the **same final regression fixture**, the previous
maintained patch fails four checks, the incomplete attempt fails the pre-scale
configure check, and the final patch has zero failures. Controls cover a genuine
800x600 user resize and fixed logical fullscreen geometry. No test timing,
settling bound, native assertion, frame scale or host resize policy was changed.

The project target rebuilds and runs this regression before its loader check.
The final native rerun passes **20/20 Wayland and 20/20 XWayland cases** (320
transitions), including all four user-resize cases on each driver. Every process
loads both intended local libraries. Three fixed-stack protocol-traced repeats
of SDL/startup-fullscreen/16x also pass, as do nine wrapped host/launcher CTests.
Patch replay from pristine source is byte-identical, repeat application is
idempotent, and source/header drift is rejected.

Evidence: `build/wayland-scale-fix/` (`regression-results.json`,
`regression-comparison.log`, `final-build.log`, `final-incremental-build.log`,
`patch-check/results.json`, `final-wayland/results.json`, `final-x11/results.json`).
The opt-in source/header hashes are in `cmake/local-sdl/PatchSDL.cmake`.

These results record passing deterministic regressions and a passing native
matrix sample. The subsequent bounded comparison below is now complete and
supports the corrected stack for the tested case, not an unrestricted claim that
intermittent failures are absent. System-library launches and packaging remain unchanged.
No installed libraries or existing saves were modified. At this checkpoint, the
separate retail OpenGL progression acceptance gap remained open.

### Bounded old-stack reconciliation (2026-10-18)

The conflicting counts belong to identifiable stacks and observation samples,
not a single interchangeable "SDL 3.4.18" result:

| Stack | SDL3 / SDL2 SHA-256 prefixes | Historical Wayland matrix | New focused passes |
|---|---|---|---|
| A: retained first patch + system shim | `13716d1cd9ff` / `7b904d7c5cec` | 20/20 | **0/3** |
| B: pre-fix integrated pair | `781b4acc76f4` / `fe9f66ee1657` | 19/20 | **0/3** |
| C: final scale/configure patch + built shim | `2c023ddb6719` / `fe9f66ee1657` | 20/20 | **3/3** |

A and B had the same earlier source patch but different binaries/build features
and shims. C contains the later repair; its 20/20 must not be attributed to the
old patch. The intermediate repair's other 19/20 (`7fd62322a5a9`, failing
SDL/windowed/8x at 1920x1088) is a fourth revision, not another result for C.

This follow-up preregistered three untraced samples per arm in fixed interleaved
order, using the **unchanged fixture binary, launch helper and comparison driver**.
The controlled differences were the selected stack and isolated HOME/XDG paths.
All nine samples confirmed both expected loaded libraries (72 transitions total).
Every A/B sample restored 480x272 at transitions 1/3/5/7 instead of 960x544;
all three C samples passed all eight transitions.
Thus a restore failure is reproducible on the retained old stack without
protocol tracing, and is not confined to the rebuilt dependency pair. The
historical 20/20 was a passing sample, not an exhaustive acceptance guarantee.
The old traced 0/3 results on each of A/B remain a separate observation regime.
Neither that half-size signature nor these new half-size failures alone establish
the cause of B's earlier untraced **1920x1043** failure.

**Status: bounded comparison complete; current stack passes 3/3.** All **9/9
planned samples** ran, with no retries or replacements. The initial budget stop
at 4/9 is retained in the original partial records. After explicit user approval,
only the task-generated per-sample caches were losslessly archived. Every archive
was reopened and fully decompressed; file SHA-256/size, member inventory, modes,
ownership, nanosecond modification times and extended attributes matched before
any redundant originals were removed. Only verified files and empty cache
directories were deleted; logs, results and other evidence were preserved.

Samples 5–9 then ran in their original order, with each new cache archived after
settlement. Nine archives preserve 279 files / 19,155,807 content bytes in 325,361
compressed bytes. Retained output is approximately **2.7 MiB of 16 MiB**, with
approximately **108.2 MiB of 128 MiB** combined evidence; staging and reservations
also stayed within the unchanged caps. The continuation crossed a user-approved
pause: input hashes and recorded environment matched, but desktop scheduling was
not frozen. This finite comparison is not a failure-rate estimate or a guarantee
for every compositor. The separate retail progression gap remained open at this
checkpoint. The [subsequent OpenGL readback fix](resolution-performance.md#linux-opengl-target-readbacks-2026-10-18)
reached Dark Mirror and Combat Ops' bounded endpoints, but Logan's Shadow still
missed its unchanged deadline at that stage. The later
[decoded-texture reuse repair](resolution-performance.md#linux-opengl-decoded-texture-reuse-2026-10-18)
reached all three original bounded endpoints, including Logan's Shadow in 99.205 seconds.
Those later observations do not replace the historical failures recorded here.

Evidence: `build/wayland-stack-comparison-20261018/` (`PLAN.md`, `manifest.json`,
`historical.json`, original partial records, `RESUME.md`, `resume-manifest.json`,
`resumed-results.json`, `completed-results.json`, `completed-summary.json`,
`resume-budget.jsonl`, and per-sample archives/verification manifests under
`archives/`). `completed-results.json` has exactly one observation for each index
1–9; the earlier raw summary's unlaunched sample-5 budget marker is not a run.
All input/system-library and preserved non-cache evidence hashes remained
unchanged. No production code changes, commits or existing-save access occurred.

## Fixed failure path

`src/runtime/host/sdl_host.cpp` used to ignore `SDL_RENDER_DEVICE_RESET` and keep
its streaming frame texture as long as the image dimensions stayed the same.
SDL requires **all textures to be recreated** after that event, even when their
public handles still exist.

This is a concrete crash path in the bundled Windows SDL 2.32.10 D3D11 presenter:

- `D3D11_WindowEvent` resizes the swapchain when the window size changes.
- If resizing reports device removal, `D3D11_HandleDeviceLost` releases texture
  driver resources, recreates the device, and queues `SDL_RENDER_DEVICE_RESET`.
- Updating the old texture returns an error. Drawing it subsequently dereferences
  its null driver data in `D3D11_SetCopyState`.

See [SDL's reset event contract](https://wiki.libsdl.org/SDL2/SDL_EventType) and
[the pinned SDL implementation](https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/render/direct3d11/SDL_render_d3d11.c).

The host now destroys the invalid texture on the reset event and recreates it
before uploading/drawing again. It also restores the retained frame when the guest
is paused or has not submitted another frame. It does not restart the emulator,
change render resolution, or fall back to another renderer.

This concerns **SDL's presentation renderer**, including the software GE path.
It is not a device-loss implementation for PSPC's separate `renderer=d3d11` GE
backend, and does not establish that every reported Windows fullscreen crash has
this cause.

## Deterministic Linux regression

With a runtime build, run:

```sh
cmake --build build/LinuxRuntime --target pspc_host_video_reset_tests
ctest --test-dir build/LinuxRuntime --output-on-failure -R '^host_video_reset_'
```

The tests run the real host event loop and SDL dummy/software renderer, with
link-time wrappers injecting the Windows device-reset texture lifetime contract.
They reject reuse of invalid resources through teardown and check the restored output
pixel for each reset generation, so a late pre-reset frame cannot satisfy the next
reset's acknowledgment. Resumed, paused, and scaled-frame cases each exercise two resets. All three fail
before the fix and pass afterward. This is fault injection, not a native Windows
GPU reset reproduction.

## Native Linux checks (2026-10-15)

The initial checks used Fedora 44, X11 through XWayland, SDL2 compatibility library
2.32.74, and NVIDIA GTX 1050 Max-Q / driver 580.178.04 for OpenGL. Each of Combat Ops,
Dark Mirror, and Logan's Shadow ran with both the software and OpenGL GE renderers
(interpreter, OpenGL render scale 2, vsync off). Dark Mirror started fullscreen;
every case then entered/exited fullscreen four times with F11. The window manager's
fullscreen property was checked after each transition.

All six 22-second runs passed both before and after the fix: 12 runs and 48 confirmed
fullscreen transitions in total, each ending at the requested timeout with zero bad
guest-memory accesses and captured guest frames. No native fullscreen crash was reproduced.
The runs reached boot/title/menu screens, not a prolonged gameplay session. Audio
used SDL's dummy driver. Saves, firmware roots, config, and caches were isolated;
existing user saves were not modified.

Task-local commands/results are retained under `.pi/fullscreen/` and
`build/fullscreen-validation/` (both ignored by Git).

## Remaining Windows confirmation

Re-test the affected game on the reporting Windows machine, recording the renderer
setting, GPU/driver, game, whether fullscreen is selected at startup or via F11,
and any crash log/backtrace. Exercise repeated F11 transitions and minimize/restore.
The log message `SDL render device reset; recreating the frame texture` identifies
this recovery path. Normal fullscreen transitions need not reset the device.

Native Windows execution, exclusive fullscreen, and extended gameplay were not
validated here. A crash in PSPC's direct D3D11 GE backend still needs its own trace.

## Native Windows follow-up (2026-10-15)

The Windows build was revalidated on an NVIDIA GeForce RTX 4060 Laptop GPU, driver
32.0.15.8157, with a 1920x1200 desktop and the bundled SDL 2.32.10. The opt-in native
window fixture runs the actual SDL host loop, substitutes F11 event delivery, and
checks fullscreen flags and restored window dimensions after each transition:

```bat
build\RelWithDebInfo\bin\pspc_host_fullscreen_tests.exe windowed 1 sdl
build\RelWithDebInfo\bin\pspc_host_fullscreen_tests.exe fullscreen 16 d3d11
```

Both startup modes, both presentation paths (`sdl` and direct `d3d11`), and frame
scales 1, 4, 8 and 16 passed eight transitions each: 128 transitions, including
restoration to the original 960x544 window. These use synthetic frames and native
swapchains; they do not reproduce GPU device removal or exercise physical key delivery.

Daxter also ran fullscreen for 180 seconds at D3D11 scale 8 in the interpreter,
capturing 17 distinct frames and reporting zero bad guest-memory accesses. Fresh AOT
builds of Daxter at scale 16 and Combat Ops at scale 8 reached their requested VBlank
limits with zero bad accesses. Their captured images were 7680x4352 and 3840x2176.
The visible runs reached menus/save dialogs, so they do not establish sustained
high-resolution gameplay. Separately, both games matched all 11 existing headless
software-renderer gameplay checkpoints with fresh native modules.

No crash occurred in these checks. Extended high-resolution gameplay, minimize/restore,
exclusive fullscreen and forced native Windows device loss remain unverified.

## Ubuntu/WSLg follow-up (2026-10-15)

The same native-window fixture now supports Linux OpenGL presentation. Ubuntu 22.04
under WSLg, SDL 2.0.20 and a 1920x1200 desktop passed both startup modes, both `sdl`
and `opengl` presenters, and scales 1, 4, 8 and 16: **128 fullscreen transitions**.
Each return to windowed mode restored 960x544. The final host change also passed the
same 128-transition Windows matrix again.

This exposed a startup-fullscreen restore bug in the older SDL/X11 path: creating
a fullscreen window, or recreating it for SDL's OpenGL presenter, could retain the
desktop dimensions as the windowed size. The host now creates the requested windowed
geometry first and preserves its size across fullscreen transitions. The fixture
waits up to two seconds for asynchronous window-manager geometry changes while
continuing to pump native events.

WSL's hardware drivers here expose OpenGL 4.1/4.2, and llvmpipe exposes OpenGL 4.5
without fragment-shader interlock. These OpenGL checks therefore explicitly used
`LIBGL_ALWAYS_SOFTWARE=1 PSPC_GL_SERIAL_FRAGMENTS=1`. The latter is an opt-in
verification path that submits one primitive per draw with image-memory barriers;
the normal renderer still requires ordered fragment interlock. This validates
software OpenGL execution, not hardware acceleration on a native Linux GPU.

```sh
export LIBGL_ALWAYS_SOFTWARE=1 PSPC_GL_SERIAL_FRAGMENTS=1 SDL_AUDIODRIVER=dummy
build/LinuxRuntimeValidation/bin/pspc_host_fullscreen_tests fullscreen 16 opengl
PSPC_AOT_RENDER_TEST=1 PSPC_RENDERER_REQUIRE=1 \
  ctest --test-dir build/LinuxRuntimeValidation -V -R '^aot_runtime$'
```

The AOT runtime probe checked three guest-generated colors, GPU writeback and every
display-image pixel at 1x, 2x, 4x, 8x and 16x in both native and interpreter modes.
The 16x image was 7680x4352. Strict OpenGL unit/reference comparisons also passed.
Evidence is retained in `build/linux-fullscreen-matrix-final.log`,
`build/validation-linux/fullscreen/`, `build/linux-aot-render.log`, and
`build/linux-ctest.log`. These synthetic checks do not establish sustained retail
gameplay, physical key delivery or native GPU performance.

Separate 30-second WSLg fullscreen boot runs of Combat Ops (configured scale 4)
and Daxter (configured scale 8) completed at their requested timeouts with zero
bad guest-memory accesses. Their eight and two captures respectively showed boot
screens at 480x272; these runs do not prove high-resolution retail GE rendering.
An additional Openbox/Xvfb native-window check passed after the harness waited for
the window manager to become ready before creating SDL windows. Evidence is under
`build/validation-linux/retail/`, `build/linux-retail-wslg.log` and
`build/linux-openbox-ready.log`.

Both retail cases also passed under an isolated Xvfb/Openbox desktop once the
window manager was ready. Each accepted four F11 events sent through X11, with
`_NET_WM_STATE_FULLSCREEN` checked after every transition. Both reached their
30-second timeouts with zero bad accesses; captured frames again showed native-size
boot screens. See `build/linux-retail-x11-final.log` and
`build/validation-linux/retail-x11/` for these eight additional transitions.
