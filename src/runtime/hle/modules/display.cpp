// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceDisplay: framebuffer selection, VBlank timing and presentation of the selected framebuffer to the
// host window at each VBlank, with the sceUtility dialogs and the HOME menu drawn over it (ppge). A
// frame is 1001/60 ms of 286 lines, starting with the vblank interrupt at the end of the last line
// (pspautotests display/vblankphase: the vblank flag is on from there for ~818 us, the hcount wraps
// to 0 in the interrupt).
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "common/log.h"
#if PSPC_HAVE_GPU
#include "gpu/backend.h"
#endif
#include "runtime/dialogs/home_menu.h"
#include "runtime/dialogs/ppge.h"
#include "runtime/hle/display.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/interrupts.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace display {

namespace {

constexpr double kFrameUs = 1001000.0 / 60.0;
// How long the vblank flag stays on after the interrupt: line 14 just starts in it (the hcount is
// seen at 13 or 14 last: display/hcount, vblanklen) and a waiter sees 730-740us of it.
constexpr double kVblankUs = 816.0;
// Where the first vblank falls after boot. On hardware a test starts anywhere in a frame; the
// recordings that depend on it agree with 0-1000us (ctrl/vblank, threads/fpl/tryallocate,
// threads/msgpipe/delete, threads/scheduling/callcosts).
constexpr double kFirstVblankUs = 500.0;
// The vblank interrupt's own time (display/vblankphase, threads/scheduling/vblankwake): ~28us
// before the handlers, ~12us for each (9 on top of what the handler's own calls cost), ~36us after. The first thread it woke then gets the CPU
// ~11us later (~1.5us more for each other one woken), give or take a few us, and returning from
// sceDisplayWaitVblankStart takes ~6us: a lone waiter runs ~81us after the interrupt, more waiting
// at a better priority than the running thread follow ~12us apart.
constexpr s64 kIntrEntryCycles = 28 * 222, kIntrHandlerCycles = 9 * 222, kIntrExitCycles = 36 * 222;
constexpr s64 kWakeLatencyCycles = 11 * 222, kWakeEachCycles = 333, kWakeJitterCycles = 4 * 222;
constexpr s64 kWaitReturnCycles = 6 * 222;
// sceDisplayIsVsync's window into the frame (guesses from tests).
constexpr double kVsyncStartUs = 592.5;
constexpr double kVsyncEndUs = 726.5;
constexpr int kLinesPerFrame = 286;
constexpr double kLineUs = kFrameUs / kLinesPerFrame;

struct FrameBuf {
	u32 addr = 0x04000000;
	int stride = 512;
	int format = 3;
};

FrameBuf g_current, g_latched;
// The display driver keeps four framebuffers by priority and shows the most urgent one set (0
// first). Games set priority 2 (sceDisplaySetFrameBuf, held in g_current/g_latched above); the
// system's own screens (the HOME screen) use priority 0, and kernel plugins draw over the game
// by setting 0 or 1 through sceDisplaySetFrameBufferInternal and give the screen back with a
// null address.
constexpr int kGamePri = 2;
FrameBuf g_pri[4];
bool g_priSet[4];
// As sceDisplayInit leaves an unused priority: no address or width, format 8888.
constexpr FrameBuf kNoFrameBuf{0, 0, 3};
// The HOME screen's framebuffer (512x272, 5650) in kernel memory, allocated the first time it
// opens and kept: DarkFrost and other cheat menus find it at priority 0 while the HOME screen is
// up and draw their own menus into it later.
u32 g_systemScreen = 0;
constexpr int kSystemScreenFormat = 0;

// What the screen shows: an override above the game's framebuffer if one is set, else the game's.
const FrameBuf &Shown() {
	for (int p = 0; p < kGamePri; p++)
		if (g_priSet[p] && g_pri[p].addr) return g_pri[p];
	if (!g_current.addr && g_priSet[3] && g_pri[3].addr) return g_pri[3];
	return g_current;
}
bool g_latchPending = false;
bool g_fbSet = false;  // the game has chosen a framebuffer
bool g_hasSetMode = false;
int g_mode = 0, g_width = 480, g_height = 272;
u32 g_vcount = 0;
u32 g_hcountBase = 0;  // the accumulated hcount at the start of this frame
double g_bootUs = 0;
double g_frameStartUs = 0;  // when the last vblank started
u64 g_frames = 0;           // vblanks so far
int g_brightness = 84;
u32 g_holdMode = 0, g_resumeMode = 0;
WaitQueue g_vblankWaiters;
std::vector<u32> g_rgba;
std::function<void()> g_vblankHook;

u64 EnterUs(u64 frame) { return (u64)std::llround(g_bootUs + kFirstVblankUs + (double)frame * kFrameUs); }

void Present() {
	const FrameBuf &fb = Shown();
	if (host::IsHeadless() || !fb.addr) return;
	const bool gameScreen = &fb == &g_current;
#if PSPC_HAVE_GPU
	// A GPU renderer presenting to the window itself: its own (upscaled) image of the framebuffer goes
	// there directly, the dialogs and the HOME menu laid over it; for a screenshot it is read back at
	// about the window's size and they are drawn onto it.
	const bool screenshot = host::ScreenshotRequested();
	std::function<void(u32 *, int, int)> draw;
	if (screenshot || (gameScreen && ppge::Visible(ppge::Layer::Dialog)) || ppge::Visible(ppge::Layer::System))
		draw = [gameScreen, screenshot](u32 *px, int w, int h) {
			ppge::Composite(px, w, h, gameScreen);
			if (screenshot) host::SaveScreenshotIfRequested(px, w, h);
		};
	if (gpu::PresentBackendDisplay(fb.addr, fb.stride, fb.format, 480, 272, draw, ppge::Revision(gameScreen), screenshot)) return;
	// Otherwise its image of the framebuffer, read back at about the window's size (or guest memory made
	// current).
	int w = 0, h = 0;
	if (gpu::GetBackendDisplayImage(fb.addr, fb.stride, fb.format, 480, 272, g_rgba, &w, &h, host::FitScale())) {
		ppge::Composite(g_rgba.data(), w, h, gameScreen);
		host::PresentFrame(g_rgba.data(), w, h, w);
		return;
	}
#endif
	ConvertFramebuffer(fb.addr, fb.stride, fb.format, 480, 272, g_rgba);
	ppge::Composite(g_rgba.data(), 480, 272, gameScreen);
	host::PresentFrame(g_rgba.data(), 480, 272, 480);
}

void OnEnterVblank() {
	g_frameStartUs = (double)EnterUs(g_frames);
	g_frames++;
	g_vcount++;
	g_hcountBase += kLinesPerFrame;
	if (g_hcountBase > 0x7FFFFFFF) g_hcountBase -= 0x80000000;

	if (g_latchPending) {
		g_current = g_latched;
		g_latchPending = false;
	}
	homemenu::OnVblank();
	// Replaying obsolete presentations makes a slow GPU fall further behind:
	// each catch-up VBlank would spend more host time displaying the same image.
	// Keep every guest VBlank/interrupt, but only present the current real-time
	// frame. Fast-time runs retain every presentation for deterministic capture.
	if (sched::FastTime() || EnterUs(g_frames) > sched::NowUs())
		Present();
	if (g_vblankHook) g_vblankHook();

	// Which vblank a wait is for is decided now; the interrupt releases the waiters at its end.
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

	sched::QueueInterrupt([release]() {
		sched::ChargeCycles(kIntrEntryCycles);
		intr::TriggerAll(intr::PSP_VBLANK_INT, kIntrHandlerCycles);
		sched::ChargeCycles(kIntrExitCycles);
		int woken = 0;
		for (SceUID id : release) {
			PspThread *t = Get<PspThread>(id);
			if (!WaitQueue::IsWaitingOn(t, WAITTYPE_VBLANK, 0)) continue;
			sched::Wake(t, 0);
			woken++;
		}
		if (woken) {
			sched::SetDispatchLatency(kWakeLatencyCycles + kWakeEachCycles * (woken - 1) +
			                          sched::JitterCycles(kWakeJitterCycles));
		}
	}, intr::PSP_VBLANK_INT);
	sched::ScheduleEvent(EnterUs(g_frames), OnEnterVblank);
}

}  // namespace

