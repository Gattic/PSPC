// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <cstdio>
#include <system_error>
#include "common/common.h"

namespace hostfile {
// Wall-clock timestamps use Unix microseconds, independent of host filesystem epochs.
struct Info {
	bool directory = false, readOnly = false, hidden = false, system = false;
	u64 size = 0;
	s64 created = 0, accessed = 0, modified = 0;
};
bool Stat(const std::filesystem::path &path, Info &out, std::error_code &ec);
bool SetTimes(const std::filesystem::path &path, const s64 *created, const s64 *accessed,
              const s64 *modified, std::error_code &ec);
FILE *Open(const std::filesystem::path &path, bool read, bool write, bool create, bool exclusive);
s64 Tell(FILE *file);
int Seek(FILE *file, s64 offset, int origin);
bool Truncate(FILE *file, s64 size);
}
