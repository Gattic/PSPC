// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <string>

#include "common/common.h"

class Sha1 {
public:
	Sha1();
	void Update(const void *data, size_t size);
	std::array<u8, 20> Final();

	static std::array<u8, 20> Hash(const void *data, size_t size);

private:
	void Block(const u8 *block);
	u32 h_[5];
	u8 buf_[64];
	u64 total_ = 0;
	size_t used_ = 0;
};

// PSP NID: first four bytes of SHA-1(name), read little-endian.
u32 ComputeNid(const std::string &name);
