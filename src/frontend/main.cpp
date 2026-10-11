// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc: run a PSP executable (ELF/PRX), PBP or UMD image natively.
//
//   pspc [options] <file>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>

#include "common/fileutil.h"
#include "common/log.h"
#include "common/settings.h"
#include "net/adhoc_server.h"
#include "pspc_version.h"
#include "net/np_client.h"
#include "runtime/runtime.h"

// "<vblank>:<BUTTON[+BUTTON...]>[:<frames>]", e.g. "600:START" or "900:CROSS+UP:4".
static bool ParsePress(const std::string &spec, runtime::Options::Press &out) {
	static const struct {
		const char *name;
		u32 bit;
	} kButtons[] = {{"SELECT", 0x1},     {"START", 0x8},     {"UP", 0x10},       {"RIGHT", 0x20},
	                {"DOWN", 0x40},      {"LEFT", 0x80},     {"L", 0x100},       {"R", 0x200},
	                {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000}, {"CROSS", 0x4000}, {"SQUARE", 0x8000},
	                {"HOME", 0x10000},   {"NOTE", 0x800000}, {"SCREEN", 0x400000}, {"VOLUP", 0x100000},
	                {"VOLDOWN", 0x200000}};
	size_t c1 = spec.find(':');
	if (c1 == std::string::npos) return false;
	size_t c2 = spec.find(':', c1 + 1);
	out.vblank = (u32)strtoul(spec.c_str(), nullptr, 10);
	out.frames = c2 == std::string::npos ? 6 : (u32)strtoul(spec.c_str() + c2 + 1, nullptr, 10);
	std::string names = spec.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
	out.buttons = 0;
	size_t pos = 0;
	while (pos <= names.size()) {
		size_t end = names.find('+', pos);
		std::string n = names.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		for (char &ch : n) ch = (char)toupper((unsigned char)ch);
		bool found = false;
		for (const auto &b : kButtons)
			if (n == b.name) {
				out.buttons |= b.bit;
				found = true;
			}
		if (!found) return false;
		if (end == std::string::npos) break;
		pos = end + 1;
	}
	return out.buttons != 0 && out.frames != 0;
}

