// SPDX-License-Identifier: GPL-2.0-or-later
// Stack layout follows PPSSPP's PSPThread::FillStack/__KernelStartThread (Portions derived from
// PPSSPP, https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include "runtime/kernel/threads.h"

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

namespace kernel {

// syscall used for the "thread returned" stub on each thread's stack.
extern u32 g_threadReturnSyscall;

SceUID CreateThread(const char *name, u32 entry, s32 prio, s32 stackSize, u32 attr, u32 gp, SceUID moduleId) {
	if (!name) return (s32)SCE_KERNEL_ERROR_ERROR;
	if ((u32)stackSize < 0x200) return (s32)SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE;
	if (prio < 0x08 || prio > 0x77) return (s32)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	if (entry && !mem::IsValidAddress(entry)) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;

	if ((attr & PSP_THREAD_ATTR_KERNEL) == 0) attr |= PSP_THREAD_ATTR_USER;

	auto *t = new PspThread();
	t->name = name;
	t->entry = entry;
	t->attr = attr | 0xFF;
	t->initPriority = t->currentPriority = prio;
	t->status = THREADSTATUS_DORMANT;
	t->exitStatus = (s32)SCE_KERNEL_ERROR_DORMANT;
	t->gpReg = gp;
	t->moduleId = moduleId;

	u32 size = AlignUp((u32)stackSize, 0x100);
	BlockAllocator &alloc = (attr & PSP_THREAD_ATTR_KERNEL) && !(attr & PSP_THREAD_ATTR_USER) ? g_kernelMem : g_userMem;
	u32 stack = alloc.Alloc(size, (attr & PSP_THREAD_ATTR_LOW_STACK) == 0, ("stack/" + t->name).c_str());
	if (!stack) {
		delete t;
		return (s32)SCE_KERNEL_ERROR_NO_MEMORY;
	}
	t->stackAddr = stack;
	t->stackSize = size;
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

int StartThread(PspThread *t, u32 argSize, u32 argPtr) {
	PspThread *cur = sched::Current();
	void *thread = t->ctx.thread;
	memset(&t->ctx, 0, sizeof(Ctx));
	t->ctx.thread = thread;
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
	if (cur && t->currentPriority < cur->currentPriority) {
		// Matches firmware: a thread better than the caller restarts at its initial priority.
	}
	t->currentPriority = t->initPriority;
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	t->wakeupCount = 0;
	t->exitStatus = (s32)SCE_KERNEL_ERROR_NOT_DORMANT;
	t->ctx.r[28] = t->gpReg;
	FillStack(t);

	u32 &sp = t->ctx.r[29];
	if (argPtr && argSize > 0) {
		sp -= (argSize + 0xF) & ~0xFu;
		t->ctx.r[4] = argSize;
		t->ctx.r[5] = sp;
		mem::Memcpy(sp, argPtr, argSize);
	} else {
		t->ctx.r[4] = 0;
		t->ctx.r[5] = 0;
	}
	// 64 bytes the firmware leaves below the args; its bottom holds the return stub.
	sp -= 64;
	GWrite32(sp, 0x03E00008);                                   // jr ra
	GWrite32(sp + 4, (g_threadReturnSyscall << 6) | 0x0C);      // syscall _sceKernelExitThread
	GWrite32(sp + 8, 0x1000FFFF);                               // b -1
	GWrite32(sp + 12, 0);
	t->threadReturnStub = sp;
	t->ctx.r[31] = sp;
	t->ctx.r[30] = sp;

	sched::LaunchThread(t);
	if (cur && cur->currentPriority > t->currentPriority) sched::Reschedule();
	return 0;
}

void FreeThreadStack(PspThread *t) {
	if (!t->stackAddr) return;
	if ((t->attr & PSP_THREAD_ATTR_CLEAR_STACK) != 0) mem::Memset(t->stackAddr, 0, t->stackSize);
	if (BlockAllocator *a = AllocatorForAddress(t->stackAddr)) a->Free(t->stackAddr);
	t->stackAddr = 0;
}

}  // namespace kernel
