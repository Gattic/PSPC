// SPDX-License-Identifier: GPL-2.0-or-later
//
// Savedata encryption and PARAM.SFO hashes as a PSP makes them (sceChnnlsv on the KIRK service).
// Ported from PPSSPP (Core/Dialog/SavedataParam.cpp: EncryptData, DecryptData, UpdateHash,
// BuildHash), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
//
// File layout of an encrypted secure file: a 16-byte IV, then the data encrypted with a stream
// cipher (so the file is the data's size + 16). Its MAC goes in PARAM.SFO's SAVEDATA_FILE_LIST.
// SAVEDATA_PARAMS (0x80 bytes) holds the PARAM.SFO hashes: byte 0 = 0x01 (+ 0x20 / 0x40 for the
// newer hash modes), +0x10 the mode 1 hash ("11C0"), +0x20 the console-keyed hash ("11D0"),
// +0x70 the mode 3/5 hash ("1220"). The XMB's Saved Data Utility checks the +0x10 hash of every
// save it lists.
#pragma once

#include <vector>

#include "common/common.h"

namespace savecrypto {

// True when the KIRK service is there (otherwise saves stay in the clear).
bool Available();

// Encrypts `size` bytes with `mode` (1, 3 or 5; `key` is the game's 16-byte key for 3 and 5).
// out = IV + ciphertext (size + 16 bytes), hash = the data's MAC for SAVEDATA_FILE_LIST.
bool Encrypt(int mode, const u8 *key, const u8 *data, size_t size, std::vector<u8> &out, u8 hash[16]);

// Decrypts a file made by Encrypt (or a PSP). out = the plain data (file size - 16 bytes),
// hash = its MAC, to compare with SAVEDATA_FILE_LIST.
bool Decrypt(int mode, const u8 *key, const std::vector<u8> &file, std::vector<u8> &out, u8 hash[16]);

// Fills the SAVEDATA_PARAMS of a serialized PARAM.SFO (`sfo`, `paramsOffset` = where its 0x80
// bytes of data start) with the hashes for `encryptMode` (1, 3 or 5). The 16 bytes at +0x40 are
// kept (and hashed) when keep0x40 is set (PSPC's marker for saves it stored in the clear).
bool UpdateHash(std::vector<u8> &sfo, size_t paramsOffset, int encryptMode, bool keep0x40 = false);

// Where the data of `key` starts in a serialized PARAM.SFO; SIZE_MAX if it isn't there.
size_t SfoValueOffset(const std::vector<u8> &sfo, const char *key);

// Saves in ms0:/PSP/SAVEDATA without PARAM.SFO hashes (made by older PSPC builds, or by PPSSPP
// without encryption) show as corrupted in the XMB's Saved Data Utility. Called when the XMB
// boots: adds the hashes without changing how the saves read (PSPC's in-the-clear secure saves
// keep their marker; unencrypted ones only get the 0x10 hash and stay flagged unencrypted).
// Returns how many saves were updated.
int AddMissingSfoHashes();

}  // namespace savecrypto
