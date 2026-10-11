// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/HLE/sceIo.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later;
// descriptor and path rules follow uOFW's iofilemgr reimplementation (MIT).
//
// IoFileMgrForUser/ForKernel and StdioForUser. Files and directories share one descriptor table
// (3..63). Calls that make the real drivers block wait for about as long here (the delays come
// from PPSSPP's measurements), which is what lets other threads run in between (pspautotests
// checkpoints show it as [r]). Async operations run when issued and deliver their result after
// the same delay, through sceIoWaitAsync/PollAsync/GetAsyncStat and the fd's callback.
#include <algorithm>
#include <cstring>
#include <memory>

#include "common/log.h"
#include "fs/vfs.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/modules/modules.h"
#include "runtime/io/io.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/media/disc.h"

using namespace kernel;

namespace {

constexpr int kStdin = 0, kStdout = 1, kStderr = 2, kFirstFd = 3, kMaxFds = 64;
constexpr u32 kDirentSize = 352, kStatSize = 88;

struct DirEntry {
	vfs::SceIoDirent ent;
	std::string shortName;
};

struct AsyncWaiter {
	SceUID thread;
	u32 resultAddr;
};

struct FileNode {
	std::shared_ptr<io::Device> dev;
	std::string device;  // canonical device name
	std::string path;    // guest path as given, for logs
	s32 handle = -1;     // vfs handle (files only)
	u32 flags = 0;       // guest open flags
	bool isDir = false;
	bool isTty = false;
	bool sectorMode = false;

	std::vector<DirEntry> entries;
	size_t dirIndex = 0;

	bool pending = false;    // async operation in flight
	bool hasResult = false;  // result not collected yet
	bool closePending = false;
	s64 asyncResult = 0;
	SceUID asyncCb = 0;
	u32 asyncCbArg = 0;
	s32 asyncPrio = -1;
	u64 asyncEvent = 0;
	bool asyncThread = false;  // the fd's async thread exists (made by its first async call)
	std::vector<AsyncWaiter> waiters;

