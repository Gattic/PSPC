// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _WIN32
#include "common/app_paths.h"
#include "common/settings.h"
#include "test_env.h"
#include "test_fs_helpers.h"
#include "testing.h"

TEST(app_paths_xdg_roots_and_settings) {
	fstest::TempDir temp("xdg_paths");
	ScopedEnv home("HOME", nullptr);
	ScopedEnv config("XDG_CONFIG_HOME", (temp.Path() / "config λ").c_str());
	ScopedEnv data("XDG_DATA_HOME", (temp.Path() / "data λ").c_str());
	ScopedEnv cache("XDG_CACHE_HOME", (temp.Path() / "cache λ").c_str());
	ScopedEnv state("XDG_STATE_HOME", (temp.Path() / "state λ").c_str());
	CHECK(apppaths::ConfigRoot() == temp.Path() / "config λ/pspc");
	CHECK(apppaths::DataRoot() == temp.Path() / "data λ/pspc");
	CHECK(apppaths::CacheRoot() == temp.Path() / "cache λ/pspc");
	CHECK(apppaths::StateRoot() == temp.Path() / "state λ/pspc");
	CHECK(apppaths::DataPath("relative games") == apppaths::DataRoot() / "relative games");
	CHECK(apppaths::DataPath((temp.Path() / "absolute games").string()) == temp.Path() / "absolute games");
	CHECK(settings::DefaultPath() == apppaths::ConfigRoot() / "pspc.ini");
	settings::Settings value;
	value.npOnlineId = "Isolated";
	CHECK(settings::Save(settings::DefaultPath(), value));
	settings::Settings loaded;
	CHECK(settings::Load(settings::DefaultPath(), loaded));
	CHECK(loaded.npOnlineId == value.npOnlineId);
}
TEST(app_paths_xdg_defaults_and_invalid_roots) {
	fstest::TempDir temp("xdg_defaults");
	ScopedEnv home("HOME", temp.Path().c_str());
	ScopedEnv config("XDG_CONFIG_HOME", "");
	ScopedEnv data("XDG_DATA_HOME", "relative");
	ScopedEnv cache("XDG_CACHE_HOME", nullptr);
	ScopedEnv state("XDG_STATE_HOME", "relative");
	CHECK(apppaths::ConfigRoot() == temp.Path() / ".config/pspc");
	CHECK(apppaths::DataRoot() == temp.Path() / ".local/share/pspc");
	CHECK(apppaths::CacheRoot() == temp.Path() / ".cache/pspc");
	CHECK(apppaths::StateRoot() == temp.Path() / ".local/state/pspc");
	ScopedEnv noHome("HOME", "relative-home");
	bool rejected = false;
	try { apppaths::ConfigRoot(); }
	catch (const std::runtime_error &) { rejected = true; }
	CHECK(rejected);
}
TEST(app_paths_relocated_resource_root) {
	fstest::TempDir temp("resource_root");
	CHECK(WriteTextFile(temp.Path() / "data/nids.csv", "ancestor"));
	auto installed = temp.Path() / "relocated λ";
	CHECK(WriteTextFile(installed / "data/nids.csv", "installed"));
	CHECK(apppaths::ResourceRoot(installed) == installed);
	CHECK(apppaths::ResourceRoot(installed / "build/config/bin") == installed);
	CHECK(!fs::exists(installed / "pspc.ini"));
}
#endif
