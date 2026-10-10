// SPDX-License-Identifier: GPL-2.0-or-later
//
// SystemCtrlForUser/Kernel, KUBridge and VshCtrlLib, implemented natively from
// docs/m33-behaviors.md. Each function notes which M33 behaviour it reproduces.
#include "runtime/m33/m33.h"
#include "runtime/m33/vsh_iso.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <fstream>
#include <sstream>

#include "common/fileutil.h"
#include "common/log.h"
#include "common/seplugins.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/io/io.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/media/disc.h"

namespace m33 {

namespace {

SEConfig g_config{};
fs::path g_configPath;
BootInfo g_boot;
std::string g_umdFile;
u32 g_discType = 0x10;
bool g_discOut = false;
u32 g_bootConfIndex = 0;
u32 g_startModuleHandler = 0;
u32 g_vshMenuCallback = 0;
std::string g_relaunch;
bool g_relaunchPending = false;
std::vector<u8> g_vshmainArgs;  // the XMB's vshmain_args for the game it launches
bool g_exitToVshPending = false;

constexpr u32 SE_MAGIC = 0x47434553;
constexpr u32 HEN_VERSION = 0x00000400;   // sctrlHENGetVersion (docs/m33-behaviors.md 2.1)
constexpr u32 SE_VERSION = 0x00010000;    // sctrlSEGetVersion: "4.01 M33" with no sub-revision

void DefaultConfig(SEConfig &c) {
	memset(&c, 0, sizeof(c));
	c.magic = (s32)SE_MAGIC;
	c.umdmode = 2;  // M33 driver
}

void SaveConfig() {
	if (g_configPath.empty()) return;
	WriteWholeFile(g_configPath, &g_config, sizeof(g_config));
}

// The firmware's module descriptor (SceModule2 in the M33 SDK's psploadcore.h), which M33 hands to
// start-module handlers. Built in kernel memory on first use, one per module.
std::map<SceUID, u32> g_module2;

u32 Module2For(kernel::PspModule *m) {
	auto it = g_module2.find(m->uid);
	u32 a = it != g_module2.end() ? it->second : kernel::AllocKernelMemory(0xA0, "SceModule2");
	if (!a) return 0;
	g_module2[m->uid] = a;
	mem::Memset(a, 0, 0xA0);
	const loader::PspModuleInfoRaw &mi = m->image.modInfo;
	// Kernel modules' addresses are in the kernel segment (0x88...), as plugins check.
	const u32 kseg = m->kernelMode ? 0x80000000 : 0;
	auto k = [kseg](u32 addr) { return addr ? addr | kseg : 0; };
	GWrite32(a + 0x00, 0);  // next
	GWrite16(a + 0x04, mi.attr);
	GWrite8(a + 0x06, mi.version[0]);
	GWrite8(a + 0x07, mi.version[1]);
	mem::WriteBytes(a + 0x08, m->name.c_str(), (u32)std::min<size_t>(m->name.size(), 27));
	GWrite8(a + 0x24, (u8)m->status);
	GWrite32(a + 0x2C, (u32)m->uid);
	GWrite32(a + 0x40, k(mi.libent));
	GWrite32(a + 0x44, mi.libentEnd - mi.libent);
	// The stub table follows the entry table in every module; plugins measure the entry table
	// as stub_top - ent_top.
	GWrite32(a + 0x48, k(mi.libstub ? mi.libstub : mi.libentEnd));
	GWrite32(a + 0x4C, mi.libstubEnd - mi.libstub);
	GWrite32(a + 0x50, k(m->moduleStart));
	GWrite32(a + 0x64, k(m->moduleStart));
	GWrite32(a + 0x68, m->gp);
	u32 textEnd = m->memAddr;
	for (const auto &r : m->image.codeRanges) textEnd = std::max(textEnd, r.second);
	GWrite32(a + 0x6C, k(m->memAddr));
	GWrite32(a + 0x70, textEnd - m->memAddr);
	GWrite32(a + 0x74, m->memAddr + m->memSize - textEnd);
	u32 nseg = 0;
	for (const auto &seg : m->image.segments) {
		if (!seg.loadAddr || nseg == 4) continue;
		GWrite32(a + 0x80 + 4 * nseg, k(seg.loadAddr));
		GWrite32(a + 0x90 + 4 * nseg, seg.memsz);
		nseg++;
	}
	GWrite32(a + 0x7C, nseg);
	return a;
}

// Runs on a module's start thread just before module_start (kernel::SetModuleStartHook).
void OnModuleStart(SceUID id) {
	if (!g_startModuleHandler) return;
	kernel::PspModule *m = kernel::Get<kernel::PspModule>(id);
	kernel::PspThread *t = sched::Current();
	if (!m || !t) return;
	u32 mod2 = Module2For(m);
	if (!mod2) return;
	LOGD("M33", "start-module handler %08x for %s", g_startModuleHandler, m->name.c_str());
	cpu::CallGuest(&t->ctx, g_startModuleHandler, {mod2 | 0x80000000});
}

}  // namespace

u32 ModuleDescriptor(int uid) {
	kernel::PspModule *m = kernel::Get<kernel::PspModule>(uid);
	const u32 a = m ? Module2For(m) : 0;
	return a ? a | 0x80000000 : 0;  // kernel memory, seen through the kernel segment
}

void Init(const std::string &flash1HostDir, const BootInfo &boot) {
	g_boot = boot;
	g_startModuleHandler = 0;
	g_module2.clear();
	kernel::SetModuleStartHook(&OnModuleStart);
	g_configPath = fs::u8path(flash1HostDir) / "config.se";
	std::vector<u8> data;
	if (ReadWholeFile(g_configPath, data) && data.size() >= sizeof(SEConfig)) {
		memcpy(&g_config, data.data(), sizeof(SEConfig));
		if ((u32)g_config.magic != SE_MAGIC) DefaultConfig(g_config);
	} else {
		DefaultConfig(g_config);
	}
}

const SEConfig &Config() { return g_config; }
void SetConfig(const SEConfig &c) {
	g_config = c;
	SaveConfig();
}

bool TakeRelaunch(std::string &hostFile) {
	if (!g_relaunchPending) return false;
	g_relaunchPending = false;
	hostFile = g_relaunch;
	return true;
}

bool TakeExitToVsh() {
	bool r = g_exitToVshPending;
	g_exitToVshPending = false;
	return r;
}

int ExitToVsh() {
	LOGI("M33", "Exit to the XMB");
	g_exitToVshPending = true;
	sched::RequestStop(0x4D33);
	return 0;
}

// The modules 4.01 and M33 have loaded when a game's plugins start, from data/fw_boot_modules.csv
// (tools/gen_fw_modules.py): pspc implements them natively; plugins find them by name and walk
// their export tables.
struct BootModule {
	std::string name;
	u16 attr = 0, version = 0;
	std::vector<kernel::FirmwareExport> libs;
};

std::vector<BootModule> ReadBootModules(const std::string &csvPath) {
	std::vector<BootModule> mods;
	std::ifstream in(fs::u8path(csvPath));
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::vector<std::string> f;
		std::stringstream ss(line);
		for (std::string t; std::getline(ss, t, ',');) f.push_back(t);
		if (f.size() >= 4 && f[0] == "module") {
			BootModule m;
			m.name = f[1];
			m.attr = (u16)strtoul(f[2].c_str(), nullptr, 16);
			m.version = (u16)strtoul(f[3].c_str(), nullptr, 16);
			mods.push_back(std::move(m));
		} else if (f.size() >= 5 && f[0] == "lib" && !mods.empty()) {
			kernel::FirmwareExport e;
			e.library = f[1];
			e.version = (u16)strtoul(f[2].c_str(), nullptr, 16);
			e.attr = (u16)strtoul(f[3].c_str(), nullptr, 16);
			std::stringstream ns(f[4]);
			for (std::string n; ns >> n;) e.nids.push_back((u32)strtoul(n.c_str(), nullptr, 16));
			mods.back().libs.push_back(std::move(e));
		}
	}
	return mods;
}

