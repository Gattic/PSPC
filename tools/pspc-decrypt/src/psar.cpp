// SPDX-License-Identifier: GPL-3.0-or-later
// PSAR (firmware updater archive) unpacking.
//
// The archive walk, the naming rules and the DES file lists are ported from PPSSPP's
// Core/Util/PSARUnpack.cpp (Copyright (c) 2026- PPSSPP Project, GPL-2.0-or-later); the format is
// described in PPSSPP's docs/PsarFileFormat.md. Version 5 demangling, the sixth table key's
// selection, the pre-DES-only tables of version 4, IPL handling and reboot.bin extraction follow
// John-K/pspdecrypt's PsarDecrypter.cpp / pspdecrypt_lib.cpp (GPL-3.0).

#include "psar.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "des.h"
#include "formats.h"
#include "inflate.h"
#include "ipl.h"
#include "prx_decrypter.h"

#include "../libkirk/kirk_engine.h"

namespace {

// A PSAR record is [header][entry], where the header is 0x150 bytes of PRX-style encryption
// metadata and the entry is 0x110 bytes describing one file. Pre-decrypted archives (rare, and
// only produced by other tools) have no header at all.
const u32 PSAR_MAGIC = 0x52415350;  // "PSAR"
const u32 PSAR_DECRYPTED_MARKER = 0x2C333333;
const u32 PSAR_ENTRY_SIZE = 0x110;
const u32 PSAR_HEADER_SIZE = 0x150;
// The KIRK CMD7 key the "demangle" step below uses. Same for every firmware.
const int PSAR_DEMANGLE_KEYSEED = 0x55;
// Sanity bound on the sizes we take from the archive, so a corrupt one can't ask for a huge
// allocation. No file inside an updater comes close.
const u32 PSAR_MAX_ENTRY_BYTES = 64 * 1024 * 1024;

// Version 5 archives XOR the mangled area with these before and after the KIRK pass (pspdecrypt).
const u8 kDemangleK1[16] = { 0xD8, 0x69, 0xB8, 0x95, 0x33, 0x6B, 0x63, 0x34, 0x98, 0xB9, 0xFC, 0x3C, 0xB7, 0x26, 0x2B, 0xD7 };
const u8 kDemangleK2[16] = { 0x0D, 0xA0, 0x90, 0x84, 0xAF, 0x9E, 0xB6, 0xE2, 0xD2, 0x94, 0xF2, 0xAA, 0xEF, 0x99, 0x68, 0x71 };

// One DES key and IV per firmware series, for the file lists.
struct PSARTableKey {
	u32 keyLow;
	u32 keyHigh;
	u8 iv[8];
};

const PSARTableKey kTableKeys[] = {
	{ 0xB730E5C7, 0x95620B49, { 0x9E, 0xA4, 0x33, 0x81, 0x86, 0x0C, 0x52, 0x85 } },
	{ 0x45C9DC95, 0x5A7B3D9D, { 0xB2, 0xFE, 0xD9, 0x79, 0x8A, 0x02, 0xB1, 0x87 } },
	{ 0x6F20585A, 0x4CCE495B, { 0x81, 0x08, 0xC1, 0xF2, 0x35, 0x98, 0x69, 0xB0 } },
	{ 0x620BF15A, 0x73F45262, { 0x6D, 0x52, 0x1B, 0xA3, 0xC2, 0x36, 0xF9, 0x2B } },
	{ 0xFD9D4498, 0xA664C8F8, { 0xDB, 0x4E, 0x79, 0x41, 0xF5, 0x97, 0x30, 0xAD } },
	{ 0x3D6426E7, 0xD7BD7481, { 0xA6, 0x83, 0x0C, 0x2F, 0x63, 0x0B, 0x96, 0x29 } },
};

// "6.61" -> 661. A single-digit minor is still tens: "5.5" is 550. 0 if it isn't a version.
int FirmwareVersionToInt(const std::string &version) {
	if (version.size() < 3 || version[0] < '0' || version[0] > '9' || version[1] != '.') {
		return 0;
	}
	int minor = 0, digits = 0;
	for (size_t i = 2; i < version.size() && digits < 2 && version[i] >= '0' && version[i] <= '9'; i++, digits++) {
		minor = minor * 10 + (version[i] - '0');
	}
	if (digits == 0) {
		return 0;
	}
	if (digits == 1) {
		minor *= 10;
	}
	return (version[0] - '0') * 100 + minor;
}

// Which table key a firmware uses. PPSSPP's series rule, plus pspdecrypt's key 5 for 6.1x-6.2x
// archives of version 5.
int TableKeyIndexForVersion(const std::string &version, int psarVersion) {
	const int v = FirmwareVersionToInt(version);
	if (v >= 380 && v < 400) return 1;
	if (v >= 400 && v < 500) return 2;
	if (v >= 500 && v < 600) return 3;
	if (v >= 610 && v < 630 && psarVersion == 5) return 5;
	if (v >= 600 && v < 700) return 4;
	return 0;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
	if (s.size() < prefix.size()) {
		return false;
	}
	for (size_t i = 0; i < prefix.size(); i++) {
		if (tolower((u8)s[i]) != tolower((u8)prefix[i])) {
			return false;
		}
	}
	return true;
}

// Turns a name out of the archive into a path relative to the output directory:
// "flash0:/font/ltn0.pgf" becomes "flash0/font/ltn0.pgf". Returns false for anything that could
// escape the output directory - these names come from a file we didn't write.
bool RelativePathFromEntryName(std::string_view name, std::string *out) {
	std::string path(name);
	if (path.empty()) {
		return false;
	}
	for (char &c : path) {
		if (c == ':' || c == '\\') {
			c = '/';
		} else if ((u8)c < 0x20 || strchr("<>\"|?*", c)) {
			// Not valid in a Windows file name.
			c = '_';
		}
	}
	// Collapse the "flash0:/" -> "flash0//" the above just produced, and any other doubles.
	std::string cleaned;
	cleaned.reserve(path.size());
	for (char c : path) {
		if (c == '/' && (cleaned.empty() || cleaned.back() == '/')) {
			continue;
		}
		cleaned.push_back(c);
	}
	while (!cleaned.empty() && cleaned.back() == '/') {
		cleaned.pop_back();
	}
	if (cleaned.empty() || cleaned.find("..") != std::string::npos) {
		return false;
	}
	*out = cleaned;
	return true;
}

// An entry name is either a real path or a short token that only the archive's own file lists can
// resolve. 1.x and 2.x archives name entries outright: "flash0:/...", but also "flash1:/..." and
// "ipl:/psp_nandipl.bin", so take the "<dev>:/" shape as the tell. 3.x's grouped short names
// ("com:00123") have a colon but no slash after it.
bool EntryNameIsRealPath(std::string_view name) {
	return name.find(":/") != std::string_view::npos;
}

// 6.x file lists write "flash0:/font/x.pgf", 3.x writes "flash0/font/x.pgf" (and "ipl/x.bin") for
// the same thing. Settle on the first.
std::string CanonicalDevicePath(std::string_view name) {
	if (EntryNameIsRealPath(name)) {
		return std::string(name);
	}
	if (name.size() > 6 && StartsWithNoCase(name, "flash") && name[5] >= '0' && name[5] <= '9' && name[6] == '/') {
		return std::string(name.substr(0, 6)) + ":" + std::string(name.substr(6));
	}
	if (name.size() > 4 && StartsWithNoCase(name, "ipl/")) {
		return "ipl:" + std::string(name.substr(3));
	}
	return std::string(name);
}

// Entries are named one of two ways, depending on the firmware's age.
//
// 5.x/6.x archives use a flat five-digit token: the low numbers are the per-model file lists, and
// everything above that is a file which could be named by any of them.
//
// 3.x/4.x archives group by model instead - "com:00123", "01g:00005" - where the group is either
// "com" for files every model gets or "NNg" for one model's, and "<group>:00000" is that group's
// file list. Files there only ever need their own group's list.
//
// Returns the group's generation (0 for "com" and for the flat form) and the numeric index.
bool SplitEntryName(std::string_view name, int *generation, int *index, bool *grouped) {
	std::string_view digits = name;
	*generation = 0;
	*grouped = false;

	const size_t colon = name.find(':');
	if (colon != std::string_view::npos) {
		const std::string_view group = name.substr(0, colon);
		digits = name.substr(colon + 1);
		*grouped = true;
		if (group != "com") {
			if (group.size() != 3 || group[2] != 'g' || group[0] < '0' || group[0] > '9' || group[1] < '0' || group[1] > '9') {
				return false;
			}
			*generation = (group[0] - '0') * 10 + (group[1] - '0');
		}
	}

	if (digits.size() != 5) {
		return false;
	}
	int value = 0;
	for (char c : digits) {
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + (c - '0');
	}
	*index = value;
	return true;
}

const int kMaxModel = 12;

// True if this entry is a file list rather than a file, and if so which model's.
bool IsNameTableEntry(std::string_view name, int *generation) {
	int index = 0;
	bool grouped = false;
	int group = 0;
	if (!SplitEntryName(name, &group, &index, &grouped)) {
		return false;
	}
	if (grouped) {
		*generation = group;
		return index == 0;
	}
	*generation = index;
	return index >= 1 && index <= kMaxModel;
}

bool LooksLikeText(const u8 *data, size_t size) {
	if (size == 0) {
		return false;
	}
	size_t printable = 0;
	for (size_t i = 0; i < size && data[i] != 0; i++) {
		if ((data[i] >= 0x20 && data[i] < 0x7F) || data[i] == '\n' || data[i] == '\r' || data[i] == '\t') {
			printable++;
		} else {
			return false;
		}
	}
	return printable > 0;
}

// Decrypts a file list and returns its text, or false on failure. DES-CBC first; underneath is a
// PRX blob, except in version 4 archives where the DES layer is all there is.
bool DecryptNameTable(const Bytes &encrypted, int keyIndex, std::string *text) {
	if (keyIndex < 0 || keyIndex >= (int)(sizeof(kTableKeys) / sizeof(kTableKeys[0])) || encrypted.size() < 8) {
		return false;
	}
	const PSARTableKey &tableKey = kTableKeys[keyIndex];
	u8 key[8];
	const u64 combined = ((u64)tableKey.keyHigh << 32) | tableKey.keyLow;
	for (int i = 0; i < 8; i++) {
		key[i] = (u8)(combined >> (56 - i * 8));
	}
	Bytes table = encrypted;
	DES(key).CbcDecrypt(table.data(), table.size(), tableKey.iv);

	// With the right key, the word at 0xD0 is a recognisable PRX tag.
	if (table.size() >= PSP_HEADER_SIZE && PrxTagKnown(rd32(table.data() + 0xD0))) {
		Bytes plain;
		if (DecryptPRX(table.data(), table.size(), table.size(), &plain, nullptr, nullptr) > 0) {
			*text = std::string((const char *)plain.data(), plain.size());
			const size_t nul = text->find('\0');
			if (nul != std::string::npos) {
				text->resize(nul);
			}
			return !text->empty();
		}
	}
	if (LooksLikeText(table.data(), table.size())) {
		*text = std::string((const char *)table.data(), strnlen((const char *)table.data(), table.size()));
		return true;
	}
	return false;
}

// Table text is one "shortname<sep>realpath" per line. 6.x separates with a comma and writes the
// path as "flash0:/font/x.pgf"; 3.x separates with a bar and writes "flash0/font/x.pgf".
void ParseNameTable(const std::string &text, std::map<std::string, std::string> *names) {
	size_t start = 0;
	const size_t length = text.size();
	while (start < length) {
		size_t end = start;
		while (end < length && text[end] != '\r' && text[end] != '\n') {
			end++;
		}
		const std::string_view line(text.data() + start, end - start);
		const size_t separator = line.find_first_of(",|");
		if (separator != std::string_view::npos && separator > 0) {
			names->emplace(std::string(line.substr(0, separator)), std::string(line.substr(separator + 1)));
		}
		while (end < length && (text[end] == '\r' || text[end] == '\n')) {
			end++;
		}
		start = end;
	}
}

bool ParseModel(const std::string &name, int *generation) {
	if (name == "any" || name == "ANY") {
		*generation = 0;
		return true;
	}
	std::string digits = name;
	if (digits.size() > 1 && (digits.back() == 'g' || digits.back() == 'G')) {
		digits.pop_back();
	}
	if (digits.empty() || digits.size() > 2) {
		return false;
	}
	int value = 0;
	for (char c : digits) {
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + (c - '0');
	}
	if (value < 1 || value > kMaxModel) {
		return false;
	}
	*generation = value;
	return true;
}

std::string ModelName(int generation) {
	return generation == 0 ? "com" : StringPrintf("%02dg", generation);
}

class PSARReader {
public:
	PSARReader(const u8 *psar, size_t size) : psar_(psar), size_(size) {}

