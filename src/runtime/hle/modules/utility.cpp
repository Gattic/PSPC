// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceUtility: system parameters, utility module loading, and the utility dialogs (savedata, message,
// on-screen keyboard, network configuration, ...; see src/runtime/dialogs/).
//
// Behaviour (status machines, which calls fail with WRONG_TYPE/INVALID_STATUS, the dialog threads
// and their timing) ported from PPSSPP (Core/HLE/sceUtility.cpp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later, which encodes pspautotests results recorded on hardware.
#include <windows.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/dialogs/msg_dialog.h"
#include "runtime/dialogs/osk_dialog.h"
#include "runtime/dialogs/save_dialog.h"
#include "runtime/dialogs/simple_dialogs.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/threads.h"

namespace systemparam {
int Language();
int ButtonMode();
}  // namespace systemparam

using namespace kernel;
using utility::Dialog;
using utility::DialogType;

namespace {

constexpr const char *LIB = "sceUtility";

// Runtime-internal NIDs: the dialog thread's code calls this.
constexpr u32 NID_DIALOG_THREAD = 0xC0DE0010;

// Calls another library's HLE function with a scratch context (only for functions that don't
// block or use the thread context).
u32 CallHle(const char *library, u32 nid, std::initializer_list<u32> args) {
	const hle::HleFunction *f = hle::FindFunction(library, nid);
	if (!f || !f->fn) return SCE_KERNEL_ERROR_ERROR;
	auto c = std::make_unique<Ctx>();
	memset(c.get(), 0, sizeof(Ctx));
	int i = 0;
	for (u32 a : args) c->r[4 + i++] = a;
	f->fn(c.get());
	return c->r[2];
}

// ---------------------------------------------------------------- dialogs

utility::SaveDialog *g_saveDialog;
utility::MsgDialog *g_msgDialog;
utility::OskDialog *g_oskDialog;
utility::NetconfDialog *g_netDialog;
utility::ScreenshotDialog *g_screenshotDialog;
utility::GamedataInstallDialog *g_gamedataInstallDialog;
utility::NpSigninDialog *g_npSigninDialog;
utility::PlaceholderDialog *g_gameSharingDialog;
utility::PlaceholderDialog *g_netplayDialog;
utility::PlaceholderDialog *g_ps3ScanDialog;
utility::PlaceholderDialog *g_storeCheckoutDialog;
// Not one of the dialogs above: it has its own state on a PSP, and doesn't count as current.
utility::HtmlViewerDialog *g_htmlViewerDialog;

// Only a single dialog at a time.
DialogType g_currentDialogType = DialogType::NONE;
bool g_currentDialogActive = false;

void CreateDialogs() {
	if (g_saveDialog) return;
	g_saveDialog = new utility::SaveDialog();
	g_msgDialog = new utility::MsgDialog();
	g_oskDialog = new utility::OskDialog();
	g_netDialog = new utility::NetconfDialog();
	g_screenshotDialog = new utility::ScreenshotDialog();
	g_gamedataInstallDialog = new utility::GamedataInstallDialog();
	g_npSigninDialog = new utility::NpSigninDialog();
	g_gameSharingDialog = new utility::PlaceholderDialog(DialogType::GAMESHARING);
	g_netplayDialog = new utility::PlaceholderDialog(DialogType::NETPLAY);
	g_ps3ScanDialog = new utility::PlaceholderDialog(DialogType::PS3SCAN);
	g_storeCheckoutDialog = new utility::PlaceholderDialog(DialogType::STORECHECKOUT);
	g_htmlViewerDialog = new utility::HtmlViewerDialog();
}

Dialog *DialogFor(DialogType type) {
	CreateDialogs();
	switch (type) {
	case DialogType::SAVEDATA: return g_saveDialog;
	case DialogType::MSG: return g_msgDialog;
	case DialogType::OSK: return g_oskDialog;
	case DialogType::NET: return g_netDialog;
	case DialogType::SCREENSHOT: return g_screenshotDialog;
	case DialogType::GAMESHARING: return g_gameSharingDialog;
	case DialogType::GAMEDATAINSTALL: return g_gamedataInstallDialog;
	case DialogType::NPSIGNIN: return g_npSigninDialog;
	case DialogType::HTMLVIEWER: return g_htmlViewerDialog;
	case DialogType::NETPLAY: return g_netplayDialog;
	case DialogType::PS3SCAN: return g_ps3ScanDialog;
	case DialogType::STORECHECKOUT: return g_storeCheckoutDialog;
	default: return nullptr;
	}
}

// ---------------------------------------------------------------- the dialog thread
//
// On a PSP, dialog init and shutdown happen partly at the accessThread priority and partly at the
// graphicsThread priority, one phase after the other. One helper thread ("ScePafJob") that switches
// priority per phase reproduces who gets to run when (utility/dialog/priority,
// utility/savedata/shutdownstatus, utility/msgdialog/workshare).

struct ThreadJob {
	bool shutdown = false;
	DialogType type = DialogType::NONE;
	int partDelayUs = 0;
	int startPriority = 0x20;
	int dialogPriority = 0x20;
	int accessPriority = 0x20;
	int finalPriority = 0x20;
};

ThreadJob g_job;             // the one the current helper runs
ThreadJob g_pendingJob;      // started when the current HLE call returns
bool g_jobPending = false;
SceUID g_helperThread = 0;
bool g_helperFinished = true;
u32 g_helperCode = 0;

bool ValidThreadPriority(int p) { return p >= 0x08 && p <= 0x77; }
int PhasePriority(int p, int fallback) { return ValidThreadPriority(p) ? p : fallback; }
// It becomes the helper's priority, so it has to be at least plausible.
int HelperPriority(int p) { return p >= 0 && p <= 0x7F ? p : 0x20; }

void DeleteHelper(PspThread *t) {
	DeleteThreadCallbacks(t);
	FreeThreadStack(t);
	sched::DeleteThread(t);
}

void CleanupDialogThreads(bool force = false) {
	if (!g_helperThread) return;
	PspThread *t = Get<PspThread>(g_helperThread);
	if (!t) {
		g_helperThread = 0;
		return;
	}
	if (t->IsDormant()) {
		DeleteHelper(t);
		g_helperThread = 0;
	} else if (force) {
		LOGW("UTIL", "Utility dialog thread still running, terminating it");
		sched::TerminateThread(t, (s32)SCE_KERNEL_ERROR_THREAD_TERMINATED);
		DeleteHelper(t);
		g_helperThread = 0;
		if (!g_job.shutdown && !g_helperFinished) {
			// An init thread may hold the lock before it tells its dialog.
			CallHle("sceSuspendForUser", 0xA569E425, {0});
		}
		g_helperFinished = true;
	}
}

// Busy for `us`: time passes without worse threads getting the CPU.
void EatUs(u64 us) {
	if (sched::FastTime()) {
		sched::BusyDelayCycles((s64)us * (s64)(sched::kCpuHz / 1000000));
	} else {
		u64 end = sched::NowUs() + us;
		while (sched::NowUs() < end) YieldProcessor();
	}
}

void WorkUs(int us) {
	// Blocks, but better threads get time meanwhile.
	if (us > 1000) {
		EatUs(1000);
		sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + (u64)(us - 1000), false);
	} else {
		EatUs((u64)std::max(us, 0));
		sched::Reschedule();
	}
}

