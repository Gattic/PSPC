// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"

#include <cstdlib>
#include <filesystem>

#include "des.h"
#include "inflate.h"
#include "prx_decrypter.h"

#include "../libkirk/kirk_engine.h"
#include "../libkirk/SHA1.h"

TestEnv g_env;
int g_checks = 0;
int g_failures = 0;

std::vector<std::pair<const char *, TestFn>> &TestList() {
	static std::vector<std::pair<const char *, TestFn>> list;
	return list;
}

Bytes HexBytes(const char *hex) {
	Bytes out;
	auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
	for (const char *p = hex; p[0] && p[1];) {
		if (*p == ' ') {
			p++;
			continue;
		}
		out.push_back((u8)((nibble(p[0]) << 4) | nibble(p[1])));
		p += 2;
	}
	return out;
}

std::string TempPath(const std::string &name) {
	return JoinPath(g_env.tmp, name);
}

Bytes PseudoRandom(size_t size, u32 seed) {
	Bytes out(size);
	u32 x = seed * 2654435761u + 0x9E3779B9u;
	for (size_t i = 0; i < size; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		out[i] = (u8)(x >> 24);
	}
	return out;
}

bool FileExists(const std::string &path) {
	std::error_code ec;
	return std::filesystem::is_regular_file(U8Path(path), ec);
}

Bytes ReadAll(const std::string &path) {
	Bytes data;
	std::string error;
	ReadFileBytes(path, &data, &error);
	return data;
}

static void DeflateStoredBlocks(const Bytes &data, Bytes *out) {
	size_t pos = 0;
	do {
		const size_t n = std::min<size_t>(data.size() - pos, 0xFFFF);
		const bool last = pos + n == data.size();
		out->push_back(last ? 1 : 0);
		out->push_back((u8)n);
		out->push_back((u8)(n >> 8));
		out->push_back((u8)~n);
		out->push_back((u8)(~n >> 8));
		out->insert(out->end(), data.begin() + pos, data.begin() + pos + n);
		pos += n;
	} while (pos < data.size());
}

Bytes GzipStore(const Bytes &data) {
	Bytes out = { 0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03 };
	DeflateStoredBlocks(data, &out);
	u8 trailer[8];
	wr32(trailer, Crc32(data.data(), data.size()));
	wr32(trailer + 4, (u32)data.size());
	out.insert(out.end(), trailer, trailer + 8);
	return out;
}

Bytes ZlibStore(const Bytes &data) {
	Bytes out = { 0x78, 0x01 };
	DeflateStoredBlocks(data, &out);
	const u32 adler = Adler32(data.data(), data.size());
	out.push_back((u8)(adler >> 24));
	out.push_back((u8)(adler >> 16));
	out.push_back((u8)(adler >> 8));
	out.push_back((u8)adler);
	return out;
}

Bytes MakeTestElf(const std::string &name, size_t size, u32 seed, u16 attribute) {
	if (size < 0x200) {
		size = 0x200;
	}
	Bytes e = PseudoRandom(size, seed);
	// Make it compress a little, like real code: zero every other 16 bytes.
	for (size_t i = 0x100; i + 16 <= size; i += 32) {
		memset(&e[i], 0, 16);
	}
	memset(e.data(), 0, 0x100);
	const u8 ident[16] = { 0x7F, 'E', 'L', 'F', 1, 1, 1, 0 };
	memcpy(e.data(), ident, 16);
	wr16(&e[0x10], 0xFFA0);
	wr16(&e[0x12], 8);
	wr32(&e[0x14], 1);
	wr32(&e[0x18], 0x100);
	wr32(&e[0x1C], 0x34);
	wr32(&e[0x24], 0x10A23000);
	wr16(&e[0x28], 0x34);
	wr16(&e[0x2A], 0x20);
	wr16(&e[0x2C], 1);
	wr16(&e[0x2E], 0x28);
	// One loadable segment; p_paddr holds the module info's file offset, as in a real PRX.
	wr32(&e[0x34], 1);
	wr32(&e[0x38], 0x60);
	wr32(&e[0x40], 0xA0);
	wr32(&e[0x44], (u32)size - 0x60);
	wr32(&e[0x48], (u32)size - 0x60 + 0x100);
	wr32(&e[0x4C], 5);
	wr32(&e[0x50], 0x10);
	wr16(&e[0xA0], attribute);
	e[0xA2] = 1;
	e[0xA3] = 1;
	memcpy(&e[0xA4], name.c_str(), std::min<size_t>(name.size(), 27));
	wr32(&e[0xC0], 0x8000);
	return e;
}

