// SPDX-License-Identifier: GPL-2.0-or-later
// sceCtrl: buttons/analog sampled at every VBlank from the host input.
#include <deque>

#include "runtime/hle/display.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace {

struct SceCtrlData {
	u32 timeStamp;
	u32 buttons;
	u8 lx, ly;
	u8 rsrv[6];
};
static_assert(sizeof(SceCtrlData) == 16, "SceCtrlData");

struct Sample {
	u32 time;
	u32 buttons;
	u8 lx, ly;
};

constexpr u32 CTRL_MASK_USER = 0x0000F3F9;  // buttons user mode can see
int g_samplingMode = 0;                     // 0 = digital, 1 = analog
int g_samplingCycle = 0;
int g_idleReset = -1, g_idleBack = -1;
std::deque<Sample> g_samples;
u32 g_latchMake = 0, g_latchBreak = 0, g_latchPress = 0, g_latchRelease = 0, g_prevButtons = 0;
u32 g_lastVcountSampled = 0xFFFFFFFF;

void TakeSample() {
	host::CtrlState s = host::GetCtrl();
	Sample smp;
	smp.time = (u32)sched::NowUs();
	smp.buttons = s.buttons;
	smp.lx = g_samplingMode ? s.lx : 128;
	smp.ly = g_samplingMode ? s.ly : 128;
	u32 changed = smp.buttons ^ g_prevButtons;
	g_latchMake |= smp.buttons & changed;
	g_latchBreak |= g_prevButtons & changed;
	g_latchPress |= smp.buttons;
	g_latchRelease |= ~smp.buttons;
	g_prevButtons = smp.buttons;
	g_samples.push_back(smp);
	while (g_samples.size() > 64) g_samples.pop_front();
}

void Fill(u32 ptr, int count, bool negative) {
	if (g_samples.empty()) TakeSample();
	int avail = (int)g_samples.size();
	if (count > avail) count = avail;
	for (int i = 0; i < count; i++) {
		const Sample &s = g_samples[g_samples.size() - count + i];
		SceCtrlData d{};
		d.timeStamp = s.time;
		d.buttons = negative ? ~s.buttons : s.buttons;
		d.lx = s.lx;
		d.ly = s.ly;
		mem::WriteBytes(ptr + i * 16, &d, sizeof(d));
	}
}

int Peek(u32 ptr, int count, bool negative) {
	if (count < 0 || count > 64) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	if (!mem::IsValidRange(ptr, (u32)count * 16)) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	Fill(ptr, count, negative);
	return count;
}

int Read(u32 ptr, int count, bool negative) {
	if (count < 0 || count > 64) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	if (!mem::IsValidRange(ptr, (u32)count * 16)) return (int)SCE_KERNEL_ERROR_INVALID_POINTER;
	// Reads block until the next sample unless one arrived since the last read.
	if (g_lastVcountSampled == display::Vcount()) display::WaitVblankStart(1, false);
	g_lastVcountSampled = display::Vcount();
	Fill(ptr, count, negative);
	return count;
}

int sceCtrlSetSamplingCycle(int cycle) {
	if ((cycle > 0 && cycle < 5555) || cycle > 20000) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	int old = g_samplingCycle;
	g_samplingCycle = cycle;
	return old;
}
int sceCtrlGetSamplingCycle(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, (u32)g_samplingCycle);
	return 0;
}
int sceCtrlSetSamplingMode(int mode) {
	if (mode < 0 || mode > 1) return (int)SCE_KERNEL_ERROR_INVALID_MODE;
	int old = g_samplingMode;
	g_samplingMode = mode;
	return old;
}
int sceCtrlGetSamplingMode(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, (u32)g_samplingMode);
	return 0;
}
int sceCtrlPeekBufferPositive(u32 p, int n) { return Peek(p, n, false); }
int sceCtrlPeekBufferNegative(u32 p, int n) { return Peek(p, n, true); }
int sceCtrlReadBufferPositive(u32 p, int n) { return Read(p, n, false); }
int sceCtrlReadBufferNegative(u32 p, int n) { return Read(p, n, true); }

static int Latch(u32 ptr, bool reset) {
	if (mem::IsValidRange(ptr, 16)) {
		GWrite32(ptr, g_latchMake);
		GWrite32(ptr + 4, g_latchBreak);
		GWrite32(ptr + 8, g_latchPress);
		GWrite32(ptr + 12, g_latchRelease);
	}
	if (reset) g_latchMake = g_latchBreak = g_latchPress = g_latchRelease = 0;
	return 1;
}
int sceCtrlPeekLatch(u32 p) { return Latch(p, false); }
int sceCtrlReadLatch(u32 p) {
	if (g_lastVcountSampled == display::Vcount()) display::WaitVblankStart(1, false);
	g_lastVcountSampled = display::Vcount();
	return Latch(p, true);
}
int sceCtrlSetIdleCancelThreshold(int reset, int back) {
	if (reset < -1 || back < -1 || reset > 128 || back > 128) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	g_idleReset = reset;
	g_idleBack = back;
	return 0;
}
int sceCtrlGetIdleCancelThreshold(u32 resetPtr, u32 backPtr) {
	if (mem::IsValidRange(resetPtr, 4)) GWrite32(resetPtr, (u32)g_idleReset);
	if (mem::IsValidRange(backPtr, 4)) GWrite32(backPtr, (u32)g_idleBack);
	return 0;
}

}  // namespace

namespace ctrl {
void OnVblank() { TakeSample(); }
}  // namespace ctrl

void Register_sceCtrl() {
	static const hle::HleFunction funcs[] = {
		HLESTUB(0x3E65A0EA, "sceCtrlInit"),
		HLEFN(0x1F4011E6, sceCtrlSetSamplingMode),
		HLEFN(0x6A2774F3, sceCtrlSetSamplingCycle),
		HLEFN(0x02BAAD91, sceCtrlGetSamplingCycle),
		HLEFN(0xDA6B76A1, sceCtrlGetSamplingMode),
		HLEFN(0x1F803938, sceCtrlReadBufferPositive),
		HLEFN(0x3A622550, sceCtrlPeekBufferPositive),
		HLEFN(0xC152080A, sceCtrlPeekBufferNegative),
		HLEFN(0x60B81F86, sceCtrlReadBufferNegative),
		HLEFN(0xB1D0E5CD, sceCtrlPeekLatch),
		HLEFN(0x0B588501, sceCtrlReadLatch),
		HLEFN(0xA7144800, sceCtrlSetIdleCancelThreshold),
		HLEFN(0x687660FA, sceCtrlGetIdleCancelThreshold),
		HLEFN(0xC4AAD55F, sceCtrlPeekBufferPositive),
		HLEFN(0x454455AC, sceCtrlReadBufferPositive),
	};
	hle::RegisterModule("sceCtrl", funcs);
	hle::RegisterModule("sceCtrl_driver", funcs);
}