// sceKernelChangeThreadPriority(0, prio) from the helper.
void SetOwnPriority(int prio) {
	PspThread *t = sched::Current();
	if (!t) return;
	if (prio == 0) prio = t->currentPriority;
	if (!ValidThreadPriority(prio)) return;
	sched::ChangePriority(t, prio);
	sched::Reschedule();
	sched::YieldToSamePriority();
}

// sceKernelVolatileMemLock: blocks while someone else holds it.
bool LockVolatileBlocking() {
	for (;;) {
		u32 r = CallHle("sceSuspendForUser", 0x3E0271D3, {0, 0, 0});
		if (r == 0) return true;
		if (r != SCE_KERNEL_ERROR_POWER_VMEM_IN_USE) {
			LOGW("UTIL", "Dialog thread: volatile memory lock failed (%08x)", r);
			return false;
		}
		sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + 1000, false);
	}
}

void DialogThreadMain(Ctx *c) {
	const ThreadJob job = g_job;
	const SceUID self = sched::CurrentId();
	int result = 0;
	if (!job.shutdown) {
		LOGD("UTIL", "dialog thread: init %d, part %d us", (int)job.type, job.partDelayUs);
		bool locked = LockVolatileBlocking();
		// Loading, at accessThread priority.
		for (int i = 0; i < 3; i++) WorkUs(job.partDelayUs);
		// Setting up the dialog, at graphicsThread priority; then RUNNING.
		SetOwnPriority(job.dialogPriority);
		WorkUs(job.partDelayUs);
		if (g_helperThread == self) g_helperFinished = true;
		Dialog *d = DialogFor(job.type);
		result = d ? d->FinishInit(locked) : 0;
	} else {
		// Tearing down the dialog, at graphicsThread priority.
		SetOwnPriority(job.dialogPriority);
		WorkUs(job.partDelayUs);
		WorkUs(job.partDelayUs);
		// Cleaning up at accessThread priority.
		SetOwnPriority(job.accessPriority);
		WorkUs(job.partDelayUs);
		WorkUs(job.partDelayUs);
		// Then NONE, from finalPriority.
		SetOwnPriority(job.finalPriority);
		if (g_helperThread == self) g_helperFinished = true;
		Dialog *d = DialogFor(job.type);
		result = d ? d->FinishShutdown() : 0;
	}
	c->r[2] = (u32)result;
}

void QueueDialogThread(const ThreadJob &job) {
	CleanupDialogThreads(true);
	g_pendingJob = job;
	g_jobPending = true;
}

// Starts a queued dialog thread. Called at the end of each sceUtility call, so it starts the way a
// thread created in the call would on a PSP: a better-priority one runs as the call returns.
void FlushDialogThread() {
	if (!g_jobPending) return;
	g_jobPending = false;
	if (!g_helperCode) {
		g_helperCode = AllocKernelMemory(8, "UtilityDialogThread");
		hle::PatchImportStub(g_helperCode, LIB, NID_DIALOG_THREAD);
	}
	PspThread *cur = sched::Current();
	SceUID id = kernel::CreateThread("ScePafJob", g_helperCode, 0x20, 0x200, PSP_THREAD_ATTR_KERNEL, cur ? cur->ctx.r[28] : 0,
	                         cur ? cur->moduleId : 0);
	PspThread *t = Get<PspThread>(id);
	if (!t) {
		LOGE("UTIL", "Can't create the utility dialog thread (%08x)", id);
		Dialog *d = DialogFor(g_pendingJob.type);
		if (d && !g_pendingJob.shutdown) d->FinishInit(false);
		if (d && g_pendingJob.shutdown) d->FinishShutdown();
		return;
	}
	t->initPriority = t->currentPriority = g_pendingJob.startPriority;
	g_job = g_pendingJob;
	g_helperThread = id;
	g_helperFinished = false;
	kernel::StartThread(t, 0, 0);
}

template <typename T>
T Done(T result) {
	FlushDialogThread();
	return result;
}

void ActivateDialog(DialogType type) {
	CleanupDialogThreads();
	if (!g_currentDialogActive) {
		g_currentDialogType = type;
		g_currentDialogActive = true;
	}
}

void DeactivateDialog() {
	CleanupDialogThreads();
	g_currentDialogActive = false;
}

bool UtilityDialogBusy() {
	Dialog *d = DialogFor(g_currentDialogType);
	return d && d->IsBusy();
}

