// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/ISOFileSystem.h), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// Read-only ISO9660 volume (PSP UMD images) over a BlockDevice: disc0:, and with sector mode umd0:.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fs/block_device.h"
#include "fs/filesystem.h"

namespace vfs {

// The UMD ioctls IsoFileSystem::Ioctl implements. Positions and sizes are in the handle's unit
// (sectors for sector-mode handles, bytes otherwise).
enum : u32 {
	UMD_IOCTL_SEEK = 0x01010005,           // in: {s64 offset; u32 unk; u32 whence}, stays inside the file
	UMD_IOCTL_GET_PVD = 0x01020001,        // out: 2048-byte primary volume descriptor (not in sector mode)
	UMD_IOCTL_GET_PATH_TABLE = 0x01020002, // out: the type-L path table (not in sector mode)
	UMD_IOCTL_GET_SECTOR_SIZE = 0x01020003,// out: u32 2048
	UMD_IOCTL_TELL = 0x01020004,           // out: u32 position
	UMD_IOCTL_GET_START_LBA = 0x01020006,  // out: u32 first sector of the file
	UMD_IOCTL_GET_SIZE = 0x01020007,       // out: u64 size
	UMD_IOCTL_READ = 0x01030008,           // in: u32 size; out: data. Same as Read
	UMD_IOCTL_SECTOR_TELL = 0x01D20001,    // out: u32 position
	UMD_IOCTL_SET_CACHE = 0x01F010DB,      // read-ahead cache setup: accepted, nothing to do
	UMD_IOCTL_SECTOR_SEEK = 0x01F100A6,    // like UMD_IOCTL_SEEK, past-end error is INVALID_FILE_SIZE
	UMD_IOCTL_SECTOR_READ = 0x01F30003,    // in: u32 count; out: data. Same as Read
};

// The most the launcher, the XMB and the boot code read whole of a metadata file (PARAM.SFO,
// ICON0, PIC1, SND0...), so a damaged record on a big image can't make them allocate gigabytes.
constexpr u64 kMaxMetadataBytes = 16u << 20;

// Parses "/sce_lbn<hex start>_size<hex size>" ("0x" prefixes optional), the raw-sector file names
// the PSP accepts on disc0:/umd0:. False if `path` isn't one.
bool ParseSceLbnPath(std::string_view path, u32 *startLba, u32 *size);

class IsoFileSystem : public FileSystem {
public:
	explicit IsoFileSystem(std::shared_ptr<BlockDevice> device);
	~IsoFileSystem() override;

	// False if no ISO9660 primary volume descriptor was found. Raw access (the device itself and
	// /sce_lbn paths) works regardless.
	bool IsValid() const { return root_ != nullptr; }
	const std::string &Error() const { return error_; }
	const std::shared_ptr<BlockDevice> &Device() const { return device_; }
	std::string VolumeId() const;
	// First sector and byte size of a file or directory, without opening it.
	bool GetExtent(std::string_view path, u32 *lba, u64 *size);
	// Convenience for loaders (PARAM.SFO, EBOOT.BIN). Returns 0 or an error; a file bigger than
	// `maxBytes` gives SCE_ERRNO_EFBIG without anything being allocated.
	s32 ReadWholeFile(std::string_view path, std::vector<u8> &out, u64 maxBytes = UINT64_MAX);

	u32 Caps() const override { return FS_CAP_READ_ONLY | FS_CAP_BLOCK_DEVICE | FS_CAP_CASE_INSENSITIVE; }
	// Paths: "" is the device (sector mode), "/sce_lbn..." a raw extent, anything else a file or
	// directory (a directory reads as its raw records, as in PPSSPP). Write access is
	// INVALID_FLAG. PSPC_O_SECTOR_MODE switches a file or raw extent to sector units.
	s32 Open(std::string_view path, u32 flags, s32 mode) override;
	s32 Close(s32 fd) override;
	s64 Read(s32 fd, void *buf, u64 size) override;
	s64 Write(s32 fd, const void *buf, u64 size) override;
	s64 Seek(s32 fd, s64 offset, s32 whence) override;
	s32 GetHandleInfo(s32 fd, HandleInfo *out) override;
	s32 Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) override;
	s32 GetStat(std::string_view path, SceIoStat *out) override;
	s32 ChStat(std::string_view path, const SceIoStat &in, u32 bits) override;
	s32 Remove(std::string_view path) override;
	s32 Mkdir(std::string_view path, s32 mode) override;
	s32 Rmdir(std::string_view path) override;
	s32 Rename(std::string_view from, std::string_view to) override;
	s32 DOpen(std::string_view path) override;
	s32 DRead(s32 dd, SceIoDirent *out) override;
	s32 DClose(s32 dd) override;

private:
	struct Node;
	struct OpenFile;
	struct OpenDir;

	void LoadDir(Node *dir);
	const Node *Lookup(std::string_view path);
	s32 AllocHandle();
	std::shared_ptr<OpenFile> GetFile(s32 fd);
	bool ReadBytes(u64 offset, u64 size, u8 *out);
	s64 ReadLocked(OpenFile &f, void *buf, u64 size);
	s64 SeekLocked(OpenFile &f, s64 offset, s32 whence);
	void FillStat(const Node *node, SceIoStat *out) const;

	std::shared_ptr<BlockDevice> device_;
	std::string error_;
	std::vector<u8> pvd_;
	std::unique_ptr<Node> root_;

	std::mutex treeMutex_;
	std::mutex mutex_;
	std::map<s32, std::shared_ptr<OpenFile>> files_;
	std::map<s32, std::shared_ptr<OpenDir>> dirs_;
	s32 nextHandle_ = 1;
};

}  // namespace vfs
