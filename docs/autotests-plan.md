# pspautotests: plan for the 42 failing tests

PSPC passes 517 of the 559 pspautotests, with the same results in the interpreter and as AOT code.
None of the 42 failures is a crash, and none comes from the recompiler. Each test runs to the end
and differs from the output recorded on a real PSP in some detail.

This plan comes from an investigation of every failing test (2026-10-11). Each test's source and
expected output were read, the test was run for its full output, and the cause was traced into
PSPC's code. Several of the proposed models were checked offline against all of the expected
lines. The causes fall into three kinds:

- **The test environment** differs from the one the tests were recorded in.
- **PSPC's behaviour** differs: a timing cost, an allocator rule, or a library detail.
- **The recording itself** can't be reproduced: hardware jitter, the PSP's uptime, or modules that
  aren't shipped with the suite.

The phases below fix the first two kinds. The third kind is listed in its own section, together
with what it would take to change it.

**Expected result:** about 537 tests pass after phases 1 to 5, and about 540 after phase 6, which
depends on your decisions (see the last section). A few more are possible: a test that is only
partly fixed in a phase may still pass. The remaining 18 or so need either a real PSP to record
better data, or reverse engineering of modules that the suite doesn't include.

## Ground rules

- **Match the hardware recording.** PSPC's own output is never the reference. No emulator code
  checks which test is running.
- **Reproduce the recording environment in the test runner.** Differences between the recording
  setup and PSPC go into `tools/run_autotests.py` as per-test options and fixtures. Each one is
  documented together with its evidence. Examples: a PSP with a screen, extended memory, file
  dates, a disc image.
- **Keep test-only hardware models out of games.** The data cache and the VRAM mirror swizzles are
  opt-in flags that force the interpreter. Games never turn them on.
- **Pass the usual gates in every phase:** unit tests, then autotests with identical interpreter
  and AOT results and no regressions, then the 6-game smoke test. Timing phases also re-run every
  test in `threads/`, `display/`, `intr/` and `audio/`, and check frame pacing in games.
- **Keep a known-differences list.** A test that can't be reproduced goes on a list in the runner,
  with its reason. The runner reports it separately, so the gate becomes "N pass, K known
  differences, 0 unexpected". The list may only get shorter.

## Phase 1: Recording environment in the runner (S–M)

### 1.1 Per-test options
Add a table in `run_autotests.py` mapping a test to its extra pspc arguments and fixtures. Each
entry has a comment explaining why. The phases below fill it.

### 1.2 The hardware output path
The expected files were recorded on a PSP with a screen. There, `common/common.c` reroutes
stdout and stderr to `host0:/__testoutput.txt` and `__testerror.txt` with `freopen`. That has
three effects:

- it takes fds 3 and 4;
- it moves the newlib heap by 0x20;
- it starts an update thread.

PSPC answers the emulator devctl with "no display", so `common.c` takes its `fmemopen` path
instead. PSPC already has `--test-display`, which takes the hardware path.

- **Effect of `--test-display`:** with it, these pass:
  - `gpu/simple/simple`: its expected pixels are the top-left of the debug font's "R", so this has
    nothing to do with the data cache.
  - `io/open/tty0`
  - `font/fonttest` (it also needs the firmware font, see 1.4)

  The heap-address lines of `video/pmf`, `video/pmf_simple` and `jpeg/mjpegcsc` also match.
- **Problem with turning it on for every test:** it breaks `threads/events/events` and
  `threads/threads/change`. A guest write to the host0 output file blocks for at least 100 µs, so
  another thread gets to print in the middle of a line.
- **Plan:**
  1. In test-display mode, make the two output files a sink: writes return at once. The file goes
     to a per-process path, so parallel suite runs no longer overwrite
     `reference/pspautotests/__testoutput.txt`.
  2. Run the whole suite on the hardware path.
  3. Tune the sink's write cost together with 3.5, the console output cost.
  4. If no single setting satisfies both the threads tests and the timing tests, keep
     `--test-display` as a per-test option instead.

