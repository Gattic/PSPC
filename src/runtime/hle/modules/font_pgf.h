// SPDX-License-Identifier: GPL-2.0-or-later
//
// PGF, the PSP firmware's bitmap font format, for sceLibFont: header and table parsing, glyph
// record decoding and glyph rendering into guest glyph buffers.
//
// The format was documented by the JPCSP and PPSSPP projects (PPSSPP Core/Font/PGF.h and PGF.cpp,
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later). PPSSPP's PGF.cpp notes that parts of it
// were copied from JPCSP (GPL-3.0), so this file is a separate implementation written from the
// format description rather than a port of that code. The rendering rules (additive saturating
// writes, horizontal-only subpixel blending, which formats draw) follow PPSSPP's hardware
// findings and are checked against pspautotests font/.
#pragma once

#include <cstddef>
#include <vector>

#include "common/common.h"

namespace font {

enum PixelFormat : s32 {
	PIXFMT_4 = 0,      // two pixels per byte, even pixel in the low nibble
	PIXFMT_4_REV = 1,  // two pixels per byte, even pixel in the high nibble
	PIXFMT_8 = 2,
	PIXFMT_24 = 3,
	PIXFMT_32 = 4,
};

enum class GlyphKind { Char, Shadow };

#pragma pack(push, 1)
// Guest structures of the sceLibFont API.
struct FontStyle {
	float fontH, fontV, fontHRes, fontVRes, fontWeight;
	u16 fontFamily, fontStyle, fontStyleSub, fontLanguage, fontRegion, fontCountry;
	char fontName[64];
	char fontFileName[64];
	u32 fontAttributes;
	u32 fontExpire;
};
static_assert(sizeof(FontStyle) == 0xA8, "FontStyle layout");

struct FontInfo {
	// Metrics in 26.6 fixed point, then the same as floats (pixels).
	s32 maxGlyphWidthI, maxGlyphHeightI, maxGlyphAscenderI, maxGlyphDescenderI;
	s32 maxGlyphLeftXI, maxGlyphBaseYI, minGlyphCenterXI, maxGlyphTopYI;
	s32 maxGlyphAdvanceXI, maxGlyphAdvanceYI;
	float maxGlyphWidthF, maxGlyphHeightF, maxGlyphAscenderF, maxGlyphDescenderF;
	float maxGlyphLeftXF, maxGlyphBaseYF, minGlyphCenterXF, maxGlyphTopYF;
	float maxGlyphAdvanceXF, maxGlyphAdvanceYF;
	s16 maxGlyphWidth, maxGlyphHeight;
	s32 numGlyphs;
	s32 shadowMapLength;
	FontStyle fontStyle;
	u8 bpp;
	u8 pad[3];
};
static_assert(sizeof(FontInfo) == 264, "FontInfo layout");
// The library writes everything up to and including bpp; the padding keeps what was there
// (pspautotests font/fontinfo).
constexpr u32 kFontInfoWrittenBytes = (u32)offsetof(FontInfo, pad);

struct CharInfo {
	u32 bitmapWidth, bitmapHeight;
	s32 bitmapLeft, bitmapTop;
	// 26.6 fixed point.
	u32 sfp26Width, sfp26Height;
	s32 sfp26Ascender, sfp26Descender;
	s32 sfp26BearingHX, sfp26BearingHY, sfp26BearingVX, sfp26BearingVY;
	s32 sfp26AdvanceH, sfp26AdvanceV;
	s16 shadowFlags, shadowId;
};
static_assert(sizeof(CharInfo) == 60, "CharInfo layout");

struct GlyphImage {
	s32 pixelFormat;
	s32 xPos64, yPos64;  // 26.6 fixed point
	u16 bufWidth, bufHeight, bytesPerLine, pad;
	u32 buffer;
};
static_assert(sizeof(GlyphImage) == 24, "GlyphImage layout");
#pragma pack(pop)

// Where the parts of a PGF file are, derived from its header alone.
struct PgfLayout {
	s32 revision = 0;
	u32 headerBytes = 0;  // fixed header, plus the revision 3 extension
	u32 dimLen = 0, xAdjLen = 0, yAdjLen = 0, advLen = 0;  // entries of two s32 each
	u32 shadowMapBytes = 0, comp1Bytes = 0, comp2Bytes = 0, charMapBytes = 0, charPtrBytes = 0;

