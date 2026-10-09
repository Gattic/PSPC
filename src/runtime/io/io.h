// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSP device namespace ("ms0:", "disc0:", "host0:", "flash0:", "emulator:") for IoFileMgrForUser
// and the module loader. A Device implements one mounted file system.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/common.h"

namespace io {

#pragma pack(push, 1)
struct PspDateTime {
	u16 year, month, day, hour, minute, second;
	u32 microsecond;
};
struct PspIoStat {
	s32 mode;
	u32 attr;
	s64 size;
	PspDateTime ctime, atime, mtime;
	u32 priv[6];
};
struct PspIoDirent {
	PspIoStat stat;
	char name[256];
	u32 priv;
	s32 dummy;
};
#pragma pack(pop)
static_assert(sizeof(PspIoStat) == 88, "SceIoStat layout");
static_assert(sizeof(PspIoDirent) == 352, "SceIoDirent layout");

enum : int {
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
};

enum : u32 {
	FIO_S_IFLNK = 0x4000,
	FIO_S_IFDIR = 0x1000,
	FIO_S_IFREG = 0x2000,
	FIO_SO_IFLNK = 0x0008,
	FIO_SO_IFDIR = 0x0010,
	FIO_SO_IFREG = 0x0020,
};

class Device {
public:
	virtual ~Device() = default;
	// Handles returned by Open/DOpen are device-local.
	virtual int Open(const std::string &path, int flags, int mode) = 0;
	virtual int Close(int h) = 0;
	virtual s64 Read(int h, void *buf, s64 size) = 0;
	virtual s64 Write(int h, const void *buf, s64 size) = 0;
	virtual s64 Seek(int h, s64 offset, int whence) = 0;
	virtual int GetStat(const std::string &path, PspIoStat &st) = 0;
	virtual int ChStat(const std::string &path, const PspIoStat &st, u32 bits) { return 0; }
	virtual int Remove(const std::string &path);
	virtual int Mkdir(const std::string &path, int mode);
	virtual int Rmdir(const std::string &path);
	virtual int Rename(const std::string &from, const std::string &to);
	virtual int DOpen(const std::string &path) = 0;
	virtual int DRead(int h, PspIoDirent &ent) = 0;  // 1 = entry, 0 = end, <0 error
	virtual int DClose(int h) = 0;
	virtual int Ioctl(int h, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen);
	virtual int Devctl(const std::string &devName, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen);
	virtual u32 DevType() const { return 0x20; }  // PSP_DEV_TYPE_FILE
};

void Init();
void Shutdown();
void Mount(const std::string &device, std::shared_ptr<Device> dev);
void Unmount(const std::string &device);
std::shared_ptr<Device> MountedDevice(const std::string &device);
// Splits "ms0:/PSP/GAME" into device "ms0" and path "/PSP/GAME", resolving the current
// directory for relative paths. Returns false for unknown devices.
bool ResolvePath(const std::string &pspPath, std::string &device, std::string &path);
void SetCwd(const std::string &pspDir);
const std::string &Cwd();

// Whole-file read through the PSP namespace (used by the module loader).
bool ReadFile(const std::string &pspPath, std::vector<u8> &out);

// Host-directory device (memory stick, host0:, flash0:).
std::shared_ptr<Device> MakeHostDevice(const std::string &hostRoot, bool readOnly);
// "emulator:" device used by pspautotests to send output.
std::shared_ptr<Device> MakeEmulatorDevice(bool hasDisplay);

// Host-side access for other modules (savedata, fonts...).
std::string HostPathFor(const std::string &pspPath);

}  // namespace io
