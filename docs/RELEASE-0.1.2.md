# PSPC 0.1.2

PSPC 0.1.2 makes high-resolution rendering faster, adds named firmware profiles and fixes the
launcher asking to rebuild native code after every session. It also brings the GameSpy server up
to the point where Star Wars Battlefront: Elite Squadron and Star Wars Super Squadron can be
played online.

## Faster graphics

- With the Direct3D 11 renderer, triangle transform, lighting and clipping now run on the GPU
  with results that match the CPU exactly. The remaining vertex work is recorded and spread over
  the worker threads. This helps most at the higher render scales.
- Texture sampling at higher resolutions is cheaper, and the Syphon Filter games no longer make
  Direct3D 11 copy its rendering back to guest memory tens of thousands of times.
- The load/save lists and the HOME menu are composited only when they change, instead of on every
  frame; with a large window that was about 10 ms of each frame.
- Textures that a game draws without their alpha channel no longer turn black at the edges when
  upscaled with xBR.
- The OpenGL renderer (Linux builds) gets the same GPU transform. On Windows, Direct3D 11 remains
  the GPU renderer.

## Firmware profiles and launcher

- Firmware is now kept in named profiles (`firmware\<name>\flash0` and `flash1`). The launcher can
  build, rebuild, select and delete profiles; firmware installs are staged so existing files are
  never overwritten, and firmware from earlier versions is migrated into a profile.
- **Play** no longer finishes with nothing happening when an empty `flash0` folder sits next to a
  migrated profile, and the native build shows its progress.
- The window's size is restored when leaving full screen, and the software renderer's window
  recreates its frame after a render device reset.
- A game's native code no longer shows **REBUILD** after every session. PSPC records only
  addresses a rebuild can use and writes them only when a run finds new ones, and the launcher
  tells a PSPC update apart from newly found code. After installing, a game that was already
  asking to be rebuilt may need one last rebuild.

## Online

- Star Wars Super Squadron joins Elite Squadron on the **Online** page's GameSpy card. Its
  GameSpy names are redirected to the public server `swss.pspc.online` by default; starting the
  server on your own PC or choosing OpenSpy overrides that.
- The game's server list now shows servers: Super Squadron pings them with ICMP echo, and PSPC
  answers that itself where Windows would not give it a raw socket.
- The GameSpy server was hardened for hosting on the internet (limits for every connection and
  protocol, bounded parsing, refusals logged with their reason) and reviewed: the 28 findings and
  their fixes are in `docs\gamespy-server-security.md`.
- The server can run sessions (dedicated game servers), list games hosted by players with a limit,
  tell its own games apart on the Join screen, read more games from `gamespy-games.json` and
  carry games through a Cloudflare Tunnel. Setup for a public server is in
  `docs\gamespy-server.md` and `server\deploy\swss`.
- New options for hosts: `game_port` in `pspc.ini` binds a game's UDP socket to a fixed port, and
  `--renderer none` runs display lists without drawing, for dedicated servers on machines without
  a GPU.

## Compatibility and correctness

- Fixed network timeouts (WinHTTP receive deadlines and cancellation), an aborted UDP receive that
  could resume twice, scheduler switch costs after a thread was deleted, and renaming or deleting
  files a game still has open.
- Recorded pspautotests results are still **539 of 559 passing**, with the same 20 tracked
  differences in interpreter and native modes.
- Linux support (OpenGL, a GTK launcher, a relocatable runtime package) is in the source and
  checked by CI. This release publishes the Windows installer only.

See [game compatibility](https://github.com/Gattic/PSPC/blob/v0.1.2/docs/COMPATIBILITY.md)
for tested progress and remaining game-specific issues.

## Install or update

Download **PSPC-0.1.2-x64.msi** below and open it on Windows 10 or 11 x64. It installs for your
Windows account without administrator rights. Opening it over an existing installation
updates PSPC while preserving settings, saves, firmware and server accounts.

The installer is unsigned. Games and Sony firmware are not included. Firmware 4.01 is optional
for the XMB and provides system fonts needed by some games; see the
[installation instructions](https://github.com/Gattic/PSPC/blob/v0.1.2/README.md#installing).

**SHA256SUMS.txt** contains the installer checksum.

[Full changelog](https://github.com/Gattic/PSPC/compare/v0.1.1...v0.1.2)
