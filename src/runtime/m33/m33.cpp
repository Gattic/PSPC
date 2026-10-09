// SPDX-License-Identifier: GPL-2.0-or-later
//
// SystemCtrlForUser/Kernel, KUBridge and VshCtrlLib, implemented natively from
// docs/m33-behaviors.md. Each function notes which M33 behaviour it reproduces.
#include "runtime/m33/m33.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <fstream>
#include <sstream>

#include "common/fileutil.h"
#include "common/log.h"
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
	GWrite32(a + 0x00, 0);  // next
	GWrite16(a + 0x04, mi.attr);
	GWrite8(a + 0x06, mi.version[0]);
	GWrite8(a + 0x07, mi.version[1]);
	mem::WriteBytes(a + 0x08, m->name.c_str(), (u32)std::min<size_t>(m->name.size(), 27));
	GWrite8(a + 0x24, (u8)m->status);
	GWrite32(a + 0x2C, (u32)m->uid);
	GWrite32(a + 0x40, mi.libent);
	GWrite32(a + 0x44, mi.libentEnd - mi.libent);
	GWrite32(a + 0x48, mi.libstub);
	GWrite32(a + 0x4C, mi.libstubEnd - mi.libstub);
	GWrite32(a + 0x50, m->moduleStart);
	GWrite32(a + 0x64, m->moduleStart);
	GWrite32(a + 0x68, m->gp);
	u32 textEnd = m->memAddr;
	for (const auto &r : m->image.codeRanges) textEnd = std::max(textEnd, r.second);
	GWrite32(a + 0x6C, m->memAddr);
	GWrite32(a + 0x70, textEnd - m->memAddr);
	GWrite32(a + 0x74, m->memAddr + m->memSize - textEnd);
	u32 nseg = 0;
	for (const auto &seg : m->image.segments) {
		if (!seg.loadAddr || nseg == 4) continue;
		GWrite32(a + 0x80 + 4 * nseg, seg.loadAddr);
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
	cpu::CallGuest(&t->ctx, g_startModuleHandler, {mod2});
}

}  // namespace

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

// seplugins/<list>.txt with enable bytes in seplugins/conf.bin (5 slots per list:
// vsh 0-4, game 5-9, pops 10-14). A second token ("path 1"/"path 0") overrides the byte.
void LoadPlugins() {
	const char *list = g_boot.apitype == APITYPE_VSH ? "vsh.txt" : g_boot.apitype == APITYPE_POPS ? "pops.txt" : "game.txt";
	int base = g_boot.apitype == APITYPE_VSH ? 0 : g_boot.apitype == APITYPE_POPS ? 10 : 5;
	std::vector<u8> txt, conf;
	if (!io::ReadFile(std::string("ms0:/seplugins/") + list, txt)) return;
	io::ReadFile("ms0:/seplugins/conf.bin", conf);
	std::istringstream in(std::string(txt.begin(), txt.end()));
	std::string line;
	int index = 0;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
		if (line.empty()) continue;
		std::string path = line;
		bool enabled = (size_t)(base + index) < conf.size() ? conf[base + index] != 0 : true;
		size_t sp = line.find_first_of(" \t");
		if (sp != std::string::npos) {
			path = line.substr(0, sp);
			std::string flag = line.substr(line.find_first_not_of(" \t", sp));
			enabled = flag != "0";
		}
		index++;
		if (!enabled) continue;
		std::vector<u8> data;
		if (!io::ReadFile(path, data)) {
			LOGW("M33", "Plugin %s not found", path.c_str());
			continue;
		}
		kernel::LoadOptions opts;
		SceUID id = kernel::LoadModuleFromMemory(data, path, opts);
		if (id < 0) {
			LOGW("M33", "Plugin %s failed to load (%08x)", path.c_str(), id);
			continue;
		}
		LOGI("M33", "Plugin %s loaded", path.c_str());
		if (auto *m = kernel::Get<kernel::PspModule>(id)) kernel::StartModule(m, 0, 0, 0, false);
	}
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
		if (hle::AddAlias(lib, o, n)) added++;
	}
	LOGI("M33", "NID resolver: %d old kernel NIDs mapped onto native functions", added);
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
		// The mounted ISO is the new executable's disc.
		host = g_umdFile.empty() ? std::string() : io::HostPathFor(g_umdFile);
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
int sctrlKernelExitVSH(u32) {
	sched::RequestStop(0);
	return 0;
}

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
	g_vshMenuCallback = cb | 0x80000000;
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
