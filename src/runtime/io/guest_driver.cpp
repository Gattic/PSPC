// SPDX-License-Identifier: GPL-2.0-or-later
// sceIoAddDrv / sceIoDelDrv: devices whose driver is the game's own code. The PspIoDrv the game
// passes names the device and holds the table of its functions (pspsdk pspiofilemgr_kernel.h);
// each call runs on the calling thread through cpu::CallGuest, as iofilemgr calls a driver on a
// PSP (pspautotests io/iodrv).
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "runtime/core/cpu.h"
#include "runtime/core/memory.h"
#include "runtime/io/io.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/sysmem.h"

namespace io {

namespace {

// PspIoDrvFuncs, by index.
enum DrvFunc : u32 {
	IoInit, IoExit, IoOpen, IoClose, IoRead, IoWrite, IoLseek, IoIoctl, IoRemove, IoMkdir, IoRmdir, IoDopen,
	IoDclose, IoDread, IoGetstat, IoChstat, IoRename, IoChdir, IoMount, IoUmount, IoDevctl, IoUnk21,
	kDrvFuncs
};

constexpr u32 kFileArgSize = 20;  // PspIoDrvFileArg: unk1, fs_num, drv, unk2, arg
constexpr u32 kPathMax = 256;
// The driver's scratch block: PspIoDrvArg, two path strings, then a stat or dirent buffer.
constexpr u32 kDrvArgOff = 0, kPath1Off = 16, kPath2Off = kPath1Off + kPathMax, kBufOff = kPath2Off + kPathMax;
constexpr u32 kScratchSize = kBufOff + sizeof(vfs::SceIoDirent);

u32 GuestAddr(const void *p) { return p ? (u32)((const u8 *)p - g_membase) : 0; }

class GuestDriverFs : public vfs::FileSystem {
public:
	GuestDriverFs(u32 drv, u32 funcs, u32 scratch) : drv_(drv), funcs_(funcs), scratch_(scratch) {}

	u32 Func(DrvFunc f) const { return GRead32(funcs_ + 4 * f); }
	u32 DrvArg() const { return scratch_ + kDrvArgOff; }

	// Calls a driver function, or returns `none` if the driver has none (or no thread can run it).
	s32 Call(DrvFunc f, std::initializer_list<u32> args, s32 none = (s32)SCE_KERNEL_ERROR_UNSUP) {
		const u32 fn = Func(f);
		Ctx *c = cpu::CurrentCtx();
		if (!fn || !mem::IsValidAddress(fn) || !c) return none;
		return (s32)cpu::CallGuest(c, fn, args);
	}

	u32 PutPath(u32 off, std::string_view path) {
		const size_t n = std::min<size_t>(path.size(), kPathMax - 1);
		memcpy(GPtr(scratch_ + off), path.data(), n);
		GWrite8(scratch_ + off + (u32)n, 0);
		return scratch_ + off;
	}

	// A PspIoDrvFileArg for one call or one open handle.
	u32 NewFileArg() {
		const u32 a = kernel::g_kernelMem.Alloc(kFileArgSize, true, "IoDrvFileArg");
		if (!a) return 0;
		mem::Memset(a, 0, kFileArgSize);
		GWrite32(a + 8, DrvArg());
		return a;
	}
	void FreeFileArg(u32 a) {
		if (a) kernel::g_kernelMem.Free(a);
	}

	// One call with a file argument of its own (stat, remove, ...).
	template <typename F>
	s32 WithFileArg(F f) {
		const u32 a = NewFileArg();
		if (!a) return (s32)SCE_KERNEL_ERROR_NO_MEMORY;
		const s32 r = f(a);
		FreeFileArg(a);
		return r;
	}

	u32 Caps() const override { return 0; }

