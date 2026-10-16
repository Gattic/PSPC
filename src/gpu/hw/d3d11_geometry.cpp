// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// HLSL for vertex transform and triangle assembly on the GPU (gpu/geometry.h): CSVertex is
// transform.cpp's ReadVertex for one vertex, CSTriangle its ProcessTriangle for one job, followed by
// the binner's winding test and DrawData::EmitTriangle. The GE's arithmetic (gemath.h) is done with
// integer operations, so the results are the CPU's bit for bit: products are kept exactly and
// truncated as the GE does, sums are aligned and added as integers, and values that leave the float
// range are rounded into denormals or infinity as the CPU's conversions round them (the GPU flushes
// denormal operands to zero, so no float operation sees one). The few plain float operations (fog
// without the GE's fog unit, UV prescale, the final texture coordinates) are `precise`, so they are
// correctly rounded like the CPU's. With the device's extended double instructions (DOUBLES=1), the
// texture-level gradient and the reciprocal w of vertices behind the camera are computed in doubles
// as on the CPU; without them, in floats.
//
// The compiler inlines every call, so the code keeps each costly piece at one call site, in loops the
// compiler doesn't unroll: matrices come by column, and clipping and output go through small arrays.
#include <cstdio>
#include <string>

#include "gpu/gemath.h"
#include "gpu/hw/d3d11_internal.h"