// The user-mode kernel library: the last module before the game, registered after the plugins.
constexpr const char *kUserKernelLibrary = "sceKernelLibrary";

// The list for the boot type (common/seplugins.h has the format), read through ms0: so that
// pspc's plugins folder counts.
void LoadPlugins(const std::string &bootModulesCsv) {
	const seplugins::List which = g_boot.apitype == APITYPE_VSH    ? seplugins::List::Vsh
	                              : g_boot.apitype == APITYPE_POPS ? seplugins::List::Pops
	                                                               : seplugins::List::Game;
	std::vector<u8> txt, conf;
	if (!io::ReadFile(std::string("ms0:/seplugins/") + seplugins::FileName(which), txt)) return;
	io::ReadFile("ms0:/seplugins/conf.bin", conf);
	int loaded = 0;
	bool registered = false;
	std::vector<BootModule> boot;
	for (const seplugins::Entry &e : seplugins::Parse(std::string(txt.begin(), txt.end()), which, conf)) {
		const std::string &path = e.path;
		if (!e.enabled) {
			LOGI("M33", "Plugin %s is disabled", path.c_str());
			continue;
		}
		std::vector<u8> data;
		if (!io::ReadFile(path, data)) {
			LOGW("M33", "Plugin %s not found", path.c_str());
			continue;
		}
		// Before the first plugin: the firmware modules a real boot has loaded by now.
		if (!registered) {
			registered = true;
			boot = ReadBootModules(bootModulesCsv);
			if (boot.empty()) LOGW("M33", "No firmware module list (%s): plugins won't find system modules", bootModulesCsv.c_str());
			for (const BootModule &b : boot)
				if (b.name != kUserKernelLibrary) kernel::RegisterFirmwareModule(b.name, b.attr, b.version, b.libs);
		}
		kernel::LoadOptions opts;
		SceUID id = kernel::LoadModuleFromMemory(data, path, opts);
		if (id < 0) {
			LOGW("M33", "Plugin %s failed to load (%08x)", path.c_str(), id);
			continue;
		}
		LOGI("M33", "Plugin %s loaded", path.c_str());
		loaded++;
		if (auto *m = kernel::Get<kernel::PspModule>(id)) kernel::StartModule(m, 0, 0, 0, false);
	}
	if (!loaded) return;
	bool userLib = false;
	for (const BootModule &b : boot) {
		if (b.name != kUserKernelLibrary) continue;
		kernel::RegisterFirmwareModule(b.name, b.attr, b.version, b.libs);
		userLib = true;
	}
	if (!userLib) kernel::RegisterFirmwareModule(kUserKernelLibrary, 0x0007, 0x0103);
}

