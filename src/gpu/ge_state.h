// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// The GE's registers: the last command word of each type, plus the matrices loaded through DATA
// commands. Field accessors follow GPU/GPUState.h.
#pragma once

#include "common/common.h"
#include "gpu/ge_constants.h"
#include "gpu/gemath.h"

namespace gpu {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201)  // nameless struct/union
#endif

struct GEState {
	union {
		u32 cmdmem[256];
		struct {
			u32 nop, vaddr, iaddr, pad00, prim, bezier, spline, boundBox, jump, bjump, call, ret, end, pad01, signal, finish,
				base, pad02, vertType, offsetAddr, origin, region1, region2, lightingEnable, lightEnable[4], depthClipEnable,
				cullfaceEnable, textureMapEnable, fogEnable, ditherEnable, alphaBlendEnable, alphaTestEnable, zTestEnable,
				stencilTestEnable, antiAliasEnable, patchCullEnable, colorTestEnable, logicOpEnable, pad03, boneMatrixNumber,
				boneMatrixData, morphwgt[8], pad04[2], patchdivision, patchprimitive, patchfacing, pad04_a,
				worldmtxnum, worldmtxdata, viewmtxnum, viewmtxdata, projmtxnum, projmtxdata, texmtxnum, texmtxdata,
				viewportxscale, viewportyscale, viewportzscale, viewportxcenter, viewportycenter, viewportzcenter,
				texscaleu, texscalev, texoffsetu, texoffsetv, offsetx, offsety, pad111[2], shademodel, reversenormals, pad222,
				materialupdate, materialemissive, materialambient, materialdiffuse, materialspecular, materialalpha, pad333[2],
				materialspecularcoef, ambientcolor, ambientalpha, lmode, ltype[4], lpos[12], ldir[12], latt[12], lconv[4],
				lcutoff[4], lcolor[12], cullmode, fbptr, fbwidth, zbptr, zbwidth, texaddr[8], texbufwidth[8], clutaddr,
				clutaddrupper, transfersrc, transfersrcw, transferdst, transferdstw, padxxx[2], texsize[8], texmapmode,
				texshade, texmode, texformat, loadclut, clutformat, texfilter, texwrap, texlevel, texfunc, texenvcolor,
				texflush, texsync, fog1, fog2, fogcolor, texlodslope, padxxxxxx, framebufpixformat, clearmode, scissor1,
				scissor2, minz, maxz, colortest, colorref, colortestmask, alphatest, stenciltest, stencilop, ztestfunc, blend,
				blendfixa, blendfixb, dithmtx[4], lop, zmsk, pmskc, pmska, transferstart, transfersrcpos, transferdstpos,
				pad99, transfersize, pad100, imm_vscx, imm_vscy, imm_vscz, imm_vtcs, imm_vtct, imm_vtcq, imm_cv, imm_ap,
				imm_fc, imm_scv;
			u32 pad05[0xFF - 0xF9];
		};
	};

	float worldMatrix[12];
	float viewMatrix[12];
	float projMatrix[16];
	float tgenMatrix[12];
	float boneMatrix[12 * 8];

	// Framebuffer and depth buffer always live in VRAM; the mirror bits are ignored.
	u32 getFrameBufAddress() const { return 0x04000000 | (fbptr & 0x1FFFF0); }
	GEBufferFormat FrameBufFormat() const { return (GEBufferFormat)(framebufpixformat & 3); }
	int FrameBufStride() const { return fbwidth & 0x7FC; }
	u32 getDepthBufAddress() const { return 0x04000000 | (zbptr & 0x1FFFF0); }
	int DepthBufStride() const { return zbwidth & 0x7FC; }

	bool isModeClear() const { return clearmode & 1; }
	bool isFogEnabled() const { return fogEnable & 1; }
	bool isCullEnabled() const { return cullfaceEnable & 1; }
	int getCullMode() const { return cullmode & 1; }
	bool isClearModeColorMask() const { return (clearmode & 0x100) != 0; }
	bool isClearModeAlphaMask() const { return (clearmode & 0x200) != 0; }
	bool isClearModeDepthMask() const { return (clearmode & 0x400) != 0; }

