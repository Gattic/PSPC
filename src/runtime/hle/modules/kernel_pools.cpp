// SPDX-License-Identifier: GPL-2.0-or-later
//
// ThreadManForUser memory pools: variable (VPL), fixed (FPL) and thread-local storage (TLSPL),
// plus Kernel_Library sceKernelGetTlsAddr. The VPL keeps its block list in guest memory in the
// firmware's layout (tests look at it). Semantics follow PPSSPP's sceKernelMemory.cpp (Portions
// derived from PPSSPP, https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later).
#include <algorithm>
#include <map>
#include <unordered_map>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/waitqueue.h"

using namespace kernel;

namespace {

bool Valid4(u32 addr) { return (addr & 3) == 0 && mem::IsValidRange(addr, 4); }

// Same check as the firmware for blockSize * count (results from a PSP).
bool IllegalPoolSize(u32 blockSize, u32 count) {
	if (blockSize == 0 || count == 0) return true;
	if ((u64)blockSize > ((0x100000000ULL / (u64)count) - 4ULL)) return true;
	if ((u64)count >= 0x100000000ULL / (((u64)blockSize + 3ULL) & ~3ULL)) return true;
	return false;
}

void WakeDeleted(PspThread *w) {
	if (w->waitTimeoutPtr && mem::IsValidRange(w->waitTimeoutPtr, 4)) GWrite32(w->waitTimeoutPtr, 0);
	w->waitTimeoutPtr = 0;
	sched::Wake(w, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
}

// ================================================================ FPL

constexpr u32 PSP_FPL_ATTR_PRIORITY = 0x0100;
constexpr u32 PSP_FPL_ATTR_HIGHMEM = 0x4000;
constexpr u32 PSP_FPL_ATTR_KNOWN = PSP_FPL_ATTR_PRIORITY | PSP_FPL_ATTR_HIGHMEM;

struct Fpl : KObject {
	static constexpr KType kType = KType::Fpl;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 blockSize = 0, numBlocks = 0;
	std::vector<bool> blocks;
	u32 address = 0;
	u32 alignedSize = 0;
	int nextBlock = 0;
	WaitQueue waiters;  // waitValue[0] = where to store the block address

	~Fpl() override {
		if (address) {
			if (BlockAllocator *a = AllocatorForAddress(address)) a->Free(address);
		}
	}

	int AllocateBlock() {
		for (int i = 0; i < numBlocks; i++) {
			int b = nextBlock++ % numBlocks;
			if (!blocks[b]) {
				blocks[b] = true;
				return b;
			}
		}
		return -1;
	}
	u32 BlockAddr(int b) const { return address + alignedSize * (u32)b; }
	void Sort() {
		waiters.Cleanup(WAITTYPE_FPL, uid);
		if (attr & PSP_FPL_ATTR_PRIORITY) waiters.SortByPriority();
	}
};

bool FplUnlockFor(Fpl *f, PspThread *t) {
	int b = f->AllocateBlock();
	if (b < 0) return false;
	if (Valid4(t->waitValue[0])) GWrite32(t->waitValue[0], f->BlockAddr(b));
	sched::Wake(t, 0);
	return true;
}

int sceKernelCreateFpl(const char *name, u32 mpid, u32 attr, u32 blockSize, u32 numBlocks, u32 optPtr) {
	if (!name) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	if (mpid < 1 || mpid > 6) return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	BlockAllocator *alloc = AllocatorForPartitionId((int)mpid, CallerIsKernelMode());
	if (!alloc) return (int)SCE_KERNEL_ERROR_ILLEGAL_PERM;
	if (((attr & ~PSP_FPL_ATTR_KNOWN) & ~0xFF) != 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (IllegalPoolSize(blockSize, numBlocks)) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
	u32 alignment = 4;
	if (mem::IsValidRange(optPtr, 8)) {
		u32 size = GRead32(optPtr);
		if (size >= 4) alignment = GRead32(optPtr + 4);
		// Must be a power of 2.
		if ((alignment & (alignment - 1)) != 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	}
	if (alignment < 4) alignment = 4;
	u32 alignedSize = (blockSize + alignment - 1) & ~(alignment - 1);
	if ((u64)alignedSize * (u64)numBlocks > 0xFFFFFFFFULL) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
	u32 total = alignedSize * numBlocks;
	u32 address = alloc->Alloc(total, (attr & PSP_FPL_ATTR_HIGHMEM) != 0, ("FPL/" + TruncName(name)).c_str());
	if (!address) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	auto *f = new Fpl();
	f->name = TruncName(name);
	f->attr = attr;
	f->blockSize = (s32)blockSize;
	f->numBlocks = (s32)numBlocks;
	f->blocks.assign(numBlocks, false);
	f->address = address;
	f->alignedSize = alignedSize;
	return Register(f);
}

int sceKernelDeleteFpl(SceUID id) {
	sched::ChargeCycles(600);
	Fpl *f = Get<Fpl>(id);
	if (!f) return (int)SCE_KERNEL_ERROR_UNKNOWN_FPLID;
	bool woke = f->waiters.WakeAll(WAITTYPE_FPL, id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

int AllocateFpl(SceUID id, u32 blockPtrAddr, u32 timeoutPtr, bool cb) {
	Fpl *f = Get<Fpl>(id);
	if (!f) return (int)SCE_KERNEL_ERROR_UNKNOWN_FPLID;
	int b = f->AllocateBlock();
	if (b >= 0) {
		if (Valid4(blockPtrAddr)) GWrite32(blockPtrAddr, f->BlockAddr(b));
		return 0;
	}
	PspThread *t = sched::Current();
	f->waiters.Remove(t->uid);
	if (sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	t->waitValue[0] = blockPtrAddr;
	f->waiters.threads.push_back(t->uid);
	SceUID me = t->uid;
	WaitHooks hooks;
	hooks.pause = [id, me]() {
		if (Fpl *fp = Get<Fpl>(id)) fp->waiters.Remove(me);
	};
	hooks.resume = [id, me]() {
		PspThread *w = Get<PspThread>(me);
		Fpl *fp = Get<Fpl>(id);
		if (!fp) {
			WakeDeleted(w);
			return true;
		}
		if (FplUnlockFor(fp, w)) return true;
		fp->waiters.threads.push_back(me);
		return false;
	};
	hooks.timeout = [id, me]() {
		if (Fpl *fp = Get<Fpl>(id)) fp->waiters.Remove(me);
	};
	return sched::Wait(WAITTYPE_FPL, id, timeoutPtr, cb, &hooks);
}

int sceKernelAllocateFpl(SceUID id, u32 blockPtrAddr, u32 timeoutPtr) { return AllocateFpl(id, blockPtrAddr, timeoutPtr, false); }
int sceKernelAllocateFplCB(SceUID id, u32 blockPtrAddr, u32 timeoutPtr) { return AllocateFpl(id, blockPtrAddr, timeoutPtr, true); }

int sceKernelTryAllocateFpl(SceUID id, u32 blockPtrAddr) {
	Fpl *f = Get<Fpl>(id);
	if (!f) return (int)SCE_KERNEL_ERROR_UNKNOWN_FPLID;
	int b = f->AllocateBlock();
	if (b < 0) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	if (Valid4(blockPtrAddr)) GWrite32(blockPtrAddr, f->BlockAddr(b));
	return 0;
}

int sceKernelFreeFpl(SceUID id, u32 blockPtr) {
	if (blockPtr > g_userMem.Start() + g_userMem.Size()) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	Fpl *f = Get<Fpl>(id);
	if (!f) return (int)SCE_KERNEL_ERROR_UNKNOWN_FPLID;
	int b = (int)((s32)(blockPtr - f->address) / (s32)f->alignedSize);
	if ((s32)(blockPtr - f->address) < 0 || b < 0 || b >= f->numBlocks) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK;
	if (!f->blocks[b]) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK;
	f->blocks[b] = false;
	f->Sort();
	bool woke = false;
	for (size_t i = 0; i < f->waiters.threads.size();) {
		PspThread *t = Get<PspThread>(f->waiters.threads[i]);
		if (t && FplUnlockFor(f, t)) {
			f->waiters.threads.erase(f->waiters.threads.begin() + i);
			woke = true;
			i = 0;
			continue;
		}
		i++;
	}
	if (woke) sched::Reschedule();
	return 0;
}

int sceKernelCancelFpl(SceUID id, u32 numWaitPtr) {
	sched::ChargeCycles(600);
	Fpl *f = Get<Fpl>(id);
	if (!f) return (int)SCE_KERNEL_ERROR_UNKNOWN_FPLID;
	int n = f->waiters.Count(WAITTYPE_FPL, id);
	if (Valid4(numWaitPtr)) GWrite32(numWaitPtr, (u32)n);
	if (f->waiters.WakeAll(WAITTYPE_FPL, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL)) sched::Reschedule();
	return 0;
}

int sceKernelReferFplStatus(SceUID id, u32 statusPtr) {
	Fpl *f = Get<Fpl>(id);
	if (!f) return (int)SCE_KERNEL_ERROR_UNKNOWN_FPLID;
	f->Sort();
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 blockSize, numBlocks, numFreeBlocks, numWaitThreads;
	} info{};
	if (mem::IsValidRange(statusPtr, sizeof(info)) && GRead32(statusPtr) != 0) {
		info.size = sizeof(info);
		CopyName(info.name, f->name);
		info.attr = f->attr;
		info.blockSize = f->blockSize;
		info.numBlocks = f->numBlocks;
		info.numFreeBlocks = (s32)std::count(f->blocks.begin(), f->blocks.end(), false);
		info.numWaitThreads = (s32)f->waiters.threads.size();
		mem::WriteBytes(statusPtr, &info, sizeof(info));
	}
	return 0;
}

// ================================================================ VPL

constexpr u32 PSP_VPL_ATTR_PRIORITY = 0x0100;
constexpr u32 PSP_VPL_ATTR_SMALLEST = 0x0200;
constexpr u32 PSP_VPL_ATTR_MASK_ORDER = 0x0300;
constexpr u32 PSP_VPL_ATTR_HIGHMEM = 0x4000;
constexpr u32 PSP_VPL_ATTR_KNOWN = PSP_VPL_ATTR_PRIORITY | PSP_VPL_ATTR_SMALLEST | PSP_VPL_ATTR_HIGHMEM;

// The firmware's VPL header, in guest memory at the start of the pool:
//   +0 startPtr, +4 startPtr2, +8 sentinel, +C sizeMinus8, +10 allocatedInBlocks,
//   +14 nextFreeBlock, +18 first block {next, sizeInBlocks}.
// Blocks are 8-byte units; each starts with {next, sizeInBlocks}. Allocated blocks point to the
// sentinel (start + 8); free ones form a circular list through the last (size 0) block.
struct VplHeader {
	u32 base;

	u32 Rd(u32 off) const { return GRead32(base + off); }
	void Wr(u32 off, u32 v) const { GWrite32(base + off, v); }
	u32 StartPtr() const { return Rd(0); }
	u32 SizeMinus8() const { return Rd(0xC); }
	u32 Allocated() const { return Rd(0x10); }
	u32 NextFree() const { return Rd(0x14); }
	u32 FirstBlockPtr() const { return StartPtr() + 0x18; }
	u32 LastBlockPtr() const { return StartPtr() + SizeMinus8(); }
	u32 SentinelPtr() const { return StartPtr() + 8; }
	static u32 Next(u32 b) { return GRead32(b); }
	static u32 Size(u32 b) { return GRead32(b + 4); }
	static void SetNext(u32 b, u32 v) { GWrite32(b, v); }
	static void SetSize(u32 b, u32 v) { GWrite32(b + 4, v); }

	void Init(u32 ptr, u32 size) const {
		Wr(0, ptr);
		Wr(4, ptr);
		Wr(8, ptr + 7);
		Wr(0xC, size - 8);
		Wr(0x10, 0);
		Wr(0x14, FirstBlockPtr());
		SetNext(FirstBlockPtr(), LastBlockPtr());
		// Includes its own header, which is one block.
		SetSize(FirstBlockPtr(), (size - 0x28) / 8 + 1);
		SetNext(LastBlockPtr(), FirstBlockPtr());
		SetSize(LastBlockPtr(), 0);
	}

	u32 FreeSize() const { return SizeMinus8() + 8 - 0x20 - Allocated() * 8; }

	bool ValidBlock(u32 b) const { return mem::IsValidRange(b, 8) && (b & 3) == 0; }

	u32 Split(u32 b, u32 allocBlocks) const {
		u32 prev = b;
		SetSize(b, Size(b) - allocBlocks);
		b += Size(b) * 8;
		SetSize(b, allocBlocks);
		SetNext(b, prev);
		return b;
	}
	void Unlink(u32 b, u32 prev) const {
		Wr(0x10, Allocated() + Size(b));
		SetNext(prev, Next(b));
		Wr(0x14, prev);
		SetNext(b, SentinelPtr());
	}
	u32 Merge(u32 first, u32 second) const {
		SetSize(first, Size(first) + Size(second));
		SetNext(first, Next(second));
		return first;
	}
	void Link(u32 b, u32 prev, u32 next) const {
		Wr(0x10, Allocated() - Size(b));
		Wr(0x14, prev);
		// Erase the magic so it isn't considered allocated later.
		SetNext(b, next);
		u32 afterB = b + Size(b) * 8;
		if (afterB == next && Size(next) != 0) b = Merge(b, next);
		u32 afterPrev = prev + Size(prev) * 8;
		if (afterPrev == b) b = Merge(prev, b);
		else SetNext(prev, b);
	}

	// Returns the address, or 0.
	u32 Allocate(u32 size) const {
		u32 allocBlocks = ((size + 7) / 8) + 1;
		u32 prev = NextFree();
		if (!ValidBlock(prev)) return 0;
		u32 start = prev;
		int guard = 0;
		do {
			u32 b = Next(prev);
			if (!ValidBlock(b)) return 0;
			if (Size(b) > allocBlocks) b = Split(b, allocBlocks);
			if (Size(b) == allocBlocks) {
				Unlink(b, prev);
				return b + 8;
			}
			prev = b;
		} while (ValidBlock(prev) && prev != start && ++guard < 0x100000);
		return 0;
	}

	bool Free(u32 ptr) const {
		u32 b = ptr - 8;
		// In range? It can't be the last block, which is always 0 sized.
		if (!ValidBlock(b) || ptr < FirstBlockPtr() || ptr >= LastBlockPtr()) return false;
		// Check the magic.
		if (Next(b) != SentinelPtr() || Size(b) > Allocated()) return false;
		u32 last = LastBlockPtr();
		u32 prev = last;
		int guard = 0;
		do {
			u32 next = Next(prev);
			if (next == b) return false;  // already free
			if (next > b) {
				Link(b, prev, next);
				return true;
			}
			prev = next;
		} while (ValidBlock(prev) && prev != last && ++guard < 0x100000);
		return false;
	}
};

struct Vpl : KObject {
	static constexpr KType kType = KType::Vpl;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 poolSize = 0;
	u32 block = 0;  // the partition block; the header lives at its start
	WaitQueue waiters;  // waitValue[0] = size, [1] = where to store the address

	~Vpl() override {
		if (block) {
			if (BlockAllocator *a = AllocatorForAddress(block)) a->Free(block);
		}
	}
	VplHeader Header() const { return VplHeader{block}; }
	void Sort() {
		waiters.Cleanup(WAITTYPE_VPL, uid);
		if (attr & PSP_VPL_ATTR_PRIORITY) waiters.SortByPriority();
	}
	bool Fifo() const { return (attr & PSP_VPL_ATTR_MASK_ORDER) == 0; }
};

bool VplUnlockFor(Vpl *v, PspThread *t) {
	u32 addr = v->Header().Allocate(t->waitValue[0]);
	if (!addr) return false;
	if (Valid4(t->waitValue[1])) GWrite32(t->waitValue[1], addr);
	sched::Wake(t, 0);
	return true;
}

// Wakes waiters that can now allocate; in FIFO mode, stops at the first that can't.
bool VplWakeWaiters(Vpl *v) {
	v->Sort();
	bool woke = false;
	for (size_t i = 0; i < v->waiters.threads.size();) {
		PspThread *t = Get<PspThread>(v->waiters.threads[i]);
		if (t && VplUnlockFor(v, t)) {
			v->waiters.threads.erase(v->waiters.threads.begin() + i);
			woke = true;
			i = 0;
			continue;
		}
		if (v->Fifo()) break;
		i++;
	}
	return woke;
}

SceUID sceKernelCreateVpl(const char *name, int partition, u32 attr, u32 vplSize, u32 optPtr) {
	if (!name) return (SceUID)SCE_KERNEL_ERROR_ERROR;
	if (partition < 1 || partition > 6) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	BlockAllocator *alloc = AllocatorForPartitionId(partition, CallerIsKernelMode());
	if (!alloc) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_PERM;
	if (((attr & ~PSP_VPL_ATTR_KNOWN) & ~0xFF) != 0) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (vplSize == 0) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
	if (vplSize >= 0x80000000) return (SceUID)SCE_KERNEL_ERROR_NO_MEMORY;
	// Can't have that little space in a VPL.
	if (vplSize <= 0x30) vplSize = 0x1000;
	vplSize = (vplSize + 7) & ~7u;
	u32 block = alloc->Alloc(vplSize, (attr & PSP_VPL_ATTR_HIGHMEM) != 0, ("VPL/" + TruncName(name)).c_str());
	if (!block) return (SceUID)SCE_KERNEL_ERROR_NO_MEMORY;
	(void)optPtr;
	auto *v = new Vpl();
	v->name = TruncName(name);
	v->attr = attr;
	v->poolSize = (s32)vplSize - 0x20;
	v->block = block;
	v->Header().Init(block, vplSize);
	return Register(v);
}

int sceKernelDeleteVpl(SceUID id) {
	Vpl *v = Get<Vpl>(id);
	if (!v) return (int)SCE_KERNEL_ERROR_UNKNOWN_VPLID;
	bool woke = v->waiters.WakeAll(WAITTYPE_VPL, id, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	Destroy(id);
	if (woke) sched::Reschedule();
	return 0;
}

// Returns false for invalid parameters; *error is 0 on success, NO_MEMORY if it must wait.
bool TryAllocateVpl(SceUID id, u32 size, u32 addrPtr, u32 *error, bool trying) {
	Vpl *v = Get<Vpl>(id);
	if (!v) {
		*error = SCE_KERNEL_ERROR_UNKNOWN_VPLID;
		return false;
	}
	if (size == 0 || size > (u32)v->poolSize) {
		*error = SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
		return false;
	}
	// For some reason, try doesn't follow the same rules.
	if (!trying && v->Fifo()) {
		v->Sort();
		if (!v->waiters.threads.empty()) {
			*error = SCE_KERNEL_ERROR_NO_MEMORY;
			return true;
		}
	}
	u32 addr = v->Header().Allocate(size);
	if (addr) {
		if (Valid4(addrPtr)) GWrite32(addrPtr, addr);
		*error = 0;
	} else {
		*error = SCE_KERNEL_ERROR_NO_MEMORY;
	}
	return true;
}

int AllocateVpl(SceUID id, u32 size, u32 addrPtr, u32 timeoutPtr, bool cb) {
	u32 error = 0;
	if (!TryAllocateVpl(id, size, addrPtr, &error, false)) return (int)error;
	PspThread *t = sched::Current();
	Vpl *v = Get<Vpl>(id);
	if (error == SCE_KERNEL_ERROR_NO_MEMORY) {
		// Allocating does more before setting up the timeout, so short ones time out at once
		// more often.
		if (sched::WaitTimesOutAtOnce(timeoutPtr, 117, 17)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
		if (!v) return (int)SCE_KERNEL_ERROR_UNKNOWN_VPLID;
		v->waiters.Remove(t->uid);
		t->waitValue[0] = size;
		t->waitValue[1] = addrPtr;
		v->waiters.threads.push_back(t->uid);
		SceUID me = t->uid;
		WaitHooks hooks;
		hooks.pause = [id, me]() {
			if (Vpl *vp = Get<Vpl>(id)) vp->waiters.Remove(me);
		};
		hooks.resume = [id, me]() {
			PspThread *w = Get<PspThread>(me);
			Vpl *vp = Get<Vpl>(id);
			if (!vp) {
				WakeDeleted(w);
				return true;
			}
			if (VplUnlockFor(vp, w)) return true;
			vp->waiters.threads.push_back(me);
			return false;
		};
		hooks.timeout = [id, me]() {
			Vpl *vp = Get<Vpl>(id);
			if (!vp) return;
			vp->waiters.Remove(me);
			// In FIFO mode, that may have cleared others to allocate.
			if (vp->Fifo()) VplWakeWaiters(vp);
		};
		return sched::Wait(WAITTYPE_VPL, id, timeoutPtr, cb, &hooks);
	}
	// If anyone else was waiting, the allocation takes a moment.
	if (error == 0 && v && v->waiters.Count(WAITTYPE_VPL, id) > 0) sched::ChargeUs(50);
	if (cb) RunPendingCallbacks(t);
	return (int)error;
}

int sceKernelAllocateVpl(SceUID id, u32 size, u32 addrPtr, u32 timeoutPtr) { return AllocateVpl(id, size, addrPtr, timeoutPtr, false); }
int sceKernelAllocateVplCB(SceUID id, u32 size, u32 addrPtr, u32 timeoutPtr) { return AllocateVpl(id, size, addrPtr, timeoutPtr, true); }

int sceKernelTryAllocateVpl(SceUID id, u32 size, u32 addrPtr) {
	u32 error = 0;
	TryAllocateVpl(id, size, addrPtr, &error, true);
	return (int)error;
}

int sceKernelFreeVpl(SceUID id, u32 addr) {
	if (addr && !mem::IsValidAddress(addr)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	Vpl *v = Get<Vpl>(id);
	if (!v) return (int)SCE_KERNEL_ERROR_UNKNOWN_VPLID;
	if (!v->Header().Free(addr)) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK;
	if (VplWakeWaiters(v)) sched::Reschedule();
	return 0;
}

int sceKernelCancelVpl(SceUID id, u32 numWaitPtr) {
	Vpl *v = Get<Vpl>(id);
	if (!v) return (int)SCE_KERNEL_ERROR_UNKNOWN_VPLID;
	int n = v->waiters.Count(WAITTYPE_VPL, id);
	if (Valid4(numWaitPtr)) GWrite32(numWaitPtr, (u32)n);
	if (v->waiters.WakeAll(WAITTYPE_VPL, id, (s32)SCE_KERNEL_ERROR_WAIT_CANCEL)) sched::Reschedule();
	return 0;
}

int sceKernelReferVplStatus(SceUID id, u32 infoPtr) {
	Vpl *v = Get<Vpl>(id);
	if (!v) return (int)SCE_KERNEL_ERROR_UNKNOWN_VPLID;
	v->Sort();
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 poolSize, freeSize, numWaitThreads;
	} info{};
	if (mem::IsValidRange(infoPtr, sizeof(info)) && GRead32(infoPtr) != 0) {
		info.size = sizeof(info);
		CopyName(info.name, v->name);
		info.attr = v->attr;
		info.poolSize = v->poolSize;
		info.freeSize = (s32)v->Header().FreeSize();
		info.numWaitThreads = (s32)v->waiters.threads.size();
		mem::WriteBytes(infoPtr, &info, sizeof(info));
	}
	return 0;
}

// ================================================================ TLSPL

constexpr u32 PSP_TLSPL_ATTR_PRIORITY = 0x0100;
constexpr u32 PSP_TLSPL_ATTR_HIGHMEM = 0x4000;
constexpr u32 PSP_TLSPL_ATTR_KNOWN = PSP_TLSPL_ATTR_HIGHMEM | PSP_TLSPL_ATTR_PRIORITY;
constexpr int TLSPL_NUM_INDEXES = 16;
// sceKernelGetTlsAddr waits with this as its "address pointer": the address is the return value.
constexpr u32 TLSPL_WAITVALUE_RETURN_ADDR = 1;

struct Tlspl : KObject {
	static constexpr KType kType = KType::Tls;
	KType Type() const override { return kType; }
	u32 attr = 0;
	s32 index = 0;
	u32 blockSize = 0, totalBlocks = 0, freeBlocks = 0;
	u32 address = 0, alignment = 4;
	int next = 0;
	std::vector<SceUID> usage;
	WaitQueue waiters;  // waitValue[0] = address pointer, or TLSPL_WAITVALUE_RETURN_ADDR

	~Tlspl() override {
		if (address) {
			if (BlockAllocator *a = AllocatorForAddress(address)) a->Free(address);
		}
	}
	u32 AlignedSize() const { return (blockSize + alignment - 1) & ~(alignment - 1); }
	void Sort() {
		waiters.Cleanup(WAITTYPE_TLSPL, uid);
		if (attr & PSP_TLSPL_ATTR_PRIORITY) waiters.SortByPriority();
	}
};

bool g_tlsplUsedIndexes[TLSPL_NUM_INDEXES];
std::unordered_multimap<SceUID, SceUID> g_tlsplHeld;  // thread -> pools it has a block of

void TlsHeldErase(SceUID thread, SceUID pool) {
	auto range = g_tlsplHeld.equal_range(thread);
	for (auto it = range.first; it != range.second; ++it) {
		if (it->second == pool) {
			g_tlsplHeld.erase(it);
			return;
		}
	}
}

int FreeTls(Tlspl *tls, SceUID threadId) {
	int freeBlock = -1;
	for (u32 i = 0; i < tls->totalBlocks; ++i) {
		if (tls->usage[i] == threadId) {
			freeBlock = (int)i;
			break;
		}
	}
	// "OK" even though nothing was freed.
	if (freeBlock == -1) return 0;
	SceUID uid = tls->uid;
	u32 freedAddress = tls->address + (u32)freeBlock * tls->AlignedSize();
	// Whenever freeing a block, clear it (even if it's not going to wake anyone).
	mem::Memset(freedAddress, 0, tls->blockSize);
	TlsHeldErase(threadId, uid);
	tls->Sort();
	while (!tls->waiters.threads.empty()) {
		SceUID w = tls->waiters.threads.front();
		tls->waiters.threads.erase(tls->waiters.threads.begin());
		PspThread *t = Get<PspThread>(w);
		if (!WaitQueue::IsWaitingOn(t, WAITTYPE_TLSPL, uid)) continue;
		// The pool was full, so this newly freed block is theirs.
		tls->usage[freeBlock] = w;
		u32 addrPtr = t->waitValue[0];
		if (addrPtr != TLSPL_WAITVALUE_RETURN_ADDR) {
			if (Valid4(addrPtr)) GWrite32(addrPtr, freedAddress);
			sched::Wake(t, 0);
		} else {
			sched::Wake(t, (s32)freedAddress);
		}
		g_tlsplHeld.emplace(w, uid);
		return 0;
	}
	tls->usage[freeBlock] = 0;
	++tls->freeBlocks;
	return 0;
}

void TlsplThreadEnd(PspThread *t) {
	if (t->waitType == WAITTYPE_TLSPL) {
		if (Tlspl *tls = Get<Tlspl>(t->waitId)) tls->waiters.Remove(t->uid);
	}
	// Free every block the thread had.
	std::vector<SceUID> pools;
	auto range = g_tlsplHeld.equal_range(t->uid);
	for (auto it = range.first; it != range.second; ++it) pools.push_back(it->second);
	g_tlsplHeld.erase(t->uid);
	for (SceUID p : pools)
		if (Tlspl *tls = Get<Tlspl>(p)) FreeTls(tls, t->uid);
}

SceUID sceKernelCreateTlspl(const char *name, u32 partition, u32 attr, u32 blockSize, u32 count, u32 optPtr) {
	if (!name) return (SceUID)SCE_KERNEL_ERROR_NO_MEMORY;
	if ((attr & ~PSP_TLSPL_ATTR_KNOWN) >= 0x100) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	if (partition < 1 || partition > 6) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	BlockAllocator *alloc = AllocatorForPartitionId((int)partition, CallerIsKernelMode());
	if (!alloc) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_PERM;
	if (IllegalPoolSize(blockSize, count)) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
	int index = -1;
	for (int i = 0; i < TLSPL_NUM_INDEXES; ++i) {
		if (!g_tlsplUsedIndexes[i]) {
			index = i;
			break;
		}
	}
	if (index == -1) return (SceUID)SCE_KERNEL_ERROR_TOO_MANY_TLSPL;
	// Unless specified, align to 4 bytes (a MIPS word).
	u32 alignment = 4;
	if (mem::IsValidRange(optPtr, 8)) {
		u32 size = GRead32(optPtr);
		if (size >= 8) alignment = GRead32(optPtr + 4);
		// 0 is intentionally allowed.
		if ((alignment & (alignment - 1)) != 0) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
		if (alignment < 4) alignment = 4;
	}
	// Upaligned; strangely, the status reports the original size.
	u32 alignedSize = (blockSize + alignment - 1) & ~(alignment - 1);
	if ((u64)alignedSize * (u64)count > 0xFFFFFFFFULL) return (SceUID)SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE;
	u32 total = alignedSize * count;
	u32 block = alloc->Alloc(total, (attr & PSP_TLSPL_ATTR_HIGHMEM) != 0, ("TLS/" + TruncName(name)).c_str());
	if (!block) return (SceUID)SCE_KERNEL_ERROR_NO_MEMORY;
	auto *tls = new Tlspl();
	tls->name = TruncName(name);
	tls->attr = attr;
	tls->index = index;
	g_tlsplUsedIndexes[index] = true;
	tls->blockSize = blockSize;
	tls->totalBlocks = count;
	tls->freeBlocks = count;
	tls->address = block;
	tls->alignment = alignment;
	tls->usage.assign(count, 0);
	return Register(tls);
}

int sceKernelDeleteTlspl(SceUID id) {
	Tlspl *tls = Get<Tlspl>(id);
	if (!tls) return (int)SCE_KERNEL_ERROR_UNKNOWN_TLSPL_ID;
	SceUID cur = sched::CurrentId();
	for (SceUID t : tls->usage)
		if (t != 0 && t != cur) return (int)SCE_KERNEL_ERROR_TLSPL_IN_USE;
	tls->waiters.Cleanup(WAITTYPE_TLSPL, id);
	for (SceUID w : tls->waiters.threads) {
		PspThread *t = Get<PspThread>(w);
		if (!t) continue;
		// sceKernelGetTlsAddr gets a null address, _sceKernelAllocateTlspl an error.
		s32 result = t->waitValue[0] != TLSPL_WAITVALUE_RETURN_ADDR ? (s32)SCE_KERNEL_ERROR_WAIT_DELETE : 0;
		sched::Wake(t, result);
	}
	tls->waiters.threads.clear();
	for (SceUID t : tls->usage)
		if (t) TlsHeldErase(t, id);
	g_tlsplUsedIndexes[tls->index] = false;
	Destroy(id);
	sched::Reschedule();
	return 0;
}

Tlspl *FindTlspl(SceUID id) {
	if (Tlspl *tls = Get<Tlspl>(id)) return tls;
	if (id < 0) return nullptr;
	// It also looks pools up by index, from bits of the id. That only matters for small numbers:
	// a stale UID of a deleted pool doesn't find one (threads/tls/get), and ours are all large.
	if (id >= 0x100000) return nullptr;
	int index = (id >> 3) & 15;
	if (!g_tlsplUsedIndexes[index]) return nullptr;
	Tlspl *found = nullptr;
	ForEach<Tlspl>([&](Tlspl *t) {
		if (!found && t->index == index) found = t;
	});
	return found;
}

// The current thread's block in the pool, allocated if needed, or 0 if the pool is full.
u32 AllocateTls(Tlspl *tls) {
	SceUID threadId = sched::CurrentId();
	int allocBlock = -1;
	bool needsClear = false;
	for (u32 i = 0; i < tls->totalBlocks && allocBlock == -1; ++i)
		if (tls->usage[i] == threadId) allocBlock = (int)i;
	if (allocBlock == -1) {
		for (u32 i = 0; i < tls->totalBlocks && allocBlock == -1; ++i) {
			// The PSP doesn't give the same block out twice in a row, even if freed.
			if (tls->usage[tls->next] == 0) allocBlock = tls->next;
			tls->next = (tls->next + 1) % (int)tls->totalBlocks;
		}
		if (allocBlock != -1) {
			tls->usage[allocBlock] = threadId;
			g_tlsplHeld.emplace(threadId, tls->uid);
			--tls->freeBlocks;
			needsClear = true;
		}
	}
	if (allocBlock == -1) return 0;
	u32 addr = tls->address + (u32)allocBlock * tls->AlignedSize();
	// Blocks are cleared on first allocation (and when freed; both are necessary).
	if (needsClear) mem::Memset(addr, 0, tls->blockSize);
	return addr;
}

WaitHooks TlsHooks(SceUID id) {
	WaitHooks hooks;
	SceUID me = sched::CurrentId();
	hooks.timeout = [id, me]() {
		if (Tlspl *tls = Get<Tlspl>(id)) tls->waiters.Remove(me);
	};
	return hooks;
}

int sceKernelGetTlsAddr(SceUID id) {
	if (!sched::DispatchEnabled() || sched::InInterrupt()) return 0;
	Tlspl *tls = FindTlspl(id);
	if (!tls) return 0;
	u32 addr = AllocateTls(tls);
	if (addr) return (int)addr;
	PspThread *t = sched::Current();
	t->waitValue[0] = TLSPL_WAITVALUE_RETURN_ADDR;
	tls->waiters.threads.push_back(t->uid);
	WaitHooks hooks = TlsHooks(tls->uid);
	return sched::Wait(WAITTYPE_TLSPL, tls->uid, 0, false, &hooks);
}

// The kernel's check on a pointer from user mode: neither end may be a kernel address.
bool BadUserPtr(u32 ptr, u32 size) {
	if (!CallerIsKernelMode() && ((ptr | (ptr + size)) & 0x80000000) != 0) return true;
	return ptr != 0 && !mem::IsValidRange(ptr, size);
}

// The syscall behind usersystemlib's sceKernelGetTlsAddr.
int _sceKernelAllocateTlspl(SceUID id, u32 addrPtr, u32 timeoutPtr) {
	if (BadUserPtr(addrPtr, 4) || addrPtr == 0 || BadUserPtr(timeoutPtr, 4)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
	// Unlike sceKernelGetTlsAddr, no lookup by index.
	Tlspl *tls = Get<Tlspl>(id);
	if (!tls) return (int)SCE_KERNEL_ERROR_UNKNOWN_TLSPL_ID;
	u32 addr = AllocateTls(tls);
	if (addr) {
		GWrite32(addrPtr, addr);
		return 0;
	}
	if (sched::WaitTimesOutAtOnce(timeoutPtr)) return (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
	PspThread *t = sched::Current();
	t->waitValue[0] = addrPtr;
	tls->waiters.threads.push_back(t->uid);
	WaitHooks hooks = TlsHooks(id);
	return sched::Wait(WAITTYPE_TLSPL, id, timeoutPtr, false, &hooks);
}

int sceKernelFreeTlspl(SceUID id) {
	Tlspl *tls = Get<Tlspl>(id);
	if (!tls) return (int)SCE_KERNEL_ERROR_UNKNOWN_TLSPL_ID;
	return FreeTls(tls, sched::CurrentId());
}

int sceKernelReferTlsplStatus(SceUID id, u32 infoPtr) {
	Tlspl *tls = Get<Tlspl>(id);
	if (!tls) return (int)SCE_KERNEL_ERROR_UNKNOWN_TLSPL_ID;
	tls->Sort();
	struct {
		u32 size;
		char name[32];
		u32 attr;
		s32 index;
		u32 blockSize, totalBlocks, freeBlocks, numWaitThreads;
	} info{};
	if (mem::IsValidRange(infoPtr, sizeof(info)) && GRead32(infoPtr) != 0) {
		info.size = sizeof(info);
		CopyName(info.name, tls->name);
		info.attr = tls->attr;
		info.index = tls->index;
		info.blockSize = tls->blockSize;
		info.totalBlocks = tls->totalBlocks;
		info.freeBlocks = tls->freeBlocks;
		info.numWaitThreads = (u32)tls->waiters.threads.size();
		mem::WriteBytes(infoPtr, &info, sizeof(info));
	}
	return 0;
}

}  // namespace

void kernel::RegisterThreadManPools() {
	AddThreadEndListener(&TlsplThreadEnd);

	constexpr u32 NID = KF_NOT_IN_INTERRUPT | KF_NOT_DISPATCH_SUSPENDED;
	static const hle::HleFunction funcs[] = {
		HLEFN(0x56C039B5, sceKernelCreateVpl),
		HLEFN(0x89B3D48C, sceKernelDeleteVpl),
		KFN(0xBED27435, sceKernelAllocateVpl, NID),
		KFN(0xEC0A693F, sceKernelAllocateVplCB, NID),
		HLEFN(0xAF36D708, sceKernelTryAllocateVpl),
		HLEFN(0xB736E9FF, sceKernelFreeVpl),
		HLEFN(0x1D371B8A, sceKernelCancelVpl),
		HLEFN(0x39810265, sceKernelReferVplStatus),

		HLEFN(0xC07BB470, sceKernelCreateFpl),
		HLEFN(0xED1410E0, sceKernelDeleteFpl),
		KFN(0xD979E9BF, sceKernelAllocateFpl, NID),
		KFN(0xE7282CB6, sceKernelAllocateFplCB, NID),
		HLEFN(0x623AE665, sceKernelTryAllocateFpl),
		HLEFN(0xF6414A71, sceKernelFreeFpl),
		HLEFN(0xA8AA591F, sceKernelCancelFpl),
		HLEFN(0xD8199E4C, sceKernelReferFplStatus),

		HLEFN(0x8DAFF657, sceKernelCreateTlspl),
		HLEFN(0x32BF938E, sceKernelDeleteTlspl),
		HLEFN(0x721067F3, sceKernelReferTlsplStatus),
		HLEFN(0x4A719FB2, sceKernelFreeTlspl),
		HLEFN(0x65F54FFB, _sceKernelAllocateTlspl),
	};
	hle::RegisterModule("ThreadManForUser", funcs);
	hle::RegisterModule("ThreadManForKernel", funcs);

	static const hle::HleFunction kl[] = {
		HLEFN(0xFA835CDE, sceKernelGetTlsAddr),
	};
	hle::RegisterModule("Kernel_Library", kl);
}