static void Usage() {
	fprintf(stderr,
	        "Usage: pspc [options] <EBOOT.PBP | game.iso | game.cso | program.prx | program.elf>\n"
	        "  --headless           no window/audio (tests, batch runs)\n"
	        "  --interp             interpreter only (ignore AOT-compiled modules)\n"
	        "  --aot-dir <dir>      directory of AOT module DLLs (default: <exe>/cache/aot)\n"
	        "  --fast-time          skip idle time instead of sleeping\n"
	        "  --timeout <sec>      stop after this many seconds\n"
	        "  --ram <32|64>        guest RAM size in MB (default 64)\n"
	        "  --large-mem          let user code use RAM beyond 24 MB (default: for the XMB and\n"
	        "                       games with MEMSIZE=1 in PARAM.SFO)\n"
	        "  --no-large-mem       keep user memory at 24 MB, also for the XMB\n"
	        "  --user-mem-end <addr> with large memory, end user memory there (e.g. 0x0BB00000)\n"
	        "  --strict-mem         treat bad guest memory accesses as fatal\n"
	        "  --memstick <dir>     memory stick root (default: <exe>/ms0)\n"
	        "  --plugins <dir>      M33 plugins folder laid over ms0: (default: the nearest plugins\\)\n"
	        "  --no-plugins         don't load M33 plugins\n"
	        "  --flash0 <dir>       flash0 root (default: <exe>/flash0)\n"
	        "  --host0 <dir>        host0: root (default: the program's folder)\n"
	        "  --umd <image>        insert a disc image while running a .prx/.elf/.pbp\n"
	        "  --osk-text <text>    headless: what the on-screen keyboard returns (a|b|c: one per keyboard in turn)\n"
	        "  --fw <ver>           firmware version reported to games, e.g. 6.60 or 4.01\n"
	        "  --scale <n>          window scale (default 2)\n"
	        "  --version            print PSPC's version\n"
	        "  --config <file>      settings file (default: <exe>/pspc.ini; \"none\": built-in defaults)\n"
	        "  --set <key>=<value>  override one setting of that file, e.g. --set fullscreen=1\n"
	        "  --log <level>        trace|debug|info|warn|error (default info)\n"
	        "  --log-file <path>    write the log to a file\n"
	        "  --test-display       report a display to pspautotests (default: none)\n"
	        "  --test-output <path> pspautotests' host0:/__testoutput.txt (.err, .finish) is <path>\n"
	        "  --test-as-psp        no emulator devices: pspautotests run as on a PSP (output: host0)\n"
	        "  --host0-raw-names    host0: names as the PC has them (psplink), not as FAT shows them\n"
	        "  --host0-extra <dir>  lay another folder under host0: (paths missing from host0 come from it)\n"
	        "  --edram-mirrors      VRAM mirrors 1 and 3 swizzled as the GE's depth layouts (implies --interp)\n"
	        "  --dcache-model       model the data cache, write-back as on hardware (implies --interp)\n"
	        "  --dump-frames <dir>  save the displayed frame as BMP every --dump-every VBlanks (60)\n"
	        "  --renderer <name>    GE renderer: software (default) or d3d11 (GPU; d3d11-warp: WARP device)\n"
	        "  --render-scale <n>   d3d11 internal resolution, 1-16 times the PSP's (default 1; 8 = 4K, 16 = 8K)\n"
	        "  --texture-filter <f> d3d11 texture filtering: auto (as the game sets it), nearest or linear\n"
	        "  --texture-upscale <n> d3d11 texture upscaling with xBR, 1 (off) - 4\n"
	        "  --press <v>:<B>[:n]  hold buttons B (e.g. START, CROSS+UP) for n VBlanks (6) from VBlank v\n"
	        "  --vsh                boot the firmware's XMB (tools/pspc_install_fw.py) instead of a file\n"
	        "  --recovery           boot M33's recovery menu (also: hold R while the XMB boots)\n"
	        "  --net-selftest <s>   run network test scenario <s> instead of a file (tools/net_selftest.py)\n"
	        "  --adhoc-server       run an ad hoc server (PPSSPP-compatible) until Ctrl+C instead of a game;\n"
	        "                       --adhoc-port <n> (default 27312), --adhoc-bind <addr> (default 0.0.0.0)\n"
	        "  --np <command>       PSPC Network account: register|signin --server <url> --id <online ID>,\n"
	        "                       signout, status, local [--id <name>] (sign in without a server),\n"
	        "                       friends, friend-add --id <online ID>, friend-remove --id <online ID>\n");
}

// pspc --adhoc-server [--adhoc-port <n>] [--adhoc-bind <addr>]: serves until Ctrl+C, boots nothing.
static int RunAdhocServer(int argc, char **argv) {
	adhocserver::Options so;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--adhoc-port" && i + 1 < argc) {
			int p = atoi(argv[++i]);
			if (p < 0 || p > 65535) {
				fprintf(stderr, "bad --adhoc-port %s\n", argv[i]);
				return 1;
			}
			so.port = (u16)p;
		} else if (a == "--adhoc-bind" && i + 1 < argc) {
			so.bindAddress = argv[++i];
		}
	}
	return adhocserver::RunCommandLine(so);
}

static u32 ParseFw(const std::string &s) {
	int major = 0, minor = 0;
	if (sscanf(s.c_str(), "%d.%d", &major, &minor) != 2) return 0x06060010;
	return ((u32)major << 24) | ((u32)(minor / 10) << 16) | ((u32)(minor % 10) << 8) | 0x10;
}

// pspc.ini -> options. The command-line options are applied afterwards and override these.
static void ApplySettings(const settings::Settings &s, runtime::Options &o) {
	o.windowScale = s.scale;
	o.fullscreen = s.fullscreen;
	o.vsync = s.vsync;
	o.aot = s.aot;
	o.firmwareVersion = ParseFw(s.fw);
	o.renderer = s.renderer;
	o.renderScale = s.renderScale;
	o.textureFilter = s.textureFilter;
	o.textureUpscale = s.textureUpscale;
	o.volume = s.mute ? 0 : s.volume;
	o.keyBindings = s.keys;
	o.padBindings = s.pads;
	o.padStick = s.padStick;
	o.gamesDir = s.gamesDir;
	o.plugins = s.plugins;
	o.pluginsDir = s.pluginsDir;
	o.network = s.network;
	o.adhocServer = s.adhocServer;
	o.npServer = s.npServer;
	o.npOnlineId = s.npOnlineId;
	o.npToken = s.npToken;
	o.infraDns = s.infraDns;
}

