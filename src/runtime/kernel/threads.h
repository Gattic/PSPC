// SPDX-License-Identifier: GPL-2.0-or-later
// Thread creation/start shared by ThreadManForUser and the module loader.
#pragma once

#include <functional>

#include "runtime/kernel/thread.h"

namespace kernel {

// Returns the new thread UID, or a negative PSP error. Validates like sceKernelCreateThread
// (allowKernel: the caller may create kernel threads). The stack fill cost is charged by the
// caller (see sceKernelCreateThread); *fillCycles receives it.
class BlockAllocator;
// stackAlloc picks the partition for the stack; by default it follows the attr.
SceUID CreateThread(const char *name, u32 entry, s32 prio, s32 stackSize, u32 attr, u32 gp, SceUID moduleId,
                    bool allowKernel = true, s64 *fillCycles = nullptr, BlockAllocator *stackAlloc = nullptr);
// Starts a dormant thread. argPtr/argSize are copied onto the new thread's stack. A thread
// better than the caller gets the CPU at once (unless dispatch is suspended).
int StartThread(PspThread *t, u32 argSize, u32 argPtr);
// Free a thread's stack block(s).
void FreeThreadStack(PspThread *t);

// Called whenever a thread stops (exit, terminate, delete), before waiters are released.
using ThreadEndListener = std::function<void(PspThread *)>;
void AddThreadEndListener(ThreadEndListener fn);
void NotifyThreadEnd(PspThread *t);

}  // namespace kernel