void RegisterNidResolver(const std::string &csvPath) {
	std::ifstream in(csvPath);
	if (!in) return;
	std::string line;
	int added = 0;
	while (std::getline(in, line)) {
		std::stringstream ss(line);
		std::string lib, oldNid, newNid;
		if (!std::getline(ss, lib, ',') || !std::getline(ss, oldNid, ',') || !std::getline(ss, newNid, ',')) continue;
		if (oldNid.rfind("0x", 0) != 0) continue;
		u32 o = (u32)strtoul(oldNid.c_str(), nullptr, 16), n = (u32)strtoul(newNid.c_str(), nullptr, 16);
		// Plugins import the old (SDK) NIDs; 4.01's own modules (the XMB once firmware is supplied)
		// import the new ones. Native functions are registered under one or the other: alias both
		// ways so either finds them.
		if (hle::AddAlias(lib, o, n)) added++;
		if (hle::AddAlias(lib, n, o)) added++;
	}
	LOGI("M33", "NID resolver: %d kernel NID aliases added (old <-> 4.01)", added);
}

}  // namespace m33

namespace {

using namespace m33;

// ---- SystemCtrl: version and identity ----
u32 sctrlHENGetVersion() { return HEN_VERSION; }
u32 sctrlSEGetVersion() { return SE_VERSION; }
int sctrlHENIsSE() { return 1; }
int sctrlHENIsDevhook() { return 0; }

// ---- SystemCtrl: config.se ----
int sctrlSEGetConfigEx(u32 cfgPtr, int size) {
	if (size < 0 || !mem::IsValidRange(cfgPtr, (u32)size)) return -1;
	mem::WriteBytes(cfgPtr, &g_config, (u32)std::min<int>(size, sizeof(SEConfig)));
	return 0;
}
int sctrlSEGetConfig(u32 cfgPtr) { return sctrlSEGetConfigEx(cfgPtr, sizeof(SEConfig)); }
int sctrlSESetConfigEx(u32 cfgPtr, int size) {
	if (size < 0 || !mem::IsValidRange(cfgPtr, (u32)size)) return -1;
	SEConfig c = g_config;
	mem::ReadBytes(&c, cfgPtr, (u32)std::min<int>(size, sizeof(SEConfig)));
	SetConfig(c);
	return 0;
}
int sctrlSESetConfig(u32 cfgPtr) { return sctrlSESetConfigEx(cfgPtr, sizeof(SEConfig)); }

// ---- SystemCtrl: UMD / ISO emulation (march33/galaxy roles, done by the native disc layer) ----
int sctrlSEMountUmdFromFile(const char *file, int noumd, int isofs) {
	(void)noumd;
	(void)isofs;
	if (!file) return -1;
	std::string host = io::HostPathFor(file);
	if (host.empty() || !media::MountDisc(host)) {
		LOGW("M33", "sctrlSEMountUmdFromFile(%s) failed", file);
		return -1;
	}
	g_umdFile = file;
	return 0;
}
int sctrlSEUmountUmd() {
	media::UnmountDisc();
	g_umdFile.clear();
	return 0;
}
void sctrlSESetDiscOut(int out) { g_discOut = out != 0; }
void sctrlSESetDiscType(int type) { g_discType = (u32)type; }
u32 g_umdFileBuf = 0;
u32 sctrlSEGetUmdFile() {
	if (!g_umdFileBuf) g_umdFileBuf = kernel::AllocKernelMemory(256, "umd file");
	mem::WriteBytes(g_umdFileBuf, g_umdFile.c_str(), (u32)std::min<size_t>(g_umdFile.size() + 1, 255));
	return g_umdFileBuf;
}
void sctrlSESetUmdFile(const char *file) { g_umdFile = file ? file : ""; }
void sctrlSESetBootConfFileIndex(int index) { g_bootConfIndex = (u32)index; }

// ---- SystemCtrl: module/syscall helpers ----
u32 sctrlHENFindFunction(const char *modName, const char *libName, u32 nid) {
	if (!libName) return 0;
	for (SceUID id : kernel::LoadedModules()) {
		auto *m = kernel::Get<kernel::PspModule>(id);
		if (!m || (modName && m->name != modName)) continue;
		for (const auto &lib : m->exports) {
			if (lib.name != libName) continue;
			for (auto &f : lib.funcs)
				if (f.first == nid) return f.second;
		}
	}
	// Firmware libraries are native: hand out a stub that calls the implementation.
	return hle::MakeGuestStub(libName, nid);
}
void sctrlHENPatchSyscall(u32 addr, u32 newAddr) {
	if (!hle::RedirectStubToGuest(addr, newAddr))
		LOGW("M33", "sctrlHENPatchSyscall(%08x -> %08x): not a native function stub", addr, newAddr);
}
u32 sctrlHENSetStartModuleHandler(u32 handler) {
	u32 old = g_startModuleHandler;
	g_startModuleHandler = handler;
	return old;
}
u32 sctrlHENFindDriver(const char *name) {
	LOGD("M33", "sctrlHENFindDriver(%s): no driver structures in the native kernel", name ? name : "");
	return 0;
}
int sctrlHENSetMemory(u32 p2, u32 p8) {
	LOGI("M33", "sctrlHENSetMemory(%u, %u) ignored", p2, p8);
	return 0;
}
void sctrlHENSetSpeed(int cpu, int bus) { LOGD("M33", "sctrlHENSetSpeed(%d, %d)", cpu, bus); }
void sctrlHENLoadModuleOnReboot(const char *after, u32 buf, int size, int flags) {
	(void)buf;
	LOGI("M33", "sctrlHENLoadModuleOnReboot(after %s, %d bytes, flags %x) recorded", after ? after : "", size, flags);
}

// ---- SystemCtrl: LoadExec family -> relaunch the runtime with the new executable ----
int LoadExecTo(const char *file, u32 apitype) {
	if (!file) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::string f = file;
	std::string host;
	if (f.rfind("disc0:", 0) == 0 || f.rfind("umd0:", 0) == 0) {
		// The mounted ISO is the new executable's disc (mounted by M33, or the disc in the drive).
		host = g_umdFile.empty() ? media::DiscPath() : io::HostPathFor(g_umdFile);
	} else {
		host = io::HostPathFor(f);
	}
	if (host.empty()) {
		LOGW("M33", "LoadExec(%s): not reachable on the host", file);
		return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	}
	LOGI("M33", "LoadExec %s (apitype %x)", file, apitype);
	g_relaunch = host;
	g_relaunchPending = true;
	sched::RequestStop(0x4D33);  // runtime relaunches on this code
	return 0;
}
int sctrlKernelLoadExecVSHWithApitype(int apitype, const char *file, u32) { return LoadExecTo(file, (u32)apitype); }
int sctrlKernelLoadExecVSHDisc(const char *file, u32) { return LoadExecTo(file, 0x120); }
int sctrlKernelLoadExecVSHDiscUpdater(const char *file, u32) { return LoadExecTo(file, 0x121); }
int sctrlKernelLoadExecVSHMs1(const char *file, u32) { return LoadExecTo(file, 0x140); }
int sctrlKernelLoadExecVSHMs2(const char *file, u32) { return LoadExecTo(file, 0x141); }
int sctrlKernelLoadExecVSHMs3(const char *file, u32) { return LoadExecTo(file, 0x142); }
int sctrlKernelLoadExecVSHMs4(const char *file, u32) { return LoadExecTo(file, 0x143); }
// Back to the XMB (the frontend restarts it when it booted it; otherwise the run just ends).
int sctrlKernelExitVSH(u32) { return ExitToVsh(); }

// ---- SystemCtrl: kernel identity overrides ----
int sctrlKernelSetInitApitype(int apitype) {
	int old = (int)g_boot.apitype;
	g_boot.apitype = (u32)apitype;
	return old;
}
int sctrlKernelSetInitFileName(const char *name) {
	g_boot.initFileName = name ? name : "";
	return 0;
}
int sctrlKernelSetInitMode(int mode) {
	(void)mode;
	return 0;
}
int sctrlKernelSetUserLevel(int level) {
	(void)level;
	return 4;
}
int sctrlKernelSetDevkitVersion(u32 version) {
	u32 old = kernel::g_firmwareVersion;
	kernel::g_firmwareVersion = version;
	return (int)old;
}

// ---- KUBridge ----
int kuKernelLoadModule(const char *path, int flags, u32 opt) {
	(void)flags;
	(void)opt;
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::vector<u8> data;
	if (!io::ReadFile(path, data)) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	kernel::LoadOptions o;
	return kernel::LoadModuleFromMemory(data, path, o);
}
int kuKernelLoadModuleWithApitype2(int apitype, const char *path, int flags, u32 opt) {
	(void)apitype;
	return kuKernelLoadModule(path, flags, opt);
}
int kuKernelInitApitype() { return (int)g_boot.apitype; }
int kuKernelInitFileName(u32 out) {
	if (mem::IsValidRange(out, (u32)g_boot.initFileName.size() + 1))
		mem::WriteBytes(out, g_boot.initFileName.c_str(), (u32)g_boot.initFileName.size() + 1);
	return 0;
}
int kuKernelBootFrom() { return g_boot.fromDisc ? 0x20 : 0x40; }
int kuKernelInitKeyConfig() { return g_boot.apitype == APITYPE_VSH ? 0x100 : g_boot.apitype == APITYPE_POPS ? 0x300 : 0x200; }
int kuKernelGetUserLevel() { return 4; }
int kuKernelSetDdrMemoryProtection(u32, int, int) { return 0; }
int kuKernelGetModel() { return mem::g_ramSize > 0x02000000 ? 1 : 0; }

// ---- VshCtrlLib ----
int vctrlVSHRegisterVshMenu(u32 cb) {
	// (vshctrl runs it in kernel mode; here it's called as the guest function it is.)
	g_vshMenuCallback = cb;
	return 0;
}
int vctrlVSHExitVSHMenu(u32 confPtr, const char *videoIso, int discType) {
	g_vshMenuCallback = 0;
	if (mem::IsValidRange(confPtr, sizeof(SEConfig))) {
		SEConfig c;
		mem::ReadBytes(&c, confPtr, sizeof(c));
		SetConfig(c);
	}
	if (videoIso) {
		g_discType = (u32)discType;
		sctrlSEMountUmdFromFile(videoIso, 0, 0);
	}
	return 0;
}

}  // namespace

