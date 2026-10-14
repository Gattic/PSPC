// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "common/json.h"
#include "frontend/native_cache.h"
#include "test_fs_helpers.h"
#include "testing.h"
#include <chrono>

TEST(native_cache_platform_stamp_hints_and_moved_image) {
	fstest::TempDir temp("native_cache");
	auto dir = temp.Path();
	frontend::GameInfo game;
	game.path = dir / std::filesystem::path(u8"moved λ.iso");
	game.id = "TEST00001";
	game.size = 123;
	const std::string sha(40, 'a');
	auto put = [&](const std::filesystem::path &p, const std::string &text) {
		CHECK(WriteWholeFile(p, text.data(), text.size()));
	};
	using frontend::Native;
	auto inspect = [&] { return frontend::InspectNativeCache(dir, game, 0x12345678); };
	CHECK(inspect().state == Native::None);
	std::string module = "{\"ok\":true,\"sha1\":" + json::Quote(sha) + "}";
	auto index = [&](const std::string &extra) {
		put(dir / "games.json",
			"{\"version\":1,\"games\":{\"/old/name.iso\":{\"disc_id\":\"TEST00001\",\"size\":123,\"modules\":[" +
				module + "," + module + extra + "]}}}");
	};
	index("");
	CHECK(inspect().total == 1);
	put(dir / (sha + hostsys::ModuleSuffix), "synthetic module placeholder");
	put(dir / (sha + ".semver"), "0x11111111\n");
	CHECK(inspect().state == Native::Stale);
	put(dir / (sha + ".semver"), "0x12345678\n");
	CHECK(inspect().state == Native::Built);
	put(dir / (sha + ".hints"), "new entry points");
	std::filesystem::last_write_time(dir / (sha + ".hints"),
									 std::filesystem::last_write_time(dir / (sha + hostsys::ModuleSuffix)) +
										 std::chrono::seconds(1));
	CHECK(inspect().state == Native::Stale);
	std::filesystem::remove(dir / (sha + ".hints"));
	index(",{\"ok\":false}");
	CHECK(inspect().state == Native::Partial);
	CHECK(inspect().failed == 1);
	game.size++;
	CHECK(inspect().state == Native::None);
	put(dir / "games.json", "not JSON");
	CHECK(!inspect().error.empty());
}
