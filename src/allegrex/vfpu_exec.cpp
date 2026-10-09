// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// VFPU instruction families with full prefix handling, ported from Core/MIPS/InterpreterVFPU.cpp.
// The quirks (which prefix lanes an op honours, forced swizzles, last-lane-only D prefixes and so
// on) are PPSSPP's, which records them from hardware tests (pspautotests tests/cpu/vfpu/*).
#include <algorithm>
#include <bit>
#include <cmath>

#include "allegrex/guestmem.h"
#include "allegrex/vfpu_exec.h"

// The interpreter and recompiled code must round identically: no fused multiply-add.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace vfpu {

// PPSSPP computes dot products (vdot, vmmul, vtfm...) as plain float sums unless a game asks for
// the bit-exact datapath. We follow its default.
constexpr bool kAccurateDot = false;

union Vec4 {
	float f[4];
	u32 u[4];
	s32 i[4];
};

static inline float F(u32 u) { return std::bit_cast<float>(u); }
static inline u32 U(float f) { return std::bit_cast<u32>(f); }
static inline bool IsNaN(float f) { return (U(f) & 0x7F800000) == 0x7F800000 && (U(f) & 0x7FFFFF); }
static inline bool IsInf(float f) { return (U(f) & 0x7FFFFFFF) == 0x7F800000; }
static inline bool IsNaNOrInf(float f) { return (U(f) & 0x7F800000) == 0x7F800000; }
static inline float FAbs(float f) { return F(U(f) & 0x7FFFFFFF); }
static inline float FNeg(float f) { return F(U(f) ^ 0x80000000); }

static inline u32 &Ctl(Ctx *c, int r) { return c->vfpuCtrl[r]; }
static inline bool WriteMasked(const Ctx *c, int i) { return (c->vfpuCtrl[VFPU_CTRL_DPREFIX] >> (8 + i)) & 1; }
static inline u32 Rewrite(const Ctx *c, int ctrl, u32 remove, u32 add) { return (c->vfpuCtrl[ctrl] & ~remove) | add; }

// ---- Register file access ----

static void ReadVector(const Ctx *c, float *rd, int n, u32 reg) {
	for (int i = 0; i < n; i++) rd[i] = c->v[VecElem(reg, n, i)];
}

static void WriteVector(Ctx *c, const float *rd, int n, u32 reg) {
	for (int i = 0; i < n; i++) {
		if (!WriteMasked(c, i)) c->v[VecElem(reg, n, i)] = rd[i];
	}
}

static void ReadMatrix(const Ctx *c, float *rd, int n, u32 reg) {
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) rd[j * 4 + i] = c->v[MtxElem(reg, n, j, i)];
	}
}

// The write mask applies to the last vector only.
static void WriteMatrix(Ctx *c, const float *rd, int n, u32 reg) {
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			if (j != n - 1 || !WriteMasked(c, i)) c->v[MtxElem(reg, n, j, i)] = rd[j * 4 + i];
		}
	}
}

// Register number of element i of a vector register, as a single (PPSSPP GetVectorRegs).
static u32 ElemRegNum(u32 reg, int n, int i) { return FromVOffset(VecElem(reg, n, i)); }

// ---- Prefixes ----

static void ApplyPrefixST(float *r, u32 data, int n, float invalid = 0.0f) {
	if (data == PFX_DEFAULT_ST) return;
	float orig[4] = {invalid, invalid, invalid, invalid};
	for (int i = 0; i < n; i++) orig[i] = r[i];
	for (int i = 0; i < n; i++) {
		const int regnum = (data >> (i * 2)) & 3;
		const int abs = (data >> (8 + i)) & 1;
		const int negate = (data >> (16 + i)) & 1;
		const int constants = (data >> (12 + i)) & 1;
		if (!constants) {
			r[i] = orig[regnum];
			if (abs) r[i] = FAbs(r[i]);
		} else {
			r[i] = F(kPrefixConstBits[regnum + (abs << 2)]);
		}
		if (negate) r[i] = FNeg(r[i]);
	}
}

static void ApplySwizzleS(const Ctx *c, float *v, int n, float invalid = 0.0f) {
	ApplyPrefixST(v, c->vfpuCtrl[VFPU_CTRL_SPREFIX], n, invalid);
}

static void ApplySwizzleT(const Ctx *c, float *v, int n, float invalid = 0.0f) {
	ApplyPrefixST(v, c->vfpuCtrl[VFPU_CTRL_TPREFIX], n, invalid);
}

// Saturation as PPSSPP's vfpu_clamp(v, min, max) = v >= max ? max : (v <= min ? min : v): NaN is
// preserved and -0.0 becomes +0.0 for [0, 1]. Written on the bits, because compilers turn the
// ternary into maxss/minss, which keep -0.0 (MSVC does, even with /fp:precise).
static inline float Clamp01(float v) {
	const u32 u = U(v);
	if ((u & 0x7FFFFFFF) > 0x7F800000) return v;  // NaN
	if (u & 0x80000000) return 0.0f;               // negative or -0
	return u >= 0x3F800000 ? 1.0f : v;
}
static inline float ClampPM1(float v) {
	const u32 u = U(v);
	if ((u & 0x7FFFFFFF) > 0x7F800000) return v;  // NaN
	if ((u & 0x7FFFFFFF) < 0x3F800000) return v;   // |v| < 1, including -0
	return (u & 0x80000000) ? -1.0f : 1.0f;
}

static void ApplyPrefixD(const Ctx *c, float *v, int n, bool onlyWriteMask = false) {
	const u32 data = c->vfpuCtrl[VFPU_CTRL_DPREFIX];
	if (!data || onlyWriteMask) return;
	for (int i = 0; i < n; i++) {
		const int sat = (data >> (i * 2)) & 3;
		if (sat == 1) v[i] = Clamp01(v[i]);
		else if (sat == 3) v[i] = ClampPM1(v[i]);
	}
}

// Ops that apply the D prefix to their last lane only take that lane's mask and saturation from
// position 0.
static void MoveDPrefixToLastLane(Ctx *c, int n) {
	const u32 d = c->vfpuCtrl[VFPU_CTRL_DPREFIX];
	const u32 lastmask = (d & (1 << 8)) << (n - 1);
	const u32 lastsat = (d & 3) << (n + n - 2);
	c->vfpuCtrl[VFPU_CTRL_DPREFIX] = lastmask | lastsat;
}

// The ops that only apply the prefixes to their last lane (vrcp, vsin, vdiv and friends) take that
// lane's prefix from position 0, as if it was a one-lane op. If it names any lane but the first,
// the result lane comes out as zero (cpu/vfpu/prefix_sat).
static bool LastLaneSwizzleInvalid(const Ctx *c, int ctrl) {
	const u32 prefix = c->vfpuCtrl[ctrl];
	return (prefix & 3) != 0 && (prefix & (1 << 12)) == 0;
}

// A lane whose S or T swizzle reads past the vector length gives zero, for the ops that do this.
static void RetainInvalidSwizzleST(const Ctx *c, float *d, int n) {
	const u32 sPrefix = c->vfpuCtrl[VFPU_CTRL_SPREFIX];
	const u32 tPrefix = c->vfpuCtrl[VFPU_CTRL_TPREFIX];
	for (int i = 0; i < n; i++) {
		const int swizzleS = (sPrefix >> (i + i)) & 3;
		const int swizzleT = (tPrefix >> (i + i)) & 3;
		const int constS = (sPrefix >> (12 + i)) & 1;
		const int constT = (tPrefix >> (12 + i)) & 1;
		if ((swizzleS >= n && !constS) || (swizzleT >= n && !constT)) d[i] = 0.0f;
	}
}

