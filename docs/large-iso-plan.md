# Disc images larger than 1.8 GB: plan

## Status (2026-10-12)

Done: Phases 1 to 4, and Phase 5 as recommended. 5.1: the first extent only, with a warning, and
the further records of the same name are hidden. 5.2: left as is, and documented. 5.3: a note in
the details pane. The user-facing summary is in `docs/COMPATIBILITY.md` (Large disc images).

Where the work differs from the plan, or adds to it:

- **1.1:** sector-mode handles are refused (`sceKernelLoadModuleByID` returns BADF). The logic
  moved into `vfs::ReadRest` (pspc_fs), so the unit tests cover it.
- **1.3:** an index left zeroed is refused too ("CSO index is empty or corrupt"). That is what the
  old mkcso left when it failed past 4 GB.
- **1.4:** the PMF stream size is capped at `INT_MAX` (negative values are unchanged); the PGD
  size ioctl is clamped.
- **1.5:** kept; it passed the timing gate (pspautotests unchanged in the interpreter and as AOT,
  and the smoke test).
- **2.1/2.2:** `tests/unit/test_fs_large.cpp`: `fs_large_iso_files`, `fs_large_iso_multi_extent`
  and `fs_large_cso_index`, on an 8 GB + 4 MB image held in memory as patches over zeros.
- **2.3:** `tools/test_mkcso.py`. mkcso's DAX writer also stopped keeping every frame in memory;
  it spools them to a temporary file.
- **2.4:**
  - The guest program reads its commands from `disc0:/PROBES.TXT`, which the runner writes into
    the image. One list therefore drives both the program and the expected lines, and the
    expected lines are computed by reading the image file.
  - `tools/large_image_test.py` matches 47 of 47 lines, in the interpreter and as native code.
    (Correction, 2026-10-12: its first `--aot` runs still used the interpreter, as pspc only runs
    native code that already exists. The runner now builds the program's native code fresh
    first, and the result holds.)
  - The launcher lists the 8.75 GB image ("ISO · 8.8 GB") and shows the 5.3 note.
- **Sparse files on Windows:** Python's `truncate()` extends a file by writing zeros (the CRT's
  `_chsize`), and that filled the disk once. `make_big_iso.py` extends by writing one byte at
  the end instead, then checks how much disk the file really takes and removes it if it isn't
  sparse.
- **3.3:** `pspc_fs` already compiled cleanly with C4244 and C4267 as errors. A test file
  confirmed that the target's `/we` overrides the global `/wd`.

## Summary

- **1.8 GB is a property of the disc, not the image.** A dual-layer UMD holds about 1.8 GB (a
  single-layer one 900 MB). None of the image formats, and nothing in the PSP's file API, depends on
  that number.
- **PSPC already reads large images.** Checked on 2026-10-11 with a sparse 8.5 GB ISO (64 KB on
  disk). It held files starting at 2.25 GB and 4.25 GB, and a 4 GB − 64 KB file starting at 4.5 GB.
  Every marker read back correctly through `IsoFileSystem` (what disc0: uses):
  - at the start and end of each file;
  - 2 GB + 1 MB into the big file, reached with SEEK_SET, SEEK_CUR and SEEK_END;
  - through `umd0:` sector reads at sector 0x43FFDF (8.5 GB into the image);
  - through `/sce_lbn`.
- **The work is fixes, tests and documentation, not a redesign.** One tool breaks above 2 GB
  (`tools/mkcso.py`). There is one memory-safety bug on UMD sector handles. A few paths read whole
  files that they don't need to. And nothing tests sizes above a few MB.
- **What's left is set by formats and by games:**
  - an ISO 9660 file can't reach 4 GB;
  - a DAX image can't reach 4 GB;
  - the PSP's own sector numbers stop at 8 TB;
  - game code written for 1.8 GB may do byte arithmetic in 32 bits.
