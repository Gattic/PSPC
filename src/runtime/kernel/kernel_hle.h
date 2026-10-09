// SPDX-License-Identifier: GPL-2.0-or-later
// Helpers shared by the kernel HLE libraries (ThreadManForUser and friends).
#pragma once

#include <algorithm>
#include <cstring>
#include <string>

#include "allegrex/guestmem.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

namespace kernel {

// Call restrictions checked before the function runs, as the firmware does at syscall entry.
enum KernelCallFlags : u32 {
	KF_NONE = 0,
	KF_NOT_IN_INTERRUPT = 1,         // fails with ILLEGAL_CONTEXT in an interrupt handler
	KF_NOT_DISPATCH_SUSPENDED = 2,   // fails with CAN_NOT_WAIT while dispatch or interrupts are off
};

template <auto F, u32 Flags>
void FlagWrap(Ctx *c) {
	if ((Flags & KF_NOT_DISPATCH_SUSPENDED) && !sched::DispatchEnabled()) {
		c->r[2] = SCE_KERNEL_ERROR_CAN_NOT_WAIT;
		return;
	}
	if ((Flags & KF_NOT_IN_INTERRUPT) && sched::InInterrupt()) {
		c->r[2] = SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
		return;
	}
	hle::Wrap<F>(c);
}

// Copies a native status struct to the guest the way the firmware does: the first word of the
// guest buffer is the size it wants; nothing is written for 0, at most sizeof(T) otherwise.
template <typename T>
bool WriteSized(u32 addr, const T &info) {
	if (!mem::IsValidRange(addr, 4)) return false;
	u32 wanted = GRead32(addr);
	if (wanted == 0) return true;
	u32 n = std::min<u32>(wanted, sizeof(T));
	if (!mem::IsValidRange(addr, n)) return false;
	mem::WriteBytes(addr, &info, n);
	return true;
}

inline void CopyName(char *dst, const std::string &name) {
	std::memset(dst, 0, 32);
	std::strncpy(dst, name.c_str(), 31);
}

inline std::string TruncName(const char *name) {
	std::string s = name ? name : "";
	if (s.size() > 31) s.resize(31);
	return s;
}

// Whether the caller runs in kernel mode (a kernel thread, or a thread of a kernel module).
bool CallerIsKernelMode();

// ThreadManForUser is registered in parts (hle/modules/threadman.cpp and kernel_*.cpp).
void RegisterThreadManSync();     // semaphores, event flags, mutexes
void RegisterThreadManTimers();   // alarms, VTimers
void RegisterThreadManMbx();      // message boxes
void RegisterThreadManMsgPipe();  // message pipes
void RegisterThreadManPools();    // VPL, FPL, TLSPL

}  // namespace kernel

#define KFN(nid, func, flags) hle::HleFunction{(u32)(nid), &kernel::FlagWrap<&func, (flags)>, #func, 0}
