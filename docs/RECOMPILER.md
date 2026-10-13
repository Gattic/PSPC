# The Allegrex recompiler (pspc-recomp)

`pspc-recomp` turns one plain PSP module (ELF or PRX) into C++ that compiles into a Windows DLL.
`tools/pspc_build.py` drives it: decrypt -> recompile -> compile with clang-cl -> copy
`<sha1>.dll` into the AOT cache that `pspc.exe` searches (`<pspc dir>/cache/aot`).

```
python -I tools/pspc_build.py Games/SomeGame.iso                # every executable on the disc
python -I tools/pspc_build.py EBOOT.PBP
python -I tools/pspc_build.py some/module.prx --keep-src     # keep the generated C++
python -I tools/pspc_build.py @list.txt --modules-in-parallel 6 --jobs 3
build\RelWithDebInfo\bin\pspc-recomp.exe module.prx --info  # analysis statistics only
build\RelWithDebInfo\bin\pspc-recomp.exe module.prx --info --dump-gaps gaps.txt
```

`pspc_build.py` compiles each module directly with clang-cl and link (`--cmake` uses the generated
CMakeLists.txt instead), links against a snapshot of `pspc_sem.lib`/`pspc_common.lib` so a
concurrent main build can't swap them mid-batch, skips modules whose SHA-1 it already built in the
same run (a disc's BOOT.BIN usually equals its decrypted EBOOT.BIN), and rebuilds a DLL when its
`<sha1>.semver` stamp differs from the current semantics version or a newer `<sha1>.hints` exists.

## Pipeline

1. **Image** (`image.cpp`). The module is loaded into a flat buffer at its recompile base
   (`0x08804000` for PRX - where the runtime loads the first module - or the fixed vaddrs of an
   `ET_EXEC`). Relocations are applied and recorded: HI16 words, R_MIPS_32 data words, words
   patched by variable imports, and the import stub range.
2. **Analysis** (`analysis.cpp`).
   - *Strong entries*: module entry, exports, every `jal`/`bal` target, hints. They are walked
     first, to a fixpoint over the tail-call targets the walks discover.
   - *Weak entries*: code addresses found in relocated data words, in `lui/addiu` pairs, and (for
     fixed executables) data words pointing at plausible function starts. A weak entry that lands
     inside code a strong function already covers is dropped unless it looks like a function start
     (stack-frame prologue, or the word after a `jr ra` and its delay slot): relocated switch
     tables would otherwise turn every case label into a function and duplicate its tail.
   - *Gap entries*: an uncovered word on a function boundary (range start, prologue, or after a
     return / tail jump and its padding) whose next words decode becomes an entry, unless its walk
     branches into already-covered code or hits an invalid word (data decoded as code does both).
     This reaches functions called only through pointers the scans miss (vtables of fixed
     executables).
   - Each function's *body* is everything reachable from its entry through fall-through,
     conditional branches, `j` within its range (the range ends at the next strong entry) and
     recovered jump tables. Bodies may overlap; shared code is compiled into each function.
   - *Jump tables*: `lw rs, lo(base)` where `base = (hi<<16) + index` is resolved backwards through
     `lui/addiu/addu`, bounded by a preceding `sltiu` and (for PRX) by relocation entries.
   - On retail games this covers 99.0-99.9% of the code words (delay slots counted with their
     branch) with about 1x duplication; `--info` prints the numbers.
3. **Codegen** (`codegen.cpp`): per function a body `B_addr(Ctx *c, u32 ra0, u32 sp0)` and a call
   entry `F_addr(Ctx *c)`; ~1 MB translation units; `module.h` declarations; `glue.cpp` with
   `pspc_aot_module()`; a CMakeLists.txt for the DLL.

## Generated code

```cpp
void B_088040AC(Ctx *PSPC_RESTRICT c, u32 ra0, u32 sp0) {
	sem_ADDIU(c, 0x27BDFFE0u);                       // addiu sp, sp, -32
	sem_LUI(c, 0x3C040880u); c->r[4] += g_delta;      // HI16-relocated lui
	c->budget -= 4;                                   // cycles of the block so far
	c->r[31] = (0x088040C4u + g_delta);              // jal 0x08805000
	sem_ADDU(c, 0x00052021u);                        //   (delay slot)
	B_08805000(c, c->r[31], c->r[29]);
	if (PSPC_UNLIKELY(c->unwinding)) { if (c->pc == (0x088040C4u + g_delta)) c->unwinding = 0; else return; }
	c->budget -= 2;
	{ const bool t_ = cond_BNE(c, 0x1440FFF8u);       // bne with delay slot
	  sem_ADDIU(c, 0x24840001u);
	  if (t_) { if (c->budget < 0) g_api->yieldCheck(c); goto L_088040B0; } }
	c->budget -= 2;
	{ const u32 t_ = c->r[31];                       // jr ra
	  sem_ADDIU(c, 0x27BD0020u);
	  if (t_ == ra0) return;
	  g_api->nonlocalReturn(c, t_); return; }
}
void F_088040AC(Ctx *PSPC_RESTRICT c) { B_088040AC(c, c->r[31], c->r[29]); }
```

* **Calls and jumps.** A call passes the new `$ra`/`$sp` as the callee's `ra0`/`sp0`. A jump into a
  function - a tail call, or a jump to a label that also starts a (weak/gap) function - passes the
  jumper's own `ra0`/`sp0`: the target continues the jumper's frame, so its `jr ra` returns where
  the jumper would and its exits to the interpreter stop at the right return. The runtime keeps
  both entries per address (`dispatch::RegisterNative`); `dispatch::Jump` uses the body.
* **Returns.** `jr ra` returns from C when `$ra` equals `ra0`; otherwise it starts a non-local
  return (`c->unwinding`, target in `c->pc`) that unwinds C frames until one expects that address.
  Nobody expecting it means the thread's top frame carries on from there (`dispatch::Resume`):
  natively if the address starts a body, in the interpreter otherwise.
* **Cycles.** One cycle per executed instruction, delay slots included, exactly as the interpreter
  counts: straight-line code accumulates a count paid (`c->budget -= N`) before anything that
  leaves it - control transfers, syscalls, labels. Back-edges check `c->budget < 0` and call
  `yieldCheck`. The scheduler's virtual clock is the budget consumed, so timing (and every test
  that measures it) is identical in both modes.
