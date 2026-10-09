// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "runtime/kernel/thread.h"

namespace kernel {

struct Callback : KObject {
	static constexpr KType kType = KType::Callback;
	KType Type() const override { return kType; }
	u32 entry = 0;
	u32 commonArg = 0;
	SceUID threadId = 0;
	s32 notifyCount = 0;
	s32 notifyArg = 0;
	u32 gp = 0;
};

SceUID CreateCallback(const char *name, u32 entry, u32 commonArg);
// Notify a callback (from guest code or from system events like UMD/power). A thread in a
// callback-enabled wait is queued to run it when it would get the CPU; the caller decides
// whether to reschedule.
int NotifyCallback(SceUID cbId, s32 arg);
bool HasPendingCallbacks(PspThread *t);
// A thread can run its callbacks unless it is already two callbacks deep (on hardware a CB wait
// that would go deeper never returns).
bool CanRunCallbacks(PspThread *t);
// Run all pending callbacks of `t` on the current fiber (t must be the current thread), including
// ones notified while they run. Returns true if any ran.
bool RunPendingCallbacks(PspThread *t);
void DeleteThreadCallbacks(PspThread *t);

}  // namespace kernel
