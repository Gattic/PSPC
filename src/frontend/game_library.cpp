// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend/game_library.h"
#include "common/fileutil.h"
#include "fs/vfs.h"
#include <algorithm>

namespace frontend {
namespace fs = std::filesystem;
namespace {
void ReadSfo(const std::vector<u8> &sfo, GameInfo &game) {
	vfs::ParamSfo parsed;
	if (!parsed.Parse(sfo)) return;
	std::string title = parsed.GetString("TITLE");
	if (!title.empty()) game.title = std::move(title);
	game.id = parsed.GetString("DISC_ID");
}
bool GetPbpMetadata(const vfs::PbpFile &pbp, vfs::PbpSection section, std::vector<u8> &out) {
	out.clear();
	return pbp.SectionSize(section) <= vfs::kMaxMetadataBytes && pbp.GetSection(section, out);
}
}

bool IsDisc(const fs::path &path) {
	std::string ext = ToLower(path.extension().string());
	return ext == ".iso" || ext == ".cso" || ext == ".zso" || ext == ".dax";
}

bool DescribeGame(const fs::path &path, GameInfo &game) {
	game = {};
	game.path = path;
	game.title = PathToUtf8(path.stem());
	std::error_code ec;
	game.size = fs::file_size(path, ec);
	if (ec) return false;
	std::string ext = ToLower(path.extension().string());
	if (IsDisc(path)) {
		std::string error;
		auto device = vfs::OpenBlockDevice(path, &error);
		if (!device) return false;
		vfs::IsoFileSystem iso(std::shared_ptr<vfs::BlockDevice>(std::move(device)));
		if (!iso.IsValid()) return false;
		std::vector<u8> sfo;
		if (iso.ReadWholeFile("/PSP_GAME/PARAM.SFO", sfo, vfs::kMaxMetadataBytes) == 0) ReadSfo(sfo, game);
		iso.ReadWholeFile("/PSP_GAME/ICON0.PNG", game.icon, vfs::kMaxMetadataBytes);
		return true;
	}
	if (ext == ".pbp") {
		vfs::PbpFile pbp;
		if (!pbp.Open(path)) return false;
		std::vector<u8> sfo;
		if (GetPbpMetadata(pbp, vfs::PbpSection::ParamSfo, sfo)) ReadSfo(sfo, game);
		GetPbpMetadata(pbp, vfs::PbpSection::Icon0Png, game.icon);
		return true;
	}
	return ext == ".prx" || ext == ".elf";
}

std::vector<u8> ReadPic1(const fs::path &path) {
	std::vector<u8> png;
	if (IsDisc(path)) {
		std::string error;
		if (auto device = vfs::OpenBlockDevice(path, &error)) {
			vfs::IsoFileSystem iso(std::shared_ptr<vfs::BlockDevice>(std::move(device)));
			if (iso.IsValid() && iso.ReadWholeFile("/PSP_GAME/PIC1.PNG", png, vfs::kMaxMetadataBytes) != 0) png.clear();
		}
	} else if (ToLower(path.extension().string()) == ".pbp") {
		vfs::PbpFile pbp;
		if (pbp.Open(path)) GetPbpMetadata(pbp, vfs::PbpSection::Pic1Png, png);
	}
	return png;
}

std::vector<GameInfo> ScanGames(const fs::path &directory) {
	std::vector<GameInfo> games;
	for (auto it = fs::recursive_directory_iterator(directory, fs::directory_options::skip_permission_denied);
	     it != fs::recursive_directory_iterator(); ++it) {
		if (it.depth() >= 3) it.disable_recursion_pending();
		if (!it->is_regular_file()) continue;
		std::string ext = ToLower(it->path().extension().string());
		// Modules inside a game's folders are its libraries, not games.
		if (it.depth() > 0 && (ext == ".prx" || ext == ".elf")) continue;
		GameInfo game;
		if (DescribeGame(it->path(), game)) games.push_back(std::move(game));
	}
	std::sort(games.begin(), games.end(), [](const GameInfo &a, const GameInfo &b) {
		return a.title == b.title ? a.path < b.path : a.title < b.title;
	});
	return games;
}
}  // namespace frontend
