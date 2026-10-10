// SPDX-License-Identifier: GPL-2.0-or-later
// Portions derived from PPSSPP (https://github.com/hrydgard/ppsspp), Copyright (c) 2012- PPSSPP
// Project, GPL-2.0-or-later: relocation handling (ElfReader::LoadRelocations / LoadRelocations2).
#include "loader/elf.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace loader {

static u32 Rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 Rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

static void SetErr(std::string *err, const char *fmt, ...) {
	if (!err) return;
	char buf[512];
	va_list a;
	va_start(a, fmt);
	vsnprintf(buf, sizeof(buf), fmt, a);
	va_end(a);
	*err = buf;
}

bool ElfFile::Parse(const u8 *data, size_t size, std::string *err) {
	data_ = data;
	size_ = size;
	if (size < 52 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
		SetErr(err, "not an ELF file");
		return false;
	}
	if (data[5] != 1) {
		SetErr(err, "not a little-endian ELF");
		return false;
	}
	type_ = Rd16(data + 16);
	u16 machine = Rd16(data + 18);
	if (machine != 8) {
		SetErr(err, "not a MIPS ELF (machine %u)", machine);
		return false;
	}
	entry_ = Rd32(data + 24);
	u32 phoff = Rd32(data + 28), shoff = Rd32(data + 32);
	u16 phentsize = Rd16(data + 42), phnum = Rd16(data + 44);
	u16 shentsize = Rd16(data + 46), shnum = Rd16(data + 48), shstrndx = Rd16(data + 50);

	if (phnum && (phentsize < 32 || (u64)phoff + (u64)phentsize * phnum > size)) {
		SetErr(err, "truncated program header table");
		return false;
	}
	if (shnum && (shentsize < 40 || (u64)shoff + (u64)shentsize * shnum > size)) {
		// Some PRXs carry bogus section tables; treat as having none.
		shnum = 0;
	}

	segments_.clear();
	for (u32 i = 0; i < phnum; i++) {
		const u8 *p = data + phoff + i * phentsize;
		Segment s;
		s.type = Rd32(p);
		s.offset = Rd32(p + 4);
		s.vaddr = Rd32(p + 8);
		s.paddr = Rd32(p + 12);
		s.filesz = Rd32(p + 16);
		s.memsz = Rd32(p + 20);
		s.flags = Rd32(p + 24);
		s.align = Rd32(p + 28);
		segments_.push_back(s);
	}

	sections_.clear();
	std::vector<u32> nameOffsets;
	for (u32 i = 0; i < shnum; i++) {
		const u8 *p = data + shoff + i * shentsize;
		Section s;
		nameOffsets.push_back(Rd32(p));
		s.type = Rd32(p + 4);
		s.flags = Rd32(p + 8);
		s.addr = Rd32(p + 12);
		s.offset = Rd32(p + 16);
		s.size = Rd32(p + 20);
		s.link = Rd32(p + 24);
		s.info = Rd32(p + 28);
		s.align = Rd32(p + 32);
		s.entsize = Rd32(p + 36);
		sections_.push_back(s);
	}
	if (shstrndx < sections_.size()) {
		const Section &strtab = sections_[shstrndx];
		for (size_t i = 0; i < sections_.size(); i++) {
			u64 off = (u64)strtab.offset + nameOffsets[i];
			if (off < size) {
				const char *start = (const char *)data + off;
				size_t maxLen = size - (size_t)off;
				sections_[i].name.assign(start, strnlen(start, maxLen));
			}
		}
	}

	totalStart_ = 0xFFFFFFFF;
	totalEnd_ = 0;
	int loadSegs = 0;
	for (const Segment &s : segments_) {
		if (s.type != PT_LOAD) continue;
		loadSegs++;
		if (s.vaddr < totalStart_) {
			totalStart_ = s.vaddr;
			firstSegAlign_ = s.align;
		}
		totalEnd_ = std::max(totalEnd_, s.vaddr + s.memsz);
	}
	if (!loadSegs) {
		SetErr(err, "ELF has no loadable segments");
		return false;
	}

	// Module info: the .rodata.sceModuleInfo section, else the first segment's p_paddr.
	modInfoOffset_ = 0xFFFFFFFF;
	modInfoVaddr_ = 0xFFFFFFFF;
	for (const Section &s : sections_) {
		if (s.name == ".rodata.sceModuleInfo" && (u64)s.offset + sizeof(PspModuleInfoRaw) <= size) {
			modInfoOffset_ = s.offset;
			modInfoVaddr_ = s.addr;
			break;
		}
	}
	if (modInfoOffset_ == 0xFFFFFFFF && !segments_.empty()) {
		u32 off = segments_[0].paddr & 0x7FFFFFFF;
		if ((u64)off + sizeof(PspModuleInfoRaw) <= size) {
			modInfoOffset_ = off;
			// Translate the file offset into a vaddr through the segment that contains it.
			for (const Segment &s : segments_) {
				if (s.type == PT_LOAD && off >= s.offset && off < s.offset + s.filesz) {
					modInfoVaddr_ = s.vaddr + (off - s.offset);
					break;
				}
			}
		}
	}
	if (modInfoOffset_ != 0xFFFFFFFF) memcpy(&modInfo_, data + modInfoOffset_, sizeof(modInfo_));
	return true;
}

