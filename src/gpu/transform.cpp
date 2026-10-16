// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Vertex transform, lighting, near-plane clipping and primitive assembly, in the GE's arithmetic.
// Port of GPU/Software/TransformUnit.cpp, Lighting.cpp and Clipper.cpp (scalar).
//
// Draws (PRIM) are recorded rather than transformed when they come: with a copy of what their vertices
// and primitives depend on (the transform state, the raster state, morph weights, bones, the culling and
// shading modes). When the binner's queue is next needed (ResolvePendingDraws), every recorded vertex is
// transformed at once, spread over the worker threads (games draw thousands of small batches a frame,
// too small to split one at a time), and the primitives are then assembled draw by draw, in order. A
// vertex's result depends only on its draw, so the queue ends up exactly as with one draw at a time.
#include <algorithm>
#include <cmath>
#include <vector>

#include "gpu/ge_state.h"
#include "gpu/gemath_simd.h"
#include "gpu/geometry.h"
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
	alignas(16) float tgenRows[4][4];     // texture matrix rows, padded
	alignas(16) float worldRows[3][4];    // world matrix rows (normals), padded
	alignas(16) float negWorldRows[4][4]; // negated world rows, the light vector
	bool enableTransform, enableLighting, enableFog, negateNormals, depthClip;
	int uvGenMode;
	bool geUVScale;
	bool uvPrescale;  // UV gen mode 3: the decoder's float scale and offset
	float uvScale[2], uvOffset[2];
	float uvScaleF[4];
	int offsetX16, offsetY16;
	// What vertices read of gstate besides: the color of vertices without one, the emissive color, the
	// texture projection source, and the environment map's two lights (UVLS0/1).
	u32 materialAmbientRGBA, materialEmissive;
	int uvProjMode;
	struct ShadeLight {
		float pos[3];
		float translated[3];  // directional: unused
		bool directional, specular;
	} shade[2];
};

TransformState ts;
bool tsDirty = true;
u32 tsVType = 0xFFFFFFFF;
u64 tsSerial = 0;  // counts the computations of ts
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

enum class CullType { CW = 0, CCW = 1, OFF = 2 };

// What assembling a draw's primitives reads besides its vertices: the gstate and raster state of when
// it was submitted. live: they're current (immediate vertices, curves), read them there.
struct AssemblyCtx {
	const TransformState *ts;
	bool live;
	bool through, textureProj, depthClip, gouraud, clearRect;
	CullType cull;
};
const AssemblyCtx *actx = nullptr;

inline bool CtxThrough() {
	return actx->live ? binner::CurrentState().throughMode : actx->through;
}
inline bool CtxTextureProj() {
	return actx->live ? binner::CurrentState().textureProj : actx->textureProj;
}
inline bool CtxDepthClip() {
	return actx->live ? gstate.isDepthClipEnabled() : actx->depthClip;
}
inline bool CtxGouraud() {
	return actx->live ? gstate.isShadeGouraud() : actx->gouraud;
}
// Through-mode rectangles in clear mode without dithering are clears.
inline bool CtxClearRect() {
	return actx->live ? gstate.isModeClear() && !gstate.isDitherEnabled() : actx->clearRect;
}
inline CullType GECullType() {
	const bool skipCull = !gstate.isCullEnabled() || gstate.isModeClear();
	return skipCull ? CullType::OFF : (gstate.getCullMode() ? CullType::CCW : CullType::CW);
}
inline CullType CtxCull() {
	return actx->live ? GECullType() : actx->cull;
}

AssemblyCtx LiveCtx() {
	AssemblyCtx c{};
	c.ts = &ts;
	c.live = true;
	return c;
}

// --- Recorded draws ---

struct PendingDraw {
	const u8 *verts, *inds;
	VertexFormat fmt;
	GEPrimitiveType prim;  // as submitted: KEEP_PREVIOUS continues the previous one
	int count;
	int lower;             // useCache: the index range [lower, lower + slots) is transformed once
	bool useCache;
	bool hasUV, hasNormal, hasColor;
	u32 tsId;              // tsSnaps
	u32 stateId;           // binner::Snapshot
	u32 boneOff;           // pendingBones, skinned formats
	u32 slotBase, slots;   // transformed vertices, in pendingOut
	float morph[8];
	Carry carryIn;         // the UV and normal of formats without them
	AssemblyCtx ctx;       // ctx.ts is set when assembling
	// Transformed by the backend (geometry.h): its GeDraw, and where its slots are in the inputs.
	bool gpu;
	u32 geoDraw, geoBase;
};

std::vector<PendingDraw> pending;
std::vector<TransformState> tsSnaps;
u64 tsSnapSerial = ~0ull;  // tsSerial of tsSnaps.back()
std::vector<float> pendingBones;
std::vector<ClipVertex> pendingOut;
u32 pendingSlots = 0;
bool resolvingDraws = false;
// The draw types record keeps, as Assemble resolves them (KEEP_PREVIOUS).
GEPrimitiveType recPrevPrim = GE_PRIM_POINTS;
// The pending draws whose last vertex carries the UV and the normal over, until a draw needs them
// (carry holds them before): -1 if carry is up to date.
int uvSource = -1, nrmSource = -1;

