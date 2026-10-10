// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// HLSL for the D3D11 backend, compiled at run time with D3DCompile.
//
// The GPU rasterizes screen-space primitives (no transform on the GPU: transform.cpp did it), and the
// pixel shader runs the GE's whole per-pixel pipeline (src/gpu/pixel.h DrawPixel, itself from PPSSPP's
// GPU/Software/DrawPixel.cpp) on rasterizer-ordered views of the target: depth range, alpha test, fog,
// color test, stencil and depth tests, every blend equation and factor, dithering, logic ops, write
// masks, all four framebuffer formats. The framebuffer and depth buffer are R32_UINT textures holding
// the raw PSP pixel values (16-bit formats in the low half), at the render scale. Per-draw state comes
// from a structured buffer indexed by a per-vertex state number, so draws that share a texture merge.
//
// Variants (#defines): FMT (GE_FORMAT_*), CLEAR (clear mode), TEX (texturing).
#include "gpu/hw/d3d11_internal.h"

namespace gpu::hw {

// Matches DrawStateGpu in d3d11_internal.h.
static const char kCommon[] = R"HLSL(
#define F_ALPHA_BLEND   0x0001
#define F_LOGIC_OP      0x0002
#define F_FOG           0x0004
#define F_COLOR_TEST    0x0008
#define F_STENCIL_TEST  0x0010
#define F_DEPTH_RANGE   0x0020
#define F_DITHER        0x0040
#define F_WRITE_MASK    0x0080
#define F_ALPHA_MASK    0x0100
#define F_STENCIL_MASK  0x0200
#define F_DEPTH_WRITE   0x0400
#define F_COLOR_CLEAR   0x0800
#define F_STENCIL_CLEAR 0x1000
#define F_DEPTH_CLEAR   0x2000
#define F_DBL_SEC       0x4000

struct DrawState {
	uint flags;
	uint alphaTest;      // func | ref << 8 | mask << 16
	uint colorTestFunc;
	uint colorTestRef;
	uint colorTestMask;
	uint stencilTest;    // func | testRef << 8 | replaceRef << 16 | testMask << 24
	uint stencilOps;     // sFail | zFail << 4 | zPass << 8
	uint depthFunc;
	uint minz;
	uint maxz;
	uint blend;          // eq | src << 4 | dst << 8
	uint fixA;
	uint fixB;
	uint fogColor;
	uint writeMask;      // raw framebuffer bits that are kept
	uint logicOp;
	uint texFunc;        // func | rgba << 4 | doubling << 5
	uint texEnv;
	uint dither0;        // rows 0-1, 4 bits per entry
	uint dither1;        // rows 2-3
	uint texMode;        // clampS | clampT << 1 | minFilt << 2 | magFilt << 3 | mipFilt << 4 | maxLevel << 5 | levelMode << 8
	int texLevelOffset;  // 1/16 levels
	int lodSlopeLog;     // GELog16(slope)
	uint rtt;            // render to texture: format | scale << 4 | 0x100 (texLevels[0]: x in the surface)
	uint texLevels[8];   // atlas x | log2(w) << 16 | log2(h) << 20
	uint rttY;           // render to texture: y in the surface (render-scale pixels)
	uint3 pad2;
};

cbuffer TargetCB : register(b0) {
	float2 ndcScale;
	float2 ndcOffset;
	uint scale;
	uint3 targetPad;
};

struct PSIn {
	float4 pos : SV_Position;
	noperspective float4 c0 : COLOR0;
	noperspective float3 c1 : COLOR1;
	noperspective float3 stq : TEXCOORD0;
	noperspective float2 zf : TEXCOORD1;
	nointerpolation uint state : TEXCOORD2;
	nointerpolation float lod : TEXCOORD3;
};
)HLSL";

static const char kVertex[] = R"HLSL(
struct VSIn {
	float2 pos : POSITION;
	float2 zf : TEXCOORD1;
	uint4 c0 : COLOR0;
	uint4 c1 : COLOR1;
	float3 stq : TEXCOORD0;
	uint state : TEXCOORD2;
	float lod : TEXCOORD3;
};

