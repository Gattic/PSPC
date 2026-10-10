// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc --vsh: the firmware's own VSH modules (paf, common_gui, common_util, vshmain and the
// plugins vshmain loads) run as guest code on the native kernel.
//
// On a PSP, init.prx walks the boot configuration (flash0:/kd/pspbtcnf*.bin) for the boot mode
// (VSH = mode flag 0x01), loading each module and waiting for its module_start before loading the
// next one. Here the kernel modules of that list are native and skipped; the user-mode ones load
// through the normal module loader (AOT DLL when built, else interpreted) and start one at a
// time on an "init" thread, so a module's module_start has finished (scePaf's heap pool is set
// up, the VSH graphics thread is created) before the next module runs.
#include "runtime/vsh.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "loader/elf.h"
#include "runtime/core/memory.h"
#include "runtime/dialogs/save_crypto.h"
#include "runtime/io/io.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/threads.h"
#include "runtime/boot.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/host/host.h"
#include "runtime/m33/m33.h"

namespace modulemgr {
bool IsNativeModuleName(const std::string &name);
}

namespace vsh {

namespace {

bool g_active = false;
bool g_returnToXmb = false;
std::vector<u8> g_vshmainArgs;

constexpr u32 kBtcnfMagic = 0x0F803001;
constexpr u32 kBootModeVsh = 0x0001;  // BootLoadFlags VSH (systemctrl_se.h)

u32 Rd32(const std::vector<u8> &d, size_t at) {
	if (at + 4 > d.size()) return 0;
	u32 v;
	memcpy(&v, &d[at], 4);
	return v;
}

u16 Rd16(const std::vector<u8> &d, size_t at) {
	if (at + 2 > d.size()) return 0;
	u16 v;
	memcpy(&v, &d[at], 2);
	return v;
}

std::string CStr(const std::vector<u8> &d, size_t at) {
	std::string s;
	while (at < d.size() && d[at]) s.push_back((char)d[at++]);
	return s;
}

// The module paths (flash0-relative, e.g. "/vsh/module/paf.prx") pspbtcnf lists for a boot mode.
// Layout as tools/fwinventory.py documents it: a mode table (0x20-byte entries: count, first
// module, mode mask, key) and a module table (0x20-byte entries: name offset, -, flags).
std::vector<std::string> BtcnfModules(const std::vector<u8> &d, u32 modeFlag) {
	std::vector<std::string> out;
	if (Rd32(d, 0) != kBtcnfMagic) return out;
	u32 modeStart = Rd32(d, 0x10), nModes = Rd32(d, 0x14);
	u32 modStart = Rd32(d, 0x20), nMods = Rd32(d, 0x24), nameStart = Rd32(d, 0x30);
	for (u32 i = 0; i < nModes; i++) {
		size_t e = modeStart + (size_t)i * 0x20;
		u16 count = Rd16(d, e), first = Rd16(d, e + 2);
		u32 mask = Rd32(d, e + 4);
		if (!(mask & modeFlag)) continue;
		for (u32 m = first; m < (u32)first + count && m < nMods; m++) {
			size_t me = modStart + (size_t)m * 0x20;
			if (Rd32(d, me + 8) & mask & 0xFFFF) out.push_back(CStr(d, nameStart + Rd32(d, me)));
		}
		break;
	}
	return out;
}

// What init.prx loads in VSH mode, from the firmware's boot configuration (the PSP-2000 one on a
// 64 MB console); the 4.01 list if there is none.
std::vector<std::string> VshModuleList() {
	const char *const confs[] = {"flash0:/kd/pspbtcnf_02g.bin", "flash0:/kd/pspbtcnf.bin", "flash0:/kd/pspbtcnf_01g.bin"};
	for (const char *c : confs) {
		if (mem::g_ramSize <= 0x02000000 && strstr(c, "_02g")) continue;
		std::vector<u8> d;
		if (!io::ReadFile(c, d)) continue;
		std::vector<std::string> mods = BtcnfModules(d, kBootModeVsh);
		if (!mods.empty()) {
			LOGI("VSH", "Boot configuration %s: %zu modules in VSH mode", c, mods.size());
			return mods;
		}
	}
	LOGI("VSH", "No boot configuration in flash0:/kd; using the 4.01 VSH module list");
	return {"/kd/vshbridge.prx", "/kd/usersystemlib.prx", "/vsh/module/heaparea1.prx", "/kd/libatrac3plus.prx",
	        "/vsh/module/paf.prx", "/vsh/module/common_gui.prx", "/vsh/module/common_util.prx",
	        "/vsh/module/vshmain.prx"};
}

// A module of the boot list that the native kernel provides: every kernel-mode module, and the
// user libraries PSPC implements natively (usersystemlib, libatrac3plus...).
bool ProvidedNatively(const std::string &flashPath, const std::vector<u8> &data) {
	if (flashPath.rfind("/kd/", 0) == 0) {
		loader::ElfFile elf;
		if (!elf.Parse(data.data(), data.size(), nullptr) || !elf.HasModuleInfo()) return true;
		if (elf.IsKernelModule()) return true;
		std::string name(elf.ModuleInfo().name, strnlen(elf.ModuleInfo().name, 28));
		return name == "sceKernelLibrary" || modulemgr::IsNativeModuleName(name);
	}
	return false;
}

// The init thread: loads and starts the VSH modules in order, waiting for each module_start.
void InitThread(bool recovery) {
	// M33's recovery boot list ends with recovery.prx in place of the XMB's modules (the
	// installer's pspbtcnf); it needs none of them.
	std::vector<std::string> list = recovery ? std::vector<std::string>{"/vsh/module/recovery.prx"} : VshModuleList();
	for (const std::string &rel : list) {
		std::string path = "flash0:" + rel;
		std::vector<u8> data;
		if (!io::ReadFile(path, data)) {
			// Kernel modules may be absent (only the VSH's own modules are installed).
			if (rel.rfind("/kd/", 0) != 0) LOGW("VSH", "%s is missing", path.c_str());
			continue;
		}
		if (ProvidedNatively(rel, data)) {
			LOGD("VSH", "%s is provided natively", path.c_str());
			continue;
		}
		kernel::LoadOptions opts;
		SceUID id = kernel::LoadModuleFromMemory(data, path, opts);
		if (id < 0) {
			LOGE("VSH", "Failed to load %s: %08x", path.c_str(), id);
			continue;
		}
		kernel::PspModule *m = kernel::Get<kernel::PspModule>(id);
		// argp: the module's path, as init.prx passes it - except for vshmain coming back from a
		// game, which gets the vshmain_args block LoadExec kept (0x20 or 0x400 bytes, starting
		// with its size and 0x20).
		const std::vector<u8> &va = g_vshmainArgs;
		const bool useVshArgs = rel == "/vsh/module/vshmain.prx" && (va.size() == 0x20 || va.size() == 0x400) &&
		                        Rd32(va, 0) == va.size() && Rd32(va, 4) == 0x20;
		u32 argSize = useVshArgs ? (u32)va.size() : (u32)path.size() + 1;
		u32 argp = kernel::AllocKernelMemory(AlignUp(argSize, 16), "vsh module args");
		if (useVshArgs) {
			mem::WriteBytes(argp, va.data(), argSize);
			LOGI("VSH", "vshmain gets the XMB's state back from the game (%u bytes)", argSize);
		} else {
			if (rel == "/vsh/module/vshmain.prx" && !va.empty())
				LOGW("VSH", "vshmain_args not usable: %zu bytes, %08x %08x", va.size(), Rd32(va, 0), Rd32(va, 4));
			mem::WriteBytes(argp, path.c_str(), argSize);
		}
		s32 status = 0;
		u32 statusPtr = kernel::AllocKernelMemory(16, "vsh module status");
		int r = kernel::StartModule(m, argSize, argp, statusPtr, true);
		status = (s32)GRead32(statusPtr);
		LOGI("VSH", "Started %s (%s): %08x, module_start returned %08x", m->name.c_str(), path.c_str(), r, status);
	}
	LOGI("VSH", "VSH modules started");
}

}  // namespace

bool Active() { return g_active; }
void SetActive(bool active) { g_active = active; }

bool ReturnToXmb() { return g_returnToXmb; }
void SetVshmainArgs(std::vector<u8> args) { g_vshmainArgs = std::move(args); }
// HOME in such a game: the HOME menu (dialogs/home_menu.cpp) quits back to the XMB.
void SetReturnToXmb(bool enable) { g_returnToXmb = enable; }

bool FirmwareMissing(std::string *why) {
	std::vector<u8> d;
	if (!io::ReadFile("flash0:/vsh/module/vshmain.prx", d) || d.size() < 4) {
		if (why) *why = "flash0:/vsh/module/vshmain.prx not found (run tools/pspc_install_fw.py)";
		return true;
	}
	if (memcmp(d.data(), "\x7F" "ELF", 4) != 0 && memcmp(d.data(), "~PSP", 4) != 0) {
		if (why) *why = "flash0:/vsh/module/vshmain.prx is not a PSP module";
		return true;
	}
	return false;
}

int Boot(bool recovery) {
	std::string why;
	if (FirmwareMissing(&why)) {
		LOGE("VSH", "Cannot boot the XMB: %s", why.c_str());
		return -1;
	}
	if (!recovery && (host::GetCtrl().buttons & 0x200)) {
		LOGI("VSH", "R held at boot: recovery menu");
		recovery = true;
	}
	if (recovery) {
		std::vector<u8> d;
		if (!io::ReadFile("flash0:/vsh/module/recovery.prx", d)) {
			LOGE("VSH", "Recovery menu not installed (flash0:/vsh/module/recovery.prx; tools/pspc_install_fw.py copies it)");
			return -1;
		}
	}
	// Saves without the PARAM.SFO hashes the Saved Data Utility checks would list as corrupted.
	if (!recovery) savecrypto::AddMissingSfoHashes();
	// A kernel thread whose entry just returns; the work happens on it before the entry runs.
	u32 entry = kernel::AllocKernelMemory(16, "vsh init entry");
	GWrite32(entry, 0x03E00008);      // jr ra
	GWrite32(entry + 4, 0x00001021);  // move v0, zero
	SceUID tid = kernel::CreateThread("SceKernelInitThread", entry, 0x20, 0x4000, kernel::PSP_THREAD_ATTR_KERNEL, 0, 0);
	if (tid < 0) return tid;
	kernel::PspThread *t = kernel::Get<kernel::PspThread>(tid);
	t->deleteOnReturn = true;
	t->onFirstRun = [recovery]() { InitThread(recovery); };
	return kernel::StartThread(t, 0, 0);
}

}  // namespace vsh
