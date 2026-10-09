// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>

#include "allegrex/decode.h"
#include "allegrex/insn.h"

namespace ag {

// Implemented by the VFPU unit; returns false when it doesn't format the instruction itself.
bool DisassembleVfpu(u32 pc, u32 w, Op op, char *out, size_t outSize);

static const char *const kGpr[32] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

const char *GprName(u32 r) { return kGpr[r & 31]; }

void Disassemble(u32 pc, u32 w, char *out, size_t n) {
	using namespace insn;
	Op op = Decode(w);
	const char *mn = OpName(op);
	const char *rs = kGpr[Rs(w)], *rt = kGpr[Rt(w)], *rd = kGpr[Rd(w)];
	if (w == 0) {
		snprintf(out, n, "nop");
		return;
	}
	switch (op) {
	case Op::SLL:
	case Op::SRL:
	case Op::SRA:
	case Op::ROTR: snprintf(out, n, "%s %s, %s, %u", mn, rd, rt, Sa(w)); return;
	case Op::SLLV:
	case Op::SRLV:
	case Op::SRAV:
	case Op::ROTRV: snprintf(out, n, "%s %s, %s, %s", mn, rd, rt, rs); return;
	case Op::JR: snprintf(out, n, "jr %s", rs); return;
	case Op::JALR: snprintf(out, n, "jalr %s, %s", rd, rs); return;
	case Op::SYSCALL:
	case Op::BREAK: snprintf(out, n, "%s 0x%05x", mn, (w >> 6) & 0xFFFFF); return;
	case Op::MFHI:
	case Op::MFLO: snprintf(out, n, "%s %s", mn, rd); return;
	case Op::MTHI:
	case Op::MTLO: snprintf(out, n, "%s %s", mn, rs); return;
	case Op::CLZ:
	case Op::CLO: snprintf(out, n, "%s %s, %s", mn, rd, rs); return;
	case Op::MULT:
	case Op::MULTU:
	case Op::DIV:
	case Op::DIVU:
	case Op::MADD:
	case Op::MADDU:
	case Op::MSUB:
	case Op::MSUBU: snprintf(out, n, "%s %s, %s", mn, rs, rt); return;
	case Op::MOVZ:
	case Op::MOVN:
	case Op::ADD:
	case Op::ADDU:
	case Op::SUB:
	case Op::SUBU:
	case Op::AND:
	case Op::OR:
	case Op::XOR:
	case Op::NOR:
	case Op::SLT:
	case Op::SLTU:
	case Op::MAX:
	case Op::MIN: snprintf(out, n, "%s %s, %s, %s", mn, rd, rs, rt); return;
	case Op::BEQ:
	case Op::BNE:
	case Op::BEQL:
	case Op::BNEL:
		snprintf(out, n, "%s %s, %s, 0x%08x", mn, rs, rt, BranchTarget(pc, w));
		return;
	case Op::BLEZ:
	case Op::BGTZ:
	case Op::BLEZL:
	case Op::BGTZL:
	case Op::BLTZ:
	case Op::BGEZ:
	case Op::BLTZL:
	case Op::BGEZL:
	case Op::BLTZAL:
	case Op::BGEZAL:
	case Op::BLTZALL:
	case Op::BGEZALL: snprintf(out, n, "%s %s, 0x%08x", mn, rs, BranchTarget(pc, w)); return;
	case Op::BC1F:
	case Op::BC1T:
	case Op::BC1FL:
	case Op::BC1TL: snprintf(out, n, "%s 0x%08x", mn, BranchTarget(pc, w)); return;
	case Op::J:
	case Op::JAL: snprintf(out, n, "%s 0x%08x", mn, JumpTarget(pc, w)); return;
	case Op::ADDI:
	case Op::ADDIU:
	case Op::SLTI:
	case Op::SLTIU: snprintf(out, n, "%s %s, %s, %d", mn, rt, rs, Simm16(w)); return;
	case Op::ANDI:
	case Op::ORI:
	case Op::XORI: snprintf(out, n, "%s %s, %s, 0x%04x", mn, rt, rs, Imm16(w)); return;
	case Op::LUI: snprintf(out, n, "lui %s, 0x%04x", rt, Imm16(w)); return;
	case Op::LB:
	case Op::LH:
	case Op::LWL:
	case Op::LW:
	case Op::LBU:
	case Op::LHU:
	case Op::LWR:
	case Op::SB:
	case Op::SH:
	case Op::SWL:
	case Op::SW:
	case Op::SWR:
	case Op::LL:
	case Op::SC: snprintf(out, n, "%s %s, %d(%s)", mn, rt, Simm16(w), rs); return;
	case Op::CACHE: snprintf(out, n, "cache 0x%02x, %d(%s)", Rt(w), Simm16(w), rs); return;
	case Op::LWC1:
	case Op::SWC1: snprintf(out, n, "%s f%u, %d(%s)", mn, Ft(w), Simm16(w), rs); return;
	case Op::MFC1:
	case Op::MTC1: snprintf(out, n, "%s %s, f%u", mn, rt, Fs(w)); return;
	case Op::CFC1:
	case Op::CTC1: snprintf(out, n, "%s %s, fcr%u", mn, rt, Fs(w)); return;
	case Op::MFC0:
	case Op::MTC0: snprintf(out, n, "%s %s, cop0_%u", mn, rt, Rd(w)); return;
	case Op::ADD_S:
	case Op::SUB_S:
	case Op::MUL_S:
	case Op::DIV_S: snprintf(out, n, "%s f%u, f%u, f%u", mn, Fd(w), Fs(w), Ft(w)); return;
	case Op::SQRT_S:
	case Op::ABS_S:
	case Op::MOV_S:
	case Op::NEG_S:
	case Op::ROUND_W_S:
	case Op::TRUNC_W_S:
	case Op::CEIL_W_S:
	case Op::FLOOR_W_S:
	case Op::CVT_S_W:
	case Op::CVT_W_S: snprintf(out, n, "%s f%u, f%u", mn, Fd(w), Fs(w)); return;
	case Op::C_COND_S: {
		static const char *const conds[16] = {"f", "un", "eq", "ueq", "olt", "ult", "ole", "ule",
		                                      "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"};
		snprintf(out, n, "c.%s.s f%u, f%u", conds[w & 15], Fs(w), Ft(w));
		return;
	}
	case Op::MFIC:
	case Op::MTIC: snprintf(out, n, "%s %s", mn, rt); return;
	case Op::EXT: snprintf(out, n, "ext %s, %s, %u, %u", rt, rs, Pos(w), Size(w) + 1); return;
	case Op::INS:
		snprintf(out, n, "ins %s, %s, %u, %u", rt, rs, Pos(w), Size(w) + 1 - Pos(w));
		return;
	case Op::WSBH:
	case Op::WSBW:
	case Op::SEB:
	case Op::SEH:
	case Op::BITREV: snprintf(out, n, "%s %s, %s", mn, rd, rt); return;
	case Op::INVALID: snprintf(out, n, "invalid 0x%08x", w); return;
	default: break;
	}
	if (!DisassembleVfpu(pc, w, op, out, n)) {
		snprintf(out, n, "%s 0x%08x", mn, w);
	}
}

}  // namespace ag
