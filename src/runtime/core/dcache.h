// SPDX-License-Identifier: GPL-2.0-or-later
// The Allegrex data cache, modelled (--dcache-model, interpreter only): 16 KB, 2-way, 64-byte lines
// in 128 sets, write-back and write-allocate. Off (the default) guest memory is coherent, as games
// expect it to be.
//
// Guest memory as the host holds it stays what a cached access sees; each resident line keeps a
// copy of what memory itself holds, which is what an uncached access (the 0x4-0x7 and 0xA-0xB
// segments) sees while the line is cached. The scratchpad, VRAM and I/O are never cached.
#pragma once

#include <functional>

#include "common/common.h"

namespace dcache {

extern bool g_enabled;

void Reset();
// Before a cached access of RAM at addr: the line is resident afterwards (filled on a miss, the
// least recently used way evicted), and dirty after a store.
void Access(u32 addr, bool store);
// An uncached access to addr: runs `access` with memory's contents where the cached view is, for
// a line that is resident.
void UncachedAccess(u32 addr, const std::function<void()> &access);
// Whether addr goes through the cache (a cached segment of RAM).
bool IsCached(u32 addr);
// Whether addr is RAM through an uncached segment.
bool IsUncachedRam(u32 addr);
// The CACHE instruction's data cache operations (0x14, 0x18, 0x19, 0x1a, 0x1b, 0x1e).
void CacheOp(u32 op, u32 addr);
// sceKernelDcache*: write back and/or invalidate a range, or everything.
void WritebackRange(u32 addr, u32 size, bool invalidate);
void InvalidateRange(u32 addr, u32 size);
void WritebackAll(bool invalidate);

}  // namespace dcache
