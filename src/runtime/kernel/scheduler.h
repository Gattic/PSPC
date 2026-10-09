// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSP thread scheduling on Windows fibers. All guest threads run on one host thread: the
// scheduler loop lives on that thread's primary fiber and switches to the highest-priority ready
// guest thread. A guest thread gives control back when it blocks (Wait), when a better thread
// becomes ready (Reschedule), or when a yield check notices a due timer event.
#pragma once

#include <functional>
#include <initializer_list>

#include "allegrex/ctx.h"
#include "runtime/kernel/thread.h"

namespace sched {

using kernel::PspThread;

void Init(bool fastTime);
void Shutdown();
// Runs until RequestStop(). Returns the exit code.
int Run();
void RequestStop(int exitCode);
bool StopRequested();

// ---- Time ----
u64 NowUs();
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
void Suspend(PspThread *t);
void Resume(PspThread *t);

// Block the current thread. timeoutPtr is a guest u32* in microseconds (0 = forever) and is
// updated with the remaining time. Returns the wake result.
s32 Wait(kernel::WaitType type, SceUID id, u32 timeoutPtr, bool processCallbacks);
// Block until an absolute time (delays). Returns 0, or a wake result.
s32 WaitUntil(kernel::WaitType type, SceUID id, u64 deadlineUs, bool processCallbacks);
// Wake a thread that is waiting (any wait type). No-op if it isn't waiting.
void Wake(PspThread *t, s32 result);
// Yield if dispatch is enabled and a better thread is ready.
void Reschedule();
// Yield the current thread to all ready threads of the same priority (sceKernelRotate...).
void YieldToSamePriority();
void YieldCheck(Ctx *c);
[[noreturn]] void ExitCurrentThread(s32 status, bool deleteAfter);
void TerminateThread(PspThread *t, s32 status);
// Free a dormant thread object (and its fiber) safely.
void DeleteThread(PspThread *t);

// A callback owned by t was notified; wake it if it is in a callback-enabled wait.
void NotifyCallbackPending(PspThread *t);

// ---- Dispatch / interrupts ----
bool DispatchEnabled();
void SetDispatchEnabled(bool enabled);
bool InInterrupt();
// Run a guest interrupt handler (sub-interrupt, GE callback...) in interrupt context. Must be
// called from the scheduler (an event or host post), never from inside a guest thread.
u32 RunInterruptHandler(u32 func, u32 gp, std::initializer_list<u32> args);
// Queue an interrupt-context action to run at the next scheduling point.
void QueueInterrupt(std::function<void()> fn);

}  // namespace sched
