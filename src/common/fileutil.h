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
// Directory containing the running executable.
fs::path ExecutableDir();
