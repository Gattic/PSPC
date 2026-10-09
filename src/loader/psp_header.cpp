// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/psp_header.h"

namespace loader {

static u32 Rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 Rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

ContainerKind Classify(const u8 *data, size_t size) {
	if (size >= 4 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
		return ContainerKind::PlainElf;
	if (size >= PSP_HEADER_SIZE && data[0] == '~' && data[1] == 'P' && data[2] == 'S' && data[3] == 'P') {
		if (size >= PSP_HEADER_SIZE + 2 && data[PSP_HEADER_SIZE] == 0x1F && data[PSP_HEADER_SIZE + 1] == 0x8B)
			return ContainerKind::PackedGzip;
		return ContainerKind::Encrypted;
	}
	return ContainerKind::Unknown;
}

bool ParsePspHeader(const u8 *d, size_t size, PspHeader &h) {
	if (size < PSP_HEADER_SIZE || memcmp(d, "~PSP", 4) != 0) return false;
	h.modAttr = Rd16(d + 0x04);
	h.compAttr = Rd16(d + 0x06);
	h.moduleVerLo = d[0x08];
	h.moduleVerHi = d[0x09];
	h.modName.assign((const char *)d + 0x0A, strnlen((const char *)d + 0x0A, 28));
	h.version = d[0x26];
	h.nsegments = d[0x27];
	h.elfSize = Rd32(d + 0x28);
	h.pspSize = Rd32(d + 0x2C);
	h.entry = Rd32(d + 0x30);
	h.modinfoOffset = Rd32(d + 0x34);
	h.bssSize = Rd32(d + 0x38);
	h.devkitVersion = Rd32(d + 0x78);
	h.decryptMode = d[0x7C];
	h.compSize = Rd32(d + 0xB0);
	h.tag = Rd32(d + 0xD0);
	h.oeTag = Rd32(d + 0x130);
	return true;
}

}  // namespace loader
