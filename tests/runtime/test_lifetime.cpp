// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/kernel/kobject.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/runtime.h"

#include <cstdio>
#include <filesystem>
#include <string>

int DeferredDelete() {
	constexpr u32 ramSize = 32 * 1024 * 1024;
	if (!mem::Init(ramSize)) return 1;
	kernel::InitMemoryPartitions(ramSize, false);
	cpu::Init();
	sched::Init(true);
	auto *first = new kernel::PspThread;
	first->name = "exit-delete";
	first->currentPriority = first->initPriority = 16;
	const SceUID firstId = kernel::Register(first);
	first->onFirstRun = [] { sched::ExitCurrentThread(0, true); };
	auto *next = new kernel::PspThread;
	next->name = "after-delete";
	next->currentPriority = next->initPriority = 32;
	kernel::Register(next);
	bool dispatched = false;
	next->onFirstRun = [&] {
		dispatched = kernel::GetAny(firstId) == nullptr;
		sched::RequestStop(0);
	};
	sched::LaunchThread(first);
	sched::LaunchThread(next);
	const int result = sched::Run();
	hostsys::FreeFiber(next->fiber);
	next->fiber = nullptr;
	sched::DeleteThread(next);
	sched::Shutdown();
	kernel::DestroyAll();
	mem::Shutdown();
	std::printf("dispatch after exit-delete: result=%d dispatched=%d\n", result, dispatched);
	return result == 0 && dispatched ? 0 : 1;
}

int RuntimeShutdown(const char *elfPath) {
	namespace fs = std::filesystem;
	const fs::path work = fs::path(elfPath).parent_path();
	runtime::Options options;
	options.file = elfPath;
	options.headless = true;
	options.fastTime = true;
	options.aot = false;
	options.plugins = false;
	options.network = false;
	options.renderer = "software";
	options.memstickDir = (work / "ms0").string();
	options.flash0Dir = (work / "flash0").string();
	options.flash1Dir = (work / "flash1").string();
	options.host0Dir = work.string();
	SceUID poolId = 0;
	// A host stop request runs after boot initialization, leaving a live guest pool.
	sched::PostFromHost([&] {
		const auto *create = hle::FindFunction("ThreadManForUser", 0x56C039B5);
		const u32 name = kernel::AllocKernelMemory(64, "pool-test-name");
		if (!create || !name) { sched::RequestStop(2); return; }
		constexpr char label[] = "shutdown-pool";
		mem::WriteBytes(name, label, sizeof(label));
		Ctx context{};
		context.r[4] = name;
		context.r[5] = 2;  // user partition
		context.r[7] = 0x1000;
		create->fn(&context);
		poolId = (SceUID)context.r[2];
		sched::RequestStop(poolId > 0 ? 0 : 2);
	});
	const int result = runtime::Run(options);
	size_t remaining = 0;
	kernel::ForEachObject([&](kernel::KObject *) { ++remaining; });
	std::printf("runtime shutdown: result=%d pool=%08x remaining=%zu\n", result, (u32)poolId, remaining);
	// Do not clean up here: the runtime owns this boundary, including destruction at exit.
	return result == 0 && poolId > 0 && remaining == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc < 2) return 2;
	const std::string mode = argv[1];
	if (mode == "deferred" && argc == 2) return DeferredDelete();
	if (mode == "shutdown" && argc == 3) return RuntimeShutdown(argv[2]);
	return 2;
}
