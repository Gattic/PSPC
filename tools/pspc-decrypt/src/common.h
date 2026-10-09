// SPDX-License-Identifier: GPL-3.0-or-later
// pspc-decrypt: shared types and small helpers (no PPSSPP framework dependencies).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

using Bytes = std::vector<u8>;

// Little-endian accessors that don't care about alignment or host byte order.
inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
inline u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3]; }
inline void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
inline void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

inline bool HasMagic(const u8 *data, size_t size, const char *magic, size_t len) {
	return size >= len && memcmp(data, magic, len) == 0;
}

// Paths are UTF-8 std::strings throughout; these convert at the filesystem boundary.
std::filesystem::path U8Path(std::string_view utf8);
std::string PathToU8(const std::filesystem::path &path);

bool ReadFileBytes(const std::string &path, Bytes *out, std::string *error);
// Writes via a temporary file and a rename, so a reader never sees a half-written output.
bool WriteFileBytes(const std::string &path, const u8 *data, size_t size, std::string *error);
inline bool WriteFileBytes(const std::string &path, const Bytes &data, std::string *error) {
	return WriteFileBytes(path, data.data(), data.size(), error);
}
std::string JoinPath(const std::string &dir, const std::string &rel);

std::string Sha1Hex(const u8 *data, size_t size);
std::string HexString(const u8 *data, size_t size);
bool ParseHex(std::string_view text, u8 *out, size_t size);
std::string Hex32(u32 value);  // "0x0000abcd"
std::string StringPrintf(const char *fmt, ...);

// Printable, NUL-terminated-or-full field from a fixed-size header.
std::string FixedString(const u8 *data, size_t maxLen);