// The effective settings (after command-line overrides), at info level.
static void LogSettings(const runtime::Options &o) {
	u32 fw = o.firmwareVersion;
	LOGI("CONFIG", "scale=%d fullscreen=%d vsync=%d aot=%d fw=%u.%u%u volume=%d network=%d adhoc_server=%s",
	     o.windowScale, o.fullscreen, o.vsync, o.aot, fw >> 24, (fw >> 16) & 0xFF, (fw >> 8) & 0xFF, o.volume,
	     o.network, o.adhocServer.c_str());
	auto quoted = [](const std::string &v) { return v.find(' ') == std::string::npos ? v : "\"" + v + "\""; };
	std::string keys, pads;
	for (size_t i = 0; i < settings::kNumControls; i++) {
		keys += std::string(" ") + settings::kControls[i].name + "=" + quoted(o.keyBindings[i]);
		pads += std::string(" ") + settings::kControls[i].name + "=" + quoted(o.padBindings[i]);
	}
	LOGI("CONFIG", "keys:%s", keys.c_str());
	LOGI("CONFIG", "pad: stick=%s%s", o.padStick.c_str(), pads.c_str());
}

// A password from the console without echo, or a line of stdin when it isn't a console (scripts).
static std::string ReadPassword(const char *prompt) {
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode = 0;
	const bool console = GetConsoleMode(in, &mode) != 0;
	if (console) {
		fprintf(stderr, "%s", prompt);
		SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
	}
	char buf[256] = {};
	if (!fgets(buf, sizeof(buf), stdin)) buf[0] = 0;
	if (console) {
		SetConsoleMode(in, mode);
		fprintf(stderr, "\n");
	}
	std::string s = buf;
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	return s;
}

