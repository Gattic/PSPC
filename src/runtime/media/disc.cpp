// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/media/disc.h"

#include <filesystem>
#include <vector>

#include "common/log.h"
#include "fs/vfs.h"
#include "runtime/io/io.h"

namespace umd {
void NotifyDiscChanged();
}

namespace media {

namespace {

std::shared_ptr<vfs::IsoFileSystem> g_iso;
std::string g_discId;
std::string g_discPath;
unsigned g_discType = 0x10;

}  // namespace

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
	g_discId.clear();
	g_discPath = hostPath;
	g_discType = 0;
	static const struct {
		const char *dir;
		unsigned type;
	} kKinds[] = {{"/PSP_GAME", 0x10}, {"/UMD_VIDEO", 0x20}, {"/UMD_AUDIO", 0x40}};
	for (const auto &k : kKinds) {
		vfs::SceIoStat st{};
		if (iso->GetStat(k.dir, &st) >= 0) g_discType |= k.type;
	}
	if (!g_discType) g_discType = 0x10;
	std::vector<u8> sfo;
	if (iso->ReadWholeFile("/UMD_VIDEO/PARAM.SFO", sfo) == 0 && !(g_discType & 0x10)) {
		vfs::ParamSfo p;
		if (p.Parse(sfo.data(), sfo.size())) {
			g_discId = p.GetString("DISC_ID");
			LOGI("UMD", "UMD Video %s: %s", g_discId.c_str(), p.GetString("TITLE").c_str());
		}
	}
	if (iso->ReadWholeFile("/PSP_GAME/PARAM.SFO", sfo) == 0) {
		vfs::ParamSfo p;
		if (p.Parse(sfo.data(), sfo.size())) {
			g_discId = p.GetString("DISC_ID");
			LOGI("UMD", "Disc %s: %s", g_discId.c_str(), p.GetString("TITLE").c_str());
		}
	}
	RemountDiscIfAny();
	umd::NotifyDiscChanged();
	return true;
}

void UnmountDisc() {
	io::Unmount("disc0");
	io::Unmount("umd0");
	g_iso.reset();
	g_discId.clear();
	g_discPath.clear();
	g_discType = 0x10;
	umd::NotifyDiscChanged();
}

void RemountDiscIfAny() {
	if (!g_iso) return;
	io::Mount("disc0", io::MakeVfsDevice(g_iso, io::DevClass::UmdFiles));
	io::Mount("umd0", io::MakeVfsDevice(g_iso, io::DevClass::UmdBlock));
}

bool HaveDisc() { return g_iso != nullptr; }
std::string DiscId() { return g_discId; }
std::string DiscPath() { return g_discPath; }
unsigned DiscType() { return g_iso ? g_discType : 0x10; }
std::shared_ptr<vfs::IsoFileSystem> Disc() { return g_iso; }

}  // namespace media
