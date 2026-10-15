// SPDX-License-Identifier: GPL-2.0-or-later
// pspc.ini parsing and writing (common/settings.h).
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "common/fileutil.h"
#include "common/settings.h"
#include "testing.h"

namespace {

fs::path TempIni(const char *name) {
#ifdef _WIN32
	const int pid = _getpid();
#else
	const int pid = getpid();
#endif
	fs::path dir = fs::temp_directory_path() / ("pspc_settings_test_" + std::to_string(pid));
	std::error_code ec;
	fs::create_directories(dir, ec);
	fs::path p = dir / name;
	fs::remove(p, ec);
	return p;
}

std::string ReadAll(const fs::path &p) {
	std::vector<u8> d;
	ReadWholeFile(p, d);
	return std::string(d.begin(), d.end());
}

int Index(const char *control) {
	for (size_t i = 0; i < settings::kNumControls; i++)
		if (std::string(settings::kControls[i].name) == control) return (int)i;
	return -1;
}

}  // namespace

TEST(settings_defaults_are_the_classic_keys) {
	settings::Settings s;
	CHECK(s.scale == 2 && !s.fullscreen && s.vsync && s.aot && s.fw == "6.60" && s.volume == 100 && !s.mute);
	CHECK(s.keys[Index("cross")] == "Z");
	CHECK(s.keys[Index("circle")] == "X");
	CHECK(s.keys[Index("select")] == "Space,Right Shift");
	CHECK(s.keys[Index("home")] == "Escape");
	CHECK(s.keys[Index("analog_left")] == "J");
	CHECK(s.pads[Index("l")] == "leftshoulder,lefttrigger");
	CHECK(s.pads[Index("analog_up")].empty());
}

TEST(settings_set_validates) {
	settings::Settings s;
	std::string err;
	CHECK(settings::Set(s, "Scale", " 3 ", &err) && s.scale == 3);
	CHECK(!settings::Set(s, "scale", "5", &err) && s.scale == 3 && !err.empty());
	CHECK(settings::Set(s, "fullscreen", "yes") && s.fullscreen);
	CHECK(!settings::Set(s, "vsync", "maybe"));
	CHECK(settings::Set(s, "fw", "5.50") && s.fw == "5.50");
	CHECK(!settings::Set(s, "fw", "6.6") && !settings::Set(s, "fw", "x.yz") && s.fw == "5.50");
	CHECK(settings::Set(s, "firmware", "4.01-m33") && s.firmware == "4.01-m33");
	CHECK(!settings::Set(s, "firmware", "../outside") && s.firmware == "4.01-m33");
	CHECK(settings::Get(s, "firmware") == "4.01-m33");
	CHECK(settings::Set(s, "volume", "0") && s.volume == 0);
	CHECK(settings::Set(s, "volume", "mute") && s.mute && s.volume == 0);
	CHECK(!settings::Set(s, "volume", "101"));
	CHECK(settings::Set(s, "key.cross", " K , Comma ") && s.keys[Index("cross")] == "K,Comma");
	CHECK(settings::Set(s, "key.start", "") && s.keys[Index("start")].empty());
	CHECK(settings::Set(s, "pad.circle", "A,RightTrigger") && s.pads[Index("circle")] == "a,righttrigger");
	CHECK(!settings::Set(s, "pad.circle", "nope") && s.pads[Index("circle")] == "a,righttrigger");
	CHECK(settings::Set(s, "pad.stick", "Right") && s.padStick == "right");
	CHECK(!settings::Set(s, "key.nosuch", "A") && !settings::Set(s, "bogus", "1"));
	CHECK(settings::Set(s, "render_scale", "16") && s.renderScale == 16);
	CHECK(!settings::Set(s, "render_scale", "17") && !settings::Set(s, "render_scale", "0") && s.renderScale == 16);
	CHECK(settings::Set(s, "texture_filter", "Linear") && s.textureFilter == "linear");
	CHECK(!settings::Set(s, "texture_filter", "bicubic") && s.textureFilter == "linear");
	CHECK(settings::Set(s, "texture_upscale", "4") && s.textureUpscale == 4);
	CHECK(!settings::Set(s, "texture_upscale", "5") && s.textureUpscale == 4);
	CHECK(settings::Get(s, "texture_upscale") == "4" && settings::Get(s, "texture_filter") == "linear");
	// The servers the launcher runs.
	CHECK(!s.hostNetwork && s.hostNetworkPort == 8423 && s.hostNetworkLan && s.hostSfcoAllowLocal);
	CHECK(settings::Set(s, "host_network", "1") && s.hostNetwork);
	CHECK(settings::Set(s, "host_network_port", "9000") && s.hostNetworkPort == 9000);
	CHECK(!settings::Set(s, "host_network_port", "70000") && s.hostNetworkPort == 9000);
	CHECK(settings::Set(s, "host_sfco_address", "203.0.113.7") && s.hostSfcoAddress == "203.0.113.7");
	CHECK(!settings::Set(s, "host_sfco_address", "http://x/") && s.hostSfcoAddress == "203.0.113.7");
	CHECK(!s.hostGameSpy && settings::Set(s, "host_gamespy", "1") && s.hostGameSpy);
	CHECK(settings::Set(s, "host_gamespy_address", "gs.example.org") && s.hostGameSpyAddress == "gs.example.org" &&
	      settings::Get(s, "host_gamespy_address") == "gs.example.org");
	CHECK(settings::Set(s, "host_adhoc", "1") && settings::Get(s, "host_adhoc") == "1");
}