void Init() {
	g_current = g_latched = FrameBuf{};
	for (int p = 0; p < 4; p++) {
		g_pri[p] = kNoFrameBuf;
		g_priSet[p] = false;
	}
	g_systemScreen = 0;
	g_latchPending = false;
	g_fbSet = false;
	g_hasSetMode = false;
	g_mode = 0;
	g_width = 480;
	g_height = 272;
	g_vcount = 0;
	g_hcountBase = 0;
	g_frames = 0;
	g_vblankWaiters.threads.clear();
	g_bootUs = (double)sched::NowUs();
	g_frameStartUs = g_bootUs;
	sched::ScheduleEvent(EnterUs(0), OnEnterVblank);
}

void SetVblankHook(std::function<void()> fn) { g_vblankHook = std::move(fn); }

void ShowSystemScreen(bool on) {
	if (!on) {
		// Only if it is still ours: a plugin may have put its own screen there meanwhile.
		if (g_priSet[0] && g_pri[0].addr == g_systemScreen) {
			g_pri[0] = kNoFrameBuf;
			g_priSet[0] = false;
		}
		return;
	}
	constexpr int kStride = 512, kW = 480, kH = 272;
	if (!g_systemScreen) g_systemScreen = kernel::AllocKernelMemory(kStride * kH * 2, "system screen");
	// It starts as a still of the game's screen; the HOME menu is drawn over it.
	const FrameBuf &game = Shown();
	std::vector<u32> rgba;
	if (game.addr) ConvertFramebuffer(game.addr, game.stride, game.format, kW, kH, rgba);
	else rgba.assign((size_t)kW * kH, 0xFF000000u);
	// A sceUtility dialog on screen is part of the game's image on a PSP.
	ppge::Composite(rgba.data(), kW, kH, &game == &g_current);
	u16 *dst = (u16 *)mem::GetPointer(g_systemScreen, kStride * kH * 2);
	if (dst) {
		for (int y = 0; y < kH; y++) {
			for (int x = 0; x < kStride; x++) {
				u16 p = 0;
				if (x < kW) {
					const u32 c = rgba[(size_t)y * kW + x];
					p = (u16)(((c >> 3) & 0x1F) | (((c >> 10) & 0x3F) << 5) | (((c >> 19) & 0x1F) << 11));
				}
				dst[(size_t)y * kStride + x] = p;
			}
		}
	}
	g_pri[0] = FrameBuf{g_systemScreen, kStride, kSystemScreenFormat};
	g_priSet[0] = true;
}

