// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include "gpu/hw/draw_data.h"
#include "common/log.h"
#include "gpu/gemath.h"
#include "gpu/sampler.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace gpu::hw {
namespace {

// rasterizer.cpp ClampFogDepth: the fog factor 0-255 from a vertex's fog depth.
inline float ClampFogDepth(float fogdepth) {
	const u32 u = FloatBits(fogdepth);
	const u32 exp = u >> 23;
	if ((u & 0x80000000) != 0 || exp <= 126 - 8)
		return 0.0f;
	if (exp > 126)
		return 255.0f;
	const u32 mantissa = (u & 0x007FFFFF) | 0x00800000;
	return (float)(u8)(mantissa >> (16 + 126 - exp));
}

// A linear function of screen subpixels through three points (a sprite's UV planes).
struct LinearFn {
	double a = 0, b = 0, c = 0;
	double At(double x, double y) const { return a + b * x + c * y; }
};

LinearFn PlaneThrough(const int64_t X[3], const int64_t Y[3], const double F[3]) {
	LinearFn f;
	const double det = (double)(X[1] - X[0]) * (double)(Y[2] - Y[0]) - (double)(X[2] - X[0]) * (double)(Y[1] - Y[0]);
	if (det == 0.0) {
		f.a = F[0];
		return f;
	}
	f.b = ((F[1] - F[0]) * (double)(Y[2] - Y[0]) - (F[2] - F[0]) * (double)(Y[1] - Y[0])) / det;
	f.c = ((F[2] - F[0]) * (double)(X[1] - X[0]) - (F[1] - F[0]) * (double)(X[2] - X[0])) / det;
	f.a = F[0] - f.b * (double)X[0] - f.c * (double)Y[0];
	return f;
}

// The pixels DrawRectangle (rasterizer.cpp) fills, inclusive.
bool RectBounds(const RVertex &v0, const RVertex &v1, const Range &range, int *px0, int *py0, int *px1, int *py1) {
	const int entireX1 = std::min(v0.x, v1.x);
	const int entireY1 = std::min(v0.y, v1.y);
	const int entireX2 = std::max(v0.x, v1.x) - 1;
	const int entireY2 = std::max(v0.y, v1.y) - 1;
	int minX = std::max(entireX1 & ~(SUBPIX - 1), range.x1) | (SUBPIX / 2 - 1);
	int minY = std::max(entireY1 & ~(SUBPIX - 1), range.y1) | (SUBPIX / 2 - 1);
	const int maxX = std::min(entireX2 - 1, range.x2);
	const int maxY = std::min(entireY2 - 1, range.y2);
	if (minX < entireX1 - 2)
		minX += SUBPIX;
	const bool rotated = v0.x < v1.x && v0.y > v1.y;
	if (minY < entireY1 - (rotated ? 2 : 1))
		minY += SUBPIX;
	if (minX > maxX || minY > maxY)
		return false;
	*px0 = minX / SUBPIX;
	*py0 = minY / SUBPIX;
	*px1 = *px0 + (maxX - minX) / SUBPIX;
	*py1 = *py0 + (maxY - minY) / SUBPIX;
	return true;
}

// The pixels ClearRectangle (rasterizer.cpp) fills, inclusive.
bool ClearBounds(const RVertex &v0, const RVertex &v1, const Range &range, int *px0, int *py0, int *px1, int *py1) {
	const int entireX1 = std::min(v0.x, v1.x);
	const int entireY1 = std::min(v0.y, v1.y);
	const int entireX2 = std::max(v0.x, v1.x) - 1;
	const int entireY2 = std::max(v0.y, v1.y) - 1;
	int minX = std::max(entireX1 & ~(SUBPIX - 1), range.x1) | (SUBPIX / 2 - 1);
	int minY = std::max(entireY1 & ~(SUBPIX - 1), range.y1) | (SUBPIX / 2 - 1);
	const int maxX = std::min(entireX2, range.x2);
	const int maxY = std::min(entireY2, range.y2);
	if (minX < entireX1 - 1)
		minX += SUBPIX;
	if (minY < entireY1 - 1)
		minY += SUBPIX;
	*px0 = minX / SUBPIX;
	*py0 = minY / SUBPIX;
	*px1 = (maxX - SUBPIX / 2) / SUBPIX;
	*py1 = (maxY - SUBPIX / 2) / SUBPIX;
	return *px1 >= *px0 && *py1 >= *py0;
}

}  // namespace

