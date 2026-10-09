// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Small system libraries: sceImpose, sceDmac, sceHprm. (UtilsForUser is in utils.cpp, sceRtc in
// rtc.cpp, scePower and sceSuspendForUser in power.cpp.)
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

using namespace kernel;

namespace {

// ---------------------------------------------------------------- sceImpose

int g_language = 1, g_buttonMode = 1;  // English, X = confirm (western default)
int g_umdPopup = 0, g_backlightOff = 0;
int sceImposeSetLanguageMode(int lang, int button) {
	g_language = lang;
	g_buttonMode = button;
	return 0;
}
int sceImposeGetLanguageMode(u32 langPtr, u32 buttonPtr) {
	if (mem::IsValidRange(langPtr, 4)) GWrite32(langPtr, (u32)g_language);
	if (mem::IsValidRange(buttonPtr, 4)) GWrite32(buttonPtr, (u32)g_buttonMode);
	return 0;
}
int sceImposeGetBatteryIconStatus(u32 chargingPtr, u32 iconPtr) {
	if (mem::IsValidRange(chargingPtr, 4)) GWrite32(chargingPtr, 0);
	if (mem::IsValidRange(iconPtr, 4)) GWrite32(iconPtr, 3);
	return 0;
}
int sceImposeSetUMDPopup(int v) {
	g_umdPopup = v;
	return 0;
}
int sceImposeGetUMDPopup() { return g_umdPopup; }
int sceImposeGetBacklightOffTime() { return g_backlightOff; }
int sceImposeSetBacklightOffTime(int v) {
	g_backlightOff = v;
	return 0;
}

// ---------------------------------------------------------------- sceDmac / sceHprm

// When the last copy's transfer ends (guest cycles); a copy started before then overlaps it.
u64 g_dmacDeadline = 0;

u32 CheckDmacArgs(u32 dst, u32 src, u32 size) {
	if (size == 0) return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (!mem::IsValidRange(dst, 1) || !mem::IsValidRange(src, 1)) return SCE_KERNEL_ERROR_INVALID_POINTER;
	if (dst + size >= 0x80000000u || src + size >= 0x80000000u || size >= 0x80000000u)
		return SCE_KERNEL_ERROR_PRIV_REQUIRED;
	return 0;
}

u32 DmacCopy(u32 dst, u32 src, u32 size) {
	if (mem::IsValidRange(dst, size) && mem::IsValidRange(src, size)) mem::Memcpy(dst, src, size);
	// The caller waits for the transfer: about 236 bytes/us for copies of 272 bytes and up
	// (measured on hardware, as in PPSSPP's sceDmac); other threads run meanwhile.
	if (size >= 272) {
		const u64 us = size / 236;
		g_dmacDeadline = sched::NowCycles() + us * (sched::kCpuHz / 1000000);
		if (sched::Current() && sched::DispatchEnabled() && !sched::InInterrupt())
			sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
	}
	return 0;
}

u32 sceDmacMemcpy(u32 dst, u32 src, u32 size) {
	if (u32 err = CheckDmacArgs(dst, src, size)) return err;
	// An overlapping copy should wait for the previous one to finish (it doesn't, as in PPSSPP).
	return DmacCopy(dst, src, size);
}

u32 sceDmacTryMemcpy(u32 dst, u32 src, u32 size) {
	if (u32 err = CheckDmacArgs(dst, src, size)) return err;
	if (g_dmacDeadline > sched::NowCycles()) return SCE_KERNEL_ERROR_BUSY;
	return DmacCopy(dst, src, size);
}

int sceHprmPeekCurrentKey(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, 0);
	return 0;
}
int sceHprmZero() { return 0; }
int sceHprmLatch(u32 ptr) {
	if (mem::IsValidRange(ptr, 16)) mem::Memset(ptr, 0, 16);
	return 0;
}
int sceHprmRegisterCallback(SceUID) { return 0; }

}  // namespace

namespace systemparam {
int Language() { return g_language; }
int ButtonMode() { return g_buttonMode; }
}  // namespace systemparam

void Register_sceImpose() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x36AA6E91, sceImposeSetLanguageMode),
		HLEFN(0x24FD7BCF, sceImposeGetLanguageMode),
		HLEFN(0x8C943191, sceImposeGetBatteryIconStatus),
		HLEFN(0x72189C48, sceImposeSetUMDPopup),
		HLEFN(0xE0887BC8, sceImposeGetUMDPopup),
		HLEFN(0x8F6E3518, sceImposeGetBacklightOffTime),
		HLEFN(0x967F6D4A, sceImposeSetBacklightOffTime),
	};
	hle::RegisterModule("sceImpose", funcs);
}

void Register_sceDmac() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x617F3FE6, sceDmacMemcpy),
		HLEFN(0xD97F94D8, sceDmacTryMemcpy),
	};
	hle::RegisterModule("sceDmac", funcs);
}

void Register_sceHprm() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x1910B327, sceHprmPeekCurrentKey),
		HLEFN(0x208DB1BD, sceHprmZero),
		HLEFN(0x7E69EDA4, sceHprmZero),
		HLEFN(0x219C58F1, sceHprmZero),
		HLEFN(0xC7154136, sceHprmRegisterCallback),
		HLEFN(0xFD7DE6CD, sceHprmRegisterCallback),
		HLEFN(0x2BCEC83E, sceHprmLatch),
		HLEFN(0x40D2F9F0, sceHprmLatch),
	};
	hle::RegisterModule("sceHprm", funcs);
}