namespace gpu::hw {

static const char kGeometry1[] = R"HLSL(
#ifndef DOUBLES
#define DOUBLES 0
#endif

#define INT_MIN_V (-2147483647 - 1)
#define SIGN_BIT 0x80000000u
#define OUTSIDE_X 2147483647  // ClipV::x of a vertex outside the screen range

struct GeVertexIn { float2 uv; uint color; float3 nrm; float3 pos; uint draw; };
struct GeLight {
	float4 pos; float4 vecCols[3]; float4 att; float4 spotDir;
	float spotDirRsqrt; float spotCutoff; float spotExp; uint flags;
	int4 ambient; int4 diffuse; int4 specular;
};
struct GeShadeLight { float4 pos; float4 vecCols[3]; uint flags; uint3 pad; };
struct GeTransform {
	float4 mtxCols[4]; float4 tgenCols[4]; float4 worldCols[3];
	float4 viewZRows; float4 posToFog; float4 screenScale; float4 screenAdd; float4 uvScaleF; float4 viewDir;
	float2 uvScale; float2 uvOffset;
	float fogEnd; float fogSlope; float specularExp; uint flags;
	int offsetX16; int offsetY16; uint materialAmbientRGBA; uint materialEmissive;
	int4 materialAmbient; int4 materialDiffuse; int4 materialSpecular; int4 baseAmbient;
	GeLight lights[4];
	GeShadeLight shade[2];
};
struct GeDraw { uint transform; uint flags; uint state; uint texShift; float4 carryUV; float4 carryNormal; };
struct GeJob { uint3 v; uint provoking; uint draw; uint reversed; uint outBase; uint pad; };
// transform.cpp ClipVertex.
struct ClipV { float4 clip; int x; int y; int z; uint color0; uint color1; float s; float t; float q; float clipw; float fog; };

#define GL_ENABLED 1u
#define GL_SPOT 2u
#define GL_DIRECTIONAL 4u
#define GL_POWERED_DIFFUSE 8u
#define GL_AMBIENT 16u
#define GL_DIFFUSE 32u
#define GL_SPECULAR 64u

#define GT_LIGHTING (1u << 0)
#define GT_FOG (1u << 1)
#define GT_FOG_GE (1u << 2)
#define GT_NEGATE_NORMALS (1u << 3)
#define GT_DEPTH_CLIP (1u << 4)
#define GT_GE_UV_SCALE (1u << 5)
#define GT_UV_PRESCALE (1u << 6)
#define GT_UV_GEN_SHIFT 8
#define GT_UV_PROJ_SHIFT 10
#define GT_COLOR_AMBIENT (1u << 12)
#define GT_COLOR_DIFFUSE (1u << 13)
#define GT_COLOR_SPECULAR (1u << 14)
#define GT_SET_COLOR1 (1u << 15)
#define GT_ADD_COLOR1 (1u << 16)
#define GT_WORLD_NORMAL (1u << 17)

#define GD_HAS_UV 1u
#define GD_HAS_NORMAL 2u
#define GD_HAS_COLOR 4u
#define GD_DEPTH_CLIP 8u
#define GD_GOURAUD 16u
#define GD_TEXTURED 32u
#define GD_FOG 64u
#define GD_PROJ 128u
#define GD_STATE_GOURAUD 256u

#define TEXMAP_TEXTURE_MATRIX 1
#define TEXMAP_ENVIRONMENT_MAP 2
#define PROJMAP_UV 1
#define PROJMAP_NORMALIZED_NORMAL 2
#define PROJMAP_NORMAL 3

StructuredBuffer<GeVertexIn> gInputs : register(t0);
StructuredBuffer<GeDraw> gDraws : register(t1);
StructuredBuffer<GeTransform> gTransforms : register(t2);
StructuredBuffer<GeJob> gJobs : register(t3);
StructuredBuffer<ClipV> gClipIn : register(t4);
RWStructuredBuffer<ClipV> gClipOut : register(u0);
RWByteAddressBuffer gOut : register(u1);

cbuffer GeoCB : register(b0) {
	uint gCount;
	uint gScale;
	uint gBase;  // CSVertex: the first input
	uint gPad;
};

// --- The GE's arithmetic (gemath.h) ---

uint Exp(uint b) { return (b >> 23) & 0xFFu; }
float Trunc24(float f) { return asfloat(asuint(f) & 0xFFFFFF00u); }
bool IsFiniteF(float f) { return (asuint(f) & 0x7F800000u) != 0x7F800000u; }
bool IsNaNF(float f) { return (asuint(f) & 0x7FFFFFFFu) > 0x7F800000u; }
float Neg(float f) { return asfloat(asuint(f) ^ SIGN_BIT); }
float AbsF(float f) { return asfloat(asuint(f) & 0x7FFFFFFFu); }

// The CPU's float to int conversion (cvttss2si): truncation, and INT_MIN out of range or for NaN.
int CvtTrunc(float x) {
	if (x >= -2147483648.0f && x < 2147483648.0f)
		return (int)x;
	return INT_MIN_V;
}

// a * b as 64 bits (lo, hi).
uint2 Mul32x32(uint a, uint b) {
	uint a0 = a & 0xFFFFu, a1 = a >> 16, b0 = b & 0xFFFFu, b1 = b >> 16;
	uint p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	uint mid = p01 + (p00 >> 16);
	uint mid2 = (mid & 0xFFFFu) + p10;
	return uint2((mid2 << 16) | (p00 & 0xFFFFu), p11 + (mid >> 16) + (mid2 >> 16));
}

// sign | m * 2^e (m != 0) where that is past the float range: infinity above it, below it the
// denormal rounded to nearest (ties to even), as ldexpf and double-to-float conversions round.
float OutOfRange(uint sign, uint m, int e) {
	int lead = firstbithigh(m);
	if (e + lead + 127 >= 255)
		return asfloat(sign | 0x7F800000u);
	// The denormal's mantissa is m * 2^(e + 149).
	int shift = -(e + 149);
	if (shift <= 0)
		return asfloat(sign | (m << (uint)(-shift)));
	if (shift > 32)
		return asfloat(sign);
	if (shift == 32)
		return asfloat(sign | (m > 0x80000000u ? 1u : 0u));
	uint q = m >> (uint)shift;
	uint rem = m & ((1u << (uint)shift) - 1u);
	uint halfway = 1u << (uint)(shift - 1);
	if (rem > halfway || (rem == halfway && (q & 1u) != 0))
		q++;
	return asfloat(sign | q);
}

// sign | m * 2^e for m < 2^24, exactly representable while in the float range.
float MakeExact(uint sign, uint m, int e) {
	if (m == 0)
		return asfloat(sign);
	int lead = firstbithigh(m);
	int biased = e + lead + 127;
	if (biased >= 1 && biased <= 254)
		return asfloat(sign | ((uint)biased << 23) | ((m << (uint)(23 - lead)) & 0x7FFFFFu));
	return OutOfRange(sign, m, e);
}

float ScaleByPow2(float f, int n, uint sign) {
	return asfloat((uint)((int)asuint(f) + n * (1 << 23)) | sign);
}

// ProductToFloat24((double)a * b): the exact product truncated to 16 significant bits, then to float.
float ProductTo24(float a, float b) {
	uint ba = asuint(a), bb = asuint(b);
	uint sign = (ba ^ bb) & SIGN_BIT;
	uint ea = Exp(ba), eb = Exp(bb);
	uint fa = ba & 0x7FFFFFu, fb = bb & 0x7FFFFFu;
	if (ea == 255u && fa != 0)
		return asfloat(ba | 0x400000u);
	if (eb == 255u && fb != 0)
		return asfloat(bb | 0x400000u);
	bool zeroA = ea == 0 && fa == 0, zeroB = eb == 0 && fb == 0;
	if (ea == 255u || eb == 255u)
		return (zeroA || zeroB) ? asfloat(0xFFC00000u) : asfloat(sign | 0x7F800000u);
	if (zeroA || zeroB)
		return asfloat(sign);
	uint ma = ea != 0 ? (fa | 0x800000u) : fa, mb = eb != 0 ? (fb | 0x800000u) : fb;
	int Ea = ea != 0 ? (int)ea - 127 : -126, Eb = eb != 0 ? (int)eb - 127 : -126;
	uint2 p = Mul32x32(ma, mb);
	int lead = p.y != 0 ? 32 + firstbithigh(p.y) : firstbithigh(p.x);
	int sh = lead - 15;
	uint T;
	if (sh <= 0)
		T = p.x << (uint)(-sh);
	else if (sh < 32)
		T = (p.x >> (uint)sh) | (p.y << (uint)(32 - sh));
	else
		T = p.y >> (uint)(sh - 32);
	return MakeExact(sign, T, Ea + Eb - 46 + sh);
}

// gemath.cpp tables (filled in by GeometryShaderSource).
TABLES

// GERecip: w a normal float24.
float GERecip(float w) {
	uint bits = asuint(w);
	uint i = (bits >> 8) & 0x7FFFu;
	int e = (int)Exp(bits) - 127;
	int2 seg = kRecip[i >> 8];
	int q = (64 * seg.x + 63 + seg.y * (int)(i & 255u)) >> 7;
	if (e >= 126)
		return MakeExact(bits & SIGN_BIT, (uint)q, -16 - e);
	return ScaleByPow2((float)q, -16 - e, bits & SIGN_BIT);
}

float GERsqrt(float d) {
	uint bits = asuint(d);
	uint i = (bits >> 8) & 0x7FFFu;
	int e = (int)Exp(bits) - 127;
	int2 seg = kRsqrt[(e & 1) * 128 + (int)(i >> 8)];
	int q = (64 * seg.x + 63 + seg.y * (int)(i & 255u)) >> 7;
	return ScaleByPow2((float)q, -16 - (e >> 1), 0u);
}

// The adder: the smaller term truncated to the precision of the larger one, then added (exactly).
float GEAdd(float a, float b) {
	uint ba = asuint(a), bb = asuint(b);
	int ea = (int)Exp(ba), eb = (int)Exp(bb);
	if (ea == 0 || eb == 0)
		return ea == 0 ? (eb == 0 ? 0.0f : Trunc24(b)) : Trunc24(a);
	int emax = max(ea, eb);
	int na = 8 + emax - ea, nb = 8 + emax - eb;
	if (emax == 255) {
		uint ma = na >= 24 ? (ba & SIGN_BIT) : (ba & ~((1u << (uint)na) - 1u));
		uint mb = nb >= 24 ? (bb & SIGN_BIT) : (bb & ~((1u << (uint)nb) - 1u));
		return asfloat(ma) + asfloat(mb);
	}
	int A = na >= 24 ? 0 : (int)(((ba & 0x7FFFFFu) | 0x800000u) >> (uint)na);
	int B = nb >= 24 ? 0 : (int)(((bb & 0x7FFFFFu) | 0x800000u) >> (uint)nb);
	if ((ba & SIGN_BIT) != 0)
		A = -A;
	if ((bb & SIGN_BIT) != 0)
		B = -B;
	int S = A + B;
	if (S == 0)
		return 0.0f;
	// S * 2^(emax - 142): 17 bits at most, so exact (as a denormal too: its unit is 2^-141 or more).
	uint sign = S < 0 ? SIGN_BIT : 0u;
	uint m = (uint)abs(S);
	int lead = firstbithigh(m);
	int biased = emax - 142 + lead + 127;
	if (biased >= 1)
		return asfloat(sign | ((uint)biased << 23) | ((m << (uint)(23 - lead)) & 0x7FFFFFu));
	return asfloat(sign | (m << (uint)(emax + 7)));
}

float GEAddFloat24(float a, float b) { return Trunc24(GEAdd(a, b)); }

static const int2 NO_TERM = int2(0, INT_MIN_V);

// GEProduct of two float24s: their 16-bit mantissas multiply within 32 bits.
int2 GEProduct24(float a, float b) {
	uint ba = asuint(a), bb = asuint(b);
	int ea = (int)Exp(ba), eb = (int)Exp(bb);
	if (ea == 0 || eb == 0)
		return NO_TERM;
	uint A = ((ba >> 8) & 0x7FFFu) | 0x8000u, B = ((bb >> 8) & 0x7FFFu) | 0x8000u;
	int m = (int)((A * B) >> 15);
	return int2(((ba ^ bb) & SIGN_BIT) != 0 ? -m : m, (ea - 127) + (eb - 127) - 15);
}

// GEProduct of a float24 and any float.
int2 GEProduct(float a, float b) {
	uint ba = asuint(a), bb = asuint(b);
	int ea = (int)Exp(ba), eb = (int)Exp(bb);
	if (ea == 0 || eb == 0)
		return NO_TERM;
	uint2 p = Mul32x32((ba & 0x7FFFFFu) | 0x800000u, (bb & 0x7FFFFFu) | 0x800000u);
	int m = (int)((p.y << 1) | (p.x >> 31));
	return int2(((ba ^ bb) & SIGN_BIT) != 0 ? -m : m, (ea - 127) + (eb - 127) - 15);
}

// GERowSum: every term truncated to the largest term's bit weight, added exactly.
float GERowSum(int2 t0, int2 t1, int2 t2, int2 t3) {
	int2 t[4] = { t0, t1, t2, t3 };
	int lsbExp = max(max(t0.y, t1.y), max(t2.y, t3.y));
	int sum = 0;
	[unroll] for (int j = 0; j < 4; ++j) {
		int m = t[j].x;
		int shift = lsbExp - t[j].y;
		if (m != 0 && shift < 32)
			sum += m < 0 ? -((-m) >> (uint)shift) : (m >> (uint)shift);
	}
	if (sum == 0)
		return 0.0f;
	uint bits = asuint((float)sum);
	int e = (int)Exp(bits) + lsbExp;
	if (e >= 1 && e <= 254)
		return asfloat((bits + ((uint)lsbExp << 23)) & 0xFFFFFF00u);
	return Trunc24(OutOfRange(sum < 0 ? SIGN_BIT : 0u, (uint)abs(sum), lsbExp));
}
)HLSL";

