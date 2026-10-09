// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/ELF/ParamSFO.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include "fs/param_sfo.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"

namespace vfs {

namespace {

// Header: +00 "\0PSF", +04 version (0x0101), +08 key table offset, +0C data table offset,
// +10 entry count. Then 16-byte index entries: u16 key offset, u16 format, u32 length,
// u32 reserved length, u32 data offset.
constexpr u32 kMagic = 0x46535000;
constexpr u32 kHeaderSize = 20;
constexpr u32 kIndexSize = 16;

u16 LE16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
u32 LE32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
void Put16(u8 *p, u16 v) {
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}
void Put32(u8 *p, u32 v) {
	for (int i = 0; i < 4; i++) {
		p[i] = (u8)(v >> (8 * i));
	}
}

}  // namespace

bool ParamSfo::Parse(const u8 *data, size_t size) {
	entries_.clear();
	if (!data || size < kHeaderSize || LE32(data) != kMagic) {
		return false;
	}
	const u32 version = LE32(data + 4);
	if (version != 0x0101) {
		LOGW("fs", "PARAM.SFO: unexpected version %08x", version);
	}
	const u32 keyTable = LE32(data + 8);
	const u32 dataTable = LE32(data + 12);
	const u32 count = LE32(data + 16);
	if ((u64)kHeaderSize + (u64)count * kIndexSize > size || keyTable > size || dataTable > size) {
		return false;
	}
	for (u32 i = 0; i < count; i++) {
		const u8 *idx = data + kHeaderSize + i * kIndexSize;
		const u64 keyOff = (u64)keyTable + LE16(idx);
		const u64 dataOff = (u64)dataTable + LE32(idx + 12);
		if (keyOff >= size || dataOff > size) {
			return false;
		}
		const char *k = (const char *)data + keyOff;
		const size_t keyLen = strnlen(k, size - (size_t)keyOff);
		if (keyLen == 0 || keyOff + keyLen >= size) {
			continue;  // truncated or nameless entry
		}
		Entry e;
		e.key.assign(k, keyLen);
		e.format = LE16(idx + 2);
		u32 len = LE32(idx + 4);
		e.maxLen = LE32(idx + 8);
		len = (u32)std::min<u64>(len, size - dataOff);
		e.data.assign(data + dataOff, data + dataOff + len);
		e.maxLen = std::max(e.maxLen, len);
		// Keep the file's order (normally sorted); a repeated key replaces the earlier one.
		auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &x) { return x.key == e.key; });
		if (it != entries_.end()) {
			*it = std::move(e);
		} else {
			entries_.push_back(std::move(e));
		}
	}
	return true;
}

const ParamSfo::Entry *ParamSfo::Find(std::string_view key) const {
	for (const Entry &e : entries_) {
		if (e.key == key) {
			return &e;
		}
	}
	return nullptr;
}

ParamSfo::Entry &ParamSfo::Upsert(std::string_view key) {
	for (Entry &e : entries_) {
		if (e.key == key) {
			return e;
		}
	}
	auto pos = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &e) { return std::string_view(e.key) > key; });
	Entry e;
	e.key = std::string(key);
	return *entries_.insert(pos, std::move(e));
}

std::string ParamSfo::GetString(std::string_view key, std::string_view def) const {
	const Entry *e = Find(key);
	if (!e || e->format != FMT_UTF8) {
		return std::string(def);
	}
	const char *s = (const char *)e->data.data();
	return std::string(s, strnlen(s, e->data.size()));
}

u32 ParamSfo::GetInt(std::string_view key, u32 def) const {
	const Entry *e = Find(key);
	if (!e || e->format != FMT_INT || e->data.size() < 4) {
		return def;
	}
	return LE32(e->data.data());
}

bool ParamSfo::GetBinary(std::string_view key, std::vector<u8> &out) const {
	const Entry *e = Find(key);
	if (!e) {
		return false;
	}
	out = e->data;
	return true;
}

void ParamSfo::SetString(std::string_view key, std::string_view value, u32 maxLen) {
	Entry &e = Upsert(key);
	if (maxLen == 0) {
		maxLen = (e.format == FMT_UTF8 && e.maxLen >= value.size() + 1) ? e.maxLen : AlignUp((u32)value.size() + 1, 4);
	}
	size_t len = value.size();
	if (len + 1 > maxLen) {
		len = maxLen ? maxLen - 1 : 0;
		while (len > 0 && ((unsigned char)value[len] & 0xC0) == 0x80) {
			len--;
		}
	}
	e.format = FMT_UTF8;
	e.maxLen = maxLen;
	e.data.assign(value.begin(), value.begin() + len);
	if (maxLen) {
		e.data.push_back(0);
	}
}

void ParamSfo::SetInt(std::string_view key, u32 value) {
	Entry &e = Upsert(key);
	e.format = FMT_INT;
	e.maxLen = 4;
	e.data.resize(4);
	Put32(e.data.data(), value);
}

void ParamSfo::SetBinary(std::string_view key, const u8 *data, size_t size, u32 maxLen) {
	Entry &e = Upsert(key);
	if (maxLen == 0) {
		maxLen = (e.format == FMT_BINARY && e.maxLen >= size) ? e.maxLen : AlignUp((u32)size, 4);
	}
	e.format = FMT_BINARY;
	e.maxLen = maxLen;
	e.data.assign(data, data + std::min<size_t>(size, maxLen));
}

bool ParamSfo::Remove(std::string_view key) {
	for (auto it = entries_.begin(); it != entries_.end(); ++it) {
		if (it->key == key) {
			entries_.erase(it);
			return true;
		}
	}
	return false;
}

std::vector<u8> ParamSfo::Serialize() const {
	u32 keyBytes = 0, dataBytes = 0;
	for (const Entry &e : entries_) {
		keyBytes += (u32)e.key.size() + 1;
		dataBytes += std::max<u32>(e.maxLen, (u32)e.data.size());
	}
	keyBytes = AlignUp(keyBytes, 4);
	const u32 count = (u32)entries_.size();
	const u32 keyTable = kHeaderSize + count * kIndexSize;
	const u32 dataTable = keyTable + keyBytes;
	std::vector<u8> out(dataTable + dataBytes, 0);
	Put32(out.data(), kMagic);
	Put32(out.data() + 4, 0x0101);
	Put32(out.data() + 8, keyTable);
	Put32(out.data() + 12, dataTable);
	Put32(out.data() + 16, count);
	u32 keyOff = 0, dataOff = 0;
	for (u32 i = 0; i < count; i++) {
		const Entry &e = entries_[i];
		const u32 reserved = std::max<u32>(e.maxLen, (u32)e.data.size());
		u8 *idx = out.data() + kHeaderSize + i * kIndexSize;
		Put16(idx, (u16)keyOff);
		Put16(idx + 2, e.format);
		Put32(idx + 4, (u32)e.data.size());
		Put32(idx + 8, reserved);
		Put32(idx + 12, dataOff);
		memcpy(out.data() + keyTable + keyOff, e.key.data(), e.key.size());
		if (!e.data.empty()) {
			memcpy(out.data() + dataTable + dataOff, e.data.data(), e.data.size());
		}
		keyOff += (u32)e.key.size() + 1;
		dataOff += reserved;
	}
	return out;
}

}  // namespace vfs
