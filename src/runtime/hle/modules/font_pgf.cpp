// SPDX-License-Identifier: GPL-2.0-or-later
//
// PGF font parsing and glyph rendering (see font_pgf.h for where the format knowledge comes from).
//
// File layout: a 0x188-byte header (0x19C for revision 3), four metric tables of (s32, s32)
// pairs (dimensions, x bearings, y bearings, advances), the shadow map, two compression tables
// (revision 3 only), the character map, the glyph pointer table and finally the glyph records.
// The shadow map, character map and pointer table are packed little-endian bit fields, each
// padded to a multiple of 32 bits. Glyph records are bit streams too:
//   14 bits record size in bytes, 7 bits width, 7 bits height, 7 bits left, 7 bits top (both
//   signed), 6 bits flags, 2+2+3 bits shadow flags, 9 bits shadow id; then for each of the
//   dimension, x bearing, y bearing and advance pairs either an 8-bit table index (flags bit
//   2/3/4/5 set) or two 32-bit values; then the run-length coded 4-bit pixels.
// A character's shadow image is a second record straight after the record of the character the
// shadow map names: 14 bits size, 7 bits each width/height/left/top, 6 bits flags, pixels.
#include "runtime/hle/modules/font_pgf.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "runtime/core/memory.h"

namespace font {

namespace {

u16 Le16(const u8 *p) {
	u16 v;
	memcpy(&v, p, 2);
	return v;
}
s32 Le32(const u8 *p) {
	s32 v;
	memcpy(&v, p, 4);
	return v;
}

// Bytes of `count` entries of `bpe` bits, padded to whole 32-bit words.
u32 PackedBytes(u32 count, u32 bpe) { return (u32)((((u64)count * bpe + 31) / 32) * 4); }

// Reads `count` (<= 32) bits LSB-first starting at bit `pos` of buf; bits past the end read as 0.
u32 ReadBits(const u8 *buf, size_t bytes, size_t pos, int count) {
	u32 v = 0;
	int got = 0;
	while (got < count) {
		size_t byte = pos >> 3;
		if (byte >= bytes) break;
		int shift = (int)(pos & 7);
		int take = std::min(8 - shift, count - got);
		v |= ((u32)(buf[byte] >> shift) & ((1u << take) - 1)) << got;
		got += take;
		pos += take;
	}
	return v;
}

int Signed7(u32 v) { return v >= 64 ? (int)v - 128 : (int)v; }

// Header offsets.
enum : u32 {
	H_HEADER_SIZE = 0x02,
	H_REVISION = 0x08,
	H_CHARMAP_LEN = 0x10,
	H_CHARPTR_LEN = 0x14,
	H_CHARMAP_BPE = 0x18,
	H_CHARPTR_BPE = 0x1C,
	H_BPP = 0x22,
	H_HSIZE = 0x24,
	H_VSIZE = 0x28,
	H_HRES = 0x2C,
	H_VRES = 0x30,
	H_FIRST_GLYPH = 0xB6,
	H_MAX_ASCENDER = 0xD4,
	H_MAX_DESCENDER = 0xD8,
	H_MAX_LEFTX = 0xDC,
	H_MAX_BASEY = 0xE0,
	H_MIN_CENTERX = 0xE4,
	H_MAX_TOPY = 0xE8,
	H_MAX_ADVANCE = 0xEC,
	H_MAX_SIZE = 0xF4,
	H_MAX_GLYPH_W = 0xFC,
	H_MAX_GLYPH_H = 0xFE,
	H_DIM_LEN = 0x102,
	H_XADJ_LEN = 0x103,
	H_YADJ_LEN = 0x104,
	H_ADV_LEN = 0x105,
	H_SHADOWMAP_LEN = 0x16C,
	H_SHADOWMAP_BPE = 0x170,
	// Revision 3 extension, right after the header.
	X_COMP1_LEN = 0x188 + 0x04,
	X_COMP2_LEN = 0x188 + 0x0C,
};

// Real fonts are far below these; they keep crafted files from causing huge allocations.
constexpr s32 kMaxCharMapLen = 0x100000;
constexpr s32 kMaxGlyphs = 0x100000;
constexpr s32 kMaxShadowMapLen = 0x10000;

bool ValidTable(s32 len, s32 bpe, s32 maxLen) {
	if (len < 0 || len > maxLen || bpe < 0 || bpe > 32) return false;
	return len == 0 || bpe != 0;
}

const u8 kFontPixelBytes[] = {0, 0, 1, 3, 4};  // 0: two pixels per byte

}  // namespace

bool ReadPgfLayout(const u8 *data, size_t size, PgfLayout &out) {
	out = PgfLayout{};
	if (size < kPgfHeaderBytes) return false;
	out.revision = Le32(data + H_REVISION);
	out.headerBytes = kPgfHeaderBytes;
	if (out.revision == 3) {
		if (size < kPgfHeaderBytes + kPgfRev3ExtraBytes) return false;
		out.headerBytes += kPgfRev3ExtraBytes;
		out.comp1Bytes = (Le32(data + X_COMP1_LEN) & 0xFFFF) * 4u;
		out.comp2Bytes = (Le32(data + X_COMP2_LEN) & 0xFFFF) * 4u;
	}
	s32 charMapLen = Le32(data + H_CHARMAP_LEN), charMapBpe = Le32(data + H_CHARMAP_BPE);
	s32 charPtrLen = Le32(data + H_CHARPTR_LEN), charPtrBpe = Le32(data + H_CHARPTR_BPE);
	s32 shadowLen = Le32(data + H_SHADOWMAP_LEN), shadowBpe = Le32(data + H_SHADOWMAP_BPE);
	if (!ValidTable(charMapLen, charMapBpe, kMaxCharMapLen) || !ValidTable(charPtrLen, charPtrBpe, kMaxGlyphs) ||
	    !ValidTable(shadowLen, shadowBpe, kMaxShadowMapLen))
		return false;
	out.dimLen = data[H_DIM_LEN];
	out.xAdjLen = data[H_XADJ_LEN];
	out.yAdjLen = data[H_YADJ_LEN];
	out.advLen = data[H_ADV_LEN];
	out.shadowMapBytes = PackedBytes((u32)shadowLen, (u32)shadowBpe);
	out.charMapBytes = PackedBytes((u32)charMapLen, (u32)charMapBpe);
	out.charPtrBytes = PackedBytes((u32)charPtrLen, (u32)charPtrBpe);
	return true;
}

bool PgfFont::Parse(std::vector<u8> file) {
	*this = PgfFont{};
	const u8 *d = file.data();
	if (!ReadPgfLayout(d, file.size(), layout_)) return false;
	if ((size_t)layout_.GlyphDataOffset() > file.size()) return false;

	hSize_ = Le32(d + H_HSIZE);
	vSize_ = Le32(d + H_VSIZE);
	hRes_ = Le32(d + H_HRES);
	vRes_ = Le32(d + H_VRES);
	bpp_ = d[H_BPP];
	firstGlyph_ = Le16(d + H_FIRST_GLYPH);
	maxAscender_ = Le32(d + H_MAX_ASCENDER);
	maxDescender_ = Le32(d + H_MAX_DESCENDER);
	maxLeftX_ = Le32(d + H_MAX_LEFTX);
	maxBaseY_ = Le32(d + H_MAX_BASEY);
	minCenterX_ = Le32(d + H_MIN_CENTERX);
	maxTopY_ = Le32(d + H_MAX_TOPY);
	maxAdvance_[0] = Le32(d + H_MAX_ADVANCE);
	maxAdvance_[1] = Le32(d + H_MAX_ADVANCE + 4);
	maxSize_[0] = Le32(d + H_MAX_SIZE);
	maxSize_[1] = Le32(d + H_MAX_SIZE + 4);
	maxGlyphW_ = Le16(d + H_MAX_GLYPH_W);
	maxGlyphH_ = Le16(d + H_MAX_GLYPH_H);

	size_t pos = layout_.headerBytes;
	auto readPairs = [&](std::vector<Pair> &t, u32 n) {
		t.resize(n);
		for (u32 i = 0; i < n; i++, pos += 8) t[i] = {Le32(d + pos), Le32(d + pos + 4)};
	};
	readPairs(dims_, layout_.dimLen);
	readPairs(xAdj_, layout_.xAdjLen);
	readPairs(yAdj_, layout_.yAdjLen);
	readPairs(adv_, layout_.advLen);

	auto readPacked = [&](std::vector<u32> &t, u32 headerLenOff, u32 headerBpeOff, u32 bytes) {
		u32 n = (u32)Le32(d + headerLenOff);
		int bpe = Le32(d + headerBpeOff);
		t.resize(n);
		for (u32 i = 0; i < n; i++) t[i] = ReadBits(d + pos, bytes, (size_t)i * bpe, bpe);
		pos += bytes;
	};
	readPacked(shadowMap_, H_SHADOWMAP_LEN, H_SHADOWMAP_BPE, layout_.shadowMapBytes);
	pos += layout_.comp1Bytes + layout_.comp2Bytes;  // not needed for lookups
	readPacked(charMap_, H_CHARMAP_LEN, H_CHARMAP_BPE, layout_.charMapBytes);
	readPacked(charPtr_, H_CHARPTR_LEN, H_CHARPTR_BPE, layout_.charPtrBytes);
	numGlyphs_ = (u32)charPtr_.size();

	glyphBase_ = pos;
	file_ = std::move(file);
	valid_ = true;
	return true;
}

void PgfFont::InitBlank(s32 hSize, s32 vSize, s32 hRes, s32 vRes, bool wide) {
	*this = PgfFont{};
	hSize_ = hSize;
	vSize_ = vSize;
	hRes_ = hRes;
	vRes_ = vRes;
	bpp_ = 4;
	firstGlyph_ = 0x20;
	blank_ = true;
	blankWide_ = wide;
	// Scale the header metrics of the real fonts (measured at 18 px, i.e. 1152 in 26.6) to the
	// em size this font would have.
	s32 em = (s32)((s64)hSize * (hRes / 64) / 72);
	auto scale = [em](s32 ref) { return (s32)((s64)ref * em / 1152); };
	if (wide) {
		maxSize_[0] = scale(1161);
		maxSize_[1] = scale(1251);
		maxAscender_ = scale(1013);
		maxDescender_ = scale(-139);
		maxLeftX_ = scale(1008);
		maxBaseY_ = scale(1204);
		minCenterX_ = scale(-580);
		maxTopY_ = scale(1151);
		maxAdvance_[0] = scale(1198);
		maxAdvance_[1] = scale(1198);
	} else {
		maxSize_[0] = scale(1455);
		maxSize_[1] = scale(1222);
		maxAscender_ = scale(863);
		maxDescender_ = scale(-196);
		maxLeftX_ = scale(195);
		maxBaseY_ = scale(1202);
		minCenterX_ = scale(-727);
		maxTopY_ = scale(18);
		maxAdvance_[0] = scale(1585);
		maxAdvance_[1] = scale(1094);
	}
	maxGlyphW_ = (u16)((maxSize_[0] + 63) / 64 + 1);
	maxGlyphH_ = (u16)((maxSize_[1] + 63) / 64 + 1);
	numGlyphs_ = 0;
	valid_ = true;
}

u32 PgfFont::Bits(size_t bitPos, int count) const {
	return ReadBits(file_.data() + glyphBase_, file_.size() - glyphBase_, bitPos, count);
}

bool PgfFont::GlyphIndex(int code, u32 &index) const {
	if (code < firstGlyph_) return false;
	u32 i = (u32)(code - firstGlyph_);
	// Codes past the end of the map index the glyphs directly.
	if (i < charMap_.size()) i = charMap_[i];
	if (i >= numGlyphs_) return false;
	index = i;
	return true;
}

bool PgfFont::DecodeRecord(u32 index, Glyph &g) const {
	size_t glyphBytes = file_.size() - glyphBase_;
	size_t bit = (size_t)charPtr_[index] * 32;
	if (bit / 8 + 8 > glyphBytes) return false;
	auto take = [&](int n) {
		u32 v = Bits(bit, n);
		bit += n;
		return v;
	};
	g = Glyph{};
	g.recordBytes = take(14);
	g.w = (int)take(7);
	g.h = (int)take(7);
	g.left = Signed7(take(7));
	g.top = Signed7(take(7));
	g.flags = (int)take(6);
	g.shadowFlags = (int)(take(2) << 5);
	g.shadowFlags |= (int)(take(2) << 3);
	g.shadowFlags |= (int)take(3);
	g.shadowId = (int)take(9);
	auto pair = [&](int flagBit, const std::vector<Pair> &table, s32 &a, s32 &b) {
		if (g.flags & flagBit) {
			u32 i = take(8);
			if (i < table.size()) {
				a = table[i].a;
				b = table[i].b;
			}
		} else {
			a = (s32)take(32);
			b = (s32)take(32);
		}
	};
	pair(0x04, dims_, g.dimW, g.dimH);
	pair(0x08, xAdj_, g.xAdjH, g.xAdjV);
	pair(0x10, yAdj_, g.yAdjH, g.yAdjV);
	pair(0x20, adv_, g.advH, g.advV);
	g.bitmapBit = bit;
	return true;
}

bool PgfFont::Lookup(int code, GlyphKind kind, Glyph &g) const {
	if (blank_) {
		if (code < firstGlyph_ || code > 0xFFFF) return false;
		s32 em = (s32)((s64)hSize_ * (hRes_ / 64) / 72);
		g = Glyph{};
		g.advH = (blankWide_ && code >= 0x80) ? em : em * 5 / 8;
		g.advV = em;
		return true;
	}
	u32 index;
	if (!valid_ || !GlyphIndex(code, index) || !DecodeRecord(index, g)) return false;
	if (kind == GlyphKind::Char) return true;

	if ((size_t)g.shadowId >= shadowMap_.size()) return false;
	u32 ownerIndex;
	Glyph owner;
	if (!GlyphIndex((int)shadowMap_[g.shadowId], ownerIndex) || !DecodeRecord(ownerIndex, owner)) return false;
	size_t bit = (size_t)charPtr_[ownerIndex] * 32 + (size_t)owner.recordBytes * 8;
	if (bit / 8 + 6 > file_.size() - glyphBase_) return false;
	auto take = [&](int n) {
		u32 v = Bits(bit, n);
		bit += n;
		return v;
	};
	g.recordBytes = take(14);
	g.w = (int)take(7);
	g.h = (int)take(7);
	g.left = Signed7(take(7));
	g.top = Signed7(take(7));
	g.flags = (int)take(6);
	g.bitmapBit = bit;
	return true;
}

bool PgfFont::LookupWithAlt(int code, int altCode, GlyphKind kind, Glyph &g) const {
	if (Lookup(code, kind, g)) return true;
	// Codes below the first glyph are just empty; only missing characters get the substitute.
	if (code < firstGlyph_) return false;
	return Lookup(altCode, kind, g);
}

bool PgfFont::GetCharInfo(int code, int altCode, GlyphKind kind, CharInfo &out) const {
	memset(&out, 0, sizeof(out));
	Glyph g;
	if (!LookupWithAlt(code, altCode, kind, g)) return false;
	out.bitmapWidth = (u32)g.w;
	out.bitmapHeight = (u32)g.h;
	out.bitmapLeft = g.left;
	out.bitmapTop = g.top;
	out.sfp26Width = (u32)g.dimW;
	out.sfp26Height = (u32)g.dimH;
	out.sfp26Ascender = g.yAdjH;
	out.sfp26Descender = g.yAdjH - g.dimH;
	out.sfp26BearingHX = g.xAdjH;
	out.sfp26BearingHY = g.yAdjH;
	out.sfp26BearingVX = g.xAdjV;
	out.sfp26BearingVY = g.yAdjV;
	out.sfp26AdvanceH = g.advH;
	out.sfp26AdvanceV = g.advV;
	out.shadowFlags = (s16)g.shadowFlags;
	out.shadowId = (s16)g.shadowId;
	return true;
}

void PgfFont::GlyphBufferSizes(int code, int altCode, GlyphKind kind, u32 &recordBytes, u32 &bitmapBytes) const {
	recordBytes = bitmapBytes = 0;
	Glyph g;
	if (!LookupWithAlt(code, altCode, kind, g)) return;
	// libfont reads a character's fixed header (8 bytes from revision 2 on, 6 before) into a stack
	// buffer, then allocates the rest of the record (its 14-bit size minus that header) and reads
	// it there (4.01 libfont_hv.prx, the glyph loader at module offset 0x42A4; pspautotests
	// font/fonttest).
	const u32 header = kind == GlyphKind::Char ? (layout_.revision >= 2 ? 8u : 6u) : 0u;
	recordBytes = g.recordBytes > header ? g.recordBytes - header : 0;
	bitmapBytes = (u32)(g.w * g.h);
}

void PgfFont::GetFontInfo(FontInfo &out) const {
	out.maxGlyphWidthI = maxSize_[0];
	out.maxGlyphHeightI = maxSize_[1];
	out.maxGlyphAscenderI = maxAscender_;
	out.maxGlyphDescenderI = maxDescender_;
	out.maxGlyphLeftXI = maxLeftX_;
	out.maxGlyphBaseYI = maxBaseY_;
	out.minGlyphCenterXI = minCenterX_;
	out.maxGlyphTopYI = maxTopY_;
	out.maxGlyphAdvanceXI = maxAdvance_[0];
	out.maxGlyphAdvanceYI = maxAdvance_[1];
	out.maxGlyphWidthF = maxSize_[0] / 64.0f;
	out.maxGlyphHeightF = maxSize_[1] / 64.0f;
	out.maxGlyphAscenderF = maxAscender_ / 64.0f;
	out.maxGlyphDescenderF = maxDescender_ / 64.0f;
	out.maxGlyphLeftXF = maxLeftX_ / 64.0f;
	out.maxGlyphBaseYF = maxBaseY_ / 64.0f;
	out.minGlyphCenterXF = minCenterX_ / 64.0f;
	out.maxGlyphTopYF = maxTopY_ / 64.0f;
	out.maxGlyphAdvanceXF = maxAdvance_[0] / 64.0f;
	out.maxGlyphAdvanceYF = maxAdvance_[1] / 64.0f;
	out.maxGlyphWidth = (s16)maxGlyphW_;
	out.maxGlyphHeight = (s16)maxGlyphH_;
	out.numGlyphs = (s32)numGlyphs_;
	out.shadowMapLength = 0;  // the firmware reports 0 here (pspautotests font/fontinfo)
	out.bpp = (u8)bpp_;
}

void PgfFont::DecodeBitmap(const Glyph &g, std::vector<u8> &out) const {
	// Runs of 4-bit pixels: a nibble n < 8 repeats the next nibble n + 1 times, n >= 8 is
	// followed by 16 - n literal nibbles.
	size_t total = (size_t)g.w * g.h;
	out.assign(total, 0);
	size_t endBit = (file_.size() - glyphBase_) * 8;
	size_t bit = g.bitmapBit;
	size_t i = 0;
	while (i < total && bit + 4 <= endBit) {
		u32 n = Bits(bit, 4);
		bit += 4;
		if (n < 8) {
			u8 v = (u8)Bits(bit, 4);
			bit += 4;
			for (u32 k = 0; k <= n && i < total; k++) out[i++] = v;
		} else {
			for (u32 k = 0; k < 16 - n && i < total; k++, bit += 4) out[i++] = (u8)Bits(bit, 4);
		}
	}
}

bool PgfFont::RenderHostGlyph(int code, HostGlyph &out) const {
	out = HostGlyph{};
	Glyph g;
	if (blank_ || !Lookup(code, GlyphKind::Char, g)) return false;
	out.advH = g.advH;
	out.left = g.left;
	out.top = g.top;
	const int order = g.flags & 3;
	if (g.w <= 0 || g.h <= 0 || (order != 1 && order != 2)) return true;  // a space
	std::vector<u8> pixels;
	DecodeBitmap(g, pixels);
	out.w = g.w;
	out.h = g.h;
	out.ink.resize((size_t)g.w * g.h);
	for (int y = 0; y < g.h; y++)
		for (int x = 0; x < g.w; x++)
			out.ink[(size_t)y * g.w + x] = pixels[order == 1 ? (size_t)y * g.w + x : (size_t)x * g.h + y];
	return true;
}

void PgfFont::Draw(const GlyphImage &img, int clipX, int clipY, int clipW, int clipH, int code, int altCode,
                   GlyphKind kind) const {
	Glyph g;
	if (!LookupWithAlt(code, altCode, kind, g)) return;
	if (g.w <= 0 || g.h <= 0) return;
	const int order = g.flags & 3;
	if (order != 1 && order != 2) {
		LOGW("FONT", "glyph %04x has bitmap order %d, not drawn", code, order);
		return;
	}
	// The hardware only draws into 4 and 8 bpp buffers; the other formats return success and
	// leave the buffer untouched (PPSSPP's PSP-1000 measurements, pspautotests
	// font/charglyphimagexfrac).
	const s32 fmt = img.pixelFormat;
	if (fmt != PIXFMT_4 && fmt != PIXFMT_8) return;

	std::vector<u8> pixels;
	DecodeBitmap(g, pixels);
	auto sample = [&](int x, int y) -> int {
		if (x < 0 || y < 0 || x >= g.w || y >= g.h) return 0;
		return pixels[order == 1 ? (size_t)y * g.w + x : (size_t)x * g.h + y];
	};

	// The vertical fraction is ignored; a horizontal one blends each column with its left
	// neighbour and widens the drawn area by one column.
	const int x0 = img.xPos64 >> 6;
	const int y0 = img.yPos64 >> 6;
	const int frac = img.xPos64 & 63;
	if (clipX < 0) clipX = 0;
	if (clipY < 0) clipY = 0;
	if (clipW < 0) clipW = 8192;
	if (clipH < 0) clipH = 8192;
	const int colBegin = std::max(clipX, x0) - x0;
	const int rowBegin = std::max(clipY, y0) - y0;
	const int colEnd = (int)std::min<s64>((s64)clipX + clipW - x0, g.w + (frac ? 1 : 0));
	const int rowEnd = (int)std::min<s64>((s64)clipY + clipH - y0, g.h);
	// Blending happens at the destination's precision: raw nibbles for 4 bpp, nibble * 17 for 8.
	const int scale = fmt == PIXFMT_4 ? 1 : 17;
	const int maxValue = fmt == PIXFMT_4 ? 15 : 255;

	for (int row = rowBegin; row < rowEnd; row++) {
		const int y = y0 + row;
		if (y < 0 || y >= (int)img.bufHeight) continue;
		for (int col = colBegin; col < colEnd; col++) {
			const int x = x0 + col;
			if (x < 0 || x >= (int)img.bufWidth) continue;
			const int left = sample(col - 1, row) * scale;
			const int self = sample(col, row) * scale;
			// The left weight rounds down and the pixel's own weight rounds up, so two full-ink
			// samples always sum to the format's maximum.
			const int value = (left * frac) / 64 + (self * (64 - frac) + 63) / 64;
			if (value == 0) continue;
			// No check against bytesPerLine: a short line just makes rows overlap (pspautotests
			// font/charglyphimage "Linesize = 1").
			const u32 addr = img.buffer + (u32)y * img.bytesPerLine +
			                 (kFontPixelBytes[fmt] == 0 ? (u32)x / 2 : (u32)x * kFontPixelBytes[fmt]);
			u8 *p = mem::GetPointer(addr, 1);
			if (!p) continue;
			// Ink adds to the existing contents, saturating.
			if (fmt == PIXFMT_4) {
				const int shift = (x & 1) ? 4 : 0;
				const int sum = std::min(((*p >> shift) & 0xF) + value, maxValue);
				*p = (u8)((*p & ~(0xF << shift)) | (sum << shift));
			} else {
				*p = (u8)std::min((int)*p + value, maxValue);
			}
		}
	}
}

}  // namespace font