// Order used by the min/max family in vsrt: denormals count as zero, inf/NaN sort by sign and
// magnitude (-NaN < -inf < finite < inf < NaN), and a tie returns the second operand.
static inline s32 OrderKey(float f) {
	const u32 u = U(f);
	u32 mag = u & 0x7FFFFFFF;
	if (mag < 0x00800000) mag = 0;
	return (u & 0x80000000) ? -(s32)mag : (s32)mag;
}
static inline float VMin(float a, float b) { return OrderKey(a) < OrderKey(b) ? a : b; }
static inline float VMax(float a, float b) { return OrderKey(a) > OrderKey(b) ? a : b; }

// Exact float order for non-NaN values as an integer (+0 and -0 tie), so selections can't be
// compiled to minss/maxss.
static inline s32 ExactKey(float f) {
	const u32 u = U(f);
	return (u & 0x80000000) ? -(s32)(u & 0x7FFFFFFF) : (s32)(u & 0x7FFFFFFF);
}
// std::min(a, b) / std::max(a, b) for non-NaN values: b < a ? b : a, a < b ? b : a.
static inline float StdMin(float a, float b) { return ExactKey(b) < ExactKey(a) ? b : a; }
static inline float StdMax(float a, float b) { return ExactKey(a) < ExactKey(b) ? b : a; }

// Plain float dot product, in PPSSPP's order.
static inline float SumDot4(const float *s, const float *t) {
	float sum = 0.0f;
	for (int i = 0; i < 4; i++) {
		const float p = s[i] * t[i];
		sum = sum + p;
	}
	return sum;
}

// vfpu_dot plus the NaN / flush fixups PPSSPP applies with the accurate path.
static inline float AccurateDot(const float *s, const float *t) {
	u32 a[4], b[4];
	for (int i = 0; i < 4; i++) {
		a[i] = U(s[i]);
		b[i] = U(t[i]);
	}
	u32 r = DotBits(a, b);
	if ((r & 0x7F800000) == 0x7F800000 && (r & 0x7FFFFF)) r = 0x7F800001;
	else if ((r & 0x7F800000) == 0) r &= 0xFF800000;
	return F(r);
}

// ---- Matrix init: vmidt vmzero vmone ----

void ExecVMatrixInit(Ctx *c, u32 w) {
	static const float idt[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	static const float zero[16] = {};
	static const float one[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
	const u32 vd = Vd(w);
	const int n = VecSize(w);
	const u32 type = (w >> 16) & 0xF;
	const float *m = type == 3 ? idt : type == 6 ? zero : one;

	// The S prefix generates constants, but only for the final (possibly transposed) row.
	if (Ctl(c, VFPU_CTRL_SPREFIX) & 0xF0F00) {
		float prefixed[16];
		memcpy(prefixed, m, sizeof(prefixed));
		const int off = n - 1;
		u32 add = 0;
		if (type == 3) {
			add = MakeConstants(off == 0 ? C_ONE : C_ZERO, off == 1 ? C_ONE : C_ZERO, off == 2 ? C_ONE : C_ZERO,
			                    off == 3 ? C_ONE : C_ZERO);
		} else if (type == 6) {
			add = MakeConstants(C_ZERO, C_ZERO, C_ZERO, C_ZERO);
		} else {
			add = MakeConstants(C_ONE, C_ONE, C_ONE, C_ONE);
		}
		ApplyPrefixST(&prefixed[off * 4], Rewrite(c, VFPU_CTRL_SPREFIX, AnySwizzle(), add), 4);
		WriteMatrix(c, prefixed, n, vd);
	} else {
		// The write mask applies to the final (maybe transposed) row. Saturation hangs the PSP.
		WriteMatrix(c, m, n, vd);
	}
	EatPrefixes(c);
}

// ---- Vector init: vzero vone ----

void ExecVVectorInit(Ctx *c, u32 w) {
	const u32 vd = Vd(w);
	const int n = VecSize(w);
	float d[4] = {};
	const int k = ((w >> 16) & 0xF) == 6 ? C_ZERO : C_ONE;
	// The S prefix generates constants, but negate is still respected.
	ApplyPrefixST(d, Rewrite(c, VFPU_CTRL_SPREFIX, AnySwizzle(), MakeConstants(k, k, k, k)), n);
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, vd);
	EatPrefixes(c);
}

// ---- viim / vfim ----

void ExecViim(Ctx *c, u32 w) {
	const u32 vt = Vt(w);
	const u32 type = (w >> 23) & 7;
	float f[1];
	if (type == 6) f[0] = (float)(s32)(s16)(u16)(w & 0xFFFF);
	else f[0] = F(Float16ToFloat32Bits((u16)(w & 0xFFFF)));
	ApplyPrefixD(c, f, 1);
	WriteVector(c, f, 1, vt);
	EatPrefixes(c);
}

// ---- vidt ----

void ExecVidt(Ctx *c, u32 w) {
	const u32 vd = Vd(w);
	const int n = VecSize(w);
	float f[4] = {};
	// The S prefix generates constants, but negate is still respected. A pair's identity row
	// depends on which column it starts at.
	const u32 offmask = n == 4 || n == 3 ? 3 : 1;
	const u32 off = vd & offmask;
	const int x = off == (0 & offmask) ? C_ONE : C_ZERO;
	const int y = off == (1 & offmask) ? C_ONE : C_ZERO;
	const int z = off == (2 & offmask) ? C_ONE : C_ZERO;
	const int ww = off == (3 & offmask) ? C_ONE : C_ZERO;
	ApplyPrefixST(f, Rewrite(c, VFPU_CTRL_SPREFIX, AnySwizzle(), MakeConstants(x, y, z, ww)), n);
	ApplyPrefixD(c, f, n);
	WriteVector(c, f, n, vd);
	EatPrefixes(c);
}

// ---- vmmul ----

void ExecVmmul(Ctx *c, u32 w) {
	float s[16] = {}, t[16] = {}, d[16] = {};
	const u32 vd = Vd(w), vs = Vs(w), vt = Vt(w);
	const int n = VecSize(w);
	ReadMatrix(c, s, n, vs);
	ReadMatrix(c, t, n, vt);
	for (int a = 0; a < n; a++) {
		for (int b = 0; b < n; b++) {
			const bool last = a == n - 1 && b == n - 1;
			if (last) {
				// The S and T prefixes work on the final dot only.
				ApplySwizzleS(c, &s[b * 4], 4);
				ApplySwizzleT(c, &t[a * 4], 4);
			}
			float sum = 0.0f;
			if (kAccurateDot) {
				sum = AccurateDot(&s[b * 4], &t[a * 4]);
			} else {
				const int len = last ? 4 : n;
				for (int k = 0; k < len; k++) {
					const float p = s[b * 4 + k] * t[a * 4 + k];
					sum = sum + p;
				}
			}
			d[a * 4 + b] = sum;
		}
	}
	// The D prefix applies only to the final element, but sat does work.
	MoveDPrefixToLastLane(c, n);
	ApplyPrefixD(c, &d[4 * (n - 1)], 4);
	WriteMatrix(c, d, n, vd);
	EatPrefixes(c);
}

// ---- vmscl ----

void ExecVmscl(Ctx *c, u32 w) {
	float s[16] = {}, t[4] = {}, d[16] = {};
	const u32 vd = Vd(w), vs = Vs(w), vt = Vt(w);
	const int n = VecSize(w);
	ReadMatrix(c, s, n, vs);
	ReadVector(c, t, 1, vt);
	for (int a = 0; a < n - 1; a++) {
		for (int b = 0; b < n; b++) d[a * 4 + b] = s[a * 4 + b] * t[0];
	}
	// The S prefix applies to the last row.
	ApplySwizzleS(c, &s[(n - 1) * 4], 4);
	// The T prefix applies only for the last row, per element; like vscl but with xxxx.
	const int tlane = (vt >> 5) & 3;
	t[tlane] = t[0];
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), Swizzle(tlane, tlane, tlane, tlane)), 4);
	for (int b = 0; b < n; b++) d[(n - 1) * 4 + b] = s[(n - 1) * 4 + b] * t[b];
	// The D prefix is applied to the last row.
	ApplyPrefixD(c, &d[(n - 1) * 4], 4);
	WriteMatrix(c, d, n, vd);
	EatPrefixes(c);
}