namespace {

// The encoder side of the range coder KL4E and LZRC decode with: normalize before each bit,
// bound = (range >> 8) * p, a 1 takes the low part.
struct RangeEncoder {
	u64 low = 0;
	u32 range = 0xFFFFFFFF;
	u8 cache = 0;
	u64 cacheSize = 1;
	Bytes out;

	void ShiftLow() {
		if ((u32)low < 0xFF000000u || (low >> 32) != 0) {
			const u8 carry = (u8)(low >> 32);
			u8 temp = cache;
			do {
				out.push_back((u8)(temp + carry));
				temp = 0xFF;
			} while (--cacheSize != 0);
			cache = (u8)(low >> 24);
		}
		cacheSize++;
		low = (low & 0x00FFFFFF) << 8;
	}
	void Normalize() {
		if (range < (1u << 24)) {
			range <<= 8;
			ShiftLow();
		}
	}
	void Bit(u8 *prob, int bit, int decay, int bonus) {
		Normalize();
		const u32 bound = (range >> 8) * *prob;
		const u8 decayed = (u8)(*prob - (*prob >> decay));
		if (bit) {
			range = bound;
			*prob = (u8)(decayed + bonus);
		} else {
			low += bound;
			range -= bound;
			*prob = decayed;
		}
	}
	void Uniform(int bit, bool normalize) {
		if (normalize) {
			Normalize();
		}
		range >>= 1;
		if (!bit) {
			low += range;
		}
	}
	// The decoders start from a 4-byte code word after their own header byte; the encoder's
	// first output byte is always 0 and is where that header byte goes.
	Bytes Finish(u8 headerByte) {
		for (int i = 0; i < 5; i++) {
			ShiftLow();
		}
		out[0] = headerByte;
		return out;
	}
};

}  // namespace

Bytes Kl4eEncodeLiterals(const Bytes &data, u8 flags, bool kl3e, bool endMarker) {
	flags &= 0x7F;
	const u8 seed = (u8)(0x80 - (((flags >> 3) & 3) << 4));
	const int shift = flags & 7;
	u8 litProbs[2040], countBits[64], countProbs[256];
	memset(litProbs, seed, sizeof(litProbs));
	memset(countBits, seed, sizeof(countBits));
	memset(countProbs, seed, sizeof(countProbs));

	RangeEncoder rc;
	auto literal = [&](int pos, u8 prev, u8 value) {
		const u32 ctx = ((u32)(pos & 7) << 8) | prev;
		u8 *probs = litProbs + ((ctx >> shift) & 7) * 255 - 1;
		u32 acc = 1;
		for (int i = 7; i >= 0; i--) {
			const int bit = (value >> i) & 1;
			rc.Bit(&probs[acc], bit, 3, 31);
			acc = (acc << 1) | bit;
		}
	};
	int idx = 0;
	literal(0, 0, data[0]);
	for (size_t pos = 1; pos < data.size(); pos++) {
		rc.Bit(&countBits[idx], 0, 4, 15);
		idx = idx > 0 ? idx - 1 : 0;
		literal((int)pos, data[pos - 1], data[pos]);
	}
	if (!endMarker) {
		Bytes stream = rc.Finish(flags);
		Bytes out = { 'K', 'L', (u8)(kl3e ? '3' : '4'), 'E' };
		out.insert(out.end(), stream.begin(), stream.end());
		return out;
	}
	// End marker: a match whose length code is 0xFF - seven length bits, all ones.
	rc.Bit(&countBits[idx], 1, 4, 15);
	for (int i = 0; i < 7; i++) {
		idx += 8;
		rc.Bit(&countBits[idx], 1, 4, 15);
	}
	u8 *probs = countProbs + ((6 << 5) | (idx & 7));
	rc.Bit(probs + 24, 1, 3, 31);
	rc.Bit(probs + 24, 1, 3, 31);
	rc.Uniform(1, true);
	rc.Uniform(1, false);
	rc.Bit(probs, 1, 3, 31);
	rc.Bit(probs + 8, 1, 3, 31);
	rc.Bit(probs + 16, 1, 3, 31);
	Bytes stream = rc.Finish(flags);
	Bytes out = { 'K', 'L', (u8)(kl3e ? '3' : '4'), 'E' };
	out.insert(out.end(), stream.begin(), stream.end());
	return out;
}

