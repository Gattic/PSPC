// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/BlockDevices.h), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// Disc images as arrays of 2048-byte sectors: plain ISO, CSO (CISO v1/v2), ZSO and DAX.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/common.h"

namespace vfs {

constexpr u32 kSectorSize = 2048;

// Random-access bytes (a host file or a memory buffer). ReadAt is thread-safe and returns fewer
// bytes than asked only at the end of the data or on a host I/O error.
class ByteSource {
public:
	virtual ~ByteSource();
	virtual u64 Size() const = 0;
	virtual size_t ReadAt(u64 offset, void *out, size_t size) = 0;
};

// Opens a host file read-only (shared with other readers and writers). Null on failure.
std::unique_ptr<ByteSource> OpenFileSource(const std::filesystem::path &path);
std::unique_ptr<ByteSource> MakeMemorySource(std::vector<u8> data);

enum class ImageFormat {
	Iso,
	CsoV1,
	CsoV2,
	Zso,
	Dax,
};
const char *ImageFormatName(ImageFormat format);

class BlockDevice {
public:
	virtual ~BlockDevice();
	// Reads `count` sectors starting at `lba` into `out` (count * 2048 bytes). Thread-safe. Sectors
	// past the end, or that fail to read or decompress, come back zero-filled and make it return
	// false. A final partial sector of an image whose size isn't a multiple of 2048 reads fine,
	// zero-padded.
	virtual bool ReadBlocks(u32 lba, u32 count, u8 *out) = 0;
	virtual u32 NumBlocks() const = 0;
	// Uncompressed image size in bytes (may end inside the last sector).
	virtual u64 SizeBytes() const { return (u64)NumBlocks() * kSectorSize; }
	virtual ImageFormat Format() const = 0;

	bool ReadBlock(u32 lba, u8 *out) { return ReadBlocks(lba, 1, out); }
};

// Opens a disc image, detecting the format from its header: "CISO" (CSO v0/v1 and v2, deflate,
// v2 also LZ4), "ZISO" (ZSO, LZ4), "DAX\0" (DAX, zlib) and anything else as a plain ISO. JSO,
// CHD and NPDRM (PBP) images are recognized and refused with an error message.
std::unique_ptr<BlockDevice> OpenBlockDevice(const std::filesystem::path &path, std::string *error = nullptr);
std::unique_ptr<BlockDevice> OpenBlockDevice(std::unique_ptr<ByteSource> source, std::string *error = nullptr);

}  // namespace vfs
