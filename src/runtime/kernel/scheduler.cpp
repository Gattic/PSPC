// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/kernel/scheduler.h"

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/interp.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/sysmem.h"

extern u32 *g_intrEnabled;

namespace sched {

using namespace kernel;

namespace {

constexpr s32 kBudget = 20000;
constexpr int kNumPrio = 128;

struct Event {
	u64 time;
	u64 id;
	std::function<void()> fn;
};
struct EventCmp {
	bool operator()(const Event &a, const Event &b) const {
		return a.time > b.time || (a.time == b.time && a.id > b.id);
	}
};

void *g_mainFiber = nullptr;
PspThread *g_current = nullptr;
std::deque<PspThread *> g_ready[kNumPrio];
int g_readyCount = 0;

bool g_stop = false;
int g_exitCode = 0;
bool g_fastTime = false;
LARGE_INTEGER g_qpcFreq, g_qpcStart;
u64 g_timeOffset = 0;

std::priority_queue<Event, std::vector<Event>, EventCmp> g_events;
std::unordered_set<u64> g_cancelled;
u64 g_nextEventId = 1;

std::mutex g_hostMutex;
std::vector<std::function<void()>> g_hostPosts;
std::atomic<bool> g_hostPending{false};
HANDLE g_wakeEvent = nullptr;
HANDLE g_timer = nullptr;

bool g_dispatchEnabled = true;
bool g_inInterrupt = false;
std::deque<std::function<void()>> g_interrupts;
alignas(64) Ctx g_intrCtx;
u32 g_intrStackTop = 0;

std::vector<void *> g_deadFibers;
std::vector<PspThread *> g_pendingDelete;

int PrioIndex(s32 prio) { return prio < 0 ? 0 : (prio >= kNumPrio ? kNumPrio - 1 : prio); }

int BestReadyPrio() {
	if (!g_readyCount) return 0x7FFFFFFF;
	for (int p = 0; p < kNumPrio; p++)
		if (!g_ready[p].empty()) return p;
	return 0x7FFFFFFF;
}

PspThread *PopBestReady() {
	for (int p = 0; p < kNumPrio; p++) {
		if (!g_ready[p].empty()) {
			PspThread *t = g_ready[p].front();
			g_ready[p].pop_front();
			g_readyCount--;
			t->inReadyQueue = false;
			return t;
		}
	}
	return nullptr;
}

void SwitchToScheduler() {
	PspThread *t = g_current;
	SwitchToFiber(g_mainFiber);
	// Resumed by the scheduler.
	(void)t;
}

u64 NextEventTime() {
	while (!g_events.empty() && g_cancelled.count(g_events.top().id)) {
		g_cancelled.erase(g_events.top().id);
		g_events.pop();
	}
	return g_events.empty() ? UINT64_MAX : g_events.top().time;
}

void ProcessDueEvents() {
	u64 now = NowUs();
	for (;;) {
		u64 next = NextEventTime();
		if (next > now) break;
		Event e = g_events.top();
		g_events.pop();
		e.fn();
		if (g_stop) return;
	}
}

void DrainHostPosts() {
	if (!g_hostPending.load(std::memory_order_acquire)) return;
	std::vector<std::function<void()>> posts;
	{
		std::lock_guard<std::mutex> lock(g_hostMutex);
		posts.swap(g_hostPosts);
		g_hostPending = false;
	}
	for (auto &fn : posts) fn();
}

void RunInterrupts() {
	while (!g_interrupts.empty() && *g_intrEnabled) {
		auto fn = std::move(g_interrupts.front());
		g_interrupts.pop_front();
		fn();
	}
}

void FreeDeadFibers() {
	for (void *f : g_deadFibers) DeleteFiber(f);
	g_deadFibers.clear();
	for (PspThread *t : g_pendingDelete) {
		RemoveFromReady(t);
		kernel::Destroy(t->uid);
	}
	g_pendingDelete.clear();
}

void RunThread(PspThread *t) {
	g_current = t;
	t->status = (t->status & ~(THREADSTATUS_READY | THREADSTATUS_WAIT)) | THREADSTATUS_RUNNING;
	cpu::SetCurrentCtx(&t->ctx);
	t->ctx.budget = kBudget;
	SwitchToFiber(t->fiber);
	g_current = nullptr;
	cpu::SetCurrentCtx(nullptr);
	FreeDeadFibers();
}

void IdleUntil(u64 next) {
	u64 now = NowUs();
	if (next == UINT64_MAX) {
		WaitForSingleObject(g_wakeEvent, 50);
		return;
	}
	if (next <= now) return;
	u64 delta = next - now;
	if (g_fastTime) {
		g_timeOffset += delta;
		return;
	}
	if (delta > 1500) {
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)((delta - 500) * 10);
		SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
		HANDLE hs[2] = {g_timer, g_wakeEvent};
		WaitForMultipleObjects(2, hs, FALSE, INFINITE);
		return;
	}
	while (NowUs() < next && !g_hostPending.load(std::memory_order_relaxed)) YieldProcessor();
}

void WINAPI ThreadFiberProc(void *param) {
	PspThread *t = (PspThread *)param;
	Ctx *c = &t->ctx;
	c->r[31] = t->threadReturnStub;
	c->unwinding = 0;
	dispatch::Call(c, t->entry);
	while (c->unwinding) {
		u32 target = c->pc;
		c->unwinding = 0;
		if (target == t->threadReturnStub) break;
		interp::Run(c, target, t->threadReturnStub, 0);
	}
	ExitCurrentThread((s32)c->r[2], false);
}

s32 DoWait(PspThread *t, WaitType type, SceUID id, u64 deadline, u32 timeoutPtr, bool cb, s32 timeoutResult) {
	t->waitType = type;
	t->waitId = id;
	t->waitDone = false;
	t->waitResult = 0;
	t->waitCallbacks = cb;
	t->waitTimeoutPtr = timeoutPtr;
	t->waitDeadline = deadline;
	u32 seq = ++t->waitSeq;
	if (deadline) {
		SceUID uid = t->uid;
		t->timeoutEventId = ScheduleEvent(deadline, [uid, seq, timeoutResult, timeoutPtr]() {
			PspThread *w = kernel::Get<PspThread>(uid);
			if (!w || w->waitSeq != seq || w->waitDone) return;
			w->timeoutEventId = 0;
			if (timeoutPtr && mem::IsValidRange(timeoutPtr, 4)) GWrite32(timeoutPtr, 0);
			if (w->timeoutHook) w->timeoutHook();
			Wake(w, timeoutResult);
		});
	}
	for (;;) {
		if (cb && kernel::HasPendingCallbacks(t)) {
			kernel::RunPendingCallbacks(t);
			if (t->waitDone) break;
		}
		if (t->waitDone) break;
		t->status = (t->status & THREADSTATUS_SUSPEND) | THREADSTATUS_WAIT;
		SwitchToScheduler();
		if (t->waitDone) break;
	}
	if (t->timeoutEventId) {
		CancelEvent(t->timeoutEventId);
		t->timeoutEventId = 0;
	}
	if (timeoutPtr && deadline && t->waitResult != timeoutResult && mem::IsValidRange(timeoutPtr, 4)) {
		u64 now = NowUs();
		GWrite32(timeoutPtr, deadline > now ? (u32)(deadline - now) : 0);
	}
	t->timeoutHook = nullptr;
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	return t->waitResult;
}

}  // namespace

