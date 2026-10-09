// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser semaphores, event flags and mutexes. Semantics (validation order, error codes,
// wake order, timeouts, behaviour around callbacks) follow PPSSPP's sceKernelSemaphore.cpp,
// sceKernelEventFlag.cpp and sceKernelMutex.cpp, which encode pspautotests results recorded on
// hardware. Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c)
// 2012- PPSSPP Project, GPL-2.0-or-later.
#include <algorithm>
#include <map>
#include <unordered_map>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace {

PspThread *CurThread() { return sched::Current(); }

// ================================================================ semaphores

struct Semaphore : KObject {
	static constexpr KType kType = KType::Semaphore;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 initCount = 0, count = 0, maxCount = 0;
	WaitQueue waiters;
};

constexpr u32 PSP_SEMA_ATTR_PRIORITY = 0x100;

// Gives the count to the waiter if it fits; returns true if it was woken.
bool SemaUnlockFor(Semaphore *s, PspThread *t) {
	s32 want = (s32)t->waitValue[0];
	if (want > s->count) return false;
	s->count -= want;
	sched::Wake(t, 0);
	return true;
}

// Wake waiters that can now take their count, in order. Returns true if any woke. With
// stopAtFirst, stops at the first waiter that must still wait.
bool SemaWakeWaiters(Semaphore *s, bool stopAtFirst = false) {
	s->waiters.Cleanup(WAITTYPE_SEMA, s->uid);
	if (s->attr & PSP_SEMA_ATTR_PRIORITY) s->waiters.SortByPriority();
	bool woke = false;
	for (size_t i = 0; i < s->waiters.threads.size();) {
		PspThread *t = Get<PspThread>(s->waiters.threads[i]);
		if (t && SemaUnlockFor(s, t)) {
			s->waiters.threads.erase(s->waiters.threads.begin() + i);
			woke = true;
			i = 0;
			continue;
		}
		if (stopAtFirst) break;
		i++;
	}
	return woke;
}

int sceKernelCreateSema(const char *name, u32 attr, s32 initVal, s32 maxVal, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (attr >= 0x200) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	auto *s = new Semaphore();
	s->name = TruncName(name);
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
	// The call's own time comes before a woken better thread gets the CPU: about 10us from the
	// call to that thread running, and from its next wait back to here (threads/scheduling/handoff).
	sched::ChargeCycles(900);
	bool woke = SemaWakeWaiters(s);
	if (woke) sched::Reschedule();
	return 0;
}

