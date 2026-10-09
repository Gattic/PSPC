// SPDX-License-Identifier: GPL-3.0-or-later
// DES with one bit per byte throughout. The tables are a few tens of KB in total, so this doesn't
// need to be fast, and the permutations are much easier to read this way. Tables and structure
// from PPSSPP's Core/Util/PSARUnpack.cpp (Copyright (c) 2026- PPSSPP Project, GPL-2.0-or-later).
#include "des.h"

static const u8 kIP[64] = {
	58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4, 62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
	57,49,41,33,25,17, 9,1, 59,51,43,35,27,19,11,3, 61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7 };
static const u8 kFP[64] = {
	40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31, 38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
	36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27, 34,2,42,10,50,18,58,26, 33,1,41, 9,49,17,57,25 };
static const u8 kE[48] = {
	32,1,2,3,4,5, 4,5,6,7,8,9, 8,9,10,11,12,13, 12,13,14,15,16,17,
	16,17,18,19,20,21, 20,21,22,23,24,25, 24,25,26,27,28,29, 28,29,30,31,32,1 };
static const u8 kP[32] = {
	16,7,20,21, 29,12,28,17, 1,15,23,26, 5,18,31,10, 2,8,24,14, 32,27,3,9, 19,13,30,6, 22,11,4,25 };
static const u8 kPC1[56] = {
	57,49,41,33,25,17, 9, 1,58,50,42,34,26,18, 10, 2,59,51,43,35,27, 19,11, 3,60,52,44,36,
	63,55,47,39,31,23,15, 7,62,54,46,38,30,22, 14, 6,61,53,45,37,29, 21,13, 5,28,20,12, 4 };
static const u8 kPC2[48] = {
	14,17,11,24, 1, 5, 3,28,15, 6,21,10, 23,19,12, 4,26, 8, 16, 7,27,20,13, 2,
	41,52,31,37,47,55, 30,40,51,45,33,48, 44,49,39,56,34,53, 46,42,50,36,29,32 };
static const u8 kShifts[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };
static const u8 kSBox[8][64] = {
	{ 14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,  0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
	   4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0, 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13 },
	{ 15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,  3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
	   0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15, 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9 },
	{ 10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8, 13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
	  13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,  1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12 },
	{  7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15, 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
	  10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,   3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14 },
	{  2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9, 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
	   4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14, 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3 },
	{ 12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,  10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
	   9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,  4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13 },
	{  4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1, 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
	   1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,  6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12 },
	{ 13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,   1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
	   7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,  2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11 } };

// One bit per byte throughout - the tables are a few tens of KB in total, so this doesn't need
// to be a fast DES, and the permutations are much easier to read this way.
static void BytesToBits(const u8 *bytes, int byteCount, u8 *bits) {
	for (int i = 0; i < byteCount; i++) {
		for (int bit = 0; bit < 8; bit++) {
			bits[i * 8 + bit] = (bytes[i] >> (7 - bit)) & 1;
		}
	}
}

static void BitsToBytes(const u8 *bits, int byteCount, u8 *bytes) {
	for (int i = 0; i < byteCount; i++) {
		u8 value = 0;
		for (int bit = 0; bit < 8; bit++) {
			value = (u8)((value << 1) | bits[i * 8 + bit]);
		}
		bytes[i] = value;
	}
}

// The tables are 1-based, as they're always written.
static void Permute(const u8 *in, u8 *out, const u8 *table, int count) {
	for (int i = 0; i < count; i++) {
		out[i] = in[table[i] - 1];
	}
}

DES::DES(const u8 key[8]) {
	u8 keyBits[64];
	BytesToBits(key, 8, keyBits);
	u8 cd[56];
	Permute(keyBits, cd, kPC1, 56);
	u8 *c = cd;
	u8 *d = cd + 28;
	for (int round = 0; round < 16; round++) {
		for (int shift = 0; shift < kShifts[round]; shift++) {
			const u8 cFirst = c[0];
			const u8 dFirst = d[0];
			memmove(c, c + 1, 27);
			memmove(d, d + 1, 27);
			c[27] = cFirst;
			d[27] = dFirst;
		}
		Permute(cd, subkeys_[round], kPC2, 48);
	}
}

void DES::Crypt(const u8 in[8], u8 out[8], bool decrypt) const {
	u8 bits[64];
	u8 permuted[64];
	BytesToBits(in, 8, bits);
	Permute(bits, permuted, kIP, 64);

	u8 left[32], right[32];
	memcpy(left, permuted, 32);
	memcpy(right, permuted + 32, 32);

	// Subkeys in reverse order is what makes this a decrypt.
	for (int step = 0; step < 16; step++) {
		const int round = decrypt ? 15 - step : step;
		u8 expanded[48];
		Permute(right, expanded, kE, 48);
		for (int i = 0; i < 48; i++) {
			expanded[i] ^= subkeys_[round][i];
		}

		u8 substituted[32];
		for (int box = 0; box < 8; box++) {
			const u8 *six = expanded + box * 6;
			const int row = (six[0] << 1) | six[5];
			const int column = (six[1] << 3) | (six[2] << 2) | (six[3] << 1) | six[4];
			const u8 value = kSBox[box][row * 16 + column];
			for (int bit = 0; bit < 4; bit++) {
				substituted[box * 4 + bit] = (value >> (3 - bit)) & 1;
			}
		}

		u8 mixed[32];
		Permute(substituted, mixed, kP, 32);
		u8 next[32];
		for (int i = 0; i < 32; i++) {
			next[i] = left[i] ^ mixed[i];
		}
		memcpy(left, right, 32);
		memcpy(right, next, 32);
	}

	u8 combined[64];
	memcpy(combined, right, 32);
	memcpy(combined + 32, left, 32);
	Permute(combined, permuted, kFP, 64);
	BitsToBytes(permuted, 8, out);
}

void DES::EncryptBlock(const u8 in[8], u8 out[8]) const {
	Crypt(in, out, false);
}

void DES::DecryptBlock(const u8 in[8], u8 out[8]) const {
	Crypt(in, out, true);
}

void DES::CbcDecrypt(u8 *data, size_t size, const u8 iv[8]) const {
	u8 previous[8];
	memcpy(previous, iv, sizeof(previous));
	for (size_t offset = 0; offset + 8 <= size; offset += 8) {
		u8 *at = data + offset;
		u8 cipher[8], plain[8];
		memcpy(cipher, at, sizeof(cipher));
		DecryptBlock(cipher, plain);
		for (int i = 0; i < 8; i++) {
			at[i] = plain[i] ^ previous[i];
		}
		memcpy(previous, cipher, sizeof(previous));
	}
}

void DES::CbcEncrypt(u8 *data, size_t size, const u8 iv[8]) const {
	u8 previous[8];
	memcpy(previous, iv, sizeof(previous));
	for (size_t offset = 0; offset + 8 <= size; offset += 8) {
		u8 *at = data + offset;
		u8 plain[8];
		for (int i = 0; i < 8; i++) {
			plain[i] = at[i] ^ previous[i];
		}
		EncryptBlock(plain, at);
		memcpy(previous, at, sizeof(previous));
	}
}
