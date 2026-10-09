// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/core/cpu.h"

#include "allegrex/decode.h"
#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/interp.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/kernel/scheduler.h"

static u32 s_cop0[32];
static u32 s_intrEnabled = 1;
u32 *g_cop0 = s_cop0;
u32 *g_intrEnabled = &s_intrEnabled;

namespace cpu {

static thread_local Ctx *t_current = nullptr;
static bool g_strict = false;
static PspcRuntimeApi g_api;

Ctx *CurrentCtx() { return t_current; }
void SetCurrentCtx(Ctx *c) { t_current = c; }
bool StrictMemory() { return g_strict; }
void SetStrictMemory(bool strict) { g_strict = strict; }

static void ApiSyscall(Ctx *c, u32 insn, u32 pc) { hle::Syscall(c, insn, pc); }
static void ApiInterpretFrom(Ctx *c, u32 pc, u32 ra0, u32 sp0) { interp::Run(c, pc, ra0, sp0); }
static void ApiExecWord(Ctx *c, u32 pc) { interp::ExecOne(c, pc, GRead32(pc)); }

void Init() {
	// The magic return stub: anything that executes it by accident traps.
	GWrite32(MAGIC_RETURN_ADDR, (SYSCALL_MAGIC_RETURN << 6) | 0x0C);
	GWrite32(MAGIC_RETURN_ADDR + 4, 0);

	g_api = PspcRuntimeApi{};
	g_api.abiVersion = PSPC_AOT_ABI_VERSION;
	g_api.structSize = sizeof(PspcRuntimeApi);
	g_api.membase = g_membase;
	g_api.cop0 = g_cop0;
	g_api.intrEnabled = g_intrEnabled;
	g_api.callIndirect = &dispatch::Call;
	g_api.jumpIndirect = &dispatch::Jump;
	g_api.nonlocalReturn = &dispatch::NonlocalReturn;
	g_api.syscall = &ApiSyscall;
	g_api.breakInsn = &Break;
	g_api.yieldCheck = &YieldCheck;
	g_api.invalidInsn = &InvalidInstruction;
	g_api.interpretFrom = &ApiInterpretFrom;
	g_api.execWord = &ApiExecWord;
}

const PspcRuntimeApi *AotApi() { return &g_api; }

void RunFunction(Ctx *c, u32 func) {
	c->r[31] = MAGIC_RETURN_ADDR;
	c->unwinding = 0;
	dispatch::Call(c, func);
	while (c->unwinding) {
		u32 target = c->pc;
		c->unwinding = 0;
		if (target == MAGIC_RETURN_ADDR) break;
		// Nobody on the host stack owns the target: keep running the thread from there.
		interp::Run(c, target, MAGIC_RETURN_ADDR, 0);
	}
}

u32 CallGuest(Ctx *c, u32 func, std::initializer_list<u32> args) {
	// Save everything a guest function may clobber, including VFPU state. This lives on the
	// current fiber's stack: guest threads are fibers sharing one host thread, so thread_local
	// storage would be shared between them.
	alignas(64) Ctx s;
	memcpy(&s, c, sizeof(Ctx));

	int i = 0;
	for (u32 a : args) {
		if (i < 8) c->r[4 + i] = a;
		i++;
	}
	c->r[2] = 0;
	RunFunction(c, func);
	u32 result = c->r[2];

	void *thread = c->thread;
	memcpy(c, &s, sizeof(Ctx));
	c->thread = thread;
	return result;
}

void YieldCheck(Ctx *c) {
	// Taking an interrupt is an exception, and an exception breaks an ll/sc pair (cpu/lsu/llsc).
	// The scheduler counts each interrupt that preempts the thread.
	kernel::PspThread *t = sched::Current();
	const u32 interrupts = t ? t->intrPreemptCount : 0;
	sched::YieldCheck(c);
	if (t && t->intrPreemptCount != interrupts) c->llbit = 0;
}

void Break(Ctx *c, u32 insn, u32 pc) {
	LOGW("CPU", "break 0x%05x at %08x (ra=%08x)", (insn >> 6) & 0xFFFFF, pc, c->r[31]);
}

void InvalidInstruction(Ctx *c, u32 insn, u32 pc) {
	char dis[128];
	ag::Disassemble(pc, insn, dis, sizeof(dis));
	Fatal("Unsupported instruction %08x (%s) at %08x (ra=%08x)", insn, dis, pc, c->r[31]);
}

}  // namespace cpu
