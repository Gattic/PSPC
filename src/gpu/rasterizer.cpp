// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Triangle, sprite, clear, line and point rasterization (GPU/Software/Rasterizer.cpp): the GE's fixed
// point planes for depth, Gouraud color and fog, its perspective texture coordinate planes, its pixel
// coverage rules, and its mip level selection.
#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

#include "gpu/gemath.h"
#include "gpu/pixel.h"
#include "gpu/render.h"
#include "gpu/sampler.h"

namespace gpu {

template <bool clearMode, int fmt>
static void PixelRun(const PixelState &ps, int x0, int y, int count, const u8 *alive, const int *z, const u8 *fog, const u32 *color) {
	constexpr int bpp = fmt == GE_FORMAT_8888 ? 4 : 2;
	const u32 fbRow = ps.fbOffset + (u32)(y * ps.fbStride) * bpp;
	const u32 zbRow = ps.zbOffset + (u32)(y * ps.zbStride) * 2;
	for (int i = 0; i < count; ++i) {
		if (alive[i])
			DrawPixel<clearMode, fmt>(ps, fbRow, zbRow, x0 + i, y, z[i], fog[i], color[i]);
	}
}

PixelRunFn GetPixelRunFn(const PixelState &ps) {
	if (ps.clearMode) {
		switch (ps.fbFormat) {
		case GE_FORMAT_565: return &PixelRun<true, GE_FORMAT_565>;
		case GE_FORMAT_5551: return &PixelRun<true, GE_FORMAT_5551>;
		case GE_FORMAT_4444: return &PixelRun<true, GE_FORMAT_4444>;
		default: return &PixelRun<true, GE_FORMAT_8888>;
		}
	}
	switch (ps.fbFormat) {
	case GE_FORMAT_565: return &PixelRun<false, GE_FORMAT_565>;
	case GE_FORMAT_5551: return &PixelRun<false, GE_FORMAT_5551>;
	case GE_FORMAT_4444: return &PixelRun<false, GE_FORMAT_4444>;
	default: return &PixelRun<false, GE_FORMAT_8888>;
	}
}

namespace {

constexpr int CHUNK = 64;

static inline int64_t FloorDiv(int64_t n, int64_t m) {
	return n >= 0 ? n / m : -((-n + m - 1) / m);
}

// The plane the GE interpolates depth, Gouraud color and fog with (gpu/probe exp36-41, exp54): gradients
// in fixed point with 14 fractional bits per subpixel, from the setup reciprocal, anchored at one vertex.
struct Plane {
	int64_t base = 0;  // value << 14 at screen (0, 0)
	int64_t kx = 0;    // per subpixel, << 14
	int64_t ky = 0;
	bool rightAnchored = false;
	int64_t At(int64_t x, int64_t y) const { return (base + kx * x + ky * y) >> 14; }
};

Plane ComputePlane(const int64_t X[3], const int64_t Y[3], const int64_t Z[3]) {
	Plane plane;
	const int64_t det = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
	if (det == 0) {
		plane.base = Z[0] << 14;
		return plane;
	}
	const int64_t nx = (Z[1] - Z[0]) * (Y[2] - Y[0]) - (Z[2] - Z[0]) * (Y[1] - Y[0]);
	const int64_t ny = (Z[2] - Z[0]) * (X[1] - X[0]) - (Z[1] - Z[0]) * (X[2] - X[0]);
	const u64 absDet = (u64)(det < 0 ? -det : det);
	int e;
	const int64_t q = GESetupRecip(absDet, &e);
	const int64_t sign = det < 0 ? -1 : 1;
	plane.kx = (sign * nx * q) >> (e + 2);
	plane.ky = (sign * ny * q) >> (e + 2);

	int top = 0, mid = 1, bot = 2;
	auto above = [&](int a, int b) { return Y[a] < Y[b] || (Y[a] == Y[b] && X[a] < X[b]); };
	if (above(mid, top)) std::swap(mid, top);
	if (above(bot, mid)) std::swap(bot, mid);
	if (above(mid, top)) std::swap(mid, top);
	const int64_t cross = (X[bot] - X[top]) * (Y[mid] - Y[top]) - (Y[bot] - Y[top]) * (X[mid] - X[top]);
	const bool flat = Y[top] == Y[mid] || Y[mid] == Y[bot];
	int anchor = 0;
	if (cross > 0 && !flat) {
		plane.rightAnchored = true;
		for (int i = 1; i < 3; ++i)
			if (X[i] > X[anchor] || (X[i] == X[anchor] && Y[i] < Y[anchor]))
				anchor = i;
	} else {
		for (int i = 1; i < 3; ++i)
			if (X[i] < X[anchor] || (X[i] == X[anchor] && Y[i] < Y[anchor]))
				anchor = i;
	}
	plane.base = (Z[anchor] << 14) - plane.kx * X[anchor] - plane.ky * Y[anchor];
	return plane;
}

// Perspective texture coordinates (gpu/probe exp55-56): per vertex q = 1/w and s = u * q as float24s,
// each a 15-bit integer at the largest exponent of the three, through the depth plane; a pixel's u is
// s * 1/q with the GE's reciprocal.
struct UVPlanes {
	Plane s, t, q;
	double scaleS = 0, scaleT = 0, scaleQ = 0;
	bool valid = false;
};

int SharedShift(const double v[3]) {
	int e = INT_MIN;
	for (int i = 0; i < 3; ++i) {
		if (v[i] != 0.0)
			e = std::max(e, std::ilogb(v[i]));
	}
	return e == INT_MIN ? 0 : 14 - e;
}

UVPlanes ComputeUVPlanesSTQ(const int64_t X[3], const int64_t Y[3], const double s[3], const double t[3], const double q[3]) {
	UVPlanes planes;
	auto fixed = [&](const double v[3], Plane *plane, double *scale) {
		const int shift = SharedShift(v);
		const int64_t V[3] = { (int64_t)std::ldexp(v[0], shift), (int64_t)std::ldexp(v[1], shift), (int64_t)std::ldexp(v[2], shift) };
		*plane = ComputePlane(X, Y, V);
		*scale = std::ldexp(1.0, -shift);
	};
	fixed(s, &planes.s, &planes.scaleS);
	fixed(t, &planes.t, &planes.scaleT);
	fixed(q, &planes.q, &planes.scaleQ);
	planes.valid = true;
	return planes;
}

UVPlanes ComputeUVPlanes(const int64_t X[3], const int64_t Y[3], const float u[3], const float v[3], const float w[3], const float *uq) {
	double s[3], t[3], q[3];
	for (int i = 0; i < 3; ++i) {
		const float w24 = TruncateToFloat24(w[i]);
		if (!(w24 > 0.0f) || !std::isfinite(w24))
			return UVPlanes{};
		const double r = GERecip(w24);
		q[i] = uq ? ProductToFloat24((double)TruncateToFloat24(uq[i]) * r) : r;
		s[i] = ProductToFloat24((double)TruncateToFloat24(u[i]) * r);
		t[i] = ProductToFloat24((double)TruncateToFloat24(v[i]) * r);
	}
	return ComputeUVPlanesSTQ(X, Y, s, t, q);
}

// The largest of the s and t planes' gradients, in texels per pixel.
float UVPlaneGradient(const UVPlanes &planes, const SamplerState &ss, bool inTexels) {
	if (!planes.valid)
		return -1.0f;
	const double perPixel = (double)SUBPIX / 16384.0;
	double gs = (double)std::max(std::llabs(planes.s.kx), std::llabs(planes.s.ky)) * perPixel * planes.scaleS;
	double gt = (double)std::max(std::llabs(planes.t.kx), std::llabs(planes.t.ky)) * perPixel * planes.scaleT;
	if (!inTexels) {
		gs *= 1 << ss.width0Shift;
		gt *= 1 << ss.height0Shift;
	}
	return (float)std::max(gs, gt);
}

// u, v and q at a pixel from the planes' values there.
inline void UVFromPlaneValues(const UVPlanes &planes, int64_t qs, int64_t ss, int64_t ts, float &s, float &t, float &q) {
	q = TruncateToFloat24((float)((double)qs * planes.scaleQ));
	if (!(q > 0.0f)) {
		s = 0.0f;
		t = 0.0f;
		return;
	}
	const double r = GERecip(q);
	s = GEUVProduct((double)TruncateToFloat24((float)((double)ss * planes.scaleS)) * r);
	t = GEUVProduct((double)TruncateToFloat24((float)((double)ts * planes.scaleT)) * r);
}

inline u8 ClampFogDepth(float fogdepth) {
	const u32 u = FloatBits(fogdepth);
	const u32 exp = u >> 23;
	if ((u & 0x80000000) != 0 || exp <= 126 - 8)
		return 0;
	if (exp > 126)
		return 255;
	const u32 mantissa = (u & 0x007FFFFF) | 0x00800000;
	return (u8)(mantissa >> (16 + 126 - exp));
}

inline bool IsRightSideOrFlatBottomLine(int vx, int vy, int l1x, int l1y, int l2x, int l2y) {
	if (l1y == l2y)
		return vy < l1y;
	const int64_t dy = l2y - l1y;
	const int64_t lhs = (int64_t)(vx - l1x) * dy;
	const int64_t rhs = (int64_t)(l2x - l1x) * (vy - l1y);
	return dy > 0 ? lhs < rhs : lhs > rhs;
}

inline u32 PackClamped(const int c[4]) {
	return (u32)Clamp255(c[0]) | ((u32)Clamp255(c[1]) << 8) | ((u32)Clamp255(c[2]) << 16) | ((u32)Clamp255(c[3]) << 24);
}

inline bool DoubleSecondaryColor(const RasterState &state) {
	return state.enableTextures && state.samp.useColorDoubling;
}

// Final color of a textured pixel: texture function on prim, plus the secondary color.
inline u32 TexturePixel(const RasterState &state, u32 prim, u32 sec, float s, float t, int level, int levelFrac, bool linear) {
	const u32 texel = SampleTexture(state.samp, s, t, level, levelFrac, linear);
	int out[4];
	TextureFunction(state.samp, prim, texel, out);
	out[0] += sec & 0xFF;
	out[1] += (sec >> 8) & 0xFF;
	out[2] += (sec >> 16) & 0xFF;
	return PackClamped(out);
}

inline u32 AddSecondary(u32 prim, u32 sec) {
	if (sec == 0)
		return prim;
	const int c[4] = { (int)(prim & 0xFF) + (int)(sec & 0xFF), (int)((prim >> 8) & 0xFF) + (int)((sec >> 8) & 0xFF),
		(int)((prim >> 16) & 0xFF) + (int)((sec >> 16) & 0xFF), (int)(prim >> 24) };
	return PackClamped(c);
}

// The secondary color as added: doubled with color doubling.
inline u32 SecondaryOf(const RasterState &state, u32 c1) {
	c1 &= 0x00FFFFFF;
	if (!DoubleSecondaryColor(state) || c1 == 0)
		return c1;
	// Doubled channels can exceed 255: keep them as 9-bit values in a wider layout via AddSecondary's ints.
	return c1;
}

inline bool EarlyDepthFail(const PixelState &ps, u32 zbRow, int x, int z) {
	if (ps.applyDepthRange && (z < ps.minz || z > ps.maxz))
		return true;
	return !CompareTest(ps.depthTestFunc, z, ReadZ(ps.fb, zbRow + (u32)x * 2));
}

// Level and filter that don't depend on the pixel.
struct LodInfo {
	bool constant;  // level/frac/filt fixed for the primitive
	int level, levelFrac;
	bool filt;
};

LodInfo ConstantLod(const SamplerState &ss) {
	LodInfo lod{};
	if (ss.maxLevel == 0 && ss.minFilt == ss.magFilt) {
		lod.constant = true;
		lod.level = 0;
		lod.levelFrac = 0;
		lod.filt = ss.magFilt;
	} else if (ss.texLevelMode != GE_TEXLEVEL_MODE_AUTO && ss.texLevelMode != GE_TEXLEVEL_MODE_SLOPE) {
		lod.constant = true;
		CalculateSamplingParams(ss, 0.0f, 0.0f, 0.0f, -1.0f, lod.level, lod.levelFrac, lod.filt);
	}
	return lod;
}

}  // namespace

void DrawTriangle(const RVertex &v0, const RVertex &v1, const RVertex &v2, const Range &range, const RasterState &state) {
	const PixelState &ps = state.pix;
	const bool clearMode = ps.clearMode;
	const int x1 = range.x1, x2 = range.x2;
	const int minY = range.y1, maxY = range.y2;

	const bool flatZ = v0.z == v1.z && v0.z == v2.z;
	if (ps.applyDepthRange && flatZ) {
		if (v0.z < ps.minz || v0.z > ps.maxz)
			return;
	}

	const RVertex *vs[3] = { &v0, &v1, &v2 };
	const int64_t X[3] = { v0.x, v1.x, v2.x };
	const int64_t Y[3] = { v0.y, v1.y, v2.y };

	// The edges as a x + b y + c + bias >= 0 at pixel centers.
	struct RowEdge {
		int64_t a, b, c;
	} edges[3];
	const int ends[3][2] = { { 1, 2 }, { 2, 0 }, { 0, 1 } };
	for (int k = 0; k < 3; ++k) {
		const RVertex &a = *vs[ends[k][0]], &b = *vs[ends[k][1]];
		const RVertex &o = *vs[k];
		const int bias = IsRightSideOrFlatBottomLine(o.x, o.y, a.x, a.y, b.x, b.y) ? -1 : 0;
		edges[k] = { (int64_t)(a.y - b.y), (int64_t)(b.x - a.x), (int64_t)b.y * a.x - (int64_t)b.x * a.y + bias };
	}

	const bool flatColor0 = !state.shadeGouraud || (v0.color0 == v1.color0 && v0.color0 == v2.color0);
	const bool flatColor1 = !state.shadeGouraud || (v0.color1 == v1.color1 && v0.color1 == v2.color1);
	const bool noFog = clearMode || !ps.applyFog || (v0.fog >= 255.0f / 256.0f && v1.fog >= 255.0f / 256.0f && v2.fog >= 255.0f / 256.0f);

	Plane depthPlane;
	if (!flatZ) {
		const int64_t Z[3] = { v0.z, v1.z, v2.z };
		depthPlane = ComputePlane(X, Y, Z);
	}
	Plane color0Planes[4], color1Planes[3], fogPlane;
	if (!flatColor0) {
		for (int i = 0; i < 4; ++i) {
			const int64_t C[3] = { (v0.color0 >> (8 * i)) & 0xFF, (v1.color0 >> (8 * i)) & 0xFF, (v2.color0 >> (8 * i)) & 0xFF };
			color0Planes[i] = ComputePlane(X, Y, C);
		}
	}
	if (!flatColor1) {
		for (int i = 0; i < 3; ++i) {
			const int64_t C[3] = { (v0.color1 >> (8 * i)) & 0xFF, (v1.color1 >> (8 * i)) & 0xFF, (v2.color1 >> (8 * i)) & 0xFF };
			color1Planes[i] = ComputePlane(X, Y, C);
		}
	}
	if (!noFog) {
		const int64_t F[3] = { ClampFogDepth(v0.fog), ClampFogDepth(v1.fog), ClampFogDepth(v2.fog) };
		fogPlane = ComputePlane(X, Y, F);
	}

	const bool textured = state.enableTextures && !clearMode;
	const SamplerState &ss = state.samp;
	UVPlanes uvPlanes;
	float autoGrad = -1.0f;
	LodInfo lod{};
	bool lodQ = false;
	float invW0 = 1.0f, invH0 = 1.0f;
	if (textured) {
		const float u[3] = { v0.s, v1.s, v2.s };
		const float v[3] = { v0.t, v1.t, v2.t };
		const float w[3] = { v0.clipw, v1.clipw, v2.clipw };
		const float q[3] = { v0.q, v1.q, v2.q };
		uvPlanes = ComputeUVPlanes(X, Y, u, v, w, state.textureProj && !state.throughMode ? q : nullptr);
		autoGrad = UVPlaneGradient(uvPlanes, ss, state.throughMode);
		lod = ConstantLod(ss);
		const bool lodUsesQ = ss.texLevelMode != GE_TEXLEVEL_MODE_CONST && (ss.maxLevel > 0 || ss.minFilt != ss.magFilt);
		lodQ = lodUsesQ && !state.throughMode;
		if (!lod.constant && !lodQ) {
			// Through mode: q is 1 everywhere, so the level is the same for the whole triangle.
			lod.constant = true;
			CalculateSamplingParams(ss, 0.0f, 0.0f, 1.0f, autoGrad, lod.level, lod.levelFrac, lod.filt);
		}
		invW0 = 1.0f / (float)(1 << ss.width0Shift);
		invH0 = 1.0f / (float)(1 << ss.height0Shift);
	}

	const u32 flatC0 = v2.color0;
	const bool dblSec = DoubleSecondaryColor(state);
	const int64_t scissorLo = -FloorDiv(-(int64_t)x1, SUBPIX), scissorHi = FloorDiv(x2, SUBPIX);
	const PixelRunFn run = GetPixelRunFn(ps);
	const u32 zbStride2 = (u32)ps.zbStride * 2;

	alignas(16) u8 alive[CHUNK];
	alignas(16) int zBuf[CHUNK];
	alignas(16) u8 fogBuf[CHUNK];
	alignas(16) u32 colorBuf[CHUNK];

	for (int64_t curY = minY; curY <= maxY; curY += SUBPIX) {
		const int64_t yc = curY + SUBPIX / 2;
		int64_t coverLo = INT64_MIN / 2, coverHi = INT64_MAX / 2;
		for (int k = 0; k < 3; ++k) {
			const RowEdge &edge = edges[k];
			const int64_t rest = edge.b * yc + edge.c;
			if (edge.a > 0) {
				const int64_t x = -FloorDiv(rest, edge.a);
				coverLo = std::max(coverLo, -FloorDiv(SUBPIX / 2 - x, SUBPIX));
			} else if (edge.a < 0) {
				const int64_t x = FloorDiv(rest, -edge.a);
				coverHi = std::min(coverHi, FloorDiv(x - SUBPIX / 2, SUBPIX));
			} else if (rest < 0) {
				coverLo = INT64_MAX / 2;
			}
		}
		const int64_t lo = std::max(scissorLo, coverLo), hi = std::min(scissorHi, coverHi);
		if (lo > hi)
			continue;
		const int py = (int)(curY / SUBPIX);
		const u32 zbRow = ps.zbOffset + (u32)py * zbStride2;

		for (int64_t chunk = lo; chunk <= hi; chunk += CHUNK) {
			const int count = (int)std::min<int64_t>(CHUNK, hi - chunk + 1);
			const int px0 = (int)chunk;
			int anyAlive = 0;
			for (int i = 0; i < count; ++i) {
				const int64_t cx = (int64_t)(px0 + i) * SUBPIX + SUBPIX / 2;
				int z;
				if (flatZ) {
					z = v2.z;
				} else {
					const int64_t zv = depthPlane.At(cx, yc);
					z = (int)std::clamp<int64_t>(zv, 0, 65535);
				}
				zBuf[i] = z;
				u8 a = 1;
				if (ps.earlyZChecks && EarlyDepthFail(ps, zbRow, px0 + i, z))
					a = 0;
				alive[i] = a;
				anyAlive |= a;
			}
			if (!anyAlive)
				continue;

			for (int i = 0; i < count; ++i) {
				if (!alive[i])
					continue;
				const int64_t cx = (int64_t)(px0 + i) * SUBPIX + SUBPIX / 2;
				u32 prim;
				if (flatColor0) {
					prim = flatC0;
				} else {
					prim = 0;
					for (int c = 0; c < 4; ++c)
						prim |= (u32)std::clamp<int64_t>(color0Planes[c].At(cx, yc), 0, 255) << (8 * c);
				}
				u32 sec = 0;
				if (!clearMode) {
					int sc[3];
					for (int c = 0; c < 3; ++c) {
						sc[c] = flatColor1 ? (int)((v2.color1 >> (8 * c)) & 0xFF) : (int)std::clamp<int64_t>(color1Planes[c].At(cx, yc), 0, 255);
						if (dblSec)
							sc[c] *= 2;
					}
					// Secondary channels up to 510: added as ints below.
					if (textured) {
						float s, t, q;
						if (uvPlanes.valid) {
							UVFromPlaneValues(uvPlanes, uvPlanes.q.At(cx, yc), uvPlanes.s.At(cx, yc), uvPlanes.t.At(cx, yc), s, t, q);
						} else {
							// No planes (w not positive): perspective barycentrics.
							const double w0 = (double)(edges[0].a * cx + edges[0].b * yc + edges[0].c);
							const double w1 = (double)(edges[1].a * cx + edges[1].b * yc + edges[1].c);
							const double w2 = (double)(edges[2].a * cx + edges[2].b * yc + edges[2].c);
							const double q0 = w0 / v0.clipw, q1 = w1 / v1.clipw, q2 = w2 / v2.clipw;
							const double qs = q0 + q1 + q2;
							s = qs != 0.0 ? (float)((v0.s * q0 + v1.s * q1 + v2.s * q2) / qs) : 0.0f;
							t = qs != 0.0 ? (float)((v0.t * q0 + v1.t * q1 + v2.t * q2) / qs) : 0.0f;
							q = 1.0f;
						}
						if (state.throughMode) {
							s *= invW0;
							t *= invH0;
						}
						int level = lod.level, levelFrac = lod.levelFrac;
						bool filt = lod.filt;
						if (!lod.constant) {
							// The level's q: once per span of four pixels, at its second pixel in the direction
							// the row is walked, or the first inside (gpu/probe exp93, exp103-106).
							const int px = px0 + i;
							const int64_t spanX = px & ~3;
							const bool rtl = uvPlanes.q.rightAnchored;
							int64_t pick = spanX + (rtl ? 2 : 1);
							if (pick < coverLo || pick > coverHi) {
								const int64_t first = rtl ? std::min<int64_t>(spanX + 3, coverHi) : std::max<int64_t>(spanX, coverLo);
								if (first >= std::max<int64_t>(spanX, coverLo) && first <= std::min<int64_t>(spanX + 3, coverHi))
									pick = first;
							}
							float lq = q;
							if (uvPlanes.valid)
								lq = TruncateToFloat24((float)((double)uvPlanes.q.At(pick * SUBPIX + SUBPIX / 2, yc) * uvPlanes.scaleQ));
							CalculateSamplingParams(ss, 0.0f, 0.0f, lq, autoGrad, level, levelFrac, filt);
						}
						const u32 texel = SampleTexture(ss, s, t, level, levelFrac, filt);
						int out[4];
						TextureFunction(ss, prim, texel, out);
						out[0] += sc[0];
						out[1] += sc[1];
						out[2] += sc[2];
						colorBuf[i] = PackClamped(out);
					} else {
						const int out[4] = { (int)(prim & 0xFF) + sc[0], (int)((prim >> 8) & 0xFF) + sc[1], (int)((prim >> 16) & 0xFF) + sc[2], (int)(prim >> 24) };
						colorBuf[i] = PackClamped(out);
					}
				} else {
					colorBuf[i] = prim;
				}
				fogBuf[i] = noFog ? 255 : (u8)std::clamp<int64_t>(fogPlane.At(cx, yc), 0, 255);
			}
			run(ps, px0, py, count, alive, zBuf, fogBuf, colorBuf);
		}
	}
}

void DrawRectangle(const RVertex &v0, const RVertex &v1, const Range &range, const RasterState &state) {
	const PixelState &ps = state.pix;
	const int entireX1 = std::min(v0.x, v1.x);
	const int entireY1 = std::min(v0.y, v1.y);
	const int entireX2 = std::max(v0.x, v1.x) - 1;
	const int entireY2 = std::max(v0.y, v1.y) - 1;
	// Pixel centers at 16k + 7 here. The GE's sprite edges (gpu/probe): the first column is (x1 + 6) >> 4,
	// the first row (y1 + 7) >> 4, and both end at (x2 + 7) >> 4, exclusive.
	int minX = std::max(entireX1 & ~(SUBPIX - 1), range.x1) | (SUBPIX / 2 - 1);
	int minY = std::max(entireY1 & ~(SUBPIX - 1), range.y1) | (SUBPIX / 2 - 1);
	const int maxX = std::min(entireX2 - 1, range.x2);
	const int maxY = std::min(entireY2 - 1, range.y2);
	if (minX < entireX1 - 2)
		minX += SUBPIX;
	// Drawn bottom-left to top-right, the first row rounds like a column (gpu/probe exp122).
	const bool rotated = v0.x < v1.x && v0.y > v1.y;
	if (minY < entireY1 - (rotated ? 2 : 1))
		minY += SUBPIX;
	if (minX > maxX || minY > maxY)
		return;

	const int z = v1.z;
	if (ps.applyDepthRange && (z < ps.minz || z > ps.maxz))
		return;

	const bool clearMode = ps.clearMode;
	const bool textured = state.enableTextures && !clearMode;
	const SamplerState &ss = state.samp;
	UVPlanes uvPlanes;
	float autoGrad = -1.0f;
	LodInfo lod{};
	if (textured) {
		float tc0s = v0.s, tc0t = v0.t, tc1s = v1.s, tc1t = v1.t;
		if (state.throughMode) {
			const float iw = 1.0f / (float)(1 << ss.width0Shift), ih = 1.0f / (float)(1 << ss.height0Shift);
			tc0s *= iw;
			tc1s *= iw;
			tc0t *= ih;
			tc1t *= ih;
		}
		// Sprite UVs use triangle planes from three corners whose s, t and q come component by component from
		// the two vertices: s from the one that supplies the corner's x, t from the one that supplies its y
		// (swapped for a rotated sprite), q from the one that supplies its x (gpu/probe exp57, exp122, exp129).
		const bool right = v0.x < v1.x, down = v0.y < v1.y;
		const bool swapST = right != down;
		const RVertex *vv[2] = { &v0, &v1 };
		double S[2], T[2], Q[2];
		for (int k = 0; k < 2; ++k) {
			const double r = state.throughMode ? 1.0 : GERecip(TruncateToFloat24(vv[k]->clipw));
			Q[k] = r;
			S[k] = ProductToFloat24((double)TruncateToFloat24(k == 0 ? tc0s : tc1s) * r);
			T[k] = ProductToFloat24((double)TruncateToFloat24(k == 0 ? tc0t : tc1t) * r);
		}
		const int64_t left = std::min(v0.x, v1.x), top = std::min(v0.y, v1.y);
		const int64_t rightX = std::max(v0.x, v1.x), bottom = std::max(v0.y, v1.y);
		const bool bottomLeftFirst = right && !down;
		const int64_t CX[3] = { left, rightX, bottomLeftFirst ? rightX : left };
		const int64_t CY[3] = { bottomLeftFirst ? bottom : top, top, bottom };
		double cs[3], ct[3], cq[3];
		for (int k = 0; k < 3; ++k) {
			const int xs = CX[k] == v0.x ? 0 : 1, ys = CY[k] == v0.y ? 0 : 1;
			cs[k] = S[swapST ? ys : xs];
			ct[k] = T[swapST ? xs : ys];
			cq[k] = Q[xs];
		}
		uvPlanes = ComputeUVPlanesSTQ(CX, CY, cs, ct, cq);
		autoGrad = UVPlaneGradient(uvPlanes, ss, false);
		lod = ConstantLod(ss);
		if (!lod.constant && uvPlanes.q.kx == 0 && uvPlanes.q.ky == 0) {
			// q is the same at every pixel.
			lod.constant = true;
			const float q = TruncateToFloat24((float)((double)uvPlanes.q.At(0, 0) * uvPlanes.scaleQ));
			CalculateSamplingParams(ss, 0.0f, 0.0f, q, autoGrad, lod.level, lod.levelFrac, lod.filt);
		}
	}

	const u8 fog = ClampFogDepth(v1.fog);
	const u32 prim = v1.color0;
	int sec[3] = { 0, 0, 0 };
	if (!clearMode) {
		for (int c = 0; c < 3; ++c)
			sec[c] = (int)((v1.color1 >> (8 * c)) & 0xFF) * (DoubleSecondaryColor(state) ? 2 : 1);
	}
	const bool hasSec = sec[0] | sec[1] | sec[2];
	u32 flatColor = prim;
	if (!textured && !clearMode && hasSec) {
		const int out[4] = { (int)(prim & 0xFF) + sec[0], (int)((prim >> 8) & 0xFF) + sec[1], (int)((prim >> 16) & 0xFF) + sec[2], (int)(prim >> 24) };
		flatColor = PackClamped(out);
	}

	const PixelRunFn run = GetPixelRunFn(ps);
	alignas(16) u8 alive[CHUNK];
	alignas(16) int zBuf[CHUNK];
	alignas(16) u8 fogBuf[CHUNK];
	alignas(16) u32 colorBuf[CHUNK];
	for (int i = 0; i < CHUNK; ++i) {
		zBuf[i] = z;
		fogBuf[i] = fog;
	}
	const int width = (maxX - minX) / SUBPIX + 1;
	const int px0 = minX / SUBPIX;
	const u32 zbStride2 = (u32)ps.zbStride * 2;

	for (int curY = minY; curY <= maxY; curY += SUBPIX) {
		const int py = curY / SUBPIX;
		const u32 zbRow = ps.zbOffset + (u32)py * zbStride2;
		// Pixel centers are at 16k + 7 here, the GE's at 16k + 8.
		const int64_t cy = curY + 1;
		for (int first = 0; first < width; first += CHUNK) {
			const int count = std::min(CHUNK, width - first);
			const int x0 = px0 + first;
			int anyAlive = 0;
			for (int i = 0; i < count; ++i) {
				u8 a = 1;
				if (ps.earlyZChecks && !CompareTest(ps.depthTestFunc, z, ReadZ(ps.fb, zbRow + (u32)(x0 + i) * 2)))
					a = 0;
				alive[i] = a;
				anyAlive |= a;
			}
			if (!anyAlive)
				continue;
			if (textured) {
				for (int i = 0; i < count; ++i) {
					if (!alive[i])
						continue;
					const int64_t cx = (int64_t)(x0 + i) * SUBPIX + SUBPIX / 2;
					float s, t, q;
					UVFromPlaneValues(uvPlanes, uvPlanes.q.At(cx, cy), uvPlanes.s.At(cx, cy), uvPlanes.t.At(cx, cy), s, t, q);
					int level = lod.level, levelFrac = lod.levelFrac;
					bool filt = lod.filt;
					if (!lod.constant)
						CalculateSamplingParams(ss, 0.0f, 0.0f, q, autoGrad, level, levelFrac, filt);
					const u32 texel = SampleTexture(ss, s, t, level, levelFrac, filt);
					int out[4];
					TextureFunction(ss, prim, texel, out);
					out[0] += sec[0];
					out[1] += sec[1];
					out[2] += sec[2];
					colorBuf[i] = PackClamped(out);
				}
			} else {
				for (int i = 0; i < count; ++i)
					colorBuf[i] = flatColor;
			}
			run(ps, x0, py, count, alive, zBuf, fogBuf, colorBuf);
		}
	}
}

void ClearRectangle(const RVertex &v0, const RVertex &v1, const Range &range, const RasterState &state) {
	const PixelState &ps = state.pix;
	const int entireX1 = std::min(v0.x, v1.x);
	const int entireY1 = std::min(v0.y, v1.y);
	const int entireX2 = std::max(v0.x, v1.x) - 1;
	const int entireY2 = std::max(v0.y, v1.y) - 1;
	int minX = std::max(entireX1 & ~(SUBPIX - 1), range.x1) | (SUBPIX / 2 - 1);
	int minY = std::max(entireY1 & ~(SUBPIX - 1), range.y1) | (SUBPIX / 2 - 1);
	const int maxX = std::min(entireX2, range.x2);
	const int maxY = std::min(entireY2, range.y2);
	// If the top left was after the half, that pixel isn't drawn.
	if (minX < entireX1 - 1)
		minX += SUBPIX;
	if (minY < entireY1 - 1)
		minY += SUBPIX;
	const int px0 = minX / SUBPIX, py0 = minY / SUBPIX;
	// Only include the end pixel when it's >= 0.5.
	const int px1 = (maxX - SUBPIX / 2) / SUBPIX, py1 = (maxY - SUBPIX / 2) / SUBPIX;
	const int w = px1 - px0 + 1;
	if (w <= 0 || py1 < py0)
		return;
	u8 *vram = ps.fb;

	if (ps.depthClear) {
		const u16 z = (u16)v1.z;
		for (int y = py0; y <= py1; ++y) {
			const u32 row = ps.zbOffset + (u32)(y * ps.zbStride) * 2;
			for (int x = 0; x < w; ++x)
				WriteZ(vram, row + (u32)(px0 + x) * 2, z);
		}
	}

	// Bits of the stored pixel that are kept.
	u32 keepOldMask = 0xFFFFFFFF;
	if (ps.colorClear && ps.stencilClear) {
		keepOldMask = 0;
	} else {
		switch (ps.fbFormat) {
		case GE_FORMAT_565:
			if (ps.colorClear)
				keepOldMask = 0;
			break;
		case GE_FORMAT_5551:
			if (ps.colorClear)
				keepOldMask = 0xFFFF8000;
			else if (ps.stencilClear)
				keepOldMask = 0xFFFF7FFF;
			break;
		case GE_FORMAT_4444:
			if (ps.colorClear)
				keepOldMask = 0xFFFFF000;
			else if (ps.stencilClear)
				keepOldMask = 0xFFFF0FFF;
			break;
		default:
			if (ps.colorClear)
				keepOldMask = 0xFF000000;
			else if (ps.stencilClear)
				keepOldMask = 0x00FFFFFF;
			break;
		}
	}
	if (ps.applyColorWriteMask)
		keepOldMask |= ps.colorWriteMask;
	if (keepOldMask == 0xFFFFFFFF)
		return;

	const u32 newColor = v1.color0;
	const bool is32 = ps.fbFormat == GE_FORMAT_8888;
	const u32 value = is32 ? newColor : ConvertFromRGBA8888(newColor, ps.fbFormat);
	const int bpp = is32 ? 4 : 2;
	for (int y = py0; y <= py1; ++y) {
		const u32 row = ps.fbOffset + (u32)(y * ps.fbStride) * bpp;
		for (int x = 0; x < w; ++x) {
			const u32 off = row + (u32)(px0 + x) * bpp;
			if (is32) {
				const u32 old = ReadFB<GE_FORMAT_8888>(vram, off);
				WriteFB<GE_FORMAT_8888>(vram, off, (old & keepOldMask) | (value & ~keepOldMask));
			} else {
				const u32 old = ReadFB<GE_FORMAT_565>(vram, off);
				WriteFB<GE_FORMAT_565>(vram, off, ((old & keepOldMask) | (value & ~keepOldMask)) & 0xFFFF);
			}
		}
	}
}

void DrawPoint(const RVertex &v0, const Range &range, const RasterState &state) {
	const PixelState &ps = state.pix;
	const int px = v0.x / SUBPIX, py = v0.y / SUBPIX;
	const int z = v0.z;
	const u32 zbRow = ps.zbOffset + (u32)(py * ps.zbStride) * 2;
	if (ps.earlyZChecks && EarlyDepthFail(ps, zbRow, px, z))
		return;

	u32 color = v0.color0;
	if (state.enableTextures && !ps.clearMode) {
		const SamplerState &ss = state.samp;
		float s = v0.s, t = v0.t;
		if (state.throughMode) {
			s = GETruncateTexCoord(s) * (1.0f / (float)(1 << ss.width0Shift));
			t = GETruncateTexCoord(t) * (1.0f / (float)(1 << ss.height0Shift));
		} else {
			const int64_t X[3] = { v0.x, v0.x, v0.x }, Y[3] = { v0.y, v0.y, v0.y };
			const float u[3] = { v0.s, v0.s, v0.s }, v[3] = { v0.t, v0.t, v0.t }, w[3] = { v0.clipw, v0.clipw, v0.clipw };
			const float q[3] = { v0.q, v0.q, v0.q };
			const UVPlanes planes = ComputeUVPlanes(X, Y, u, v, w, state.textureProj ? q : nullptr);
			if (planes.valid) {
				float qq;
				UVFromPlaneValues(planes, planes.q.At(v0.x, v0.y), planes.s.At(v0.x, v0.y), planes.t.At(v0.x, v0.y), s, t, qq);
			} else {
				const float qv = state.textureProj ? v0.q : 1.0f;
				s = qv != 0.0f ? v0.s / qv : 0.0f;
				t = qv != 0.0f ? v0.t / qv : 0.0f;
			}
		}
		int level, levelFrac;
		bool filt;
		CalculateSamplingParams(ss, 0.0f, 0.0f, 1.0f / v0.clipw, -1.0f, level, levelFrac, filt);
		const u32 sec = (v0.color1 & 0x00FFFFFF);
		const u32 texel = SampleTexture(ss, s, t, level, levelFrac, filt);
		int out[4];
		TextureFunction(ss, color, texel, out);
		const int mul = DoubleSecondaryColor(state) ? 2 : 1;
		for (int c = 0; c < 3; ++c)
			out[c] += (int)((sec >> (8 * c)) & 0xFF) * mul;
		color = PackClamped(out);
	} else if (!ps.clearMode) {
		color = AddSecondary(color, v0.color1 & 0x00FFFFFF);
	}
	const u8 fog = ps.applyFog ? ClampFogDepth(v0.fog) : 255;
	const u8 alive = 1;
	const PixelRunFn run = GetPixelRunFn(ps);
	run(ps, px, py, 1, &alive, &z, &fog, &color);
}

namespace {

// Which pixels a line lights (gpu/probe exp72): diamond exit, in 1/16 pixel units, exact.
struct LinePixel {
	int x, y;
	float t;
};

bool LineDiamondEdgeInside(int64_t dx, int64_t dy, bool yMajor) {
	if (yMajor)
		std::swap(dx, dy);
	return dy < 0;
}

bool InLineDiamond(int64_t cx, int64_t cy, int64_t x, int64_t y, bool yMajor) {
	const int64_t dx = x - cx, dy = y - cy;
	const int64_t d = std::llabs(dx) + std::llabs(dy);
	if (d != SUBPIX / 2)
		return d < SUBPIX / 2;
	return LineDiamondEdgeInside(dx, dy, yMajor);
}

bool LineCrossesDiamond(int64_t cx, int64_t cy, int64_t x0, int64_t y0, int64_t x1, int64_t y1, bool yMajor) {
	int64_t loN = 0, loD = 1, hiN = 1, hiD = 1;
	static const int signs[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	for (const auto &s : signs) {
		const int64_t a = s[0] * (x1 - x0) + s[1] * (y1 - y0);
		const int64_t b = SUBPIX / 2 - (s[0] * (x0 - cx) + s[1] * (y0 - cy));
		if (a == 0) {
			if (b < 0)
				return false;
		} else if (a > 0) {
			if (b * hiD < hiN * a) {
				hiN = b;
				hiD = a;
			}
		} else if ((-b) * loD > loN * (-a)) {
			loN = -b;
			loD = -a;
		}
	}
	if (loN * hiD > hiN * loD)
		return false;
	const int64_t tn = loN * hiD + hiN * loD, td = 2 * loD * hiD;
	const int64_t dxN = (x0 - cx) * td + (x1 - x0) * tn;
	const int64_t dyN = (y0 - cy) * td + (y1 - y0) * tn;
	const int64_t dN = std::llabs(dxN) + std::llabs(dyN);
	if (dN < (SUBPIX / 2) * td)
		return true;
	return LineDiamondEdgeInside(dxN, dyN, yMajor);
}

void LinePixels(int64_t x0, int64_t y0, int64_t x1, int64_t y1, std::vector<LinePixel> &out) {
	out.clear();
	if (x0 == x1 && y0 == y1)
		return;
	// A diagonal line is y-major (gpu/probe exp137).
	const bool xMajor = std::llabs(x1 - x0) > std::llabs(y1 - y0);
	const int64_t a0 = xMajor ? x0 : y0, a1 = xMajor ? x1 : y1;
	const int64_t b0 = xMajor ? y0 : x0, b1 = xMajor ? y1 : x1;
	const int dir = a1 >= a0 ? 1 : -1;
	const int bdir = b1 >= b0 ? 1 : -1;
	const int64_t first = (a0 / SUBPIX) - dir, last = (a1 / SUBPIX) + dir;
	for (int64_t c = first; c != last + dir; c += dir) {
		const int64_t ac = c * SUBPIX + SUBPIX / 2;
		const int64_t num = b0 * (a1 - a0) + (ac - a0) * (b1 - b0);
		const int64_t den = a1 - a0;
		int64_t bc = num / den;
		if ((num % den != 0) && ((num < 0) != (den < 0)))
			bc--;
		const int64_t r = bc >= 0 ? bc / SUBPIX : -((-bc + SUBPIX - 1) / SUBPIX);
		for (int k = -1; k <= 1; ++k) {
			const int64_t rr = r + k * bdir;
			const int64_t px = xMajor ? c : rr, py = xMajor ? rr : c;
			const int64_t cx = px * SUBPIX + SUBPIX / 2;
			const int64_t cy = py * SUBPIX + SUBPIX / 2;
			if (InLineDiamond(cx, cy, x1, y1, !xMajor))
				continue;
			if (InLineDiamond(cx, cy, x0, y0, !xMajor) || LineCrossesDiamond(cx, cy, x0, y0, x1, y1, !xMajor)) {
				const float t = std::clamp((float)(ac - a0) / (float)(a1 - a0), 0.0f, 1.0f);
				out.push_back({ (int)px, (int)py, t });
			}
		}
	}
}

// Gouraud values along a line (gpu/probe exp137): the gradient along the major axis from the setup
// reciprocal, a pixel's value the start plus the gradient times its center's distance from v0, floored.
int64_t LineFixedAt(int64_t c0, int64_t c1, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int px, int py) {
	const bool xMajor = std::llabs(x1 - x0) > std::llabs(y1 - y0);
	const int64_t a0 = xMajor ? x0 : y0, a1 = xMajor ? x1 : y1;
	const int64_t ac = (int64_t)(xMajor ? px : py) * SUBPIX + SUBPIX / 2;
	if (a1 == a0)
		return c1;
	int e;
	const int64_t q = GESetupRecip((u64)std::llabs(a1 - a0), &e);
	const int64_t k = ((c1 - c0) * q) >> (e + 2);
	const int64_t walk = a1 > a0 ? ac - a0 : a0 - ac;
	return ((c0 << 14) + k * walk) >> 14;
}

int LineValueAt(int c0, int c1, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int px, int py, int maxValue) {
	return (int)std::clamp<int64_t>(LineFixedAt(c0, c1, x0, y0, x1, y1, px, py), 0, maxValue);
}

struct LineUV {
	int64_t s[2], t[2], q[2];
	double scaleS, scaleT, scaleQ;
	bool valid;
};

LineUV ComputeLineUV(const RVertex &v0, const RVertex &v1, bool textureProj) {
	LineUV uv{};
	double s[2], t[2], q[2];
	const RVertex *v[2] = { &v0, &v1 };
	for (int i = 0; i < 2; ++i) {
		const float w24 = TruncateToFloat24(v[i]->clipw);
		if (!(w24 > 0.0f) || !std::isfinite(w24))
			return uv;
		const double r = GERecip(w24);
		q[i] = textureProj ? ProductToFloat24((double)TruncateToFloat24(v[i]->q) * r) : r;
		s[i] = ProductToFloat24((double)TruncateToFloat24(v[i]->s) * r);
		t[i] = ProductToFloat24((double)TruncateToFloat24(v[i]->t) * r);
	}
	auto fixed = [](const double c[2], int64_t out[2], double *scale) {
		const double v3[3] = { c[0], c[1], 0.0 };
		const int shift = SharedShift(v3);
		out[0] = (int64_t)std::ldexp(c[0], shift);
		out[1] = (int64_t)std::ldexp(c[1], shift);
		*scale = std::ldexp(1.0, -shift);
	};
	fixed(s, uv.s, &uv.scaleS);
	fixed(t, uv.t, &uv.scaleT);
	fixed(q, uv.q, &uv.scaleQ);
	uv.valid = true;
	return uv;
}

float LineUVComponentAt(const int64_t c[2], double scale, const RVertex &v0, const RVertex &v1, int px, int py) {
	const int64_t value = LineFixedAt(c[0], c[1], v0.x, v0.y, v1.x, v1.y, px, py);
	return TruncateToFloat24((float)((double)value * scale));
}

void LineTextureCoordinatesAt(const LineUV &uv, const RVertex &v0, const RVertex &v1, int px, int py, float &s, float &t, float &q) {
	q = LineUVComponentAt(uv.q, uv.scaleQ, v0, v1, px, py);
	if (!(q > 0.0f)) {
		s = 0.0f;
		t = 0.0f;
		return;
	}
	const double r = GERecip(q);
	s = GEUVProduct((double)LineUVComponentAt(uv.s, uv.scaleS, v0, v1, px, py) * r);
	t = GEUVProduct((double)LineUVComponentAt(uv.t, uv.scaleT, v0, v1, px, py) * r);
}

}  // namespace

void DrawLine(const RVertex &v0, const RVertex &v1, const Range &range, const RasterState &state) {
	const PixelState &ps = state.pix;
	const int ax = v0.x, ay = v0.y, az = v0.z;
	const int bx = v1.x, by = v1.y, bz = v1.z;
	int dx = bx - ax, dy = by - ay;
	int steps = std::abs(dx) < std::abs(dy) ? std::abs(dy) / SUBPIX : std::abs(dx) / SUBPIX;
	if (dx < 0 && dx >= -SUBPIX)
		dx++;
	if (dy < 0 && dy >= -SUBPIX)
		dy++;
	const double xinc = steps ? (double)dx / steps : 0.0;
	const double yinc = steps ? (double)dy / steps : 0.0;
	const bool interpolateColor = state.shadeGouraud && !(v0.color0 == v1.color0 && v0.color1 == v1.color1);
	const int steps1 = steps == 0 ? 1 : steps;
	const bool textured = state.enableTextures && !ps.clearMode;
	const LineUV lineUV = textured && !state.throughMode ? ComputeLineUV(v0, v1, state.textureProj) : LineUV{};
	const SamplerState &ss = state.samp;
	const PixelRunFn run = GetPixelRunFn(ps);

	static thread_local std::vector<LinePixel> pixels;
	LinePixels(ax, ay, bx, by, pixels);
	for (const LinePixel &lp : pixels) {
		const int i = std::clamp((int)lroundf(lp.t * steps), 0, steps);
		const int x = lp.x * SUBPIX + SUBPIX / 2;
		const int y = lp.y * SUBPIX + SUBPIX / 2;
		if (!(x >= range.x1 && y >= range.y1 && x <= range.x2 && y <= range.y2))
			continue;
		const int z = LineValueAt(az, bz, ax, ay, bx, by, lp.x, lp.y, 65535);
		const int px = lp.x, py = lp.y;
		const u32 zbRow = ps.zbOffset + (u32)(py * ps.zbStride) * 2;
		if (ps.earlyZChecks && EarlyDepthFail(ps, zbRow, px, z))
			continue;

		int prim[4];
		int sec[3];
		for (int c = 0; c < 4; ++c) {
			const int c0 = (v0.color0 >> (8 * c)) & 0xFF, c1 = (v1.color0 >> (8 * c)) & 0xFF;
			prim[c] = interpolateColor ? LineValueAt(c0, c1, ax, ay, bx, by, lp.x, lp.y, 255) : c1;
			if (c < 3) {
				const int s0 = (v0.color1 >> (8 * c)) & 0xFF, s1 = (v1.color1 >> (8 * c)) & 0xFF;
				sec[c] = interpolateColor ? LineValueAt(s0, s1, ax, ay, bx, by, lp.x, lp.y, 255) : s1;
			}
		}
		u8 fog = 255;
		if (ps.applyFog)
			fog = ClampFogDepth((v0.fog * (float)(steps1 - i) + v1.fog * (float)i) / steps1);
		if (state.antialiasLines)
			prim[3] = GELineCoverageAlpha(ax, ay, bx, by, lp.x, lp.y);

		u32 color = PackClamped(prim);
		if (textured) {
			float s, s1, t, t1;
			if (state.throughMode) {
				const float f0 = (float)(steps1 - i) / steps1, f1 = (float)i / steps1;
				const float g0 = (float)(steps1 - i - 1) / steps1, g1 = (float)(i + 1) / steps1;
				s = (v0.s * f0 + v1.s * f1) * (1.0f / (float)(1 << ss.width0Shift));
				t = (v0.t * f0 + v1.t * f1) * (1.0f / (float)(1 << ss.height0Shift));
				s1 = (v0.s * g0 + v1.s * g1) * (1.0f / (float)(1 << ss.width0Shift));
				t1 = (v0.t * g0 + v1.t * g1) * (1.0f / (float)(1 << ss.height0Shift));
			} else if (lineUV.valid) {
				float q, q1;
				LineTextureCoordinatesAt(lineUV, v0, v1, lp.x, lp.y, s, t, q);
				const bool xMajor = std::abs(dx) > std::abs(dy);
				const int stepX = xMajor ? (dx >= 0 ? 1 : -1) : 0, stepY = xMajor ? 0 : (dy >= 0 ? 1 : -1);
				LineTextureCoordinatesAt(lineUV, v0, v1, lp.x + stepX, lp.y + stepY, s1, t1, q1);
			} else {
				s = s1 = v1.s;
				t = t1 = v1.t;
			}
			const float ds = xinc == 0.0 ? 0.0f : (s1 - s) * (float)SUBPIX * (float)(1.0 / xinc);
			const float dt = yinc == 0.0 ? 0.0f : (t1 - t) * (float)SUBPIX * (float)(1.0 / yinc);
			const float w = (v0.clipw * (float)(steps - i) + v1.clipw * (float)i) / steps1;
			int level, levelFrac;
			bool filt;
			CalculateSamplingParams(ss, ds, dt, 1.0f / w, -1.0f, level, levelFrac, filt);
			if (state.antialiasLines) {
				s = ((float)px + (float)(xinc / 32.0)) / 512.0f;
				t = ((float)py + (float)(yinc / 32.0)) / 512.0f;
				filt = true;
			}
			const u32 texel = SampleTexture(ss, s, t, level, levelFrac, filt);
			int out[4];
			TextureFunction(ss, color, texel, out);
			const int mul = DoubleSecondaryColor(state) ? 2 : 1;
			for (int c = 0; c < 3; ++c)
				out[c] += sec[c] * mul;
			color = PackClamped(out);
		} else if (!ps.clearMode) {
			const int out[4] = { prim[0] + sec[0], prim[1] + sec[1], prim[2] + sec[2], prim[3] };
			color = PackClamped(out);
		}
		const u8 alive = 1;
		run(ps, px, py, 1, &alive, &z, &fog, &color);
	}
}

}  // namespace gpu