// A PSP runs one dialog at a time whatever its type: until the last one started is back at NONE,
// every InitStart fails with INVALID_STATUS before its params are looked at. A failed InitStart
// leaves the current type alone (utility/dialog/status).
template <typename D>
int UtilityInitStart(DialogType type, D *dialog, u32 paramAddr) {
	if (UtilityDialogBusy()) return (int)SCE_ERROR_UTILITY_INVALID_STATUS;
	Dialog *current = DialogFor(g_currentDialogType);
	// One that's only waiting for its SHUTDOWN to be seen gives volatile memory up either way.
	if (current) current->FinishVolatile();
	dialog->FinishAutoShutdown();
	const bool wasBusy = dialog->IsBusy();
	int result = dialog->Init(paramAddr);
	// Some fail after starting to shut down, and the game may still poll that one's status.
	if (result >= 0 || (!wasBusy && dialog->IsBusy())) {
		if (current && current != dialog) current->FinishAutoShutdown();
		g_currentDialogActive = false;
		ActivateDialog(type);
	}
	return result;
}

int GetDialogStatus(DialogType type, Dialog *d) {
	if (g_currentDialogType != type) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	int status = d->GetStatus();
	CleanupDialogThreads();
	return status;
}

int ShutdownDialog(DialogType type, Dialog *d) {
	if (g_currentDialogType != type) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	DeactivateDialog();
	return Done(d->Shutdown());
}

int UpdateDialog(DialogType type, Dialog *d, int animSpeed) {
	if (g_currentDialogType != type) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	return Done(d->Update(animSpeed));
}

void DelayResult(u64 us) {
	if (sched::Current()) sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
}

// ---- savedata

int sceUtilitySavedataInitStart(u32 paramAddr) {
	return Done(UtilityInitStart(DialogType::SAVEDATA, g_saveDialog, paramAddr));
}
int sceUtilitySavedataShutdownStart() {
	if (g_currentDialogType != DialogType::SAVEDATA) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	DeactivateDialog();
	int ret = g_saveDialog->Shutdown();
	sched::ChargeCycles(30000);
	return Done(ret);
}
int sceUtilitySavedataGetStatus() {
	sched::ChargeCycles(200);
	return GetDialogStatus(DialogType::SAVEDATA, g_saveDialog);
}
int sceUtilitySavedataUpdate(int animSpeed) {
	if (g_currentDialogType != DialogType::SAVEDATA) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	int result = Done(g_saveDialog->Update(animSpeed));
	if (result >= 0) DelayResult(300);
	return result;
}

// ---- message dialog

int sceUtilityMsgDialogInitStart(u32 paramAddr) { return Done(UtilityInitStart(DialogType::MSG, g_msgDialog, paramAddr)); }
int sceUtilityMsgDialogShutdownStart() { return ShutdownDialog(DialogType::MSG, g_msgDialog); }
int sceUtilityMsgDialogUpdate(int animSpeed) {
	if (g_currentDialogType != DialogType::MSG) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	int ret = Done(g_msgDialog->Update(animSpeed));
	if (ret >= 0) DelayResult(800);
	return ret;
}
int sceUtilityMsgDialogGetStatus() { return GetDialogStatus(DialogType::MSG, g_msgDialog); }
int sceUtilityMsgDialogAbort() {
	if (g_currentDialogType != DialogType::MSG) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	return g_msgDialog->Abort();
}

// ---- on-screen keyboard

int sceUtilityOskInitStart(u32 paramAddr) { return Done(UtilityInitStart(DialogType::OSK, g_oskDialog, paramAddr)); }
int sceUtilityOskShutdownStart() { return ShutdownDialog(DialogType::OSK, g_oskDialog); }
int sceUtilityOskUpdate(int animSpeed) {
	if (g_currentDialogType != DialogType::OSK) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	// The vblank period plus a little (Ghost Recon: Predator, PPSSPP issue 12044).
	sched::ChargeUs(832);
	return Done(g_oskDialog->Update(animSpeed));
}
int sceUtilityOskGetStatus() { return GetDialogStatus(DialogType::OSK, g_oskDialog); }

// ---- network configuration (offline)

int sceUtilityNetconfInitStart(u32 paramAddr) { return Done(UtilityInitStart(DialogType::NET, g_netDialog, paramAddr)); }
int sceUtilityNetconfShutdownStart() { return ShutdownDialog(DialogType::NET, g_netDialog); }
int sceUtilityNetconfUpdate(int animSpeed) { return UpdateDialog(DialogType::NET, g_netDialog, animSpeed); }
int sceUtilityNetconfGetStatus() { return GetDialogStatus(DialogType::NET, g_netDialog); }

// ---- screenshot

int sceUtilityScreenshotInitStart(u32 paramAddr) {
	return Done(UtilityInitStart(DialogType::SCREENSHOT, g_screenshotDialog, paramAddr));
}
int sceUtilityScreenshotShutdownStart() { return ShutdownDialog(DialogType::SCREENSHOT, g_screenshotDialog); }
int sceUtilityScreenshotUpdate(int animSpeed) { return UpdateDialog(DialogType::SCREENSHOT, g_screenshotDialog, animSpeed); }
int sceUtilityScreenshotGetStatus() { return GetDialogStatus(DialogType::SCREENSHOT, g_screenshotDialog); }
int sceUtilityScreenshotContStart(u32) {
	if (g_currentDialogType != DialogType::SCREENSHOT) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	return g_screenshotDialog->ContStart();
}

// ---- game data install