PSIn VSMain(VSIn i) {
	PSIn o;
	o.pos = float4(i.pos * ndcScale + ndcOffset, 0.0, 1.0);
	o.c0 = (float4)i.c0;
	o.c1 = (float3)i.c1.rgb;
	o.stq = i.stq;
	o.zf = i.zf;
	o.state = i.state;
	o.lod = i.lod;
	return o;
}
)HLSL";

static const char kPixel1[] = R"HLSL(
#ifndef FMT
#define FMT 3
#endif
#ifndef CLEAR
#define CLEAR 0
#endif
#ifndef TEX
#define TEX 0
#endif

StructuredBuffer<DrawState> states : register(t1);
// The texture's levels side by side, RGBA8888 as decoded by sampler.h.
Texture2D<uint4> tex : register(t0);
// Render to texture: the surface holding the texture (raw pixels at the render scale).
Texture2D<uint> rttTex : register(t2);
RasterizerOrderedTexture2D<uint> fbuf : register(u0);
RasterizerOrderedTexture2D<uint> zbuf : register(u1);

uint RGBA4444ToRGBA8888(uint src) {
	uint c = (src & 0x000F) | ((src & 0x00F0) << 4) | ((src & 0x0F00) << 8) | ((src & 0xF000) << 12);
	return c | (c << 4);
}
uint RGBA5551ToRGBA8888(uint src) {
	uint dark = ((src & 0x1F) << 3) | ((src & 0x3E0) << 6) | ((src & 0x7C00) << 9);
	uint full = dark | ((dark >> 5) & 0x070707);
	if (src >> 15)
		full |= 0xFF000000;
	return full;
}
uint RGB565ToRGBA8888(uint src) {
	uint dark_rb = ((src & 0x1F) << 3) | ((src & 0xF800) << 8);
	uint full_rb = dark_rb | ((dark_rb >> 5) & 0x070007);
	uint dark_g = (src & 0x7E0) << 5;
	uint full_g = dark_g | ((dark_g >> 6) & 0x300);
	return full_rb | full_g | 0xFF000000;
}
uint ExpandColor(uint raw) {
#if FMT == 0
	return RGB565ToRGBA8888(raw) & 0x00FFFFFF;
#elif FMT == 1
	return RGBA5551ToRGBA8888(raw);
#elif FMT == 2
	return RGBA4444ToRGBA8888(raw);
#else
	return raw;
#endif
}
uint PackColor(uint v) {
#if FMT == 0
	return ((v >> 3) & 0x1F) | ((v >> 5) & (0x3F << 5)) | ((v >> 8) & (0x1F << 11));
#elif FMT == 1
	return ((v >> 3) & 0x1F) | ((v >> 6) & (0x1F << 5)) | ((v >> 9) & (0x1F << 10)) | ((v >> 16) & 0x8000);
#elif FMT == 2
	uint c = v >> 4;
	return (c & 0x000F) | ((c >> 4) & 0x00F0) | ((c >> 8) & 0x0F00) | ((c >> 12) & 0xF000);
#else
	return v;
#endif
}
uint StencilOf(uint raw) {
#if FMT == 0
	return 0;
#elif FMT == 1
	return (raw & 0x8000) ? 0xFF : 0;
#elif FMT == 2
	uint n = (raw >> 12) & 0xF;
	return (n << 4) | n;
#else
	return raw >> 24;
#endif
}
// The stored pixel with only its stencil changed.
uint WriteStencilOnly(uint raw, uint writeMask, uint value) {
#if FMT == 0
	return raw;
#elif FMT == 1
	if ((writeMask & 0x8000) == 0)
		return (raw & ~0x8000u) | ((value & 0x80u) << 8);
	return raw;
#elif FMT == 2
	uint wm = writeMask | 0x0FFF;
	return ((raw & wm) | ((value << 8) & ~wm)) & 0xFFFF;
#else
	uint wm = writeMask | 0x00FFFFFF;
	return (raw & wm) | ((value << 24) & ~wm);
#endif
}
bool CompareTest(uint func, int v, int ref) {
	switch (func) {
	case 0: return false;
	case 1: return true;
	case 2: return v == ref;
	case 3: return v != ref;
	case 4: return v < ref;
	case 5: return v <= ref;
	case 6: return v > ref;
	default: return v >= ref;
	}
}
uint ApplyStencilOp(uint op, uint replace, uint old) {
	switch (op) {
	case 0: return old;
	case 1: return 0;
	case 2: return replace;
	case 3: return (~old) & 0xFF;
	case 4:
#if FMT == 3
		return old != 0xFF ? old + 1 : old;
#elif FMT == 1
		return 0xFF;
#elif FMT == 2
		return old < 0xF0 ? old + 0x10 : old;
#else
		return old;
#endif
	case 5:
#if FMT == 2
		return old >= 0x10 ? old - 0x10 : old;
#elif FMT == 1
		return 0;
#else
		return old != 0 ? old - 1 : old;
#endif
	}
	return old;
}
uint ApplyLogicOp(uint op, uint oldc, uint newc) {
	switch (op) {
	case 0: newc &= 0xFF000000; break;
	case 1: newc = newc & (oldc | 0xFF000000); break;
	case 2: newc = newc & (~oldc | 0xFF000000); break;
	case 3: break;
	case 4: newc = (~newc & (oldc & 0x00FFFFFF)) | (newc & 0xFF000000); break;
	case 5: newc = (oldc & 0x00FFFFFF) | (newc & 0xFF000000); break;
	case 6: newc = newc ^ (oldc & 0x00FFFFFF); break;
	case 7: newc = newc | (oldc & 0x00FFFFFF); break;
	case 8: newc = (~(newc | oldc) & 0x00FFFFFF) | (newc & 0xFF000000); break;
	case 9: newc = (~(newc ^ oldc) & 0x00FFFFFF) | (newc & 0xFF000000); break;
	case 10: newc = (~oldc & 0x00FFFFFF) | (newc & 0xFF000000); break;
	case 11: newc = newc | (~oldc & 0x00FFFFFF); break;
	case 12: newc = (~newc & 0x00FFFFFF) | (newc & 0xFF000000); break;
	case 13: newc = ((~newc | oldc) & 0x00FFFFFF) | (newc & 0xFF000000); break;
	case 14: newc = (~(newc & oldc) & 0x00FFFFFF) | (newc & 0xFF000000); break;
	default: newc |= 0x00FFFFFF; break;
	}
	return newc;
}
int3 Unpack3(uint c) {
	return int3(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
}
int3 BlendFactor(uint factor, bool isSrc, int4 src, int4 dst, uint fix) {
	int4 other = isSrc ? dst : src;
	switch (factor) {
	case 0: return other.rgb;
	case 1: return 255 - other.rgb;
	case 2: return src.aaa;
	case 3: return 255 - src.aaa;
	case 4: return dst.aaa;
	case 5: return 255 - dst.aaa;
	case 6: return 2 * src.aaa;
	case 7: return 255 - 2 * src.aaa;
	case 8: return 2 * dst.aaa;
	case 9: return 255 - 2 * dst.aaa;
	case 11: return int3(0, 0, 0);
	case 12: return int3(255, 255, 255);
	default: return Unpack3(fix);
	}
}
int3 AlphaBlend(DrawState st, int4 src, int4 dst) {
	uint eq = st.blend & 0xF;
	if (eq == 3)
		return min(src.rgb, dst.rgb);
	if (eq == 4)
		return max(src.rgb, dst.rgb);
	if (eq >= 5)
		return abs(src.rgb - dst.rgb);
	uint fs = (st.blend >> 4) & 0xF, fd = (st.blend >> 8) & 0xF;
	int3 sf = BlendFactor(fs, true, src, dst, st.fixA);
	int3 df = BlendFactor(fd, false, src, dst, st.fixB);
	// 255 - 2a goes negative for a >= 128; the term subtracts then (gpu/probe exp116).
	int3 ts = ((2 * src.rgb + 1) * (2 * abs(sf) + 1)) >> 10;
	int3 td = ((2 * dst.rgb + 1) * (2 * abs(df) + 1)) >> 10;
	int3 s = sf < 0 ? -ts : ts;
	int3 d = df < 0 ? -td : td;
	if (eq == 0)
		return s + d;
	if (eq == 1)
		return s - d;
	return d - s;
}
)HLSL";

