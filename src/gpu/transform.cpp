// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Vertex transform, lighting, near-plane clipping and primitive assembly, in the GE's arithmetic.
// Port of GPU/Software/TransformUnit.cpp, Lighting.cpp and Clipper.cpp (scalar).
#include <algorithm>
#include <cmath>
#include <vector>

#include "gpu/ge_state.h"
#include "gpu/render.h"
#include "gpu/vertex.h"

namespace gpu {

namespace {

struct ClipVertex {
	float clip[4];
	RVertex v;
	bool OutsideRange() const { return v.x == 0x7FFFFFFF; }
};

// Light color factors are 2c + 1 per channel (the GE's half-offset rounding).
struct Color4i {
	int c[4];
};

static inline Color4i ColorFactor(u32 rgba) {
	Color4i r;
	for (int i = 0; i < 4; ++i)
		r.c[i] = (int)((rgba >> (8 * i)) & 0xFF) * 2 + 1;
	return r;
}

static inline bool IsLargerThanHalf(const Color4i &v) {
	return v.c[0] > 1 || v.c[1] > 1 || v.c[2] > 1;
}

static inline Color4i LightProduct(const Color4i &l, const Color4i &m) {
	Color4i r;
	for (int i = 0; i < 4; ++i)
		r.c[i] = (l.c[i] * m.c[i]) >> 10;
	return r;
}

static inline Color4i LightScale(const Color4i &x, float f) {
	const int s = std::clamp((int)(256.0f * f), 0, 256);
	Color4i r;
	for (int i = 0; i < 4; ++i)
		r.c[i] = ((x.c[i] * 2 + 1) * (2 * s + 1)) >> 10;
	return r;
}

static inline u32 ToRGBA(const int c[4]) {
	u32 r = 0;
	for (int i = 0; i < 4; ++i)
		r |= (u32)std::clamp(c[i], 0, 255) << (8 * i);
	return r;
}

struct LightState {
	struct Light {
		float pos[3];
		float translated[3];
		float att[3];
		float spotDir[3];
		float spotDirRsqrt;
		float spotCutoff;
		float spotExp;
		Color4i ambientColorFactor, diffuseColorFactor, specularColorFactor;
		bool enabled, spot, directional, poweredDiffuse, ambient, diffuse, specular;
	} lights[4];
	Color4i materialAmbient, materialDiffuse, materialSpecular;
	Color4i baseAmbient;
	float specularExp;
	float viewDir[3];
	bool colorForAmbient, colorForDiffuse, colorForSpecular;
	bool setColor1, addColor1, usesWorldNormal;
};

struct TransformState {
	LightState light;
	float matrix[16];
	float viewZRows[4];
	float posToFog[4];
	bool fogGE;
	float fogEnd, fogSlope;
	float screenScale[3], screenAdd[3];
	float tgen[12];
	bool enableTransform, enableLighting, enableFog, negateNormals, depthClip;
	int uvGenMode;
	bool geUVScale;
	bool uvPrescale;  // UV gen mode 3: the decoder's float scale and offset
	float uvScale[2], uvOffset[2];
	float uvScaleF[4];
	int offsetX16, offsetY16;
};

TransformState ts;
bool tsDirty = true;
u32 tsVType = 0xFFFFFFFF;
VertexFormat vfmt;

struct Carry {
	float uv[3] = {};
	float normal[3] = {};
} carry;

// Primitive assembly (continuation across PRIM commands with KEEP_PREVIOUS).
ClipVertex data[4];
int dataIndex = 0;
GEPrimitiveType prevPrim = GE_PRIM_POINTS;
bool isImmDraw = false;
bool cullXY = true;

std::vector<ClipVertex> cache;
std::vector<SimpleVertex> simple;

static inline float Light24(u32 v) {
	return getFloat24(v);
}

void ComputeLightState(LightState *state, bool hasColor0) {
	bool anyAmbient = false, anyDiffuse = false, anySpecular = false, anyNonDirectional = false;
	for (int l = 0; l < 4; ++l) {
		auto &ls = state->lights[l];
		ls.enabled = gstate.isLightChanEnabled(l);
		if (!ls.enabled)
			continue;
		ls.poweredDiffuse = gstate.isUsingPoweredDiffuseLight(l);
		ls.specular = gstate.isUsingSpecularLight(l);
		ls.ambientColorFactor = ColorFactor(gstate.getLightAmbientColor(l));
		ls.ambient = IsLargerThanHalf(ls.ambientColorFactor);
		anyAmbient = anyAmbient || ls.ambient;
		ls.diffuseColorFactor = ColorFactor(gstate.getDiffuseColor(l));
		ls.diffuse = IsLargerThanHalf(ls.diffuseColorFactor);
		anyDiffuse = anyDiffuse || ls.diffuse;
		if (ls.specular) {
			ls.specularColorFactor = ColorFactor(gstate.getSpecularColor(l));
			ls.specular = IsLargerThanHalf(ls.specularColorFactor);
			anySpecular = anySpecular || ls.specular;
		}
		if (!ls.specular && !ls.ambient && !ls.diffuse) {
			ls.enabled = false;
			continue;
		}
		for (int i = 0; i < 3; ++i)
			ls.pos[i] = Light24(gstate.lpos[3 * l + i]);
		ls.directional = gstate.isDirectionalLight(l);
		if (ls.directional) {
			GENormalize3(ls.pos);
		} else {
			for (int i = 0; i < 3; ++i) {
				ls.att[i] = Light24(gstate.latt[3 * l + i]);
				// The light position minus the world translation, with the GE's adder.
				ls.translated[i] = GEAddFloat24(ls.pos[i], -TruncateToFloat24(gstate.worldMatrix[9 + i]));
			}
			anyNonDirectional = true;
		}
		ls.spot = gstate.isSpotLight(l);
		if (ls.spot) {
			for (int i = 0; i < 3; ++i)
				ls.spotDir[i] = Light24(gstate.ldir[3 * l + i]);
			if (!std::isfinite(ls.spotDir[0]) || !std::isfinite(ls.spotDir[1]) || !std::isfinite(ls.spotDir[2])) {
				for (int i = 0; i < 3; ++i)
					ls.spotDir[i] = std::isfinite(ls.spotDir[i]) ? 0.0f : (std::signbit(ls.spotDir[i]) ? -1.0f : 1.0f);
			}
			const float len2 = GEDot3(ls.spotDir, ls.spotDir);
			ls.spotDirRsqrt = len2 > 0.0f && std::isfinite(len2) ? GERsqrt(len2) : 0.0f;
			ls.spotCutoff = getFloat24(gstate.lcutoff[l]);
			if (std::isnan(ls.spotCutoff) && std::signbit(ls.spotCutoff))
				ls.spotCutoff = 0.0f;
			ls.spotExp = PSPLightExponent(getFloat24(gstate.lconv[l]));
			if (ls.spotExp <= 0.0f)
				ls.spotExp = 0.0f;
		}
	}

	const int materialupdate = gstate.materialupdate & (hasColor0 ? 7 : 0);
	state->colorForAmbient = (materialupdate & 1) != 0;
	state->colorForDiffuse = (materialupdate & 2) != 0;
	state->colorForSpecular = (materialupdate & 4) != 0;

	if (!state->colorForAmbient) {
		state->materialAmbient = ColorFactor(gstate.getMaterialAmbientRGBA());
		if (!IsLargerThanHalf(state->materialAmbient) && anyAmbient) {
			for (int i = 0; i < 4; ++i)
				state->lights[i].ambient = false;
		}
	}
	if (anyDiffuse && !state->colorForDiffuse) {
		state->materialDiffuse = ColorFactor(gstate.getMaterialDiffuse());
		if (!IsLargerThanHalf(state->materialDiffuse)) {
			anyDiffuse = false;
			for (int i = 0; i < 4; ++i)
				state->lights[i].diffuse = false;
		}
	}
	if (anySpecular && !state->colorForSpecular) {
		state->materialSpecular = ColorFactor(gstate.getMaterialSpecular());
		if (!IsLargerThanHalf(state->materialSpecular)) {
			anySpecular = false;
			for (int i = 0; i < 4; ++i)
				state->lights[i].specular = false;
		}
	}
	state->specularExp = 0.0f;
	if (anyDiffuse || anySpecular) {
		state->specularExp = PSPLightExponent(gstate.getMaterialSpecularCoef());
		if (state->specularExp <= 0.0f)
			state->specularExp = 0.0f;
	}
	state->baseAmbient = ColorFactor(gstate.getAmbientRGBA());
	state->setColor1 = gstate.isUsingSecondaryColor() && anySpecular;
	state->addColor1 = !gstate.isUsingSecondaryColor() && anySpecular;
	state->usesWorldNormal = gstate.getUVGenMode() == GE_TEXMAP_ENVIRONMENT_MAP || anyDiffuse || anySpecular;
	(void)anyNonDirectional;
}

static inline void Convert4x3To4x4(float out[16], const float in[12]) {
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 3; ++c)
			out[r * 4 + c] = in[r * 3 + c];
		out[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
	}
}

static inline bool IsNanOrInf(float f) {
	return !std::isfinite(f);
}

void ComputeTransformState(bool through, bool hasColor0) {
	TransformState &s = ts;
	s.enableTransform = !through;
	s.enableLighting = gstate.isLightingEnabled();
	s.enableFog = gstate.isFogEnabled();
	s.depthClip = gstate.isDepthClipEnabled();
	s.geUVScale = !through && gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_COORDS && !gstate.isModeClear() && gstate.isTextureMapEnabled();
	s.uvPrescale = !through && gstate.getUVGenMode() == GE_TEXMAP_UNKNOWN;
	s.uvScale[0] = TruncateToFloat24(getFloat24(gstate.texscaleu));
	s.uvScale[1] = TruncateToFloat24(getFloat24(gstate.texscalev));
	s.uvOffset[0] = TruncateToFloat24(getFloat24(gstate.texoffsetu));
	s.uvOffset[1] = TruncateToFloat24(getFloat24(gstate.texoffsetv));
	s.uvScaleF[0] = getFloat24(gstate.texscaleu);
	s.uvScaleF[1] = getFloat24(gstate.texscalev);
	s.uvScaleF[2] = getFloat24(gstate.texoffsetu);
	s.uvScaleF[3] = getFloat24(gstate.texoffsetv);
	s.negateNormals = gstate.areNormalsReversed();
	s.uvGenMode = gstate.getUVGenMode();
	if (s.uvGenMode == GE_TEXMAP_UNKNOWN)
		s.uvGenMode = GE_TEXMAP_TEXTURE_COORDS;
	s.offsetX16 = gstate.getOffsetX16();
	s.offsetY16 = gstate.getOffsetY16();

	if (!s.enableTransform)
		return;

	if (s.enableLighting)
		ComputeLightState(&s.light, hasColor0);
	else
		s.light.usesWorldNormal = s.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP;

	float viewDir[3] = { gstate.viewMatrix[2], gstate.viewMatrix[5], gstate.viewMatrix[8] };
	if (GENormalize3(viewDir) == 0.0f) {
		viewDir[0] = viewDir[1] = 0.0f;
		viewDir[2] = 1.0f;
	}
	memcpy(s.light.viewDir, viewDir, sizeof(viewDir));

	memcpy(s.tgen, gstate.tgenMatrix, sizeof(s.tgen));

	float world[16], view[16], worldview[16];
	Convert4x3To4x4(view, gstate.viewMatrix);
	Convert4x3To4x4(world, gstate.worldMatrix);
	GECombineMatrices(worldview, world, view);
	GECombineMatrices(s.matrix, worldview, gstate.projMatrix);

	if (s.enableFog) {
		float fogEnd = getFloat24(gstate.fog1);
		float fogSlope = getFloat24(gstate.fog2);
		s.posToFog[0] = worldview[2];
		s.posToFog[1] = worldview[6];
		s.posToFog[2] = worldview[10];
		s.posToFog[3] = worldview[14] + fogEnd;
		s.fogGE = !IsNanOrInf(fogEnd) && !IsNanOrInf(fogSlope);
		for (int i = 0; i < 4; ++i)
			s.viewZRows[i] = worldview[2 + 4 * i];
		s.fogEnd = TruncateToFloat24(fogEnd);
		s.fogSlope = TruncateToFloat24(fogSlope);
		if (IsNanOrInf(fogEnd)) {
			bool sign = std::signbit(fogEnd);
			if (std::signbit(fogSlope))
				sign = !sign;
			if (fogSlope == 0.0f)
				sign = true;
			const float forced = sign ? 0.0f : 1.0f;
			s.posToFog[0] = s.posToFog[1] = s.posToFog[2] = 0.0f;
			s.posToFog[3] = forced;
		} else if (IsNanOrInf(fogSlope)) {
			fogSlope = std::signbit(fogSlope) ? -262144.0f : 262144.0f;
			for (int i = 0; i < 4; ++i)
				s.posToFog[i] *= fogSlope;
		} else {
			for (int i = 0; i < 4; ++i)
				s.posToFog[i] *= fogSlope;
		}
	}

	s.screenScale[0] = gstate.getViewportXScale();
	s.screenScale[1] = gstate.getViewportYScale();
	s.screenScale[2] = gstate.getViewportZScale();
	s.screenAdd[0] = gstate.getViewportXCenter();
	s.screenAdd[1] = gstate.getViewportYCenter();
	s.screenAdd[2] = gstate.getViewportZCenter();
}

void EnsureTransformState(bool through, bool hasColor0, u32 vtype) {
	// The lighting state depends on the vertex having a color (material update).
	const u32 key = vtype ^ (hasColor0 ? 0x80000000 : 0);
	if (tsDirty || key != tsVType) {
		ComputeTransformState(through, hasColor0);
		tsDirty = false;
		tsVType = key;
	}
}

constexpr float SCREEN_BOUND = 4095.0f + (15.5f / 16.0f);

// alwaysCheckRange: for vertices made by clipping (no near-plane exemption).
static inline void RoundToScreen(RVertex &v, const float scaled[3], const float clip[4], bool depthClamp, bool alwaysCheckRange, bool *outside) {
	float z = scaled[2];
	if (depthClamp) {
		if ((alwaysCheckRange || !(clip[2] < -clip[3])) && !(scaled[0] < SCREEN_BOUND && scaled[1] < SCREEN_BOUND && scaled[0] >= 0 && scaled[1] >= 0))
			*outside = true;
		if (z < 0.0f)
			z = 0.0f;
		else if (z > 65535.0f)
			z = 65535.0f;
	} else if (!(scaled[0] <= SCREEN_BOUND && scaled[1] < SCREEN_BOUND && scaled[0] >= 0 && scaled[1] >= 0 && z >= 0.0f && z < 65536.0f)) {
		*outside = true;
	}
	v.x = (int)floorf(scaled[0] * 16.0f) - ts.offsetX16;
	v.y = (int)floorf(scaled[1] * 16.0f) - ts.offsetY16;
	v.z = (z >= 0.0f && z < 65536.0f) ? (int)(u16)(u32)z : 0;
}

static inline void ClipToScreen(RVertex &v, const float clip[4], bool alwaysCheck, bool *outside) {
	float scaled[3];
	for (int i = 0; i < 2; ++i)
		scaled[i] = GEViewport(clip[i], clip[3], ts.screenScale[i], ts.screenAdd[i]);
	scaled[2] = GEScreenZ(clip[2], clip[3], ts.screenScale[2], ts.screenAdd[2]);
	RoundToScreen(v, scaled, clip, ts.depthClip, alwaysCheck, outside);
}

// v . N for the unnormalized N, scaled by its reciprocal length.
static inline float GENormalDot(const float *v, const float *n, float nRsqrt) {
	return ProductToFloat24((double)GEDot3(v, n) * nRsqrt);
}

// The vector from a vertex to a point light (gpu/probe exp153-158).
static inline void GELightVector(float out[3], const float translated[3], const float modelpos[4]) {
	const float *m = gstate.worldMatrix;
	for (int c = 0; c < 3; ++c) {
		const float b[4] = { -m[c], -m[3 + c], -m[6 + c], translated[c] };
		out[c] = GERow(modelpos, b, 4);
	}
}

static float GEShadeMapCoord(int l, const float modelpos[4], const float worldnormal[3], float normalRsqrt, const float viewDir[3]) {
	float L[3];
	for (int i = 0; i < 3; ++i)
		L[i] = getFloat24(gstate.lpos[l * 3 + i]);
	if (gstate.getLightType(l) != GE_LIGHTTYPE_DIRECTIONAL) {
		float tr[3];
		for (int i = 0; i < 3; ++i)
			tr[i] = GEAddFloat24(L[i], -TruncateToFloat24(gstate.worldMatrix[9 + i]));
		GELightVector(L, tr, modelpos);
	}
	GENormalize3(L);
	if (gstate.isUsingSpecularLight(l)) {
		for (int i = 0; i < 3; ++i)
			L[i] = GEAddFloat24(L[i], viewDir[i]);
		GENormalize3(L);
	}
	return GEAddFloat24(GENormalDot(L, worldnormal, normalRsqrt), 1.0f) * 0.5f;
}

void ProcessLighting(RVertex &vertex, const float modelpos[4], const float worldnormal[3], float normalRsqrt, const LightState &state) {
	static const float zAxis[3] = { 0.0f, 0.0f, 1.0f };
	Color4i colorFactor{};
	if (state.colorForAmbient || state.colorForDiffuse || state.colorForSpecular)
		colorFactor = ColorFactor(vertex.color0);

	const u32 mecv = gstate.getMaterialEmissive();
	const Color4i mac = state.colorForAmbient ? colorFactor : state.materialAmbient;
	int final_color[4];
	for (int i = 0; i < 4; ++i)
		final_color[i] = (int)((mecv >> (8 * i)) & 0xFF) * (i < 3 ? 1 : 0) + ((mac.c[i] * state.baseAmbient.c[i]) >> 10);
	int specular_color[4] = { 0, 0, 0, 0 };

	for (int l = 0; l < 4; ++l) {
		const auto &ls = state.lights[l];
		if (!ls.enabled)
			continue;
		float L[3] = { ls.pos[0], ls.pos[1], ls.pos[2] };
		float att = 1.0f, spot = 1.0f;
		if (!ls.directional) {
			GELightVector(L, ls.translated, modelpos);
			const float d2 = GEDot3(L, L);
			const float d = GENormalize3(L, d2);
			if (d == 0.0f)
				memcpy(L, zAxis, sizeof(L));
			const float factors[3] = { 1.0f, d, d2 };
			const float den = GEDot3(ls.att, factors);
			att = den > 0.0f ? GERecip(den) : 0.0f;
			if (!(att > 0.0f))
				att = 0.0f;
			else if (att > 1.0f)
				att = 1.0f;
		}
		if (ls.spot) {
			float rawSpot = ProductToFloat24((double)GEDot3(ls.spotDir, L) * ls.spotDirRsqrt);
			if (std::isnan(rawSpot))
				rawSpot = std::signbit(rawSpot) ? 0.0f : 1.0f;
			if (rawSpot >= ls.spotCutoff) {
				spot = GELightPow(rawSpot, ls.spotExp);
				if (std::isnan(spot))
					spot = 0.0f;
			} else {
				spot = 0.0f;
			}
		}
		auto scaleAttSpot = [&](Color4i c) {
			if (att < 1.0f)
				c = LightScale(c, att);
			if (spot < 1.0f)
				c = LightScale(c, spot);
			return c;
		};
		if (ls.ambient) {
			const Color4i la = scaleAttSpot(LightProduct(ls.ambientColorFactor, mac));
			for (int i = 0; i < 4; ++i)
				final_color[i] += la.c[i];
		}
		float diffuse_factor = 0.0f;
		if (ls.diffuse || ls.specular) {
			diffuse_factor = GENormalDot(L, worldnormal, normalRsqrt);
			if (ls.poweredDiffuse)
				diffuse_factor = GELightPow(diffuse_factor, state.specularExp);
		}
		if (ls.diffuse) {
			const Color4i mdc = state.colorForDiffuse ? colorFactor : state.materialDiffuse;
			const Color4i ld = scaleAttSpot(LightScale(LightProduct(ls.diffuseColorFactor, mdc), diffuse_factor));
			for (int i = 0; i < 4; ++i)
				final_color[i] += ld.c[i];
		}
		if (ls.specular && diffuse_factor >= 0.0f) {
			float H[3];
			for (int i = 0; i < 3; ++i)
				H[i] = GEAddFloat24(L[i], state.viewDir[i]);
			if (GENormalize3(H) == 0.0f)
				memcpy(H, zAxis, sizeof(H));
			float specular_factor = GENormalDot(H, worldnormal, normalRsqrt);
			specular_factor = GELightPow(specular_factor, state.specularExp);
			if (specular_factor > 0.0f) {
				const Color4i msc = state.colorForSpecular ? colorFactor : state.materialSpecular;
				const Color4i lsp = scaleAttSpot(LightScale(LightProduct(ls.specularColorFactor, msc), specular_factor));
				for (int i = 0; i < 4; ++i)
					specular_color[i] += lsp.c[i];
			}
		}
	}

	if (state.setColor1) {
		vertex.color0 = ToRGBA(final_color);
		specular_color[3] = 0;
		vertex.color1 = ToRGBA(specular_color) & 0x00FFFFFF;
	} else if (state.addColor1) {
		int sum[4];
		for (int i = 0; i < 4; ++i)
			sum[i] = final_color[i] + specular_color[i];
		vertex.color0 = ToRGBA(sum);
	} else {
		vertex.color0 = ToRGBA(final_color);
	}
}

// One vertex through the pipeline. The SimpleVertex has morph and skinning applied.
ClipVertex ReadVertex(const SimpleVertex &sv, bool hasUV, bool hasNormal, bool hasColor, Carry &cr) {
	ClipVertex vertex;
	const float pos[4] = { sv.pos[0], sv.pos[1], sv.pos[2], 1.0f };

	if (hasUV) {
		cr.uv[0] = sv.uv[0];
		cr.uv[1] = sv.uv[1];
		if (ts.uvPrescale) {
			cr.uv[0] = cr.uv[0] * ts.uvScaleF[0] + ts.uvScaleF[2];
			cr.uv[1] = cr.uv[1] * ts.uvScaleF[1] + ts.uvScaleF[3];
		}
		cr.uv[2] = 0.0f;
	}
	vertex.v.s = cr.uv[0];
	vertex.v.t = cr.uv[1];
	vertex.v.q = cr.uv[2];
	if (ts.geUVScale) {
		float *tc[2] = { &vertex.v.s, &vertex.v.t };
		for (int i = 0; i < 2; ++i) {
			const float scaled = ProductToFloat24((double)TruncateToFloat24(*tc[i]) * ts.uvScale[i]);
			*tc[i] = TruncateToFloat24(GEAdd(scaled, ts.uvOffset[i]));
		}
	}

	if (hasNormal)
		memcpy(cr.normal, sv.nrm, sizeof(cr.normal));
	float normal[3] = { cr.normal[0], cr.normal[1], cr.normal[2] };
	if (ts.negateNormals) {
		for (int i = 0; i < 3; ++i)
			normal[i] = BitsFloat(FloatBits(normal[i]) ^ 0x80000000);
	}

	vertex.v.color0 = hasColor ? sv.color : gstate.getMaterialAmbientRGBA();
	vertex.v.color1 = 0;

	if (ts.enableTransform) {
		for (int c = 0; c < 4; ++c) {
			const float b[4] = { ts.matrix[c], ts.matrix[4 + c], ts.matrix[8 + c], ts.matrix[12 + c] };
			vertex.clip[c] = GERow(pos, b, 4);
		}
		float scaled[3];
		for (int i = 0; i < 2; ++i)
			scaled[i] = GEViewport(vertex.clip[i], vertex.clip[3], ts.screenScale[i], ts.screenAdd[i]);
		scaled[2] = GEScreenZ(vertex.clip[2], vertex.clip[3], ts.screenScale[2], ts.screenAdd[2]);
		bool outside = false;
		RoundToScreen(vertex.v, scaled, vertex.clip, ts.depthClip, false, &outside);
		vertex.v.clipw = vertex.clip[3];
		vertex.v.fog = 1.0f;
		if (outside) {
			vertex.v.x = 0x7FFFFFFF;
			// Only a vertex the near plane clips away can still contribute (to new vertices).
			if (!(ts.depthClip && vertex.clip[2] < -vertex.clip[3]))
				return vertex;
		}

		if (ts.enableFog && ts.fogGE) {
			const float viewZ = GERow(pos, ts.viewZRows, 4);
			const float f = ProductToFloat24((double)TruncateToFloat24(GEAdd(viewZ, ts.fogEnd)) * ts.fogSlope);
			vertex.v.fog = GEFogFactor(f) * (1.0f / 256.0f);
		} else if (ts.enableFog) {
			const float d = ts.posToFog[0] * pos[0] + ts.posToFog[1] * pos[1] + ts.posToFog[2] * pos[2] + ts.posToFog[3];
			vertex.v.fog = GEFogFactor(d) * (1.0f / 256.0f);
		}

		float worldnormal[3] = { 0.0f, 0.0f, 0.0f };
		float normalRsqrt = 1.0f;
		const bool usesWorldNormal = ts.enableLighting ? ts.light.usesWorldNormal : ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP;
		if (usesWorldNormal) {
			const float *m = gstate.worldMatrix;
			for (int c = 0; c < 3; ++c) {
				const float b[3] = { m[c], m[3 + c], m[6 + c] };
				worldnormal[c] = GERow(normal, b, 3);
			}
			const float len2 = GEDot3(worldnormal, worldnormal);
			if (len2 > 0.0f && std::isfinite(len2)) {
				normalRsqrt = GERsqrt(len2);
			} else if (len2 != 0.0f) {
				worldnormal[0] = worldnormal[1] = 0.0f;
				worldnormal[2] = 1.0f;
			}
		}

		if (ts.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX) {
			float source[4] = { pos[0], pos[1], pos[2], 1.0f };
			switch (gstate.getUVProjMode()) {
			case GE_PROJMAP_POSITION:
				break;
			case GE_PROJMAP_UV:
				source[0] = vertex.v.s;
				source[1] = vertex.v.t;
				source[2] = 0.0f;
				break;
			case GE_PROJMAP_NORMALIZED_NORMAL:
				memcpy(source, normal, sizeof(normal));
				GENormalize3(source);
				break;
			case GE_PROJMAP_NORMAL:
				memcpy(source, normal, sizeof(normal));
				break;
			}
			float out[3];
			for (int c = 0; c < 3; ++c) {
				const float b[4] = { ts.tgen[c], ts.tgen[3 + c], ts.tgen[6 + c], ts.tgen[9 + c] };
				out[c] = GERow(source, b, 4);
			}
			vertex.v.s = out[0];
			vertex.v.t = out[1];
			vertex.v.q = out[2];
		} else if (ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
			vertex.v.s = GEShadeMapCoord(gstate.getUVLS0(), pos, worldnormal, normalRsqrt, ts.light.viewDir);
			vertex.v.t = GEShadeMapCoord(gstate.getUVLS1(), pos, worldnormal, normalRsqrt, ts.light.viewDir);
		}

		if (ts.enableLighting)
			ProcessLighting(vertex.v, pos, worldnormal, normalRsqrt, ts.light);
	} else {
		vertex.v.x = (int)(pos[0] * SUBPIX);
		vertex.v.y = (int)(pos[1] * SUBPIX);
		vertex.v.z = (int)(u16)(u32)pos[2];
		vertex.v.clipw = 1.0f;
		vertex.v.fog = 1.0f;
		vertex.clip[0] = vertex.clip[1] = vertex.clip[2] = 0.0f;
		vertex.clip[3] = 1.0f;
	}
	return vertex;
}

// --- Clipper ---

static inline bool BehindNear(const float *c) {
	return c[2] < -c[3];
}

static inline int OutsideMask(const float *p) {
	int mask = 0;
	if (cullXY) {
		if (p[0] > p[3]) mask |= 1;
		if (-p[0] > p[3]) mask |= 2;
		if (p[1] > p[3]) mask |= 4;
		if (-p[1] > p[3]) mask |= 8;
	}
	if (p[2] > p[3]) mask |= 16;
	if (-p[2] > p[3]) mask |= 32;
	return mask;
}

static inline bool OutsideRangeBeforeClip(const ClipVertex &v, bool depthClip) {
	return v.OutsideRange() && !(depthClip && BehindNear(v.clip));
}

static inline float LerpF(float a, float b, float t) {
	return a * (1.0f - t) + b * t;
}

static void LerpVertex(ClipVertex &dest, float t, const ClipVertex &a, const ClipVertex &b) {
	for (int i = 0; i < 4; ++i)
		dest.clip[i] = LerpF(a.clip[i], b.clip[i], t);
	dest.v.s = LerpF(a.v.s, b.v.s, t);
	dest.v.t = LerpF(a.v.t, b.v.t, t);
	dest.v.q = LerpF(a.v.q, b.v.q, t);
	// Colors and fog take t rounded to 1/256 (gpu/probe exp136).
	const int ti = (int)(t * 256.0f + 0.5f);
	dest.v.fog = (float)(((int)(a.v.fog * 256.0f) * (256 - ti) + (int)(b.v.fog * 256.0f) * ti) >> 8) * (1.0f / 256.0f);
	u32 c0 = 0, c1 = 0;
	for (int i = 0; i < 4; ++i) {
		const int ca = (a.v.color0 >> (8 * i)) & 0xFF, cb = (b.v.color0 >> (8 * i)) & 0xFF;
		c0 |= (u32)((ca * (256 - ti) + cb * ti) / 256) << (8 * i);
		if (i < 3) {
			const int sa = (a.v.color1 >> (8 * i)) & 0xFF, sb = (b.v.color1 >> (8 * i)) & 0xFF;
			c1 |= (u32)((sa * (256 - ti) + sb * ti) / 256) << (8 * i);
		}
	}
	dest.v.color0 = c0;
	dest.v.color1 = c1;
}

// Where an edge from an inside vertex crosses the near plane, as the GE computes it (gpu/probe exp43-46).
static float NearPlaneT(const float *in, const float *out) {
	const float dIn = GEAdd(in[2], in[3]);
	const float dOut = GEAdd(out[2], out[3]);
	const float den = TruncateToFloat24(GEAdd(dIn, -dOut));
	return ProductToFloat24((double)TruncateToFloat24(dIn) * GERecip(den));
}

static void NearPoint(ClipVertex &dest, const ClipVertex &in, const ClipVertex &out) {
	const float t = NearPlaneT(in.clip, out.clip);
	LerpVertex(dest, t, in, out);
	for (int c = 0; c < 4; ++c) {
		const float delta = TruncateToFloat24(GEAdd(out.clip[c], -in.clip[c]));
		dest.clip[c] = TruncateToFloat24(GEAdd(ProductToFloat24((double)t * delta), in.clip[c]));
	}
	// Texture coordinates go through the same arithmetic as the position (gpu/probe exp136).
	const float ins[3] = { in.v.s, in.v.t, in.v.q }, outs[3] = { out.v.s, out.v.t, out.v.q };
	float res[3];
	for (int c = 0; c < 3; ++c) {
		const float delta = TruncateToFloat24(GEAdd(outs[c], -ins[c]));
		res[c] = TruncateToFloat24(GEAdd(ProductToFloat24((double)t * delta), ins[c]));
	}
	dest.v.s = res[0];
	dest.v.t = res[1];
	dest.v.q = res[2];
	bool outside = false;
	ClipToScreen(dest.v, dest.clip, true, &outside);
	dest.v.clipw = dest.clip[3];
	if (outside)
		dest.v.x = 0x7FFFFFFF;
}

static void AddTriangleFlat(const RVertex &v0, const RVertex &v1, const RVertex &v2, const RVertex &provoking) {
	if (!gstate.isShadeGouraud()) {
		RVertex corrected = v2;
		corrected.color0 = provoking.color0;
		corrected.color1 = provoking.color1;
		binner::AddTriangle(v0, v1, corrected);
	} else {
		binner::AddTriangle(v0, v1, v2);
	}
}

void ProcessTriangle(const ClipVertex &v0, const ClipVertex &v1, const ClipVertex &v2, const ClipVertex &provoking, bool reversed) {
	int mask = 0;
	const bool through = binner::CurrentState().throughMode;
	const bool depthClip = gstate.isDepthClipEnabled();
	if (!through) {
		if (OutsideRangeBeforeClip(v0, depthClip) || OutsideRangeBeforeClip(v1, depthClip) || OutsideRangeBeforeClip(v2, depthClip))
			return;
		if (v0.clip[3] < 0.0f && v1.clip[3] < 0.0f && v2.clip[3] < 0.0f)
			return;
		if (BehindNear(v0.clip) || BehindNear(v1.clip) || BehindNear(v2.clip))
			mask = 1;
		if (OutsideMask(v0.clip) & OutsideMask(v1.clip) & OutsideMask(v2.clip))
			return;
		// With depth clip off, the GE doesn't clip at the near plane; a vertex behind the camera culls.
		if (!depthClip) {
			if (!(v0.clip[3] > 0.0f && v1.clip[3] > 0.0f && v2.clip[3] > 0.0f))
				return;
			mask = 0;
		}
	}
	if (mask == 0) {
		AddTriangleFlat(v0.v, v1.v, v2.v, provoking.v);
		return;
	}

	// Near plane clipping (gpu/probe exp43, exp44): with one vertex outside (o), the quad is split from
	// the vertex before it (p) in the submitted order.
	const ClipVertex *src[3] = { &v0, &v1, &v2 };
	bool outside[3];
	int numOutside = 0;
	for (int i = 0; i < 3; ++i) {
		outside[i] = BehindNear(src[i]->clip);
		numOutside += outside[i] ? 1 : 0;
	}
	ClipVertex made[2];
	const ClipVertex *tris[2][3];
	int numTris = 0;
	if (numOutside == 1) {
		const int o = outside[0] ? 0 : (outside[1] ? 1 : 2);
		const int p = reversed ? (o + 1) % 3 : (o + 2) % 3;
		const int n = reversed ? (o + 2) % 3 : (o + 1) % 3;
		NearPoint(made[0], *src[p], *src[o]);
		NearPoint(made[1], *src[n], *src[o]);
		const int first = reversed ? 2 : 1, second = reversed ? 1 : 2;
		tris[0][0] = src[p]; tris[0][first] = &made[0]; tris[0][second] = &made[1];
		tris[1][0] = src[p]; tris[1][first] = &made[1]; tris[1][second] = src[n];
		numTris = 2;
	} else if (numOutside == 2) {
		const int i = !outside[0] ? 0 : (!outside[1] ? 1 : 2);
		const int n = (i + 1) % 3, p = (i + 2) % 3;
		NearPoint(made[0], *src[i], *src[n]);
		NearPoint(made[1], *src[i], *src[p]);
		tris[0][0] = src[i]; tris[0][1] = &made[0]; tris[0][2] = &made[1];
		numTris = 1;
	}
	for (int t = 0; t < numTris; ++t) {
		if (tris[t][0]->OutsideRange() || tris[t][1]->OutsideRange() || tris[t][2]->OutsideRange())
			continue;
		AddTriangleFlat(tris[t][0]->v, tris[t][1]->v, tris[t][2]->v, provoking.v);
	}
}

enum class CullType { CW = 0, CCW = 1, OFF = 2 };

void SendTriangle(CullType cullType, const ClipVertex *verts, int provoking = 2, bool orderReversed = false) {
	if (cullType == CullType::OFF) {
		ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking], orderReversed);
		ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking], !orderReversed);
	} else if (cullType == CullType::CW) {
		ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking], !orderReversed);
	} else {
		ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking], orderReversed);
	}
}

