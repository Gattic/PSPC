// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceChnnlsv (the savedata MAC and cipher) and the kernel "semaphore" library
// (sceUtilsBufferCopyWithRange: KIRK itself), on top of the pspc-decrypt KIRK service.
//
// Ported from PPSSPP (Core/HLE/sceChnnlsv.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later. The KIRK commands run in pspc-decrypt (kirk_client.h); nothing of libkirk is
// linked here.
#include "runtime/hle/modules/chnnlsv.h"

#include <cstring>
#include <mutex>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/kirk_client.h"
#include "runtime/hle/modules/misc_libs.h"

namespace chnnlsv {

namespace {

constexpr const char *LOG = "CHNNLSV";

// The MAC works through this buffer (a KIRK header, then up to 2048 bytes of data).
std::mutex g_lock;
u8 g_dataBuf[2048 + 20];
u8 *const g_dataBuf2 = g_dataBuf + 20;

const u8 kHash198C[16] = {0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF};
const u8 kHash19BC[16] = {0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86};
const u8 kKey19CC[16] = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};
const u8 kKey19DC[16] = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};
const u8 kKey199C[16] = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
const u8 kKey19AC[16] = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};

void MemXor(u8 *dest, const u8 *src, size_t n) {
	for (size_t i = 0; i < n; i++) dest[i] ^= src[i];
}

void Put32(u8 *p, s32 v) { memcpy(p, &v, 4); }

// The KIRK key index each mode uses (why these values is not known).
int NumFromMode(int mode) {
	switch (mode) {
	case 1: return 3;
	case 2: return 5;
	case 3: return 12;
	case 4: return 13;
	case 6: return 17;
	default: return 16;
	}
}

int NumFromMode2(int mode) {
	if (mode == 1) return 4;
	if (mode == 3) return 14;
	return 18;
}

int TypeFromMode(int mode) { return (mode == 1 || mode == 2) ? 83 : ((mode == 3 || mode == 4) ? 87 : 100); }

// KIRK 4/7 on data[20..20+length) with key `num`; the header goes in data[0..20).
int KirkSendCmd(u8 *data, int length, int num, bool encrypt) {
	Put32(data + 0, (s32)(encrypt ? kirk::MODE_ENCRYPT_CBC : kirk::MODE_DECRYPT_CBC));
	Put32(data + 4, 0);
	Put32(data + 8, 0);
	Put32(data + 12, num);
	Put32(data + 16, length);
	if (kirk::Command(encrypt ? kirk::CMD_ENCRYPT_IV_0 : kirk::CMD_DECRYPT_IV_0, data, length + 20, data, length + 20))
		return -257;
	return 0;
}

// KIRK 5/8: the console's own key.
int KirkSendFuseCmd(u8 *data, int length, bool encrypt) {
	Put32(data + 0, (s32)(encrypt ? kirk::MODE_ENCRYPT_CBC : kirk::MODE_DECRYPT_CBC));
	Put32(data + 4, 0);
	Put32(data + 8, 0);
	Put32(data + 12, 256);
	Put32(data + 16, length);
	if (kirk::Command(encrypt ? kirk::CMD_ENCRYPT_IV_FUSE : kirk::CMD_DECRYPT_IV_FUSE, data, length + 20, data,
	                  length + 20))
		return -258;
	return 0;
}

int Sub15B0(u8 *data, int alignedLen, u8 *buf, int val) {
	u8 sp0[16];
	memcpy(sp0, data + alignedLen + 4, 16);
	int res = KirkSendCmd(data, alignedLen, val, false);
	if (res) return res;
	MemXor(data, buf, 16);
	memcpy(buf, sp0, 16);
	return 0;
}

