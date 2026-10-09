// SPDX-License-Identifier: GPL-2.0-or-later
// Kernel_Library (interrupt control, LwMutex, memcpy/memset) and InterruptManager.
// LwMutex semantics follow PPSSPP's sceKernelMutex.cpp (Portions derived from PPSSPP,
// https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include <algorithm>
#include <map>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/interrupts.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace intr {

namespace {
struct SubIntr {
	u32 handler = 0, arg = 0, gp = 0;
	bool enabled = false;
};
std::map<int, std::map<int, SubIntr>> g_subIntrs;
}  // namespace

void TriggerAll(int intrNo) {
	auto it = g_subIntrs.find(intrNo);
	if (it == g_subIntrs.end()) return;
	for (auto &kv : it->second) {
		const SubIntr &s = kv.second;
		if (!s.enabled || !s.handler) continue;
		sched::RunInterruptHandler(s.handler, s.gp, {(u32)kv.first, s.arg});
	}
}

void TriggerSub(int intrNo, int subIntrNo, u32 arg) {
	auto it = g_subIntrs.find(intrNo);
	if (it == g_subIntrs.end()) return;
	auto s = it->second.find(subIntrNo);
	if (s == it->second.end() || !s->second.enabled || !s->second.handler) return;
	sched::RunInterruptHandler(s->second.handler, s->second.gp, {(u32)subIntrNo, arg});
}

bool HasEnabledHandler(int intrNo, int subIntrNo) {
	auto it = g_subIntrs.find(intrNo);
	if (it == g_subIntrs.end()) return false;
	auto s = it->second.find(subIntrNo);
	return s != it->second.end() && s->second.enabled;
}

int RegisterSub(int intrNo, int subIntrNo, u32 handler, u32 arg) {
	if (intrNo < 0 || intrNo >= PSP_NUMBER_INTERRUPTS) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	if (subIntrNo < 0 || subIntrNo >= 32) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	auto &tbl = g_subIntrs[intrNo];
	if (tbl.count(subIntrNo) && tbl[subIntrNo].handler) return (int)SCE_KERNEL_ERROR_FOUND_HANDLER;
	SubIntr s;
	s.handler = handler;
	s.arg = arg;
	PspThread *t = sched::Current();
	s.gp = t ? t->ctx.r[28] : 0;
	tbl[subIntrNo] = s;
	return 0;
}

int ReleaseSub(int intrNo, int subIntrNo) {
	auto it = g_subIntrs.find(intrNo);
	if (intrNo < 0 || intrNo >= PSP_NUMBER_INTERRUPTS || subIntrNo < 0 || subIntrNo >= 32)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	if (it == g_subIntrs.end() || !it->second.count(subIntrNo)) return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	it->second.erase(subIntrNo);
	return 0;
}

int EnableSub(int intrNo, int subIntrNo, bool enable) {
	if (intrNo < 0 || intrNo >= PSP_NUMBER_INTERRUPTS || subIntrNo < 0 || subIntrNo >= 32)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	auto &tbl = g_subIntrs[intrNo];
	if (!tbl.count(subIntrNo)) {
		if (!enable) return 0;
		// Enabling an unregistered handler is allowed; it starts firing once registered.
		tbl[subIntrNo] = SubIntr{};
	}
	tbl[subIntrNo].enabled = enable;
	return 0;
}

}  // namespace intr

