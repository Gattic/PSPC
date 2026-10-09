// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser: threads, callbacks, delays, semaphores, event flags, mutexes, alarms and
// system time. Behaviour (argument validation order, error codes, wake order) follows PPSSPP's
// HLE, which encodes pspautotests results recorded on hardware.
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later.
#include <algorithm>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace kernel {
u32 g_threadReturnSyscall = 0;
}

namespace {

constexpr u32 NID_THREAD_RETURN = 0xAD58E3D2;  // runtime-internal

PspThread *CurThread() { return sched::Current(); }

PspThread *ThreadOrCurrent(SceUID id) {
	if (id == 0) return CurThread();
	return Get<PspThread>(id);
}

bool TimesOutAtOnce(u32 timeoutPtr) {
	if (!timeoutPtr || !mem::IsValidRange(timeoutPtr, 4)) return false;
	return GRead32(timeoutPtr) == 0;
}

// ---------------------------------------------------------------- threads

int sceKernelCreateThread(const char *name, u32 entry, u32 prio, s32 stackSize, u32 attr, u32 optionAddr) {
	PspThread *cur = CurThread();
	u32 gp = cur ? cur->ctx.r[28] : 0;
	SceUID mod = cur ? cur->moduleId : 0;
	if ((attr & ~0xF0F0FFFFu) != 0) attr &= 0xF0F0FFFFu;
	SceUID id = CreateThread(name, entry, (s32)prio, stackSize, attr, gp, mod);
	if (id < 0) LOGD("THREAD", "sceKernelCreateThread(%s) failed %08x", name ? name : "(null)", id);
	return id;
}

int sceKernelStartThread(SceUID id, s32 argSize, u32 argPtr) {
	if (id == 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	if (argSize < 0 || (argPtr & 0x80000000)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsDormant()) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	return StartThread(t, (u32)argSize, argPtr);
}

void _sceKernelExitThread(s32 status) {
	sched::ExitCurrentThread(status, false);
}

int sceKernelExitThread(s32 status) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (status < 0) status = (s32)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	sched::ExitCurrentThread(status, false);
}

int sceKernelExitDeleteThread(s32 status) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	DeleteThreadCallbacks(t);
	FreeThreadStack(t);
	sched::ExitCurrentThread(status, true);
}

int sceKernelDeleteThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsDormant()) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	for (SceUID w : t->endWaiters) {
		if (PspThread *wt = Get<PspThread>(w); wt && wt->waitType == WAITTYPE_THREADEND && wt->waitId == id)
			sched::Wake(wt, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	}
	DeleteThreadCallbacks(t);
	FreeThreadStack(t);
	sched::DeleteThread(t);
	return 0;
}

int sceKernelTerminateThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->IsDormant()) return (int)SCE_KERNEL_ERROR_DORMANT;
	sched::TerminateThread(t, (s32)SCE_KERNEL_ERROR_THREAD_TERMINATED);
	sched::Reschedule();
	return 0;
}

int sceKernelTerminateDeleteThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsDormant()) sched::TerminateThread(t, (s32)SCE_KERNEL_ERROR_THREAD_TERMINATED);
	DeleteThreadCallbacks(t);
	FreeThreadStack(t);
	sched::DeleteThread(t);
	sched::Reschedule();
	return 0;
}

int sceKernelGetThreadId() {
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	return sched::CurrentId();
}

int sceKernelGetThreadCurrentPriority() {
	PspThread *t = CurThread();
	return t ? t->currentPriority : 0;
}

int sceKernelChangeThreadPriority(SceUID id, s32 prio) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->IsDormant()) return (int)SCE_KERNEL_ERROR_DORMANT;
	if (prio == 0) prio = CurThread() ? CurThread()->currentPriority : prio;
	if (prio < 0x08 || prio > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	sched::ChangePriority(t, prio);
	sched::Reschedule();
	if (t == CurThread()) sched::YieldToSamePriority();
	return 0;
}

int sceKernelRotateThreadReadyQueue(s32 prio) {
	PspThread *cur = CurThread();
	if (!cur) return 0;
	if (prio == 0) prio = cur->currentPriority;
	if (prio < 0x08 || prio > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	if (prio == cur->currentPriority) {
		sched::YieldToSamePriority();
	} else {
		sched::RotateReadyQueue(prio);
	}
	return 0;
}

int sceKernelGetThreadExitStatus(SceUID id) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsDormant()) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
	return t->exitStatus;
}

int sceKernelGetThreadStackFreeSize(SceUID id) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	// Count untouched 0xFF bytes from the bottom (after the UID word).
	u32 n = 0;
	for (u32 a = t->stackAddr + 4; a < t->stackAddr + t->stackSize; a++, n++) {
		if (GRead8(a) != 0xFF) break;
	}
	return (int)(n & ~3u);
}

