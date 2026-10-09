// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// sceCtrl: buttons/analog sampled into a 64-entry ring buffer, at the end of every VBlank (sampling
// cycle 0) or by a timer every `cycle` us. Reads take the samples that arrived since the last read
// and block for the next one when there are none; peeks return the latest N without consuming.
#include <cstring>
#include <vector>

#include "runtime/hle/display.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/host/host.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

using namespace kernel;

namespace {

constexpr u32 kNumBufs = 64;
constexpr u8 kAnalogCenter = 128;
constexpr u32 CTRL_MASK_USER = 0x0000F3F9;  // buttons user mode can see
// The vblank lasts this long (as display.cpp models it); samples are taken as it ends.
constexpr u64 kVblankUs = 770;

enum : SceUID { CTRL_WAIT_POSITIVE = 1, CTRL_WAIT_NEGATIVE = 2 };

struct CtrlData {
	u32 frame;  // timestamp (us)
	u32 buttons;
	u8 analog[2][2];  // [stick][x/y]; the second stick is unused on a PSP (left centered)
	u8 unused[4];
};
static_assert(sizeof(CtrlData) == 16, "CtrlData");

struct CtrlLatch {
	u32 btnMake, btnBreak, btnPress, btnRelease;
};

bool g_analogEnabled = false;
int g_latchBufs = 0;
u32 g_oldButtons = 0;
CtrlData g_bufs[kNumBufs];
u32 g_buf = 1, g_bufRead = 0;  // write head, read head
CtrlLatch g_latch;
int g_idleReset = -1, g_idleBack = -1;
u32 g_cycle = 0;
u64 g_timerEvent = 0;
u32 g_timerGen = 0;
std::vector<SceUID> g_waiting;

// Buttons held by --press (headless runs, scripted tests): [vblank, vblank + frames).
struct ScriptedPress {
	u32 vblank, frames, buttons;
};
std::vector<ScriptedPress> g_script;

u32 ScriptedButtons() {
	u32 v = display::Vcount(), b = 0;
	for (const ScriptedPress &p : g_script)
		if (v >= p.vblank && v - p.vblank < p.frames) b |= p.buttons;
	return b;
}

void ResetState() {
	g_analogEnabled = false;
	g_latchBufs = 0;
	g_oldButtons = 0;
	g_buf = 1;
	g_bufRead = 0;
	g_latch = CtrlLatch{};
	g_latch.btnRelease = 0xFFFFFFFF;  // start with everything released
	g_idleReset = g_idleBack = -1;
	g_cycle = 0;
	g_timerEvent = 0;
	g_timerGen++;
	g_waiting.clear();
	CtrlData init{};
	memset(init.analog, kAnalogCenter, sizeof(init.analog));
	for (CtrlData &d : g_bufs) d = init;
}

void UpdateLatch() {
	host::CtrlState s = host::GetCtrl();
	const u32 buttons = s.buttons | ScriptedButtons();

	CtrlData &d = g_bufs[g_buf];
	d = CtrlData{};
	d.frame = (u32)sched::NowUs();
	d.buttons = buttons;
	if (g_analogEnabled) {
		d.analog[0][0] = s.lx;
		d.analog[0][1] = s.ly;
		d.analog[1][0] = d.analog[1][1] = kAnalogCenter;
	} else {
		memset(d.analog, kAnalogCenter, sizeof(d.analog));
	}

	const u32 changed = buttons ^ g_oldButtons;
	g_latch.btnMake |= buttons & changed;
	g_latch.btnBreak |= g_oldButtons & changed;
	g_latch.btnPress |= buttons;
	g_latch.btnRelease |= ~buttons;
	g_latchBufs++;
	g_oldButtons = buttons;

	g_buf = (g_buf + 1) % kNumBufs;
	// Wrapped around: push the read head forward.
	if (g_bufRead == g_buf) g_bufRead = (g_bufRead + 1) % kNumBufs;
}

int ReadSingleBuffer(u32 ptr, bool negative) {
	if (!mem::IsValidRange(ptr, sizeof(CtrlData))) return 0;
	CtrlData d = g_bufs[g_bufRead];
	g_bufRead = (g_bufRead + 1) % kNumBufs;
	// Mask out buttons games aren't allowed to see.
	d.buttons &= CTRL_MASK_USER;
	if (negative) d.buttons = ~d.buttons;
	mem::WriteBytes(ptr, &d, sizeof(d));
	return 1;
}

int ReadBuffer(u32 ptr, u32 nBufs, bool negative, bool peek) {
	if (nBufs > kNumBufs) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	if (!peek && !sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	if (!peek && sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;

	const u32 resetRead = g_bufRead;
	u32 avail;
	// Peeks always work: they just go from now back nBufs samples.
	if (peek) {
		avail = nBufs;
	} else {
		avail = (g_buf - g_bufRead + kNumBufs) % kNumBufs;
		if (avail > nBufs) avail = nBufs;
	}
	g_bufRead = (g_buf - avail + kNumBufs) % kNumBufs;
	int done = 0;
	for (u32 i = 0; i < avail; i++) done += ReadSingleBuffer(ptr + i * (u32)sizeof(CtrlData), negative);
	if (peek) g_bufRead = resetRead;
	return done;
}

void DoSample() {
	UpdateLatch();
	// Wake up a single thread that was waiting for the buffer.
	while (!g_waiting.empty() && g_buf != g_bufRead) {
		const SceUID uid = g_waiting.front();
		g_waiting.erase(g_waiting.begin());
		PspThread *t = Get<PspThread>(uid);
		if (!t) continue;
		SceUID kind = 0;
		if (t->IsWaitingFor(WAITTYPE_CTRL, CTRL_WAIT_POSITIVE)) kind = CTRL_WAIT_POSITIVE;
		else if (t->IsWaitingFor(WAITTYPE_CTRL, CTRL_WAIT_NEGATIVE)) kind = CTRL_WAIT_NEGATIVE;
		if (!kind) continue;  // woken some other way
		const int ret = ReadSingleBuffer(t->waitValue[0], kind == CTRL_WAIT_NEGATIVE);
		sched::Wake(t, ret);
		break;
	}
}

void ScheduleTimer(u64 atUs) {
	const u32 gen = g_timerGen;
	g_timerEvent = sched::ScheduleEvent(atUs, [gen, atUs]() {
		if (gen != g_timerGen || g_cycle == 0) return;
		ScheduleTimer(atUs + g_cycle);
		DoSample();
	});
}

void StopTimer() {
	g_timerGen++;
	if (g_timerEvent) sched::CancelEvent(g_timerEvent);
	g_timerEvent = 0;
}

u32 sceCtrlSetSamplingCycle(u32 cycle) {
	if ((cycle > 0 && cycle < 5555) || cycle > 20000) return SCE_KERNEL_ERROR_INVALID_VALUE;
	const u32 prev = g_cycle;
	g_cycle = cycle;
	if (prev > 0) StopTimer();
	if (cycle > 0) ScheduleTimer(sched::NowUs() + cycle);
	return prev;
}
int sceCtrlGetSamplingCycle(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, g_cycle);
	return 0;
}
u32 sceCtrlSetSamplingMode(u32 mode) {
	if (mode > 1) return SCE_KERNEL_ERROR_INVALID_MODE;
	const u32 prev = g_analogEnabled ? 1 : 0;
	g_analogEnabled = mode == 1;
	return prev;
}
int sceCtrlGetSamplingMode(u32 ptr) {
	if (mem::IsValidRange(ptr, 4)) GWrite32(ptr, g_analogEnabled ? 1 : 0);
	return 0;
}

// Hardware: ~2us, plus ~0.2us for each sample copied (pspautotests ctrl/peekcost).
int Peek(u32 ptr, u32 n, bool negative) {
	const int done = ReadBuffer(ptr, n, negative, true);
	sched::ChargeCycles(330 + 44 * (s64)(done > 0 ? done : 0));
	return done;
}
int Read(u32 ptr, u32 n, bool negative) {
	const int done = ReadBuffer(ptr, n, negative, false);
	sched::ChargeCycles(330);
	if (done != 0) return done;
	PspThread *t = sched::Current();
	if (!t) return 0;
	g_waiting.push_back(t->uid);
	t->waitValue[0] = ptr;
	return sched::Wait(WAITTYPE_CTRL, negative ? CTRL_WAIT_NEGATIVE : CTRL_WAIT_POSITIVE, 0, false);
}
int sceCtrlPeekBufferPositive(u32 p, u32 n) { return Peek(p, n, false); }
int sceCtrlPeekBufferNegative(u32 p, u32 n) { return Peek(p, n, true); }
int sceCtrlReadBufferPositive(u32 p, u32 n) { return Read(p, n, false); }
int sceCtrlReadBufferNegative(u32 p, u32 n) { return Read(p, n, true); }

void WriteUserLatch(u32 ptr) {
	if (!mem::IsValidRange(ptr, sizeof(CtrlLatch))) return;
	CtrlLatch l = g_latch;
	l.btnBreak &= CTRL_MASK_USER;
	l.btnMake &= CTRL_MASK_USER;
	l.btnPress &= CTRL_MASK_USER;
	if (g_latchBufs > 0) l.btnRelease |= ~CTRL_MASK_USER;
	mem::WriteBytes(ptr, &l, sizeof(l));
}
int sceCtrlPeekLatch(u32 p) {
	WriteUserLatch(p);
	return g_latchBufs;
}
int sceCtrlReadLatch(u32 p) {
	WriteUserLatch(p);
	const int old = g_latchBufs;
	g_latch = CtrlLatch{};
	g_latchBufs = 0;
	return old;
}

int sceCtrlSetIdleCancelThreshold(int reset, int back) {
	if (reset < -1 || back < -1 || reset > 128 || back > 128) return (int)SCE_KERNEL_ERROR_INVALID_VALUE;
	g_idleReset = reset;
	g_idleBack = back;
	return 0;
}
int sceCtrlGetIdleCancelThreshold(u32 resetPtr, u32 backPtr) {
	auto bad = [](u32 p) { return p && ((p & 3) != 0 || !mem::IsValidRange(p, 4)); };
	if (bad(resetPtr) || bad(backPtr)) return (int)SCE_KERNEL_ERROR_PRIV_REQUIRED;
	if (resetPtr) GWrite32(resetPtr, (u32)g_idleReset);
	if (backPtr) GWrite32(backPtr, (u32)g_idleBack);
	return 0;
}

}  // namespace

namespace ctrl {
// Called at every VBlank start: in vblank sampling mode, the sample is taken as the vblank ends.
void OnVblank() {
	sched::ScheduleEvent(sched::NowUs() + kVblankUs, []() {
		if (g_cycle == 0) DoSample();
	});
}
void AddScriptedPress(u32 vblank, u32 buttons, u32 frames) { g_script.push_back({vblank, frames, buttons}); }
}  // namespace ctrl

void Register_sceCtrl() {
	ResetState();
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
