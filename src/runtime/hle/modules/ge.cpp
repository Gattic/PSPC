// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceGe_user / sceGe_driver on top of the pspc_gpu library (Core/HLE/sceGe.cpp's semantics). The GE runs
// synchronously inside these calls; its SIGNAL/FINISH interrupts are delivered from the scheduler in
// interrupt context, and sceGeListSync/sceGeDrawSync block the calling thread until the GE wakes it.
#include <cstring>
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

// The parts of the GE's behaviour that depend on the caller's state.
void Prepare() {
	gpu::SetRamSize(mem::g_ramSize);
	gpu::SetSdkVersion(CompiledSdkVersion());
	gpu::SetInterruptsEnabled(*g_intrEnabled != 0 && !sched::InInterrupt());
}

// After a call that ran the GE: an interrupt it raised is taken right away, as on hardware.
template <typename T>
T Done(T result) {
	if (g_intrQueued) {
		g_intrQueued = false;
		sched::Reschedule();
	}
	return result;
}

// The GE interrupt: one SIGNAL or FINISH, its callback run in interrupt context.
void DeliverInterrupt() {
	gpu::Interrupt intr;
	if (!gpu::TakeInterrupt(&intr))
		return;
	if (intr.callbackId >= 0 && intr.callbackId < 16 && g_callbacks[intr.callbackId].used) {
		const GeCallback &cb = g_callbacks[intr.callbackId];
		const u32 func = intr.isFinish ? cb.finishFunc : cb.signalFunc;
		const u32 arg = intr.isFinish ? cb.finishArg : cb.signalArg;
		if (func)
			sched::RunInterruptHandler(func, cb.gp, { intr.arg, arg, intr.listPc });
	}
	gpu::InterruptDone();
}

void OnInterruptPending(void *) {
	g_intrQueued = true;
	sched::QueueInterrupt(&DeliverInterrupt);
}

void OnListDone(void *, int listId) {
	auto it = g_listWaiters.find(listId);
	if (it != g_listWaiters.end())
		it->second.WakeAll(WAITTYPE_GELISTSYNC, listId, 0);
}

void OnDrawSyncDone(void *) {
	// The firmware frees completed lists when it wakes sceGeDrawSync waiters.
	if (g_drawWaiters.WakeAll(WAITTYPE_GEDRAWSYNC, kDrawSyncWaitId, 0))
		gpu::DrawSyncWoken();
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
	Prepare();
	const int id = gpu::EnqueueList(listAddr, stallAddr, callbackId, optParamAddr, false);
	sched::ChargeCycles(490);
	LOGD("sceGe", "sceGeListEnQueue(%08x, %08x, %d, %08x) = %08x", listAddr, stallAddr, callbackId, optParamAddr, id);
	return Done(id);
}

int sceGeListEnQueueHead(u32 listAddr, u32 stallAddr, int callbackId, u32 optParamAddr) {
	Prepare();
	const int id = gpu::EnqueueList(listAddr, stallAddr, callbackId, optParamAddr, true);
	sched::ChargeCycles(480);
	LOGD("sceGe", "sceGeListEnQueueHead(%08x, %08x, %d, %08x) = %08x", listAddr, stallAddr, callbackId, optParamAddr, id);
	return Done(id);
}

int sceGeListDeQueue(u32 listId) {
	Prepare();
	return Done(gpu::DequeueList((int)listId));
}

int sceGeListUpdateStallAddr(u32 listId, u32 stallAddr) {
	Prepare();
	sched::ChargeCycles(190);
	return Done(gpu::UpdateStallAddr((int)listId, stallAddr));
}

int sceGeListSync(u32 listId, u32 mode) {
	Prepare();
	sched::ChargeCycles(220);
	const int r = gpu::ListSync((int)listId, (int)mode);
	if (mode != 0 || r < 0)
		return Done(r);
	if (int err = CheckCanWait())
		return Done(err);
	if (r == gpu::SYNC_WAIT) {
		PspThread *t = sched::Current();
		if (!t)
			return 0;
		g_listWaiters[(int)listId].Add(t->uid);
		g_intrQueued = false;
		sched::Wait(WAITTYPE_GELISTSYNC, (SceUID)listId, 0, false);
	}
	return Done(0);
}

int sceGeDrawSync(u32 mode) {
	Prepare();
	sched::ChargeCycles(1240);
	if (mode != 0)
		return Done(gpu::DrawSync((int)mode));
	if (int err = CheckCanWait())
		return Done(err);
	if (gpu::DrawSync(0) == gpu::SYNC_WAIT) {
		PspThread *t = sched::Current();
		if (!t)
			return 0;
		g_drawWaiters.Add(t->uid);
		g_intrQueued = false;
		sched::Wait(WAITTYPE_GEDRAWSYNC, kDrawSyncWaitId, 0, false);
	}
	return Done(0);
}

int sceGeBreak(u32 mode, u32 paramAddr) {
	Prepare();
	return Done(gpu::Break((int)mode, paramAddr));
}

int sceGeContinue() {
	Prepare();
	const int r = gpu::Continue();
	sched::ChargeCycles(220);
	return Done(r);
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
	Prepare();
	return gpu::SaveContext(ctxAddr);
}

int sceGeRestoreContext(u32 ctxAddr) {
	Prepare();
	return Done(gpu::RestoreContext(ctxAddr));
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
