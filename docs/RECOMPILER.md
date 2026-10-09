# The Allegrex recompiler (pspc-recomp)

`pspc-recomp` turns one plain PSP module (ELF or PRX) into C++ that compiles into a Windows DLL.
`tools/pspc_build.py` drives it: decrypt -> recompile -> compile with clang-cl -> copy
`<sha1>.dll` into the AOT cache that `pspc.exe` searches (`<pspc dir>/cache/aot`).

```
python -I tools/pspc_build.py EBOOT.PBP
python -I tools/pspc_build.py some/module.prx --keep-src     # keep the generated C++
build\RelWithDebInfo\bin\pspc-recomp.exe module.prx --info  # analysis statistics only
```

## Pipeline

1. **Image** (`image.cpp`). The module is loaded into a flat buffer at its recompile base
   (`0x08804000` for PRX - where the runtime loads the first module - or the fixed vaddrs of an
   `ET_EXEC`). Relocations are applied and recorded: HI16 words, R_MIPS_32 data words, words
   patched by variable imports, and the import stub range.
2. **Analysis** (`analysis.cpp`).
   - *Strong entries*: module entry, exports, every `jal`/`bal` target, hints.
   - *Weak entries*: code addresses found in relocated data words, in `lui/addiu` pairs, and (for
     fixed executables) data words pointing at plausible function starts. These become functions
     too, so indirect calls usually land on native code.
   - Each function's *body* is everything reachable from its entry through fall-through,
     conditional branches, `j` within its range (the range ends at the next strong entry) and
     recovered jump tables. Bodies may overlap; shared code is compiled into each function.
   - *Jump tables*: `lw rs, lo(base)` where `base = (hi<<16) + index` is resolved backwards through
     `lui/addiu/addu`, bounded by a preceding `sltiu` and (for PRX) by relocation entries.
3. **Codegen** (`codegen.cpp`): one `void F_addr(Ctx *c)` per function; ~1 MB translation units;
   `module.h` declarations; `glue.cpp` with `pspc_aot_module()`; a CMakeLists.txt for the DLL.

## Generated code

```cpp
void F_088040AC(Ctx *PSPC_RESTRICT c) {
	const u32 ra0 = c->r[31];
	const u32 sp0 = c->r[29];
	sem_ADDIU(c, 0x27BDFFE0u);                       // addiu sp, sp, -32
	sem_LUI(c, 0x3C040880u); c->r[4] += g_delta;      // HI16-relocated lui
	c->r[31] = (0x088040C4u + g_delta);              // jal F_08805000
	sem_ADDU(c, 0x00052021u);                        //   (delay slot)
	F_08805000(c);
	if (PSPC_UNLIKELY(c->unwinding)) { if (c->pc == (0x088040C4u + g_delta)) c->unwinding = 0; else return; }
	{ const bool t_ = cond_BNE(c, 0x1440FFF8u);       // bne with delay slot
	  sem_ADDIU(c, 0x24840001u);
	  if (t_) { if (--c->budget < 0) g_api->yieldCheck(c); goto L_088040B0; } }
	{ const u32 t_ = c->r[31];                       // jr ra
	  sem_ADDIU(c, 0x27BD0020u);
	  if (t_ == ra0) return;
	  g_api->nonlocalReturn(c, t_); return; }
}
```

* `g_delta` is `load base - recompile base` for relocatable modules (0 when the runtime can load
  the module where it was analysed, which it tries first). Constants derived from the module's
  own addresses (return addresses, jump-table cases) are adjusted by it; LO16-only relocations
  stay correct because the loader keeps the low 16 bits of the base.
* Calls outside the module (imports, function pointers) go through `g_api->callIndirect`.
* Unrecovered `jr` targets go to `g_api->jumpIndirect`, which runs native code or the interpreter.
* Words patched by variable imports run through `g_api->execWord` (read from memory at runtime).

## Validation

* `cpu/cpu_alu` runs with 757 native functions and **0 interpreted instructions**; its output is
  byte-identical to the hardware recording.
* `tools/run_autotests.py --aot` runs the pspautotests suite with AOT DLLs enabled; comparing it
  with the interpreter run is a differential test of the code generator.

## Known limits / next steps

* Self-modifying or runtime-generated code, and modules loaded from memory buffers, run in the
  interpreter (correct but slower). A write-watch on text pages to invalidate native code after
  guest writes is not implemented yet.
* No register caching across calls: values live in `Ctx` (the compiler keeps them in registers
  within straight-line code thanks to `__restrict`).
* Weak entries duplicate code inside larger functions; an alternate-entry scheme would shrink DLLs.
* A runtime "coverage feedback" (logging interpreted call targets inside AOT modules into a hints
  file for the next build) is planned.
