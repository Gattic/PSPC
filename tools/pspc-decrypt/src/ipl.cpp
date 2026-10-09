// SPDX-License-Identifier: GPL-3.0-or-later
// IPL stage 1 decryption. The block walk, the 03g+ descrambling (xor keys and original
// descrambling code thanks to Davee and Proxima) and the linearization follow John-K/pspdecrypt's
// pspDecryptIPL1() / pspLinearizeIPL2() (GPL-3.0).
#include "ipl.h"

#include "prx_decrypter.h"

#include "../libkirk/kirk_engine.h"

static const u32 kXorKeys[] = {
	0x61A0C918, 0x45695E82, 0x9CAFD36E, 0xFA499B0F,
	0x7E84B6E2, 0x91324D29, 0xB3522009, 0xA8BC0FAF,
	0x48C3C1C5, 0xE4C2A9DC, 0x00012ED1, 0x57D9327C,
	0xAFB8E4EF, 0x72489A15, 0xC6208D85, 0x06021249,
	0x41BE16DB, 0x2BD98F2F, 0xD194BEEB, 0xD1A6E669,
	0xC0AC336B, 0x88FF3544, 0x5E018640, 0x34318761,
	0x5974E1D2, 0x1E55581B, 0x6F28379E, 0xA90E2587,
	0x091CB883, 0xBDC2088A, 0x7E76219C, 0x9C4BEE1B,
	0xDD322601, 0xBB477339, 0x6678CF47, 0xF3C1209B,
	0x5A96E435, 0x908896FA, 0x5B2D962A, 0x7FEC378C,
	0xE3A3B3AE, 0x8B902D93, 0xD0DF32EF, 0x6484D261,
	0x0A84A153, 0x7EB16575, 0xB10E53DD, 0x1B222753,
	0x58DD63D0, 0x8E8B8D48, 0x755B32C2, 0xA63DFFF7,
	0x97CABF7C, 0x33BDC660, 0x64522286, 0x403F3698,
	0x3406C651, 0x9F4B8FB9, 0xE284F475, 0xB9189A13,
	0x12C6F917, 0x5DE6B7ED, 0xDB674F88, 0x06DDB96E,
	0x2B2165A6, 0x0F920D3F, 0x732B3475, 0x1908D613,
};

static const u32 kIplBlockSize = 0x1000;
static const u32 kMaxDescrambleIndex = 0x7E0;

static u32 Rotr(u32 x, u32 r) {
	r &= 31;
	return r ? (x >> r) | (x << (32 - r)) : x;
}

static u32 BitReverse(u32 b) {
	u32 x = 0;
	for (int i = 0; i < 32; i++) {
		x |= ((b >> i) & 1) << (31 - i);
	}
	return x;
}

// 03g+ IPLs scramble the first 16 bytes of each block's KIRK header with one of these keys.
static void Descramble03g(u8 *block, u32 i) {
	const u32 idx = (i >> 5) & 0x3F;
	const u32 rot = i & 0x1F;
	const u32 x1 = Rotr(kXorKeys[idx], rot);
	const u32 x2 = BitReverse(Rotr(kXorKeys[idx + 1], rot));
	const u32 x3 = Rotr(kXorKeys[idx + 2], rot) ^ kXorKeys[idx + 3];
	const u32 x4 = Rotr(kXorKeys[idx + 3], rot);
	wr32(block + 0, rd32(block + 0) ^ x1);
	wr32(block + 4, rd32(block + 4) ^ x2);
	wr32(block + 8, rd32(block + 8) ^ x3);
	wr32(block + 12, rd32(block + 12) ^ x4);
}