	bool Init(std::string *error);

	// 1: got an entry. 0: no more entries. -1: failed.
	int NextEntry(std::string *error);

	const std::string &firmwareVersion() const { return firmwareVersion_; }
	const std::string &versionRecord() const { return versionRecord_; }
	int version() const { return version_; }
	bool predecrypted() const { return decrypted_; }
	const std::string &entryName() const { return entryName_; }
	bool entryIsDirectory() const { return entryIsDirectory_; }
	bool entrySigncheck() const { return entrySigncheck_; }
	u32 entryExpandedSize() const { return contentExpandedSize_; }
	// Decrypting an entry's contents is the expensive part, so it waits until asked for.
	const std::string &entryCompression() { EnsureContents(); return entryCompression_; }
	// Empty for a directory, or for an entry we couldn't decode (entryError() says why).
	const Bytes &entryData() { EnsureContents(); return entryData_; }
	const std::string &entryError() { EnsureContents(); return entryError_; }

private:
	// Decrypts one record into 'out'. Returns the decrypted size, or <= 0 on failure.
	int DecodeBlock(u32 offset, u32 cbIn, Bytes &out);
	void EnsureContents();

	const u8 *psar_;
	size_t size_;

	bool decrypted_ = false;
	bool oldschool_ = false;
	int version_ = 0;
	u32 overhead_ = PSAR_HEADER_SIZE;
	u32 pos_ = 0;
	// Where the records stop. The header says how long the archive really is; there are a few
	// bytes of padding after that which don't decode as a record.
	size_t limit_ = 0;
	std::string firmwareVersion_;
	std::string versionRecord_;