static const char kPixel2[] = R"HLSL(
#if TEX
// Texture sampling as sampler.h does it: texel coordinates in 1/16 texel, wrapping and clamping inside
// 512 texels, bilinear filtering with 4-bit fractions truncated (horizontal first), mip levels blended
// with a 4-bit fraction, and the level from rasterizer.cpp's CalculateSamplingParams.
int GELog16(float v) {
	return (int)((asuint(v) >> 19) & 0x0FFF) - 127 * 16;
}
int TexelFixed(float f) {
	return f >= -2147483648.0 && f < 2147483648.0 ? (int)f : (int)0x80000000;
}
// limit: the last texel the GE reaches (511, times the render scale for render to texture).
int ClampUV(int v, int size, int limit) {
	if (v >= size - 1)
		return min(size - 1, limit);
	if (v >= limit)
		return limit;
	return max(v, 0);
}
int WrapUV(int v, int size, int limit) {
	int period = min(size, limit + 1);
	return ((v % period) + period) % period;
}
uint4 TexelFromRaw(uint raw, uint fmt) {
	uint c = fmt == 0 ? RGB565ToRGBA8888(raw) : fmt == 1 ? RGBA5551ToRGBA8888(raw) : fmt == 2 ? RGBA4444ToRGBA8888(raw) : raw;
	return uint4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24);
}
uint4 Fetch(DrawState st, uint info, int u, int v) {
	if (st.rtt & 0x100)
		return TexelFromRaw(rttTex.Load(int3((int)(info & 0xFFFF) + u, (int)st.rttY + v, 0)), st.rtt & 3);
	return tex.Load(int3((int)(info & 0xFFFF) + u, v, 0));
}
uint4 Lerp16(uint4 a, uint4 b, int f) {
	return (a * (uint)(16 - f) + b * (uint)f) >> 4;
}
void TexelPair(float st, int size, bool clampIt, int limit, out int c0, out int c1, out int frac) {
	int base = TexelFixed(st * (float)size * 16.0) - 8;
	frac = base & 0x0F;
	base >>= 4;
	if (clampIt) {
		int hi = size > limit + 1 ? limit : size - 1;
		c0 = max(min(base, hi), 0);
		c1 = max(min(base + 1, hi), 0);
	} else {
		c0 = WrapUV(base, size, limit);
		c1 = WrapUV(base + 1, size, limit);
	}
}
uint4 SampleLevel(DrawState st, float s, float t, int level, bool bilinear) {
	uint info = st.texLevels[level];
	// A rendered texture is sampled at the render scale.
	int texScale = (st.rtt & 0x100) ? (int)((st.rtt >> 4) & 0xF) : 1;
	int w = (1 << ((info >> 16) & 0xF)) * texScale, h = (1 << ((info >> 20) & 0xF)) * texScale;
	int limit = 512 * texScale - 1;
	bool clampS = (st.texMode & 1) != 0, clampT = (st.texMode & 2) != 0;
	if (!bilinear) {
		int u = TexelFixed(s * (float)w * 16.0) >> 4;
		int v = TexelFixed(t * (float)h * 16.0) >> 4;
		u = clampS ? ClampUV(u, w, limit) : WrapUV(u, w, limit);
		v = clampT ? ClampUV(v, h, limit) : WrapUV(v, h, limit);
		return Fetch(st, info, u, v);
	}
	int u0, u1, v0, v1, fu, fv;
	TexelPair(s, w, clampS, limit, u0, u1, fu);
	TexelPair(t, h, clampT, limit, v0, v1, fv);
	uint4 tl = Fetch(st, info, u0, v0), tr = Fetch(st, info, u1, v0);
	uint4 bl = Fetch(st, info, u0, v1), br = Fetch(st, info, u1, v1);
	return Lerp16(Lerp16(tl, tr, fu), Lerp16(bl, br, fu), fv);
}
uint4 SampleTexture(DrawState st, float s, float t, int level, int levelFrac, bool bilinear) {
	uint4 c0 = SampleLevel(st, s, t, level, bilinear);
	if (levelFrac != 0)
		c0 = Lerp16(c0, SampleLevel(st, s, t, level + 1, bilinear), levelFrac);
	return c0;
}
// lod: the primitive's largest UV gradient in texels per pixel, divided by q at the pixel; negative:
// the gradient itself (lines, points).
void SamplingParams(DrawState st, float q, float lod, out int level, out int levelFrac, out bool filt) {
	uint mode = (st.texMode >> 8) & 3;
	int maxLevel = (int)((st.texMode >> 5) & 7);
	bool mipFilt = (st.texMode & 0x10) != 0;
	int detail = 0;
	if (mode == 0)
		detail = lod >= 0.0 ? GELog16(lod) - GELog16(q) : GELog16(lod);  // GELog16 ignores the sign
	else if (mode == 2)
		detail = 16 + st.lodSlopeLog - GELog16(q);
	detail += st.texLevelOffset;
	level = 0;
	levelFrac = 0;
	if (detail > 0 && maxLevel > 0) {
		int level8 = min(detail, maxLevel * 16);
		if (!mipFilt)
			level8 += 8;
		level = level8 >> 4;
		levelFrac = mipFilt ? (level8 & 0xF) : 0;
		if (level >= maxLevel) {
			level = maxLevel;
			levelFrac = 0;
		}
	}
	filt = detail > 0 ? (st.texMode & 4) != 0 : (st.texMode & 8) != 0;
}

