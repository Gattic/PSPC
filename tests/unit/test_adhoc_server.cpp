// SPDX-License-Identifier: GPL-2.0-or-later
// pspc_net's ad hoc server (src/net/adhoc_server.*) against fake clients on loopback: login, group
// create / scan / join / leave, chat, and the protocol errors that drop a user.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "net/adhoc_proto.h"
#include "net/adhoc_server.h"
#include "net/host_net.h"
#include "testing.h"

using namespace adhocproto;

namespace {

// A blocking TCP client bound to its own loopback address (the server tells players apart by IP).
struct FakeClient {
	SOCKET s = INVALID_SOCKET;
	u32 ip = 0;

	bool Connect(const char *localIp, u16 port) {
		s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) return false;
		DWORD tmo = 3000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		hostnet::ParseIpv4(localIp, &ip);
		a.sin_addr.s_addr = ip;
		if (bind(s, (sockaddr *)&a, sizeof(a)) != 0) return false;
		sockaddr_in srv{};
		srv.sin_family = AF_INET;
		srv.sin_port = htons(port);
		srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		return connect(s, (sockaddr *)&srv, sizeof(srv)) == 0;
	}
	void Close() {
		if (s != INVALID_SOCKET) closesocket(s);
		s = INVALID_SOCKET;
	}
	~FakeClient() { Close(); }

	bool Send(const void *p, size_t n) { return send(s, (const char *)p, (int)n, 0) == (int)n; }
	bool Login(const u8 *mac, const char *name, const char *game) {
		LoginC2S p{};
		p.opcode = OPCODE_LOGIN;
		memcpy(p.mac, mac, 6);
		strncpy((char *)p.name, name, sizeof(p.name) - 1);
		memcpy(p.game, game, kProductCodeLen);
		return Send(&p, sizeof(p));
	}
	bool Group(u8 op, const char *group = nullptr) {
		if (op != OPCODE_CONNECT) return Send(&op, 1);
		ConnectC2S p{};
		p.opcode = op;
		strncpy((char *)p.group, group, kGroupNameLen);
		return Send(&p, sizeof(p));
	}
	// Reads one complete server packet (empty on time-out / close).
	std::string Read() {
		std::string out;
		char op;
		if (recv(s, &op, 1, 0) != 1) return out;
		out.push_back(op);
		size_t need = S2CPacketSize((u8)op);
		while (out.size() < need) {
			char buf[256];
			int n = recv(s, buf, (int)std::min<size_t>(sizeof(buf), need - out.size()), 0);
			if (n <= 0) return std::string();
			out.append(buf, (size_t)n);
		}
		return out;
	}
	// True if the server closed the connection (recv returns 0) within the time-out.
	bool ClosedByServer() {
		char c;
		return recv(s, &c, 1, 0) == 0;
	}
};

const u8 kMacA[6] = {0x00, 0x16, 0xFE, 0x50, 0x53, 0x43};
const u8 kMacB[6] = {0x00, 0x16, 0xFE, 0x50, 0x53, 0x44};

}  // namespace

