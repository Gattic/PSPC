// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// VFPU decoding, following the dispatch tables in PPSSPP's Core/MIPS/MIPSTables.cpp. Encodings
// PPSSPP has no interpreter for (mfc2/cfc2/mtc2/ctc2 and the other COP2 rs values) are INVALID.
#include "allegrex/decode.h"
#include "allegrex/vfpu_regs.h"

namespace ag {

// COP2 (opcode 18): 010010 rs(5) ...
static Op DecodeCop2(u32 w) {
	switch ((w >> 21) & 31) {
	case 3: return (w & 0x80) ? Op::MFVC : Op::MFV;
	case 7: return (w & 0x80) ? Op::MTVC : Op::MTV;
	case 8:
		switch ((w >> 16) & 3) {
		case 0: return Op::BVF;
		case 1: return Op::BVT;
		case 2: return Op::BVFL;
		default: return Op::BVTL;
		}
	default: return Op::INVALID;
	}
}

// 110100 00000 xxxxx
static Op DecodeVfpu4(u32 w) {
	static const Op table[32] = {
		Op::VMOV, Op::VABS, Op::VNEG, Op::VIDT, Op::VSAT0, Op::VSAT1, Op::VZERO, Op::VONE,
		Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
		Op::VRCP, Op::VRSQ, Op::VSIN, Op::VCOS, Op::VEXP2, Op::VLOG2, Op::VSQRT, Op::VASIN,
		Op::VNRCP, Op::INVALID, Op::VNSIN, Op::INVALID, Op::VREXP2, Op::INVALID, Op::INVALID, Op::INVALID,
	};
	return table[(w >> 16) & 31];
}

// 110100 00001 xxxxx
static Op DecodeVfpu7(u32 w) {
	static const Op table[32] = {
		Op::VRNDS, Op::VRNDI, Op::VRNDF1, Op::VRNDF2, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
		Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
		Op::INVALID, Op::INVALID, Op::VF2H, Op::VH2F, Op::INVALID, Op::INVALID, Op::VSBZ, Op::VLGB,
		Op::VUC2I, Op::VC2I, Op::VUS2I, Op::VS2I, Op::VI2UC, Op::VI2C, Op::VI2US, Op::VI2S,
	};
	return table[(w >> 16) & 31];
}

// 110100 00010 xxxxx
static Op DecodeVfpu9(u32 w) {
	static const Op table[32] = {
		Op::VSRT1, Op::VSRT2, Op::VBFY1, Op::VBFY2, Op::VOCP, Op::VSOCP, Op::VFAD, Op::VAVG,
		Op::VSRT3, Op::VSRT4, Op::VSGN, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
		Op::VMFVC, Op::VMTVC, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
		Op::INVALID, Op::VT4444, Op::VT5551, Op::VT5650, Op::INVALID, Op::INVALID, Op::INVALID, Op::INVALID,
	};
	return table[(w >> 16) & 31];
}

// Opcode 52: 110100 xxxxx
static Op DecodeVfpu4Jump(u32 w) {
	const u32 rs = (w >> 21) & 31;
	switch (rs) {
	case 0: return DecodeVfpu4(w);
	case 1: return DecodeVfpu7(w);
	case 2: return DecodeVfpu9(w);
	case 3: return Op::VCST;
	case 16: return Op::VF2IN;
	case 17: return Op::VF2IZ;
	case 18: return Op::VF2IU;
	case 19: return Op::VF2ID;
	case 20: return Op::VI2F;
	case 21: return ((w >> 19) & 1) ? Op::VCMOVF : Op::VCMOVT;
	default: return rs >= 24 ? Op::VWBN : Op::INVALID;
	}
}

// Opcode 60: 111100 xxxxx (bits 21-22 are the top of vt for most of these)
static Op DecodeVfpu6(u32 w) {
	const u32 rs = (w >> 21) & 31;
	if (rs < 4) return Op::VMMUL;
	if (rs < 16) {
		// vtfm2/3/4 (bits 23-24 give the matrix size); the vector size says homogeneous or not.
		const int ins = (int)((w >> 23) & 3);
		return vfpu::VecSize(w) > ins ? Op::VTFM : Op::VHTFM;
	}
	if (rs < 20) return Op::VMSCL;
	if (rs < 24) return vfpu::VecSize(w) == 4 ? Op::VQMUL : Op::VCRSP;
	if (rs == 28) {
		switch ((w >> 16) & 0xF) {
		case 0: return Op::VMMOV;
		case 3: return Op::VMIDT;
		case 6: return Op::VMZERO;
		case 7: return Op::VMONE;
		default: return Op::INVALID;
		}
	}
	if (rs == 29) return Op::VROT;
	return Op::INVALID;
}

Op DecodeVfpu(u32 w) {
	switch (w >> 26) {
	case 18: return DecodeCop2(w);
	case 24:
		switch ((w >> 23) & 7) {
		case 0: return Op::VADD;
		case 1: return Op::VSUB;
		case 2: return Op::VSBN;
		case 7: return Op::VDIV;
		default: return Op::INVALID;
		}
	case 25:
		switch ((w >> 23) & 7) {
		case 0: return Op::VMUL;
		case 1: return Op::VDOT;
		case 2: return Op::VSCL;
		case 4: return Op::VHDP;
		case 5: return Op::VCRS;
		case 6: return Op::VDET;
		default: return Op::INVALID;
		}
	case 27:
		switch ((w >> 23) & 7) {
		case 0: return Op::VCMP;
		case 2: return Op::VMIN;
		case 3: return Op::VMAX;
		case 5: return Op::VSCMP;
		case 6: return Op::VSGE;
		case 7: return Op::VSLT;
		default: return Op::INVALID;
		}
	case 50: return Op::LV_S;
	case 52: return DecodeVfpu4Jump(w);
	case 53: return (w & 2) ? Op::LVR_Q : Op::LVL_Q;
	case 54: return Op::LV_Q;
	case 55:
		switch ((w >> 23) & 7) {
		case 0:
		case 1: return Op::VPFXS;
		case 2:
		case 3: return Op::VPFXT;
		case 4:
		case 5: return Op::VPFXD;
		case 6: return Op::VIIM;
		default: return Op::VFIM;
		}
	case 58: return Op::SV_S;
	case 60: return DecodeVfpu6(w);
	case 61: return (w & 2) ? Op::SVR_Q : Op::SVL_Q;
	case 62: return Op::SV_Q;
	case 63:
		if (w == 0xFFFF0000u) return Op::VNOP;
		if (w == 0xFFFF040Du) return Op::VFLUSH;
		if ((w >> 16) == 0xFFFF) return Op::VSYNC;
		return Op::VNOPX;
	default: return Op::INVALID;
	}
}

}  // namespace ag
