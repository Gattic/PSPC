// SPDX-License-Identifier: GPL-3.0-or-later
// DEFLATE decoding per RFC 1951: canonical Huffman codes decoded a bit at a time from per-length
// symbol counts. Not fast, but PSP modules are small and this has no tables to get wrong.
#include "inflate.h"

namespace {

const int kMaxBits = 15;
const int kMaxLitLen = 288;
const int kMaxDist = 32;

struct Huffman {
	u16 count[kMaxBits + 1];
	u16 symbol[kMaxLitLen];
};

// Returns false if the lengths over-subscribe the code space. Incomplete codes are allowed (a
// single-distance-code stream is legal); decoding an unused code then fails cleanly.
bool BuildHuffman(Huffman *h, const u8 *lengths, int n) {
	memset(h->count, 0, sizeof(h->count));
	for (int i = 0; i < n; i++) {
		h->count[lengths[i]]++;
	}
	if (h->count[0] == n) {
		return true;
	}
	int left = 1;
	for (int len = 1; len <= kMaxBits; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0) {
			return false;
		}
	}
	u16 offsets[kMaxBits + 1];
	offsets[1] = 0;
	for (int len = 1; len < kMaxBits; len++) {
		offsets[len + 1] = offsets[len] + h->count[len];
	}
	for (int i = 0; i < n; i++) {
		if (lengths[i] != 0) {
			h->symbol[offsets[lengths[i]]++] = (u16)i;
		}
	}
	return true;
}

struct Inflater {
	const u8 *in;
	size_t size;
	size_t pos = 0;
	u32 bitBuf = 0;
	int bitCount = 0;
	bool overrun = false;
	Bytes *out;
	size_t maxOut;
	std::string error;

	u32 Bits(int need) {
		u32 val = bitBuf;
		while (bitCount < need) {
			if (pos >= size) {
				overrun = true;
				return 0;
			}
			val |= (u32)in[pos++] << bitCount;
			bitCount += 8;
		}
		bitBuf = val >> need;
		bitCount -= need;
		return val & ((1u << need) - 1);
	}