	vfs::FileSystem *Fs() const { return dev ? dev->Fs() : nullptr; }
	bool Busy() const { return pending || hasResult; }
};

std::unique_ptr<FileNode> g_fds[kMaxFds];
s32 g_defaultAsyncPrio = -1;

FileNode *GetFd(int fd) {
	if (fd < kFirstFd || fd >= kMaxFds) return nullptr;
	return g_fds[fd].get();
}

int AllocFd(std::unique_ptr<FileNode> node) {
	// The lowest free descriptor after the standard three.
	for (int fd = kFirstFd; fd < kMaxFds; fd++) {
		if (!g_fds[fd]) {
			g_fds[fd] = std::move(node);
			return fd;
		}
	}
	return (int)SCE_KERNEL_ERROR_MFILE;
}

void CloseHandle(FileNode *f) {
	if (f->handle >= 0 && f->Fs()) f->Fs()->Close(f->handle);
	f->handle = -1;
}

void FreeFd(int fd) {
	FileNode *f = GetFd(fd);
	if (!f) return;
	if (f->asyncEvent) sched::CancelEvent(f->asyncEvent);
	for (const AsyncWaiter &w : f->waiters) {
		PspThread *t = Get<PspThread>(w.thread);
		if (t && t->waitType == WAITTYPE_ASYNCIO && t->waitId == fd) sched::Wake(t, (s32)SCE_KERNEL_ERROR_WAIT_DELETE);
	}
	CloseHandle(f);
	g_fds[fd].reset();
}

// Blocks the caller for `us`, as the real driver would, then returns `result`.
int Delay(int result, u32 us) {
	if (us && sched::Current() && sched::DispatchEnabled() && !sched::InInterrupt()) {
		sched::WaitUntil(WAITTYPE_HLEDELAY, sched::CurrentId(), sched::NowUs() + us, false);
	}
	return result;
}

u32 SdkVersion() {
	// The compiled SDK version lives in SysMemUserForUser; ask it through its HLE entry point.
	const hle::HleFunction *fn = hle::FindFunction("SysMemUserForUser", 0xFC114573);
	if (!fn || !fn->fn) return 0;
	alignas(64) Ctx c{};
	fn->fn(&c);
	return c.r[2];
}

// With dispatch suspended the drivers fail when they try to wait: the memory stick with
// CAN_NOT_WAIT, usbhostfs (host0:) with -1.
int DispatchDisabledError(const std::string &device) {
	return device == "host0" ? -1 : (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
}

bool IsTtyPath(const char *path) {
	while (*path == ' ') path++;
	if (_strnicmp(path, "tty", 3) != 0) return false;
	const char *p = path + 3;
	while (*p >= '0' && *p <= '9') p++;
	return *p == ':' && p > path + 3;
}

bool HasWildcard(const char *s) { return s && strpbrk(s, "*?") != nullptr; }

void WriteResult(u32 addr, s64 v) {
	if ((addr & 3) == 0 && mem::IsValidRange(addr, 8)) GWrite<s64>(addr, v);
}

// ---------------------------------------------------------------- async completion

void CompleteAsync(int fd) {
	FileNode *f = GetFd(fd);
	if (!f) return;
	f->asyncEvent = 0;
	f->pending = false;
	f->hasResult = true;
	if (f->asyncCb) NotifyCallback(f->asyncCb, (s32)f->asyncCbArg);
	while (!f->waiters.empty()) {
		AsyncWaiter w = f->waiters.front();
		f->waiters.erase(f->waiters.begin());
		PspThread *t = Get<PspThread>(w.thread);
		if (!t || t->waitType != WAITTYPE_ASYNCIO || t->waitId != fd || t->waitDone) continue;
		WriteResult(w.resultAddr, f->asyncResult);
		f->hasResult = false;
		sched::Wake(t, 0);
		if (f->closePending) FreeFd(fd);
		break;
	}
}

void StartAsync(int fd, s64 result, u32 us) {
	FileNode *f = GetFd(fd);
	// The first async call creates the fd's I/O thread, and creating a thread resumes dispatch
	// unless interrupts are off (threads/scheduling/dispatch).
	if (!f->asyncThread) {
		f->asyncThread = true;
		if (*g_intrEnabled) sched::SetDispatchEnabled(true);
	}
	f->pending = true;
	f->hasResult = false;
	f->asyncResult = result;
	f->asyncEvent = sched::ScheduleEvent(sched::NowUs() + us, [fd]() { CompleteAsync(fd); });
}

int CollectAsync(int fd, u32 resultAddr) {
	FileNode *f = GetFd(fd);
	WriteResult(resultAddr, f->asyncResult);
	f->hasResult = false;
	if (f->closePending) FreeFd(fd);
	return 0;
}

int WaitAsync(int fd, u32 resultAddr, bool cb) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (cb) {
		PspThread *t = sched::Current();
		if (t && CanRunCallbacks(t)) RunPendingCallbacks(t);
		f = GetFd(fd);
		if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	}
	if (f->pending) {
		if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
		const SceUID uid = sched::CurrentId();
		f->waiters.push_back({uid, resultAddr});
		// A CB wait paused for callbacks leaves the queue; when they return, a result that
		// arrived meanwhile ends the wait.
		WaitHooks hooks;
		hooks.pause = [fd, uid]() {
			if (FileNode *n = GetFd(fd)) {
				auto &w = n->waiters;
				w.erase(std::remove_if(w.begin(), w.end(), [uid](const AsyncWaiter &x) { return x.thread == uid; }), w.end());
			}
		};
		hooks.resume = [fd, uid, resultAddr]() {
			FileNode *n = GetFd(fd);
			PspThread *t = Get<PspThread>(uid);
			if (!n || !t) return false;
			if (!n->pending && n->hasResult) {
				CollectAsync(fd, resultAddr);
				sched::Wake(t, 0);
				return true;
			}
			n->waiters.push_back({uid, resultAddr});
			return false;
		};
		return sched::Wait(WAITTYPE_ASYNCIO, fd, 0, cb, cb ? &hooks : nullptr);
	}
	if (f->hasResult) {
		if (!cb && !sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
		return CollectAsync(fd, resultAddr);
	}
	return (int)SCE_KERNEL_ERROR_NOASYNC;
}

// ---------------------------------------------------------------- open

// Opens `path` into `*out`. Returns 0 or an error, and in *delayUs how long the driver takes.
int DoOpen(const char *path, u32 flags, s32 mode, std::unique_ptr<FileNode> *out, u32 *delayUs) {
	*delayUs = 0;
	auto node = std::make_unique<FileNode>();
	node->path = path;
	node->flags = flags & ~vfs::PSPC_O_SECTOR_MODE;
	if (IsTtyPath(path)) {
		node->isTty = true;
		*out = std::move(node);
		return 0;
	}
	io::Resolved r;
	int err = io::Resolve(path, &r);
	if (err == (int)SCE_KERNEL_ERROR_NOCWD) {
		*delayUs = 10000;
		return err;
	}
	if (err < 0) return err;
	vfs::FileSystem *fs = r.dev->Fs();
	if (!fs) {
		*delayUs = 10000;
		return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	}
	const bool umd = r.dev->IsUmd();
	std::string p = r.path;
	u32 fsFlags = node->flags;
	if (r.dev->Class() == io::DevClass::UmdBlock) {
		// Everything opened on umd0: is the disc itself, in sectors (raw extents stay raw).
		fsFlags |= vfs::PSPC_O_SECTOR_MODE;
		u32 a, b;
		std::vector<std::string> comps = vfs::SplitPspPathComponents(p);
		if (!(comps.size() == 1 && vfs::ParseSceLbnPath("/" + comps[0], &a, &b))) p.clear();
	}
	if (r.dev->IsFat() && strpbrk(p.c_str(), "?*<>|\"")) {
		// Characters FAT can't store fail before the file is looked for.
		*delayUs = 10000;
		return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	}
	s32 h = fs->Open(p, fsFlags, mode);
	if (h < 0) {
		// UMD: 5-10 ms. Memory stick: 10-13 ms on a Pro Duo, depending on the path depth.
		*delayUs = h == (s32)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND && umd ? 6000 : 10000;
		return h;
	}
	vfs::HandleInfo info;
	fs->GetHandleInfo(h, &info);
	node->dev = r.dev;
	node->device = r.device;
	node->handle = h;
	node->sectorMode = info.sectorMode;
	// Block devices open without waiting at all.
	*delayUs = (info.sectorMode || info.rawLbn) ? 0 : (umd ? 4000 : 10000);
	*out = std::move(node);
	return 0;
}

int sceIoOpen(const char *path, int flags, int mode) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (!sched::DispatchEnabled()) {
		io::Resolved r;
		return DispatchDisabledError(io::Resolve(path, &r) == 0 ? r.device : std::string());
	}
	std::unique_ptr<FileNode> node;
	u32 us;
	int err = DoOpen(path, (u32)flags, mode, &node, &us);
	if (err < 0) {
		LOGD("IO", "sceIoOpen(%s, %x) -> %08x", path, flags, err);
		return Delay(err, us);
	}
	FileNode *raw = node.get();
	int fd = AllocFd(std::move(node));
	if (fd < 0) {
		CloseHandle(raw);
		return Delay(fd, 1000);
	}
	g_fds[fd]->asyncPrio = g_defaultAsyncPrio;
	LOGD("IO", "sceIoOpen(%s, %x) -> fd %d", path, flags, fd);
	return Delay(fd, us);
}

int sceIoOpenAsync(const char *path, int flags, int mode) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	std::unique_ptr<FileNode> node;
	u32 us;
	int err = DoOpen(path, (u32)flags, mode, &node, &us);
	if (err == (int)SCE_KERNEL_ERROR_NODEV) return err;
	if (err < 0) {
		// Async opens hand out a descriptor even when they fail; the error is the async result.
		node = std::make_unique<FileNode>();
		node->path = path;
		node->closePending = true;
	}
	FileNode *raw = node.get();
	int fd = AllocFd(std::move(node));
	if (fd < 0) {
		CloseHandle(raw);
		return Delay(fd, 1000);
	}
	g_fds[fd]->asyncPrio = g_defaultAsyncPrio;
	StartAsync(fd, err < 0 ? err : fd, err < 0 ? us : (us ? us : 80));
	return fd;
}

