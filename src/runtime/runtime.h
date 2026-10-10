// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "common/log.h"

namespace runtime {

struct Options {
	std::string file;          // host path of the executable / PBP / ISO (empty with vsh)
	bool vsh = false;          // boot the firmware's XMB (flash0:/vsh/module) instead of a game
	bool returnToVsh = false;  // a game launched from the XMB: its exit restarts the XMB
	bool recovery = false;     // with vsh: M33's recovery menu instead of the XMB (R held at boot)
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
	std::string flash1Dir;
	std::string host0Dir;      // host0: root (default: the boot file's folder)
	std::string umdImage;      // disc image to insert when booting a non-disc executable
	std::string bootPath;      // LoadExec relaunch: guest path to boot instead of the file's own
	std::string bootArgsFile;  // LoadExec relaunch: argument block for the booted module
	bool plugins = true;      // load ms0:/seplugins/game.txt (M33)
	std::string dataDir;       // contains nids.csv
	std::string decryptTool;
	std::string decryptCacheDir;
	u32 firmwareVersion = 0x06060010;
	int windowScale = 2;
	// Window, audio and input (pspc.ini; see common/settings.h and host::Config).
	bool fullscreen = false;
	bool vsync = true;
	int volume = 100;  // 0-100 (0 = muted)
	std::vector<std::string> keyBindings, padBindings;  // per settings::kControls; empty = defaults
	std::string padStick = "left";
	std::string screenshotDir;  // F12 (default: <exe>/screenshots)
	bool hasDisplayForTests = false;
	std::string dumpFramesDir;  // write the displayed frame as BMP here every dumpEvery VBlanks
	int dumpEvery = 60;
	// GE renderer: "software" (the default, the accuracy reference), "d3d11" or "d3d11-warp" (the
	// optional GPU backend, src/gpu/hw). Empty: $PSPC_RENDERER, else software. renderScale (1-4): the
	// GPU backend's internal resolution; 0: $PSPC_RENDER_SCALE, else 1.
	std::string renderer;
	int renderScale = 0;
	// Scripted input: hold `buttons` (PSP_CTRL_* bits) for `frames` VBlanks from VBlank `vblank`.
	struct Press {
		u32 vblank, buttons, frames;
	};
	std::vector<Press> presses;
	std::string oskText;  // headless: what the on-screen keyboard returns
};

// Boots the program and runs until it exits. Returns the exit code.
int Run(const Options &opts);
// After Run(): an M33 LoadExec asked to continue with this host file.
bool TakeRelaunch(std::string &hostFile);
// After Run(): the program called sceKernelLoadExec; restart booting this guest path with the
// argument block stored in argsFile ("" for none). See Options::bootPath.
bool TakeLoadExecRelaunch(std::string &pspPath, std::string &argsFile);
// After Run(): the program asked to go back to the XMB (a game launched from it exited).
bool TakeExitToVsh();

}  // namespace runtime