TEST(settings_lists) {
	auto v = settings::SplitList("Space, Right Shift,,  Keypad 8 ");
	CHECK(v.size() == 3 && v[0] == "Space" && v[1] == "Right Shift" && v[2] == "Keypad 8");
	CHECK(settings::JoinList({"A", ",", "Return"}) == "A,Comma,Return");
}

TEST(settings_load_and_save_round_trip) {
	fs::path ini = TempIni("new.ini");
	settings::Settings s;
	s.scale = 4;
	s.firmware = "4.01";
	s.keys[Index("cross")] = "Right Ctrl";
	CHECK(settings::Save(ini, s));
	settings::Settings back;
	std::vector<std::string> warnings;
	CHECK(settings::Load(ini, back, &warnings));
	CHECK(warnings.empty());
	CHECK(back.firmware == "4.01");
	CHECK(back.scale == 4 && back.keys[Index("cross")] == "Right Ctrl" && back.keys == s.keys && back.pads == s.pads);
	settings::Settings missing;
	CHECK(!settings::Load(TempIni("missing.ini"), missing));
	std::error_code ec;
	fs::remove_all(ini.parent_path(), ec);
}

TEST(settings_save_keeps_comments_and_unknown_lines) {
	fs::path ini = TempIni("edited.ini");
	WriteTextFile(ini, "\xEF\xBB\xBF# my settings\r\nVSync = 0\r\ncustom.thing=42\r\n\r\n; note\r\nscale=1\r\nnot a setting\r\n");
	settings::Settings s;
	std::vector<std::string> warnings;
	CHECK(settings::Load(ini, s, &warnings));
	CHECK(!s.vsync && s.scale == 1);
	CHECK(warnings.size() == 2);  // custom.thing, "not a setting"
	s.scale = 3;
	CHECK(settings::Save(ini, s));
	std::string text = ReadAll(ini);
	CHECK(text.rfind("\xEF\xBB\xBF# my settings\r\nvsync=0\r\ncustom.thing=42\r\n\r\n; note\r\nscale=3\r\nnot a setting\r\n", 0) == 0);
	CHECK(text.find("key.cross=Z\r\n") != std::string::npos);  // appended
	settings::Settings again;
	CHECK(settings::Load(ini, again) && again.scale == 3 && !again.vsync);
	std::error_code ec;
	fs::remove_all(ini.parent_path(), ec);
}
