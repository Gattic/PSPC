// SPDX-License-Identifier: GPL-2.0-or-later
// Kernel_Library (interrupt control, LwMutex, memcpy/memset) and InterruptManager.
// LwMutex semantics follow PPSSPP's sceKernelMutex.cpp and interrupt control its
// sceKernelInterrupt.cpp (Portions derived from PPSSPP, https://github.com/hrydgard/ppsspp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include <algorithm>
#include <map>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/interrupts.h"
#include "runtime/kernel/kernel_hle.h"
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

// What a user mode caller finds on each interrupt, as interruptman checks it: whether a driver
// installed a handler at all, whether it has sub-interrupt slots, and whether user handlers may
// use them (recorded by intr/registersub and intr/releasesub; table from PPSSPP).
enum class UserAccess { NoHandler, NoSubs, KernelSubs, User };

UserAccess AccessOf(int intrNo) {
	switch (intrNo) {
	case PSP_GE_INT:
	case PSP_VBLANK_INT: return UserAccess::User;
	case 4: case 6: case 21: return UserAccess::KernelSubs;
	case 7: case 10: case 12: case 15: case 16: case 17: case 18: case 19: case 20: case 22:
	case 23: case 24: case 26: case 31: case 36: case 50: case 56: case 57: case 58: case 59:
	case 60: case 61: case 65:
		return UserAccess::NoSubs;
	default: return UserAccess::NoHandler;
	}
}

// The vblank slots a user handler may take (the rest are the kernel's), and those the display
// driver already holds.
bool IsUserVblankSub(int sub) { return sub < 16; }
bool IsKernelHeldVblankSub(int sub) { return (sub >= 18 && sub <= 20) || (sub >= 24 && sub <= 26); }

int RegisterSub(int intrNo, int subIntrNo, u32 handler, u32 arg) {
	if ((u32)intrNo >= PSP_NUMBER_INTERRUPTS) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	switch (AccessOf(intrNo)) {
	case UserAccess::NoHandler: return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	case UserAccess::NoSubs:
	case UserAccess::KernelSubs: return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	case UserAccess::User: break;
	}
	if ((u32)subIntrNo >= 32) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	if (intrNo == PSP_VBLANK_INT) {
		if (IsKernelHeldVblankSub(subIntrNo)) return (int)SCE_KERNEL_ERROR_FOUND_HANDLER;
		if (!IsUserVblankSub(subIntrNo)) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	}
	auto &tbl = g_subIntrs[intrNo];
	auto it = tbl.find(subIntrNo);
	if (it != tbl.end() && it->second.handler) return (int)SCE_KERNEL_ERROR_FOUND_HANDLER;
	PspThread *t = sched::Current();
	u32 gp = t ? t->ctx.r[28] : 0;
	if (it != tbl.end()) {
		// Enabled before it was registered: it stays enabled.
		it->second.handler = handler;
		it->second.arg = arg;
		it->second.gp = gp;
	} else {
		SubIntr s;
		s.handler = handler;
		s.arg = arg;
		s.gp = gp;
		tbl[subIntrNo] = s;
	}
	return 0;
}

int ReleaseSub(int intrNo, int subIntrNo) {
	if ((u32)intrNo >= PSP_NUMBER_INTERRUPTS) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	UserAccess access = AccessOf(intrNo);
	if (access == UserAccess::NoHandler) return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	if (access == UserAccess::NoSubs || (u32)subIntrNo >= 32) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	// User code can't have put anything in the kernel's slots, and can't release what's there.
	if (access == UserAccess::KernelSubs || (intrNo == PSP_VBLANK_INT && !IsUserVblankSub(subIntrNo)))
		return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	auto it = g_subIntrs.find(intrNo);
	if (it == g_subIntrs.end()) return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	auto s = it->second.find(subIntrNo);
	if (s == it->second.end() || !s->second.handler) return (int)SCE_KERNEL_ERROR_NOTFOUND_HANDLER;
	// This also disables it.
	it->second.erase(s);
	return 0;
}