int sceUtilityGamedataInstallInitStart(u32 paramAddr) {
	return Done(UtilityInitStart(DialogType::GAMEDATAINSTALL, g_gamedataInstallDialog, paramAddr));
}
int sceUtilityGamedataInstallShutdownStart() { return ShutdownDialog(DialogType::GAMEDATAINSTALL, g_gamedataInstallDialog); }
int sceUtilityGamedataInstallUpdate(int animSpeed) {
	return UpdateDialog(DialogType::GAMEDATAINSTALL, g_gamedataInstallDialog, animSpeed);
}
int sceUtilityGamedataInstallGetStatus() {
	if (g_currentDialogType != DialogType::GAMEDATAINSTALL) {
		sched::ChargeCycles(200);
		return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	}
	int status = g_gamedataInstallDialog->GetStatus();
	CleanupDialogThreads();
	return status;
}
int sceUtilityGamedataInstallAbort() {
	if (g_currentDialogType != DialogType::GAMEDATAINSTALL) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	DeactivateDialog();
	return Done(g_gamedataInstallDialog->Abort());
}

// ---- NP sign-in (offline: cancelled)

int sceUtilityNpSigninInitStart(u32 paramAddr) {
	return Done(UtilityInitStart(DialogType::NPSIGNIN, g_npSigninDialog, paramAddr));
}
int sceUtilityNpSigninShutdownStart() { return ShutdownDialog(DialogType::NPSIGNIN, g_npSigninDialog); }
int sceUtilityNpSigninUpdate(int animSpeed) { return UpdateDialog(DialogType::NPSIGNIN, g_npSigninDialog, animSpeed); }
int sceUtilityNpSigninGetStatus() { return GetDialogStatus(DialogType::NPSIGNIN, g_npSigninDialog); }

// ---- placeholders: game sharing, netplay, PS3 scan, store checkout

#define PLACEHOLDER_DIALOG(Name, TYPE, dialog)                                                                   \
	int sceUtility##Name##InitStart(u32 paramAddr) { return Done(UtilityInitStart(DialogType::TYPE, dialog, paramAddr)); } \
	int sceUtility##Name##ShutdownStart() { return ShutdownDialog(DialogType::TYPE, dialog); }                  \
	int sceUtility##Name##Update(int animSpeed) { return UpdateDialog(DialogType::TYPE, dialog, animSpeed); }    \
	int sceUtility##Name##GetStatus() { return GetDialogStatus(DialogType::TYPE, dialog); }

PLACEHOLDER_DIALOG(GameSharing, GAMESHARING, g_gameSharingDialog)
PLACEHOLDER_DIALOG(Netplay, NETPLAY, g_netplayDialog)
PLACEHOLDER_DIALOG(PS3Scan, PS3SCAN, g_ps3ScanDialog)
PLACEHOLDER_DIALOG(StoreCheckout, STORECHECKOUT, g_storeCheckoutDialog)

// ---- HTML viewer: its own state; until one has started its calls return WRONG_TYPE
// (utility/dialog/htmlviewer).

int sceUtilityHtmlViewerInitStart(u32 paramAddr) { return Done(g_htmlViewerDialog->Init(paramAddr)); }
int sceUtilityHtmlViewerShutdownStart() {
	if (!g_htmlViewerDialog->HasStarted()) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	return Done(g_htmlViewerDialog->Shutdown());
}
int sceUtilityHtmlViewerUpdate(int animSpeed) {
	if (!g_htmlViewerDialog->HasStarted()) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	return Done(g_htmlViewerDialog->Update(animSpeed));
}
int sceUtilityHtmlViewerGetStatus() {
	if (!g_htmlViewerDialog->HasStarted()) return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
	return g_htmlViewerDialog->GetStatus();
}

// ---------------------------------------------------------------- utility modules

struct ModuleLoadInfo {
	int mod;
	u32 size;
	const char *name;
	std::initializer_list<int> deps;
};

const ModuleLoadInfo kModules[] = {
	{0x100, 0x00014000, "net_common", {}},
	{0x101, 0x00020000, "net_adhoc", {}},
	{0x102, 0x00058000, "net_inet", {}},
	{0x103, 0x00006000, "net_parse_uri", {}},
	{0x104, 0x00002000, "net_parse_http", {}},
	{0x105, 0x00028000, "net_http", {0x0102, 0x0103, 0x0104}},
	{0x106, 0x00044000, "net_ssl", {0x0102}},
	{0x107, 0x00010000, "unk_0x107", {}},
	{0x108, 0x00008000, "usb_pspcm", {0x0100, 0x0102, 0x0103, 0x0104, 0x0105}},
	{0x200, 0x00000000, "usb_mic", {}},
	{0x201, 0x00000000, "usb_cam", {}},
	{0x202, 0x00000000, "usb_gps", {}},
	{0x203, 0x00000000, "usb_unk_0x203", {}},
	{0x2ff, 0x00000000, "unk_0x2ff", {}},
	{0x300, 0x00000000, "av_avcodec", {}},
	{0x301, 0x00000000, "av_sascore", {}},
	{0x302, 0x00008000, "av_atrac3plus", {0x0300}},
	{0x303, 0x0000c000, "av_mpegbase", {0x0300}},
	{0x304, 0x00004000, "av_mp3", {}},
	{0x305, 0x0000a300, "av_vaudio", {}},
	{0x306, 0x00004000, "av_aac", {}},
	{0x307, 0x00000000, "av_g729", {}},
	{0x308, 0x0003c000, "av_mp4", {0x0300}},
	{0x3fe, 0x00000000, "me_stuff", {}},
	{0x3ff, 0x00000000, "me_core", {}},
	{0x400, 0x0000c000, "np_common", {}},
	{0x401, 0x00018000, "np_service", {}},
	{0x402, 0x00048000, "np_matching2", {}},
	{0x403, 0x0000e000, "np_unk_0x403", {}},
	{0x500, 0x00000000, "np_drm", {}},
	{0x600, 0x00000000, "irda", {}},
	{0x601, 0x00000000, "unk_0x601", {}},
};

// Loaded utility module -> the user memory block standing in for its code (0 = none).
std::map<int, u32> g_loadedModules;

const ModuleLoadInfo *ModuleInfo(int module) {
	for (const auto &m : kModules)
		if (m.mod == module) return &m;
	return nullptr;
}

