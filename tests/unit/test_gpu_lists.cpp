// SPDX-License-Identifier: GPL-2.0-or-later
// GE display list queue semantics: enqueue, stall, signals, finish, break/continue, callbacks.
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstring>
#include <string>
#include <vector>

#include "allegrex/guestmem.h"
#include "gpu/ge.h"
#include "gpu/ge_constants.h"
#include "testing.h"

using namespace gpu;

#define CHECK_STR(a, b) \
	do { \
		const std::string _a = (a), _b = (b); \
		if (_a != _b) { \
			printf("  FAILED %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, _a.c_str(), _b.c_str()); \
			g_testFailures++; \
		} \
	} while (0)

namespace {

constexpr u32 VRAM = 0x04000000;
constexpr u32 RAM = 0x08000000;
constexpr u32 RAM_SIZE = 0x00400000;
constexpr u32 LIST = RAM + 0x100000;

struct Env {
	u8 *mem = nullptr;
	u8 *saved = nullptr;
	Env() {
		mem = (u8 *)VirtualAlloc(nullptr, 0x0A000000, MEM_RESERVE, PAGE_NOACCESS);
		VirtualAlloc(mem + VRAM, 0x200000, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(mem + RAM, RAM_SIZE, MEM_COMMIT, PAGE_READWRITE);
		saved = g_membase;
		g_membase = mem;
		SetRamSize(RAM_SIZE);
		SetSdkVersion(0x06060010);
		Init();
		SetRenderThreads(1);
	}
	~Env() {
		Shutdown();
		g_membase = saved;
		VirtualFree(mem, 0, MEM_RELEASE);
	}
};

struct DL {
	u32 start, pc;
	explicit DL(u32 addr = LIST) : start(addr), pc(addr) {}
	DL &C(int cmd, u32 data = 0) {
		GWrite32(pc, ((u32)cmd << 24) | (data & 0xFFFFFF));
		pc += 4;
		return *this;
	}
	DL &Signal(int behaviour, u32 arg) { return C(GE_CMD_SIGNAL, ((u32)behaviour << 16) | (arg & 0xFFFF)).C(GE_CMD_END); }
	DL &Finish(u32 arg = 0) { return C(GE_CMD_FINISH, arg).C(GE_CMD_END); }
};

struct Recorder {
	std::vector<std::string> events;
	int lastList = 0;
	int statusInFinish = -1;
	int drawStatusInFinish = -1;
	static Recorder *self;

	static void Signal(void *, int listId, int cb, u32 arg) {
		char buf[64];
		snprintf(buf, sizeof(buf), "signal:%d:%x", cb, arg);
		self->events.push_back(buf);
		self->lastList = listId;
	}
	static void Finish(void *, int listId, int cb, u32 arg) {
		char buf[64];
		snprintf(buf, sizeof(buf), "finish:%d:%x", cb, arg);
		self->events.push_back(buf);
		self->lastList = listId;
		self->statusInFinish = ListSync(listId, 1);
		self->drawStatusInFinish = DrawSync(1);
	}
	static void ListDone(void *, int listId) {
		self->events.push_back("listdone");
	}
	static void DrawDone(void *) {
		self->events.push_back("drawdone");
	}
	Recorder() {
		self = this;
		Hooks h;
		h.onSignal = &Signal;
		h.onFinish = &Finish;
		h.onListDone = &ListDone;
		h.onDrawSyncDone = &DrawDone;
		SetHooks(h);
	}
	~Recorder() {
		SetHooks(Hooks{});
		self = nullptr;
	}
	std::string Joined() const {
		std::string s;
		for (const auto &e : events) {
			if (!s.empty())
				s += ",";
			s += e;
		}
		return s;
	}
};
Recorder *Recorder::self = nullptr;

}  // namespace

TEST(gpu_list_signal_finish_order) {
	Env env;
	Recorder rec;
	DL dl;
	dl.Signal(SIGNAL_HANDLER_SUSPEND, 0x42).C(GE_CMD_ALPHATEST, 0x11).Finish(0x77);
	const int id = EnqueueList(dl.start, 0, 3, 0, false);
	CHECK(id >= 0);
	CHECK_STR(rec.Joined(), "signal:3:42,finish:3:77,drawdone,listdone");
	CHECK_EQ(rec.lastList, id);
	// In the finish callback the list reads COMPLETED and the queue as done.
	CHECK_EQ(rec.statusInFinish, 0);
	CHECK_EQ(rec.drawStatusInFinish, 0);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), ((u32)GE_CMD_ALPHATEST << 24) | 0x11u);
	CHECK_EQ(ListSync(id, 0), 0);
	CHECK_EQ(DrawSync(0), 0);
	// sceGeDrawSync(0) turned the completed list into NONE.
	CHECK_EQ(ListSync(id, 1), (int)0x80000100);
}

TEST(gpu_list_no_callback) {
	Env env;
	Recorder rec;
	DL dl;
	dl.Signal(SIGNAL_HANDLER_SUSPEND, 1).Finish(2);
	const int id = EnqueueList(dl.start, 0, -1, 0, false);
	CHECK_STR(rec.Joined(), "drawdone,listdone");
	CHECK_EQ(ListSync(id, 1), 0);
}

TEST(gpu_list_stall) {
	Env env;
	Recorder rec;
	DL dl;
	dl.C(GE_CMD_ALPHATEST, 0x1).C(GE_CMD_ALPHATEST, 0x2);
	const u32 mid = dl.pc;
	dl.C(GE_CMD_ALPHATEST, 0x3).Finish();
	// Stalled at the start: nothing runs.
	const int id = EnqueueList(dl.start, dl.start, 0, 0, false);
	CHECK(id >= 0);
	CHECK_EQ(ListSync(id, 1), 3);
	CHECK_EQ(DrawSync(1), 3);
	CHECK_EQ(ListSync(id, 0), SYNC_WAIT);
	CHECK_EQ(DrawSync(0), SYNC_WAIT);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), (u32)GE_CMD_ALPHATEST << 24);
	// Up to the middle.
	CHECK_EQ(UpdateStallAddr(id, mid), 0);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), ((u32)GE_CMD_ALPHATEST << 24) | 2u);
	CHECK_EQ(ListSync(id, 1), 3);
	CHECK(rec.events.empty());
	// A second list waits behind it.
	DL dl2(LIST + 0x1000);
	dl2.C(GE_CMD_ALPHATEST, 0x9).Finish(5);
	const int id2 = EnqueueList(dl2.start, 0, 0, 0, false);
	CHECK(id2 >= 0 && id2 != id);
	CHECK_EQ(ListSync(id2, 1), 1);
	// Release the stall: both run, in order.
	CHECK_EQ(UpdateStallAddr(id, 0), 0);
	CHECK_STR(rec.Joined(), "finish:0:0,listdone,finish:0:5,drawdone,listdone");
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), ((u32)GE_CMD_ALPHATEST << 24) | 9u);
	CHECK_EQ(UpdateStallAddr(id, 0), (int)0x80000020);
	CHECK_EQ(DrawSync(1), 0);
}

