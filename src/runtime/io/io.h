// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSP device namespace ("ms0:", "disc0:", "umd0:", "host0:", "flash0:", "emulator:") for
// IoFileMgrForUser and the module loader. A Device is one mounted volume: a vfs::FileSystem
// plus the behaviour the HLE needs to know about (FAT, UMD timing, devctls).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/common.h"
#include "fs/filesystem.h"
#include "runtime/kernel/kobject.h"

namespace vfs {
class HostFileSystem;
}

namespace io {

// Guest structures and flags (same layouts as vfs::SceIoStat / vfs::SceIoDirent).
using PspDateTime = vfs::ScePspDateTime;
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
static_assert(sizeof(PspIoStat) == sizeof(vfs::SceIoStat), "SceIoStat layout");
static_assert(sizeof(PspIoDirent) == sizeof(vfs::SceIoDirent), "SceIoDirent layout");

constexpr u32 PSP_O_RDONLY = vfs::PSP_O_RDONLY, PSP_O_WRONLY = vfs::PSP_O_WRONLY, PSP_O_RDWR = vfs::PSP_O_RDWR,
              PSP_O_APPEND = vfs::PSP_O_APPEND, PSP_O_CREAT = vfs::PSP_O_CREAT, PSP_O_TRUNC = vfs::PSP_O_TRUNC,
              PSP_O_EXCL = vfs::PSP_O_EXCL;
constexpr u32 FIO_S_IFDIR = vfs::FIO_S_IFDIR, FIO_S_IFREG = vfs::FIO_S_IFREG, FIO_SO_IFDIR = vfs::FIO_SO_IFDIR,
              FIO_SO_IFREG = vfs::FIO_SO_IFREG;

enum class DevClass {
	MemoryStick,  // ms0: (FAT, d_private short names, memory stick devctls)
	Flash,        // flash0: (FAT, read-only)
	Host,         // host0: (PSPLink usbhostfs semantics)
	UmdFiles,     // disc0: (ISO9660 files)
	UmdBlock,     // umd0:/umd1:/umd: (the whole disc in sectors)
	Emulator,     // emulator:/kemulator: (pspautotests output)
	GuestDriver,  // a driver the game added (sceIoAddDrv)
};

class Device {
public:
	virtual ~Device() = default;
	virtual DevClass Class() const = 0;
	// Null for character devices.
	virtual vfs::FileSystem *Fs() { return nullptr; }
	// sceIoGetDevType: 0x10 file system, 0x04 block device.
	virtual u32 DevType() const;
	// Called by Mount with the canonical device name ("ms0", "host0", ...).
	virtual void OnMount(const std::string &name) {}
	// sceIoDevctl for this device. `devName` is the name as the guest wrote it ("fatms0:").
	// Guest addresses; *delayUs may be raised to make the caller wait. SCE_KERNEL_ERROR_UNSUP
	// when the command is unknown.
	virtual int Devctl(const std::string &devName, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen, u32 *delayUs);

	bool IsFat() const { return Class() == DevClass::MemoryStick || Class() == DevClass::Flash; }
	bool IsUmd() const { return Class() == DevClass::UmdFiles || Class() == DevClass::UmdBlock; }

