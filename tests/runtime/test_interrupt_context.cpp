// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv) {
	if (argc != 3) return 2;
	const std::string mode = argv[1];
	const bool preempted = mode == "preempted";
	const bool busy = mode == "busy";
	const bool woken = mode == "woken";
	if (!preempted && !busy && !woken && mode != "blocked") return 2;
	constexpr u32 ramSize = 32 * 1024 * 1024;
	if (!mem::Init(ramSize)) return 1;
	kernel::InitMemoryPartitions(ramSize, false);
	cpu::Init();
	dispatch::Init();
	hle::Init();
	Register_ThreadManForUser();
	sched::Init(std::string(argv[2]) == "fast");
	const u32 wakeStub = hle::MakeGuestStub("ThreadManForUser", 0xD59EAD2F);
	if (!wakeStub) return 1;
	auto *thread = new kernel::PspThread;
	thread->name = "interrupt-context";
	thread->currentPriority = thread->initPriority = 32;
	kernel::Register(thread);
	s32 wakeResult = 0;
	bool handlerRan = false;
	auto interrupt = [&] {
		wakeResult = (s32)sched::RunInterruptHandler(wakeStub, 0, {(u32)thread->uid});
		handlerRan = true;
		sched::RequestStop(0);
	};
	thread->onFirstRun = [&] {
		if (preempted) {
			sched::QueueInterrupt(interrupt);
			sched::Reschedule();
		} else if (busy) {
			sched::ScheduleEvent(sched::NowUs() + 1, [&] { sched::QueueInterrupt(interrupt); });
			sched::BusyDelayCycles(10000);
		} else {
			sched::ScheduleEvent(sched::NowUs() + 1000, [&] {
				// Becoming ready during idle timer processing is not being interrupted.
				if (woken) sched::Wake(thread, 0);
				sched::QueueInterrupt(interrupt);
			});
			sched::Wait(kernel::WAITTYPE_SLEEP, 0, 0, false);
		}
		sched::RequestStop(2);
	};
	sched::LaunchThread(thread);
	const int exit = sched::Run();
	// Self-wakeup is rejected; an idle CPU has no interrupted guest thread.
	const bool valid = handlerRan && exit == 0 &&
		(preempted || busy ? wakeResult == (s32)SCE_KERNEL_ERROR_ILLEGAL_THID && thread->wakeupCount == 0
		           : wakeResult == 0 && thread->wakeupCount == (woken ? 1 : 0));
	std::printf("%s: wake=%08x pending=%d interrupt=%d exit=%d\n", mode.c_str(),
	            (u32)wakeResult, thread->wakeupCount, handlerRan, exit);
	hostsys::FreeFiber(thread->fiber);
	thread->fiber = nullptr;
	sched::DeleteThread(thread);
	sched::Shutdown();
	kernel::DestroyAll();
	mem::Shutdown();
	return valid ? 0 : 1;
}