TEST(gpu_list_errors) {
	Env env;
	DL dl;
	dl.Finish();
	CHECK_EQ(EnqueueList(dl.start + 2, 0, -1, 0, false), (int)0x80000103);
	CHECK_EQ(EnqueueList(0x00001000, 0, -1, 0, false), (int)0x80000103);
	CHECK_EQ(ListSync(0x12345, 1), (int)0x80000100);
	// Stall the first so it stays queued; the same address again is busy.
	const int id = EnqueueList(dl.start, dl.start, -1, 0, false);
	CHECK(id >= 0);
	CHECK_EQ(EnqueueList(dl.start, dl.start, -1, 0, false), (int)0x80000021);
	CHECK_EQ(ListSync(id, 2), (int)0x80000107);
	CHECK_EQ(DrawSync(2), (int)0x80000107);
	// A started list can't be dequeued.
	CHECK_EQ(DequeueList(id), (int)0x80000021);
	// One that hasn't started can.
	DL dl2(LIST + 0x100);
	dl2.Finish();
	const int id2 = EnqueueList(dl2.start, 0, -1, 0, false);
	CHECK_EQ(ListSync(id2, 1), 1);
	CHECK_EQ(DequeueList(id2), 0);
	CHECK_EQ(ListSync(id2, 1), (int)0x80000100);
	// Enqueue head needs the head paused.
	DL dl3(LIST + 0x200);
	dl3.Finish();
	CHECK_EQ(EnqueueList(dl3.start, 0, -1, 0, true), (int)0x800001FE);
	CHECK_EQ(Break(2, 0), (int)0x80000107);
	CHECK_EQ(GetCmd(256), 0x80000102u);
}