// ---- vmmov ----

void ExecVmmov(Ctx *c, u32 w) {
	float s[16] = {};
	const int n = VecSize(w);
	ReadMatrix(c, s, n, Vs(w));
	// The S and D prefixes are applied to the last row.
	const int off = n - 1;
	ApplySwizzleS(c, &s[off * 4], 4);
	ApplyPrefixD(c, &s[off * 4], 4);
	WriteMatrix(c, s, n, Vd(w));
	EatPrefixes(c);
}

// ---- vmov vabs vneg vsat0 vsat1 and the one-input math ops ----

void ExecVV2Op(Ctx *c, u32 w) {
	float s[4] = {}, d[4] = {};
	const u32 vd = Vd(w), vs = Vs(w);
	const int optype = (w >> 16) & 0x1F;
	const int n = VecSize(w);
	ReadVector(c, s, n, vs);
	// Some of these are prefix hacks.
	switch (optype) {
	case 1: ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, 0, PfxAbs(1, 1, 1, 1)), n); break;
	case 2: ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, 0, PfxNeg(1, 1, 1, 1)), n); break;
	case 16:
	case 17:
	case 18:
	case 19:
	case 20:
	case 21:
	case 22:
	case 23:
		// Like vdiv: the prefix applies to the last lane only. An invalid swizzle zeroes it, below.
		ApplySwizzleS(c, &s[n - 1], 1, INFINITY);
		break;
	case 24:
	case 26:
		// The same, but negate is ignored.
		ApplyPrefixST(&s[n - 1], Rewrite(c, VFPU_CTRL_SPREFIX, PfxNeg(1, 0, 0, 0), 0), 1, -INFINITY);
		break;
	case 28:
		ApplyPrefixST(&s[n - 1], Rewrite(c, VFPU_CTRL_SPREFIX, PfxNeg(1, 0, 0, 0), 0), 1, INFINITY);
		break;
	default: ApplySwizzleS(c, s, n); break;
	}
	for (int i = 0; i < n; i++) {
		switch (optype) {
		case 0:
		case 1:
		case 2: d[i] = s[i]; break;  // vmov, vabs, vneg (the prefix does the work)
		// vsat0 changes -0.0 to +0.0, both keep NaN (the same results as the D saturations).
		case 4: d[i] = Clamp01(s[i]); break;
		case 5: d[i] = ClampPM1(s[i]); break;
		case 16: d[i] = F(Rcp(U(s[i]))); break;
		case 17: d[i] = F(Rsqrt(U(s[i]))); break;
		case 18: d[i] = F(Sin(U(s[i]))); break;
		case 19: d[i] = F(Cos(U(s[i]))); break;
		case 20: d[i] = F(Exp2(U(s[i]))); break;
		case 21: d[i] = F(Log2(U(s[i]))); break;
		case 22: d[i] = F(Sqrt(U(s[i]))); break;
		case 23: d[i] = F(Asin(U(s[i]))); break;
		case 24: d[i] = F(Rcp(U(s[i])) ^ 0x80000000); break;
		case 26: d[i] = F(Sin(U(s[i])) ^ 0x80000000); break;
		case 28: d[i] = F(Rexp2(U(s[i]))); break;
		default: break;
		}
	}
	switch (optype) {
	case 5:
		// vsat1 is a prefix hack, so 0:1 doesn't apply.
		ApplyPrefixD(c, d, n, true);
		break;
	case 16:
	case 17:
	case 18:
	case 19:
	case 20:
	case 21:
	case 22:
	case 23:
	case 24:
	case 26:
	case 28:
		if (LastLaneSwizzleInvalid(c, VFPU_CTRL_SPREFIX)) d[n - 1] = 0.0f;
		// Only the last element gets the mask applied.
		MoveDPrefixToLastLane(c, n);
		ApplyPrefixD(c, d, n);
		break;
	default: ApplyPrefixD(c, d, n); break;
	}
	WriteVector(c, d, n, vd);
	EatPrefixes(c);
}

// ---- vocp vsocp vsgn ----

void ExecVocp(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	// The S prefix forces the negate flags.
	ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, 0, PfxNeg(1, 1, 1, 1)), n);
	// The T prefix forces constants on and regnum to 1, so negate still works and abs picks a
	// different constant.
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), MakeConstants(C_ONE, C_ONE, C_ONE, C_ONE)), n);
	for (int i = 0; i < n; i++) {
		// Always positive NaN. s is always negated from the registers.
		d[i] = IsNaN(s[i]) ? FAbs(s[i]) : t[i] + s[i];
	}
	RetainInvalidSwizzleST(c, d, n);
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

void ExecVsocp(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const int n = VecSize(w);
	const int outN = n == 1 ? 2 : 4;
	ReadVector(c, s, n, Vs(w));
	// The S prefix forces negate in even/odd and an xxyy swizzle. abs works, and applies to the
	// final position.
	ApplyPrefixST(s,
	              Rewrite(c, VFPU_CTRL_SPREFIX, AnySwizzle() | PfxNeg(1, 1, 1, 1), Swizzle(0, 0, 1, 1) | PfxNeg(1, 0, 1, 0)),
	              outN);
	// The T prefix forces constants on and regnum to 1, 0, 1, 0.
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), MakeConstants(C_ONE, C_ZERO, C_ONE, C_ZERO)), outN);
	// D prefix saturation is effectively forced (PPSSPP nanclamp to [0, 1], the same as Clamp01).
	d[0] = Clamp01(t[0] + s[0]);
	d[1] = Clamp01(t[1] + s[1]);
	if (outN == 4) {
		d[2] = Clamp01(t[2] + s[2]);
		d[3] = Clamp01(t[3] + s[3]);
	}
	WriteVector(c, d, outN, Vd(w));
	EatPrefixes(c);
}

void ExecVsgn(Ctx *c, u32 w) {
	Vec4 s = {}, t = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	// Using abs/neg on T allows a compare against 3 or -3.
	ApplyPrefixST(t.f, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), MakeConstants(C_ZERO, C_ZERO, C_ZERO, C_ZERO)), n);
	// A swizzle past the vector length compares a zero.
	for (int i = n; i < 4; i++) s.f[i] = t.f[i];
	ApplySwizzleS(c, s.f, 4);
	for (int i = 0; i < n; i++) {
		// A denormal difference is zero, like everywhere else on the VFPU.
		const u32 val = U(s.f[i] - t.f[i]);
		if ((val & 0x7F800000) == 0) d.f[i] = 0.0f;
		else if ((val >> 31) == 0) d.f[i] = 1.0f;
		else d.f[i] = -1.0f;
	}
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

// ---- Conversions ----

// Rounds *.5 to the closest even number (PPSSPP round_ieee_754).
static inline double RoundIeee754(double d) {
	const float i = (float)std::floor(d);
	d -= i;
	if (d < 0.5f) return i;
	if (d > 0.5f) return i + 1.0f;
	float intPart;
	std::modf(i / 2.0f, &intPart);
	if (2.0f * intPart == i) return i;
	return i + 1.0f;
}

