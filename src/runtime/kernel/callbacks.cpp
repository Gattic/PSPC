// SPDX-License-Identifier: GPL-2.0-or-later
// Callback delivery rules follow PPSSPP's sceKernelThread.cpp (Portions derived from PPSSPP,
// https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#include "runtime/kernel/callbacks.h"

#include <algorithm>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace kernel {

namespace {
// Setting up the call costs about 8us on top of the switch: 14us from a notify to a better
// thread's callback running on hardware (pspautotests threads/callbacks/combos).
constexpr s64 kCallbackCycles = 1800;
}  // namespace

SceUID CreateCallback(const char *name, u32 entry, u32 commonArg) {
	PspThread *t = sched::Current();
	auto *cb = new Callback();
	cb->name = name ? name : "";
	if (cb->name.size() > 31) cb->name.resize(31);
	cb->entry = entry;
	cb->commonArg = commonArg;
	cb->threadId = t ? t->uid : 0;
	cb->gp = t ? t->ctx.r[28] : 0;
	SceUID id = Register(cb);
	if (t) t->callbacks.push_back(id);
	return id;
}

int NotifyCallback(SceUID cbId, s32 arg) {
	Callback *cb = Get<Callback>(cbId);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	cb->notifyCount++;
	cb->notifyArg = arg;
	if (PspThread *t = Get<PspThread>(cb->threadId)) sched::NotifyCallbackPending(t);
	return 0;
}

bool HasPendingCallbacks(PspThread *t) {
	for (SceUID id : t->callbacks) {
		Callback *cb = Get<Callback>(id);
		if (cb && cb->notifyCount != 0) return true;
	}
	return false;
}

bool CanRunCallbacks(PspThread *t) { return t->callbackDepth < 2; }

bool RunPendingCallbacks(PspThread *t) {
	if (!CanRunCallbacks(t)) return false;
	bool ran = false;
	for (;;) {
		Callback *cb = nullptr;
		for (SceUID id : t->callbacks) {
			Callback *c = Get<Callback>(id);
			if (c && c->notifyCount != 0) {
				cb = c;
				break;
			}
		}
		if (!cb) break;
		SceUID id = cb->uid;
		s32 count = cb->notifyCount, arg = cb->notifyArg;
		cb->notifyCount = 0;
		cb->notifyArg = 0;
		u32 entry = cb->entry, common = cb->commonArg;
		sched::ChargeCycles(kCallbackCycles);
		t->callbackDepth++;
		u32 result = cpu::CallGuest(&t->ctx, entry, {(u32)count, (u32)arg, common});
		t->callbackDepth--;
		ran = true;
		// Callbacks that don't return 0 are deleted.
		if (result != 0 && Get<Callback>(id)) {
			t->callbacks.erase(std::remove(t->callbacks.begin(), t->callbacks.end(), id), t->callbacks.end());
			Destroy(id);
		}
	}
	return ran;
}

void DeleteThreadCallbacks(PspThread *t) {
	for (SceUID id : t->callbacks) Destroy(id);
	t->callbacks.clear();
}

}  // namespace kernel
