// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _WIN32
#include "frontend/child_process.h"
#include "test_fs_helpers.h"
#include "testing.h"
#include <chrono>
#include <thread>

namespace {
void Wait(frontend::ChildProcess &child) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (child.Running() && std::chrono::steady_clock::now() < deadline) {
		child.Poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(!child.Running());
}
} // namespace
TEST(launcher_child_argv_input_failure_and_bounded_output) {
	fstest::TempDir temp("launcher_child");
	frontend::ChildProcess child;
	std::string error;
	CHECK(child.Start({"/bin/sh", "-c", "printf '%s' \"$1\"; cat", "fixture", "space ; λ"}, temp.Path(),
					  temp.Path() / "log", &error, " input"));
	Wait(child);
	CHECK(child.ExitCode() == 0);
	CHECK(child.Output().find("space ; λ input") != std::string::npos);
	CHECK(!child.Start({"/not/a/program"}, temp.Path(), {}, &error));
	CHECK(!error.empty());
	CHECK(child.Start({"python3", "-c", "print('x'*600000)"}, temp.Path(), {}, &error));
	Wait(child);
	CHECK(child.ExitCode() == 0);
	CHECK(child.Output().size() <= 256 * 1024);
	// A child closing stdin before the parent finishes sending cannot SIGPIPE the launcher.
	CHECK(child.Start({"/bin/true"}, temp.Path(), {}, &error, std::string(100000, 'x')));
	Wait(child);
	CHECK(child.ExitCode() == 0);
}
TEST(launcher_child_cancel_and_close_kill_owned_group) {
	fstest::TempDir temp("launcher_cancel");
	std::string error;
	frontend::ChildProcess child;
	CHECK(child.Start({"/bin/sh", "-c", "trap '' TERM; sleep 30 & wait"}, temp.Path(), {}, &error));
	child.Cancel();
	Wait(child);
	CHECK(child.Cancelled());
	CHECK(child.ExitCode() != 0);
	{
		frontend::ChildProcess closing;
		CHECK(closing.Start({"/bin/sh", "-c", "sleep 30 & wait"}, temp.Path(), {}, &error));
	}
}
#endif
