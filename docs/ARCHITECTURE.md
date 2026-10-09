# Architecture

PSPC runs PSP software as a native Windows process. The design rule from [PLAN.md](../PLAN.md):

> Recompile what is pure software; reimplement natively what touches hardware or CPU state.
> Use the decompiled firmware as the specification for everything we reimplement.

```mermaid
flowchart TB
  subgraph offline["Offline (once per game / firmware)"]
    iso["ISO / CSO / PBP / PRX"] --> dec["pspc-decrypt<br/>(KIRK, gzip/KL4E, PSAR)"]
    dec --> rec["pspc-recomp<br/>Allegrex -> C++"]
    rec --> cc["clang-cl -> &lt;sha1&gt;.dll<br/>(tools/pspc_build.py)"]
  end
  subgraph rt["pspc.exe"]
    ld["module loader<br/>(src/runtime/kernel/module.cpp)"]
    cpu["execution: AOT DLL code + interpreter fallback<br/>(src/runtime/core)"]
    k["native kernel: threads on fibers, sync objects,<br/>memory partitions (src/runtime/kernel)"]
    hle["native libraries (HLE), NID-keyed<br/>(src/runtime/hle/modules)"]
    m33["native M33 layer<br/>(src/runtime/m33)"]
    dev["GE renderer (src/gpu), audio (src/audio),<br/>storage (src/fs), host SDL2 (src/runtime/host)"]
  end
  cc --> ld --> cpu
  cpu <--> hle
  k --- hle
  m33 --> hle
  hle --> dev
```

## Source tree

| Path | What |
|---|---|
| `src/allegrex/` | Instruction list (`ops.inc`, `vfpu_ops.inc`), decoder, disassembler and the **shared semantics** (`sem.h`, `vfpu_sem.h`): one definition per instruction used by both the interpreter and generated code. `ctx.h` (guest CPU state) and `aot_abi.h` (runtime <-> DLL interface) are ABI. |
| `src/loader/` | ELF/PRX parsing, PSP relocations (type A and B), import/export tables, `~PSP` header. Shared by runtime and recompiler. |
| `src/recomp/` | `pspc-recomp`: module image, function discovery, C++ code generation. |
| `src/runtime/core/` | Guest memory (512 MB reservation, aliased mirrors), dispatch table (guest address -> native function), interpreter, CPU glue. |
| `src/runtime/kernel/` | Scheduler (guest threads = Windows fibers on one host thread), threads, callbacks, wait queues, memory partitions, module loading. |
| `src/runtime/hle/` | HLE framework (`hle.cpp`: syscall table, NID registry; `hle_wrap.h`: automatic PSP-EABI argument marshalling) and one file per library in `modules/`. |
| `src/runtime/io/`, `src/runtime/media/` | The PSP device namespace (`ms0:`, `host0:`, `flash0:`, `flash1:`, `disc0:`, `umd0:`, `emulator:`). |
| `src/runtime/m33/` | The M33 CFW behaviours (SystemCtrl, KUBridge, VshCtrl, config.se, plugins, NID resolver). |
| `src/fs/` | Disc images (ISO/CSO/ZSO/DAX), ISO9660, host file system, PBP, PARAM.SFO. |
| `src/gpu/` | GE display-list processing, timing and the multithreaded software rasterizer. |
| `src/audio/` | Audio decoders and mixers (ATRAC3/3+, SAS, MP3/AAC through Media Foundation). |
| `src/media/` | Video and image decoding for sceMpeg/PSMF/sceJpeg (PS demux, H.264 through Media Foundation, WIC). |
| `src/runtime/dialogs/` | sceUtility dialogs (message, OSK, save data...) and their host-side prompts. |
| `src/frontend/` | `pspc.exe` (command line, LoadExec relaunch), `pspc-launcher.exe` (game library), `pspc-iso.exe`. |
| `tools/pspc-decrypt/` | Separate GPL-3.0 executable (uses libkirk). Never linked into `src/`. |
| `tools/*.py` | AOT build driver, autotest runner, firmware/M33 extraction and analysis, NID database generation, Ghidra pipeline. |

## Execution model