static void RotateUV(const RVertex &tl, const RVertex &br, RVertex &tr, RVertex &bl) {
	const int x1 = tl.x, x2 = br.x, y1 = tl.y, y2 = br.y;
	if ((x1 < x2 && y1 > y2) || (x1 > x2 && y1 < y2)) {
		std::swap(bl.s, tr.s);
		std::swap(bl.t, tr.t);
		std::swap(bl.q, tr.q);
	}
}

// Rectangles with texture projection become triangles.
static void AddTriangleRect(const RVertex &v0, const RVertex &v1) {
	RVertex buf[4];
	buf[0] = v1;
	buf[0].x = v0.x; buf[0].y = v0.y;
	buf[0].s = v0.s; buf[0].t = v0.t; buf[0].q = v0.q;
	buf[1] = v1;
	buf[1].x = v0.x; buf[1].y = v1.y;
	buf[1].s = v0.s; buf[1].t = v1.t; buf[1].q = v0.q;
	buf[2] = v1;
	buf[2].x = v1.x; buf[2].y = v0.y;
	buf[2].s = v1.s; buf[2].t = v0.t; buf[2].q = v1.q;
	buf[3] = v1;
	RVertex *topleft = &buf[0], *topright = &buf[1], *bottomleft = &buf[2], *bottomright = &buf[3];
	for (int i = 0; i < 4; ++i) {
		if (buf[i].x < topleft->x && buf[i].y < topleft->y)
			topleft = &buf[i];
		if (buf[i].x > topright->x && buf[i].y < topright->y)
			topright = &buf[i];
		if (buf[i].x < bottomleft->x && buf[i].y > bottomleft->y)
			bottomleft = &buf[i];
		if (buf[i].x > bottomright->x && buf[i].y > bottomright->y)
			bottomright = &buf[i];
	}
	RotateUV(v0, v1, *topright, *bottomleft);
	binner::AddTriangle(*topleft, *topright, *bottomleft);
	binner::AddTriangle(*bottomleft, *topright, *topleft);
	binner::AddTriangle(*topright, *bottomright, *bottomleft);
	binner::AddTriangle(*bottomleft, *bottomright, *topright);
}

