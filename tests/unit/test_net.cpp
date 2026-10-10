// SPDX-License-Identifier: GPL-2.0-or-later
// pspc_net (src/net/host_net.*): conversions, and the socket layer and poller against loopback.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "net/host_net.h"
#include "testing.h"

using namespace hostnet;

namespace {

// Waits (on this test thread) until one of the events fires, like a guest thread would through
// net::WaitHostEvents. Returns false on timeout.
bool WaitFor(const WSAEVENT *evs, size_t n, int timeoutMs) {
	std::mutex mu;
	std::condition_variable cv;
	bool fired = false;
	WatchId id = Watch(evs, n, [&] {
		std::lock_guard<std::mutex> l(mu);
		fired = true;
		cv.notify_all();
	});
	std::unique_lock<std::mutex> l(mu);
	bool ok = cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return fired; });
	l.unlock();
	Unwatch(id);
	return ok;
}

sockaddr_in Loopback(u16 port) {
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

u16 PortOf(SOCKET s) {
	sockaddr_in a{};
	int len = sizeof(a);
	getsockname(s, (sockaddr *)&a, &len);
	return ntohs(a.sin_port);
}

}  // namespace

TEST(net_errno_mapping) {
	CHECK_EQ(PspErrnoFromWsa(0), 0);
	CHECK_EQ(PspErrnoFromWsa(WSAEWOULDBLOCK), (int)PSP_EAGAIN);
	CHECK_EQ(PspErrnoFromWsa(WSAEWOULDBLOCK, true), (int)PSP_EINPROGRESS);
	CHECK_EQ(PspErrnoFromWsa(WSAECONNREFUSED), (int)PSP_ECONNREFUSED);
	CHECK_EQ(PspErrnoFromWsa(WSAECONNRESET), (int)PSP_ECONNRESET);
	CHECK_EQ(PspErrnoFromWsa(WSAEADDRINUSE), (int)PSP_EADDRINUSE);
	CHECK_EQ(PspErrnoFromWsa(WSAETIMEDOUT), (int)PSP_ETIMEDOUT);
	CHECK_EQ(PspErrnoFromWsa(WSAENOTCONN), (int)PSP_ENOTCONN);
	CHECK_EQ(PspErrnoFromWsa(WSAEISCONN), (int)PSP_EISCONN);
	CHECK_EQ(PspErrnoFromWsa(WSAEALREADY), (int)PSP_EALREADY);
	CHECK(strcmp(PspErrnoName(PSP_ECONNREFUSED), "ECONNREFUSED") == 0);
	// The PSP numbers themselves (newlib), which games compare against.
	CHECK_EQ((int)PSP_EAGAIN, 11);
	CHECK_EQ((int)PSP_EINPROGRESS, 119);
	CHECK_EQ((int)PSP_ECONNREFUSED, 111);
}

TEST(net_address_helpers) {
	u32 a = 0;
	CHECK(ParseIpv4("192.168.1.20", &a));
	CHECK_EQ(a, 0x1401A8C0u);
	CHECK(Ipv4ToString(a) == "192.168.1.20");
	CHECK(!ParseIpv4("192.168.1", &a));
	CHECK(!ParseIpv4("1.2.3.256", &a));
	CHECK(!ParseIpv4("1.2.3.4x", &a));
	CHECK(!ParseIpv4("", &a));
	CHECK(IsLoopback(htonl(0x7F000002)));
	CHECK(!IsLoopback(htonl(0x0A000001)));

	PspSockaddrIn p{};
	p.len = 16;
	p.family = PSP_AF_INET;
	p.port = htons(8080);
	p.addr = htonl(0x7F000001);
	sockaddr_in h = ToHost(p);
	CHECK_EQ(h.sin_family, AF_INET);
	CHECK_EQ(ntohs(h.sin_port), 8080);
	CHECK_EQ(h.sin_addr.s_addr, htonl(0x7F000001));
	PspSockaddrIn back = ToPsp(h);
	CHECK_EQ(back.len, 16);
	CHECK_EQ(back.family, PSP_AF_INET);
	CHECK_EQ(back.port, p.port);
	CHECK_EQ(back.addr, p.addr);
}

TEST(net_sockopt_mapping) {
	int l, n;
	CHECK(MapSockopt(PSP_SOL_SOCKET, PSP_SO_REUSEADDR, &l, &n));
	CHECK_EQ(l, SOL_SOCKET);
	CHECK_EQ(n, SO_REUSEADDR);
	CHECK(MapSockopt(PSP_SOL_SOCKET, PSP_SO_BROADCAST, &l, &n));
	CHECK_EQ(n, SO_BROADCAST);
	CHECK(MapSockopt(PSP_IPPROTO_TCP, PSP_TCP_NODELAY, &l, &n));
	CHECK_EQ(l, IPPROTO_TCP);
	CHECK_EQ(n, TCP_NODELAY);
	CHECK(MapSockopt(PSP_IPPROTO_IP, PSP_IP_ADD_MEMBERSHIP, &l, &n));
	CHECK_EQ(n, IP_ADD_MEMBERSHIP);
	// Emulated by the HLE layer, or BSD-only: no host option.
	CHECK(!MapSockopt(PSP_SOL_SOCKET, PSP_SO_NBIO, &l, &n));
	CHECK(!MapSockopt(PSP_SOL_SOCKET, PSP_SO_REUSEPORT, &l, &n));
	CHECK_EQ(MapMsgFlags(PSP_MSG_PEEK | PSP_MSG_DONTWAIT | PSP_MSG_WAITALL), MSG_PEEK);
}

