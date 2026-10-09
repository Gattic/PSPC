// SPDX-License-Identifier: GPL-2.0-or-later
//
// ELF / PRX loading shared by the runtime and the recompiler.
// Relocation semantics follow PPSSPP's ElfReader (Portions derived from PPSSPP,
// https://github.com/hrydgard/ppsspp, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/common.h"

namespace loader {

constexpr u16 ET_EXEC = 2;
constexpr u16 ET_PRX = 0xFFA0;
constexpr u32 PT_LOAD = 1;
constexpr u32 PT_PSPREL1 = 0x700000A0;
constexpr u32 PT_PSPREL2 = 0x700000A1;
constexpr u32 SHT_REL = 9;
constexpr u32 SHT_PSPREL = 0x700000A0;
constexpr u32 SHF_WRITE = 1;
constexpr u32 SHF_ALLOC = 2;
constexpr u32 SHF_EXECINSTR = 4;
constexpr u32 PF_X = 1;

enum RelocType : u8 {
	R_MIPS_NONE = 0,
	R_MIPS_16 = 1,
	R_MIPS_32 = 2,
	R_MIPS_26 = 4,
	R_MIPS_HI16 = 5,
	R_MIPS_LO16 = 6,
	R_MIPS_GPREL16 = 7,
};

struct Segment {
	u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
	u32 loadAddr = 0;  // where it ended up (0 if not PT_LOAD)
};

struct Section {
	std::string name;
	u32 type, flags, addr, offset, size, link, info, align, entsize;
	u32 loadAddr = 0;
};

// One applied relocation. The recompiler needs to know which code words were patched and how.
struct RelocRecord {
	u32 addr;   // absolute guest address of the patched word
	u8 type;    // RelocType (type-B relocs are normalised to the same values)
	u32 target; // segment base that was added
};

struct PspModuleInfoRaw {
	u16 attr;
	u8 version[2];
	char name[28];
	u32 gp;
	u32 libent;
	u32 libentEnd;
	u32 libstub;
	u32 libstubEnd;
};
static_assert(sizeof(PspModuleInfoRaw) == 52, "module info layout");

struct LoadedImage {
	bool relocatable = false;
	u32 base = 0;         // added to every vaddr (0 for ET_EXEC)
	u32 lowAddr = 0;      // first loaded byte
	u32 size = 0;         // bytes from lowAddr
	u32 entry = 0;
	u32 modInfoAddr = 0;  // guest address of the module info (0 if absent)
	PspModuleInfoRaw modInfo{};
	std::vector<Segment> segments;
	std::vector<Section> sections;
	std::vector<RelocRecord> relocs;
	// Executable ranges [start, end) after loading.
	std::vector<std::pair<u32, u32>> codeRanges;
	int relocErrors = 0;
};

class ElfFile {
public:
	bool Parse(const u8 *data, size_t size, std::string *err);

	u16 Type() const { return type_; }
	bool IsPrx() const { return type_ == ET_PRX; }
	bool Relocatable() const { return type_ != ET_EXEC; }
	u32 TotalStart() const { return totalStart_; }
	u32 TotalSize() const { return totalEnd_ - totalStart_; }
	u32 FirstSegmentAlign() const { return firstSegAlign_; }
	bool HasModuleInfo() const { return modInfoOffset_ != 0xFFFFFFFF; }
	const PspModuleInfoRaw &ModuleInfo() const { return modInfo_; }
	bool IsKernelModule() const { return HasModuleInfo() && (modInfo_.attr & 0x1000) != 0; }
	const std::vector<Segment> &Segments() const { return segments_; }
	const std::vector<Section> &Sections() const { return sections_; }

	// Copy segments into guest memory and relocate. `ptr(addr, size)` must return a host
	// pointer covering [addr, addr+size) of guest memory (or nullptr when invalid). For
	// relocatable images `base` is added to all vaddrs; for ET_EXEC it must be 0.
	bool Load(u32 base, const std::function<u8 *(u32, u32)> &ptr, LoadedImage &out, std::string *err) const;

private:
	void ApplyRelocsA(const u8 *relData, u32 numRelocs, const std::vector<u32> &segAddr,
	                  const std::function<u8 *(u32, u32)> &ptr, LoadedImage &out) const;
	void ApplyRelocsB(int seg, const std::vector<u32> &segAddr, const std::function<u8 *(u32, u32)> &ptr,
	                  LoadedImage &out) const;

	const u8 *data_ = nullptr;
	size_t size_ = 0;
	u16 type_ = 0;
	u32 entry_ = 0;
	u32 totalStart_ = 0, totalEnd_ = 0, firstSegAlign_ = 0;
	u32 modInfoOffset_ = 0xFFFFFFFF;  // file offset
	u32 modInfoVaddr_ = 0;            // vaddr (before base) or 0xFFFFFFFF if only via file offset
	PspModuleInfoRaw modInfo_{};
	std::vector<Segment> segments_;
	std::vector<Section> sections_;
};

}  // namespace loader