### 1.3 Fixtures
- **`io/file/file`:** it stats `file.c` and expects `2013-01-21 00:11:54`, the file's date on the
  author's disk. Git doesn't keep file dates, so the runner sets it with `SetFileTime`. The time is
  converted with `TzSpecificLocalTimeToSystemTime` so that the result doesn't depend on the time
  zone. The test also needs 2.1.
- **`umd/io/umd_io` and `umd/raw_access`:** the runner generates a minimal ISO9660 disc in
  `build/autotest-discs/`.
  - Layout: PVD at 16, path tables at 18–21, directories at 22–25, then PARAM.SFO at LBN 26.
  - PARAM.SFO is the 472-byte SFO embedded in the test source (UCJS10041).
  - PSP_GAME also holds 1×1 ICON0.PNG and PIC1.PNG and empty SYSDIR and USRDIR folders.

  The generated disc is used instead of the stale `raw_access.iso` shipped with the test. With it,
  umd_io passes; this was verified. Most of raw_access passes too; for the rest, see the
  known-differences section.

### 1.4 Firmware fonts
`font/fonttest` opens jpn0.pgf. Run the suite with a flash0 that contains the firmware fonts, which
`tools/pspc_install_fw.py` installs. With the real font, PSPC's allocation-callback sequence
already matches. If the fonts are missing, the runner marks fonttest "skipped: no firmware fonts".

### 1.5 Documentation
In `docs/COMPATIBILITY.md`, correct the note that `gpu/simple/simple` needs a data-cache model.

**Expected result:** gpu/simple/simple, io/open/tty0, font/fonttest and umd/io/umd_io pass: **521**.

## Phase 2: Small, deterministic fixes (S each)

### 2.1 host0 file mode
`HostFileSystem::FillStat` (`src/fs/host_fs.cpp`) reports mode 0664 for every file. A file the
guest created through psplink (usbhostfs) came out 0644 on the recording host. Remember the files
created with `O_CREAT` in this session, follow them through renames and drop them on removal, and
report 0644 for them.

This models the psplink host's umask. Together with 1.3 it makes `io/file/file` pass.

### 2.2 Growable kernel heaps
Change `sceKernelCreateHeap` / `sceKernelAllocHeapMemory` (`hle/modules/sysmem.cpp`) to follow the
firmware, as described in uOFW's heap.c:

- **Flags:** bit 0 lets the heap grow. Bit 1 creates it High; without it, Low.
- **Blocks:** a heap is a ring of blocks. Each block costs 24 bytes plus an 8-byte end sentinel;
  each allocation has an 8-byte header and is rounded to 8.
- **Growing:** when no block has room and the heap may grow, a new block comes from Low:

  ```
  newSize = heapSize
  align == 0:   if (newSize - 40 < size) newSize = UPALIGN8(size) + 40
  align != 0:   size = UPALIGN8(size)
                shift = align - 32 % align  (0 if align divides 32)
                newSize = max(newSize, size + shift + 40)
  ```
- **Free and delete:** freeing searches all blocks; deleting the heap frees all of them.

This makes `sysmem/kernel/heapgrow` pass. Re-check `sysmem/kernel/heap`.

### 2.3 Thread stack size that rounds to 0
In `kernel/threads.cpp` `CreateThread`, `AlignUp(0xFFFFFFFF, 0x100)` wraps to 0, and the allocator
then gives out a 0x100 block. Fail with 0x80020190, as the firmware does. This is part of
`threads/threads/create`; the rest is in 6.3.

### 2.4 sceGeDrawSync(1)
`ge_core.cpp` currently compares the list's pc with its stall address. Real geman compares the
first active list's stall address with the hardware LISTADDR register (uOFW `ge.c`).

Track an emulated LISTADDR:
- the running list's pc, or its stall address when it is stalled;
- 0 after `sceGeBreak(0)`, which runs geman's own internal list;
- restored on Continue and when a list starts.

`DrawSync(1)` then returns STALL when the first list that isn't done has its stall address equal to
LISTADDR, and DRAWING otherwise. All 14 DrawSync(1) lines of `gpu/displaylist/state` were checked
against this by hand.