// ---------------------------------------------------------------- read / write / seek / close

int DoRead(FileNode *f, u32 buf, int size) {
	if (f->isDir || !(f->flags & vfs::PSP_O_RDONLY)) return (int)SCE_KERNEL_ERROR_BADF;
	if (size < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (f->isTty || size == 0) return 0;
	// Sector-mode handles (umd0:) count sectors.
	const u64 bytes = f->sectorMode ? (u64)size * vfs::kSectorSize : (u64)size;
	if (bytes > 0xFFFFFFFFull || !mem::IsValidRange(buf, (u32)bytes)) return -1;
	return (int)f->Fs()->Read(f->handle, GPtr(buf), (u64)size);
}

int DoWrite(FileNode *f, u32 buf, int size) {
	if (f->isDir || !(f->flags & vfs::PSP_O_WRONLY)) return (int)SCE_KERNEL_ERROR_BADF;
	if (size < 0) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (size == 0) return 0;
	if (!mem::IsValidRange(buf, (u32)size)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (f->isTty) {
		// The console (PSPLink's shell on hardware), not the program's stdout (io/open/tty0).
		LOGI("TTY", "%.*s", size, (const char *)GPtr(buf));
		return size;
	}
	return (int)f->Fs()->Write(f->handle, GPtr(buf), (u64)size);
}

s64 DoSeek(FileNode *f, s64 offset, int whence) {
	if (whence < 0 || whence > 2) return (s32)SCE_KERNEL_ERROR_INVAL;
	if (f->isDir) return (s32)SCE_KERNEL_ERROR_BADF;
	if (f->isTty) return 0;
	return f->Fs()->Seek(f->handle, offset, whence);
}

u32 RwDelayUs(int size) { return size / 100 > 100 ? (u32)(size / 100) : 100; }

int sceIoRead(int fd, u32 buf, int size) {
	if (fd == kStdin) return 0;
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	if (!sched::DispatchEnabled()) return DispatchDisabledError(f->device);
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	int r = DoRead(f, buf, size);
	return r >= 0 ? Delay(r, RwDelayUs(size)) : r;
}

int sceIoWrite(int fd, u32 buf, int size) {
	if (fd == kStdout || fd == kStderr) {
		// Console output doesn't give up the CPU (threads/scheduling/dispatch).
		if (size > 0 && mem::IsValidRange(buf, (u32)size)) logging::GuestOutput((const char *)GPtr(buf), (size_t)size);
		return size;
	}
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	if (!sched::DispatchEnabled()) return DispatchDisabledError(f->device);
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	int r = DoWrite(f, buf, size);
	return r >= 0 ? Delay(r, RwDelayUs(size)) : r;
}

s64 sceIoLseek(int fd, s64 offset, int whence) {
	FileNode *f = GetFd(fd);
	if (!f) return (s32)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (s32)SCE_KERNEL_ERROR_ASYNC_BUSY;
	s64 r = DoSeek(f, offset, whence);
	// On host0: every call is a USB round trip that waits, so a thread of the same priority runs
	// meanwhile (font/openfile); elsewhere a seek only gives way to a higher priority.
	if (f->device == "host0" && !f->isTty) Delay(0, 100);
	else sched::Reschedule();
	return r;
}

int sceIoLseek32(int fd, int offset, int whence) { return (int)sceIoLseek(fd, offset, whence); }

int sceIoClose(int fd) {
	if (fd >= 0 && fd < kFirstFd) return (int)SCE_KERNEL_ERROR_ILLEGAL_PERM;
	FileNode *f = GetFd(fd);
	if (!f || f->isDir) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->pending) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	FreeFd(fd);
	return Delay(0, 100);
}

int sceIoReadAsync(int fd, u32 buf, int size) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	StartAsync(fd, DoRead(f, buf, size), RwDelayUs(size));
	return 0;
}

