// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest CPU context. This layout is part of the AOT ABI: recompiled module DLLs are compiled
// against it, so any change here must bump PSPC_AOT_ABI_VERSION in aot_abi.h.
#pragma once

#include "common/common.h"

// VFPU control register indices (vfpuCtrl[]).
enum VfpuCtrl : int {
	VFPU_CTRL_SPREFIX = 0,
	VFPU_CTRL_TPREFIX = 1,
	VFPU_CTRL_DPREFIX = 2,
	VFPU_CTRL_CC = 3,
	VFPU_CTRL_INF4 = 4,
	VFPU_CTRL_RSV5 = 5,
	VFPU_CTRL_RSV6 = 6,
	VFPU_CTRL_REV = 7,
	VFPU_CTRL_RCX0 = 8,  // ..RCX7 = 15: random number generator state
};

struct alignas(64) Ctx {
	u32 r[32];
	u32 hi;
	u32 lo;
	u32 pc;        // Guest PC. Kept up to date by the interpreter; set by dispatch on indirect flow.
	u32 fcr31;     // FPU control/status. Bit 23 mirrors fpcond when read via cfc1.
	u32 fpcond;    // FPU condition flag (0/1).
	u32 llbit;
	s32 budget;    // Decremented on backward branches; <0 => yield check.
	u32 unwinding; // Non-zero while a non-local return (e.g. longjmp) propagates; target in pc.

	union {
		float f[32];
		u32 fi[32];
	};
	// VFPU register file, 128 singles. Indexing follows PPSSPP's convention:
	// v[voffset[reg]] where reg is the 7-bit VFPU register number (see vfpu_regs.h).
	union {
		float v[128];
		u32 vi[128];
	};
	u32 vfpuCtrl[16];

	// Runtime bookkeeping (not touched by recompiled code except via the API).
	void *thread;  // Owning kernel thread object.
	u32 interruptsEnabled;
	u32 inInterrupt;
	u32 pad[14];
};

static_assert(sizeof(Ctx) % 64 == 0, "Ctx must stay cache-line sized");
