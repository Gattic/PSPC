// SPDX-License-Identifier: GPL-2.0-or-later
//
// The servers pspc-launcher runs on this PC (its Online page): the PSPC Network server
// (server/pspc_network: accounts and sign-in, the PlayStation Network stand-in), game servers
// (server/pspc_medius: Syphon Filter: Combat Ops; server/pspc_gamespy: GameSpy games) and the ad hoc
// server (pspc --adhoc-server).
// Each is a child process writing to a log file in the servers folder next to pspc.exe, with its
// database there too; they stop when the launcher closes (a job object kills them).
//
// The Python servers run with the Python that comes with PSPC (python\ next to pspc.exe, in an
// installed PSPC), else one installed on this PC.
//
// Also here: the servers' admin commands (accounts, missions, news, ...), this PC's LAN address,
// and redirects that send a game to a server (infrastructure.json next to pspc.exe).
#pragma once

#include <string>
#include <vector>

#include "common/fileutil.h"
#include "common/settings.h"

namespace servers {

enum class Kind { Network, Sfco, Adhoc, GameSpy };
constexpr int kKinds = 4;
inline int Index(Kind k) { return (int)k; }

enum class State { Stopped, Starting, Running, Failed };

// The game Kind::Sfco serves, and the server name it looks up.
constexpr const char *kSfcoDiscId = "NPUG80114";
constexpr const char *kSfcoDomain = "combatops.psp.online.scea.com";
constexpr int kAdhocPort = 27312;

// The games Kind::GameSpy serves. Their redirect sends every GameSpy name (*.gamespy.com) to a
// server; OpenSpy's public servers have the same names under openspy.net.
struct GameSpyGame {
	const char *discId, *name;
};
constexpr GameSpyGame kGameSpyGames[] = {{"ULUS10390", "Star Wars Battlefront: Elite Squadron"}};
constexpr const char *kGameSpyDomain = "*.gamespy.com";
constexpr const char *kOpenSpyTarget = "*.openspy.net";

// exeDir: where pspc.exe is (the servers folder and an installed PSPC's python\ and server\ are
// next to it).
void Init(const fs::path &exeDir);

// The Python the servers (and PSPC's Python tools) run with, as a command-line prefix: the quoted
// python.exe and any arguments it needs first; "" without one.
std::wstring PythonCommand();

// What's missing for a server to run ("" = nothing): Python or the server's code.
std::wstring Missing(Kind k);
fs::path DataDir();  // <exe dir>\servers
fs::path LogPath(Kind k);
// What a running server says about itself (Kind::GameSpy: who is online, the hosted games), as
// JSON; "" if it doesn't say.
std::string Status(Kind k);
int Port(Kind k, const settings::Settings &s);

// Starts a server with the settings (Kind::Sfco: umd is the game's disc image, for its official
// missions; empty = none). False with the reason in *error.
bool Start(Kind k, const settings::Settings &s, const std::wstring &umd, std::wstring *error);
void Stop(Kind k);
void StopAll();
// Updates and returns a server's state: Starting until it accepts connections, Failed if it exited
// on its own.
State Poll(Kind k);
bool AnyRunning();
unsigned ExitCode(Kind k);
// The last lines of a server's log (for an error message).
std::wstring LogTail(Kind k, int lines);

// Runs a Python server's admin command (python -m <module> --db <its database> <args>) and returns
// its output; false (output: the error) if it couldn't run or failed. input goes to its stdin.
bool Admin(Kind k, const std::vector<std::wstring> &args, std::string *out, const std::string &input = {});

// This PC's address on its network (the adapter the internet route goes through), "127.0.0.1"
// without one.
std::wstring LanAddress();
// The PSPC Network server this launcher runs: its address for other PCs (http://<LAN address>:port,
// or for this PC only http://127.0.0.1:port).
std::wstring NetworkUrl(const settings::Settings &s, bool forThisPc);

// Redirects (infrastructure.json next to pspc.exe): where a game's server name goes ("" = nowhere
// set), and setting it (empty address: remove the redirect).
std::string Redirect(const std::string &discId, const std::string &domain);
bool SetRedirect(const std::string &gameName, const std::string &discId, const std::string &domain, const std::string &address,
                 std::wstring *error);

}  // namespace servers
