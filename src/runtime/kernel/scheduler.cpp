// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scheduling and wait semantics follow PPSSPP's sceKernelThread.cpp and KernelWaitHelpers.h,
// ported from its asynchronous begin/end-callback model to blocking fiber waits; the timing
// constants (wait timeout deadline/latency, delay rounding, "times out at once" odds, context
// switch and call costs) are PPSSPP's, measured against pspautotests on hardware.
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later.
#include "runtime/kernel/scheduler.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
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
#include "runtime/kernel/threads.h"

extern u32 *g_intrEnabled;

namespace sched {

using namespace kernel;

namespace {

constexpr s32 kBudget = 20000;
constexpr int kNumPrio = 128;
constexpr u64 kCyclesPerUs = kCpuHz / 1000000;
// How long after its deadline a wait's timeout goes off, and how far into the call the deadline
// is taken (PPSSPP WAIT_TIMEOUT_LATENCY_US / WAIT_TIMEOUT_DEADLINE_US).
constexpr u32 kWaitTimeoutLatencyUs = 18;
constexpr u32 kWaitTimeoutDeadlineUs = 12;
// A switch between two threads costs about 5us (pspautotests threads/scheduling/handoff).
constexpr s64 kSwitchCycles = 1150;
constexpr s64 kThreadExitCycles = 3300;

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
PspThread *g_lastRun = nullptr;  // last thread that had the CPU (for switch costs)
std::deque<PspThread *> g_ready[kNumPrio];
int g_readyCount = 0;
// Set when the current thread switched to the scheduler only so it could run due events while
// dispatch is suspended: it gets the CPU back whatever else is ready.
PspThread *g_returnTo = nullptr;
// sceKernelStartThread hands the CPU straight to a better new thread.
SceUID g_handoff = 0;

bool g_stop = false;
int g_exitCode = 0;
bool g_fastTime = false;
LARGE_INTEGER g_qpcFreq, g_qpcStart;
u64 g_timeOffset = 0;
DWORD g_emuThreadId = 0;

// Virtual clock (fast-time mode).
u64 g_vcycles = 0;
std::atomic<u64> g_vcyclesPublished{0};
u64 g_lastIcount = 0;
Ctx *g_budgetCtx = nullptr;
s32 g_budgetMark = 0;

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
std::vector<SceUID> g_busy;  // threads in a busy syscall

int g_chanceAccumulator = 50;

u64 g_sliceStartUs = 0;

int PrioIndex(s32 prio) { return prio < 0 ? 0 : (prio >= kNumPrio ? kNumPrio - 1 : prio); }

bool OnEmuThread() { return GetCurrentThreadId() == g_emuThreadId; }

u64 RealUs() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	u64 ticks = (u64)(now.QuadPart - g_qpcStart.QuadPart);
	return (ticks / (u64)g_qpcFreq.QuadPart) * 1000000ull +
	       ((ticks % (u64)g_qpcFreq.QuadPart) * 1000000ull) / (u64)g_qpcFreq.QuadPart;
}

// Fold the guest instructions executed since the last sync into the virtual clock. Interpreted
// code is counted exactly; native (AOT) code only through its back-edges, at two cycles each.
void SyncClock() {
	if (!g_fastTime || !OnEmuThread()) return;
	u64 ic = interp::InstructionCount();
	u64 di = ic - g_lastIcount;
	g_lastIcount = ic;
	u64 db = 0;
	Ctx *c = cpu::CurrentCtx();
	if (c && c == g_budgetCtx) {
		s32 b = c->budget;
		if (b < g_budgetMark) db = (u64)((s64)g_budgetMark - b);
		g_budgetMark = b;
	}
	g_vcycles += std::max(di, db * 2);
	g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
}

u64 NextEventTime() {
	while (!g_events.empty() && g_cancelled.count(g_events.top().id)) {
		g_cancelled.erase(g_events.top().id);
		g_events.pop();
	}
	return g_events.empty() ? UINT64_MAX : g_events.top().time;
}

// Back-edges the running code may take before the next yield check. In fast-time mode the check
// has to come around when the next event is due, so it is a fraction of the cycles until then.
s32 ComputeBudget() {
	if (!g_fastTime) return kBudget;
	u64 next = NextEventTime();
	if (next == UINT64_MAX) return kBudget;
	u64 now = g_vcycles / kCyclesPerUs;
	if (next <= now) return 1;
	u64 cycles = (next - now) * kCyclesPerUs;
	u64 b = cycles / 32;
	if (b < 1) b = 1;
	if (b > (u64)kBudget) b = kBudget;
	return (s32)b;
}

void SetBudget(Ctx *c) {
	SyncClock();
	c->budget = ComputeBudget();
	g_budgetCtx = c;
	g_budgetMark = c->budget;
}

bool EventsDue() {
	if (g_hostPending.load(std::memory_order_relaxed)) return true;
	if (!g_interrupts.empty() && *g_intrEnabled) return true;
	return NextEventTime() <= NowUs();
}

void Enqueue(PspThread *t, bool front) {
	if (t->inReadyQueue) return;
	auto &q = g_ready[PrioIndex(t->currentPriority)];
	if (front) q.push_front(t);
	else q.push_back(t);
	t->inReadyQueue = true;
	g_readyCount++;
}

PspThread *PeekBestReady() {
	if (!g_readyCount) return nullptr;
	for (int p = 0; p < kNumPrio; p++)
		if (!g_ready[p].empty()) return g_ready[p].front();
	return nullptr;
}

int BestReadyPrio() {
	PspThread *t = PeekBestReady();
	return t ? t->currentPriority : 0x7FFFFFFF;
}

void SwitchToScheduler() { SwitchToFiber(g_mainFiber); }

void ProcessDueEvents() {
	for (;;) {
		u64 now = NowUs();
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
	if (t->inReadyQueue) RemoveFromReady(t);
	if (g_lastRun != t && g_lastRun != nullptr) ChargeCycles(kSwitchCycles);
	g_lastRun = t;
	g_current = t;
	t->status = (t->status & ~(THREADSTATUS_READY | THREADSTATUS_WAIT)) | THREADSTATUS_RUNNING;
	cpu::SetCurrentCtx(&t->ctx);
	SetBudget(&t->ctx);
	g_sliceStartUs = NowUs();
	SwitchToFiber(t->fiber);
	// Back on the scheduler.
	SyncClock();
	if (PspThread *ran = g_current) {
		u64 now = NowUs();
		if (now > g_sliceStartUs) ran->runUs += now - g_sliceStartUs;
	}
	g_current = nullptr;
	g_budgetCtx = nullptr;
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
		g_vcycles = next * kCyclesPerUs;
		g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
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

PspThread *BestBusyThread() {
	PspThread *best = nullptr;
	for (size_t i = 0; i < g_busy.size();) {
		PspThread *t = Get<PspThread>(g_busy[i]);
		if (!t || !t->busy || !t->IsWaitingFor(WAITTYPE_HLEDELAY, 2)) {
			if (t) t->busy = false;
			g_busy.erase(g_busy.begin() + i);
			continue;
		}
		if (!best || t->currentPriority < best->currentPriority) best = t;
		i++;
	}
	return best;
}

// A busy syscall keeps the CPU unless a better thread is ready: burn its cycles up to the next
// event. Returns true if it did (the scheduler loop then goes round again).
bool RunBusy() {
	if (g_busy.empty()) return false;
	PspThread *busy = BestBusyThread();
	if (!busy) return false;
	PspThread *ready = PeekBestReady();
	if (ready && ready->currentPriority < busy->currentPriority) return false;
	SyncClock();
	u64 now = g_vcycles / kCyclesPerUs;
	u64 next = NextEventTime();
	s64 burn = busy->busyCycles;
	if (next != UINT64_MAX) {
		s64 untilNext = next > now ? (s64)((next - now) * kCyclesPerUs) : 0;
		if (untilNext < burn) burn = untilNext;
	}
	if (burn < 0) burn = 0;
	g_vcycles += (u64)burn;
	g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
	busy->busyCycles -= burn;
	if (busy->busyCycles <= 0) {
		busy->busy = false;
		g_busy.erase(std::remove(g_busy.begin(), g_busy.end(), busy->uid), g_busy.end());
		Wake(busy, 0);
		// It never gave up the CPU, so it goes back ahead of threads of the same priority.
		if (busy->inReadyQueue) {
			RemoveFromReady(busy);
			Enqueue(busy, true);
		}
	}
	return true;
}

// The thread that must get the CPU next whatever else is ready, if any.
PspThread *TakeForcedNext() {
	if (g_returnTo) {
		PspThread *t = g_returnTo;
		g_returnTo = nullptr;
		if (t->fiber && !(t->status & (THREADSTATUS_DORMANT | THREADSTATUS_DEAD))) return t;
	}
	if (g_handoff) {
		PspThread *t = Get<PspThread>(g_handoff);
		g_handoff = 0;
		if (t && t->inReadyQueue && (t->status & THREADSTATUS_READY)) return t;
	}
	return nullptr;
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

// ---- waits ----

struct SavedWait {
	WaitType type;
	SceUID id;
	u32 seq;
	bool done;
	s32 result;
	u32 timeoutPtr;
	u64 deadline;
	u32 latency;
	s32 timeoutResult;
	u32 value[4];
	bool callbacks;
	bool paused;
	WaitHooks hooks;
};

void SaveWait(PspThread *t, SavedWait &s) {
	s.type = t->waitType;
	s.id = t->waitId;
	s.seq = t->waitSeq;
	s.done = t->waitDone;
	s.result = t->waitResult;
	s.timeoutPtr = t->waitTimeoutPtr;
	s.deadline = t->waitDeadline;
	s.latency = t->waitLatencyUs;
	s.timeoutResult = t->waitTimeoutResult;
	memcpy(s.value, t->waitValue, sizeof(s.value));
	s.callbacks = t->waitCallbacks;
	s.paused = t->waitPaused;
	s.hooks = std::move(t->waitHooks);
}

void RestoreWait(PspThread *t, SavedWait &s) {
	t->waitType = s.type;
	t->waitId = s.id;
	t->waitSeq = s.seq;
	t->waitDone = s.done;
	t->waitResult = s.result;
	t->waitTimeoutPtr = s.timeoutPtr;
	t->waitDeadline = s.deadline;
	t->waitLatencyUs = s.latency;
	t->waitTimeoutResult = s.timeoutResult;
	memcpy(t->waitValue, s.value, sizeof(s.value));
	t->waitCallbacks = s.callbacks;
	t->waitPaused = s.paused;
	t->waitHooks = std::move(s.hooks);
	t->timeoutEventId = 0;
}

void WriteTimeLeft(PspThread *t) {
	if (!t->waitTimeoutPtr || !t->waitDeadline || !mem::IsValidRange(t->waitTimeoutPtr, 4)) return;
	GWrite32(t->waitTimeoutPtr, WaitTimeLeft(t));
}

void TimeoutNow(PspThread *t) {
	if (t->waitHooks.timeout) t->waitHooks.timeout();
	if (t->waitDone) return;
	if (t->waitTimeoutPtr && mem::IsValidRange(t->waitTimeoutPtr, 4)) GWrite32(t->waitTimeoutPtr, 0);
	t->waitTimeoutPtr = 0;
	Wake(t, t->waitTimeoutResult);
}

void ArmTimeout(PspThread *t) {
	if (!t->waitDeadline || t->timeoutEventId) return;
	SceUID uid = t->uid;
	u32 seq = t->waitSeq;
	t->timeoutEventId = ScheduleEvent(t->waitDeadline, [uid, seq]() {
		PspThread *w = kernel::Get<PspThread>(uid);
		if (!w || w->waitSeq != seq || w->waitDone || w->waitType == WAITTYPE_NONE) return;
		w->timeoutEventId = 0;
		if (w->waitPaused) return;  // re-checked when the callbacks return
		TimeoutNow(w);
	});
}

void PauseWait(PspThread *t) {
	if (t->waitPaused) return;
	t->waitPaused = true;
	if (t->timeoutEventId) {
		CancelEvent(t->timeoutEventId);
		t->timeoutEventId = 0;
	}
	if (t->waitHooks.pause) t->waitHooks.pause();
}

// Callbacks are done (or were canceled before the thread got to run): carry on with the wait,
// unless it can be satisfied now or its deadline has passed meanwhile.
void ResumeWait(PspThread *t) {
	t->waitPaused = false;
	if (t->waitDone) return;
	// The timeout kept running during the callback; put it back first so an unlock reports
	// the time that's left.
	if (t->waitHooks.resume && t->waitHooks.resume()) return;
	if (t->waitDone) return;
	if (t->waitDeadline && NowUs() >= t->waitDeadline) {
		TimeoutNow(t);
		return;
	}
	ArmTimeout(t);
}

s32 DoWait(PspThread *t, WaitType type, SceUID id, u64 deadline, u32 latency, u32 timeoutPtr, bool cb,
           s32 timeoutResult, const WaitHooks *hooks) {
	// A wait inside a callback of an outer wait: keep the outer one for when this returns.
	SavedWait outer;
	bool nested = t->waitType != WAITTYPE_NONE;
	if (nested) SaveWait(t, outer);

	t->waitType = type;
	t->waitId = id;
	t->waitDone = false;
	t->waitResult = 0;
	t->waitCallbacks = cb;
	t->waitTimeoutPtr = timeoutPtr;
	t->waitDeadline = deadline;
	t->waitLatencyUs = latency;
	t->waitTimeoutResult = timeoutResult;
	t->waitPaused = false;
	t->cbQueued = false;
	t->waitHooks = hooks ? *hooks : WaitHooks{};
	if (!t->waitHooks.timeout && t->timeoutHook) t->waitHooks.timeout = std::move(t->timeoutHook);
	t->timeoutHook = nullptr;
	t->hasWaited = true;
	t->timeoutEventId = 0;
	++t->waitSeq;
	ArmTimeout(t);

	for (;;) {
		if (t->waitDone) break;
		if (cb && kernel::CanRunCallbacks(t) && kernel::HasPendingCallbacks(t)) {
			PauseWait(t);
			kernel::RunPendingCallbacks(t);
			continue;
		}
		if (t->waitPaused) {
			ResumeWait(t);
			continue;
		}
		t->status = (t->status & THREADSTATUS_SUSPEND) | THREADSTATUS_WAIT;
		SwitchToScheduler();
		t->cbQueued = false;
	}
	if (t->timeoutEventId) {
		CancelEvent(t->timeoutEventId);
		t->timeoutEventId = 0;
	}
	s32 result = t->waitResult;
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	t->waitTimeoutPtr = 0;
	t->waitDeadline = 0;
	t->waitPaused = false;
	t->waitHooks = WaitHooks{};
	if (nested) RestoreWait(t, outer);
	return result;
}

}  // namespace

void Init(bool fastTime) {
	g_fastTime = fastTime;
	g_emuThreadId = GetCurrentThreadId();
	QueryPerformanceFrequency(&g_qpcFreq);
	QueryPerformanceCounter(&g_qpcStart);
	g_vcycles = 0;
	g_lastIcount = interp::InstructionCount();
	g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
	timeBeginPeriod(1);
	memset(&g_intrCtx, 0, sizeof(g_intrCtx));
	u32 stack = kernel::AllocKernelMemory(0x4000, "interrupt stack");
	g_intrStackTop = stack + 0x4000 - 0x10;
	g_chanceAccumulator = 50;
	g_dispatchEnabled = true;
}

void Shutdown() { timeEndPeriod(1); }

bool FastTime() { return g_fastTime; }

u64 NowUs() {
	if (g_fastTime) {
		if (!OnEmuThread()) return g_vcyclesPublished.load(std::memory_order_relaxed) / kCyclesPerUs;
		SyncClock();
		return g_vcycles / kCyclesPerUs;
	}
	return RealUs() + g_timeOffset;
}

void ChargeCycles(s64 cycles) {
	if (!g_fastTime || cycles <= 0 || !OnEmuThread()) return;
	SyncClock();
	g_vcycles += (u64)cycles;
	g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
}

void BusyDelayCycles(s64 cycles) {
	PspThread *t = g_current;
	if (!g_fastTime || cycles <= 0 || !t || g_inInterrupt || !DispatchEnabled()) {
		ChargeCycles(cycles);
		return;
	}
	t->busyCycles = cycles;
	t->busy = true;
	g_busy.push_back(t->uid);
	DoWait(t, WAITTYPE_HLEDELAY, 2, 0, 0, 0, false, 0, nullptr);
	t->busy = false;
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
	g_emuThreadId = GetCurrentThreadId();
	while (!g_stop) {
		DrainHostPosts();
		ProcessDueEvents();
		RunInterrupts();
		if (g_stop) break;
		if (PspThread *t = TakeForcedNext()) {
			RunThread(t);
			continue;
		}
		if (RunBusy()) continue;
		if (PspThread *t = PeekBestReady()) {
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
	t->status = (t->status & ~(THREADSTATUS_RUNNING | THREADSTATUS_WAIT | THREADSTATUS_DORMANT)) | THREADSTATUS_READY;
	if (t->status & THREADSTATUS_SUSPEND) {
		t->status &= ~THREADSTATUS_READY;
		return;
	}
	Enqueue(t, front);
}

void RemoveFromReady(PspThread *t) {
	if (!t->inReadyQueue) return;
	auto &q = g_ready[PrioIndex(t->currentPriority)];
	auto it = std::find(q.begin(), q.end(), t);
	if (it != q.end()) {
		q.erase(it);
		g_readyCount--;
		t->inReadyQueue = false;
		return;
	}
	for (auto &qq : g_ready) {
		auto it2 = std::find(qq.begin(), qq.end(), t);
		if (it2 != qq.end()) {
			qq.erase(it2);
			g_readyCount--;
			break;
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
	t->hasWaited = false;
	t->callbackDepth = 0;
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	t->waitDone = false;
	t->waitPaused = false;
	t->cbQueued = false;
	t->busy = false;
	t->status = (t->status & THREADSTATUS_SUSPEND) | THREADSTATUS_READY;
	if (t->status & THREADSTATUS_SUSPEND) {
		t->status = THREADSTATUS_SUSPEND;
		return;
	}
	Enqueue(t, false);
}

void ChangePriority(PspThread *t, s32 prio) {
	bool queued = t->inReadyQueue;
	if (queued) RemoveFromReady(t);
	t->currentPriority = prio;
	if (queued) Enqueue(t, false);
}

void RotateReadyQueue(s32 prio) {
	auto &q = g_ready[PrioIndex(prio)];
	if (q.size() > 1) {
		PspThread *t = q.front();
		q.pop_front();
		q.push_back(t);
	}
}

bool ReadyQueueEmpty(s32 prio) { return g_ready[PrioIndex(prio)].empty(); }

void Suspend(PspThread *t) {
	t->status |= THREADSTATUS_SUSPEND;
	if (t->inReadyQueue) RemoveFromReady(t);
	t->status &= ~THREADSTATUS_READY;
}

void Resume(PspThread *t) {
	t->status &= ~THREADSTATUS_SUSPEND;
	if (t->status == 0) {
		t->status = THREADSTATUS_READY;
		Enqueue(t, false);
	} else if ((t->status & THREADSTATUS_WAIT) && t->cbQueued) {
		Enqueue(t, false);
	}
}

void HandOffTo(PspThread *t) {
	PspThread *cur = g_current;
	if (!cur || g_inInterrupt || !DispatchEnabled()) return;
	g_handoff = t->uid;
	cur->threadPreemptCount++;
	MakeReady(cur, true);
	SwitchToScheduler();
}

s32 Wait(WaitType type, SceUID id, u32 timeoutPtr, bool processCallbacks, const WaitHooks *hooks) {
	PspThread *t = g_current;
	if (!t || g_inInterrupt) {
		if (t) t->timeoutHook = nullptr;
		return (s32)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	}
	if (!DispatchEnabled()) {
		t->timeoutHook = nullptr;
		return (s32)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	}
	u64 deadline = 0;
	if (timeoutPtr && mem::IsValidRange(timeoutPtr, 4)) {
		u32 us = GRead32(timeoutPtr);
		deadline = NowUs() + std::max<u32>(us, 205) + kWaitTimeoutDeadlineUs + kWaitTimeoutLatencyUs;
	} else {
		timeoutPtr = 0;
	}
	return DoWait(t, type, id, deadline, kWaitTimeoutLatencyUs, timeoutPtr, processCallbacks,
	              (s32)SCE_KERNEL_ERROR_WAIT_TIMEOUT, hooks);
}

s32 WaitUntil(WaitType type, SceUID id, u64 deadlineUs, bool processCallbacks, const WaitHooks *hooks) {
	PspThread *t = g_current;
	if (!t || g_inInterrupt) return (s32)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!DispatchEnabled()) return (s32)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	return DoWait(t, type, id, deadlineUs ? deadlineUs : 1, 0, 0, processCallbacks, 0, hooks);
}

// On hardware these outcomes depend on the phase of the clock. Go with the likelier one, except
// that always taking a likely-but-not-certain one could lock a polling loop into never yielding:
// those are true for the given share of calls, spread evenly (PPSSPP __KernelChance).
static bool Chance(int percent) {
	if (percent < 50) return false;
	if (percent >= 100) return true;
	g_chanceAccumulator += percent;
	if (g_chanceAccumulator >= 100) {
		g_chanceAccumulator -= 100;
		return true;
	}
	return false;
}

bool WaitTimesOutAtOnce(u32 timeoutPtr, int basePercent, int stepPercent) {
	if (!timeoutPtr || (timeoutPtr & 3) || !mem::IsValidRange(timeoutPtr, 4)) return false;
	u32 micro = std::min<u32>(GRead32(timeoutPtr), 100);
	return Chance(basePercent - (int)micro * stepPercent);
}

bool DelayReturnsAtOnce(u32 usec) {
	// The deadline is taken, and the clock read again when the alarm is set, about 0.6us later
	// (1.65us on a thread's first wait, when the code is out of the cache).
	const PspThread *t = g_current;
	int gapPercent = t && !t->hasWaited ? 165 : 60;
	return Chance(gapPercent - (int)std::min<u32>(usec, 2) * 100);
}

u32 WaitTimeLeft(PspThread *t) {
	if (!t->waitDeadline) return 0;
	u64 now = NowUs();
	u64 end = t->waitDeadline - std::min<u64>(t->waitDeadline, t->waitLatencyUs);
	return end > now ? (u32)(end - now) : 0;
}

void Wake(PspThread *t, s32 result) {
	if (t->waitDone || t->waitType == WAITTYPE_NONE) return;
	WriteTimeLeft(t);
	t->waitDone = true;
	t->waitResult = result;
	if (t->timeoutEventId) {
		CancelEvent(t->timeoutEventId);
		t->timeoutEventId = 0;
	}
	t->cbQueued = false;
	if (t == g_current) return;  // woken while running callbacks inside its own wait
	if (!(t->status & THREADSTATUS_WAIT)) return;  // preempted inside a callback; already queued
	t->status &= ~THREADSTATUS_WAIT;
	if (t->status & (THREADSTATUS_SUSPEND | THREADSTATUS_DORMANT | THREADSTATUS_DEAD)) return;
	t->status |= THREADSTATUS_READY;
	Enqueue(t, false);
}

void Reschedule() {
	PspThread *t = g_current;
	if (!t || g_inInterrupt || !*g_intrEnabled) return;
	SyncClock();
	if (EventsDue()) {
		if (DispatchEnabled()) {
			MakeReady(t, true);
		} else {
			g_returnTo = t;
		}
		SwitchToScheduler();
		return;
	}
	if (DispatchEnabled() && BestReadyPrio() < t->currentPriority) {
		t->threadPreemptCount++;
		MakeReady(t, true);
		SwitchToScheduler();
	}
}

void YieldToSamePriority() {
	PspThread *t = g_current;
	if (!t || g_inInterrupt || !DispatchEnabled()) return;
	MakeReady(t, false);
	SwitchToScheduler();
}

void YieldCheck(Ctx *c) {
	PspThread *t = g_current;
	if (!t || g_inInterrupt || &t->ctx != c) {
		c->budget = kBudget;
		return;
	}
	SyncClock();
	if (*g_intrEnabled && EventsDue()) {
		t->intrPreemptCount++;
		if (DispatchEnabled()) {
			MakeReady(t, true);
		} else {
			g_returnTo = t;
		}
		SwitchToScheduler();
		return;
	}
	SetBudget(c);
}

static void WakeEndWaiters(PspThread *t, s32 status) {
	std::vector<SceUID> waiters;
	waiters.swap(t->endWaiters);
	for (SceUID w : waiters) {
		PspThread *wt = kernel::Get<PspThread>(w);
		if (wt && wt->waitType == WAITTYPE_THREADEND && wt->waitId == t->uid && !wt->waitDone && !wt->waitPaused)
			Wake(wt, status);
	}
}

void ExitCurrentThread(s32 status, bool deleteAfter) {
	PspThread *t = g_current;
	if (!t) Fatal("ExitCurrentThread outside a thread");
	ChargeCycles(kThreadExitCycles);
	t->exitStatus = status;
	t->status = THREADSTATUS_DORMANT;
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	t->callbackDepth = 0;
	t->busy = false;
	WakeEndWaiters(t, status);
	kernel::NotifyThreadEnd(t);
	g_deadFibers.push_back(t->fiber);
	t->fiber = nullptr;
	if (deleteAfter) {
		t->status = THREADSTATUS_DEAD;
		g_pendingDelete.push_back(t);
	}
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
	t->waitId = 0;
	t->waitDone = false;
	t->waitPaused = false;
	t->cbQueued = false;
	t->waitHooks = WaitHooks{};
	t->callbackDepth = 0;
	t->busy = false;
	t->exitStatus = status;
	t->status = THREADSTATUS_DORMANT;
	t->currentPriority = t->initPriority;
	if (g_returnTo == t) g_returnTo = nullptr;
	WakeEndWaiters(t, status);
	kernel::NotifyThreadEnd(t);
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
	if (g_returnTo == t) g_returnTo = nullptr;
	if (g_lastRun == t) g_lastRun = nullptr;
	if (t->fiber) {
		g_deadFibers.push_back(t->fiber);
		t->fiber = nullptr;
	}
	kernel::Destroy(t->uid);
}

void UpdateRunTime() {
	PspThread *t = g_current;
	if (!t) return;
	u64 now = NowUs();
	if (now > g_sliceStartUs) t->runUs += now - g_sliceStartUs;
	g_sliceStartUs = now;
}

void NotifyCallbackPending(PspThread *t) {
	if (t == g_current) return;  // it will see them at its next check point
	if (!(t->status & THREADSTATUS_WAIT) || t->waitDone || !t->waitCallbacks || t->waitType == WAITTYPE_NONE) return;
	if (!kernel::CanRunCallbacks(t)) return;
	PauseWait(t);
	if (!t->cbQueued) {
		t->cbQueued = true;
		if (!(t->status & THREADSTATUS_SUSPEND)) Enqueue(t, false);
	}
}

bool DispatchEnabled() { return g_dispatchEnabled && *g_intrEnabled; }
bool DispatchFlag() { return g_dispatchEnabled; }
void SetDispatchEnabled(bool enabled) { g_dispatchEnabled = enabled; }
bool InInterrupt() { return g_inInterrupt; }

u32 InterruptStackTop() { return g_intrStackTop; }

u32 RunInterruptHandler(u32 func, u32 gp, std::initializer_list<u32> args, u32 stackReserve) {
	bool prevIntr = g_inInterrupt;
	Ctx *prevCtx = cpu::CurrentCtx();
	g_inInterrupt = true;
	g_intrCtx.r[29] = g_intrStackTop - stackReserve;
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
