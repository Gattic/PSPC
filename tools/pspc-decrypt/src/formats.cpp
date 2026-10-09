// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats.h"

#include "inflate.h"

#include "../libkirk/AES.h"
#include "../libkirk/amctrl.h"
#include "../libkirk/kirk_engine.h"

FileKind SniffKind(const u8 *data, size_t size) {
	if (HasMagic(data, size, "\x7F" "ELF", 4)) return FileKind::Elf;
	if (HasMagic(data, size, "~PSP", 4)) return FileKind::PspModule;
	if (HasMagic(data, size, "~SCE", 4)) return FileKind::SceWrapped;
	if (HasMagic(data, size, "\0PSPEDAT", 8)) return FileKind::Edat;
	if (HasMagic(data, size, "\0PBP", 4)) return FileKind::Pbp;
	if (HasMagic(data, size, "PSAR", 4)) return FileKind::Psar;
	if (HasMagic(data, size, "\0PSF", 4)) return FileKind::Sfo;
	if (HasMagic(data, size, "KL4E", 4)) return FileKind::KL4E;
	if (HasMagic(data, size, "KL3E", 4)) return FileKind::KL3E;
	if (HasMagic(data, size, "2RLZ", 4)) return FileKind::LZRC;
	if (HasMagic(data, size, "NPUMDIMG", 8)) return FileKind::NpUmdImg;
	if (HasMagic(data, size, "\x89" "PNG", 4)) return FileKind::Png;
	if (HasMagic(data, size, "PSMF", 4)) return FileKind::Pmf;
	if (HasMagic(data, size, "RIFF", 4)) return FileKind::Riff;
	if (IsGzipMagic(data, size)) return FileKind::Gzip;
	if (size >= 2 && data[0] == 0x78 && IsZlibHeader(data, size)) return FileKind::Zlib;
	return FileKind::Unknown;
}

const char *FileKindName(FileKind kind) {
	switch (kind) {
	case FileKind::Elf: return "elf";
	case FileKind::PspModule: return "~PSP";
	case FileKind::SceWrapped: return "~SCE";
	case FileKind::Edat: return "PSPEDAT";
	case FileKind::Pbp: return "PBP";
	case FileKind::Psar: return "PSAR";
	case FileKind::Sfo: return "PARAM.SFO";
	case FileKind::Gzip: return "gzip";
	case FileKind::Zlib: return "zlib";
	case FileKind::KL4E: return "KL4E";
	case FileKind::KL3E: return "KL3E";
	case FileKind::LZRC: return "2RLZ";
	case FileKind::NpUmdImg: return "NPUMDIMG";
	case FileKind::Png: return "PNG";
	case FileKind::Pmf: return "PSMF";
	case FileKind::Riff: return "RIFF";
	default: return "unknown";
	}
}

const char *const PbpFile::kNames[PbpFile::kSections] = {
	"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG", "PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR",
};

bool PbpFile::Parse(const u8 *data, size_t fileSize, std::string *error) {
	if (fileSize < 0x28 || !HasMagic(data, fileSize, "\0PBP", 4)) {
		*error = "not a PBP";
		return false;
	}
	version = rd32(data + 4);
	for (int i = 0; i < kSections; i++) {
		offset[i] = rd32(data + 8 + i * 4);
	}
	for (int i = 0; i < kSections; i++) {
		const u64 end = i + 1 < kSections ? offset[i + 1] : fileSize;
		if (offset[i] < 0x28 || offset[i] > fileSize || end < offset[i] || end > fileSize) {
			*error = StringPrintf("PBP section %s has a bad offset (0x%x)", kNames[i], offset[i]);
			return false;
		}
		size[i] = (u32)(end - offset[i]);
	}
	return true;
}

bool ParseSfo(const u8 *data, size_t size, std::map<std::string, SfoEntry> *out) {
	if (size < 0x14 || !HasMagic(data, size, "\0PSF", 4)) {
		return false;
	}
	const u32 keyTable = rd32(data + 8);
	const u32 dataTable = rd32(data + 12);
	const u32 count = rd32(data + 16);
	if (count > 1024 || 0x14 + (u64)count * 16 > size) {
		return false;
	}
	for (u32 i = 0; i < count; i++) {
		const u8 *e = data + 0x14 + i * 16;
		const u32 keyAt = keyTable + rd16(e);
		const u16 fmt = rd16(e + 2);
		const u32 len = rd32(e + 4);
		const u32 dataAt = dataTable + rd32(e + 12);
		if (keyAt >= size || dataAt > size || len > size - dataAt) {
			return false;
		}
		const std::string key = FixedString(data + keyAt, size - keyAt);
		SfoEntry entry;
		if (fmt == 0x0404 && len >= 4) {
			entry.isInt = true;
			entry.intValue = rd32(data + dataAt);
		} else {
			std::string s((const char *)data + dataAt, len);
			const size_t nul = s.find('\0');
			if (nul != std::string::npos) {
				s.resize(nul);
			}
			entry.str = s;
		}
		(*out)[key] = entry;
	}
	return true;
}

Json SfoToJson(const std::map<std::string, SfoEntry> &sfo) {
	Json j = Json::Object();
	for (const auto &[key, entry] : sfo) {
		if (entry.isInt) {
			j.Set(key, (long long)entry.intValue);
		} else {
			j.Set(key, entry.str);
		}
	}
	return j;
}

bool IsElf(const u8 *data, size_t size) {
	return HasMagic(data, size, "\x7F" "ELF", 4);
}

u16 ElfType(const u8 *data, size_t size) {
	return size >= 0x34 && IsElf(data, size) ? rd16(data + 0x10) : 0;
}

