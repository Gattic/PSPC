// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/firmware.h"
#include <algorithm>
#include <stdexcept>

namespace firmware {
namespace {
void NoSymlink(const fs::path &path) {
	if (fs::is_symlink(fs::symlink_status(path)))
		throw std::runtime_error("Firmware paths must not be symbolic links: " + PathToUtf8(path));
}
}
bool ValidName(const std::string &name) {
	auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
	if (name.empty() || name.size() > 64 || !alnum(name.front()) || !alnum(name.back())) return false;
	for (char c : name)
		if (!alnum(c) && c != '.' && c != '-' && c != '_') return false;
	// Windows device names remain reserved even with an extension.
	auto stem = name.substr(0, name.find('.'));
	if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
	if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9') return false;
	return true;
}
fs::path Profile(const fs::path &dataRoot, const std::string &name) {
	if (!ValidName(name)) throw std::runtime_error("Use a firmware name of 1-64 lowercase letters, digits, dots, hyphens or underscores, starting and ending with a letter or digit.");
	auto store = dataRoot / "firmware";
	NoSymlink(store);
	NoSymlink(store / name);
	return store / name;
}
bool Installed(const fs::path &dataRoot, const std::string &name) {
	return fs::is_regular_file(Profile(dataRoot, name) / "flash0/vsh/module/vshmain.prx");
}
std::vector<std::string> List(const fs::path &dataRoot) {
	auto store = Profile(dataRoot, "default").parent_path();
	std::vector<std::string> names;
	if (!fs::exists(store)) return names;
	for (const auto &entry : fs::directory_iterator(store)) {
		auto name = PathToUtf8(entry.path().filename());
		if (ValidName(name) && !entry.is_symlink() && entry.is_directory() && Installed(dataRoot, name))
			names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	return names;
}
void Migrate(const fs::path &dataRoot, bool migrateFlash0, bool migrateFlash1) {
	std::vector<const char *> roots;
	if (migrateFlash0) roots.push_back("flash0");
	if (migrateFlash1) roots.push_back("flash1");
	if (roots.empty()) return;
	auto target = Profile(dataRoot, "default");
	// Check every requested destination before moving any tree. Partial migration can resume.
	for (const char *flash : roots) {
		NoSymlink(dataRoot / flash);
		if (fs::exists(dataRoot / flash) && fs::exists(target / flash))
			throw std::runtime_error("Cannot migrate firmware: both " + PathToUtf8(dataRoot / flash) + " and " + PathToUtf8(target / flash) + " exist. Move one aside first.");
	}
	for (const char *flash : roots) {
		if (!fs::exists(dataRoot / flash)) continue;
		fs::create_directories(target);
		fs::rename(dataRoot / flash, target / flash);
	}
}
void Select(const fs::path &dataRoot, const fs::path &ini, settings::Settings &config, const std::string &name) {
	if (!Installed(dataRoot, name)) throw std::runtime_error("Firmware is not installed: " + name);
	// Preserve settings changed by another dialog or process since the manager opened.
	auto updated = config;
	settings::Load(ini, updated);
	updated.firmware = name;
	if (!settings::Save(ini, updated)) throw std::runtime_error("Cannot save active firmware selection.");
	config = std::move(updated);
}
void Delete(const fs::path &dataRoot, const std::string &active, const std::string &name) {
	if (name == active) throw std::runtime_error("Switch to another firmware before deleting the active version.");
	auto path = Profile(dataRoot, name);
	if (!Installed(dataRoot, name)) throw std::runtime_error("Firmware is not installed: " + name);
	// remove_all removes links themselves, never their targets. The store/profile ancestors
	// were checked above so this cannot descend through a linked profile into unrelated data.
	fs::remove_all(path);
}
} // namespace firmware