Bytes LzrcEncodeLiterals(const Bytes &data, u8 lc) {
	u8 literal[8][256], match[8][8], len[8][31];
	memset(literal, 0x80, sizeof(literal));
	memset(match, 0x80, sizeof(match));
	memset(len, 0x80, sizeof(len));
	RangeEncoder rc;
	int state = 0;
	u8 last = 0;
	for (size_t pos = 0; pos < data.size(); pos++) {
		rc.Bit(&match[state][0], 0, 3, 31);
		if (state > 0) {
			state--;
		}
		const int ctx = (int)(((((pos & 7) << 8) | last) >> lc) & 7);
		int number = 1;
		for (int i = 7; i >= 0; i--) {
			const int bit = (data[pos] >> i) & 1;
			rc.Bit(&literal[ctx][number], bit, 3, 31);
			number = (number << 1) | bit;
		}
		last = data[pos];
	}
	// End marker: seven length bits, then a length number of 0xFF.
	rc.Bit(&match[state][0], 1, 3, 31);
	for (int i = 1; i <= 7; i++) {
		rc.Bit(&match[state][i], 1, 3, 31);
	}
	u8 *probs = &len[state][24];
	rc.Bit(probs + 3, 1, 3, 31);
	rc.Bit(probs + 3, 1, 3, 31);
	rc.Normalize();
	rc.Uniform(1, false);
	rc.Uniform(1, false);
	rc.Bit(probs, 1, 3, 31);
	rc.Bit(probs + 1, 1, 3, 31);
	rc.Bit(probs + 2, 1, 3, 31);
	Bytes stream = rc.Finish(lc);
	Bytes out = { '2', 'R', 'L', 'Z' };
	out.insert(out.end(), stream.begin(), stream.end());
	return out;
}

Bytes KirkCmd1Block(const Bytes &area, const Bytes &plain, u32 keySeed) {
	const size_t n = plain.size();
	const size_t padded = (n + 15) & ~(size_t)15;
	Bytes in(0x90 + area.size() + padded, 0);
	const Bytes aes = PseudoRandom(16, keySeed), cmac = PseudoRandom(16, keySeed + 1);
	memcpy(&in[0], aes.data(), 16);
	memcpy(&in[16], cmac.data(), 16);
	wr32(&in[0x60], 1);
	wr32(&in[0x70], (u32)n);
	wr32(&in[0x74], (u32)area.size());
	memcpy(&in[0x90], area.data(), area.size());
	memcpy(&in[0x90 + area.size()], plain.data(), n);
	Bytes out(in.size(), 0);
	KirkState kirk{};
	kirk_init(&kirk);
	kirk_CMD0(&kirk, out.data(), in.data(), (int)in.size(), 0);
	return out;
}

static void Sha1(const std::vector<std::pair<const u8 *, size_t>> &parts, u8 out[20]) {
	SHA_CTX ctx;
	SHAInit(&ctx);
	for (const auto &[p, n] : parts) {
		SHAUpdate(&ctx, p, (int)n);
	}
	SHAFinal(out, &ctx);
}

Bytes EncryptPrxType0(u32 tag, const u8 prxHeader[0x80], const Bytes &payload, u32 keySeed) {
	u8 x[0x90], code = 0;
	if (!PrxDebugTagKey(tag, x, &code)) {
		return Bytes();
	}
	const size_t padded = (payload.size() + 15) & ~(size_t)15;
	const Bytes k = KirkCmd1Block(Bytes(prxHeader, prxHeader + 0x80), payload, keySeed);
	Bytes prx(0x150 + padded, 0);
	memcpy(&prx[0], prxHeader, 0x80);
	memcpy(&prx[0x150], &k[0x110], padded);
	// decryptKirkHeaderType0 computes kirk7(kb ^ X[0x14..]) ^ X[0x20..]; invert it.
	u8 kb[0x90], t[0x70];
	for (int i = 0; i < 0x70; i++) {
		t[i] = k[i] ^ x[i + 0x20];
	}
	kirk4(t, t, 0x70, code);
	for (int i = 0; i < 0x70; i++) {
		kb[i] = t[i] ^ x[i + 0x14];
	}
	memcpy(kb + 0x70, &k[0x70], 0x20);
	memcpy(&prx[0x110], kb, 0x40);
	memcpy(&prx[0x80], kb + 0x40, 0x50);
	wr32(&prx[0xD0], tag);
	u8 sha[20];
	Sha1({ { x, 0x14 }, { &prx[0xE8], 0x28 }, { kb, 0x90 }, { prxHeader, 0x80 } }, sha);
	memcpy(&prx[0xD4], sha, 20);
	return prx;
}

