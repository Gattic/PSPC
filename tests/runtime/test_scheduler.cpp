// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

#include <cstdio>
#include <functional>

int main() {
	constexpr u32 ramSize = 32 * 1024 * 1024;
	if (!mem::Init(ramSize)) return 1;
	kernel::InitMemoryPartitions(ramSize, false);
	cpu::Init();
	sched::Init(false);
	int ticks = 0;
	bool interruptRan = false;
	std::function<void()> periodic;
	periodic = [&] {
		++ticks;
		if (ticks == 1) sched::QueueInterrupt([&] {
			interruptRan = true;
			sched::RequestStop(0);
		});
		// Bound the failing case: the queued interrupt must run before this limit.
		if (ticks == 100) { sched::RequestStop(1); return; }
		sched::ScheduleEvent(sched::NowUs() + 1, periodic);
		// Reproduce a host callback costing more time than its recurrence interval.
		hostsys::SleepMs(2);
	};
	sched::ScheduleEvent(0, periodic);
	int result = sched::Run();
	sched::Shutdown();
	mem::Shutdown();
	if (result != 0 || !interruptRan || ticks != 1) {
		std::fprintf(stderr, "real-time callbacks starved dispatch: %d ticks, result %d, interrupt %d\n",
		             ticks, result, interruptRan);
		return 1;
	}
	std::puts("real-time event batching permits interrupt dispatch");
	return 0;
}