void ExecVf2i(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	const int imm = (w >> 16) & 0x1F;
	const float mult = (float)(1ULL << imm);
	ReadVector(c, s.f, n, Vs(w));
	// Negate, abs and constants apply to the bits as you'd expect.
	ApplySwizzleS(c, s.f, n);
	for (int i = 0; i < n; i++) {
		if (IsNaN(s.f[i])) {
			d.i[i] = 0x7FFFFFFF;
			continue;
		}
		const double sv = s.f[i] * mult;  // (float)0x7fffffff == (float)0x80000000
		if (sv > (double)0x7FFFFFFF) {
			d.i[i] = 0x7FFFFFFF;
		} else if (sv <= (double)(int)0x80000000) {
			d.u[i] = 0x80000000;
		} else {
			switch ((w >> 21) & 0x1F) {
			case 16: d.i[i] = (int)RoundIeee754(sv); break;                                // n
			case 17: d.i[i] = s.f[i] >= 0 ? (int)std::floor(sv) : (int)std::ceil(sv); break;  // z
			case 18: d.i[i] = (int)std::ceil(sv); break;                                    // u
			case 19: d.i[i] = (int)std::floor(sv); break;                                   // d
			default: d.i[i] = 0x7FFFFFFF; break;
			}
		}
	}
	// No saturation, but the mask applies.
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

void ExecVi2f(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	const int imm = (w >> 16) & 0x1F;
	const float mult = 1.0f / (float)(1ULL << imm);
	ReadVector(c, s.f, n, Vs(w));
	// Negate, abs and constants apply to the bits as you'd expect.
	ApplySwizzleS(c, s.f, n);
	for (int i = 0; i < n; i++) d.f[i] = (float)s.i[i] * mult;
	// Sat and mask apply normally.
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

void ExecVh2f(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	int outN;
	if (n == 1) {
		outN = 2;
		d.u[0] = H2F(s.u[0] & 0xFFFF);
		d.u[1] = H2F(s.u[0] >> 16);
	} else {
		// All other sizes are treated as a pair.
		outN = 4;
		d.u[0] = H2F(s.u[0] & 0xFFFF);
		d.u[1] = H2F(s.u[0] >> 16);
		d.u[2] = H2F(s.u[1] & 0xFFFF);
		d.u[3] = H2F(s.u[1] >> 16);
	}
	ApplyPrefixD(c, d.f, outN);
	WriteVector(c, d.f, outN, Vd(w));
	EatPrefixes(c);
}

void ExecVf2h(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	// A swizzle lets a single write both halves.
	ApplySwizzleS(c, s.f, 4);
	// Negate doesn't apply to an invalid swizzle.
	RetainInvalidSwizzleST(c, s.f, 4);
	int outN;
	if (n <= 2) {
		outN = 1;
		d.u[0] = F2H(s.u[0]) | ((u32)F2H(s.u[1]) << 16);
	} else {
		outN = 2;
		d.u[0] = F2H(s.u[0]) | ((u32)F2H(s.u[1]) << 16);
		d.u[1] = F2H(s.u[2]) | ((u32)F2H(s.u[3]) << 16);
	}
	ApplyPrefixD(c, d.f, outN);
	WriteVector(c, d.f, outN, Vd(w));
	EatPrefixes(c);
}

void ExecVx2i(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	int n = VecSize(w);
	int oz = n;
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	switch ((w >> 16) & 3) {
	case 0: {
		// vuc2i: 8-bit unsigned to 31-bit signed, replicating the byte (quad output only).
		u32 value = s.u[0];
		for (int i = 0; i < 4; i++) {
			d.u[i] = (u32)((value & 0xFF) * 0x01010101u) >> 1;
			value >>= 8;
		}
		oz = 4;
		break;
	}
	case 1: {
		// vc2i: signed, so no shift.
		const u32 value = s.u[0];
		d.u[0] = (value & 0xFF) << 24;
		d.u[1] = (value & 0xFF00) << 16;
		d.u[2] = (value & 0xFF0000) << 8;
		d.u[3] = value & 0xFF000000;
		oz = 4;
		break;
	}
	case 2:
	case 3: {
		// vus2i / vs2i. Triples and quads are treated as pairs.
		const bool isSigned = ((w >> 16) & 3) == 3;
		if (n >= 2) {
			n = 2;
			oz = 4;
		} else {
			oz = 2;
		}
		for (int i = 0; i < n; i++) {
			const u32 value = s.u[i];
			if (isSigned) {
				d.u[i * 2] = (value & 0xFFFF) << 16;
				d.u[i * 2 + 1] = value & 0xFFFF0000;
			} else {
				// vus2i skips the replication, so 0xFFFF -> 0x7FFF8000.
				d.u[i * 2] = (value & 0xFFFF) << 15;
				d.u[i * 2 + 1] = (value & 0xFFFF0000) >> 1;
			}
		}
		break;
	}
	}
	// Saturation does apply.
	ApplyPrefixD(c, d.f, oz);
	WriteVector(c, d.f, oz, Vd(w));
	EatPrefixes(c);
}

void ExecVi2x(Ctx *c, u32 w) {
	Vec4 s = {};
	u32 d[2] = {};
	const int n = VecSize(w);
	int oz = 1;
	ReadVector(c, s.f, n, Vs(w));
	// Negate, const etc. apply as expected.
	ApplySwizzleS(c, s.f, 4);
	switch ((w >> 16) & 3) {
	case 0:  // vi2uc
		for (int i = 0; i < 4; i++) {
			s32 v = s.i[i];
			if (v < 0) v = 0;
			v >>= 23;
			d[0] |= ((u32)v & 0xFF) << (i * 8);
		}
		oz = 1;
		break;
	case 1:  // vi2c
		for (int i = 0; i < 4; i++) d[0] |= (s.u[i] >> 24) << (i * 8);
		oz = 1;
		break;
	case 2: {  // vi2us
		const int elems = (n + 1) / 2;
		for (int i = 0; i < elems; i++) {
			s32 low = s.i[i * 2], high = s.i[i * 2 + 1];
			if (low < 0) low = 0;
			if (high < 0) high = 0;
			low >>= 15;
			high >>= 15;
			d[i] = (u32)low | ((u32)high << 16);
		}
		oz = n >= 3 ? 2 : 1;
		break;
	}
	default: {  // vi2s
		const int elems = (n + 1) / 2;
		for (int i = 0; i < elems; i++) {
			const u32 low = s.u[i * 2] >> 16, high = s.u[i * 2 + 1] >> 16;
			d[i] = low | (high << 16);
		}
		oz = n >= 3 ? 2 : 1;
		break;
	}
	}
	Vec4 out = {};
	out.u[0] = d[0];
	out.u[1] = d[1];
	ApplyPrefixD(c, out.f, oz);
	WriteVector(c, out.f, oz, Vd(w));
	EatPrefixes(c);
}

void ExecColorConv(Ctx *c, u32 w) {
	Vec4 s = {};
	const int isz = VecSize(w);
	ReadVector(c, s.f, 4, Vs(w));
	ApplySwizzleS(c, s.f, 4);
	u16 colors[4];
	for (int i = 0; i < 4; i++) {
		const u32 in = s.u[i];
		u32 col = 0;
		switch ((w >> 16) & 3) {
		case 1: {  // 4444
			const u32 a = ((in >> 24) & 0xFF) >> 4, b = ((in >> 16) & 0xFF) >> 4;
			const u32 g = ((in >> 8) & 0xFF) >> 4, r = (in & 0xFF) >> 4;
			col = (a << 12) | (b << 8) | (g << 4) | r;
			break;
		}
		case 2: {  // 5551
			const u32 a = ((in >> 24) & 0xFF) >> 7, b = ((in >> 16) & 0xFF) >> 3;
			const u32 g = ((in >> 8) & 0xFF) >> 3, r = (in & 0xFF) >> 3;
			col = (a << 15) | (b << 10) | (g << 5) | r;
			break;
		}
		case 3: {  // 565
			const u32 b = ((in >> 16) & 0xFF) >> 3, g = ((in >> 8) & 0xFF) >> 2, r = (in & 0xFF) >> 3;
			col = (b << 11) | (g << 5) | r;
			break;
		}
		}
		colors[i] = (u16)col;
	}
	Vec4 ov = {};
	ov.u[0] = (u32)colors[0] | ((u32)colors[1] << 16);
	ov.u[1] = (u32)colors[2] | ((u32)colors[3] << 16);
	ApplyPrefixD(c, ov.f, 2);
	WriteVector(c, ov.f, isz == 1 ? 1 : 2, Vd(w));
	EatPrefixes(c);
}

// ---- Dot products and friends ----

void ExecVDot(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, 4);
	ReadVector(c, t, n, Vt(w));
	ApplySwizzleT(c, t, 4);
	float d = kAccurateDot ? AccurateDot(s, t) : SumDot4(s, t);
	ApplyPrefixD(c, &d, 1);
	WriteVector(c, &d, 1, Vd(w));
	EatPrefixes(c);
}

void ExecVHdp(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ReadVector(c, t, n, Vt(w));
	ApplySwizzleT(c, t, 4);
	// The S prefix forces constant 1 for the last element; otherwise this is vdot.
	u32 remove, add;
	if (n == 4) {
		remove = Swizzle(0, 0, 0, 3);
		add = MakeConstants(C_NONE, C_NONE, C_NONE, C_ONE);
	} else if (n == 3) {
		remove = Swizzle(0, 0, 3, 0);
		add = MakeConstants(C_NONE, C_NONE, C_ONE, C_NONE);
	} else if (n == 2) {
		remove = Swizzle(0, 3, 0, 0);
		add = MakeConstants(C_NONE, C_ONE, C_NONE, C_NONE);
	} else {
		remove = Swizzle(3, 0, 0, 0);
		add = MakeConstants(C_ONE, C_NONE, C_NONE, C_NONE);
	}
	ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, remove, add), 4);
	const float sum = kAccurateDot ? F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t)))
	                               : SumDot4(s, t);
	float d = IsNaN(sum) ? FAbs(sum) : sum;
	ApplyPrefixD(c, &d, 1);
	WriteVector(c, &d, 1, Vd(w));
	EatPrefixes(c);
}

