// SPDX-License-Identifier: GPL-2.0-or-later
// How the media library reaches PSP memory. The runtime installs an accessor at startup (the
// same idea as audio/guest_memory.h), so the library itself never depends on the runtime.
#pragma once

#include "common/common.h"

namespace media {

// Returns a host pointer to [addr, addr+size) if the whole range is valid guest memory.
using GuestPtrFn = u8 *(*)(u32 addr, u32 size);

void SetGuestMemory(GuestPtrFn fn);
u8 *GuestPtr(u32 addr, u32 size);
inline bool GuestValid(u32 addr, u32 size) { return GuestPtr(addr, size) != nullptr; }

// VRAM (any of its mirrors), where PPSSPP's swizzled-mirror convention applies.
inline bool IsVramAddress(u32 addr) {
	const u32 p = addr & 0x1FFFFFFF;
	return p >= 0x04000000 && p < 0x04800000;
}

}  // namespace media