TEST(net_tcp_loopback) {
	CHECK(Startup());
	int err = 0;
	Socket server = OpenSocket(AF_INET, SOCK_STREAM, 0, &err);
	CHECK(server.Valid());
	sockaddr_in any = Loopback(0);
	CHECK_EQ(bind(server.s, (sockaddr *)&any, sizeof(any)), 0);
	CHECK_EQ(listen(server.s, 4), 0);
	u16 port = PortOf(server.s);
	CHECK(port != 0);

	// Nothing to accept yet: the socket is non-blocking.
	CHECK(accept(server.s, nullptr, nullptr) == INVALID_SOCKET);
	CHECK_EQ(WSAGetLastError(), WSAEWOULDBLOCK);

	Socket client = OpenSocket(AF_INET, SOCK_STREAM, 0, &err);
	CHECK(client.Valid());
	sockaddr_in to = Loopback(port);
	int rc = connect(client.s, (sockaddr *)&to, sizeof(to));
	CHECK(rc == 0 || WSAGetLastError() == WSAEWOULDBLOCK);

	// The listening socket's event fires for the incoming connection.
	CHECK(WaitFor(&server.ev, 1, 2000));
	Readiness r;
	CHECK(CheckReady(server.s, &r));
	CHECK(r.read);
	SOCKET acc = accept(server.s, nullptr, nullptr);
	CHECK(acc != INVALID_SOCKET);
	Socket peer;
	CHECK(AdoptSocket(acc, &peer, &err));
	CHECK(peer.ev != server.ev);

	// The connect completes: writable, no pending error.
	for (int i = 0; i < 100; i++) {
		CHECK(CheckReady(client.s, &r));
		if (r.write || r.except) break;
		WaitFor(&client.ev, 1, 20);
	}
	CHECK(r.write);
	CHECK(!r.except);
	CHECK_EQ(PendingError(client.s), 0);

	// Data goes through; the receiving side's event fires and recv gets it.
	const char msg[] = "hello psp";
	CHECK_EQ(send(client.s, msg, sizeof(msg), 0), (int)sizeof(msg));
	CHECK(WaitFor(&peer.ev, 1, 2000) || BytesAvailable(peer.s) > 0);
	char buf[64] = {};
	int got = 0;
	for (int i = 0; i < 100 && got < (int)sizeof(msg); i++) {
		int n = recv(peer.s, buf + got, (int)sizeof(buf) - got, 0);
		if (n > 0) got += n;
		else WaitFor(&peer.ev, 1, 20);
	}
	CHECK_EQ(got, (int)sizeof(msg));
	CHECK(strcmp(buf, msg) == 0);

	// Nothing more: recv would block.
	CHECK_EQ(recv(peer.s, buf, sizeof(buf), 0), -1);
	CHECK_EQ(WSAGetLastError(), WSAEWOULDBLOCK);

	// The client closes: the peer becomes readable and reads end of stream.
	CloseSocket(client);
	bool eof = false;
	for (int i = 0; i < 100 && !eof; i++) {
		CHECK(CheckReady(peer.s, &r));
		if (r.read) eof = recv(peer.s, buf, sizeof(buf), 0) == 0;
		else WaitFor(&peer.ev, 1, 20);
	}
	CHECK(eof);
	CloseSocket(peer);
	CloseSocket(server);
}

TEST(net_tcp_refused) {
	int err = 0;
	// A port nobody listens on: bind one, learn its number, close it.
	Socket probe = OpenSocket(AF_INET, SOCK_STREAM, 0, &err);
	sockaddr_in any = Loopback(0);
	bind(probe.s, (sockaddr *)&any, sizeof(any));
	u16 port = PortOf(probe.s);
	CloseSocket(probe);

	Socket client = OpenSocket(AF_INET, SOCK_STREAM, 0, &err);
	sockaddr_in to = Loopback(port);
	int rc = connect(client.s, (sockaddr *)&to, sizeof(to));
	CHECK(rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK);
	// Windows retries a refused loopback connect for about two seconds before reporting it.
	Readiness r;
	bool failed = false;
	for (int i = 0; i < 400 && !failed; i++) {
		CHECK(CheckReady(client.s, &r));
		if (r.except) failed = true;
		else WaitFor(&client.ev, 1, 20);
	}
	CHECK(failed);
	CHECK_EQ(PspErrnoFromWsa(PendingError(client.s)), (int)PSP_ECONNREFUSED);
	CloseSocket(client);
}

