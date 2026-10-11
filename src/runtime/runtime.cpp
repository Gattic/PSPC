// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

#include "common/fileutil.h"
#include "common/log.h"
#include "runtime/boot.h"
#include "runtime/core/backtrace.h"
#include "runtime/dialogs/home_menu.h"
#include "runtime/dialogs/osk_dialog.h"
#include "runtime/dialogs/ppge.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/interp.h"
#include "runtime/core/memory.h"
#include "runtime/hle/display.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/hle/modules/net_hle.h"
#include "runtime/hle/modules/net_selftest.h"
#include "runtime/host/host.h"
#include "runtime/io/io.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/m33/m33.h"
#include "runtime/m33/vsh_iso.h"
#include "runtime/media/disc.h"
#include "runtime/vsh.h"
#if PSPC_HAVE_GPU
#include "gpu/backend.h"
#include "gpu/ge.h"
#endif
#if PSPC_HAVE_GPU_HW
#include "gpu/hw/d3d11_backend.h"
#endif

namespace ctrl {
void OnVblank();
void AddScriptedPress(u32 vblank, u32 buttons, u32 frames);
}
namespace umd {
void NotifyDiscChanged();
void SetDriveEmpty(bool empty);
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
	if (!o.host0Dir.empty()) return fs::u8path(o.host0Dir);
	if (o.file.empty()) return ExecutableDir();
	return fs::u8path(o.file).parent_path();
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
	io::Mount("host0", io::MakeHostDevice(Host0Root(o).string(), false));
	if (!o.netSelftest.empty()) {
		// A network test scenario runs instead of an executable (net::selftest::Start).
		bt.pspPath = "host0:/net-selftest";
		bt.cwd = "host0:/";
		return true;
	}
	if (o.vsh) {
		// The XMB: no executable of our own. A disc is in the drive only with --umd.
		bt.pspPath = "flash0:/vsh/module/vshmain.prx";
		bt.cwd = "flash0:/vsh/module/";
		if (!o.umdImage.empty() && !media::MountDisc(o.umdImage)) {
			LOGE("BOOT", "Cannot open disc image %s", o.umdImage.c_str());
			return false;
		}
		return true;
	}
	fs::path file = fs::u8path(o.file);
	std::string ext = ToLower(file.extension().string());
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

// An RGBA8888 image as a 24-bit BMP.
void WriteBmp(const fs::path &path, const std::vector<u32> &px, int w, int h) {
	const int rowBytes = (w * 3 + 3) & ~3;
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
	WriteWholeFile(path, bmp.data(), bmp.size());
}

// --dump-frames: the displayed framebuffer as a 24-bit BMP every N VBlanks (works headless). With an
// upscaling GPU renderer, also what it shows (hires_<vcount>.bmp).
void DumpFrame(const std::string &dir) {
	u32 addr;
	int stride, format;
	display::CurrentFramebuffer(addr, stride, format);
	if (!addr) return;
	std::vector<u32> px;
	char name[32];
#if PSPC_HAVE_GPU
	// (This also writes GPU rendering back to guest memory for the dump below.)
	int hw = 0, hh = 0;
	if (gpu::GetBackendDisplayImage(addr, stride, format, 480, 272, px, &hw, &hh)) {
		snprintf(name, sizeof(name), "hires_%06u.bmp", display::Vcount());
		ppge::Composite(px.data(), hw, hh, display::ShowingGameScreen());
		WriteBmp(fs::u8path(dir) / name, px, hw, hh);
	}
#endif
	display::ConvertFramebuffer(addr, stride, format, 480, 272, px);
	// What the dialogs and the HOME menu draw over it.
	ppge::Composite(px.data(), 480, 272, display::ShowingGameScreen());
	snprintf(name, sizeof(name), "frame_%06u.bmp", display::Vcount());
	WriteBmp(fs::u8path(dir) / name, px, 480, 272);
}

// PSPC_GPU_STATS: GE host time per frame, every 600 VBlanks.
void ReportGpuStats() {
#if PSPC_HAVE_GPU
	static const bool enabled = getenv("PSPC_GPU_STATS") != nullptr;
	static u64 lastNs = 0, lastPrims = 0;
	static std::chrono::steady_clock::time_point lastWall = std::chrono::steady_clock::now();
	if (!enabled || display::Vcount() % 600 != 0) return;
	const gpu::Stats st = gpu::GetStats();
	const auto now = std::chrono::steady_clock::now();
	const double wall = std::chrono::duration<double>(now - lastWall).count();
	LOGI("GPU", "vblank %u: %.2f ms/frame in the GE, %.0f prims/frame, %.1f frames/s wall", display::Vcount(),
	     (double)(st.hostNs - lastNs) / 1e6 / 600.0, (double)(st.prims - lastPrims) / 600.0, wall > 0 ? 600.0 / wall : 0.0);
	lastNs = st.hostNs;
	lastPrims = st.prims;
	lastWall = now;
#endif
}

// The GE renderer's name (Options::renderer, else $PSPC_RENDERER, else software), lower case.
std::string RendererName(const Options &o) {
	std::string name = o.renderer;
	if (name.empty()) {
		const char *env = getenv("PSPC_RENDERER");
		name = env && *env ? env : "software";
	}
	return ToLower(name);
}

#if PSPC_HAVE_GPU
// The GE renderer (Options::renderer). Lives until the process ends.
std::unique_ptr<gpu::RenderBackend> g_renderBackend;

void SetUpRenderer(const Options &o) {
	const std::string name = RendererName(o);
	int scale = o.renderScale;
	if (scale <= 0) {
		const char *env = getenv("PSPC_RENDER_SCALE");
		scale = env ? atoi(env) : 1;
	}
	scale = std::clamp(scale, 1, 16);
	gpu::SetRenderBackend(nullptr);
	if (name == "software")
		return;
#if PSPC_HAVE_GPU_HW
	if (name == "d3d11" || name == "d3d11-warp") {
		if (!g_renderBackend) {
			gpu::hw::D3D11Options d;
			d.scale = scale;
			d.warp = name == "d3d11-warp";
			d.privateVram = mem::PrivateVramView();
			d.window = host::NativeWindow();
			d.textureFilter = o.textureFilter == "nearest" ? 1 : o.textureFilter == "linear" ? 2 : 0;
			d.textureUpscale = std::clamp(o.textureUpscale, 1, 4);
			if (const char *a = getenv("PSPC_D3D11_ADAPTER")) d.adapter = atoi(a);
			std::string err;
			g_renderBackend = gpu::hw::CreateD3D11Backend(d, &err);
			if (!g_renderBackend) {
				// PSPC_RENDERER_REQUIRE=1 (test runs): no silent fallback.
				if (getenv("PSPC_RENDERER_REQUIRE")) Fatal("D3D11 renderer unavailable: %s", err.c_str());
				LOGW("RUNTIME", "D3D11 renderer unavailable (%s); using the software renderer", err.c_str());
				return;
			}
			// It presents to the window itself, CPU images (software-drawn screens, dialogs) too.
			if (d.window)
				host::SetFramePresenter([](const u32 *px, int w, int h, int stride) {
					return g_renderBackend && g_renderBackend->PresentImage(px, w, h, stride);
				});
		}
		gpu::SetRenderBackend(g_renderBackend.get());
		return;
	}
#endif
	LOGW("RUNTIME", "Unknown renderer \"%s\"; using the software renderer", name.c_str());
}

// The window is about to close: the renderer stops presenting to it.
void ReleaseRendererWindow() {
	host::SetFramePresenter(nullptr);
	if (g_renderBackend) g_renderBackend->ReleaseWindow();
}
#endif

int RunOnce(const Options &o, BootTarget bt) {
	backtrace::InstallFatalHook();
	if (!mem::Init((u32)o.ramMB << 20)) Fatal("Cannot reserve guest memory");
	const bool xmb = o.vsh && o.bootPath.empty() && o.netSelftest.empty();
	kernel::InitMemoryPartitions(mem::g_ramSize, o.largeUserMemory >= 0 ? o.largeUserMemory != 0 : xmb);
	kernel::g_firmwareVersion = o.firmwareVersion;
	cpu::Init();
	cpu::SetStrictMemory(o.strictMemory);
	dispatch::Init();
	hle::Init();
	if (!o.dataDir.empty()) hle::LoadNidDatabase(fs::u8path(o.dataDir) / "nids.csv");
	hle::RegisterAllModules();
#if PSPC_HAVE_GPU
	SetUpRenderer(o);
#endif
	sched::Init(o.fastTime);
	io::Init();
	io::Mount("ms0", io::MakeHostDevice(o.memstickDir, false));
	if (!o.pluginsDir.empty() && io::AddHostFallback("ms0", o.pluginsDir))
		LOGI("M33", "Plugins folder %s overlays ms0:", o.pluginsDir.c_str());
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
	utility::SetHeadlessOskText(o.oskText);
	net::Config netConfig;
	netConfig.enabled = o.network;
	netConfig.adhocServer = o.adhocServer;
	netConfig.npServer = o.npServer;
	netConfig.npOnlineId = o.npOnlineId;
	netConfig.npToken = o.npToken;
	netConfig.infraDns = o.infraDns;
	netConfig.dataDir = o.dataDir;
	net::Configure(netConfig);
	if (!o.dumpFramesDir.empty()) {
		std::error_code ec;
		fs::create_directories(fs::u8path(o.dumpFramesDir), ec);
		std::string dir = o.dumpFramesDir;
		u32 every = (u32)std::max(1, o.dumpEvery);
		display::SetVblankHook([dir, every]() {
			ctrl::OnVblank();
			ReportGpuStats();
			if (display::Vcount() % every == 0) DumpFrame(dir);
		});
	} else {
		display::SetVblankHook([]() {
			ctrl::OnVblank();
			ReportGpuStats();
		});
	}

	// A sceKernelLoadExec relaunch boots the requested guest path, with every device mounted as
	// for the original program.
	std::vector<u8> bootArgs;
	if (!o.bootPath.empty()) {
		std::vector<u8> image;
		if (!io::ReadFile(o.bootPath, image) || image.empty()) {
			LOGE("BOOT", "LoadExec target %s not found", o.bootPath.c_str());
			return 2;
		}
		bt.image = std::move(image);
		bt.pspPath = o.bootPath;
		bt.cwd = o.bootPath.substr(0, o.bootPath.rfind('/') + 1);
		io::SetCwd(bt.cwd);
		if (!o.bootArgsFile.empty()) ReadWholeFile(fs::u8path(o.bootArgsFile), bootArgs);
	}

	// The M33 layer: config.se, kernel NID resolver, plugins (docs/m33-behaviors.md).
	vsh::SetActive(o.vsh && o.bootPath.empty());
	vsh::SetReturnToXmb(!vsh::Active() && o.returnToVsh);
	// HOME in a game: the HOME menu (the XMB handles HOME itself).
	homemenu::Install(!vsh::Active());
	m33::BootInfo bi;
	bi.fromDisc = bt.pspPath.rfind("disc0:", 0) == 0;
	bi.apitype = vsh::Active() ? m33::APITYPE_VSH : bi.fromDisc ? m33::APITYPE_GAME_UMD : m33::APITYPE_GAME_MS;
	bi.initFileName = bt.pspPath;
	m33::Init(o.flash1Dir, bi);
	if (!o.dataDir.empty()) m33::RegisterNidResolver((fs::u8path(o.dataDir) / "m33_nid_resolver.csv").string());
	if (o.plugins) m33::LoadPlugins(o.dataDir.empty() ? std::string() : (fs::u8path(o.dataDir) / "fw_boot_modules.csv").string());

	if (!o.netSelftest.empty()) {
		if (net::selftest::Start(o.netSelftest) < 0) return 2;
	} else if (vsh::Active()) {
		// No disc unless one was given: the XMB shows an empty drive. UMD images in ms0:/ISO and
		// in pspc's Games folder show in the Game menu (M33 vshctrl).
		umd::SetDriveEmpty(!media::HaveDisc());
		m33::InstallIsoBrowser({o.gamesDir});
		io::Mount("msstor0p1", io::MakeMsStorDevice());
		io::Mount("msstor0", io::MakeMsStorDevice());
		{
			// The second memory stick the Saved Data Utility copies to: ms1 next to ms0.
			fs::path ms = fs::u8path(o.memstickDir);
			std::string name = PathToUtf8(ms.filename());
			if (name.empty()) name = PathToUtf8(ms.parent_path().filename()), ms = ms.parent_path();
			const std::string other = !name.empty() && name.back() == '0' ? name.substr(0, name.size() - 1) + "1" : name + "_2";
			io::SetOtherMemoryStick(PathToUtf8(ms.parent_path() / fs::u8path(other)));
		}
		if (!o.vshArgsFile.empty()) {
			// Back from a game the XMB launched: its vshmain_args, which LoadExec kept.
			std::vector<u8> vshArgs;
			if (ReadWholeFile(fs::u8path(o.vshArgsFile), vshArgs)) vsh::SetVshmainArgs(std::move(vshArgs));
			std::error_code ec;
			fs::remove(fs::u8path(o.vshArgsFile), ec);
		}
		if (vsh::Boot(o.recovery) < 0) return 2;
	} else {
		SceUID mod = boot::BootModule(bt.image, bt.pspPath, bootArgs);
		if (mod < 0) {
			LOGE("BOOT", "Failed to load %s: %08x", bt.pspPath.c_str(), mod);
			return 2;
		}
	}
	int rc = sched::Run();
	LOGI("RUNTIME", "Exited with code %d after %llu interpreted instructions, %u bad memory accesses", rc,
	     (unsigned long long)interp::InstructionCount(), mem::BadAccessCount());
	if (logging::g_level <= LogLevel::Info) hle::DumpUnimplementedStats();
	WriteAotHints();
	return rc;
}

std::string g_relaunchTarget;
std::string g_loadExecPath, g_loadExecArgsFile;
bool g_exitToVsh = false;
std::string g_vshArgsFile;

}  // namespace

