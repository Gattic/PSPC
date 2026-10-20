// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend/native_cache.h"
#include "common/fileutil.h"
#include "common/host.h"
#include "common/json.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace frontend {
namespace fs = std::filesystem;
namespace {
std::string Text(const fs::path &path) {
	std::vector<u8> bytes;
	if (!ReadWholeFile(path, bytes))
		return {};
	return std::string(bytes.begin(), bytes.end());
}
} // namespace
NativeInfo InspectNativeCache(const fs::path &cache, const GameInfo &game, u32 version) {
	NativeInfo result;
	auto text = Text(cache / "games.json");
	if (text.empty())
		return result;
	auto root = json::Value::Parse(text, &result.error);
	if (!result.error.empty())
		return result;
	if (root["version"].Number() != 1 || !root["games"].IsObject()) {
		result.error = "Invalid native cache index";
		return result;
	}
	const json::Value *entry = nullptr;
	std::error_code ec;
	for (const auto &[path, candidate] : root["games"].Members()) {
		if (fs::equivalent(game.path, fs::u8path(path), ec) || game.path == fs::u8path(path)) {
			entry = &candidate;
			break;
		}
	}
	if (!entry && !game.id.empty()) {
		for (const auto &[path, candidate] : root["games"].Members()) {
			if (candidate["disc_id"].String() == game.id && candidate["size"].Number(-1) == (double)game.size) {
				entry = &candidate;
				break;
			}
		}
	}
	if (!entry || !(*entry)["modules"].IsArray())
		return result;
	char wanted[16];
	snprintf(wanted, sizeof(wanted), "0x%08x", version);
	std::set<std::string> seen;
	int stale = 0;
	for (const auto &module : (*entry)["modules"].Array()) {
		const auto &sha = module["sha1"].String();
		if (sha.size() != 40 ||
			!std::all_of(sha.begin(), sha.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
			!module["ok"].Bool()) {
			++result.failed;
			continue;
		}
		if (!seen.insert(sha).second)
			continue;
		++result.total;
		auto binary = cache / (sha + hostsys::ModuleSuffix);
		if (!fs::is_regular_file(binary, ec))
			continue;
		auto stamp = Text(cache / (sha + ".semver"));
		while (!stamp.empty() && std::isspace((unsigned char)stamp.back()))
			stamp.pop_back();
		auto hints = cache / (sha + ".hints");
		bool fresh = stamp == wanted;
		if (fs::exists(hints, ec))
			fresh &= fs::last_write_time(hints, ec) <= fs::last_write_time(binary, ec);
		if (fresh)
			++result.built;
		else
			++stale;
		result.pspcChanged |= stamp != wanted;
	}
	if (result.built && result.built == result.total && !result.failed)
		result.state = Native::Built;
	else if (result.built)
		result.state = Native::Partial;
	else if (stale)
		result.state = Native::Stale;
	return result;
}
const char *NativeLabel(Native state) {
	switch (state) {
	case Native::Built:
		return "NATIVE";
	case Native::Partial:
		return "PARTIAL";
	case Native::Stale:
		return "REBUILD";
	default:
		return "NOT BUILT";
	}
}
} // namespace frontend
