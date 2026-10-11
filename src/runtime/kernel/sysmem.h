// SPDX-License-Identifier: GPL-2.0-or-later
// PSP memory partitions and the block allocator behind SysMemUserForUser.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"
#include "runtime/kernel/kobject.h"

namespace kernel {

class BlockAllocator {
public:
	void Init(u32 start, u32 size, u32 grain);
	// Returns the block address or 0 on failure. align must be a power of two (0 = grain).
	u32 Alloc(u32 size, bool fromTop, const char *tag, u32 align = 0);
	u32 AllocAt(u32 addr, u32 size, const char *tag);
	bool Free(u32 addr);
	bool Contains(u32 addr) const { return addr >= start_ && addr < start_ + size_; }
	u32 BlockSize(u32 addr) const;
	u32 MaxFree() const;
	u32 TotalFree() const;
	u32 Start() const { return start_; }
	u32 Size() const { return size_; }
	void Dump() const;

private:
	struct Block {
		u32 start, size;
		bool taken;
		std::string tag;
	};
	std::vector<Block> blocks_;  // sorted, contiguous, covering [start_, start_+size_)
	u32 start_ = 0, size_ = 0, grain_ = 0x100;
	u32 Take(size_t idx, u32 addr, u32 size, const char *tag);
};

extern BlockAllocator g_kernelMem;
extern BlockAllocator g_userMem;
extern BlockAllocator g_volatileMem;

// userMemEnd (with largeUserMemory): where user memory stops, if before the end of RAM.
void InitMemoryPartitions(u32 ramSize, bool largeUserMemory, u32 userMemEnd = 0);
BlockAllocator *AllocatorForPartition(int partition);
BlockAllocator *AllocatorForAddress(u32 addr);
// Convenience allocations for runtime-internal structures. Fatal on failure.
u32 AllocKernelMemory(u32 size, const char *tag);

// Firmware version reported to games (sceKernelDevkitVersion format, e.g. 0x06060010).
extern u32 g_firmwareVersion;
inline u32 FirmwareVersion() { return g_firmwareVersion; }
// The SDK version the game declared through sceKernelSetCompiledSdkVersion*, 0 if none.
u32 CompiledSdkVersion();

// The allocator behind a partition id as sceKernelCreate{Vpl,Fpl,MsgPipe,Tlspl} see it: the
// kernel partitions (1, 3, 4) and 8/10 only for kernel-mode callers. nullptr if not allowed.
BlockAllocator *AllocatorForPartitionId(int id, bool kernelMode);

struct MemBlock : KObject {
	static constexpr KType kType = KType::MemBlock;
	KType Type() const override { return kType; }
	u32 addr = 0, size = 0;
	int partition = 2;
};

}  // namespace kernel