int Sub0000(u8 *dataOut, u8 *data, int alignedLen, const u8 *data2, int &data3, int mode) {
	memcpy(dataOut + 20, data2, 16);
	// Modes 1/2 are type 83, 3/4 type 87, 5/6 type 100.
	const int type = TypeFromMode(mode);
	if (type == 87)
		MemXor(dataOut + 20, kKey19AC, 16);
	else if (type == 100)
		MemXor(dataOut + 20, kKey19DC, 16);
	// Odd modes use a fixed key, even ones the console's.
	int res;
	switch (mode) {
	case 2:
	case 4:
	case 6: res = KirkSendFuseCmd(dataOut, 16, false); break;
	default: res = KirkSendCmd(dataOut, 16, NumFromMode2(mode), false); break;
	}
	if (type == 87)
		MemXor(dataOut, kKey199C, 16);
	else if (type == 100)
		MemXor(dataOut, kKey19CC, 16);
	if (res) return res;

	u8 sp0[16], sp16[16];
	memcpy(sp16, dataOut, 16);
	if (data3 == 1) {
		memset(sp0, 0, 16);
	} else {
		memcpy(sp0, sp16, 12);
		Put32(sp0 + 12, data3 - 1);
	}
	for (int i = 20; i < alignedLen + 20; i += 16) {
		memcpy(dataOut + i, sp16, 12);
		Put32(dataOut + 12 + i, data3);
		data3++;
	}
	res = Sub15B0(dataOut, alignedLen, sp0, type);
	if (res) return res;
	if (alignedLen > 0) MemXor(data, dataOut, alignedLen);
	return 0;
}

int Sub1510(u8 *data, int size, u8 *result, int num) {
	MemXor(data + 20, result, 16);
	int res = KirkSendCmd(data, size, num, true);
	if (res) return res;
	memcpy(result, data + size + 4, 16);
	return 0;
}

// KIRK 14: 20 random bytes.
int Sub17A8(u8 *data) { return kirk::Command(kirk::CMD_PRNG, data, 20, nullptr, 0) == 0 ? 0 : -261; }

// One step of the CMAC subkey doubling in GF(2^128).
void Double(u8 d[16]) {
	const int carry = (d[0] & 0x80) ? 135 : 0;
	for (int i = 0; i < 15; i++) d[i] = (u8)((d[i] << 1) | (d[i + 1] >> 7));
	d[15] = (u8)((d[15] << 1) ^ carry);
}

}  // namespace

int MacInit(MacContext &ctx, int mode) {
	ctx.mode = mode;
	memset(ctx.result, 0, 16);
	memset(ctx.key, 0, 16);
	ctx.keyLength = 0;
	return 0;
}

int MacUpdate(MacContext &ctx, const u8 *data, int length) {
	std::lock_guard<std::mutex> guard(g_lock);
	if (ctx.keyLength >= 17 || ctx.keyLength < 0) return -1026;
	if (length < 0) return -1026;
	if (ctx.keyLength + length < 17) {
		memcpy(ctx.key + ctx.keyLength, data, length);
		ctx.keyLength += length;
		return 0;
	}
	const int num = NumFromMode(ctx.mode);
	memset(g_dataBuf2, 0, 2048);
	memcpy(g_dataBuf2, ctx.key, ctx.keyLength);
	int len = (ctx.keyLength + length) & 0xF;
	if (len == 0) len = 16;
	int newSize = ctx.keyLength;
	ctx.keyLength = len;
	const int diff = length - len;
	memcpy(ctx.key, data + diff, len);
	for (int i = 0; i < diff; i++) {
		if (newSize == 2048) {
			int res = Sub1510(g_dataBuf, 2048, ctx.result, num);
			if (res) return res;
			newSize = 0;
		}
		g_dataBuf2[newSize] = data[i];
		newSize++;
	}
	if (newSize) Sub1510(g_dataBuf, newSize, ctx.result, num);
	// The firmware always returns 0 here.
	return 0;
}