int sceKernelCheckThreadStack() {
	PspThread *t = CurThread();
	if (!t) return 0;
	u32 sp = t->ctx.r[29];
	if (sp < t->stackAddr || sp > t->stackAddr + t->stackSize) return (int)SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE;
	return (int)(sp - t->stackAddr);
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

int sceKernelReferThreadStatus(SceUID id, u32 infoPtr) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!mem::IsValidRange(infoPtr, 4)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	u32 wanted = GRead32(infoPtr);
	if (wanted == 0) return 0;
	SceKernelThreadInfo info{};
	info.size = sizeof(info);
	strncpy(info.name, t->name.c_str(), 31);
	info.attr = t->attr;
	info.status = t->status;
	if (t == CurThread()) info.status = THREADSTATUS_RUNNING | (t->status & THREADSTATUS_SUSPEND);
	if (t->inReadyQueue && !(t->status & THREADSTATUS_WAIT)) info.status = THREADSTATUS_READY;
	info.entry = t->entry;
	info.stack = t->stackAddr;
	info.stackSize = (s32)t->stackSize;
	info.gpReg = t->gpReg;
	info.initPriority = t->initPriority;
	info.currentPriority = t->currentPriority;
	info.waitType = t->waitDone ? 0 : t->waitType;
	info.waitId = t->waitDone ? 0 : t->waitId;
	info.wakeupCount = t->wakeupCount;
	info.exitStatus = t->exitStatus;
	info.intrPreemptCount = (s32)t->intrPreemptCount;
	info.threadPreemptCount = (s32)t->threadPreemptCount;
	info.releaseCount = (s32)t->releaseCount;
	u32 n = std::min<u32>(wanted, sizeof(info));
	if (!mem::IsValidRange(infoPtr, n)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	memcpy(GPtr(infoPtr), &info, n);
	return 0;
}

u32 sceKernelReferThreadRunStatus(SceUID id, u32 statusPtr) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!mem::IsValidRange(statusPtr, 4)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	// SceKernelThreadRunStatus: size, status, currentPriority, waitType, waitId, wakeupCount,
	// runForClocks(8), intr/thread preempt counts, releaseCount.
	u32 buf[13] = {};
	buf[0] = sizeof(buf);
	buf[1] = t == CurThread() ? THREADSTATUS_RUNNING : t->status;
	buf[2] = (u32)t->currentPriority;
	buf[3] = t->waitType;
	buf[4] = (u32)t->waitId;
	buf[5] = (u32)t->wakeupCount;
	buf[8] = t->intrPreemptCount;
	buf[9] = t->threadPreemptCount;
	buf[10] = t->releaseCount;
	u32 wanted = std::min<u32>(GRead32(statusPtr), sizeof(buf));
	if (wanted) mem::WriteBytes(statusPtr, buf, wanted);
	return 0;
}

int sceKernelSuspendThread(SceUID id) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->IsDormant()) return (int)SCE_KERNEL_ERROR_DORMANT;
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