TEST(gpu_list_break_continue) {
	Env env;
	Recorder rec;
	DL dl;
	dl.C(GE_CMD_ALPHATEST, 1).Finish(1);
	const int id = EnqueueList(dl.start, dl.start, 0, 0, false);
	// Break the running (stalled) list: PAUSED.
	CHECK_EQ(Break(0, 0), id);
	CHECK_EQ(ListSync(id, 1), 4);
	CHECK_EQ(Break(0, 0), (int)0x80000020);
	// Continue resumes it; it's still stalled.
	CHECK_EQ(Continue(), 0);
	CHECK_EQ(ListSync(id, 1), 3);
	CHECK_EQ(Continue(), (int)0x80000020);
	// Break(1) throws everything away, without callbacks.
	CHECK_EQ(Break(1, 0), 0);
	CHECK_EQ(ListSync(id, 1), (int)0x80000100);
	CHECK_EQ(Break(0, 0), (int)0x80000020);
	CHECK(rec.events.empty());
	// The same address can be enqueued again and runs.
	const int id2 = EnqueueList(dl.start, 0, 0, 0, false);
	CHECK(id2 >= 0);
	CHECK_STR(rec.Joined(), "finish:0:1,drawdone,listdone");
}

TEST(gpu_list_enqueue_head) {
	Env env;
	Recorder rec;
	DL dl;
	dl.C(GE_CMD_ALPHATEST, 5).Finish(3);
	// Head with an empty queue: PAUSED, and not started until sceGeContinue.
	const int id = EnqueueList(dl.start, 0, 0, 0, true);
	CHECK(id >= 0);
	Run();
	CHECK_EQ(ListSync(id, 1), 4);
	CHECK(rec.events.empty());
	CHECK_EQ(Continue(), 0);
	CHECK_STR(rec.Joined(), "finish:0:3,drawdone,listdone");
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST), ((u32)GE_CMD_ALPHATEST << 24) | 5u);
}

TEST(gpu_list_call_ret_jump) {
	Env env;
	// Main: CALL sub; ALPHATEST 2; JUMP tail. Sub: ALPHATEST 1 (checked via COLORREF), RET. Tail: FINISH.
	DL sub(LIST + 0x400);
	sub.C(GE_CMD_COLORREF, 0xABCDEF).C(GE_CMD_RET);
	DL tail(LIST + 0x800);
	tail.C(GE_CMD_COLORTESTMASK, 0x123).Finish();
	DL dl;
	dl.C(GE_CMD_BASE, (LIST >> 8) & 0x0F0000).C(GE_CMD_CALL, (LIST + 0x400) & 0xFFFFFF).C(GE_CMD_ALPHATEST, 2);
	dl.C(GE_CMD_JUMP, (LIST + 0x800) & 0xFFFFFF).C(GE_CMD_ALPHATEST, 3).Finish();
	const int id = EnqueueList(dl.start, 0, -1, 0, false);
	CHECK_EQ(ListSync(id, 1), 0);
	CHECK_EQ(GetCmd(GE_CMD_COLORREF) & 0xFFFFFF, 0xABCDEFu);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFFFFFF, 2u);
	CHECK_EQ(GetCmd(GE_CMD_COLORTESTMASK) & 0xFFFFFF, 0x123u);

	// OFFSETADDR + ORIGIN relative addressing: ORIGIN sets the offset to its own address.
	DL dl2(LIST + 0x1000);
	dl2.C(GE_CMD_BASE, 0).C(GE_CMD_ORIGIN);  // offset = address of ORIGIN
	dl2.C(GE_CMD_JUMP, 3 * 4).C(GE_CMD_COLORREF, 0x111).C(GE_CMD_COLORREF, 0x222).Finish();
	EnqueueList(dl2.start, 0, -1, 0, false);
	// The jump lands at ORIGIN + 12 = the second COLORREF.
	CHECK_EQ(GetCmd(GE_CMD_COLORREF) & 0xFFFFFF, 0x222u);
}