static const char kGeometry2[] = R"HLSL(
// sum_k a[k] * b[k], a truncated to float24 here (b already is).
float GERow4(float4 a, float4 b) {
	return GERowSum(GEProduct24(Trunc24(a.x), b.x), GEProduct24(Trunc24(a.y), b.y), GEProduct24(Trunc24(a.z), b.z),
	                GEProduct24(Trunc24(a.w), b.w));
}
float GERow3(float3 a, float3 b) {
	return GERowSum(GEProduct24(Trunc24(a.x), b.x), GEProduct24(Trunc24(a.y), b.y), GEProduct24(Trunc24(a.z), b.z), NO_TERM);
}
float GEDot3(float3 a, float3 b) {
	return GERowSum(GEProduct24(Trunc24(a.x), Trunc24(b.x)), GEProduct24(Trunc24(a.y), Trunc24(b.y)),
	                GEProduct24(Trunc24(a.z), Trunc24(b.z)), NO_TERM);
}

// v normalized like the GE; returns the length (0 leaves v alone). d2 = GEDot3(v, v).
float GENormalize3(inout float3 v, float d2) {
	if (!(d2 > 0.0f) || !IsFiniteF(d2))
		return 0.0f;
	float r = GERsqrt(d2);
	float o[3] = { v.x, v.y, v.z };
	[loop] for (int i = 0; i < 3; ++i)
		o[i] = GERowSum(GEProduct(Trunc24(o[i]), r), NO_TERM, NO_TERM, NO_TERM);
	v = float3(o[0], o[1], o[2]);
	return ProductTo24(d2, r);
}

// GEFogFactor / 256.
float GEFogFactorF(float f) {
	uint bits = asuint(f);
	uint e = bits >> 23;
	if ((bits & SIGN_BIT) != 0 || e <= 126u - 8u)
		return 0.0f;
	if (e > 126u)
		return 255.0f * (1.0f / 256.0f);
	uint mantissa = (bits & 0x7FFFFFu) | 0x800000u;
	return (float)(mantissa >> (16u + 126u - e)) * (1.0f / 256.0f);
}

// GELightPow: exp2(e * log2(v)) the GE's way, on the float's bits.
float GELightPow(float v, float e) {
	if (e <= 0.0f)
		return 1.0f;
	if (!(v > 0.0f))
		return v;
	int D = (int)asuint(v) - 0x3F800000;
	// e = M * 2^(E - 4) with M = 16..31 (PSPLightExponent keeps four mantissa bits), so
	// trunc(e * D / 16) = sign(D) * ((M * |D|) >> (8 - E)), E <= 8 (e < 512).
	uint eb = asuint(e);
	int E = (int)Exp(eb) - 127;
	uint2 p = Mul32x32(16u + ((eb >> 19) & 15u), (uint)abs(D));
	int k = 8 - E;
	uint xlo, xhi;
	if (k >= 64) {
		xlo = 0;
		xhi = 0;
	} else if (k >= 32) {
		xlo = p.y >> (uint)(k - 32);
		xhi = 0;
	} else if (k > 0) {
		xlo = (p.x >> (uint)k) | (p.y << (uint)(32 - k));
		xhi = p.y >> (uint)k;
	} else {
		xlo = p.x;
		xhi = p.y;
	}
	// t = X * 16 + 1065353216 clamped to [0, 2139095039]: past 2^26 either way it's clamped.
	if (xhi != 0 || xlo >= (1u << 26))
		return D < 0 ? 0.0f : asfloat(2139095039u);
	int X = D < 0 ? -(int)xlo : (int)xlo;
	return asfloat((uint)clamp(X * 16 + 1065353216, 0, 2139095039));
}