namespace {

// ---- interrupt state ----

u32 sceKernelCpuSuspendIntr() {
	u32 old = *g_intrEnabled ? 1 : 0;
	*g_intrEnabled = 0;
	return old;
}
void sceKernelCpuResumeIntr(u32 flags) {
	if (flags) {
		*g_intrEnabled = 1;
		sched::Reschedule();
	}
}
int sceKernelIsCpuIntrSuspended(u32 flags) { return flags == 0 ? 1 : 0; }
int sceKernelIsCpuIntrEnable() { return *g_intrEnabled ? 1 : 0; }

u32 sceKernelMemset(u32 addr, int c, u32 n) {
	mem::Memset(addr, (u8)c, n);
	return addr;
}
u32 sceKernelMemcpy(u32 dst, u32 src, u32 n) {
	mem::Memcpy(dst, src, n);
	return dst;
}

// ---- LwMutex ----

struct LwWorkarea {
	s32 lockLevel;
	SceUID lockThread;
	u32 attr;
	s32 numWaitThreads;
	SceUID uid;
	s32 pad[3];
};

struct LwMutex : KObject {
	static constexpr KType kType = KType::LwMutex;
	KType Type() const override { return kType; }
	u32 workarea = 0;
	WaitQueue waiters;
};

constexpr u32 PSP_MUTEX_ATTR_PRIORITY = 0x100;
constexpr u32 PSP_MUTEX_ATTR_ALLOW_RECURSIVE = 0x200;

int sceKernelCreateLwMutex(u32 workareaPtr, const char *name, u32 attr, s32 initCount, u32 optPtr) {
	(void)optPtr;
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (attr >= 0x400) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (initCount < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && initCount > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	auto *m = new LwMutex();
	m->name = name;
	m->workarea = workareaPtr;
	SceUID id = Register(m);
	LwWorkarea w{};
	w.lockLevel = initCount;
	w.lockThread = initCount ? sched::CurrentId() : -1;
	w.attr = attr;
	w.numWaitThreads = 0;
	w.uid = id;
	mem::WriteBytes(workareaPtr, &w, sizeof(w));
	return 0;
}

int sceKernelDeleteLwMutex(u32 workareaPtr) {
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	LwWorkarea *w = (LwWorkarea *)GPtr(workareaPtr);
	LwMutex *m = Get<LwMutex>(w->uid);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_LWMUTEXID;
	bool woke = m->waiters.WakeAll(WAITTYPE_LWMUTEX, m->uid, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(m->uid);
	w->uid = 0;
	w->numWaitThreads = 0;
	if (woke) sched::Reschedule();
	return 0;
}

// Returns 1 if locked, 0 if it must wait; sets *error on failure.
int LwTryLock(LwWorkarea *w, s32 count, int *error) {
	if (count <= 0) {
		*error = (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		return 0;
	}
	if (count > 1 && !(w->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)) {
		*error = (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		return 0;
	}
	if ((s64)w->lockLevel + count < 0) {
		*error = (int)SCE_KERNEL_ERROR_LWMUTEX_LOCK_OVERFLOW;
		return 0;
	}
	if (!Get<LwMutex>(w->uid)) {
		*error = (int)SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
		return 0;
	}
	SceUID cur = sched::CurrentId();
	if (w->lockLevel == 0) {
		w->lockLevel = count;
		w->lockThread = cur;
		return 1;
	}
	if (w->lockThread == cur) {
		if (w->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) {
			w->lockLevel += count;
			return 1;
		}
		*error = (int)SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE_NOT_ALLOWED;
		return 0;
	}
	return 0;
}

int LockLw(u32 workareaPtr, s32 count, u32 timeoutPtr, bool cb) {
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	LwWorkarea *w = (LwWorkarea *)GPtr(workareaPtr);
	int error = 0;
	if (LwTryLock(w, count, &error)) return 0;
	if (error) return error;
	if (timeoutPtr && mem::IsValidRange(timeoutPtr, 4) && GRead32(timeoutPtr) == 0)
		return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	LwMutex *m = Get<LwMutex>(w->uid);
	PspThread *t = sched::Current();
	t->waitValue[0] = (u32)count;
	m->waiters.Add(t->uid);
	w->numWaitThreads++;
	SceUID mid = m->uid;
	t->timeoutHook = [mid]() {
		if (LwMutex *mm = Get<LwMutex>(mid); mm && mem::IsValidRange(mm->workarea, sizeof(LwWorkarea)))
			((LwWorkarea *)GPtr(mm->workarea))->numWaitThreads--;
	};
	return sched::Wait(WAITTYPE_LWMUTEX, mid, timeoutPtr, cb);
}

int sceKernelLockLwMutex(u32 w, s32 count, u32 timeoutPtr) { return LockLw(w, count, timeoutPtr, false); }
int sceKernelLockLwMutexCB(u32 w, s32 count, u32 timeoutPtr) { return LockLw(w, count, timeoutPtr, true); }

int sceKernelTryLockLwMutex(u32 workareaPtr, s32 count) {
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	int error = 0;
	if (LwTryLock((LwWorkarea *)GPtr(workareaPtr), count, &error)) return 0;
	// The pre-6.00 entry point reports every failure as "locked".
	return (int)SCE_KERNEL_ERROR_MUTEX_LOCKED;
}

int sceKernelTryLockLwMutex_600(u32 workareaPtr, s32 count) {
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	int error = 0;
	if (LwTryLock((LwWorkarea *)GPtr(workareaPtr), count, &error)) return 0;
	return error ? error : (int)SCE_KERNEL_ERROR_LWMUTEX_LOCKED;
}

int sceKernelUnlockLwMutex(u32 workareaPtr, s32 count) {
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	LwWorkarea *w = (LwWorkarea *)GPtr(workareaPtr);
	if (w->uid == -1) return (int)SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
	if (count <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((w->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && count > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (w->lockLevel == 0 || w->lockThread != sched::CurrentId()) return (int)SCE_KERNEL_ERROR_LWMUTEX_UNLOCKED;
	if (w->lockLevel < count) return (int)SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW;
	w->lockLevel -= count;
	if (w->lockLevel != 0) return 0;
	w->lockThread = -1;
	LwMutex *m = Get<LwMutex>(w->uid);
	if (!m) return 0;
	m->waiters.Cleanup(WAITTYPE_LWMUTEX, m->uid);
	if (m->waiters.threads.empty()) return 0;
	if (w->attr & PSP_MUTEX_ATTR_PRIORITY) m->waiters.SortByPriority();
	SceUID next = m->waiters.threads.front();
	m->waiters.threads.erase(m->waiters.threads.begin());
	PspThread *t = Get<PspThread>(next);
	w->lockLevel = (s32)t->waitValue[0];
	w->lockThread = next;
	w->numWaitThreads--;
	sched::Wake(t, 0);
	sched::Reschedule();
	return 0;
}

static int ReferLw(LwWorkarea *w, u32 infoPtr) {
	LwMutex *m = Get<LwMutex>(w->uid);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_LWMUTEXID;
	if (!mem::IsValidRange(infoPtr, 4)) return 0;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		SceUID uid;
		u32 workarea;
		s32 initCount, lockLevel;
		SceUID lockThread;
		s32 numWaitThreads;
	} info{};
	info.size = sizeof(info);
	strncpy(info.name, m->name.c_str(), 31);
	info.attr = w->attr;
	info.uid = m->uid;
	info.workarea = m->workarea;
	info.lockLevel = w->lockLevel;
	info.lockThread = w->lockThread;
	info.numWaitThreads = m->waiters.Count(WAITTYPE_LWMUTEX, m->uid);
	u32 wanted = std::min<u32>(GRead32(infoPtr), sizeof(info));
	if (wanted) mem::WriteBytes(infoPtr, &info, wanted);
	return 0;
}

int sceKernelReferLwMutexStatus(u32 workareaPtr, u32 infoPtr) {
	if (!mem::IsValidRange(workareaPtr, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	return ReferLw((LwWorkarea *)GPtr(workareaPtr), infoPtr);
}

int sceKernelReferLwMutexStatusByID(SceUID id, u32 infoPtr) {
	LwMutex *m = Get<LwMutex>(id);
	if (!m || !mem::IsValidRange(m->workarea, sizeof(LwWorkarea))) return (int)SCE_KERNEL_ERROR_UNKNOWN_LWMUTEXID;
	return ReferLw((LwWorkarea *)GPtr(m->workarea), infoPtr);
}

int KL_sceKernelGetThreadId() { return sched::CurrentId(); }
int KL_sceKernelCheckThreadStack() {
	PspThread *t = sched::Current();
	return t ? (int)(t->ctx.r[29] - t->stackAddr) : 0;
}
void KL_sceKernelExitGame() { sched::RequestStop(0); }

// ---- InterruptManager ----

int sceKernelRegisterSubIntrHandler(int intrNo, int subIntrNo, u32 handler, u32 arg) {
	return intr::RegisterSub(intrNo, subIntrNo, handler, arg);
}
int sceKernelReleaseSubIntrHandler(int intrNo, int subIntrNo) { return intr::ReleaseSub(intrNo, subIntrNo); }
int sceKernelEnableSubIntr(int intrNo, int subIntrNo) { return intr::EnableSub(intrNo, subIntrNo, true); }
int sceKernelDisableSubIntr(int intrNo, int subIntrNo) { return intr::EnableSub(intrNo, subIntrNo, false); }
int QueryIntrHandlerInfo(u32, u32, u32) { return 0; }

}  // namespace

void Register_Kernel_Library() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x092968F4, sceKernelCpuSuspendIntr),
		HLEFN(0x5F10D406, sceKernelCpuResumeIntr),
		HLEFN(0x3B84732D, sceKernelCpuResumeIntr),
		HLEFN(0x47A0B729, sceKernelIsCpuIntrSuspended),
		HLEFN(0xB55249D2, sceKernelIsCpuIntrEnable),
		HLEFN(0xA089ECA4, sceKernelMemset),
		HLEFN(0xDC692EE3, sceKernelTryLockLwMutex),
		HLEFN(0x37431849, sceKernelTryLockLwMutex_600),
		HLEFN(0xBEA46419, sceKernelLockLwMutex),
		HLEFN(0x1FC64E09, sceKernelLockLwMutexCB),
		HLEFN(0x15B6446B, sceKernelUnlockLwMutex),
		HLEFN(0xC1734599, sceKernelReferLwMutexStatus),
		HLEFN(0x293B45B8, KL_sceKernelGetThreadId),
		HLEFN(0xD13BDE95, KL_sceKernelCheckThreadStack),
		HLEFN(0x1839852A, sceKernelMemcpy),
		HLEFN(0x05572A5F, KL_sceKernelExitGame),
	};
	hle::RegisterModule("Kernel_Library", funcs);

	static const hle::HleFunction thman[] = {
		HLEFN(0x19CFF145, sceKernelCreateLwMutex),
		HLEFN(0x60107536, sceKernelDeleteLwMutex),
		HLEFN(0x4C145944, sceKernelReferLwMutexStatusByID),
	};
	hle::RegisterModule("ThreadManForUser", thman);
}

void Register_InterruptManager() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xCA04A2B9, sceKernelRegisterSubIntrHandler),
		HLEFN(0xD61E6961, sceKernelReleaseSubIntrHandler),
		HLEFN(0xFB8E22EC, sceKernelEnableSubIntr),
		HLEFN(0x8A389411, sceKernelDisableSubIntr),
		HLEFN(0xD2E8363F, QueryIntrHandlerInfo),
	};
	hle::RegisterModule("InterruptManager", funcs);
}
