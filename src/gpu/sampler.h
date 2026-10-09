// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Texture sampling: texel fetch for every format (swizzled, CLUT, DXT), wrap/clamp, nearest and bilinear
// filtering, mip level selection, and the texture functions (GPU/Software/Sampler.cpp).
#pragma once

#include <algorithm>
#include <climits>

#include "gpu/color.h"
#include "gpu/gemath.h"
#include "gpu/render.h"

namespace gpu {

PSPC_FORCEINLINE u16 Rd16(const u8 *p) {
	u16 v;
	memcpy(&v, p, 2);
	return v;
}

PSPC_FORCEINLINE u32 Rd32(const u8 *p) {
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

// Texture bits per texel, by format.
constexpr u8 kTexelBits[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };

PSPC_FORCEINLINE u32 TexRowOffset(int bits, bool swizzled, int bufw, int v) {
	if (!swizzled)
		return (u32)v * (u32)(bufw * bits >> 3);
	// Blocks of 16 bytes by 8 rows.
	return (u32)(v >> 3) * (u32)((bufw * bits / 32) * 32) + (u32)(v & 7) * 16;
}

PSPC_FORCEINLINE u32 TexColumnOffset(int bits, bool swizzled, int u) {
	const u32 b = (u32)(u * bits) >> 3;
	return swizzled ? (b >> 4) * 128 + (b & 15) : b;
}

PSPC_FORCEINLINE u32 LookupClut(const SamplerState &ss, u32 index, int level) {
	if (!ss.clutSimple) {
		const u32 offsetMask = ss.clutfmt == GE_CMODE_32BIT_ABGR8888 ? 0xFF : 0x1FF;
		index = ((index >> ss.clutShift) & ss.clutMask) | (ss.clutOffset & offsetMask);
	} else {
		index &= 0xFF;
	}
	if (!ss.useSharedClut)
		index += ss.texfmt == GE_TFMT_CLUT4 ? level * 16 : (level & 1) * 256;
	// The CLUT snapshot is 2 KB: 1 KB of palette, repeated.
	switch (ss.clutfmt) {
	case GE_CMODE_16BIT_BGR5650: return RGB565ToRGBA8888(Rd16(ss.clut + 2 * (index & 0x3FF)));
	case GE_CMODE_16BIT_ABGR5551: return RGBA5551ToRGBA8888(Rd16(ss.clut + 2 * (index & 0x3FF)));
	case GE_CMODE_16BIT_ABGR4444: return RGBA4444ToRGBA8888(Rd16(ss.clut + 2 * (index & 0x3FF)));
	default: return Rd32(ss.clut + 4 * (index & 0x1FF));
	}
}

static inline u32 MakeCol(int r, int g, int b, int a) {
	return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

static inline int Mix23(int c1, int c2) {
	return (c1 + c1 + c2) / 3;
}

// A DXT block's color (8 bytes: 4 lines of 2-bit indices, then the two 565 colors).
static inline u32 DXTTexelColor(const u8 *block, int x, int y, int alpha) {
	const u16 c1 = Rd16(block + 4), c2 = Rd16(block + 6);
	const int blue1 = (c1 << 3) & 0xF8, blue2 = (c2 << 3) & 0xF8;
	const int green1 = (c1 >> 3) & 0xFC, green2 = (c2 >> 3) & 0xFC;
	const int red1 = (c1 >> 8) & 0xF8, red2 = (c2 >> 8) & 0xF8;
	const int colorIndex = (block[y] >> (x * 2)) & 3;
	if (colorIndex == 0)
		return MakeCol(red1, green1, blue1, alpha);
	if (colorIndex == 1)
		return MakeCol(red2, green2, blue2, alpha);
	if (c1 > c2) {
		if (colorIndex == 2)
			return MakeCol(Mix23(red1, red2), Mix23(green1, green2), Mix23(blue1, blue2), alpha);
		return MakeCol(Mix23(red2, red1), Mix23(green2, green1), Mix23(blue2, blue1), alpha);
	}
	if (colorIndex == 3)
		return MakeCol(0, 0, 0, 0);
	return MakeCol((red1 + red2) / 2, (green1 + green2) / 2, (blue1 + blue2) / 2, alpha);
}

static inline u8 DXT5Lerp8(int a1, int a2, int n) {
	return (u8)(((a1 * ((7 - n) << 8)) / 7 + (a2 * (n << 8)) / 7 + 31) >> 8);
}

static inline u8 DXT5Lerp6(int a1, int a2, int n) {
	return (u8)(((a1 * ((5 - n) << 8)) / 5 + (a2 * (n << 8)) / 5 + 31) >> 8);
}

static inline u32 DXTTexel(int fmt, const u8 *src, int bufw, int u, int v) {
	const int blockSize = fmt == GE_TFMT_DXT1 ? 8 : 16;
	const u8 *block = src + ((size_t)(v >> 2) * (bufw >> 2) + (u >> 2)) * blockSize;
	const int x = u & 3, y = v & 3;
	if (fmt == GE_TFMT_DXT1)
		return DXTTexelColor(block, x, y, 255);
	const u32 color = DXTTexelColor(block, x, y, 0);
	if (fmt == GE_TFMT_DXT3) {
		const u32 alpha = (Rd16(block + 8 + 2 * y) >> (x * 4)) & 0xF;
		return color | (alpha << 28);
	}
	// DXT5: color, then alphadata2 (u32), alphadata1 (u16), alpha1, alpha2.
	const u64 alphadata = ((u64)Rd16(block + 12) << 32) | Rd32(block + 8);
	const int alpha1 = block[14], alpha2 = block[15];
	const int alphaIndex = (int)((alphadata >> (y * 12 + x * 3)) & 7);
	if (alphaIndex == 0)
		return color | ((u32)alpha1 << 24);
	if (alphaIndex == 1)
		return color | ((u32)alpha2 << 24);
	if (alpha1 > alpha2)
		return color | ((u32)DXT5Lerp8(alpha1, alpha2, alphaIndex - 1) << 24);
	if (alphaIndex == 6)
		return color;
	if (alphaIndex == 7)
		return color | 0xFF000000;
	return color | ((u32)DXT5Lerp6(alpha1, alpha2, alphaIndex - 1) << 24);
}

// The texel at (u, v), already wrapped/clamped, of level `level`.
template <int fmt, bool swz>
PSPC_FORCEINLINE u32 FetchTexelT(const SamplerState &ss, int level, int u, int v) {
	const TexLevel &lv = ss.levels[level];
	const u8 *base = lv.ptr;
	if (!base)
		return 0;
	if constexpr (fmt >= GE_TFMT_DXT1) {
		if (lv.bounded) {
			const u32 off = ((u32)(v >> 2) * (lv.bufw >> 2) + (u >> 2)) * (fmt == GE_TFMT_DXT1 ? 8 : 16);
			if (off + 16 > lv.validBytes)
				return 0;
		}
		return DXTTexel(fmt, base, lv.bufw, u, v);
	} else {
		constexpr int bits = kTexelBits[fmt];
		const u32 off = TexRowOffset(bits, swz, lv.bufw, v) + TexColumnOffset(bits, swz, u);
		if (lv.bounded && off + 4 > lv.validBytes)
			return 0;
		const u8 *p = base + off;
		switch (fmt) {
		case GE_TFMT_5650: return RGB565ToRGBA8888(Rd16(p));
		case GE_TFMT_5551: return RGBA5551ToRGBA8888(Rd16(p));
		case GE_TFMT_4444: return RGBA4444ToRGBA8888(Rd16(p));
		case GE_TFMT_8888: return Rd32(p);
		case GE_TFMT_CLUT32: return LookupClut(ss, Rd32(p), level);
		case GE_TFMT_CLUT16: return LookupClut(ss, Rd16(p), level);
		case GE_TFMT_CLUT8: return LookupClut(ss, *p, level);
		case GE_TFMT_CLUT4: return LookupClut(ss, (u & 1) ? (*p >> 4) : (*p & 0xF), level);
		default: return 0;
		}
	}
}

// A texel coordinate in 1/16 texel, truncated; out of int range it's INT_MIN (gpu/probe exp5).
PSPC_FORCEINLINE int TexelFixed(float f) {
	return f >= -2147483648.0f && f < 2147483648.0f ? (int)f : INT_MIN;
}

PSPC_FORCEINLINE int ClampUV(int v, int size) {
	if (v >= size - 1)
		return size - 1;
	if (v >= 511)
		return 511;
	if (v < 0)
		return 0;
	return v;
}

PSPC_FORCEINLINE int WrapUV(int v, int size) {
	return v & (size - 1) & 511;
}

template <int fmt, bool swz>
PSPC_FORCEINLINE u32 SampleNearestLevelT(const SamplerState &ss, float s, float t, int level) {
	const TexLevel &lv = ss.levels[level];
	int u = TexelFixed(s * lv.w * 16.0f) >> 4;
	int v = TexelFixed(t * lv.h * 16.0f) >> 4;
	u = ss.clampS ? ClampUV(u, lv.w) : WrapUV(u, lv.w);
	v = ss.clampT ? ClampUV(v, lv.h) : WrapUV(v, lv.h);
	return FetchTexelT<fmt, swz>(ss, level, u, v);
}

PSPC_FORCEINLINE void TexelPair(float st, int size, bool clamp, int &c0, int &c1, int &frac) {
	int base = TexelFixed(st * size * 16.0f) - 8;
	frac = base & 0x0F;
	base >>= 4;
	if (clamp) {
		const int hi = size > 512 ? 511 : size - 1;
		c0 = std::max(std::min(base, hi), 0);
		c1 = std::max(std::min(base + 1, hi), 0);
	} else {
		const int mask = (size - 1) & 511;
		c0 = base & mask;
		c1 = (base + 1) & mask;
	}
}

PSPC_FORCEINLINE u64 Spread(u32 c) {
	u64 x = c;
	x = (x | (x << 16)) & 0x0000FFFF0000FFFFULL;
	return (x | (x << 8)) & 0x00FF00FF00FF00FFULL;
}

PSPC_FORCEINLINE u32 Unspread(u64 x) {
	x &= 0x00FF00FF00FF00FFULL;
	x = (x | (x >> 8)) & 0x0000FFFF0000FFFFULL;
	return (u32)(x | (x >> 16));
}

// (a * (16 - f) + b * f) >> 4 per channel.
PSPC_FORCEINLINE u64 LerpSpread(u64 a, u64 b, int f) {
	return ((a * (u64)(16 - f) + b * (u64)f) >> 4) & 0x00FF00FF00FF00FFULL;
}

template <int fmt, bool swz>
PSPC_FORCEINLINE u32 SampleLinearLevelT(const SamplerState &ss, float s, float t, int level) {
	const TexLevel &lv = ss.levels[level];
	int u0, u1, v0, v1, fu, fv;
	TexelPair(s, lv.w, ss.clampS, u0, u1, fu);
	TexelPair(t, lv.h, ss.clampT, v0, v1, fv);
	const u64 tl = Spread(FetchTexelT<fmt, swz>(ss, level, u0, v0));
	const u64 tr = Spread(FetchTexelT<fmt, swz>(ss, level, u1, v0));
	const u64 bl = Spread(FetchTexelT<fmt, swz>(ss, level, u0, v1));
	const u64 br = Spread(FetchTexelT<fmt, swz>(ss, level, u1, v1));
	// Horizontal lerps truncated to 8 bits, then the vertical one (gpu/probe exp52).
	return Unspread(LerpSpread(LerpSpread(tl, tr, fu), LerpSpread(bl, br, fu), fv));
}

// The texel color at a level (with the fraction toward level + 1).
template <int fmt, bool swz>
u32 SampleTextureT(const SamplerState &ss, float s, float t, int level, int levelFrac, bool linear) {
	if (!linear) {
		u32 c0 = SampleNearestLevelT<fmt, swz>(ss, s, t, level);
		if (levelFrac) {
			const u32 c1 = SampleNearestLevelT<fmt, swz>(ss, s, t, level + 1);
			c0 = Unspread(LerpSpread(Spread(c0), Spread(c1), levelFrac));
		}
		return c0;
	}
	u32 c0 = SampleLinearLevelT<fmt, swz>(ss, s, t, level);
	if (levelFrac) {
		const u32 c1 = SampleLinearLevelT<fmt, swz>(ss, s, t, level + 1);
		c0 = Unspread(LerpSpread(Spread(c0), Spread(c1), levelFrac));
	}
	return c0;
}

// The sampler for the state's format and swizzling (SamplerState::sample).
TexSampleFn GetTexSampleFn(int texfmt, bool swizzle);

PSPC_FORCEINLINE u32 SampleTexture(const SamplerState &ss, float s, float t, int level, int levelFrac, bool linear) {
	return ss.sample(ss, s, t, level, levelFrac, linear);
}

// The texture function. prim and tex are RGBA (0-255); out may exceed 255.
PSPC_FORCEINLINE void TextureFunction(const SamplerState &ss, u32 primc, u32 texc, int out[4]) {
	const int p[4] = { (int)(primc & 0xFF), (int)((primc >> 8) & 0xFF), (int)((primc >> 16) & 0xFF), (int)(primc >> 24) };
	const int t[4] = { (int)(texc & 0xFF), (int)((texc >> 8) & 0xFF), (int)((texc >> 16) & 0xFF), (int)(texc >> 24) };
	const bool rgba = ss.useTextureAlpha;
	const bool dbl = ss.useColorDoubling;
	switch (ss.texFunc) {
	case GE_TEXFUNC_MODULATE:
		for (int i = 0; i < 3; ++i)
			out[i] = ((p[i] + 1) * t[i] * (dbl ? 2 : 1)) / 256;
		out[3] = rgba ? ((p[3] + 1) * t[3] / 256) : p[3];
		break;
	case GE_TEXFUNC_DECAL:
		if (rgba) {
			const int a = t[3], inva = 255 - t[3];
			for (int i = 0; i < 3; ++i) {
				const int sum = (p[i] + 1) * inva + (t[i] + 1) * a;
				out[i] = dbl ? sum / 128 : sum / 256;
			}
		} else {
			for (int i = 0; i < 3; ++i)
				out[i] = dbl ? t[i] * 2 : t[i];
		}
		out[3] = p[3];
		break;
	case GE_TEXFUNC_BLEND: {
		const u32 env = ss.texBlendColor;
		for (int i = 0; i < 3; ++i) {
			const int sum = (255 - t[i]) * p[i] + t[i] * (int)((env >> (8 * i)) & 0xFF) + 255;
			out[i] = dbl ? sum / 128 : sum / 256;
		}
		out[3] = rgba ? ((p[3] + 1) * t[3] / 256) : p[3];
		break;
	}
	case GE_TEXFUNC_REPLACE:
		for (int i = 0; i < 3; ++i)
			out[i] = dbl ? t[i] * 2 : t[i];
		out[3] = rgba ? t[3] : p[3];
		break;
	default:
		for (int i = 0; i < 3; ++i)
			out[i] = dbl ? (p[i] + t[i]) * 2 : p[i] + t[i];
		out[3] = rgba ? ((p[3] + 1) * t[3] / 256) : p[3];
		break;
	}
}

// Mip level and filter (GPU/Software/Rasterizer.cpp CalculateSamplingParams). q is 1/w at the pixel;
// autoGrad the largest UV plane gradient in texels per pixel (negative when there are no planes).
PSPC_FORCEINLINE void CalculateSamplingParams(const SamplerState &ss, float ds, float dt, float q, float autoGrad, int &level, int &levelFrac, bool &filt) {
	const int width = 1 << ss.width0Shift;
	const int height = 1 << ss.height0Shift;
	int detail;
	switch (ss.texLevelMode) {
	case GE_TEXLEVEL_MODE_AUTO:
		if (autoGrad >= 0.0f)
			detail = GELog16(autoGrad) - GELog16(q);
		else
			detail = GELog16(std::max(std::abs(ds * width), std::abs(dt * height)));
		break;
	case GE_TEXLEVEL_MODE_SLOPE:
		detail = 16 + GELog16(ss.lodSlope) - GELog16(q);
		break;
	default:
		detail = 0;
		break;
	}
	detail += ss.texLevelOffset;
	if (detail > 0 && ss.maxLevel > 0) {
		int level8 = std::min(detail, ss.maxLevel * 16);
		if (!ss.mipFilt)
			level8 += 8;
		level = level8 >> 4;
		levelFrac = ss.mipFilt ? level8 & 0xF : 0;
		if (level >= ss.maxLevel) {
			level = ss.maxLevel;
			levelFrac = 0;
		}
	} else {
		level = 0;
		levelFrac = 0;
	}
	filt = detail > 0 ? ss.minFilt : ss.magFilt;
}

}  // namespace gpu