	int getBlendFuncA() const { return blend & 0xF; }
	int getBlendFuncB() const { return (blend >> 4) & 0xF; }
	u32 getFixA() const { return blendfixa & 0xFFFFFF; }
	u32 getFixB() const { return blendfixb & 0xFFFFFF; }
	int getBlendEq() const { return (blend >> 8) & 0x7; }
	bool isAlphaBlendEnabled() const { return alphaBlendEnable & 1; }
	bool isAntiAliasEnabled() const { return antiAliasEnable & 1; }
	bool isDitherEnabled() const { return ditherEnable & 1; }
	int getDitherValue(int x, int y) const {
		u8 raw = (dithmtx[y & 3] >> ((x & 3) * 4)) & 0xF;
		return ((s8)(raw << 4)) >> 4;
	}
	u32 getColorMask() const { return (pmskc & 0xFFFFFF) | ((pmska & 0xFF) << 24); }
	bool isLogicOpEnabled() const { return logicOpEnable & 1; }
	GELogicOp getLogicOp() const { return (GELogicOp)(lop & 0xF); }

	bool isDepthTestEnabled() const { return zTestEnable & 1; }
	bool isDepthWriteEnabled() const { return !(zmsk & 1); }
	GEComparison getDepthTestFunction() const { return (GEComparison)(ztestfunc & 0x7); }
	u16 getDepthRangeMin() const { return minz & 0xFFFF; }
	u16 getDepthRangeMax() const { return maxz & 0xFFFF; }

	bool isStencilTestEnabled() const { return stencilTestEnable & 1; }
	GEComparison getStencilTestFunction() const { return (GEComparison)(stenciltest & 0x7); }
	int getStencilTestRef() const { return (stenciltest >> 8) & 0xFF; }
	int getStencilTestMask() const { return (stenciltest >> 16) & 0xFF; }
	int getStencilOpSFail() const { return stencilop & 0x7; }
	int getStencilOpZFail() const { return (stencilop >> 8) & 0x7; }
	int getStencilOpZPass() const { return (stencilop >> 16) & 0x7; }

	bool isAlphaTestEnabled() const { return alphaTestEnable & 1; }
	GEComparison getAlphaTestFunction() const { return (GEComparison)(alphatest & 0x7); }
	int getAlphaTestRef() const { return (alphatest >> 8) & 0xFF; }
	int getAlphaTestMask() const { return (alphatest >> 16) & 0xFF; }

	bool isColorTestEnabled() const { return colorTestEnable & 1; }
	GEComparison getColorTestFunction() const { return (GEComparison)(colortest & 0x3); }
	u32 getColorTestRef() const { return colorref & 0xFFFFFF; }
	u32 getColorTestMask() const { return colortestmask & 0xFFFFFF; }

