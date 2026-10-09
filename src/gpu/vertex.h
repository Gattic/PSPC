// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Vertex formats (GPU/Common/VertexDecoderCommon) and the transform pipeline's entry points.
#pragma once

#include "common/common.h"
#include "gpu/ge_constants.h"
#include "gpu/render.h"

namespace gpu {

struct VertexFormat {
	u32 vtype = 0xFFFFFFFF;
	bool through;
	int tc, col, nrm, pos, weighttype, idx;
	int morphcount, nweights;
	int weightoff, tcoff, coloff, nrmoff, posoff;
	int onesize;  // one morph frame
	int size;     // onesize * morphcount
	void Set(u32 vtype);
};

// A control point / vertex after morph and skinning, in model space.
struct SimpleVertex {
	float uv[2];
	u32 color;
	float nrm[3];
	float pos[3];
};

// Reads index i (already through the index buffer) of a draw: morph and skinning applied (transform mode).
// hasUV/hasNormal/hasColor come from the format.
void DecodeVertex(const VertexFormat &fmt, const u8 *base, int index, SimpleVertex *out);

// Index buffer helpers.
int ReadIndex(const u8 *inds, int idxType, int i);
void GetIndexBounds(const u8 *inds, int count, int idxType, int *lo, int *hi);

// Transform pipeline (transform.cpp).
void TransformDirty();
void ResetPrimAssembly();
// Draws count vertices of vertType from guest memory. Returns the bytes read (vertices).
void SubmitPrimitive(const u8 *verts, const u8 *inds, GEPrimitiveType prim, int count, u32 vertType);
// Same with vertices already in SimpleVertex form (curves): float position/normal/uv, 8888 color.
void SubmitSimpleVertices(const SimpleVertex *verts, const u16 *inds, GEPrimitiveType prim, int count, bool through, bool hasColor, bool hasUV, bool hasNormal);
// Immediate mode (VAP) vertices, in screen space.
void SubmitImmVertex(GEPrimitiveType prim, const RVertex &v, float clipw, bool first);
// Bounding box test for BOUNDINGBOX: true if possibly visible.
bool TestBoundingBox(const u8 *verts, const u8 *inds, int count, u32 vertType);

// Curves (spline.cpp).
void SubmitBezier(const u8 *verts, const u8 *inds, u32 op, u32 vertType);
void SubmitSpline(const u8 *verts, const u8 *inds, u32 op, u32 vertType);

}  // namespace gpu
