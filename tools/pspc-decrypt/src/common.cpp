// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "../libkirk/SHA1.h"

namespace fs = std::filesystem;

fs::path U8Path(std::string_view utf8) {
	return fs::path(std::u8string((const char8_t *)utf8.data(), utf8.size()));
}

std::string PathToU8(const fs::path &path) {
	const std::u8string s = path.u8string();
	return std::string((const char *)s.data(), s.size());
}

bool ReadFileBytes(const std::string &path, Bytes *out, std::string *error) {
	std::ifstream f(U8Path(path), std::ios::binary);
	if (!f) {
		*error = "cannot open " + path;
		return false;
	}
	f.seekg(0, std::ios::end);
	const std::streamoff size = f.tellg();
	if (size < 0) {
		*error = "cannot size " + path;
		return false;
	}
	f.seekg(0, std::ios::beg);
	out->resize((size_t)size);
	if (size > 0 && !f.read((char *)out->data(), size)) {
		*error = "cannot read " + path;
		return false;
	}
	return true;
}

bool WriteFileBytes(const std::string &path, const u8 *data, size_t size, std::string *error) {
	const fs::path target = U8Path(path);
	std::error_code ec;
	if (target.has_parent_path()) {
		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			*error = "cannot create directory for " + path + ": " + ec.message();
			return false;
		}
	}
	fs::path temp = target;
	temp += ".part";
	{
		std::ofstream f(temp, std::ios::binary | std::ios::trunc);
		if (!f) {
			*error = "cannot create " + path;
			return false;
		}
		if (size > 0 && !f.write((const char *)data, (std::streamsize)size)) {
			*error = "cannot write " + path;
			return false;
		}
	}
	fs::rename(temp, target, ec);
	if (ec) {
		// rename() can refuse to replace on some filesystems; fall back to remove + rename.
		fs::remove(target, ec);
		fs::rename(temp, target, ec);
		if (ec) {
			fs::remove(temp, ec);
			*error = "cannot move output into place: " + path;
			return false;
		}
	}
	return true;
}

std::string JoinPath(const std::string &dir, const std::string &rel) {
	return PathToU8(U8Path(dir) / U8Path(rel));
}

std::string HexString(const u8 *data, size_t size) {
	static const char digits[] = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for (size_t i = 0; i < size; i++) {
		s.push_back(digits[data[i] >> 4]);
		s.push_back(digits[data[i] & 15]);
	}
	return s;
}

std::string Sha1Hex(const u8 *data, size_t size) {
	SHA_CTX ctx;
	SHAInit(&ctx);
	// SHAUpdate takes an int; feed large buffers in pieces.
	while (size > 0) {
		const int chunk = size > 0x40000000 ? 0x40000000 : (int)size;
		SHAUpdate(&ctx, data, chunk);
		data += chunk;
		size -= chunk;
	}
	u8 digest[20];
	SHAFinal(digest, &ctx);
	return HexString(digest, sizeof(digest));
}

bool ParseHex(std::string_view text, u8 *out, size_t size) {
	if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text.remove_prefix(2);
	}
	if (text.size() != size * 2) {
		return false;
	}
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < size; i++) {
		const int hi = nibble(text[i * 2]), lo = nibble(text[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[i] = (u8)((hi << 4) | lo);
	}
	return true;
}

std::string Hex32(u32 value) {
	return StringPrintf("0x%08x", value);
}

std::string StringPrintf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char buf[512];
	const int n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n < 0) {
		return std::string();
	}
	if ((size_t)n < sizeof(buf)) {
		return std::string(buf, n);
	}
	std::string big((size_t)n + 1, '\0');
	va_start(args, fmt);
	vsnprintf(big.data(), big.size(), fmt, args);
	va_end(args);
	big.resize(n);
	return big;
}

std::string FixedString(const u8 *data, size_t maxLen) {
	size_t len = 0;
	while (len < maxLen && data[len] != 0) {
		len++;
	}
	std::string s((const char *)data, len);
	for (char &c : s) {
		if ((u8)c < 0x20 || (u8)c == 0x7F) {
			c = '?';
		}
	}
	return s;
}