- **Recommendation:**
  - PSPC enforces no size limit.
  - Keep images that should also work in PPSSPP or on a PSP at 4 GB − 1 byte or less.
  - Mods should keep data the game addresses by byte offset below 2 GB.
  - The reasons are under [Recommended limits](#recommended-limits).

## How PPSSPP handles big images, and why about 4 GB

Read from `reference/ppsspp` (commit 0ac523c):

- **Host I/O is 64-bit.** `FileLoader::FileSize()` returns `s64` and `ReadAt` takes an `s64`
  position (`Core/Loaders.h:87`, `:92`). On Windows the high half of the position goes into
  `OVERLAPPED.OffsetHigh` (`Core/FileLoaders/LocalFileLoader.cpp:227-228`).
- **Block devices count sectors.** `ReadBlocks(u32 minBlock, ...)`
  (`Core/FileSystems/BlockDevices.h:39-40`) reaches 8 TB.
  - The CSO reader checks that the frame and block counts fit in 32 bits
    (`BlockDevices.cpp:570-577`).
  - Index entries are 31-bit positions shifted by the header's alignment, so a CSO above 2 GB needs
    an alignment of at least 1.
- **The ISO 9660 layer is the real limit.**
  - Each file's start is a 32-bit *byte* offset, `u32 startingPosition`
    (`Core/FileSystems/ISOFileSystem.h:72`), computed as `dir.firstDataSector * 2048` in 32-bit
    arithmetic (`ISOFileSystem.cpp:289`).
  - A read goes to `positionOnIso = e.file->startingPosition + e.seekPos` (`:598`). That is a `u32`
    plus an `unsigned int` (`ISOFileSystem.h:91`, "TODO: Make 64-bit?"), so the sum wraps before it
    is widened to `u64`.
  - Any byte of a file that lies at or past 4 GB in the image is read from 4 GB lower down, and the
    game gets the wrong data. Raw `/sce_lbn` reads use `sectorStart * 2048ULL` and are not affected.
- **Seeks are 32-bit, which costs ISO files nothing.**
  - Every file system takes `SeekFile(u32 handle, s32 position, FileMove type)`
    (`Core/FileSystems/FileSystem.h:152`). `sceIoLseek` passes `(s32) offset` down
    (`Core/HLE/sceIo.cpp:1494`).
  - In an ISO file the position is stored in that `unsigned int`, so everything wraps modulo 4 GB.
    Seeks anywhere inside a file below 4 GB still land correctly.
  - Memory-stick and host files are different. There the `s32` is sign-extended into
    `SetFilePointerEx` (`DirectoryFileSystem.cpp:489-492`), so a seek offset beyond 2 GB fails.
- **Other 4 GB caps:**
  - Installing a zipped game stops at 4 GB of contents (`Core/Util/GameManager.h:97`,
    `maxTotalSize = 0x100000000`).
  - "Cache ISO in RAM" allocates the whole image (`Core/FileLoaders/RamCachingFileLoader.cpp:91-93`).

So, in PPSSPP, an image of any size opens. But every byte of every file must sit below 4 GB of the
image, and the ISO 9660 metadata at the front leaves the rest of the image beyond that unreadable.

Outside the emulator, a PSP runs ISOs from a FAT32 memory stick. A FAT32 file can't exceed 4 GB − 1
byte, and many SD cards and USB drives are FAT32 as well. So no image a PSP can run is bigger than
that, and 4 GB is the natural ceiling to recommend.

## Where PSPC stands

| Layer | Width | Limit |
|---|---|---|
| Host file (`FileSource`, `block_device.cpp:46-66`) | u64, `OVERLAPPED` high half, 1 GiB chunks | none |
| Plain ISO (`PlainIsoDevice`) | u32 sector count, u64 byte offsets | 8 TB |
| CSO v1/v2, ZSO (`CsoDevice`) | 31-bit index `<< shift` (≤ 20), u64 math; "CSO image too large" at `:274` | 8 TB; a compressed file over 2 GB needs shift ≥ 1 |
| DAX | `u32 total = LE32(h + 4)` (`:366`), u32 frame offsets | under 4 GB |
| ISO 9660 records (`iso_fs.cpp`) | u32 LBA, u32 size per record; multi-extent (flag 0x80) not joined (`:200`) | 4 GB − 1 per file |
| Open file (`OpenFile`) | u64 size and position; `SeekLocked` takes s64 | none |
| `UMD_IOCTL_GET_SIZE` / `SEEK` | u64 / 64-bit offset | none |
| `UMD_IOCTL_TELL`, `SECTOR_TELL` (`:498`) | `(u32)f->pos` | PSP ABI (u32) |
| `/sce_lbn` (`ParseSceLbnPath`, `:69`) | u32 start sector, u32 byte size | PSP ABI |
| devctl 0x01F20003 (`iofilemgr.cpp:628`) | u32 last sector | 8 TB |
| `sceIoLseek` (`hle_wrap.h:106-108`), `SceIoStat.st_size` | s64 in v0:v1, s64 | none |
| `sceIoLseek32` (`iofilemgr.cpp:426`) | int offset and result | PSP ABI: 2 GB |
| ms0:/host0: (`host_fs.cpp`) | u64 sizes and seeks | none (FAT32 would cap at 4 GB − 1) |
| M33 ISO mounting, XMB ISO overlay | `media::MountDisc` → `OpenBlockDevice`; 64-bit `st_size` | none |
| Launcher scan | `fs::file_size` (u64); no hashing of image contents | none |

There's no UMD seek-time model keyed on sector numbers, so a big image doesn't change timing.
0x01F100A6 is a fixed 15 ms. Compressed images keep a 4 MB frame cache. The CSO index is loaded
whole: 4 bytes per frame, so 8 MB for a 4 GB image with 2 KB frames.

## What breaks today

1. **`tools/mkcso.py` above 2 GB of output.**
   - The index shift defaults to 0 (`-a/--align`, `:159`), and `index.append((pos >> align) | flag)`
     (`:108`) lets a position past 2 GB set bit 31, which is the "stored, not compressed" flag.
     PSPC then refuses the file with "CSO index not monotonic" (`block_device.cpp:294`).
   - Past 4 GB, `struct.pack('<%dI' ...)` (`:117`) raises `struct.error`. That only happens after
     the whole compression pass, and leaves the output with a zeroed index.
   - maxcso picks a shift automatically. mkcso doesn't.
2. **`mkcso.py -f dax` at 4 GB or more** raises `struct.error` (`:146-147`). This is the DAX
   format's own limit, but the tool should say so before doing the work.
3. **`io::ReadRestOfFd` on a sector-mode handle** (`iofilemgr.cpp:918`, used by
   `sceKernelLoadModuleByID`, `modulemgr.cpp:156`). This isn't size-specific, but it gets worse
   with size.
   - `end - pos` is in sectors on a `umd0:` or sector-mode `/sce_lbn` handle, so the buffer is
     sized in sectors, but `Read` fills that many *sectors*: 2048 times the buffer. That is a heap
     overflow.
   - `io::ReadFile` (`io.cpp:529`) already refuses sector mode.
4. **Whole-file reads:**
   - `pspc-iso info` and `modules` (`iso_tool.cpp:79-80`, `:103-107`) read every file of 64 MB or
     less in full, just to test a 4-byte magic. `tools/smoke_games.py:41` (`info`) and
     `tools/pspc_build.py:129` (`modules`) inherit that cost.
   - `pspc-iso get` builds the whole file in memory.
   - Booting a PBP reads all of it, DATA.PSAR included, to take DATA.PSP (`runtime.cpp:173-179`,
     where `u32 end` would truncate past 4 GB). `pspc_build.py:95` does the same with
     `read_bytes()`.
5. **Unbounded allocation from a damaged record.** `IsoFileSystem::ReadWholeFile` does
   `out.resize((size_t)info.size)` (`iso_fs.cpp:269`). The record's size is clamped only to the
   image size, so on a large image a corrupt PARAM.SFO or ICON0 record can make the launcher or the
   XMB overlay allocate up to 4 GB.
6. **Single files over 2 GB** truncate in two places that a real UMD could never reach:
   - `media_psmf.cpp:865`: `streamSize = (int)(fileSize - ...)`;
   - `iofilemgr.cpp:557`: the PGD data-size ioctl returns `(int)info.size`.
7. **Timing:** `RwDelayUs(size)` (`iofilemgr.cpp:385`, `:396`, `:442`) treats a `umd0:` sector
   count as a byte count, so sector reads are charged 1/2048 of their cost.

## Limits that stay

- **4 GB − 1 per file.** The ISO 9660 directory record has a 32-bit size. Bigger files need
  multi-extent records (several records with the same name, all but the last flagged 0x80). PSPC
  shows only the first extent. See decision 5.1.
- **DAX under 4 GB.** Its header stores a 32-bit uncompressed size.
- **8 TB per image.** The PSP's own interface counts sectors in 32 bits (umd0: positions, devctl
  0x01F20003, `/sce_lbn`).
