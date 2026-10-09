// SPDX-License-Identifier: GPL-3.0-or-later
// Plain DES (FIPS 46-3), for the file lists inside a firmware updater's PSAR.
// From PPSSPP's Core/Util/PSARUnpack.cpp (GPL-2.0-or-later); the encrypt direction is added for tests.
#pragma once

#include "common.h"

class DES {
public:
	explicit DES(const u8 key[8]);
	void EncryptBlock(const u8 in[8], u8 out[8]) const;
	void DecryptBlock(const u8 in[8], u8 out[8]) const;
	// CBC over whole 8-byte blocks; a trailing partial block is left as is.
	void CbcDecrypt(u8 *data, size_t size, const u8 iv[8]) const;
	void CbcEncrypt(u8 *data, size_t size, const u8 iv[8]) const;

private:
	void Crypt(const u8 in[8], u8 out[8], bool decrypt) const;
	u8 subkeys_[16][48];
};
