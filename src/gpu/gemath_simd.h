// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// GERowSum4 (GPU/Software/GEMath.h) with SSE2: four GE matrix row sums sharing their left operands, the
// result bit-identical to four scalar GERowSum calls (lanes it can't do exactly fall back to those).
#pragma once

#include <emmintrin.h>

#include "gpu/gemath.h"

namespace gpu {

// a: the left operands (truncated to float24 here); b[k]: the k-th row's four entries, already float24s.
// Returns lane c = GERowSum(a[k] * b[k][c], k < count).
template <int count>
inline void GERowSum4(const float a[4], const float b[][4], float out[4]) {
	const __m128i expMask = _mm_set1_epi32(0x7F800000);
	const __m128i fracMask = _mm_set1_epi32(0x7FFF);
	const __m128i noTerm = _mm_set1_epi32(-100000);
	const __m128i zero = _mm_setzero_si128();

	alignas(16) int32_t aBitsArr[4];
	for (int k = 0; k < 4; ++k)
		aBitsArr[k] = (int32_t)(FloatBits(a[k]) & 0xFFFFFF00);

	__m128i m[4], lsb[4];
	__m128i lsbMax = noTerm;
	for (int k = 0; k < count; ++k) {
		const int32_t ab = aBitsArr[k];
		const int ea = (ab >> 23) & 0xFF;
		const int af = (ab >> 8) & 0x7FFF;
		const __m128i bb = _mm_castps_si128(_mm_loadu_ps(b[k]));
		const __m128i eb = _mm_srli_epi32(_mm_and_si128(bb, expMask), 23);
		const __m128i bf = _mm_and_si128(_mm_srai_epi32(bb, 8), fracMask);
		const __m128i afk = _mm_set1_epi32(af);
		// (1.a' * 1.b') >> 15 = 32768 + a' + b' + (a' b' >> 15), the products in 16-bit multiplies.
		const __m128i ab16 = _mm_madd_epi16(bf, afk);
		__m128i prod = _mm_add_epi32(_mm_add_epi32(afk, _mm_set1_epi32(32768)), _mm_add_epi32(bf, _mm_srai_epi32(ab16, 15)));
		const __m128i sign = _mm_srai_epi32(_mm_xor_si128(bb, _mm_set1_epi32(ab)), 31);
		prod = _mm_sub_epi32(_mm_xor_si128(prod, sign), sign);
		const __m128i none = _mm_or_si128(_mm_set1_epi32(ea == 0 ? -1 : 0), _mm_cmpeq_epi32(eb, zero));
		m[k] = _mm_andnot_si128(none, prod);
		const __m128i l = _mm_add_epi32(eb, _mm_set1_epi32(ea - (254 + 15)));
		lsb[k] = _mm_or_si128(_mm_andnot_si128(none, l), _mm_and_si128(noTerm, none));
		const __m128i gt = _mm_cmpgt_epi32(lsb[k], lsbMax);
		lsbMax = _mm_or_si128(_mm_and_si128(gt, lsb[k]), _mm_andnot_si128(gt, lsbMax));
	}

	__m128i sum = zero;
	for (int k = 0; k < count; ++k) {
		// 2^(lsb - lsbMax), or zero once that's below the float range.
		__m128i field = _mm_add_epi32(_mm_sub_epi32(lsb[k], lsbMax), _mm_set1_epi32(127));
		field = _mm_andnot_si128(_mm_srai_epi32(field, 31), field);
		const __m128 scale = _mm_castsi128_ps(_mm_slli_epi32(field, 23));
		sum = _mm_add_epi32(sum, _mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(m[k]), scale)));
	}
	const __m128i isZero = _mm_cmpeq_epi32(sum, zero);
	const __m128i fbits = _mm_castps_si128(_mm_cvtepi32_ps(sum));
	const __m128i resultExp = _mm_add_epi32(_mm_srli_epi32(_mm_and_si128(fbits, expMask), 23), lsbMax);
	const __m128i result = _mm_andnot_si128(isZero, _mm_and_si128(_mm_add_epi32(fbits, _mm_slli_epi32(lsbMax, 23)), _mm_set1_epi32((int)0xFFFFFF00)));
	const __m128i outside = _mm_andnot_si128(isZero, _mm_or_si128(_mm_cmplt_epi32(resultExp, _mm_set1_epi32(1)), _mm_cmpgt_epi32(resultExp, _mm_set1_epi32(254))));
	_mm_storeu_ps(out, _mm_castsi128_ps(result));
	const int bad = _mm_movemask_ps(_mm_castsi128_ps(outside));
	if (bad) {
		for (int c = 0; c < 4; ++c) {
			if (bad & (1 << c)) {
				float col[4];
				for (int k = 0; k < count; ++k)
					col[k] = b[k][c];
				out[c] = GERow(a, col, count);
			}
		}
	}
}

}  // namespace gpu