int MacFinal(MacContext &ctx, u8 *hash, const u8 *key) {
	std::lock_guard<std::mutex> guard(g_lock);
	if (ctx.keyLength >= 17 || ctx.keyLength < 0) return -1026;
	const int num = NumFromMode(ctx.mode);
	memset(g_dataBuf2, 0, 16);
	int res = KirkSendCmd(g_dataBuf, 16, num, true);
	if (res) return res;

	u8 data1[16], data2[16];
	memcpy(data1, g_dataBuf2, 16);
	Double(data1);
	if (ctx.keyLength < 16) {
		Double(data1);
		const int oldKeyLength = ctx.keyLength;
		ctx.key[oldKeyLength] = 0x80;
		if (oldKeyLength + 1 < 16) memset(ctx.key + oldKeyLength + 1, 0, 16 - (oldKeyLength + 1));
	}
	MemXor(ctx.key, data1, 16);
	memcpy(g_dataBuf2, ctx.key, 16);
	memcpy(data2, ctx.result, 16);
	res = Sub1510(g_dataBuf, 16, data2, num);
	if (res) return res;

	if (ctx.mode == 3 || ctx.mode == 4)
		MemXor(data2, kHash198C, 16);
	else if (ctx.mode == 5 || ctx.mode == 6)
		MemXor(data2, kHash19BC, 16);

	if (ctx.mode == 2 || ctx.mode == 4 || ctx.mode == 6) {
		memcpy(g_dataBuf2, data2, 16);
		res = KirkSendFuseCmd(g_dataBuf, 16, true);
		if (res) return res;
		res = KirkSendCmd(g_dataBuf, 16, num, true);
		if (res) return res;
		memcpy(data2, g_dataBuf2, 16);
	}
	if (key) {
		for (int i = 0; i < 16; i++) data2[i] ^= key[i];
		memcpy(g_dataBuf2, data2, 16);
		res = KirkSendCmd(g_dataBuf, 16, num, true);
		if (res) return res;
		memcpy(data2, g_dataBuf2, 16);
	}
	memcpy(hash, data2, 16);
	MacInit(ctx, 0);
	return 0;
}

int CipherInit(CipherContext &ctx, int mode, int genMode, u8 *data, const u8 *key) {
	std::lock_guard<std::mutex> guard(g_lock);
	ctx.mode = mode;
	ctx.unkn = 1;
	if (genMode == 2) {
		// Decryption: the IV is the first 16 bytes of the data.
		memcpy(ctx.cryptedData, data, 16);
		if (key) MemXor(ctx.cryptedData, key, 16);
		return 0;
	}
	if (genMode == 1) {
		// Encryption: a random IV, encrypted, which goes in front of the data.
		u8 kirkHeader[37];
		u8 *kirkData = kirkHeader + 20;
		int res = Sub17A8(kirkHeader);
		if (res) return res;
		memcpy(kirkHeader + 20, kirkHeader, 16);
		memset(kirkHeader + 32, 0, 4);
		const int type = TypeFromMode(mode);
		if (type == 87)
			MemXor(kirkData, kKey199C, 16);
		else if (type == 100)
			MemXor(kirkData, kKey19CC, 16);
		switch (mode) {
		case 2:
		case 4:
		case 6: res = KirkSendFuseCmd(kirkHeader, 16, true); break;
		default: res = KirkSendCmd(kirkHeader, 16, NumFromMode2(mode), true); break;
		}
		if (type == 87)
			MemXor(kirkData, kKey19AC, 16);
		else if (type == 100)
			MemXor(kirkData, kKey19DC, 16);
		if (res) return res;
		memcpy(ctx.cryptedData, kirkData, 16);
		memcpy(data, kirkData, 16);
		if (key) MemXor(ctx.cryptedData, key, 16);
	}
	return 0;
}

int CipherUpdate(CipherContext &ctx, u8 *data, int alignedLen) {
	std::lock_guard<std::mutex> guard(g_lock);
	if (alignedLen == 0) return 0;
	if ((alignedLen & 0xF) != 0 || alignedLen < 0) return -1025;
	int i = 0;
	static u8 kirkData[20 + 2048];
	for (; alignedLen >= 2048; i += 2048) {
		int unkn = ctx.unkn;
		int res = Sub0000(kirkData, data + i, 2048, ctx.cryptedData, unkn, ctx.mode);
		ctx.unkn = unkn;
		alignedLen -= 2048;
		if (res) return res;
	}
	if (alignedLen == 0) return 0;
	int unkn = ctx.unkn;
	int res = Sub0000(kirkData, data + i, alignedLen, ctx.cryptedData, unkn, ctx.mode);
	ctx.unkn = unkn;
	return res;
}

int CipherFinal(CipherContext &ctx) {
	memset(ctx.cryptedData, 0, 16);
	ctx.unkn = 0;
	ctx.mode = 0;
	return 0;
}

}  // namespace chnnlsv

