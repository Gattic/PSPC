// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The GE's own arithmetic, as measured on hardware by PPSSPP's gpu/probe experiments (GPU/Software/GEMath).
// float24s are floats with the low 8 mantissa bits clear. Scalar versions only.
#pragma once

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "common/common.h"

namespace gpu {

inline float getFloat24(u32 data) {
	data <<= 8;
	float f;
	memcpy(&f, &data, 4);
	return f;
}

inline u32 toFloat24(float f) {
	u32 i;
	memcpy(&i, &f, 4);
	return i >> 8;
}

inline u32 FloatBits(float f) {
	u32 u;
	memcpy(&u, &f, 4);
	return u;
}

inline float BitsFloat(u32 u) {
	float f;
	memcpy(&f, &u, 4);
	return f;
}

inline float TruncateToFloat24(float f) {
	return BitsFloat(FloatBits(f) & 0xFFFFFF00);
}

// A product of two float24s, truncated to a float24 in double precision.
inline float ProductToFloat24(double d) {
	u64 bits;
	memcpy(&bits, &d, sizeof(bits));
	if (((bits >> 52) & 0x7FF) != 0x7FF)
		bits &= ~((1ULL << (52 - 15)) - 1);
	memcpy(&d, &bits, sizeof(d));
	return (float)d;
}

struct GERecipSegment {
	int32_t b;
	int32_t m;
};
extern const GERecipSegment geRecipSegments[128];

inline float ScaleByPow2(float f, int n, u32 sign) {
	return BitsFloat((u32)((int32_t)FloatBits(f) + n * (1 << 23)) | sign);
}

// The GE's reciprocal. w must be a normal float24; returns a float24.
inline float GERecip(float w) {
	const u32 bits = FloatBits(w);
	const u32 i = (bits >> 8) & 0x7FFF;
	const int e = (int)((bits >> 23) & 0xFF) - 127;
	const GERecipSegment &seg = geRecipSegments[i >> 8];
	const int32_t q = (64 * seg.b + 63 + seg.m * (int32_t)(i & 255)) >> 7;
	if (e >= 126)
		return std::copysign(ldexpf((float)q, -16 - e), w);
	return ScaleByPow2((float)q, -16 - e, bits & 0x80000000);
}

float GERsqrt(float d);

// The adder has no guard bits: the smaller term is truncated to the precision of the larger one.
inline float GEAdd(float a, float b) {
	u32 ba = FloatBits(a), bb = FloatBits(b);
	const int ea = (ba >> 23) & 0xFF;
	const int eb = (bb >> 23) & 0xFF;
	if (ea == 0 || eb == 0)
		return ea == 0 ? (eb == 0 ? 0.0f : TruncateToFloat24(b)) : TruncateToFloat24(a);
	const int emax = std::max(ea, eb);
	const int na = 8 + emax - ea;
	const int nb = 8 + emax - eb;
	ba = na >= 24 ? (ba & 0x80000000) : (ba & ~((1U << na) - 1));
	bb = nb >= 24 ? (bb & 0x80000000) : (bb & ~((1U << nb) - 1));
	return BitsFloat(ba) + BitsFloat(bb);
}

inline float GEAddFloat24(float a, float b) {
	return TruncateToFloat24(GEAdd(a, b));
}

struct GERowTerm {
	int32_t mantissa;
	int lsbExp;
};

inline GERowTerm GEProduct(float a, float b) {
	const u32 ba = FloatBits(a), bb = FloatBits(b);
	const int ea = (ba >> 23) & 0xFF;
	const int eb = (bb >> 23) & 0xFF;
	if (ea == 0 || eb == 0)
		return { 0, INT_MIN };
	const u64 ma = (ba & 0x007FFFFF) | 0x00800000;
	const u64 mb = (bb & 0x007FFFFF) | 0x00800000;
	const int32_t m = (int32_t)((ma * mb) >> 31);
	return { ((ba ^ bb) & 0x80000000) ? -m : m, (ea - 127) + (eb - 127) - 15 };
}

// A matrix row summed in one go: every term truncated to the largest term's bit weight, added exactly.
inline float GERowSum(const GERowTerm *terms, int count) {
	int lsbExp = INT_MIN;
	for (int i = 0; i < count; ++i)
		lsbExp = std::max(lsbExp, terms[i].lsbExp);
	int32_t sum = 0;
	for (int i = 0; i < count; ++i) {
		const int32_t m = terms[i].mantissa;
		if (m == 0)
			continue;
		const int shift = lsbExp - terms[i].lsbExp;
		if (shift < 32)
			sum += m < 0 ? -(-m >> shift) : (m >> shift);
	}
	if (sum == 0)
		return 0.0f;
	// float(sum) * 2^lsbExp: by the exponent bits while the result stays normal.
	const u32 bits = FloatBits((float)sum);
	const int e = (int)((bits >> 23) & 0xFF) + lsbExp;
	if (e >= 1 && e <= 254)
		return BitsFloat((bits + ((u32)lsbExp << 23)) & 0xFFFFFF00);
	return TruncateToFloat24(ldexpf((float)sum, lsbExp));
}

// sum_k a[k] * b[k] as a row (a truncated to float24 here; b already is).
inline float GERow(const float *a, const float *b, int count) {
	GERowTerm terms[4];
	for (int k = 0; k < count; ++k)
		terms[k] = GEProduct(TruncateToFloat24(a[k]), b[k]);
	return GERowSum(terms, count);
}

// (world * view) * projection the way the GE combines them, 4x4 row-major (row = input component).
inline void GECombineMatrices(float out[16], const float a[16], const float b[16]) {
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			GERowTerm terms[4];
			for (int k = 0; k < 4; ++k)
				terms[k] = GEProduct(a[r * 4 + k], b[k * 4 + c]);
			out[r * 4 + c] = GERowSum(terms, 4);
		}
	}
}