void Init(bool fastTime) {
	g_fastTime = fastTime;
	QueryPerformanceFrequency(&g_qpcFreq);
	QueryPerformanceCounter(&g_qpcStart);
	g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
	timeBeginPeriod(1);
	memset(&g_intrCtx, 0, sizeof(g_intrCtx));
	u32 stack = kernel::AllocKernelMemory(0x4000, "interrupt stack");
	g_intrStackTop = stack + 0x4000 - 0x10;
}

void Shutdown() {
	timeEndPeriod(1);
}

u64 NowUs() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	u64 ticks = (u64)(now.QuadPart - g_qpcStart.QuadPart);
	return (ticks / (u64)g_qpcFreq.QuadPart) * 1000000ull + ((ticks % (u64)g_qpcFreq.QuadPart) * 1000000ull) / (u64)g_qpcFreq.QuadPart +
	       g_timeOffset;
}

u64 ScheduleEvent(u64 atUs, std::function<void()> fn) {
	u64 id = g_nextEventId++;
	g_events.push(Event{atUs, id, std::move(fn)});
	return id;
}

void CancelEvent(u64 id) {
	if (id) g_cancelled.insert(id);
}

void PostFromHost(std::function<void()> fn) {
	{
		std::lock_guard<std::mutex> lock(g_hostMutex);
		g_hostPosts.push_back(std::move(fn));
		g_hostPending = true;
	}
	SetEvent(g_wakeEvent);
}