namespace {

using chnnlsv::CipherContext;
using chnnlsv::MacContext;

// The guest functions: contexts live in guest memory.
int sceSdSetIndex(PspPtr<MacContext> ctx, int mode) {
	if (!ctx) return 0;
	return chnnlsv::MacInit(*ctx, mode);
}

int sceSdRemoveValue(PspPtr<MacContext> ctx, u32 data, int length) {
	if (!ctx || length < 0 || !mem::IsValidRange(data, (u32)length)) return 0;
	return chnnlsv::MacUpdate(*ctx, mem::GetPointer(data, (u32)length), length);
}

int sceSdGetLastIndex(PspPtr<MacContext> ctx, u32 hash, u32 key) {
	if (!ctx || !mem::IsValidRange(hash, 16)) return 0;
	u8 out[16];
	const u8 *k = key && mem::IsValidRange(key, 16) ? mem::GetPointer(key, 16) : nullptr;
	int r = chnnlsv::MacFinal(*ctx, out, k);
	if (r == 0) mem::WriteBytes(hash, out, 16);
	return r;
}

int sceSdCreateList(u32 ctxAddr, int mode, int genMode, u32 data, u32 key) {
	if (!mem::IsValidRange(ctxAddr, sizeof(CipherContext)) || !mem::IsValidRange(data, 16)) return 0;
	const u8 *k = key && mem::IsValidRange(key, 16) ? mem::GetPointer(key, 16) : nullptr;
	return chnnlsv::CipherInit(*(CipherContext *)mem::GetPointer(ctxAddr, sizeof(CipherContext)), mode, genMode,
	                           mem::GetPointer(data, 16), k);
}

int sceSdSetMember(u32 ctxAddr, u32 data, int alignedLen) {
	if (!mem::IsValidRange(ctxAddr, sizeof(CipherContext)) || alignedLen < 0 || !mem::IsValidRange(data, (u32)alignedLen))
		return 0;
	return chnnlsv::CipherUpdate(*(CipherContext *)mem::GetPointer(ctxAddr, sizeof(CipherContext)),
	                             mem::GetPointer(data, (u32)alignedLen), alignedLen);
}

int sceSdCleanList(u32 ctxAddr) {
	if (!mem::IsValidRange(ctxAddr, sizeof(CipherContext))) return 0;
	return chnnlsv::CipherFinal(*(CipherContext *)mem::GetPointer(ctxAddr, sizeof(CipherContext)));
}

// The KIRK engine itself (kernel library "semaphore", exported by memlmd).
int BufferCopy(u32 outAddr, int outSize, u32 inAddr, int inSize, int cmd) {
	u8 *out = outSize > 0 && mem::IsValidRange(outAddr, (u32)outSize) ? mem::GetPointer(outAddr, (u32)outSize) : nullptr;
	u8 *in = inSize > 0 && mem::IsValidRange(inAddr, (u32)inSize) ? mem::GetPointer(inAddr, (u32)inSize) : nullptr;
	return kirk::Command(cmd, out, out ? outSize : 0, in, in ? inSize : 0);
}

u32 sceUtilsBufferCopyWithRange(u32 outAddr, int outSize, u32 inAddr, int inSize, int cmd) {
	int r = BufferCopy(outAddr, outSize, inAddr, inSize, cmd);
	if (r != 0) LOGW("CHNNLSV", "sceUtilsBufferCopyWithRange(cmd %d): KIRK returned %d", cmd, r);
	return 0;
}

int sceUtilsBufferCopyByPollingWithRange(u32 outAddr, int outSize, u32 inAddr, int inSize, int cmd) {
	return BufferCopy(outAddr, outSize, inAddr, inSize, cmd);
}

}  // namespace

void Register_ChnnlsvLibraries() {
	static const hle::HleFunction sceChnnlsv[] = {
		HLEFN(0xE7833020, sceSdSetIndex),
		HLEFN(0xF21A1FCA, sceSdRemoveValue),
		HLEFN(0xC4C494F8, sceSdGetLastIndex),
		HLEFN(0xABFDFC8B, sceSdCreateList),
		HLEFN(0x850A7FA1, sceSdSetMember),
		HLEFN(0x21BE78B4, sceSdCleanList),
	};
	hle::RegisterModule("sceChnnlsv", sceChnnlsv);
	hle::RegisterModule("sceChnnlsv_driver", sceChnnlsv);
	static const hle::HleFunction semaphore[] = {
		HLEFN(0x4C537C72, sceUtilsBufferCopyWithRange),
		HLEFN(0x77E97079, sceUtilsBufferCopyByPollingWithRange),
	};
	hle::RegisterModule("semaphore", semaphore);
}