bool ShowingGameScreen() { return &Shown() == &g_current; }

void CurrentFramebuffer(u32 &addr, int &stride, int &format) {
	const FrameBuf &fb = Shown();
	addr = g_fbSet || &fb != &g_current ? fb.addr : 0;
	stride = fb.stride;
	format = fb.format;
}

u32 Vcount() { return g_vcount; }

// How far into its frame the clock is. The flag and the hcount are the display's counters, so they
// follow the clock even where an HLE call's time has run past a vblank the scheduler hasn't
// processed yet.
// Before the first vblank, the time since boot (not in a vblank).
double IntoFrameUs(bool *beforeFirst = nullptr) {
	const double now = (double)sched::NowUs();
	double start = g_frameStartUs;
	const bool next = now >= (double)EnterUs(g_frames);
	if (next) start = (double)EnterUs(g_frames);
	if (beforeFirst) *beforeFirst = !g_frames && !next;
	return std::max(0.0, now - start);
}

bool InVblank() {
	bool beforeFirst;
	const double into = IntoFrameUs(&beforeFirst);
	return !beforeFirst && into < kVblankUs;
}

u32 CurrentHcount() {
	// 0 from the interrupt, which takes most of line 0 (a thread first reads 1).
	return std::min<u32>((u32)(IntoFrameUs() / kLineUs), kLinesPerFrame - 1);
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
	const int result = sched::Wait(WAITTYPE_VBLANK, 0, 0, cb);
	sched::ChargeCycles(kWaitReturnCycles);
	return result;
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

// The driver's framebuffer at a priority (0-3; 2 is the game's, as sceDisplaySetFrameBuf and
// sceDisplayGetFrameBuf use it). From display.prx 4.01 (390C7E0E / A1BEF1F6; 63E22A26 / 5B5AEFAD
// before M33's NID resolver).
u32 sceDisplaySetFrameBufferInternal(u32 pri, u32 topaddr, int linesize, int pixelformat, int sync) {
	if (pri >= 4) return SCE_KERNEL_ERROR_INVALID_VALUE;
	if (pri == kGamePri) return sceDisplaySetFrameBuf(topaddr, linesize, pixelformat, sync);
	if (sync != 0 && sync != 1) return SCE_KERNEL_ERROR_INVALID_MODE;
	if (topaddr & 0xF) return SCE_KERNEL_ERROR_INVALID_POINTER;
	if (pixelformat < 0 || pixelformat > 3) return SCE_KERNEL_ERROR_INVALID_FORMAT;
	g_pri[pri].addr = topaddr;
	g_pri[pri].stride = linesize;
	g_pri[pri].format = pixelformat;
	g_priSet[pri] = topaddr != 0;
	return 0;
}
u32 sceDisplayGetFrameBufferInternal(u32 pri, u32 topPtr, u32 linePtr, u32 fmtPtr, int sync) {
	if (pri >= 4) return SCE_KERNEL_ERROR_INVALID_VALUE;
	if (pri == kGamePri) return sceDisplayGetFrameBuf(topPtr, linePtr, fmtPtr, sync);
	const FrameBuf &fb = g_priSet[pri] ? g_pri[pri] : kNoFrameBuf;
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
	static const hle::HleFunction driverFuncs[] = {
		HLEFN(0x390C7E0E, sceDisplaySetFrameBufferInternal),
		HLEFN(0xA1BEF1F6, sceDisplayGetFrameBufferInternal),
		HLEFN(0x63E22A26, sceDisplaySetFrameBufferInternal),
		HLEFN(0x5B5AEFAD, sceDisplayGetFrameBufferInternal),
	};
	hle::RegisterModule("sceDisplay_driver", driverFuncs);
}
