// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/settings.h"

#include <cstdio>
#include <cstdlib>

namespace settings {

// The defaults are the keys pspc has always used (README "Keyboard").
const Control kControls[] = {
	{"up", "Up", 0x000010, 0, 0, "Up", "dpup"},
	{"down", "Down", 0x000040, 0, 0, "Down", "dpdown"},
	{"left", "Left", 0x000080, 0, 0, "Left", "dpleft"},
	{"right", "Right", 0x000020, 0, 0, "Right", "dpright"},
	{"cross", "Cross", 0x004000, 0, 0, "Z", "a"},
	{"circle", "Circle", 0x002000, 0, 0, "X", "b"},
	{"square", "Square", 0x008000, 0, 0, "A", "x"},
	{"triangle", "Triangle", 0x001000, 0, 0, "S", "y"},
	{"l", "L", 0x000100, 0, 0, "Q", "leftshoulder,lefttrigger"},
	{"r", "R", 0x000200, 0, 0, "W", "rightshoulder,righttrigger"},
	{"start", "Start", 0x000008, 0, 0, "Return", "start"},
	{"select", "Select", 0x000001, 0, 0, "Space,Right Shift", "back"},
	{"home", "Home", 0x010000, 0, 0, "Escape", "guide"},
	// Kernel-only buttons games never see; M33 plugins use them (DarkFrost: VOL+ and VOL- together
	// open its menu, both sticks clicked on a controller).
	{"vol_up", "Volume +", 0x100000, 0, 0, "PageUp", "rightstick"},
	{"vol_down", "Volume -", 0x200000, 0, 0, "PageDown", "leftstick"},
	{"note", "Note", 0x800000, 0, 0, "N", "misc1"},
	{"screen", "Screen", 0x400000, 0, 0, "B", ""},
	{"analog_up", "Analog up", 0, 1, -1, "I", ""},
	{"analog_down", "Analog down", 0, 1, +1, "K", ""},
	{"analog_left", "Analog left", 0, 0, -1, "J", ""},
	{"analog_right", "Analog right", 0, 0, +1, "L", ""},
};
const size_t kNumControls = ArraySize(kControls);

const char *const kPadNames[] = {
	"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
	"rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
	"paddle3", "paddle4", "touchpad", "lefttrigger", "righttrigger",
};
const size_t kNumPadNames = ArraySize(kPadNames);

namespace {

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

bool ParseBool(const std::string &v, bool &out) {
	std::string l = ToLower(v);
	if (l == "1" || l == "true" || l == "yes" || l == "on") out = true;
	else if (l == "0" || l == "false" || l == "no" || l == "off") out = false;
	else return false;
	return true;
}

bool ParseInt(const std::string &v, int lo, int hi, int &out) {
	if (v.empty() || v.size() > 6) return false;
	for (char c : v)
		if (c < '0' || c > '9') return false;
	int n = atoi(v.c_str());
	if (n < lo || n > hi) return false;
	out = n;
	return true;
}

// "6.60": one or two digits, a dot, two digits.
bool ValidFirmware(const std::string &v) {
	size_t dot = v.find('.');
	if (dot == std::string::npos || dot == 0 || dot > 2 || v.size() != dot + 3) return false;
	for (size_t i = 0; i < v.size(); i++)
		if (i != dot && (v[i] < '0' || v[i] > '9')) return false;
	return true;
}

int ControlIndex(const std::string &name) {
	for (size_t i = 0; i < kNumControls; i++)
		if (name == kControls[i].name) return (int)i;
	return -1;
}

const char *B(bool v) { return v ? "1" : "0"; }

}  // namespace

Settings::Settings() {
	for (size_t i = 0; i < kNumControls; i++) {
		keys.push_back(kControls[i].defaultKeys);
		pads.push_back(kControls[i].defaultPad);
	}
}

fs::path DefaultPath() { return ExecutableDir() / "pspc.ini"; }

std::vector<std::string> SplitList(const std::string &value) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= value.size()) {
		size_t end = value.find(',', pos);
		std::string t = Trim(value.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
		if (!t.empty()) out.push_back(t);
		if (end == std::string::npos) break;
		pos = end + 1;
	}
	return out;
}

