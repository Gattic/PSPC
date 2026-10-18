// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "gpu/backend.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/hle/display.h"
#include "runtime/host/host.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

#include <cstdio>
#include <cstring>

namespace {
// Model an expensive high-resolution presentation without depending on a GPU or
// driver. The actual display callbacks, scheduler and SDL host are exercised.
class SlowDisplay final : public gpu::RenderBackend {
public:
	bool fast = false;
	unsigned presents = 0, firstVcount = 0;
	const char *Name() const override { return "pacing-test"; }
	void DrawBatch(const gpu::BatchPrim *, size_t, const gpu::RasterState *, size_t) override {}
	bool PresentDisplay(u32, int, int, int, int, const std::function<void(u32 *, int, int)> &, u64, bool) override {
		if (!presents) firstVcount = display::Vcount();
		++presents;
		if (!fast) hostsys::SleepMs(20);
		return true;
	}
};
}
int main(int argc, char **argv) {
	const bool fast = argc == 2 && !strcmp(argv[1], "fast");
	host::Config cfg;
	cfg.windowScale = 1;
	cfg.vsync = false;
	cfg.gpuPresent = true;
	return host::Run(cfg, [fast] {
		if (host::IsHeadless() || !mem::Init(32 * 1024 * 1024)) return 1;
		kernel::InitMemoryPartitions(32 * 1024 * 1024, false);
		cpu::Init();
		sched::Init(fast);
		SlowDisplay backend;
		backend.fast = fast;
		gpu::SetRenderBackend(&backend);
		// Keep the scheduler alive between display events, like a game waiting
		// on a kernel object. No guest CPU work is needed to reproduce the stall.
		auto *thread = new kernel::PspThread;
		thread->currentPriority = thread->initPriority = 32;
		kernel::Register(thread);
		thread->onFirstRun = [] { sched::Wait(kernel::WAITTYPE_SLEEP, 0, 0, false); };
		sched::LaunchThread(thread);
		display::Init();
		unsigned hooks = 0;
		display::SetVblankHook([&] {
			++hooks;
			if (display::Vcount() >= 8 && backend.presents) sched::RequestStop(0);
		});
		// Both modes begin six VBlanks behind. Fast-time frame capture stays
		// deterministic; real-time presentation must not replay obsolete frames.
		if (fast) sched::ChargeUs(100000);
		else hostsys::SleepMs(100);
		const u64 start = hostsys::MonotonicUs();
		const int result = sched::Run();
		printf("%s: %u vblanks, %u hooks, %u presents, first at vcount %u, %.2f ms\n",
		       fast ? "fast" : "real", display::Vcount(), hooks, backend.presents, backend.firstVcount,
		       (hostsys::MonotonicUs() - start) / 1000.0);
		const bool ok = result == 0 && hooks == display::Vcount() && hooks >= 8 &&
			(fast ? hooks == 8 && backend.presents == 8 && backend.firstVcount == 1
			      : backend.presents > 0 && backend.presents <= 3 && backend.firstVcount >= 6);
		display::SetVblankHook({});
		gpu::SetRenderBackend(nullptr);
		hostsys::FreeFiber(thread->fiber);
		thread->fiber = nullptr;
		sched::DeleteThread(thread);
		sched::Shutdown();
		kernel::DestroyAll();
		mem::Shutdown();
		if (!ok) std::fprintf(stderr, "display catch-up replayed obsolete presentations or lost VBlanks\n");
		return ok ? 0 : 1;
	}, [] { sched::RequestStop(1); });
}