bool SameTexture(const SamplerState &a, const SamplerState &b) {
	if (a.texfmt != b.texfmt || a.swizzle != b.swizzle || a.maxLevel != b.maxLevel || a.clut != b.clut ||
		a.clutfmt != b.clutfmt || a.clutShift != b.clutShift || a.clutMask != b.clutMask || a.clutOffset != b.clutOffset ||
		a.useSharedClut != b.useSharedClut)
		return false;
	for (int i = 0; i <= a.maxLevel; ++i) {
		const TexLevel &x = a.levels[i], &y = b.levels[i];
		if (x.addr != y.addr || x.bufw != y.bufw || x.w != y.w || x.h != y.h)
			return false;
	}
	return a.clampS == b.clampS && a.clampT == b.clampT && a.minFilt == b.minFilt && a.magFilt == b.magFilt && a.mipFilt == b.mipFilt;
}

namespace {

// Whether two states can share a draw call.
bool Compatible(const RasterState &a, const RasterState &b) {
	if (a.pix.fbFormat != b.pix.fbFormat || a.pix.clearMode != b.pix.clearMode)
		return false;
	const bool ta = a.enableTextures && !a.pix.clearMode, tb = b.enableTextures && !b.pix.clearMode;
	if (ta != tb || memcmp(&a.scissor, &b.scissor, sizeof(Range)) != 0)
		return false;
	return !ta || SameTexture(a.samp, b.samp);
}

inline u64 Rotl(u64 x, int r) {
	return (x << r) | (x >> (64 - r));
}

constexpr u64 P3 = 0x165667B19E3779F9ULL;

inline u64 Round(u64 acc, u64 v) {
	return Rotl(acc + v * P2, 31) * P1;
}

typedef void (*DecodeFn)(const SamplerState &ss, int level, int w, int h, u32 *out, size_t pitch);

// One level's first w x h texels (the sampler never reads past 512 in either direction).
template <int fmt, bool swz>
void DecodeLevelT(const SamplerState &ss, int level, int w, int h, u32 *out, size_t pitch) {
	for (int v = 0; v < h; ++v) {
		u32 *row = out + (size_t)v * pitch;
		for (int u = 0; u < w; ++u)
			row[u] = FetchTexelT<fmt, swz>(ss, level, u, v);
	}
}

// Linear direct-color levels whose rows are all in memory: whole rows at a time (same results).
template <int fmt>
void DecodeLinearDirect(const SamplerState &ss, int level, int w, int h, u32 *out, size_t pitch) {
	const TexLevel &lv = ss.levels[level];
	constexpr int bpp = fmt == GE_TFMT_8888 ? 4 : 2;
	if (!lv.ptr || (u64)(h - 1) * lv.bufw * bpp + (u64)w * bpp + 4 > lv.validBytes) {
		DecodeLevelT<fmt, false>(ss, level, w, h, out, pitch);
		return;
	}
	for (int v = 0; v < h; ++v) {
		const u8 *src = lv.ptr + (size_t)v * lv.bufw * bpp;
		u32 *row = out + (size_t)v * pitch;
		if constexpr (fmt == GE_TFMT_8888) {
			memcpy(row, src, (size_t)w * 4);
		} else {
			const u16 *s16 = (const u16 *)src;
			for (int u = 0; u < w; ++u) {
				if constexpr (fmt == GE_TFMT_5650)
					row[u] = RGB565ToRGBA8888(s16[u]);
				else if constexpr (fmt == GE_TFMT_5551)
					row[u] = RGBA5551ToRGBA8888(s16[u]);
				else
					row[u] = RGBA4444ToRGBA8888(s16[u]);
			}
		}
	}
}

template <int fmt>
DecodeFn PickDecode(bool swizzle) {
	if constexpr (fmt <= GE_TFMT_8888) {
		if (!swizzle)
			return &DecodeLinearDirect<fmt>;
	}
	return swizzle ? &DecodeLevelT<fmt, true> : &DecodeLevelT<fmt, false>;
}

DecodeFn GetDecodeFn(int texfmt, bool swizzle) {
	switch (texfmt) {
	case GE_TFMT_5650: return PickDecode<GE_TFMT_5650>(swizzle);
	case GE_TFMT_5551: return PickDecode<GE_TFMT_5551>(swizzle);
	case GE_TFMT_4444: return PickDecode<GE_TFMT_4444>(swizzle);
	case GE_TFMT_8888: return PickDecode<GE_TFMT_8888>(swizzle);
	case GE_TFMT_CLUT4: return PickDecode<GE_TFMT_CLUT4>(swizzle);
	case GE_TFMT_CLUT8: return PickDecode<GE_TFMT_CLUT8>(swizzle);
	case GE_TFMT_CLUT16: return PickDecode<GE_TFMT_CLUT16>(swizzle);
	case GE_TFMT_CLUT32: return PickDecode<GE_TFMT_CLUT32>(swizzle);
	// DXT isn't swizzled; invalid formats have no texel pointer and read 0 (sampler.cpp).
	case GE_TFMT_DXT1: return &DecodeLevelT<GE_TFMT_DXT1, false>;
	case GE_TFMT_DXT3: return &DecodeLevelT<GE_TFMT_DXT3, false>;
	default: return &DecodeLevelT<GE_TFMT_DXT5, false>;
	}
}

}  // namespace

