// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/fileutil.h"

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

bool ReadWholeFile(const fs::path &path, std::vector<u8> &out) {
	FILE *f = _wfopen(path.wstring().c_str(), L"rb");
	if (!f) return false;
	_fseeki64(f, 0, SEEK_END);
	s64 size = _ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);
	if (size < 0) {
		fclose(f);
		return false;
	}
	out.resize((size_t)size);
	size_t got = size ? fread(out.data(), 1, (size_t)size, f) : 0;
	fclose(f);
	return got == (size_t)size;
}

bool WriteWholeFile(const fs::path &path, const void *data, size_t size) {
	std::error_code ec;
	if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
	FILE *f = _wfopen(path.wstring().c_str(), L"wb");
	if (!f) return false;
	size_t wrote = size ? fwrite(data, 1, size, f) : 0;
	fclose(f);
	return wrote == size;
}

bool WriteTextFile(const fs::path &path, const std::string &text) {
	return WriteWholeFile(path, text.data(), text.size());
}

std::string ToLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

std::string HexString(const u8 *data, size_t size) {
	static const char *hex = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for (size_t i = 0; i < size; i++) {
		s.push_back(hex[data[i] >> 4]);
		s.push_back(hex[data[i] & 15]);
	}
	return s;
}

fs::path ExecutableDir() {
#ifdef _WIN32
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return fs::path(std::wstring(buf, n)).parent_path();
#else
	return fs::current_path();
#endif
}