TEST(adhoc_server_groups) {
	CHECK(hostnet::Startup());
	adhocserver::Server server;
	adhocserver::Options opt;
	opt.bindAddress = "127.0.0.1";
	opt.port = 0;
	opt.log = [](const std::string &) {};
	std::string err;
	CHECK(server.Start(opt, &err));
	u16 port = server.Port();
	CHECK(port != 0);

	FakeClient a, b;
	CHECK(a.Connect("127.0.0.1", port));
	CHECK(b.Connect("127.0.0.2", port));
	CHECK(a.Login(kMacA, "Alice", "ULUS10000"));
	CHECK(b.Login(kMacB, "Bob", "ULUS10000"));

	// A creates GROUP1: it is alone, so the BSSID is its own MAC.
	CHECK(a.Group(OPCODE_CONNECT, "GROUP1"));
	std::string p = a.Read();
	CHECK(p.size() == sizeof(ConnectBssidS2C) && (u8)p[0] == OPCODE_CONNECT_BSSID && !memcmp(p.data() + 1, kMacA, 6));

	// B scans: one group, hosted by A.
	CHECK(b.Group(OPCODE_SCAN));
	p = b.Read();
	CHECK(p.size() == sizeof(ScanS2C) && (u8)p[0] == OPCODE_SCAN);
	if (p.size() == sizeof(ScanS2C)) {
		ScanS2C s;
		memcpy(&s, p.data(), sizeof(s));
		CHECK(!strncmp((const char *)s.group, "GROUP1", 8));
		CHECK(!memcmp(s.mac, kMacA, 6));
	}
	p = b.Read();
	CHECK(p.size() == 1 && (u8)p[0] == OPCODE_SCAN_COMPLETE);

	// B joins: B hears about A (CONNECT, then the BSSID of the founder A), A hears about B.
	CHECK(b.Group(OPCODE_CONNECT, "GROUP1"));
	p = b.Read();
	CHECK(p.size() == sizeof(ConnectS2C) && (u8)p[0] == OPCODE_CONNECT);
	if (p.size() == sizeof(ConnectS2C)) {
		ConnectS2C c;
		memcpy(&c, p.data(), sizeof(c));
		CHECK(!memcmp(c.mac, kMacA, 6));
		CHECK(!strcmp((const char *)c.name, "Alice"));
		CHECK_EQ(c.ip, a.ip);
	}
	p = b.Read();
	CHECK(p.size() == sizeof(ConnectBssidS2C) && (u8)p[0] == OPCODE_CONNECT_BSSID && !memcmp(p.data() + 1, kMacA, 6));
	p = a.Read();
	CHECK(p.size() == sizeof(ConnectS2C) && (u8)p[0] == OPCODE_CONNECT);
	if (p.size() == sizeof(ConnectS2C)) {
		ConnectS2C c;
		memcpy(&c, p.data(), sizeof(c));
		CHECK(!memcmp(c.mac, kMacB, 6));
		CHECK(!strcmp((const char *)c.name, "Bob"));
		CHECK_EQ(c.ip, b.ip);
	}

	// Chat goes to the other members, with the sender's name.
	ChatC2S chat{};
	chat.opcode = OPCODE_CHAT;
	strcpy(chat.message, "hello");
	CHECK(a.Send(&chat, sizeof(chat)));
	p = b.Read();
	CHECK(p.size() == sizeof(ChatS2C) && (u8)p[0] == OPCODE_CHAT);
	if (p.size() == sizeof(ChatS2C)) {
		ChatS2C c;
		memcpy(&c, p.data(), sizeof(c));
		CHECK(!strcmp(c.message, "hello") && !strcmp((const char *)c.name, "Alice"));
	}

	// Pings are accepted silently. B leaves: A gets DISCONNECT with B's IP.
	u8 ping = OPCODE_PING;
	CHECK(b.Send(&ping, 1));
	CHECK(b.Group(OPCODE_DISCONNECT));
	p = a.Read();
	CHECK(p.size() == sizeof(DisconnectS2C) && (u8)p[0] == OPCODE_DISCONNECT);
	if (p.size() == sizeof(DisconnectS2C)) {
		DisconnectS2C d;
		memcpy(&d, p.data(), sizeof(d));
		CHECK_EQ(d.ip, b.ip);
	}
	CHECK_EQ(server.GroupCount(), 1);

	// Leaving a group one isn't in drops the connection (PPSSPP's server does the same).
	CHECK(b.Group(OPCODE_DISCONNECT));
	CHECK(b.ClosedByServer());

	// A second connection from an IP already logged in is refused.
	FakeClient dup;
	CHECK(dup.Connect("127.0.0.1", port));
	CHECK(dup.ClosedByServer());

	// A login with a bad product code is refused.
	FakeClient bad;
	CHECK(bad.Connect("127.0.0.3", port));
	CHECK(bad.Login(kMacB, "Bad", "ulus-1000"));
	CHECK(bad.ClosedByServer());

	// A disconnect from the server (connection closed): the group goes away with its last member.
	a.Close();
	for (int i = 0; i < 100 && server.GroupCount() != 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK_EQ(server.GroupCount(), 0);
	server.Stop();
}
