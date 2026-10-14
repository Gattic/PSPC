// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#pragma once

#include "gpu/backend.h"

namespace gpu::hw {
// Maximum atlas side used by the hardware renderers.
constexpr int MAX_TEXTURE_SIDE = 16384;

// The factor textures of an atlas this size are scaled up by, at texture upscaling `setting` (1 = off):
// within the maximum atlas size.
inline int TextureUpscale(int setting, int atlasW, int atlasH) {
	int f = setting;
	while (f > 1 && (atlasW * f > MAX_TEXTURE_SIDE || atlasH * f > MAX_TEXTURE_SIDE))
		--f;
	return f > 1 ? f : 1;
}

// Per-draw state for the pixel shader; matches DrawState in the HLSL.
struct DrawStateGpu {
	u32 flags;
	u32 alphaTest;
	u32 colorTestFunc;
	u32 colorTestRef;
	u32 colorTestMask;
	u32 stencilTest;
	u32 stencilOps;
	u32 depthFunc;
	u32 minz;
	u32 maxz;
	u32 blend;
	u32 fixA;
	u32 fixB;
	u32 fogColor;
	u32 writeMask;
	u32 logicOp;
	u32 texFunc;
	u32 texEnv;
	u32 dither0;
	u32 dither1;
	u32 texMode;
	s32 texLevelOffset;
	s32 lodSlopeLog;
	u32 rtt;  // format | 0x100 (render to texture) | texture scale << 12
	u32 texLevels[8];
	u32 rttY;
	u32 pad2[3];
};
static_assert(sizeof(DrawStateGpu) == 144, "DrawStateGpu layout");

enum : u32 {
	F_ALPHA_BLEND = 0x0001,
	F_LOGIC_OP = 0x0002,
	F_FOG = 0x0004,
	F_COLOR_TEST = 0x0008,
	F_STENCIL_TEST = 0x0010,
	F_DEPTH_RANGE = 0x0020,
	F_DITHER = 0x0040,
	F_WRITE_MASK = 0x0080,
	F_ALPHA_MASK = 0x0100,
	F_STENCIL_MASK = 0x0200,
	F_DEPTH_WRITE = 0x0400,
	F_COLOR_CLEAR = 0x0800,
	F_STENCIL_CLEAR = 0x1000,
	F_DEPTH_CLEAR = 0x2000,
	F_DBL_SEC = 0x4000,
};

// A vertex as the GPU takes it: screen space, already divided by w; s, t and q interpolate linearly
// in screen space and the texel coordinate is (s/q, t/q), as on the GE.
struct GpuVertex {
	float x, y;      // PSP pixels
	float z, fog;    // 0-65535, 0-255
	u32 c0, c1;      // RGBA
	float s, t, q;
	u32 state;
	float lod;       // the primitive's mip level gradient (same at every vertex), see the pixel shader
};
static_assert(sizeof(GpuVertex) == 44, "GpuVertex layout");

// The texture atlas layout of a sampler state's levels (each level at most 512 x 512, side by side):
// DrawStateGpu::texLevels. Returns the atlas size.
void TextureAtlasLayout(const SamplerState &ss, u32 levels[8], int *atlasW, int *atlasH);

// Texture-data and cache-key hash mixing primes.
constexpr u64 P1 = 0x9E3779B185EBCA87ULL;
constexpr u64 P2 = 0xC2B2AE3D27D4EB4FULL;
// A 64-bit hash of guest memory (texture validation).
u64 HashBytes(const u8 *p, size_t n, u64 seed);

inline u32 Log2Pow2(u32 v) {
	u32 n = 0;
	while (n < 31 && (v >> n) > 1)
		++n;
	return n;
}

struct Segment {
	u32 first, count;
	u32 state, lastState;
	bool lines;
};

// GPU-independent post-transform preparation shared by the native backends.
class DrawData {
protected:
	void BuildVertices(const BatchPrim *prims, size_t n, const RasterState *states);
	void FillStates(const RasterState *states, size_t n);
	int scale_ = 1, texFilter_ = 0, texUpscale_ = 1;
	std::vector<GpuVertex> verts_;
	std::vector<Segment> segs_;
	std::vector<DrawStateGpu> gpuStates_;
private:
	void EmitTriangle(const BatchPrim &, const RasterState &);
	void EmitRect(const BatchPrim &, const RasterState &, bool clear);
	void EmitPoint(const BatchPrim &, const RasterState &);
	void EmitLine(const BatchPrim &, const RasterState &);
};
bool SameTexture(const SamplerState &a, const SamplerState &b);
void DecodeTextureLevel(const SamplerState &ss, int level, int w, int h, u32 *out, size_t pitch);

}  // namespace gpu::hw