TEST(gpu_list_signal_pause_and_sync) {
	Env env;
	Recorder rec;
	// PAUSE: the list reads PAUSED right away, and at the FINISH the signal callback runs with its argument.
	DL dl;
	dl.Signal(SIGNAL_HANDLER_PAUSE, 0x55).C(GE_CMD_ALPHATEST, 7).Finish(0x66).C(GE_CMD_ALPHATEST, 8).Finish(0x67);
	const int id = EnqueueList(dl.start, 0, 1, 0, false);
	CHECK_STR(rec.Joined(), "signal:1:55");
	CHECK_EQ(ListSync(id, 1), 4);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFF, 7u);
	// Continue runs to the next FINISH.
	CHECK_EQ(Continue(), 0);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFF, 8u);
	CHECK_STR(rec.Joined(), "signal:1:55,finish:1:67,drawdone,listdone");

	// SYNC: the next FINISH is swallowed.
	Recorder rec2;
	DL dl2(LIST + 0x1000);
	dl2.Signal(SIGNAL_SYNC, 0).Finish(1).C(GE_CMD_ALPHATEST, 9).Finish(2);
	EnqueueList(dl2.start, 0, 2, 0, false);
	CHECK_STR(rec2.Joined(), "finish:2:2,drawdone,listdone");
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFF, 9u);
}

TEST(gpu_list_signal_jump_call) {
	Env env;
	// SIGNAL JUMP to an absolute address, no interrupt.
	DL target(LIST + 0x2000);
	target.C(GE_CMD_COLORREF, 0x777).Finish();
	DL dl;
	const u32 t = LIST + 0x2000;
	dl.C(GE_CMD_SIGNAL, ((u32)SIGNAL_JUMP << 16) | (t >> 16)).C(GE_CMD_END, t & 0xFFFF).C(GE_CMD_COLORREF, 0x666).Finish();
	const int id = EnqueueList(dl.start, 0, -1, 0, false);
	CHECK_EQ(ListSync(id, 1), 0);
	CHECK_EQ(GetCmd(GE_CMD_COLORREF) & 0xFFFFFF, 0x777u);
}

TEST(gpu_list_context_save_restore) {
	Env env;
	// The state before: ALPHATEST 0x42.
	DL dl;
	dl.C(GE_CMD_ALPHATEST, 0x42).Finish();
	EnqueueList(dl.start, 0, -1, 0, false);
	// A list with a context: its changes are undone when it completes.
	const u32 ctx = RAM + 0x3000, args = RAM + 0x4000;
	GWrite32(args, 16);
	GWrite32(args + 4, ctx);
	GWrite32(args + 8, 0);
	GWrite32(args + 12, 0);
	DL dl2(LIST + 0x1000);
	dl2.C(GE_CMD_ALPHATEST, 0x99).Finish();
	const int id = EnqueueList(dl2.start, 0, -1, args, false);
	CHECK(id >= 0);
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFF, 0x42u);
	// Too deep a stack.
	GWrite32(args + 8, 256);
	CHECK_EQ(EnqueueList(dl2.start, 0, -1, args, false), (int)0x80000104);
}

TEST(gpu_list_interrupts_disabled) {
	Env env;
	Recorder rec;
	SetInterruptsEnabled(false);
	DL dl;
	dl.Finish(1);
	DL dl2(LIST + 0x100);
	dl2.Finish(2);
	const int id = EnqueueList(dl.start, 0, 0, 0, false);
	const int id2 = EnqueueList(dl2.start, 0, 0, 0, false);
	// Stands still at the first FINISH: DRAWING, the next QUEUED, no callback.
	CHECK(rec.events.empty());
	CHECK_EQ(ListSync(id, 1), 2);
	CHECK_EQ(ListSync(id2, 1), 1);
	SetInterruptsEnabled(true);
	Run();
	CHECK_STR(rec.Joined(), "finish:0:1,listdone,finish:0:2,drawdone,listdone");
}