	std::string entryName_;
	bool entryIsDirectory_ = false;
	bool entrySigncheck_ = false;
	std::string entryCompression_;
	std::string entryError_;
	Bytes entryData_;

	bool contentsDecoded_ = true;
	u32 contentPos_ = 0;
	u32 contentChunkSize_ = 0;
	u32 contentExpandedSize_ = 0;

	Bytes block_;
	Bytes block2_;
};

int PSARReader::DecodeBlock(u32 offset, u32 cbIn, Bytes &out) {
	if (cbIn == 0 || cbIn > PSAR_MAX_ENTRY_BYTES || offset >= size_) {
		return -1;
	}
	const u8 *in = psar_ + offset;
	const size_t avail = size_ - offset;
	if (cbIn > avail) {
		return -1;
	}
	if (decrypted_) {
		out.assign(in, in + cbIn);
		return (int)cbIn;
	}

	// The hash checks read a little past the block and use what they find there, so hand them the
	// real bytes whenever the archive has them. The last record ends flush with the archive and
	// gets zeroes instead (DecryptPRX pads).
	const size_t readable = std::min<size_t>(avail, (size_t)cbIn + 0x40);
	Bytes blob(in, in + readable);

	if (!oldschool_) {
		// "Demangle": the 0x130 bytes at +0x20 are AES-128-CBC encrypted on top of everything
		// else, and hide the PRX tag at +0xD0 that says how to decrypt the rest.
		if (cbIn < PSP_HEADER_SIZE) {
			return -1;
		}
		u8 *mangled = blob.data() + 0x20;
		if (version_ == 5) {
			for (int i = 0; i < 0x130; i++) {
				mangled[i] ^= kDemangleK1[i & 0xF];
			}
		}
		kirk7(mangled, mangled, 0x130, PSAR_DEMANGLE_KEYSEED);
		if (version_ == 5) {
			for (int i = 0; i < 0x130; i++) {
				mangled[i] ^= kDemangleK2[i & 0xF];
			}
		}
	}

	return DecryptPRX(blob.data(), cbIn, blob.size(), &out, nullptr, nullptr);
}

bool PSARReader::Init(std::string *error) {
	if (size_ < 0x40 || rd32(psar_) != PSAR_MAGIC) {
		*error = "not a PSAR archive";
		return false;
	}

	version_ = psar_[4];
	oldschool_ = version_ == 1;
	decrypted_ = rd32(psar_ + 0x20) == PSAR_DECRYPTED_MARKER;
	overhead_ = decrypted_ ? 0 : PSAR_HEADER_SIZE;

	// The header records how long the run of records is. Trusting it both stops us decoding
	// trailing padding as an entry and catches a truncated archive, which would otherwise just
	// look like a smaller complete one.
	limit_ = size_;
	const u32 declaredSize = rd32(psar_ + 8);
	if (declaredSize > size_) {
		*error = StringPrintf("truncated archive: it declares %u bytes but only %u are here", declaredSize, (u32)size_);
		return false;
	}
	if (declaredSize >= 0x40) {
		limit_ = declaredSize;
	}

	int decoded = DecodeBlock(0x10, overhead_ + PSAR_ENTRY_SIZE, block_);
	if (decoded != (int)PSAR_ENTRY_SIZE) {
		*error = StringPrintf("couldn't decrypt the PSAR's first block (got %d)", decoded);
		return false;
	}

	// The first block is a text record ending in the firmware version, e.g. "...,6.61".
	const char *text = (const char *)block_.data() + 0x10;
	const size_t textLen = strnlen(text, PSAR_ENTRY_SIZE - 0x10);
	versionRecord_ = FixedString(block_.data() + 0x10, textLen);
	const std::string_view firstLine(text, textLen);
	const size_t comma = firstLine.rfind(',');
	firmwareVersion_ = comma == std::string_view::npos ? std::string(firstLine) : std::string(firstLine.substr(comma + 1));

	pos_ = 0x10 + overhead_ + PSAR_ENTRY_SIZE;

	if (decrypted_) {
		decoded = DecodeBlock(pos_, rd32(block_.data() + 0x90), block2_);
		if (decoded <= 0) {
			*error = "couldn't read the PSAR's second block";
			return false;
		}
		pos_ += overhead_ + decoded;
		return true;
	}

	if (!oldschool_) {
		// The second block's size isn't recorded anywhere, so try the ones real updaters use.
		// 100 covers most, 2.7x is bigger, and some store it in the first block.
		const u32 candidates[] = { 100, 144, rd16(block_.data() + 0x90) };
		decoded = -1;
		for (u32 candidate : candidates) {
			if (candidate == 0) {
				continue;
			}
			decoded = DecodeBlock(pos_, overhead_ + candidate, block2_);
			if (decoded > 0) {
				break;
			}
		}
		if (decoded <= 0) {
			*error = "couldn't read the PSAR's second block";
			return false;
		}
		pos_ += overhead_ + ((decoded + 15) & ~15);
	}
	return true;
}

int PSARReader::NextEntry(std::string *error) {
	entryName_.clear();
	entryData_.clear();
	entryError_.clear();
	entryCompression_.clear();
	entryIsDirectory_ = false;
	entrySigncheck_ = false;
	contentExpandedSize_ = 0;
	contentsDecoded_ = true;

	// Stop when what's left can't hold another whole record, or the padding at the end would be
	// decoded as a truncated entry and reported as a failure right at the finish line.
	if ((size_t)pos_ + overhead_ + PSAR_ENTRY_SIZE > limit_) {
		return 0;
	}

	const int decoded = DecodeBlock(pos_, overhead_ + PSAR_ENTRY_SIZE, block_);
	if (decoded != (int)PSAR_ENTRY_SIZE) {
		*error = StringPrintf("couldn't decrypt the entry at 0x%x (got %d)", pos_, decoded);
		return -1;
	}

	// A well-formed entry has a zero here. If it doesn't, we've lost our place in the archive.
	if (rd32(block_.data() + 0x100) != 0) {
		*error = StringPrintf("malformed entry at 0x%x", pos_);
		return -1;
	}

	entryName_ = FixedString(block_.data() + 4, PSAR_ENTRY_SIZE - 4);
	entrySigncheck_ = block_[0x10F] == 2;

	pos_ += overhead_ + PSAR_ENTRY_SIZE;

	const u32 chunkSize = rd32(block_.data() + 0x104);
	const u32 expandedSize = rd32(block_.data() + 0x108);

	if (expandedSize == 0) {
		entryIsDirectory_ = true;
	} else if (expandedSize > PSAR_MAX_ENTRY_BYTES) {
		*error = StringPrintf("entry '%s' claims an unreasonable size (%u)", entryName_.c_str(), expandedSize);
		return -1;
	} else {
		contentPos_ = pos_;
		contentChunkSize_ = chunkSize;
		contentExpandedSize_ = expandedSize;
		contentsDecoded_ = false;
	}

	if ((u64)pos_ + chunkSize > limit_) {
		*error = StringPrintf("entry '%s' runs past the end of the archive", entryName_.c_str());
		return -1;
	}
	pos_ += chunkSize;
	return 1;
}

void PSARReader::EnsureContents() {
	if (contentsDecoded_) {
		return;
	}
	contentsDecoded_ = true;

	const int decoded = DecodeBlock(contentPos_, contentChunkSize_, block2_);
	if (decoded <= 0) {
		entryError_ = "contents don't decrypt";
		return;
	}
	size_t consumed = 0;
	std::string err;
	if (!DecompressAny(block2_.data(), (size_t)decoded, contentExpandedSize_, &entryData_, &entryCompression_, &consumed, &err)) {
		entryError_ = (entryCompression_.empty() ? std::string("decompress") : entryCompression_) + ": " + err;
		entryData_.clear();
		return;
	}
	if (entryData_.size() != contentExpandedSize_) {
		entryError_ = StringPrintf("expanded to %zu bytes, the entry says %u", entryData_.size(), contentExpandedSize_);
		entryData_.clear();
	}
}

// Everything one unpack run needs to write files and keep the books.
struct Unpacker {
	std::string outDir;
	const PsarOptions *options;
	PsarSummary *summary;
	Json entries = Json::Array();
	// Derived files wait here so they follow the entry they came from in the manifest.
	std::vector<Json> derivedQueue;
	std::map<std::string, std::string> written;  // relative path -> sha1 of what's there