### 2.5 Message dialog start-up with shared priority
When the dialog's access and graphics threads share one priority, hardware takes 625–675 ms to
start (`utility/msgdialog/workshare`). PSPC takes 540 ms. Use about 640 ms in that case
(`msg_dialog.cpp`). This rests on two hardware data points, so the confidence is moderate.

### 2.6 host0 seek blocks
On hardware every host0 operation is a USB round trip and blocks. `sceIoLseek` only calls
`Reschedule()`, which never yields to a thread of the same priority. For host0, use a
`Delay(0, ~100 µs)` instead. This makes `font/openfile` pass: its seek callback is a host0 seek.
Leave ms0 and UMD as they are; this test can't show how they behave.

### 2.7 sceIoOpenAsync re-enables dispatch
Creating the async thread re-enables thread dispatch, as `sceKernelCreateThread` does. In
`sceIoOpenAsync`, and on an fd's first async operation, call
`if (*g_intrEnabled) sched::SetDispatchEnabled(true)`. This is part of
`threads/scheduling/dispatch`; the rest is in 3.2.

### 2.8 Screen-clear cost
`ClearFillCycles` uses 7.82 ns per pixel. That figure came from a probe that included the list's
start and sync overhead. Use about 7.15 ns per pixel plus about 85 µs per list, so that one clear
still totals about 1.02 ms, and scale the 5.28 and 3.77 figures the same way. This is the
`sceGeDrawSync` line of `threads/scheduling/syscallkinds`.

### 2.9 Audio: NULL output while a buffer is playing
A NULL output (`SRCEnqueue`) while the previous buffer is still playing must block until it
finishes. Today it returns at once. This is part of `audio/sceaudio/output`.

### 2.10 JPEG colour conversion with unusual chroma layouts
`JpegCsc` takes its 2×2 fast path whenever the width and height are even. That is wrong for the
chroma layouts 0x00020201 and 0x00020101. Take the fast path only for 0x202; otherwise look up
chroma per pixel at `(w>>ws)*(y>>hs) + (x>>ws)`. This fixes four lines of `jpeg/csc`.

**Expected result:** io/file/file, sysmem/kernel/heapgrow, gpu/displaylist/state,
utility/msgdialog/workshare and font/openfile pass: **526**.

## Phase 3: Timing model (M)

Under `--fast-time`, which the runner uses, PSPC already has a deterministic virtual clock
(`kernel/scheduler.cpp`). These tests fail on the costs charged to that clock.

### 3.1 Split costs and an I/O cost table
- **`SpendUs(busyUs, waitUs)`:** under fast-time, `BusyDelayCycles` for the busy part, then
  `WaitUntil` for the rest. In real time, all of it is waited. `modulemgr` already does this ad hoc
  for LoadModule, which matches hardware.
- **Per-device cost table.** The ms0 (FAT) costs come from `callcosts`:

  | Operation | Busy | Wait |
  |---|---|---|
  | open | ~6.5 ms | ~2.5 ms |
  | read | ~0.8 ms + ~19 ns/B | ~0.2 ms + ~70 ns/B |
  | close | 100 µs | — |

  Hardware read times are 1.5 ms for 5162 bytes and 20 ms for 213454 bytes. host0 keeps its
  waiting behaviour: in `dispatch`, a host0 call must still let another thread run.
- **Atrac:**

  | Call | Busy | Wait |
  |---|---|---|
  | SetData, mono | ~250 µs | ~650 µs |
  | SetData, AT3+ | ~0.9 ms | ~2.6 ms |
  | first decode | ~200 µs | ~500 µs, plus ~500 µs for AT3+ |
  | GetAtracID | 10–30 µs | — |

  GetAtracID currently charges nothing, which makes the test divide by zero and print "?".

This makes `threads/scheduling/callcosts` pass. With 2.8 it also makes
`threads/scheduling/syscallkinds` pass: SetDataAndGetID for AT3+ comes to about 3.7 ms, where
hardware shows "~4 ms".

### 3.2 Async I/O on helper threads
On hardware, each fd's async operations run on a "SceIoAsync" thread at `asyncPrio`, which defaults
to the caller's priority. `sceIoWaitAsync` blocks until that thread has run.