std::string JoinList(const std::vector<std::string> &names) {
	std::string s;
	for (const std::string &n : names) {
		if (!s.empty()) s += ',';
		s += n == "," ? "Comma" : n;
	}
	return s;
}

std::vector<std::string> Keys() {
	std::vector<std::string> k = {"scale",  "fullscreen", "vsync",  "aot",  "fw",
	                              "renderer", "render_scale", "games_dir", "plugins", "plugins_dir", "volume", "mute",
	                              "network", "adhoc_server", "np_server", "np_online_id", "np_token", "infra_dns"};
	for (size_t i = 0; i < kNumControls; i++) k.push_back(std::string("key.") + kControls[i].name);
	k.push_back("pad.stick");
	for (size_t i = 0; i < kNumControls; i++) k.push_back(std::string("pad.") + kControls[i].name);
	return k;
}

bool Set(Settings &s, const std::string &rawKey, const std::string &rawValue, std::string *error) {
	std::string key = ToLower(Trim(rawKey)), v = Trim(rawValue);
	auto fail = [&](const char *what) {
		if (error) *error = key + "=" + v + ": " + what;
		return false;
	};
	bool b;
	int n;
	if (key == "scale") {
		if (!ParseInt(v, 1, 4, n)) return fail("want 1-4");
		s.scale = n;
	} else if (key == "fullscreen" || key == "vsync" || key == "aot" || key == "mute" || key == "network") {
		if (!ParseBool(v, b)) return fail("want 0 or 1");
		(key == "fullscreen" ? s.fullscreen
		 : key == "vsync"    ? s.vsync
		 : key == "aot"      ? s.aot
		 : key == "network"  ? s.network
		                     : s.mute) = b;
	} else if (key == "adhoc_server") {
		if (v.empty() || v.find_first_of(" \t/") != std::string::npos) return fail("want a host name or address");
		s.adhocServer = v;
	} else if (key == "fw") {
		if (!ValidFirmware(v)) return fail("want a version like 6.60");
		s.fw = v;
	} else if (key == "renderer") {
		std::string l = ToLower(v);
		if (!l.empty() && l != "software" && l != "d3d11" && l != "d3d11-warp")
			return fail("want software, d3d11 or d3d11-warp");
		s.renderer = l;
	} else if (key == "games_dir") {
		s.gamesDir = v;
	} else if (key == "np_server") {
		s.npServer = v;
	} else if (key == "np_online_id") {
		s.npOnlineId = v;
	} else if (key == "np_token") {
		s.npToken = v;
	} else if (key == "infra_dns") {
		s.infraDns = v;
	} else if (key == "plugins_dir") {
		s.pluginsDir = v;
	} else if (key == "plugins") {
		if (!ParseBool(v, s.plugins)) return fail("want 0 or 1");
	} else if (key == "render_scale") {
		if (v.empty()) s.renderScale = 0;
		else if (ParseInt(v, 1, 4, n)) s.renderScale = n;
		else return fail("want 1-4");
	} else if (key == "volume") {
		if (ToLower(v) == "mute") s.mute = true;
		else if (ParseInt(v, 0, 100, n)) s.volume = n;
		else return fail("want 0-100 or mute");
	} else if (key == "pad.stick") {
		std::string l = ToLower(v);
		if (l != "both" && l != "left" && l != "right" && l != "none") return fail("want both, left, right or none");
		s.padStick = l;
	} else if (key.rfind("key.", 0) == 0 && ControlIndex(key.substr(4)) >= 0) {
		// Key names are checked by the host, which knows SDL's spellings.
		s.keys[ControlIndex(key.substr(4))] = JoinList(SplitList(v));
	} else if (key.rfind("pad.", 0) == 0 && ControlIndex(key.substr(4)) >= 0) {
		std::vector<std::string> names = SplitList(v);
		for (std::string &name : names) {
			name = ToLower(name);
			bool known = false;
			for (size_t i = 0; i < kNumPadNames; i++) known |= name == kPadNames[i];
			if (!known) return fail("unknown controller button");
		}
		s.pads[ControlIndex(key.substr(4))] = JoinList(names);
	} else {
		if (error) *error = "unknown setting " + key;
		return false;
	}
	return true;
}