namespace m33 {

// vshctrl's hook on the XMB's controller reads (docs/m33-behaviors.md section 3). SELECT opens
// the VSH menu: flash0:/vsh/module/satelite.prx is loaded and started (not while the browser,
// the on-screen keyboard or the camera plugin runs, nor with config.se's novshmenu), and SELECT
// is taken out of the read. While the menu runs, the callback it registered gets every read
// (it draws and takes the input); after it unregisters (vctrlVSHExitVSHMenu) it is stopped and
// unloaded.
void OnVshCtrlRead(u32 padPtr, int count) {
	kernel::PspModule *menu = kernel::ModuleByName("VshCtrlSatelite");
	if (!menu) {
		if (g_config.novshmenu || count < 1 || !mem::IsValidRange(padPtr, 16)) return;
		if (!(GRead32(padPtr + 4) & 0x1)) return;  // SELECT
		for (const char *busy : {"htmlviewer_plugin_module", "sceVshOSK_Module", "camera_plugin_module"})
			if (kernel::ModuleByName(busy)) return;
		std::vector<u8> data;
		if (!io::ReadFile("flash0:/vsh/module/satelite.prx", data)) {
			LOGW("M33", "VSH menu: flash0:/vsh/module/satelite.prx not installed");
			return;
		}
		kernel::LoadOptions opts;
		SceUID id = kernel::LoadModuleFromMemory(data, "flash0:/vsh/module/satelite.prx", opts);
		if (id < 0) return;
		g_vshMenuCallback = 0;
		LOGI("M33", "VSH menu opened");
		kernel::StartModule(kernel::Get<kernel::PspModule>(id), 0, 0, 0, true);
		GWrite32(padPtr + 4, GRead32(padPtr + 4) & ~1u);
		return;
	}
	if (g_vshMenuCallback) {
		if (kernel::PspThread *t = sched::Current()) cpu::CallGuest(&t->ctx, g_vshMenuCallback, {padPtr, (u32)count});
		return;
	}
	if (menu->status == kernel::MODULE_STATUS_STARTED && kernel::StopModule(menu, 0, 0, 0) < 0) return;
	if (menu->status != kernel::MODULE_STATUS_STARTING && menu->status != kernel::MODULE_STATUS_STOPPING) {
		kernel::UnloadModule(menu);
		LOGI("M33", "VSH menu closed");
	}
}

bool TakeVshmainArgs(std::vector<u8> &out) {
	if (g_vshmainArgs.empty()) return false;
	out = std::move(g_vshmainArgs);
	g_vshmainArgs.clear();
	return true;
}

int LoadExecFromVsh(const char *file, u32 apitype, u32 paramPtr) {
	// SceKernelLoadExecVSHParam: size, args, argp, key, vshmain_args_size, vshmain_args, ... The
	// vshmain_args block is the XMB's own state (which item was selected, that it has booted
	// before, the launch result); LoadExec keeps it through the game and gives it back to vshmain
	// when the game exits.
	g_vshmainArgs.clear();
	if (mem::IsValidRange(paramPtr, 0x18)) {
		const u32 size = GRead32(paramPtr + 0x10), args = GRead32(paramPtr + 0x14);
		LOGD("M33", "LoadExecVSH param %08x: size %u, vshmain_args %u bytes at %08x", paramPtr, GRead32(paramPtr), size, args);
		if ((size == 0x20 || size == 0x400) && mem::IsValidRange(args, size)) {
			g_vshmainArgs.resize(size);
			mem::ReadBytes(g_vshmainArgs.data(), args, size);
			// LoadExec frames the block for vshmain's module_start: its size, then 0x20.
			const u32 frame[2] = {size, 0x20};
			memcpy(g_vshmainArgs.data(), frame, sizeof(frame));
		}
	}
	// A UMD image the Game menu shows as a memory stick game (vshctrl): boot the image.
	std::string iso;
	if (file && IsoForVirtualGamePath(file, &iso)) {
		LOGI("M33", "LoadExec %s: the UMD image %s", file, iso.c_str());
		g_umdFile = file;
		g_relaunch = iso;
		g_relaunchPending = true;
		sched::RequestStop(0x4D33);
		return 0;
	}
	// A game of GAME150/GAME4XX or of pspc's Games folder (vsh_iso.h): its EBOOT.PBP where it is.
	std::string host;
	if (file && HostPathForVirtualGame(file, &host)) {
		LOGI("M33", "LoadExec %s: %s", file, host.c_str());
		g_relaunch = host;
		g_relaunchPending = true;
		sched::RequestStop(0x4D33);
		return 0;
	}
	return LoadExecTo(file, apitype);
}
}  // namespace m33

