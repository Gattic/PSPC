// SPDX-License-Identifier: GPL-2.0-or-later
// PSP module import (.lib.stub) and export (.lib.ent) tables, read from loaded guest memory.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/common.h"

namespace loader {

// Well-known NIDs of the system export library (the libent entry with no name).
constexpr u32 NID_MODULE_START = 0xD632ACDB;
constexpr u32 NID_MODULE_STOP = 0xCEE8593C;
constexpr u32 NID_MODULE_REBOOT_BEFORE = 0x2F064FA6;
constexpr u32 NID_MODULE_REBOOT_PHASE = 0xADF12745;
constexpr u32 NID_MODULE_BOOTSTART = 0xD3744BE0;
constexpr u32 NID_MODULE_INFO = 0xF01D73A7;
constexpr u32 NID_MODULE_START_THREAD_PARAMETER = 0x0F7C276C;
constexpr u32 NID_MODULE_STOP_THREAD_PARAMETER = 0xCF0CC697;
constexpr u32 NID_MODULE_SDK_VERSION = 0x11B97506;

struct ImportFunc {
	u32 nid;
	u32 stubAddr;  // 8-byte stub the loader patches
};

struct VarRef {
	u32 addr;  // word to patch
	u8 type;   // R_MIPS_32 / R_MIPS_HI16 / R_MIPS_LO16
};

struct ImportVar {
	u32 nid;
	std::vector<VarRef> refs;
};

struct ImportLib {
	std::string name;
	u16 version = 0, attr = 0;
	u32 entryAddr = 0;
	std::vector<ImportFunc> funcs;
	std::vector<ImportVar> vars;
};

struct ExportLib {
	std::string name;  // empty for the system library (module_start etc.)
	u16 version = 0, attr = 0;
	std::vector<std::pair<u32, u32>> funcs;  // nid, address
	std::vector<std::pair<u32, u32>> vars;   // nid, address
};

using GuestReadPtr = std::function<const u8 *(u32 addr, u32 size)>;

bool ParseImports(u32 libstub, u32 libstubEnd, const GuestReadPtr &mem, std::vector<ImportLib> &out,
                  std::string *err);
bool ParseExports(u32 libent, u32 libentEnd, const GuestReadPtr &mem, std::vector<ExportLib> &out,
                  std::string *err);

}  // namespace loader
