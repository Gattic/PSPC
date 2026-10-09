// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest memory access. The host reserves 512 MB and maps the PSP physical address space into
// it; every virtual segment (0x0, 0x4, 0x8, 0xA, 0xC...) folds onto physical memory by masking
// with 0x1FFFFFFF, so cached/uncached/kernel mirrors cost nothing. Unmapped pages fault, and the
// runtime's exception handler reports the guest address.
//
// g_membase is defined once per binary (the runtime executable and each AOT module DLL) and set
// at start-up / registration.
#pragma once

#include "common/common.h"

extern u8 *g_membase;

constexpr u32 GUEST_ADDR_MASK = 0x1FFFFFFF;

PSPC_FORCEINLINE u8 *GPtr(u32 addr) { return g_membase + (addr & GUEST_ADDR_MASK); }

template <typename T>
PSPC_FORCEINLINE T GRead(u32 addr) {
	T v;
	memcpy(&v, GPtr(addr), sizeof(T));
	return v;
}

template <typename T>
PSPC_FORCEINLINE void GWrite(u32 addr, T v) {
	memcpy(GPtr(addr), &v, sizeof(T));
}

PSPC_FORCEINLINE u8 GRead8(u32 a) { return *GPtr(a); }
PSPC_FORCEINLINE u16 GRead16(u32 a) { return GRead<u16>(a); }
PSPC_FORCEINLINE u32 GRead32(u32 a) { return GRead<u32>(a); }
PSPC_FORCEINLINE void GWrite8(u32 a, u8 v) { *GPtr(a) = v; }
PSPC_FORCEINLINE void GWrite16(u32 a, u16 v) { GWrite<u16>(a, v); }
PSPC_FORCEINLINE void GWrite32(u32 a, u32 v) { GWrite<u32>(a, v); }