	int Decode(const Huffman &h) {
		int code = 0, first = 0, index = 0;
		for (int len = 1; len <= kMaxBits; len++) {
			code |= (int)Bits(1);
			if (overrun) {
				return -1;
			}
			const int count = h.count[len];
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

	bool Stored() {
		bitBuf = 0;
		bitCount = 0;
		if (pos + 4 > size) {
			error = "truncated stored block";
			return false;
		}
		const u32 len = rd16(in + pos);
		const u32 nlen = rd16(in + pos + 2);
		pos += 4;
		if ((len ^ 0xFFFF) != nlen) {
			error = "stored block length check failed";
			return false;
		}
		if (pos + len > size) {
			error = "truncated stored block";
			return false;
		}
		if (out->size() + len > maxOut) {
			error = "output too large";
			return false;
		}
		out->insert(out->end(), in + pos, in + pos + len);
		pos += len;
		return true;
	}

	bool Codes(const Huffman &lencode, const Huffman &distcode) {
		static const u16 lenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
			35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		static const u8 lenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
			3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		static const u16 distBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
			257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		static const u8 distExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
			7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
		while (true) {
			int sym = Decode(lencode);
			if (sym < 0) {
				error = overrun ? "truncated deflate stream" : "bad literal/length code";
				return false;
			}
			if (sym < 256) {
				if (out->size() >= maxOut) {
					error = "output too large";
					return false;
				}
				out->push_back((u8)sym);
				continue;
			}
			if (sym == 256) {
				return true;
			}
			sym -= 257;
			if (sym >= 29) {
				error = "bad length symbol";
				return false;
			}
			const u32 len = lenBase[sym] + Bits(lenExtra[sym]);
			const int dsym = Decode(distcode);
			if (dsym < 0 || dsym >= 30) {
				error = overrun ? "truncated deflate stream" : "bad distance code";
				return false;
			}
			const u32 dist = distBase[dsym] + Bits(distExtra[dsym]);
			if (overrun) {
				error = "truncated deflate stream";
				return false;
			}
			if (dist > out->size()) {
				error = "distance too far back";
				return false;
			}
			if (out->size() + len > maxOut) {
				error = "output too large";
				return false;
			}
			// Byte at a time: overlapping copies are how runs are encoded.
			const size_t from = out->size() - dist;
			out->reserve(out->size() + len);
			for (u32 i = 0; i < len; i++) {
				const u8 b = (*out)[from + i];
				out->push_back(b);
			}
		}
	}

	bool Fixed() {
		static Huffman lencode, distcode;
		static bool built = false;
		if (!built) {
			u8 lengths[kMaxLitLen];
			int i = 0;
			for (; i < 144; i++) lengths[i] = 8;
			for (; i < 256; i++) lengths[i] = 9;
			for (; i < 280; i++) lengths[i] = 7;
			for (; i < 288; i++) lengths[i] = 8;
			BuildHuffman(&lencode, lengths, 288);
			for (i = 0; i < 30; i++) lengths[i] = 5;
			BuildHuffman(&distcode, lengths, 30);
			built = true;
		}
		return Codes(lencode, distcode);
	}

	bool Dynamic() {
		static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
		const int nlen = (int)Bits(5) + 257;
		const int ndist = (int)Bits(5) + 1;
		const int ncode = (int)Bits(4) + 4;
		if (overrun || nlen > 286 || ndist > 30) {
			error = "bad dynamic block header";
			return false;
		}
		u8 lengths[kMaxLitLen + kMaxDist] = {};
		for (int i = 0; i < ncode; i++) {
			lengths[order[i]] = (u8)Bits(3);
		}
		Huffman lencode, distcode;
		if (!BuildHuffman(&lencode, lengths, 19)) {
			error = "bad code length code";
			return false;
		}
		int index = 0;
		while (index < nlen + ndist) {
			int sym = Decode(lencode);
			if (sym < 0) {
				error = "bad code lengths";
				return false;
			}
			if (sym < 16) {
				lengths[index++] = (u8)sym;
				continue;
			}
			u8 value = 0;
			int repeat;
			if (sym == 16) {
				if (index == 0) {
					error = "repeat with no previous length";
					return false;
				}
				value = lengths[index - 1];
				repeat = 3 + (int)Bits(2);
			} else if (sym == 17) {
				repeat = 3 + (int)Bits(3);
			} else {
				repeat = 11 + (int)Bits(7);
			}
			if (index + repeat > nlen + ndist) {
				error = "too many code lengths";
				return false;
			}
			while (repeat--) {
				lengths[index++] = value;
			}
		}
		if (overrun) {
			error = "truncated deflate stream";
			return false;
		}
		if (lengths[256] == 0) {
			error = "no end-of-block code";
			return false;
		}
		if (!BuildHuffman(&lencode, lengths, nlen) || !BuildHuffman(&distcode, lengths + nlen, ndist)) {
			error = "over-subscribed Huffman code";
			return false;
		}
		return Codes(lencode, distcode);
	}

	bool Run() {
		bool last;
		do {
			last = Bits(1) != 0;
			const u32 type = Bits(2);
			if (overrun) {
				error = "truncated deflate stream";
				return false;
			}
			bool ok;
			if (type == 0) {
				ok = Stored();
			} else if (type == 1) {
				ok = Fixed();
			} else if (type == 2) {
				ok = Dynamic();
			} else {
				error = "invalid block type";
				ok = false;
			}
			if (!ok) {
				return false;
			}
		} while (!last);
		// Whatever is left of the last byte is padding.
		bitBuf = 0;
		bitCount = 0;
		return true;
	}
};

}  // namespace

u32 Crc32(const u8 *data, size_t size, u32 crc) {
	static u32 table[256];
	static bool built = false;
	if (!built) {
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
		built = true;
	}
	crc = ~crc;
	for (size_t i = 0; i < size; i++) {
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	}
	return ~crc;
}

u32 Adler32(const u8 *data, size_t size, u32 adler) {
	u32 a = adler & 0xFFFF, b = adler >> 16;
	for (size_t i = 0; i < size; i++) {
		a = (a + data[i]) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}

bool IsZlibHeader(const u8 *p, size_t size) {
	if (size < 2) {
		return false;
	}
	const u8 cmf = p[0], flg = p[1];
	return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20);
}

bool InflateStream(const u8 *in, size_t inSize, DeflateWrapper wrapper, Bytes *out, size_t *consumed, std::string *error, size_t maxOut) {
	out->clear();
	size_t start = 0;
	if (wrapper == DeflateWrapper::Gzip) {
		if (inSize < 18 || !IsGzipMagic(in, inSize)) {
			*error = "not a gzip stream";
			return false;
		}
		const u8 flags = in[3];
		size_t p = 10;
		if (flags & 4) {  // FEXTRA
			if (p + 2 > inSize) {
				*error = "truncated gzip header";
				return false;
			}
			p += 2 + rd16(in + p);
		}
		for (int field = 0; field < 2; field++) {  // FNAME, FCOMMENT
			if (flags & (field == 0 ? 8 : 16)) {
				while (p < inSize && in[p] != 0) {
					p++;
				}
				p++;
			}
		}
		if (flags & 2) {  // FHCRC
			p += 2;
		}
		if (p >= inSize) {
			*error = "truncated gzip header";
			return false;
		}
		start = p;
	} else if (wrapper == DeflateWrapper::Zlib) {
		if (!IsZlibHeader(in, inSize)) {
			*error = "not a zlib stream";
			return false;
		}
		start = 2;
	}

	if (wrapper == DeflateWrapper::Gzip) {
		// ISIZE is a good guess at the output size; growing a vector byte by byte is slow.
		const u32 isize = rd32(in + inSize - 4);
		if (isize <= maxOut && isize <= inSize * 1032) {
			out->reserve(isize);
		}
	}

	Inflater inf;
	inf.in = in + start;
	inf.size = inSize - start;
	inf.out = out;
	inf.maxOut = maxOut;
	if (!inf.Run()) {
		*error = inf.error;
		return false;
	}
	size_t end = start + inf.pos;

	if (wrapper == DeflateWrapper::Gzip) {
		if (end + 8 > inSize) {
			*error = "truncated gzip trailer";
			return false;
		}
		if (rd32(in + end) != Crc32(out->data(), out->size())) {
			*error = "gzip CRC mismatch";
			return false;
		}
		if (rd32(in + end + 4) != (u32)out->size()) {
			*error = "gzip size mismatch";
			return false;
		}
		end += 8;
	} else if (wrapper == DeflateWrapper::Zlib) {
		if (end + 4 > inSize) {
			*error = "truncated zlib trailer";
			return false;
		}
		if (rd32be(in + end) != Adler32(out->data(), out->size())) {
			*error = "zlib Adler-32 mismatch";
			return false;
		}
		end += 4;
	}
	if (consumed) {
		*consumed = end;
	}
	return true;
}
