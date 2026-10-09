// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// VFPU semantics: sem_V*/cond_BV* for every op in vfpu_ops.inc. Included at the end of sem.h.
//
// The common instructions get an inline fast path for the state that dominates real code (all
// three prefixes at their defaults, no write mask). Everything else, including every op with a
// pending prefix, goes to the out-of-line executors in vfpu_exec.cpp, which implement PPSSPP's
// prefix rules completely. The fast paths are bit-identical to those executors by construction.
//
// Prefix consumption: an op that "eats" prefixes resets S/T to 0xE4 and D to 0 afterwards. The
// fast paths only run when the prefixes already have those values, so they skip the reset.
// vpfx*, loads/stores, mfv/mtv/mfvc/mtvc, vmfvc/vmtvc, branches, vnop/vsync/vflush leave them.
#pragma once

#include "allegrex/vfpu_exec.h"

// Recompiled code is built with clang-cl; keep it from fusing multiply-adds, so it rounds like the
// interpreter.
#if defined(__clang__)
#define VFPU_NO_CONTRACT _Pragma("clang fp contract(off)")
#else
#define VFPU_NO_CONTRACT
#endif

namespace vfpu {

PSPC_FORCEINLINE u32 LsAddr(const Ctx *c, u32 w) { return c->r[(w >> 21) & 31] + (u32)LsOffset(w); }

// Element-wise vs (op) vt with default prefixes.
template <typename Fn>
PSPC_FORCEINLINE void FastBinary(Ctx *c, u32 w, Fn fn) {
	const int n = VecSize(w);
	float d[4];
	for (int i = 0; i < n; i++) d[i] = fn(c->v[VecElem(Vs(w), n, i)], c->v[VecElem(Vt(w), n, i)]);
	for (int i = 0; i < n; i++) c->v[VecElem(Vd(w), n, i)] = d[i];
}

// Element-wise unary op on the bits of vs with default prefixes.
template <typename Fn>
PSPC_FORCEINLINE void FastUnaryBits(Ctx *c, u32 w, Fn fn) {
	const int n = VecSize(w);
	u32 d[4];
	for (int i = 0; i < n; i++) d[i] = fn(c->vi[VecElem(Vs(w), n, i)]);
	for (int i = 0; i < n; i++) c->vi[VecElem(Vd(w), n, i)] = d[i];
}

PSPC_FORCEINLINE void FastFill(Ctx *c, u32 w, u32 bits) {
	const int n = VecSize(w);
	for (int i = 0; i < n; i++) c->vi[VecElem(Vd(w), n, i)] = bits;
}

}  // namespace vfpu

// ---- COP2: transfers and branches ----
SEM(MFV) { sem::SetR(c, insn::Rt(w), c->vi[vfpu::VOffset(w & 0x7F)]); }
SEM(MFVC) {
	// mfvc zero, $255 is used as a pipeline interlock; registers past 15 read nothing.
	const u32 reg = (w & 0xFF) - 128;
	if (reg < 16) sem::SetR(c, insn::Rt(w), c->vfpuCtrl[reg]);
}
SEM(MTV) { c->vi[vfpu::VOffset(w & 0x7F)] = c->r[insn::Rt(w)]; }
SEM(MTVC) { vfpu::WriteCtrl(c, (w & 0xFF) - 128, c->r[insn::Rt(w)]); }

// Condition bit imm3 (bits 18-20) of the CC register: x, y, z, w, any, all.
#define VFPU_CC_BIT_ ((c->vfpuCtrl[VFPU_CTRL_CC] >> ((w >> 18) & 7)) & 1)
COND(BVF) { return VFPU_CC_BIT_ == 0; }
COND(BVT) { return VFPU_CC_BIT_ != 0; }
COND(BVFL) { return VFPU_CC_BIT_ == 0; }
COND(BVTL) { return VFPU_CC_BIT_ != 0; }
#undef VFPU_CC_BIT_

