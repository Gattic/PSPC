// SPDX-License-Identifier: GPL-2.0-or-later
//
// Instruction semantics for the integer, FPU and control-register instructions.
//
// Every non-control-flow instruction ID in ops.inc has sem_ID(Ctx*, u32 insn); every branch has
// cond_ID(Ctx*, u32 insn). The interpreter calls these with the fetched word and the recompiler
// emits calls with the word as a literal, so both execution engines share one definition and the
// compiler folds the field extraction away in generated code.
//
// Edge-case behaviour (division by zero, float->int saturation, NaN results) follows PPSSPP's
// interpreter, which records the hardware-verified results from pspautotests.
#pragma once

#include <bit>
#include <cmath>
#include <xmmintrin.h>

#include "allegrex/ctx.h"
#include "allegrex/guestmem.h"
#include "allegrex/insn.h"

// Shared CPU state that lives in the runtime executable. AOT DLLs receive these pointers at
// registration so that every binary sees the same storage.
extern u32 *g_cop0;            // 32 dummy COP0 registers
extern u32 *g_intrEnabled;     // interrupt enable flag (mfic/mtic, sceKernelCpuSuspendIntr)

namespace sem {

constexpr u32 FCR0_VALUE = 0x00003351;

PSPC_FORCEINLINE void SetR(Ctx *c, u32 reg, u32 v) {
	if (reg != 0) c->r[reg] = v;
}

PSPC_FORCEINLINE s32 SatF2I(double d) {
	if (d >= 2147483648.0) return 0x7FFFFFFF;
	if (d <= -2147483648.0) return (s32)0x80000000;
	if (d != d) return 0x7FFFFFFF;
	return (s32)d;
}

// Round half to even, independent of the host rounding mode.
PSPC_FORCEINLINE double RoundIeee754(double d) {
	double i = std::floor(d);
	double frac = d - i;
	if (frac < 0.5) return i;
	if (frac > 0.5) return i + 1.0;
	return (std::fmod(i, 2.0) == 0.0) ? i : i + 1.0;
}

PSPC_FORCEINLINE u32 F2U(float f) { return std::bit_cast<u32>(f); }

// The FPU's rounding mode (fcr31 bits 0-1) and flush-to-zero (bit 24) map onto MXCSR. Each guest
// thread is a fiber created with FIBER_FLAG_FLOAT_SWITCH, so the setting follows the thread.
PSPC_FORCEINLINE void ApplyFpuMode(u32 fcr31) {
	static const unsigned rc[4] = {_MM_ROUND_NEAREST, _MM_ROUND_TOWARD_ZERO, _MM_ROUND_UP, _MM_ROUND_DOWN};
	unsigned csr = _mm_getcsr() & ~(unsigned)(_MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | 0x40);
	csr |= rc[fcr31 & 3];
	if (fcr31 & (1u << 24)) csr |= _MM_FLUSH_ZERO_ON | 0x40;
	_mm_setcsr(csr);
}

// FPU arithmetic produces the positive default NaN for every NaN result (cpu/fpu/fpu_nan).
PSPC_FORCEINLINE float CanonNan(float r) { return r != r ? std::bit_cast<float>(0x7FC00000u) : r; }
PSPC_FORCEINLINE float U2F(u32 u) { return std::bit_cast<float>(u); }

PSPC_FORCEINLINE u32 ReverseBits32(u32 v) {
	v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
	v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
	v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
	v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
	return (v >> 16) | (v << 16);
}

PSPC_FORCEINLINE u32 Swap32(u32 v) {
	return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

}  // namespace sem

#define SEM(id) PSPC_FORCEINLINE void sem_##id(Ctx *PSPC_RESTRICT c, u32 w)
#define COND(id) PSPC_FORCEINLINE bool cond_##id(Ctx *PSPC_RESTRICT c, u32 w)

#define RS_ insn::Rs(w)
#define RT_ insn::Rt(w)
#define RD_ insn::Rd(w)
#define SA_ insn::Sa(w)
#define VRS c->r[insn::Rs(w)]
#define VRT c->r[insn::Rt(w)]
#define ADDR_ (VRS + (u32)insn::Simm16(w))

// ---- SPECIAL ----
SEM(SLL) { sem::SetR(c, RD_, VRT << SA_); }
SEM(SRL) { sem::SetR(c, RD_, VRT >> SA_); }
SEM(ROTR) { sem::SetR(c, RD_, std::rotr(VRT, (int)SA_)); }
SEM(SRA) { sem::SetR(c, RD_, (u32)((s32)VRT >> SA_)); }
SEM(SLLV) { sem::SetR(c, RD_, VRT << (VRS & 31)); }
SEM(SRLV) { sem::SetR(c, RD_, VRT >> (VRS & 31)); }
SEM(ROTRV) { sem::SetR(c, RD_, std::rotr(VRT, (int)(VRS & 31))); }
SEM(SRAV) { sem::SetR(c, RD_, (u32)((s32)VRT >> (VRS & 31))); }
SEM(MOVZ) {
	if (VRT == 0) sem::SetR(c, RD_, VRS);
}
SEM(MOVN) {
	if (VRT != 0) sem::SetR(c, RD_, VRS);
}
SEM(SYNC) {}
SEM(MFHI) { sem::SetR(c, RD_, c->hi); }
SEM(MTHI) { c->hi = VRS; }
SEM(MFLO) { sem::SetR(c, RD_, c->lo); }
SEM(MTLO) { c->lo = VRS; }
SEM(CLZ) { sem::SetR(c, RD_, (u32)std::countl_zero(VRS)); }
SEM(CLO) { sem::SetR(c, RD_, (u32)std::countl_zero(~VRS)); }
SEM(MULT) {
	s64 r = (s64)(s32)VRS * (s64)(s32)VRT;
	c->lo = (u32)r;
	c->hi = (u32)((u64)r >> 32);
}
SEM(MULTU) {
	u64 r = (u64)VRS * (u64)VRT;
	c->lo = (u32)r;
	c->hi = (u32)(r >> 32);
}
SEM(DIV) {
	s32 a = (s32)VRS, b = (s32)VRT;
	if (a == (s32)0x80000000 && b == -1) {
		c->lo = 0x80000000u;
		c->hi = 0;
	} else if (b != 0) {
		c->lo = (u32)(a / b);
		c->hi = (u32)(a % b);
	} else {
		c->lo = a < 0 ? 1u : 0xFFFFFFFFu;
		c->hi = (u32)a;
	}
}
SEM(DIVU) {
	u32 a = VRS, b = VRT;
	if (b != 0) {
		c->lo = a / b;
		c->hi = a % b;
	} else {
		c->lo = a <= 0xFFFF ? 0xFFFFu : 0xFFFFFFFFu;
		c->hi = a;
	}
}
SEM(MADD) {
	s64 acc = (s64)(((u64)c->hi << 32) | c->lo);
	acc += (s64)(s32)VRS * (s64)(s32)VRT;
	c->lo = (u32)acc;
	c->hi = (u32)((u64)acc >> 32);
}
SEM(MADDU) {
	u64 acc = ((u64)c->hi << 32) | c->lo;
	acc += (u64)VRS * (u64)VRT;
	c->lo = (u32)acc;
	c->hi = (u32)(acc >> 32);
}
SEM(MSUB) {
	s64 acc = (s64)(((u64)c->hi << 32) | c->lo);
	acc -= (s64)(s32)VRS * (s64)(s32)VRT;
	c->lo = (u32)acc;
	c->hi = (u32)((u64)acc >> 32);
}
SEM(MSUBU) {
	u64 acc = ((u64)c->hi << 32) | c->lo;
	acc -= (u64)VRS * (u64)VRT;
	c->lo = (u32)acc;
	c->hi = (u32)(acc >> 32);
}
// Overflow traps of add/sub/addi are not modelled (games never rely on them).
SEM(ADD) { sem::SetR(c, RD_, VRS + VRT); }
SEM(ADDU) { sem::SetR(c, RD_, VRS + VRT); }
SEM(SUB) { sem::SetR(c, RD_, VRS - VRT); }
SEM(SUBU) { sem::SetR(c, RD_, VRS - VRT); }
SEM(AND) { sem::SetR(c, RD_, VRS & VRT); }
SEM(OR) { sem::SetR(c, RD_, VRS | VRT); }
SEM(XOR) { sem::SetR(c, RD_, VRS ^ VRT); }
SEM(NOR) { sem::SetR(c, RD_, ~(VRS | VRT)); }
SEM(SLT) { sem::SetR(c, RD_, (s32)VRS < (s32)VRT ? 1u : 0u); }
SEM(SLTU) { sem::SetR(c, RD_, VRS < VRT ? 1u : 0u); }
SEM(MAX) { sem::SetR(c, RD_, (s32)VRS > (s32)VRT ? VRS : VRT); }
SEM(MIN) { sem::SetR(c, RD_, (s32)VRS < (s32)VRT ? VRS : VRT); }

// ---- Immediate ----
SEM(ADDI) { sem::SetR(c, RT_, VRS + (u32)insn::Simm16(w)); }
SEM(ADDIU) { sem::SetR(c, RT_, VRS + (u32)insn::Simm16(w)); }
SEM(SLTI) { sem::SetR(c, RT_, (s32)VRS < insn::Simm16(w) ? 1u : 0u); }
SEM(SLTIU) { sem::SetR(c, RT_, VRS < (u32)insn::Simm16(w) ? 1u : 0u); }
SEM(ANDI) { sem::SetR(c, RT_, VRS & insn::Imm16(w)); }
SEM(ORI) { sem::SetR(c, RT_, VRS | insn::Imm16(w)); }
SEM(XORI) { sem::SetR(c, RT_, VRS ^ insn::Imm16(w)); }
SEM(LUI) { sem::SetR(c, RT_, insn::Imm16(w) << 16); }

// ---- Branch conditions ----
COND(BEQ) { return VRS == VRT; }
COND(BNE) { return VRS != VRT; }
COND(BLEZ) { return (s32)VRS <= 0; }
COND(BGTZ) { return (s32)VRS > 0; }
COND(BEQL) { return VRS == VRT; }
COND(BNEL) { return VRS != VRT; }
COND(BLEZL) { return (s32)VRS <= 0; }
COND(BGTZL) { return (s32)VRS > 0; }
COND(BLTZ) { return (s32)VRS < 0; }
COND(BGEZ) { return (s32)VRS >= 0; }
COND(BLTZL) { return (s32)VRS < 0; }
COND(BGEZL) { return (s32)VRS >= 0; }
COND(BLTZAL) { return (s32)VRS < 0; }
COND(BGEZAL) { return (s32)VRS >= 0; }
COND(BLTZALL) { return (s32)VRS < 0; }
COND(BGEZALL) { return (s32)VRS >= 0; }
COND(BC1F) { return c->fpcond == 0; }
COND(BC1T) { return c->fpcond != 0; }
COND(BC1FL) { return c->fpcond == 0; }
COND(BC1TL) { return c->fpcond != 0; }

// ---- COP0 (dummy) ----
SEM(MFC0) { sem::SetR(c, RT_, g_cop0[RD_]); }
SEM(MTC0) { g_cop0[RD_] = VRT; }
SEM(COP0_NOP) {}

// ---- COP1 ----
SEM(MFC1) { sem::SetR(c, RT_, c->fi[insn::Fs(w)]); }
SEM(MTC1) { c->fi[insn::Fs(w)] = VRT; }
SEM(CFC1) {
	u32 fs = insn::Fs(w);
	u32 v = 0;
	if (fs == 31) {
		c->fcr31 = (c->fcr31 & ~(1u << 23)) | ((c->fpcond & 1) << 23);
		v = c->fcr31;
	} else if (fs == 0) {
		v = sem::FCR0_VALUE;
	}
	sem::SetR(c, RT_, v);
}
SEM(CTC1) {
	if (insn::Fs(w) == 31) {
		u32 v = VRT;
		c->fcr31 = v & 0x0181FFFF;
		c->fpcond = (v >> 23) & 1;
		sem::ApplyFpuMode(c->fcr31);
	}
}

#define FS_ c->f[insn::Fs(w)]
#define FT_ c->f[insn::Ft(w)]
#define FD_ c->f[insn::Fd(w)]
#define FDI_ c->fi[insn::Fd(w)]

SEM(ADD_S) { FD_ = sem::CanonNan(FS_ + FT_); }
SEM(SUB_S) { FD_ = sem::CanonNan(FS_ - FT_); }
SEM(MUL_S) {
	float a = FS_, b = FT_;
	if ((std::isinf(a) && b == 0.0f) || (std::isinf(b) && a == 0.0f)) {
		FDI_ = 0x7FC00000u;
	} else {
		FD_ = sem::CanonNan(a * b);
	}
}
SEM(DIV_S) { FD_ = sem::CanonNan(FS_ / FT_); }
SEM(SQRT_S) {
	float a = FS_;
	if (a < 0.0f) {
		FDI_ = 0x7FC00000u;
	} else {
		FD_ = std::sqrt(a);
	}
}
SEM(ABS_S) { FD_ = std::fabs(FS_); }
SEM(MOV_S) { c->fi[insn::Fd(w)] = c->fi[insn::Fs(w)]; }
SEM(NEG_S) { FD_ = -FS_; }
SEM(ROUND_W_S) { FDI_ = (u32)sem::SatF2I(sem::RoundIeee754(FS_)); }
SEM(TRUNC_W_S) { FDI_ = (u32)sem::SatF2I(std::trunc((double)FS_)); }
SEM(CEIL_W_S) { FDI_ = (u32)sem::SatF2I(std::ceil((double)FS_)); }
SEM(FLOOR_W_S) { FDI_ = (u32)sem::SatF2I(std::floor((double)FS_)); }
SEM(CVT_S_W) { FD_ = (float)(s32)c->fi[insn::Fs(w)]; }
SEM(CVT_W_S) {
	double v = FS_;
	switch (c->fcr31 & 3) {
	case 0: FDI_ = (u32)sem::SatF2I(sem::RoundIeee754(v)); break;
	case 1: FDI_ = (u32)sem::SatF2I(std::trunc(v)); break;
	case 2: FDI_ = (u32)sem::SatF2I(std::ceil(v)); break;
	default: FDI_ = (u32)sem::SatF2I(std::floor(v)); break;
	}
}
SEM(C_COND_S) {
	float a = FS_, b = FT_;
	bool un = std::isnan(a) || std::isnan(b);
	bool cond;
	switch (w & 7) {
	case 0: cond = false; break;                  // f / sf
	case 1: cond = un; break;                     // un / ngle
	case 2: cond = !un && a == b; break;          // eq / seq
	case 3: cond = a == b || un; break;           // ueq / ngl
	case 4: cond = a < b; break;                  // olt / lt
	case 5: cond = a < b || un; break;            // ult / nge
	case 6: cond = a <= b; break;                 // ole / le
	default: cond = a <= b || un; break;          // ule / ngt
	}
	c->fpcond = cond ? 1 : 0;
}

#undef FS_
#undef FT_
#undef FD_
#undef FDI_

// ---- SPECIAL2 ----
SEM(HALT) {}
SEM(MFIC) { sem::SetR(c, RT_, *g_intrEnabled ? 1u : 0u); }
SEM(MTIC) { *g_intrEnabled = VRT & 1; }

// ---- SPECIAL3 ----
SEM(EXT) {
	u32 size = insn::Size(w) + 1;
	u32 mask = 0xFFFFFFFFu >> (32 - size);
	sem::SetR(c, RT_, (VRS >> insn::Pos(w)) & mask);
}
SEM(INS) {
	u32 mask = 0xFFFFFFFFu >> (31 - insn::Size(w));
	u32 pos = insn::Pos(w);
	u32 srcmask = mask >> pos;
	u32 dstmask = srcmask << pos;
	sem::SetR(c, RT_, (VRT & ~dstmask) | ((VRS & srcmask) << pos));
}
SEM(WSBH) {
	u32 v = VRT;
	sem::SetR(c, RD_, ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8));
}
SEM(WSBW) { sem::SetR(c, RD_, sem::Swap32(VRT)); }
SEM(SEB) { sem::SetR(c, RD_, (u32)(s32)(s8)(u8)VRT); }
SEM(SEH) { sem::SetR(c, RD_, (u32)(s32)(s16)(u16)VRT); }
SEM(BITREV) { sem::SetR(c, RD_, sem::ReverseBits32(VRT)); }

