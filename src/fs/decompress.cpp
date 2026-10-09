// SPDX-License-Identifier: GPL-2.0-or-later
// DEFLATE decoder: canonical Huffman decoding with a 10-bit lookup table and a bit-serial fallback
// for longer codes (the fallback follows the approach of Mark Adler's puff.c).
#include "fs/decompress.h"

#include <cstring>

namespace vfs {

namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10;
constexpr int kMaxLitCodes = 288;
constexpr int kMaxDistCodes = 30;

struct BitReader {
	const u8 *p;
	const u8 *end;
	u64 buf = 0;
	int cnt = 0;
	int pad = 0;  // zero bits appended past the end of the input

	BitReader(const u8 *in, size_t len) : p(in), end(in + len) {}

	void Refill() {
		while (cnt <= 56) {
			u64 b = 0;
			if (p < end) {
				b = *p++;
			} else {
				pad += 8;
			}
			buf |= b << cnt;
			cnt += 8;
		}
	}
	u32 Peek(int n) {
		if (cnt < n) {
			Refill();
		}
		return (u32)(buf & ((1ull << n) - 1));
	}
	void Drop(int n) {
		buf >>= n;
		cnt -= n;
	}
	u32 Bits(int n) {
		if (n == 0) {
			return 0;
		}
		u32 v = Peek(n);
		Drop(n);
		return v;
	}
	// True once a bit past the real input has been consumed.
	bool Overrun() const { return cnt < pad; }
	// Discards the partial byte and hands the buffered whole bytes back to the input pointer.
	bool AlignToByte() {
		Drop(cnt & 7);
		int real = cnt - pad;
		if (real < 0) {
			return false;
		}
		p -= real / 8;
		buf = 0;
		cnt = 0;
		pad = 0;
		return true;
	}
	size_t BytesConsumed(const u8 *start) const {
		int real = cnt - pad;
		if (real < 0) {
			real = 0;
		}
		return (size_t)(p - start) - (size_t)(real / 8);
	}
};

struct Huffman {
	u16 count[kMaxBits + 1];
	u16 symbol[kMaxLitCodes];
	// (length << 9) | symbol for codes of up to kFastBits bits, indexed by the next input bits.
	u16 fast[1 << kFastBits];
};

// Returns 0 for a complete code, > 0 if incomplete, < 0 if over-subscribed.
int BuildHuffman(Huffman &h, const u8 *lengths, int n) {
	memset(h.count, 0, sizeof(h.count));
	for (int i = 0; i < n; i++) {
		h.count[lengths[i]]++;
	}
	h.count[0] = 0;
	int left = 1;
	for (int len = 1; len <= kMaxBits; len++) {
		left <<= 1;
		left -= h.count[len];
		if (left < 0) {
			return left;
		}
	}
	u16 offs[kMaxBits + 2];
	offs[1] = 0;
	for (int len = 1; len <= kMaxBits; len++) {
		offs[len + 1] = (u16)(offs[len] + h.count[len]);
	}
	for (int sym = 0; sym < n; sym++) {
		if (lengths[sym]) {
			h.symbol[offs[lengths[sym]]++] = (u16)sym;
		}
	}
	memset(h.fast, 0, sizeof(h.fast));
	u32 nextCode[kMaxBits + 1];
	u32 code = 0;
	nextCode[0] = 0;
	for (int len = 1; len <= kMaxBits; len++) {
		code = (code + h.count[len - 1]) << 1;
		nextCode[len] = code;
	}
	for (int sym = 0; sym < n; sym++) {
		int len = lengths[sym];
		if (len == 0) {
			continue;
		}
		u32 c = nextCode[len]++;
		if (len > kFastBits) {
			continue;
		}
		// Codes are stored MSB-first in the stream, which is read LSB-first.
		u32 rev = 0;
		for (int b = 0; b < len; b++) {
			rev |= ((c >> b) & 1) << (len - 1 - b);
		}
		for (u32 i = rev; i < (1u << kFastBits); i += 1u << len) {
			h.fast[i] = (u16)((len << 9) | sym);
		}
	}
	return left;
}

int Decode(BitReader &br, const Huffman &h) {
	u32 e = h.fast[br.Peek(kFastBits)];
	if (e) {
		br.Drop((int)(e >> 9));
		return (int)(e & 511);
	}
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= kMaxBits; len++) {
		code |= (int)br.Bits(1);
		int count = h.count[len];
		if (code - count < first) {
			return h.symbol[index + (code - first)];
		}
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

const u16 kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const u16 kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct FixedTables {
	Huffman lit, dist;
	FixedTables() {
		u8 lengths[kMaxLitCodes];
		int i = 0;
		for (; i < 144; i++) lengths[i] = 8;
		for (; i < 256; i++) lengths[i] = 9;
		for (; i < 280; i++) lengths[i] = 7;
		for (; i < 288; i++) lengths[i] = 8;
		BuildHuffman(lit, lengths, kMaxLitCodes);
		for (i = 0; i < kMaxDistCodes; i++) lengths[i] = 5;
		BuildHuffman(dist, lengths, kMaxDistCodes);
	}
};

const FixedTables &Fixed() {
	static const FixedTables tables;
	return tables;
}

bool InflateCodes(BitReader &br, const Huffman &lit, const Huffman &dist, u8 *out, size_t outCap, size_t &o) {
	for (;;) {
		int sym = Decode(br, lit);
		if (sym < 0 || br.Overrun()) {
			return false;
		}
		if (sym < 256) {
			if (o >= outCap) {
				return false;
			}
			out[o++] = (u8)sym;
			continue;
		}
		if (sym == 256) {
			return true;
		}
		sym -= 257;
		if (sym >= 29) {
			return false;
		}
		size_t len = kLenBase[sym] + br.Bits(kLenExtra[sym]);
		int dsym = Decode(br, dist);
		if (dsym < 0 || dsym >= kMaxDistCodes) {
			return false;
		}
		size_t d = kDistBase[dsym] + br.Bits(kDistExtra[dsym]);
		if (br.Overrun() || d > o || len > outCap - o) {
			return false;
		}
		u8 *dst = out + o;
		const u8 *src = dst - d;
		if (d >= len) {
			memcpy(dst, src, len);
		} else {
			for (size_t k = 0; k < len; k++) {
				dst[k] = src[k];
			}
		}
		o += len;
	}
}

bool InflateDynamic(BitReader &br, u8 *out, size_t outCap, size_t &o) {
	static const u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	int nlen = (int)br.Bits(5) + 257;
	int ndist = (int)br.Bits(5) + 1;
	int ncode = (int)br.Bits(4) + 4;
	if (nlen > 286 || ndist > kMaxDistCodes) {
		return false;
	}
	u8 lengths[kMaxLitCodes + kMaxDistCodes];
	memset(lengths, 0, sizeof(lengths));
	for (int i = 0; i < ncode; i++) {
		lengths[kOrder[i]] = (u8)br.Bits(3);
	}
	Huffman lencode, distcode;
	if (BuildHuffman(lencode, lengths, 19) != 0) {
		return false;  // the code length code must be complete
	}
	int index = 0;
	while (index < nlen + ndist) {
		int sym = Decode(br, lencode);
		if (sym < 0 || br.Overrun()) {
			return false;
		}
		if (sym < 16) {
			lengths[index++] = (u8)sym;
			continue;
		}
		u8 len = 0;
		int repeat;
		if (sym == 16) {
			if (index == 0) {
				return false;
			}
			len = lengths[index - 1];
			repeat = 3 + (int)br.Bits(2);
		} else if (sym == 17) {
			repeat = 3 + (int)br.Bits(3);
		} else {
			repeat = 11 + (int)br.Bits(7);
		}
		if (index + repeat > nlen + ndist) {
			return false;
		}
		while (repeat--) {
			lengths[index++] = len;
		}
	}
	if (lengths[256] == 0) {
		return false;
	}
	// Incomplete codes are tolerated: an unused code simply fails to decode.
	if (BuildHuffman(lencode, lengths, nlen) < 0) {
		return false;
	}
	if (BuildHuffman(distcode, lengths + nlen, ndist) < 0) {
		return false;
	}
	return InflateCodes(br, lencode, distcode, out, outCap, o);
}

}  // namespace

s64 InflateRaw(const u8 *in, size_t inLen, u8 *out, size_t outCap, size_t *consumed) {
	BitReader br(in, inLen);
	size_t o = 0;
	bool last;
	do {
		last = br.Bits(1) != 0;
		u32 type = br.Bits(2);
		if (br.Overrun()) {
			return -1;
		}
		if (type == 0) {
			if (!br.AlignToByte() || br.end - br.p < 4) {
				return -1;
			}
			u32 len = br.p[0] | (br.p[1] << 8);
			u32 nlen = br.p[2] | (br.p[3] << 8);
			br.p += 4;
			if ((len ^ 0xFFFF) != nlen || (size_t)(br.end - br.p) < len || len > outCap - o) {
				return -1;
			}
			memcpy(out + o, br.p, len);
			br.p += len;
			o += len;
		} else if (type == 1) {
			if (!InflateCodes(br, Fixed().lit, Fixed().dist, out, outCap, o)) {
				return -1;
			}
		} else if (type == 2) {
			if (!InflateDynamic(br, out, outCap, o)) {
				return -1;
			}
		} else {
			return -1;
		}
	} while (!last);
	if (consumed) {
		*consumed = br.BytesConsumed(in);
	}
	return (s64)o;
}

bool IsZlibHeader(const u8 *in, size_t inLen) {
	if (inLen < 2) {
		return false;
	}
	u32 cmf = in[0], flg = in[1];
	return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20);
}