	// Host-side file access for other runtime modules (savedata, fonts...): forwards to Fs() with
	// device-relative paths, no guest timing. Errors as from the vfs.
	int Open(const std::string &path, u32 flags, int mode);
	int Close(int h);
	s64 Read(int h, void *buf, s64 size);
	s64 Write(int h, const void *buf, s64 size);
	s64 Seek(int h, s64 offset, int whence);
	int GetStat(const std::string &path, PspIoStat &st);
	int ChStat(const std::string &path, const PspIoStat &st, u32 bits);
	int Remove(const std::string &path);
	int Mkdir(const std::string &path, int mode);
	int Rmdir(const std::string &path);
	int Rename(const std::string &from, const std::string &to);
	int DOpen(const std::string &path);
	int DRead(int h, PspIoDirent &ent);  // 1 = entry, 0 = end, <0 error
	int DClose(int h);
};

void Init();
void Shutdown();
// sceIoAddDrv / sceIoDelDrv (guest_driver.cpp): a device whose driver is the game's own, the
// PspIoDrv at drvAddr; its functions run on the calling thread. 0 or a PSP error.
int AddGuestDriver(u32 drvAddr);
int DelGuestDriver(const std::string &name);
void ResetGuestDrivers();
// `device` without the colon; aliases are folded ("fatms0" and "memstick" are ms0, "umd1",
// "umd" and "umd00" are umd0, device names compare case-insensitively).
void Mount(const std::string &device, std::shared_ptr<Device> dev);
void Unmount(const std::string &device);
std::shared_ptr<Device> MountedDevice(const std::string &device);
std::string CanonicalDevice(std::string_view device);

// A guest path resolved for the calling thread: device plus device-relative path. `path` is ""
// for the device itself ("umd0:") and starts with '/' otherwise.
struct Resolved {
	std::shared_ptr<Device> dev;
	std::string device;  // canonical name, no colon
	std::string path;
};
// 0, SCE_KERNEL_ERROR_NOCWD for a relative path without a cwd, SCE_KERNEL_ERROR_NODEV.
int Resolve(std::string_view pspPath, Resolved *out);
// Same, as device name plus path; false if it doesn't resolve.
bool ResolvePath(const std::string &pspPath, std::string &device, std::string &path);

// Current directory. Each thread has its own (sceIoChdir); threads that never set one use the
// default (the boot directory).
void SetCwd(const std::string &pspDir);
const std::string &Cwd();
std::string ThreadCwd(SceUID thread);
void SetThreadCwd(SceUID thread, const std::string &dir);

// Whole-file read through the PSP namespace (used by the module loader).
bool ReadFile(const std::string &pspPath, std::vector<u8> &out);
// Reads an open guest descriptor from its current position to the end, leaving the position
// unchanged (sceKernelLoadModuleByID). Implemented in iofilemgr.cpp.
bool ReadRestOfFd(int fd, std::vector<u8> &out, std::string *path);
// Host-side path of a file on a host-directory device, or "" (savedata, fonts...).
std::string HostPathFor(const std::string &pspPath);

// Host-directory device; its flavour (memory stick, flash, host0) follows the mount name.
std::shared_ptr<Device> MakeHostDevice(const std::string &hostRoot, bool readOnly);
// Puts a file system in front of a mounted host-directory device (the XMB's ISO browsing on ms0:):
// `make` gets the device's own file system and returns the one the guest sees. HostPathFor keeps
// answering for the host directory. Until the device is mounted again. False if `device` isn't a
// host-directory device.
bool SetFsOverlay(const std::string &device, const std::function<std::shared_ptr<vfs::FileSystem>(vfs::HostFileSystem *)> &make);
// Overlays host directory `dir` onto host-directory device `device` (vfs::HostFileSystem::
// AddFallbackRoot): pspc's plugins folder on ms0:. False if `device` isn't one or `dir` is missing.
bool AddHostFallback(const std::string &device, const std::string &dir);
// The host folder of a second memory stick, which goes into ms0: when the XMB asks for another
// stick (copying a save in the Saved Data Utility); the first one is then the other.
void SetOtherMemoryStick(const std::string &hostDir);
// sceIoDopen calls this first: the Saved Data Utility listing the saves again ends a copy
// between memory sticks ("Copy completed. Insert the source Memory Stick").
void OnDirectoryOpen(const std::string &device, const std::string &path);
std::shared_ptr<Device> MakeVfsDevice(std::shared_ptr<vfs::FileSystem> fs, DevClass cls);
// "emulator:" device pspautotests send their output through.
std::shared_ptr<Device> MakeEmulatorDevice(bool hasDisplay);
// pspautotests' host0:/__testoutput.txt, __testerror.txt and __testfinish.txt are `path`,
// path.err and path.finish (--test-output), so tests running in parallel each have their own.
// Before host0: is mounted.
void SetTestOutput(const std::string &path);
// host0: shows names as the PC has them (psplink's view, pspautotests io/io) instead of as the
// memory stick's FAT driver would (io/directory, the default). Before host0 is mounted.
void SetHost0RawNames(bool raw);
// The memory stick's raw partition ("msstor0p1:") the XMB queries.
std::shared_ptr<Device> MakeMsStorDevice();

}  // namespace io