	bool Write(const std::string &rel, const Bytes &data, std::string *error) {
		return WriteFileBytes(JoinPath(outDir, rel), data, error);
	}

	void Log(const std::string &line) {
		if (options->verbose) {
			fprintf(stderr, "%s\n", line.c_str());
		}
	}

	// Writes `data` at `rel`, unless something different already went there this run, in which
	// case it goes under conflicts/<group>/. Returns the path used, or empty on failure.
	std::string Place(const std::string &rel, const Bytes &data, const std::string &group, Json &e) {
		std::string target = rel;
		const std::string sha1 = Sha1Hex(data.data(), data.size());
		auto it = written.find(rel);
		if (it != written.end() && it->second != sha1) {
			target = "conflicts/" + group + "/" + rel;
			e.Set("conflicts_with", rel);
			summary->conflicts++;
		}
		std::string error;
		if (!Write(target, data, &error)) {
			e.Set("error", error);
			return std::string();
		}
		written[target] = sha1;
		e.Set("path", target);
		e.Set("size", (unsigned long long)data.size());
		e.Set("sha1", sha1);
		return target;
	}

	void AddDerived(const std::string &rel, const Bytes &data, const std::string &source, const std::string &what) {
		Json d = Json::Object();
		d.Set("entry", source);
		d.Set("kind", "derived");
		d.Set("what", what);
		std::string error;
		if (Write(rel, data, &error)) {
			d.Set("path", rel);
			d.Set("size", (unsigned long long)data.size());
			d.Set("sha1", Sha1Hex(data.data(), data.size()));
			summary->derived++;
		} else {
			d.Set("error", error);
		}
		derivedQueue.push_back(d);
	}