u64 HashBytes(const u8 *p, size_t n, u64 seed) {
	u64 a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
	size_t i = 0;
	for (; i + 32 <= n; i += 32) {
		u64 w[4];
		memcpy(w, p + i, 32);
		a = Round(a, w[0]);
		b = Round(b, w[1]);
		c = Round(c, w[2]);
		d = Round(d, w[3]);
	}
	u64 h = Rotl(a, 1) + Rotl(b, 7) + Rotl(c, 12) + Rotl(d, 18) + (u64)n;
	for (; i + 8 <= n; i += 8) {
		u64 w;
		memcpy(&w, p + i, 8);
		h ^= Round(0, w);
		h = Rotl(h, 27) * P1 + P3;
	}
	for (; i < n; ++i) {
		h ^= p[i] * P3;
		h = Rotl(h, 11) * P1;
	}
	h ^= h >> 33;
	h *= P2;
	h ^= h >> 29;
	h *= P3;
	h ^= h >> 32;
	return h;
}

void TextureAtlasLayout(const SamplerState &ss, u32 levels[8], int *atlasW, int *atlasH) {
	int x = 0, h = 1;
	for (int i = 0; i < 8; ++i) {
		const TexLevel &lv = ss.levels[i];
		if (i > ss.maxLevel) {
			levels[i] = levels[i > 0 ? i - 1 : 0];
			continue;
		}
		const int ws = lv.w ? (int)Log2Pow2(lv.w) : 0, hs = lv.h ? (int)Log2Pow2(lv.h) : 0;
		levels[i] = (u32)x | ((u32)ws << 16) | ((u32)hs << 20);
		x += std::min<int>(lv.w, 512);
		h = std::max(h, std::min<int>(lv.h, 512));
	}
	*atlasW = std::max(x, 1);
	*atlasH = h;
}


