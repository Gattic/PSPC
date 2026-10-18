# Higher-resolution rendering performance

Measured on 2026-10-15 against `9a77e8b`, with the changes described below. The
baseline includes the same diagnostic hooks and benchmark harness as the optimized
build, with the original shaders.

## What slows down

Internal resolution multiplies both dimensions: 4x shades 16 times as many pixels
as 1x, 8x shades 64 times as many, and 16x shades 256 times as many. The GE pixel
shader implements texture filtering, blending, depth/stencil, masks and dithering
using rasterizer-ordered views; this work repeats at the internal resolution.

D3D11 timestamp queries on four full-screen wrapped, bilinear, alpha-blended
sprites at 8x attributed 98.9% of the sampled GPU time to textured draws and 1.1%
to readback reduction. Upload expansion was below 0.01%. The baseline shader
disassembly contained 21 integer-division instructions, including repeated
remainder operations for the texture taps and pixel-coordinate division for
dithering even when dithering was disabled. These are static instruction counts,
not instruction-level hardware profiling.

## Changes

- Wrap power-of-two texture periods with a bit mask. For other periods, including
  3x rendered/upscaled textures, use one unsigned remainder and sign correction.
  Unsigned arithmetic also fixes negative-coordinate wrapping on GLSL, where
  negative operands to `%` have undefined results.
- Derive the adjacent bilinear tap from the first wrapped coordinate instead of
  wrapping it again.
- Keep the conversion from internal pixels to PSP pixels inside the dithering
  branch. Explicit HLSL branches keep the expensive divisions inside their
  fallback paths rather than executing them before selecting a result.

The shared shader source serves D3D11 and OpenGL. The GLSL generator strips the
HLSL branch hints. Resolution, filtering, blending, supersampling and texture
quality settings retain their meaning. No approximate filtering or lower
resolution is introduced. The final shader contains nine static division
instructions; ordinary power-of-two wrapping and disabled dithering bypass them.

