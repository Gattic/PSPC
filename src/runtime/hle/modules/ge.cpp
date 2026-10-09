// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceGe_user / sceGe_driver on top of the pspc_gpu library (Core/HLE/sceGe.cpp's semantics). The GE runs
// synchronously inside these calls; its SIGNAL/FINISH interrupts are delivered from the scheduler in
// interrupt context, and sceGeListSync/sceGeDrawSync block the calling thread until the GE wakes it.
#include <cstring>
#include <functional>
#include <map>

#include "allegrex/sem.h"
#include "common/log.h"
#include "gpu/ge.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace {

struct GeCallback {
	u32 signalFunc = 0, signalArg = 0;
	u32 finishFunc = 0, finishArg = 0;
	u32 gp = 0;
	bool used = false;
};

GeCallback g_callbacks[16];
WaitQueue g_drawWaiters;
std::map<int, WaitQueue> g_listWaiters;
constexpr SceUID kDrawSyncWaitId = 1;
bool g_intrQueued = false;  // a GE interrupt was queued during the current call

// sceKernelGetCompiledSdkVersion of the running program (0 until it sets one, as on hardware).
u32 CompiledSdkVersion() {
	static const hle::HleFunction *fn = hle::FindFunction("SysMemUserForUser", 0xFC114573);
	if (!fn || !fn->fn)
		return 0;
	Ctx c;
	memset(&c, 0, sizeof(c));
	fn->fn(&c);
	return c.r[2];
}

// --- Time ---
// The GE's clock is the CPU's, in cycles. A call runs the GE at its start; the cycles the call itself
// takes are charged after, and what the GE did within them has happened by the time it returns.
constexpr u64 kCyclesPerUs = sched::kCpuHz / 1000000;
u64 NowTicks() { return sched::NowCycles(); }

bool g_inCall = false;
u64 g_callStart = 0;  // the GE's clock during a call
u64 g_horizon = 0;    // where the CPU's clock is when the call returns
// How long dispatching an interrupt and returning from it takes (a guess).
constexpr s64 kIntrReturnCycles = 500;

// Makes a stretch of host code a GE call (see GeCall), restoring the previous state after.
struct CallScope {
	bool wasInCall = g_inCall;
	u64 wasStart = g_callStart, wasHorizon = g_horizon;
	explicit CallScope(s64 charge) {
		g_inCall = true;
		g_callStart = NowTicks();
		g_horizon = g_callStart + (u64)charge;
	}
	void Restore() {
		g_inCall = wasInCall;
		g_callStart = wasStart;
		g_horizon = wasHorizon;
	}
};

u64 GetTicks(void *) {
	return g_inCall ? g_callStart : NowTicks();
}

// Runs fn once the clock reaches `at` (now if it has).
void AtTicks(u64 at, std::function<void()> fn) {
	if (at <= (g_inCall ? g_horizon : NowTicks())) {
		fn();
		return;
	}
	sched::ScheduleEvent((at + kCyclesPerUs - 1) / kCyclesPerUs, std::move(fn));
}

// One sceGe call that may run the GE.
class GeCall {
public:
	// charge: what the call costs, after the GE ran.
	explicit GeCall(s64 charge) : charge_(charge), scope_(charge) {
		gpu::SetRamSize(mem::g_ramSize);
		gpu::SetSdkVersion(CompiledSdkVersion());
		gpu::SetInterruptsEnabled(*g_intrEnabled != 0 && !sched::InInterrupt());
	}
	~GeCall() { End(); }

	// Before blocking: the clock moves on without us.
	void End() {
		if (ended_)
			return;
		ended_ = true;
		scope_.Restore();
		sched::ChargeCycles(charge_);
		// An interrupt the GE raised meanwhile is taken right away, as on hardware.
		if (g_intrQueued) {
			g_intrQueued = false;
			sched::Reschedule();
		}
	}

private:
	s64 charge_;
	CallScope scope_;
	bool ended_ = false;
};

// Runs fn as the GE going on while an interrupt is dispatched or returns: what it does in that
// time (often the FINISH right after a signal) interrupts again before the thread runs.
template <typename F>
auto InInterruptWindow(F fn) {
	CallScope scope(kIntrReturnCycles);
	auto r = fn();
	scope.Restore();
	return r;
}