// --- transform.cpp ---

int4 ColorFactor(uint rgba) {
	return int4((int)(rgba & 0xFFu), (int)((rgba >> 8) & 0xFFu), (int)((rgba >> 16) & 0xFFu), (int)(rgba >> 24)) * 2 + 1;
}
int4 LightProduct(int4 l, int4 m) { return (l * m) >> 10; }
int4 LightScale(int4 x, float f) {
	precise float f256 = 256.0f * f;
	int s = clamp(CvtTrunc(f256), 0, 256);
	return ((x * 2 + 1) * (2 * s + 1)) >> 10;
}
int4 ScaleAttSpot(int4 c, float att, float spot) {
	if (att < 1.0f)
		c = LightScale(c, att);
	if (spot < 1.0f)
		c = LightScale(c, spot);
	return c;
}
uint ToRGBA(int4 c) {
	uint4 u = (uint4)clamp(c, 0, 255);
	return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24);
}

#define SCREEN_BOUND 4095.96875f

// Viewport, then RoundToScreen: the vertex's screen position from its clip coordinates.
void ClipToScreen(uint ti, inout ClipV v, bool alwaysCheck, inout bool outside) {
	float w = Trunc24(v.clip.w);
	float c[4] = { v.clip.x, v.clip.y, v.clip.z, v.clip.w };
	float4 sc = gTransforms[ti].screenScale, sa = gTransforms[ti].screenAdd;
	float scale[3] = { sc.x, sc.y, sc.z };
	float add[3] = { sa.x, sa.y, sa.z };
	// An unusable w takes GEViewport's float path for every component.
	bool slow = !IsFiniteF(w) || AbsF(w) < 1.175494351e-38f;
	float r = slow ? 0.0f : GERecip(w);
	float o[3];
	[loop] for (int i = 0; i < 3; ++i) {
		float ci = c[i];
		if (slow || !IsFiniteF(ci)) {
			precise float q = ci * scale[i] / c[3] + add[i];
			o[i] = q;
		} else {
			o[i] = GEAdd(ProductTo24(ProductTo24(Trunc24(ci), r), scale[i]), add[i]);
		}
	}
	float z = floor(o[2]);
	if ((gTransforms[ti].flags & GT_DEPTH_CLIP) != 0) {
		if ((alwaysCheck || !(c[2] < Neg(c[3]))) && !(o[0] < SCREEN_BOUND && o[1] < SCREEN_BOUND && o[0] >= 0.0f && o[1] >= 0.0f))
			outside = true;
		if (z < 0.0f)
			z = 0.0f;
		else if (z > 65535.0f)
			z = 65535.0f;
	} else if (!(o[0] <= SCREEN_BOUND && o[1] < SCREEN_BOUND && o[0] >= 0.0f && o[1] >= 0.0f && z >= 0.0f && z < 65536.0f)) {
		outside = true;
	}
	v.x = CvtTrunc(floor(o[0] * 16.0f)) - gTransforms[ti].offsetX16;
	v.y = CvtTrunc(floor(o[1] * 16.0f)) - gTransforms[ti].offsetY16;
	v.z = (z >= 0.0f && z < 65536.0f) ? (int)((uint)z & 0xFFFFu) : 0;
}

// v . N for the unnormalized N, scaled by its reciprocal length.
float GENormalDot(float3 v, float3 n, float nRsqrt) {
	return ProductTo24(GEDot3(v, n), nRsqrt);
}

float3 AddViewDir(uint ti, float3 L) {
	float4 vd = gTransforms[ti].viewDir;
	return float3(GEAddFloat24(L.x, vd.x), GEAddFloat24(L.y, vd.y), GEAddFloat24(L.z, vd.z));
}

// The vector from a vertex to a point light (GELightVector), from the light vector's matrix by column.
float3 LightVector(float4 cols[3], float4 modelpos) {
	float o[3];
	[loop] for (int c = 0; c < 3; ++c)
		o[c] = GERow4(modelpos, cols[c]);
	return float3(o[0], o[1], o[2]);
}
)HLSL";

