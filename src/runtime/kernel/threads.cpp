// SPDX-License-Identifier: GPL-2.0-or-later
// Thread creation, stack layout and start follow PPSSPP's __KernelCreateThread,
// PSPThread::FillStack and __KernelStartThread (Portions derived from PPSSPP,
// https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include "runtime/kernel/threads.h"

#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

extern u32 *g_intrEnabled;

namespace kernel {

// syscall used for the "thread returned" stub on each thread's stack.
extern u32 g_threadReturnSyscall;

namespace {
constexpr u32 kAttrUserMask = 0xF8F060FF;
constexpr u32 kAttrUserErase = 0x78800000;
std::vector<ThreadEndListener> g_endListeners;
}  // namespace

void AddThreadEndListener(ThreadEndListener fn) { g_endListeners.push_back(std::move(fn)); }

void NotifyThreadEnd(PspThread *t) {
	for (auto &fn : g_endListeners) fn(t);
}

SceUID CreateThread(const char *name, u32 entry, s32 prio, s32 stackSize, u32 attr, u32 gp, SceUID moduleId,
                    bool allowKernel, s64 *fillCycles, BlockAllocator *stackAlloc) {
	if (fillCycles) *fillCycles = 0;
	if (!name) return (s32)SCE_KERNEL_ERROR_ERROR;
	for (int i = 0; i < 4 && name[i]; i++) {
		if ((u8)name[i] >= 0xF0) {
			name = "badThreadName";
			break;
		}
	}
	if ((u32)stackSize < 0x200) return (s32)SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE;
	if (prio < 0x08 || prio > 0x77) return (s32)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	if (entry && !mem::IsValidAddress(entry)) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if ((attr & ~kAttrUserMask) != 0 && !allowKernel) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	attr &= ~kAttrUserErase;
	if ((attr & PSP_THREAD_ATTR_KERNEL) == 0) {
		if (allowKernel && (attr & PSP_THREAD_ATTR_USER) == 0) attr |= PSP_THREAD_ATTR_KERNEL;
		else attr |= PSP_THREAD_ATTR_USER;
	}

	auto *t = new PspThread();
	t->name = name;
	if (t->name.size() > 31) t->name.resize(31);
	t->entry = entry;
	t->attr = attr | 0xFF;
	t->initPriority = t->currentPriority = prio;
	t->status = THREADSTATUS_DORMANT;
	t->exitStatus = (s32)SCE_KERNEL_ERROR_DORMANT;
	if (moduleId) {
		if (PspModule *m = Get<PspModule>(moduleId); m && m->gp) gp = m->gp;
	}
	t->gpReg = gp;
	t->moduleId = moduleId;

	u32 size = AlignUp((u32)stackSize, 0x100);
	BlockAllocator &alloc = stackAlloc ? *stackAlloc : (attr & PSP_THREAD_ATTR_KERNEL) ? g_kernelMem : g_userMem;
	u32 stack = alloc.Alloc(size, (attr & PSP_THREAD_ATTR_LOW_STACK) == 0, ("stack/" + t->name).c_str());
	if (!stack) {
		delete t;
		return (s32)SCE_KERNEL_ERROR_NO_MEMORY;
	}
	t->stackAddr = stack;
	t->stackSize = size;
	// Filling the stack with 0xFF takes about a cycle per byte.
	if (fillCycles && (attr & PSP_THREAD_ATTR_NO_FILLSTACK) == 0) *fillCycles = (s64)size - (s64)size / 25;
	return Register(t);
}

static void FillStack(PspThread *t) {
	if ((t->attr & PSP_THREAD_ATTR_NO_FILLSTACK) == 0) mem::Memset(t->stackAddr, 0xFF, t->stackSize);
	u32 sp = t->stackAddr + t->stackSize;
	// 256-byte k0 area at the top of the stack.
	sp -= 256;
	u32 k0 = sp;
	mem::Memset(k0, 0, 0x100);
	GWrite32(k0 + 0xC0, (u32)t->uid);
	GWrite32(k0 + 0xC8, t->stackAddr);
	GWrite32(k0 + 0xF8, 0xFFFFFFFF);
	GWrite32(k0 + 0xFC, 0xFFFFFFFF);
	GWrite32(t->stackAddr, (u32)t->uid);
	t->ctx.r[29] = sp;
	t->ctx.r[26] = k0;
}

static void ResetContext(PspThread *t) {
	void *thread = t->ctx.thread;
	memset(&t->ctx, 0, sizeof(Ctx));
	t->ctx.thread = thread;
	for (int i = 1; i < 32; i++) t->ctx.r[i] = 0xDEADBEEF;
	for (int i = 0; i < 32; i++) t->ctx.fi[i] = 0x7F800001;
	for (int i = 0; i < 128; i++) t->ctx.vi[i] = 0x7F800001;
	t->ctx.hi = t->ctx.lo = 0xDEADBEEF;
	t->ctx.fcr31 = 0x00000E00;
	t->ctx.vfpuCtrl[VFPU_CTRL_SPREFIX] = 0xE4;
	t->ctx.vfpuCtrl[VFPU_CTRL_TPREFIX] = 0xE4;
	t->ctx.vfpuCtrl[VFPU_CTRL_DPREFIX] = 0;
	t->ctx.vfpuCtrl[VFPU_CTRL_CC] = 0x3F;
	t->ctx.vfpuCtrl[VFPU_CTRL_INF4] = 0;
	t->ctx.vfpuCtrl[VFPU_CTRL_REV] = 0x7772CEAB;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0] = 0x3F800001;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 1] = 0x3F800002;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 2] = 0x3F800004;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 3] = 0x3F800008;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 4] = 0x3F800000;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 5] = 0x3F800000;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 6] = 0x3F800000;
	t->ctx.vfpuCtrl[VFPU_CTRL_RCX0 + 7] = 0x3F800000;
	t->ctx.pc = t->entry;
}