// The backend transforms transform-mode triangles (geometry.h); when it verifies them, they're also
// transformed here (for the resolve under way).
bool gpuTransformVerify = false;
// While assembling a draw for the backend: the vertex slots in data[] (-1: data[] holds the vertex).
int dataSlot[4] = { -1, -1, -1, -1 };
// The verification's triangles (after the winding test), instead of the binner.
std::vector<RVertex> *captureTris = nullptr;

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
	s.materialAmbientRGBA = gstate.getMaterialAmbientRGBA();
	s.materialEmissive = gstate.getMaterialEmissive();
	s.uvProjMode = gstate.getUVProjMode();

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

	if (s.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
		const int ls[2] = { gstate.getUVLS0(), gstate.getUVLS1() };
		for (int k = 0; k < 2; ++k) {
			TransformState::ShadeLight &sl = s.shade[k];
			const int l = ls[k];
			for (int i = 0; i < 3; ++i)
				sl.pos[i] = getFloat24(gstate.lpos[l * 3 + i]);
			sl.directional = gstate.getLightType(l) == GE_LIGHTTYPE_DIRECTIONAL;
			if (!sl.directional) {
				for (int i = 0; i < 3; ++i)
					sl.translated[i] = GEAddFloat24(sl.pos[i], -TruncateToFloat24(gstate.worldMatrix[9 + i]));
			}
			sl.specular = gstate.isUsingSpecularLight(l);
		}
	}

	for (int k = 0; k < 4; ++k) {
		for (int c = 0; c < 3; ++c)
			s.tgenRows[k][c] = gstate.tgenMatrix[3 * k + c];
		s.tgenRows[k][3] = 0.0f;
	}
	for (int k = 0; k < 3; ++k) {
		for (int c = 0; c < 3; ++c) {
			s.worldRows[k][c] = gstate.worldMatrix[3 * k + c];
			s.negWorldRows[k][c] = BitsFloat(FloatBits(gstate.worldMatrix[3 * k + c]) ^ 0x80000000);
		}
		s.worldRows[k][3] = 0.0f;
		s.negWorldRows[k][3] = 0.0f;
	}

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
		tsSerial++;
	}
}

constexpr float SCREEN_BOUND = 4095.0f + (15.5f / 16.0f);

// alwaysCheckRange: for vertices made by clipping (no near-plane exemption).
static inline void RoundToScreen(const TransformState &s, RVertex &v, const float scaled[3], const float clip[4], bool depthClamp,
                                 bool alwaysCheckRange, bool *outside) {
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
	v.x = (int)floorf(scaled[0] * 16.0f) - s.offsetX16;
	v.y = (int)floorf(scaled[1] * 16.0f) - s.offsetY16;
	v.z = (z >= 0.0f && z < 65536.0f) ? (int)(u16)(u32)z : 0;
}

// The screen position: each clip component times the GE's reciprocal of w, scaled and offset.
static inline void Viewport(const TransformState &s, const float clip[4], float scaled[3]) {
	const float w = TruncateToFloat24(clip[3]);
	if (!IsFiniteF(w) || fabsf(w) < FLT_MIN) {
		for (int i = 0; i < 3; ++i)
			scaled[i] = GEViewport(clip[i], clip[3], s.screenScale[i], s.screenAdd[i]);
		scaled[2] = floorf(scaled[2]);
		return;
	}
	const double r = GERecip(w);
	for (int i = 0; i < 3; ++i) {
		if (!IsFiniteF(clip[i])) {
			scaled[i] = clip[i] * s.screenScale[i] / clip[3] + s.screenAdd[i];
		} else {
			const float ndc = ProductToFloat24((double)TruncateToFloat24(clip[i]) * r);
			scaled[i] = GEAdd(ProductToFloat24((double)ndc * s.screenScale[i]), s.screenAdd[i]);
		}
	}
	scaled[2] = floorf(scaled[2]);
}

static inline void ClipToScreen(const TransformState &s, RVertex &v, const float clip[4], bool alwaysCheck, bool *outside) {
	float scaled[3];
	Viewport(s, clip, scaled);
	RoundToScreen(s, v, scaled, clip, s.depthClip, alwaysCheck, outside);
}

// v . N for the unnormalized N, scaled by its reciprocal length.
static inline float GENormalDot(const float *v, const float *n, float nRsqrt) {
	return ProductToFloat24((double)GEDot3(v, n) * nRsqrt);
}

// The vector from a vertex to a point light (gpu/probe exp153-158).
static inline void GELightVector(const TransformState &s, float out[3], const float translated[3], const float modelpos[4]) {
	alignas(16) float rows[4][4];
	memcpy(rows, s.negWorldRows, sizeof(float) * 12);
	rows[3][0] = translated[0];
	rows[3][1] = translated[1];
	rows[3][2] = translated[2];
	rows[3][3] = 0.0f;
	alignas(16) float r[4];
	GERowSum4<4>(modelpos, rows, r);
	out[0] = r[0];
	out[1] = r[1];
	out[2] = r[2];
}

static float GEShadeMapCoord(const TransformState &s, const TransformState::ShadeLight &sl, const float modelpos[4],
                             const float worldnormal[3], float normalRsqrt, const float viewDir[3]) {
	float L[3] = { sl.pos[0], sl.pos[1], sl.pos[2] };
	if (!sl.directional)
		GELightVector(s, L, sl.translated, modelpos);
	GENormalize3(L);
	if (sl.specular) {
		for (int i = 0; i < 3; ++i)
			L[i] = GEAddFloat24(L[i], viewDir[i]);
		GENormalize3(L);
	}
	return GEAddFloat24(GENormalDot(L, worldnormal, normalRsqrt), 1.0f) * 0.5f;
}

