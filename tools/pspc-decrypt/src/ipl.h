// SPDX-License-Identifier: GPL-3.0-or-later
// IPL stage 1: the IPL in an updater is a chain of 0x1000-byte KIRK CMD1 blocks, each carrying a
// load address and a piece of the loader. Decrypting and linearizing them gives the first stage.
// Logic from John-K/pspdecrypt's pspdecrypt_lib.cpp (GPL-3.0; descrambling by Davee and Proxima).
#pragma once

#include "common.h"

struct IplStage1 {
	Bytes image;           // Linear image starting at loadAddress.
	u32 loadAddress = 0;
	u32 entry = 0;         // From the block that carries one (the last), 0 if none did.
	int blocks = 0;
	int descrambleKey = -1;  // 03g+ IPLs: which XOR key index unscrambled them.
	bool preDecrypted = false;  // The whole file was a tag-encrypted blob around the blocks.
	std::string note;
};

// Returns false if not even the first block decrypts.
bool DecryptIplStage1(const u8 *data, size_t size, IplStage1 *out, std::string *error);
