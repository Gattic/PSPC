// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser: threads, callbacks, delays, dispatch control, system time, id lists and
// thread event handlers. Synchronisation objects, alarms/VTimers, message boxes/pipes and memory
// pools live in kernel_*.cpp and register into the same library.
//
// Behaviour (argument validation order, error codes, wake order, call costs) follows PPSSPP's
// sceKernelThread.cpp / sceKernelTime.cpp, which encode pspautotests results recorded on hardware.
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later.
#include <algorithm>
#include <map>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace kernel {
u32 g_threadReturnSyscall = 0;

bool CallerIsKernelMode() {
	PspThread *cur = sched::Current();
	if (cur && (cur->attr & PSP_THREAD_ATTR_KERNEL)) return true;
	if (cur) {
		if (PspModule *m = Get<PspModule>(cur->moduleId)) return m->kernelMode;
	}
	return false;
}
}  // namespace kernel

namespace {

constexpr u32 NID_THREAD_RETURN = 0xAD58E3D2;  // runtime-internal

PspThread *CurThread() { return sched::Current(); }

PspThread *ThreadOrCurrent(SceUID id) {
	if (id == 0) return CurThread();
	return Get<PspThread>(id);
}

bool IsStopped(PspThread *t) { return (t->status & (THREADSTATUS_DORMANT | THREADSTATUS_DEAD)) != 0; }

// ---------------------------------------------------------------- thread event handlers

enum ThreadEventType : u32 {
	THREADEVENT_CREATE = 1,
	THREADEVENT_START = 2,
	THREADEVENT_EXIT = 4,
	THREADEVENT_DELETE = 8,
	THREADEVENT_SUPPORTED = 15,
};
constexpr SceUID SCE_TE_THREADID_ALL_USER = (SceUID)0xFFFFFFF0;

struct ThreadEventHandler : KObject {
	static constexpr KType kType = KType::ThreadEventHandler;
	KType Type() const override { return kType; }
	SceUID threadId = 0;
	u32 mask = 0, handler = 0, common = 0;
};

std::map<SceUID, std::vector<SceUID>> g_threadEventHandlers;

// Runs the matching handlers as calls on the current thread. Returns true if any ran.
bool TriggerThreadEvent(PspThread *subject, ThreadEventType type) {
	if (g_threadEventHandlers.empty()) return false;
	PspThread *cur = CurThread();
	if (!cur || sched::InInterrupt()) return false;
	bool ran = false;
	auto runList = [&](SceUID key) {
		auto it = g_threadEventHandlers.find(key);
		if (it == g_threadEventHandlers.end()) return;
		std::vector<SceUID> ids = it->second;
		for (SceUID id : ids) {
			ThreadEventHandler *h = Get<ThreadEventHandler>(id);
			if (!h || (h->mask & type) == 0) continue;
			cpu::CallGuest(&cur->ctx, h->handler, {(u32)type, (u32)subject->uid, h->common});
			ran = true;
		}
	};
	runList(subject->uid);
	runList(SCE_TE_THREADID_ALL_USER);
	return ran;
}

SceUID sceKernelRegisterThreadEventHandler(const char *name, SceUID threadId, u32 mask, u32 handler, u32 common) {
	if (!name) return (SceUID)SCE_KERNEL_ERROR_ERROR;
	if (threadId == 0 && mask != THREADEVENT_EXIT) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (threadId != SCE_TE_THREADID_ALL_USER && !Get<PspThread>(threadId)) return (SceUID)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if ((mask & ~THREADEVENT_SUPPORTED) != 0) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_MASK;
	auto *h = new ThreadEventHandler();
	h->name = TruncName(name);
	h->threadId = threadId;
	h->mask = mask;
	h->handler = handler;
	h->common = common;
	SceUID id = Register(h);
	g_threadEventHandlers[threadId].push_back(id);
	return id;
}

int sceKernelReleaseThreadEventHandler(SceUID id) {
	ThreadEventHandler *h = Get<ThreadEventHandler>(id);
	if (!h) return (int)SCE_KERNEL_ERROR_UNKNOWN_TEID;
	auto &list = g_threadEventHandlers[h->threadId];
	list.erase(std::remove(list.begin(), list.end(), id), list.end());
	if (list.empty()) g_threadEventHandlers.erase(h->threadId);
	Destroy(id);
	return 0;
}

int sceKernelReferThreadEventHandlerStatus(SceUID id, u32 infoPtr) {
	ThreadEventHandler *h = Get<ThreadEventHandler>(id);
	if (!h) return (int)SCE_KERNEL_ERROR_UNKNOWN_TEID;
	struct {
		u32 size;
		char name[32];
		SceUID threadId;
		u32 mask, handler, common;
	} info{};
	info.size = sizeof(info);
	CopyName(info.name, h->name);
	info.threadId = h->threadId;
	info.mask = h->mask;
	info.handler = h->handler;
	info.common = h->common;
	if (mem::IsValidRange(infoPtr, sizeof(info)) && GRead32(infoPtr) != 0) mem::WriteBytes(infoPtr, &info, sizeof(info));
	return 0;
}

// ---------------------------------------------------------------- threads

int sceKernelCreateThread(const char *name, u32 entry, u32 prio, s32 stackSize, u32 attr, u32 optionAddr) {
	(void)optionAddr;
	PspThread *cur = CurThread();
	u32 gp = cur ? cur->ctx.r[28] : 0;
	SceUID mod = cur ? cur->moduleId : 0;
	s64 fill = 0;
	SceUID id = CreateThread(name, entry, (s32)prio, stackSize, attr, gp, mod, CallerIsKernelMode(), &fill);
	if (id < 0) return id;
	// Creating a thread resumes dispatch.
	sched::SetDispatchEnabled(true);
	// About 165us, plus filling the stack at a little under a cycle per byte (a busy wait that
	// better threads can preempt): threads/scheduling/costs and preemptsyscall.
	sched::ChargeCycles(37000);
	sched::Reschedule();
	PspThread *t = Get<PspThread>(id);
	if (t && TriggerThreadEvent(t, THREADEVENT_CREATE)) {
		sched::ChargeCycles(fill);
	} else {
		sched::BusyDelayCycles(fill);
	}
	return id;
}

int sceKernelStartThread(SceUID id, s32 argSize, u32 argPtr) {
	if (id == 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	if (argSize < 0 || (argPtr & 0x80000000)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->status != THREADSTATUS_DORMANT) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	// 10-25us when the new thread doesn't get the CPU (threads/scheduling/costs).
	sched::ChargeCycles(3800);
	int r = StartThread(t, (u32)argSize, argPtr);
	if (r == 0) TriggerThreadEvent(t, THREADEVENT_START);
	return r;
}

[[noreturn]] void ExitThreadCommon(s32 status, bool deleteAfter) {
	PspThread *t = CurThread();
	TriggerThreadEvent(t, THREADEVENT_EXIT);
	if (deleteAfter) {
		DeleteThreadCallbacks(t);
		FreeThreadStack(t);
	}
	sched::ExitCurrentThread(status, deleteAfter);
}

void _sceKernelExitThread(s32 status) {
	if (!CurThread()) return;
	ExitThreadCommon(status, false);
}

int sceKernelExitThread(s32 status) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled() && CompiledSdkVersion() >= 0x03080000) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (status < 0) status = (s32)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	ExitThreadCommon(status, false);
}

