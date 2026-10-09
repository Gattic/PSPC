// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// VFPU disassembly, after PPSSPP's Core/MIPS/MIPSDisVFPU.cpp. Register names use the usual
// notation: S<m><c><r> singles, C<m><c><r> column / R<m><r><c> row vectors, M<m><c><r> matrices and
// E<m><r><c> transposed matrices.
#include <cstdio>

#include "allegrex/decode.h"
#include "allegrex/insn.h"
#include "allegrex/vfpu_exec.h"

namespace ag {

const char *GprName(u32 r);  // disasm.cpp

namespace {

struct Name {
	char s[16];
};

Name VecName(u32 reg, int n) {
	Name out;
	const u32 mtx = (reg >> 2) & 7, col = reg & 3;
	u32 row;
	bool transpose = ((reg >> 5) & 1) != 0;
	char c = 'C';
	switch (n) {
	case 1:
		transpose = false;
		c = 'S';
		row = (reg >> 5) & 3;
		break;
	case 3: row = (reg >> 6) & 1; break;
	default: row = (reg >> 5) & 2; break;
	}
	if (transpose) c = 'R';
	if (transpose) snprintf(out.s, sizeof(out.s), "%c%u%u%u", c, mtx, row, col);
	else snprintf(out.s, sizeof(out.s), "%c%u%u%u", c, mtx, col, row);
	return out;
}

Name MtxName(u32 reg, int n) {
	if (n == 1) return VecName(reg, 1);
	Name out;
	const u32 mtx = (reg >> 2) & 7, col = reg & 3;
	const u32 row = n == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
	const bool transpose = ((reg >> 5) & 1) != 0;
	if (transpose) snprintf(out.s, sizeof(out.s), "E%u%u%u", mtx, row, col);
	else snprintf(out.s, sizeof(out.s), "M%u%u%u", mtx, col, row);
	return out;
}

const char *CtrlName(u32 reg) {
	static const char *const names[16] = {"SPFX", "TPFX", "DPFX", "CC",   "INF4", "RSV5", "RSV6", "REV",
	                                      "RCX0", "RCX1", "RCX2", "RCX3", "RCX4", "RCX5", "RCX6", "RCX7"};
	return reg < 16 ? names[reg] : nullptr;
}

Name CtrlOrNum(u32 reg) {
	Name out;
	if (const char *n = CtrlName(reg)) snprintf(out.s, sizeof(out.s), "%s", n);
	else if (reg == 127) snprintf(out.s, sizeof(out.s), "(interlock)");
	else snprintf(out.s, sizeof(out.s), "$%u", reg + 128);
	return out;
}

const char *Suffix(int n) {
	static const char *const s[5] = {"", ".s", ".p", ".t", ".q"};
	return s[n];
}

// S/T prefix lane list, e.g. [-y, |x|, 1/2, w].
void FormatPfxST(u32 data, char *out, size_t n) {
	static const char *const regnam[4] = {"x", "y", "z", "w"};
	static const char *const constan[8] = {"0", "1", "2", "1/2", "3", "1/3", "1/4", "1/6"};
	size_t pos = (size_t)snprintf(out, n, "[");
	for (int i = 0; i < 4 && pos < n; i++) {
		const int regnum = (data >> (i * 2)) & 3;
		const int abs = (data >> (8 + i)) & 1;
		const int negate = (data >> (16 + i)) & 1;
		const int constants = (data >> (12 + i)) & 1;
		const char *sep = i ? ", " : "";
		if (constants) {
			pos += (size_t)snprintf(out + pos, n - pos, "%s%s%s", sep, negate ? "-" : "", constan[regnum + (abs << 2)]);
		} else {
			pos += (size_t)snprintf(out + pos, n - pos, "%s%s%s%s%s", sep, negate ? "-" : "", abs ? "|" : "",
			                        regnam[regnum], abs ? "|" : "");
		}
	}
	if (pos < n) snprintf(out + pos, n - pos, "]");
}

// D prefix lane list: saturation 0:1 / -1:1 and m for a masked lane, e.g. [0:1, , m, -1:1].
void FormatPfxD(u32 data, char *out, size_t n) {
	static const char *const satNames[4] = {"", "0:1", "x", "-1:1"};
	size_t pos = (size_t)snprintf(out, n, "[");
	for (int i = 0; i < 4 && pos < n; i++) {
		const int sat = (data >> (i * 2)) & 3;
		const int mask = (data >> (8 + i)) & 1;
		pos += (size_t)snprintf(out + pos, n - pos, "%s%s%s%s", i ? ", " : "", satNames[sat], sat && mask ? " " : "",
		                        mask ? "m" : "");
	}
	if (pos < n) snprintf(out + pos, n - pos, "]");
}

const char *CstName(u32 k) {
	static const char *const names[20] = {
		"(undef)", "VFPU_HUGE", "VFPU_SQRT2",  "VFPU_SQRT1_2", "VFPU_2_SQRTPI", "VFPU_2_PI",     "VFPU_1_PI",
		"VFPU_PI_4", "VFPU_PI_2", "VFPU_PI",   "VFPU_E",       "VFPU_LOG2E",    "VFPU_LOG10E",   "VFPU_LN2",
		"VFPU_LN10", "VFPU_2PI",  "VFPU_PI_6", "VFPU_LOG10TWO", "VFPU_LOG2TEN", "VFPU_SQRT3_2",
	};
	return k < 20 ? names[k] : names[0];
}

}  // namespace

bool DisassembleVfpu(u32 pc, u32 w, Op op, char *out, size_t n) {
	using namespace vfpu;
	const char *mn = OpName(op);
	const int sz = VecSize(w);
	const char *sfx = Suffix(sz);
	const u32 vd = Vd(w), vs = Vs(w), vt = Vt(w);
	const char *rs = GprName(insn::Rs(w)), *rt = GprName(insn::Rt(w));

	switch (op) {
	case Op::MFV:
	case Op::MTV: snprintf(out, n, "%s %s, %s", mn, rt, VecName(w & 0x7F, 1).s); return true;
	case Op::MFVC:
	case Op::MTVC: snprintf(out, n, "%s %s, %s", mn, rt, CtrlOrNum((w & 0xFF) - 128).s); return true;
	case Op::BVF:
	case Op::BVT:
	case Op::BVFL:
	case Op::BVTL: snprintf(out, n, "%s %u, 0x%08x", mn, (w >> 18) & 7, insn::BranchTarget(pc, w)); return true;

	case Op::LV_S:
	case Op::SV_S: snprintf(out, n, "%s %s, %d(%s)", mn, VecName(LsRegS(w), 1).s, LsOffset(w), rs); return true;
	case Op::LV_Q:
	case Op::SV_Q:
		snprintf(out, n, "%s %s, %d(%s)%s", mn, VecName(LsRegQ(w), 4).s, LsOffset(w), rs, (w & 2) ? ", wb" : "");
		return true;
	case Op::LVL_Q:
	case Op::LVR_Q:
	case Op::SVL_Q:
	case Op::SVR_Q: snprintf(out, n, "%s %s, %d(%s)", mn, VecName(LsRegQ(w), 4).s, LsOffset(w), rs); return true;

	// vd, vs, vt of one size
	case Op::VADD:
	case Op::VSUB:
	case Op::VSBN:
	case Op::VDIV:
	case Op::VMUL:
	case Op::VCRS:
	case Op::VMIN:
	case Op::VMAX:
	case Op::VSCMP:
	case Op::VSGE:
	case Op::VSLT:
	case Op::VCRSP:
	case Op::VQMUL:
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, VecName(vd, sz).s, VecName(vs, sz).s, VecName(vt, sz).s);
		return true;
	// single result from two vectors
	case Op::VDOT:
	case Op::VHDP:
	case Op::VDET:
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, VecName(vd, 1).s, VecName(vs, sz).s, VecName(vt, sz).s);
		return true;
	case Op::VSCL:
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, VecName(vd, sz).s, VecName(vs, sz).s, VecName(vt, 1).s);
		return true;
	case Op::VCMP: {
		static const char *const conds[16] = {"FL", "EQ", "LT", "LE", "TR", "NE", "GE", "GT",
		                                      "EZ", "EN", "EI", "ES", "NZ", "NN", "NI", "NS"};
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, conds[w & 15], VecName(vs, sz).s, VecName(vt, sz).s);
		return true;
	}

	// vd, vs of one size
	case Op::VMOV:
	case Op::VABS:
	case Op::VNEG:
	case Op::VSAT0:
	case Op::VSAT1:
	case Op::VRCP:
	case Op::VRSQ:
	case Op::VSIN:
	case Op::VCOS:
	case Op::VEXP2:
	case Op::VLOG2:
	case Op::VSQRT:
	case Op::VASIN:
	case Op::VNRCP:
	case Op::VNSIN:
	case Op::VREXP2:
	case Op::VSBZ:
	case Op::VLGB:
	case Op::VSRT1:
	case Op::VSRT2:
	case Op::VSRT3:
	case Op::VSRT4:
	case Op::VBFY1:
	case Op::VBFY2:
	case Op::VOCP:
	case Op::VSGN: snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, sz).s, VecName(vs, sz).s); return true;
	// vd only
	case Op::VIDT:
	case Op::VZERO:
	case Op::VONE:
	case Op::VRNDI:
	case Op::VRNDF1:
	case Op::VRNDF2: snprintf(out, n, "%s%s %s", mn, sfx, VecName(vd, sz).s); return true;
	case Op::VRNDS: snprintf(out, n, "%s%s %s", mn, sfx, VecName(vs, 1).s); return true;
	case Op::VFAD:
	case Op::VAVG: snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, 1).s, VecName(vs, sz).s); return true;

	// Conversions with a different output size
	case Op::VF2H:
		snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, sz <= 2 ? 1 : 2).s, VecName(vs, sz).s);
		return true;
	case Op::VH2F:
		snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, sz == 1 ? 2 : 4).s, VecName(vs, sz).s);
		return true;
	case Op::VUC2I:
	case Op::VC2I: snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, 4).s, VecName(vs, sz).s); return true;
	case Op::VUS2I:
	case Op::VS2I:
	case Op::VSOCP:
		snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, sz == 1 ? 2 : 4).s, VecName(vs, sz).s);
		return true;
	case Op::VI2UC:
	case Op::VI2C: snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, 1).s, VecName(vs, sz).s); return true;
	case Op::VI2US:
	case Op::VI2S:
	case Op::VT4444:
	case Op::VT5551:
	case Op::VT5650:
		snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, sz >= 3 ? 2 : 1).s, VecName(vs, sz).s);
		return true;

	case Op::VMFVC: snprintf(out, n, "%s %s, %s", mn, VecName(vd, 1).s, CtrlOrNum((w >> 8) & 0x7F).s); return true;
	case Op::VMTVC: snprintf(out, n, "%s %s, %s", mn, VecName(vs, 1).s, CtrlOrNum(w & 0x7F).s); return true;
	case Op::VCST:
		snprintf(out, n, "%s%s %s, %s", mn, sfx, VecName(vd, sz).s, CstName((w >> 16) & 0x1F));
		return true;
	case Op::VF2IN:
	case Op::VF2IZ:
	case Op::VF2IU:
	case Op::VF2ID:
	case Op::VI2F:
		snprintf(out, n, "%s%s %s, %s, %u", mn, sfx, VecName(vd, sz).s, VecName(vs, sz).s, (w >> 16) & 0x1F);
		return true;
	case Op::VCMOVT:
	case Op::VCMOVF:
		snprintf(out, n, "%s%s %s, %s, %u", mn, sfx, VecName(vd, sz).s, VecName(vs, sz).s, (w >> 16) & 7);
		return true;
	case Op::VWBN:
		snprintf(out, n, "%s%s %s, %s, %u", mn, sfx, VecName(vd, sz).s, VecName(vs, sz).s, (w >> 16) & 0xFF);
		return true;

	case Op::VPFXS:
	case Op::VPFXT: {
		char buf[96];
		FormatPfxST(w & 0xFFFFF, buf, sizeof(buf));
		snprintf(out, n, "%s %s", mn, buf);
		return true;
	}
	case Op::VPFXD: {
		char buf[96];
		FormatPfxD(w & 0xFFF, buf, sizeof(buf));
		snprintf(out, n, "%s %s", mn, buf);
		return true;
	}
	case Op::VIIM: snprintf(out, n, "viim.s %s, %d", VecName(vt, 1).s, (int)(s16)(u16)(w & 0xFFFF)); return true;
	case Op::VFIM: {
		const float f = std::bit_cast<float>(Float16ToFloat32Bits((u16)(w & 0xFFFF)));
		snprintf(out, n, "vfim.s %s, %g", VecName(vt, 1).s, (double)f);
		return true;
	}

	case Op::VMMUL:
		// The first source is read transposed; show it that way, as PPSSPP does.
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, MtxName(vd, sz).s, MtxName(vs ^ 0x20, sz).s, MtxName(vt, sz).s);
		return true;
	case Op::VTFM:
	case Op::VHTFM: {
		// The matrix size is in bits 23-24; bits 7/15 give the vector size (one less if homogeneous).
		const int m = (int)((w >> 23) & 3) + 1;
		snprintf(out, n, "%s%d%s %s, %s, %s", mn, m, Suffix(m), VecName(vd, m).s, MtxName(vs, m).s, VecName(vt, m).s);
		return true;
	}
	case Op::VMSCL:
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, MtxName(vd, sz).s, MtxName(vs, sz).s, VecName(vt, 1).s);
		return true;
	case Op::VMMOV: snprintf(out, n, "%s%s %s, %s", mn, sfx, MtxName(vd, sz).s, MtxName(vs, sz).s); return true;
	case Op::VMIDT:
	case Op::VMZERO:
	case Op::VMONE: snprintf(out, n, "%s%s %s", mn, sfx, MtxName(vd, sz).s); return true;
	case Op::VROT: {
		const u32 imm = (w >> 16) & 0x1F;
		const bool negSin = (imm & 0x10) != 0;
		char lanes[5] = "0000";
		if (((imm >> 2) & 3) == (imm & 3)) {
			for (int i = 0; i < 4; i++) lanes[i] = 'S';
		}
		lanes[(imm >> 2) & 3] = 'S';
		lanes[imm & 3] = 'C';
		char temp[24];
		int pos = 0;
		temp[pos++] = '[';
		for (int i = 0; i < sz; i++) {
			if (lanes[i] == 'S' && negSin) temp[pos++] = '-';
			temp[pos++] = lanes[i];
			if (i != sz - 1) temp[pos++] = ',';
		}
		temp[pos++] = ']';
		temp[pos] = 0;
		snprintf(out, n, "%s%s %s, %s, %s", mn, sfx, VecName(vd, sz).s, VecName(vs, 1).s, temp);
		return true;
	}

	case Op::VNOP:
	case Op::VFLUSH: snprintf(out, n, "%s", mn); return true;
	case Op::VSYNC:
		if ((w & 0xFFFF) == 0x0320) snprintf(out, n, "vsync");
		else snprintf(out, n, "vsync 0x%04x", w & 0xFFFF);
		return true;
	case Op::VNOPX: snprintf(out, n, "vnopx 0x%08x", w); return true;
	default: return false;
	}
}

}  // namespace ag
