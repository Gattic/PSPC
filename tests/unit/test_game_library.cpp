// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend/game_library.h"
#include "fs/param_sfo.h"
#include "test_fs_helpers.h"
#include "testing.h"

namespace {
bool WriteBytes(const std::filesystem::path &path, const std::vector<u8> &bytes) {
	return WriteWholeFile(path, bytes.data(), bytes.size());
}
}

TEST(game_library_metadata_unicode_and_scan_boundaries) {
	fstest::TempDir temp("game_library");
	vfs::ParamSfo sfo;
	sfo.SetString("TITLE", "Unicode title 日本語");
	sfo.SetString("DISC_ID", "TEST00001");
	const std::vector<u8> icon{1, 2, 3}, background{4, 5, 6};
	auto iso = fstest::BuildIso({{"PSP_GAME/PARAM.SFO", sfo.Serialize()},
	                           {"PSP_GAME/ICON0.PNG", icon}, {"PSP_GAME/PIC1.PNG", background}});
	auto path = temp.Path() / std::filesystem::path(u8"λ disc.ISO");
	CHECK(WriteBytes(path, iso.image));
	frontend::GameInfo game;
	CHECK(frontend::DescribeGame(path, game));
	CHECK(game.path == path);
	CHECK(game.title == "Unicode title 日本語");
	CHECK(game.id == "TEST00001");
	CHECK_EQ(game.size, (u64)iso.image.size());
	CHECK(game.icon == icon);
	CHECK(frontend::ReadPic1(path) == background);

	std::filesystem::create_directories(temp.Path() / "nested/deeper/third/fourth");
	CHECK(WriteBytes(temp.Path() / "standalone.ELF", std::vector<u8>{0}));
	CHECK(WriteBytes(temp.Path() / "nested/library.prx", std::vector<u8>{0}));
	CHECK(WriteBytes(temp.Path() / "nested/deeper/third/fourth/hidden.iso", iso.image));
	CHECK(WriteBytes(temp.Path() / "invalid.iso", std::vector<u8>{0}));
	CHECK(WriteBytes(temp.Path() / "notes.txt", std::vector<u8>{0}));
	auto games = frontend::ScanGames(temp.Path());
	CHECK_EQ(games.size(), (size_t)2);
	if (games.size() == 2) {
		CHECK(games[0].path == path);
		CHECK(games[1].title == "standalone");
	}
	CHECK(!frontend::DescribeGame(temp.Path() / "invalid.iso", game));
}

TEST(game_library_pbp_metadata_and_missing_directory) {
	fstest::TempDir temp("game_library_pbp");
	vfs::ParamSfo sfo;
	sfo.SetString("TITLE", "Packaged game");
	sfo.SetString("DISC_ID", "TEST00002");
	auto bytes = sfo.Serialize();
	std::vector<u8> pbp(40, 0);
	memcpy(pbp.data(), "\0PBP", 4);
	fstest::Put32LE(pbp.data() + 4, 0x10000);
	fstest::Put32LE(pbp.data() + 8, 40);
	for (int i = 1; i < 8; ++i) fstest::Put32LE(pbp.data() + 8 + i * 4, 40 + (u32)bytes.size());
	pbp.insert(pbp.end(), bytes.begin(), bytes.end());
	std::filesystem::create_directories(temp.Path() / "Package");
	CHECK(WriteBytes(temp.Path() / "Package/EBOOT.PBP", pbp));
	auto games = frontend::ScanGames(temp.Path());
	CHECK_EQ(games.size(), (size_t)1);
	if (!games.empty()) {
		CHECK(games[0].title == "Packaged game");
		CHECK(games[0].id == "TEST00002");
		CHECK(games[0].icon.empty());
	}
	bool threw = false;
	try { frontend::ScanGames(temp.Path() / "missing"); }
	catch (const std::filesystem::filesystem_error &) { threw = true; }
	CHECK(threw);
}
