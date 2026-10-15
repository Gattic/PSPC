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

// Matches DrawStateGpu in draw_data.h.
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
	uint rtt;            // render to texture: format | 0x100 (texLevels[0]: x in the surface); texture scale << 12
	                     // (the render scale for render to texture, else the texture upscaling factor)
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
	// Ordinary textures and power-of-two render/upscale factors wrap with a mask, including negative
	// coordinates. Keep one unsigned remainder for factors such as 3x; two remainders per tap are costly
	// at high resolutions. The branch is uniform across a draw's texture samples.
	[branch] if ((period & (period - 1)) == 0)
		return v & (period - 1);
	else {
		// Unsigned magnitude also defines negative-coordinate wrapping in GLSL, whose signed
		// remainder is undefined for negative operands. The subtraction handles INT_MIN too.
		uint magnitude = v < 0 ? 0u - (uint)v : (uint)v;
		int r = (int)(magnitude % (uint)period);
		return v < 0 && r != 0 ? period - r : r;
	}
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
		// The adjacent bilinear tap wraps only at the end of the same period.
		c1 = c0 + 1;
		if (c1 == min(size, limit + 1)) c1 = 0;
	}
}
uint4 SampleLevel(DrawState st, float s, float t, int level, bool bilinear) {
	uint info = st.texLevels[level];
	// A rendered texture is sampled at the render scale, an upscaled one at its scale.
	int texScale = max((int)((st.rtt >> 12) & 0x1F), 1);
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
	[branch] if (st.flags & F_DITHER) {
		uint2 pp = hp / scale;
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

// Display images: a framebuffer's raw pixels as RGBA8888, scaled (CSDisplay and PSPresent).
static const char kDisplay[] = R"HLSL(
uint DisplayRGBA(uint raw, uint fmt) {
	uint r, g, b;
	if (fmt == 3)
		return raw | 0xFF000000;
	if (fmt == 0) {
		r = raw & 0x1F; g = (raw >> 5) & 0x3F; b = (raw >> 11) & 0x1F;
		r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
	} else if (fmt == 1) {
		r = raw & 0x1F; g = (raw >> 5) & 0x1F; b = (raw >> 10) & 0x1F;
		r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
	} else {
		r = (raw & 0xF) * 17; g = ((raw >> 4) & 0xF) * 17; b = ((raw >> 8) & 0xF) * 17;
	}
	return r | (g << 8) | (b << 16) | 0xFF000000;
}
float3 DisplayColor(Texture2D<uint> src, int2 p, uint fmt) {
	uint c = DisplayRGBA(src.Load(int3(p, 0)), fmt);
	return float3(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF) / 255.0;
}
// The average color of the texels under [p0, p1) (texel coordinates in the size-texel region at origin),
// each weighted by how much of it is covered, averaged in linear light (gamma 2): a supersampled image
// reduced to its display size.
float3 AreaAverage(Texture2D<uint> src, int2 origin, int2 size, float2 p0, float2 p1, uint fmt) {
	int2 t0 = max((int2)floor(p0), 0), t1 = min((int2)ceil(p1), size) - 1;
	float3 sum = 0.0;
	float total = 0.0;
	[loop] for (int y = t0.y; y <= t1.y; ++y) {
		float wy = min(p1.y, y + 1.0) - max(p0.y, (float)y);
		[loop] for (int x = t0.x; x <= t1.x; ++x) {
			float w = wy * (min(p1.x, x + 1.0) - max(p0.x, (float)x));
			float3 c = DisplayColor(src, origin + int2(x, y), fmt);
			sum += c * c * w;
			total += w;
		}
	}
	return total > 0.0 ? sqrt(sum / total) : float3(0.0, 0.0, 0.0);
}
// Bilinear filtering at p (texel coordinates, texel centers at +0.5), clamped to the region.
float3 Bilinear(Texture2D<uint> src, int2 size, float2 p, uint fmt) {
	p -= 0.5;
	int2 i = (int2)floor(p);
	float2 f = p - (float2)i;
	int2 a = clamp(i, 0, size - 1), b = clamp(i + 1, 0, size - 1);
	float3 top = lerp(DisplayColor(src, int2(a.x, a.y), fmt), DisplayColor(src, int2(b.x, a.y), fmt), f.x);
	float3 bottom = lerp(DisplayColor(src, int2(a.x, b.y), fmt), DisplayColor(src, int2(b.x, b.y), fmt), f.x);
	return lerp(top, bottom, f.y);
}
)HLSL";

// Upload, download and display conversion.
static const char kCompute[] = R"HLSL(
cbuffer CopyCB : register(b0) {
	uint2 origin;     // PSP pixels
	uint2 size;       // PSP pixels
	uint copyScale;
	uint format;      // GE_FORMAT_*, 4 = depth
	uint outScale;    // CSDisplay: the image's scale, at most copyScale
	uint copyPad;
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

// Render-scale surface region -> RGBA8888 at outScale for display (at 0,0 in dstTex): each pixel the
// average of the render-scale pixels under it.
[numthreads(8, 8, 1)]
void CSDisplay(uint3 id : SV_DispatchThreadID) {
	uint2 hp = id.xy;
	if (hp.x >= size.x * outScale || hp.y >= size.y * outScale)
		return;
	if (outScale == copyScale) {
		dstTex[hp] = DisplayRGBA(srcTex[origin * copyScale + hp], format);
		return;
	}
	float r = (float)copyScale / (float)outScale;
	float3 c = AreaAverage(srcTex, (int2)(origin * copyScale), (int2)(size * copyScale), (float2)hp * r, (float2)(hp + 1) * r, format);
	uint3 q = (uint3)round(saturate(c) * 255.0);
	dstTex[hp] = q.r | (q.g << 8) | (q.b << 16) | 0xFF000000;
}
)HLSL";

// Texture upscaling: xBR level 2, Hyllian's algorithm (after his MIT-licensed xBR-lv2 shader). Every
// output texel comes from the 5x5 texels around its source texel: where an edge runs through a corner of
// the source texel, the corner takes the neighbor's color up to the edge, at 45, 30 or 60 degrees, so
// diagonals and curves come out smooth instead of stepped. Edges are found on luma, with alpha counted
// (outlines against transparent texels are edges too); colors blend premultiplied by alpha.
static const char kUpscale[] = R"HLSL(
cbuffer UpscaleCB : register(b0) {
	int2 lvOrigin;   // the level in the 1x atlas
	int2 lvSize;
	uint factor;
	uint wrapMode;   // 1: S wraps, 2: T wraps (else clamped at the level's edges)
	uint2 upPad;
};
Texture2D<uint4> upSrc : register(t0);
RWTexture2D<uint4> upDst : register(u0);

float4 UpTexel(int2 p, int dx, int dy) {
	int x = p.x + dx, y = p.y + dy;
	x = (wrapMode & 1) ? ((x % lvSize.x) + lvSize.x) % lvSize.x : clamp(x, 0, lvSize.x - 1);
	y = (wrapMode & 2) ? ((y % lvSize.y) + lvSize.y) % lvSize.y : clamp(y, 0, lvSize.y - 1);
	float4 c = (float4)upSrc.Load(int3(lvOrigin + int2(x, y), 0)) / 255.0;
	return float4(c.rgb * c.a, c.a);
}
float Luma(float4 c) {
	return 48.0 * dot(c.rgb, float3(0.299, 0.587, 0.114)) + 24.0 * c.a;
}
float4 Df(float4 a, float4 b) {
	return abs(a - b);
}
bool4 Eq(float4 a, float4 b) {
	return Df(a, b) < 15.0;
}
float4 WeightedDistance(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h) {
	return Df(a, b) + Df(a, c) + Df(d, e) + Df(d, f) + 4.0 * Df(g, h);
}

[numthreads(8, 8, 1)]
void CSUpscale(uint3 id : SV_DispatchThreadID) {
	if (id.x >= (uint)lvSize.x * factor || id.y >= (uint)lvSize.y * factor)
		return;
	int2 p = (int2)(id.xy / factor);
	float2 fp = ((float2)(id.xy % factor) + 0.5) / (float)factor;
	//       A1 B1 C1
	//    A0  A  B  C C4
	//    D0  D  E  F F4
	//    G0  G  H  I I4
	//       G5 H5 I5
	float4 A1 = UpTexel(p, -1, -2), B1 = UpTexel(p, 0, -2), C1 = UpTexel(p, 1, -2);
	float4 A0 = UpTexel(p, -2, -1), A = UpTexel(p, -1, -1), B = UpTexel(p, 0, -1), C = UpTexel(p, 1, -1), C4 = UpTexel(p, 2, -1);
	float4 D0 = UpTexel(p, -2, 0), D = UpTexel(p, -1, 0), E = UpTexel(p, 0, 0), F = UpTexel(p, 1, 0), F4 = UpTexel(p, 2, 0);
	float4 G0 = UpTexel(p, -2, 1), G = UpTexel(p, -1, 1), H = UpTexel(p, 0, 1), I = UpTexel(p, 1, 1), I4 = UpTexel(p, 2, 1);
	float4 G5 = UpTexel(p, -1, 2), H5 = UpTexel(p, 0, 2), I5 = UpTexel(p, 1, 2);

	// The four corners at once, each turned to be the bottom right one: x bottom right, y top right,
	// z top left, w bottom left.
	float4 b = float4(Luma(B), Luma(D), Luma(H), Luma(F));
	float4 c = float4(Luma(C), Luma(A), Luma(G), Luma(I));
	float4 e = Luma(E).xxxx;
	float4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
	float4 i4 = float4(Luma(I4), Luma(C1), Luma(A0), Luma(G5));
	float4 i5 = float4(Luma(I5), Luma(C4), Luma(A1), Luma(G0));
	float4 h5 = float4(Luma(H5), Luma(F4), Luma(B1), Luma(D0));
	float4 f4 = h5.yzwx;

	// The edge lines (45, 30 and 60 degrees) beyond which a corner takes the neighbor's color.
	const float4 Ao = float4(1.0, -1.0, -1.0, 1.0), Bo = float4(1.0, 1.0, -1.0, -1.0), Co = float4(1.5, 0.5, -0.5, 0.5);
	const float4 Ax = float4(1.0, -1.0, -1.0, 1.0), Bx = float4(0.5, 2.0, -0.5, -2.0), Cx = float4(1.0, 1.0, -0.5, 0.0);
	const float4 Ay = float4(1.0, -1.0, -1.0, 1.0), By = float4(2.0, 0.5, -2.0, -0.5), Cy = float4(2.0, 0.0, -1.0, 0.5);
	const float4 Ci = float4(0.25, 0.25, 0.25, 0.25);
	float4 fx = Ao * fp.y + Bo * fp.x;
	float4 fxL = Ax * fp.y + Bx * fp.x;
	float4 fxU = Ay * fp.y + By * fp.x;
	// Antialiased over about one output texel.
	float4 delta = (1.0 / (float)factor).xxxx;
	float4 deltaL = float4(0.5, 1.0, 0.5, 1.0) / (float)factor;
	float4 deltaU = deltaL.yxwz;
	float4 fx45i = saturate((fx + delta - Co - Ci) / (2.0 * delta));
	float4 fx45 = saturate((fx + delta - Co) / (2.0 * delta));
	float4 fx30 = saturate((fxL + deltaL - Cx) / (2.0 * deltaL));
	float4 fx60 = saturate((fxU + deltaU - Cy) / (2.0 * deltaU));

	bool4 lv0 = e != f && e != h;
	bool4 lv1 = lv0 && ((!Eq(f, b) && !Eq(f, c)) || (!Eq(h, d) && !Eq(h, g)) ||
	                    (Eq(e, i) && ((!Eq(f, f4) && !Eq(f, i4)) || (!Eq(h, h5) && !Eq(h, i5)))) || Eq(e, g) || Eq(e, c));
	bool4 lv2L = e != g && d != g;
	bool4 lv2U = e != c && b != c;
	float4 wd1 = WeightedDistance(e, c, g, i, h5, f4, h, f);
	float4 wd2 = WeightedDistance(h, d, i5, f, i4, b, e, i);
	bool4 edri = wd1 <= wd2 && lv0;
	bool4 edr = wd1 < wd2 && lv1;
	bool4 edrL = 2.0 * Df(f, g) <= Df(h, c) && lv2L && edr;
	bool4 edrU = Df(f, g) >= 2.0 * Df(h, c) && lv2U && edr;
	fx45 *= (float4)edr;
	fx30 *= (float4)edrL;
	fx60 *= (float4)edrU;
	fx45i *= (float4)edri;
	bool4 px = Df(e, f) <= Df(e, h);
	float4 m = max(max(fx30, fx60), max(fx45, fx45i));

	float4 res1 = lerp(E, px.x ? F : H, m.x);
	res1 = lerp(res1, px.z ? D : B, m.z);
	float4 res2 = lerp(E, px.y ? B : F, m.y);
	res2 = lerp(res2, px.w ? H : D, m.w);
	// Of the two, the one further from E.
	const float4 one = float4(1.0, 1.0, 1.0, 1.0);
	float4 r = dot(abs(E - res2), one) >= dot(abs(E - res1), one) ? res2 : res1;
	float3 rgb = r.a > 0.0 ? r.rgb / r.a : float3(0.0, 0.0, 0.0);
	upDst[(uint2)lvOrigin * factor + id.xy] = (uint4)round(saturate(float4(rgb, r.a)) * 255.0);
}
)HLSL";