// ---- Loads / stores ----
SEM(LB) { sem::SetR(c, RT_, (u32)(s32)(s8)GRead8(ADDR_)); }
SEM(LH) { sem::SetR(c, RT_, (u32)(s32)(s16)GRead16(ADDR_)); }
SEM(LW) { sem::SetR(c, RT_, GRead32(ADDR_)); }
SEM(LBU) { sem::SetR(c, RT_, GRead8(ADDR_)); }
SEM(LHU) { sem::SetR(c, RT_, GRead16(ADDR_)); }
SEM(LWL) {
	u32 addr = ADDR_;
	u32 mem = GRead32(addr & ~3u);
	u32 shift = (addr & 3) * 8;
	sem::SetR(c, RT_, (VRT & (0x00FFFFFFu >> shift)) | (mem << (24 - shift)));
}
SEM(LWR) {
	u32 addr = ADDR_;
	u32 mem = GRead32(addr & ~3u);
	u32 shift = (addr & 3) * 8;
	sem::SetR(c, RT_, (VRT & (0xFFFFFF00u << (24 - shift))) | (mem >> shift));
}
SEM(SB) { GWrite8(ADDR_, (u8)VRT); }
SEM(SH) { GWrite16(ADDR_, (u16)VRT); }
SEM(SW) { GWrite32(ADDR_, VRT); }
SEM(SWL) {
	u32 addr = ADDR_;
	u32 shift = (addr & 3) * 8;
	u32 mem = GRead32(addr & ~3u);
	GWrite32(addr & ~3u, (VRT >> (24 - shift)) | (mem & (0xFFFFFF00u << shift)));
}
SEM(SWR) {
	u32 addr = ADDR_;
	u32 shift = (addr & 3) * 8;
	u32 mem = GRead32(addr & ~3u);
	GWrite32(addr & ~3u, (VRT << shift) | (mem & (0x00FFFFFFu >> (24 - shift))));
}
SEM(CACHE) {}
SEM(LL) {
	sem::SetR(c, RT_, GRead32(ADDR_));
	c->llbit = 1;
}
SEM(SC) {
	if (c->llbit) {
		GWrite32(ADDR_, VRT);
		sem::SetR(c, RT_, 1);
	} else {
		sem::SetR(c, RT_, 0);
	}
}
SEM(LWC1) { c->fi[insn::Ft(w)] = GRead32(ADDR_); }
SEM(SWC1) { GWrite32(ADDR_, c->fi[insn::Ft(w)]); }

#undef ADDR_

// VFPU semantics (sem_V*/cond_BV*) live in vfpu_sem.h.
#include "allegrex/vfpu_sem.h"
