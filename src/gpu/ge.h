// SPDX-License-Identifier: GPL-2.0-or-later
//
// The PSP Graphics Engine: display list queue, command processor and software rasterizer.
//
// Everything runs synchronously on the caller's (emulation) thread; rasterization may fan out to an
// internal worker pool, but all drawing is finished before any API call returns, so guest memory is
// always up to date when the CPU looks at it. Results don't depend on the thread count.
//
// List IDs are the firmware-visible ones (0x35000000 ^ index, as PPSSPP hands out). Errors are the PSP
// kernel codes (negative when read as int).
#pragma once

#include "common/common.h"

namespace gpu {

void Init();
void Shutdown();
// What sceKernelLoadExec does to the GE: empty queue, every register and matrix zero.
void Reinitialize();

struct Hooks {
	void *user = nullptr;
	// GE SIGNAL interrupt with a signal callback (HANDLER_SUSPEND/CONTINUE, and the FINISH that delivers a
	// HANDLER_PAUSE). The GE is stopped while this runs; it continues when the hook returns.
	void (*onSignal)(void *user, int listId, int callbackId, u32 signalArg) = nullptr;
	// GE FINISH interrupt: the finish callback. The list reads COMPLETED, the GE is idle and its state is
	// still what the list left; the context is restored and the next list starts after the hook returns.
	void (*onFinish)(void *user, int listId, int callbackId, u32 finishArg) = nullptr;
	// A list completed or was dequeued: wake threads in sceGeListSync(listId, 0) once ListWaitTicks(listId)
	// has come.
	void (*onListDone)(void *user, int listId) = nullptr;
	// The queue drained: wake threads in sceGeDrawSync(0) once DrawCompleteTicks() has come (then call
	// DrawSyncWoken() if any were woken).
	void (*onDrawSyncDone)(void *user) = nullptr;
	// Optional, enables asynchronous interrupt delivery: called when a GE interrupt is raised, which
	// happens at atTicks. The GE stays stopped; the HLE calls TakeInterrupt() when it can take it (from
	// then), runs the callback it names, then calls InterruptDone(). onSignal/onFinish are unused then.
	void (*onInterruptPending)(void *user, u64 atTicks) = nullptr;
	// Optional, the CPU clock in cycles (222 MHz). The GE then takes time: lists run at once, but their
	// interrupts and completion happen when they would on hardware (PPSSPP's cycle estimates), and
	// sceGeListSync/sceGeDrawSync wait for that. Without it, everything happens at time 0.
	u64 (*getTicks)(void *user) = nullptr;
};
void SetHooks(const Hooks &h);

// sceGe_user semantics (see reference/ppsspp/docs/sceGe.md).
int EnqueueList(u32 listAddr, u32 stallAddr, int callbackId, u32 optParamAddr, bool head);
int DequeueList(int listId);
int UpdateStallAddr(int listId, u32 stallAddr);
// mode 1: the list's status (0 COMPLETED, 1 QUEUED, 2 DRAWING, 3 STALLING, 4 PAUSED) or an error.
// mode 0: 0 if there is nothing to wait for, SYNC_WAIT if the caller must block until onListDone(listId).
// The HLE checks for an interrupt/dispatch-disabled context itself (0x80020064 / 0x800201A7).
int ListSync(int listId, int mode);
// mode 1: COMPLETED/DRAWING/STALLING. mode 0: 0 (completed lists recycled) or SYNC_WAIT, then block until
// onDrawSyncDone and return 0.
int DrawSync(int mode);
constexpr int SYNC_WAIT = 1;
// After onDrawSyncDone woke at least one thread: completed lists become free (like the firmware's wake).
void DrawSyncWoken();
// When the waits of sceGeListSync(listId, 0) / sceGeDrawSync(0) end (~0: not yet known).
u64 ListWaitTicks(int listId);
u64 DrawCompleteTicks();
int Break(int mode, u32 paramAddr);
int Continue();
u32 GetCmd(int cmd);
int GetMatrix(int type, u32 outAddr);
int GetStack(int index, u32 stackAddr);
// sceGeSaveContext/RestoreContext (512 words; -1 / 0x80000021 while the GE is executing).
int SaveContext(u32 ctxAddr);
int RestoreContext(u32 ctxAddr);
// sceGeEdramSetAddrTranslation: returns the previous value or an error.
int SetEdramAddrTranslation(u32 value);

// Process queued lists until all are stalled/finished/paused (and, in synchronous mode, delivers the
// interrupts that raises). Reentrant calls (from hooks) are folded into the outer one.
void Run();

// Asynchronous interrupts (Hooks::onInterruptPending set).
struct Interrupt {
	int listId;
	int callbackId;   // -1: no callback to run
	bool isFinish;    // true: finish callback, false: signal callback
	u32 arg;          // low 16 bits of the SIGNAL/FINISH command
	u32 listPc;       // the address after the END (callback a2 for SDK > 0x02000010)
};
// Starts handling the oldest raised interrupt; false if none. Then run the callback (if any) and call
// InterruptDone(), which resumes the GE.
bool TakeInterrupt(Interrupt *out);
void InterruptDone();

// Interrupt masking for sceKernelCpuSuspendIntr: while disabled, raised interrupts wait (the queue stands
// still at the first FINISH). Call Run() after re-enabling.
void SetInterruptsEnabled(bool enabled);
// During onSignal/onFinish: the address after the END (callback a2 for SDK > 0x02000010).
u32 CurrentInterruptListPc();

// Behaviour that depends on the game's SDK version (sceKernelGetCompiledSdkVersion). Default 0x06060010.
void SetSdkVersion(u32 version);
// Main RAM size for address validation (default 64 MB at 0x08000000).
void SetRamSize(u32 bytes);
// Rasterizer worker threads: 0 = automatic, 1 = draw on the calling thread only.
void SetRenderThreads(int count);

// For sceDisplay: convert a framebuffer in guest memory to RGBA8888 (R,G,B,A bytes in memory).
// pixelFormat: 0=565, 1=5551, 2=4444, 3=8888. stride in pixels. Alpha is forced to 255.
void ReadFramebuffer(u32 addr, int stride, int pixelFormat, int width, int height, u32 *outRgba);

// Statistics, reset by ResetStats().
struct Stats {
	u64 commands;
	u64 prims;
	u64 vertices;
	u64 pixelsSubmitted;   // bounding area of rasterized primitives
	u64 flushes;
	u64 threadedFlushes;
	u64 transfers;
	u64 hostNs;  // host time spent processing lists (and writing GPU rendering back), in ns
};
Stats GetStats();
void ResetStats();

}  // namespace gpu