int Run() {
	g_mainFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
	if (!g_mainFiber) Fatal("ConvertThreadToFiber failed (%lu)", GetLastError());
	while (!g_stop) {
		DrainHostPosts();
		ProcessDueEvents();
		RunInterrupts();
		if (g_stop) break;
		PspThread *t = PopBestReady();
		if (t) {
			RunThread(t);
			continue;
		}
		IdleUntil(NextEventTime());
	}
	ConvertFiberToThread();
	return g_exitCode;
}

void RequestStop(int exitCode) {
	if (!g_stop) g_exitCode = exitCode;
	g_stop = true;
	if (g_current && GetCurrentFiber() != g_mainFiber) {
		// Called from a guest thread: leave it for good.
		SwitchToFiber(g_mainFiber);
	}
}

bool StopRequested() { return g_stop; }

PspThread *Current() { return g_current; }
SceUID CurrentId() { return g_current ? g_current->uid : 0; }

void MakeReady(PspThread *t, bool front) {
	if (t->inReadyQueue) return;
	t->status = (t->status & ~(THREADSTATUS_RUNNING | THREADSTATUS_WAIT | THREADSTATUS_DORMANT)) | THREADSTATUS_READY;
	if (t->status & THREADSTATUS_SUSPEND) return;
	auto &q = g_ready[PrioIndex(t->currentPriority)];
	if (front) q.push_front(t);
	else q.push_back(t);
	t->inReadyQueue = true;
	g_readyCount++;
}

void RemoveFromReady(PspThread *t) {
	if (!t->inReadyQueue) return;
	for (auto &q : g_ready) {
		for (auto it = q.begin(); it != q.end(); ++it) {
			if (*it == t) {
				q.erase(it);
				g_readyCount--;
				t->inReadyQueue = false;
				return;
			}
		}
	}
	t->inReadyQueue = false;
}

void LaunchThread(PspThread *t) {
	if (t->fiber) {
		g_deadFibers.push_back(t->fiber);
		t->fiber = nullptr;
	}
	t->fiber = CreateFiberEx(64 * 1024, 8 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, ThreadFiberProc, t);
	if (!t->fiber) Fatal("CreateFiberEx failed (%lu)", GetLastError());
	t->ctx.thread = t;
	t->status = THREADSTATUS_READY;
	MakeReady(t);
}

void ChangePriority(PspThread *t, s32 prio) {
	bool queued = t->inReadyQueue;
	if (queued) RemoveFromReady(t);
	t->currentPriority = prio;
	if (queued) MakeReady(t);
}

void RotateReadyQueue(s32 prio) {
	auto &q = g_ready[PrioIndex(prio)];
	if (q.size() > 1) {
		PspThread *t = q.front();
		q.pop_front();
		q.push_back(t);
	}
}

void Suspend(PspThread *t) {
	t->status |= THREADSTATUS_SUSPEND;
	if (t->inReadyQueue) RemoveFromReady(t);
	if (t->status & THREADSTATUS_READY) t->status &= ~THREADSTATUS_READY;
}

void Resume(PspThread *t) {
	t->status &= ~THREADSTATUS_SUSPEND;
	if (!(t->status & (THREADSTATUS_WAIT | THREADSTATUS_DORMANT | THREADSTATUS_DEAD | THREADSTATUS_RUNNING))) {
		MakeReady(t);
	}
}

s32 Wait(WaitType type, SceUID id, u32 timeoutPtr, bool processCallbacks) {
	PspThread *t = g_current;
	if (!t || g_inInterrupt) return (s32)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!g_dispatchEnabled) return (s32)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	u64 deadline = 0;
	if (timeoutPtr && mem::IsValidRange(timeoutPtr, 4)) {
		u32 us = GRead32(timeoutPtr);
		deadline = NowUs() + us;
	} else {
		timeoutPtr = 0;
	}
	return DoWait(t, type, id, deadline, timeoutPtr, processCallbacks, (s32)SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

s32 WaitUntil(WaitType type, SceUID id, u64 deadlineUs, bool processCallbacks) {
	PspThread *t = g_current;
	if (!t || g_inInterrupt) return (s32)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!g_dispatchEnabled) return (s32)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	return DoWait(t, type, id, deadlineUs ? deadlineUs : 1, 0, processCallbacks, 0);
}

void Wake(PspThread *t, s32 result) {
	if (t->waitDone || t->waitType == WAITTYPE_NONE) return;
	t->waitDone = true;
	t->waitResult = result;
	if (t->timeoutEventId) {
		CancelEvent(t->timeoutEventId);
		t->timeoutEventId = 0;
	}
	if (t == g_current) return;  // woken while running callbacks inside its own wait
	t->status &= ~THREADSTATUS_WAIT;
	if (!(t->status & THREADSTATUS_SUSPEND)) MakeReady(t);
}

