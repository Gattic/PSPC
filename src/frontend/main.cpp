// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc: run a PSP executable (ELF/PRX), PBP or UMD image natively.
//
//   pspc [options] <file>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/fileutil.h"
#include "common/log.h"
#include "runtime/runtime.h"

static void Usage() {
	fprintf(stderr,
	        "Usage: pspc [options] <EBOOT.PBP | game.iso | game.cso | program.prx | program.elf>\n"
	        "  --headless           no window/audio (tests, batch runs)\n"
	        "  --interp             interpreter only (ignore AOT-compiled modules)\n"
	        "  --aot-dir <dir>      directory of AOT module DLLs (default: <exe>/cache/aot)\n"
	        "  --fast-time          skip idle time instead of sleeping\n"
	        "  --timeout <sec>      stop after this many seconds\n"
	        "  --ram <32|64>        guest RAM size in MB (default 64)\n"
	        "  --large-mem          let user code use RAM beyond 24 MB\n"
	        "  --strict-mem         treat bad guest memory accesses as fatal\n"
	        "  --memstick <dir>     memory stick root (default: <exe>/ms0)\n"
	        "  --flash0 <dir>       flash0 root (default: <exe>/flash0)\n"
	        "  --fw <ver>           firmware version reported to games, e.g. 6.60 or 4.01\n"
	        "  --scale <n>          window scale (default 2)\n"
	        "  --log <level>        trace|debug|info|warn|error (default info)\n"
	        "  --log-file <path>    write the log to a file\n"
	        "  --test-display       report a display to pspautotests (default: none)\n");
}

static u32 ParseFw(const std::string &s) {
	int major = 0, minor = 0;
	if (sscanf(s.c_str(), "%d.%d", &major, &minor) != 2) return 0x06060010;
	return ((u32)major << 24) | ((u32)(minor / 10) << 16) | ((u32)(minor % 10) << 8) | 0x10;
}

int main(int argc, char **argv) {
	runtime::Options o;
	fs::path exeDir = ExecutableDir();
	o.aotDir = (exeDir / "cache" / "aot").string();
	o.memstickDir = (exeDir / "ms0").string();
	o.flash0Dir = (exeDir / "flash0").string();
	o.dataDir = (exeDir / "data").string();
	o.decryptTool = (exeDir / "pspc-decrypt.exe").string();
	o.decryptCacheDir = (exeDir / "cache" / "decrypted").string();
	LogLevel level = LogLevel::Info;
	std::string logFile;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) {
				Usage();
				exit(1);
			}
			return argv[++i];
		};
		if (a == "--headless") o.headless = true;
		else if (a == "--interp") o.aot = false;
		else if (a == "--aot-dir") o.aotDir = next();
		else if (a == "--fast-time") o.fastTime = true;
		else if (a == "--timeout") o.timeoutSec = atoi(next().c_str());
		else if (a == "--ram") o.ramMB = atoi(next().c_str()) == 32 ? 32 : 64;
		else if (a == "--large-mem") o.largeUserMemory = true;
		else if (a == "--strict-mem") o.strictMemory = true;
		else if (a == "--memstick") o.memstickDir = next();
		else if (a == "--flash0") o.flash0Dir = next();
		else if (a == "--fw") o.firmwareVersion = ParseFw(next());
		else if (a == "--scale") o.windowScale = atoi(next().c_str());
		else if (a == "--test-display") o.hasDisplayForTests = true;
		else if (a == "--log") {
			std::string l = next();
			level = l == "trace" ? LogLevel::Trace
			        : l == "debug" ? LogLevel::Debug
			        : l == "warn"  ? LogLevel::Warn
			        : l == "error" ? LogLevel::Error
			                       : LogLevel::Info;
		} else if (a == "--log-file") logFile = next();
		else if (a == "-h" || a == "--help") {
			Usage();
			return 0;
		} else if (!a.empty() && a[0] == '-') {
			fprintf(stderr, "Unknown option %s\n", a.c_str());
			Usage();
			return 1;
		} else {
			o.file = a;
		}
	}
	if (o.file.empty()) {
		Usage();
		return 1;
	}
	if (!fs::exists(fs::path(o.dataDir) / "nids.csv")) {
		// Running from the build tree: use the repository's data directory.
		for (fs::path p = exeDir; !p.empty() && p != p.root_path(); p = p.parent_path()) {
			if (fs::exists(p / "data" / "nids.csv")) {
				o.dataDir = (p / "data").string();
				break;
			}
		}
	}
	logging::Init(level, logFile.empty() ? nullptr : logFile.c_str());
	return runtime::Run(o);
}
