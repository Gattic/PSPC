// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSP thread scheduling on Windows fibers. All guest threads run on one host thread: the
// scheduler loop lives on that thread's primary fiber and switches to the highest-priority ready
// guest thread. A guest thread gives control back when it blocks (Wait), when a better thread
// becomes ready (Reschedule), or when a yield check notices a due timer event.
//
// Time: normally the guest clock is the host clock. With --fast-time the clock is virtual and
// deterministic: it advances with the guest instructions executed (one cycle each at 222 MHz),
// with the costs HLE calls charge (Charge*), and jumps over idle time.
#pragma once

#include <functional>
#include <initializer_list>

#include "allegrex/ctx.h"
#include "runtime/kernel/thread.h"

namespace sched {

using kernel::PspThread;

constexpr u64 kCpuHz = 222000000;

void Init(bool fastTime);
void Shutdown();
// Runs until RequestStop(). Returns the exit code.
int Run();
void RequestStop(int exitCode);
bool StopRequested();

// ---- Time ----
u64 NowUs();
// The CPU clock in cycles (222 MHz): exact in fast-time mode, NowUs()-based otherwise.
u64 NowCycles();
bool FastTime();
// Charge CPU time to the running code (fast-time mode only; no-op otherwise).
void ChargeCycles(s64 cycles);
inline void ChargeUs(s64 us) { ChargeCycles(us * (s64)(kCpuHz / 1000000)); }
// A syscall that keeps the CPU busy: better threads that become ready meanwhile run first (and
// their time doesn't count), worse ones don't run. Fast-time mode only; elsewhere a no-op.
void BusyDelayCycles(s64 cycles);
u64 ScheduleEvent(u64 atUs, std::function<void()> fn);
void CancelEvent(u64 id);
// Run fn on the emulation thread as soon as possible (thread-safe; for host threads).
void PostFromHost(std::function<void()> fn);

// ---- Threads ----
PspThread *Current();
SceUID CurrentId();
// Prepare a started thread (status READY) and queue it.
void LaunchThread(PspThread *t);
void MakeReady(PspThread *t, bool front = false);
void RemoveFromReady(PspThread *t);
void ChangePriority(PspThread *t, s32 prio);
void RotateReadyQueue(s32 prio);  // move the first thread of that priority to the back
bool ReadyQueueEmpty(s32 prio);
void Suspend(PspThread *t);
void Resume(PspThread *t);
// Like Reschedule, but the next dispatch goes to `t` (just started, better than the caller),
// even if a better thread is ready but wasn't dispatched yet.
void HandOffTo(PspThread *t);

// Block the current thread. timeoutPtr is a guest u32* in microseconds (0 = forever) and is
// updated with the time left, as the hardware does: the timeout fires max(t, 205us) + 30us after
// the call. hooks (optional) make a CB wait pausable while callbacks run (see WaitHooks).
// Returns the wake result. Fails with CAN_NOT_WAIT when dispatch or interrupts are disabled.
s32 Wait(kernel::WaitType type, SceUID id, u32 timeoutPtr, bool processCallbacks,
         const kernel::WaitHooks *hooks = nullptr);
// Block until an absolute time (delays). Returns 0, or a wake result.
s32 WaitUntil(kernel::WaitType type, SceUID id, u64 deadlineUs, bool processCallbacks,
              const kernel::WaitHooks *hooks = nullptr);
// On hardware a very short timeout often expires before the wait even starts: the call then
// fails with WAIT_TIMEOUT at once, without writing the timeout back. Call this before queueing.
bool WaitTimesOutAtOnce(u32 timeoutPtr, int basePercent = 85, int stepPercent = 35);
// Same idea for delays: a delay of 0-1us sometimes returns at once.
bool DelayReturnsAtOnce(u32 usec);
// Wake a thread that is waiting (any wait type). No-op if it isn't waiting.
void Wake(PspThread *t, s32 result);
// Time left for the current wait of t, as written back to its timeout pointer.
u32 WaitTimeLeft(PspThread *t);
// Dispatch point: process due events, and yield if dispatch is enabled and a better thread is
// ready.
void Reschedule();
// Yield the current thread to all ready threads of the same priority (sceKernelRotate...).
void YieldToSamePriority();
void YieldCheck(Ctx *c);
[[noreturn]] void ExitCurrentThread(s32 status, bool deleteAfter);
void TerminateThread(PspThread *t, s32 status);
// Free a dormant thread object (and its fiber) safely.
void DeleteThread(PspThread *t);
// Account the time the current thread has run so far (runUs).
void UpdateRunTime();

// A callback owned by t was notified; if t is in a callback-enabled wait, pause it and queue t
// to run the callback when it would get the CPU.
void NotifyCallbackPending(PspThread *t);

// ---- Dispatch / interrupts ----
bool DispatchEnabled();  // dispatch enabled and interrupts enabled
bool DispatchFlag();     // the sceKernelSuspendDispatchThread state alone
void SetDispatchEnabled(bool enabled);
bool InInterrupt();
// Run a guest interrupt handler (sub-interrupt, GE callback...) in interrupt context. Must be
// called from the scheduler (an event or host post), never from inside a guest thread.
// stackReserve bytes are left free at the top of the interrupt stack (see InterruptStackTop) for
// data the caller passes by pointer.
u32 RunInterruptHandler(u32 func, u32 gp, std::initializer_list<u32> args, u32 stackReserve = 0);
u32 InterruptStackTop();
// Queue an interrupt-context action to run at the next scheduling point.
void QueueInterrupt(std::function<void()> fn);

}  // namespace sched
