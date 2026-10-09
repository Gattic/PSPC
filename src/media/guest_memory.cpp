// SPDX-License-Identifier: GPL-2.0-or-later
#include "media/guest_memory.h"

namespace media {

namespace {
GuestPtrFn g_fn = nullptr;
}

void SetGuestMemory(GuestPtrFn fn) { g_fn = fn; }

u8 *GuestPtr(u32 addr, u32 size) { return g_fn ? g_fn(addr, size) : nullptr; }

}  // namespace media
