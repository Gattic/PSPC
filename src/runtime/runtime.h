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
	// User memory beyond 24 MB: 1 (--large-mem) or 0 (--no-large-mem); -1 (default): on a 64 MB
	// model for the XMB, whose browser and other plugins use the PSP-2000's extra RAM, and for games
	// whose PARAM.SFO has MEMSIZE = 1.
	int largeUserMemory = -1;
	u32 userMemEnd = 0;  // --user-mem-end: with large memory, where user memory stops (0 = end of RAM)
	bool strictMemory = false;
	int timeoutSec = 0;        // stop after this many wall-clock seconds (0 = never)
	u32 exitVblank = 0;        // stop after this VBlank, a point in game time (0 = never)
	std::string memstickDir;
	std::string flash0Dir;
	std::string flash1Dir;
	std::string host0Dir;      // host0: root (default: the boot file's folder)
	std::string umdImage;      // disc image to insert when booting a non-disc executable
	std::string bootPath;      // LoadExec relaunch: guest path to boot instead of the file's own
	std::string bootArgsFile;  // LoadExec relaunch: argument block for the booted module
	bool plugins = true;      // load ms0:/seplugins/game.txt (M33)
	std::string pluginsDir;    // overlaid onto ms0: (seplugins/, the plugins' own folders); "" = none
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
	// pspautotests' host0:/__testoutput.txt (and .err, .finish) are this file (io::SetTestOutput).
	std::string testOutput;
	bool testAsPsp = false;  // no emulator devices: pspautotests take the path they take on a PSP
	bool host0RawNames = false;  // io::SetHost0RawNames
	std::string host0Extra;      // a folder laid under host0: (io::AddHostFallback)
	bool edramMirrors = false;  // mem::g_edramMirrors (interpreter only)
	bool dcacheModel = false;   // dcache::g_enabled (interpreter only)
	std::string dumpFramesDir;  // write the displayed frame as BMP here every dumpEvery VBlanks
	int dumpEvery = 60;
	// GE renderer: "software" (the default, the accuracy reference), "d3d11" or "d3d11-warp" (the
	// optional GPU backend, src/gpu/hw). Empty: $PSPC_RENDERER, else software. renderScale (1-16): the
	// GPU backend's internal resolution (8 = 4K, 16 = 8K); 0: $PSPC_RENDER_SCALE, else 1.
	std::string renderer;
	int renderScale = 0;
	// The GPU backend's textures: filtering "auto" (as the game sets it), "nearest" or "linear"; upscaling
	// 1 (off) - 4 times with xBR.
	std::string textureFilter = "auto";
	int textureUpscale = 1;
	// Scripted input: hold `buttons` (PSP_CTRL_* bits) for `frames` VBlanks from VBlank `vblank`.
	struct Press {
		u32 vblank, buttons, frames;
	};
	std::vector<Press> presses;
	std::string oskText;  // headless: what the on-screen keyboard returns
	std::string gamesDir;  // host folder whose UMD images the XMB lists besides ms0:/ISO
	// --vsh-args: the XMB's vshmain_args, kept through a game it launched (deleted once read).
	std::string vshArgsFile;
	// Networking (pspc.ini network=, adhoc_server=): false = the WLAN switch reads off. The ad hoc
	// server is a host name or address, optionally with :port (default 27312).
	bool network = true;
	std::string adhocServer = "localhost";
	// PSPC Network (pspc.ini np_server=, np_online_id=, np_token=) and online-service redirects
	// (infra_dns=, data/infrastructure.json).
	std::string npServer, npOnlineId, npToken, infraDns;
	// Run a network selftest scenario ("name[:args]", hle/modules/net_selftest.h) instead of a file.
	std::string netSelftest;
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
// After Run() relaunched from the XMB: the file holding the XMB's vshmain_args (--vsh-args).
bool TakeVshArgsFile(std::string &file);

}  // namespace runtime
