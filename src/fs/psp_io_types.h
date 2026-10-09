// SPDX-License-Identifier: GPL-2.0-or-later
// Guest-visible IoFileMgr types with their exact PSP layouts, plus the firmware error codes the
// file systems return. Layouts are little-endian, naturally aligned, as they sit in guest memory.
#pragma once

#include <cstddef>

#include "common/common.h"

namespace vfs {

// sceIoOpen flags (pspiofilemgr_fcntl.h).
enum : u32 {
	PSP_O_RDONLY = 0x0001,
	PSP_O_WRONLY = 0x0002,
	PSP_O_RDWR = 0x0003,
	PSP_O_NBLOCK = 0x0004,
	PSP_O_DIROPEN = 0x0008,
	PSP_O_APPEND = 0x0100,
	PSP_O_CREAT = 0x0200,
	PSP_O_TRUNC = 0x0400,
	PSP_O_EXCL = 0x0800,
	PSP_O_NOWAIT = 0x8000,
	PSP_O_NPDRM = 0x40000000,  // set by sceNpDrm for PGD-encrypted files
};

// PSPC extension for FileSystem::Open: positions and sizes of this handle are in 2048-byte sectors.
// The HLE sets it for opens through umd0:/umd1:/umd: and must clear it from guest-supplied flags.
// Opening the device itself (path "") on an IsoFileSystem always uses sector mode.
constexpr u32 PSPC_O_SECTOR_MODE = 0x10000000;

// sceIoLseek whence.
enum : s32 {
	PSP_SEEK_SET = 0,
	PSP_SEEK_CUR = 1,
	PSP_SEEK_END = 2,
};

// SceIoStat.st_mode (pspiofilemgr_stat.h).
enum : u32 {
	FIO_S_IFMT = 0xF000,
	FIO_S_IFLNK = 0x4000,
	FIO_S_IFDIR = 0x1000,
	FIO_S_IFREG = 0x2000,
	FIO_S_ISUID = 0x0800,
	FIO_S_ISGID = 0x0400,
	FIO_S_ISVTX = 0x0200,
	FIO_S_IRWXU = 0x01C0,
	FIO_S_IRUSR = 0x0100,
	FIO_S_IWUSR = 0x0080,
	FIO_S_IXUSR = 0x0040,
	FIO_S_IRWXG = 0x0038,
	FIO_S_IRGRP = 0x0020,
	FIO_S_IWGRP = 0x0010,
	FIO_S_IXGRP = 0x0008,
	FIO_S_IRWXO = 0x0007,
	FIO_S_IROTH = 0x0004,
	FIO_S_IWOTH = 0x0002,
	FIO_S_IXOTH = 0x0001,
};

// SceIoStat.st_attr. On a FAT volume this is the FAT attribute byte, whose directory (0x10) and
// archive (0x20) bits coincide with FIO_SO_IFDIR/IFREG; FAT_ATTR_* are its other bits.
enum : u32 {
	FIO_SO_IFMT = 0x0038,
	FIO_SO_IFLNK = 0x0008,
	FIO_SO_IFDIR = 0x0010,
	FIO_SO_IFREG = 0x0020,
	FIO_SO_IROTH = 0x0004,
	FIO_SO_IWOTH = 0x0002,
	FIO_SO_IXOTH = 0x0001,
	FAT_ATTR_READONLY = 0x01,
	FAT_ATTR_HIDDEN = 0x02,
	FAT_ATTR_SYSTEM = 0x04,
};

// sceIoChstat change bits.
enum : u32 {
	SCE_CST_MODE = 0x0001,
	SCE_CST_ATTR = 0x0002,
	SCE_CST_SIZE = 0x0004,
	SCE_CST_CT = 0x0008,
	SCE_CST_AT = 0x0010,
	SCE_CST_MT = 0x0020,
	SCE_CST_PRVT = 0x0040,
};

struct ScePspDateTime {
	u16 year;
	u16 month;
	u16 day;
	u16 hour;
	u16 minute;
	u16 second;
	u32 microsecond;
};

struct SceIoStat {
	s32 st_mode;
	u32 st_attr;
	s64 st_size;
	ScePspDateTime st_ctime;
	ScePspDateTime st_atime;
	ScePspDateTime st_mtime;
	// Device specific. On a UMD st_private[0] is the file's first sector. Our file systems never
	// touch the other words (the real kernel leaves them as the caller had them).
	u32 st_private[6];
};

struct SceIoDirent {
	SceIoStat d_stat;
	char d_name[256];
	u32 d_private;  // guest pointer to SceIoFatDirentPrivate (memory stick), handled by the HLE
	s32 dummy;
};

static_assert(sizeof(ScePspDateTime) == 16, "ScePspDateTime layout");
static_assert(sizeof(SceIoStat) == 88, "SceIoStat layout");
static_assert(offsetof(SceIoStat, st_size) == 8, "SceIoStat layout");
static_assert(offsetof(SceIoStat, st_ctime) == 16, "SceIoStat layout");
static_assert(offsetof(SceIoStat, st_atime) == 32, "SceIoStat layout");
static_assert(offsetof(SceIoStat, st_mtime) == 48, "SceIoStat layout");
static_assert(offsetof(SceIoStat, st_private) == 64, "SceIoStat layout");
static_assert(sizeof(SceIoDirent) == 352, "SceIoDirent layout");
static_assert(offsetof(SceIoDirent, d_name) == 88, "SceIoDirent layout");
static_assert(offsetof(SceIoDirent, d_private) == 344, "SceIoDirent layout");

// Firmware error codes (negative when held in s32). Names follow the errno they wrap.
constexpr s32 SCE_ERRNO_ENOENT = (s32)0x80010002;
constexpr s32 SCE_ERRNO_EIO = (s32)0x80010005;
constexpr s32 SCE_ERRNO_EBADF = (s32)0x80010009;
constexpr s32 SCE_ERRNO_ENOMEM = (s32)0x8001000C;
constexpr s32 SCE_ERRNO_EACCES = (s32)0x8001000D;
constexpr s32 SCE_ERRNO_EBUSY = (s32)0x80010010;
constexpr s32 SCE_ERRNO_EEXIST = (s32)0x80010011;
constexpr s32 SCE_ERRNO_EXDEV = (s32)0x80010012;
constexpr s32 SCE_ERRNO_ENODEV = (s32)0x80010013;
constexpr s32 SCE_ERRNO_ENOTDIR = (s32)0x80010014;
constexpr s32 SCE_ERRNO_EISDIR = (s32)0x80010015;
constexpr s32 SCE_ERRNO_EINVAL = (s32)0x80010016;
constexpr s32 SCE_ERRNO_EMFILE = (s32)0x80010018;
constexpr s32 SCE_ERRNO_EFBIG = (s32)0x8001001B;
constexpr s32 SCE_ERRNO_ENOSPC = (s32)0x8001001C;
constexpr s32 SCE_ERRNO_EROFS = (s32)0x8001001E;
constexpr s32 SCE_ERRNO_ENAMETOOLONG = (s32)0x80010024;
constexpr s32 SCE_ERRNO_ENOTEMPTY = (s32)0x8001005A;
constexpr s32 SCE_ERRNO_FUNCTION_NOT_SUPPORTED = (s32)0x8001B000;
constexpr s32 SCE_ERRNO_INVALID_FILE_SIZE = (s32)0x8001B003;
constexpr s32 SCE_ERRNO_INVALID_FLAG = (s32)0x8001B004;
// iofilemgr's own checks: unknown descriptor, or a read/write the descriptor wasn't opened for
// (pspautotests io/file records 0x80020323 for both).
constexpr s32 SCE_KERNEL_ERROR_BADF = (s32)0x80020323;
constexpr s32 SCE_KERNEL_ERROR_INVAL = (s32)0x80020324;

}  // namespace vfs