Bytes EncryptPrxType2(u32 tag, const u8 prxHeader[0x80], const Bytes &payload, u32 keySeed) {
	u8 key[16], code = 0;
	if (!PrxDebugTagKey2(tag, key, &code)) {
		return Bytes();
	}
	u8 x[0x90];
	for (int i = 0; i < 9; i++) {
		memcpy(x + i * 16, key, 16);
		x[i * 16] = (u8)i;
	}
	kirk7(x, x, 0x90, code);

	const size_t padded = (payload.size() + 15) & ~(size_t)15;
	const Bytes k = KirkCmd1Block(Bytes(prxHeader, prxHeader + 0x80), payload, keySeed);
	const u8 *metadata = &k[0x70];

	// decryptKirkHeader computes kirk7(kh ^ X[0x10..]) ^ X[0x50..]; invert it.
	u8 kh[0x40];
	for (int i = 0; i < 0x40; i++) {
		kh[i] = k[i] ^ x[0x50 + i];
	}
	kirk4(kh, kh, 0x40, code);
	for (int i = 0; i < 0x40; i++) {
		kh[i] ^= x[0x10 + i];
	}

	const Bytes id = PseudoRandom(16, keySeed + 7);
	u8 tagBytes[4], zeros[0x58] = {};
	wr32(tagBytes, tag);
	u8 sha[20];
	Sha1({ { tagBytes, 4 }, { x, 0x10 }, { zeros, 0x58 }, { id.data(), 0x10 }, { kh, 0x40 }, { metadata, 0x10 }, { prxHeader, 0x80 } }, sha);

	// PRXType2::decrypt runs kirk7 over id | sha1 | kirkHeader[0..0x3C); invert that too.
	u8 a[0x60];
	memcpy(a, id.data(), 0x10);
	memcpy(a + 0x10, sha, 0x14);
	memcpy(a + 0x24, kh, 0x3C);
	kirk4(a, a, 0x60, code);
	u8 khStored[0x40];
	memcpy(khStored, a + 0x24, 0x3C);
	memcpy(khStored + 0x3C, kh + 0x3C, 4);

	Bytes prx(0x150 + padded, 0);
	memcpy(&prx[0], prxHeader, 0x80);
	memcpy(&prx[0x80], khStored, 0x30);
	memcpy(&prx[0xB0], metadata, 0x10);
	memcpy(&prx[0xC0], khStored + 0x30, 0x10);
	wr32(&prx[0xD0], tag);
	memcpy(&prx[0x12C], a + 0x10, 0x14);
	memcpy(&prx[0x140], a, 0x10);
	memcpy(&prx[0x150], &k[0x110], padded);
	return prx;
}