// The texture function (sampler.h TextureFunction). prim and t are 0-255.
int4 TextureFunction(DrawState st, int4 p, int4 t) {
	uint func = st.texFunc & 7;
	bool rgba = (st.texFunc & 0x10) != 0;
	bool dbl = (st.texFunc & 0x20) != 0;
	int4 o;
	if (func == 0) {
		o.rgb = ((p.rgb + 1) * t.rgb * (dbl ? 2 : 1)) / 256;
		o.a = rgba ? ((p.a + 1) * t.a / 256) : p.a;
	} else if (func == 1) {
		if (rgba) {
			int a = t.a, inva = 255 - t.a;
			int3 sum = (p.rgb + 1) * inva + (t.rgb + 1) * a;
			o.rgb = dbl ? sum / 128 : sum / 256;
		} else {
			o.rgb = dbl ? t.rgb * 2 : t.rgb;
		}
		o.a = p.a;
	} else if (func == 2) {
		int3 env = Unpack3(st.texEnv);
		int3 sum = (255 - t.rgb) * p.rgb + t.rgb * env + 255;
		o.rgb = dbl ? sum / 128 : sum / 256;
		o.a = rgba ? ((p.a + 1) * t.a / 256) : p.a;
	} else if (func == 3) {
		o.rgb = dbl ? t.rgb * 2 : t.rgb;
		o.a = rgba ? t.a : p.a;
	} else {
		o.rgb = dbl ? (p.rgb + t.rgb) * 2 : p.rgb + t.rgb;
		o.a = rgba ? ((p.a + 1) * t.a / 256) : p.a;
	}
	return o;
}
#endif

