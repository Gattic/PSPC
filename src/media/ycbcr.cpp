// SPDX-License-Identifier: GPL-2.0-or-later
#include "media/ycbcr.h"

#include <algorithm>
#include <cstring>

namespace media {

u32 YCbCrToPixel(int y, int cbv, int crv, int pixelMode) {
	const int c = y - 16, d = cbv - 128, e = crv - 128;
	int r = (298 * c + 409 * e + 128) >> 8;
	int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
	int b = (298 * c + 516 * d + 128) >> 8;
	r = std::min(255, std::max(0, r));
	g = std::min(255, std::max(0, g));
	b = std::min(255, std::max(0, b));
	switch (pixelMode) {
	case PIXEL_5650: return (u32)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
	case PIXEL_5551: return (u32)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
	case PIXEL_4444: return (u32)(((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4));
	default: return (u32)((b << 16) | (g << 8) | r);
	}
}

void YCbCr420ToPixels(u8 *dest, int destStride, int pixelMode, const u8 *luma, const u8 *cb, const u8 *cr,
                      int width, int rangeX, int rangeY, int rangeWidth, int rangeHeight) {
	const int width2 = width >> 1;
	if (pixelMode == PIXEL_8888) {
		for (int y = 0; y < rangeHeight; y++) {
			const int sy = rangeY + y;
			const u8 *lrow = luma + (size_t)sy * width;
			const u8 *cbrow = cb + (size_t)(sy >> 1) * width2;
			const u8 *crrow = cr + (size_t)(sy >> 1) * width2;
			u32 *out = (u32 *)(dest + (size_t)y * destStride * 4);
			for (int x = 0; x < rangeWidth; x++) {
				const int sx = rangeX + x;
				out[x] = YCbCrToPixel(lrow[sx], cbrow[sx >> 1], crrow[sx >> 1], PIXEL_8888);
			}
		}
		return;
	}
	for (int y = 0; y < rangeHeight; y++) {
		const int sy = rangeY + y;
		const u8 *lrow = luma + (size_t)sy * width;
		const u8 *cbrow = cb + (size_t)(sy >> 1) * width2;
		const u8 *crrow = cr + (size_t)(sy >> 1) * width2;
		u16 *out = (u16 *)(dest + (size_t)y * destStride * 2);
		for (int x = 0; x < rangeWidth; x++) {
			const int sx = rangeX + x;
			out[x] = (u16)YCbCrToPixel(lrow[sx], cbrow[sx >> 1], crrow[sx >> 1], pixelMode);
		}
	}
}

void SwizzleTex16(const u32 *ysrcp, u8 *texptr, int bxc, int byc, u32 pitch) {
	const u32 pitchBy32 = pitch >> 2;
	u32 *dest = (u32 *)texptr;
	for (int by = 0; by < byc; by++) {
		const u32 *xsrc = ysrcp;
		for (int bx = 0; bx < bxc; bx++) {
			const u32 *src = xsrc;
			for (int n = 0; n < 8; n++) {
				memcpy(dest, src, 16);
				src += pitchBy32;
				dest += 4;
			}
			xsrc += 4;
		}
		ysrcp += pitchBy32 * 8;
	}
}

u32 FrameBufferLayout(int width, int height, int sizes[8], u32 offsets[8]) {
	// buffer0/2 take the odd band out when the width isn't a multiple of 32.
	const int lumaLeft = ((width + 16) >> 5) * (height >> 1) * 16;
	const int lumaRight = (width >> 5) * (height >> 1) * 16;
	const int local[8] = {
	    lumaLeft, lumaRight, lumaLeft, lumaRight, lumaLeft >> 1, lumaRight >> 1, lumaLeft >> 1, lumaRight >> 1,
	};
	u32 total = 0;
	for (int i = 0; i < 8; i++) {
		if (sizes) sizes[i] = local[i];
		if (offsets) offsets[i] = total;
		total += (u32)((local[i] + 63) & ~63);
	}
	return total;
}

void TileYCbCr(u8 *const dst[8], const int sizes[8], const u8 *srcY, const u8 *srcCb, const u8 *srcCr, int strideY,
               int strideC, int width, int height) {
	const int width2 = width >> 1;
	const int height2 = height >> 1;
	for (int b = 0; b < 4; b++) {
		if (!dst[b] || sizes[b] <= 0) continue;
		const int xOffset = (b & 1) ? 16 : 0;
		const int yStart = (b >> 1) ? 1 : 0;
		int j = 0;
		for (int bandX = xOffset; bandX < width; bandX += 32) {
			const int run = std::min(16, width - bandX);
			for (int row = yStart; row < height; row += 2, j += 16) {
				if (run <= 0 || j + run > sizes[b]) continue;
				memcpy(dst[b] + j, srcY + (size_t)row * strideY + bandX, run);
			}
		}
	}
	for (int b = 0; b < 4; b++) {
		u8 *d = dst[4 + b];
		const int size = sizes[4 + b];
		if (!d || size <= 0) continue;
		const int xOffset = (b & 1) ? 8 : 0;
		const int yStart = (b >> 1) ? 1 : 0;
		int j = 0;
		for (int bandX = xOffset; bandX < width2; bandX += 16) {
			for (int row = yStart; row < height2; row += 2) {
				for (int k = 0; k < 8; k++, j += 2) {
					const int x = bandX + k;
					if (x >= width2 || j + 1 >= size) continue;
					d[j] = srcCb[(size_t)row * strideC + x];
					d[j + 1] = srcCr[(size_t)row * strideC + x];
				}
			}
		}
	}
}

void UntileYCbCr(u8 *luma, u8 *cb, u8 *cr, const u8 *const src[8], const int sizes[8], int width, int height) {
	const int width2 = width >> 1;
	const int height2 = height >> 1;
	for (int b = 0; b < 4; b++) {
		if (!src[b]) continue;
		const int xOffset = (b & 1) ? 16 : 0;
		const int yStart = (b >> 1) ? 1 : 0;
		int j = 0;
		for (int bandX = xOffset; bandX < width; bandX += 32) {
			const int run = std::min(16, width - bandX);
			for (int row = yStart; row < height; row += 2, j += 16) {
				if (run <= 0 || j + run > sizes[b]) continue;
				memcpy(luma + (size_t)row * width + bandX, src[b] + j, run);
			}
		}
	}
	for (int b = 0; b < 4; b++) {
		if (!src[b + 4]) continue;
		const int xOffset = (b & 1) ? 8 : 0;
		const int yStart = (b >> 1) ? 1 : 0;
		int j = 0;
		for (int bandX = xOffset; bandX < width2; bandX += 16) {
			for (int row = yStart; row < height2; row += 2, j += 16) {
				const int fits = std::min(8, (sizes[b + 4] - j) >> 1);
				const int run = std::min(width2 - bandX, fits);
				if (run <= 0) continue;
				const u8 *from = src[b + 4] + j;
				u8 *toCb = cb + (size_t)row * width2 + bandX;
				u8 *toCr = cr + (size_t)row * width2 + bandX;
				for (int k = 0; k < run; k++) {
					toCb[k] = from[k * 2];
					toCr[k] = from[k * 2 + 1];
				}
			}
		}
	}
}

}  // namespace media