- **`sceIoLseek32`** takes and returns an `int`, which is the PSP's ABI. A game that uses it can't
  seek past 2 GB in a file on real hardware either.
- **Game code.** Every game was written for a disc of 1.8 GB or less. A game that reads its own
  archive with byte offsets in an `int` (for example `lba * 2048` in signed 32-bit math) breaks
  once a mod moves that archive past 2 GB in the image, or past 4 GB if the math is unsigned. That
  happens inside the game, and PSPC can't see it.

## Recommended limits

- **PSPC:** no enforced limit. ISO, CSO and ZSO images work up to the format limits above.
  The tests in Phase 2 will cover images up to 9 GB. Nothing in the launcher warns about size alone.
- **Images meant to run elsewhere too:** at most 4 GB − 1 byte. That is the most a FAT32 memory
  stick can hold, and the point past which PPSSPP reads the wrong data.
- **Mods:** keep every file under 4 GB. Keep anything the game addresses by byte offset (its
  archives, if it reads them through `umd0:` or `/sce_lbn`) below 2 GB in the image, and put new
  bulk data after it.

This is relevant here: `Games\SWSS.iso` is already 1.59 GB, and the SWSS map port adds content to
an image.

## Ground rules

- **Pass the usual gates:**
  - unit tests;
  - pspautotests with 539 passing and 20 known differences, identical in the interpreter and as AOT;
  - the 6-game smoke test.
