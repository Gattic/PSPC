// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/common.h"

namespace ag {

enum class Op : u16 {
#define OP(id, mn, kind) id,
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP
	INVALID,
	COUNT
};

enum class Kind : u8 {
	ALU,
	BR,
	BRL,
	BRAL,
	BRALL,
	J,
	JAL,
	JR,
	JALR,
	SYSCALL,
	BREAK,
	ERET,
	INVALID,
};

Op Decode(u32 w);
// VFPU decoding (vfpu_decode.cpp). Called for primary opcodes 18, 24, 25, 27, 50, 52-55, 58, 60-63.
Op DecodeVfpu(u32 w);

Kind OpKind(Op op);
const char *OpName(Op op);
// Text form of a register-transfer free instruction, for logs and generated comments.
void Disassemble(u32 pc, u32 w, char *out, size_t outSize);

inline bool IsBranchKind(Kind k) {
	return k == Kind::BR || k == Kind::BRL || k == Kind::BRAL || k == Kind::BRALL;
}
inline bool IsLikelyKind(Kind k) { return k == Kind::BRL || k == Kind::BRALL; }

// Condition hazards (cpu/fpu/fpu_branch_hazard, cpu/vfpu/vbranch_hazard). Compilers pad for
// them, so only hand-written code sees them:
//   * bc1f/bc1t (and the likely forms) right behind c.cond.s test the condition from before
//     the compare;
//   * bvf/bvt (and the likely forms) test the CC from before an mtvc to it within the three
//     instructions before the branch, unless a vcmp is among them (the branch then waits for the
//     VFPU and sees everything);
//   * mfvc of the CC right behind vcmp reads the CC from before the vcmp.
// The rule is static, from the words before the reader, so the interpreter and recompiled code
// agree; the values from before live in Ctx::fpcondPrev / Ctx::ccPrev.
inline bool MaySeeStaleCondition(Op op) {
	return (op >= Op::BC1F && op <= Op::BC1TL) || (op >= Op::BVF && op <= Op::BVTL) || op == Op::MFVC;
}
// w is the reader, w1..w3 the words 4, 8 and 12 bytes before it.
bool SeesStaleCondition(Op op, u32 w, u32 w1, u32 w2, u32 w3);
inline bool HasDelaySlot(Kind k) {
	return IsBranchKind(k) || k == Kind::J || k == Kind::JAL || k == Kind::JR || k == Kind::JALR;
}

}  // namespace ag