	// loadexec carries reboot.bin as an embedded ~PSP.
	void ExtractReboot(const std::string &rel, const Bytes &loadexec) {
		const std::vector<size_t> found = FindEmbeddedModules(loadexec.data(), loadexec.size(), 4);
		if (found.empty()) {
			return;
		}
		const size_t off = found[0];
		DecodeResult r;
		// The decoder stops at psp_size, but the hash checks may read a little past it.
		DecodeExecutable(loadexec.data() + off, loadexec.size() - off, options->decode, &r);
		std::string base = rel.substr(rel.rfind('/') + 1);
		const size_t at = base.find("loadexec");
		base = at == std::string::npos ? "reboot.bin" : "reboot" + base.substr(at + 8);
		if (base.size() > 4 && base.compare(base.size() - 4, 4, ".prx") == 0) {
			base.replace(base.size() - 4, 4, ".bin");
		}
		if (r.status == DecodeStatus::Ok) {
			AddDerived("derived/" + base, r.output, rel, "reboot.bin from loadexec (" + r.compression + ")");
		} else {
			Json d = Json::Object();
			d.Set("entry", rel);
			d.Set("kind", "derived");
			d.Set("what", "reboot.bin from loadexec");
			d.Set("error", r.error);
			derivedQueue.push_back(d);
		}
	}

