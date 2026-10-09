// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <initializer_list>

#include "allegrex/aot_abi.h"
#include "allegrex/ctx.h"

// Shared CPU state (defined in cpu.cpp; AOT DLLs get pointers to it).
extern u32 *g_cop0;
extern u32 *g_intrEnabled;

namespace cpu {

// Return address used for host->guest calls. The word there is a syscall that traps strays.
constexpr u32 MAGIC_RETURN_ADDR = 0x08000000;
constexpr u32 SYSCALL_MAGIC_RETURN = 0xFFFFF;  // 20-bit code reserved for the magic stub

void Init();

Ctx *CurrentCtx();
void SetCurrentCtx(Ctx *c);

bool StrictMemory();
void SetStrictMemory(bool strict);

// Runs guest function `func` on `c` with $ra = MAGIC_RETURN_ADDR until it returns. Handles a
// non-local return that escapes all frames by continuing in the interpreter.
void RunFunction(Ctx *c, u32 func);

// Host->guest call that preserves every register of `c` except the result. Used for callbacks
// and interrupt handlers that run on an existing thread context. Returns $v0.
u32 CallGuest(Ctx *c, u32 func, std::initializer_list<u32> args);

void YieldCheck(Ctx *c);
void Break(Ctx *c, u32 insn, u32 pc);
void InvalidInstruction(Ctx *c, u32 insn, u32 pc);

const PspcRuntimeApi *AotApi();

}  // namespace cpu
