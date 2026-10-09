// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The per-draw raster state snapshot, from the GE registers (GPU/Software/FuncId.cpp, Rasterizer.cpp).
#include <algorithm>
#include <cstring>

#include "gpu/color.h"
#include "gpu/ge_state.h"
#include "gpu/render.h"

namespace gpu {

static const u8 textureBitsPerPixel[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };

static const u32 textureAlignMask16[16] = {
	0x7FF & ~(((8 * 16) / 16) - 1),
	0x7FF & ~(((8 * 16) / 16) - 1),
	0x7FF & ~(((8 * 16) / 16) - 1),
	0x7FF & ~(((8 * 16) / 32) - 1),
	0x7FF & ~(((8 * 16) / 4) - 1),
	0x7FF & ~(((8 * 16) / 8) - 1),
	0x7FF & ~(((8 * 16) / 16) - 1),
	0x7FF & ~(((8 * 16) / 32) - 1),
	0x7FF, 0x7FF, 0x7FF,
	0, 0, 0, 0, 0,
};

static u32 GetTextureBufw(int level, GETextureFormat format) {
	u32 bufw = gstate.texbufwidth[level] & textureAlignMask16[format];
	if (bufw == 0 && format <= GE_TFMT_DXT5)
		bufw = (8 * 16) / textureBitsPerPixel[format];
	return bufw;
}

static inline GEComparison OptimizeRefByteCompare(GEComparison func, u8 ref) {
	if (ref == 0 && func == GE_COMP_GREATER)
		return GE_COMP_NOTEQUAL;
	if (ref == 0xFF && func == GE_COMP_LESS)
		return GE_COMP_NOTEQUAL;
	if (ref == 0 && func == GE_COMP_GEQUAL)
		return GE_COMP_ALWAYS;
	if (ref == 0xFF && func == GE_COMP_LEQUAL)
		return GE_COMP_ALWAYS;
	return func;
}

// The stencil test compares ref against the buffer's value.
static inline GEComparison OptimizeStencilRefCompare(GEComparison func, u8 ref) {
	if (ref == 0 && func == GE_COMP_GREATER)
		return GE_COMP_NEVER;
	if (ref == 0xFF && func == GE_COMP_LESS)
		return GE_COMP_NEVER;
	if (ref == 0 && func == GE_COMP_LEQUAL)
		return GE_COMP_ALWAYS;
	if (ref == 0xFF && func == GE_COMP_GEQUAL)
		return GE_COMP_ALWAYS;
	return func;
}

static inline u8 OptimizeAlphaFactor(u32 color) {
	if (color == 0x00000000)
		return GE_BLEND_ZERO;
	if (color == 0x00FFFFFF)
		return GE_BLEND_ONE;
	return GE_BLEND_FIX;
}

static void ComputePixelState(PixelState *id) {
	memset(id, 0, sizeof(*id));
	const GEBufferFormat fmt = gstate.FrameBufFormat();

	id->fb = VramPtr();
	id->fbOffset = gstate.getFrameBufAddress() & (VRAM_SIZE - 1);
	id->zbOffset = gstate.getDepthBufAddress() & (VRAM_SIZE - 1);
	id->fbStride = gstate.FrameBufStride();
	id->zbStride = gstate.DepthBufStride();
	id->fbFormat = fmt;

	id->applyDepthRange = !gstate.isModeThrough();
	// Dither happens even in clear mode.
	id->dithering = gstate.isDitherEnabled();
	id->applyColorWriteMask = gstate.getColorMask() != 0;
	id->clearMode = gstate.isModeClear();
	id->alphaTestFunc = GE_COMP_ALWAYS;
	id->depthTestFunc = GE_COMP_ALWAYS;

	if (id->clearMode) {
		id->colorClear = gstate.isClearModeColorMask();
		id->stencilClear = gstate.isClearModeAlphaMask() && fmt != GE_FORMAT_565;
		id->depthClear = gstate.isClearModeDepthMask();
	} else {
		id->colorTest = gstate.isColorTestEnabled() && gstate.getColorTestFunction() != GE_COMP_ALWAYS;
		if (gstate.isStencilTestEnabled() && gstate.getStencilTestFunction() == GE_COMP_ALWAYS) {
			// If stencil always passes, force off when no stencil bits are written.
			const bool stencilWrite = (gstate.pmska & 0xFF) != 0xFF && fmt != GE_FORMAT_565;
			if (gstate.isDepthTestEnabled() && gstate.getDepthTestFunction() != GE_COMP_ALWAYS)
				id->stencilTest = stencilWrite && (gstate.getStencilOpZPass() != GE_STENCILOP_KEEP || gstate.getStencilOpZFail() != GE_STENCILOP_KEEP);
			else
				id->stencilTest = stencilWrite && gstate.getStencilOpZPass() != GE_STENCILOP_KEEP;
		} else {
			id->stencilTest = gstate.isStencilTestEnabled();
		}
		id->depthWrite = gstate.isDepthTestEnabled() && gstate.isDepthWriteEnabled();
		id->depthTestFunc = gstate.isDepthTestEnabled() ? gstate.getDepthTestFunction() : GE_COMP_ALWAYS;

		if (id->stencilTest) {
			id->stencilTestRef = gstate.getStencilTestRef() & gstate.getStencilTestMask();
			id->stencilTestFunc = OptimizeStencilRefCompare(gstate.getStencilTestFunction(), id->stencilTestRef);
			id->hasStencilTestMask = gstate.getStencilTestMask() != 0xFF && fmt != GE_FORMAT_565;
			// Stencil can't be written on 565, and invalid ops act like KEEP.
			if (fmt != GE_FORMAT_565 && gstate.getStencilOpSFail() <= GE_STENCILOP_DECR)
				id->sFail = gstate.getStencilOpSFail();
			if (fmt != GE_FORMAT_565 && gstate.getStencilOpZFail() <= GE_STENCILOP_DECR)
				id->zFail = gstate.isDepthTestEnabled() ? gstate.getStencilOpZFail() : GE_STENCILOP_KEEP;
			if (fmt != GE_FORMAT_565 && gstate.getStencilOpZPass() <= GE_STENCILOP_DECR)
				id->zPass = gstate.getStencilOpZPass();
			if (gstate.getStencilTestRef() == 0) {
				if (id->sFail == GE_STENCILOP_REPLACE) id->sFail = GE_STENCILOP_ZERO;
				if (id->zFail == GE_STENCILOP_REPLACE) id->zFail = GE_STENCILOP_ZERO;
				if (id->zPass == GE_STENCILOP_REPLACE) id->zPass = GE_STENCILOP_ZERO;
			}
			if (fmt == GE_FORMAT_5551) {
				if (id->sFail == GE_STENCILOP_DECR) id->sFail = GE_STENCILOP_ZERO;
				if (id->zFail == GE_STENCILOP_DECR) id->zFail = GE_STENCILOP_ZERO;
				if (id->zPass == GE_STENCILOP_DECR) id->zPass = GE_STENCILOP_ZERO;
			}
			if (id->stencilTestFunc == GE_COMP_ALWAYS) {
				if (id->depthTestFunc == GE_COMP_ALWAYS)
					id->zFail = GE_STENCILOP_KEEP;
				id->sFail = GE_STENCILOP_KEEP;
				id->stencilTestRef = gstate.getStencilTestRef();
				id->hasStencilTestMask = false;
				if (id->sFail == GE_STENCILOP_KEEP && id->zFail == GE_STENCILOP_KEEP && id->zPass == GE_STENCILOP_KEEP) {
					id->stencilTest = false;
					id->stencilTestFunc = 0;
					id->stencilTestRef = 0;
				}
			} else if (id->depthTestFunc == GE_COMP_ALWAYS) {
				id->zFail = id->zPass;
			}
		}

		id->alphaTestFunc = gstate.isAlphaTestEnabled() ? gstate.getAlphaTestFunction() : GE_COMP_ALWAYS;
		if (id->alphaTestFunc != GE_COMP_ALWAYS) {
			id->alphaTestRef = gstate.getAlphaTestRef() & gstate.getAlphaTestMask();
			id->hasAlphaTestMask = gstate.getAlphaTestMask() != 0xFF;
			id->alphaTestFunc = OptimizeRefByteCompare((GEComparison)id->alphaTestFunc, id->alphaTestRef);
			if (id->alphaTestFunc == GE_COMP_ALWAYS) {
				id->alphaTestRef = 0;
				id->hasAlphaTestMask = false;
			}
		}

		// Equations 6 and 7 don't blend.
		id->alphaBlend = gstate.isAlphaBlendEnabled() && gstate.getBlendEq() <= 5;
		if (id->alphaBlend && gstate.getBlendEq() == GE_BLENDMODE_MUL_AND_ADD) {
			const bool srcFixedOne = gstate.getBlendFuncA() == GE_BLEND_FIX && gstate.getFixA() == 0x00FFFFFF;
			const bool dstFixedZero = gstate.getBlendFuncB() == GE_BLEND_FIX && gstate.getFixB() == 0x00000000;
			if (srcFixedOne && dstFixedZero)
				id->alphaBlend = false;
		}
		if (id->alphaBlend)
			id->blendEq = gstate.getBlendEq();
		if (id->alphaBlend && id->blendEq <= GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE) {
			id->blendSrc = gstate.getBlendFuncA();
			id->blendDst = gstate.getBlendFuncB();
			// Factors 10 and up are the fixed colors.
			if (id->blendSrc >= GE_BLEND_FIX)
				id->blendSrc = OptimizeAlphaFactor(gstate.getFixA());
			if (id->blendDst >= GE_BLEND_FIX)
				id->blendDst = OptimizeAlphaFactor(gstate.getFixB());
		}

		id->applyLogicOp = gstate.isLogicOpEnabled() && gstate.getLogicOp() != GE_LOGIC_COPY;
		id->applyFog = gstate.isFogEnabled() && !gstate.isModeThrough();

		id->earlyZChecks = id->depthTestFunc != GE_COMP_ALWAYS;
		if (id->stencilTest && id->earlyZChecks) {
			// Can't do them early if stencil might need to write.
			if (id->sFail != GE_STENCILOP_KEEP || id->zFail != GE_STENCILOP_KEEP)
				id->earlyZChecks = false;
		}
	}

	if (id->dithering) {
		for (int y = 0; y < 4; ++y)
			for (int x = 0; x < 4; ++x)
				id->ditherMatrix[y * 4 + x] = (s8)gstate.getDitherValue(x, y);
	}
	if (id->applyColorWriteMask) {
		u32 mask = gstate.getColorMask();
		// Alpha is only written as stencil (stencil test or stencil clear).
		const bool writesStencil = id->clearMode ? id->stencilClear : id->stencilTest;
		if (!writesStencil)
			mask |= 0xFF000000;
		switch (fmt) {
		case GE_FORMAT_565: id->colorWriteMask = RGBA8888ToRGB565(mask); break;
		case GE_FORMAT_5551: id->colorWriteMask = RGBA8888ToRGBA5551(mask); break;
		case GE_FORMAT_4444: id->colorWriteMask = RGBA8888ToRGBA4444(mask); break;
		default: id->colorWriteMask = mask; break;
		}
	}
	if (id->applyFog)
		id->fogColor = gstate.fogcolor & 0x00FFFFFF;
	if (id->applyLogicOp)
		id->logicOp = gstate.getLogicOp();
	id->minz = gstate.getDepthRangeMin();
	id->maxz = gstate.getDepthRangeMax();
	if (id->hasStencilTestMask) {
		id->stencilRef = gstate.getStencilTestRef();
		id->stencilTestMask = gstate.getStencilTestMask();
	}
	if (id->hasAlphaTestMask)
		id->alphaTestMask = gstate.getAlphaTestMask();
	if (!id->clearMode && id->colorTest) {
		id->colorTestFunc = gstate.getColorTestFunction();
		id->colorTestMask = gstate.getColorTestMask();
		id->colorTestRef = gstate.getColorTestRef() & id->colorTestMask;
	}
	if (id->blendSrc == GE_BLEND_FIX)
		id->fixA = gstate.getFixA();
	if (id->blendDst == GE_BLEND_FIX)
		id->fixB = gstate.getFixB();
}

static void ComputeSamplerState(SamplerState *s, const u8 *clut) {
	memset(s, 0, sizeof(*s));
	const GETextureFormat fmt = gstate.getTextureFormat();
	const int maxLevel = gstate.isMipmapEnabled() ? gstate.getTextureMaxLevel() : 0;
	s->maxLevel = maxLevel;
	s->texfmt = fmt;
	s->swizzle = gstate.isTextureSwizzled();
	const int bits = textureBitsPerPixel[fmt];
	for (int i = 0; i <= maxLevel; ++i) {
		TexLevel &lv = s->levels[i];
		lv.addr = gstate.getTextureAddress(i);
		lv.bufw = (u16)GetTextureBufw(i, fmt);
		lv.w = (u16)gstate.getTextureWidth(i);
		lv.h = (u16)gstate.getTextureHeight(i);
		const u32 valid = ValidBytesAt(lv.addr);
		if (valid == 0 || bits == 0) {
			lv.ptr = nullptr;
			lv.validBytes = 0;
		} else {
			lv.ptr = GuestPtr(FoldAddress(lv.addr));
			lv.validBytes = valid;
			// Rows a texel lookup can reach (wrapping and clamping stay within 512).
			const u32 h = std::min<u32>(lv.h, 512);
			const u32 rows = s->swizzle ? ((h + 7) & ~7u) : h;
			const u64 need = (u64)rows * lv.bufw * bits / 8 + 16;
			lv.bounded = need > valid;
		}
	}
	s->width0Shift = gstate.texsize[0] & 0xF;
	s->height0Shift = (gstate.texsize[0] >> 8) & 0xF;

	// With separate CLUTs, level n puts n above the index bits (gpu/probe exp90).
	const bool clutPerLevel = fmt == GE_TFMT_CLUT4 || (fmt == GE_TFMT_CLUT8 && gstate.getClutPaletteFormat() != GE_CMODE_32BIT_ABGR8888);
	s->useSharedClut = !clutPerLevel || maxLevel == 0 || !gstate.isMipmapEnabled() || gstate.isClutSharedForMipmaps();
	if (gstate.isTextureFormatIndexed()) {
		s->clutfmt = gstate.getClutPaletteFormat();
		s->clutShift = (u8)gstate.getClutIndexShift();
		s->clutMask = (u8)gstate.getClutIndexMask();
		s->clutOffset = (u16)gstate.getClutIndexStartPos();
		s->clutSimple = s->clutShift == 0 && s->clutMask == 0xFF && s->clutOffset == 0;
	}
	s->clut = clut;
	s->clampS = gstate.isTexCoordClampedS();
	s->clampT = gstate.isTexCoordClampedT();
	s->useTextureAlpha = gstate.isTextureAlphaUsed();
	s->useColorDoubling = gstate.isColorDoublingEnabled();
	s->texFunc = (u8)std::min(gstate.getTextureFunction(), (int)GE_TEXFUNC_ADD);
	s->texBlendColor = gstate.getTextureEnvColRGB();

	s->lodSlope = gstate.getTextureLodSlope();
	s->texLevelMode = (u8)gstate.getTexLevelMode();
	s->texLevelOffset = (s8)gstate.getTexLevelOffset16();
	s->mipFilt = gstate.isMipmapFilteringEnabled();
	s->minFilt = gstate.isMinifyFilteringEnabled();
	s->magFilt = gstate.isMagnifyFilteringEnabled();
}

void ComputeRasterState(RasterState *state, const u8 *clut) {
	ComputePixelState(&state->pix);
	state->enableTextures = gstate.isTextureMapEnabled() && !state->pix.clearMode;
	if (state->enableTextures) {
		ComputeSamplerState(&state->samp, clut);
		state->textureProj = gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_MATRIX;
		if (state->textureProj) {
			// The source q factor is zero, or the source is UV, and q comes out 1.
			const bool qZeroST = gstate.tgenMatrix[2] == 0.0f && gstate.tgenMatrix[5] == 0.0f;
			const bool qZeroQ = gstate.tgenMatrix[8] == 0.0f;
			const bool qFactorZero = gstate.getUVProjMode() == GE_PROJMAP_UV;
			if (qZeroST && (qZeroQ || qFactorZero) && gstate.tgenMatrix[11] == 1.0f)
				state->textureProj = false;
		}
	} else {
		memset(&state->samp, 0, sizeof(state->samp));
		state->textureProj = false;
	}
	state->shadeGouraud = !gstate.isModeClear() && gstate.isShadeGouraud();
	state->throughMode = gstate.isModeThrough();
	state->antialiasLines = gstate.isAntiAliasEnabled();

	const int sx1 = gstate.getScissorX1(), sy1 = gstate.getScissorY1();
	const int sx2 = std::min(gstate.getScissorX2(), gstate.getRegionX2());
	const int sy2 = std::min(gstate.getScissorY2(), gstate.getRegionY2());
	state->scissor.x1 = sx1 * SUBPIX;
	state->scissor.y1 = sy1 * SUBPIX;
	state->scissor.x2 = sx2 * SUBPIX + SUBPIX - 1;
	state->scissor.y2 = sy2 * SUBPIX + SUBPIX - 1;
}

}  // namespace gpu
