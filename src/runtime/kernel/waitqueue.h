// SPDX-License-Identifier: GPL-2.0-or-later
// Threads waiting on a kernel object. Entries are validated lazily: a thread that timed out or
// was released simply no longer matches (waitType, waitId) and is dropped.
#pragma once

#include <algorithm>
#include <vector>

#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

namespace kernel {

struct WaitQueue {
	std::vector<SceUID> threads;

	static bool IsWaitingOn(PspThread *t, WaitType type, SceUID obj) {
		return t && t->waitType == type && t->waitId == obj && !t->waitDone;
	}

	void Add(SceUID uid) {
		if (std::find(threads.begin(), threads.end(), uid) == threads.end()) threads.push_back(uid);
	}

	void Cleanup(WaitType type, SceUID obj) {
		threads.erase(std::remove_if(threads.begin(), threads.end(),
		                             [&](SceUID u) { return !IsWaitingOn(Get<PspThread>(u), type, obj); }),
		              threads.end());
	}

	int Count(WaitType type, SceUID obj) {
		Cleanup(type, obj);
		return (int)threads.size();
	}

	void SortByPriority() {
		std::stable_sort(threads.begin(), threads.end(), [](SceUID a, SceUID b) {
			PspThread *ta = Get<PspThread>(a), *tb = Get<PspThread>(b);
			int pa = ta ? ta->currentPriority : 0x7FFF, pb = tb ? tb->currentPriority : 0x7FFF;
			return pa < pb;
		});
	}

	// Wake every valid waiter with `result`. Returns true if any thread was woken.
	bool WakeAll(WaitType type, SceUID obj, s32 result) {
		Cleanup(type, obj);
		bool any = !threads.empty();
		std::vector<SceUID> copy;
		copy.swap(threads);
		for (SceUID u : copy) {
			if (PspThread *t = Get<PspThread>(u)) sched::Wake(t, result);
		}
		return any;
	}
};

}  // namespace kernel