std::string Get(const Settings &s, const std::string &key) {
	if (key == "scale") return std::to_string(s.scale);
	if (key == "fullscreen") return B(s.fullscreen);
	if (key == "vsync") return B(s.vsync);
	if (key == "aot") return B(s.aot);
	if (key == "fw") return s.fw;
	if (key == "renderer") return s.renderer;
	if (key == "games_dir") return s.gamesDir;
	if (key == "plugins") return B(s.plugins);
	if (key == "plugins_dir") return s.pluginsDir;
	if (key == "render_scale") return s.renderScale ? std::to_string(s.renderScale) : "";
	if (key == "volume") return std::to_string(s.volume);
	if (key == "mute") return B(s.mute);
	if (key == "network") return B(s.network);
	if (key == "adhoc_server") return s.adhocServer;
	if (key == "np_server") return s.npServer;
	if (key == "np_online_id") return s.npOnlineId;
	if (key == "np_token") return s.npToken;
	if (key == "infra_dns") return s.infraDns;
	if (key == "pad.stick") return s.padStick;
	if (key.size() > 4) {
		int i = ControlIndex(key.substr(4));
		if (i >= 0 && key.rfind("key.", 0) == 0) return s.keys[i];
		if (i >= 0 && key.rfind("pad.", 0) == 0) return s.pads[i];
	}
	return "";
}

