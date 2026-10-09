// SPDX-License-Identifier: GPL-2.0-or-later
//
// IoFileMgrForUser and StdioForUser. Asynchronous calls complete immediately; the result is held
// until sceIoWaitAsync/PollAsync collects it.
#include <map>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/io/io.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"

namespace {

constexpr int kStdin = 0, kStdout = 1, kStderr = 2;
constexpr int kFirstFd = 3, kMaxFds = 64;

struct OpenFile {
	std::shared_ptr<io::Device> dev;
	std::string devName;
	std::string path;
	int handle = -1;
	bool isDir = false;
	int flags = 0;
	bool asyncPending = false;
	s64 asyncResult = 0;
	SceUID asyncCallback = 0;
	u32 asyncCallbackArg = 0;
};

std::map<int, OpenFile> g_fds;

int AllocFd() {
	for (int fd = kFirstFd; fd < kMaxFds; fd++)
		if (!g_fds.count(fd)) return fd;
	return -1;
}

OpenFile *GetFd(int fd) {
	auto it = g_fds.find(fd);
	return it == g_fds.end() ? nullptr : &it->second;
}

void SetAsync(OpenFile *f, s64 result) {
	f->asyncPending = true;
	f->asyncResult = result;
	if (f->asyncCallback) kernel::NotifyCallback(f->asyncCallback, (s32)f->asyncCallbackArg);
}

int sceIoOpen(const char *path, int flags, int mode) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	std::string dev, p;
	if (!io::ResolvePath(path, dev, p)) {
		LOGW("IO", "sceIoOpen(%s): no such device", path);
		return (int)SCE_KERNEL_ERROR_NODEV;
	}
	auto d = io::MountedDevice(dev);
	int fd = AllocFd();
	if (fd < 0) return (int)SCE_KERNEL_ERROR_ERRNO_TOO_MANY_OPEN_SYSTEM_FILES;
	int h = d->Open(p, flags, mode);
	if (h < 0) {
		LOGD("IO", "sceIoOpen(%s, %x) -> %08x", path, flags, h);
		return h;
	}
	OpenFile &f = g_fds[fd];
	f.dev = d;
	f.devName = dev;
	f.path = p;
	f.handle = h;
	f.flags = flags;
	LOGD("IO", "sceIoOpen(%s, %x) -> fd %d", path, flags, fd);
	return fd;
}

int sceIoClose(int fd) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->isDir) f->dev->DClose(f->handle);
	else f->dev->Close(f->handle);
	g_fds.erase(fd);
	return 0;
}

int sceIoRead(int fd, u32 buf, u32 size) {
	if (fd == kStdin) return 0;
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if ((s32)size < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (size == 0) return 0;
	if (!mem::IsValidRange(buf, size)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	return (int)f->dev->Read(f->handle, GPtr(buf), size);
}

int sceIoWrite(int fd, u32 buf, u32 size) {
	if (fd == kStdout || fd == kStderr) {
		if (mem::IsValidRange(buf, size)) logging::GuestOutput((const char *)GPtr(buf), size);
		return (int)size;
	}
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (size == 0) return 0;
	if (!mem::IsValidRange(buf, size)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	return (int)f->dev->Write(f->handle, GPtr(buf), size);
}

s64 sceIoLseek(int fd, s64 offset, int whence) {
	OpenFile *f = GetFd(fd);
	if (!f) return (s64)(s32)SCE_KERNEL_ERROR_BADF;
	if (whence < 0 || whence > 2) return (s64)(s32)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	return f->dev->Seek(f->handle, offset, whence);
}

int sceIoLseek32(int fd, int offset, int whence) { return (int)sceIoLseek(fd, offset, whence); }

int sceIoOpenAsync(const char *path, int flags, int mode) {
	int fd = sceIoOpen(path, flags, mode);
	if (fd < 0) return fd;
	SetAsync(GetFd(fd), fd);
	return fd;
}
int sceIoCloseAsync(int fd) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	// The descriptor stays allocated until the result is collected.
	if (f->isDir) f->dev->DClose(f->handle);
	else f->dev->Close(f->handle);
	f->handle = -1;
	SetAsync(f, 0);
	f->flags |= 0x40000000;  // closing
	return 0;
}
int sceIoReadAsync(int fd, u32 buf, u32 size) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	SetAsync(f, sceIoRead(fd, buf, size));
	return 0;
}
int sceIoWriteAsync(int fd, u32 buf, u32 size) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	SetAsync(f, sceIoWrite(fd, buf, size));
	return 0;
}
int sceIoLseekAsync(int fd, s64 offset, int whence) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	SetAsync(f, sceIoLseek(fd, offset, whence));
	return 0;
}
int sceIoLseek32Async(int fd, int offset, int whence) { return sceIoLseekAsync(fd, offset, whence); }

static int CollectAsync(int fd, u32 resPtr, bool poll) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (!f->asyncPending) return (int)SCE_KERNEL_ERROR_NOASYNC;
	if (mem::IsValidRange(resPtr, 8)) GWrite<s64>(resPtr, f->asyncResult);
	f->asyncPending = false;
	if (f->flags & 0x40000000) g_fds.erase(fd);
	(void)poll;
	return 0;
}

