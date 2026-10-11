// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// Internal interfaces of the software renderer: the post-transform vertex, the per-draw raster state
// snapshot (pixel pipeline + sampler, modelled on PPSSPP's PixelFuncID/SamplerID), and the binner that
// queues primitives and draws them, possibly on worker threads.
#pragma once

#include <cstdint>
#include <vector>

#include "common/common.h"
#include "gpu/ge_constants.h"

namespace gpu {

constexpr int SUBPIX = 16;  // screen coordinates are 12.4 fixed point

// VRAM and guest memory helpers.
constexpr u32 VRAM_BASE = 0x04000000;
constexpr u32 VRAM_SIZE = 0x00200000;
extern u32 g_ramSize;
bool IsValidAddress(u32 addr);
// Bytes from addr to the end of its memory region (0 if invalid). VRAM mirrors fold onto VRAM.
u32 ValidBytesAt(u32 addr);
// Folds VRAM mirrors (0x04200000-0x047FFFFF) onto VRAM and strips the segment bits.
u32 FoldAddress(u32 addr);
u8 *GuestPtr(u32 addr);
u8 *VramPtr();

// A vertex as the rasterizer takes it.
struct RVertex {
	int x, y;        // screen subpixels, drawing offset applied
	int z;           // 0..65535
	u32 color0;      // RGBA
	u32 color1;      // RGB, secondary (specular) color
	float s, t, q;   // texture coordinates (texels in through mode), q for texture projection
	float clipw;     // 1 in through mode
	float fog;       // fog depth, 1 = no fog
};

enum class PrimType : u8 { Triangle, Rect, ClearRect, Line, Point };

// Pixel range, inclusive, in screen subpixels (like PPSSPP's BinCoords).
struct Range {
	int x1, y1, x2, y2;
	bool Invalid() const { return x2 < x1 || y2 < y1; }
};

struct PixelState {
	u8 *fb;           // framebuffer base in host memory (VRAM)
	u32 fbOffset;     // offset of the framebuffer in VRAM
	u32 zbOffset;
	int fbStride;
	int zbStride;
	u8 fbFormat;

	bool clearMode;
	bool colorClear, stencilClear, depthClear;
	bool colorTest;
	bool stencilTest;
	bool depthWrite;
	bool applyDepthRange;
	bool alphaBlend;
	bool dithering;
	bool applyLogicOp;
	bool applyFog;
	bool applyColorWriteMask;
	bool earlyZChecks;
	bool hasAlphaTestMask;
	bool hasStencilTestMask;

	u8 alphaTestFunc, alphaTestRef, alphaTestMask;
	u8 depthTestFunc;
	u8 stencilTestFunc, stencilTestRef, stencilRef, stencilTestMask;
	u8 sFail, zFail, zPass;
	u8 blendEq, blendSrc, blendDst;
	u8 logicOp;
	u8 colorTestFunc;
	u32 colorTestMask, colorTestRef;
	u32 fixA, fixB;
	u32 fogColor;
	u32 colorWriteMask;  // in framebuffer format: set bits are kept
	int minz, maxz;
	s8 ditherMatrix[16];
};

struct TexLevel {
	const u8 *ptr;     // null: invalid address, texels read as 0
	u32 addr;
	u32 validBytes;    // readable bytes from ptr
	u16 bufw;
	u16 w, h;
	bool bounded;      // reads must be checked against validBytes
};

struct SamplerState;
typedef u32 (*TexSampleFn)(const SamplerState &ss, float s, float t, int level, int levelFrac, bool linear);

struct SamplerState {
	TexSampleFn sample;
	u8 texfmt;
	u8 clutfmt;
	bool clampS, clampT;
	bool swizzle;
	bool useSharedClut;
	bool useTextureAlpha;
	bool useColorDoubling;
	u8 texFunc;
	u8 clutShift, clutMask;
	u16 clutOffset;
	bool clutSimple;   // no shift/mask/offset
	u32 texBlendColor;
	const u8 *clut;    // 1 KB copy taken at LOADCLUT time
	int maxLevel;
	int width0Shift, height0Shift;
	TexLevel levels[8];

	u8 texLevelMode;
	s8 texLevelOffset;
	float lodSlope;
	bool mipFilt, minFilt, magFilt;
};

struct RasterState {
	PixelState pix;
	SamplerState samp;
	bool enableTextures;
	bool shadeGouraud;
	bool throughMode;
	bool antialiasLines;
	bool textureProj;
	Range scissor;      // subpixels
};

// Draws one primitive, limited to the pixel rows y1..y2 (inclusive, drawing coordinates).
void DrawTriangle(const RVertex &v0, const RVertex &v1, const RVertex &v2, const Range &range, const RasterState &state);
void DrawRectangle(const RVertex &v0, const RVertex &v1, const Range &range, const RasterState &state);
void ClearRectangle(const RVertex &v0, const RVertex &v1, const Range &range, const RasterState &state);
void DrawLine(const RVertex &v0, const RVertex &v1, const Range &range, const RasterState &state);
void DrawPoint(const RVertex &v0, const Range &range, const RasterState &state);

void ComputeRasterState(RasterState *state, const u8 *clut);

// The primitive queue.
namespace binner {

void Init();
void Shutdown();
void SetThreads(int count);
// Marks the raster state (pixel pipeline, textures, scissor) for recomputation.
void DirtyState();
// A render target property changed: flushes what's queued.
void TargetChanged();
// Copies the CLUT (1 KB) that later draws use.
void UpdateClut(const u8 *src);
// TEXFLUSH: the texture cache starts over.
void TexFlush();
const RasterState &CurrentState();
void AddTriangle(const RVertex &v0, const RVertex &v1, const RVertex &v2);
void AddRect(const RVertex &v0, const RVertex &v1);
void AddClearRect(const RVertex &v0, const RVertex &v1);
void AddLine(const RVertex &v0, const RVertex &v1);
void AddPoint(const RVertex &v0);
// Draws everything queued.
void Flush();
bool HasQueued();
// Flushes if queued drawing writes [addr, addr+size) or (writing) reads it.
void FlushIfOverlap(u32 addr, u32 size, bool modifying);

}  // namespace binner

// Per-frame counters (ge.h Stats).
struct InternalStats {
	u64 commands, prims, vertices, pixels, flushes, threadedFlushes, transfers;
	u64 hostNs;  // host time in Run and in the syncs
};
extern InternalStats g_stats;

}  // namespace gpu