	s32 Open(std::string_view path, u32 flags, s32 mode) override {
		const u32 a = NewFileArg();
		if (!a) return (s32)SCE_KERNEL_ERROR_NO_MEMORY;
		const s32 r = Call(IoOpen, {a, PutPath(kPath1Off, path), flags & ~vfs::PSPC_O_SECTOR_MODE, (u32)mode});
		if (r < 0) {
			FreeFileArg(a);
			return r;
		}
		const s32 h = next_++;
		handles_[h] = Handle{a, flags, false};
		return h;
	}
	s32 Close(s32 fd) override {
		auto it = handles_.find(fd);
		if (it == handles_.end() || it->second.dir) return (s32)SCE_KERNEL_ERROR_BADF;
		const s32 r = Call(IoClose, {it->second.arg}, 0);
		FreeFileArg(it->second.arg);
		handles_.erase(it);
		return r;
	}
	s64 Read(s32 fd, void *buf, u64 size) override {
		const Handle *h = Get(fd);
		return h ? Call(IoRead, {h->arg, GuestAddr(buf), (u32)size}) : (s32)SCE_KERNEL_ERROR_BADF;
	}
	s64 Write(s32 fd, const void *buf, u64 size) override {
		const Handle *h = Get(fd);
		return h ? Call(IoWrite, {h->arg, GuestAddr(buf), (u32)size}) : (s32)SCE_KERNEL_ERROR_BADF;
	}
	s64 Seek(s32 fd, s64 offset, s32 whence) override {
		const Handle *h = Get(fd);
		if (!h) return (s32)SCE_KERNEL_ERROR_BADF;
		const u32 fn = Func(IoLseek);
		Ctx *c = cpu::CurrentCtx();
		if (!fn || !c) return (s32)SCE_KERNEL_ERROR_UNSUP;
		// The 64-bit offset goes in an aligned register pair (a2:a3), the result comes back in v0:v1.
		return (s64)cpu::CallGuest64(c, fn, {h->arg, 0, (u32)offset, (u32)((u64)offset >> 32), (u32)whence});
	}
	s32 GetHandleInfo(s32 fd, vfs::HandleInfo *out) override {
		const Handle *h = Get(fd);
		if (!h) return (s32)SCE_KERNEL_ERROR_BADF;
		*out = vfs::HandleInfo{};
		out->flags = h->flags;
		return 0;
	}
	s32 Ioctl(s32 fd, u32 cmd, const void *in, u32 inLen, void *out, u32 outLen) override {
		const Handle *h = Get(fd);
		if (!h) return (s32)SCE_KERNEL_ERROR_BADF;
		return Call(IoIoctl, {h->arg, cmd, GuestAddr(in), inLen, GuestAddr(out), outLen});
	}
	s32 GetStat(std::string_view path, vfs::SceIoStat *out) override {
		return WithFileArg([&](u32 a) {
			const u32 buf = scratch_ + kBufOff;
			mem::Memset(buf, 0, sizeof(vfs::SceIoStat));
			const s32 r = Call(IoGetstat, {a, PutPath(kPath1Off, path), buf});
			if (r >= 0) memcpy(out, GPtr(buf), sizeof(vfs::SceIoStat));
			return r;
		});
	}
	s32 ChStat(std::string_view path, const vfs::SceIoStat &in, u32 bits) override {
		return WithFileArg([&](u32 a) {
			const u32 buf = scratch_ + kBufOff;
			memcpy(GPtr(buf), &in, sizeof(vfs::SceIoStat));
			return Call(IoChstat, {a, PutPath(kPath1Off, path), buf, bits});
		});
	}
	s32 Remove(std::string_view path) override {
		return WithFileArg([&](u32 a) { return Call(IoRemove, {a, PutPath(kPath1Off, path)}); });
	}
	s32 Mkdir(std::string_view path, s32 mode) override {
		return WithFileArg([&](u32 a) { return Call(IoMkdir, {a, PutPath(kPath1Off, path), (u32)mode}); });
	}
	s32 Rmdir(std::string_view path) override {
		return WithFileArg([&](u32 a) { return Call(IoRmdir, {a, PutPath(kPath1Off, path)}); });
	}
	s32 Rename(std::string_view from, std::string_view to) override {
		return WithFileArg([&](u32 a) { return Call(IoRename, {a, PutPath(kPath1Off, from), PutPath(kPath2Off, to)}); });
	}
	s32 DOpen(std::string_view path) override {
		const u32 a = NewFileArg();
		if (!a) return (s32)SCE_KERNEL_ERROR_NO_MEMORY;
		const s32 r = Call(IoDopen, {a, PutPath(kPath1Off, path)});
		if (r < 0) {
			FreeFileArg(a);
			return r;
		}
		const s32 h = next_++;
		handles_[h] = Handle{a, 0, true};
		return h;
	}
	s32 DRead(s32 dd, vfs::SceIoDirent *out) override {
		auto it = handles_.find(dd);
		if (it == handles_.end() || !it->second.dir) return (s32)SCE_KERNEL_ERROR_BADF;
		const u32 buf = scratch_ + kBufOff;
		mem::Memset(buf, 0, sizeof(vfs::SceIoDirent));
		const s32 r = Call(IoDread, {it->second.arg, buf});
		if (r >= 0) memcpy(out, GPtr(buf), sizeof(vfs::SceIoDirent));
		return r;
	}
	s32 DClose(s32 dd) override {
		auto it = handles_.find(dd);
		if (it == handles_.end() || !it->second.dir) return (s32)SCE_KERNEL_ERROR_BADF;
		const s32 r = Call(IoDclose, {it->second.arg}, 0);
		FreeFileArg(it->second.arg);
		handles_.erase(it);
		return r;
	}

