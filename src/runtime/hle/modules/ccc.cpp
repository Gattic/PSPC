// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceCcc (libccc.prx): character code conversion between UTF-8, UTF-16 (little endian) and
// Shift-JIS. The library holds no tables of its own: the game passes the JIS<->UCS-2 tables (64K
// u16 entries each, indexed by JIS X 0208 code / UCS-2 code point) to sceCccSetTable, and they stay
// in guest memory.
//
// Ported from PPSSPP (Core/HLE/sceCcc.cpp and Common/Data/Encoding/{Utf8,Utf16,Shiftjis}.h),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. The conversions stop at a character that
// can't be decoded or mapped and has no replacement (the error characters default to 0), and
// leave room for the terminator, as libccc does (pspautotests ccc/convertstring).
#include <string>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/misc_libs.h"

namespace {

constexpr const char *LOG = "CCC";
constexpr u32 INVALID = 0xFFFFFFFF;

u16 g_errorUTF8 = 0, g_errorUTF16 = 0, g_errorSJIS = 0;
// Guest addresses of the two 0x20000-byte tables (0 until sceCccSetTable).
u32 g_jis2ucsTable = 0, g_ucs2jisTable = 0;

// ---------------------------------------------------------------- guest byte streams

// Reads guest memory sequentially; reads outside valid memory give 0 (which ends a string).
struct GuestReader {
	u32 addr;
	u8 Byte(u32 off = 0) const { return mem::IsValidAddress(addr + off) ? GRead8(addr + off) : 0; }
	u16 Unit(u32 off = 0) const { return mem::IsValidRange(addr + off * 2, 2) ? GRead16(addr + off * 2) : 0; }
};

// UTF-8: the next code point, or INVALID for a malformed sequence (a bad lead byte consumes one
// byte; a truncated sequence consumes the bytes that were valid).
u32 DecodeUTF8(u32 *addr) {
	GuestReader r{*addr};
	u8 b0 = r.Byte();
	if (b0 < 0x80) {
		*addr += b0 ? 1 : 0;
		return b0;
	}
	int extra;
	u32 c;
	if (b0 >= 0xC2 && b0 <= 0xDF) {
		extra = 1;
		c = b0 & 0x1F;
	} else if (b0 >= 0xE0 && b0 <= 0xEF) {
		extra = 2;
		c = b0 & 0x0F;
	} else if (b0 >= 0xF0 && b0 <= 0xF4) {
		extra = 3;
		c = b0 & 0x07;
	} else {
		*addr += 1;
		return INVALID;
	}
	for (int i = 1; i <= extra; i++) {
		u8 b = r.Byte(i);
		if ((b & 0xC0) != 0x80) {
			*addr += i;
			return INVALID;
		}
		c = (c << 6) | (b & 0x3F);
	}
	*addr += extra + 1;
	// Overlong forms, surrogates and values past U+10FFFF.
	if ((extra == 2 && c < 0x800) || (extra == 3 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF)) {
		return INVALID;
	}
	return c;
}

u32 EncodeUTF8Units(u32 c) {
	if (c < 0x80) return 1;
	if (c < 0x800) return 2;
	if (c < 0x10000) return 3;
	if (c < 0x110000) return 4;
	return 0;
}

u32 EncodeUTF8(u8 *out, u32 c) {
	if (c < 0x80) {
		out[0] = (u8)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (u8)(0xC0 | (c >> 6));
		out[1] = (u8)(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (u8)(0xE0 | (c >> 12));
		out[1] = (u8)(0x80 | ((c >> 6) & 0x3F));
		out[2] = (u8)(0x80 | (c & 0x3F));
		return 3;
	}
	if (c < 0x110000) {
		out[0] = (u8)(0xF0 | (c >> 18));
		out[1] = (u8)(0x80 | ((c >> 12) & 0x3F));
		out[2] = (u8)(0x80 | ((c >> 6) & 0x3F));
		out[3] = (u8)(0x80 | (c & 0x3F));
		return 4;
	}
	return 0;
}

// UTF-16LE: a surrogate that isn't part of a proper pair is INVALID (and consumes one unit).
u32 DecodeUTF16(u32 *addr) {
	GuestReader r{*addr};
	u32 u = r.Unit();
	if (u == 0) return 0;
	if (u >= 0xD800 && u <= 0xDBFF) {
		u32 lo = r.Unit(1);
		if (lo >= 0xDC00 && lo <= 0xDFFF) {
			*addr += 4;
			return 0x10000 + (((u & 0x3FF) << 10) | (lo & 0x3FF));
		}
		*addr += 2;
		return INVALID;
	}
	*addr += 2;
	if (u >= 0xDC00 && u <= 0xDFFF) return INVALID;
	return u;
}

u32 EncodeUTF16Units(u32 c) { return c >= 0x10000 ? 2 : 1; }

u32 EncodeUTF16(u16 *out, u32 c) {
	if (c >= 0x10000) {
		c -= 0x10000;
		out[0] = (u16)(0xD800 + ((c >> 10) & 0x3FF));
		out[1] = (u16)(0xDC00 + (c & 0x3FF));
		return 2;
	}
	out[0] = (u16)c;
	return 1;
}

// Shift-JIS -> JIS code (JIS X 0201 single bytes are returned as is). Ported from PPSSPP's
// ShiftJIS::next.
u32 DecodeSJIS(u32 *addr) {
	GuestReader r{*addr};
	u32 j = r.Byte();
	if (j == 0) return 0;
	*addr += 1;
	int row;
	bool emojiAdjust = false;
	switch (j >> 4) {
	case 0x8:
		if (j == 0x80) return INVALID;
		[[fallthrough]];
	case 0x9:
	case 0xE:
		row = (int)((j & 0x3F) << 1) - 0x01;
		break;
	case 0xF:
		emojiAdjust = true;
		if (j < 0xF4) {
			row = (int)((j & 0x7F) << 1) - 0x59;
		} else if (j < 0xFD) {
			row = (int)((j & 0x7F) << 1) - 0x1B;
		} else {
			return j;
		}
		break;
	default:
		return j;
	}
	// A lead byte right before the terminator: don't consume the terminator.
	u32 t = r.Byte(1);
	if (t == 0) return INVALID;
	*addr += 1;
	if (t < 0x40 || t == 0x7F || t >= 0xFD) return INVALID;
	if (t >= 0x9F) {
		++row;
		t -= 0x7E;
	} else {
		if (t >= 0x80) t -= 0x20;
		else t -= 0x20 - 1;
		if (emojiAdjust) {
			if (row == 0x87) row = 0x81;
			else if (row == 0x8B) row = 0x85;
			else if (row == 0xCD) row = 0x8F;
		}
	}
	return ((u32)(row + 0x20) << 8) | t;
}

u32 EncodeSJISUnits(u32 j) { return (j & ~0xFFu) == 0 ? 1 : 2; }

// JIS code -> Shift-JIS bytes (0 when the row can't be encoded). Ported from PPSSPP's
// ShiftJIS::encode.
u32 EncodeSJIS(u8 *out, u32 j) {
	if ((j & ~0xFFu) == 0) {
		out[0] = (u8)j;
		return 1;
	}
	int row = (int)(j >> 8) - 0x20;
	u32 cell = j & 0xFF;
	if (row < 0x3F) out[0] = (u8)(0x80 + ((row + 1) >> 1));
	else if (row < 0x5F) out[0] = (u8)(0xE0 + ((row - 0x40 + 1) >> 1));
	else return 0;
	if (row & 1) out[1] = (u8)(cell < 0x60 ? cell + 0x20 - 1 : cell + 0x20);
	else out[1] = (u8)(cell + 0x7E);
	return 2;
}

// ---------------------------------------------------------------- tables

u32 UCStoJIS(u32 c, u32 alt) {
	alt &= 0xFFFF;
	if (!g_ucs2jisTable || c > 0xFFFF) return alt;
	u16 v = GRead16(g_ucs2jisTable + c * 2);
	return v ? v : alt;
}

u32 JIStoUCS(u32 c, u32 alt) {
	if (!g_jis2ucsTable) return alt;
	u16 v = GRead16(g_jis2ucsTable + (c & 0xFFFF) * 2);
	return v ? v : alt;
}

// ---------------------------------------------------------------- conversions

enum class Enc { UTF8, UTF16, SJIS };

u32 Decode(Enc e, u32 *addr) {
	switch (e) {
	case Enc::UTF8: return DecodeUTF8(addr);
	case Enc::UTF16: return DecodeUTF16(addr);
	default: return DecodeSJIS(addr);
	}
}

u16 ErrorChar(Enc e) {
	switch (e) {
	case Enc::UTF8: return g_errorUTF8;
	case Enc::UTF16: return g_errorUTF16;
	default: return g_errorSJIS;
	}
}

// Converts the NUL-terminated string at src into dst (dstSize bytes). Characters that can't be
// decoded or represented become the destination's error character; a character that is (or
// becomes) 0, or doesn't fit with room for the terminator, ends the conversion. The terminator is
// written when there's room for it. Returns the number of characters converted.
int Convert(Enc from, Enc to, u32 dstAddr, u32 dstSize, u32 srcAddr) {
	if (!dstAddr || !srcAddr || !mem::IsValidAddress(dstAddr) || !mem::IsValidAddress(srcAddr)) return 0;
	const bool sjis = from == Enc::SJIS || to == Enc::SJIS;
	if (sjis && !(from == Enc::SJIS ? g_jis2ucsTable : g_ucs2jisTable)) {
		LOGW(LOG, "SJIS conversion without sceCccSetTable");
		return 0;
	}
	const u32 unit = to == Enc::UTF16 ? 2 : 1;
	const u32 capacity = dstSize / unit;  // in units
	u32 used = 0;
	int n = 0;
	u32 src = srcAddr;
	while (true) {
		u32 c = Decode(from, &src);
		if (c == 0) break;
		if (c == INVALID) {
			c = ErrorChar(to);
		} else if (from == Enc::SJIS) {
			c = JIStoUCS(c, ErrorChar(to));
		} else if (to == Enc::SJIS) {
			c = UCStoJIS(c, ErrorChar(to));
		}
		if (c == 0) break;
		u32 units = to == Enc::UTF8 ? EncodeUTF8Units(c) : to == Enc::UTF16 ? EncodeUTF16Units(c) : EncodeSJISUnits(c);
		if (units == 0 || used + units >= capacity) break;
		u8 buf[4];
		u32 written;
		if (to == Enc::UTF8) written = EncodeUTF8(buf, c);
		else if (to == Enc::UTF16) written = EncodeUTF16((u16 *)buf, c);
		else written = EncodeSJIS(buf, c);
		if (written == 0) break;
		if (!mem::IsValidRange(dstAddr + used * unit, written * unit)) break;
		mem::WriteBytes(dstAddr + used * unit, buf, written * unit);
		used += written;
		n++;
	}
	if (used < capacity && mem::IsValidRange(dstAddr + used * unit, unit)) mem::Memset(dstAddr + used * unit, 0, unit);
	return n;
}

void sceCccSetTable(u32 jis2ucs, u32 ucs2jis) {
	g_jis2ucsTable = mem::IsValidRange(jis2ucs, 0x20000) ? jis2ucs : 0;
	g_ucs2jisTable = mem::IsValidRange(ucs2jis, 0x20000) ? ucs2jis : 0;
	LOGD(LOG, "sceCccSetTable(%08x, %08x)", jis2ucs, ucs2jis);
}

int sceCccUTF8toUTF16(u32 dst, u32 dstSize, u32 src) { return Convert(Enc::UTF8, Enc::UTF16, dst, dstSize, src); }
int sceCccUTF8toSJIS(u32 dst, u32 dstSize, u32 src) { return Convert(Enc::UTF8, Enc::SJIS, dst, dstSize, src); }
int sceCccUTF16toUTF8(u32 dst, u32 dstSize, u32 src) { return Convert(Enc::UTF16, Enc::UTF8, dst, dstSize, src); }
int sceCccUTF16toSJIS(u32 dst, u32 dstSize, u32 src) { return Convert(Enc::UTF16, Enc::SJIS, dst, dstSize, src); }
int sceCccSJIStoUTF8(u32 dst, u32 dstSize, u32 src) { return Convert(Enc::SJIS, Enc::UTF8, dst, dstSize, src); }
int sceCccSJIStoUTF16(u32 dst, u32 dstSize, u32 src) { return Convert(Enc::SJIS, Enc::UTF16, dst, dstSize, src); }

int Strlen(Enc e, u32 str) {
	if (!str || !mem::IsValidAddress(str)) return 0;
	int n = 0;
	u32 p = str;
	while (Decode(e, &p) != 0) n++;
	return n;
}
int sceCccStrlenUTF8(u32 str) { return Strlen(Enc::UTF8, str); }
int sceCccStrlenUTF16(u32 str) { return Strlen(Enc::UTF16, str); }
int sceCccStrlenSJIS(u32 str) { return Strlen(Enc::SJIS, str); }

// Encode one character at *dstPtr and advance it. UTF-8 / SJIS return the new pointer.
u32 sceCccEncodeUTF8(u32 dstPtrAddr, u32 ucs) {
	if (!mem::IsValidRange(dstPtrAddr, 4)) return 0;
	u32 dst = GRead32(dstPtrAddr);
	if (!mem::IsValidAddress(dst)) return 0;
	u8 buf[4];
	u32 n = EncodeUTF8(buf, ucs);
	if (n == 0 && g_errorUTF8) n = EncodeUTF8(buf, g_errorUTF8);
	if (!mem::IsValidRange(dst, n)) return 0;
	mem::WriteBytes(dst, buf, n);
	GWrite32(dstPtrAddr, dst + n);
	return dst + n;
}

void sceCccEncodeUTF16(u32 dstPtrAddr, u32 ucs) {
	if (!mem::IsValidRange(dstPtrAddr, 4)) return;
	u32 dst = GRead32(dstPtrAddr);
	if (!mem::IsValidAddress(dst)) return;
	// Past U+10FFFF and the surrogate range aren't encodable.
	if (ucs > 0x10FFFF || (ucs & 0xD800) == 0xD800) ucs = g_errorUTF16;
	u16 buf[2];
	u32 n = EncodeUTF16(buf, ucs);
	if (!mem::IsValidRange(dst, n * 2)) return;
	mem::WriteBytes(dst, buf, n * 2);
	GWrite32(dstPtrAddr, dst + n * 2);
}

u32 sceCccEncodeSJIS(u32 dstPtrAddr, u32 jis) {
	if (!mem::IsValidRange(dstPtrAddr, 4)) return 0;
	u32 dst = GRead32(dstPtrAddr);
	if (!mem::IsValidAddress(dst)) return 0;
	u8 buf[2];
	u32 n = EncodeSJIS(buf, jis);
	if (n == 0) n = EncodeSJIS(buf, g_errorSJIS);
	if (!mem::IsValidRange(dst, n)) return 0;
	mem::WriteBytes(dst, buf, n);
	GWrite32(dstPtrAddr, dst + n);
	return dst + n;
}

// Decode one character at *srcPtr and advance it.
u32 DecodeOne(Enc e, u32 srcPtrAddr) {
	if (!mem::IsValidRange(srcPtrAddr, 4)) return 0;
	u32 src = GRead32(srcPtrAddr);
	if (!mem::IsValidAddress(src)) return 0;
	u32 p = src;
	u32 c = Decode(e, &p);
	GWrite32(srcPtrAddr, p);
	return c == INVALID ? ErrorChar(e) : c;
}
u32 sceCccDecodeUTF8(u32 srcPtrAddr) { return DecodeOne(Enc::UTF8, srcPtrAddr); }
u32 sceCccDecodeUTF16(u32 srcPtrAddr) { return DecodeOne(Enc::UTF16, srcPtrAddr); }
u32 sceCccDecodeSJIS(u32 srcPtrAddr) { return DecodeOne(Enc::SJIS, srcPtrAddr); }

bool IsScalar(u32 c) { return c < 0x110000 && !(c >= 0xD800 && c <= 0xDFFF); }
int sceCccIsValidUTF8(u32 c) { return IsScalar(c) ? 1 : 0; }
int sceCccIsValidUTF16(u32 c) { return IsScalar(c) ? 1 : 0; }
int sceCccIsValidUCS2(u32 c) { return c < 0x10000 && IsScalar(c) ? 1 : 0; }
int sceCccIsValidUCS4(u32 c) { return c < 0x80000000 ? 1 : 0; }
int sceCccIsValidUnicode(u32 c) { return IsScalar(c) ? 1 : 0; }
// JIS X 0201 bytes, or a JIS X 0208 row/cell pair (0x21-0x7E each).
int sceCccIsValidJIS(u32 c) {
	if (c < 0x100) return 1;
	u32 hi = c >> 8, lo = c & 0xFF;
	return c <= 0xFFFF && hi >= 0x21 && hi <= 0x7E && lo >= 0x21 && lo <= 0x7E ? 1 : 0;
}
// A single-byte character, or a lead/trail byte pair.
int sceCccIsValidSJIS(u32 c) {
	if (c < 0x100) return (c < 0x80 || (c >= 0xA1 && c <= 0xDF)) ? 1 : 0;
	if (c > 0xFFFF) return 0;
	u32 lead = c >> 8, trail = c & 0xFF;
	bool leadOk = (lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC);
	bool trailOk = trail >= 0x40 && trail <= 0xFC && trail != 0x7F;
	return leadOk && trailOk ? 1 : 0;
}

u32 sceCccSetErrorCharUTF8(u32 c) {
	u32 old = g_errorUTF8;
	g_errorUTF8 = (u16)c;
	return old;
}
u32 sceCccSetErrorCharUTF16(u32 c) {
	u32 old = g_errorUTF16;
	g_errorUTF16 = (u16)c;
	return old;
}
u32 sceCccSetErrorCharSJIS(u32 c) {
	u32 old = g_errorSJIS;
	g_errorSJIS = (u16)c;
	return old;
}

u32 sceCccUCStoJIS(u32 c, u32 alt) { return UCStoJIS(c, alt); }
u32 sceCccJIStoUCS(u32 c, u32 alt) { return JIStoUCS(c, alt); }

}  // namespace

void Register_CccLibrary() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xB4D1CBBF, sceCccSetTable),
		HLEFN(0x00D1378F, sceCccUTF8toUTF16),
		HLEFN(0x6F82EE03, sceCccUTF8toSJIS),
		HLEFN(0x41B724A5, sceCccUTF16toUTF8),
		HLEFN(0xF1B73D12, sceCccUTF16toSJIS),
		HLEFN(0xA62E6E80, sceCccSJIStoUTF8),
		HLEFN(0xBEB47224, sceCccSJIStoUTF16),
		HLEFN(0xB7D3C112, sceCccStrlenUTF8),
		HLEFN(0x4BDEB2A8, sceCccStrlenUTF16),
		HLEFN(0xD9392CCB, sceCccStrlenSJIS),
		HLEFN(0x92C05851, sceCccEncodeUTF8),
		HLEFN(0x8406F469, sceCccEncodeUTF16),
		HLEFN(0x068C4320, sceCccEncodeSJIS),
		HLEFN(0xC6A8BEE2, sceCccDecodeUTF8),
		HLEFN(0xE0CF8091, sceCccDecodeUTF16),
		HLEFN(0x953E6C10, sceCccDecodeSJIS),
		HLEFN(0x90521AC5, sceCccIsValidUTF8),
		HLEFN(0xCC0A8BDA, sceCccIsValidUTF16),
		HLEFN(0x67BF0D19, sceCccIsValidSJIS),
		HLEFN(0x76E33E9C, sceCccIsValidUCS2),
		HLEFN(0xD2B18485, sceCccIsValidUCS4),
		HLEFN(0xA2D5D209, sceCccIsValidJIS),
		HLEFN(0xBD11EEF3, sceCccIsValidUnicode),
		HLEFN(0x17E1D813, sceCccSetErrorCharUTF8),
		HLEFN(0xB8476CF4, sceCccSetErrorCharUTF16),
		HLEFN(0xC56949AD, sceCccSetErrorCharSJIS),
		HLEFN(0x70ECAA10, sceCccUCStoJIS),
		HLEFN(0xFB7846E2, sceCccJIStoUCS),
	};
	hle::RegisterModule("sceCcc", funcs);
}
