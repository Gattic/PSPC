// SPDX-License-Identifier: GPL-2.0-or-later
//
// Interface between the runtime and ahead-of-time recompiled module DLLs.
//
// A module DLL contains two C++ entry points per guest function, compiled against sem.h: the
// body B_xxx(Ctx*, ra0, sp0), which runs the guest code as part of a frame that returns to ra0
// (sp0 is the stack pointer that frame started with), and the call entry F_xxx(Ctx*) =
// B_xxx(c, $ra, $sp). Calls use the call entry; jumps into a function (tail calls, or a jump to a
// label that also starts a function) use the body with the jumper's ra0/sp0, so the code knows
// where its frame really returns to. It exports pspc_aot_module(), which the runtime calls once after LoadLibrary. The DLL
// keeps its own g_membase / g_cop0 / g_intrEnabled globals and fills them from the API.
//
// Control-flow conventions used by generated code (see docs/RECOMPILER.md):
//   * jal inside the module      -> direct C call of the body, then an unwind check.
//   * j to a function entry      -> body with the current ra0/sp0, then return.
//   * jal/jalr to anything else  -> api->callIndirect(c, target).
//   * jr $ra                     -> return if $ra equals the value it had on entry,
//                                   otherwise api->nonlocalReturn(c, target) and return.
//   * jr to an unknown target    -> api->jumpIndirect(c, target, ra0, sp0) and return.
//   * after every call           -> if (c->unwinding) { if (c->pc == retAddr) c->unwinding = 0;
//                                   else return; }
//   * backward branch            -> if (--c->budget < 0) api->yieldCheck(c);
#pragma once

#include "allegrex/ctx.h"

#define PSPC_AOT_ABI_VERSION 2

struct PspcRuntimeApi {
	u32 abiVersion;
	u32 structSize;
	u8 *membase;
	u32 *cop0;
	u32 *intrEnabled;
	void (*callIndirect)(Ctx *c, u32 target);
	void (*jumpIndirect)(Ctx *c, u32 target, u32 ra0, u32 sp0);
	void (*nonlocalReturn)(Ctx *c, u32 target);
	void (*syscall)(Ctx *c, u32 insn, u32 pc);
	void (*breakInsn)(Ctx *c, u32 insn, u32 pc);
	void (*yieldCheck)(Ctx *c);
	void (*invalidInsn)(Ctx *c, u32 insn, u32 pc);
	// Continue execution in the interpreter at pc until the current guest function returns.
	void (*interpretFrom)(Ctx *c, u32 pc, u32 ra0, u32 sp0);
	// Execute the (non-control-flow) instruction currently stored at pc. Used for words that are
	// patched at load time and therefore can't be compiled in (variable-import relocations).
	void (*execWord)(Ctx *c, u32 pc);
};

typedef void (*AotFn)(Ctx *);
typedef void (*AotBodyFn)(Ctx *, u32 ra0, u32 sp0);

struct AotFunctionEntry {
	u32 offset;      // Guest address minus recompileBase.
	AotFn fn;        // call entry
	AotBodyFn body;  // jump entry
};

struct AotModuleDesc {
	u32 abiVersion;
	u32 semVersion;
	const char *moduleName;
	u8 sha1[20];        // SHA-1 of the decrypted, decompressed ELF image.
	u32 recompileBase;  // Load address the code was analysed at.
	u32 relocatable;    // 0 for fixed-address executables.
	u32 numFunctions;
	const AotFunctionEntry *functions;
	u32 *delta;         // Written by the runtime: actual load base - recompileBase.
};

typedef const AotModuleDesc *(*PspcAotModuleFn)(const PspcRuntimeApi *api);
#define PSPC_AOT_ENTRY_NAME "pspc_aot_module"