static const char kGeometry3[] = R"HLSL(
void ProcessLighting(uint ti, inout ClipV vertex, float4 modelpos, float3 worldnormal, float normalRsqrt) {
	uint f = gTransforms[ti].flags;
	int4 colorFactor = int4(0, 0, 0, 0);
	if ((f & (GT_COLOR_AMBIENT | GT_COLOR_DIFFUSE | GT_COLOR_SPECULAR)) != 0)
		colorFactor = ColorFactor(vertex.color0);
	uint mecv = gTransforms[ti].materialEmissive;
	int4 mac = (f & GT_COLOR_AMBIENT) != 0 ? colorFactor : gTransforms[ti].materialAmbient;
	int4 emissive = int4((int)(mecv & 0xFFu), (int)((mecv >> 8) & 0xFFu), (int)((mecv >> 16) & 0xFFu), 0);
	int4 final_color = emissive + ((mac * gTransforms[ti].baseAmbient) >> 10);
	int4 specular_color = int4(0, 0, 0, 0);
	float specularExp = gTransforms[ti].specularExp;

	[loop] for (int l = 0; l < 4; ++l) {
		GeLight ls = gTransforms[ti].lights[l];
		if ((ls.flags & GL_ENABLED) == 0)
			continue;
		float3 L = ls.pos.xyz;
		float att = 1.0f, spot = 1.0f;
		if ((ls.flags & GL_DIRECTIONAL) == 0) {
			L = LightVector(ls.vecCols, modelpos);
			float d2 = GEDot3(L, L);
			float d = GENormalize3(L, d2);
			if (d == 0.0f)
				L = float3(0.0f, 0.0f, 1.0f);
			float den = GEDot3(ls.att.xyz, float3(1.0f, d, d2));
			att = den > 0.0f ? GERecip(den) : 0.0f;
			if (!(att > 0.0f))
				att = 0.0f;
			else if (att > 1.0f)
				att = 1.0f;
		}
		if ((ls.flags & GL_SPOT) != 0) {
			float rawSpot = ProductTo24(GEDot3(ls.spotDir.xyz, L), ls.spotDirRsqrt);
			if (IsNaNF(rawSpot))
				rawSpot = (asuint(rawSpot) & SIGN_BIT) != 0 ? 0.0f : 1.0f;
			if (rawSpot >= ls.spotCutoff) {
				spot = GELightPow(rawSpot, ls.spotExp);
				if (IsNaNF(spot))
					spot = 0.0f;
			} else {
				spot = 0.0f;
			}
		}
		if ((ls.flags & GL_AMBIENT) != 0)
			final_color += ScaleAttSpot(LightProduct(ls.ambient, mac), att, spot);
		// The diffuse factor (j = 0), then the specular one (j = 1) when it applies: they share the dot
		// product and the pow.
		float factor[2] = { 0.0f, 0.0f };
		bool specular = false;
		[loop] for (int j = 0; j < 2; ++j) {
			if (j == 0 && (ls.flags & (GL_DIFFUSE | GL_SPECULAR)) == 0)
				break;
			if (j == 1 && !specular)
				break;
			float3 dir = L;
			if (j == 1) {
				dir = AddViewDir(ti, L);
				if (GENormalize3(dir, GEDot3(dir, dir)) == 0.0f)
					dir = float3(0.0f, 0.0f, 1.0f);
			}
			float fj = GENormalDot(dir, worldnormal, normalRsqrt);
			if (j == 1 || (ls.flags & GL_POWERED_DIFFUSE) != 0)
				fj = GELightPow(fj, specularExp);
			factor[j] = fj;
			if (j == 0)
				specular = (ls.flags & GL_SPECULAR) != 0 && fj >= 0.0f;
		}
		if ((ls.flags & GL_DIFFUSE) != 0) {
			int4 mdc = (f & GT_COLOR_DIFFUSE) != 0 ? colorFactor : gTransforms[ti].materialDiffuse;
			final_color += ScaleAttSpot(LightScale(LightProduct(ls.diffuse, mdc), factor[0]), att, spot);
		}
		if (specular && factor[1] > 0.0f) {
			int4 msc = (f & GT_COLOR_SPECULAR) != 0 ? colorFactor : gTransforms[ti].materialSpecular;
			specular_color += ScaleAttSpot(LightScale(LightProduct(ls.specular, msc), factor[1]), att, spot);
		}
	}

	if ((f & GT_SET_COLOR1) != 0) {
		vertex.color0 = ToRGBA(final_color);
		specular_color.w = 0;
		vertex.color1 = ToRGBA(specular_color) & 0x00FFFFFFu;
	} else if ((f & GT_ADD_COLOR1) != 0) {
		vertex.color0 = ToRGBA(final_color + specular_color);
	} else {
		vertex.color0 = ToRGBA(final_color);
	}
}

// ReadVertex for a transform-mode vertex.
ClipV ReadVertex(uint ti, GeDraw d, GeVertexIn sv) {
	ClipV vertex = (ClipV)0;
	uint f = gTransforms[ti].flags;
	float4 pos = float4(sv.pos, 1.0f);
	float3 cuv = d.carryUV.xyz;
	if ((d.flags & GD_HAS_UV) != 0) {
		cuv.xy = sv.uv;
		if ((f & GT_UV_PRESCALE) != 0) {
			float4 ps = gTransforms[ti].uvScaleF;
			precise float u = cuv.x * ps.x + ps.z;
			precise float v = cuv.y * ps.y + ps.w;
			cuv.xy = float2(u, v);
		}
		cuv.z = 0.0f;
	}
	float tc[2] = { cuv.x, cuv.y };
	if ((f & GT_GE_UV_SCALE) != 0) {
		float scale[2] = { gTransforms[ti].uvScale.x, gTransforms[ti].uvScale.y };
		float offset[2] = { gTransforms[ti].uvOffset.x, gTransforms[ti].uvOffset.y };
		[loop] for (int k = 0; k < 2; ++k)
			tc[k] = Trunc24(GEAdd(ProductTo24(Trunc24(tc[k]), scale[k]), offset[k]));
	}
	vertex.s = tc[0];
	vertex.t = tc[1];
	vertex.q = cuv.z;
	float3 normal = (d.flags & GD_HAS_NORMAL) != 0 ? sv.nrm : d.carryNormal.xyz;
	if ((f & GT_NEGATE_NORMALS) != 0)
		normal = float3(Neg(normal.x), Neg(normal.y), Neg(normal.z));
	vertex.color0 = (d.flags & GD_HAS_COLOR) != 0 ? sv.color : gTransforms[ti].materialAmbientRGBA;
	vertex.color1 = 0;

	float clip[4];
	[loop] for (int c = 0; c < 4; ++c)
		clip[c] = GERow4(pos, gTransforms[ti].mtxCols[c]);
	vertex.clip = float4(clip[0], clip[1], clip[2], clip[3]);
	bool outside = false;
	ClipToScreen(ti, vertex, false, outside);
	vertex.clipw = vertex.clip.w;
	vertex.fog = 1.0f;
	bool depthClip = (f & GT_DEPTH_CLIP) != 0;
	if (outside) {
		vertex.x = OUTSIDE_X;
		// Only a vertex the near plane clips away can still contribute (to new vertices).
		if (!(depthClip && vertex.clip.z < Neg(vertex.clip.w)))
			return vertex;
	}

	if ((f & GT_FOG) != 0) {
		if ((f & GT_FOG_GE) != 0) {
			float viewZ = GERow4(pos, gTransforms[ti].viewZRows);
			vertex.fog = GEFogFactorF(ProductTo24(Trunc24(GEAdd(viewZ, gTransforms[ti].fogEnd)), gTransforms[ti].fogSlope));
		} else {
			float4 pf = gTransforms[ti].posToFog;
			precise float dz = pf.x * pos.x + pf.y * pos.y + pf.z * pos.z + pf.w;
			vertex.fog = GEFogFactorF(dz);
		}
	}

	float3 worldnormal = float3(0.0f, 0.0f, 0.0f);
	float normalRsqrt = 1.0f;
	int uvGen = (int)((f >> GT_UV_GEN_SHIFT) & 3u);
	bool usesWorldNormal = (f & GT_LIGHTING) != 0 ? (f & GT_WORLD_NORMAL) != 0 : uvGen == TEXMAP_ENVIRONMENT_MAP;
	if (usesWorldNormal) {
		float wn[3];
		[loop] for (int c = 0; c < 3; ++c)
			wn[c] = GERow3(normal, gTransforms[ti].worldCols[c].xyz);
		worldnormal = float3(wn[0], wn[1], wn[2]);
		float len2 = GEDot3(worldnormal, worldnormal);
		if (len2 > 0.0f && IsFiniteF(len2))
			normalRsqrt = GERsqrt(len2);
		else if (len2 != 0.0f)
			worldnormal = float3(0.0f, 0.0f, 1.0f);
	}

	if (uvGen == TEXMAP_TEXTURE_MATRIX) {
		float4 source = pos;
		int proj = (int)((f >> GT_UV_PROJ_SHIFT) & 3u);
		if (proj == PROJMAP_UV) {
			source = float4(vertex.s, vertex.t, 0.0f, 1.0f);
		} else if (proj == PROJMAP_NORMALIZED_NORMAL) {
			float3 n = normal;
			GENormalize3(n, GEDot3(n, n));
			source = float4(n, 1.0f);
		} else if (proj == PROJMAP_NORMAL) {
			source = float4(normal, 1.0f);
		}
		float o[3];
		[loop] for (int c = 0; c < 3; ++c)
			o[c] = GERow4(source, gTransforms[ti].tgenCols[c]);
		vertex.s = o[0];
		vertex.t = o[1];
		vertex.q = o[2];
	} else if (uvGen == TEXMAP_ENVIRONMENT_MAP) {
		float st[2];
		[loop] for (int k = 0; k < 2; ++k) {
			GeShadeLight sl = gTransforms[ti].shade[k];
			float3 L = sl.pos.xyz;
			if ((sl.flags & GL_DIRECTIONAL) == 0)
				L = LightVector(sl.vecCols, pos);
			// Normalized, then for a specular light with the view direction added and normalized again.
			[loop] for (int n = 0; n < 2; ++n) {
				GENormalize3(L, GEDot3(L, L));
				if (n == 1 || (sl.flags & GL_SPECULAR) == 0)
					break;
				L = AddViewDir(ti, L);
			}
			st[k] = GEAddFloat24(GENormalDot(L, worldnormal, normalRsqrt), 1.0f) * 0.5f;
		}
		vertex.s = st[0];
		vertex.t = st[1];
	}

	if ((f & GT_LIGHTING) != 0)
		ProcessLighting(ti, vertex, pos, worldnormal, normalRsqrt);
	return vertex;
}