void DecodeTextureLevel(const SamplerState &ss, int level, int w, int h, u32 *out, size_t pitch) {
	GetDecodeFn(ss.texfmt, ss.swizzle)(ss, level, w, h, out, pitch);
}
namespace {

void TexCoordsPerspective(const RVertex &v, bool proj, GpuVertex &g) {
	const float w24 = TruncateToFloat24(v.clipw);
	double r;
	if (w24 > 0.0f && IsFiniteF(w24))
		r = GERecip(w24);
	else
		r = v.clipw != 0.0f ? 1.0 / v.clipw : 1.0;
	g.s = (float)(v.s * r);
	g.t = (float)(v.t * r);
	g.q = (float)((proj ? v.q : 1.0f) * r);
}

// rasterizer.cpp UVPlaneGradient for a triangle: the largest s and t plane gradient, in texels per
// pixel (of the render scale).
float TriangleLodImpl(const GpuVertex *v, const SamplerState &ss, int scale) {
	const double x1 = v[1].x - v[0].x, y1 = v[1].y - v[0].y, x2 = v[2].x - v[0].x, y2 = v[2].y - v[0].y;
	const double det = x1 * y2 - x2 * y1;
	if (det == 0.0)
		return 0.0f;
	auto grad = [&](double f0, double f1, double f2) {
		const double d1 = f1 - f0, d2 = f2 - f0;
		const double kx = (d1 * y2 - d2 * y1) / det, ky = (d2 * x1 - d1 * x2) / det;
		return std::max(std::abs(kx), std::abs(ky));
	};
	const double gs = grad(v[0].s, v[1].s, v[2].s) * (double)(1 << ss.width0Shift);
	const double gt = grad(v[0].t, v[1].t, v[2].t) * (double)(1 << ss.height0Shift);
	return (float)(std::max(gs, gt) / scale);
}

}  // namespace

void DrawData::EmitTriangle(const BatchPrim &p, const RasterState &st) {
	const bool textured = st.enableTextures && !st.pix.clearMode;
	const bool flat = !st.shadeGouraud;
	const bool proj = st.textureProj && !st.throughMode;
	const RVertex &pv = p.v[2];
	float iw = 1.0f, ih = 1.0f;
	if (textured && st.throughMode) {
		iw = 1.0f / (float)(1 << st.samp.width0Shift);
		ih = 1.0f / (float)(1 << st.samp.height0Shift);
	}
	for (int k = 0; k < 3; ++k) {
		const RVertex &v = p.v[k];
		GpuVertex g;
		g.x = (float)v.x * (1.0f / SUBPIX);
		g.y = (float)v.y * (1.0f / SUBPIX);
		g.z = (float)v.z;
		g.fog = st.pix.applyFog ? ClampFogDepth(v.fog) : 255.0f;
		g.c0 = flat ? pv.color0 : v.color0;
		g.c1 = flat ? pv.color1 : v.color1;
		g.s = g.t = 0.0f;
		g.q = 1.0f;
		if (textured) {
			if (st.throughMode) {
				g.s = v.s * iw;
				g.t = v.t * ih;
			} else {
				TexCoordsPerspective(v, proj, g);
			}
		}
		g.state = p.stateIndex;
		g.lod = 0.0f;
		verts_.push_back(g);
	}
	if (textured) {
		const float lod = TriangleLodImpl(&verts_[verts_.size() - 3], st.samp, scale_);
		for (size_t k = verts_.size() - 3; k < verts_.size(); ++k)
			verts_[k].lod = lod;
	}
}

