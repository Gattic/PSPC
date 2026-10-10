// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/core/interp.h"

#include <cstdlib>
#include <vector>

#include "allegrex/decode.h"
#include "allegrex/guestmem.h"
#include "allegrex/insn.h"
#include "allegrex/sem.h"
#include "allegrex/sem_tables.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/dispatch.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"

namespace interp {

using ag::Kind;
using ag::Op;

static u64 g_icount = 0;

u64 InstructionCount() { return g_icount; }

// Reverse-engineering aids (run with --interp so every call is interpreted):
//   PSPC_TRACE_PC=<hex addr>[,<hex addr>...]  logs a0-a3, ra and sp each time code reaches an address;
//   PSPC_TRACE_AFTER=<hex addr>,<n>           logs the next n function calls after reaching addr.
static std::vector<u32> g_tracePcs;
static u32 g_traceAfterPc = 0, g_traceAfterCount = 0, g_traceCallsLeft = 0;
static bool g_traceOn = [] {
	const char *env = getenv("PSPC_TRACE_PC");
	for (const char *p = env; p && *p;) {
		char *end = nullptr;
		u32 a = (u32)strtoul(p, &end, 16);
		if (end == p) break;
		g_tracePcs.push_back(a);
		p = *end ? end + 1 : end;
	}
	if (const char *after = getenv("PSPC_TRACE_AFTER")) {
		char *end = nullptr;
		g_traceAfterPc = (u32)strtoul(after, &end, 16);
		g_traceAfterCount = end && *end == ',' ? (u32)strtoul(end + 1, nullptr, 10) : 200;
	}
	return !g_tracePcs.empty() || g_traceAfterPc != 0;
}();

static void TraceCall(Ctx *c, u32 target, u32 from) {
	g_traceCallsLeft--;
	LOGI("TRACE", "  call %08x from %08x a0=%08x a1=%08x a2=%08x a3=%08x", target, from, c->r[4], c->r[5], c->r[6], c->r[7]);
}

static void TracePc(Ctx *c, u32 pc) {
	if (pc == g_traceAfterPc) g_traceCallsLeft = g_traceAfterCount;
	for (u32 a : g_tracePcs) {
		if (a == pc) {
			LOGI("TRACE", "%08x v0=%08x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x", pc, c->r[2], c->r[4], c->r[5], c->r[6], c->r[7],
			     c->r[31], c->r[29]);
			return;
		}
	}
}

[[noreturn]] static void BadPc(Ctx *c, u32 pc, u32 from) {
	Fatal("Guest jumped to invalid address %08x (from %08x, ra=%08x sp=%08x)", pc, from, c->r[31], c->r[29]);
}

static void ExecDelaySlot(Ctx *c, u32 pc) {
	u32 w = GRead32(pc);
	Op op = ag::Decode(w);
	Kind k = ag::OpKind(op);
	g_icount++;
	c->budget--;
	switch (k) {
	case Kind::ALU: ag::g_semTable[(size_t)op](c, w); return;
	case Kind::SYSCALL:
		c->pc = pc;
		hle::Syscall(c, w, pc);
		return;
	case Kind::BREAK: cpu::Break(c, w, pc); return;
	default:
		// A branch in a delay slot is architecturally undefined; real code doesn't do it.
		LOGW("CPU", "Control-flow instruction %08x in delay slot at %08x ignored", w, pc);
		return;
	}
}

// A condition reader right behind the instruction that set the condition sees the value from
// before it (ag::SeesStaleCondition, the same static rule recompiled code applies).
static bool SeesStale(Op op, u32 pc, u32 w) {
	auto word = [](u32 a) { return mem::IsValidAddress(a) ? GRead32(a) : 0u; };
	return ag::SeesStaleCondition(op, w, word(pc - 4), word(pc - 8), word(pc - 12));
}

static bool StaleCond(Ctx *c, Op op, u32 w) {
	switch (op) {
	case Op::BC1F: return cond_BC1F_PREV(c, w);
	case Op::BC1T: return cond_BC1T_PREV(c, w);
	case Op::BC1FL: return cond_BC1FL_PREV(c, w);
	case Op::BC1TL: return cond_BC1TL_PREV(c, w);
	case Op::BVF: return cond_BVF_PREV(c, w);
	case Op::BVT: return cond_BVT_PREV(c, w);
	case Op::BVFL: return cond_BVFL_PREV(c, w);
	default: return cond_BVTL_PREV(c, w);
	}
}

void ExecOne(Ctx *c, u32 pc, u32 w) {
	Op op = ag::Decode(w);
	if (ag::OpKind(op) == Kind::ALU) {
		ag::g_semTable[(size_t)op](c, w);
	} else {
		cpu::InvalidInstruction(c, w, pc);
	}
}

void Run(Ctx *c, u32 pc, u32 ra0, u32 sp0) {
	if (!mem::IsValidAddress(pc)) BadPc(c, pc, c->pc);

	// Calls a native function at `target` if one exists. Returns false when the call should be
	// interpreted inline instead. On return, *propagate is set if a non-local return passes us.
	auto nativeCall = [&](u32 target, u32 ret, bool *propagate) -> bool {
		HostFn fn = dispatch::Lookup(target);
		if (!fn) return false;
		c->pc = target;
		fn(c);
		if (c->unwinding) {
			if (c->pc == ret) {
				c->unwinding = 0;
			} else {
				*propagate = true;
			}
		}
		return true;
	};

	// A jump to native code. A recompiled body continues this frame (it returns to ra0 for us).
	// A thunk (HLE import stub) has call semantics: run it, then carry on at $ra in this loop -
	// not in a nested interpreter, which would grow the host stack every time a loop tail-calls an
	// import. Returns false when target has no native code; *done means this frame is finished.
	auto nativeJump = [&](u32 target, bool *done, u32 *next) -> bool {
		if (dispatch::LookupBody(target)) {
			dispatch::Jump(c, target, ra0, sp0);
			*done = true;
			return true;
		}
		HostFn fn = dispatch::Lookup(target);
		if (!fn) return false;
		u32 ret = c->r[31];
		c->pc = target;
		fn(c);
		if (c->unwinding || (ret == ra0 && c->r[29] >= sp0)) {
			if (!c->unwinding) c->pc = ret;
			*done = true;
			return true;
		}
		*done = false;
		*next = ret;
		return true;
	};

	for (;;) {
		c->pc = pc;
		if (PSPC_UNLIKELY(g_traceOn)) TracePc(c, pc);
		u32 w = GRead32(pc);
		Op op = ag::Decode(w);
		Kind k = ag::OpKind(op);
		g_icount++;
		c->budget--;  // one cycle per instruction, as recompiled code charges
		switch (k) {
		case Kind::ALU:
			if (PSPC_UNLIKELY(op == Op::MFVC) && SeesStale(op, pc, w)) sem_MFVC_PREV(c, w);
			else ag::g_semTable[(size_t)op](c, w);
			pc += 4;
			continue;

		case Kind::BR:
		case Kind::BRL:
		case Kind::BRAL:
		case Kind::BRALL: {
			bool taken = PSPC_UNLIKELY(ag::MaySeeStaleCondition(op)) && SeesStale(op, pc, w)
			                 ? StaleCond(c, op, w)
			                 : ag::g_condTable[(size_t)op](c, w);
			u32 target = insn::BranchTarget(pc, w);
			bool link = (k == Kind::BRAL || k == Kind::BRALL);
			bool likely = (k == Kind::BRL || k == Kind::BRALL);
			if (link) c->r[31] = pc + 8;
			if (taken || !likely) ExecDelaySlot(c, pc + 4);
			if (!taken) {
				pc += 8;
				continue;
			}
			if (link) {
				bool propagate = false;
				if (nativeCall(target, pc + 8, &propagate)) {
					if (propagate) return;
					pc += 8;
					continue;
				}
			} else if (target <= pc && c->budget < 0) {
				c->pc = target;
				cpu::YieldCheck(c);
			}
			pc = target;
			continue;
		}

		case Kind::J: {
			u32 target = insn::JumpTarget(pc, w);
			ExecDelaySlot(c, pc + 4);
			bool done = false;
			u32 next = 0;
			if (nativeJump(target, &done, &next)) {
				if (done) return;
				pc = next;
				continue;
			}
			if (target <= pc && c->budget < 0) {
				c->pc = target;
				cpu::YieldCheck(c);
			}
			if (!mem::IsValidAddress(target)) BadPc(c, target, pc);
			pc = target;
			continue;
		}

		case Kind::JAL: {
			u32 target = insn::JumpTarget(pc, w);
			if (PSPC_UNLIKELY(g_traceCallsLeft)) TraceCall(c, target, pc);
			c->r[31] = pc + 8;
			ExecDelaySlot(c, pc + 4);
			bool propagate = false;
			if (nativeCall(target, pc + 8, &propagate)) {
				if (propagate) return;
				pc += 8;
				continue;
			}
			if (!mem::IsValidAddress(target)) BadPc(c, target, pc);
			pc = target;
			continue;
		}

		case Kind::JR: {
			u32 rs = insn::Rs(w);
			u32 target = c->r[rs];
			ExecDelaySlot(c, pc + 4);
			if (target == ra0 && c->r[29] >= sp0) {
				c->pc = target;
				return;
			}
			if (rs != 31) {
				bool done = false;
				u32 next = 0;
				if (nativeJump(target, &done, &next)) {
					if (done) return;
					pc = next;
					continue;
				}
			}
			if (!mem::IsValidAddress(target)) BadPc(c, target, pc);
			pc = target;
			continue;
		}

		case Kind::JALR: {
			u32 target = c->r[insn::Rs(w)];
			if (PSPC_UNLIKELY(g_traceCallsLeft)) TraceCall(c, target, pc);
			u32 rd = insn::Rd(w);
			if (rd) c->r[rd] = pc + 8;
			ExecDelaySlot(c, pc + 4);
			bool propagate = false;
			if (nativeCall(target, pc + 8, &propagate)) {
				if (propagate) return;
				pc += 8;
				continue;
			}
			if (!mem::IsValidAddress(target)) BadPc(c, target, pc);
			pc = target;
			continue;
		}

		case Kind::SYSCALL:
			hle::Syscall(c, w, pc);
			if (c->unwinding) return;
			pc += 4;
			continue;

		case Kind::BREAK:
			cpu::Break(c, w, pc);
			pc += 4;
			continue;

		case Kind::ERET:
		case Kind::INVALID:
		default:
			cpu::InvalidInstruction(c, w, pc);
			pc += 4;
			continue;
		}
	}
}

}  // namespace interp
