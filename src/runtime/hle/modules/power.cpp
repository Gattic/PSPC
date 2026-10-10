// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// scePower / sceSuspendForUser: power callbacks, clock frequencies and the volatile memory lock.
// The clocks are reported only: guest timing stays at 222 MHz whatever the CPU clock is set to.
#include <cmath>
#include <vector>

#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

using namespace kernel;

namespace {

constexpr u32 PSP_POWER_ERROR_TAKEN_SLOT = 0x80000020;
constexpr u32 PSP_POWER_ERROR_SLOTS_FULL = 0x80000022;
constexpr u32 PSP_POWER_ERROR_EMPTY_SLOT = 0x80000025;
constexpr u32 PSP_POWER_ERROR_INVALID_CB = 0x80000100;
constexpr u32 PSP_POWER_ERROR_INVALID_SLOT = 0x80000102;

constexpr s32 PSP_POWER_CB_AC_POWER = 0x00001000;
constexpr s32 PSP_POWER_CB_BATTERY_EXIST = 0x00000080;
constexpr s32 PSP_POWER_CB_BATTERY_FULL = 0x00000064;

constexpr int kUserSlots = 16;    // callback slots for user mode
constexpr int kKernelSlots = 32;  // ... and for kernel mode

constexpr u32 kVolatileAddr = 0x08400000, kVolatileSize = 0x00400000;

SceUID g_cbSlots[kUserSlots] = {};
bool g_volatileLocked = false;
struct VolatileWaiter {
	SceUID thread;
	u32 addrPtr, sizePtr;
};
std::vector<VolatileWaiter> g_volatileWaiters;

int g_realPllHz = 222000000, g_realBusHz = 111000000;
int g_pllHz = 222000000, g_busHz = 111000000;
int g_cpuHz = 222000000;

// The CPU clock can only be a multiple of the PLL divided by 511.
int CpuMhzToHz(int desired, int pllHz) {
	const double maxfreq = desired * 1000000.0;
	const double step = (double)pllHz / 511.0;
	// These values seem to be locked.
	if (pllHz >= 333000000 && desired == 333) return 333000000;
	if (pllHz >= 222000000 && desired == 222) return 222000000;
	double freq = 0;
	while (freq + step < maxfreq) freq += step;
	// The PSP's own functions have the same float error.
	return (int)((float)(freq / 1000000.0f) * 1000000);
}

// These seem to be the only steps the PLL and bus have.
int PllMhzToHz(int mhz) {
	if (mhz <= 190) return 190285721;
	if (mhz <= 222) return 222000000;
	if (mhz <= 266) return 266399994;
	if (mhz <= 333) return 333000000;
	return mhz * 1000000;
}
int BusMhzToHz(int mhz) {
	if (mhz <= 95) return 95142860;
	if (mhz <= 111) return 111000000;
	if (mhz <= 133) return 133199997;
	if (mhz <= 166) return 166500000;
	return mhz * 1000000;
}

bool CanWait() { return sched::Current() && sched::DispatchEnabled() && !sched::InInterrupt(); }

void HleDelay(u64 us) {
	if (CanWait()) sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
}

// ---------------------------------------------------------------- callbacks and battery

int scePowerRegisterCallback(int slot, SceUID cbId) {
	if (slot < -1 || slot >= kKernelSlots) return (int)PSP_POWER_ERROR_INVALID_SLOT;
	if (slot >= kUserSlots) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if (cbId == 0 || !Get<Callback>(cbId)) return (int)PSP_POWER_ERROR_INVALID_CB;
	int ret = -1;
	if (slot == -1) {
		for (int i = 0; i < kUserSlots; i++) {
			if (g_cbSlots[i] == 0) {
				g_cbSlots[i] = cbId;
				ret = i;
				break;
			}
		}
		if (ret == -1) return (int)PSP_POWER_ERROR_SLOTS_FULL;
	} else {
		if (g_cbSlots[slot] != 0) return (int)PSP_POWER_ERROR_TAKEN_SLOT;
		g_cbSlots[slot] = cbId;
		ret = 0;
	}
	NotifyCallback(cbId, PSP_POWER_CB_AC_POWER | PSP_POWER_CB_BATTERY_EXIST | PSP_POWER_CB_BATTERY_FULL);
	return ret;
}
int scePowerUnregisterCallback(int slot) {
	if (slot < 0 || slot >= kKernelSlots) return (int)PSP_POWER_ERROR_INVALID_SLOT;
	if (slot >= kUserSlots) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if (g_cbSlots[slot] == 0) return (int)PSP_POWER_ERROR_EMPTY_SLOT;
	g_cbSlots[slot] = 0;
	return 0;
}
int scePowerTick(int) { return 0; }
int scePowerIsPowerOnline() { return 1; }
int scePowerIsBatteryExist() { return 1; }
int scePowerIsBatteryCharging() { return 0; }
int scePowerGetBatteryChargingStatus() { return 0; }
int scePowerIsLowBattery() { return 0; }
int scePowerIsSuspendRequired() { return 0; }
// The power switch was never slid (the camera plugin polls this while it runs).
int scePowerIsRequest() { return 0; }
int scePowerGetBatteryLifePercent() { return 100; }
int scePowerGetBatteryLifeTime() { return 0; }  // 0: on AC power
int scePowerGetBatteryTemp() { return 0; }
int scePowerCancelRequest() { return 0; }
int scePowerRequestStandby() { return 0; }
int scePowerRequestSuspend() { return 0; }
int scePowerCheckWlanCoexistenceClock() { return 0; }
int scePowerZero() { return 0; }

// ---------------------------------------------------------------- clocks

u32 scePowerSetClockFrequency(u32 pll, u32 cpu, u32 bus) {
	// 190 might be a typo for 19, but it's what the PSP validates against.
	if (pll < 19 || pll < cpu || pll > 333) return SCE_KERNEL_ERROR_INVALID_VALUE;
	if (cpu == 0 || cpu > 333) return SCE_KERNEL_ERROR_INVALID_VALUE;
	if (bus == 0 || bus > 166) return SCE_KERNEL_ERROR_INVALID_VALUE;
	// It only reschedules when the stepped PLL frequency changes; the bus argument has no effect.
	if (g_realPllHz != PllMhzToHz((int)pll)) {
		const int oldPll = g_realPllHz / 1000000;
		g_realPllHz = PllMhzToHz((int)pll);
		g_realBusHz = BusMhzToHz(g_realPllHz / 2000000);
		g_pllHz = g_realPllHz;
		g_busHz = g_realBusHz;
		g_cpuHz = CpuMhzToHz((int)cpu, g_pllHz);
		// The delay depends on the source and destination frequency; most are 150ms.
		const int newPll = g_realPllHz / 1000000;
		u64 us = 150000;
		if ((newPll == 190 && oldPll == 222) || (newPll == 222 && oldPll == 190)) us = 15700;
		else if ((newPll == 266 && oldPll == 333) || (newPll == 333 && oldPll == 266)) us = 16600;
		HleDelay(us);
		return 0;
	}
	g_cpuHz = CpuMhzToHz((int)cpu, g_pllHz);
	return 0;
}
u32 scePowerSetCpuClockFrequency(u32 cpu) {
	if (cpu == 0 || cpu > 333) return SCE_KERNEL_ERROR_INVALID_VALUE;
	// The CPU can't run faster than the PLL it's divided from.
	if ((u64)cpu * 1000000 > (u64)g_pllHz) return SCE_KERNEL_ERROR_INVALID_VALUE;
	g_cpuHz = CpuMhzToHz((int)cpu, g_pllHz);
	return 0;
}
u32 scePowerSetBusClockFrequency(u32 bus) {
	if (bus == 0 || bus > 111) return SCE_KERNEL_ERROR_INVALID_VALUE;
	// The value passed is validated, but then doesn't seem to matter for the result (PPSSPP;
	// note that it compares the Hz value against MHz thresholds, so this is always pll / 2).
	if (g_pllHz <= 190) g_busHz = 94956673;
	else if (g_pllHz <= 222) g_busHz = 111000000;
	else if (g_pllHz <= 266) g_busHz = 132939331;
	else if (g_pllHz <= 333) g_busHz = 165848343;
	else g_busHz = g_pllHz / 2;
	return 0;
}
u32 scePowerGetCpuClockFrequencyInt() { return (u32)(g_cpuHz / 1000000); }
u32 scePowerGetPllClockFrequencyInt() { return (u32)(g_pllHz / 1000000); }
u32 scePowerGetBusClockFrequencyInt() { return (u32)(g_busHz / 1000000); }
float scePowerGetCpuClockFrequencyFloat() {
	// The CPU runs at a multiple of pll/511, and the firmware works the value out in single
	// precision, as pll * n / 511 (power/freq).
	const double step = (double)g_pllHz / 511.0;
	const float steps = (float)std::round((double)g_cpuHz / step);
	const float pllMhz = (float)(g_pllHz / 1000000.0);
	return (pllMhz * steps) / 511.0f;
}
float scePowerGetPllClockFrequencyFloat() { return (float)g_pllHz / 1000000.0f; }
float scePowerGetBusClockFrequencyFloat() { return (float)g_busHz / 1000000.0f; }

// ---------------------------------------------------------------- volatile memory

int VolatileLockNow(int type, u32 addrPtr, u32 sizePtr) {
	if (type != 0) return (int)SCE_KERNEL_ERROR_INVALID_MODE;
	if (g_volatileLocked) return (int)SCE_KERNEL_ERROR_POWER_VMEM_IN_USE;
	// Volatile RAM is always at 0x08400000 and 4 MB.
	if (mem::IsValidRange(addrPtr, 4)) GWrite32(addrPtr, kVolatileAddr);
	if (mem::IsValidRange(sizePtr, 4)) GWrite32(sizePtr, kVolatileSize);
	g_volatileLocked = true;
	return 0;
}

int sceKernelVolatileMemTryLock(int type, u32 addrPtr, u32 sizePtr) {
	const int err = VolatileLockNow(type, addrPtr, sizePtr);
	// Under 100us on hardware (pspautotests threads/scheduling/syscallkinds).
	if (err == 0) sched::ChargeCycles(1200);
	return err;
}

int sceKernelVolatileMemLock(int type, u32 addrPtr, u32 sizePtr) {
	int err;
	// With dispatch disabled or in an interrupt it doesn't check, just fails; but it still writes
	// the address and size, through the pointers that are there.
	if (!sched::DispatchEnabled()) err = (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	else if (sched::InInterrupt()) err = (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	else err = VolatileLockNow(type, addrPtr, sizePtr);

	if (err == 0) {
		sched::ChargeCycles(1200);
	} else if (err == (int)SCE_KERNEL_ERROR_POWER_VMEM_IN_USE) {
		PspThread *t = sched::Current();
		if (!t) return err;
		g_volatileWaiters.push_back({t->uid, addrPtr, sizePtr});
		return sched::Wait(WAITTYPE_VMEM, 1, 0, false);
	} else if (err == (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT || err == (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT) {
		if ((addrPtr & 3) == 0 && mem::IsValidRange(addrPtr, 4)) GWrite32(addrPtr, kVolatileAddr);
		if ((sizePtr & 3) == 0 && mem::IsValidRange(sizePtr, 4)) GWrite32(sizePtr, kVolatileSize);
	}
	return err;
}

int sceKernelVolatileMemUnlock(int type) {
	if (type != 0) return (int)SCE_KERNEL_ERROR_INVALID_MODE;
	// It must use a semaphore.
	if (!g_volatileLocked) return (int)SCE_KERNEL_ERROR_SEMA_OVF;
	g_volatileLocked = false;
	// Hand it to the first waiter (always FIFO).
	while (!g_volatileWaiters.empty() && !g_volatileLocked) {
		const VolatileWaiter w = g_volatileWaiters.front();
		g_volatileWaiters.erase(g_volatileWaiters.begin());
		PspThread *t = Get<PspThread>(w.thread);
		// Skip threads that were force-released.
		if (t && t->IsWaitingFor(WAITTYPE_VMEM, 1) && VolatileLockNow(0, w.addrPtr, w.sizePtr) == 0) sched::Wake(t, 0);
	}
	return 0;
}

int sceKernelPowerLock(int type) { return type == 0 ? 0 : (int)SCE_KERNEL_ERROR_INVALID_MODE; }
int sceKernelPowerUnlock(int type) { return type == 0 ? 0 : (int)SCE_KERNEL_ERROR_INVALID_MODE; }
int sceKernelPowerTick(int) { return 0; }

}  // namespace

void Register_scePower() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x04B7766E, scePowerRegisterCallback),
		HLEFN(0xEFD3C963, scePowerTick),
		HLEFN(0x87440F5E, scePowerIsPowerOnline),
		HLEFN(0x0AFD0D8B, scePowerIsBatteryExist),
		HLEFN(0x1E490401, scePowerIsBatteryCharging),
		HLEFN(0xB4432BC8, scePowerGetBatteryChargingStatus),
		HLEFN(0xD3075926, scePowerIsLowBattery),
		HLEFN(0x78A1A796, scePowerIsSuspendRequired),
		HLEFN(0x7FA406DD, scePowerIsRequest),
		HLEFN(0x2085D15D, scePowerGetBatteryLifePercent),
		HLEFN(0x8EFB3FA2, scePowerGetBatteryLifeTime),
		HLEFN(0x28E12023, scePowerGetBatteryTemp),
		HLEFN(0xDB62C9CF, scePowerCancelRequest),
		HLEFN(0x2B7C7CF4, scePowerRequestStandby),
		HLEFN(0xAC32C9CC, scePowerRequestSuspend),
		HLEFN(0xDFA8BAF8, scePowerUnregisterCallback),
		HLEFN(0xDB9D28DD, scePowerUnregisterCallback),
		HLEFN(0x843FBF43, scePowerSetCpuClockFrequency),
		HLEFN(0xB8D7B3FB, scePowerSetBusClockFrequency),
		HLEFN(0xFEE03A2F, scePowerGetCpuClockFrequencyInt),
		HLEFN(0x478FE6F5, scePowerGetBusClockFrequencyInt),
		HLEFN(0xFDB5BFE9, scePowerGetCpuClockFrequencyInt),
		HLEFN(0xBD681969, scePowerGetBusClockFrequencyInt),
		HLEFN(0xB1A52C83, scePowerGetCpuClockFrequencyFloat),
		HLEFN(0x9BADB3EB, scePowerGetBusClockFrequencyFloat),
		HLEFN(0x737486F2, scePowerSetClockFrequency),
		HLEFN(0x34F9C463, scePowerGetPllClockFrequencyInt),
		HLEFN(0xEA382A27, scePowerGetPllClockFrequencyFloat),
		HLEFN(0xEBD177D6, scePowerSetClockFrequency),
		HLEFN(0x469989AD, scePowerSetClockFrequency),
		HLEFN(0xA85880D0, scePowerCheckWlanCoexistenceClock),
		HLEFN(0x23C31FFE, sceKernelVolatileMemLock),
		HLEFN(0xFA97A599, sceKernelVolatileMemTryLock),
		HLEFN(0xB3EDD801, sceKernelVolatileMemUnlock),
		// scePower_driver: real names unknown; both just succeed.
		HLEFN(0x5F5006D2, scePowerZero),
		HLEFN(0xBA566CD0, scePowerZero),  // scePowerSetWakeupCondition
	};
	hle::RegisterModule("scePower", funcs);
	hle::RegisterModule("scePower_driver", funcs);
}

void Register_sceSuspendForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xEADB1BD7, sceKernelPowerLock),
		HLEFN(0x3AEE7261, sceKernelPowerUnlock),
		HLEFN(0x090CCB3F, sceKernelPowerTick),
		HLEFN(0xA14F40B2, sceKernelVolatileMemTryLock),
		HLEFN(0xA569E425, sceKernelVolatileMemUnlock),
		HLEFN(0x3E0271D3, sceKernelVolatileMemLock),
	};
	hle::RegisterModule("sceSuspendForUser", funcs);
}