int sceKernelExitDeleteThread(s32 status) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled() && CompiledSdkVersion() >= 0x03080000) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	ExitThreadCommon(status, true);
}

// Stops (if needed) and frees a thread that isn't the current one.
void DestroyThread(PspThread *t) {
	if (!IsStopped(t)) sched::TerminateThread(t, (s32)SCE_KERNEL_ERROR_THREAD_TERMINATED);
	t->status = THREADSTATUS_DEAD;
	DeleteThreadCallbacks(t);
	FreeThreadStack(t);
	TriggerThreadEvent(t, THREADEVENT_DELETE);
	sched::DeleteThread(t);
}

int sceKernelDeleteThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!IsStopped(t)) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	// 50-100us whatever the stack size (threads/scheduling/costs).
	sched::ChargeCycles(15000);
	DestroyThread(t);
	return 0;
}

// Finding the thread, and then terminating it. checkpoint() terminates and restarts a thread, at
// least 25us in all even when the thread had already ended (threads/scheduling/costs).
constexpr s64 kTerminateLookupCycles = 700;
constexpr s64 kTerminateCycles = 2100;

int sceKernelTerminateThread(SceUID id) {
	if (sched::InInterrupt() && CompiledSdkVersion() >= 0x03080000) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled() && CompiledSdkVersion() >= 0x03080000) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	sched::ChargeCycles(kTerminateLookupCycles);
	if (IsStopped(t)) return (int)SCE_KERNEL_ERROR_DORMANT;
	sched::ChargeCycles(kTerminateCycles);
	// Threads this wakes are made ready, but nothing is dispatched until the next dispatch point
	// (threads/threads/termsuspended).
	sched::TerminateThread(t, (s32)SCE_KERNEL_ERROR_THREAD_TERMINATED);
	TriggerThreadEvent(t, THREADEVENT_EXIT);
	return 0;
}

int sceKernelTerminateDeleteThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	if (!sched::DispatchEnabled() && CompiledSdkVersion() >= 0x03080000) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	bool wasStopped = IsStopped(t);
	sched::ChargeCycles(kTerminateLookupCycles);
	if (!wasStopped) {
		sched::ChargeCycles(kTerminateCycles);
		sched::TerminateThread(t, (s32)SCE_KERNEL_ERROR_THREAD_TERMINATED);
		TriggerThreadEvent(t, THREADEVENT_EXIT);
	}
	DestroyThread(t);
	return 0;
}

int sceKernelGetThreadId() {
	sched::ChargeCycles(180);
	return sched::CurrentId();
}

