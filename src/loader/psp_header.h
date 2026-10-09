// SPDX-License-Identifier: GPL-2.0-or-later
// "~PSP" executable container header.
#pragma once

#include <string>

#include "common/common.h"

namespace loader {

enum class ContainerKind {
	PlainElf,     // starts with \x7FELF
	PackedGzip,   // ~PSP whose payload is a gzip stream (M33/OE psp-packer): no keys needed
	Encrypted,    // ~PSP KIRK-encrypted: needs pspc-decrypt
	Unknown,
};

struct PspHeader {
	u16 modAttr = 0;
	u16 compAttr = 0;
	u8 moduleVerLo = 0, moduleVerHi = 0;
	std::string modName;
	u8 version = 0, nsegments = 0;
	u32 elfSize = 0;
	u32 pspSize = 0;
	u32 entry = 0;
	u32 modinfoOffset = 0;
	u32 bssSize = 0;
	u32 devkitVersion = 0;
	u32 decryptMode = 0;
	u32 compSize = 0;
	u32 tag = 0;
	u32 oeTag = 0;
};

constexpr u32 PSP_HEADER_SIZE = 0x150;

ContainerKind Classify(const u8 *data, size_t size);
bool ParsePspHeader(const u8 *data, size_t size, PspHeader &out);

}  // namespace loader