[numthreads(64, 1, 1)]
void CSVertex(uint3 id : SV_DispatchThreadID) {
	if (id.x >= gCount)
		return;
	uint i = gBase + id.x;
	GeVertexIn sv = gInputs[i];
	GeDraw d = gDraws[sv.draw];
	gClipOut[i] = ReadVertex(d.transform, d, sv);
}
)HLSL";

static const char kGeometry4[] = R"HLSL(
// --- Clipper (transform.cpp) ---

bool OutsideRange(ClipV v) { return v.x == OUTSIDE_X; }
bool BehindNear(float4 c) { return c.z < Neg(c.w); }

int OutsideMask(float4 p) {
	int mask = 0;
	if (p.x > p.w) mask |= 1;
	if (Neg(p.x) > p.w) mask |= 2;
	if (p.y > p.w) mask |= 4;
	if (Neg(p.y) > p.w) mask |= 8;
	if (p.z > p.w) mask |= 16;
	if (Neg(p.z) > p.w) mask |= 32;
	return mask;
}

bool OutsideRangeBeforeClip(ClipV v, bool depthClip) {
	return OutsideRange(v) && !(depthClip && BehindNear(v.clip));
}

// NearPoint: where the edge from inV to outV crosses the near plane, as the GE computes it.
ClipV NearPoint(uint ti, ClipV inV, ClipV outV) {
	ClipV dest = (ClipV)0;
	float dIn = GEAdd(inV.clip.z, inV.clip.w);
	float dOut = GEAdd(outV.clip.z, outV.clip.w);
	float t = ProductTo24(Trunc24(dIn), GERecip(Trunc24(GEAdd(dIn, Neg(dOut)))));
	// LerpVertex's colors and fog, with t rounded to 1/256.
	precise float tt = t * 256.0f + 0.5f;
	int ti256 = CvtTrunc(tt);
	int fa = CvtTrunc(inV.fog * 256.0f), fb = CvtTrunc(outV.fog * 256.0f);
	dest.fog = (float)((fa * (256 - ti256) + fb * ti256) >> 8) * (1.0f / 256.0f);
	uint c0 = 0, c1 = 0;
	[unroll] for (int i = 0; i < 4; ++i) {
		int ca = (int)((inV.color0 >> (8 * i)) & 0xFFu), cb = (int)((outV.color0 >> (8 * i)) & 0xFFu);
		c0 |= (uint)((ca * (256 - ti256) + cb * ti256) / 256) << (8 * i);
		if (i < 3) {
			int sa = (int)((inV.color1 >> (8 * i)) & 0xFFu), sb = (int)((outV.color1 >> (8 * i)) & 0xFFu);
			c1 |= (uint)((sa * (256 - ti256) + sb * ti256) / 256) << (8 * i);
		}
	}
	dest.color0 = c0;
	dest.color1 = c1;
	// The clip coordinates, then the texture coordinates, through the same arithmetic.
	float a[7] = { inV.clip.x, inV.clip.y, inV.clip.z, inV.clip.w, inV.s, inV.t, inV.q };
	float b[7] = { outV.clip.x, outV.clip.y, outV.clip.z, outV.clip.w, outV.s, outV.t, outV.q };
	float res[7];
	[loop] for (int k = 0; k < 7; ++k)
		res[k] = Trunc24(GEAdd(ProductTo24(t, Trunc24(GEAdd(b[k], Neg(a[k])))), a[k]));
	dest.clip = float4(res[0], res[1], res[2], res[3]);
	dest.s = res[4];
	dest.t = res[5];
	dest.q = res[6];
	bool outside = false;
	ClipToScreen(ti, dest, true, outside);
	dest.clipw = dest.clip.w;
	if (outside)
		dest.x = OUTSIDE_X;
	return dest;
}