int sceKernelGetThreadCurrentPriority() {
	PspThread *t = CurThread();
	return t ? t->currentPriority : 0;
}

int sceKernelChangeThreadPriority(SceUID id, s32 prio) {
	if (id == 0) id = sched::CurrentId();
	// 0 means the current (running) thread's priority, not the target's.
	if (prio == 0 && CurThread()) prio = CurThread()->currentPriority;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (IsStopped(t)) return (int)SCE_KERNEL_ERROR_DORMANT;
	if (prio < 0x08 || prio > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	sched::ChargeCycles(450);
	if (t == CurThread()) {
		// The running thread goes to the back of its (new) priority's queue.
		t->currentPriority = prio;
		sched::YieldToSamePriority();
	} else {
		sched::ChangePriority(t, prio);
		sched::Reschedule();
	}
	return 0;
}

int sceKernelRotateThreadReadyQueue(s32 prio) {
	PspThread *cur = CurThread();
	if (!cur) return 0;
	if (prio == 0) prio = cur->currentPriority;
	if (prio <= 0x07 || prio > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	sched::ChargeCycles(250);
	if (!sched::ReadyQueueEmpty(prio)) {
		if (cur->currentPriority == prio) {
			// Yield to everyone else of the same priority.
			sched::YieldToSamePriority();
			return 0;
		}
		sched::RotateReadyQueue(prio);
	}
	sched::Reschedule();
	return 0;
}

int sceKernelGetThreadExitStatus(SceUID id) {
	sched::ChargeCycles(330);
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->status == THREADSTATUS_DORMANT) return t->exitStatus;
	return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
}

int sceKernelGetThreadStackFreeSize(SceUID id) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	// Count the untouched 0xFF bytes from 0x10 above the bottom (the UID is written there).
	int n = 0;
	if (t->stackSize > 0x10 && mem::IsValidRange(t->stackAddr + 0x10, t->stackSize - 0x10)) {
		for (u32 off = 0x10; off < t->stackSize; ++off) {
			if (GRead8(t->stackAddr + off) != 0xFF) break;
			n++;
		}
	}
	return n & ~3;
}

int sceKernelCheckThreadStack() {
	PspThread *t = CurThread();
	if (!t) return -1;
	s64 diff = (s64)t->ctx.r[29] - (s64)t->stackAddr;
	return (int)(diff < 0 ? -diff : diff);
}

struct SceKernelThreadInfo {
	u32 size;
	char name[32];
	u32 attr;
	u32 status;
	u32 entry;
	u32 stack;
	s32 stackSize;
	u32 gpReg;
	s32 initPriority;
	s32 currentPriority;
	u32 waitType;
	SceUID waitId;
	s32 wakeupCount;
	s32 exitStatus;
	u32 runClocksLo, runClocksHi;
	s32 intrPreemptCount;
	s32 threadPreemptCount;
	s32 releaseCount;
};
static_assert(sizeof(SceKernelThreadInfo) == 0x68, "thread info size");

SceKernelThreadInfo MakeThreadInfo(PspThread *t, u32 size) {
	SceKernelThreadInfo info{};
	info.size = size;
	CopyName(info.name, t->name);
	info.attr = t->attr;
	info.status = t->status;
	bool current = t == CurThread();
	if (current) {
		sched::UpdateRunTime();
		info.status = THREADSTATUS_RUNNING | (t->status & THREADSTATUS_SUSPEND);
	}
	info.entry = t->entry;
	info.stack = t->stackAddr;
	info.stackSize = (s32)t->stackSize;
	info.gpReg = t->gpReg;
	info.initPriority = t->initPriority;
	info.currentPriority = t->currentPriority;
	// A thread keeps reporting its last wait until it gets the CPU again.
	bool showWait = !current && ((t->status & THREADSTATUS_WAIT) || t->waitDone);
	info.waitType = showWait ? t->waitType : 0;
	info.waitId = showWait ? t->waitId : 0;
	info.wakeupCount = t->wakeupCount;
	info.exitStatus = t->exitStatus;
	info.runClocksLo = (u32)t->runUs;
	info.runClocksHi = (u32)(t->runUs >> 32);
	info.intrPreemptCount = (s32)t->intrPreemptCount;
	info.threadPreemptCount = (s32)t->threadPreemptCount;
	info.releaseCount = (s32)t->releaseCount;
	return info;
}

