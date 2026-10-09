// SPDX-License-Identifier: GPL-2.0-or-later
#include "recomp/image.h"

#include "common/sha1.h"

namespace recomp {

bool ModuleImage::Load(const std::vector<u8> &elfData, u32 prxBase, std::string *err) {
	loader::ElfFile elf;
	if (!elf.Parse(elfData.data(), elfData.size(), err)) return false;
	sha1 = Sha1::Hash(elfData.data(), elfData.size());
	relocatable = elf.Relocatable();
	base = relocatable ? prxBase - elf.TotalStart() : 0;
	low = base + elf.TotalStart();
	high = low + elf.TotalSize();
	mem.assign(high - low, 0);
	auto ptr = [this](u32 a, u32 size) -> u8 * {
		if (a < low || (u64)a + size > high) return nullptr;
		return mem.data() + (a - low);
	};
	if (!elf.Load(base, ptr, image, err)) return false;
	name = elf.HasModuleInfo() ? std::string(elf.ModuleInfo().name, strnlen(elf.ModuleInfo().name, 28)) : "ELF";
	entry = image.entry;
	code = image.codeRanges;
	for (const auto &r : image.relocs) {
		if (r.type == loader::R_MIPS_HI16) hi16Relocs.insert(r.addr);
		if (r.type == loader::R_MIPS_32) relocWords.insert(r.addr);
	}
	auto rd = [this](u32 a, u32 size) -> const u8 * {
		if (a < low || (u64)a + size > high) return nullptr;
		return mem.data() + (a - low);
	};
	const auto &mi = image.modInfo;
	loader::ParseImports(mi.libstub, mi.libstubEnd, rd, imports, nullptr);
	loader::ParseExports(mi.libent, mi.libentEnd, rd, exports, nullptr);
	for (const auto &lib : imports) {
		for (const auto &f : lib.funcs) {
			stubWords.insert(f.stubAddr);
			stubWords.insert(f.stubAddr + 4);
		}
		for (const auto &v : lib.vars)
			for (const auto &r : v.refs) varImportWords.insert(r.addr);
	}
	return true;
}

bool ModuleImage::InCode(u32 a) const {
	for (const auto &r : code)
		if (a >= r.first && a < r.second) return true;
	return false;
}

u32 ModuleImage::Read32(u32 a) const {
	if (!InImage(a)) return 0;
	u32 v;
	memcpy(&v, mem.data() + (a - low), 4);
	return v;
}

}  // namespace recomp