s64 InflateZlib(const u8 *in, size_t inLen, u8 *out, size_t outCap) {
	if (!IsZlibHeader(in, inLen)) {
		return -1;
	}
	size_t used = 0;
	s64 n = InflateRaw(in + 2, inLen - 2, out, outCap, &used);
	if (n < 0) {
		return -1;
	}
	size_t tail = 2 + used;
	if (inLen - tail >= 4) {
		u32 a = 1, b = 0;
		for (s64 i = 0; i < n; i++) {
			a = (a + out[i]) % 65521;
			b = (b + a) % 65521;
		}
		const u8 *t = in + tail;
		u32 want = ((u32)t[0] << 24) | ((u32)t[1] << 16) | ((u32)t[2] << 8) | t[3];
		if (((b << 16) | a) != want) {
			return -1;
		}
	}
	return n;
}

s64 Lz4DecompressBlock(const u8 *in, size_t inLen, u8 *out, size_t outLen) {
	size_t ip = 0, op = 0;
	while (ip < inLen && op < outLen) {
		u32 token = in[ip++];
		size_t lit = token >> 4;
		if (lit == 15) {
			u8 b;
			do {
				if (ip >= inLen) {
					return -1;
				}
				b = in[ip++];
				lit += b;
			} while (b == 255);
		}
		if (lit > inLen - ip || lit > outLen - op) {
			return -1;
		}
		memcpy(out + op, in + ip, lit);
		ip += lit;
		op += lit;
		if (op == outLen || ip >= inLen) {
			break;  // the last sequence carries literals only
		}
		if (inLen - ip < 2) {
			return -1;
		}
		size_t off = in[ip] | (in[ip + 1] << 8);
		ip += 2;
		if (off == 0 || off > op) {
			return -1;
		}
		size_t mlen = token & 15;
		if (mlen == 15) {
			u8 b;
			do {
				if (ip >= inLen) {
					return -1;
				}
				b = in[ip++];
				mlen += b;
			} while (b == 255);
		}
		mlen += 4;
		if (mlen > outLen - op) {
			return -1;
		}
		u8 *dst = out + op;
		const u8 *src = dst - off;
		if (off >= mlen) {
			memcpy(dst, src, mlen);
		} else {
			for (size_t k = 0; k < mlen; k++) {
				dst[k] = src[k];
			}
		}
		op += mlen;
	}
	return (s64)op;
}

}  // namespace vfs