PSPC finishes the operation at the next scheduling point and puts the caller first in line. Model
it with a native helper thread queued behind threads of the same priority, like `utility.cpp`'s
ScePafJob. With 2.7, `threads/scheduling/dispatch` passes.

### 3.3 Interrupt and switch costs
A timer-woken thread costs 23 µs (`kDelayWakeCycles`). Move about 10 µs of that into a cost that is
charged only when such a thread is dispatched over another thread. Then the main thread gets
about 190 µs of work between wakes, and `sceKernelCreateThread` with a 256 KB stack takes about
2.5 ms instead of 2.7 ms.

This makes `threads/scheduling/preemptsyscall` pass. Re-check these tests, which pass today:
handoff, costs, waittimeouts, semaphores and alarmcosts.

### 3.4 Vblank interrupt
Model the interrupt from the hardware notes in `display/vblankphase.cpp`:

- **Interrupt and hcount:** the interrupt comes at the end of line 285. hcount is
  `floor((now - T_interrupt) / 58.33 µs)` and counts 0 to 285; PSPC is currently one higher. The
  vblank flag stays on for 816 µs after the interrupt.
- **CPU taken:** the interrupt itself costs 64 µs, 28 before the user handlers and 36 after. Each
  handler adds about 12 µs.
- **Waking waiters:** the first thread run after an interrupt that woke waiters pays about 11 µs of
  dispatch latency, replacing the 5.2 µs switch; each further waiter adds about 1.5 µs. Returning
  from `sceDisplayWaitVblankStart` costs about 6 µs, so later waiters follow each other about 12 µs
  apart.
- **Jitter:** a deterministic pseudo-random 0–4 µs added to the dispatch latency, from a seeded
  source in `sched`, so that the min–max ranges in the tests appear.

Predicted against hardware:

| Value | Predicted | Hardware |
|---|---|---|
| vblank length | 730–740 µs | 730–740 µs |
| first waiter | 81–88 µs | 75–90 µs |
| first waiter, with handler | 93–99 µs | 85–100 µs |
| spacing between waiters | ~12 µs | 9–15 µs |
| interrupt CPU | ~67 µs | 58–69 µs |

This makes `display/vblanklen` and `display/vblankphase` pass. Re-check display/hcount, isstate,
vblankmulti, intr/vblank, ctrl/vblank and threads/scheduling/vblankwake.

**Risk for games:** in fast-time, every thread paced by vblank wakes about 30 µs later and loses
64 µs of CPU per frame (0.4%). Watch pacing in the smoke games.

### 3.5 Console output cost
PSPC treats writes to stdout and stderr as free. Hardware takes more than 1.3 ms for a flush to
host0. Charge that cost, tuned together with 1.2's sink and 3.4.

- **Fixes:** part (a) of `audio/sceaudio/output`. It also lets `utility/msgdialog/dialog` pass,
  once its start-up is 549 ms: that test is sensitive to where in a frame each dialog starts, and
  3.4 alone would push every dialog into the wrong frame.
- **Re-run:** this changes every test that prints, so the whole suite is re-run.

**Expected result:** threads/scheduling callcosts, syscallkinds, dispatch and preemptsyscall,
display/vblanklen and vblankphase, and utility/msgdialog/dialog pass: **533**.

## Phase 4: GPU and memory hardware models (M)

### 4.1 Texture cache (always on)
- **Hardware:** the GE loads a texture into its 8 KB cache when a primitive first samples it, and
  keeps serving it to later primitives until TEXFLUSH.
- **PSPC today:** TEXFLUSH and TEXSYNC are no-ops, and the software rasterizer samples VRAM live. A
  primitive that textures from its own render target therefore sees what earlier sprites drew.
- **Fix:**
  - Count TEXFLUSH generations.
  - A primitive whose texture overlaps the batch's color or depth target draws from a snapshot of
    the texture.
  - The snapshot is reused while the texture fits in 8 KB, its address and size are unchanged, and
    no TEXFLUSH has come; otherwise it is copied again.

  This is the approach of PPSSPP's `SelfTextureSnapshot`.
- **Result:** a simulation with PSPC's blend formula gives `0e4c1945` / `d5ce0f73`, the expected
  values, so `gpu/exact/pixel` will pass.