void PSMain(PSIn i) {
	DrawState st = states[i.state];
	uint2 hp = uint2(i.pos.xy);
	uint2 pp = hp / scale;
	int z = clamp((int)floor(i.zf.x + 0.002), 0, 65535);
	int4 prim = clamp((int4)floor(i.c0 + 0.002), 0, 255);
	int4 c;
#if CLEAR
	c = prim;
#else
	int3 sec = clamp((int3)floor(i.c1 + 0.002), 0, 255);
	if (st.flags & F_DBL_SEC)
		sec *= 2;
#if TEX
	float q = i.stq.z;
	// Where q isn't positive the GE samples the last texel (gpu/texmtx/negq).
	float2 uv = q > 0.0 ? i.stq.xy / q : float2(0.99999994, 0.99999994);
	int level, levelFrac;
	bool filt;
	SamplingParams(st, q, i.lod, level, levelFrac, filt);
	int4 t = (int4)SampleTexture(st, uv.x, uv.y, level, levelFrac, filt);
	int4 o = TextureFunction(st, prim, t);
	o.rgb += sec;
	c = clamp(o, 0, 255);
#else
	c = clamp(int4(prim.rgb + sec, prim.a), 0, 255);
#endif
#endif

	if ((st.flags & F_DEPTH_RANGE) && (z < (int)st.minz || z > (int)st.maxz))
		return;
#if !CLEAR
	uint atf = st.alphaTest & 0xFF;
	if (atf != 1) {
		int a = (st.flags & F_ALPHA_MASK) ? (c.a & (int)((st.alphaTest >> 16) & 0xFF)) : c.a;
		if (!CompareTest(atf, a, (int)((st.alphaTest >> 8) & 0xFF)))
			return;
	}
	if (st.flags & F_FOG) {
		// Like the BLEND texfunc, always rounds up.
		int fog = clamp((int)floor(i.zf.y + 0.002), 0, 255);
		c.rgb = (c.rgb * fog + Unpack3(st.fogColor) * (255 - fog) + 255) / 256;
	}
	if (st.flags & F_COLOR_TEST) {
		uint rgb = ((uint)c.r | ((uint)c.g << 8) | ((uint)c.b << 16)) & st.colorTestMask;
		if (st.colorTestFunc == 0)
			return;
		if (st.colorTestFunc == 2 && rgb != st.colorTestRef)
			return;
		if (st.colorTestFunc == 3 && rgb == st.colorTestRef)
			return;
	}
#endif

	uint targetWriteMask = (st.flags & F_WRITE_MASK) ? st.writeMask : 0;
	uint oldRaw = fbuf[hp];
#if CLEAR
	uint stencil = (uint)c.a;
	if (st.flags & F_DEPTH_CLEAR)
		zbuf[hp] = (uint)z;
#else
	uint stencil = StencilOf(oldRaw);
	uint depthFunc = st.depthFunc;
	if (st.flags & F_STENCIL_TEST) {
		uint sfunc = st.stencilTest & 0xFF;
		uint testRef = (st.stencilTest >> 8) & 0xFF;
		bool hasMask = (st.flags & F_STENCIL_MASK) != 0;
		uint replace = hasMask ? ((st.stencilTest >> 16) & 0xFF) : testRef;
		uint sv = hasMask ? (stencil & ((st.stencilTest >> 24) & 0xFF)) : stencil;
		// ref func stencil
		if (!CompareTest(sfunc, (int)testRef, (int)sv)) {
			fbuf[hp] = WriteStencilOnly(oldRaw, targetWriteMask, ApplyStencilOp(st.stencilOps & 0xF, replace, stencil));
			return;
		}
		if (depthFunc != 1 && !CompareTest(depthFunc, z, (int)(zbuf[hp] & 0xFFFF))) {
			fbuf[hp] = WriteStencilOnly(oldRaw, targetWriteMask, ApplyStencilOp((st.stencilOps >> 4) & 0xF, replace, stencil));
			return;
		}
		stencil = ApplyStencilOp((st.stencilOps >> 8) & 0xF, replace, stencil);
	} else if (depthFunc != 1) {
		if (!CompareTest(depthFunc, z, (int)(zbuf[hp] & 0xFFFF)))
			return;
	}
	if (st.flags & F_DEPTH_WRITE)
		zbuf[hp] = (uint)z;
#endif

	uint oldColor = ExpandColor(oldRaw);
	int dith = 0;
	if (st.flags & F_DITHER) {
		uint idx = (pp.y & 3) * 4 + (pp.x & 3);
		uint word = idx < 8 ? st.dither0 : st.dither1;
		uint raw4 = (word >> ((idx & 7) * 4)) & 0xF;
		dith = (int)(raw4 << 28) >> 28;
	}
	int3 rgb = c.rgb;
#if !CLEAR
	if (st.flags & F_ALPHA_BLEND) {
		int4 dst = int4(Unpack3(oldColor), (int)(oldColor >> 24));
		rgb = AlphaBlend(st, c, dst);
	}
#endif
	uint3 q3 = (uint3)clamp(rgb + dith, 0, 255);
	uint newColor = q3.r | (q3.g << 8) | (q3.b << 16) | (stencil << 24);
#if CLEAR
	if (!(st.flags & F_COLOR_CLEAR))
		newColor = (newColor & 0xFF000000) | (oldColor & 0x00FFFFFF);
	if (!(st.flags & F_STENCIL_CLEAR))
		newColor = (newColor & 0x00FFFFFF) | (oldColor & 0xFF000000);
#else
	if (st.flags & F_LOGIC_OP)
		newColor = ApplyLogicOp(st.logicOp, oldColor, newColor);
#endif
	uint value = PackColor(newColor);
	if (targetWriteMask)
		value = (value & ~targetWriteMask) | (oldRaw & targetWriteMask);
	fbuf[hp] = value;
}
)HLSL";

