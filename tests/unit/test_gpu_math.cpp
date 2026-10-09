// SPDX-License-Identifier: GPL-2.0-or-later
// GE arithmetic: the SIMD row sum matches the scalar one bit for bit; reciprocal sanity.
#include <cmath>
#include <cstring>

#include "gpu/gemath.h"
#include "gpu/gemath_simd.h"
#include "testing.h"

using namespace gpu;

namespace {

u32 seed = 0x12345678;
u32 Rand() {
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

// Random floats over a wide exponent range, with zeros and denormals mixed in.
float RandFloat(bool float24) {
	const u32 kind = Rand() % 16;
	u32 bits;
	if (kind == 0)
		bits = 0;
	else if (kind == 1)
		bits = Rand() & 0x807FFFFF;  // denormal
	else
		bits = (Rand() & 0x80000000) | (((Rand() % 120) + 67) << 23) | (Rand() & 0x7FFFFF);
	if (float24)
		bits &= 0xFFFFFF00;
	return BitsFloat(bits);
}

}  // namespace

TEST(gpu_math_rowsum_simd_matches_scalar) {
	int mismatches = 0;
	for (int iter = 0; iter < 200000; ++iter) {
		float a[4], b[4][4], out[4];
		for (int k = 0; k < 4; ++k) {
			a[k] = RandFloat(false);
			for (int c = 0; c < 4; ++c)
				b[k][c] = RandFloat(true);
		}
		GERowSum4<4>(a, b, out);
		for (int c = 0; c < 4; ++c) {
			const float col[4] = { b[0][c], b[1][c], b[2][c], b[3][c] };
			const float ref = GERow(a, col, 4);
			if (FloatBits(ref) != FloatBits(out[c]))
				mismatches++;
		}
		GERowSum4<3>(a, b, out);
		for (int c = 0; c < 4; ++c) {
			const float col[3] = { b[0][c], b[1][c], b[2][c] };
			if (FloatBits(GERow(a, col, 3)) != FloatBits(out[c]))
				mismatches++;
		}
	}
	CHECK_EQ(mismatches, 0);
}

TEST(gpu_math_recip_and_add) {
	CHECK_EQ(GERecip(1.0f), 1.0f);
	CHECK_EQ(GERecip(2.0f), 0.5f);
	CHECK_EQ(GERecip(-4.0f), -0.25f);
	// Within the 16-bit precision elsewhere.
	for (float w : { 3.0f, 7.5f, 0.1f, 123.456f, 1e-3f, 6e4f }) {
		const float r = GERecip(TruncateToFloat24(w));
		CHECK(std::fabs(r * w - 1.0f) < 1e-4f);
	}
	CHECK_EQ(GEAdd(1.0f, 0.0f), 1.0f);
	CHECK_EQ(GEAdd(2048.0f, 0.5f), 2048.5f);
	// No guard bits: 1 + 2^-20 loses the small term at float24 precision.
	CHECK_EQ(GEAdd(1.0f, std::ldexp(1.0f, -20)), 1.0f);
	CHECK_EQ(GEFogFactor(0.5f), 128);
	CHECK_EQ(GEFogFactor(2.0f), 255);
	CHECK_EQ(GEFogFactor(-1.0f), 0);
}
