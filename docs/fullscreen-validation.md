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
