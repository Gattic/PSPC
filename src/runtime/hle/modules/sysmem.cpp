// SPDX-License-Identifier: GPL-2.0-or-later
// SysMemUserForUser: partition memory, firmware/SDK version queries, sceKernelPrintf.
#include <cstdarg>
#include <string>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/kernel_hle.h"
#include "runtime/kernel/sysmem.h"

using namespace kernel;

namespace {

u32 g_compiledSdkVersion = 0;
u32 g_compilerVersion = 0;

u32 sceKernelMaxFreeMemSize() { return g_userMem.MaxFree(); }
u32 sceKernelTotalFreeMemSize() { return g_userMem.TotalFree(); }
u32 sceKernelTotalMemSize() { return g_userMem.Size(); }
u32 sceKernelDevkitVersion() { return g_firmwareVersion; }

// Validation order and error codes follow PPSSPP's sceKernelMemory.cpp / sceKernelHeap.cpp,
// recorded by pspautotests sysmem/* on hardware (Portions derived from PPSSPP,
// https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
int AllocPartitionMemory(int partition, const char *name, int type, u32 size, u32 addr, bool kernelEntry) {
	if (type < 0 || type > 4) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE;
	// Alignment is only allowed for powers of 2.
	if ((type == 3 || type == 4) && ((addr & (addr - 1)) != 0 || addr == 0))
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ALIGNMENT_SIZE;
	// The user entry point reports an out-of-range partition as ILLEGAL_ARGUMENT, the kernel one
	// as ILLEGAL_PARTITION (sysmem/partitions).
	if (partition < 1 || partition > 6)
		return (int)(kernelEntry ? SCE_KERNEL_ERROR_ILLEGAL_PARTITION : SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
	BlockAllocator *a = AllocatorForPartitionId(partition, kernelEntry || CallerIsKernelMode());
	if (!a) return (int)SCE_KERNEL_ERROR_ILLEGAL_PARTITION;
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (size == 0) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	std::string tag = std::string(name).substr(0, 31);
	u32 block = 0;
	switch (type) {
	case 0: block = a->Alloc(size, false, tag.c_str()); break;  // PSP_SMEM_Low
	case 1: block = a->Alloc(size, true, tag.c_str()); break;   // PSP_SMEM_High
	case 2: block = a->AllocAt(addr & ~0xFFu, size, tag.c_str()); break;  // PSP_SMEM_Addr
	case 3:                                                      // PSP_SMEM_LowAligned
	case 4:                                                      // PSP_SMEM_HighAligned
		block = a->Alloc(size, type == 4, tag.c_str(), addr);
		break;
	}
	if (!block) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	auto *b = new MemBlock();
	b->name = tag;
	b->addr = block;
	b->size = a->BlockSize(block) ? a->BlockSize(block) : AlignUp(size, 0x100);
	b->partition = partition;
	return Register(b);
}

int sceKernelAllocPartitionMemory(int partition, const char *name, int type, u32 size, u32 addr) {
	return AllocPartitionMemory(partition, name, type, size, addr, false);
}
int Kernel_sceKernelAllocPartitionMemory(int partition, const char *name, int type, u32 size, u32 addr) {
	return AllocPartitionMemory(partition, name, type, size, addr, true);
}

int sceKernelFreePartitionMemory(SceUID id) {
	MemBlock *b = Get<MemBlock>(id);
	if (!b) return (int)SCE_KERNEL_ERROR_UNKNOWN_UID;
	if (BlockAllocator *a = AllocatorForAddress(b->addr)) a->Free(b->addr);
	Destroy(id);
	return 0;
}

u32 sceKernelGetBlockHeadAddr(SceUID id) {
	MemBlock *b = Get<MemBlock>(id);
	if (!b) return 0;
	return b->addr;
}

// SysMemUserForUser's AllocMemoryBlock family (firmware 6.x API, also used by later SDKs).
int sceKernelAllocMemoryBlock(const char *name, u32 type, u32 size, u32 paramsAddr) {
	if (paramsAddr && (paramsAddr & 3) == 0 && mem::IsValidRange(paramsAddr, 4) && GRead32(paramsAddr) != 4)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	if (type != 0 && type != 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE;
	if (size == 0) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	std::string tag = std::string(name).substr(0, 31);
	u32 block = g_userMem.Alloc(size, type == 1, tag.c_str());
	if (!block) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	auto *b = new MemBlock();
	b->name = tag;
	b->addr = block;
	b->size = g_userMem.BlockSize(block);
	b->partition = 2;
	return Register(b);
}
int sceKernelFreeMemoryBlock(SceUID id) { return sceKernelFreePartitionMemory(id); }
int sceKernelGetMemoryBlockAddr(SceUID id, u32 outPtr) {
	MemBlock *b = Get<MemBlock>(id);
	if (!b) return 0;
	if (mem::IsValidRange(outPtr, 4)) GWrite32(outPtr, b->addr);
	return 0;
}

// ---- SysMemForKernel heaps ----

struct KernelHeap : KObject {
	static constexpr KType kType = KType::Other;
	KType Type() const override { return kType; }
	int partition = 0;
	u32 size = 0, address = 0;
	int flags = 0;
	BlockAllocator alloc;
};

KernelHeap *GetHeap(SceUID id) {
	KObject *o = GetAny(id);
	return o && o->Type() == KType::Other ? dynamic_cast<KernelHeap *>(o) : nullptr;
}

int sceKernelCreateHeap(int partition, int size, int flags, const char *name) {
	// Only partitions 1-6 exist; anything else is ILLEGAL_PARTITION (sysmem/kernel/heap).
	if (partition < 1 || partition > 6) return (int)SCE_KERNEL_ERROR_ILLEGAL_PARTITION;
	BlockAllocator *a = AllocatorForPartitionId(partition, true);
	if (!a) return (int)SCE_KERNEL_ERROR_ILLEGAL_PARTITION;
	if (size <= 0) return (int)SCE_KERNEL_ERROR_HEAPBLOCK_ALLOC_FAILED;
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	u32 allocSize = ((u32)size + 3) & ~3u;
	// flags is ignored: every value creates a heap.
	u32 addr = a->Alloc(allocSize, true, ("KernelHeap/" + std::string(name)).c_str());
	if (!addr) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	auto *h = new KernelHeap();
	h->name = name;
	h->partition = partition;
	h->flags = flags;
	h->size = a->BlockSize(addr);
	h->address = addr;
	// The first 128 bytes hold the heap's own bookkeeping.
	h->alloc.Init(addr + 128, h->size > 128 ? h->size - 128 : 0, 16);
	return Register(h);
}

u32 sceKernelAllocHeapMemory(SceUID id, int size) {
	KernelHeap *h = GetHeap(id);
	// Returns a pointer, so every failure is a null pointer.
	if (!h) return 0;
	// There are 8 bytes at the end of every block, reserved.
	return h->alloc.Alloc((u32)size + 8, true, "heapblock");
}

u32 sceKernelAllocHeapMemoryWithOption(SceUID id, u32 size, u32 paramsPtr) {
	KernelHeap *h = GetHeap(id);
	if (!h) return 0;
	u32 grain = 4;
	if (paramsPtr != 0) {
		if ((paramsPtr & 3) || !mem::IsValidRange(paramsPtr, 8)) return 0;
		// The size field isn't validated; only the alignment matters, a power of 2 from 4 to
		// 0x80 (0 means no preference).
		grain = GRead32(paramsPtr + 4);
		if (grain == 0) grain = 4;
		else if (grain < 4 || grain > 0x80 || (grain & (grain - 1)) != 0) return 0;
	}
	u32 memSize = size + 8;
	if (grain > 16) memSize = AlignUp(memSize, grain);
	return h->alloc.Alloc(memSize, true, "heapblock", grain);
}

int sceKernelFreeHeapMemory(SceUID id, u32 block) {
	KernelHeap *h = GetHeap(id);
	if (!h) return (int)SCE_KERNEL_ERROR_UNKNOWN_UID;
	if (block == 0) return 0;
	if (!h->alloc.Free(block)) return (int)0x80000103;  // SCE_KERNEL_ERROR_INVALID_POINTER
	return 0;
}

int sceKernelDeleteHeap(SceUID id) {
	KernelHeap *h = GetHeap(id);
	if (!h) return (int)SCE_KERNEL_ERROR_UNKNOWN_UID;
	if (BlockAllocator *a = AllocatorForAddress(h->address)) a->Free(h->address);
	Destroy(id);
	return 0;
}

u32 sceKernelPartitionTotalFreeMemSize(int partition) {
	BlockAllocator *a = AllocatorForPartitionId(partition, true);
	if (!a) return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	return a->TotalFree();
}

u32 sceKernelPartitionMaxFreeMemSize(int partition) {
	BlockAllocator *a = AllocatorForPartitionId(partition, true);
	if (!a) return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	return a->MaxFree();
}

int sceKernelGetModel() { return 1; }  // PSP-2000

// sceKernelPrintf(fmt, ...) with a minimal printf over guest varargs (a1..t3).
void sceKernelPrintf(Ctx *c) {
	const char *fmt = mem::GetCString(c->r[4]);
	if (!fmt) {
		c->r[2] = 0;
		return;
	}
	std::string out;
	int arg = 5;
	auto next = [&]() -> u32 { return arg <= 11 ? c->r[arg++] : 0; };
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			out.push_back(*p);
			continue;
		}
		std::string spec = "%";
		p++;
		while (*p && strchr("-+ #0123456789.lhz", *p)) spec.push_back(*p++);
		if (!*p) break;
		char conv = *p;
		char buf[256];
		switch (conv) {
		case 's': {
			const char *s = mem::GetCString(next());
			snprintf(buf, sizeof(buf), (spec + "s").c_str(), s ? s : "(null)");
			out += buf;
			break;
		}
		case '%': out.push_back('%'); break;
		case 'c':
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'p':
			snprintf(buf, sizeof(buf), (spec + conv).c_str(), next());
			out += buf;
			break;
		default: out += spec + conv; break;
		}
	}
	logging::GuestOutput(out.data(), out.size());
	c->r[2] = 0;
}

int sceKernelSetCompiledSdkVersion(u32 v) {
	g_compiledSdkVersion = v;
	return 0;
}
u32 sceKernelGetCompiledSdkVersion() { return g_compiledSdkVersion; }
int sceKernelSetCompilerVersion(u32 v) {
	g_compilerVersion = v;
	return 0;
}
u32 SysMemUserForUser_945E45DA(u32 ptr) {
	// Writes a 16-byte "PSP" model string on hardware; leave the buffer alone.
	(void)ptr;
	return 0;
}
u32 SysMemUserForUser_D8DE5C1E() { return 0; }

}  // namespace