int StartThread(PspThread *t, u32 argSize, u32 argPtr) {
	PspThread *cur = sched::Current();
	ResetContext(t);
	// If the thread would be better than the caller, it restarts at its initial priority.
	s32 lowest = cur ? cur->currentPriority : 0;
	if (t->currentPriority < lowest) t->currentPriority = t->initPriority;
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	memset(t->waitValue, 0, sizeof(t->waitValue));
	t->exitStatus = (s32)SCE_KERNEL_ERROR_NOT_DORMANT;
	t->ctx.r[28] = t->gpReg;
	FillStack(t);

	u32 &sp = t->ctx.r[29];
	if (argPtr && argSize > 0) {
		sp -= (argSize + 0xF) & ~0xFu;
		t->ctx.r[4] = argSize;
		t->ctx.r[5] = sp;
		if (mem::IsValidAddress(argPtr)) mem::Memcpy(sp, argPtr, argSize);
	} else {
		t->ctx.r[4] = 0;
		t->ctx.r[5] = 0;
	}
	// 64 bytes the firmware leaves below the args; its bottom holds the return stub.
	sp -= 64;
	GWrite32(sp, 0x03E00008);                               // jr ra
	GWrite32(sp + 4, (g_threadReturnSyscall << 6) | 0x0C);  // syscall _sceKernelExitThread
	GWrite32(sp + 8, 0x1000FFFF);                           // b -1
	GWrite32(sp + 12, 0);
	t->threadReturnStub = sp;
	t->ctx.r[31] = sp;
	t->ctx.r[30] = sp;

	sched::LaunchThread(t);
	if (cur && cur->currentPriority > t->currentPriority) {
		// The firmware hands the CPU straight to the new thread, even when a better thread is
		// ready but hasn't been dispatched (pspautotests threads/threads/termsuspended).
		if (*g_intrEnabled) {
			// About 35us from the call to the new thread's entry (threads/scheduling/handoff).
			sched::ChargeCycles(3100);
			sched::HandOffTo(t);
		}
	} else if (cur) {
		// Starting a thread that isn't better resumes dispatch.
		sched::SetDispatchEnabled(true);
	}
	return 0;
}

void FreeThreadStack(PspThread *t) {
	for (auto &s : t->pushedStacks) {
		if (BlockAllocator *a = AllocatorForAddress(t->stackAddr)) a->Free(t->stackAddr);
		t->stackAddr = s.first;
		t->stackSize = s.second;
	}
	t->pushedStacks.clear();
	if (!t->stackAddr) return;
	if ((t->attr & PSP_THREAD_ATTR_CLEAR_STACK) != 0) mem::Memset(t->stackAddr, 0, t->stackSize);
	if (BlockAllocator *a = AllocatorForAddress(t->stackAddr)) a->Free(t->stackAddr);
	t->stackAddr = 0;
}

}  // namespace kernel