// ---- Loads / stores ----
SEM(LV_S) { c->vi[vfpu::VOffset(vfpu::LsRegS(w))] = GRead32(vfpu::LsAddr(c, w)); }
SEM(SV_S) { GWrite32(vfpu::LsAddr(c, w), c->vi[vfpu::VOffset(vfpu::LsRegS(w))]); }
SEM(LV_Q) {
	// The D write mask applies to quad loads (PPSSPP WriteVector).
	if (PSPC_LIKELY((c->vfpuCtrl[VFPU_CTRL_DPREFIX] & 0xF00) == 0)) {
		const u32 addr = vfpu::LsAddr(c, w), vt = vfpu::LsRegQ(w);
		if (vfpu::VecElem(vt, 4, 0) + 3 == vfpu::VecElem(vt, 4, 3) && !vfpu::Transposed(vt, 4)) {
			memcpy(&c->vi[vfpu::VecElem(vt, 4, 0)], GPtr(addr), 16);
		} else {
			for (int i = 0; i < 4; i++) c->vi[vfpu::VecElem(vt, 4, i)] = GRead32(addr + 4 * i);
		}
	} else {
		vfpu::ExecLvq(c, w);
	}
}
SEM(SV_Q) {
	// Bit 1 is the write-back flag ("sv.q ..., wb"); it doesn't change the store.
	const u32 addr = vfpu::LsAddr(c, w), vt = vfpu::LsRegQ(w);
	if (vfpu::VecElem(vt, 4, 0) + 3 == vfpu::VecElem(vt, 4, 3) && !vfpu::Transposed(vt, 4)) {
		memcpy(GPtr(addr), &c->vi[vfpu::VecElem(vt, 4, 0)], 16);
	} else {
		for (int i = 0; i < 4; i++) GWrite32(addr + 4 * i, c->vi[vfpu::VecElem(vt, 4, i)]);
	}
}
SEM(LVL_Q) { vfpu::ExecLvlq(c, w); }
SEM(LVR_Q) { vfpu::ExecLvrq(c, w); }
SEM(SVL_Q) { vfpu::ExecSvlq(c, w); }
SEM(SVR_Q) { vfpu::ExecSvrq(c, w); }

// ---- VFPU0 / VFPU1 ----
SEM(VADD) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastBinary(c, w, [](float a, float b) { return a + b; });
	else vfpu::ExecVecDo3(c, w);
}
SEM(VSUB) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastBinary(c, w, [](float a, float b) { return a - b; });
	else vfpu::ExecVecDo3(c, w);
}
SEM(VDIV) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastBinary(c, w, [](float a, float b) { return a / b; });
	else vfpu::ExecVecDo3(c, w);
}
SEM(VMUL) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastBinary(c, w, [](float a, float b) { return a * b; });
	else vfpu::ExecVecDo3(c, w);
}
SEM(VSBN) { vfpu::ExecVsbn(c, w); }
SEM(VDOT) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) {
		// PPSSPP's sum: four products in order, starting from +0, zero-padded past the length.
		const int n = vfpu::VecSize(w);
		float s[4] = {}, t[4] = {};
		for (int i = 0; i < n; i++) {
			s[i] = c->v[vfpu::VecElem(vfpu::Vs(w), n, i)];
			t[i] = c->v[vfpu::VecElem(vfpu::Vt(w), n, i)];
		}
		float sum = 0.0f;
		for (int i = 0; i < 4; i++) {
			const float p = s[i] * t[i];
			sum = sum + p;
		}
		c->v[vfpu::VOffset(vfpu::Vd(w))] = sum;
	} else {
		vfpu::ExecVDot(c, w);
	}
}
SEM(VSCL) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) {
		const float k = c->v[vfpu::VOffset(vfpu::Vt(w))];
		const int n = vfpu::VecSize(w);
		float d[4];
		for (int i = 0; i < n; i++) d[i] = c->v[vfpu::VecElem(vfpu::Vs(w), n, i)] * k;
		for (int i = 0; i < n; i++) c->v[vfpu::VecElem(vfpu::Vd(w), n, i)] = d[i];
	} else {
		vfpu::ExecVScl(c, w);
	}
}
SEM(VHDP) { vfpu::ExecVHdp(c, w); }
SEM(VCRS) { vfpu::ExecVcrs(c, w); }
SEM(VDET) { vfpu::ExecVdet(c, w); }

// ---- VFPU3 ----
SEM(VCMP) { vfpu::ExecVcmp(c, w); }
SEM(VMIN) { vfpu::ExecVminmax(c, w); }
SEM(VMAX) { vfpu::ExecVminmax(c, w); }
SEM(VSCMP) { vfpu::ExecVscmp(c, w); }
SEM(VSGE) { vfpu::ExecVsge(c, w); }
SEM(VSLT) { vfpu::ExecVslt(c, w); }

