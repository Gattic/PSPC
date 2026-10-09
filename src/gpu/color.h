// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// PSP pixel format conversions (R in the low bits, as the GE stores them).
#pragma once

#include "common/common.h"

namespace gpu {

PSPC_FORCEINLINE u32 RGBA4444ToRGBA8888(u16 src) {
	const u32 c = (src & 0x000F) | ((src & 0x00F0) << 4) | ((src & 0x0F00) << 8) | ((src & 0xF000) << 12);
	return c | (c << 4);
}

PSPC_FORCEINLINE u32 RGBA5551ToRGBA8888(u16 src) {
	u32 dark = ((src & 0x1F) << 3) | ((src & 0x3E0) << 6) | ((src & 0x7C00) << 9);
	u32 full = dark | ((dark >> 5) & 0x070707);
	if (src >> 15)
		full |= 0xFF000000;
	return full;
}

PSPC_FORCEINLINE u32 RGB565ToRGBA8888(u16 src) {
	u32 dark_rb = ((src & 0x1F) << 3) | ((src & 0xF800) << 8);
	u32 full_rb = dark_rb | ((dark_rb >> 5) & 0x070007);
	u32 dark_g = (src & 0x7E0) << 5;
	u32 full_g = dark_g | ((dark_g >> 6) & 0x300);
	return full_rb | full_g | 0xFF000000;
}

PSPC_FORCEINLINE u16 RGBA8888ToRGB565(u32 v) {
	return (u16)(((v >> 3) & 0x1F) | ((v >> 5) & (0x3F << 5)) | ((v >> 8) & (0x1F << 11)));
}

PSPC_FORCEINLINE u16 RGBA8888ToRGBA5551(u32 v) {
	return (u16)(((v >> 3) & 0x1F) | ((v >> 6) & (0x1F << 5)) | ((v >> 9) & (0x1F << 10)) | ((v >> 16) & 0x8000));
}

PSPC_FORCEINLINE u16 RGBA8888ToRGBA4444(u32 v) {
	const u32 c = v >> 4;
	return (u16)((c & 0x000F) | ((c >> 4) & 0x00F0) | ((c >> 8) & 0x0F00) | ((c >> 12) & 0xF000));
}

PSPC_FORCEINLINE u32 ConvertToRGBA8888(u16 v, int fmt) {
	switch (fmt) {
	case 0: return RGB565ToRGBA8888(v);
	case 1: return RGBA5551ToRGBA8888(v);
	default: return RGBA4444ToRGBA8888(v);
	}
}

PSPC_FORCEINLINE u16 ConvertFromRGBA8888(u32 v, int fmt) {
	switch (fmt) {
	case 0: return RGBA8888ToRGB565(v);
	case 1: return RGBA8888ToRGBA5551(v);
	default: return RGBA8888ToRGBA4444(v);
	}
}

}  // namespace gpu
