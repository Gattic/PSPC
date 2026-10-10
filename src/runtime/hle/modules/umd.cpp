// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/HLE/sceUmd.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceUmdUser. In the firmware (mediaman.prx) the drive state is an event flag and every wait is a
// wait on it. As in PPSSPP a disc counts as inserted even when none is mounted (pspautotests run
// without one), and the drive starts activated.
#include <algorithm>
#include <vector>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/media/disc.h"

using namespace kernel;

namespace {

enum : u32 {
	PSP_UMD_NOT_PRESENT = 0x01,
	PSP_UMD_PRESENT = 0x02,
	PSP_UMD_CHANGED = 0x04,
	PSP_UMD_NOT_READY = 0x08,
	PSP_UMD_READY = 0x10,
	PSP_UMD_READABLE = 0x20,
	// The states a wait may name (not CHANGED).
	UMD_STAT_ALLOW_WAIT = PSP_UMD_NOT_PRESENT | PSP_UMD_PRESENT | PSP_UMD_NOT_READY | PSP_UMD_READY | PSP_UMD_READABLE,
};
constexpr u32 PSP_UMD_TYPE_GAME = 0x10;
constexpr u64 kActivateDelayUs = 4000;  // the drive spins up before it reads
constexpr SceUID kUmdWaitId = 1;

bool g_activated = true;
bool g_inserted = true;
// The XMB boots with an empty drive: then "inserted" follows whether a disc image is mounted.
bool g_followMedia = false;
SceUID g_driveCb = 0;
u64 g_activateEvent = 0;
std::vector<SceUID> g_waiters;

u32 SdkVersion() {
	const hle::HleFunction *fn = hle::FindFunction("SysMemUserForUser", 0xFC114573);
	if (!fn || !fn->fn) return 0;
	alignas(64) Ctx c{};
	fn->fn(&c);
	return c.r[2];
}

// A user-mode caller can't pass a kernel address (mediaman checks them with k1).
bool IsUserAddress(u32 addr) { return (addr & 0x80000000) == 0; }

u32 DriveState() {
	if (!g_inserted) return PSP_UMD_NOT_PRESENT;
	// Games expect the disc to be ready early on, active or not.
	u32 state = PSP_UMD_PRESENT | PSP_UMD_READY;
	if (g_activated) state |= PSP_UMD_READABLE;
	return state;
}

bool IsUmdWaiter(PspThread *t) { return t && t->waitType == WAITTYPE_UMD && t->waitId == kUmdWaitId && !t->waitDone; }

void WakeWaiters() {
	std::vector<SceUID> keep;
	for (SceUID id : g_waiters) {
		PspThread *t = Get<PspThread>(id);
		if (!IsUmdWaiter(t)) continue;
		if (t->waitValue[0] & DriveState()) sched::Wake(t, 0);
		else keep.push_back(id);
	}
	g_waiters.swap(keep);
}

void Notify(u32 arg) {
	if (g_driveCb) NotifyCallback(g_driveCb, (s32)arg);
}

void Activate() {
	u32 arg = PSP_UMD_PRESENT | PSP_UMD_READABLE;
	if (SdkVersion() != 0) arg |= PSP_UMD_READY;
	Notify(arg);
	if (g_activateEvent) sched::CancelEvent(g_activateEvent);
	g_activateEvent = sched::ScheduleEvent(sched::NowUs() + kActivateDelayUs, []() {
		g_activateEvent = 0;
		g_activated = true;
		WakeWaiters();
	});
}

void Deactivate() {
	Notify(PSP_UMD_PRESENT | PSP_UMD_READY);
	if (g_activateEvent) sched::CancelEvent(g_activateEvent);
	g_activateEvent = 0;
	g_activated = false;
	WakeWaiters();
}

int CheckWait(u32 stat) {
	if ((stat & UMD_STAT_ALLOW_WAIT) == 0) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	return 0;
}

void RemoveWaiter(SceUID id) { g_waiters.erase(std::remove(g_waiters.begin(), g_waiters.end(), id), g_waiters.end()); }

// Waits until the state has one of `stat`'s bits. timeoutUs 0 waits forever, as the firmware
// passes no timeout at all then; the scheduler applies the hardware's timeout granularity.
int WaitStat(u32 stat, u32 timeoutUs, bool cb) {
	PspThread *t = sched::Current();
	const SceUID uid = t->uid;
	t->waitValue[0] = stat;
	g_waiters.push_back(uid);
	// While callbacks run the thread isn't waiting on the drive (a cancel then misses it); when
	// they return, the state is checked again (umd/api).
	WaitHooks hooks;
	hooks.pause = [uid]() { RemoveWaiter(uid); };
	hooks.resume = [uid, stat]() {
		PspThread *w = Get<PspThread>(uid);
		if (w && (stat & DriveState())) {
			sched::Wake(w, 0);
			return true;
		}
		g_waiters.push_back(uid);
		return false;
	};
	hooks.timeout = [uid]() { RemoveWaiter(uid); };
	if (timeoutUs == 0) return sched::Wait(WAITTYPE_UMD, kUmdWaitId, 0, cb, &hooks);
	u32 slot = AllocKernelMemory(4, "umd wait timeout");
	GWrite32(slot, timeoutUs);
	int r = sched::Wait(WAITTYPE_UMD, kUmdWaitId, slot, cb, &hooks);
	g_kernelMem.Free(slot);
	return r;
}

int sceUmdCheckMedium() { return g_inserted ? 1 : 0; }

u32 sceUmdGetDiscInfo(u32 infoAddr) {
	if (!mem::IsValidRange(infoAddr, 8) || !IsUserAddress(infoAddr) || !IsUserAddress(infoAddr + 4))
		return SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	if (GRead32(infoAddr) != 8) return SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	GWrite32(infoAddr + 4, media::DiscType());  // PSP_UMD_TYPE_GAME / _VIDEO / _AUDIO
	return 0;
}

int sceUmdActivate(u32 mode, u32 namePtr) {
	if (mode < 1 || mode > 2) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	// The name is compared before the pointer is checked.
	const char *name = namePtr ? mem::GetCString(namePtr) : nullptr;
	if (!name || strcmp(name, "disc0:") != 0 || !IsUserAddress(namePtr)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	Activate();
	return 0;
}

int sceUmdDeactivate(u32 mode, u32 namePtr) {
	if (mode > 18) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	// Unlike activation the name isn't compared, and only mode 2 requires one.
	if ((mode == 2 && namePtr == 0) || !IsUserAddress(namePtr)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	Deactivate();
	return 0;
}

u32 sceUmdRegisterUMDCallBack(SceUID cb) {
	if (!Get<Callback>(cb)) return SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	g_driveCb = cb;  // there is only ever one
	return 0;
}

int sceUmdUnRegisterUMDCallBack(SceUID cb) {
	if (cb != g_driveCb) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	g_driveCb = 0;
	return SdkVersion() > 0x3000000 ? 0 : cb;
}

u32 sceUmdGetDriveStat() { return DriveState(); }

int sceUmdWaitDriveStat(u32 stat) {
	int err = CheckWait(stat);
	if (err < 0) return err;
	if ((stat & DriveState()) == 0) return WaitStat(stat, 0, false);
	return 0;
}

int sceUmdWaitDriveStatWithTimer(u32 stat, u32 timeout) {
	int err = CheckWait(stat);
	if (err < 0) return err;
	if ((stat & DriveState()) == 0) return WaitStat(stat, timeout, false);
	sched::Reschedule();
	return 0;
}

int sceUmdWaitDriveStatCB(u32 stat, u32 timeout) {
	int err = CheckWait(stat);
	if (err < 0) return err;
	PspThread *t = sched::Current();
	if (t && CanRunCallbacks(t)) RunPendingCallbacks(t);
	if ((stat & DriveState()) == 0) return WaitStat(stat, timeout, true);
	sched::Reschedule();
	return 0;
}

u32 sceUmdCancelWaitDriveStat() {
	// Waits paused for a callback aren't on the list, so they aren't canceled (umd/api).
	std::vector<SceUID> waiters;
	waiters.swap(g_waiters);
	for (SceUID id : waiters) {
		PspThread *t = Get<PspThread>(id);
		if (IsUmdWaiter(t)) sched::Wake(t, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL);
	}
	return 0;
}

u32 sceUmdGetErrorStat() { return 0; }
u32 sceUmdReplaceProhibit() { return 0; }
u32 sceUmdReplacePermit() { return 0; }
u32 sceUmdUseUMDInMsUsbWlan() { return 0; }
u32 sceUmdUnuseUMDInMsUsbWlan() { return 0; }

}  // namespace

namespace umd {
// A disc image was mounted or removed.
void NotifyDiscChanged() {
	if (g_followMedia) {
		g_inserted = media::HaveDisc();
		if (g_driveCb) Notify(g_inserted ? PSP_UMD_PRESENT | PSP_UMD_READABLE | PSP_UMD_CHANGED : PSP_UMD_NOT_PRESENT | PSP_UMD_CHANGED);
		WakeWaiters();
		return;
	}
	if (g_driveCb) Notify(PSP_UMD_PRESENT | PSP_UMD_READABLE | PSP_UMD_CHANGED);
	WakeWaiters();
}
// The XMB: no disc counts as inserted unless an image is mounted (games always have one).
void SetDriveEmpty(bool empty) {
	g_followMedia = true;
	g_inserted = !empty;
}
bool DiscInserted() { return g_inserted; }
}  // namespace umd

void Register_sceUmdUser() {
	g_activated = true;
	g_inserted = true;
	g_followMedia = false;
	g_driveCb = 0;
	g_activateEvent = 0;
	g_waiters.clear();
	static const hle::HleFunction funcs[] = {
		HLEFN(0xC6183D47, sceUmdActivate),
		HLEFN(0x6B4A146C, sceUmdGetDriveStat),
		HLEFN(0x46EBB729, sceUmdCheckMedium),
		HLEFN(0xE83742BA, sceUmdDeactivate),
		HLEFN(0x8EF08FCE, sceUmdWaitDriveStat),
		HLEFN(0x56202973, sceUmdWaitDriveStatWithTimer),
		HLEFN(0x4A9E5E29, sceUmdWaitDriveStatCB),
		HLEFN(0x6AF9B50A, sceUmdCancelWaitDriveStat),
		HLEFN(0x20628E6F, sceUmdGetErrorStat),
		HLEFN(0x340B7686, sceUmdGetDiscInfo),
		HLEFN(0xAEE7404D, sceUmdRegisterUMDCallBack),
		HLEFN(0xBD2BDE07, sceUmdUnRegisterUMDCallBack),
		HLEFN(0x87533940, sceUmdReplaceProhibit),
		HLEFN(0xCBE9F02A, sceUmdReplacePermit),
		HLEFN(0xB103FA38, sceUmdUseUMDInMsUsbWlan),
		HLEFN(0x14C6C45C, sceUmdUnuseUMDInMsUsbWlan),
	};
	hle::RegisterModule("sceUmdUser", funcs);
}