int sceKernelReferThreadStatus(SceUID id, u32 infoPtr) {
	constexpr u32 kSize = 104, kSizeAfter260 = 108;
	PspThread *t = ThreadOrCurrent(id);
	if (!t) {
		sched::ChargeCycles(700);
		sched::Reschedule();
		return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	}
	if (!mem::IsValidRange(infoPtr, 4) || (infoPtr & 3)) return 0;
	u32 wanted = GRead32(infoPtr);
	if (CompiledSdkVersion() > 0x02060010) {
		if (wanted > kSizeAfter260) {
			sched::ChargeCycles(1200);
			sched::Reschedule();
			return (int)SCE_KERNEL_ERROR_ILLEGAL_SIZE;
		}
		SceKernelThreadInfo info = MakeThreadInfo(t, kSizeAfter260);
		if (wanted != 0) {
			u32 n = std::min<u32>(wanted, sizeof(info));
			if (mem::IsValidRange(infoPtr, wanted)) {
				mem::WriteBytes(infoPtr, &info, n);
				if (wanted > sizeof(info)) mem::Memset(infoPtr + sizeof(info), 0, wanted - sizeof(info));
			}
		}
	} else {
		SceKernelThreadInfo info = MakeThreadInfo(t, kSize);
		u32 n = std::min(kSize, wanted);
		if (n && mem::IsValidRange(infoPtr, n)) mem::WriteBytes(infoPtr, &info, n);
	}
	sched::ChargeCycles(1400);
	sched::Reschedule();
	return 0;
}

int sceKernelReferThreadRunStatus(SceUID id, u32 statusPtr) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	// SceKernelThreadRunStatus: size, status, currentPriority, waitType, waitId, wakeupCount,
	// runForClocks(8), intr/thread preempt counts, releaseCount.
	u32 buf[11] = {};
	if (!mem::IsValidRange(statusPtr, sizeof(buf))) return -1;
	SceKernelThreadInfo info = MakeThreadInfo(t, 0);
	buf[0] = sizeof(buf);
	buf[1] = info.status;
	buf[2] = (u32)info.currentPriority;
	buf[3] = info.waitType;
	buf[4] = (u32)info.waitId;
	buf[5] = (u32)info.wakeupCount;
	buf[6] = info.runClocksLo;
	buf[7] = info.runClocksHi;
	buf[8] = (u32)info.intrPreemptCount;
	buf[9] = (u32)info.threadPreemptCount;
	buf[10] = (u32)info.releaseCount;
	mem::WriteBytes(statusPtr, buf, sizeof(buf));
	return 0;
}

int sceKernelSuspendThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (IsStopped(t)) return (int)SCE_KERNEL_ERROR_DORMANT;
	if (t->IsSuspended()) return (int)SCE_KERNEL_ERROR_SUSPEND;
	sched::Suspend(t);
	return 0;
}

int sceKernelResumeThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsSuspended()) return (int)SCE_KERNEL_ERROR_NOT_SUSPEND;
	sched::Resume(t);
	sched::Reschedule();
	return 0;
}

int WaitThreadEnd(SceUID id, u32 timeoutPtr, bool cb) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	PspThread *cur = CurThread();
	if (t->status == THREADSTATUS_DORMANT) {
		if (cb) RunPendingCallbacks(cur);
		return t->exitStatus;
	}
	if (sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	SceUID me = cur->uid;
	auto addWaiter = [id, me]() {
		if (PspThread *target = Get<PspThread>(id)) {
			if (std::find(target->endWaiters.begin(), target->endWaiters.end(), me) == target->endWaiters.end())
				target->endWaiters.push_back(me);
		}
	};
	addWaiter();
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (PspThread *target = Get<PspThread>(id))
			target->endWaiters.erase(std::remove(target->endWaiters.begin(), target->endWaiters.end(), me),
			                         target->endWaiters.end());
	};
	hooks.resume = [id, me, addWaiter]() {
		PspThread *w = Get<PspThread>(me);
		PspThread *target = Get<PspThread>(id);
		if (!target) {
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		if (target->status == THREADSTATUS_DORMANT) {
			sched::Wake(w, target->exitStatus);
			return true;
		}
		addWaiter();
		return false;
	};
	return sched::Wait(WAITTYPE_THREADEND, id, timeoutPtr, cb, &hooks);
}

int sceKernelWaitThreadEnd(SceUID id, u32 timeoutPtr) { return WaitThreadEnd(id, timeoutPtr, false); }
int sceKernelWaitThreadEndCB(SceUID id, u32 timeoutPtr) { return WaitThreadEnd(id, timeoutPtr, true); }

int sceKernelReleaseWaitThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsWaiting() || t->waitDone || t->waitType == WAITTYPE_NONE) return (int)SCE_KERNEL_ERROR_NOT_WAIT;
	if (t->waitType == WAITTYPE_HLEDELAY || t->waitType == WAITTYPE_MODULE) return (int)SCE_KERNEL_ERROR_NOT_WAIT;
	t->waitTimeoutPtr = 0;  // a released wait leaves the timeout alone
	sched::Wake(t, (s32)SCE_KERNEL_ERROR_RELEASE_WAIT);
	sched::Reschedule();
	return 0;
}

// ---------------------------------------------------------------- sleep / delay

int SleepThread(bool cb) {
	PspThread *t = CurThread();
	if (!t) return -1;
	bool pending = cb && HasPendingCallbacks(t) && CanRunCallbacks(t);
	if (t->wakeupCount > 0 && !pending) {
		t->wakeupCount--;
		return 0;
	}
	SceUID me = t->uid;
	WaitHooks hooks;
	hooks.resume = [me]() {
		PspThread *w = Get<PspThread>(me);
		if (w && w->wakeupCount > 0) {
			w->wakeupCount--;
			sched::Wake(w, 0);
			return true;
		}
		return false;
	};
	return sched::Wait(WAITTYPE_SLEEP, 0, 0, cb, &hooks);
}

