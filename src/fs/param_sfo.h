// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/ELF/ParamSFO.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// PARAM.SFO ("\0PSF" key/value tables): game metadata and save data descriptions.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/common.h"

namespace vfs {

class ParamSfo {
public:
	enum Format : u16 {
		FMT_BINARY = 0x0004,  // raw bytes (e.g. SAVEDATA_PARAMS)
		FMT_UTF8 = 0x0204,    // NUL-terminated UTF-8
		FMT_INT = 0x0404,     // u32
	};
	struct Entry {
		std::string key;
		u16 format = FMT_UTF8;
		u32 maxLen = 0;        // bytes reserved in the data table
		std::vector<u8> data;  // param_len bytes (a string's NUL included when the file had it)
	};

	// Replaces the contents. False if the data isn't a PARAM.SFO.
	bool Parse(const u8 *data, size_t size);
	bool Parse(const std::vector<u8> &data) { return Parse(data.data(), data.size()); }
	void Clear() { entries_.clear(); }

	bool Has(std::string_view key) const { return Find(key) != nullptr; }
	// The string up to its first NUL; `def` if missing or not a string.
	std::string GetString(std::string_view key, std::string_view def = {}) const;
	u32 GetInt(std::string_view key, u32 def = 0) const;
	bool GetBinary(std::string_view key, std::vector<u8> &out) const;

	// Adds or replaces. `maxLen` 0 keeps an existing entry's reservation when the value fits, or
	// reserves the value's size rounded up to 4. A nonzero `maxLen` is used as is, and a longer
	// string is cut (at a UTF-8 boundary) to maxLen - 1 bytes. New keys are inserted in sorted order.
	void SetString(std::string_view key, std::string_view value, u32 maxLen = 0);
	void SetInt(std::string_view key, u32 value);
	void SetBinary(std::string_view key, const u8 *data, size_t size, u32 maxLen = 0);
	bool Remove(std::string_view key);

	const std::vector<Entry> &Entries() const { return entries_; }
	// Re-encodes in the firmware's layout (keys padded to 4, each value in maxLen bytes), entry
	// order preserved. Parsing a well-formed file and serializing gives the same bytes back.
	std::vector<u8> Serialize() const;

private:
	const Entry *Find(std::string_view key) const;
	Entry &Upsert(std::string_view key);

	std::vector<Entry> entries_;
};

}  // namespace vfs