// Presentation: an image (a surface at the render scale, or a CPU image) drawn into the window's back
// buffer at dstOrigin, scaled by 1 / dstScale. Reduced: each pixel the average of the texels under it
// (supersampling); enlarged: bilinear or nearest. An overlay (RGBA8888, premultiplied alpha, covering
// the same rectangle) may go over it.
static const char kPresent[] = R"HLSL(
cbuffer PresentCB : register(b0) {
	float2 dstOrigin;  // the image's top left in the window, pixels
	float2 dstScale;   // source texels per window pixel
	int2 srcSize;      // the source image, texels
	uint srcFormat;    // GE_FORMAT_* (3 for a CPU image)
	uint magFilter;    // enlarging: 0 nearest, 1 bilinear
	int2 overlaySize;  // 0: no overlay
	uint2 presentPad;
};
Texture2D<uint> presentTex : register(t0);
Texture2D<uint> overlayTex : register(t1);

float4 OverlayTexel(int2 p) {
	uint c = overlayTex.Load(int3(clamp(p, 0, overlaySize - 1), 0));
	return float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
}
// The overlay, bilinear at p (its texel coordinates).
float4 Overlay(float2 p) {
	p -= 0.5;
	int2 i = (int2)floor(p);
	float2 f = p - (float2)i;
	float4 top = lerp(OverlayTexel(i), OverlayTexel(i + int2(1, 0)), f.x);
	float4 bottom = lerp(OverlayTexel(i + int2(0, 1)), OverlayTexel(i + int2(1, 1)), f.x);
	return lerp(top, bottom, f.y);
}