void DrawData::VerifyGeometry(const GeometryBatch &g, const RasterState *states, const GpuVertex *gpu, TransformStats &stats, const char *tag) {
	// EmitTriangle appends to verts_, which holds the batch's own vertices: work past them.
	const size_t keep = verts_.size();
	static const char *const names[11] = { "x", "y", "z", "fog", "c0", "c1", "s", "t", "q", "state", "lod" };
	u64 fieldMismatches[11] = {};
	u64 bad = 0;
	for (size_t j = 0; j < g.jobs.size() && j < g.expectFirst.size(); ++j) {
		const GeJob &job = g.jobs[j];
		const GeDraw &d = g.draws[job.draw];
		const u32 e0 = g.expectFirst[j], e1 = j + 1 < g.expectFirst.size() ? g.expectFirst[j + 1] : (u32)g.expect.size();
		// The CPU's triangles, made into GPU vertices, against the GPU's in order (a triangle the GPU
		// leaves out is degenerate: every vertex at 0, 0).
		std::vector<GpuVertex> want, got;
		for (u32 e = e0; e + 3 <= e1; e += 3) {
			BatchPrim p{};
			p.type = PrimType::Triangle;
			p.stateIndex = d.state;
			for (int k = 0; k < 3; ++k)
				p.v[k] = g.expect[e + k];
			EmitTriangle(p, states[d.state]);
			want.insert(want.end(), verts_.end() - 3, verts_.end());
			verts_.resize(keep);
		}
		for (u32 t = 0; t < GEO_JOB_VERTS / 3; ++t) {
			const GpuVertex *v = &gpu[job.out + t * 3];
			bool degenerate = true;
			for (int k = 0; k < 3; ++k)
				degenerate = degenerate && FloatBits(v[k].x) == 0 && FloatBits(v[k].y) == 0;
			if (!degenerate)
				got.insert(got.end(), v, v + 3);
		}
		bool jobBad = false;
		if (want.size() != got.size()) {
			if (stats.mismatches < 20)
				LOGW(tag, "GPU transform: job %zu: GPU made %zu triangles, CPU %zu", j, got.size() / 3, want.size() / 3);
			jobBad = true;
		}
		for (size_t k = 0; k < want.size() && k < got.size(); ++k) {
			const u32 *a = (const u32 *)&got[k], *b = (const u32 *)&want[k];
			for (int f = 0; f < 11; ++f) {
				if (a[f] != b[f]) {
					fieldMismatches[f]++;
					if (!jobBad && stats.mismatches < 20)
						LOGW(tag, "GPU transform: job %zu vertex %zu %s: GPU %08x, CPU %08x", j, k, names[f], a[f], b[f]);
					jobBad = true;
				}
			}
		}
		if (jobBad) {
			bad++;
			stats.mismatches++;
		}
		stats.checked++;
	}
	if (bad) {
		std::string fields;
		for (int f = 0; f < 11; ++f) {
			if (fieldMismatches[f])
				fields += std::string(fields.empty() ? "" : ", ") + names[f] + " " + std::to_string(fieldMismatches[f]);
		}
		LOGW(tag, "GPU transform: %llu of %zu jobs differ (%s); %llu of %llu so far", (unsigned long long)bad, g.jobs.size(), fields.c_str(),
		     (unsigned long long)stats.mismatches, (unsigned long long)stats.checked);
	}
}

