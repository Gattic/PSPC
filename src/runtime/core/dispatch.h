// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest address -> host function table. Entries are recompiled functions (from AOT DLLs) and
// HLE thunks (import stubs). Anything not in the table runs in the interpreter.
#pragma once

#include "allegrex/ctx.h"

using HostFn = void (*)(Ctx *);
// Body of a recompiled function, run as part of a frame that returns to ra0 (see aot_abi.h).
using BodyFn = void (*)(Ctx *, u32 ra0, u32 sp0);

namespace dispatch {

void Init();
// A host function with call semantics only (HLE thunks); clears any body entry at addr.
void Register(u32 addr, HostFn fn);
// A recompiled function: call entry plus body entry for jumps.
void RegisterNative(u32 addr, HostFn fn, BodyFn body);
void UnregisterRange(u32 addr, u32 size);
HostFn Lookup(u32 addr);
BodyFn LookupBody(u32 addr);
u32 RegisteredCount();

// Call semantics: $ra already holds the return address. Runs native code if registered,
// otherwise interprets until the callee returns. Sets c->pc = target before a native call.
void Call(Ctx *c, u32 target);
// Tail jump from a function whose entry values of $ra/$sp were ra0/sp0.
void Jump(Ctx *c, u32 target, u32 ra0, u32 sp0);
// Called (once per address) when native code has to fall back to the interpreter at `target`.
void SetMissHook(void (*hook)(u32 target));

// jr $ra with $ra != value at entry: start a non-local return to `target`.
void NonlocalReturn(Ctx *c, u32 target);

}  // namespace dispatch