int sceIoWaitAsync(int fd, u32 resPtr) { return CollectAsync(fd, resPtr, false); }
int sceIoWaitAsyncCB(int fd, u32 resPtr) {
	if (kernel::PspThread *t = sched::Current()) kernel::RunPendingCallbacks(t);
	return CollectAsync(fd, resPtr, false);
}
int sceIoPollAsync(int fd, u32 resPtr) { return CollectAsync(fd, resPtr, true); }
int sceIoGetAsyncStat(int fd, int poll, u32 resPtr) { return CollectAsync(fd, resPtr, poll != 0); }
int sceIoChangeAsyncPriority(int fd, int prio) {
	(void)prio;
	if (fd != -1 && !GetFd(fd)) return (int)SCE_KERNEL_ERROR_BADF;
	return 0;
}
int sceIoSetAsyncCallback(int fd, SceUID cb, u32 arg) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	f->asyncCallback = cb;
	f->asyncCallbackArg = arg;
	return 0;
}
int sceIoCancel(int fd) {
	if (!GetFd(fd)) return (int)SCE_KERNEL_ERROR_BADF;
	return (int)SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED;
}

int sceIoGetstat(const char *path, u32 statPtr) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	std::string dev, p;
	if (!io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	io::PspIoStat st{};
	int r = io::MountedDevice(dev)->GetStat(p, st);
	if (r < 0) return r;
	if (mem::IsValidRange(statPtr, sizeof(st))) mem::WriteBytes(statPtr, &st, sizeof(st));
	return 0;
}

int sceIoChstat(const char *path, u32 statPtr, u32 bits) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	std::string dev, p;
	if (!io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	io::PspIoStat st{};
	if (mem::IsValidRange(statPtr, sizeof(st))) mem::ReadBytes(&st, statPtr, sizeof(st));
	return io::MountedDevice(dev)->ChStat(p, st, bits);
}

