// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceDisplay: framebuffer selection, VBlank timing and presentation of the selected framebuffer to the
// host window at each VBlank. Timing as PPSSPP measured it: a frame is 1001/60 ms, starting when the
// vblank does; the vblank lasts 0.77 ms and the hcount counts 286 lines a frame from 1.
#include <algorithm>
#include <cmath>
#include <vector>

#include "common/log.h"
#include "runtime/hle/display.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/interrupts.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace display {

namespace {

constexpr double kFrameUs = 1001000.0 / 60.0;
// tests/display/vblanklen: 730-770us from sceDisplayWaitVblankStart returning until the vblank ends.
constexpr double kVblankUs = 770.0;
// sceDisplayIsVsync's window into the frame (guesses from tests).
constexpr double kVsyncStartUs = 592.5;
constexpr double kVsyncEndUs = 726.5;
constexpr int kLinesPerFrame = 286;
constexpr u64 kCyclesPerLine = sched::kCpuHz / 60 / kLinesPerFrame;
constexpr u64 kCyclesPerUs = sched::kCpuHz / 1000000;

struct FrameBuf {
	u32 addr = 0x04000000;
	int stride = 512;
	int format = 3;
};

FrameBuf g_current, g_latched;
bool g_latchPending = false;
bool g_fbSet = false;  // the game has chosen a framebuffer
bool g_hasSetMode = false;
int g_mode = 0, g_width = 480, g_height = 272;
u32 g_vcount = 0;
u32 g_hcountBase = 0;  // the accumulated hcount at the start of this frame
bool g_inVblank = false;
double g_bootUs = 0;
double g_frameStartUs = 0;  // when the last vblank started
u64 g_frames = 0;           // vblanks so far
int g_brightness = 84;
u32 g_holdMode = 0, g_resumeMode = 0;
WaitQueue g_vblankWaiters;
std::vector<u32> g_rgba;
std::function<void()> g_vblankHook;

u64 EnterUs(u64 frame) { return (u64)std::llround(g_bootUs + kFrameUs - kVblankUs + (double)frame * kFrameUs); }

void Present() {
	if (host::IsHeadless() || !g_current.addr) return;
	ConvertFramebuffer(g_current.addr, g_current.stride, g_current.format, 480, 272, g_rgba);
	host::PresentFrame(g_rgba.data(), 480, 272, 480);
}

void OnLeaveVblank() {
	g_inVblank = false;
}

void OnEnterVblank() {
	g_frameStartUs = (double)EnterUs(g_frames);
	g_frames++;
	g_inVblank = true;
	g_vcount++;
	g_hcountBase += kLinesPerFrame;
	if (g_hcountBase > 0x7FFFFFFF) g_hcountBase -= 0x80000000;
	sched::ScheduleEvent((u64)std::llround(g_frameStartUs + kVblankUs), OnLeaveVblank);

	if (g_latchPending) {
		g_current = g_latched;
		g_latchPending = false;
	}
	Present();
	if (g_vblankHook) g_vblankHook();

	// The waiters for this vblank are released ~48us after it, plus ~9us for each one beyond the first
	// (pspautotests threads/scheduling/vblankwake). Which vblank a wait is for is decided now.
	g_vblankWaiters.Cleanup(WAITTYPE_VBLANK, 0);
	std::vector<SceUID> waiters = g_vblankWaiters.threads, release;
	g_vblankWaiters.threads.clear();
	for (SceUID id : waiters) {
		PspThread *t = Get<PspThread>(id);
		if (!t) continue;
		if (t->waitValue[0] > 1) {
			t->waitValue[0]--;
			g_vblankWaiters.threads.push_back(id);
			continue;
		}
		release.push_back(id);
	}
	if (!release.empty()) {
		const double at = g_frameStartUs + 48.0 + 9.0 * (double)(release.size() - 1);
		sched::ScheduleEvent((u64)std::llround(at), [release]() {
			for (SceUID id : release) {
				PspThread *t = Get<PspThread>(id);
				if (WaitQueue::IsWaitingOn(t, WAITTYPE_VBLANK, 0)) sched::Wake(t, 0);
			}
		});
	}

	sched::QueueInterrupt([]() { intr::TriggerAll(intr::PSP_VBLANK_INT); });
	sched::ScheduleEvent(EnterUs(g_frames), OnEnterVblank);
}

}  // namespace