- **Cost and scope:** one texture copy per self-texturing primitive. The D3D11 renderer is not
  covered yet.

### 4.2 VRAM mirrors (opt-in, `--edram-mirrors`, interpreter only)
Let L be the VRAM offset and T the value set with `sceGeEdramSetAddrTranslation`. With X = 0x600
when T = 0, otherwise X = (T << 3) | 0x40:

| Mirror | Transform |
|---|---|
| 0x04200000 | L ^ X |
| 0x04400000 | identity |
| 0x04600000 | rotate bits 5 .. log2(T)-1 left by one, then ^ X; at T = 0, swap bits 9 and 10, then ^ 0x600 |

This matches PPSSPP's DepthSwizzle and all 375 translation lines of `gpu/ge/edramswizzle`.

Doing this for games would also need the GE to store depth in the same swizzled layout. About 16
gpu/exact tests read depth through the linear mirror and rely on today's matching linear layouts.
Doing it for games is large and slows every memory access, for almost no benefit. So it is a
flag: interpreted memory accesses to mirrors 1 and 3 go through the transform.

### 4.3 Data cache (opt-in, `--dcache-model`, interpreter only)
- **The cache:** 16 KB, 2-way, 64-byte lines in 128 sets, write-back and write-allocate.
  - Uncached segments: 0x4–0x7xxxxxxx and 0xA–0xBxxxxxxx.
  - The scratchpad is never cached.
- **Model:** a new `src/runtime/core/dcache.cpp`.
  - Each slot holds a tag, valid and dirty bits, LRU state and a 64-byte copy of the line.
  - Host memory stays the cached view.
  - Uncached accesses read the line's copy when the line is resident.
  - The HLE `sceKernelDcache*` functions use the same model.
- **Cache instructions:**

  | Op | Meaning |
  |---|---|
  | 0x14 | index writeback-invalidate |
  | 0x18 | create dirty exclusive |
  | 0x19 | hit invalidate (dirty data is lost) |
  | 0x1a | hit writeback |
  | 0x1b | hit writeback-invalidate |
  | 0x1e | fill |
- **Wiring:** the interpreter's load, store and CACHE handlers are swapped for model-aware ones when
  the flag is on.
- **Result:** traced by hand, all 11 lines of `cpu/lsu/cacheop` match.
- **Why not on for games:** games depend on the cache being invisible, and AOT code would need a
  tag lookup on every access.

**Expected result:** gpu/exact/pixel, gpu/ge/edramswizzle and cpu/lsu/cacheop pass: **536**.

## Phase 5: Library behaviour (M–L)

### 5.1 AAC frame lengths (L)
For AAC, the codec context reports how many input bytes each decode used. On hardware these are
0x13b, 0x131 and 0x11b, one raw access unit each. PSPC reports 0x600, because Media Foundation can't
say how much it consumed.

Fix: write a bitstream parser for AAC-LC `raw_data_block`, without decoding. It needs:
- the elements SCE, CPE, LFE, DSE, PCE, FIL and END;
- ics_info, section data, scalefactors, pulse and TNS;
- spectral codebooks 1–11.

It measures the bits used, the decode consumes exactly those bytes, and if parsing fails it falls
back to today's behaviour. This needs the Huffman and scalefactor-band tables from ISO 14496-3,
which are not in the repo; see the decisions below. It makes `audio/audiocodec/context` pass.

### 5.2 Custom I/O drivers (M)
Implement `sceIoAddDrv` and `sceIoDelDrv`:

- **Registration:** read the driver's PspIoDrv `{name, type, …, funcs}` and call IoInit with a
  `PspIoDrvArg`, through `cpu::CallGuest`. Register the driver whatever IoInit returns; the
  expected output requires that. Mount it as an `io::Device`.
- **Calls:** route open, close, read, write, ioctl, dopen and the rest to the function table.
- **Lseek** returns 64 bits in v0:v1, which needs a `CallGuest` variant.
- **Removal:** DelDrv calls IoExit and unmounts.

This fixes everything in `io/iodrv/iodrv` except line 2. That line comes from an emulator: the
test passes flags 0x202, and only that emulator turned them into 7.

