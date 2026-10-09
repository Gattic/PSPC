// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "common/log.h"

namespace runtime {

struct Options {
	std::string file;          // host path of the executable / PBP / ISO
	bool headless = false;
	bool aot = true;
	std::string aotDir;        // directory with <sha1>.dll module builds
	bool fastTime = false;     // skip idle time (tests)
	int ramMB = 64;
	bool largeUserMemory = false;
	bool strictMemory = false;
	int timeoutSec = 0;        // stop after this many wall-clock seconds (0 = never)
	std::string memstickDir;
	std::string flash0Dir;
	std::string dataDir;       // contains nids.csv
	std::string decryptTool;
	std::string decryptCacheDir;
	u32 firmwareVersion = 0x06060010;
	int windowScale = 2;
	bool hasDisplayForTests = false;
};

// Boots the program and runs until it exits. Returns the exit code.
int Run(const Options &opts);

}  // namespace runtime
