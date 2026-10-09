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
inline bool HasDelaySlot(Kind k) {
	return IsBranchKind(k) || k == Kind::J || k == Kind::JAL || k == Kind::JR || k == Kind::JALR;
}

}  // namespace ag