void Init() {
	g_current = g_latched = FrameBuf{};
	g_latchPending = false;
	g_fbSet = false;
	g_hasSetMode = false;
	g_mode = 0;
	g_width = 480;
	g_height = 272;
	g_vcount = 0;
	g_hcountBase = 0;
	g_inVblank = false;
	g_frames = 0;
	g_vblankWaiters.threads.clear();
	g_bootUs = (double)sched::NowUs();
	g_frameStartUs = g_bootUs;
	sched::ScheduleEvent(EnterUs(0), OnEnterVblank);
}

void SetVblankHook(std::function<void()> fn) { g_vblankHook = std::move(fn); }

void CurrentFramebuffer(u32 &addr, int &stride, int &format) {
	addr = g_fbSet ? g_current.addr : 0;
	stride = g_current.stride;
	format = g_current.format;
}

u32 Vcount() { return g_vcount; }

bool InVblank() { return g_inVblank; }

u32 CurrentHcount() {
	const double into = std::max(0.0, (double)sched::NowUs() - g_frameStartUs);
	// Can't seem to produce a 0 on real hardware, offsetting by 1 makes things look right (PPSSPP).
	return 1 + (u32)((u64)(into * (double)kCyclesPerUs) / kCyclesPerLine);
}

void ConvertFramebuffer(u32 addr, int stride, int format, int width, int height, std::vector<u32> &out) {
	out.resize((size_t)width * height);
	int bpp = format == 3 ? 4 : 2;
	for (int y = 0; y < height; y++) {
		u32 row = addr + (u32)(y * stride * bpp);
		const u8 *src = mem::GetPointer(row, (u32)(width * bpp));
		u32 *dst = &out[(size_t)y * width];
		if (!src) {
			std::fill(dst, dst + width, 0xFF000000u);
			continue;
		}
		if (format == 3) {
			for (int x = 0; x < width; x++) {
				u32 p;
				memcpy(&p, src + x * 4, 4);
				dst[x] = p | 0xFF000000u;
			}
			continue;
		}
		for (int x = 0; x < width; x++) {
			u16 p = (u16)(src[x * 2] | (src[x * 2 + 1] << 8));
			u32 r, g, b;
			switch (format) {
			case 0:  // 565
				r = p & 0x1F;
				g = (p >> 5) & 0x3F;
				b = (p >> 11) & 0x1F;
				r = (r << 3) | (r >> 2);
				g = (g << 2) | (g >> 4);
				b = (b << 3) | (b >> 2);
				break;
			case 1:  // 5551
				r = p & 0x1F;
				g = (p >> 5) & 0x1F;
				b = (p >> 10) & 0x1F;
				r = (r << 3) | (r >> 2);
				g = (g << 3) | (g >> 2);
				b = (b << 3) | (b >> 2);
				break;
			default:  // 4444
				r = (p & 0xF) * 17;
				g = ((p >> 4) & 0xF) * 17;
				b = ((p >> 8) & 0xF) * 17;
				break;
			}
			dst[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
		}
	}
}

int WaitVblankStart(int count, bool cb) {
	PspThread *t = sched::Current();
	if (!t) return 0;
	// These calls take ~115us: a vblank closer than that is missed, and the wait is for the next
	// (on hardware a wait from >= 16500us into the frame waits for two).
	const double untilNext = (double)EnterUs(g_frames) - (double)sched::NowUs();
	if (untilNext <= 115.0) count++;
	t->waitValue[0] = (u32)std::max(count, 1);
	g_vblankWaiters.Add(t->uid);
	return sched::Wait(WAITTYPE_VBLANK, 0, 0, cb);
}

}  // namespace display