int sceIoRemove(const char *path) {
	std::string dev, p;
	if (!path || !io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	return io::MountedDevice(dev)->Remove(p);
}
int sceIoMkdir(const char *path, int mode) {
	std::string dev, p;
	if (!path || !io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	return io::MountedDevice(dev)->Mkdir(p, mode);
}
int sceIoRmdir(const char *path) {
	std::string dev, p;
	if (!path || !io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	return io::MountedDevice(dev)->Rmdir(p);
}
int sceIoRename(const char *from, const char *to) {
	std::string d1, p1, d2, p2;
	if (!from || !to || !io::ResolvePath(from, d1, p1)) return (int)SCE_KERNEL_ERROR_NODEV;
	// The target may omit the device.
	std::string t = to;
	if (t.find(':') == std::string::npos) t = d1 + ":" + (t[0] == '/' ? t : "/" + t);
	if (!io::ResolvePath(t, d2, p2) || d1 != d2) return (int)SCE_KERNEL_ERROR_ERRNO_CROSS_DEV_LINK;
	return io::MountedDevice(d1)->Rename(p1, p2);
}
int sceIoChdir(const char *path) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	std::string dev, p;
	if (!io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	io::SetCwd(dev + ":" + p);
	return 0;
}

int sceIoDopen(const char *path) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	std::string dev, p;
	if (!io::ResolvePath(path, dev, p)) return (int)SCE_KERNEL_ERROR_NODEV;
	auto d = io::MountedDevice(dev);
	int fd = AllocFd();
	if (fd < 0) return (int)SCE_KERNEL_ERROR_ERRNO_TOO_MANY_OPEN_SYSTEM_FILES;
	int h = d->DOpen(p);
	if (h < 0) return h;
	OpenFile &f = g_fds[fd];
	f.dev = d;
	f.devName = dev;
	f.path = p;
	f.handle = h;
	f.isDir = true;
	return fd;
}
int sceIoDread(int fd, u32 direntPtr) {
	OpenFile *f = GetFd(fd);
	if (!f || !f->isDir) return (int)SCE_KERNEL_ERROR_BADF;
	io::PspIoDirent e{};
	int r = f->dev->DRead(f->handle, e);
	if (r <= 0) return r;
	if (mem::IsValidRange(direntPtr, sizeof(io::PspIoStat) + 256)) {
		// Keep the guest's d_private pointer: write stat + name only.
		mem::WriteBytes(direntPtr, &e, sizeof(io::PspIoStat) + 256);
	}
	return 1;
}
int sceIoDclose(int fd) { return sceIoClose(fd); }

int sceIoIoctl(int fd, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	return f->dev->Ioctl(f->handle, cmd, in, inLen, out, outLen);
}
int sceIoIoctlAsync(int fd, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
	OpenFile *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	SetAsync(f, sceIoIoctl(fd, cmd, in, inLen, out, outLen));
	return 0;
}
int sceIoDevctl(const char *name, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
	if (!name) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	std::string n = name;
	size_t colon = n.find(':');
	std::string dev = colon == std::string::npos ? n : n.substr(0, colon);
	auto d = io::MountedDevice(dev);
	if (!d) return (int)SCE_KERNEL_ERROR_NODEV;
	return d->Devctl(dev, cmd, in, inLen, out, outLen);
}
u32 sceIoGetDevType(int fd) {
	if (fd <= kStderr) return 0x01;  // character device
	OpenFile *f = GetFd(fd);
	if (!f) return SCE_KERNEL_ERROR_BADF;
	return f->dev->DevType();
}
int sceIoAssign(const char *alias, const char *physical, const char *fs, int mode, u32 arg, u32 argSize) {
	(void)physical;
	(void)fs;
	(void)mode;
	(void)arg;
	(void)argSize;
	LOGD("IO", "sceIoAssign(%s) ignored", alias ? alias : "");
	return 0;
}
int sceIoUnassign(const char *alias) {
	(void)alias;
	return 0;
}
int sceIoSync(const char *dev, u32 flag) {
	(void)dev;
	(void)flag;
	return 0;
}
int sceIoGetFdList(u32 outPtr, int outSize, u32 fdNumPtr) {
	int n = 0;
	for (int fd = 0; fd < kMaxFds; fd++) {
		bool used = fd <= kStderr || g_fds.count(fd);
		if (!used) continue;
		if (n < outSize && mem::IsValidRange(outPtr + n * 4, 4)) GWrite32(outPtr + n * 4, fd);
		n++;
	}
	if (mem::IsValidRange(fdNumPtr, 4)) GWrite32(fdNumPtr, n);
	return n < outSize ? n : outSize;
}

int sceKernelStdin() { return kStdin; }
int sceKernelStdout() { return kStdout; }
int sceKernelStderr() { return kStderr; }
int sceKernelRegisterStdoutPipe(u32) { return 0; }
int sceKernelRegisterStderrPipe(u32) { return 0; }

}  // namespace

void Register_IoFileMgrForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0xB29DDF9C, sceIoDopen),
		HLEFN(0xE3EB004C, sceIoDread),
		HLEFN(0xEB092469, sceIoDclose),
		HLEFN(0xE95A012B, sceIoIoctlAsync),
		HLEFN(0x63632449, sceIoIoctl),
		HLEFN(0xACE946E8, sceIoGetstat),
		HLEFN(0xB8A740F4, sceIoChstat),
		HLEFN(0x55F4717D, sceIoChdir),
		HLEFN(0x08BD7374, sceIoGetDevType),
		HLEFN(0xB2A628C1, sceIoAssign),
		HLEFN(0xE8BC6571, sceIoCancel),
		HLEFN(0xB293727F, sceIoChangeAsyncPriority),
		HLEFN(0x810C4BC3, sceIoClose),
		HLEFN(0xFF5940B6, sceIoCloseAsync),
		HLEFN(0x54F5FB11, sceIoDevctl),
		HLEFN(0xCB05F8D6, sceIoGetAsyncStat),
		HLEFN(0x27EB27B8, sceIoLseek),
		HLEFN(0x68963324, sceIoLseek32),
		HLEFN(0x1B385D8F, sceIoLseek32Async),
		HLEFN(0x71B19E77, sceIoLseekAsync),
		HLEFN(0x109F50BC, sceIoOpen),
		HLEFN(0x89AA9906, sceIoOpenAsync),
		HLEFN(0x06A70004, sceIoMkdir),
		HLEFN(0x3251EA56, sceIoPollAsync),
		HLEFN(0x6A638D83, sceIoRead),
		HLEFN(0xA0B5A7C2, sceIoReadAsync),
		HLEFN(0xF27A9C51, sceIoRemove),
		HLEFN(0x779103A0, sceIoRename),
		HLEFN(0x1117C65F, sceIoRmdir),
		HLEFN(0xA12A0514, sceIoSetAsyncCallback),
		HLEFN(0xAB96437F, sceIoSync),
		HLEFN(0x6D08A871, sceIoUnassign),
		HLEFN(0x42EC03AC, sceIoWrite),
		HLEFN(0x0FACAB19, sceIoWriteAsync),
		HLEFN(0x35DBD746, sceIoWaitAsyncCB),
		HLEFN(0xE23EEC33, sceIoWaitAsync),
		HLEFN(0x5C2BE2CC, sceIoGetFdList),
	};
	hle::RegisterModule("IoFileMgrForUser", funcs);
	hle::RegisterModule("IoFileMgrForKernel", funcs);
}

void Register_StdioForUser() {
	static const hle::HleFunction funcs[] = {
		HLEFN(0x172D316E, sceKernelStdin),
		HLEFN(0xA6BAB2E9, sceKernelStdout),
		HLEFN(0xF78BA90A, sceKernelStderr),
		HLEFN(0x432D8F5C, sceKernelRegisterStdoutPipe),
		HLEFN(0x6F797E03, sceKernelRegisterStderrPipe),
	};
	hle::RegisterModule("StdioForUser", funcs);
}