	s32 Devctl(const std::string &devName, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
		return WithFileArg([&](u32 a) {
			return Call(IoDevctl, {a, PutPath(kPath1Off, devName + ":"), cmd, in, inLen, out, outLen});
		});
	}

	void Init() {
		GWrite32(DrvArg(), drv_);
		GWrite32(DrvArg() + 4, 0);
		// Registered whatever IoInit returns.
		Call(IoInit, {DrvArg()}, 0);
	}
	void Exit() {
		for (auto &kv : handles_) FreeFileArg(kv.second.arg);
		handles_.clear();
		Call(IoExit, {DrvArg()}, 0);
	}

private:
	struct Handle {
		u32 arg;  // its PspIoDrvFileArg
		u32 flags;
		bool dir;
	};
	const Handle *Get(s32 fd) const {
		auto it = handles_.find(fd);
		return it == handles_.end() || it->second.dir ? nullptr : &it->second;
	}

	u32 drv_, funcs_, scratch_;
	std::map<s32, Handle> handles_;
	s32 next_ = 1;
};

class GuestDriverDevice : public Device {
public:
	GuestDriverDevice(u32 drv, u32 devType, std::unique_ptr<GuestDriverFs> fs, u32 scratch)
		: drv_(drv), devType_(devType), fs_(std::move(fs)), scratch_(scratch) {}
	~GuestDriverDevice() override { kernel::g_kernelMem.Free(scratch_); }
	DevClass Class() const override { return DevClass::GuestDriver; }
	vfs::FileSystem *Fs() override { return fs_.get(); }
	u32 DevType() const override { return devType_; }
	int Devctl(const std::string &devName, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, u32 *) override {
		return fs_->Devctl(devName, cmd, in, inLen, out, outLen);
	}
	GuestDriverFs *DriverFs() { return fs_.get(); }

private:
	u32 drv_, devType_;
	std::unique_ptr<GuestDriverFs> fs_;
	u32 scratch_;
};

std::map<std::string, std::shared_ptr<GuestDriverDevice>> g_drivers;

}  // namespace

int AddGuestDriver(u32 drv) {
	if (!mem::IsValidRange(drv, 20)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	const char *name = mem::GetCString(GRead32(drv), 32);
	const u32 funcs = GRead32(drv + 16);
	if (!name || !*name || !mem::IsValidRange(funcs, 4 * kDrvFuncs)) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	const std::string device = CanonicalDevice(name);
	if (g_drivers.count(device)) return (int)SCE_KERNEL_ERROR_ERROR;
	const u32 scratch = kernel::g_kernelMem.Alloc(kScratchSize, true, "IoDrv");
	if (!scratch) return (int)SCE_KERNEL_ERROR_NO_MEMORY;
	auto fs = std::make_unique<GuestDriverFs>(drv, funcs, scratch);
	GuestDriverFs *raw = fs.get();
	auto dev = std::make_shared<GuestDriverDevice>(drv, GRead32(drv + 4), std::move(fs), scratch);
	g_drivers[device] = dev;
	Mount(device, dev);
	LOGI("IO", "sceIoAddDrv: %s:", device.c_str());
	raw->Init();
	return 0;
}

int DelGuestDriver(const std::string &name) {
	const std::string device = CanonicalDevice(name);
	auto it = g_drivers.find(device);
	if (it == g_drivers.end()) return (int)SCE_KERNEL_ERROR_NODEV;
	std::shared_ptr<GuestDriverDevice> dev = it->second;
	g_drivers.erase(it);
	dev->DriverFs()->Exit();
	if (MountedDevice(device) == dev) Unmount(device);
	return 0;
}

void ResetGuestDrivers() { g_drivers.clear(); }

}  // namespace io