namespace {

using namespace display;

int CheckCanWait() {
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	return 0;
}

u32 sceDisplaySetMode(int mode, int width, int height) {
	if (mode != 0) return SCE_KERNEL_ERROR_INVALID_MODE;
	if (width != 480 || height != 272) return SCE_KERNEL_ERROR_INVALID_SIZE;
	g_hasSetMode = true;
	g_mode = mode;
	g_width = width;
	g_height = height;
	// It waits for a vblank start, where it can (pspautotests intr/waits).
	if (!sched::InInterrupt() && sched::DispatchEnabled()) WaitVblankStart(1, false);
	return 0;
}

int sceDisplayGetMode(u32 modePtr, u32 wPtr, u32 hPtr) {
	if (mem::IsValidRange(modePtr, 4)) GWrite32(modePtr, (u32)g_mode);
	if (mem::IsValidRange(wPtr, 4)) GWrite32(wPtr, (u32)g_width);
	if (mem::IsValidRange(hPtr, 4)) GWrite32(hPtr, (u32)g_height);
	return 0;
}

u32 sceDisplaySetFrameBuf(u32 topaddr, int linesize, int pixelformat, int sync) {
	if (sync != 0 && sync != 1) return SCE_KERNEL_ERROR_INVALID_MODE;
	const u32 a = topaddr & 0x3FFFFFFF;
	const bool vram = a >= 0x04000000 && a < 0x04800000;
	if (topaddr != 0 && !mem::IsRam(topaddr) && !vram) return SCE_KERNEL_ERROR_INVALID_POINTER;
	if (topaddr & 0xF) return SCE_KERNEL_ERROR_INVALID_POINTER;
	if ((linesize & 0x3F) != 0 || (linesize == 0 && topaddr != 0)) return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (pixelformat < 0 || pixelformat > 3) return SCE_KERNEL_ERROR_INVALID_FORMAT;
	// An immediate change can't change the format or stride: those go through the latch first.
	if (sync == 0 && (pixelformat != g_latched.format || linesize != g_latched.stride))
		return SCE_KERNEL_ERROR_INVALID_MODE;
	sched::ChargeCycles(290);
	FrameBuf fb;
	fb.addr = topaddr;
	fb.stride = linesize;
	fb.format = pixelformat;
	g_fbSet = true;
	if (sync == 0) {
		g_current = fb;
		g_latched = fb;
		g_latchPending = false;
	} else {
		g_latched = fb;
		g_latchPending = true;
		// The format and stride apply to the current framebuffer at once.
		g_current.format = fb.format;
		g_current.stride = fb.stride;
	}
	return 0;
}

u32 sceDisplayGetFrameBuf(u32 topPtr, u32 linePtr, u32 fmtPtr, int sync) {
	const FrameBuf &fb = sync == 1 ? g_latched : g_current;
	if (mem::IsValidRange(topPtr, 4)) GWrite32(topPtr, fb.addr);
	if (mem::IsValidRange(linePtr, 4)) GWrite32(linePtr, (u32)fb.stride);
	if (mem::IsValidRange(fmtPtr, 4)) GWrite32(fmtPtr, (u32)fb.format);
	return 0;
}

int sceDisplayWaitVblankStart() {
	if (int err = CheckCanWait()) return err;
	return WaitVblankStart(1, false);
}
int sceDisplayWaitVblankStartCB() {
	if (int err = CheckCanWait()) return err;
	return WaitVblankStart(1, true);
}
int sceDisplayWaitVblankStartMulti(int n) {
	if (n <= 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	if (int err = CheckCanWait()) return err;
	return WaitVblankStart(n, false);
}
int sceDisplayWaitVblankStartMultiCB(int n) {
	if (n <= 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	if (int err = CheckCanWait()) return err;
	return WaitVblankStart(n, true);
}
// In a vblank already: no wait, and 1 (even in an interrupt handler; pspautotests intr/waits).
int WaitVblank(bool cb) {
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (!InVblank()) {
		if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
		WaitVblankStart(1, cb);
		return 0;
	}
	sched::ChargeCycles(1110);
	sched::Reschedule();
	return 1;
}
int sceDisplayWaitVblank() { return WaitVblank(false); }
int sceDisplayWaitVblankCB() { return WaitVblank(true); }

float sceDisplayGetFramePerSec() { return 59.9400599f; }
u32 sceDisplayGetVcount() {
	sched::ChargeCycles(150);
	sched::Reschedule();
	return Vcount();
}
u32 sceDisplayGetCurrentHcount() {
	sched::ChargeCycles(275);
	return CurrentHcount();
}
// Always a positive int: wraps from 0x7FFFFFFF to 0.
u32 AccumulatedHcount() { return (g_hcountBase + CurrentHcount()) & 0x7FFFFFFF; }
u32 sceDisplayGetAccumulatedHcount() {
	const u32 v = AccumulatedHcount();
	sched::ChargeCycles(235);
	return v;
}
int sceDisplayAdjustAccumulatedHcount(int value) {
	if (value < 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	g_hcountBase += (u32)value - AccumulatedHcount();
	return 0;
}
int sceDisplayIsVblank() { return InVblank() ? 1 : 0; }
int sceDisplayIsVsync() {
	const double into = (double)sched::NowUs() - g_frameStartUs;
	return into >= kVsyncStartUs && into <= kVsyncEndUs ? 1 : 0;
}
int sceDisplayIsForeground() { return g_hasSetMode && g_current.addr ? 1 : 0; }
int sceDisplaySetHoldMode(u32 mode) {
	g_holdMode = mode;
	return 0;
}
int sceDisplaySetResumeMode(u32 mode) {
	g_resumeMode = mode;
	return 0;
}
int sceDisplayGetResumeMode(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, g_resumeMode);
	return 0;
}
int sceDisplayGetBrightness(u32 levelPtr, u32 unkPtr) {
	if (mem::IsValidRange(levelPtr, 4)) GWrite32(levelPtr, (u32)g_brightness);
	if (mem::IsValidRange(unkPtr, 4)) GWrite32(unkPtr, 0);
	return 0;
}
int sceDisplaySetBrightness(int level, int unk) {
	(void)unk;
	g_brightness = level;
	return 0;
}

}  // namespace

void Register_sceDisplay() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x0E20F177, sceDisplaySetMode),
		HLEFN(0x289D82FE, sceDisplaySetFrameBuf),
		HLEFN(0xEEDA2E54, sceDisplayGetFrameBuf),
		HLEFN(0x36CDFADE, sceDisplayWaitVblank),
		HLEFN(0x984C27E7, sceDisplayWaitVblankStart),
		HLEFN(0x40F1469C, sceDisplayWaitVblankStartMulti),
		HLEFN(0x8EB9EC49, sceDisplayWaitVblankCB),
		HLEFN(0x46F186C3, sceDisplayWaitVblankStartCB),
		HLEFN(0x77ED8B3A, sceDisplayWaitVblankStartMultiCB),
		HLEFN(0xDBA6C4C4, sceDisplayGetFramePerSec),
		HLEFN(0x773DD3A3, sceDisplayGetCurrentHcount),
		HLEFN(0x210EAB3A, sceDisplayGetAccumulatedHcount),
		HLEFN(0xA83EF139, sceDisplayAdjustAccumulatedHcount),
		HLEFN(0x9C6EAAD7, sceDisplayGetVcount),
		HLEFN(0xDEA197D4, sceDisplayGetMode),
		HLEFN(0x7ED59BC4, sceDisplaySetHoldMode),
		HLEFN(0xA544C486, sceDisplaySetResumeMode),
		HLEFN(0xBF79F646, sceDisplayGetResumeMode),
		HLEFN(0xB4F378FA, sceDisplayIsForeground),
		HLEFN(0x31C4BAA8, sceDisplayGetBrightness),
		HLEFN(0x9E3C6DC6, sceDisplaySetBrightness),
		HLEFN(0x4D4E10EC, sceDisplayIsVblank),
		HLEFN(0x21038913, sceDisplayIsVsync),
	};
	hle::RegisterModule("sceDisplay", funcs);
	hle::RegisterModule("sceDisplay_driver", funcs);
}