int WaitSema(SceUID id, s32 want, u32 timeoutPtr, bool cb) {
	// A signal and a wait that doesn't block take under 10us together (threads/scheduling/costs).
	sched::ChargeCycles(900);
	if (want <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	if (want > s->maxCount) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	PspThread *t = CurThread();
	// If there are callbacks to run, it always waits, and wakes after the callbacks.
	bool hasCallbacks = cb && HasPendingCallbacks(t) && CanRunCallbacks(t);
	if (s->count >= want && s->waiters.Count(WAITTYPE_SEMA, id) == 0 && !hasCallbacks) {
		s->count -= want;
		return 0;
	}
	if (!hasCallbacks && sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	t->waitValue[0] = (u32)want;
	s->waiters.Add(t->uid);
	SceUID me = t->uid;
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (Semaphore *sm = Get<Semaphore>(id)) sm->waiters.Remove(me);
	};
	hooks.resume = [id, me]() {
		PspThread *w = Get<PspThread>(me);
		Semaphore *sm = Get<Semaphore>(id);
		if (!sm) {
			if (w->waitTimeoutPtr && mem::IsValidRange(w->waitTimeoutPtr, 4)) GWrite32(w->waitTimeoutPtr, 0);
			w->waitTimeoutPtr = 0;
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		if (SemaUnlockFor(sm, w)) return true;
		sm->waiters.Add(me);
		return false;
	};
	hooks.timeout = [id, me]() {
		Semaphore *sm = Get<Semaphore>(id);
		if (!sm) return;
		sm->waiters.Remove(me);
		// In FIFO mode, a timed-out head may unblock the threads behind it.
		if ((sm->attr & PSP_SEMA_ATTR_PRIORITY) == 0) SemaWakeWaiters(sm, true);
	};
	return sched::Wait(WAITTYPE_SEMA, id, timeoutPtr, cb, &hooks);
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
	if (mem::IsValidAddress(numWaitPtr)) GWrite32(numWaitPtr, (u32)n);
	s->count = newCount < 0 ? s->initCount : newCount;
	if (s->waiters.WakeAll(WAITTYPE_SEMA, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL)) sched::Reschedule();
	return 0;
}

int sceKernelReferSemaStatus(SceUID id, u32 infoPtr) {
	Semaphore *s = Get<Semaphore>(id);
	if (!s) return (int)SCE_KERNEL_ERROR_UNKNOWN_SEMID;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 initCount, currentCount, maxCount, numWaitThreads;
	} info{};
	if (!mem::IsValidRange(infoPtr, sizeof(info))) return -1;
	info.size = sizeof(info);
	CopyName(info.name, s->name);
	info.attr = s->attr;
	info.initCount = s->initCount;
	info.currentCount = s->count;
	info.maxCount = s->maxCount;
	info.numWaitThreads = s->waiters.Count(WAITTYPE_SEMA, id);
	if (GRead32(infoPtr) != 0) mem::WriteBytes(infoPtr, &info, sizeof(info));
	return 0;
}

// ================================================================ event flags

struct EventFlag : KObject {
	static constexpr KType kType = KType::EventFlag;
	KType Type() const override { return kType; }
	u32 attr = 0, initPattern = 0, pattern = 0;
	WaitQueue waiters;  // waitValue[0] = bits, [1] = mode, [2] = out address
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
	if (outAddr && mem::IsValidRange(outAddr, 4) && (outAddr & 3) == 0) GWrite32(outAddr, pattern);
	if (mode & PSP_EVENT_WAITCLEAR) pattern &= ~bits;
	if (mode & PSP_EVENT_WAITCLEARALL) pattern = 0;
	return true;
}

// Ends a waiter's wait: with 0 if its pattern matches (applying the clear modes), or with an
// error result (which reports the current pattern). Returns true if it was woken.
bool EvfUnlockFor(EventFlag *e, PspThread *t, s32 result) {
	if (result == 0) {
		if (!EvfApply(e->pattern, t->waitValue[0], t->waitValue[1], t->waitValue[2])) return false;
	} else if (t->waitValue[2] && mem::IsValidRange(t->waitValue[2], 4)) {
		GWrite32(t->waitValue[2], e->pattern);
	}
	sched::Wake(t, result);
	return true;
}

bool EvfWakeAll(EventFlag *e, s32 result) {
	e->waiters.Cleanup(WAITTYPE_EVENTFLAG, e->uid);
	std::vector<SceUID> copy;
	copy.swap(e->waiters.threads);
	bool woke = false;
	for (SceUID u : copy)
		if (PspThread *t = Get<PspThread>(u)) woke |= EvfUnlockFor(e, t, result);
	return woke;
}

int sceKernelCreateEventFlag(const char *name, u32 attr, u32 initPattern, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if ((attr & 0x100) != 0 || attr >= 0x300) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	auto *e = new EventFlag();
	e->name = TruncName(name);
	e->attr = attr;
	e->initPattern = e->pattern = initPattern;
	(void)optPtr;
	return Register(e);
}

u32 sceKernelDeleteEventFlag(SceUID id) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	bool woke = EvfWakeAll(e, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
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
		if (t && EvfUnlockFor(e, t, 0)) {
			e->waiters.threads.erase(e->waiters.threads.begin() + i);
			woke = true;
		} else {
			i++;
		}
	}
	sched::ChargeCycles(430);
	if (woke) sched::Reschedule();
	return 0;
}

u32 sceKernelClearEventFlag(SceUID id, u32 bits) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	e->pattern &= bits;
	sched::ChargeCycles(430);
	return 0;
}