### 5.3 MJPEG colour conversion (M)
`jpeg/mjpegcsc` still fails afterwards; see the known-differences section.

- **Colour formula:** clamp the chroma to at least 1, so d and e are in [-127, 127], then:

  ```
  R = Y + ((359e + 128) >> 8)
  G = Y + ((-88d - 182e + 128) >> 8)
  B = Y + ((454d + 128) >> 8)
  ```

  This matches all 10 "Bits" lines and the 0x00008600 written with a NULL input.
- **Misaligned planes:** when a plane's base is 4 mod 8, byte i is read from base+i if (i & 4) is 0,
  and from base+i−8 otherwise. This matches the four pattern lines, given 1.2.
- **Width under 16:** treated as 1024 pixels.

### 5.4 sceMpeg (M, with an L part; risk to games)
Several `media_mpeg.cpp` fixes:

- **InitAu:** writes the buffer address and an esSize of 0; the stream number is kept elsewhere.
- **GetAtracAu with no data:** returns 0 pts/dts and 0 size, and doesn't yield.
- **AtracDecode with an empty AU:** yields and returns 807f00fd.
- **GetAvcAu:** takes a real AU:
  - size from AUD to AUD;
  - pts/dts only when the AU starts a PES with a PTS;
  - attr from nal_ref_idc.

  This is the L part.
- **AvcDecode:** zeroes esSize and keeps the pts.
- **RingbufferAvailableSize:** counts packs as AUs are consumed.
- **Flush:** yields.
- **RegistStream:** returns `AlignUp(data, 64) + 0x12C0`.

These fix most of `video/mpeg/basic`, `video/pmf` and `video/pmf_simple`. They are unlikely to make
them pass exactly; see the known-differences section. Daxter's intro movie plays through sceMpeg,
so every change is checked against it, as are SFLS's cutscenes.

### 5.5 PSMF player (S, partial)
- **GetVideoData:** fails without writing when no new frame is due. That gives about 50 writes in
  `video/psmfplayer/playmode`, where hardware does 51.
- **Loop restart:** `video/psmfplayer/configplayer` stalls about 100 calls when the video loops,
  probably re-buffering from host0 over USB. A calibrated delay is possible but low-confidence, and
  only safe for host0.

**Expected result:** audio/audiocodec/context passes: **537**, plus configplayer if its
calibration holds. Most of the rest of this phase reduces differences rather than making tests
pass.

## Phase 6: Needs your decision (S–L)

### 6.1 cpu/vfpu/vregs
- **Problem:** the shipped vregs.prx is older than its .expected: its value table has 1e+07 where
  the source now has 100.0, and three sections are missing.
- **PSPC is correct for it:** with that one value patched in a scratch copy, PSPC's output matches
  the expected file byte for byte.
- **Fix:** rebuild the test with the pspdev toolchain, which isn't installed (it needs a download).
  The runner then uses the rebuilt .prx.

### 6.2 io/io/io and io/directory
These two tests disagree about how host0 names look:

- **io/directory** expects `1.txt` shown as `1.TXT`, as on a memory stick.
- **io/io** expects `io.c` and `io.prx` unchanged, as psplink shows them. It also lists `io.elf`
  and `io.o`, build products of the author's folder that no checkout has.

With a per-test host0 naming option (FAT-style names for io/directory, psplink names for io/io) and
stub `io.elf`/`io.o` files in a scratch copy of the test folder, both pass. The stubs reproduce the
recording folder; whether that is acceptable is your call.

### 6.3 Extended memory
`threads/threads/create` was recorded on a PSP whose user memory ended at 0x0BB00000, which puts
thread stacks above 0x0A000000; the expected output shows them as "UNKNOWN". The pmf tests'
T_Reader address confirms that end address.

The runner gives these tests `--large-mem` and a new `--user-mem-end 0x0BB00000`. Together with
2.3 that fixes every line except one priority line. That line needs `sceKernelCreateThread`'s cost
calibrated, which comes partly out of 3.3.