namespace {
int pendingCount = 0;
void OnPending(void *, u64) {
	pendingCount++;
}
}  // namespace

TEST(gpu_list_async_interrupts) {
	Env env;
	Hooks h;
	h.onInterruptPending = &OnPending;
	pendingCount = 0;
	SetHooks(h);
	DL dl;
	dl.Signal(SIGNAL_HANDLER_SUSPEND, 0x10).C(GE_CMD_ALPHATEST, 3).Finish(0x20);
	const int id = EnqueueList(dl.start, 0, 4, 0, false);
	CHECK_EQ(pendingCount, 1);
	// Stopped at the signal.
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFF, 0u);
	Interrupt intr;
	CHECK(TakeInterrupt(&intr));
	CHECK_EQ(intr.listId, id);
	CHECK_EQ(intr.callbackId, 4);
	CHECK(!intr.isFinish);
	CHECK_EQ(intr.arg, 0x10u);
	InterruptDone();
	CHECK_EQ(GetCmd(GE_CMD_ALPHATEST) & 0xFF, 3u);
	CHECK_EQ(pendingCount, 2);
	CHECK(TakeInterrupt(&intr));
	CHECK(intr.isFinish);
	CHECK_EQ(intr.arg, 0x20u);
	CHECK_EQ(ListSync(id, 1), 0);
	InterruptDone();
	CHECK(!TakeInterrupt(&intr));
	CHECK_EQ(ListSync(id, 0), 0);
	SetHooks(Hooks{});
}

namespace {
u64 fakeTicks = 0;
u64 lastPendingAt = 0;
u64 GetFakeTicks(void *) {
	return fakeTicks;
}
void OnPendingAt(void *, u64 at) {
	lastPendingAt = at;
}
}  // namespace

TEST(gpu_list_timing) {
	Env env;
	Hooks h;
	h.getTicks = &GetFakeTicks;
	h.onInterruptPending = &OnPendingAt;
	fakeTicks = 1000;
	SetHooks(h);
	DL dl;
	dl.Signal(SIGNAL_HANDLER_SUSPEND, 1).C(GE_CMD_ALPHATEST, 3).Finish(0);
	const int id = EnqueueList(dl.start, 0, -1, 0, false);
	// No callback: still an interrupt, raised once the GE got there (list start + commands + END).
	CHECK(lastPendingAt > 1000);
	CHECK_EQ(ListSync(id, 0), SYNC_WAIT);
	Interrupt intr;
	fakeTicks = lastPendingAt;
	CHECK(TakeInterrupt(&intr));
	InterruptDone();
	// The FINISH interrupt, later; the list completes when it's delivered.
	CHECK(lastPendingAt > fakeTicks);
	CHECK_EQ(DrawSync(0), SYNC_WAIT);
	fakeTicks = lastPendingAt;
	CHECK(TakeInterrupt(&intr));
	InterruptDone();
	CHECK_EQ(ListSync(id, 0), 0);
	CHECK_EQ(DrawSync(0), 0);

	// The drawing is done when the GE reaches the FINISH, not when the interrupt was taken.
	DL dl2;
	dl2.C(GE_CMD_ALPHATEST, 4).Finish(0);
	fakeTicks = 5000;
	const int id2 = EnqueueList(dl2.start, 0, -1, 0, false);
	while (TakeInterrupt(&intr))
		InterruptDone();
	CHECK(DrawCompleteTicks() > 5000);
	CHECK_EQ(DrawSync(0), SYNC_WAIT);
	fakeTicks = DrawCompleteTicks();
	CHECK_EQ(DrawSync(0), 0);
	CHECK_EQ(ListSync(id2, 0), 0);
	SetHooks(Hooks{});
}