void ProcessRect(const ClipVertex &v0, const ClipVertex &v1) {
	const RasterState &st = binner::CurrentState();
	if (!st.throughMode) {
		if (v0.OutsideRange() || v1.OutsideRange())
			return;
		if (OutsideMask(v0.clip) & OutsideMask(v1.clip))
			return;
		// Rects aren't clipped: one with a vertex behind the camera is culled.
		if (!(v0.clip[3] > 0.0f && v1.clip[3] > 0.0f))
			return;
		bool splitFog = v0.v.fog != v1.v.fog;
		if (splitFog) {
			static constexpr float foghalfstep = 0.5f / 255.0f;
			if (v1.v.fog - foghalfstep <= v0.v.fog && v1.v.fog + foghalfstep >= v0.v.fog)
				splitFog = false;
		}
		if (splitFog) {
			// Rectangles use nearest along X for fog depth, but reversed.
			RVertex vhalf0 = v1.v;
			vhalf0.x = v0.v.x + (v1.v.x - v0.v.x) / 2;
			vhalf0.s = v0.v.s + (v1.v.s - v0.v.s) / 2;
			RVertex vhalf1 = v1.v;
			vhalf1.x = v0.v.x + (v1.v.x - v0.v.x) / 2;
			vhalf1.y = v0.v.y;
			vhalf1.s = v0.v.s + (v1.v.s - v0.v.s) / 2;
			vhalf1.t = v0.v.t;
			RVertex vrev1 = v1.v;
			vrev1.fog = v0.v.fog;
			if (st.textureProj) {
				AddTriangleRect(v0.v, vhalf0);
				AddTriangleRect(vhalf1, vrev1);
			} else {
				binner::AddRect(v0.v, vhalf0);
				binner::AddRect(vhalf1, vrev1);
			}
		} else if (st.textureProj) {
			AddTriangleRect(v0.v, v1.v);
		} else {
			binner::AddRect(v0.v, v1.v);
		}
	} else {
		if (gstate.isModeClear() && !gstate.isDitherEnabled())
			binner::AddClearRect(v0.v, v1.v);
		else
			binner::AddRect(v0.v, v1.v);
	}
}