// ---- VFPU4 (opcode 52, rs = 0) ----
SEM(VMOV) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastUnaryBits(c, w, [](u32 x) { return x; });
	else vfpu::ExecVV2Op(c, w);
}
SEM(VABS) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastUnaryBits(c, w, [](u32 x) { return x & 0x7FFFFFFFu; });
	else vfpu::ExecVV2Op(c, w);
}
SEM(VNEG) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastUnaryBits(c, w, [](u32 x) { return x ^ 0x80000000u; });
	else vfpu::ExecVV2Op(c, w);
}
SEM(VIDT) { vfpu::ExecVidt(c, w); }
SEM(VSAT0) { vfpu::ExecVV2Op(c, w); }
SEM(VSAT1) { vfpu::ExecVV2Op(c, w); }
SEM(VZERO) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastFill(c, w, 0);
	else vfpu::ExecVVectorInit(c, w);
}
SEM(VONE) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastFill(c, w, 0x3F800000u);
	else vfpu::ExecVVectorInit(c, w);
}
SEM(VRCP) { vfpu::ExecVV2Op(c, w); }
SEM(VRSQ) { vfpu::ExecVV2Op(c, w); }
SEM(VSIN) { vfpu::ExecVV2Op(c, w); }
SEM(VCOS) { vfpu::ExecVV2Op(c, w); }
SEM(VEXP2) { vfpu::ExecVV2Op(c, w); }
SEM(VLOG2) { vfpu::ExecVV2Op(c, w); }
SEM(VSQRT) { vfpu::ExecVV2Op(c, w); }
SEM(VASIN) { vfpu::ExecVV2Op(c, w); }
SEM(VNRCP) { vfpu::ExecVV2Op(c, w); }
SEM(VNSIN) { vfpu::ExecVV2Op(c, w); }
SEM(VREXP2) { vfpu::ExecVV2Op(c, w); }

// ---- VFPU7 (opcode 52, rs = 1) ----
SEM(VRNDS) { vfpu::ExecVrnds(c, w); }
SEM(VRNDI) { vfpu::ExecVrndX(c, w); }
SEM(VRNDF1) { vfpu::ExecVrndX(c, w); }
SEM(VRNDF2) { vfpu::ExecVrndX(c, w); }
SEM(VF2H) { vfpu::ExecVf2h(c, w); }
SEM(VH2F) { vfpu::ExecVh2f(c, w); }
SEM(VSBZ) { vfpu::ExecVsbz(c, w); }
SEM(VLGB) { vfpu::ExecVlgb(c, w); }
SEM(VUC2I) { vfpu::ExecVx2i(c, w); }
SEM(VC2I) { vfpu::ExecVx2i(c, w); }
SEM(VUS2I) { vfpu::ExecVx2i(c, w); }
SEM(VS2I) { vfpu::ExecVx2i(c, w); }
SEM(VI2UC) { vfpu::ExecVi2x(c, w); }
SEM(VI2C) { vfpu::ExecVi2x(c, w); }
SEM(VI2US) { vfpu::ExecVi2x(c, w); }
SEM(VI2S) { vfpu::ExecVi2x(c, w); }

// ---- VFPU9 (opcode 52, rs = 2) ----
SEM(VSRT1) { vfpu::ExecVsrt1(c, w); }
SEM(VSRT2) { vfpu::ExecVsrt2(c, w); }
SEM(VBFY1) { vfpu::ExecVbfy(c, w); }
SEM(VBFY2) { vfpu::ExecVbfy(c, w); }
SEM(VOCP) { vfpu::ExecVocp(c, w); }
SEM(VSOCP) { vfpu::ExecVsocp(c, w); }
SEM(VFAD) { vfpu::ExecVfad(c, w); }
SEM(VAVG) { vfpu::ExecVavg(c, w); }
SEM(VSRT3) { vfpu::ExecVsrt3(c, w); }
SEM(VSRT4) { vfpu::ExecVsrt4(c, w); }
SEM(VSGN) { vfpu::ExecVsgn(c, w); }
// Control register moves leave the prefixes alone.
SEM(VMFVC) {
	const u32 reg = (w >> 8) & 0x7F;
	c->vi[vfpu::VOffset(vfpu::Vd(w))] = reg < 16 ? c->vfpuCtrl[reg] : 0;
}
SEM(VMTVC) { vfpu::WriteCtrl(c, w & 0x7F, c->vi[vfpu::VOffset(vfpu::Vs(w))]); }
SEM(VT4444) { vfpu::ExecColorConv(c, w); }
SEM(VT5551) { vfpu::ExecColorConv(c, w); }
SEM(VT5650) { vfpu::ExecColorConv(c, w); }