float4 VSPresent(uint id : SV_VertexID) : SV_Position {
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}

float4 PSPresent(float4 pos : SV_Position) : SV_Target {
	float2 p0 = (floor(pos.xy) - dstOrigin) * dstScale, p1 = p0 + dstScale;
	float3 c;
	if (dstScale.x > 1.0 || dstScale.y > 1.0)
		c = AreaAverage(presentTex, int2(0, 0), srcSize, p0, p1, srcFormat);
	else if (magFilter != 0)
		c = Bilinear(presentTex, srcSize, (p0 + p1) * 0.5, srcFormat);
	else
		c = DisplayColor(presentTex, clamp((int2)floor((p0 + p1) * 0.5), 0, srcSize - 1), srcFormat);
	if (overlaySize.x > 0) {
		float4 o = Overlay((pos.xy - dstOrigin) * dstScale * (float2)overlaySize / (float2)srcSize);
		c = o.rgb + c * (1.0 - o.a);
	}
	return float4(c, 1.0);
}
)HLSL";

std::string ShaderSource(ShaderKind kind) {
	switch (kind) {
	case ShaderKind::Vertex: return std::string(kCommon) + kVertex;
	case ShaderKind::Pixel: return std::string(kCommon) + kPixel1 + kPixel2;
	case ShaderKind::Upscale: return kUpscale;
	case ShaderKind::Present: return std::string(kDisplay) + kPresent;
	default: return std::string(kDisplay) + kCompute;
	}
}

}  // namespace gpu::hw