void ProcessPoint(const ClipVertex &v0) {
	if (!binner::CurrentState().throughMode) {
		if (v0.OutsideRange())
			return;
		if (OutsideMask(v0.clip))
			return;
	}
	binner::AddPoint(v0.v);
}

void ProcessLine(const ClipVertex &v0, const ClipVertex &v1) {
	if (binner::CurrentState().throughMode) {
		binner::AddLine(v0.v, v1.v);
		return;
	}
	const bool depthClip = gstate.isDepthClipEnabled();
	if (OutsideRangeBeforeClip(v0, depthClip) || OutsideRangeBeforeClip(v1, depthClip))
		return;
	if (OutsideMask(v0.clip) & OutsideMask(v1.clip))
		return;
	bool clip = BehindNear(v0.clip) || BehindNear(v1.clip);
	if (!depthClip) {
		if (!(v0.clip[3] > 0.0f && v1.clip[3] > 0.0f))
			return;
		clip = false;
	}
	if (!clip) {
		binner::AddLine(v0.v, v1.v);
		return;
	}
	ClipVertex a = v0, b = v1;
	auto dot = [](const ClipVertex &v) { return v.clip[2] + v.clip[3]; };
	float dp0 = dot(a), dp1 = dot(b);
	if (BehindNear(a.clip) && dp0 < 0) {
		const float t = dp1 / (dp1 - dp0);
		ClipVertex n;
		LerpVertex(n, t, b, a);
		bool outside = false;
		ClipToScreen(n.v, n.clip, true, &outside);
		n.v.clipw = n.clip[3];
		if (outside)
			n.v.x = 0x7FFFFFFF;
		a = n;
	}
	dp0 = dot(a);
	if (BehindNear(b.clip) && dp1 < 0) {
		const float t = dp1 / (dp1 - dp0);
		ClipVertex n;
		LerpVertex(n, t, b, a);
		bool outside = false;
		ClipToScreen(n.v, n.clip, true, &outside);
		n.v.clipw = n.clip[3];
		if (outside)
			n.v.x = 0x7FFFFFFF;
		b = n;
	}
	if (!a.OutsideRange() && !b.OutsideRange())
		binner::AddLine(a.v, b.v);
}

