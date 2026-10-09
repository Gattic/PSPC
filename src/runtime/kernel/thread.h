// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <deque>
#include <functional>
#include <vector>

#include "allegrex/ctx.h"
#include "runtime/kernel/kobject.h"

namespace kernel {

enum ThreadStatus : u32 {
	THREADSTATUS_RUNNING = 1,
	THREADSTATUS_READY = 2,
	THREADSTATUS_WAIT = 4,
	THREADSTATUS_SUSPEND = 8,
	THREADSTATUS_DORMANT = 16,
	THREADSTATUS_DEAD = 32,
};

enum WaitType : u32 {
	WAITTYPE_NONE = 0,
	WAITTYPE_SLEEP = 1,
	WAITTYPE_DELAY = 2,
	WAITTYPE_SEMA = 3,
	WAITTYPE_EVENTFLAG = 4,
	WAITTYPE_MBX = 5,
	WAITTYPE_VPL = 6,
	WAITTYPE_FPL = 7,
	WAITTYPE_MSGPIPE = 8,
	WAITTYPE_THREADEND = 9,
	WAITTYPE_AUDIOCHANNEL = 10,
	WAITTYPE_UMD = 11,
	WAITTYPE_VBLANK = 12,
	WAITTYPE_MUTEX = 13,
	WAITTYPE_LWMUTEX = 14,
	WAITTYPE_CTRL = 15,
	WAITTYPE_IO = 16,
	WAITTYPE_GEDRAWSYNC = 17,
	WAITTYPE_GELISTSYNC = 18,
	WAITTYPE_MODULE = 19,
	WAITTYPE_HLEDELAY = 20,
	WAITTYPE_TLSPL = 21,
	WAITTYPE_VMEM = 22,
	WAITTYPE_ASYNCIO = 23,
	WAITTYPE_MICINPUT = 24,
	WAITTYPE_NET = 25,
	WAITTYPE_HLE = 26,  // generic native wait (completion signalled by host code)
};

enum ThreadAttr : u32 {
	PSP_THREAD_ATTR_KERNEL = 0x00001000,
	PSP_THREAD_ATTR_VFPU = 0x00004000,
	PSP_THREAD_ATTR_SCRATCH_SRAM = 0x00008000,
	PSP_THREAD_ATTR_NO_FILLSTACK = 0x00100000,
	PSP_THREAD_ATTR_CLEAR_STACK = 0x00200000,
	PSP_THREAD_ATTR_LOW_STACK = 0x00400000,
	PSP_THREAD_ATTR_USER = 0x80000000,
	PSP_THREAD_ATTR_USBWLAN = 0xa0000000,
	PSP_THREAD_ATTR_VSH = 0xc0000000,
};

struct PspThread : KObject {
	static constexpr KType kType = KType::Thread;
	KType Type() const override { return kType; }

	alignas(64) Ctx ctx;

	// SceKernelThreadInfo-equivalent fields.
	u32 attr = 0;
	u32 status = THREADSTATUS_DORMANT;
	u32 entry = 0;
	u32 stackAddr = 0;  // lowest address of the stack block
	u32 stackSize = 0;
	u32 gpReg = 0;
	s32 initPriority = 0;
	s32 currentPriority = 0;
	WaitType waitType = WAITTYPE_NONE;
	SceUID waitId = 0;
	s32 wakeupCount = 0;
	s32 exitStatus = 0;
	u32 runClocksLo = 0, runClocksHi = 0;
	u32 intrPreemptCount = 0, threadPreemptCount = 0, releaseCount = 0;
	SceUID moduleId = 0;

	// Wait bookkeeping.
	u32 waitSeq = 0;           // incremented per wait; matches timeout events to their wait
	bool inReadyQueue = false;
	std::function<void()> timeoutHook;  // run when the current wait times out
	bool waitDone = false;
	s32 waitResult = 0;
	u32 waitTimeoutPtr = 0;    // guest u32* with remaining microseconds
	u64 waitDeadline = 0;      // absolute us, 0 = none
	u64 timeoutEventId = 0;
	u32 waitValue[4] = {};     // per-wait-type data (e.g. event flag pattern)
	bool waitCallbacks = false;

	// Callbacks owned by this thread, and whether one is running.
	std::vector<SceUID> callbacks;
	bool inCallback = false;
	bool callbacksPendingWake = false;

	// Threads waiting for this one to end.
	std::vector<SceUID> endWaiters;

	// Host execution.
	void *fiber = nullptr;
	bool fiberStarted = false;
	u32 threadReturnStub = 0;  // guest address of the "thread returned" stub on the stack

	bool IsWaiting() const { return (status & THREADSTATUS_WAIT) != 0; }
	bool IsDormant() const { return (status & THREADSTATUS_DORMANT) != 0; }
	bool IsSuspended() const { return (status & THREADSTATUS_SUSPEND) != 0; }
};

}  // namespace kernel
