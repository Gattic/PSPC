// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest address -> host function table. Entries are recompiled functions (from AOT DLLs) and
// HLE thunks (import stubs). Anything not in the table runs in the interpreter.
#pragma once

#include "allegrex/ctx.h"

using HostFn = void (*)(Ctx *);

namespace dispatch {

void Init();
void Register(u32 addr, HostFn fn);
void UnregisterRange(u32 addr, u32 size);
HostFn Lookup(u32 addr);
u32 RegisteredCount();

// Call semantics: $ra already holds the return address. Runs native code if registered,
// otherwise interprets until the callee returns. Sets c->pc = target before a native call.
void Call(Ctx *c, u32 target);
// Tail jump from a function whose entry values of $ra/$sp were ra0/sp0.
void Jump(Ctx *c, u32 target, u32 ra0, u32 sp0);
// jr $ra with $ra != value at entry: start a non-local return to `target`.
void NonlocalReturn(Ctx *c, u32 target);

}  // namespace dispatch
