// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/media/disc.h"

#include <vector>

#include "allegrex/guestmem.h"
#include "common/log.h"
#include "fs/vfs.h"
#include "runtime/core/memory.h"
#include "runtime/kernel/errors.h"

namespace media {

namespace {

static_assert(sizeof(vfs::SceIoStat) == sizeof(io::PspIoStat), "stat layouts must match");
static_assert(sizeof(vfs::SceIoDirent) == sizeof(io::PspIoDirent), "dirent layouts must match");

class VfsDevice : public io::Device {
public:
	VfsDevice(std::shared_ptr<vfs::FileSystem> fs, bool sectorMode, u32 devType)
		: fs_(std::move(fs)), sectorMode_(sectorMode), devType_(devType) {}

	int Open(const std::string &path, int flags, int mode) override {
		u32 f = (u32)flags;
		if (sectorMode_) f |= vfs::PSPC_O_SECTOR_MODE;
		// "umd0:" with no path opens the whole device.
		std::string p = (sectorMode_ && path == "/") ? std::string() : path;
		return fs_->Open(p, f, mode);
	}
	int Close(int h) override { return fs_->Close(h); }
	s64 Read(int h, void *buf, s64 size) override { return fs_->Read(h, buf, (u64)size); }
	s64 Write(int h, const void *buf, s64 size) override { return fs_->Write(h, buf, (u64)size); }
	s64 Seek(int h, s64 offset, int whence) override { return fs_->Seek(h, offset, whence); }
	int GetStat(const std::string &path, io::PspIoStat &st) override {
		vfs::SceIoStat s{};
		int r = fs_->GetStat(path, &s);
		if (r >= 0) memcpy(&st, &s, sizeof(st));
		return r;
	}
	int ChStat(const std::string &path, const io::PspIoStat &st, u32 bits) override {
		vfs::SceIoStat s;
		memcpy(&s, &st, sizeof(s));
		return fs_->ChStat(path, s, bits);
	}
	int Remove(const std::string &p) override { return fs_->Remove(p); }
	int Mkdir(const std::string &p, int mode) override { return fs_->Mkdir(p, mode); }
	int Rmdir(const std::string &p) override { return fs_->Rmdir(p); }
	int Rename(const std::string &a, const std::string &b) override { return fs_->Rename(a, b); }
	int DOpen(const std::string &p) override { return fs_->DOpen(p); }
	int DRead(int h, io::PspIoDirent &e) override {
		vfs::SceIoDirent d{};
		int r = fs_->DRead(h, &d);
		if (r > 0) memcpy(&e, &d, sizeof(e));
		return r;
	}
	int DClose(int h) override { return fs_->DClose(h); }
	int Ioctl(int h, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen) override {
		const void *in = (inLen && mem::IsValidRange(inAddr, inLen)) ? GPtr(inAddr) : nullptr;
		void *out = (outLen && mem::IsValidRange(outAddr, outLen)) ? GPtr(outAddr) : nullptr;
		return fs_->Ioctl(h, cmd, in, in ? inLen : 0, out, out ? outLen : 0);
	}
	int Devctl(const std::string &devName, u32 cmd, u32 inAddr, u32 inLen, u32 outAddr, u32 outLen) override {
		switch (cmd) {
		case 0x01F00003:  // UMD: activate/check
		case 0x01F20014:  // UMD: registered
		case 0x01F100A3:
		case 0x01F100A4:
		case 0x01F300A5:
		case 0x01F300A7:
		case 0x01F300A8:
		case 0x01E18030:
			if (outLen >= 4 && mem::IsValidRange(outAddr, 4)) GWrite32(outAddr, 1);
			return 0;
		case 0x01F20001:  // disc type
			if (outLen >= 8 && mem::IsValidRange(outAddr, 8)) {
				GWrite32(outAddr, 0xFFFFFFFF);
				GWrite32(outAddr + 4, 0x10);
			}
			return 0;
		case 0x01F20002:  // current LBA
		case 0x01F20003:  // number of sectors
			if (outLen >= 4 && mem::IsValidRange(outAddr, 4)) {
				auto *iso = dynamic_cast<vfs::IsoFileSystem *>(fs_.get());
				GWrite32(outAddr, (cmd == 0x01F20003 && iso) ? iso->Device()->NumBlocks() : 0);
			}
			return 0;
		default: return io::Device::Devctl(devName, cmd, inAddr, inLen, outAddr, outLen);
		}
	}
	u32 DevType() const override { return devType_; }

private:
	std::shared_ptr<vfs::FileSystem> fs_;
	bool sectorMode_;
	u32 devType_;
};

std::shared_ptr<vfs::IsoFileSystem> g_iso;
std::string g_discId;

}  // namespace

std::shared_ptr<io::Device> MakeVfsDevice(std::shared_ptr<vfs::FileSystem> fs, bool sectorMode, u32 devType) {
	return std::make_shared<VfsDevice>(std::move(fs), sectorMode, devType);
}

bool MountDisc(const std::string &hostPath) {
	std::string err;
	std::unique_ptr<vfs::BlockDevice> bd = vfs::OpenBlockDevice(std::filesystem::u8path(hostPath), &err);
	if (!bd) {
		LOGE("UMD", "%s: %s", hostPath.c_str(), err.c_str());
		return false;
	}
	auto iso = std::make_shared<vfs::IsoFileSystem>(std::shared_ptr<vfs::BlockDevice>(std::move(bd)));
	if (!iso->IsValid()) {
		LOGE("UMD", "%s: %s", hostPath.c_str(), iso->Error().c_str());
		return false;
	}
	g_iso = iso;
	std::vector<u8> sfo;
	if (iso->ReadWholeFile("/PSP_GAME/PARAM.SFO", sfo) == 0) {
		vfs::ParamSfo p;
		if (p.Parse(sfo.data(), sfo.size())) {
			g_discId = p.GetString("DISC_ID");
			LOGI("UMD", "Disc %s: %s", g_discId.c_str(), p.GetString("TITLE").c_str());
		}
	}
	RemountDiscIfAny();
	return true;
}

void RemountDiscIfAny() {
	if (!g_iso) return;
	io::Mount("disc0", MakeVfsDevice(g_iso, false, 0x10));
	io::Mount("umd0", MakeVfsDevice(g_iso, true, 0x04));
}

bool HaveDisc() { return g_iso != nullptr; }
std::string DiscId() { return g_discId; }

}  // namespace media