// --- The binner's winding test and DrawData::EmitTriangle ---

// a * b as a signed 64-bit (lo, hi) value.
uint2 MulS64(int a, int b) {
	uint2 p = Mul32x32((uint)abs(a), (uint)abs(b));
	if ((a < 0) != (b < 0)) {
		p = uint2(~p.x, ~p.y);
		p.x += 1u;
		if (p.x == 0)
			p.y += 1u;
	}
	return p;
}

// d01x * d02y - d01y * d02x > 0 (binner::AddTriangle), for vertices with x and y.
bool CounterClockwise(int3 x, int3 y) {
	uint2 a = MulS64(x.x - x.y, y.x - y.z), b = MulS64(y.x - y.y, x.x - x.z);
	uint lo = a.x - b.x;
	uint hi = a.y - b.y - (a.x < b.x ? 1u : 0u);
	return (hi & SIGN_BIT) == 0 && (hi != 0 || lo != 0);
}

float ClampFogDepth(float fogdepth) {
	uint u = asuint(fogdepth);
	uint e = u >> 23;
	if ((u & SIGN_BIT) != 0 || e <= 126u - 8u)
		return 0.0f;
	if (e > 126u)
		return 255.0f;
	uint mantissa = (u & 0x7FFFFFu) | 0x800000u;
	return (float)((mantissa >> (16u + 126u - e)) & 0xFFu);
}

struct GpuV { float x; float y; float z; float fog; uint c0; uint c1; float s; float t; float q; uint state; float lod; };

void StoreVertex(uint index, GpuV g) {
	uint a = index * 44u;
	gOut.Store4(a, uint4(asuint(g.x), asuint(g.y), asuint(g.z), asuint(g.fog)));
	gOut.Store4(a + 16u, uint4(g.c0, g.c1, asuint(g.s), asuint(g.t)));
	gOut.Store3(a + 32u, uint3(asuint(g.q), g.state, asuint(g.lod)));
}

// One of EmitTriangle's vertices, pv the provoking one (flat shading).
GpuV MakeGpuVertex(ClipV v, ClipV pv, GeDraw d) {
	GpuV g = (GpuV)0;
	bool flat = (d.flags & GD_STATE_GOURAUD) == 0;
	g.x = (float)v.x * (1.0f / 16.0f);
	g.y = (float)v.y * (1.0f / 16.0f);
	g.z = (float)v.z;
	g.fog = (d.flags & GD_FOG) != 0 ? ClampFogDepth(v.fog) : 255.0f;
	g.c0 = flat ? pv.color0 : v.color0;
	g.c1 = flat ? pv.color1 : v.color1;
	g.q = 1.0f;
	if ((d.flags & GD_TEXTURED) != 0) {
		// TexCoordsPerspective.
		bool proj = (d.flags & GD_PROJ) != 0;
		float w24 = Trunc24(v.clipw);
		if (w24 > 0.0f && IsFiniteF(w24)) {
			float r = GERecip(w24);
			precise float s = v.s * r;
			precise float t = v.t * r;
			precise float q = (proj ? v.q : 1.0f) * r;
			g.s = s;
			g.t = t;
			g.q = q;
		} else {
#if DOUBLES
			double r = v.clipw != 0.0f ? 1.0 / (double)v.clipw : 1.0;
			g.s = (float)((double)v.s * r);
			g.t = (float)((double)v.t * r);
			g.q = (float)((double)(proj ? v.q : 1.0f) * r);
#else
			precise float r = v.clipw != 0.0f ? 1.0f / v.clipw : 1.0f;
			precise float s = v.s * r;
			precise float t = v.t * r;
			precise float q = (proj ? v.q : 1.0f) * r;
			g.s = s;
			g.t = t;
			g.q = q;
#endif
		}
	}
	g.state = d.state;
	return g;
}

// draw_data.cpp TriangleLodImpl.
float TriangleLod(GpuV v0, GpuV v1, GpuV v2, uint texShift) {
#if DOUBLES
	double x1 = (double)(v1.x - v0.x), y1 = (double)(v1.y - v0.y), x2 = (double)(v2.x - v0.x), y2 = (double)(v2.y - v0.y);
	double det = x1 * y2 - x2 * y1;
	if (det == 0.0)
		return 0.0f;
	// (Integers go through float: exact, and doubles only convert from floats.)
	double d1 = (double)v1.s - (double)v0.s, d2 = (double)v2.s - (double)v0.s;
	double gs = max(abs((d1 * y2 - d2 * y1) / det), abs((d2 * x1 - d1 * x2) / det)) * (double)(float)(1u << (texShift & 0xFFu));
	d1 = (double)v1.t - (double)v0.t;
	d2 = (double)v2.t - (double)v0.t;
	double gt = max(abs((d1 * y2 - d2 * y1) / det), abs((d2 * x1 - d1 * x2) / det)) * (double)(float)(1u << ((texShift >> 8) & 0xFFu));
	return (float)(max(gs, gt) / (double)(float)gScale);
#else
	precise float x1 = v1.x - v0.x, y1 = v1.y - v0.y, x2 = v2.x - v0.x, y2 = v2.y - v0.y;
	precise float det = x1 * y2 - x2 * y1;
	if (det == 0.0f)
		return 0.0f;
	precise float d1 = v1.s - v0.s, d2 = v2.s - v0.s;
	precise float gs = max(abs((d1 * y2 - d2 * y1) / det), abs((d2 * x1 - d1 * x2) / det)) * (float)(1u << (texShift & 0xFFu));
	d1 = v1.t - v0.t;
	d2 = v2.t - v0.t;
	precise float gt = max(abs((d1 * y2 - d2 * y1) / det), abs((d2 * x1 - d1 * x2) / det)) * (float)(1u << ((texShift >> 8) & 0xFFu));
	return max(gs, gt) / (float)gScale;
#endif
}