void ExecVdet(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const int n = VecSize(w);
	// Normally a pair. Unfilled s/t values are zero.
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, 4);
	ReadVector(c, t, n, Vt(w));
	// The T prefix forces the swizzle for x and y (yx??), without forced negation.
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, Swizzle(3, 3, 0, 0), Swizzle(1, 0, 0, 0)), 4);
	if (kAccurateDot) {
		s[1] = FNeg(s[1]);
		d[0] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t)));
	} else {
		const float p0 = s[0] * t[0], p1 = s[1] * t[1], p2 = s[2] * t[2], p3 = s[3] * t[3];
		d[0] = p0 - p1;
		const float q = p2 + p3;
		d[0] = d[0] + q;
	}
	ApplyPrefixD(c, d, 1);
	WriteVector(c, d, 1, Vd(w));
	EatPrefixes(c);
}

void ExecVfad(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, 4);
	// The T prefix generates constants, but abs can change the constant.
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), MakeConstants(C_ONE, C_ONE, C_ONE, C_ONE)), 4);
	float d = kAccurateDot ? AccurateDot(s, t) : SumDot4(s, t);
	ApplyPrefixD(c, &d, 1);
	WriteVector(c, &d, 1, Vd(w));
	EatPrefixes(c);
}

void ExecVavg(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, 4);
	// The T prefix generates constants, but supports negate.
	const int k = n == 1 ? C_ZERO : n == 2 ? C_HALF : n == 3 ? C_THIRD : C_FOURTH;
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle() | PfxAbs(1, 1, 1, 1), MakeConstants(k, k, k, k)), 4);
	float d = kAccurateDot ? AccurateDot(s, t) : SumDot4(s, t);
	ApplyPrefixD(c, &d, 1);
	WriteVector(c, &d, 1, Vd(w));
	EatPrefixes(c);
}

void ExecVScl(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const u32 vt = Vt(w);
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, n);
	// The T prefix forces the swizzle (zzzz for some reason, so it works as a quad), so negate
	// still works but constants are a bit weird.
	const int tlane = (vt >> 5) & 3;
	t[tlane] = c->v[VOffset(vt)];
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), Swizzle(tlane, tlane, tlane, tlane)), 4);
	for (int i = 0; i < n; i++) d[i] = s[i] * t[i];
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

void ExecVbfy(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4];
	const u32 vs = Vs(w);
	const int n = VecSize(w);
	ReadVector(c, s, n, vs);
	ReadVector(c, t, n, vs);
	if (w & 0x10000) {
		// vbfy2: S forces negate on z and w, T forces a zwxy swizzle.
		ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, 0, PfxNeg(0, 0, 1, 1)), n);
		ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), Swizzle(2, 3, 0, 1)), n);
	} else {
		// vbfy1: S forces negate on y and w, T forces a yxwz swizzle.
		ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, 0, PfxNeg(0, 1, 0, 1)), n);
		ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, AnySwizzle(), Swizzle(1, 0, 3, 2)), n);
	}
	for (int i = 0; i < 4; i++) d[i] = s[i] + t[i];
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

// vsrt1-4 sort lane pairs. T is S with a forced swizzle: yxwz for 1 and 3, wzyx for 2 and 4.
template <int kind>
static void Vsrt(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const u32 vs = Vs(w);
	const int n = VecSize(w);
	ReadVector(c, s, n, vs);
	ApplySwizzleS(c, s, n);
	ReadVector(c, t, n, vs);
	const u32 add = (kind == 1 || kind == 3) ? Swizzle(1, 0, 3, 2) : Swizzle(3, 2, 1, 0);
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, Swizzle(3, 3, 3, 3), add), n);
	switch (kind) {
	case 1:
		// Compared as (y, x) and (w, z), so a tie keeps the lower lane.
		d[0] = VMin(t[0], s[0]);
		d[1] = VMax(s[1], t[1]);
		d[2] = VMin(t[2], s[2]);
		d[3] = VMax(s[3], t[3]);
		break;
	case 2:
		// Compared as (w, x) and (z, y), ties keep x and y.
		d[0] = VMin(t[0], s[0]);
		d[1] = VMin(t[1], s[1]);
		d[2] = VMax(s[2], t[2]);
		d[3] = VMax(s[3], t[3]);
		break;
	case 3:
		// Compared as (x, y) and (z, w), ties keep y and w.
		d[0] = VMax(s[0], t[0]);
		d[1] = VMin(t[1], s[1]);
		d[2] = VMax(s[2], t[2]);
		d[3] = VMin(t[3], s[3]);
		break;
	default:
		// Compared as (x, w) and (y, z), ties keep w and z.
		d[0] = VMax(s[0], t[0]);
		d[1] = VMax(s[1], t[1]);
		d[2] = VMin(t[2], s[2]);
		d[3] = VMin(t[3], s[3]);
		break;
	}
	RetainInvalidSwizzleST(c, d, n);
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

void ExecVsrt1(Ctx *c, u32 w) { Vsrt<1>(c, w); }
void ExecVsrt2(Ctx *c, u32 w) { Vsrt<2>(c, w); }
void ExecVsrt3(Ctx *c, u32 w) { Vsrt<3>(c, w); }
void ExecVsrt4(Ctx *c, u32 w) { Vsrt<4>(c, w); }

void ExecVcrs(Ctx *c, u32 w) {
	// Half a cross product.
	float s[4] = {}, t[4] = {}, d[4];
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ReadVector(c, t, n, Vt(w));
	// S forces a yzx? swizzle, T a zxy? swizzle; negate still works.
	ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, Swizzle(3, 3, 3, 0), Swizzle(1, 2, 0, 0)), n);
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, Swizzle(3, 3, 3, 0), Swizzle(2, 0, 1, 0)), n);
	for (int i = 0; i < 4; i++) d[i] = s[i] * t[i];
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