void DrawData::EmitRect(const BatchPrim &p, const RasterState &st, bool clear) {
	const RVertex &v0 = p.v[0], &v1 = p.v[1];
	int px0, py0, px1, py1;
	if (clear ? !ClearBounds(v0, v1, p.range, &px0, &py0, &px1, &py1) : !RectBounds(v0, v1, p.range, &px0, &py0, &px1, &py1))
		return;
	GpuVertex base{};
	base.z = (float)v1.z;
	base.fog = st.pix.applyFog ? ClampFogDepth(v1.fog) : 255.0f;
	base.c0 = v1.color0;
	base.c1 = clear ? 0 : v1.color1;
	base.q = 1.0f;
	base.state = p.stateIndex;
	const bool textured = !clear && st.enableTextures && !st.pix.clearMode;
	LinearFn fs, ft, fq;
	if (textured) {
		// rasterizer.cpp DrawRectangle: UV planes from three corners whose s, t and q come from the two
		// vertices component by component (gpu/probe exp57, exp122, exp129).
		float tc0s = v0.s, tc0t = v0.t, tc1s = v1.s, tc1t = v1.t;
		if (st.throughMode) {
			const float iw = 1.0f / (float)(1 << st.samp.width0Shift), ih = 1.0f / (float)(1 << st.samp.height0Shift);
			tc0s *= iw;
			tc1s *= iw;
			tc0t *= ih;
			tc1t *= ih;
		}
		const bool right = v0.x < v1.x, down = v0.y < v1.y;
		const bool swapST = right != down;
		const RVertex *vv[2] = { &v0, &v1 };
		double S[2], T[2], Q[2];
		for (int k = 0; k < 2; ++k) {
			const double r = st.throughMode ? 1.0 : GERecip(TruncateToFloat24(vv[k]->clipw));
			Q[k] = r;
			S[k] = (double)(k == 0 ? tc0s : tc1s) * r;
			T[k] = (double)(k == 0 ? tc0t : tc1t) * r;
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
		fs = PlaneThrough(CX, CY, cs);
		ft = PlaneThrough(CX, CY, ct);
		fq = PlaneThrough(CX, CY, cq);
		// rasterizer.cpp UVPlaneGradient: texels per pixel.
		const double gs = std::max(std::abs(fs.b), std::abs(fs.c)) * SUBPIX * (double)(1 << st.samp.width0Shift);
		const double gt = std::max(std::abs(ft.b), std::abs(ft.c)) * SUBPIX * (double)(1 << st.samp.height0Shift);
		base.lod = (float)(std::max(gs, gt) / scale_);
	}
	const int xs[2] = { px0, px1 + 1 }, ys[2] = { py0, py1 + 1 };
	GpuVertex c[4];
	for (int k = 0; k < 4; ++k) {
		c[k] = base;
		c[k].x = (float)xs[k & 1];
		c[k].y = (float)ys[k >> 1];
		if (textured) {
			const double sx = (double)xs[k & 1] * SUBPIX, sy = (double)ys[k >> 1] * SUBPIX;
			c[k].s = (float)fs.At(sx, sy);
			c[k].t = (float)ft.At(sx, sy);
			c[k].q = (float)fq.At(sx, sy);
		}
	}
	verts_.push_back(c[0]);
	verts_.push_back(c[1]);
	verts_.push_back(c[2]);
	verts_.push_back(c[2]);
	verts_.push_back(c[1]);
	verts_.push_back(c[3]);
}

void DrawData::EmitPoint(const BatchPrim &p, const RasterState &st) {
	const RVertex &v = p.v[0];
	const int px = v.x / SUBPIX, py = v.y / SUBPIX;
	GpuVertex base{};
	base.z = (float)v.z;
	base.fog = st.pix.applyFog ? ClampFogDepth(v.fog) : 255.0f;
	base.c0 = v.color0;
	base.c1 = v.color1 & 0x00FFFFFF;
	base.q = 1.0f;
	base.state = p.stateIndex;
	base.lod = -1e-30f;
	if (st.enableTextures && !st.pix.clearMode) {
		if (st.throughMode) {
			base.s = GETruncateTexCoord(v.s) * (1.0f / (float)(1 << st.samp.width0Shift));
			base.t = GETruncateTexCoord(v.t) * (1.0f / (float)(1 << st.samp.height0Shift));
		} else {
			// The level's q is 1/w (rasterizer.cpp DrawPoint).
			const double r = v.clipw != 0.0f ? 1.0 / v.clipw : 1.0;
			base.s = (float)(v.s * r);
			base.t = (float)(v.t * r);
			base.q = (float)((st.textureProj ? v.q : 1.0f) * r);
		}
	}
	GpuVertex c[4];
	for (int k = 0; k < 4; ++k) {
		c[k] = base;
		c[k].x = (float)(px + (k & 1));
		c[k].y = (float)(py + (k >> 1));
	}
	verts_.push_back(c[0]);
	verts_.push_back(c[1]);
	verts_.push_back(c[2]);
	verts_.push_back(c[2]);
	verts_.push_back(c[1]);
	verts_.push_back(c[3]);
}

void DrawData::EmitLine(const BatchPrim &p, const RasterState &st) {
	const bool textured = st.enableTextures && !st.pix.clearMode;
	const RVertex &v0 = p.v[0], &v1 = p.v[1];
	const bool interpolate = st.shadeGouraud && !(v0.color0 == v1.color0 && v0.color1 == v1.color1);
	const bool proj = st.textureProj && !st.throughMode;
	for (int k = 0; k < 2; ++k) {
		const RVertex &v = p.v[k];
		GpuVertex g;
		g.x = (float)v.x * (1.0f / SUBPIX);
		g.y = (float)v.y * (1.0f / SUBPIX);
		g.z = (float)v.z;
		g.fog = st.pix.applyFog ? ClampFogDepth(v.fog) : 255.0f;
		g.c0 = interpolate ? v.color0 : v1.color0;
		g.c1 = interpolate ? v.color1 : v1.color1;
		g.s = g.t = 0.0f;
		g.q = 1.0f;
		if (textured) {
			if (st.throughMode) {
				g.s = v.s * (1.0f / (float)(1 << st.samp.width0Shift));
				g.t = v.t * (1.0f / (float)(1 << st.samp.height0Shift));
			} else {
				TexCoordsPerspective(v, proj, g);
			}
		}
		g.state = p.stateIndex;
		g.lod = 0.0f;
		verts_.push_back(g);
	}
	if (textured) {
		// The texel step along the line (rasterizer.cpp DrawLine's ds, dt).
		GpuVertex *a = &verts_[verts_.size() - 2], *b = a + 1;
		const float steps = std::max(1.0f, std::max(std::abs(b->x - a->x), std::abs(b->y - a->y)));
		const float ua = a->q != 0.0f ? a->s / a->q : 0.0f, ub = b->q != 0.0f ? b->s / b->q : 0.0f;
		const float va = a->q != 0.0f ? a->t / a->q : 0.0f, vb = b->q != 0.0f ? b->t / b->q : 0.0f;
		const float grad = std::max(std::abs(ub - ua) * (float)(1 << st.samp.width0Shift), std::abs(vb - va) * (float)(1 << st.samp.height0Shift)) / steps;
		a->lod = b->lod = -std::max(grad / (float)scale_, 1e-30f);
	}
}

void DrawData::BuildVertices(const BatchPrim *prims, size_t n, const RasterState *states, const GeometryBatch *geo) {
	verts_.clear();
	segs_.clear();
	for (size_t i = 0; i < n; ++i) {
		const BatchPrim &p = prims[i];
		const RasterState &st = states[p.stateIndex];
		auto compatible = [&] { return p.stateIndex == segs_.back().lastState || Compatible(states[segs_.back().state], st); };
		if (p.type == PrimType::Geometry) {
			// The jobs' vertices follow each other in the GPU's buffer.
			const u32 first = geo->jobs[GeoFirstJob(p)].out, count = GeoJobCount(p) * GEO_JOB_VERTS;
			if (segs_.empty() || !segs_.back().gpu || segs_.back().first + segs_.back().count != first || !compatible())
				segs_.push_back({ first, 0, p.stateIndex, p.stateIndex, false, true });
			segs_.back().lastState = p.stateIndex;
			segs_.back().count += count;
			continue;
		}
		const bool lines = p.type == PrimType::Line;
		if (segs_.empty() || segs_.back().gpu || segs_.back().lines != lines || !compatible())
			segs_.push_back({ (u32)verts_.size(), 0, p.stateIndex, p.stateIndex, lines });
		Segment &seg = segs_.back();
		seg.lastState = p.stateIndex;
		const size_t before = verts_.size();
		switch (p.type) {
		case PrimType::Triangle: EmitTriangle(p, st); break;
		case PrimType::Rect: EmitRect(p, st, false); break;
		case PrimType::ClearRect: EmitRect(p, st, true); break;
		case PrimType::Point: EmitPoint(p, st); break;
		case PrimType::Line: EmitLine(p, st); break;
		case PrimType::Geometry: break;
		}
		seg.count += (u32)(verts_.size() - before);
	}
}

void DrawData::FillStates(const RasterState *states, size_t n) {
	gpuStates_.resize(n);
	for (size_t i = 0; i < n; ++i) {
		const RasterState &rs = states[i];
		const PixelState &ps = rs.pix;
		DrawStateGpu &d = gpuStates_[i];
		memset(&d, 0, sizeof(d));
		u32 f = 0;
		if (ps.alphaBlend) f |= F_ALPHA_BLEND;
		if (ps.applyLogicOp) f |= F_LOGIC_OP;
		if (ps.applyFog) f |= F_FOG;
		if (ps.colorTest) f |= F_COLOR_TEST;
		if (ps.stencilTest) f |= F_STENCIL_TEST;
		if (ps.applyDepthRange) f |= F_DEPTH_RANGE;
		if (ps.dithering) f |= F_DITHER;
		if (ps.applyColorWriteMask) f |= F_WRITE_MASK;
		if (ps.hasAlphaTestMask) f |= F_ALPHA_MASK;
		if (ps.hasStencilTestMask) f |= F_STENCIL_MASK;
		if (ps.depthWrite) f |= F_DEPTH_WRITE;
		if (ps.colorClear) f |= F_COLOR_CLEAR;
		if (ps.stencilClear) f |= F_STENCIL_CLEAR;
		if (ps.depthClear) f |= F_DEPTH_CLEAR;
		const bool textured = rs.enableTextures && !ps.clearMode;
		if (textured && rs.samp.useColorDoubling) f |= F_DBL_SEC;
		d.flags = f;
		d.alphaTest = (u32)ps.alphaTestFunc | ((u32)ps.alphaTestRef << 8) | ((u32)ps.alphaTestMask << 16);
		d.colorTestFunc = ps.colorTestFunc;
		d.colorTestRef = ps.colorTestRef;
		d.colorTestMask = ps.colorTestMask;
		d.stencilTest = (u32)ps.stencilTestFunc | ((u32)ps.stencilTestRef << 8) | ((u32)ps.stencilRef << 16) | ((u32)ps.stencilTestMask << 24);
		d.stencilOps = (u32)ps.sFail | ((u32)ps.zFail << 4) | ((u32)ps.zPass << 8);
		d.depthFunc = ps.depthTestFunc;
		d.minz = (u32)ps.minz;
		d.maxz = (u32)ps.maxz;
		d.blend = (u32)ps.blendEq | ((u32)ps.blendSrc << 4) | ((u32)ps.blendDst << 8);
		d.fixA = ps.fixA;
		d.fixB = ps.fixB;
		d.fogColor = ps.fogColor;
		d.writeMask = ps.colorWriteMask;
		d.logicOp = ps.logicOp;
		if (ps.dithering) {
			for (int k = 0; k < 16; ++k) {
				const u32 nib = (u32)(u8)ps.ditherMatrix[k] & 0xF;
				if (k < 8)
					d.dither0 |= nib << (k * 4);
				else
					d.dither1 |= nib << ((k - 8) * 4);
			}
		}
		if (textured) {
			const SamplerState &ss = rs.samp;
			d.texFunc = (u32)ss.texFunc | (ss.useTextureAlpha ? 0x10u : 0u) | (ss.useColorDoubling ? 0x20u : 0u);
			d.texEnv = ss.texBlendColor;
			// Texture filtering as the game sets it, or always nearest / linear.
			u32 filt = (ss.minFilt ? 4u : 0u) | (ss.magFilt ? 8u : 0u);
			if (texFilter_ == 1)
				filt = 0;
			else if (texFilter_ == 2)
				filt = 12;
			d.texMode = (ss.clampS ? 1u : 0u) | (ss.clampT ? 2u : 0u) | filt | (ss.mipFilt ? 0x10u : 0u) | ((u32)(ss.maxLevel & 7) << 5) |
				((u32)(ss.texLevelMode & 3) << 8);
			d.texLevelOffset = ss.texLevelOffset;
			d.lodSlopeLog = GELog16(ss.lodSlope);
			int aw, ah;
			TextureAtlasLayout(ss, d.texLevels, &aw, &ah);
			// Texture-cache atlases are sampled at their upscale factor; the backend may replace this
			// layout when resolving render-to-texture.
			const int up = TextureUpscale(texUpscale_, aw, ah);
			if (up > 1) {
				for (u32 &lv : d.texLevels)
					lv = (lv & 0xFFFF0000u) | ((lv & 0xFFFFu) * (u32)up);
				d.rtt = (u32)up << 12;
			}
		}
	}
}


}  // namespace gpu::hw
