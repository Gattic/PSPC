// SPDX-License-Identifier: GPL-3.0-or-later
// PRX (~PSP) decryption. Ported from PPSSPP's Core/ELF/PrxDecrypter.cpp (GPL-2.0-or-later,
// Copyright (c) 2012- PPSSPP Project), with the extra tags carried by John-K/pspdecrypt
// (GPL-3.0). Thanks to PSARDUMPER, JPCSP and everyone who dug these keys out.
#pragma once

#include "common.h"

constexpr u32 PSP_HEADER_SIZE = 0x150;

// The ~PSP header, decoded field by field (offsets in comments).
struct PspHeader {
	u32 signature;        // 00 "~PSP"
	u16 attribute;        // 04 module info attribute
	u16 compAttribute;    // 06 bit 0 compressed, 0x200 KL4E, 0x8 gzip overlap
	u8 moduleVerLo;       // 08
	u8 moduleVerHi;       // 09
	std::string modname;  // 0A, 28 chars
	u8 version;           // 26
	u8 nsegments;         // 27
	u32 elfSize;          // 28
	u32 pspSize;          // 2C
	u32 entry;            // 30
	u32 modinfoOffset;    // 34
	s32 bssSize;          // 38
	u16 segAlign[4];      // 3C
	u32 segAddress[4];    // 44
	s32 segSize[4];       // 54
	u32 devkitVersion;    // 78
	u32 decryptMode;      // 7C
	s32 compSize;         // B0
	u32 tag;              // D0
	u32 oeTag;            // 130 (custom firmware packers)
};

bool ParsePspHeader(const u8 *data, size_t size, PspHeader *out);

enum class PrxError {
	None,
	TooSmall,
	UnknownTag,     // No key table entry for the tag at 0xD0.
	NeedsSeed,      // Type 5 tag whose hash only checks out with an NPDRM seed we weren't given.
	HeaderCheck,    // Known tag, but the reserved area isn't empty for any type that uses it.
	HashMismatch,   // Known tag, header SHA1 didn't match: wrong key/type or corrupt data.
	BadKirkHeader,  // KIRK header asks for more data than there is.
	KirkFailed,     // KIRK CMD1 rejected the block (CMAC/ECDSA).
};

const char *PrxErrorString(PrxError e);

struct PrxDecryptInfo {
	PrxError error = PrxError::None;
	int type = -1;     // 0, 1, 2, 5, 6 or 9: which header layout decrypted it.
	u32 tag = 0;
	int kirkResult = 0;
};

// Decrypts a KIRK-encrypted PRX blob of `size` bytes. `readable` (>= size) is how much of `in`
// may be read: the hash checks cover up to 0x40 bytes past the declared size, so callers should
// expose the real bytes that follow when they have them. On success returns the payload size
// (still compressed if the header says so) and fills `out`; on failure returns a negative value.
int DecryptPRX(const u8 *in, size_t size, size_t readable, Bytes *out, const u8 *seed, PrxDecryptInfo *info);

// What the key tables know about a tag: 0 = nothing, 1 = 144-byte key (types 0/1),
// 2 = 16-byte key (types 2/5/6/9), 3 = both.
int PrxTagKnown(u32 tag);

// The effective 0x90-byte XOR key for a 144-byte-key tag (after the kirk7 pass for raw keys).
// For tests. Returns false for tags without such a key.
bool PrxDebugTagKey(u32 tag, u8 out[0x90], u8 *code);
// The 16-byte seed key and KIRK code for a type 2/5/6/9 tag. For tests.
bool PrxDebugTagKey2(u32 tag, u8 key[16], u8 *code);