int WaitEventFlag(SceUID id, u32 bits, u32 mode, u32 outBitsPtr, u32 timeoutPtr, bool cb) {
	if ((mode & ~PSP_EVENT_WAITKNOWN) != 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	// Can't wait on 0, that's guaranteed to wait forever.
	if (bits == 0) return (int)SCE_KERNEL_ERROR_EVF_ILPAT;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return (int)SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	PspThread *t = CurThread();
	bool hasCallbacks = cb && HasPendingCallbacks(t) && CanRunCallbacks(t);
	// Only check, don't apply, when callbacks may run first: the CLEAR modes apply after them.
	bool doWait = hasCallbacks ? true : !EvfMatches(e->pattern, bits, mode);
	if (!doWait) {
		EvfApply(e->pattern, bits, mode, outBitsPtr);
		sched::ChargeCycles(500);
		return 0;
	}
	// A thread left in the list after a timeout must not count against WAITMULTIPLE.
	e->waiters.Remove(t->uid);
	e->waiters.Cleanup(WAITTYPE_EVENTFLAG, id);
	if (!e->waiters.threads.empty() && (e->attr & PSP_EVENT_WAITMULTIPLE) == 0)
		return (int)SCE_KERNEL_ERROR_EVF_MULTI;
	u32 timeout = 0xFFFFFFFF;
	if (timeoutPtr && mem::IsValidRange(timeoutPtr, 4)) timeout = GRead32(timeoutPtr);
	if (!hasCallbacks && sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	t->waitValue[0] = bits;
	t->waitValue[1] = mode;
	// With a zero timeout, hardware doesn't reliably write the bits back.
	t->waitValue[2] = timeout == 0 ? 0 : outBitsPtr;
	e->waiters.Add(t->uid);
	SceUID me = t->uid;
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (EventFlag *ev = Get<EventFlag>(id)) ev->waiters.Remove(me);
	};
	hooks.resume = [id, me]() {
		PspThread *w = Get<PspThread>(me);
		EventFlag *ev = Get<EventFlag>(id);
		if (!ev) {
			if (w->waitTimeoutPtr && mem::IsValidRange(w->waitTimeoutPtr, 4)) GWrite32(w->waitTimeoutPtr, 0);
			w->waitTimeoutPtr = 0;
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		if (EvfUnlockFor(ev, w, 0)) return true;
		ev->waiters.Add(me);
		return false;
	};
	hooks.timeout = [id, me]() {
		PspThread *w = Get<PspThread>(me);
		EventFlag *ev = Get<EventFlag>(id);
		if (!ev || !w) return;
		ev->waiters.Remove(me);
		if (w->waitValue[2] && mem::IsValidRange(w->waitValue[2], 4)) GWrite32(w->waitValue[2], ev->pattern);
	};
	int r = sched::Wait(WAITTYPE_EVENTFLAG, id, timeoutPtr, cb, &hooks);
	sched::ChargeCycles(500);
	return r;
}

int sceKernelWaitEventFlag(SceUID id, u32 bits, u32 mode, u32 outBitsPtr, u32 timeoutPtr) {
	return WaitEventFlag(id, bits, mode, outBitsPtr, timeoutPtr, false);
}
int sceKernelWaitEventFlagCB(SceUID id, u32 bits, u32 mode, u32 outBitsPtr, u32 timeoutPtr) {
	return WaitEventFlag(id, bits, mode, outBitsPtr, timeoutPtr, true);
}

int sceKernelPollEventFlag(SceUID id, u32 bits, u32 mode, u32 outBitsPtr) {
	if ((mode & ~PSP_EVENT_WAITKNOWN) != 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	// Poll also fails when CLEAR and CLEARALL are used together, unlike wait.
	if ((mode & PSP_EVENT_WAITCLEARALL) && (mode & PSP_EVENT_WAITCLEAR)) return (int)SCE_KERNEL_ERROR_ILLEGAL_MODE;
	if (bits == 0) return (int)SCE_KERNEL_ERROR_EVF_ILPAT;
	sched::ChargeCycles(360);
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return (int)SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	if (EvfApply(e->pattern, bits, mode, outBitsPtr)) return 0;
	if (outBitsPtr && mem::IsValidRange(outBitsPtr, 4) && (outBitsPtr & 3) == 0) GWrite32(outBitsPtr, e->pattern);
	if (e->waiters.Count(WAITTYPE_EVENTFLAG, id) > 0 && (e->attr & PSP_EVENT_WAITMULTIPLE) == 0)
		return (int)SCE_KERNEL_ERROR_EVF_MULTI;
	return (int)SCE_KERNEL_ERROR_EVF_COND;
}

u32 sceKernelCancelEventFlag(SceUID id, u32 pattern, u32 numWaitPtr) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	int n = e->waiters.Count(WAITTYPE_EVENTFLAG, id);
	if (mem::IsValidRange(numWaitPtr, 4) && (numWaitPtr & 3) == 0) GWrite32(numWaitPtr, (u32)n);
	e->pattern = pattern;
	sched::ChargeCycles(580);
	if (EvfWakeAll(e, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL)) sched::Reschedule();
	return 0;
}

u32 sceKernelReferEventFlagStatus(SceUID id, u32 infoPtr) {
	EventFlag *e = Get<EventFlag>(id);
	if (!e) return SCE_KERNEL_ERROR_UNKNOWN_EVFID;
	struct {
		u32 size;
		char name[32];
		u32 attr, initPattern, currentPattern;
		s32 numWaitThreads;
	} info{};
	if (!mem::IsValidRange(infoPtr, sizeof(info))) return (u32)-1;
	info.size = sizeof(info);
	CopyName(info.name, e->name);
	info.attr = e->attr;
	info.initPattern = e->initPattern;
	info.currentPattern = e->pattern;
	info.numWaitThreads = e->waiters.Count(WAITTYPE_EVENTFLAG, id);
	if (GRead32(infoPtr) != 0) mem::WriteBytes(infoPtr, &info, sizeof(info));
	return 0;
}

// ================================================================ mutexes

struct Mutex : KObject {
	static constexpr KType kType = KType::Mutex;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 initCount = 0, lockLevel = 0;
	SceUID lockThread = -1;
	WaitQueue waiters;  // waitValue[0] = count wanted
};

constexpr u32 PSP_MUTEX_ATTR_PRIORITY = 0x100;
constexpr u32 PSP_MUTEX_ATTR_ALLOW_RECURSIVE = 0x200;

// Thread -> mutexes it holds, so they can be released when it ends.
std::unordered_multimap<SceUID, SceUID> g_mutexHeld;

void MutexAcquire(Mutex *m, s32 count, SceUID thread) {
	g_mutexHeld.emplace(thread, m->uid);
	m->lockLevel = count;
	m->lockThread = thread;
}

void MutexEraseLock(Mutex *m) {
	if (m->lockThread != -1) {
		auto range = g_mutexHeld.equal_range(m->lockThread);
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second == m->uid) {
				g_mutexHeld.erase(it);
				break;
			}
		}
	}
	m->lockThread = -1;
}

