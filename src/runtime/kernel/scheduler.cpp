// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scheduling and wait semantics follow PPSSPP's sceKernelThread.cpp and KernelWaitHelpers.h,
// ported from its asynchronous begin/end-callback model to blocking fiber waits; the timing
// constants (wait timeout deadline/latency, delay rounding, "times out at once" odds, context
// switch and call costs) are PPSSPP's, measured against pspautotests on hardware.
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later.
#include "runtime/kernel/scheduler.h"

#include "common/host.h"
#include <thread>

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

constexpr s32 kBudget = 200000;  // cycles (guest instructions) between yield checks at most
constexpr int kNumPrio = 128;
constexpr u64 kCyclesPerUs = kCpuHz / 1000000;
// How long after its deadline a wait's timeout goes off, and how far into the call the deadline
// is taken (PPSSPP WAIT_TIMEOUT_LATENCY_US / WAIT_TIMEOUT_DEADLINE_US).
constexpr u32 kWaitTimeoutLatencyUs = 18;
constexpr u32 kWaitTimeoutDeadlineUs = 12;
// A switch between two threads costs about 5us (pspautotests threads/scheduling/handoff).
constexpr s64 kSwitchCycles = 1150;
// A thread a timer woke (sceKernelDelayThread) that takes the CPU from another thread still
// running spends about 10us more getting there: with a better thread delaying 200us and working
// 150us after each wake, a busy syscall gets ~190us between wakes (threads/scheduling/
// preemptsyscall). Onto an idle CPU it doesn't (threads/semaphores/wait).
constexpr s64 kTimerDispatchCycles = 10 * 222;
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
// Identity of the thread whose CPU work was interrupted, even on the scheduler fiber.
PspThread *g_interrupted = nullptr;
// Keep the identity after deletion: switching away still costs time, while a raw
// pointer would either dangle or lose that cost when teardown clears it.
SceUID g_lastRun = 0;
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
u64 g_clockStart = 0;
u64 g_timeOffset = 0;
std::thread::id g_emuThreadId;

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
hostsys::WakeEvent g_wakeEvent;

bool g_dispatchEnabled = true;
bool g_inInterrupt = false;
std::deque<std::pair<int, std::function<void()>>> g_interrupts;
alignas(64) Ctx g_intrCtx;
u32 g_intrStackTop = 0;

std::vector<void *> g_deadFibers;
std::vector<PspThread *> g_pendingDelete;
std::vector<SceUID> g_busy;  // threads in a busy syscall

int g_chanceAccumulator = 50;

u64 g_sliceStartUs = 0;

int PrioIndex(s32 prio) { return prio < 0 ? 0 : (prio >= kNumPrio ? kNumPrio - 1 : prio); }

bool OnEmuThread() { return std::this_thread::get_id() == g_emuThreadId; }

u64 RealUs() { return hostsys::MonotonicUs() - g_clockStart; }

// Fold the guest cycles run since the last sync into the virtual clock. Interpreted and native
// code both charge one cycle per instruction to the running context's budget, so the clock (and
// every test that measures time) is the same in both modes.
void SyncClock() {
	if (!g_fastTime || !OnEmuThread()) return;
	Ctx *c = cpu::CurrentCtx();
	if (c && c == g_budgetCtx) {
		s32 b = c->budget;
		if (b < g_budgetMark) g_vcycles += (u64)((s64)g_budgetMark - b);
		g_budgetMark = b;
	}
	g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
}

u64 NextEventTime() {
	while (!g_events.empty() && g_cancelled.count(g_events.top().id)) {
		g_cancelled.erase(g_events.top().id);
		g_events.pop();
	}
	return g_events.empty() ? UINT64_MAX : g_events.top().time;
}

