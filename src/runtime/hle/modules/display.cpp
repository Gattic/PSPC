// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceDisplay: framebuffer selection, VBlank timing (59.94 Hz, 286 lines of which 272 visible)
// and presentation of the selected framebuffer to the host window at each VBlank.
#include <algorithm>
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

constexpr double kFrameUs = 1000000.0 / 59.9400599;
constexpr int kLinesPerFrame = 286;
constexpr int kVisibleLines = 272;

struct FrameBuf {
	u32 addr = 0;
	int stride = 512;
	int format = 3;
};

FrameBuf g_current, g_latched;
bool g_latchPending = false;
int g_mode = 0, g_width = 480, g_height = 272;
u32 g_vcount = 0;
u64 g_bootUs = 0;
int g_brightness = 84;
WaitQueue g_vblankWaiters;
std::vector<u32> g_rgba;
std::function<void()> g_vblankHook;

u64 FrameStart(u64 frame) { return g_bootUs + (u64)(frame * kFrameUs); }
u64 VblankStart(u64 frame) { return FrameStart(frame) + (u64)(kFrameUs * kVisibleLines / kLinesPerFrame); }

void Present() {
	if (host::IsHeadless() || !g_current.addr) return;
	ConvertFramebuffer(g_current.addr, g_current.stride, g_current.format, 480, 272, g_rgba);
	host::PresentFrame(g_rgba.data(), 480, 272, 480);
}

void OnVblank() {
	g_vcount++;
	if (g_latchPending) {
		g_current = g_latched;
		g_latchPending = false;
	}
	Present();
	if (g_vblankHook) g_vblankHook();
	g_vblankWaiters.Cleanup(WAITTYPE_VBLANK, 0);
	std::vector<SceUID> waiters = g_vblankWaiters.threads;
	g_vblankWaiters.threads.clear();
	for (SceUID id : waiters) {
		PspThread *t = Get<PspThread>(id);
		if (!t) continue;
		if (t->waitValue[0] > 1) {
			t->waitValue[0]--;
			g_vblankWaiters.threads.push_back(id);
			continue;
		}
		sched::Wake(t, 0);
	}
	sched::QueueInterrupt([]() { intr::TriggerAll(intr::PSP_VBLANK_INT); });
	sched::ScheduleEvent(VblankStart(g_vcount), OnVblank);
}

}  // namespace

void Init() {
	g_current = g_latched = FrameBuf{};
	g_latchPending = false;
	g_vcount = 0;
	g_vblankWaiters.threads.clear();
	g_bootUs = sched::NowUs();
	sched::ScheduleEvent(VblankStart(0), OnVblank);
}

void SetVblankHook(std::function<void()> fn) { g_vblankHook = std::move(fn); }

u32 Vcount() { return g_vcount; }

bool InVblank() {
	u64 now = sched::NowUs();
	double inFrame = std::fmod((double)(now - g_bootUs), kFrameUs);
	return inFrame >= kFrameUs * kVisibleLines / kLinesPerFrame;
}

u32 CurrentHcount() {
	u64 now = sched::NowUs();
	double inFrame = std::fmod((double)(now - g_bootUs), kFrameUs);
	return (u32)(inFrame / kFrameUs * kLinesPerFrame);
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
	t->waitValue[0] = (u32)std::max(count, 1);
	g_vblankWaiters.Add(t->uid);
	return sched::Wait(WAITTYPE_VBLANK, 0, 0, cb);
}

}  // namespace display

namespace {

using namespace display;

int sceDisplaySetMode(int mode, int width, int height) {
	if (mode != 0 || width != 480 || height != 272) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	g_mode = mode;
	g_width = width;
	g_height = height;
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
	if (topaddr != 0 && (!mem::IsValidAddress(topaddr) || (topaddr & 0xF))) return SCE_KERNEL_ERROR_INVALID_POINTER;
	if ((linesize & 0x3F) != 0 || (linesize == 0 && topaddr != 0)) return SCE_KERNEL_ERROR_INVALID_SIZE;
	if (pixelformat < 0 || pixelformat > 3) return SCE_KERNEL_ERROR_INVALID_FORMAT;
	FrameBuf fb{topaddr, linesize, pixelformat};
	if (sync == 0) {
		g_current = fb;
		g_latchPending = false;
	} else {
		g_latched = fb;
		g_latchPending = true;
	}
	return 0;
}

u32 sceDisplayGetFrameBuf(u32 topPtr, u32 linePtr, u32 fmtPtr, int sync) {
	const FrameBuf &fb = (sync == 1 && g_latchPending) ? g_latched : g_current;
	if (mem::IsValidRange(topPtr, 4)) GWrite32(topPtr, fb.addr);
	if (mem::IsValidRange(linePtr, 4)) GWrite32(linePtr, (u32)fb.stride);
	if (mem::IsValidRange(fmtPtr, 4)) GWrite32(fmtPtr, (u32)fb.format);
	return 0;
}

int sceDisplayWaitVblankStart() { return WaitVblankStart(1, false); }
int sceDisplayWaitVblankStartCB() { return WaitVblankStart(1, true); }
int sceDisplayWaitVblankStartMulti(int n) {
	if (n <= 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	return WaitVblankStart(n, false);
}
int sceDisplayWaitVblankStartMultiCB(int n) {
	if (n <= 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	return WaitVblankStart(n, true);
}
int sceDisplayWaitVblank() {
	if (InVblank()) return 1;
	return WaitVblankStart(1, false);
}
int sceDisplayWaitVblankCB() {
	if (InVblank()) return 1;
	return WaitVblankStart(1, true);
}

float sceDisplayGetFramePerSec() { return 59.9400599f; }
u32 sceDisplayGetVcount() { return Vcount(); }
u32 sceDisplayGetCurrentHcount() { return CurrentHcount(); }
u32 sceDisplayGetAccumulatedHcount() { return Vcount() * kLinesPerFrame + CurrentHcount(); }
int sceDisplayAdjustAccumulatedHcount(int value) {
	if (value < 0) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	return 0;
}
int sceDisplayIsVblank() { return InVblank() ? 1 : 0; }
int sceDisplayIsVsync() { return InVblank() ? 1 : 0; }
int sceDisplayIsForeground() { return g_current.addr ? 1 : 0; }
int sceDisplaySetHoldMode(u32) { return 0; }
int sceDisplaySetResumeMode(u32) { return 0; }
int sceDisplayGetResumeMode(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, 0);
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
