// SPDX-License-Identifier: GPL-2.0-or-later
// ModuleMgrForUser and LoadExecForUser.
#include <algorithm>

#include "common/log.h"
#include "runtime/boot.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/io/io.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"

using namespace kernel;

namespace {

// Firmware libraries that need hardware we don't emulate at the register level. Loading them
// returns a placeholder module and their imports resolve to the native implementations.
const char *const kNativeModules[] = {
	"sceATRAC3plus_Library", "sceFont_Library", "SceFont_Library", "SceHttp_Library", "sceMpeg_library",
	"sceMp3_Library", "sceNetAdhocctl_Library", "sceNetAdhocDownload_Library", "sceNetAdhocMatching_Library",
	"sceNetApDialogDummy_Library", "sceNetAdhoc_Library", "sceNetApctl_Library", "sceNetInet_Library",
	"sceNetResolver_Library", "sceNet_Library", "sceNetAdhocAuth_Service", "sceNetIfhandle_Service", "sceNet_Service",
	"sceSsl_Module", "sceMemab", "sceAvcodec_driver", "sceAudiocodec_Driver", "sceAudiocodec", "sceVideocodec_Driver",
	"sceVideocodec", "sceMpegbase_Driver", "sceMpegbase", "scePsmf_library", "scePsmfP_library", "scePsmfPlayer",
	"sceSAScore", "sceMp4_library", "mp4msv_module", "sceJpeg", "sceJpeg_library", "sceJpeg_Library",
	"sceVaudio_driver", "sceUsbGps_Driver", "sceUsbCam_Driver", "sceUsbMic_Driver",
};

bool IsNativeModuleName(const std::string &name) {
	for (const char *n : kNativeModules)
		if (_stricmp(n, name.c_str()) == 0) return true;
	return false;
}

std::string ModuleNameOf(const std::vector<u8> &data) {
	loader::ElfFile elf;
	if (elf.Parse(data.data(), data.size(), nullptr) && elf.HasModuleInfo())
		return std::string(elf.ModuleInfo().name, strnlen(elf.ModuleInfo().name, 28));
	// ~PSP header carries the name too.
	if (data.size() > 0x26 && memcmp(data.data(), "~PSP", 4) == 0)
		return std::string((const char *)data.data() + 0x0A, strnlen((const char *)data.data() + 0x0A, 28));
	return "";
}

SceUID MakePlaceholderModule(const std::string &name, const std::string &path) {
	auto *m = new PspModule();
	m->name = name;
	m->path = path;
	m->status = MODULE_STATUS_LOADED;
	SceUID id = Register(m);
	LOGI("LOADER", "%s (%s) is provided natively; not loading its code", name.c_str(), path.c_str());
	return id;
}

SceUID LoadFromBytes(const std::vector<u8> &data, const std::string &path, u32 optPtr) {
	std::string name = ModuleNameOf(data);
	if (!name.empty() && IsNativeModuleName(name)) return MakePlaceholderModule(name, path);
	LoadOptions opts;
	(void)optPtr;
	return LoadModuleFromMemory(data, path, opts);
}

int sceKernelLoadModule(const char *path, u32 flags, u32 optPtr) {
	(void)flags;
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::vector<u8> data;
	if (!io::ReadFile(path, data)) {
		LOGW("LOADER", "sceKernelLoadModule(%s): file not found", path);
		return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	}
	return LoadFromBytes(data, path, optPtr);
}

int sceKernelLoadModuleNpDrm(const char *path, u32 flags, u32 optPtr) { return sceKernelLoadModule(path, flags, optPtr); }
int sceKernelLoadModuleDNAS(const char *path, u32 key, u32 flags, u32 optPtr) {
	(void)key;
	return sceKernelLoadModule(path, flags, optPtr);
}

int sceKernelLoadModuleBufferUsbWlan(u32 size, u32 buf, u32 flags, u32 optPtr) {
	(void)flags;
	if (!mem::IsValidRange(buf, size)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::vector<u8> data(GPtr(buf), GPtr(buf) + size);
	return LoadFromBytes(data, "(buffer)", optPtr);
}

int sceKernelStartModule(SceUID id, u32 argSize, u32 argPtr, u32 statusPtr, u32 optPtr) {
	(void)optPtr;
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!m->memAddr) {
		// Placeholder for a native library.
		m->status = MODULE_STATUS_STARTED;
		if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, 0);
		return id;
	}
	int r = StartModule(m, argSize, argPtr, statusPtr, true);
	if (r < 0 && r != 0 && m->status != MODULE_STATUS_STARTED) return r;
	return id;
}

int sceKernelStopModule(SceUID id, u32 argSize, u32 argPtr, u32 statusPtr, u32 optPtr) {
	(void)optPtr;
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!m->memAddr) {
		m->status = MODULE_STATUS_STOPPED;
		return 0;
	}
	return StopModule(m, argSize, argPtr, statusPtr);
}

int sceKernelUnloadModule(SceUID id) {
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!m->memAddr) {
		Destroy(id);
		return id;
	}
	int r = UnloadModule(m);
	return r < 0 ? r : id;
}

PspModule *CallerModule() {
	if (PspModule *m = ModuleByAddress(hle::CurrentCallerPc())) return m;
	if (PspThread *t = sched::Current()) return Get<PspModule>(t->moduleId);
	return nullptr;
}

int sceKernelSelfStopUnloadModule(int exitCode, u32 argSize, u32 argPtr) {
	(void)argSize;
	(void)argPtr;
	PspModule *m = CallerModule();
	if (m) m->status = MODULE_STATUS_STOPPED;
	sched::ExitCurrentThread(exitCode, true);
}

