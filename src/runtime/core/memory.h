// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "allegrex/guestmem.h"
#include "common/common.h"

namespace mem {

constexpr u32 SCRATCHPAD_BASE = 0x00010000;
constexpr u32 SCRATCHPAD_SIZE = 0x00004000;
constexpr u32 VRAM_BASE = 0x04000000;
constexpr u32 VRAM_SIZE = 0x00200000;
constexpr u32 RAM_BASE = 0x08000000;
constexpr u32 KERNEL_PARTITION_SIZE = 0x00800000;  // 0x08000000 - 0x087FFFFF
constexpr u32 USER_BASE = 0x08800000;

extern u32 g_ramSize;  // 32 MB (PSP-1000) or 64 MB (PSP-2000+)

bool Init(u32 ramSize);
void Shutdown();

inline u32 RamEnd() { return RAM_BASE + g_ramSize; }

bool IsValidAddress(u32 addr);
bool IsValidRange(u32 addr, u32 size);
inline bool IsRam(u32 addr) {
	u32 p = addr & GUEST_ADDR_MASK;
	return p >= RAM_BASE && p < RAM_BASE + g_ramSize;
}

// Host pointer for a validated range, or nullptr.
u8 *GetPointer(u32 addr, u32 size);
// NUL-terminated guest string, or nullptr if it doesn't terminate inside valid memory.
const char *GetCString(u32 addr, u32 maxLen = 0x10000);
std::string ReadString(u32 addr, u32 maxLen = 0x10000);

void Memset(u32 addr, u8 value, u32 size);
void Memcpy(u32 dst, u32 src, u32 size);
void WriteBytes(u32 dst, const void *src, u32 size);
void ReadBytes(void *dst, u32 src, u32 size);

// Number of accesses outside mapped memory so far (each one is logged).
u32 BadAccessCount();

// A second host view of VRAM (the same memory the guest sees), outside the guest window; nullptr when
// VRAM isn't a shared section. A GPU renderer that makes guest VRAM pages inaccessible while it holds
// newer data for them (gpu/hw) does its own accesses through it.
u8 *PrivateVramView();

}  // namespace mem