Bytes MakePspModule(const Bytes &elf, const std::string &modname, TestPacking packing, u32 tag,
		const std::string &compression, u32 keySeed) {
	Bytes payload = elf;
	u16 comp = 0;
	if (compression == "gzip") {
		payload = GzipStore(elf);
		comp = 1;
	} else if (compression == "kl4e") {
		payload = Kl4eEncodeLiterals(elf, 0x05);
		comp = 0x201;
	}
	u8 hdr[0x80] = {};
	memcpy(hdr, "~PSP", 4);
	wr16(hdr + 4, 0x1006);
	wr16(hdr + 6, comp);
	hdr[8] = 1;
	hdr[9] = 1;
	memcpy(hdr + 0x0A, modname.c_str(), std::min<size_t>(modname.size(), 27));
	hdr[0x26] = 1;
	hdr[0x27] = 1;
	wr32(hdr + 0x28, (u32)elf.size());
	wr32(hdr + 0x2C, (u32)(0x150 + payload.size()));
	wr32(hdr + 0x30, 0x100);
	wr32(hdr + 0x34, 0x800000A0);
	wr32(hdr + 0x78, 0x04010010);
	wr32(hdr + 0x7C, 2);

	switch (packing) {
	case TestPacking::KirkType0:
		return EncryptPrxType0(tag, hdr, payload, keySeed);
	case TestPacking::KirkType2:
		return EncryptPrxType2(tag, hdr, payload, keySeed);
	case TestPacking::CfwGzip:
	case TestPacking::UnknownTag: {
		Bytes prx(0x150, 0);
		memcpy(prx.data(), hdr, 0x80);
		wr32(&prx[0xB0], (u32)payload.size());
		wr32(&prx[0xD0], packing == TestPacking::UnknownTag ? 0x12345678 : tag);
		wr32(&prx[0x130], 0xC6BA41D3);
		if (packing == TestPacking::UnknownTag) {
			payload = PseudoRandom(payload.size(), keySeed);
		}
		prx.insert(prx.end(), payload.begin(), payload.end());
		return prx;
	}
	}
	return Bytes();
}

void MangleBlock(u8 *block) {
	kirk4(block + 0x20, block + 0x20, 0x130, 0x55);
}

Bytes EncryptFileList(const std::string &text, int tableKeyIndex) {
	// The PSAR file list keys (PPSSPP's kTableKeys).
	static const struct { u32 keyLow, keyHigh; u8 iv[8]; } kKeys[] = {
		{ 0xB730E5C7, 0x95620B49, { 0x9E, 0xA4, 0x33, 0x81, 0x86, 0x0C, 0x52, 0x85 } },
		{ 0x45C9DC95, 0x5A7B3D9D, { 0xB2, 0xFE, 0xD9, 0x79, 0x8A, 0x02, 0xB1, 0x87 } },
		{ 0x6F20585A, 0x4CCE495B, { 0x81, 0x08, 0xC1, 0xF2, 0x35, 0x98, 0x69, 0xB0 } },
		{ 0x620BF15A, 0x73F45262, { 0x6D, 0x52, 0x1B, 0xA3, 0xC2, 0x36, 0xF9, 0x2B } },
		{ 0xFD9D4498, 0xA664C8F8, { 0xDB, 0x4E, 0x79, 0x41, 0xF5, 0x97, 0x30, 0xAD } },
		{ 0x3D6426E7, 0xD7BD7481, { 0xA6, 0x83, 0x0C, 0x2F, 0x63, 0x0B, 0x96, 0x29 } },
	};
	const u32 keyLow = kKeys[tableKeyIndex].keyLow, keyHigh = kKeys[tableKeyIndex].keyHigh;
	const u8 *iv = kKeys[tableKeyIndex].iv;
	u8 key[8];
	const u64 combined = ((u64)keyHigh << 32) | keyLow;
	for (int i = 0; i < 8; i++) {
		key[i] = (u8)(combined >> (56 - i * 8));
	}
	const u8 zeroHeader[0x80] = {};
	Bytes blob = EncryptPrxType0(0x0E000000, zeroHeader, Bytes(text.begin(), text.end()), 0x7AB1E);
	DES(key).CbcEncrypt(blob.data(), blob.size(), iv);
	return blob;
}

int RunTool(const std::vector<std::string> &args, std::string *out) {
	const std::string outFile = TempPath("tool-stdout.txt");
	auto quote = [](std::string s) {
#ifdef _WIN32
		for (char &c : s) {
			if (c == '/') {
				c = '\\';
			}
		}
#endif
		return "\"" + s + "\"";
	};
	std::string cmd = quote(g_env.tool);
	for (const std::string &a : args) {
		cmd += " " + (a.size() && a[0] == '-' ? a : quote(a));
	}
	cmd += " > " + quote(outFile) + " 2>&1";
#ifdef _WIN32
	// cmd.exe strips one pair of outer quotes.
	cmd = "\"" + cmd + "\"";
#endif
	int rc = std::system(cmd.c_str());
#ifndef _WIN32
	if (rc != -1) {
		rc = WEXITSTATUS(rc);
	}
#endif
	if (out) {
		const Bytes data = ReadAll(outFile);
		out->assign(data.begin(), data.end());
	}
	return rc;
}