// Upload, download and display conversion.
static const char kCompute[] = R"HLSL(
cbuffer CopyCB : register(b0) {
	uint2 origin;     // PSP pixels
	uint2 size;       // PSP pixels
	uint copyScale;
	uint format;      // GE_FORMAT_*, 4 = depth
	uint2 copyPad;
};

Texture2D<uint> srcTex : register(t0);
RWTexture2D<uint> dstTex : register(u0);

// 1x rows (at origin, in srcTex at 0,0) -> render-scale surface.
[numthreads(8, 8, 1)]
void CSExpand(uint3 id : SV_DispatchThreadID) {
	uint2 hp = id.xy;
	if (hp.x >= size.x * copyScale || hp.y >= size.y * copyScale)
		return;
	dstTex[origin * copyScale + hp] = srcTex[hp / copyScale];
}

uint4 RawChannels(uint raw) {
	if (format == 0)
		return uint4(((raw & 0x1F) << 3) | ((raw >> 2) & 7), ((raw >> 3) & 0xFC) | ((raw >> 9) & 3), ((raw >> 8) & 0xF8) | ((raw >> 13) & 7), 0);
	if (format == 1)
		return uint4(((raw & 0x1F) << 3) | ((raw >> 2) & 7), ((raw >> 2) & 0xF8) | ((raw >> 7) & 7), ((raw >> 7) & 0xF8) | ((raw >> 12) & 7), 0);
	if (format == 2)
		return uint4((raw & 0xF) * 17, ((raw >> 4) & 0xF) * 17, ((raw >> 8) & 0xF) * 17, 0);
	return uint4(raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF, 0);
}
// RGB (8-bit) with the stencil bits of keep.
uint PackRaw(uint3 c, uint keep) {
	if (format == 0)
		return (c.r >> 3) | ((c.g >> 2) << 5) | ((c.b >> 3) << 11);
	if (format == 1)
		return (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | (keep & 0x8000);
	if (format == 2)
		return (c.r >> 4) | ((c.g >> 4) << 4) | ((c.b >> 4) << 8) | (keep & 0xF000);
	return c.r | (c.g << 8) | (c.b << 16) | (keep & 0xFF000000);
}

// Render-scale surface -> 1x (at 0,0 in dstTex): the sample at the pixel's center (odd scales), or the
// average color of the four around it (even scales; stencil and depth from one of them).
[numthreads(8, 8, 1)]
void CSReduce(uint3 id : SV_DispatchThreadID) {
	uint2 p = id.xy;
	if (p.x >= size.x || p.y >= size.y)
		return;
	uint2 c = (origin + p) * copyScale + copyScale / 2;
	if ((copyScale & 1) != 0 || format > 3) {
		dstTex[p] = srcTex[c];
		return;
	}
	uint a = srcTex[c - uint2(1, 1)], b = srcTex[c - uint2(0, 1)], d = srcTex[c - uint2(1, 0)], e = srcTex[c];
	uint3 sum = RawChannels(a).rgb + RawChannels(b).rgb + RawChannels(d).rgb + RawChannels(e).rgb;
	dstTex[p] = PackRaw((sum + 2) / 4, e);
}

uint ToRGBA(uint raw) {
	uint r, g, b;
	if (format == 3)
		return raw | 0xFF000000;
	if (format == 0) {
		r = raw & 0x1F; g = (raw >> 5) & 0x3F; b = (raw >> 11) & 0x1F;
		r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
	} else if (format == 1) {
		r = raw & 0x1F; g = (raw >> 5) & 0x1F; b = (raw >> 10) & 0x1F;
		r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
	} else {
		r = (raw & 0xF) * 17; g = ((raw >> 4) & 0xF) * 17; b = ((raw >> 8) & 0xF) * 17;
	}
	return r | (g << 8) | (b << 16) | 0xFF000000;
}

// Render-scale surface region -> RGBA8888 for display (at 0,0 in dstTex).
[numthreads(8, 8, 1)]
void CSDisplay(uint3 id : SV_DispatchThreadID) {
	uint2 hp = id.xy;
	if (hp.x >= size.x * copyScale || hp.y >= size.y * copyScale)
		return;
	dstTex[hp] = ToRGBA(srcTex[origin * copyScale + hp]);
}
)HLSL";

std::string ShaderSource(ShaderKind kind) {
	switch (kind) {
	case ShaderKind::Vertex: return std::string(kCommon) + kVertex;
	case ShaderKind::Pixel: return std::string(kCommon) + kPixel1 + kPixel2;
	default: return kCompute;
	}
}

}  // namespace gpu::hw
