// SPDX-License-Identifier: GPL-2.0-or-later
// The file system interface IoFileMgrForUser sits on. One instance per mounted volume; paths are
// relative to the volume (the HLE strips "ms0:" etc. and resolves the thread's cwd, see
// NormalizePspPath). Every method returns a firmware error code (< 0) on failure. Handles are
// small positive integers private to each FileSystem; the HLE maps guest descriptors onto them.
// All methods are thread-safe.
#pragma once

#include <string_view>
#include <vector>

#include "common/common.h"
#include "fs/psp_io_types.h"

namespace vfs {

enum FsCaps : u32 {
	FS_CAP_READ_ONLY = 1u << 0,
	// Sector-addressed volume (UMD): st_private[0] is the start LBA, the UMD ioctls work and the
	// device itself opens as path "".
	FS_CAP_BLOCK_DEVICE = 1u << 1,
	// FAT semantics (memory stick, flash): st_mode is 0777 or 0555 (read-only), st_attr carries the
	// FAT attribute bits and st_private is left alone.
	FS_CAP_FAT = 1u << 2,
	FS_CAP_CASE_INSENSITIVE = 1u << 3,
};

// What a handle refers to, for the HLE's own checks (buffer sizes of sector reads, ioctls).
struct HandleInfo {
	u32 flags = 0;           // PSP open flags (plus PSPC_O_SECTOR_MODE)
	bool sectorMode = false; // size and position count 2048-byte sectors, Read/Write sizes too
	bool rawLbn = false;     // opened through "/sce_lbn<start>_size<size>"
	bool device = false;     // the whole device (path "")
	u32 startLba = 0;        // first sector of the extent on block devices
	u64 size = 0;            // in bytes, or sectors in sector mode
	u64 position = 0;        // same unit as size
};

class FileSystem {
public:
	virtual ~FileSystem() = default;

	virtual u32 Caps() const = 0;

	// Returns a handle (> 0) or an error. `flags` are PSP_O_*; `mode` is the permission argument
	// of sceIoOpen (FAT ignores it).
	virtual s32 Open(std::string_view path, u32 flags, s32 mode) = 0;
	virtual s32 Close(s32 fd) = 0;
	// Bytes (sectors in sector mode) read or written, or an error. `buf` holds `size` units.
	virtual s64 Read(s32 fd, void *buf, u64 size) = 0;
	virtual s64 Write(s32 fd, const void *buf, u64 size) = 0;
	// Returns the new position. A target before the start returns -1 and leaves the position
	// alone, as the firmware does; seeking past the end is allowed.
	virtual s64 Seek(s32 fd, s64 offset, s32 whence) = 0;
	virtual s32 GetHandleInfo(s32 fd, HandleInfo *out) = 0;
	// Device control on an open file. `in`/`out` are host pointers valid for inLen/outLen bytes;
	// nothing outside those is touched. Returns 0, a positive result (reads) or an error.
	virtual s32 Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) = 0;

	// Fills mode, attr, size and the three times; st_private[0] only on block devices.
	virtual s32 GetStat(std::string_view path, SceIoStat *out) = 0;
	// `bits` is SCE_CST_*; unsupported fields are ignored.
	virtual s32 ChStat(std::string_view path, const SceIoStat &in, u32 bits) = 0;
	virtual s32 Remove(std::string_view path) = 0;
	virtual s32 Mkdir(std::string_view path, s32 mode) = 0;
	virtual s32 Rmdir(std::string_view path) = 0;
	virtual s32 Rename(std::string_view from, std::string_view to) = 0;

	// Directory listing snapshot taken at DOpen. DRead returns 1 and fills d_stat and d_name, or 0
	// at the end (d_name emptied); d_private and dummy are never touched.
	virtual s32 DOpen(std::string_view path) = 0;
	virtual s32 DRead(s32 dd, SceIoDirent *out) = 0;
	virtual s32 DClose(s32 dd) = 0;

	// Free and total bytes of the volume, for the memory stick devctls.
	virtual bool QueryFreeSpace(u64 *freeBytes, u64 *totalBytes) {
		return false;
	}
};

// Reads from a handle's position to its end into `out` and puts the position back
// (sceKernelLoadModuleByID loads from an open descriptor this way). False on an error, and for
// sector-mode handles (umd0:), whose size and reads count sectors rather than bytes.
bool ReadRest(FileSystem &fs, s32 handle, std::vector<u8> &out);

}  // namespace vfs