bool ElfFile::Load(u32 base, const std::function<u8 *(u32, u32)> &ptr, LoadedImage &out, std::string *err) const {
	out = LoadedImage();
	out.relocatable = Relocatable();
	out.base = Relocatable() ? base : 0;
	out.lowAddr = out.base + totalStart_;
	out.size = totalEnd_ - totalStart_;
	// 0xFFFFFFFF: no entry point (a library without module_start), not relocated.
	out.entry = entry_ == 0xFFFFFFFF ? 0xFFFFFFFF : entry_ + out.base;
	out.segments = segments_;
	out.sections = sections_;

	std::vector<u32> segAddr(segments_.size(), 0xFFFFFFFF);
	for (size_t i = 0; i < segments_.size(); i++) {
		Segment &s = out.segments[i];
		if (s.type != PT_LOAD) continue;
		if ((u64)s.offset + s.filesz > size_ || s.filesz > s.memsz) {
			SetErr(err, "segment %d out of file bounds", (int)i);
			return false;
		}
		u32 dst = out.base + s.vaddr;
		u8 *p = ptr(dst, s.memsz);
		if (!p && s.memsz) {
			SetErr(err, "segment %d target %08x+%x not mapped", (int)i, dst, s.memsz);
			return false;
		}
		if (s.memsz) {
			memcpy(p, data_ + s.offset, s.filesz);
			memset(p + s.filesz, 0, s.memsz - s.filesz);
		}
		s.loadAddr = dst;
		segAddr[i] = dst;
		if (s.flags & PF_X) out.codeRanges.push_back({dst, dst + s.memsz});
	}
	for (Section &s : out.sections) {
		if (s.flags & SHF_ALLOC) s.loadAddr = out.base + s.addr;
	}
	// Prefer section-granular code ranges when sections exist (excludes stubs data etc. less).
	{
		std::vector<std::pair<u32, u32>> secRanges;
		for (const Section &s : out.sections) {
			if ((s.flags & (SHF_ALLOC | SHF_EXECINSTR)) == (SHF_ALLOC | SHF_EXECINSTR) && s.size)
				secRanges.push_back({s.loadAddr, s.loadAddr + s.size});
		}
		if (!secRanges.empty()) out.codeRanges = secRanges;
	}

	if (modInfoOffset_ != 0xFFFFFFFF) {
		out.modInfo = modInfo_;
		out.modInfoAddr = modInfoVaddr_ != 0xFFFFFFFF ? out.base + modInfoVaddr_ : 0;
	}

	if (Relocatable()) {
		bool any = false;
		for (const Section &s : sections_) {
			if (s.type == SHT_PSPREL) {
				if (s.info < sections_.size() && !(sections_[s.info].flags & SHF_ALLOC)) continue;
				if ((u64)s.offset + s.size > size_) continue;
				ApplyRelocsA(data_ + s.offset, s.size / 8, segAddr, ptr, out);
				any = true;
			}
		}
		if (!any && sections_.empty()) {
			for (size_t i = 0; i < segments_.size(); i++) {
				const Segment &s = segments_[i];
				if ((u64)s.offset + s.filesz > size_) continue;
				if (s.type == PT_PSPREL1) {
					ApplyRelocsA(data_ + s.offset, s.filesz / 8, segAddr, ptr, out);
				} else if (s.type == PT_PSPREL2) {
					ApplyRelocsB((int)i, segAddr, ptr, out);
				}
			}
		}
	}

	// Module info after relocation (its pointers were relocated in memory).
	if (out.modInfoAddr) {
		u8 *p = ptr(out.modInfoAddr, sizeof(PspModuleInfoRaw));
		if (p) memcpy(&out.modInfo, p, sizeof(PspModuleInfoRaw));
	}
	return true;
}