- **Large-image tests cost no disk.** Unit tests use an in-memory sparse `ByteSource`. On-disk
  fixtures are sparse files: `FSCTL_SET_SPARSE` on NTFS, a hole past the end elsewhere. They are
  refused where sparse files aren't supported, and removed if they take real disk space.
- **Expected results come from the fixture, never from PSPC.** Each test writes known markers at
  known offsets and checks that it reads them back.

## Phase 1: Fixes (S)

### 1.1 `ReadRestOfFd` and sector mode
Refuse sector-mode handles, as `io::ReadFile` does. The alternative is to convert to bytes
(`(end - pos) * 2048`), which would let `sceKernelLoadModuleByID` load a module from a raw sector
handle.

Check how `sceKernelLoadModuleByID` behaves on a `umd0:` fd on hardware before choosing. Without
that evidence, refuse (`SCE_KERNEL_ERROR_BADF`, like any unreadable fd). Add a unit test with a
sector-mode handle.

### 1.2 `mkcso.py`
- **Pick the shift from the input size:** the smallest shift for which the worst case fits in 31
  bits. The worst case is input size + header + index + padding, since a frame is never stored
  larger than raw.
- **`-a` becomes a minimum.** If the user passes too small a value, say why and use the needed one.
- **Check before compressing:**
  - refuse DAX for inputs of 4 GB or more, before any work is done;
  - assert every index entry fits before packing, so a failure never leaves a half-written file.
- **Write the output to a temporary name** and rename it at the end.

### 1.3 Clearer CSO errors
When the index stops being monotonic exactly where bit 31 would have been reached, say so. For
example: "CSO index overflows 2 GB: recompress with an index shift (maxcso and tools/mkcso.py
choose one)."

