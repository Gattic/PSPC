// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/app_paths.h"
#include <cstdlib>
#include <stdexcept>

namespace apppaths {
#ifndef _WIN32
namespace {
fs::path XdgRoot(const char *variable, const char *fallback) {
	if (const char *value = std::getenv(variable); value && fs::path(value).is_absolute())
		return fs::path(value) / "pspc";
	// XDG specifies that empty/relative overrides are ignored, never resolved against cwd.
	const char *home = std::getenv("HOME");
	if (!home || !fs::path(home).is_absolute())
		throw std::runtime_error(std::string("Set an absolute HOME or ") + variable + " for PSPC user data");
	return fs::path(home) / fallback / "pspc";
}
} // namespace
#endif
fs::path ConfigRoot() {
#ifdef _WIN32
	return ExecutableDir();
#else
	return XdgRoot("XDG_CONFIG_HOME", ".config");
#endif
}
fs::path DataRoot() {
#ifdef _WIN32
	return ExecutableDir();
#else
	return XdgRoot("XDG_DATA_HOME", ".local/share");
#endif
}
fs::path CacheRoot() {
#ifdef _WIN32
	return ExecutableDir() / "cache";
#else
	return XdgRoot("XDG_CACHE_HOME", ".cache");
#endif
}
fs::path StateRoot() {
#ifdef _WIN32
	return ExecutableDir();
#else
	return XdgRoot("XDG_STATE_HOME", ".local/state");
#endif
}
fs::path DataPath(const std::string &path) { return DataRoot() / fs::path(std::u8string(path.begin(), path.end())); }
fs::path ResourceRoot(const fs::path &exeDir) {
	for (auto path = exeDir; !path.empty(); path = path.parent_path()) {
		if (fs::is_regular_file(path / "data/nids.csv")) return path;
		if (path == path.root_path()) break;
	}
	return exeDir; // Missing resources are diagnosed by the consumer, not borrowed from cwd.
}
} // namespace apppaths
