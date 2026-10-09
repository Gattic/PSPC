// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/ELF/PBPReader.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// EBOOT.PBP container: "\0PBP", a version word and eight section offsets.
#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "common/common.h"

namespace vfs {

class ByteSource;

enum class PbpSection : u32 {
	ParamSfo = 0,  // PARAM.SFO
	Icon0Png,      // ICON0.PNG
	Icon1Pmf,      // ICON1.PMF
	Pic0Png,       // PIC0.PNG
	Pic1Png,       // PIC1.PNG
	Snd0At3,       // SND0.AT3
	DataPsp,       // DATA.PSP (the executable)
	DataPsar,      // DATA.PSAR
	Count,
};

class PbpFile {
public:
	PbpFile();
	~PbpFile();

	// Reads the header; sections are read on demand (DATA.PSAR can be most of a PSN game).
	bool Open(const std::filesystem::path &path);
	bool Open(std::vector<u8> bytes);
	bool Open(const u8 *data, size_t size) { return Open(std::vector<u8>(data, data + size)); }
	void Close();
	bool IsOpen() const { return source_ != nullptr; }

	u32 Version() const { return version_; }
	u64 FileSize() const { return fileSize_; }
	// A section runs to the next section's offset (the last one to the end of the file). Offsets
	// that are out of order or past the end give an empty section.
	u64 SectionOffset(PbpSection which) const;
	u64 SectionSize(PbpSection which) const;
	bool GetSection(PbpSection which, std::vector<u8> &out) const;
	// Reads part of a section; returns the bytes read.
	size_t ReadSection(PbpSection which, u64 offset, void *out, size_t size) const;

	static const char *SectionName(PbpSection which);

private:
	bool Init();

	std::unique_ptr<ByteSource> source_;
	u64 fileSize_ = 0;
	u32 version_ = 0;
	u32 offsets_[8] = {};
};

}  // namespace vfs
