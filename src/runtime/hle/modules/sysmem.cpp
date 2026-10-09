// SPDX-License-Identifier: GPL-2.0-or-later
// SysMemUserForUser: partition memory, firmware/SDK version queries, sceKernelPrintf.
#include <cstdarg>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/sysmem.h"

using namespace kernel;

namespace {

u32 g_compiledSdkVersion = 0;
u32 g_compilerVersion = 0;

u32 sceKernelMaxFreeMemSize() { return g_userMem.MaxFree(); }
u32 sceKernelTotalFreeMemSize() { return g_userMem.TotalFree(); }
u32 sceKernelTotalMemSize() { return g_userMem.Size(); }
u32 sceKernelDevkitVersion() { return g_firmwareVersion; }

int sceKernelAllocPartitionMemory(int partition, const char *name, int type, u32 size, u32 addr) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (type < 0 || type > 4) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE;
	BlockAllocator *a = AllocatorForPartition(partition);
	if (!a || partition == 1 || partition == 3 || partition == 4)
		return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	if (size == 0) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	u32 block = 0;
	switch (type) {
	case 0: block = a->Alloc(size, false, name); break;              // PSP_SMEM_Low
	case 1: block = a->Alloc(size, true, name); break;               // PSP_SMEM_High
	case 2: block = a->AllocAt(addr, size, name); break;             // PSP_SMEM_Addr
	case 3:                                                         // PSP_SMEM_LowAligned
	case 4: {                                                       // PSP_SMEM_HighAligned
		if ((addr & (addr - 1)) != 0 || addr == 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ALIGNMENT_SIZE;
		block = a->Alloc(size, type == 4, name, addr);
		break;
	}
	}
	if (!block) return (int)SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED;
	auto *b = new MemBlock();
	b->name = name;
	b->addr = block;
	b->size = a->BlockSize(block & ~0xFFu) ? a->BlockSize(block & ~0xFFu) : size;
	b->partition = partition;
	return Register(b);
}

int sceKernelFreePartitionMemory(SceUID id) {
	MemBlock *b = Get<MemBlock>(id);
	if (!b) return (int)SCE_KERNEL_ERROR_UNKNOWN_UID;
	if (BlockAllocator *a = AllocatorForAddress(b->addr)) a->Free(AlignDown(b->addr, 0x100));
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
	(void)paramsAddr;
	if (!name) return (int)SCE_KERNEL_ERROR_ERROR;
	if (type > 1) return (int)SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE;
	return sceKernelAllocPartitionMemory(2, name, (int)type, size, 0);
}
int sceKernelFreeMemoryBlock(SceUID id) { return sceKernelFreePartitionMemory(id); }
int sceKernelGetMemoryBlockAddr(SceUID id, u32 outPtr) {
	MemBlock *b = Get<MemBlock>(id);
	if (!b) return (int)SCE_KERNEL_ERROR_UNKNOWN_UID;
	if (mem::IsValidRange(outPtr, 4)) GWrite32(outPtr, b->addr);
	return 0;
}

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
}