bool TakeExitToVsh() {
	bool r = g_exitToVsh;
	g_exitToVsh = false;
	return r;
}

bool TakeVshArgsFile(std::string &file) {
	if (g_vshArgsFile.empty()) return false;
	file = g_vshArgsFile;
	g_vshArgsFile.clear();
	return true;
}

bool TakeRelaunch(std::string &hostFile) {
	if (g_relaunchTarget.empty()) return false;
	hostFile = g_relaunchTarget;
	g_relaunchTarget.clear();
	return true;
}

bool TakeLoadExecRelaunch(std::string &pspPath, std::string &argsFile) {
	if (g_loadExecPath.empty()) return false;
	pspPath = g_loadExecPath;
	argsFile = g_loadExecArgsFile;
	g_loadExecPath.clear();
	return true;
}

int Run(const Options &o) {
	host::Config hc;
	hc.headless = o.headless;
	hc.windowScale = o.windowScale;
	hc.title = o.vsh && o.file.empty() ? std::string("PSPC - XMB") : "PSPC - " + PathToUtf8(fs::u8path(o.file).filename());
	hc.fullscreen = o.fullscreen;
	hc.vsync = o.vsync;
	hc.volume = o.volume;
	hc.keys = o.keyBindings;
	hc.pads = o.padBindings;
	hc.padStick = o.padStick;
	hc.screenshotDir = o.screenshotDir;
	hc.gpuPresent = RendererName(o).rfind("d3d11", 0) == 0;
	{
		// Screenshot names: the game file's name (its folder's for EBOOT.PBP).
		fs::path f = fs::u8path(o.file.empty() ? std::string("xmb") : o.file);
		std::string name = PathToUtf8(ToLower(PathToUtf8(f.filename())) == "eboot.pbp" ? f.parent_path().filename() : f.stem());
		for (char &ch : name)
			if (strchr("<>:\"/\\|?* ", ch)) ch = '_';
		if (!name.empty()) hc.gameName = name;
	}

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
#if PSPC_HAVE_GPU
			ReleaseRendererWindow();
#endif
			std::string next;
			std::vector<u8> args;
			if (code == boot::kExitLoadExec && boot::TakeLoadExecRequest(next, args)) {
				// LoadExec needs a full reset (fresh memory and kernel objects): the frontend starts a
				// new process that boots `next`. The argument block travels in a temp file.
				wchar_t tmp[MAX_PATH];
				GetTempPathW(MAX_PATH, tmp);
				fs::path argsFile = fs::path(tmp) / ("pspc_loadexec_" + std::to_string(GetCurrentProcessId()) + ".bin");
				if (!args.empty()) WriteWholeFile(argsFile, args.data(), args.size());
				LOGI("RUNTIME", "sceKernelLoadExec(%s): restarting", next.c_str());
				g_loadExecPath = next;
				g_loadExecArgsFile = args.empty() ? "" : PathToUtf8(argsFile);
				code = 0;
			}
			// M33's LoadExecVSH family (and the XMB launching a game) swap the host file.
			std::string m33next;
			if (m33::TakeRelaunch(m33next)) {
				LOGI("RUNTIME", "LoadExec %s: restarting", m33next.c_str());
				g_relaunchTarget = m33next;
				code = 0;
				// The XMB's own state goes with the game and comes back to vshmain (--vsh-args).
				std::vector<u8> vshArgs;
				if (m33::TakeVshmainArgs(vshArgs)) {
					wchar_t tmp[MAX_PATH];
					GetTempPathW(MAX_PATH, tmp);
					fs::path f = fs::path(tmp) / ("pspc_vshargs_" + std::to_string(GetCurrentProcessId()) + ".bin");
					if (WriteWholeFile(f, vshArgs.data(), vshArgs.size())) g_vshArgsFile = PathToUtf8(f);
				}
			}
			if (m33::TakeExitToVsh()) {
				g_exitToVsh = true;
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
