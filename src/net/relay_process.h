// SPDX-License-Identifier: GPL-2.0-or-later
//
// The relay for public game servers. A Cloudflare Tunnel (or any web proxy) carries only HTTP, so a
// game whose server is public - an infrastructure.json entry with a "tunnel" address - reaches it
// through a small Python program on this PC, server/pspc_gamespy/relay.py: it listens where the game
// is told the server is (127.77.0.1, the entry's "domains") and passes the game's TCP and UDP over
// WebSockets to the server (docs/gamespy-server.md, "Public server"). PSPC starts it when the game
// connects to the network; it ends with PSPC (--parent-pid) and, if another PSPC on this PC runs one
// already, takes over when that one ends.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hostnet {

struct RelayCommand {
	std::vector<std::string> args;      // the program and its arguments
	std::filesystem::path workingDir;   // where the relay's package is
};

// The command that runs the relay for the server at `url` (https://swss.pspc.online), for the
// PSPC whose process ID is parentPid. `resourceRoot` is the folder with data/ and server/. False,
// with the reason, when this PC has no Python (PSPC_PYTHON, python\ next to pspc.exe, or PATH) or
// the server's files.
bool BuildRelayCommand(const std::string &url, const std::filesystem::path &resourceRoot, int parentPid, RelayCommand *out,
                       std::string *why);

// Starts it, its output in servers/relay.log (next to the launcher's servers' logs).
bool StartRelay(const std::string &url, std::string *why);

}  // namespace hostnet