void ExecCrossQuat(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ReadVector(c, t, n, Vt(w));
	const u32 tremove = AnySwizzle() | PfxNeg(1, 1, 1, 1);
	switch (n) {
	case 3: {  // vcrsp.t
		if (kAccurateDot) {
			const float t0[4] = {0.0f, t[2], FNeg(t[1]), 0.0f};
			const float t1[4] = {FNeg(t[2]), 0.0f, t[0], 0.0f};
			d[0] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t0)));
			d[1] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t1)));
		} else {
			const float a0 = s[1] * t[2], b0 = s[2] * t[1];
			d[0] = a0 - b0;
			const float a1 = s[2] * t[0], b1 = s[0] * t[2];
			d[1] = a1 - b1;
		}
		// The T prefix forces swizzle and negate, which can make odd constants.
		ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, tremove, Swizzle(1, 0, 3, 2) | PfxNeg(0, 1, 0, 0)), 4);
		ApplySwizzleS(c, s, 4);
		if (kAccurateDot) {
			for (int i = 0; i < 4; ++i) {
				if (IsInf(s[i])) s[i] = 0.0f;
				if (IsInf(t[i])) t[i] = 0.0f;
			}
			d[2] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t)));
		} else {
			const float p0 = s[0] * t[0], p1 = s[1] * t[1], p2 = s[2] * t[2], p3 = s[3] * t[3];
			d[2] = p0 + p1;
			d[2] = d[2] + p2;
			d[2] = d[2] + p3;
		}
		break;
	}
	case 4: {  // vqmul.q
		if (kAccurateDot) {
			const float t0[4] = {t[3], t[2], FNeg(t[1]), t[0]};
			const float t1[4] = {FNeg(t[2]), t[3], t[0], t[1]};
			const float t2[4] = {t[1], FNeg(t[0]), t[3], t[2]};
			d[0] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t0)));
			d[1] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t1)));
			d[2] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t2)));
		} else {
			// Same association as PPSSPP's single expressions (left to right).
			float a, b, e, g;
			a = s[0] * t[3];
			b = s[1] * t[2];
			e = s[2] * t[1];
			g = s[3] * t[0];
			d[0] = a + b;
			d[0] = d[0] - e;
			d[0] = d[0] + g;
			a = FNeg(s[0]) * t[2];
			b = s[1] * t[3];
			e = s[2] * t[0];
			g = s[3] * t[1];
			d[1] = a + b;
			d[1] = d[1] + e;
			d[1] = d[1] + g;
			a = s[0] * t[1];
			b = s[1] * t[0];
			e = s[2] * t[3];
			g = s[3] * t[2];
			d[2] = a - b;
			d[2] = d[2] + e;
			d[2] = d[2] + g;
		}
		ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, tremove, Swizzle(0, 1, 2, 3) | PfxNeg(1, 1, 1, 0)), 4);
		ApplySwizzleS(c, s, n);
		if (kAccurateDot) {
			d[3] = F(DotBits(reinterpret_cast<const u32 *>(s), reinterpret_cast<const u32 *>(t)));
		} else {
			const float p0 = s[0] * t[0], p1 = s[1] * t[1], p2 = s[2] * t[2], p3 = s[3] * t[3];
			d[3] = p0 + p1;
			d[3] = d[3] + p2;
			d[3] = d[3] + p3;
		}
		break;
	}
	case 2:
		// The T swizzles are invalid, so the multiply is always zero.
		d[0] = 0;
		ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, tremove, Swizzle(0, 0, 0, 0)), 4);
		ApplySwizzleS(c, s, 4);
		// A value can come through by swizzling s[2].
		d[1] = s[2] * t[2];
		break;
	default:
		d[0] = 0;
		break;
	}
	// The D prefix applies to the last element only (mask and sat) for pair and larger.
	if (n != 1) {
		MoveDPrefixToLastLane(c, n);
		ApplyPrefixD(c, d, n);
	} else {
		// A single always writes zero.
		Ctl(c, VFPU_CTRL_DPREFIX) = 0;
	}
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

// ---- Constants, compares, conditional moves ----

void ExecVcst(Ctx *c, u32 w) {
	const float k = F(kCstBits[(w >> 16) & 0x1F]);
	const int n = VecSize(w);
	float temp[4] = {k, k, k, k};
	ApplyPrefixD(c, temp, n);
	WriteVector(c, temp, n, Vd(w));
	EatPrefixes(c);
}

void ExecVcmp(Ctx *c, u32 w) {
	const int cond = w & 0xF;
	const int n = VecSize(w);
	float s[4] = {}, t[4] = {};
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, n);
	ReadVector(c, t, n, Vt(w));
	ApplySwizzleT(c, t, n);
	int cc = 0, orVal = 0, andVal = 1;
	int affected = (1 << 4) | (1 << 5);
	for (int i = 0; i < n; i++) {
		int r;
		switch (cond) {
		case 0: r = 0; break;                                   // FL
		case 1: r = s[i] == t[i]; break;                        // EQ
		case 2: r = s[i] < t[i]; break;                         // LT
		case 3: r = s[i] <= t[i]; break;                        // LE
		case 4: r = 1; break;                                   // TR
		case 5: r = s[i] != t[i]; break;                        // NE
		case 6: r = s[i] >= t[i]; break;                        // GE
		case 7: r = s[i] > t[i]; break;                         // GT
		case 8: r = s[i] == 0.0f || s[i] == -0.0f; break;       // EZ
		case 9: r = IsNaN(s[i]); break;                         // EN
		case 10: r = IsInf(s[i]); break;                        // EI
		case 11: r = IsNaNOrInf(s[i]); break;                   // ES
		case 12: r = s[i] != 0; break;                          // NZ
		case 13: r = !IsNaN(s[i]); break;                       // NN
		case 14: r = !IsInf(s[i]); break;                       // NI
		default: r = !IsNaNOrInf(s[i]); break;                  // NS
		}
		cc |= r << i;
		orVal |= r;
		andVal &= r;
		affected |= 1 << i;
	}
	// Only the affected bits change.
	Ctl(c, VFPU_CTRL_CC) = (Ctl(c, VFPU_CTRL_CC) & ~affected) | ((cc | (orVal << 4) | (andVal << 5)) & affected);
	EatPrefixes(c);
}