int sceKernelSleepThread() { return SleepThread(false); }
int sceKernelSleepThreadCB() { return SleepThread(true); }

int sceKernelWakeupThread(SceUID id) {
	if (id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsWaitingFor(WAITTYPE_SLEEP, 0)) {
		t->wakeupCount++;
		return 0;
	}
	sched::Wake(t, 0);
	sched::Reschedule();
	return 0;
}

int sceKernelCancelWakeupThread(SceUID id) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	int count = t->wakeupCount;
	t->wakeupCount = 0;
	return count;
}

// Measured from inside the delaying thread, a delay of n us takes n + 25us, and at least 230us
// (threads/scheduling/delaylen, handoff). Part of that is the call's own time before it waits.
constexpr s64 kDelayCallCycles = 400;     // up to the "returns at once" decision
// Setting up the wait: a thread that a better one's delay hands the CPU to starts waiting late
// enough that a 1000us delay ends the wait less than 1012us in (threads/semaphores/wait).
constexpr s64 kDelayWaitCycles = 2750;
// The timer interrupt that ends a delay takes about 23us of whatever thread has the CPU then: a
// better thread delaying 200us at a time costs a busy thread about 45us per wake, switches
// included (threads/scheduling/preemptsyscall).
constexpr u32 kDelayWakeCycles = 23 * 222;
u64 DelayUs(u64 us) {
	if (us < 200) us = 205;
	// Wraps around (seen on firmware), and potentially wakes up soon.
	if (us > 0x8000000000000000ULL) us -= 0x8000000000000000ULL;
	// Converting millennia to hundreds of years: basically forever, but no overflow.
	if (us > 0x0010000000000000ULL) us >>= 12;
	// It never wakes up right away.
	return us + 10;
}

int Delay(u64 us, bool cb, bool atOnceCheck) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	sched::ChargeCycles(kDelayCallCycles);
	if (atOnceCheck) {
		if (sched::DelayReturnsAtOnce((u32)std::min<u64>(us, 0xFFFFFFFF)) && !(cb && HasPendingCallbacks(t))) return 0;
	}
	sched::ChargeCycles(kDelayWaitCycles);
	u64 deadline = sched::NowUs() + DelayUs(us);
	return sched::WaitUntil(WAITTYPE_DELAY, t->uid, deadline, cb, nullptr, kDelayWakeCycles);
}

int sceKernelDelayThread(u32 us) { return Delay(us, false, true); }
int sceKernelDelayThreadCB(u32 us) { return Delay(us, true, true); }