// SceModuleInfo: u16 attribute, u8 version[2], char name[28], then gp and the lib tables.
static bool ReadModuleInfo(const u8 *data, size_t size, u64 offset, Json *out) {
	if (offset + 0x34 > size) {
		return false;
	}
	const u8 *mi = data + offset;
	const std::string name = FixedString(mi + 4, 28);
	if (name.empty()) {
		return false;
	}
	Json j = Json::Object();
	j.Set("name", name);
	j.Set("attribute", Hex32(rd16(mi)));
	j.Set("version", StringPrintf("%d.%d", mi[3], mi[2]));
	j.Set("gp", Hex32(rd32(mi + 0x20)));
	j.Set("file_offset", Hex32((u32)offset));
	*out = j;
	return true;
}

Json DescribeElf(const u8 *data, size_t size) {
	Json j = Json::Object();
	if (size < 0x34 || !IsElf(data, size)) {
		j.Set("error", "not an ELF");
		return j;
	}
	const u16 type = rd16(data + 0x10);
	j.Set("class", data[4] == 1 ? "ELF32" : data[4] == 2 ? "ELF64" : "?");
	j.Set("endian", data[5] == 1 ? "little" : data[5] == 2 ? "big" : "?");
	j.Set("type", Hex32(type));
	j.Set("type_name", type == 0xFFA0 ? "PRX (relocatable PSP module)" : type == 2 ? "EXEC (static executable)" :
		type == 1 ? "REL" : "other");
	j.Set("machine", rd16(data + 0x12) == 8 ? "MIPS" : Hex32(rd16(data + 0x12)));
	j.Set("entry", Hex32(rd32(data + 0x18)));
	j.Set("flags", Hex32(rd32(data + 0x24)));
	const u32 phoff = rd32(data + 0x1C);
	const u32 shoff = rd32(data + 0x20);
	const u16 phentsize = rd16(data + 0x2A), phnum = rd16(data + 0x2C);
	const u16 shentsize = rd16(data + 0x2E), shnum = rd16(data + 0x30), shstrndx = rd16(data + 0x32);
	j.Set("program_headers", phnum);
	j.Set("section_headers", shnum);
	j.Set("size", (unsigned long long)size);

	Json modinfo;
	bool found = false;
	// A PRX keeps the module info's file offset in the first segment's p_paddr (the top bit marks
	// a kernel module). psp-packer reads it the same way.
	if (type == 0xFFA0 && phnum > 0 && phentsize >= 0x20 && (u64)phoff + 0x20 <= size) {
		found = ReadModuleInfo(data, size, rd32(data + phoff + 0x0C) & 0x7FFFFFFF, &modinfo);
	}
	// Otherwise look for the section by name.
	if (!found && shnum > 0 && shentsize >= 0x28 && shstrndx < shnum && (u64)shoff + (u64)shnum * shentsize <= size) {
		const u8 *strsh = data + shoff + (size_t)shstrndx * shentsize;
		const u32 strOff = rd32(strsh + 0x10), strSize = rd32(strsh + 0x14);
		if ((u64)strOff + strSize <= size) {
			for (u16 i = 0; i < shnum && !found; i++) {
				const u8 *sh = data + shoff + (size_t)i * shentsize;
				const u32 nameOff = rd32(sh);
				if (nameOff < strSize && FixedString(data + strOff + nameOff, strSize - nameOff) == ".rodata.sceModuleInfo") {
					found = ReadModuleInfo(data, size, rd32(sh + 0x10), &modinfo);
				}
			}
		}
	}
	if (found) {
		j.Set("module_info", modinfo);
	}
	return j;
}

bool ParseEdat(const u8 *data, size_t size, EdatHeader *out) {
	if (size < 0x90 || !HasMagic(data, size, "\0PSPEDAT", 8)) {
		return false;
	}
	out->keyMode = data[0x08];
	out->payloadOffset = rd16(data + 0x0C);
	out->payloadType = rd16(data + 0x0E);
	out->flags = data[0x0F];
	out->contentId = FixedString(data + 0x10, 0x30);
	return true;
}

// The last pass over a module key, from npdrm.prx's module path. Via PPSSPP's
// Core/HLE/scePspNpDrm_user.cpp (GPL-2.0-or-later), worked out from JPCSP.
static const u8 kDrmModuleKey[16] = {
	0xBA, 0x87, 0xE4, 0xAB, 0x2C, 0x60, 0x5F, 0x59, 0xB8, 0x3B, 0xDB, 0xA6, 0x82, 0xFD, 0xAE, 0x14,
};

bool DeriveEdatModuleSeed(const u8 *edat, size_t size, u8 seed[16], const u8 *licenseeKey) {
	EdatHeader h;
	if (!ParseEdat(edat, size, &h)) {
		return false;
	}
	if ((h.flags & 1) && !licenseeKey) {
		return false;
	}
	KirkState kirk{};
	kirk_init(&kirk);
	char contentId[0x31] = {};
	memcpy(contentId, edat + 0x10, 0x30);
	if (sceNpDrmGetFixedKey(&kirk, seed, contentId, 0x01000000 | h.keyMode) != 0) {
		return false;
	}
	if (h.flags & 1) {
		for (int i = 0; i < 16; i++) {
			seed[i] ^= licenseeKey[i];
		}
	}
	if (h.flags & 2) {
		for (int i = 0; i < 16; i++) {
			seed[i] ^= edat[0x40 + i];
		}
	}
	AES_ctx ctx;
	AES_set_key(&ctx, kDrmModuleKey, 128);
	AES_decrypt(&ctx, seed, seed);
	return true;
}
