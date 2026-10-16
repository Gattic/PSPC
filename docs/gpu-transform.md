# GE vertex work: in parallel, and on the GPU

PSPC's GE does its vertex work (decoding, morphing, skinning, transform, lighting, texture-coordinate
generation, fog, clipping and culling) in the GE's own arithmetic: float24 products truncated as the
hardware truncates them, sums aligned to the largest term, reciprocal and square-root tables. The
software renderer is the definition. Until 2026-10-16 all of it ran on the emulation thread, one vertex
at a time, with both renderers; in Logan's Shadow and Battlefront II it was most of the GE's time.

Two changes move it off that thread. Both keep every result bit for bit.

## Draws transformed in parallel (all renderers)

Games submit thousands of small draws a frame (Logan's Shadow: about 5000, most with 4-7 vertices), so
one draw can't usefully be split across threads. `src/gpu/transform.cpp` records each draw instead,
with a copy of what its vertices and primitives depend on:

- the transform state, including the gstate values the vertex code used to read directly;
- a snapshot of the raster state (`binner::SnapshotState`);
- morph weights, bone matrices, the culling and shading modes;
- the UV and normal that formats without them carry over (worked out only when a draw needs them).

When the binner's queue is next needed (a flush, a target change, a transfer, a CLUT load that could
read what the draws draw), every recorded vertex is transformed at once on the worker threads, and the
primitives are assembled draw by draw in the original order. The queue ends up exactly as if the draws
had gone in one by one, so batches, flush points and states are unchanged. Workers run in the caller's
floating-point mode.

## Transform and lighting on the GPU (Direct3D 11 and OpenGL)

With the Direct3D 11 renderer (Windows) and the OpenGL renderer (Linux), transform-mode triangles,
strips and fans go to the GPU (`src/gpu/geometry.h`, `src/gpu/hw/d3d11_geometry.cpp`):

- The CPU still decodes the vertices (in parallel), with morphing and skinning. It hands over the
  decoded vertices, each draw's transform state, and one *job* per call `ProcessTriangle` would see,
  in assembly order (topology, culling winding, provoking vertex).
- `CSVertex`, a compute shader, is `ReadVertex`: transform, viewport, fog, normals, texture matrix or
  environment map, and all four lights.
- `CSTriangle` is `ProcessTriangle` for one job: the outside and culling tests, near-plane clipping
  into at most two triangles, flat shading, the binner's winding test, and `DrawData::EmitTriangle`'s
  vertices with their texture level. A triangle a job doesn't make is degenerate and draws nothing.
- The pixel shader then draws them like any other vertices, in the batch's order.

The shaders compute the GE's arithmetic with integer operations: exact 32 x 32-bit products from
16-bit halves, the adder's alignment, the reciprocal and square-root tables generated from
`gemath.cpp`, and the CPU's rounding where a value leaves the float range (denormals and infinities
made by hand, since the GPU flushes denormal operands). The few plain float operations are `precise`,
which D3D11 rounds correctly. With the device's extended double instructions, the texture-level
gradient and the reciprocal w of vertices behind the camera are computed in doubles as on the CPU.

Lines, points, rectangles, through-mode (2D) draws, immediate vertices, curves and draws that continue
the previous one (`KEEP_PREVIOUS`) stay on the CPU. The software renderer uses the parallel CPU path.

The shaders compile in the background the first time (about 8 s on a Core i7-13650HX) and are cached
under the cache root (`cache/shaders`); until they're ready the CPU transforms. Later starts load the
cache.

### OpenGL

The OpenGL renderer runs the same shaders. `tools/gl_shaders.py` turns `CSVertex` and `CSTriangle`
into GLSL at build time, as it does the pixel shaders, so the GE's arithmetic has one source. What
differs:

- The structured buffers are std430 storage blocks with the C++ layouts. std430 aligns a `vec3` to 16
  bytes, so `GeVertexIn`'s normal and position and `GeShadeLight`'s padding are float arrays there;
  the shaders' own `ClipV` has a 64-byte stride. CSTriangle writes its vertices into a storage buffer
  that the draws then read as their vertex buffer.
- Every float operation that `precise` covers in HLSL is `precise` in GLSL too, and so are the doubles:
  D3D11 compiles with IEEE strictness, but GLSL compilers may fuse a double multiply-add otherwise.
  Shifts never reach 32 bits (GLSL leaves that undefined; HLSL masks it), the `GL_` light flags are
  renamed (GLSL reserves the prefix), and so are the variables `sign` and `flat`.
- Doubles are core in GLSL 4.00, so the texture-level gradient and the reciprocal w of vertices behind
  the camera always use them.
- The driver compiles the shaders on threads of its own (`KHR_parallel_shader_compile`), and the CPU
  transforms until they're ready. Mesa compiles both in about 1.5 s on llvmpipe and keeps them in its
  shader cache, so later starts take about 0.1 s. Without that extension the compile happens at startup.
  (llvmpipe runs them on the CPU, so there they only test exactness: Battlefront II to VBlank 9000 took
  1598 s with them and 834 s without, in the serial-fragment mode llvmpipe needs.)
