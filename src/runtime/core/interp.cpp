// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/core/interp.h"

#include "allegrex/decode.h"
#include "allegrex/guestmem.h"
#include "allegrex/insn.h"
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

[[noreturn]] static void BadPc(Ctx *c, u32 pc, u32 from) {
	Fatal("Guest jumped to invalid address %08x (from %08x, ra=%08x sp=%08x)", pc, from, c->r[31], c->r[29]);
}

static void ExecDelaySlot(Ctx *c, u32 pc) {
	u32 w = GRead32(pc);
	Op op = ag::Decode(w);
	Kind k = ag::OpKind(op);
	g_icount++;
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

	for (;;) {
		c->pc = pc;
		u32 w = GRead32(pc);
		Op op = ag::Decode(w);
		Kind k = ag::OpKind(op);
		g_icount++;
		switch (k) {
		case Kind::ALU:
			ag::g_semTable[(size_t)op](c, w);
			pc += 4;
			continue;

		case Kind::BR:
		case Kind::BRL:
		case Kind::BRAL:
		case Kind::BRALL: {
			bool taken = ag::g_condTable[(size_t)op](c, w);
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
			} else if (target <= pc && --c->budget < 0) {
				c->pc = target;
				cpu::YieldCheck(c);
			}
			pc = target;
			continue;
		}

		case Kind::J: {
			u32 target = insn::JumpTarget(pc, w);
			ExecDelaySlot(c, pc + 4);
			if (dispatch::Lookup(target)) {
				dispatch::Jump(c, target, ra0, sp0);
				return;
			}
			if (target <= pc && --c->budget < 0) {
				c->pc = target;
				cpu::YieldCheck(c);
			}
			if (!mem::IsValidAddress(target)) BadPc(c, target, pc);
			pc = target;
			continue;
		}

		case Kind::JAL: {
			u32 target = insn::JumpTarget(pc, w);
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
			if (rs != 31 && dispatch::Lookup(target)) {
				dispatch::Jump(c, target, ra0, sp0);
				return;
			}
			if (!mem::IsValidAddress(target)) BadPc(c, target, pc);
			pc = target;
			continue;
		}

		case Kind::JALR: {
			u32 target = c->r[insn::Rs(w)];
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
