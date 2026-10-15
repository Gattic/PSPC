# Resolution performance diagnostics

## Real-time presentation catch-up

Higher render scales increase GPU work in both interpreter and AOT modes. Previously,
when emulation fell behind the host clock, `sceDisplay` presented **every** overdue
VBlank. Repeatedly presenting the current framebuffer added more GPU work while the
runtime was already trying to catch up.

Real-time mode now presents only a VBlank whose next display deadline has not passed.
It still processes every framebuffer latch, guest VBlank, interrupt, waiter and VBlank
hook. It does not skip GE drawing or change resolution. Fast-time mode retains every
presentation for deterministic captures and tests.

A fixture using the actual display/scheduler/SDL host and a simulated 20 ms presenter
reproduced the problem: after a 100 ms stall, processing eight VBlanks took **160.76 ms
with eight presentations**, versus **40.24 ms with two presentations** after the change.
Both runs processed eight VBlank hooks; fast-time still presented all eight frames.
This is catch-up overhead, **not a measured fourfold increase in game FPS**.

## Reproducible checks

Build an optimized runtime with tests enabled. On Windows, the usual `build.cmd`
places binaries in `build\RelWithDebInfo\bin`; substitute your build directory below.

```bat
ctest --test-dir build\RelWithDebInfo -V -R "^display_pacing_"
build\RelWithDebInfo\bin\pspc_interp_tests.exe --bench
set GPU_HW_BENCH=1
build\RelWithDebInfo\bin\pspc_unittests.exe gpu_hardware_resolution_perf
set GPU_HW_BENCH=
```

The interpreter benchmark reports median instruction throughput for scalar and memory
loops and checks instruction accounting. The GPU benchmark uses four full-screen,
wrapped, bilinear, alpha-blended sprites at 1×, 2×, 4× and 8×. Shader compilation and
warm-up are outside the timer; VRAM synchronization includes GPU completion. On Windows
it selects the hardware adapter, not WARP. It is a synchronous rendering microbenchmark,
not the normal guarded-VRAM, windowed gameplay path.

Linux equivalents and an opt-in actual AOT/interpreter-to-GPU integration check:

```sh
build/LinuxRuntime/bin/pspc_interp_tests --bench
GPU_HW_BENCH=1 PSPC_RENDERER_REQUIRE=1 \
  build/LinuxRuntime/bin/pspc_unittests gpu_hardware_resolution_perf
PSPC_AOT_RENDER_TEST=1 PSPC_RENDERER_REQUIRE=1 \
  ctest --test-dir build/LinuxRuntime -V -R '^aot_runtime$'
```

The last check compiles a synthetic guest module, verifies native versus interpreted
instruction counts, and uses its output to draw changing colors at 1×/2×/4×/8×. It
checks guest VRAM writeback and every pixel of the full-resolution display image.
The GPU option is explicit so ordinary AOT CTest runs remain display-independent.
Generated AOT fixtures are temporary and removed by the existing test driver.

## Validation limits

The catch-up regression and synthetic AOT/interpreter rendering checks passed on Linux
with Mesa Intel UHD Graphics (CML GT2). The textured GPU baseline was approximately
10.9/11.1/29.1/100.9 ms per frame at 1×/2×/4×/8×, respectively. This illustrates the
remaining fill-rate cost; it is not a Windows result or a before/after GPU speedup.

Windows/D3D11 execution and retail high-resolution gameplay were **not available** for
this change. The original Windows report therefore still needs confirmation on the
affected machine. Compare the same scene, renderer, texture options and window size
with/without `--interp` at each `--render-scale`; for AOT, confirm the log actually loads
the game's native modules. `PSPC_GPU_STATS=1` also exposes D3D11 downloads, guest faults,
texture work and wait time. Do not treat interpreter fallback as an AOT measurement.

Shader-wrap masking, inline scalar dispatch, word-decode caching and inline opcode-kind
lookup were tried and reverted: they did not show consistent throughput gains in the
local benchmarks. No CPU decoder, instruction semantics or shader changes are retained.
