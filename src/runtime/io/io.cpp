// SPDX-License-Identifier: GPL-2.0-or-later
// Memory stick devctls ported from PPSSPP (Core/HLE/sceIo.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later.
#include "runtime/io/io.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>

#include "allegrex/guestmem.h"
#include "common/fileutil.h"
#include "common/log.h"
#include "fs/vfs.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/kernel/callbacks.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/threads.h"

namespace io {

namespace {

std::map<std::string, std::shared_ptr<Device>> g_mounts;
std::string g_cwd = "ms0:/";
std::map<SceUID, std::string> g_threadCwd;

// Memory stick state: always inserted, FAT assigned.
constexpr u32 kMsInserted = 1;
constexpr u32 kMsDeviceInserted = 4;  // 0x02025801 driver status
std::vector<SceUID> g_msCallbacks, g_msFatCallbacks;
u32 g_msFatState = 1;
// A constant capacity keeps results reproducible (PPSSPP does the same).
constexpr u64 kFakeFreeSpace = 1536ull << 20;
// Four words the memory stick driver keeps per inserted stick (mscmhc0: devctl 0x0201580B sets
// them, 0x0202580A reads them back) and forgets when the stick is changed. The XMB's Saved Data
// Utility sets them before it asks for another stick (to copy a save), and reads them back to
// tell whether the stick was really swapped (savedata_plugin.prx 4.01, module offset 0x11220).
u32 g_msStickWords[4] = {};
// The other memory stick (a host folder): when the XMB asks for another stick, PSPC swaps them
// as a user would (ChangeMemoryStick, defined after HostDevice).
std::string g_otherStick;
std::string g_testOutput;  // SetTestOutput
bool g_msChangeArmed = false;  // a copy between sticks is under way
bool g_otherStickIn = false;   // the second stick is in ms0:
void ChangeMemoryStick();

bool IsCallback(u32 uid) { return kernel::Get<kernel::Callback>((SceUID)uid) != nullptr; }

bool RemoveOne(std::vector<SceUID> &v, SceUID id) {
	auto it = std::find(v.begin(), v.end(), id);
	if (it == v.end()) return false;
	v.erase(it);
	return true;
}

int WriteMsCapacity(u32 inAddr, u32 inLen) {
	// The argument is a pointer to the struct, not the struct (PPSSPP; pspautotests mstick).
	if (!mem::IsValidRange(inAddr, 4) || inLen < 4) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
	u32 ptr = GRead32(inAddr);
	if (mem::IsValidRange(ptr, 20)) {
		const u32 sectorSize = 0x200, sectorCount = 0x8000 / sectorSize;
		const u32 clusters = (u32)((kFakeFreeSpace * 95 / 100) / (sectorSize * sectorCount));
		GWrite32(ptr, clusters);       // max clusters
		GWrite32(ptr + 4, clusters);   // free clusters
		GWrite32(ptr + 8, clusters);   // max sectors
		GWrite32(ptr + 12, sectorSize);
		GWrite32(ptr + 16, sectorCount);
	}
	return 0;
}

int MemoryStickDevctl(const std::string &name, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen, u32 *delayUs) {
	if (name == "fatms0") {
		switch (cmd) {
		case 0x0240D81E:  // invalidate the driver's file table cache
			return 0;
		case 0x02415821:  // register an insert/eject callback, notified at once while inserted
			if (!mem::IsValidRange(inAddr, 4) || inLen < 4) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
			if (g_msFatCallbacks.size() >= 32 || !IsCallback(GRead32(inAddr))) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
			g_msFatCallbacks.push_back((SceUID)GRead32(inAddr));
			kernel::NotifyCallback((SceUID)GRead32(inAddr), (s32)g_msFatState);
			return 0;
		case 0x02415822:  // unregister
			if (!mem::IsValidRange(inAddr, 4) || inLen < 4) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
			return RemoveOne(g_msFatCallbacks, (SceUID)GRead32(inAddr)) ? 0 : (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
		case 0x02415823:  // set FAT state
			if (!mem::IsValidRange(inAddr, 4) || inLen != 4) return -1;
			g_msFatState = GRead32(inAddr);
			return 0;
		case 0x02425823:  // FAT enabled? (ignores outLen)
			if ((s32)(outAddr + outLen) < (s32)outAddr) return (int)SCE_KERNEL_ERROR_ILLEGAL_ADDR;
			if (!mem::IsValidRange(outAddr, 4)) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
			GWrite32(outAddr, g_msFatState);
			*delayUs = 106;
			return 0;
		case 0x02425824:  // write protected?
			if (!mem::IsValidRange(outAddr, 4) || outLen != 4) return -1;
			GWrite32(outAddr, 0);
			return 0;
		case 0x02425818:
			return WriteMsCapacity(inAddr, inLen);
		}
		return (int)SCE_KERNEL_ERROR_UNSUP;
	}
	switch (cmd) {
	case 0x02025801:  // driver status
		if (!mem::IsValidRange(outAddr, 4) || outLen < 4) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
		GWrite32(outAddr, kMsDeviceInserted);
		return 0;
	case 0x02015804:  // register an insert/eject callback (mscmhc0:)
		if (!mem::IsValidRange(inAddr, 4) || outAddr != 0 || inLen < 4) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
		if (g_msCallbacks.size() >= 32 || !IsCallback(GRead32(inAddr))) return (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
		g_msCallbacks.push_back((SceUID)GRead32(inAddr));
		kernel::NotifyCallback((SceUID)GRead32(inAddr), (s32)kMsInserted);
		return 0;
	case 0x02015805:
		if (!mem::IsValidRange(inAddr, 4) || inLen < 4) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
		return RemoveOne(g_msCallbacks, (SceUID)GRead32(inAddr)) ? 0 : (int)SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	case 0x02025806:  // inserted? 1 = yes, 2 = no
		if (!mem::IsValidRange(outAddr, 4) || outLen < 4) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
		GWrite32(outAddr, kMsInserted);
		return 0;
	case 0x0201580B:  // set the stick's four words (plus a flags word)
		if (!mem::IsValidRange(inAddr, 16) || inLen < 16) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
		for (int i = 0; i < 4; i++) g_msStickWords[i] = GRead32(inAddr + 4 * i);
		// The Saved Data Utility marks the stick it copies from (see ChangeMemoryStick).
		if (!g_otherStick.empty()) g_msChangeArmed = true;
		return 0;
	case 0x0202580A:  // read them back
		if (!mem::IsValidRange(outAddr, 16) || outLen < 16) return (int)SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
		// The utility only asks after "insert the other Memory Stick and press X".
		if (g_msChangeArmed) ChangeMemoryStick();
		for (int i = 0; i < 4; i++) GWrite32(outAddr + 4 * i, g_msStickWords[i]);
		return 0;
	case 0x02425818:
		return WriteMsCapacity(inAddr, inLen);
	case 0x02425824:
		if (!mem::IsValidRange(outAddr, 4) || outLen != 4) return -1;
		GWrite32(outAddr, 0);
		return 0;
	case 0x02425856:  // used by the VSH, meaning unknown
		return 0;
	}
	return (int)SCE_KERNEL_ERROR_UNSUP;
}

class HostDevice : public Device {
public:
	HostDevice(std::string root, bool readOnly) : root_(std::move(root)), readOnly_(readOnly) {}

	DevClass Class() const override { return cls_; }
	vfs::FileSystem *Fs() override {
		if (!fs_) Configure("host0");
		return overlay_ ? overlay_.get() : fs_.get();
	}
	void OnMount(const std::string &name) override {
		name_ = name;
		Configure(name);
		overlay_.reset();
	}
	void SetOverlay(std::shared_ptr<vfs::FileSystem> fs) { overlay_ = std::move(fs); }
	// What SetFsOverlay / AddHostFallback set up, so a memory stick change can redo it.
	void SetOverlayMaker(std::function<std::shared_ptr<vfs::FileSystem>(vfs::HostFileSystem *)> make) {
		overlayMake_ = std::move(make);
	}
	void RememberFallback(const std::filesystem::path &dir) { fallbacks_.push_back(dir); }
	const std::string &RootDir() const { return root_; }
	// Another host folder behind the device (a memory stick change).
	void Reroot(const std::string &root) {
		root_ = root;
		Configure(name_);
		for (const auto &d : fallbacks_) fs_->AddFallbackRoot(d);
		overlay_ = overlayMake_ ? overlayMake_(fs_.get()) : nullptr;
	}
	int Devctl(const std::string &devName, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen, u32 *delayUs) override {
		if (cls_ == DevClass::MemoryStick) return MemoryStickDevctl(devName, cmd, inAddr, inLen, outAddr, outLen, delayUs);
		return Device::Devctl(devName, cmd, inAddr, inLen, outAddr, outLen, delayUs);
	}
	vfs::HostFileSystem *Host() {
		Fs();
		return fs_.get();
	}

private:
	void Configure(const std::string &name) {
		vfs::HostFsOptions o;
		o.readOnly = readOnly_;
		std::filesystem::path root = std::filesystem::u8path(root_);
		if (name == "ms0") {
			// Files the PSP creates keep their case, so no 8.3 uppercasing (io/shortname).
			cls_ = DevClass::MemoryStick;
			o.fat = true;
			std::error_code ec;
			for (const char *d : {"PSP/GAME", "PSP/SAVEDATA", "PSP/COMMON", "PSP/SYSTEM"}) {
				std::filesystem::create_directories(root / d, ec);
			}
		} else if (name.rfind("flash", 0) == 0) {
			cls_ = DevClass::Flash;
			o.fat = true;
		} else {
			// usbhostfs modes; names written by a PC appear as the FAT driver would show them
			// (io/directory).
			cls_ = DevClass::Host;
			o.fat = false;
			o.upperCase83 = true;
			if (name == "host0" && !g_testOutput.empty()) {
				const std::filesystem::path out = std::filesystem::u8path(g_testOutput);
				o.redirects = {{"__testoutput.txt", out},
				               {"__testerror.txt", std::filesystem::path(out).concat(".err")},
				               {"__testfinish.txt", std::filesystem::path(out).concat(".finish")}};
			}
		}
		fs_ = std::make_shared<vfs::HostFileSystem>(root, o);
	}

	std::string root_;
	bool readOnly_;
	std::string name_ = "host0";
	DevClass cls_ = DevClass::Host;
	std::shared_ptr<vfs::HostFileSystem> fs_;
	std::shared_ptr<vfs::FileSystem> overlay_;  // in front of fs_ (see SetFsOverlay)
	std::function<std::shared_ptr<vfs::FileSystem>(vfs::HostFileSystem *)> overlayMake_;
	std::vector<std::filesystem::path> fallbacks_;
};

// The XMB's Saved Data Utility copies a save from one memory stick to another: it marks the stick
// it copies from (devctl 0x0201580B), reads the save, says "Insert the destination Memory Stick
// and press the X button", then reads the marks back (0x0202580A): a stick that still has them
// wasn't changed ("This is not the correct Memory Stick"). Then it writes the save, and for a big
// one asks for the source again, and so on (savedata_plugin.prx 4.01, module offsets 0x10050-
// 0x11220). PSPC has a second stick, a host folder (SetOtherMemoryStick; ms1 next to ms0 by
// default), and changes sticks for the user when the utility checks after such a request: the
// other stick goes into ms0: and the driver forgets the marks, as for any newly inserted stick.
// No insert/eject callbacks: the change happens between two of the utility's own calls, while
// its page waits for exactly that. "Copy completed. Insert the source Memory Stick": when the
// utility lists the saves again, the first stick goes back in (OnDirectoryOpen).
void ChangeMemoryStick() {
	auto *h = dynamic_cast<HostDevice *>(MountedDevice("ms0").get());
	if (!h || g_otherStick.empty()) return;
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(g_otherStick), ec);
	const std::string previous = h->RootDir();
	h->Reroot(g_otherStick);
	LOGI("IO", "Memory stick changed for the Saved Data Utility: ms0: is now %s (was %s)", g_otherStick.c_str(),
	     previous.c_str());
	g_otherStick = previous;
	g_otherStickIn = !g_otherStickIn;
	for (u32 &w : g_msStickWords) w = 0;
}

class VfsDevice : public Device {
public:
	VfsDevice(std::shared_ptr<vfs::FileSystem> fs, DevClass cls) : fs_(std::move(fs)), cls_(cls) {}
	DevClass Class() const override { return cls_; }
	vfs::FileSystem *Fs() override { return fs_.get(); }

private:
	std::shared_ptr<vfs::FileSystem> fs_;
	DevClass cls_;
};

// The memory stick's raw partition ("msstor0p1:"), which the XMB's Saved Data Utility opens to
// ask about the stick: always writable, a Memory Stick PRO Duo. Reads give zeroed sectors; nothing
// is written. Answers as msstor.prx does: ioctl 0x02125008 "writable" (inserted and not locked),
// 0x02125009 "write protected", devctl 0x02125802 the stick type (0x11 or 0x41).
class MsStorFs : public vfs::FileSystem {
public:
	u32 Caps() const override { return vfs::FS_CAP_BLOCK_DEVICE; }
	s32 Open(std::string_view path, u32 flags, s32) override {
		if (!path.empty() && path != "/") return vfs::SCE_ERRNO_ENOENT;
		if (flags & vfs::PSP_O_WRONLY) return vfs::SCE_ERRNO_EROFS;
		return 1;
	}
	s32 Close(s32) override { return 0; }
	s64 Read(s32, void *buf, u64 size) override {
		memset(buf, 0, (size_t)size);
		return (s64)size;
	}
	s64 Write(s32, const void *, u64) override { return vfs::SCE_ERRNO_EROFS; }
	s64 Seek(s32, s64 offset, s32 whence) override { return whence == vfs::PSP_SEEK_SET ? offset : 0; }
	s32 GetHandleInfo(s32, vfs::HandleInfo *out) override {
		*out = vfs::HandleInfo();
		out->device = true;
		out->size = kFakeFreeSpace;
		return 0;
	}
	s32 Ioctl(s32, u32 cmd, const void *, u32, void *out, u32 outLen) override {
		if (out && outLen) memset(out, 0, outLen);
		if (cmd == 0x02125008 && out && outLen >= 4) *(u32 *)out = 1;
		return 0;
	}
	s32 GetStat(std::string_view, vfs::SceIoStat *) override { return vfs::SCE_ERRNO_ENOENT; }
	s32 ChStat(std::string_view, const vfs::SceIoStat &, u32) override { return vfs::SCE_ERRNO_EROFS; }
	s32 Remove(std::string_view) override { return vfs::SCE_ERRNO_EROFS; }
	s32 Mkdir(std::string_view, s32) override { return vfs::SCE_ERRNO_EROFS; }
	s32 Rmdir(std::string_view) override { return vfs::SCE_ERRNO_EROFS; }
	s32 Rename(std::string_view, std::string_view) override { return vfs::SCE_ERRNO_EROFS; }
	s32 DOpen(std::string_view) override { return vfs::SCE_ERRNO_ENOTDIR; }
	s32 DRead(s32, vfs::SceIoDirent *) override { return 0; }
	s32 DClose(s32) override { return 0; }
};

class MsStorDevice : public Device {
public:
	DevClass Class() const override { return DevClass::Host; }
	vfs::FileSystem *Fs() override { return &fs_; }
	u32 DevType() const override { return 0x04; }
	int Devctl(const std::string &, u32 cmd, u32, u32, u32 outAddr, u32 outLen, u32 *) override {
		if (cmd == 0x02125802 && mem::IsValidRange(outAddr, 4) && outLen >= 4) {
			GWrite32(outAddr, 0x41);
			return 0;
		}
		if (outLen && mem::IsValidRange(outAddr, outLen)) mem::Memset(outAddr, 0, outLen);
		return 0;
	}

private:
	MsStorFs fs_;
};

class EmulatorDevice : public Device {
public:
	explicit EmulatorDevice(bool hasDisplay) : hasDisplay_(hasDisplay) {
		// A thread that ends right after a printf (returning from its entry) makes no further call.
		static bool listening = false;
		if (!listening) {
			listening = true;
			kernel::AddThreadEndListener([](kernel::PspThread *t) { FlushOutput(t->uid); });
		}
	}
	DevClass Class() const override { return DevClass::Emulator; }
	int Devctl(const std::string &, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen, u32 *) override {
		switch (cmd) {
		case 1:  // GET_HAS_DISPLAY
			if (mem::IsValidRange(outAddr, 4)) GWrite32(outAddr, hasDisplay_ ? 1 : 0);
			return 0;
		case 2:  // SEND_OUTPUT
			if (inLen && mem::IsValidRange(inAddr, inLen)) SendOutput((const char *)GPtr(inAddr), inLen);
			return 0;
		case 3:  // IS_EMULATOR
			if (mem::IsValidRange(outAddr, 4)) GWrite32(outAddr, 1);
			return 0;
		case 4:     // VERIFY_STATE
		case 0x10:  // SEND_CTRLDATA
		case 0x20:  // EMIT_SCREENSHOT
		case 0x30:  // TOGGLE_FASTFORWARD
			return 0;
		default:
			return 0;
		}
	}

private:
	// pspautotests' expectations were recorded with stdout going to an unbuffered host0: file:
	// each printf is a single write, ~1.3 ms over USB that keeps the CPU (a thread of the same
	// priority doesn't run meanwhile: threads/threads/change; time does pass: intr/waits). Here
	// a printf arrives in pieces, one call each with nothing in between, so the pieces are kept
	// together, and the line is logged and the write's time spent when the thread moves on to
	// another call. With a display the test writes that file as well and pays there.
	static constexpr u32 kOutputWriteUs = 1300;

	void SendOutput(const char *text, u32 len) {
		const SceUID tid = sched::CurrentId();
		if (hasDisplay_ || !tid) {
			logging::GuestOutput(text, len);
			return;
		}
		PendingOutput()[tid].append(text, len);
		const bool deferred = hle::DeferToNextCall([tid]() {
			FlushOutput(tid);
			sched::SpendUs(kOutputWriteUs, 0);
		});
		if (!deferred) FlushOutput(tid);
	}

	static std::map<SceUID, std::string> &PendingOutput() {
		static std::map<SceUID, std::string> pending;
		return pending;
	}

	static void FlushOutput(SceUID tid) {
		auto &pending = PendingOutput();
		auto it = pending.find(tid);
		if (it == pending.end()) return;
		logging::GuestOutput(it->second.data(), it->second.size());
		pending.erase(it);
	}

	bool hasDisplay_;
};

}  // namespace

u32 Device::DevType() const { return Class() == DevClass::UmdBlock ? 0x04 : 0x10; }

int Device::Devctl(const std::string &devName, u32 cmd, u32, u32, u32, u32, u32 *) {
	LOGW("IO", "Unsupported devctl %s: %08x", devName.c_str(), cmd);
	return (int)SCE_KERNEL_ERROR_UNSUP;
}

namespace {
constexpr int kNoFs = (int)SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND;
}

int Device::Open(const std::string &path, u32 flags, int mode) { return Fs() ? Fs()->Open(path, flags, mode) : kNoFs; }
int Device::Close(int h) { return Fs() ? Fs()->Close(h) : kNoFs; }
s64 Device::Read(int h, void *buf, s64 size) { return Fs() && size >= 0 ? Fs()->Read(h, buf, (u64)size) : kNoFs; }
s64 Device::Write(int h, const void *buf, s64 size) { return Fs() && size >= 0 ? Fs()->Write(h, buf, (u64)size) : kNoFs; }
s64 Device::Seek(int h, s64 offset, int whence) { return Fs() ? Fs()->Seek(h, offset, whence) : kNoFs; }
int Device::GetStat(const std::string &path, PspIoStat &st) {
	if (!Fs()) return kNoFs;
	vfs::SceIoStat v;
	memcpy(&v, &st, sizeof(v));
	int r = Fs()->GetStat(path, &v);
	if (r >= 0) memcpy(&st, &v, sizeof(v));
	return r;
}
int Device::ChStat(const std::string &path, const PspIoStat &st, u32 bits) {
	if (!Fs()) return kNoFs;
	vfs::SceIoStat v;
	memcpy(&v, &st, sizeof(v));
	return Fs()->ChStat(path, v, bits);
}
int Device::Remove(const std::string &path) { return Fs() ? Fs()->Remove(path) : kNoFs; }
int Device::Mkdir(const std::string &path, int mode) { return Fs() ? Fs()->Mkdir(path, mode) : kNoFs; }
int Device::Rmdir(const std::string &path) { return Fs() ? Fs()->Rmdir(path) : kNoFs; }
int Device::Rename(const std::string &from, const std::string &to) { return Fs() ? Fs()->Rename(from, to) : kNoFs; }
int Device::DOpen(const std::string &path) { return Fs() ? Fs()->DOpen(path) : kNoFs; }
int Device::DRead(int h, PspIoDirent &ent) {
	if (!Fs()) return kNoFs;
	vfs::SceIoDirent v;
	memcpy(&v, &ent, sizeof(v));
	int r = Fs()->DRead(h, &v);
	memcpy(&ent, &v, sizeof(v));
	return r;
}
int Device::DClose(int h) { return Fs() ? Fs()->DClose(h) : kNoFs; }

std::string CanonicalDevice(std::string_view device) {
	while (!device.empty() && device.front() == ' ') device.remove_prefix(1);
	if (!device.empty() && device.back() == ':') device.remove_suffix(1);
	std::string d = ToLower(std::string(device));
	if (d == "fatms0" || d == "fatms" || d == "ms" || d == "memstick" || d == "mscmhc0") return "ms0";
	if (d.rfind("umd", 0) == 0) return "umd0";  // umd:, umd1:, umd00:... are the same block device
	if (d == "isofs" || d == "isofs0") return "disc0";
	if (d.rfind("host", 0) == 0) return "host0";
	return d;
}

void Init() {
	ResetGuestDrivers();
	g_mounts.clear();
	g_threadCwd.clear();
	g_cwd = "ms0:/";
	g_msCallbacks.clear();
	g_msFatCallbacks.clear();
	g_msFatState = 1;
}

void Shutdown() {
	g_mounts.clear();
	g_threadCwd.clear();
}

void Mount(const std::string &device, std::shared_ptr<Device> dev) {
	std::string name = CanonicalDevice(device);
	if (dev) dev->OnMount(name);
	g_mounts[name] = std::move(dev);
}

void Unmount(const std::string &device) { g_mounts.erase(CanonicalDevice(device)); }

std::shared_ptr<Device> MountedDevice(const std::string &device) {
	auto it = g_mounts.find(CanonicalDevice(device));
	return it == g_mounts.end() ? nullptr : it->second;
}

void SetCwd(const std::string &pspDir) { g_cwd = pspDir; }
const std::string &Cwd() { return g_cwd; }

std::string ThreadCwd(SceUID thread) {
	auto it = g_threadCwd.find(thread);
	return it == g_threadCwd.end() ? g_cwd : it->second;
}

void SetThreadCwd(SceUID thread, const std::string &dir) { g_threadCwd[thread] = dir; }

int Resolve(std::string_view pspPath, Resolved *out) {
	std::string_view in = pspPath;
	if (in.find(':') != std::string_view::npos) {
		// "   ms0:/x" is "ms0:/x".
		while (!in.empty() && in.front() == ' ') in.remove_prefix(1);
	}
	std::string_view dev, rest;
	std::string cwd;
	if (!vfs::SplitPspDevice(in, &dev, &rest)) {
		cwd = ThreadCwd(sched::CurrentId());
		if (cwd.empty()) return (int)SCE_KERNEL_ERROR_NOCWD;
	}
	std::string full = vfs::NormalizePspPath(cwd, in);
	if (!vfs::SplitPspDevice(full, &dev, &rest)) return (int)SCE_KERNEL_ERROR_NODEV;
	out->device = CanonicalDevice(dev);
	auto it = g_mounts.find(out->device);
	if (it == g_mounts.end() || !it->second) return (int)SCE_KERNEL_ERROR_NODEV;
	out->dev = it->second;
	out->path = std::string(rest);
	return 0;
}

bool ResolvePath(const std::string &pspPath, std::string &device, std::string &path) {
	Resolved r;
	if (Resolve(pspPath, &r) < 0) return false;
	device = r.device;
	path = r.path;
	return true;
}

bool ReadFile(const std::string &pspPath, std::vector<u8> &out) {
	Resolved r;
	if (Resolve(pspPath, &r) < 0 || !r.dev->Fs()) return false;
	vfs::FileSystem *fs = r.dev->Fs();
	s32 h = fs->Open(r.path, vfs::PSP_O_RDONLY, 0);
	if (h < 0) return false;
	vfs::HandleInfo info;
	bool ok = fs->GetHandleInfo(h, &info) >= 0 && !info.sectorMode;
	if (ok) {
		out.resize((size_t)info.size);
		s64 got = out.empty() ? 0 : fs->Read(h, out.data(), out.size());
		ok = got == (s64)out.size();
	}
	fs->Close(h);
	return ok;
}

std::string HostPathFor(const std::string &pspPath) {
	Resolved r;
	if (Resolve(pspPath, &r) < 0) return "";
	auto *h = dynamic_cast<HostDevice *>(r.dev.get());
	std::filesystem::path p;
	if (!h || h->Host()->HostPath(r.path, &p) < 0) return "";
	return PathToUtf8(p);
}

std::shared_ptr<Device> MakeHostDevice(const std::string &hostRoot, bool readOnly) {
	return std::make_shared<HostDevice>(hostRoot, readOnly);
}

bool SetFsOverlay(const std::string &device, const std::function<std::shared_ptr<vfs::FileSystem>(vfs::HostFileSystem *)> &make) {
	auto *h = dynamic_cast<HostDevice *>(MountedDevice(device).get());
	if (!h) return false;
	h->SetOverlay(make(h->Host()));
	h->SetOverlayMaker(make);
	return true;
}

bool AddHostFallback(const std::string &device, const std::string &dir) {
	auto *h = dynamic_cast<HostDevice *>(MountedDevice(device).get());
	std::error_code ec;
	if (!h || dir.empty() || !std::filesystem::is_directory(fs::u8path(dir), ec)) return false;
	h->Host()->AddFallbackRoot(fs::u8path(dir));
	h->RememberFallback(fs::u8path(dir));
	return true;
}

void SetOtherMemoryStick(const std::string &hostDir) { g_otherStick = hostDir; }

void OnDirectoryOpen(const std::string &device, const std::string &path) {
	if (!g_msChangeArmed || device != "ms0") return;
	std::string p = ToLower(path);
	std::replace(p.begin(), p.end(), '\\', '/');
	while (p.size() > 1 && p.back() == '/') p.pop_back();
	if (p != "/psp/savedata") return;
	// The copy is over (or was cancelled): the source stick goes back in, as the XMB asks.
	g_msChangeArmed = false;
	if (g_otherStickIn) ChangeMemoryStick();
}

std::shared_ptr<Device> MakeVfsDevice(std::shared_ptr<vfs::FileSystem> fs, DevClass cls) {
	return std::make_shared<VfsDevice>(std::move(fs), cls);
}

std::shared_ptr<Device> MakeEmulatorDevice(bool hasDisplay) { return std::make_shared<EmulatorDevice>(hasDisplay); }

void SetTestOutput(const std::string &path) { g_testOutput = path; }

std::shared_ptr<Device> MakeMsStorDevice() { return std::make_shared<MsStorDevice>(); }

}  // namespace io
