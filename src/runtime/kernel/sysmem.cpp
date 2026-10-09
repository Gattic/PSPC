// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/kernel/sysmem.h"

#include "common/log.h"
#include "runtime/core/memory.h"

namespace kernel {

BlockAllocator g_kernelMem;
BlockAllocator g_userMem;
BlockAllocator g_volatileMem;
u32 g_firmwareVersion = 0x06060010;

void BlockAllocator::Init(u32 start, u32 size, u32 grain) {
	start_ = start;
	size_ = size;
	grain_ = grain;
	blocks_.clear();
	blocks_.push_back({start, size, false, ""});
}

u32 BlockAllocator::Take(size_t idx, u32 addr, u32 size, const char *tag) {
	Block b = blocks_[idx];
	std::vector<Block> repl;
	if (addr > b.start) repl.push_back({b.start, addr - b.start, false, ""});
	repl.push_back({addr, size, true, tag ? tag : ""});
	u32 end = addr + size, bend = b.start + b.size;
	if (bend > end) repl.push_back({end, bend - end, false, ""});
	blocks_.erase(blocks_.begin() + idx);
	blocks_.insert(blocks_.begin() + idx, repl.begin(), repl.end());
	return addr;
}

u32 BlockAllocator::Alloc(u32 size, bool fromTop, const char *tag, u32 align) {
	if (size == 0) size = grain_;
	size = AlignUp(size, grain_);
	if (align < grain_) align = grain_;
	if (!fromTop) {
		for (size_t i = 0; i < blocks_.size(); i++) {
			const Block &b = blocks_[i];
			if (b.taken) continue;
			u32 a = AlignUp(b.start, align);
			if (a >= b.start + b.size) continue;
			if ((u64)a + size <= (u64)b.start + b.size) return Take(i, a, size, tag);
		}
	} else {
		for (size_t i = blocks_.size(); i-- > 0;) {
			const Block &b = blocks_[i];
			if (b.taken || b.size < size) continue;
			u32 a = AlignDown(b.start + b.size - size, align);
			if (a >= b.start) return Take(i, a, size, tag);
		}
	}
	return 0;
}

u32 BlockAllocator::AllocAt(u32 addr, u32 size, const char *tag) {
	u32 a = AlignDown(addr, grain_);
	size = AlignUp(size + (addr - a), grain_);
	for (size_t i = 0; i < blocks_.size(); i++) {
		const Block &b = blocks_[i];
		if (a >= b.start && a < b.start + b.size) {
			if (b.taken || (u64)a + size > (u64)b.start + b.size) return 0;
			Take(i, a, size, tag);
			return addr;
		}
	}
	return 0;
}

bool BlockAllocator::Free(u32 addr) {
	for (size_t i = 0; i < blocks_.size(); i++) {
		if (blocks_[i].start == addr && blocks_[i].taken) {
			blocks_[i].taken = false;
			blocks_[i].tag.clear();
			// Merge with neighbours.
			if (i + 1 < blocks_.size() && !blocks_[i + 1].taken) {
				blocks_[i].size += blocks_[i + 1].size;
				blocks_.erase(blocks_.begin() + i + 1);
			}
			if (i > 0 && !blocks_[i - 1].taken) {
				blocks_[i - 1].size += blocks_[i].size;
				blocks_.erase(blocks_.begin() + i);
			}
			return true;
		}
	}
	return false;
}

u32 BlockAllocator::BlockSize(u32 addr) const {
	for (const Block &b : blocks_)
		if (b.start == addr && b.taken) return b.size;
	return 0;
}

u32 BlockAllocator::MaxFree() const {
	u32 m = 0;
	for (const Block &b : blocks_)
		if (!b.taken && b.size > m) m = b.size;
	return m;
}

u32 BlockAllocator::TotalFree() const {
	u32 t = 0;
	for (const Block &b : blocks_)
		if (!b.taken) t += b.size;
	return t;
}

void BlockAllocator::Dump() const {
	for (const Block &b : blocks_)
		LOGI("MEM", "  %08x-%08x %s %s", b.start, b.start + b.size, b.taken ? "used" : "free", b.tag.c_str());
}

void InitMemoryPartitions(u32 ramSize, bool largeUserMemory) {
	// Kernel partition (runtime-internal structures, interrupt stacks, module stubs).
	// The first 0x4000 bytes are reserved for the magic return stub and friends.
	g_kernelMem.Init(0x08000000 + 0x4000, 0x00400000 - 0x4000, 0x100);
	g_volatileMem.Init(0x08400000, 0x00400000, 0x100);
	u32 userEnd = 0x0A000000;
	if (largeUserMemory && ramSize > 0x02000000) userEnd = 0x08000000 + ramSize;
	g_userMem.Init(0x08800000, userEnd - 0x08800000, 0x100);
	// The firmware keeps usersystemlib's work area at the bottom of user memory, so the first
	// module loads at 0x08804000 (cpu/cpu_alu/cpu_branch prints addresses relative to that).
	g_userMem.AllocAt(0x08800000, 0x4000, "usersystemlib");
}

BlockAllocator *AllocatorForPartition(int partition) {
	switch (partition) {
	case 1:
	case 3:
	case 4: return &g_kernelMem;
	case 2:
	case 6: return &g_userMem;
	case 5: return &g_volatileMem;
	default: return nullptr;
	}
}

BlockAllocator *AllocatorForAddress(u32 addr) {
	addr &= 0x3FFFFFFF;
	if (g_userMem.Contains(addr)) return &g_userMem;
	if (g_kernelMem.Contains(addr)) return &g_kernelMem;
	if (g_volatileMem.Contains(addr)) return &g_volatileMem;
	return nullptr;
}

u32 AllocKernelMemory(u32 size, const char *tag) {
	u32 a = g_kernelMem.Alloc(size, false, tag);
	if (!a) Fatal("Out of kernel memory allocating %u bytes for %s", size, tag);
	return a;
}

}  // namespace kernel