// Assembles primitives from the vertices produced by read(i) (count may be 0: imm continuation).
template <typename ReadFn>
void Assemble(GEPrimitiveType primType, int count, ReadFn read) {
	if (primType != GE_PRIM_KEEP_PREVIOUS) {
		dataIndex = 0;
		prevPrim = primType;
	} else {
		primType = prevPrim;
	}
	const bool skipCull = !gstate.isCullEnabled() || gstate.isModeClear();
	const CullType cullType = skipCull ? CullType::OFF : (gstate.getCullMode() ? CullType::CCW : CullType::CW);

	switch (primType) {
	case GE_PRIM_POINTS:
		for (int i = 0; i < dataIndex; ++i)
			ProcessPoint(data[i]);
		dataIndex = 0;
		for (int vtx = 0; vtx < count; ++vtx) {
			data[0] = read(vtx);
			ProcessPoint(data[0]);
		}
		break;

	case GE_PRIM_LINES:
		for (int i = 0; i < dataIndex - 1; i += 2)
			ProcessLine(data[i], data[i + 1]);
		dataIndex &= 1;
		for (int vtx = 0; vtx < count; ++vtx) {
			data[dataIndex++] = read(vtx);
			if (dataIndex == 2) {
				ProcessLine(data[0], data[1]);
				dataIndex = 0;
			}
		}
		break;

	case GE_PRIM_TRIANGLES:
		for (int vtx = 0; vtx < count; ++vtx) {
			data[dataIndex++] = read(vtx);
			if (dataIndex < 3)
				continue;
			dataIndex = 0;
			SendTriangle(cullType, &data[0]);
		}
		if (dataIndex >= 3) {
			SendTriangle(cullType, &data[0]);
			dataIndex = 0;
		}
		break;

	case GE_PRIM_RECTANGLES:
		for (int vtx = 0; vtx < count; ++vtx) {
			data[dataIndex++] = read(vtx);
			if (dataIndex == 2) {
				ProcessRect(data[0], data[1]);
				dataIndex = 0;
			}
		}
		if (dataIndex >= 2) {
			ProcessRect(data[0], data[1]);
			dataIndex -= 2;
		}
		break;

	case GE_PRIM_LINE_STRIP: {
		int skip_count = dataIndex == 0 ? 1 : 0;
		for (int vtx = 0; vtx < count; ++vtx) {
			data[(dataIndex++) & 1] = read(vtx);
			if (skip_count)
				--skip_count;
			else
				ProcessLine(data[dataIndex & 1], data[(dataIndex & 1) ^ 1]);
		}
		if (isImmDraw && dataIndex >= 2)
			ProcessLine(data[dataIndex & 1], data[(dataIndex & 1) ^ 1]);
		break;
	}

	case GE_PRIM_TRIANGLE_STRIP: {
		int skip_count = dataIndex >= 2 ? 0 : 2 - dataIndex;
		int start_vtx = 0;
		for (int vtx = 0; vtx < count && skip_count > 0; ++vtx) {
			data[(dataIndex++) % 3] = read(vtx);
			--skip_count;
			++start_vtx;
		}
		for (int vtx = start_vtx; vtx < count; ++vtx) {
			const int provoking_index = (dataIndex++) % 3;
			data[provoking_index] = read(vtx);
			const int wind = (dataIndex - 1) % 2;
			const CullType alt = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
			// Odd triangles reach us in the opposite order from the GE's (gpu/probe exp45).
			SendTriangle(alt, &data[0], provoking_index, wind != 0);
		}
		if (isImmDraw && dataIndex >= 3) {
			const int provoking_index = (dataIndex - 1) % 3;
			const int wind = (dataIndex - 1) % 2;
			const CullType alt = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
			SendTriangle(alt, &data[0], provoking_index, wind != 0);
		}
		break;
	}

	case GE_PRIM_TRIANGLE_FAN: {
		int skip_count = dataIndex <= 1 ? 1 : 0;
		int start_vtx = 0;
		if (dataIndex == 0 && count > 0) {
			data[0] = read(0);
			dataIndex++;
			start_vtx = 1;
		}
		for (int vtx = start_vtx; vtx < count && skip_count > 0; ++vtx) {
			const int provoking_index = 2 - ((dataIndex++) % 2);
			data[provoking_index] = read(vtx);
			--skip_count;
			++start_vtx;
		}
		for (int vtx = start_vtx; vtx < count; ++vtx) {
			const int provoking_index = 2 - ((dataIndex++) % 2);
			data[provoking_index] = read(vtx);
			const int wind = (dataIndex - 1) % 2;
			const CullType alt = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
			SendTriangle(alt, &data[0], provoking_index, wind != 0);
		}
		if (isImmDraw && dataIndex >= 3) {
			const int wind = (dataIndex - 1) % 2;
			const int provoking_index = 2 - wind;
			const CullType alt = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
			SendTriangle(alt, &data[0], provoking_index, wind != 0);
		}
		break;
	}

	default:
		break;
	}
}

}  // namespace

