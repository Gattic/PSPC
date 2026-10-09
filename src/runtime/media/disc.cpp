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
	std::vector<u8> sfo;
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
	umd::NotifyDiscChanged();
}

void RemountDiscIfAny() {
	if (!g_iso) return;
	io::Mount("disc0", io::MakeVfsDevice(g_iso, io::DevClass::UmdFiles));
	io::Mount("umd0", io::MakeVfsDevice(g_iso, io::DevClass::UmdBlock));
}

bool HaveDisc() { return g_iso != nullptr; }
std::string DiscId() { return g_discId; }
std::shared_ptr<vfs::IsoFileSystem> Disc() { return g_iso; }

}  // namespace media