// The codecs, network and USB libraries are native, so loading only does the bookkeeping (and
// takes the memory the real module would).
int LoadModuleInternal(u32 module, bool av) {
	const ModuleLoadInfo *info = ModuleInfo((int)module);
	if (!info) return (int)(av ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID);
	if (g_loadedModules.count((int)module))
		return (int)(av ? SCE_ERROR_AV_MODULE_ALREADY_LOADED : SCE_ERROR_MODULE_ALREADY_LOADED);
	// Some games (Kamen Rider Climax Heroes OOO) need an error while the dependencies aren't loaded.
	for (int dep : info->deps)
		if (!g_loadedModules.count(dep)) return (int)SCE_KERNEL_ERROR_LIBRARY_NOTFOUND;
	u32 address = 0;
	if (info->size) {
		char name[64];
		snprintf(name, sizeof(name), "UtilityModule/%3x_%s", (int)module, info->name);
		address = g_userMem.Alloc(info->size, false, name);
		if (!address) LOGW("UTIL", "No room for utility module %03x (%08x bytes); loading it without", module, info->size);
	}
	g_loadedModules[(int)module] = address;
	LOGI("UTIL", "Loaded utility module %03x (%s)", module, info->name);
	return 0;
}

int UnloadModuleInternal(u32 module, bool av) {
	const ModuleLoadInfo *info = ModuleInfo((int)module);
	if (!info) return (int)(av ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID);
	auto it = g_loadedModules.find((int)module);
	if (it == g_loadedModules.end()) return (int)(av ? SCE_ERROR_AV_MODULE_NOT_LOADED : SCE_ERROR_MODULE_NOT_LOADED);
	if (it->second) g_userMem.Free(it->second);
	g_loadedModules.erase(it);
	return 0;
}

// Loading or unloading takes time; the caller blocks meanwhile (and lower threads get to run).
u32 sceUtilityLoadModule(u32 module) {
	int result = LoadModuleInternal(module, false);
	DelayResult(module == 0x3FF ? 130 : 25000);
	return (u32)result;
}
u32 sceUtilityUnloadModule(u32 module) {
	int result = UnloadModuleInternal(module, false);
	DelayResult(module == 0x3FF ? 110 : 400);
	return (u32)result;
}
u32 sceUtilityLoadAvModule(u32 module) {
	if (module > 7) return SCE_ERROR_AV_MODULE_BAD_ID;
	int result = LoadModuleInternal(0x300 | module, true);
	DelayResult(25000);
	return (u32)result;
}
u32 sceUtilityUnloadAvModule(u32 module) {
	if (module > 7) return SCE_ERROR_AV_MODULE_BAD_ID;
	int result = UnloadModuleInternal(0x300 | module, true);
	DelayResult(800);
	return (u32)result;
}
// The older net module API (PSP_NET_MODULE_*): always succeeds, the libraries are native.
u32 sceUtilityLoadNetModule(u32 module) {
	LOGI("UTIL", "sceUtilityLoadNetModule(%u)", module);
	DelayResult(25000);
	return 0;
}
u32 sceUtilityUnloadNetModule(u32 module) {
	(void)module;
	DelayResult(400);
	return 0;
}
u32 sceUtilityLoadUsbModule(u32 module) {
	if (module < 1 || module > 5) LOGW("UTIL", "sceUtilityLoadUsbModule: invalid module %u", module);
	return 0;
}
u32 sceUtilityUnloadUsbModule(u32 module) {
	(void)module;
	return 0;
}

// ---------------------------------------------------------------- system parameters

enum : u32 {
	PSP_SYSTEMPARAM_ID_STRING_NICKNAME = 1,
	PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL = 2,
	PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE = 3,
	PSP_SYSTEMPARAM_ID_INT_DATE_FORMAT = 4,
	PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT = 5,
	PSP_SYSTEMPARAM_ID_INT_TIMEZONE = 6,
	PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS = 7,
	PSP_SYSTEMPARAM_ID_INT_LANGUAGE = 8,
	PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE = 9,
	PSP_SYSTEMPARAM_ID_INT_LOCK_PARENTAL_LEVEL = 10,
};

struct SystemSettings {
	std::string nickname;
	int adhocChannel = 0;  // automatic
	int wlanPowerSave = 0;
	int dateFormat = 0;    // 0 YYYYMMDD, 1 MMDDYYYY, 2 DDMMYYYY
	int timeFormat = 0;    // 0 24h, 1 12h
	int timezone = 0;      // minutes from UTC
	int daylightSavings = 0;
	int parentalLevel = 9;
};

// Without a window (tests, batch runs) report the settings of the console pspautotests' results were
// recorded on, like PPSSPP's headless build does; with one, PSPC's own and the host's time zone.
const SystemSettings &Settings() {
	static SystemSettings s;
	static bool init = false;
	if (!init) {
		init = true;
		if (host::IsHeadless()) {
			s.nickname = "shadow";
			s.dateFormat = 2;
			s.timezone = 60;
		} else {
			s.nickname = "PSPC";
			TIME_ZONE_INFORMATION tzi{};
			DWORD r = GetTimeZoneInformation(&tzi);
			s.timezone = -(int)tzi.Bias;
			s.daylightSavings = r == TIME_ZONE_ID_DAYLIGHT ? 1 : 0;
		}
	}
	return s;
}

u32 sceUtilityGetSystemParamInt(u32 id, u32 destAddr) {
	const SystemSettings &s = Settings();
	u32 param = 0;
	switch (id) {
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL: param = (u32)s.adhocChannel; break;
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE: param = (u32)s.wlanPowerSave; break;
	case PSP_SYSTEMPARAM_ID_INT_DATE_FORMAT: param = (u32)s.dateFormat; break;
	case PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT: param = (u32)s.timeFormat; break;
	case PSP_SYSTEMPARAM_ID_INT_TIMEZONE: param = (u32)s.timezone; break;
	case PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS: param = (u32)s.daylightSavings; break;
	case PSP_SYSTEMPARAM_ID_INT_LANGUAGE: param = (u32)systemparam::Language(); break;
	case PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE: param = (u32)systemparam::ButtonMode(); break;
	case PSP_SYSTEMPARAM_ID_INT_LOCK_PARENTAL_LEVEL: param = (u32)s.parentalLevel; break;
	default: return SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID;
	}
	if (mem::IsValidRange(destAddr, 4)) GWrite32(destAddr, param);
	return 0;
}

