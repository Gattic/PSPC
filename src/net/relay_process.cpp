// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/relay_process.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

#include "common/app_paths.h"
#include "common/fileutil.h"
#include "common/host.h"

namespace hostnet {

namespace {

bool IsFile(const fs::path &p) {
	std::error_code ec;
	return fs::is_regular_file(p, ec);
}

// The first of these names found in a folder of PATH (not Windows' Microsoft Store stand-ins, which
// open the Store instead of running Python).
fs::path FindOnPath(const std::vector<std::string> &names) {
	const char *path = std::getenv("PATH");
	if (!path) return {};
#ifdef _WIN32
	const char separator = ';';
#else
	const char separator = ':';
#endif
	std::string rest = path;
	for (size_t start = 0; start <= rest.size();) {
		size_t end = rest.find(separator, start);
		if (end == std::string::npos) end = rest.size();
		const std::string dir = rest.substr(start, end - start);
		start = end + 1;
		std::string lower = dir;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (dir.empty() || lower.find("windowsapps") != std::string::npos) continue;
		for (const std::string &name : names) {
			const fs::path candidate = fs::u8path(dir) / name;
			if (IsFile(candidate)) return candidate;
		}
	}
	return {};
}

fs::path FindPython(const fs::path &resourceRoot) {
	if (const char *chosen = std::getenv("PSPC_PYTHON"); chosen && *chosen) return fs::u8path(chosen);
#ifdef _WIN32
	// An installed PSPC brings its own, next to pspc.exe.
	for (const fs::path &dir : {ExecutableDir(), resourceRoot})
		if (IsFile(dir / "python" / "python.exe")) return dir / "python" / "python.exe";
	return FindOnPath({"python.exe"});
#else
	(void)resourceRoot;
	return FindOnPath({"python3", "python"});
#endif
}

}  // namespace

bool BuildRelayCommand(const std::string &url, const fs::path &resourceRoot, int parentPid, RelayCommand *out, std::string *why) {
	const fs::path server = resourceRoot / "server";
	if (!IsFile(server / "pspc_gamespy" / "relay.py")) {
		*why = "the relay's files (server/pspc_gamespy) aren't with this copy of PSPC";
		return false;
	}
	const fs::path python = FindPython(resourceRoot);
	if (python.empty()) {
		*why = "Python 3 isn't installed (the PSPC installer brings its own; or install it from python.org)";
		return false;
	}
	out->args = {PathToUtf8(python), "-u", "-B", "-X", "utf8", "-m", "pspc_gamespy", "tunnel", url, "--parent-pid", std::to_string(parentPid)};
	out->workingDir = server;
	return true;
}

bool StartRelay(const std::string &url, std::string *why) {
	RelayCommand command;
	if (!BuildRelayCommand(url, apppaths::ResourceRoot(), hostsys::ProcessId(), &command, why)) return false;
	const fs::path log = apppaths::StateRoot() / "servers" / "relay.log";
	std::error_code ec;
	if (fs::file_size(log, ec) > (4u << 20)) fs::rename(log, fs::path(log).replace_extension(".old.log"), ec);
	if (!hostsys::StartProcess(command.args, command.workingDir, log)) {
		*why = "couldn't start " + command.args[0];
		return false;
	}
	return true;
}

}  // namespace hostnet