int EnableSub(int intrNo, int subIntrNo, bool enable) {
	if ((u32)intrNo >= PSP_NUMBER_INTERRUPTS || (u32)subIntrNo >= 32) return (int)SCE_KERNEL_ERROR_ILLEGAL_INTRCODE;
	auto &tbl = g_subIntrs[intrNo];
	if (!tbl.count(subIntrNo)) {
		// Disabling when not registered isn't an error; enabling first works fine.
		if (!enable) return 0;
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
	sched::ChargeCycles(15);
	return old;
}
void sceKernelCpuResumeIntr(u32 flags) {
	// This is mtic a0, $0, which only looks at bit 0 (intr/mfic).
	if (flags & 1) {
		*g_intrEnabled = 1;
		sched::Reschedule();
	} else {
		*g_intrEnabled = 0;
	}
	sched::ChargeCycles(15);
}
int sceKernelIsCpuIntrSuspended(u32 flags) { return flags == 0 ? 1 : 0; }
int sceKernelIsCpuIntrEnable() { return *g_intrEnabled ? 1 : 0; }

u32 sceKernelMemset(u32 addr, int c, u32 n) {
	mem::Memset(addr, (u8)c, n);
	return addr;
}
u32 sceKernelMemcpy(u32 dst, u32 src, u32 n) {
	if (n == 0 || !mem::IsValidRange(dst, n) || !mem::IsValidRange(src, n)) return dst;
	if (dst + n < src || src + n < dst) {
		mem::Memcpy(dst, src, n);
		return dst;
	}
	// Overlapping: copied forward 8 bytes at a time like the hardware, so a copy to a little
	// further on repeats the start (pspautotests misc/libc; PPSSPP's sceKernelMemcpy).
	u8 *d = GPtr(dst);
	const u8 *s = GPtr(src);
	for (u32 i = n / 8; i > 0; --i, d += 8, s += 8) memmove(d, s, 8);
	for (u32 i = n % 8; i > 0; --i) *d++ = *s++;
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
	u32 attr = 0;
	s32 initCount = 0;
	WaitQueue waiters;  // waitValue[0] = count wanted
};

constexpr u32 PSP_MUTEX_ATTR_PRIORITY = 0x100;
constexpr u32 PSP_MUTEX_ATTR_ALLOW_RECURSIVE = 0x200;

LwWorkarea *Workarea(u32 addr) {
	if (!mem::IsValidRange(addr, sizeof(LwWorkarea))) return nullptr;
	return (LwWorkarea *)GPtr(addr);
}

int sceKernelCreateLwMutex(u32 workareaPtr, const char *name, u32 attr, s32 initCount, u32 optPtr) {
	(void)optPtr;
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (attr >= 0x400) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (initCount < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && initCount > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	auto *m = new LwMutex();
	m->name = TruncName(name);
	m->workarea = workareaPtr;
	m->attr = attr;
	m->initCount = initCount;
	SceUID id = Register(m);
	memset(w, 0, sizeof(*w));
	w->lockLevel = initCount;
	w->lockThread = initCount ? sched::CurrentId() : 0;
	w->attr = attr;
	w->uid = id;
	return 0;
}

// Ends a waiter's wait: handing it the lock (result 0) or with an error.
bool LwUnlockFor(LwMutex *m, LwWorkarea *w, PspThread *t, s32 result) {
	if (!WaitQueue::IsWaitingOn(t, WAITTYPE_LWMUTEX, m->uid)) return false;
	if (result == 0) {
		w->lockLevel = (s32)t->waitValue[0];
		w->lockThread = t->uid;
	}
	sched::Wake(t, result);
	return true;
}

int sceKernelDeleteLwMutex(u32 workareaPtr) {
	if (!workareaPtr || !mem::IsValidAddress(workareaPtr)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	LwMutex *m = Get<LwMutex>(w->uid);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_LWMUTEXID;
	bool woke = false;
	std::vector<SceUID> copy = m->waiters.threads;
	for (SceUID u : copy)
		if (PspThread *t = Get<PspThread>(u)) woke |= LwUnlockFor(m, w, t, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	m->waiters.threads.clear();
	w->lockLevel = 0;
	w->lockThread = -1;
	w->uid = -1;
	Destroy(m->uid);
	if (woke) sched::Reschedule();
	return 0;
}

// Returns true if locked; sets *error on failure.
bool LwTryLock(LwWorkarea *w, s32 count, u32 *error) {
	if (!*error) {
		if (count <= 0) *error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		else if (count > 1 && !(w->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE)) *error = SCE_KERNEL_ERROR_ILLEGAL_COUNT;
		// Two positive ints always overflow to negative.
		else if ((s32)((u32)count + (u32)w->lockLevel) < 0) *error = SCE_KERNEL_ERROR_LWMUTEX_LOCK_OVERFLOW;
		else if (w->uid == -1) *error = SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
	}
	if (*error) return false;
	SceUID cur = sched::CurrentId();
	if (w->lockLevel == 0) {
		// Validate that it actually exists so an error can be returned if not.
		if (w->lockThread != 0 && !Get<LwMutex>(w->uid)) {
			*error = SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
			return false;
		}
		w->lockLevel = count;
		w->lockThread = cur;
		return true;
	}
	if (w->lockThread == cur) {
		if (w->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) {
			w->lockLevel += count;
			return true;
		}
		*error = SCE_KERNEL_ERROR_LWMUTEX_RECURSIVE_NOT_ALLOWED;
		return false;
	}
	return false;
}

int LockLw(u32 workareaPtr, s32 count, u32 timeoutPtr, bool cb) {
	if (!mem::IsValidAddress(workareaPtr)) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	sched::ChargeCycles(48);
	u32 error = 0;
	if (LwTryLock(w, count, &error)) return 0;
	if (error) return (int)error;
	LwMutex *m = Get<LwMutex>(w->uid);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_LWMUTEXID;
	if (sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	PspThread *t = sched::Current();
	t->waitValue[0] = (u32)count;
	m->waiters.Add(t->uid);
	SceUID id = m->uid, me = t->uid;
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (LwMutex *mm = Get<LwMutex>(id)) mm->waiters.Remove(me);
	};
	hooks.resume = [id, me]() {
		PspThread *wt = Get<PspThread>(me);
		LwMutex *mm = Get<LwMutex>(id);
		LwWorkarea *wa = mm ? Workarea(mm->workarea) : nullptr;
		if (!mm || !wa) {
			if (wt->waitTimeoutPtr && mem::IsValidRange(wt->waitTimeoutPtr, 4)) GWrite32(wt->waitTimeoutPtr, 0);
			wt->waitTimeoutPtr = 0;
			sched::Wake(wt, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
			return true;
		}
		// The lock state lives in the workarea.
		if (wa->lockLevel == 0 && LwUnlockFor(mm, wa, wt, 0)) return true;
		mm->waiters.Add(me);
		return false;
	};
	hooks.timeout = [id, me]() {
		if (LwMutex *mm = Get<LwMutex>(id)) mm->waiters.Remove(me);
	};
	return sched::Wait(WAITTYPE_LWMUTEX, id, timeoutPtr, cb, &hooks);
}

int sceKernelLockLwMutex(u32 w, s32 count, u32 timeoutPtr) { return LockLw(w, count, timeoutPtr, false); }
int sceKernelLockLwMutexCB(u32 w, s32 count, u32 timeoutPtr) { return LockLw(w, count, timeoutPtr, true); }

int sceKernelTryLockLwMutex(u32 workareaPtr, s32 count) {
	if (!mem::IsValidAddress(workareaPtr)) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	sched::ChargeCycles(24);
	u32 error = 0;
	if (LwTryLock(w, count, &error)) return 0;
	// Unlike sceKernelTryLockLwMutex_600, this always returns the same error.
	return (int)SCE_KERNEL_ERROR_MUTEX_LOCKED;
}

int sceKernelTryLockLwMutex_600(u32 workareaPtr, s32 count) {
	if (!mem::IsValidAddress(workareaPtr)) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	sched::ChargeCycles(24);
	u32 error = 0;
	if (LwTryLock(w, count, &error)) return 0;
	return error ? (int)error : (int)SCE_KERNEL_ERROR_LWMUTEX_LOCKED;
}

int sceKernelUnlockLwMutex(u32 workareaPtr, s32 count) {
	if (!mem::IsValidAddress(workareaPtr)) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	sched::ChargeCycles(28);
	if (w->uid == -1) return (int)SCE_KERNEL_ERROR_LWMUTEX_NOT_FOUND;
	if (count <= 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if ((w->attr & PSP_MUTEX_ATTR_ALLOW_RECURSIVE) == 0 && count > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_COUNT;
	if (w->lockLevel == 0 || w->lockThread != sched::CurrentId()) return (int)SCE_KERNEL_ERROR_LWMUTEX_UNLOCKED;
	if (w->lockLevel < count) return (int)SCE_KERNEL_ERROR_LWMUTEX_UNLOCK_UNDERFLOW;
	w->lockLevel -= count;
	if (w->lockLevel != 0) return 0;
	LwMutex *m = Get<LwMutex>(w->uid);
	if (!m) {
		w->lockThread = 0;
		return 0;
	}
	m->waiters.Cleanup(WAITTYPE_LWMUTEX, m->uid);
	bool woke = false;
	while (!woke && !m->waiters.threads.empty()) {
		size_t idx = 0;
		if (m->attr & PSP_MUTEX_ATTR_PRIORITY) {
			int best = m->waiters.BestPriorityIndex();
			idx = best < 0 ? 0 : (size_t)best;
		}
		PspThread *t = Get<PspThread>(m->waiters.threads[idx]);
		woke |= t && LwUnlockFor(m, w, t, 0);
		m->waiters.threads.erase(m->waiters.threads.begin() + idx);
	}
	if (!woke) w->lockThread = 0;
	else sched::Reschedule();
	return 0;
}

int ReferLw(SceUID id, u32 infoPtr) {
	LwMutex *m = Get<LwMutex>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_LWMUTEXID;
	struct {
		u32 size;
		char name[32];
		u32 attr;
		SceUID uid;
		u32 workarea;
		s32 initCount, currentCount;
		SceUID lockThread;
		s32 numWaitThreads;
	} info{};
	if (!mem::IsValidRange(infoPtr, sizeof(info))) return -1;
	if (GRead32(infoPtr) == 0) return 0;
	LwWorkarea *w = Workarea(m->workarea);
	info.size = sizeof(info);
	CopyName(info.name, m->name);
	info.attr = m->attr;
	info.uid = m->uid;
	info.workarea = m->workarea;
	info.initCount = m->initCount;
	info.currentCount = w ? w->lockLevel : 0;
	info.lockThread = w && w->lockThread != 0 ? w->lockThread : -1;
	info.numWaitThreads = m->waiters.Count(WAITTYPE_LWMUTEX, m->uid);
	mem::WriteBytes(infoPtr, &info, sizeof(info));
	return 0;
}

int sceKernelReferLwMutexStatus(u32 workareaPtr, u32 infoPtr) {
	LwWorkarea *w = Workarea(workareaPtr);
	if (!w) return (int)SCE_KERNEL_ERROR_ACCESS_ERROR;
	return ReferLw(w->uid, infoPtr);
}

int sceKernelReferLwMutexStatusByID(SceUID id, u32 infoPtr) { return ReferLw(id, infoPtr); }

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
	constexpr u32 NID = KF_NOT_IN_INTERRUPT | KF_NOT_DISPATCH_SUSPENDED;
	static const hle::HleFunction funcs[] = {
		HLEFN(0x092968F4, sceKernelCpuSuspendIntr),
		HLEFN(0x5F10D406, sceKernelCpuResumeIntr),
		HLEFN(0x3B84732D, sceKernelCpuResumeIntr),
		HLEFN(0x47A0B729, sceKernelIsCpuIntrSuspended),
		HLEFN(0xB55249D2, sceKernelIsCpuIntrEnable),
		HLEFN(0xA089ECA4, sceKernelMemset),
		HLEFN(0xDC692EE3, sceKernelTryLockLwMutex),
		HLEFN(0x37431849, sceKernelTryLockLwMutex_600),
		KFN(0xBEA46419, sceKernelLockLwMutex, NID),
		KFN(0x1FC64E09, sceKernelLockLwMutexCB, NID),
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
