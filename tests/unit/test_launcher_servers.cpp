// SPDX-License-Identifier: GPL-2.0-or-later
#if PSPC_TEST_LAUNCHER
#include "common/json.h"
#include "common/app_paths.h"
#include "test_env.h"
#include "frontend/servers.h"
#include "net/np_client.h"
#include "test_fs_helpers.h"
#include "testing.h"
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

TEST(launcher_server_routing_preserves_other_entries) {
	fstest::TempDir temp("server_routes");
	ScopedEnv config("XDG_CONFIG_HOME", temp.Path().c_str());
	servers::Init(temp.Path());
	std::string original =
		R"({"custom":42,"games":[{"ids":["OTHER"],"domains":{"old.example":"host"}},{"ids":["TEST"],"dns":"8.8.8.8","domains":{"kept.example":"kept"}}]})";
	CHECK(WriteWholeFile(apppaths::ConfigRoot() / "infrastructure.json", original.data(), original.size()));
	std::wstring error;
	CHECK(servers::SetRedirect("Test", "TEST", "new.example", "127.0.0.1", &error));
	CHECK(servers::Redirect("TEST", "new.example") == "127.0.0.1");
	CHECK(servers::Redirect("TEST", "kept.example") == "kept");
	CHECK(servers::Redirect("OTHER", "old.example") == "host");
	CHECK(servers::SetRedirect("Test", "TEST", "new.example", "", &error));
	CHECK(servers::Redirect("TEST", "new.example").empty());
	std::vector<u8> bytes;
	CHECK(ReadWholeFile(apppaths::ConfigRoot() / "infrastructure.json", bytes));
	auto root = json::Value::Parse(std::string(bytes.begin(), bytes.end()));
	CHECK(root["custom"].Number() == 42);
	CHECK(root["games"].Array()[1]["dns"].String() == "8.8.8.8");
}
TEST(launcher_network_server_account_admin_lifecycle) {
	fstest::TempDir temp("launcher_network_server");
	ScopedEnv data("XDG_DATA_HOME", (temp.Path() / "data").c_str());
	ScopedEnv state("XDG_STATE_HOME", (temp.Path() / "state").c_str());
	CHECK(WriteTextFile(temp.Path() / "data/nids.csv", "resource-root marker"));
	std::filesystem::create_directory_symlink(
		std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "server", temp.Path() / "server");
	servers::Init(temp.Path());
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(fd >= 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(fd, (sockaddr *)&address, sizeof(address)) == 0);
	socklen_t size = sizeof(address);
	CHECK(getsockname(fd, (sockaddr *)&address, &size) == 0);
	close(fd);
	settings::Settings settings;
	settings.hostNetworkLan = false;
	settings.hostNetworkPort = ntohs(address.sin_port);
	std::wstring error;
	CHECK(servers::Start(servers::Kind::Network, settings, L"", &error));
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (servers::Poll(servers::Kind::Network) == servers::State::Starting &&
		   std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(servers::Poll(servers::Kind::Network) == servers::State::Running);
	CHECK(servers::DataDir() == apppaths::DataRoot() / "servers");
	CHECK(fs::exists(servers::DataDir() / "pspc_network.sqlite"));
	CHECK(servers::LogPath(servers::Kind::Network) == apppaths::StateRoot() / "servers/pspc_network.log");
	CHECK(!fs::exists(temp.Path() / "servers"));
	np::NetworkClient client("http://127.0.0.1:" + std::to_string(settings.hostNetworkPort));
	client.timeoutMs = 1000;
	np::Profile profile;
	std::string token;
	CHECK(client.Register("TestPlayer", "fixture-password-123", "us", &profile, &token).ok);
	CHECK(!token.empty());
	client.SetToken(token);
	CHECK(client.Me(&profile).ok);
	CHECK(profile.onlineId == "TestPlayer");
	std::vector<np::Friend> friends;
	CHECK(client.Friends(&friends).ok);
	np::NetworkClient peer(client.Server());
	CHECK(peer.Register("TestFriend", "peer-password-123", "us", nullptr, nullptr).ok);
	np::Friend friendship;
	CHECK(client.AddFriend("TestFriend", &friendship).ok);
	CHECK(friendship.status == "sent");
	CHECK(peer.AddFriend("TestPlayer", &friendship).ok);
	CHECK(friendship.status == "friend");
	CHECK(client.RemoveFriend("TestFriend").ok);
	CHECK(client.Friends(&friends).ok && friends.empty());
	auto logout = client.SignOut();
	if (!logout.ok)
		printf("Sign out: HTTP %d: %s\n", logout.httpStatus, logout.message.c_str());
	CHECK(logout.ok);
	std::string output;
	CHECK(servers::Admin(servers::Kind::Network, {L"--help"}, &output));
	CHECK(output.find("serve") != std::string::npos);
	CHECK(servers::Admin(servers::Kind::Network, {L"--json", L"accounts"}, &output));
	CHECK(json::Value::Parse(output).IsArray());
	for (auto kind : {servers::Kind::Sfco, servers::Kind::GameSpy}) {
		CHECK(servers::Admin(kind, {L"--help"}, &output));
		CHECK(output.find("serve") != std::string::npos);
	}
	CHECK(servers::Admin(servers::Kind::Network, {L"passwd", L"TestPlayer", L"--password-stdin"}, &output,
						 "new-password-123\n"));
	CHECK(client.SignIn("TestPlayer", "new-password-123", &profile, &token).ok);
	CHECK(servers::Admin(servers::Kind::Network, {L"ban", L"TestPlayer"}, &output));
	CHECK(!client.Me(&profile).ok);
	CHECK(servers::Admin(servers::Kind::Network, {L"unban", L"TestPlayer"}, &output));
	CHECK(client.SignIn("TestPlayer", "new-password-123", &profile, &token).ok);
	CHECK(client.SignOut().ok);
	servers::Stop(servers::Kind::Network);
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (servers::AnyRunning() && std::chrono::steady_clock::now() < deadline) {
		servers::Poll(servers::Kind::Network);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(!servers::AnyRunning());
	CHECK(servers::Poll(servers::Kind::Network) == servers::State::Stopped);
	servers::StopAll();
}
#endif