static void AddrToHiLo(u32 addr, u16 &hi, s16 &lo) {
	lo = (s16)(addr & 0xFFFF);
	hi = (u16)((addr - (u32)(s32)lo) >> 16);
}

void ElfFile::ApplyRelocsA(const u8 *rel, u32 n, const std::vector<u32> &segAddr,
                           const std::function<u8 *(u32, u32)> &ptr, LoadedImage &out) const {
	auto loaded = [&](u32 seg) { return seg < segAddr.size() && segAddr[seg] != 0xFFFFFFFF; };
	// First pass: capture original words (HI16 pairing needs unrelocated LO16 values).
	std::vector<u32> orig(n, 0);
	std::vector<u8> valid(n, 0);
	for (u32 r = 0; r < n; r++) {
		u32 off = Rd32(rel + r * 8), info = Rd32(rel + r * 8 + 4);
		u32 type = info & 0xF, seg = (info >> 8) & 0xFF;
		if (!loaded(seg)) {
			out.relocErrors++;
			continue;
		}
		u32 addr = off + segAddr[seg];
		if ((addr & 3) && type != R_MIPS_32) {
			out.relocErrors++;
			continue;
		}
		u8 *p = ptr(addr, 4);
		if (!p) {
			out.relocErrors++;
			continue;
		}
		orig[r] = Rd32(p);
		valid[r] = 1;
	}
	for (u32 r = 0; r < n; r++) {
		if (!valid[r]) continue;
		u32 off = Rd32(rel + r * 8), info = Rd32(rel + r * 8 + 4);
		u32 type = info & 0xF, seg = (info >> 8) & 0xFF, relSeg = (info >> 16) & 0xFF;
		u32 addr = off + segAddr[seg];
		u32 relocateTo = loaded(relSeg) ? segAddr[relSeg] : 0;
		u32 op = orig[r];
		switch (type) {
		case R_MIPS_32: op += relocateTo; break;
		case R_MIPS_26: op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (relocateTo >> 2)) & 0x03FFFFFF); break;
		case R_MIPS_HI16: {
			u32 cur = (op & 0xFFFF) << 16;
			bool found = false;
			for (u32 t = r + 1; t < n; t++) {
				u32 tinfo = Rd32(rel + t * 8 + 4);
				u32 ttype = tinfo & 0xF;
				if (ttype == R_MIPS_HI16) continue;
				if (!valid[t]) continue;
				if (ttype != R_MIPS_LO16 && ttype != R_MIPS_16) continue;
				cur += (u32)(s32)(s16)(orig[t] & 0xFFFF);
				cur += relocateTo;
				u16 hi;
				s16 lo;
				AddrToHiLo(cur, hi, lo);
				op = (op & 0xFFFF0000) | hi;
				found = true;
				break;
			}
			if (!found) {
				out.relocErrors++;
				continue;
			}
			break;
		}
		case R_MIPS_LO16: op = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocateTo) & 0xFFFF); break;
		case R_MIPS_16: op = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocateTo) & 0xFFFF); break;
		case R_MIPS_GPREL16:
		case R_MIPS_NONE: continue;
		default: out.relocErrors++; continue;
		}
		u8 *p = ptr(addr, 4);
		memcpy(p, &op, 4);
		out.relocs.push_back({addr, (u8)type, relocateTo});
	}
}