### 6.4 umd/raw_access under an M33 ISO driver
- **Problem:** the remaining lines need two things Sony's isofs refuses:
  - a 33-character `sce_lbn` path; the 4.01 isofs rejects 32 characters or more;
  - byte offsets on `umd0:`.

  The reference was probably recorded under a custom-firmware ISO driver.
- **Option:** PSPC is an M33 reimplementation, so modelling the M33 ISO driver (march33) is
  legitimate. It needs that driver's binary.

**Expected result:** if you approve these, vregs, io/io and threads/create pass: **540**.

## What stays different, and why

| Test | Why it can't match today | What would change that |
|---|---|---|
| threads/scheduling/delaylen | Hardware jitter. The lines contradict each other: 210 µs needs at least 25 µs of overhead, 500 µs needs at most 24 µs. | Nothing deterministic. |
| threads/scheduling/preemptuser | Hardware jitter. Shortest and longest of 8 runs differ by 10–20 µs, and the delay must be both ≤1024 here and ≥1025 in delaylen. | Nothing deterministic. |
| threads/scheduling/scheduling | One cost in a chain of five is off. Which one can't be determined. | Timed hardware data (the test built with `CHECKPOINT_ENABLE_TIME=1`). |
| display/hcountwrap | The expected output is the phase sample of one hardware run. | Nothing. Costs can be made realistic, but matching is luck. |
| threads/threads/profiler | It prints the PSP's uptime (183566489 µs) and cache warm-up. | Nothing. |
| audio/sceaudio/output | After 2.9 and 3.5, block 4 still differs from blocks 1–3 under identical conditions (jitter). | Nothing. |
| sysmem/partition | Depends on what was resident on the recording PSP. Every model of the hardware path tried still gives "high". | A memory-map dump from hardware. |
| threads/vpl/memory | About 16 KB more was resident on the recording PSP (632 against 637). | Same. A calibrated resident block would move every game's addresses. |
| video/psmfplayer/basic | Prints addresses inside libpsmfplayer.prx, which the suite doesn't ship. | That module. |
| video/psmfplayer/playmode | Slow motion, step frame and the warm-up need libpsmfplayer's rules. | Reverse engineering of libpsmfplayer.prx. |
| video/mpeg/basic, video/pmf, video/pmf_simple | Even after 5.4, thousands of AU and decoder lines remain, and 27 checkpoint markers look like timing noise. | Full sceMpeg demuxer fidelity (L). |
| jpeg/decodeycbcr | Hardware writes into the test's frame buffer when the output is NULL. Where exactly couldn't be determined. | Hardware data. |
| jpeg/csc ("extra bits"), jpeg/mjpegcsc (heights under 16, stride −4 and 1, height 479) | Behaviour not determinable from the test. | Hardware data. |
| io/iodrv/iodrv, line 2 | The expected output came from an emulator that turned flags 0x202 into 7. | Nothing legitimate. |
| umd/raw_access, rest | Contradicts Sony's isofs. | 6.4. |

## Order and dependencies

```
Phase 1 (runner environment)
  -> Phase 2 (small fixes; 2.1 needs 1.3, 2.7 needs 3.2 to finish dispatch)
  -> Phase 3 (timing: 3.4 and 3.5 tuned together with 1.2)
  -> Phase 4 (GPU and cache models; independent, can run alongside Phase 3)
  -> Phase 5 (libraries; 5.3 needs 1.2)
  -> Phase 6 (your decisions)
```

Each phase is one or more commits, each through the gates. The runner's known-differences list
starts with the last table and shrinks as tests pass.

## Decisions needed from you

1. **Recording environment in the runner:** the test-display path, fixtures (file dates, a
   generated disc), extended memory, and per-test host0 naming. Are these acceptable as long as
   each is documented with its evidence?
2. **Test-only hardware models:** the data cache and VRAM mirror flags force the interpreter and
   are used only by the runner. Acceptable?
3. **pspdev toolchain:** download and install it to rebuild vregs (6.1)?
4. **AAC tables (5.1):** take them from ffmpeg's `aactab.c` (LGPL; see PLAN.md §7 for the license
   posture), or transcribe them from ISO 14496-3?
5. **Known-differences list:** the gate becomes "0 unexpected failures" with the list above
   reported separately.
