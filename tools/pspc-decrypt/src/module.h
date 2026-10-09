// SPDX-License-Identifier: GPL-3.0-or-later
// Turning whatever a PSP executable arrives as into a plain ELF: peel ~SCE / EDAT / PBP
// wrappers, decrypt ~PSP, unpack custom-firmware gzip packing, decompress gzip/KL4E/KL3E/2RLZ.
#pragma once

#include "common.h"
#include "json.h"
#include "prx_decrypter.h"

enum class DecodeStatus {
	Ok,
	NeedsKey,  // Unknown tag, or an NPDRM seed/licensee key we weren't given.
	Failed,
};

struct DecodeOptions {
	const u8 *seed = nullptr;         // 16-byte NPDRM seed for type 5 modules.
	const u8 *licenseeKey = nullptr;  // 16-byte licensee key, for EDAT modules that need it.
};

struct DecodeResult {
	DecodeStatus status = DecodeStatus::Failed;
	std::string error;
	Bytes output;                       // The final plain image (an ELF for every real module).
	std::vector<std::string> steps;     // What was peeled off, in order.
	std::vector<std::string> warnings;

	bool hadPspHeader = false;
	PspHeader psp{};
	bool encrypted = false;             // KIRK-encrypted (as opposed to plain or CFW-packed).
	bool cfwPacked = false;             // ~PSP header in front of a plain gzip stream (M33 psp-packer).
	int prxType = -1;
	u32 tag = 0;
	std::string compression = "none";   // none, gzip, KL4E, KL3E, 2RLZ
	// A second compressed stream after the first one (some firmware files carry two).
	Bytes trailing;
};

// `data`/`size` is the whole file. Never throws; check result.status.
void DecodeExecutable(const u8 *data, size_t size, const DecodeOptions &options, DecodeResult *result);

// Decompresses a buffer that starts with a gzip, zlib, KL4E, KL3E or 2RLZ magic. `sizeHint` is
// the expected output size (0 if unknown). `consumed` gets how much of the input the stream used.
bool DecompressAny(const u8 *data, size_t size, size_t sizeHint, Bytes *out, std::string *method,
	size_t *consumed, std::string *error);

// The facts `info` prints for a ~PSP header.
Json DescribePspHeader(const PspHeader &h);
// Everything we can say about a file without writing anything.
// `baseOffset` is where `data` sits in the file, for nested sections.
Json DescribeFile(const u8 *data, size_t size, const DecodeOptions &options, size_t baseOffset = 0);

// "~PSP" headers embedded in a larger file (M33's installer carries eleven inside its ELF,
// loadexec carries reboot.bin). Returns their offsets; each is skipped past once found.
std::vector<size_t> FindEmbeddedModules(const u8 *data, size_t size, size_t start = 0);