	void ProcessModule(const std::string &rel, const std::string &group, const Bytes &content, bool meImage, Json &e) {
		summary->modules++;
		DecodeResult r;
		if (meImage && SniffKind(content.data(), content.size()) == FileKind::Unknown) {
			// ME images can lack the ~PSP magic but are still tag-encrypted blobs.
			Bytes payload;
			PrxDecryptInfo info;
			if (DecryptPRX(content.data(), content.size(), content.size(), &payload, nullptr, &info) >= 0) {
				r.encrypted = true;
				r.prxType = info.type;
				r.tag = info.tag;
				r.status = DecodeStatus::Ok;
				size_t consumed = 0;
				Bytes out;
				std::string err;
				if (IsGzipMagic(payload.data(), payload.size()) && DecompressAny(payload.data(), payload.size(), 0, &out, &r.compression, &consumed, &err)) {
					payload = std::move(out);
				}
				r.output = std::move(payload);
			} else {
				r.status = info.error == PrxError::UnknownTag ? DecodeStatus::NeedsKey : DecodeStatus::Failed;
				r.error = std::string("cannot decrypt ME image: ") + PrxErrorString(info.error);
			}
		} else {
			DecodeExecutable(content.data(), content.size(), options->decode, &r);
		}

		e.Set("kind", "module");
		e.Set("raw_size", (unsigned long long)content.size());
		e.Set("raw_sha1", Sha1Hex(content.data(), content.size()));
		if (r.hadPspHeader) {
			e.Set("module_name", r.psp.modname);
			e.Set("tag", Hex32(r.psp.tag));
		}
		if (r.status == DecodeStatus::Ok) {
			e.Set("decrypted", true);
			e.Set("encrypted", r.encrypted);
			if (r.cfwPacked) {
				e.Set("cfw_packed", true);
			}
			if (r.prxType >= 0) {
				e.Set("prx_type", r.prxType);
			}
			e.Set("compression", r.compression);
			if (!r.warnings.empty()) {
				Json w = Json::Array();
				for (const std::string &s : r.warnings) {
					w.Push(s);
				}
				e.Set("warnings", w);
			}
			const std::string placed = Place(rel, r.output, group, e);
			if (placed.empty()) {
				summary->failed++;
				return;
			}
			summary->modulesDecrypted++;
			if (options->keepEncrypted) {
				std::string error;
				if (Write("encrypted/" + placed, content, &error)) {
					e.Set("raw_path", "encrypted/" + placed);
				}
			}
			if (!r.trailing.empty()) {
				AddDerived("derived/" + placed + ".2", r.trailing, placed, "second compressed stream in the module");
			}
			const std::string base = placed.substr(placed.rfind('/') + 1);
			if (base.rfind("loadexec", 0) == 0) {
				ExtractReboot(placed, r.output);
			}
			Log("  " + placed + ": " + (r.encrypted ? "decrypted" : "unpacked") + ", " + r.compression);
		} else {
			// Keep the tree complete: the encrypted original goes where the module belongs, and the
			// manifest says it isn't usable.
			e.Set("decrypted", false);
			e.Set("encrypted", true);
			e.Set("status", r.status == DecodeStatus::NeedsKey ? "needs-key" : "failed");
			e.Set("error", r.error);
			Place(rel, content, group, e);
			if (r.status == DecodeStatus::NeedsKey) {
				summary->needsKey++;
			} else {
				summary->failed++;
			}
			Log("  " + rel + ": " + r.error);
		}
	}

	void ProcessIpl(const std::string &rel, const std::string &group, const Bytes &content, Json &e) {
		e.Set("kind", "ipl");
		Place(rel, content, group, e);
		IplStage1 ipl;
		std::string error;
		if (DecryptIplStage1(content.data(), content.size(), &ipl, &error)) {
			std::string what = StringPrintf("IPL stage 1, %d blocks, load 0x%08x, entry 0x%08x", ipl.blocks, ipl.loadAddress, ipl.entry);
			if (ipl.descrambleKey >= 0) {
				what += StringPrintf(", descrambled with key %d", ipl.descrambleKey);
			}
			if (!ipl.note.empty()) {
				what += ", " + ipl.note;
			}
			AddDerived("derived/" + rel + ".stage1.bin", ipl.image, rel, what);
		} else {
			e.Set("ipl_error", error);
		}
	}