void TransformDirty() {
	tsDirty = true;
}

void ResetPrimAssembly() {
	dataIndex = 0;
	prevPrim = GE_PRIM_POINTS;
	carry = Carry{};
	tsDirty = true;
}

void SubmitPrimitive(const u8 *verts, const u8 *inds, GEPrimitiveType prim, int count, u32 vertType) {
	if (vfmt.vtype != vertType)
		vfmt.Set(vertType);
	const VertexFormat &fmt = vfmt;
	if ((vertType & GE_VTYPE_POS_MASK) == 0)
		return;
	const bool hasUV = fmt.tc != 0, hasNormal = fmt.nrm != 0, hasColor = fmt.col != 0;
	EnsureTransformState(fmt.through, hasColor, vertType);
	g_stats.vertices += count;

	const bool useIndices = inds != nullptr;
	int lower = 0, upper = count > 0 ? count - 1 : 0;
	if (useIndices && count > 0)
		GetIndexBounds(inds, count, fmt.idx, &lower, &upper);

	// Reusing indexed vertices: transform the range once.
	const bool useCache = useIndices && count > upper - lower + 1;
	if (useCache) {
		if ((int)cache.size() < upper - lower + 1)
			cache.resize(upper - lower + 1);
		Carry c = carry;
		SimpleVertex sv;
		for (int i = lower; i <= upper; ++i) {
			DecodeVertex(fmt, verts, i, &sv);
			cache[i - lower] = ReadVertex(sv, hasUV, hasNormal, hasColor, c);
		}
		// What the next draw carries is the last vertex in draw order.
		if (count != 0) {
			DecodeVertex(fmt, verts, ReadIndex(inds, fmt.idx, count - 1), &sv);
			ReadVertex(sv, hasUV, hasNormal, hasColor, carry);
		}
	}

	auto read = [&](int vtx) -> ClipVertex {
		if (useCache)
			return cache[ReadIndex(inds, fmt.idx, vtx) - lower];
		SimpleVertex sv;
		DecodeVertex(fmt, verts, useIndices ? ReadIndex(inds, fmt.idx, vtx) : vtx, &sv);
		return ReadVertex(sv, hasUV, hasNormal, hasColor, carry);
	};
	isImmDraw = false;
	Assemble(prim, count, read);
}

