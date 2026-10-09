// SPDX-License-Identifier: GPL-3.0-or-later
// Sony system software updater (DATA.PSAR) unpacking.
// Ported from PPSSPP's Core/Util/PSARUnpack.cpp (GPL-2.0-or-later, Copyright (c) 2026- PPSSPP
// Project; see its docs/PsarFileFormat.md) with details from John-K/pspdecrypt's
// PsarDecrypter.cpp (GPL-3.0; newpsardumper by PspPet, Vampire, Nem, Dark_AleX, Noobz, Team C+D,
// M33 Team, bbtgp, Proxima, some1, artart78): version 5 demangling, table key 5, IPL handling
// and pulling reboot.bin out of loadexec.
#pragma once

#include "common.h"
#include "json.h"
#include "module.h"

struct PsarOptions {
	std::string model = "any";   // "any", or "01g".."12g": whose file lists name files.
	bool keepEncrypted = false;  // Also write the original encrypted modules under encrypted/.
	bool verbose = false;        // One line per entry on stderr.
	DecodeOptions decode;
};

struct PsarSummary {
	std::string firmwareVersion;
	int psarVersion = 0;
	int entries = 0;
	int files = 0;
	int directories = 0;
	int fileLists = 0;
	int modules = 0;
	int modulesDecrypted = 0;
	int needsKey = 0;
	int failed = 0;
	int unnamed = 0;
	int otherModel = 0;
	int conflicts = 0;
	int derived = 0;
};

// Accepts an updater EBOOT.PBP (DATA.PSAR section) or a bare PSAR (DATA.PSAR, a disc's DATA.BIN).
bool LocatePsar(const u8 *data, size_t size, const u8 **psar, size_t *psarSize, std::string *error);

// Unpacks into outDir: flash0/, flash1/, ipl/ ... as the archive names them, with every module
// decrypted and decompressed, plus manifest.json. Returns false only if the archive itself can't
// be walked; per-file problems are counted in the summary and listed in the manifest.
bool UnpackPsar(const u8 *psar, size_t size, const std::string &outDir, const PsarOptions &options,
	Json *manifest, PsarSummary *summary, std::string *error);

// Version byte, encryption state and the firmware version from the first record.
Json DescribePsar(const u8 *psar, size_t size);
