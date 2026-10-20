// SPDX-License-Identifier: GPL-2.0-or-later
// The relay for public game servers (net/relay_process.h): the command that runs it, and starting it.
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "common/app_paths.h"
#include "common/fileutil.h"
#include "common/host.h"
#include "net/relay_process.h"
#include "test_env.h"
#include "test_fs_helpers.h"
#include "testing.h"

namespace {

void MakeExecutable(const fs::path &file, const std::string &text) {
	fs::create_directories(file.parent_path());
	WriteTextFile(file, text);
	chmod(file.c_str(), 0755);
}

// A folder with the relay's package, as an installed PSPC has it.
fs::path MakeResources(const fs::path &root) {
	WriteTextFile(root / "server/pspc_gamespy/relay.py", "# the relay\n");
	return root;
}

std::string ReadFile(const fs::path &p) {
	std::vector<u8> d;
	return ReadWholeFile(p, d) ? std::string(d.begin(), d.end()) : std::string();
}

bool WaitFor(const fs::path &file, const char *text) {
	for (int i = 0; i < 500; i++) {
		if (ReadFile(file).find(text) != std::string::npos) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

}  // namespace

TEST(relay_command_runs_the_relay_from_the_server_folder) {
	fstest::TempDir temp("relay_command");
	ScopedEnv python("PSPC_PYTHON", (temp.Path() / "mypython").c_str());
	hostnet::RelayCommand command;
	std::string why;
	CHECK(hostnet::BuildRelayCommand("https://swss.pspc.online", MakeResources(temp.Path() / "res"), 4242, &command, &why));
	const std::vector<std::string> expected = {(temp.Path() / "mypython").string(), "-u", "-B", "-X", "utf8", "-m", "pspc_gamespy",
	                                           "tunnel", "https://swss.pspc.online", "--parent-pid", "4242"};
	CHECK(command.args == expected);
	CHECK(command.workingDir == temp.Path() / "res/server");
}

TEST(relay_command_finds_python_on_the_path) {
	fstest::TempDir temp("relay_path");
	MakeExecutable(temp.Path() / "bin/python3", "#!/bin/sh\n");
	ScopedEnv python("PSPC_PYTHON", nullptr);
	ScopedEnv path("PATH", (temp.Path() / "empty").string().c_str());
	hostnet::RelayCommand command;
	std::string why;
	const fs::path resources = MakeResources(temp.Path() / "res");
	CHECK(!hostnet::BuildRelayCommand("https://x", resources, 1, &command, &why));      // no Python anywhere
	CHECK(why.find("Python") != std::string::npos);
	ScopedEnv withPython("PATH", ((temp.Path() / "empty").string() + ":" + (temp.Path() / "bin").string()).c_str());
	CHECK(hostnet::BuildRelayCommand("https://x", resources, 1, &command, &why));
	CHECK(command.args[0] == (temp.Path() / "bin/python3").string());
}

TEST(relay_command_needs_the_servers_files) {
	fstest::TempDir temp("relay_missing");
	ScopedEnv python("PSPC_PYTHON", "/bin/true");
	hostnet::RelayCommand command;
	std::string why;
	CHECK(!hostnet::BuildRelayCommand("https://x", temp.Path(), 1, &command, &why));
	CHECK(why.find("relay") != std::string::npos);
}

TEST(start_process_runs_in_its_folder_with_only_the_standard_streams) {
	fstest::TempDir temp("start_process");
	const fs::path log = temp.Path() / "logs/out.log";
	// A descriptor the child must not inherit (not close-on-exec, like the guest's sockets).
	int leaked[2];
	CHECK(pipe(leaked) == 0);
	MakeExecutable(temp.Path() / "child.sh", "#!/bin/sh\npwd\necho \"args: $1|$2\"\nls /proc/$$/fd | tr '\\n' ' '\necho\necho done\n");
	CHECK(hostsys::StartProcess({(temp.Path() / "child.sh").string(), "a b", "c"}, temp.Path() / "work", log) == false);   // no such folder
	fs::create_directories(temp.Path() / "work");
	CHECK(hostsys::StartProcess({(temp.Path() / "child.sh").string(), "a b", "c"}, temp.Path() / "work", log));
	CHECK(WaitFor(log, "done"));
	const std::string out = ReadFile(log);
	CHECK(out.find((temp.Path() / "work").string()) != std::string::npos);
	CHECK(out.find("args: a b|c") != std::string::npos);
	CHECK(out.find("0 1 ") != std::string::npos);                                  // its descriptors, listed by the shell
	CHECK(out.find(" " + std::to_string(leaked[0]) + " ") == std::string::npos);
	CHECK(out.find(" " + std::to_string(leaked[1]) + " ") == std::string::npos);
	close(leaked[0]);
	close(leaked[1]);
	CHECK(!hostsys::StartProcess({(temp.Path() / "nothing-here").string()}, temp.Path(), log));
}
#endif