The unsigned wrapping rule follows the
[GLSL 4.60 arithmetic specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#expressions).

## Repeated measurements

Hardware: RTX 4060 Laptop GPU, NVIDIA driver 581.57, Core i7-13650HX, Windows 11
10.0.26200. Both builds use MSVC 19.44, `RelWithDebInfo`, and the same optimization
flags. The benchmark process is pinned to logical CPU 2. Each resolution has
750 ms of sustained warm-up, followed by 301 individually timed frames; shader
compilation and allocation warm-up are excluded. Each frame completes its GPU
work and VRAM readback before the timer ends. Seven separate runs are compared.
Timestamp profiling is disabled during these measurements.

CPU/GPU clocks remain under OS control. Low-resolution CPU/driver timing is
noisier than GPU-bound high-resolution timing. Reported confidence intervals
resample the seven run medians; they describe these runs on this machine, not
other GPUs or game FPS.

Completed-frame times (milliseconds), using the median of seven run medians:

| Resolution | Before | After | Less time | Bootstrap 95% interval |
|---|---:|---:|---:|---:|
| 3x | 1.181 | 1.062 | 10.1% | 9.1–11.0% |
| 4x | 1.699 | 1.498 | 11.8% | 11.1–12.8% |
| 8x | 5.199 | 4.390 | 15.6% | 15.2–15.9% |
| 16x | 19.279 | 16.176 | 16.1% | 15.7–16.2% |

Run-to-run coefficients of variation of the medians are below 1% at these
resolutions in both builds. Median per-run tail timings and sample deviations:

| Resolution | Before p99 | After p99 | Before standard deviation | After standard deviation |
|---|---:|---:|---:|---:|
| 3x | 1.269 | 1.151 | 0.036 | 0.040 |
| 4x | 1.935 | 1.619 | 0.048 | 0.044 |
| 8x | 5.487 | 4.662 | 0.062 | 0.054 |
| 16x | 19.850 | 16.492 | 0.123 | 0.065 |

Intervals use 10,000 independent bootstrap resamples of the run medians. The
single-CPU 1x/2x results were too noisy for a reliable conclusion. Seven further
paired runs with logical CPUs 0–5 available to the driver measured 0.282 to
0.278 ms at 1x and 0.592 to 0.494 ms at 2x. Those checks did not show a regression,
but run variation remained 4–11%, so no precise low-resolution speedup is claimed.
Raw run logs, disassemblies and the machine-readable summary are retained locally
under `build/resolution-perf/`.

## Correctness and limits

- Windows: all 20 CTest groups passed, including all 146 unit tests.
- Linux: all six hardware-renderer tests passed using Mesa llvmpipe's serial
  fragment verification mode. The same six tests passed with GCC
  AddressSanitizer/UBSan. Leak detection was disabled; the ASan quarantine was
  reduced to fit the software renderer in WSL memory. This is correctness
  evidence, not hardware OpenGL performance evidence.
- New tests cover negative and positive texture periods, nearest/bilinear seam
  samples, 3x/4x texture upscaling, and rendered textures at 3x/4x/16x.
- Daxter's scripted headless run to VBlank 3300 at 8x produced byte-identical
  480x272 and 3840x2176 captures before and after, with zero bad memory accesses.
  This run used the interpreter because the existing native cache was stale.
- A fresh, isolated native build also completed the windowed 8x run to VBlank
  3300 with zero interpreted instructions and zero bad memory accesses. Both
  builds submitted exactly 148,853 draws. The captures differ only in the
  save-dialog minute digits (90 pixels at 1x); windowed save timestamps use host
  time. This path ends on the save-completed dialog. The headless scripted path
  reaches the first level. The windowed run is paced around 60 VBlanks/s, so these
  runs do not establish a whole-game FPS improvement.

The change reduces avoidable shader arithmetic; high resolutions still have
quadratically more pixels. The profiler samples draw, upload-expansion,
readback-reduction and display GPU commands. It does not separately time texture
decoding/upscaling or resource copies. Per-command queries perturb the workload,
so use them to locate expensive stages and benchmark again with profiling off.

## Reproduce

Build and run the bounded hardware benchmark in PowerShell:

```powershell
.\build.cmd RelWithDebInfo --target pspc_unittests pspc
$env:GPU_HW_BENCH = '1'
$env:GPU_HW_BENCH_FRAMES = '301'
$env:PSPC_GPU_PROFILE = $null
(Get-Process -Id $PID).ProcessorAffinity = 4
.\build\RelWithDebInfo\bin\pspc_unittests.exe gpu_hardware_resolution_perf
```

The default resolutions are 1x, 2x, 3x, 4x, 8x and 16x. Set
`GPU_HW_BENCH_SCALE` to any value from 1 through 16 to isolate it. Output includes
median, p99 and sample standard deviation in milliseconds per completed frame.
Repeat the command at least five times for each build, without other GPU work.

For stage profiling, set `PSPC_GPU_PROFILE=1` and run the same command or a game
with `--renderer d3d11 --render-scale 8`. GPU totals and sample counts print on
shutdown; `PSPC_GPU_STATS=1` also prints periodic CPU-side statistics. Pending
queries are polled without flushing; the queue is bounded, invalid timestamps
are rejected, failed queries are discarded, and skipped/disjoint/failed/pending
samples are reported. Unset
`PSPC_GPU_PROFILE` before measuring before/after performance.

## Linux OpenGL target readbacks (2026-10-18)

A separate investigation on Fedora 44 / Intel UHD CML GT2 / Mesa 26.2.4 found
avoidable CPU/GPU synchronization in `OpenGLBackend::DrawBatchImpl`. A bounded
120-second Dark Mirror profile recorded 37,073 `glGetTextureImage` calls requesting
20.66 GB, with 83.77 seconds inside that API. Matched runs with geometry verification
present versus absent both missed VBlank 1800; disabling verification did not solve
this bottleneck.

The backend downloaded color and depth before **every textured segment**, even
when its textures could not read either target. Skipping RAM-only dependencies
first let Dark Mirror reach its endpoint, but was insufficient for the other two
games. A Logan's Shadow trace then showed swizzled CLUT textures at `0x040cc000`
and above while the render targets ended at `0x040cc000`.

Intermediate downloads now require a possible overlap with an active mip's
readable memory range. Address mirrors are folded, wrapped targets remain
conservative, and eager batch-end guest-memory writeback is unchanged. Using the
sampler's existing readable bound avoids duplicating format/swizzle address math;
it may still synchronize more than the actual texels require. No resolution,
filtering, geometry-verification, interpreter or deadline settings were relaxed.

Three Linux regressions cover RAM and unrelated VRAM textures (nine readbacks
before the relevant fix, one after, with exact guest pixels), plus in-batch indexed
feedback through a mirrored address, higher mip and wrapped target. The latter
retains both required readbacks and checks exact RGB/stencil output. The configured
build and **40/40 CTests** pass, including 160 registered unit cases with zero
failures and synthetic interpreter/AOT render-writeback at 1x/2x/4x/8x/16x.
The hardware-required NVIDIA GTX 1050 Max-Q GPU group also passes **54/54**.
Missing reference corpora still cause the existing explicit skips.

Fresh isolated retail runs used the original scripted inputs, 120-second limit,
2x rendering/texture upscaling, strict guest memory and geometry verification:

| Game | Required VBlank | Result | Elapsed / last capture |
|---|---:|---|---|
| Dark Mirror | 1800 | PASS | 52.20 s / 1800 |
| Combat Ops | 3900 | PASS | 117.02 s / 3900 |
| Logan's Shadow | 3600 | FAIL: deadline | 120.31 s / 3000 |

There were zero geometry differences or reported guest-memory errors. Common
before/after captures were byte-identical. These are finite title/menu progression
observations, not sustained gameplay or a repeatability/performance guarantee.
At this stage the overall retail gate remained incomplete: Logan's Shadow still
had expensive late GE work. Approximately 156.3 MiB of the then 192 MiB investigation
cap was retained. Subsequent authorized archival, profiling and repair are described
below; the original failed runs were preserved rather than retried with longer deadlines.

Evidence and all failed/intermediate attempts are retained under
`build/linux-pull-check-048f7f9/retail-investigation/`, notably `REPORT.md`,
`final-results.json`, revision manifests, `final-ctest.log` and `final-nvidia-gpu.log`.

## Linux OpenGL decoded-texture reuse (2026-10-18)

A retained genuine-AOT/interpreter comparison isolated the remaining upload/readback
cost. In matched Logan's Shadow VBlanks 1800–2400, switching to AOT reduced wall time
from 64.113 to 57.552 ms/VBlank, but texture upload remained about 27 ms/VBlank.
The interpreter control missed 3600 at 120 seconds; AOT reached it once at 119.559 seconds.
This did not establish comfortable headroom or fix the forced-interpreter gate.

An observation-only probe found exact RGBA-image repeats in 81.3% of the late uploads.
Per-pixel surface-validation uploads did not increase during that interval.
The backend now retains up to 32 decoded textures per context, bounded by 64 MiB of
CPU decoded plus final GPU-image payload. Each draw still decodes current guest
memory, including CLUT and mip data. A hit requires exact decoded pixels (not merely
a hash), atlas dimensions, mip layout, upscale factor and wrap settings. Entries
are least-recently-used; oversized images remain transient, and context teardown
releases the retained GL resources. Driver overhead and transient miss allocations
are not included in the payload bound. Eager target downloads, feedback handling,
geometry verification and filtering are unchanged.

The new interpreter run used the retained frame-boundary observer and identical
normalized command/environment, with fresh isolated saves and the original 120-second
limit. Matched VBlanks 1800–2400:

| Metric | Before | Texture reuse |
|---|---:|---:|
| Wall ms/VBlank | 64.113 | 37.363 |
| Texture-upload calls | 89,736 | 16,792 |
| Upload API ms/VBlank | 27.146 | 2.652 |
| Readback calls | 1,201 | 1,201 |
| Readback API ms/VBlank | 12.909 | 13.849 |

This interval's wall time fell 41.7%. Readback, geometry-buffer transfer, draw and
other non-texture work counts/units match in all four common 600-VBlank intervals.
API wall time includes queued driver/GPU waits; these are not shader timestamps.
Readback remains substantial and was deliberately not deferred.

All three original forced-interpreter endpoints now pass: Dark Mirror 1800 in 49.936 s,
Combat Ops 3900 in 90.905 s, and Logan's Shadow 3600 in 99.205 s. All 36 native/high-resolution
captures match retained references byte-for-byte, with zero reported guest-memory
errors or geometry differences. This is a single bounded progression pass per game,
not sustained-gameplay or repeatability evidence.

The three initial deterministic render regressions failed upload-count assertions before
the repair and pass afterward. They cover CPU/CLUT/higher-mip edits, swizzling,
address-independent reuse, wrap/layout separation, count/byte eviction, oversized
images and context ownership, alongside the existing feedback/writeback tests.
At the original implementation checkpoint, the full configured build and strict
Intel/X11 CTest suite passed 40/40, with 163 unit cases and the existing
reference-dependent skips. Evidence, the original executable
and all earlier failed attempts are retained under
`build/linux-pull-check-048f7f9/retail-investigation/texture-reuse/`; see `REPORT.md`,
`comparison.json`, `strict-ctest.log`, per-game `parity.json` and `final-audit.json`.

A subsequent cache review added whole-image spatial and alpha-only edit checks,
observed actual GL allocation/deletion lifetimes, and exercised feedback after
warming the cache. Its unit run recorded 164 cases with zero failures (the existing
reference-data skips remain), and the required OpenGL group passed 8/8 on both
Intel and NVIDIA. These are distinct from the original full-suite totals above.
The review also independently reconciled the retained timings, 36 capture matches
and preservation hashes. No production renderer change or new retail run was
needed; see `review/REPORT.md` and `review/final-review.json` under the same
evidence directory. The 64 MiB bound remains retained payload, not a process or
physical-VRAM ceiling.