TEST(net_udp_loopback) {
	int err = 0;
	Socket a = OpenSocket(AF_INET, SOCK_DGRAM, 0, &err);
	Socket b = OpenSocket(AF_INET, SOCK_DGRAM, 0, &err);
	CHECK(a.Valid() && b.Valid());
	sockaddr_in la = Loopback(0), lb = Loopback(0);
	CHECK_EQ(bind(a.s, (sockaddr *)&la, sizeof(la)), 0);
	CHECK_EQ(bind(b.s, (sockaddr *)&lb, sizeof(lb)), 0);
	sockaddr_in toB = Loopback(PortOf(b.s));

	// Nothing yet.
	char buf[2048];
	CHECK_EQ(recvfrom(b.s, buf, sizeof(buf), 0, nullptr, nullptr), -1);
	CHECK_EQ(WSAGetLastError(), WSAEWOULDBLOCK);

	const char msg[] = "datagram";
	CHECK_EQ(sendto(a.s, msg, sizeof(msg), 0, (sockaddr *)&toB, sizeof(toB)), (int)sizeof(msg));
	CHECK(WaitFor(&b.ev, 1, 2000) || BytesAvailable(b.s) > 0);
	sockaddr_in from{};
	int fromLen = sizeof(from);
	CHECK_EQ(recvfrom(b.s, buf, sizeof(buf), 0, (sockaddr *)&from, &fromLen), (int)sizeof(msg));
	CHECK(strcmp(buf, msg) == 0);
	CHECK_EQ(ntohs(from.sin_port), PortOf(a.s));

	// A datagram to a closed port must not make the next receive fail (no WSAECONNRESET).
	Socket gone = OpenSocket(AF_INET, SOCK_DGRAM, 0, &err);
	sockaddr_in lg = Loopback(0);
	bind(gone.s, (sockaddr *)&lg, sizeof(lg));
	sockaddr_in toGone = Loopback(PortOf(gone.s));
	CloseSocket(gone);
	sendto(a.s, msg, sizeof(msg), 0, (sockaddr *)&toGone, sizeof(toGone));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	int n = recvfrom(a.s, buf, sizeof(buf), 0, nullptr, nullptr);
	CHECK(n == -1 && WSAGetLastError() == WSAEWOULDBLOCK);

	CloseSocket(a);
	CloseSocket(b);
}

TEST(net_poller_watch_semantics) {
	int err = 0;
	Socket a = OpenSocket(AF_INET, SOCK_DGRAM, 0, &err);
	Socket b = OpenSocket(AF_INET, SOCK_DGRAM, 0, &err);
	sockaddr_in la = Loopback(0), lb = Loopback(0);
	bind(a.s, (sockaddr *)&la, sizeof(la));
	bind(b.s, (sockaddr *)&lb, sizeof(lb));
	sockaddr_in toB = Loopback(PortOf(b.s));

	// Drain the initial FD_WRITE so b's event is quiet.
	WaitFor(&b.ev, 1, 50);
	// An unwatched watch never fires.
	std::atomic<int> fired{0};
	WatchId id = Watch(&b.ev, 1, [&] { fired++; });
	Unwatch(id);
	sendto(a.s, "x", 1, 0, (sockaddr *)&toB, sizeof(toB));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK_EQ(fired.load(), 0);

	// A watch on several events fires once, for whichever is signalled.
	WSAEVENT both[2] = {a.ev, b.ev};
	std::mutex mu;
	std::condition_variable cv;
	int count = 0;
	Watch(both, 2, [&] {
		std::lock_guard<std::mutex> l(mu);
		count++;
		cv.notify_all();
	});
	{
		std::unique_lock<std::mutex> l(mu);
		cv.wait_for(l, std::chrono::milliseconds(1000), [&] { return count > 0; });
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK_EQ(count, 1);

	// Watching nothing returns an id that never fires.
	WatchId none = Watch(nullptr, 0, [&] { fired++; });
	CHECK(none != 0);

	// Releasing an event cancels the watches that use it.
	char buf[8];
	recvfrom(b.s, buf, sizeof(buf), 0, nullptr, nullptr);
	Watch(&b.ev, 1, [&] { fired++; });
	CloseSocket(b);
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	CHECK_EQ(fired.load(), 0);
	CloseSocket(a);
}

TEST(net_host_config_and_resolver) {
	HostConfig cfg = QueryHostConfig();
	CHECK(cfg.ip != 0);
	CHECK(cfg.netmask != 0);
	if (cfg.valid) {
		CHECK(!IsLoopback(cfg.ip));
		printf("  host network: %s ip %s mask %s gw %s dns %s\n", cfg.adapter.c_str(), Ipv4ToString(cfg.ip).c_str(),
		       Ipv4ToString(cfg.netmask).c_str(), Ipv4ToString(cfg.gateway).c_str(), Ipv4ToString(cfg.dns[0]).c_str());
	}
	u32 a = 0;
	CHECK_EQ(ResolveIpv4("localhost", &a), 0);
	CHECK(IsLoopback(a));
	CHECK(ResolveIpv4("this-name-does-not-exist.invalid", &a) != 0);
}