	u32 getTextureAddress(int level) const { return (texaddr[level] & 0xFFFFF0) | ((texbufwidth[level] << 8) & 0x0F000000); }
	int getTextureWidth(int level) const { return 1 << (texsize[level] & 0xf); }
	int getTextureHeight(int level) const { return 1 << ((texsize[level] >> 8) & 0xf); }
	int getTexLevelMode() const { return texlevel & 0x3; }
	int getTexLevelOffset16() const { return (int)(s8)((texlevel >> 16) & 0xFF); }
	bool isTextureMapEnabled() const { return textureMapEnable & 1; }
	int getTextureFunction() const { return texfunc & 0x7; }
	bool isColorDoublingEnabled() const { return (texfunc & 0x10000) != 0; }
	bool isTextureAlphaUsed() const { return (texfunc & 0x100) != 0; }
	GETextureFormat getTextureFormat() const { return (GETextureFormat)(texformat & 0xF); }
	bool isTextureFormatIndexed() const { return (texformat & 4) != 0; }
	u32 getTextureEnvColRGB() const { return texenvcolor & 0x00FFFFFF; }
	u32 getClutAddress() const { return (clutaddr & 0x00FFFFF0) | ((clutaddrupper << 8) & 0x0F000000); }
	int getClutLoadBlocks() const {
		if ((loadclut & 0x7F) == 0x40)
			return 0x40;
		return loadclut & 0x3F;
	}
	GEPaletteFormat getClutPaletteFormat() const { return (GEPaletteFormat)(clutformat & 3); }
	int getClutIndexShift() const { return (clutformat >> 2) & 0x1F; }
	int getClutIndexMask() const { return (clutformat >> 8) & 0xFF; }
	int getClutIndexStartPos() const { return ((clutformat >> 16) & 0x1F) << 4; }
	bool isTextureSwizzled() const { return texmode & 1; }
	bool isClutSharedForMipmaps() const { return (texmode & 0x100) == 0; }
	bool isMipmapEnabled() const { return (texfilter & 4) != 0; }
	bool isMipmapFilteringEnabled() const { return (texfilter & 2) != 0; }
	bool isMinifyFilteringEnabled() const { return (texfilter & 1) != 0; }
	bool isMagnifyFilteringEnabled() const { return (texfilter >> 8) & 1; }
	int getTextureMaxLevel() const { return (texmode >> 16) & 0x7; }
	float getTextureLodSlope() const { return getFloat24(texlodslope); }

	bool isLightingEnabled() const { return lightingEnable & 1; }
	bool isLightChanEnabled(int chan) const { return lightEnable[chan] & 1; }
	int getLightComputation(int chan) const { return ltype[chan] & 0x3; }
	bool isUsingPoweredDiffuseLight(int chan) const { return getLightComputation(chan) == GE_LIGHTCOMP_ONLYPOWDIFFUSE; }
	bool isUsingSpecularLight(int chan) const { return getLightComputation(chan) == GE_LIGHTCOMP_BOTH; }
	bool isUsingSecondaryColor() const { return lmode & 1; }
	int getLightType(int chan) const { return (ltype[chan] >> 8) & 3; }
	bool isDirectionalLight(int chan) const { return getLightType(chan) == GE_LIGHTTYPE_DIRECTIONAL; }
	bool isSpotLight(int chan) const { return getLightType(chan) >= GE_LIGHTTYPE_SPOT; }
	bool isShadeGouraud() const { return shademodel & 1; }
	u32 getAmbientRGBA() const { return (ambientcolor & 0xFFFFFF) | ((ambientalpha & 0xFF) << 24); }
	u32 getMaterialAmbientRGBA() const { return (materialambient & 0x00FFFFFF) | (materialalpha << 24); }
	u32 getMaterialDiffuse() const { return materialdiffuse & 0xffffff; }
	u32 getMaterialEmissive() const { return materialemissive & 0xffffff; }
	u32 getMaterialSpecular() const { return materialspecular & 0xffffff; }
	float getMaterialSpecularCoef() const { return getFloat24(materialspecularcoef); }
	u32 getLightAmbientColor(int chan) const { return lcolor[chan * 3] & 0xFFFFFF; }
	u32 getDiffuseColor(int chan) const { return lcolor[1 + chan * 3] & 0xFFFFFF; }
	u32 getSpecularColor(int chan) const { return lcolor[2 + chan * 3] & 0xFFFFFF; }

	int getPatchDivisionU() const { return patchdivision & 0x7F; }
	int getPatchDivisionV() const { return (patchdivision >> 8) & 0x7F; }
	int getPatchPrimitiveType() const { return patchprimitive & 3; }

	int getUVGenMode() const { return texmapmode & 3; }
	int getUVProjMode() const { return (texmapmode >> 8) & 3; }
	int getUVLS0() const { return texshade & 0x3; }
	int getUVLS1() const { return (texshade >> 8) & 0x3; }
	bool isTexCoordClampedS() const { return texwrap & 1; }
	bool isTexCoordClampedT() const { return (texwrap >> 8) & 1; }