- A draw whose state the renderer would hand to the software reference (color and depth overlapping in
  memory, a scissor past the target's stride) stays on the CPU: `RenderBackend::TransformsGeometryFor`.
  When a batch has to go primitive by primitive, the geometry runs once and each of its runs is drawn
  by itself.
- A run of GPU-made triangles covers the whole scissor, while CPU-made triangles cover their own
  bounds, so the batches they're drawn in differ. The renderer's viewport used to follow the rows a
  batch covered, and llvmpipe clips triangles to the viewport: the same triangle came out a little
  different in a taller batch (a depth step, a color step at the odd pixel). The viewport is now the
  same for every batch (at least 512 x 512, sides powers of two, so pixel positions map to clip space
  and back exactly).

## Settings

| Environment variable | Effect |
|---|---|
| `PSPC_GPU_TRANSFORM=0` | Transform on the CPU with Direct3D 11 or OpenGL too. |
| `PSPC_GPU_TRANSFORM_WAIT=1` | Wait for the shaders at startup instead of transforming on the CPU until they're compiled. |
| `PSPC_GPU_TRANSFORM_VERIFY=1` | Also transform on the CPU, read the GPU's vertices back, and compare every job bit for bit (slow). Mismatches are logged; `PSPC_GPU_STATS=1` prints the counts. |

`D3D11Options` and `OpenGLOptions` have `gpuTransform`, `waitForShaders` and `verifyTransform` for the
same; `RenderBackend::GetTransformStats` returns the counts.

## Verification

- `PSPC_GPU_TRANSFORM_VERIFY=1` through Logan's Shadow to VBlank 7800 (menus, briefing and the first
  mission): 102,440,471 triangle jobs, none different from the CPU.
- All 42 game checkpoints with Direct3D 11 and the GPU transform are byte-identical to the frames
  recorded with the CPU transform, and all 42 with the software renderer to the frames before the
  parallel transform.
- `gpu_hardware_transform_matches_cpu` (unit test, WARP on Windows, the OpenGL renderer on Linux): a
  lit, textured, fogged scene with every light kind, material colors from the vertex, both specular
  modes, a texture matrix and an environment map, skinning, morphing, flat shading, every cull mode,
  indexed strips and fans, triangles through the near plane with depth clipping on and off, and a batch
  OpenGL draws primitive by primitive with draws it leaves to the CPU. The VRAM is byte-identical with
  and without the GPU transform, and no job differs.
- OpenGL on Mesa 23.2.1 llvmpipe (Ubuntu 22.04 under WSL, `PSPC_GL_SERIAL_FRAGMENTS=1`):
  `gpu_hardware_transform_matches_cpu` passes, and `PSPC_GPU_TRANSFORM_VERIFY=1` found no difference
  in Logan's Shadow to VBlank 3600 (the main menu, 1,273,218 triangle jobs) or Battlefront II to
  VBlank 9000 (the Boz Pity battle, 774,584 jobs). Battlefront II's 30 frames to VBlank 9000 are
  byte-identical with and without the GPU transform. No hardware GPU has run it on Linux yet. GLSL only promises double division as
  accurate as float division, so a driver that doesn't round it correctly could make the texture level
  differ; `PSPC_GPU_TRANSFORM_VERIFY=1` shows it.

## Measurements

GE time per frame (`PSPC_GPU_STATS`), headless, in the interpreter (these games' native code was stale),
on a Core i7-13650HX with an RTX 4060 Laptop GPU:

| Scene | Before (2026-10-16) | Parallel CPU transform | GPU transform |
|---|---:|---:|---:|
| Logan's Shadow, first mission, D3D11 | 19.6-24.3 ms | 8.3-11.4 ms | 8.1-8.4 ms |
| Logan's Shadow, first mission, software | 26.9-31.8 ms | 15.3-18.9 ms | (CPU) |
| Battlefront II, Dagobah battle, D3D11 | 14.1-17.1 ms | 4.6-5.3 ms | 2.7-3.3 ms |
| Battlefront II, Dagobah battle, software | 19.8-21.9 ms | 9.2-10.9 ms | (CPU) |

With the process limited to 4 logical CPUs, where the parallel transform has few spare cores:

| Scene, D3D11, 4 logical CPUs | CPU transform | GPU transform |
|---|---:|---:|
| Logan's Shadow, first mission | 12.9-14.4 ms, 26.8-30.3 frames/s | 7.5-8.2 ms, 32.4-36.3 frames/s |
| Battlefront II, Dagobah battle | 9.4-9.6 ms, 35.8-37.4 frames/s | 3.6-3.9 ms, 45.0-47.6 frames/s |

Frames per second here are interpreted, headless and unthrottled; with native code the CPU share of the
emulation is smaller and the GE's larger. What remains of the GE's time with the GPU transform is mostly
Direct3D 11 waiting for VRAM readbacks the games ask for (Logan's Shadow reads its framebuffers back
several times a frame) and the primitive queue's bookkeeping.