	void ProcessFile(const std::string &entryName, const std::string &deviceName, const std::string &rel,
			const std::string &group, PSARReader &reader) {
		Json e = Json::Object();
		e.Set("entry", entryName);
		if (!deviceName.empty()) {
			e.Set("device_path", deviceName);
		}
		e.Set("group", group);
		if (reader.entrySigncheck()) {
			e.Set("signcheck", true);
		}
		const Bytes &content = reader.entryData();
		e.Set("archive_compression", reader.entryCompression().empty() ? "none" : reader.entryCompression());
		if (content.empty()) {
			e.Set("kind", "file");
			e.Set("error", reader.entryError().empty() ? "no contents" : reader.entryError());
			e.Set("path", rel);
			summary->failed++;
			Log("  " + rel + ": " + reader.entryError());
			entries.Push(e);
			return;
		}
		summary->files++;
		const FileKind kind = SniffKind(content.data(), content.size());
		const bool meImage = deviceName.find("/kd/resource/me") != std::string::npos;
		const bool isIpl = rel.rfind("ipl/", 0) == 0;
		if (kind == FileKind::PspModule || kind == FileKind::SceWrapped ||
				(meImage && content.size() >= PSP_HEADER_SIZE && PrxTagKnown(rd32(content.data() + 0xD0)))) {
			ProcessModule(rel, group, content, meImage, e);
		} else if (isIpl) {
			ProcessIpl(rel, group, content, e);
		} else {
			e.Set("kind", "file");
			if (Place(rel, content, group, e).empty()) {
				summary->failed++;
			}
		}
		entries.Push(e);
		for (Json &d : derivedQueue) {
			entries.Push(std::move(d));
		}
		derivedQueue.clear();
	}
};

}  // namespace

bool PsarParseModel(const std::string &name, int *generation) {
	return ParseModel(name, generation);
}

bool LocatePsar(const u8 *data, size_t size, const u8 **psar, size_t *psarSize, std::string *error) {
	if (HasMagic(data, size, "\0PBP", 4)) {
		PbpFile pbp;
		if (!pbp.Parse(data, size, error)) {
			return false;
		}
		if (pbp.size[PbpFile::DATA_PSAR] < 0x40 || !HasMagic(data + pbp.offset[PbpFile::DATA_PSAR], pbp.size[PbpFile::DATA_PSAR], "PSAR", 4)) {
			*error = "this PBP has no PSAR in DATA.PSAR (not a firmware updater)";
			return false;
		}
		*psar = data + pbp.offset[PbpFile::DATA_PSAR];
		*psarSize = pbp.size[PbpFile::DATA_PSAR];
		return true;
	}
	if (HasMagic(data, size, "PSAR", 4)) {
		*psar = data;
		*psarSize = size;
		return true;
	}
	*error = "not a firmware updater: expected an EBOOT.PBP with a PSAR, or a PSAR";
	return false;
}

Json DescribePsar(const u8 *psar, size_t size) {
	Json j = Json::Object();
	if (size < 0x40) {
		j.Set("error", "too small");
		return j;
	}
	j.Set("version", psar[4]);
	j.Set("predecrypted", rd32(psar + 0x20) == PSAR_DECRYPTED_MARKER);
	j.Set("declared_size", rd32(psar + 8));
	PSARReader reader(psar, size);
	std::string error;
	if (reader.Init(&error)) {
		j.Set("firmware_version", reader.firmwareVersion());
		j.Set("version_record", reader.versionRecord());
		j.Set("table_key", TableKeyIndexForVersion(reader.firmwareVersion(), psar[4]));
	} else {
		j.Set("error", error);
	}
	return j;
}

