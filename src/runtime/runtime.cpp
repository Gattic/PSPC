// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

#include "common/fileutil.h"
#include "common/log.h"
#include "runtime/boot.h"
#include "runtime/core/backtrace.h"
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
#include "runtime/m33/m33.h"
#include "runtime/media/disc.h"

namespace ctrl {
void OnVblank();
void AddScriptedPress(u32 vblank, u32 buttons, u32 frames);
}
namespace umd {
void NotifyDiscChanged();
}

namespace runtime {

namespace {

// Interpreter fallbacks inside AOT-compiled modules, written to <aot dir>/<sha1>.hints so the
// next pspc_build.py run compiles those entry points natively.
std::map<std::string, std::set<u32>> g_aotMisses;
std::string g_aotDirForHints;

void RecordAotMiss(u32 target) {
	kernel::PspModule *m = kernel::ModuleByAddress(target);
	if (!m || !m->aot) return;
	u32 delta = m->image.relocatable ? m->image.lowAddr - m->aotRecompileBase : 0;
	g_aotMisses[HexString(m->sha1.data(), m->sha1.size())].insert(target - delta);
}

void WriteAotHints() {
	for (auto &kv : g_aotMisses) {
		fs::path p = fs::u8path(g_aotDirForHints) / (kv.first + ".hints");
		std::set<u32> all = kv.second;
		std::vector<u8> old;
		if (ReadWholeFile(p, old)) {
			std::string text(old.begin(), old.end());
			size_t pos = 0;
			while (pos < text.size()) {
				size_t end = text.find('\n', pos);
				if (end == std::string::npos) end = text.size();
				std::string line = text.substr(pos, end - pos);
				if (!line.empty() && line[0] != '#') all.insert((u32)strtoul(line.c_str(), nullptr, 16));
				pos = end + 1;
			}
		}
		std::string out = "# entry points the interpreter reached inside this AOT module\n";
		char buf[16];
		for (u32 a : all) {
			snprintf(buf, sizeof(buf), "%08x\n", a);
			out += buf;
		}
		WriteTextFile(p, out);
		LOGI("AOT", "%zu interpreter entries recorded in %s", kv.second.size(), p.string().c_str());
	}
	g_aotMisses.clear();
}

struct BootTarget {
	std::vector<u8> image;
	std::string pspPath;
	std::string cwd;
};

u32 Rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

fs::path Host0Root(const Options &o) {
	return o.host0Dir.empty() ? fs::u8path(o.file).parent_path() : fs::u8path(o.host0Dir);
}

// host0: path of a host file, relative to the host0 root when the file lies under it.
std::string Host0PathOf(const Options &o, const fs::path &file) {
	std::error_code ec;
	fs::path rel = fs::relative(fs::absolute(file, ec), fs::absolute(Host0Root(o), ec), ec);
	std::string r = ec || rel.empty() ? PathToUtf8(file.filename()) : PathToUtf8(rel);
	std::replace(r.begin(), r.end(), '\\', '/');
	if (r.rfind("..", 0) == 0) r = PathToUtf8(file.filename());
	return "host0:/" + r;
}

bool PrepareBoot(const Options &o, BootTarget &bt) {
	fs::path file = fs::u8path(o.file);
	std::string ext = ToLower(file.extension().string());
	io::Mount("host0", io::MakeHostDevice(Host0Root(o).string(), false));
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
	bt.pspPath = Host0PathOf(o, file);
	bt.cwd = bt.pspPath.substr(0, bt.pspPath.rfind('/') + 1);
	if (!o.umdImage.empty() && !media::MountDisc(o.umdImage)) {
		LOGE("BOOT", "Cannot open disc image %s", o.umdImage.c_str());
		return false;
	}
	return true;
}

// --dump-frames: the displayed framebuffer as a 24-bit BMP every N VBlanks (works headless).
void DumpFrame(const std::string &dir) {
	u32 addr;
	int stride, format;
	display::CurrentFramebuffer(addr, stride, format);
	if (!addr) return;
	std::vector<u32> px;
	display::ConvertFramebuffer(addr, stride, format, 480, 272, px);
	const int w = 480, h = 272, rowBytes = w * 3;
	std::vector<u8> bmp(54 + (size_t)rowBytes * h);
	auto put32 = [&](size_t at, u32 v) { memcpy(&bmp[at], &v, 4); };
	bmp[0] = 'B';
	bmp[1] = 'M';
	put32(2, (u32)bmp.size());
	put32(10, 54);
	put32(14, 40);
	put32(18, (u32)w);
	put32(22, (u32)h);
	bmp[26] = 1;
	bmp[28] = 24;
	put32(34, (u32)(rowBytes * h));
	for (int y = 0; y < h; y++) {
		u8 *row = &bmp[54 + (size_t)(h - 1 - y) * rowBytes];  // bottom-up
		for (int x = 0; x < w; x++) {
			u32 p = px[(size_t)y * w + x];
			row[x * 3 + 0] = (u8)(p >> 16);
			row[x * 3 + 1] = (u8)(p >> 8);
			row[x * 3 + 2] = (u8)p;
		}
	}
	char name[32];
	snprintf(name, sizeof(name), "frame_%06u.bmp", display::Vcount());
	WriteWholeFile(fs::u8path(dir) / name, bmp.data(), bmp.size());
}

int RunOnce(const Options &o, BootTarget bt) {
	backtrace::InstallFatalHook();
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
	io::Mount("flash1", io::MakeHostDevice(o.flash1Dir, false));
	io::Mount("emulator", io::MakeEmulatorDevice(o.hasDisplayForTests));
	io::Mount("kemulator", io::MakeEmulatorDevice(o.hasDisplayForTests));
	kernel::SetAotCacheDir(o.aotDir);
	kernel::SetAotEnabled(o.aot);
	g_aotDirForHints = o.aotDir;
	dispatch::SetMissHook(&RecordAotMiss);
	kernel::SetDecryptTool(o.decryptTool);
	kernel::SetDecryptCacheDir(o.decryptCacheDir);

	// Re-mount host0/disc0 for this run (PrepareBoot mounted them before io::Init cleared them).
	io::Mount("host0", io::MakeHostDevice(Host0Root(o).string(), false));
	media::RemountDiscIfAny();
	io::SetCwd(bt.cwd);

	display::Init();
	for (const Options::Press &p : o.presses) ctrl::AddScriptedPress(p.vblank, p.buttons, p.frames);
	if (!o.dumpFramesDir.empty()) {
		std::error_code ec;
		fs::create_directories(fs::u8path(o.dumpFramesDir), ec);
		std::string dir = o.dumpFramesDir;
		u32 every = (u32)std::max(1, o.dumpEvery);
		display::SetVblankHook([dir, every]() {
			ctrl::OnVblank();
			if (display::Vcount() % every == 0) DumpFrame(dir);
		});
	} else {
		display::SetVblankHook([]() { ctrl::OnVblank(); });
	}

	// The M33 layer: config.se, kernel NID resolver, plugins (docs/m33-behaviors.md).
	m33::BootInfo bi;
	bi.fromDisc = bt.pspPath.rfind("disc0:", 0) == 0;
	bi.apitype = bi.fromDisc ? m33::APITYPE_GAME_UMD : m33::APITYPE_GAME_MS;
	bi.initFileName = bt.pspPath;
	m33::Init(o.flash1Dir, bi);
	if (!o.dataDir.empty()) m33::RegisterNidResolver((fs::u8path(o.dataDir) / "m33_nid_resolver.csv").string());
	if (o.plugins) m33::LoadPlugins();

	SceUID mod = boot::BootModule(bt.image, bt.pspPath);
	if (mod < 0) {
		LOGE("BOOT", "Failed to load %s: %08x", bt.pspPath.c_str(), mod);
		return 2;
	}
	int rc = sched::Run();
	LOGI("RUNTIME", "Exited with code %d after %llu interpreted instructions, %u bad memory accesses", rc,
	     (unsigned long long)interp::InstructionCount(), mem::BadAccessCount());
	if (logging::g_level <= LogLevel::Info) hle::DumpUnimplementedStats();
	WriteAotHints();
	return rc;
}

std::string g_relaunchTarget;

}  // namespace

bool TakeRelaunch(std::string &hostFile) {
	if (g_relaunchTarget.empty()) return false;
	hostFile = g_relaunchTarget;
	g_relaunchTarget.clear();
	return true;
}

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
				// The emulation thread may be stuck somewhere that never looks at host posts.
				for (int i = 0; i < 50 && !finished; i++) Sleep(100);
				if (!finished) {
					fprintf(stderr, "[FATAL] Emulation did not stop within 5 s of the timeout; terminating\n");
					fflush(stdout);
					fflush(stderr);
					logging::Shutdown();
					TerminateProcess(GetCurrentProcess(), 124);
				}
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
