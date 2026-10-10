// SPDX-License-Identifier: GPL-2.0-or-later
//
// PSPC's built-in ad hoc server (`pspc --adhoc-server`): the "pro ad hoc server" protocol of aemu and
// PPSSPP (net/adhoc_proto.h), so PSPC and PPSSPP clients can share it. It only introduces the players
// of a group to each other; game data flows directly between them.
//
// Behaviour (users, games, groups, the order of CONNECT / CONNECT_BSSID / SCAN replies, time-outs and
// which protocol errors drop a user) follows PPSSPP's Core/HLE/proAdhocServer.cpp, Copyright (c)
// 2014- PPSSPP Project, GPL-2.0-or-later, itself Coldbird's aemu server. Not ported: the product
// code database (crosslinks between regions of the same game), the status XML, UPnP.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/common.h"

namespace adhocserver {

struct Options {
	std::string bindAddress = "0.0.0.0";  // dotted IPv4
	u16 port = 27312;                      // 0 = any free port (tests)
	// Where log lines go (default: the PSPC log, tag ADHOCSRV).
	std::function<void(const std::string &)> log;
};

class Server {
public:
	Server();
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	// Opens the listening socket. False (with a message) if the address can't be bound.
	bool Listen(const Options &options, std::string *error);
	// Serves until Stop(). Call after Listen.
	void Run();
	// Listen + Run on a thread of its own.
	bool Start(const Options &options, std::string *error);
	// Ends Run (from any thread, also a console control handler), tells the players and closes their
	// connections. Joins the thread Start created.
	void Stop();

	// The port actually listened on, and the addresses players can use to reach it.
	u16 Port() const;
	std::vector<std::string> Addresses() const;
	// Connected users (logged in or not), groups of all games. Thread-safe snapshots.
	int UserCount() const;
	int GroupCount() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// `pspc --adhoc-server [--adhoc-port <n>] [--adhoc-bind <addr>]`: serves until Ctrl+C or the console
// closes. Returns the process exit code.
int RunCommandLine(const Options &options);

}  // namespace adhocserver