// Cycles the running code may use before the next yield check (made at back-edges and
// syscalls). In fast-time mode that is exactly the time until the next event.
s32 ComputeBudget() {
	if (!g_fastTime) return kBudget;
	u64 next = NextEventTime();
	if (next == UINT64_MAX) return kBudget;
	u64 due = next * kCyclesPerUs;
	if (due <= g_vcycles) return 0;
	return (s32)std::min<u64>(due - g_vcycles, (u64)kBudget);
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

void SwitchToScheduler() { hostsys::SwitchFiber(g_mainFiber); }

void ProcessDueEvents() {
	// Real-time callbacks consume wall time. Chasing a moving deadline can keep this
	// loop busy forever, starving ready threads and interrupts under host load.
	const u64 batchTime = NowUs();
	for (;;) {
		u64 now = g_fastTime ? NowUs() : batchTime;
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
		auto best = g_interrupts.begin();
		for (auto it = g_interrupts.begin(); it != g_interrupts.end(); ++it) {
			if (it->first < best->first) best = it;
		}
		auto fn = std::move(best->second);
		g_interrupts.erase(best);
		fn();
	}
}

void FreeDeadFibers() {
	for (void *f : g_deadFibers) hostsys::FreeFiber(f);
	g_deadFibers.clear();
	for (PspThread *t : g_pendingDelete) DeleteThread(t);
	g_pendingDelete.clear();
}

s64 g_dispatchLatency = -1;  // SetDispatchLatency, for the next RunThread
u32 g_jitterState = 0x9E3779B9;

void RunThread(PspThread *t) {
	if (t->inReadyQueue) RemoveFromReady(t);
	if (g_dispatchLatency >= 0) {
		ChargeCycles(g_dispatchLatency);
		g_dispatchLatency = -1;
	} else if (g_lastRun != t->uid && g_lastRun != 0) {
		PspThread *last = Get<PspThread>(g_lastRun);
		const bool preempts = last && (last->busy || (last->status & (THREADSTATUS_READY | THREADSTATUS_RUNNING)) != 0);
		ChargeCycles(kSwitchCycles + (t->timerWoken && preempts ? kTimerDispatchCycles : 0));
	}
	t->timerWoken = false;
	g_lastRun = t->uid;
	g_current = t;
	t->status = (t->status & ~(THREADSTATUS_READY | THREADSTATUS_WAIT)) | THREADSTATUS_RUNNING;
	cpu::SetCurrentCtx(&t->ctx);
	// Another thread or an interrupt may have touched memory: a pending ll/sc fails
	// (cpu/lsu/llsc).
	t->ctx.llbit = 0;
	SetBudget(&t->ctx);
	g_sliceStartUs = NowUs();
	hostsys::SwitchFiber(t->fiber);
	// Back on the scheduler.
	SyncClock();
	g_interrupted = nullptr;
	if (PspThread *ran = g_current) {
		u64 now = NowUs();
		if (now > g_sliceStartUs) ran->runUs += now - g_sliceStartUs;
		// A blocked thread becoming ready in a timer callback was not interrupted.
		if ((ran->status & (THREADSTATUS_RUNNING | THREADSTATUS_READY)) || ran->busy)
			g_interrupted = ran;
	}
	g_current = nullptr;
	g_budgetCtx = nullptr;
	cpu::SetCurrentCtx(nullptr);
	FreeDeadFibers();
}

int g_realTimeHolds = 0;

void IdleUntil(u64 next) {
	u64 now = NowUs();
	if (next == UINT64_MAX) {
		g_wakeEvent.WaitUs(50000);
		return;
	}
	if (next <= now) return;
	u64 delta = next - now;
	if (g_fastTime && g_realTimeHolds > 0 && delta >= 1000) {
		// Host-paced: sleep (until a host post at the latest) and let the virtual clock follow.
		u64 start = RealUs();
		g_wakeEvent.WaitUs(std::min<u64>(delta, 50000));
		u64 slept = std::max<u64>(RealUs() - start, 1);
		g_vcycles = (now + std::min(delta, slept)) * kCyclesPerUs;
		g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
		return;
	}
	if (g_fastTime) {
		g_vcycles = next * kCyclesPerUs;
		g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
		return;
	}
	if (delta > 1500) {
		g_wakeEvent.WaitUs(delta - 500);
		return;
	}
	while (NowUs() < next && !g_hostPending.load(std::memory_order_relaxed)) std::this_thread::yield();
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
	// The interrupt returns to the busy call, not to a thread it woke.
	g_dispatchLatency = -1;
	// Getting back to the busy thread after another one ran costs a switch, as for any thread.
	if (g_lastRun != busy->uid) {
		if (g_lastRun != 0) ChargeCycles(kSwitchCycles);
		g_lastRun = busy->uid;
	}
	g_interrupted = busy;
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

void ThreadFiberProc(void *param) {
	PspThread *t = (PspThread *)param;
	Ctx *c = &t->ctx;
	if (t->onFirstRun) {
		auto fn = std::move(t->onFirstRun);
		t->onFirstRun = nullptr;
		fn();
	}
	c->r[31] = t->threadReturnStub;
	c->unwinding = 0;
	dispatch::Call(c, t->entry);
	while (c->unwinding) {
		u32 target = c->pc;
		c->unwinding = 0;
		if (target == t->threadReturnStub) break;
		dispatch::Resume(c, target, t->threadReturnStub);
	}
	if (t->deleteOnReturn) {
		kernel::DeleteThreadCallbacks(t);
		kernel::FreeThreadStack(t);
		ExitCurrentThread((s32)c->r[2], true);
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
	u32 wakeCycles;
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
	s.wakeCycles = t->waitWakeCycles;
	s.timeoutResult = t->waitTimeoutResult;
	memcpy(s.value, t->waitValue, sizeof(s.value));
	s.callbacks = t->waitCallbacks;
	s.paused = t->waitPaused;
	s.hooks = t->waitHooks;
}

void RestoreWait(PspThread *t, SavedWait &s) {
	t->waitType = s.type;
	t->waitId = s.id;
	t->waitDone = s.done;
	t->waitResult = s.result;
	t->waitTimeoutPtr = s.timeoutPtr;
	t->waitDeadline = s.deadline;
	t->waitLatencyUs = s.latency;
	t->waitWakeCycles = s.wakeCycles;
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

// The thread is woken first and the hook runs after, so threads the hook releases (FIFO
// semaphores and VPLs) queue up behind it.
void TimeoutNow(PspThread *t) {
	if (t->waitDone) return;
	std::function<void()> hook = t->waitHooks.timeout;
	if (t->waitTimeoutPtr && mem::IsValidRange(t->waitTimeoutPtr, 4)) GWrite32(t->waitTimeoutPtr, 0);
	t->waitTimeoutPtr = 0;
	Wake(t, t->waitTimeoutResult);
	if (hook) hook();
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
		// The interrupt's own time goes to whoever has the CPU (threads/scheduling/preemptsyscall).
		if (w->waitWakeCycles) {
			ChargeCycles(w->waitWakeCycles);
			w->timerWoken = true;
		}
		TimeoutNow(w);
	});
}

void PauseWait(PspThread *t) {
	if (t->waitPaused) return;
	LOGT("SCHED", "pause wait of %s (type %d id %08x)", t->name.c_str(), t->waitType, t->waitId);
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
	LOGT("SCHED", "resume wait of %s (type %d id %08x, done %d)", t->name.c_str(), t->waitType, t->waitId, t->waitDone);
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
           s32 timeoutResult, const WaitHooks *hooks, u32 wakeCycles = 0) {
	// A wait inside a callback of an outer wait replaces it; the outer one is put back when the
	// callbacks return (see below).
	bool nested = t->waitType != WAITTYPE_NONE;

	t->waitType = type;
	t->waitId = id;
	t->waitDone = false;
	t->waitResult = 0;
	t->waitCallbacks = cb;
	t->waitTimeoutPtr = timeoutPtr;
	t->waitDeadline = deadline;
	t->waitLatencyUs = latency;
	t->waitWakeCycles = wakeCycles;
	t->waitTimeoutResult = timeoutResult;
	t->waitPaused = false;
	t->cbQueued = false;
	t->waitHooks = hooks ? *hooks : WaitHooks{};
	if (!t->waitHooks.timeout && t->timeoutHook) t->waitHooks.timeout = std::move(t->timeoutHook);
	t->timeoutHook = nullptr;
	t->hasWaited = true;
	t->releaseCount++;
	t->timeoutEventId = 0;
	++t->waitSeq;
	LOGT("SCHED", "%s waits (type %d id %08x%s%s)", t->name.c_str(), type, id, cb ? " CB" : "", nested ? " nested" : "");
	ArmTimeout(t);

	for (;;) {
		if (t->waitDone) break;
		if (cb && kernel::CanRunCallbacks(t) && kernel::HasPendingCallbacks(t)) {
			PauseWait(t);
			// The callbacks may wait themselves (overwriting the wait state, including the values
			// the caller stored for this wait): keep this one for when they return.
			SavedWait outer;
			SaveWait(t, outer);
			u32 seq = t->waitSeq;
			kernel::RunPendingCallbacks(t);
			if (t->waitSeq != seq) RestoreWait(t, outer);
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
	LOGT("SCHED", "%s wait (type %d id %08x) ends with %08x", t->name.c_str(), t->waitType, t->waitId, result);
	t->waitType = WAITTYPE_NONE;
	t->waitId = 0;
	t->waitTimeoutPtr = 0;
	t->waitDeadline = 0;
	t->waitPaused = false;
	t->waitHooks = WaitHooks{};
	return result;
}

}  // namespace

void Init(bool fastTime) {
	g_fastTime = fastTime;
	g_emuThreadId = std::this_thread::get_id();
	g_clockStart = hostsys::MonotonicUs();
	g_vcycles = 0;
	g_lastIcount = interp::InstructionCount();
	g_lastRun = 0;
	memset(&g_intrCtx, 0, sizeof(g_intrCtx));
	u32 stack = kernel::AllocKernelMemory(0x4000, "interrupt stack");
	g_intrStackTop = stack + 0x4000 - 0x10;
	g_chanceAccumulator = 50;
	g_dispatchLatency = -1;
	g_jitterState = 0x9E3779B9;
	g_dispatchEnabled = true;
}

void Shutdown() {}

bool FastTime() { return g_fastTime; }

u64 NowUs() {
	if (g_fastTime) {
		if (!OnEmuThread()) return g_vcyclesPublished.load(std::memory_order_relaxed) / kCyclesPerUs;
		SyncClock();
		return g_vcycles / kCyclesPerUs;
	}
	return RealUs() + g_timeOffset;
}

u64 NowCycles() {
	if (g_fastTime) {
		if (!OnEmuThread()) return g_vcyclesPublished.load(std::memory_order_relaxed);
		SyncClock();
		return g_vcycles;
	}
	return NowUs() * kCyclesPerUs;
}

void ChargeCycles(s64 cycles) {
	if (!g_fastTime || cycles <= 0 || !OnEmuThread()) return;
	SyncClock();
	g_vcycles += (u64)cycles;
	g_vcyclesPublished.store(g_vcycles, std::memory_order_relaxed);
	// Charged time comes out of the running code's budget too (budget and mark move together, so
	// SyncClock doesn't count it twice): an event that became due is noticed at the next
	// back-edge, and a loop of cheap HLE calls sees timers on time.
	if (g_budgetCtx && g_budgetCtx == cpu::CurrentCtx()) {
		s64 b = std::max<s64>((s64)g_budgetCtx->budget - cycles, -(s64)kBudget);
		g_budgetCtx->budget = (s32)b;
		g_budgetMark = (s32)b;
	}
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

void SetDispatchLatency(s64 cycles) {
	if (g_fastTime) g_dispatchLatency = std::max<s64>(cycles, 0);
}

s64 JitterCycles(s64 maxCycles) {
	if (maxCycles <= 0) return 0;
	u32 x = g_jitterState;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_jitterState = x;
	return (s64)(x % (u32)(maxCycles + 1));
}

void SpendUs(u64 busyUs, u64 waitUs) {
	PspThread *t = g_current;
	if (!t || g_inInterrupt || !DispatchEnabled()) return;
	if (g_fastTime)
		BusyDelayCycles((s64)(busyUs * kCyclesPerUs));
	else
		waitUs += busyUs;
	if (waitUs) WaitUntil(WAITTYPE_HLEDELAY, t->uid, NowUs() + waitUs, false);
}

u64 ScheduleEvent(u64 atUs, std::function<void()> fn) {
	u64 id = g_nextEventId++;
	g_events.push(Event{atUs, id, std::move(fn)});
	// An event sooner than the running code's budget reaches must cut the budget short.
	if (g_fastTime && g_budgetCtx && g_budgetCtx == cpu::CurrentCtx() && OnEmuThread()) {
		SyncClock();
		s64 until = (s64)(atUs * kCyclesPerUs) - (s64)g_vcycles;
		if (until < g_budgetCtx->budget) {
			s32 b = (s32)std::max<s64>(until, 0);
			g_budgetCtx->budget = b;
			g_budgetMark = b;
		}
	}
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
	g_wakeEvent.Signal();
}

void HoldRealTime(bool hold) {
	g_realTimeHolds += hold ? 1 : -1;
	if (g_realTimeHolds < 0) g_realTimeHolds = 0;
}

int Run() {
	g_mainFiber = hostsys::EnterFibers();
	if (!g_mainFiber) Fatal("Cannot enter guest fiber scheduler");
	g_emuThreadId = std::this_thread::get_id();
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
		// Every thread has ended (dormant or dead): nothing can ever run again, so the program
		// is over (a kernel module that unloaded itself, a homebrew that returned from main).
		bool alive = false;
		kernel::ForEach<PspThread>([&](PspThread *t) {
			if (!(t->status & (THREADSTATUS_DORMANT | THREADSTATUS_DEAD))) alive = true;
		});
		if (!alive) {
			LOGI("SCHED", "No thread left alive; stopping");
			g_exitCode = 0;
			break;
		}
		g_interrupted = nullptr;
		IdleUntil(NextEventTime());
	}
	hostsys::LeaveFibers();
	return g_exitCode;
}

void RequestStop(int exitCode) {
	if (!g_stop) g_exitCode = exitCode;
	g_stop = true;
	if (g_current && hostsys::CurrentFiber() != g_mainFiber) {
		// Called from a guest thread: leave it for good.
		hostsys::SwitchFiber(g_mainFiber);
	}
}

bool StopRequested() { return g_stop; }

PspThread *Current() { return g_current ? g_current : (g_inInterrupt ? g_interrupted : nullptr); }
SceUID CurrentId() {
	PspThread *t = Current();
	return t ? t->uid : 0;
}

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
	t->fiber = hostsys::NewFiber(8 * 1024 * 1024, ThreadFiberProc, t);
	if (!t->fiber) Fatal("Cannot allocate guest fiber stack");
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
		if (t) {
			t->timeoutHook = nullptr;
			++t->waitSeq;  // the caller may have clobbered the wait values
		}
		return (s32)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	}
	if (!DispatchEnabled()) {
		t->timeoutHook = nullptr;
		++t->waitSeq;
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

s32 WaitUntil(WaitType type, SceUID id, u64 deadlineUs, bool processCallbacks, const WaitHooks *hooks,
              u32 wakeCycles) {
	PspThread *t = g_current;
	if (t) ++t->waitSeq;
	if (!t || g_inInterrupt) return (s32)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!DispatchEnabled()) return (s32)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (!deadlineUs) deadlineUs = 1;
	if (g_fastTime && wakeCycles) {
		// The interrupt runs that long before the deadline, so the thread is ready on time.
		u64 wakeUs = wakeCycles / kCyclesPerUs;
		u64 now = NowUs();
		deadlineUs = deadlineUs > now + wakeUs ? deadlineUs - wakeUs : std::min(deadlineUs, now + 1);
	} else {
		wakeCycles = 0;
	}
	return DoWait(t, type, id, deadlineUs, 0, 0, processCallbacks, 0, hooks, (u32)wakeCycles);
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
	// (about 1.85us on a thread's first wait or delay, when the code is out of the cache: a new
	// thread's first sceKernelDelayThread(1) usually returns at once - "sometimes" in
	// threads/scheduling/delayzero, all three in threads/scheduling/scheduling - and its second
	// doesn't).
	PspThread *t = g_current;
	int gapPercent = t && !t->hasWaited ? 185 : 60;
	if (t) t->hasWaited = true;
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
	LOGT("SCHED", "wake %s (type %d id %08x) with %08x", t->name.c_str(), t->waitType, t->waitId, result);
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
		SyncClock();
		c->budget = kBudget;
		if (c == g_budgetCtx) g_budgetMark = kBudget;
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
	hostsys::SwitchFiber(g_mainFiber);
	Fatal("Exited thread resumed");
}

void TerminateThread(PspThread *t, s32 status) {
	if (t == g_current) ExitCurrentThread(status, false);
	if (g_interrupted == t) g_interrupted = nullptr;
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
	if (g_interrupted == t) g_interrupted = nullptr;
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
	// The handler's cycles go on the clock too: give it its own budget for the duration.
	SyncClock();
	Ctx *prevBudgetCtx = g_budgetCtx;
	g_intrCtx.budget = kBudget;
	g_budgetCtx = &g_intrCtx;
	g_budgetMark = kBudget;
	cpu::SetCurrentCtx(&g_intrCtx);
	u32 result = cpu::CallGuest(&g_intrCtx, func, args);
	SyncClock();
	cpu::SetCurrentCtx(prevCtx);
	g_budgetCtx = prevBudgetCtx;
	if (prevBudgetCtx) g_budgetMark = prevBudgetCtx->budget;
	g_inInterrupt = prevIntr;
	return result;
}

void QueueInterrupt(std::function<void()> fn, int intrNo) { g_interrupts.emplace_back(intrNo, std::move(fn)); }

}  // namespace sched
