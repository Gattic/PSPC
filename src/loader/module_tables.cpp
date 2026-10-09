// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/module_tables.h"

#include <algorithm>

namespace loader {

static bool ReadU32(const GuestReadPtr &mem, u32 addr, u32 &v) {
	const u8 *p = mem(addr, 4);
	if (!p) return false;
	memcpy(&v, p, 4);
	return true;
}

static std::string ReadString(const GuestReadPtr &mem, u32 addr) {
	std::string s;
	if (!addr) return s;
	for (u32 i = 0; i < 256; i++) {
		const u8 *p = mem(addr + i, 1);
		if (!p || !*p) break;
		s.push_back((char)*p);
	}
	return s;
}

bool ParseImports(u32 libstub, u32 libstubEnd, const GuestReadPtr &mem, std::vector<ImportLib> &out,
                  std::string *err) {
	out.clear();
	u32 pos = libstub;
	while (pos + 20 <= libstubEnd) {
		const u8 *e = mem(pos, 20);
		if (!e) {
			if (err) *err = "import table not readable";
			return false;
		}
		u32 nameAddr, nidData, firstSym;
		u16 version, flags, numFuncs;
		memcpy(&nameAddr, e, 4);
		memcpy(&version, e + 4, 2);
		memcpy(&flags, e + 6, 2);
		u8 size = e[8], numVars = e[9];
		memcpy(&numFuncs, e + 10, 2);
		memcpy(&nidData, e + 12, 4);
		memcpy(&firstSym, e + 16, 4);
		if (size == 0) break;
		u32 varData = 0;
		if (size >= 6) ReadU32(mem, pos + 20, varData);

		ImportLib lib;
		lib.name = ReadString(mem, nameAddr);
		lib.version = version;
		lib.attr = flags;
		lib.entryAddr = pos;
		if (numFuncs && nidData) {
			for (u32 i = 0; i < numFuncs; i++) {
				u32 nid;
				if (!ReadU32(mem, nidData + i * 4, nid)) break;
				lib.funcs.push_back({nid, firstSym + i * 8});
			}
		}
		if (numVars && varData) {
			for (u32 i = 0; i < numVars; i++) {
				u32 refsPtr, nid;
				if (!ReadU32(mem, varData + i * 8, refsPtr) || !ReadU32(mem, varData + i * 8 + 4, nid)) break;
				ImportVar var;
				var.nid = nid;
				for (u32 k = 0; k < 0x10000; k++) {
					u32 ref;
					if (!ReadU32(mem, refsPtr + k * 4, ref) || ref == 0) break;
					var.refs.push_back({(ref & 0x03FFFFFF) << 2, (u8)(ref >> 26)});
				}
				lib.vars.push_back(std::move(var));
			}
		}
		out.push_back(std::move(lib));
		pos += size * 4;
	}
	return true;
}

bool ParseExports(u32 libent, u32 libentEnd, const GuestReadPtr &mem, std::vector<ExportLib> &out,
                  std::string *err) {
	out.clear();
	u32 pos = libent;
	while (pos + 16 <= libentEnd) {
		const u8 *e = mem(pos, 16);
		if (!e) {
			if (err) *err = "export table not readable";
			return false;
		}
		u32 nameAddr, resident;
		u16 version, attr, fcount;
		memcpy(&nameAddr, e, 4);
		memcpy(&version, e + 4, 2);
		memcpy(&attr, e + 6, 2);
		u8 size = e[8], vcount = e[9];
		memcpy(&fcount, e + 10, 2);
		memcpy(&resident, e + 12, 4);
		if (size == 0) break;
		u32 varCount = vcount;
		if (size > 4) {
			const u8 *x = mem(pos + 16, 4);
			if (x) {
				u16 vcountNew;
				memcpy(&vcountNew, x, 2);
				varCount = std::max<u32>(vcount, vcountNew);
			}
		}
		ExportLib lib;
		lib.name = ReadString(mem, nameAddr);
		lib.version = version;
		lib.attr = attr;
		u32 total = fcount + varCount;
		for (u32 i = 0; i < total; i++) {
			u32 nid, addr;
			if (!ReadU32(mem, resident + i * 4, nid) || !ReadU32(mem, resident + (total + i) * 4, addr)) break;
			if (i < fcount) lib.funcs.push_back({nid, addr});
			else lib.vars.push_back({nid, addr});
		}
		out.push_back(std::move(lib));
		pos += size * 4;
	}
	return true;
}

}  // namespace loader