int sceKernelDelaySysClockThread(u32 clockPtr) {
	if (!mem::IsValidRange(clockPtr, 8)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	return Delay(GRead<u64>(clockPtr), false, false);
}
int sceKernelDelaySysClockThreadCB(u32 clockPtr) {
	if (!mem::IsValidRange(clockPtr, 8)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	return Delay(GRead<u64>(clockPtr), true, false);
}

// ---------------------------------------------------------------- dispatch

u32 sceKernelSuspendDispatchThread() {
	if (!*g_intrEnabled) return SCE_KERNEL_ERROR_CPUDI;
	u32 old = sched::DispatchFlag() ? 1 : 0;
	sched::SetDispatchEnabled(false);
	sched::ChargeCycles(940);
	return old;
}

u32 sceKernelResumeDispatchThread(u32 enabled) {
	if (!*g_intrEnabled) return SCE_KERNEL_ERROR_CPUDI;
	sched::SetDispatchEnabled(enabled != 0);
	sched::ChargeCycles(940);
	sched::Reschedule();
	return 0;
}

int sceKernelChangeCurrentThreadAttr(u32 clearAttr, u32 setAttr) {
	if ((clearAttr & ~PSP_THREAD_ATTR_VFPU) != 0 || (setAttr & ~PSP_THREAD_ATTR_VFPU) != 0)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	PspThread *t = CurThread();
	if (!t) return -1;
	t->attr = (t->attr & ~clearAttr) | setAttr;
	return 0;
}

// sceKernelExtendThreadStack(size, entry, arg): run entry(arg) on a fresh stack, which the
// thread reports as its stack meanwhile.
u32 sceKernelExtendThreadStack(u32 size, u32 entry, u32 arg) {
	if (size < 512) return SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE;
	PspThread *t = CurThread();
	if (!t) return (u32)-1;
	u32 stack = g_userMem.Alloc(size, true, ("extended/" + t->name).c_str());
	if (!stack) return SCE_KERNEL_ERROR_NO_MEMORY;
	u32 blockSize = g_userMem.BlockSize(stack);
	if (!blockSize) blockSize = AlignUp(size, 0x100);
	t->pushedStacks.push_back({t->stackAddr, t->stackSize});
	t->stackAddr = stack;
	t->stackSize = blockSize;
	mem::Memset(stack, 0xFF, blockSize);
	GWrite32(stack, (u32)t->uid);
	u32 end = stack + blockSize;
	// The firmware saves ra, sp and pc at the top of the new stack.
	GWrite32(end - 4, t->ctx.r[31]);
	GWrite32(end - 8, t->ctx.r[29]);
	GWrite32(end - 12, hle::CurrentCallerPc() + 8);
	u32 oldSp = t->ctx.r[29];
	t->ctx.r[29] = end - 0x10;
	u32 result = cpu::CallGuest(&t->ctx, entry, {arg});
	t->ctx.r[29] = oldSp;
	if (!t->pushedStacks.empty()) {
		g_userMem.Free(t->stackAddr);
		t->stackAddr = t->pushedStacks.back().first;
		t->stackSize = t->pushedStacks.back().second;
		t->pushedStacks.pop_back();
	}
	return result;
}

// ---------------------------------------------------------------- callbacks

int sceKernelCreateCallback(const char *name, u32 entry, u32 common) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (entry & 0xF0000000) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	return CreateCallback(name, entry, common);
}

int sceKernelDeleteCallback(SceUID id) {
	Callback *cb = Get<Callback>(id);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	if (PspThread *t = Get<PspThread>(cb->threadId))
		t->callbacks.erase(std::remove(t->callbacks.begin(), t->callbacks.end(), id), t->callbacks.end());
	Destroy(id);
	return 0;
}

int sceKernelNotifyCallback(SceUID id, s32 arg) {
	Callback *cb = Get<Callback>(id);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	SceUID owner = cb->threadId;
	NotifyCallback(id, arg);
	// A better priority thread in a CB wait runs the callback right away.
	PspThread *t = Get<PspThread>(owner);
	if (t && t != CurThread() && t->IsWaiting() && t->waitCallbacks) sched::Reschedule();
	return 0;
}

int sceKernelCancelCallback(SceUID id) {
	Callback *cb = Get<Callback>(id);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	cb->notifyArg = 0;
	cb->notifyCount = 0;
	return 0;
}

int sceKernelGetCallbackCount(SceUID id) {
	Callback *cb = Get<Callback>(id);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	return cb->notifyCount;
}

int sceKernelReferCallbackStatus(SceUID id, u32 infoPtr) {
	Callback *cb = Get<Callback>(id);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	struct {
		u32 size;
		char name[32];
		SceUID threadId;
		u32 callback;
		u32 common;
		s32 notifyCount;
		s32 notifyArg;
	} info{};
	if (!mem::IsValidRange(infoPtr, sizeof(info)) || GRead32(infoPtr) == 0) return 0;
	info.size = sizeof(info);
	CopyName(info.name, cb->name);
	info.threadId = cb->threadId;
	info.callback = cb->entry;
	info.common = cb->commonArg;
	info.notifyCount = cb->notifyCount;
	info.notifyArg = cb->notifyArg;
	mem::WriteBytes(infoPtr, &info, sizeof(info));
	return 0;
}

void sceKernelCheckCallback(Ctx *c) {
	PspThread *t = CurThread();
	sched::ChargeCycles(230);
	// Unlike CB waits, this refuses to run callbacks from inside one.
	if (t && t->callbackDepth > 0) {
		c->r[2] = SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
		return;
	}
	c->r[2] = (t && RunPendingCallbacks(t)) ? 1 : 0;
}

// ---------------------------------------------------------------- time

int sceKernelGetSystemTime(u32 clockPtr) {
	u64 t = sched::NowUs();
	if (mem::IsValidRange(clockPtr, 8) && (clockPtr & 3) == 0) GWrite<u64>(clockPtr, t);
	sched::ChargeCycles(265);
	return 0;
}
u32 sceKernelGetSystemTimeLow() {
	u64 t = sched::NowUs();
	sched::ChargeCycles(165);
	return (u32)t;
}
u64 sceKernelGetSystemTimeWide() {
	u64 t = sched::NowUs();
	sched::ChargeCycles(250);
	return t;
}

int sceKernelUSec2SysClock(u32 us, u32 clockPtr) {
	if (mem::IsValidRange(clockPtr, 8) && (clockPtr & 3) == 0) GWrite<u64>(clockPtr, us);
	sched::ChargeCycles(165);
	return 0;
}
u64 sceKernelUSec2SysClockWide(u32 us) {
	sched::ChargeCycles(150);
	return us;
}
int sceKernelSysClock2USec(u32 clockPtr, u32 highPtr, u32 lowPtr) {
	if (!mem::IsValidRange(clockPtr, 8)) return -1;
	u64 t = GRead<u64>(clockPtr);
	if (mem::IsValidRange(highPtr, 4) && (highPtr & 3) == 0) GWrite32(highPtr, (u32)(t / 1000000));
	if (mem::IsValidRange(lowPtr, 4) && (lowPtr & 3) == 0) GWrite32(lowPtr, (u32)(t % 1000000));
	sched::ChargeCycles(415);
	return 0;
}
int sceKernelSysClock2USecWide(u32 lo, u32 hi, u32 lowPtr, u32 highPtr) {
	u64 t = ((u64)hi << 32) | lo;
	if (mem::IsValidRange(lowPtr, 4) && (lowPtr & 3) == 0) {
		GWrite32(lowPtr, (u32)(t / 1000000));
		if (mem::IsValidRange(highPtr, 4) && (highPtr & 3) == 0) GWrite32(highPtr, (u32)(t % 1000000));
	} else if (mem::IsValidRange(highPtr, 4) && (highPtr & 3) == 0) {
		GWrite32(highPtr, (u32)t);
	}
	sched::ChargeCycles(385);
	return 0;
}

// ---------------------------------------------------------------- id lists / system status

enum : u32 {
	TMID_Thread = 1,
	TMID_Semaphore = 2,
	TMID_EventFlag = 3,
	TMID_Mbox = 4,
	TMID_Vpl = 5,
	TMID_Fpl = 6,
	TMID_Mpipe = 7,
	TMID_Callback = 8,
	TMID_ThreadEventHandler = 9,
	TMID_Alarm = 10,
	TMID_VTimer = 11,
	TMID_Mutex = 12,
	TMID_LwMutex = 13,
	TMID_Tlspl = 14,
	TMID_SleepThread = 64,
	TMID_DelayThread = 65,
	TMID_SuspendThread = 66,
	TMID_DormantThread = 67,
};

u32 IdTypeOf(KObject *o) {
	switch (o->Type()) {
	case KType::Thread: return TMID_Thread;
	case KType::Semaphore: return TMID_Semaphore;
	case KType::EventFlag: return TMID_EventFlag;
	case KType::Mbx: return TMID_Mbox;
	case KType::Vpl: return TMID_Vpl;
	case KType::Fpl: return TMID_Fpl;
	case KType::MsgPipe: return TMID_Mpipe;
	case KType::Callback: return TMID_Callback;
	case KType::ThreadEventHandler: return TMID_ThreadEventHandler;
	case KType::Alarm: return TMID_Alarm;
	case KType::VTimer: return TMID_VTimer;
	case KType::Mutex: return TMID_Mutex;
	case KType::LwMutex: return TMID_LwMutex;
	case KType::Tls: return TMID_Tlspl;
	default: return 0;
	}
}

u32 sceKernelGetThreadmanIdType(SceUID id) {
	KObject *o = GetAny(id);
	if (!o) return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	u32 type = IdTypeOf(o);
	return type ? type : SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
}

u32 sceKernelGetThreadmanIdList(u32 type, u32 listPtr, u32 listSize, u32 countPtr) {
	if (listSize >= 0x8000000) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (!mem::IsValidAddress(listPtr) && listSize > 0) return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	std::vector<SceUID> ids;
	if (type > 0 && type <= TMID_Tlspl) {
		ForEachObject([&](KObject *o) {
			if (IdTypeOf(o) == type) ids.push_back(o->uid);
		});
	} else if (type >= TMID_SleepThread && type <= TMID_DormantThread) {
		ForEach<PspThread>([&](PspThread *t) {
			bool match = false;
			switch (type) {
			case TMID_SleepThread: match = t->IsWaitingFor(WAITTYPE_SLEEP, 0); break;
			case TMID_DelayThread: match = t->IsWaitingFor(WAITTYPE_DELAY, t->uid); break;
			case TMID_SuspendThread: match = t->IsSuspended(); break;
			case TMID_DormantThread: match = IsStopped(t); break;
			}
			if (match) ids.push_back(t->uid);
		});
	} else {
		return SCE_KERNEL_ERROR_ILLEGAL_TYPE;
	}
	u32 n = 0;
	for (SceUID id : ids) {
		if (n < listSize && mem::IsValidRange(listPtr + n * 4, 4)) GWrite32(listPtr + n * 4, (u32)id);
		n++;
	}
	if (mem::IsValidAddress(countPtr)) GWrite32(countPtr, (u32)ids.size());
	return (u32)ids.size() > listSize ? listSize : (u32)ids.size();
}

int sceKernelReferSystemStatus(u32 statusPtr) {
	u32 buf[7] = {};
	buf[0] = sizeof(buf);
	if (mem::IsValidRange(statusPtr, sizeof(buf))) mem::WriteBytes(statusPtr, buf, sizeof(buf));
	return 0;
}

// They just return NULL, in about 140 cycles.
u32 sceKernelReferGlobalProfiler() {
	sched::ChargeCycles(140);
	return 0;
}
u32 sceKernelReferThreadProfiler() {
	sched::ChargeCycles(140);
	return 0;
}

}  // namespace

