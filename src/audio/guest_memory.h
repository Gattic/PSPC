// SPDX-License-Identifier: GPL-2.0-or-later
// How the audio library reaches PSP memory. The runtime installs an accessor at startup; unit
// tests install their own over a plain buffer.
#pragma once

#include "common/common.h"

namespace audio {

// Returns a host pointer to [addr, addr+size) if the whole range is valid guest memory.
using GuestPtrFn = u8 *(*)(u32 addr, u32 size);

void SetGuestMemory(GuestPtrFn fn);
u8 *GuestPtr(u32 addr, u32 size);
inline bool GuestValid(u32 addr, u32 size) { return GuestPtr(addr, size) != nullptr; }

}  // namespace audio
