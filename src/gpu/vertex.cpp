// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Vertex decoding, with morphing and skinning in the GE's arithmetic (GPU/Software/TransformUnit.cpp's
// ApplyGEMorph/ApplyGESkinning, GPU/Common/VertexDecoderCommon.cpp for the layout).
#include "gpu/vertex.h"

#include <algorithm>
#include <cmath>

#include "gpu/color.h"
#include "gpu/ge_state.h"

namespace gpu {

static const u8 tcsize[4] = { 0, 2, 4, 8 }, tcalign[4] = { 0, 1, 2, 4 };
static const u8 colsize[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }, colalign[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
static const u8 nrmsize[4] = { 0, 3, 6, 12 }, nrmalign[4] = { 0, 1, 2, 4 };
static const u8 possize[4] = { 3, 3, 6, 12 }, posalign[4] = { 1, 1, 2, 4 };
static const u8 wtsize[4] = { 0, 1, 2, 4 }, wtalign[4] = { 0, 1, 2, 4 };

static inline int AlignTo(int v, int a) {
	return a <= 1 ? v : (v + a - 1) & ~(a - 1);
}

void VertexFormat::Set(u32 fmt) {
	vtype = fmt;
	through = (fmt & GE_VTYPE_THROUGH) != 0;
	tc = fmt & 0x3;
	col = (fmt >> 2) & 0x7;
	nrm = (fmt >> 5) & 0x3;
	pos = (fmt >> 7) & 0x3;
	weighttype = (fmt >> 9) & 0x3;
	idx = (fmt >> 11) & 0x3;
	morphcount = ((fmt >> 18) & 0x7) + 1;
	nweights = ((fmt >> 14) & 0x7) + 1;

	int s = 0, biggest = 0;
	weightoff = tcoff = coloff = nrmoff = posoff = 0;
	if (weighttype) {
		weightoff = s;
		s += wtsize[weighttype] * nweights;
		biggest = std::max(biggest, (int)wtalign[weighttype]);
	}
	if (tc) {
		s = AlignTo(s, tcalign[tc]);
		tcoff = s;
		s += tcsize[tc];
		biggest = std::max(biggest, (int)tcalign[tc]);
	}
	if (col) {
		s = AlignTo(s, colalign[col]);
		coloff = s;
		s += colsize[col];
		biggest = std::max(biggest, (int)colalign[col]);
	}
	if (nrm) {
		s = AlignTo(s, nrmalign[nrm]);
		nrmoff = s;
		s += nrmsize[nrm];
		biggest = std::max(biggest, (int)nrmalign[nrm]);
	}
	s = AlignTo(s, posalign[pos]);
	posoff = s;
	s += possize[pos];
	biggest = std::max(biggest, (int)posalign[pos]);
	s = AlignTo(s, biggest);
	onesize = s;
	size = s * morphcount;
}

static inline float ReadF32(const u8 *p) {
	float f;
	memcpy(&f, p, 4);
	return f;
}

static inline u16 ReadU16(const u8 *p) {
	u16 v;
	memcpy(&v, p, 2);
	return v;
}

// Signed components: positions and normals.
static inline float ReadRawComponent(const u8 *p, int fmt, int i) {
	switch (fmt) {
	case 1: return ((const s8 *)p)[i] * (1.0f / 128.0f);
	case 2: return (s16)ReadU16(p + 2 * i) * (1.0f / 32768.0f);
	case 3: return ReadF32(p + 4 * i);
	default: return 0.0f;
	}
}

// Unsigned components: weights and UVs.
static inline float ReadRawWeight(const u8 *p, int fmt, int i) {
	switch (fmt) {
	case 1: return p[i] * (1.0f / 128.0f);
	case 2: return ReadU16(p + 2 * i) * (1.0f / 32768.0f);
	case 3: return ReadF32(p + 4 * i);
	default: return 0.0f;
	}
}

static inline u32 ReadColor(const u8 *p, int col) {
	switch (col) {
	case 4: return RGB565ToRGBA8888(ReadU16(p));
	case 5: return RGBA5551ToRGBA8888(ReadU16(p));
	case 6: return RGBA4444ToRGBA8888(ReadU16(p));
	case 7: { u32 c; memcpy(&c, p, 4); return c; }
	default: return 0;
	}
}

// One morphed component: each frame's value times its weight as a float24, summed in frame order with
// the GE's adder (gpu/probe exp67, exp107).
template <typename Read>
static float GEMorphComponent(const VertexFormat &fmt, const u8 *in, int off, int c, const float *morphWeights, Read read) {
	float acc = 0.0f;
	for (int k = 0; k < fmt.morphcount; ++k) {
		const float v = TruncateToFloat24(read(in + k * fmt.onesize + off, c));
		const float w = TruncateToFloat24(morphWeights[k]);
		const float term = v == 0.0f || w == 0.0f ? 0.0f : ProductToFloat24((double)v * w);
		if (term != 0.0f)
			acc = acc == 0.0f ? term : TruncateToFloat24(GEAdd(acc, term));
	}
	return acc;
}

static inline float GEMul24(float a, float b) {
	a = TruncateToFloat24(a);
	b = TruncateToFloat24(b);
	if (a == 0.0f || b == 0.0f)
		return 0.0f;
	return ProductToFloat24((double)a * b);
}

void DecodeVertex(const VertexFormat &fmt, const u8 *base, int index, SimpleVertex *out) {
	DecodeVertex(fmt, base, index, out, gstate_c.morphWeights, gstate.boneMatrix);
}

void DecodeVertex(const VertexFormat &fmt, const u8 *base, int index, SimpleVertex *out, const float *morphWeights,
                  const float *boneMatrix) {
	const u8 *in = base + (size_t)index * fmt.size;
	const bool morph = fmt.morphcount > 1 && !fmt.through;
	auto component = [&](int off, int c, auto read) {
		return morph ? GEMorphComponent(fmt, in, off, c, morphWeights, read) : read(in + off, c);
	};

	// Texture coordinates.
	out->uv[0] = out->uv[1] = 0.0f;
	if (fmt.tc) {
		if (fmt.through) {
			const u8 *p = in + fmt.tcoff;
			switch (fmt.tc) {
			case 1: out->uv[0] = p[0] * (1.0f / 128.0f); out->uv[1] = p[1] * (1.0f / 128.0f); break;
			case 2: out->uv[0] = ReadU16(p); out->uv[1] = ReadU16(p + 2); break;
			case 3: out->uv[0] = ReadF32(p); out->uv[1] = ReadF32(p + 4); break;
			}
		} else {
			for (int c = 0; c < 2; ++c)
				out->uv[c] = component(fmt.tcoff, c, [&](const u8 *p, int i) { return ReadRawWeight(p, fmt.tc, i); });
		}
	}

	// Color.
	out->color = 0;
	if (fmt.col) {
		if (fmt.morphcount > 1 && fmt.col >= 4) {
			if (!fmt.through) {
				// Per channel expanded to 8 bits, morphed, floored.
				for (int c = 0; c < 4; ++c) {
					const float ch = GEMorphComponent(fmt, in, fmt.coloff, c, morphWeights, [&](const u8 *p, int i) {
						return (float)((ReadColor(p, fmt.col) >> (8 * i)) & 0xFF);
					});
					out->color |= (u32)std::clamp((int)std::floor(ch), 0, 255) << (8 * c);
				}
			} else {
				float acc[4] = {};
				for (int k = 0; k < fmt.morphcount; ++k) {
					const u32 cv = ReadColor(in + k * fmt.onesize + fmt.coloff, fmt.col);
					for (int c = 0; c < 4; ++c)
						acc[c] += (float)((cv >> (8 * c)) & 0xFF) * morphWeights[k];
				}
				for (int c = 0; c < 4; ++c)
					out->color |= (u32)std::clamp((int)acc[c], 0, 255) << (8 * c);
			}
		} else {
			out->color = ReadColor(in + fmt.coloff, fmt.col);
		}
	}

	// Normal and position.
	float nrm[3] = { 0.0f, 0.0f, 0.0f };
	float pos[3] = { 0.0f, 0.0f, 0.0f };
	if (fmt.nrm) {
		for (int c = 0; c < 3; ++c)
			nrm[c] = component(fmt.nrmoff, c, [&](const u8 *p, int i) { return ReadRawComponent(p, fmt.nrm, i); });
	}
	if (fmt.through) {
		const u8 *p = in + fmt.posoff;
		switch (fmt.pos) {
		case 2:
			pos[0] = (s16)ReadU16(p);
			pos[1] = (s16)ReadU16(p + 2);
			pos[2] = ReadU16(p + 4);
			break;
		case 3: {
			pos[0] = ReadF32(p);
			pos[1] = ReadF32(p + 4);
			// Depth is an integer in through mode: truncated, clamped to 16 bits (NaN becomes 0).
			const float z = ReadF32(p + 8);
			pos[2] = z >= 65535.0f ? 65535.0f : (z > 0.0f ? (float)(int)z : 0.0f);
			break;
		}
		default:
			// 8-bit positions decode to 0 in through mode.
			break;
		}
	} else {
		for (int c = 0; c < 3; ++c) {
			pos[c] = component(fmt.posoff, c, [&](const u8 *p, int i) { return ReadRawComponent(p, fmt.pos, i); });
			if (fmt.pos == 3 && !std::isfinite(pos[c]))
				pos[c] = std::isnan(pos[c]) ? 0.0f : (pos[c] > 0 ? FLT_MAX : -FLT_MAX);
		}
	}

	if (fmt.weighttype && !fmt.through) {
		// Skinning as the GE does it (gpu/probe exp68, exp70, exp71): each bone scaled by its weight, every
		// entry a float24 product, one accumulator through all bones in order: translation, then x, y, z.
		float weights[8];
		for (int b = 0; b < fmt.nweights; ++b)
			weights[b] = TruncateToFloat24(component(fmt.weightoff, b, [&](const u8 *p, int i) { return ReadRawWeight(p, fmt.weighttype, i); }));
		float accPos[3] = { 0.0f, 0.0f, 0.0f }, accNrm[3] = { 0.0f, 0.0f, 0.0f };
		for (int b = 0; b < fmt.nweights; ++b) {
			const float w = weights[b];
			if (w == 0.0f)
				continue;
			const float *m = boneMatrix + b * 12;
			float rows[4][3];
			for (int j = 0; j < 4; ++j)
				for (int c = 0; c < 3; ++c)
					rows[j][c] = GEMul24(w, m[3 * j + c]);
			for (int c = 0; c < 3; ++c)
				accPos[c] = GEAddFloat24(accPos[c], rows[3][c]);
			for (int j = 0; j < 3; ++j) {
				for (int c = 0; c < 3; ++c) {
					accPos[c] = GEAddFloat24(accPos[c], GEMul24(pos[j], rows[j][c]));
					if (fmt.nrm)
						accNrm[c] = GEAddFloat24(accNrm[c], GEMul24(nrm[j], rows[j][c]));
				}
			}
		}
		memcpy(pos, accPos, sizeof(pos));
		if (fmt.nrm)
			memcpy(nrm, accNrm, sizeof(nrm));
	}

	memcpy(out->pos, pos, sizeof(pos));
	memcpy(out->nrm, nrm, sizeof(nrm));
}

int ReadIndex(const u8 *inds, int idxType, int i) {
	switch (idxType) {
	case 1: return inds[i];
	case 2: return ReadU16(inds + 2 * i);
	case 3: { u32 v; memcpy(&v, inds + 4 * i, 4); return (int)(v & 0xFFFF); }
	default: return i;
	}
}

void GetIndexBounds(const u8 *inds, int count, int idxType, int *lo, int *hi) {
	int l = 0x7FFFFFFF, h = 0;
	for (int i = 0; i < count; ++i) {
		const int v = ReadIndex(inds, idxType, i);
		l = std::min(l, v);
		h = std::max(h, v);
	}
	if (count == 0)
		l = 0;
	*lo = l;
	*hi = h;
}

}  // namespace gpu