int sceIoWriteAsync(int fd, u32 buf, int size) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	StartAsync(fd, DoWrite(f, buf, size), RwDelayUs(size));
	return 0;
}

int sceIoLseekAsync(int fd, s64 offset, int whence) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (whence < 0 || whence > 2) return (int)SCE_KERNEL_ERROR_INVAL;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	StartAsync(fd, DoSeek(f, offset, whence), 100);
	return 0;
}

int sceIoLseek32Async(int fd, int offset, int whence) { return sceIoLseekAsync(fd, offset, whence); }

int sceIoCloseAsync(int fd) {
	FileNode *f = GetFd(fd);
	if (!f || f->isDir) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	CloseHandle(f);
	f->closePending = true;
	// No delay for a close (PPSSPP #12549); the descriptor goes once the result is collected.
	StartAsync(fd, 0, 0);
	return 0;
}

int sceIoWaitAsync(int fd, u32 resultAddr) { return WaitAsync(fd, resultAddr, false); }
int sceIoWaitAsyncCB(int fd, u32 resultAddr) { return WaitAsync(fd, resultAddr, true); }

int sceIoPollAsync(int fd, u32 resultAddr) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->pending) return 1;
	if (f->hasResult) return CollectAsync(fd, resultAddr);
	return (int)SCE_KERNEL_ERROR_NOASYNC;
}

int sceIoGetAsyncStat(int fd, int poll, u32 resultAddr) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (sched::InInterrupt()) return (int)SCE_KERNEL_ERROR_ILLEGAL_CONTEXT;
	if (f->pending) {
		if (poll) return 1;
		if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
		f->waiters.push_back({sched::CurrentId(), resultAddr});
		return sched::Wait(WAITTYPE_ASYNCIO, fd, 0, false);
	}
	if (f->hasResult) {
		if (!sched::DispatchEnabled()) return (int)SCE_KERNEL_ERROR_CAN_NOT_WAIT;
		return CollectAsync(fd, resultAddr);
	}
	return (int)SCE_KERNEL_ERROR_NOASYNC;
}