### 1.4 Single-file truncations
- `media_psmf.cpp:865`: clamp the stream size to `INT_MAX` (a PMF over 2 GB isn't a real case).
- `iofilemgr.cpp:557`: return the size clamped, or an error, instead of wrapping negative.

### 1.5 Sector-mode read cost
Charge `RwDelayUs` in bytes (`count * 2048`) when the handle is in sector mode.

Gate this one separately: re-run the `umd/` and `io/` autotests and the timing suites (`threads/`,
`intr/`, `display/`), and smoke the games that read `umd0:` directly. If a recorded test disagrees,
the recording wins and this item is dropped.

## Phase 2: Tests (M)

### 2.1 Sparse in-memory source (unit tests)
A test `ByteSource` with a declared size (for example 9 GB) and a map of patches; everything else
reads as zeros. On top of it, an ISO builder that places files at chosen sectors, extending
`fstest::BuildIso` with explicit LBAs. Cases:

- files starting just below and above 2 GB, 4 GB and 8 GB, read at their first and last byte;
- a 4 GB − 1 file read at its end: seeks with SEEK_SET, SEEK_CUR and SEEK_END to 2 GB + 1 MB and to
  the last 64 bytes;
- reads that straddle the 2 GB and 4 GB image boundaries;
- `GetStat` sizes, `UMD_IOCTL_GET_SIZE`, and `SEEK` with an offset past 4 GB;
- `umd0:` sector reads past sector 0x200000 (4 GB) and 0x400000 (8 GB), and `/sce_lbn` paths there;
- a record whose size runs past the end of the image is clamped (the existing check, on a big
  image);
- the multi-extent behaviour, whichever decision 5.1 takes.

### 2.2 CSO and ZSO above 4 GB
- **Index positions past 2 GB and 4 GB.** A sparse source holding a CSO whose index entries, with
  shifts 1 and 2, point past 2 GB and past 4 GB. The frames there are stored (uncompressed) so no
  compressed data has to be produced.
- **An uncompressed image over 4 GB.** Index positions small, frame count over 2^21.
- **The error case.** A shift-0 index that crosses bit 31 gives the error from 1.3.

### 2.3 `mkcso.py`
- A unit test of the shift picker as a pure function, on sizes just below and above 2 GB, 4 GB and
  8 GB.
- A round trip of a small image with `-a 1` and `-a 2`: compress, then open in PSPC and compare
  every sector.
- DAX refusal on a sparse 4 GB input; it fails at once, without compressing.

### 2.4 End to end through the guest
- **`tools/make_big_iso.py`:** a sparse-ISO fixture builder, from the scratch script used for the
  check above. It takes a layout, and writes markers and a PARAM.SFO.
- **A guest test built with pspdev:** a small homebrew that, through the HLE:
  - opens `disc0:` files past 2 GB and 4 GB;
  - uses `sceIoLseek` (64-bit) to reach 2 GB + 1 MB and the end of a 4 GB − 1 file;
  - checks that `sceIoLseek32` behaves as the PSP ABI says (an `int` offset);
  - reads `umd0:` sectors past 4 GB, and the last sector from devctl 0x01F20003.

  It prints what it reads. The expected file comes from the fixture's layout. It runs in a runner
  mode next to the pspautotests, not inside their gate: no PSP recording exists for it.
- **The launcher, for the record:** a 9 GB sparse image in `Games\`. It should list with the right
  size and boot to the guest test's EBOOT.

## Phase 3: Tools and hardening (S–M)

### 3.1 Read only what's needed
- `pspc-iso info` and `modules`: read 4 bytes first, and the whole file only for executables.
- `pspc-iso get`: stream in 1 MB pieces to the output file.
- PBP boot (`runtime.cpp`): read DATA.PSP by its offsets instead of the whole PBP, and keep the
  offsets in `u64`.
- `pspc_build.py`: do the same, reading just the header and the DATA.PSP range.

### 3.2 Bounded whole-file reads
Give `ReadWholeFile` a maximum size. The launcher and the XMB overlay pass a small cap for
PARAM.SFO, ICON0, PIC1 and SND0, so a damaged record fails cleanly instead of allocating gigabytes.

### 3.3 Build guards
- Add `CMAKE_SIZEOF_VOID_P EQUAL 8` as a configure-time check. The code assumes a 64-bit `size_t`
  wherever a u64 size becomes a buffer length, and only x64 is built.
- Re-enable C4244 and C4267 for `src/fs` alone, which is suppressed globally at `CMakeLists.txt:20`,
  and fix what it reports. New narrowing in the storage layer then fails to compile instead of
  passing silently.

## Phase 4: Documentation (S)

A "Large images" section in `docs/COMPATIBILITY.md`:

- the limits table, condensed;
- the recommended limits above;
- a note that a DAX image can't reach 4 GB, and that a CSO bigger than 2 GB needs an index shift
  (maxcso and `mkcso.py` choose one).

## Phase 5: Decisions

### 5.1 Files of 4 GB and over (multi-extent)
- **Joining multi-extent records (M):**
  - `Node` gets a list of extents;
  - reads walk the list;
  - `/sce_lbn` and `GET_START_LBA` keep reporting the first extent;
  - `st_size` reports the sum.
- **Why it can wait:** no PSP game has such a file (a whole UMD is 1.8 GB), so only a mod could
  produce one, and `mkisofs`/`xorriso` only write multi-extent files at ISO level 3. My
  recommendation is to skip it until a real image needs it, and meanwhile log a warning when a
  0x80 record is seen, so the cause is obvious.

### 5.2 FAT32 behaviour on ms0:
The memory stick under `ms0:/ISO` accepts files larger than FAT32 allows, unlike a real PSP. Making
it refuse them would only take something away. My recommendation is to leave it, and mention it in
the documentation.

### 5.3 A launcher hint above 4 GB
- **What it would be:** a line in the game's details: "Larger than 4 GB: this image won't run on a
  PSP or in PPSSPP."
- **The cost:** nothing; it's only information.
- **My recommendation:** yes, in the details pane only, not as a warning.

## Order and effort

| Phase | Effort | Depends on |
|---|---|---|
| 1.1-1.4 | S (half a day) | — |
| 1.5 | S, plus the timing gate | — |
| 2.1-2.3 | M (1-2 days) | 1.2, 1.3 |
| 2.4 | M (a day; pspdev under WSL Ubuntu-22.04, as for vregs) | 2.1 |
| 3.1-3.3 | S–M | — |
| 4 | S | 1-3 |
| 5.x | after your decision | — |