u32 sceUtilityGetSystemParamString(u32 id, u32 destAddr, int destSize) {
	// A size that isn't positive can't hold the string, and that's what the PSP reports.
	if (destSize > 0 && !mem::IsValidRange(destAddr, (u32)destSize)) return 0xFFFFFFFF;
	switch (id) {
	case PSP_SYSTEMPARAM_ID_STRING_NICKNAME: {
		const std::string &nick = Settings().nickname;
		// The string and its terminator must fit.
		if (destSize <= (int)nick.size()) return SCE_ERROR_UTILITY_STRING_TOO_LONG;
		char *buf = (char *)GPtr(destAddr);
		strncpy(buf, nick.c_str(), (size_t)destSize);
		return 0;
	}
	default: return SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID;
	}
}

u32 sceUtilitySetSystemParamInt(u32 id, u32 value) {
	switch (id) {
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL:
		if (value != 0 && value != 1 && value != 6 && value != 11) return SCE_ERROR_UTILITY_INVALID_ADHOC_CHANNEL;
		return 0;
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE: return 0;
	default: return SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID;
	}
}

u32 sceUtilitySetSystemParamString(u32 id, u32 strAddr) {
	LOGW("UTIL", "sceUtilitySetSystemParamString(%u, %08x) ignored", id, strAddr);
	return 0;
}

// ---------------------------------------------------------------- network configurations
// One stored configuration ("PSPC 1") describing an access point we never reach.

constexpr int PSP_NETPARAM_MAX_NUMBER_DUMMY_ENTRIES = 1;
int g_netParamLatestId = 1;

int sceUtilityCheckNetParam(int id) {
	bool available = id >= 0 && id <= 24 && id <= PSP_NETPARAM_MAX_NUMBER_DUMMY_ENTRIES;
	return available ? 0 : (int)SCE_ERROR_NETPARAM_BAD_NETCONF;
}

int sceUtilityGetNetParam(int id, int param, u32 dataAddr) {
	if (id < 0 || id > 24) return (int)SCE_ERROR_NETPARAM_BAD_NETCONF;
	auto putStr = [&](const char *s, u32 len) -> int {
		if (!mem::IsValidRange(dataAddr, len)) return -1;
		mem::Memset(dataAddr, 0, len);
		mem::WriteBytes(dataAddr, s, (u32)std::min<size_t>(strlen(s), len - 1));
		return 0;
	};
	auto putU32 = [&](u32 v) -> int {
		if (!mem::IsValidRange(dataAddr, 4)) return -1;
		GWrite32(dataAddr, v);
		return 0;
	};
	char name[64];
	snprintf(name, sizeof(name), "PSPC %d", id == 0 ? g_netParamLatestId : id);
	switch (param) {
	case 0: return putStr(name, 64);                 // NAME
	case 1: return putStr("PSPC", 33);               // SSID
	case 2: return putU32(0);                        // SECURE: none
	case 3: return putStr("", 5);                    // WEPKEY
	case 4: return putU32(0);                        // IS_STATIC_IP: DHCP
	case 5: return putStr("0.0.0.0", 16);            // IP
	case 6: return putStr("255.255.255.0", 16);      // NETMASK
	case 7: return putStr("0.0.0.0", 16);            // ROUTE
	case 8: return putU32(0);                        // MANUAL_DNS
	case 9: return putStr("0.0.0.0", 16);            // PRIMARYDNS
	case 10: return putStr("0.0.0.0", 16);           // SECONDARYDNS
	case 11: return putStr("", 128);                 // PROXY_USER
	case 12: return putStr("", 128);                 // PROXY_PASS
	case 13: return putU32(0);                       // USE_PROXY
	case 14: return putStr("", 128);                 // PROXY_SERVER
	case 15:                                         // PROXY_PORT
		if (!mem::IsValidRange(dataAddr, 2)) return -1;
		GWrite16(dataAddr, 0);
		return 0;
	case 16: return putU32(2);                       // VERSION: new
	case 17: return putU32(0);                       // UNKNOWN
	case 18: return putU32(0);                       // 8021X_AUTH_TYPE: none
	case 19: return putStr("", 128);                 // 8021X_USER
	case 20: return putStr("", 128);                 // 8021X_PASS
	case 21: return putU32(0);                       // WPA_TYPE
	case 22: return putStr("", 64);                  // WPA_KEY
	case 23: return putU32(0);                       // BROWSER
	case 24: return putU32(0);                       // WIFI_CONFIG
	default: return (int)SCE_ERROR_NETPARAM_BAD_PARAM;
	}
}

int sceUtilityGetNetParamLatestID(u32 idAddr) {
	if (mem::IsValidRange(idAddr, 4)) GWrite32(idAddr, (u32)g_netParamLatestId);
	return 0;
}

// ---------------------------------------------------------------- other dialogs nobody uses

int sceUtilityUnsupportedDialog() {
	LOGW("UTIL", "%s: not supported", hle::CurrentFunctionName());
	return (int)SCE_ERROR_UTILITY_WRONG_TYPE;
}
void sceUtilityInstallInitStart(u32) { LOGW("UTIL", "sceUtilityInstallInitStart: not supported"); }

}  // namespace