// The GE interrupt: one SIGNAL or FINISH, its callback run in interrupt context.
void DeliverInterrupt() {
	gpu::Interrupt intr;
	if (!InInterruptWindow([&] { return gpu::TakeInterrupt(&intr); }))
		return;
	if (intr.callbackId >= 0 && intr.callbackId < 16 && g_callbacks[intr.callbackId].used) {
		const GeCallback &cb = g_callbacks[intr.callbackId];
		const u32 func = intr.isFinish ? cb.finishFunc : cb.signalFunc;
		const u32 arg = intr.isFinish ? cb.finishArg : cb.signalArg;
		if (func)
			sched::RunInterruptHandler(func, cb.gp, { intr.arg, arg, intr.listPc });
	}
	InInterruptWindow([] {
		gpu::InterruptDone();
		return 0;
	});
	sched::ChargeCycles(kIntrReturnCycles);
}

void OnInterruptPending(void *, u64 at) {
	g_intrQueued = true;
	AtTicks(at, [] { sched::QueueInterrupt(&DeliverInterrupt); });
}

void WakeListWaiters(int listId) {
	if (gpu::ListWaitTicks(listId) > NowTicks() && !g_inCall)
		return;  // re-enqueued since: a later onListDone wakes them
	auto it = g_listWaiters.find(listId);
	if (it != g_listWaiters.end())
		it->second.WakeAll(WAITTYPE_GELISTSYNC, listId, 0);
}

void WakeDrawWaiters() {
	if (gpu::DrawCompleteTicks() > NowTicks() && !g_inCall)
		return;
	// The firmware frees completed lists when it wakes sceGeDrawSync waiters.
	if (g_drawWaiters.WakeAll(WAITTYPE_GEDRAWSYNC, kDrawSyncWaitId, 0))
		gpu::DrawSyncWoken();
}

void OnListDone(void *, int listId) {
	AtTicks(gpu::ListWaitTicks(listId), [listId] { WakeListWaiters(listId); });
}

void OnDrawSyncDone(void *) {
	const u64 at = gpu::DrawCompleteTicks();
	if (at != ~0ull)
		AtTicks(at, [] { WakeDrawWaiters(); });
}

// Where a thread can't wait: refused even with nothing to wait for.
int CheckCanWait() {
	if (!sched::DispatchEnabled())
		return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (sched::InInterrupt())
		return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	return 0;
}

u32 sceGeEdramGetAddr() {
	sched::ChargeCycles(150);
	return 0x04000000;
}

u32 sceGeEdramGetSize() {
	return 0x00200000;
}

int sceGeEdramSetAddrTranslation(u32 value) {
	return gpu::SetEdramAddrTranslation(value);
}

int sceGeListEnQueue(u32 listAddr, u32 stallAddr, int callbackId, u32 optParamAddr) {
	GeCall call(490);
	const int id = gpu::EnqueueList(listAddr, stallAddr, callbackId, optParamAddr, false);
	LOGD("sceGe", "sceGeListEnQueue(%08x, %08x, %d, %08x) = %08x", listAddr, stallAddr, callbackId, optParamAddr, id);
	return id;
}

int sceGeListEnQueueHead(u32 listAddr, u32 stallAddr, int callbackId, u32 optParamAddr) {
	GeCall call(480);
	const int id = gpu::EnqueueList(listAddr, stallAddr, callbackId, optParamAddr, true);
	LOGD("sceGe", "sceGeListEnQueueHead(%08x, %08x, %d, %08x) = %08x", listAddr, stallAddr, callbackId, optParamAddr, id);
	return id;
}

int sceGeListDeQueue(u32 listId) {
	GeCall call(0);
	return gpu::DequeueList((int)listId);
}

int sceGeListUpdateStallAddr(u32 listId, u32 stallAddr) {
	GeCall call(190);
	return gpu::UpdateStallAddr((int)listId, stallAddr);
}

// The syncs take a while themselves: what the GE (and its interrupts) did meanwhile counts.
void SyncCallOverhead(s64 cycles) {
	sched::ChargeCycles(cycles);
	sched::Reschedule();
}

int sceGeListSync(u32 listId, u32 mode) {
	SyncCallOverhead(220);
	GeCall call(0);
	const int r = gpu::ListSync((int)listId, (int)mode);
	if (mode != 0 || r < 0)
		return r;
	if (int err = CheckCanWait())
		return err;
	if (r == gpu::SYNC_WAIT) {
		PspThread *t = sched::Current();
		if (!t)
			return 0;
		g_listWaiters[(int)listId].Add(t->uid);
		call.End();
		sched::Wait(WAITTYPE_GELISTSYNC, (SceUID)listId, 0, false);
	}
	return 0;
}

