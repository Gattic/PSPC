// SPDX-License-Identifier: GPL-2.0-or-later
//
// pspc.ini: the user settings next to pspc.exe (window, audio, input mapping, native code,
// firmware version). pspc reads it at startup (command-line options override it); the launcher's
// Settings dialog writes it. The file is plain "key=value" lines; a line starting with '#' or ';'
// is a comment.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"
#include "common/fileutil.h"

namespace settings {

// A PSP input that keyboard keys and controller buttons can be bound to.
struct Control {
	const char *name;         // ini key suffix: key.<name>, pad.<name>
	const char *label;        // shown by the launcher
	u32 button;               // PSP_CTRL_* bit; 0 for an analog stick direction
	int axis;                 // analog direction: 0 = x, 1 = y
	int dir;                  // analog direction: -1 = left/up, +1 = right/down
	const char *defaultKeys;  // SDL key names (as SDL_GetKeyName spells them), comma-separated
	const char *defaultPad;   // SDL game controller button/trigger names, comma-separated
};
extern const Control kControls[];
extern const size_t kNumControls;

// SDL game controller names a pad.<control> entry may use (SDL_GameControllerGetStringForButton
// spellings, plus the two triggers).
extern const char *const kPadNames[];
extern const size_t kNumPadNames;

struct Settings {
	int scale = 2;                  // window scale, 1-4
	bool fullscreen = false;        // borderless desktop fullscreen
	bool vsync = true;
	bool aot = true;                // use AOT-compiled module DLLs (false = interpreter only)
	std::string fw = "6.60";        // firmware version reported to games
	std::string renderer;           // "software", "d3d11", "d3d11-warp"; "" = $PSPC_RENDERER or software
	int renderScale = 0;            // d3d11 internal resolution 1-16 (8 = 4K, 16 = 8K); 0 = default (1)
	std::string textureFilter = "auto";  // d3d11: "auto" (as the game sets it), "nearest", "linear"
	int textureUpscale = 1;         // d3d11: 1 = off, 2-4 = textures upscaled with xBR
	std::string gamesDir;           // folder of game images the XMB lists ("" = the Games folder)
	bool plugins = true;            // load M33 plugins (seplugins/game.txt, vsh.txt, pops.txt)
	std::string pluginsDir;         // overlaid onto ms0: ("" = the plugins folder)
	int volume = 100;               // 0-100
	bool mute = false;
	std::string padStick = "both";  // stick(s) driving the PSP analog: both, left, right, none
	bool network = true;            // false: the WLAN switch reads off (no infrastructure or ad hoc)
	std::string adhocServer = "localhost";  // ad hoc server (PPSSPP's "pro adhoc server" protocol)
	// PSPC Network, the stand-in for the PlayStation Network (net/np_client.h, docs/pspc-network.md).
	std::string npServer;    // server URL; "" = sign in locally as npOnlineId, with no server
	std::string npOnlineId;  // the online ID: the account's (set by the launcher) or the local one
	std::string npToken;     // the server's sign-in token for that account (the launcher's Network dialog)
	std::string infraDns;    // DNS server for every game's online services ("" = per-game table)
	// One comma-separated binding list per kControls entry ("" = unbound).
	std::vector<std::string> keys, pads;
	Settings();
};

// The settings file of this installation: <directory of the running exe>\pspc.ini.
fs::path DefaultPath();

// Reads `file` over the values already in `s`. Returns false if the file can't be read.
// Unknown keys and bad values are skipped and described in `warnings`.
bool Load(const fs::path &file, Settings &s, std::vector<std::string> *warnings = nullptr);
// Applies one setting ("scale", "key.cross", ...). Returns false with a message for an unknown
// key or a bad value, leaving `s` unchanged.
bool Set(Settings &s, const std::string &key, const std::string &value, std::string *error = nullptr);
// The value of a setting as the file spells it ("" for an unknown key).
std::string Get(const Settings &s, const std::string &key);
// Every setting key, in file order.
std::vector<std::string> Keys();
// Writes every setting. An existing file keeps its comments, line order and unknown lines; keys
// it doesn't have yet are appended. A new file gets a commented template.
bool Save(const fs::path &file, const Settings &s);

// Binding lists: "Space,Right Shift" <-> {"Space", "Right Shift"}. The comma key is "Comma".
std::vector<std::string> SplitList(const std::string &value);
std::string JoinList(const std::vector<std::string> &names);

}  // namespace settings
