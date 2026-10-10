// SPDX-License-Identifier: GPL-2.0-or-later
//
// End-to-end tests of the network libraries: `pspc --net-selftest <name>[:<arg>,<arg>...]` runs a
// test "program" written in C++ instead of a PSP executable. It runs on a guest thread and calls the
// libraries exactly as a game does - through their syscall stubs, with its data in guest memory and
// guest addresses for its handlers - so blocking calls, the scheduler, handler threads and the host
// sockets all take the real paths. (PSPC's toolchain can't build MIPS code, so this stands in for a
// homebrew test program.) Output goes to stdout; the exit code is the number of failed checks
// (0 = pass). tools/net_selftest.py drives the scenarios, including two-instance ad hoc runs.
#pragma once

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include "allegrex/ctx.h"
#include "common/common.h"

namespace net::selftest {

// A scenario returns the number of failed checks.
using Scenario = std::function<int(const std::vector<std::string> &args)>;
void Register(const char *name, const char *help, Scenario fn);
// Starts scenario `spec` ("name" or "name:arg1,arg2") on a new guest thread. Returns < 0 (after
// listing the scenarios) if there is no such scenario. The run ends when the scenario returns.
int Start(const std::string &spec);

// ---- for scenarios (they run on the selftest guest thread) ----

// Calls library function `nid` of `lib` through its syscall stub, like guest code. Arguments past
// the eighth go on the stack (PSP EABI). Returns $v0.
u32 Call(const char *lib, u32 nid, std::initializer_list<u32> args);
// Zeroed guest memory (user partition), freed when the run ends.
u32 Alloc(u32 size);
// A NUL-terminated copy of `s` in guest memory.
u32 Str(const std::string &s);
std::string ReadStr(u32 addr, u32 maxLen = 1024);
// A guest address that runs `fn` when guest code (a library) calls it. $a0.. hold the arguments;
// set $v0 for the result.
u32 Callback(std::function<void(Ctx *)> fn);
// sceKernelDelayThread.
void Delay(u32 us);
// sceKernelGetSystemTimeWide, in microseconds.
u64 NowUs();
void Print(const char *fmt, ...);

// Counts and prints checks: "[ok] what" / "[FAIL] what".
struct Checker {
	int failures = 0;
	bool Check(bool ok, const char *fmt, ...);
};

}  // namespace net::selftest