* `g_delta` is `load base - recompile base` for relocatable modules (0 when the runtime can load
  the module where it was analysed, which it tries first). Constants derived from the module's
  own addresses (return addresses, jump-table cases) are adjusted by it; LO16-only relocations
  stay correct because the loader keeps the low 16 bits of the base.
* Calls outside the module (imports, function pointers) go through `g_api->callIndirect`.
* Unrecovered `jr` targets go to `g_api->jumpIndirect`, which runs native code or the interpreter.
  A `jr` that leaves the jumper's stack (`$sp` above `sp0`, or more than 1 MB below it) is a
  stack switch, not a jump within the frame: coroutine and task schedulers swap `$sp` and jump to
  the other context's saved `$ra` through another register (Daxter's does). `dispatch::Jump`
  makes it a non-local return, as for `jr ra`, so the host stack doesn't grow with every switch
  (`tests/guest/coroutines`).
* Words patched by variable imports run through `g_api->execWord` (read from memory at runtime).
* A module that writes `fcr31` (`ctc1 x, $31`) is compiled with `#pragma STDC FENV_ACCESS ON`
  for Clang. GCC ignores that pragma, so both Linux AOT build routes also use `-frounding-math`
  to prevent constant-folding under round-to-nearest when the guest changes rounding modes.
* DLLs carry the ABI version (`PSPC_AOT_ABI_VERSION`, now 2) and the semantics version (a hash of
  `src/allegrex`, `src/recomp/codegen.cpp`, and `tools/pspc_build.py`). The runtime rejects a
  different version, and the builder regenerates cached modules after semantics, codegen or
  compiler-policy changes. Rebuild the main tree before rebuilding AOT modules.

## Validation

* On Linux, with `PSPC_BUILD_TESTS=ON` (the default), build `pspc_aot_probe`, then run
  `ctest --test-dir <build-dir> -R '^aot_pipeline$' --output-on-failure`. This synthetic ELF
  test covers both build routes, guest round-up, native execution, ABI isolation and cache
  invalidation. It needs Python 3, CMake, Ninja and GCC or Clang, but no PSP compiler, firmware
  or full runtime (`PSPC_BUILD_RUNTIME=OFF` is supported). With the full runtime enabled,
  also build `pspc_aot_runtime_probe` and run
  `ctest --test-dir <build-dir> -R '^aot_runtime$' --output-on-failure` to check actual native
  loading, interpreter fallback and unload. These fixed-address fixtures do not establish
  retail AOT, relocatable PRX or firmware compatibility.
* `tools/run_autotests.py --aot` runs the pspautotests suite with AOT DLLs enabled; comparing it
  with the interpreter run is a differential test of the code generator.
* Interpreter misses inside AOT modules are written to `<aot dir>/<sha1>.hints`; the next
  `pspc_build.py` run adds them as strong entries.
* `tools/guest_tests.py` runs PSPC's own guest programs in the interpreter and as native code
  built fresh for the run (no hints), the way a player's first build is.
* `tools/smoke_games.py` checks that the games still reach their checkpoints
  (`tools/checkpoints.txt`) with the same frames, interpreter or native.

## Known limits / next steps

* Self-modifying or runtime-generated code, and modules loaded from memory buffers, run in the
  interpreter (correct but slower). A write-watch on text pages to invalidate native code after
  guest writes is not implemented yet.
* No register caching across calls: values live in `Ctx` (the compiler keeps them in registers
  within straight-line code thanks to `__restrict`).
