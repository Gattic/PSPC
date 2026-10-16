// SPDX-License-Identifier: GPL-2.0-or-later
//
// Vertex transform and triangle assembly on the GPU, for a backend that says it can
// (RenderBackend::TransformsGeometry). transform.cpp keeps the draws it can hand over (transform-mode
// triangles, strips and fans) out of its own transform: it decodes their vertices (morph and skinning
// stay on the CPU), and for every triangle ProcessTriangle would see it adds a job. The backend runs
// ReadVertex on each vertex and ProcessTriangle, the binner's winding test and DrawData::EmitTriangle on
// each job in compute shaders, with the GE's arithmetic done bit for bit as the CPU does it, and draws
// the result in place of the batch's PrimType::Geometry primitives.
//
// Each job writes GEO_JOB_VERTS GPU vertices (two triangles, the most near-plane clipping makes);
// triangles it doesn't make are degenerate and draw nothing. The layouts below match the HLSL in
// hw/d3d11_geometry.cpp.
#pragma once

#include <vector>

#include "common/common.h"
#include "gpu/render.h"

namespace gpu {

constexpr u32 GEO_JOB_VERTS = 6;

// A decoded vertex (SimpleVertex) and its draw.
struct GeVertexIn {
	float uv[2];
	u32 color;
	float nrm[3];
	float pos[3];
	u32 draw;
};
static_assert(sizeof(GeVertexIn) == 40, "GeVertexIn layout");

struct GeColor4i {
	s32 c[4];
};

enum : u32 {
	GL_ENABLED = 1,
	GL_SPOT = 2,
	GL_DIRECTIONAL = 4,
	GL_POWERED_DIFFUSE = 8,
	GL_AMBIENT = 16,
	GL_DIFFUSE = 32,
	GL_SPECULAR = 64,
};

// Matrices are kept by column (the shader takes one column per row sum): column c of a 4-row matrix
// is (m[0][c], m[1][c], m[2][c], m[3][c]).
struct GeLight {
	float pos[4];
	float vecCols[3][4];  // the light vector's matrix: the negated world rows, then the translated light
	float att[4];
	float spotDir[4];
	float spotDirRsqrt, spotCutoff, spotExp;
	u32 flags;  // GL_*
	GeColor4i ambient, diffuse, specular;
};
static_assert(sizeof(GeLight) == 160, "GeLight layout");

struct GeShadeLight {
	float pos[4];
	float vecCols[3][4];
	u32 flags;  // GL_DIRECTIONAL, GL_SPECULAR
	u32 pad[3];
};
static_assert(sizeof(GeShadeLight) == 80, "GeShadeLight layout");

enum : u32 {
	GT_LIGHTING = 1u << 0,
	GT_FOG = 1u << 1,
	GT_FOG_GE = 1u << 2,
	GT_NEGATE_NORMALS = 1u << 3,
	GT_DEPTH_CLIP = 1u << 4,
	GT_GE_UV_SCALE = 1u << 5,
	GT_UV_PRESCALE = 1u << 6,
	GT_UV_GEN_SHIFT = 8,      // 2 bits, GE_TEXMAP_*
	GT_UV_PROJ_SHIFT = 10,    // 2 bits, GE_PROJMAP_*
	GT_COLOR_AMBIENT = 1u << 12,
	GT_COLOR_DIFFUSE = 1u << 13,
	GT_COLOR_SPECULAR = 1u << 14,
	GT_SET_COLOR1 = 1u << 15,
	GT_ADD_COLOR1 = 1u << 16,
	GT_WORLD_NORMAL = 1u << 17,
};

// transform.cpp's TransformState.
struct GeTransform {
	float mtxCols[4][4];
	float tgenCols[4][4];
	float worldCols[3][4];
	float viewZRows[4];
	float posToFog[4];
	float screenScale[4];
	float screenAdd[4];
	float uvScaleF[4];
	float viewDir[4];
	float uvScale[2], uvOffset[2];
	float fogEnd, fogSlope, specularExp;
	u32 flags;  // GT_*
	s32 offsetX16, offsetY16;
	u32 materialAmbientRGBA, materialEmissive;
	GeColor4i materialAmbient, materialDiffuse, materialSpecular, baseAmbient;
	GeLight lights[4];
	GeShadeLight shade[2];
};
static_assert(sizeof(GeTransform) == 1184, "GeTransform layout");

enum : u32 {
	GD_HAS_UV = 1,
	GD_HAS_NORMAL = 2,
	GD_HAS_COLOR = 4,
	GD_DEPTH_CLIP = 8,
	GD_GOURAUD = 16,
	GD_TEXTURED = 32,  // and not clear mode
	GD_FOG = 64,       // PixelState::applyFog
	GD_PROJ = 128,     // texture projection
	GD_STATE_GOURAUD = 256,  // RasterState::shadeGouraud (EmitTriangle; GD_GOURAUD is AddTriangleFlat's)
};

struct GeDraw {
	u32 transform;  // in GeometryBatch::transforms
	u32 flags;      // GD_*
	u32 state;      // the batch's state index (set when its primitive is queued)
	u32 texShift;   // width0Shift | height0Shift << 8
	float carryUV[4];
	float carryNormal[4];
};
static_assert(sizeof(GeDraw) == 48, "GeDraw layout");

// One ProcessTriangle call.
struct GeJob {
	u32 v[3];       // vertices, in GeometryBatch::inputs
	u32 provoking;
	u32 draw;
	u32 reversed;
	u32 out;        // first of its GEO_JOB_VERTS GPU vertices
	u32 pad;
};
static_assert(sizeof(GeJob) == 32, "GeJob layout");

// What a batch's PrimType::Geometry primitives refer to (backend.h GeoFirstJob, GeoJobCount).
struct GeometryBatch {
	std::vector<GeVertexIn> inputs;
	std::vector<GeDraw> draws;
	std::vector<GeTransform> transforms;
	std::vector<GeJob> jobs;
	// PSPC_GPU_TRANSFORM_VERIFY: the triangles (after the winding test) transform.cpp made of each job,
	// expectFirst[j] .. expectFirst[j + 1] in expect.
	std::vector<RVertex> expect;
	std::vector<u32> expectFirst;

	bool Empty() const { return jobs.empty(); }
	void Clear() {
		inputs.clear();
		draws.clear();
		transforms.clear();
		jobs.clear();
		expect.clear();
		expectFirst.clear();
	}
};

}  // namespace gpu