void ProcessLighting(const TransformState &s, RVertex &vertex, const float modelpos[4], const float worldnormal[3], float normalRsqrt) {
	static const float zAxis[3] = { 0.0f, 0.0f, 1.0f };
	const LightState &state = s.light;
	Color4i colorFactor{};
	if (state.colorForAmbient || state.colorForDiffuse || state.colorForSpecular)
		colorFactor = ColorFactor(vertex.color0);

	const u32 mecv = s.materialEmissive;
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
			GELightVector(s, L, ls.translated, modelpos);
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

// What reading a vertex leaves for formats without UV or normal: its UV (prescaled) and normal.
inline void UpdateCarry(const TransformState &s, const SimpleVertex &sv, bool hasUV, bool hasNormal, Carry &cr) {
	if (hasUV) {
		cr.uv[0] = sv.uv[0];
		cr.uv[1] = sv.uv[1];
		if (s.uvPrescale) {
			cr.uv[0] = cr.uv[0] * s.uvScaleF[0] + s.uvScaleF[2];
			cr.uv[1] = cr.uv[1] * s.uvScaleF[1] + s.uvScaleF[3];
		}
		cr.uv[2] = 0.0f;
	}
	if (hasNormal)
		memcpy(cr.normal, sv.nrm, sizeof(cr.normal));
}

// One vertex through the pipeline. The SimpleVertex has morph and skinning applied.
ClipVertex ReadVertex(const TransformState &s, const SimpleVertex &sv, bool hasUV, bool hasNormal, bool hasColor, Carry &cr) {
	ClipVertex vertex;
	const float pos[4] = { sv.pos[0], sv.pos[1], sv.pos[2], 1.0f };

	UpdateCarry(s, sv, hasUV, hasNormal, cr);
	vertex.v.s = cr.uv[0];
	vertex.v.t = cr.uv[1];
	vertex.v.q = cr.uv[2];
	if (s.geUVScale) {
		float *tc[2] = { &vertex.v.s, &vertex.v.t };
		for (int i = 0; i < 2; ++i) {
			const float scaled = ProductToFloat24((double)TruncateToFloat24(*tc[i]) * s.uvScale[i]);
			*tc[i] = TruncateToFloat24(GEAdd(scaled, s.uvOffset[i]));
		}
	}

	float normal[3] = { cr.normal[0], cr.normal[1], cr.normal[2] };
	if (s.negateNormals) {
		for (int i = 0; i < 3; ++i)
			normal[i] = BitsFloat(FloatBits(normal[i]) ^ 0x80000000);
	}

	vertex.v.color0 = hasColor ? sv.color : s.materialAmbientRGBA;
	vertex.v.color1 = 0;

	if (s.enableTransform) {
		GERowSum4<4>(pos, (const float(*)[4])s.matrix, vertex.clip);
		float scaled[3];
		Viewport(s, vertex.clip, scaled);
		bool outside = false;
		RoundToScreen(s, vertex.v, scaled, vertex.clip, s.depthClip, false, &outside);
		vertex.v.clipw = vertex.clip[3];
		vertex.v.fog = 1.0f;
		if (outside) {
			vertex.v.x = 0x7FFFFFFF;
			// Only a vertex the near plane clips away can still contribute (to new vertices).
			if (!(s.depthClip && vertex.clip[2] < -vertex.clip[3]))
				return vertex;
		}

		if (s.enableFog && s.fogGE) {
			const float viewZ = GERow(pos, s.viewZRows, 4);
			const float f = ProductToFloat24((double)TruncateToFloat24(GEAdd(viewZ, s.fogEnd)) * s.fogSlope);
			vertex.v.fog = GEFogFactor(f) * (1.0f / 256.0f);
		} else if (s.enableFog) {
			const float d = s.posToFog[0] * pos[0] + s.posToFog[1] * pos[1] + s.posToFog[2] * pos[2] + s.posToFog[3];
			vertex.v.fog = GEFogFactor(d) * (1.0f / 256.0f);
		}

		float worldnormal[3] = { 0.0f, 0.0f, 0.0f };
		float normalRsqrt = 1.0f;
		const bool usesWorldNormal = s.enableLighting ? s.light.usesWorldNormal : s.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP;
		if (usesWorldNormal) {
			alignas(16) float wn[4];
			const float n4[4] = { normal[0], normal[1], normal[2], 0.0f };
			GERowSum4<3>(n4, s.worldRows, wn);
			worldnormal[0] = wn[0];
			worldnormal[1] = wn[1];
			worldnormal[2] = wn[2];
			const float len2 = GEDot3(worldnormal, worldnormal);
			if (len2 > 0.0f && std::isfinite(len2)) {
				normalRsqrt = GERsqrt(len2);
			} else if (len2 != 0.0f) {
				worldnormal[0] = worldnormal[1] = 0.0f;
				worldnormal[2] = 1.0f;
			}
		}

		if (s.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX) {
			float source[4] = { pos[0], pos[1], pos[2], 1.0f };
			switch (s.uvProjMode) {
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
			alignas(16) float out[4];
			GERowSum4<4>(source, s.tgenRows, out);
			vertex.v.s = out[0];
			vertex.v.t = out[1];
			vertex.v.q = out[2];
		} else if (s.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
			vertex.v.s = GEShadeMapCoord(s, s.shade[0], pos, worldnormal, normalRsqrt, s.light.viewDir);
			vertex.v.t = GEShadeMapCoord(s, s.shade[1], pos, worldnormal, normalRsqrt, s.light.viewDir);
		}

		if (s.enableLighting)
			ProcessLighting(s, vertex.v, pos, worldnormal, normalRsqrt);
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
	ClipToScreen(*actx->ts, dest.v, dest.clip, true, &outside);
	dest.v.clipw = dest.clip[3];
	if (outside)
		dest.v.x = 0x7FFFFFFF;
}

// binner::AddTriangle's winding test.
inline bool BinnerWinds(const RVertex &v0, const RVertex &v1, const RVertex &v2) {
	const int64_t d01x = (int64_t)v0.x - v1.x, d01y = (int64_t)v0.y - v1.y;
	const int64_t d02x = (int64_t)v0.x - v2.x, d02y = (int64_t)v0.y - v2.y;
	return d01x * d02y - d01y * d02x > 0;
}

static void AddTriangleFlat(const RVertex &v0, const RVertex &v1, const RVertex &v2, const RVertex &provoking) {
	RVertex corrected = v2;
	if (!CtxGouraud()) {
		corrected.color0 = provoking.color0;
		corrected.color1 = provoking.color1;
	}
	if (captureTris) {
		if (BinnerWinds(v0, v1, corrected)) {
			captureTris->push_back(v0);
			captureTris->push_back(v1);
			captureTris->push_back(corrected);
		}
		return;
	}
	binner::AddTriangle(v0, v1, corrected);
}

void ProcessTriangle(const ClipVertex &v0, const ClipVertex &v1, const ClipVertex &v2, const ClipVertex &provoking, bool reversed) {
	int mask = 0;
	const bool through = CtxThrough();
	const bool depthClip = CtxDepthClip();
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
	if (!CtxThrough()) {
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
			if (CtxTextureProj()) {
				AddTriangleRect(v0.v, vhalf0);
				AddTriangleRect(vhalf1, vrev1);
			} else {
				binner::AddRect(v0.v, vhalf0);
				binner::AddRect(vhalf1, vrev1);
			}
		} else if (CtxTextureProj()) {
			AddTriangleRect(v0.v, v1.v);
		} else {
			binner::AddRect(v0.v, v1.v);
		}
	} else {
		if (CtxClearRect())
			binner::AddClearRect(v0.v, v1.v);
		else
			binner::AddRect(v0.v, v1.v);
	}
}

void ProcessPoint(const ClipVertex &v0) {
	if (!CtxThrough()) {
		if (v0.OutsideRange())
			return;
		if (OutsideMask(v0.clip))
			return;
	}
	binner::AddPoint(v0.v);
}

void ProcessLine(const ClipVertex &v0, const ClipVertex &v1) {
	if (CtxThrough()) {
		binner::AddLine(v0.v, v1.v);
		return;
	}
	const bool depthClip = CtxDepthClip();
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
		ClipToScreen(*actx->ts, n.v, n.clip, true, &outside);
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
		ClipToScreen(*actx->ts, n.v, n.clip, true, &outside);
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
	const CullType cullType = CtxCull();

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

// --- Recorded draws ---

inline const float *BonesOf(const PendingDraw &d) {
	return d.boneOff != ~0u ? pendingBones.data() + d.boneOff : nullptr;
}

// The carry the last vertex (in draw order) of d leaves: its UV if uv, its normal if nrm.
void LastVertexCarry(const PendingDraw &d, bool uv, bool nrm, Carry &cr) {
	SimpleVertex sv;
	const int last = d.inds ? ReadIndex(d.inds, d.fmt.idx, d.count - 1) : d.count - 1;
	DecodeVertex(d.fmt, d.verts, last, &sv, d.morph, BonesOf(d));
	UpdateCarry(tsSnaps[d.tsId], sv, uv, nrm, cr);
}

// Brings carry's UV (uv) and normal (nrm) up to date with the pending draws.
void MaterializeCarry(bool uv, bool nrm) {
	if (uv && uvSource >= 0) {
		const bool both = nrm && nrmSource == uvSource;
		LastVertexCarry(pending[uvSource], true, both, carry);
		uvSource = -1;
		if (both)
			nrmSource = -1;
	}
	if (nrm && nrmSource >= 0) {
		LastVertexCarry(pending[nrmSource], false, true, carry);
		nrmSource = -1;
	}
}

void RecordDraw(const u8 *verts, const u8 *inds, GEPrimitiveType prim, int count, const VertexFormat &fmt, int lower, int upper,
                bool useCache) {
	PendingDraw d;
	// The raster state first: taking it may assemble the draws before (a texture copy must see them).
	d.stateId = binner::SnapshotState();
	if (tsSnaps.empty() || tsSnapSerial != tsSerial) {
		tsSnaps.push_back(ts);
		tsSnapSerial = tsSerial;
	}
	d.tsId = (u32)tsSnaps.size() - 1;
	d.verts = verts;
	d.inds = inds;
	d.fmt = fmt;
	d.prim = prim;
	d.count = count;
	d.lower = lower;
	d.useCache = useCache;
	d.hasUV = fmt.tc != 0;
	d.hasNormal = fmt.nrm != 0;
	d.hasColor = fmt.col != 0;
	memcpy(d.morph, gstate_c.morphWeights, sizeof(d.morph));
	d.boneOff = ~0u;
	if (fmt.weighttype && !fmt.through) {
		d.boneOff = (u32)pendingBones.size();
		pendingBones.insert(pendingBones.end(), gstate.boneMatrix, gstate.boneMatrix + 12 * fmt.nweights);
	}
	// A format without UV or normal takes the last ones read.
	if (!d.hasUV || !d.hasNormal)
		MaterializeCarry(!d.hasUV, !d.hasNormal);
	d.carryIn = carry;

	// Assemble reads every vertex in order, unless the type (as KEEP_PREVIOUS continues it) is invalid.
	GEPrimitiveType type = prim;
	if (prim != GE_PRIM_KEEP_PREVIOUS)
		recPrevPrim = prim;
	else
		type = recPrevPrim;
	const bool assembles = (int)type >= (int)GE_PRIM_POINTS && (int)type <= (int)GE_PRIM_RECTANGLES;
	d.slots = useCache ? (u32)(upper - lower + 1) : assembles ? (u32)count : 0;
	d.slotBase = pendingSlots;
	pendingSlots += d.slots;
	if ((d.hasUV || d.hasNormal) && (useCache || assembles)) {
		if (d.hasUV)
			uvSource = (int)pending.size();
		if (d.hasNormal)
			nrmSource = (int)pending.size();
	}

	const RasterState &st = binner::Snapshot(d.stateId);
	d.ctx.ts = nullptr;
	d.ctx.live = false;
	d.ctx.through = st.throughMode;
	d.ctx.textureProj = st.textureProj;
	d.ctx.depthClip = gstate.isDepthClipEnabled();
	d.ctx.gouraud = gstate.isShadeGouraud();
	d.ctx.clearRect = gstate.isModeClear() && !gstate.isDitherEnabled();
	d.ctx.cull = GECullType();
	pending.push_back(d);
}

// Transforms slots [s0, s1) of d into pendingOut; for the backend, decodes them into its inputs.
void TransformSlots(const PendingDraw &d, u32 s0, u32 s1) {
	const TransformState &s = tsSnaps[d.tsId];
	const float *bones = BonesOf(d);
	ClipVertex *out = pendingOut.data() + d.slotBase;
	GeVertexIn *in = d.gpu ? binner::Geometry().inputs.data() + d.geoBase : nullptr;
	SimpleVertex sv;
	for (u32 slot = s0; slot < s1; ++slot) {
		const int index = d.useCache ? d.lower + (int)slot : d.inds ? ReadIndex(d.inds, d.fmt.idx, (int)slot) : (int)slot;
		DecodeVertex(d.fmt, d.verts, index, &sv, d.morph, bones);
		if (in) {
			GeVertexIn &g = in[slot];
			memcpy(g.uv, sv.uv, sizeof(g.uv));
			g.color = sv.color;
			memcpy(g.nrm, sv.nrm, sizeof(g.nrm));
			memcpy(g.pos, sv.pos, sizeof(g.pos));
			g.draw = d.geoDraw;
			if (!gpuTransformVerify)
				continue;
		}
		Carry c = d.carryIn;
		out[slot] = ReadVertex(s, sv, d.hasUV, d.hasNormal, d.hasColor, c);
	}
}

// Transforms the pending draws' slots [g0, g1).
void TransformRange(u32 g0, u32 g1) {
	// The last draw starting at or before g0.
	auto it = std::upper_bound(pending.begin(), pending.end(), g0, [](u32 v, const PendingDraw &d) { return v < d.slotBase; });
	for (size_t i = (size_t)(it - pending.begin()) - 1; i < pending.size() && g0 < g1; ++i) {
		const PendingDraw &d = pending[i];
		const u32 end = d.slotBase + d.slots;
		if (end <= g0)
			continue;
		const u32 stop = std::min(g1, end);
		TransformSlots(d, g0 - d.slotBase, stop - d.slotBase);
		g0 = stop;
	}
}

constexpr u32 CHUNK_SLOTS = 128;

void TransformPending() {
	const int chunks = (int)((pendingSlots + CHUNK_SLOTS - 1) / CHUNK_SLOTS);
	if (chunks <= 1) {
		TransformRange(0, pendingSlots);
		return;
	}
	static const std::function<void(int)> fn = [](int c) {
		const u32 g0 = (u32)c * CHUNK_SLOTS;
		TransformRange(g0, std::min(g0 + CHUNK_SLOTS, pendingSlots));
	};
	ParallelFor(chunks, fn);
}

inline bool InVram(const u8 *p) {
	const u8 *vram = VramPtr();
	return p >= vram && p < vram + VRAM_SIZE;
}

// --- Geometry the backend transforms (geometry.h) ---

void PackColor(GeColor4i &out, const Color4i &c) {
	memcpy(out.c, c.c, sizeof(out.c));
}

// The light vector's matrix by column (GELightVector): the negated world rows, then translated.
void PackLightVector(const TransformState &s, const float translated[3], float cols[3][4]) {
	for (int c = 0; c < 3; ++c) {
		for (int k = 0; k < 3; ++k)
			cols[c][k] = s.negWorldRows[k][c];
		cols[c][3] = translated[c];
	}
}

void PackTransform(const TransformState &s, GeTransform &g) {
	memset(&g, 0, sizeof(g));
	for (int c = 0; c < 4; ++c) {
		for (int k = 0; k < 4; ++k) {
			g.mtxCols[c][k] = s.matrix[k * 4 + c];
			g.tgenCols[c][k] = s.tgenRows[k][c];
		}
	}
	for (int c = 0; c < 3; ++c) {
		for (int k = 0; k < 3; ++k)
			g.worldCols[c][k] = s.worldRows[k][c];
	}
	memcpy(g.viewZRows, s.viewZRows, sizeof(g.viewZRows));
	memcpy(g.posToFog, s.posToFog, sizeof(g.posToFog));
	memcpy(g.screenScale, s.screenScale, sizeof(s.screenScale));
	memcpy(g.screenAdd, s.screenAdd, sizeof(s.screenAdd));
	memcpy(g.uvScaleF, s.uvScaleF, sizeof(g.uvScaleF));
	memcpy(g.viewDir, s.light.viewDir, sizeof(s.light.viewDir));
	memcpy(g.uvScale, s.uvScale, sizeof(g.uvScale));
	memcpy(g.uvOffset, s.uvOffset, sizeof(g.uvOffset));
	g.fogEnd = s.fogEnd;
	g.fogSlope = s.fogSlope;
	g.specularExp = s.light.specularExp;
	const LightState &l = s.light;
	u32 f = 0;
	if (s.enableLighting) f |= GT_LIGHTING;
	if (s.enableFog) f |= GT_FOG;
	if (s.fogGE) f |= GT_FOG_GE;
	if (s.negateNormals) f |= GT_NEGATE_NORMALS;
	if (s.depthClip) f |= GT_DEPTH_CLIP;
	if (s.geUVScale) f |= GT_GE_UV_SCALE;
	if (s.uvPrescale) f |= GT_UV_PRESCALE;
	f |= (u32)(s.uvGenMode & 3) << GT_UV_GEN_SHIFT;
	f |= (u32)(s.uvProjMode & 3) << GT_UV_PROJ_SHIFT;
	if (l.colorForAmbient) f |= GT_COLOR_AMBIENT;
	if (l.colorForDiffuse) f |= GT_COLOR_DIFFUSE;
	if (l.colorForSpecular) f |= GT_COLOR_SPECULAR;
	if (l.setColor1) f |= GT_SET_COLOR1;
	if (l.addColor1) f |= GT_ADD_COLOR1;
	if (l.usesWorldNormal) f |= GT_WORLD_NORMAL;
	g.flags = f;
	g.offsetX16 = s.offsetX16;
	g.offsetY16 = s.offsetY16;
	g.materialAmbientRGBA = s.materialAmbientRGBA;
	g.materialEmissive = s.materialEmissive;
	PackColor(g.materialAmbient, l.materialAmbient);
	PackColor(g.materialDiffuse, l.materialDiffuse);
	PackColor(g.materialSpecular, l.materialSpecular);
	PackColor(g.baseAmbient, l.baseAmbient);
	for (int i = 0; i < 4; ++i) {
		const LightState::Light &ls = l.lights[i];
		GeLight &gl = g.lights[i];
		memcpy(gl.pos, ls.pos, sizeof(ls.pos));
		PackLightVector(s, ls.translated, gl.vecCols);
		memcpy(gl.att, ls.att, sizeof(ls.att));
		memcpy(gl.spotDir, ls.spotDir, sizeof(ls.spotDir));
		gl.spotDirRsqrt = ls.spotDirRsqrt;
		gl.spotCutoff = ls.spotCutoff;
		gl.spotExp = ls.spotExp;
		u32 lf = 0;
		if (ls.enabled) lf |= GL_ENABLED;
		if (ls.spot) lf |= GL_SPOT;
		if (ls.directional) lf |= GL_DIRECTIONAL;
		if (ls.poweredDiffuse) lf |= GL_POWERED_DIFFUSE;
		if (ls.ambient) lf |= GL_AMBIENT;
		if (ls.diffuse) lf |= GL_DIFFUSE;
		if (ls.specular) lf |= GL_SPECULAR;
		gl.flags = lf;
		PackColor(gl.ambient, ls.ambientColorFactor);
		PackColor(gl.diffuse, ls.diffuseColorFactor);
		PackColor(gl.specular, ls.specularColorFactor);
	}
	for (int k = 0; k < 2; ++k) {
		const TransformState::ShadeLight &sl = s.shade[k];
		GeShadeLight &gs = g.shade[k];
		memcpy(gs.pos, sl.pos, sizeof(sl.pos));
		PackLightVector(s, sl.translated, gs.vecCols);
		gs.flags = (sl.directional ? (u32)GL_DIRECTIONAL : 0u) | (sl.specular ? (u32)GL_SPECULAR : 0u);
	}
}

// The GeTransform of each tsSnaps entry in the batch's geometry (~0u: not packed yet).
std::vector<u32> tsGeo;

// Chooses the pending draws the backend transforms, and makes their draw records and vertex slots.
void PrepareGeometry() {
	const bool backend = binner::TransformsGeometry();
	gpuTransformVerify = backend && binner::VerifiesGeometry();
	GeometryBatch &geo = binner::Geometry();
	tsGeo.assign(tsSnaps.size(), ~0u);
	for (PendingDraw &d : pending) {
		d.gpu = false;
		// Triangles in transform mode that don't continue a draw before.
		if (!backend || d.ctx.through || d.slots == 0 ||
		    (d.prim != GE_PRIM_TRIANGLES && d.prim != GE_PRIM_TRIANGLE_STRIP && d.prim != GE_PRIM_TRIANGLE_FAN))
			continue;
		d.gpu = true;
		d.geoBase = (u32)geo.inputs.size();
		geo.inputs.resize(geo.inputs.size() + d.slots);
		if (tsGeo[d.tsId] == ~0u) {
			tsGeo[d.tsId] = (u32)geo.transforms.size();
			geo.transforms.emplace_back();
			PackTransform(tsSnaps[d.tsId], geo.transforms.back());
		}
		const RasterState &st = binner::Snapshot(d.stateId);
		GeDraw gd{};
		gd.transform = tsGeo[d.tsId];
		u32 f = 0;
		if (d.hasUV) f |= GD_HAS_UV;
		if (d.hasNormal) f |= GD_HAS_NORMAL;
		if (d.hasColor) f |= GD_HAS_COLOR;
		if (d.ctx.depthClip) f |= GD_DEPTH_CLIP;
		if (d.ctx.gouraud) f |= GD_GOURAUD;
		if (st.enableTextures && !st.pix.clearMode) f |= GD_TEXTURED;
		if (st.pix.applyFog) f |= GD_FOG;
		if (st.textureProj && !st.throughMode) f |= GD_PROJ;
		if (st.shadeGouraud) f |= GD_STATE_GOURAUD;
		gd.flags = f;
		gd.texShift = (u32)st.samp.width0Shift | ((u32)st.samp.height0Shift << 8);
		memcpy(gd.carryUV, d.carryIn.uv, sizeof(d.carryIn.uv));
		memcpy(gd.carryNormal, d.carryIn.normal, sizeof(d.carryIn.normal));
		d.geoDraw = (u32)geo.draws.size();
		geo.draws.push_back(gd);
	}
}

// The CPU's transform of a backend draw's slot.
ClipVertex CpuVertex(const PendingDraw &d, u32 slot) {
	if (gpuTransformVerify)
		return pendingOut[d.slotBase + slot];
	const GeVertexIn &g = binner::Geometry().inputs[d.geoBase + slot];
	SimpleVertex sv;
	memcpy(sv.uv, g.uv, sizeof(sv.uv));
	sv.color = g.color;
	memcpy(sv.nrm, g.nrm, sizeof(sv.nrm));
	memcpy(sv.pos, g.pos, sizeof(sv.pos));
	Carry c = d.carryIn;
	return ReadVertex(tsSnaps[d.tsId], sv, d.hasUV, d.hasNormal, d.hasColor, c);
}

// Assemble for a backend draw: the same triangles, as jobs, and the same assembly state left behind
// (data[] as the slots in dataSlot, made real by MaterializeData).
void AssembleJobs(const PendingDraw &d) {
	GeometryBatch &geo = binner::Geometry();
	const u32 firstJob = (u32)geo.jobs.size();
	auto slotOf = [&](int vtx) -> u32 {
		return d.geoBase + (u32)(d.useCache ? ReadIndex(d.inds, d.fmt.idx, vtx) - d.lower : vtx);
	};
	auto job = [&](u32 a, u32 b, u32 c, u32 prov, bool rev) {
		GeJob j{};
		j.v[0] = a;
		j.v[1] = b;
		j.v[2] = c;
		j.provoking = prov;
		j.draw = d.geoDraw;
		j.reversed = rev ? 1 : 0;
		j.out = (u32)geo.jobs.size() * GEO_JOB_VERTS;
		if (gpuTransformVerify) {
			geo.expectFirst.push_back((u32)geo.expect.size());
			const ClipVertex *o = pendingOut.data() + d.slotBase;
			captureTris = &geo.expect;
			ProcessTriangle(o[a - d.geoBase], o[b - d.geoBase], o[c - d.geoBase], o[prov - d.geoBase], rev);
			captureTris = nullptr;
		}
		geo.jobs.push_back(j);
	};
	// SendTriangle.
	auto send = [&](CullType cull, const u32 *v, int provoking, bool rev) {
		const u32 p = v[provoking];
		if (cull == CullType::OFF) {
			job(v[0], v[1], v[2], p, rev);
			job(v[2], v[1], v[0], p, !rev);
		} else if (cull == CullType::CW) {
			job(v[2], v[1], v[0], p, !rev);
		} else {
			job(v[0], v[1], v[2], p, rev);
		}
	};

	const CullType cullType = d.ctx.cull;
	const int count = d.count;
	u32 slots[3] = { ~0u, ~0u, ~0u };
	dataIndex = 0;
	prevPrim = d.prim;
	switch (d.prim) {
	case GE_PRIM_TRIANGLES:
		for (int vtx = 0; vtx < count; ++vtx) {
			slots[dataIndex++] = slotOf(vtx);
			if (dataIndex < 3)
				continue;
			dataIndex = 0;
			send(cullType, slots, 2, false);
		}
		break;
	case GE_PRIM_TRIANGLE_STRIP: {
		int skip_count = 2, start_vtx = 0;
		for (int vtx = 0; vtx < count && skip_count > 0; ++vtx) {
			slots[(dataIndex++) % 3] = slotOf(vtx);
			--skip_count;
			++start_vtx;
		}
		for (int vtx = start_vtx; vtx < count; ++vtx) {
			const int provoking_index = (dataIndex++) % 3;
			slots[provoking_index] = slotOf(vtx);
			const int wind = (dataIndex - 1) % 2;
			const CullType alt = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
			send(alt, slots, provoking_index, wind != 0);
		}
		break;
	}
	case GE_PRIM_TRIANGLE_FAN: {
		int skip_count = 1, start_vtx = 0;
		if (count > 0) {
			slots[0] = slotOf(0);
			dataIndex++;
			start_vtx = 1;
		}
		for (int vtx = start_vtx; vtx < count && skip_count > 0; ++vtx) {
			slots[2 - ((dataIndex++) % 2)] = slotOf(vtx);
			--skip_count;
			++start_vtx;
		}
		for (int vtx = start_vtx; vtx < count; ++vtx) {
			const int provoking_index = 2 - ((dataIndex++) % 2);
			slots[provoking_index] = slotOf(vtx);
			const int wind = (dataIndex - 1) % 2;
			const CullType alt = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
			send(alt, slots, provoking_index, wind != 0);
		}
		break;
	}
	default:
		break;
	}
	for (int i = 0; i < 3; ++i)
		dataSlot[i] = slots[i] == ~0u ? -1 : (int)(slots[i] - d.geoBase);
	dataSlot[3] = -1;
	const u32 jobs = (u32)geo.jobs.size() - firstJob;
	if (jobs != 0)
		binner::AddGeometry(d.geoDraw, firstJob, jobs);
}

// data[] as Assemble would have left it after backend draw d.
void MaterializeData(const PendingDraw &d) {
	for (int i = 0; i < 4; ++i) {
		if (dataSlot[i] >= 0)
			data[i] = CpuVertex(d, (u32)dataSlot[i]);
		dataSlot[i] = -1;
	}
}

}  // namespace

void ResolvePendingDraws() {
	if (pending.empty() || resolvingDraws)
		return;
	resolvingDraws = true;
	// The carry the draws leave, while their vertices are at hand.
	MaterializeCarry(true, true);
	PrepareGeometry();
	if (pendingOut.size() < pendingSlots)
		pendingOut.resize(pendingSlots);
	TransformPending();

	binner::BeginResolve();
	for (size_t i = 0; i < pending.size(); ++i) {
		PendingDraw &d = pending[i];
		d.ctx.ts = &tsSnaps[d.tsId];
		actx = &d.ctx;
		binner::UseSnapshot(d.stateId);
		const ClipVertex *out = pendingOut.data() + d.slotBase;
		isImmDraw = false;
		if (d.gpu) {
			AssembleJobs(d);
			// A draw continuing this one, or what comes after these draws, may read data[].
			if (i + 1 == pending.size() || pending[i + 1].prim == GE_PRIM_KEEP_PREVIOUS)
				MaterializeData(d);
		} else if (d.useCache) {
			const u8 *inds = d.inds;
			const int idx = d.fmt.idx, lower = d.lower;
			Assemble(d.prim, d.count, [&](int vtx) { return out[ReadIndex(inds, idx, vtx) - lower]; });
		} else {
			Assemble(d.prim, d.count, [&](int vtx) { return out[vtx]; });
		}
	}
	binner::EndResolve();
	actx = nullptr;
	pending.clear();
	tsSnaps.clear();
	tsSnapSerial = ~0ull;
	pendingBones.clear();
	pendingSlots = 0;
	resolvingDraws = false;
}

void TransformDirty() {
	tsDirty = true;
}

void ResetPrimAssembly() {
	ResolvePendingDraws();
	dataIndex = 0;
	prevPrim = GE_PRIM_POINTS;
	recPrevPrim = GE_PRIM_POINTS;
	carry = Carry{};
	tsDirty = true;
}

void SubmitPrimitive(const u8 *verts, const u8 *inds, GEPrimitiveType prim, int count, u32 vertType) {
	if (vfmt.vtype != vertType)
		vfmt.Set(vertType);
	const VertexFormat &fmt = vfmt;
	if ((vertType & GE_VTYPE_POS_MASK) == 0)
		return;
	const bool hasColor = fmt.col != 0;
	EnsureTransformState(fmt.through, hasColor, vertType);
	g_stats.vertices += count;

	const bool useIndices = inds != nullptr;
	int lower = 0, upper = count > 0 ? count - 1 : 0;
	if (useIndices && count > 0)
		GetIndexBounds(inds, count, fmt.idx, &lower, &upper);
	// Reusing indexed vertices: transform the range once.
	const bool useCache = useIndices && count > upper - lower + 1;
	RecordDraw(verts, inds, prim, count, fmt, lower, upper, useCache);
	// Vertices in VRAM are read now, on this thread (rendering a GPU keeps may have to come back first).
	if (InVram(verts) || (inds && InVram(inds)))
		ResolvePendingDraws();
}

void SubmitSimpleVertices(const SimpleVertex *verts, const u16 *inds, GEPrimitiveType prim, int count, bool through, bool hasColor, bool hasUV, bool hasNormal) {
	ResolvePendingDraws();
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
		return ReadVertex(ts, sv, hasUV, hasNormal, hasColor, carry);
	};
	const AssemblyCtx ctx = LiveCtx();
	actx = &ctx;
	isImmDraw = false;
	Assemble(prim, count, read);
	actx = nullptr;
	recPrevPrim = prevPrim;
}

void SubmitImmVertex(GEPrimitiveType prim, const RVertex &v, float clipw, bool first) {
	ResolvePendingDraws();
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
	const AssemblyCtx ctx = LiveCtx();
	actx = &ctx;
	isImmDraw = true;
	cullXY = false;
	Assemble(GE_PRIM_KEEP_PREVIOUS, 0, [](int) { return ClipVertex{}; });
	cullXY = true;
	isImmDraw = false;
	actx = nullptr;
	recPrevPrim = prevPrim;
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
