// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

#include <cstdio>
#include <functional>
#include <string>

int ExitSwitchCost() {
	constexpr u32 ramSize = 32 * 1024 * 1024;
	if (!mem::Init(ramSize)) return 1;
	kernel::InitMemoryPartitions(ramSize, false);
	cpu::Init();
	sched::Init(true);
	u64 exitedAt = 0, resumedAt = 0;
	auto *first = new kernel::PspThread;
	auto *second = new kernel::PspThread;
	for (auto *thread : {first, second}) {
		thread->currentPriority = thread->initPriority = 32;
		kernel::Register(thread);
	}
	const SceUID firstId = first->uid;
	first->onFirstRun = [&] {
		exitedAt = sched::NowCycles();
		sched::ExitCurrentThread(0, true);
	};
	second->onFirstRun = [&] {
		resumedAt = sched::NowCycles();
		sched::RequestStop(0);
	};
	sched::LaunchThread(first);
	sched::LaunchThread(second);
	const int result = sched::Run();
	const bool ok = result == 0 && !kernel::Get<kernel::PspThread>(firstId) && resumedAt - exitedAt == 4450;
	std::printf("exit and dispatch: %llu cycles (expected 3300 + 1150)\n", resumedAt - exitedAt);
	hostsys::FreeFiber(second->fiber);
	second->fiber = nullptr;
	sched::DeleteThread(second);
	sched::Shutdown();
	kernel::DestroyAll();
	mem::Shutdown();
	return ok ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc == 2 && std::string(argv[1]) == "exit_switch") return ExitSwitchCost();
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
