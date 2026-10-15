# Fullscreen crash investigation

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