int sceIoChangeAsyncPriority(int fd, int prio) {
	if (prio != -1 && (prio < 0x08 || prio > 0x77)) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
	if (fd == -1) {
		g_defaultAsyncPrio = prio;
		return 0;
	}
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	f->asyncPrio = prio;
	return 0;
}

int sceIoSetAsyncCallback(int fd, SceUID cb, u32 arg) {
	if (!Get<Callback>(cb)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	f->asyncCb = cb;
	f->asyncCbArg = arg;
	return 0;
}

int sceIoCancel(int fd) {
	// Neither the UMD nor the memory stick driver supports it.
	return GetFd(fd) ? (int)SCE_KERNEL_ERROR_UNSUP : (int)SCE_KERNEL_ERROR_BADF;
}

// ---------------------------------------------------------------- ioctl / devctl

int DoIoctl(FileNode *f, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, u32 *us) {
	*us = 100;
	if (f->isDir || f->isTty || !f->Fs()) return (int)SCE_KERNEL_ERROR_UNSUP;
	vfs::FileSystem *fs = f->Fs();
	switch (cmd) {
	case 0x04100001: {  // PGD key setup (sceNpDrm). Plain files proceed; encrypted ones are unsupported.
		u8 magic[4] = {};
		s64 pos = fs->Seek(f->handle, 0, vfs::PSP_SEEK_CUR);
		fs->Read(f->handle, magic, 4);
		fs->Seek(f->handle, pos, vfs::PSP_SEEK_SET);
		if (!memcmp(magic, "\0PGD", 4)) {
			LOGW("IO", "%s is PGD-encrypted; decryption is not supported", f->path.c_str());
			return (int)SCE_KERNEL_ERROR_UNSUP;
		}
		return 0;
	}
	case 0x04100002:  // PGD offset
		return 0;
	case 0x04100010: {  // PGD data size: the plain size here
		vfs::HandleInfo info;
		fs->GetHandleInfo(f->handle, &info);
		return (int)info.size;
	}
	case 0x01F100A6:
		*us += 15000;  // sector seek; Pangya Portable needs the time
		break;
	}
	// The firmware reads a 16-byte struct for the seek ioctls even when inLen is 4.
	u32 inAvail = inLen;
	if ((cmd == 0x01010005 || cmd == 0x01F100A6) && inLen >= 4 && mem::IsValidRange(in, 16)) inAvail = 16;
	const void *inPtr = (inAvail && mem::IsValidRange(in, inAvail)) ? GPtr(in) : nullptr;
	void *outPtr = (outLen && mem::IsValidRange(out, outLen)) ? GPtr(out) : nullptr;
	int r = fs->Ioctl(f->handle, cmd, inPtr, inPtr ? inAvail : 0, outPtr, outPtr ? outLen : 0);
	if (r == (int)vfs::SCE_ERRNO_FUNCTION_NOT_SUPPORTED) {
		LOGW("IO", "Unsupported ioctl %08x on %s", cmd, f->path.c_str());
	}
	return r;
}

int sceIoIoctl(int fd, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	u32 us;
	int r = DoIoctl(f, cmd, in, inLen, out, outLen, &us);
	return Delay(r, us);
}

int sceIoIoctlAsync(int fd, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
	FileNode *f = GetFd(fd);
	if (!f) return (int)SCE_KERNEL_ERROR_BADF;
	if (f->Busy()) return (int)SCE_KERNEL_ERROR_ASYNC_BUSY;
	u32 us;
	int r = DoIoctl(f, cmd, in, inLen, out, outLen, &us);
	StartAsync(fd, r, us);
	return 0;
}

// UMD drive commands answered for any device name, as the UMD driver stack does.
bool UmdDevctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, int *result, u32 *us) {
	auto badParams = [&]() { *result = (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS; };
	switch (cmd) {
	case 0x01E18030:  // region check: 1 = matches (the answer is the return value)
		*result = inLen >= 16 ? 1 : -1;
		return true;
	case 0x01F20001:  // disc type
		if ((out & 3) == 0 && mem::IsValidRange(out, 8) && outLen >= 8) {
			GWrite32(out + 4, media::DiscType());  // 0x10 game, 0x20 UMD Video, 0x40 UMD Audio
			*result = 0;
		} else {
			badParams();
		}
		return true;
	case 0x01F20002:  // current LBA
		if ((out & 3) == 0 && mem::IsValidRange(out, 4) && outLen >= 4) {
			GWrite32(out, 0x10);
			*result = 0;
		} else {
			badParams();
		}
		return true;
	case 0x01F20003: {  // last sector
		if ((in & 3) == 0 && mem::IsValidRange(in, 4) && inLen >= 4 && mem::IsValidRange(out, 4)) {
			auto dev = io::MountedDevice("umd0");
			vfs::HandleInfo info{};
			if (dev && dev->Fs()) {
				s32 h = dev->Fs()->Open("", vfs::PSP_O_RDONLY, 0);
				if (h >= 0) {
					dev->Fs()->GetHandleInfo(h, &info);
					dev->Fs()->Close(h);
				}
			}
			GWrite32(out, (u32)info.size - 1);
			*result = 0;
		} else {
			badParams();
		}
		return true;
	}
	case 0x01F100A3:  // seek (raw)
		if (mem::IsValidAddress(in) && inLen >= 4) {
			*us = 100;
			*result = 0;
		} else {
			badParams();
		}
		return true;
	case 0x01F100A4:  // prepare data into the cache
	case 0x01F300A7:  // wait for the cache thread
	case 0x01F300A8:  // poll it (0 = finished)
	case 0x01F300A9:  // cancel it
		if (mem::IsValidAddress(in) && inLen >= 4) *result = 0;
		else badParams();
		return true;
	case 0x01F300A5:  // prepare into the cache and get the status
		if (!mem::IsValidAddress(in) || inLen < 4) {
			badParams();
		} else if ((out & 3) || !mem::IsValidRange(out, 4)) {
			badParams();
		} else {
			GWrite32(out, 1);
			*result = 0;
		}
		return true;
	case 0x01F100A6:
	case 0x01F100A8:
	case 0x01F100A9:
		*result = 0;
		return true;
	}
	return false;
}

int sceIoDevctl(const char *name, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
	if (!name) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	int result;
	u32 us = 0;
	if (UmdDevctl(cmd, in, inLen, out, outLen, &result, &us)) return Delay(result, us);
	std::string dev = name;
	size_t colon = dev.find(':');
	if (colon != std::string::npos) dev.resize(colon);
	while (!dev.empty() && dev[0] == ' ') dev.erase(0, 1);
	auto d = io::MountedDevice(dev);
	if (!d) return (int)SCE_KERNEL_ERROR_NODEV;
	for (char &c : dev) c = (char)tolower((unsigned char)c);
	result = d->Devctl(dev, cmd, in, inLen, out, outLen, &us);
	return Delay(result, us);
}

u32 sceIoGetDevType(int fd) {
	if (fd >= 0 && fd < kFirstFd) return 0x10;
	FileNode *f = GetFd(fd);
	if (!f) return SCE_KERNEL_ERROR_BADF;
	if (f->isTty) return 0x10;
	return f->dev->DevType() & 0xFF;
}

// ---------------------------------------------------------------- paths

// A stat of the volume itself ("ms0:" or "ms0:/") is refused (io/stat).
bool IsVolumeRoot(const std::string &path) { return path.find_first_not_of('/') == std::string::npos; }

int ResolveFs(const char *path, io::Resolved *r) {
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	int err = io::Resolve(path, r);
	if (err < 0) return err;
	if (!r->dev->Fs()) return (int)SCE_KERNEL_ERROR_UNSUP;
	return 0;
}

// How much of a SceIoStat the drivers write: mode..mtime, plus st_private[0] (the LBN) on a UMD.
u32 StatWriteSize(const io::Device &dev) { return dev.IsUmd() ? 68 : 64; }

int sceIoGetstat(const char *path, u32 statAddr) {
	io::Resolved r;
	int err = ResolveFs(path, &r);
	if (err < 0) return err == (int)SCE_KERNEL_ERROR_NOCWD ? Delay(err, 1000) : err;
	if (IsVolumeRoot(r.path)) return Delay((int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, 1000);
	if (!mem::IsValidRange(statAddr, kStatSize)) return Delay((int)SCE_KERNEL_ERROR_ILLEGAL_ADDR, 1000);
	vfs::SceIoStat st;
	mem::ReadBytes(&st, statAddr, kStatSize);
	int res = r.dev->Fs()->GetStat(r.path, &st);
	if (res >= 0) mem::WriteBytes(statAddr, &st, StatWriteSize(*r.dev));
	return Delay(res, 1000);
}

int sceIoChstat(const char *path, u32 statAddr, u32 bits) {
	io::Resolved r;
	int err = ResolveFs(path, &r);
	if (err < 0) return err;
	if (!mem::IsValidRange(statAddr, kStatSize)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	vfs::SceIoStat st;
	mem::ReadBytes(&st, statAddr, kStatSize);
	return r.dev->Fs()->ChStat(r.path, st, bits);
}

int sceIoRemove(const char *path) {
	io::Resolved r;
	int err = ResolveFs(path, &r);
	if (err < 0) return err;
	return Delay(r.dev->Fs()->Remove(r.path), 100);
}

int sceIoMkdir(const char *path, int mode) {
	io::Resolved r;
	int err = ResolveFs(path, &r);
	if (err < 0) return err;
	return Delay(r.dev->Fs()->Mkdir(r.path, mode), 1000);
}

int sceIoRmdir(const char *path) {
	io::Resolved r;
	int err = ResolveFs(path, &r);
	if (err < 0) return err;
	return Delay(r.dev->Fs()->Rmdir(r.path), 1000);
}

int sceIoRename(const char *from, const char *to) {
	if (!from || !to) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	// Wildcards are refused in either name, before anything is looked up.
	if (HasWildcard(from) || HasWildcard(to)) return Delay((int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, 1000);
	io::Resolved src;
	int err = ResolveFs(from, &src);
	if (err < 0) return err;
	std::string target;
	if (strchr(to, ':')) {
		io::Resolved dst;
		err = ResolveFs(to, &dst);
		if (err < 0) return err;
		// Across devices fails up front, without the wait the other errors take.
		if (dst.dev != src.dev) return (int)SCE_KERNEL_ERROR_XDEV;
		target = dst.path;
	} else {
		// A name without a device is not resolved against the cwd: the file is renamed in
		// its own directory (io/file/rename).
		std::vector<std::string> comps = vfs::SplitPspPathComponents(src.path);
		std::vector<std::string> name = vfs::SplitPspPathComponents(to);
		if (comps.empty() || name.empty()) return Delay((int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, 1000);
		comps.back() = name.back();
		for (const std::string &c : comps) target += "/" + c;
	}
	return Delay(src.dev->Fs()->Rename(src.path, target), 1000);
}

int sceIoChdir(const char *path) {
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (strlen(path) >= 1023) return (int)SCE_KERNEL_ERROR_NAMETOOLONG;
	io::Resolved r;
	int err = io::Resolve(path, &r);
	if (err < 0) return err;
	// The directory need not exist (io/io).
	io::SetThreadCwd(sched::CurrentId(), r.device + ":" + r.path);
	return 0;
}

// ---------------------------------------------------------------- directories

int sceIoDopen(const char *path) {
	if (!path) return (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
	io::Resolved r;
	int err = ResolveFs(path, &r);
	if (err < 0) return err;
	io::OnDirectoryOpen(r.device, r.path);
	vfs::FileSystem *fs = r.dev->Fs();
	s32 dd = fs->DOpen(r.path);
	if (dd < 0) return dd;
	auto node = std::make_unique<FileNode>();
	node->dev = r.dev;
	node->device = r.device;
	node->path = path;
	node->isDir = true;
	for (;;) {
		DirEntry e{};
		if (fs->DRead(dd, &e.ent) <= 0) break;
		node->entries.push_back(e);
	}
	fs->DClose(dd);
	if (r.dev->IsFat()) {
		std::vector<std::string> names;
		for (const DirEntry &e : node->entries) names.push_back(e.ent.d_name);
		std::vector<std::string> shorts = vfs::GenerateFatShortNames(names);
		for (size_t i = 0; i < shorts.size(); i++) node->entries[i].shortName = shorts[i];
	}
	return AllocFd(std::move(node));
}

// strncpy without the padding: copies at most limit - 1 characters and always terminates.
void CopyLimited(u32 dst, const char *src, u32 limit) {
	u32 n = 0;
	while (n + 1 < limit && src[n]) n++;
	if (!mem::IsValidRange(dst, n + 1)) return;
	mem::WriteBytes(dst, src, n);
	GWrite<u8>(dst + n, 0);
}

int sceIoDread(int fd, u32 direntAddr) {
	FileNode *f = GetFd(fd);
	if (!f || !f->isDir) return (int)SCE_KERNEL_ERROR_BADF;
	if (!mem::IsValidRange(direntAddr, kDirentSize)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	if (f->dirIndex >= f->entries.size()) {
		GWrite<u8>(direntAddr + 88, 0);
		return 0;
	}
	const DirEntry &e = f->entries[f->dirIndex++];
	mem::WriteBytes(direntAddr, &e.ent.d_stat, StatWriteSize(*f->dev));
	mem::WriteBytes(direntAddr + 88, e.ent.d_name, sizeof(e.ent.d_name));
	if (f->dev->IsFat()) {
		// d_private gets the 8.3 name and the long name, in a layout that depends on the SDK.
		u32 priv = GRead32(direntAddr + 344);
		if (priv && mem::IsValidRange(priv, 4)) {
			if (SdkVersion() <= 0x0307FFFF) {
				CopyLimited(priv, e.shortName.c_str(), 13);
				CopyLimited(priv + 13, e.ent.d_name, 256);
			} else if (GRead32(priv) >= 1044) {
				CopyLimited(priv + 4, e.shortName.c_str(), 16);
				CopyLimited(priv + 20, e.ent.d_name, 256);
			}
		}
	}
	// Only the first entry costs time, on both the memory stick and the UMD.
	return f->dirIndex == 1 ? Delay(1, 1000) : 1;
}

int sceIoDclose(int fd) {
	FileNode *f = GetFd(fd);
	if (!f || !f->isDir) return (int)SCE_KERNEL_ERROR_BADF;
	FreeFd(fd);
	return 0;
}

// ---------------------------------------------------------------- misc

int sceIoAssign(const char *alias, const char *physical, const char *fs, int mode, u32 arg, u32 argSize) {
	if (!alias || !physical || !fs) return -1;
	LOGD("IO", "sceIoAssign(%s, %s, %s, %d) ignored", alias, physical, fs, mode);
	return 0;
}

int sceIoUnassign(const char *alias) { return 0; }
int sceIoSync(const char *dev, u32 flag) { return 0; }

int sceIoGetFdList(u32 outPtr, int outSize, u32 countPtr) {
	int count = 0;
	auto put = [&](int fd) {
		if (count < outSize && mem::IsValidRange(outPtr + count * 4, 4)) GWrite32(outPtr + count * 4, (u32)fd);
		count++;
	};
	for (int fd = 0; fd < kFirstFd; fd++) put(fd);
	for (int fd = kFirstFd; fd < kMaxFds; fd++) {
		if (g_fds[fd]) put(fd);
	}
	if (mem::IsValidRange(countPtr, 4)) GWrite32(countPtr, (u32)count);
	return count >= outSize ? outSize : count;
}

int sceIoGetThreadCwd(SceUID thread, u32 buf, int len) {
	std::string cwd = io::ThreadCwd(thread ? thread : sched::CurrentId());
	if (cwd.empty()) return 0;
	int need = (int)cwd.size() + 1;
	if (len > 0 && mem::IsValidRange(buf, (u32)len)) CopyLimited(buf, cwd.c_str(), (u32)std::min(len, need));
	return need;
}

int sceIoChangeThreadCwd(SceUID thread, const char *path) {
	if (!path) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	io::Resolved r;
	int err = io::Resolve(path, &r);
	if (err < 0) return err;
	io::SetThreadCwd(thread, r.device + ":" + r.path);
	return 0;
}

int sceKernelStdin() { return kStdin; }
int sceKernelStdout() { return kStdout; }
int sceKernelStderr() { return kStderr; }
int sceKernelRegisterStdoutPipe(u32) { return 0; }
int sceKernelRegisterStderrPipe(u32) { return 0; }

}  // namespace

namespace io {

bool ReadRestOfFd(int fd, std::vector<u8> &out, std::string *path) {
	FileNode *f = fd >= 0 && fd < kMaxFds ? GetFd(fd) : nullptr;
	if (!f || f->isDir || f->isTty || !f->Fs()) return false;
	vfs::FileSystem *fs = f->Fs();
	s64 pos = fs->Seek(f->handle, 0, 1);
	s64 end = fs->Seek(f->handle, 0, 2);
	fs->Seek(f->handle, pos, 0);
	if (pos < 0 || end < pos) return false;
	out.resize((size_t)(end - pos));
	s64 n = out.empty() ? 0 : fs->Read(f->handle, out.data(), out.size());
	fs->Seek(f->handle, pos, 0);
	if (n < 0) return false;
	out.resize((size_t)n);
	if (path) *path = f->path;
	return true;
}

}  // namespace io

void Register_IoFileMgrForUser() {
	for (auto &f : g_fds) f.reset();
	g_defaultAsyncPrio = -1;
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

	static const hle::HleFunction kernelFuncs[] = {
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
		HLEFN(0x411106BA, sceIoGetThreadCwd),
		HLEFN(0xCB0A151F, sceIoChangeThreadCwd),
	};
	hle::RegisterModule("IoFileMgrForKernel", kernelFuncs);
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