	int getScissorX1() const { return scissor1 & 0x3FF; }
	int getScissorY1() const { return (scissor1 >> 10) & 0x3FF; }
	int getScissorX2() const { return scissor2 & 0x3FF; }
	int getScissorY2() const { return (scissor2 >> 10) & 0x3FF; }
	int getRegionX1() const { return region1 & 0x3FF; }
	int getRegionY1() const { return (region1 >> 10) & 0x3FF; }
	int getRegionX2() const { return region2 & 0x3FF; }
	int getRegionY2() const { return (region2 >> 10) & 0x3FF; }

	bool isDepthClipEnabled() const { return depthClipEnable & 1; }
	float getViewportXScale() const { return getFloat24(viewportxscale); }
	float getViewportYScale() const { return getFloat24(viewportyscale); }
	float getViewportZScale() const { return getFloat24(viewportzscale); }
	float getViewportXCenter() const { return getFloat24(viewportxcenter); }
	float getViewportYCenter() const { return getFloat24(viewportycenter); }
	float getViewportZCenter() const { return getFloat24(viewportzcenter); }
	int getOffsetX16() const { return offsetx & 0xFFFF; }
	int getOffsetY16() const { return offsety & 0xFFFF; }
	float getOffsetX() const { return (float)getOffsetX16() / 16.0f; }
	float getOffsetY() const { return (float)getOffsetY16() / 16.0f; }

	bool isModeThrough() const { return (vertType & GE_VTYPE_THROUGH) != 0; }
	bool areNormalsReversed() const { return reversenormals & 1; }

	u32 getTransferSrcAddress() const { return (transfersrc & 0xFFFFF0) | ((transfersrcw & 0xFF0000) << 8); }
	u32 getTransferSrcStride() const { int s = transfersrcw & 0x7F8; return s > 0x400 ? 0 : s; }
	int getTransferSrcX() const { return transfersrcpos & 0x3FF; }
	int getTransferSrcY() const { return (transfersrcpos >> 10) & 0x3FF; }
	u32 getTransferDstAddress() const { return (transferdst & 0xFFFFF0) | ((transferdstw & 0xFF0000) << 8); }
	u32 getTransferDstStride() const { int s = transferdstw & 0x7F8; return s > 0x400 ? 0 : s; }
	int getTransferDstX() const { return transferdstpos & 0x3FF; }
	int getTransferDstY() const { return (transferdstpos >> 10) & 0x3FF; }
	int getTransferWidth() const { return (transfersize & 0x3FF) + 1; }
	int getTransferHeight() const { return ((transfersize >> 10) & 0x3FF) + 1; }
	int getTransferBpp() const { return (transferstart & 1) ? 4 : 2; }
};

static_assert(sizeof(((GEState *)nullptr)->cmdmem) == 1024, "cmdmem");

// State that isn't a register: addresses with BASE/OFFSET applied, morph weights.
struct GEStateCache {
	u32 vertexAddr;
	u32 indexAddr;
	u32 offsetAddr;
	float morphWeights[8];

	u32 getRelativeAddress(u32 data, u32 base) const {
		u32 baseExtended = ((base & 0x000F0000) << 8) | data;
		return (offsetAddr + baseExtended) & 0x0FFFFFFF;
	}
};

// The matrix values the CPU reads back (sceGeGetMtx/SaveContext), 24-bit each; they update a little
// differently from the ones used for drawing when more DATA commands are sent than a matrix holds.
union MatrixVisible {
	u32 all[12 * 8 + 12 + 12 + 16 + 12];
	struct {
		u32 bone[12 * 8];
		u32 world[12];
		u32 view[12];
		u32 proj[16];
		u32 tgen[12];
	};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

extern GEState gstate;
extern GEStateCache gstate_c;
extern MatrixVisible matrixVisible;

void ResetGEState();
// The context format (PPSSPP's savedContextVersion 1): addresses at words 5-7, registers from word 17.
void SaveGEState(u32 *ptr);
void RestoreGEState(const u32 *ptr);
void ResetVisibleMatrices();

}  // namespace gpu