static int WaitThreadEnd(SceUID id, u32 timeoutPtr, bool cb) {
	if (id == 0 || id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (cb) RunPendingCallbacks(CurThread());
	if (t->IsDormant()) return t->exitStatus;
	if (TimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	if (std::find(t->endWaiters.begin(), t->endWaiters.end(), sched::CurrentId()) == t->endWaiters.end())
		t->endWaiters.push_back(sched::CurrentId());
	return sched::Wait(WAITTYPE_THREADEND, id, timeoutPtr, cb);
}

int sceKernelWaitThreadEnd(SceUID id, u32 timeoutPtr) { return WaitThreadEnd(id, timeoutPtr, false); }
int sceKernelWaitThreadEndCB(SceUID id, u32 timeoutPtr) { return WaitThreadEnd(id, timeoutPtr, true); }

int sceKernelReleaseWaitThread(SceUID id) {
	if (id == sched::CurrentId()) return (int)SCE_KERNEL_ERROR_ILLEGAL_THID;
	PspThread *t = Get<PspThread>(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (!t->IsWaiting() || t->waitDone) return (int)SCE_KERNEL_ERROR_NOT_WAIT;
	if (t->waitType == WAITTYPE_HLEDELAY) return (int)SCE_KERNEL_ERROR_NOT_WAIT;
	t->releaseCount++;
	sched::Wake(t, (s32)SCE_KERNEL_ERROR_RELEASE_WAIT);
	sched::Reschedule();
	return 0;
}

// ---------------------------------------------------------------- sleep / delay

static int SleepThread(bool cb) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (cb) RunPendingCallbacks(t);
	if (t->wakeupCount > 0) {
		t->wakeupCount--;
		return 0;
	}
	return sched::Wait(WAITTYPE_SLEEP, 0, 0, cb);
}

int sceKernelSleepThread() { return SleepThread(false); }
int sceKernelSleepThreadCB() { return SleepThread(true); }

int sceKernelWakeupThread(SceUID id) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	if (t->IsWaiting() && t->waitType == WAITTYPE_SLEEP && !t->waitDone) {
		sched::Wake(t, 0);
		sched::Reschedule();
	} else {
		t->wakeupCount++;
	}
	return 0;
}

int sceKernelCancelWakeupThread(SceUID id) {
	PspThread *t = ThreadOrCurrent(id);
	if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
	int count = t->wakeupCount;
	t->wakeupCount = 0;
	return count;
}

u64 DelayUs(u64 us) {
	if (us < 200) return 210;
	if (us > 0x8000000000000000ULL) us -= 0x8000000000000000ULL;
	if (us > 0x0010000000000000ULL) us >>= 12;
	return us;
}

static int Delay(u64 us, bool cb) {
	PspThread *t = CurThread();
	if (!t) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	u64 deadline = sched::NowUs() + DelayUs(us);
	return sched::WaitUntil(WAITTYPE_DELAY, t->uid, deadline, cb);
}

int sceKernelDelayThread(u32 us) { return Delay(us, false); }
int sceKernelDelayThreadCB(u32 us) { return Delay(us, true); }

int sceKernelDelaySysClockThread(u32 clockPtr) {
	if (!mem::IsValidRange(clockPtr, 8)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	return Delay(GRead<u64>(clockPtr), false);
}
int sceKernelDelaySysClockThreadCB(u32 clockPtr) {
	if (!mem::IsValidRange(clockPtr, 8)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	return Delay(GRead<u64>(clockPtr), true);
}

// ---------------------------------------------------------------- dispatch

u32 sceKernelSuspendDispatchThread() {
	if (!*g_intrEnabled) return SCE_KERNEL_ERROR_CPUDI;
	u32 old = sched::DispatchEnabled() ? 1 : 0;
	sched::SetDispatchEnabled(false);
	return old;
}

u32 sceKernelResumeDispatchThread(u32 enabled) {
	if (!*g_intrEnabled) return SCE_KERNEL_ERROR_CPUDI;
	bool was = sched::DispatchEnabled();
	sched::SetDispatchEnabled(enabled != 0);
	if (!was && enabled) sched::Reschedule();
	return 0;
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

int sceKernelNotifyCallback(SceUID id, s32 arg) { return NotifyCallback(id, arg); }

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
	if (!mem::IsValidRange(infoPtr, 4)) return 0;
	// SceKernelCallbackInfo: size, name[32], threadId, callback, common, notifyCount, notifyArg.
	struct {
		u32 size;
		char name[32];
		SceUID threadId;
		u32 callback;
		u32 common;
		s32 notifyCount;
		s32 notifyArg;
	} info{};
	info.size = sizeof(info);
	strncpy(info.name, cb->name.c_str(), 31);
	info.threadId = cb->threadId;
	info.callback = cb->entry;
	info.common = cb->commonArg;
	info.notifyCount = cb->notifyCount;
	info.notifyArg = cb->notifyArg;
	u32 wanted = std::min<u32>(GRead32(infoPtr), sizeof(info));
	if (wanted) mem::WriteBytes(infoPtr, &info, wanted);
	return 0;
}

void sceKernelCheckCallback(Ctx *c) {
	PspThread *t = CurThread();
	c->r[2] = (t && RunPendingCallbacks(t)) ? 1 : 0;
}

// ---------------------------------------------------------------- time

struct SceKernelSysClock {
	u32 low, hi;
};

u64 sceKernelGetSystemTimeWide() { return sched::NowUs(); }
u32 sceKernelGetSystemTimeLow() { return (u32)sched::NowUs(); }
int sceKernelGetSystemTime(u32 clockPtr) {
	if (!mem::IsValidRange(clockPtr, 8)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	GWrite<u64>(clockPtr, sched::NowUs());
	return 0;
}

int sceKernelUSec2SysClock(u32 us, u32 clockPtr) {
	if (mem::IsValidRange(clockPtr, 8)) GWrite<u64>(clockPtr, us);
	return 0;
}
u64 sceKernelUSec2SysClockWide(u32 us) { return us; }
int sceKernelSysClock2USec(u32 clockPtr, u32 lowPtr, u32 highPtr) {
	u64 t = mem::IsValidRange(clockPtr, 8) ? GRead<u64>(clockPtr) : 0;
	if (mem::IsValidRange(lowPtr, 4)) GWrite32(lowPtr, (u32)(t / 1000000));
	if (mem::IsValidRange(highPtr, 4)) GWrite32(highPtr, (u32)(t % 1000000));
	return 0;
}
int sceKernelSysClock2USecWide(u32 lo, u32 hi, u32 secPtr, u32 usecPtr) {
	u64 t = ((u64)hi << 32) | lo;
	if (mem::IsValidRange(secPtr, 4)) GWrite32(secPtr, (u32)(t / 1000000));
	if (mem::IsValidRange(usecPtr, 4)) GWrite32(usecPtr, (u32)(t % 1000000));
	return 0;
}

u32 sceKernelLibcClock() { return (u32)sched::NowUs(); }
u32 sceKernelLibcGettimeofday(u32 tvPtr, u32 tzPtr) {
	if (mem::IsValidRange(tvPtr, 8)) {
		u64 now = sched::NowUs() + 1262304000ull * 1000000ull;  // arbitrary epoch base (2010)
		GWrite32(tvPtr, (u32)(now / 1000000));
		GWrite32(tvPtr + 4, (u32)(now % 1000000));
	}
	(void)tzPtr;
	return 0;
}

int sceKernelDcacheWritebackAll() { return 0; }
int sceKernelDcacheWritebackInvalidateAll() { return 0; }

// ---------------------------------------------------------------- semaphores

struct Semaphore : KObject {
	static constexpr KType kType = KType::Semaphore;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 initCount = 0, count = 0, maxCount = 0;
	WaitQueue waiters;
};

constexpr u32 PSP_SEMA_ATTR_PRIORITY = 0x100;

// Wake waiters that can now take their count. Returns true if any woke.
bool SemaWakeWaiters(Semaphore *s) {
	s->waiters.Cleanup(WAITTYPE_SEMA, s->uid);
	if (s->attr & PSP_SEMA_ATTR_PRIORITY) s->waiters.SortByPriority();
	bool woke = false;
	for (size_t i = 0; i < s->waiters.threads.size();) {
		PspThread *t = Get<PspThread>(s->waiters.threads[i]);
		s32 want = (s32)t->waitValue[0];
		if (want > s->count) {
			if (!(s->attr & PSP_SEMA_ATTR_PRIORITY)) break;  // FIFO: strict order
			i++;
			continue;
		}
		s->count -= want;
		s->waiters.threads.erase(s->waiters.threads.begin() + i);
		sched::Wake(t, 0);
		woke = true;
	}
	return woke;
}

int sceKernelCreateSema(const char *name, u32 attr, s32 initVal, s32 maxVal, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (attr >= 0x200) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	auto *s = new Semaphore();
	s->name = name;
	s->attr = attr;
	s->initCount = s->count = initVal;
	s->maxCount = maxVal;
	(void)optPtr;
	return Register(s);
}

int sceKernelDeleteSema(SceUID id) {
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	bool woke = s->waiters.WakeAll(WAITTYPE_SEMA, id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

int sceKernelSignalSema(SceUID id, s32 signal) {
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	int waiting = s->waiters.Count(WAITTYPE_SEMA, id);
	if ((s64)s->count + signal - waiting > s->maxCount) return (int)SCE_KERNEL_ERROR_SEMA_OVF;
	s->count += signal;
	if (SemaWakeWaiters(s)) sched::Reschedule();
	return 0;
}

static int WaitSema(SceUID id, s32 want, u32 timeoutPtr, bool cb) {
	if (want <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	if (want > s->maxCount) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	PspThread *t = CurThread();
	bool hasCallbacks = cb && t && HasPendingCallbacks(t);
	if (s->count >= want && s->waiters.Count(WAITTYPE_SEMA, id) == 0 && !hasCallbacks) {
		s->count -= want;
		return 0;
	}
	if (!hasCallbacks && TimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	t->waitValue[0] = (u32)want;
	s->waiters.Add(t->uid);
	SceUID semId = id;
	t->timeoutHook = [semId]() {
		// FIFO semaphores: a timed-out head may unblock the next waiter.
		if (Semaphore *sm = Get<Semaphore>(semId)) {
			PspThread *cur = sched::Current();
			(void)cur;
			sched::ScheduleEvent(0, [semId]() {
				if (Semaphore *s2 = Get<Semaphore>(semId)) SemaWakeWaiters(s2);
			});
			(void)sm;
		}
	};
	return sched::Wait(WAITTYPE_SEMA, id, timeoutPtr, cb);
}

int sceKernelWaitSema(SceUID id, s32 want, u32 timeoutPtr) { return WaitSema(id, want, timeoutPtr, false); }
int sceKernelWaitSemaCB(SceUID id, s32 want, u32 timeoutPtr) { return WaitSema(id, want, timeoutPtr, true); }

int sceKernelPollSema(SceUID id, s32 want) {
	if (want <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	if (s->count >= want && s->waiters.Count(WAITTYPE_SEMA, id) == 0) {
		s->count -= want;
		return 0;
	}
	return (int)SCE_KERNEL_ERROR_SEMA_ZERO;
}

int sceKernelCancelSema(SceUID id, s32 newCount, u32 numWaitPtr) {
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	if (newCount > s->maxCount) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	int n = s->waiters.Count(WAITTYPE_SEMA, id);
	if (mem::IsValidRange(numWaitPtr, 4)) GWrite32(numWaitPtr, (u32)n);
	s->count = newCount < 0 ? s->initCount : newCount;
	if (s->waiters.WakeAll(WAITTYPE_SEMA, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL)) sched::Reschedule();
	return 0;
}

int sceKernelReferSemaStatus(SceUID id, u32 infoPtr) {
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	if (!mem::IsValidRange(infoPtr, 4)) return -1;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 initCount, currentCount, maxCount, numWaitThreads;
	} info{};
	info.size = sizeof(info);
	strncpy(info.name, s->name.c_str(), 31);
	info.attr = s->attr;
	info.initCount = s->initCount;
	info.currentCount = s->count;
	info.maxCount = s->maxCount;
	info.numWaitThreads = s->waiters.Count(WAITTYPE_SEMA, id);
	u32 wanted = std::min<u32>(GRead32(infoPtr), sizeof(info));
	if (wanted) mem::WriteBytes(infoPtr, &info, wanted);
	return 0;
}

// ---------------------------------------------------------------- event flags

struct EventFlag : KObject {
	static constexpr KType kType = KType::EventFlag;
	KType Type() const override { return kType; }
	u32 attr = 0, initPattern = 0, pattern = 0;
	WaitQueue waiters;
};

constexpr u32 PSP_EVENT_WAITMULTIPLE = 0x200;
constexpr u32 PSP_EVENT_WAITOR = 0x01;
constexpr u32 PSP_EVENT_WAITCLEARALL = 0x10;
constexpr u32 PSP_EVENT_WAITCLEAR = 0x20;
constexpr u32 PSP_EVENT_WAITKNOWN = PSP_EVENT_WAITCLEAR | PSP_EVENT_WAITCLEARALL | PSP_EVENT_WAITOR;

bool EvfMatches(u32 pattern, u32 bits, u32 mode) {
	return (mode & PSP_EVENT_WAITOR) ? (bits & pattern) != 0 : (bits & pattern) == bits;
}

bool EvfApply(u32 &pattern, u32 bits, u32 mode, u32 outAddr) {
	if (!EvfMatches(pattern, bits, mode)) return false;
	if (outAddr && mem::IsValidRange(outAddr, 4)) GWrite32(outAddr, pattern);
	if (mode & PSP_EVENT_WAITCLEAR) pattern &= ~bits;
	if (mode & PSP_EVENT_WAITCLEARALL) pattern = 0;
	return true;
}

int sceKernelCreateEventFlag(const char *name, u32 attr, u32 initPattern, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if ((attr & 0x100) != 0 || attr >= 0x300) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	auto *e = new EventFlag();
	e->name = name;
	e->attr = attr;
	e->initPattern = e->pattern = initPattern;
	(void)optPtr;
	return Register(e);
}

u32 sceKernelDeleteEventFlag(SceUID id) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	e->waiters.Cleanup(WAITTYPE_EVENTFLAG, id);
	for (SceUID u : e->waiters.threads) {
		PspThread *t = Get<PspThread>(u);
		if (t->waitValue[2] && mem::IsValidRange(t->waitValue[2], 4)) GWrite32(t->waitValue[2], e->pattern);
	}
	bool woke = e->waiters.WakeAll(WAITTYPE_EVENTFLAG, id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

u32 sceKernelSetEventFlag(SceUID id, u32 bits) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	e->pattern |= bits;
	e->waiters.Cleanup(WAITTYPE_EVENTFLAG, id);
	bool woke = false;
	for (size_t i = 0; i < e->waiters.threads.size();) {
		PspThread *t = Get<PspThread>(e->waiters.threads[i]);
		if (EvfApply(e->pattern, t->waitValue[0], t->waitValue[1], t->waitValue[2])) {
			e->waiters.threads.erase(e->waiters.threads.begin() + i);
			sched::Wake(t, 0);
			woke = true;
		} else {
			i++;
		}
	}
	if (woke) sched::Reschedule();
	return 0;
}

u32 sceKernelClearEventFlag(SceUID id, u32 bits) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	e->pattern &= bits;
	return 0;
}

static int WaitEventFlag(SceUID id, u32 bits, u32 mode, u32 outBitsPtr, u32 timeoutPtr, bool cb) {
	if ((mode & ~PSP_EVENT_WAITKNOWN) != 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	if (bits == 0) return (int)SCE_KERNEL_ERROR_EVF_ILPAT;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return (int)SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	PspThread *t = CurThread();
	bool hasCallbacks = cb && HasPendingCallbacks(t);
	if (!hasCallbacks && EvfApply(e->pattern, bits, mode, outBitsPtr)) return 0;
	if (hasCallbacks && EvfMatches(e->pattern, bits, mode)) {
		RunPendingCallbacks(t);
		if (EvfApply(e->pattern, bits, mode, outBitsPtr)) return 0;
	}
	if (e->waiters.Count(WAITTYPE_EVENTFLAG, id) > 0 && (e->attr & PSP_EVENT_WAITMULTIPLE) == 0)
		return (int)SCE_KERNEL_ERROR_EVF_MULTI;
	if (TimesOutAtOnce(timeoutPtr)) {
		if (outBitsPtr && mem::IsValidRange(outBitsPtr, 4)) GWrite32(outBitsPtr, e->pattern);
		return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	}
	t->waitValue[0] = bits;
	t->waitValue[1] = mode;
	t->waitValue[2] = outBitsPtr;
	e->waiters.Add(t->uid);
	t->timeoutHook = [id, outBitsPtr]() {
		if (EventFlag *ev = Get<EventFlag>(id); ev && outBitsPtr && mem::IsValidRange(outBitsPtr, 4))
			GWrite32(outBitsPtr, ev->pattern);
	};
	return sched::Wait(WAITTYPE_EVENTFLAG, id, timeoutPtr, cb);
}

int sceKernelWaitEventFlag(SceUID id, u32 bits, u32 mode, u32 outBitsPtr, u32 timeoutPtr) {
	return WaitEventFlag(id, bits, mode, outBitsPtr, timeoutPtr, false);
}
int sceKernelWaitEventFlagCB(SceUID id, u32 bits, u32 mode, u32 outBitsPtr, u32 timeoutPtr) {
	return WaitEventFlag(id, bits, mode, outBitsPtr, timeoutPtr, true);
}

int sceKernelPollEventFlag(SceUID id, u32 bits, u32 mode, u32 outBitsPtr) {
	if ((mode & ~PSP_EVENT_WAITKNOWN) != 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	if ((mode & PSP_EVENT_WAITCLEARALL) && (mode & PSP_EVENT_WAITCLEAR)) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	if (bits == 0) return (int)SCE_KERNEL_ERROR_EVF_ILPAT;
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return (int)SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	if (EvfApply(e->pattern, bits, mode, outBitsPtr)) return 0;
	if (outBitsPtr && mem::IsValidRange(outBitsPtr, 4)) GWrite32(outBitsPtr, e->pattern);
	if (e->waiters.Count(WAITTYPE_EVENTFLAG, id) > 0 && (e->attr & PSP_EVENT_WAITMULTIPLE) == 0)
		return (int)SCE_KERNEL_ERROR_EVF_MULTI;
	return (int)SCE_KERNEL_ERROR_EVF_COND;
}

u32 sceKernelCancelEventFlag(SceUID id, u32 pattern, u32 numWaitPtr) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	int n = e->waiters.Count(WAITTYPE_EVENTFLAG, id);
	if (mem::IsValidRange(numWaitPtr, 4)) GWrite32(numWaitPtr, (u32)n);
	e->pattern = pattern;
	for (SceUID u : e->waiters.threads) {
		PspThread *t = Get<PspThread>(u);
		if (t->waitValue[2] && mem::IsValidRange(t->waitValue[2], 4)) GWrite32(t->waitValue[2], pattern);
	}
	if (e->waiters.WakeAll(WAITTYPE_EVENTFLAG, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL)) sched::Reschedule();
	return 0;
}

u32 sceKernelReferEventFlagStatus(SceUID id, u32 infoPtr) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	if (!mem::IsValidRange(infoPtr, 4)) return 0;
	struct {
		u32 size;
		char name[32];
		u32 attr, initPattern, currentPattern;
		s32 numWaitThreads;
	} info{};
	info.size = sizeof(info);
	strncpy(info.name, e->name.c_str(), 31);
	info.attr = e->attr;
	info.initPattern = e->initPattern;
	info.currentPattern = e->pattern;
	info.numWaitThreads = e->waiters.Count(WAITTYPE_EVENTFLAG, id);
	u32 wanted = std::min<u32>(GRead32(infoPtr), sizeof(info));
	if (wanted) mem::WriteBytes(infoPtr, &info, wanted);
	return 0;
}

// ---------------------------------------------------------------- mutexes

struct Mutex : KObject {
	static constexpr KType kType = KType::Mutex;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 initCount = 0, lockLevel = 0;
	SceUID owner = 0;
	WaitQueue waiters;
};

constexpr u32 PSP_MUTEX_ATTR_PRIORITY = 0x100;
constexpr u32 PSP_MUTEX_ATTR_ALLOW_RECURSIVE = 0x200;

bool MutexWakeNext(Mutex *m) {
	m->waiters.Cleanup(WAITTYPE_MUTEX, m->uid);
	if (m->waiters.threads.empty()) return false;
	if (m->attr & PSP_MUTEX_ATTR_PRIORITY) m->waiters.SortByPriority();
	SceUID next = m->waiters.threads.front();
	m->waiters.threads.erase(m->waiters.threads.begin());
	PspThread *t = Get<PspThread>(next);
	m->owner = next;
	m->lockLevel = (s32)t->waitValue[0];
	sched::Wake(t, 0);
	return true;
}

int sceKernelCreateMutex(const char *name, u32 attr, s32 initCount, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (attr & ~0xBFF) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (initCount < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && initCount > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	auto *m = new Mutex();
	m->name = name;
	m->attr = attr;
	m->initCount = initCount;
	m->lockLevel = initCount;
	SceUID id = Register(m);
	if (initCount > 0) m->owner = sched::CurrentId();
	(void)optPtr;
	return id;
}

int sceKernelDeleteMutex(SceUID id) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	bool woke = m->waiters.WakeAll(WAITTYPE_MUTEX, id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

static int TryLockMutexFor(Mutex *m, s32 count, int *error) {
	SceUID cur = sched::CurrentId();
	if (count <= 0) {
		*error = (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		return 0;
	}
	if (count > 1 && !(m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)) {
		*error = (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		return 0;
	}
	if ((s64)m->lockLevel + count < 0) {
		*error = (int)SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
		return 0;
	}
	if (m->lockLevel == 0) {
		m->lockLevel = count;
		m->owner = cur;
		return 1;
	}
	if (m->owner == cur) {
		if (m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) {
			m->lockLevel += count;
			return 1;
		}
		*error = (int)SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED;
		return 0;
	}
	return 0;
}

static int LockMutex(SceUID id, s32 count, u32 timeoutPtr, bool cb) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	int error = 0;
	if (TryLockMutexFor(m, count, &error)) return 0;
	if (error) return error;
	if (TimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	PspThread *t = CurThread();
	t->waitValue[0] = (u32)count;
	m->waiters.Add(t->uid);
	return sched::Wait(WAITTYPE_MUTEX, id, timeoutPtr, cb);
}

int sceKernelLockMutex(SceUID id, s32 count, u32 timeoutPtr) { return LockMutex(id, count, timeoutPtr, false); }
int sceKernelLockMutexCB(SceUID id, s32 count, u32 timeoutPtr) { return LockMutex(id, count, timeoutPtr, true); }

int sceKernelTryLockMutex(SceUID id, s32 count) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	int error = 0;
	if (TryLockMutexFor(m, count, &error)) return 0;
	return error ? error : (int)SCE_KERNEL_ERROR_MUTEX_LOCKED;
}

int sceKernelUnlockMutex(SceUID id, s32 count) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	if (count <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && count > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (m->lockLevel == 0 || m->owner != sched::CurrentId()) return (int)SCE_KERNEL_ERROR_MUTEX_UNLOCKED;
	if (m->lockLevel < count) return (int)SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW;
	m->lockLevel -= count;
	if (m->lockLevel == 0) {
		m->owner = 0;
		if (MutexWakeNext(m)) sched::Reschedule();
	}
	return 0;
}

int sceKernelCancelMutex(SceUID id, s32 count, u32 numWaitPtr) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	bool recursive = (m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) != 0;
	if (count < 0 && count != -1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (count > 1 && !recursive) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	int n = m->waiters.Count(WAITTYPE_MUTEX, id);
	if (mem::IsValidRange(numWaitPtr, 4)) GWrite32(numWaitPtr, (u32)n);
	bool woke = m->waiters.WakeAll(WAITTYPE_MUTEX, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	if (count <= 0) {
		m->lockLevel = 0;
		m->owner = 0;
	} else {
		m->lockLevel = count;
		m->owner = sched::CurrentId();
	}
	if (woke) sched::Reschedule();
	return 0;
}

int sceKernelReferMutexStatus(SceUID id, u32 infoPtr) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	if (!mem::IsValidRange(infoPtr, 4)) return 0;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 initCount, lockLevel;
		SceUID lockThread;
		s32 numWaitThreads;
	} info{};
	info.size = sizeof(info);
	strncpy(info.name, m->name.c_str(), 31);
	info.attr = m->attr;
	info.initCount = m->initCount;
	info.lockLevel = m->lockLevel;
	info.lockThread = m->lockLevel ? m->owner : -1;
	info.numWaitThreads = m->waiters.Count(WAITTYPE_MUTEX, id);
	u32 wanted = std::min<u32>(GRead32(infoPtr), sizeof(info));
	if (wanted) mem::WriteBytes(infoPtr, &info, wanted);
	return 0;
}

// ---------------------------------------------------------------- alarms

struct Alarm : KObject {
	static constexpr KType kType = KType::Alarm;
	KType Type() const override { return kType; }
	u32 handler = 0, common = 0, gp = 0;
	u64 schedule = 0;  // absolute time
	u64 eventId = 0;
};

void ScheduleAlarm(SceUID id, u64 at) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return;
	a->schedule = at;
	a->eventId = sched::ScheduleEvent(at, [id]() {
		Alarm *al = Get<Alarm>(id);
		if (!al) return;
		al->eventId = 0;
		u32 handler = al->handler, common = al->common, gp = al->gp;
		sched::QueueInterrupt([id, handler, common, gp]() {
			u32 next = sched::RunInterruptHandler(handler, gp, {common});
			if (!Get<Alarm>(id)) return;
			if (next == 0) {
				Destroy(id);
			} else {
				ScheduleAlarm(id, sched::NowUs() + next);
			}
		});
	});
}

SceUID SetAlarm(u64 us, u32 handler, u32 common) {
	if (!mem::IsValidAddress(handler)) return (s32)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	auto *a = new Alarm();
	a->name = "alarm";
	a->handler = handler;
	a->common = common;
	PspThread *t = CurThread();
	a->gp = t ? t->ctx.r[28] : 0;
	SceUID id = Register(a);
	ScheduleAlarm(id, sched::NowUs() + us);
	return id;
}

int sceKernelSetAlarm(u32 us, u32 handler, u32 common) { return SetAlarm(us, handler, common); }
int sceKernelSetSysClockAlarm(u32 clockPtr, u32 handler, u32 common) {
	u64 us = mem::IsValidRange(clockPtr, 8) ? GRead<u64>(clockPtr) : 0;
	return SetAlarm(us, handler, common);
}
int sceKernelCancelAlarm(SceUID id) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return (int)SCE_KERNEL_ERROR_UNKNOWN_ALMID;
	sched::CancelEvent(a->eventId);
	Destroy(id);
	return 0;
}
int sceKernelReferAlarmStatus(SceUID id, u32 infoPtr) {
	Alarm *a = Get<Alarm>(id);
	if (!a) return (int)SCE_KERNEL_ERROR_UNKNOWN_ALMID;
	if (!mem::IsValidRange(infoPtr, 4)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	u32 size = GRead32(infoPtr);
	if (size >= 12) GWrite<u64>(infoPtr + 4, a->schedule);
	if (size >= 16) GWrite32(infoPtr + 12, a->handler);
	if (size >= 20) GWrite32(infoPtr + 16, a->common);
	return 0;
}

// ---------------------------------------------------------------- id lists / system status

u32 sceKernelGetThreadmanIdType(SceUID id) {
	KObject *o = GetAny(id);
	if (!o) return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	switch (o->Type()) {
	case KType::Thread: return 1;
	case KType::Semaphore: return 2;
	case KType::EventFlag: return 3;
	case KType::Mbx: return 4;
	case KType::Vpl: return 5;
	case KType::Fpl: return 6;
	case KType::MsgPipe: return 7;
	case KType::Callback: return 8;
	case KType::ThreadEventHandler: return 9;
	case KType::Alarm: return 10;
	case KType::VTimer: return 11;
	case KType::Mutex: return 12;
	case KType::LwMutex: return 13;
	case KType::Tls: return 14;
	default: return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	}
}

u32 sceKernelGetThreadmanIdList(u32 type, u32 listPtr, u32 listSize, u32 countPtr) {
	if (type < 1 || type > 14) return SCE_KERNEL_ERROR_ILLEGAL_TYPE;
	std::vector<SceUID> ids;
	ForEachObject([&](KObject *o) {
		if (sceKernelGetThreadmanIdType(o->uid) == type) ids.push_back(o->uid);
	});
	u32 n = 0;
	for (SceUID id : ids) {
		if (n < listSize && mem::IsValidRange(listPtr + n * 4, 4)) GWrite32(listPtr + n * 4, (u32)id);
		n++;
	}
	if (mem::IsValidRange(countPtr, 4)) GWrite32(countPtr, (u32)ids.size());
	return 0;
}

int sceKernelReferSystemStatus(u32 statusPtr) {
	if (mem::IsValidRange(statusPtr, 4)) {
		u32 size = std::min<u32>(GRead32(statusPtr), 64);
		for (u32 i = 4; i < size; i += 4) GWrite32(statusPtr + i, 0);
	}
	return 0;
}

int sceKernelReferGlobalProfiler() { return 0; }
u32 sceKernelReferThreadProfiler() { return 0; }

int sceKernelChangeCurrentThreadAttr(u32 clearAttr, u32 setAttr) {
	PspThread *t = CurThread();
	if ((clearAttr & ~PSP_THREAD_ATTR_VFPU) != 0 || (setAttr & ~PSP_THREAD_ATTR_VFPU) != 0)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (t) t->attr = (t->attr & ~clearAttr) | setAttr;
	return 0;
}

// sceKernelExtendThreadStack(size, entry, arg): run entry(arg) on a fresh stack.
u32 sceKernelExtendThreadStack(u32 size, u32 entry, u32 arg) {
	if (size < 512) return SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE;
	PspThread *t = CurThread();
	if (!t) return SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	u32 stack = g_userMem.Alloc(size, true, "extended stack");
	if (!stack) return SCE_KERNEL_ERROR_NO_MEMORY;
	mem::Memset(stack, 0xFF, size);
	GWrite32(stack, (u32)t->uid);
	u32 oldSp = t->ctx.r[29];
	t->ctx.r[29] = stack + size;
	u32 result = cpu::CallGuest(&t->ctx, entry, {arg});
	t->ctx.r[29] = oldSp;
	g_userMem.Free(stack);
	return result;
}

}  // namespace

void Register_ThreadManForUser() {
	g_threadReturnSyscall = hle::GetSyscallCode("FakeSysCalls", NID_THREAD_RETURN);
	static const hle::HleFunction fake[] = {
		HLEFN(NID_THREAD_RETURN, _sceKernelExitThread),
	};
	hle::RegisterModule("FakeSysCalls", fake);

	static const hle::HleFunction funcs[] = {
		HLEFN(0x55C20A00, sceKernelCreateEventFlag),
		HLEFN(0x812346E4, sceKernelClearEventFlag),
		HLEFN(0xEF9E4C70, sceKernelDeleteEventFlag),
		HLEFN(0x1FB15A32, sceKernelSetEventFlag),
		HLEFN(0x402FCF22, sceKernelWaitEventFlag),
		HLEFN(0x328C546A, sceKernelWaitEventFlagCB),
		HLEFN(0x30FD48F0, sceKernelPollEventFlag),
		HLEFN(0xCD203292, sceKernelCancelEventFlag),
		HLEFN(0xA66B0120, sceKernelReferEventFlagStatus),

		HLEFN(0x8FFDF9A2, sceKernelCancelSema),
		HLEFN(0xD6DA4BA1, sceKernelCreateSema),
		HLEFN(0x28B6489C, sceKernelDeleteSema),
		HLEFN(0x58B1F937, sceKernelPollSema),
		HLEFN(0xBC6FEBC5, sceKernelReferSemaStatus),
		HLEFN(0x3F53E640, sceKernelSignalSema),
		HLEFN(0x4E3A1105, sceKernelWaitSema),
		HLEFN(0x6D212BAC, sceKernelWaitSemaCB),

		HLEFN(0xF8170FBE, sceKernelDeleteMutex),
		HLEFN(0xB011B11F, sceKernelLockMutex),
		HLEFN(0x5BF4DD27, sceKernelLockMutexCB),
		HLEFN(0x6B30100F, sceKernelUnlockMutex),
		HLEFN(0xB7D098C6, sceKernelCreateMutex),
		HLEFN(0x0DDCD2C9, sceKernelTryLockMutex),
		HLEFN(0xA9C2CB9A, sceKernelReferMutexStatus),
		HLEFN(0x87D9223C, sceKernelCancelMutex),

		HLEFN(0xFCCFAD26, sceKernelCancelWakeupThread),
		HLEFN(0xEA748E31, sceKernelChangeCurrentThreadAttr),
		HLEFN(0x71BC9871, sceKernelChangeThreadPriority),
		HLEFN(0x446D8DE6, sceKernelCreateThread),
		HLEFN(0x9FA03CD3, sceKernelDeleteThread),
		HLEFN(0xBD123D9E, sceKernelDelaySysClockThread),
		HLEFN(0x1181E963, sceKernelDelaySysClockThreadCB),
		HLEFN(0xCEADEB47, sceKernelDelayThread),
		HLEFN(0x68DA9E36, sceKernelDelayThreadCB),
		HLEFN(0xAA73C935, sceKernelExitThread),
		HLEFN(0x809CE29B, sceKernelExitDeleteThread),
		HLEFN(0x94AA61EE, sceKernelGetThreadCurrentPriority),
		HLEFN(0x293B45B8, sceKernelGetThreadId),
		HLEFN(0x3B183E26, sceKernelGetThreadExitStatus),
		HLEFN(0x52089CA1, sceKernelGetThreadStackFreeSize),
		HLEFN(0xFFC36A14, sceKernelReferThreadRunStatus),
		HLEFN(0x17C1684E, sceKernelReferThreadStatus),
		HLEFN(0x2C34E053, sceKernelReleaseWaitThread),
		HLEFN(0x75156E8F, sceKernelResumeThread),
		HLEFN(0x3AD58B8C, sceKernelSuspendDispatchThread),
		HLEFN(0x27E22EC2, sceKernelResumeDispatchThread),
		HLEFN(0x912354A7, sceKernelRotateThreadReadyQueue),
		HLEFN(0x9ACE131E, sceKernelSleepThread),
		HLEFN(0x82826F70, sceKernelSleepThreadCB),
		HLEFN(0xF475845D, sceKernelStartThread),
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

		HLEFN(0x6652B8CA, sceKernelSetAlarm),
		HLEFN(0xB2C25152, sceKernelSetSysClockAlarm),
		HLEFN(0x7E65B999, sceKernelCancelAlarm),
		HLEFN(0xDAA3F564, sceKernelReferAlarmStatus),

		HLEFN(0xBA6B92E2, sceKernelSysClock2USec),
		HLEFN(0x110DEC9A, sceKernelUSec2SysClock),
		HLEFN(0xC8CD158C, sceKernelUSec2SysClockWide),
		HLEFN(0xE1619D7C, sceKernelSysClock2USecWide),

		HLEFN(0x278C0DF5, sceKernelWaitThreadEnd),
		HLEFN(0xD59EAD2F, sceKernelWakeupThread),

		HLERAW(0x349D6D6C, sceKernelCheckCallback),
		HLEFN(0xE81CAF8F, sceKernelCreateCallback),
		HLEFN(0xEDBA5844, sceKernelDeleteCallback),
		HLEFN(0xC11BA8C4, sceKernelNotifyCallback),
		HLEFN(0xBA4051D6, sceKernelCancelCallback),
		HLEFN(0x2A3D44FF, sceKernelGetCallbackCount),
		HLEFN(0x730ED8BC, sceKernelReferCallbackStatus),

		HLEFN(0x532A522E, _sceKernelExitThread),
		HLEFN(0x71EC4271, sceKernelLibcGettimeofday),
		HLEFN(0x79D1C3FA, sceKernelDcacheWritebackAll),
		HLEFN(0x91E4F6A7, sceKernelLibcClock),
		HLEFN(0xB435DEC5, sceKernelDcacheWritebackInvalidateAll),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);
}