// pspc --np register|signin|signout|status|local [--server <url>] [--id <online ID>] [--country <cc>]
// [--config <file>]: PSPC Network account (the launcher's Network dialog does the same).
static int RunNpCommand(int argc, char **argv) {
	std::string command, server, onlineId, country = "us";
	std::string cfgPath = PathToUtf8(settings::DefaultPath());
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--np") command = next();
		else if (a == "--server") server = next();
		else if (a == "--id") onlineId = next();
		else if (a == "--country") country = next();
		else if (a == "--config") cfgPath = next();
	}
	settings::Settings s;
	settings::Load(fs::u8path(cfgPath), s);
	if (server.empty()) server = s.npServer;
	auto save = [&]() {
		if (!settings::Save(fs::u8path(cfgPath), s)) {
			fprintf(stderr, "Could not write %s\n", cfgPath.c_str());
			return false;
		}
		return true;
	};
	if (command == "local") {
		if (onlineId.empty()) onlineId = s.npOnlineId;
		s.npServer.clear();
		s.npToken.clear();
		s.npOnlineId = onlineId;
		if (!save()) return 1;
		printf("Games sign in locally as %s (no PSPC Network server).\n", onlineId.empty() ? "PSPCPlayer" : onlineId.c_str());
		return 0;
	}
	if (command == "register" || command == "signin") {
		if (server.empty() || onlineId.empty()) {
			fprintf(stderr, "usage: pspc --np %s --server <url> --id <online ID>\n", command.c_str());
			return 1;
		}
		const std::string password = ReadPassword("Password: ");
		if (command == "register" && ReadPassword("Again: ") != password) {
			fprintf(stderr, "The passwords don't match.\n");
			return 1;
		}
		np::NetworkClient client(server);
		np::Profile profile;
		std::string token;
		const np::Result r = command == "register" ? client.Register(onlineId, password, country, &profile, &token)
		                                           : client.SignIn(onlineId, password, &profile, &token);
		if (!r) {
			fprintf(stderr, "%s\n", r.message.c_str());
			return 1;
		}
		s.npServer = client.Server();
		s.npOnlineId = profile.onlineId;
		s.npToken = token;
		if (!save()) return 1;
		printf("%s %s on %s.\n", command == "register" ? "Created and signed in as" : "Signed in as", profile.onlineId.c_str(),
		       client.Server().c_str());
		return 0;
	}
	if (command == "signout") {
		if (!s.npToken.empty() && !server.empty()) np::NetworkClient(server, s.npToken).SignOut();
		s.npToken.clear();
		if (!save()) return 1;
		printf("Signed out.\n");
		return 0;
	}
	if (command == "status") {
		if (server.empty()) {
			printf("No PSPC Network server: games sign in locally as %s.\n", s.npOnlineId.empty() ? "PSPCPlayer" : s.npOnlineId.c_str());
			return 0;
		}
		if (s.npToken.empty()) {
			printf("Server %s; not signed in.\n", np::NormalizeServerUrl(server).c_str());
			return 1;
		}
		np::Profile profile;
		const np::Result r = np::NetworkClient(server, s.npToken).Me(&profile);
		if (!r) {
			printf("Server %s: %s\n", np::NormalizeServerUrl(server).c_str(), r.message.c_str());
			return 1;
		}
		printf("Signed in to %s as %s (country %s).\n", np::NormalizeServerUrl(server).c_str(), profile.onlineId.c_str(),
		       profile.country.c_str());
		return 0;
	}
	if (command == "friends" || command == "friend-add" || command == "friend-remove") {
		if (server.empty() || s.npToken.empty()) {
			fprintf(stderr, "Not signed in to a PSPC Network server (pspc --np signin).\n");
			return 1;
		}
		np::NetworkClient client(server, s.npToken);
		np::Result r;
		if (command != "friends") {
			if (onlineId.empty()) {
				fprintf(stderr, "usage: pspc --np %s --id <online ID>\n", command.c_str());
				return 1;
			}
			np::Friend f;
			r = command == "friend-add" ? client.AddFriend(onlineId, &f) : client.RemoveFriend(onlineId);
			if (!r) {
				fprintf(stderr, "%s\n", r.message.c_str());
				return 1;
			}
		}
		std::vector<np::Friend> list;
		r = client.Friends(&list);
		if (!r) {
			fprintf(stderr, "%s\n", r.message.c_str());
			return 1;
		}
		if (list.empty()) printf("No friends yet.\n");
		for (const np::Friend &f : list)
			printf("%-16s %s\n", f.onlineId.c_str(),
			       f.status == "friend" ? "friends" : f.status == "sent" ? "request sent" : "wants to be friends (friend-add to accept)");
		return 0;
	}
	fprintf(stderr, "usage: pspc --np register|signin|signout|status|local|friends|friend-add|friend-remove [--server <url>] [--id <online ID>]\n");
	return 1;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version")) {
			printf("PSPC %s\n", PSPC_VERSION);
			return 0;
		}
		if (!strcmp(argv[i], "--adhoc-server")) return RunAdhocServer(argc, argv);
		if (!strcmp(argv[i], "--np")) return RunNpCommand(argc, argv);
	}
	runtime::Options o;
	fs::path exeDir = ExecutableDir();
	o.aotDir = (exeDir / "cache" / "aot").string();
	o.memstickDir = (exeDir / "ms0").string();
	o.flash0Dir = (exeDir / "flash0").string();
	o.flash1Dir = (exeDir / "flash1").string();
	o.dataDir = (exeDir / "data").string();
	o.decryptTool = (exeDir / "pspc-decrypt.exe").string();
	o.decryptCacheDir = (exeDir / "cache" / "decrypted").string();
	o.screenshotDir = (exeDir / "screenshots").string();
	LogLevel level = LogLevel::Info;
	std::string logFile;

	// Settings: pspc.ini (or --config <file>), then --set key=value, then the other options.
	settings::Settings cfg;
	std::string cfgPath = PathToUtf8(settings::DefaultPath());
	bool cfgExplicit = false;
	std::vector<std::string> cfgSets, cfgWarnings;
	for (int i = 1; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--config")) {
			cfgPath = argv[++i];
			cfgExplicit = true;
		} else if (!strcmp(argv[i], "--set")) {
			cfgSets.push_back(argv[++i]);
		}
	}
	bool cfgLoaded = cfgPath != "none" && settings::Load(fs::u8path(cfgPath), cfg, &cfgWarnings);
	for (const std::string &kv : cfgSets) {
		std::string err = "want <key>=<value>";
		size_t eq = kv.find('=');
		if (eq == std::string::npos || !settings::Set(cfg, kv.substr(0, eq), kv.substr(eq + 1), &err)) {
			fprintf(stderr, "bad --set %s: %s\n", kv.c_str(), err.c_str());
			return 1;
		}
	}
	ApplySettings(cfg, o);

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) {
				Usage();
				exit(1);
			}
			return argv[++i];
		};
		if (a == "--config" || a == "--set") next();  // read above
		else if (a == "--headless") o.headless = true;
		else if (a == "--vsh") o.vsh = true;
		else if (a == "--return-to-vsh") o.returnToVsh = true;
		else if (a == "--recovery") o.vsh = o.recovery = true;
		else if (a == "--net-selftest") o.netSelftest = next();
		else if (a == "--interp") o.aot = false;
		else if (a == "--aot-dir") o.aotDir = next();
		else if (a == "--fast-time") o.fastTime = true;
		else if (a == "--timeout") o.timeoutSec = atoi(next().c_str());
		else if (a == "--ram") o.ramMB = atoi(next().c_str()) == 32 ? 32 : 64;
		else if (a == "--large-mem") o.largeUserMemory = 1;
		else if (a == "--no-large-mem") o.largeUserMemory = 0;
		else if (a == "--user-mem-end") o.userMemEnd = (u32)strtoul(next().c_str(), nullptr, 0);
		else if (a == "--strict-mem") o.strictMemory = true;
		else if (a == "--memstick") o.memstickDir = next();
		else if (a == "--flash0") o.flash0Dir = next();
		else if (a == "--flash1") o.flash1Dir = next();
		else if (a == "--no-plugins") o.plugins = false;
		else if (a == "--plugins") o.pluginsDir = next();
		else if (a == "--host0") o.host0Dir = next();
		else if (a == "--umd") o.umdImage = next();
		else if (a == "--vsh-args") o.vshArgsFile = next();
		else if (a == "--osk-text") o.oskText = next();
		else if (a == "--boot-path") o.bootPath = next();
		else if (a == "--boot-args") o.bootArgsFile = next();
		else if (a == "--fw") o.firmwareVersion = ParseFw(next());
		else if (a == "--scale") o.windowScale = atoi(next().c_str());
		else if (a == "--test-display") o.hasDisplayForTests = true;
		else if (a == "--test-output") o.testOutput = next();
		else if (a == "--test-as-psp") o.testAsPsp = true;
		else if (a == "--host0-raw-names") o.host0RawNames = true;
		else if (a == "--host0-extra") o.host0Extra = next();
		else if (a == "--edram-mirrors") {
			// Native code doesn't go through the transform.
			o.edramMirrors = true;
			o.aot = false;
		} else if (a == "--dcache-model") {
			o.dcacheModel = true;
			o.aot = false;
		}
		else if (a == "--dump-frames") o.dumpFramesDir = next();
		else if (a == "--dump-every") o.dumpEvery = atoi(next().c_str());
		else if (a == "--renderer") o.renderer = next();
		else if (a == "--render-scale") o.renderScale = atoi(next().c_str());
		else if (a == "--texture-filter") o.textureFilter = ToLower(next());
		else if (a == "--texture-upscale") o.textureUpscale = atoi(next().c_str());
		else if (a == "--press") {
			runtime::Options::Press p;
			std::string spec = next();
			if (!ParsePress(spec, p)) {
				fprintf(stderr, "bad --press %s (want <vblank>:<BUTTON[+BUTTON]>[:<frames>])\n", spec.c_str());
				return 1;
			}
			o.presses.push_back(p);
		}
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
	if (o.file.empty() && !o.vsh && o.netSelftest.empty()) {
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
	if (o.gamesDir.empty()) {
		// The launcher's Games folder: next to pspc.exe or in a parent (the repository's).
		for (fs::path p = exeDir; !p.empty() && p != p.root_path(); p = p.parent_path()) {
			if (fs::is_directory(p / "Games")) {
				o.gamesDir = (p / "Games").string();
				break;
			}
		}
	}
	if (o.pluginsDir.empty()) {
		// The plugins folder (seplugins\game.txt and the plugins' files), found like Games: the
		// nearest one with a seplugins folder, else the nearest one.
		for (int pass = 0; pass < 2 && o.pluginsDir.empty(); pass++) {
			for (fs::path p = exeDir; !p.empty() && p != p.root_path(); p = p.parent_path()) {
				if (fs::is_directory(pass == 0 ? p / "plugins" / "seplugins" : p / "plugins")) {
					o.pluginsDir = (p / "plugins").string();
					break;
				}
			}
		}
	}
	logging::Init(level, logFile.empty() ? nullptr : logFile.c_str());
	if (cfgLoaded) LOGI("CONFIG", "Settings from %s", cfgPath.c_str());
	else if (cfgExplicit && cfgPath != "none") LOGW("CONFIG", "Settings file %s not found; using defaults", cfgPath.c_str());
	else LOGI("CONFIG", "No settings file (%s); using defaults", cfgPath.c_str());
	for (const std::string &w : cfgWarnings) LOGW("CONFIG", "%s", w.c_str());
	LogSettings(o);
	int rc = runtime::Run(o);
	std::string next, bootPath, bootArgs;
	bool m33 = runtime::TakeRelaunch(next);
	bool loadExec = !m33 && runtime::TakeLoadExecRelaunch(bootPath, bootArgs);
	// Back to the XMB: a game the XMB launched exited (or the XMB restarts itself).
	bool toVsh = !m33 && !loadExec && runtime::TakeExitToVsh() && (o.vsh || o.returnToVsh);
	if (m33 || loadExec || toVsh) {
		// LoadExec: a fresh runtime (new process) for the new executable, with the same options.
		// M33 swaps the host file; plain sceKernelLoadExec boots a guest path with the original
		// mounts. From the XMB the game's file replaces --vsh, and its exit comes back to the XMB.
		std::wstring cmd = L"\"" + (exeDir / "pspc.exe").wstring() + L"\"";
		bool fileSeen = false;
		for (int i = 1; i < argc; i++) {
			std::string a = argv[i];
			if ((a == "--boot-path" || a == "--boot-args") && i + 1 < argc) {
				i++;
				continue;
			}
			if (a == "--vsh" || a == "--return-to-vsh" || a == "--recovery") continue;
			if (a == "--umd" && i + 1 < argc) {
				// The disc stays in the drive: through the game the XMB launched and back to the
				// XMB (an image from ms0:/ISO only stands in for it while its game runs).
				cmd += L" \"--umd\" \"" + fs::u8path(argv[++i]).wstring() + L"\"";
				continue;
			}
			if (a == "--vsh-args" && i + 1 < argc && m33) {
				i++;  // replaced by the XMB state of this launch (below)
				continue;
			}
			std::wstring arg = fs::u8path(a).wstring();
			if (!o.file.empty() && a == o.file) {
				fileSeen = true;
				if (toVsh) continue;
				if (m33) arg = fs::u8path(next).wstring();
			}
			cmd += L" \"" + arg + L"\"";
		}
		if (toVsh) cmd += L" --vsh";
		else if (m33 && !fileSeen) cmd += L" \"" + fs::u8path(next).wstring() + L"\"";
		if ((m33 || loadExec) && (o.vsh || o.returnToVsh)) cmd += L" --return-to-vsh";
		std::string vshArgs;
		if (m33 && runtime::TakeVshArgsFile(vshArgs)) cmd += L" --vsh-args \"" + fs::u8path(vshArgs).wstring() + L"\"";
		if (loadExec) {
			cmd += L" --boot-path \"" + fs::u8path(bootPath).wstring() + L"\"";
			if (!bootArgs.empty()) cmd += L" --boot-args \"" + fs::u8path(bootArgs).wstring() + L"\"";
		}
		fflush(stdout);
		fflush(stderr);
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION pi{};
		if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
			CloseHandle(pi.hThread);
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD code = 0;
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hProcess);
			if (!bootArgs.empty()) DeleteFileW(fs::u8path(bootArgs).wstring().c_str());
			return (int)code;
		}
	}
	return rc;
}
