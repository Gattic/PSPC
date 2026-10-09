// SPDX-License-Identifier: GPL-2.0-or-later
// A module loaded into a flat buffer at its recompile base, plus the facts analysis needs.
#pragma once

#include <array>
#include <set>
#include <string>
#include <vector>

#include "loader/elf.h"
#include "loader/module_tables.h"

namespace recomp {

struct ModuleImage {
	std::string name;
	std::array<u8, 20> sha1{};
	bool relocatable = false;
	u32 base = 0;  // load base used for analysis (vaddr offset for PRX, 0 for ET_EXEC)
	u32 low = 0, high = 0;
	std::vector<u8> mem;  // [low, high)
	loader::LoadedImage image;
	std::vector<loader::ImportLib> imports;
	std::vector<loader::ExportLib> exports;
	std::vector<std::pair<u32, u32>> code;  // executable ranges
	std::set<u32> hi16Relocs;               // lui words patched by HI16 relocations
	std::set<u32> relocWords;               // R_MIPS_32 targets (data words that hold addresses)
	std::set<u32> varImportWords;           // words patched at runtime by variable imports
	std::set<u32> stubWords;                // import stub instructions (handled by the runtime)
	u32 entry = 0;

	bool Load(const std::vector<u8> &elfData, u32 prxBase, std::string *err);
	bool InCode(u32 a) const;
	bool InImage(u32 a, u32 size = 4) const { return a >= low && (u64)a + size <= high; }
	u32 Read32(u32 a) const;
	bool IsStub(u32 a) const { return stubWords.count(a & ~7u) || stubWords.count(a); }
};

}  // namespace recomp