// ProcessTriangle of transform mode, then for each triangle it makes AddTriangleFlat, the winding test
// and EmitTriangle: the job's two triangles (unmade or dropped ones degenerate).
[numthreads(64, 1, 1)]
void CSTriangle(uint3 id : SV_DispatchThreadID) {
	if (id.x >= gCount)
		return;
	GeJob job = gJobs[id.x];
	GeDraw d = gDraws[job.draw];
	uint ti = d.transform;
	// The job's vertices, then those clipping makes.
	ClipV verts[5];
	verts[0] = gClipIn[job.v.x];
	verts[1] = gClipIn[job.v.y];
	verts[2] = gClipIn[job.v.z];
	verts[3] = verts[0];
	verts[4] = verts[0];
	ClipV provoking = gClipIn[job.provoking];
	bool depthClip = (d.flags & GD_DEPTH_CLIP) != 0;
	bool behind[3] = { BehindNear(verts[0].clip), BehindNear(verts[1].clip), BehindNear(verts[2].clip) };
	int numTris = 0;
	int tri[6] = { 0, 1, 2, 0, 0, 0 };
	int made = 0;
	int edgeIn[2] = { 0, 0 }, edgeOut[2] = { 0, 0 };
	bool drop = OutsideRangeBeforeClip(verts[0], depthClip) || OutsideRangeBeforeClip(verts[1], depthClip) ||
		OutsideRangeBeforeClip(verts[2], depthClip) || (verts[0].clip.w < 0.0f && verts[1].clip.w < 0.0f && verts[2].clip.w < 0.0f) ||
		(OutsideMask(verts[0].clip) & OutsideMask(verts[1].clip) & OutsideMask(verts[2].clip)) != 0;
	bool clipNear = behind[0] || behind[1] || behind[2];
	// With depth clip off, the GE doesn't clip at the near plane; a vertex behind the camera culls.
	if (!drop && !depthClip) {
		drop = !(verts[0].clip.w > 0.0f && verts[1].clip.w > 0.0f && verts[2].clip.w > 0.0f);
		clipNear = false;
	}
	if (!drop) {
		if (!clipNear) {
			numTris = 1;
		} else {
			// With one vertex outside (o), the quad is split from the vertex before it (p) in the
			// submitted order.
			int numOutside = (behind[0] ? 1 : 0) + (behind[1] ? 1 : 0) + (behind[2] ? 1 : 0);
			bool reversed = job.reversed != 0;
			if (numOutside == 1) {
				int o = behind[0] ? 0 : (behind[1] ? 1 : 2);
				int p = reversed ? (o + 1) % 3 : (o + 2) % 3;
				int n = reversed ? (o + 2) % 3 : (o + 1) % 3;
				edgeIn[0] = p;
				edgeOut[0] = o;
				edgeIn[1] = n;
				edgeOut[1] = o;
				made = 2;
				// (p, made0, made1) and (p, made1, n), the last two swapped when reversed.
				tri[0] = p;
				tri[1] = reversed ? 4 : 3;
				tri[2] = reversed ? 3 : 4;
				tri[3] = p;
				tri[4] = reversed ? n : 4;
				tri[5] = reversed ? 4 : n;
				numTris = 2;
			} else if (numOutside == 2) {
				int i = !behind[0] ? 0 : (!behind[1] ? 1 : 2);
				edgeIn[0] = i;
				edgeOut[0] = (i + 1) % 3;
				edgeIn[1] = i;
				edgeOut[1] = (i + 2) % 3;
				made = 2;
				tri[0] = i;
				tri[1] = 3;
				tri[2] = 4;
				numTris = 1;
			}
		}
	}
	[loop] for (int k = 0; k < made; ++k)
		verts[3 + k] = NearPoint(ti, verts[edgeIn[k]], verts[edgeOut[k]]);

	[loop] for (int t = 0; t < 2; ++t) {
		ClipV tv[3] = { verts[tri[t * 3]], verts[tri[t * 3 + 1]], verts[tri[t * 3 + 2]] };
		// AddTriangleFlat: flat shading gives the last vertex the provoking colors.
		if ((d.flags & GD_GOURAUD) == 0) {
			tv[2].color0 = provoking.color0;
			tv[2].color1 = provoking.color1;
		}
		bool emit = t < numTris && !OutsideRange(tv[0]) && !OutsideRange(tv[1]) && !OutsideRange(tv[2]) &&
			CounterClockwise(int3(tv[0].x, tv[1].x, tv[2].x), int3(tv[0].y, tv[1].y, tv[2].y));
		GpuV g[3];
		[loop] for (int k = 0; k < 3; ++k)
			g[k] = MakeGpuVertex(tv[k], tv[2], d);
		float lod = 0.0f;
		if (emit && (d.flags & GD_TEXTURED) != 0)
			lod = TriangleLod(g[0], g[1], g[2], d.texShift);
		[loop] for (int m = 0; m < 3; ++m) {
			GpuV o = g[m];
			if (emit) {
				o.lod = lod;
			} else {
				o = (GpuV)0;
				o.state = d.state;
			}
			StoreVertex(job.outBase + (uint)(t * 3 + m), o);
		}
	}
}
)HLSL";

std::string GeometryShaderSource() {
	std::string tables = "static const int2 kRecip[128] = {";
	char buf[48];
	for (int i = 0; i < 128; ++i) {
		snprintf(buf, sizeof(buf), "%sint2(%d, %d)", i ? ", " : "", geRecipSegments[i].b, geRecipSegments[i].m);
		tables += buf;
	}
	tables += "};\nstatic const int2 kRsqrt[256] = {";
	for (int i = 0; i < 256; ++i) {
		const GERecipSegment &s = geRsqrtSegments[i >> 7][i & 127];
		snprintf(buf, sizeof(buf), "%sint2(%d, %d)", i ? ", " : "", s.b, s.m);
		tables += buf;
	}
	tables += "};\n";
	std::string src = std::string(kGeometry1) + kGeometry2 + kGeometry3 + kGeometry4;
	src.replace(src.find("TABLES"), 6, tables);
	return src;
}

}  // namespace gpu::hw