int sceGeDrawSync(u32 mode) {
	SyncCallOverhead(1240);
	GeCall call(0);
	if (mode != 0)
		return gpu::DrawSync((int)mode);
	if (int err = CheckCanWait())
		return err;
	if (gpu::DrawSync(0) == gpu::SYNC_WAIT) {
		PspThread *t = sched::Current();
		if (!t)
			return 0;
		g_drawWaiters.Add(t->uid);
		call.End();
		sched::Wait(WAITTYPE_GEDRAWSYNC, kDrawSyncWaitId, 0, false);
	}
	return 0;
}

int sceGeBreak(u32 mode, u32 paramAddr) {
	GeCall call(0);
	return gpu::Break((int)mode, paramAddr);
}

int sceGeContinue() {
	GeCall call(220);
	return gpu::Continue();
}

int sceGeSetCallback(u32 structAddr) {
	int cbid = -1;
	for (int i = 0; i < 16; ++i) {
		if (!g_callbacks[i].used) {
			cbid = i;
			break;
		}
	}
	if (cbid < 0)
		return (int)SCE_KERNEL_ERROR_OUT_OF_MEMORY;
	GeCallback &cb = g_callbacks[cbid];
	cb = GeCallback{};
	if (mem::IsValidRange(structAddr, 16)) {
		cb.signalFunc = GRead32(structAddr);
		cb.signalArg = GRead32(structAddr + 4);
		cb.finishFunc = GRead32(structAddr + 8);
		cb.finishArg = GRead32(structAddr + 12);
	}
	PspThread *t = sched::Current();
	cb.gp = t ? t->ctx.r[28] : 0;
	cb.used = true;
	return cbid;
}

int sceGeUnsetCallback(u32 cbid) {
	if (cbid >= 16)
		return (int)SCE_KERNEL_ERROR_INVALID_ID;
	g_callbacks[cbid] = GeCallback{};
	return 0;
}

int sceGeSaveContext(u32 ctxAddr) {
	GeCall call(0);
	return gpu::SaveContext(ctxAddr);
}

int sceGeRestoreContext(u32 ctxAddr) {
	GeCall call(0);
	return gpu::RestoreContext(ctxAddr);
}

int sceGeGetMtx(int type, u32 matrixPtr) {
	return gpu::GetMatrix(type, matrixPtr);
}

u32 sceGeGetCmd(int cmd) {
	return gpu::GetCmd(cmd);
}

int sceGeGetStack(int index, u32 stackPtr) {
	return gpu::GetStack(index, stackPtr);
}

}  // namespace

void Register_sceGe_user() {
	gpu::Init();
	for (GeCallback &cb : g_callbacks)
		cb = GeCallback{};
	g_drawWaiters.threads.clear();
	g_listWaiters.clear();
	g_intrQueued = false;
	gpu::Hooks hooks;
	hooks.onInterruptPending = &OnInterruptPending;
	hooks.onListDone = &OnListDone;
	hooks.onDrawSyncDone = &OnDrawSyncDone;
	hooks.getTicks = &GetTicks;
	gpu::SetHooks(hooks);

	static const hle::HleFunction funcs[] = {
		HLEFN(0xE47E40E4, sceGeEdramGetAddr),
		HLEFN(0xAB49E76A, sceGeListEnQueue),
		HLEFN(0x1C0D95A6, sceGeListEnQueueHead),
		HLEFN(0xE0D68148, sceGeListUpdateStallAddr),
		HLEFN(0x03444EB4, sceGeListSync),
		HLEFN(0xB287BD61, sceGeDrawSync),
		HLEFN(0xB448EC0D, sceGeBreak),
		HLEFN(0x4C06E472, sceGeContinue),
		HLEFN(0xA4FC06A4, sceGeSetCallback),
		HLEFN(0x05DB22CE, sceGeUnsetCallback),
		HLEFN(0x1F6752AD, sceGeEdramGetSize),
		HLEFN(0xB77905EA, sceGeEdramSetAddrTranslation),
		HLEFN(0xDC93CFEF, sceGeGetCmd),
		HLEFN(0x57C8945B, sceGeGetMtx),
		HLEFN(0x438A385A, sceGeSaveContext),
		HLEFN(0x0BF608FB, sceGeRestoreContext),
		HLEFN(0x5FB86AB0, sceGeListDeQueue),
		HLEFN(0xE66CB92E, sceGeGetStack),
	};
	hle::RegisterModule("sceGe_user", funcs);
	// Same functions under the driver NIDs (kernel modules such as chfont.prx).
	hle::RegisterModule("sceGe_driver", funcs);
}