void Reschedule() {
	PspThread *t = g_current;
	if (!t || g_inInterrupt || !g_dispatchEnabled) return;
	if (BestReadyPrio() < t->currentPriority) {
		t->threadPreemptCount++;
		MakeReady(t, true);
		SwitchToScheduler();
	}
}

void YieldToSamePriority() {
	PspThread *t = g_current;
	if (!t || g_inInterrupt || !g_dispatchEnabled) return;
	if (BestReadyPrio() <= t->currentPriority) {
		MakeReady(t, false);
		SwitchToScheduler();
	}
}

void YieldCheck(Ctx *c) {
	c->budget = kBudget;
	PspThread *t = g_current;
	if (!t || g_inInterrupt || !g_dispatchEnabled || &t->ctx != c) return;
	bool due = g_hostPending.load(std::memory_order_relaxed) || !g_interrupts.empty() || NextEventTime() <= NowUs();
	if (due || BestReadyPrio() < t->currentPriority) {
		if (due) t->intrPreemptCount++;
		MakeReady(t, true);
		SwitchToScheduler();
	}
}

void ExitCurrentThread(s32 status, bool deleteAfter) {
	PspThread *t = g_current;
	if (!t) Fatal("ExitCurrentThread outside a thread");
	t->exitStatus = status;
	t->status = THREADSTATUS_DORMANT;
	t->waitType = WAITTYPE_NONE;
	for (SceUID w : t->endWaiters) {
		PspThread *wt = kernel::Get<PspThread>(w);
		if (wt && wt->waitType == WAITTYPE_THREADEND && wt->waitId == t->uid) Wake(wt, status);
	}
	t->endWaiters.clear();
	g_deadFibers.push_back(t->fiber);
	t->fiber = nullptr;
	if (deleteAfter) g_pendingDelete.push_back(t);
	SwitchToFiber(g_mainFiber);
	Fatal("Exited thread resumed");
}

void TerminateThread(PspThread *t, s32 status) {
	if (t == g_current) ExitCurrentThread(status, false);
	RemoveFromReady(t);
	if (t->timeoutEventId) {
		CancelEvent(t->timeoutEventId);
		t->timeoutEventId = 0;
	}
	t->waitSeq++;
	t->waitType = WAITTYPE_NONE;
	t->exitStatus = status;
	t->status = THREADSTATUS_DORMANT;
	for (SceUID w : t->endWaiters) {
		PspThread *wt = kernel::Get<PspThread>(w);
		if (wt && wt->waitType == WAITTYPE_THREADEND && wt->waitId == t->uid) Wake(wt, status);
	}
	t->endWaiters.clear();
	if (t->fiber) {
		g_deadFibers.push_back(t->fiber);
		t->fiber = nullptr;
	}
}

void DeleteThread(PspThread *t) {
	if (t == g_current) {
		// Only reachable for a running thread through ExitDeleteThread, which defers it.
		g_pendingDelete.push_back(t);
		return;
	}
	RemoveFromReady(t);
	if (t->fiber) {
		g_deadFibers.push_back(t->fiber);
		t->fiber = nullptr;
	}
	kernel::Destroy(t->uid);
}

void NotifyCallbackPending(PspThread *t) {
	if (t == g_current) return;  // it will see them at its next check point
	if (t->IsWaiting() && t->waitCallbacks && !t->waitDone && !t->IsSuspended()) {
		t->status &= ~THREADSTATUS_WAIT;
		MakeReady(t);
	}
}

bool DispatchEnabled() { return g_dispatchEnabled; }
void SetDispatchEnabled(bool enabled) { g_dispatchEnabled = enabled; }
bool InInterrupt() { return g_inInterrupt; }

u32 RunInterruptHandler(u32 func, u32 gp, std::initializer_list<u32> args) {
	bool prevIntr = g_inInterrupt;
	Ctx *prevCtx = cpu::CurrentCtx();
	g_inInterrupt = true;
	g_intrCtx.r[29] = g_intrStackTop;
	g_intrCtx.r[28] = gp;
	g_intrCtx.fcr31 = 0x00000E00;
	cpu::SetCurrentCtx(&g_intrCtx);
	u32 result = cpu::CallGuest(&g_intrCtx, func, args);
	cpu::SetCurrentCtx(prevCtx);
	g_inInterrupt = prevIntr;
	return result;
}

void QueueInterrupt(std::function<void()> fn) { g_interrupts.push_back(std::move(fn)); }

}  // namespace sched
