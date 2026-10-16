// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/firmware.h"
#include "testing.h"
#include <chrono>

namespace {
struct Fixture {
	fs::path root = fs::temp_directory_path() / ("pspc-firmware-test-" +
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Fixture() { fs::create_directories(root); }
	~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
	void Put(const fs::path &file, const std::string &text = "sentinel") {
		fs::create_directories(file.parent_path());
		CHECK(WriteTextFile(file, text));
	}
	void Install(const std::string &name) {
		auto profile = firmware::Profile(root, name);
		Put(profile / "flash0/vsh/module/vshmain.prx");
		Put(profile / "flash1/registry/system.dreg", name);
	}
};
template <class F> bool Fails(F action) {
	try { action(); } catch (const std::exception &) { return true; }
	return false;
}
}

TEST(firmware_names_and_catalog) {
	Fixture f;
	for (auto name : {"4.01", "4.01-m33", "default", "test_6.61"}) CHECK(firmware::ValidName(name));
	for (auto name : {"", ".", "..", "../outside", "/tmp", "C:\\data", "a/b", "a\\b", "a\nb", "a ", "CON", "nul.txt", "com1", "lpt9.bin", "-x", "x."})
		CHECK(!firmware::ValidName(name));
	CHECK(!firmware::ValidName(std::string(65, 'a')));
	CHECK(firmware::List(f.root).empty());
	f.Install("6.61");
	f.Install("4.01");
	f.Put(f.root / "firmware/.building-test/flash0/vsh/module/vshmain.prx");
	fs::create_directories(f.root / "firmware/incomplete/flash1");
	CHECK((firmware::List(f.root) == std::vector<std::string>{"4.01", "6.61"}));
	CHECK(Fails([&] { firmware::Profile(f.root, "../outside"); }));
}

TEST(firmware_select_persists_and_delete_is_scoped) {
	Fixture f;
	f.Install("4.01");
	f.Install("6.61");
	f.Put(f.root / "ms0/PSP/SAVEDATA/keep");
	f.Put(f.root / "cache/aot/shared.so");
	settings::Settings config;
	config.fw = "5.50";
	auto ini = f.root / "pspc.ini";
	CHECK(settings::Save(ini, config));
	firmware::Select(f.root, ini, config, "4.01");
	settings::Settings loaded;
	CHECK(settings::Load(ini, loaded));
	CHECK(loaded.firmware == "4.01" && loaded.fw == "5.50");
	CHECK(Fails([&] { firmware::Select(f.root, ini, config, "missing"); }));
	CHECK(config.firmware == "4.01");
	CHECK(Fails([&] { firmware::Delete(f.root, config.firmware, "4.01"); }));
	CHECK(Fails([&] { firmware::Delete(f.root, config.firmware, "../ms0"); }));
	firmware::Select(f.root, ini, config, "6.61");
	firmware::Delete(f.root, config.firmware, "4.01");
	CHECK(!fs::exists(f.root / "firmware/4.01"));
	CHECK(fs::is_regular_file(f.root / "firmware/6.61/flash1/registry/system.dreg"));
	CHECK(fs::is_regular_file(f.root / "ms0/PSP/SAVEDATA/keep"));
	CHECK(fs::is_regular_file(f.root / "cache/aot/shared.so"));
	// A failed settings write must not change the in-memory active profile.
	fs::create_directory(f.root / "not-an-ini");
	f.Install("4.01");
	CHECK(Fails([&] { firmware::Select(f.root, f.root / "not-an-ini", config, "4.01"); }));
	CHECK(config.firmware == "6.61");
}

TEST(firmware_migration_preserves_existing_trees) {
	Fixture f;
	f.Put(f.root / "flash0/vsh/module/vshmain.prx");
	f.Put(f.root / "flash1/registry/system.dreg");
	f.Put(f.root / "ms0/save");
	firmware::Migrate(f.root);
	firmware::Migrate(f.root);
	CHECK(!fs::exists(f.root / "flash0") && !fs::exists(f.root / "flash1"));
	CHECK(firmware::Installed(f.root, "default"));
	CHECK(fs::is_regular_file(f.root / "firmware/default/flash1/registry/system.dreg"));
	CHECK(fs::is_regular_file(f.root / "ms0/save"));
	f.Put(f.root / "flash0/keep");
	CHECK(Fails([&] { firmware::Migrate(f.root); }));
	CHECK(fs::is_regular_file(f.root / "flash0/keep"));
}

TEST(firmware_partial_migration_preserves_overridden_roots) {
	for (bool migrate0 : {false, true}) {
		Fixture f;
		f.Put(f.root / "flash0/keep");
		f.Put(f.root / "flash1/keep");
		firmware::Migrate(f.root, migrate0, !migrate0);
		const char *moved = migrate0 ? "flash0" : "flash1";
		const char *untouched = migrate0 ? "flash1" : "flash0";
		CHECK(fs::is_regular_file(f.root / "firmware/default" / moved / "keep"));
		CHECK(fs::is_regular_file(f.root / untouched / "keep"));
		CHECK(!fs::exists(f.root / moved));
		CHECK(!fs::exists(f.root / "firmware/default" / untouched));
		firmware::Migrate(f.root);
		CHECK(fs::is_regular_file(f.root / "firmware/default" / untouched / "keep"));
		CHECK(!fs::exists(f.root / untouched));
	}
}

TEST(firmware_migration_does_not_inspect_overridden_roots) {
	Fixture f;
	f.Put(f.root / "flash0/keep");
	f.Put(f.root / "firmware/default/flash0/other"); // conflict belongs to an explicit root
	f.Put(f.root / "flash1/keep");
	firmware::Migrate(f.root, false, true);
	CHECK(fs::is_regular_file(f.root / "flash0/keep"));
	CHECK(fs::is_regular_file(f.root / "firmware/default/flash1/keep"));
	firmware::Migrate(f.root, false, false);
	CHECK(fs::is_regular_file(f.root / "flash0/keep"));
}

TEST(firmware_migration_drops_empty_folders) {
	Fixture f;
	f.Install("default");
	// A run given the old root as --flash0 left an empty folder next to the migrated profile.
	fs::create_directories(f.root / "flash0");
	firmware::Migrate(f.root);
	CHECK(!fs::exists(f.root / "flash0"));
	CHECK(firmware::Installed(f.root, "default"));
	// An empty profile folder doesn't keep the old tree from moving in.
	fs::remove_all(f.root / "firmware/default/flash1");
	fs::create_directories(f.root / "firmware/default/flash1");
	f.Put(f.root / "flash1/registry/system.dreg", "old");
	firmware::Migrate(f.root);
	CHECK(!fs::exists(f.root / "flash1"));
	CHECK(fs::is_regular_file(f.root / "firmware/default/flash1/registry/system.dreg"));
}

#ifndef _WIN32
TEST(firmware_symlinks_cannot_redirect_deletion) {
	Fixture f;
	f.Install("4.01");
	f.Put(f.root / "outside/keep");
	fs::create_directory_symlink(f.root / "outside", f.root / "firmware/linked");
	CHECK(Fails([&] { firmware::Delete(f.root, "default", "linked"); }));
	fs::create_directory_symlink(f.root / "outside", f.root / "firmware/4.01/external");
	firmware::Delete(f.root, "default", "4.01");
	CHECK(fs::is_regular_file(f.root / "outside/keep"));
	fs::create_directory(f.root / "other");
	fs::create_directory_symlink(f.root / "outside", f.root / "other/firmware");
	CHECK(Fails([&] { firmware::Profile(f.root / "other", "4.01"); }));
}
#endif