void Register_ThreadManForUser() {
	g_threadReturnSyscall = hle::GetSyscallCode("FakeSysCalls", NID_THREAD_RETURN);
	static const hle::HleFunction fake[] = {
		HLEFN(NID_THREAD_RETURN, _sceKernelExitThread),
	};
	hle::RegisterModule("FakeSysCalls", fake);

	constexpr u32 NI = KF_NOT_IN_INTERRUPT;
	constexpr u32 NID = KF_NOT_IN_INTERRUPT | KF_NOT_DISPATCH_SUSPENDED;
	static const hle::HleFunction funcs[] = {
		HLEFN(0xFCCFAD26, sceKernelCancelWakeupThread),
		HLEFN(0xEA748E31, sceKernelChangeCurrentThreadAttr),
		HLEFN(0x71BC9871, sceKernelChangeThreadPriority),
		KFN(0x446D8DE6, sceKernelCreateThread, NI),
		HLEFN(0x9FA03CD3, sceKernelDeleteThread),
		KFN(0xBD123D9E, sceKernelDelaySysClockThread, NID),
		KFN(0x1181E963, sceKernelDelaySysClockThreadCB, NID),
		KFN(0xCEADEB47, sceKernelDelayThread, NID),
		KFN(0x68DA9E36, sceKernelDelayThreadCB, NID),
		HLEFN(0xAA73C935, sceKernelExitThread),
		HLEFN(0x809CE29B, sceKernelExitDeleteThread),
		HLEFN(0x94AA61EE, sceKernelGetThreadCurrentPriority),
		KFN(0x293B45B8, sceKernelGetThreadId, NI),
		HLEFN(0x3B183E26, sceKernelGetThreadExitStatus),
		HLEFN(0x52089CA1, sceKernelGetThreadStackFreeSize),
		HLEFN(0xFFC36A14, sceKernelReferThreadRunStatus),
		HLEFN(0x17C1684E, sceKernelReferThreadStatus),
		HLEFN(0x2C34E053, sceKernelReleaseWaitThread),
		HLEFN(0x75156E8F, sceKernelResumeThread),
		KFN(0x3AD58B8C, sceKernelSuspendDispatchThread, NI),
		KFN(0x27E22EC2, sceKernelResumeDispatchThread, NI),
		HLEFN(0x912354A7, sceKernelRotateThreadReadyQueue),
		KFN(0x9ACE131E, sceKernelSleepThread, NID),
		KFN(0x82826F70, sceKernelSleepThreadCB, NID),
		KFN(0xF475845D, sceKernelStartThread, NI),
		HLEFN(0x9944F31F, sceKernelSuspendThread),
		HLEFN(0x616403BA, sceKernelTerminateThread),
		HLEFN(0x383F7BCC, sceKernelTerminateDeleteThread),
		HLEFN(0x840E8133, sceKernelWaitThreadEndCB),
		HLEFN(0xD13BDE95, sceKernelCheckThreadStack),
		HLEFN(0x94416130, sceKernelGetThreadmanIdList),
		HLEFN(0x57CF62DD, sceKernelGetThreadmanIdType),
		HLEFN(0xBC80EC7C, sceKernelExtendThreadStack),

		HLEFN(0x82BC5777, sceKernelGetSystemTimeWide),
		HLEFN(0xDB738F35, sceKernelGetSystemTime),
		HLEFN(0x369ED59D, sceKernelGetSystemTimeLow),
		HLEFN(0x8218B4DD, sceKernelReferGlobalProfiler),
		HLEFN(0x627E6F3A, sceKernelReferSystemStatus),
		HLEFN(0x64D4540E, sceKernelReferThreadProfiler),

		HLEFN(0xBA6B92E2, sceKernelSysClock2USec),
		HLEFN(0x110DEC9A, sceKernelUSec2SysClock),
		HLEFN(0xC8CD158C, sceKernelUSec2SysClockWide),
		HLEFN(0xE1619D7C, sceKernelSysClock2USecWide),

		HLEFN(0x278C0DF5, sceKernelWaitThreadEnd),
		HLEFN(0xD59EAD2F, sceKernelWakeupThread),

		HLEFN(0x0C106E53, sceKernelRegisterThreadEventHandler),
		HLEFN(0x72F3C145, sceKernelReleaseThreadEventHandler),
		HLEFN(0x369EEB6B, sceKernelReferThreadEventHandlerStatus),

		HLERAW(0x349D6D6C, sceKernelCheckCallback),
		HLEFN(0xE81CAF8F, sceKernelCreateCallback),
		HLEFN(0xEDBA5844, sceKernelDeleteCallback),
		HLEFN(0xC11BA8C4, sceKernelNotifyCallback),
		HLEFN(0xBA4051D6, sceKernelCancelCallback),
		HLEFN(0x2A3D44FF, sceKernelGetCallbackCount),
		HLEFN(0x730ED8BC, sceKernelReferCallbackStatus),

		HLEFN(0x532A522E, _sceKernelExitThread),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);

	kernel::RegisterThreadManSync();
	kernel::RegisterThreadManTimers();
	kernel::RegisterThreadManMbx();
	kernel::RegisterThreadManMsgPipe();
	kernel::RegisterThreadManPools();
}