bool Load(const fs::path &file, Settings &s, std::vector<std::string> *warnings) {
	std::vector<u8> data;
	if (!ReadWholeFile(file, data)) return false;
	std::string text(data.begin(), data.end());
	if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
	size_t pos = 0;
	for (int line = 1; pos < text.size(); line++) {
		size_t end = text.find('\n', pos);
		std::string l = Trim(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
		pos = end == std::string::npos ? text.size() : end + 1;
		if (l.empty() || l[0] == '#' || l[0] == ';') continue;
		size_t eq = l.find('=');
		std::string err;
		if (eq == std::string::npos) err = "not a key=value line";
		else Set(s, l.substr(0, eq), l.substr(eq + 1), &err);
		if (!err.empty() && warnings) warnings->push_back(PathToUtf8(file.filename()) + ":" + std::to_string(line) + ": " + err);
	}
	return true;
}

namespace {

std::string Template(const Settings &s) {
	std::string t;
	auto put = [&](const std::string &key) { t += key + "=" + Get(s, key) + "\n"; };
	t += "# PSPC settings. pspc.exe reads this at startup; command-line options override it.\n"
	     "# One key=value per line; lines starting with # are comments.\n\n"
	     "# Window: scale 1-4 (times 480x272), fullscreen 0/1 (borderless; F11 toggles), vsync 0/1.\n";
	put("scale");
	put("fullscreen");
	put("vsync");
	t += "\n# Native code: 1 = run AOT-compiled modules (cache\\aot) when built, 0 = interpreter only.\n";
	put("aot");
	t += "\n# Firmware version reported to games.\n";
	put("fw");
	t += "\n# Graphics: renderer software (default, the accuracy reference) or d3d11 (GPU; faster, and\n"
	     "# render_scale 1-4 renders at that many times the PSP's resolution).\n";
	put("renderer");
	put("render_scale");
	t += "\n# Folder of game images (ISO/CSO) the XMB lists besides ms0:/ISO; empty = pspc's Games folder.\n";
	put("games_dir");
	t += "\n# M33 plugins: 1 = load the PRX files listed in seplugins/game.txt, vsh.txt and pops.txt (a\n"
	     "# trailing 0/1 on a line, or conf.bin, turns one plugin off or on). plugins_dir is laid over the\n"
	     "# memory stick, so its seplugins/ and the plugins' own folders appear in ms0:; empty = pspc's\n"
	     "# plugins folder.\n";
	put("plugins");
	put("plugins_dir");
	t += "\n# Audio: volume 0-100; mute=1 silences the output.\n";
	put("volume");
	put("mute");
	t += "\n# Network: 1 = the virtual access point and ad hoc play work (WLAN switch on), 0 = offline.\n"
	     "# adhoc_server: the ad hoc server to meet other players through (PPSSPP-compatible; run one\n"
	     "# with pspc --adhoc-server). localhost when playing two pspc instances on one PC.\n";
	put("network");
	put("adhoc_server");
	t += "\n# PSPC Network, the PlayStation Network stand-in: np_server is the server (https://...); the\n"
	     "# launcher's Network button signs in and stores np_online_id and np_token (keep the token private).\n"
	     "# Without a server, games sign in locally as np_online_id. infra_dns: a DNS server that sends\n"
	     "# every game's online services to revival servers (empty = the per-game table in\n"
	     "# data/infrastructure.json).\n";
	put("np_server");
	put("np_online_id");
	put("np_token");
	put("infra_dns");
	t += "\n# Keyboard: key.<control>=<SDL key name>[,<another>]. Names as SDL spells them: A-Z, 0-9,\n"
	     "# F1-F10, Up, Down, Left, Right, Return, Space, Escape, Tab, Backspace, Left Shift, Right Shift,\n"
	     "# Left Ctrl, Right Ctrl, Left Alt, Right Alt, Keypad 0-9, Keypad Enter, Comma, ... (empty =\n"
	     "# unbound). F11 (fullscreen), F12 (screenshot) and Ctrl+Q (quit) are reserved.\n";
	for (size_t i = 0; i < kNumControls; i++) put(std::string("key.") + kControls[i].name);
	t += "\n# Controller: pad.<control>=<button>[,<another>] with SDL's names: a b x y back guide start\n"
	     "# leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 paddle1-4\n"
	     "# touchpad lefttrigger righttrigger. pad.stick: the stick(s) that drive the PSP analog stick\n"
	     "# (both = whichever is pushed further, left, right or none).\n";
	put("pad.stick");
	for (size_t i = 0; i < kNumControls; i++) put(std::string("pad.") + kControls[i].name);
	return t;
}

// Writes with CRLF line ends (Windows editors) through a sibling file that is then swapped in,
// so a failed write never leaves half a file.
bool WriteAtomic(const fs::path &file, const std::string &text) {
	std::string crlf;
	for (char c : text) {
		if (c == '\n') crlf += '\r';
		crlf += c;
	}
	fs::path tmp = file;
	tmp += ".tmp";
	if (!WriteTextFile(tmp, crlf)) return false;
	std::error_code ec, ignored;
	fs::rename(tmp, file, ec);
	if (ec) fs::remove(tmp, ignored);
	return !ec;
}

}  // namespace

bool Save(const fs::path &file, const Settings &s) {
	std::vector<u8> data;
	if (!ReadWholeFile(file, data)) return WriteAtomic(file, Template(s));
	std::string text(data.begin(), data.end()), out;
	bool bom = text.rfind("\xEF\xBB\xBF", 0) == 0;
	if (bom) {
		text.erase(0, 3);
		out = "\xEF\xBB\xBF";
	}
	std::vector<std::string> keys = Keys();
	std::vector<bool> written(keys.size(), false);
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		std::string raw = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = end == std::string::npos ? text.size() : end + 1;
		if (!raw.empty() && raw.back() == '\r') raw.pop_back();
		std::string l = Trim(raw);
		size_t eq = l.find('=');
		if (!l.empty() && l[0] != '#' && l[0] != ';' && eq != std::string::npos) {
			std::string key = ToLower(Trim(l.substr(0, eq)));
			for (size_t i = 0; i < keys.size(); i++)
				if (keys[i] == key) {
					raw = key + "=" + Get(s, key);
					written[i] = true;
				}
		}
		out += raw + "\n";
	}
	for (size_t i = 0; i < keys.size(); i++)
		if (!written[i]) out += keys[i] + "=" + Get(s, keys[i]) + "\n";
	return WriteAtomic(file, out);
}

}  // namespace settings