// Ends a waiter's wait: acquiring the mutex for it (result 0) or with an error.
bool MutexUnlockFor(Mutex *m, PspThread *t, s32 result) {
	if (!WaitQueue::IsWaitingOn(t, WAITTYPE_MUTEX, m->uid)) return false;
	if (result == 0) MutexAcquire(m, (s32)t->waitValue[0], t->uid);
	sched::Wake(t, result);
	return true;
}

// The lock level dropped to 0: hand it to the next waiter. Returns true if one woke.
bool MutexUnlock(Mutex *m) {
	MutexEraseLock(m);
	bool woke = false;
	while (!woke && !m->waiters.threads.empty()) {
		size_t idx = 0;
		if (m->attr & PSP_MUTEX_ATTR_PRIORITY) {
			int best = m->waiters.BestPriorityIndex();
			idx = best < 0 ? 0 : (size_t)best;
		}
		PspThread *t = Get<PspThread>(m->waiters.threads[idx]);
		woke |= t && MutexUnlockFor(m, t, 0);
		m->waiters.threads.erase(m->waiters.threads.begin() + idx);
	}
	if (!woke) m->lockThread = -1;
	return woke;
}

// Returns true if the current thread can take the lock now; sets *error on failure.
bool MutexLockCheck(Mutex *m, s32 count, u32 *error) {
	if (*error) return false;
	bool recursive = (m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) != 0;
	SceUID cur = sched::CurrentId();
	if (count <= 0) *error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	else if (count > 1 && !recursive) *error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	// Two positive ints always overflow to negative.
	else if ((s32)((u32)count + (u32)m->lockLevel) < 0) *error = SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW;
	// Only a recursive mutex can re-lock.
	else if (m->lockThread == cur) {
		if (recursive) return true;
		*error = SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED;
	} else if (m->lockLevel == 0) {
		return true;
	}
	return false;
}

bool MutexTryLock(Mutex *m, s32 count, u32 *error) {
	if (!MutexLockCheck(m, count, error)) return false;
	if (m->lockLevel == 0) {
		MutexAcquire(m, count, sched::CurrentId());
		return true;
	}
	if (m->lockThread == sched::CurrentId()) {
		m->lockLevel += count;
		return true;
	}
	return false;
}

