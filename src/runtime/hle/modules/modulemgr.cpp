// SPDX-License-Identifier: GPL-2.0-or-later
// ModuleMgrForUser and LoadExecForUser.
#include <algorithm>

#include "common/log.h"
#include "loader/psp_header.h"
#include "runtime/boot.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/io/io.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

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
	"sceVaudio_driver", "sceUsbGps_Driver", "sceUsbCam_Driver", "sceUsbMic_Driver", "sceCert_Loader",
	"sceUSB_PSPComm_Driver",
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
	// The ~PSP header (possibly behind a ~SCE wrapper) carries the name too.
	loader::PspHeader h;
	if (loader::ParsePspHeader(data.data(), data.size(), h)) return h.modName;
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

// How long a load takes on hardware (fast-time mode only), from a memory stick: about 12ms for a
// 5KB module and 36ms for a 210KB one, 15ms for the 5KB one with a better thread delaying 200us at
// a time alongside. Reading the file is waiting (worse threads get the CPU: 10-50% of it for the
// small module, 50-90% for the large one); parsing and relocating is busy, preemptible by better
// threads (threads/scheduling/callcosts, syscallkinds).
bool CanSpendTime() {
	return sched::FastTime() && sched::Current() && !sched::InInterrupt() && sched::DispatchEnabled();
}

void LoadReadTime(size_t fileSize) {
	if (!CanSpendTime()) return;
	u64 us = 900 + (u64)fileSize * 102 / 1000;
	sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
}

void LoadWorkTime(size_t fileSize) {
	if (!CanSpendTime()) return;
	u64 us = 10700 + (u64)fileSize / 80;
	sched::BusyDelayCycles((s64)(us * (sched::kCpuHz / 1000000)));
}

// Unloading is busy too, about 400us with a better thread delaying alongside (syscallkinds).
void UnloadWorkTime() {
	if (!CanSpendTime()) return;
	sched::BusyDelayCycles((s64)(330 * (sched::kCpuHz / 1000000)));
}

int sceKernelLoadModule(const char *path, u32 flags, u32 optPtr) {
	(void)flags;
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::vector<u8> data;
	if (!io::ReadFile(path, data)) {
		LOGW("LOADER", "sceKernelLoadModule(%s): file not found", path);
		return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	}
	LoadReadTime(data.size());
	SceUID id = LoadFromBytes(data, path, optPtr);
	if (id >= 0) LoadWorkTime(data.size());
	return id;
}

int sceKernelLoadModuleNpDrm(const char *path, u32 flags, u32 optPtr) { return sceKernelLoadModule(path, flags, optPtr); }
int sceKernelLoadModuleDNAS(const char *path, u32 key, u32 flags, u32 optPtr) {
	(void)key;
	return sceKernelLoadModule(path, flags, optPtr);
}

// Loads from an open descriptor, starting at its current position (games keep modules inside
// their own archives this way).
int sceKernelLoadModuleByID(int fd, u32 flags, u32 optPtr) {
	(void)flags;
	std::vector<u8> data;
	std::string path;
	if (!io::ReadRestOfFd(fd, data, &path)) return (int)SCE_KERNEL_ERROR_BADF;
	LoadReadTime(data.size());
	SceUID id = LoadFromBytes(data, path + " (by fd)", optPtr);
	if (id >= 0) LoadWorkTime(data.size());
	return id;
}

int sceKernelLoadModuleBufferUsbWlan(u32 size, u32 buf, u32 flags, u32 optPtr) {
	(void)flags;
	if (!mem::IsValidRange(buf, size)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	std::vector<u8> data(GPtr(buf), GPtr(buf) + size);
	SceUID id = LoadFromBytes(data, "(buffer)", optPtr);
	if (id >= 0) LoadWorkTime(data.size());
	return id;
}

int sceKernelStartModule(SceUID id, u32 argSize, u32 argPtr, u32 statusPtr, u32 optPtr) {
	if (int r = ValidateSMOption(optPtr); r < 0) return r;
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!m->memAddr) {
		// Placeholder for a native library.
		m->status = MODULE_STATUS_STARTED;
		if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, 0);
		return id;
	}
	if (m->status == MODULE_STATUS_STARTED) return (int)SCE_KERNEL_ERROR_ERROR;
	int r = StartModule(m, argSize, argPtr, statusPtr, true, optPtr);
	if (r < 0) return r;
	return m->status == MODULE_STATUS_STARTED ? id : 0;
}

