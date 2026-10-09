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
	THREADSTATUS_WAITSUSPEND = THREADSTATUS_WAIT | THREADSTATUS_SUSPEND,
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

// Optional per-wait behaviour for waits on kernel objects (see sched::Wait).
struct WaitHooks {
	// A callback-enabled wait is paused while its thread runs callbacks: take the thread off the
	// object's wait list (so a signal meanwhile doesn't go to it).
	std::function<void()> pause;
	// The callbacks returned and the wait wasn't ended meanwhile: try to satisfy it (calling
	// sched::Wake with the result and returning true), or put the thread back on the wait list
	// and return false.
	std::function<bool()> resume;
	// The wait timed out; runs just before the thread is woken with the timeout result.
	std::function<void()> timeout;
};

struct PspThread : KObject {
	static constexpr KType kType = KType::Thread;
	KType Type() const override { return kType; }

	alignas(64) Ctx ctx;

	// SceKernelThreadInfo-equivalent fields.
	u32 attr = 0;
	u32 status = THREADSTATUS_DORMANT;
	u32 entry = 0;
	u32 stackAddr = 0;  // lowest address of the current stack block
	u32 stackSize = 0;
	u32 gpReg = 0;
	s32 initPriority = 0;
	s32 currentPriority = 0;
	WaitType waitType = WAITTYPE_NONE;
	SceUID waitId = 0;
	s32 wakeupCount = 0;
	s32 exitStatus = 0;
	u64 runUs = 0;  // time spent running (reported as runClocks)
	u32 intrPreemptCount = 0, threadPreemptCount = 0, releaseCount = 0;
	SceUID moduleId = 0;

	// Wait bookkeeping (the innermost wait; waits nested in callbacks save the outer one).
	u32 waitSeq = 0;           // incremented per wait; matches timeout events to their wait
	bool inReadyQueue = false;
	bool waitDone = false;
	s32 waitResult = 0;
	u32 waitTimeoutPtr = 0;    // guest u32* that receives the time left
	u64 waitDeadline = 0;      // absolute us when the timeout fires, 0 = none
	u32 waitLatencyUs = 0;     // how far past the deadline the timeout fires (not part of time left)
	s32 waitTimeoutResult = 0; // what a timeout wakes the wait with
	u64 timeoutEventId = 0;
	u32 waitValue[4] = {};     // per-wait-type data (e.g. event flag pattern)
	bool waitCallbacks = false;
	bool waitPaused = false;   // a CB wait paused because callbacks are pending
	bool cbQueued = false;     // queued (still WAIT) to run its callbacks
	bool hasWaited = false;    // has waited since it was started
	WaitHooks waitHooks;
	std::function<void()> timeoutHook;  // legacy: same as waitHooks.timeout, for hook-less callers

	// Callbacks owned by this thread, and how many are running nested on it.
	std::vector<SceUID> callbacks;
	int callbackDepth = 0;

	// Threads waiting for this one to end.
	std::vector<SceUID> endWaiters;

	// Stacks pushed by sceKernelExtendThreadStack (start, size of each outer stack).
	std::vector<std::pair<u32, u32>> pushedStacks;

	// Busy syscall (fast-time mode): cycles of CPU time it still has to burn.
	s64 busyCycles = 0;
	bool busy = false;

	// Host execution.
	void *fiber = nullptr;
	bool fiberStarted = false;
	u32 threadReturnStub = 0;  // guest address of the "thread returned" stub on the stack

	bool IsWaiting() const { return (status & THREADSTATUS_WAIT) != 0; }
	bool IsDormant() const { return (status & THREADSTATUS_DORMANT) != 0; }
	bool IsSuspended() const { return (status & THREADSTATUS_SUSPEND) != 0; }
	bool IsWaitingFor(WaitType type, SceUID id) const {
		return IsWaiting() && !waitDone && waitType == type && waitId == id;
	}
};

}  // namespace kernel