int sceKernelCreateMutex(const char *name, u32 attr, s32 initCount, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (attr & ~0xBFF) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (initCount < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && initCount > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	auto *m = new Mutex();
	m->name = TruncName(name);
	m->attr = attr;
	m->initCount = initCount;
	SceUID id = Register(m);
	if (initCount > 0) MutexAcquire(m, initCount, sched::CurrentId());
	(void)optPtr;
	return id;
}

int sceKernelDeleteMutex(SceUID id) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	bool woke = false;
	std::vector<SceUID> copy = m->waiters.threads;
	for (SceUID u : copy)
		if (PspThread *t = Get<PspThread>(u)) woke |= MutexUnlockFor(m, t, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	if (m->lockThread != -1) MutexEraseLock(m);
	m->waiters.threads.clear();
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

WaitHooks MutexHooks(SceUID id, SceUID me) {
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (Mutex *m = Get<Mutex>(id)) m->waiters.Remove(me);
	};
	hooks.resume = [id, me]() {
		PspThread *w = Get<PspThread>(me);
		Mutex *m = Get<Mutex>(id);
		if (!m) {
			if (w->waitTimeoutPtr && mem::IsValidRange(w->waitTimeoutPtr, 4)) GWrite32(w->waitTimeoutPtr, 0);
			w->waitTimeoutPtr = 0;
			sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		if (m->lockThread == -1 && MutexUnlockFor(m, w, 0)) return true;
		m->waiters.Add(me);
		return false;
	};
	hooks.timeout = [id, me]() {
		if (Mutex *m = Get<Mutex>(id)) m->waiters.Remove(me);
	};
	return hooks;
}

int LockMutex(SceUID id, s32 count, u32 timeoutPtr, bool cb) {
	Mutex *m = Get<Mutex>(id);
	u32 error = m ? 0 : SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	PspThread *t = CurThread();
	if (!cb) {
		if (MutexTryLock(m, count, &error)) return 0;
		if (error) return (int)error;
	} else {
		if (MutexLockCheck(m, count, &error)) {
			if (!(HasPendingCallbacks(t) && CanRunCallbacks(t))) {
				MutexTryLock(m, count, &error);
				return 0;
			}
			// Callbacks run first; the lock is taken (or waited for) after them.
		} else if (error) {
			return (int)error;
		}
	}
	bool hasCallbacks = cb && HasPendingCallbacks(t) && CanRunCallbacks(t);
	if (!hasCallbacks && sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	t->waitValue[0] = (u32)count;
	m->waiters.Add(t->uid);
	WaitHooks hooks = MutexHooks(id, t->uid);
	return sched::Wait(WAITTYPE_MUTEX, id, timeoutPtr, cb, &hooks);
}

int sceKernelLockMutex(SceUID id, s32 count, u32 timeoutPtr) {
	// This ID is always invalid; Tekken 6 calls it, as on hardware.
	if (id == (SceUID)0x80020001 && timeoutPtr == 0) return 0;
	return LockMutex(id, count, timeoutPtr, false);
}
int sceKernelLockMutexCB(SceUID id, s32 count, u32 timeoutPtr) { return LockMutex(id, count, timeoutPtr, true); }

int sceKernelTryLockMutex(SceUID id, s32 count) {
	Mutex *m = Get<Mutex>(id);
	u32 error = m ? 0 : SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	if (MutexTryLock(m, count, &error)) return 0;
	return error ? (int)error : (int)SCE_KERNEL_ERROR_MUTEX_LOCKED;
}

int sceKernelUnlockMutex(SceUID id, s32 count) {
	if (id == (SceUID)0x80020001) return 0;
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	if (count <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((m->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && count > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (m->lockLevel == 0 || m->lockThread != sched::CurrentId()) return (int)SCE_KERNEL_ERROR_MUTEX_UNLOCKED;
	if (m->lockLevel < count) return (int)SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW;
	m->lockLevel -= count;
	if (m->lockLevel == 0) {
		m->waiters.Cleanup(WAITTYPE_MUTEX, id);
		if (MutexUnlock(m)) sched::Reschedule();
	}
	return 0;
}

int sceKernelCancelMutex(SceUID id, s32 count, u32 numWaitPtr) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	u32 error = 0;
	bool lockable = count <= 0 || MutexLockCheck(m, count, &error);
	if (!lockable && error != 0 && error != SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW &&
	    error != SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED)
		return (int)error;
	m->waiters.Cleanup(WAITTYPE_MUTEX, id);
	if (mem::IsValidRange(numWaitPtr, 4) && (numWaitPtr & 3) == 0) GWrite32(numWaitPtr, (u32)m->waiters.threads.size());
	bool woke = false;
	std::vector<SceUID> copy = m->waiters.threads;
	for (SceUID u : copy)
		if (PspThread *t = Get<PspThread>(u)) woke |= MutexUnlockFor(m, t, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	if (m->lockThread != -1) MutexEraseLock(m);
	m->waiters.threads.clear();
	if (count <= 0) {
		m->lockLevel = 0;
		m->lockThread = -1;
	} else {
		MutexAcquire(m, count, sched::CurrentId());
	}
	if (woke) sched::Reschedule();
	return 0;
}

int sceKernelReferMutexStatus(SceUID id, u32 infoPtr) {
	Mutex *m = Get<Mutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MUTEXID;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 initCount, lockLevel;
		SceUID lockThread;
		s32 numWaitThreads;
	} info{};
	if (!mem::IsValidRange(infoPtr, sizeof(info))) return -1;
	if (GRead32(infoPtr) == 0) return 0;
	info.size = sizeof(info);
	CopyName(info.name, m->name);
	info.attr = m->attr;
	info.initCount = m->initCount;
	info.lockLevel = m->lockLevel;
	info.lockThread = m->lockThread;
	info.numWaitThreads = m->waiters.Count(WAITTYPE_MUTEX, id);
	mem::WriteBytes(infoPtr, &info, sizeof(info));
	return 0;
}

// A thread that ends releases every mutex it holds, and stops waiting.
void MutexThreadEnd(PspThread *t) {
	if (t->waitType == WAITTYPE_MUTEX) {
		if (Mutex *m = Get<Mutex>(t->waitId)) m->waiters.Remove(t->uid);
	}
	auto range = g_mutexHeld.equal_range(t->uid);
	std::vector<SceUID> held;
	for (auto it = range.first; it != range.second; ++it) held.push_back(it->second);
	for (SceUID mid : held) {
		if (Mutex *m = Get<Mutex>(mid)) {
			m->lockLevel = 0;
			m->waiters.Cleanup(WAITTYPE_MUTEX, mid);
			MutexUnlock(m);
		}
	}
	g_mutexHeld.erase(t->uid);
}

}  // namespace

void kernel::RegisterThreadManSync() {
	AddThreadEndListener(&MutexThreadEnd);

	constexpr u32 NI = KF_NOT_IN_INTERRUPT;
	constexpr u32 NID = KF_NOT_IN_INTERRUPT | KF_NOT_DISPATCH_SUSPENDED;
	static const hle::HleFunction funcs[] = {
		HLEFN(0x55C20A00, sceKernelCreateEventFlag),
		HLEFN(0x812346E4, sceKernelClearEventFlag),
		HLEFN(0xEF9E4C70, sceKernelDeleteEventFlag),
		HLEFN(0x1FB15A32, sceKernelSetEventFlag),
		KFN(0x402FCF22, sceKernelWaitEventFlag, NI),
		KFN(0x328C546A, sceKernelWaitEventFlagCB, NI),
		HLEFN(0x30FD48F0, sceKernelPollEventFlag),
		HLEFN(0xCD203292, sceKernelCancelEventFlag),
		HLEFN(0xA66B0120, sceKernelReferEventFlagStatus),

		HLEFN(0x8FFDF9A2, sceKernelCancelSema),
		HLEFN(0xD6DA4BA1, sceKernelCreateSema),
		HLEFN(0x28B6489C, sceKernelDeleteSema),
		HLEFN(0x58B1F937, sceKernelPollSema),
		HLEFN(0xBC6FEBC5, sceKernelReferSemaStatus),
		HLEFN(0x3F53E640, sceKernelSignalSema),
		KFN(0x4E3A1105, sceKernelWaitSema, NID),
		KFN(0x6D212BAC, sceKernelWaitSemaCB, NID),

		HLEFN(0xF8170FBE, sceKernelDeleteMutex),
		KFN(0xB011B11F, sceKernelLockMutex, NID),
		KFN(0x5BF4DD27, sceKernelLockMutexCB, NID),
		HLEFN(0x6B30100F, sceKernelUnlockMutex),
		HLEFN(0xB7D098C6, sceKernelCreateMutex),
		HLEFN(0x0DDCD2C9, sceKernelTryLockMutex),
		HLEFN(0xA9C2CB9A, sceKernelReferMutexStatus),
		HLEFN(0x87D9223C, sceKernelCancelMutex),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);
}
