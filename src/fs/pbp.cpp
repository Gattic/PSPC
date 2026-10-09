// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/ELF/PBPReader.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include "fs/pbp.h"

#include <algorithm>
#include <cstring>

#include "fs/block_device.h"

namespace vfs {

PbpFile::PbpFile() = default;
PbpFile::~PbpFile() = default;

bool PbpFile::Open(const std::filesystem::path &path) {
	Close();
	source_ = OpenFileSource(path);
	return Init();
}

bool PbpFile::Open(std::vector<u8> bytes) {
	Close();
	source_ = MakeMemorySource(std::move(bytes));
	return Init();
}

void PbpFile::Close() {
	source_.reset();
	fileSize_ = 0;
	version_ = 0;
	memset(offsets_, 0, sizeof(offsets_));
}

bool PbpFile::Init() {
	u8 h[40];
	if (!source_ || source_->ReadAt(0, h, sizeof(h)) != sizeof(h) || memcmp(h, "\0PBP", 4) != 0) {
		Close();
		return false;
	}
	fileSize_ = source_->Size();
	auto le32 = [](const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); };
	version_ = le32(h + 4);
	for (int i = 0; i < 8; i++) {
		offsets_[i] = le32(h + 8 + i * 4);
	}
	return true;
}

u64 PbpFile::SectionOffset(PbpSection which) const {
	u32 i = (u32)which;
	return i < 8 ? offsets_[i] : 0;
}

u64 PbpFile::SectionSize(PbpSection which) const {
	u32 i = (u32)which;
	if (!source_ || i >= 8) {
		return 0;
	}
	u64 start = offsets_[i];
	u64 stop = i + 1 < 8 ? offsets_[i + 1] : fileSize_;
	if (stop < start || stop > fileSize_) {
		return 0;
	}
	return stop - start;
}

bool PbpFile::GetSection(PbpSection which, std::vector<u8> &out) const {
	out.clear();
	if (!source_ || (u32)which >= 8) {
		return false;
	}
	u64 size = SectionSize(which);
	if (size > (u64)SIZE_MAX / 2) {
		return false;
	}
	out.resize((size_t)size);
	if (size && source_->ReadAt(SectionOffset(which), out.data(), out.size()) != out.size()) {
		out.clear();
		return false;
	}
	return true;
}

size_t PbpFile::ReadSection(PbpSection which, u64 offset, void *out, size_t size) const {
	u64 secSize = SectionSize(which);
	if (!source_ || offset >= secSize) {
		return 0;
	}
	size_t n = (size_t)std::min<u64>(size, secSize - offset);
	return source_->ReadAt(SectionOffset(which) + offset, out, n);
}

const char *PbpFile::SectionName(PbpSection which) {
	static const char *const names[] = {"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG", "PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR"};
	u32 i = (u32)which;
	return i < 8 ? names[i] : "?";
}

}  // namespace vfs
