// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/kernel/callbacks.h"

#include <algorithm>

#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace kernel {

SceUID CreateCallback(const char *name, u32 entry, u32 commonArg) {
	PspThread *t = sched::Current();
	auto *cb = new Callback();
	cb->name = name ? name : "";
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
	if (t->inCallback) return false;
	for (SceUID id : t->callbacks) {
		Callback *cb = Get<Callback>(id);
		if (cb && cb->notifyCount > 0) return true;
	}
	return false;
}

bool RunPendingCallbacks(PspThread *t) {
	if (t->inCallback) return false;
	bool ran = false;
	t->inCallback = true;
	for (bool again = true; again;) {
		again = false;
		std::vector<SceUID> ids = t->callbacks;
		for (SceUID id : ids) {
			Callback *cb = Get<Callback>(id);
			if (!cb || cb->notifyCount <= 0) continue;
			s32 count = cb->notifyCount, arg = cb->notifyArg;
			cb->notifyCount = 0;
			cb->notifyArg = 0;
			u32 entry = cb->entry, common = cb->commonArg;
			u32 savedGp = t->ctx.r[28];
			u32 result = cpu::CallGuest(&t->ctx, entry, {(u32)count, (u32)arg, common});
			t->ctx.r[28] = savedGp;
			ran = true;
			again = true;
			if (result != 0) {
				t->callbacks.erase(std::remove(t->callbacks.begin(), t->callbacks.end(), id), t->callbacks.end());
				Destroy(id);
			}
		}
	}
	t->inCallback = false;
	return ran;
}

void DeleteThreadCallbacks(PspThread *t) {
	for (SceUID id : t->callbacks) Destroy(id);
	t->callbacks.clear();
}

}  // namespace kernel
