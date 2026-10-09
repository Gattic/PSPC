// SPDX-License-Identifier: GPL-2.0-or-later
//
// Colour space conversion and pixel layout helpers shared by the video and JPEG libraries.
// The Media Engine tile layout (UntileYCbCr / FrameBufferLayout) is ported from PPSSPP's
// Core/HLE/sceMpegbase.cpp and sceVideocodec.cpp (https://github.com/hrydgard/ppsspp,
// Copyright (c) 2026- PPSSPP Project, GPL-2.0-or-later), which in turn follow JPCSP.
#pragma once

#include "common/common.h"

namespace media {

// GE colour formats, as the PSP's video APIs number them.
enum PixelMode : int {
	PIXEL_5650 = 0,
	PIXEL_5551 = 1,
	PIXEL_4444 = 2,
	PIXEL_8888 = 3,
};

inline int PixelModeBytes(int mode) { return mode == PIXEL_8888 ? 4 : 2; }

// One studio-swing BT.601 pixel in the given format, alpha left clear (as the hardware does).
u32 YCbCrToPixel(int y, int cb, int cr, int pixelMode);

// Converts a rectangle of a planar 4:2:0 picture (luma `width` wide, chroma half that in both
// directions) to `dest`, destStride pixels per row, starting at the rectangle's origin.
void YCbCr420ToPixels(u8 *dest, int destStride, int pixelMode, const u8 *luma, const u8 *cb, const u8 *cr,
                      int width, int rangeX, int rangeY, int rangeWidth, int rangeHeight);

// PSP texture swizzling of 16-byte by 8-row blocks (GE swizzled VRAM mirror).
void SwizzleTex16(const u32 *src, u8 *dest, int bxc, int byc, u32 pitch);

// The eight buffers the Media Engine delivers a decoded frame in: four luma (left/right half of
// a 32 pixel band, even/odd rows), then four chroma. Returns the total size of one allocation
// holding all eight at 64-byte aligned offsets; sizes/offsets may be null.
u32 FrameBufferLayout(int width, int height, int sizes[8], u32 offsets[8]);
// The inverse operations: tile planar 4:2:0 into the eight buffers, or untile them. A null
// buffer is skipped.
void TileYCbCr(u8 *const dst[8], const int sizes[8], const u8 *luma, const u8 *cb, const u8 *cr, int strideY,
               int strideC, int width, int height);
void UntileYCbCr(u8 *luma, u8 *cb, u8 *cr, const u8 *const src[8], const int sizes[8], int width, int height);

}  // namespace media