namespace utility {

void DialogThreadInit(DialogType type, int delayUs, int accessPriority, int graphicsPriority) {
	ThreadJob job;
	job.shutdown = false;
	job.type = type;
	job.partDelayUs = delayUs / 4;
	job.accessPriority = HelperPriority(accessPriority);
	job.startPriority = job.accessPriority;
	job.dialogPriority = PhasePriority(graphicsPriority, job.accessPriority);
	job.finalPriority = job.accessPriority;
	QueueDialogThread(job);
}

void DialogThreadShutdown(DialogType type, int delayUs, int accessPriority, int graphicsPriority) {
	ThreadJob job;
	job.shutdown = true;
	job.type = type;
	job.partDelayUs = delayUs / 4;
	job.accessPriority = HelperPriority(accessPriority);
	job.startPriority = job.accessPriority;
	job.dialogPriority = PhasePriority(graphicsPriority, job.accessPriority);
	// A savedata shutdown ends at priority 0x20 whatever the dialog's thread priorities: a caller at
	// 0x20 gets the CPU back first and sees SHUTDOWN, one at 0x21 or worse only NONE
	// (utility/savedata/shutdownstatus).
	job.finalPriority = type == DialogType::SAVEDATA ? 0x20 : job.accessPriority;
	QueueDialogThread(job);
}

int VolatileMemTryLock() { return (int)CallHle("sceSuspendForUser", 0xA14F40B2, {0, 0, 0}); }
int VolatileMemUnlock() { return (int)CallHle("sceSuspendForUser", 0xA569E425, {0}); }
int SystemLanguage() { return systemparam::Language(); }
int SystemButtonPreference() { return systemparam::ButtonMode(); }
u32 CompiledSdkVersion() { return CallHle("SysMemUserForUser", 0xFC114573, {}); }
bool Headless() { return host::IsHeadless(); }

}  // namespace utility

