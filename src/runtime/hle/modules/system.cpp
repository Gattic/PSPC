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

// sceImpose_driver: the settings the VSH's volume/brightness overlay keeps (impose.prx). After
// PPSSPP's sceImpose.cpp: the parameter ids, the values reported and the change mask that
// sceImposeChanges hands out. 0x40000000 is a real setting on every firmware up to 5.55 (the
// 3.xx VSH blanks the display if it is rejected).
enum : u32 {
	IMPOSE_MAIN_VOLUME = 0x1,
	IMPOSE_BACKLIGHT_BRIGHTNESS = 0x2,
	IMPOSE_EQUALIZER_MODE = 0x4,
	IMPOSE_MUTE = 0x8,
	IMPOSE_AVLS = 0x10,
	IMPOSE_TIME_FORMAT = 0x20,
	IMPOSE_DATE_FORMAT = 0x40,
	IMPOSE_LANGUAGE = 0x80,
	IMPOSE_00000100 = 0x100,
	IMPOSE_BACKLIGHT_OFF_INTERVAL = 0x200,
	IMPOSE_SOUND_REDUCTION = 0x400,
};
u32 g_imposeChanges = 0, g_imposeAvls = 0, g_imposeVolume = 30, g_imposeBrightness = 3;

int sceImposeGetParam(u32 param) {
	switch (param) {
	case IMPOSE_MAIN_VOLUME: return (int)g_imposeVolume;  // 0-30
	case IMPOSE_BACKLIGHT_BRIGHTNESS: return (int)g_imposeBrightness;
	case IMPOSE_AVLS: return (int)g_imposeAvls;
	case IMPOSE_MUTE:
	case IMPOSE_SOUND_REDUCTION:
	case IMPOSE_BACKLIGHT_OFF_INTERVAL:
	case IMPOSE_EQUALIZER_MODE:
	case IMPOSE_TIME_FORMAT:
	case IMPOSE_DATE_FORMAT:
	case IMPOSE_LANGUAGE:
	case IMPOSE_00000100:
	case 0x20000000:
	case 0x40000000: return 0;
	default:
		if (param >= 0x80000001 && param <= 0x8000000B) return 0;
		return (int)SCE_KERNEL_ERROR_INVALID_MODE;
	}
}

int sceImposeSetParam(u32 param, int value) {
	switch (param) {
	case IMPOSE_AVLS:
		if (value < 0 || value > 1) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
		g_imposeAvls = (u32)value;
		g_imposeChanges |= IMPOSE_AVLS | IMPOSE_MAIN_VOLUME;
		break;
	case IMPOSE_MAIN_VOLUME:
		if (value < 0 || value >= 31) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
		g_imposeVolume = (u32)value;
		g_imposeChanges |= IMPOSE_MAIN_VOLUME;
		break;
	case IMPOSE_BACKLIGHT_BRIGHTNESS:
		g_imposeBrightness = (u32)value;
		g_imposeChanges |= param;
		break;
	default: g_imposeChanges |= param; break;
	}
	return 0;
}

int sceImposeChanges() {
	u32 c = g_imposeChanges;
	g_imposeChanges = 0;
	return (int)c;
}

int sceImposeSetStatus(int) { return 0; }
// No TV cable: nothing to report, nothing to switch.
int sceImposeCheckVideoOut(u32 outPtr) {
	if (mem::IsValidRange(outPtr, 4)) GWrite32(outPtr, 0);
	return 0;
}
int sceImposeSetVideoOutMode(int, int, int) { return 0; }
int sceImpose_FF1A2F07(int) { return 0; }

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

	// Kernel NIDs as of 1.50 (data/m33_nid_resolver.csv adds the 4.01 ones).
	static const hle::HleFunction driver[] = {
		HLEFN(0x531C9778, sceImposeGetParam),
		HLEFN(0x810FB7FB, sceImposeSetParam),
		HLEFN(0xB415FC59, sceImposeChanges),
		HLEFN(0x116CFF64, sceImposeCheckVideoOut),
		HLEFN(0x116DDED6, sceImposeSetVideoOutMode),
		HLEFN(0xFF1A2F07, sceImpose_FF1A2F07),
		// sceImposeSetStatus has the same NID on 3.95-4.05 (PPSSPP).
		HLEFN(0x8434B075, sceImposeSetStatus),
		HLEFN(0x24FD7BCF, sceImposeGetLanguageMode),
		HLEFN(0x36AA6E91, sceImposeSetLanguageMode),
		HLEFN(0x8C943191, sceImposeGetBatteryIconStatus),
	};
	hle::RegisterModule("sceImpose_driver", driver);
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