	u32 MetricTableBytes() const { return (dimLen + xAdjLen + yAdjLen + advLen) * 8; }
	// Everything between the header and the glyph data: what a font opened without its glyphs
	// keeps in memory.
	u32 TableBytes() const {
		return MetricTableBytes() + shadowMapBytes + comp1Bytes + comp2Bytes + charMapBytes + charPtrBytes;
	}
	u32 GlyphDataOffset() const { return headerBytes + TableBytes(); }
};

constexpr u32 kPgfHeaderBytes = 0x188;
constexpr u32 kPgfRev3ExtraBytes = 0x14;

// Fills `out` from the first bytes of a PGF file (at least kPgfHeaderBytes, plus the revision 3
// extension when present). Returns false for headers with impossible sizes.
bool ReadPgfLayout(const u8 *data, size_t size, PgfLayout &out);

// One glyph record. Bitmap fields come from the shadow record for GlyphKind::Shadow; the
// metrics are always the character's.
struct Glyph {
	int w = 0, h = 0, left = 0, top = 0;
	int flags = 0;  // low two bits: 1 = bitmap stored in rows, 2 = in columns
	int shadowFlags = 0, shadowId = 0;
	s32 dimW = 0, dimH = 0, xAdjH = 0, xAdjV = 0, yAdjH = 0, yAdjV = 0, advH = 0, advV = 0;
	u32 recordBytes = 0;
	size_t bitmapBit = 0;  // start of the pixel data, in bits from the start of the glyph data
};

class PgfFont {
public:
	// Parses a complete PGF file. Returns false (and stays invalid) for unusable data.
	bool Parse(std::vector<u8> file);
	// A font without glyph images, for firmware fonts that aren't installed: every character
	// from U+0020 up has a plausible advance and an empty bitmap, like a space. `wide` makes
	// non-ASCII characters full-width (CJK fonts).
	void InitBlank(s32 hSize, s32 vSize, s32 hRes, s32 vRes, bool wide);

	bool IsValid() const { return valid_; }
	bool IsBlank() const { return blank_; }
	const PgfLayout &Layout() const { return layout_; }
	s32 HSize() const { return hSize_; }
	s32 VSize() const { return vSize_; }
	s32 HResolution() const { return hRes_; }
	s32 VResolution() const { return vRes_; }
	int FirstGlyph() const { return firstGlyph_; }

	// Character metrics, falling back to altCode for characters the font lacks (but not for
	// codes below the first glyph). Returns false, with `out` zeroed, when neither exists.
	bool GetCharInfo(int code, int altCode, GlyphKind kind, CharInfo &out) const;
	// Size of the glyph's record in the file and of its decoded bitmap (one byte per pixel), with
	// the same fallback as GetCharInfo. Both 0 when there is no glyph.
	void GlyphBufferSizes(int code, int altCode, GlyphKind kind, u32 &recordBytes, u32 &bitmapBytes) const;
	// Everything but fontStyle.
	void GetFontInfo(FontInfo &out) const;
	// Draws a glyph into the guest buffer described by `img`, adding to what is there.
	// Negative clip values mean "no clipping on that side".
	void Draw(const GlyphImage &img, int clipX, int clipY, int clipW, int clipH, int code, int altCode,
	          GlyphKind kind) const;

	// A character's image for drawing on the host (the sceUtility dialog overlays): one byte per
	// pixel holding the 4-bit ink (0-15), rows top to bottom; the bitmap goes `left` pixels right
	// of the pen and its top `top` pixels above the baseline, and the pen then moves `advH`
	// (26.6). False when the font has no glyph for `code` (blank fonts have none).
	struct HostGlyph {
		int w = 0, h = 0, left = 0, top = 0;
		s32 advH = 0;
		std::vector<u8> ink;
	};
	bool RenderHostGlyph(int code, HostGlyph &out) const;

private:
	bool GlyphIndex(int code, u32 &index) const;
	bool DecodeRecord(u32 index, Glyph &g) const;
	bool Lookup(int code, GlyphKind kind, Glyph &g) const;
	bool LookupWithAlt(int code, int altCode, GlyphKind kind, Glyph &g) const;
	u32 Bits(size_t bitPos, int count) const;
	void DecodeBitmap(const Glyph &g, std::vector<u8> &out) const;

	bool valid_ = false;
	bool blank_ = false;
	bool blankWide_ = false;
	PgfLayout layout_;
	std::vector<u8> file_;
	size_t glyphBase_ = 0;  // offset of the glyph data in file_

	s32 hSize_ = 0, vSize_ = 0, hRes_ = 0, vRes_ = 0;
	int bpp_ = 4;
	int firstGlyph_ = 0;
	u32 numGlyphs_ = 0;
	s32 maxAscender_ = 0, maxDescender_ = 0, maxLeftX_ = 0, maxBaseY_ = 0, minCenterX_ = 0, maxTopY_ = 0;
	s32 maxAdvance_[2] = {}, maxSize_[2] = {};
	u16 maxGlyphW_ = 0, maxGlyphH_ = 0;

	struct Pair {
		s32 a, b;
	};
	std::vector<Pair> dims_, xAdj_, yAdj_, adv_;
	std::vector<u32> shadowMap_;  // shadow id -> character code whose record holds the shadow
	std::vector<u32> charMap_;    // code - firstGlyph -> glyph index
	std::vector<u32> charPtr_;    // glyph index -> record offset in 32-bit words
};

}  // namespace font
