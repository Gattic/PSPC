# PSPC 0.1.1

PSPC 0.1.1 brings native game compilation to the Windows release installer, along with
compatibility fixes, large-disc support and launcher improvements.

## Native code in the installed app

Choose **Build native code** in the launcher, or let **Play** build it first. The installer now
includes the recompiler, portable LLVM/MinGW compiler, matching AOT SDK and private Python
runtime. No separate Visual Studio, LLVM, Windows SDK or Python installation is needed.
Compiled code is cached between runs, and the launcher offers to rebuild outdated code after
an update. The interpreter remains available.

Native builds also work from installation and game paths containing non-ASCII characters.

## Other changes since 0.1.0

- Fixed a crash when freshly compiled Daxter code switches task stacks on entering gameplay.
- Added a GameSpy server for Star Wars Battlefront: Elite Squadron, with accounts, friends,
  hosted matches and rankings. The launcher's **Online** page can run it locally or connect to
  another server, including OpenSpy. Host and join testing used two instances on one PC.
- Improved kernel timing, graphics, audio, I/O and library compatibility. Recorded
  pspautotests results are now **539 of 559 passing**, with the remaining 20 tracked as known
  differences and matching results in interpreter and native modes.
- Added support and regression coverage for disc images beyond the original 1.8 GB UMD size,
  including reads and seeks beyond 2 GB and 4 GB. ISO 9660 files split into multiple extents
  still expose only their first extent.
- Games requesting PSP-2000 extended user memory through `PARAM.SFO` now receive it.
- Fixed scrolling artifacts on the **Online** page and missing rows in server dialogs.
- Added a new PSPC logo and app icon, plus repeatable gameplay checkpoints for seven games.

See [game compatibility](https://github.com/Gattic/PSPC/blob/v0.1.1/docs/COMPATIBILITY.md)
for tested progress and remaining game-specific issues.

## Install or update

Download **PSPC-0.1.1-x64.msi** below and open it on Windows 10 or 11 x64. It installs for your
Windows account without administrator rights. Opening it over an existing installation
updates PSPC while preserving settings, saves, firmware and server accounts.

The installer is unsigned. Games and Sony firmware are not included. Firmware 4.01 is optional
for the XMB and provides system fonts needed by some games; see the
[installation instructions](https://github.com/Gattic/PSPC/blob/v0.1.1/README.md#installing).

**SHA256SUMS.txt** contains the installer checksum.

[Full changelog](https://github.com/Gattic/PSPC/compare/v0.1.0...v0.1.1)
