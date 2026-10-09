// SPDX-License-Identifier: GPL-2.0-or-later
#include "allegrex/decode.h"

#include <cstdio>

#include "allegrex/insn.h"

namespace ag {

using namespace insn;

static Op DecodeSpecial(u32 w) {
	switch (Funct(w)) {
	case 0: return Op::SLL;
	case 2:
		if (Rs(w) == 0) return Op::SRL;
		if (Rs(w) == 1) return Op::ROTR;
		return Op::INVALID;
	case 3: return Op::SRA;
	case 4: return Op::SLLV;
	case 6:
		if (Sa(w) == 0) return Op::SRLV;
		if (Sa(w) == 1) return Op::ROTRV;
		return Op::INVALID;
	case 7: return Op::SRAV;
	case 8: return Op::JR;
	case 9: return Op::JALR;
	case 10: return Op::MOVZ;
	case 11: return Op::MOVN;
	case 12: return Op::SYSCALL;
	case 13: return Op::BREAK;
	case 15: return Op::SYNC;
	case 16: return Op::MFHI;
	case 17: return Op::MTHI;
	case 18: return Op::MFLO;
	case 19: return Op::MTLO;
	case 22: return Op::CLZ;
	case 23: return Op::CLO;
	case 24: return Op::MULT;
	case 25: return Op::MULTU;
	case 26: return Op::DIV;
	case 27: return Op::DIVU;
	case 28: return Op::MADD;
	case 29: return Op::MADDU;
	case 32: return Op::ADD;
	case 33: return Op::ADDU;
	case 34: return Op::SUB;
	case 35: return Op::SUBU;
	case 36: return Op::AND;
	case 37: return Op::OR;
	case 38: return Op::XOR;
	case 39: return Op::NOR;
	case 42: return Op::SLT;
	case 43: return Op::SLTU;
	case 44: return Op::MAX;
	case 45: return Op::MIN;
	case 46: return Op::MSUB;
	case 47: return Op::MSUBU;
	default: return Op::INVALID;
	}
}

static Op DecodeRegImm(u32 w) {
	switch (Rt(w)) {
	case 0: return Op::BLTZ;
	case 1: return Op::BGEZ;
	case 2: return Op::BLTZL;
	case 3: return Op::BGEZL;
	case 16: return Op::BLTZAL;
	case 17: return Op::BGEZAL;
	case 18: return Op::BLTZALL;
	case 19: return Op::BGEZALL;
	default: return Op::INVALID;
	}
}

static Op DecodeCop0(u32 w) {
	switch (Rs(w)) {
	case 0: return Op::MFC0;
	case 4: return Op::MTC0;
	case 16:
		if (Funct(w) == 0x18) return Op::ERET;
		return Op::COP0_NOP;
	default: return Op::COP0_NOP;
	}
}

static Op DecodeCop1(u32 w) {
	switch (Rs(w)) {
	case 0: return Op::MFC1;
	case 2: return Op::CFC1;
	case 4: return Op::MTC1;
	case 6: return Op::CTC1;
	case 8:
		switch (Rt(w) & 3) {
		case 0: return Op::BC1F;
		case 1: return Op::BC1T;
		case 2: return Op::BC1FL;
		default: return Op::BC1TL;
		}
	case 16:  // S format
		switch (Funct(w)) {
		case 0: return Op::ADD_S;
		case 1: return Op::SUB_S;
		case 2: return Op::MUL_S;
		case 3: return Op::DIV_S;
		case 4: return Op::SQRT_S;
		case 5: return Op::ABS_S;
		case 6: return Op::MOV_S;
		case 7: return Op::NEG_S;
		case 12: return Op::ROUND_W_S;
		case 13: return Op::TRUNC_W_S;
		case 14: return Op::CEIL_W_S;
		case 15: return Op::FLOOR_W_S;
		case 36: return Op::CVT_W_S;
		default:
			if (Funct(w) >= 48) return Op::C_COND_S;
			return Op::INVALID;
		}
	case 20:  // W format
		if (Funct(w) == 32) return Op::CVT_S_W;
		return Op::INVALID;
	default: return Op::INVALID;
	}
}

static Op DecodeSpecial2(u32 w) {
	switch (Funct(w)) {
	case 0: return Op::HALT;
	case 36: return Op::MFIC;
	case 38: return Op::MTIC;
	default: return Op::INVALID;
	}
}

static Op DecodeSpecial3(u32 w) {
	switch (Funct(w)) {
	case 0: return Op::EXT;
	case 4: return Op::INS;
	case 32:
		switch (Sa(w)) {
		case 2: return Op::WSBH;
		case 3: return Op::WSBW;
		case 16: return Op::SEB;
		case 20: return Op::BITREV;
		case 24: return Op::SEH;
		default: return Op::INVALID;
		}
	default: return Op::INVALID;
	}
}

Op Decode(u32 w) {
	switch (Opc(w)) {
	case 0: return DecodeSpecial(w);
	case 1: return DecodeRegImm(w);
	case 2: return Op::J;
	case 3: return Op::JAL;
	case 4: return Op::BEQ;
	case 5: return Op::BNE;
	case 6: return Op::BLEZ;
	case 7: return Op::BGTZ;
	case 8: return Op::ADDI;
	case 9: return Op::ADDIU;
	case 10: return Op::SLTI;
	case 11: return Op::SLTIU;
	case 12: return Op::ANDI;
	case 13: return Op::ORI;
	case 14: return Op::XORI;
	case 15: return Op::LUI;
	case 16: return DecodeCop0(w);
	case 17: return DecodeCop1(w);
	case 20: return Op::BEQL;
	case 21: return Op::BNEL;
	case 22: return Op::BLEZL;
	case 23: return Op::BGTZL;
	case 28: return DecodeSpecial2(w);
	case 31: return DecodeSpecial3(w);
	case 32: return Op::LB;
	case 33: return Op::LH;
	case 34: return Op::LWL;
	case 35: return Op::LW;
	case 36: return Op::LBU;
	case 37: return Op::LHU;
	case 38: return Op::LWR;
	case 40: return Op::SB;
	case 41: return Op::SH;
	case 42: return Op::SWL;
	case 43: return Op::SW;
	case 46: return Op::SWR;
	case 47: return Op::CACHE;
	case 48: return Op::LL;
	case 49: return Op::LWC1;
	case 56: return Op::SC;
	case 57: return Op::SWC1;
	case 18:
	case 24:
	case 25:
	case 27:
	case 50:
	case 52:
	case 53:
	case 54:
	case 55:
	case 58:
	case 60:
	case 61:
	case 62:
	case 63: return DecodeVfpu(w);
	default: return Op::INVALID;
	}
}

static const Kind s_kinds[] = {
#define OP(id, mn, kind) Kind::kind,
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP
	Kind::INVALID,
};

static const char *const s_names[] = {
#define OP(id, mn, kind) mn,
#include "allegrex/ops.inc"
#include "allegrex/vfpu_ops.inc"
#undef OP
	"invalid",
};

static_assert(ArraySize(s_kinds) == (size_t)Op::COUNT, "kind table size");
static_assert(ArraySize(s_names) == (size_t)Op::COUNT, "name table size");

Kind OpKind(Op op) { return s_kinds[(size_t)op]; }
const char *OpName(Op op) { return s_names[(size_t)op]; }

}  // namespace ag