bool UnpackPsar(const u8 *psar, size_t size, const std::string &outDir, const PsarOptions &options,
		Json *manifest, PsarSummary *summary, std::string *error) {
	*summary = PsarSummary();
	int model = 0;
	if (!ParseModel(options.model, &model)) {
		*error = "bad --model '" + options.model + "' (any, or 01g..12g)";
		return false;
	}

	PSARReader reader(psar, size);
	if (!reader.Init(error)) {
		return false;
	}
	summary->firmwareVersion = reader.firmwareVersion();
	summary->psarVersion = reader.version();
	const int tableKeyIndex = TableKeyIndexForVersion(summary->firmwareVersion, reader.version());

	Unpacker up;
	up.outDir = outDir;
	up.options = &options;
	up.summary = summary;
	up.Log(StringPrintf("PSAR version %d, firmware %s, table key %d", reader.version(), summary->firmwareVersion.c_str(), tableKeyIndex));

	// One file list per model, filled in as we go - they come before the files they name.
	std::map<int, std::map<std::string, std::string>> namesByModel;
	bool walkFailed = false;
	std::string walkError;

	while (true) {
		const int result = reader.NextEntry(&walkError);
		if (result < 0) {
			// Losing our place means the rest of the archive is unreadable; keep what we have.
			walkFailed = true;
			summary->failed++;
			break;
		}
		if (result == 0) {
			break;
		}
		summary->entries++;
		const std::string &name = reader.entryName();
		up.Log(name);

		if (reader.entryIsDirectory()) {
			summary->directories++;
			Json e = Json::Object();
			e.Set("entry", name);
			e.Set("kind", "directory");
			std::string rel;
			if (EntryNameIsRealPath(name) && RelativePathFromEntryName(CanonicalDevicePath(name), &rel)) {
				std::error_code ec;
				std::filesystem::create_directories(U8Path(JoinPath(outDir, rel)), ec);
				e.Set("path", rel);
			}
			up.entries.Push(e);
			continue;
		}

		// File lists have to be read before anything they name, which the archive's own order
		// takes care of. Which numbers are lists varies, but a list always decrypts (the PRX
		// layer validates a hash), so failing here just means this is an ordinary file.
		int tableGeneration = 0;
		if (IsNameTableEntry(name, &tableGeneration)) {
			std::string text;
			if (!reader.entryData().empty() && DecryptNameTable(reader.entryData(), tableKeyIndex, &text)) {
				summary->fileLists++;
				std::map<std::string, std::string> &names = namesByModel[tableGeneration];
				const size_t before = names.size();
				ParseNameTable(text, &names);
				std::string listName = name;
				std::replace(listName.begin(), listName.end(), ':', '_');
				Json e = Json::Object();
				e.Set("entry", name);
				e.Set("kind", "file_list");
				e.Set("model", ModelName(tableGeneration));
				e.Set("names", (unsigned long long)(names.size() - before));
				Bytes textBytes(text.begin(), text.end());
				up.Place("psar/lists/" + listName + ".txt", textBytes, ModelName(tableGeneration), e);
				up.entries.Push(e);
				continue;
			}
		}

		int entryGeneration = 0;
		int entryIndex = 0;
		bool entryGrouped = false;
		SplitEntryName(name, &entryGeneration, &entryIndex, &entryGrouped);

		std::string realName;
		std::string group = "-";
		bool wrongModel = false;
		if (EntryNameIsRealPath(name)) {
			realName = name;
		} else if (entryGrouped) {
			// The name says which list to look in, and the lists key on just the number - entry
			// "com:00004" is "00004" in the com list. "com" entries are common to every model.
			group = ModelName(entryGeneration);
			if (model != 0 && entryGeneration != 0 && entryGeneration != model) {
				wrongModel = true;
			} else {
				const auto list = namesByModel.find(entryGeneration);
				if (list != namesByModel.end()) {
					const auto found = list->second.find(name.substr(name.find(':') + 1));
					if (found != list->second.end()) {
						realName = found->second;
					}
				}
			}
		} else if (model != 0) {
			// Only this model's list counts. A file it doesn't name belongs to some other model.
			const auto list = namesByModel.find(model);
			group = ModelName(model);
			if (list != namesByModel.end()) {
				const auto found = list->second.find(name);
				if (found != list->second.end()) {
					realName = found->second;
				} else {
					wrongModel = true;
				}
			}
		} else {
			// A short name that several lists claim is the same file; take the first name for it.
			for (const auto &[generation, names] : namesByModel) {
				const auto found = names.find(name);
				if (found != names.end()) {
					realName = found->second;
					group = ModelName(generation);
					break;
				}
			}
		}

		if (wrongModel) {
			summary->otherModel++;
			continue;
		}

		std::string rel;
		realName = CanonicalDevicePath(realName);
		if (realName.empty()) {
			// No list claimed this one. Still worth writing out under its short name.
			summary->unnamed++;
			std::string shortName = name;
			std::replace(shortName.begin(), shortName.end(), ':', '_');
			RelativePathFromEntryName("unnamed/" + shortName, &rel);
		} else if (!RelativePathFromEntryName(realName, &rel)) {
			Json e = Json::Object();
			e.Set("entry", name);
			e.Set("device_path", realName);
			e.Set("kind", "file");
			e.Set("error", "refusing to write a path outside the output directory");
			up.entries.Push(e);
			summary->failed++;
			continue;
		}
		up.ProcessFile(name, realName, rel, group, reader);
	}

	Json m = Json::Object();
	m.Set("tool", "pspc-decrypt");
	m.Set("psar_version", summary->psarVersion);
	m.Set("predecrypted", reader.predecrypted());
	m.Set("firmware_version", summary->firmwareVersion);
	m.Set("version_record", reader.versionRecord());
	m.Set("table_key", tableKeyIndex);
	m.Set("model", model == 0 ? std::string("any") : ModelName(model));
	m.Set("complete", !walkFailed);
	if (walkFailed) {
		m.Set("error", walkError);
	}
	Json s = Json::Object();
	s.Set("entries", summary->entries);
	s.Set("files", summary->files);
	s.Set("directories", summary->directories);
	s.Set("file_lists", summary->fileLists);
	s.Set("modules", summary->modules);
	s.Set("modules_decrypted", summary->modulesDecrypted);
	s.Set("needs_key", summary->needsKey);
	s.Set("failed", summary->failed);
	s.Set("unnamed", summary->unnamed);
	s.Set("other_model", summary->otherModel);
	s.Set("conflicts", summary->conflicts);
	s.Set("derived", summary->derived);
	m.Set("summary", s);
	m.Set("entries", up.entries);
	*manifest = m;

	const std::string manifestText = m.Dump(1) + "\n";
	std::string writeError;
	if (!WriteFileBytes(JoinPath(outDir, "manifest.json"), (const u8 *)manifestText.data(), manifestText.size(), &writeError)) {
		*error = writeError;
		return false;
	}
	if (walkFailed) {
		*error = walkError;
	}
	return true;
}