// ---- Opcode 52, rs >= 3 ----
SEM(VCST) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) vfpu::FastFill(c, w, vfpu::kCstBits[(w >> 16) & 0x1F]);
	else vfpu::ExecVcst(c, w);
}
SEM(VF2IN) { vfpu::ExecVf2i(c, w); }
SEM(VF2IZ) { vfpu::ExecVf2i(c, w); }
SEM(VF2IU) { vfpu::ExecVf2i(c, w); }
SEM(VF2ID) { vfpu::ExecVf2i(c, w); }
SEM(VI2F) {
	VFPU_NO_CONTRACT
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) {
		const float mult = 1.0f / (float)(1ULL << ((w >> 16) & 0x1F));
		const int n = vfpu::VecSize(w);
		float d[4];
		for (int i = 0; i < n; i++) d[i] = (float)(s32)c->vi[vfpu::VecElem(vfpu::Vs(w), n, i)] * mult;
		for (int i = 0; i < n; i++) c->v[vfpu::VecElem(vfpu::Vd(w), n, i)] = d[i];
	} else {
		vfpu::ExecVi2f(c, w);
	}
}
SEM(VCMOVT) { vfpu::ExecVcmov(c, w); }
SEM(VCMOVF) { vfpu::ExecVcmov(c, w); }
SEM(VWBN) { vfpu::ExecVwbn(c, w); }

// ---- VFPU5 (opcode 55) ----
SEM(VPFXS) { c->vfpuCtrl[VFPU_CTRL_SPREFIX] = w & 0x000FFFFF; }
SEM(VPFXT) { c->vfpuCtrl[VFPU_CTRL_TPREFIX] = w & 0x000FFFFF; }
SEM(VPFXD) { c->vfpuCtrl[VFPU_CTRL_DPREFIX] = w & 0x00000FFF; }
SEM(VIIM) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) {
		c->v[vfpu::VOffset(vfpu::Vt(w))] = (float)(s32)(s16)(u16)(w & 0xFFFF);
	} else {
		vfpu::ExecViim(c, w);
	}
}
SEM(VFIM) {
	if (PSPC_LIKELY(vfpu::HasDefaultPrefixes(c))) {
		c->vi[vfpu::VOffset(vfpu::Vt(w))] = vfpu::Float16ToFloat32Bits((u16)(w & 0xFFFF));
	} else {
		vfpu::ExecViim(c, w);
	}
}

// ---- VFPU6 (opcode 60) ----
SEM(VMMUL) { vfpu::ExecVmmul(c, w); }
SEM(VTFM) { vfpu::ExecVtfm(c, w); }
SEM(VHTFM) { vfpu::ExecVtfm(c, w); }
SEM(VMSCL) { vfpu::ExecVmscl(c, w); }
SEM(VCRSP) { vfpu::ExecCrossQuat(c, w); }
SEM(VQMUL) { vfpu::ExecCrossQuat(c, w); }
SEM(VMMOV) { vfpu::ExecVmmov(c, w); }
SEM(VMIDT) { vfpu::ExecVMatrixInit(c, w); }
SEM(VMZERO) { vfpu::ExecVMatrixInit(c, w); }
SEM(VMONE) { vfpu::ExecVMatrixInit(c, w); }
SEM(VROT) { vfpu::ExecVrot(c, w); }

// ---- Opcode 63 ----
// vnop / vsync / vflush (0xFFFFxxxx) keep the prefixes; any other opcode-63 word is a nop that
// consumes them (PPSSPP Int_Vflush).
SEM(VNOP) {
	(void)c;
	(void)w;
}
SEM(VSYNC) {
	(void)c;
	(void)w;
}
SEM(VFLUSH) {
	(void)c;
	(void)w;
}
SEM(VNOPX) {
	(void)w;
	vfpu::EatPrefixes(c);
}

#undef VFPU_NO_CONTRACT
