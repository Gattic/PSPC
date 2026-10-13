// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common.h"

namespace fs = std::filesystem;

bool ReadWholeFile(const fs::path &path, std::vector<u8> &out);
bool WriteWholeFile(const fs::path &path, const void *data, size_t size);
bool WriteTextFile(const fs::path &path, const std::string &text);
std::string ToLower(std::string s);
std::string HexString(const u8 *data, size_t size);
// UTF-8 text of a path (C++20 u8string() returns std::u8string).
std::string PathToUtf8(const fs::path &p);
// Directory containing the running executable.
fs::path ExecutableDir();
#ifdef _WIN32
// Console tools accept UTF-8 paths internally, regardless of the user's Windows code page.
int RunUtf8Main(int (*run)(int, char **), int argc, wchar_t **argv);
#endif