u32 kernel::CompiledSdkVersion() { return g_compiledSdkVersion; }

void Register_SysMemUserForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xA291F107, sceKernelMaxFreeMemSize),
		HLEFN(0xF919F628, sceKernelTotalFreeMemSize),
		HLEFN(0x3FC9AE6A, sceKernelDevkitVersion),
		HLEFN(0x237DBD4F, sceKernelAllocPartitionMemory),
		HLEFN(0xB6D61D02, sceKernelFreePartitionMemory),
		HLEFN(0x9D9A5BA1, sceKernelGetBlockHeadAddr),
		HLERAW(0x13A5ABEF, sceKernelPrintf),
		HLEFN(0x7591C7DB, sceKernelSetCompiledSdkVersion),
		HLEFN(0x342061E5, sceKernelSetCompiledSdkVersion),
		HLEFN(0x315AD3A0, sceKernelSetCompiledSdkVersion),
		HLEFN(0xEBD5C3E6, sceKernelSetCompiledSdkVersion),
		HLEFN(0x057E7380, sceKernelSetCompiledSdkVersion),
		HLEFN(0xF77D77CB, sceKernelSetCompilerVersion),
		HLEFN(0x91DE343C, sceKernelSetCompiledSdkVersion),
		HLEFN(0x7893F79A, sceKernelSetCompiledSdkVersion),
		HLEFN(0x35669D4C, sceKernelSetCompiledSdkVersion),
		HLEFN(0x1B4217BC, sceKernelSetCompiledSdkVersion),
		HLEFN(0x358CA1BB, sceKernelSetCompiledSdkVersion),
		HLEFN(0xFC114573, sceKernelGetCompiledSdkVersion),
		HLEFN(0xACBD88CA, sceKernelTotalMemSize),
		HLEFN(0x945E45DA, SysMemUserForUser_945E45DA),
		HLESTUB(0xA6848DF8, "sceKernelSetUsersystemLibWork"),
		HLEFN(0xDB83A952, sceKernelGetMemoryBlockAddr),
		HLEFN(0x50F61D8A, sceKernelFreeMemoryBlock),
		HLEFN(0xFE707FDF, sceKernelAllocMemoryBlock),
		HLEFN(0xD8DE5C1E, SysMemUserForUser_D8DE5C1E),
	};
	hle::RegisterModule("SysMemUserForUser", funcs);

	static const hle::HleFunction kfuncs[] = {
		HLEFN(0x636C953B, sceKernelAllocHeapMemory),
		HLEFN(0xC9805775, sceKernelDeleteHeap),
		HLEFN(0x1C1FBFE7, sceKernelCreateHeap),
		HLEFN(0x237DBD4F, Kernel_sceKernelAllocPartitionMemory),
		HLEFN(0xB6D61D02, sceKernelFreePartitionMemory),
		HLEFN(0x9D9A5BA1, sceKernelGetBlockHeadAddr),
		HLEFN(0x9697CD32, sceKernelPartitionTotalFreeMemSize),
		HLEFN(0xE6581468, sceKernelPartitionMaxFreeMemSize),
		HLEFN(0x3FC9AE6A, sceKernelDevkitVersion),
		HLEFN(0x7B749390, sceKernelFreeHeapMemory),
		HLEFN(0xEB7A74DB, sceKernelAllocHeapMemoryWithOption),
		HLEFN(0x6373995D, sceKernelGetModel),
		HLEFN(0x07C586A1, sceKernelGetModel),
		HLEFN(0xDA07DC6E, sceKernelGetModel),
		HLEFN(0x4823B9D9, sceKernelGetModel),
		HLEFN(0x864EBFD7, sceKernelGetModel),
		HLEFN(0x458A70B5, sceKernelGetModel),
		HLEFN(0xA3B0B6BC, sceKernelGetModel),
		HLEFN(0xA291F107, sceKernelMaxFreeMemSize),
		HLEFN(0xF919F628, sceKernelTotalFreeMemSize),
		HLERAW(0x13A5ABEF, sceKernelPrintf),
	};
	hle::RegisterModule("SysMemForKernel", kfuncs);
}