**Guest memory.** `g_membase` points at a 512 MB reservation; every PSP virtual segment folds onto
physical memory with `addr & 0x1FFFFFFF`. The scratchpad, VRAM (+3 mirrors) and RAM are views of
one page-file section, so mirrors alias for free. A stray access maps a shared zero page and is
logged (PPSSPP's "ignore bad accesses" default); `--strict-mem` makes it fatal.

**Two engines, one semantics.** For every non-control-flow instruction `X` there is
`sem_X(Ctx *c, u32 insn)` in `sem.h`/`vfpu_sem.h`. The interpreter calls it with the fetched word;
generated code calls it with the word as a literal, so the compiler folds the field decoding away.
Control flow (delay slots, likely branches, calls, returns) is implemented separately in the
interpreter and the code generator, against the conventions in `aot_abi.h`.

**Calls and returns.** Guest functions become C functions `B_addr(Ctx *c, u32 ra0, u32 sp0)`, plus a
call entry `F_addr(Ctx *c)`. A `jal` inside a module is a direct C call; anything else goes through
`dispatch::Call`, which runs native code if registered for that address and otherwise interprets.
A jump into a function (tail call, or a jump to a label that also starts a function) runs its body
with the jumper's `ra0`/`sp0`, so it returns where the jumper would. `jr $ra` returns when `$ra`
equals `ra0`; otherwise (longjmp-style) a non-local return propagates up the C stack
(`c->unwinding`) until a frame whose call site matches. Unknown jump targets fall into the
interpreter, which calls back into native code whenever it reaches a registered function; jumps
from the interpreter to HLE thunks are run inline so loops that tail-call imports don't nest.

**Time.** `Ctx::budget` counts guest cycles, one per executed instruction in both engines (the
interpreter per instruction, generated code per straight-line block). With `--fast-time` the
scheduler's clock is the cycles consumed plus the time HLE functions charge, and idle time is
skipped, so timing is deterministic and identical with and without native code; otherwise it
follows the host clock. Back-edges check `budget < 0` to deliver due events and interrupts.

**Self-modifying code.** Native code is compiled from the module as loaded. When the guest flushes
the instruction cache (`sceKernelIcacheInvalidate*`), changed code words are found by comparison
with a load-time snapshot, and the functions holding them switch to the interpreter (a per-function
flag in the DLL catches direct calls from other native code). CFW plugins patch games this way.

**Threads.** Each PSP thread is a Windows fiber with its own guest stack (in guest RAM, laid out
like the firmware does) and host stack. All fibers run on one host thread, which keeps scheduling
deterministic. A blocking HLE call (`sched::Wait`) switches to the scheduler fiber; strict
priority scheduling, callbacks, timeouts and interrupts (VBlank, alarms, GE) follow the PSP's
rules. When no thread is left alive the run ends.

**HLE.** Import stubs are rewritten to `jr ra; syscall CODE` and registered in the dispatch table;
`CODE` indexes a table of (library, NID) -> native function. Functions are declared with
`HLEFN(nid, fn)`; arguments are read from `$a0-$t3` / `$f12-` per the PSP EABI and results are
written to `$v0/$v1/$f0`. Unknown NIDs log once and return 0.

**Modules.** `kernel::LoadModuleFromMemory` classifies the file (plain ELF, M33 gzip-packed, KIRK
encrypted -> `pspc-decrypt`), allocates memory (keeping the low 16 bits of an AOT module's
recompile address so LO16-only relocations stay valid), relocates, binds imports (to other guest
modules' exports, else native), and loads `cache/aot/<sha1>.dll` when present. Firmware libraries
a game ships on its disc (codecs, network, fonts...) are recognized by module name and satisfied
natively instead of being loaded. A second copy of a module that already runs natively is
interpreted (one DLL instance holds one relocation delta). Before a module's `module_start` runs,
M33's start-module handler (if a plugin set one) is called with the module's `SceModule2`.

**LoadExec.** `sceKernelLoadExec` (and M33's LoadExec family) restart the runtime in a fresh process
that boots the requested executable with the same devices mounted, like the PSP's reboot.

**Diagnostics.** `Fatal()`, host stack overflows, breakpoints and illegal instructions print a
symbolized host backtrace; frames inside AOT DLLs are named after the guest function.

## AOT DLLs

See [RECOMPILER.md](RECOMPILER.md). A DLL holds one module; the runtime matches it by the SHA-1 of
the decrypted ELF, sets its relocation delta and registers its functions in the dispatch table.

## What is native vs. recompiled

| Native (HLE) | Runs as guest code (recompiled or interpreted) |
|---|---|
| kernel (threads, sync, memory, modules, interrupts), I/O, display, controller, power, RTC, UMD | games, homebrew, game-shipped libraries (sceDeflt, sceMd5, sceHeap, sceCcc...) |
| GE graphics, audio output, SAS, ATRAC/MP3/MPEG codecs, network (offline) | M33 user-level UI (VSH menu, recovery) once firmware is present |
| sceUtility dialogs and save data | the 4.01 XMB (vshmain, paf, plugins) once firmware is present |
| M33 SystemCtrl/KUBridge/VshCtrl behaviours | M33 game plugins (`seplugins/game.txt`) |
