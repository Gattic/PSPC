// SPDX-License-Identifier: GPL-2.0-or-later
// Thread creation/start shared by ThreadManForUser and the module loader.
#pragma once

#include "runtime/kernel/thread.h"

namespace kernel {

// Returns the new thread UID, or a negative PSP error.
SceUID CreateThread(const char *name, u32 entry, s32 prio, s32 stackSize, u32 attr, u32 gp, SceUID moduleId);
// Starts a dormant thread. argPtr/argSize are copied onto the new thread's stack.
int StartThread(PspThread *t, u32 argSize, u32 argPtr);
// Free a thread's stack block.
void FreeThreadStack(PspThread *t);

}  // namespace kernel