int sceKernelStopUnloadSelfModuleWithStatus(int exitCode, u32 argSize, u32 argPtr, u32 statusPtr, u32 optPtr) {
	(void)optPtr;
	if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, 0);
	return sceKernelSelfStopUnloadModule(exitCode, argSize, argPtr);
}

int sceKernelGetModuleId() {
	PspModule *m = CallerModule();
	return m ? m->uid : (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
}

int sceKernelGetModuleIdByAddress(u32 addr) {
	PspModule *m = ModuleByAddress(addr);
	return m ? m->uid : (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
}

u32 sceKernelGetModuleGPByAddress(u32 addr, u32 gpPtr) {
	PspModule *m = ModuleByAddress(addr);
	if (!m) return SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (mem::IsValidRange(gpPtr, 4)) GWrite32(gpPtr, m->gp);
	return 0;
}

int sceKernelGetModuleIdList(u32 listPtr, u32 listSize, u32 countPtr) {
	u32 n = 0;
	for (SceUID id : LoadedModules()) {
		if ((n + 1) * 4 <= listSize && mem::IsValidRange(listPtr + n * 4, 4)) GWrite32(listPtr + n * 4, (u32)id);
		n++;
	}
	if (mem::IsValidRange(countPtr, 4)) GWrite32(countPtr, n);
	return 0;
}

int sceKernelQueryModuleInfo(SceUID id, u32 infoPtr) {
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!mem::IsValidRange(infoPtr, 4)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	// SceKernelModuleInfo (0x60 bytes).
	struct {
		u32 size;
		u8 nsegment, reserved[3];
		u32 segmentaddr[4];
		u32 segmentsize[4];
		u32 entry_addr, gp_value, text_addr, text_size, data_size, bss_size;
		u16 attribute;
		u8 version[2];
		char name[28];
	} info{};
	info.size = sizeof(info);
	int seg = 0;
	for (const auto &s : m->image.segments) {
		if (s.type != loader::PT_LOAD || seg >= 4) continue;
		info.segmentaddr[seg] = s.loadAddr;
		info.segmentsize[seg] = s.memsz;
		seg++;
	}
	info.nsegment = (u8)seg;
	info.entry_addr = m->moduleStart;
	info.gp_value = m->gp;
	info.text_addr = m->memAddr;
	info.text_size = m->memSize;
	info.attribute = m->image.modInfo.attr;
	info.version[0] = m->image.modInfo.version[0];
	info.version[1] = m->image.modInfo.version[1];
	strncpy(info.name, m->name.c_str(), 27);
	u32 wanted = std::min<u32>(GRead32(infoPtr), sizeof(info));
	if (wanted) mem::WriteBytes(infoPtr, &info, wanted);
	return 0;
}

// ---- LoadExecForUser ----

SceUID g_exitCallback = 0;

void sceKernelExitGame() {
	LOGI("KERNEL", "sceKernelExitGame");
	sched::RequestStop(0);
}

void sceKernelExitGameWithStatus(int status) {
	LOGI("KERNEL", "sceKernelExitGameWithStatus(%d)", status);
	sched::RequestStop(status);
}

int sceKernelRegisterExitCallback(SceUID cb) {
	g_exitCallback = cb;
	return 0;
}

int sceKernelLoadExec(const char *path, u32 paramPtr) {
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::vector<u8> args;
	if (mem::IsValidRange(paramPtr, 16)) {
		u32 argSize = GRead32(paramPtr + 4), argp = GRead32(paramPtr + 8);
		if (argSize && mem::IsValidRange(argp, argSize)) args.assign(GPtr(argp), GPtr(argp) + argSize);
	}
	boot::RequestLoadExec(path, args);
	sched::RequestStop(boot::kExitLoadExec);
	return 0;
}

int LoadExecForUser_362A956B() {
	// Notifies the exit callback (used by some games to "quit" through the HOME menu path).
	if (g_exitCallback) NotifyCallback(g_exitCallback, 0);
	return 0;
}

}  // namespace

namespace boot {
SceUID ExitCallback() { return g_exitCallback; }
}

void Register_ModuleMgrForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x977DE386, sceKernelLoadModule),
		HLEFN(0x50F0C1EC, sceKernelStartModule),
		HLEFN(0xD675EBB8, sceKernelSelfStopUnloadModule),
		HLEFN(0xD1FF982A, sceKernelStopModule),
		HLEFN(0x2E0911AA, sceKernelUnloadModule),
		HLEFN(0xF9275D98, sceKernelLoadModuleBufferUsbWlan),
		HLEFN(0x748CBED9, sceKernelQueryModuleInfo),
		HLEFN(0xD8B73127, sceKernelGetModuleIdByAddress),
		HLEFN(0xF0A26395, sceKernelGetModuleId),
		HLEFN(0x8F2DF740, sceKernelStopUnloadSelfModuleWithStatus),
		HLEFN(0xFEF27DC1, sceKernelLoadModuleDNAS),
		HLEFN(0x644395E2, sceKernelGetModuleIdList),
		HLEFN(0xF2D8D1B4, sceKernelLoadModuleNpDrm),
		HLEFN(0xD2FBC957, sceKernelGetModuleGPByAddress),
	};
	hle::RegisterModule("ModuleMgrForUser", funcs);
	hle::RegisterModule("ModuleMgrForKernel", funcs);
}

void Register_LoadExecForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x05572A5F, sceKernelExitGame),
		HLEFN(0x4AC57943, sceKernelRegisterExitCallback),
		HLEFN(0xBD2F1094, sceKernelLoadExec),
		HLEFN(0x2AC9954B, sceKernelExitGameWithStatus),
		HLEFN(0x362A956B, LoadExecForUser_362A956B),
	};
	hle::RegisterModule("LoadExecForUser", funcs);
	hle::RegisterModule("LoadExecForKernel", funcs);
}
