// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// VFPU register layout, instruction fields and shared constants.
//
// A VFPU register number is 7 bits: 0RRMMMCC. MMM is the matrix (0-7), CC the column and RR the
// row. Ctx::v[] stores them in PPSSPP's order, v[mtx * 16 + col * 4 + row], so every column
// vector (C...) and every 4x4 matrix is contiguous. Everything here is constexpr so recompiled
// code, which passes the instruction word as a literal, folds the index math away.
#pragma once

#include <bit>

#include "allegrex/ctx.h"

namespace vfpu {

// ---- Instruction fields ----
constexpr u32 Vd(u32 w) { return w & 0x7F; }
constexpr u32 Vs(u32 w) { return (w >> 8) & 0x7F; }
constexpr u32 Vt(u32 w) { return (w >> 16) & 0x7F; }
// Vector length 1-4 (.s .p .t .q) from bits 7 and 15.
constexpr int VecSize(u32 w) { return (int)(((w >> 7) & 1) | ((w >> 14) & 2)) + 1; }
// lv.s/sv.s register: rt field plus bits 0-1 as the row.
constexpr u32 LsRegS(u32 w) { return ((w >> 16) & 0x1F) | ((w & 3) << 5); }
// lv.q/sv.q/lvl/lvr/svl/svr register: rt field plus bit 0 as the transpose bit.
constexpr u32 LsRegQ(u32 w) { return ((w >> 16) & 0x1F) | ((w & 1) << 5); }
// Load/store offset (the low two bits belong to the register / flags).
constexpr s32 LsOffset(u32 w) { return (s32)(s16)(u16)(w & 0xFFFC); }

// ---- Register layout ----
// Index into Ctx::v of a single register.
constexpr u32 VOffset(u32 reg) { return ((reg & 0x1F) << 2) | ((reg >> 5) & 3); }
// Register number of Ctx::v[idx].
constexpr u32 FromVOffset(u32 idx) { return ((idx >> 2) & 0x1F) | ((idx & 3) << 5); }

// First row of an n-element vector or n x n matrix register.
constexpr u32 StartRow(u32 reg, int n) {
	return n == 1 ? (reg >> 5) & 3 : n == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
}
constexpr bool Transposed(u32 reg, int n) { return n != 1 && ((reg >> 5) & 1) != 0; }

// Ctx::v index of element i of the n-element vector register reg (n = 1..4).
constexpr u32 VecElem(u32 reg, int n, int i) {
	const u32 mtx = (reg << 2) & 0x70;
	const u32 col = reg & 3;
	const u32 row = (StartRow(reg, n) + (u32)i) & 3;
	if (n == 1) return VOffset(reg);
	return Transposed(reg, n) ? mtx + col + row * 4 : mtx + col * 4 + row;
}

// Ctx::v index of element (j, i) of the n x n matrix register reg, where j selects the vector
// (column for M, row for E) and i the element inside it. This is PPSSPP's rd[j * 4 + i] order.
constexpr u32 MtxElem(u32 reg, int n, int j, int i) {
	const u32 mtx = (reg << 2) & 0x70;
	const u32 col = reg & 3;
	const u32 row = StartRow(reg, n);
	const u32 a = (col + (u32)j) & 3, b = (row + (u32)i) & 3;
	return Transposed(reg, n) ? mtx + b * 4 + a : mtx + a * 4 + b;
}

// ---- Prefixes ----
constexpr u32 PFX_DEFAULT_ST = 0xE4;  // x, y, z, w with no modifiers
constexpr u32 PFX_DEFAULT_D = 0;

constexpr u32 Swizzle(int x, int y, int z, int w) { return (u32)(x | (y << 2) | (z << 4) | (w << 6)); }
constexpr u32 LaneMask(int x, int y, int z, int w) { return (u32)(x | (y << 1) | (z << 2) | (w << 3)); }
constexpr u32 AnySwizzle() { return 0xFF; }
constexpr u32 PfxAbs(int x, int y, int z, int w) { return LaneMask(x, y, z, w) << 8; }
constexpr u32 PfxConst(int x, int y, int z, int w) { return LaneMask(x, y, z, w) << 12; }
constexpr u32 PfxNeg(int x, int y, int z, int w) { return LaneMask(x, y, z, w) << 16; }

// Constant selectors for the S/T prefix constant mode (swizzle bits + abs bit pick one of 8).
enum Const : int {
	C_NONE = -1,
	C_ZERO,
	C_ONE,
	C_TWO,
	C_HALF,
	C_THREE,
	C_THIRD,
	C_FOURTH,
	C_SIXTH,
};

constexpr u32 MakeConstants(int x, int y, int z, int w) {
	u32 r = 0;
	if (x != C_NONE) r |= ((u32)(x & 3) << 0) | ((u32)(x & 4) << 6) | (1u << 12);
	if (y != C_NONE) r |= ((u32)(y & 3) << 2) | ((u32)(y & 4) << 7) | (1u << 13);
	if (z != C_NONE) r |= ((u32)(z & 3) << 4) | ((u32)(z & 4) << 8) | (1u << 14);
	if (w != C_NONE) r |= ((u32)(w & 3) << 6) | ((u32)(w & 4) << 9) | (1u << 15);
	return r;
}

// Values selected by the prefix constant mode, as float bits:
// 0, 1, 2, 1/2, 3, 1/3, 1/4, 1/6.
inline constexpr u32 kPrefixConstBits[8] = {
	0x00000000u, 0x3F800000u, 0x40000000u, 0x3F000000u,
	0x40400000u, 0x3EAAAAABu, 0x3E800000u, 0x3E2AAAABu,
};

// vcst constants (float bits, the values PPSSPP verified on hardware). Index = bits 16-20.
inline constexpr u32 kCstBits[32] = {
	0x00000000u,  // (undefined)
	0x7F7FFFFFu,  // VFPU_HUGE (FLT_MAX)
	0x3FB504F3u,  // VFPU_SQRT2
	0x3F3504F3u,  // VFPU_SQRT1_2
	0x3F906EBAu,  // VFPU_2_SQRTPI
	0x3F22F983u,  // VFPU_2_PI
	0x3EA2F983u,  // VFPU_1_PI
	0x3F490FDBu,  // VFPU_PI_4
	0x3FC90FDBu,  // VFPU_PI_2
	0x40490FDBu,  // VFPU_PI
	0x402DF854u,  // VFPU_E
	0x3FB8AA3Bu,  // VFPU_LOG2E
	0x3EDE5BD9u,  // VFPU_LOG10E
	0x3F317218u,  // VFPU_LN2
	0x40135D8Eu,  // VFPU_LN10
	0x40C90FDBu,  // VFPU_2PI
	0x3F060A92u,  // VFPU_PI_6
	0x3E9A209Bu,  // VFPU_LOG10TWO
	0x40549A78u,  // VFPU_LOG2TEN
	0x3F5DB3D7u,  // VFPU_SQRT3_2
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Half float to float as viim/vfim decode it (PPSSPP Float16ToFloat32: denormals are expanded,
// inf/NaN keep their mantissa bits unshifted).
constexpr u32 Float16ToFloat32Bits(u16 h) {
	const u32 sign = (u32)(h >> 15) & 1;
	int exponent = (h >> 10) & 0x1F;
	u32 fraction = h & 0x3FF;
	if (exponent == 0x1F) return (sign << 31) | (255u << 23) | fraction;
	if (exponent == 0 && fraction == 0) return sign << 31;
	if (exponent == 0) {
		do {
			fraction <<= 1;
			exponent--;
		} while (!(fraction & 0x400));
		fraction &= 0x3FF;
	}
	return (sign << 31) | ((u32)(exponent + 112) << 23) | (fraction << 13);
}

// ---- Control registers ----
// Writable bits of a control register through mtvc/vmtvc. Returns false for read-only ones.
constexpr bool CtrlWriteMask(u32 reg, u32 *mask) {
	switch (reg) {
	case VFPU_CTRL_SPREFIX:
	case VFPU_CTRL_TPREFIX: *mask = 0x000FFFFF; return true;
	case VFPU_CTRL_DPREFIX: *mask = 0x00000FFF; return true;
	case VFPU_CTRL_CC: *mask = 0x0000003F; return true;
	case VFPU_CTRL_INF4: *mask = 0xFFFFFFFF; return true;
	case VFPU_CTRL_RSV5:
	case VFPU_CTRL_RSV6:
	case VFPU_CTRL_REV: return false;
	default:
		if (reg >= VFPU_CTRL_RCX0 && reg < VFPU_CTRL_RCX0 + 8) {
			*mask = 0x000FFFFF;
			return true;
		}
		return false;
	}
}
// Bits a write always sets: the RNG state registers keep 0x3F800000 (cpu/vfpu/vrnd).
constexpr u32 CtrlSetBits(u32 reg) {
	return reg >= VFPU_CTRL_RCX0 && reg < VFPU_CTRL_RCX0 + 8 ? 0x3F800000u : 0u;
}
// mtvc/vmtvc semantics.
PSPC_FORCEINLINE void WriteCtrl(Ctx *c, u32 reg, u32 value) {
	u32 mask = 0;
	if (reg < 16 && CtrlWriteMask(reg, &mask)) c->vfpuCtrl[reg] = (value & mask) | CtrlSetBits(reg);
}

// Power-on VFPU state (PPSSPP MIPSState::Init). The runtime must apply this to every new
// thread context: a zeroed S/T prefix means "swizzle everything from x".
inline void InitCtx(Ctx *c) {
	for (int i = 0; i < 128; i++) c->vi[i] = 0;
	for (int i = 0; i < 16; i++) c->vfpuCtrl[i] = 0;
	c->vfpuCtrl[VFPU_CTRL_SPREFIX] = PFX_DEFAULT_ST;
	c->vfpuCtrl[VFPU_CTRL_TPREFIX] = PFX_DEFAULT_ST;
	c->vfpuCtrl[VFPU_CTRL_DPREFIX] = PFX_DEFAULT_D;
	c->vfpuCtrl[VFPU_CTRL_CC] = 0x3F;
	c->vfpuCtrl[VFPU_CTRL_INF4] = 0;
	c->vfpuCtrl[VFPU_CTRL_REV] = 0x7772CEAB;
	c->vfpuCtrl[VFPU_CTRL_RCX0 + 0] = 0x3F800001;
	c->vfpuCtrl[VFPU_CTRL_RCX0 + 1] = 0x3F800002;
	c->vfpuCtrl[VFPU_CTRL_RCX0 + 2] = 0x3F800004;
	c->vfpuCtrl[VFPU_CTRL_RCX0 + 3] = 0x3F800008;
	for (int i = 4; i < 8; i++) c->vfpuCtrl[VFPU_CTRL_RCX0 + i] = 0x3F800000;
}

PSPC_FORCEINLINE bool HasDefaultPrefixes(const Ctx *c) {
	return c->vfpuCtrl[VFPU_CTRL_SPREFIX] == PFX_DEFAULT_ST && c->vfpuCtrl[VFPU_CTRL_TPREFIX] == PFX_DEFAULT_ST &&
	       c->vfpuCtrl[VFPU_CTRL_DPREFIX] == PFX_DEFAULT_D;
}

PSPC_FORCEINLINE void EatPrefixes(Ctx *c) {
	c->vfpuCtrl[VFPU_CTRL_SPREFIX] = PFX_DEFAULT_ST;
	c->vfpuCtrl[VFPU_CTRL_TPREFIX] = PFX_DEFAULT_ST;
	c->vfpuCtrl[VFPU_CTRL_DPREFIX] = PFX_DEFAULT_D;
}

}  // namespace vfpu