void SubmitSimpleVertices(const SimpleVertex *verts, const u16 *inds, GEPrimitiveType prim, int count, bool through, bool hasColor, bool hasUV, bool hasNormal) {
	// The curve's vertex format: float position, normal and UVs, 8888 color, 16-bit indices.
	const u32 vtype = (through ? GE_VTYPE_THROUGH : 0) | GE_VTYPE_POS_FLOAT | GE_VTYPE_IDX_16BIT |
		(hasColor ? GE_VTYPE_COL_8888 : 0) | (hasUV ? GE_VTYPE_TC_FLOAT : 0) | (hasNormal ? GE_VTYPE_NRM_FLOAT : 0);
	EnsureTransformState(through, hasColor, vtype);
	g_stats.vertices += count;
	auto read = [&](int vtx) -> ClipVertex {
		SimpleVertex sv = verts[inds ? inds[vtx] : vtx];
		if (through) {
			sv.pos[2] = sv.pos[2] >= 65535.0f ? 65535.0f : (sv.pos[2] > 0.0f ? (float)(int)sv.pos[2] : 0.0f);
		}
		return ReadVertex(sv, hasUV, hasNormal, hasColor, carry);
	};
	isImmDraw = false;
	Assemble(prim, count, read);
}

void SubmitImmVertex(GEPrimitiveType prim, const RVertex &v, float clipw, bool first) {
	if (first) {
		dataIndex = 0;
		prevPrim = prim;
	}
	ClipVertex cv;
	cv.v = v;
	cv.clip[0] = (float)v.x / SUBPIX;
	cv.clip[1] = (float)v.y / SUBPIX;
	cv.clip[2] = (float)v.z / 65535.0f;
	cv.clip[3] = clipw;
	switch (prevPrim) {
	case GE_PRIM_LINE_STRIP:
		data[(dataIndex++) & 1] = cv;
		break;
	case GE_PRIM_TRIANGLE_STRIP:
		data[(dataIndex++) % 3] = cv;
		break;
	case GE_PRIM_TRIANGLE_FAN:
		if (dataIndex == 0)
			data[dataIndex++] = cv;
		else
			data[2 - ((dataIndex++) % 2)] = cv;
		break;
	default:
		if (dataIndex < 4)
			data[dataIndex++] = cv;
		break;
	}
	isImmDraw = true;
	cullXY = false;
	Assemble(GE_PRIM_KEEP_PREVIOUS, 0, [](int) { return ClipVertex{}; });
	cullXY = true;
	isImmDraw = false;
}