int sceKernelStopModule(SceUID id, u32 argSize, u32 argPtr, u32 statusPtr, u32 optPtr) {
	if (int r = ValidateSMOption(optPtr); r < 0) return r;
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!m->memAddr) {
		if (mem::IsValidRange(statusPtr, 4)) GWrite32(statusPtr, 0);
		m->status = MODULE_STATUS_STOPPED;
		return 0;
	}
	return StopModule(m, argSize, argPtr, statusPtr, optPtr);
}

int sceKernelUnloadModule(SceUID id) {
	PspModule *m = Get<PspModule>(id);
	if (!m) return (int)SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	if (!m->memAddr) {
		Destroy(id);
		return id;
	}
	int r = UnloadModule(m);
	if (r >= 0) UnloadWorkTime();
	return r < 0 ? r : id;
}

PspModule *CallerModule() {
	if (PspModule *m = ModuleByAddress(hle::CurrentCallerPc())) return m;
	if (PspThread *t = sched::Current()) return Get<PspModule>(t->moduleId);
	return nullptr;
}

// noinline: MSVC 19.44 at /O2 miscompiled the inlined HLE wrapper of this never-returning
// function (it dropped the body and evaluated a stack argument that doesn't exist).
__declspec(noinline) int sceKernelSelfStopUnloadModule(int exitCode, u32 argSize, u32 argPtr) {
	(void)argSize;
	(void)argPtr;
	PspModule *m = CallerModule();
	if (m) m->status = MODULE_STATUS_STOPPED;
	sched::ExitCurrentThread(exitCode, true);
	return 0;  // not reached
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
	if (!Get<Callback>(cb)) {
		if (kernel::CompiledSdkVersion() >= 0x3090500) return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
		return 0;
	}
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

// Updates the exit callback's parameter area (behaviour from PPSSPP, which matches
// threads/callbacks/exit): the callback's common argument points just past {unknown1, params}.
int LoadExecForUser_362A956B() {
	Callback *cb = Get<Callback>(g_exitCallback);
	if (!cb) return (int)SCE_KERNEL_ERROR_UNKNOWN_CBID;
	u32 cbArg = cb->commonArg;
	if (!mem::IsValidRange(cbArg - 8, 8)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	u32 unknown1 = GRead32(cbArg - 8);
	if (unknown1 >= 4) return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	u32 params = GRead32(cbArg - 4);
	if ((params & 3) || !mem::IsValidRange(params, 12)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	u32 size = GRead32(params);
	if (size < 12) return (int)SCE_KERNEL_ERROR_ILLEGAL_SIZE;
	GWrite32(params + 4, 0);
	GWrite32(params + 8, 0xFFFFFFFF);
	return 0;
}

}  // namespace

namespace boot {
SceUID ExitCallback() { return g_exitCallback; }
}

void Register_ModuleMgrForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x977DE386, sceKernelLoadModule),
		HLEFN(0xB7F46618, sceKernelLoadModuleByID),
		KFN(0x50F0C1EC, sceKernelStartModule, kernel::KF_NOT_IN_INTERRUPT | kernel::KF_NOT_DISPATCH_SUSPENDED),
		HLEFN(0xD675EBB8, sceKernelSelfStopUnloadModule),
		KFN(0xD1FF982A, sceKernelStopModule, kernel::KF_NOT_IN_INTERRUPT | kernel::KF_NOT_DISPATCH_SUSPENDED),
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