void Register_M33() {
	static const hle::HleFunction user[] = {
		HLEFN(0x78E46415, sctrlHENFindDriver),
		HLEFN(0x1090A2E1, sctrlHENGetVersion),
		HLEFN(0x2E2935EF, sctrlHENIsDevhook),
		HLEFN(0xD339E2E9, sctrlHENIsSE),
		HLEFN(0x745286D1, sctrlHENSetMemory),
		HLEFN(0x1C90BECB, sctrlHENSetStartModuleHandler),
		HLEFN(0x2794CCF4, sctrlKernelExitVSH),
		HLEFN(0x577AF198, sctrlKernelLoadExecVSHDisc),
		HLEFN(0x94FE5E4B, sctrlKernelLoadExecVSHDiscUpdater),
		HLEFN(0x75643FCA, sctrlKernelLoadExecVSHMs1),
		HLEFN(0xABA7F1B0, sctrlKernelLoadExecVSHMs2),
		HLEFN(0x7B369596, sctrlKernelLoadExecVSHMs3),
		HLEFN(0xD690750F, sctrlKernelLoadExecVSHMs4),
		HLEFN(0x2D10FB28, sctrlKernelLoadExecVSHWithApitype),
		HLEFN(0xD8FF9B99, sctrlKernelSetDevkitVersion),
		HLEFN(0x8D5BE1F0, sctrlKernelSetInitApitype),
		HLEFN(0x128112C3, sctrlKernelSetInitFileName),
		HLEFN(0xCB76B778, sctrlKernelSetInitMode),
		HLEFN(0xB551220C, sctrlKernelSetInitMode),
		HLEFN(0xEB74FE45, sctrlKernelSetUserLevel),
		HLEFN(0x16C3B7EE, sctrlSEGetConfig),
		HLEFN(0x8E426F09, sctrlSEGetConfigEx),
		HLEFN(0xB47C9D77, sctrlSEGetVersion),
		HLEFN(0x85B520C6, sctrlSEMountUmdFromFile),
		HLEFN(0x1DDDAD0C, sctrlSESetConfig),
		HLEFN(0xAD4D5EA5, sctrlSESetConfigEx),
		HLEFN(0xFFEFA034, sctrlSESetDiscOut),
		HLEFN(0x31C6160D, sctrlSESetDiscType),
		HLEFN(0x512E0CD8, sctrlSEUmountUmd),
	};
	hle::RegisterModule("SystemCtrlForUser", user);
	hle::RegisterModule("SystemCtrlForKernel", user);
	static const hle::HleFunction kernelOnly[] = {
		HLEFN(0x159AF5CC, sctrlHENFindFunction),
		HLEFN(0xCE0A654E, sctrlHENLoadModuleOnReboot),
		HLEFN(0x826668E9, sctrlHENPatchSyscall),
		HLEFN(0xCC9ADCF8, sctrlHENSetSpeed),
		HLEFN(0xAC56B90B, sctrlSEGetUmdFile),
		HLEFN(0x5CB025F0, sctrlSESetBootConfFileIndex),
		HLEFN(0xB64186D0, sctrlSESetUmdFile),
	};
	hle::RegisterModule("SystemCtrlForKernel", kernelOnly);

	static const hle::HleFunction ku[] = {
		HLEFN(0x60DDB4AE, kuKernelBootFrom),
		HLEFN(0x24331850, kuKernelGetModel),
		HLEFN(0xA2ABB6D3, kuKernelGetUserLevel),
		HLEFN(0x8E5A4057, kuKernelInitApitype),
		HLEFN(0x1742445F, kuKernelInitFileName),
		HLEFN(0xB0B8824E, kuKernelInitKeyConfig),
		HLEFN(0x4C25EA72, kuKernelLoadModule),
		HLEFN(0x1E9F0498, kuKernelLoadModuleWithApitype2),
		HLEFN(0xC4AF12AB, kuKernelSetDdrMemoryProtection),
	};
	hle::RegisterModule("KUBridge", ku);

	static const hle::HleFunction vsh[] = {
		HLEFN(0xFD26DA72, vctrlVSHRegisterVshMenu),
		HLEFN(0xCD6B3913, vctrlVSHExitVSHMenu),
	};
	hle::RegisterModule("VshCtrlLib", vsh);
}
