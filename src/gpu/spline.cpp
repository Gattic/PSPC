// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Bezier patches and splines, evaluated as the GE does (GPU/Common/SplineCommon.cpp's TessellateBezierGE and
// TessellateSplineGE, gpu/probe exp48-51, exp139-145), then drawn as triangles, lines or points.
#include <algorithm>
#include <cmath>
#include <vector>

#include "gpu/ge_state.h"
#include "gpu/vertex.h"

namespace gpu {

namespace {

constexpr int MAX_CURVE_VERTICES = 65536;

struct Surface {
	int tess_u, tess_v;
	int num_points_u, num_points_v;
	int num_patches_u, num_patches_v;
	int type_u, type_v;
	int primType;
	bool patchFacing;

	void BaseInit() {
		if (tess_u < 1) tess_u = 1;
		if (tess_v < 1) tess_v = 1;
	}
	void ReduceLargerTess() {
		if (tess_u >= tess_v)
			tess_u--;
		else
			tess_v--;
	}
};

// The parameter k/256, rounded toward the middle of the patch.
int BezierParam256(int i, int n) {
	return 2 * i <= n ? (256 * i) / n : 256 - (256 * (n - i)) / n;
}

// A lerp in 16-bit fixed point at the larger operand's exponent, both truncated toward zero to that grid.
float GEBezierLerp(float a, float b, int k) {
	if (k == 0)
		return a;
	if (k == 256)
		return b;
	const u32 ba = FloatBits(a), bb = FloatBits(b);
	const int ea = (ba >> 23) & 0xFF;
	const int eb = (bb >> 23) & 0xFF;
	const int e = std::max(ea, eb);
	if (e == 0)
		return 0.0f;
	auto toFixed = [e](u32 bits, int ex) -> int {
		if (ex == 0 || e - ex >= 16)
			return 0;
		const int m = (int)(((bits & 0x7FFFFF) | 0x800000) >> (e - ex + 8));
		return (bits & 0x80000000) ? -m : m;
	};
	const int fa = toFixed(ba, ea);
	const int fb = toFixed(bb, eb);
	const int r = fa + (((fb - fa) * k) >> 8);
	if (e <= 15)
		return ldexpf((float)r, e - 127 - 15);
	return (float)r * BitsFloat((u32)(e - 15) << 23);
}

float GEBezierEval(const float p[4], int k, float *ab = nullptr, float *bc = nullptr) {
	const float a = GEBezierLerp(p[0], p[1], k);
	const float b = GEBezierLerp(p[1], p[2], k);
	const float c = GEBezierLerp(p[2], p[3], k);
	const float l = GEBezierLerp(a, b, k);
	const float r = GEBezierLerp(b, c, k);
	if (ab) {
		*ab = l;
		*bc = r;
	}
	return GEBezierLerp(l, r, k);
}

int GEBezierEvalColor(const int p[4], int k) {
	auto lerp = [k](int a, int b) { return a + (((b - a) * k) >> 8); };
	const int a = lerp(p[0], p[1]);
	const int b = lerp(p[1], p[2]);
	const int c = lerp(p[2], p[3]);
	return lerp(lerp(a, b), lerp(b, c));
}

struct GESplineParam {
	int seg;
	int alpha[6];
	int k;
};

// de Boor at t = k/256; each blending factor from the nearer of its two knots (gpu/probe exp144-145).
GESplineParam GESplineParamAt(int index, int tess, int numPatches, int type) {
	GESplineParam p;
	p.seg = index / tess;
	p.k = BezierParam256(index % tess, tess);
	if (p.seg == numPatches) {
		p.seg = numPatches - 1;
		p.k = 256;
	}
	auto knot = [&](int i) {
		if (i < 3 && (type & 1))
			return 0;
		if (i > numPatches + 3 && (type & 2))
			return numPatches;
		return i - 3;
	};
	const int l = p.seg + 3;
	const int is[6] = { l - 2, l - 1, l, l - 1, l, l };
	const int rs[6] = { 1, 1, 1, 2, 2, 3 };
	for (int n = 0; n < 6; ++n) {
		const int i = is[n];
		const int span = knot(i + 4 - rs[n]) - knot(i);
		const int dl = 256 * (p.seg - knot(i)) + p.k;
		const int dr = 256 * span - dl;
		p.alpha[n] = span == 0 ? 0 : (dl <= dr ? dl / span : 256 - dr / span);
	}
	return p;
}

float GESplineEval(const float d[4], const int a[6], float *ab = nullptr, float *bc = nullptr) {
	const float l0 = GEBezierLerp(d[0], d[1], a[0]);
	const float l1 = GEBezierLerp(d[1], d[2], a[1]);
	const float l2 = GEBezierLerp(d[2], d[3], a[2]);
	const float m0 = GEBezierLerp(l0, l1, a[3]);
	const float m1 = GEBezierLerp(l1, l2, a[4]);
	if (ab) {
		*ab = m0;
		*bc = m1;
	}
	return GEBezierLerp(m0, m1, a[5]);
}

int GESplineEvalColor(const int d[4], const int a[6]) {
	auto lerp = [](int x, int y, int k) { return x + (((y - x) * k) >> 8); };
	const int l0 = lerp(d[0], d[1], a[0]);
	const int l1 = lerp(d[1], d[2], a[1]);
	const int l2 = lerp(d[2], d[3], a[2]);
	return lerp(lerp(l0, l1, a[3]), lerp(l1, l2, a[4]), a[5]);
}

struct Column {
	float pos[3];
	float ab[3], bc[3];
	float tex[2];
	int col[4];
};

struct Flags {
	bool sampleNrm, sampleCol, sampleTex, patchFacing;
};

// Normal from the tangents: the cross product as the matrix unit sums it.
void CrossNormal(const float tu[3], const float tv[3], bool facing, float out[3]) {
	for (int j = 0; j < 3; ++j) {
		const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
		const GERowTerm terms[2] = { GEProduct(tu[j1], tv[j2]), GEProduct(-tu[j2], tv[j1]) };
		out[j] = GERowSum(terms, 2);
		if (facing)
			out[j] = -out[j];
	}
}

void FinishVertex(SimpleVertex &vert, const Flags &f, const Column *cols, const float tu[3], const float tv[3], u32 defcolor) {
	(void)cols;
	if (f.sampleNrm) {
		CrossNormal(tu, tv, f.patchFacing, vert.nrm);
	} else {
		vert.nrm[0] = vert.nrm[1] = 0.0f;
		vert.nrm[2] = 1.0f;
	}
	if (!f.sampleCol)
		vert.color = defcolor;
}

// One quad's indices: triangles (0,2,1)(1,2,3), or for lines a zigzag (gpu/probe exp175).
void CopyQuadIndex(std::vector<u16> &idx, int type, int idx0, int idx1, int idx2, int idx3) {
	if (type == GE_PATCHPRIM_LINES) {
		const u16 v[6] = { (u16)idx0, (u16)idx2, (u16)idx2, (u16)idx1, (u16)idx1, (u16)idx3 };
		idx.insert(idx.end(), v, v + 6);
	} else {
		const u16 v[6] = { (u16)idx0, (u16)idx2, (u16)idx1, (u16)idx1, (u16)idx2, (u16)idx3 };
		idx.insert(idx.end(), v, v + 6);
	}
}

void BuildIndex(std::vector<u16> &idx, int num_u, int num_v, int type, int total) {
	for (int v = 0; v < num_v; ++v) {
		for (int u = 0; u < num_u; ++u) {
			const int idx0 = v * (num_u + 1) + u + total;
			const int idx2 = (v + 1) * (num_u + 1) + u + total;
			CopyQuadIndex(idx, type, idx0, idx0 + 1, idx2, idx2 + 1);
		}
	}
}

std::vector<SimpleVertex> controlPoints;
std::vector<SimpleVertex> outVerts;
std::vector<u16> outIndices;
VertexFormat curveFmt;

// Decodes the control points (morph and skinning applied).
bool LoadControlPoints(const u8 *verts, const u8 *inds, int numPoints, u32 vertType, Flags &f) {
	if (curveFmt.vtype != vertType)
		curveFmt.Set(vertType);
	controlPoints.resize(numPoints);
	const u32 defColor = gstate.getMaterialAmbientRGBA();
	for (int i = 0; i < numPoints; ++i) {
		const int index = inds ? ReadIndex(inds, curveFmt.idx, i) : i;
		SimpleVertex &sv = controlPoints[i];
		DecodeVertex(curveFmt, verts, index, &sv);
		if (!curveFmt.col)
			sv.color = defColor;
		if (!curveFmt.nrm) {
			sv.nrm[0] = sv.nrm[1] = 0.0f;
			sv.nrm[2] = 1.0f;
		}
	}
	// Shade mapping uses the normal even with lighting off (gpu/probe exp143).
	f.sampleNrm = curveFmt.nrm != 0 || gstate.isLightingEnabled() || gstate.getUVGenMode() == GE_TEXMAP_ENVIRONMENT_MAP;
	f.sampleCol = curveFmt.col != 0;
	f.sampleTex = curveFmt.tc != 0;
	return true;
}

void TessellateBezier(const Surface &s, const Flags &f) {
	const int vertsPerPatch = (s.tess_u + 1) * (s.tess_v + 1);
	outVerts.assign((size_t)vertsPerPatch * s.num_patches_u * s.num_patches_v, SimpleVertex{});
	outIndices.clear();
	std::vector<Column> columns((s.tess_v + 1) * 4);
	const u32 defcolor = controlPoints[0].color;
	for (int patch_u = 0; patch_u < s.num_patches_u; ++patch_u) {
		for (int patch_v = 0; patch_v < s.num_patches_v; ++patch_v) {
			const SimpleVertex *patch = controlPoints.data() + patch_v * 3 * s.num_points_u + patch_u * 3;
			const SimpleVertex *rows[4] = { patch, patch + s.num_points_u, patch + s.num_points_u * 2, patch + s.num_points_u * 3 };
			for (int tile_v = 0; tile_v <= s.tess_v; ++tile_v) {
				const int kv = BezierParam256(tile_v, s.tess_v);
				for (int c = 0; c < 4; ++c) {
					Column &column = columns[tile_v * 4 + c];
					for (int j = 0; j < 3; ++j) {
						const float p[4] = { rows[0][c].pos[j], rows[1][c].pos[j], rows[2][c].pos[j], rows[3][c].pos[j] };
						column.pos[j] = GEBezierEval(p, kv, &column.ab[j], &column.bc[j]);
					}
					if (f.sampleTex) {
						for (int j = 0; j < 2; ++j) {
							const float p[4] = { rows[0][c].uv[j], rows[1][c].uv[j], rows[2][c].uv[j], rows[3][c].uv[j] };
							column.tex[j] = GEBezierEval(p, kv);
						}
					}
					if (f.sampleCol) {
						for (int j = 0; j < 4; ++j) {
							int p[4];
							for (int r = 0; r < 4; ++r)
								p[r] = (int)(((rows[r][c].color >> (8 * j)) & 0xFF) << 7) | 0x7F;
							column.col[j] = GEBezierEvalColor(p, kv);
						}
					}
				}
			}
			const int patchBase = (patch_v * s.num_patches_u + patch_u) * vertsPerPatch;
			for (int tile_u = 0; tile_u <= s.tess_u; ++tile_u) {
				const int ku = BezierParam256(tile_u, s.tess_u);
				for (int tile_v = 0; tile_v <= s.tess_v; ++tile_v) {
					SimpleVertex &vert = outVerts[patchBase + tile_v * (s.tess_u + 1) + tile_u];
					const Column *cols = &columns[tile_v * 4];
					float tu[3], tv[3];
					for (int j = 0; j < 3; ++j) {
						const float row[4] = { cols[0].pos[j], cols[1].pos[j], cols[2].pos[j], cols[3].pos[j] };
						float ab, bc;
						vert.pos[j] = GEBezierEval(row, ku, &ab, &bc);
						if (f.sampleNrm) {
							tu[j] = GEAdd(bc, -ab);
							const float abRow[4] = { cols[0].ab[j], cols[1].ab[j], cols[2].ab[j], cols[3].ab[j] };
							const float bcRow[4] = { cols[0].bc[j], cols[1].bc[j], cols[2].bc[j], cols[3].bc[j] };
							tv[j] = GEAdd(GEBezierEval(bcRow, ku), -GEBezierEval(abRow, ku));
						}
					}
					if (f.sampleCol) {
						u32 color = 0;
						for (int j = 0; j < 4; ++j) {
							const int row[4] = { cols[0].col[j], cols[1].col[j], cols[2].col[j], cols[3].col[j] };
							color |= (u32)(GEBezierEvalColor(row, ku) >> 7) << (8 * j);
						}
						vert.color = color;
					}
					if (f.sampleTex) {
						for (int j = 0; j < 2; ++j) {
							const float row[4] = { cols[0].tex[j], cols[1].tex[j], cols[2].tex[j], cols[3].tex[j] };
							vert.uv[j] = GEBezierEval(row, ku);
						}
					} else {
						// Generated: the parameter itself (exp140).
						vert.uv[0] = patch_u + ku * (1.0f / 256.0f);
						vert.uv[1] = patch_v + BezierParam256(tile_v, s.tess_v) * (1.0f / 256.0f);
					}
					FinishVertex(vert, f, cols, tu, tv, defcolor);
				}
			}
		}
	}
	for (int patch_u = 0; patch_u < s.num_patches_u; ++patch_u) {
		for (int patch_v = 0; patch_v < s.num_patches_v; ++patch_v) {
			const int patch_index = patch_v * s.num_patches_u + patch_u;
			BuildIndex(outIndices, s.tess_u, s.tess_v, s.primType, patch_index * vertsPerPatch);
		}
	}
}

void TessellateSpline(const Surface &s, const Flags &f) {
	const int nu = s.num_patches_u * s.tess_u + 1;
	const int nv = s.num_patches_v * s.tess_v + 1;
	outVerts.assign((size_t)nu * nv, SimpleVertex{});
	outIndices.clear();
	std::vector<GESplineParam> paramsU(nu);
	for (int i = 0; i < nu; ++i)
		paramsU[i] = GESplineParamAt(i, s.tess_u, s.num_patches_u, s.type_u);
	std::vector<Column> columns(s.num_points_u);
	const u32 defcolor = controlPoints[0].color;
	for (int iv = 0; iv < nv; ++iv) {
		const GESplineParam pv = GESplineParamAt(iv, s.tess_v, s.num_patches_v, s.type_v);
		for (int c = 0; c < s.num_points_u; ++c) {
			Column &column = columns[c];
			int idx[4];
			for (int r = 0; r < 4; ++r)
				idx[r] = (pv.seg + r) * s.num_points_u + c;
			for (int j = 0; j < 3; ++j) {
				const float p[4] = { controlPoints[idx[0]].pos[j], controlPoints[idx[1]].pos[j], controlPoints[idx[2]].pos[j], controlPoints[idx[3]].pos[j] };
				column.pos[j] = GESplineEval(p, pv.alpha, &column.ab[j], &column.bc[j]);
			}
			if (f.sampleTex) {
				for (int j = 0; j < 2; ++j) {
					const float p[4] = { controlPoints[idx[0]].uv[j], controlPoints[idx[1]].uv[j], controlPoints[idx[2]].uv[j], controlPoints[idx[3]].uv[j] };
					column.tex[j] = GESplineEval(p, pv.alpha);
				}
			}
			if (f.sampleCol) {
				for (int j = 0; j < 4; ++j) {
					int p[4];
					for (int r = 0; r < 4; ++r)
						p[r] = (int)(((controlPoints[idx[r]].color >> (8 * j)) & 0xFF) << 7) | 0x7F;
					column.col[j] = GESplineEvalColor(p, pv.alpha);
				}
			}
		}
		for (int iu = 0; iu < nu; ++iu) {
			const GESplineParam &pu = paramsU[iu];
			const Column *cols = &columns[pu.seg];
			SimpleVertex &vert = outVerts[iv * nu + iu];
			float tu[3], tv[3];
			for (int j = 0; j < 3; ++j) {
				const float row[4] = { cols[0].pos[j], cols[1].pos[j], cols[2].pos[j], cols[3].pos[j] };
				float ab, bc;
				vert.pos[j] = GESplineEval(row, pu.alpha, &ab, &bc);
				if (f.sampleNrm) {
					tu[j] = GEAdd(bc, -ab);
					const float abRow[4] = { cols[0].ab[j], cols[1].ab[j], cols[2].ab[j], cols[3].ab[j] };
					const float bcRow[4] = { cols[0].bc[j], cols[1].bc[j], cols[2].bc[j], cols[3].bc[j] };
					tv[j] = GEAdd(GESplineEval(bcRow, pu.alpha), -GESplineEval(abRow, pu.alpha));
				}
			}
			if (f.sampleCol) {
				u32 color = 0;
				for (int j = 0; j < 4; ++j) {
					const int row[4] = { cols[0].col[j], cols[1].col[j], cols[2].col[j], cols[3].col[j] };
					color |= (u32)(GESplineEvalColor(row, pu.alpha) >> 7) << (8 * j);
				}
				vert.color = color;
			}
			if (f.sampleTex) {
				for (int j = 0; j < 2; ++j) {
					const float row[4] = { cols[0].tex[j], cols[1].tex[j], cols[2].tex[j], cols[3].tex[j] };
					vert.uv[j] = GESplineEval(row, pu.alpha);
				}
			} else {
				vert.uv[0] = pu.seg + pu.k * (1.0f / 256.0f);
				vert.uv[1] = pv.seg + pv.k * (1.0f / 256.0f);
			}
			FinishVertex(vert, f, cols, tu, tv, defcolor);
		}
	}
	BuildIndex(outIndices, s.num_patches_u * s.tess_u, s.num_patches_v * s.tess_v, s.primType, 0);
}

void Draw(const Surface &s, u32 vertType) {
	if (outIndices.empty())
		return;
	const bool through = (vertType & GE_VTYPE_THROUGH) != 0;
	SubmitSimpleVertices(outVerts.data(), outIndices.data(), PatchPrimToPrim(s.primType), (int)outIndices.size(), through, true, true, true);
}

}  // namespace

void SubmitBezier(const u8 *verts, const u8 *inds, u32 op, u32 vertType) {
	Surface s{};
	s.tess_u = gstate.getPatchDivisionU();
	s.tess_v = gstate.getPatchDivisionV();
	s.num_points_u = op & 0xFF;
	s.num_points_v = (op >> 8) & 0xFF;
	// Hardware draws nothing with fewer than 4 points either way.
	if (s.num_points_u < 4 || s.num_points_v < 4)
		return;
	s.num_patches_u = (s.num_points_u - 1) / 3;
	s.num_patches_v = (s.num_points_v - 1) / 3;
	s.primType = gstate.getPatchPrimitiveType();
	s.patchFacing = gstate.patchfacing & 1;
	s.BaseInit();
	while ((s.tess_u + 1) * (s.tess_v + 1) * s.num_patches_u * s.num_patches_v > MAX_CURVE_VERTICES && (s.tess_u > 1 || s.tess_v > 1))
		s.ReduceLargerTess();
	Flags f;
	LoadControlPoints(verts, inds, s.num_points_u * s.num_points_v, vertType, f);
	f.patchFacing = s.patchFacing;
	TessellateBezier(s, f);
	Draw(s, vertType);
}

void SubmitSpline(const u8 *verts, const u8 *inds, u32 op, u32 vertType) {
	Surface s{};
	s.tess_u = gstate.getPatchDivisionU();
	s.tess_v = gstate.getPatchDivisionV();
	s.type_u = (op >> 16) & 0x3;
	s.type_v = (op >> 18) & 0x3;
	s.num_points_u = op & 0xFF;
	s.num_points_v = (op >> 8) & 0xFF;
	if (s.num_points_u < 4 || s.num_points_v < 4)
		return;
	s.num_patches_u = s.num_points_u - 3;
	s.num_patches_v = s.num_points_v - 3;
	s.primType = gstate.getPatchPrimitiveType();
	s.patchFacing = gstate.patchfacing & 1;
	s.BaseInit();
	while ((s.num_patches_u * s.tess_u + 1) * (s.num_patches_v * s.tess_v + 1) > MAX_CURVE_VERTICES && (s.tess_u > 1 || s.tess_v > 1))
		s.ReduceLargerTess();
	Flags f;
	LoadControlPoints(verts, inds, s.num_points_u * s.num_points_v, vertType, f);
	f.patchFacing = s.patchFacing;
	TessellateSpline(s, f);
	Draw(s, vertType);
}

}  // namespace gpu