bool TestBoundingBox(const u8 *verts, const u8 *inds, int count, u32 vertType) {
	if (count > 1024)
		return true;
	VertexFormat fmt;
	fmt.Set(vertType);
	// Plain float math with the combined world-view-projection.
	float world[16], view[16], wv[16], wvp[16];
	Convert4x3To4x4(view, gstate.viewMatrix);
	Convert4x3To4x4(world, gstate.worldMatrix);
	auto mul = [](float out[16], const float a[16], const float b[16]) {
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c) {
				float s = 0.0f;
				for (int k = 0; k < 4; ++k)
					s += a[r * 4 + k] * b[k * 4 + c];
				out[r * 4 + c] = s;
			}
	};
	mul(wv, world, view);
	mul(wvp, wv, gstate.projMatrix);

	const int left = (int)(gstate.getOffsetX() + std::max(gstate.getRegionX1(), gstate.getScissorX1()) - 1);
	const int top = (int)(gstate.getOffsetY() + std::max(gstate.getRegionY1(), gstate.getScissorY1()) - 1);
	const int right = (int)(gstate.getOffsetX() + std::min(gstate.getRegionX2(), gstate.getScissorX2()) + 1);
	const int bottom = (int)(gstate.getOffsetY() + std::min(gstate.getRegionY2(), gstate.getScissorY2()) + 1);
	if (right >= 4096 || bottom >= 4096 || left < 1 || top < 1)
		return true;

	int insideCount[6] = {};
	const float xs = gstate.getViewportXScale(), xc = gstate.getViewportXCenter();
	const float ys = gstate.getViewportYScale(), yc = gstate.getViewportYCenter();
	for (int i = 0; i < count; ++i) {
		SimpleVertex sv;
		fmt.through = false;  // positions as model space
		DecodeVertex(fmt, verts, inds ? ReadIndex(inds, fmt.idx, i) : i, &sv);
		float p[4];
		for (int c = 0; c < 4; ++c)
			p[c] = sv.pos[0] * wvp[c] + sv.pos[1] * wvp[4 + c] + sv.pos[2] * wvp[8 + c] + wvp[12 + c];
		if (p[2] >= -p[3])
			insideCount[4]++;
		if (p[2] <= p[3])
			insideCount[5]++;
		const float w = p[3];
		const float drawX = p[0] * xs + xc * w;
		const float drawY = p[1] * ys + yc * w;
		if (drawX >= left * w) insideCount[0]++;
		if (drawX <= right * w) insideCount[1]++;
		if (drawY >= top * w) insideCount[2]++;
		if (drawY <= bottom * w) insideCount[3]++;
	}
	const int countToCheck = gstate.isDepthClipEnabled() ? 6 : 4;
	for (int i = 0; i < countToCheck; ++i) {
		if (insideCount[i] == 0)
			return false;
	}
	return true;
}

}  // namespace gpu