void Register_sceUtility() {
	CreateDialogs();
	g_currentDialogType = DialogType::NONE;
	g_currentDialogActive = false;
	g_loadedModules.clear();
	g_helperThread = 0;
	g_helperFinished = true;
	g_helperCode = 0;
	g_jobPending = false;

	static const hle::HleFunction funcs[] = {
		HLEFN(0x1579A159, sceUtilityLoadNetModule),
		HLEFN(0x64D50C56, sceUtilityUnloadNetModule),

		HLEFN(0xF88155F6, sceUtilityNetconfShutdownStart),
		HLEFN(0x4DB1E739, sceUtilityNetconfInitStart),
		HLEFN(0x91E70E35, sceUtilityNetconfUpdate),
		HLEFN(0x6332AA39, sceUtilityNetconfGetStatus),
		HLEFN(0x5EEE6548, sceUtilityCheckNetParam),
		HLEFN(0x434D4B3A, sceUtilityGetNetParam),
		HLEFN(0x4FED24D8, sceUtilityGetNetParamLatestID),

		HLEFN(0x67AF3428, sceUtilityMsgDialogShutdownStart),
		HLEFN(0x2AD8E239, sceUtilityMsgDialogInitStart),
		HLEFN(0x95FC253B, sceUtilityMsgDialogUpdate),
		HLEFN(0x9A1C91D7, sceUtilityMsgDialogGetStatus),
		HLEFN(0x4928BD96, sceUtilityMsgDialogAbort),

		HLEFN(0x9790B33C, sceUtilitySavedataShutdownStart),
		HLEFN(0x50C4CD57, sceUtilitySavedataInitStart),
		HLEFN(0xD4B95FFB, sceUtilitySavedataUpdate),
		HLEFN(0x8874DBE0, sceUtilitySavedataGetStatus),

		HLEFN(0x3DFAEBA9, sceUtilityOskShutdownStart),
		HLEFN(0xF6269B82, sceUtilityOskInitStart),
		HLEFN(0x4B85C861, sceUtilityOskUpdate),
		HLEFN(0xF3F76017, sceUtilityOskGetStatus),

		HLEFN(0x41E30674, sceUtilitySetSystemParamString),
		HLEFN(0x45C18506, sceUtilitySetSystemParamInt),
		HLEFN(0x34B78343, sceUtilityGetSystemParamString),
		HLEFN(0xA5DA2406, sceUtilityGetSystemParamInt),

		HLEFN(0xC492F751, sceUtilityGameSharingInitStart),
		HLEFN(0xEFC6F80F, sceUtilityGameSharingShutdownStart),
		HLEFN(0x7853182D, sceUtilityGameSharingUpdate),
		HLEFN(0x946963F3, sceUtilityGameSharingGetStatus),

		HLEFN(0xBDA7D894, sceUtilityHtmlViewerGetStatus),
		HLEFN(0xCDC3AA41, sceUtilityHtmlViewerInitStart),
		HLEFN(0xF5CE1134, sceUtilityHtmlViewerShutdownStart),
		HLEFN(0x05AFB9E4, sceUtilityHtmlViewerUpdate),

		HLEFN(0xC629AF26, sceUtilityLoadAvModule),
		HLEFN(0xF7D8D092, sceUtilityUnloadAvModule),
		HLEFN(0x2A2B3DE0, sceUtilityLoadModule),
		HLEFN(0xE49BFE92, sceUtilityUnloadModule),

		HLEFN(0x0251B134, sceUtilityScreenshotInitStart),
		HLEFN(0xF9E0008C, sceUtilityScreenshotShutdownStart),
		HLEFN(0xAB083EA9, sceUtilityScreenshotUpdate),
		HLEFN(0xD81957B7, sceUtilityScreenshotGetStatus),
		HLEFN(0x86A03A27, sceUtilityScreenshotContStart),

		HLEFN(0x0D5BC6D2, sceUtilityLoadUsbModule),
		HLEFN(0xF64910F0, sceUtilityUnloadUsbModule),

		HLEFN(0x24AC31EB, sceUtilityGamedataInstallInitStart),
		HLEFN(0x32E32DCB, sceUtilityGamedataInstallShutdownStart),
		HLEFN(0x4AECD179, sceUtilityGamedataInstallUpdate),
		HLEFN(0xB57E95D9, sceUtilityGamedataInstallGetStatus),
		HLEFN(0x180F7B62, sceUtilityGamedataInstallAbort),

		HLEFN(0x16D02AF0, sceUtilityNpSigninInitStart),
		HLEFN(0xE19C97D6, sceUtilityNpSigninShutdownStart),
		HLEFN(0xF3FBC572, sceUtilityNpSigninUpdate),
		HLEFN(0x86ABDB1B, sceUtilityNpSigninGetStatus),

		HLEFN(0x1281DA8E, sceUtilityInstallInitStart),

		HLEFN(0x54A5C62F, sceUtilityStoreCheckoutShutdownStart),
		HLEFN(0xDA97F1AA, sceUtilityStoreCheckoutInitStart),
		HLEFN(0xB8592D5F, sceUtilityStoreCheckoutUpdate),
		HLEFN(0x3AAD51DC, sceUtilityStoreCheckoutGetStatus),

		HLEFN(0xD17A0573, sceUtilityPS3ScanShutdownStart),
		HLEFN(0x42071A83, sceUtilityPS3ScanInitStart),
		HLEFN(0xD852CDCE, sceUtilityPS3ScanUpdate),
		HLEFN(0x89317C8F, sceUtilityPS3ScanGetStatus),

		HLEFN(0xBC6B6296, sceUtilityNetplayShutdownStart),
		HLEFN(0x3AD50AE7, sceUtilityNetplayInitStart),
		HLEFN(0x417BED54, sceUtilityNetplayUpdate),
		HLEFN(0xB6CEE597, sceUtilityNetplayGetStatus),

		// Dialogs without an implementation: report that another type is current.
		HLEFN(0x2995D020, sceUtilityUnsupportedDialog),  // sceUtilitySavedataErrInitStart
		HLEFN(0xB62A4061, sceUtilityUnsupportedDialog),  // sceUtilitySavedataErrShutdownStart
		HLEFN(0xED0FAD38, sceUtilityUnsupportedDialog),  // sceUtilitySavedataErrUpdate
		HLEFN(0x88BC7406, sceUtilityUnsupportedDialog),  // sceUtilitySavedataErrGetStatus
		HLEFN(0x16A1A8D8, sceUtilityUnsupportedDialog),  // sceUtilityAuthDialogGetStatus
		HLEFN(0x943CBA46, sceUtilityUnsupportedDialog),  // sceUtilityAuthDialogInitStart
		HLEFN(0x0F3EEAAC, sceUtilityUnsupportedDialog),  // sceUtilityAuthDialogShutdownStart
		HLEFN(0x147F7C85, sceUtilityUnsupportedDialog),  // sceUtilityAuthDialogUpdate
		HLEFN(0x5EF1C24A, sceUtilityUnsupportedDialog),  // sceUtilityInstallShutdownStart
		HLEFN(0xA03D29BA, sceUtilityUnsupportedDialog),  // sceUtilityInstallUpdate
		HLEFN(0xC4700FA3, sceUtilityUnsupportedDialog),  // sceUtilityInstallGetStatus
		HLEFN(0xC130D441, sceUtilityUnsupportedDialog),  // sceUtilityPsnShutdownStart
		HLEFN(0xA7BB7C67, sceUtilityUnsupportedDialog),  // sceUtilityPsnInitStart
		HLEFN(0x0940A1B9, sceUtilityUnsupportedDialog),  // sceUtilityPsnUpdate
		HLEFN(0x094198B8, sceUtilityUnsupportedDialog),  // sceUtilityPsnGetStatus
		HLEFN(0x9F313D14, sceUtilityUnsupportedDialog),  // sceUtilityAutoConnectShutdownStart
		HLEFN(0x3A15CD0A, sceUtilityUnsupportedDialog),  // sceUtilityAutoConnectInitStart
		HLEFN(0xD23665F4, sceUtilityUnsupportedDialog),  // sceUtilityAutoConnectUpdate
		HLEFN(0xD4C2BD73, sceUtilityUnsupportedDialog),  // sceUtilityAutoConnectGetStatus
		HLEFN(0x0E0C27AF, sceUtilityUnsupportedDialog),  // sceUtilityAutoConnectAbort
		HLEFN(0x06A48659, sceUtilityUnsupportedDialog),  // sceUtilityRssSubscriberShutdownStart
		HLEFN(0x4B0A8FE5, sceUtilityUnsupportedDialog),  // sceUtilityRssSubscriberInitStart
		HLEFN(0xA084E056, sceUtilityUnsupportedDialog),  // sceUtilityRssSubscriberUpdate
		HLEFN(0x2B96173B, sceUtilityUnsupportedDialog),  // sceUtilityRssSubscriberGetStatus
		HLEFN(0x149A7895, sceUtilityUnsupportedDialog),  // sceUtilityDNASShutdownStart
		HLEFN(0xDDE5389D, sceUtilityUnsupportedDialog),  // sceUtilityDNASInitStart
		HLEFN(0x4A833BA4, sceUtilityUnsupportedDialog),  // sceUtilityDNASUpdate
		HLEFN(0xA50E5B30, sceUtilityUnsupportedDialog),  // sceUtilityDNASGetStatus
		HLEFN(0xE7B778D8, sceUtilityUnsupportedDialog),  // sceUtilityRssReaderShutdownStart
		HLEFN(0x81C44706, sceUtilityUnsupportedDialog),  // sceUtilityRssReaderInitStart
		HLEFN(0x6F56F9CF, sceUtilityUnsupportedDialog),  // sceUtilityRssReaderUpdate
		HLEFN(0x8326AB05, sceUtilityUnsupportedDialog),  // sceUtilityRssReaderGetStatus
		HLEFN(0xB0FB7FF5, sceUtilityUnsupportedDialog),  // sceUtilityRssReaderContStart

		HLERAW(NID_DIALOG_THREAD, DialogThreadMain),
	};
	hle::RegisterModule(LIB, funcs);
}
