// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/runtime.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <thread>

#include "common/fileutil.h"
#include "common/log.h"
#include "runtime/boot.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/interp.h"
#include "runtime/core/memory.h"
#include "runtime/hle/display.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/io/io.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/media/disc.h"

namespace ctrl {
void OnVblank();
}
namespace umd {
void NotifyDiscChanged();
}

namespace runtime {

namespace {

struct BootTarget {
	std::vector<u8> image;
	std::string pspPath;
	std::string cwd;
};

u32 Rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

bool PrepareBoot(const Options &o, BootTarget &bt) {
	fs::path file = fs::u8path(o.file);
	std::string ext = ToLower(file.extension().string());
	io::Mount("host0", io::MakeHostDevice(file.parent_path().string(), false));
	if (ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax" || ext == ".jso") {
		if (!media::MountDisc(file.string())) {
			LOGE("BOOT", "Cannot open disc image %s", o.file.c_str());
			return false;
		}
		bt.pspPath = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
		bt.cwd = "disc0:/PSP_GAME/USRDIR";
		if (!io::ReadFile(bt.pspPath, bt.image) || bt.image.empty() || bt.image[0] == 0) {
			// Some discs ship a plain BOOT.BIN alongside the encrypted EBOOT.BIN.
			std::vector<u8> boot;
			if (io::ReadFile("disc0:/PSP_GAME/SYSDIR/BOOT.BIN", boot) && !boot.empty() && boot[0] == 0x7F) {
				bt.image = std::move(boot);
			}
		}
		return !bt.image.empty();
	}
	std::vector<u8> data;
	if (!ReadWholeFile(file, data)) {
		LOGE("BOOT", "Cannot read %s", o.file.c_str());
		return false;
	}
	if (data.size() >= 40 && memcmp(data.data(), "\0PBP", 4) == 0) {
		u32 dataPsp = Rd32(&data[8 + 6 * 4]), dataPsar = Rd32(&data[8 + 7 * 4]);
		u32 end = dataPsar > dataPsp ? dataPsar : (u32)data.size();
		if (dataPsp >= data.size() || end > data.size()) {
			LOGE("BOOT", "Malformed PBP");
			return false;
		}
		bt.image.assign(data.begin() + dataPsp, data.begin() + end);
		bt.pspPath = "ms0:/PSP/GAME/" + PathToUtf8(file.parent_path().filename()) + "/EBOOT.PBP";
		bt.cwd = "host0:/";
		return true;
	}
	bt.image = std::move(data);
	bt.pspPath = "host0:/" + PathToUtf8(file.filename());
	bt.cwd = "host0:/";
	return true;
}

int RunOnce(const Options &o, BootTarget bt) {
	if (!mem::Init((u32)o.ramMB << 20)) Fatal("Cannot reserve guest memory");
	kernel::InitMemoryPartitions(mem::g_ramSize, o.largeUserMemory);
	kernel::g_firmwareVersion = o.firmwareVersion;
	cpu::Init();
	cpu::SetStrictMemory(o.strictMemory);
	dispatch::Init();
	hle::Init();
	if (!o.dataDir.empty()) hle::LoadNidDatabase(fs::u8path(o.dataDir) / "nids.csv");
	hle::RegisterAllModules();
	sched::Init(o.fastTime);
	io::Init();
	io::Mount("ms0", io::MakeHostDevice(o.memstickDir, false));
	io::Mount("flash0", io::MakeHostDevice(o.flash0Dir, true));
	io::Mount("emulator", io::MakeEmulatorDevice(o.hasDisplayForTests));
	io::Mount("kemulator", io::MakeEmulatorDevice(o.hasDisplayForTests));
	kernel::SetAotCacheDir(o.aotDir);
	kernel::SetAotEnabled(o.aot);
	kernel::SetDecryptTool(o.decryptTool);
	kernel::SetDecryptCacheDir(o.decryptCacheDir);

	// Re-mount host0/disc0 for this run (PrepareBoot mounted them before io::Init cleared them).
	fs::path file = fs::u8path(o.file);
	io::Mount("host0", io::MakeHostDevice(file.parent_path().string(), false));
	media::RemountDiscIfAny();
	io::SetCwd(bt.cwd);

	display::Init();
	display::SetVblankHook([]() { ctrl::OnVblank(); });

	SceUID mod = boot::BootModule(bt.image, bt.pspPath);
	if (mod < 0) {
		LOGE("BOOT", "Failed to load %s: %08x", bt.pspPath.c_str(), mod);
		return 2;
	}
	int rc = sched::Run();
	LOGI("RUNTIME", "Exited with code %d after %llu interpreted instructions, %u bad memory accesses", rc,
	     (unsigned long long)interp::InstructionCount(), mem::BadAccessCount());
	if (logging::g_level <= LogLevel::Info) hle::DumpUnimplementedStats();
	return rc;
}

}  // namespace

int Run(const Options &o) {
	host::Config hc;
	hc.headless = o.headless;
	hc.windowScale = o.windowScale;
	hc.title = "PSPC - " + PathToUtf8(fs::u8path(o.file).filename());

	std::atomic<bool> finished{false};
	std::thread watchdog;
	if (o.timeoutSec > 0) {
		watchdog = std::thread([&]() {
			for (int i = 0; i < o.timeoutSec * 10 && !finished; i++) Sleep(100);
			if (!finished) {
				LOGW("RUNTIME", "Timeout after %d s", o.timeoutSec);
				sched::PostFromHost([]() { sched::RequestStop(124); });
			}
		});
	}

	int rc = host::Run(
		hc,
		[&]() -> int {
			BootTarget bt;
			io::Init();
			if (!PrepareBoot(o, bt)) return 2;
			int code = RunOnce(o, bt);
			std::string next;
			std::vector<u8> args;
			if (code == boot::kExitLoadExec && boot::TakeLoadExecRequest(next, args)) {
				// A full reset (fresh memory, kernel objects) is required for LoadExec; it needs a
				// process-level restart today. Report and stop.
				LOGW("RUNTIME", "sceKernelLoadExec(%s) requested; chaining executables is not supported yet", next.c_str());
				code = 0;
			}
			return code;
		},
		[]() { sched::PostFromHost([]() { sched::RequestStop(0); }); });
	finished = true;
	if (watchdog.joinable()) watchdog.join();
	logging::Shutdown();
	return rc;
}

}  // namespace runtime