void ElfFile::ApplyRelocsB(int relSeg, const std::vector<u32> &segAddr, const std::function<u8 *(u32, u32)> &ptr,
                           LoadedImage &out) const {
	const Segment &ph = segments_[relSeg];
	const u8 *buf = data_ + ph.offset;
	const u8 *end = buf + ph.filesz;
	auto loaded = [&](int seg) { return seg >= 0 && seg < (int)segAddr.size() && segAddr[seg] != 0xFFFFFFFF; };
	if (end - buf < 4) return;
	int flagBits = buf[2], typeBits = buf[3];
	int segBits = 1;
	while ((1 << segBits) < relSeg) segBits++;
	buf += 4;
	if (end - buf < 1) return;
	const u8 *flagTable = buf;
	int flagTableSize = flagTable[0];
	if (end - buf < flagTableSize) return;
	buf += flagTableSize;
	if (end - buf < 1) return;
	const u8 *typeTable = buf;
	int typeTableSize = typeTable[0];
	if (end - buf < typeTableSize) return;
	buf += typeTableSize;

	int offSeg = 0, relBase = 0, lastType = -1, lo16 = 0;
	while (buf < end) {
		if (end - buf < 2) return;
		int cmd = buf[0] | (buf[1] << 8);
		buf += 2;
		int flag = (cmd << (16 - flagBits)) & 0xFFFF;
		flag = (flag >> (16 - flagBits)) & 0xFFFF;
		if (flag >= flagTableSize) return;
		flag = flagTable[flag];
		int seg = (cmd << (16 - segBits - flagBits)) & 0xFFFF;
		seg = (seg >> (16 - segBits)) & 0xFFFF;
		int type = (cmd << (16 - typeBits - segBits - flagBits)) & 0xFFFF;
		type = (type >> (16 - typeBits)) & 0xFFFF;
		if (type >= typeTableSize) return;
		type = typeTable[type];

		if ((flag & 0x01) == 0) {
			offSeg = seg;
			if ((flag & 0x06) == 0) {
				relBase = cmd >> (segBits + flagBits);
			} else if ((flag & 0x06) == 4) {
				if (end - buf < 4) return;
				relBase = (int)Rd32(buf);
				buf += 4;
			} else {
				relBase = 0;
			}
			continue;
		}
		int addrSeg = seg;
		if (!loaded(addrSeg)) continue;
		u32 relocateTo = segAddr[addrSeg];
		int relOffset;
		if ((flag & 0x06) == 0x00) {
			relOffset = cmd;
			if (cmd & 0x8000) {
				relOffset |= (int)0xFFFF0000;
				relOffset >>= typeBits + segBits + flagBits;
				relOffset |= (int)0xFFFF0000;
			} else {
				relOffset >>= typeBits + segBits + flagBits;
			}
			relBase += relOffset;
		} else if ((flag & 0x06) == 0x02) {
			relOffset = cmd;
			if (cmd & 0x8000) relOffset |= (int)0xFFFF0000;
			relOffset >>= typeBits + segBits + flagBits;
			if (end - buf < 2) return;
			relOffset = (relOffset << 16) | buf[0] | (buf[1] << 8);
			buf += 2;
			relBase += relOffset;
		} else if ((flag & 0x06) == 0x04) {
			if (end - buf < 4) return;
			relBase = (int)Rd32(buf);
			buf += 4;
		}
		if (!loaded(offSeg)) continue;
		u32 addr = (u32)relBase + segAddr[offSeg];

		if ((flag & 0x38) == 0x00) {
			lo16 = 0;
		} else if ((flag & 0x38) == 0x08) {
			if (lastType != 0x04) lo16 = 0;
		} else if ((flag & 0x38) == 0x10) {
			if (end - buf < 2) return;
			lo16 = buf[0] | (buf[1] << 8);
			if (lo16 & 0x8000) lo16 |= (int)0xFFFF0000;
			buf += 2;
		}

		u8 *p = ptr(addr, 4);
		if (!p) {
			out.relocErrors++;
			continue;
		}
		u32 op = Rd32(p);
		u8 normType = R_MIPS_NONE;
		switch (type) {
		case 0: break;
		case 2:
			op += relocateTo;
			normType = R_MIPS_32;
			break;
		case 3:
		case 6:
		case 7:
			op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (relocateTo >> 2)) & 0x03FFFFFF);
			if (type == 6) op = (op & ~0xFC000000u) | 0x08000000u;
			else if (type == 7) op = (op & ~0xFC000000u) | 0x0C000000u;
			normType = R_MIPS_26;
			break;
		case 4: {
			u32 a = ((op << 16) + (u32)lo16) + relocateTo;
			if (a & 0x8000) a += 0x00010000;
			op = (op & 0xFFFF0000) | (a >> 16);
			normType = R_MIPS_HI16;
			break;
		}
		case 1:
		case 5:
			op = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocateTo) & 0xFFFF);
			normType = R_MIPS_LO16;
			break;
		default: out.relocErrors++; break;
		}
		memcpy(p, &op, 4);
		if (normType != R_MIPS_NONE) out.relocs.push_back({addr, normType, relocateTo});
		lastType = type;
	}
}

}  // namespace loader