// One block through KIRK CMD1. Blocks signed with ECDSA are decrypted without checking the
// signature if KIRK's check fails (pspdecrypt disables it outright); the CMAC ones are verified,
// which is also what makes the 03g+ key search work.
static bool Kirk1Block(KirkState *kirk, const u8 *block, u8 *out, bool allowUnverified) {
	u8 buf[0x40 + kIplBlockSize + 0x40] = {};
	memcpy(buf + 0x40, block, kIplBlockSize);
	const KIRK_CMD1_HEADER *h = (const KIRK_CMD1_HEADER *)(buf + 0x40);
	const u32 dataOffset = h->data_offset, dataSize = h->data_size;
	if (h->mode != KIRK_MODE_CMD1 || dataSize == 0 || (u64)0x90 + dataOffset + ((dataSize + 15) & ~15u) > kIplBlockSize) {
		return false;
	}
	const bool ecdsa = h->ecdsa_hash == 1;
	memset(out, 0, kIplBlockSize);
	const int res = kirk_CMD1(kirk, buf, buf + 0x40, (int)(0x90 + dataOffset + dataSize));
	if (res == 0) {
		memcpy(out, buf, dataSize);
		return true;
	}
	if (!ecdsa || !allowUnverified) {
		return false;
	}
	u8 aesKey[16];
	AES_cbc_decrypt(&kirk->aes_kirk1, block, aesKey, 16);
	AES_ctx ctx;
	AES_set_key(&ctx, aesKey, 128);
	AES_cbc_decrypt(&ctx, block + 0x90 + dataOffset, buf, (int)dataSize);
	memcpy(out, buf, dataSize);
	return true;
}

bool DecryptIplStage1(const u8 *data, size_t size, IplStage1 *out, std::string *error) {
	*out = IplStage1();
	Bytes input(data, data + size);

	// An IPL whose first block isn't a KIRK CMD1 block (mode 1, or 0x10001 for 03g+) is wrapped
	// in a tag-encrypted blob first.
	if (size >= 0x150 && rd32(data + 0x60) != 1 && rd32(data + 0x60) != 0x10001) {
		Bytes inner;
		PrxDecryptInfo info;
		if (DecryptPRX(data, size, size, &inner, nullptr, &info) > 0) {
			input = std::move(inner);
			out->preDecrypted = true;
		}
	}

	KirkState kirk{};
	kirk_init(&kirk);

	Bytes decrypted;
	u8 block[kIplBlockSize];
	u8 plain[kIplBlockSize];
	int descramble = -1;
	for (size_t pos = 0; pos + kIplBlockSize <= input.size(); pos += kIplBlockSize) {
		memcpy(block, input.data() + pos, kIplBlockSize);
		bool ok;
		if (block[0x62] == 1) {
			block[0x62] = 0;
			if (descramble < 0) {
				// Which key was used isn't recorded anywhere; the CMAC check tells us when it's right.
				ok = false;
				for (u32 i = 0; i < kMaxDescrambleIndex && !ok; i++) {
					u8 trial[kIplBlockSize];
					memcpy(trial, block, kIplBlockSize);
					Descramble03g(trial, i);
					if (Kirk1Block(&kirk, trial, plain, false)) {
						descramble = (int)i;
						ok = true;
					}
				}
			} else {
				Descramble03g(block, (u32)descramble);
				ok = Kirk1Block(&kirk, block, plain, true);
			}
		} else {
			ok = Kirk1Block(&kirk, block, plain, true);
		}
		if (!ok) {
			if (out->blocks == 0) {
				*error = "the first IPL block doesn't decrypt";
				return false;
			}
			out->note = StringPrintf("stopped at block %d, which doesn't decrypt", out->blocks);
			break;
		}
		decrypted.insert(decrypted.end(), plain, plain + kIplBlockSize);
		out->blocks++;
	}
	out->descrambleKey = descramble;
	if (out->blocks == 0) {
		*error = "no IPL blocks";
		return false;
	}

	// Each decrypted block: load address, byte count, entry point (last block only), checksum,
	// then the data. They're meant to be contiguous.
	u32 nextAddr = 0;
	for (int i = 0; i < out->blocks; i++) {
		const u8 *b = decrypted.data() + (size_t)i * kIplBlockSize;
		const u32 addr = rd32(b), count = rd32(b + 4), entry = rd32(b + 8);
		if (count > kIplBlockSize - 0x10) {
			out->note = StringPrintf("block %d has a bad length", i);
			break;
		}
		if (i == 0) {
			out->loadAddress = addr;
		} else if (addr != nextAddr) {
			out->note = StringPrintf("block %d isn't contiguous (0x%08x, expected 0x%08x)", i, addr, nextAddr);
			break;
		}
		out->image.insert(out->image.end(), b + 0x10, b + 0x10 + count);
		nextAddr = addr + count;
		if (entry != 0) {
			out->entry = entry;
		}
	}
	return true;
}