inline float GEViewport(float clipC, float clipW, float scale, float center) {
	const float w = TruncateToFloat24(clipW);
	if (!std::isfinite(w) || !std::isfinite(clipC) || fabsf(w) < FLT_MIN)
		return clipC * scale / clipW + center;
	const float ndc = ProductToFloat24((double)TruncateToFloat24(clipC) * GERecip(w));
	return GEAdd(ProductToFloat24((double)ndc * scale), center);
}

inline float GEScreenZ(float clipZ, float clipW, float zScale, float zCenter) {
	return floorf(GEViewport(clipZ, clipW, zScale, zCenter));
}

float GEDot3(const float *a, const float *b);
// Normalizes v[0..2] like the GE; returns the length (0 leaves v alone). d2 = GEDot3(v, v).
float GENormalize3(float *v, float d2);
inline float GENormalize3(float *v) {
	return GENormalize3(v, GEDot3(v, v));
}

int64_t GESetupRecip(u64 absDet, int *e);

inline int GELog16(float delta) {
	return (int)((FloatBits(delta) >> 19) & 0x0FFF) - 127 * 16;
}

inline float GETruncateTexCoord(float f) {
	return BitsFloat(FloatBits(f) & 0xFFFFFE00);
}

inline float GEUVProduct(double d) {
	u64 bits;
	memcpy(&bits, &d, sizeof(bits));
	if (((bits >> 52) & 0x7FF) != 0x7FF)
		bits &= ~((1ULL << (52 - 23)) - 1);
	memcpy(&d, &bits, sizeof(d));
	return (float)d;
}

float GELightPow(float v, float e);

// The exponent of the lighting pow (gpu/probe exp221-223).
inline float PSPLightExponent(float e) {
	if (std::isnan(e))
		return std::signbit(e) ? 0.0f : 496.0f;
	if (e >= 512.0f)
		return 496.0f;
	return BitsFloat(FloatBits(e) & 0xFFF80000);
}

// Per-vertex 8-bit fog factor: min(floor(256 * f), 255), sign decides NaN/inf.
inline int GEFogFactor(float f) {
	const u32 bits = FloatBits(f);
	const u32 exp = bits >> 23;
	if ((bits & 0x80000000) != 0 || exp <= 126 - 8)
		return 0;
	if (exp > 126)
		return 255;
	const u32 mantissa = (bits & 0x007FFFFF) | 0x00800000;
	return mantissa >> (16 + 126 - exp);
}

int GELineCoverageAlpha(int64_t x0, int64_t y0, int64_t x1, int64_t y1, int px, int py);

}  // namespace gpu
