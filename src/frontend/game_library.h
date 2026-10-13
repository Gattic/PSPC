// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/common.h"
#include <filesystem>
#include <string>
#include <vector>

namespace frontend {
struct GameInfo {
	std::filesystem::path path;
	std::string title, id;
	u64 size = 0;
	std::vector<u8> icon;
};
bool IsDisc(const std::filesystem::path &path);
bool DescribeGame(const std::filesystem::path &path, GameInfo &game);
std::vector<u8> ReadPic1(const std::filesystem::path &path);
// Filesystem failures throw filesystem_error; callers decide how to display them.
std::vector<GameInfo> ScanGames(const std::filesystem::path &directory);
}  // namespace frontend
