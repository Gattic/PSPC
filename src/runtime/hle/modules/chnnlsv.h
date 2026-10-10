// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceChnnlsv: the savedata hash (MAC) and cipher the system uses for secure saves and PARAM.SFO
// hashes, built on KIRK commands 4/5/7/8/14 (kirk_client.h). Also usable natively, by the
// savedata utility. Ported from PPSSPP (Core/HLE/sceChnnlsv.cpp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later.
#pragma once

#include "common/common.h"

namespace chnnlsv {

// pspChnnlsvContext1 (the MAC state), 0x28 bytes in guest memory.
struct MacContext {
	s32 mode;
	u8 result[0x10];
	u8 key[0x10];
	s32 keyLength;
};
static_assert(sizeof(MacContext) == 0x28, "pspChnnlsvContext1");

// pspChnnlsvContext2 (the cipher state).
struct CipherContext {
	s32 mode;
	s32 unkn;
	u8 cryptedData[0x92];
};

int MacInit(MacContext &ctx, int mode);                                 // sceSdSetIndex
int MacUpdate(MacContext &ctx, const u8 *data, int length);             // sceSdRemoveValue
int MacFinal(MacContext &ctx, u8 *hash, const u8 *key);                 // sceSdGetLastIndex
int CipherInit(CipherContext &ctx, int mode, int genMode, u8 *data, const u8 *key);  // sceSdCreateList
int CipherUpdate(CipherContext &ctx, u8 *data, int alignedLen);         // sceSdSetMember
int CipherFinal(CipherContext &ctx);                                    // sceSdCleanList

}  // namespace chnnlsv
