// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// UtilsForUser: libc time, cache maintenance, MD5/SHA-1/MT19937 with their contexts in guest
// memory laid out as the firmware's (pspautotests hash/*ctx), and the debug GPIO.
#include <cstring>

#include "common/sha1.h"
#include "runtime/core/dcache.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/hle/modules/rtc.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/module.h"
#include "runtime/kernel/scheduler.h"

namespace {

// ---------------------------------------------------------------- libc time

u32 sceKernelLibcClock() {
	const u32 t = (u32)sched::NowUs();
	sched::ChargeCycles(330);
	return t;
}

u32 sceKernelLibcTime(u32 outPtr) {
	const u32 t = (u32)(rtc::NowUnixUs() / 1000000ULL);
	// The PSP sure takes its sweet time on this function.
	sched::ChargeCycles(3385);
	if (mem::IsValidRange(outPtr, 4)) GWrite32(outPtr, t);
	else if (outPtr != 0) return 0;
	return t;
}

u32 sceKernelLibcGettimeofday(u32 tvPtr, u32 tzPtr) {
	const u64 now = rtc::NowUnixUs();
	if (mem::IsValidRange(tvPtr, 8)) {
		GWrite32(tvPtr, (u32)(now / 1000000));
		GWrite32(tvPtr + 4, (u32)(now % 1000000));
	}
	if (mem::IsValidRange(tzPtr, 8)) {
		GWrite32(tzPtr, (u32)(-rtc::StandardOffsetUs() / 60000000));  // minutes west
		GWrite32(tzPtr + 4, 0);
	}
	sched::ChargeCycles(1885);
	return 0;
}

// ---------------------------------------------------------------- caches
// Guest memory is coherent here, so these only validate and take their time, unless the data
// cache is modelled (--dcache-model).

int sceKernelDcacheInvalidateRange(u32 addr, int size) {
	if (size < 0 || (int)addr + size < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (size > 0 && ((addr % 64) != 0 || (size % 64) != 0)) return (int)SCE_KERNEL_ERROR_CACHE_ALIGNMENT;
	if (dcache::g_enabled) dcache::InvalidateRange(addr, (u32)size);
	sched::ChargeCycles(190);
	return 0;
}
int sceKernelDcacheWritebackRange(u32 addr, int size) {
	if (size < 0) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	if (dcache::g_enabled) dcache::WritebackRange(addr, (u32)size, false);
	sched::ChargeCycles(165);
	return 0;
}
int sceKernelDcacheWritebackInvalidateRange(u32 addr, int size) {
	if (size < 0) return (int)SCE_KERNEL_ERROR_INVALID_SIZE;
	if (dcache::g_enabled) dcache::WritebackRange(addr, (u32)size, true);
	sched::ChargeCycles(165);
	return 0;
}
int sceKernelDcacheWritebackAll() {
	if (dcache::g_enabled) dcache::WritebackAll(false);
	sched::ChargeCycles(3524);
	sched::Reschedule();
	return 0;
}
int sceKernelDcacheWritebackInvalidateAll() {
	if (dcache::g_enabled) dcache::WritebackAll(true);
	sched::ChargeCycles(1165);
	sched::Reschedule();
	return 0;
}
// Code writes become visible to execution here: native (AOT) functions whose code changed fall
// back to the interpreter (kernel::InvalidateNativeCode).
int sceKernelIcacheInvalidateRange(u32 addr, int size) {
	if (size > 0) kernel::InvalidateNativeCode(addr, (u32)size);
	return 0;
}
u32 sceKernelIcacheInvalidateAll() {
	kernel::InvalidateNativeCode(0x08000000, 0x02000000);
	return 0;
}

// ---------------------------------------------------------------- MD5 / SHA-1

u32 Rol(u32 x, int c) { return (x << c) | (x >> (32 - c)); }

void Md5Compress(u32 h[5], const u8 *p) {
	static const u32 K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
	static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
	                          14, 20, 5, 9, 14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	                          4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
	u32 m[16];
	for (int i = 0; i < 16; i++) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((u32)p[i * 4 + 3] << 24);
	u32 a = h[0], b = h[1], c = h[2], d = h[3];
	for (int i = 0; i < 64; i++) {
		u32 f;
		int g;
		if (i < 16) {
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & c);
			g = (5 * i + 1) % 16;
		} else if (i < 48) {
			f = b ^ c ^ d;
			g = (3 * i + 5) % 16;
		} else {
			f = c ^ (b | ~d);
			g = (7 * i) % 16;
		}
		const u32 tmp = d;
		d = c;
		c = b;
		b = b + Rol(a + f + K[i] + m[g], R[i]);
		a = tmp;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
}

void Sha1Compress(u32 h[5], const u8 *p) {
	u32 w[80];
	for (int i = 0; i < 16; i++) w[i] = ((u32)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		const u32 t = Rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = Rol(b, 30);
		b = a;
		a = t;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
}

// The firmware's 96-byte context: the state (4 or 5 words; MD5 never writes word 4), the bytes
// held in buf (u16) and a u16 that stays 0, the total length in bytes (u64), then buf.
struct HashCtx {
	bool sha;
	u32 h[5];
	u64 total;
	u8 buf[64];
};
constexpr u32 kHashCtxSize = 96;

void Compress(HashCtx &c, const u8 *block) { c.sha ? Sha1Compress(c.h, block) : Md5Compress(c.h, block); }

void HashUpdate(HashCtx &c, const u8 *data, u32 len) {
	u32 left = (u32)(c.total & 63);
	const u32 fill = 64 - left;
	c.total += len;
	if (left && len >= fill) {
		memcpy(c.buf + left, data, fill);
		Compress(c, c.buf);
		data += fill;
		len -= fill;
		left = 0;
	}
	while (len >= 64) {
		Compress(c, data);
		data += 64;
		len -= 64;
	}
	if (len) memcpy(c.buf + left, data, len);
}

void HashFinish(HashCtx &c, u8 *out) {
	const u64 bits = c.total * 8;
	u8 lenBytes[8];
	for (int i = 0; i < 8; i++) lenBytes[i] = (u8)(c.sha ? bits >> (56 - 8 * i) : bits >> (8 * i));
	static const u8 pad[64] = {0x80};
	const u32 last = (u32)(c.total & 63);
	const u32 padLen = last < 56 ? 56 - last : 120 - last;
	HashUpdate(c, pad, padLen);
	HashUpdate(c, lenBytes, 8);
	const int words = c.sha ? 5 : 4;
	for (int i = 0; i < words; i++)
		for (int j = 0; j < 4; j++) out[i * 4 + j] = (u8)(c.sha ? c.h[i] >> (24 - 8 * j) : c.h[i] >> (8 * j));
}

HashCtx ReadCtx(u32 addr, bool sha) {
	HashCtx c{};
	c.sha = sha;
	for (int i = 0; i < (sha ? 5 : 4); i++) c.h[i] = GRead32(addr + 4 * i);
	c.total = GRead<u64>(addr + 24);
	mem::ReadBytes(c.buf, addr + 32, 64);
	return c;
}

void WriteCtx(u32 addr, const HashCtx &c, bool withBuf) {
	for (int i = 0; i < (c.sha ? 5 : 4); i++) GWrite32(addr + 4 * i, c.h[i]);
	GWrite16(addr + 20, (u16)(c.total & 0x3F));
	GWrite16(addr + 22, 0);
	GWrite<u64>(addr + 24, c.total);
	if (withBuf) mem::WriteBytes(addr + 32, c.buf, 64);
}

int BlockInit(u32 ctx, bool sha) {
	if (!mem::IsValidRange(ctx, kHashCtxSize)) return -1;
	static const u32 init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	HashCtx c{};
	c.sha = sha;
	memcpy(c.h, init, sizeof(init));
	// Only the state and the counters: buf (and MD5's spare word) are left as they were.
	WriteCtx(ctx, c, false);
	return 0;
}

int BlockUpdate(u32 ctx, u32 data, u32 len, bool sha) {
	if (!mem::IsValidRange(ctx, kHashCtxSize) || (len && !mem::IsValidRange(data, len))) return -1;
	HashCtx c = ReadCtx(ctx, sha);
	const u64 before = c.total;
	const u8 *p = len ? GPtr(data) : nullptr;
	if (len) HashUpdate(c, p, len);
	if (sha) {
		// SHA-1 hashes whole blocks straight from the caller's data; buf holds only the tail.
		WriteCtx(ctx, c, true);
	} else {
		// MD5 streams every byte through buf: after the update it holds the last (up to) 64
		// bytes, each at its offset in the stream.
		WriteCtx(ctx, c, false);
		u64 pos = before;
		if (len >= 64) {
			p += len - 64;
			pos += len - 64;
			len = 64;
		}
		for (u32 i = 0; i < len; i++) GWrite8(ctx + 32 + (u32)((pos + i) % 64), p[i]);
	}
	return 0;
}

int BlockResult(u32 ctx, u32 digest, bool sha) {
	const u32 size = sha ? 20 : 16;
	if (!mem::IsValidRange(ctx, kHashCtxSize) || !mem::IsValidRange(digest, size)) return -1;
	HashCtx c = ReadCtx(ctx, sha);
	u8 out[20];
	HashFinish(c, out);
	mem::WriteBytes(digest, out, size);
	WriteCtx(ctx, c, sha);
	return 0;
}

int sceKernelUtilsMd5Digest(u32 data, int size, u32 digest) {
	if ((size > 0 && !mem::IsValidRange(data, (u32)size)) || !mem::IsValidRange(digest, 16)) return -1;
	HashCtx c{};
	c.h[0] = 0x67452301;
	c.h[1] = 0xEFCDAB89;
	c.h[2] = 0x98BADCFE;
	c.h[3] = 0x10325476;
	if (size > 0) HashUpdate(c, GPtr(data), (u32)size);
	u8 out[20];
	HashFinish(c, out);
	mem::WriteBytes(digest, out, 16);
	return 0;
}
int sceKernelUtilsMd5BlockInit(u32 ctx) { return BlockInit(ctx, false); }
int sceKernelUtilsMd5BlockUpdate(u32 ctx, u32 data, int len) { return BlockUpdate(ctx, data, (u32)len, false); }
int sceKernelUtilsMd5BlockResult(u32 ctx, u32 digest) { return BlockResult(ctx, digest, false); }

int sceKernelUtilsSha1Digest(u32 data, int size, u32 digest) {
	if ((size > 0 && !mem::IsValidRange(data, (u32)size)) || !mem::IsValidRange(digest, 20)) return -1;
	auto h = Sha1::Hash(size > 0 ? GPtr(data) : nullptr, size > 0 ? (size_t)size : 0);
	mem::WriteBytes(digest, h.data(), 20);
	return 0;
}
int sceKernelUtilsSha1BlockInit(u32 ctx) { return BlockInit(ctx, true); }
int sceKernelUtilsSha1BlockUpdate(u32 ctx, u32 data, int len) { return BlockUpdate(ctx, data, (u32)len, true); }
int sceKernelUtilsSha1BlockResult(u32 ctx, u32 digest) { return BlockResult(ctx, digest, true); }

// ---------------------------------------------------------------- MT19937
// Context: the index (u32), then the 624-word state. Init seeds and twists the whole array; each
// draw reads a word and regenerates it in place, which in index order is the same as twisting
// all of it at once (and is what the hardware leaves in the context).

constexpr u32 kMtN = 624;

void MtTwist(u32 *mt, u32 i) {
	const u32 y = (mt[i] & 0x80000000u) | (mt[(i + 1) % kMtN] & 0x7FFFFFFFu);
	mt[i] = mt[(i + 397) % kMtN] ^ (y >> 1);
	if (y & 1) mt[i] ^= 0x9908B0DFu;
}

u32 sceKernelUtilsMt19937Init(u32 ctx, u32 seed) {
	if (!mem::IsValidRange(ctx, (kMtN + 1) * 4)) return (u32)-1;
	u32 mt[kMtN];
	mt[0] = seed;
	for (u32 i = 1; i < kMtN; i++) mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
	for (u32 i = 0; i < kMtN; i++) MtTwist(mt, i);
	GWrite32(ctx, 0);
	mem::WriteBytes(ctx + 4, mt, sizeof(mt));
	return 0;
}

u32 sceKernelUtilsMt19937UInt(u32 ctx) {
	if (!mem::IsValidRange(ctx, (kMtN + 1) * 4)) return (u32)-1;
	u32 *mt = (u32 *)GPtr(ctx + 4);
	u32 index = GRead32(ctx) % kMtN;
	u32 y = mt[index];
	MtTwist(mt, index);
	GWrite32(ctx, (index + 1) % kMtN);
	y ^= y >> 11;
	y ^= (y << 7) & 0x9D2C5680u;
	y ^= (y << 15) & 0xEFC60000u;
	y ^= y >> 18;
	return y;
}

// ---------------------------------------------------------------- GPIO
// Production units read 0 from the switches; the LEDs go nowhere.
u32 g_gpo = 0;
u32 sceKernelGetGPI() { return 0; }
void sceKernelSetGPO(u32 bits) { g_gpo = bits; }

}  // namespace

void Register_UtilsForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x91E4F6A7, sceKernelLibcClock),
		HLEFN(0x27CC57F0, sceKernelLibcTime),
		HLEFN(0x71EC4271, sceKernelLibcGettimeofday),
		HLEFN(0xBFA98062, sceKernelDcacheInvalidateRange),
		HLEFN(0xC8186A58, sceKernelUtilsMd5Digest),
		HLEFN(0x9E5C5086, sceKernelUtilsMd5BlockInit),
		HLEFN(0x61E1E525, sceKernelUtilsMd5BlockUpdate),
		HLEFN(0xB8D24E78, sceKernelUtilsMd5BlockResult),
		HLEFN(0x840259F1, sceKernelUtilsSha1Digest),
		HLEFN(0xF8FCD5BA, sceKernelUtilsSha1BlockInit),
		HLEFN(0x346F6DA8, sceKernelUtilsSha1BlockUpdate),
		HLEFN(0x585F1C09, sceKernelUtilsSha1BlockResult),
		HLEFN(0xE860E75E, sceKernelUtilsMt19937Init),
		HLEFN(0x06FB8A63, sceKernelUtilsMt19937UInt),
		HLEFN(0x37FB5C42, sceKernelGetGPI),
		HLEFN(0x6AD345D7, sceKernelSetGPO),
		HLEFN(0x79D1C3FA, sceKernelDcacheWritebackAll),
		HLEFN(0xB435DEC5, sceKernelDcacheWritebackInvalidateAll),
		HLEFN(0x3EE30821, sceKernelDcacheWritebackRange),
		HLEFN(0x34B9FA9E, sceKernelDcacheWritebackInvalidateRange),
		HLEFN(0xC2DF770E, sceKernelIcacheInvalidateRange),
		HLEFN(0x920F104A, sceKernelIcacheInvalidateAll),
	};
	hle::RegisterModule("UtilsForUser", funcs);
	hle::RegisterModule("UtilsForKernel", funcs);
}
