// SPDX-License-Identifier: GPL-3.0-or-later
// Container formats around PSP executables: PBP, PARAM.SFO, ELF headers, NPDRM EDAT.
#pragma once

#include <map>

#include "common.h"
#include "json.h"

enum class FileKind {
	Unknown,
	Elf,
	PspModule,   // "~PSP"
	SceWrapped,  // "~SCE" header in front of a ~PSP
	Edat,        // "\0PSPEDAT" (NPDRM .sprx/.edat)
	Pbp,
	Psar,
	Sfo,
	Gzip,
	Zlib,
	KL4E,
	KL3E,
	LZRC,        // "2RLZ"
	NpUmdImg,    // PSN game image (DATA.PSAR of a PSN EBOOT)
};

FileKind SniffKind(const u8 *data, size_t size);
const char *FileKindName(FileKind kind);

// PBP: "\0PBP", version, then eight section offsets.
struct PbpFile {
	static const int kSections = 8;
	static const char *const kNames[kSections];
	enum { PARAM_SFO, ICON0_PNG, ICON1_PMF, PIC0_PNG, PIC1_PNG, SND0_AT3, DATA_PSP, DATA_PSAR };

	u32 version = 0;
	u32 offset[kSections] = {};
	u32 size[kSections] = {};

	bool Parse(const u8 *data, size_t fileSize, std::string *error);
};

// PARAM.SFO key/value pairs. Integers are stored as decimal strings with a flag.
struct SfoEntry {
	bool isInt = false;
	u32 intValue = 0;
	std::string str;
};
bool ParseSfo(const u8 *data, size_t size, std::map<std::string, SfoEntry> *out);
Json SfoToJson(const std::map<std::string, SfoEntry> &sfo);

// ELF32 header facts, plus the PSP module info when it can be found.
Json DescribeElf(const u8 *data, size_t size);
bool IsElf(const u8 *data, size_t size);
u16 ElfType(const u8 *data, size_t size);

// NPDRM "\0PSPEDAT" header: content ID, flags, payload offset.
struct EdatHeader {
	std::string contentId;
	u32 keyMode = 0;     // low byte of the u32 at 0x08
	u8 flags = 0;        // 0x0F: bit 0 needs the licensee key, bit 1 extra XOR
	u32 payloadOffset = 0;
	u16 payloadType = 0; // 0x0E: 0x0101 module, 0x0003 PGD data
};
bool ParseEdat(const u8 *data, size_t size, EdatHeader *out);
// The NPDRM module seed (version key) for an EDAT-wrapped module, as npdrm.prx derives it.
// Returns false when it needs the licensee key (flags bit 0) and none was given, or if the
// derivation fails.
bool DeriveEdatModuleSeed(const u8 *edat, size_t size, u8 seed[16], const u8 *licenseeKey);
