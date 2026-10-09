// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The per-pixel pipeline after texturing: depth range, alpha test, fog, color test, stencil and depth
// tests, blending, dithering, logic op, write masks (GPU/Software/DrawPixel.cpp's DrawPixelInline).
#pragma once

#include <algorithm>
#include <cstdlib>

#include "gpu/color.h"
#include "gpu/render.h"

namespace gpu {

constexpr u32 VRAM_MASK = VRAM_SIZE - 1;

template <int fmt>
PSPC_FORCEINLINE u32 ReadFB(const u8 *vram, u32 off) {
	if constexpr (fmt == GE_FORMAT_8888) {
		u32 v;
		memcpy(&v, vram + (off & VRAM_MASK), 4);
		return v;
	} else {
		u16 v;
		memcpy(&v, vram + (off & VRAM_MASK), 2);
		return v;
	}
}

template <int fmt>
PSPC_FORCEINLINE void WriteFB(u8 *vram, u32 off, u32 v) {
	if constexpr (fmt == GE_FORMAT_8888) {
		memcpy(vram + (off & VRAM_MASK), &v, 4);
	} else {
		const u16 v16 = (u16)v;
		memcpy(vram + (off & VRAM_MASK), &v16, 2);
	}
}

PSPC_FORCEINLINE u16 ReadZ(const u8 *vram, u32 off) {
	u16 v;
	memcpy(&v, vram + (off & VRAM_MASK), 2);
	return v;
}

PSPC_FORCEINLINE void WriteZ(u8 *vram, u32 off, u16 v) {
	memcpy(vram + (off & VRAM_MASK), &v, 2);
}

template <int fmt>
PSPC_FORCEINLINE u32 ExpandColor(u32 raw) {
	switch (fmt) {
	case GE_FORMAT_565: return RGB565ToRGBA8888((u16)raw) & 0x00FFFFFF;  // alpha 0 for blending
	case GE_FORMAT_5551: return RGBA5551ToRGBA8888((u16)raw);
	case GE_FORMAT_4444: return RGBA4444ToRGBA8888((u16)raw);
	default: return raw;
	}
}

template <int fmt>
PSPC_FORCEINLINE u32 PackColor(u32 rgba) {
	switch (fmt) {
	case GE_FORMAT_565: return RGBA8888ToRGB565(rgba);
	case GE_FORMAT_5551: return RGBA8888ToRGBA5551(rgba);
	case GE_FORMAT_4444: return RGBA8888ToRGBA4444(rgba);
	default: return rgba;
	}
}

template <int fmt>
PSPC_FORCEINLINE u8 StencilOf(u32 raw) {
	switch (fmt) {
	case GE_FORMAT_565: return 0;
	case GE_FORMAT_5551: return (raw & 0x8000) ? 0xFF : 0;
	case GE_FORMAT_4444: { const u8 n = (u8)((raw >> 12) & 0xF); return (u8)((n << 4) | n); }
	default: return (u8)(raw >> 24);
	}
}

// Only the stencil of the stored pixel changes (a failed stencil or depth test).
template <int fmt>
PSPC_FORCEINLINE void WriteStencilOnly(u8 *vram, u32 off, u32 raw, u32 writeMask, u8 value) {
	if constexpr (fmt == GE_FORMAT_565) {
		return;
	} else if constexpr (fmt == GE_FORMAT_5551) {
		if ((writeMask & 0x8000) == 0)
			WriteFB<fmt>(vram, off, (raw & ~0x8000u) | ((value & 0x80u) << 8));
	} else if constexpr (fmt == GE_FORMAT_4444) {
		const u32 wm = writeMask | 0x0FFF;
		WriteFB<fmt>(vram, off, ((raw & wm) | (((u32)value << 8) & ~wm)) & 0xFFFF);
	} else {
		const u32 wm = writeMask | 0x00FFFFFF;
		WriteFB<fmt>(vram, off, (raw & wm) | (((u32)value << 24) & ~wm));
	}
}

PSPC_FORCEINLINE bool CompareTest(int func, int v, int ref) {
	switch (func) {
	case GE_COMP_NEVER: return false;
	case GE_COMP_ALWAYS: return true;
	case GE_COMP_EQUAL: return v == ref;
	case GE_COMP_NOTEQUAL: return v != ref;
	case GE_COMP_LESS: return v < ref;
	case GE_COMP_LEQUAL: return v <= ref;
	case GE_COMP_GREATER: return v > ref;
	case GE_COMP_GEQUAL: return v >= ref;
	}
	return true;
}

template <int fmt>
PSPC_FORCEINLINE u8 ApplyStencilOp(int op, u8 replace, u8 old) {
	switch (op) {
	case GE_STENCILOP_KEEP: return old;
	case GE_STENCILOP_ZERO: return 0;
	case GE_STENCILOP_REPLACE: return replace;
	case GE_STENCILOP_INVERT: return (u8)~old;
	case GE_STENCILOP_INCR:
		switch (fmt) {
		case GE_FORMAT_8888: return old != 0xFF ? old + 1 : old;
		case GE_FORMAT_5551: return 0xFF;
		case GE_FORMAT_4444: return old < 0xF0 ? old + 0x10 : old;
		default: return old;
		}
	case GE_STENCILOP_DECR:
		switch (fmt) {
		case GE_FORMAT_4444: return old >= 0x10 ? old - 0x10 : old;
		case GE_FORMAT_5551: return 0;
		default: return old != 0 ? old - 1 : old;
		}
	}
	return old;
}

PSPC_FORCEINLINE u32 ApplyLogicOp(int op, u32 old_color, u32 new_color) {
	// All preserve alpha (stencil).
	switch (op) {
	case GE_LOGIC_CLEAR: new_color &= 0xFF000000; break;
	case GE_LOGIC_AND: new_color = new_color & (old_color | 0xFF000000); break;
	case GE_LOGIC_AND_REVERSE: new_color = new_color & (~old_color | 0xFF000000); break;
	case GE_LOGIC_COPY: break;
	case GE_LOGIC_AND_INVERTED: new_color = (~new_color & (old_color & 0x00FFFFFF)) | (new_color & 0xFF000000); break;
	case GE_LOGIC_NOOP: new_color = (old_color & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_XOR: new_color = new_color ^ (old_color & 0x00FFFFFF); break;
	case GE_LOGIC_OR: new_color = new_color | (old_color & 0x00FFFFFF); break;
	case GE_LOGIC_NOR: new_color = (~(new_color | old_color) & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_EQUIV: new_color = (~(new_color ^ old_color) & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_INVERTED: new_color = (~old_color & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_OR_REVERSE: new_color = new_color | (~old_color & 0x00FFFFFF); break;
	case GE_LOGIC_COPY_INVERTED: new_color = (~new_color & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_OR_INVERTED: new_color = ((~new_color | old_color) & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_NAND: new_color = (~(new_color & old_color) & 0x00FFFFFF) | (new_color & 0xFF000000); break;
	case GE_LOGIC_SET: new_color |= 0x00FFFFFF; break;
	}
	return new_color;
}

// A blend factor per channel. src/dst are 0-255 channels (dst alpha 0 for 565).
PSPC_FORCEINLINE void BlendFactor(int factor, bool isSrc, const int src[4], const int dst[4], u32 fix, int out[3]) {
	const int *other = isSrc ? dst : src;
	switch (factor) {
	case GE_BLEND_OTHERCOLOR: out[0] = other[0]; out[1] = other[1]; out[2] = other[2]; return;
	case GE_BLEND_INVOTHERCOLOR: out[0] = 255 - other[0]; out[1] = 255 - other[1]; out[2] = 255 - other[2]; return;
	case GE_BLEND_SRCALPHA: out[0] = out[1] = out[2] = src[3]; return;
	case GE_BLEND_INVSRCALPHA: out[0] = out[1] = out[2] = 255 - src[3]; return;
	case GE_BLEND_DSTALPHA: out[0] = out[1] = out[2] = dst[3]; return;
	case GE_BLEND_INVDSTALPHA: out[0] = out[1] = out[2] = 255 - dst[3]; return;
	case GE_BLEND_DOUBLESRCALPHA: out[0] = out[1] = out[2] = 2 * src[3]; return;
	case GE_BLEND_DOUBLEINVSRCALPHA: out[0] = out[1] = out[2] = 255 - 2 * src[3]; return;
	case GE_BLEND_DOUBLEDSTALPHA: out[0] = out[1] = out[2] = 2 * dst[3]; return;
	case GE_BLEND_DOUBLEINVDSTALPHA: out[0] = out[1] = out[2] = 255 - 2 * dst[3]; return;
	case GE_BLEND_ZERO: out[0] = out[1] = out[2] = 0; return;
	case GE_BLEND_ONE: out[0] = out[1] = out[2] = 255; return;
	default:
		out[0] = fix & 0xFF;
		out[1] = (fix >> 8) & 0xFF;
		out[2] = (fix >> 16) & 0xFF;
		return;
	}
}

PSPC_FORCEINLINE void AlphaBlend(const PixelState &ps, const int src[4], const int dst[4], int out[3]) {
	const int eq = ps.blendEq;
	if (eq >= GE_BLENDMODE_MIN) {
		for (int i = 0; i < 3; ++i) {
			switch (eq) {
			case GE_BLENDMODE_MIN: out[i] = std::min(src[i], dst[i]); break;
			case GE_BLENDMODE_MAX: out[i] = std::max(src[i], dst[i]); break;
			default: out[i] = std::abs(src[i] - dst[i]); break;
			}
		}
		return;
	}
	int sf[3], df[3];
	BlendFactor(ps.blendSrc, true, src, dst, ps.fixA, sf);
	BlendFactor(ps.blendDst, false, src, dst, ps.fixB, df);
	const bool signedFactor = ps.blendSrc == GE_BLEND_DOUBLEINVSRCALPHA || ps.blendSrc == GE_BLEND_DOUBLEINVDSTALPHA ||
		ps.blendDst == GE_BLEND_DOUBLEINVSRCALPHA || ps.blendDst == GE_BLEND_DOUBLEINVDSTALPHA;
	for (int i = 0; i < 3; ++i) {
		int s, d;
		if (signedFactor) {
			// 255 - 2a goes negative for a >= 128; the term subtracts then (gpu/probe exp116).
			const int ts = ((2 * src[i] + 1) * (2 * std::abs(sf[i]) + 1)) >> 10;
			const int td = ((2 * dst[i] + 1) * (2 * std::abs(df[i]) + 1)) >> 10;
			s = sf[i] < 0 ? -ts : ts;
			d = df[i] < 0 ? -td : td;
		} else {
			s = ((2 * src[i] + 1) * (2 * sf[i] + 1)) >> 10;
			d = ((2 * dst[i] + 1) * (2 * df[i] + 1)) >> 10;
		}
		switch (eq) {
		case GE_BLENDMODE_MUL_AND_ADD: out[i] = s + d; break;
		case GE_BLENDMODE_MUL_AND_SUBTRACT: out[i] = s - d; break;
		default: out[i] = d - s; break;
		}
	}
}

PSPC_FORCEINLINE int Clamp255(int v) {
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// One pixel. color is RGBA with channels already clamped to 0-255.
template <bool clearMode, int fmt>
PSPC_FORCEINLINE void DrawPixel(const PixelState &ps, u32 fbRow, u32 zbRow, int x, int y, int z, int fog, u32 color) {
	u8 *vram = ps.fb;
	constexpr int bpp = fmt == GE_FORMAT_8888 ? 4 : 2;
	int c[4] = { (int)(color & 0xFF), (int)((color >> 8) & 0xFF), (int)((color >> 16) & 0xFF), (int)(color >> 24) };

	if (ps.applyDepthRange && !ps.earlyZChecks) {
		if (z < ps.minz || z > ps.maxz)
			return;
	}
	if constexpr (!clearMode) {
		if (ps.alphaTestFunc != GE_COMP_ALWAYS) {
			const int a = ps.hasAlphaTestMask ? (c[3] & ps.alphaTestMask) : c[3];
			if (!CompareTest(ps.alphaTestFunc, a, ps.alphaTestRef))
				return;
		}
		if (ps.applyFog) {
			// Like the BLEND texfunc, always rounds up.
			const u32 fc = ps.fogColor;
			for (int i = 0; i < 3; ++i)
				c[i] = (c[i] * fog + (int)((fc >> (8 * i)) & 0xFF) * (255 - fog) + 255) / 256;
		}
		if (ps.colorTest) {
			const u32 rgb = ((u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16)) & ps.colorTestMask;
			switch (ps.colorTestFunc) {
			case GE_COMP_NEVER: return;
			case GE_COMP_EQUAL: if (rgb != ps.colorTestRef) return; break;
			case GE_COMP_NOTEQUAL: if (rgb == ps.colorTestRef) return; break;
			default: break;
			}
		}
	}

	const u32 targetWriteMask = ps.applyColorWriteMask ? ps.colorWriteMask : 0;
	const u32 fbOff = fbRow + (u32)x * bpp;
	const u32 zOff = zbRow + (u32)x * 2;
	const u32 oldRaw = ReadFB<fmt>(vram, fbOff);
	u8 stencil = clearMode ? (u8)c[3] : StencilOf<fmt>(oldRaw);
	if constexpr (clearMode) {
		if (ps.depthClear)
			WriteZ(vram, zOff, (u16)z);
	} else {
		if (ps.stencilTest) {
			const u8 replace = ps.hasStencilTestMask ? ps.stencilRef : ps.stencilTestRef;
			const u8 sv = ps.hasStencilTestMask ? (stencil & ps.stencilTestMask) : stencil;
			// ref func stencil
			if (!CompareTest(ps.stencilTestFunc, ps.stencilTestRef, sv)) {
				WriteStencilOnly<fmt>(vram, fbOff, oldRaw, targetWriteMask, ApplyStencilOp<fmt>(ps.sFail, replace, stencil));
				return;
			}
			if (!ps.earlyZChecks && ps.depthTestFunc != GE_COMP_ALWAYS && !CompareTest(ps.depthTestFunc, z, ReadZ(vram, zOff))) {
				WriteStencilOnly<fmt>(vram, fbOff, oldRaw, targetWriteMask, ApplyStencilOp<fmt>(ps.zFail, replace, stencil));
				return;
			}
			stencil = ApplyStencilOp<fmt>(ps.zPass, replace, stencil);
		} else if (!ps.earlyZChecks) {
			if (ps.depthTestFunc != GE_COMP_ALWAYS && !CompareTest(ps.depthTestFunc, z, ReadZ(vram, zOff)))
				return;
		}
		if (ps.depthWrite)
			WriteZ(vram, zOff, (u16)z);
	}

	const u32 oldColor = ExpandColor<fmt>(oldRaw);
	u32 newColor;
	int dith = 0;
	if (ps.dithering)
		dith = ps.ditherMatrix[(y & 3) * 4 + (x & 3)];
	if (!clearMode && ps.alphaBlend) {
		const int dst[4] = { (int)(oldColor & 0xFF), (int)((oldColor >> 8) & 0xFF), (int)((oldColor >> 16) & 0xFF), (int)(oldColor >> 24) };
		int blended[3];
		AlphaBlend(ps, c, dst, blended);
		newColor = (u32)Clamp255(blended[0] + dith) | ((u32)Clamp255(blended[1] + dith) << 8) | ((u32)Clamp255(blended[2] + dith) << 16);
	} else {
		newColor = (u32)Clamp255(c[0] + dith) | ((u32)Clamp255(c[1] + dith) << 8) | ((u32)Clamp255(c[2] + dith) << 16);
	}
	newColor |= (u32)stencil << 24;

	if (!clearMode && ps.applyLogicOp)
		newColor = ApplyLogicOp(ps.logicOp, oldColor, newColor);

	if constexpr (clearMode) {
		if (!ps.colorClear)
			newColor = (newColor & 0xFF000000) | (oldColor & 0x00FFFFFF);
		if (!ps.stencilClear)
			newColor = (newColor & 0x00FFFFFF) | (oldColor & 0xFF000000);
	}

	u32 value = PackColor<fmt>(newColor);
	if (targetWriteMask)
		value = (value & ~targetWriteMask) | (oldRaw & targetWriteMask);
	WriteFB<fmt>(vram, fbOff, value);
}

// A run of pixels in one row.
typedef void (*PixelRunFn)(const PixelState &ps, int x0, int y, int count, const u8 *alive, const int *z, const u8 *fog, const u32 *color);
PixelRunFn GetPixelRunFn(const PixelState &ps);

}  // namespace gpu
