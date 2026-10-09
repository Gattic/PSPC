// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "allegrex/ctx.h"

namespace interp {

// Execute guest code from `pc` as the body of a function entered with $ra == ra0 and
// $sp == sp0. Returns when control jumps to ra0 with $sp >= sp0, or when a non-local return
// is propagating (c->unwinding set, target in c->pc).
void Run(Ctx *c, u32 pc, u32 ra0, u32 sp0);

// Execute exactly one non-control-flow instruction (used by AOT code for words patched at
// load time, e.g. variable-import relocations).
void ExecOne(Ctx *c, u32 pc, u32 word);

u64 InstructionCount();

}  // namespace interp