void ExecVminmax(Ctx *c, u32 w) {
	Vec4 s = {}, t = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	ReadVector(c, t.f, n, Vt(w));
	ApplySwizzleT(c, t.f, n);
	// If both are zero, take t's sign. Otherwise -NaN < -inf < real < inf < NaN.
	const bool isMin = ((w >> 23) & 3) == 2;
	for (int i = 0; i < n; i++) {
		if (IsNaNOrInf(s.f[i]) || IsNaNOrInf(t.f[i])) {
			// If both are negative the comparison flips (not two's complement).
			const bool bothNeg = s.i[i] < 0 && t.i[i] < 0;
			if (isMin == bothNeg) d.i[i] = std::max(t.i[i], s.i[i]);
			else d.i[i] = std::min(t.i[i], s.i[i]);
		} else {
			d.f[i] = isMin ? StdMin(t.f[i], s.f[i]) : StdMax(t.f[i], s.f[i]);
		}
	}
	RetainInvalidSwizzleST(c, d.f, n);
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

void ExecVscmp(Ctx *c, u32 w) {
	Vec4 s = {}, t = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	ReadVector(c, t.f, n, Vt(w));
	ApplySwizzleT(c, t.f, n);
	for (int i = 0; i < n; i++) {
		const float a = s.f[i] - t.f[i];
		if (IsNaN(a)) {
			// NaN/inf are treated as just larger numbers, as in vmin/vmax.
			const int sMag = s.u[i] & 0x7FFFFFFF;
			const int tMag = t.u[i] & 0x7FFFFFFF;
			const int b = (s.i[i] < 0 ? -sMag : sMag) - (t.i[i] < 0 ? -tMag : tMag);
			d.f[i] = (float)((0 < b) - (b < 0));
		} else {
			d.f[i] = (float)((0.0f < a) - (a < 0.0f));
		}
	}
	RetainInvalidSwizzleST(c, d.f, n);
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

template <bool kGe>
static void VsgeVslt(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const int n = VecSize(w);
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, n);
	ReadVector(c, t, n, Vt(w));
	ApplySwizzleT(c, t, n);
	for (int i = 0; i < n; i++) {
		if (IsNaN(s[i]) || IsNaN(t[i])) d[i] = 0.0f;
		else d[i] = (kGe ? s[i] >= t[i] : s[i] < t[i]) ? 1.0f : 0.0f;
	}
	RetainInvalidSwizzleST(c, d, n);
	// The clamp cannot matter, so only the mask applies.
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

void ExecVsge(Ctx *c, u32 w) { VsgeVslt<true>(c, w); }
void ExecVslt(Ctx *c, u32 w) { VsgeVslt<false>(c, w); }

void ExecVcmov(Ctx *c, u32 w) {
	const u32 vd = Vd(w);
	const int tf = (w >> 19) & 1;
	const int imm3 = (w >> 16) & 7;
	const int n = VecSize(w);
	float s[4] = {}, d[4] = {};
	ReadVector(c, s, n, Vs(w));
	ApplySwizzleS(c, s, n);
	// Not only is D read (as T), but the T prefix applies to it.
	ReadVector(c, d, n, vd);
	ApplySwizzleT(c, d, n);
	const u32 cc = Ctl(c, VFPU_CTRL_CC);
	if (imm3 < 6) {
		if ((int)((cc >> imm3) & 1) == !tf) {
			for (int i = 0; i < n; i++) d[i] = s[i];
		}
	} else if (imm3 == 6) {
		for (int i = 0; i < n; i++) {
			if ((int)((cc >> i) & 1) == !tf) d[i] = s[i];
		}
	}
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, vd);
	EatPrefixes(c);
}

// ---- vadd vsub vdiv vmul ----

void ExecVecDo3(Ctx *c, u32 w) {
	float s[4] = {}, t[4] = {}, d[4] = {};
	const int n = VecSize(w);
	// 0 vadd, 1 vsub, 7 vdiv (opcode 24); 8 vmul (opcode 25).
	const int optype = (w >> 26) == 25 ? 8 : (int)((w >> 23) & 7);
	ReadVector(c, s, n, Vs(w));
	ReadVector(c, t, n, Vt(w));
	if (optype != 7) {
		ApplySwizzleS(c, s, n);
		ApplySwizzleT(c, t, n);
	} else {
		// vdiv applies the prefixes to the last element only, as a one-lane prefix. A swizzle past
		// x zeroes the result, below.
		ApplySwizzleS(c, &s[n - 1], 1, -INFINITY);
		ApplySwizzleT(c, &t[n - 1], 1, -INFINITY);
	}
	for (int i = 0; i < n; i++) {
		switch (optype) {
		case 0: d[i] = s[i] + t[i]; break;
		case 1: d[i] = s[i] - t[i]; break;
		case 7: d[i] = s[i] / t[i]; break;
		default: d[i] = s[i] * t[i]; break;
		}
	}
	if (optype == 7) {
		// For vdiv the D prefix also only applies to the last element.
		if (LastLaneSwizzleInvalid(c, VFPU_CTRL_SPREFIX) || LastLaneSwizzleInvalid(c, VFPU_CTRL_TPREFIX)) d[n - 1] = 0.0f;
		MoveDPrefixToLastLane(c, n);
		ApplyPrefixD(c, d, n);
	} else {
		RetainInvalidSwizzleST(c, d, n);
		ApplyPrefixD(c, d, n);
	}
	WriteVector(c, d, n, Vd(w));
	EatPrefixes(c);
}

// ---- Exponent manipulation: vsbn vsbz vlgb vwbn ----

void ExecVsbn(Ctx *c, u32 w) {
	Vec4 s = {}, t = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	ReadVector(c, t.f, n, Vt(w));
	ApplySwizzleT(c, t.f, n);
	// The swizzle applies to the value read as an integer.
	const u8 exp = (u8)(127 + t.i[0]);
	// Replace the exponent bits.
	const u32 prev = s.u[0] & 0x7F800000;
	if (prev != 0 && prev != 0x7F800000) d.u[0] = (s.u[0] & ~0x7F800000u) | ((u32)exp << 23);
	else d.u[0] = s.u[0];
	// The rest are copied unchanged.
	for (int i = 1; i < n; ++i) d.u[i] = s.u[i];
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

void ExecVsbz(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	// NaN and denormals pass through.
	if (IsNaN(s.f[0]) || (s.u[0] & 0x7F800000) == 0) d.u[0] = s.u[0];
	else d.u[0] = (127u << 23) | (s.u[0] & 0x007FFFFF);
	for (int i = 1; i < n; ++i) d.u[i] = s.u[i];
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

void ExecVlgb(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	const int exp = (s.u[0] & 0x7F800000) >> 23;
	if (exp == 0xFF) d.f[0] = s.f[0];
	else if (exp == 0) d.f[0] = -INFINITY;
	else d.f[0] = (float)(exp - 127);
	for (int i = 1; i < n; ++i) d.u[i] = s.u[i];
	RetainInvalidSwizzleST(c, d.f, n);
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

void ExecVwbn(Ctx *c, u32 w) {
	Vec4 s = {}, d = {};
	const int n = VecSize(w);
	const u32 exp = (w >> 16) & 0xFF;
	ReadVector(c, s.f, n, Vs(w));
	ApplySwizzleS(c, s.f, n);
	const u32 sigbit = s.u[0] & 0x80000000;
	const u32 prevExp = (s.u[0] & 0x7F800000) >> 23;
	u32 mantissa = (s.u[0] & 0x007FFFFF) | 0x00800000;
	if (prevExp != 0xFF && prevExp != 0) {
		if (exp > prevExp) mantissa = mantissa >> ((exp - prevExp) & 0xF);
		else mantissa = mantissa << ((prevExp - exp) & 0xF);
		d.u[0] = sigbit | (mantissa & 0x007FFFFF) | (exp << 23);
	} else {
		d.u[0] = s.u[0] | (exp << 23);
	}
	for (int i = 1; i < n; ++i) d.u[i] = s.u[i];
	RetainInvalidSwizzleST(c, d.f, n);
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

// ---- Random numbers ----

void ExecVrnds(Ctx *c, u32 w) {
	// The seed register is the vs field, as assemblers encode vrnds.s (PPSSPP reads vd, which
	// assembled code always leaves as S000).
	float seed = c->v[VOffset(Vs(w))];
	// Swizzles apply a constant value, constants/abs/neg vary the seed.
	ApplySwizzleS(c, &seed, 1);
	RndInit(U(seed), &c->vfpuCtrl[VFPU_CTRL_RCX0]);
	EatPrefixes(c);
}

void ExecVrndX(Ctx *c, u32 w) {
	Vec4 d = {};
	const int n = VecSize(w);
	u32 *rcx = &c->vfpuCtrl[VFPU_CTRL_RCX0];
	// Values are written in backwards order.
	for (int i = n - 1; i >= 0; i--) {
		switch ((w >> 16) & 0x1F) {
		case 1: d.u[i] = RndGenerate(rcx); break;                                       // vrndi
		case 2: d.u[i] = 0x3F800000 | (RndGenerate(rcx) & 0x007FFFFF); break;           // vrndf1 [1, 2)
		default: d.u[i] = 0x40000000 | (RndGenerate(rcx) & 0x007FFFFF); break;          // vrndf2 [2, 4)
		}
	}
	// The D prefix is broken and applies to the last element only (mask and sat).
	MoveDPrefixToLastLane(c, n);
	ApplyPrefixD(c, d.f, n);
	WriteVector(c, d.f, n, Vd(w));
	EatPrefixes(c);
}

// ---- vrot: one row of a rotation matrix ----

void ExecVrot(Ctx *c, u32 w) {
	float d[4] = {};
	const u32 vd = Vd(w), vs = Vs(w);
	const int imm = (w >> 16) & 0x1F;
	const int n = VecSize(w);
	const bool negSin = (imm & 0x10) != 0;
	const int sineLane = (imm >> 2) & 3;
	const int cosineLane = imm & 3;

	float sine, cosine;
	if (Ctl(c, VFPU_CTRL_SPREFIX) == PFX_DEFAULT_ST) {
		u32 sb, cb;
		SinCos(c->vi[VOffset(vs)], &sb, &cb);
		if (negSin) sb ^= 0x80000000;
		sine = F(sb);
		cosine = F(cb);
	} else {
		// A swizzle on S is a bit odd here, but generally only applies to the sine.
		float s[4] = {};
		ReadVector(c, s, 1, vs);
		// negSin is applied later, which handles zero better.
		ApplyPrefixST(s, Rewrite(c, VFPU_CTRL_SPREFIX, PfxNeg(1, 0, 0, 0), 0), 1);
		// The cosine ignores all prefixes, so take the original.
		cosine = F(Cos(c->vi[VOffset(vs)]));
		sine = F(Sin(U(s[0])));
		if (negSin) sine = FNeg(sine);
		RetainInvalidSwizzleST(c, &sine, 1);
	}

	if (sineLane == cosineLane) {
		for (int i = 0; i < 4; i++) d[i] = sine;
	} else {
		d[sineLane] = sine;
	}

	if (((vd >> 2) & 7) == ((vs >> 2) & 7)) {
		// Same matrix: the cosine comes from the sine/zero lane that overlaps vs.
		bool written = false;
		for (int i = 0; i < n; i++) {
			if (vs == ElemRegNum(vd, n, i)) {
				d[cosineLane] = F(Cos(U(d[i])));
				written = true;
				break;
			}
		}
		if (!written) d[cosineLane] = cosine;
	} else {
		d[cosineLane] = cosine;
	}

	// The D prefix works, just not for the cosine lane.
	const u32 remove = (3u << (cosineLane * 2)) | (1u << (8 + cosineLane));
	Ctl(c, VFPU_CTRL_DPREFIX) &= 0xFFFFF ^ remove;
	ApplyPrefixD(c, d, n);
	WriteVector(c, d, n, vd);
	EatPrefixes(c);
}

// ---- vtfm / vhtfm ----

void ExecVtfm(Ctx *c, u32 w) {
	float s[16] = {}, t[4] = {};
	float d[4] = {};
	const u32 vd = Vd(w), vs = Vs(w), vt = Vt(w);
	const int ins = (w >> 23) & 3;
	const int sz = ins + 1;
	const int n = VecSize(w);
	const int tn = std::min(n, ins + 1);
	ReadMatrix(c, s, sz, vs);
	ReadVector(c, t, sz, vt);

	if (kAccurateDot) {
		float t2[4];
		for (int i = 0; i < 4; i++) t2[i] = i < tn ? t[i] : (i == ins ? 1.0f : 0.0f);
		for (int i = 0; i < ins; i++) d[i] = AccurateDot(&s[i * 4], t2);
	} else {
		for (int i = 0; i < ins; i++) {
			d[i] = s[i * 4] * t[0];
			for (int k = 1; k < tn; k++) {
				const float p = s[i * 4 + k] * t[k];
				d[i] = d[i] + p;
			}
			if (ins >= n) d[i] = d[i] + s[i * 4 + ins];
		}
	}

	// The S and T prefixes apply to the final row only. T supplies the zero/one constants, but
	// abs still changes them.
	ApplySwizzleS(c, &s[ins * 4], 4);
	int cx = C_NONE, cy = n < 2 ? C_ZERO : C_NONE, cz = n < 3 ? C_ZERO : C_NONE, cw = n < 4 ? C_ZERO : C_NONE;
	if (ins >= n) {
		if (ins == 1) cy = C_ONE;
		else if (ins == 2) cz = C_ONE;
		else if (ins == 3) cw = C_ONE;
	}
	const u32 tremove = Swizzle(0, n < 2 ? 3 : 0, n < 3 ? 3 : 0, n < 4 ? 3 : 0);
	ApplyPrefixST(t, Rewrite(c, VFPU_CTRL_TPREFIX, tremove, MakeConstants(cx, cy, cz, cw)), 4);

	// This is really the operation every row probably uses (with constant wiring).
	if (kAccurateDot) {
		d[ins] = AccurateDot(&s[ins * 4], t);
	} else {
		d[ins] = s[ins * 4] * t[0];
		for (int k = 1; k < 4; k++) {
			const float p = s[ins * 4 + k] * t[k];
			d[ins] = d[ins] + p;
		}
	}

	// The D prefix applies to the last element only.
	MoveDPrefixToLastLane(c, sz);
	ApplyPrefixD(c, d, sz);
	WriteVector(c, d, sz, vd);
	EatPrefixes(c);
}

// ---- Loads / stores ----

static inline float LoadF(u32 addr) { return F(GRead32(addr)); }
static inline void StoreF(u32 addr, float f) { GWrite32(addr, U(f)); }

void ExecLvq(Ctx *c, u32 w) {
	const u32 addr = c->r[(w >> 21) & 31] + (u32)LsOffset(w);
	float d[4];
	for (int i = 0; i < 4; i++) d[i] = LoadF(addr + 4 * i);
	WriteVector(c, d, 4, LsRegQ(w));
}

void ExecSvq(Ctx *c, u32 w) {
	const u32 addr = c->r[(w >> 21) & 31] + (u32)LsOffset(w);
	float d[4];
	ReadVector(c, d, 4, LsRegQ(w));
	for (int i = 0; i < 4; i++) StoreF(addr + 4 * i, d[i]);
}

// lvl/lvr/svl/svr move the words of the quadword at the 16-byte boundary below (lvl/svl) or above
// (lvr/svr) the address. Misaligned addresses fault on hardware; here the word is aligned down.
void ExecLvlq(Ctx *c, u32 w) {
	const u32 addr = (c->r[(w >> 21) & 31] + (u32)LsOffset(w)) & ~3u;
	const u32 vt = LsRegQ(w);
	float d[4];
	ReadVector(c, d, 4, vt);
	const int offset = (addr >> 2) & 3;
	for (int i = 0; i < offset + 1; i++) d[3 - i] = LoadF(addr - 4 * i);
	WriteVector(c, d, 4, vt);
}

void ExecLvrq(Ctx *c, u32 w) {
	const u32 addr = (c->r[(w >> 21) & 31] + (u32)LsOffset(w)) & ~3u;
	const u32 vt = LsRegQ(w);
	float d[4];
	ReadVector(c, d, 4, vt);
	const int offset = (addr >> 2) & 3;
	for (int i = 0; i < (3 - offset) + 1; i++) d[i] = LoadF(addr + 4 * i);
	WriteVector(c, d, 4, vt);
}

void ExecSvlq(Ctx *c, u32 w) {
	const u32 addr = (c->r[(w >> 21) & 31] + (u32)LsOffset(w)) & ~3u;
	float d[4];
	ReadVector(c, d, 4, LsRegQ(w));
	const int offset = (addr >> 2) & 3;
	for (int i = 0; i < offset + 1; i++) StoreF(addr - i * 4, d[3 - i]);
}

void ExecSvrq(Ctx *c, u32 w) {
	const u32 addr = (c->r[(w >> 21) & 31] + (u32)LsOffset(w)) & ~3u;
	float d[4];
	ReadVector(c, d, 4, LsRegQ(w));
	const int offset = (addr >> 2) & 3;
	for (int i = 0; i < (3 - offset) + 1; i++) StoreF(addr + 4 * i, d[i]);
}

}  // namespace vfpu
